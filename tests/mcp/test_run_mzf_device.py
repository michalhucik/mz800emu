#!/usr/bin/env python3
"""E2E test kódu zařízení v BC při přímém spuštění MZF (``[CMT] run_mzf_device``).

Program zavedený z média dostane od ROM v BC kód zařízení (báze
mz800-knowledge ``conventions/memory-layout-user-prog.md``, "BC kódy
zařízení": RD 0000h, CMT 0100h, FD 0200h, QD 0300h); po zavedení z kazety
nechá ROM MZ-800 a MZ-1500 BC = 0100h a monitor MZ-700 1Z-013A BC = 0000h
(``firmware/stav-po-zavedeni-z-cmt.md``). Přímé spuštění MZF (bootstrap)
hodnotu nastavuje podle volby: jméno ``cmt`` (výchozí), ``rd`` / ``sram``,
``fd``, ``qd`` nebo číslo 0..65535 (``512``, ``0x0200``, ``0200h``).

Jeden běh = jedna binárka (argument: mz800emu, mz700emu-pal, mz700emu-ntsc,
mz1500emu; ctest ``mcp_run_mzf_device_<binárka>``):

  P  pipe (izolovaný ``--cfg-dir``, vlastní INI): ``settings_get
     CMT/run_mzf_device`` = ``cmt`` (výchozí, typ text); ``media_run_mzf``
     bez parametru = BC výchozího CMT platformy a odpověď nese ``device``
     a ``bc``; s parametrem ``device`` (jména i velikost písmen, desítkově,
     ``0x``, přípona ``h``, celé číslo JSON) = BC podle tabulky, nastavení
     se nemění; neplatný ``device`` (neznámé jméno, prázdný, mimo rozsah,
     záporné, s mezerou uprostřed, desetinné číslo JSON) = chyba a stroj se
     nezresetuje (PC beze změny); ``device`` u ``media_load_mzf`` = chyba;
     ``media_load_mzf`` BC nemění; ``settings_set`` (``FD``, ``0X00ff``)
     uloží normalizovaný zápis, ``settings_get`` vrátí ``value`` i ``live``
     a další ``media_run_mzf`` bez parametru ho použije; neplatný zápis
     ``settings_set`` odmítne a nastavení nechá;
     INI ``run_mzf_device`` s neplatnou hodnotou = výchozí ``cmt``;
     po ``settings_set`` a ``shutdown`` s ukládáním INI je v INI nová hodnota.
  C  CLI (headless + MCP TCP, exec breakpoint z ``.bpt``): ``--run-mzf``
     s INI ``run_mzf_device = qd`` dá BC 0300h; ``--run-mzf-device fd``
     přebije INI (BC 0200h) a nastavení (``settings_get``) nechá ``qd``;
     ``--run-mzf-device 0x1234`` = BC 1234h.
  F  chybné CLI: ``--run-mzf-device`` bez ``--run-mzf`` a s neplatnou
     hodnotou skončí nenulovým kódem a anglickou zprávou; desítkové číslo
     s úvodní nulou (``0200``) se odmítne s radou o šestnáctkovém zápisu
     (stejně MCP ``device`` a ``settings_set``).

Port MCP TCP: volný od systému (nikdy 23800). Zabíjí se jen vlastní PID.
Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL (i chybějící binárka; sestavují se všechny
čtyři).
"""

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
from test_run_mzf_state import TcpEmu, TestFailure  # noqa: E402

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent

#: Binárky: jméno -> (platforma 700 / 800 / 1500, jméno .bpt souboru).
_MACHINES = {
    "mz800emu": (800, "mz800-breakpoints.bpt"),
    "mz700emu-pal": (700, "mz700-breakpoints.bpt"),
    "mz700emu-ntsc": (700, "mz700-breakpoints.bpt"),
    "mz1500emu": (1500, "mz1500-breakpoints.bpt"),
}

#: Exec adresa testovacího MZF (tělo ``JR $``).
_EXEC = 0x1200


def _expected(name, platform):
    """Očekávaný BC pro jméno zařízení (tabulka bootstrap_device.h)."""
    if name == "cmt":
        return 0x0000 if platform == 700 else 0x0100
    return {"rd": 0x0000, "sram": 0x0000, "fd": 0x0200, "qd": 0x0300}[name]


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


def _make_mzf(path):
    """MZF: typ 01h, fstrt = fexec = 1200h, tělo ``JR $``."""
    body = bytes([0x18, 0xFE])
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"BC DEVICE TEST\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = _EXEC.to_bytes(2, "little")
    hdr[0x16:0x18] = _EXEC.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, ini, save_ini=False):
        args = [str(exe), "--mcp-pipe", "--headless", "--no-first-run-windows",
                f"--cfg-dir={cfg_dir}", f"--work-dir={cfg_dir}", f"--config={ini}"]
        if not save_ini:
            args.insert(3, "--no-save-ini")
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

    def call_raw(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí celou response (i neúspěšnou)."""
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

    def call(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        resp = self.call_raw(cmd, data, timeout)
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def close(self):
        """Ukončí proces přes shutdown, jinak zabije vlastní PID."""
        try:
            self.call("shutdown", timeout=10.0)
        except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
            pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()


def _bc(emu):
    """Aktuální BC (get_registers)."""
    return emu.call("get_registers")["BC"]


def _run(emu, mzf, device=None):
    """media_run_mzf (s device, je-li zadané); vrátí (odpověď, BC po spuštění)."""
    data = {"path": str(mzf)}
    if device is not None:
        data["device"] = device
    res = emu.call("media_run_mzf", data)
    return res, _bc(emu)


def phase_pipe(name, exe, platform, tmp):
    """P: media_run_mzf s parametrem i bez, settings_get / settings_set, INI."""
    print(f"Phase P ({name}): media_run_mzf device and CMT/run_mzf_device")
    cfg = tmp / "p"
    cfg.mkdir()
    mzf = cfg / "dev.mzf"
    _make_mzf(mzf)
    ini = cfg / "emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
    emu = PipeEmu(exe, cfg, ini)
    try:
        st = emu.call("settings_get", {"key": "CMT/run_mzf_device"})
        check(st.get("value") == "cmt" and st.get("type") == "text",
              f"default setting is cmt, type text ({st})")
        want_cmt = _expected("cmt", platform)

        res, bc = _run(emu, mzf)
        check(bc == want_cmt, f"default start: BC = {bc:04X}h (CMT {want_cmt:04X}h)")
        check(res.get("device") == "cmt" and res.get("bc") == want_cmt,
              f"response carries device and bc ({res.get('device')}, {res.get('bc')})")

        cases = [("rd", _expected("rd", platform)), ("SRAM", 0x0000),
                 ("cmt", want_cmt), ("FD", 0x0200), ("qd", 0x0300), ("Qd", 0x0300),
                 ("0x1234", 0x1234), ("4660", 0x1234), ("1234h", 0x1234),
                 ("0FFFFh", 0xFFFF), ("65535", 0xFFFF), ("0", 0x0000),
                 (" 0X00ab ", 0x00AB), (512, 0x0200)]
        for dev, want in cases:
            res, bc = _run(emu, mzf, dev)
            check(bc == want and res.get("bc") == want,
                  f"device {dev!r}: BC = {bc:04X}h (expected {want:04X}h)")
        res, _ = _run(emu, mzf, "SRAM")
        check(res.get("device") == "sram", f"device name is normalised to lower case ({res.get('device')})")
        res, _ = _run(emu, mzf, " 0X00ab ")
        check(res.get("device") == "0X00ab", f"number keeps its notation, trimmed ({res.get('device')})")
        st = emu.call("settings_get", {"key": "CMT/run_mzf_device"})
        check(st.get("value") == "cmt", f"the device parameter does not change the setting ({st.get('value')})")

        # neplatné device: chyba, stroj se nezresetuje (PC beze změny v pauze)
        emu.call("pause")
        emu.call("set_register", {"reg": "PC", "value": 0x4321})
        for bad in ("xyz", "", "0x10000", "65536", "-1", "+5", "0x", "h", "fd x", "1 2",
                    "12ab", "ABh", 70000, -1, 1.5, True, "0200", "00"):
            resp = emu.call_raw("media_run_mzf", {"path": str(mzf), "device": bad})
            check(not resp.get("success") and "device" in (resp.get("error") or "").lower(),
                  f"invalid device {bad!r} refused ({resp.get('error')})")
        resp = emu.call_raw("media_run_mzf", {"path": str(mzf), "device": "0200"})
        check("0x0200 or 0200h" in (resp.get("error") or ""),
              f"a leading zero is refused with advice on hexadecimal ({resp.get('error')})")
        check(emu.call("get_registers")["PC"] == 0x4321, "an invalid device does not reset the machine")
        resp = emu.call_raw("media_load_mzf", {"path": str(mzf), "device": "fd"})
        check(not resp.get("success"), f"media_load_mzf refuses device ({resp.get('error')})")

        # media_load_mzf (bez resetu) BC nemění
        emu.call("set_register", {"reg": "BC", "value": 0xBEEF})
        emu.call("media_load_mzf", {"path": str(mzf)})
        check(_bc(emu) == 0xBEEF, "media_load_mzf keeps BC")

        # settings_set: normalizovaný zápis, live, použije ho další start
        res = emu.call("settings_set", {"key": "CMT/run_mzf_device", "value": " FD "})
        check(res.get("previous_value") == "cmt" and res.get("new_value") == "fd",
              f"settings_set returns the previous and the stored (normalised) value ({res})")
        st = emu.call("settings_get", {"key": "CMT/run_mzf_device"})
        check(st.get("value") == "fd" and st.get("live") == "fd", f"setting stored as fd ({st})")
        res, bc = _run(emu, mzf)
        check(bc == 0x0200 and res.get("device") == "fd", f"start after settings_set fd: BC = {bc:04X}h")
        emu.call("settings_set", {"key": "CMT/run_mzf_device", "value": "0X00ff"})
        res, bc = _run(emu, mzf)
        check(bc == 0x00FF and res.get("device") == "0X00ff", f"setting 0X00ff: BC = {bc:04X}h ({res.get('device')})")
        for bad in ("bogus", "", "0x1FFFF", "-3", "0200"):
            resp = emu.call_raw("settings_set", {"key": "CMT/run_mzf_device", "value": bad})
            check(not resp.get("success"), f"settings_set {bad!r} refused ({resp.get('error')})")
        resp = emu.call_raw("settings_set", {"key": "CMT/run_mzf_device", "value": "0200"})
        check("0x0200 or 0200h" in (resp.get("error") or ""),
              f"settings_set explains the leading zero ({resp.get('error')})")
        st = emu.call("settings_get", {"key": "CMT/run_mzf_device"})
        check(st.get("value") == "0X00ff" and st.get("live") == "0X00ff",
              f"refused values keep the setting ({st})")
    finally:
        emu.close()

    # INI s neplatnou hodnotou: výchozí cmt
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n[CMT]\nrun_mzf_device = nonsense\n",
                   encoding="utf-8")
    emu = PipeEmu(exe, cfg, ini)
    try:
        st = emu.call("settings_get", {"key": "CMT/run_mzf_device"})
        check(st.get("live") == "cmt", f"invalid INI value falls back to cmt ({st})")
        _, bc = _run(emu, mzf)
        check(bc == want_cmt, f"invalid INI value: BC = {bc:04X}h (CMT)")
    finally:
        emu.close()

    # INI se uloží s hodnotou ze settings_set
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
    emu = PipeEmu(exe, cfg, ini, save_ini=True)
    try:
        emu.call("settings_set", {"key": "CMT/run_mzf_device", "value": "QD"})
    finally:
        emu.close()
    text = ini.read_text(encoding="utf-8", errors="replace")
    section = text.split("[CMT]", 1)[1].split("\n[", 1)[0] if "[CMT]" in text else ""
    vals = [ln.split("=", 1)[1].strip() for ln in section.splitlines()
            if ln.split("=", 1)[0].strip() == "run_mzf_device"]
    check(vals == ["qd"], f"saved INI has [CMT] run_mzf_device = qd ({vals})")


def _cli_start(exe, bpt_name, tmp, label, ini_extra, extra_args):
    """--run-mzf s exec breakpointem; vrátí TcpEmu zastavený na exec adrese."""
    d = tmp / label
    d.mkdir()
    mzf = d / "dev.mzf"
    _make_mzf(mzf)
    ini = d / "emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load=1\nauto_save=0\n" + ini_extra, encoding="utf-8")
    (d / bpt_name).write_text(json.dumps({
        "breakpoints": [{"id": 1, "name": "exec", "addr": _EXEC, "type": "PC_EXEC",
                         "enabled": True, "skip_count": 0, "hit_count": 0,
                         "expr": None, "action": None, "edge_triggered": False}],
        "groups": []}), encoding="utf-8")
    port = emu_test_proc.free_tcp_port()
    args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
            f"--cfg-dir={d}", f"--work-dir={d}", f"--config={ini}",
            f"--mcp-tcp-port={port}", "--run-mzf", str(mzf), *extra_args]
    emu = TcpEmu(args, d, port)
    deadline = time.monotonic() + 20.0
    while not emu.call("get_state").get("paused"):
        if time.monotonic() > deadline:
            emu.close()
            raise TestFailure("exec breakpoint was not hit")
        time.sleep(0.1)
    return emu


def phase_cli(name, exe, platform, bpt_name, tmp):
    """C: --run-mzf s nastavením z INI a s --run-mzf-device."""
    print(f"Phase C ({name}): --run-mzf with the INI setting and --run-mzf-device")
    for label, ini_extra, extra, want, setting in (
            ("c1", "[CMT]\nrun_mzf_device = qd\n", [], 0x0300, "qd"),
            ("c2", "[CMT]\nrun_mzf_device = qd\n", ["--run-mzf-device", "fd"], 0x0200, "qd"),
            ("c3", "", ["--run-mzf-device=0x1234"], 0x1234, "cmt"),
            ("c4", "", [], _expected("cmt", platform), "cmt")):
        emu = _cli_start(exe, bpt_name, tmp, label, ini_extra, extra)
        try:
            regs = emu.call("get_registers")
            check(regs["PC"] == _EXEC and regs["BC"] == want,
                  f"{label} {' '.join(extra) or '(no option)'}: BC = {regs['BC']:04X}h (expected {want:04X}h)")
            st = emu.call("settings_get", {"key": "CMT/run_mzf_device"})
            check(st.get("live") == setting, f"{label}: setting stays {setting} ({st.get('live')})")
        finally:
            emu.close()


def _run_invalid(exe, tmp, args, expect):
    """F: spuštění s chybnou volbou musí skončit chybou se zprávou."""
    d = tmp / "f"
    d.mkdir(exist_ok=True)
    ini = d / "emu.ini"
    ini.write_text("", encoding="utf-8")
    cmd = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
           f"--cfg-dir={d}", f"--work-dir={d}", f"--config={ini}", *args]
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding="utf-8", errors="replace",
                           timeout=20, cwd=str(_REPO_ROOT))
    except subprocess.TimeoutExpired:
        raise TestFailure(f"F: {' '.join(args)}: emulator did not exit")
    shown = " ".join(a if not a.endswith(".mzf") else Path(a).name for a in args)
    check(r.returncode != 0 and expect in r.stdout,
          f"F: {shown} -> exit {r.returncode}, message {expect!r}")


def phase_invalid(exe, tmp):
    """F: chybné použití --run-mzf-device."""
    print("Phase F: invalid --run-mzf-device")
    d = tmp / "f"
    d.mkdir(exist_ok=True)
    mzf = d / "dev.mzf"
    _make_mzf(mzf)
    _run_invalid(exe, tmp, ["--run-mzf-device", "fd"], "--run-mzf-device requires --run-mzf")
    for bad in ("floppy", "0x10000", "-1"):
        _run_invalid(exe, tmp, ["--run-mzf", str(mzf), f"--run-mzf-device={bad}"],
                     "--run-mzf-device: invalid value")
    _run_invalid(exe, tmp, ["--run-mzf", str(mzf), "--run-mzf-device=0200"],
                 "write hexadecimal as 0x0200 or 0200h")


def main():
    emu_test_proc.install(deadline_s=170)
    if len(sys.argv) != 2 or sys.argv[1] not in _MACHINES:
        print(f"FAIL: usage: {Path(sys.argv[0]).name} <{'|'.join(_MACHINES)}>")
        return 1
    name = sys.argv[1]
    platform, bpt_name = _MACHINES[name]
    exe = _find_exe(name)
    if exe is None:
        print(f"FAIL: missing binary {name}")
        return 1
    with tempfile.TemporaryDirectory(prefix="mz_run_mzf_device_") as td:
        tmp = Path(td)
        try:
            phase_pipe(name, exe, platform, tmp)
            phase_cli(name, exe, platform, bpt_name, tmp)
            phase_invalid(exe, tmp)
        except TestFailure as exc:
            print(f"  FAIL: {exc}")
            print("FAILED")
            return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
