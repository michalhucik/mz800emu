#!/usr/bin/env python3
"""E2E test: run_until_tstate s target_ticks - zastavení na čase GDG (pipe).

Emu vlákno zastaví běh na začátku první instrukce, jejíž čas GDG je >=
cíl (DBGAPI_CMD_RUN_UNTIL_TICKS, brána GLOBAL breakpointů). Ověřuje:

  1  cíl přesně na začátku instrukce: ``exact``, PC té instrukce,
     ``time.ticks`` ze ``state_observe`` = ``ticks`` odpovědi,
  2  cíl uvnitř instrukce: zastaví na začátku další instrukce,
  3  cíl teď (už dosažený): ``reached`` bez běhu, ``delta_ticks`` 0,
  4  uživatelský breakpoint před cílem zastaví dřív (``stopped_by``
     ``breakpoint``, ``reached`` false); po něm běh na čas funguje dál.

Program (MZ-800, RAM 3000h): ``DI; 3001h: INC HL; JR 3001h``. Délky:
DI 4 T, INC HL 6 T, JR 12 T; takt GDG = T-stav x 5 (MZ-800).

Proces běží izolovaně (dočasný ``--cfg-dir``, INI bez načítání uložených
BP, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Program a jeho adresy.
PROG = 0x3000
PROG_HEX = "F3" "23" "18FD"
#: Takty GDG na T-stav (MZ-800).
DIV = 5


def now_ticks(emu):
    """Absolutní čas GDG a PC z jednoho okamžiku."""
    o = emu.call("state_observe", {"windows": []})
    return o["time"]["ticks"], o["regs"]["PC"]


def run_until(emu, target):
    """run_until_tstate s target_ticks."""
    return emu.call("run_until_tstate", {"target_ticks": target}, timeout=40)


def run(emu):
    """Scénáře 1-4."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    t0, pc = now_ticks(emu)
    check(pc == PROG, "program at 3000h")
    # Začátky instrukcí: DI t0, INC HL t0+20, JR t0+50, INC HL t0+110, JR t0+140.

    r = run_until(emu, t0 + 110)
    t, pc = now_ticks(emu)
    check(r["reached"] and r["exact"] and r["ticks"] == t0 + 110,
          f"1: exact stop at an instruction start ({r})")
    check(t == r["ticks"] and pc == 0x3001, f"1: state_observe agrees (ticks {t}, PC {pc:04X})")
    check(r["stopped_by"] == "run_until_ticks", "1: stopped_by run_until_ticks")

    r = run_until(emu, t + 1)
    t2, pc = now_ticks(emu)
    check(r["reached"] and not r["exact"] and r["ticks"] == t + 6 * DIV,
          f"2: target inside INC HL stops at the next start ({r})")
    check(t2 == r["ticks"] and pc == 0x3002, f"2: PC of JR (PC {pc:04X})")

    r = run_until(emu, t2)
    check(r["reached"] and r["exact"] and r["delta_ticks"] == 0,
          f"3: target now = reached without running ({r})")

    bp = emu.call("bp_create_with_init", {"fields": ["type", "addr", "enabled"],
                                          "type": "PC_EXEC", "addr": 0x3002, "enabled": True})["id"]
    r = run_until(emu, t2 + 100000)
    t3, pc = now_ticks(emu)
    check(not r["reached"] and r["stopped_by"] == "breakpoint" and pc == 0x3002,
          f"4: a user breakpoint stops earlier ({r}, PC {pc:04X})")
    emu.call("bp_remove", {"id": bp})
    r = run_until(emu, t3 + 1000)
    check(r["reached"] and r["ticks"] >= t3 + 1000 and r["ticks"] - (t3 + 1000) < 12 * DIV,
          f"4: run until time works after the breakpoint ({r})")


def main():
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_run_until_ticks_"))
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
