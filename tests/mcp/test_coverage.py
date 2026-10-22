#!/usr/bin/env python3
"""E2E test: pokrytí kódu (coverage_*) přes MCP pipe na skutečném emulátoru.

Ověřuje:

  1  úroveň branch na programu se všemi druhy podmíněných instrukcí
     (DJNZ, JR NZ, ``JR Z,$+2``, JP NZ/Z, CALL Z/NZ, RET Z, LDIR, CPIR):
     stav větve každé adresy a souhrn ``branches``;
  2  úroveň detail na témže programu: čítače exec/taken/not_taken,
     samomodifikace na 3037h (klíč opcode = jedna jednotka s
     ``operand_varied``, klíč full_bytes = dvě jednotky), osamocený
     prefix ``DD 00`` jako jeden krok, úplnost (``complete``);
  3  malá kapacita: ``dropped`` > 0, ``complete`` false;
  4  vstup do obsluhy přerušení (IM 2 z CTC2): příznak ``int_entry``;
  5  stránkování ``coverage_read`` (limit, kurzor), filtr podle stavu
     a ``new_since_epoch`` po ``coverage_mark``; po ``coverage_reset``
     starý kurzor -> ``segment_replaced:``; reset stroje = nespojitost;
  6  ``coverage_export`` do JSON a CSV;
  7  bitová shoda: stejný program 60 snímků se zapnutým (detail)
     a vypnutým pokrytím dá stejné registry a paměť;
  8  MZ-700 a MZ-1500 (pokud binárky existují): jednotka v RAM;
  9  konfigurace: INI ``[COVERAGE] mode = DETAIL`` spustí sběr při startu,
     ``--coverage`` přebije INI, ``--coverage-save-on-exit`` zapíše CSV
     (relativně k adresáři procesu, ne ``--work-dir``), ``result.json``
     má objekt ``coverage`` (bez dat ``null``).

Program z rešerše (debug-gaps research/09, sonda cov_probe.py) má
očekávané hodnoty ověřené prototypem. Procesy izolované (dočasný
``--cfg-dir`` / ``--work-dir``, ``--no-save-ini``); zabíjí se jen vlastní
PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import base64
import csv
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_headless_exit import Run, make_mzf  # noqa: E402 - headless běh do konce procesu
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)

PROG = 0x3000
#: DI; LD SP,4000h; LD B,3; DJNZ $ (3006h); XOR A; JR NZ,+2 (3009h);
#: JR Z,$+2 (300Bh); JP NZ,3013h (300Dh); JP Z,3013h (3010h);
#: CALL Z,301Ch (3013h); CALL NZ,301Ch (3016h); JP 3020h (3019h);
#: RET Z (301Ch); RET; NOP NOP; LD HL,5000h; LD DE,5100h; LD BC,3;
#: LDIR (3029h); LD HL,5000h; LD BC,16; LD A,77h; CPIR (3033h);
#: LD B,2; LD A,1 (3037h); LD A,5; LD (3038h),A; DJNZ 3037h (303Eh);
#: DD 00 (3040h); JR $ (3042h).
PROG_BRANCH = bytes.fromhex(
    "F3" "310040" "0603" "10FE" "AF" "2002" "2800" "C21330" "CA1330"
    "CC1C30" "C41C30" "C32030" "C8" "C9" "0000"
    "210050" "110051" "010300" "EDB0"
    "210050" "011000" "3E77" "EDB1"
    "0602" "3E01" "3E05" "323830" "10F7"
    "DD00" "18FE")
#: Data pro CPIR: shoda 77h ve 4. bajtu.
DATA_5000 = bytes.fromhex("000000" "77") + bytes(12)
#: Očekávané stavy podmíněných instrukcí (adresa -> stav).
WANT_STATES = {
    0x3006: "both", 0x3009: "not_taken_only", 0x300B: "taken_only",
    0x300D: "not_taken_only", 0x3010: "taken_only", 0x3013: "taken_only",
    0x3016: "not_taken_only", 0x301C: "taken_only", 0x3029: "both",
    0x3033: "both", 0x303E: "both",
}
#: DI; LD SP,4100h; IM 2; I = 32h; CTC1/CTC2 (trvalé INT z CTC2, jako
#: test_ramlog); EI; smyčka. ISR na 3434h: INC D; EI; RETI.
PROG_INT = bytes.fromhex(
    "F3" "310041" "ED5E" "3E32" "ED47"
    "3E04" "3203E0"
    "3E74" "3207E0" "3E02" "3205E0" "AF" "3205E0"
    "3EB0" "3207E0" "3E01" "3206E0" "AF" "3206E0"
    "3E05" "3203E0"
    "0600" "10FE"
    "FB" "210050"
    "71" "23" "7C" "E60F" "F650" "67" "0C" "18F5")
ISR = bytes.fromhex("14" "FB" "ED4D")
#: MZ-700 / MZ-1500: XOR A; JR NZ,+0; JR $.
PROG_700 = bytes.fromhex("F3" "AF" "2000" "18FE")


def load(emu, prog, extra=None):
    """Pauza, program na 3000h (+ další bloky {adresa: bajty}), PC = 3000h."""
    emu.call("pause")
    for addr, data in (extra or {}).items():
        emu.call("mem_write", {"addr": addr, "data_hex": data.hex()})
    emu.call("mem_write", {"addr": PROG, "data_hex": prog.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG})


def run_branch_prog(emu, start):
    """Spustí program PROG_BRANCH se zadaným coverage_start a zastaví na JR $."""
    load(emu, PROG_BRANCH, {0x5000: DATA_5000})
    emu.call("coverage_start", start)
    emu.call("run_until_addr", {"addr": 0x3042})
    emu.call("pause")


def read_all(emu, columns=None, flt=None, limit=5000):
    """Všechny řádky coverage_read (stránkami)."""
    out, cursor, last = [], None, None
    for _ in range(1000):
        req = {"limit": limit}
        if columns:
            req["columns"] = columns
        if flt:
            req["filter"] = flt
        if cursor:
            req["cursor"] = cursor
        last = emu.call("coverage_read", req)
        out.extend(last["rows"])
        cursor = last["next_cursor"]
        if last["end_of_data"]:
            return out, last
    raise TestFailure("coverage_read did not reach end_of_data")


def case_branch(emu):
    """Scénář 1: úroveň branch, stavy větví."""
    run_branch_prog(emu, {"level": "branch", "reset": True})
    st = emu.call("coverage_status")
    check(st["level"] == "branch" and st["active"] and st["complete"], f"branch running and complete ({st})")
    got, _ = read_all(emu, ["pc", "state"], {"state": "decision"})
    states = {pc: s for pc, s in got}
    check(states == WANT_STATES, f"branch states of all conditional instructions ({states})")
    b = st["branches"]
    check((b["decisions"], b["both"], b["taken_only"], b["not_taken_only"]) == (11, 4, 4, 3),
          f"branch summary 11 decisions, 4 both, 4 taken only, 3 not taken only ({b})")
    emu.call("coverage_stop")


def unit_map(rows):
    """Řádky detail (sloupce pc, key, exec, taken, not_taken, operand, flags, space, len) podle (pc, key)."""
    return {(r[0], r[1]): r for r in rows}


def case_detail(emu):
    """Scénář 2: úroveň detail, čítače, samomodifikace, DD 00."""
    cols = ["pc", "key", "exec", "taken", "not_taken", "operand", "flags", "space", "len"]
    run_branch_prog(emu, {"level": "detail", "reset": True})
    st = emu.call("coverage_status")
    check(st["level"] == "detail" and st["complete"] and st["dropped"] == 0 and st["fetch_fallback"] == 0,
          f"detail complete ({st})")
    rows, _ = read_all(emu, cols)
    u = unit_map(rows)
    check(u[(0x3006, "10")][2:5] == [3, 2, 1], f"DJNZ $ exec 3, taken 2, not taken 1 ({u[(0x3006, '10')]})")
    check(u[(0x300B, "28")][2:5] == [1, 1, 0], f"JR Z,$+2 taken from F ({u[(0x300B, '28')]})")
    check(u[(0x3029, "EDB0")][2:5] == [3, 2, 1], f"LDIR BC=3: 3 iterations ({u[(0x3029, 'EDB0')]})")
    check(u[(0x3033, "EDB1")][2:5] == [4, 3, 1], f"CPIR match in 4th byte ({u[(0x3033, 'EDB1')]})")
    smc = u[(0x3037, "3E")]
    check(smc[2] == 2 and smc[5] == 5 and "operand_varied" in smc[6],
          f"self-modified LD A,n: one unit, last operand 05h, operand_varied ({smc})")
    dd = u.get((0x3040, "DD00"))
    check(dd is not None and dd[2] == 1 and dd[8] == 2, f"lone DD 00 is one 2-byte step ({dd})")
    check(all(r[7] in ("ram", "memext") for r in rows), "code cells in RAM (or MemExt bank)")
    check(len(rows) == st["units"], f"rows = units ({len(rows)}, {st['units']})")

    run_branch_prog(emu, {"level": "detail", "key": "full_bytes", "reset": True})
    rows, _ = read_all(emu, ["pc", "key", "exec"], {"pc_from": 0x3037, "pc_to": 0x3037})
    check(sorted(rows) == [[0x3037, "3E01", 1], [0x3037, "3E05", 1]],
          f"full_bytes key: two versions of 3037h ({rows})")
    emu.call("coverage_stop")


def case_capacity(emu):
    """Scénář 3: malá kapacita -> dropped, complete false."""
    run_branch_prog(emu, {"level": "detail", "capacity": 4, "reset": True})
    st = emu.call("coverage_status")
    check(st["units"] == 4 and st["capacity"] == 4 and st["dropped"] > 0 and not st["complete"],
          f"capacity 4: units 4, dropped > 0, incomplete ({st})")
    emu.call("coverage_stop")


def case_int_entry(emu):
    """Scénář 4: první instrukce obsluhy přerušení = int_entry."""
    load(emu, PROG_INT, {0x3200: b"\x34" * 257, 0x3434: ISR})
    emu.call("coverage_start", {"level": "branch", "reset": True})
    emu.call("run", {"frames": 5})
    emu.call("pause")
    rows, _ = read_all(emu, ["pc", "int_entry"], {"flags": ["int_entry"]})
    check(rows == [[0x3434, True]], f"only the ISR start 3434h is int_entry ({rows})")
    emu.call("coverage_stop")


def case_paging(emu):
    """Scénář 5: stránkování, filtr, epocha, reset, nespojitost."""
    run_branch_prog(emu, {"level": "branch", "reset": True})
    allrows, _ = read_all(emu, ["pc"])
    paged, last = read_all(emu, ["pc"], limit=3)
    check(paged == allrows and len(allrows) > 10, f"limit 3 pages = one big page ({len(paged)} rows)")
    part, _ = read_all(emu, ["pc"], {"state": "partial"})
    check([r[0] for r in part] == [0x3009, 0x300B, 0x300D, 0x3010, 0x3013, 0x3016, 0x301C],
          f"state partial ({part})")

    st = emu.call("coverage_mark")
    check(st["epoch"] == 1, f"coverage_mark -> epoch 1 ({st['epoch']})")
    # Program stojí před 3042h (run_until_addr instrukci na cíli nevykoná):
    # NOP na 3042h je první nová adresa po značce, JR $ na 3043h se už nevykoná.
    emu.call("mem_write", {"addr": 0x3042, "data_hex": "00" "18FE"})
    emu.call("run_until_addr", {"addr": 0x3043})
    emu.call("pause")
    new, _ = read_all(emu, ["pc", "first_epoch"], {"new_since_epoch": 1})
    check(new == [[0x3042, 1]], f"new_since_epoch 1: only 3042h ({new})")

    first = emu.call("coverage_read", {"limit": 2, "columns": ["pc"]})
    emu.call("coverage_reset")
    try:
        emu.call("coverage_read", {"cursor": first["next_cursor"]})
        raise TestFailure("old cursor after coverage_reset should fail")
    except TestFailure as e:
        check("segment_replaced" in str(e), f"old cursor -> segment_replaced ({e})")

    before = emu.call("coverage_status")["discontinuities"]
    emu.call("reset")
    emu.call("pause")
    after = emu.call("coverage_status")["discontinuities"]
    check(after == before + 1, f"machine reset counted as discontinuity ({before} -> {after})")
    emu.call("coverage_stop")


def case_export(emu, tmp):
    """Scénář 6: export JSON a CSV."""
    run_branch_prog(emu, {"level": "detail", "reset": True})
    st = emu.call("coverage_status")
    pj, pc = tmp / "cov.json", tmp / "cov.csv"
    rj = emu.call("coverage_export", {"path": str(pj)})
    rc = emu.call("coverage_export", {"path": str(pc), "format": "csv"})
    data = json.loads(pj.read_text(encoding="utf-8"))
    check(data["format"] == "mz800emu-coverage/1" and len(data["rows"]) == st["units"] == rj["rows"],
          f"JSON export: all units ({rj['rows']}, {st['units']})")
    with pc.open(encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    check(len(rows) == rc["rows"] == st["units"], f"CSV export rows ({len(rows)})")
    cp = [r for r in rows if r["pc"] == str(0x3033)][0]
    check((cp["key"], cp["exec"], cp["state"]) == ("EDB1", "4", "both") and "decision" in cp["flags"],
          f"CSV row CPIR ({cp})")
    emu.call("coverage_stop")


def state(emu):
    """Registry a celá logická paměť."""
    regs = emu.call("get_registers")
    mem = b""
    for a in (0x0000, 0x8000):
        mem += base64.b64decode(emu.call("mem_read", {"addr": a, "len": 0x8000})["data_b64"])
    return regs, mem


def case_bit_identity(exe, tmp):
    """Scénář 7: se zapnutým pokrytím (detail) stejný stav jako bez něj."""
    res = []
    for on in (False, True):
        d = tmp / ("bit_" + str(on))
        d.mkdir()
        emu = PipeEmu(exe, d)
        try:
            load(emu, PROG_INT, {0x3200: b"\x34" * 257, 0x3434: ISR})
            if on:
                emu.call("coverage_start", {"level": "detail", "reset": True})
            emu.call("run", {"frames": 60})
            emu.call("pause")
            res.append(state(emu))
            if on:
                st = emu.call("coverage_status")
                check(st["instructions"] > 1000 and st["complete"],
                      f"coverage on: {st['instructions']} steps, complete")
        finally:
            emu.close()
    check(res[0][0] == res[1][0], "registers equal with and without coverage")
    check(res[0][1] == res[1][1], "memory equal with and without coverage")


def case_config(exe, tmp):
    """Scénář 9: [COVERAGE] v INI, --coverage přebije INI, uložení při konci, result.json."""
    make_mzf(tmp / "exittest.mzf")
    ini = "[BREAKPOINTS]\nauto_load = 0\n[COVERAGE]\nmode = DETAIL\n"
    r = Run(exe, tmp, "cfg_ini", ["--exit-after-frames", "20", "--result-json", "result.json",
                                  "--coverage-save-on-exit", "cov.csv"], ini_text=ini, work_dir="wd")
    cov = (r.result or {}).get("coverage")
    check(r.code == 0 and cov and cov["level"] == "detail" and cov["active"] and cov["units"] > 5,
          f"INI [COVERAGE] mode = DETAIL started at boot, result.json coverage ({cov}, {r.tail()})")
    csv_path = r.dir / "cov.csv"
    check(csv_path.is_file() and not (r.dir / "wd" / "cov.csv").exists(),
          f"--coverage-save-on-exit wrote {csv_path.name} relative to the process directory,"
          f" not --work-dir, like --result-json ({r.tail()})")
    lines = csv_path.read_text(encoding="utf-8").splitlines()
    check(lines[0].startswith("pc,space,offset,key") and len(lines) - 1 == cov["units"],
          f"saved CSV: header and one row per unit ({len(lines) - 1}, {cov['units']})")

    r = Run(exe, tmp, "cfg_cli", ["--exit-after-frames", "20", "--result-json", "result.json",
                                  "--coverage", "branch"], ini_text=ini)
    cov = (r.result or {}).get("coverage")
    check(cov and cov["level"] == "branch", f"--coverage=branch overrides INI ({cov})")

    r = Run(exe, tmp, "cfg_off", ["--exit-after-frames", "20", "--result-json", "result.json"])
    check(r.result and r.result.get("coverage", 0) is None,
          f"no coverage data: result.json coverage null ({(r.result or {}).get('coverage', 0)})")


def case_platform(name, tmp):
    """Scénář 8: MZ-700 / MZ-1500 - jednotka detail v RAM, stav větve."""
    exe = _find_exe(name)
    if not exe:
        print(f"  SKIP: {name} binary not found")
        return
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        load(emu, PROG_700)
        emu.call("coverage_start", {"level": "detail", "reset": True})
        emu.call("run_until_addr", {"addr": 0x3004})
        emu.call("pause")
        rows, _ = read_all(emu, ["pc", "space", "state"], {"state": "decision"})
        check(len(rows) == 1 and rows[0][0] == 0x3002 and rows[0][1] in ("ram", "memext")
              and rows[0][2] == "not_taken_only", f"{name}: JR NZ in RAM, not taken ({rows})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_coverage_"))
    ok = False
    try:
        d = tmp / "main"
        d.mkdir()
        emu = PipeEmu(exe, d)
        try:
            case_branch(emu)
            case_detail(emu)
            case_capacity(emu)
            case_int_entry(emu)
            case_paging(emu)
            case_export(emu, tmp)
        finally:
            emu.close()
        case_bit_identity(exe, tmp)
        case_config(exe, tmp)
        for name in ("mz700emu-pal", "mz1500emu"):
            case_platform(name, tmp)
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
