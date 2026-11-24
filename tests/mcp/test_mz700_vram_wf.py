#!/usr/bin/env python3
"""E2E test: zápis do VRAM v režimu MZ-700 na MZ-800 jde přes registr WF (MCP pipe).

SM800 předepisuje v režimu MZ-700 WF = 01h ("Writes WD into the DATA, ATB,
and CG area"); skutečné MZ-800 zapisuje znaky a atributy na D000h jako do
roviny I podle registru formátu zápisu WF. Program, který nechá WF v režimu
XOR, na stroji nechá na obrazovce starý obraz (Unicard Manager 3.0 smazal
obrazovku a napsal LOADING před nastavením WF a zůstal nepořádek, 2026-11-12).

Případy jsou z programu tests/data/mz700_vram_wf/wftest.mzf a hodnoty RD jsou
naměřené na skutečném MZ-800 (2026-11-24, všech 12 shodně s tímto modelem):
buňka na D000h dostane přes WF 01h hodnotu PR, pak se do ní přes zkoušený WF
zapíše NW a přes WF 01h a RF 01h se přečte. Režimy WF (bity 7-5): 000 SINGLE,
001 XOR, 010 OR, 011 RESET, 10x REPLACE, 11x PSET; rovina I vybraná (xx1h)
a nevybraná (jen rovina II, xx2h): SINGLE, XOR, OR a RESET pak nezapíšou nic,
REPLACE zapíše 0, PSET smaže bity NW (jako nevybrané roviny v režimu MZ-800).

Jen MZ-800. Binárka se hledá v kořeni repa nebo v adresáři z proměnné
``MZ_EMU_DIR``. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import base64
import gettext
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
import test_gdg_cksw as h  # noqa: E402 - PipeEmu a příprava stroje

_ = gettext.gettext

#: Adresa testovacího kódu a výsledku.
_CODE = 0x5200
_RESULT = 0x6000

#: (WF, PR, NW, RD na skutečném MZ-800), jako wftest.
_CASES = (
    (0x01, 0x04, 0x0F, 0x0F),   # SINGLE  I
    (0x21, 0x0B, 0x04, 0x0F),   # XOR     I
    (0x41, 0x0B, 0x04, 0x0F),   # OR      I
    (0x61, 0x1F, 0x10, 0x0F),   # RESET   I
    (0x81, 0x04, 0x0F, 0x0F),   # REPLACE I
    (0xC1, 0x0B, 0x04, 0x0F),   # PSET    I
    (0x02, 0x0F, 0x04, 0x0F),   # SINGLE  II: nezapíše
    (0x22, 0x0F, 0x04, 0x0F),   # XOR     II: nezapíše
    (0x42, 0x0F, 0x04, 0x0F),   # OR      II: nezapíše
    (0x62, 0x0F, 0x04, 0x0F),   # RESET   II: nezapíše
    (0x82, 0x0F, 0x04, 0x00),   # REPLACE II: 0
    (0xC2, 0x1F, 0x10, 0x0F),   # PSET    II: 1Fh and not 10h
)


def _case(emu, wf, preset, value):
    """V režimu MZ-700: PR přes WF 01h, NW přes zkoušený WF, čtení přes WF a RF 01h."""
    code = bytes([
        0x3E, 0x08, 0xD3, 0xCE,             # LD A,08h; OUT (CEh),A: DMD režim MZ-700
        0xD3, 0xE3,                         # OUT (E3h),A: D000h až FFFFh = VRAM a ROM
        0x3E, 0x01, 0xD3, 0xCC,             # WF 01h
        0x3E, preset, 0x32, 0x00, 0xD0,     # LD A,PR; LD (D000h),A
        0x3E, wf, 0xD3, 0xCC,               # LD A,wf; OUT (CCh),A: zkoušený WF
        0x3E, value, 0x32, 0x00, 0xD0,      # LD A,NW; LD (D000h),A
        0x3E, 0x01, 0xD3, 0xCC, 0xD3, 0xCD,  # WF, RF 01h
        0x3A, 0x00, 0xD0,                   # LD A,(D000h)
        0x32, _RESULT & 0xFF, _RESULT >> 8,  # LD (6000h),A
        0xD3, 0xE1,                         # OUT (E1h),A: D000h až FFFFh = RAM
        0x3E, 0x00, 0xD3, 0xCE,             # DMD 00h
    ])
    emu.call("mem_write", {"addr": _CODE, "data_hex": code.hex() + "18fe"})
    emu.call("set_register", {"reg": "PC", "value": _CODE})
    emu.call("run", {"frames": 2})
    emu.call("set_register", {"reg": "PC", "value": h._LOOP})
    r = emu.call("mem_read", {"addr": _RESULT, "len": 1})
    return base64.b64decode(r.get("data_b64", ""))[0]


def run(exe):
    """Všechny případy na jedné binárce. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="mz700_vram_wf_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        h._setup(emu)
        for wf, preset, value, expected in _CASES:
            got = _case(emu, wf, preset, value)
            print(f"  WF {wf:02X}h, PR {preset:02X}h, NW {value:02X}h: read {got:02X}h"
                  f" (a real MZ-800 {expected:02X}h)")
            h.check(got == expected, f"WF {wf:02X}h as on a real MZ-800")
        return True
    except h.TestFailure as e:
        print(f"  FAILED: {e}")
        return False
    finally:
        if emu is not None:
            emu.close()


def main():
    """Vstupní bod: najde binárku MZ-800 a spustí test."""
    emu_test_proc.install(deadline_s=120)
    exe = h._find_exe("mz800emu")
    if exe is None:
        print(_("SKIP: mz800emu binary not found"))
        return 77
    ok = run(exe)
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
