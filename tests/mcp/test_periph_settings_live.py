#!/usr/bin/env python3
"""E2E test: periph_attach/detach, settings_set/get a media_state qd za běhu (MCP pipe, M21).

Ověřuje přes MCP:

  1  media_state hlásí u slotu qd v ``ro`` efektivní ochranu vloženého
     obrazu (stejně jako get_periph_qd), po vysunutí false,
  2  periph_attach/detach připojí a odpojí MemExt, FDC1, Quick Disk a IDE8
     za běhu (``changed``, ``type``, živý stav), opakování stav nemění,
     neplatná varianta a gal5 skončí chybou,
  3  settings_set se projeví v navázané proměnné (``live`` v settings_get),
     custom_fps mimo rozsah je odmítnut,
  4  po ukončení s ukládáním INI obsahuje INI hodnoty z kroků 2 a 3
     (dřív se zápis settings_set při ukládání přepsal z proměnné).

Proces běží izolovaně (dočasný ``--cfg-dir`` a ``--config``); zabíjí se
jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import configparser
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402
from test_state_rev import call_any  # noqa: E402

#: Velikost nového prázdného obrazu QD (QDISK_FORMAT_SIZE).
QD_IMAGE_SIZE = 82958


def slot(emu, name):
    """Stav slotu z media_state."""
    return next(s for s in emu.call("media_state")["slots"] if s["slot"] == name)


def periph_qd(emu):
    """get_periph_qd (available, type, readonly)."""
    return emu.call("get_periph_qd")


def live(emu, key):
    """Pole live ze settings_get (None = prvek vazbu nemá)."""
    return emu.call("settings_get", {"key": key}).get("live")


def scenario_media_state_ro(emu, tmp):
    """1: ro u qd = efektivní ochrana."""
    print("Scenario 1: media_state qd ro is the effective protection")
    img = tmp / "a.mzq"
    img.write_bytes(bytes(QD_IMAGE_SIZE))
    emu.call("media_insert", {"slot": "qd", "path": str(img), "ro": True})
    check(slot(emu, "qd")["ro"] is True and periph_qd(emu).get("readonly") is True,
          "1: inserted read-only: ro true")
    emu.call("media_insert", {"slot": "qd", "path": str(img), "ro": False})
    check(slot(emu, "qd")["ro"] is False, "1: inserted writable: ro false")
    emu.call("media_insert", {"slot": "qd", "path": str(img), "ro": True})
    emu.call("media_eject", {"slot": "qd"})
    s = slot(emu, "qd")
    check(not s["inserted"] and s["ro"] is False,
          f"1: after eject ro false although protection was chosen: {s}")


def attach(emu, kind, typ=None, detach=False):
    """periph_attach / periph_detach s volitelným options.type."""
    data = {"kind": kind}
    if typ is not None:
        data["options"] = {"type": typ}
    return emu.call("periph_detach" if detach else "periph_attach", data)


def scenario_periph(emu):
    """2: připojení a odpojení za běhu."""
    print("Scenario 2: periph_attach / periph_detach at runtime")
    # MemExt (výchozí připojený PEHU)
    r = attach(emu, "memext", detach=True)
    check(r["changed"] is True and r["requires_restart"] is False,
          f"2: memext detach changed the state: {r}")
    check(live(emu, "MEMEXT/connected") == "false", "2: MEMEXT/connected live false")
    r = attach(emu, "memext", detach=True)
    check(r["changed"] is False, "2: second memext detach changes nothing")
    r = attach(emu, "memext", "LUFTNER")
    check(r["changed"] is True and r["type"] == "luftner", f"2: memext attach luftner: {r}")
    check(live(emu, "MEMEXT/connected") == "true" and live(emu, "MEMEXT/type") == "LUFTNER",
          "2: MEMEXT live connected true, type LUFTNER")
    r = attach(emu, "memext")
    check(r["changed"] is False and r["type"] == "luftner",
          "2: memext attach without type keeps the current type, no change")
    resp = call_any(emu, "periph_attach", {"kind": "memext", "options": {"type": "luftner4k"}})
    check(not resp.get("success") and "Invalid options.type" in str(resp.get("error")),
          f"2: unknown memext variant rejected: {resp.get('error')}")

    # FDC1 (výchozí odpojený)
    r = attach(emu, "fdc", "fdc1")
    check(r["changed"] is True and r["type"] == "fdc1", f"2: fdc1 attach: {r}")
    check(live(emu, "FDC1/connected") == "true", "2: FDC1/connected live true")
    r = attach(emu, "fdc", "fdc1", detach=True)
    check(r["changed"] is True and live(emu, "FDC1/connected") == "false", "2: fdc1 detach")

    # Quick Disk (scénář 1 ho přes media_insert už přepnul na MZ-1F11)
    r = attach(emu, "qd")
    check(r["changed"] is False and r["type"] == "mz1f11"
          and periph_qd(emu).get("type") == "image",
          f"2: qd attach on MZ-1F11 changes nothing: {r}")
    check(live(emu, "QDISK/mz1f11_type") == "STANDARD", "2: QDISK/mz1f11_type live STANDARD")
    r = attach(emu, "qd", detach=True)
    check(r["changed"] is True and not periph_qd(emu).get("available"), "2: qd detach")
    r = attach(emu, "qd")
    check(r["changed"] is True and periph_qd(emu).get("type") == "image", "2: qd attach again")

    # IDE8 master (výchozí odpojený, obraz se vytvoří v cfg_dir)
    r = attach(emu, "ide8")
    check(r["changed"] is True and r["type"] == "master" and slot(emu, "ide8")["inserted"],
          f"2: ide8 master attach: {r}")
    r = attach(emu, "ide8", detach=True)
    check(r["changed"] is True and not slot(emu, "ide8")["inserted"], "2: ide8 master detach")

    resp = call_any(emu, "periph_attach", {"kind": "gal5"})
    check(not resp.get("success") and "not available" in str(resp.get("error")),
          f"2: gal5 not available: {resp.get('error')}")


def scenario_settings(emu):
    """3: settings_set se projeví v proměnné."""
    print("Scenario 3: settings_set applies to the live variable")
    emu.call("settings_set", {"key": "DISPLAY/custom_fps", "value": "120"})
    g = emu.call("settings_get", {"key": "DISPLAY/custom_fps"})
    check(g["value"] == "120" and g.get("live") == "120", f"3: custom_fps value and live 120: {g}")
    resp = call_any(emu, "settings_set", {"key": "DISPLAY/custom_fps", "value": "10"})
    check(not resp.get("success"), "3: custom_fps 10 rejected (range 50..1000)")
    emu.call("settings_set", {"key": "DISPLAY/forced_full_screen_redrawing", "value": "true"})
    check(live(emu, "DISPLAY/forced_full_screen_redrawing") == "true",
          "3: forced_full_screen_redrawing live true")
    emu.call("settings_set", {"key": "AUDIO/volume_8253", "value": "37"})
    g = emu.call("settings_get", {"key": "AUDIO/volume_8253"})
    check(g["value"] == "37" and "live" not in g,
          f"3: volume_8253 value 37 (callback binding, no live field): {g}")


def check_saved_ini(ini):
    """4: INI po ukončení."""
    print("Scenario 4: saved INI keeps the runtime changes")
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.read(ini, encoding="utf-8")
    got = {
        "AUDIO/volume_8253": cp.get("AUDIO", "volume_8253", fallback=None),
        "DISPLAY/custom_fps": cp.get("DISPLAY", "custom_fps", fallback=None),
        "DISPLAY/forced_full_screen_redrawing":
            cp.get("DISPLAY", "forced_full_screen_redrawing", fallback=None),
        "MEMEXT/connected": cp.get("MEMEXT", "connected", fallback=None),
        "MEMEXT/type": cp.get("MEMEXT", "type", fallback=None),
        "QDISK/mz1f11_type": cp.get("QDISK", "mz1f11_type", fallback=None),
        "FDC1/connected": cp.get("FDC1", "connected", fallback=None),
    }
    want = {
        "AUDIO/volume_8253": "37",
        "DISPLAY/custom_fps": "120",
        "DISPLAY/forced_full_screen_redrawing": "1",
        "MEMEXT/connected": "1",
        "MEMEXT/type": "LUFTNER",
        "QDISK/mz1f11_type": "STANDARD",
        "FDC1/connected": "0",
    }
    for k, v in want.items():
        g = got[k]
        if g is not None and v.isdigit():
            g = str(int(g, 0))  # cfgfile ukládá UNSIGNED hexadecimálně
        check(g == v, f"4: {k} = {v} (got {got[k]})")


def main():
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_periph_live_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini, save_ini=True)
        try:
            emu.call("pause")
            scenario_media_state_ro(emu, tmp)
            scenario_periph(emu)
            scenario_settings(emu)
        finally:
            emu.close()
        check_saved_ini(ini)
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
