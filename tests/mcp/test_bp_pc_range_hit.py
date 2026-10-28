#!/usr/bin/env python3
"""PC_EXEC breakpoint s rozsahem nebo maskou adres opravdu zastaví běh.

Rozsah (``addr_match_mode`` RANGE, ``addr_end``) a maska (MASK,
``addr_mask``) se nastavují až po vytvoření breakpointu (``bp_create_with_init``,
``bp_update``, editor breakpointu v GUI). Breakpointy PC_EXEC se v hlavní
smyčce hledají v mapě adres (bptmap), takže změna rozsahu, masky i zapnutí
musí mapu přestavět; jinak breakpoint zastaví nejvýš na ``addr``.

Test po 100 snímcích vezme PC ve smyčce monitoru ROM (``pc``) a kontroluje
``run(frames=3)``:

  1. RANGE pc-10h..pc+10h vytvořený přes bp_create_with_init zastaví,
  2. vypnutý (bp_enable false) nezastaví, znovu zapnutý zastaví,
  3. SINGLE na adrese mimo smyčku, přes bp_update změněný na RANGE přes pc,
     zastaví,
  4. MASK (addr_mask 0FF00h, addr = stránka pc) zastaví.

Binárka: kořen repa nebo proměnná ``MZ_EMU``. Výstup anglicky.
Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
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

#: Typ breakpointu PC_EXEC (en_BPT_TYPE).
BPT_TYPE_PC_EXEC = 0


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

    def stopped_by(self):
        """Spustí run(frames=3) a vrátí stopped_by."""
        return self.call("run", {"frames": 3}).get("stopped_by")


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def main():
    """Spustí kontroly; 0 = PASS, 1 = FAIL."""
    emu_test_proc.install(deadline_s=50)
    exe = _find_exe()
    work = Path(tempfile.mkdtemp(prefix="mztest-bprange-"))
    pipe = None
    try:
        pipe = Pipe(exe, work)
        time.sleep(1.0)
        pipe.call("pause")
        # Monitor ROM po 100 snímcích čeká ve smyčce na klávesu; PC v ní je
        # adresa, kterou emulace prochází stále dokola.
        pipe.call("run", {"frames": 100})
        pc = pipe.call("get_registers")["PC"]
        check(pipe.stopped_by() == "frames", f"no breakpoint: run(frames=3) ends by frames (loop PC {pc:04X}h)")

        # 1. RANGE při vytvoření.
        bp = pipe.call("bp_create_with_init", {
            "fields": ["type", "addr", "addr_end", "addr_match_mode"],
            "type": BPT_TYPE_PC_EXEC, "addr": pc - 0x10, "addr_end": pc + 0x10,
            "addr_match_mode": "RANGE"})
        got = pipe.stopped_by()
        check(got == "breakpoint", f"RANGE {pc - 0x10:04X}h-{pc + 0x10:04X}h from bp_create_with_init stops (got {got!r})")

        # 2. Vypnutí a zapnutí rozsahu.
        pipe.call("bp_enable", {"id": bp["id"], "enabled": False})
        got = pipe.stopped_by()
        check(got == "frames", f"disabled RANGE breakpoint does not stop (got {got!r})")
        pipe.call("bp_enable", {"id": bp["id"], "enabled": True})
        got = pipe.stopped_by()
        check(got == "breakpoint", f"re-enabled RANGE breakpoint stops (got {got!r})")
        pipe.call("bp_remove", {"id": bp["id"]})

        # 3. SINGLE mimo smyčku změněný přes bp_update na RANGE přes pc.
        bp = pipe.call("bp_create_with_init", {
            "fields": ["type", "addr"], "type": BPT_TYPE_PC_EXEC, "addr": pc - 0x10})
        got = pipe.stopped_by()
        check(got == "frames", f"SINGLE {pc - 0x10:04X}h outside the loop does not stop (got {got!r})")
        pipe.call("bp_update", {"id": bp["id"], "fields": ["addr_end", "addr_match_mode"],
                                "addr_end": pc + 0x10, "addr_match_mode": "RANGE"})
        got = pipe.stopped_by()
        check(got == "breakpoint", f"SINGLE changed to RANGE by bp_update stops (got {got!r})")
        pipe.call("bp_remove", {"id": bp["id"]})

        # 4. MASK přes stránku pc.
        bp = pipe.call("bp_create_with_init", {
            "fields": ["type", "addr", "addr_mask", "addr_match_mode"],
            "type": BPT_TYPE_PC_EXEC, "addr": pc & 0xFF00, "addr_mask": 0xFF00,
            "addr_match_mode": "MASK"})
        got = pipe.stopped_by()
        check(got == "breakpoint", f"MASK {pc & 0xFF00:04X}h/FF00h from bp_create_with_init stops (got {got!r})")
        pipe.call("bp_remove", {"id": bp["id"]})

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
