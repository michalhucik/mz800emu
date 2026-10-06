#!/usr/bin/env python3
"""Regresní test GUI režimu bez audio zařízení (MCP TCP, viditelné okno).

Dřívější vada: když se v GUI nepodařilo inicializovat SDL audio nebo otevřít
výstupní zařízení, iface_audio_lowlevel_init() vrátila false, iface_init()
selhala a emulátor skončil s "Failed to initialize interface" - emulace se
vůbec nespustila. Oprava přepne na tempo podle systémových hodin
(``sync_by_timer``, stejná cesta jako headless) a emulace běží bez zvuku.

Selhání audia se simuluje proměnnou prostředí ``SDL_AUDIO_DRIVER`` s
neexistujícím driverem: SDL_InitSubSystem(SDL_INIT_AUDIO) pak selže s
"Audio target '...' not available" (ověřeno na Windows, SDL3). Větev
"subsystém běží, ale SDL_OpenAudioDeviceStream selže" vede do stejné
náhradní funkce (sdl3_audio_fallback_no_device), samostatně se tu nesimuluje.

Spustí mz800emu(.exe) v GUI režimu (otevře okno!) s ``--mcp-tcp-port``
v izolovaném prostředí (vlastní dočasný ``--cfg-dir``, INI, ``--no-save-ini``)
a ověří:

  1. emulátor nastartuje (MCP TCP hello) a v logu je varování
     "running without sound";
  2. volný běh při 100 % jde reálným časem (25..75 snímků/s, očekáváno 50);
  3. ``run frames=250`` při 100 % doběhne celý za 4..10 s;
  4. ``set_speed max`` + ``run frames=500`` není brzděný (< 4 s);
  5. návrat na 100 %: ``run frames=100`` za 1,5..4 s;
  6. (s kopií Bloxorz DSK a ffmpeg) video záznam podle reality má zvuk
     vyrobený bez zařízení (nenulový, 960 vzorků na snímek) - stejná cesta
     jako headless (videorec_rt_audio_wants_output při sync_by_timer).

Protože otevírá okno, běží jen na vyžádání: proměnná ``MZ_GUI_TESTS=1``,
jinak skončí kódem 77 (ctest SKIP). Spouštění: ``MZ_GUI_TESTS=1 ctest -R
mcp_gui_no_audio`` nebo ručně ``MZ_GUI_TESTS=1 python
tests/mcp/test_gui_no_audio.py``. Port ``MZ_GUI_TEST_PORT`` (bez ní volný
port od systému, nikdy 23800), binárka ``MZ_EMU``, DSK ``MZ_BLOXORZ_DSK``. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů, volné TCP porty

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]
#: Michalova hra (originál se nikdy nemění - test pracuje s read-only kopií).
_BLOXORZ_DSK = Path("C:/msys64/home/Michal/projects/bloxorz-mz800/build/bloxorz.dsk")
#: Neexistující SDL audio driver -> selhání SDL audio subsystému.
_BAD_AUDIO_DRIVER = "mz_no_such_audio_driver"


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def _find_exe():
    """Najde binárku mz800emu (přednost má proměnná MZ_EMU)."""
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env).resolve()
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    sys.exit(1)


def _find_ffmpeg():
    """Vrátí cestu k ffmpeg, nebo None (pak se zvuk záznamu nekontroluje)."""
    p = shutil.which("ffmpeg")
    if p:
        return p
    cand = Path("C:/msys64/ucrt64/bin/ffmpeg.exe")
    return str(cand) if cand.is_file() else None


class TcpGuiEmu:
    """GUI emulátor s MCP TCP serverem: JSONL request/response přes socket."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, ini, port, log_path):
        # port None = volný port přidělený systémem (souběžné běhy testů).
        port = port or emu_test_proc.free_tcp_port()
        args = [str(exe), "--no-save-ini", "--no-first-run-windows",
                f"--cfg-dir={cfg_dir}", f"--work-dir={cfg_dir}", f"--config={ini}",
                f"--mcp-tcp-port={port}"]
        env = dict(os.environ)
        env["SDL_AUDIO_DRIVER"] = _BAD_AUDIO_DRIVER
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=self.log,
                                     stderr=subprocess.STDOUT, env=env, cwd=str(_REPO_ROOT))
        self.sock = None
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise TestFailure(f"emulator exited during startup (exit code {self.proc.returncode})")
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
                break
            except OSError:
                time.sleep(0.2)
        if self.sock is None:
            raise TestFailure(f"cannot connect to MCP TCP port {port}")
        self.sock.settimeout(120.0)
        self.f = self.sock.makefile("rw", buffering=1, encoding="utf-8", newline="\n")
        hello = self._read(lambda m: m.get("type") == "hello")
        if hello is None:
            raise TestFailure("no hello from emulator")
        self.rid = 0

    def _read(self, pred):
        while True:
            line = self.f.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if pred(msg):
                return msg

    def call(self, cmd, data=None):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        self.rid += 1
        rid = self.rid
        self.f.write(json.dumps({"type": "request", "req_id": rid, "cmd": cmd,
                                 "data": data or {}}) + "\n")
        self.f.flush()
        try:
            resp = self._read(lambda m: m.get("type") == "response" and m.get("req_id") == rid)
        except socket.timeout:
            resp = None
        if resp is None:
            raise TestFailure(f"no response to {cmd}")
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def screens(self):
        """Počítadlo emulovaných snímků (g_gdg.total_elapsed.screens)."""
        return int(self.call("get_periph_gdg")["total_screens"])

    def close(self):
        """Ukončí emulátor: MCP shutdown, pak zabije jen vlastní PID.

        V GUI režimu MCP TCP ``shutdown`` jen potvrdí (shutdown callback
        registruje pouze pipe transport), proces neukončí - proto krátké
        čekání a pak kill vlastního PID.
        """
        try:
            self.call("shutdown")
        except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
            pass
        try:
            self.proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()
        if self.sock is not None:
            self.sock.close()
        self.log.close()


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def check_run(emu, frames, lo, hi, label):
    """Spustí ``run frames`` a ověří úplný doběh v časovém okně lo..hi s."""
    t = time.monotonic()
    d = emu.call("run", {"frames": frames})
    dt = time.monotonic() - t
    check(d.get("stopped_by") == "frames" and d.get("actual_frames") == frames,
          f"{label}: run {frames} frames complete ({d.get('stopped_by')}, "
          f"{d.get('actual_frames')} frames)")
    check(lo <= dt <= hi, f"{label}: run {frames} frames took {dt:.2f} s "
          f"(expected {lo}..{hi} s)")


def _avi_audio_peak(ffmpeg, avi):
    """Největší |vzorek| zvuku AVI a počet stereo vzorků (s16le přes ffmpeg)."""
    out = subprocess.run([ffmpeg, "-v", "error", "-i", str(avi), "-map", "0:a", "-f", "s16le", "-"],
                         capture_output=True, timeout=120)
    if out.returncode != 0:
        raise TestFailure(f"ffmpeg cannot decode audio of {avi}")
    n = len(out.stdout) // 2
    samples = struct.unpack(f"<{n}h", out.stdout[:2 * n])
    return max((abs(v) for v in samples), default=0), n // 2


def run_pacing(emu, log_path):
    """Start bez zařízení, varování v logu a tempo emulace."""
    check(emu.proc.poll() is None, "emulator started without an audio device")
    emu.call("set_speed", {"mode": "normal"})
    a = emu.screens()
    t = time.monotonic()
    time.sleep(3.5)
    b = emu.screens()
    fps = (b - a) / (time.monotonic() - t)
    check(25.0 <= fps <= 75.0, f"free run at 100 % paces at real time ({fps:.1f} fps, "
          f"expected ~50, accepted 25..75)")
    emu.log.flush()
    log = Path(log_path).read_text(encoding="utf-8", errors="replace")
    check("running without sound" in log, "log contains the no-audio-device warning")

    emu.call("pause")
    check_run(emu, 250, 4.0, 10.0, "100 %")
    emu.call("set_speed", {"mode": "max"})
    check_run(emu, 500, 0.0, 4.0, "MAX SPEED")
    emu.call("set_speed", {"mode": "normal"})
    check_run(emu, 100, 1.5, 4.0, "100 % after MAX SPEED")


def run_realtime_audio(emu, avi, ffmpeg):
    """Video záznam podle reality: zvuk se vyrábí i bez audio zařízení."""
    emu.call("pause")
    emu.call("videorec_timebase", {"timebase": "realtime"})
    emu.call("videorec_start", {"path": str(avi)})
    emu.call("run", {"frames": 1})  # konec snímku zpracuje start
    emu.call("run", {"frames": 250})  # boot z diskety (pípnutí IPL)
    st = emu.call("videorec_status")
    check(st["state"] == "recording" and st["timebase_effective"] == "realtime",
          f"recording in real time ({st['state']}, {st['timebase_effective']})")
    seq = st["last_event"]["seq"] if st.get("last_event") else 0
    emu.call("videorec_stop")
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        st = emu.call("videorec_status")
        ev = st.get("last_event") or {}
        if ev.get("seq", 0) > seq and ev.get("kind") == "saved":
            break
        time.sleep(0.2)
    else:
        raise TestFailure("recording was not saved")
    total = st["last_event"]["frame"]
    peak, n = _avi_audio_peak(ffmpeg, avi)
    check(n == total * 960, f"audio samples ({n} = {total} x 960)")
    check(peak > 1000, f"real-time audio without an audio device is not silent (peak {peak})")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=165)
    if os.environ.get("MZ_GUI_TESTS") != "1":
        print("SKIP: GUI test (opens a window); set MZ_GUI_TESTS=1 to run")
        return 77
    exe = _find_exe()
    # Pevný port jen z proměnné prostředí; jinak si každý spuštěný emulátor
    # vezme volný port (souběžné běhy testů se nesrazí).
    port = emu_test_proc.mcp_test_port("MZ_GUI_TEST_PORT")
    dsk = Path(os.environ.get("MZ_BLOXORZ_DSK", str(_BLOXORZ_DSK)))
    ffmpeg = _find_ffmpeg()
    tmp = Path(tempfile.mkdtemp(prefix="mz_gui_no_audio_"))
    ini = tmp / "mz800emu.ini"
    ini_text = "[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
    with_dsk = dsk.is_file() and ffmpeg is not None
    if with_dsk:
        copy = tmp / "bloxorz.dsk"
        shutil.copyfile(dsk, copy)
        ini_text += ("[FDC]\nconnected = 1\n"
                     f"wd279x_fdd0_dskpath = {copy}\nwd279x_fdd0_readonly = 1\n")
    ini.write_text(ini_text, encoding="utf-8")
    log_path = tmp / "emu.log"
    print(f"GUI without audio device test ({exe}, SDL_AUDIO_DRIVER={_BAD_AUDIO_DRIVER}, port {port or 'auto'})")
    emu = None
    rc = 0
    try:
        emu = TcpGuiEmu(exe, tmp, ini, port, log_path)
        run_pacing(emu, log_path)
        if with_dsk:
            run_realtime_audio(emu, tmp / "rt.avi", ffmpeg)
        else:
            print(f"SKIP real-time audio scenario: DSK or ffmpeg not found ({dsk})")
        print("RESULT: PASS")
    except TestFailure as e:
        print(f"RESULT: FAIL - {e}")
        rc = 1
    finally:
        if emu is not None:
            emu.close()
        if rc != 0 and log_path.is_file():
            print("--- emulator log (tail) ---")
            print("\n".join(log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-30:]))
        shutil.rmtree(tmp, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
