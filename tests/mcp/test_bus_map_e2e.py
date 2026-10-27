#!/usr/bin/env python3
"""E2E test: mapa vlastníků sběrnice (``get_bus_map``, ``bus_peek``) a model
sběrnice devbus (``settings_set DEVBUS/*``) na MZ-700; statická tabulka
vlastníků na MZ-800 a MZ-1500 (bez devbus).

Ověřuje přes MCP (pipe mód, bez TCP portu):

  - mz700emu-pal: ``get_bus_map`` před připojením karty K&P: porty 70h-73h
    mají registrovaného odpojeného vlastníka ``kp80`` (drive ``tristate``,
    maska 255) a příznak ``unmapped``; po ``periph_attach(kind="kp80")`` je
    vlastník připojený a příznak zmizí; port 74h nemá vlastníka;
  - statická tabulka: port 0E0h vlastní ``mapper`` (jen zápis, drive
    ``none``), 50h ``unicard`` (odpojený, drive ``tristate``), MMIO 0E000h
    ``ppi8255`` (``tristate``, ``when`` mapped), 0E007h jen zápis, 0E010h po
    připojení PCG-700 ``pcg700`` (drive ``none``), 0E00Ch po připojení SUC
    ``suc80`` (``none``);
  - ``include`` a ``only_mapped`` omezí odpověď; seznam zařízení obsahuje
    statické čipy i zařízení devbus;
  - ``bus_peek``: port 72h s kartou = status z peeku karty (``source``
    ``devbus``, ``driven_mask`` 255), port 74h = model nebuzené sběrnice
    (``idle``, maska 0), MMIO 0E000h = deska (``board``); chybné parametry
    jsou chyba;
  - ``settings_set DEVBUS/conflict`` (and, refuse) a ``DEVBUS/bus_idle``
    (const + ``bus_idle_value``) se promítnou do ``get_bus_map`` a do
    ``bus_peek`` nebuzeného portu; neplatná hodnota je chyba;
  - mz800emu: ``supported`` true, ``devbus_supported`` false; statická
    tabulka: 0CEh GDG (OUT DMD, IN status), 0D0h-0D3h 8255 a 0D4h-0D7h 8253
    (přítomné jen v režimu MZ-800), 0E0h-0E6h mapper, 0F0h paleta OUT /
    joystick IN (``dir_split``, ``partial`` 3Fh), 0F2h PSG, 0FCh-0FFh Z80
    PIO, MMIO 0E000h 8255; ``bus_peek`` portu i MMIO adresy;
  - mz1500emu: ``supported`` true; PSG na 0E9h, 0F2h, 0F3h, GDG 0F0h-0F1h,
    mapper 0E0h-0E6h, Z80 PIO 0FCh-0FFh, MMIO 0E000h 8255 vždy přítomná;
    porty 0CEh a 0D0h-0D7h bez vlastníka.

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
from test_devbus_e2e import call_raw  # noqa: E402


def port_entry(bm, port):
    """Záznam portu z odpovědi get_bus_map."""
    found = [p for p in bm["ports"] if p["port"] == port]
    check(len(found) == 1, f"port {port:02X}h present once in get_bus_map ({len(found)})")
    return found[0]


def mmio_entry(bm, addr):
    """Záznam MMIO buňky z odpovědi get_bus_map."""
    found = [c for c in bm["mmio"] if c["addr"] == addr]
    check(len(found) == 1, f"mmio {addr:04X}h present once in get_bus_map ({len(found)})")
    return found[0]


def owner(entry, oid):
    """Vlastník podle id."""
    found = [o for o in entry["owners"] if o["id"] == oid]
    check(len(found) == 1, f"owner '{oid}' present once ({[o['id'] for o in entry['owners']]})")
    return found[0]


def check_pal(exe, tmp):
    """Plná sada kontrol na mz700emu-pal."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        bm = emu.call("get_bus_map")
        check(bm["platform"] == "mz700" and bm["supported"] is True and bm["devbus_supported"] is True,
              f"mz700 bus map supported ({bm['platform']}, {bm['supported']}, {bm['devbus_supported']})")
        check(bm["conflict_policy"] == "refuse" and bm["bus_idle"] == "latch",
              f"default policy refuse / idle latch ({bm['conflict_policy']}, {bm['bus_idle']})")
        check(len(bm["ports"]) == 256 and len(bm["mmio"]) == 256,
              f"256 ports and 256 mmio cells ({len(bm['ports'])}, {len(bm['mmio'])})")
        check(bm["tracking_active"] is False, "activity tracking off in headless run")

        # K&P registrovaná, odpojená: vlastník 70h-73h s drive tristate, port unmapped
        p70 = port_entry(bm, 0x70)
        kp = owner(p70, "kp80")
        check(kp["kind"] == "devbus" and kp["connected"] is False and kp["drive"] == "tristate"
              and kp["drive_mask"] == 255 and kp["dir"] == "rw" and kp["range"] == [0x70, 0x73],
              f"kp80 owner of 70h before attach ({kp})")
        check("unmapped" in p70["flags"] and "potential_conflict" not in p70["flags"],
              f"70h flags before attach ({p70['flags']})")
        p74 = port_entry(bm, 0x74)
        check(p74["owners"] == [] and p74["flags"] == ["unmapped"], f"74h has no owner ({p74})")

        # statická tabulka
        pe0 = port_entry(bm, 0xE0)
        mp = owner(pe0, "mapper")
        check(mp["kind"] == "static" and mp["dir"] == "w" and mp["drive"] == "none" and mp["connected"] is True,
              f"mapper owns 0E0h write-only ({mp})")
        check("unmapped" not in pe0["flags"], f"0E0h mapped ({pe0['flags']})")
        p50 = port_entry(bm, 0x50)
        uc = owner(p50, "unicard")
        check(uc["drive"] == "tristate" and isinstance(uc["connected"], bool) and "VHDL" in uc.get("source_note", ""),
              f"unicard 50h tristate, sourced ({uc})")
        check(("unmapped" in p50["flags"]) == (not uc["connected"]),
              f"50h unmapped iff Unicard disconnected ({uc['connected']}, {p50['flags']})")
        pd8 = port_entry(bm, 0xD8)
        f0 = owner(pd8, "fdc0")
        check(f0["drive"] == "unknown" and f0["dir"] == "rw", f"fdc0 0D8h unknown drive ({f0})")
        pdc = port_entry(bm, 0xDC)
        check(owner(pdc, "fdc0")["dir"] == "w", "fdc0 0DCh write only")
        m0 = mmio_entry(bm, 0xE000)
        ppi = owner(m0, "ppi8255")
        check(ppi["drive"] == "tristate" and ppi["when"] == ["mapped"] and ppi["dir"] == "rw",
              f"ppi8255 at 0E000h ({ppi})")
        m7 = mmio_entry(bm, 0xE007)
        check(owner(m7, "ctc8253")["dir"] == "w" and owner(m7, "ctc8253")["drive"] == "none",
              "0E007h control word write only")
        check(mmio_entry(bm, 0xE009)["owners"] == [], "0E009h has no owner")

        # zařízení: statická i devbus
        ids = {dv["id"]: dv for dv in bm["devices"]}
        check("ppi8255" in ids and ids["ppi8255"]["kind"] == "static" and ids["ppi8255"]["mmio"] == 4,
              f"devices list has ppi8255 static with 4 mmio cells ({ids.get('ppi8255')})")
        check("kp80" in ids and ids["kp80"]["kind"] == "devbus" and ids["kp80"]["ports"] == 4
              and ids["kp80"]["outputs"] == 1 and ids["kp80"]["connected"] is False,
              f"devices list has kp80 devbus ({ids.get('kp80')})")

        # include / only_mapped
        part = emu.call("get_bus_map", {"include": ["ports"], "only_mapped": True})
        check("mmio" not in part and "devices" not in part, "include=[ports] omits mmio and devices")
        check(all(len(p["owners"]) > 0 for p in part["ports"]) and len(part["ports"]) < 256,
              f"only_mapped keeps owned ports only ({len(part['ports'])})")
        check("last_in" not in part["ports"][0], "no activity fields without include activity")
        full = emu.call("get_bus_map", {"include": ["ports", "activity"]})
        check("last_in" in full["ports"][0] and "ghost_hits" in full["ports"][0], "activity fields present")
        bad = call_raw(emu, "get_bus_map", {"include": ["nonsense"]})
        check(bad.get("success") is False, f"invalid include item is an error ({bad})")

        # připojení K&P a PCG-700, SUC
        emu.call("periph_attach", {"kind": "kp80"})
        emu.call("periph_attach", {"kind": "pcg700"})
        emu.call("periph_attach", {"kind": "suc80"})
        bm = emu.call("get_bus_map")
        p70 = port_entry(bm, 0x70)
        kp = owner(p70, "kp80")
        check(kp["connected"] is True and p70["flags"] == [], f"kp80 connected, 70h flags empty ({p70['flags']})")
        pcg = owner(mmio_entry(bm, 0xE010), "pcg700")
        check(pcg["drive"] == "none" and pcg["dir"] == "w" and pcg["when"] == ["mapped"] and pcg["connected"] is True,
              f"pcg700 at 0E010h none ({pcg})")
        suc = owner(mmio_entry(bm, 0xE00C), "suc80")
        check(suc["drive"] == "none" and suc["connected"] is True, f"suc80 at 0E00Ch none ({suc})")
        check(bm["devbus"] == {"registered": 3, "connected": 3}, f"devbus counts ({bm['devbus']})")

        # bus_peek
        pk = emu.call("bus_peek", {"port": 0x72})
        check(pk["source"] == "devbus" and pk["driven_mask"] == 255 and 0 <= pk["value"] <= 255,
              f"bus_peek 72h from kp80 ({pk})")
        pk74 = emu.call("bus_peek", {"port": 0x74})
        check(pk74["source"] == "idle" and pk74["driven_mask"] == 0, f"bus_peek 74h idle ({pk74})")
        pkm = emu.call("bus_peek", {"addr": 0xE000})
        check(pkm["source"] == "board" and pkm["driven_mask"] == 255, f"bus_peek 0E000h board ({pkm})")
        pk9 = emu.call("bus_peek", {"addr": 0xE009})
        check(pk9["source"] == "idle" and pk9["driven_mask"] == 0, f"bus_peek 0E009h idle ({pk9})")
        for bad_args in ({}, {"port": 1, "addr": 0xE000}, {"addr": 0x1000}, {"port": 70000}):
            r = call_raw(emu, "bus_peek", bad_args)
            check(r.get("success") is False, f"bus_peek rejects {bad_args} ({r})")
        # peek nemá vedlejší efekty: dva peeky 70h (VRAM s autoinkrementem) vrátí totéž
        a = emu.call("bus_peek", {"port": 0x70})["value"]
        b = emu.call("bus_peek", {"port": 0x70})["value"]
        check(a == b, f"bus_peek 70h twice is the same (no autoincrement): {a} {b}")

        # model sběrnice přes settings
        r = emu.call("settings_set", {"key": "DEVBUS/conflict", "value": "and"})
        check(r.get("applies") in (None, "now"), f"settings_set DEVBUS/conflict ({r})")
        check(emu.call("get_bus_map", {"include": ["devices"]})["conflict_policy"] == "and", "policy and")
        g = emu.call("settings_get", {"key": "DEVBUS/conflict"})
        check(g.get("value") == "and", f"settings_get DEVBUS/conflict ({g})")
        bad = call_raw(emu, "settings_set", {"key": "DEVBUS/conflict", "value": "maybe"})
        check(bad.get("success") is False, f"invalid policy is an error ({bad})")
        emu.call("settings_set", {"key": "DEVBUS/conflict", "value": "refuse"})
        emu.call("settings_set", {"key": "DEVBUS/bus_idle_value", "value": "0x7e"})
        emu.call("settings_set", {"key": "DEVBUS/bus_idle", "value": "const"})
        bm = emu.call("get_bus_map", {"include": ["devices"]})
        check(bm["bus_idle"] == "const" and bm["bus_idle_value"] == 0x7E and bm["idle_byte"] == 0x7E,
              f"bus_idle const 7Eh ({bm['bus_idle']}, {bm['bus_idle_value']}, {bm['idle_byte']})")
        pk74 = emu.call("bus_peek", {"port": 0x74})
        check(pk74["value"] == 0x7E and pk74["source"] == "idle", f"idle port reads the constant ({pk74})")
        # IN z nebuzeného portu přes CPU cestu vrátí konstantu (io_read jde skutečnou cestou)
        v = emu.call("io_read", {"port": 0x74})["value"]
        check(v == 0x7E, f"io_read of an unmapped port returns bus_idle const ({v:02X})")
        emu.call("settings_set", {"key": "DEVBUS/bus_idle", "value": "latch"})
        check(emu.call("get_bus_map", {"include": ["devices"]})["bus_idle"] == "latch", "bus_idle back to latch")
    finally:
        emu.close()


def check_static_common(bm, name):
    """Řádky společné MZ-800 a MZ-1500 (Unicard, FDC, mapper, Z80 PIO, MMIO)."""
    check(len(bm["ports"]) == 256 and len(bm["mmio"]) == 256, f"{name}: 256 ports and 256 mmio cells")
    uc = owner(port_entry(bm, 0x50), "unicard")
    check(uc["kind"] == "static" and uc["drive"] == "tristate" and uc["dir"] == "rw",
          f"{name}: unicard 50h tristate ({uc})")
    f0 = owner(port_entry(bm, 0xD8), "fdc0")
    check(f0["drive"] == "unknown" and f0["dir"] == "rw" and f0["range"] == [0xD8, 0xDB],
          f"{name}: fdc0 0D8h unknown ({f0})")
    check(owner(port_entry(bm, 0xDC), "fdc0")["dir"] == "w", f"{name}: fdc0 0DCh write only")
    for p in range(0xE2, 0xE7):
        mp = owner(port_entry(bm, p), "mapper")
        check(mp["dir"] == "w" and mp["drive"] == "none" and mp["connected"] is True,
              f"{name}: mapper {p:02X}h write-only ({mp})")
    z = owner(port_entry(bm, 0xFC), "pioz80")
    check(z["drive"] == "tristate" and z["dir"] == "rw" and z["range"] == [0xFC, 0xFF] and z["connected"] is True,
          f"{name}: Z80 PIO 0FCh-0FFh tristate ({z})")
    check(all("not measured" in o["mirror_note"] for o in port_entry(bm, 0xFC)["owners"]),
          f"{name}: mirror note says the emulator mirrors over A8-A15 and hardware is not measured")
    ppi = owner(mmio_entry(bm, 0xE000), "ppi8255")
    check(ppi["drive"] == "tristate" and ppi["dir"] == "rw" and ppi["when"] == ["mapped"],
          f"{name}: ppi8255 at 0E000h ({ppi})")
    check(owner(mmio_entry(bm, 0xE007), "ctc8253")["dir"] == "w", f"{name}: 0E007h control word write only")
    check(owner(mmio_entry(bm, 0xE008), "gdg")["drive"] == "unknown", f"{name}: 0E008h status unknown drive")
    check(mmio_entry(bm, 0xE009)["owners"] == [] and mmio_entry(bm, 0xE010)["owners"] == [],
          f"{name}: 0E009h and 0E010h have no owner")
    check(port_entry(bm, 0x74)["owners"] == [] and "unmapped" in port_entry(bm, 0x74)["flags"],
          f"{name}: 74h has no owner")
    ids = {dv["id"]: dv for dv in bm["devices"]}
    check("ppi8255" in ids and ids["ppi8255"]["kind"] == "static" and "mapper" in ids and "fdc0" in ids,
          f"{name}: devices list has static chips ({sorted(ids)})")


def check_800(exe, tmp):
    """MZ-800: statická tabulka mz800_bus_owners.c, bez devbus."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        bm = emu.call("get_bus_map")
        check(bm["platform"] == "mz800" and bm["supported"] is True and bm["devbus_supported"] is False,
              f"mz800: static table without devbus ({bm['platform']}, {bm['supported']}, {bm['devbus_supported']})")
        check(bm["devbus"] == {"registered": 0, "connected": 0}, f"mz800: no devbus devices ({bm['devbus']})")
        check_static_common(bm, "mz800")
        # GDG: 0CCh-0CEh OUT, 0CEh IN status (týž čip: bez dir_split), 0CFh 16bitové registry
        pce = port_entry(bm, 0xCE)
        dirs = sorted(o["dir"] for o in pce["owners"] if o["id"] == "gdg")
        check(dirs == ["r", "w"] and "dir_split" not in pce["flags"], f"mz800: 0CEh GDG OUT + IN status ({pce})")
        st = [o for o in pce["owners"] if o["dir"] == "r"][0]
        check(st["drive"] == "unknown" and "custom LSI" in st["source_note"], f"mz800: GDG status drive unknown ({st})")
        check(owner(port_entry(bm, 0xCC), "gdg")["dir"] == "w", "mz800: 0CCh WF write only")
        cf = owner(port_entry(bm, 0xCF), "gdg")
        check("register" in cf["mirror_note"], f"mz800: 0CFh high byte is the register number ({cf})")
        # 8255 / 8253 na portech jen v režimu MZ-800: po startu ROM přepne do MZ-700 (odpojené)
        ppi = owner(port_entry(bm, 0xD0), "ppi8255")
        check(ppi["drive"] == "tristate" and ppi["range"] == [0xD0, 0xD3] and isinstance(ppi["connected"], bool),
              f"mz800: 8255 0D0h-0D3h ({ppi})")
        ctc = owner(port_entry(bm, 0xD4), "ctc8253")
        check(ctc["drive"] == "tristate" and ctc["range"] == [0xD4, 0xD6], f"mz800: 8253 0D4h-0D6h ({ctc})")
        check(owner(port_entry(bm, 0xD7), "ctc8253")["dir"] == "w", "mz800: 0D7h control word write only")
        mode = emu.call("get_platform_info").get("mode")
        check(mode in ("native", "compat700"), f"mz800: get_platform_info mode ({mode})")
        check(ppi["connected"] == (mode == "native"),
              f"mz800: 8255 on ports present iff MZ-800 mode ({mode}, {ppi['connected']})")
        # mapper: IN 0E0h / 0E1h dekódovaný bez budiče
        mp = owner(port_entry(bm, 0xE0), "mapper")
        check(mp["dir"] == "rw" and mp["drive"] == "none" and mp["range"] == [0xE0, 0xE1], f"mz800: mapper 0E0h IN/OUT none ({mp})")
        # 0F0h: paleta OUT (GDG) a joystick IN (PARTIAL 3Fh) = dir_split
        pf0 = port_entry(bm, 0xF0)
        joy = owner(pf0, "joy0")
        check(joy["dir"] == "r" and joy["drive"] == "partial" and joy["drive_mask"] == 0x3F and joy["connected"] is True,
              f"mz800: joystick 0F0h partial 3Fh ({joy})")
        check(owner(pf0, "gdg")["dir"] == "w" and "dir_split" in pf0["flags"], f"mz800: 0F0h dir_split ({pf0['flags']})")
        check(owner(port_entry(bm, 0xF1), "joy1")["drive_mask"] == 0x3F, "mz800: joystick 0F1h partial 3Fh")
        psg = owner(port_entry(bm, 0xF2), "psg")
        check(psg["dir"] == "w" and psg["drive"] == "none" and psg["connected"] is True, f"mz800: PSG 0F2h write only ({psg})")
        st2 = owner(port_entry(bm, 0xF3), "psg_stereo")
        check(st2["connected"] is False, f"mz800: second PSG absent without allow_psg1 ({st2})")
        check(owner(port_entry(bm, 0xF9), "psg_stereo")["dir"] == "w", "mz800: 0F9h second PSG write")
        # bus_peek: port i MMIO adresa
        pk = emu.call("bus_peek", {"port": 0x74})
        check(pk["source"] == "idle" and pk["driven_mask"] == 0, f"mz800 bus_peek 74h idle ({pk})")
        pk = emu.call("bus_peek", {"port": 0xF0})
        check(pk["source"] == "board" and 0 <= pk["value"] <= 255, f"mz800 bus_peek 0F0h board ({pk})")
        pkm = emu.call("bus_peek", {"addr": 0xE000})
        check(pkm["source"] in ("board", "idle") and 0 <= pkm["value"] <= 255, f"mz800 bus_peek 0E000h ({pkm})")
        pkr = emu.call("bus_peek", {"addr": 0xE020})
        check(pkr["source"] == "board" and pkr["driven_mask"] == 255, f"mz800 bus_peek 0E020h ROM board ({pkr})")
        part = emu.call("get_bus_map", {"include": ["ports"], "only_mapped": True})
        check(0 < len(part["ports"]) < 256 and "mmio" not in part, f"mz800: only_mapped ({len(part['ports'])})")
    finally:
        emu.close()


def check_1500(exe, tmp):
    """MZ-1500: statická tabulka mz1500_bus_owners.c, bez devbus."""
    d = tmp / exe.stem
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        bm = emu.call("get_bus_map")
        check(bm["platform"] == "mz1500" and bm["supported"] is True and bm["devbus_supported"] is False,
              f"mz1500: static table without devbus ({bm['platform']}, {bm['supported']}, {bm['devbus_supported']})")
        check_static_common(bm, "mz1500")
        for p in (0xE9, 0xF2, 0xF3):
            psg = owner(port_entry(bm, p), "psg")
            check(psg["dir"] == "w" and psg["drive"] == "none" and psg["connected"] is True,
                  f"mz1500: PSG {p:02X}h write only ({psg})")
        g = owner(port_entry(bm, 0xF0), "gdg")
        check(g["dir"] == "w" and g["range"] == [0xF0, 0xF1] and "dir_split" not in port_entry(bm, 0xF0)["flags"],
              f"mz1500: GDG 0F0h-0F1h write only, no joystick ({g})")
        for p in (0xCE, 0xD0, 0xD3, 0xD4, 0xD7, 0xE8):
            check(port_entry(bm, p)["owners"] == [], f"mz1500: {p:02X}h has no owner")
        mp = owner(port_entry(bm, 0xE0), "mapper")
        check(mp["dir"] == "w" and mp["range"] == [0xE0, 0xE6], f"mz1500: mapper 0E0h-0E6h write only ({mp})")
        ppi = owner(mmio_entry(bm, 0xE000), "ppi8255")
        check(ppi["connected"] is True, f"mz1500: 8255 at 0E000h always present ({ppi})")
        pk = emu.call("bus_peek", {"port": 0xCE})
        check(pk["source"] == "idle" and pk["driven_mask"] == 0, f"mz1500 bus_peek 0CEh idle ({pk})")
        pkm = emu.call("bus_peek", {"addr": 0xE000})
        check(pkm["source"] in ("board", "idle") and 0 <= pkm["value"] <= 255, f"mz1500 bus_peek 0E000h ({pkm})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=300)
    pal = _find_exe("mz700emu-pal")
    m800 = _find_exe("mz800emu")
    m1500 = _find_exe("mz1500emu")
    if pal is None or m800 is None or m1500 is None:
        print("SKIP: mz700emu-pal, mz800emu or mz1500emu binary not found (run make first)")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_bus_map_e2e_"))
    try:
        check_pal(pal, tmp)
        check_800(m800, tmp)
        check_1500(m1500, tmp)
    except TestFailure as e:
        print(f"FAIL: {e}")
        print(f"Artifacts kept in {tmp}")
        return 1
    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS: bus map e2e")
    return 0


if __name__ == "__main__":
    sys.exit(main())
