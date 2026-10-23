#!/usr/bin/env python3
"""E2E test: pauza a rozběh nemění okamžik přijetí přerušení (pipe).

Výstup z pauzy přeplánoval další událost a zahodil kontrolu čekajícího
přerušení po příští instrukci. Pauza mezi EI a HALT při trvale aktivní
lince INT pak posunula přijetí přerušení až k další skutečné události a
běh se časově rozešel s během bez pauzy.

Program (MZ-800): trvalé INT z CTC2 v IM 2 (vzor test_trace_read.py),
smyčka ``LD HL,4000h; LD DE,4100h; LD BC,10h; LDIR; EI; HALT; JR``,
obsluha ``INC D; RETI``. Postup:

  A  ze snapshotu běh do času T (run_until_tstate target_ticks) bez
     zastavení, registry,
  B  ze stejného snapshotu zastavení na několika začátcích HALT hned po
     EI (časy z trace cputrack běhu A) a pokračování do T, registry.

Registry v A a B se musí shodovat (D = počet obsluh přerušení, R, PC).

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
s adresářem cputrack, ``--no-save-ini``); zabíjí se jen vlastní PID.
Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

PROG = 0x3000
#: DI; IM 2; I = 32h; CT1 režim 2 N = 2, CT2 režim 0 N = 1 (trvalé INT,
#: viz test_trace_read.py); DJNZ; pak smyčka od 3031h:
#: LD HL,4000h; LD DE,4100h; LD BC,10h; LDIR; EI; HALT (303Dh); JR 3031h.
PROG_HEX = ("F3" "ED5E" "3E32" "ED47"
            "3E04" "3203E0"
            "3E74" "3207E0" "3E02" "3205E0" "AF" "3205E0"
            "3EB0" "3207E0" "3E01" "3206E0" "AF" "3206E0"
            "3E05" "3203E0"
            "0600" "10FE"
            "210040" "110041" "011000" "EDB0" "FB" "76" "18F1")
#: Obsluha na 3434h: INC D; RETI. Tabulka IM 2 3200h-3300h -> 3434h.
ISR_HEX = "14" "ED4D"


def regs_at(emu, t):
    """Dojede na čas t a vrátí (čas, registry)."""
    r = emu.call("run_until_tstate", {"target_ticks": t}, timeout=40)
    check(r["reached"], f"run until {t} reached ({r.get('stopped_by')})")
    o = emu.call("state_observe", {"windows": []})
    return o["time"]["ticks"], o["regs"]


def run(emu):
    """Běh A a B ze stejného snapshotu."""
    emu.call("pause")
    emu.call("mem_write", {"addr": 0x3200, "data_hex": "34" * 257})
    emu.call("mem_write", {"addr": 0x3434, "data_hex": ISR_HEX})
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {"frames": 1})
    snap = emu.call("snapshot_save_buffer", {"description": "start"})["bytes_b64"]
    t0 = emu.call("state_observe", {"windows": []})["time"]["ticks"]

    # A: bez zastavení, s trace pro časy HALT po EI.
    emu.call("trace_start", {"channel": "cputrack"})
    t_end = t0 + 200000
    ta, ra = regs_at(emu, t_end)
    emu.call("trace_stop", {"channel": "cputrack"})
    rows = emu.call("trace_read", {"channel": "cputrack", "from": "oldest", "limit": 3000,
                                   "columns": ["asm", "t"]})["rows"]
    halts = [rows[i][1] for i in range(1, len(rows)) if rows[i][0] == "HALT" and rows[i - 1][0] == "EI"]
    check(len(halts) >= 3, f"HALT right after EI found in the trace ({len(halts)})")

    # B: ze stejného snapshotu zastavit na HALT po EI a pokračovat.
    emu.call("snapshot_load_buffer", {"bytes_b64": snap})
    for t in halts[:3]:
        r = emu.call("run_until_tstate", {"target_ticks": t}, timeout=40)
        o = emu.call("state_observe", {"windows": []})
        check(r["exact"] and o["regs"]["PC"] == 0x303D, f"stopped at HALT after EI (t {t}, PC {o['regs']['PC']:04X})")
    tb, rb = regs_at(emu, t_end)
    check(ta == tb and ra == rb,
          f"pauses do not change the run: A {ta} {ra} / B {tb} {rb}")


def main():
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_pause_int_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[TRACE_CPUTRACK]\n"
                       f"dir = {(tmp / 'trace').as_posix()}\nname = ct\nsave_on_exit = 0\n",
                       encoding="utf-8")
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
