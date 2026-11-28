#!/usr/bin/env python3
"""E2E test: vysoký tón PSG nemá ve zvukovém výstupu (sdl.wav) falešný bas (MCP pipe).

SDL cesta dřív brala z logu kanálu PSG jeden bod na výstupní vzorek. Harmonické
obdélníku nad Nyquistovým kmitočtem se tím překládaly do slyšitelného pásma:
na MZ-1500 s děličem 28 (3 995 Hz) vznikala 11. harmonickou čára 154,69 Hz
o 94,9 % amplitudy nosné (-0,46 dBc), v reference.wav ze stejného běhu
-49,4 dBc. Nově se výchylka kanálu průměruje přes interval vzorku jako
v rendereru reference.wav.

Pro každý případ (MZ-800 a MZ-1500, kanál 0 PSG0; chybějící mz1500emu se
přeskočí, mz800emu je povinná) se při MAX SPEED zachytí asi 7,5 s tónu do
reference.wav a sdl.wav, a to v PHASES fázích mřížky bloků SDL cesty: mezi
zachyceními běží krátké vložené zachycení (1 + 5 k mod 13 snímků), takže
akumulátor délky bloků a poloha konce snímku vůči krokům PSG při dalším
zachycení vychází jinak. V levém kanálu, okno 3 s od 4. sekundy (za
doznívání střídavých vazeb po resetu, časová konstanta výstupu 0,57 s), se
změří:

  1  nejsilnější předpovězená čára v basovém pásmu 20 až 1000 Hz:
     přeložené liché harmonické h * f (h <= 199) na |h * f - m * 44100|,
     amplituda Goertzelovým algoritmem s Hannovým oknem, vůči nosné [dBc],
  2  celkový výkon pásma 20 až 1000 Hz vůči výkonu nosné [dB]: horní propust
     2. řádu 20 Hz a Butterworthova dolní propust 8. řádu 1000 Hz (bilineární
     transformace, čtyři bikvady), RMS výstupu; nosná nad 2,4 kHz je
     potlačena aspoň o 60 dB.

Posuzuje se nejhorší fáze. Posloupnost fází je v jednom procesu
deterministická (každý běh testu zkouší stejných 6 fází); jejich počet
a rezervy mezí jsou zvolené podle měření přes víc fází. Meze (sdl.wav proti reference.wav ze stejného běhu):

  - děliče 28, 33, 38 a 45 (nosná 2,4 až 4 kHz) - oblast, kde má být výstup na
    úrovni reference: čára i pásmo nejvýš o LIMIT_DB = 6 dB nad referencí.
    Změřeno po průměru PSG nejvýš +1,9 dB (6 fází testu; 8 fází jinou metodou
    +0,2 dB, revize ve 12 fázích +1,1 dB), bodové vzorkování dávalo +38 až
    +60 dB;
  - dělič 20 (5,5 kHz) - oblast se zbytkovou chybou: log PSG snímku zná
    hodnoty jen do posledního kroku PSG, poslední výstupní vzorek bloku proto
    může mít až 79 taktů (MZ-1500 63) držené hodnoty; podle fáze to dává
    v pásmu 0 až +5,4 dB (MZ-800) a 0 až +4,0 dB (MZ-1500) nad referencí
    a jednotlivou čáru až +9 dB (model tools/rev30/psg_tail_model.py totéž
    potvrzuje). Hlídá se jen výkon pásma s mezí RESIDUAL_LIMIT_DB = 12 dB:
    rezerva 6 dB nad nejhorší změřenou fází, a bodové vzorkování (+38 až
    +49 dB) test dál shodí. Čára se jen vypisuje.

Reference je digitální kontrola převodu (integrál přes interval vzorku bez
blokového zpracování SDL cesty). Absolutní úroveň basu reference (-43 až
-65 dBc) určuje průměr přes interval vzorku (sinc), ne test.

Proces běží izolovaně (dočasný --cfg-dir a --work-dir, --no-save-ini); zabíjí
se jen vlastní PID. Binárky: kořen repa, nebo proměnná MZ_EMU_DIR. Výstup
anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import math
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_gdg_cksw import PipeEmu, TestFailure, _find_exe  # noqa: E402
from test_audio_crystal_pitch import _capture_tone, _prog_psg, _read_left  # noqa: E402

#: Mez: o kolik dB smí být bas v sdl.wav nad reference.wav (oblast na úrovni reference).
LIMIT_DB = 6.0
#: Mez výkonu pásma pro dělič se zbytkovou chybou drženého konce snímku (viz hlavička).
RESIDUAL_LIMIT_DB = 12.0
#: Počet fází mřížky bloků na případ.
PHASES = 6
#: Vzorkovací kmitočet.
_RATE = 44100
#: Okno měření [s] (za doznívání střídavých vazeb po resetu).
_WIN_START_S, _WIN_LEN_S = 4.0, 3.0
#: Délka zachycení [s].
_CAPTURE_S = 7.5
#: Basové pásmo [Hz].
_BAND = (20.0, 1000.0)
#: Děliče tónu PSG: (dělič, mez [dB], hlídat čáru).
_CASES = ((28, LIMIT_DB, True), (33, LIMIT_DB, True), (38, LIMIT_DB, True), (45, LIMIT_DB, True),
          (20, RESIDUAL_LIMIT_DB, False))
#: Binárky: jméno, krystal [Hz], dělič CPU.
_PLATFORMS = (("mz800emu", 17734475, 5), ("mz1500emu", 14318180, 4))


def _biquad(x, b0, b1, b2, a1, a2):
    """Bikvadratický filtr (přímá forma I) nad seznamem; vrátí nový seznam."""
    y = []
    x1 = x2 = y1 = y2 = 0.0
    for v in x:
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y.append(o)
    return y


def _rbj(kind, f0, q):
    """Koeficienty RBJ (kind 'lp' nebo 'hp') normované na a0."""
    w = 2.0 * math.pi * f0 / _RATE
    c, s = math.cos(w), math.sin(w)
    alpha = s / (2.0 * q)
    a0 = 1.0 + alpha
    if kind == "lp":
        b = ((1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0)
    else:
        b = ((1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0)
    return (b[0] / a0, b[1] / a0, b[2] / a0, -2.0 * c / a0, (1.0 - alpha) / a0)


def _band_power_db(x, carrier_amp):
    """Výkon pásma 20 až 1000 Hz vůči výkonu nosné (amplituda `carrier_amp`) [dB]."""
    lead = int(0.5 * _RATE)   # filtry se ustálí za necelých 0,1 s
    a0 = int(_WIN_START_S * _RATE) - lead
    x = x[a0:a0 + lead + int(_WIN_LEN_S * _RATE)]
    y = _biquad(x, *_rbj("hp", _BAND[0], math.sqrt(0.5)))
    for k in range(1, 5):   # Butterworth 8. řádu: Q_k = 1 / (2 cos((2k - 1) pi / 16))
        y = _biquad(y, *_rbj("lp", _BAND[1], 1.0 / (2.0 * math.cos((2 * k - 1) * math.pi / 16.0))))
    seg = y[lead:]
    rms = math.sqrt(sum(v * v for v in seg) / len(seg))
    return 20.0 * math.log10(max(rms, 1e-9) / (carrier_amp / math.sqrt(2.0)))


def _amplitudes(x, freqs):
    """Amplitudy kmitočtů `freqs` v okně (Hann, Goertzel), přepočtené na amplitudu sinusovky."""
    a, n = int(_WIN_START_S * _RATE), int(_WIN_LEN_S * _RATE)
    if len(x) < a + n:
        raise TestFailure(f"capture too short: {len(x)} samples")
    w = [v * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1))) for i, v in enumerate(x[a:a + n])]
    out = []
    for hz in freqs:
        c = 2.0 * math.cos(2.0 * math.pi * hz / _RATE)
        s1 = s2 = 0.0
        for v in w:
            s1, s2 = v + c * s1 - s2, s1
        out.append(4.0 * math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0)) / n)
    return out


def _alias_lines(f):
    """Přeložené liché harmonické nosné `f` v basovém pásmu: seznam (h, kmitočet)."""
    lines = []
    for h in range(3, 200, 2):
        g = (h * f) % _RATE
        g = min(g, _RATE - g)
        if _BAND[0] <= g <= _BAND[1]:
            lines.append((h, g))
    return lines


def _measure(path, f):
    """(nosná, nejsilnější čára [dBc], její kmitočet, výkon pásma [dB vůči nosné])."""
    x = _read_left(path)
    lines = _alias_lines(f)
    amps = _amplitudes(x, [f] + [g for _, g in lines])
    car = amps[0]
    if car < 10.0:
        raise TestFailure(f"{path.name}: no carrier at {f:.2f} Hz")
    best, best_hz = -200.0, 0.0
    for (h, g), amp in zip(lines, amps[1:]):
        d = 20.0 * math.log10(max(amp, 1e-9) / car)
        if d > best:
            best, best_hz = d, g
    return car, best, best_hz, _band_power_db(x, car)


def run_platform(exe, crystal, cpu_div):
    """Všechny případy jedné binárky ve PHASES fázích; vrátí True = nejhorší fáze každého případu v mezi."""
    tmp = Path(tempfile.mkdtemp(prefix="mztest-psgbass-"))
    emu = PipeEmu(exe, tmp)
    worst = {}
    try:
        fps = emu.call("get_platform_info")["scanline"]["screens_per_sec"]
        emu.call("set_speed", {"mode": "max"})
        for k in range(PHASES):
            for n, _, _ in _CASES:
                f = crystal / cpu_div / 32.0 / n
                ref_path, sdl_path = _capture_tone(emu, tmp, f"n{n}_{k}", _prog_psg(n), int(_CAPTURE_S * fps))
                _, rl, rhz, rb = _measure(ref_path, f)
                _, sl, shz, sb = _measure(sdl_path, f)
                shutil.rmtree(tmp / f"n{n}_{k}", ignore_errors=True)
                w = worst.get(n)
                if w is None or (sl - rl) > w[0]:
                    w = (sl - rl, sl, shz, rl, rhz) + (w[5:] if w else (None, None, None))
                if w[5] is None or (sb - rb) > w[5]:
                    w = w[:5] + (sb - rb, sb, rb)
                worst[n] = w
            # posun fáze mřížky bloků: krátké vložené zachycení
            _capture_tone(emu, tmp, f"shift{k}", _prog_psg(28), 1 + (k * 5) % 13)
            shutil.rmtree(tmp / f"shift{k}", ignore_errors=True)
    finally:
        emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    ok = True
    for n, limit, check_line in _CASES:
        dl, sl, shz, rl, rhz, db, sb, rb = worst[n]
        f = crystal / cpu_div / 32.0 / n
        good = db <= limit and (dl <= limit or not check_line)
        ok = ok and good
        print(f"  {'PASS' if good else 'FAIL'}: {exe.stem} PSG N={n} ({f:.2f} Hz), worst of {PHASES} phases: "
              f"strongest bass alias sdl - reference {dl:+.2f} dB (sdl {sl:.2f} dBc @ {shz:.2f} Hz, reference "
              f"{rl:.2f} dBc @ {rhz:.2f} Hz){'' if check_line else ' (reported only)'}; band 20-1000 Hz "
              f"sdl - reference {db:+.2f} dB (sdl {sb:.2f} dB, reference {rb:.2f} dB); limit reference + {limit} dB")
    return ok


def main():
    """Spustí případy nalezených binárek; 0 = PASS, 1 = FAIL, 77 = SKIP."""
    emu_test_proc.install(deadline_s=290)
    if _find_exe("mz800emu") is None:
        print("SKIP: no mz800emu binary found")
        return 77
    ok = True
    for name, crystal, cpu_div in _PLATFORMS:
        exe = _find_exe(name)
        if exe is None:
            print(f"\n=== {name}: binary not found, skipped ===")
            continue
        print(f"\n=== {name} ===")
        try:
            ok = run_platform(exe, crystal, cpu_div) and ok
        except TestFailure as e:
            print(f"  FAIL: {e}")
            ok = False
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
