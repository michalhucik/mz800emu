#!/usr/bin/env python3
"""E2E test klíčového slova ``[ROM] rom_type`` v INI (MZ-800).

MZ-800 má v konfiguraci ROM dva vývojové řádky (JSS105C, JSS103), které se
bez ``[EMULATOR] development_mode`` nenabízejí a nemají klíčové slovo. Převod
mezi klíčovým slovem a řádkem konfigurace je dřív nepřeskakoval, takže
``rom_type = JSS108C`` vybralo JSS 1.05C a do INI se vybraná ROM zapsala
pod jménem jiné.

Vybraná ROM se pozná podle osmi bajtů dolní ROM na 0751h (v každém obrazu
jiné, mimo místa, která mění CMT hack; hodnoty přečtené z obrazů ROM ve
zdroji).

Kontroly (jen ``mz800emu``):
  - ``rom_type = <jméno>`` pro STANDARD, JSS108C, JSS106A, WILLY_EN vybere
    ROM toho jména a po ukončení s ukládáním INI je v INI stejné jméno;
  - vývojové jméno bez vývojového režimu není klíčové slovo: zůstane
    výchozí ROM (STANDARD);
  - s ``development_mode = 1`` vybere JSS105C i JSS103 a JSS108C zůstane
    JSS108C;
  - INI zapsané starším emulátorem (``[CFGMAIN] config_version = 1.0``) má
    jméno posunuté o vývojové ROM: čte se postaru (``WILLY_EN`` = JSS108C,
    ``WILLY_GE`` = JSS106A, ``WILLY_JAP`` = WILLY_EN), aby se uživateli po
    přechodu na novou verzi ROM nezměnila, a uloží se se skutečným jménem
    a ``config_version = 1.1``; INI s ``config_version = 1.1`` se čte podle
    jmen.

Výstup anglicky. Exit code: 0 = PASS, 1 = FAIL (i chybějící binárka).
"""

import base64
import re
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_run_mzf_state import TestFailure  # noqa: E402
from test_run_mzf_device import PipeEmu, _find_exe, check  # noqa: E402

#: Adresa a délka rozlišovacího okna v dolní ROM.
_SIG_ADDR = 0x0751
_SIG_LEN = 8

#: Obraz dolní ROM -> bajty na 0751h.
_SIG = {
    "STANDARD": bytes.fromhex("0000000000000000"),
    "JSS108C": bytes.fromhex("000000000000006c"),
    "JSS106A": bytes.fromhex("7d07000000000094"),
    "JSS105C": bytes.fromhex("7d0700000000004c"),
    "JSS103": bytes.fromhex("7d07ed780dbac9fc"),
    "WILLY_EN": bytes.fromhex("e681c0c3d9070000"),
}


def _which(sig):
    """Jméno obrazu podle bajtů, jinak hex zápis."""
    for name, s in _SIG.items():
        if s == sig:
            return name
    return sig.hex()


def _boot(exe, tmp, tag, keyword, devel, cfg_version=None, build_time="2026-10-02 11:36:24"):
    """Spustí emulátor s INI, vrátí (jméno vybrané ROM, rom_type zapsaný do INI).

    ``cfg_version``: hodnota ``[CFGMAIN] config_version`` vstupního INI (None =
    bez sekce, jako ručně psané INI). Verzi uloženého INI vrací ``_saved_version``.
    """
    cfg = tmp / tag
    cfg.mkdir()
    ini = cfg / "emu.ini"
    text = "[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
    if cfg_version is not None:
        text = (f"[CFGMAIN]\nconfig_version = {cfg_version}\n"
                f"emulator_build_time = {build_time}\n" + text)
    if devel:
        text += "[EMULATOR]\ndevelopment_mode = 1\n"
    text += f"[ROM]\nrom_type = {keyword}\n"
    ini.write_text(text, encoding="utf-8")
    emu = PipeEmu(exe, cfg, ini, save_ini=True)
    try:
        emu.call("pause")
        raw = base64.b64decode(emu.call("mem_read", {"addr": _SIG_ADDR, "len": _SIG_LEN})
                               .get("data_b64", ""))
    finally:
        emu.close()
    saved = ini.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"^\s*rom_type\s*=\s*(\S+)", saved, re.M)
    v = re.search(r"^\s*config_version\s*=\s*(\S+)", saved, re.M)
    global _saved_version
    _saved_version = v.group(1) if v else None
    return _which(raw), (m.group(1) if m else None)


#: ``config_version`` INI uloženého posledním ``_boot()``.
_saved_version = None


def main():
    emu_test_proc.install(deadline_s=150)
    exe = _find_exe("mz800emu")
    if exe is None:
        print("FAILED: binary mz800emu not found")
        return 1
    try:
        with tempfile.TemporaryDirectory(prefix="mz_romtype_") as td:
            tmp = Path(td)
            print("ROM keywords without development mode")
            for kw in ("STANDARD", "JSS108C", "JSS106A", "WILLY_EN"):
                got, saved = _boot(exe, tmp, "n_" + kw, kw, False)
                check(got == kw, f"rom_type = {kw} selects {kw} (got {got})")
                check(saved == kw, f"the INI is saved with rom_type = {kw} (got {saved})")
            got, saved = _boot(exe, tmp, "n_dev", "JSS105C", False)
            check(got == "STANDARD", f"a development ROM is not a keyword without development mode (got {got})")
            print("ROM keywords with development mode")
            for kw in ("JSS105C", "JSS103", "JSS108C"):
                got, saved = _boot(exe, tmp, "d_" + kw, kw, True)
                check(got == kw, f"development mode: rom_type = {kw} selects {kw} (got {got})")
                check(saved == kw, f"development mode: the INI is saved with rom_type = {kw} (got {saved})")
            print("INI written by an older emulator (config_version 1.0)")
            # dolní ROM mají Willy EN / GE / JAP společnou: variantu rozliší až uložené jméno
            for old_kw, rom, saved_as in (("STANDARD", "STANDARD", "STANDARD"),
                                          ("WILLY_EN", "JSS108C", "JSS108C"),
                                          ("WILLY_GE", "JSS106A", "JSS106A"),
                                          ("WILLY_JAP", "WILLY_EN", "WILLY_EN"),
                                          ("USER_DEFINED", "WILLY_EN", "WILLY_GE")):
                got, saved = _boot(exe, tmp, "o_" + old_kw, old_kw, False, cfg_version="1.0")
                check(got == rom, f"old INI: rom_type = {old_kw} keeps the ROM the user had (lower ROM {rom}, got {got})")
                check(saved == saved_as and _saved_version == "1.1",
                      f"old INI is saved as rom_type = {saved_as}, config_version 1.1 (got {saved}, {_saved_version})")
            # formát 1.0 z doby před dynamickou tabulkou ROM měl jména správně
            got, saved = _boot(exe, tmp, "o_early", "JSS108C", False, cfg_version="1.0",
                               build_time="2026-01-15 10:00:00")
            check(got == "JSS108C" and saved == "JSS108C",
                  f"INI 1.0 built before 2026-03-08 is read by name (got {got}, saved {saved})")
            got, saved = _boot(exe, tmp, "v11", "WILLY_EN", False, cfg_version="1.1")
            check(got == "WILLY_EN" and saved == "WILLY_EN",
                  f"INI with config_version 1.1 is read by name (got {got}, saved {saved})")
    except TestFailure as exc:
        print(f"FAILED: {exc}")
        return 1
    print("PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
