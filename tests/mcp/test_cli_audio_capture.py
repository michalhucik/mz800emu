#!/usr/bin/env python3
"""Test CLI voleb ``--audio-capture <složka>`` a ``--audio-capture-frames <počet>``.

Spustí mz800emu(.exe) s ``--headless`` v izolovaném prostředí (vlastní
dočasný ``--cfg-dir``, ``--config``, ``--no-save-ini``) bez programu (běží
ROM) a ověří:

  1. emulátor po dosažení počtu snímků sám skončí s kódem 0,
  2. ve složce jsou events.csv, psg_writes.csv, reference.wav, sdl.wav
     a capture.json; capture.json má ``frames`` = zadaný počet,
     ``stop_reason`` = ``Frame count reached`` a v headless režimu
     ``sdl_blocks`` = ``frames`` (SDL cesta vyrábí blok každý snímek),
  3. velikost obou WAV odpovídá počtu vzorků v capture.json (hlavička 44 B);
     počet vzorků odpovídá skutečnému času podle krystalu (881,36 vzorku na
     snímek PAL), capture.json má vedle jmenovitých ``gdg_clk_hz``
     a ``frames_per_sec`` i ``real_clk_hz`` a ``frames_per_sec_real*``,
  4. výběr souborů z INI ``[AUDIO_CAPTURE]`` (psg_writes=0, sdl_wav=0) se
     použije - tyto soubory chybí.

MCP TCP port: volný od systému (23800 je vyhrazen pro běžné instance).
Binárka: kořen repa nebo proměnná ``MZ_EMU``. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů, volné TCP porty

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]

#: Počet zachycených zvukových snímků (1,2 s při PAL).
FRAMES = 60


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


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def run_capture(exe, tmp, ini_text):
    """Spustí headless emulátor se zachycením zvuku; vrátí (kód, složka)."""
    ini = tmp / "mz800emu.ini"
    ini.write_text(ini_text, encoding="utf-8")
    out = tmp / "capture"
    port = emu_test_proc.free_tcp_port()
    args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
            f"--cfg-dir={tmp}", f"--work-dir={tmp}", f"--config={ini}",
            f"--mcp-tcp-port={port}",
            "--audio-capture", str(out), "--audio-capture-frames", str(FRAMES)]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            cwd=str(tmp))
    try:
        code = proc.wait(timeout=60)
    except subprocess.TimeoutExpired:
        emu_test_proc.kill_proc(proc)
        raise TestFailure("emulator did not exit after the requested frame count")
    return code, out


def test_all_files(exe):
    """Výchozí výběr: všech pět souborů, počty a velikosti WAV."""
    print("Test: --audio-capture with all files (default selection)")
    tmp = Path(tempfile.mkdtemp(prefix="mztest-cli-audiocap-"))
    try:
        code, out = run_capture(exe, tmp, "[BREAKPOINTS]\nauto_load=0\nauto_save=0\n")
        check(code == 0, f"emulator exits with code 0 (got {code})")
        for name in ("events.csv", "psg_writes.csv", "reference.wav", "sdl.wav", "capture.json"):
            check((out / name).is_file(), f"{name} exists")
        info = json.loads((out / "capture.json").read_text(encoding="utf-8"))
        check(info["frames"] == FRAMES, f"capture.json frames == {FRAMES} (got {info['frames']})")
        check(info["stop_reason"] == "Frame count reached",
              f"stop_reason 'Frame count reached' (got {info['stop_reason']!r})")
        check(info["sdl_blocks"] == FRAMES,
              f"headless: sdl_blocks == frames (got {info['sdl_blocks']})")
        check(info["platform"] == "mz800", f"platform mz800 (got {info['platform']!r})")
        # Absolutní časy: reference.wav pokrývá origin_ticks..end_ticks.
        check(info["began"] is True, "capture.json began == true")
        # WAV je ve skutečném čase podle krystalu: takty na vzorek = real_clk_hz / wav_rate.
        span = info["end_ticks"] - info["origin_ticks"]
        expect = info["reference_samples"] * info["real_clk_hz"] / info["wav_rate"]
        check(abs(span - expect) <= info["real_clk_hz"] / info["wav_rate"] * 2,
              f"end_ticks - origin_ticks matches reference.wav length ({span} vs {expect:.0f})")
        check(info["gdg_clk_hz"] == 17721600 and info["frames_per_sec"] == 50,
              "capture.json keeps the nominal gdg_clk_hz and frames_per_sec")
        check(info["real_clk_hz"] == 17734475, f"capture.json real_clk_hz (got {info['real_clk_hz']})")
        check(info["frames_per_sec_real_num"] == 17734475 and info["frames_per_sec_real_den"] == 354432,
              "capture.json frames_per_sec_real_num / _den = crystal / ticks per frame")
        check(abs(info["frames_per_sec_real"] - 50.036326) < 1e-6,
              f"capture.json frames_per_sec_real (got {info['frames_per_sec_real']})")
        check(info["last_event_ticks"] is None or info["last_event_ticks"] >= info["origin_ticks"],
              "last_event_ticks >= origin_ticks")
        clocks = info["clocks"]
        check(clocks["gdg_base_hz"] == info["gdg_clk_hz"], "clocks.gdg_base_hz == gdg_clk_hz")
        check(clocks["gdg_real_base_hz"] == 17734475,
              f"clocks.gdg_real_base_hz (got {clocks['gdg_real_base_hz']})")
        for name, key in (("reference.wav", "reference_samples"), ("sdl.wav", "sdl_samples")):
            size = (out / name).stat().st_size
            check(size == 44 + info[key] * 4, f"{name} size matches {key} ({size} bytes)")
        # snímek podle krystalu má 44100 * 354432 / 17734475 = 881,36 vzorku
        expect_samples = FRAMES * 44100 * info["frames_per_sec_real_den"] // info["frames_per_sec_real_num"]
        check(abs(info["reference_samples"] - expect_samples) <= 1,
              f"reference.wav has {FRAMES} frames of samples ({expect_samples}, got {info['reference_samples']})")
        # sdl.wav: bloky SDL cesty 881 nebo 882 vzorků (akumulátor podle krystalu)
        check(abs(info["sdl_samples"] - info["sdl_blocks"] * 44100 * 354432 / 17734475) <= 1,
              f"sdl.wav: sdl_blocks blocks of 881 or 882 samples (got {info['sdl_samples']} in {info['sdl_blocks']})")
        events = (out / "events.csv").read_text(encoding="utf-8").splitlines()
        check(events[0] == "ticks,time_us,frame,channel,level", "events.csv header")
        check(events[1].startswith("0,0.000,0,ctc0,"), "events.csv starts with the initial levels")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_ini_selection(exe):
    """Výběr souborů z INI [AUDIO_CAPTURE]."""
    print("Test: file selection from [AUDIO_CAPTURE] in the INI")
    tmp = Path(tempfile.mkdtemp(prefix="mztest-cli-audiocap-"))
    try:
        code, out = run_capture(exe, tmp, "[BREAKPOINTS]\nauto_load=0\nauto_save=0\n"
                                          "[AUDIO_CAPTURE]\npsg_writes=0\nsdl_wav=0\n")
        check(code == 0, f"emulator exits with code 0 (got {code})")
        check((out / "events.csv").is_file(), "events.csv exists")
        check((out / "reference.wav").is_file(), "reference.wav exists")
        check(not (out / "psg_writes.csv").exists(), "psg_writes.csv not written (psg_writes=0)")
        check(not (out / "sdl.wav").exists(), "sdl.wav not written (sdl_wav=0)")
        info = json.loads((out / "capture.json").read_text(encoding="utf-8"))
        check(info["frames"] == FRAMES, f"capture.json frames == {FRAMES}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    """Spustí oba testy; 0 = PASS, 1 = FAIL."""
    emu_test_proc.install(deadline_s=110)
    exe = _find_exe()
    try:
        test_all_files(exe)
        test_ini_selection(exe)
    except TestFailure as e:
        print(f"FAIL: {e}")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
