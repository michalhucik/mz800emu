#!/usr/bin/env python3
"""E2E test: CLI volby disketových mechanik --fdd, --fdd<N>, --fdd-readonly.

Ověřuje na skutečném emulátoru (MZ-800):

  1  vložení (MCP pipe, INI s připojeným FDC1 a disky v mechanikách 1 a 2):
     ``--fdd 0=a.dsk``, ``--fdd1 b.dsk`` (nahradí disk z INI), ``--fdd5
     b.dsk`` (FDC1, mechanika 1), ``--fdd-readonly 0``; disk z INI
     v mechanice 2 zůstane; ``media_state`` a ``get_periph_wd1793``
     ukážou disky a read-only; po ukončení INI obsahuje cesty z CLI
     a read-only z CLI v něm není;
  2  chyby voleb (konec s kódem 1 před startem): chybějící soubor, číslo
     mechaniky mimo 0-7, ``--fdd`` bez ``=``, mechanika dvakrát (``--fdd``
     i ``--fdd<N>``), ``--fdd-readonly`` mimo 0-7;
  3  chyby zjištěné při inicializaci (headless bez MCP, konec s kódem 3):
     disk pro odpojený FDC1, obraz, který nejde otevřít.

Procesy izolované (dočasný ``--cfg-dir`` / ``--work-dir``, vlastní INI);
zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import TestFailure, _find_exe, check  # noqa: E402
from test_replay_media import make_dsk  # noqa: E402

#: Kořen repa (pracovní adresář emulátoru).
_REPO_ROOT = Path(__file__).resolve().parents[2]


def _args(exe, d, ini):
    """Společné argumenty: headless, izolované prostředí."""
    return [str(exe), "--headless", "--no-first-run-windows",
            f"--cfg-dir={d}", f"--work-dir={d}", f"--config={ini}"]


class Pipe:
    """Emulátor v pipe módu s vlastními volbami (INI se při konci ukládá)."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, d, ini, extra):
        self.proc = subprocess.Popen(_args(exe, d, ini) + ["--mcp-pipe"] + extra,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.rid = 0
        if self._read(lambda m: m.get("type") == "hello", 30.0) is None:
            raise TestFailure("no hello from emulator")

    def _read(self, pred, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                return None
            if line.startswith("{"):
                msg = json.loads(line)
                if pred(msg):
                    return msg
        return None

    def call(self, cmd, data=None):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        self.rid += 1
        rid = self.rid
        self.proc.stdin.write(json.dumps({"type": "request", "req_id": rid, "cmd": cmd,
                                          "data": data or {}}) + "\n")
        self.proc.stdin.flush()
        resp = self._read(lambda m: m.get("type") == "response" and m.get("req_id") == rid, 30.0)
        if resp is None or not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp and resp.get('error')}")
        return resp.get("data") or {}

    def close(self):
        """Řádný konec přes shutdown (uloží INI), jinak zabije vlastní PID."""
        try:
            self.call("shutdown")
            self.proc.wait(timeout=20.0)
        except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
            self.proc.kill()  # vlastní PID
            self.proc.wait()


def _ini_value(ini, section, key):
    """Hodnota klíče v sekci INI (None = chybí)."""
    cur = None
    for line in ini.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1]
        elif cur == section and "=" in line:
            k, v = line.split("=", 1)
            if k.strip() == key:
                return v.strip()
    return None


def _same_path(value, path):
    """Shoduje se cesta z INI se souborem? (lomítka a velikost písmen podle OS)."""
    return value is not None and os.path.normcase(os.path.normpath(value)) ==         os.path.normcase(os.path.normpath(str(path)))


def case_mount(exe, tmp):
    """Scénář 1: vložení, nahrazení disku z INI, read-only jen pro běh, zápis do INI."""
    d = tmp / "mount"
    d.mkdir()
    a, b, c = d / "a.dsk", d / "b.dsk", d / "c.dsk"
    for p in (a, b, c):
        make_dsk(p)
    ini = d / "emu.ini"
    ini.write_text("[FDC]\n"
                   f"wd279x_fdd1_dskpath = {c.as_posix()}\n"
                   f"wd279x_fdd2_dskpath = {c.as_posix()}\n"
                   "[FDC1]\nconnected = 1\n", encoding="utf-8")
    emu = Pipe(exe, d, ini, ["--fdd", f"0={a}", "--fdd1", str(b), "--fdd5", str(b),
                             "--fdd-readonly", "0"])
    try:
        slots = {s["slot"]: s for s in emu.call("media_state")["slots"]}
        got = {k: (Path(v["path"]).name if v["inserted"] else None, v["ro"])
               for k, v in slots.items() if k.startswith("fdc")}
        want = {"fdc0_fd0": ("a.dsk", True), "fdc0_fd1": ("b.dsk", False),
                "fdc0_fd2": ("c.dsk", False), "fdc0_fd3": (None, False),
                "fdc1_fd0": (None, False), "fdc1_fd1": ("b.dsk", False),
                "fdc1_fd2": (None, False), "fdc1_fd3": (None, False)}
        check(got == want, f"drives after start ({got})")
        drv = emu.call("get_periph_wd1793")["drives"][0]
        check(drv["present"] and drv["user_readonly"] and drv["tracks"] == 2,
              f"drive 0: DSK geometry read, write-protected by --fdd-readonly ({drv})")
    finally:
        emu.close()
    check(_same_path(_ini_value(ini, "FDC", "wd279x_fdd0_dskpath"), a)
          and _same_path(_ini_value(ini, "FDC", "wd279x_fdd1_dskpath"), b)
          and _same_path(_ini_value(ini, "FDC1", "fdc1_fdd1_dskpath"), b),
          "CLI paths saved to the INI like a disk inserted from the menu")
    check(_ini_value(ini, "FDC", "wd279x_fdd0_readonly") == "0",
          "--fdd-readonly is not saved to the INI")


def _run(exe, d, ini, extra, timeout=60.0):
    """Spustí headless emulátor bez MCP; vrátí (kód, výstup)."""
    p = subprocess.run(_args(exe, d, ini) + ["--no-save-ini"] + extra, capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=timeout,
                       cwd=str(_REPO_ROOT))
    return p.returncode, p.stdout + p.stderr


def case_errors(exe, tmp):
    """Scénáře 2 a 3: chyby voleb (kód 1) a chyby při inicializaci (kód 3)."""
    d = tmp / "err"
    d.mkdir()
    a = d / "a.dsk"
    make_dsk(a)
    bad = d / "bad.dsk"
    bad.write_bytes(b"not a DSK image")
    ini = d / "emu.ini"
    ini.write_text("", encoding="utf-8")
    cases = (
        (["--fdd", f"0={d / 'missing.dsk'}"], 1, "DSK file not found"),
        (["--fdd", f"8={a}"], 1, "floppy drive number must be 0-7"),
        (["--fdd", str(a)], 1, "expected <N>=<file>"),
        (["--fdd", f"3={a}", "--fdd3", str(a)], 1, "is given more than once"),
        (["--fdd-readonly", "9"], 1, "floppy drive number must be 0-7"),
        (["--fdd", f"4={a}"], 3, "which is not connected"),
        (["--fdd", f"1={bad}"], 3, "cannot open DSK image in floppy drive 1"),
    )
    for extra, code, text in cases:
        rc, out = _run(exe, d, ini, extra)
        check(rc == code and text in out, f"{' '.join(extra[:1])} {extra[1:] and Path(extra[-1]).name}: "
                                          f"exit code {rc} (want {code}), message '{text}'")


def main():
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_cli_fdd_"))
    ok = False
    try:
        case_mount(exe, tmp)
        case_errors(exe, tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
