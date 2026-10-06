#!/usr/bin/env python3
"""End-to-end test MCP příkazů video záznamu (videorec_*) přes pipe transport.

Spustí mz800emu(.exe) s ``--mcp-pipe`` v izolovaném prostředí (vlastní
dočasný ``--cfg-dir``, INI s ``[BREAKPOINTS] auto_load=0, auto_save=0`` a
``[VIDEOREC] output_dir`` v dočasném adresáři, ``--no-save-ini``) bez
programu (IPL menu MZ-800) a projde celý postup agenta:

  1. ``videorec_status`` bez nahrávání (supported, idle), marker bez
     nahrávání -> čitelná chyba;
  2. ``videorec_start`` s cestou -> čekající start (cesta + sidecar známé
     hned), marker před zpracováním startu -> chyba "has not started yet";
  3. ``run frames`` -> nahrává se, marker "Intro";
  4. ``snapshot_save``, další snímky, ``snapshot_load`` -> retake (událost
     ``retake``, počet snímků zpět na bod snapshotu), marker po retake;
  5. ``videorec_pause`` paused=true / false (record-pause = nový segment);
  6. ``videorec_stop`` v pauze emulace -> událost ``saved``;
  7. kontrola AVI (počet snímků) a sidecaru (segmenty, markery);
  8. ``videorec_start`` bez cesty s ``frames`` -> vygenerované jméno ve
     výstupním adresáři a auto-stop po N snímcích (emulátor dál běží);
  9. ``shutdown``.

Scénář 2 (přeskočí se, pokud DSK chybí): Michalova hra Bloxorz z KOPIE
diskety (``MZ_BLOXORZ_DSK`` nebo výchozí cesta; originál se nemění, kopie je
připojená read-only přes INI ``[FDC] wd279x_fdd0_dskpath``) - boot do menu,
nahrávání, marker, ``CR`` (start levelu 1), checkpoint + špatný tah + retake,
tahy šipkami, stop; kontrola počtu snímků, markerů, segmentu a toho, že se
obsah nahraných snímků mění (menu / start levelu / po tazích, přes ffmpeg).

Scénář 3 (Task 18, přeskočí se bez DSK): režim podle reality s Bloxorz -
``videorec_timebase realtime`` bez nahrávání (nastavení pro start), start,
hraní přes ``run frames`` při 100 % (snímky podle hodin vzorkovače, pauzy
emulace mezi příkazy se podle výchozího ``realtime_pause = skip``
nezapisují), přepnutí zpět ``videorec_timebase emulated`` (hranice
segmentu), stop; kontrola počtu snímků proti uběhlému času emulace, zvuku
z headless SDL cesty (nenulový), sidecaru verze 3 (události timebase,
pause_start s value "frames", 2 segmenty).

Počet snímků AVI se ověřuje přes ffprobe (pokud je k dispozici), jinak
z hlavičky AVI (``avih.dwTotalFrames`` + součet partů není potřeba - nahrávky
jsou malé, jeden part).

Spouštění: ctest ``mcp_videorec_e2e`` (WORKING_DIRECTORY = kořen repa), nebo
ručně ``python tests/mcp/test_videorec_e2e.py`` (binárka z kořene repa nebo
proměnná ``MZ_EMU``). Vyžaduje MZ-800 build (``mz800emu``). Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import hashlib
import json
import os
import queue
import shutil
import struct
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


def _find_ffprobe():
    """Vrátí cestu k ffprobe, nebo None (pak se použije hlavička AVI)."""
    p = shutil.which("ffprobe")
    if p:
        return p
    cand = Path("C:/msys64/ucrt64/bin/ffprobe.exe")
    return str(cand) if cand.is_file() else None


def _avi_frames(path, ffprobe):
    """Počet video snímků AVI (ffprobe -count_frames, jinak avih.dwTotalFrames)."""
    if ffprobe:
        out = subprocess.run(
            [ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
             "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0", str(path)],
            capture_output=True, text=True, timeout=60)
        return int(out.stdout.strip())
    data = Path(path).read_bytes()[:256]
    i = data.find(b"avih")
    if i < 0:
        raise TestFailure(f"AVI header not found in {path}")
    # avih: size(4), dwMicroSecPerFrame, dwMaxBytesPerSec, dwPaddingGranularity,
    # dwFlags, dwTotalFrames
    return struct.unpack_from("<I", data, i + 8 + 16)[0]


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
        hello = self._read(20.0, lambda m: m.get("type") == "hello")
        if not hello:
            raise TestFailure("no hello from emulator")
        cmds = hello.get("commands", [])
        for c in ("videorec_start", "videorec_stop", "videorec_pause",
                  "videorec_marker", "videorec_status", "videorec_timebase"):
            if c not in cmds:
                raise TestFailure(f"hello does not advertise {c}")

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
            msg = json.loads(line)
            if pred(msg):
                return msg
        return None

    def call(self, cmd, data=None, expect_ok=True, timeout=30.0):
        """Pošle request a vrátí response (při expect_ok ověří success)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to {cmd}")
        if expect_ok and not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        if not expect_ok and resp.get("success"):
            raise TestFailure(f"{cmd} unexpectedly succeeded: {resp}")
        return resp

    def wait_saved(self, after_seq, timeout=30.0):
        """Počká na událost saved/failed novější než after_seq; vrátí status."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            st = self.call("videorec_status")["data"]
            ev = st.get("last_event")
            if st["state"] == "idle" and ev and ev["seq"] > after_seq \
                    and ev["kind"] in ("saved", "failed"):
                return st
            time.sleep(0.1)
        raise TestFailure("recording was not saved in time")

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


def run(emu, tmp, ffprobe):
    """Vlastní scénář; vyhodí TestFailure při první chybě."""
    avi = tmp / "agent.avi"
    cuts = tmp / "agent.cuts.json"
    snap = tmp / "retake.mzs"

    emu.call("pause")
    # Scénář běží při 100 % (headless tempo podle systémových hodin, cca
    # 50 snímků/s) - ověřuje i nahrávání při normální rychlosti.
    st = emu.call("videorec_status")["data"]
    check(st["supported"] is True and st["state"] == "idle", "status idle + supported")
    check(st["retake_mode"] == "discard", f"default retake mode discard ({st['retake_mode']})")
    r = emu.call("videorec_marker", {"label": "x"}, expect_ok=False)
    check("not running" in r.get("error", ""), "marker without recording -> readable error")
    r = emu.call("videorec_stop", expect_ok=False)
    check("not running" in r.get("error", ""), "stop without recording -> readable error")

    r = emu.call("videorec_start", {"path": str(tmp / "no_such_dir" / "x.avi")}, expect_ok=False)
    check("Cannot create video file" in r.get("error", ""), "start into missing directory -> readable error")

    # Start (čekající), cesta a sidecar známé hned.
    d = emu.call("videorec_start", {"path": str(avi)})["data"]
    check(d["start_requested"] and d["start_pending"] and d["state"] == "idle",
          "start accepted, pending until next frame end")
    check(Path(d["path"]) == avi and Path(d["sidecar"]) == cuts, "start returns path + sidecar")
    r = emu.call("videorec_marker", {"label": "early"}, expect_ok=False)
    check("not started yet" in r.get("error", ""), "marker before start processed -> error")
    r = emu.call("videorec_start", {"path": str(tmp / "other.avi")}, expect_ok=False)
    check("already running" in r.get("error", ""), "second start -> already running")

    # Nahrávání: 1. konec snímku start zpracuje (snímek se nezapíše).
    emu.call("run", {"frames": 11})
    st = emu.call("videorec_status")["data"]
    check(st["state"] == "recording" and st["frames"] == 10, f"recording, 10 frames (got {st['frames']})")
    m = emu.call("videorec_marker", {"label": "Intro"})["data"]
    check(m["label"] == "Intro", "marker Intro accepted")

    # Retake: snapshot v bodě 12, 20 snímků navíc, nahrání -> zpět na 12.
    # (Marker přesně v bodě snapshotu by retake zahodil - markery od bodu
    # snapshotu dál patří do zahozené části; proto nejdřív 2 snímky.)
    emu.call("run", {"frames": 2})
    emu.call("snapshot_save", {"path": str(snap)})
    emu.call("run", {"frames": 20})
    st = emu.call("videorec_status")["data"]
    check(st["frames"] == 32, f"32 frames before retake (got {st['frames']})")
    emu.call("snapshot_load", {"path": str(snap)})
    emu.call("run", {"frames": 1})
    st = emu.call("videorec_status")["data"]
    ev = st["last_event"]
    check(ev is not None and ev["kind"] == "retake" and ev["frame"] == 12,
          f"retake event at frame 12 ({ev})")
    check(st["frames"] == 13, f"frames rewound to snapshot point (got {st['frames']})")
    m = emu.call("videorec_marker")["data"]
    check(m["label"] == "Marker at frame 13", f"default marker label ({m['label']})")
    emu.call("run", {"frames": 9})

    # Record-pause: explicitní stavy, idempotentní.
    emu.call("videorec_pause", {"paused": True})
    emu.call("videorec_pause", {"paused": True})
    emu.call("run", {"frames": 5})
    st = emu.call("videorec_status")["data"]
    check(st["state"] == "paused" and st["frames"] == 22,
          f"record-pause holds at 22 frames ({st['state']}, {st['frames']})")
    emu.call("videorec_pause", {"paused": False})
    emu.call("run", {"frames": 6})
    st = emu.call("videorec_status")["data"]
    check(st["state"] == "recording" and st["frames"] == 28 and st["segment"] == 2,
          f"resumed, new segment ({st['frames']} frames, segment {st['segment']})")

    # Stop v pauze emulace -> saved.
    seq = st["last_event"]["seq"]
    d = emu.call("videorec_stop")["data"]
    check(d["stop_requested"] is True, "stop requested")
    st = emu.wait_saved(seq)
    check(st["last_event"]["kind"] == "saved", f"saved event ({st['last_event']})")
    check(Path(st["last_event"]["path"]) == avi, "saved event path")
    check(st["bytes"] > 0 and st["parts"] == 1, f"bytes/parts ({st['bytes']}, {st['parts']})")

    n = _avi_frames(avi, ffprobe)
    check(n == 28, f"AVI has 28 video frames (got {n})")
    sc = json.loads(cuts.read_text(encoding="utf-8"))
    labels = [(mk["frame"], mk["label"]) for mk in sc["markers"]]
    # Nahrání snapshotu ani pauzy v emulačním čase auto marker nedostávají
    # (final review I2) - nahrání snapshotu zůstává jen jako událost v "events".
    check(labels == [(10, "Intro"), (13, "Marker at frame 13")],
          f"sidecar markers {labels}")
    check(any(e["kind"] == "snapshot" and e["frame"] == 12 for e in sc["events"]),
          f"sidecar snapshot event at frame 12 ({sc.get('events')})")
    check(len(sc["segments"]) == 2, f"sidecar has 2 segments ({sc['segments']})")

    # Vygenerované jméno + auto-stop po N snímcích.
    d = emu.call("videorec_start", {"frames": 7})["data"]
    gen = Path(d["path"])
    check(gen.parent == tmp and gen.name.startswith("mz800_") and gen.suffix == ".avi",
          f"generated name in output_dir ({gen})")
    check(d["stop_after_frames"] == 7, "stop_after_frames echoed")
    seq = (d["last_event"] or {"seq": 0})["seq"]
    emu.call("run", {"frames": 12})
    st = emu.wait_saved(seq)
    n = _avi_frames(gen, ffprobe)
    check(n == 7, f"auto-stop after 7 frames (got {n})")
    emu.call("run", {"frames": 1})  # emulátor po auto-stopu dál běží


# ---------------------------------------------------------------------------
# Scénář 2: skutečná hra z diskety (Michalova Bloxorz)
# ---------------------------------------------------------------------------

#: Výchozí cesta k Michalově hře; přepíše proměnná MZ_BLOXORZ_DSK. Originál se
#: nikdy nemění - test pracuje s kopií (navíc připojenou read-only).
_BLOXORZ_DSK = Path("C:/msys64/home/Michal/projects/bloxorz-mz800/build/bloxorz.dsk")


def _find_ffmpeg():
    """Vrátí cestu k ffmpeg, nebo None (pak se obsah snímků nekontroluje)."""
    p = shutil.which("ffmpeg")
    if p:
        return p
    cand = Path("C:/msys64/ucrt64/bin/ffmpeg.exe")
    return str(cand) if cand.is_file() else None


def _frame_hash(ffmpeg, avi, n):
    """MD5 obsahu snímku `n` AVI (rgb24 přes ffmpeg)."""
    out = subprocess.run(
        [ffmpeg, "-v", "error", "-i", str(avi), "-vf", f"select=eq(n\\,{n})",
         "-fps_mode", "passthrough", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, timeout=120)
    if out.returncode != 0 or not out.stdout:
        raise TestFailure(f"ffmpeg cannot extract frame {n} of {avi}")
    return hashlib.md5(out.stdout).hexdigest()


def _tap(emu, key, hold=5, settle=40):
    """Stiskne klávesu na `hold` snímků, pustí a nechá hru `settle` snímků doběhnout."""
    emu.call("input_press_key", {"key": key})
    emu.call("run", {"frames": hold})
    emu.call("input_release_key", {"key": key})
    emu.call("run", {"frames": settle})


def run_bloxorz(exe, tmp, ffprobe, ffmpeg, dsk):
    """Hra Bloxorz z kopie DSK: menu -> CR (level 1) -> tahy, marker, retake, stop.

    Posloupnost kláves (ověřeno screenshoty): po bootu (cca 300 snímků) je
    menu se zvýrazněným START GAME, `CR` spustí level 1 (blok na startu,
    MOVES 0), šipky `RIGHT` / `DOWN` (staré názvy z hid_keymap;
    aliasy `CURSOR_RIGHT` / `CURSOR_DOWN` platí také) kutálí blok (MOVES roste).
    """
    work = tmp / "bloxorz"
    work.mkdir()
    copy = work / "bloxorz.dsk"
    shutil.copyfile(dsk, copy)
    cfg = work / "cfg"
    cfg.mkdir()
    ini = work / "bloxorz.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                   "[FDC]\nconnected = 1\n"
                   f"wd279x_fdd0_dskpath = {copy}\nwd279x_fdd0_readonly = 1\n",
                   encoding="utf-8")
    avi = work / "bloxorz.avi"
    snap = work / "level1.mzs"
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        # MAX SPEED jen zkracuje test (boot z diskety 300 snímků + tahy); nahrávka
        # se týká emulovaných snímků, takže na obsah videa vliv nemá.
        emu.call("set_speed", {"mode": "max"})
        r = emu.call("run", {"frames": 300})["data"]  # boot z diskety do menu
        check(r["actual_frames"] == 300, "booted 300 frames")

        emu.call("videorec_start", {"path": str(avi)})
        emu.call("run", {"frames": 1})  # konec snímku zpracuje start
        emu.call("run", {"frames": 50})  # 50 snímků menu
        st = emu.call("videorec_status")["data"]
        check(st["state"] == "recording" and st["frames"] == 50, f"menu recorded ({st['frames']})")
        menu_frame = 25

        emu.call("videorec_marker", {"label": "Level 1"})
        k = emu.call("input_send_keys", {"text": json.dumps(["CR"]), "encoding": "key_names",
                                         "frame_per_key": 5})["data"]
        check(k["keys_landed"] == 1, f"CR landed ({k})")
        emu.call("run", {"frames": 100})
        st = emu.call("videorec_status")["data"]
        start_frame = st["frames"] - 1  # level 1 na startu, MOVES 0

        # Checkpoint, špatný tah, retake.
        emu.call("snapshot_save", {"path": str(snap)})
        cp = st["frames"]
        _tap(emu, "DOWN")
        emu.call("snapshot_load", {"path": str(snap)})
        emu.call("run", {"frames": 1})
        st = emu.call("videorec_status")["data"]
        check(st["last_event"]["kind"] == "retake" and st["last_event"]["frame"] == cp,
              f"retake to checkpoint {cp} ({st['last_event']})")

        # Dobré tahy.
        _tap(emu, "RIGHT")
        emu.call("videorec_marker", {"label": "Rolling"})
        _tap(emu, "RIGHT")
        _tap(emu, "DOWN")
        st = emu.call("videorec_status")["data"]
        total = st["frames"]
        seq = st["last_event"]["seq"]
        emu.call("videorec_stop")
        st = emu.wait_saved(seq)
        check(st["last_event"]["kind"] == "saved" and st["last_event"]["frame"] == total,
              f"saved {total} frames ({st['last_event']})")
    finally:
        emu.close()

    check(_avi_frames(avi, ffprobe) == total, f"AVI has {total} frames")
    sc = json.loads((work / "bloxorz.cuts.json").read_text(encoding="utf-8"))
    labels = [mk["label"] for mk in sc["markers"]]
    # Retake v emulačním čase bez auto markeru (final review I2); událost zůstává.
    check(labels == ["Level 1", "Rolling"] and sc["markers"][0]["frame"] == 50,
          f"sidecar markers {sc['markers']}")
    check(sc["version"] == 4 and sc.get("platform") == "mz800" and any(e["kind"] == "snapshot" and e["value"] == "retake" for e in sc["events"]),
          f"sidecar v4 retake event ({sc.get('events')})")
    check(len(sc["segments"]) == 1, f"retake keeps one segment ({sc['segments']})")
    check(copy.stat().st_size == dsk.stat().st_size, "DSK copy intact size")

    if ffmpeg:
        h_menu = _frame_hash(ffmpeg, avi, menu_frame)
        h_start = _frame_hash(ffmpeg, avi, start_frame)
        h_end = _frame_hash(ffmpeg, avi, total - 1)
        check(len({h_menu, h_start, h_end}) == 3,
              "recorded frames change: menu != level start != after moves")
    else:
        print("  SKIP: ffmpeg not found, frame content not checked")


def _avi_audio_peak(ffmpeg, avi):
    """Největší |vzorek| zvuku AVI (s16le přes ffmpeg)."""
    out = subprocess.run([ffmpeg, "-v", "error", "-i", str(avi), "-map", "0:a", "-f", "s16le", "-"],
                         capture_output=True, timeout=120)
    if out.returncode != 0:
        raise TestFailure(f"ffmpeg cannot decode audio of {avi}")
    n = len(out.stdout) // 2
    samples = struct.unpack(f"<{n}h", out.stdout[:2 * n])
    return max((abs(v) for v in samples), default=0), n // 2


#: Zpoždění obrazu v režimu podle reality [snímky] = videorec_rt_video_delay_ticks() při 50 snímcích/s
#: (VIDEOREC_RT_VIDEO_DELAY_MS = 40 ms, videorec_rt.h).
RT_DELAY = 2


def run_realtime_bloxorz(exe, tmp, ffprobe, ffmpeg, dsk):
    """Režim podle reality (Task 18) s kopií Bloxorz: realtime -> emulated, stop, sidecar v3."""
    work = tmp / "realtime"
    work.mkdir()
    copy = work / "bloxorz.dsk"
    shutil.copyfile(dsk, copy)
    cfg = work / "cfg"
    cfg.mkdir()
    ini = work / "rt.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                   "[FDC]\nconnected = 1\n"
                   f"wd279x_fdd0_dskpath = {copy}\nwd279x_fdd0_readonly = 1\n",
                   encoding="utf-8")
    avi = work / "rt.avi"
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        st = emu.call("videorec_timebase", {"timebase": "realtime"})["data"]
        check(st["state"] == "idle" and st["timebase"] == "realtime"
              and st["timebase_effective"] == "emulated",
              f"timebase set for the next start ({st['timebase']}, {st['timebase_effective']})")
        r = emu.call("videorec_timebase", {"timebase": "fast"}, expect_ok=False)
        check("timebase" in r.get("error", ""), "invalid timebase -> readable error")

        # Nahrávání od startu bootu z diskety (pípnutí IPL = zvuk v nahrávce).
        emu.call("videorec_start", {"path": str(avi)})
        emu.call("run", {"frames": 1})  # konec snímku zpracuje start -> rovnou realtime
        t0 = time.time()
        emu.call("run", {"frames": 150})
        emu.call("run", {"frames": 100})
        run_s = time.time() - t0
        time.sleep(0.5)  # pauza emulace mezi příkazy: skip = nezapisuje se
        st = emu.call("videorec_status")["data"]
        check(st["state"] == "recording" and st["timebase_effective"] == "realtime",
              f"recording in real time ({st['state']}, {st['timebase_effective']})")
        check(st["rt_activity"] == "skipping", f"paused emulation is skipped ({st['rt_activity']})")
        # 250 emulovaných snímků při 100 % = 5 s; vzorkovač 50/s => cca 250 snímků
        # (+ start, tolerance na fázi ticků vůči příkazům). Čas mezi příkazy se nepočítá.
        rt_frames = st["frames"]
        check(225 <= rt_frames <= 275,
              f"real-time frames match 5 s of emulation ({rt_frames}, wall {run_s:.2f} s)")

        st = emu.call("videorec_timebase", {"timebase": "emulated"})["data"]
        check(st["timebase"] == "emulated", "switch back to emulated requested")
        emu.call("run", {"frames": 50})
        st = emu.call("videorec_status")["data"]
        check(st["timebase_effective"] == "emulated" and st["rt_activity"] == "off",
              f"back in emulated time ({st['timebase_effective']}, {st['rt_activity']})")
        # návrat: zpožďovací fronta (RT_DELAY snímků) + 50 emulovaných
        check(st["frames"] == rt_frames + RT_DELAY + 50,
              f"emulated frames after switch ({st['frames']}, rt {rt_frames})")
        check(st["segment"] == 2, f"timebase switch = segment boundary ({st['segment']})")
        total = st["frames"]
        seq = st["last_event"]["seq"]
        emu.call("videorec_stop")
        st = emu.wait_saved(seq)
        check(st["last_event"]["frame"] == total, f"saved {total} frames")
    finally:
        emu.close()

    check(_avi_frames(avi, ffprobe) == total, f"AVI has {total} frames")
    if ffmpeg:
        peak, n = _avi_audio_peak(ffmpeg, avi)
        check(n == total * 960, f"audio samples ({n} = {total} x 960)")
        check(peak > 1000, f"real-time audio from the headless SDL path is not silent (peak {peak})")
    sc = json.loads((work / "rt.cuts.json").read_text(encoding="utf-8"))
    check(sc["version"] == 4 and sc.get("fps_num") == 50, "sidecar version 4, 50 fps")
    tb = [(e["frame"], e["value"]) for e in sc["events"] if e["kind"] == "timebase"]
    check(tb == [(0, "realtime"), (rt_frames + RT_DELAY, "emulated")], f"timebase events {tb}")
    check(any(e["kind"] == "pause_start" and e["value"] == "frames" for e in sc["events"]),
          "emu_run stop recorded as pause_start 'frames'")
    check(not any(m["label"] == "Pause" for m in sc["markers"]),
          f"no auto marker for emu_run stops ({sc['markers']})")
    check([(g["start"], g["end"]) for g in sc["segments"]]
          == [(0, rt_frames + RT_DELAY), (rt_frames + RT_DELAY, total)],
          f"segments {sc['segments']}")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    ffprobe = _find_ffprobe()
    ffmpeg = _find_ffmpeg()
    print(f"Using binary: {exe}; ffprobe: {ffprobe or 'not found (AVI header)'}")
    dsk = Path(os.environ.get("MZ_BLOXORZ_DSK", str(_BLOXORZ_DSK)))
    tmp = Path(tempfile.mkdtemp(prefix="mz_videorec_e2e_"))
    cfg = tmp / "cfg"
    cfg.mkdir()
    ini = tmp / "test.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                   f"[VIDEOREC]\noutput_dir = {tmp}\n", encoding="utf-8")
    emu = None
    ok = False
    try:
        emu = PipeEmu(exe, cfg, ini)
        run(emu, tmp, ffprobe)
        emu.close()
        emu = None
        if dsk.is_file():
            print(f"Scenario 2: game {dsk} (copy)")
            run_bloxorz(exe, tmp, ffprobe, ffmpeg, dsk)
            print(f"Scenario 3: real-time timebase, game {dsk} (copy)")
            run_realtime_bloxorz(exe, tmp, ffprobe, ffmpeg, dsk)
        else:
            print(f"SKIP scenarios 2 and 3: game DSK not found ({dsk})")
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    finally:
        if emu:
            emu.close()
    if ok:
        if os.environ.get("MZ_VIDEOREC_KEEP"):  # ruční vizuální kontrola nahrávek
            print(f"Artifacts kept in {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
