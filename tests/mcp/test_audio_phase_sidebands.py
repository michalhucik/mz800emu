#!/usr/bin/env python3
"""E2E test: stálý tón ve zvukovém výstupu (sdl.wav) nemá postranní složky z délky bloků (MCP pipe).

Zvukový výstup dělí zvuk na bloky jednoho snímku. Snímek trvá podle krystalu
zlomkový počet vzorků (PAL 881,3597, NTSC 735,9476), bloky mají proto střídavě
dolní a horní celou část. Pokud by se čas prvního vzorku zakládal znovu na
začátek každého bloku, fáze tónu by se periodicky posouvala podle zbytku
akumulátoru a stálý tón by dostal postranní složky na odstupu
``|44100 * takty / krystal - round(...)| * krystal / takty`` (PAL 17,997 Hz,
NTSC 3,139 Hz) od nosné - na větvi 589baade až -14,8 dBc.

Pro každý případ (PSG i CTC0, PAL i NTSC sestavení; chybějící binárka se
přeskočí, mz800emu je povinná) se v headless běhu při MAX SPEED zachytí asi
6 s stálého tónu do reference.wav a sdl.wav a ověří se:

  1  výška tónu v obou souborech do +-0,1 centu od kmitočtu podle krystalu
     (CTC0: krystal / 16 / N, PSG: krystal / dělič CPU / 32 / N), měřeno přes
     dlouhé okno jako v test_audio_crystal_pitch.py,
  2  počet vzorků sdl.wav = ``sdl_samples`` v capture.json a od
     ``sdl_blocks * 44100 * takty / krystal`` se liší méně než o 1 vzorek
     (bloky z akumulátoru bez driftu),
  3  silnější ze dvou prvních postranních složek (nosná +- odstup) v obou
     souborech pod SIDEBAND_LIMIT_DBC: Hannovo okno 4 s od 1. sekundy, amplituda
     Goertzelovým algoritmem,
  4  (jen CTC0) největší nereferenční čára v okolí nosné +-NEAR_HALF_HZ
     (3 Hz až 350 Hz od nosné) v sdl.wav pod NEAR_LIMIT_DBC: čára, která je
     v sdl.wav aspoň o NEAR_MARGIN_DB silnější než v reference.wav na stejném
     místě (+-1 Hz). Spektrum okolí nosné: Hannovo okno 4 s, posun nosné do
     nuly, dolní propust (čtyři klouzavé součty po 36 vzorcích, korigovaná
     v pásmu), decimace na 1225 Hz a FFT 8192 bodů (krok 0,15 Hz), vše
     v čistém Pythonu. Hlídá převzorkování CTC0 do buněk po 32 taktech:
     hranice vzorku zaokrouhlená na buňku dávala se vzorem 12/13 (NTSC 10/11)
     buněk na vzorek čáry na pevném odstupu (PAL +-272,6 Hz, NTSC
     +-293,7 Hz), u fc8151b4 -69,7 až -75,3 dBc pro N = 64 a 128.
     PSG se tu nehlídá: s bodovým vzorkováním PSG (do 2026-11-26) měl
     kanál PSG v okolí nosné aliasy kolem -69 dBc i ve výchozím stavu
     0677e23b; od průměru přes interval vzorku hlídá falešné čáry PSG
     test_audio_psg_bass.py.

Mez SIDEBAND_LIMIT_DBC = -60 dBc: reference.wav i sdl.wav výchozího stavu
0677e23b mají na těchto odstupech nejvýš -73,4 dBc (NTSC u -76 dBc omezuje
rozlišení okna 4 s, odstup 3,14 Hz je jen 12,6 binu), větev 589baade
-14,8 až -24,4 dBc; mez tak nechává 13 dB rezervy proti správnému výstupu
a 35 dB proti chybě.

Mez NEAR_LIMIT_DBC = -80 dBc: s krajními buňkami CTC0 váženými podílem
překryvu je největší nereferenční čára v okolí nosné v těchto osmi případech
-92,8 až -114,1 dBc (rezerva aspoň 12,8 dB). Větev fc8151b4 (hranice vzorku
zaokrouhlená na buňku) dává -40,2 až -62,3 dBc (nejsilnější jsou aliasy
hran obdélníku posunutých na buňku, čára vzoru 12/13 na +-272,6 Hz má u N = 64
-69,7 dBc), tedy aspoň 17,7 dB nad mezí. Výchozí stav 0677e23b měl podle
sondy externí revize (jiná metoda: Blackman-Harris, bez aliasů harmonických)
v okolí nosné u MZ-700 N = 64 -77,5 (PAL) a -76,8 dBc (NTSC); mez je
přísnější než výchozí stav, protože hlídá převzorkování CTC0 této větve.

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí
se jen vlastní PID. Binárky: kořen repa, nebo proměnná MZ_EMU_DIR. Výstup
anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import cmath
import json
import math
import shutil
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, check, _find_exe  # noqa: E402
from test_audio_crystal_pitch import (  # noqa: E402
    _prog_ctc0, _prog_psg, _read_left, _measure_hz, _cents, _capture_tone)

#: Vzorkovací kmitočet zachycení.
_RATE = 44100
#: Mez silnější postranní složky [dBc] (zdůvodnění v hlavičce).
SIDEBAND_LIMIT_DBC = -60.0
#: Tolerance výšky tónu [cent].
_TOL_CENTS = 0.1
#: Začátek a délka okna spektra [s].
_WIN_START_S = 1.0
_WIN_LEN_S = 4.0
#: Délka zachycení tónu [s].
_TONE_S = 6
#: Mez největší nereferenční čáry v okolí nosné (jen CTC0) [dBc] (zdůvodnění v hlavičce).
NEAR_LIMIT_DBC = -80.0
#: Polovina šířky hlídaného okolí nosné [Hz].
NEAR_HALF_HZ = 350.0
#: Čára v okolí nosné blíž než tolik k nosné se nehodnotí (hlavní lalok okna) [Hz].
NEAR_MIN_HZ = 3.0
#: O kolik musí být čára v sdl.wav silnější než v reference.wav, aby byla nereferenční [dB].
NEAR_MARGIN_DB = 6.0
#: Decimace okolí nosné (1225 Hz po posunu nosné do nuly).
_NEAR_DECIM = 36
#: Délka FFT okolí nosné (krok 1225 / 8192 = 0,15 Hz).
_NEAR_FFT = 8192

#: Případy: binárka, zdroj, dělič N.
_CASES = (
    ("mz800emu", "PSG", 28),
    ("mz800emu", "CTC0", 256),
    ("mz800emu", "CTC0", 64),
    ("mz1500emu", "PSG", 28),
    ("mz1500emu", "CTC0", 128),
    ("mz1500emu", "CTC0", 64),
    ("mz700emu-pal", "CTC0", 256),
    ("mz700emu-pal", "CTC0", 64),
    ("mz700emu-ntsc", "CTC0", 128),
    ("mz700emu-ntsc", "CTC0", 64),
)


def _fft(a):
    """Komplexní FFT (radix 2, délka mocnina dvou) v čistém Pythonu; vrátí nový seznam."""
    n = len(a)
    a = list(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        half = size // 2
        tw = [w ** k for k in range(half)]
        for st in range(0, n, size):
            for k in range(half):
                u = a[st + k]
                v = a[st + k + half] * tw[k]
                a[st + k] = u + v
                a[st + k + half] = u - v
        size *= 2
    return a


def _near_spectrum(x, carrier):
    """Amplitudové spektrum okolí nosné: seznam (odstup od nosné [Hz], amplituda), korigované o propust."""
    start, size = int(_WIN_START_S * _RATE), int(_WIN_LEN_S * _RATE)
    if len(x) < start + size:
        raise TestFailure(f"capture too short: {len(x)} samples")
    step = cmath.exp(-2j * math.pi * carrier / _RATE)
    rot = 1.0 + 0j
    z = []
    for i, v in enumerate(x[start:start + size]):
        if i % 4096 == 0:
            rot = cmath.exp(-2j * math.pi * carrier * i / _RATE)   # bez nahromadění chyby
        z.append(v * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / (size - 1))) * rot)
        rot *= step
    d = _NEAR_DECIM
    for _ in range(4):                          # čtyři klouzavé součty délky d
        acc = 0j
        out = []
        for i, v in enumerate(z):
            acc += v
            if i >= d:
                acc -= z[i - d]
            out.append(acc)
        z = out
    y = z[d * 4::d]
    y = y + [0j] * (_NEAR_FFT - len(y)) if len(y) < _NEAR_FFT else y[:_NEAR_FFT]
    spec = _fft(y)
    fs = _RATE / d
    res = []
    for k, c in enumerate(spec):
        off = (k if k < _NEAR_FFT // 2 else k - _NEAR_FFT) * fs / _NEAR_FFT
        if abs(off) > NEAR_HALF_HZ + 2.0:
            continue
        t = math.pi * off / _RATE
        h = 1.0 if t == 0 else (math.sin(t * d) / (d * math.sin(t)))
        res.append((off, abs(c) / max(abs(h) ** 4, 1e-9)))
    res.sort()
    return res


def _near_nonref_dbc(sdl, ref, carrier):
    """Největší nereferenční čára sdl v okolí nosné: (dBc, odstup [Hz], ref dBc) nebo None."""
    s = _near_spectrum(sdl, carrier)
    r = _near_spectrum(ref, carrier)
    cs = max(a for o, a in s if abs(o) < 2.0)
    cr = max(a for o, a in r if abs(o) < 2.0)
    ds = [20.0 * math.log10(max(a / cs, 1e-15)) for _, a in s]
    dr = [20.0 * math.log10(max(a / cr, 1e-15)) for _, a in r]
    offs = [o for o, _ in s]
    best = None
    for i in range(1, len(ds) - 1):
        o = offs[i]
        if not (NEAR_MIN_HZ < abs(o) <= NEAR_HALF_HZ) or ds[i] < ds[i - 1] or ds[i] < ds[i + 1]:
            continue
        lo, hi = i, i
        while lo > 0 and offs[lo - 1] >= o - 1.0:
            lo -= 1
        while hi < len(dr) - 1 and offs[hi + 1] <= o + 1.0:
            hi += 1
        rmax = max(dr[lo:hi + 1])
        if ds[i] - rmax >= NEAR_MARGIN_DB and (best is None or ds[i] > best[0]):
            best = (ds[i], o, rmax)
    return best


def _amplitude(x, hz):
    """Amplituda jedné frekvence (Goertzel) ve vzorcích `x`."""
    c = 2.0 * math.cos(2.0 * math.pi * hz / _RATE)
    a = b = 0.0
    for v in x:
        a, b = v + c * a - b, a
    return math.sqrt(max(a * a + b * b - c * a * b, 0.0))


def _sidebands_dbc(x, carrier, offset):
    """Postranní složky nosná - offset a nosná + offset vůči nosné [dBc] (Hannovo okno)."""
    start, size = int(_WIN_START_S * _RATE), int(_WIN_LEN_S * _RATE)
    if len(x) < start + size:
        raise TestFailure(f"capture too short: {len(x)} samples")
    w = [v * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / (size - 1))) for i, v in enumerate(x[start:start + size])]
    main = _amplitude(w, carrier)
    if main <= 1.0:
        raise TestFailure(f"no carrier at {carrier:.4f} Hz")
    return [20.0 * math.log10(max(_amplitude(w, carrier + s * offset) / main, 1e-12)) for s in (-1, 1)]


def run_case(exe, source, n):
    """Jeden případ; vrátí True = vše prošlo."""
    tmp = Path(tempfile.mkdtemp(prefix="mztest-sideband-"))
    emu = PipeEmu(exe, tmp)
    ok = True
    try:
        info = emu.call("get_platform_info")
        crystal = info["clocks"]["gdg_real_base_hz"]
        ticks = info["scanline"]["screen_total_ticks_per_frame"]
        fps = info["scanline"]["screens_per_sec"]
        ratio = _RATE * ticks / crystal          # vzorků na snímek (zlomek)
        offset = abs(ratio - round(ratio)) * crystal / ticks
        if source == "CTC0":
            carrier = crystal / 16.0 / n
            prog = _prog_ctc0(n)
        else:
            carrier = crystal / info["clocks"]["cpu_divider"] / 32.0 / n
            prog = _prog_psg(n)
        emu.call("set_speed", {"mode": "max"})
        paths = _capture_tone(emu, tmp, "tone", prog, _TONE_S * fps)
        label = f"{exe.stem} {source} N={n}"
        meta = json.loads((tmp / "tone" / "capture.json").read_text(encoding="utf-8"))
        for path in paths:
            x = _read_left(path)
            # 1. výška tónu
            m = _measure_hz(x, carrier)
            if m is None:
                print(f"  FAIL: {label} {path.name}: not enough samples for the pitch")
                ok = False
            else:
                dev = _cents(m[0], carrier)
                good = abs(dev) <= _TOL_CENTS
                print(f"  {'PASS' if good else 'FAIL'}: {label} {path.name}: pitch {m[0]:.4f} Hz, "
                      f"expected {carrier:.4f} Hz ({dev:+.3f} cent), tolerance +-{_TOL_CENTS} cent")
                ok = ok and good
            # 3. postranní složky
            lo, hi = _sidebands_dbc(x, carrier, offset)
            worst = max(lo, hi)
            good = worst < SIDEBAND_LIMIT_DBC
            print(f"  {'PASS' if good else 'FAIL'}: {label} {path.name}: sidebands at +-{offset:.5f} Hz "
                  f"lower {lo:.2f} dBc, upper {hi:.2f} dBc, limit {SIDEBAND_LIMIT_DBC:.0f} dBc")
            ok = ok and good
        # 4. nereferenční čáry v okolí nosné (CTC0)
        if source == "CTC0":
            xs, xr = _read_left(paths[1]), _read_left(paths[0])
            nn = _near_nonref_dbc(xs, xr, carrier)
            good = nn is None or nn[0] < NEAR_LIMIT_DBC
            txt = "none" if nn is None else f"{nn[0]:.1f} dBc at {nn[1]:+.2f} Hz (reference {nn[2]:.1f} dBc)"
            print(f"  {'PASS' if good else 'FAIL'}: {label} sdl.wav: largest non-reference line within "
                  f"+-{NEAR_HALF_HZ:.0f} Hz of the carrier: {txt}, limit {NEAR_LIMIT_DBC:.0f} dBc")
            ok = ok and good
        # 2. počet vzorků SDL cesty
        with wave.open(str(paths[1]), "rb") as w:
            nwav = w.getnframes()
        blocks, samples = meta["sdl_blocks"], meta["sdl_samples"]
        want = blocks * ratio
        good = (nwav == samples) and abs(samples - want) < 1.0
        print(f"  {'PASS' if good else 'FAIL'}: {label} sdl.wav: {nwav} samples, capture.json {samples} "
              f"in {blocks} blocks, expected {want:.2f} (+-1)")
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
    for name, source, n in _CASES:
        exe = _find_exe(name)
        if exe is None:
            print(f"\n=== {name} {source}: binary not found, skipped ===")
            continue
        print(f"\n=== {name} {source} N={n} ===")
        try:
            ok = run_case(exe, source, n) and ok
        except TestFailure as e:
            print(f"  FAIL: {e}")
            ok = False
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
