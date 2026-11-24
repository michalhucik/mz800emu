#!/usr/bin/env python3
"""E2E test: opravy GDG podle měření na skutečném MZ-800, druhá série (MCP pipe).

Zdroj: emu-experiments gdg-meas (měření 6 až 18, 2026-11-22 a 23), v bázi
mz800-knowledge hw-measurement/vysledky.md, oddíly 17 až 23; souhrn rozdílů
emu-experiments knowledge/mz800emu/rozdily-emulatoru-proti-hw-souhrn.md
(čísla bodů A1 až F1 níže).

Řádky jsou řádky rastru emulátoru (canvas 46 až 245, na stroji řádky od
sestupné hrany VSYNC 68 až 267), sloupce od začátku viditelné části
(sloupec 0 = 186 px po sestupné hraně HSYNC). S je řádek canvasu.

Kroky:
  1  (A5) přepnutí z režimu MZ-700 do 320x200 v levém borderu řádku S:
     řádek S ukáže řádek VRAM S div 8, každý další řádek o 1 víc (jeden
     čítač adresy řádku, na stroji 24 z 24 přepnutí);
  2  (A6) přepnutí z 320x200 do režimu MZ-700 v levém borderu řádku S:
     text od offsetu (40 * S) mod 1024, +40 na každém řádku canvasu
     dělitelném 8, offset buňky přetéká na 1024;
  3  (A9) paleta po návratu z režimu MZ-700: okno konce M-cyklu zápisu DMD
     163 až 802, buňka (E - 163) div 16 (na stroji /IORQ v 333 až 973 px,
     buňka (P - 333) div 16);
  4  (A10, A11) paleta po návratu na řádcích mimo canvas: atribut podle
     čítače, který běží dál (pod canvasem textový řádek 25 s přetečením
     buněk 24 až 39 na offset 0 až 15, pak bez nulování přes konec snímku);
  5  (B1, B2) režim MZ-700: čtení 0E009h až 0E00Fh vrací poslední bajt na
     datové sběrnici (E0h, 7Eh, 1Ah, 05h, L = H = E0h podle instrukce),
     v režimu MZ-800 FFh; při zákazu vybere JP 0E010h bajt E0h (RET PO).
  6  (C1) sestupná hrana CLK0 obvodu 8253 (1M1 = CKMS) leží ve sloupcích
     16k + 6, tedy 0 až 0,2 px po sestupné hraně HSYNC jako na stroji
     (OUT0 v režimu 3 se mění jen na těchto hranách).
  7  (A7) přepnutí z a do režimu MZ-700 uprostřed řádku: registr DMD se
     taktuje ve sloupcích 148 + 16k (334 + 16k px) a převezme zápis, jehož
     M-cyklus skončí nejvýš 14 sloupců za hranou; nový režim se ukáže 22
     sloupců po hraně, u MZ-700 -> 640x200 po 12 (měření U7: 340 + 16k,
     resp. 346 + 16k px).
  8  (A1, A2) režim 640: canvas ve sloupcích 152 až 791 (338 až 977 px),
     border končí o 1 px dřív (sloupec 927 je zatemnění); začátek canvasu
     se řídí režimem v okamžiku hrany (zápis DMD s koncem M-cyklu do
     sloupce 162, na stroji /IORQ do 332,5 px).
 16  konec pravého borderu se zkrácením řádku (CKSW = 1) a s režimem 640
     se sčítá: první sloupec zatemnění 928, 927, 912 a 911 pro DMD 00h / 04h
     a CKSW 0 / 1 (na stroji 1114, 1113, 1098 a 1097 px, měření 15); totéž
     hned po načtení snapshotu (úplné překreslení).
 17  paleta po návratu z DMD 0Ch až 0Fh do grafiky nese barvy bajtu čtvrtého
     čtení buňky pod paprskem (mimo okno buňky 39), po návratu z DMD 08h dál
     atribut (C model netlistu; na stroji neměřeno).
  9  (A12) HW scroll s SW, SSA a SEA, které porušují SW = SEA - SSA, se
     nevypne a obraz jde podle vzorce posunu adres (měření U4).
 12  (A8) přepnutí 320 / 640 uprostřed řádku: sloupce 148 až 153 + 16k
     podle C modelu netlistu ve variantě shodné s U19 (640 -> 320: úsek
     položky 3 o 4 px u 00h / FFh a 2 px u AAh / 55h; 320 -> 640: 2 px
     černá, 1 px položka 1).
 13  (U8) zápis CPU do VRAM se v řádku zápisu ukáže jen v buňkách, které
     video fetch ještě nepřečetl (buňka k asi 33,5 px před 340 + 16k px;
     na stroji T1 nejvýš 626,5 px pro buňku v 660 px / 658 px v 640x200).
 10  (A3, A4) DMD 0Ch až 0Fh podle C modelu netlistu: buňka = vzor znaku
     v barvách atributu a atribut další buňky v barvách čtvrtého čtení
     (pravidlo sedí se záznamy U5 ze stroje bod po bodu), poloha jako
     640x200; adresa CPU A jde na offset (A >> 1) & 1FFFh.
 15  snapshot s čítačem adresy řádku mimo rozsah snímku (poškozený) se načte se
     stavem podle DMD; grafika nečte mimo rovinu VRAM.
 14  stopa iorqlog nese délku pulzu /IORQ (2,5 T, s čekacími takty delší);
     dřív byla vždy 0.
 11  (F1) MZ-800 bez rozšíření VRAM MZ-1R25 ([MZ800] exvram = 0): roviny
     III a IV se čtou jako FFh a zápis do nich se ztratí, frame B (DMD 01h)
     má jen barvu položky 3; snapshot volbu nese.

Jen MZ-800. Binárka se hledá v kořeni repa nebo v adresáři z proměnné
``MZ_EMU_DIR``. Proces běží izolovaně (dočasný profil); zabíjí se jen vlastní
PID. Výstup anglicky.

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
import test_gdg_cksw as h  # noqa: E402 - PipeEmu a příprava stroje

_ = gettext.gettext

#: První řádek a první sloupec canvasu (mz800_video.h).
_CANVAS_ROW = 46
_CANVAS_COL = 154

#: Adresa testovacího kódu (mimo smyčku 3000h a NOP 3100h až 50FFh).
_CODE = 0x5200


def _gdg(emu):
    """Stav GDG (get_periph_gdg)."""
    return emu.call("get_periph_gdg")


def _frame(emu):
    """Poslední dokončený snímek: (šířka, funkce bod(x, y) -> RGB bajty)."""
    r = emu.call("get_frame_screenshot_raw")
    if not r.get("available"):
        raise h.TestFailure("framebuffer is not available")
    w, bpp = r["width"], r["bytes_per_pixel"]
    data = base64.b64decode(r["data_b64"])

    def px(x, y):
        o = (y * w + x) * bpp
        return data[o:o + 3]
    return w, px


def _run_code(emu, body_hex, frames):
    """Spustí kód na _CODE (zakončený JR $) a vrátí PC do smyčky."""
    emu.call("mem_write", {"addr": _CODE, "data_hex": body_hex + "18fe"})
    emu.call("set_register", {"reg": "PC", "value": _CODE})
    emu.call("run", {"frames": frames})
    emu.call("set_register", {"reg": "PC", "value": h._LOOP})


def _pad_hex(delta):
    """Instrukce bez vlivu na A, B, C o délce ``delta`` pxCLK.

    NOP = 4 T (20 px), INC DE = 6 T (30 px), LD E,n = 7 T (35 px); složí se
    každý násobek 5 kromě 5, 10, 15, 25 a 45. Vrací (hex, počet instrukcí).
    """
    for ld in range(2):
        for inc in range(3):
            rest = delta - 35 * ld - 30 * inc
            if rest >= 0 and rest % 20 == 0:
                return "1e00" * ld + "13" * inc + "00" * (rest // 20), ld + inc + rest // 20
    raise ValueError(delta)


def _out_at(emu, port, value, line, e_min, e_max):
    """OUT (C),A na řádku ``line`` tak, aby M-cyklus skončil ve sloupci e_min až e_max.

    Doběhne na řádek o něco dřív, doplní čekací instrukce a OUT provede
    krokováním. Vrací skutečný sloupec konce M-cyklu (poloha po instrukci).
    Rozsah musí mít aspoň 5 sloupců (instrukce jdou po 5 pxCLK).
    """
    pos = h._goto(emu, line, max(0, e_min - 165))
    e0 = pos["col"] + 60
    delta = next(d for d in range(0, 400, 5)
                 if e_min <= e0 + d <= e_max and d not in (5, 10, 15, 25, 45))
    pad, n = _pad_hex(delta)
    jp = "c3%02x%02x" % (h._LOOP & 0xFF, h._LOOP >> 8)
    emu.call("mem_write", {"addr": _CODE, "data_hex": pad + "ed79" + jp})
    emu.call("set_register", {"reg": "BC", "value": port})
    emu.call("set_register", {"reg": "AF", "value": value << 8})
    emu.call("set_register", {"reg": "PC", "value": _CODE})
    for _i in range(n + 1):
        emu.call("step_into")
    end = h._pos(emu)
    if end["line"] != line:
        raise h.TestFailure(f"OUT ended on line {end['line']}, expected {line}")
    return end["col"]


def _fill_ruler(emu):
    """Grafika: roviny I a II nulové, bajt 0 řádku VRAM r roviny I = r (r = 0 až 199)."""
    body = ("dbe0"
            "3e03d3cc" "210080" "3600" "110180" "013f1f" "edb0"       # I + II = 0
            "3e01d3cc" "210080" "112800" "af" "77" "19" "3c" "fec8" "20f9"  # pravítko
            "dbe1")
    _run_code(emu, body, 8)


def _ruler_value(px, row):
    """Bajt 0 řádku podle obrazu 320x200: bod položky palety 1 (černá) = bit 1, bit 0 vlevo."""
    v = 0
    for i in range(8):
        if px(_CANVAS_COL + 2 * i, row) == bytes(3):
            v |= 1 << i
    return v


def _text_attrs(emu, attr_of_offset):
    """Režim MZ-700: atributy 0D800h až 0DBFFh podle funkce offsetu, znaky 00h.

    Nastaví mapování po resetu (OUT E4h), aby na 0D000h byla VRAM."""
    emu.call("io_write", {"port": 0xE4, "value": 0})
    emu.call("mem_write", {"addr": 0xD000, "data_hex": "00" * 1024})
    emu.call("mem_write", {"addr": 0xD800, "data_hex": "".join("%02x" % attr_of_offset(p) for p in range(1024))})


def _palette_entries(emu):
    """Položky palety 3, 1, 2, 0."""
    pal = _gdg(emu)["palette"]
    return [pal[3], pal[1], pal[2], pal[0]]


def _mz700_color(attr, fg):
    """Barva režimu MZ-700 z atributu (popředí / pozadí): nenulová s intenzitou."""
    c = (attr >> 4) & 7 if fg else attr & 7
    return (c | 8) if c else 0


def _expect_entries(attr):
    """Položky 3, 1, 2, 0 po návratu uvnitř okna: popředí, popředí, pozadí, pozadí."""
    f, b = _mz700_color(attr, True), _mz700_color(attr, False)
    return [f, f, b, b]


def _step_row_counter_700_to_320(emu):
    """Krok 1 (A5)."""
    _fill_ruler(emu)
    for s in (20, 101):
        emu.call("io_write", {"port": 0xCE, "value": 0x08})
        emu.call("run", {"frames": 2})
        row = _CANVAS_ROW + s
        e = _out_at(emu, 0xCE, 0x00, row, 80, 100)
        # zbytková paleta po návratu mimo okno závisí na CG-RAM znaku 0 (tady pravítko, tedy černá);
        # pravítko potřebuje položku 0 bílou a položku 1 černou ještě před canvasem řádku S
        emu.call("io_write", {"port": 0xF0, "value": 0x0F})
        emu.call("io_write", {"port": 0xF0, "value": 0x10})
        emu.call("run", {"frames": 1})
        _w, px = _frame(emu)
        got = [_ruler_value(px, row + k) for k in (0, 1, 2, 8)]
        want = [s // 8 + k for k in (0, 1, 2, 8)]
        h.check(got == want, f"MZ-700 -> 320x200 in the left border of canvas row {s} (M-cycle end {e}): "
                             f"rows S, S+1, S+2, S+8 show VRAM rows {got}, expected {want}")
        last = _ruler_value(px, _CANVAS_ROW + 199)
        h.check(last == s // 8 + 199 - s, f"last canvas row shows VRAM row {last}, expected {s // 8 + 199 - s}")
        emu.call("run", {"frames": 1})
        _w, px = _frame(emu)
        got = [_ruler_value(px, row + k) for k in (0, 1)]
        h.check(got == [s, s + 1], f"next frame entirely in 320x200: rows S, S+1 show {got}, expected {[s, s + 1]}")


def _attr_row_color(p):
    """Atribut pro krok 2: barva = textový řádek mod 8 (popředí = pozadí)."""
    k = (p // 40) % 8 if p < 1000 else 25 % 8
    return (k << 4) | k


def _step_row_counter_320_to_700(emu):
    """Krok 2 (A6)."""
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    _text_attrs(emu, _attr_row_color)
    emu.call("run", {"frames": 2})
    _w, px = _frame(emu)
    # referenční barvy: textový řádek k (k = 0 až 7) na řádku canvasu 8k
    ref = {px(_CANVAS_COL, _CANVAS_ROW + 8 * k): k for k in range(8)}
    h.check(len(ref) == 8, f"reference frame in mode 700 has 8 distinct row colors (got {len(ref)})")

    def color(row, cell):
        return ref.get(px(_CANVAS_COL + 16 * cell + 8, row), -1)

    for s in (20, 50):
        emu.call("io_write", {"port": 0xCE, "value": 0x00})
        emu.call("run", {"frames": 2})
        row = _CANVAS_ROW + s
        e = _out_at(emu, 0xCE, 0x08, row, 80, 100)
        emu.call("run", {"frames": 1})
        _w, px = _frame(emu)
        checks = []
        for c in range(s, s + 12):
            base = (40 * s + 40 * (c // 8 - s // 8)) % 1024
            for cell in (0, 8, 24, 39):
                p = (base + cell) % 1024
                checks.append((c, cell, color(_CANVAS_ROW + c, cell), _attr_row_color(p) & 7))
        bad = [x for x in checks if x[2] != x[3]]
        h.check(not bad, f"320x200 -> MZ-700 in the left border of canvas row {s} (M-cycle end {e}): "
                         f"text offset (40 * S) mod 1024, +40 per 8 rows, wraps at 1024 "
                         f"(mismatches row/cell/got/expected: {bad[:6]})")


def _attr_cell(p):
    """Atribut pro krok 3: podle buňky, sousední buňky se liší popředím."""
    x = p % 40
    return ((1 + x % 7) << 4) | ((x // 7) % 8)


def _return_palette(emu, line, e_min, e_max):
    """Snímek v režimu MZ-700, návrat DMD 00h s koncem M-cyklu v e_min až e_max.

    Vrací (sloupec konce M-cyklu, položky 3, 1, 2, 0)."""
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    e = _out_at(emu, 0xCE, 0x00, line, e_min, e_max)
    pal = _palette_entries(emu)
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    return e, pal


def _cg0_fill(emu, value):
    """Vzor znaku 0 v CG-RAM (rovina I, offset 0 až 7) = ``value``; zapisuje v DMD 00h, končí v DMD 08h."""
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    _run_code(emu, "dbe0" "3e01d3cc" "210080" "0608" "36%02x" "23" "10fb" "dbe1" % value, 2)
    emu.call("io_write", {"port": 0xCE, "value": 0x08})


def _step_palette_window(emu):
    """Krok 3 (A9)."""
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    _text_attrs(emu, _attr_cell)
    # mimo okno zůstanou v paletě barvy bajtu vzoru znaku 0 z CG-RAM (0C000h + řádek vzoru):
    # výplň 0Fh dá 0, 0, F, F jako v měřeních 5 až 9; po kroku se vrátí nuly z _fill_ruler()
    _cg0_fill(emu, 0x0F)
    emu.call("run", {"frames": 2})
    seen = set()
    for lo, hi in ((156, 170), (174, 186), (795, 810)):
        for e_min in range(lo, hi - 3):
            e, pal = _return_palette(emu, 100, e_min, e_min + 4)
            if e in seen:
                continue
            seen.add(e)
            if 163 <= e <= 802:
                cell = (e - 163) // 16
                want = _expect_entries(_attr_cell(cell))
            else:
                cell, want = None, [0, 0, 0xF, 0xF]
            h.check(pal == want, f"return from mode 700 with M-cycle end {e}: entries 3, 1, 2, 0 = {pal}, "
                                 f"expected {want} ({'cell %d' % cell if cell is not None else 'outside the window'})")
    for need in ((161, 162), (179, 180), (801, 802), (803, 804)):
        h.check(any(e in seen for e in need), f"window sweep covered column {need[0]} or {need[1]}")
    _cg0_fill(emu, 0x00)


def _attr_row25(p):
    """Atribut pro krok 4: offset 0 až 15 = 35h, 1000 až 1023 = 26h, jinak 71h."""
    return 0x35 if p < 16 else 0x26 if p >= 1000 else 0x71


def _step_palette_outside(emu):
    """Krok 4 (A10, A11)."""
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    _text_attrs(emu, _attr_row25)
    emu.call("run", {"frames": 2})
    cases = (
        (_CANVAS_ROW + 202, 30, 0x35, "text row 25 below the canvas, cell 30 -> offset 6"),
        (_CANVAS_ROW + 202, 20, 0x26, "text row 25 below the canvas, cell 20 -> offset 1020"),
        (_CANVAS_ROW + 208, 10, 0x71, "first row after text row 25, cell 10 -> offset 26 (no reset)"),
        (40, 10, 0x71, "above the canvas (counter 38), cell 10 -> offset 506"),
        (_CANVAS_ROW + 100, 10, 0x71, "inside the canvas, cell 10"),
    )
    for line, cell, attr, what in cases:
        e_min = 163 + 16 * cell + 4
        emu.call("run", {"frames": 1})
        e, pal = _return_palette(emu, line, e_min, e_min + 6)
        want = _expect_entries(attr)
        h.check(pal == want, f"palette after mode 700 on row {line}, {what} (M-cycle end {e}): "
                             f"entries 3, 1, 2, 0 = {pal}, expected {want}")


def _peek(emu, addr, n=1):
    """Bajty paměti tak, jak je vidí debugger (mem_read)."""
    r = emu.call("mem_read", {"addr": addr, "len": n})
    return list(base64.b64decode(r["data_b64"]))


def _step_open_bus(emu):
    """Krok 5 (B1, B2)."""
    emu.call("io_write", {"port": 0xE4, "value": 0})        # mapování po resetu
    emu.call("io_write", {"port": 0xCE, "value": 0x08})     # režim MZ-700
    emu.call("mem_write", {"addr": 0x3200, "data_hex": "00" * 16})
    body = ("3a09e0" "320032"                 # LD A,(0E009h); LD (3200h),A
            "210ae0" "7e" "320132"            # LD HL,0E00Ah; LD A,(HL); LD (3201h),A
            "110fe0" "1a" "320232"            # LD DE,0E00Fh; LD A,(DE); LD (3202h),A
            "dd2109e0" "dd7e05" "320332"      # LD IX,0E009h; LD A,(IX+5); LD (3203h),A
            "2a09e0" "220532")                # LD HL,(0E009h); LD (3205h),HL
    _run_code(emu, body, 1)
    got = _peek(emu, 0x3200, 4) + _peek(emu, 0x3205, 2)
    want = [0xE0, 0x7E, 0x1A, 0x05, 0xE0, 0xE0]
    h.check(got == want, f"mode 700, 0E009h to 0E00Fh read the last byte on the data bus: "
                         f"LD A,(nn), LD A,(HL), LD A,(DE), LD A,(IX+5), LD HL,(nn) -> "
                         f"{['%02X' % v for v in got]}, expected {['%02X' % v for v in want]}")
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    _run_code(emu, "3a09e0" "320632", 1)
    v = _peek(emu, 0x3206)[0]
    h.check(v == 0xFF, f"mode 800, 0E009h reads FFh (got {v:02X}h)")
    # Zákaz (OUT 0E5h): JP 0E010h vybere poslední bajt na sběrnici = E0h
    # (vyšší bajt adresy skoku) a provede RET PO; RAM před tím čtená přes
    # rychlou cestu má jinou hodnotu (55h).
    emu.call("mem_write", {"addr": 0x1234, "data_hex": "55"})
    emu.call("mem_write", {"addr": 0x3300, "data_hex": "0053"})       # návratová adresa 5300h
    emu.call("mem_write", {"addr": 0x5300, "data_hex": "320732" "d3e4" "18fe"})
    emu.call("io_write", {"port": 0xE5, "value": 0})
    emu.call("set_register", {"reg": "SP", "value": 0x3300})
    _run_code(emu, "3e01" "b7" "3a3412" "c310e0", 1)   # LD A,1; OR A (PO); LD A,(1234h); JP 0E010h
    emu.call("io_write", {"port": 0xE4, "value": 0})
    v = _peek(emu, 0x3207)[0]
    h.check(v == 0x55, f"prohibited area: JP 0E010h fetches E0h (RET PO) and returns to 5300h "
                       f"(marker {v:02X}h, expected 55h)")
    emu.call("io_write", {"port": 0xE0, "value": 0})
    emu.call("io_write", {"port": 0xE1, "value": 0})


def _read_all(emu, channel, columns=None, flt=None):
    """Přečte celý kanál stopy; vrátí řádky."""
    rows, cursor = [], None
    while True:
        req = {"channel": channel, "limit": 2000}
        if columns:
            req["columns"] = columns
        if flt:
            req["filter"] = flt
        if cursor:
            req["cursor"] = cursor
        tr = emu.call("trace_read", req, timeout=120.0)
        rows += tr["rows"]
        if tr.get("end_of_data"):
            return rows
        cursor = tr["next_cursor"]


def _step_ctc0_phase(emu):
    """Krok 6 (C1): sestupné hrany CLK0 ve sloupcích 16k + 6."""
    emu.call("audio_capture_start", {"frames": 8, "events": False, "psg_writes": False,
                                     "reference_wav": False, "sdl_wav": False, "pcm": []})
    emu.call("run", {"frames": 1})
    emu.call("trace_start", {"channel": "iorqlog"})
    # PC0 = 1 (OUT0 do zvukové cesty, jinak ho záznam úrovní nevidí); CTC0
    # režim 3, dělič 16 (OUT0 se mění po 8 hranách CLK0); v režimu MZ-800
    # 8255 na D0h až D3h, 8253 na D4h až D7h
    _run_code(emu, "3e01d3d3" "3e16d3d7" "3e10d3d4", 2)
    io = _read_all(emu, "iorqlog", ["t", "pos", "dir", "addr"])
    outs = [(t, pos) for t, pos, d, addr in io if d == "out" and addr[-2:].upper() == "D4"]
    lv = _read_all(emu, "audio_levels", ["t", "level"], {"channels": ["ctc0"]})
    emu.call("audio_capture_stop")
    emu.call("trace_stop", {"channel": "iorqlog"})
    if not outs:
        raise h.TestFailure("no OUT to port D4h in the I/O trace")
    t_w, pos_w = outs[0]
    base = (t_w - pos_w) % 1136
    changes, prev = [], None
    for t, level in lv:
        level = 1 if level else 0
        if prev is not None and level != prev and t > t_w:
            changes.append(t)
        prev = level
    phases = sorted({((t - base) % 1136) % 16 for t in changes})
    h.check(len(changes) > 100 and phases == [6],
            f"CTC0 mode 3: OUT0 changes on CLK0 falling edges in columns 16k + 6, "
            f"0 to 0.2 px after the HSYNC edge as on the real MZ-800 "
            f"({len(changes)} changes, columns mod 16 = {phases})")


def _switch_x(e, show):
    """Očekávaný první sloupec nového režimu: hrana registru DMD 148 + 16k nejvýš 14 sloupců před E, plus odstup."""
    k = -((148 - (e - 14)) // 16)
    return 148 + 16 * k + show


def _step_mode700_switch(emu):
    """Krok 7 (A7): přepnutí z a do režimu MZ-700 uprostřed řádku."""
    # Na MZ-800 leží CG-RAM, text i atributy režimu MZ-700 v rovině I. Roviny
    # I a II jsou nulové (znak 00h bez bodů), FFh jen na adresách, které čte
    # grafika v řádku 120 (řádek canvasu 74): čítač 74 v grafickém snímku
    # (8B90h) a 9 po přepnutí z režimu MZ-700 (8168h).
    _run_code(emu, "dbe0" "3e03d3cc"
                   "210080" "3600" "110180" "013f1f" "edb0"
                   "216881" "36ff" "116981" "012700" "edb0"
                   "21908b" "36ff" "11918b" "012700" "edb0"
                   "dbe1", 8)
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    _text_attrs(emu, lambda p: 0x71)                                     # popředí 7, pozadí 1 (barva 9)
    line = 120
    cases = (
        (0x00, 0x08, 22, "320x200 -> MZ-700"),
        (0x04, 0x08, 22, "640x200 -> MZ-700"),
        (0x08, 0x00, 22, "MZ-700 -> 320x200"),
        (0x08, 0x04, 12, "MZ-700 -> 640x200"),
    )
    for before, after, show, what in cases:
        seen = []
        for e_min in range(400, 440, 5):
            emu.call("io_write", {"port": 0xCE, "value": before})
            for value in (0x00, 0x14, 0x22, 0x33):        # paleta 0, 4, 2, 3 (po návratu ji přepíše atribut)
                emu.call("io_write", {"port": 0xF0, "value": value})
            emu.call("run", {"frames": 2})
            e = _out_at(emu, 0xCE, after, line, e_min, e_min + 4)
            emu.call("run", {"frames": 1})
            _w, px = _frame(emu)
            new = px(780, line)
            old = px(154, line)
            first = next((x for x in range(154, 781) if px(x, line) == new), None)
            want = _switch_x(e, show)
            seen.append(e)
            h.check(old != new and first == want,
                    f"{what}: write ending in column {e}, new mode from column {first}, expected {want} "
                    f"(DMD edge 148 + 16k at most 14 columns before the end, +{show})")
        h.check(len({_switch_x(e, 0) for e in seen}) >= 2, f"{what}: writes cover two DMD edges ({seen})")
    emu.call("io_write", {"port": 0xCE, "value": 0x00})


def _step_geometry_640(emu):
    """Krok 8 (A1, A2): poloha canvasu a borderu v režimech 640."""
    # roviny I a II celé FFh: každý bod canvasu má barvu položky 1 (640) / 3 (320)
    _run_code(emu, "dbe0" "3e03d3cc" "210080" "36ff" "110180" "013f1f" "edb0" "dbe1", 8)
    for value in (0x00, 0x14, 0x24, 0x34):          # položky 1 až 3 = barva 4
        emu.call("io_write", {"port": 0xF0, "value": value})
    emu.call("io_write", {"port": 0x06CF, "value": 0x02})   # border barva 2
    line = 100

    def row_colors(px):
        return {x: px(x, line) for x in (151, 152, 153, 154, 791, 792, 793, 794, 926, 927)}

    # 320, 640 a znovu 320 (bez změny barvy borderu: sloupce 152, 153, 792, 793 a 927 se musí
    # překreslit i tak, jinak v nich zůstane obsah z dřívějších snímků)
    for dmd, canvas, what in ((0x00, (154, 793), "320x200"), (0x04, (152, 791), "640x200"),
                              (0x00, (154, 793), "320x200 after 640x200")):
        emu.call("io_write", {"port": 0xCE, "value": dmd})
        emu.call("run", {"frames": 2})
        _w, px = _frame(emu)
        c = row_colors(px)
        border, fill = px(100, line), px(400, line)
        got_canvas = [x for x in sorted(c) if c[x] == fill]
        want_canvas = [x for x in sorted(c) if canvas[0] <= x <= canvas[1]]
        h.check(border != fill and got_canvas == want_canvas,
                f"{what}: canvas columns {canvas[0]} to {canvas[1]} (got {got_canvas} of the sampled columns)")
        last = c[927]
        want_last = bytes(3) if dmd == 0x04 else border
        h.check(last == want_last, f"{what}: column 927 (1113 px) is {'blanking' if dmd == 0x04 else 'border'}")
    # začátek canvasu podle režimu v okamžiku hrany: zápis DMD 04h s koncem
    # M-cyklu do sloupce 162 (P <= 332,5 px) dá canvas od 152, později od 154
    for e_min, e_max, start, before, after in ((150, 162, 152, 0x00, 0x04), (165, 175, 154, 0x00, 0x04),
                                               (150, 162, 154, 0x04, 0x00), (163, 175, 152, 0x04, 0x00)):
        emu.call("io_write", {"port": 0xCE, "value": before})
        emu.call("run", {"frames": 2})
        e = _out_at(emu, 0xCE, after, line, e_min, e_max)
        emu.call("run", {"frames": 1})
        _w, px = _frame(emu)
        fill = px(400, line)
        first = next((x for x in range(140, 170) if px(x, line) == fill), None)
        h.check(first == start, f"DMD {before:02X}h -> {after:02X}h with M-cycle end {e}: canvas starts at "
                                f"column {first}, expected {start}")
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    emu.call("io_write", {"port": 0x06CF, "value": 0x00})


def _step_border_end_cksw(emu, tmp):
    """Krok 16: konec pravého borderu pro DMD 00h / 04h a CKSW 0 / 1, i hned po načtení snapshotu."""
    tmp = Path(tmp or tempfile.mkdtemp(prefix="gdg_bend_"))
    _run_code(emu, "dbe0" "3e03d3cc" "210080" "36ff" "110180" "013f1f" "edb0" "dbe1", 8)
    for value in (0x00, 0x14, 0x24, 0x34):          # položky 1 až 3 = barva 4
        emu.call("io_write", {"port": 0xF0, "value": value})
    emu.call("io_write", {"port": 0x06CF, "value": 0x02})   # border barva 2
    line = 100

    def first_black(px, width):
        """První sloupec zatemnění za pravým borderem (šířka framebufferu = žádné)."""
        border = px(100, line)
        return 1 + max(x for x in range(800, width) if px(x, line) == border)

    try:
        for cksw in (0, 1):
            h._cksw(emu, cksw)
            for dmd in (0x00, 0x04):
                emu.call("io_write", {"port": 0xCE, "value": dmd})
                emu.call("run", {"frames": 3})
                width, px = _frame(emu)
                want = width - 16 * cksw - (1 if dmd == 0x04 else 0)
                what = f"DMD {dmd:02X}h, CKSW {cksw}"
                got = first_black(px, width)
                h.check(got == want, f"{what}: right border ends at column {got}, expected {want} "
                                     f"({186 + want} px after the HSYNC fall)")
                black = [x for x in range(want, width) if px(x, line) != bytes(3)]
                h.check(not black, f"{what}: columns {want} to {width - 1} are blanking (not black: {black[:4]})")
                snap = {"path": str(tmp / "border-end.mzs")}
                emu.call("snapshot_save", dict(snap, description="border end"))
                emu.call("snapshot_load", snap)
                width, px = _frame(emu)
                got = first_black(px, width)
                h.check(got == want, f"{what}: right after a snapshot load the right border ends at column {got}, "
                                     f"expected {want}")
                black = [x for x in range(want, width) if px(x, line) != bytes(3)]
                h.check(not black, f"{what}: after a snapshot load columns {want} to {width - 1} are blanking "
                                   f"(not black: {black[:4]})")
    finally:
        h._cksw(emu, 0)
        emu.call("io_write", {"port": 0xCE, "value": 0x00})
        emu.call("io_write", {"port": 0x06CF, "value": 0x00})
        emu.call("run", {"frames": 2})


def _step_palette_after_dmd0c(emu):
    """Krok 17: paleta po návratu z DMD 0Ch nese bajt čtvrtého čtení, z DMD 08h atribut."""
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    emu.call("io_write", {"port": 0xE4, "value": 0})
    _run_code(emu, "dbe0", 1)                       # CG-RAM a text do adresního prostoru
    for page in range(0xC000, 0xD000, 256):
        emu.call("mem_write", {"addr": page, "data_hex": "00" * 256})
    # znak 40h: vzor A5h, čtvrté čtení (A5h >> 5) * 100h + 00h + r = 0C500h + r
    # znak 41h (jen buňka 39): vzor A5h, čtvrté čtení 0C508h + r
    emu.call("mem_write", {"addr": 0xC200, "data_hex": "a5" * 16})
    emu.call("mem_write", {"addr": 0xC500, "data_hex": "36" * 8 + "52" * 8})
    emu.call("mem_write", {"addr": 0xD000, "data_hex": ("40" * 39 + "41") * 25 + "40" * 24})
    emu.call("mem_write", {"addr": 0xD800, "data_hex": "25" * 1024})
    line = _CANVAS_ROW + 54
    cases = ((0x08, 270, 275, [0xA, 0xA, 0xD, 0xD], "attribute 25h of the cell"),
             (0x0C, 270, 275, [0xB, 0xB, 0xE, 0xE], "fourth byte 36h of the cell"),
             (0x0C, 510, 515, [0xB, 0xB, 0xE, 0xE], "fourth byte 36h of the cell"),
             (0x0F, 390, 395, [0xB, 0xB, 0xE, 0xE], "fourth byte 36h of the cell"),
             (0x0C, 80, 100, [0xD, 0xD, 0xA, 0xA], "fourth byte 52h of cell 39 of the previous line"),
             (0x0C, 880, 900, [0xD, 0xD, 0xA, 0xA], "fourth byte 52h of cell 39 of this line"))
    for mode, e_min, e_max, want, what in cases:
        emu.call("io_write", {"port": 0xCE, "value": 0x00})
        for value in (0x04, 0x12, 0x21, 0x3F):
            emu.call("io_write", {"port": 0xF0, "value": value})
        emu.call("io_write", {"port": 0xCE, "value": mode})
        emu.call("run", {"frames": 2})
        e = _out_at(emu, 0xCE, 0x00, line, e_min, e_max)
        pal = _palette_entries(emu)
        h.check(pal == want, f"return from DMD {mode:02X}h with M-cycle end {e}: entries 3, 1, 2, 0 = {pal}, "
                             f"expected {want} ({what})")
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    for page in range(0xC000, 0xD000, 256):
        emu.call("mem_write", {"addr": page, "data_hex": "00" * 256})
    _run_code(emu, "dbe1", 1)
    emu.call("io_write", {"port": 0xCE, "value": 0x00})


def _step_scroll_sw(emu):
    """Krok 9 (A12): SW, SSA a SEA porušující SW = SEA - SSA scroll nevypnou."""
    _fill_ruler(emu)
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    for value in (0x0F, 0x10, 0x20, 0x30):          # položka 0 = barva F, ostatní 0 (černá)
        emu.call("io_write", {"port": 0xF0, "value": value})
    for sw, what in ((0x7D, "SW = SEA - SSA"), (0x7C, "SW = 7Ch != SEA - SSA")):
        for port, value in ((0x04CF, 0x00), (0x05CF, 0x7D), (0x03CF, sw), (0x02CF, 0x00), (0x01CF, 0x05)):
            emu.call("io_write", {"port": port, "value": value})
        emu.call("run", {"frames": 2})
        _w, px = _frame(emu)
        got = [_ruler_value(px, _CANVAS_ROW + r) for r in (0, 1, 100)]
        h.check(got == [1, 2, 101], f"HW scroll SSA 00h, SEA 7Dh, {what}, SOF 1 row: canvas rows 0, 1, 100 "
                                    f"show VRAM rows {got}, expected [1, 2, 101]")
    emu.call("io_write", {"port": 0x01CF, "value": 0x00})
    emu.call("io_write", {"port": 0x03CF, "value": 0x7D})


class _IniPipeEmu(h.PipeEmu):
    """PipeEmu s vlastním obsahem INI (jinak stejný start jako h.PipeEmu)."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp, ini_text):
        import queue
        import subprocess
        import threading
        ini = tmp / "emu.ini"
        ini.write_text(ini_text, encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(h._REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        self.dead = False
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise h.TestFailure("no hello from emulator")


def _plane3_roundtrip(emu):
    """Zápis 00h do roviny III (WF SINGLE) a čtení zpět (RF roviny III); vrátí přečtený bajt."""
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    emu.call("mem_write", {"addr": 0x3210, "data_hex": "55"})
    _run_code(emu, "dbe0" "3e04d3cc" "d3cd" "af" "320080" "3a0080" "321032" "dbe1", 1)
    return _peek(emu, 0x3210)[0]


def _frame_b_colors(emu):
    """DMD 01h (frame B, roviny III a IV) s paletou 0, 1, 2, 3 = barvy 0, 9, 0Ah, 0Ch: barvy řádku 100."""
    for value in (0x00, 0x19, 0x2A, 0x3C):
        emu.call("io_write", {"port": 0xF0, "value": value})
    _run_code(emu, "dbe0" "3e0cd3cc" "210080" "3600" "110180" "013f1f" "edb0" "dbe1", 8)  # III, IV = 0
    emu.call("io_write", {"port": 0xCE, "value": 0x01})
    emu.call("run", {"frames": 2})
    _w, px = _frame(emu)
    colors = {px(x, _CANVAS_ROW + 100) for x in range(_CANVAS_COL, _CANVAS_COL + 640, 7)}
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    return colors


def _step_exvram(emu, exe):
    """Krok 11 (F1): MZ-800 bez rozšíření VRAM MZ-1R25."""
    v = _plane3_roundtrip(emu)
    h.check(v == 0x00, f"with the VRAM extension, plane III keeps a written 00h (read {v:02X}h)")
    with_ext = _frame_b_colors(emu)
    h.check(len(with_ext) == 1, f"with the VRAM extension, frame B with planes III and IV = 00h has one color")
    tmp = Path(tempfile.mkdtemp(prefix="gdg_hw_fixes_noext_"))
    emu2 = _IniPipeEmu(exe, tmp, "[MZ800]\nexvram = 0\n")
    try:
        h._setup(emu2)
        v = _plane3_roundtrip(emu2)
        h.check(v == 0xFF, f"without the VRAM extension, plane III reads FFh after writing 00h (read {v:02X}h)")
        no_ext = _frame_b_colors(emu2)
        h.check(len(no_ext) == 1 and no_ext != with_ext,
                f"without the VRAM extension, frame B shows only palette entry 3 ({len(no_ext)} color(s), "
                f"different from entry 0: {no_ext != with_ext})")
        st = emu2.call("snapshot_save", {"path": str(tmp / "noext.mzs")})
        del st
    finally:
        emu2.close()
    # snapshot bez rozšíření obnoví volbu i v emulátoru s rozšířením
    emu.call("snapshot_load", {"path": str(tmp / "noext.mzs")})
    emu.call("set_speed", {"mode": "max"})
    emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
    emu.call("set_register", {"reg": "PC", "value": h._LOOP})
    v = _plane3_roundtrip(emu)
    h.check(v == 0xFF, f"a snapshot saved without the VRAM extension restores it (plane III reads {v:02X}h)")


def _step_dmd0c(emu):
    """Krok 10 (A3, A4): DMD 0Ch až 0Fh podle C modelu netlistu (pravidlo sedí se záznamy U5 ze stroje)."""
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    emu.call("io_write", {"port": 0xE4, "value": 0})
    _run_code(emu, "dbe0", 1)                                  # IN A,(0E0h): CG-RAM na 0C000h
    # znak 1: vzor 41h na všech řádcích (bity 7..5 = 2 -> čtvrté čtení z 0C200h + 8 + r = znak 41h)
    emu.call("mem_write", {"addr": 0xC008, "data_hex": "41" * 8})
    emu.call("mem_write", {"addr": 0xC208, "data_hex": "25" * 8})   # F = 25h: popředí 2, pozadí 5
    emu.call("mem_write", {"addr": 0xD000, "data_hex": "01" * 1024})
    emu.call("mem_write", {"addr": 0xD800, "data_hex": "17" * 1024})
    line = _CANVAS_ROW + 10
    for dmd in (0x0C, 0x0F):
        emu.call("io_write", {"port": 0xCE, "value": dmd})
        emu.call("run", {"frames": 2})
        _w, px = _frame(emu)
        bad = []
        for k in (0, 5, 38, 39):
            c = [px(152 + 16 * k + i, line) for i in range(16)]
            fg1, bg1 = c[0], c[1]
            x = 0x17 if k < 39 else 0x25          # druhá polovina: atribut další buňky, u buňky 39 bajt F
            fg2 = c[8 + next(j for j in range(8) if (x >> j) & 1)]
            bg2 = c[8 + next(j for j in range(8) if not (x >> j) & 1)]
            for j in range(8):
                if c[j] != (fg1 if (0x41 >> j) & 1 else bg1):
                    bad.append((k, j))
                if c[8 + j] != (fg2 if (x >> j) & 1 else bg2):
                    bad.append((k, 8 + j))
            if len({fg1, bg1, fg2, bg2}) != 4:
                bad.append((k, "colors"))
        h.check(not bad, f"DMD {dmd:02X}h: cell = pattern in the attribute colors, then the next cell's attribute "
                         f"in the colors of the fourth read, canvas from column 152 (mismatches {bad[:6]})")
        h.check(px(151, line) == px(100, line) and px(792, line) == px(100, line),
                f"DMD {dmd:02X}h: border in columns 151 and 792 (640x200 position)")
    # přístup CPU v 0Ch: 0A00Ah a 0A00Bh jdou oba na text buňky 5 (offset 1005h)
    emu.call("io_write", {"port": 0xCE, "value": 0x0C})
    _run_code(emu, "3e33" "320ba0" "3a0aa0" "321132", 1)       # LD (0A00Bh),33h; LD A,(0A00Ah); LD (3211h),A
    v = _peek(emu, 0x3211)[0]
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    t = _peek(emu, 0xD005)[0]
    h.check(v == 0x33 and t == 0x33, f"DMD 0Ch: CPU address A goes to VRAM offset (A >> 1) & 1FFFh "
                                     f"(read back {v:02X}h, text cell 5 in DMD 08h {t:02X}h)")
    emu.call("io_write", {"port": 0xCE, "value": 0x00})


def _step_switch_320_640(emu):
    """Krok 12 (A8): body na rozhraní přepnutí 320 / 640 uprostřed řádku (U19, C model)."""
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    for value in (0x00, 0x11, 0x22, 0x3C):          # položky 0 až 3 = barvy 0, 1, 2, 0Ch
        emu.call("io_write", {"port": 0xF0, "value": value})
    line = 120
    # (rovina I, rovina II, směr, očekávané sloupce 148 až 153 + 16k jako položky palety;
    #  None = sloupec se nekontroluje)
    cases = (
        ("00", "ff", (0x04, 0x00), [1, 1, 3, 3, 3, 3], "640 -> 320, 00h / FFh: 4 px entry 3"),
        ("aa", "55", (0x04, 0x00), [1, 1, 0, 0, 3, 3], "640 -> 320, AAh / 55h: 2 px entry 3"),
        ("00", "ff", (0x00, 0x04), [None, 0, 0, 1, None, None], "320 -> 640, 00h / FFh: 2 px black, 1 px blue"),
    )
    for p1, p2, (before, after), want, what in cases:
        # LDIR čte zdroj přes RF: pro každou rovinu WF i RF na tutéž rovinu
        _run_code(emu, "dbe0" "3e01d3ccd3cd" "210080" "36" + p1 + "110180" "013f1f" "edb0"
                       "3e02d3ccd3cd" "210080" "36" + p2 + "110180" "013f1f" "edb0" "3e01d3cd" "dbe1", 20)
        emu.call("io_write", {"port": 0xCE, "value": before})
        emu.call("run", {"frames": 2})
        _w, px = _frame(emu)
        e = _out_at(emu, 0xCE, after, line, 400, 404)
        emu.call("run", {"frames": 1})
        _w, px = _frame(emu)
        # hrana registru DMD ve sloupci 148 + 16k převezme zápis s koncem M-cyklu nejvýš 14 sloupců
        # za ní; hranice bajtu režimu 320 je 6 sloupců za hranou, tedy nejvýš 8 před koncem M-cyklu
        x = (e - 8 - 154 + 15) // 16 * 16
        cols = [154 + x - 6 + i for i in range(6)]
        got = [px(c, line) for c in cols]
        ref = _palette_rgb(emu)
        exp = [ref[w] if w is not None else None for w in want]
        bad = [(c, g.hex(), w.hex()) for c, g, w in zip(cols, got, exp) if w is not None and g != w]
        h.check(not bad, f"{what} (M-cycle end {e}, switch at column {154 + x}): mismatches {bad}")
    emu.call("io_write", {"port": 0xCE, "value": 0x00})


def _palette_rgb(emu):
    """RGB položek palety 0 až 3: snímek 320x200, čtyři pruhy podle položek (roviny I a II)."""
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    # řádek VRAM 100: bajty 0 až 9 položka 0, 10 až 19 položka 1, 20 až 29 položka 2, 30 až 39 položka 3
    code = ("dbe0"
            "3e01d3cc" "21a08f" "060a" "3600" "23" "10fb" "060a" "36ff" "23" "10fb" "060a" "3600" "23" "10fb"
            "060a" "36ff" "23" "10fb"
            "3e02d3cc" "21a08f" "060a" "3600" "23" "10fb" "060a" "3600" "23" "10fb" "060a" "36ff" "23" "10fb"
            "060a" "36ff" "23" "10fb" "dbe1")
    _run_code(emu, code, 4)
    emu.call("run", {"frames": 2})
    _w, px = _frame(emu)
    return [px(_CANVAS_COL + 16 * (10 * e) + 8, _CANVAS_ROW + 100) for e in range(4)]


def _write_at(emu, line, e_min, e_max):
    """LD (HL),A (7 T, zápis je poslední M-cyklus) s koncem v e_min až e_max na řádku line; vrátí konec."""
    pos = h._goto(emu, line, max(0, e_min - 140))
    e0 = pos["col"] + 35
    delta = next(d for d in range(0, 400, 5)
                 if e_min <= e0 + d <= e_max and d not in (5, 10, 15, 25, 45))
    pad, n = _pad_hex(delta)
    jp = "c3%02x%02x" % (h._LOOP & 0xFF, h._LOOP >> 8)
    emu.call("mem_write", {"addr": _CODE, "data_hex": pad + "77" + jp})
    emu.call("set_register", {"reg": "PC", "value": _CODE})
    for _i in range(n + 1):
        emu.call("step_into")
    end = h._pos(emu)
    if end["line"] != line:
        raise h.TestFailure(f"write ended on line {end['line']}, expected {line}")
    return end["col"]


def _step_vram_write_fetch(emu):
    """Krok 13 (U8): zápis do VRAM se v řádku zápisu ukáže jen v buňce, kterou video fetch ještě nečetl."""
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    for value in (0x00, 0x1F, 0x20, 0x30):
        emu.call("io_write", {"port": 0xF0, "value": value})
    line = _CANVAS_ROW + 100
    # bajt buňky 20 řádku 100: 320x200 bajt 20 (40 bajtů na řádek), 640x200 bajt 40 (80 bajtů na řádek)
    for dmd, col, addr, what in ((0x00, 154 + 320, 0x8000 + 100 * 40 + 20, "320x200"),
                                 (0x04, 152 + 320, 0x8000 + 100 * 80 + 40, "640x200")):
        for e_min, shown in ((448, True), (452, True), (457, False), (461, False)):
            emu.call("io_write", {"port": 0xCE, "value": 0x00})   # výplň v adresování 320x200
            _run_code(emu, "dbe0" "3e03d3cc" "210080" "3600" "110180" "013f1f" "edb0" "3e01d3cc", 8)
            emu.call("io_write", {"port": 0xCE, "value": dmd})
            emu.call("run", {"frames": 2})
            emu.call("set_register", {"reg": "HL", "value": addr})
            emu.call("set_register", {"reg": "AF", "value": 0xFF00})
            e = _write_at(emu, line, e_min, e_min + 4)
            emu.call("run", {"frames": 1})
            _w, px = _frame(emu)
            got = px(col, line) != px(col - 16, line)
            h.check(got == shown and (e <= 456) == shown,
                    f"{what}: VRAM write ending in column {e} shows in the write line: {got}, expected {shown} "
                    f"(the cell is fetched about 33.5 px before its start, U8: T1 <= 626.5 px for the cell at 660 px)")
    _run_code(emu, "dbe1", 1)
    emu.call("io_write", {"port": 0xCE, "value": 0x00})


def _step_iorqlog_pulse(emu):
    """Krok 14: stopa iorqlog nese délku pulzu /IORQ včetně čekacích taktů."""
    emu.call("io_write", {"port": 0xE4, "value": 0})
    emu.call("io_write", {"port": 0xCE, "value": 0x08})
    emu.call("trace_start", {"channel": "iorqlog"})
    h._goto(emu, _CANVAS_ROW + 50, 300)
    # OUT (0F0h),A bez čekání; LD (0D000h),A ve viditelné části; OUT (C),A na 06CFh čeká do konce viditelné části
    emu.call("set_register", {"reg": "BC", "value": 0x06CF})
    emu.call("set_register", {"reg": "AF", "value": 0x0000})
    emu.call("set_register", {"reg": "HL", "value": 0xD000})
    _run_code(emu, "d3f0" "77" "ed79", 1)
    rows = _read_all(emu, "iorqlog", ["dir", "addr", "pulse"])
    emu.call("trace_stop", {"channel": "iorqlog"})
    outs = [(a[-2:].upper(), int(pl)) for d, a, pl in rows if d == "out"]
    f0 = [pl for a, pl in outs if a == "F0"]
    cf = [pl for a, pl in outs if a == "CF"]
    h.check(f0 and f0[0] == 12 and cf and cf[0] > 12,
            f"iorqlog pulse: OUT without wait 12 px, OUT to 0CFh after a VRAM write waits (pulses F0 {f0[:2]}, CF {cf[:2]})")
    emu.call("io_write", {"port": 0xCE, "value": 0x00})


def _step_snapshot_row_counter(emu, tmp=None):
    """Krok 15: snapshot s čítačem adresy řádku mimo rozsah snímku se načte se stavem podle DMD."""
    import zipfile
    tmp = Path(tmp or tempfile.mkdtemp(prefix="gdg_rowctr_"))
    _fill_ruler(emu)
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    for value in (0x0F, 0x10, 0x20, 0x30):          # položka 0 = barva F, ostatní 0 (černá)
        emu.call("io_write", {"port": 0xF0, "value": value})
    emu.call("run", {"frames": 1})
    s = 50
    h._goto(emu, _CANVAS_ROW + s, 300)
    good, bad = tmp / "rowctr-good.mzs", tmp / "rowctr-bad.mzs"
    emu.call("snapshot_save", {"path": str(good), "description": "row counter"})
    with zipfile.ZipFile(good) as zin, zipfile.ZipFile(bad, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "hw/gdg.xml":
                t = data.decode("utf-8")
                t = t.replace(f'<row_addr type="uint">{s}</row_addr>', '<row_addr type="uint">60000</row_addr>')
                t = t.replace(f'<row_addr_next type="uint">{s}</row_addr_next>',
                              '<row_addr_next type="uint">60000</row_addr_next>')
                h.check("60000" in t, "test snapshot: row counter element found and replaced")
                data = t.encode("utf-8")
            elif item.filename == "manifest.xml":
                # obsah se změnil; bez uloženého součtu ho čtečka nekontroluje
                t, n = re.subn(r"<checksum.*?</checksum>\s*", "", data.decode("utf-8"), flags=re.S)
                h.check(n == 1, "test snapshot: manifest holds one checksum")
                data = t.encode("utf-8")
            zout.writestr(item.filename, data)
    for path, what in ((good, "valid snapshot"), (bad, "snapshot with row counter 60000")):
        emu.call("snapshot_load", {"path": str(path)})
        emu.call("run", {"frames": 1})
        _w, px = _frame(emu)
        got = [_ruler_value(px, _CANVAS_ROW + r) for r in (s + 1, s + 2, 120, 199)]
        h.check(got == [s + 1, s + 2, 120, 199],
                f"{what} loaded in canvas row {s}: rows S+1, S+2, 120, 199 show VRAM rows {got}")


def run(exe):
    """Všechny kroky testu na jedné binárce. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="gdg_hw_fixes_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        h._setup(emu)
        _step_row_counter_700_to_320(emu)
        _step_row_counter_320_to_700(emu)
        _step_palette_window(emu)
        _step_palette_outside(emu)
        _step_open_bus(emu)
        _step_ctc0_phase(emu)
        _step_mode700_switch(emu)
        _step_geometry_640(emu)
        _step_border_end_cksw(emu, tmp)
        _step_scroll_sw(emu)
        _step_switch_320_640(emu)
        _step_vram_write_fetch(emu)
        _step_dmd0c(emu)
        _step_palette_after_dmd0c(emu)
        _step_iorqlog_pulse(emu)
        _step_snapshot_row_counter(emu, tmp)
        _step_exvram(emu, exe)
        emu.call("io_write", {"port": 0xCE, "value": 0x00})
        emu.call("io_write", {"port": 0xE0, "value": 0})
        emu.call("io_write", {"port": 0xE1, "value": 0})
        return True
    except h.TestFailure as e:
        print(f"  FAILED: {e}")
        return False
    finally:
        if emu is not None:
            emu.close()


def main():
    """Vstupní bod: najde binárku MZ-800 a spustí test."""
    emu_test_proc.install(deadline_s=280)
    exe = h._find_exe("mz800emu")
    if exe is None:
        print(_("SKIP: mz800emu binary not found"))
        return 77
    ok = run(exe)
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
