#!/usr/bin/env python3
"""E2E test: 80sloupcová karta K&P (zařízení devbus ``kp80``) na MZ-700.

Ověřuje lepidlo karty přes MCP (pipe mód, bez TCP portu) na mz700emu-pal
a zkráceně na mz700emu-ntsc:

  - ``get_devices``: karta je registrovaná, odpojená, má výstup ``kp80``
    640 x 250 a regiony VRAM, font a registry CRTC;
  - bez připojené karty (před připojením a po odpojení) se porty 70h - 73h
    čtou jako neobsazený port 74h (latch datové sběrnice, jako na masteru),
    ``device_screenshot`` hlásí odpojené zařízení;
  - vlastní program v RAM inicializuje CRTC podle BKK (R0 - R15), nastaví
    R18/R19, vybere R31, čeká na bit 7 statusu a zapíše text na 71h;
    VRAM karty (region) obsahuje text, region registrů hodnoty R0 - R15;
  - obraz MZ-700 (frame_hash) je stejný s kartou i bez ní;
  - ``device_screenshot``: indexy palety odpovídají fontu karty (region
    fontu) pro znaky textu, PNG a zápis do souboru; změna fosforu
    (settings_set ``KP80/phosphor``) změní index svítících bodů;
  - zápis přes io_write v pauze překreslí druhý monitor hned;
  - snapshot round-trip (odpojení, načtení: karta, VRAM, registry i obraz);
  - RESET MZ-700 VRAM ani registry karty nemění;
  - všechna tři zařízení MZ-700 připojená naráz (PAL i NTSC): PCG-700,
    kit SUC a karta K&P. Obraz MZ-700 je v 80 sloupcích s glyfem kódu 80h
    z PCG RAM a zároveň druhý monitor K&P ukazuje text programu; snapshot
    round-trip (odpojení všech tří, načtení) vrátí všechna tři zařízení,
    obraz MZ-700, obraz druhého monitoru i PCG RAM.

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
from test_devbus_e2e import call_raw  # noqa: E402
from test_suc80_e2e import (  # noqa: E402 - pomocné funkce kitu SUC a PCG-700
    ATTR_TEXT, PCG_GLYPH, PCG_TABLE, PROG, _prog_pcg_suc, glyph80, park, screen)

#: Inicializace CRTC podle BKK (R0 - R15).
BKK_REGS = bytes.fromhex("7F 50 64 0C 1E 02 19 1C 78 09 60 09 00 00 00 00")
#: Text zapsaný programem od adresy VRAM 0.
TEXT = b"HELLO K&P 80"
#: Šířka a výška obrazu karty.
FB_W, FB_H = 640, 250


def kp80_program():
    """Program na 3000h: init R0 - R15, R18/R19 = 0, R31, čekání, text na 71h.

    DI; LD HL,regs; LD B,16; XOR A
    loop: OUT (72h),A; LD D,A; LD A,(HL); OUT (73h),A; INC HL; LD A,D;
          INC A; DJNZ loop
    LD A,18; OUT (72h),A; XOR A; OUT (73h),A
    LD A,19; OUT (72h),A; XOR A; OUT (73h),A
    LD A,31; OUT (72h),A
    wait: IN A,(72h); BIT 7,A; JR Z,wait
    LD HL,text; LD B,len
    wl: LD A,(HL); OUT (71h),A; INC HL; DJNZ wl
    JR $                     (3035h)
    regs (3037h), text (3047h)
    """
    code = bytearray.fromhex(
        "F3 21 37 30 06 10 AF D3 72 57 7E D3 73 23 7A 3C 10 F5"
        " 3E 12 D3 72 AF D3 73 3E 13 D3 72 AF D3 73 3E 1F D3 72"
        " DB 72 CB 7F 28 FA 21 47 30 06" + " %02X" % len(TEXT)
        + " 7E D3 71 23 10 FA 18 FE")
    assert len(code) == 0x37
    return bytes(code) + BKK_REGS + TEXT


def device_region(emu, name_prefix):
    """Region zařízení podle začátku jména."""
    regions = emu.call("regions_list")["regions"]
    found = [r for r in regions if r["kind"] == "device" and r["name"].startswith(name_prefix)]
    check(len(found) == 1, f"one device region '{name_prefix}' ({[r['name'] for r in regions if r['kind'] == 'device']})")
    return found[0]


def region_bytes(emu, region, length, offset=0):
    """Přečte bajty regionu."""
    return base64.b64decode(emu.call("region_read", {
        "region_id": region["id"], "offset": offset, "length": length})["data_b64"])


def check_ports_unconnected(emu, what):
    """Porty 70h - 73h se čtou jako nepoužitý port 74h (latch datové sběrnice).

    Bez zařízení vrací IN neobsazeného portu poslední bajt na sběrnici
    (port_read_cb, větev default), stejně jako na masteru.
    """
    ref = emu.call("io_read", {"port": 0x74})["value"]
    vals = [emu.call("io_read", {"port": p})["value"] for p in range(0x70, 0x74)]
    check(vals == [ref] * 4, f"{what}: ports 70h-73h read like unused port 74h ({vals}, {ref})")


def shot_indexed(emu):
    """device_screenshot ve formátu indexed: (odpověď, bytes indexů)."""
    d = emu.call("device_screenshot", {"output": "kp80", "format": "indexed"})
    check(d.get("available") is True, f"device_screenshot available ({d.get('reason')})")
    pix = base64.b64decode(d["data_b64"])
    check(len(pix) == FB_W * FB_H and d["width"] == FB_W and d["height"] == FB_H,
          f"indexed picture 640x250 ({d['width']}x{d['height']}, {len(pix)} B)")
    return d, pix


def expected_cell(font, code, lit):
    """Body jedné buňky 8 x 10 podle fontu (bit 7 = levý bod, bit 7 kódu = inverze)."""
    rows = []
    for ra in range(10):
        b = font[(code & 0x7F) * 16 + ra]
        if code & 0x80:
            b ^= 0xFF
        rows.append(bytes(lit if (b >> (7 - x)) & 1 else 0 for x in range(8)))
    return rows


def cell(pix, col, row):
    """Body buňky (sloupec, řádek) z obrazu 640 x 250 (R9 = 9: 10 řádků)."""
    return [pix[(row * 10 + ra) * FB_W + col * 8:(row * 10 + ra) * FB_W + col * 8 + 8]
            for ra in range(10)]


def run_program(emu):
    """Nahraje program, spustí ho na 2 snímky a ověří, že stojí v JR $."""
    emu.call("mem_write", {"addr": 0x3000, "data_hex": kp80_program().hex()})
    emu.call("set_register", {"reg": "PC", "value": 0x3000})
    emu.call("run", {"frames": 2})
    emu.call("pause")
    pc = emu.call("get_registers").get("PC")
    check(pc == 0x3035, f"test program reached JR $ (pc={pc:04X}h)")


def check_pal(exe, tmp):
    """Plná sada kontrol na mz700emu-pal."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        devs = emu.call("get_devices")["devices"]
        kp = [x for x in devs if x["id"] == "kp80"]
        check(len(kp) == 1 and kp[0]["connected"] is False, "kp80 registered, not connected")
        kp = kp[0]
        check(kp["outputs"] == [{"id": "kp80", "title": "K&P 80 Column Monitor",
                                 "width": FB_W, "height": FB_H}], f"kp80 output ({kp['outputs']})")
        check([p["first"] for p in kp["ports"]] == [0x70] and kp["ports"][0]["last"] == 0x73,
              "kp80 ports 70h-73h")
        opts = {o["key"]: o["value"] for o in kp["options"]}
        check(opts == {"read_autoinc": True, "data_to_reg": True, "addr_on_r31": True,
                       "update_delay": 0, "wait": 0, "phosphor": "white"},
              f"kp80 option defaults ({opts})")

        # Bez karty: porty jako bez zařízení, výstup odpojený.
        check_ports_unconnected(emu, "before attach")
        r = emu.call("device_screenshot", {"output": "kp80"})
        check(r.get("available") is False and "not connected" in r.get("reason", ""),
              f"device_screenshot of a disconnected card ({r})")
        r = call_raw(emu, "device_screenshot", {})
        check(not r.get("success"), "device_screenshot without output and without a connected device fails")

        # Obraz MZ-700 bez karty: DI; JR $ (program karty by bez ní čekal
        # na bit 7 statusu navždy). Obraz MZ-700 pak stojí.
        emu.call("mem_write", {"addr": 0x3100, "data_hex": "F318FE"})
        emu.call("set_register", {"reg": "PC", "value": 0x3100})
        emu.call("run", {"frames": 2})
        emu.call("pause")
        h0 = emu.call("frame_hash")

        # Připojení: prázdný obraz (registry 0, R6 = 0).
        emu.call("periph_attach", {"kind": "kp80"})
        s0, pix0 = shot_indexed(emu)
        check(s0["lit_pixels"] == 0 and not any(pix0), "empty second monitor after power on")

        run_program(emu)
        h1 = emu.call("frame_hash")
        check(h0["image_sha256"] == h1["image_sha256"] and h0["vram_sha256"] == h1["vram_sha256"],
              "MZ-700 picture is the same with and without the card")

        vram_reg = device_region(emu, "K&P 80 VRAM")
        font_reg = device_region(emu, "K&P 80 font")
        regs_reg = device_region(emu, "K&P 80 CRTC")
        check(vram_reg["writable"] and not font_reg["writable"] and not regs_reg["writable"],
              "VRAM region writable, font and registers read only")
        vram = region_bytes(emu, vram_reg, 2048)
        check(vram[:len(TEXT)] == TEXT and vram[len(TEXT)] == 0,
              f"card VRAM holds the text ({vram[:16]!r})")
        regs = region_bytes(emu, regs_reg, 32)
        check(regs[:16] == BKK_REGS and regs[18] == 0 and regs[19] == 0 and regs[20] == 31,
              f"CRTC registers R0-R15, R18/R19, AR = 31 ({regs.hex()})")
        upd = regs[22] | (regs[23] << 8)
        check(upd == len(TEXT), f"update address after the text ({upd})")
        font = region_bytes(emu, font_reg, 2048)
        check(font[0x41 * 16 + 1] == 0x38, "font glyph 'A' row 1 = 38h")

        # Obraz: buňky textu podle fontu (buňka 0 nese blikající kurzor).
        s1, pix1 = shot_indexed(emu)
        check(s1["palette"][0] == "#000000" and len(s1["palette"]) == 4, f"palette ({s1['palette']})")
        for col in range(1, len(TEXT) + 2):
            code = vram[col]
            check(cell(pix1, col, 0) == expected_cell(font, code, 1),
                  f"cell {col} shows character {code:02X}h")
        check(cell(pix1, 5, 24) == expected_cell(font, 0, 1), "last row shows '@' (VRAM 00h)")
        check(not any(pix1[25 * 10 * FB_W:]), "no picture below 25 rows (R6 = 25)")
        check(s1["seq"] > s0["seq"], "output frame sequence grows")

        # PNG a zápis do souboru.
        png = emu.call("device_screenshot", {})
        raw = base64.b64decode(png["data_b64"])
        check(png["format"] == "png" and raw[:8] == b"\x89PNG\r\n\x1a\n" and png["output"] == "kp80",
              "PNG picture of the first connected output")
        check(png["image_sha256"] == s1["image_sha256"], "PNG and indexed picture have the same hash")
        target = d / "kp80.png"
        f = emu.call("device_screenshot", {"output": "kp80", "path": str(target)})
        check(f.get("path") == str(target) and target.read_bytes()[:8] == raw[:8],
              "device_screenshot writes the PNG to a file")

        # Fosfor: svítící body dostanou index 2 (zelená), počet stejný.
        emu.call("settings_set", {"key": "KP80/phosphor", "value": "green"})
        s2, pix2 = shot_indexed(emu)
        check(s2["lit_pixels"] == s1["lit_pixels"] and set(pix2) <= {0, 2}
              and s2["image_sha256"] != s1["image_sha256"],
              "green phosphor: same dots, palette index 2, different hash")
        emu.call("settings_set", {"key": "KP80/phosphor", "value": "white"})

        # io_write v pauze: 'Z' na adresu 5, obraz hned.
        for port, value in ((0x72, 18), (0x73, 0), (0x72, 19), (0x73, 5), (0x72, 31), (0x71, ord("Z"))):
            emu.call("io_write", {"port": port, "value": value})
        vram = region_bytes(emu, vram_reg, 2048)
        check(vram[5] == ord("Z"), "io_write stored 'Z' at VRAM 5")
        regs = region_bytes(emu, regs_reg, 32)
        check(regs[19] == 5, "io_write set R19")
        s3, pix3 = shot_indexed(emu)
        check(cell(pix3, 5, 0) == expected_cell(font, ord("Z"), 1),
              "second monitor redrawn at once in pause")

        # Snapshot round-trip.
        snap = emu.call("snapshot_save_buffer", {"description": "kp80"})["bytes_b64"]
        emu.call("periph_detach", {"kind": "kp80"})
        r = emu.call("device_screenshot", {"output": "kp80"})
        check(r.get("available") is False, "card detached")
        check_ports_unconnected(emu, "after detach")
        emu.call("snapshot_load_buffer", {"bytes_b64": snap})
        devs = emu.call("get_devices")["devices"]
        check([x for x in devs if x["id"] == "kp80"][0]["connected"] is True,
              "snapshot load connects the card again")
        check(region_bytes(emu, vram_reg, 2048) == vram, "VRAM restored from the snapshot")
        check(region_bytes(emu, regs_reg, 20) == regs[:20], "CRTC registers restored from the snapshot")
        s4, _ = shot_indexed(emu)
        check(s4["image_sha256"] == s3["image_sha256"], "second monitor picture restored")

        # RESET MZ-700: VRAM a registry karty zůstávají.
        emu.call("reset")
        emu.call("pause")
        check(region_bytes(emu, vram_reg, 2048) == vram, "reset keeps card VRAM")
        check(region_bytes(emu, regs_reg, 20) == regs[:20], "reset keeps CRTC registers")
    finally:
        emu.close()


def check_ntsc(exe, tmp):
    """Zkrácená kontrola na mz700emu-ntsc (jiný takt GDG pro přepočet času)."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        emu.call("periph_attach", {"kind": "kp80"})
        run_program(emu)
        vram = region_bytes(emu, device_region(emu, "K&P 80 VRAM"), 64)
        check(vram[:len(TEXT)] == TEXT, f"{exe.stem}: card VRAM holds the text")
        s, _ = shot_indexed(emu)
        check(s["lit_pixels"] > 0, f"{exe.stem}: second monitor shows the text")
    finally:
        emu.close()


def check_all_devices(exe, tmp):
    """PCG-700 + SUC + K&P naráz: 80 sloupců s glyfem PCG a text na kartě K&P."""
    d = tmp / (exe.stem + "_all")
    d.mkdir()
    emu = PipeEmu(exe, d)
    name = exe.stem + " all devices"
    want = [tuple((b >> (7 - i)) & 1 for i in range(8)) for b in PCG_GLYPH]
    cells80 = ((0, 0), (0, 1), (24, 78), (24, 79))
    try:
        emu.call("run", {"frames": 50})
        emu.call("pause")
        park(emu)
        img_plain = screen(emu)
        for kind in ("pcg700", "suc80", "kp80"):
            emu.call("periph_attach", {"kind": kind})
        devs = {x["id"]: x for x in emu.call("get_devices")["devices"]}
        check(all(devs.get(k, {}).get("connected") is True for k in ("pcg700", "suc80", "kp80")),
              f"{name}: pcg700, suc80 and kp80 connected together")

        # VRAM MZ-700: mezery, atribut 70h, kód 80h v první a poslední dvojici.
        chars = bytearray(0x800)
        for off in (0x000, 0x401, 0x3E7, 0x7E8):
            chars[off] = 0x80
        attrs = bytearray([ATTR_TEXT]) * 0x800
        for base, data in ((0xD000, chars), (0xD800, attrs)):
            for off in range(0, len(data), 0x400):
                emu.call("mem_write", {"addr": base + off,
                                       "data_hex": data[off:off + 0x400].hex()})

        # Glyf do PCG RAM, PCG zapnout, SUC 80 sloupců.
        code = _prog_pcg_suc()
        emu.call("mem_write", {"addr": PCG_TABLE, "data_hex": PCG_GLYPH.hex()})
        emu.call("mem_write", {"addr": PROG, "data_hex": code.hex()})
        emu.call("set_register", {"reg": "PC", "value": PROG})
        emu.call("run", {"frames": 3})
        emu.call("pause")
        pc = emu.call("get_registers").get("PC")
        check(pc == PROG + len(code) - 2, f"{name}: PCG + SUC program reached JR $ (pc={pc:#x})")

        # Text na kartě K&P (program na 3000h přepíše předchozí).
        run_program(emu)
        img80 = screen(emu)
        check(img80[0] != img_plain[0], f"{name}: MZ-700 picture changed by PCG + SUC")
        for (row, col) in cells80:
            got = [glyph80(img80, row, col, ln) for ln in range(8)]
            check(got == want, f"{name}: 80 columns row {row} col {col} shows the PCG glyph of 80h")

        vram_reg = device_region(emu, "K&P 80 VRAM")
        font_reg = device_region(emu, "K&P 80 font")
        pcg_reg = device_region(emu, "PCG-700 RAM")
        vram = region_bytes(emu, vram_reg, 2048)
        check(vram[:len(TEXT)] == TEXT, f"{name}: card VRAM holds the text")
        font = region_bytes(emu, font_reg, 2048)
        s1, pix1 = shot_indexed(emu)
        for col in range(1, len(TEXT)):
            check(cell(pix1, col, 0) == expected_cell(font, vram[col], 1),
                  f"{name}: second monitor cell {col} shows character {vram[col]:02X}h")
        pcg = region_bytes(emu, pcg_reg, 2048)
        check(pcg[:8] == bytes(int(f"{b:08b}"[::-1], 2) for b in PCG_GLYPH),
              f"{name}: PCG RAM holds the glyph of 80h (CG-ROM bit order)")

        # Snapshot round-trip se všemi třemi zařízeními.
        snap = emu.call("snapshot_save_buffer", {"description": "all devices"})["bytes_b64"]
        for kind in ("pcg700", "suc80", "kp80"):
            emu.call("periph_detach", {"kind": kind})
        emu.call("run", {"frames": 2})
        emu.call("pause")
        check(screen(emu)[0] != img80[0], f"{name}: detaching changes the MZ-700 picture")
        r = emu.call("device_screenshot", {"output": "kp80"})
        check(r.get("available") is False, f"{name}: second monitor gone after detach")
        emu.call("snapshot_load_buffer", {"bytes_b64": snap})
        emu.call("run", {"frames": 2})
        emu.call("pause")
        devs = {x["id"]: x for x in emu.call("get_devices")["devices"]}
        check(all(devs.get(k, {}).get("connected") is True for k in ("pcg700", "suc80", "kp80")),
              f"{name}: snapshot load connects all three devices")
        check(screen(emu)[0] == img80[0], f"{name}: snapshot restores the 80-column PCG picture")
        s2, _ = shot_indexed(emu)
        check(s2["image_sha256"] == s1["image_sha256"], f"{name}: snapshot restores the second monitor")
        check(region_bytes(emu, pcg_reg, 2048) == pcg, f"{name}: snapshot restores the PCG RAM")
        check(region_bytes(emu, vram_reg, 2048) == vram, f"{name}: snapshot restores the card VRAM")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=300)
    pal = _find_exe("mz700emu-pal")
    ntsc = _find_exe("mz700emu-ntsc")
    if not pal and not ntsc:
        print("SKIP: no MZ-700 emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_kp80_e2e_"))
    ok = False
    try:
        if pal:
            check_pal(Path(pal), tmp)
        else:
            print("  SKIP: mz700emu-pal binary not found")
        if ntsc:
            check_ntsc(Path(ntsc), tmp)
        else:
            print("  SKIP: mz700emu-ntsc binary not found")
        for exe in (pal, ntsc):
            if exe:
                check_all_devices(Path(exe), tmp)
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
