#!/usr/bin/env python3
"""Regresní test tempa emulace v headless režimu (MCP pipe, bez audio zařízení).

Headless režim neotevírá audio zařízení, takže tempo emulace při 100 % nemůže
udávat audio callback - musí ho udávat systémové hodiny (iface_audio_20ms_sync,
sync_by_timer). Dřívější vada: po prvním odpauzování čekala synchronizace na
neexistující audio callback cca 1 s na každý snímek (emulace běžela asi
1 snímek/s a ``run frames`` končil safety timeoutem), a před prvním
odpauzováním naopak běžela bez brzdy rychlostí MAX SPEED.

Spustí mz800emu(.exe) s ``--mcp-pipe`` v izolovaném prostředí (vlastní
dočasný ``--cfg-dir``, INI s vypnutým auto load/save breakpointů,
``--no-save-ini``) bez programu a ověří:

  1. volný běh hned po startu (100 %) jde reálným časem, cca 50 snímků/s
     (počítadlo ``total_screens`` z ``get_periph_gdg`` za cca 3,5 s; práh
     25..75 snímků/s - široký kvůli sdíleným CI strojům, ale pořád odliší
     obě dřívější vady: cca 1 snímek/s i neomezený běh MAX SPEED, který dává
     stovky snímků/s);
  1b. ``run frames=N`` spuštěný za běhu emulace hlásí ``actual_frames`` == N
     (a ze zastaveného stavu navíc posune čítač snímků přesně o N);
  2. ``run frames=250`` při 100 % doběhne celý (``stopped_by: frames``)
     za 4..10 s (reálný čas 250 / 50 = 5 s);
  3. ``set_speed custom 200 %`` + ``run frames=200`` trvá cca 2 s (1,5..4 s);
  4. ``set_speed max`` + ``run frames=500`` není brzděný (< 4 s);
  5. návrat na ``normal`` po MAX SPEED: ``run frames=100`` cca 2 s
     (1,5..4 s) - řada časových termínů se po MAX SPEED založí znovu
     a nedohání zameškaný čas.

Spouštění: ctest ``mcp_headless_pacing`` (WORKING_DIRECTORY = kořen repa),
nebo ručně ``python tests/mcp/test_headless_pacing.py`` (binárka z kořene
repa nebo proměnná ``MZ_EMU``). Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
import queue
import shutil
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
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def _find_exe():
    """Najde binárku mz800emu (přednost má proměnná MZ_EMU)."""
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env)
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    sys.exit(1)


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, ini):
        args = [str(exe), "--mcp-pipe", "--no-save-ini", "--no-first-run-windows",
                f"--cfg-dir={cfg_dir}", f"--work-dir={cfg_dir}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
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
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                continue  # případný ne-JSON výpis na stdout přeskočíme
            if pred(msg):
                return msg
        return None

    def call(self, cmd, data=None, timeout=60.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to {cmd}")
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def screens(self):
        """Počítadlo emulovaných snímků (g_gdg.total_elapsed.screens)."""
        return int(self.call("get_periph_gdg")["total_screens"])

    def timed_run(self, frames):
        """``run frames=N``; vrátí (data response, doba v sekundách)."""
        t = time.monotonic()
        d = self.call("run", {"frames": frames}, timeout=120.0)
        return d, time.monotonic() - t

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


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def check_run(emu, frames, lo, hi, label):
    """Spustí ``run frames`` a ověří úplný doběh v časovém okně lo..hi s."""
    d, dt = emu.timed_run(frames)
    check(d.get("stopped_by") == "frames" and d.get("actual_frames") == frames,
          f"{label}: run {frames} frames complete ({d.get('stopped_by')}, "
          f"{d.get('actual_frames')} frames)")
    check(lo <= dt <= hi, f"{label}: run {frames} frames took {dt:.2f} s "
          f"(expected {lo}..{hi} s)")


def run(emu):
    """Vlastní scénář; vyhodí TestFailure při první chybě."""
    # 1. Volný běh po startu: reálný čas, ne MAX SPEED ani 1 snímek/s.
    emu.call("set_speed", {"mode": "normal"})
    # Delší okno a široký práh: odolné proti krátkému zdržení na sdíleném CI
    # stroji, ale vady (cca 1 fps / MAX SPEED se stovkami fps) odliší spolehlivě.
    a = emu.screens()
    t = time.monotonic()
    time.sleep(3.5)
    b = emu.screens()
    fps = (b - a) / (time.monotonic() - t)
    check(25.0 <= fps <= 75.0, f"free run at 100 % paces at real time ({fps:.1f} fps, expected ~50, "
          f"accepted 25..75)")

    # 1b. run frames=N spuštěný ZA BĚHU emulace: actual_frames musí být
    #     přesně N. Regrese: po přesunu drainu dbgapi za uzavření snímku
    #     (09103e29) se výchozí čítač snímků četl na dispatch vlákně před
    #     zpracováním příkazu, takže zahrnul i snímek ukončený mezi čtením
    #     a drainem a hlásil N+1. Run sám emulaci po doběhu pozastaví.
    for n in (50, 1):
        d = emu.call("run", {"frames": n}, timeout=120.0)
        check(d.get("stopped_by") == "frames" and d.get("actual_frames") == n,
              f"run {n} frames started while running reports {n} frames "
              f"({d.get('stopped_by')}, {d.get('actual_frames')} frames)")
        emu.call("run")  # znovu volný běh pro další průchod

    emu.call("pause")
    # 1c. Ze zastaveného stavu: actual_frames i skutečný posun čítače = N.
    a = emu.screens()
    d = emu.call("run", {"frames": 7}, timeout=120.0)
    b = emu.screens()
    check(d.get("actual_frames") == 7 and b - a == 7,
          f"run 7 frames from pause reports 7 and advances 7 frames "
          f"(reported {d.get('actual_frames')}, advanced {b - a})")
    # 2. Frame-bounded běh při 100 %.
    check_run(emu, 250, 4.0, 10.0, "100 %")
    # 3. Vlastní rychlost 200 %: dvojnásobek snímků za reálný čas.
    emu.call("set_speed", {"mode": "custom", "percent": 200})
    check_run(emu, 200, 1.5, 4.0, "200 %")
    # 4. MAX SPEED: bez brzdy.
    emu.call("set_speed", {"mode": "max"})
    check_run(emu, 500, 0.0, 4.0, "MAX SPEED")
    # 5. Zpět na 100 %: bez dohánění času zameškaného v MAX SPEED.
    emu.call("set_speed", {"mode": "normal"})
    check_run(emu, 100, 1.5, 4.0, "100 % after MAX SPEED")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    tmp = Path(tempfile.mkdtemp(prefix="mz_headless_pacing_"))
    ini = tmp / "mz800emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load=0\nauto_save=0\n", encoding="utf-8")
    print(f"Headless pacing test ({exe})")
    emu = None
    rc = 0
    try:
        emu = PipeEmu(exe, tmp, ini)
        run(emu)
        print("RESULT: PASS")
    except TestFailure as e:
        print(f"RESULT: FAIL - {e}")
        rc = 1
    finally:
        if emu is not None:
            emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
