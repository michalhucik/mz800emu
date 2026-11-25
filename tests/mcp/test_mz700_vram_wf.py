#!/usr/bin/env python3
"""E2E test: zápis a čtení VRAM v režimu MZ-700 na MZ-800 jde přes registry WF a RF (MCP pipe).

Spustí měřicí program tests/data/mz700_vram_wf/wf700_meas1.mzf a tabulky výsledků
(6000h až 69FFh, 10 tabulek po 256 bajtech) porovná s hodnotami modelu netlistu GDG
(blok EXPM v wf700_meas1_tables.asm) na místech, kde je maska MASK = 1. Tytéž hodnoty
dal program na skutečném MZ-800 s rozšířenou VRAM (2026-11-25). Program zkouší všech
256 hodnot WF na buňce znaku, atributu a CG-RAM (čtení zpět přes RF 01h a 04h), všech
256 hodnot RF a zápis v DMD 0Ch; viz hlavička wf700_meas1.asm.

Tabulka 10 (atribut bez čekání po zápisu) musí být shodná s tabulkou 2.

Jen MZ-800. Binárka se hledá v kořeni repa nebo v adresáři z proměnné
``MZ_EMU_DIR``. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import base64
import gettext
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
import test_gdg_cksw as h  # noqa: E402 - PipeEmu

_ = gettext.gettext

_DATA = Path(__file__).resolve().parent.parent / "data" / "mz700_vram_wf"
#: Adresa tabulek výsledků a počet tabulek s očekávanými hodnotami.
_RES = 0x6000
_NTAB = 10
_NAMES = ("chr VA", "chr VC", "atr VA", "atr VC", "cg VA", "cg VC", "rd 01/04", "rd FE/FB", "0C VA", "0C VC")


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


def _tables(path):
    """Načte bloky defb ze souboru pro pasmo. Vrací slovník návěští -> bytes."""
    tables, cur = {}, None
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(\w+):", line)
        if m:
            cur = m.group(1)
            tables[cur] = bytearray()
        elif "defb" in line and cur:
            tables[cur] += bytes(int(x, 16) for x in re.findall(r"0([0-9A-F]{2})h", line))
    return {k: bytes(v) for k, v in tables.items()}


def run(exe):
    """Spustí program a porovná tabulky. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    exp = _tables(_DATA / "wf700_meas1_tables.asm")
    tmp = Path(tempfile.mkdtemp(prefix="mz700_vram_wf_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        emu.call("pause")
        emu.call("media_run_mzf", {"path": str(_DATA / "wf700_meas1.mzf")})
        emu.call("set_speed", {"mode": "max"})
        emu.call("run", {"frames": 600})
        done = _mem(emu, 0xD000 + 24 * 40, 7)
        h.check(done == bytes([0x12, 0x05, 0x13, 0x15, 0x0C, 0x14, 0x13]), "the program finished (RESULTS on the last line)")
        res = _mem(emu, _RES, (_NTAB + 1) * 256)
        for t in range(_NTAB):
            s = slice(t * 256, t * 256 + 256)
            idx = [i for i in range(256) if exp["MASK"][s][i]]
            bad = [i for i in idx if res[s][i] != exp["EXPM"][s][i]]
            print(f"  table {t} {_NAMES[t]:9s}: {len(idx) - len(bad)} of {len(idx)} as a real MZ-800")
            if bad:
                i = bad[0]
                print(f"    first difference: index {i:02X}h read {res[s][i]:02X}h, a real MZ-800 {exp['EXPM'][s][i]:02X}h")
            h.check(not bad, f"table {t} as on a real MZ-800")
        h.check(res[0x200:0x300] == res[0xA00:0xB00], "attribute read right after the write as after a delay")
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
