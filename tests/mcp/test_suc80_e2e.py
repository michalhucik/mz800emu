#!/usr/bin/env python3
"""E2E test: 80sloupcový kit SUC (zařízení devbus ``suc80``) na MZ-700.

Na mz700emu-pal a mz700emu-ntsc (pipe mód, izolované --cfg-dir a
--work-dir, zabíjí se jen vlastní PID):

  - ``get_devices`` hlásí ``suc80`` (MMIO 0E00Ch-0E00Fh jen ``mapped``,
    ``render_row``, volba ``attributes``), po startu odpojené;
  - bez zařízení zápis 50h na 0E00Ch instrukcí CPU obraz nezmění (chování
    jako bez devbus);
  - po připojení je obraz ve 40 sloupcích bajtově stejný jako bez zařízení;
  - zápis 50h přepne na 80 sloupců: znaky sudých sloupců z 0D000h + k,
    lichých z 0D401h + k (první a poslední dvojice), glyf má 8 bodů
    (vzor = každý druhý bod téhož znaku ve 40 sloupcích), atributy po
    znacích (0D800h + k / 0DC01h + k), volba ``per_pair`` bere atribut
    bloku A; bajty 0D400h / 0DC00h se nezobrazí;
  - zápis 28h vrátí obraz 40 sloupců (stejný hash jako bez zařízení);
  - po ``OUT (0E1h)`` (DRAM na 0E000h) zápis 50h režim nepřepne;
  - RESET stroje -> 40 sloupců;
  - snapshot round-trip vrátí 80 sloupců i obraz;
  - spolu s PCG-700 (``pcg700`` + ``suc80`` připojené naráz): program
    v RAM zapíše glyf kódu 80h sady 1 do PCG RAM, zapne PCG a přepne
    na 80 sloupců; kód 80h v sudém i lichém sloupci (první a poslední
    dvojice) se kreslí glyfem z PCG RAM, 8 bodů na znak, bit 7 = levý
    bod; ve 40 sloupcích týž glyf na 16 bodů; s HSW NOM (glyfy CG-ROM)
    je obraz jiný.

Obraz se čte přes ``get_frame_screenshot_raw`` (RGBA celé obrazovky,
plátno 640 x 200 od bodu 32, 16) - tedy stejnou cestou jako MCP screenshot.

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

# Levý horní roh plátna v obrazu get_frame_screenshot_raw (okraje MZ-700).
CANVAS_X = 32
CANVAS_Y = 16

PROG = 0x3000  # programy v RAM
PARK = 0x3010  # DI; JR $

# Kódy znaků testovacích buněk (ověřuje se jen, že mají neprázdný glyf).
C1, C2, C3, C4 = 0x01, 0x02, 0x03, 0x04
ATTR_TEXT = 0x70  # popředí 7, pozadí 0


def screen(emu):
    """Vrátí (image_sha256, rgba bytes, šířka) aktuálního obrazu."""
    d = emu.call("get_frame_screenshot_raw")
    return d["image_sha256"], base64.b64decode(d["data_b64"]), d["width"]


def px(img, x, y):
    """RGBA bodu plátna (x 0-639, y 0-199)."""
    data, width = img[1], img[2]
    o = ((CANVAS_Y + y) * width + CANVAS_X + x) * 4
    return data[o:o + 4]


def cell_bits(img, x0, y, n, step):
    """n bodů od x0 s krokem step: 1 = barva jiná než bod pozadí (0, y)."""
    bg = px(img, 0, 199)
    return tuple(int(px(img, x0 + i * step, y) != bg) for i in range(n))


def run_latch(emu, value):
    """Zapíše value na 0E00Ch instrukcí CPU a nechá proběhnout 2 snímky.

    DI; LD A,value; LD (0E00Ch),A; JR $
    """
    emu.call("mem_write", {"addr": PROG,
                           "data_hex": f"F33E{value:02X}320CE018FE"})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {"frames": 2})
    emu.call("pause")
    pc = emu.call("get_registers").get("PC")
    check(pc == PROG + 6, f"latch program reached JR $ (pc={pc:#x})")


def park(emu):
    """DI; JR $ - CPU stojí, VRAM nikdo nemění; 2 snímky."""
    emu.call("mem_write", {"addr": PARK, "data_hex": "F318FE"})
    emu.call("set_register", {"reg": "PC", "value": PARK})
    emu.call("run", {"frames": 2})
    emu.call("pause")


def fill_vram(emu):
    """Testovací obsah VRAM (adresy podle SUC 2015, báze hw/28)."""
    chars = bytearray(0x800)
    attrs = bytearray([ATTR_TEXT]) * 0x800
    chars[0x000] = C1   # 80: řádek 0, sloupec 0  | 40: k = 0
    chars[0x401] = C2   # 80: řádek 0, sloupec 1
    chars[0x001] = C2   # 40: k = 1 (vzor C2); 80: sloupec 2
    chars[0x3E7] = C3   # 80: řádek 24, sloupec 78 | 40: k = 999
    chars[0x7E8] = C4   # 80: řádek 24, sloupec 79
    chars[0x002] = C4   # 40: k = 2 (vzor C4); 80: sloupec 4
    # Atributy na řádku 1 (k = 40, 41), znaky mezery (kód 0):
    attrs[0x028] = 0x01  # 0D828h: 40 k=40, 80 sloupec 0 -> pozadí 1
    attrs[0x429] = 0x02  # 0DC29h: 80 sloupec 1 (po znacích) -> pozadí 2
    attrs[0x029] = 0x02  # 0D829h: 40 k=41 (vzor barvy 2); 80 sloupec 2
    for base, data in ((0xD000, chars), (0xD800, attrs)):
        for off in range(0, len(data), 0x400):
            emu.call("mem_write", {"addr": base + off,
                                   "data_hex": data[off:off + 0x400].hex()})


def glyph40(img, k, line):
    """Vzor 8 bodů znaku k (40 sloupců) na mikrořádku line (každý 2. bod)."""
    r, c = divmod(k, 40)
    return cell_bits(img, c * 16, r * 8 + line, 8, 2)


def glyph80(img, row, col, line):
    """8 bodů znaku (row, col) v 80 sloupcích."""
    return cell_bits(img, col * 8, row * 8 + line, 8, 1)


# Glyf kódu 80h pro PCG-700 (bit 7 = levý bod), asymetrický, aby šlo poznat
# převrácení bitů i pořadí řádků (stejný vzor jako test_pcg700_e2e.py).
PCG_GLYPH = bytes([0xFF, 0x80, 0xC0, 0xE0, 0xF0, 0x80, 0x80, 0xFF])
PCG_TABLE = 0x3100  # 8 bajtů glyfu pro program PCG


def _prog_pcg_suc():
    """Zapíše PCG_GLYPH do kódu 80h sady 1, zapne PCG a přepne SUC na 80 sloupců.

    DI; LD HL,PCG_TABLE; LD B,0; pro B = 0..7: data (HL) -> 0E010h,
    B -> 0E011h, 10h -> 0E012h, 00h -> 0E012h (strobe, zároveň PCG
    zapnuto); LD A,50h; LD (0E00Ch),A; JR $.
    """
    return bytes([
        0xF3,                                    # DI
        0x21, PCG_TABLE & 0xFF, PCG_TABLE >> 8,  # LD HL,PCG_TABLE
        0x06, 0x00,                              # LD B,0
        # loop:
        0x7E,                                    # LD A,(HL)
        0x32, 0x10, 0xE0,                        # LD (0E010h),A
        0x78,                                    # LD A,B
        0x32, 0x11, 0xE0,                        # LD (0E011h),A
        0x3E, 0x10,                              # LD A,10h
        0x32, 0x12, 0xE0,                        # LD (0E012h),A
        0xAF,                                    # XOR A
        0x32, 0x12, 0xE0,                        # LD (0E012h),A
        0x23,                                    # INC HL
        0x04,                                    # INC B
        0x78,                                    # LD A,B
        0xFE, 0x08,                              # CP 8
        0x20, 0xE8,                              # JR NZ,loop (-24)
        0x3E, 0x50,                              # LD A,50h
        0x32, 0x0C, 0xE0,                        # LD (0E00Ch),A
        0x18, 0xFE,                              # JR $
    ])


def check_with_pcg700(exe, tmp):
    """PCG-700 a SUC připojené naráz: 80 sloupců kreslí glyfy z PCG RAM."""
    d = tmp / (exe.stem + "_pcg")
    d.mkdir()
    emu = PipeEmu(exe, d)
    name = exe.stem + " +pcg700"
    want = [tuple((b >> (7 - i)) & 1 for i in range(8)) for b in PCG_GLYPH]
    try:
        emu.call("run", {"frames": 50})
        emu.call("pause")
        park(emu)
        for kind in ("pcg700", "suc80"):
            emu.call("periph_attach", {"kind": kind})
        dev = {x["id"]: x for x in emu.call("get_devices").get("devices", [])}
        check(dev.get("pcg700", {}).get("connected") is True
              and dev.get("suc80", {}).get("connected") is True,
              f"{name}: pcg700 and suc80 connected together")

        # VRAM: mezery, atribut 70h (sada 1); kód 80h v sudém i lichém sloupci
        # první dvojice (0D000h, 0D401h) a poslední dvojice (0D3E7h, 0D7E8h).
        chars = bytearray(0x800)
        for off in (0x000, 0x401, 0x3E7, 0x7E8):
            chars[off] = 0x80
        attrs = bytearray([ATTR_TEXT]) * 0x800
        for base, data in ((0xD000, chars), (0xD800, attrs)):
            for off in range(0, len(data), 0x400):
                emu.call("mem_write", {"addr": base + off,
                                       "data_hex": data[off:off + 0x400].hex()})

        code = _prog_pcg_suc()
        emu.call("mem_write", {"addr": PCG_TABLE, "data_hex": PCG_GLYPH.hex()})
        emu.call("mem_write", {"addr": PROG, "data_hex": code.hex()})
        emu.call("set_register", {"reg": "PC", "value": PROG})
        emu.call("run", {"frames": 3})
        emu.call("pause")
        pc = emu.call("get_registers").get("PC")
        check(pc == PROG + len(code) - 2, f"{name}: PCG + SUC program reached JR $ (pc={pc:#x})")

        img80 = screen(emu)
        for (row, col) in ((0, 0), (0, 1), (24, 78), (24, 79)):
            got = [glyph80(img80, row, col, ln) for ln in range(8)]
            check(got == want, f"{name}: 80 columns row {row} col {col} shows "
                  f"the PCG RAM glyph of 80h with 8 dots")
        check(not any(any(glyph80(img80, 0, 2, ln)) for ln in range(8)),
              f"{name}: row 0 col 2 (0D001h) is a space")

        # HSW NOM: generátor CG-ROM, obraz kódu 80h je jiný.
        emu.call("settings_set", {"key": "PCG700/hsw", "value": "nom"})
        park(emu)
        nom = [glyph80(screen(emu), 0, 0, ln) for ln in range(8)]
        check(nom != want, f"{name}: with HSW NOM 80h is drawn from the CG-ROM")
        emu.call("settings_set", {"key": "PCG700/hsw", "value": "pcg"})
        park(emu)
        check(screen(emu)[0] == img80[0], f"{name}: HSW PCG again gives the PCG image")

        # 40 sloupců: týž glyf na 16 bodů (každý druhý bod = vzor).
        run_latch(emu, 0x28)
        img40 = screen(emu)
        got = [glyph40(img40, 0, ln) for ln in range(8)]
        check(got == want, f"{name}: 40 columns shows the PCG RAM glyph")
    finally:
        emu.close()


def check_binary(exe, tmp):
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    name = exe.stem
    try:
        dev = emu.call("get_devices")
        suc = [x for x in dev.get("devices", []) if x.get("id") == "suc80"]
        check(len(suc) == 1, f"{name}: get_devices lists suc80")
        suc = suc[0]
        check(suc["connected"] is False, f"{name}: suc80 disconnected after start")
        check(suc["mmio"] == [{"first": 0xE00C, "last": 0xE00F, "when": ["mapped"],
                               "read": False, "write": True, "drive": "none", "drive_mask": 0}],
              f"{name}: suc80 MMIO 0E00Ch-0E00Fh mapped ({suc['mmio']})")
        check(suc["video"] == {"glyph_source": False, "render_row": True},
              f"{name}: suc80 video hooks ({suc['video']})")
        opt = suc["options"][0]
        check(opt["key"] == "attributes" and opt["value"] == "per_char"
              and opt["keywords"] == ["per_char", "per_pair"],
              f"{name}: option attributes ({opt})")

        # ROM monitor dokončí inicializaci, pak CPU zaparkovat a naplnit VRAM.
        emu.call("run", {"frames": 50})
        emu.call("pause")
        park(emu)
        fill_vram(emu)
        park(emu)
        img40 = screen(emu)

        # Bez zařízení: zápis 50h na 0E00Ch obraz nezmění.
        run_latch(emu, 0x50)
        check(screen(emu)[0] == img40[0],
              f"{name}: without the device a write of 50h to 0E00Ch keeps the image")

        # Připojení: 40 sloupců bajtově stejně jako bez zařízení.
        emu.call("periph_attach", {"kind": "suc80"})
        run_latch(emu, 0x28)
        check(screen(emu)[0] == img40[0],
              f"{name}: attached in 40 columns, image identical to no device")

        # Vzory glyfů a barev ze 40 sloupců.
        g = {c: [glyph40(img40, k, ln) for ln in range(8)]
             for c, k in ((C1, 0), (C2, 1), (C4, 2), (C3, 999))}
        for c, rows in g.items():
            check(any(any(r) for r in rows), f"{name}: glyph {c:#x} is not empty")
        col1 = px(img40, 0, 8)    # pozadí 1 (k = 40)
        col2 = px(img40, 16, 8)   # pozadí 2 (k = 41)
        check(col1 != col2, f"{name}: reference colours 1 and 2 differ")

        # 80 sloupců.
        run_latch(emu, 0x50)
        img80 = screen(emu)
        check(img80[0] != img40[0], f"{name}: 50h switches the image")
        for (row, col, c) in ((0, 0, C1), (0, 1, C2), (0, 2, C2), (0, 4, C4),
                              (24, 78, C3), (24, 79, C4)):
            got = [glyph80(img80, row, col, ln) for ln in range(8)]
            check(got == g[c], f"{name}: 80 columns row {row} col {col} shows "
                  f"char {c:#x} with 8-dot glyph")
        check(not any(any(glyph80(img80, 0, 3, ln)) for ln in range(8)),
              f"{name}: row 0 col 3 (0D402h) is a space")
        check(px(img80, 0, 8) == col1 and px(img80, 8, 8) == col2
              and px(img80, 16, 8) == col2,
              f"{name}: per-char attributes 0D828h / 0DC29h / 0D829h")

        # 0D400h a 0DC00h se nezobrazí.
        emu.call("mem_write", {"addr": 0xD400, "data_hex": f"{C1:02X}"})
        emu.call("mem_write", {"addr": 0xDC00, "data_hex": "12"})
        park(emu)
        check(screen(emu)[0] == img80[0], f"{name}: 0D400h / 0DC00h are not displayed")

        # Volba atributů na dvojici.
        r = emu.call("settings_set", {"key": "SUC80/attributes", "value": "per_pair"})
        check(r.get("applies") == "now", f"{name}: settings_set applies now ({r})")
        park(emu)
        imgp = screen(emu)
        check(px(imgp, 8, 8) == col1, f"{name}: per_pair takes the attribute of block A")
        emu.call("settings_set", {"key": "SUC80/attributes", "value": "per_char"})
        park(emu)
        check(screen(emu)[0] == img80[0], f"{name}: per_char again gives the 80-column image")

        # Zpět 28h.
        run_latch(emu, 0x28)
        check(screen(emu)[0] == img40[0], f"{name}: 28h returns to 40 columns")

        # Po OUT (0E1h) je na 0E000h DRAM: latch zápis 50h nevidí.
        # DI; OUT (0E1h),A; LD A,50h; LD (0E00Ch),A; OUT (0E3h),A; JR $
        emu.call("mem_write", {"addr": PROG, "data_hex": "F3D3E13E50320CE0D3E318FE"})
        emu.call("set_register", {"reg": "PC", "value": PROG})
        emu.call("run", {"frames": 2})
        emu.call("pause")
        pc = emu.call("get_registers").get("PC")
        check(pc == PROG + 10, f"{name}: DRAM program reached JR $ (pc={pc:#x})")
        check(screen(emu)[0] == img40[0],
              f"{name}: a write of 50h after OUT (0E1h) does not switch")

        # Snapshot v 80 sloupcích.
        run_latch(emu, 0x50)
        check(screen(emu)[0] == img80[0], f"{name}: 50h again gives the same 80-column image")
        snap = emu.call("snapshot_save_buffer", {"description": "suc80"})["bytes_b64"]

        # RESET -> 40 sloupců (ROM po resetu smaže obrazovku, VRAM znovu).
        emu.call("reset")
        emu.call("run", {"frames": 50})
        emu.call("pause")
        park(emu)
        fill_vram(emu)
        park(emu)
        check(screen(emu)[0] == img40[0], f"{name}: reset returns to 40 columns")

        # Snapshot round-trip.
        emu.call("snapshot_load_buffer", {"bytes_b64": snap})
        emu.call("run", {"frames": 2})
        emu.call("pause")
        check(screen(emu)[0] == img80[0], f"{name}: snapshot restores 80 columns")

        # Odpojení: zpět obraz bez zařízení.
        emu.call("periph_detach", {"kind": "suc80"})
        emu.call("run", {"frames": 2})
        emu.call("pause")
        check(screen(emu)[0] == img40[0], f"{name}: detach returns the plain image")
        r = call_raw(emu, "settings_set", {"key": "SUC80/attributes", "value": "bogus"})
        check(not r.get("success"), f"{name}: invalid keyword is refused")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=300)
    names = ("mz700emu-pal", "mz700emu-ntsc")
    found = {n: _find_exe(n) for n in names}
    if not any(found.values()):
        print("SKIP: no MZ-700 binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_suc80_e2e_"))
    ok = False
    try:
        for n in names:
            if not found[n]:
                print(f"  SKIP: {n} binary not found")
                continue
            check_binary(Path(found[n]), tmp)
            check_with_pcg700(Path(found[n]), tmp)
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
