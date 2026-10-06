#!/usr/bin/env python3
"""Test stavu stroje po CLI ``--run-mzf`` (headless + MCP TCP).

Kontrakt ``--run-mzf``: zkratka, která nahraje MZF ve stavu, v jakém by ho
spustila ROM (reset -> IPL/monitor -> načtení z pásky -> skok na exec).
Očekávané hodnoty jsou změřené v emulátoru na exec adrese po zavedení
z pásky přes ROM (CMT hack vypnutý) a doložené adresami v ROM uvedenými
u jednotlivých kontrol.

Pro binárky mz700emu-pal, mz700emu-ntsc, mz800emu a mz1500emu v kořeni repa
vytvoří malé MZF (fstrt = fexec = 1200h, tělo ``JR $``), spustí ho přes
``--run-mzf`` v izolovaném prostředí (vlastní dočasný ``--cfg-dir``,
``--no-save-ini``) a PC_EXEC breakpointem na 1200h načteným z ``.bpt``
zastaví CPU před první instrukcí programu. Pak ověří:

  * všechny platformy: IM 1 (004Dh / E814h / E813h), DI, SP = 10F0h,
    trampolína 1038h = JP 038Dh (0085h / E88Bh / E858h), TEMPO (119Eh) = 4,
    BPFLG (119Dh) = 1, znak D000h = 00h a atributy D800h-DFFFh = 71h
    (CLS a FILLA), CTC0 mode 3 s předvolbou tónu z BEEP (0577h, tabulka not
    027Ch: 04ECh, na MZ-1500 03F8h),
  * MZ-700: HL = exec (012Eh JP (HL)), CTC1 neprogramovaný (monitor 1Z-013A
    nevolá TIMST),
  * MZ-800: BC = 0100h (E99Dh), IX = exec (ED41h JP (IX)), PC0 = 1 (E86Ch),
    CTC1 předvolba 3CFBh (TIMST 033Eh), CTC2 bez latche (TIMST čte E006h
    na 0331h), PSG ztišené (útlum 15 na všech kanálech),
  * MZ-1500: BC = 0100h (E9E6h), HL = exec (E9ECh JP (HL)), PC0 = 1 (E83Ah),
    CTC1 předvolba 3D54h, CTC2 bez latche, paleta i -> i (E881h), PSG
    ztišené.

Druhý běh na každé platformě kontroluje pořadí "inicializace před nahráním":
MZF s tělem od 1000h do 1201h (přes hlavičku na 10F0h a pracovní oblast
monitoru 10F1h-11EFh) musí mít data 1000h-11FFh po bootstrapu neporušená.

Chybí-li některá z binárek, zatímco jiné existují, test selže (neúplný
build). Bez jediné binárky vrací SKIP.

Port MCP TCP: proměnná ``MZ_RUN_MZF_TEST_PORT`` (23800 je vyhrazen pro
běžné instance); bez ní dostane každý spuštěný emulátor volný port od systému,
takže souběžné běhy testu se nesrazí. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka nenalezena).
"""

import base64
import json
import os
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

#: Exec adresa testovacího programu (= fstrt základního MZF).
_EXEC = 0x1200

#: Začátek těla MZF pro kontrolu pořadí (přes hlavičku a pracovní oblast).
_OVERLAP_START = 0x1000

#: (jméno binárky, jméno .bpt souboru podle architektury, platforma)
_PLATFORMS = [
    ("mz700emu-pal", "mz700-breakpoints.bpt", "mz700"),
    ("mz700emu-ntsc", "mz700-breakpoints.bpt", "mz700"),
    ("mz800emu", "mz800-breakpoints.bpt", "mz800"),
    ("mz1500emu", "mz1500-breakpoints.bpt", "mz1500"),
]

#: Předvolba CTC0 po BEEP podle platformy (tabulka not ROM, 027Ch).
_BEEP_COUNT = {"mz700": 0x04EC, "mz800": 0x04EC, "mz1500": 0x03F8}


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


def _overlap_pattern():
    """Vzorek dat 1000h-11FFh pro kontrolu pořadí (nenulové bajty)."""
    return bytes((((i * 7) + 3) & 0xFF) or 0x5A for i in range(_EXEC - _OVERLAP_START))


def _make_mzf(path, overlap=False):
    """Vytvoří MZF: atribut 01h, fexec = 1200h, na 1200h ``JR $`` (18h FEh).

    Při ``overlap`` začíná tělo na 1000h a před ``JR $`` nese vzorek
    _overlap_pattern() přes hlavičku (10F0h) a pracovní oblast monitoru.
    """
    fstrt = _OVERLAP_START if overlap else _EXEC
    body = (_overlap_pattern() if overlap else b"") + bytes([0x18, 0xFE])
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"RUNMZF TEST\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = fstrt.to_bytes(2, "little")
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

    def call(self, cmd, data=None):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
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
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def mem(self, addr, length=1):
        """Přečte bajty z pohledu CPU (aktuální mapování)."""
        d = self.call("mem_read", {"addr": addr, "len": length})
        return base64.b64decode(d["data_b64"])

    def close(self):
        """Zabije jen vlastní PID (headless TCP režim se přes MCP neukončuje)."""
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        if self.sock is not None:
            self.sock.close()
        self.log.close()


def _start(exe, bpt_name, port, tmp, overlap):
    """Spustí ``--run-mzf`` s exec breakpointem a počká na jeho zásah.

    Vrátí běžící TcpEmu (CPU zastavené na exec adrese); volající ho zavře.
    """
    mzf = tmp / "runmzf_test.mzf"
    _make_mzf(mzf, overlap)
    ini = tmp / "emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load=1\nauto_save=0\n", encoding="utf-8")
    (tmp / bpt_name).write_text(json.dumps({
        "breakpoints": [{"id": 1, "name": "exec", "addr": _EXEC, "type": "PC_EXEC",
                         "enabled": True, "skip_count": 0, "hit_count": 0,
                         "expr": None, "action": None, "edge_triggered": False}],
        "groups": []}), encoding="utf-8")
    # port None = volný port přidělený systémem (souběžné běhy testů).
    port = port or emu_test_proc.free_tcp_port()
    args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
            f"--cfg-dir={tmp}", f"--work-dir={tmp}", f"--config={ini}",
            f"--mcp-tcp-port={port}", "--run-mzf", str(mzf)]
    emu = TcpEmu(args, tmp, port)
    deadline = time.monotonic() + 20.0
    while not emu.call("get_state").get("paused"):
        if time.monotonic() > deadline:
            emu.close()
            raise TestFailure("exec breakpoint was not hit")
        time.sleep(0.1)
    return emu


def run_overlap(exe, bpt_name, platform, port):
    """Kontrola pořadí: tělo MZF přes 10F0h-11EFh musí po bootstrapu zůstat celé."""
    tmp = Path(tempfile.mkdtemp(prefix="mz_run_mzf_ovl_"))
    emu = None
    try:
        emu = _start(exe, bpt_name, port, tmp, True)
        want = _overlap_pattern()
        got = emu.mem(_OVERLAP_START, len(want))
        bad = [i for i in range(len(want)) if got[i] != want[i]]
        first = f"{_OVERLAP_START + bad[0]:04X}h" if bad else "none"
        check(not bad, f"MZF data {_OVERLAP_START:04X}h-{_EXEC - 1:04X}h intact after "
                       f"bootstrap (init before load), first mismatch: {first}")
    finally:
        if emu is not None:
            emu.close()
        shutil.rmtree(tmp, ignore_errors=True)


def run_platform(exe, bpt_name, platform, port):
    """Spustí ``--run-mzf`` na jedné platformě a ověří stav na exec adrese."""
    print(f"--- {platform} ({exe.name})")
    tmp = Path(tempfile.mkdtemp(prefix="mz_run_mzf_"))
    emu = None
    try:
        emu = _start(exe, bpt_name, port, tmp, False)

        regs = emu.call("get_registers")
        flags = emu.call("get_cpu_flags")
        check(regs["PC"] == _EXEC, f"PC = {regs['PC']:04X}h (exec {_EXEC:04X}h)")
        check(flags["im"] == 1, f"IM = {flags['im']} (ROM sets IM 1)")
        check(not flags["iff1"] and not flags["iff2"], "interrupts disabled (IFF1 = IFF2 = 0)")
        check(regs["SP"] == 0x10F0, f"SP = {regs['SP']:04X}h (expected 10F0h)")
        check(emu.mem(0x1038, 3) == bytes([0xC3, 0x8D, 0x03]), "1038h = JP 038Dh (INTSRQ)")
        check(emu.mem(0x119E)[0] == 0x04, "TEMPO (119Eh) = 4")
        check(emu.mem(0x119D)[0] == 0x01, "BPFLG (119Dh) = 1")
        check(emu.mem(0xD000)[0] == 0x00, "text VRAM D000h cleared (00h)")
        check(emu.mem(0xD800)[0] == 0x71 and emu.mem(0xDFFF)[0] == 0x71,
              "attribute VRAM D800h-DFFFh = 71h")
        ctc = emu.call("get_periph_i8253")["channels"]
        check(ctc[0]["mode"] == "mode3", f"CTC0 {ctc[0]['mode']} (MSTP sets mode 3)")
        want_beep = _BEEP_COUNT[platform]
        check(ctc[0]["preset_value"] == want_beep,
              f"CTC0 count {ctc[0]['preset_value']:04X}h loaded by BEEP (ROM {want_beep:04X}h)")

        if platform == "mz700":
            check(regs["HL"] == _EXEC, f"HL = {regs['HL']:04X}h (monitor JP (HL))")
            check(ctc[1]["mode"] == "mode0" and ctc[1]["preset_value"] == 0xFFFF,
                  "CTC1 not programmed (1Z-013A does not call TIMST)")
        else:
            check(regs["BC"] == 0x0100, f"BC = {regs['BC']:04X}h (IPL device code 0100h)")
            if platform == "mz800":
                check(regs["IX"] == _EXEC, f"IX = {regs['IX']:04X}h (IPL JP (IX))")
                want_ctc1 = 0x3CFB
            else:
                check(regs["HL"] == _EXEC, f"HL = {regs['HL']:04X}h (IPL JP (HL))")
                want_ctc1 = 0x3D54
            check(ctc[1]["mode"] == "mode2" and ctc[1]["preset_value"] == want_ctc1,
                  f"CTC1 mode2 preset {ctc[1]['preset_value']:04X}h (TIMST {want_ctc1:04X}h)")
            check(ctc[2]["preset_value"] == 0xA8C0, "CTC2 preset A8C0h (TIMST)")
            check(not ctc[2]["latch_op"], "CTC2 not latched (TIMST reads E006h at 0331h)")
            pio = emu.call("get_periph_i8255")
            check(pio["signal_pc00"] == 1, "8255 PC0 = 1 (IPL BSR 01h, CTC0 audio gate)")
            check(pio["signal_pc02"] == 1 and pio["signal_pc03"] == 1, "8255 PC2 = PC3 = 1")
            psg = emu.call("get_periph_sn76489")
            atts = [c["attenuation"] for c in psg["psg0"]["channels"]]
            check(atts == [15, 15, 15, 15], f"PSG silenced (attenuation {atts})")
            if platform == "mz1500":
                pal = emu.call("get_periph_gdg").get("palette")
                check(pal == list(range(8)), f"MZ-1500 palette {pal} (IPL E881h: i -> i)")
    finally:
        if emu is not None:
            emu.close()
        shutil.rmtree(tmp, ignore_errors=True)
    run_overlap(exe, bpt_name, platform, port)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    # Pevný port jen z proměnné prostředí; jinak si každý spuštěný emulátor
    # vezme volný port (souběžné běhy testů se nesrazí).
    port = emu_test_proc.mcp_test_port("MZ_RUN_MZF_TEST_PORT")
    found = [(n, _find_exe(n), b, p) for (n, b, p) in _PLATFORMS]
    present = [x for x in found if x[1]]
    if not present:
        print("SKIP: no emulator binary found")
        return 77
    print(f"--run-mzf machine state test (port {port or 'auto'})")
    rc = 0
    try:
        missing = [n for (n, e, b, p) in found if not e]
        check(not missing, f"all emulator binaries present (missing: {', '.join(missing) or 'none'})")
        for name, exe, bpt, platform in present:
            run_platform(exe, bpt, platform, port)
        print("RESULT: PASS")
    except TestFailure as e:
        print(f"RESULT: FAIL - {e}")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
