#!/usr/bin/env python3
"""E2E regresní test: CMT hack bez GUI nesmí zablokovat emulační vlákno.

Chyba nahlášená 3. 10. 2026 (boot MZ-800 do menu, klávesa C, ``run 50`` ->
``actual_frames: 0``, ``stopped_by: timeout`` a potom každý příkaz
"Emulator busy" navždy). Příčina: se zapnutým CMT hack patchem volá ROM
při čtení hlavičky z kazety ``OUT (01h)`` a ``cmthack_load_file()`` otevřel
uvnitř instrukce blokující dialog pro výběr MZF. V ``--headless`` /
``--mcp-pipe`` ho nikdo nezavře, takže EMU vlákno čekalo navždy a frontu
dbgapi už nikdo nevybral. Oprava: bez GUI se požadavek vyřídí hned jako
zrušený (ROM dostane Break) s varováním na stderr.

Fáze (každá v novém izolovaném procesu: dočasný ``--cfg-dir``/``--work-dir``,
vlastní prázdné INI, ``--no-save-ini``, zabíjí se jen vlastní PID):

  A  všechny nalezené binárky (mz700emu-pal, mz700emu-ntsc, mz800emu,
     mz1500emu; chybí-li jen některá, test selže): CMT hack zapnutý, vlastní
     program v RAM provede ``OUT (01h),A`` (stejná instrukce jako patch ROM).
     ``run 50`` musí doběhnout 50 snímků, příkazy odpovídají, CPU pokračuje
     za OUT a na stderr je varování.
  B  mz800emu, přesná posloupnost z bugreportu: ``run 350`` (boot do menu),
     klávesa C, ``run 50`` -> 50 snímků, ``get_registers`` odpoví.
  C  mz800emu, fáze A původního skriptu bugreportu: ``run 350``, ``cpu_boost``
     0, ``cmt_open`` (vlastní MZF), klávesa C, ``cmt_transport play``,
     ``run 300`` -> 300 snímků, ``get_periph_cmt`` odpoví.
  D  všechny nalezené binárky: CMT hack vypnutý, program provede
     ``OUT (01h),A`` a ``OUT (02h),A``. Porty nejsou pastí (dřív se past
     spustila i bez patche a změnila AF z 0044h): AF i HL zůstanou, CPU
     doběhne do smyčky, na stderr není varování CMT hacku.
  E  mz800emu, CMT hack z vložené pásky: MZT se dvěma programy (4000h,
     5000h), zavaděč v RAM volá pasti stejně jako patch ROM (hlavička
     ``OUT (01h)``, tělo ``OUT (02h)`` s HL = fstrt, BC = fsize z hlavičky).
     První načtení dá program 1 (obsah paměti na fstrt), druhé program 2,
     třetí Break s varováním; transport zůstává ve STOP.
  F  mz800emu, páska TAP + CMT hack: aktuální blok MZF nenese, načtení
     skončí Break s dnešním varováním.
  G  mz800emu, jako E, ale páska vložená s ``play_immediately`` (hraje):
     první načtení transport zastaví (STOP), žádný program se nepřeskočí.

Bez jakékoli binárky vrací 77 (SKIP). Výstup anglicky.
Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
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

#: (jméno binárky, platforma)
_PLATFORMS = [
    ("mz700emu-pal", "mz700"),
    ("mz700emu-ntsc", "mz700"),
    ("mz800emu", "mz800"),
    ("mz1500emu", "mz1500"),
]

#: Limit na odpověď běžného příkazu (s). Chyba dávala "Emulator busy" po 10 s.
_CMD_LIMIT_S = 3.0

#: Limit na ``run`` s frames (s). 350 snímků trvá v reálném čase cca 7 s.
_RUN_LIMIT_S = 40.0

#: Exec adresa testovacího programu.
_EXEC = 0x1200

#: Program: 1200h DI; 1201h LD HL,10F0h; 1204h OUT (01h),A; 1206h INC A;
#: 1207h JR 1206h. OUT (01h) je požadavek CMT hacku na hlavičku (stejně
#: jako patch ROM, HL = adresa hlavičky).
_PROGRAM = bytes([0xF3, 0x21, 0xF0, 0x10, 0xD3, 0x01, 0x3C, 0x18, 0xFD])

#: PC smyčky za OUT (01h).
_LOOP_PCS = (0x1206, 0x1207)

#: Program fáze D: 1200h DI; 1201h LD HL,10F0h; 1204h XOR A;
#: 1205h OUT (01h),A; 1207h OUT (02h),A; 1209h JR 1209h.
_PROGRAM_OFF = bytes([0xF3, 0x21, 0xF0, 0x10, 0xAF, 0xD3, 0x01,
                      0xD3, 0x02, 0x18, 0xFE])

#: PC smyčky programu fáze D.
_LOOP_PC_OFF = 0x1209

#: Fáze E/F: zavaděč na 1200h. Hlavička na 2000h (OUT 01h), tělo podle
#: hlavičky (HL = (2014h) fstrt, BC = (2012h) fsize, OUT 02h), výsledek na
#: 1300h: 55h = načteno, jinak A z pasti (1 chyba, 2 Break).
#:   1200 DI / LD HL,2000h / OUT (01h),A / JR C,1215h / LD HL,(2014h)
#:   LD BC,(2012h) / OUT (02h),A / JR C,1215h / LD A,55h / 1215 LD (1300h),A
#:   1218 JR $
_LOADER = bytes([0xF3, 0x21, 0x00, 0x20, 0xD3, 0x01, 0x38, 0x0D,
                 0x2A, 0x14, 0x20, 0xED, 0x4B, 0x12, 0x20, 0xD3, 0x02,
                 0x38, 0x02, 0x3E, 0x55, 0x32, 0x00, 0x13, 0x18, 0xFE])

#: PC smyčky zavaděče.
_LOADER_LOOP_PC = 0x1218

#: Adresa výsledku zavaděče.
_RESULT_ADDR = 0x1300

#: Programy MZT fáze E: (jméno, fstrt, délka těla, posun obsahu).
_TAPE_PROGRAMS = [(b"PROG ONE\r", 0x4000, 256, 3), (b"PROG TWO\r", 0x5000, 300, 77)]

#: Text varování na stderr (cmthack_load_file() bez GUI).
_WARNING = "CMT hack needs the GUI file dialog"


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


class CheckFailure(TestFailure):
    """Selhání check(); FAIL řádek už je vypsaný."""


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise CheckFailure(what)


def _find_exe(name):
    """Najde binárku v kořeni repa (s příponou .exe i bez), jinak None."""
    for c in (_REPO_ROOT / (name + ".exe"), _REPO_ROOT / name):
        if c.is_file():
            return c
    return None


def _mzf_bytes(name, addr, body):
    """MZF (hlavička + tělo): atribut 01h, fstrt = fexec = addr."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    hdr[1:18] = b"\r" * 17
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = addr.to_bytes(2, "little")
    hdr[0x16:0x18] = addr.to_bytes(2, "little")
    return bytes(hdr) + bytes(body)


def _program_body(size, shift):
    """Tělo programu fáze E (pseudonáhodné, programy se liší)."""
    return bytes(((i * 73) + shift) & 0xFF for i in range(size))


def _make_tap(path):
    """TAP s hlavičkou programu ZX Spectrum (blok s příznakem 00h)."""
    data = bytes([0x00, 0x03]) + b"TESTPROG  " + bytes([0x10, 0x00, 0x00, 0x80, 0x00, 0x80])
    chk = 0
    for b in data:
        chk ^= b
    block = data + bytes([chk])
    path.write_bytes(len(block).to_bytes(2, "little") + block)


def _make_mzf(path, body, name=b"CMTHACK TEST\r"):
    """Vytvoří MZF: atribut 01h, fstrt = fexec = 1200h, tělo ``body``."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = _EXEC.to_bytes(2, "little")
    hdr[0x16:0x18] = _EXEC.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp):
        ini = tmp / "emu.ini"
        ini.write_text("", encoding="utf-8")
        self.stderr_path = tmp / "stderr.txt"
        self._stderr = open(self.stderr_path, "w", encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self._stderr, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        self.dead = False
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

    def call(self, cmd, data=None, timeout=_CMD_LIMIT_S + 2.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure).

        Neúspěch (i "Emulator busy") je selhání testu, stejně jako
        překročený ``timeout``.
        """
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            # Zaseknuté emu vlákno neodpoví ani na shutdown - close() zabije PID.
            self.dead = True
            rc = self.proc.poll()
            raise TestFailure(f"no response to {cmd} within {timeout:.0f} s "
                              f"(emulator exit code: {rc})")
        if not resp.get("success"):
            self.dead = True
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def ram(self, addr, length):
        """Přečte bajty z regionu User RAM."""
        d = self.call("region_read", {"region_id": 1, "offset": addr, "length": length})
        return base64.b64decode(d["data_b64"])

    def run_frames(self, frames):
        """``run`` s frames: musí skončit počtem snímků, ne timeoutem."""
        d = self.call("run", {"frames": frames}, timeout=_RUN_LIMIT_S)
        got = d.get("actual_frames")
        # actual_frames je přesně N z pauzy i za běhu (výchozí čítač snímků
        # dodává handler RUN_FRAMES na emu vlákně).
        check(d.get("stopped_by") == "frames" and got == frames,
              f"run frames={frames} delivered {got} frames "
              f"(stopped_by: {d.get('stopped_by')})")
        return d

    def stderr_text(self):
        """Obsah stderr procesu (po close() úplný)."""
        if not self._stderr.closed:
            self._stderr.flush()
        return self.stderr_path.read_text(encoding="utf-8", errors="replace")

    def close(self):
        """Ukončí proces přes shutdown, jinak zabije vlastní PID."""
        if not self.dead:
            try:
                self.call("shutdown", timeout=10.0)
            except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
                pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()
        self._stderr.close()


def _enable_cmthack(emu):
    """Zapne CMT hack patch a ověří, že je nainstalovaný."""
    emu.call("cmt_hack_set", {"enabled": True})
    cmt = emu.call("get_periph_cmt")
    check(cmt.get("cmthack_enabled") is True, "CMT hack ROM patch installed")


def phase_a(exe, tmp, mzf):
    """OUT (01h) z vlastního programu: emulace i MCP musí běžet dál."""
    print("  Phase A: OUT (01h) with the CMT hack enabled, no GUI")
    emu = PipeEmu(exe, tmp)
    try:
        emu.run_frames(50)
        _enable_cmthack(emu)
        emu.call("media_load_mzf", {"path": str(mzf)})
        emu.call("set_register", {"reg": "PC", "value": _EXEC})
        emu.run_frames(50)
        regs = emu.call("get_registers")
        check(regs["PC"] in _LOOP_PCS,
              f"CPU continued past OUT (01h) (PC = {regs['PC']:04X}h)")
        emu.call("get_state")
    finally:
        emu.close()
    check(_WARNING in emu.stderr_text(), "headless CMT hack warning on stderr")


def phase_d(exe, tmp, mzf):
    """OUT (01h) a OUT (02h) s vypnutým CMT hackem nic nezmění."""
    print("  Phase D: OUT (01h) and OUT (02h) with the CMT hack disabled")
    emu = PipeEmu(exe, tmp)
    try:
        emu.run_frames(50)
        emu.call("cmt_hack_set", {"enabled": False})
        cmt = emu.call("get_periph_cmt")
        check(cmt.get("cmthack_enabled") is False, "CMT hack ROM patch not installed")
        emu.call("media_load_mzf", {"path": str(mzf)})
        emu.call("set_register", {"reg": "PC", "value": _EXEC})
        emu.run_frames(5)
        regs = emu.call("get_registers")
        check(regs["PC"] == _LOOP_PC_OFF,
              f"CPU reached the loop after both OUTs (PC = {regs['PC']:04X}h)")
        check(regs["AF"] == 0x0044 and regs["HL"] == 0x10F0,
              f"ports 01h/02h are no trap: AF = {regs['AF']:04X}h, "
              f"HL = {regs['HL']:04X}h (expected 0044h, 10F0h)")
    finally:
        emu.close()
    check(_WARNING not in emu.stderr_text(), "no CMT hack warning on stderr")


def phase_b(exe, tmp):
    """Přesná posloupnost bugreportu: boot menu, klávesa C, run 50."""
    print("  Phase B: MZ-800 boot menu, key C, run 50 (bug report sequence)")
    emu = PipeEmu(exe, tmp)
    try:
        _enable_cmthack(emu)
        emu.run_frames(350)
        emu.call("input_send_key", {"key": "C", "frames": 5})
        emu.call("get_registers")
        emu.run_frames(50)
        emu.call("get_registers")
        emu.call("get_state")
    finally:
        emu.close()


def phase_c(exe, tmp, tape):
    """Fáze A původního skriptu: cmt_open, klávesa C, play, run 300."""
    print("  Phase C: MZ-800 boot menu, cmt_open, key C, play, run 300")
    emu = PipeEmu(exe, tmp)
    try:
        _enable_cmthack(emu)
        emu.run_frames(350)
        emu.call("cmt_set_property", {"property": "cpu_boost", "value": 0})
        emu.call("cmt_open", {"path": str(tape), "play_immediately": False})
        emu.call("input_send_key", {"key": "C", "frames": 5})
        emu.call("cmt_transport", {"action": "play"})
        emu.run_frames(300)
        emu.call("get_periph_cmt")
        emu.call("get_registers")
    finally:
        emu.close()


def _run_loader(emu):
    """Spustí zavaděč (výsledek na 1300h vynulovaný) a vrátí bajt výsledku."""
    emu.call("region_write", {"region_id": 1, "offset": _RESULT_ADDR,
                              "data_b64": base64.b64encode(b"\x00").decode("ascii")})
    emu.call("set_register", {"reg": "PC", "value": _EXEC})
    emu.run_frames(5)
    regs = emu.call("get_registers")
    check(regs["PC"] == _LOADER_LOOP_PC, f"loader finished (PC = {regs['PC']:04X}h)")
    return emu.ram(_RESULT_ADDR, 1)[0]


def phase_e(exe, tmp, loader, mzt, playing=False):
    """CMT hack čte programy z vložené MZT pásky, třetí načtení = konec pásky."""
    print("  Phase %s: CMT hack loads the programs of the inserted tape%s"
          % (("G", " (tape playing)") if playing else ("E", "")))
    emu = PipeEmu(exe, tmp)
    try:
        emu.run_frames(50)
        _enable_cmthack(emu)
        emu.call("media_load_mzf", {"path": str(loader)})
        emu.call("cmt_open", {"path": str(mzt), "play_immediately": playing})
        if playing:
            emu.run_frames(25)
            cmt = emu.call("get_periph_cmt")
            check(cmt.get("state") == "play" and not cmt.get("paused"),
                  "tape plays before the first load")
        for n, (name, addr, size, shift) in enumerate(_TAPE_PROGRAMS):
            res = _run_loader(emu)
            if playing and n == 0:
                cmt = emu.call("get_periph_cmt")
                check(cmt.get("state") == "stop", "first load stopped the transport")
            check(res == 0x55, f"load {n + 1}: result {res:02X}h (55h = loaded)")
            check(emu.ram(addr, size) == _program_body(size, shift),
                  f"load {n + 1}: program {n + 1} in memory at {addr:04X}h")
            hdr = emu.ram(0x2000, 0x18)
            check(hdr[1:1 + len(name) - 1] == name[:-1],
                  f"load {n + 1}: header of program {n + 1} at 2000h")
        res = _run_loader(emu)
        check(res == 0x02, f"load 3: Break at the end of the tape (A = {res:02X}h)")
        cmt = emu.call("get_periph_cmt")
        check(cmt.get("state") == "stop" and cmt.get("filled"),
              "tape stays inserted, transport STOP")
        check(emu.call("cmt_tape_list").get("current_block") == 1,
              "tape stays on its last block")
    finally:
        emu.close()
    check(_WARNING in emu.stderr_text(), "end of tape: headless warning on stderr")


def phase_f(exe, tmp, loader, tap):
    """Páska TAP: aktuální blok MZF nenese, hack skončí Break s varováním."""
    print("  Phase F: CMT hack with a TAP tape (block without MZF)")
    emu = PipeEmu(exe, tmp)
    try:
        emu.run_frames(50)
        _enable_cmthack(emu)
        emu.call("media_load_mzf", {"path": str(loader)})
        emu.call("cmt_open", {"path": str(tap), "play_immediately": False})
        res = _run_loader(emu)
        check(res == 0x02, f"TAP tape: Break (A = {res:02X}h)")
    finally:
        emu.close()
    check(_WARNING in emu.stderr_text(), "TAP tape: headless warning on stderr")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=285)
    found = [(n, a, _find_exe(n)) for n, a in _PLATFORMS]
    present = [(n, a, e) for n, a, e in found if e is not None]
    if not present:
        print("SKIP: no emulator binaries found")
        return 77
    missing = [n for n, _, e in found if e is None]

    failures = []
    with tempfile.TemporaryDirectory(prefix="mz_cmthack_headless_") as td:
        root = Path(td)
        mzf = root / "out01.mzf"
        _make_mzf(mzf, _PROGRAM)
        mzf_off = root / "out0102.mzf"
        _make_mzf(mzf_off, _PROGRAM_OFF, name=b"CMTHACK OFF\r")
        tape = root / "tape.mzf"
        _make_mzf(tape, bytes((i * 37 + 11) & 0xFF for i in range(4096)),
                  name=b"CMTHACK TAPE\r")
        loader = root / "loader.mzf"
        _make_mzf(loader, _LOADER, name=b"CMTHACK LOADER\r")
        mzt = root / "two.mzt"
        mzt.write_bytes(b"".join(_mzf_bytes(n, a, _program_body(sz, sh))
                                 for n, a, sz, sh in _TAPE_PROGRAMS))
        tap = root / "spectrum.tap"
        _make_tap(tap)
        for name, arch, exe in present:
            print(f"== {name} ({arch}) ==")
            phases = [("A", lambda t, e=exe: phase_a(e, t, mzf)),
                      ("D", lambda t, e=exe: phase_d(e, t, mzf_off))]
            if arch == "mz800":
                phases.append(("B", lambda t, e=exe: phase_b(e, t)))
                phases.append(("C", lambda t, e=exe: phase_c(e, t, tape)))
                phases.append(("E", lambda t, e=exe: phase_e(e, t, loader, mzt)))
                phases.append(("F", lambda t, e=exe: phase_f(e, t, loader, tap)))
                phases.append(("G", lambda t, e=exe: phase_e(e, t, loader, mzt, playing=True)))
            for pname, fn in phases:
                tmp = root / f"{name}-{pname}"
                tmp.mkdir()
                try:
                    fn(tmp)
                except TestFailure as exc:
                    # FAIL kontroly už vypsala check(); jinak (bez odpovědi) ho vypsat.
                    failures.append(f"{name} phase {pname}: {exc}")
                    if not isinstance(exc, CheckFailure):
                        print(f"  FAIL: {exc}")

    if missing:
        failures.append("missing binaries: " + ", ".join(missing))
    if failures:
        print("FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
