#!/usr/bin/env python3
"""E2E test oznámení o restartu emulátoru přes skutečný MCP protokol (stdio).

Spustí ``mcp-server/mcp_server.py`` pod venv interpretem (FastMCP) jako MCP
server a mluví s ním JSON-RPC přes stdin/stdout jako MCP klient (stejně jako
``test_mcp_server_stdio.py``). Místo mz800emu je jako ``MZ800EMU_EXE``
podstrčen falešný emulátor (dávkový soubor / shell skript spouštějící malý
Python skript se stejným JSONL protokolem), takže test je deterministický
a nesahá na konfiguraci skutečného emulátoru. Ověřuje:

  1. ``emu_ping`` projde (pong),
  2. emulátor během ``emu_ping`` skončí (exit 3) -> tools/call vrátí chybu
     (isError) s "emulator process exited (exit code 3)" a hned, ne po 30 s,
  3. ``emu_status`` hlásí ``connected: false`` a ``last_exit``,
  4. další ``emu_ping`` spustí nový emulátor a výsledek nese
     ``restarted: true`` + ``restart_reason``,
  5. následující ``emu_ping`` už ``restarted`` nenese.

Pád se vyvolá souborem-příznakem (falešný emulátor skončí na ping, když
příznak existuje). Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí venv s balíčkem mcp).
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
import queue
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů

SKIP_EXIT = 77

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_MCP_DIR = _REPO_ROOT / "mcp-server"
_MCP_SERVER_PY = _MCP_DIR / "mcp_server.py"

#: Falešný emulátor; FAKE_EMU_CRASH_FLAG = cesta k souboru-příznaku.
FAKE_EMU = r'''
import json, os, sys
flag = os.environ.get("FAKE_EMU_CRASH_FLAG", "")
print("fake emulator banner", flush=True)
print(json.dumps({"type": "hello", "commands": ["ping", "get_state"]}), flush=True)
for line in sys.stdin:
    try:
        req = json.loads(line)
    except ValueError:
        continue
    cmd, rid = req.get("cmd"), req.get("req_id")
    if cmd == "ping" and flag and os.path.exists(flag):
        sys.stdout.flush()
        os._exit(3)
    data = {"running": True} if cmd == "get_state" else {"pong": True, "pid": os.getpid()}
    print(json.dumps({"req_id": rid, "success": True, "data": data}), flush=True)
    if cmd in ("shutdown", "emu_stop"):
        sys.stdout.flush()
        os._exit(0)
'''


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva anglicky)."""


def _find_venv_python():
    """Venv interpret MCP serveru (MZ_MCP_VENV_PY, .venv), jinak None."""
    env = os.environ.get("MZ_MCP_VENV_PY")
    if env and Path(env).is_file():
        return Path(env)
    for c in (_MCP_DIR / ".venv" / "Scripts" / "python.exe",
              _MCP_DIR / ".venv" / "bin" / "python",
              _MCP_DIR / ".venv" / "bin" / "python3"):
        if c.is_file():
            return c
    return None


def _make_fake_exe(tmp):
    """Vytvoří spustitelný falešný emulátor (Windows .bat, jinak sh skript)."""
    script = tmp / "fake_emu.py"
    script.write_text(FAKE_EMU, encoding="utf-8")
    if sys.platform == "win32":
        exe = tmp / "fake_emu.bat"
        exe.write_text(f'@"{sys.executable}" "{script}"\r\n', encoding="utf-8")
    else:
        exe = tmp / "fake_emu.sh"
        exe.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}"\n',
                       encoding="utf-8")
        exe.chmod(0o755)
    return exe


class McpClient:
    """Minimální MCP klient nad stdio JSON-RPC (newline-delimited)."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, py, env):
        self.proc = subprocess.Popen([str(py), str(_MCP_SERVER_PY)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", env=env)
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0

    def _pump(self):
        for line in iter(self.proc.stdout.readline, ""):
            self.q.put(line)
        self.q.put(None)

    def _send(self, msg):
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def request(self, method, params=None, timeout=20.0):
        """Pošle JSON-RPC request a vrátí jeho result (nebo dict s error)."""
        self.rid += 1
        rid = self.rid
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                break
            if line is None:
                raise TestFailure("MCP server closed stdout")
            try:
                resp = json.loads(line)
            except ValueError:
                continue
            if resp.get("id") == rid:
                if "error" in resp:
                    return {"_rpc_error": resp["error"]}
                return resp.get("result", {})
        raise TestFailure(f"no JSON-RPC response to {method}")

    def notify(self, method):
        self._send({"jsonrpc": "2.0", "method": method})

    def call_tool(self, name, args=None, timeout=20.0):
        """tools/call; vrátí (is_error, text) z prvního text contentu."""
        res = self.request("tools/call", {"name": name, "arguments": args or {}},
                           timeout=timeout)
        if "_rpc_error" in res:
            return True, json.dumps(res["_rpc_error"])
        texts = [c.get("text", "") for c in res.get("content", [])
                 if c.get("type") == "text"]
        return bool(res.get("isError")), (texts[0] if texts else "")

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def run(client, flag):
    init = client.request("initialize", {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "test_mcp_server_restart_stdio", "version": "1.0"}})
    check("serverInfo" in init, "initialize handshake")
    client.notify("notifications/initialized")

    err, text = client.call_tool("emu_ping")
    first = json.loads(text) if not err else {}
    check(not err and first.get("pong") is True, "emu_ping returns pong")
    check("restarted" not in first, "first call is not a restart")

    flag.write_text("crash", encoding="utf-8")
    t0 = time.monotonic()
    err, text = client.call_tool("emu_ping", timeout=40.0)
    took = time.monotonic() - t0
    flag.unlink()
    check(err, "emu_ping during emulator exit is reported as an error")
    check("emulator process exited (exit code 3)" in text,
          f"error names the exit (got: {text[:160]!r})")
    check(took < 10.0, f"error arrives immediately ({took:.1f} s)")

    err, text = client.call_tool("emu_status")
    st = json.loads(text) if not err else {}
    check(st.get("connected") is False and "exit code 3" in st.get("last_exit", ""),
          f"emu_status reports last_exit (got: {text[:160]!r})")

    err, text = client.call_tool("emu_ping")
    second = json.loads(text) if not err else {}
    check(not err and second.get("pong") is True, "next emu_ping restarts the emulator")
    check(second.get("restarted") is True and "exit code 3" in second.get("restart_reason", ""),
          f"result carries restarted + restart_reason (got: {text[:200]!r})")
    check(second.get("pid") != first.get("pid"), "a new emulator process was started")

    err, text = client.call_tool("emu_ping")
    third = json.loads(text) if not err else {}
    check(not err and "restarted" not in third, "restart is reported only once")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    py = _find_venv_python()
    if py is None:
        print("SKIP: mcp-server/.venv interpreter not found", file=sys.stderr)
        return SKIP_EXIT
    tmp = Path(tempfile.mkdtemp(prefix="mz_mcp_restart_"))
    client = None
    ok = False
    try:
        exe = _make_fake_exe(tmp)
        flag = tmp / "crash.flag"
        ini = tmp / "wrapper.ini"
        ini.write_text("[MCP]\n", encoding="utf-8")
        env = os.environ.copy()
        env.update({"MZ800EMU_TRANSPORT": "pipe", "MZ800EMU_EXE": str(exe),
                    "MZ800EMU_INI": str(ini), "FAKE_EMU_CRASH_FLAG": str(flag)})
        client = McpClient(py, env)
        run(client, flag)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    finally:
        if client:
            client.close()
        # Falešný emulátor běží s cwd = tmp (mcp_server.py ho spouští
        # v adresáři binárky) a po konci wrapperu může ještě doznívat;
        # jednorázové rmtree pak nechávalo v tmp prázdné mz_mcp_restart_*.
        emu_test_proc.rmtree_retry(tmp)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
