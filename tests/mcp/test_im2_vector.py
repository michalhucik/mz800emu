#!/usr/bin/env python3
"""E2E test: vektor IM 2 bez Z80 PIO je poslední bajt na datové sběrnici (MCP pipe).

Spustí ověřovací program tests/data/im2_vector/im2_vector.mzf a jeho histogramy
(8000h + k * 400h) porovná s tím, co týž program dal na skutečném MZ-800 (2026-11-26).
Program bere přerušení z CTC2 v IM 2 nad tabulkou 257 bajtů, ve které každý vektor vede
na jinou adresu, a sbírá, jaký vektor procesor přečetl po různých instrukcích:

  0  vektor dodá Z80 PIO (5Ah)            -> 5Ah   (kontrola metody)
  1  JP 4D33h                             -> 4Dh   (bit 0 se nemaskuje)
  2  LD A,(3001h), data 9Bh               -> 9Bh
  3  HALT, za ním SCF (37h)               -> 37h   (v HALT bajt za HALT)
  4  LD (HL),A s A = 2Ch                  -> 2Ch   (zapsaný bajt)
  5  PUSH DE s E = 4Bh                    -> 4Bh   (druhý zapsaný bajt)
  6  OUT (0A1h),A s A = 2Ch               -> 2Ch   (bajt zápisu na port)
  7  IN A,(0A0h), volný port              -> A0h   (A = A0h, operand)
  8  IN A,(0FFh), PIO B Mode 0 výstup A9h -> A9h   (A = A9h)

Kontroluje se nejčastější vektor každého testu (pár vzorků po JP na konci bloku je jiný,
stejně jako na stroji) a nejčastější hodnota registru A u testů 7 a 8.

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

_DATA = Path(__file__).resolve().parent.parent / "data" / "im2_vector"
#: Adresa výsledků testu 0; test k má výsledky o k * 400h dál (vektory, od +200h registr A).
_RES = 0x8000
#: Název testu, vektor naměřený na MZ-800, hodnota A naměřená na MZ-800 (None = nekontroluje se).
_EXPECT = [("PIO VEC", 0x5A, None), ("JP 4D33", 0x4D, None), ("LDA 9B", 0x9B, None),
           ("HALT", 0x37, None), ("LDHL 2C", 0x2C, None), ("PUSH 4B", 0x4B, None),
           ("OUT A1", 0x2C, None), ("IN A0", 0xA0, 0xA0), ("IN PIOB", 0xA9, 0xA9)]


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


def _top(raw):
    """Vrátí (hodnota, počet, celkem) nejčastější položky histogramu 256 slov LE."""
    words = [raw[2 * i] | raw[2 * i + 1] << 8 for i in range(256)]
    best = max(range(256), key=lambda i: (words[i], -i))
    return best, words[best], sum(words)


def run(exe):
    """Spustí program a porovná histogramy. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="im2_vector_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        emu.call("pause")
        emu.call("media_run_mzf", {"path": str(_DATA / "im2_vector.mzf")})
        emu.call("set_speed", {"mode": "max"})
        emu.call("run", {"frames": 100})
        emu.call("input_press_key", {"key": "1"})
        emu.call("run", {"frames": 10})
        emu.call("input_release_key", {"key": "1"})
        done = False
        for _i in range(40):
            emu.call("run", {"frames": 50})
            if _mem(emu, 0xD000 + 16 * 40, 4) == bytes([0x04, 0x0F, 0x0E, 0x05]):
                done = True
                break
        h.check(done, "the program finished (DONE on row 16)")
        for k, (name, vec, areg) in enumerate(_EXPECT):
            raw = _mem(emu, _RES + k * 0x400, 0x400)
            v, n, total = _top(raw[:0x200])
            print(f"  {k} {name:8s} vector {v:02X}h x{n} of {total} (real MZ-800 {vec:02X}h)")
            h.check(v == vec, f"test {k} {name}: vector as on a real MZ-800")
            if areg is not None:
                a, n, total = _top(raw[0x200:])
                print(f"    {'':8s} register A {a:02X}h x{n} of {total} (real MZ-800 {areg:02X}h)")
                h.check(a == areg, f"test {k} {name}: register A as on a real MZ-800")
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
