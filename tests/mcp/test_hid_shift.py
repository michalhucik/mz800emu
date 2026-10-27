#!/usr/bin/env python3
"""E2E test: znak se SHIFT přes input_send_keys stiskne SHIFT dřív než klávesu (N-D5-2).

Program, který SHIFT čte jinde ve skenu než klávesu (např. K&P Disk
BASIC: sloupec 8 do RAM, sloupce kláves později), dostal při současném
stisku SHIFT a klávesy na hranici snímku občas klávesu bez SHIFT (``*``
jako ``:``, v K&P BASICu asi každý sedmý pokus). Na skutečné klávesnici
člověk drží SHIFT dřív. Na mz700emu-pal (pipe mód, bez TCP portu):

  - vlastní program v RAM čte sloupec 8 (SHIFT), čeká asi 3300 T, čte
    sloupec 0 a při nové hraně klávesy ``:`` (sloupec 0, bit 1) uloží
    přečtený sloupec 8 do bufferu; okno mezi čtením SHIFT a klávesy pokrývá
    skoro celý průchod smyčkou, současný stisk se tak téměř vždy trefí do
    něj;
  - ``input_send_keys`` ``"*"`` (5x), ``"*:*"`` a ``input_send_key`` ``"*"``:
    každý ``*`` uloží sloupec 8 se SHIFT (bit 0 = 0), každý ``:`` bez SHIFT;
  - ``total_frames`` a ``emu_frames`` se nemění (SHIFT jde o snímek dřív
    v rámci držení klávesy, výchozí ``frame_per_key`` 3).

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

#: Adresa programu.
PROG = 0x1200
#: Buffer uložených sloupců 8 (jeden bajt na hranu klávesy ':').
BUF = 0x3000
#: DI; LD HL,BUF; LD C,0FFh
#: loop: LD A,8; LD (0E000h),A; LD A,(0E001h); LD D,A; LD B,0; DJNZ $
#:       XOR A; LD (0E000h),A; LD A,(0E001h); LD E,A
#:       LD A,C; CPL; OR E; LD C,E; AND 02h; JR NZ,loop
#:       LD (HL),D; INC HL; JR loop
PROG_HEX = ("F3 21 00 30 0E FF"
            " 3E 08 32 00 E0 3A 01 E0 57 06 00 10 FE"
            " AF 32 00 E0 3A 01 E0 5F"
            " 79 2F B3 4B E6 02 20 E3"
            " 72 23 18 DF")


def buf(emu, n):
    """Prvních n uložených bajtů bufferu."""
    return base64.b64decode(emu.call("mem_read", {"addr": BUF, "len": n})["data_b64"])


def check_pal(exe, tmp):
    """Kontroly na mz700emu-pal."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("run", {"frames": 60})
        emu.call("pause")
        code = bytes.fromhex(PROG_HEX)
        check(len(code) == 39 and code[33:35] == b"\x20\xe3" and code[-2:] == b"\x18\xdf", "test program layout")
        emu.call("mem_write", {"addr": BUF, "data_hex": "AA" * 16})
        emu.call("mem_write", {"addr": PROG, "data_hex": code.hex()})
        emu.call("set_register", {"reg": "PC", "value": PROG})
        emu.call("run", {"frames": 5})
        emu.call("pause")

        want = []
        for i in range(5):
            r = emu.call("input_send_keys", {"text": "*"})
            check(r.get("total_frames") == 3 and r.get("emu_frames") == 3,
                  f"'*' #{i + 1}: timing unchanged (total {r.get('total_frames')}, emu {r.get('emu_frames')})")
            emu.call("run", {"frames": 3 + i})
            emu.call("pause")
            want.append(True)
        r = emu.call("input_send_keys", {"text": "*:*"})
        check(r.get("total_frames") == 11 and r.get("emu_frames") == 11,
              f"'*:*': timing unchanged (total {r.get('total_frames')}, emu {r.get('emu_frames')})")
        want += [True, False, True]
        emu.call("run", {"frames": 3})
        emu.call("pause")
        r = emu.call("input_send_key", {"key": "*"})
        check(r.get("shift") is True, f"input_send_key '*' uses SHIFT ({r})")
        want.append(True)
        emu.call("run", {"frames": 3})
        emu.call("pause")

        got = buf(emu, len(want) + 1)
        print("  stored column 8: " + " ".join(f"{b:02X}" for b in got))
        check(got[len(want)] == 0xAA, f"exactly {len(want)} key edges recorded")
        for i, (w, b) in enumerate(zip(want, got)):
            check((b & 1) == (0 if w else 1),
                  f"edge {i + 1}: SHIFT {'held' if w else 'not held'} when the key was read "
                  f"(column 8 = {b:02X}h)")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=120)
    pal = _find_exe("mz700emu-pal")
    if not pal:
        print("SKIP: mz700emu-pal binary not found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_hid_shift_"))
    ok = False
    try:
        check_pal(Path(pal), tmp)
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
