#!/usr/bin/env python3
"""E2E test: výška tónu ve zachyceném zvuku odpovídá krystalu skutečného počítače (MCP pipe).

Emulátor běží celý počet taktů na snímek, ale skutečný čas počítá z kmitočtu
krystalu (17 734 475 Hz PAL, 14 318 180 Hz NTSC): snímků je 50,036326 nebo
59,922743 za sekundu. Tón ve zvukovém výstupu (sdl.wav) i ve zvuku spočteném
v emulovaném čase (reference.wav) proto musí mít kmitočet skutečného stroje.

Pro každou nalezenou binárku (mz800emu povinně, mz1500emu, mz700emu-pal,
mz700emu-ntsc volitelně):

  1  CTC0 v režimu 3 s děličem N (PAL 2519, NTSC 2034), PC0 = 1, GATE0 = 1
     (8253 a 8255 na 0E003h až 0E008h): tón krystal / 16 / N
     (MZ-800: 440,0177 Hz, MZ-1500: 439,9637 Hz) v obou souborech do
     +-0,1 centu,
  2  (jen se zvukovým čipem PSG) CTC0 umlčený (PC0 = 0, GATE0 = 0; ROM
     MZ-800 po resetu pípá), kanál 0 PSG0 s děličem 252 (port 0F2h):
     tón krystal / dělič CPU / 32 / 252 v reference.wav do +-0,1 centu
     a v sdl.wav do +-PSG_SDL_TOL_CENTS (SDL cesta průměruje kanál PSG
     přes interval vzorku jako renderer; mez beze změny).

Kmitočet se měří přes dlouhé okno (asi 4 s, přes 1700 period): náběžné
průchody nulou po odečtení klouzavého průměru přes jednu periodu, čas
průchodu lineární interpolací mezi vzorky, kmitočet = (počet průchodů - 1) /
(čas posledního - čas prvního). Začátek tónu (doznívání střídavých vazeb) se
vynechá.

Proměnná prostředí ``PITCH_REPORT_ONLY=1`` jen vypíše naměřené hodnoty
(bez vyhodnocení) - pro měření stavu před opravou.

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí
se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import math
import os
import shutil
import struct
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, check, _find_exe  # noqa: E402

#: Adresa programu v RAM.
_PROG = 0x3000
#: Délka tónu v snímcích (asi 5 s).
_TONE_FRAMES_PAL = 250
_TONE_FRAMES_NTSC = 300
#: Vynechaný začátek tónu [s] (rozjezd a doznívání střídavých vazeb).
_SKIP_S = 0.8
#: Délka měřicího okna [s].
_WINDOW_S = 4.0
#: Tolerance výšky tónu [cent].
_TOL_CENTS = 0.1
#: Tolerance PSG v sdl.wav [cent] (mez z doby bodového vzorkování PSG, beze změny).
PSG_SDL_TOL_CENTS = 0.1
#: Vzorkovací kmitočet zachycení.
_WAV_RATE = 44100

#: Platformy: binárka, krystal [Hz], dělič CPU, dělič CTC0 N, má PSG.
_PLATFORMS = (
    ("mz800emu", 17734475, 5, 2519, True),
    ("mz1500emu", 14318180, 4, 2034, True),
    ("mz700emu-pal", 17734475, 5, 2519, False),
    ("mz700emu-ntsc", 14318180, 4, 2034, False),
)

#: Dělič tónu PSG.
_PSG_DIV = 252


def _prog_ctc0(n):
    """DI; PC0 = 1; GATE0 = 1; CT0 režim 3, LSB a MSB; N; JR $."""
    return bytes.fromhex(
        "F3" "3E01" "3203E0" "3E01" "3208E0" "3E36" "3207E0"
        f"3E{n & 0xFF:02X}" "3204E0" f"3E{n >> 8:02X}" "3204E0" "18FE")


def _prog_psg(div):
    """DI; PC0 = 0, GATE0 = 0 (ticho CTC0, ROM MZ-800 po resetu pípá);
    PSG0 kanál 0: dělič `div`, útlum 0 (port 0F2h); JR $."""
    return bytes.fromhex(
        "F3" "3E00" "3203E0" "AF" "3208E0"
        f"3E{0x80 | (div & 0x0F):02X}" "D3F2" f"3E{div >> 4:02X}" "D3F2"
        "3E90" "D3F2" "18FE")


def _read_left(path):
    """Levý kanál WAV (16 bit stereo) jako seznam int."""
    with wave.open(str(path), "rb") as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2 or w.getframerate() != _WAV_RATE:
            raise TestFailure(f"{path.name}: unexpected WAV format")
        raw = w.readframes(w.getnframes())
    n = len(raw) // 4
    return [v for i, v in enumerate(struct.unpack(f"<{n * 2}h", raw)) if i % 2 == 0]


def _measure_hz(x, f_guess, skip_s=_SKIP_S, window_s=_WINDOW_S):
    """Kmitočet tónu přes dlouhé okno (náběžné průchody nulou).

    @param x        Vzorky (44100 Hz).
    @param f_guess  Přibližný kmitočet (délka klouzavého průměru = jedna perioda).
    @return (kmitočet [Hz], počet period); None, když okno nestačí.
    """
    w = max(2, int(round(_WAV_RATE / f_guess)))
    a = int(skip_s * _WAV_RATE)
    b = a + int(window_s * _WAV_RATE)
    if b + w > len(x):
        return None
    # odečtení klouzavého průměru přes jednu periodu (střed okna)
    pref = [0]
    for v in x:
        pref.append(pref[-1] + v)
    half = w // 2

    def y(i):
        lo, hi = i - half, i - half + w
        return x[i] - (pref[hi] - pref[lo]) / w

    times = []
    prev = y(a)
    for i in range(a + 1, b):
        cur = y(i)
        if prev < 0.0 <= cur:
            times.append((i - 1) + (-prev) / (cur - prev))
        prev = cur
    if len(times) < 10:
        return None
    periods = len(times) - 1
    return periods * _WAV_RATE / (times[-1] - times[0]), periods


def _cents(f, ref):
    """Odchylka kmitočtu `f` od `ref` v centech."""
    return 1200.0 * math.log2(f / ref)


def _capture_tone(emu, tmp, name, prog, frames):
    """Zachytí zvuk programu `prog`; vrátí cesty (reference.wav, sdl.wav)."""
    cap = tmp / name
    emu.call("reset")
    emu.call("pause")
    emu.call("audio_capture_start", {"dir": cap.as_posix(), "events": False, "psg_writes": False,
                                     "reference_wav": True, "sdl_wav": True, "pcm": []})
    emu.call("run", {"frames": 10}, timeout=60.0)   # začátek zachycení + rozjezd SDL cesty
    emu.call("pause")
    emu.call("mem_write", {"addr": _PROG, "data_hex": prog.hex()})
    emu.call("set_register", {"reg": "PC", "value": _PROG})
    emu.call("run", {"frames": frames}, timeout=120.0)
    emu.call("pause")
    emu.call("audio_capture_stop")
    return cap / "reference.wav", cap / "sdl.wav"


def _check_tone(label, paths, expect, tol_ref, tol_sdl, report_only):
    """Změří tón v obou souborech a porovná s očekávaným kmitočtem."""
    ok = True
    for (src, path), tol in zip((("reference.wav", paths[0]), ("sdl.wav", paths[1])), (tol_ref, tol_sdl)):
        m = _measure_hz(_read_left(path), expect)
        if m is None:
            print(f"  FAIL: {label} {src}: not enough samples for the measurement")
            ok = False
            continue
        f, periods = m
        dev = _cents(f, expect)
        line = f"{label} {src}: {f:.4f} Hz over {periods} periods, expected {expect:.4f} Hz ({dev:+.3f} cent)"
        if report_only:
            print("  " + line)
            continue
        good = abs(dev) <= tol
        print(f"  {'PASS' if good else 'FAIL'}: {line}, tolerance +-{tol} cent")
        ok = ok and good
    return ok


def run_platform(exe, crystal, cpu_div, n, have_psg, report_only):
    """Měření jedné platformy; vrátí True = vše prošlo."""
    ntsc = crystal == 14318180
    frames = _TONE_FRAMES_NTSC if ntsc else _TONE_FRAMES_PAL
    tmp = Path(tempfile.mkdtemp(prefix="mztest-pitch-"))
    emu = PipeEmu(exe, tmp)
    ok = True
    try:
        info = emu.call("get_platform_info")
        check(info["clocks"]["gdg_real_base_hz"] == crystal,
              f"{exe.name}: crystal {crystal} Hz (platform info)")
        check(info["clocks"]["ctc0_divider"] == 16, f"{exe.name}: CTC0 divider 16")
        expect = crystal / 16.0 / n
        paths = _capture_tone(emu, tmp, "ctc0", _prog_ctc0(n), frames)
        ok = _check_tone(f"{exe.name} CTC0 N={n}", paths, expect, _TOL_CENTS, _TOL_CENTS, report_only) and ok
        if have_psg:
            expect = crystal / cpu_div / 32.0 / _PSG_DIV
            paths = _capture_tone(emu, tmp, "psg", _prog_psg(_PSG_DIV), frames)
            ok = _check_tone(f"{exe.name} PSG divider {_PSG_DIV}", paths, expect, _TOL_CENTS,
                             PSG_SDL_TOL_CENTS, report_only) and ok
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main():
    """Spustí měření pro nalezené binárky; 0 = PASS, 1 = FAIL, 77 = SKIP."""
    emu_test_proc.install(deadline_s=290)
    report_only = os.environ.get("PITCH_REPORT_ONLY") == "1"
    if _find_exe("mz800emu") is None:
        print("SKIP: no mz800emu binary found")
        return 77
    ok = True
    for name, crystal, cpu_div, n, have_psg in _PLATFORMS:
        exe = _find_exe(name)
        if exe is None:
            print(f"\n=== {name}: binary not found, skipped ===")
            continue
        print(f"\n=== {name} ===")
        try:
            ok = run_platform(exe, crystal, cpu_div, n, have_psg, report_only) and ok
        except TestFailure as e:
            print(f"  FAIL: {e}")
            ok = False
    if report_only:
        print("\nRESULT: report only")
        return 0
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
