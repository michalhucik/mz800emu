#!/usr/bin/env python3
"""End-to-end test of the MCP pipe transport (V0.A.4).

Spawns mz800emu(.exe) with --mcp-pipe flag, sends a sequence of JSONL
requests over stdin and verifies responses on stdout. Designed for ctest
integration (mcp_pipe_e2e).

Test sequence (mirrors the ai2 main_pipe E2E vzor, adapted to V0.A.3
dispatch capabilities):

  0. Read hello message (capability advertisement)
  1. ping                  -> {"pong": true}
  2. pause                 -> {"paused": true}
  3. get_state             -> {"running": false, "paused": true}
  4. get_registers         -> 14 register fields (AF..IR)
  5. mem_read addr=0xE800  -> 16 bytes base64
  6. reset                 -> {"ok": true}
  7. mem_write             -> SKIPPED (not in V0.A.3 dispatch cmd_map)
  8. bp_add addr=0xE800    -> {"id": int, "addr": 0xE800}
  9. nonexistent_cmd       -> error response
 10. shutdown              -> {"shutdown": true}, exit code 0

Exit code: 0 if all assertions PASS, 1 otherwise.
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path


# Resolve binary path: tests/mcp/test_pipe.py -> repo root mz800emu.exe
_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_EXE_CANDIDATES = [
    _REPO_ROOT / "mz800emu.exe",
    _REPO_ROOT / "mz800emu",
]


def _find_exe():
    """Find mz800emu(.exe) binary; allow MZ_EMU env override."""
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env)
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    print("Tried:", file=sys.stderr)
    for c in _EXE_CANDIDATES:
        print(f"  {c}", file=sys.stderr)
    sys.exit(1)


def _spawn_line_reader(proc):
    """Spustí background vlákno, které čte řádky z proc.stdout do fronty.

    Vrací thread-safe ``queue.Queue`` s řádky (bez koncového '\\n'). EOF
    signalizuje sentinel ``None``.

    Důvod: dřívější ``select()`` + ``readline()`` varianta ztrácela řádky,
    které dorazily v jednom burstu - ``readline()`` přečetl první řádek a
    zbytek zůstal v Pythoním bufferu, takže ``select()`` na fd už další
    data neohlásil (deadlock do timeoutu). To se projevovalo u příkazů,
    které emitují víc řádků najednou (např. ``reset`` smíchaný s emu
    printf výstupem na stejném stdout kanálu). Vlákno + fronta čte
    spolehlivě řádek po řádku a funguje stejně na POSIXu i Windows
    (žádný ``select()`` na file objektu, který Windows nepodporuje).
    """
    q = queue.Queue()

    def _pump():
        for line in iter(proc.stdout.readline, ""):
            q.put(line.rstrip("\n"))
        q.put(None)  # EOF sentinel

    t = threading.Thread(target=_pump, daemon=True)
    t.start()
    return q


def main():
    exe = _find_exe()
    print(f"Using binary: {exe}")

    proc = subprocess.Popen(
        [str(exe), "--mcp-pipe"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
        cwd=str(_REPO_ROOT),
    )

    line_q = _spawn_line_reader(proc)
    passed = 0
    total = 0

    def _read_json_line(timeout_sec=10.0):
        """Read a single JSON object line from the emulator's stdout.

        The pipe transport reserves stdout exclusively for the JSONL
        protocol - the emulator redirects all of its free-form
        printf/g_print output (banners, INFO, "MZ800 Reset!", ...) to
        stderr at startup (see main_pipe.c::_redirect_stdout_to_stderr).
        Therefore every non-empty stdout line MUST be a valid JSON
        object; a non-JSON line means that guarantee regressed and the
        test fails it (instead of silently filtering, as it did before
        the stdout/stderr split).

        Řádky bere z background reader fronty (viz _spawn_line_reader),
        takže i víceřádkový burst se spolehlivě vyčte.
        """
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            try:
                line = line_q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                return None
            if line is None:
                # EOF - emulator zavřel stdout
                return None
            s = line.strip()
            if not s:
                # prázdný řádek je benigní - ignoruj
                continue
            try:
                return json.loads(s)
            except json.JSONDecodeError:
                # Regrese čistoty JSONL kanálu: na stdout se objevil
                # ne-JSON text (měl by jít na stderr). Nech test selhat.
                print(f"  REGRESSION: non-JSON line on stdout (JSONL "
                      f"channel must stay clean): {s!r}", file=sys.stderr)
                return None
        return None

    def send(req):
        proc.stdin.write(json.dumps(req) + "\n")
        proc.stdin.flush()
        return _read_json_line(10.0)

    try:
        # 0. Hello (první JSONL řádek na čistém stdout kanálu)
        total += 1
        hello = _read_json_line(15.0)
        if hello and hello.get("type") == "hello" and "commands" in hello:
            cmds = hello["commands"]
            print(f"Test 0 HELLO: type=hello, "
                  f"commands={len(cmds)}, PASS")
            passed += 1
        else:
            print(f"Test 0 HELLO: FAIL ({hello})")

        # Wait briefly for emu thread to come up (mzarch_main loop start).
        time.sleep(0.5)

        # 1. Ping
        total += 1
        resp = send({"req_id": 1, "cmd": "ping"})
        if resp and resp.get("success") and resp["data"].get("pong") is True:
            print("Test 1 PING: PASS")
            passed += 1
        else:
            print(f"Test 1 PING: FAIL ({resp})")

        # 2. Pause
        total += 1
        resp = send({"req_id": 2, "cmd": "pause"})
        if resp and resp.get("success") and resp["data"].get("paused") is True:
            print("Test 2 PAUSE: PASS")
            passed += 1
        else:
            print(f"Test 2 PAUSE: FAIL ({resp})")

        time.sleep(0.1)

        # 3. Get state
        total += 1
        resp = send({"req_id": 3, "cmd": "get_state"})
        if (resp and resp.get("success")
                and resp["data"].get("paused") is True
                and resp["data"].get("running") is False):
            print("Test 3 GET_STATE: PASS")
            passed += 1
        else:
            print(f"Test 3 GET_STATE: FAIL ({resp})")

        # 4. Get registers
        total += 1
        resp = send({"req_id": 4, "cmd": "get_registers"})
        regs_required = ["AF", "BC", "DE", "HL", "SP", "PC", "IR"]
        if (resp and resp.get("success")
                and all(k in resp.get("data", {}) for k in regs_required)):
            pc = resp["data"]["PC"]
            print(f"Test 4 GET_REGISTERS: PC={pc:04X}, PASS")
            passed += 1
        else:
            print(f"Test 4 GET_REGISTERS: FAIL ({resp})")

        # 5. Mem read (ROM area 0xE800 = monitor ROM in MZ-800)
        total += 1
        resp = send({"req_id": 5, "cmd": "mem_read",
                     "data": {"addr": 0xE800, "len": 16}})
        if (resp and resp.get("success")
                and resp["data"].get("len") == 16
                and isinstance(resp["data"].get("data_b64"), str)
                and len(resp["data"]["data_b64"]) > 0):
            print("Test 5 MEM_READ [E800..]: PASS")
            passed += 1
        else:
            print(f"Test 5 MEM_READ: FAIL ({resp})")

        # 6. Reset
        total += 1
        resp = send({"req_id": 6, "cmd": "reset"})
        if resp and resp.get("success") and resp["data"].get("ok") is True:
            print("Test 6 RESET: PASS")
            passed += 1
        else:
            print(f"Test 6 RESET: FAIL ({resp})")

        # 7. mem_write SKIP (not in V0.A.3 dispatch).
        print("Test 7 MEM_WRITE: SKIPPED (not in V0.A.3 cmd_map)")

        # 8. Breakpoint add
        total += 1
        resp = send({"req_id": 8, "cmd": "bp_add",
                     "data": {"addr": 0xE800}})
        if (resp and resp.get("success")
                and resp["data"].get("addr") == 0xE800
                and isinstance(resp["data"].get("id"), int)):
            print(f"Test 8 BP_ADD: id={resp['data']['id']}, PASS")
            passed += 1
        else:
            print(f"Test 8 BP_ADD: FAIL ({resp})")

        # 9. Unknown command
        total += 1
        resp = send({"req_id": 9, "cmd": "nonexistent_cmd"})
        if resp and resp.get("success") is False and "error" in resp:
            print(f"Test 9 UNKNOWN_CMD: PASS (error={resp['error']!r})")
            passed += 1
        else:
            print(f"Test 9 UNKNOWN_CMD: FAIL ({resp})")

        # 9b. Breakpoint cleanup - BP z testu 8 NESMÍ přežít shutdown.
        # Emulátor při čistém exitu persistuje breakpointy do
        # mz800-breakpoints.bpt v CWD; další start emu v témže adresáři
        # (další test, typicky mcp_speed_smoke) by pak okamžitě narazil
        # na PC_EXEC 0xE800 a startoval paused -> timeout celé suity.
        total += 1
        resp = send({"req_id": 11, "cmd": "bp_clear"})
        if resp and resp.get("success"):
            print("Test 9b BP_CLEAR: PASS")
            passed += 1
        else:
            print(f"Test 9b BP_CLEAR: FAIL ({resp})")

        # 10. Shutdown
        total += 1
        resp = send({"req_id": 10, "cmd": "shutdown"})
        if resp and resp.get("success") and resp["data"].get("shutdown") is True:
            print("Test 10 SHUTDOWN: PASS")
            passed += 1
        else:
            print(f"Test 10 SHUTDOWN: FAIL ({resp})")

        # Wait for clean exit
        try:
            rc = proc.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            print("ERROR: process did not exit within 10s, killing")
            proc.kill()
            rc = -1

        print(f"\n{passed}/{total} tests PASSED, exit code={rc}")
        sys.exit(0 if passed == total and rc == 0 else 1)

    finally:
        if proc.poll() is None:
            try:
                proc.kill()
            except Exception:
                pass


if __name__ == "__main__":
    main()
