#!/usr/bin/env python3
"""E2E test: výstup akcí breakpointů ``log`` a ``stop`` čitelný přes MCP (kanál bplog).

Ověřuje na skutečném emulátoru (MZ-800, MCP pipe):

  1  před první akcí je kanál prázdný, ale už má segment (běží vždy),
     ``bplog_clear`` nic nemaže a kurzor z prázdného kanálu platí i po
     prvních záznamech;
  2  MEM_W BP s akcí ``log "A=%X", A`` na zápisu ve smyčce: jeden řádek na
     zápis, text podle hodnoty A, ``insn_pc`` = zapisující instrukce,
     ``pc`` = živé PC za ní, id a jméno BP, čas roste;
  3  PC_EXEC BP s akcí ``stop "done B=%d", B`` zastaví a hlášku zapíše
     jako druh ``stop``; filtr ``kind`` a ``bp`` (id i jméno);
  4  přetečení ringu (5000 zápisů, kapacita 4096): ``first_seq`` /
     ``lost``, starý kurzor vrátí ``gap``, poslední text odpovídá;
  5  ``bplog_clear`` založí nový segment, starý kurzor ->
     ``segment_replaced:``.

Program (RAM 3000h):

    3000 F3        DI
    3001 06 0A     LD B,10
    3003 78        LD A,B
    3004 32 00 40  LD (4000h),A
    3007 10 FA     DJNZ 3003h
    3009 18 FE     JR $

Proces běží izolovaně (dočasný ``--cfg-dir``, vlastní INI bez načítání
uložených BP, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup
anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Program smyčky se zápisem (viz docstring modulu).
PROG = 0x3000
PROG_HEX = "F3" "060A" "78" "320040" "10FA" "18FE"
#: Smyčka 5000 zápisů: DI; LD DE,5000; LD A,E; LD (4000h),A; DEC DE;
#: LD A,D; OR E; JR NZ,3004h; JR $.
PROG_MANY_HEX = "F3" "118813" "7B" "320040" "1B" "7A" "B3" "20F7" "18FE"
#: Kapacita ringu bplog (BPLOG_CAPACITY).
CAPACITY = 4096
#: Jak dlouho čekat na zastavení [s].
WAIT_S = 5.0


def _rows(emu, flt=None, columns=None, cursor=None):
    """Všechny řádky kanálu bplog od nejstaršího (nebo od kurzoru); vrátí (řádky, poslední stránka)."""
    out = []
    for _ in range(100):
        req = {"channel": "bplog", "limit": 5000, "max_chars": 45000}
        if columns:
            req["columns"] = columns
        if flt:
            req["filter"] = flt
        if cursor:
            req["cursor"] = cursor
        r = emu.call("trace_read", req)
        out.extend(r["rows"])
        cursor = r["next_cursor"]
        if r["end_of_data"]:
            return out, r
    raise TestFailure("trace_read bplog did not reach end_of_data")


def _wait_paused(emu):
    """Počká na pauzu (BP s akcí stop); vrátí True, když se emulace zastavila."""
    deadline = time.time() + WAIT_S
    while time.time() < deadline:
        if emu.call("get_state").get("paused"):
            return True
        time.sleep(0.05)
    emu.call("pause")
    return False


def _status(emu):
    """Stav kanálu bplog z trace_status."""
    return emu.call("trace_status", {"channel": "bplog"})["channels"][0]


def case_empty(emu):
    """Scénář 1: bez akce kanál prázdný, clear nic nemaže; vrátí kurzor prázdného kanálu."""
    st = _status(emu)
    check(st["segment"] >= 1 and st["live"] and st["next_seq"] == 0 and st["record_size"] == 296,
          f"no action yet: empty channel, already a segment ({st})")
    cl = emu.call("bplog_clear")
    check(cl["allocated"] is False and cl["segment"] == st["segment"] and cl["capacity"] == CAPACITY,
          f"bplog_clear before any action changes nothing ({cl})")
    rows, last = _rows(emu)
    check(rows == [] and last["end_of_data"], "empty page before any action")
    return last["next_cursor"]


def case_log_stop(emu, empty_cursor):
    """Scénáře 2 a 3: log z MEM_W BP, stop z PC_EXEC BP, filtry.

    Vlastní jméno BP vyžaduje ``auto_name: false`` (jinak ho nastavení adresy
    přepíše na ``Addr: 0x....``).
    """
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    wr = emu.call("bp_create_with_init", {
        "fields": ["type", "addr", "auto_name", "name", "action", "enabled"],
        "type": "MEM_W", "addr": 0x4000, "auto_name": False, "name": "wr",
        "action": 'log "A=%X", A', "enabled": True})["id"]
    end = emu.call("bp_create_with_init", {
        "fields": ["type", "addr", "auto_name", "name", "action", "enabled"],
        "type": "PC_EXEC", "addr": 0x3009, "auto_name": False, "name": "end",
        "action": 'stop "done B=%d", B', "enabled": True})["id"]
    emu.call("run")
    check(_wait_paused(emu), "PC_EXEC BP with stop paused the emulation")

    rows, last = _rows(emu)
    cont, _ = _rows(emu, cursor=empty_cursor)
    check(cont == rows, f"cursor from the empty channel reads the new rows ({len(cont)})")
    cols = last["columns"]
    check(cols == ["seq", "t", "frame", "insn_pc", "sym", "pc", "bp", "name", "kind", "text", "flags"],
          f"bplog columns ({cols})")
    check(len(rows) == 11, f"10 log rows and 1 stop row ({len(rows)})")
    logs = rows[:10]
    texts = [r[9] for r in logs]
    check(texts == [f"A={v:X}" for v in range(10, 0, -1)], f"log text follows A ({texts})")
    check(all(r[3] == "3004" and r[5] == "3007" for r in logs),
          f"insn_pc = writing instruction 3004h, pc = live PC 3007h ({logs[0]})")
    check(all(r[6] == wr and r[7] == "wr" and r[8] == "log" and r[10] == "" for r in logs),
          f"breakpoint id {wr}, name wr, kind log ({logs[0]})")
    ts = [r[1] for r in logs]
    check(all(a < b for a, b in zip(ts, ts[1:])), f"time grows ({ts[:3]} ...)")
    stop = rows[10]
    check(stop[3] == "3009" and stop[5] == "3009" and stop[6] == end and stop[7] == "end"
          and stop[8] == "stop" and stop[9] == "done B=0" and stop[1] > ts[-1],
          f"stop message recorded as kind stop ({stop})")

    for flt, n in (({"kind": "stop"}, 1), ({"kind": "log"}, 10), ({"bp": wr}, 10),
                   ({"bp": "end"}, 1), ({"pc_from": 0x3009, "pc_to": 0x3009}, 1)):
        got, _ = _rows(emu, flt=flt)
        check(len(got) == n, f"filter {flt}: {n} rows ({len(got)})")
    for bp_id in (wr, end):
        emu.call("bp_remove", {"id": bp_id})
    return last["next_cursor"], last["segment"]


def case_overflow(emu):
    """Scénář 4: 5000 zápisů do ringu 4096: lost, gap, poslední text."""
    emu.call("pause")
    cl = emu.call("bplog_clear")
    check(cl["allocated"] and cl["next_seq"] == 0, f"cleared before overflow ({cl})")
    _, start = _rows(emu)          # prázdný kanál: kurzor na seq 0
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_MANY_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    wr = emu.call("bp_create_with_init", {
        "fields": ["type", "addr", "action", "enabled"],
        "type": "MEM_W", "addr": 0x4000, "action": 'log "%d", DE', "enabled": True})["id"]
    end = emu.call("bp_create_with_init", {
        "fields": ["type", "addr", "action", "enabled"],
        "type": "PC_EXEC", "addr": 0x300D, "action": "stop", "enabled": True})["id"]
    emu.call("run")
    check(_wait_paused(emu), "loop of 5000 writes finished")
    st = _status(emu)
    check(st["next_seq"] == 5000 and st["first_seq"] == 5000 - CAPACITY and st["lost"] == 5000 - CAPACITY,
          f"ring keeps the newest {CAPACITY}: first_seq {st['first_seq']}, lost {st['lost']}")
    r = emu.call("trace_read", {"channel": "bplog", "cursor": start["next_cursor"], "limit": 2,
                                "columns": ["seq", "text"]})
    check(r["gap"] == {"from_seq": 0, "to_seq": 5000 - CAPACITY} and r["rows"][0][0] == 5000 - CAPACITY,
          f"old cursor: gap and first readable record ({r['gap']}, {r['rows'][0]})")
    # Bez hlášky stop nic nezapíše; poslední log je DE = 1.
    newest = emu.call("trace_read", {"channel": "bplog", "from": "newest", "limit": 1,
                                     "columns": ["seq", "kind", "text"]})
    check(newest["rows"] == [[4999, "log", "1"]], f"newest record is the last write ({newest['rows']})")
    for bp_id in (wr, end):
        emu.call("bp_remove", {"id": bp_id})
    return r["next_cursor"], st["segment"]


def case_clear(emu, cursor, segment):
    """Scénář 5: clear = nový segment, starý kurzor neplatí."""
    cl = emu.call("bplog_clear")
    check(cl["segment"] > segment and cl["next_seq"] == 0, f"clear starts a new segment ({cl})")
    try:
        emu.call("trace_read", {"channel": "bplog", "cursor": cursor})
    except TestFailure as e:
        check("segment_replaced:" in str(e), f"old cursor after clear: segment_replaced ({e})")
        return
    raise TestFailure("old cursor after clear was accepted")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_bplog_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            empty_cursor = case_empty(emu)
            case_log_stop(emu, empty_cursor)
            cursor, segment = case_overflow(emu)
            case_clear(emu, cursor, segment)
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
