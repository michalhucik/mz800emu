#!/usr/bin/env python3
"""E2E regresní test: MCP odpoví v omezeném čase i při zaseknutém emu vlákně.

Dřív: příkaz, který emu vlákno převzalo z fronty dbgapi, se čekal bez
limitu přímo na transportním vlákně (pipe: mcp-stdin-reader). Když se emu
vlákno uprostřed příkazu zaseklo, MCP neodpovídalo vůbec (ani ``ping``)
a klient dostal jen vlastní timeout. Teď dispatch běží na pracovním vlákně
(src/emulator/mcp/dispatch_runner.c) a po 10 s (fronta) + 10 s (rozpracovaný
příkaz) přijde chyba ``Emulator busy: command still running ...``.

Zaseknutí simuluje testovací háček v emulátoru (proměnné prostředí
``MZ800EMU_TEST_STALL_MCP_CMD`` a ``MZ800EMU_TEST_STALL_MS``, viz
dispatch.c ``_test_hook_maybe_arm_stall``): emu vlákno při prvním
``get_registers`` uvnitř dispatch spí _STALL_MS.

Průběh (mz800emu v pipe módu, izolovaný ``--cfg-dir``, bez TCP portu):
  1  ``get_registers`` se zasekne: odpověď ``Emulator busy: command still
     running`` přijde mezi _STALLED_MIN_S a _STALLED_MAX_S.
  2  ``ping`` (bez emu vlákna) odpoví hned - transport není blokovaný.
  3  ``get_state`` během zaseknutí: ``Emulator busy: command not executed``
     zhruba po 10 s (příkaz se zruší, emu ho neprovede).
  4  Po skončení zaseknutí emulátor funguje: ``get_registers`` a
     ``get_state`` uspějí rychle, emulace běží (PC/IR se mění), a pozdní
     výsledek opuštěného ``get_registers`` klientovi nepřijde.

Zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (binárka nenalezena).
"""

import json
import os
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

#: Doba zaseknutí emu vlákna (ms). Musí přesáhnout 20 s (busy odpověď)
#: + 10 s (zrušený get_state ve fázi 3) s rezervou.
_STALL_MS = 36000

#: Okno pro odpověď "still running" (limit 10 s + 10 s).
_STALLED_MIN_S = 17.0
_STALLED_MAX_S = 25.0

#: Okno pro odpověď "not executed" (timeout fronty 10 s).
_NOT_EXEC_MIN_S = 8.0
_NOT_EXEC_MAX_S = 13.0

#: Limit pro rychlé příkazy (s).
_QUICK_S = 3.0


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


class PipeEmu:
    """Emulátor v pipe módu s testovacím háčkem zaseknutí."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp):
        ini = tmp / "emu.ini"
        ini.write_text("", encoding="utf-8")
        env = dict(os.environ)
        env["MZ800EMU_TEST_STALL_MCP_CMD"] = "get_registers"
        env["MZ800EMU_TEST_STALL_MS"] = str(_STALL_MS)
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True,
                                     bufsize=1, encoding="utf-8",
                                     cwd=str(_REPO_ROOT), env=env)
        self.q = queue.Queue()
        self.seen = []          # všechny přijaté JSON zprávy
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
            self.seen.append(msg)
            if pred(msg):
                return msg
        return None

    def send(self, cmd, data=None):
        """Pošle request, vrátí jeho req_id a čas odeslání."""
        self.rid += 1
        req = {"type": "request", "req_id": self.rid, "cmd": cmd,
               "data": data or {}}
        t0 = time.monotonic()
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        return self.rid, t0

    def wait(self, rid, t0, timeout):
        """Počká na response s req_id, vrátí (response, doba v s)."""
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to request {rid} within "
                              f"{timeout:.0f} s (exit code {self.proc.poll()})")
        return resp, time.monotonic() - t0

    def call(self, cmd, data=None, timeout=30.0):
        """Request + čekání na odpověď; vrací (response, doba)."""
        rid, t0 = self.send(cmd, data)
        return self.wait(rid, t0, timeout)

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


def run(exe, tmp):
    emu = PipeEmu(exe, tmp)
    try:
        r, dt = emu.call("get_state", timeout=10.0)
        check(r.get("success"), f"get_state before stall ({dt:.2f} s)")
        if (r.get("data") or {}).get("paused"):
            emu.call("run", timeout=10.0)

        # 1: zaseknutý rozpracovaný příkaz -> "still running" v limitu
        t_stall = time.monotonic()
        rid_regs, t0 = emu.send("get_registers")
        r, dt = emu.wait(rid_regs, t0, 40.0)
        err = r.get("error") or ""
        check(not r.get("success") and err.startswith(
            "Emulator busy: command still running"),
            f"stalled get_registers answered busy: {err!r}")
        check(_STALLED_MIN_S <= dt <= _STALLED_MAX_S,
              f"busy answer within limit ({dt:.1f} s, expected "
              f"{_STALLED_MIN_S}-{_STALLED_MAX_S} s)")

        # 2: transport žije
        r, dt = emu.call("ping", timeout=_QUICK_S + 2)
        check(r.get("success") and dt <= _QUICK_S,
              f"ping answered during stall ({dt:.2f} s)")

        # 3: příkaz do fronty během zaseknutí -> zrušen, neproveden
        r, dt = emu.call("get_state", timeout=20.0)
        err = r.get("error") or ""
        check(not r.get("success") and err.startswith(
            "Emulator busy: command not executed"),
            f"get_state during stall answered not executed: {err!r}")
        check(_NOT_EXEC_MIN_S <= dt <= _NOT_EXEC_MAX_S,
              f"not-executed answer after queue timeout ({dt:.1f} s)")

        # 4: po skončení zaseknutí se emulátor zotaví
        rest = _STALL_MS / 1000.0 - (time.monotonic() - t_stall) + 1.0
        if rest > 0:
            time.sleep(rest)
        pcir = set()
        for _ in range(4):
            r, dt = emu.call("get_registers", timeout=10.0)
            check(r.get("success") and dt <= _QUICK_S,
                  f"get_registers after stall ({dt:.2f} s)")
            d = r.get("data") or {}
            pcir.add((d.get("PC"), d.get("IR")))
            time.sleep(0.2)
        r, dt = emu.call("get_state", timeout=10.0)
        check(r.get("success") and dt <= _QUICK_S,
              f"get_state after stall ({dt:.2f} s)")
        check(len(pcir) > 1, f"emulation runs after stall "
                             f"({len(pcir)} distinct PC/IR)")
        late = [m for m in emu.seen if m.get("type") == "response"
                and m.get("req_id") == rid_regs]
        check(len(late) == 1, "late result of abandoned request not sent "
                              f"({len(late)} response(s) for req {rid_regs})")
    finally:
        emu.close()


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=135)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mcp_busy_stall_"))
    try:
        print(f"[{exe.name}] stalled emulator thread -> bounded MCP answer")
        run(exe, tmp)
    except TestFailure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
