#!/usr/bin/env python3
"""E2E test zadního přepínače SW1 MZ-800 (INI ``[MZ800] mode_switch``, CLI
``--mode-switch``), headless + MCP TCP.

Poloha přepínače je bit 1 Status registru GDG. ROM 9Z-504M ji čte v ]GOPGM
(0ECFCh): při bitu 1 = 1 zapíše DMD = 0 (MZ-800 mód) a zhasne paletu, při
bitu 1 = 0 ponechá DMD = 08h (MZ-700 mód). Bootstrap ``--run-mzf`` tento
krok replikuje, takže poloha přepínače je vidět na DMD a paletě na exec
adrese programu (PC_EXEC breakpoint 1200h).

Případy:

  * výchozí INI, bez volby            -> DMD 08h (MZ-700 mód)
  * INI ``mode_switch = MZ800``       -> DMD 00h, paleta 0
  * výchozí INI, ``--mode-switch=800``-> DMD 00h, paleta 0
  * INI MZ800, ``--mode-switch=700``  -> DMD 08h (CLI má přednost)
  * ``--mode-switch=900``             -> proces skončí s chybou před startem

Uložení ``--mode-switch`` do INI (stejně jako ostatní CLI override) tady
ověřit nejde - headless TCP se neukončuje korektně a INI se neuloží.
Snapshot a INI viz test_snapshot_ini.py.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka).
"""

import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů, volné TCP porty
from test_run_mzf_state import (TcpEmu, TestFailure, _EXEC, _find_exe,  # noqa: E402
                                _make_mzf, check)

#: DMD po bootstrapu v MZ-700 módu (IPL E816h) a v MZ-800 módu (]GOPGM ED02h).
_DMD_MZ700, _DMD_MZ800 = 0x08, 0x00


def _run(exe, tmp, name, ini_extra="", extra_args=()):
    """Spustí ``--run-mzf`` a vrátí get_periph_gdg na exec adrese programu."""
    work = tmp / name
    work.mkdir()
    mzf = work / "mode_switch.mzf"
    _make_mzf(mzf)
    ini = work / "emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load=1\nauto_save=0\n" + ini_extra, encoding="utf-8")
    (work / "mz800-breakpoints.bpt").write_text(json.dumps({
        "breakpoints": [{"id": 1, "name": "exec", "addr": _EXEC, "type": "PC_EXEC",
                         "enabled": True, "skip_count": 0, "hit_count": 0,
                         "expr": None, "action": None, "edge_triggered": False}],
        "groups": []}), encoding="utf-8")
    port = emu_test_proc.free_tcp_port()
    args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
            f"--cfg-dir={work}", f"--work-dir={work}", f"--config={ini}",
            f"--mcp-tcp-port={port}", *extra_args, "--run-mzf", str(mzf)]
    emu = TcpEmu(args, work, port)
    try:
        deadline = time.monotonic() + 20.0
        while not emu.call("get_state").get("paused"):
            if time.monotonic() > deadline:
                raise TestFailure(f"[{name}] exec breakpoint was not hit")
            time.sleep(0.1)
        return emu.call("get_periph_gdg")
    finally:
        emu.close()


def _expect(gdg, name, mz800):
    """Ověří DMD (a u MZ-800 módu zhasnutou paletu) podle očekávaného módu."""
    want = _DMD_MZ800 if mz800 else _DMD_MZ700
    mode = "MZ-800" if mz800 else "MZ-700"
    check(gdg.get("regDMD") == want,
          f"[{name}] DMD = {want:02X}h ({mode} mode), got {gdg.get('regDMD')}")
    if mz800:
        check(gdg.get("palette", [None] * 4)[:4] == [0, 0, 0, 0],
              f"[{name}] palette PAL0-PAL3 cleared by @BLACK")


def main():
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_mode_switch_"))
    ok = False
    try:
        _expect(_run(exe, tmp, "default"), "default", False)
        _expect(_run(exe, tmp, "ini800", "[MZ800]\nmode_switch = MZ800\n"), "ini800", True)
        _expect(_run(exe, tmp, "cli800", extra_args=("--mode-switch=800",)), "cli800", True)
        _expect(_run(exe, tmp, "ini800_cli700", "[MZ800]\nmode_switch = MZ800\n",
                     ("--mode-switch=700",)), "ini800_cli700", False)

        proc = subprocess.run([str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
                               f"--cfg-dir={tmp}", f"--work-dir={tmp}", "--mode-switch=900"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace", timeout=30)
        check(proc.returncode != 0 and "--mode-switch requires 700 or 800" in proc.stdout,
              f"--mode-switch=900 is rejected (exit code {proc.returncode})")
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
