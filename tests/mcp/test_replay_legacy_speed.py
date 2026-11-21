#!/usr/bin/env python3
"""E2E regresní test: záznam vstupů ze starší verze se přehraje i při rychlosti 150 %.

Záznamy v ``tests/data/replay/legacy-1564848f-*.mzr`` pořídila binárka
mz800new ``1564848f`` (před emulací bitu CKSW): smyčka ``JP 3000h``, CKSW = 0,
otisk po každém snímku, rychlost 100 % nebo 150 % a 1 nebo 3 snímky rozběhu
mezi nastavením rychlosti a začátkem záznamu.

Starší verze termín 20ms události (uzavírá zvukový log a dokrokovává PSG)
do snapshotu neukládala a při načtení ho neměnila. Při 150 % se tento termín
nekryje s hranicí snímku. Nová verze termín ukládá; aby se starší záznamy
přehrály stejně jako na původní verzi, nesmí

- termín u snapshotu bez klíče přepočítávat (při rychlosti jiné než 100 %),
- klíč s termínem zahrnout do otisku stavu (otisky v záznamu ho neznají).

Test pro každý záznam připraví stroj stejně jako při pořízení (stejná rychlost
a rozběh; starší záznam při jiné rychlosti než 100 % se přehraje jen ze stejně
připraveného procesu, to platilo i na původní verzi), záznam přehraje a čeká
výsledek ``ended``. Dřív přehrání při 150 % skončilo ``state differs at
frame 1 (periph)``.

Čtvrtý záznam (``legacy-1564848f-150-100.mzr``) vznikl po návratu rychlosti
ze 150 % na 100 %: termín zůstal uprostřed snímku, i když rychlost je 100 %.
Hlídá, že se termín u starého snapshotu nepřesouvá na hranici snímku jen
proto, že je právě nastaveno 100 %.

Snapshot ``legacy-1564848f-row156.mzs`` pořídila verze 1564848f po stejném
návratu ze 150 na 100 %, jednom dalším snímku a doběhnutí na řádek 156.
Dvě zkoušky načtou tento soubor, ihned ho přeuloží a porovnají otisky dalšího
snímku po přímém běhu a po obnovení z jiné fáze. Příprava přehrávače proběhne
jednou s dokončeným krátkým snímkem a jednou s trvale vypnutým CKSW.

Jen MZ-800. Binárka se hledá v kořeni repa nebo v adresáři z proměnné
``MZ_EMU_DIR``. Každý proces běží izolovaně (dočasný profil); zabíjí se jen
vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import gettext
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
import test_gdg_cksw as h  # noqa: E402 - PipeEmu a příprava stroje

_ = gettext.gettext

_DATA = Path(__file__).resolve().parent.parent / "data" / "replay"

#: (rychlost v procentech, snímky rozběhu před záznamem)
_CASES = [(100, 3), (150, 3), (150, 1)]

#: Záznam po návratu rychlosti: 1 snímek při 150 %, pak 100 % a 4 snímky;
#: termín 20ms události tím zůstal uprostřed snímku (čtvrtá kontrola
#: posudku v emu-experiments superimpose/cdx-posudek, odkud záznam pochází).
_RETURN_REC = "legacy-1564848f-150-100.mzr"

#: Počet snímků záznamu (make_legacy_replays.py v emu-experiments/superimpose/tools).
_FRAMES = 12


def run_case(exe, percent, warmup):
    """Přehraje jeden starší záznam ze stejně připraveného stroje."""
    rec = _DATA / f"legacy-1564848f-speed{percent}-warmup{warmup}.mzr"
    if not rec.is_file():
        raise h.TestFailure(f"missing test data {rec.name}")
    tmp = Path(tempfile.mkdtemp(prefix="replay_legacy_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        h._setup(emu)
        first = {"path": str(tmp / "initial.mzs")}
        emu.call("snapshot_save", first)
        emu.call("snapshot_load", first)
        emu.call("set_speed", {"mode": "custom", "percent": percent})
        emu.call("run", {"frames": warmup})
        emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
        if emu.call("replay_status")["mode"] == "playing":
            emu.call("run", {"frames": _FRAMES + 2})
        st = emu.call("replay_status")
        h.check(st.get("outcome") == "ended",
                f"recording from 1564848f at {percent} % (warm-up {warmup}) plays to its end"
                f" ({st.get('outcome')}: {st.get('outcome_message')})")
    finally:
        if emu is not None:
            emu.close()
        emu_test_proc.rmtree_retry(tmp)


def prepare_return(emu, tmp):
    """Uloží výchozí stav a posune fázi časovače přechodem ze 150 na 100 %."""
    first = {"path": str(tmp / "initial.mzs")}
    emu.call("snapshot_save", first)
    emu.call("snapshot_load", first)
    emu.call("set_speed", {"mode": "custom", "percent": 150})
    emu.call("run", {"frames": 1})
    emu.call("set_speed", {"mode": "custom", "percent": 100})
    emu.call("run", {"frames": 4})
    return first


def run_resave_case(exe, short):
    """Porovná přímé pokračování s obnovou nového snapshotu z jiné fáze časovače."""
    tmp = Path(tempfile.mkdtemp(prefix="legacy_resave_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        h._setup(emu)
        initial = prepare_return(emu, tmp)
        if short:
            h._cksw(emu, True)
            emu.call("run", {"frames": 1})
            h._goto(emu, 156, 100)
            h._cksw(emu, False)
        # Soubor pořídil původní emulátor na řádku 156, sloupci 110.
        emu.call("snapshot_load", {"path": str(_DATA / "legacy-1564848f-row156.mzs")})
        converted = {"path": str(tmp / "converted.mzs")}
        emu.call("snapshot_save", converted)
        parts = {"parts": ["cpu", "ram", "vram", "periph"]}
        emu.call("run", {"frames": 1})
        direct = emu.call("machine_hash", parts)["parts"]
        emu.call("snapshot_load", initial)
        emu.call("snapshot_load", converted)
        emu.call("run", {"frames": 1})
        resumed = emu.call("machine_hash", parts)["parts"]
        h.check(direct == resumed, _("Resaved legacy snapshot reproduces the next frame (CKSW={bit})").format(bit=int(short)))
    finally:
        if emu is not None:
            emu.close()
        emu_test_proc.rmtree_retry(tmp)


def run_return_case(exe, cksw_pos=None):
    """Starší záznam pořízený po návratu ze 150 % na 100 %.

    S ``cksw_pos`` (řádek, sloupec) se před přehráním v témže snímku krátce
    zapne CKSW, doběhne na danou pozici a bit se zase vypne: řádek se tím
    zkrátil, ale snímek ještě neskončil, takže termín 20ms události posunutý
    není a načtení staršího snapshotu ho nesmí přepočítat (pátá kontrola
    posudku).

    S ``cksw_pos`` rovným ``"frame"`` se s CKSW dokončí celý snímek (od
    řádku 311 do dalšího snímku): termín se tím posunul o zkrácení snímku
    a načtení staršího snapshotu má tento posun odečíst (šestá kontrola).
    """
    rec = _DATA / _RETURN_REC
    if not rec.is_file():
        raise h.TestFailure(f"missing test data {rec.name}")
    tmp = Path(tempfile.mkdtemp(prefix="replay_legacy_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        h._setup(emu)
        prepare_return(emu, tmp)
        what = "recording from 1564848f made after 150 % -> 100 % plays to its end"
        if cksw_pos == "frame":
            h._cksw(emu, True)
            h._goto(emu, 311, 1000)
            emu.call("run", {"frames": 1})
            h._cksw(emu, False)
            what += " after a completed short frame in the player"
        elif cksw_pos is not None:
            start = h._pos(emu)
            h._cksw(emu, True)
            pos = h._goto(emu, cksw_pos[0], cksw_pos[1])
            h._cksw(emu, False)
            if pos["frame"] != start["frame"]:
                raise h.TestFailure("the CKSW probe crossed a frame boundary")
            what += f" after a short line in the same frame (player at {pos['line']}/{pos['col']})"
        emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
        if emu.call("replay_status")["mode"] == "playing":
            emu.call("run", {"frames": _FRAMES + 2})
        st = emu.call("replay_status")
        h.check(st.get("outcome") == "ended",
                what + f" ({st.get('outcome')}: {st.get('outcome_message')})")
    finally:
        if emu is not None:
            emu.close()
        emu_test_proc.rmtree_retry(tmp)


def main():
    emu_test_proc.install(deadline_s=170)
    exe = h._find_exe("mz800emu")
    if exe is None:
        print("SKIP: no emulator binary found")
        return 77
    print(f"\n=== {exe.name} ===")
    ok = True
    for percent, warmup in _CASES:
        try:
            run_case(exe, percent, warmup)
        except h.TestFailure as e:
            print(f"  FAILED: {e}")
            ok = False
    # bez CKSW; po načtení bitu v řádku 0 (zkrácený běžící řádek); po konci
    # prvního krátkého řádku (zkrácení v součtu snímku)
    # a po dokončeném krátkém snímku (posun termínu se má odečíst)
    for cksw_pos in (None, (0, 900), (1, 170), "frame"):
        try:
            run_return_case(exe, cksw_pos)
        except h.TestFailure as e:
            print(f"  FAILED: {e}")
            ok = False
    for short in (True, False):
        try:
            run_resave_case(exe, short)
        except h.TestFailure as error:
            print(_("  FAILED: {error}").format(error=error))
            ok = False
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
