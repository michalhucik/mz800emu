#!/usr/bin/env python3
"""E2E test: čas záznamů cputrack přes hranice chunků a po skoku času (pipe).

Čtenář počítá čas záznamu jako kotvu chunku (``start_pxclk``
v manifestu) + součet T-stavů (``t_std`` + ``wait``) předchozích záznamů
chunku x dělička. Test ověřuje, že tento čas sedí se skutečným časem
stroje:

  A  kotva nového chunku navazuje na konec předchozího chunku (dřív se
     brala uprostřed instrukce, u ``IN`` posunutá o synchronizované takty);
     program ``IN A,(0CEh)`` v těsné smyčce, ``chunk_mb = 1``, několik
     chunků; konec posledního záznamu = čas stroje v pauze,
  B  po načtení snapshotu (skok času zpět) během trace začne nový chunk
     s kotvou ve skutečném čase; konec posledního záznamu = čas stroje
     (dřív čas pokračoval od staré kotvy).

Obojí pro formát v2 (12 B) i v3 (32 B, registry).

Program (MZ-800, RAM 3000h): ``DI; L: IN A,(0CEh); JR L``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
s adresářem cputrack v dočasném adresáři, ``--no-save-ini``); zabíjí se
jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Program: DI; 3001h: IN A,(0CEh); JR 3001h.
PROG = 0x3000
PROG_HEX = "F3" "DBCE" "18FC"
#: Limit čekání na zastavení (ms).
WAIT_MS = 20000
#: Synchronizační záznam v3 (do času se nepočítá).
INSN_LEN_SYNC = 0xFE


def wait_stop(emu):
    """Počká na zastavení po ``run``."""
    emu.call("wait_stop", {"timeout_ms": WAIT_MS}, timeout=WAIT_MS / 1000 + 10)


def run_frames(emu, frames):
    """Rozběhne emulaci na @p frames snímků a počká na zastavení."""
    emu.call("run", {"frames": frames})
    wait_stop(emu)


def machine_time(emu, per_screen):
    """Čas stroje v pxCLK (snímek x takty snímku + takt ve snímku)."""
    t = emu.call("state_observe", {"windows": []})["time"]
    return t["frame"] * per_screen + t["frame_ticks"]


def chunks(trace_dir):
    """Manifest a chunky segmentu: (manifest, [(meta, bajty), ...])."""
    man = json.loads((trace_dir / "ct.json").read_text(encoding="utf-8"))
    return man, [(ch, (trace_dir / ch["file"]).read_bytes()) for ch in man["chunks"]]


def chunk_end(meta, data, rs, div):
    """Čas konce posledního záznamu chunku (kotva + součet délek)."""
    t = meta["start_pxclk"]
    for off in range(0, len(data), rs):
        r = data[off:off + rs]
        if r[2] == INSN_LEN_SYNC:
            continue
        t += (r[11] + struct.unpack_from("<I", r, 7)[0]) * div
    return t


def start_program(emu, regs):
    """Pauza, program do RAM, PC na začátek, start cputrack."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("trace_start", {"channel": "cputrack", "regs": regs})


def scenario_a(emu, trace_dir, regs):
    """A: kotvy chunků navazují, konec = čas stroje."""
    label = "v3" if regs else "v2"
    start_program(emu, regs)
    # 1 MB chunk: v3 ~5 snímků, v2 ~14 snímků smyčky IN/JR.
    run_frames(emu, 25 if regs else 60)
    man_live = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]
    emu.call("trace_stop", {"channel": "cputrack"})
    man, chs = chunks(trace_dir)
    div = man["cpu_divider"]
    per_screen = man["pxclk_per_screen"]
    rs = man_live["record_size"]
    now = machine_time(emu, per_screen)
    check(len(chs) >= 3, f"{label} A: at least 3 chunks (got {len(chs)})")
    prev_end = None
    for meta, data in chs:
        if prev_end is not None:
            check(meta["start_pxclk"] == prev_end,
                  f"{label} A: chunk {meta['index']} anchor {meta['start_pxclk']} "
                  f"== end of previous chunk {prev_end}")
        prev_end = chunk_end(meta, data, rs, div)
    check(prev_end == now, f"{label} A: end of trace {prev_end} == machine time {now}")
    print(f"  {label} A: {len(chs)} chunks, anchors continuous, end == machine time")


def scenario_b(emu, trace_dir, regs):
    """B: snapshot load během trace = nový chunk, konec = čas stroje."""
    label = "v3" if regs else "v2"
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    snap = emu.call("snapshot_save_buffer", {"description": "time jump"})
    emu.call("trace_start", {"channel": "cputrack", "regs": regs})
    # Celý běh se vejde do jednoho chunku (v3 ~0,6 MB): druhý chunk
    # vznikne jen ze skoku času.
    run_frames(emu, 2)
    emu.call("snapshot_load_buffer", {"bytes_b64": snap["bytes_b64"]})
    run_frames(emu, 1)
    rs = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]["record_size"]
    emu.call("trace_stop", {"channel": "cputrack"})
    man, chs = chunks(trace_dir)
    div = man["cpu_divider"]
    now = machine_time(emu, man["pxclk_per_screen"])
    check(len(chs) >= 2, f"{label} B: time jump starts a new chunk (got {len(chs)} chunks)")
    end = chunk_end(chs[-1][0], chs[-1][1], rs, div)
    check(end == now, f"{label} B: end of trace {end} == machine time {now} after the jump")
    print(f"  {label} B: {len(chs)} chunks, end == machine time after snapshot load")


def one_process(exe, tmp, name, fn, regs):
    """Jeden scénář v samostatném procesu (čistý segment a adresář)."""
    trace_dir = tmp / f"trace_{name}"
    ini = tmp / f"{name}.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                   "[TRACE_CPUTRACK]\n"
                   f"dir = {trace_dir.as_posix()}\nname = ct\n"
                   "chunk_mb = 1\nsave_on_exit = 0\n",
                   encoding="utf-8")
    cfg = tmp / f"cfg_{name}"
    cfg.mkdir()
    emu = PipeEmu(exe, cfg, ini)
    try:
        fn(emu, trace_dir, regs)
    finally:
        emu.close()


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_cputrack_time_"))
    ok = False
    try:
        for regs in (True, False):
            v = "v3" if regs else "v2"
            one_process(exe, tmp, f"a_{v}", scenario_a, regs)
            one_process(exe, tmp, f"b_{v}", scenario_b, regs)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
