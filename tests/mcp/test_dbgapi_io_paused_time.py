#!/usr/bin/env python3
"""E2E test: io_read / io_write přes MCP v pauze neposune čas emulace (N-D5-6).

Port I/O příkazu debuggeru jde stejnými callbacky jako instrukce IN / OUT
a ty volají mzarch_main_insideop_iorq(): čas GDG dorovná k T-stavu
rozpracované instrukce (cpu->op_tstate). Mezi instrukcemi je v op_tstate
délka předchozí instrukce, takže každý io_read / io_write v pauze dřív
natrvalo posunul ``time.ticks`` (``state_observe``) o délku poslední
instrukce (MZ-700 20, MZ-800 35 taktů GDG; i v masteru a ve 2.0.6).
Na mz700emu-pal, mz800emu a mz1500emu (pipe mód, bez TCP portu):

  - 10x ``io_read`` a 10x ``io_write`` neobsazeného portu 74h v pauze
    nezmění ``time.ticks`` ani ``time.frame``;
  - ``get_registers`` po příkazech hlásí stejné PC a emulace pak běží dál
    (``run`` 2 snímky posune čas).

Zápis do PSG (MZ-800, MZ-1500) čeká na takt PSG a čas posune dál; test ho
nekontroluje.

Emulátor běží s izolovaným --cfg-dir a --work-dir; zabíjí se jen vlastní
PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)

#: Neobsazený port na všech třech platformách.
PORT = 0x74


def now(emu):
    """(time.frame, time.ticks) ze state_observe."""
    t = emu.call("state_observe", {"windows": []})["time"]
    return int(t["frame"]), int(t["ticks"])


def check_exe(exe, tmp):
    """Kontroly na jedné binárce."""
    name = exe.stem
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("run", {"frames": 5})
        emu.call("pause")
        pc0 = emu.call("get_registers")["PC"]
        t0 = now(emu)
        for _ in range(10):
            emu.call("io_read", {"port": PORT})
        t1 = now(emu)
        for _ in range(10):
            emu.call("io_write", {"port": PORT, "value": 0})
        t2 = now(emu)
        print(f"  {name}: time {t0} -> after 10x io_read {t1} -> after 10x io_write {t2}")
        check(t1 == t0, f"{name}: io_read while paused keeps the emulation time ({t0} -> {t1})")
        check(t2 == t1, f"{name}: io_write while paused keeps the emulation time ({t1} -> {t2})")
        check(emu.call("get_registers")["PC"] == pc0, f"{name}: PC unchanged")
        emu.call("run", {"frames": 2})
        emu.call("pause")
        check(now(emu) > t2, f"{name}: emulation runs on after the commands")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=150)
    exes = [e for e in (_find_exe("mz700emu-pal"), _find_exe("mz800emu"), _find_exe("mz1500emu")) if e]
    if not exes:
        print("SKIP: no emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_io_paused_time_"))
    ok = False
    try:
        for exe in exes:
            check_exe(Path(exe), tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"  (work dir kept: {tmp})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
