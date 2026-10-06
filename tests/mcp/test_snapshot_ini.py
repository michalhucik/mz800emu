#!/usr/bin/env python3
"""E2E regresní test: načtení snapshotu nepřepíše uživatelské INI (MCP pipe).

Dříve snapshot přepsal proměnné, na které ukazují save handlery cfg
elementů, a hodnota ze snapshotu se při ukončení emulátoru uložila do INI.
Ověřuje se:

  1. proces A (INI s nevýchozími hodnotami, ``--no-save-ini``) pořídí
     snapshot;
  2. proces B0 (výchozí INI, ukládá ho) jen naběhne a skončí - jeho INI
     je referenční "co by uživatel měl po ukončení bez snapshotu";
  3. proces B1 (výchozí INI, ukládá ho) načte snapshot z A:
     - uživatelské preference (CMT polarity/rychlost/kontrola velikosti,
       MEMEXT plnění paměti, záloha Pezik ramdisku) se neobnoví ani za běhu;
     - stav stroje (připojení a typy periferií, verze FW Unicardu,
       druhý PSG, zadní přepínač SW1) se za běhu obnoví ze snapshotu;
     - INI po ukončení je ve všech sledovaných klíčích shodné s B0.

Každý proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní
INI); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Společný základ INI všech procesů (bez autoload breakpointů).
_INI_BASE = "[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"

#: Nevýchozí hodnoty pro proces A, který pořizuje snapshot.
#: Klíč = (sekce, klíč), hodnota = text v INI.
_SNAP_VALUES = {
    ("CMT", "mz_cmtspeed"): "SPEED_2/1",
    ("CMT", "cmt_polarity_inverted"): "1",
    ("CMT", "mzfsize_check"): "0",
    ("MEMEXT", "connected"): "0",
    ("MEMEXT", "type"): "LUFTNER",
    ("MEMEXT", "luftner_force_init"): "0",
    ("MEMEXT", "filling_on_init"): "RANDOM",
    ("HWCOMPAT", "allow_psg1"): "1",
    ("FDC", "connected"): "0",
    ("FDC1", "connected"): "1",
    ("RAMDISK", "mr1r18_pluged"): "0",
    ("RAMDISK", "mr1r18_type"): "STANDARD",
    ("RAMDISK", "mr1r18_size"): "256K",
    ("RAMDISK", "pezik_e8_pluged"): "1",
    ("RAMDISK", "pezik_e8_portmask"): "0x7f",
    ("RAMDISK", "pezik_e8_backuped"): "1",
    ("QDISK", "mz1f11_connected"): "0",
    ("QDISK", "mz1f11_type"): "VIRTUAL",
    ("UNICARD", "connected"): "0",
    ("UNICARD", "fw_version"): "1",
    ("MZ800", "mode_switch"): "MZ800",
}

#: Minimální počet sledovaných klíčů, v nichž se A a B0 musí lišit
#: (jinak by test nic neověřoval).
_MIN_DIFFERING_KEYS = 15


def soft_check(cond, what):
    """Jako check(), ale při selhání jen vypíše FAIL a vrátí False."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    return bool(cond)


def _ini_text(tmp, values=None):
    """Sestaví INI ze základu a slovníku {(sekce, klíč): hodnota}.

    Soubory záloh ramdisků míří do @p tmp - jinak by se zakládaly
    v pracovním adresáři procesu (kořen repozitáře).
    """
    values = dict(values or {})
    for key in ("mr1r18_filepath", "pezik_e8_filepath", "pezik_68_filepath"):
        values[("RAMDISK", key)] = str(tmp / f"{key}.dat")
    sections = {}
    for (sec, key), val in values.items():
        sections.setdefault(sec, []).append(f"{key} = {val}")
    out = _INI_BASE
    for sec, lines in sections.items():
        out += f"[{sec}]\n" + "\n".join(lines) + "\n"
    return out


def _read_ini(path):
    """Načte INI do slovníku {(SEKCE, klíč): normalizovaná hodnota}."""
    result = {}
    section = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().upper()
        elif section and "=" in line:
            key, val = (x.strip() for x in line.split("=", 1))
            if val.lower().startswith("0x"):
                val = str(int(val, 16))
            result[(section, key)] = val.upper()
    return result


def _run_saving(exe, tmp, name, ini_text, action=None):
    """Spustí proces s ukládáním INI, provede action(emu) a vrátí INI po ukončení."""
    cfg = tmp / f"{name}_cfg"
    cfg.mkdir()
    ini = tmp / f"{name}.ini"
    ini.write_text(ini_text, encoding="utf-8")
    emu = PipeEmu(exe, cfg, ini, save_ini=True)
    try:
        emu.call("pause")
        if action:
            action(emu)
    finally:
        emu.close()
    return _read_ini(ini)


def main():
    emu_test_proc.install(deadline_s=110)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_snapshot_ini_"))
    snap = tmp / "a.mzs"
    keys = list(_SNAP_VALUES)
    ok = False
    try:
        print("Process A: take a snapshot with non-default settings")
        state_a = {}

        def take(emu):
            state_a["cmt"] = emu.call("get_periph_cmt")
            state_a["memext"] = emu.call("get_memext_info")
            emu.call("snapshot_save", {"path": str(snap), "description": "A"})

        ini_a = _run_saving(exe, tmp, "a", _ini_text(tmp, _SNAP_VALUES), take)
        check(snap.is_file(), "snapshot file was written")

        print("Process B0: reference run with the default INI (no snapshot)")
        ini_b0 = _run_saving(exe, tmp, "b0", _ini_text(tmp))

        differing = [k for k in keys if ini_a.get(k) != ini_b0.get(k)]
        check(len(differing) >= _MIN_DIFFERING_KEYS,
              f"A and B0 differ in {len(differing)} watched keys "
              f"(need >= {_MIN_DIFFERING_KEYS}): {sorted(set(keys) - set(differing))} equal")

        print("Process B1: default INI, load the snapshot from A")
        failed = 0
        state_b = {}

        def load(emu):
            state_b["cmt_before"] = emu.call("get_periph_cmt")
            emu.call("snapshot_load", {"path": str(snap)})
            state_b["cmt"] = emu.call("get_periph_cmt")
            state_b["memext"] = emu.call("get_memext_info")

        ini_b1 = _run_saving(exe, tmp, "b1", _ini_text(tmp), load)

        for k in differing:
            failed += not soft_check(
                ini_b1.get(k) == ini_b0.get(k),
                f"INI [{k[0]}] {k[1]} = {ini_b0.get(k)} kept after exit "
                f"(snapshot had {ini_a.get(k)}, got {ini_b1.get(k)})")
        cmt_a, cmt_b0, cmt_b1 = state_a["cmt"], state_b["cmt_before"], state_b["cmt"]
        for field in ("polarity_inverted", "cmtspeed", "mzfsize_check"):
            check(cmt_a.get(field) != cmt_b0.get(field),
                  f"CMT {field} differs between A and B before load")
            check(cmt_b1.get(field) == cmt_b0.get(field),
                  f"CMT {field} (user preference) is not restored from the snapshot")
        check(state_b["memext"].get("connected") == state_a["memext"].get("connected"),
              "MEMEXT connection (machine state) is restored for the run")

        check(failed == 0, f"{failed} INI key(s) overwritten by the snapshot")
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
