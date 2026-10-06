#!/usr/bin/env python3
"""Regresní test MCP wrapperu: konec procesu emulátoru během požadavku.

Testuje skutečný ``_PipeTransport`` + ``_stdout_reader_task`` +
``_send_request`` z ``mcp_server.py`` proti FALEŠNÉMU emulátoru (malý
Python skript místo mz800emu, mluví stejným JSONL protokolem):

  A  proces skončí (exit 3) během čekání na odpověď -> ``_send_request``
     selže hned (ne až po SEND_TIMEOUT_S) s "emulator process exited
     (exit code 3)".
  B  ``emu_status`` po pádu hlásí ``connected: false`` a ``last_exit``.
  C  další tool call spustí nový emulátor a jeho výsledek nese
     ``restarted: true`` + ``restart_reason``; následující volání už ne.
  D  ``emu_stop`` (konec vyžádaný wrapperem) + další volání -> bez
     ``restarted``.
  E  proces skončí před hello -> ``_ensure_connected`` selže hned, ne po
     15 s timeoutu hello.
  F  tool call, který restart vyvolal, ale sám selže -> oznámení je v textu
     chyby (``[restarted: true; ...]``), nepřejde na další volání.
  G  restart vyvolaný mimo tool (přímý ``_send_request`` jako u resource)
     -> oznámení dostane nejbližší tool.
  H  odpověď odeslaná těsně před normálním koncem procesu se neztratí
     (reader čte až do EOF).

mcp_server importuje FastMCP; pod systémovým Pythonem bez něj se test
re-execne pod venv interpretem MCP serveru, bez venv vrací 77 (SKIP).

Exit 0 = vše PASS, 1 = aspoň jeden FAIL, 77 = SKIP.
"""
import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů

SKIP_EXIT = 77

REPO_ROOT = Path(__file__).resolve().parents[2]
MCP_DIR = REPO_ROOT / "mcp-server"


def _find_venv_python():
    """Najde venv interpret MCP serveru (priorita: env, .venv, žádný)."""
    env = os.environ.get("MZ_MCP_VENV_PY")
    if env and Path(env).is_file():
        return Path(env)
    for c in (MCP_DIR / ".venv" / "Scripts" / "python.exe",
              MCP_DIR / ".venv" / "bin" / "python",
              MCP_DIR / ".venv" / "bin" / "python3"):
        if c.is_file():
            return c
    return None


# Úklid spuštěných procesů (i venv interpretu) při selhání, přerušení
# nebo zabití ctestem; vnitřní limit je kratší než TIMEOUT v ctestu.
if __name__ == "__main__":
    emu_test_proc.install(deadline_s=105)

# Spuštění pod venv interpretem (sentinel MZ_MCP_REEXEC zaručí právě jedno
# opakované spuštění). Záměrně subprocess, ne os.execv: na Windows execv
# nenahradí proces, rodič hned skončí s kódem 0 a výsledek testu se ztratí.
try:
    import mcp  # noqa: F401
except ImportError:
    _venv = _find_venv_python()
    if _venv is not None and not os.environ.get("MZ_MCP_REEXEC"):
        os.environ["MZ_MCP_REEXEC"] = "1"
        sys.exit(subprocess.call(
            [str(_venv), str(Path(__file__).resolve()), *sys.argv[1:]]))
    print("SKIP: FastMCP/mcp not available (.venv missing or without mcp)",
          file=sys.stderr)
    sys.exit(SKIP_EXIT)

sys.path.insert(0, str(MCP_DIR))
os.environ.setdefault("MZ800EMU_TRANSPORT", "pipe")
import mcp_server as m  # noqa: E402

#: Falešný emulátor: banner + hello, pak JSONL odpovědi. "crash" ukončí
#: proces kódem 3 bez odpovědi, "emu_stop"/"shutdown" odpoví a skončí 0.
#: Proměnná FAKE_EMU_DIE_EARLY=1 ukončí proces před hello.
FAKE_EMU = r'''
import json, os, sys
if os.environ.get("FAKE_EMU_DIE_EARLY"):
    sys.exit(5)
print("fake emulator banner", flush=True)
print(json.dumps({"type": "hello", "commands": ["ping", "crash"]}), flush=True)
for line in sys.stdin:
    try:
        req = json.loads(line)
    except ValueError:
        continue
    cmd, rid = req.get("cmd"), req.get("req_id")
    if cmd == "crash" or (cmd == "ping" and os.environ.get("FAKE_EMU_CRASH_ON_PING")):
        sys.stdout.flush()
        os._exit(3)
    if cmd == "get_state":
        data = {"running": True, "paused": False}
    else:
        data = {"pong": True, "pid": os.getpid()}
    print(json.dumps({"req_id": rid, "success": True, "data": data}), flush=True)
    if cmd in ("emu_stop", "shutdown", "bye"):
        sys.stdout.flush()
        os._exit(0)
'''


class _FakePipeTransport(m._PipeTransport):
    """Pipe transport spouštějící falešný emulátor (Python skript)."""

    script: Path = Path()

    async def connect(self) -> None:
        self.process = subprocess.Popen(
            [sys.executable, str(self.script)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self._loop = asyncio.get_event_loop()


def _reset_bridge_globals():
    """Vynuluje globální stav bridge mezi testy."""
    m._transport = None
    m._reader_task = None
    m._response_queue = None
    m._event_queue = None
    m._send_lock = None
    m._next_req_id = 1
    m._last_exit = None
    m._restart_notice = None


async def _cleanup():
    """Ukončí případný běžící falešný emulátor (vlastní PID)."""
    tr = m._transport
    if isinstance(tr, m._PipeTransport) and tr.process is not None \
            and tr.process.poll() is None:
        tr.process.kill()
        tr.process.wait()
    if m._reader_task is not None:
        m._reader_task.cancel()
        try:
            await m._reader_task
        except (asyncio.CancelledError, Exception):
            pass
    _reset_bridge_globals()


async def test_a_to_c_crash_detection_and_restart_notice() -> None:
    """A+B+C: okamžitá chyba při pádu, last_exit, restarted u dalšího volání."""
    _reset_bridge_globals()
    try:
        first = json.loads(await m.emu_ping())
        assert first.get("pong") is True, f"ping failed: {first!r}"
        assert "restarted" not in first, f"unexpected restart: {first!r}"
        pid1 = first["pid"]

        t0 = time.monotonic()
        try:
            await m._send_request("crash")
        except RuntimeError as e:
            msg = str(e)
        else:
            raise AssertionError("crash request did not fail")
        took = time.monotonic() - t0
        assert took < 5.0, f"crash detected only after {took:.1f}s"
        assert "emulator process exited (exit code 3)" in msg, msg

        status = json.loads(await m.emu_status())
        assert status.get("connected") is False, status
        assert "exit code 3" in status.get("last_exit", ""), status

        second = json.loads(await m.emu_ping())
        assert second.get("pong") is True, second
        assert second.get("restarted") is True, \
            f"restart not reported: {second!r}"
        assert "exit code 3" in second.get("restart_reason", ""), second
        assert second["pid"] != pid1, "no new process was started"

        third = json.loads(await m.emu_ping())
        assert "restarted" not in third, f"restart reported twice: {third!r}"
    finally:
        await _cleanup()


async def test_d_requested_stop_is_not_a_restart() -> None:
    """D: emu_stop + další volání -> nový proces bez restarted."""
    _reset_bridge_globals()
    try:
        json.loads(await m.emu_ping())
        stopped = json.loads(await m.emu_stop())
        assert "error" not in stopped, stopped
        status = json.loads(await m.emu_status())
        assert "last_exit" not in status, status
        after = json.loads(await m.emu_ping())
        assert after.get("pong") is True, after
        assert "restarted" not in after, f"requested stop reported: {after!r}"
    finally:
        await _cleanup()


async def test_e_exit_before_hello_fails_fast() -> None:
    """E: proces skončí před hello -> chyba hned, ne po 15 s."""
    _reset_bridge_globals()
    os.environ["FAKE_EMU_DIE_EARLY"] = "1"
    try:
        t0 = time.monotonic()
        try:
            await m._ensure_connected()
        except RuntimeError as e:
            msg = str(e)
        else:
            raise AssertionError("connect did not fail")
        took = time.monotonic() - t0
        assert took < 5.0, f"early exit detected only after {took:.1f}s"
        assert "exit code 5" in msg and "before hello" in msg, msg
    finally:
        del os.environ["FAKE_EMU_DIE_EARLY"]
        await _cleanup()


async def _crash_current() -> None:
    """Shodí běžící falešný emulátor požadavkem crash (očekává chybu)."""
    try:
        await m._send_request("crash")
    except RuntimeError:
        return
    raise AssertionError("crash request did not fail")


async def test_f_failed_restarting_call_reports_restart() -> None:
    """F: restart ve volání, které samo selže -> oznámení v textu chyby."""
    _reset_bridge_globals()
    try:
        json.loads(await m.emu_ping())
        await _crash_current()
        os.environ["FAKE_EMU_CRASH_ON_PING"] = "1"
        try:
            await m.emu_ping()
        except RuntimeError as e:
            msg = str(e)
        else:
            raise AssertionError("ping on crashing emulator did not fail")
        finally:
            del os.environ["FAKE_EMU_CRASH_ON_PING"]
        assert "[restarted: true;" in msg, msg
        after = json.loads(await m.emu_ping())
        assert after.get("restarted") is True,             f"second unexpected end must be reported again: {after!r}"
        again = json.loads(await m.emu_ping())
        assert "restarted" not in again, f"notice leaked: {again!r}"
    finally:
        await _cleanup()


async def test_g_restart_outside_tool_goes_to_next_tool() -> None:
    """G: restart z _send_request mimo tool -> nese ho nejbližší tool."""
    _reset_bridge_globals()
    try:
        json.loads(await m.emu_ping())
        await _crash_current()
        resp = await m._send_request("ping")  # jako čtení resource
        assert "restarted" not in resp.get("data", {}), resp
        nxt = json.loads(await m.emu_ping())
        assert nxt.get("restarted") is True, nxt
    finally:
        await _cleanup()


async def test_h_last_response_before_exit_is_kept() -> None:
    """H: odpověď těsně před koncem procesu dorazí (reader čte do EOF)."""
    _reset_bridge_globals()
    try:
        json.loads(await m.emu_ping())
        m._transport.expected_exit = True
        resp = await m._send_request("bye")
        assert resp.get("success") is True, resp
    finally:
        await _cleanup()


async def _amain(tmp: Path) -> int:
    script = tmp / "fake_emu.py"
    script.write_text(FAKE_EMU, encoding="utf-8")
    _FakePipeTransport.script = script
    m._create_transport = lambda: _FakePipeTransport("fake")
    m.TRANSPORT_KIND = "pipe"
    m.SEND_TIMEOUT_S = 20.0  # bez opravy by test A čekal celý timeout

    tests = [
        ("A-C: crash detected at once, last_exit, restarted flag",
         test_a_to_c_crash_detection_and_restart_notice),
        ("D: emu_stop is not reported as restart",
         test_d_requested_stop_is_not_a_restart),
        ("E: exit before hello fails fast",
         test_e_exit_before_hello_fails_fast),
        ("F: failed restarting call reports restart in its error",
         test_f_failed_restarting_call_reports_restart),
        ("G: restart outside a tool goes to the next tool",
         test_g_restart_outside_tool_goes_to_next_tool),
        ("H: last response before exit is kept",
         test_h_last_response_before_exit_is_kept),
    ]
    failed = 0
    for name, fn in tests:
        try:
            await fn()
            print(f"OK   {name}")
        except Exception as e:  # noqa: BLE001 - výpis selhání testu
            failed += 1
            print(f"FAIL {name}: {e!r}", file=sys.stderr)
    if failed:
        print(f"\n{failed}/{len(tests)} tests failed", file=sys.stderr)
        return 1
    print(f"\nall {len(tests)} tests PASS")
    return 0


if __name__ == "__main__":
    _tmp = Path(tempfile.mkdtemp(prefix="mz_wrapper_exit_"))
    try:
        _rc = asyncio.run(_amain(_tmp))
    finally:
        shutil.rmtree(_tmp, ignore_errors=True)
    sys.exit(_rc)
