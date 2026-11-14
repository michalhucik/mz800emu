#!/usr/bin/env python3
"""E2E test kazety přes MCP (pipe): MCP dělá totéž co okno Virtual CMT.

Příkazy JSONL ``cmt_hack_set``, ``cmt_load_directly``, ``cmt_tape_step``,
``cmt_export_block`` (alias ``block_id``), ``settings_set``
(``CMT/rec_autostop_s``) a nová pole ``cmt_tape_list`` (resource
``emulator://periph/cmt/tape``).

  A  CMT hack: mění se jen ve STOP (bez pásky i s páskou), mimo STOP
     (PLAY, pauza, nahrávání, hrající páska se zapnutým hackem) chyba
     ``tape is not stopped`` a nic se nezmění; požadavek beze změny projde
     v každém stavu; CPU Boost zůstane.
  B  Load directly: úspěch (blok vybraný, Direct, CPU Boost vypnutý),
     opakování s Direct (mode_changed false), řídicí blok, neexistující
     blok, surový blok, mimo STOP, bez pásky, přísné typy.
  C  Krok po pásce: páska s řídicími bloky na začátku, uprostřed a na
     konci; next / prev / start, kraje, transport při PLAY a v pauze,
     nahrávání, prázdná páska, páska jen s řídicím blokem, bez pásky,
     neplatný směr.
  D  Nová pole resource: status, playable po blocích, údaje souboru,
     length_s (známá, neznámá, řídicí 0), position_s a block_position_s
     (STOP, neznámá délka před blokem, PLAY, nahrávání, prázdná páska,
     bez pásky), páska nad 256 bloků (pozice i za stropem výpisu).
  E  Záznam a přehrávání vstupů: hack a Load directly odmítnuté v obou,
     krok po pásce se zapíše jako událost ``cmt`` (next, seek).
  F  ``cmt_export_block`` s ``block_id``, obě jména naráz = chyba;
     ``CMT/rec_autostop_s`` přes ``settings_set`` (rozsah 0..600).
  G  Python wrapper (bez balíku mcp, jen text): nové nástroje posílají
     správné příkazy, mez rec_autostop_s v dispatch.c = CMT_REC_AUTOSTOP_MAX_S.
  H  Se zapnutým CMT hackem play, play_paused, pause ze STOP a open
     s play_immediately vrátí varování, že motor programu pásku nerozjede;
     record varování nemá (motor programu nahrávání i s hackem řídí).
  I  Analýza souboru NORMAL useknutého uvnitř těla: řádek unknown,
     s repair_body soubor, pravidlo apply (opravné volby i ve volání s apply).
  J  ROM bez podpory CMT hacku (vlastní proces): hack ani Load directly nic
     nezmění a ohlásí chybu.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
s vypnutým CMT hackem, ``--no-save-ini``), bez TCP portu; zabíjí se jen
vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (bez binárky mz800emu).
"""

import ast
import json
import re
import struct
import sys
import tempfile
import wave
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_record import PipeEmu, TestFailure, check, _find_exe  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
_DATA = _REPO_ROOT / "tests" / "data" / "cmt"
_PROG1 = _DATA / "prog1.mzf"
_TWO_PROGRAMS = _DATA / "two_programs.tmz"
#: Jména bloků z testovacích dat (pole name v cmt_tape_list).
_ONE = "TMZ PROG ONE"
_TWO = "TMZ PROG TWO"
_NOT_STOPPED = "tape is not stopped"


# ---------------------------------------------------------------------------
# Testovací pásky TMZ
# ---------------------------------------------------------------------------

def _tmz_parts():
    """Hlavička TMZ a dva bloky 0x46 (bajty) z two_programs.tmz."""
    src = _TWO_PROGRAMS.read_bytes()
    head, pos, blocks = src[:10], 10, []
    while pos < len(src):
        check(src[pos] == 0x46, f"two_programs.tmz: block at {pos} is 0x46")
        size, = struct.unpack_from("<I", src, pos + 1)
        blocks.append(src[pos:pos + 5 + size])
        pos += 5 + size
    check(len(blocks) == 2, f"two_programs.tmz has two file blocks ({len(blocks)})")
    return head, blocks


def _bad_mz_file(block):
    """Kopie bloku 0x46 se změněnou velikostí v hlavičce MZF: soubor z něj nejde sestavit.

    Knihovna TMZ (tmz_block_to_mzf()) odmítne blok 0x46, jehož fsize v hlavičce
    nesouhlasí s délkou těla; index pásky ho pak vede jako jiný blok TZX, ne MZF.
    """
    b = bytearray(block)
    pos = b.find(_ONE.encode("ascii"))
    check(pos > 0, "MZF name found in the 0x46 block")
    hdr = pos - 1
    size, = struct.unpack_from("<H", b, hdr + 0x12)
    struct.pack_into("<H", b, hdr + 0x12, size + 1)
    return bytes(b)


#: Blok 0x18 CSW Recording s neznámou kompresí 03h: player ho přehrát neumí.
_BAD_CSW = bytes([0x18]) + struct.pack("<I", 10) + bytes([0x00, 0x00, 0x44, 0xAC, 0x00, 0x03]) + struct.pack("<I", 0)


def _tap_block(flag, data):
    """Blok souboru ZX TAP: délka LE16, příznak, data, XOR kontrolní součet."""
    body = bytes([flag]) + data
    x = 0
    for c in body:
        x ^= c
    return struct.pack("<H", len(body) + 1) + body + bytes([x])


def _text(s):
    """TZX blok 0x30 Text description (řídicí blok bez signálu)."""
    b = s.encode("ascii")
    return bytes([0x30, len(b)]) + b


#: TZX blok 0x20 Pause s délkou 0 = zastav pásku (řídicí blok bez signálu).
_STOP = bytes([0x20, 0x00, 0x00])


def _write_tmz(path, parts):
    """Zapíše TMZ z hlavičky two_programs.tmz a daných bloků."""
    head, _ = _tmz_parts()
    path.write_bytes(head + b"".join(parts))
    return path


def _mzf_header(path):
    """Typ, velikost, adresa načtení a spuštění z hlavičky MZF."""
    d = path.read_bytes()
    size, load, exe = struct.unpack_from("<HHH", d, 0x12)
    return d[0], size, load, exe


def _tape(emu):
    """Výpis pásky (cmt_tape_list)."""
    return emu.call("cmt_tape_list")


def _cmt(emu):
    """Stav kazety (get_periph_cmt)."""
    return emu.call("get_periph_cmt")


def _err(resp):
    """Text chyby odpovědi."""
    return resp.get("error") or ""


def _open(emu, path):
    """Otevře pásku a zastaví deck."""
    emu.call("cmt_open", {"path": str(path), "play_immediately": False})
    st = _cmt(emu)
    check(st.get("state") == "stop", f"{path.name} opened, deck stopped ({st.get('state')})")


def _hack(emu, enabled):
    """Nastaví CMT hack (deck musí stát) a ověří stav."""
    res = emu.call("cmt_hack_set", {"enabled": enabled})
    check(res.get("installed") is enabled and _cmt(emu).get("cmthack_enabled") is enabled,
          f"hack set to {enabled} ({res})")


# ---------------------------------------------------------------------------
# Fáze
# ---------------------------------------------------------------------------

def phase_a(emu, tmp):
    """CMT hack jako přepínač Loading okna."""
    print("Phase A: emu_cmt_hack_set changes the hack only with the tape stopped")
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_set_property", {"property": "cpu_boost", "value": 1})
    _hack(emu, True)
    check(_cmt(emu).get("cpu_boost") is True, "hack on without a tape keeps the CPU boost")
    _hack(emu, False)
    emu.call("cmt_set_property", {"property": "cpu_boost", "value": 0})

    tape = _write_tmz(tmp / "a_two.tmz", [_tmz_parts()[1][0], _tmz_parts()[1][1]])
    _open(emu, tape)
    _hack(emu, True)
    _hack(emu, False)

    def refused(state_label, enable):
        before = _cmt(emu).get("cmthack_enabled")
        resp = emu.call_raw("cmt_hack_set", {"enabled": enable})
        after = _cmt(emu).get("cmthack_enabled")
        check(not resp.get("success") and _NOT_STOPPED in _err(resp) and after == before,
              f"hack {enable} refused in {state_label}, hack stays {before} ({_err(resp)})")
        same = emu.call("cmt_hack_set", {"enabled": before})
        check(same.get("installed") is before, f"a request without a change succeeds in {state_label} ({same})")

    emu.call("cmt_transport", {"action": "play"})
    refused("PLAY", True)
    emu.call("cmt_transport", {"action": "pause", "pause": 1})
    check(_cmt(emu).get("paused") is True, "tape paused")
    refused("PAUSE", True)
    emu.call("cmt_transport", {"action": "stop"})
    emu.call("cmt_record")
    check(_cmt(emu).get("state") == "record", "recording armed")
    refused("RECORD", True)
    emu.call("cmt_transport", {"action": "stop"})
    # PLAY čekající na motor (bez pauzy uživatele): deck nestojí
    emu.call("cmt_transport", {"action": "play_paused"})
    st = _cmt(emu)
    check(st.get("state") == "play" and st.get("paused") is True and st.get("user_paused") is False,
          f"play requested, waiting for the motor ({st.get('state')}, {st.get('paused')}, {st.get('user_paused')})")
    refused("PLAY waiting for the motor", True)
    emu.call("cmt_transport", {"action": "stop"})

    # hrající páska se zapnutým hackem (Fast Open & Play, MCP play): hack
    # nejde vypnout, dokud páska neskončí nebo ji někdo nezastaví
    _open(emu, tape)
    _hack(emu, True)
    emu.call("cmt_transport", {"action": "play"})
    check(_cmt(emu).get("state") == "play", "tape plays with the hack on")
    refused("PLAY with the hack on", False)
    emu.call("cmt_transport", {"action": "stop"})
    _hack(emu, False)


def phase_b(emu, tmp):
    """Load directly jako kontextové menu okna."""
    print("Phase B: emu_cmt_load_directly")
    head, blocks = _tmz_parts()
    tape = _write_tmz(tmp / "b_ctrl.tmz", [_text("LEADING INFO"), blocks[0], _STOP, blocks[1], _text("TRAILING")])
    _open(emu, tape)
    check(_tape(emu).get("current_block") == 1, "the first playable block 1 is open")
    emu.call("cmt_set_property", {"property": "cpu_boost", "value": 1})
    res = emu.call("cmt_load_directly", {"block_id": 3})
    check(res.get("block_id") == 3 and res.get("mode_changed") is True and res.get("installed") is True
          and res.get("current_block") == 3, f"load directly block 3 ({res})")
    st = _cmt(emu)
    check(st.get("cmthack_enabled") is True and st.get("cpu_boost") is False and st.get("state") == "stop",
          f"Direct: hack on, CPU boost off, deck stopped ({st.get('cmthack_enabled')}, {st.get('cpu_boost')})")
    emu.call("cmt_set_property", {"property": "cpu_boost", "value": 1})
    res = emu.call("cmt_load_directly", {"block_id": 1})
    check(res.get("mode_changed") is False and res.get("current_block") == 1,
          f"load directly in Direct only picks the block ({res})")
    check(_cmt(emu).get("cpu_boost") is True, "with the hack already on the CPU boost stays")
    emu.call("cmt_set_property", {"property": "cpu_boost", "value": 0})
    _hack(emu, False)

    for bid, word in ((0, "Block 0 does not contain an MZF file"), (2, "Block 2 does not contain an MZF file"),
                      (5, "Block 5 does not exist"), (99, "Block 99 does not exist")):
        resp = emu.call_raw("cmt_load_directly", {"block_id": bid})
        check(not resp.get("success") and word in _err(resp), f"load directly block {bid} refused ({_err(resp)})")
    t = _tape(emu)
    check(t.get("current_block") == 1 and _cmt(emu).get("cmthack_enabled") is False,
          "refused load directly changes nothing")

    for data in ({"block_id": "1"}, {"block_id": 1.0}, {"block_id": True}, {"block_id": -1}, {}, {"block_id": None}):
        resp = emu.call_raw("cmt_load_directly", data)
        check(not resp.get("success") and ("integer" in _err(resp) or "Missing" in _err(resp)),
              f"load directly {data} is an invalid parameter ({_err(resp)})")

    for state in ("play", "pause", "record"):
        if state == "record":
            emu.call("cmt_record")
        else:
            emu.call("cmt_transport", {"action": "play"})
            if state == "pause":
                emu.call("cmt_transport", {"action": "pause", "pause": 1})
        resp = emu.call_raw("cmt_load_directly", {"block_id": 3})
        check(not resp.get("success") and _NOT_STOPPED in _err(resp) and _cmt(emu).get("cmthack_enabled") is False,
              f"load directly refused in {state} ({_err(resp)})")
        emu.call("cmt_transport", {"action": "stop"})

    # blok 0x46, ze kterého nejde sestavit MZF: okno Load directly nenabídne,
    # MCP ho odmítne a nic nezmění
    bad = _write_tmz(tmp / "b_badmzf.tmz", [blocks[0], _bad_mz_file(blocks[0])])
    # CPU Boost zapnutý: odmítnutý Load directly ho nesmí vypnout (nezávisle na výchozím stavu)
    emu.call("cmt_set_property", {"property": "cpu_boost", "value": 1})
    _open(emu, bad)
    t = _tape(emu)
    check(t["blocks"][1].get("type") != 1 and t["blocks"][1].get("size") is None,
          f"the 0x46 block without an MZF is not a file block ({t['blocks'][1]})")
    resp = emu.call_raw("cmt_load_directly", {"block_id": 1})
    check(not resp.get("success") and "Block 1 does not contain an MZF file" in _err(resp),
          f"load directly of a 0x46 block without an MZF refused ({_err(resp)})")
    check(_tape(emu).get("current_block") == 0 and _cmt(emu).get("cmthack_enabled") is False
          and _cmt(emu).get("cpu_boost") is True, "the refused load directly changed nothing")
    emu.call("cmt_set_property", {"property": "cpu_boost", "value": 0})

    # surový blok (WAV z exportu bloku): nenese MZF
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(_PROG1)})
    wav = tmp / "b_one.wav"
    emu.call("cmt_export_block", {"block": 0, "path": str(wav)})
    emu.call("cmt_tape_import", {"path": str(wav)})
    resp = emu.call_raw("cmt_load_directly", {"block_id": 1})
    check(not resp.get("success") and "Block 1 does not contain an MZF file" in _err(resp),
          f"load directly of a raw block refused ({_err(resp)})")

    emu.call("cmt_transport", {"action": "eject"})
    resp = emu.call_raw("cmt_load_directly", {"block_id": 0})
    check(not resp.get("success") and "No tape inserted" in _err(resp), f"load directly without a tape ({_err(resp)})")
    emu.call("cmt_new_tape")
    resp = emu.call_raw("cmt_load_directly", {"block_id": 0})
    check(not resp.get("success") and "Block 0 does not exist" in _err(resp),
          f"load directly on an empty tape ({_err(resp)})")
    check(_cmt(emu).get("cmthack_enabled") is False, "hack still off")


def _step(emu, direction):
    """Krok po pásce; vrátí číslo bloku z odpovědi."""
    res = emu.call("cmt_tape_step", {"direction": direction})
    check(res.get("direction") == direction and res.get("block_id") == _tape(emu).get("current_block"),
          f"step {direction} -> block {res.get('block_id')}")
    return res.get("block_id")


def _step_err(emu, direction, word):
    """Krok, který musí selhat s textem ``word`` a nic nezmění."""
    before = _tape(emu).get("current_block")
    resp = emu.call_raw("cmt_tape_step", {"direction": direction})
    check(not resp.get("success") and word in _err(resp) and _tape(emu).get("current_block") == before,
          f"step {direction} refused: {_err(resp)}")


def phase_c(emu, tmp):
    """Krok po pásce jako tlačítka Rewind / Previous / Next."""
    print("Phase C: emu_cmt_tape_step skips control blocks like the deck buttons")
    head, blocks = _tmz_parts()
    tape = _write_tmz(tmp / "c_ctrl.tmz", [_text("LEADING INFO"), blocks[0], _STOP, blocks[1], _text("TRAILING")])
    _open(emu, tape)
    check(_tape(emu).get("current_block") == 1, "block 1 open after the leading control block")
    check(_step(emu, "next") == 3, "next skips the control block 2")
    _step_err(emu, "next", "There is no playable block after this one")
    check(_step(emu, "prev") == 1, "prev skips the control block 2")
    _step_err(emu, "prev", "There is no playable block before this one")
    _step(emu, "next")
    check(_step(emu, "start") == 1, "start goes to the first playable block 1")

    # transport: PLAY hraje dál, pauza zůstane
    emu.call("cmt_transport", {"action": "play"})
    emu.call("run", {"frames": 10})
    _step(emu, "next")
    st = _cmt(emu)
    check(st.get("state") == "play" and st.get("paused") is False, f"next keeps PLAY ({st.get('state')}, {st.get('paused')})")
    emu.call("cmt_transport", {"action": "pause", "pause": 1})
    _step(emu, "prev")
    st = _cmt(emu)
    check(st.get("state") == "play" and st.get("paused") is True and st.get("user_paused") is True,
          f"prev keeps the user's pause ({st.get('paused')}, {st.get('user_paused')})")
    _step(emu, "start")
    st = _cmt(emu)
    check(st.get("state") == "play" and st.get("paused") is True, "start keeps the transport")
    emu.call("cmt_transport", {"action": "stop"})

    emu.call("cmt_record")
    _step_err(emu, "next", "Not available while recording")
    emu.call("cmt_transport", {"action": "stop"})

    for data in ({"direction": "forward"}, {"direction": 1}, {"direction": True}, {}, {"direction": None},
                 {"direction": "NEXT"}):
        resp = emu.call_raw("cmt_tape_step", data)
        check(not resp.get("success") and ("direction" in _err(resp)),
              f"step {data} is an invalid parameter ({_err(resp)})")

    only_ctrl = _write_tmz(tmp / "c_only.tmz", [_text("JUST INFO")])
    emu.call("cmt_open", {"path": str(only_ctrl), "play_immediately": False})
    check(_tape(emu).get("current_block") == -1, "a tape with a control block only has no open block")
    for d in ("start", "next", "prev"):
        _step_err(emu, d, "The tape has no playable block")
    emu.call("cmt_new_tape")
    _step_err(emu, "next", "The tape is empty")
    _step_err(emu, "start", "The tape is empty")
    emu.call("cmt_transport", {"action": "eject"})
    _step_err(emu, "prev", "No tape inserted")


def phase_d(emu, tmp):
    """Nová pole resource periph/cmt/tape."""
    print("Phase D: new fields of the tape resource")
    head, blocks = _tmz_parts()
    ftype, fsize, fload, fexec = _mzf_header(_PROG1)
    tape = _write_tmz(tmp / "d_pos.tmz", [_text("LEADING INFO"), blocks[0], blocks[1], blocks[0], _STOP])
    _open(emu, tape)
    t = _tape(emu)
    b = t["blocks"]
    check([x.get("status") for x in b] == ["control", "ok", "ok", "ok", "control"],
          f"status per block ({[x.get('status') for x in b]})")
    check([x.get("playable") for x in b] == [False, True, True, True, False],
          f"playable per block ({[x.get('playable') for x in b]})")
    check(b[1].get("name") == _ONE and b[1].get("size") == fsize and b[1].get("load_addr") == fload
          and b[1].get("exec_addr") == fexec and b[1].get("file_type") == ftype,
          f"file fields of block 1 from the MZF header ({b[1]})")
    check(all(b[i].get(k) is None for i in (0, 4) for k in ("size", "load_addr", "exec_addr", "file_type")),
          "control blocks have no file fields")
    check(b[0].get("length_s") == 0.0 and b[4].get("length_s") == 0.0, "control blocks have length 0")
    check(isinstance(b[1].get("length_s"), float) and b[1]["length_s"] > 0, f"the open block has a length ({b[1].get('length_s')})")
    check(b[2].get("length_s") is None and b[3].get("length_s") is None,
          f"blocks never opened have no length yet ({b[2].get('length_s')}, {b[3].get('length_s')})")
    check(t.get("block_position_s") == 0.0 and t.get("position_s") == 0.0,
          f"STOP at the start of block 1: positions 0 ({t.get('position_s')}, {t.get('block_position_s')})")

    emu.call("cmt_tape_seek", {"block_id": 3})
    t = _tape(emu)
    check(t.get("block_position_s") == 0.0 and t.get("position_s") is None,
          f"position unknown while block 2 has no length ({t.get('position_s')}, {t.get('block_position_s')})")
    emu.call("cmt_tape_seek", {"block_id": 2})
    emu.call("cmt_tape_seek", {"block_id": 3})
    t = _tape(emu)
    b = t["blocks"]
    want = b[1]["length_s"] + b[2]["length_s"]
    check(t.get("position_s") is not None and abs(t["position_s"] - want) < 1e-6,
          f"position = lengths of blocks 1 and 2 ({t.get('position_s')} vs {want})")

    emu.call("cmt_transport", {"action": "play"})
    emu.call("run", {"frames": 25})
    t = _tape(emu)
    bp = t.get("block_position_s")
    check(bp is not None and bp > 0.3 and abs(t.get("position_s") - (want + bp)) < 1e-6,
          f"PLAY: position advances ({t.get('position_s')}, {bp})")
    emu.call("cmt_transport", {"action": "stop"})
    emu.call("cmt_record")
    t = _tape(emu)
    check(t.get("position_s") is None and t.get("block_position_s") is None, "recording: positions null")
    emu.call("cmt_transport", {"action": "stop"})

    # surový blok: not_decoded, přehratelný, bez údajů souboru
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(_PROG1)})
    wav = tmp / "d_one.wav"
    emu.call("cmt_export_block", {"block": 0, "path": str(wav)})
    emu.call("cmt_tape_import", {"path": str(wav)})
    raw = _tape(emu)["blocks"][1]
    check(raw.get("type") == 0 and raw.get("status") == "not_decoded" and raw.get("playable") is True
          and raw.get("size") is None and raw.get("file_type") is None,
          f"raw block fields ({raw})")

    # blok, který player neumí (0x18 s neznámou kompresí): cannot_play
    bad = _write_tmz(tmp / "d_cannot.tmz", [blocks[0], _BAD_CSW, blocks[1]])
    _open(emu, bad)
    b = _tape(emu)["blocks"]
    check([x.get("status") for x in b] == ["ok", "cannot_play", "ok"]
          and [x.get("playable") for x in b] == [True, False, True] and b[1].get("size") is None,
          f"block the player cannot play ({[(x.get('status'), x.get('playable')) for x in b]})")
    check(_step(emu, "next") == 2, "next skips the block the player cannot play")

    # bloky ZX TAP: hlavička a data, přehratelné, bez údajů MZF
    tap = tmp / "d_zx.tap"
    hdr = bytes([3]) + b"ZXCODE    " + struct.pack("<HHH", 4, 0x8000, 0x8000)
    tap.write_bytes(_tap_block(0x00, hdr) + _tap_block(0xFF, bytes([1, 2, 3, 4])))
    _open(emu, tap)
    b = _tape(emu)["blocks"]
    check([x.get("type") for x in b] == [2, 3] and all(x.get("status") == "ok" and x.get("playable") is True
                                                      and x.get("size") is None and x.get("file_type") is None
                                                      for x in b),
          f"ZX TAP blocks ({[(x.get('type'), x.get('status'), x.get('playable'), x.get('size')) for x in b]})")
    check(b[0].get("name", "").startswith("ZXCODE"), f"ZX header name ({b[0].get('name')})")

    # prázdná páska a bez pásky
    emu.call("cmt_new_tape")
    t = _tape(emu)
    check(t.get("available") is True and t.get("blocks") == [] and t.get("position_s") == 0.0
          and t.get("block_position_s") is None, f"empty tape: position 0, no block position ({t})")
    emu.call("cmt_transport", {"action": "eject"})
    t = _tape(emu)
    check(t.get("available") is False and t.get("position_s") is None and t.get("block_position_s") is None
          and t.get("current_block") == -1 and t.get("count") == 0 and isinstance(t.get("profiles"), list),
          f"no tape: positions null ({t})")

    # páska nad 256 bloků: výpis do stropu, pozice ze všech bloků
    big = _write_tmz(tmp / "d_big.tmz", [_text("I%03d" % i) for i in range(300)] + [blocks[0], blocks[1]])
    _open(emu, big)
    t = _tape(emu)
    check(t.get("truncated") is True and len(t.get("blocks")) == 256 and t.get("count") == 256
          and t.get("current_block") == 300, f"big tape: 256 listed, block 300 open ({t.get('count')}, {t.get('current_block')})")
    check(t.get("position_s") == 0.0 and t.get("block_position_s") == 0.0,
          f"big tape: position over the listing cap ({t.get('position_s')})")
    check(all(x.get("status") == "control" and x.get("playable") is False for x in t["blocks"]),
          "big tape: listed blocks are control blocks")
    check(_step(emu, "next") == 301, "big tape: next to block 301")
    t = _tape(emu)
    check(t.get("position_s") is not None and t["position_s"] > 0, f"big tape: position after block 300 ({t.get('position_s')})")
    check(_step(emu, "prev") == 300, "big tape: prev back to block 300")
    _step_err(emu, "prev", "There is no playable block before this one")


def phase_e(emu, tmp):
    """Záznam a přehrávání vstupů."""
    print("Phase E: input replay recording and playback")
    head, blocks = _tmz_parts()
    tape = _write_tmz(tmp / "e_ctrl.tmz", [_text("LEADING INFO"), blocks[0], _STOP, blocks[1]])
    _open(emu, tape)
    mzr = tmp / "e.mzr"
    emu.call("replay_record_start", {"path": str(mzr), "hash_interval": 10})
    emu.call("run", {"frames": 5})
    for cmd, data in (("cmt_hack_set", {"enabled": True}), ("cmt_load_directly", {"block_id": 1})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "not allowed while recording a replay" in _err(resp),
              f"{cmd} refused while an input replay is recorded ({_err(resp)})")
    check(_step(emu, "next") == 3, "step next while recording")
    emu.call("run", {"frames": 5})
    check(_step(emu, "start") == 1, "step start while recording")
    emu.call("run", {"frames": 5})
    st = emu.call("replay_record_stop")
    check(st.get("outcome") == "recorded", f"input replay recorded ({st.get('outcome_message')})")
    with zipfile.ZipFile(mzr) as z:
        events = [json.loads(line) for line in z.read("inputs.jsonl").decode().splitlines() if line.strip()]
    ops = [(e.get("op"), e.get("v")) for e in events if e.get("k") == "cmt"]
    check([o for o, _ in ops] == ["next", "seek"] and ops[1][1] == 1,
          f"steps recorded as cmt events next and seek 1 ({ops})")
    check(not any(e.get("k") == "dbg" for e in events), "no dbg event for the steps")

    st = emu.call("replay_play", {"path": str(mzr), "pause_after_load": True})
    check(st.get("mode") == "playing", f"input replay playing ({st})")
    for cmd, data in (("cmt_hack_set", {"enabled": True}), ("cmt_load_directly", {"block_id": 1})):
        resp = emu.call_raw(cmd, data)
        check(not resp.get("success") and "input replay" in _err(resp),
              f"{cmd} refused while an input replay is played ({_err(resp)})")
    check(_cmt(emu).get("cmthack_enabled") is False, "hack unchanged during playback")
    emu.call("replay_play_stop")


def phase_f(emu, tmp):
    """Alias block_id u exportu bloku a rec_autostop_s přes settings_set."""
    print("Phase F: export block with block_id, rec_autostop_s through settings_set")
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(_PROG1)})
    out = tmp / "f_block.mzf"
    res = emu.call("cmt_export_block", {"block_id": 0, "path": str(out)})
    check(out.is_file() and out.read_bytes() == _PROG1.read_bytes() and res.get("block_id") == 0
          and res.get("block") == 0, f"export with block_id ({res})")
    for data, word in (({"block": 0, "block_id": 0, "path": str(tmp / "f2.mzf")}, "not both"),
                       ({"block_id": "0", "path": str(tmp / "f3.mzf")}, "block_id must be an integer"),
                       ({"block_id": 0.5, "path": str(tmp / "f4.mzf")}, "block_id must be an integer"),
                       ({"path": str(tmp / "f5.mzf")}, "Missing")):
        resp = emu.call_raw("cmt_export_block", data)
        check(not resp.get("success") and word in _err(resp), f"export {data} refused ({_err(resp)})")
    check(not any((tmp / n).exists() for n in ("f2.mzf", "f3.mzf", "f4.mzf", "f5.mzf")), "no refused export wrote a file")

    res = emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "5"})
    check(res.get("new_value") == "5" and res.get("type") == "unsigned", f"rec_autostop_s set ({res})")
    got = emu.call("settings_get", {"key": "CMT/rec_autostop_s"})
    check(got.get("value") == "5" and got.get("live") == "5", f"rec_autostop_s applies to the tape ({got})")
    # stejná soustava v kontrole rozsahu i v zápisu (základ 0 jako ostatní klíče)
    emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "010"})
    got = emu.call("settings_get", {"key": "CMT/rec_autostop_s"})
    check(got.get("live") == "8", f"010 is octal 8 in the check and in the store ({got})")
    res = emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "0x258"})
    check(emu.call("settings_get", {"key": "CMT/rec_autostop_s"}).get("live") == "600", f"0x258 = 600 ({res})")
    resp = emu.call_raw("settings_set", {"key": "CMT/rec_autostop_s", "value": "01131"})
    check(not resp.get("success"), f"01131 (octal 601) refused ({_err(resp)})")
    emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "5"})
    res = emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "600"})
    check(res.get("previous_value") == "5", f"600 is allowed ({res})")
    for bad in ("601", "-1", "abc", "", "1.5"):
        resp = emu.call_raw("settings_set", {"key": "CMT/rec_autostop_s", "value": bad})
        check(not resp.get("success"), f"rec_autostop_s {bad!r} refused ({_err(resp)})")
    got = emu.call("settings_get", {"key": "CMT/rec_autostop_s"})
    check(got.get("live") == "600", f"refused values do not change it ({got})")
    emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "0"})

    # při přehrávání záznamu vstupů: zápis projde, po konci přehrávání se vrátí
    # hodnota z doby před přehráváním (přehrávání nese vlastní hodnotu v manifestu)
    emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "3"})
    mzr = tmp / "f_autostop.mzr"
    emu.call("replay_record_start", {"path": str(mzr), "hash_interval": 10})
    emu.call("run", {"frames": 5})
    st = emu.call("replay_record_stop")
    check(st.get("outcome") == "recorded", f"input replay recorded ({st.get('outcome_message')})")
    st = emu.call("replay_play", {"path": str(mzr), "pause_after_load": True})
    check(st.get("mode") == "playing", f"input replay playing ({st})")
    res = emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "9"})
    got = emu.call("settings_get", {"key": "CMT/rec_autostop_s"})
    check(res.get("new_value") == "9" and got.get("live") == "9",
          f"rec_autostop_s can be set during playback ({res}, {got})")
    emu.call("replay_play_stop")
    got = emu.call("settings_get", {"key": "CMT/rec_autostop_s"})
    check(got.get("live") == "3", f"after the playback the value from before it is back ({got})")
    emu.call("settings_set", {"key": "CMT/rec_autostop_s", "value": "0"})


def phase_g():
    """Python wrapper a konstanty (bez spuštění, jen text zdrojů)."""
    print("Phase G: Python wrapper of the new tools, rec_autostop_s limit")
    src = (_REPO_ROOT / "mcp-server" / "mcp_server.py").read_text(encoding="utf-8")
    tree = ast.parse(src)
    funcs = {n.name: n for n in tree.body if isinstance(n, ast.AsyncFunctionDef)}

    def params(name):
        f = funcs.get(name)
        return [a.arg for a in f.args.args] if f else None

    def sends(name, cmd):
        f = funcs.get(name)
        return f is not None and f'_send_request("{cmd}"' in ast.get_source_segment(src, f)

    check(params("emu_cmt_load_directly") == ["block_id"] and sends("emu_cmt_load_directly", "cmt_load_directly"),
          "emu_cmt_load_directly(block_id) sends cmt_load_directly")
    check(params("emu_cmt_tape_step") == ["direction"] and sends("emu_cmt_tape_step", "cmt_tape_step"),
          "emu_cmt_tape_step(direction) sends cmt_tape_step")
    check(params("emu_cmt_export_block") == ["block", "path", "block_id"],
          f"emu_cmt_export_block takes block, path and block_id ({params('emu_cmt_export_block')})")
    check('"CMT/rec_autostop_s"' in src, "the wrapper allows CMT/rec_autostop_s")
    disp = (_REPO_ROOT / "src" / "emulator" / "mcp" / "dispatch.c").read_text(encoding="utf-8")
    hdr = (_REPO_ROOT / "src" / "emulator" / "hw-generic" / "cmt" / "cmt.h").read_text(encoding="utf-8")
    m1 = re.search(r"#define MCP_CMT_REC_AUTOSTOP_MAX_S (\d+)", disp)
    m2 = re.search(r"#define CMT_REC_AUTOSTOP_MAX_S (\d+)", hdr)
    check(m1 and m2 and m1.group(1) == m2.group(1),
          f"rec_autostop_s limit of dispatch.c equals cmt.h ({m1 and m1.group(1)}, {m2 and m2.group(1)})")


#: Začátek varování MCP se zapnutým CMT hackem (dispatch.c, DBGAPI_CMT_WARNING_DIRECT).
_DIRECT_WARNING = "Direct loading (CMT hack) is on"


def _direct_warned(res):
    """Nese odpověď varování o režimu Direct?"""
    return _DIRECT_WARNING in (res.get("warning") or "")


def phase_h(emu, tmp):
    """Varování play, play_paused a open s play_immediately se zapnutým hackem; record bez varování.

    Nahrávání varování Direct nemá: při RECORD řídí deck povel motoru
    programu i se zapnutým CMT hackem (pio8255.c, CMTHACK_TEST_FAKES_MOTOR).
    """
    print("Phase H: play with the CMT hack on warns that the motor does not move the tape, record does not")
    head, blocks = _tmz_parts()
    tape = _write_tmz(tmp / "h_two.tmz", [blocks[0], blocks[1]])
    for hack in (True, False):
        _open(emu, tape)
        _hack(emu, hack)
        res = emu.call("cmt_transport", {"action": "play"})
        check(_direct_warned(res) is hack, f"play with the hack {hack}: warning {res.get('warning')!r}")
        emu.call("cmt_transport", {"action": "stop"})
        res = emu.call("cmt_transport", {"action": "play_paused"})
        check(_direct_warned(res) is hack, f"play_paused with the hack {hack}: warning {res.get('warning')!r}")
        emu.call("cmt_transport", {"action": "stop"})
        res = emu.call("cmt_record")
        check(not _direct_warned(res) and _cmt(emu).get("state") == "record",
              f"record with the hack {hack} does not warn ({res.get('warning')!r})")
        emu.call("cmt_transport", {"action": "stop"})
        res = emu.call("cmt_open", {"path": str(tape), "play_immediately": True})
        check(_direct_warned(res) is hack and res.get("state") == "play",
              f"open with play_immediately and the hack {hack}: warning {res.get('warning')!r}")
        res = emu.call("cmt_transport", {"action": "stop"})
        check(not _direct_warned(res), "stop never warns")
        res = emu.call("cmt_open", {"path": str(tape), "play_immediately": False})
        check(not _direct_warned(res), f"open without play_immediately does not warn (hack {hack})")
    # pause(true) ze STOP spustí PLAY čekající na motor: totéž varování
    _open(emu, tape)
    _hack(emu, True)
    res = emu.call("cmt_transport", {"action": "pause", "pause": 1})
    check(_direct_warned(res) and _cmt(emu).get("state") == "play",
          f"pause(true) from STOP with the hack on warns ({res.get('warning')!r})")
    # volání, která nic neudělala, nevarují: play, play_paused a pause(true) nad hrající páskou
    for action, extra in (("play", {}), ("play_paused", {}), ("pause", {"pause": 1})):
        res = emu.call("cmt_transport", dict({"action": action}, **extra))
        check(not _direct_warned(res), f"{action} over a playing tape does not warn ({res.get('warning')!r})")
    emu.call("cmt_transport", {"action": "stop"})
    # bez pásky: play a play_paused nic neudělají
    emu.call("cmt_transport", {"action": "eject"})
    for action in ("play", "play_paused"):
        res = emu.call("cmt_transport", {"action": action})
        check(not _direct_warned(res) and _cmt(emu).get("state") == "stop",
              f"{action} without a tape does not warn ({res.get('warning')!r})")
    emu.call("cmt_transport", {"action": "stop"})
    _hack(emu, False)

    # pause(paused=False) rozjede čekající pásku i se zapnutým hackem (rada ve varování)
    _open(emu, tape)
    _hack(emu, True)
    emu.call("cmt_transport", {"action": "play_paused"})
    emu.call("cmt_transport", {"action": "pause", "pause": 0})
    st = _cmt(emu)
    check(st.get("state") == "play" and st.get("paused") is False, f"pause(false) starts the tape with the hack on ({st})")
    emu.call("run", {"frames": 10})
    check(_cmt(emu).get("paused") is False, "the tape keeps running with the hack on")
    emu.call("cmt_transport", {"action": "stop"})
    _hack(emu, False)


def _cut_wav(src, dst, part):
    """Kopie WAV zkrácená na ``part`` vzorků (signál skončí uvnitř souboru)."""
    with wave.open(str(src), "rb") as w:
        params = w.getparams()
        frames = w.readframes(w.getnframes())
    keep = int(len(frames) * part)
    keep -= keep % (params.sampwidth * params.nchannels)
    with wave.open(str(dst), "wb") as w:
        w.setparams(params)
        w.writeframes(frames[:keep])


def phase_i(emu, tmp):
    """Analýza souboru NORMAL, jehož signál skončí uvnitř těla."""
    print("Phase I: analysis of a NORMAL file cut inside its body, repair_body, apply rule")
    body = bytes((i * 37 + 11) & 0xFF for i in range(3232))
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"CUT BODY TEST\r"
    hdr[1:1 + len(name)] = name
    struct.pack_into("<HHH", hdr, 0x12, len(body), 0x4000, 0x4000)
    mzf = tmp / "i_cut.mzf"
    mzf.write_bytes(bytes(hdr) + body)
    emu.call("cmt_transport", {"action": "eject"})
    emu.call("cmt_tape_import", {"path": str(mzf)})
    full = tmp / "i_full.wav"
    emu.call("cmt_export_block", {"block_id": 0, "path": str(full)})
    cut = tmp / "i_cut.wav"
    _cut_wav(full, cut, 0.85)
    emu.call("cmt_open", {"path": str(cut), "play_immediately": False})
    check(_tape(emu)["blocks"][0].get("status") == "not_decoded", "the cut WAV is a raw block")

    rows = emu.call("cmt_tape_analyze", {"block_id": 0}).get("rows") or []
    check(not any(r.get("kind") == "file" for r in rows)
          and any(r.get("kind") == "unknown" and r.get("name") == ""
                  and r.get("reason") == "No file found in this part of the signal" for r in rows),
          f"without repair_body the cut file is only an unknown part ({rows})")
    rows = emu.call("cmt_tape_analyze", {"block_id": 0, "repair_body": True}).get("rows") or []
    files = [r for r in rows if r.get("kind") == "file"]
    check(len(files) == 1 and files[0].get("name") == "CUT BODY TEST" and files[0].get("header") == "ok"
          and files[0].get("body") == "checksum_error" and files[0].get("result") == "addable_repaired",
          f"with repair_body the file row has its name and is addable_repaired ({files})")

    res = emu.call("cmt_tape_analyze", {"block_id": 0, "apply": "replace"})
    t = _tape(emu)
    check(res.get("applied") == 0 and t.get("count") == 1 and t["blocks"][0].get("type") == 0,
          f"apply without repair_body adds nothing, the raw block stays ({res.get('applied')}, {t.get('count')})")
    res = emu.call("cmt_tape_analyze", {"block_id": 0, "apply": "replace", "repair_body": True})
    t = _tape(emu)
    check(res.get("applied") == 1 and t["blocks"][0].get("type") == 1 and t["blocks"][0].get("size") == len(body),
          f"apply with repair_body adds the repaired file ({res.get('applied')}, {t['blocks']})")


#: Text chyby ROM bez podpory CMT hacku (dbgapi_cmdrq.h, DBGAPI_CMT_NO_HACK_ROM).
_NO_HACK_ROM = "The selected ROM does not support the CMT hack"


def phase_j(exe, tmp):
    """ROM bez podpory CMT hacku: hack ani Load directly nic nezmění a ohlásí chybu."""
    print("Phase J: a ROM without CMT hack support refuses the hack and Load directly")
    # WILLY_JAP: řádek ROM Willy (ROM_CONFIG_FLAG_CMTHACK_INCOMP)
    emu = PipeEmu(exe, tmp / "cfg_willy", extra_ini="[ROM]\nrom_type = WILLY_JAP\n")
    try:
        emu.call("run", {"frames": 50})
        resp = emu.call_raw("cmt_hack_set", {"enabled": True})
        check(not resp.get("success") and _NO_HACK_ROM in _err(resp) and _cmt(emu).get("cmthack_enabled") is False,
              f"hack_set(true) refused with this ROM ({_err(resp)})")
        res = emu.call("cmt_hack_set", {"enabled": False})
        check(res.get("installed") is False, f"hack_set(false) succeeds ({res})")
        head, blocks = _tmz_parts()
        tape = _write_tmz(tmp / "j_two.tmz", [blocks[0], blocks[1]])
        emu.call("cmt_open", {"path": str(tape), "play_immediately": False})
        emu.call("cmt_set_property", {"property": "cpu_boost", "value": 1})
        resp = emu.call_raw("cmt_load_directly", {"block_id": 1})
        st = _cmt(emu)
        check(not resp.get("success") and _NO_HACK_ROM in _err(resp), f"load directly refused ({_err(resp)})")
        check(_tape(emu).get("current_block") == 0 and st.get("cpu_boost") is True and st.get("cmthack_enabled") is False,
              f"the refused load directly changed nothing (block {_tape(emu).get('current_block')}, {st})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=170)
    failures = []
    try:
        phase_g()
    except TestFailure as exc:
        failures.append(f"phase G: {exc}")
    exe = _find_exe("mz800emu")
    if exe is None:
        # textová fáze G nesmí selhání schovat za SKIP
        if failures:
            print("FAILED (binary not found, only phase G ran):")
            for f in failures:
                print(f"  {f}")
            return 1
        print("SKIP: mz800emu binary not found")
        return 77
    with tempfile.TemporaryDirectory(prefix="mz_cmt_parity_") as td:
        tmp = Path(td)
        emu = PipeEmu(exe, tmp / "cfg")
        try:
            emu.call("run", {"frames": 350})
            for label, fn in (("A", phase_a), ("B", phase_b), ("C", phase_c), ("D", phase_d), ("E", phase_e),
                              ("F", phase_f), ("H", phase_h), ("I", phase_i)):
                try:
                    fn(emu, tmp)
                except TestFailure as exc:
                    failures.append(f"phase {label}: {exc}")
                    print(f"  FAIL: {exc}")
                    break
        finally:
            emu.close()
        # fáze J má vlastní instanci emulátoru: běží vždy, i po selhání dřívější fáze
        try:
            phase_j(exe, tmp)
        except TestFailure as exc:
            failures.append(f"phase J: {exc}")
            print(f"  FAIL: {exc}")
    if failures:
        print("FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
