#!/usr/bin/env python3
"""E2E test: PCG-700 (HAL) jako zařízení devbus na MZ-700.

Na mz700emu-pal a mz700emu-ntsc (pipe mód) ověřuje celou cestu od zápisu
CPU na 0E010h-0E012h po obraz:

  - ``get_devices``: zařízení ``pcg700`` je registrované, odpojené, má
    volby a MMIO 0E010h-0E012h; ``periph_attach`` ho připojí a
    ``regions_list`` ukáže region ``device`` (PCG RAM);
  - vlastní program v RAM podle postupu manuálu (data 0E010h, adresa
    0E011h, horní adresa + 10h a pak 0 na 0E012h) zapíše glyf kódu 80h
    sady 1 a zápisem 00h do 0E012h zapne PCG; na obrazovce je kód 80h:
      * rozdíl snímků s glyfem G a s prázdným glyfem dává přesně body G,
        bit 7 bajtu = levý bod (rozdíl RGBA ze ``get_frame_screenshot_raw``),
      * ``region_read`` vrací G bitově převrácený (formát CG-ROM);
  - bez PCG (zápis 08h do 0E012h), s HSW NOM (``settings_set``) a s COPY
    z CG-ROM (+30h) je obraz shodný s obrazem bez karty (``frame_hash``);
  - snapshot round-trip s připojeným zařízením (odpojení, načtení,
    obraz i RAM zpět);
  - bez připojené karty program obraz nezmění;
  mz800emu: zařízení ``pcg700`` neexistuje (MZ-800 nemá devbus).

Emulátor běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, prázdné
INI, ``--no-save-ini``), bez TCP portu; zabíjí se jen vlastní PID.
Výstup anglicky.

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
from test_devbus_e2e import call_raw  # noqa: E402


# Glyf G ve formátu PCG-700 (bit 7 = levý bod): levý sloupec a horní
# i dolní řádek plné (bounding box = celá buňka), uvnitř asymetrický
# schod, aby šlo poznat převrácení bitů i pořadí řádků.
GLYPH = bytes([0xFF, 0x80, 0xC0, 0xE0, 0xF0, 0x80, 0x80, 0xFF])
EMPTY = bytes(8)

# Pozice kódu 80h na obrazovce (sloupec 10, řádek 5).
VRAM_POS = 0xD000 + 5 * 40 + 10

TABLE = 0x3100  # 8 bajtů glyfu pro program WRITE


def _prog_write():
    """Smaže obrazovku, dá 80h na VRAM_POS a zapíše 8 řádků glyfu z TABLE.

    DI; vyplnit 0D000h-0D3FFh kódem 00h a 0D800h-0DBFFh atributem 70h;
    LD (VRAM_POS),80h; pro B = 0..7: data (HL) -> 0E010h, B -> 0E011h,
    10h -> 0E012h, 00h -> 0E012h (uvolnění strobe, zároveň bit 3 = 0
    = PCG zapnuto); JR $.
    """
    lo, hi = VRAM_POS & 0xFF, VRAM_POS >> 8
    code = bytes([
        0xF3,                    # DI
        0x21, 0x00, 0xD0,        # LD HL,0D000h
        0x11, 0x01, 0xD0,        # LD DE,0D001h
        0x01, 0xFF, 0x03,        # LD BC,03FFh
        0x36, 0x00,              # LD (HL),00h
        0xED, 0xB0,              # LDIR
        0x21, 0x00, 0xD8,        # LD HL,0D800h
        0x11, 0x01, 0xD8,        # LD DE,0D801h
        0x01, 0xFF, 0x03,        # LD BC,03FFh
        0x36, 0x70,              # LD (HL),70h
        0xED, 0xB0,              # LDIR
        0x3E, 0x80,              # LD A,80h
        0x32, lo, hi,            # LD (VRAM_POS),A
        0x21, TABLE & 0xFF, TABLE >> 8,  # LD HL,TABLE
        0x06, 0x00,              # LD B,0
        # loop:
        0x7E,                    # LD A,(HL)
        0x32, 0x10, 0xE0,        # LD (0E010h),A
        0x78,                    # LD A,B
        0x32, 0x11, 0xE0,        # LD (0E011h),A
        0x3E, 0x10,              # LD A,10h
        0x32, 0x12, 0xE0,        # LD (0E012h),A
        0xAF,                    # XOR A
        0x32, 0x12, 0xE0,        # LD (0E012h),A
        0x23,                    # INC HL
        0x04,                    # INC B
        0x78,                    # LD A,B
        0xFE, 0x08,              # CP 8
        0x20, 0xE8,              # JR NZ,loop (-24)
        0x18, 0xFE,              # JR $
    ])
    return code


def _prog_copy():
    """COPY z CG-ROM do kódu 80h sady 1 (adresa B, 0E012h = 30h, pak 00h); JR $."""
    return bytes([
        0xF3,                    # DI
        0x06, 0x00,              # LD B,0
        # loop:
        0x78,                    # LD A,B
        0x32, 0x11, 0xE0,        # LD (0E011h),A
        0x3E, 0x30,              # LD A,30h
        0x32, 0x12, 0xE0,        # LD (0E012h),A
        0xAF,                    # XOR A
        0x32, 0x12, 0xE0,        # LD (0E012h),A
        0x04,                    # INC B
        0x78,                    # LD A,B
        0xFE, 0x08,              # CP 8
        0x20, 0xED,              # JR NZ,loop (-19)
        0x18, 0xFE,              # JR $
    ])


def _prog_poke(value):
    """DI; LD A,value; LD (0E012h),A; JR $."""
    return bytes([0xF3, 0x3E, value, 0x32, 0x12, 0xE0, 0x18, 0xFE])


PROG_ADDR = 0x3000


def run_prog(emu, code, table=None, label=""):
    """Nahraje a spustí program na PROG_ADDR, počká na JR $ a vrátí frame_hash."""
    if table is not None:
        emu.call("mem_write", {"addr": TABLE, "data_hex": table.hex()})
    emu.call("mem_write", {"addr": PROG_ADDR, "data_hex": code.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG_ADDR})
    emu.call("run", {"frames": 3})
    emu.call("pause")
    pc = emu.call("get_registers").get("PC")
    check(pc == PROG_ADDR + len(code) - 2, f"{label}: program reached JR $ (pc={pc})")
    return frame(emu)


def frame(emu):
    """Hash obrazu po dokreslení snímku (v pauze se obraz sám nepřekreslí)."""
    emu.call("run", {"frames": 1})
    emu.call("pause")
    return emu.call("frame_hash")["image_sha256"]


def screenshot(emu):
    """RGBA snímek: (šířka, výška, bytes)."""
    d = emu.call("get_frame_screenshot_raw")
    check(d.get("available") is True, f"screenshot available ({d.get('reason')})")
    return d["width"], d["height"], base64.b64decode(d["data_b64"])


def check_glyph_pixels(stem, shot_g, shot_0):
    """Rozdíl snímků s glyfem G a s prázdným glyfem = přesně body G."""
    w, h, a = shot_g
    w2, h2, b = shot_0
    check((w, h) == (w2, h2), f"{stem}: screenshots have the same size")
    diff = [(i // 4) for i in range(0, len(a), 4) if a[i:i + 3] != b[i:i + 3]]
    check(diff, f"{stem}: the PCG glyph changes the picture")
    xs = [p % w for p in diff]
    ys = [p // w for p in diff]
    x0, y0 = min(xs), min(ys)
    bw, bh = max(xs) - x0 + 1, max(ys) - y0 + 1
    check(bw % 8 == 0 and bh % 8 == 0, f"{stem}: difference is one character cell ({bw} x {bh})")
    sx, sy = bw // 8, bh // 8
    dset = set(diff)
    bad = []
    for r in range(8):
        for c in range(8):
            px = (y0 + r * sy + sy // 2) * w + x0 + c * sx + sx // 2
            want = bool(GLYPH[r] & (0x80 >> c))
            if (px in dset) != want:
                bad.append((r, c))
    check(not bad, f"{stem}: dots match the glyph with bit 7 = left dot (bad {bad[:6]})")
    check(len(diff) == sum(bin(x).count("1") for x in GLYPH) * sx * sy,
          f"{stem}: no pixel outside the glyph dots changed")


def device_region(emu):
    regs = emu.call("regions_list")["regions"]
    dev = [x for x in regs if x["kind"] == "device"]
    return dev[0] if dev else None


def region_bytes(emu, reg, off, length):
    d = emu.call("region_read", {"region_id": reg["id"], "offset": off, "length": length})
    return base64.b64decode(d["data_b64"])


def bitrev(v):
    return int(f"{v:08b}"[::-1], 2)


def check_mz700(exe, tmp):
    stem = exe.stem
    d = tmp / stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        devs = emu.call("get_devices")["devices"]
        pcg = [x for x in devs if x["id"] == "pcg700"]
        check(len(pcg) == 1 and pcg[0]["connected"] is False,
              f"{stem}: pcg700 registered and not connected ({devs})")
        keys = [o["key"] for o in pcg[0]["options"]]
        check(keys == ["hsw", "ssw", "gsw", "ssw_off", "strobe", "decode_dram", "poweron_ram"],
              f"{stem}: pcg700 options ({keys})")
        check(pcg[0]["mmio"] == [{"first": 0xE010, "last": 0xE012, "when": ["mapped"]}],
              f"{stem}: pcg700 mmio ({pcg[0]['mmio']})")
        check(pcg[0]["video"] == {"glyph_source": True, "render_row": False},
              f"{stem}: pcg700 video hooks ({pcg[0]['video']})")

        # Bez karty: obraz programu s kódem 80h z CG-ROM.
        emu.call("reset")
        emu.call("pause")
        h_rom = run_prog(emu, _prog_write(), GLYPH, f"{stem} no card")

        emu.call("periph_attach", {"kind": "pcg700"})
        check(emu.call("get_devices")["devices"][0]["connected"] is True,
              f"{stem}: periph_attach connects pcg700")
        reg = device_region(emu)
        check(reg is not None and reg["size"] == 2048 and "PCG-700" in reg["name"],
              f"{stem}: device region PCG RAM ({reg})")

        # Po připojení je bit 3 = 1 (vypnuto): obraz beze změny.
        check(frame(emu) == h_rom, f"{stem}: connected PCG starts switched off")

        # Prázdný glyf, pak glyf G (program zapne PCG zápisem 00h do 0E012h).
        emu.call("reset")
        emu.call("pause")
        run_prog(emu, _prog_write(), EMPTY, f"{stem} empty glyph")
        shot_0 = screenshot(emu)
        h_pcg = run_prog(emu, _prog_write(), GLYPH, f"{stem} glyph")
        check(h_pcg != h_rom, f"{stem}: PCG glyph differs from CG-ROM")
        check_glyph_pixels(stem, screenshot(emu), shot_0)
        ram = region_bytes(emu, reg, 0, 8)
        check(ram == bytes(bitrev(x) for x in GLYPH),
              f"{stem}: PCG RAM holds the glyph in CG-ROM bit order ({ram.hex()})")

        # HSW NOM: obraz z CG-ROM; zpět PCG.
        r = emu.call("settings_set", {"key": "PCG700/hsw", "value": "nom"})
        check(r.get("applies") == "now", f"{stem}: settings_set hsw applies now ({r})")
        check(frame(emu) == h_rom, f"{stem}: HSW NOM shows the CG-ROM character")
        emu.call("settings_set", {"key": "PCG700/hsw", "value": "pcg"})
        check(frame(emu) == h_pcg, f"{stem}: HSW PCG shows the PCG glyph again")

        # Vypnutí softwarem (08h do 0E012h) a znovu zapnutí (00h).
        check(run_prog(emu, _prog_poke(0x08), None, f"{stem} off") == h_rom,
              f"{stem}: 0E012h = 08h switches PCG off")
        check(run_prog(emu, _prog_poke(0x00), None, f"{stem} on") == h_pcg,
              f"{stem}: 0E012h = 00h switches PCG on")

        # SSW OFF + ssw_off force_off: software PCG nezapne.
        emu.call("settings_set", {"key": "PCG700/ssw", "value": "off"})
        emu.call("settings_set", {"key": "PCG700/ssw_off", "value": "force_off"})
        check(frame(emu) == h_rom, f"{stem}: SSW OFF with force_off keeps PCG off")
        emu.call("settings_set", {"key": "PCG700/ssw", "value": "on"})
        emu.call("settings_set", {"key": "PCG700/ssw_off", "value": "hsw"})
        check(frame(emu) == h_pcg, f"{stem}: SSW ON shows the PCG glyph again")

        # COPY z CG-ROM: glyf 80h = ROM, obraz jako bez karty.
        check(run_prog(emu, _prog_copy(), None, f"{stem} copy") == h_rom,
              f"{stem}: COPY from CG-ROM restores the ROM character")

        # Snapshot round-trip s připojenou kartou.
        h_pcg2 = run_prog(emu, _prog_write(), GLYPH, f"{stem} glyph again")
        check(h_pcg2 == h_pcg, f"{stem}: glyph written again")
        snap = emu.call("snapshot_save_buffer", {"description": "pcg700"})["bytes_b64"]
        emu.call("periph_detach", {"kind": "pcg700"})
        check(frame(emu) == h_rom, f"{stem}: detached card shows the CG-ROM character")
        emu.call("snapshot_load_buffer", {"bytes_b64": snap})
        emu.call("pause")
        check(emu.call("get_devices")["devices"][0]["connected"] is True,
              f"{stem}: snapshot load connects pcg700")
        check(emu.call("frame_hash")["image_sha256"] == h_pcg,
              f"{stem}: snapshot load restores the PCG picture")
        reg = device_region(emu)
        check(region_bytes(emu, reg, 0, 8) == bytes(bitrev(x) for x in GLYPH),
              f"{stem}: snapshot load restores PCG RAM")

        # Bez karty program obraz nezmění.
        emu.call("periph_detach", {"kind": "pcg700"})
        check(run_prog(emu, _prog_write(), GLYPH, f"{stem} detached") == h_rom,
              f"{stem}: without the card the program shows CG-ROM")
    finally:
        emu.close()


def check_mz800(exe, tmp):
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        dev = emu.call("get_devices")
        check(dev.get("devices") == [], f"{exe.stem}: no devbus devices ({dev})")
        r = call_raw(emu, "periph_attach", {"kind": "pcg700"})
        check(not r.get("success"), f"{exe.stem}: periph_attach pcg700 fails")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=300)
    names = ("mz700emu-pal", "mz700emu-ntsc", "mz800emu")
    found = {n: _find_exe(n) for n in names}
    if not found["mz700emu-pal"] and not found["mz700emu-ntsc"]:
        print("SKIP: no MZ-700 emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_pcg700_e2e_"))
    ok = False
    try:
        for n in names:
            if not found[n]:
                print(f"  SKIP: {n} binary not found")
                continue
            if n.startswith("mz700"):
                check_mz700(Path(found[n]), tmp)
            else:
                check_mz800(Path(found[n]), tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
