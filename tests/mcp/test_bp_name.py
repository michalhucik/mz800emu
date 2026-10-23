#!/usr/bin/env python3
"""E2E test: jméno breakpointu zadané přes MCP vydrží (MCP pipe).

``bp_create_with_init`` dřív jméno bez ``auto_name: false`` zahodilo:
breakpoint vznikl s automatickým jménem a nastavení adresy ho přepsalo
na ``Addr: 0x....``. Nově pole ``name`` bez ``auto_name`` automatické
jméno vypne. Jméno se čte z kanálu ``bplog`` (akce ``log`` breakpointu
zapíše jméno do řádku). Ověřuje:

  1  vytvoření s ``name`` (bez ``auto_name``): jméno vydrží;
  2  ``bp_update`` jen adresy: jméno vydrží;
  3  vytvoření bez ``name``: automatické jméno ``Addr: 0x3003``;
  4  ``bp_update`` jen ``name``: vlastní jméno, změna adresy ho nepřepíše;
  5  ``auto_name: true`` spolu s ``name``: platí automatické jméno.

Program (RAM 3000h): DI, 4x NOP (3001h-3004h), JR $.

Proces běží izolovaně (dočasný ``--cfg-dir``, INI bez načítání uložených
BP, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Adresa a bajty programu.
PROG = 0x3000
PROG_HEX = "F3" "00000000" "18FE"


def _name_after_run(emu):
    """Spustí program od 3000h na jeden snímek a vrátí jména z bplog (pořadí zásahů)."""
    emu.call("pause")
    emu.call("bplog_clear")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {"frames": 1})
    emu.call("pause")
    r = emu.call("trace_read", {"channel": "bplog", "columns": ["name"], "limit": 50})
    return [row[0] for row in r["rows"]]


def _create(emu, fields, values):
    """Založí PC_EXEC breakpoint s akcí log a vrátí id."""
    data = {"fields": ["type", "addr", "action", "enabled"] + fields,
            "type": "PC_EXEC", "action": 'log "hit"', "enabled": True}
    data.update(values)
    res = emu.call("bp_create_with_init", data)
    check(res.get("created") is True, f"breakpoint created ({values})")
    return res["id"]


def run(emu):
    """Scénáře 1-5."""
    bp = _create(emu, ["name"], {"addr": 0x3003, "name": "named"})
    check(_name_after_run(emu) == ["named"], "name without auto_name is kept")
    emu.call("bp_update", {"id": bp, "fields": ["addr"], "addr": 0x3004})
    check(_name_after_run(emu) == ["named"], "address change keeps the name")
    emu.call("bp_remove", {"id": bp})

    bp = _create(emu, [], {"addr": 0x3003})
    check(_name_after_run(emu) == ["Addr: 0x3003"], "no name: automatic name")
    emu.call("bp_update", {"id": bp, "fields": ["name"], "name": "renamed"})
    emu.call("bp_update", {"id": bp, "fields": ["addr"], "addr": 0x3002})
    check(_name_after_run(emu) == ["renamed"], "bp_update name turns the automatic name off")
    emu.call("bp_remove", {"id": bp})

    bp = _create(emu, ["auto_name", "name"], {"addr": 0x3003, "auto_name": True, "name": "ignored"})
    check(_name_after_run(emu) == ["Addr: 0x3003"], "auto_name true wins over name")
    emu.call("bp_remove", {"id": bp})


def main():
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_bp_name_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
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
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
