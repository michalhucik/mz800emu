#!/usr/bin/env python3
"""E2E test: seek zpět přes checkpoint se zvukovým čipem v běhu (MCP pipe).

PSG se krokuje po PSG_DIVIDER ticích GDG, líně (při zápisu do PSG a na
konci snímku). Dřív načtení snapshotu (checkpoint při seeku zpět) ukotvilo
mřížku kroků PSG na okamžik načtení a neodkrokovaná část periody se ztratila:
čítače tónových kanálů se rozešly se záznamem a přehrání hlásilo
``state differs at frame N (periph)`` (SAPO-P, seek 900 -> 300). Teď snapshot
ukládá fázi krokování PSG (``hw/audio.xml``).

Program v RAM nastaví tři tónové kanály PSG (děliče 3FFh, 2A5h, 1F3h a útlum 0;
ztlumený kanál nekrokuje) a točí se ve smyčce. Záznam 300 snímků s checkpointy po 50 a
hashi po 10, pak přehrání: seek 280, seek zpět 130 (načte checkpoint 100),
seek 160, dohrání do konce. Ověřuje, že se přehrání nikde nerozjede a že
opětovný průchod checkpointem 150 nesmaže novější checkpointy 200 a 250
(dřív je uložení checkpointu staršího snímku smazalo).

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402
from test_replay import _ini  # noqa: E402

#: 2000h: DI; tóny a hlasitost PSG kanálů 0-2 přes OUT (0F2h); smyčka INC HL; JR.
PROG = ("F3"
        "3E8FD3F23E3FD3F2"      # kanál 0: dělič 3FFh
        "3EA5D3F23E2AD3F2"      # kanál 1: dělič 2A5h
        "3EC3D3F23E1FD3F2"      # kanál 2: dělič 1F3h
        "3E90D3F23EB0D3F23ED0D3F2"  # útlum 0 kanálů 0-2 (ztlumený kanál PSG nekrokuje)
        "2318FD")               # INC HL; JR $-1
#: Interval checkpointů a hashů (snímky), délka záznamu.
CKPT = 50
HASH_INTERVAL = 10
FRAMES = 300
#: Nejdelší čekání na zastavení [ms].
WAIT_MS = 20000


def _wait(emu, reason, run_seq):
    """Počká na konec běhu `run_seq` a ověří důvod zastavení.

    Bez `run_seq` by wait_stop v pauze před rozběhem seeku vrátil
    předchozí zastavení (N108).
    """
    check(run_seq is not None, f"run_seq to wait for {reason}")
    w = emu.call("wait_stop", {"run_seq": run_seq, "timeout_ms": WAIT_MS},
                 timeout=WAIT_MS / 1000 + 10)
    rec = w.get("record") or {}
    check(w.get("done") is True and rec.get("reason") == reason,
          f"stopped with reason {reason} ({rec.get('reason')})")


def _no_desync(emu, what):
    """Přehrání běží bez rozjetí."""
    st = emu.call("replay_status")
    check(st.get("mode") == "playing" and st.get("deviated") is False
          and st.get("outcome") != "desync",
          f"{what}: no desync (frame {st.get('frame')}, outcome {st.get('outcome')}, "
          f"{st.get('outcome_message')!r})")


def run(tmp):
    """Záznam s PSG v běhu, pak seek vpřed, zpět přes checkpoint a dohrání."""
    rec = tmp / "psg.mzr"
    cfg, ini = _ini(tmp, "proc")
    emu = PipeEmu(_find_exe(), cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        emu.call("mem_write", {"addr": 0x2000, "data_hex": PROG})
        emu.call("set_register", {"reg": "PC", "value": 0x2000})
        emu.call("run", {"frames": 5})
        st = emu.call("replay_record_start", {"path": str(rec), "hash_interval": HASH_INTERVAL,
                                              "checkpoint_interval": CKPT, "checkpoint_count": 10})
        check(st.get("mode") == "recording", "recording started")
        emu.call("run", {"frames": FRAMES}, timeout=120)
        st = emu.call("replay_record_stop")
        check(st.get("outcome") == "recorded", f"recording saved ({st.get('outcome_message')})")

        st = emu.call("replay_play", {"path": str(rec), "pause_after_load": True,
                                      "checkpoint_interval": CKPT})
        check(st.get("mode") == "playing", f"playback loaded ({st.get('frames')} frames)")
        st = emu.call("replay_seek", {"frame": 280})
        _wait(emu, "replay_seek", st.get("run_seq"))
        _no_desync(emu, "seek forward to 280")
        ck_before = emu.call("replay_status")["checkpoints"]
        st = emu.call("replay_seek", {"frame": 130})
        _wait(emu, "replay_seek", st.get("run_seq"))
        _no_desync(emu, "seek back to 130 from checkpoint 100")
        # Opětovný průchod checkpointem 150 nesmí smazat novější 200 a 250
        # (přehrávání je deterministické).
        st = emu.call("replay_seek", {"frame": 160})
        _wait(emu, "replay_seek", st.get("run_seq"))
        _no_desync(emu, "seek forward to 160 again")
        ck_after = emu.call("replay_status")["checkpoints"]
        check(ck_after.get("count") == ck_before.get("count")
              and ck_after.get("last_frame") == ck_before.get("last_frame"),
              f"checkpoints kept after passing them again ({ck_before} -> {ck_after})")

        r = emu.call("run")
        _wait(emu, "replay_end", r.get("run_seq"))
        st = emu.call("replay_status")
        check(st.get("outcome") == "ended" and st.get("deviated") is False,
              f"playback ends without desync ({st.get('outcome_message')})")
    finally:
        emu.close()


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=150)
    print(f"Using binary: {_find_exe()}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_replay_seek_psg_"))
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
