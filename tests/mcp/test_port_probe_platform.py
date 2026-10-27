#!/usr/bin/env python3
"""E2E test: sonda ``port[N]`` čte jen položky katalogu I/O aktuální platformy (N-D6-9).

``port_read_no_se_cb()`` (výraz ``port[N]`` ve Watch a v podmínce
breakpointu) bere hodnotu ze zrcadla katalogu ``g_io_ports[]``
(``io_catalog_probe_byte()``), jinak vrací poslední bajt datové sběrnice
(latch). Katalog obsahuje položky všech platforem s maskou
``available_for_mzarch``; sonda ji dřív nekontrolovala, takže na MZ-700
a MZ-1500 vracela pro 0CEh zrcadlo stavu GDG, pro 0D0h / 0D4h zrcadla
8255 / 8253 portů MZ-800 a pro 0F0h joystick MZ-800, ač tyto porty
statický switch platformy nedekóduje a CPU by přečetlo latch.

Program na 3000h: ``DI; JR $`` (``F3 18 FE``). Ve smyčce je posledním
přečteným bajtem operand ``FEh``, takže latch v pauze i při vyhodnocení
breakpointu na 3001h je známý. Stav GDG nemá nikdy nastavený bit 1 ani 3
(``read_gdg_status()``), zrcadlo 0CEh se proto s ``FEh`` ani ``F3h``
shodovat nemůže a test bez opravy selže deterministicky.

  1  mz700emu-pal, mz1500emu: ``watch_eval`` ``port[0x20]`` (volný port)
     = FEh; ``port[0xCE]``, ``port[0xD0]``, ``port[0xD4]``, ``port[0xF0]``
     (na MZ-700 i ``port[0xFC]``) = tatáž hodnota (latch);
  2  tytéž binárky: breakpoint PC_EXEC 3001h s podmínkou
     ``port[0xCE] == port[0x20]`` zastaví (podmínka BP jde stejnou sondou);
  3  mz800emu (citlivost): ``port[0xCE]`` je zrcadlo stavu GDG (bit 3 = 0),
     tedy jiná hodnota než latch ``port[0x20]``.

Emulátor běží s izolovaným --cfg-dir a --work-dir; zabíjí se jen vlastní
PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)

#: Adresa programu a jeho bajty (DI; JR $).
PROG = 0x3000
PROG_HEX = "F318FE"
#: Latch ve smyčce JR $ (operand instrukce).
LATCH = 0xFE
#: Porty, které MZ-700 ani MZ-1500 pro čtení nedekódují, ale katalog
#: pro ně má zrcadlo platformy MZ-800.
FOREIGN_PORTS = (0xCE, 0xD0, 0xD4, 0xF0)
#: Navíc jen na MZ-700: Z80 PIO (položky katalogu MZ-800 a MZ-1500).
FOREIGN_PORTS_700 = (0xFC,)
#: Volný port bez položky katalogu (sonda vrací latch na všech platformách).
FREE_PORT = 0x20
#: Jak dlouho čekat, zda breakpoint zastaví [s].
WAIT_S = 3.0


def probe(emu, port):
    """Hodnota výrazu port[N] (sonda bez vedlejšího efektu)."""
    r = emu.call("watch_eval", {"expr": f"port[0x{port:02X}]"})
    check(r.get("error") in (None, ""), f"watch_eval port[{port:02X}h] without error ({r.get('error')})")
    return int(r["value_int"]) & 0xFF


def start_loop(emu):
    """Zavede smyčku na 3000h a nechá ji běžet, pak pauza."""
    emu.call("run", {"frames": 5})
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {"frames": 3})
    emu.call("pause")


def bp_stops(emu, expr):
    """Breakpoint PC_EXEC na 3001h s podmínkou: zastaví běh?"""
    res = emu.call("bp_create_with_init", {
        "fields": ["type", "addr", "expr"],
        "type": "PC_EXEC", "addr": PROG + 1, "expr": expr})
    check(res.get("created") is True, f"breakpoint created ({expr})")
    try:
        emu.call("run")
        deadline = time.time() + WAIT_S
        while time.time() < deadline:
            if emu.call("get_state").get("paused"):
                return True
            time.sleep(0.05)
        emu.call("pause")
        return False
    finally:
        emu.call("bp_remove", {"id": res["id"]})


def check_foreign(exe, tmp):
    """Scénáře 1 a 2 na MZ-700 / MZ-1500."""
    name = exe.stem
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        start_loop(emu)
        latch = probe(emu, FREE_PORT)
        check(latch == LATCH, f"{name}: port[{FREE_PORT:02X}h] is the bus latch {LATCH:02X}h ({latch:02X}h)")
        ports = FOREIGN_PORTS + (FOREIGN_PORTS_700 if name.startswith("mz700") else ())
        for port in ports:
            v = probe(emu, port)
            check(v == latch, f"{name}: port[{port:02X}h] returns the bus latch, "
                              f"not an MZ-800 catalog mirror ({v:02X}h, latch {latch:02X}h)")
        stopped = bp_stops(emu, f"port[0xCE] == port[0x{FREE_PORT:02X}]")
        check(stopped, f"{name}: BP condition port[0xCE] == port[0x{FREE_PORT:02X}] is true")
        print(f"  {name}: OK")
    finally:
        emu.close()


def check_mz800(exe, tmp):
    """Scénář 3: na MZ-800 zůstává zrcadlo stavu GDG."""
    name = exe.stem
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        start_loop(emu)
        latch = probe(emu, FREE_PORT)
        status = probe(emu, 0xCE)
        check(latch == LATCH, f"{name}: port[{FREE_PORT:02X}h] is the bus latch {LATCH:02X}h ({latch:02X}h)")
        check(status != latch and (status & 0x0A) == 0,
              f"{name}: port[CEh] is the GDG status mirror ({status:02X}h, latch {latch:02X}h)")
        print(f"  {name}: OK")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=150)
    foreign = [e for e in (_find_exe("mz700emu-pal"), _find_exe("mz1500emu")) if e]
    mz800 = _find_exe("mz800emu")
    if not foreign and not mz800:
        print("SKIP: no emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_port_probe_"))
    ok = False
    try:
        for exe in foreign:
            check_foreign(Path(exe), tmp)
        if mz800:
            check_mz800(Path(mz800), tmp)
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
