#!/usr/bin/env python3
"""E2E test: checkpointy záznamu vstupů - rewind při záznamu, seek při přehrávání.

Ověřuje:

  - záznam v ROM monitoru (MZ-800) s checkpointy každých 50 snímků;
    ``emu_replay_rewind`` vrátí stroj na poslední checkpoint (zápis paměti
    po něm zmizí), záznam pokračuje jinou cestou a uložený soubor má
    ``rerecord_count`` 1 a jen zápisy nové cesty,
  - ``--replay ... --replay-verify`` takového záznamu v novém procesu
    skončí kódem 0 (stav po rewindu sedí s hashi záznamu),
  - přehrávání: seek dopředu zastaví na cílovém snímku s důvodem
    ``replay_seek``, seek zpět načte checkpoint a dohraje, opakovaný seek na
    týž snímek dá stejný stav stroje (registry, RAM, VRAM), rewind při
    přehrávání skočí na předchozí checkpoint, přehrávání pak doběhne do
    konce bez rozjetí.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import json
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402
from test_replay import _ini, _state, cli  # noqa: E402

#: Klávesy pro ROM monitor (prompt "*"): stisk a mezera po 3 snímcích.
KEYS_A = [{"key": k, "hold_frames": 3, "gap_frames": 3} for k in ["M", "A"]]
KEYS_B = [{"key": k, "hold_frames": 3, "gap_frames": 3} for k in ["B", "RETURN"]]
#: Interval checkpointů a hashů (snímky).
CKPT = 50
HASH_INTERVAL = 10
#: Adresa zapisovaná debuggerem (RAM nad ROM monitorem, mimo zásobník).
ADDR = 0x2000
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
    check(w.get("done") is True and rec.get("reason") == reason, f"stopped with reason {reason} ({rec.get('reason')})")


def _byte(emu, addr):
    """Bajt paměti jako hex text (mem_read vrací data v base64)."""
    data = base64.b64decode(emu.call("mem_read", {"addr": addr, "len": 1}).get("data_b64", ""))
    check(len(data) == 1, f"mem_read {addr:04X}h returned one byte")
    return data.hex().upper()


def record(exe, tmp, rec):
    """Fáze 1: záznam s rewindem přes MCP pipe."""
    print("Phase 1: recording with a rewind")
    cfg, ini = _ini(tmp, "rec")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        emu.call("reset")
        emu.call("run", {"frames": 150})
        st = emu.call("replay_record_start", {"path": str(rec), "hash_interval": HASH_INTERVAL,
                                              "checkpoint_interval": CKPT, "checkpoint_count": 10})
        ck = st.get("checkpoints") or {}
        check(st.get("mode") == "recording" and ck.get("interval") == CKPT and ck.get("count") == 1,
              f"recording started with checkpoint 0 ({ck})")
        emu.call("input_send_keys_with_delays", {"events": KEYS_A})
        emu.call("run", {"frames": 120})
        old = _byte(emu, ADDR)
        emu.call("mem_write", {"addr": ADDR, "data_hex": "AA"})
        # Klávesy přes HID skript odehrají své snímky samy: snímek čteme ze stavu.
        st = emu.call("replay_status")
        check(100 < st.get("frame", 0) < 150 and (st.get("checkpoints") or {}).get("last_frame") == 100,
              f"past frame 100, newest checkpoint 100 ({st.get('frame')}, {st.get('checkpoints')})")

        st = emu.call("replay_rewind")
        check(st.get("frame") == 100 and st.get("rerecord_count") == 1,
              f"rewind: frame 100, rerecord_count 1 ({st.get('frame')}, {st.get('rerecord_count')})")
        check(old != "AA" and _byte(emu, ADDR) == old,
              f"write after the checkpoint is gone ({_byte(emu, ADDR)}, was {old})")

        # Jiná cesta po rewindu.
        emu.call("input_send_keys_with_delays", {"events": KEYS_B})
        emu.call("run", {"frames": 100})
        emu.call("mem_write", {"addr": ADDR, "data_hex": "BB"})
        emu.call("run", {"frames": 60})
        frames = emu.call("replay_status").get("frame")
        st = emu.call("replay_record_stop")
        check(st.get("outcome") == "recorded", f"recording saved ({st.get('outcome_message')})")
    finally:
        emu.close()

    with zipfile.ZipFile(rec) as z:
        manifest = json.loads(z.read("manifest.json"))
        events = [json.loads(l) for l in z.read("inputs.jsonl").decode().splitlines() if l.strip()]
    check(manifest.get("rerecord_count") == 1 and manifest.get("frames") == frames,
          f"manifest: rerecord_count 1, {frames} frames ({manifest.get('rerecord_count')}, {manifest.get('frames')})")
    writes = [json.dumps(e) for e in events if e.get("k") == "dbg"]
    check(len(writes) == 1 and "BB" in writes[0].upper() and "AA" not in writes[0].upper(),
          f"only the write of the new path is recorded ({writes})")


def verify(exe, tmp, rec):
    """Fáze 2: --replay-verify záznamu s rewindem v novém procesu."""
    print("Phase 2: --replay-verify after a rewind")
    code, res = cli(exe, tmp, "verify", ["--replay", rec, "--replay-verify"])
    check(code == 0 and res and res.get("reason") == "replay_end",
          f"verify: exit code 0, replay_end ({code}, {res and res.get('reason')})")


def seek(exe, tmp, rec):
    """Fáze 3: seek dopředu, zpět, znovu dopředu (stejný stav), rewind, konec."""
    print("Phase 3: seek during playback")
    cfg, ini = _ini(tmp, "seek")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        st = emu.call("replay_play", {"path": str(rec), "pause_after_load": True,
                                      "checkpoint_interval": CKPT})
        check(st.get("mode") == "playing", f"playback loaded ({st.get('frames')} frames)")

        st = emu.call("replay_seek", {"frame": 180})
        check(st.get("seeking") is True and st.get("seek_target") == 180, f"seek 180 started ({st.get('seeking')})")
        _wait(emu, "replay_seek", st.get("run_seq"))
        st = emu.call("replay_status")
        check(st.get("frame") == 180 and st.get("seeking") is False, f"at frame 180 ({st.get('frame')})")
        at180 = _state(emu)

        st = emu.call("replay_seek", {"frame": 130})
        check(st.get("seeking") is True, "seek back to 130 runs from a checkpoint")
        _wait(emu, "replay_seek", st.get("run_seq"))
        check(emu.call("replay_status").get("frame") == 130, "at frame 130")

        st = emu.call("replay_seek", {"frame": 180})
        _wait(emu, "replay_seek", st.get("run_seq"))
        check(_state(emu) == at180, "the same frame after another seek gives the same machine state")

        st = emu.call("replay_rewind")
        check(st.get("frame") == 150 and st.get("seeking") is False, f"rewind in playback: frame 150 ({st.get('frame')})")
        check("run_seq" not in st, "rewind to a checkpoint without a run has no run_seq")

        r = emu.call("run")
        _wait(emu, "replay_end", r.get("run_seq"))
        st = emu.call("replay_status")
        check(st.get("outcome") == "ended" and st.get("deviated") is False,
              f"playback ends without desync ({st.get('outcome_message')})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_replay_rw_"))
    ok = False
    try:
        rec = tmp / "rewind.mzr"
        record(exe, tmp, rec)
        verify(exe, tmp, rec)
        seek(exe, tmp, rec)
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
