#!/usr/bin/env python3
"""E2E regresní test: snapshot pořízený během přehrávání kazety (MCP pipe).

Reprodukuje chyby nahlášené 3. 10. 2026 (pád a "zamrzlá" páska po načtení
snapshotu v novém procesu) a ověřuje opravy:

  A  proces 1: boot MZ-800, vlastní vygenerované MZF (CMT hack vypnutý),
     volba C v boot menu, přehrávání, po pár snímcích ``snapshot_save``.
     Výpis pásky (``cmt_tape_list``) musí hlásit ``playable: true``,
     ``recordable: false`` (dříve obráceně).
  B  procesy bez vložené pásky: snapshot z A se ve fázi B1 načte se
     smazaným souborem pásky -> páska nevložená, transport STOP, na stderr
     varování "Snapshot tape file not found: <cesta>"; ve fázi B2 se načte
     kopie snapshotu ve starém formátu (bez elementu <tape> v devices/cmt.xml,
     bez kontrolního součtu) -> jako dřív páska nevložená a STOP. Pak ``run``
     nesmí shodit proces (dříve access violation), ``cmt_transport stop`` bez
     pásky vrací ok a stav zůstává STOP, ``play_paused`` bez pásky nenechá
     STOP s příznakem pauzy.
  C  proces 3: ``snapshot_load`` + ``cmt_open`` s ``play_immediately`` ->
     přehrávání začne od začátku (playsts BODY, start_time nový) a signál
     z pásky se během běhu mění (dříve zůstal konstantní). Odpověď
     ``cmt_open`` hlásí skutečný stav transportu (``playing``, ``state``,
     ``paused``), shodný s ``get_periph_cmt`` (dříve echo požadavku).
  D  proces 4: cpu_boost a snapshot - snapshot s hrající páskou a cpu_boost
     po načtení zapne MAX SPEED (dříve zůstala normální rychlost), snapshot
     ve STOP MAX SPEED od boostu vypne; MAX SPEED zvolenou uživatelem
     (``set_speed max``) nemění ani stop pásky, ani načtení snapshotu.
  E  procesy 5 a 6 (s ukládáním vlastního INI): snapshot neobnovuje volbu
     cpu_boost (uživatelská preference). Snapshot s hrající páskou
     a cpu_boost=1 načtený při cpu_boost=0 v INI -> volba zůstane 0, MAX
     SPEED neběží a INI po ukončení má dál cpu_boost=0; opačně snapshot
     s cpu_boost=0 při cpu_boost=1 -> volba zůstane 1 a MAX SPEED běží.
  F  snapshot pásky: MZT = LOADER + DATA, ROM nahraje LOADER (volba C)
     a LOADER přes RHEAD/RDATA díl DATA. Během PLAY se 1 s po začátku bloku 1
     uloží snapshot, páska se vysune a snapshot načte: páska je vložená,
     PLAY bez pauzy, blok 1, pozice v bloku do 20 ms (1 snímek) od uložené.
     Totéž v novém procesu bez pásky, kde dohrání pak DATA nahraje
     (výsledek LOADERu 55h, data v RAM shodná).
  G  varování loaderu pásky (stderr procesu): páska jen s řídicím blokem
     (bez otevřeného bloku) se obnoví ve STOP bez varování; upravené kopie
     snapshotu MZF pásky: jiný počet bloků -> přesně "Snapshot tape does
     not match file: <cesta>", ``<player>`` odmítnutý už při čtení XML
     (loop_depth mimo meze) i odmítnutý ověřením stavu (signal_level 9)
     -> stejné varování "Snapshot tape player state is invalid".

Každý proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní
INI, ``--no-save-ini`` kromě fáze E, která INI záměrně ukládá); zabíjí se
jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]

#: Délka těla MZF (bajty) - páska musí hrát déle než celý test.
_MZF_BODY = 16384

#: Stavy transportu v get_periph_cmt a playsts (en_CMTEXT_BLOCK_PLAYSTS).
_STATE_STOP, _STATE_PLAY = "stop", "play"
_PLAYSTS_BODY = 0


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def _find_exe():
    """Najde binárku mz800emu (přednost má proměnná MZ_EMU)."""
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env)
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    sys.exit(1)


#: Fáze F: adresa a délka dílu DATA, adresa výsledku LOADERu (55h OK, EEh chyba).
_DATA_ADDR = 0x4000
_DATA_SIZE = 512
_FLAG_ADDR = 0x1300
#: Fáze F: nejvýš snímků na nahrání obou dílů 1:1 (asi 25 s pásky).
_LOAD_FRAMES = 2500


def _mzf(name, ftype, addr, body):
    """Sestaví MZF (hlavička 128 B + tělo, fexec = addr)."""
    hdr = bytearray(128)
    hdr[0] = ftype
    hdr[1:18] = b"\r" * 17
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = addr.to_bytes(2, "little")
    hdr[0x16:0x18] = addr.to_bytes(2, "little")
    return bytes(hdr) + bytes(body)


def _loader_body():
    """LOADER na 1200h: RHEAD (0027h) + RDATA (002Ah), výsledek na 1300h, smyčka.

    1200 CD 27 00   CALL 0027h
    1203 38 09      JR C,120Eh
    1205 CD 2A 00   CALL 002Ah
    1208 38 04      JR C,120Eh
    120A 3E 55      LD A,55h
    120C 18 02      JR 1210h
    120E 3E EE      LD A,0EEh
    1210 32 00 13   LD (1300h),A
    1213 18 FE      JR $
    """
    return bytes([0xCD, 0x27, 0x00, 0x38, 0x09, 0xCD, 0x2A, 0x00, 0x38, 0x04,
                  0x3E, 0x55, 0x18, 0x02, 0x3E, 0xEE, 0x32, 0x00, 0x13, 0x18, 0xFE])


def _data_body():
    """Tělo dílu DATA (pseudonáhodné, aby chyba čtení byla vidět)."""
    return bytes(((i * 73) + 29) & 0xFF for i in range(_DATA_SIZE))


def _make_mzt(path):
    """MZT = LOADER (blok 0) + DATA (blok 1)."""
    path.write_bytes(_mzf(b"SNAP LOADER", 0x01, 0x1200, _loader_body())
                     + _mzf(b"SNAP DATA", 0x01, _DATA_ADDR, _data_body()))


def _patch_snapshot(src, dst, fn):
    """Kopie snapshotu s devices/cmt.xml upraveným funkcí fn(text) a bez kontrolního součtu."""
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "devices/cmt.xml":
                data = fn(data.decode("utf-8")).encode("utf-8")
            elif item.filename == "manifest.xml":
                data = re.sub(r"\s*<checksum[^>]*>.*?</checksum>", "",
                              data.decode("utf-8"), flags=re.S).encode("utf-8")
            zout.writestr(item.filename, data)


def _set_elem(text, name, value):
    """Nahradí hodnotu prvního elementu name (musí existovat)."""
    out, n = re.subn(r"(<%s(?:\s[^>]*)?>)[^<]*(</%s>)" % (name, name),
                     lambda m: f"{m.group(1)}{value}{m.group(2)}", text, count=1)
    if n != 1:
        raise TestFailure(f"element {name} not found in devices/cmt.xml")
    return out


def _make_old_snapshot(src, dst):
    """Kopie snapshotu ve starém formátu: bez <tape> v devices/cmt.xml a bez kontrolního součtu."""
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "devices/cmt.xml":
                text, n = re.subn(r"\s*<tape>.*?</tape>", "", data.decode("utf-8"), flags=re.S)
                if n != 1:
                    raise TestFailure("snapshot has no <tape> element in devices/cmt.xml")
                data = text.encode("utf-8")
            elif item.filename == "manifest.xml":
                text = re.sub(r"\s*<checksum[^>]*>.*?</checksum>", "",
                              data.decode("utf-8"), flags=re.S)
                data = text.encode("utf-8")
            zout.writestr(item.filename, data)


def _make_mzf(path):
    """Vytvoří MZF (atribut 01h, fstrt = fexec = 1200h, tělo _MZF_BODY bajtů)."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"CMT SNAP TEST\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = _MZF_BODY.to_bytes(2, "little")
    hdr[0x14:0x16] = (0x1200).to_bytes(2, "little")
    hdr[0x16:0x18] = (0x1200).to_bytes(2, "little")
    body = bytes(((i * 37) + 11) & 0xFF for i in range(_MZF_BODY))
    path.write_bytes(bytes(hdr) + body)


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, ini, save_ini=False, stderr_path=None):
        args = [str(exe), "--mcp-pipe", "--headless",
                "--no-first-run-windows", f"--cfg-dir={cfg_dir}",
                f"--work-dir={cfg_dir}", f"--config={ini}"]
        if not save_ini:
            args.insert(3, "--no-save-ini")
        self._stderr = open(stderr_path, "w", encoding="utf-8") if stderr_path else None
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self._stderr or subprocess.DEVNULL, text=True,
                                     bufsize=1, encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise TestFailure("no hello from emulator")

    def _pump(self):
        for line in iter(self.proc.stdout.readline, ""):
            self.q.put(line.strip())
        self.q.put(None)

    def _read(self, timeout, pred):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                return None
            if line is None:
                return None
            if not line.startswith("{"):
                continue
            msg = json.loads(line)
            if pred(msg):
                return msg
        return None

    def call(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            rc = self.proc.poll()
            raise TestFailure(f"no response to {cmd} (emulator exit code: {rc})")
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def close(self):
        try:
            self.call("shutdown", timeout=10.0)
        except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
            pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()
        if self._stderr:
            self._stderr.close()

    def ram(self, addr, length):
        """Přečte bajty z regionu User RAM."""
        d = self.call("region_read", {"region_id": 1, "offset": addr, "length": length})
        return base64.b64decode(d["data_b64"])

    def ticks(self):
        """Čas v taktech GDG od startu emulátoru (state_observe time.ticks)."""
        return int(self.call("state_observe", {"windows": []})["time"]["ticks"])


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def phase_a(new_emu, mzf, snap):
    """Pořídí snapshot během přehrávání a ověří příznaky playable/recordable."""
    print("Phase A: snapshot during tape playback")
    emu = new_emu()
    try:
        emu.call("run", {"frames": 350})
        res = emu.call("cmt_open", {"path": str(mzf), "play_immediately": False})
        check(res.get("playing") is False and res.get("state") == _STATE_STOP
              and res.get("paused") is False and "warning" not in res,
              f"cmt_open without play reports the real state (stop): {res}")
        tape = emu.call("cmt_tape_list")
        blocks = tape.get("blocks") or []
        check(len(blocks) == 1, "tape list has one block")
        check(blocks[0].get("playable") is True, "MZF block is playable")
        check(blocks[0].get("recordable") is False, "MZF block is not recordable")
        emu.call("input_send_key", {"key": "C", "frames": 5})
        emu.call("cmt_transport", {"action": "play"})
        emu.call("run", {"frames": 100})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == _STATE_PLAY and st.get("filled"),
              "tape is playing before snapshot_save")
        emu.call("snapshot_save", {"path": str(snap), "description": "cmt midload"})
    finally:
        emu.close()


def _load_without_tape(new_emu, snap, stderr_path, what):
    """Načte snapshot do procesu bez pásky; páska nevložená, STOP. Vrátí emulátor."""
    emu = new_emu(stderr_path)
    emu.call("pause")
    emu.call("snapshot_load", {"path": str(snap)})
    st = emu.call("get_periph_cmt")
    check(not st.get("filled"), f"[{what}] no tape inserted after snapshot_load")
    check(st.get("state") == _STATE_STOP, f"[{what}] transport is STOP after snapshot_load")
    check(not st.get("paused"), f"[{what}] transport is not paused")
    return emu


def phase_b(new_emu, mzf, snap, tmp):
    """Snapshot bez souboru pásky a ve starém formátu: STOP, run nepadá, stop je ok."""
    print("Phase B1: snapshot_load with the tape file missing")
    hidden = mzf.with_suffix(".hidden")
    mzf.rename(hidden)
    log = tmp / "b1_stderr.txt"
    try:
        emu = _load_without_tape(new_emu, snap, log, "missing file")
        emu.close()
    finally:
        hidden.rename(mzf)
    warn = f"Snapshot tape file not found: {mzf}"
    check(warn in log.read_text(encoding="utf-8", errors="replace"),
          f"stderr has the warning '{warn}'")

    print("Phase B2: old-format snapshot (no <tape>) without tape, then run")
    old = tmp / "midload_old.mzs"
    _make_old_snapshot(snap, old)
    emu = _load_without_tape(new_emu, old, None, "old snapshot")
    try:
        emu.call("run", {"frames": 5})
        emu.call("run", {"frames": 50})
        check(emu.proc.poll() is None, "emulator process alive after run")
        # Stav PLAY bez pásky už po opravách přes MCP nevznikne (snapshot ho
        # srovná do STOP), takže stop z PLAY bez pásky tu ověřit nejde - to
        # pokrývá unit test cmt_state. Tady jen: stop bez pásky vrací ok
        # a stav zůstává STOP.
        emu.call("cmt_transport", {"action": "stop"})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == _STATE_STOP,
              "cmt_transport stop without tape returns ok, state stays STOP")
        # play_paused bez pásky nesmí nechat STOP s příznakem pauzy.
        emu.call("cmt_transport", {"action": "play_paused"})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == _STATE_STOP and not st.get("paused"),
              "cmt_transport play_paused without tape leaves STOP, not paused")
    finally:
        emu.close()


def phase_c(new_emu, mzf, snap):
    """Po snapshot_load + cmt_open play_immediately hraje páska od začátku."""
    print("Phase C: snapshot_load + cmt_open play_immediately")
    emu = new_emu()
    try:
        emu.call("pause")
        emu.call("snapshot_load", {"path": str(snap)})
        res = emu.call("cmt_open", {"path": str(mzf), "play_immediately": True})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == _STATE_PLAY and st.get("filled"), "tape is playing")
        check(res.get("playing") is True and res.get("state") == st.get("state")
              and res.get("paused") == st.get("paused") and "warning" not in res,
              f"cmt_open play_immediately reports the real state: {res}")
        check(st.get("playsts") == _PLAYSTS_BODY, "playsts is BODY (fresh play)")
        check(st.get("paused_time", 0) == 0, "no stale paused_time from the snapshot")
        # Vzorkování výstupu v nepravidelných bodech (proměnlivý počet
        # instrukcí mezi snímky), aby vzorkování nebylo v pevné fázi signálu.
        levels = []
        for i in range(40):
            emu.call("run", {"frames": 1})
            emu.call("step_n", {"count": (i * 37) % 97 + 1})
            st = emu.call("get_periph_cmt")
            levels.append(st.get("output"))
        check(st.get("state") == _STATE_PLAY, "tape still playing")
        check(levels.count(0) >= 3 and levels.count(1) >= 3,
              f"tape signal changes while playing (levels: {''.join(map(str, levels))})")
    finally:
        emu.close()


def _check_restored(emu, snap, mzt, pos, frame_ticks, what):
    """Načte snapshot fáze F a ověří vloženou pásku, PLAY, blok 1 a pozici (do 1 snímku = 20 ms)."""
    emu.call("snapshot_load", {"path": str(snap)})
    st = emu.call("get_periph_cmt")
    check(st.get("filled") and st.get("image_basename") == mzt.name,
          f"[{what}] tape inserted after snapshot_load ({st.get('image_basename')})")
    check(st.get("state") == _STATE_PLAY and not st.get("paused"),
          f"[{what}] transport PLAY (not paused) as when saved")
    check(emu.call("cmt_tape_list").get("current_block") == 1,
          f"[{what}] block 1 after snapshot_load")
    got = emu.ticks() - st.get("start_time")
    check(abs(got - pos) <= frame_ticks,
          f"[{what}] position within 20 ms of the saved one ({got} vs {pos} ticks)")


def phase_f(new_emu, tmp):
    """Snapshot pásky při PLAY 1 s do bloku 1: po vysunutí a načtení stejná pozice, DATA se nahraje."""
    print("Phase F: tape, block and position in the snapshot")
    mzt = tmp / "snap_tape.mzt"
    _make_mzt(mzt)
    snap = tmp / "tape_pos.mzs"
    emu = new_emu()
    try:
        emu.call("run", {"frames": 350})
        emu.call("cmt_open", {"path": str(mzt), "play_immediately": False})
        emu.call("region_write", {"region_id": 1, "offset": _FLAG_ADDR,
                                  "data_b64": base64.b64encode(b"\x00").decode("ascii")})
        emu.call("input_send_key", {"key": "C", "frames": 5})
        emu.call("cmt_transport", {"action": "play"})
        done = 0
        while emu.call("cmt_tape_list").get("current_block") != 1 and done < _LOAD_FRAMES:
            emu.call("run", {"frames": 5})
            done += 5
        check(emu.call("cmt_tape_list").get("current_block") == 1, "tape reached block 1")
        t0 = emu.ticks()
        emu.call("run", {"frames": 50})
        frame_ticks = (emu.ticks() - t0) // 50
        emu.call("pause")
        st = emu.call("get_periph_cmt")
        check(st.get("state") == _STATE_PLAY and not st.get("paused"), "tape plays 1 s into block 1")
        pos = emu.ticks() - st.get("start_time")
        check(pos >= 50 * frame_ticks, f"position in block 1 is at least 1 s ({pos} ticks)")
        emu.call("snapshot_save", {"path": str(snap), "description": "tape position"})
        with zipfile.ZipFile(snap) as z:
            check(b"<tape>" in z.read("devices/cmt.xml"), "snapshot has the <tape> element")

        emu.call("cmt_transport", {"action": "eject"})
        check(not emu.call("get_periph_cmt").get("filled"), "tape ejected")
        _check_restored(emu, snap, mzt, pos, frame_ticks, "same process")
    finally:
        emu.close()

    # Nový proces bez pásky: načtení snapshotu pásku otevře a dohraje.
    emu = new_emu()
    try:
        emu.call("pause")
        _check_restored(emu, snap, mzt, pos, frame_ticks, "new process")
        flag = 0
        done = 0
        while done < _LOAD_FRAMES and flag not in (0x55, 0xEE):
            emu.call("run", {"frames": 100})
            done += 100
            flag = emu.ram(_FLAG_ADDR, 1)[0]
        check(flag == 0x55, f"LOADER loaded DATA after snapshot_load (result {flag:02X}h)")
        check(emu.ram(_DATA_ADDR, _DATA_SIZE) == _data_body(), "DATA in RAM matches the tape")
    finally:
        emu.close()


def _make_text_only_tzx(path):
    """TZX jen s blokem 0x30 Text Description (žádný přehratelný blok)."""
    text = b"only text"
    path.write_bytes(b"ZXTape!\x1a" + bytes([1, 20, 0x30, len(text)]) + text)


def phase_g(new_emu, mzf, tmp):
    """Varování loaderu pásky: bez bloku bez varování, neshoda a neplatný stav playeru."""
    print("Phase G: tape loader warnings")
    tzx = tmp / "text_only.tzx"
    _make_text_only_tzx(tzx)
    snap = tmp / "text_only.mzs"
    log = tmp / "g1_stderr.txt"
    emu = new_emu(log)
    try:
        emu.call("pause")
        emu.call("cmt_open", {"path": str(tzx), "play_immediately": False})
        check(emu.call("get_periph_cmt").get("filled"), "control-only tape inserted")
        emu.call("snapshot_save", {"path": str(snap), "description": "no block"})
        with zipfile.ZipFile(snap) as z:
            check(b"<block_id" in z.read("devices/cmt.xml"), "snapshot has the tape element")
        emu.call("cmt_transport", {"action": "eject"})
        emu.call("snapshot_load", {"path": str(snap)})
        st = emu.call("get_periph_cmt")
        check(st.get("filled") and st.get("state") == _STATE_STOP,
              "tape without an open block restored, STOP")
    finally:
        emu.close()
    text = log.read_text(encoding="utf-8", errors="replace")
    check("does not match" not in text and "player state is invalid" not in text,
          "no mismatch or player warning for a tape without an open block")

    base = tmp / "g_base.mzs"
    variants = [
        ("mismatch", lambda t: _set_elem(t, "block_count", 2)),
        ("player_read", lambda t: _set_elem(t, "loop_depth", 99)),
        ("player_check", lambda t: _set_elem(t, "signal_level", 9)),
    ]
    log = tmp / "g2_stderr.txt"
    emu = new_emu(log)
    try:
        emu.call("pause")
        emu.call("cmt_open", {"path": str(mzf), "play_immediately": False})
        emu.call("snapshot_save", {"path": str(base), "description": "base"})
        for name, fn in variants:
            dst = tmp / f"g_{name}.mzs"
            _patch_snapshot(base, dst, fn)
            emu.call("snapshot_load", {"path": str(dst)})
            st = emu.call("get_periph_cmt")
            check(st.get("filled") and st.get("state") == _STATE_STOP,
                  f"[{name}] tape stays inserted, STOP")
    finally:
        emu.close()
    text = log.read_text(encoding="utf-8", errors="replace")
    mismatch = f"Snapshot tape does not match file: {mzf}"
    check(text.count(mismatch) == 1, f"stderr has exactly one '{mismatch}'")
    invalid = "Snapshot tape player state is invalid, playback continues from block 0"
    check(text.count(invalid) == 2,
          f"player rejected while reading and by the check give the same warning ({text.count(invalid)}x)")


def _max_speed(emu):
    """Vrátí příznak MAX SPEED z ``get_speed``."""
    return bool(emu.call("get_speed").get("max_speed"))


def phase_d(new_emu, mzf, tmp):
    """cpu_boost: MAX SPEED po snapshot_load odpovídá stavu transportu."""
    print("Phase D: cpu_boost and MAX SPEED after snapshot_load")
    snap_play = tmp / "boost_play.mzs"
    snap_stop = tmp / "boost_stop.mzs"
    emu = new_emu()
    try:
        emu.call("pause")
        emu.call("cmt_set_property", {"property": "cpu_boost", "value": 1})
        check(not _max_speed(emu), "MAX SPEED is off at start")
        emu.call("snapshot_save", {"path": str(snap_stop), "description": "boost stop"})
        emu.call("cmt_open", {"path": str(mzf), "play_immediately": True})
        check(_max_speed(emu), "cpu_boost turns MAX SPEED on while the tape plays")
        emu.call("snapshot_save", {"path": str(snap_play), "description": "boost play"})
        emu.call("cmt_transport", {"action": "stop"})
        check(not _max_speed(emu), "stop turns boost MAX SPEED off")

        emu.call("snapshot_load", {"path": str(snap_play)})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == _STATE_PLAY and st.get("cpu_boost"),
              "snapshot with playing tape restores PLAY (cpu_boost unchanged)")
        check(_max_speed(emu), "MAX SPEED is on after loading the playing snapshot")
        emu.call("snapshot_load", {"path": str(snap_stop)})
        check(emu.call("get_periph_cmt").get("state") == _STATE_STOP,
              "STOP snapshot restores STOP")
        check(not _max_speed(emu), "boost MAX SPEED is off after loading the STOP snapshot")

        # MAX SPEED zvolená uživatelem: CMT ji nesmí vypnout.
        emu.call("set_speed", {"mode": "max"})
        emu.call("cmt_transport", {"action": "play"})
        emu.call("cmt_transport", {"action": "stop"})
        check(_max_speed(emu), "user MAX SPEED survives tape play + stop")
        emu.call("snapshot_load", {"path": str(snap_stop)})
        check(_max_speed(emu), "user MAX SPEED survives loading the STOP snapshot")
    finally:
        emu.close()


def _ini_cpu_boost(ini):
    """Vrátí hodnotu klíče cpu_boost ze sekce [CMT] v INI (int), jinak None."""
    section = None
    for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().upper()
        elif section == "CMT" and "=" in line:
            key, val = (x.strip() for x in line.split("=", 1))
            if key == "cpu_boost":
                try:
                    return int(val)
                except ValueError:
                    return None
    return None


def _boost_case(exe, mzf, tmp, name, snap_boost, user_boost):
    """Jeden směr fáze E: snapshot s cpu_boost=snap_boost, uživatel user_boost.

    Snapshot (hrající páska) pořídí proces s --no-save-ini, načte ho proces
    s vlastním INI (cpu_boost=user_boost), který INI při ukončení uloží.
    """
    snap = tmp / f"{name}.mzs"
    base_ini = tmp / "test.ini"
    cfg = tmp / f"{name}_save"
    cfg.mkdir()
    emu = PipeEmu(exe, cfg, base_ini)
    try:
        emu.call("pause")
        emu.call("cmt_set_property", {"property": "cpu_boost", "value": snap_boost})
        emu.call("cmt_open", {"path": str(mzf), "play_immediately": True})
        check(emu.call("get_periph_cmt").get("state") == _STATE_PLAY,
              f"[{name}] tape is playing before snapshot_save")
        emu.call("snapshot_save", {"path": str(snap), "description": name})
    finally:
        emu.close()

    ini = tmp / f"{name}_user.ini"
    ini.write_text(base_ini.read_text(encoding="utf-8")
                   + f"[CMT]\ncpu_boost = {user_boost}\n", encoding="utf-8")
    cfg = tmp / f"{name}_load"
    cfg.mkdir()
    emu = PipeEmu(exe, cfg, ini, save_ini=True)
    try:
        emu.call("pause")
        st = emu.call("get_periph_cmt")
        check(bool(st.get("cpu_boost")) == bool(user_boost),
              f"[{name}] user cpu_boost={user_boost} is active before load")
        emu.call("cmt_open", {"path": str(mzf), "play_immediately": False})
        emu.call("snapshot_load", {"path": str(snap)})
        st = emu.call("get_periph_cmt")
        check(st.get("state") == _STATE_PLAY, f"[{name}] snapshot restores PLAY")
        check(bool(st.get("cpu_boost")) == bool(user_boost),
              f"[{name}] snapshot (cpu_boost={snap_boost}) keeps user cpu_boost={user_boost}")
        check(_max_speed(emu) == bool(user_boost),
              f"[{name}] MAX SPEED follows the user cpu_boost after load")
    finally:
        emu.close()
    val = _ini_cpu_boost(ini)
    check(val == user_boost,
          f"[{name}] INI keeps cpu_boost={user_boost} after exit (got {val})")


def phase_e(exe, mzf, tmp):
    """Snapshot neobnovuje cpu_boost ani ho nepropíše do INI uživatele."""
    print("Phase E: snapshot_load keeps the user cpu_boost (runtime and INI)")
    _boost_case(exe, mzf, tmp, "snap_on_user_off", 1, 0)
    _boost_case(exe, mzf, tmp, "snap_off_user_on", 0, 1)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=165)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_cmt_snapshot_"))
    ok = False
    try:
        mzf = tmp / "cmt_snapshot_test.mzf"
        _make_mzf(mzf)
        snap = tmp / "midload.mzs"
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[CMTHACK]\nenable = 0\n", encoding="utf-8")
        counter = [0]

        def new_emu(stderr_path=None):
            counter[0] += 1
            cfg = tmp / f"cfg{counter[0]}"
            cfg.mkdir()
            return PipeEmu(exe, cfg, ini, stderr_path=stderr_path)

        phase_a(new_emu, mzf, snap)
        phase_b(new_emu, mzf, snap, tmp)
        phase_c(new_emu, mzf, snap)
        phase_d(new_emu, mzf, tmp)
        phase_e(exe, mzf, tmp)
        phase_f(new_emu, tmp)
        phase_g(new_emu, mzf, tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
