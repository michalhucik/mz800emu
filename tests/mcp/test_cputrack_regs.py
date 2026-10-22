#!/usr/bin/env python3
"""E2E test: cputrack formát v3 s registry přes MCP (pipe).

Ověřuje cestu agenta od ``trace_start regs=true`` po ``trace_read``:

  - ``trace_status``: ``has_regs``, ``format_version`` 3, ``record_size``
    32 a ``initial_regs`` (PC = začátek programu),
  - záznamy nesou stav po instrukci (``LD HL,1234h`` -> HL = 1234h),
    ``regs: before`` dává stav před (``EX DE,HL`` před: HL = 1234h),
  - zápis registru přes MCP v pauze (``set_register HL``) zapíše za
    sbalenou smyčku ``JR $`` synchronizační záznam ``SYNC write``,
  - filtr podle registru (``{"hl": "0xBEEF"}``) najde synchronizaci,
  - čtení za běhu segmentu (data v RAM) i po ``trace_stop``; stav
    posledního záznamu = ``get_registers``,
  - chyby: ``trace_start regs=false`` za běhu záznamu v3, sloupec
    registru nad segmentem v2,
  - ``history_get`` (cpuhist ALWAYS): položka nese ``regs`` se stavem
    před instrukcí.

Program (MZ-800, RAM 3000h): ``DI; LD HL,1234h; LD DE,5678h; EX DE,HL;
JR $``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
s adresářem cputrack v dočasném adresáři, ``--no-save-ini``); zabíjí se
jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Program: DI; LD HL,1234h; LD DE,5678h; EX DE,HL; JR $.
PROG = 0x3000
PROG_HEX = "F3" "213412" "117856" "EB" "18FE"
#: Sloupce čtení.
COLS = ["seq", "pc", "asm", "kind", "hl", "de"]


def call_raw(emu, cmd, data=None, timeout=30.0):
    """Pošle request a vrátí celou response (i neúspěšnou)."""
    emu.rid += 1
    rid = emu.rid
    req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
    emu.proc.stdin.write(json.dumps(req) + "\n")
    emu.proc.stdin.flush()
    resp = emu._read(timeout, lambda m: m.get("type") == "response" and m.get("req_id") == rid)
    if resp is None:
        raise TestFailure(f"no response to {cmd}")
    return resp


def read_rows(emu, regs="after", flt=None, cols=None):
    """Všechny řádky cputrack od začátku (jedna stránka stačí)."""
    data = {"channel": "cputrack", "from": "oldest", "limit": 500,
            "columns": cols or COLS, "regs": regs}
    if flt:
        data["filter"] = flt
    page = emu.call("trace_read", data)
    names = page["columns"]
    return [dict(zip(names, r)) for r in page["rows"]]


def run(emu):
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("trace_start", {"channel": "cputrack", "regs": True})

    st = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]
    print(f"  status: format {st['format_version']}, record {st['record_size']}, "
          f"has_regs {st.get('has_regs')}")
    check(st.get("has_regs") is True and st["format_version"] == 3 and st["record_size"] == 32,
          "trace_status reports a format 3 segment with registers")
    check((st.get("initial_regs") or {}).get("PC") == PROG, "initial_regs.PC = program start")

    emu.call("run", {"frames": 1})

    # Historie debuggeru (cpuhist ALWAYS v INI): registry před instrukcí.
    hist = emu.call("history_get")
    newest = hist["entries"][hist["current_position"]]
    print(f"  history newest: {newest}")
    check(newest["addr"] == 0x3008 and newest["regs"]["hl"] == "5678" and newest["regs"]["de"] == "1234",
          "history_get: JR $ with HL/DE before = 5678/1234")

    emu.call("set_register", {"reg": "HL", "value": 0xBEEF})

    # Čtení za běhu segmentu (záznamy v RAM).
    rows = read_rows(emu)
    for r in rows:
        print(f"  {r}")
    check(len(rows) >= 7, f"at least 7 records ({len(rows)})")
    check(rows[0]["kind"] == "sync" and rows[0]["asm"] == "SYNC start" and rows[0]["pc"] == "3000",
          "record 0 is SYNC start at 3000h")
    by_pc = {r["pc"]: r for r in rows if r["kind"] == "insn"}
    check(by_pc.get("3001", {}).get("hl") == "1234", "LD HL,1234h: HL after = 1234")
    check(by_pc.get("3007", {}).get("hl") == "5678" and by_pc["3007"]["de"] == "1234",
          "EX DE,HL: HL/DE after = 5678/1234")
    loop = [r for r in rows if r["kind"] == "loop"]
    check(len(loop) == 1 and loop[0]["pc"] == "3008", "JR $ collapsed into one loop record")
    sync = rows[loop[0]["seq"] + 1] if loop else {}
    check(sync.get("asm") == "SYNC write" and sync.get("hl") == "BEEF",
          f"set_register HL wrote SYNC write after the loop ({sync})")

    before = {r["seq"]: r for r in read_rows(emu, regs="before")}
    ex = by_pc["3007"]["seq"]
    check(before[ex]["hl"] == "1234" and before[ex]["de"] == "5678",
          "regs=before: EX DE,HL had HL/DE = 1234/5678")
    check(before[0]["hl"] is None, "regs=before: first record has no state before")
    check(before[sync["seq"]]["hl"] == "5678", "regs=before of SYNC write = state of the loop")

    found = read_rows(emu, flt={"hl": "0xBEEF"})
    check(found and found[0]["seq"] == sync["seq"], "filter hl=0xBEEF finds the SYNC write")
    found = read_rows(emu, flt={"hl": [0x1234, 0x1234], "de": 0x5678}, regs="before")
    check([r["pc"] for r in found] == ["3007"], f"filter on the state before finds EX DE,HL ({found})")

    resp = call_raw(emu, "trace_start", {"channel": "cputrack", "regs": False})
    check(not resp.get("success") and "already recording" in (resp.get("error") or ""),
          f"trace_start regs=false while recording format 3 fails ({resp.get('error')})")

    emu.call("run", {"frames": 1})
    emu.call("trace_stop", {"channel": "cputrack"})
    rows = read_rows(emu, cols=["seq", "pc", "kind", "hl", "sp", "r"])
    last = rows[-1]
    regs = emu.call("get_registers")
    print(f"  last record {last}, registers HL={regs['HL']:04X} SP={regs['SP']:04X}")
    check(last["kind"] == "loop" and int(last["hl"], 16) == regs["HL"]
          and int(last["sp"], 16) == regs["SP"], "last record state = get_registers")

    # Segment v2: sloupce registrů odmítnuty.
    emu.call("trace_start", {"channel": "cputrack", "regs": False})
    emu.call("run", {"frames": 1})
    emu.call("trace_stop", {"channel": "cputrack"})
    st = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]
    check(st.get("has_regs") is False and st["format_version"] == 2, "regs=false records format 2")
    resp = call_raw(emu, "trace_read", {"channel": "cputrack", "columns": ["seq", "hl"]})
    check(not resp.get("success") and "recorded with regs" in (resp.get("error") or ""),
          f"register column on a format 2 segment fails ({resp.get('error')})")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_cputrack_regs_"))
    ok = False
    try:
        trace_dir = tmp / "trace"
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[DEBUGGER]\ncpuhist_mode = ALWAYS\n"
                       "[TRACE_CPUTRACK]\n"
                       f"dir = {trace_dir.as_posix()}\nname = ct\n"
                       "save_on_exit = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            run(emu)
            ok = True
        finally:
            emu.close()
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
