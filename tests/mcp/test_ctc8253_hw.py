#!/usr/bin/env python3
"""Regresní test: čítač 8253 a příjem přerušení podle měření na MZ-800.

Podklad: měření logickým analyzátorem přímo na pinech 8253 (2026-11-15,
emu-experiments/ctc-meas). Jednotkové testy (tests/peripherals/test_ctc8253.c)
ověřují čítač po jednotlivých hranách na CTC2; tento test ověřuje to, co je
vidět jen v běžícím stroji:

  1  Odezva na přerušení od CTC2: žádost, která přijde těsně před koncem
     instrukce, se přijme až po další. Procesor čeká v HALT (kroky po 4 T);
     od sestupné hrany HSYNC, na které OUT2 přejde do 1, do konce kroku, na
     kterém se přerušení přijme, musí uplynout aspoň 13 tiků pixel clocku
     (zpoždění výstupu 5 + předstih 8) a méně než 13 + 20. Osm variant
     programu posouvá kroky HALT vůči řádku po 15 ticích.
  2  CTC0 v režimu 4 (rychlá cesta CTC0): OUT0 jde do 0 na 32. nebo 33.
     hraně CLK0 po zápisu děliče 32 a po jedné periodě CLK0 zpět do 1.
  3  CTC0 v režimu 0 a GATE0: hradlo v 0 čítání pozastaví, po návratu do 1
     čítač pokračuje od místa zastavení (OUT0 nejde do 1 hned).

Stroj MZ-800 po resetu (režim MZ-700): 8253 a 8255 na 0E003h-0E008h.
Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``); zabíjí se jen
vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Adresa programů v RAM.
_PROG = 0x3000
#: Tabulka vektorů IM 2 (I = 32h, libovolný vektor -> 3434h) a ISR.
_IM2_TABLE = 0x3200
_ISR = 0x3434
#: ISR: LD (0E004h),A (značka ve stopě); PC2 = 0 (konec žádosti); JR $.
_ISR_CODE = bytes.fromhex("3204E0" "3E04" "3203E0" "18FE")

#: Tiky pixel clocku na řádek a sloupec sestupné hrany HSYNC (CLK1) na MZ-800.
_LINE = 1136
_HSYNC_FALL = 950
#: Tiky na takt CPU a na periodu CLK0.
_T = 5
_CLK0 = 16
#: Přijetí přerušení v IM 2 (19 T) a LD (nn),A (13 T) po konec zápisu.
_ISR_WRITE_TICKS = (19 + 13) * _T
#: Nejmenší doba od hrany HSYNC po konec instrukce, na kterém se INT přijme.
_INT_MIN_TICKS = 13
#: Krok HALT.
_HALT_TICKS = 4 * _T


def _prog_int(k):
    """DI; IM 2; I = 32h; PC2 = 0; CT1 režim 2, N = 2; CT2 režim 0, N = 5;
    PC2 = 1; k krát LD A,0 (7 T); EI; HALT; JR na HALT."""
    return bytes.fromhex(
        "F3" "ED5E" "3E32" "ED47"
        "3E04" "3203E0"
        "3E74" "3207E0" "3E02" "3205E0" "AF" "3205E0"
        "3EB0" "3207E0" "3E05" "3206E0" "AF" "3206E0"
        "3E05" "3203E0"
        + "3E00" * k +
        "FB" "76" "18FD")


#: DI; PC0 = 1; GATE0 = 1; CT0 režim 4, jen LSB; N = 32; JR $.
_PROG_MODE4 = bytes.fromhex(
    "F3" "3E01" "3203E0" "3E01" "3208E0" "3E18" "3207E0" "3E20" "3204E0" "18FE")

#: DI; PC0 = 1; GATE0 = 1; CT0 režim 0, jen LSB; N = 40; GATE0 = 0;
#: čekání 64 x DJNZ; GATE0 = 1; JR $.
_PROG_GATE = bytes.fromhex(
    "F3" "3E01" "3203E0" "3E01" "3208E0" "3E10" "3207E0" "3E28" "3204E0"
    "AF" "3208E0" "0640" "10FE" "3E01" "3208E0" "18FE")


def _read_all(emu, channel, columns=None, flt=None):
    """Přečte celý kanál stopy; vrátí řádky."""
    rows, cursor = [], None
    while True:
        req = {"channel": channel, "limit": 2000}
        if columns:
            req["columns"] = columns
        if flt:
            req["filter"] = flt
        if cursor:
            req["cursor"] = cursor
        tr = emu.call("trace_read", req, timeout=120.0)
        rows += tr["rows"]
        if tr.get("end_of_data"):
            return rows
        cursor = tr["next_cursor"]


def _run_program(emu, prog, frames, audio=False):
    """Reset, program na _PROG, stopy přístupů a přerušení, běh; vrátí
    (zápisy na 0E003h-0E008h jako (tick, pos, adresa, hodnota), hrany OUT2, změny OUT0)."""
    emu.call("reset")
    emu.call("pause")
    if audio:
        # záznam zvuku začíná až na hranici snímku: spustit ho snímek předem
        emu.call("audio_capture_start", {"frames": frames + 5, "events": False, "psg_writes": False,
                                         "reference_wav": False, "sdl_wav": False, "pcm": []})
        emu.call("run", {"frames": 1})
    emu.call("mem_write", {"addr": _PROG, "data_hex": prog.hex()})
    emu.call("mem_write", {"addr": _IM2_TABLE, "data_hex": "34" * 257})
    emu.call("mem_write", {"addr": _ISR, "data_hex": _ISR_CODE.hex()})
    emu.call("set_register", {"reg": "PC", "value": _PROG})
    emu.call("trace_start", {"channel": "iorqlog"})
    emu.call("trace_start", {"channel": "intlog"})
    emu.call("run", {"frames": frames})
    io = _read_all(emu, "iorqlog", ["t", "pos", "dir", "addr", "value"])
    writes = [(t, pos, int(addr, 16), int(value, 16)) for t, pos, direction, addr, value in io
              if direction == "out" and addr in ("E003", "E004", "E005", "E006", "E007", "E008")]
    irows = _read_all(emu, "intlog")
    out2 = [(r[1], r[7] == "rising") for r in irows if r[4] == "pin_edge" and r[5] == "ctc2"]
    out0 = []
    if audio:
        lv = _read_all(emu, "audio_levels", ["t", "level"], {"channels": ["ctc0"]})
        prev = None
        for t, level in lv:
            level = 1 if level else 0
            if prev is not None and level != prev:
                out0.append((t, level))
            prev = level
        emu.call("audio_capture_stop")
    emu.call("trace_stop", {"channel": "iorqlog"})
    emu.call("trace_stop", {"channel": "intlog"})
    return writes, out2, out0


def case_int_latency(emu):
    """Scénář 1: předstih žádosti o přerušení před koncem instrukce."""
    seen = []
    for k in range(8):
        writes, out2, _ = _run_program(emu, _prog_int(k), 2)
        rises = [t for t, rising in out2 if rising]
        marks = [(t, pos) for t, pos, addr, _v in writes if addr == 0xE004]
        if not rises or not marks:
            raise TestFailure(f"variant {k}: no OUT2 rise or no ISR write in the trace")
        t_w, pos_w = marks[0]
        base = (t_w - pos_w) % _LINE
        log_t = max(t for t in rises if t <= t_w)
        edge = log_t - ((log_t - base - _HSYNC_FALL) % _LINE)
        accept = t_w - _ISR_WRITE_TICKS
        wait = accept - edge
        seen.append(wait)
        check(_INT_MIN_TICKS <= wait < _INT_MIN_TICKS + _HALT_TICKS,
              f"variant {k}: INT accepted {wait} ticks after the HSYNC edge "
              f"(expected {_INT_MIN_TICKS} to {_INT_MIN_TICKS + _HALT_TICKS - 1})")
    check(len(set(seen)) >= 3, f"variants cover different phases of the HALT step ({sorted(seen)})")


def case_mode4(emu):
    """Scénář 2: pulz režimu 4 na CTC0."""
    writes, _, out0 = _run_program(emu, _PROG_MODE4, 2, audio=True)
    t_w = [t for t, _p, addr, _v in writes if addr == 0xE004][0]
    after = [(t, lv) for t, lv in out0 if t > t_w]
    check(len(after) == 2 and after[0][1] == 0 and after[1][1] == 1,
          f"mode 4: one low pulse on OUT0 after the count write ({after[:4]})")
    edges = -((t_w - after[0][0]) // _CLK0)
    check(edges in (33, 34), f"mode 4: OUT0 falls on CLK0 edge {edges} after the write (expected 33 or 34)")
    check(after[1][0] - after[0][0] == _CLK0, "mode 4: the pulse lasts one CLK0 period")


def case_mode0_gate(emu):
    """Scénář 3: GATE0 pozastaví čítání v režimu 0."""
    writes, _, out0 = _run_program(emu, _PROG_GATE, 2, audio=True)
    t_w = [t for t, _p, addr, _v in writes if addr == 0xE004][0]
    gates = [(t, v & 1) for t, _p, addr, v in writes if addr == 0xE008 and t > t_w]
    check([g for _t, g in gates] == [0, 1], "mode 0: GATE0 written 0 then 1 after the count write")
    t_g0, t_g1 = gates[0][0], gates[1][0]
    rises = [t for t, lv in out0 if t > t_w and lv == 1]
    check(len(rises) == 1, f"mode 0: OUT0 rises once ({rises[:4]})")
    counted = (t_g0 - t_w) // _CLK0
    rest = -((t_g1 - rises[0]) // _CLK0)
    check(rises[0] > t_g1, "mode 0: OUT0 does not rise while GATE0 is low")
    check(40 - counted - 1 <= rest <= 40 - counted + 3,
          f"mode 0: after GATE0 returns the counter needs {rest} more CLK0 edges "
          f"({counted} periods elapsed before the pause, count 40)")


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: no emulator binary found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_ctc_hw_"))
    emu = PipeEmu(exe, tmp)
    try:
        case_int_latency(emu)
        case_mode4(emu)
        case_mode0_gate(emu)
    except TestFailure as e:
        print(f"FAIL: {e}; artifacts kept in {tmp}")
        emu.close()
        return 1
    emu.close()
    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
