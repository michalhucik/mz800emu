#!/usr/bin/env python3
"""E2E test: záznam a přehrání vstupů (replay_*, --replay, --replay-verify).

Ověřuje:

  - záznam v ROM monitoru (MZ-800): klávesy přes HID skript, zápis paměti
    v pauze na hranici snímku (poloha ``p``), za běhu (poloha ``d``)
    a po MCP ``pause`` za běhu (pauza na hranici snímku, opět ``p``),
  - při záznamu se zápis v pauze mimo hranici snímku (po kroku) a načtení
    snapshotu odmítnou s vysvětlením,
  - přehrání v novém procesu na MAX SPEED skončí s důvodem ``replay_end``
    (``wait_stop``) a stav (registry, celá RAM, VRAM) je shodný se
    stavem na konci záznamu,
  - ``--replay ... --replay-verify`` skončí kódem 0 a ``result.json``
    s ``reason: "replay_end"``,
  - záznam s odebraným zápisem v ``inputs.jsonl``: ``--replay-verify``
    skončí kódem 4 (``replay_desync``) na prvním hashi po odebrané události,
  - zápis do paměti během přehrávání přehrávání označí jako ``deviated``,
    přehrávání doběhne do konce.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Klávesy pro ROM monitor (prompt "*"): stisk a mezera po 3 snímcích.
KEYS = [{"key": k, "hold_frames": 3, "gap_frames": 3} for k in ["M", "A", "B", "RETURN"]]
#: Interval hashů záznamu (snímky).
HASH_INTERVAL = 10
#: Adresy zapisované debuggerem (RAM nad ROM monitorem, mimo zásobník).
ADDR_P = 0x2000
ADDR_D = 0x2010
#: Nejdelší čekání na konec přehrávání [ms].
WAIT_MS = 20000


def _ini(tmp, name):
    """Vlastní INI a cfg adresář pro jeden proces."""
    cfg = tmp / name
    cfg.mkdir()
    ini = cfg / "test.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
    return cfg, ini


def _state(emu):
    """Stav pro porovnání: registry, SHA-256 paměti (logicky, po 16 KB) a VRAM."""
    regs = emu.call("get_registers")
    ram = [emu.call("mem_read", {"addr": a, "len": 0x4000, "want_data": False}).get("sha256")
           for a in range(0, 0x10000, 0x4000)]
    fh = emu.call("frame_hash")
    return {"regs": regs, "ram": ram, "vram": fh.get("vram_sha256")}


def _expect_error(emu, cmd, data, needle):
    """Příkaz musí selhat a chyba obsahovat needle."""
    try:
        emu.call(cmd, data)
    except TestFailure as e:
        check(needle in str(e), f"{cmd} refused: ...{needle}... ({str(e)[:160]})")
        return
    raise TestFailure(f"{cmd} was accepted while recording")


def record(exe, tmp, rec):
    """Fáze 1: záznam přes MCP pipe, vrátí stav na konci."""
    print("Phase 1: recording")
    cfg, ini = _ini(tmp, "rec")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        emu.call("reset")
        emu.call("run", {"frames": 150})
        st = emu.call("replay_record_start", {"path": str(rec), "hash_interval": HASH_INTERVAL,
                                              "description": "e2e"})
        check(st.get("mode") == "recording" and st.get("frame") == 0, f"recording started ({st})")
        emu.call("input_send_keys_with_delays", {"events": KEYS})
        emu.call("mem_write", {"addr": ADDR_P, "data_hex": "112233"})
        emu.call("run")
        emu.call("mem_write", {"addr": ADDR_D, "data_hex": "44"})
        emu.call("pause")
        # MCP pause za běhu zastaví na hranici snímku: zápis se přijme (p).
        emu.call("mem_write", {"addr": ADDR_D + 8, "data_hex": "66"})
        emu.call("run", {"frames": 30})
        emu.call("step_into")
        _expect_error(emu, "mem_write", {"addr": 0x2020, "data_hex": "55"}, "frame boundar")
        _expect_error(emu, "snapshot_load", {"path": str(rec)}, "not allowed while recording")
        emu.call("run", {"frames": 20})
        st = emu.call("replay_record_stop")
        check(st.get("outcome") == "recorded" and st.get("mode") == "off", f"recording saved ({st.get('outcome_message')})")
        return _state(emu)
    finally:
        emu.close()


def inspect(rec):
    """Události a hashe ze souboru; vrátí (manifest, události, snímky hashů)."""
    with zipfile.ZipFile(rec) as z:
        manifest = json.loads(z.read("manifest.json"))
        events = [json.loads(l) for l in z.read("inputs.jsonl").decode().splitlines() if l.strip()]
        rows = [l.split(",") for l in z.read("hashes.csv").decode().splitlines()[1:] if l.strip()]
    check(manifest.get("format") == "mz800emu-replay" and manifest.get("has_debugger_events") is True
          and manifest.get("final_hash") is True, f"manifest ({manifest.get('frames')} frames)")
    dbg = [e for e in events if e.get("k") == "dbg"]
    check([e["at"] for e in dbg] == ["p", "d", "p"], f"debugger writes recorded at p, d, p ({[e['at'] for e in dbg]})")
    check(any(e.get("k") == "kbd" and e["f"] > 0 for e in events), "keyboard changes recorded")
    return manifest, events, [int(r[0]) for r in rows]


def play_pipe(exe, tmp, rec, want):
    """Fáze 2: přehrání v novém procesu na MAX SPEED, stav = záznam."""
    print("Phase 2: playback in a new process")
    cfg, ini = _ini(tmp, "play")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        # Přehrání je další běh po pauze; wait_stop bez run_seq by před jeho
        # rozběhem vrátil zastavení z pause (N108).
        play_run = emu.call("get_state")["run_seq"] + 1
        st = emu.call("replay_play", {"path": str(rec)})
        check(st.get("mode") == "playing", f"playback started ({st.get('frames')} frames)")
        w = emu.call("wait_stop", {"run_seq": play_run, "timeout_ms": WAIT_MS},
                     timeout=WAIT_MS / 1000 + 10)
        rec_ = w.get("record") or {}
        check(w.get("done") is True and rec_.get("reason") == "replay_end", f"wait_stop reason replay_end ({rec_.get('reason')})")
        st = emu.call("replay_status")
        check(st.get("outcome") == "ended" and st.get("deviated") is False, f"outcome ended ({st.get('outcome_message')})")
        got = _state(emu)
        check(got == want, "state after playback equals the state at the end of the recording")
    finally:
        emu.close()


def cli(exe, tmp, name, args, timeout=60):
    """Headless běh do konce procesu; vrátí (kód, result.json)."""
    d = tmp / name
    d.mkdir()
    cfg, ini = _ini(d, "cfg")
    res = d / "result.json"
    cmd = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows", f"--cfg-dir={cfg}",
           f"--work-dir={d}", f"--config={ini}", f"--result-json={res}"] + [str(a) for a in args]
    with open(d / "emu.log", "w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=str(d))
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            emu_test_proc.kill_proc(proc)
            raise TestFailure(f"{name}: emulator did not exit within {timeout} s")
    result = json.loads(res.read_text(encoding="utf-8")) if res.is_file() else None
    return code, result


def tamper(rec, out, drop):
    """Kopie záznamu bez událostí, pro které drop(event) vrátí True."""
    with zipfile.ZipFile(rec) as zi, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zo:
        for it in zi.infolist():
            data = zi.read(it.filename)
            if it.filename == "inputs.jsonl":
                lines = [l for l in data.decode().splitlines() if l.strip() and not drop(json.loads(l))]
                data = ("\n".join(lines) + "\n").encode()
            zo.writestr(it.filename, data)


def verify(exe, tmp, rec, events, hash_frames):
    """Fáze 3: --replay-verify bez změny (kód 0) a s odebraným zápisem (kód 4)."""
    print("Phase 3: --replay-verify")
    code, res = cli(exe, tmp, "verify", ["--replay", rec, "--replay-verify"])
    check(code == 0 and res and res.get("reason") == "replay_end", f"verify: exit code 0, replay_end ({code})")
    check(res["replay"]["outcome"] == "ended" and res["trigger"].get("option") == "--replay-verify",
          f"result.json replay object ({res['replay']})")

    ev = next(e for e in events if e.get("k") == "dbg" and e["at"] == "p")
    bad = tmp / "bad.mzr"
    tamper(rec, bad, lambda e: e.get("k") == "dbg" and e["at"] == "p")
    # Událost p se provede až po hashi svého snímku: první hash, který ji vidí,
    # je na dalším snímku s hashem.
    want_frame = min(f for f in hash_frames if f > ev["f"])
    code, res = cli(exe, tmp, "tampered", ["--replay", bad, "--replay-verify"])
    check(code == 4 and res and res.get("reason") == "replay_desync", f"tampered: exit code 4, replay_desync ({code})")
    check(res["replay"]["outcome_frame"] == want_frame and res["replay"]["desync_parts"],
          f"desync at frame {want_frame} ({res['replay']['outcome_frame']}, {res['replay']['desync_parts']})")


def deviation(exe, tmp, rec):
    """Fáze 4: zápis během přehrávání = odchýleno, přehrávání doběhne."""
    print("Phase 4: write during playback")
    cfg, ini = _ini(tmp, "dev")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        st = emu.call("replay_play", {"path": str(rec), "pause_after_load": True})
        check(st.get("mode") == "playing", "playback loaded and paused")
        emu.call("run", {"frames": 5})
        emu.call("mem_write", {"addr": 0x2030, "data_hex": "77"})
        st = emu.call("replay_status")
        check(st.get("deviated") is True and "mem_write" in st.get("deviation", ""), f"deviated ({st.get('deviation')})")
        r = emu.call("run")
        w = emu.call("wait_stop", {"run_seq": r["run_seq"], "timeout_ms": WAIT_MS},
                     timeout=WAIT_MS / 1000 + 10)
        check((w.get("record") or {}).get("reason") == "replay_end", "deviated playback still ends")
        st = emu.call("replay_status")
        check(st.get("outcome") == "ended", f"outcome ended ({st.get('outcome_message')})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_replay_"))
    ok = False
    try:
        rec = tmp / "run.mzr"
        want = record(exe, tmp, rec)
        _manifest, events, hash_frames = inspect(rec)
        play_pipe(exe, tmp, rec, want)
        verify(exe, tmp, rec, events, hash_frames)
        deviation(exe, tmp, rec)
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
