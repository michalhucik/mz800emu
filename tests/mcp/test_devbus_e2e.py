#!/usr/bin/env python3
"""E2E test: sběrnice volitelných zařízení (devbus) bez připojeného zařízení.

Test ověřuje, že binárky bez připojeného zařízení (výchozí stav, žádné
zařízení není nasazené) vypadají a chovají se jako dřív a že rozhraní
MCP odpovídá:

  mz700emu-pal, mz700emu-ntsc:
    - ``get_devices``: platform_supported = true, žádné zařízení
      v seznamu není připojené (registrovaná zařízení, např. pcg700,
      jsou v seznamu s connected = false);
    - ``periph_attach`` / ``periph_detach`` s neznámým id = chyba
      (id zařízení ověří emulátor);
    - ``settings_set`` neznámého klíče = chyba;
    - ``regions_list`` bez druhu ``device``;
    - zápis na 0E010h a 0E00Ch instrukcemi CPU (horní oblast mapovaná)
      čtení horní ROM nezmění: 0E010h-0E01Fh čte dál bajty
      ``rom_upper`` (offset 10h), zápisy na 0E009h-0EFFFh deska zahazuje;
  mz800emu:
    - ``get_devices``: platform_supported = false, devices = [].

Emulátor běží v pipe módu (bez TCP portu) s izolovaným --cfg-dir
a --work-dir; zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import base64
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)


def call_raw(emu, cmd, data=None, timeout=30.0):
    """Pošle request a vrátí celou response (i neúspěšnou)."""
    emu.rid += 1
    rid = emu.rid
    req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
    emu.proc.stdin.write(json.dumps(req) + "\n")
    emu.proc.stdin.flush()
    resp = emu._read(timeout, lambda m: m.get("type") == "response"
                     and m.get("req_id") == rid)
    if resp is None:
        emu.dead = True
        raise TestFailure(f"no response to {cmd} within {timeout:.0f} s")
    return resp


def mem(emu, addr, length):
    """Přečte paměť CPU (mem_read) jako bytes."""
    d = emu.call("mem_read", {"addr": addr, "len": length})
    return base64.b64decode(d["data_b64"])


def check_mz700(exe, tmp):
    """Kontroly na binárce MZ-700."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        dev = emu.call("get_devices")
        devices = dev.get("devices")
        check(dev.get("platform_supported") is True and isinstance(devices, list)
              and all(d.get("connected") is False for d in devices),
              f"{exe.stem}: get_devices = supported, no device connected ({dev})")

        for cmd in ("periph_attach", "periph_detach"):
            r = call_raw(emu, cmd, {"kind": "nonexistent_dev"})
            check(not r.get("success") and "device id" in (r.get("error") or ""),
                  f"{exe.stem}: {cmd} with an unknown id fails ({r.get('error')})")
        r = call_raw(emu, "periph_attach", {"kind": "memext", "options": {"type": "pehu"}})
        check(r.get("success") is True, f"{exe.stem}: static periph kind still works")
        call_raw(emu, "periph_detach", {"kind": "memext"})

        r = call_raw(emu, "settings_set", {"key": "NODEVICE/option", "value": "1"})
        check(not r.get("success"), f"{exe.stem}: settings_set of an unknown key fails")

        regions = emu.call("regions_list")["regions"]
        check(not [x for x in regions if x["kind"] == "device"],
              f"{exe.stem}: regions_list has no device regions")
        rom_up = [x for x in regions if x["kind"] == "rom_upper"][0]
        rom = base64.b64decode(emu.call("region_read", {
            "region_id": rom_up["id"], "offset": 0x10, "length": 16})["data_b64"])

        # Reset = horní oblast mapovaná (ROM E000h, porty 0E000h-0E008h).
        emu.call("reset")
        emu.call("pause")
        before = mem(emu, 0xE010, 16)
        # Zápis instrukcemi CPU (mem_write stránku s porty odmítá):
        # DI; LD A,5Ah; LD (0E010h),A; LD (0E00Ch),A; JR $
        emu.call("mem_write", {"addr": 0x3000,
                               "data_hex": "F33E5A3210E0320CE018FE"})
        emu.call("set_register", {"reg": "PC", "value": 0x3000})
        emu.call("run", {"frames": 1})
        emu.call("pause")
        pc = emu.call("get_registers").get("PC")
        check(pc == 0x3009, f"{exe.stem}: test program reached JR $ (pc={pc})")
        after = mem(emu, 0xE010, 16)
        check(before == rom and after == rom,
              f"{exe.stem}: 0E010h reads upper ROM before and after the writes "
              f"({before.hex()} / {after.hex()} / rom {rom.hex()})")
    finally:
        emu.close()


def check_mz800(exe, tmp):
    """Kontroly na binárce MZ-800 (devbus neinicializovaný)."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        dev = emu.call("get_devices")
        check(dev.get("platform_supported") is False and dev.get("devices") == [],
              f"{exe.stem}: get_devices = not supported, no devices ({dev})")
        r = call_raw(emu, "periph_attach", {"kind": "nonexistent_dev"})
        check(not r.get("success") and "not available" in (r.get("error") or ""),
              f"{exe.stem}: periph_attach with a device id fails ({r.get('error')})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=200)
    names = ("mz700emu-pal", "mz700emu-ntsc", "mz800emu")
    found = {n: _find_exe(n) for n in names}
    if not any(found.values()):
        print("SKIP: no emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_devbus_e2e_"))
    ok = False
    try:
        for n in names:
            if not found[n]:
                print(f"  SKIP: {n} binary not found")
                continue
            if n.startswith("mz700"):
                check_mz700(Path(found[n]), tmp)
            else:
                check_mz800(Path(found[n]), tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
