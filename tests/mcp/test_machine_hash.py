#!/usr/bin/env python3
"""E2E test: otisk stroje machine_hash a úplnost revize stavu (MCP pipe, M19).

Ověřuje přes MCP:

  1  tvar odpovědi machine_hash (všechny části, pokryté a nepokryté části,
     výběr částí, include_media) a že dva otisky v pauze bez zásahu jsou
     stejné; vypíše cenu výpočtu,
  2  úplnost revize: pro každý nástroj, který mění stav (cílový dbgapi
     příkaz druhu MUTATES), v pauze platí "změnil se otisk => změnila se
     state_rev"; u vybraných zásahů se otisk změnit musí (mapa paměti,
     ROM přes CMT hack, média, vstupy, kazeta),
  3  čtecí nástroje (druh READ) nemění otisk ani revizi,
  4  ABA: zápis a návrat hodnoty = stejný otisk, jiná revize.

Proces běží izolovaně (dočasný ``--cfg-dir``, INI bez načítání uložených
BP, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, _make_mzf, check  # noqa: E402
from test_state_rev import READ_TOOLS, call_any  # noqa: E402

#: Program: DI; JR $ (smyčka).
PROG = 0x3000
PROG_HEX = "F3" "18FE"

#: Všechny části otisku (machine_hash.h).
#: Zápis do paměti CPU: podle mapování RAM nebo RAM MemExt.
MEM = ("ram", "memext")

PARTS = ["cpu", "ram", "memext", "vram", "pcg", "periph",
         "memmap", "rom", "media", "inputs", "ramdisk", "cmt"]


def mh(emu, media=True):
    """Otisk stroje v pauze (s obsahem médií)."""
    d = emu.call("machine_hash", {"include_media": media})
    check(d.get("stable") is True, "machine_hash in pause is stable")
    return d


def changed_parts(a, b):
    """Jména částí, jejichž hash se liší."""
    return [p for p in PARTS if a["parts"].get(p) != b["parts"].get(p)]


def step(emu, name, cmd, data, must_change=None):
    """Jeden zásah v pauze: otisk a revize před a po.

    Platí "změnil se otisk => změnila se state_rev". @p must_change =
    části, které se změnit musí (jinak se zásah neprojevil a test by nic
    neověřil); položka n-tice = stačí jedna z částí (zápis do paměti CPU
    jde podle mapování do RAM nebo do MemExt). Vrátí změněné části.
    """
    h0 = mh(emu)
    resp = call_any(emu, cmd, data)
    if not emu.call("get_state").get("paused"):
        emu.call("pause")
    h1 = mh(emu)
    parts = changed_parts(h0, h1)
    ok = "ok" if resp.get("success") else f"error: {resp.get('error')}"
    print(f"    {name}: {ok}; parts changed: {parts or '-'}; "
          f"state_rev {h0['state_rev']} -> {h1['state_rev']}")
    if parts or h0["sha256"] != h1["sha256"]:
        check(h1["state_rev"] != h0["state_rev"], f"2: {name}: fingerprint changed => state_rev changed")
    for p in must_change or []:
        alts = p if isinstance(p, tuple) else (p,)
        check(any(a in parts for a in alts), f"2: {name}: part {'/'.join(alts)} changed")
    return parts


def scenario_1(emu):
    """Tvar odpovědi, stabilita, cena."""
    a = mh(emu, media=False)
    check(len(a["sha256"]) == 64, "1: sha256 is 64 hex chars")
    check(sorted(a["parts"]) == sorted(PARTS), "1: all parts present in the answer")
    memext = emu.call("get_memext_info").get("connected")
    check((a["parts"]["memext"] is None) == (not memext) and ("memext" in a["covered"]) == bool(memext),
          f"1: memext part follows MemExt connection (connected: {memext})")
    check(a["parts"]["pcg"] is None and "pcg" not in a["covered"], "1: pcg is null on MZ-800")
    for p in ("cpu", "ram", "vram", "periph", "memmap", "rom", "media", "inputs", "ramdisk", "cmt"):
        check(p in a["covered"] and len(a["parts"][p]) == 64, f"1: part {p} covered")
    check("screen" in a["not_covered"] and "media_content" in a["not_covered"],
          "1: not_covered lists screen and media_content without include_media")
    b = mh(emu, media=False)
    check(a["sha256"] == b["sha256"] and a["state_rev"] == b["state_rev"],
          "1: two fingerprints in pause without action are equal")
    c = mh(emu, media=True)
    check(c["include_media"] is True and "media_content" not in c["not_covered"],
          "1: include_media covers media content")
    sub = emu.call("machine_hash", {"parts": ["cpu", "rom"]})
    check(sorted(sub["parts"]) == ["cpu", "rom"] and sub["parts"]["rom"] == a["parts"]["rom"],
          "1: parts selects a subset with the same part hashes")
    print(f"    cost: {a['elapsed_us']} us without media content, "
          f"{c['elapsed_us']} us with media content")


def scenario_2(emu, tmp, mzf, snap):
    """Úplnost revize přes všechny nástroje druhu MUTATES."""
    binf = tmp / "data.bin"
    binf.write_bytes(bytes(range(32)))
    wav = tmp / "rec.wav"
    mzt = tmp / "tape.mzt"
    mzt.write_bytes(mzf.read_bytes() * 2)

    step(emu, "mem_write", "mem_write", {"addr": 0x4000, "data_hex": "5A"}, [MEM])
    step(emu, "mem_write_force", "mem_write_force", {"addr": 0x4001, "data_hex": "5B"}, [MEM])
    step(emu, "region_write", "region_write",
         {"region_id": 1, "offset": 0x4002, "data_b64": base64.b64encode(b"\x5c").decode("ascii")}, [MEM])
    step(emu, "set_register", "set_register", {"reg": "HL", "value": 0x1234}, ["cpu"])
    step(emu, "set_cpu_flags", "set_cpu_flags", {"i": 0x12}, ["cpu"])
    step(emu, "io_write E0h", "io_write", {"port": 0xE0, "value": 0}, ["memmap"])
    step(emu, "io_write E4h", "io_write", {"port": 0xE4, "value": 0}, ["memmap"])
    step(emu, "io_read E0h", "io_read", {"port": 0xE0})
    step(emu, "irq_inject", "irq_inject", {"source": "manual", "vector": 0xF0})
    step(emu, "nmi_inject", "nmi_inject", {})
    step(emu, "set_pioz80_interrupt_vector", "set_pioz80_interrupt_vector", {"port": 0, "vector": 0x20})
    step(emu, "freeze_add", "freeze_add", {"kind": "ram", "offset": 0x4010, "value": 0x77})
    step(emu, "input_press_key", "input_press_key", {"key": "A"}, ["inputs"])
    step(emu, "input_release_key", "input_release_key", {"key": "A"}, ["inputs"])
    step(emu, "input_send_key", "input_send_key", {"key": "B", "frames": 2})
    step(emu, "input_send_joystick", "input_send_joystick", {"port": 0, "state": 1, "frames": 2})
    step(emu, "input_send_keys", "input_send_keys", {"text": "AB"})
    step(emu, "input_release_key all", "input_release_key", {"key": ""})
    step(emu, "cmt_open", "cmt_open", {"path": str(mzf), "play_immediately": False}, ["media", "cmt"])
    step(emu, "cmt_transport play", "cmt_transport", {"action": "play"}, ["cmt"])
    step(emu, "cmt_transport stop", "cmt_transport", {"action": "stop"}, ["cmt"])
    step(emu, "cmt_open mzt", "cmt_open", {"path": str(mzt), "play_immediately": False}, ["media", "cmt"])
    step(emu, "cmt_tape_seek", "cmt_tape_seek", {"block_id": 1}, ["cmt"])
    step(emu, "cmt_set_property speed", "cmt_set_property", {"property": "speed", "value": 2}, ["cmt"])
    step(emu, "cmt_set_property polarity", "cmt_set_property", {"property": "polarity", "value": 1}, ["cmt"])
    step(emu, "cmt_tape_block_speed", "cmt_tape_block_speed", {"block_id": 0, "speed": 4}, ["cmt"])
    step(emu, "cmt_hack_set off", "cmt_hack_set", {"enabled": False})
    step(emu, "cmt_hack_set on", "cmt_hack_set", {"enabled": True}, ["rom"])
    step(emu, "cmt_transport eject", "cmt_transport", {"action": "eject"}, ["media", "cmt"])
    step(emu, "cmt_record", "cmt_record", {"path": str(wav)}, ["cmt"])
    step(emu, "cmt_transport stop after record", "cmt_transport", {"action": "stop"})
    step(emu, "media_insert cmt", "media_insert", {"slot": "cmt", "path": str(mzf), "ro": True}, ["media"])
    step(emu, "media_eject cmt", "media_eject", {"slot": "cmt"}, ["media"])
    step(emu, "media_load_mzf", "media_load_mzf", {"path": str(mzf)}, [MEM])
    step(emu, "media_load_binary", "media_load_binary", {"path": str(binf), "addr": 0x5000}, [MEM])
    step(emu, "settings_set QDISK/write_protected", "settings_set",
         {"key": "QDISK/write_protected", "value": "1"})
    step(emu, "periph_attach memext", "periph_attach", {"kind": "memext"})
    step(emu, "periph_detach memext", "periph_detach", {"kind": "memext"})
    step(emu, "platform_set", "platform_set", {"kind": "mz800"})
    step(emu, "snapshot_load_buffer", "snapshot_load_buffer", {"bytes_b64": snap})
    step(emu, "reset", "reset", {}, ["cpu"])
    step(emu, "media_run_mzf", "media_run_mzf", {"path": str(mzf)})


def scenario_3(emu):
    """Čtecí nástroje nemění otisk ani revizi."""
    h0 = mh(emu)
    bad = []
    for cmd in READ_TOOLS:
        call_any(emu, cmd, {})
        h1 = mh(emu)
        if h1["sha256"] != h0["sha256"] or h1["state_rev"] != h0["state_rev"]:
            bad.append(f"{cmd} ({changed_parts(h0, h1)})")
            h0 = h1
    check(not bad, f"3: {len(READ_TOOLS)} READ tools keep fingerprint and state_rev ({bad or 'all'})")


def scenario_4(emu):
    """ABA: obsah zpět = stejný otisk, revize jiná."""
    h0 = mh(emu)
    old = emu.call("mem_read", {"addr": 0x4100, "len": 1})["data_b64"]
    new = base64.b64encode(bytes([base64.b64decode(old)[0] ^ 0xFF])).decode("ascii")
    emu.call("region_write", {"region_id": 1, "offset": 0x4100, "data_b64": new})
    emu.call("region_write", {"region_id": 1, "offset": 0x4100, "data_b64": old})
    h1 = mh(emu)
    check(h1["sha256"] == h0["sha256"], "4: write and write back keep the fingerprint")
    check(h1["state_rev"] != h0["state_rev"], "4: write and write back change state_rev (ABA)")


def run(emu, tmp):
    """Scénáře 1-4."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    mzf = tmp / "prog.mzf"
    _make_mzf(mzf)
    snap = emu.call("snapshot_save_buffer", {"description": "mh"})["bytes_b64"]

    print("Scenario 1: answer and cost")
    scenario_1(emu)
    print("Scenario 2: completeness over MUTATES tools")
    scenario_2(emu, tmp, mzf, snap)
    print("Scenario 3: READ tools")
    emu.call("pause")
    scenario_3(emu)
    print("Scenario 4: ABA")
    scenario_4(emu)


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_machine_hash_"))
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
