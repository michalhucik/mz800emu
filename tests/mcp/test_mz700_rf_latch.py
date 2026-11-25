#!/usr/bin/env python3
"""E2E test: čtení s RF s rovinou II, IV a SEARCH v režimu MZ-700 na MZ-800 (MCP pipe).

Spustí tests/data/mz700_vram_wf/wf700_meas2.mzf. Program naplní VRAM tak, že bajt čtvrtého
video čtení buňky 39 (latch rovin II a IV) je x = (8 * C + r) AND 0FFh ve VA a x XOR 80h
ve VC (C kód znaku buňky 39, r řádek znaku), a čte 0C0FFh (VA 0FFh, VC 7Fh) a 0C0AAh
(VA 0AAh, VC 2Ah) s dvanácti hodnotami RF, čtyřikrát bez synchronizace a dvakrát na
začátku svislého zatemnění, pro text = sloupec (A) a text = textový řádek (B).

Kontroluje:
- RF 01h a 04h čtou buňku adresy (VA, VC);
- každá hodnota odpovídá pravidlu z Doxygenu vramctrl_mz700_read_rf() pro nějaké x
  z povolené množiny (A: x = 38h až 3Fh v canvasu, 78h až 7Fh ve svislém zatemnění;
  B: x = číslo řádku, 00h až C7h v canvasu, 00h až 6Fh ve svislém zatemnění);
- obě sady ve svislém zatemnění jsou shodné;
- ve svislém zatemnění se x každého čtení liší od x změřeného na skutečném MZ-800
  (2026-11-25) nejvýš o 1 řádek (synchronizace programu má rozptyl asi 25 T).

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
import test_gdg_cksw as h  # noqa: E402 - PipeEmu

_ = gettext.gettext

_MZF = Path(__file__).resolve().parent.parent / "data" / "mz700_vram_wf" / "wf700_meas2.mzf"
#: Hodnoty RF v pořadí čtení programu.
_RFS = (0x01, 0x04, 0x02, 0x08, 0x0A, 0x03, 0x0F, 0x80, 0x81, 0x82, 0x84, 0x88)
#: Čtené buňky: (VA, VC).
_CELLS = ((0xFF, 0x7F), (0xAA, 0x2A))
#: Naměřeno na MZ-800 ve svislém zatemnění: [varianta][buňka] = 12 bajtů v pořadí _RFS.
_HW = (
    ((0xFF, 0x7F, 0x7A, 0xFA, 0x7B, 0x7B, 0x7C, 0x00, 0x82, 0x00, 0x00, 0x00),
     (0xAA, 0x2A, 0x78, 0xF8, 0x79, 0x2A, 0x2A, 0x04, 0x80, 0x54, 0x01, 0x00)),
    ((0xFF, 0x7F, 0x02, 0x82, 0x03, 0x03, 0x04, 0x00, 0xFA, 0x00, 0x00, 0x00),
     (0xAA, 0x2A, 0x08, 0x89, 0x09, 0x0A, 0x0A, 0x54, 0xA0, 0x04, 0x51, 0x50)),
)
#: "ANY KEY" v display kódu na začátku řádku 24 (program doběhl).
_DONE = bytes([0x01, 0x0E, 0x19, 0x00, 0x0B, 0x05, 0x19])


def _rule(rf, va, vc, x2, x4):
    """Bajt čtení podle pravidla vramctrl_mz700_read_rf()."""
    if not rf & 0x80:
        v = 0xFF
        for bit, b in ((0x01, va), (0x02, x2), (0x04, vc), (0x08, x4)):
            if rf & bit:
                v &= b
        return v
    if not rf & 0x10:
        return ((va if rf & 1 else ~va) & (x2 if rf & 2 else ~x2)) & 0xFF
    return ((vc if rf & 4 else ~vc) & (x4 if rf & 8 else ~x4)) & 0xFF


def _candidates(rf, cell, value, allowed):
    """Množina x z ``allowed``, pro které pravidlo dá ``value``."""
    va, vc = cell
    return {x for x in allowed if _rule(rf, va, vc, x, x ^ 0x80) == value}


def _mem(emu, addr, length):
    """Přečte blok paměti po částech 256 bajtů."""
    out = bytearray()
    while length:
        n = min(256, length)
        r = emu.call("mem_read", {"addr": addr, "len": n})
        out += base64.b64decode(r.get("data_b64", ""))
        addr += n
        length -= n
    return bytes(out)


def _xdist(a, b, var):
    """Vzdálenost dvou x v řádcích (A: řádek znaku mod 8, B: číslo řádku)."""
    if var == 0:
        d = (a - b) & 7
        return min(d, 8 - d)
    return abs(a - b)


def run(exe):
    """Spustí program a zkontroluje výsledky. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="mz700_rf_latch_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        emu.call("pause")
        emu.call("media_run_mzf", {"path": str(_MZF)})
        emu.call("set_speed", {"mode": "max"})
        emu.call("run", {"frames": 300})
        h.check(_mem(emu, 0xD000 + 24 * 40, 7) == _DONE, "the program finished (ANY KEY on the last line)")
        res = _mem(emu, 0x6000, 14 * 32)
        for var, name in enumerate(("text = column", "text = row")):
            if var == 0:
                canvas = set(range(0x38, 0x40)) | set(range(0x78, 0x80))
                blank = set(range(0x78, 0x80))
            else:
                canvas = set(range(0x00, 0xC8))
                blank = set(range(0x00, 0x70))
            bad = []
            for s in range(6):
                for ai, cell in enumerate(_CELLS):
                    for ri, rf in enumerate(_RFS):
                        v = res[(var * 8 + s) * 32 + ai * 12 + ri]
                        if rf == 0x01:
                            ok = v == cell[0]
                        elif rf == 0x04:
                            ok = v == cell[1]
                        else:
                            ok = bool(_candidates(rf, cell, v, blank if s >= 4 else canvas))
                        if not ok:
                            bad.append(f"set {s} cell {cell[0]:02X}h RF {rf:02X}h read {v:02X}h")
            h.check(not bad, f"{name}: all 144 reads fit the rule ({', '.join(bad[:4])})")
            b4 = res[(var * 8 + 4) * 32:(var * 8 + 4) * 32 + 24]
            b5 = res[(var * 8 + 5) * 32:(var * 8 + 5) * 32 + 24]
            h.check(b4 == b5, f"{name}: both read sets in the vertical blanking are the same")
            far = []
            for ai, cell in enumerate(_CELLS):
                for ri, rf in enumerate(_RFS):
                    if rf in (0x01, 0x04):
                        continue
                    ce = _candidates(rf, cell, b4[ai * 12 + ri], blank)
                    ch = _candidates(rf, cell, _HW[var][ai][ri], blank)
                    if not any(_xdist(a, b, var) <= 1 for a in ce for b in ch):
                        far.append(f"cell {cell[0]:02X}h RF {rf:02X}h read {b4[ai * 12 + ri]:02X}h, "
                                   f"a real MZ-800 {_HW[var][ai][ri]:02X}h")
            h.check(not far, f"{name}: blanking reads at most 1 line from a real MZ-800 ({', '.join(far[:4])})")
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
