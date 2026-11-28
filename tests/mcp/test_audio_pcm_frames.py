#!/usr/bin/env python3
"""E2E test: výřezy emu_audio_capture_pcm po snímcích na sebe navazují (MCP pipe).

Snímek má podle krystalu zlomkový počet vzorků (PAL 881,36, NTSC 735,95).
Výřez `frame_from=f, frames=k` musí začínat a končit na skutečných indexech
zvoleného proudu (reference nebo SDL cesta) na hranicích snímků zachycení,
jinak výřezy po jednom snímku vynechávají nebo zdvojují vzorky (větev
589baade: na MZ-700 NTSC chyběl vzorek 1470 a 9 dalších z 11 přechodů).

Pro každou nalezenou binárku (mz800emu povinně, mz1500emu, mz700emu-pal,
mz700emu-ntsc volitelně) při 100 % a pro mz800emu i při vlastní rychlosti
50 % a 200 % (snímek zachycení = synchronizační událost; SDL cesta dodá
jeden blok, reference vzorky emulovaného času) se zachytí tón CTC0
a pro `source` reference i sdl ověří:

  1  12 výřezů `frame_from=0..11, frames=1` (44100 Hz, stereo, pcm_b64)
     složených za sebou je bajt po bajtu totéž co jeden výřez
     `frame_from=0, frames=12`; každý další výřez začíná tam, kde předchozí
     skončil (`from_sample` = předchozí `next_sample`), žádný není zkrácený,
  2  data výřezů jsou stejný úsek WAV souboru (reference.wav, sdl.wav).

Navíc okraj ringu souhrnů snímků (mz700emu-ntsc, jinak mz800emu): zachycení
s `memory_minutes=1` a `pcm_seconds=120` přes 72 s při MAX SPEED, takže ring
souhrnů nejstarší snímky přepíše a PCM je ještě drží. Výřezy po jednom
snímku od 4 snímků před nejstarším souhrnem do 4 za ním (před okrajem je
hranice odhad podle krystalu zakotvený k nejstaršímu souhrnu) musí navazovat
a souhlasit s WAV, v obou proudech; na fc8151b4 chyběl na okraji 1 vzorek
proudu sdl.

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí
se jen vlastní PID. Binárky: kořen repa, nebo proměnná MZ_EMU_DIR. Výstup
anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import base64
import shutil
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, _find_exe  # noqa: E402
from test_audio_crystal_pitch import _prog_ctc0  # noqa: E402

#: Počet výřezů po snímcích.
_SLICES = 12
#: Případy: binárka, rychlost [%].
_CASES = (
    ("mz800emu", 100), ("mz1500emu", 100), ("mz700emu-pal", 100), ("mz700emu-ntsc", 100),
    ("mz800emu", 50), ("mz800emu", 200),
)


def _pcm(emu, source, frame_from, frames):
    """Výřez PCM 44100 Hz stereo; vrátí (from_sample, next_sample, data)."""
    q = emu.call("audio_capture_pcm", {"source": source, "frame_from": frame_from, "frames": frames,
                                       "rate": 44100, "stereo": True, "encoding": "pcm_b64",
                                       "max_bytes": 4194304})
    if q["truncated"]:
        raise TestFailure(f"{source} frame {frame_from}: unexpected truncation")
    data = base64.b64decode(q["data_b64"])
    if len(data) != 4 * q["samples"] or q["next_sample"] - q["from_sample"] != q["samples"]:
        raise TestFailure(f"{source} frame {frame_from}: inconsistent response")
    return q["from_sample"], q["next_sample"], data


def run_case(exe, speed):
    """Jeden případ; vrátí True = vše prošlo."""
    tmp = Path(tempfile.mkdtemp(prefix="mztest-pcmframes-"))
    emu = PipeEmu(exe, tmp)
    ok = True
    try:
        emu.call("pause")
        emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
        emu.call("mem_write", {"addr": 0x3000, "data_hex": _prog_ctc0(2519).hex()})
        emu.call("set_register", {"reg": "PC", "value": 0x3000})
        if speed != 100:
            emu.call("set_speed", {"mode": "custom", "percent": speed})
        emu.call("run", {"frames": 8}, timeout=60.0)
        emu.call("audio_capture_start", {"dir": (tmp / "cap").as_posix(), "events": False, "psg_writes": False,
                                         "pcm": ["reference", "sdl"]})
        emu.call("run", {"frames": 38}, timeout=60.0)
        emu.call("audio_capture_stop")
        for source, fname in (("reference", "reference.wav"), ("sdl", "sdl.wav")):
            with wave.open(str(tmp / "cap" / fname), "rb") as w:
                raw = w.readframes(w.getnframes())
            parts, ranges = [], []
            for f in range(_SLICES):
                a, b, data = _pcm(emu, source, f, 1)
                if raw[4 * a:4 * b] != data:
                    raise TestFailure(f"{source} frame {f}: data differ from {fname} [{a}, {b})")
                parts.append(data)
                ranges.append((a, b))
            a, b, whole = _pcm(emu, source, 0, _SLICES)
            gaps = [(m[1], n[0]) for m, n in zip(ranges, ranges[1:]) if m[1] != n[0]]
            same = b"".join(parts) == whole and ranges[0][0] == a and ranges[-1][1] == b
            good = same and not gaps and raw[4 * a:4 * b] == whole
            label = f"{exe.stem} {speed} % {source}"
            print(f"  {'PASS' if good else 'FAIL'}: {label}: {_SLICES} frame slices {ranges[:3]}... "
                  f"joined {'==' if same else '!='} one slice [{a}, {b}), discontinuities {gaps}")
            ok = ok and good
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def run_ring(exe):
    """Okraj ringu souhrnů snímků (viz hlavička); vrátí True = prošlo."""
    tmp = Path(tempfile.mkdtemp(prefix="mztest-pcmring-"))
    emu = PipeEmu(exe, tmp)
    ok = True
    try:
        fps = emu.call("get_platform_info")["scanline"]["screens_per_sec"]
        emu.call("pause")
        emu.call("set_cpu_flags", {"iff1": False, "iff2": False})
        emu.call("mem_write", {"addr": 0x3000, "data_hex": _prog_ctc0(2519).hex()})
        emu.call("set_register", {"reg": "PC", "value": 0x3000})
        emu.call("set_speed", {"mode": "max"})
        emu.call("run", {"frames": 8}, timeout=60.0)
        emu.call("audio_capture_start", {"dir": (tmp / "cap").as_posix(), "events": False, "psg_writes": False,
                                         "pcm": ["reference", "sdl"], "memory_minutes": 1, "pcm_seconds": 120})
        left = 72 * fps
        while left > 0:
            k = min(left, 1000)
            emu.call("run", {"frames": k}, timeout=240.0)
            emu.call("pause")
            left -= k
        emu.call("audio_capture_stop")
        f0 = emu.call("audio_capture_status")["memory"]["streams"]["frames"]["first_seq"]
        if f0 < 8:
            raise TestFailure(f"frame summary ring was not overwritten (first_seq {f0})")
        for source, fname in (("reference", "reference.wav"), ("sdl", "sdl.wav")):
            with wave.open(str(tmp / "cap" / fname), "rb") as w:
                raw = w.readframes(w.getnframes())
            ranges = []
            same_wav = True
            for f in range(f0 - 4, f0 + 4):
                a, b, data = _pcm(emu, source, f, 1)
                same_wav = same_wav and raw[4 * a:4 * b] == data
                ranges.append((f, a, b))
            gaps = [(m[0], m[2], n[1]) for m, n in zip(ranges, ranges[1:]) if m[2] != n[1]]
            good = same_wav and not gaps
            print(f"  {'PASS' if good else 'FAIL'}: {exe.stem} ring edge {source}: oldest summary frame {f0}, "
                  f"windows {ranges[3:6]}, discontinuities (frame, end, next start) {gaps}, data == WAV {same_wav}")
            ok = ok and good
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main():
    """Spustí všechny případy nalezených binárek; 0 = PASS, 1 = FAIL, 77 = SKIP."""
    emu_test_proc.install(deadline_s=290)
    if _find_exe("mz800emu") is None:
        print("SKIP: no mz800emu binary found")
        return 77
    ok = True
    for name, speed in _CASES:
        exe = _find_exe(name)
        if exe is None:
            print(f"\n=== {name}: binary not found, skipped ===")
            continue
        print(f"\n=== {name} {speed} % ===")
        try:
            ok = run_case(exe, speed) and ok
        except TestFailure as e:
            print(f"  FAIL: {e}")
            ok = False
    ring_exe = _find_exe("mz700emu-ntsc") or _find_exe("mz800emu")
    print(f"\n=== {ring_exe.stem} frame summary ring edge ===")
    try:
        ok = run_ring(ring_exe) and ok
    except TestFailure as e:
        print(f"  FAIL: {e}")
        ok = False
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
