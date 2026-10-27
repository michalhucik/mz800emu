#!/usr/bin/env python3
"""E2E test: přístup debuggeru k portu zařízení devbus nevolá WAIT (N-D5-5).

Karta K&P (zařízení devbus ``kp80``) s volbou ``wait`` prodlouží každé IN a
OUT na portech 71h - 73h o ``wait`` T-stavů (``devbus_cpu_wait()``). Přístup
debuggeru mimo instrukci CPU ale WAIT přidat nesmí, jinak ladění mění
časování laděného programu. Na mz700emu-pal (pipe mód, bez TCP portu):

  - PC breakpoint s podmínkou ``port[0x71]! == 0x1FF`` (nikdy pravda,
    vyhodnocuje se při každém průchodu smyčkou) a druhý breakpoint na konci
    programu; počet taktů GDG od startu po konec (``state_observe``
    ``time.ticks``) je s ``wait = 0`` a ``wait = 8`` shodný. Bez opravy se
    liší o 8 T za každé vyhodnocení podmínky;
  - kontrolně bez podmíněného breakpointu se s ``wait = 8`` počet taktů
    nemění (program na porty karty nesahá);
  - ``io_read`` 71h a ``io_write`` 73h v pauze s ``wait = 8`` posunou
    ``time.ticks`` stejně jako přístup k neobsazenému portu 74h (bez WAIT
    karty). Že je posun nulový, hlídá ``mcp_dbgapi_io_paused_time``
    (do opravy N-D5-6 to byla délka poslední instrukce).

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

#: Adresa programu.
PROG = 0x3000
#: Počet průchodů smyčkou (= počet vyhodnocení podmínky breakpointu).
LOOPS = 0x40
#: DI; LD B,LOOPS; loop: NOP; DJNZ loop; JR $.
PROG_HEX = "F3 06 %02X 00 10 FD 18 FE" % LOOPS
#: Adresa smyčky (NOP) s podmíněným breakpointem.
LOOP_ADDR = PROG + 3
#: Adresa JR $ na konci programu.
END_ADDR = PROG + 6
#: Hodnota volby wait karty v testu (T-stavů).
WAIT = 8


def ticks(emu):
    """Takty GDG od startu emulace (state_observe time.ticks)."""
    return int(emu.call("state_observe", {"windows": []})["time"]["ticks"])


def wait_paused(emu, timeout_s=10.0):
    """Počká, až se emulace zastaví (breakpoint)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if emu.call("get_state").get("running") is False:
            return
        time.sleep(0.02)
    raise TestFailure("emulation did not stop at the breakpoint")


def set_wait(emu, value):
    """Nastaví volbu wait karty K&P."""
    emu.call("settings_set", {"key": "KP80/wait", "value": str(value)})
    opts = {o["key"]: o["value"] for x in emu.call("get_devices")["devices"]
            if x["id"] == "kp80" for o in x["options"]}
    check(opts.get("wait") == value, f"KP80 wait = {value} ({opts.get('wait')})")


def run_measure(emu):
    """Spustí program od PROG, počká na breakpoint na END_ADDR, vrátí takty GDG."""
    emu.call("set_register", {"reg": "PC", "value": PROG})
    t0 = ticks(emu)
    emu.call("run")
    wait_paused(emu)
    pc = int(emu.call("get_registers")["PC"])
    check(pc == END_ADDR, f"stopped at the end of the program (PC={pc:04X}h)")
    return ticks(emu) - t0


def check_pal(exe, tmp):
    """Kontroly na mz700emu-pal."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        emu.call("periph_attach", {"kind": "kp80"})
        emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX.replace(" ", "")})
        end_bp = emu.call("bp_add", {"addr": END_ADDR})["id"]

        # Kontrola: bez podmíněného BP program na porty karty nesahá.
        set_wait(emu, 0)
        plain0 = run_measure(emu)
        set_wait(emu, WAIT)
        plain8 = run_measure(emu)
        print(f"  without the conditional BP: wait=0 {plain0} ticks, wait={WAIT} {plain8} ticks")
        check(plain0 == plain8, "program alone does not depend on the card wait option")

        # Podmíněný BP s port[0x71]! na smyčce (nikdy nezastaví).
        bp = emu.call("bp_create_with_init", {
            "fields": ["type", "addr", "expr", "enabled"],
            "type": "PC_EXEC", "addr": LOOP_ADDR, "expr": "port[0x71]! == 0x1FF", "enabled": True})
        check(bp.get("id", 0) > 0, f"conditional PC BP created ({bp})")
        set_wait(emu, 0)
        cond0 = run_measure(emu)
        set_wait(emu, WAIT)
        cond8 = run_measure(emu)
        print(f"  with port[0x71]! condition: wait=0 {cond0} ticks, wait={WAIT} {cond8} ticks "
              f"(difference {cond8 - cond0})")
        check(cond0 == cond8,
              f"breakpoint condition port[0x71]! adds no card WAIT to the program "
              f"(wait=0 {cond0}, wait={WAIT} {cond8} ticks)")
        emu.call("bp_remove", {"id": bp["id"]})
        emu.call("bp_remove", {"id": end_bp})

        # io_read / io_write v pauze: posun času jako u neobsazeného portu 74h.
        def io_delta(cmd, port):
            t0 = ticks(emu)
            args = {"port": port} if cmd == "io_read" else {"port": port, "value": 0}
            emu.call(cmd, args)
            return ticks(emu) - t0
        ref_r, card_r = io_delta("io_read", 0x74), io_delta("io_read", 0x71)
        ref_w, card_w = io_delta("io_write", 0x74), io_delta("io_write", 0x73)
        print(f"  while paused: io_read 74h {ref_r}, 71h {card_r}; io_write 74h {ref_w}, 73h {card_w} ticks")
        check(card_r == ref_r and card_w == ref_w,
              "io_read / io_write of the card ports while paused add no card WAIT")
        emu.call("run", {"frames": 1})
        emu.call("pause")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=120)
    pal = _find_exe("mz700emu-pal")
    if not pal:
        print("SKIP: mz700emu-pal binary not found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_devbus_dbg_wait_"))
    ok = False
    try:
        check_pal(Path(pal), tmp)
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
