#!/usr/bin/env python3
"""E2E test úprav pásky přes MCP (pipe): import, přesun, smazání, uložení, export.

Příkazy JSONL ``cmt_tape_import``, ``cmt_tape_move_block``,
``cmt_tape_delete_blocks`` a ``cmt_export_tape`` (nástroje
``emu_cmt_tape_import``, ``emu_cmt_tape_move_block``,
``emu_cmt_tape_delete_blocks``, ``emu_cmt_export_tape``) a pole ``unsaved``
bloku v ``cmt_tape_list`` (resource ``emulator://periph/cmt/tape``).

  A  Import dvou MZF (první do prázdného decku = nová páska, druhý před
     blok 0), přesun, kontrola pořadí, ``unsaved`` a ``modified``.
  B  Uložení do TMZ (``unsaved`` i ``modified`` false), znovuotevření
     (stejné pořadí), import na konec (jen nový blok ``unsaved``), smazání
     prvního bloku (příznak ostatních zůstane), přesun na vlastní místo.
  C  Export celé pásky do WAV (soubor existuje, hlavička RIFF/WAVE, délka
     dat > 0), pásku nemění; chybná přípona = chyba.
  D  Chyby: mimo STOP ``tape is not stopped`` (páska beze změny), neplatný
     blok, prázdný a opakovaný seznam, nečitelný soubor importu.
  E  Záznam vstupů: smazání, přesun a import odmítne brána záznamu vstupů,
     export je povolený; při přehrávání vstupů úpravy odmítne páska sama.
  F  Import s ``at`` do prázdného decku = chyba, bez ``at`` nová páska.
  G  ``at`` < -1 a čísla nad INT_MAX odmítnuté, ``at`` null bez pásky =
     nová páska; ``block_id`` / ``block`` starších příkazů (rychlost bloku,
     posun, export bloku) jen jako celé číslo; varování taktu (blok 0x43
     jiného taktu) přes MCP a bez varování u výchozího taktu.
  H  Varování taktu u mazání a přesunu: smazání bloku 0x43
     jiného taktu a přesun programu přes něj vrátí ``warning``, mazání
     a přesun bez změny taktu ho nevrátí.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
s vypnutým CMT hackem, ``--no-save-ini``), bez TCP portu; zabíjí se jen
vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (bez binárky mz800emu).
"""

import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_record import PipeEmu, TestFailure, check, _find_exe  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
_PROG1 = _REPO_ROOT / "tests" / "data" / "cmt" / "prog1.mzf"
_PROG2 = _REPO_ROOT / "tests" / "data" / "cmt" / "prog2.mzf"
#: Jména bloků z testovacích MZF (pole name v cmt_tape_list).
_ONE = "TMZ PROG ONE"
_TWO = "TMZ PROG TWO"


def _tape(emu):
    """Seznam bloků vložené pásky (cmt_tape_list)."""
    return emu.call("cmt_tape_list")


def _names(tape):
    """Jména bloků v pořadí pásky."""
    return [b.get("name") for b in tape.get("blocks") or []]


def _unsaved(tape):
    """Příznaky unsaved bloků v pořadí pásky."""
    return [b.get("unsaved") for b in tape.get("blocks") or []]


def phase_a(emu, tmp):
    """Import do prázdného decku a před blok 0, přesun."""
    print("Phase A: import two MZF files, move, check order, unsaved and modified")
    emu.call("cmt_transport", {"action": "eject"})
    res = emu.call("cmt_tape_import", {"path": str(_PROG1)})
    check(res.get("imported") == 1 and res.get("at") == 0 and res.get("count") == 1
          and res.get("new_tape") is True, f"import into an empty deck creates a new tape ({res})")
    res = emu.call("cmt_tape_import", {"path": str(_PROG2), "at": 0})
    check(res.get("imported") == 1 and res.get("at") == 0 and res.get("count") == 2
          and res.get("new_tape") is False, f"import before block 0 ({res})")
    tape = _tape(emu)
    check(_names(tape) == [_TWO, _ONE], f"order after imports ({_names(tape)})")
    check(_unsaved(tape) == [True, True] and tape.get("modified") is True and tape.get("path") is None,
          f"imported blocks unsaved, tape modified, no path ({_unsaved(tape)}, {tape.get('modified')})")

    res = emu.call("cmt_tape_move_block", {"block_id": 0, "to": 1})
    check(res.get("block_id") == 0 and res.get("to") == 1 and res.get("count") == 2, f"move 0 -> 1 ({res})")
    tape = _tape(emu)
    check(_names(tape) == [_ONE, _TWO], f"order after move ({_names(tape)})")
    check(_unsaved(tape) == [True, True], f"move keeps unsaved flags ({_unsaved(tape)})")


def phase_b(emu, tmp):
    """Uložení, znovuotevření, import na konec, smazání, přesun na vlastní místo."""
    print("Phase B: save to TMZ, reopen, import at the end, delete, move to the same place")
    tmz = tmp / "edited.tmz"
    emu.call("cmt_save", {"path": str(tmz)})
    tape = _tape(emu)
    check(tape.get("modified") is False and _unsaved(tape) == [False, False],
          f"after save: not modified, no unsaved block ({tape.get('modified')}, {_unsaved(tape)})")
    emu.call("cmt_open", {"path": str(tmz), "play_immediately": False})
    tape = _tape(emu)
    check(_names(tape) == [_ONE, _TWO] and _unsaved(tape) == [False, False] and tape.get("modified") is False,
          f"reopened tape has the edited order ({_names(tape)}, {_unsaved(tape)})")

    res = emu.call("cmt_tape_import", {"path": str(_PROG1)})
    check(res.get("at") == 2 and res.get("count") == 3, f"import without 'at' appends ({res})")
    tape = _tape(emu)
    check(_names(tape) == [_ONE, _TWO, _ONE] and _unsaved(tape) == [False, False, True]
          and tape.get("modified") is True,
          f"only the imported block is unsaved ({_names(tape)}, {_unsaved(tape)})")

    res = emu.call("cmt_tape_delete_blocks", {"block_ids": [0]})
    check(res.get("deleted") == 1 and res.get("count") == 2, f"delete block 0 ({res})")
    tape = _tape(emu)
    check(_names(tape) == [_TWO, _ONE] and _unsaved(tape) == [False, True] and tape.get("modified") is True,
          f"delete keeps the flags of the other blocks ({_names(tape)}, {_unsaved(tape)})")

    res = emu.call("cmt_tape_move_block", {"block_id": 1, "to": 1})
    check(res.get("to") == 1 and _names(_tape(emu)) == [_TWO, _ONE], f"move to the same place ({res})")


def phase_c(emu, tmp):
    """Export celé pásky do WAV."""
    print("Phase C: export the whole tape to WAV")
    before = _tape(emu)
    wav = tmp / "tape.wav"
    res = emu.call("cmt_export_tape", {"path": str(wav)})
    check(wav.is_file() and res.get("path") == str(wav) and "Tape exported" in res.get("message", ""),
          f"tape exported ({res})")
    data = wav.read_bytes()
    check(len(data) > 44 and data[:4] == b"RIFF" and data[8:12] == b"WAVE", f"WAV file, {len(data)} B")
    rate, = struct.unpack_from("<I", data, 24)
    check(rate == 44100, f"44100 Hz ({rate})")
    after = _tape(emu)
    check(_names(after) == _names(before) and after.get("modified") == before.get("modified")
          and _unsaved(after) == _unsaved(before), "export does not change the tape")
    bad = tmp / "tape.tmz"
    resp = emu.call_raw("cmt_export_tape", {"path": str(bad)})
    check(not resp.get("success") and not bad.exists(), f"export with a wrong extension fails ({resp.get('error')})")


def phase_d(emu, tmp):
    """Chyby: mimo STOP, neplatný blok, prázdný a opakovaný seznam, nečitelný soubor."""
    print("Phase D: errors outside STOP, bad block, bad list, unreadable file")
    before = _names(_tape(emu))
    emu.call("cmt_transport", {"action": "play"})
    for cmd, data in (("cmt_tape_delete_blocks", {"block_ids": [0]}),
                      ("cmt_tape_move_block", {"block_id": 0, "to": 1}),
                      ("cmt_tape_import", {"path": str(_PROG2)})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "tape is not stopped" in (resp.get("error") or ""),
              f"{cmd} refused outside STOP ({resp.get('error')})")
    emu.call("cmt_transport", {"action": "stop"})
    check(_names(_tape(emu)) == before, "tape unchanged after refused edits")

    # pauza a nahrávání: úpravy odmítnuté stejně jako při PLAY
    for state in ("pause", "record"):
        if state == "pause":
            emu.call("cmt_transport", {"action": "play"})
            emu.call("cmt_transport", {"action": "pause", "pause": 1})
        else:
            emu.call("cmt_record")
        for cmd, data in (("cmt_tape_move_block", {"block_id": 0, "to": 1}),
                          ("cmt_tape_import", {"path": str(_PROG2)})):
            resp = emu.call_raw(cmd, data)
            check(not resp.get("success") and "tape is not stopped" in (resp.get("error") or ""),
                  f"{cmd} refused in {state} ({resp.get('error')})")
        emu.call("cmt_transport", {"action": "stop"})
    check(_names(_tape(emu)) == before, "tape unchanged after edits refused in pause and record")

    # jen JSON celá čísla: řetězec, desetinné číslo ani bool se nepřevedou na pozici
    for cmd, data in (("cmt_tape_move_block", {"block_id": 0, "to": "1"}),
                      ("cmt_tape_move_block", {"block_id": 0.9, "to": 1}),
                      ("cmt_tape_move_block", {"block_id": 0, "to": True}),
                      ("cmt_tape_import", {"path": str(_PROG2), "at": "end"}),
                      ("cmt_tape_import", {"path": str(_PROG2), "at": 1.5}),
                      ("cmt_tape_import", {"path": str(_PROG2), "at": False})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "integer" in (resp.get("error") or "")
              or not resp.get("success") and "block position" in (resp.get("error") or ""),
              f"{cmd} {data} refused as invalid params ({resp.get('error')})")
    check(_names(_tape(emu)) == before, "tape unchanged after edits with non-integer positions")

    resp = emu.call_raw("cmt_tape_delete_blocks", {"block_ids": [7]})
    check(not resp.get("success") and "Block 7 does not exist" in (resp.get("error") or ""),
          f"delete of a missing block fails ({resp.get('error')})")
    resp = emu.call_raw("cmt_tape_delete_blocks", {"block_ids": []})
    check(not resp.get("success"), f"empty block list fails ({resp.get('error')})")
    resp = emu.call_raw("cmt_tape_delete_blocks", {"block_ids": [1, 1]})
    check(not resp.get("success"), f"repeated block fails ({resp.get('error')})")
    resp = emu.call_raw("cmt_tape_move_block", {"block_id": 0, "to": 5})
    check(not resp.get("success") and "out of range" in (resp.get("error") or ""),
          f"move out of range fails ({resp.get('error')})")
    resp = emu.call_raw("cmt_tape_import", {"path": str(tmp / "missing.mzf")})
    check(not resp.get("success") and "Can't import" in (resp.get("error") or ""),
          f"import of a missing file fails ({resp.get('error')})")
    check(_names(_tape(emu)) == before, "tape unchanged after failed edits")


def phase_e(emu, tmp):
    """Záznam a přehrávání vstupů: úpravy odmítnuté, export povolený."""
    print("Phase E: edits refused while an input replay is recorded or played, export allowed")
    before = _names(_tape(emu))
    mzr = tmp / "edit.mzr"
    emu.call("replay_record_start", {"path": str(mzr), "hash_interval": 10})
    for cmd, data in (("cmt_tape_delete_blocks", {"block_ids": [0]}),
                      ("cmt_tape_move_block", {"block_id": 0, "to": 1}),
                      ("cmt_tape_import", {"path": str(_PROG2)})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "not allowed while recording a replay" in (resp.get("error") or ""),
              f"{cmd} refused while an input replay is recorded ({resp.get('error')})")
    wav = tmp / "rec_export.wav"
    emu.call("cmt_export_tape", {"path": str(wav)})
    check(wav.is_file(), "cmt_export_tape allowed while an input replay is recorded")
    emu.call("run", {"frames": 5})
    st = emu.call("replay_record_stop")
    check(st.get("outcome") == "recorded", f"input replay recorded ({st.get('outcome_message')})")
    check(_names(_tape(emu)) == before, "tape unchanged during the input recording")

    st = emu.call("replay_play", {"path": str(mzr), "pause_after_load": True})
    check(st.get("mode") == "playing", f"input replay playing ({st})")
    resp = emu.call_raw("cmt_tape_delete_blocks", {"block_ids": [0]})
    check(not resp.get("success") and "input replay" in (resp.get("error") or ""),
          f"delete refused while an input replay is played ({resp.get('error')})")
    for cmd, data in (("cmt_tape_move_block", {"block_id": 0, "to": 1}),
                      ("cmt_tape_import", {"path": str(_PROG2)})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "input replay" in (resp.get("error") or ""),
              f"{cmd} refused while an input replay is played ({resp.get('error')})")
    emu.call("replay_play_stop")


def phase_f(emu, tmp):
    """Import s at bez pásky: chyba; bez at do nové pásky."""
    print("Phase F: import with an explicit at into an empty deck fails, without at it creates a tape")
    emu.call("cmt_transport", {"action": "eject"})
    resp = emu.call_raw("cmt_tape_import", {"path": str(_PROG1), "at": 0})
    check(not resp.get("success") and "no tape in the deck" in (resp.get("error") or ""),
          f"import with at into an empty deck fails ({resp.get('error')})")
    check(not _tape(emu).get("available"), "the deck stays empty")
    res = emu.call("cmt_tape_import", {"path": str(_PROG1)})
    check(res.get("new_tape") is True and res.get("count") == 1, f"import without at creates a tape ({res})")


_TWO_PROGRAMS = _REPO_ROOT / "tests" / "data" / "cmt" / "two_programs.tmz"


def _clock_tmz(path, clock):
    """Zapíše TMZ: blok 0x43 (MZ Machine Info, takt ``clock`` Hz) a za ním dva bloky 0x46 z two_programs.tmz.

    Blok 0x43 na disku: ID, délka dat LE32 (6), stroj (2 = MZ-800), takt LE32,
    verze ROM (tmz_blocks.c: tmz_block_create_mz_machine_info()). Hlavička
    TMZ má 10 bajtů ("TapeMZ!" 0x1A, verze 1.1).
    """
    src = _TWO_PROGRAMS.read_bytes()
    head, blocks = src[:10], src[10:]
    mi = bytes([0x43]) + struct.pack("<I", 6) + bytes([2]) + struct.pack("<I", clock) + bytes([0])
    path.write_bytes(head + mi + blocks)


def phase_g(emu, tmp):
    """Celá čísla v mezích, null, starší příkazy pásky, varování taktu přes MCP."""
    print("Phase G: integer limits, null, block numbers of older tape commands, clock warning")
    emu.call("cmt_transport", {"action": "eject"})
    # at: null bez pásky = nová páska (jako bez at)
    res = emu.call("cmt_tape_import", {"path": str(_PROG1), "at": None})
    check(res.get("new_tape") is True and res.get("count") == 1, f"import with at null creates a tape ({res})")
    emu.call("cmt_tape_import", {"path": str(_PROG2)})
    before = _names(_tape(emu))
    check(before == [_ONE, _TWO], f"two blocks ({before})")
    for cmd, data in (("cmt_tape_import", {"path": str(_PROG2), "at": -2}),
                      ("cmt_tape_import", {"path": str(_PROG2), "at": 2 ** 31}),
                      ("cmt_tape_move_block", {"block_id": 0, "to": 2 ** 31}),
                      ("cmt_tape_move_block", {"block_id": 2 ** 31, "to": 0}),
                      ("cmt_tape_move_block", {"block_id": None, "to": 1})):
        resp = emu.call_raw(cmd, data)
        err = resp.get("error") or ""
        check(not resp.get("success") and ("integer" in err or "block position" in err or "Missing" in err),
              f"{cmd} {data} refused as invalid params ({err})")
    check(_names(_tape(emu)) == before, "tape unchanged after out-of-range positions")

    # starší příkazy: block_id / block jen jako JSON celé číslo ("1" se nečte jako blok 0)
    tape0 = _tape(emu)
    speed0 = [b.get("cmt_speed") for b in tape0.get("blocks") or []]
    for cmd, data in (("cmt_tape_block_speed", {"block_id": "1", "speed": 3}),
                      ("cmt_tape_block_speed", {"block_id": 0.5, "speed": 3}),
                      ("cmt_tape_seek", {"block_id": "1"}),
                      ("cmt_tape_seek", {"block_id": True}),
                      ("cmt_export_block", {"block": "1", "path": str(tmp / "blk.mzf")})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "must be an integer" in (resp.get("error") or ""),
              f"{cmd} {data} refused ({resp.get('error')})")
    tape1 = _tape(emu)
    check([b.get("cmt_speed") for b in tape1.get("blocks") or []] == speed0, "no block speed changed")
    check(not (tmp / "blk.mzf").exists(), "no block exported")
    res = emu.call("cmt_tape_seek", {"block_id": 1})
    check(res.get("block_id") == 1, f"seek with an integer works ({res})")

    # varování taktu přes MCP: soubor s 0x43 jiného taktu doprostřed pásky bez 0x43
    clock_word = "The tape clock setting now applies to different blocks"
    other = tmp / "clock_other.tmz"
    _clock_tmz(other, 4000000)
    res = emu.call("cmt_tape_import", {"path": str(other), "at": 1})
    check(clock_word in (res.get("warning") or ""), f"clock warning through MCP ({res})")
    # 0x43 s výchozím taktem (3546895 Hz): hraje se stejně, bez varování
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(_PROG1)})
    emu.call("cmt_tape_import", {"path": str(_PROG2)})
    same = tmp / "clock_default.tmz"
    _clock_tmz(same, 3546895)
    res = emu.call("cmt_tape_import", {"path": str(same), "at": 1})
    check(clock_word not in (res.get("warning") or ""), f"no clock warning for the default clock ({res})")



def phase_h(emu, tmp):
    """Mazání a přesun vrátí varování o taktu (společný kanál varování úprav)."""
    print("Phase H: clock warning of delete and move")
    clock_word = "The tape clock setting now applies to different blocks"
    other = tmp / "clock_h.tmz"
    _clock_tmz(other, 4000000)
    # páska: 0x43 (4 MHz), program, program
    emu.call("cmt_transport", {"action": "eject"})
    res = emu.call("cmt_tape_import", {"path": str(other)})
    check(res.get("count") == 3, f"three blocks ({res})")
    # přesun druhého programu před první: oba za blokem 0x43, takt se nemění
    res = emu.call("cmt_tape_move_block", {"block_id": 2, "to": 1})
    check("warning" not in res, f"move behind the same clock block has no warning ({res})")
    # přesun programu před blok 0x43: hraje výchozím taktem
    res = emu.call("cmt_tape_move_block", {"block_id": 1, "to": 0})
    check(clock_word in (res.get("warning") or ""), f"move across the clock block warns ({res})")
    # smazání programu na začátku (výchozí takt): ostatní bloky beze změny taktu
    res = emu.call("cmt_tape_delete_blocks", {"block_ids": [0]})
    check("warning" not in res and res.get("count") == 2, f"delete without a clock change has no warning ({res})")
    # smazání bloku 0x43: zbylý program hraje výchozím taktem
    res = emu.call("cmt_tape_delete_blocks", {"block_ids": [0]})
    check(clock_word in (res.get("warning") or "") and res.get("count") == 1,
          f"deleting the clock block warns ({res})")


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe("mz800emu")
    if exe is None:
        print("SKIP: mz800emu binary not found")
        return 77
    failures = []
    with tempfile.TemporaryDirectory(prefix="mz_cmt_edit_") as td:
        tmp = Path(td)
        emu = PipeEmu(exe, tmp / "cfg")
        try:
            emu.call("run", {"frames": 350})
            for label, fn in (("A", phase_a), ("B", phase_b), ("C", phase_c), ("D", phase_d), ("E", phase_e), ("F", phase_f),
                              ("G", phase_g), ("H", phase_h)):
                try:
                    fn(emu, tmp)
                except TestFailure as exc:
                    failures.append(f"phase {label}: {exc}")
                    print(f"  FAIL: {exc}")
                    break
        finally:
            emu.close()
    if failures:
        print("FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
