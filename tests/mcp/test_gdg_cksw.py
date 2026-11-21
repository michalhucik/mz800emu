#!/usr/bin/env python3
"""E2E test: bit CKSW (Superimpose, port 0CF07h bit 7) zkracuje řádek (MCP pipe).

Podle simulace netlistu GDG (mz800-knowledge hw/11-palette-attributes.md,
"CKSW - Superimpose bit") má řádek při CKSW = 1 délku 1120 pxCLK místo 1136,
snímek má dál 312 řádků a čip si bit načítá jednou za řádek na sestupné
hraně HBLN (v emulátoru sloupec 790). Měření na skutečném MZ-800
(2026-11-21) délku řádku, počet řádků i hranici načtení potvrdilo.

Test běží jen na MZ-800 (jiné platformy bit nemají):

  1  bez CKSW: snímek 354432 pxCLK, řádek 1136, bit 2 Status registru 0;
  2  zápis 80h na 07CFh před sloupcem 790: už tento řádek má 1120 pxCLK;
  3  zápis za sloupcem 790: tento řádek 1136, další 1120;
  3a hranice po sloupcích: instrukce OUT, jejíž I/O cyklus končí do sloupce
     791, změní už tento řádek, od sloupce 792 až další (klopný obvod se
     podle netlistu překlápí 2 pxCLK před koncem M-cyklu), oběma směry,
     v obrazových řádcích i pod nimi; stavový bit HSYNC ve sloupcích 914
     až 927 odpovídá výsledné délce řádku, i když ho ve volném běhu bez
     zastavení čte sám program (přeplánování čekající události);
  4  CKSW = 1 trvale: snímek 349440 pxCLK, sloupec nikdy >= 1120, bit 2 = 1,
     absolutní čas je souvislý přes konec snímku;
  5  snapshot uprostřed krátkého řádku: stav po načtení a doběhu 2 snímků je
     stejný jako po přímém běhu bez načtení i po načtení přes "špinavý"
     stroj, včetně otisku periferií (PSG dokrokovává 20ms událost, jejíž
     fázi zkrácené snímky posouvají a snapshot ji musí obnovit); načtení
     obnoví i zkrácení běžícího řádku;
  5a po vypnutí bitu má už další řádek plnou délku (pole line_ticks);
  5b čítač 0 obvodu 8253 ubývá podle času i přes konce krátkých snímků
     a jeho výstup se dál překlápí (událost čekající přes konec snímku);
  5c zpožděné povolení přerušení PIO-Z80, které čeká přes konec krátkého
     snímku, se uplatní stejně jako uprostřed snímku;
  5d snapshot bez klíče cksw (starší verze) načte bit vynulovaný, i když
     ho běžící stroj měl nastavený;
  5e hwlog zaznamená každý zápis bitu (gdg_video, sub 9, zapsaný bajt)
     a breakpoint na událost cksw_change zastaví hned za instrukcí OUT;
  6  zápis 00h: snímek zase 354432 pxCLK;
  7  záznam a přehrání vstupů s CKSW = 1 a otiskem po každém snímku se
     nerozejde, a cíl goto zadaný časem i dvojicí snímek + takty ve snímku
     skončí ve stejném bodě (i bez CKSW);
  8  goto zadané snímkem odmítne cíl před startem záznamu a číslo snímku
     mimo 32 bitů a cíl s taktem za koncem krátkého snímku ohlásí jako
     nedosažený;
  9  bez CKSW při rychlosti 100 % i 150 %: běh po načtení snapshotu má
     stejný otisk periferií jako nepřerušený běh (termín 20ms události se
     při 150 % nekryje s hranicí snímku a snapshot ho musí nést).

Délky se měří z absolutního času a pozice: začátek řádku = čas - sloupec,
začátek snímku = čas - tiky ve snímku. Binárka se hledá v kořeni repa nebo
v adresáři z proměnné ``MZ_EMU_DIR``. Proces běží izolovaně (dočasný
``--cfg-dir``/``--work-dir``, prázdné INI, ``--no-save-ini``); zabíjí se jen
vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import base64
import json
import os
import queue
import subprocess
import re
import sys
import tempfile
import threading
import zipfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent

#: Délka řádku a snímku bez CKSW a s CKSW (mz800_video.h, mz800_gdg.h).
_LINE = 1136
_LINE_SHORT = 1120
_LINES = 312
_FRAME = _LINE * _LINES
_FRAME_SHORT = _LINE_SHORT * _LINES

#: Sloupec načtení bitu CKSW (VIDEO_BEAM_HBLN_FIRST_COLUMN).
_LATCH_COL = 790

#: O kolik tiků před koncem M-cyklu zápisu se překlápí klopný obvod CKSW
#: (netlist, sonda probe_cksw_wr.c): zápis končící do tolika tiků po načtení
#: bitu ještě platí pro tento řádek.
_WR_EDGE = 2

#: Port CKSW a Status registr GDG.
_PORT_CKSW = 0x07CF
_PORT_STATUS = 0xCE

#: Program: 3000h JP 3000h; od 3100h NOP (krok po 4 T = 20 pxCLK).
_LOOP = 0x3000
_NOPS = 0x3100


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def _find_exe(name):
    """Najde binárku (MZ_EMU_DIR, jinak kořen repa; .exe i bez), jinak None."""
    base = Path(os.environ["MZ_EMU_DIR"]) if os.environ.get("MZ_EMU_DIR") else _REPO_ROOT
    for c in (base / (name + ".exe"), base / name):
        if c.is_file():
            return c
    return None


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp):
        ini = tmp / "emu.ini"
        ini.write_text("", encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        self.dead = False
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise TestFailure("no hello from emulator")

    def _pump(self):
        for line in iter(self.proc.stdout.readline, ""):
            self.q.put(line.strip())
        self.q.put(None)

    def _read(self, timeout, pred):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                return None
            if line is None:
                return None
            if not line.startswith("{"):
                continue
            msg = json.loads(line)
            if pred(msg):
                return msg
        return None

    def call(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí data úspěšné response (jinak FAIL)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            self.dead = True
            raise TestFailure(f"no response to {cmd} within {timeout:.0f} s "
                              f"(emulator exit code: {self.proc.poll()})")
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def close(self):
        """Ukončí proces přes shutdown, jinak zabije vlastní PID."""
        if not self.dead:
            try:
                self.call("shutdown", timeout=10.0)
            except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
                pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()


def _pos(emu):
    """Pozice a čas: řádek, sloupec, číslo snímku, tiky ve snímku, absolutní čas.

    Odvozené hodnoty: ``row_start`` = absolutní čas sloupce 0 aktuálního
    řádku, ``frame_start`` = absolutní čas začátku snímku.
    """
    r = emu.call("get_raster_pos")
    t = emu.call("state_observe", {"windows": []})["time"]
    if r["frame_number"] != t["frame"]:
        raise TestFailure("raster and time read in different frames")
    return {
        "line": r["scanline"], "col": r["column_pixel"], "frame": t["frame"],
        "frame_ticks": t["frame_ticks"], "ticks": t["ticks"],
        "row_start": t["ticks"] - r["column_pixel"],
        "frame_start": t["ticks"] - t["frame_ticks"],
    }


def _goto(emu, line, col, pc=_LOOP):
    """Doběhne na řádek a sloupec (první hranice instrukce na pozici nebo za ní).

    run_until_raster bere pozici v aktuálním snímku, která už je za paprskem,
    jako dosaženou; v tom případě se nejdřív doběhne do dalšího snímku.
    """
    emu.call("set_register", {"reg": "PC", "value": pc})
    p = _pos(emu)
    if (p["line"], p["col"]) >= (line, col):
        emu.call("run", {"frames": 1})
    emu.call("run_until_raster", {"line": line, "col": col, "max_cycles": 400000})
    p = _pos(emu)
    if p["line"] != line or p["col"] < col:
        raise TestFailure(f"raster {line}/{col} not reached ({p['line']}/{p['col']})")
    return p


def _row_len(emu, line, col):
    """Délka řádku ``line``: rozdíl začátků řádku line a line + 1."""
    a = _goto(emu, line, col)
    b = _goto(emu, line + 1, 0)
    return b["row_start"] - a["row_start"]


def _frame_len(emu):
    """Délka snímku: rozdíl začátků dvou po sobě jdoucích snímků."""
    a = _goto(emu, 10, 100)
    _goto(emu, 11, 0)
    b = _goto(emu, 10, 100)
    if b["frame"] != a["frame"] + 1:
        raise TestFailure("two raster stops are not in consecutive frames")
    return b["frame_start"] - a["frame_start"]


def _status(emu):
    """Bit 2 Status registru GDG (CKSW)."""
    return (emu.call("io_read", {"port": _PORT_STATUS})["value"] >> 2) & 1


def _cksw(emu, on):
    """Zápis na port CKSW v aktuální pozici paprsku."""
    emu.call("io_write", {"port": _PORT_CKSW, "value": 0x80 if on else 0x00})


def _ctc0_drift(emu, divider):
    """Rozdíl mezi úbytkem čítače 0 obvodu 8253 a uplynulým časem přes tři snímky.

    Čítač 0 má na MZ-800 hodiny pxCLK / 16 nezávislé na délce řádku, v režimu 2
    tedy ubývá o 1 za 16 tiků. Vrací (úbytek - čas / 16) modulo dělič,
    přepočtený do rozsahu kolem nuly.
    """
    def read():
        emu.call("io_write", {"port": 0xD7, "value": 0x00})
        lo = emu.call("io_read", {"port": 0xD4})["value"]
        hi = emu.call("io_read", {"port": 0xD4})["value"]
        return lo | (hi << 8), _pos(emu)["ticks"]

    _goto(emu, 20, 100)
    v1, t1 = read()
    _goto(emu, 300, 100)
    emu.call("run", {"frames": 3})
    v2, t2 = read()
    d = ((v1 - v2) - (t2 - t1) // 16) % divider
    return d - divider if d > divider // 2 else d


def _ctc0_out_toggles(emu, lines):
    """Počet změn výstupu čítače 0 při vzorkování po řádcích."""
    last = None
    count = 0
    start = _goto(emu, 20, 100)["line"]
    for line in range(start + 1, start + 1 + lines):
        _goto(emu, line, 100)
        out = emu.call("get_periph_i8253")["channels"][0]["out"]
        if last is not None and out != last:
            count += 1
        last = out
    return count


#: Instrukce pro posun začátku OUT po 5 ticích: NOP, RET Z (nesplněno),
#: INC HL, LD A,83h a jejich délky v ticích.
_PADS = (("", 0), ("00", 20), ("c8", 25), ("23", 30), ("3e83", 35), ("0000", 40), ("00c8", 45),
         ("0023", 50), ("003e83", 55), ("2323", 60), ("233e83", 65), ("3e833e83", 70), ("00003e83", 75))


def _pio_enable_over_frame_end(emu, frame_end_col):
    """Zpožděné povolení přerušení PIO-Z80, které čeká přes konec snímku.

    Hledá začátek instrukce OUT (C),A v posledním řádku tak, aby instrukce
    (60 tiků) skončila před koncem snímku a událost povolení (75 tiků od
    začátku) ležela až za ním. Vrací (začátek vůči konci snímku, počet kroků
    od OUT do povolení), nebo (None, None), když se okno netrefilo.
    """
    code = 0x2F00
    for pad_hex, _ticks in _PADS:
        emu.call("io_write", {"port": 0xFC, "value": 0x03})
        emu.call("mem_write", {"addr": code, "data_hex": pad_hex + "ed79" + "00" * 32})
        _goto(emu, _LINES - 1, frame_end_col - 150, pc=_NOPS)
        emu.call("set_register", {"reg": "BC", "value": 0x00FC})
        emu.call("set_register", {"reg": "AF", "value": 0x8300})
        emu.call("set_register", {"reg": "PC", "value": code})
        n = 0
        while emu.call("get_registers")["PC"] != code + len(pad_hex) // 2 and n < 8:
            emu.call("step_into")
            n += 1
        start = _pos(emu)
        rel = start["col"] - frame_end_col
        if start["line"] != _LINES - 1 or not (-75 < rel <= -60):
            continue
        for step in range(1, 9):
            emu.call("step_into")
            if step == 1 and _pos(emu)["line"] != _LINES - 1:
                raise TestFailure("OUT did not end before the frame end")
            if emu.call("get_periph_z80_pio")["port_a"]["int_enable"]:
                return rel, step
        return rel, None
    return None, None


def _pio_enable_steps(emu, line, col):
    """Po kolika instrukcích od OUT na řídicí port PIO-Z80 se povolí přerušení portu A.

    Povolení se uplatní zpožděnou událostí. Instrukce OUT (C),A začíná na
    dané pozici rastru; vrací (pozice začátku, počet kroků do povolení).
    """
    code = 0x2F00
    emu.call("mem_write", {"addr": code, "data_hex": "ed79" + "00" * 32})
    emu.call("io_write", {"port": 0xFC, "value": 0x03})
    p = _goto(emu, line, col, pc=_NOPS)
    emu.call("set_register", {"reg": "BC", "value": 0x00FC})
    emu.call("set_register", {"reg": "AF", "value": 0x8300})
    emu.call("set_register", {"reg": "PC", "value": code})
    for step in range(1, 9):
        emu.call("step_into")
        if emu.call("get_periph_z80_pio")["port_a"]["int_enable"]:
            return p, step
    return p, None


def _legacy_without_cksw_key(tmp):
    """Snapshot verze 1564848f s odstraněným klíčem cksw (podoba ještě starších verzí)."""
    src = _REPO_ROOT / "tests" / "data" / "replay" / "legacy-1564848f-row156.mzs"
    dst = tmp / "legacy-no-cksw-key.mzs"
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "hw/gdg.xml":
                text = data.decode("utf-8")
                lines = [ln for ln in text.splitlines(keepends=True) if "<cksw " not in ln]
                if len(lines) != len(text.splitlines()) - 1:
                    raise TestFailure("legacy snapshot does not hold exactly one cksw key")
                data = "".join(lines).encode("utf-8")
            elif item.filename == "manifest.xml":
                # obsah se změnil; bez uloženého součtu ho čtečka nekontroluje
                text, count = re.subn(r"<checksum.*?</checksum>\s*", "", data.decode("utf-8"), flags=re.S)
                if count != 1:
                    raise TestFailure("legacy snapshot manifest does not hold one checksum")
                data = text.encode("utf-8")
            zout.writestr(item.filename, data)
    return dst


def _hsync_status_at(emu, line):
    """Stavový bit HSYNC (pole sts_hsync) ve sloupcích 914 až 927 řádku ``line``.

    V nezkráceném řádku začíná stavový HSYNC ve sloupci 928, ve zkráceném
    už v 912; hodnota v tomto okně tedy říká, podle které délky emulátor
    naplánoval události řádku. Vrací None, když se okno netrefilo (krok
    instrukce je 20 tiků).
    """
    # NOP (20 tiků), INC HL (30) a LD A,n (35), každá následovaná NOPy
    stubs = {20: (0x2E00, "00"), 30: (0x2E10, "23"), 35: (0x2E20, "3e00")}
    for addr, code in stubs.values():
        emu.call("mem_write", {"addr": addr, "data_hex": code + "00" * 8})
    p = _pos(emu)
    if p["line"] != line or p["col"] > 914:
        return None
    while p["line"] == line and p["col"] < 914:
        fit = [t for t in (20, 30, 35) if 914 <= p["col"] + t <= 927]
        emu.call("set_register", {"reg": "PC", "value": stubs[fit[0] if fit else 20][0]})
        emu.call("step_into")
        p = _pos(emu)
    if p["line"] != line or p["col"] > 927:
        return None
    return emu.call("get_periph_gdg")["sts_hsync"]


def _write_boundary(emu, on, first=60, last=260, continuous=False, steady=False):
    """Délka řádku podle sloupce, ve kterém skončil zápis bitu CKSW.

    Instrukce OUT (C),A zapisující hodnotu ``on`` (předtím je bit opačný
    a ustálený) se časuje tak, aby její I/O cyklus (poslední M-cyklus
    instrukce, 60 tiků od začátku) končil v daném sloupci kolem načtení
    bitu. Začátek se doladí výplňovými instrukcemi (_PADS, kroky po
    5 ticích) a fází CPU vůči řádku: každý nezkrácený řádek ji posune
    o 1 tik, zkrácený ji drží, proto se před měřením vkládá proměnný počet
    nezkrácených řádků. Měří se v řádcích ``first`` až ``last``: mimo
    obrazovou část (řádky nad 245) čeká v době zápisu už událost konce
    viditelné části, kterou pozdní zápis musí přeplánovat.

    S ``continuous`` proběhne výplň, zápis a čtení Status registru jedním
    během bez zastavení: každé zastavení znovu vybere nejbližší událost
    a zakrylo by chybějící přeplánování po pozdním zápisu. Stav HSYNC proto
    čte sám program instrukcí IN A,(0CEh), jejíž I/O cyklus končí 130 tiků
    po konci zápisu (sloupce 917 až 924), a uloží ho do paměti. Sloupec
    konce zápisu se pak neověřuje krokem, jen výpočtem, a délka řádku se
    neměří (vrácený slovník délek nese hodnoty očekávané podle pravidla). Se ``steady`` se
    zapisuje hodnota, kterou bit už má (referenční stav pro obě délky).

    Vrací (délky, stavy): slovník {sloupec konce: délka tohoto řádku}
    a slovník {sloupec konce: stavový bit HSYNC ve sloupcích 914 až 927}
    (jen kde se okno trefilo).
    """
    code = 0x2F00
    pads = {ticks: pad_hex for pad_hex, ticks in _PADS}
    found = {}
    status = {}
    line = first
    for target in range(_LATCH_COL - 3, _LATCH_COL + _WR_EDGE + 3):
        for k in range(40):
            line = line + 21 if line + 21 <= last else first + k % 7
            _cksw(emu, False)
            _goto(emu, line - 14, 0)
            _goto(emu, line - 12 + k % 10, 0)
            _cksw(emu, on if steady else not on)
            _goto(emu, line - 2, 0)
            # souvislý běh nemůže použít výplň s LD A,n; výplně 40, 45 a 50
            # tiků bez ní stačí, když se přiblíží o 50 tiků místo o 75
            a = _goto(emu, line, target - 60 - (50 if continuous else 75), pc=_NOPS)
            pad_hex = pads.get(target - 60 - a["col"])
            if pad_hex is None:
                continue
            if continuous:
                # Výplň s LD A,n by přepsala zapisovanou hodnotu, kterou A
                # nese od začátku; přeskočí se. Za OUT: NOP, NOP, LD D,0
                # (75 tiků), IN A,(0CEh) (55 tiků), LD (2FF0h),A a NOPy.
                if "3e" in pad_hex:
                    continue
                emu.call("mem_write", {"addr": code, "data_hex":
                                       pad_hex + "ed79" + "0000" + "1600" + "dbce" + "32f02f" + "00" * 64})
                emu.call("mem_write", {"addr": 0x2FF0, "data_hex": "ff"})
                emu.call("set_register", {"reg": "BC", "value": _PORT_CKSW})
                emu.call("set_register", {"reg": "AF", "value": 0x8000 if on else 0x0000})
                emu.call("set_register", {"reg": "PC", "value": code})
                # Volný běh o jeden snímek (program po uložení stavu doběhne
                # NOPy do smyčky na _LOOP). Běh na pozici rastru by po každé
                # instrukci vybíral nejbližší událost znovu. Délku řádku tu
                # změřit nejde (snímek uběhne); slovník délek nese hodnotu
                # očekávanou podle pravidla a kontroluje se jen stav HSYNC.
                emu.call("run", {"frames": 1})
                st = emu.call("state_observe", {"windows": [{"addr": 0x2FF0, "len": 1}]})
                byte = base64.b64decode(st["windows"][0]["data_b64"])[0]
                if byte == 0xFF:
                    raise TestFailure(f"the program did not store the status for column {target}")
                status[target] = (byte >> 5) & 1
                new_len = _LINE_SHORT if on else _LINE
                old_len = new_len if steady else (_LINE if on else _LINE_SHORT)
                found[target] = new_len if target < _LATCH_COL + _WR_EDGE else old_len
                break
            emu.call("mem_write", {"addr": code, "data_hex": pad_hex + "ed79" + "00" * 64})
            emu.call("set_register", {"reg": "BC", "value": _PORT_CKSW})
            emu.call("set_register", {"reg": "AF", "value": 0})
            emu.call("set_register", {"reg": "PC", "value": code})
            for _ in range(8):
                if emu.call("get_registers")["PC"] == code + len(pad_hex) // 2:
                    break
                emu.call("step_into")
            # výplňové instrukce smějí změnit A (LD A,n); F zůstává 0
            emu.call("set_register", {"reg": "AF", "value": 0x8000 if on else 0x0000})
            emu.call("step_into")
            end = _pos(emu)
            if end["line"] != line or end["col"] != target:
                raise TestFailure(f"write timed for column {target} ended at {end['line']}/{end['col']}")
            sts = _hsync_status_at(emu, line)
            if sts is not None:
                status[target] = sts
            b = _goto(emu, line + 1, 0)
            found[target] = b["row_start"] - a["row_start"]
            break
    return found, status


def _machine_state(emu):
    """Stav stroje pro porovnání běhu se snapshotem a bez něj."""
    r = emu.call("get_raster_pos")
    return {
        "registers": emu.call("get_registers"),
        "raster": {k: r[k] for k in ("frame_number", "scanline", "column_pixel")},
        "gdg": emu.call("get_periph_gdg"),
        "i8253": emu.call("get_periph_i8253"),
        "z80_pio": emu.call("get_periph_z80_pio"),
        "hash": emu.call("machine_hash", {"parts": ["cpu", "ram", "vram", "periph"]})["parts"],
    }


def _replay_case(emu, tmp, on, interval):
    """Záznam smyčky s daným CKSW, přehrání a goto oběma způsoby zadání cíle.

    Bod cíle se odečte při záznamu (snímek, takty ve snímku, absolutní čas);
    přehrání do něj musí dojít bez rozchodu otisků a oba zápisy cíle musí
    skončit v témže stavu času.
    """
    _cksw(emu, on)
    emu.call("set_register", {"reg": "PC", "value": _LOOP})
    emu.call("run", {"frames": 2})
    rec = tmp / f"cksw{int(on)}_{interval}.mzr"
    emu.call("replay_record_start", {"path": str(rec), "hash_interval": interval})
    emu.call("run", {"frames": 4})
    emu.call("step_into")
    point = emu.call("state_observe", {"windows": []})["time"]
    emu.call("run", {"frames": 16})
    emu.call("replay_record_stop")
    emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
    for target in ({"t": point["ticks"]},
                   {"frame": point["frame"], "frame_ticks": point["frame_ticks"]}):
        res = emu.call("replay_goto", target)
        st = emu.call("replay_status")
        now = emu.call("state_observe", {"windows": []})["time"]
        how = "time" if "t" in target else "frame + frame_ticks"
        check(res.get("reached") and now == point,
              f"CKSW={int(on)}, hash interval {interval}: replay goto by {how} lands on the recorded point"
              + ("" if now == point else f" (expected {point}, got {now}, {st.get('outcome_message')})"))
    emu.call("replay_play_stop")


def _diff(a, b, path=""):
    """Seznam cest, kde se dva JSON stavy liší (pro čitelný FAIL)."""
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for k in sorted(set(a) | set(b)):
            out += _diff(a.get(k), b.get(k), f"{path}.{k}")
        return out
    return [] if a == b else [f"{path}: {a!r} != {b!r}"]


def _goto_rejected(emu, target):
    """True = replay_goto cíl odmítl, nebo ho ohlásil jako nedosažený."""
    try:
        res = emu.call("replay_goto", target)
    except TestFailure as e:
        if not str(e).startswith("replay_goto failed:"):
            raise
        return True
    return res.get("done") is True and res.get("reached") is False


def _goto_bounds(emu, tmp):
    """Záznam s CKSW = 1 a cíle goto mimo rozsah (vždy od nového přehrání)."""
    _cksw(emu, True)
    emu.call("run", {"frames": 2})
    _goto(emu, 150, 500)
    start = emu.call("state_observe", {"windows": []})["time"]
    rec = tmp / "bounds.mzr"
    emu.call("replay_record_start", {"path": str(rec), "hash_interval": 1})
    emu.call("run", {"frames": 16})
    emu.call("replay_record_stop")
    emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
    res = emu.call("replay_goto", {"frame": start["frame"], "frame_ticks": start["frame_ticks"]})
    now = emu.call("state_observe", {"windows": []})["time"]
    check(res.get("reached") and now == start, "replay goto reaches the recording start point")
    frame = start["frame"] + 5
    for target, what in (
            ({"frame": start["frame"], "frame_ticks": 5000}, "a target before the recording start is rejected"),
            ({"frame": frame, "frame_ticks": _FRAME_SHORT}, "frame_ticks equal to the short frame length is not reached"),
            ({"frame": frame, "frame_ticks": _FRAME - 1}, "frame_ticks beyond the short frame is not reached"),
            ({"frame": frame + 2 ** 32, "frame_ticks": 800}, "a frame number above 32 bits is rejected")):
        emu.call("replay_play_stop")
        emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
        check(_goto_rejected(emu, target), "replay goto: " + what)
    emu.call("replay_play_stop")
    emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
    res = emu.call("replay_goto", {"frame": frame, "frame_ticks": _FRAME_SHORT - 1})
    check(res.get("reached") is True, "replay goto: the last tick of a short frame is reached")
    emu.call("replay_play_stop")

    # Odložené čekání (timeout_ms = 0): výsledek kontroly rozsahu nese
    # záznam zastavení daného běhu (důvod), ne až odpověď goto.
    for ticks, want in ((_FRAME_SHORT - 1, "replay_goto"), (_FRAME_SHORT, "replay_goto_out_of_range")):
        emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
        res = emu.call("replay_goto", {"frame": frame, "frame_ticks": ticks, "timeout_ms": 0})
        stop = emu.call("wait_stop", {"run_seq": res["run_seq"], "timeout_ms": 20000})
        record = stop.get("record") or {}
        reason = record.get("reason")
        check(reason == want, f"deferred replay goto to tick {ticks}: stop reason {want} (got {reason})")
        check(bool(record.get("out_of_range")) == (ticks >= _FRAME_SHORT),
              f"deferred replay goto to tick {ticks}: the stop record carries the range result")
        emu.call("replay_play_stop")

    # Cíl v posledním snímku záznamu: běh skončí koncem přehrávání (důvod
    # replay_end), výsledek kontroly rozsahu musí i tak být v odpovědi.
    last = start["frame"] + 15
    emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
    res = emu.call("replay_goto", {"frame": last, "frame_ticks": _FRAME_SHORT})
    check(res.get("reached") is False and res.get("out_of_range") is True,
          f"replay goto beyond the last frame reports out_of_range (stopped by {res.get('stopped_by')})")
    # přehrávání tím doběhlo do konce záznamu, není co zastavovat


def _speed_phase(exe, percent):
    """Bez CKSW: běh po načtení snapshotu proti nepřerušenému běhu při dané rychlosti."""
    tmp = Path(tempfile.mkdtemp(prefix="gdg_cksw_speed_"))
    emu = None
    try:
        emu = PipeEmu(exe, tmp)
        _setup(emu)
        first = {"path": str(tmp / "initial.mzs")}
        emu.call("snapshot_save", first)
        emu.call("snapshot_load", first)
        emu.call("set_speed", {"mode": "custom", "percent": percent})
        emu.call("run", {"frames": 3})
        saved = {"path": str(tmp / "saved.mzs")}
        emu.call("snapshot_save", saved)
        emu.call("run", {"frames": 1})
        want = (emu.call("state_observe", {"windows": []})["time"],
                emu.call("machine_hash", {"parts": ["periph"]})["parts"]["periph"])
        emu.call("run", {"frames": 2})
        emu.call("snapshot_load", saved)
        emu.call("run", {"frames": 1})
        got = (emu.call("state_observe", {"windows": []})["time"],
               emu.call("machine_hash", {"parts": ["periph"]})["parts"]["periph"])
        check(want == got, f"speed {percent} %, no CKSW: run after snapshot load equals the uninterrupted run")
    finally:
        if emu is not None:
            emu.close()
        emu_test_proc.rmtree_retry(tmp)


def _setup(emu):
    """Program v RAM, DI, MAX SPEED, grafický režim DMD = 00h."""
    emu.call("pause")
    emu.call("io_write", {"port": 0xE0, "value": 0})
    emu.call("io_write", {"port": 0xE1, "value": 0})
    emu.call("io_write", {"port": 0xCE, "value": 0x00})
    emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
    emu.call("set_speed", {"mode": "max"})
    emu.call("mem_write", {"addr": _LOOP, "data_hex": "c3%02x%02x" % (_LOOP & 0xFF, _LOOP >> 8)})
    emu.call("mem_write", {"addr": _NOPS, "data_hex": "00" * 8192})
    emu.call("set_register", {"reg": "PC", "value": _LOOP})


def run(exe):
    """Všechny kroky testu na jedné binárce. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="gdg_cksw_"))
    emu = None
    try:
        emu = PipeEmu(exe, tmp)
        _setup(emu)

        # 1 - bez CKSW
        check(_status(emu) == 0, "status bit 2 is 0 after start")
        n = _frame_len(emu)
        check(n == _FRAME, f"frame without CKSW has {_FRAME} ticks (got {n})")
        n = _row_len(emu, 100, 300)
        check(n == _LINE, f"row without CKSW has {_LINE} ticks (got {n})")

        # 2 - zápis před načtením bitu: krátký už tento řádek
        a = _goto(emu, 120, 700)
        check(a["col"] < _LATCH_COL, f"write position {a['col']} is before the latch column")
        _cksw(emu, True)
        check(_status(emu) == 1, "status bit 2 follows the write immediately")
        b = _goto(emu, 121, 0)
        n = b["row_start"] - a["row_start"]
        check(n == _LINE_SHORT, f"row written before the latch has {_LINE_SHORT} ticks (got {n})")

        # 3 - zápis za načtením bitu: tento řádek ještě dlouhý, další krátký
        _cksw(emu, False)
        _goto(emu, 130, 300)
        a = _goto(emu, 140, _LATCH_COL + 10)
        _cksw(emu, True)
        b = _goto(emu, 141, 0)
        n = b["row_start"] - a["row_start"]
        check(n == _LINE, f"row written after the latch keeps {_LINE} ticks (got {n})")
        n = _row_len(emu, 141, 300)
        check(n == _LINE_SHORT, f"the next row has {_LINE_SHORT} ticks (got {n})")

        # 3a - hranice zápisu po sloupcích: klopný obvod se překlápí _WR_EDGE
        # tiků před koncem M-cyklu, takže zápis končící ve sloupcích
        # _LATCH_COL a _LATCH_COL + 1 ještě platí pro tento řádek
        # stavový HSYNC ve sloupcích 914 až 927 ustáleného řádku obou délek
        ref = {}
        for want_len, on in ((_LINE, False), (_LINE_SHORT, True)):
            _cksw(emu, on)
            for line in range(262, 280):
                _goto(emu, line, 880, pc=_NOPS)
                sts = _hsync_status_at(emu, line)
                if sts is not None:
                    ref[want_len] = sts
                    break
        check(len(ref) == 2 and ref[_LINE] != ref[_LINE_SHORT],
              f"status HSYNC in columns 914-927 tells a short row from a full one ({ref})")
        for on, first, last, cont in ((True, 60, 240, False), (False, 60, 240, False),
                                      (True, 250, 284, False), (False, 250, 284, False),
                                      (True, 250, 284, True), (False, 250, 284, True)):
            found, status = _write_boundary(emu, on, first, last, cont)
            new, old = (_LINE_SHORT, _LINE) if on else (_LINE, _LINE_SHORT)
            where = "picture rows" if last < 245 else "rows below the picture"
            want_status = ref
            if cont:
                where += ", one uninterrupted run"
                # reference: tentýž program bez změny bitu, čte Status registr sám
                want_status = {}
                for length, state in ((_LINE, False), (_LINE_SHORT, True)):
                    _, st = _write_boundary(emu, state, first, last, True, steady=True)
                    if len(set(st.values())) == 1:
                        want_status[length] = next(iter(st.values()))
                check(len(want_status) == 2 and want_status[_LINE] != want_status[_LINE_SHORT],
                      f"status HSYNC read by the program tells a short row from a full one ({want_status})")
            bad = {c: v for c, v in status.items() if v != want_status.get(found[c])}
            check(len(status) >= 4 and not bad,
                  f"status HSYNC after a write of CKSW={int(on)} follows the row length, {where}"
                  f" ({len(status)} columns checked, wrong: {bad})")
            want = {c: (new if c < _LATCH_COL + _WR_EDGE else old) for c in found}
            edge = {c: found[c] for c in sorted(found) if abs(c - _LATCH_COL) <= 4}
            check(all(c in found for c in range(_LATCH_COL - 2, _LATCH_COL + _WR_EDGE + 2)),
                  f"writes of CKSW={int(on)} end in every column around the latch, {where} ({sorted(edge)})")
            check(found == want,
                  f"write of CKSW={int(on)} ending before column {_LATCH_COL + _WR_EDGE} changes this row,"
                  f" {where} ({edge})")
        _cksw(emu, True)

        # 4 - CKSW trvale
        _goto(emu, 5, 0)
        n = _frame_len(emu)
        check(n == _FRAME_SHORT, f"frame with CKSW has {_FRAME_SHORT} ticks (got {n})")
        p = _goto(emu, 200, 1100, pc=_NOPS)
        cols = [p["col"]]
        line = p["line"]
        while line == 200:
            emu.call("step_into")
            q = _pos(emu)
            line = q["line"]
            if line == 200:
                cols.append(q["col"])
        check(max(cols) < _LINE_SHORT, f"column stays below {_LINE_SHORT} in a short row (max {max(cols)})")
        check(q["line"] == 201 and q["col"] < 20, f"row 201 starts right after (column {q['col']})")
        a = _goto(emu, 311, 900)
        b = _goto(emu, 0, 100)
        check(b["frame"] == a["frame"] + 1 and b["ticks"] > a["ticks"]
              and b["ticks"] - a["ticks"] < 2 * _LINE,
              f"absolute time is continuous over the frame end (delta {b['ticks'] - a['ticks']})")

        # 5 - snapshot uprostřed krátkého řádku (za hranou HSYNC, před koncem řádku)
        # Nejdřív jedno uložení a načtení: srovná stav, který snapshot nenese
        # přesně (registr šumu PSG), aby šel běh bez načtení porovnat s během
        # po načtení.
        norm = tmp / "norm.mzs"
        emu.call("snapshot_save", {"path": str(norm)})
        emu.call("snapshot_load", {"path": str(norm)})
        saved = _goto(emu, 150, 1000)
        snap = tmp / "cksw.mzs"
        emu.call("snapshot_save", {"path": str(snap)})
        # Přímý běh bez načtení je reference: chybu společnou zápisu i čtení
        # (například neobnovené zkrácení běžícího řádku) dvě načtení neodhalí.
        emu.call("run", {"frames": 2})
        state_direct = _machine_state(emu)
        time_direct = _pos(emu)["ticks"] - saved["ticks"]
        # Referenční běh také vychází z načteného snapshotu: načtení samo
        # normalizuje stav, který snapshot nenese přesně (nepoužitý registr
        # šumu tónových kanálů PSG má za běhu 0, po načtení výchozí hodnotu),
        # a otisk periferií by se jinak lišil nezávisle na CKSW.
        emu.call("snapshot_load", {"path": str(snap)})
        check(emu.call("get_periph_gdg")["line_ticks"] == _LINE_SHORT,
              "load in a short row restores its length")
        emu.call("run", {"frames": 2})
        state_a = _machine_state(emu)
        time_a = _pos(emu)["ticks"] - saved["ticks"]
        diff = _diff(state_direct, state_a)
        check(not diff, "state after load + run equals the uninterrupted run"
              + ("" if not diff else " - differs in: " + "; ".join(diff[:6])))
        check(time_direct == time_a,
              f"elapsed time after load equals the uninterrupted run ({time_direct} vs {time_a})")
        _cksw(emu, False)
        _goto(emu, 103, 400)
        emu.call("snapshot_load", {"path": str(snap)})
        r = _pos(emu)
        check((r["line"], r["col"]) == (saved["line"], saved["col"]),
              f"load restores raster {saved['line']}/{saved['col']} (got {r['line']}/{r['col']})")
        check(r["ticks"] == saved["ticks"], "load restores absolute time")
        emu.call("run", {"frames": 2})
        diff = _diff(state_a, _machine_state(emu))
        check(not diff, "state after load + run equals run without load"
              + ("" if not diff else " - differs in: " + "; ".join(diff[:6])))
        time_b = _pos(emu)["ticks"] - saved["ticks"]
        check(time_a == time_b, f"elapsed time after load equals run without load ({time_a} vs {time_b})")

        # 5a - zkrácení platí jen pro řádek, ve kterém se bit načetl
        _cksw(emu, True)
        _goto(emu, 160, 850)
        check(emu.call("get_periph_gdg")["line_ticks"] == _LINE_SHORT,
              "line_ticks is short after the latch of a short row")
        _cksw(emu, False)
        _goto(emu, 161, 100)
        check(emu.call("get_periph_gdg")["line_ticks"] == _LINE,
              "line_ticks is full again in the next row after clearing CKSW")

        # 5b - čítač 0 obvodu 8253 běží podle času i přes konce krátkých snímků
        divider = 10007
        emu.call("io_write", {"port": 0xD7, "value": 0x34})
        emu.call("io_write", {"port": 0xD4, "value": divider & 0xFF})
        emu.call("io_write", {"port": 0xD4, "value": divider >> 8})
        for on in (False, True):
            _cksw(emu, on)
            d = _ctc0_drift(emu, divider)
            check(abs(d) <= 1, f"8253 counter 0 follows time over three frames, CKSW={int(on)} (drift {d})")
        # Režim 3 s půlperiodou 4800 tiků (kratší než zkrácení snímku 4992):
        # událost změny výstupu čeká přes každý konec snímku těsně za ním.
        emu.call("io_write", {"port": 0xD7, "value": 0x36})
        emu.call("io_write", {"port": 0xD4, "value": 600 & 0xFF})
        emu.call("io_write", {"port": 0xD4, "value": 600 >> 8})
        emu.call("run", {"frames": 2})
        n = _ctc0_out_toggles(emu, 12)
        check(n >= 2, f"8253 counter 0 output keeps toggling after short frames ({n} changes in 12 rows)")

        # 5c - zpožděné povolení přerušení PIO-Z80 přes konec krátkého snímku
        _cksw(emu, False)
        _goto(emu, 50, 0)
        _, mid = _pio_enable_steps(emu, 100, 500)
        check(mid is not None, f"PIO-Z80 interrupt enable takes effect in mid frame (steps {mid})")
        _cksw(emu, True)
        _goto(emu, 50, 0)
        rel, end = _pio_enable_over_frame_end(emu, _LINE_SHORT)
        check(rel is not None, f"OUT placed so that the PIO-Z80 enable waits over a short frame end (start {rel})")
        check(end == mid, f"PIO-Z80 interrupt enable over a short frame end takes the same steps ({end} vs {mid})")
        emu.call("io_write", {"port": 0xFC, "value": 0x03})

        # 5e - nástroje: záznam zápisu bitu v hwlogu a breakpoint cksw_change
        code = 0x2F00
        emu.call("mem_write", {"addr": code, "data_hex": "ed79" + "00" * 32})
        emu.call("trace_start", {"channel": "hwlog"})
        for value in (0x80, 0x00):
            _goto(emu, 100, 300, pc=_NOPS)
            emu.call("set_register", {"reg": "BC", "value": _PORT_CKSW})
            emu.call("set_register", {"reg": "AF", "value": value << 8})
            emu.call("set_register", {"reg": "PC", "value": code})
            emu.call("step_into")
        page = emu.call("trace_read", {"channel": "hwlog", "limit": 200})
        emu.call("trace_stop", {"channel": "hwlog"})
        cols = page["columns"]
        recs = [dict(zip(cols, r)) for r in page["rows"]]
        got = [r["payload"][:8] for r in recs if r["chip"] == "gdg_video" and r["sub"] == 9]
        check(got == ["CF 07 80", "CF 07 00"], f"hwlog records both CKSW writes with the byte written ({got})")
        bp = emu.call("bp_create_with_init", {"fields": ["type", "event_name"],
                                              "type": "HW_EVENT", "event_name": "cksw_change"})
        check(bp.get("created") is True, f"breakpoint on cksw_change is created ({bp})")
        _goto(emu, 100, 300, pc=_NOPS)
        emu.call("set_register", {"reg": "BC", "value": _PORT_CKSW})
        emu.call("set_register", {"reg": "AF", "value": 0x8000})
        emu.call("set_register", {"reg": "PC", "value": code})
        res = emu.call("run", {"frames": 2})
        check(res.get("stopped_by") == "breakpoint" and emu.call("get_registers")["PC"] == code + 2,
              f"breakpoint on cksw_change stops right after the OUT ({res.get('stopped_by')})")
        emu.call("bp_remove", {"id": bp["id"]})
        _cksw(emu, True)

        # 5d - snapshot bez klíče cksw (starší verze) bit nedědí z běžícího stroje
        _cksw(emu, True)
        _goto(emu, 200, 100)
        emu.call("snapshot_load", {"path": str(_legacy_without_cksw_key(tmp))})
        check(_status(emu) == 0 and emu.call("get_periph_gdg")["cksw"] == 0,
              "snapshot without the cksw key loads with CKSW cleared")
        n = _row_len(emu, 170, 300)
        check(n == _LINE, f"row after such a load has {_LINE} ticks (got {n})")
        _setup(emu)
        _cksw(emu, True)

        # 6 - vypnutí
        _cksw(emu, False)
        check(_status(emu) == 0, "status bit 2 is 0 after clearing")
        _goto(emu, 5, 0)
        n = _frame_len(emu)
        check(n == _FRAME, f"frame after clearing CKSW has {_FRAME} ticks again (got {n})")

        # 7 - záznam, přehrání a goto (bez CKSW i s CKSW)
        for on in (False, True):
            for interval in (1, 10):
                _replay_case(emu, tmp, on, interval)

        # 8 - hranice cíle goto zadaného snímkem
        _goto_bounds(emu, tmp)
        emu.close()
        emu = None

        # 9 - fáze 20ms události po načtení snapshotu (samostatné procesy)
        for percent in (100, 150):
            _speed_phase(exe, percent)
        return True
    except TestFailure as e:
        print(f"  FAILED: {e}")
        return False
    finally:
        if emu is not None:
            emu.close()
        emu_test_proc.rmtree_retry(tmp)


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe("mz800emu")
    if exe is None:
        print("SKIP: no emulator binary found")
        return 77
    ok = run(exe)
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
