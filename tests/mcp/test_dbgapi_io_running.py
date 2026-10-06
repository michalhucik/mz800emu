#!/usr/bin/env python3
"""E2E regresní test: port I/O a načtení MZF přes MCP za běhu emulace (pipe).

Reprodukuje chybu nahlášenou 3. 10. 2026: ``io_write`` nebo ``io_read`` přes
MCP za běhu emulace zastavil CPU (PC i IR stály) a další port I/O příkaz
zablokoval zpracování příkazů na ~240 s, takže zamrzlo celé MCP. Příčina:
fronta dbgapi se vybírala v callbacku konce snímku, ještě před odečtem délky
snímku, a port I/O přes insideop zpracoval konec snímku podruhé
(``g_gdg.total_elapsed.ticks`` podtekl). Týkalo se i ``media_load_mzf`` +
``io_write`` 0E0h/0E1h, tedy složeného ``emu_media_run_mzf`` z wrapperu.

Pro každou nalezenou binárku (mz700emu-pal, mz700emu-ntsc, mz800emu,
mz1500emu; chybí-li jen některá, test selže) spustí izolovaný proces v pipe
módu (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI, ``--no-save-ini``)
a ověří (A a B reprodukují původní chybu, C a D hlídají novou pozici drainu,
E reprodukuje podtečení účtů insideop v paused smyčce):

  A  ``io_read`` 0CEh, ``io_write`` 0CFh a pak opakovaně ``io_write``
     0E0h a 0E1h: každý příkaz odpoví do _CMD_LIMIT_S, PC nebo IR se mezi
     vzorky dál mění, ``total_ticks`` z ``get_periph_gdg`` je menší než
     délka snímku (bez podtečení) a čítač snímků roste.
  B  složený "run MZF" jako ``emu_media_run_mzf`` ve wrapperu:
     ``media_load_mzf`` + ``io_write`` 0E0h a 0E1h + ``set_register`` PC za
     běhu. Program (DI; smyčka INC A) pak musí běžet: PC je ve smyčce a A se
     mezi vzorky mění. Pak ještě dva ``io_write`` za běhu, oba rychle.
  C  ``run`` s ``frames`` (pauza těsně za koncem snímku), v pauze
     ``io_write``/``io_read``, pak ``run``: CPU běží a čítač tiků
     nepodtekl.
  D  program s I/O ve smyčce (EX (SP),IX; IN A,(0CEh); JR) a za jeho běhu
     _IO_LOOP_WRITES x ``io_write``: odpovědi rychlé, tiky bez podtečení,
     PC zůstává ve smyčce.
  E  pauza (podmíněný PC_EXEC breakpoint na posledním řádku + step_into)
     na NOP po EX (SP),IX těsně před koncem snímku, ``io_write`` v pauze,
     ``run``: CPU běží a MCP odpovídá. Bez úklidu účtů insideop po příkazu
     čítač tiků podtekl a emulace i MCP zamrzly (ověřeno na všech binárkách).

Zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka nenalezena).
"""

import json
import queue
import shutil
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

#: Limit na odpověď jednoho příkazu (s). Chyba dávala ~240 s.
_CMD_LIMIT_S = 3.0

#: Horní mez g_gdg.total_elapsed.ticks (délka snímku je pod 360 000 tiků
#: na všech platformách). Podtečení dávalo hodnoty kolem 2^32.
_TICKS_SANE = 1_000_000

#: Exec adresa testovacího programu.
_EXEC = 0x1200

#: Tělo programu: 1200h DI; 1201h INC A; 1202h JR 1201h.
_PROGRAM = bytes([0xF3, 0x3C, 0x18, 0xFD])

#: Rozsah PC uvnitř smyčky programu.
_LOOP_PCS = (0x1201, 0x1202, 0x1203)

#: Program s I/O ve smyčce: 1200h DI; 1201h EX (SP),IX (23 T);
#: 1203h IN A,(0CEh) (11 T, I/O na konci instrukce); 1205h JR 1201h.
#: Skončí-li snímek po EX (SP),IX, insideop příkazu dbgapi mimo instrukci
#: předplatí 23 T a následující IN je odečte od svých 11 T. Hlídá, že po
#: přesunu drainu za konec snímku čítač tiků přesto nepodteče.
_IO_PROGRAM = bytes([0xF3, 0xDD, 0xE3, 0xDB, 0xCE, 0x18, 0xFA])

#: Počet io_write za běhu programu s I/O ve smyčce (fáze D).
_IO_LOOP_WRITES = 40

#: Program pro fázi E: 1200h DI; 1201h EX (SP),IX (23 T); 1203h NOP (4 T);
#: 1204h JR 1201h (12 T). Pauza na NOP (1203h) znamená, že mezi instrukcemi
#: je cpu->op_tstate = 23 T z EX (SP),IX a další instrukce je krátká.
_PRE_END_PROGRAM = bytes([0xF3, 0xDD, 0xE3, 0x00, 0x18, 0xFB])

#: Adresa NOP v _PRE_END_PROGRAM (pauza na ní = předchozí instrukce 23 T).
_PRE_END_NOP = 0x1203

#: Okno vzdálenosti pauzy od konce snímku v T-stavech CPU (násobí se
#: GDG děličkou cpu_divider z get_platform_info). Port I/O v pauze přičte
#: 23 T předstihu (EX (SP),IX) a překročí konec snímku. Okno je zvolené
#: empiricky: na buildu bez úklidu účtů zamrzlo 12-20 T před koncem
#: snímku (mz700/mz800/mz1500), 21 T a 26 T ne.
_PRE_END_WINDOW_T = (6, 18)

#: Max počet step_into v jednom snímku (od posledního řádku do konce).
_PRE_END_MAX_STEPS = 60

#: Max počet snímků, ve kterých se pozice před koncem snímku hledá.
_PRE_END_FRAMES = 60


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


def _make_mzf(path, body=_PROGRAM):
    """Vytvoří MZF: atribut 01h, fstrt = fexec = 1200h, tělo ``body``."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"IO RUN TEST\r"
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
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
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

    def call(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí (data úspěšné response, doba odezvy v s)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        t0 = time.monotonic()
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        dt = time.monotonic() - t0
        if resp is None:
            # Zaseknuté emu vlákno neodpoví ani na shutdown - close() zabije PID.
            self.dead = True
            rc = self.proc.poll()
            raise TestFailure(f"no response to {cmd} within {timeout:.0f} s "
                              f"(emulator exit code: {rc})")
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}, dt

    def quick(self, cmd, data=None, verbose=True):
        """Příkaz, který musí odpovědět do _CMD_LIMIT_S (jinak FAIL).

        Při ``verbose=False`` se PASS nevypisuje (vzorkování), FAIL ano.
        """
        d, dt = self.call(cmd, data, timeout=_CMD_LIMIT_S + 2.0)
        what = f"{cmd} {json.dumps(data or {})} answered in {dt:.2f} s"
        if verbose or dt > _CMD_LIMIT_S:
            check(dt <= _CMD_LIMIT_S, what)
        return d

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


def _ensure_running(emu):
    """Zajistí běžící emulaci (po startu headless může běžet i stát)."""
    st, _ = emu.call("get_state")
    if st.get("paused"):
        emu.call("run")
    st, _ = emu.call("get_state")
    check(not st.get("paused"), "emulation is running")


def _samples(emu, n=5, gap=0.25):
    """Odebere n vzorků (registry, GDG čítače) s odstupem gap sekund."""
    out = []
    for _ in range(n):
        time.sleep(gap)
        regs = emu.quick("get_registers", verbose=False)
        gdg = emu.quick("get_periph_gdg", verbose=False)
        out.append((regs, gdg))
    return out


def _check_cpu_alive(samples, what):
    """CPU běží: PC nebo IR se mění, tiky bez podtečení, snímky rostou."""
    pcir = {(r["PC"], r["IR"]) for r, _ in samples}
    check(len(pcir) > 1, f"{what}: PC/IR keep changing ({len(pcir)} distinct of {len(samples)})")
    ticks = [g["total_ticks"] for _, g in samples]
    check(max(ticks) < _TICKS_SANE,
          f"{what}: frame tick counter sane (max {max(ticks)} < {_TICKS_SANE})")
    screens = [g["total_screens"] for _, g in samples]
    check(screens[-1] > screens[0], f"{what}: frame counter advances "
                                    f"({screens[0]} -> {screens[-1]})")


def phase_a(exe, tmp):
    """Port I/O za běhu: rychlé odpovědi a CPU dál vykonává instrukce."""
    print("  Phase A: io_read / io_write while running")
    emu = PipeEmu(exe, tmp)
    try:
        _ensure_running(emu)
        time.sleep(1.0)
        emu.quick("io_read", {"port": 0xCE})
        emu.quick("io_write", {"port": 0xCF, "value": 0})
        _check_cpu_alive(_samples(emu), "after io_read 0CEh + io_write 0CFh")
        for _ in range(3):
            emu.quick("io_write", {"port": 0xE0, "value": 0})
            emu.quick("io_write", {"port": 0xE1, "value": 0})
            emu.quick("io_read", {"port": 0xCE})
        _check_cpu_alive(_samples(emu), "after repeated io_write 0E0h/0E1h")
    finally:
        emu.close()


def phase_b(exe, tmp, mzf):
    """Složený "run MZF" za běhu (media_load_mzf + 0E0h/0E1h + PC)."""
    print("  Phase B: media_load_mzf + io_write 0E0h/0E1h + set PC while running")
    emu = PipeEmu(exe, tmp)
    try:
        _ensure_running(emu)
        time.sleep(1.0)
        emu.quick("media_load_mzf", {"path": str(mzf)})
        emu.quick("io_write", {"port": 0xE0, "value": 0})
        emu.quick("io_write", {"port": 0xE1, "value": 0})
        emu.quick("set_register", {"reg": "PC", "value": _EXEC})
        samples = _samples(emu)
        pcs = [r["PC"] for r, _ in samples]
        check(all(pc in _LOOP_PCS for pc in pcs),
              "PC stays in the loaded program loop (" +
              ", ".join(f"{pc:04X}h" for pc in pcs) + ")")
        accs = {r["AF"] >> 8 for r, _ in samples}
        check(len(accs) > 1, f"register A keeps changing ({len(accs)} distinct values)")
        _check_cpu_alive(samples, "after run-MZF sequence")
        emu.quick("io_write", {"port": 0xE0, "value": 0})
        emu.quick("io_write", {"port": 0xE1, "value": 0})
        _check_cpu_alive(_samples(emu, n=3), "after further io_write")
    finally:
        emu.close()


def phase_c(exe, tmp):
    """Port I/O v pauze přesně na hranici snímku (po ``run`` s frames), pak běh.

    Po frame-bounded ``run`` stojí emulace těsně za koncem snímku, kdy je
    ``total_ticks`` malé. Port I/O mimo instrukci tu nesmí rozbít účty
    insideop (čítač tiků nesmí po rozběhu podtéct).
    """
    print("  Phase C: io_write while paused at a frame boundary, then run")
    emu = PipeEmu(exe, tmp)
    try:
        emu.call("run", {"frames": 50})
        st, _ = emu.call("get_state")
        check(st.get("paused"), "emulation paused after run frames=50")
        emu.quick("io_write", {"port": 0xCF, "value": 0})
        emu.quick("io_read", {"port": 0xCE})
        emu.quick("io_write", {"port": 0xCF, "value": 0})
        emu.quick("run")
        _check_cpu_alive(_samples(emu, n=4), "after io in pause + run")
    finally:
        emu.close()


def phase_d(exe, tmp, mzf_io):
    """Mnoho io_write za běhu programu, který sám dělá I/O ve smyčce."""
    print(f"  Phase D: {_IO_LOOP_WRITES}x io_write while an I/O loop program runs")
    emu = PipeEmu(exe, tmp)
    try:
        _ensure_running(emu)
        time.sleep(1.0)
        emu.quick("media_load_mzf", {"path": str(mzf_io)})
        emu.quick("io_write", {"port": 0xE0, "value": 0})
        emu.quick("io_write", {"port": 0xE1, "value": 0})
        emu.quick("set_register", {"reg": "PC", "value": _EXEC})
        worst = 0.0
        for _ in range(_IO_LOOP_WRITES):
            _, dt = emu.call("io_write", {"port": 0xCF, "value": 0},
                             timeout=_CMD_LIMIT_S + 2.0)
            worst = max(worst, dt)
            gdg = emu.quick("get_periph_gdg", verbose=False)
            if gdg["total_ticks"] >= _TICKS_SANE:
                check(False, f"frame tick counter sane after io_write "
                             f"({gdg['total_ticks']} < {_TICKS_SANE})")
        check(worst <= _CMD_LIMIT_S,
              f"{_IO_LOOP_WRITES}x io_write answered, worst {worst:.2f} s")
        samples = _samples(emu, n=4)
        pcs = [r["PC"] for r, _ in samples]
        check(all(0x1201 <= pc <= 0x1206 for pc in pcs),
              "PC stays in the I/O loop (" + ", ".join(f"{pc:04X}h" for pc in pcs) + ")")
        _check_cpu_alive(samples, "after io_write during I/O loop")
    finally:
        emu.close()


def _gdg_ticks(emu):
    """Vrátí g_gdg.total_elapsed.ticks (get_periph_gdg) bez výpisu."""
    return emu.quick("get_periph_gdg", verbose=False)["total_ticks"]


def phase_e(exe, tmp, mzf_pre):
    """Port I/O v pauze těsně před koncem snímku, pak běh.

    Pauza na NOP po EX (SP),IX ve vzdálenosti _PRE_END_WINDOW_T T-stavů před
    koncem snímku. io_write v pauze (drain paused smyčky) volá insideop:
    ta přičte předstih 23 T, překročí konec snímku (odečte délku snímku)
    a do instruction_insideop_sync_ticks zapíše 23 T v tikách. Bez úklidu účtů
    po příkazu odečte NOP po rozběhu víc tiků, než v čítači zbylo, a
    unsigned čítač podteče (stejná ~240 s zamrzlost jako původní chyba).
    """
    print("  Phase E: io_write while paused just before frame end, then run")
    emu = PipeEmu(exe, tmp)
    try:
        info, _ = emu.call("get_platform_info")
        frame = info["scanline"]["screen_total_ticks_per_frame"]
        lines = info["scanline"]["screen_total_height_lines"]
        emu.call("run", {"frames": 50})
        emu.quick("media_load_mzf", {"path": str(mzf_pre)})
        emu.quick("io_write", {"port": 0xE0, "value": 0})
        emu.quick("io_write", {"port": 0xE1, "value": 0})
        emu.quick("set_register", {"reg": "PC", "value": _EXEC})
        # Na poslední řádek snímku dojede podmíněný PC_EXEC breakpoint na NOP
        # (run_until_raster krokuje přes MCP a je na to příliš pomalý).
        emu.call("bp_create_with_init", {"fields": ["addr", "expr"], "addr": _PRE_END_NOP,
                                         "expr": f"Scanline == {lines - 1}"})
        div = info["clocks"]["cpu_divider"]
        lo = _PRE_END_WINDOW_T[0] * div + 1
        hi = _PRE_END_WINDOW_T[1] * div - 1
        found = None
        # Fáze smyčky vůči konci snímku se mezi snímky posouvá (délka snímku
        # není násobkem délky smyčky), proto se zkouší několik snímků.
        for _ in range(_PRE_END_FRAMES):
            emu.call("run")
            deadline = time.monotonic() + 10.0
            while not emu.call("get_state")[0].get("paused"):
                check(time.monotonic() < deadline, "breakpoint on the last frame line was hit")
                time.sleep(0.05)
            prev = _gdg_ticks(emu)
            for _ in range(_PRE_END_MAX_STEPS):
                emu.call("step_into")
                pc = emu.quick("get_registers", verbose=False)["PC"]
                ticks = _gdg_ticks(emu)
                if ticks < prev:
                    break  # snímek skončil, zkusit další
                prev = ticks
                dist = frame - ticks
                if pc == _PRE_END_NOP and lo <= dist <= hi:
                    found = dist
                    break
            if found is not None:
                break
        check(found is not None,
              f"paused on NOP {_PRE_END_NOP:04X}h {lo}-{hi} ticks before frame end")
        emu.call("bp_clear")
        print(f"  info: {found} ticks before frame end (frame {frame} ticks)")
        emu.quick("io_write", {"port": 0xCF, "value": 0})
        emu.quick("run")
        samples = _samples(emu, n=4)
        _check_cpu_alive(samples, "after io_write paused before frame end + run")
    finally:
        emu.close()


def run_platform(exe, platform):
    """Všechny fáze na jedné platformě, každá v čerstvém procesu."""
    print(f"--- {platform} ({exe.name})")
    tmp = Path(tempfile.mkdtemp(prefix="mz_dbgapi_io_"))
    try:
        mzf = tmp / "io_run_test.mzf"
        _make_mzf(mzf)
        mzf_io = tmp / "io_loop_test.mzf"
        _make_mzf(mzf_io, _IO_PROGRAM)
        phase_a(exe, tmp)
        phase_b(exe, tmp, mzf)
        phase_c(exe, tmp)
        phase_d(exe, tmp, mzf_io)
        mzf_pre = tmp / "pre_end_test.mzf"
        _make_mzf(mzf_pre, _PRE_END_PROGRAM)
        phase_e(exe, tmp, mzf_pre)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=285)
    found = [(n, _find_exe(n), p) for (n, p) in _PLATFORMS]
    present = [x for x in found if x[1]]
    if not present:
        print("SKIP: no emulator binary found")
        return 77
    print("MCP port I/O and MZF load while running (pipe)")
    rc = 0
    try:
        missing = [n for (n, e, p) in found if not e]
        check(not missing, f"all emulator binaries present (missing: {', '.join(missing) or 'none'})")
        for name, exe, platform in present:
            run_platform(exe, platform)
        print("RESULT: PASS")
    except TestFailure as e:
        print(f"RESULT: FAIL - {e}")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
