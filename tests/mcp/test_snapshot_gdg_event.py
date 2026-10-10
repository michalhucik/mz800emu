#!/usr/bin/env python3
"""E2E regresní test: snapshot obnoví i čekající GDG událost (MCP pipe).

Chyba (GitHub issue #6): načtení snapshotu obnovilo pozici paprsku GDG
(``beam_row``, ``total_elapsed``), ale čekající GDG událost ``g_gdg.event``
zůstala ze stroje, který se přepisoval. Na MZ-800 v grafickém režimu se pak
událost ``AFTER_LAST_SCREEN_PIXEL`` zpracovala v horním borderu, index do
VRAM přetekl a proces spadl (0xC0000005). Na všech platformách se navíc
přeskočily nebo posunuly události řádku (HBLN, VSYNC, hrana CTC1).

Pro každou binárku v kořeni repa a pro několik řádků uložení:

  1  program v RAM (3000h JP 3000h, od 3100h NOP), DI, MAX SPEED; na MZ-800
     DMD = 00h (320x200, 4 barvy) - grafický režim, ve kterém chyba padala;
  2  ``run_until_raster`` na řádek uložení, ``snapshot_save``;
  3  reference A: z uloženého stavu ``run`` 2 snímky, zapsat stav stroje;
  4  "špinavý" stroj: doběhnout na řádek 103 do fáze, ve které čeká
     zvolená GDG událost (pro každou událost řádku 103 jedna fáze, okna
     sloupců podle g_gdgevent[] a video konstant platformy); okno
     AFTER_LAST_SCREEN_PIXEL je široké jen 4 sloupce, dosahuje se
     krokováním NOP a jen na MZ-800 (scénář z issue);
  5  B: ``snapshot_load``, ``run`` 2 snímky, zapsat stav;
  6  A a B se musí shodovat (registry, raster, GDG, 8253, Z80 PIO) a proces
     nesmí spadnout.

Dřív B na MZ-800 spadl a na ostatních platformách se od A lišil.

Binárky se hledají v kořeni repa, nebo v adresáři z proměnné ``MZ_EMU_DIR``
(např. pro ověření, že test na starém buildu selže). Každý proces běží
izolovaně (dočasný ``--cfg-dir``/``--work-dir``, prázdné INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import json
import os
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

#: Okna sloupců řádku 103, ve kterých čeká daná GDG událost: od sloupce
#: předchozí události (už zpracovaná) do sloupce dané události - 1.
#: Sloupce událostí podle g_gdgevent[] (stejná tabulka na všech platformách)
#: a video hlaviček:
#:   MZ-800 (mz800_video.h): HBLN_END 150, HBLN_START 790,
#:     AFTER_LAST_SCREEN_PIXEL 794, AFTER_LAST_VISIBLE_PIXEL 928,
#:     REAL_HSYNC_START 950, SCREEN_ROW_END 1136;
#:   MZ-700 PAL (mz700_video_pal.h): 28, 668, 672, 704, 920, 1136;
#:   MZ-700 NTSC a MZ-1500 (mz700_video_ntsc.h, mz1500_video.h):
#:     28, 668, 672, 704, 743, 912.
def _windows(hbln_end, hbln_start, alsp, alvp, rhs, width):
    """Okna run_until_raster (bez úzkého okna AFTER_LAST_SCREEN_PIXEL)."""
    return [
        ("HBLN_END", range(0, hbln_end)),
        ("HBLN_START", range(hbln_end, hbln_start)),
        ("AFTER_LAST_VISIBLE_PIXEL", range(alsp, alvp)),
        ("REAL_HSYNC_START", range(alvp, rhs)),
        ("SCREEN_ROW_END", range(rhs, width)),
    ]


#: (jméno binárky, VIDEO_SCREEN_HEIGHT, okna fází, je MZ-800)
#: Výšky podle mz800_video.h (312), mz700_video_pal.h (312),
#: mz700_video_ntsc.h a mz1500_video.h (16 + 200 + 16 + 30 = 262).
_PLATFORMS = [
    ("mz700emu-pal", 312, _windows(28, 668, 672, 704, 920, 1136), False),
    ("mz700emu-ntsc", 262, _windows(28, 668, 672, 704, 743, 912), False),
    ("mz800emu", 312, _windows(150, 790, 794, 928, 950, 1136), True),
    ("mz1500emu", 262, _windows(28, 668, 672, 704, 743, 912), False),
]

#: Řádky, na kterých se snapshot ukládá: první řádek, hranice canvasu
#: (VIDEO_BEAM_CANVAS_FIRST_ROW = 46 a VIDEO_BEAM_CANVAS_LAST_ROW = 245 na
#: MZ-800) a poslední řádek snímku (doplní se podle výšky).
_SAVE_ROWS = [0, 45, 46, 150, 245, 246]

#: Sloupec, na kterém run_until_raster zastaví při ukládání.
_SAVE_COL = 200

#: Řádek "špinavého" stroje (viditelný řádek canvasu MZ-800).
_DIRTY_ROW = 103

#: Sloupce 790-793 na MZ-800: čeká AFTER_LAST_SCREEN_PIXEL
#: (HBLN_START na 790 už proběhl, AFTER_LAST_SCREEN_PIXEL je na 794).
_ALSP_COLS = range(790, 794)

#: Kolik snímků běží A i B po uložení / načtení.
_RUN_FRAMES = 2

#: Program: 3000h JP 3000h; od 3100h NOP (krok po 4 T jemně posouvá fázi rastru).
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


def _raster(emu):
    """Pozice paprsku bez čítačů cyklů (ty snapshot neobnovuje)."""
    r = emu.call("get_raster_pos")
    return {k: r[k] for k in ("frame_number", "scanline", "column_pixel")}


def _machine_state(emu):
    """Stav stroje pro porovnání A a B."""
    return {
        "registers": emu.call("get_registers"),
        "raster": _raster(emu),
        "gdg": emu.call("get_periph_gdg"),
        "i8253": emu.call("get_periph_i8253"),
        "z80_pio": emu.call("get_periph_z80_pio"),
    }


def _diff(a, b, path=""):
    """Seznam cest, kde se dva JSON stavy liší (pro čitelný FAIL)."""
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for k in sorted(set(a) | set(b)):
            out += _diff(a.get(k), b.get(k), f"{path}.{k}")
        return out
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        out = []
        for i, (x, y) in enumerate(zip(a, b)):
            out += _diff(x, y, f"{path}[{i}]")
        return out
    return [] if a == b else [f"{path}: {a!r} != {b!r}"]


def _setup(emu, is_mz800):
    """Program v RAM, DI, MAX SPEED; na MZ-800 grafický režim DMD = 00h."""
    emu.call("pause")
    emu.call("io_write", {"port": 0xE0, "value": 0})
    emu.call("io_write", {"port": 0xE1, "value": 0})
    if is_mz800:
        emu.call("io_write", {"port": 0xCE, "value": 0x00})
    emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
    emu.call("set_speed", {"mode": "max"})
    emu.call("mem_write", {"addr": _LOOP, "data_hex": "c3%02x%02x" % (_LOOP & 0xFF, _LOOP >> 8)})
    emu.call("mem_write", {"addr": _NOPS, "data_hex": "00" * 8192})
    emu.call("set_register", {"reg": "PC", "value": _LOOP})


def _dirty_window(emu, cols):
    """Špinavý stroj na řádku 103 se sloupcem v okně cols (NOP, krok <= 20)."""
    emu.call("set_register", {"reg": "PC", "value": _NOPS})
    emu.call("run_until_raster", {"line": _DIRTY_ROW, "col": cols.start, "max_cycles": 200000})
    r = _raster(emu)
    if r["scanline"] != _DIRTY_ROW or r["column_pixel"] not in cols:
        raise TestFailure(f"dirty phase {cols.start}-{cols.stop - 1} not reached "
                          f"({r['scanline']}/{r['column_pixel']})")
    return r


def _dirty_alsp(emu):
    """Špinavý stroj na viditelném řádku s čekající AFTER_LAST_SCREEN_PIXEL (issue #6)."""
    emu.call("run_until_raster", {"line": _DIRTY_ROW, "col": 750, "max_cycles": 200000})
    emu.call("set_register", {"reg": "PC", "value": _NOPS})
    for _ in range(400):
        r = _raster(emu)
        if 46 <= r["scanline"] <= 245 and r["column_pixel"] in _ALSP_COLS:
            return r
        emu.call("step_into")
    raise TestFailure("could not reach raster phase with pending AFTER_LAST_SCREEN_PIXEL")


def _case(emu, tmp, row, dirty_phases):
    """Jeden řádek uložení: A (bez loadu) vs. B (load přes špinavý stroj)."""
    emu.call("set_register", {"reg": "PC", "value": _LOOP})
    emu.call("run_until_raster", {"line": row, "col": _SAVE_COL, "max_cycles": 200000})
    saved = _raster(emu)
    snap = tmp / f"row{row}.mzs"
    emu.call("snapshot_save", {"path": str(snap)})

    emu.call("run", {"frames": _RUN_FRAMES})
    state_a = _machine_state(emu)

    for name, dirty in dirty_phases:
        at = dirty()
        emu.call("snapshot_load", {"path": str(snap)})
        check(_raster(emu) == saved,
              f"row {row}, pending {name}: load restores raster {saved['scanline']}/{saved['column_pixel']} "
              f"(dirty machine at {at['scanline']}/{at['column_pixel']})")
        emu.call("run", {"frames": _RUN_FRAMES})
        diff = _diff(state_a, _machine_state(emu))
        check(not diff, f"row {row}, pending {name}: state after load + run equals run without load"
              + ("" if not diff else " - differs in: " + "; ".join(diff[:6])))


def run_platform(exe, height, windows, is_mz800):
    """Všechny řádky uložení pro jednu binárku. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="snap_gdg_event_"))
    emu = None
    try:
        emu = PipeEmu(exe, tmp)
        _setup(emu, is_mz800)
        phases = [(name, lambda c=cols: _dirty_window(emu, c)) for name, cols in windows]
        if is_mz800:
            phases.append(("AFTER_LAST_SCREEN_PIXEL", lambda: _dirty_alsp(emu)))
        for row in _SAVE_ROWS + [height - 1]:
            _case(emu, tmp, row, phases)
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
    found = [(exe, h, c, m) for name, h, c, m in _PLATFORMS
             if (exe := _find_exe(name)) is not None]
    if not found:
        print("SKIP: no emulator binary found")
        return 77
    ok = all([run_platform(*f) for f in found])
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
