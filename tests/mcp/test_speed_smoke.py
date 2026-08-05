#!/usr/bin/env python3
"""Speed-layer smoke: set_speed max + run_until_tstate přes MCP pipe.

Kryje díru v pokrytí nalezenou 2026-08-04 (Linux review): žádný test
dosud neověřoval, že speed/benchmark vrstva skutečně žene CPU. Symptom
"set_speed failed / delta_cycles 0 / --maxspeed-bench nuly" typicky
znamená emulátor stojící v paused stavu.

Kroky:
  1. spawn --mcp-pipe, hello,
  2. get_state - emulátor NESMÍ startovat paused,
  3. set_speed {"mode":"max"} musí uspět a get_speed to potvrdit,
  4. run_until_tstate na absolutní target musí vrátit delta_cycles > 0
     a reached=true,
  5. set_speed {"mode":"normal"} zpět + shutdown s exit 0.

Exit: 0 = PASS, 1 = FAIL.
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
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env)
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    sys.exit(1)


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

    q = queue.Queue()

    def _pump():
        for line in iter(proc.stdout.readline, ""):
            q.put(line.rstrip("\n"))
        q.put(None)

    threading.Thread(target=_pump, daemon=True).start()
    rid = [0]

    def read_json(timeout=15.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = q.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                return None
            try:
                return json.loads(line)
            except ValueError:
                continue
        return None

    def send(cmd, data=None):
        rid[0] += 1
        proc.stdin.write(json.dumps({"type": "request", "req_id": rid[0],
                                     "cmd": cmd, "data": data or {}}) + "\n")
        proc.stdin.flush()
        while True:
            m = read_json()
            if m is None:
                return None
            if m.get("req_id") == rid[0]:
                return m

    failures = []
    try:
        hello = read_json()
        if not (hello and hello.get("type") == "hello"):
            print("FAIL: no hello")
            return 1

        # 2. nesmi startovat paused
        st = send("get_state")
        st_data = (st or {}).get("data", {})
        if st_data.get("paused") is True:
            failures.append(f"emulator starts PAUSED: {st_data}")

        # 3. set_speed max
        r = send("set_speed", {"mode": "max"})
        if not (r and r.get("success") and r["data"].get("max_speed") is True):
            failures.append(f"set_speed max failed: {r}")
        sp = send("get_speed")
        if not (sp and sp.get("success") and sp["data"].get("mode") == "max"):
            failures.append(f"get_speed after max: {sp}")

        # 4. run_until_tstate musi realne utratit cykly
        rt = send("run_until_tstate", {"target_total_cycles": 500000})
        rt_data = (rt or {}).get("data", {})
        if not (rt and rt.get("success")
                and rt_data.get("reached") is True
                and rt_data.get("delta_cycles", 0) > 0):
            failures.append(f"run_until_tstate: {rt}")
        else:
            print(f"run_until_tstate OK (delta_cycles="
                  f"{rt_data['delta_cycles']})")

        # 5. zpet na normal + shutdown
        send("set_speed", {"mode": "normal"})
        send("shutdown")
        try:
            rc = proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            failures.append("no clean exit after shutdown")
            rc = -1
        if rc != 0:
            failures.append(f"exit code {rc}")

        if failures:
            print("SPEED SMOKE FAIL:")
            for f in failures:
                print("  " + f)
            return 1
        print("SPEED SMOKE PASS")
        return 0

    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
