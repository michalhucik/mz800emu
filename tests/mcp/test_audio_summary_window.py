#!/usr/bin/env python3
"""E2E test: okno a pořadí snímků souhrnu zvuku podle skutečných indexů PCM (MCP pipe).

Souhrn `audio_capture_summary` (`ref_vs_sdl`) má pro každý snímek stejné
hranice jako výřez `audio_capture_pcm` s `frame_from` (obojí z uložených
hranic snímků zachycení, ne přepočtem z taktů). Na mz800emu (PAL),
mz1500emu a mz700emu-ntsc (NTSC), souběžně, s pamětí PCM 5 s (všechny
vzorky drží) a zastaveným zachycením po 230 snímcích ověřuje:

  1  pro každý uzavřený snímek: `window` souhrnu s pevným nulovým posunem
     = `frame_range.samples_requested` výřezu téhož snímku, `compared_samples`
     = délka průniku okna s drženými vzorky obou proudů, `worst_frames_detail`
     má ten snímek s týmž počtem dvojic; okna sousedních snímků navazují bez
     mezery a bez překryvu. Na 0374ca6a se okno na části hranic lišilo
     o 1 vzorek (posudek 0374ca6a: MZ-800 snímek 128 [112813, 113695) místo
     [112814, 113695), MZ-1500 a MZ-700 NTSC snímek 209 konec o 1 dřív);
  2  delší okna (s dvousekundovým stropem 88 200 dvojic) s pevným posunem
     0 a 1 snímek: `worst_frames` = pořadí nejvýš pěti snímků podle RMS
     rozdílu spočítaného z PCM nad hranicemi výřezů a průnikem porovnaného
     úseku; `worst_frames_detail` totéž v témž pořadí s počtem dvojic;
  3  klient tvaru vydané verze: `for frame, rms in worst_frames` projde
     (na 0374ca6a ValueError, položka měla tři prvky).

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí
se jen vlastní PID. Binárky: kořen repa, nebo proměnná MZ_EMU_DIR. Výstup
anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import base64
import math
import shutil
import struct
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, _find_exe  # noqa: E402
from test_audio_pcm_frame_index import _setup  # noqa: E402
from test_audio_read import read_rows  # noqa: E402

#: Tolerance RMS: odpověď je zaokrouhlená na 5 desetinných míst.
_TOL = 0.0000051
#: Strop porovnání souhrnu (2 s při 44100 Hz).
_CAP = 88200


def _pcm(emu, source, **sel):
    """PCM 44100 Hz stereo jako mono (L + R) / 65536; vrátí (odpověď, vzorky)."""
    a = emu.call("audio_capture_pcm", dict(source=source, rate=44100, stereo=True, encoding="pcm_b64",
                                           max_bytes=4194304, **sel))
    raw = base64.b64decode(a["data_b64"])
    return a, [(l + r) / 65536 for l, r in struct.iter_unpack("<hh", raw)]


def _ranking(bounds, first, last, lo, hi, ref, r0, sdl, s0, delay):
    """Nejvýš 5 snímků [first, last] podle RMS rozdílu na průniku s [lo, hi), sestupně, stabilně."""
    scores = []
    for f in range(first, last + 1):
        a, b = max(lo, bounds[f]), min(hi, bounds[f + 1])
        if b > a:
            s = sum((ref[k - r0] - sdl[k + delay - s0]) ** 2 for k in range(a, b))
            scores.append((f, math.sqrt(s / (b - a)), b - a))
    return sorted(scores, key=lambda v: -v[1])[:5]


def _same_ranking(want, worst, detail):
    """Pořadí souhlasí (snímek a počet dvojic; při RMS shodném do tolerance i přehozené pořadí)."""
    if len(want) != len(worst) or len(worst) != len(detail):
        return False
    for k, (f, rms, n) in enumerate(want):
        wf, wr = worst[k]
        d = detail[k]
        if abs(wr - rms) >= _TOL or d["frame"] != wf or abs(d["diff_rms"] - wr) >= _TOL:
            return False
        if wf != f or d["compared_samples"] != n:
            # jen přehozené pořadí snímků se shodným RMS do tolerance
            alt = [x for x in want if x[0] == wf]
            if not alt or abs(alt[0][1] - rms) >= _TOL or alt[0][2] != d["compared_samples"]:
                return False
    return True


def run(exe, results):
    """Všechny případy jedné binárky; výsledek do `results[exe.stem]`."""
    out, ok = [], True
    tmp = Path(tempfile.mkdtemp(prefix="mztest-sumwin-"))
    emu = PipeEmu(exe, tmp)
    label = exe.stem
    try:
        _setup(emu, 0)
        emu.call("audio_capture_start", dict(events=False, psg_writes=False, reference_wav=False, sdl_wav=False,
                                             memory_minutes=1, pcm=["reference", "sdl"], pcm_seconds=5))
        emu.call("run", {"frames": 230}, timeout=120.0)
        emu.call("audio_capture_stop")
        fields, rows, _ = read_rows(emu, "audio_frames", ["seq", "ref_first"])
        nfr = len(rows)
        held = {}
        for s in ("reference", "sdl"):
            held[s] = _pcm(emu, s, sample_from=0, samples=1)[0]["available"]
        (r0, r1), (s0, s1) = held["reference"], held["sdl"]
        # 1: každý snímek - okno = výřez PCM, průnik, sousedé navazují
        bounds, bad, prev_end = [], 0, None
        for f in range(nfr):
            a = _pcm(emu, "reference", frame_from=f, frames=1)[0]
            want = a["frame_range"]["samples_requested"]
            rv = emu.call("audio_capture_summary", {"frame_from": f, "frame_to": f, "lag_frames": 0})["ref_vs_sdl"]
            lo, hi = max(want[0], r0, s0), min(want[1], r1, s1)
            pairs = max(0, hi - lo)
            det = rv.get("worst_frames_detail") or []
            good = (rv.get("window") == want and rv.get("compared_samples", 0) == pairs
                    and (pairs == 0 or (rv.get("from_sample") == lo and len(det) == 1 and det[0]["frame"] == f
                                        and det[0]["compared_samples"] == pairs))
                    and (prev_end is None or want[0] == prev_end))
            if not good:
                bad += 1
                if bad <= 5:
                    out.append(f"  FAIL: {label} frame {f}: pcm window {want}, summary window {rv.get('window')}, "
                               f"expected {pairs} pairs from {lo}, got {rv.get('compared_samples')} "
                               f"from {rv.get('from_sample')}, detail {det}")
            bounds.append(want[0])
            prev_end = want[1]
        bounds.append(prev_end)
        good = bad == 0
        ok = ok and good
        out.append(f"  {'PASS' if good else 'FAIL'}: {label}: {nfr} frames, summary window = PCM frame window, "
                   f"pairs = intersection, neighbours join ({bad} wrong)")
        # 2: delší okna a pořadí z PCM
        ref = _pcm(emu, "reference", sample_from=r0, samples=r1 - r0)[1]
        sdl = _pcm(emu, "sdl", sample_from=s0, samples=s1 - s0)[1]
        start = next(i for i in range(nfr) if bounds[i] >= max(r0, s0))
        capped = 0
        for first, last in ((start, nfr - 2), (start + 4, start + 12), (nfr - 60, nfr - 2)):
            for lag_frames in (0, 1):
                rv = emu.call("audio_capture_summary", {"frame_from": first, "frame_to": last,
                                                        "lag_frames": lag_frames})["ref_vs_sdl"]
                if rv.get("status") != "ok":
                    ok = False
                    out.append(f"  FAIL: {label} frames [{first}, {last}] lag_frames={lag_frames}: {rv.get('status')}")
                    continue
                delay = rv["lag_samples"]
                lo = max(bounds[first], r0, s0 - delay)
                hi = min(bounds[last + 1], r1, s1 - delay, lo + _CAP)
                want = _ranking(bounds, first, last, lo, hi, ref, r0, sdl, s0, delay)
                capped += (hi - lo == _CAP)
                good = (rv["window"] == [bounds[first], bounds[last + 1]] and rv["from_sample"] == lo
                        and rv["compared_samples"] == hi - lo
                        and _same_ranking(want, rv["worst_frames"], rv.get("worst_frames_detail") or []))
                ok = ok and good
                out.append(f"  {'PASS' if good else 'FAIL'}: {label} frames [{first}, {last}] lag_frames={lag_frames}: "
                           f"{rv['compared_samples']} pairs from {rv['from_sample']} (expected {hi - lo} from {lo}), "
                           f"worst {rv['worst_frames']}, from PCM {[(f, round(r, 5), n) for f, r, n in want]}")
        good = capped >= 1
        ok = ok and good
        out.append(f"  {'PASS' if good else 'FAIL'}: {label}: {capped} window(s) at the 2 s limit")
        # 3: klient tvaru vydané verze
        rv = emu.call("audio_capture_summary", {"frame_from": 11, "frame_to": 11, "lag_frames": 0})["ref_vs_sdl"]
        try:
            pairs = [(frame, rms) for frame, rms in rv.get("worst_frames", [])]
            good = len(pairs) == 1 and pairs[0][0] == 11
            why = f"{pairs}"
        except ValueError as e:
            good, why = False, str(e)
        ok = ok and good
        out.append(f"  {'PASS' if good else 'FAIL'}: {label}: client 'for frame, rms in worst_frames': {why}")
    except Exception as e:  # noqa: BLE001 - výsledek vlákna (i chybějící pole odpovědi)
        ok = False
        out.append(f"  FAIL: {label}: {e!r}")
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    results[exe.stem] = (ok, out)


def main():
    """Spustí případy; 0 = PASS, 1 = FAIL, 77 = SKIP."""
    emu_test_proc.install(deadline_s=290)
    pal = _find_exe("mz800emu")
    if pal is None:
        print("SKIP: no mz800emu binary found")
        return 77
    exes = [pal] + [e for e in (_find_exe("mz1500emu"), _find_exe("mz700emu-ntsc")) if e is not None]
    results = {}
    threads = [threading.Thread(target=run, args=(e, results)) for e in exes]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    ok = len(results) == len(exes)
    for e in exes:
        good, lines = results.get(e.stem, (False, [f"  FAIL: {e.stem}: no result"]))
        print(f"\n=== {e.stem} ===")
        print("\n".join(lines))
        ok = ok and good
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
