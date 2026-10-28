#!/usr/bin/env python3
"""E2E test: média v záznamu vstupů (kazeta a disk, replay_*, --replay-verify).

Ověřuje:

  - záznam, který začne uprostřed bloku hrající kazety (MZ-800 boot z CMT),
    obsahuje pásku (``media`` slot ``cmt``, vložený obsah), aktuální blok
    a nastavení kazety (``cmt`` v manifestu) a DSK obraz v mechanice
    (slot ``fdc0.0``),
  - ovládání kazety přes MCP během záznamu (``cmt_transport`` pause / play)
    se zapíše jako události ``cmt`` (ne ``dbg``),
  - ``--replay ... --replay-verify`` v novém procesu bez vložené pásky
    a bez disku skončí kódem 0: přehrání pásku i disk nasadí z kopií,
    kazeta pokračuje ve stejném bloku a stavu,
  - záznam bez pásky (upravený manifest a archiv) se při přehrání rozjede
    (kód 4): obsah pásky do záznamu opravdu patří,
  - přehrání v pipe režimu nezmění originální soubory pásky a disku.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import hashlib
import json
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Délka těla MZF (bajty): páska hraje po celý záznam.
MZF_BODY = 8192
#: Interval hashů záznamu (snímky).
HASH_INTERVAL = 25
#: Snímky záznamu po pauze pásky (asi 30 s emulace: ROM už čte hlavičku a tělo).
LOAD_FRAMES = 1000
#: Nejdelší čekání na konec přehrávání [ms].
WAIT_MS = 20000


def _ini(tmp, name):
    """Vlastní INI a cfg adresář pro jeden proces (CMT hack vypnutý)."""
    cfg = tmp / name
    cfg.mkdir()
    ini = cfg / "test.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n[CMTHACK]\nenable = 0\n", encoding="utf-8")
    return cfg, ini


def make_mzf(path):
    """MZF (atribut 01h, 1200h, tělo MZF_BODY bajtů)."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"REPLAY TAPE\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = MZF_BODY.to_bytes(2, "little")
    hdr[0x14:0x16] = (0x1200).to_bytes(2, "little")
    hdr[0x16:0x18] = (0x1200).to_bytes(2, "little")
    body = bytes(((i * 53) + 7) & 0xFF for i in range(MZF_BODY))
    path.write_bytes(bytes(hdr) + body)


def make_dsk(path, tracks=2, sectors=16):
    """Extended CPC DSK: 1 strana, `tracks` stop po `sectors` x 256 B (vyplněno E5h)."""
    track_size = 256 + sectors * 256
    info = bytearray(256)
    magic = b"EXTENDED CPC DSK File\r\nDisk-Info\r\n"
    info[0:len(magic)] = magic
    info[0x22:0x22 + 14] = b"mz800emu test "
    info[0x30] = tracks
    info[0x31] = 1
    for t in range(tracks):
        info[0x34 + t] = track_size // 256
    out = bytearray(info)
    for t in range(tracks):
        ti = bytearray(256)
        ti[0:12] = b"Track-Info\r\n"
        ti[0x10] = t
        ti[0x11] = 0
        ti[0x14] = 1          # 256 B sektory
        ti[0x15] = sectors
        ti[0x16] = 0x4E
        ti[0x17] = 0xE5
        for s in range(sectors):
            o = 0x18 + s * 8
            ti[o:o + 8] = bytes([t, 0, s + 1, 1, 0, 0]) + (256).to_bytes(2, "little")
        out += ti + bytes([0xE5]) * (sectors * 256)
    path.write_bytes(bytes(out))


def sha(path):
    """SHA-256 souboru (hex)."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def record(exe, tmp, rec, mzf, dsk):
    """Záznam přes MCP pipe: start uprostřed hrající pásky, pauza a play pásky."""
    print("Phase 1: recording with a playing tape and a disk")
    cfg, ini = _ini(tmp, "rec")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        emu.call("reset")
        emu.call("run", {"frames": 350})
        emu.call("cmt_open", {"path": str(mzf), "play_immediately": False})
        emu.call("input_send_key", {"key": "C", "frames": 5})
        emu.call("cmt_transport", {"action": "play"})
        emu.call("run", {"frames": 120})
        # Disk až po volbě C v boot menu (s disketou by ROM bootovala z ní).
        emu.call("media_insert", {"slot": "fdc0_fd0", "path": str(dsk)})
        st = emu.call("replay_record_start", {"path": str(rec), "hash_interval": HASH_INTERVAL})
        check(st.get("mode") == "recording", f"recording started with the tape playing ({st})")
        emu.call("run", {"frames": 100})
        emu.call("cmt_transport", {"action": "pause", "pause": 1})
        emu.call("run", {"frames": 40})
        emu.call("cmt_transport", {"action": "pause", "pause": 0})
        # Dost dlouho, aby ROM po úvodním tónu načítala hlavičku a tělo do RAM
        # (bez pásky by se pak přehrání rozjelo).
        for _ in range(LOAD_FRAMES // 500):
            emu.call("run", {"frames": 500})
        st = emu.call("replay_record_stop")
        check(st.get("outcome") == "recorded", f"recording saved ({st.get('outcome_message')})")
    finally:
        emu.close()


def inspect(rec, mzf, dsk):
    """Média a události v souboru."""
    with zipfile.ZipFile(rec) as z:
        manifest = json.loads(z.read("manifest.json"))
        names = z.namelist()
        events = [json.loads(l) for l in z.read("inputs.jsonl").decode().splitlines() if l.strip()]
        media = {m["slot"]: m for m in manifest.get("media", [])}
        check("cmt" in media and media["cmt"]["embedded"] and media["cmt"]["sha256"] == sha(mzf)
              and "media/cmt.bin" in names, f"tape stored in the replay ({sorted(media)})")
        check("fdc0.0" in media and media["fdc0.0"]["embedded"] and media["fdc0.0"]["sha256"] == sha(dsk)
              and "media/fdc0.0.bin" in names, "disk image stored in the replay")
    cmt = manifest.get("cmt") or {}
    check(cmt.get("hack") is False and cmt.get("block", -1) >= 0, f"tape state in the manifest ({cmt})")
    ops = [(e.get("op"), e.get("v")) for e in events if e.get("k") == "cmt"]
    check(ops == [("pause", 1), ("pause", 0)], f"tape control recorded as cmt events ({ops})")
    check(not any(e.get("k") == "dbg" for e in events), "no dbg events for tape control")
    return manifest


def cli(exe, tmp, name, args, timeout=90):
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


def without_tape(rec, out):
    """Kopie záznamu bez pásky (manifest bez slotu cmt, bez media/cmt.bin)."""
    with zipfile.ZipFile(rec) as zi, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zo:
        for it in zi.infolist():
            if it.filename == "media/cmt.bin":
                continue
            data = zi.read(it.filename)
            if it.filename == "manifest.json":
                m = json.loads(data)
                m["media"] = [x for x in m["media"] if x["slot"] != "cmt"]
                data = json.dumps(m).encode()
            zo.writestr(it.filename, data)


def verify(exe, tmp, rec):
    """--replay-verify v novém procesu bez pásky a disku; bez pásky v záznamu = rozjetí."""
    print("Phase 2: --replay-verify in a new process without tape and disk")
    code, res = cli(exe, tmp, "verify", ["--replay", rec, "--replay-verify"])
    check(code == 0 and res and res.get("reason") == "replay_end",
          f"verify: exit code 0, replay_end ({code}, {res and res.get('replay')})")

    print("Phase 3: replay without the tape must differ")
    bad = tmp / "notape.mzr"
    without_tape(rec, bad)
    code, res = cli(exe, tmp, "notape", ["--replay", bad, "--replay-verify"])
    check(code == 4 and res and res.get("reason") == "replay_desync",
          f"replay without the tape: exit code 4 ({code}, {res and res.get('replay')})")


def play_pipe(exe, tmp, rec, mzf, dsk):
    """Přehrání v pipe režimu: originály se nezmění, po konci zpátky podle INI."""
    print("Phase 4: playback leaves the original files untouched")
    before = (sha(mzf), sha(dsk))
    cfg, ini = _ini(tmp, "play")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        # Přehrání je další běh po pauze; wait_stop bez run_seq by před jeho
        # rozběhem vrátil zastavení z pause (N108).
        play_run = emu.call("get_state")["run_seq"] + 1
        st = emu.call("replay_play", {"path": str(rec)})
        check(st.get("mode") == "playing", "playback started")
        w = emu.call("wait_stop", {"run_seq": play_run, "timeout_ms": WAIT_MS},
                     timeout=WAIT_MS / 1000 + 10)
        check((w.get("record") or {}).get("reason") == "replay_end",
              f"playback ended ({(w.get('record') or {}).get('reason')})")
        st = emu.call("replay_status")
        check(st.get("outcome") == "ended" and st.get("deviated") is False, f"outcome ended ({st.get('outcome_message')})")
    finally:
        emu.close()
    check((sha(mzf), sha(dsk)) == before, "original tape and disk files are unchanged")


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_replay_media_"))
    ok = False
    try:
        mzf = tmp / "tape.mzf"
        dsk = tmp / "disk.dsk"
        make_mzf(mzf)
        make_dsk(dsk)
        rec = tmp / "media.mzr"
        record(exe, tmp, rec, mzf, dsk)
        inspect(rec, mzf, dsk)
        verify(exe, tmp, rec)
        play_pipe(exe, tmp, rec, mzf, dsk)
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
