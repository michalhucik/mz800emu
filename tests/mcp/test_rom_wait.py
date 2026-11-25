#!/usr/bin/env python3
"""E2E test čekacího taktu při čtení z monitorové ROM na MZ-700, MZ-1500 a MZ-800 (MCP pipe).

Změřeno logickým analyzátorem na skutečných strojích:
  - MZ-700 PAL (2026-11-24, emu-experiments mz700-meas, VYSLEDKY4.md až VYSLEDKY6.md) a
    MZ-1500 (2026-11-25, mz1500-meas, VYSLEDKY6.md): když je spodní ROM připojená, trvá každý
    čtecí cyklus procesoru (M1, operand, data) z adres 0000h až 0FFFh o 1 T déle. Zápis na
    tyto adresy, RAM připojená na jejich místo (OUT 0E0h), adresy od 1000h a oblast 0E800h
    až 0FFFFh nečekají. Na obou strojích to platí i bez karty Unicard.
  - MZ-800 (2026-11-25, mz800-meas, VYSLEDKY1.md): čtení z ROM ani CG-ROM nečeká, v režimu
    MZ-700 ani MZ-800.

Metoda je stejná jako na stroji: program v RAM (2000h) dá kolem měřeného kódu dvě značky
IN A,(0A0h); odstup jejich řádků v kanálu iorqlog je 11 T + délka kódu + čekací takty.
Počet taktů GDGCLK na T se bere z odstupu u kódu bez čekání (tři NOP, 23 T).

Kroky pro mz700emu-pal, mz700emu-ntsc (NTSC má pravidlo převzaté z měření PAL) a mz1500emu:
  1  NOP, NOP, NOP: 23 T (určí takty na T)
  2  čtení RAM 3000h: bez čekání
  3  čtení ROM 0000h, 0010h a 0FFFh: +1 T
  4  čtení 1000h, 0E800h a 0F000h: bez čekání
  5  zápis na 0010h: bez čekání
  6  CALL na bajt RET nalezený v ROM: +1 T (jeden cyklus M1 z ROM)
  7  po OUT 0E0h (RAM na místě ROM): čtení 0010h a CALL na RET v RAM bez čekání
  8  po OUT 0E4h: čtení 0010h zase +1 T
Kroky pro mz800emu v režimu MZ-800 a MZ-700 (DMD 00h a 08h): čtení ROM 0010h, CALL na RET
v ROM a čtení horní ROM 0F000h bez čekání.

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí se jen
vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (nenalezena žádná binárka).
"""

import base64
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, check, _find_exe  # noqa: E402

#: Adresa programu v RAM.
_PROG = 0x2000
#: Délka značky a jmenovité délky měřeného kódu v T.
_MARK_T = 11
_NOP3_T = 12
_LD_T = 15          # LD A,(HL) nebo LD (HL),C a dva NOP
_CALL_RET_T = 27    # CALL nn a RET
#: Měřený kód: čtení a zápis přes HL.
_RD = [0x7E, 0x00, 0x00]
_WR = [0x71, 0x00, 0x00]


def _program(slot):
    """Smyčka: značka, tři bajty měřeného kódu, značka, pauza, skok zpět."""
    code = bytes([
        0xDB, 0xA0,             # in a,(0A0h)
        *slot,                  # měřený kód (3 bajty)
        0xDB, 0xA0,             # in a,(0A0h)
        0x06, 0x0E,             # ld b,14
        0x10, 0xFE,             # djnz
        0xC3, _PROG & 0xFF, _PROG >> 8,
    ])
    return code.hex()


def _distance(emu, slot, hl):
    """Pustí smyčku s měřeným kódem slot a vrátí četnost odstupu značek v taktech GDGCLK."""
    emu.call("mem_write", {"addr": _PROG, "data_hex": _program(slot)})
    emu.call("set_register", {"reg": "HL", "value": hl})
    emu.call("set_register", {"reg": "BC", "value": 0x24})
    emu.call("set_register", {"reg": "SP", "value": 0x7F00})
    emu.call("set_register", {"reg": "PC", "value": _PROG})
    emu.call("trace_start", {"channel": "iorqlog"})
    emu.call("run", {"frames": 1})
    emu.call("trace_stop", {"channel": "iorqlog"})
    rows, cur = [], None
    while True:
        req = {"channel": "iorqlog", "limit": 4000}
        if cur is not None:
            req["cursor"] = cur
        pg = emu.call("trace_read", req)
        cols = pg["columns"]
        rows += [dict(zip(cols, r)) for r in pg.get("rows", [])]
        cur = pg.get("next_cursor")
        if pg.get("end_of_data") or cur is None:
            break
    emu.call("trace_reset", {"channel": "iorqlog"})
    marks = [r["t"] for r in rows if r["addr"].upper().endswith("A0") and r["type"].startswith("iorq")]
    # stopa začíná na začátku programu, první značka je tedy první značka průchodu
    return Counter(b - a for a, b in zip(marks[0::2], marks[1::2]))


def _start(exe, tmp):
    """Spustí emulátor: výchozí mapa paměti, DI, nejvyšší rychlost."""
    emu = PipeEmu(exe, tmp)
    emu.call("pause")
    emu.call("io_write", {"port": 0xE4, "value": 0})
    emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
    emu.call("set_speed", {"mode": "max"})
    return emu


def _per_t(emu):
    """Z odstupu značek kolem tří NOP (23 T) určí počet taktů GDGCLK na T."""
    dist = _distance(emu, [0x00, 0x00, 0x00], 0x3000)
    check(len(dist) == 1 and sum(dist.values()) >= 50, f"NOP x 3: one marker distance ({dict(dist)})")
    per_t, rest = divmod(next(iter(dist)), _MARK_T + _NOP3_T)
    check(rest == 0 and per_t in (4, 5), f"NOP x 3: {_MARK_T + _NOP3_T} T, {per_t} GDGCLK ticks per T")
    return per_t


def _expect(emu, what, slot, hl, nominal_t, wait_t, per_t):
    """Zkontroluje, že odstup značek je (11 + nominal_t + wait_t) T ve všech průchodech."""
    dist = _distance(emu, slot, hl)
    want = (_MARK_T + nominal_t + wait_t) * per_t
    check(sum(dist.values()) >= 50 and set(dist) == {want},
          f"{what}: marker distance is {_MARK_T + nominal_t} T + {wait_t} T wait "
          f"({ {k / per_t: v for k, v in dist.items()} })")


def _rom_ret(emu):
    """Adresa prvního bajtu RET (0C9h) v ROM 0100h až 0FFFh."""
    st = emu.call("state_observe", {"windows": [{"addr": 0x0100, "len": 0x0F00}]})
    rom = base64.b64decode(st["windows"][0]["data_b64"])
    return 0x0100 + rom.index(0xC9)


def run_waiting(exe):
    """Kroky 1 až 8 pro stroj s čekacím taktem u ROM (MZ-700, MZ-1500). Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    emu = None
    try:
        emu = _start(exe, Path(tempfile.mkdtemp(prefix="rom_wait_")))
        per_t = _per_t(emu)
        _expect(emu, "read RAM 3000h", _RD, 0x3000, _LD_T, 0, per_t)
        for addr in (0x0000, 0x0010, 0x0FFF):
            _expect(emu, f"read ROM {addr:04X}h", _RD, addr, _LD_T, 1, per_t)
        for addr in (0x1000, 0xE800, 0xF000):
            _expect(emu, f"read {addr:04X}h", _RD, addr, _LD_T, 0, per_t)
        _expect(emu, "write to ROM address 0010h", _WR, 0x0010, _LD_T, 0, per_t)

        ret = _rom_ret(emu)
        call = [0xCD, ret & 0xFF, ret >> 8]
        _expect(emu, f"CALL {ret:04X}h (RET in ROM)", call, 0x3000, _CALL_RET_T, 1, per_t)

        emu.call("io_write", {"port": 0xE0, "value": 0})
        emu.call("mem_write", {"addr": ret, "data_hex": "c9"})
        _expect(emu, "OUT 0E0h, read RAM at 0010h", _RD, 0x0010, _LD_T, 0, per_t)
        _expect(emu, f"OUT 0E0h, CALL {ret:04X}h (RET in RAM)", call, 0x3000, _CALL_RET_T, 0, per_t)
        emu.call("io_write", {"port": 0xE4, "value": 0})
        _expect(emu, "OUT 0E4h, read ROM 0010h", _RD, 0x0010, _LD_T, 1, per_t)
        return True
    except TestFailure:
        return False
    finally:
        if emu is not None:
            emu.close()


def run_mz800(exe):
    """MZ-800: čtení z ROM bez čekání v režimu MZ-800 i MZ-700. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    emu = None
    try:
        emu = _start(exe, Path(tempfile.mkdtemp(prefix="mz800_rom_wait_")))
        for dmd, mode in ((0x00, "MZ-800"), (0x08, "MZ-700")):
            emu.call("io_write", {"port": 0xCE, "value": dmd})
            emu.call("io_write", {"port": 0xE4, "value": 0})
            per_t = _per_t(emu)
            _expect(emu, f"{mode} mode, read ROM 0010h", _RD, 0x0010, _LD_T, 0, per_t)
            _expect(emu, f"{mode} mode, read upper ROM 0F000h", _RD, 0xF000, _LD_T, 0, per_t)
            ret = _rom_ret(emu)
            _expect(emu, f"{mode} mode, CALL {ret:04X}h (RET in ROM)", [0xCD, ret & 0xFF, ret >> 8],
                    0x3000, _CALL_RET_T, 0, per_t)
        return True
    except TestFailure:
        return False
    finally:
        if emu is not None:
            emu.close()


def main():
    emu_test_proc.install(deadline_s=170)
    ok, found = True, 0
    for name, fn in (("mz700emu-pal", run_waiting), ("mz700emu-ntsc", run_waiting),
                     ("mz1500emu", run_waiting), ("mz800emu", run_mz800)):
        exe = _find_exe(name)
        if exe is None:
            print(f"\n=== {name}: binary not found, skipped ===")
            continue
        found += 1
        ok = fn(exe) and ok
    if not found:
        print("SKIP: no emulator binary found")
        return 77
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
