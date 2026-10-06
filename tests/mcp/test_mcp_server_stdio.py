#!/usr/bin/env python3
"""E2E test of ``mcp_server.py`` via the JSON-RPC MCP stdio protocol (V0.B.7).

Spawns ``mcp_server.py`` as a subprocess (running on the developer's
venv) and acts as a minimal MCP client over the JSON-RPC newline-delimited
stdio framing used by Claude Code. The exercise covers:

  1. ``initialize``       -> returns server info + capabilities.
  2. ``notifications/initialized`` -> notification (no response).
  3. ``tools/list``       -> verifies that all 39 expected tools are
                             advertised by name (V0.B.3 10 + V0.B.6 7 +
                             V1.A.1 5 + V1.A.2 4 + V1.A.3 4 +
                             V1.A.4 4 + V1.A.5 5).
  4. ``tools/call`` ``emu_ping`` -> triggers lazy spawn of the emulator
                             subprocess via the pipe transport and
                             verifies the returned ``{"pong": true}``
                             payload.
  5. ``tools/call`` ``emu_pause`` -> exercises one dbgapi-proxied tool.
  6. ``resources/list``   -> verifies all 7 expected Resource URIs are
                             advertised (V0.B.7).
  7. ``resources/read`` on ``emulator://state`` -> verifies the read
                             returns JSON with a ``transport`` field.

The test is idempotent and does not require a running GUI emulator -
``mcp_server.py`` in pipe mode spawns its own headless ``mz800emu.exe``
child process.

Exit code: 0 if all assertions PASS, 1 otherwise, 77 = SKIP (no venv).

Environment notes:

* ``MZ_MCP_VENV_PY`` env var can override the Python interpreter
  (otherwise the test uses ``mcp-server/.venv/Scripts/python.exe``
  on Windows; without the venv the test is SKIPPED with exit code 77).
* The emulator subprocess inherits ``cwd = repo root`` from
  ``_PipeTransport.connect``; ``mz800emu.exe`` must therefore be present
  in the repo root (= where ``Makefile`` puts it after ``make mz800emu``).
* Its ``--work-dir`` is a temporary directory (``MZ800EMU_WORK_DIR``),
  so relative outputs (CDL export on exit) do not land in the repo root.
"""

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


_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_MCP_SERVER_DIR = _REPO_ROOT / "mcp-server"
_MCP_SERVER_PY = _MCP_SERVER_DIR / "mcp_server.py"

# Exit kód pro ctest "skip" (SKIP_RETURN_CODE v tests/mcp/CMakeLists.txt).
SKIP_EXIT = 77


def _find_python():
    """Najde Python interpret pro mcp_server.py.

    Priorita:
      1. env override ``MZ_MCP_VENV_PY``
      2. mcp-server/.venv/Scripts/python.exe (Win) nebo bin/python (POSIX)

    Systémový Python (``sys.executable``) se záměrně NEpoužívá jako záloha:
    nemá balíček ``mcp`` (FastMCP), server by nenastartoval a test by selhal
    při INITIALIZE. Bez venv interpretu funkce vrací None a test se přeskočí.

    Returns:
        Path k venv interpretu, nebo None (venv chybí).
    """
    env = os.environ.get("MZ_MCP_VENV_PY")
    if env and Path(env).is_file():
        return Path(env)
    candidates = [
        _MCP_SERVER_DIR / ".venv" / "Scripts" / "python.exe",
        _MCP_SERVER_DIR / ".venv" / "bin" / "python",
        _MCP_SERVER_DIR / ".venv" / "bin" / "python3",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def _readline_with_timeout(proc, timeout_sec):
    """Read a single line from proc.stdout with a soft timeout.

    On Windows ``select`` does not support file objects, so this falls
    back to a simple blocking readline; the outer test harness relies
    on the subprocess timeout (= ctest TIMEOUT 90) for hard stop.
    """
    if sys.platform == "win32":
        return proc.stdout.readline()
    import select
    end = time.time() + timeout_sec
    while time.time() < end:
        r, _, _ = select.select([proc.stdout], [], [], 0.5)
        if r:
            return proc.stdout.readline()
    return ""


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=80)
    py = _find_python()
    if py is None:
        print("SKIP: mcp-server/.venv interpreter not found "
              "(the 'mcp' package is required to run mcp_server.py); "
              "create the venv or set MZ_MCP_VENV_PY",
              file=sys.stderr)
        sys.exit(SKIP_EXIT)
    if not _MCP_SERVER_PY.is_file():
        print(f"ERROR: mcp_server.py not found at {_MCP_SERVER_PY}",
              file=sys.stderr)
        sys.exit(1)

    # Ensure pipe transport (default) - we don't depend on a running GUI.
    env = os.environ.copy()
    env["MZ800EMU_TRANSPORT"] = "pipe"
    # Mz800emu binary path - resolve relative to repo root regardless of
    # CMAKE_BINARY_DIR.
    exe = _REPO_ROOT / ("mz800emu.exe" if sys.platform == "win32" else "mz800emu")
    env["MZ800EMU_EXE"] = str(exe)
    # Dočasný work_dir emulátoru: CDL export při ukončení (výchozí
    # cdl_export_dir = "cdl-export" relativně k work_dir) nesmí vzniknout
    # v kořeni repa.
    work_dir = tempfile.mkdtemp(prefix="mz_mcp_stdio_e2e_")
    env["MZ800EMU_WORK_DIR"] = work_dir

    print(f"Python:     {py}")
    print(f"Server:     {_MCP_SERVER_PY}")
    print(f"Emu binary: {exe} (exists={exe.is_file()})")

    proc = subprocess.Popen(
        [str(py), str(_MCP_SERVER_PY)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
        env=env,
    )

    passed = 0
    total = 0

    def _send_jsonrpc(req):
        line = json.dumps(req) + "\n"
        proc.stdin.write(line)
        proc.stdin.flush()

    def _read_jsonrpc_response(timeout_sec=20.0):
        """Read JSON-RPC response - skips notifications without ``id``."""
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            line = _readline_with_timeout(
                proc, max(0.5, deadline - time.time()))
            if not line:
                return None
            s = line.strip()
            if not s:
                continue
            try:
                msg = json.loads(s)
            except json.JSONDecodeError:
                # Non-JSON banner shouldn't normally happen on the MCP
                # wire from FastMCP, but be defensive.
                continue
            # Notifications mají method, žádné id. Pro náš test
            # zahazujeme.
            if "id" not in msg:
                continue
            return msg
        return None

    try:
        # 1. Initialize handshake (MCP protocol version 2025-06-18 per
        #    FastMCP default).
        total += 1
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "test_mcp_server_stdio", "version": "1.0"},
            },
        })
        resp = _read_jsonrpc_response(timeout_sec=15.0)
        if (resp and resp.get("id") == 1
                and "result" in resp
                and "serverInfo" in resp.get("result", {})):
            server_name = resp["result"]["serverInfo"].get("name", "?")
            print(f"Test 1 INITIALIZE: serverInfo.name={server_name!r}, PASS")
            passed += 1
        else:
            print(f"Test 1 INITIALIZE: FAIL ({resp})")
            return 1

        # 2. Klient musí poslat initialized notification - bez ní FastMCP
        #    odmítne tools/list (= protocol stage check).
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "method": "notifications/initialized",
        })

        # 3. tools/list - 53 tools expected (V0.B.3 10 + V0.B.6 7 +
        #    V1.A.1 5 snapshot+cooperation + V1.A.2 4 symbol management +
        #    V1.A.3 4 step/run_until + V1.A.4 4 event subscribe + trap +
        #    V1.A.5 5 chip-level fault injection + V1.A.6 9 Watch +
        #    Callstack + CDL + V1.A.7 5 Profiler).
        total += 1
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/list",
        })
        resp = _read_jsonrpc_response(timeout_sec=15.0)
        expected_tools = {
            # V0.B.3 (10)
            "emu_status", "emu_ping", "emu_pause", "emu_get_registers",
            "emu_mem_read", "emu_mem_write", "emu_run", "emu_reset",
            "emu_bp_add", "emu_bp_list",
            # V0.B.6 (7)
            "emu_bp_remove", "emu_bp_clear", "emu_bp_enable",
            "emu_step_into", "emu_step_over", "emu_step_n",
            "emu_run_until_addr",
            # V1.A.1 (5)
            "emu_snapshot_save", "emu_snapshot_save_buffer",
            "emu_snapshot_load", "emu_snapshot_load_buffer",
            "emu_cooperation_hint_set",
            # V1.A.2 (4)
            "emu_symbol_add", "emu_symbol_remove",
            "emu_symbol_lookup", "emu_symbol_list",
            # V1.A.3 (4)
            "emu_step_out", "emu_run_until_raster",
            "emu_run_until_tstate", "emu_run_until_event",
            # V1.A.4 (4)
            "emu_event_subscribe", "emu_event_unsubscribe",
            "emu_event_poll", "emu_trap_respond",
            # V1.A.5 (5)
            "emu_io_read", "emu_io_write",
            "emu_irq_inject", "emu_nmi_inject",
            "emu_mem_write_force",
            # V1.A.6 (9)
            "emu_watch_add", "emu_watch_remove",
            "emu_watch_list", "emu_watch_eval",
            "emu_callstack_get",
            "emu_cdl_start", "emu_cdl_stop",
            "emu_cdl_reset", "emu_cdl_export",
            # V1.A.7 (5)
            "emu_profiler_start", "emu_profiler_stop",
            "emu_profiler_reset", "emu_profiler_export",
            "emu_profiler_get",
            # V1.B.1 (5)
            "emu_media_load_mzf", "emu_media_load_binary",
            "emu_media_insert", "emu_media_eject", "emu_media_state",
            # V1.B.2 (5)
            "emu_settings_set", "emu_settings_get",
            "emu_platform_set",
            "emu_periph_attach", "emu_periph_detach",
            # V1.B.3 (2 - hot-swap, pipe-only)
            "emu_stop", "emu_start",
            # V1.C.1 (6 - HID Tools)
            "emu_input_send_key", "emu_input_send_keys",
            "emu_input_press_key", "emu_input_release_key",
            "emu_input_send_joystick",
            "emu_input_send_keys_with_delays",
            # CPU register write
            "emu_set_register",
            # Disassembly + history
            "emu_dasm", "emu_history_get",
            # Media bootstrap composite (mzdos-support 0002)
            "emu_media_run_mzf",
            # V1.E.2 commit 1 - CPU control + details (5)
            "emu_get_reg", "emu_force_pause",
            "emu_set_user_cycle_origin",
            "emu_get_im2_vector", "emu_get_raster_pos",
            # V1.E.2 commit 2 - CPU flags (2)
            "emu_get_cpu_flags", "emu_set_cpu_flags",
            # V1.E.2 commit 3 - last_instr + panel_batch (2)
            "emu_get_last_instr", "emu_get_cpu_panel_batch",
            # V1.E.3 commit 1 - debugger state (3)
            "emu_debugger_activate", "emu_debugger_deactivate",
            "emu_is_debugger_active",
            # V1.E.3 commit 2 - PIO-Z80 IM2 vector (1)
            "emu_set_pioz80_interrupt_vector",
            # V1.E.4 commit 1 - BP advanced (6)
            "emu_bp_create_with_init", "emu_bp_set_parent",
            "emu_bp_update",
            "emu_bpgrp_add", "emu_bpgrp_remove", "emu_bpgrp_update",
            # V1.E.4 commit 2 - Stack analytics (6)
            "emu_stack_history_enable", "emu_stack_history_reset",
            "emu_stack_regions_add", "emu_stack_regions_edit",
            "emu_stack_regions_remove",
            "emu_stack_regions_reset_watermark",
            # V1.E.5 - Eventlog/TLOG (6)
            "emu_eventlog_start", "emu_eventlog_stop",
            "emu_eventlog_clear", "emu_eventlog_set_capacity",
            "emu_eventlog_set_mask", "emu_eventlog_get_event",
            # mzdos-support 0007 - Direct memory region read/write/file (5)
            "emu_regions_list", "emu_region_read", "emu_region_write",
            "emu_file_load_to_region", "emu_file_save_from_region",
            # BACKLOG D - emulation speed control (2)
            "emu_set_speed", "emu_speed_step",
            # BACKLOG B - bookmark write (2)
            "emu_bookmark_add", "emu_bookmark_remove",
            # CMT-A - CMT transport + recording + cmthack toggle (7)
            "emu_cmt_play", "emu_cmt_play_paused", "emu_cmt_stop",
            "emu_cmt_pause", "emu_cmt_eject", "emu_cmt_record",
            "emu_cmt_hack_set",
            # CMT-B - CMT properties + tape ops (7)
            "emu_cmt_set_speed", "emu_cmt_set_polarity",
            "emu_cmt_set_cpu_boost", "emu_cmt_set_mzfsize_check",
            "emu_cmt_open", "emu_cmt_tape_seek",
            "emu_cmt_tape_set_block_speed",
        }
        if (resp and resp.get("id") == 2
                and "result" in resp
                and "tools" in resp["result"]):
            tools = resp["result"]["tools"]
            names = {t.get("name") for t in tools}
            missing = expected_tools - names
            if not missing and len(tools) >= 113:
                print(f"Test 2 TOOLS_LIST: {len(tools)} tools, PASS")
                passed += 1
            else:
                print(f"Test 2 TOOLS_LIST: FAIL "
                      f"(missing={missing}, got={names})")
        else:
            print(f"Test 2 TOOLS_LIST: FAIL ({resp})")

        # 3a. Description of emu_mem_write MUST contain a WARNING token
        #     about destructive operation (= V0.B.3 acceptance criterion).
        total += 1
        if resp and "result" in resp:
            mw = next((t for t in resp["result"]["tools"]
                       if t.get("name") == "emu_mem_write"), None)
            desc = (mw or {}).get("description", "")
            if "WARNING" in desc and "destructive" in desc.lower():
                print("Test 3 MEM_WRITE_WARNING: PASS")
                passed += 1
            else:
                print(f"Test 3 MEM_WRITE_WARNING: FAIL (desc={desc!r})")
        else:
            print("Test 3 MEM_WRITE_WARNING: SKIPPED (no tools/list result)")

        # 3b. V1.B.1 destructive Media Tools MUST also carry WARNING
        #     tokens. emu_media_load_binary (raw write to RAM) and
        #     emu_media_insert (auto-eject of currently mounted image).
        total += 1
        if resp and "result" in resp:
            destructive_media = ("emu_media_load_binary", "emu_media_insert")
            all_ok = True
            for tname in destructive_media:
                t = next((x for x in resp["result"]["tools"]
                          if x.get("name") == tname), None)
                d = (t or {}).get("description", "")
                if "WARNING" not in d:
                    all_ok = False
                    print(f"Test 3b MEDIA_WARNING: FAIL "
                          f"({tname} missing WARNING)")
                    break
            if all_ok:
                print("Test 3b MEDIA_WARNING: PASS")
                passed += 1
        else:
            print("Test 3b MEDIA_WARNING: SKIPPED (no tools/list result)")

        # 3c. V1.B.2 destructive Platform + Config Tools MUST also
        #     carry WARNING tokens. emu_settings_set (mutates global
        #     state), emu_platform_set (would discard state) and
        #     emu_periph_attach / detach (cfgmain mutation).
        total += 1
        if resp and "result" in resp:
            destructive_v1b2 = (
                "emu_settings_set",
                "emu_platform_set",
                "emu_periph_attach",
                "emu_periph_detach",
            )
            all_ok = True
            for tname in destructive_v1b2:
                t = next((x for x in resp["result"]["tools"]
                          if x.get("name") == tname), None)
                d = (t or {}).get("description", "")
                if "WARNING" not in d:
                    all_ok = False
                    print(f"Test 3c PLATFORM_CFG_WARNING: FAIL "
                          f"({tname} missing WARNING)")
                    break
            if all_ok:
                print("Test 3c PLATFORM_CFG_WARNING: PASS")
                passed += 1
        else:
            print("Test 3c PLATFORM_CFG_WARNING: SKIPPED "
                  "(no tools/list result)")

        # 3d. V1.C.1 HID Tools MUST carry WARNING tokens (= user input
        #     simulation can cause unexpected behavior).
        total += 1
        if resp and "result" in resp:
            hid_tools = (
                "emu_input_send_key",
                "emu_input_send_keys",
                "emu_input_press_key",
                "emu_input_release_key",
                "emu_input_send_joystick",
                "emu_input_send_keys_with_delays",
            )
            all_ok = True
            for tname in hid_tools:
                t = next((x for x in resp["result"]["tools"]
                          if x.get("name") == tname), None)
                d = (t or {}).get("description", "")
                if "WARNING" not in d:
                    all_ok = False
                    print(f"Test 3d HID_WARNING: FAIL "
                          f"({tname} missing WARNING)")
                    break
            if all_ok:
                print("Test 3d HID_WARNING: PASS")
                passed += 1
        else:
            print("Test 3d HID_WARNING: SKIPPED (no tools/list result)")

        # 4. tools/call emu_ping - triggers lazy spawn of emu subprocess.
        total += 1
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {"name": "emu_ping", "arguments": {}},
        })
        # Lazy spawn může trvat delší dobu (= emu init), proto delší timeout.
        resp = _read_jsonrpc_response(timeout_sec=30.0)
        if (resp and resp.get("id") == 3 and "result" in resp):
            content = resp["result"].get("content", [])
            text_blob = ""
            for c in content:
                if c.get("type") == "text":
                    text_blob += c.get("text", "")
            try:
                payload = json.loads(text_blob) if text_blob else {}
            except json.JSONDecodeError:
                payload = {}
            if payload.get("pong") is True:
                print("Test 4 TOOLS_CALL emu_ping: pong=true, PASS")
                passed += 1
            else:
                print(f"Test 4 TOOLS_CALL emu_ping: FAIL "
                      f"(payload={payload})")
        else:
            print(f"Test 4 TOOLS_CALL emu_ping: FAIL ({resp})")

        # 5. tools/call emu_pause - exercises a dbgapi-proxied tool.
        # Krátká pauza, aby emu thread dokončil bootstrap a začal
        # zpracovávat dbgapi cmdrq frontu (= analogie test_pipe.py
        # time.sleep(0.5) mezi hello a první pause; jinak by mohl
        # dispatch_pause vrátit timeout místo paused=true).
        time.sleep(0.5)
        total += 1
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {"name": "emu_pause", "arguments": {}},
        })
        resp = _read_jsonrpc_response(timeout_sec=15.0)
        if (resp and resp.get("id") == 4 and "result" in resp):
            content = resp["result"].get("content", [])
            text_blob = "".join(
                c.get("text", "") for c in content if c.get("type") == "text")
            try:
                payload = json.loads(text_blob) if text_blob else {}
            except json.JSONDecodeError:
                payload = {}
            if payload.get("paused") is True:
                print("Test 5 TOOLS_CALL emu_pause: paused=true, PASS")
                passed += 1
            else:
                print(f"Test 5 TOOLS_CALL emu_pause: FAIL "
                      f"(payload={payload})")
        else:
            print(f"Test 5 TOOLS_CALL emu_pause: FAIL ({resp})")

        # 6. resources/list - 7 Resources expected (V0.B.7).
        #    Non-template URI count varies podle FastMCP API: některé
        #    verze hlásí templates v separátní `resourceTemplates`,
        #    jiné v `resources` jako URI bez expanzi. Pro robustnost
        #    posbíráme oba seznamy.
        total += 1
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 5,
            "method": "resources/list",
        })
        resp = _read_jsonrpc_response(timeout_sec=15.0)
        # Static (non-template) URIs - musí být v resources/list.
        # V0.B.7 mělo 6 static URIs; V1.D.1 doplnilo 8 dalších = 14 static;
        # V1.D.2.A doplnilo dalších 5 = 19 static.
        # V1.D.2.B krok 1 doplnil watch + stack = 21 static.
        # V1.D.2.B krok 2+3 doplnil vars + bookmarks = 23 static.
        # V1.D.3.A krok 1 doplnil periph/i8255 = 24 static
        # V1.D.3.A krok 2 doplnil periph/i8253 = 25 static
        # V1.D.3.A krok 3 doplnil periph/z80_pio = 26 static
        # V1.D.3.B doplnil periph/sn76489 + ay3_8910 + beeper = 29 static
        # V1.D.3.C doplnil periph/gdg + wd1793 + cmt + qd = 33 static
        # V1.D.4 doplnil input/keyboard/{state,matrix_info},
        # input/joystick/state, frame/{framebuffer/info,screenshot.raw,
        # screenshot}, video/text_dump = 40 static
        # MCP docs Resources doplnily 8 static (= 48 static celkem):
        # emulator://docs/{index,memory_layout,bp_dsl,smart_vars,
        # action_dsl,watch_dsl,eventlog_mask,sharp_display_code}
        # (= 49 Resources celkem minus 1 template
        # `memory/{addr_hex}/{length}`; watch/snapshot/{name} je také
        # template, viz následující test 7).
        expected_static_uris = {
            # V0.B.7
            "emulator://state",
            "emulator://cpu/registers",
            "emulator://breakpoints",
            "emulator://platform/info",
            "emulator://config/mcp",
            "emulator://config/peripherals",
            # V1.D.1
            "emulator://config/settings",
            "emulator://media/state",
            # BACKLOG D - emulation speed control
            "emulator://speed",
            "emulator://cpu/im2_vector",
            "emulator://cpu/interrupt_bus",
            "emulator://cooperation/policy",
            "emulator://security/profile",
            "emulator://memory/map",
            "emulator://memext/info",
            # V1.D.2.A
            "emulator://callstack",
            "emulator://profiler",
            "emulator://symbols",
            "emulator://stack/history",
            "emulator://stack/regions",
            # V1.D.2.B krok 1
            "emulator://watch",
            "emulator://stack",
            # V1.D.2.B krok 2+3
            "emulator://vars",
            "emulator://bookmarks",
            # V1.D.3.A - IRQ chip Resources
            "emulator://periph/i8255",
            "emulator://periph/i8253",
            "emulator://periph/z80_pio",
            # V1.D.3.B - audio chip Resources
            "emulator://periph/sn76489",
            "emulator://periph/ay3_8910",
            "emulator://periph/beeper",
            # V1.D.3.C - storage + display Resources
            "emulator://periph/gdg",
            "emulator://periph/wd1793",
            "emulator://periph/cmt",
            "emulator://periph/cmt/tape",
            "emulator://periph/qd",
            # V1.D.4 - input + frame Resources
            "emulator://input/keyboard/state",
            "emulator://input/keyboard/matrix_info",
            "emulator://input/joystick/state",
            "emulator://frame/framebuffer/info",
            "emulator://frame/screenshot.raw",
            "emulator://frame/screenshot",
            "emulator://video/text_dump",
            # MCP docs Resources - statické EN reference docs pro AI klienty
            "emulator://docs/index",
            "emulator://docs/memory_layout",
            "emulator://docs/bp_dsl",
            "emulator://docs/smart_vars",
            "emulator://docs/action_dsl",
            "emulator://docs/watch_dsl",
            "emulator://docs/eventlog_mask",
            "emulator://docs/sharp_display_code",
            "emulator://docs/cmt_workflow",
        }
        if (resp and resp.get("id") == 5
                and "result" in resp
                and "resources" in resp["result"]):
            resources = resp["result"]["resources"]
            uris = {r.get("uri") for r in resources}
            missing = expected_static_uris - uris
            if not missing and len(resources) >= 19:
                print(f"Test 6 RESOURCES_LIST: {len(resources)} resources, PASS")
                passed += 1
            else:
                print(f"Test 6 RESOURCES_LIST: FAIL "
                      f"(missing={missing}, got={uris})")
        else:
            print(f"Test 6 RESOURCES_LIST: FAIL ({resp})")

        # 6a. resources/templates/list - template URI s placeholdery.
        #     FastMCP 1.x odděluje template Resources do dedikovaného
        #     endpointu (= MCP spec 2025-06-18).
        total += 1
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 6,
            "method": "resources/templates/list",
        })
        resp = _read_jsonrpc_response(timeout_sec=15.0)
        memory_template = "emulator://memory/{addr_hex}/{length}"
        # V1.D.2.C - per-watch snapshot Resource (= druhý template URI).
        watch_snapshot_template = "emulator://watch/snapshot/{name}"
        if (resp and resp.get("id") == 6 and "result" in resp):
            templates = (resp["result"].get("resourceTemplates")
                         or resp["result"].get("templates") or [])
            template_uris = {t.get("uriTemplate") or t.get("uri")
                             for t in templates}
            missing_templates = {memory_template, watch_snapshot_template} - template_uris
            if not missing_templates:
                print(f"Test 7 RESOURCE_TEMPLATE: 2 templates, PASS")
                passed += 1
            else:
                print(f"Test 7 RESOURCE_TEMPLATE: FAIL "
                      f"(missing={missing_templates}, "
                      f"template_uris={template_uris})")
        else:
            print(f"Test 7 RESOURCE_TEMPLATE: FAIL ({resp})")

        # 8. resources/read na emulator://state - bez transportu vrátí
        #    {"connected": false, "transport": "pipe"} (emu_ping z testu
        #    4 ho už ovšem spawnoval; transport žije).
        total += 1
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 7,
            "method": "resources/read",
            "params": {"uri": "emulator://state"},
        })
        resp = _read_jsonrpc_response(timeout_sec=15.0)
        if (resp and resp.get("id") == 7 and "result" in resp):
            contents = resp["result"].get("contents", [])
            text_blob = "".join(
                c.get("text", "") for c in contents if "text" in c)
            try:
                state = json.loads(text_blob) if text_blob else {}
            except json.JSONDecodeError:
                state = {}
            if "transport" in state:
                print(f"Test 8 RESOURCE_READ emulator://state: "
                      f"transport={state.get('transport')!r}, PASS")
                passed += 1
            else:
                print(f"Test 8 RESOURCE_READ emulator://state: FAIL "
                      f"(state={state})")
        else:
            print(f"Test 8 RESOURCE_READ emulator://state: FAIL ({resp})")

        # 9. E2E roundtrip (V1.A.5): subscribe(breakpoint_hit) -> bp_add
        #    -> resume -> wait -> poll. We verify the wiring of the
        #    event_bus across the JSONL/MCP transport boundary. Use a
        #    "manual" topic ("paused") because forcing a real BP hit
        #    on a deterministic PC during the cold boot screen would
        #    require an MZ800-specific test fixture.
        total += 1

        # 9a. subscribe to "paused" topic.
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 8,
            "method": "tools/call",
            "params": {
                "name": "emu_event_subscribe",
                "arguments": {"topics": ["paused"]},
            },
        })
        resp_sub = _read_jsonrpc_response(timeout_sec=15.0)

        # 9b. emu_pause (already paused from test 5, but call again to
        #     guarantee a "paused" event is emitted via the BP/manual
        #     pause path).
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 9,
            "method": "tools/call",
            "params": {"name": "emu_run", "arguments": {"frames": 1}},
        })
        _read_jsonrpc_response(timeout_sec=15.0)
        time.sleep(0.3)
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 10,
            "method": "tools/call",
            "params": {"name": "emu_pause", "arguments": {}},
        })
        _read_jsonrpc_response(timeout_sec=15.0)

        # 9c. poll with timeout.
        _send_jsonrpc({
            "jsonrpc": "2.0",
            "id": 11,
            "method": "tools/call",
            "params": {
                "name": "emu_event_poll",
                "arguments": {"timeout_ms": 2000, "max_events": 5},
            },
        })
        resp = _read_jsonrpc_response(timeout_sec=10.0)
        event_ok = False
        if (resp_sub and resp_sub.get("id") == 8 and "result" in resp_sub
                and resp and resp.get("id") == 11 and "result" in resp):
            content = resp["result"].get("content", [])
            text_blob = "".join(
                c.get("text", "") for c in content if c.get("type") == "text")
            try:
                payload = json.loads(text_blob) if text_blob else {}
            except json.JSONDecodeError:
                payload = {}
            events = payload.get("events", [])
            # Accept any "paused" topic event regardless of cause - we
            # only verify the wiring.
            for ev in events:
                if ev.get("topic") == "paused":
                    event_ok = True
                    break
            if event_ok:
                print(f"Test 9 EVENT_E2E_ROUNDTRIP: "
                      f"got {len(events)} event(s), paused seen, PASS")
                passed += 1
            else:
                print(f"Test 9 EVENT_E2E_ROUNDTRIP: FAIL "
                      f"(no paused event; events={events})")
        else:
            print(f"Test 9 EVENT_E2E_ROUNDTRIP: FAIL "
                  f"(sub_resp={resp_sub}, poll_resp={resp})")

        print(f"\n{passed}/{total} tests PASSED")
        return 0 if passed == total else 1

    finally:
        # Graceful close - posíláme EOF na stdin a počkáme. mcp_server.py
        # má best-effort cleanup _shutdown_emu v finally bloku main().
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            print("WARN: mcp_server didn't exit in 15s, killing", file=sys.stderr)
            proc.kill()
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                pass
        _cleanup_work_dir(work_dir)


def _cleanup_work_dir(work_dir):
    """Smaže dočasný work_dir emulátoru.

    Emulátor (potomek mcp_server.py) může po skončení wrapperu ještě chvíli
    běžet a při ukončení zapsat CDL export; proto se nejdřív počká (max.
    10 s), až export vznikne, a mazání se pak zkusí opakovaně.
    """
    marker = Path(work_dir) / "cdl-export" / "cdl-export.json"
    deadline = time.time() + 10.0
    while time.time() < deadline and not marker.exists():
        time.sleep(0.2)
    for _ in range(25):
        shutil.rmtree(work_dir, ignore_errors=True)
        if not Path(work_dir).exists():
            return
        time.sleep(0.2)


if __name__ == "__main__":
    sys.exit(main())
