#!/usr/bin/env python3
"""E2E test Direct Save přes MCP (pipe): SAVE a VERIFY z ROM se zapnutým CMT hackem.

S CMT hackem (Direct, výchozí nové konfigurace) a nastavením
``[CMTHACK] direct_save`` (výchozí 1) patch ROM přesměruje zápisové rutiny
monitoru na past emulátoru: WHEAD 0436h a WDATA 0475h (port 03h) uloží
soubor rovnou jako nový blok MZF na konec pásky Virtual CMT, VERFY 0588h
(port 04h) porovná paměť s tělem otevřeného bloku. Vstupní body ROM:
0021h = JP 0436h (@WHEAD), 0024h = JP 0475h (@WDATA), 002Dh = JP 0588h
(@VERFY), 0027h / 002Ah = @RHEAD / @RDATA - shodně ve výchozích ROM všech
tří strojů (ověřeno v bajtech ROM_MZ800_0000.c, ROM_MZ700_13A_0000.c,
ROM_MZ1500_0000.c v src/emulator/mzarch/*/memory/ROM/).

Pro jednu binárku (``--machine <binárka>``, ctest
mcp_cmt_direct_save_<binárka>):

  A  Po startu: ``cmthack_enabled``, ``direct_save`` i ``direct_save_enabled``
     true; bajty ROM (čtené přes MCP mem_read): CALL @CHECK na 0444h
     původní, 0447h-044Bh pět NOP, 045Eh OUT (03h),A, 059Eh OUT (04h),A;
     XOR obou rozsahů zápisu dohromady a XOR rozsahu ověření se rovná XOR
     původního obrazu ROM (zdrojový soubor C).
  B  SAVE z ROM bez Record (stub @WHEAD + @WDATA, hlavička na 10F0h, tělo
     na 1200h): ROM vrátí OK během pár snímků, deck zůstane ve STOP, páska
     (nová) má jeden blok MZF, export bloku je bajtově shodný s pamětí,
     na obrazovce je řádek ROM "WRITING".
  C  VERIFY z ROM (stub @VERFY): OK; po změně jednoho bajtu těla chyba
     (CY, A = 1); s tělem zpět OK; s deckem v PLAY (blok s MZF) OK
     a transport se zastaví.
  D  LOAD z ROM přes patch (stub @RHEAD + @RDATA) do vynulované paměti dá
     tytéž bajty.
  E  Druhý soubor (jiné jméno, tělo, adresa) se připojí jako další blok
     (blok 1), páska stojí na něm.
  F  Record + Direct + direct_save: ``emu_cmt_record(path=f.tmz)`` -
     v RECORD je patch pryč (``direct_save`` false, bajty ROM původní),
     SAVE z ROM se nahraje jako signál (nic se nepřipojí hned), STOP dá
     jeden MZF blok, soubor f.tmz se zapíše a patch je zpět.
  G  settings_set ``CMTHACK/direct_save`` v PLAY odmítnut (text jako
     menu); false ve STOP: ``direct_save`` false, hack zapnutý, bajty ROM
     na 0447h, 045Eh a 059Eh jsou původní; true vrátí patch.
  H  (jen mz800emu) Záznam vstupů s checkpointem: SAVE z ROM připojí blok,
     ``replay_rewind`` na checkpoint před ním pásku vrátí (blok zmizí);
     manifest nese nastavení ``direct_save``; při přehrávání je
     settings_set odmítnut; záznam bez klíče ``direct_save`` (starší
     záznam = vypnuto) se se zapnutým nastavením nepřehraje.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
bez sekce [CMTHACK] = výchozí hodnoty, ``--no-save-ini``), bez TCP portu;
zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL (i chybějící binárka; sestavují se všechny čtyři).
"""

import base64
import json
import re
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_record import (  # noqa: E402
    PipeEmu, TestFailure, check, _find_exe, _rom_save_mzf, _HEAD_ADDR, _BODY_ADDR,
    _BODY_SIZE, _STUB_ADDR, _SAVE_RESULT, _LOAD_RESULT, _SAVE_STUB, _LOAD_STUB,
    _ROM_SAVE_MACHINES, _REPO_ROOT)

#: Výchozí ROM 0000h-0FFFh binárky (zdrojový soubor C s polem bajtů).
_ROM_SOURCE = {
    "mz800emu": "src/emulator/mzarch/mz800/memory/ROM/ROM_MZ800_0000.c",
    "mz700emu-pal": "src/emulator/mzarch/mz700/memory/ROM/ROM_MZ700_13A_0000.c",
    "mz700emu-ntsc": "src/emulator/mzarch/mz700/memory/ROM/ROM_MZ700_13A_0000.c",
    "mz1500emu": "src/emulator/mzarch/mz1500/memory/ROM/ROM_MZ1500_0000.c",
}
#: Rozsahy patche Direct Save (adresa, délka): zápis 0447h-044Bh a 045Eh-0463h, ověření 059Eh-05AAh.
_PATCH_RANGES = ((0x0447, 5), (0x045E, 6), (0x059E, 13))
#: Chyba settings_set CMTHACK/direct_save mimo STOP (text jako zásah menu).
_NOT_STOPPED = "Stop the tape to switch Direct Save on or off."
#: Chyba settings_set CMTHACK/direct_save při záznamu nebo přehrávání vstupů.
_IN_REPLAY = "Direct Save cannot be changed while an input replay is recorded or played."
#: Výsledek ROM SAVE s Record (signál v reálném čase): nejvýš snímků.
_ROM_FRAMES = 4000
#: Výsledek VERIFY (55h OK, EEh chyba) a registr A po návratu z @VERFY.
_VERIFY_RESULT = 0x6082
_VERIFY_A = 0x6083
#: 6000 CALL 002Dh (@VERFY) / LD (6083h),A / JR C,600Ch / LD A,55h / JR 600Eh /
#: 600C LD A,0EEh / 600E LD (6082h),A / JR $ (LD nemění příznaky)
_VERIFY_STUB = bytes([0xCD, 0x2D, 0x00, 0x32, 0x83, 0x60, 0x38, 0x04, 0x3E, 0x55, 0x18, 0x02,
                      0x3E, 0xEE, 0x32, 0x82, 0x60, 0x18, 0xFE])
#: "WRITING" v display kódu Sharp (A = 01h ... Z = 1Ah), řádek, který ROM vypíše při WHEAD.
_WRITING = bytes([0x17, 0x12, 0x09, 0x14, 0x09, 0x0E, 0x07])
#: Druhý soubor (fáze E): adresa a délka těla.
_BODY2_ADDR = 0x1400
_BODY2_SIZE = 100
#: Nejvýš snímků na SAVE / VERIFY / LOAD s patchem (proběhne v jednom snímku).
_FAST_FRAMES = 200


def _rom_image(name):
    """Původní obraz dolní ROM binárky ze zdrojového souboru C (4096 B)."""
    text = (_REPO_ROOT / _ROM_SOURCE[name]).read_text(encoding="latin-1")
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", text[text.index("{"):]))
    if len(data) < 0x1000:
        raise TestFailure(f"ROM image {_ROM_SOURCE[name]} has {len(data)} bytes")
    return data[:0x1000]


def _xor(data):
    """XOR všech bajtů."""
    x = 0
    for b in data:
        x ^= b
    return x


def _mem(emu, addr, length):
    """Bajty paměti tak, jak je vidí CPU (mem_read)."""
    d = emu.call("mem_read", {"addr": addr, "len": length})
    return base64.b64decode(d.get("data_b64", ""))


def _cmt(emu):
    """Stav kazety (emulator://periph/cmt)."""
    return emu.call("get_periph_cmt")


def _tape(emu):
    """Výpis pásky (emulator://periph/cmt/tape)."""
    return emu.call("cmt_tape_list")


def _run_stub(emu, stub, result_addr, frames=_FAST_FRAMES):
    """Nahraje stub na 6000h, vynuluje výsledek a běží, dokud výsledek není nenulový."""
    emu.poke(_STUB_ADDR, stub)
    emu.poke(result_addr, b"\x00")
    emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
    return emu.run_until(result_addr, frames, step=10 if frames <= _FAST_FRAMES else 100)


def _save(emu, hdr, body, body_addr, frames=_FAST_FRAMES):
    """Hlavička na 10F0h, tělo na body_addr, SAVE z ROM (@WHEAD + @WDATA); vrátí výsledek stubu."""
    emu.poke(_HEAD_ADDR, hdr)
    emu.poke(body_addr, body)
    return _run_stub(emu, _SAVE_STUB, _SAVE_RESULT, frames)


def _export(emu, work, block):
    """Exportuje blok pásky do MZF a vrátí jeho bajty."""
    out = work / f"block{block}.mzf"
    emu.call("cmt_export_block", {"block": block, "path": str(out)})
    return out.read_bytes()


def _second_mzf():
    """MZF fáze E: typ 01h, jiné jméno, 100 B na 1400h."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"SECOND"
    hdr[1:18] = name + b"\r" * (17 - len(name))
    hdr[0x12:0x14] = _BODY2_SIZE.to_bytes(2, "little")
    hdr[0x14:0x16] = _BODY2_ADDR.to_bytes(2, "little")
    hdr[0x16:0x18] = _BODY2_ADDR.to_bytes(2, "little")
    body = bytes((i * 29 + 101) & 0xFF for i in range(_BODY2_SIZE))
    return bytes(hdr), body


def phase_a(emu, name, orig):
    """Stav po startu a bajty patche v ROM."""
    print(f"Phase A ({name}): Direct Save on after the start, patch bytes in ROM")
    st = _cmt(emu)
    check(st.get("cmthack_enabled") is True and st.get("direct_save") is True
          and st.get("direct_save_enabled") is True,
          f"cmthack_enabled, direct_save and direct_save_enabled true ({st.get('cmthack_enabled')}, "
          f"{st.get('direct_save')}, {st.get('direct_save_enabled')})")
    # zápis: jeden vyrovnávací bajt (044Bh) pro oba rozsahy; ověření: vlastní výplň
    now = [_mem(emu, a, n) for a, n in _PATCH_RANGES]
    was = [orig[a:a + n] for a, n in _PATCH_RANGES]
    check(_xor(now[0] + now[1]) == _xor(was[0] + was[1]),
          f"XOR of the save patch (0447h-044Bh, 045Eh-0463h) equals the original ROM "
          f"({now[0].hex()}, {now[1].hex()})")
    check(_xor(now[2]) == _xor(was[2]),
          f"XOR of the verify patch (059Eh-05AAh) equals the original ROM ({now[2].hex()})")
    check(_mem(emu, 0x0444, 3) == orig[0x0444:0x0447], "0444h: original CALL @CHECK kept")
    check(_mem(emu, 0x0447, 5) == bytes(5), "0447h-044Bh: 5x NOP")
    check(_mem(emu, 0x045E, 2) == b"\xD3\x03", "045Eh: OUT (03h),A")
    check(_mem(emu, 0x0464, 3) == orig[0x0464:0x0467], "0464h: original JP CMTEND kept")
    check(_mem(emu, 0x059E, 2) == b"\xD3\x04", "059Eh: OUT (04h),A")
    check(_mem(emu, 0x05AB, 2) == orig[0x05AB:0x05AD], "05ABh: original JR CMTEND kept")


def phase_b(emu, name, work, hdr, body):
    """SAVE z ROM bez Record."""
    print(f"Phase B ({name}): ROM SAVE without Record stores one MZF block at once")
    check(_cmt(emu).get("filled") is False, "no tape before the SAVE")
    t0 = time.time()
    result, frames = _save(emu, hdr, body, _BODY_ADDR)
    check(result == 0x55, f"ROM WHEAD + WDATA returned OK (result {result:02X}h, {frames} frames, "
                          f"{time.time() - t0:.1f} s)")
    st = _cmt(emu)
    check(st.get("state") == "stop" and st.get("filled") is True,
          f"deck stopped, a new tape inserted ({st.get('state')}, filled {st.get('filled')})")
    tape = _tape(emu)
    blocks = tape.get("blocks") or []
    check(tape.get("count") == 1 and blocks[0].get("type") == 1 and tape.get("current_block") == 0,
          f"tape: 1 MZF block, current block 0 ({tape.get('count')}, "
          f"{[b.get('type') for b in blocks]}, {tape.get('current_block')})")
    check(tape.get("last_recording") is None, f"a direct save is not a recording ({tape.get('last_recording')})")
    data = _export(emu, work, 0)
    check(data == hdr + body, f"exported MZF is byte-identical to memory ({len(data)} bytes)")
    dump = emu.call("get_video_text_dump")
    chars = base64.b64decode(dump.get("chars_b64", ""))
    check(_WRITING in chars, "the screen shows the ROM line WRITING")


def phase_c(emu, name, body):
    """VERIFY z ROM proti otevřenému bloku."""
    print(f"Phase C ({name}): ROM VERIFY against the open block")
    result, frames = _run_stub(emu, _VERIFY_STUB, _VERIFY_RESULT)
    check(result == 0x55, f"VERIFY of the saved area is OK (result {result:02X}h, {frames} frames)")
    changed = bytes([body[0] ^ 0xFF]) + body[1:]
    emu.poke(_BODY_ADDR, changed)
    result, _ = _run_stub(emu, _VERIFY_STUB, _VERIFY_RESULT)
    a = _mem(emu, _VERIFY_A, 1)[0]
    check(result == 0xEE and a == 0x01, f"VERIFY after a changed byte reports an error (result {result:02X}h, A {a:02X}h)")
    emu.poke(_BODY_ADDR, body)
    result, _ = _run_stub(emu, _VERIFY_STUB, _VERIFY_RESULT)
    check(result == 0x55, f"VERIFY with the body back is OK (result {result:02X}h)")
    check(_cmt(emu).get("state") == "stop" and _tape(emu).get("current_block") == 0,
          "VERIFY moves neither the deck nor the tape")
    emu.call("cmt_transport", {"action": "play"})
    check(_cmt(emu).get("state") == "play", "deck playing before VERIFY")
    result, _ = _run_stub(emu, _VERIFY_STUB, _VERIFY_RESULT)
    st = _cmt(emu)
    check(result == 0x55 and st.get("state") == "stop" and _tape(emu).get("current_block") == 0,
          f"VERIFY with the deck playing (block with an MZF) is OK and stops the deck "
          f"(result {result:02X}h, {st.get('state')})")


def phase_d(emu, name, hdr, body):
    """LOAD z ROM přes patch zpět."""
    print(f"Phase D ({name}): ROM LOAD of the saved block through the CMT hack")
    emu.poke(_BODY_ADDR, bytes(_BODY_SIZE))
    emu.poke(_HEAD_ADDR, bytes(128))
    result, frames = _run_stub(emu, _LOAD_STUB, _LOAD_RESULT)
    check(result == 0x55, f"ROM RHEAD + RDATA read the saved block (result {result:02X}h, {frames} frames)")
    check(emu.ram(_BODY_ADDR, _BODY_SIZE) == body, "loaded body equals the saved one")
    check(emu.ram(_HEAD_ADDR, 128) == hdr, "loaded header equals the saved one")


def phase_e(emu, name, work):
    """Druhý soubor jako další blok."""
    print(f"Phase E ({name}): a second file lands as the next block")
    hdr2, body2 = _second_mzf()
    result, _ = _save(emu, hdr2, body2, _BODY2_ADDR)
    check(result == 0x55, f"second ROM SAVE returned OK (result {result:02X}h)")
    tape = _tape(emu)
    check(tape.get("count") == 2 and tape.get("current_block") == 1,
          f"tape: 2 blocks, current block 1 ({tape.get('count')}, {tape.get('current_block')})")
    check(_export(emu, work, 1) == hdr2 + body2, "block 1 is the second file, byte-identical")


def phase_f(emu, name, work, orig):
    """Record + Direct + direct_save: v RECORD patch pryč, SAVE z ROM se nahraje jako signál, soubor se zapíše."""
    print(f"Phase F ({name}): Record pressed + Direct + Direct Save records the ROM SAVE as signal")
    count = _tape(emu).get("count")
    target = work / "f.tmz"
    res = emu.call("cmt_record", {"path": str(target)})
    check("error" not in res and res.get("path") == str(target), f"recording to a file armed ({res})")
    st = _cmt(emu)
    check(st.get("state") == "record" and st.get("direct_save") is False and st.get("direct_save_enabled") is True,
          f"in RECORD the Direct Save patch is not in ROM ({st.get('state')}, {st.get('direct_save')}, "
          f"{st.get('direct_save_enabled')})")
    for addr, length in _PATCH_RANGES:
        now = _mem(emu, addr, length)
        check(now == orig[addr:addr + length], f"{addr:04X}h: original ROM bytes while recording ({now.hex()})")
    hdr, body = _rom_save_mzf()
    result, frames = _save(emu, hdr, body, _BODY_ADDR, _ROM_FRAMES)
    check(result == 0x55, f"ROM SAVE while recording returned OK (result {result:02X}h, {frames} frames)")
    check(_tape(emu).get("count") == count, "nothing was appended directly")
    emu.call("run", {"frames": 10})
    res = emu.call("cmt_transport", {"action": "stop"})
    rec = res.get("recorded")
    print(f"  INFO: recorded {rec}")
    check(rec is not None and rec.get("blocks") == 1 and rec.get("mzf") == 1,
          f"the ROM SAVE was recorded as one MZF block ({rec})")
    check(_tape(emu).get("count") == count + 1, "the recorded block was appended at the stop")
    check(_export(emu, work, count) == hdr + body, f"block {count} is the recorded file")
    check(target.is_file() and target.stat().st_size > 0, f"the recording file was written ({target.name})")
    st = _cmt(emu)
    check(st.get("direct_save") is True and _mem(emu, 0x045E, 2) == b"\xD3\x03",
          f"after the stop the patch is back ({st.get('direct_save')})")


def phase_g(emu, name, orig):
    """direct_save false: patch zápisu z ROM pryč, true ho vrátí."""
    print(f"Phase G ({name}): CMTHACK/direct_save only in STOP; false removes the patch")
    emu.call("cmt_transport", {"action": "play"})
    resp = emu.call_raw("settings_set", {"key": "CMTHACK/direct_save", "value": "false"})
    check(not resp.get("success") and _NOT_STOPPED in (resp.get("error") or ""),
          f"settings_set refused while the deck plays ({resp.get('error')})")
    check(_cmt(emu).get("direct_save_enabled") is True, "the setting did not change")
    emu.call("cmt_transport", {"action": "stop"})
    emu.call("settings_set", {"key": "CMTHACK/direct_save", "value": "false"})
    st = _cmt(emu)
    check(st.get("direct_save") is False and st.get("cmthack_enabled") is True,
          f"direct_save false, CMT hack still on ({st.get('direct_save')}, {st.get('cmthack_enabled')})")
    for addr, length in _PATCH_RANGES:
        now = _mem(emu, addr, length)
        check(now == orig[addr:addr + length], f"{addr:04X}h: original ROM bytes ({now.hex()})")
    emu.call("settings_set", {"key": "CMTHACK/direct_save", "value": "true"})
    check(_cmt(emu).get("direct_save") is True and _mem(emu, 0x045E, 2) == b"\xD3\x03",
          "direct_save true installs the patch again")


def phase_h(emu, name, tmp):
    """Záznam vstupů: rewind na checkpoint před SAVE vrátí pásku."""
    print(f"Phase H ({name}): input replay checkpoint restore after a direct save")
    count = _tape(emu).get("count")
    hdr, body = _second_mzf()
    emu.poke(_HEAD_ADDR, hdr)
    emu.poke(_BODY2_ADDR, body)
    emu.poke(_STUB_ADDR, _SAVE_STUB)
    emu.poke(_SAVE_RESULT, b"\x00")
    emu.call("set_register", {"reg": "PC", "value": _STUB_ADDR})
    st = emu.call("replay_record_start", {"path": str(tmp / "h.mzr"), "hash_interval": 10,
                                          "checkpoint_interval": 50, "checkpoint_count": 4})
    check(st.get("mode") == "recording", f"input replay recording ({st.get('mode')})")
    emu.call("run", {"frames": 10})
    check(emu.ram(_SAVE_RESULT, 1)[0] == 0x55, "ROM SAVE during the recording returned OK")
    check(_tape(emu).get("count") == count + 1, "the file was appended")
    st = emu.call("replay_rewind")
    check(st.get("frame") == 0, f"rewind to the checkpoint at frame 0 ({st.get('frame')})")
    check(_tape(emu).get("count") == count, f"the appended block is gone after the rewind ({_tape(emu).get('count')})")
    # záznam po rewindu ještě pár snímků (záznam s 0 snímky se při přehrání hned ukončí)
    emu.call("run", {"frames": 20})
    st = emu.call("replay_record_stop")
    check(st.get("outcome") == "recorded", f"input replay saved ({st.get('outcome_message')})")

    rec = tmp / "h.mzr"
    with zipfile.ZipFile(rec) as z:
        manifest = json.loads(z.read("manifest.json"))
    cmt = manifest.get("cmt") or {}
    check(cmt.get("direct_save") is True, f"the manifest stores the Direct Save setting ({cmt})")
    st = emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
    check(st.get("mode") == "playing", f"input replay playing ({st})")
    resp = emu.call_raw("settings_set", {"key": "CMTHACK/direct_save", "value": "false"})
    check(not resp.get("success") and _IN_REPLAY in (resp.get("error") or ""),
          f"settings_set refused while an input replay is played ({resp.get('error')})")
    emu.call("replay_play_stop")

    old = tmp / "h_old.mzr"
    with zipfile.ZipFile(rec) as zi, zipfile.ZipFile(old, "w", zipfile.ZIP_DEFLATED) as zo:
        for it in zi.infolist():
            data = zi.read(it.filename)
            if it.filename == "manifest.json":
                m = json.loads(data)
                m["cmt"].pop("direct_save", None)
                data = json.dumps(m).encode()
            zo.writestr(it.filename, data)
    resp = emu.call_raw("replay_play", {"path": str(old), "pause_after_load": True})
    err = resp.get("error") or ""
    check(not resp.get("success") and "Direct Save of the CMT hack was off when recording, here it is on" in err,
          f"a replay without the direct_save key (older) is refused with Direct Save on ({err})")


def run_machine(name, exe, tmp):
    """Všechny fáze pro jednu binárku."""
    orig = _rom_image(name)
    work = tmp / name
    emu = PipeEmu(exe, work / "cfg", hack=True)
    try:
        emu.call("run", {"frames": 350})
        hdr, body = _rom_save_mzf()
        phase_a(emu, name, orig)
        phase_b(emu, name, work, hdr, body)
        phase_c(emu, name, body)
        phase_d(emu, name, hdr, body)
        phase_e(emu, name, work)
        phase_f(emu, name, work, orig)
        phase_g(emu, name, orig)
        if name == "mz800emu":
            phase_h(emu, name, work)
    finally:
        emu.close()


def main():
    if len(sys.argv) != 3 or sys.argv[1] != "--machine":
        print("usage: test_cmt_direct_save.py --machine <binary>")
        return 1
    name = sys.argv[2]
    emu_test_proc.install(deadline_s=170)
    if name not in _ROM_SAVE_MACHINES:
        print(f"FAIL: unknown binary {name} (allowed: {', '.join(_ROM_SAVE_MACHINES)})")
        return 1
    exe = _find_exe(name)
    if exe is None:
        print(f"FAIL: missing binary {name}")
        return 1
    with tempfile.TemporaryDirectory(prefix="mz_cmt_direct_save_") as td:
        try:
            run_machine(name, exe, Path(td))
        except TestFailure as exc:
            print(f"  FAIL: {exc}")
            print("FAILED")
            return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
