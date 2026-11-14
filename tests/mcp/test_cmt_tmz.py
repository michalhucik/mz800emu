#!/usr/bin/env python3
"""E2E test pásek TMZ ve Virtual CMT a načtení MZF ROM na všech platformách (MCP pipe).

Testovací data jsou v ``tests/data/cmt/``: ``prog1.mzf`` a ``prog2.mzf``
(program na 1200h, zapíše 11h / 22h na 1300h a zůstane ve smyčce),
``two_programs.tmz`` (oba programy jako bloky 0x46 profilu ROM-800-NOMINAL)
a ``stop_between.tmz`` (prog1, Pause 0, prog2). Vyrábí je a kontroluje
unit test ``cmt_testdata`` (tests/peripherals/test_cmt_testdata.c) z knihovny
tmz; žádné cizí programy.

  A  mz800emu ``--cmt two_programs.tmz --cmt-play``: po bootu je transport
     PLAY + pauza (pásku spustila volba, hrana motoru při inicializaci ROM
     ji pozastavila), klávesa C bez dalšího PLAY: ROM zapne motor, načte
     z pásky první program a spustí ho (1300h = 11h, tělo na 1200h).
  B  mz800emu ``emu_cmt_open`` na .tmz: ``periph/cmt/tape`` má 2 bloky typu
     MZF (1) se jmény programů.
  C  mz800emu ``emu_cmt_tape_seek`` na blok 1, klávesa C, PLAY: ROM načte
     druhý program (1300h = 22h).
  D  mz800emu ``stop_between.tmz``: páska hraje bez ROM; po prvním programu
     se zastaví blokem Pause 0: transport PLAY, ``paused`` true, aktuální
     blok 2 (za příkazem).
  F  mz800emu ``emu_cmt_open`` na TZX s textem a blokem 0x19: pole
     ``warning`` vyjmenuje blok, který player neumí, a hlásí, že páska nemá
     přehratelný blok.
  G  mz800emu ``emu_cmt_tape_seek`` na blok 1 po 2 s přehrávání bloku 0
     (při PLAY a v pauze): transport zůstane ve stavu před seekem, pozice
     v bloku 1 je do 0,1 s od začátku a blok 1 hraje do konce pásky stejně
     dlouho jako po seeku ve STOP (reference). Neúspěšný seek při PLAY
     (blok mimo pásku, řídicí blok Pause 0 v ``stop_between.tmz``) vrátí
     chybu, páska dál hraje blok 0 a pozice běží.
  E  mz700emu-pal, mz1500emu (a mz800emu): ``--cmt prog1.mzf``, zavaděč v RAM
     (6000h) volá ROM RHEAD (0027h) a RDATA (002Ah), PLAY: ROM program
     načte (výsledek 55h, tělo na 1200h).

Každý proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní
INI s vypnutým CMT hackem, ``--no-save-ini``); zabíjí se jen vlastní PID.
Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka mz800emu).
"""

import base64
import json
import queue
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_DATA = _TESTS_DIR.parent / "data" / "cmt"

#: Adresa programů a výsledku programu (zapíše 11h / 22h).
_PROG_ADDR = 0x1200
_PROG_RESULT = 0x1300
#: Zavaděč fáze E a jeho výsledek (55h = načteno, EEh = chyba ROM).
_LOADER_ADDR = 0x6000
_LOADER_RESULT = 0x1301
#: Nejvýš snímků na nahrání programu 1:1.
_LOAD_FRAMES = 3000
#: Fáze G: krok měření konce pásky (snímky) a strop na dohrání bloku.
_SEEK_STEP = 5
_SEEK_END_FRAMES = 3000

#: 6000 CALL 0027h / JR C,600Eh / CALL 002Ah / JR C,600Eh / LD A,55h / JR 6010h
#: 600E LD A,0EEh / 6010 LD (1301h),A / 6013 JR $
_LOADER = bytes([0xCD, 0x27, 0x00, 0x38, 0x09, 0xCD, 0x2A, 0x00, 0x38, 0x04,
                 0x3E, 0x55, 0x18, 0x02, 0x3E, 0xEE, 0x32, 0x01, 0x13, 0x18, 0xFE])


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def _find_exe(name):
    """Binárka v kořeni repa (s .exe i bez), jinak None."""
    for c in (_REPO_ROOT / (name + ".exe"), _REPO_ROOT / name):
        if c.is_file():
            return c
    return None


def _mzf_body(path):
    """Tělo MZF souboru (bez 128 B hlavičky)."""
    return path.read_bytes()[128:]


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, extra_args=()):
        ini = cfg_dir / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[CMTHACK]\nenable = 0\n", encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--no-save-ini", "--headless",
                "--no-first-run-windows", f"--cfg-dir={cfg_dir}",
                f"--work-dir={cfg_dir}", f"--config={ini}", *extra_args]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise TestFailure(f"no hello from emulator (exit code: {self.proc.poll()})")

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

    def call_raw(self, cmd, data=None, timeout=60.0):
        """Pošle request a vrátí celou response (i neúspěšnou); bez response TestFailure."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to {cmd} (emulator exit code: {self.proc.poll()})")
        return resp

    def call(self, cmd, data=None, timeout=60.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        resp = self.call_raw(cmd, data, timeout)
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def ram(self, addr, length):
        """Přečte bajty z regionu User RAM."""
        d = self.call("region_read", {"region_id": 1, "offset": addr, "length": length})
        return base64.b64decode(d["data_b64"])

    def poke(self, addr, data):
        """Zapíše bajty do regionu User RAM."""
        self.call("region_write", {"region_id": 1, "offset": addr,
                                   "data_b64": base64.b64encode(bytes(data)).decode("ascii")})

    def run_until(self, addr, frames=_LOAD_FRAMES, step=100):
        """Běží po ``step`` snímcích, dokud bajt na ``addr`` není nenulový; vrátí ho."""
        done = 0
        while done < frames:
            self.call("run", {"frames": step}, timeout=120.0)
            done += step
            value = self.ram(addr, 1)[0]
            if value:
                return value
        return self.ram(addr, 1)[0]

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


def _boot_load(emu, marker, prog, play=True):
    """Boot menu MZ-800: klávesa C (a PLAY, je-li ``play``), ROM načte program z pásky a spustí ho."""
    emu.poke(_PROG_RESULT, b"\x00")
    emu.call("input_send_key", {"key": "C", "frames": 5})
    if play:
        emu.call("cmt_transport", {"action": "play"})
    res = emu.run_until(_PROG_RESULT)
    check(res == marker, f"ROM loaded and started the program (1300h = {res:02X}h, expected {marker:02X}h)")
    body = _mzf_body(prog)
    check(emu.ram(_PROG_ADDR, len(body)) == body, f"program body in memory at {_PROG_ADDR:04X}h")


def phase_a(exe, tmp):
    """--cmt two_programs.tmz --cmt-play: ROM načte první program."""
    print("Phase A: --cmt <file>.tmz --cmt-play, ROM loads the first program")
    emu = PipeEmu(exe, tmp / "a", ("--cmt", str(_DATA / "two_programs.tmz"), "--cmt-play"))
    try:
        emu.call("run", {"frames": 350})
        st = emu.call("get_periph_cmt")
        check(st.get("filled") and st.get("image_basename") == "two_programs.tmz",
              f"tape inserted from the command line ({st.get('image_basename')})")
        check(st.get("state") == "play" and st.get("paused") is True,
              f"--cmt-play started the tape, the ROM motor edge paused it "
              f"({st.get('state')}, paused {st.get('paused')})")
        _boot_load(emu, 0x11, _DATA / "prog1.mzf", play=False)
    finally:
        emu.close()


def phase_b_c(exe, tmp):
    """emu_cmt_open .tmz: dva bloky MZF se jmény; seek na blok 1 a ROM načte druhý program."""
    print("Phase B: emu_cmt_open on a .tmz, tape list")
    emu = PipeEmu(exe, tmp / "bc")
    try:
        emu.call("run", {"frames": 350})
        res = emu.call("cmt_open", {"path": str(_DATA / "two_programs.tmz"), "play_immediately": False})
        check("warning" not in res, f"cmt_open without import warning ({res})")
        tape = emu.call("cmt_tape_list")
        blocks = tape.get("blocks") or []
        check(tape.get("count") == 2 and len(blocks) == 2, f"tape has 2 blocks ({tape.get('count')})")
        check(all(b.get("type") == 1 for b in blocks), f"both blocks are MZF (type 1): {[b.get('type') for b in blocks]}")
        names = [b.get("name", "").strip() for b in blocks]
        check(names == ["TMZ PROG ONE", "TMZ PROG TWO"], f"block names {names}")

        print("Phase C: emu_cmt_tape_seek to block 1, ROM loads the second program")
        emu.call("cmt_tape_seek", {"block_id": 1})
        check(emu.call("cmt_tape_list").get("current_block") == 1, "current block is 1")
        _boot_load(emu, 0x22, _DATA / "prog2.mzf")

        print("Phase F: emu_cmt_open warns about blocks the player cannot play")
        tzx = tmp / "bc" / "unplayable.tzx"
        # TZX 1.20: text 0x30 a Generalized Data 0x19 (DWORD délka 0)
        tzx.write_bytes(b"ZXTape!\x1a\x01\x14" + b"\x30\x04text" + b"\x19\x00\x00\x00\x00")
        res = emu.call("cmt_open", {"path": str(tzx), "play_immediately": False})
        warning = res.get("warning", "")
        check("Blocks the player cannot play (skipped): 1 (ID 0x19)" in warning,
              f"warning names block 1 (ID 0x19) ({res})")
        check("The tape has no playable block" in warning, f"warning says there is no playable block ({res})")
    finally:
        emu.close()


def phase_d(exe, tmp):
    """Pause 0 mezi programy zastaví pásku za prvním programem."""
    print("Phase D: Pause 0 between the programs pauses the deck")
    emu = PipeEmu(exe, tmp / "d")
    try:
        emu.call("run", {"frames": 350})
        emu.call("cmt_open", {"path": str(_DATA / "stop_between.tmz"), "play_immediately": False})
        emu.call("cmt_transport", {"action": "play"})
        done = 0
        st = emu.call("get_periph_cmt")
        while done < _LOAD_FRAMES and not st.get("paused"):
            emu.call("run", {"frames": 100}, timeout=120.0)
            done += 100
            st = emu.call("get_periph_cmt")
        check(st.get("state") == "play" and st.get("paused") is True,
              f"after the first program: PLAY, paused ({st.get('state')}, {st.get('paused')})")
        check(emu.call("cmt_tape_list").get("current_block") == 2,
              "the tape waits at the block after the stop command (2)")
    finally:
        emu.close()


def _ticks(emu):
    """Čas v taktech GDG od startu emulátoru (state_observe time.ticks)."""
    return int(emu.call("state_observe", {"windows": []})["time"]["ticks"])


def _frames_until_tape_end(emu, limit=_SEEK_END_FRAMES):
    """Běží po ``_SEEK_STEP`` snímcích, dokud páska na konci nepřejde do STOP; vrátí takty GDG.

    Měří od zavolání do prvního kroku, po kterém je transport ve STOP
    (přesnost jeden krok). Při překročení ``limit`` snímků TestFailure.
    """
    t0 = _ticks(emu)
    done = 0
    while done < limit:
        emu.call("run", {"frames": _SEEK_STEP}, timeout=120.0)
        done += _SEEK_STEP
        if emu.call("get_periph_cmt").get("state") == "stop":
            return _ticks(emu) - t0
    raise TestFailure(f"the tape did not reach its end within {limit} frames")


def phase_g(exe, tmp):
    """emu_cmt_tape_seek při PLAY a v pauze: blok 1 hraje od začátku a dohraje se celý."""
    print("Phase G: emu_cmt_tape_seek while playing and paused starts the block from its start")
    emu = PipeEmu(exe, tmp / "g")
    try:
        emu.call("run", {"frames": 350})
        emu.call("cmt_open", {"path": str(_DATA / "two_programs.tmz"), "play_immediately": False})
        t0 = _ticks(emu)
        emu.call("run", {"frames": 50})
        frame_ticks = (_ticks(emu) - t0) // 50
        tol = 5 * frame_ticks  # 0,1 s
        step_tol = (_SEEK_STEP + 3) * frame_ticks  # přesnost měření konce pásky

        # Reference: seek na blok 1 ve STOP, PLAY, délka bloku 1 do konce pásky.
        emu.call("cmt_tape_seek", {"block_id": 1})
        emu.call("cmt_transport", {"action": "play"})
        ref = _frames_until_tape_end(emu)
        check(ref > 100 * frame_ticks, f"reference: block 1 plays {ref / frame_ticks:.0f} frames")

        for paused in (False, True):
            what = "paused" if paused else "playing"
            emu.call("cmt_tape_seek", {"block_id": 0})
            emu.call("cmt_transport", {"action": "play"})
            emu.call("run", {"frames": 100})  # 2 s do bloku 0
            if paused:
                emu.call("cmt_transport", {"action": "pause", "pause": 1})
            st = emu.call("get_periph_cmt")
            check(st.get("state") == "play" and bool(st.get("paused")) == paused
                  and emu.call("cmt_tape_list").get("current_block") == 0,
                  f"[{what}] block 0 is playing 2 s ({st.get('state')}, paused {st.get('paused')})")
            emu.call("cmt_tape_seek", {"block_id": 1})
            st = emu.call("get_periph_cmt")
            check(st.get("state") == "play" and bool(st.get("paused")) == paused,
                  f"[{what}] seek keeps the transport state ({st.get('state')}, paused {st.get('paused')})")
            check(emu.call("cmt_tape_list").get("current_block") == 1, f"[{what}] current block is 1")
            pos = st.get("paused_time") if paused else _ticks(emu) - st.get("start_time")
            check(0 <= pos <= tol, f"[{what}] position in block 1 is within 0.1 s of its start ({pos} ticks)")
            if paused:
                emu.call("cmt_transport", {"action": "pause", "pause": 0})
            got = _frames_until_tape_end(emu)
            check(abs(got - ref) <= step_tol,
                  f"[{what}] block 1 plays whole ({got} vs {ref} ticks, tolerance {step_tol})")

        # Neúspěšný seek při PLAY (blok mimo pásku, řídicí blok Pause 0):
        # chyba, transport dál PLAY na bloku 0 a pozice běží dál.
        emu.call("cmt_open", {"path": str(_DATA / "stop_between.tmz"), "play_immediately": False})
        blocks = emu.call("cmt_tape_list").get("blocks") or []
        check(len(blocks) == 3 and blocks[1].get("type") == 5,
              f"stop_between.tmz: block 1 is a control block (types {[b.get('type') for b in blocks]})")
        emu.call("cmt_transport", {"action": "play"})
        emu.call("run", {"frames": 50})
        for bad in (99, 1):
            st = emu.call("get_periph_cmt")
            pos0 = _ticks(emu) - st.get("start_time")
            resp = emu.call_raw("cmt_tape_seek", {"block_id": bad})
            check(not resp.get("success"), f"seek to block {bad} fails ({resp.get('error')})")
            st = emu.call("get_periph_cmt")
            check(st.get("state") == "play" and not st.get("paused")
                  and emu.call("cmt_tape_list").get("current_block") == 0,
                  f"after the failed seek to {bad} block 0 still plays ({st.get('state')}, paused {st.get('paused')})")
            emu.call("run", {"frames": 25})
            st = emu.call("get_periph_cmt")
            pos1 = _ticks(emu) - st.get("start_time")
            check(pos1 - pos0 >= 25 * frame_ticks,
                  f"after the failed seek to {bad} the position keeps advancing ({pos0} -> {pos1} ticks)")
    finally:
        emu.close()


def phase_e(name, exe, tmp):
    """--cmt prog1.mzf: ROM RHEAD/RDATA z vlastního zavaděče program načte."""
    print(f"Phase E ({name}): --cmt prog1.mzf, ROM RHEAD/RDATA load the program")
    loader = tmp / f"loader_{name}.mzf"
    hdr = bytearray(128)
    hdr[0] = 0x01
    hdr[1:18] = b"\r" * 17
    hdr[1:12] = b"TMZ LOADER\r"
    hdr[0x12:0x14] = len(_LOADER).to_bytes(2, "little")
    hdr[0x14:0x16] = _LOADER_ADDR.to_bytes(2, "little")
    hdr[0x16:0x18] = _LOADER_ADDR.to_bytes(2, "little")
    loader.write_bytes(bytes(hdr) + _LOADER)
    (tmp / f"e_{name}").mkdir(exist_ok=True)
    emu = PipeEmu(exe, tmp / f"e_{name}", ("--cmt", str(_DATA / "prog1.mzf")))
    try:
        emu.call("run", {"frames": 350})
        st = emu.call("get_periph_cmt")
        check(st.get("filled") and st.get("cmthack_enabled") is False,
              "tape inserted, CMT hack off")
        emu.call("media_load_mzf", {"path": str(loader)})
        emu.poke(_LOADER_RESULT, b"\x00")
        emu.call("set_register", {"reg": "PC", "value": _LOADER_ADDR})
        emu.call("cmt_transport", {"action": "play"})
        res = emu.run_until(_LOADER_RESULT)
        check(res == 0x55, f"ROM RHEAD + RDATA succeeded (result {res:02X}h, 55h = loaded)")
        body = _mzf_body(_DATA / "prog1.mzf")
        check(emu.ram(_PROG_ADDR, len(body)) == body, f"program body in memory at {_PROG_ADDR:04X}h")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=280)
    exe800 = _find_exe("mz800emu")
    if exe800 is None:
        print("SKIP: mz800emu binary not found")
        return 77
    failures = []
    with tempfile.TemporaryDirectory(prefix="mz_cmt_tmz_") as td:
        tmp = Path(td)
        steps = [("A", lambda: phase_a(exe800, tmp)),
                 ("B+C", lambda: phase_b_c(exe800, tmp)),
                 ("D", lambda: phase_d(exe800, tmp)),
                 ("G", lambda: phase_g(exe800, tmp))]
        for name in ("mz700emu-pal", "mz1500emu", "mz800emu"):
            exe = _find_exe(name)
            if exe is None:
                failures.append(f"missing binary {name}")
                continue
            steps.append((f"E {name}", lambda n=name, e=exe: phase_e(n, e, tmp)))
        for label, fn in steps:
            for sub in ("a", "bc", "d", "g"):
                (tmp / sub).mkdir(exist_ok=True)
            try:
                fn()
            except TestFailure as exc:
                failures.append(f"phase {label}: {exc}")
                print(f"  FAIL: {exc}")
    if failures:
        print("FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
