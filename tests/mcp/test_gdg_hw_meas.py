#!/usr/bin/env python3
"""E2E test: chování GDG podle měření analyzátorem na skutečném MZ-800 (MCP pipe).

Zdroj: emu-experiments gdg-meas (pět měření z 2026-11-21), v bázi
mz800-knowledge hw-measurement/vysledky.md, oddíly 12 až 16.

Kroky:
  1  čtení 0E008h v režimu MZ-700 nese jen HBLN (bit 7) a TEMPO (bit 0);
     bit CKSW tam není ani s nastaveným bitem, na portu 0CEh je v bitu 2;
  2  TEMPO se překlápí po 228 řádcích: za 228 snímků (71 136 řádků) právě
     312krát;
  3  snapshot verze 1564848f (bez klíče tempo_rows) běží dál s tehdejší
     periodou 229 řádků, nově uložený snapshot periodu 228 zachová;
  4  zákaz oblasti 0E000h (OUT 0E5h): čtení vrací poslední bajt na datové
     sběrnici (E0h, 7Eh, 1Ah, 00h podle instrukce), OUT 0E4h zákaz ruší,
     zápis při zákazu se neprovede.
  5  zápis barvy borderu (06CFh) se na obraze projeví 13 nebo 14 sloupců
     před koncem M-cyklu zápisu, vždy na sudém sloupci (na stroji 1,5 nebo
     2,5 px po sestupné hraně /IORQ, jen sudé px);
  6  zápis palety (0F0h) se na obraze projeví 11 nebo 12 sloupců před koncem
     M-cyklu zápisu, na hranici bodu režimu 320x200 (na stroji 3,5 nebo
     4,5 px po sestupné hraně /IORQ), i když zápis končí těsně za canvasem;
  7  přepnutí 320x200 -> 640x200 (0CEh) se projeví až na hranici 16 sloupců
     canvasu, nejvýš 8 sloupců před koncem M-cyklu (na stroji převezme registr
     DMD v 334 + 16k px zápis se sestupnou hranou /IORQ nejpozději 1,5 px před
     hranou; první měření -2,5 až 12,5 px, měření 8 U19); přechod na rozhraní smí
     začít až 6 sloupců před hranicí (U19, viz test_gdg_hw_fixes krok 12);
  8  nový offset scrollu (01CFh) platí od hranice 16 sloupců canvasu, nejvýš
     4 sloupce před koncem M-cyklu (na stroji hranice 11,5 až 26,5 px po
     sestupné hraně /IORQ);
  9  čtení Status registru (IN A,(0CEh)): bit HBLN je 1, když M-cyklus
     čtení končí ve sloupcích 154 až 793, bit HSYNC ve sloupcích 2 až 929
     (na stroji náběžná hrana /IORQ 337 až 976 px a 185 až 1112 px);
 10  režim MZ-700: OUT na porty 0CCh až 0CFh po zápisu do VRAM ve viditelné
     části řádku čeká do jejího konce (M-cyklus končí ve sloupcích 793 až
     797; na stroji /IORQ končí 976 až 980 px), OUT na 0F0h nečeká a po
     zápisu v zatemnění nečeká žádný;
 11  paleta po návratu z režimu MZ-700 (zápis DMD 00h): ve viditelné části
     řádku zůstane v lichých položkách barva popředí a v sudých barva pozadí
     atributu (25h -> A, A, D, D pro položky 3, 1, 2, 0), mimo ni barvy bajtu CG-RAM znaku 0 (výplň 0Fh -> 0, 0, F, F),
     a to i na řádku mimo canvas.

Jen MZ-800. Binárka se hledá v kořeni repa nebo v adresáři z proměnné
``MZ_EMU_DIR``. Proces běží izolovaně (dočasný profil); zabíjí se jen vlastní
PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import base64
import gettext
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
import test_gdg_cksw as h  # noqa: E402 - PipeEmu a příprava stroje

_ = gettext.gettext

#: Počet řádků snímku a perioda TEMPO v řádcích (mz800_gdg.h).
_ROWS = 312
_TEMPO_ROWS = 228
_TEMPO_ROWS_LEGACY = 229

#: Snapshot pořízený verzí 1564848f (bez klíče tempo_rows).
_LEGACY = Path(__file__).resolve().parent.parent / "data" / "replay" / "legacy-1564848f-row156.mzs"


def _gdg(emu):
    """Stav GDG (get_periph_gdg)."""
    return emu.call("get_periph_gdg")


def _peek(emu, addr):
    """Jeden bajt paměti tak, jak ho vidí debugger (mem_read)."""
    r = emu.call("mem_read", {"addr": addr, "len": 1})
    return base64.b64decode(r["data_b64"])[0]


def _frame_row(emu, line):
    """Řádek posledního dokončeného snímku jako seznam bodů RGB (get_frame_screenshot_raw)."""
    r = emu.call("get_frame_screenshot_raw")
    if not r.get("available"):
        raise h.TestFailure("framebuffer is not available")
    w, bpp = r["width"], r["bytes_per_pixel"]
    data = base64.b64decode(r["data_b64"])
    row = data[line * w * bpp:(line + 1) * w * bpp]
    return [row[x * bpp:x * bpp + 3] for x in range(w)]


def _write_change_col(emu, port, before, after, line, col, pad_hex, pad_ticks, first=0):
    """Zapíše na port instrukcí OUT (C),A a vrátí (sloupec konce M-cyklu, sloupec změny barvy).

    Před zápisem platí celý snímek hodnota ``before``, zapisuje se ``after``.
    ``pad_hex`` jsou instrukce před OUT (mění paritu sloupce), ``pad_ticks``
    jejich délka. OUT (C),A trvá 12 T = 60 tiků a jeho M-cyklus končí
    s instrukcí. Změna je první sloupec řádku ``line`` (od sloupce ``first``),
    který se liší od téhož řádku snímku se starou hodnotou.
    """
    emu.call("io_write", {"port": port, "value": before})
    emu.call("run", {"frames": 2})
    ref = _frame_row(emu, line)         # řádek celý se starou hodnotou
    pos = h._goto(emu, line, col)
    code = 0x3100
    jp = "c3%02x%02x" % (h._LOOP & 0xFF, h._LOOP >> 8)
    emu.call("mem_write", {"addr": code, "data_hex": pad_hex + "ed79" + jp})
    emu.call("set_register", {"reg": "BC", "value": port})
    emu.call("set_register", {"reg": "AF", "value": after << 8})
    emu.call("set_register", {"reg": "PC", "value": code})
    emu.call("run", {"frames": 1})      # obraz se čte z posledního dokončeného snímku
    px = _frame_row(emu, line)
    end = pos["col"] + pad_ticks + 60
    change = next((x for x in range(first, len(px)) if px[x] != ref[x]), None)
    return end, change


def _border_change_col(emu, line, col, pad_hex, pad_ticks):
    """Zápis barvy borderu 01h -> 0Fh: (sloupec konce M-cyklu, sloupec změny)."""
    return _write_change_col(emu, 0x06CF, 0x01, 0x0F, line, col, pad_hex, pad_ticks)


def _fill_vram(emu, body_hex):
    """Spustí program, který naplní VRAM (rovina I, WF = SINGLE), a vrátí PC do smyčky.

    ``body_hex`` je tělo programu mezi namapováním VRAM a jejím odpojením.
    """
    emu.call("mem_write", {"addr": 0x3100, "data_hex":
                           "dbe0" "3e01d3cc" + body_hex + "dbe1" "18fe"})
    emu.call("set_register", {"reg": "PC", "value": 0x3100})
    emu.call("run", {"frames": 12})
    emu.call("set_register", {"reg": "PC", "value": h._LOOP})


def _boundary_leads(emu, port, before, after, canvas, what, max_lead, restore=None, pre=0):
    """Zápisy na port v různých polohách; vrátí množinu (konec M-cyklu - hranice změny).

    Změna obrazu musí ležet vždy ve stejné vzdálenosti ``k`` za hranicí
    16 sloupců canvasu (podle obsahu VRAM se první odlišný bod nemusí krýt
    s hranicí) a hranice musí být první hranice 16 sloupců, která leží
    nejvýš ``max_lead`` sloupců před koncem M-cyklu. ``pre`` sloupců přechodu
    smí ležet ještě před hranicí (přepnutí 320 -> 640: podle měření U19
    a C modelu netlistu začíná přechod 3 sloupce před hranicí bajtu 320).
    """
    cases = []
    for pad_hex, pad_ticks in (("", 0), ("3e%02x" % after, 35)):
        for col in range(380, 460, 9):
            end, change = _write_change_col(emu, port, before, after, 100, col, pad_hex, pad_ticks, first=canvas)
            if restore:
                restore()
            h.check(change is not None, f"{what}: write ending at column {end} changes its line")
            cases.append((end, change))
    ks = {(c - canvas) % 16 for _, c in cases}
    h.check(len(ks) == 1, f"{what}: the change always lies at the same offset in a 16-column cell (offsets {sorted(ks)})")
    k = ks.pop()
    leads = set()
    for end, change in cases:
        bound = change - k if k < 16 - pre else change - k + 16
        want = canvas + -(-(end - max_lead - canvas) // 16) * 16
        h.check(bound == want, f"{what}: takes effect at the first 16-column boundary at most {max_lead} columns "
                               f"before the M-cycle end (end {end}, boundary {bound}, expected {want})")
        leads.add(end - bound)
    return leads


#: Výplně před čtením Status registru: (kód, tiky, počet instrukcí). NOP 20,
#: INC HL 30 a LD A,n 35 tiků a jejich součty dají všechny zbytky sloupce.
_STATUS_PADS = (("", 0, 0), ("00", 20, 1), ("23", 30, 1), ("3e00", 35, 1), ("003e00", 55, 2), ("233e00", 65, 2))


def _status_reads(emu, around, lines):
    """Čte Status registr instrukcí IN A,(0CEh) kolem sloupce ``around``.

    Vrací slovník {sloupec konce M-cyklu čtení: přečtený bajt}. Instrukce
    má 11 T = 55 tiků a její M-cyklus čtení končí s instrukcí. Fáze procesoru
    vůči řádku se mění s řádkem, výplně (_STATUS_PADS) doplní zbylé sloupce.
    """
    code = 0x3100
    jp = "c3%02x%02x" % (h._LOOP & 0xFF, h._LOOP >> 8)
    out = {}
    for line in lines:
        for pad_hex, pad_ticks, pad_steps in _STATUS_PADS:
            start = (around - 55 - pad_ticks - 25) % 1136
            row = line if start <= around else line - 1
            h._goto(emu, row, start)
            emu.call("mem_write", {"addr": code, "data_hex": pad_hex + "dbce" + jp})
            emu.call("set_register", {"reg": "PC", "value": code})
            for _ in range(pad_steps + 1):
                emu.call("step_into")
            pos = h._pos(emu)
            value = emu.call("get_registers")["AF"] >> 8
            if pos["col"] in out and out[pos["col"]] & 0xA0 != value & 0xA0:
                raise h.TestFailure(f"status read ending at column {pos['col']} gives two different values")
            out[pos["col"]] = value
    return out


def _out_after_vram_write(emu, port, line, col):
    """Režim MZ-700: LD (HL),A na 0D000h a hned OUT (C),A na ``port``.

    Vrací (sloupec konce OUT bez čekání, skutečný řádek a sloupec konce OUT).
    LD (HL),A má 7 T, OUT (C),A 12 T: bez čekání 95 tiků od začátku.
    """
    code = 0x3100
    jp = "c3%02x%02x" % (h._LOOP & 0xFF, h._LOOP >> 8)
    pos = h._goto(emu, line, col)
    emu.call("mem_write", {"addr": code, "data_hex": "77" "ed79" + jp})
    emu.call("set_register", {"reg": "HL", "value": 0xD000})
    emu.call("set_register", {"reg": "BC", "value": port})
    emu.call("set_register", {"reg": "AF", "value": 0x2000 if port != 0x00CE else 0x0800})
    emu.call("set_register", {"reg": "PC", "value": code})
    emu.call("step_into")
    emu.call("step_into")
    end = h._pos(emu)
    return pos["col"] + 95, end["line"], end["col"]


def _palette_after_mode700(emu, attr, line, col, cg0=None):
    """Paleta 0, 1, 2, 4, režim MZ-700 s atributem ``attr`` všude, návrat OUT (C),A na DMD 00h.

    ``cg0`` (8 bajtů hex) je vzor znaku 0 v CG-RAM (0C000h až 0C007h); bez něj zůstane, co tam je.
    Vrací (sloupec konce M-cyklu zápisu DMD, [položka 3, 1, 2, 0]).
    """
    emu.call("io_write", {"port": 0x00CE, "value": 0x00})
    for value in (0x00, 0x11, 0x22, 0x34):
        emu.call("io_write", {"port": 0x00F0, "value": value})
    emu.call("io_write", {"port": 0x00CE, "value": 0x08})
    emu.call("mem_write", {"addr": 0xD800, "data_hex": ("%02x" % attr) * 1000})
    if cg0 is not None:
        emu.call("mem_write", {"addr": 0xC000, "data_hex": cg0})
    pos = h._goto(emu, line, col)
    code = 0x3100
    jp = "c3%02x%02x" % (h._LOOP & 0xFF, h._LOOP >> 8)
    emu.call("mem_write", {"addr": code, "data_hex": "ed79" + jp})
    emu.call("set_register", {"reg": "BC", "value": 0x00CE})
    emu.call("set_register", {"reg": "AF", "value": 0x0000})
    emu.call("set_register", {"reg": "PC", "value": code})
    emu.call("step_into")
    pal = _gdg(emu)["palette"]
    return pos["col"] + 60, [pal[3], pal[1], pal[2], pal[0]]


def _tempo_count(emu, frames):
    """Počet překlopení TEMPO za daný počet celých snímků (čítač g_gdg.tempo).

    Nejdřív doběhne na hranici snímku: po načtení snapshotu stojí stroj
    uprostřed snímku a první ``run`` by byl kratší.
    """
    emu.call("run", {"frames": 1})
    a = _gdg(emu)["tempo"]
    done = 0
    while done < frames:
        n = min(200, frames - done)
        emu.call("run", {"frames": n})
        done += n
    return _gdg(emu)["tempo"] - a


def run(exe):
    """Všechny kroky testu na jedné binárce. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="gdg_hw_meas_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        h._setup(emu)

        # 1 - 0E008h bez CKSW
        emu.call("io_write", {"port": 0xE4, "value": 0})      # mapování po resetu
        emu.call("io_write", {"port": 0xCE, "value": 0x08})   # režim MZ-700
        h._cksw(emu, True)
        code = 0x3100
        # LD A,(0E008h); LD (3200h),A; JR $
        emu.call("mem_write", {"addr": code, "data_hex": "3a08e0" + "320032" + "18fe"})
        emu.call("set_register", {"reg": "PC", "value": code})
        emu.call("run", {"frames": 1})
        v = _peek(emu, 0x3200)
        h.check((v & 0x7E) == 0, f"E008h read by the program carries only HBLN and TEMPO with CKSW = 1 (got {v:02X}h)")
        v = _peek(emu, 0xE008)
        h.check((v & 0x7E) == 0, f"E008h read by the debugger carries only HBLN and TEMPO with CKSW = 1 (got {v:02X}h)")
        v = emu.call("io_read", {"port": 0xCE})["value"]
        h.check((v & 0x04) != 0, f"port CEh has CKSW in bit 2 (got {v:02X}h)")
        h._cksw(emu, False)
        emu.call("io_write", {"port": 0xCE, "value": 0x00})
        emu.call("io_write", {"port": 0xE0, "value": 0})
        emu.call("io_write", {"port": 0xE1, "value": 0})
        emu.call("set_register", {"reg": "PC", "value": h._LOOP})

        # 2 - TEMPO po 228 řádcích
        n = _tempo_count(emu, _TEMPO_ROWS)
        h.check(n == _ROWS, f"TEMPO toggles {_ROWS} times in {_TEMPO_ROWS} frames (got {n})")

        # 3 - snapshot starší verze drží periodu 229 řádků
        snap = tmp / "new.mzs"
        emu.call("snapshot_save", {"path": str(snap)})
        with zipfile.ZipFile(snap) as z:
            h.check("<tempo_rows " in z.read("hw/gdg.xml").decode("utf-8"), "snapshot stores the tempo_rows key")
        emu.call("snapshot_load", {"path": str(_LEGACY)})
        emu.call("set_speed", {"mode": "max"})
        n = _tempo_count(emu, _TEMPO_ROWS_LEGACY)
        h.check(n == _ROWS, f"snapshot made by 1564848f keeps {_TEMPO_ROWS_LEGACY} rows: {_ROWS} toggles in {_TEMPO_ROWS_LEGACY} frames (got {n})")
        emu.call("snapshot_load", {"path": str(snap)})
        n = _tempo_count(emu, _TEMPO_ROWS)
        h.check(n == _ROWS, f"new snapshot keeps {_TEMPO_ROWS} rows (got {n} toggles)")

        # 4 - zákaz oblasti 0E000h
        emu.call("io_write", {"port": 0xE4, "value": 0})
        emu.call("io_write", {"port": 0xCE, "value": 0x00})
        rom = _peek(emu, 0xE010)
        prog = (
            "d3e5"                      # OUT (0E5h),A
            "3a10e0" "320032"           # LD A,(0E010h); LD (3200h),A
            "2110e0" "7e" "320132"      # LD HL,0E010h; LD A,(HL); LD (3201h),A
            "1110e0" "1a" "320232"      # LD DE,0E010h; LD A,(DE); LD (3202h),A
            "dd2110e0" "dd7e00" "320332"  # LD IX,0E010h; LD A,(IX+0); LD (3203h),A
            "d3e4"                      # OUT (0E4h),A
            "3a10e0" "320432"           # LD A,(0E010h); LD (3204h),A
            "d3e1"                      # OUT (0E1h),A - DRAM na 0E000h
            "3e55" "3210e0"             # LD A,55h; LD (0E010h),A
            "d3e5"                      # OUT (0E5h),A
            "3eaa" "3210e0"             # LD A,0AAh; LD (0E010h),A
            "d3e6" "d3e1"               # OUT (0E6h),A; OUT (0E1h),A
            "3a10e0" "320532"           # LD A,(0E010h); LD (3205h),A
            "18fe"                      # JR $
        )
        emu.call("mem_write", {"addr": code, "data_hex": prog})
        emu.call("set_register", {"reg": "PC", "value": code})
        emu.call("run", {"frames": 1})
        got = [_peek(emu, 0x3200 + i) for i in range(6)]
        want = [0xE0, 0x7E, 0x1A, 0x00, rom, 0x55]
        names = ["LD A,(nn) reads the high address byte", "LD A,(HL) reads its opcode",
                 "LD A,(DE) reads its opcode", "LD A,(IX+0) reads the displacement",
                 "OUT E4h cancels the prohibition (ROM byte)", "write while prohibited is dropped"]
        for g, w, name in zip(got, want, names):
            h.check(g == w, f"prohibited area: {name} (want {w:02X}h, got {g:02X}h)")

        # 5 - místo změny barvy borderu
        emu.call("io_write", {"port": 0xE0, "value": 0})
        emu.call("io_write", {"port": 0xE1, "value": 0})
        emu.call("set_register", {"reg": "PC", "value": h._LOOP})
        seen = set()
        for pad_hex, pad_ticks in (("", 0), ("3e0f", 35)):      # LD A,0Fh = 7 T mění paritu
            for col in (300, 301, 500):
                end, change = _border_change_col(emu, 10, col, pad_hex, pad_ticks)
                h.check(change is not None, f"border write ending at column {end} changes the colour in its line")
                lead = end - change
                seen.add(lead)
                h.check(lead in (13, 14) and change % 2 == 0,
                        f"border changes 13 or 14 columns before the M-cycle end, on an even column "
                        f"(end {end}, change {change})")
        h.check(seen == {13, 14}, f"both leads occur (got {sorted(seen)})")
        # zápis končící těsně za viditelnou částí řádku (řádek je už uzavřený)
        tail = None
        for col in range(850, 900, 3):
            end, change = _border_change_col(emu, 10, col, "", 0)
            if 929 <= end <= 940:
                tail = (end, change)
                break
        h.check(tail is not None, "a border write ending just after the visible part was found")
        h.check(tail[1] is not None and tail[0] - tail[1] in (13, 14) and tail[1] % 2 == 0,
                f"border write ending after the visible part still recolours its tail (end {tail[0]}, change {tail[1]})")

        # 6 - místo změny palety (položka 0; mění se body s kódem 0)
        canvas = 154      # první sloupec canvasu (VIDEO_BEAM_CANVAS_FIRST_COLUMN)
        # canvas vyplnit kódem 0: program maže roviny I a II (WF = SINGLE, 03h)
        emu.call("mem_write", {"addr": 0x3100, "data_hex":
                               "dbe0"          # IN A,(0E0h) - VRAM na 8000h
                               "3e03d3cc"      # LD A,03h; OUT (0CCh),A
                               "210080" "11018001" "3f1f" "3600" "edb0"   # LD HL,8000h; LD DE,8001h; LD BC,1F3Fh; LD (HL),0; LDIR
                               "dbe1"          # IN A,(0E1h)
                               "18fe"})
        emu.call("set_register", {"reg": "PC", "value": 0x3100})
        emu.call("run", {"frames": 12})
        emu.call("set_register", {"reg": "PC", "value": h._LOOP})
        seen = set()
        for pad_hex, pad_ticks in (("", 0), ("3e0f", 35)):
            for col in (400, 401, 600):
                end, change = _write_change_col(emu, 0x00F0, 0x01, 0x0F, 100, col, pad_hex, pad_ticks, first=canvas)
                lead = None if change is None else end - change
                seen.add(lead)
                h.check(lead in (11, 12) and (change - canvas) % 2 == 0,
                        f"palette changes 11 or 12 columns before the M-cycle end, on a pixel boundary "
                        f"(end {end}, change {change})")
        h.check(seen == {11, 12}, f"both palette leads occur (got {sorted(seen)})")
        tail = None
        for col in range(715, 760, 3):
            end, change = _write_change_col(emu, 0x00F0, 0x01, 0x0F, 100, col, "", 0, first=canvas)
            if 795 <= end <= 803:
                tail = (end, change)
                break
        h.check(tail is not None, "a palette write ending just after the canvas was found")
        h.check(tail[1] is not None and tail[0] - tail[1] in (11, 12) and tail[1] < canvas + 640,
                f"palette write ending after the canvas still recolours the end of its line "
                f"(end {tail[0]}, change {tail[1]})")

        # 7 - přepnutí 320x200 -> 640x200 na hranici 16 sloupců (VRAM: bajty 0Fh)
        _fill_vram(emu, "210080" "11018001" "3f1f" "360f" "edb0")
        emu.call("io_write", {"port": 0x00F0, "value": 0x00})
        emu.call("io_write", {"port": 0x00F0, "value": 0x1F})
        leads = _boundary_leads(emu, 0x00CE, 0x00, 0x04, canvas, "resolution switch", 8, pre=6)
        h.check(len(leads) >= 6 and min(leads) >= -7 and max(leads) <= 8,
                f"resolution switch: boundary lies 8 columns before to 7 after the M-cycle end (leads {sorted(leads)})")
        emu.call("io_write", {"port": 0x00CE, "value": 0x00})

        # 8 - offset scrollu od hranice 16 sloupců (VRAM: liché řádky FFh, sudé 00h; posun o řádek)
        _fill_vram(emu, "210080" "0ec8" "af" "0628" "772310fc" "2f" "0d" "20f6")
        for port, value in ((0x03CF, 0x7D), (0x04CF, 0x00), (0x05CF, 0x7D), (0x02CF, 0x00)):
            emu.call("io_write", {"port": port, "value": value})
        leads = _boundary_leads(emu, 0x01CF, 0x00, 0x05, canvas, "scroll offset", 4)
        h.check(len(leads) >= 6 and min(leads) >= -11 and max(leads) <= 4,
                f"scroll offset: boundary lies 4 columns before to 11 after the M-cycle end (leads {sorted(leads)})")
        emu.call("io_write", {"port": 0x01CF, "value": 0x00})

        # 9 - okamžik vzorkování Status registru
        emu.call("io_write", {"port": 0x00CE, "value": 0x00})
        lines = range(100, 150)
        for bit, name, first, last in ((7, "HBLN", 154, 793), (5, "HSYNC", 2, 929)):
            for edge in (first, last + 1):
                reads = _status_reads(emu, edge, lines)
                near = {c: v >> bit & 1 for c, v in reads.items() if abs(c - edge) <= 6}
                h.check(edge - 1 in near and edge in near,
                        f"status {name}: reads ending in columns {edge - 1} and {edge} were taken ({sorted(near)})")
                bad = {c: v for c, v in near.items() if v != (1 if first <= c <= last else 0)}
                h.check(not bad, f"status {name} is 1 for reads ending in columns {first} to {last} "
                                 f"(checked {sorted(near)}, wrong {bad})")

        # 10 - OUT na porty GDG po zápisu do VRAM v režimu MZ-700
        emu.call("io_write", {"port": 0xE4, "value": 0})
        emu.call("io_write", {"port": 0x00CE, "value": 0x08})
        for port in (0x06CF, 0x00CC, 0x00CD, 0x00CE):
            free, line, col = _out_after_vram_write(emu, port, 100, 300)
            h.check(line == 100 and 793 <= col <= 797 and (col - free) % 5 == 0,
                    f"OUT to {port:04X}h after a VRAM write in the visible part waits to its end "
                    f"(would end at column {free}, ended at {line}/{col})")
        free, line, col = _out_after_vram_write(emu, 0x00F0, 100, 300)
        h.check(line == 100 and col == free, f"OUT to 00F0h after a VRAM write does not wait (ended at {line}/{col}, expected {free})")
        free, line, col = _out_after_vram_write(emu, 0x06CF, 100, 830)
        h.check(line == 100 and col == free, f"OUT to 06CFh after a VRAM write in the blanking does not wait (ended at {line}/{col}, expected {free})")
        free, line, col = _out_after_vram_write(emu, 0x06CF, 100, 20)
        h.check(line == 100 and col == free, f"OUT to 06CFh after a VRAM write served before the visible part does not wait (ended at {line}/{col}, expected {free})")
        emu.call("io_write", {"port": 0x00CE, "value": 0x00})

        # 11 - paleta po návratu z režimu MZ-700
        emu.call("io_write", {"port": 0xE4, "value": 0})
        for attr, want in ((0x25, [0xA, 0xA, 0xD, 0xD]), (0x17, [0x9, 0x9, 0xF, 0xF]), (0x70, [0xF, 0xF, 0x0, 0x0])):
            end, pal = _palette_after_mode700(emu, attr, 100, 400)
            h.check(pal == want, f"palette after mode 700, attribute {attr:02X}h, return in the visible part "
                                 f"(M-cycle end {end}): entries 3, 1, 2, 0 = {pal}, expected {want}")
        # mimo okno: bajt CG-RAM znaku 0, řádek vzoru posledního řádku rastru, jehož okno proběhlo
        # (canvas začíná řádkem 46; řádek 102 = řádek vzoru 0, před oknem tedy vzor 7 řádku 101)
        for line, col, where in ((100, 40, "left border"), (100, 880, "line blanking"), (100, 1000, "line blanking")):
            end, pal = _palette_after_mode700(emu, 0x25, line, col, "0f" * 8)
            h.check(pal == [0x0, 0x0, 0xF, 0xF], f"palette after mode 700, CG-RAM of character 0 = 0Fh, return in the {where} "
                                                f"(M-cycle end {end}): entries 3, 1, 2, 0 = {pal}, expected 0, 0, F, F")
        cg0 = "0112233445566770"
        for line, col, where, want in ((102, 40, "left border (pattern row 7 of the previous line)", [0xF, 0xF, 0x0, 0x0]),
                                       (103, 40, "left border (pattern row 0 of the previous line)", [0x0, 0x0, 0x9, 0x9]),
                                       (102, 880, "line blanking (pattern row 0 of this line)", [0x0, 0x0, 0x9, 0x9]),
                                       (105, 1000, "line blanking (pattern row 3 of this line)", [0xB, 0xB, 0xC, 0xC])):
            end, pal = _palette_after_mode700(emu, 0x25, line, col, cg0)
            h.check(pal == want, f"palette after mode 700, return in the {where} "
                                 f"(M-cycle end {end}): entries 3, 1, 2, 0 = {pal}, expected {want}")
        end, pal = _palette_after_mode700(emu, 0x25, 100, 40, "00" * 8)
        h.check(pal == [0x0, 0x0, 0x0, 0x0], f"palette after mode 700, CG-RAM of character 0 = 00h, return in the left border "
                                            f"(M-cycle end {end}): entries 3, 1, 2, 0 = {pal}, expected 0, 0, 0, 0")
        end, pal = _palette_after_mode700(emu, 0x25, 270, 400)
        h.check(pal == [0xA, 0xA, 0xD, 0xD], f"palette after mode 700, return below the canvas in the visible "
                                            f"width (M-cycle end {end}): entries 3, 1, 2, 0 = {pal}")
        emu.call("io_write", {"port": 0x00CE, "value": 0x00})
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
