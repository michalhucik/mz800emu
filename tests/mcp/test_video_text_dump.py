#!/usr/bin/env python3
"""E2E test: ``get_video_text_dump`` vrací textovou VRAM, kterou CPU vidí na 0D000h (N-D6-1).

Na MZ-800 v režimu MZ-700 leží znaková VRAM na fyzické adrese 1000h roviny I
(0D000h & 1FFFh), atributová na 1800h; začátek roviny (0000h - 0FFFh) je CG-RAM
na 0C000h. Dump dřív četl roviny od 0000h a 0800h jako na MZ-700 / MZ-1500,
takže na ``mz800emu`` vracel CG-RAM místo textu.

Program (MZF sestavený testem, 1200h) nastaví režim MZ-700 (``OUT (0CEh)``,
na MZ-700 / MZ-1500 zápis na volný port) a výchozí mapu (``OUT (0E4h)``),
zapíše display kódy na 0D000h a atributy na 0D800h a zůstane ve smyčce.
Pak na ``mz800emu``, ``mz700emu-pal`` a ``mz1500emu`` (pipe mód):

  - ``get_video_text_dump``: ``available`` true, první bajty ``chars_b64``
    jsou zapsané display kódy, ``attributes_b64`` zapsané atributy;
  - celý dump (1000 znaků i atributů) se shoduje s oknem ``state_observe``
    na 0D000h a 0D800h (pohled CPU).

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

#: Display kódy "D7 TEXT DUMP" (Sharp display code: A = 01h, 0 = 20h, mezera = 00h).
TEXT = bytes([0x04, 0x27, 0x00, 0x14, 0x05, 0x18, 0x14, 0x00, 0x04, 0x15, 0x0D, 0x10])
#: Atribut zapsaný pod text (popředí 7, pozadí 1).
ATTR = 0x71
#: Adresa zavedení a startu programu.
ORG = 0x1200


def _program():
    """Strojový kód programu (Z80, ORG 1200h), ručně sestavený.

    DI; LD A,08h; OUT (0CEh),A; OUT (0E4h),A; LD HL,0D000h; LD DE,text;
    LD B,n; smyčka LD A,(DE) / LD (HL),A / INC HL / INC DE / DJNZ;
    LD HL,0D800h; LD B,n; LD A,attr; smyčka LD (HL),A / INC HL / DJNZ; JR $.
    """
    n = len(TEXT)
    head = bytes([
        0xF3,                    # DI
        0x3E, 0x08,              # LD A,08h
        0xD3, 0xCE,              # OUT (0CEh),A - režim MZ-700
        0xD3, 0xE4,              # OUT (0E4h),A - výchozí mapa
        0x21, 0x00, 0xD0,        # LD HL,0D000h
    ])
    # LD DE,text doplníme, až bude známá délka kódu.
    tail = bytes([
        0x06, n,                 # LD B,n
        0x1A,                    # loop: LD A,(DE)
        0x77,                    #       LD (HL),A
        0x23,                    #       INC HL
        0x13,                    #       INC DE
        0x10, 0xFA,              #       DJNZ loop
        0x21, 0x00, 0xD8,        # LD HL,0D800h
        0x06, n,                 # LD B,n
        0x3E, ATTR,              # LD A,attr
        0x77,                    # aloop: LD (HL),A
        0x23,                    #        INC HL
        0x10, 0xFC,              #        DJNZ aloop
        0x18, 0xFE,              # JR $
    ])
    text_addr = ORG + len(head) + 3 + len(tail)
    ld_de = bytes([0x11, text_addr & 0xFF, text_addr >> 8])
    return head + ld_de + tail + TEXT


def _make_mzf(path):
    """Zapíše MZF (atribut 01h, fstrt = fexec = 1200h)."""
    body = _program()
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"D7 TEXT DUMP\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = ORG.to_bytes(2, "little")
    hdr[0x16:0x18] = ORG.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


def _cpu_view(emu, addr, length):
    """Bajty paměti tak, jak je vidí CPU (okno state_observe)."""
    st = emu.call("state_observe", {"windows": [{"addr": addr, "len": length}]})
    return base64.b64decode(st["windows"][0]["data_b64"])


def check_exe(exe, mzf, tmp):
    """Kontroly na jedné binárce."""
    name = exe.stem
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("run", {"frames": 5})
        emu.call("pause")
        emu.call("media_run_mzf", {"path": str(mzf)})
        emu.call("run", {"frames": 20})
        emu.call("pause")
        cpu_chars = _cpu_view(emu, 0xD000, 1000)
        cpu_attrs = _cpu_view(emu, 0xD800, 1000)
        check(cpu_chars[:len(TEXT)] == TEXT,
              f"{name}: the program wrote the text to 0D000h ({cpu_chars[:len(TEXT)].hex()})")
        dump = emu.call("get_video_text_dump")
        check(dump.get("available") is True,
              f"{name}: text dump available ({dump.get('reason')})")
        chars = base64.b64decode(dump["chars_b64"])
        attrs = base64.b64decode(dump["attributes_b64"])
        check(chars[:len(TEXT)] == TEXT,
              f"{name}: dump chars start with the written display codes "
              f"(got {chars[:len(TEXT)].hex()}, want {TEXT.hex()})")
        check(attrs[:len(TEXT)] == bytes([ATTR]) * len(TEXT),
              f"{name}: dump attributes start with {ATTR:02X}h (got {attrs[:len(TEXT)].hex()})")
        check(chars == cpu_chars,
              f"{name}: all 1000 dump chars equal CPU view of 0D000h")
        check(attrs == cpu_attrs,
              f"{name}: all 1000 dump attributes equal CPU view of 0D800h")
        print(f"  {name}: OK")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=150)
    exes = [e for e in (_find_exe("mz800emu"), _find_exe("mz700emu-pal"),
                        _find_exe("mz1500emu")) if e]
    if not exes:
        print("SKIP: no emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_video_text_dump_"))
    ok = False
    try:
        mzf = tmp / "d7_text_dump.mzf"
        _make_mzf(mzf)
        for exe in exes:
            check_exe(Path(exe), mzf, tmp)
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
