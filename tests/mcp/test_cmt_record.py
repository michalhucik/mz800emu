#!/usr/bin/env python3
"""E2E test záznamu do pásky přes MCP (pipe): nová páska, záznam, uložení, export, varování.

Hrany na zápisovém výstupu kazety dává krátký strojový program v RAM
(2000h): BSR přes řídicí registr 8255 (port 0D3h v režimu MZ-800 i adresa
0E003h v režimu MZ-700; hodnota 03h = PC1 = 1, 02h = PC1 = 0, báze
reference/agent/hw/05-pio-8255.md "Tabulka BSR hodnot" a hw/14-cmt.md
"Adresy 8255 PC"), 200 period, tedy 400 hran. Signál není ve formátu ROM, záznam proto skončí jako jeden surový
blok. Motor se nespouští programem; záznam začíná v pauze a pauzu zruší
MCP (cmt_transport pause=0). Skutečné SAVE z ROM testuje fáze E.

  A  emu_cmt_new_tape, emu_cmt_record() bez cesty, hrany, emu_cmt_stop ->
     recorded {blocks 1, mzf 0, raw 1}; periph/cmt/tape: modified true,
     path null, 1 blok; emu_cmt_stop bez nahrávání pole recorded nemá.
  B  emu_cmt_save(t.tmz) -> modified false, path t.tmz; emu_cmt_open(t.tmz)
     dá 1 blok; emu_cmt_export_block(0, x.mzf) = chyba (surový blok),
     emu_cmt_export_block(0, x.wav) vytvoří WAV.
  C  Eject pásky Modified vrátí varování "Unsaved tape
     changes were discarded"; emu_cmt_open jiné pásky během nahrávání
     záznam zahodí s varováním, 100x po sobě bez pádu; reset stroje během
     nahrávání nahrávání nezastaví.
  D  Neúspěšné otevření: emu_cmt_open a emu_media_insert (slot cmt) neexistujícího
     a poškozeného souboru s páskou Modified i během nahrávání selžou
     a pásku i nahrávání nechají; media_insert / media_eject vrací
     varování jako cmt_open / eject; emu_cmt_new_tape nad páskou Modified
     varuje; cmt_record bez objektu data; varování uložení (.tzx s bloky
     MZ, rychlost bloku 0 v MZT); export bloku mimo rozsah a s nepodporovanou
     příponou selže.
  E  SAVE z ROM na mz800emu, mz700emu-pal, mz700emu-ntsc a mz1500emu
     (každý vlastní proces): emu_cmt_record() bez pásky, hlavička MZF
     (typ 01h, jméno, 300 B na 1200h, komentář 18h-7Fh se vzorem) na 10F0h,
     tělo na 1200h, stub v RAM (6000h) volá ROM @WHEAD (0021h) a @WDATA
     (0024h); emu_cmt_stop -> recorded {blocks 1, mzf 1, raw 0};
     emu_cmt_export_block(0) = bajtově shodný MZF včetně komentáře (pozorováno
     na všech čtyřech binárkách, proto je to kontrola). Zpět: vynulovaná paměť,
     PLAY, stub @RHEAD (0027h) + @RDATA (002Ah) -> tělo i hlavička na 10F0h
     shodné. Surový blok: jen @WHEAD (bez těla) se uloží jako blok 0x18
     s úrovní PC1; přehrání a @RHEAD hlavičku přečte - ověřuje polaritu
     přehrání záznamu skutečnou ROM. Vstupní body a buffer HEAD: báze
     firmware/rom-deep/07-cmt.md a 02-service-vector.md (MZ-800), pro MZ-700
     a MZ-1500 ověřeno v bajtech ROM tohoto repa (viz _HEAD_ADDR). Čas běží
     bez omezení rychlosti (headless ``run``), signál se nezkracuje.
  F  Výsledek nahrávání (vlastní proces s ``[CMT] rec_autostop_s`` 1 s):
     periph/cmt/tape ``last_recording`` - null po startu, výsledek
     nahrávání ukončeného samo (automaticky po tichu; emu_cmt_stop ho
     nevrátí), zahozené nahrávání po vysunutí (bez pásky) i po otevření
     jiné pásky (vložení ho nesmaže); emu_cmt_save se odmítne při záznamu i přehrávání vstupů
     (soubor nevznikne); při přehrávání emu_cmt_record s cestou odmítnut
     (soubor nevznikne), bez cesty povolen, emu_cmt_export_block povolen.

  G  SAVE z ROM se zapnutým CMT hackem a Direct Save (výchozí nové
     konfigurace) na všech čtyřech binárkách: v RECORD patch zápisu v ROM
     není (``direct_save`` false, ``direct_save_enabled`` true, bajty ROM
     původní), emu_cmt_record() bez varování Direct, stub
     @WHEAD + @WDATA jako ve fázi E -> hrana PC3 programu (@MGON) rozjede
     nahrávání i s hackem; emu_cmt_stop -> jeden MZF blok bajtově shodný
     s pamětí; zpět LOAD z ROM přes patch hacku (deck ve STOP, blok 0) do
     vynulované paměti; přehrávání s hackem beze změny: emu_cmt_play_paused
     vrátí varování Direct a hrana PC3 programu pásku nerozjede (paused true);
     vlastní ukládací rutina programu (hrana PC3 a hrany PC1 bez ROM) nahraje
     i s hackem (surový blok); nakonec totéž SAVE z ROM s ``direct_save``
     vypnutým přes settings_set (opět jeden MZF blok).

Každý proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní
INI s vypnutým CMT hackem, ve fázi G bez sekce [CMTHACK], ``--no-save-ini``),
bez TCP portu; zabíjí se jen vlastní PID. Výstup anglicky.

Spuštění: bez argumentů fáze A až D (ctest mcp_cmt_record); ``--rom-save
<binárka>`` jen fáze E pro jednu binárku (ctest mcp_cmt_rom_save_<binárka>,
jeden na každou ze čtyř binárek); ``--rom-save-direct <binárka>`` jen fáze
G (ctest mcp_cmt_rom_save_direct_<binárka>).

Exit code: 0 = PASS, 1 = FAIL (ve fázi E i chybějící binárka), 77 = SKIP
(fáze A až D bez binárky mz800emu).

"""

import base64
import json
import queue
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent

#: Adresa programu v RAM.
_PROG_ADDR = 0x2000
#: DI; LD B,200; loop: LD A,03h; OUT (0D3h),A; LD (0E003h),A; LD C,50;
#: DEC C; JR NZ,-3; LD A,02h; OUT (0D3h),A; LD (0E003h),A; LD C,50; DEC C;
#: JR NZ,-3; DJNZ loop; JR $. Zápis jde na obě cesty k řídicímu registru
#: 8255 (IORQ 0D3h v režimu MZ-800, mapovaná 0E003h v režimu MZ-700, báze
#: hw/05-pio-8255.md); po bootu do menu je stroj v režimu MZ-700 (zjištěno
#: zde: samotný OUT (0D3h) stav 8255 neměnil).
_PROG = bytes([0xF3, 0x06, 0xC8,
               0x3E, 0x03, 0xD3, 0xD3, 0x32, 0x03, 0xE0, 0x0E, 0x32, 0x0D, 0x20, 0xFD,
               0x3E, 0x02, 0xD3, 0xD3, 0x32, 0x03, 0xE0, 0x0E, 0x32, 0x0D, 0x20, 0xFD,
               0x10, 0xE6, 0x18, 0xFE])
#: Varování při ztrátě neuložených změn pásky.
_UNSAVED = "Unsaved tape changes were discarded"

#: Fáze E (SAVE z ROM): buffer hlavičky ROM, adresa těla, stuby a výsledky.
#: HEAD = 10F0h: báze reference/agent/firmware/rom-deep/07-cmt.md ("RAM
#: proměnné pro CMT") a 02-service-vector.md (@WHEAD "z 10F0h") pro MZ-800;
#: pro všechny tři ROM v tomto repu ověřeno v bajtech: 0021h = JP 0436h,
#: 0024h = JP 0475h, 0436h: ... LD HL,10F0h; LD BC,0080h (WHEAD zapíše 128 B
#: z 10F0h), 0475h: ... LD BC,(1102h); LD HL,(1104h) (WDATA bere délku
#: a zaváděcí adresu z hlavičky) - shodně v ROM_MZ700_13A_0000.c,
#: ROM_MZ1500_0000.c a ROM_MZ800_0000.c (src/emulator/mzarch/*/memory/ROM/).
_HEAD_ADDR = 0x10F0
_BODY_ADDR = 0x1200
_BODY_SIZE = 300
_STUB_ADDR = 0x6000
_SAVE_RESULT = 0x6080
_LOAD_RESULT = 0x6081
#: Nejvýš snímků na SAVE / LOAD z ROM 1:1 (hlavička ~14 s, tělo ~7 s, motor).
_ROM_FRAMES = 4000
#: 6000 CALL 0021h (@WHEAD) / JR C,600Eh / CALL 0024h (@WDATA) / JR C,600Eh /
#: LD A,55h / JR 6010h / 600E LD A,0EEh / 6010 LD (6080h),A / JR $
_SAVE_STUB = bytes([0xCD, 0x21, 0x00, 0x38, 0x09, 0xCD, 0x24, 0x00, 0x38, 0x04,
                    0x3E, 0x55, 0x18, 0x02, 0x3E, 0xEE, 0x32, 0x80, 0x60, 0x18, 0xFE])
#: Totéž s @RHEAD (0027h) a @RDATA (002Ah), výsledek na 6081h.
_LOAD_STUB = bytes([0xCD, 0x27, 0x00, 0x38, 0x09, 0xCD, 0x2A, 0x00, 0x38, 0x04,
                    0x3E, 0x55, 0x18, 0x02, 0x3E, 0xEE, 0x32, 0x81, 0x60, 0x18, 0xFE])
#: Jen hlavička: 6000 CALL 0021h (@WHEAD) / JR C,6009h / LD A,55h / JR 600Bh /
#: 6009 LD A,0EEh / 600B LD (6080h),A / JR $
_WHEAD_STUB = bytes([0xCD, 0x21, 0x00, 0x38, 0x04, 0x3E, 0x55, 0x18, 0x02, 0x3E, 0xEE,
                     0x32, 0x80, 0x60, 0x18, 0xFE])
#: Totéž s @RHEAD (0027h), výsledek na 6081h.
_RHEAD_STUB = bytes([0xCD, 0x27, 0x00, 0x38, 0x04, 0x3E, 0x55, 0x18, 0x02, 0x3E, 0xEE,
                     0x32, 0x81, 0x60, 0x18, 0xFE])
#: Binárky fáze E (MZ-800, MZ-700 PAL a NTSC, MZ-1500).
_ROM_SAVE_MACHINES = ("mz800emu", "mz700emu-pal", "mz700emu-ntsc", "mz1500emu")


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def _find_exe(name):
    """Binárka v kořeni repa (s .exe i bez), jinak None."""
    for c in (_REPO_ROOT / (name + ".exe"), _REPO_ROOT / name):
        if c.is_file():
            return c
    return None


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, extra_ini="", hack=False):
        cfg_dir.mkdir(parents=True, exist_ok=True)
        ini = cfg_dir / "test.ini"
        # hack=True: [CMTHACK] enable se nepíše, platí výchozí nové konfigurace
        # (hack zapnutý); extra_ini smí sekci [CMTHACK] doplnit jinými klíči
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       + ("" if hack else "[CMTHACK]\nenable = 0\n") + extra_ini, encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--no-save-ini", "--headless",
                "--no-first-run-windows", f"--cfg-dir={cfg_dir}",
                f"--work-dir={cfg_dir}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise TestFailure(f"no hello from emulator (exit code: {self.proc.poll()})")

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

    def call_raw(self, cmd, data=None, timeout=60.0):
        """Pošle request a vrátí celou response (i neúspěšnou)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to {cmd} (emulator exit code: {self.proc.poll()})")
        return resp

    def call(self, cmd, data=None, timeout=60.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        resp = self.call_raw(cmd, data, timeout)
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def ram(self, addr, length):
        """Přečte bajty z regionu User RAM."""
        d = self.call("region_read", {"region_id": 1, "offset": addr, "length": length})
        return base64.b64decode(d["data_b64"])

    def poke(self, addr, data):
        """Zapíše bajty do regionu User RAM."""
        self.call("region_write", {"region_id": 1, "offset": addr,
                                   "data_b64": base64.b64encode(bytes(data)).decode("ascii")})

    def run_until(self, addr, frames, step=100):
        """Běží po ``step`` snímcích, dokud bajt na ``addr`` není nenulový; vrátí ho a počet snímků."""
        done = 0
        while done < frames:
            self.call("run", {"frames": step}, timeout=120.0)
            done += step
            value = self.ram(addr, 1)[0]
            if value:
                return value, done
        return self.ram(addr, 1)[0], done

    def close(self):
        try:
            self.call("shutdown", timeout=10.0)
        except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
            pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()


def _record_edges(emu, frames=20):
    """Nahrávání (už zahájené) rozběhne a nechá program zapsat 400 hran."""
    emu.call("cmt_transport", {"action": "pause", "pause": 0})
    emu.call("region_write", {"region_id": 1, "offset": _PROG_ADDR,
                              "data_b64": base64.b64encode(_PROG).decode("ascii")})
    emu.call("set_register", {"reg": "PC", "value": _PROG_ADDR})
    emu.call("run", {"frames": frames}, timeout=120.0)


def phase_a_b(emu, tmp):
    """Nová páska, záznam bez cesty, výsledek, uložení, export."""
    print("Phase A: new tape, record without a path, stop -> recorded raw block")
    res = emu.call("cmt_new_tape")
    check("warning" not in res, f"new tape without a warning ({res})")
    tape = emu.call("cmt_tape_list")
    check(tape.get("available") and tape.get("count") == 0 and tape.get("path") is None
          and tape.get("modified") is False, f"new tape: 0 blocks, no path, not modified ({tape})")
    res = emu.call("cmt_record")
    check(res.get("path") is None, f"record without a path ({res})")
    check(emu.call("get_periph_cmt").get("state") == "record", "state is record")
    _record_edges(emu)
    res = emu.call("cmt_transport", {"action": "stop"})
    rec = res.get("recorded") or {}
    check(rec.get("blocks") == 1 and rec.get("raw") == 1 and rec.get("mzf") == 0,
          f"recorded one raw block ({res})")
    check("raw block" in rec.get("message", ""), f"recorded message ({rec.get('message')})")
    tape = emu.call("cmt_tape_list")
    check(tape.get("count") == 1 and tape.get("modified") is True and tape.get("path") is None,
          f"tape: 1 block, modified, no path ({tape.get('count')}, {tape.get('modified')}, {tape.get('path')})")
    res = emu.call("cmt_transport", {"action": "stop"})
    check("recorded" not in res, f"stop without recording has no 'recorded' ({res})")

    print("Phase B: save, reopen, export")
    tmz = tmp / "t.tmz"
    res = emu.call("cmt_save", {"path": str(tmz)})
    check(tmz.is_file() and res.get("path") == str(tmz), f"tape saved ({res})")
    tape = emu.call("cmt_tape_list")
    check(tape.get("modified") is False and tape.get("path") == str(tmz),
          f"after save: not modified, path is the file ({tape.get('modified')}, {tape.get('path')})")
    res = emu.call("cmt_open", {"path": str(tmz), "play_immediately": False})
    check("warning" not in res, f"reopen without a warning ({res})")
    check(emu.call("cmt_tape_list").get("count") == 1, "reopened tape has 1 block")
    resp = emu.call_raw("cmt_export_block", {"block": 0, "path": str(tmp / "x.mzf")})
    check(not resp.get("success") and not (tmp / "x.mzf").exists(),
          f"raw block cannot be exported as MZF ({resp.get('error')})")
    wav = tmp / "x.wav"
    res = emu.call("cmt_export_block", {"block": 0, "path": str(wav)})
    check(wav.is_file() and wav.stat().st_size > 44, f"block exported as WAV ({res})")
    resp = emu.call_raw("cmt_save", {})
    check(resp.get("success"), f"save to the current path ({resp.get('error')})")


def phase_c(emu, tmp):
    """Eject s Modified, open během nahrávání (100x), reset během nahrávání."""
    print("Phase C: eject of a modified tape, open while recording (100x), reset while recording")
    emu.call("cmt_new_tape")
    emu.call("cmt_record")
    _record_edges(emu)
    emu.call("cmt_transport", {"action": "stop"})
    check(emu.call("cmt_tape_list").get("modified") is True, "tape is modified")
    res = emu.call("cmt_transport", {"action": "eject"})
    check(res.get("warning") == _UNSAVED, f"eject of a modified tape warns ({res})")
    check(emu.call("cmt_tape_list").get("available") is False, "tape ejected")

    tmz = tmp / "t.tmz"
    for i in range(100):
        emu.call("cmt_record")
        _record_edges(emu, frames=2)
        res = emu.call("cmt_open", {"path": str(tmz), "play_immediately": False})
        if i == 0 or i == 99:
            check(res.get("warning", "").startswith(_UNSAVED),
                  f"[{i}] open while recording warns ({res.get('warning')})")
            st = emu.call("get_periph_cmt")
            check(st.get("state") == "stop" and st.get("filled"), f"[{i}] state stop, tape inserted")
            check(emu.call("cmt_tape_list").get("count") == 1, f"[{i}] the opened tape has its 1 block")
    check(emu.proc.poll() is None, "emulator still running after 100 discarded recordings")

    emu.call("cmt_record")
    emu.call("cmt_transport", {"action": "pause", "pause": 0})
    emu.call("reset")
    emu.call("run", {"frames": 5})
    check(emu.call("get_periph_cmt").get("state") == "record", "machine reset does not stop recording")
    emu.call("cmt_transport", {"action": "stop"})


def _modified_tape(emu):
    """Nová páska s jedním nahraným surovým blokem (Modified)."""
    emu.call("cmt_new_tape")
    emu.call("cmt_record")
    _record_edges(emu, frames=4)
    emu.call("cmt_transport", {"action": "stop"})
    tape = emu.call("cmt_tape_list")
    check(tape.get("count") == 1 and tape.get("modified") is True, f"modified tape with 1 block ({tape.get('count')})")


def phase_d(emu, tmp):
    """Neúspěšné otevření nic nezahodí, media_*, new_tape, cmt_record bez data, varování uložení, export."""
    print("Phase D: failed open keeps the tape, media_insert/eject, new_tape, save warnings, export errors")
    missing = tmp / "missing.tmz"
    corrupt = tmp / "corrupt.tmz"
    corrupt.write_text("this is not a tape file", encoding="utf-8")
    good = _REPO_ROOT / "tests" / "data" / "cmt" / "prog1.mzf"

    _modified_tape(emu)
    res = emu.call("cmt_new_tape")
    check(res.get("warning") == _UNSAVED, f"new tape over a modified tape warns ({res})")

    _modified_tape(emu)
    for bad in (missing, corrupt):
        for cmd, data in (("cmt_open", {"path": str(bad), "play_immediately": False}),
                          ("media_insert", {"slot": "cmt", "path": str(bad)})):
            resp = emu.call_raw(cmd, data)
            check(not resp.get("success"), f"{cmd} {bad.name} fails ({resp.get('error')})")
            tape = emu.call("cmt_tape_list")
            check(tape.get("available") and tape.get("count") == 1 and tape.get("modified") is True,
                  f"{cmd} {bad.name}: modified tape kept ({tape.get('count')}, {tape.get('modified')})")

    emu.call("cmt_record")
    _record_edges(emu, frames=2)
    for bad in (missing, corrupt):
        for cmd, data in (("cmt_open", {"path": str(bad), "play_immediately": False}),
                          ("media_insert", {"slot": "cmt", "path": str(bad)})):
            resp = emu.call_raw(cmd, data)
            check(not resp.get("success"), f"{cmd} {bad.name} while recording fails")
            check(emu.call("get_periph_cmt").get("state") == "record",
                  f"{cmd} {bad.name}: recording keeps running")
    _record_edges(emu, frames=2)
    res = emu.call("cmt_transport", {"action": "stop"})
    check((res.get("recorded") or {}).get("blocks") == 1, f"recording survived failed opens ({res.get('recorded')})")
    check(emu.call("cmt_tape_list").get("count") == 2, "recorded block appended to the kept tape")

    res = emu.call("media_insert", {"slot": "cmt", "path": str(good)})
    check(res.get("warning", "").startswith(_UNSAVED), f"media_insert over a modified tape warns ({res})")
    check(emu.call("cmt_tape_list").get("count") == 1, "media_insert inserted the new tape")
    res = emu.call("media_insert", {"slot": "cmt", "path": str(good)})
    check("warning" not in res, f"media_insert over an unchanged tape does not warn ({res})")
    _modified_tape(emu)
    res = emu.call("media_eject", {"slot": "cmt"})
    check(res.get("warning") == _UNSAVED, f"media_eject of a modified tape warns ({res})")
    res = emu.call("media_eject", {"slot": "cmt"})
    check("warning" not in res, f"media_eject without a tape does not warn ({res})")

    # cmt_record bez objektu data (path je nepovinný)
    emu.rid += 1
    emu.proc.stdin.write(json.dumps({"type": "request", "req_id": emu.rid, "cmd": "cmt_record"}) + "\n")
    emu.proc.stdin.flush()
    rid = emu.rid
    resp = emu._read(30.0, lambda m: m.get("type") == "response" and m.get("req_id") == rid)
    check(resp is not None and resp.get("success"), f"cmt_record without data succeeds ({resp})")
    emu.call("cmt_transport", {"action": "stop"})
    emu.call("cmt_transport", {"action": "eject"})

    # varování uložení: .tzx s bloky MZ, rychlost bloku v MZT
    emu.call("cmt_open", {"path": str(good), "play_immediately": False})
    res = emu.call("cmt_save", {"path": str(tmp / "mz.tzx")})
    check("TMZ format" in res.get("warning", ""), f"save .tzx with MZ blocks warns ({res})")
    check(emu.call("cmt_tape_list").get("modified") is False, "tape saved to .tzx is not modified")
    emu.call("cmt_tape_block_speed", {"block_id": 0, "speed": 2})
    # skutečná změna rychlosti bloku přes MCP označí pásku jako modified
    check(emu.call("cmt_tape_list").get("modified") is True, "block speed change over MCP marks the tape modified")
    res = emu.call("cmt_save", {"path": str(tmp / "fast.mzt")})
    check(res.get("warning") == "Speed of block 0 cannot be stored in MZT", f"save .mzt with block 0 at 2:1 warns ({res})")

    # export: blok mimo rozsah, nepodporovaná přípona
    resp = emu.call_raw("cmt_export_block", {"block": 5, "path": str(tmp / "b5.mzf")})
    check(not resp.get("success") and not (tmp / "b5.mzf").exists(),
          f"export of a block out of range fails ({resp.get('error')})")
    resp = emu.call_raw("cmt_export_block", {"block": 0, "path": str(tmp / "b0.xyz")})
    check(not resp.get("success") and not (tmp / "b0.xyz").exists(),
          f"export with an unsupported extension fails ({resp.get('error')})")
    emu.call("cmt_transport", {"action": "eject"})


def _rom_save_mzf():
    """MZF fáze E: typ 01h, jméno, 300 B na 1200h, komentář 18h-7Fh se vzorem, deterministické tělo."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"ROM SAVE TEST"
    hdr[1:18] = name + b"\r" + b"\r" * (16 - len(name))
    hdr[0x12:0x14] = _BODY_SIZE.to_bytes(2, "little")
    hdr[0x14:0x16] = _BODY_ADDR.to_bytes(2, "little")
    hdr[0x16:0x18] = _BODY_ADDR.to_bytes(2, "little")
    for i in range(0x18, 0x80):
        hdr[i] = (i * 7 + 3) & 0xFF
    body = bytes(((i * 53 + 7) ^ (i >> 3)) & 0xFF for i in range(_BODY_SIZE))
    return bytes(hdr), body


def phase_rom_save(name, exe, tmp):
    """SAVE z ROM (@WHEAD + @WDATA) do pásky, export MZF, zpět LOAD z ROM (@RHEAD + @RDATA)."""
    print(f"Phase E ({name}): ROM SAVE into the tape, export as MZF, ROM LOAD back")
    hdr, body = _rom_save_mzf()
    work = tmp / f"e_{name}"
    emu = PipeEmu(exe, work / "cfg")
    try:
        emu.call("run", {"frames": 350})
        res = emu.call("cmt_record")
        check(res.get("path") is None, "record into a new tape (no path)")
        emu.poke(_HEAD_ADDR, hdr)
        emu.poke(_BODY_ADDR, body)
        emu.poke(_STUB_ADDR, _SAVE_STUB)
        emu.poke(_SAVE_RESULT, b"\x00\x00")
        emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
        t0 = time.time()
        result, frames = emu.run_until(_SAVE_RESULT, _ROM_FRAMES)
        check(result == 0x55, f"ROM WHEAD + WDATA returned without error (result {result:02X}h, "
                              f"{frames} frames, {time.time() - t0:.1f} s)")
        emu.call("run", {"frames": 10})
        res = emu.call("cmt_transport", {"action": "stop"})
        rec = res.get("recorded") or {}
        print(f"  INFO: recorded {rec}")
        check(rec.get("blocks") == 1 and rec.get("mzf") == 1 and rec.get("raw") == 0,
              f"the ROM SAVE decoded as one MZF block ({rec})")
        tape = emu.call("cmt_tape_list")
        blocks = tape.get("blocks") or []
        check(tape.get("count") == 1 and tape.get("modified") is True and blocks[0].get("type") == 1,
              f"tape: 1 MZF block, modified ({tape.get('count')}, {[b.get('type') for b in blocks]})")

        out = work / "out.mzf"
        emu.call("cmt_export_block", {"block": 0, "path": str(out)})
        data = out.read_bytes()
        check(len(data) == 128 + _BODY_SIZE, f"exported MZF has 128 + {_BODY_SIZE} bytes ({len(data)})")
        check(data[:0x18] == hdr[:0x18],
              f"header fields equal: type, name, size, load and exec address ({data[:0x18].hex()})")
        check(data[128:] == body, "body equals the original")
        same_comment = data[0x18:0x80] == hdr[0x18:0x80]
        print(f"  INFO: comment area 18h-7Fh equal: {same_comment}")
        check(same_comment, "comment area 18h-7Fh equal (whole file identical)")

        # zpět: blok 0 přehrát, ROM ho načte do vynulované paměti
        emu.poke(_BODY_ADDR, bytes(_BODY_SIZE))
        emu.poke(_HEAD_ADDR, bytes(128))
        emu.poke(_STUB_ADDR, _LOAD_STUB)
        emu.poke(_LOAD_RESULT, b"\x00")
        emu.call("cmt_tape_seek", {"block_id": 0})
        emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
        emu.call("cmt_transport", {"action": "play"})
        result, frames = emu.run_until(_LOAD_RESULT, _ROM_FRAMES)
        check(result == 0x55, f"ROM RHEAD + RDATA of the recorded block succeeded (result {result:02X}h, {frames} frames)")
        check(emu.ram(_BODY_ADDR, _BODY_SIZE) == body, "loaded body equals the original")
        check(emu.ram(_HEAD_ADDR, 128) == hdr, "loaded header (10F0h, 128 B) equals the original")

        # Surový blok (úroveň PC1, jak ji ROM zapsala): jen hlavička bez těla
        # se nedekóduje na MZF; přehrání a @RHEAD ji musí přečíst - ověří
        # polaritu přehrání bloku 0x18 ze záznamu skutečnou ROM.
        emu.call("cmt_new_tape")
        emu.call("cmt_record")
        emu.poke(_HEAD_ADDR, hdr)
        emu.poke(_STUB_ADDR, _WHEAD_STUB)
        emu.poke(_SAVE_RESULT, b"\x00\x00")
        emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
        result, frames = emu.run_until(_SAVE_RESULT, _ROM_FRAMES)
        check(result == 0x55, f"ROM WHEAD alone returned without error (result {result:02X}h, {frames} frames)")
        emu.call("run", {"frames": 10})
        rec = emu.call("cmt_transport", {"action": "stop"}).get("recorded") or {}
        print(f"  INFO: header-only recording: {rec}")
        check(rec.get("blocks") == 1 and rec.get("raw") == 1 and rec.get("mzf") == 0,
              f"header without a body is kept as one raw block ({rec})")
        emu.poke(_HEAD_ADDR, bytes(128))
        emu.poke(_STUB_ADDR, _RHEAD_STUB)
        emu.poke(_LOAD_RESULT, b"\x00")
        emu.call("cmt_tape_seek", {"block_id": 0})
        emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
        emu.call("cmt_transport", {"action": "play"})
        result, frames = emu.run_until(_LOAD_RESULT, _ROM_FRAMES)
        check(result == 0x55, f"ROM RHEAD of the raw block succeeded (result {result:02X}h, {frames} frames)")
        check(emu.ram(_HEAD_ADDR, 128) == hdr, "header read from the raw block equals the original")
    finally:
        emu.close()


#: Program přepnutí motoru (jako ROM @MGON): 3000h LD A,06h; LD (0E003h),A;
#: INC A; LD (0E003h),A; JR $ (BSR PC3 = 0, 1 = náběžná hrana).
_MOTOR_ADDR = 0x3000
_MOTOR_PROG = bytes([0x3E, 0x06, 0x32, 0x03, 0xE0, 0x3C, 0x32, 0x03, 0xE0, 0x18, 0xFE])
#: Totéž přepnutí motoru, pak skok na program hran (_PROG na 2000h):
#: ... INC A; LD (0E003h),A; JP 2000h.
_MOTOR_THEN_PROG = bytes([0x3E, 0x06, 0x32, 0x03, 0xE0, 0x3C, 0x32, 0x03, 0xE0, 0xC3, 0x00, 0x20])
#: Začátek varování MCP se zapnutým CMT hackem (DBGAPI_CMT_WARNING_DIRECT).
_DIRECT_WARNING = "Direct loading (CMT hack) is on"


def phase_rom_save_direct(name, exe, tmp):
    """SAVE z ROM se zapnutým CMT hackem, LOAD zpět přes patch, přehrávání s hackem beze změny."""
    print(f"Phase G ({name}): ROM SAVE with Direct loading on (fresh configuration)")
    hdr, body = _rom_save_mzf()
    work = tmp / f"g_{name}"
    # výchozí nová konfigurace: hack i Direct Save zapnuté; s Record se
    # signál nahrává i rutinami ROM (patch zápisu v RECORD v ROM není)
    emu = PipeEmu(exe, work / "cfg", hack=True)
    try:
        emu.call("run", {"frames": 350})
        st = emu.call("get_periph_cmt")
        check(st.get("cmthack_enabled") is True and st.get("direct_save") is True
              and st.get("direct_save_enabled") is True,
              f"the CMT hack and Direct Save on (fresh configuration) ({st.get('cmthack_enabled')}, "
              f"{st.get('direct_save')}, {st.get('direct_save_enabled')})")
        res = emu.call("cmt_record")
        check("error" not in res and res.get("path") is None and "warning" not in res,
              f"emu_cmt_record with Direct on returns no warning ({res})")
        st = emu.call("get_periph_cmt")
        check(st.get("state") == "record" and st.get("paused") is True,
              f"recording waits for the motor ({st.get('state')}, paused {st.get('paused')})")
        check(st.get("direct_save") is False and st.get("direct_save_enabled") is True,
              f"while recording the Direct Save patch is not in ROM ({st.get('direct_save')}, "
              f"{st.get('direct_save_enabled')})")
        emu.poke(_HEAD_ADDR, hdr)
        emu.poke(_BODY_ADDR, body)
        emu.poke(_STUB_ADDR, _SAVE_STUB)
        emu.poke(_SAVE_RESULT, b"\x00\x00")
        emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
        t0 = time.time()
        result, frames = emu.run_until(_SAVE_RESULT, _ROM_FRAMES)
        check(result == 0x55, f"ROM WHEAD + WDATA returned without error (result {result:02X}h, "
                              f"{frames} frames, {time.time() - t0:.1f} s)")
        emu.call("run", {"frames": 10})
        st = emu.call("get_periph_cmt")
        check(st.get("cmthack_enabled") is True, "the CMT hack stayed on during the SAVE")
        res = emu.call("cmt_transport", {"action": "stop"})
        rec = res.get("recorded") or {}
        print(f"  INFO: recorded {rec}")
        check(rec.get("blocks") == 1 and rec.get("mzf") == 1 and rec.get("raw") == 0,
              f"the ROM SAVE with Direct on decoded as one MZF block ({rec})")
        out = work / "out.mzf"
        emu.call("cmt_export_block", {"block": 0, "path": str(out)})
        data = out.read_bytes()
        check(data == hdr + body, f"exported MZF is byte-identical to memory ({len(data)} bytes)")

        # zpět: LOAD z ROM přes patch hacku (deck ve STOP na bloku 0)
        emu.poke(_BODY_ADDR, bytes(_BODY_SIZE))
        emu.poke(_HEAD_ADDR, bytes(128))
        emu.poke(_STUB_ADDR, _LOAD_STUB)
        emu.poke(_LOAD_RESULT, b"\x00")
        emu.call("cmt_tape_seek", {"block_id": 0})
        emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
        result, frames = emu.run_until(_LOAD_RESULT, 200, step=10)
        check(result == 0x55, f"ROM RHEAD + RDATA through the CMT hack read the recorded block "
                              f"(result {result:02X}h, {frames} frames)")
        check(emu.ram(_BODY_ADDR, _BODY_SIZE) == body, "loaded body equals the original")

        # přehrávání s hackem beze změny: hrana PC3 pásku nerozjede
        emu.call("cmt_transport", {"action": "stop"})
        emu.call("cmt_tape_seek", {"block_id": 0})
        res = emu.call("cmt_transport", {"action": "play_paused"})
        check(_DIRECT_WARNING in (res.get("warning") or ""),
              f"emu_cmt_play_paused with Direct on still warns ({res.get('warning')})")
        emu.poke(_MOTOR_ADDR, _MOTOR_PROG)
        emu.call("set_register", {"reg": "PC", "value": _MOTOR_ADDR})
        emu.call("run", {"frames": 5})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == "play" and st.get("paused") is True,
              f"with Direct on the motor command does not start playback ({st.get('state')}, "
              f"paused {st.get('paused')})")
        emu.call("cmt_transport", {"action": "stop"})

        # vlastní ukládací rutina programu (ne ROM): motor programem, pak hrany PC1
        emu.call("cmt_new_tape")
        emu.call("cmt_record")
        # PC4 v debuggeru jako ho čte program: 0 (nahrávání čeká), potom 1;
        # podvržený bit hacku se při nahrávání nemění, jedna z kontrol ho tedy odliší
        ppi = emu.call("get_periph_i8255")
        check(ppi.get("signal_pc04") == 0,
              f"the debugger view of PC4 shows what the program reads (waiting: 0) ({ppi.get('signal_pc04')})")
        emu.poke(_PROG_ADDR, _PROG)
        emu.poke(_MOTOR_ADDR, _MOTOR_THEN_PROG)
        emu.call("set_register", {"reg": "PC", "value": _MOTOR_ADDR})
        emu.call("run", {"frames": 20})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == "record" and st.get("paused") is False,
              f"a program's own motor command starts the recording with Direct on (paused {st.get('paused')})")
        ppi = emu.call("get_periph_i8255")
        check(ppi.get("signal_pc04") == 1,
              f"the debugger view of PC4 shows what the program reads (deck running: 1) ({ppi.get('signal_pc04')})")
        rec = emu.call("cmt_transport", {"action": "stop"}).get("recorded") or {}
        check(rec.get("blocks") == 1 and rec.get("raw") == 1,
              f"a program's own save routine records with Direct on ({rec})")

        # totéž SAVE z ROM s Direct Save vypnutým (settings_set ve STOP)
        emu.call("settings_set", {"key": "CMTHACK/direct_save", "value": "false"})
        st = emu.call("get_periph_cmt")
        check(st.get("direct_save") is False and st.get("direct_save_enabled") is False,
              f"Direct Save switched off ({st.get('direct_save')}, {st.get('direct_save_enabled')})")
        emu.call("cmt_new_tape")
        emu.call("cmt_record")
        emu.poke(_HEAD_ADDR, hdr)
        emu.poke(_BODY_ADDR, body)
        emu.poke(_STUB_ADDR, _SAVE_STUB)
        emu.poke(_SAVE_RESULT, b"\x00\x00")
        emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
        result, frames = emu.run_until(_SAVE_RESULT, _ROM_FRAMES)
        check(result == 0x55, f"ROM SAVE with Direct Save off returned without error (result {result:02X}h)")
        emu.call("run", {"frames": 10})
        rec = emu.call("cmt_transport", {"action": "stop"}).get("recorded") or {}
        check(rec.get("blocks") == 1 and rec.get("mzf") == 1,
              f"the ROM SAVE with Direct Save off decoded as one MZF block ({rec})")
    finally:
        emu.close()


def main_rom_save(name, direct=False):
    """Jen fáze E (SAVE z ROM), nebo s ``direct`` fáze G, pro jednu binárku: vlastní ctest na binárku.

    Chybějící binárka je chyba (exit 1), ne SKIP: sestavují se všechny čtyři.
    """
    emu_test_proc.install(deadline_s=170)
    if name not in _ROM_SAVE_MACHINES:
        print(f"FAIL: unknown binary {name} (allowed: {', '.join(_ROM_SAVE_MACHINES)})")
        return 1
    exe = _find_exe(name)
    if exe is None:
        print(f"FAIL: missing binary {name}")
        return 1
    with tempfile.TemporaryDirectory(prefix="mz_cmt_rom_save_") as td:
        try:
            if direct:
                phase_rom_save_direct(name, exe, Path(td))
            else:
                phase_rom_save(name, exe, Path(td))
        except TestFailure as exc:
            print(f"  FAIL: {exc}")
            print("FAILED")
            return 1
    print("ALL PASS")
    return 0


def phase_f(exe, tmp):
    """last_recording v periph/cmt/tape a emu_cmt_save při záznamu a přehrávání vstupů."""
    print("Phase F: last_recording (auto-stop, discard, new tape), cmt_save refused in replay")
    emu = PipeEmu(exe, tmp / "cfg_f", "[CMT]\nrec_autostop_s = 0x01\n")
    try:
        emu.call("run", {"frames": 350})
        emu.call("cmt_new_tape")
        tape = emu.call("cmt_tape_list")
        check("last_recording" in tape and tape.get("last_recording") is None,
              f"no last_recording after the start ({tape.get('last_recording')})")

        # nahrávání skončí samo: program po hranách stojí (JR $), 1 s ticha
        emu.call("cmt_record")
        _record_edges(emu)
        emu.call("run", {"frames": 150}, timeout=120.0)
        check(emu.call("get_periph_cmt").get("state") == "stop", "recording stopped by itself (rec_autostop_s)")
        rec = emu.call("cmt_tape_list").get("last_recording") or {}
        check(rec.get("blocks") == 1 and rec.get("raw") == 1 and rec.get("mzf") == 0
              and rec.get("discarded") is False and "automatically" in rec.get("message", ""),
              f"last_recording of the auto-stopped recording ({rec})")
        serial = rec.get("serial")

        # zahozené nahrávání (vysunutí): výsledek i bez pásky
        emu.call("cmt_record")
        _record_edges(emu, frames=4)
        emu.call("cmt_transport", {"action": "eject"})
        tape = emu.call("cmt_tape_list")
        rec = tape.get("last_recording") or {}
        check(tape.get("available") is False and rec.get("discarded") is True
              and rec.get("serial") == serial + 1 and "discarded" in rec.get("message", ""),
              f"last_recording of a discarded recording without a tape ({tape})")
        # otevření jiné pásky během nahrávání: zahozený výsledek zůstane čitelný
        emu.call("cmt_new_tape")
        emu.call("cmt_record")
        _record_edges(emu, frames=4)
        good = _REPO_ROOT / "tests" / "data" / "cmt" / "prog1.mzf"
        emu.call("cmt_open", {"path": str(good), "play_immediately": False})
        rec = emu.call("cmt_tape_list").get("last_recording") or {}
        check(rec.get("discarded") is True and rec.get("serial") == serial + 2
              and "another tape was opened" in rec.get("message", ""),
              f"last_recording survives the open that discarded it ({rec})")

        # emu_cmt_save při záznamu a při přehrávání vstupů
        _modified_tape(emu)
        mzr = tmp / "f.mzr"
        target = tmp / "f_saved.tmz"
        emu.call("replay_record_start", {"path": str(mzr), "hash_interval": 10})
        resp = emu.call_raw("cmt_save", {"path": str(target)})
        check(not resp.get("success") and not target.exists(),
              f"cmt_save refused while an input replay is recorded ({resp.get('error')})")
        emu.call("run", {"frames": 5})
        st = emu.call("replay_record_stop")
        check(st.get("outcome") == "recorded", f"input replay recorded ({st.get('outcome_message')})")
        st = emu.call("replay_play", {"path": str(mzr), "pause_after_load": True})
        check(st.get("mode") == "playing", f"input replay playing ({st})")
        resp = emu.call_raw("cmt_save", {"path": str(target)})
        check(not resp.get("success") and not target.exists(),
              f"cmt_save refused while an input replay is played ({resp.get('error')})")
        rec_wav = tmp / "f_rec.wav"
        resp = emu.call_raw("cmt_record", {"path": str(rec_wav)})
        check(not resp.get("success") and not rec_wav.exists()
              and "while a replay is played" in (resp.get("error") or ""),
              f"cmt_record with a path refused while an input replay is played ({resp.get('error')})")
        exported = tmp / "f_export.wav"
        res = emu.call("cmt_export_block", {"block": 0, "path": str(exported)})
        check(exported.is_file(), f"cmt_export_block allowed while an input replay is played ({res.get('path')})")
        res = emu.call("cmt_record")
        check(res.get("path") is None and emu.call("get_periph_cmt").get("state") == "record",
              "cmt_record without a path allowed while an input replay is played")
        emu.call("cmt_transport", {"action": "stop"})
        emu.call("replay_play_stop")
    finally:
        emu.close()


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--rom-save":
        return main_rom_save(sys.argv[2])
    if len(sys.argv) == 3 and sys.argv[1] == "--rom-save-direct":
        return main_rom_save(sys.argv[2], direct=True)
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe("mz800emu")
    if exe is None:
        print("SKIP: mz800emu binary not found")
        return 77
    failures = []
    with tempfile.TemporaryDirectory(prefix="mz_cmt_record_") as td:
        tmp = Path(td)
        emu = PipeEmu(exe, tmp / "cfg")
        try:
            emu.call("run", {"frames": 350})
            for label, fn in (("A+B", phase_a_b), ("C", phase_c), ("D", phase_d)):
                try:
                    fn(emu, tmp)
                except TestFailure as exc:
                    failures.append(f"phase {label}: {exc}")
                    print(f"  FAIL: {exc}")
        finally:
            emu.close()
        try:
            phase_f(exe, tmp)
        except TestFailure as exc:
            failures.append(f"phase F: {exc}")
            print(f"  FAIL: {exc}")
    if failures:
        print("FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
