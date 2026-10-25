#!/usr/bin/env python3
"""E2E test: get_memory_map a memory_map ve state_observe podle mapování (MCP pipe, N94).

Ověřuje přes MCP:

  1  po resetu v pauze (MZ-800: dolní ROM, CG-ROM na 1000h, horní ROM) hlásí
     get_memory_map slot 0000h ``rom_low`` / ``rom``, 1000h ``cgrom`` /
     ``cgrom``, F000h ``rom_high`` / ``rom``, vše jen pro čtení; dřív
     s MemExtem hlásil všude ``memext_ram``; 8000h ``vram_i`` / ``vram``
     a C000h, D000h ``ram`` (režim MZ-800 jako na HW, dřív emulátor po
     resetu zapínal režim 700 a VRAM nepřipojil),
  2  po prvních instrukcích ROM (CG-ROM odpojena) je na 1000h ``ram``
     se zdrojem ``ram`` nebo ``memext_ram`` (podle MemExtu), zápis povolen,
  3  u každého slotu platí: kind ``ram`` právě když zdroj je ``ram``,
     ``memext_ram`` nebo ``memext_flash``,
  4  memory_map ve state_observe má stejné kind a source jako get_memory_map.

Proces běží izolovaně (dočasný ``--cfg-dir`` a ``--config``); zabíjí se
jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

RAM_SOURCES = ("ram", "memext_ram", "memext_flash")


def slot(mm, addr):
    """Slot mapy paměti s adresou addr."""
    for s in mm["slots"]:
        if s["addr_start"] <= addr <= s["addr_end"]:
            return s
    raise TestFailure(f"no slot for {addr:04X}h")


def check_consistent(mm, what):
    """Kontrola 3: kind ram <=> zdroj RAM / MemExt."""
    for s in mm["slots"]:
        is_ram = s["kind"] == "ram"
        check(is_ram == (s["source"] in RAM_SOURCES),
              f"{what}: slot {s['addr_start']:04X}h kind {s['kind']} matches source {s['source']}")


def check_observe_same(emu, mm, what):
    """Kontrola 4: state_observe memory_map = get_memory_map."""
    obs = emu.call("state_observe", {"windows": [], "include": ["memory_map"]})
    om = obs["memory_map"]
    a = [(s["kind"], s["source"], s["ro_rw"]) for s in mm["slots"]]
    b = [(s["kind"], s["source"], s["ro_rw"]) for s in om["slots"]]
    check(a == b, f"{what}: state_observe memory_map equals get_memory_map")


def main():
    emu_test_proc.install(deadline_s=60)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_memmap_kind_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            emu.call("pause")
            emu.call("reset")
            mm = emu.call("get_memory_map")
            s0, s1, sf = slot(mm, 0x0000), slot(mm, 0x1200), slot(mm, 0xF000)
            check((s0["kind"], s0["source"], s0["ro_rw"]) == ("rom_low", "rom", "r"),
                  f"after reset 0000h is ROM ({s0})")
            check((s1["kind"], s1["source"], s1["ro_rw"]) == ("cgrom", "cgrom", "r"),
                  f"after reset 1000h is CG-ROM ({s1})")
            check((sf["kind"], sf["source"], sf["ro_rw"]) == ("rom_high", "rom", "r"),
                  f"after reset F000h is ROM ({sf})")
            s8, sc, sd = slot(mm, 0x8000), slot(mm, 0xC000), slot(mm, 0xD000)
            check((s8["kind"], s8["source"]) == ("vram_i", "vram"),
                  f"after reset 8000h is VRAM ({s8})")
            check(sc["kind"] == "ram" and sd["kind"] == "ram",
                  f"after reset C000h and D000h are RAM ({sc}, {sd})")
            check_consistent(mm, "after reset")
            check_observe_same(emu, mm, "after reset")

            # Program v ROM odpojí CG-ROM během prvních 11 instrukcí.
            emu.call("step_n", {"count": 20})
            mm = emu.call("get_memory_map")
            s1 = slot(mm, 0x1200)
            check(s1["kind"] == "ram" and s1["source"] in ("ram", "memext_ram") and s1["ro_rw"] == "rw",
                  f"after 20 instructions 1000h is RAM ({s1})")
            check_consistent(mm, "after 20 instructions")
            check_observe_same(emu, mm, "after 20 instructions")
        finally:
            emu.close()
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"temporary files kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
