#!/usr/bin/env python3
"""E2E test: replay_goto - stav stroje v libovolném čase přehrávání (pipe).

Záznam vstupů programu s přerušením a blokovou instrukcí, při přehrávání
trace cputrack v3 úseku, pak ``replay_goto`` na záznamy trace. Ověřuje:

  1  mimo přehrávání chyba ``only during playback``,
  2  ``trace`` {seq, at}: u pseudonáhodných záznamů (pevné semínko), u
     přijetí přerušení a u sbalené smyčky LDIR dojede na cíl a registry ze
     záznamu = dosažený stav (``trace_check.match``); záznam přijetí
     přerušení jako cíl = stav po přijetí (``after_int_ack``), jinak
     ``exact`` (začátek instrukce přesně v čase záznamu),
  3  ``t`` a ``frame`` + ``frame_ticks`` stejného času dojedou stejně,
  4  uživatelský breakpoint: výchozí ``suppress`` goto nezastaví a akce
     ``log`` nezapíše řádek do ``bplog``; s ``keep`` akce ``log`` zapisuje
     a breakpoint bez akce goto zastaví (``stopped_by`` breakpoint),
  5  ``checkpoint``: uloží checkpoint na hranici snímku přiblížení, novější
     zůstanou; další goto zpět do okolí vyjde z něj,
  6  chyby: čas před startem a za koncem záznamu, dvě adresy naráz,
     záznam mimo segment trace,
  7  přehrávání se nerozjelo.

Program (MZ-800): trvalé INT z CTC2 v IM 2 (vzor test_trace_read.py),
smyčka ``LD HL,4000h; LD DE,4100h; LD BC,10h; LDIR; EI; HALT; JR``,
obsluha ``INC D; RETI`` (bez EI, přerušení povolí až smyčka).

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import random
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402
from test_replay import _ini  # noqa: E402

PROG = 0x3000
#: DI; IM 2; I = 32h; CT1 režim 2 N = 2, CT2 režim 0 N = 1 (trvalé INT,
#: viz test_trace_read.py); DJNZ; pak smyčka od 3031h:
#: LD HL,4000h; LD DE,4100h; LD BC,10h; LDIR; EI; HALT; JR 3031h.
PROG_HEX = ("F3" "ED5E" "3E32" "ED47"
            "3E04" "3203E0"
            "3E74" "3207E0" "3E02" "3205E0" "AF" "3205E0"
            "3EB0" "3207E0" "3E01" "3206E0" "AF" "3206E0"
            "3E05" "3203E0"
            "0600" "10FE"
            "210040" "110041" "011000" "EDB0" "FB" "76" "18F1")
#: Začátek smyčky.
LOOP = 0x3031
#: Obsluha na 3434h: INC D; RETI. Tabulka IM 2 3200h-3300h -> 3434h.
ISR_HEX = "14" "ED4D"
#: Interval checkpointů a hashů, délka záznamu.
CKPT = 50
FRAMES = 200
WAIT_MS = 20000


def wait(emu, reason):
    """Počká na zastavení s daným důvodem."""
    w = emu.call("wait_stop", {"timeout_ms": WAIT_MS}, timeout=WAIT_MS / 1000 + 10)
    rec = w.get("record") or {}
    check(w.get("done") is True and rec.get("reason") == reason,
          f"stopped with reason {reason} ({rec.get('reason')})")


def call_err(emu, cmd, data):
    """Příkaz musí selhat; vrátí text chyby."""
    emu.rid += 1
    req = {"type": "request", "req_id": emu.rid, "cmd": cmd, "data": data}
    emu.proc.stdin.write(json.dumps(req) + "\n")
    emu.proc.stdin.flush()
    resp = emu._read(40, lambda m: m.get("type") == "response" and m.get("req_id") == emu.rid)
    check(resp is not None and resp.get("success") is False,
          f"{cmd} {data} fails ({resp.get('error') if resp else 'no response'})")
    return resp.get("error", "")


def goto(emu, data):
    """replay_goto s dlouhým timeoutem odpovědi."""
    return emu.call("replay_goto", data, timeout=60)


def all_kinds(emu):
    """Druh všech záznamů segmentu cputrack (index = seq).

    Stránku omezuje i rozpočet znaků, proto se čte až do `next_seq`.
    """
    n = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]["next_seq"]
    kinds = []
    while len(kinds) < n:
        p = emu.call("trace_read", {"channel": "cputrack", "from": f"seq:{len(kinds)}", "limit": 5000,
                                    "columns": ["seq", "kind"]})
        check(p["rows"] and p["rows"][0][0] == len(kinds), "trace pages follow each other")
        kinds += [r[1] for r in p["rows"]]
    return kinds


def run(tmp):
    """Scénáře 1-7."""
    rec = tmp / "goto.mzr"
    cfg, ini = _ini(tmp, "proc")
    emu = PipeEmu(_find_exe(), cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        emu.call("mem_write", {"addr": 0x3200, "data_hex": "34" * 257})
        emu.call("mem_write", {"addr": 0x3434, "data_hex": ISR_HEX})
        emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
        emu.call("set_register", {"reg": "PC", "value": PROG})
        emu.call("run", {"frames": 2})

        # 1: mimo přehrávání
        e = call_err(emu, "replay_goto", {"t": 1000})
        check("only during playback" in e, f"1: goto without playback ({e})")

        st = emu.call("replay_record_start", {"path": str(rec), "hash_interval": 10,
                                              "checkpoint_interval": CKPT, "checkpoint_count": 10})
        check(st.get("mode") == "recording", "recording started")
        emu.call("run", {"frames": FRAMES}, timeout=120)
        st = emu.call("replay_record_stop")
        check(st.get("outcome") == "recorded", f"recording saved ({st.get('outcome_message')})")

        st = emu.call("replay_play", {"path": str(rec), "pause_after_load": True,
                                      "checkpoint_interval": CKPT})
        check(st.get("mode") == "playing", f"playback loaded ({st.get('frames')} frames)")
        emu.call("replay_seek", {"frame": 100})
        wait(emu, "replay_seek")
        emu.call("trace_start", {"channel": "cputrack", "regs": True})
        emu.call("run", {"frames": 20})
        wait(emu, "frames")
        emu.call("trace_stop", {"channel": "cputrack"})
        kinds = all_kinds(emu)
        n = len(kinds)
        check(n > 1000, f"trace has records ({n})")

        # 2: cíle podle trace
        rnd = random.Random(17)
        ints = [i for i in range(1, n - 1) if kinds[i] == "int"]
        loops = [i for i in range(1, n - 1) if kinds[i] == "loop"]
        check(len(ints) >= 5 and len(loops) >= 3,
              f"trace has interrupt acknowledges ({len(ints)}) and collapsed loops ({len(loops)})")
        cases = [(0, "before"), (0, "after"), (n - 1, "before"), (n - 1, "after")]
        cases += [(rnd.randrange(1, n - 1), rnd.choice(("before", "after"))) for _ in range(24)]
        cases += [(s, "before") for s in rnd.sample(ints, 4)]
        cases += [(s - 1, "after") for s in rnd.sample(ints, 4)]
        cases += [(s, at) for s in rnd.sample(loops, 3) for at in ("before", "after")]
        n_ack = 0
        for s, at in cases:
            r = goto(emu, {"trace": {"seq": s, "at": at}})
            target_rec = s if at == "before" else s + 1
            is_ack = target_rec < n and kinds[target_rec] == "int"
            n_ack += is_ack
            ok = (r["done"] and r["reached"] and r["trace_check"]["match"]
                  and r["after_int_ack"] == is_ack and (r["exact"] or is_ack))
            check(ok, f"2: goto trace seq {s} {at} ({kinds[s]}): reached, registers match"
                      f"{', after interrupt acknowledge' if is_ack else ', exact'} "
                      f"(t {r.get('t')} target {r.get('target_t')} by {r.get('stopped_by')} "
                      f"check {r.get('trace_check')})")
        check(n_ack >= 8, f"2: interrupt acknowledge targets tested ({n_ack})")

        # 3: t a frame + frame_ticks
        o = emu.call("state_observe", {"windows": []})["time"]
        per = (o["ticks"] - o["frame_ticks"]) // o["frame"]
        tq = o["ticks"] - 3 * per + 12345
        a = goto(emu, {"t": tq})
        b = goto(emu, {"frame": tq // per, "frame_ticks": tq % per})
        check(a["reached"] and b["reached"] and a["t"] == b["t"] and a["t"] >= tq
              and a["observe"]["regs"] == b["observe"]["regs"],
              f"3: t and frame + frame_ticks reach the same point ({a['t']} / {b['t']})")

        # 4: breakpointy - akce log (nezastavuje) a breakpoint bez akce (zastaví)
        bp = emu.call("bp_create_with_init", {"fields": ["type", "addr", "action", "enabled"],
                                              "type": "PC_EXEC", "addr": LOOP,
                                              "action": 'log "loop"', "enabled": True})["id"]
        emu.call("bplog_clear")
        r = goto(emu, {"t": tq - 2 * per})
        rows = emu.call("trace_read", {"channel": "bplog", "from": "oldest", "limit": 10})["rows"]
        check(r["reached"] and not rows, f"4: suppress: reached, no bplog line ({len(rows)})")
        r = goto(emu, {"t": tq + 2 * per, "breakpoints": "keep"})
        rows = emu.call("trace_read", {"channel": "bplog", "from": "oldest", "limit": 10})["rows"]
        check(r["reached"] and rows, f"4: keep: the log action writes bplog lines ({len(rows)})")
        emu.call("bp_remove", {"id": bp})
        bp = emu.call("bp_create_with_init", {"fields": ["type", "addr", "enabled"],
                                              "type": "PC_EXEC", "addr": LOOP, "enabled": True})["id"]
        r = goto(emu, {"t": tq - per, "breakpoints": "keep"})
        check(r["done"] and not r["reached"] and r["stopped_by"] == "breakpoint"
              and r["observe"]["regs"]["PC"] == LOOP,
              f"4: keep: a stopping breakpoint stops the goto ({r.get('stopped_by')})")
        r = goto(emu, {"t": tq})
        check(r["reached"], f"4: suppress: the stopping breakpoint does not stop the goto ({r.get('stopped_by')})")
        emu.call("bp_remove", {"id": bp})

        # 5: checkpoint v cíli. Nejdřív dojet za snímek 150 (checkpointy 0,
        # 50, 100, 150), pak goto zpět do snímku 125 s checkpointem: vloží se
        # mezi 100 a 150, novější zůstanou. S0 = absolutní snímek startu.
        s0 = emu.call("state_observe", {"windows": []})["time"]["frame"] - emu.call("replay_status")["frame"]
        r = goto(emu, {"t": (s0 + 160) * per + 20000})
        check(r["reached"], "5: goto past frame 150")
        ck0 = emu.call("replay_status")["checkpoints"]
        t_c = (s0 + 125) * per + 20000
        r = goto(emu, {"t": t_c, "checkpoint": True})
        ck1 = emu.call("replay_status")["checkpoints"]
        check(r["reached"] and r["checkpoint_saved"] and ck1["count"] == ck0["count"] + 1
              and ck1["last_frame"] == ck0["last_frame"] == 150,
              f"5: checkpoint saved at frame 125, newer kept ({ck0} -> {ck1})")
        r = goto(emu, {"t": t_c - 1000})
        check(r["reached"] and r["seek_from"] == "checkpoint" and r["seek_from_frame"] == 125,
              f"5: the next goto back starts from it ({r.get('seek_from')} {r.get('seek_from_frame')})")

        # 6: chyby
        e = call_err(emu, "replay_goto", {"t": 0})
        check("before the start" in e, f"6: before the start ({e})")
        e = call_err(emu, "replay_goto", {"t": o["ticks"] + 10000 * per})
        check("after the end" in e, f"6: after the end ({e})")
        e = call_err(emu, "replay_goto", {"t": 1, "frame": 1})
        check("exactly one" in e, f"6: two targets ({e})")
        e = call_err(emu, "replay_goto", {"trace": {"seq": n + 100}})
        check("not in the current" in e, f"6: record outside the segment ({e})")

        # 7
        st = emu.call("replay_status")
        check(st.get("mode") == "playing" and st.get("deviated") is False,
              f"7: playback still in sync ({st.get('outcome')}, {st.get('outcome_message')!r})")
        check(st.get("goto_active") is False and st.get("goto_target_t") == t_c - 1000,
              f"7: replay_status reports the last goto ({st.get('goto_active')}, {st.get('goto_target_t')})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=230)
    print(f"Using binary: {_find_exe()}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_replay_goto_"))
    ok = False
    try:
        run(tmp)
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
