#!/usr/bin/env python3
"""E2E test: připojení Quick Disku přes MCP media_insert (MCP pipe, M20).

Ověřuje přes MCP:

  1  neexistující obraz: chyba "File not found", typ mechaniky ani otisk
     stroje se nezmění,
  2  první insert z výchozího zavaděče Unicard přepne mechaniku na
     MZ-1F11 (``switched_to_mz1f11``), obraz je vložený, ``ro`` platí,
     otisk i revize stavu se změní,
  3  další insert do MZ-1F11 už nepřepíná, ``ro=False`` ochranu zruší,
  4  media_eject slot vyprázdní.

Proces běží izolovaně (dočasný ``--cfg-dir``, ``--no-save-ini``); zabíjí
se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402
from test_state_rev import call_any  # noqa: E402

#: Velikost nového prázdného obrazu (QDISK_FORMAT_SIZE, qdisk_create_image()).
QD_IMAGE_SIZE = 82958


def qd_slot(emu):
    """Stav slotu qd z media_state."""
    slots = emu.call("media_state")["slots"]
    return next(s for s in slots if s["slot"] == "qd")


def qd_type(emu):
    """Živý typ mechaniky QD (image / virtual / unicard), None = odpojená.

    settings_get QDISK/mz1f11_type nestačí: vrací hodnotu prvku INI, ne
    živý stav mechaniky (N63).
    """
    d = emu.call("get_periph_qd")
    return d.get("type") if d.get("available") else None


def mh(emu):
    """Otisk stroje v pauze."""
    return emu.call("machine_hash", {"include_media": False})


def run(emu, tmp):
    """Scénáře 1-4."""
    emu.call("pause")
    img_a = tmp / "a.mzq"
    img_b = tmp / "b.mzq"
    img_a.write_bytes(bytes(QD_IMAGE_SIZE))
    img_b.write_bytes(bytes(QD_IMAGE_SIZE))

    type0 = qd_type(emu)
    print(f"  initial QD type: {type0}, slot: {qd_slot(emu)}")
    check(type0 != "image", "default QD drive is not MZ-1F11 (test needs a switch)")

    print("Scenario 1: missing image")
    h0 = mh(emu)
    resp = call_any(emu, "media_insert", {"slot": "qd", "path": str(tmp / "missing.mzq")})
    check(not resp.get("success") and "File not found" in str(resp.get("error")),
          f"1: missing image is rejected: {resp.get('error')}")
    check(qd_type(emu) == type0, "1: QD type unchanged")
    check(mh(emu)["sha256"] == h0["sha256"], "1: machine fingerprint unchanged")

    print("Scenario 2: first insert switches to MZ-1F11")
    h0 = mh(emu)
    res = emu.call("media_insert", {"slot": "qd", "path": str(img_a), "ro": True})
    check(res.get("switched_to_mz1f11") is True, f"2: switched_to_mz1f11 reported: {res}")
    check(qd_type(emu) == "image", "2: QD drive is MZ-1F11 (type image)")
    s = qd_slot(emu)
    check(s["inserted"] and Path(s["path"]) == img_a, f"2: image inserted: {s}")
    check(s["ro"] is True, "2: write protection on")
    h1 = mh(emu)
    check(h1["parts"]["media"] != h0["parts"]["media"], "2: media part of the fingerprint changed")
    check(h1["state_rev"] != h0["state_rev"], "2: state_rev changed")

    print("Scenario 3: second insert, writable")
    res = emu.call("media_insert", {"slot": "qd", "path": str(img_b), "ro": False})
    check(res.get("switched_to_mz1f11") is False, f"3: no switch reported: {res}")
    s = qd_slot(emu)
    check(s["inserted"] and Path(s["path"]) == img_b and s["ro"] is False,
          f"3: second image inserted writable: {s}")

    print("Scenario 4: eject")
    emu.call("media_eject", {"slot": "qd"})
    check(not qd_slot(emu)["inserted"], "4: slot empty after eject")


def main():
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_media_qd_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            run(emu, tmp)
            ok = True
        finally:
            emu.close()
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
