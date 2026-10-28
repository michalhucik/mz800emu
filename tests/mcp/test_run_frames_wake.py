#!/usr/bin/env python3
"""Blokující MCP ``run(frames)`` odpoví hned po zastavení uprostřed snímku.

``run`` s počtem snímků čeká v dispatch vlákně na konec běhu. Když běh
zastaví breakpoint (nebo jiná pauza) uprostřed snímku, odpověď musí přijít
hned po zastavení, ne až po zveřejnění dalšího snímku nebo po uplynutí
kroku čekání 50 ms (N106).

Test CYCLES-krát postaví breakpoint na PC ve smyčce monitoru ROM
s podmínkou ``Cycle >= teď + půl snímku`` a zavolá ``run(frames=50)``:
běh se zastaví uprostřed snímku, až dispatch na konec běhu už čeká
(před opravou odpověď vždy kolem 51 ms, po ní pod 1 ms). Emulátor běží
v MAX SPEED (``set_speed``), aby doba volání nezahrnovala půl snímku
emulace v reálném čase (10 ms), jen latenci odpovědi. Kontroluje:

  1. ``stopped_by`` je ``"breakpoint"``, ``complete`` false,
     ``actual_frames`` 0 nebo 1 (podle toho, kde ve snímku běh začal),
  2. medián doby jednoho volání pod LIMIT_MS (vypíše i maximum).

Binárka: kořen repa nebo proměnná ``MZ_EMU``. Výstup anglicky.
Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]

#: Počet měřených volání.
CYCLES = 30
#: Horní mez mediánu jednoho volání [ms].
LIMIT_MS = 5.0
#: Půl snímku PAL v taktech GDG (snímek 354432 taktů, docs/agent/bp_dsl.md).
HALF_FRAME_TICKS = 177216


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


class Pipe:
    """Emulátor v režimu --mcp-pipe a synchronní volání JSONL příkazů."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, work):
        self.rid = 0
        self.proc = subprocess.Popen(
            [str(exe), "--mcp-pipe", f"--cfg-dir={work}", f"--work-dir={work}",
             "--no-save-ini", "--no-first-run-windows"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8", errors="replace", cwd=str(work))

    def call(self, cmd, data=None):
        """Pošle příkaz a vrátí jeho odpověď (ostatní řádky přeskočí)."""
        self.rid += 1
        msg = {"type": "request", "req_id": self.rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise TestFailure(f"emulator closed the pipe during '{cmd}'")
            line = line.strip()
            if line.startswith("{"):
                m = json.loads(line)
                if m.get("req_id") == self.rid:
                    if not m.get("success"):
                        raise TestFailure(f"'{cmd}' failed: {m}")
                    return m.get("data") or {}


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def main():
    """Spustí měření; 0 = PASS, 1 = FAIL."""
    emu_test_proc.install(deadline_s=50)
    exe = _find_exe()
    work = Path(tempfile.mkdtemp(prefix="mztest-runwake-"))
    pipe = None
    try:
        pipe = Pipe(exe, work)
        time.sleep(1.0)
        pipe.call("pause")
        pipe.call("set_speed", {"mode": "max"})
        # Monitor ROM po 100 snímcích čeká ve smyčce na klávesu; PC v ní je
        # adresa, kterou emulace prochází stále dokola.
        pipe.call("run", {"frames": 100})
        pc = pipe.call("get_registers")["PC"]

        times = []
        last = None
        for _ in range(CYCLES):
            # Zastavení až půl snímku od rozběhu: dispatch už čeká na konec
            # běhu (breakpoint bez podmínky by emulaci zastavil dřív, než
            # dispatch začne čekat, a chybu by neukázal).
            ticks = pipe.call("state_observe", {"windows": []})["time"]["ticks"]
            bp = pipe.call("bp_create_with_init", {
                "fields": ["type", "expr", "addr"], "type": 0, "addr": pc,
                "expr": f"Cycle >= {ticks + HALF_FRAME_TICKS}"})
            t = time.perf_counter()
            last = pipe.call("run", {"frames": 50})
            times.append((time.perf_counter() - t) * 1000.0)
            pipe.call("bp_remove", {"id": bp["id"]})
            if last.get("stopped_by") != "breakpoint":
                break

        check(last.get("stopped_by") == "breakpoint",
              f"run(frames=50) stopped by breakpoint at {pc:04X}h (got {last.get('stopped_by')!r})")
        check(last.get("complete") is False and last.get("actual_frames") in (0, 1),
              f"complete false, actual_frames 0 or 1 (got {last.get('complete')!r}, "
              f"{last.get('actual_frames')!r})")
        med = statistics.median(times)
        print(f"  run(frames=50) -> breakpoint: median {med:.2f} ms, max {max(times):.2f} ms")
        check(med < LIMIT_MS, f"median {med:.2f} ms < {LIMIT_MS} ms")

        pipe.call("bp_clear")
        pipe.call("shutdown")
        pipe.proc.wait(timeout=20)
    except TestFailure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        if pipe:
            emu_test_proc.kill_proc(pipe.proc)
        emu_test_proc.rmtree_retry(work)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
