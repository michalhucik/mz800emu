#!/usr/bin/env python3
"""E2E test: ``media_run_mzf`` spustí MZF ve stejném stavu jako CLI ``--run-mzf``.

``media_run_mzf`` (MCP) resetuje stroj a zavede MZF bootstrapem
``mzarch_bootstrap_run_mzf()``. Výsledek nesmí záviset na tom, co v emulátoru
běželo předtím. Původní složené spuštění ve wrapperu (``media_load_mzf`` +
``io_write`` 0E0h/0E1h + PC) nechávalo stav předchozího programu (IM, I,
vektor Z80 PIO, ...), takže program po startu mohl běžet jinak než po
nahrání z pásky.

Pro binárky mz700emu-pal, mz700emu-ntsc, mz800emu a mz1500emu v kořeni repa
(headless + MCP TCP, izolovaný dočasný ``--cfg-dir``, ``--no-save-ini``):

  A  reference: ``--run-mzf`` s programem ``JR $`` na 1200h, PC_EXEC
     breakpoint na 1200h z ``.bpt`` zastaví CPU před první instrukcí;
     zachycení stavu;
  B  druhý proces bez ``--run-mzf``: "špinavý" program (IM 2, I = 0FEh,
     HL/DE/IX/IY s jinými hodnotami) zavedený bez resetu, na MZ-800 a
     MZ-1500 navíc vektor Z80 PIO kanálu A = 80h. V pauze ``media_run_mzf``
     téhož MZF jako v A: emulace zůstane v pauze na exec adrese a stav se
     musí shodovat s A;
  C  ve stejném procesu ``media_run_mzf`` s neexistujícím a se zkráceným
     souborem (fsize větší než tělo): chyba a stroj se nezresetuje (PC
     zůstane ve smyčce špinavého programu, IM 2 zůstane);
  D  ``media_run_mzf`` za běhu emulace: program se rozběhne (PC ve smyčce
     1200h, IM 1, DI).

Srovnává se: registry CPU kromě R, příznaky a režim přerušení, 8255,
8253 (bez běžících čítačů), Z80 PIO, PSG, mapování paměti, stav GDG bez
časových čítačů a signálů daných paprskem (HBLN, VBLN, HSYNC, VSYNC), RAM, kterou bootstrap nastavuje (trampolína 1038h,
hlavička 10F0h-116Fh, proměnné 119Bh-11A2h), a text na obrazovce.
Vynechané položky a důvody viz _VOLATILE_KEYS.

Port MCP TCP: proměnná ``MZ_MEDIA_RUN_MZF_TEST_PORT`` (23800 je vyhrazen
pro běžné instance); bez ní dostane každý spuštěný emulátor volný port od
systému. Zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka nenalezena).
"""

import base64
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů, volné TCP porty

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent

#: (jméno binárky, jméno .bpt souboru podle architektury, má Z80 PIO)
_PLATFORMS = [
    ("mz700emu-pal", "mz700-breakpoints.bpt", False),
    ("mz700emu-ntsc", "mz700-breakpoints.bpt", False),
    ("mz800emu", "mz800-breakpoints.bpt", True),
    ("mz1500emu", "mz1500-breakpoints.bpt", True),
]

#: Exec adresa testovacích programů.
_EXEC = 0x1200

#: Testovaný program: 1200h JR 1200h.
_PROGRAM = bytes([0x18, 0xFE])

#: "Špinavý" program: DI; LD A,0FEh; LD I,A; IM 2; LD HL,1234h;
#: LD DE,5678h; LD IX,9ABCh; LD IY,0DEF0h; JR $ (bez EI - deterministický).
_DIRTY = bytes([0xF3, 0x3E, 0xFE, 0xED, 0x47, 0xED, 0x5E,
                0x21, 0x34, 0x12, 0x11, 0x78, 0x56,
                0xDD, 0x21, 0xBC, 0x9A, 0xFD, 0x21, 0xF0, 0xDE,
                0x18, 0xFE])

#: Adresa smyčky JR $ ve špinavém programu.
_DIRTY_LOOP = _EXEC + len(_DIRTY) - 2

#: Úseky RAM, které bootstrap nastavuje (adresa, délka).
_RAM_RANGES = [(0x1038, 3), (0x10F0, 0x80), (0x119B, 8)]

#: Klíče odpovědí, které se nesrovnávají:
#:  - čas a běžící čítače (tiky a snímky GDG, paprsek, hodnoty CTC),
#:  - ``op_tstate``: mezi instrukcemi jen informativní (drain fronty dbgapi
#:    ho po příkazu vrací na hodnotu před příkazem),
#:  - ``tempo``/``tempo_divider``: signál TEMPO nuluje emulátor jen při
#:    startu procesu (gdg_init), reset ho nemění - stejně jako tlačítko Reset,
#:  - ``read_latch`` CTC: zbytek posledního Counter Latch; 8253 nemá vstup
#:    reset, program ho přečte až po novém latchi,
#:  - ``hbln``/``vbln``/``sts_hsync``/``sts_vsync`` GDG: signály dané jen
#:    pozicí paprsku (nastavují je události GDG). Reset raster nenuluje
#:    (gdg_reset mění jen registry), takže v B závisí na tom, kde zastavil
#:    předchozí ``run`` - např. MZ-700 PAL za sloupcem 28 (HBLN_END) má
#:    hbln = 1, v A (paprsek 0/0) 0.
_VOLATILE_KEYS = {"total_ticks", "total_screens", "ticks", "screens",
                  "beam_x", "beam_y", "raster_x", "raster_y",
                  "counter", "current_value", "value", "output",
                  "cycles", "total_cycles", "frame_cycles", "r", "op_tstate",
                  "tempo", "tempo_divider", "read_latch",
                  "hbln", "vbln", "sts_hsync", "sts_vsync"}


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def _find_exe(name):
    """Najde binárku v kořeni repa (s příponou .exe i bez), jinak None."""
    for c in (_REPO_ROOT / (name + ".exe"), _REPO_ROOT / name):
        if c.is_file():
            return c
    return None


def _make_mzf(path, body, name=b"RUN MZF TEST\r", size=None):
    """Vytvoří MZF: atribut 01h, fstrt = fexec = 1200h, tělo ``body``.

    ``size`` přepíše fsize v hlavičce (zkrácený soubor pro fázi C).
    """
    hdr = bytearray(128)
    hdr[0] = 0x01
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = (len(body) if size is None else size).to_bytes(2, "little")
    hdr[0x14:0x16] = _EXEC.to_bytes(2, "little")
    hdr[0x16:0x18] = _EXEC.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


class TcpEmu:
    """Headless emulátor s MCP TCP serverem: JSONL request/response přes socket."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, args, tmp, port):
        self.log = open(tmp / "emu.log", "w", encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=self.log,
                                     stderr=subprocess.STDOUT, cwd=str(_REPO_ROOT))
        self.sock = None
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise TestFailure(f"emulator exited during startup (exit code {self.proc.returncode})")
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
                break
            except OSError:
                time.sleep(0.2)
        if self.sock is None:
            raise TestFailure(f"cannot connect to MCP TCP port {port}")
        self.sock.settimeout(60.0)
        self.f = self.sock.makefile("rw", buffering=1, encoding="utf-8", newline="\n")
        if self._read(lambda m: m.get("type") == "hello") is None:
            raise TestFailure("no hello from emulator")
        self.rid = 0

    def _read(self, pred):
        while True:
            line = self.f.readline()
            if not line:
                return None
            try:
                msg = json.loads(line.strip())
            except ValueError:
                continue
            if pred(msg):
                return msg

    def request(self, cmd, data=None):
        """Pošle request a vrátí celou response (i neúspěšnou)."""
        self.rid += 1
        rid = self.rid
        self.f.write(json.dumps({"type": "request", "req_id": rid, "cmd": cmd,
                                 "data": data or {}}) + "\n")
        self.f.flush()
        try:
            resp = self._read(lambda m: m.get("type") == "response" and m.get("req_id") == rid)
        except socket.timeout:
            resp = None
        if resp is None:
            raise TestFailure(f"no response to {cmd}")
        return resp

    def call(self, cmd, data=None):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        resp = self.request(cmd, data)
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def ram(self, addr, length):
        """Přečte bajty z regionu User RAM (nezávisle na mapování)."""
        d = self.call("region_read", {"region_id": 1, "offset": addr, "length": length})
        return base64.b64decode(d["data_b64"])

    def close(self):
        """Zabije jen vlastní PID (headless TCP režim se přes MCP neukončuje)."""
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        if self.sock is not None:
            self.sock.close()
        self.log.close()


def _start(exe, tmp, port, extra_args=(), bpt_name=None):
    """Spustí headless emulátor s MCP TCP; s ``bpt_name`` i exec breakpoint."""
    ini = tmp / "emu.ini"
    if bpt_name:
        ini.write_text("[BREAKPOINTS]\nauto_load=1\nauto_save=0\n", encoding="utf-8")
        (tmp / bpt_name).write_text(json.dumps({
            "breakpoints": [{"id": 1, "name": "exec", "addr": _EXEC, "type": "PC_EXEC",
                             "enabled": True, "skip_count": 0, "hit_count": 0,
                             "expr": None, "action": None, "edge_triggered": False}],
            "groups": []}), encoding="utf-8")
    else:
        ini.write_text("", encoding="utf-8")
    port = port or emu_test_proc.free_tcp_port()
    args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
            f"--cfg-dir={tmp}", f"--work-dir={tmp}", f"--config={ini}",
            f"--mcp-tcp-port={port}", *extra_args]
    return TcpEmu(args, tmp, port)


def _wait_paused(emu, what):
    """Počká, až emulace stojí (breakpoint nebo pauza)."""
    deadline = time.monotonic() + 20.0
    while not emu.call("get_state").get("paused"):
        if time.monotonic() > deadline:
            raise TestFailure(f"emulation did not pause ({what})")
        time.sleep(0.1)


def _strip_volatile(obj):
    """Rekurzivně odstraní klíče s časem a čítači (viz _VOLATILE_KEYS)."""
    if isinstance(obj, dict):
        return {k: _strip_volatile(v) for k, v in obj.items()
                if k not in _VOLATILE_KEYS}
    if isinstance(obj, list):
        return [_strip_volatile(v) for v in obj]
    return obj


def _capture(emu, has_pio):
    """Zachytí porovnávaný stav stroje (viz docstring modulu)."""
    regs = dict(emu.call("get_registers"))
    regs["I"] = (regs.pop("IR") >> 8) & 0xFF
    st = {
        "regs": regs,
        "flags": _strip_volatile(emu.call("get_cpu_flags")),
        "i8255": emu.call("get_periph_i8255"),
        "i8253": _strip_volatile(emu.call("get_periph_i8253")),
        "memory_map": emu.call("get_memory_map"),
        "gdg": _strip_volatile(emu.call("get_periph_gdg")),
        "text": emu.call("get_video_text_dump"),
        "ram": {f"{a:04X}": emu.ram(a, n).hex() for a, n in _RAM_RANGES},
    }
    if has_pio:
        st["z80_pio"] = emu.call("get_periph_z80_pio")
        st["psg"] = emu.call("get_periph_sn76489")
    return st


def _diff(a, b, path=""):
    """Vrátí seznam rozdílů dvou JSON struktur (cesta: a != b)."""
    out = []
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            out += _diff(a.get(k), b.get(k), f"{path}.{k}")
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            out += _diff(x, y, f"{path}[{i}]")
    elif a != b:
        out.append(f"{path}: {json.dumps(a)[:70]} != {json.dumps(b)[:70]}")
    return out


def _load_dirty(emu, dirty, has_pio):
    """Zavede špinavý program bez resetu a nechá ho doběhnout do smyčky."""
    emu.call("media_load_mzf", {"path": str(dirty)})
    emu.call("io_write", {"port": 0xE0, "value": 0})
    emu.call("io_write", {"port": 0xE1, "value": 0})
    emu.call("set_register", {"reg": "PC", "value": _EXEC})
    if has_pio:
        # Vektor Z80 PIO kanálu A = 80h (port 0FDh = řízení kanálu A).
        emu.call("io_write", {"port": 0xFD, "value": 0x80})
    emu.call("run", {"frames": 5})
    _wait_paused(emu, "after dirty program")
    pc = emu.call("get_registers")["PC"]
    check(pc == _DIRTY_LOOP, f"dirty program runs (PC {pc:04X}h)")
    check(emu.call("get_cpu_flags")["im"] == 2, "dirty program set IM 2")


def run_platform(name, exe, bpt_name, has_pio, port):
    """Fáze A-D pro jednu binárku."""
    print(f"--- {name}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_media_run_mzf_"))
    emu = None
    try:
        mzf = tmp / "prog.mzf"
        dirty = tmp / "dirty.mzf"
        short = tmp / "short.mzf"
        _make_mzf(mzf, _PROGRAM)
        _make_mzf(dirty, _DIRTY, name=b"DIRTY\r")
        _make_mzf(short, _PROGRAM, name=b"SHORT\r", size=0x100)

        # A: reference --run-mzf, zastaveno exec breakpointem.
        dir_a = tmp / "a"
        dir_a.mkdir()
        emu = _start(exe, dir_a, port, ("--run-mzf", str(mzf)), bpt_name)
        _wait_paused(emu, "exec breakpoint after --run-mzf")
        ref = _capture(emu, has_pio)
        emu.close()
        emu = None
        check(ref["regs"]["PC"] == _EXEC, f"A: --run-mzf stopped at exec ({ref['regs']['PC']:04X}h)")

        # B: špinavý stroj, v pauze media_run_mzf.
        dir_b = tmp / "b"
        dir_b.mkdir()
        emu = _start(exe, dir_b, port)
        emu.call("run", {"frames": 20})
        _wait_paused(emu, "after boot frames")
        _load_dirty(emu, dirty, has_pio)
        check(_diff(ref, _capture(emu, has_pio)) != [], "B: dirty state differs from reference")
        r = emu.call("media_run_mzf", {"path": str(mzf)})
        check(r.get("reset") is True and r.get("exec_addr") == _EXEC
              and r.get("load_addr") == _EXEC and r.get("size") == len(_PROGRAM),
              f"B: media_run_mzf response {json.dumps(r)}")
        check(emu.call("get_state").get("paused"), "B: pause state kept")
        got = _capture(emu, has_pio)
        diffs = _diff(ref, got)
        for d in diffs[:25]:
            print(f"    diff {d}")
        check(not diffs, f"B: state equals --run-mzf reference ({len(diffs)} differences)")

        # C: chybný soubor nesmí stroj resetovat.
        _load_dirty(emu, dirty, has_pio)
        for bad, what in ((tmp / "missing.mzf", "missing file"),
                          (short, "truncated file")):
            resp = emu.request("media_run_mzf", {"path": str(bad)})
            check(not resp.get("success"), f"C: {what} -> error ({resp.get('error')})")
            pc = emu.call("get_registers")["PC"]
            im = emu.call("get_cpu_flags")["im"]
            check(pc == _DIRTY_LOOP and im == 2,
                  f"C: {what} did not reset the machine (PC {pc:04X}h, IM {im})")

        # D: za běhu emulace.
        emu.call("run")
        time.sleep(0.3)
        emu.call("media_run_mzf", {"path": str(mzf)})
        time.sleep(0.3)
        check(not emu.call("get_state").get("paused"), "D: emulation keeps running")
        pc = emu.call("get_registers")["PC"]
        flags = emu.call("get_cpu_flags")
        check(pc == _EXEC and flags["im"] == 1 and not flags["iff1"],
              f"D: program runs after media_run_mzf while running "
              f"(PC {pc:04X}h, IM {flags['im']}, IFF1 {flags['iff1']})")
    finally:
        if emu is not None:
            emu.close()
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=170)
    port = emu_test_proc.mcp_test_port("MZ_MEDIA_RUN_MZF_TEST_PORT")
    found = [(n, _find_exe(n), b, p) for (n, b, p) in _PLATFORMS]
    present = [x for x in found if x[1]]
    if not present:
        print("SKIP: no emulator binary found")
        return 77
    print(f"media_run_mzf state test (port {port or 'auto'})")
    rc = 0
    try:
        missing = [n for (n, e, b, p) in found if not e]
        check(not missing, f"all emulator binaries present (missing: {', '.join(missing) or 'none'})")
        for name, exe, bpt, has_pio in present:
            run_platform(name, exe, bpt, has_pio, port)
        print("RESULT: PASS")
    except TestFailure as e:
        print(f"RESULT: FAIL - {e}")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
