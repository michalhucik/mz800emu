#!/usr/bin/env python3
"""E2E test: Watch nemění stav stroje ani s `!` (MCP pipe, N67).

Watch (okno, ``watch_list``, ``watch_eval``) vyhodnocoval ``[addr]!``
a ``port[N]!`` se skutečným čtením: ``watch_eval`` s ``port[0xE0]!``
v pauze přemapoval paměť (IN z portu E0h na MZ-800 mapuje). Nově se ``!``
ve Watch ignoruje stejně jako v Test Eval breakpointu. Ověřuje:

  1  ``watch_eval`` ad-hoc ``port[0xE0]!`` nezmění mapu paměti,
  2  uložený watch ``port[0xE0]!`` (expr_scalar) při ``watch_list``
     a ``watch_eval`` podle jména nezmění mapu paměti,
  3  kontrola citlivosti: ``io_read`` portu E0h mapu změní (test by bez
     opravy opravdu selhal).

Proces běží izolovaně (dočasný ``--cfg-dir``, INI bez načítání uložených
BP, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402


def memory_map(emu):
    """Mapa paměti jako porovnatelný řetězec."""
    o = emu.call("state_observe", {"windows": [], "include": ["memory_map"]})
    return json.dumps(o.get("memory_map"), sort_keys=True)


def run(emu):
    """Scénáře 1-3."""
    emu.call("run", {"frames": 100})
    emu.call("pause")
    base = memory_map(emu)

    emu.call("watch_eval", {"expr": "port[0xE0]!"})
    check(memory_map(emu) == base, "1: watch_eval port[0xE0]! leaves the memory map")

    emu.call("watch_add", {"name": "w_e0", "mode": "expr_scalar", "expr": "port[0xE0]!", "type": "u8"})
    emu.call("watch_list")
    emu.call("watch_eval", {"name": "w_e0"})
    check(memory_map(emu) == base, "2: a stored watch port[0xE0]! leaves the memory map")

    emu.call("io_read", {"port": 0xE0})
    check(memory_map(emu) != base, "3: io_read of port E0h does change the memory map (sensitivity)")


def main():
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_watch_nose_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            run(emu)
            ok = True
        finally:
            emu.close()
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
