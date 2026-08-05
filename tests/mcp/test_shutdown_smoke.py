#!/usr/bin/env python3
"""Shutdown smoke: teardown kontrakt s aktivnim MCP spojenim (mzhal krok 12).

Spawns mz800emu(.exe) with --mcp-pipe, opens the JSONL dispatch surface
and then requests shutdown WHILE a background thread keeps submitting
requests over the same pipe. This exercises the mandatory teardown
ordering (g_thread_join -> MCP/dbgapi shutdown -> emulator_teardown):
a use-after-free in the teardown path typically manifests as a crash
(nonzero exit code) or a hang (timeout) here.

The stdin pipe is written from two threads; writes are serialized with
a lock so each JSONL line stays intact.

Exit code: 0 = clean shutdown (exit 0 within timeout), 1 otherwise.
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]

_EXE_CANDIDATES = [
    _REPO_ROOT / "mz800emu.exe",
    _REPO_ROOT / "mz800emu",
]


def _find_exe():
    """Find mz800emu(.exe) binary; allow MZ_EMU env override."""
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env)
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    sys.exit(1)


def _spawn_line_reader(proc):
    """Background thread draining proc.stdout lines into a queue."""
    q = queue.Queue()

    def _pump():
        for line in iter(proc.stdout.readline, ""):
            q.put(line.rstrip("\n"))
        q.put(None)

    t = threading.Thread(target=_pump, daemon=True)
    t.start()
    return q


def main():
    exe = _find_exe()
    print(f"Using binary: {exe}")

    proc = subprocess.Popen(
        [str(exe), "--mcp-pipe"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
        cwd=str(_REPO_ROOT),
    )

    line_q = _spawn_line_reader(proc)
    write_lock = threading.Lock()
    req_counter = [100]

    def _write(payload):
        line = json.dumps(payload)
        with write_lock:
            try:
                proc.stdin.write(line + "\n")
                proc.stdin.flush()
            except OSError:
                pass  # pipe uz muze byt zavrena behem shutdownu

    def _read_json_line(timeout_sec=15.0):
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            try:
                line = line_q.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                return None
            try:
                return json.loads(line)
            except ValueError:
                continue  # ne-JSON radek (bannery) preskoc
        return None

    try:
        # 1. hello handshake
        resp = None
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            msg = _read_json_line(5.0)
            if msg is None:
                break
            if msg.get("type") == "hello":
                resp = msg
                break
        if resp is None:
            print("FAIL: no hello line")
            return 1
        print("hello OK")

        # 2. ping sanity
        _write({"type": "request", "req_id": 1, "cmd": "ping", "data": {}})
        msg = _read_json_line()
        if not msg or not msg.get("success"):
            print(f"FAIL: ping ({msg})")
            return 1
        print("ping OK")

        # 3. background hammer: get_registers ve smycce po celou dobu shutdownu
        stop_flag = threading.Event()

        def _hammer():
            while not stop_flag.is_set() and proc.poll() is None:
                req_counter[0] += 1
                _write({"type": "request", "req_id": req_counter[0],
                        "cmd": "get_registers", "data": {}})
                time.sleep(0.002)

        hammer = threading.Thread(target=_hammer, daemon=True)
        hammer.start()
        time.sleep(0.3)  # nech dispatch rozjet

        # 4. shutdown s aktivnim trafficem
        _write({"type": "request", "req_id": 2, "cmd": "shutdown", "data": {}})

        try:
            rc = proc.wait(timeout=20.0)
        except subprocess.TimeoutExpired:
            print("FAIL: process did not exit within 20s (hang)")
            proc.kill()
            return 1
        finally:
            stop_flag.set()

        if rc != 0:
            print(f"FAIL: exit code {rc} (crash during teardown?)")
            return 1

        print("shutdown smoke PASS (exit 0 with active MCP traffic)")
        return 0

    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
