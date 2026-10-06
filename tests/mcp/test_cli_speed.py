#!/usr/bin/env python3
"""Test CLI volby ``--speed <procenta|max>`` (headless + MCP TCP, bez audio).

Pro každou hodnotu spustí mz800emu(.exe) s ``--headless`` a MCP TCP serverem
v izolovaném prostředí (vlastní dočasný ``--cfg-dir``, ``--no-save-ini``) bez
programu a z počítadla ``total_screens`` (``get_periph_gdg``) změří tempo
emulace v reálném čase. Ověřuje:

  1. ``--speed max``  -> MAX SPEED (``get_speed``: max_speed=true, tempo
     výrazně nad 50 snímků/s),
  2. ``--speed 200``  -> cca 100 snímků/s (akceptováno 70..140),
  3. ``--speed 50``   -> cca 25 snímků/s (akceptováno 12..40),
  4. ``--speed 100 --maxspeed-bench`` -> benchmark vyhrává, běží MAX SPEED,
  5. neplatné hodnoty (0, 4001, abc, -5, 1.5, prázdná) -> okamžité ukončení
     s nenulovým kódem a anglickou chybou (``Invalid value for --speed``,
     u prázdné hodnoty obecná ``Option --speed requires a value``).

Prahy jsou záměrně široké kvůli sdíleným CI strojům; odliší ale normální
rychlost (50 snímků/s) od 200 %, 50 % i MAX SPEED (stovky snímků/s).

Port MCP TCP: proměnná ``MZ_CLI_SPEED_TEST_PORT`` (23800 je vyhrazen pro
běžné instance); bez ní dostane každý spuštěný emulátor volný port od systému,
takže souběžné běhy testu se nesrazí. Binárka: kořen repa nebo proměnná ``MZ_EMU``.
Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
import shutil
import socket
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


def _base_args(exe, tmp, ini):
    """Společné argumenty: headless, izolované prostředí."""
    return [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
            f"--cfg-dir={tmp}", f"--work-dir={tmp}", f"--config={ini}"]


class TcpEmu:
    """Headless emulátor s MCP TCP serverem: JSONL request/response přes socket."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp, ini, port, extra):
        # port None = volný port přidělený systémem (souběžné běhy testů).
        port = port or emu_test_proc.free_tcp_port()
        args = _base_args(exe, tmp, ini) + [f"--mcp-tcp-port={port}"] + extra
        self.log = open(tmp / "emu.log", "w", encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=self.log,
                                     stderr=subprocess.STDOUT, cwd=str(_REPO_ROOT))
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
        self.sock.settimeout(60.0)
        self.f = self.sock.makefile("rw", buffering=1, encoding="utf-8", newline="\n")
        if self._read(lambda m: m.get("type") == "hello") is None:
            raise TestFailure("no hello from emulator")
        self.rid = 0

    def _read(self, pred):
        while True:
            line = self.f.readline()
            if not line:
                return None
            try:
                msg = json.loads(line.strip())
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

    def fps(self, seconds=2.5):
        """Změří tempo emulace (snímky za reálnou sekundu)."""
        a = int(self.call("get_periph_gdg")["total_screens"])
        t = time.monotonic()
        time.sleep(seconds)
        b = int(self.call("get_periph_gdg")["total_screens"])
        return (b - a) / (time.monotonic() - t)

    def close(self):
        """Zabije jen vlastní PID (headless TCP režim se přes MCP neukončuje)."""
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        if self.sock is not None:
            self.sock.close()
        self.log.close()


def run_speed(exe, port, extra, lo, hi, want_max, label):
    """Spustí emulátor s ``extra`` argumenty a ověří tempo a stav get_speed."""
    tmp = Path(tempfile.mkdtemp(prefix="mz_cli_speed_"))
    ini = tmp / "mz800emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load=0\nauto_save=0\n", encoding="utf-8")
    emu = None
    try:
        emu = TcpEmu(exe, tmp, ini, port, extra)
        time.sleep(1.0)  # doběh startu (nastavení rychlosti při startu emulace)
        gs = emu.call("get_speed")
        check(bool(gs.get("max_speed")) == want_max,
              f"{label}: get_speed max_speed={gs.get('max_speed')} (expected {want_max})")
        fps = emu.fps()
        check(lo <= fps <= hi, f"{label}: {fps:.1f} frames/s (expected {lo}..{hi})")
    finally:
        if emu is not None:
            emu.close()
        shutil.rmtree(tmp, ignore_errors=True)


def run_invalid(exe, value):
    """Neplatná hodnota: okamžitý konec s nenulovým kódem a anglickou chybou."""
    tmp = Path(tempfile.mkdtemp(prefix="mz_cli_speed_bad_"))
    ini = tmp / "mz800emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load=0\nauto_save=0\n", encoding="utf-8")
    try:
        p = subprocess.run(_base_args(exe, tmp, ini) + [f"--speed={value}"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True, errors="replace",
                           timeout=30, cwd=str(_REPO_ROOT))
        check(p.returncode != 0 and ("Invalid value for --speed" in p.stderr
                                      or "Option --speed requires a value" in p.stderr),
              f"invalid --speed={value!r} rejected (exit {p.returncode})")
    except subprocess.TimeoutExpired:
        raise TestFailure(f"--speed={value!r} did not exit (emulator started)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    # Pevný port jen z proměnné prostředí; jinak si každý spuštěný emulátor
    # vezme volný port (souběžné běhy testů se nesrazí).
    port = emu_test_proc.mcp_test_port("MZ_CLI_SPEED_TEST_PORT")
    print(f"CLI --speed test ({exe}, port {port or 'auto'})")
    rc = 0
    try:
        run_speed(exe, port, ["--speed", "max"], 120.0, 1e9, True, "--speed max")
        run_speed(exe, port, ["--speed=200"], 70.0, 140.0, False, "--speed 200")
        run_speed(exe, port, ["--speed", "50"], 12.0, 40.0, False, "--speed 50")
        run_speed(exe, port, ["--speed", "100", "--maxspeed-bench"], 120.0, 1e9, True,
                  "--speed 100 + --maxspeed-bench")
        for bad in ("0", "4001", "abc", "-5", "1.5", ""):
            run_invalid(exe, bad)
        print("RESULT: PASS")
    except TestFailure as e:
        print(f"RESULT: FAIL - {e}")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
