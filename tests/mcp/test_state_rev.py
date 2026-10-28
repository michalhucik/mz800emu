#!/usr/bin/env python3
"""E2E test: revize stavu stroje state_rev (MCP pipe, M18).

Ověřuje přes MCP:

  1  čtecí nástroje (mem_read, get_registers, state_observe, watch_eval,
     get_state) a pauza v pauze revizi nemění,
  2  zápis (mem_write, set_register), reset, načtení snapshotu a běh
     (run, step_into) revizi zvednou; odpověď zápisu vrací state_rev
     shodnou s get_state,
  3  podmíněný zápis s expected_state_rev: aktuální projde, stará
     (i po zápisu jinam) selže `Precondition failed: state changed` a nic
     nezapíše, totéž u io_write, input_send_key a media_eject; revize se
     kontroluje před expected_sha256 a nesplněná podmínka obsahu revizi
     nezvedne; čtecí nástroj podmínku odmítne,
  4  state_observe s if_state_rev_differs: shoda = not_modified, jinak
     plná odpověď,
  5  odmítnutý podmíněný zápis během záznamu vstupů se nezaznamená:
     přehrání doběhne bez rozjetí a hodnota v paměti odpovídá záznamu,
  6  událost state_changed: vlastní zápis a reset MCP klienta ji nepošlou,
     požadavek záznamu vstupů v pauze pošle jednu se zdrojem replay;
     odpověď reset nese revizi po resetu,
  7  všechny čtecí nástroje (druh READ) a vybrané konfigurace debuggeru
     revizi v pauze nezvednou.

Proces běží izolovaně (dočasný ``--cfg-dir``, INI bez načítání uložených
BP, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Program: DI; JR $ (smyčka, stav se během běhu mění jen v R a čase).
PROG = 0x3000
PROG_HEX = "F3" "18FE"
WAIT_MS = 20000

#: Nástroje s cílovým dbgapi příkazem druhu READ (dbgapi_cmd_props.c) bez
#: těch, které spouštějí běh (run_until_raster, run_until_tstate), zapisují
#: soubory (snapshot_save, trace_save, cdl_export, profiler_export,
#: screenshot_save_to_file) nebo mažou breakpointy (bp_clear). Volají se
#: s prázdnými parametry; na úspěchu nezáleží, revize se měnit nesmí.
READ_TOOLS = (
    "bp_list callstack_get cmt_tape_list dasm frame_hash freeze_list get_bookmarks get_callstack "
    "get_cpu_flags get_cpu_im2_vector get_cpu_interrupt_bus get_cpu_panel_batch "
    "get_frame_framebuffer_info get_frame_screenshot get_frame_screenshot_raw get_im2_vector "
    "get_input_joystick_state get_input_keyboard_matrix_info get_input_keyboard_state get_last_instr "
    "get_media_state get_memext_info get_memory_map get_periph_ay3_8910 get_periph_beeper "
    "get_periph_cmt get_periph_gdg get_periph_i8253 get_periph_i8255 get_periph_qd "
    "get_periph_sn76489 get_periph_wd1793 get_periph_z80_pio get_profiler get_raster_pos get_reg "
    "get_registers get_speed get_stack get_stack_history get_stack_regions get_state get_symbols "
    "get_vars get_video_text_dump get_watch get_watch_snapshot history_get is_debugger_active "
    "media_state mem_read memsearch_filter memsearch_sample memsearch_start profiler_get "
    "region_read regions_list settings_get snapshot_save_buffer state_observe symbol_list "
    "symbol_lookup trace_read trace_status watch_eval watch_list").split()

#: Konfigurace debuggeru (druh DEBUG_CONFIG) s platnými parametry.
CONFIG_CALLS = (
    ("bp_add", {"addr": 0x1234}),
    ("watch_add", {"expr": "HL", "name": "w"}),
    ("symbol_add", {"name": "srev_sym", "addr": 0x1234}),
    ("bookmark_add", {"addr": 0x1234, "label": "b"}),
    ("eventlog_set_mask", {"mask": 0}),
)


def rev(emu):
    """Aktuální revize z get_state."""
    return emu.call("get_state")["state_rev"]


def call_err(emu, cmd, data):
    """Příkaz musí selhat; vrátí text chyby."""
    emu.rid += 1
    req = {"type": "request", "req_id": emu.rid, "cmd": cmd, "data": data}
    emu.proc.stdin.write(json.dumps(req) + "\n")
    emu.proc.stdin.flush()
    resp = emu._read(40, lambda m: m.get("type") == "response" and m.get("req_id") == emu.rid)
    check(resp is not None and resp.get("success") is False,
          f"{cmd} fails ({resp.get('error') if resp else 'no response'})")
    return resp.get("error", "")


def events(emu, wait_ms=300):
    """Data událostí state_changed ve frontě (po krátkém čekání)."""
    time.sleep(wait_ms / 1000.0)
    d = emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})
    return [ev.get("data", {}) for ev in d.get("events") or [] if ev.get("topic") == "state_changed"]


def call_any(emu, cmd, data):
    """Pošle příkaz a vrátí response (úspěch i chybu), bez kontroly."""
    emu.rid += 1
    req = {"type": "request", "req_id": emu.rid, "cmd": cmd, "data": data}
    emu.proc.stdin.write(json.dumps(req) + "\n")
    emu.proc.stdin.flush()
    resp = emu._read(40, lambda m: m.get("type") == "response" and m.get("req_id") == emu.rid)
    check(resp is not None, f"{cmd} answered")
    return resp


def byte_at(emu, addr):
    """Bajt paměti CPU."""
    return base64.b64decode(emu.call("mem_read", {"addr": addr, "len": 1})["data_b64"])[0]


def run(emu, tmp):
    """Scénáře 1-5."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})

    # 1: čtení a pauza revizi nemění
    r0 = rev(emu)
    check(r0 > 0, f"get_state has state_rev ({r0})")
    emu.call("mem_read", {"addr": 0x4000, "len": 16})
    emu.call("get_registers")
    o = emu.call("state_observe", {"windows": [{"addr": 0x4000, "len": 4}]})
    emu.call("watch_eval", {"expr": "HL + 1"})
    emu.call("pause")
    check(rev(emu) == r0 and o["state_rev"] == r0, "1: reads and pause in pause keep state_rev")

    # 2: zápisy, reset, snapshot, běh zvednou
    w = emu.call("mem_write", {"addr": 0x4000, "data_hex": "11"})
    r1 = rev(emu)
    check(r1 != r0 and w.get("state_rev") == r1, f"2: mem_write bumps state_rev and returns it ({w.get('state_rev')}, {r1})")
    snap = emu.call("snapshot_save_buffer", {"description": "x"})["bytes_b64"]
    check(rev(emu) == r1, "2: snapshot_save_buffer keeps state_rev")
    emu.call("snapshot_load_buffer", {"bytes_b64": snap})
    r2 = rev(emu)
    check(r2 != r1, "2: snapshot_load_buffer bumps state_rev")
    emu.call("step_into")
    r3 = rev(emu)
    check(r3 != r2, "2: step_into bumps state_rev")
    emu.call("run", {"frames": 1})
    r4 = rev(emu)
    check(r4 != r3, "2: run bumps state_rev")

    # 3: podmíněný zápis
    w = emu.call("set_register", {"reg": "HL", "value": 0x1234, "expected_state_rev": r4})
    r5 = w["state_rev"]
    check(r5 != r4 and rev(emu) == r5, "3: set_register with the current revision passes")
    emu.call("mem_write", {"addr": 0x5000, "data_hex": "22"})       # zásah jinam
    e = call_err(emu, "mem_write", {"addr": 0x4000, "data_hex": "99", "expected_state_rev": r5})
    check(e.startswith("Precondition failed: state changed"), f"3: stale revision refused ({e})")
    check(byte_at(emu, 0x4000) == 0x11, "3: nothing written after the refusal")
    for cmd, data in (("io_write", {"port": 0xE0, "value": 0}),
                      ("input_send_key", {"key": "A"}),
                      ("media_eject", {"slot": "qd"})):
        before = rev(emu)
        e = call_err(emu, cmd, dict(data, expected_state_rev=r5))
        check(e.startswith("Precondition failed: state changed") and rev(emu) == before,
              f"3: {cmd} with a stale revision refused, nothing changed ({e})")
    bad = "0" * 64
    cur = rev(emu)
    e = call_err(emu, "mem_write", {"addr": 0x4000, "data_hex": "99",
                                    "expected_state_rev": cur - 1, "expected_sha256": bad})
    check(e.startswith("Precondition failed: state changed"), f"3: revision checked before the content ({e})")
    e = call_err(emu, "mem_write", {"addr": 0x4000, "data_hex": "99",
                                    "expected_state_rev": cur, "expected_sha256": bad})
    check(e.startswith("Precondition failed: memory changed") and rev(emu) == cur,
          f"3: both conditions in one call, failed content keeps state_rev ({e})")
    e = call_err(emu, "mem_read", {"addr": 0, "len": 1, "expected_state_rev": rev(emu)})
    check("expected_state_rev" in e, f"3: a read tool refuses expected_state_rev ({e})")

    # 4: if_state_rev_differs
    cur = rev(emu)
    o = emu.call("state_observe", {"windows": [], "if_state_rev_differs": cur})
    check(o.get("not_modified") is True and o["state_rev"] == cur, "4: not_modified when the revision matches")
    o = emu.call("state_observe", {"windows": [], "if_state_rev_differs": cur - 1})
    check("not_modified" not in o and "regs" in o, "4: full answer when the revision differs")

    # 5: odmítnutý podmíněný zápis při záznamu se nezaznamená
    rec = tmp / "srev.mzr"
    emu.call("mem_write", {"addr": 0x4100, "data_hex": "00"})
    st = emu.call("replay_record_start", {"path": str(rec), "hash_interval": 5})
    check(st.get("mode") == "recording", "5: recording started")
    emu.call("run", {"frames": 5})
    stale = rev(emu) - 1
    e = call_err(emu, "mem_write", {"addr": 0x4100, "data_hex": "77", "expected_state_rev": stale})
    check(e.startswith("Precondition failed"), f"5: stale write refused while recording ({e})")
    emu.call("run", {"frames": 5})
    emu.call("replay_record_stop")
    # Přehrání je další běh po pauze; wait_stop bez run_seq by před jeho
    # rozběhem vrátil předchozí zastavení (N108).
    play_run = emu.call("get_state")["run_seq"] + 1
    emu.call("replay_play", {"path": str(rec), "pause_after_load": False})
    w = emu.call("wait_stop", {"run_seq": play_run, "timeout_ms": WAIT_MS},
                 timeout=WAIT_MS / 1000 + 10)
    st = emu.call("replay_status")
    check(st.get("outcome") == "ended" and st.get("deviated") is False,
          f"5: playback ends without desync ({st.get('outcome')}, {st.get('outcome_message')!r})")
    check(byte_at(emu, 0x4100) == 0x00, "5: the refused write was not replayed")

    # 6: událost state_changed jen pro změny mimo MCP klienta
    emu.call("pause")
    emu.call("event_subscribe", {"topics": ["state_changed"]})
    events(emu)
    emu.call("mem_write", {"addr": 0x4200, "data_hex": "01"})
    w = emu.call("reset")
    check(w.get("state_rev") == rev(emu), f"6: reset in pause returns the revision after the reset ({w})")
    emu.call("pause")
    ev0 = events(emu)
    check(ev0 == [], f"6: own MCP writes and reset give no state_changed ({ev0})")
    rec2 = tmp / "srev2.mzr"
    emu.call("replay_record_start", {"path": str(rec2), "hash_interval": 5})
    ev = events(emu)
    r_now = rev(emu)
    emu.call("replay_record_stop")
    check(len(ev) == 1 and "replay" in ev[0].get("sources", []) and ev[0].get("state_rev") == r_now,
          f"6: a replay request in pause gives one state_changed with source replay ({ev}, {r_now})")

    # 7: žádný čtecí nástroj ani konfigurace debuggeru revizi nezvedne
    emu.call("pause")
    r7 = rev(emu)
    ok = 0
    for cmd in READ_TOOLS:
        ok += bool(call_any(emu, cmd, {}).get("success"))
    for cmd, data in CONFIG_CALLS:
        ok += bool(call_any(emu, cmd, data).get("success"))
    r7b = rev(emu)
    check(r7b == r7, f"7: {len(READ_TOOLS)} read tools and {len(CONFIG_CALLS)} debugger config calls "
          f"keep state_rev ({ok} succeeded, {r7} -> {r7b})")


def main():
    emu_test_proc.install(deadline_s=150)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_state_rev_"))
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
