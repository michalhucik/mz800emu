#!/usr/bin/env python3
"""E2E test: regiony VRAM 700 pokrývají celou znakovou a atributovou VRAM (N-D5-4).

Znaková VRAM MZ-700 je 2 KB (0D000h - 0D7FFh), atributová 2 KB
(0D800h - 0DFFFh); obraz používá prvních 1000 bajtů, zbytek je RAM (např.
blok B kitu SUC). Regiony ``vram_700_char`` a ``vram_700_attr`` dřív měly
jen 1 KB a horní polovina přes ``region_read`` nešla. Na mz700emu-pal
a mz1500emu (pipe mód, bez TCP portu):

  - ``regions_list``: oba regiony mají ``size`` 2048 a jméno ``(2 KB)``;
  - ``mem_write`` na 0D400h / 0D7FFh / 0DC00h / 0DFFFh, ``region_read``
    na offsetech 400h / 7FFh vrátí zapsané bajty;
  - ``region_write`` na offset 7FEh atributové VRAM je vidět přes
    ``mem_read`` 0DFFEh.

Emulátor běží s izolovaným --cfg-dir a --work-dir; zabíjí se jen vlastní
PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import base64
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)


def region(emu, kind):
    """Region podle druhu (právě jeden)."""
    found = [r for r in emu.call("regions_list")["regions"] if r["kind"] == kind]
    check(len(found) == 1, f"one region {kind}")
    return found[0]


def rd(emu, reg, offset, length=1):
    """Bajty regionu."""
    return base64.b64decode(emu.call("region_read", {
        "region_id": reg["id"], "offset": offset, "length": length})["data_b64"])


def check_exe(exe, tmp):
    """Kontroly na jedné binárce."""
    name = exe.stem
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("run", {"frames": 5})
        emu.call("pause")
        ch, at = region(emu, "vram_700_char"), region(emu, "vram_700_attr")
        for r, base in ((ch, "char"), (at, "attr")):
            check(r["size"] == 0x800 and r["name"] == f"VRAM 700 {base} (2 KB)",
                  f"{name}: region {r['kind']} is 2 KB ({r['size']}, '{r['name']}')")
        emu.call("mem_write", {"addr": 0xD400, "data_hex": "5A"})
        emu.call("mem_write", {"addr": 0xD7FF, "data_hex": "5B"})
        emu.call("mem_write", {"addr": 0xDC00, "data_hex": "6A"})
        emu.call("mem_write", {"addr": 0xDFFF, "data_hex": "6B"})
        got = (rd(emu, ch, 0x400), rd(emu, ch, 0x7FF), rd(emu, at, 0x400), rd(emu, at, 0x7FF))
        check(got == (b"\x5a", b"\x5b", b"\x6a", b"\x6b"),
              f"{name}: region_read of the upper 1 KB returns the written bytes ({got})")
        emu.call("region_write", {"region_id": at["id"], "offset": 0x7FE,
                                  "data_b64": base64.b64encode(bytes.fromhex("99")).decode()})
        raw = base64.b64decode(emu.call("mem_read", {"addr": 0xDFFE, "len": 1})["data_b64"])
        check(raw == b"\x99", f"{name}: region_write at offset 7FEh lands at 0DFFEh ({raw.hex()})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=120)
    exes = [e for e in (_find_exe("mz700emu-pal"), _find_exe("mz1500emu")) if e]
    if not exes:
        print("SKIP: no MZ-700 / MZ-1500 emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_vram700_regions_"))
    ok = False
    try:
        for exe in exes:
            check_exe(Path(exe), tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"  (work dir kept: {tmp})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
