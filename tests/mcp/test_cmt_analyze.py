#!/usr/bin/env python3
"""E2E test nástrojů nad páskou přes MCP (pipe): analýza, informace o pásce, kontrola, převod.

Příkazy JSONL ``cmt_tape_analyze``, ``cmt_tape_info``, ``cmt_tape_check``
a ``cmt_tape_convert_block`` (nástroje ``emu_cmt_tape_analyze``,
``emu_cmt_tape_info``, ``emu_cmt_tape_check``, ``emu_cmt_tape_convert_block``)
a pole ``profiles``, ``profile`` a ``convertible`` v ``cmt_tape_list``.

SAVE z ROM v emulátoru se dekóduje sám, proto test vyrobí surový blok
exportem bloku do WAV a importem toho WAV (jeden blok 0x18).

  A  Surový blok: import MZF do prázdného decku, export bloku do WAV,
     import WAV na konec. Analýza bez uplatnění (řádek souboru addable,
     páska beze změny, i při PLAY), chyby (blok 0x46, mimo STOP s apply,
     přísné typy parametrů).
  B  Replace: blok souboru se jménem na místě surového, export MZF bajtově
     shodný s původním MZF.
  C  Informace o pásce: čtení, zápis titulu a stroje, varování (znak mimo
     ISO 8859-1), následný převod bez varování nenese varování, uložení
     do TMZ a znovuotevření, neplatné ``set``.
  D  Kontrola pásky.
  E  Převod profilu: nabídka profilů, ROM-800-NOMINAL, tentýž profil beze
     změny, rychlost u profilu bez rychlosti, neznámý profil, zpět na
     MZ800EMU-CMT s rychlostí.
  F  Záznam vstupů: čtení (analýza bez uplatnění, informace, kontrola)
     povolené, změny (Replace, zápis informací, převod) odmítne brána.
  G  Uplatnění bez souboru, který jde přidat (WAV jen se šumem), čtecí
     varianty bez ``state_rev`` a s odmítnutým ``expected_state_rev``,
     rychlost převodu jen jako celé číslo, profil jiného typu.
  H  Varování o taktu: blok 0x43 s taktem 4 MHz před
     surovým blokem - Add i Replace vrátí ``warning`` s větou o taktu; bez
     bloku 0x43 (fáze B) ``warning`` chybí.

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
#: Jméno bloku z testovacího MZF (pole name v cmt_tape_list).
_ONE = "TMZ PROG ONE"


def _tape(emu):
    """Seznam bloků vložené pásky (cmt_tape_list)."""
    return emu.call("cmt_tape_list")


def _names(tape):
    """Jména bloků v pořadí pásky."""
    return [b.get("name") for b in tape.get("blocks") or []]


def _block_of(tape, name):
    """Číslo prvního bloku se jménem ``name`` (-1 = žádný)."""
    for b in tape.get("blocks") or []:
        if b.get("name") == name:
            return b.get("block_id")
    return -1


def _err(resp):
    """Text chyby odpovědi."""
    return resp.get("error") or ""


def phase_a(emu, tmp):
    """Surový blok z exportu do WAV, analýza bez uplatnění, chyby."""
    print("Phase A: raw block from a WAV export, analysis without apply, errors")
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(_PROG1)})
    wav = tmp / "one.wav"
    emu.call("cmt_export_block", {"block": 0, "path": str(wav)})
    check(wav.is_file(), "block exported to WAV")
    res = emu.call("cmt_tape_import", {"path": str(wav)})
    check(res.get("at") == 1 and res.get("count") == 2, f"WAV imported as block 1 ({res})")
    tape = _tape(emu)
    check(tape["blocks"][1].get("type") == 0, f"block 1 is a raw WAV block ({tape['blocks'][1]})")

    res = emu.call("cmt_tape_analyze", {"block_id": 1})
    check(res.get("apply") == "none" and res.get("applied") == 0 and res.get("first_added") is None,
          f"analysis without apply ({res.get('apply')}, {res.get('applied')})")
    check(res.get("sample_rate") == 44100 and res.get("pulse_count", 0) > 1000 and res.get("length_s", 0) > 1,
          f"block data ({res.get('sample_rate')}, {res.get('pulse_count')}, {res.get('length_s')})")
    check("analysis_error" not in res, f"no analysis error ({res.get('analysis_error')})")
    files = [r for r in res.get("rows") or [] if r.get("kind") == "file"]
    check(len(files) == 1, f"one file row ({res.get('rows')})")
    row = files[0]
    check(row.get("name") == _ONE and row.get("type") == 1 and row.get("format") == "NORMAL"
          and row.get("header") == "ok" and row.get("body") == "ok" and row.get("result") == "addable"
          and row.get("reason") == "" and row.get("size", 0) > 0 and row.get("length_s", 0) > 0
          and row.get("start_s", -1) >= 0,
          f"file row fields ({row})")
    for r in res.get("rows") or []:
        if r.get("kind") == "unknown":
            check(r.get("result") == "stays_raw" and r.get("reason") and r.get("type") is None
                  and r.get("header") == "not_available",
                  f"unknown row fields ({r})")
    check(_names(_tape(emu)) == _names(tape), "analysis without apply does not change the tape")

    # chyby
    resp = emu.call_raw("cmt_tape_analyze", {"block_id": 0})
    check(not resp.get("success") and "is not a raw signal block" in _err(resp),
          f"analysis of a file block fails ({_err(resp)})")
    resp = emu.call_raw("cmt_tape_analyze", {"block_id": 9})
    check(not resp.get("success") and "Block 9 does not exist" in _err(resp),
          f"analysis of a missing block fails ({_err(resp)})")
    for data in ({"block_id": "1"}, {"block_id": 1.0}, {"block_id": True}, {},
                 {"block_id": 1, "tolerance": "0.2"}, {"block_id": 1, "tolerance": True},
                 {"block_id": 1, "repair_body": 1}, {"block_id": 1, "repair_header": "true"},
                 {"block_id": 1, "apply": "all"}, {"block_id": 1, "apply": True},
                 {"block_id": 1, "apply": 2},
                 {"block_id": 1, "tolerance": 0.5}, {"block_id": 1, "tolerance": 0.01}):
        resp = emu.call_raw("cmt_tape_analyze", data)
        check(not resp.get("success"), f"invalid params refused {data} ({_err(resp)})")
    resp = emu.call_raw("cmt_tape_analyze", {"block_id": 1, "tolerance": 0.5})
    check("between 0.02 and 0.35" in _err(resp), f"tolerance out of range is an error ({_err(resp)})")
    # čtecí varianta: bez state_rev, expected_state_rev odmítnutý
    res = emu.call("cmt_tape_analyze", {"block_id": 1, "tolerance": 0.35})
    check("state_rev" not in res, f"read-only analysis returns no state_rev ({sorted(res)})")
    resp = emu.call_raw("cmt_tape_analyze", {"block_id": 1, "expected_state_rev": 1})
    check(not resp.get("success") and "expected_state_rev" in _err(resp),
          f"read-only analysis refuses expected_state_rev ({_err(resp)})")
    res = emu.call("cmt_tape_analyze", {"block_id": 1, "tolerance": 0.25, "repair_body": False,
                                        "repair_header": None, "apply": "none"})
    check(res.get("applied") == 0, "valid options with null accepted")

    # PLAY: čtení jde, uplatnění ne
    emu.call("cmt_transport", {"action": "play"})
    res = emu.call("cmt_tape_analyze", {"block_id": 1})
    check(len(res.get("rows") or []) >= 1, "analysis without apply works while playing")
    resp = emu.call_raw("cmt_tape_analyze", {"block_id": 1, "apply": "replace"})
    check(not resp.get("success") and "tape is not stopped" in _err(resp),
          f"replace refused outside STOP ({_err(resp)})")
    emu.call("cmt_transport", {"action": "stop"})
    check(_names(_tape(emu)) == _names(tape), "tape unchanged")


def phase_b(emu, tmp):
    """Replace: blok souboru se jménem, MZF bajtově shodné s původním."""
    print("Phase B: replace the raw block, exported MZF equals the original")
    res = emu.call("cmt_tape_analyze", {"block_id": 1, "apply": "replace"})
    check(res.get("apply") == "replace" and res.get("applied") == 1, f"one file added ({res.get('applied')})")
    check(isinstance(res.get("state_rev"), int), "a changing analysis returns state_rev")
    first = res.get("first_added")
    tape = _tape(emu)
    check(isinstance(first, int) and 1 <= first < tape.get("count", 0)
          and tape["blocks"][first].get("name") == _ONE,
          f"first added block has the file name ({first}, {_names(tape)})")
    check(res.get("count") == tape.get("count"), f"count matches the tape ({res.get('count')})")
    blk = tape["blocks"][first]
    check(blk.get("unsaved") is True and tape.get("modified") is True, "new block unsaved, tape modified")
    check(blk.get("type") == 1 and blk.get("profile") == "MZ800EMU-CMT", f"file block is an MZF block ({blk})")
    check("warning" not in res, f"no clock warning on a tape without a machine info block ({res.get('warning')})")
    out = tmp / "added.mzf"
    emu.call("cmt_export_block", {"block": first, "path": str(out)})
    check(out.read_bytes() == _PROG1.read_bytes(), "exported MZF equals the original MZF byte for byte")


def phase_c(emu, tmp):
    """Informace o pásce: čtení, zápis, varování, uložení a znovuotevření."""
    print("Phase C: tape information read, write, warning, save and reopen")
    info = emu.call("cmt_tape_info")
    check(info.get("title") == "" and info.get("machine") is None and info.get("machine_code") == -1
          and info.get("storable") is True and "changed" not in info,
          f"empty tape information ({info})")
    count0 = _tape(emu).get("count")
    res = emu.call("cmt_tape_info", {"set": {"title": "Analyze test", "machine": "mz800"}})
    check(res.get("changed") is True and res.get("title") == "Analyze test" and res.get("machine") == "mz800"
          and res.get("machine_code") == 2 and "warning" not in res,
          f"title and machine written ({res})")
    tape = _tape(emu)
    check(tape.get("count") == count0 + 2, f"Archive Info and Machine Info inserted ({_names(tape)})")
    res = emu.call("cmt_tape_info", {"set": {"title": "Analyze test", "machine": "mz800"}})
    check(res.get("changed") is False, "same values change nothing")

    res = emu.call("cmt_tape_info", {"set": {"author": "Řehoř"}})
    check(res.get("changed") is True and res.get("title") == "Analyze test" and res.get("author") == "?eho?"
          and "Characters outside ISO 8859-1" in (res.get("warning") or ""),
          f"merge keeps other fields, replaced characters warned ({res})")
    # varování nepatří další operaci (kontrola odpovědi; převod maže varování sám,
    # opravu kanálu v jádře pojišťuje C test test_edit_warning_does_not_leak)
    one = _block_of(_tape(emu), _ONE)
    res = emu.call("cmt_tape_convert_block", {"block_id": one, "profile": "MZ800EMU-CMT", "speed": 2})
    check(res.get("changed") is True and "warning" not in res,
          f"later conversion without a warning carries none ({res})")

    # neplatné set
    for data in ({"set": {"titel": "x"}}, {"set": {"title": 5}}, {"set": {"machine": "zx"}},
                 {"set": {"machine": 2}}, {"set": "title"}, {"set": {"comment": "x" * 600}}):
        resp = emu.call_raw("cmt_tape_info", data)
        check(not resp.get("success"), f"invalid set refused {str(data)[:60]} ({_err(resp)})")

    # uložení a znovuotevření
    tmz = tmp / "info.tmz"
    emu.call("cmt_save", {"path": str(tmz)})
    emu.call("cmt_open", {"path": str(tmz), "play_immediately": False})
    info = emu.call("cmt_tape_info")
    check(info.get("title") == "Analyze test" and info.get("author") == "?eho?" and info.get("machine") == "mz800",
          f"information survives save and reopen ({info})")
    res = emu.call("cmt_tape_info", {"set": {"machine": None, "title": "", "author": ""}})
    check(res.get("changed") is True and res.get("machine") is None and res.get("title") == "",
          f"empty information removes the blocks ({res})")
    check(_tape(emu).get("count") == count0, f"blocks removed ({_names(_tape(emu))})")


def phase_d(emu, tmp):
    """Kontrola pásky."""
    print("Phase D: tape check")
    res = emu.call("cmt_tape_check")
    tape = _tape(emu)
    check(res.get("blocks_checked") == tape.get("count") and res.get("count") == len(res.get("findings") or []),
          f"check covers all blocks ({res})")
    for f in res.get("findings") or []:
        check(f.get("severity") in ("warning", "info") and isinstance(f.get("text"), str), f"finding ({f})")
    emu.call("cmt_transport", {"action": "eject"})
    resp = emu.call_raw("cmt_tape_check", {})
    check(not resp.get("success") and "No tape" in _err(resp), f"check without a tape fails ({_err(resp)})")
    resp = emu.call_raw("cmt_tape_info", {})
    check(not resp.get("success") and "No tape" in _err(resp), f"info without a tape fails ({_err(resp)})")
    emu.call("cmt_open", {"path": str(tmp / "info.tmz"), "play_immediately": False})


def phase_e(emu, tmp):
    """Převod profilu."""
    print("Phase E: profile conversion")
    tape = _tape(emu)
    profiles = {p.get("id"): p for p in tape.get("profiles") or []}
    check("MZ800EMU-CMT" in profiles and profiles["MZ800EMU-CMT"].get("accepts_speed") is True
          and "ROM-800-NOMINAL" in profiles and profiles["ROM-800-NOMINAL"].get("accepts_speed") is False
          and all(p.get("description") for p in profiles.values()),
          f"profile offer ({sorted(profiles)})")
    one = _block_of(tape, _ONE)
    blk = tape["blocks"][one]
    check(blk.get("convertible") is True and blk.get("profile") == "MZ800EMU-CMT", f"block profile ({blk})")
    check(all(b.get("profile") is None and b.get("convertible") is False
              for b in tape["blocks"] if b.get("type") != 1), "blocks without MZ file have no profile")

    res = emu.call("cmt_tape_convert_block", {"block_id": one, "profile": "ROM-800-NOMINAL"})
    check(res.get("changed") is True and res.get("profile") == "ROM-800-NOMINAL", f"converted ({res})")
    res = emu.call("cmt_tape_convert_block", {"block_id": one, "profile": "ROM-800-NOMINAL"})
    check(res.get("changed") is False, f"same profile without a speed changes nothing ({res})")
    resp = emu.call_raw("cmt_tape_convert_block", {"block_id": one, "profile": "ROM-800-NOMINAL", "speed": 2})
    check(not resp.get("success") and "does not accept a speed" in _err(resp), f"speed refused ({_err(resp)})")
    resp = emu.call_raw("cmt_tape_convert_block", {"block_id": one, "profile": "NO-SUCH"})
    check(not resp.get("success") and "Unknown profile: NO-SUCH" in _err(resp), f"unknown profile ({_err(resp)})")
    for data in ({"block_id": one, "profile": "MZ800EMU-CMT", "speed": "2"},
                 {"block_id": one, "profile": "MZ800EMU-CMT", "speed": 10},
                 {"block_id": str(one), "profile": "MZ800EMU-CMT"},
                 {"block_id": one, "profile": 5}, {"block_id": one}):
        resp = emu.call_raw("cmt_tape_convert_block", data)
        check(not resp.get("success"), f"invalid params refused {data} ({_err(resp)})")
    emu.call("cmt_transport", {"action": "play"})
    resp = emu.call_raw("cmt_tape_convert_block", {"block_id": one, "profile": "MZ800EMU-CMT"})
    check(not resp.get("success") and "tape is not stopped" in _err(resp), f"refused outside STOP ({_err(resp)})")
    emu.call("cmt_transport", {"action": "stop"})
    res = emu.call("cmt_tape_convert_block", {"block_id": one, "profile": "MZ800EMU-CMT", "speed": 2})
    check(res.get("changed") is True and res.get("profile") == "MZ800EMU-CMT", f"converted back ({res})")


def phase_f(emu, tmp):
    """Záznam vstupů: čtení povolené, změny odmítnuté branou."""
    print("Phase F: reads allowed, changes refused while an input replay is recorded")
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(_PROG1)})
    wav = tmp / "rec.wav"
    emu.call("cmt_export_block", {"block": 0, "path": str(wav)})
    emu.call("cmt_tape_import", {"path": str(wav)})
    before = _names(_tape(emu))
    mzr = tmp / "tools.mzr"
    emu.call("replay_record_start", {"path": str(mzr), "hash_interval": 10})
    res = emu.call("cmt_tape_analyze", {"block_id": 1})
    check(any(r.get("result") == "addable" for r in res.get("rows") or []), "analysis allowed while recording")
    emu.call("cmt_tape_info")
    emu.call("cmt_tape_check")
    for cmd, data in (("cmt_tape_analyze", {"block_id": 1, "apply": "replace"}),
                      ("cmt_tape_info", {"set": {"title": "x"}}),
                      ("cmt_tape_convert_block", {"block_id": 0, "profile": "ROM-800-NOMINAL"})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "not allowed while recording a replay" in _err(resp),
              f"{cmd} refused while an input replay is recorded ({_err(resp)})")
    emu.call("run", {"frames": 5})
    st = emu.call("replay_record_stop")
    check(st.get("outcome") == "recorded", f"input replay recorded ({st.get('outcome_message')})")
    check(_names(_tape(emu)) == before, "tape unchanged during the input recording")


def _noise_wav(path):
    """Zapíše WAV 44 100 Hz, 8 bit mono: 2 s pulzů pseudonáhodné délky bez souboru."""
    samples = bytearray()
    level = 0
    x = 12345
    while len(samples) < 88200:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        samples += bytes([0xE0 if level else 0x20]) * (3 + x % 40)
        level ^= 1
    data = bytes(samples)
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 44100, 1, 8)
           + b"data" + struct.pack("<I", len(data)))
    path.write_bytes(hdr + data)


def phase_g(emu, tmp):
    """Uplatnění bez souboru, který jde přidat; čtecí varianty bez state_rev; texty chyb."""
    print("Phase G: apply without addable rows, read-only variants, error texts")
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(_PROG1)})
    noise = tmp / "noise.wav"
    _noise_wav(noise)
    emu.call("cmt_tape_import", {"path": str(noise)})
    before = _names(_tape(emu))
    res = emu.call("cmt_tape_analyze", {"block_id": 1, "apply": "add"})
    check(res.get("applied") == 0 and res.get("first_added") is None
          and not any(r.get("result", "").startswith("addable") for r in res.get("rows") or []),
          f"apply without an addable row adds nothing ({res.get('applied')}, {res.get('first_added')})")
    check(_names(_tape(emu)) == before, "tape unchanged")

    info = emu.call("cmt_tape_info")
    check("state_rev" not in info, f"reading the tape information returns no state_rev ({sorted(info)})")
    resp = emu.call_raw("cmt_tape_info", {"expected_state_rev": 1})
    check(not resp.get("success") and "expected_state_rev" in _err(resp),
          f"reading the tape information refuses expected_state_rev ({_err(resp)})")
    res = emu.call("cmt_tape_info", {"set": {"title": "G"}})
    check(isinstance(res.get("state_rev"), int) and res.get("changed") is True, "a write returns state_rev")

    # převod: JSONL bere jen celé číslo rychlosti, ne klíč poměru; profil jiného typu
    resp = emu.call_raw("cmt_tape_convert_block", {"block_id": 1, "profile": "MZ800EMU-CMT", "speed": "2:1"})
    check(not resp.get("success") and "speed" in _err(resp), f"speed as a ratio key refused by the command ({_err(resp)})")
    resp = emu.call_raw("cmt_tape_convert_block", {"block_id": 1, "profile": 5})
    check(not resp.get("success") and "profile must be a non-empty string" in _err(resp),
          f"profile of another type ({_err(resp)})")
    res = emu.call("cmt_tape_convert_block", {"block_id": 1, "profile": "MZ800EMU-CMT", "speed": 4})
    check(res.get("changed") is True, f"speed as an integer accepted ({res})")


_TWO_PROGRAMS = _REPO_ROOT / "tests" / "data" / "cmt" / "two_programs.tmz"
#: Věta varování o taktu (CMTTMZ_CLOCK_WARNING, cmt_tmz.h).
_CLOCK_WORD = "The tape clock setting now applies to different blocks"


def _clock_tmz(path, clock):
    """Zapíše TMZ: blok 0x43 (MZ Machine Info, takt ``clock`` Hz) a za ním dva bloky 0x46 z two_programs.tmz.

    Blok 0x43 na disku: ID, délka dat LE32 (6), stroj (2 = MZ-800), takt LE32,
    verze ROM (jako test_cmt_edit.py). Hlavička TMZ má 10 bajtů.
    """
    src = _TWO_PROGRAMS.read_bytes()
    head, blocks = src[:10], src[10:]
    mi = bytes([0x43]) + struct.pack("<I", 6) + bytes([2]) + struct.pack("<I", clock) + bytes([0])
    path.write_bytes(head + mi + blocks)


def phase_h(emu, tmp):
    """Nové bloky souborů za blokem 0x43 s jiným taktem - Add i Replace varují."""
    print("Phase H: clock warning after add and replace behind a machine info block with another clock")
    clock = tmp / "clock_4mhz.tmz"
    _clock_tmz(clock, 4000000)
    wav = tmp / "one.wav"
    check(wav.is_file(), "WAV of phase A exists")
    for apply in ("add", "replace"):
        emu.call("cmt_transport", {"action": "eject"})
        emu.call("cmt_tape_import", {"path": str(clock)})
        res = emu.call("cmt_tape_import", {"path": str(wav)})
        raw = res.get("at")
        check(raw == 3, f"raw block after the machine info block and two programs ({res})")
        res = emu.call("cmt_tape_analyze", {"block_id": raw, "apply": apply})
        check(res.get("applied") == 1, f"{apply}: one file added ({res.get('applied')})")
        warning = res.get("warning") or ""
        check(_CLOCK_WORD in warning and warning.count(_CLOCK_WORD) == 1,
              f"{apply}: the clock warning once ({warning!r})")


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe("mz800emu")
    if exe is None:
        print("SKIP: mz800emu binary not found")
        return 77
    failures = []
    with tempfile.TemporaryDirectory(prefix="mz_cmt_analyze_") as td:
        tmp = Path(td)
        emu = PipeEmu(exe, tmp / "cfg")
        try:
            emu.call("run", {"frames": 350})
            for label, fn in (("A", phase_a), ("B", phase_b), ("C", phase_c), ("D", phase_d), ("E", phase_e),
                              ("F", phase_f), ("G", phase_g), ("H", phase_h)):
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
