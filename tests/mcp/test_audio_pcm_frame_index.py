#!/usr/bin/env python3
"""E2E test: výřezy emu_audio_capture_pcm podle snímku jsou přesné po celou dobu uchování PCM (MCP pipe).

Zachycení drží index hranic snímků (index prvního vzorku snímku v proudu
reference i SDL cesty) po celou dobu, co jsou data snímku v PCM ringu,
nezávisle na ringu souhrnů snímků. Test ověřuje na skutečném emulátoru:

  1  Okraj ringu souhrnů při vlastní rychlosti a její změně uprostřed
     zachycení (mz800emu 50 % -> 200 %, mz700emu-ntsc 200 % -> 50 %, oba
     běhy souběžně, asi 65 s): rozsah i otisk obsahu snímků 40 a 110
     (a pětice 38..42) před přepsáním souhrnu a po něm je stejný, v obou
     proudech. Na 04ee7d22 vracel tentýž dotaz po přepsání jiný snímek
     reference ([117221, 118102) místo [70508, 72271) při 200 %) nebo
     prázdný výřez (50 %) bez upozornění.
  2  Totéž při MAX SPEED na mz700emu-ntsc (rychle, jmenovitá délka snímku).
  3  Kontrakt konce (mz800emu 100 %, 50 %, 200 %; ruční stop, automatický
     konec po `frames`, živé čtení s otevřeným snímkem): dotazy podle snímku
     pokrývají jen uzavřené snímky. Společný dotaz (N - 1, 2) = samostatný
     (N - 1, 1) (rozsah i data), samostatný (N, 1) je prázdný s
     `frame_range.status` "empty" a důvodem start_after_last_closed_frame;
     další PCM (blok SDL přijatý po posledním snímku) je čitelné přes
     `sample_from`. Na 04ee7d22 vracel (N, 1) dalších 881 vzorků SDL.
  4  `audio_capture_summary` s `lag_frames=0`: při 100 % `ref_vs_sdl.status`
     "ok", při 50 % a 200 % "not_comparable" s důvodem (dřív číselná
     odchylka jiného místa nebo hlášení, že se data nepřekrývají).

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí
se jen vlastní PID. Binárky: kořen repa, nebo proměnná MZ_EMU_DIR. Výstup
anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import base64
import hashlib
import shutil
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, _find_exe  # noqa: E402
from test_audio_crystal_pitch import _prog_ctc0  # noqa: E402

_SOURCES = ("reference", "sdl")


def _window(emu, source, frame_from=None, frames=1, sample_from=None, samples=None):
    """Výřez 44100 Hz stereo; vrátí (from, next, otisk, frame_range)."""
    q = {"source": source, "rate": 44100, "stereo": True, "encoding": "pcm_b64", "max_bytes": 4194304}
    if sample_from is None:
        q.update({"frame_from": frame_from, "frames": frames})
    else:
        q.update({"sample_from": sample_from, "samples": samples})
    a = emu.call("audio_capture_pcm", q)
    data = base64.b64decode(a["data_b64"])
    if a["truncated"] or len(data) != 4 * a["samples"]:
        raise TestFailure(f"{source} {q}: inconsistent response")
    return a["from_sample"], a["next_sample"], hashlib.sha256(data).hexdigest()[:16], a.get("frame_range")


def _setup(emu, speed):
    """Tón CTC0 a rychlost; speed 0 = MAX SPEED."""
    emu.call("pause")
    emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
    emu.call("mem_write", {"addr": 0x3000, "data_hex": _prog_ctc0(2519).hex()})
    emu.call("set_register", {"reg": "PC", "value": 0x3000})
    if speed == 0:
        emu.call("set_speed", {"mode": "max"})
    elif speed != 100:
        emu.call("set_speed", {"mode": "custom", "percent": speed})
    emu.call("run", {"frames": 8}, timeout=60.0)


def _run_until(emu, key, value, step=100, timeout=240.0):
    """Běží po `step` snímcích obrazu, dokud stav zachycení `key` (frames / first_seq) nedosáhne `value`."""
    while True:
        st = emu.call("audio_capture_status")
        cur = st["frames"] if key == "frames" else st["memory"]["streams"]["frames"]["first_seq"]
        if cur >= value:
            return st
        emu.call("run", {"frames": step}, timeout=timeout)


def run_ring(exe, speeds, results, label):
    """Případ 1 / 2: rozsah a obsah snímků před přepsáním souhrnu a po něm; výsledek do `results[label]`."""
    tmp = Path(tempfile.mkdtemp(prefix="mztest-pcmindex-"))
    emu = PipeEmu(exe, tmp)
    lines, ok = [], True
    try:
        _setup(emu, speeds[0])
        emu.call("audio_capture_start", {"events": False, "psg_writes": False, "reference_wav": False,
                                         "sdl_wav": False, "pcm": list(_SOURCES),
                                         "memory_minutes": 1, "pcm_seconds": 180})
        _run_until(emu, "frames", 60, step=20)
        if len(speeds) > 1:
            emu.call("pause")
            emu.call("set_speed", {"mode": "custom", "percent": speeds[1]} if speeds[1] else {"mode": "max"})
        _run_until(emu, "frames", 130, step=20)
        probes = ((40, 1), (110, 1), (38, 5))
        before = {(s, f, n): _window(emu, s, f, n) for s in _SOURCES for f, n in probes}
        st = _run_until(emu, "first_seq", 116, step=500)
        emu.call("audio_capture_stop")
        oldest = st["memory"]["streams"]["frames"]["first_seq"]
        for (s, f, n), b in before.items():
            a = _window(emu, s, f, n)
            fr = a[3] or {}
            good = a[:3] == b[:3] and b[1] > b[0] and fr.get("status") == "complete"
            ok = ok and good
            lines.append(f"  {'PASS' if good else 'FAIL'}: {label} {s} frames [{f}, {f + n}) before "
                         f"[{b[0]}, {b[1]}) {b[2]}, after summary overwrite (oldest summary {oldest}) "
                         f"[{a[0]}, {a[1]}) {a[2]} {fr.get('status')}")
    except Exception as e:  # noqa: BLE001 - výsledek vlákna
        lines.append(f"  FAIL: {label}: {e!r}")
        ok = False
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    results[label] = (ok, lines)


def run_end(exe, speed):
    """Případ 3 a 4 pro jednu rychlost; vrátí True = prošlo."""
    ok = True
    for mode in ("stop", "frames", "live"):
        tmp = Path(tempfile.mkdtemp(prefix="mztest-pcmend-"))
        emu = PipeEmu(exe, tmp)
        try:
            _setup(emu, speed)
            start = {"events": False, "psg_writes": False, "reference_wav": False, "sdl_wav": False,
                     "pcm": list(_SOURCES)}
            if mode == "frames":
                start["frames"] = 30
            emu.call("audio_capture_start", start)
            emu.call("run", {"frames": 38 if mode != "frames" else 80}, timeout=120.0)
            emu.call("pause")
            if mode == "stop":
                emu.call("audio_capture_stop")
            st = emu.call("audio_capture_status")
            n = st["frames"]
            if mode == "frames" and (n != 30 or st["active"]):
                raise TestFailure(f"automatic end after 30 frames expected (frames {n}, active {st['active']})")
            for s in _SOURCES:
                last = _window(emu, s, n - 1, 1)
                after = _window(emu, s, n, 1)
                both = _window(emu, s, n - 1, 2)
                fr_after, fr_both = after[3] or {}, both[3] or {}
                extra = _window(emu, s, sample_from=after[0], samples=4096)
                good = (both[:3] == last[:3] and last[1] > last[0]
                        and after[0] == after[1] == last[1]
                        and fr_after.get("status") == "empty"
                        and "start_after_last_closed_frame" in fr_after.get("reasons", [])
                        and fr_both.get("status") == "partial"
                        and "end_after_last_closed_frame" in fr_both.get("reasons", [])
                        and fr_both.get("closed_frames") == n)
                ok = ok and good
                print(f"  {'PASS' if good else 'FAIL'}: {exe.stem} {speed} % {mode} {s}: closed {n}, "
                      f"[{n - 1}] [{last[0]}, {last[1]}), [{n}] [{after[0]}, {after[1]}) {fr_after.get('status')}, "
                      f"[{n - 1}, {n + 1}) [{both[0]}, {both[1]}) {fr_both.get('status')}; "
                      f"further PCM by sample_from: {extra[1] - extra[0]} samples")
            if mode == "stop":
                f = n // 2
                rv = emu.call("audio_capture_summary", {"frame_from": f, "frame_to": f, "lag_frames": 0})["ref_vs_sdl"]
                want = "ok" if speed == 100 else "not_comparable"
                good = rv.get("status") == want and (want == "ok") == ("diff_rms" in rv) and \
                    (want == "ok" or bool(rv.get("reason")))
                ok = ok and good
                print(f"  {'PASS' if good else 'FAIL'}: {exe.stem} {speed} % summary frame {f} lag_frames=0: "
                      f"status {rv.get('status')} (expected {want}), diff_rms {rv.get('diff_rms')}, "
                      f"frames_off_nominal {rv.get('frames_off_nominal')}")
            if mode == "live":
                emu.call("audio_capture_stop")
        except TestFailure as e:
            print(f"  FAIL: {exe.stem} {speed} % {mode}: {e}")
            ok = False
        finally:
            emu.close()
            shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main():
    """Spustí případy; 0 = PASS, 1 = FAIL, 77 = SKIP."""
    emu_test_proc.install(deadline_s=290)
    pal = _find_exe("mz800emu")
    if pal is None:
        print("SKIP: no mz800emu binary found")
        return 77
    ntsc = _find_exe("mz700emu-ntsc")
    results = {}
    threads = [threading.Thread(target=run_ring, args=(pal, (50, 200), results, "mz800emu 50 % -> 200 %"))]
    if ntsc is not None:
        threads.append(threading.Thread(target=run_ring, args=(ntsc, (200, 50), results, "mz700emu-ntsc 200 % -> 50 %")))
    for t in threads:
        t.start()
    ok = True
    print("\n=== end of capture: frame queries cover closed frames ===")
    for speed in (100, 50, 200):
        ok = run_end(pal, speed) and ok
    if ntsc is not None:
        print("\n=== mz700emu-ntsc MAX SPEED: frame summary ring edge ===")
        run_ring(ntsc, (0,), results, "mz700emu-ntsc MAX")
    for t in threads:
        t.join()
    print("\n=== frame boundaries after the summary ring overwrote the frame ===")
    for label in sorted(results):
        good, lines = results[label]
        print("\n".join(lines))
        ok = ok and good
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
