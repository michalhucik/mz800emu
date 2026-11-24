#!/usr/bin/env python3
"""E2E test čekání procesoru u VRAM a rovin PCG a polohy obrazu v řádku na MZ-1500 a MZ-700 (MCP pipe).

Hodnoty jsou z měření na skutečném MZ-1500 (2026-11-24, emu-experiments mz1500-meas,
VYSLEDKY1.md až VYSLEDKY3.md): řádek 912 taktů GDGCLK, obraz 195 až 835 taktů od
sestupné hrany HS, každý přístup do VRAM v okně řádku čeká do konce okna (i první v řádku
a i v řádcích mimo obraz), rovina PCG čeká o 5 T déle.

Program v RAM (2000h) je smyčka dlouhá 229 T (řádek má 228 T, přístup se tedy každý řádek
posune o 1 T): IN A,(0A0h) jako značka v kanálu iorqlog a hned za ním jeden přístup na
adresu v HL. Na stroji trvá průchod bez čekání 916 taktů, průchod s čekáním 1584 (VRAM)
a 1604 (rovina PCG); značka čekajícího průchodu je 133 taktů po sestupné hraně HS
(v mřížce emulátoru 131) a značka následujícího průchodu 805 (803), u PCG 825 (823).

  1  get_platform_info: řádek 65 + 98 + 704 + 45 = 912 (obraz 195 taktů za hranou HS)
  2  čtení 0D000h: průchody jen 916 a 1584, čekající značka 131, další 803
  3  čekání nastává i v řádcích mimo canvas
  4  zápis na 0D800h (atributy): totéž co krok 2
  5  zápis na 0D000h s rovinou PCG 1 (OUT 0E5h = 1): průchody 916 a 1604, další značka 823
  6  čtení 3000h (RAM): žádné čekání
  7  mz700emu-ntsc (hodnoty převzaté z MZ-1500, na MZ-700 neměřeno): kroky 1, 2, 3 a 6
     se stejnými čísly; get_platform_info hlásí dělič CTC0 16
  8  mz700emu-pal (poloha obrazu podle MZ-800, okno podle MZ-1500, neměřeno): řádek
     80 + 228 + 704 + 124, čtení 0D000h má průchody s čekáním i v řádcích mimo canvas,
     čtení RAM je bez čekání

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí se jen
vlastní PID. Výstup anglicky.

Chybějící binárky MZ-700 se přeskočí.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz1500emu).
"""

import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, check, _find_exe  # noqa: E402

#: Adresa programu v RAM.
_PROG = 0x2000
#: Délka řádku a průchodu bez čekání v taktech GDGCLK (229 T po 4 taktech).
_LINE = 912
_PASS = 916
#: Průchod s čekáním u VRAM a u roviny PCG (změřeno na stroji).
_PASS_VRAM = 1584
_PASS_PCG = 1604
#: Sloupec sestupné hrany HS v řádku emulátoru: 704 + přední zatemnění 45.
_HS_COL = 749
#: Kanál iorqlog zapisuje konec V/V cyklu; začátek pulzu /IORQ je o 3 T dřív.
_IORQ_BEFORE_END = 12
#: Značka čekajícího a následujícího průchodu v mřížce emulátoru (stroj 133, 805 a 825).
_MARK_WAIT = 131
_MARK_NEXT_VRAM = 803
_MARK_NEXT_PCG = 823
#: První a poslední řádek canvasu v řádcích snímku emulátoru.
_CANVAS_ROWS = (16, 215)
#: Počet snímků jednoho měření (1572 řádků, asi 25 čekání; stopa se nezaplní).
_FRAMES = 6


#: Řádek a průchod 229 T v sestavení PAL (5 taktů na T).
_LINE_PAL = 1136
_PASS_PAL = 1145


def _program(opcode):
    """Smyčka 229 T: značka, přístup instrukcí s kódem opcode (7Eh čtení, 71h zápis C)."""
    code = bytes([
        0xDB, 0xA0,             # in a,(0A0h)   11 T
        opcode,                 # ld a,(hl) / ld (hl),c   7 T
        0x00, 0x00, 0x00, 0x00, # 4 x nop       16 T
        0x3E, 0x00, 0x3E, 0x00, # 2 x ld a,0    14 T
        0x06, 0x0D,             # ld b,13        7 T
        0x10, 0xFE,             # djnz           164 T
        0xC3, _PROG & 0xFF, _PROG >> 8,  # jp   10 T
    ])
    return code.hex()


def _measure(emu, opcode, addr, value=0, line=_LINE, hs_col=_HS_COL, before_end=_IORQ_BEFORE_END):
    """Pustí smyčku a vrátí seznam (sloupec značky, délka průchodu, sloupec další značky, řádek).

    line je délka řádku, hs_col sloupec sestupné hrany HS v řádku emulátoru a before_end
    odstup začátku pulzu /IORQ od konce V/V cyklu (3 T) v taktech GDGCLK.
    """
    emu.call("mem_write", {"addr": _PROG, "data_hex": _program(opcode)})
    emu.call("set_register", {"reg": "HL", "value": addr})
    emu.call("set_register", {"reg": "BC", "value": value})
    emu.call("set_register", {"reg": "PC", "value": _PROG})
    emu.call("run", {"frames": 2})
    emu.call("trace_start", {"channel": "iorqlog"})
    emu.call("run", {"frames": _FRAMES})
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
    marks = [r for r in rows if r["addr"].upper().endswith("A0") and r["type"].startswith("iorq")]
    out = []
    for a, b in zip(marks, marks[1:]):
        col_a = (a["pos"] % line - before_end - hs_col) % line
        col_b = (b["pos"] % line - before_end - hs_col) % line
        out.append((col_a, b["t"] - a["t"], col_b, a["pos"] // line))
    return out


def _check_wait(passes, long_len, next_col, what):
    """Společné kontroly: délky průchodů, značka čekajícího a následujícího průchodu."""
    lens = Counter(p[1] for p in passes)
    check(len(passes) > 1000, f"{what}: {len(passes)} passes recorded")
    check(set(lens) == {_PASS, long_len},
          f"{what}: pass lengths are {_PASS} and {long_len} ({dict(lens)})")
    waits = [p for p in passes if p[1] == long_len]
    check(len(waits) >= 15, f"{what}: {len(waits)} waiting passes")
    check(all(p[0] == _MARK_WAIT for p in waits),
          f"{what}: waiting marker column is {_MARK_WAIT} ({sorted(set(p[0] for p in waits))})")
    check(all(p[2] == next_col for p in waits),
          f"{what}: marker after the wait is at column {next_col} ({sorted(set(p[2] for p in waits))})")
    return waits


def run(exe):
    """Všechny kroky testu. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="mz1500_vram_wait_"))
    emu = None
    try:
        emu = PipeEmu(exe, tmp)
        emu.call("pause")
        emu.call("io_write", {"port": 0xE4, "value": 0})
        emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
        emu.call("set_speed", {"mode": "max"})

        # 1 - rozložení řádku
        sl = emu.call("get_platform_info")["scanline"]
        got = (sl["h_sync_ticks"], sl["h_back_porch_ticks"], sl["display_width"], sl["h_front_porch_ticks"])
        check(got == (65, 98, 704, 45), f"line layout is 65 + 98 + 704 + 45 ({got})")
        check(sl["h_sync_ticks"] + sl["h_back_porch_ticks"] + sl["border_left_width"] == 195,
              "picture starts 195 ticks after the HS falling edge")

        # 2 a 3 - čtení znaků
        waits = _check_wait(_measure(emu, 0x7E, 0xD000), _PASS_VRAM, _MARK_NEXT_VRAM, "read 0D000h")
        outside = [p for p in waits if not _CANVAS_ROWS[0] <= p[3] % 262 <= _CANVAS_ROWS[1]]
        check(len(outside) >= 2, f"read 0D000h: {len(outside)} waits in rows outside the canvas")

        # 4 - zápis atributů
        _check_wait(_measure(emu, 0x71, 0xD800, 0x70), _PASS_VRAM, _MARK_NEXT_VRAM, "write 0D800h")

        # 5 - rovina PCG
        emu.call("io_write", {"port": 0xE5, "value": 1})
        _check_wait(_measure(emu, 0x71, 0xD000, 0x00), _PASS_PCG, _MARK_NEXT_PCG, "write PCG plane 1")
        emu.call("io_write", {"port": 0xE6, "value": 0})

        # 6 - RAM
        passes = _measure(emu, 0x7E, 0x3000)
        lens = Counter(p[1] for p in passes)
        check(len(passes) > 1000 and set(lens) == {_PASS}, f"read 3000h: no wait ({dict(lens)})")
        return True
    except TestFailure:
        return False
    finally:
        if emu is not None:
            emu.close()


def _start(exe, tmp):
    """Spustí emulátor, výchozí mapa paměti, DI, nejvyšší rychlost."""
    emu = PipeEmu(exe, tmp)
    emu.call("pause")
    emu.call("io_write", {"port": 0xE4, "value": 0})
    emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
    emu.call("set_speed", {"mode": "max"})
    return emu


def run_mz700_ntsc(exe):
    """Krok 7: MZ-700 NTSC má stejná čísla jako MZ-1500. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    emu = None
    try:
        emu = _start(exe, Path(tempfile.mkdtemp(prefix="mz700n_vram_wait_")))
        info = emu.call("get_platform_info")
        sl = info["scanline"]
        got = (sl["h_sync_ticks"], sl["h_back_porch_ticks"], sl["display_width"], sl["h_front_porch_ticks"])
        check(got == (65, 98, 704, 45), f"line layout is 65 + 98 + 704 + 45 ({got})")
        div = info["clocks"]["ctc0_divider"]
        check(div == 16, f"CTC0 divider is 16 ({div})")
        waits = _check_wait(_measure(emu, 0x7E, 0xD000), _PASS_VRAM, _MARK_NEXT_VRAM, "read 0D000h")
        outside = [p for p in waits if not _CANVAS_ROWS[0] <= p[3] % 262 <= _CANVAS_ROWS[1]]
        check(len(outside) >= 2, f"read 0D000h: {len(outside)} waits in rows outside the canvas")
        lens = Counter(p[1] for p in _measure(emu, 0x7E, 0x3000))
        check(set(lens) == {_PASS}, f"read 3000h: no wait ({dict(lens)})")
        return True
    except TestFailure:
        return False
    finally:
        if emu is not None:
            emu.close()


def run_mz700_pal(exe):
    """Krok 8: MZ-700 PAL - rozložení řádku a čekání u jediného přístupu v řádku. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    emu = None
    try:
        emu = _start(exe, Path(tempfile.mkdtemp(prefix="mz700p_vram_wait_")))
        sl = emu.call("get_platform_info")["scanline"]
        got = (sl["h_sync_ticks"], sl["h_back_porch_ticks"], sl["display_width"], sl["h_front_porch_ticks"])
        check(got == (80, 228, 704, 124), f"line layout is 80 + 228 + 704 + 124 ({got})")
        hs_col = sl["display_width"] + sl["h_front_porch_ticks"]
        passes = _measure(emu, 0x7E, 0xD000, line=_LINE_PAL, hs_col=hs_col, before_end=15)
        waits = [p for p in passes if p[1] != _PASS_PAL]
        check(len(passes) > 1000 and len(waits) >= 10,
              f"read 0D000h: {len(waits)} waiting passes of {len(passes)}")
        check(all(p[1] > _PASS_PAL for p in waits), "read 0D000h: waiting passes are longer than 229 T")
        ends = sorted(set(p[2] for p in waits))
        # řádek 1136 není násobek průchodu ani T (5 taktů), fáze T-stavů vůči řádku se mění
        check(ends[-1] - ends[0] < 5, f"read 0D000h: all waits end within one T-state ({ends})")
        outside = [p for p in waits if not _CANVAS_ROWS[0] <= p[3] % 312 <= _CANVAS_ROWS[1]]
        check(len(outside) >= 2, f"read 0D000h: {len(outside)} waits in rows outside the canvas")
        lens = Counter(p[1] for p in _measure(emu, 0x7E, 0x3000, line=_LINE_PAL, hs_col=hs_col, before_end=15))
        check(set(lens) == {_PASS_PAL}, f"read 3000h: no wait ({dict(lens)})")
        return True
    except TestFailure:
        return False
    finally:
        if emu is not None:
            emu.close()


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe("mz1500emu")
    if exe is None:
        print("SKIP: no mz1500emu binary found")
        return 77
    ok = run(exe)
    for name, fn in (("mz700emu-ntsc", run_mz700_ntsc), ("mz700emu-pal", run_mz700_pal)):
        other = _find_exe(name)
        if other is None:
            print(f"\n=== {name}: binary not found, skipped ===")
            continue
        ok = fn(other) and ok
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
