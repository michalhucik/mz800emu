#!/usr/bin/env python3
"""E2E regresní test: snapshot obnoví latch WAIT VRAM režimu MZ-700 (MCP pipe).

V režimu MZ-700 je první zápis CPU do VRAM ve viditelné části řádku bez
WAIT, každý další na stejném řádku s WAIT. Buildy MZ-700 a MZ-1500 to dřív
držely v latchi ``mz700_wr_latch_is_used`` (nuluje ho konec viditelné části),
MZ-800 v modelu WAIT u VRAM (``g_gdg.vram_wrff_release``: zápis jde přes
buffer GDG a do DRAM se dostane až po konci viditelné části). Snapshot
v buildu MZ-700 latch neukládal (prázdný handler VRAM Control): po načtení
snapshotu uloženého hned po prvním přístupu dostal druhý přístup WAIT podle
stroje, který se přepisoval.

MZ-1500 a MZ-700 latch už nepoužívají: podle měření na skutečném MZ-1500
(2026-11-24) čeká každý přístup do VRAM v okně řádku, i první; MZ-700 to přebírá
bez vlastního měření. Tam test kontroluje opak - první zápis s WAIT, druhý (už za
koncem okna) bez WAIT - a shodu po načtení snapshotu. Latch platí dál na MZ-800.

Pro každou binárku v kořeni repa (nebo v ``MZ_EMU_DIR``):

  1  ``run_until_raster`` na řádek 100 do zatemněné části za koncem
     viditelné části (latch je tam vynulovaný), program ``DI``, 24x ``NOP``
     (doběh do viditelné části řádku 101), ``LD A,0; LD (0D000h),A;
     LD (0D001h),A; JR $``; krok přes první zápis do VRAM (bez WAIT,
     latch = 1), ``snapshot_save``;
  2  A: krok přes druhý zápis, délka kroku v T-stavech (``total_cycles``)
     musí obsahovat WAIT;
  3  B: "špinavý" stroj (reset, 3 snímky), ``snapshot_load``, tentýž krok;
  4  délky A a B se musí shodovat (dřív v buildech MZ-700 B bez WAIT).

Každý proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, prázdné
INI, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Binárky a sloupec startu programu: zatemněná část řádku za koncem
#: viditelné části (HBLN_START: MZ-700 a MZ-1500 sloupec 668, MZ-800
#: sloupec 790), odkud 24 NOP (96 T) doběhne do viditelné části dalšího
#: řádku (MZ-700 PAL a MZ-800 1136 sloupců na řádek, 5 na T-stav;
#: MZ-700 NTSC a MZ-1500 912 sloupců, 4 na T-stav). Třetí položka: True =
#: první přístup v řádku je bez WAIT a další čeká (latch, MZ-800); False = čeká
#: každý přístup v okně řádku (MZ-1500 a MZ-700).
_BINARIES = [("mz700emu-pal", 700, False), ("mz700emu-ntsc", 700, False),
             ("mz800emu", 960, True), ("mz1500emu", 700, False)]

#: DI; 24x NOP; LD A,0; LD (0D000h),A; LD (0D001h),A; JR $ na 3000h.
_PROG = 0x3000
_NOPS = 24
_CODE = bytes.fromhex("F3" + "00" * _NOPS + "3E00" "3200D0" "3201D0" "18FE")
#: Adresa prvního zápisu do VRAM.
_WR1 = _PROG + 1 + _NOPS + 2
#: Délka LD (nn),A bez WAIT.
_LD_NN_A_T = 13

#: Řádek startu programu.
_LINE = 100


def _cycles(emu):
    """total_cycles CPU."""
    return emu.call("get_cpu_flags")["total_cycles"]


def _write_step(emu, next_pc):
    """Krok přes zápis do VRAM; vrátí jeho délku v T-stavech."""
    c0 = _cycles(emu)
    emu.call("step_into")
    check(emu.call("get_registers")["PC"] == next_pc, f"VRAM write executed (PC {next_pc:04X}h)")
    return _cycles(emu) - c0


def _second_write(emu):
    """Krok přes druhý zápis do VRAM; vrátí jeho délku v T-stavech."""
    return _write_step(emu, _WR1 + 6)


def run_platform(exe, col, latch, tmp):
    """Scénář pro jednu binárku; latch viz _BINARIES."""
    snap = tmp / "latch.mzs"
    emu = PipeEmu(exe, tmp)
    try:
        emu.call("pause")
        emu.call("run_until_raster", {"line": _LINE, "col": col, "max_cycles": 400000})
        emu.call("mem_write", {"addr": _PROG, "data_hex": _CODE.hex()})
        emu.call("set_register", {"reg": "PC", "value": _PROG})
        for _ in range(1 + _NOPS + 1):  # DI, NOP, LD A,0
            emu.call("step_into")
        first = _write_step(emu, _WR1 + 3)
        if latch:
            check(first == _LD_NN_A_T, f"first VRAM write in the visible part without WAIT ({first} T)")
        else:
            check(first > _LD_NN_A_T, f"first VRAM write in the visible part with WAIT ({first} T)")
        emu.call("snapshot_save", {"path": str(snap)})
        a = _second_write(emu)
        if latch:
            check(a > _LD_NN_A_T, f"second VRAM write in continuous run with WAIT ({a} T)")
        else:
            check(a == _LD_NN_A_T, f"second VRAM write after the end of the window without WAIT ({a} T)")
        emu.call("reset")
        emu.call("pause")
        emu.call("run", {"frames": 3})
        emu.call("pause")
        emu.call("snapshot_load", {"path": str(snap)})
        b = _second_write(emu)
        check(a == b, f"second VRAM write after load takes the same T-states ({b} == {a})")
    finally:
        emu.close()


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=170)
    found = [(n, c, l, _find_exe(n)) for n, c, l in _BINARIES]
    found = [(n, c, l, e) for n, c, l, e in found if e]
    if not found:
        print("SKIP: no emulator binary found")
        return 77
    failed = []
    for name, col, latch, exe in found:
        print(f"== {name} ({exe})")
        tmp = Path(tempfile.mkdtemp(prefix="mz_snap_latch_"))
        try:
            run_platform(exe, col, latch, tmp)
            shutil.rmtree(tmp, ignore_errors=True)
        except TestFailure as e:
            print(f"FAIL ({name}): {e}; artifacts kept in {tmp}")
            failed.append(name)
    if failed:
        print(f"FAIL: {', '.join(failed)}")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
