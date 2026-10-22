#!/usr/bin/env python3
"""Tests of the converter coverage_to_lcov.py (code coverage -> lcov).

Code comments and docstrings are in English (explicit exception from the
project rule of Czech comments, see the header of the script).

The listings below are real assembler output of the same source (sjasmplus
1.22.0 --lst, pasmo 0.6.0 --listing; pasmo keeps CRLF and the form feed of
its page headers). The coverage exports are built in the emulator export
format (mz800emu-coverage/1, rows as written by coverage_export_write).

The test lives in tests/tools/ (not next to the script): everything under
docs/ ships in `make dist`, so no test and no bytecode may end up there.
Registered in ctest as `coverage_to_lcov_py` (tests/tools/CMakeLists.txt).
"""

import json
import os
import sys
import tempfile
import unittest

# The imported script lives in docs/ (shipped in dist) - never write __pycache__ there.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir, "docs", "tools"))
import coverage_to_lcov as cl  # noqa: E402

#: Source of both listings (for reference):
#:         ORG 3000h
#: start:  ld b,3 / loop: dec b / jr nz,loop / ld a,(data) / cp 1 / jp z,skip
#:         call subr / skip: ld hl,msg / ldir / jr $ / subr: ret nc / ret
#: data:   db 1,2,3 / msg: defm "AB" / long: db 1..12 / include "inc.asm"
#: inc.asm: incl: nop / ret z
SJASM_LST = (
    '# file opened: t.asm\r\n 1    0000                      ORG 3000h\r\n'
    ' 2    3000 06 03        start:  ld b,3          ; komentar\r\n'
    ' 3    3002 05           loop:   dec b\r\n 4    3003 20 FD                jr nz,loop\r\n'
    ' 5    3005 3A 19 30             ld a,(data)\r\n 6    3008 FE 01                cp 1\r\n'
    ' 7    300A CA 10 30             jp z,skip\r\n 8    300D CD 17 30             call subr\r\n'
    ' 9    3010 21 1C 30     skip:   ld hl,msg\r\n10    3013 ED B0                ldir\r\n'
    '11    3015 18 FE                jr $\r\n12    3017 D0           subr:    ret nc\r\n'
    '13    3018 C9                   ret\r\n14    3019 01 02 03     data:   db 1,2,3\r\n'
    '15    301C 41 42        msg:    defm "AB"\r\n'
    '16    301E 01 02 03 04  long:   db 1,2,3,4,5,6,7,8,9,10,11,12\r\n'
    '16    3022 05 06 07 08\r\n16    3026 09 0A 0B 0C\r\n'
    '17    302A                      include "inc.asm"\r\n# file opened: inc.asm\r\n'
    ' 1+   302A 00           incl:   nop\r\n 2+   302B C8                   ret z\r\n'
    ' 3+   302C\r\n# file closed: inc.asm\r\n18    302C\r\n# file closed: t.asm\r\n')

PASMO_LST = (
    '\x0c\tpasmo 0.6.0+git20220830\tPAGE 1\r\n\r\n\r\n\t\t                ORG 3000h\r\n'
    '  3000   06 03          start:  LD B,3           komentar\r\n'
    '  3002   05             loop:   DEC B\r\n  3003   20 FD                  JR NZ,loop\r\n'
    '  3005   3A 3019                LD A,(data)\r\n  3008   FE 01                  CP 1\r\n'
    '  300A   CA 3010                JP Z,skip\r\n  300D   CD 3017                CALL subr\r\n'
    '  3010   21 301C        skip:   LD HL,msg\r\n  3013   ED B0                  LDIR\r\n'
    '  3015   18 FE                  JR $\r\n  3017   D0             subr:    RET NC\r\n'
    '  3018   C9                     RET\r\n  3019   01 02 03       data:   DB 1,2,3\r\n'
    '  301C   41 42          msg:    DEFM "AB"\r\n'
    '  301E   01 02 03 04    long:   DB 1,2,3,4,5,6,7,8,9,10,11,12\r\n'
    '  3022   05 06 07 08  \r\n  3026   09 0A 0B 0C  \r\n\t\t                INCLUDEinc.asm\r\n'
    '  302A   00           C incl:   NOP\r\n  302B   C8           C         RET Z\r\n'
    '\x0c\tpasmo 0.6.0+git20220830\tPAGE S\r\n\r\n\r\nMacros:\r\n\r\nSymbols:\r\n'
    '3019\tdata            302A\tincl            301E\tlong            \r\n'
    '3002\tloop            301C\tmsg             3010\tskip            \r\n'
    '3000\tstart           3017\tsubr            \r\n')

#: Instruction addresses of the program (data at 3019h-302Ah skipped).
CODE = [0x3000, 0x3002, 0x3003, 0x3005, 0x3008, 0x300A, 0x300D, 0x3010,
        0x3013, 0x3015, 0x3017, 0x3018, 0x302A, 0x302B]
#: Conditional instructions: JR NZ, JP Z, LDIR, RET NC, RET Z.
COND = [0x3003, 0x300A, 0x3013, 0x3017, 0x302B]


def detail_row(pc, exec_, taken=0, not_taken=0, key="00", space="ram"):
    """One row of a detail export (state derived like coverage_branch_state_name)."""
    decision = pc in COND
    if not decision:
        state = "none"
    elif taken and not_taken:
        state = "both"
    elif taken:
        state = "taken_only"
    elif not_taken:
        state = "not_taken_only"
    else:
        state = "unobserved"
    return {"pc": pc, "space": space, "offset": pc, "key": key, "len": 1, "exec": exec_,
            "taken": taken, "not_taken": not_taken, "state": state, "operand": 0,
            "next_pc": pc + 1, "first_epoch": 0, "flags": ["decision"] if decision else []}


def export(level, rows, complete=True):
    """Export document in the emulator format."""
    cols = (["pc", "state", "int_entry", "first_epoch"] if level == "branch" else
            ["pc", "space", "offset", "key", "len", "exec", "taken", "not_taken", "state",
             "operand", "next_pc", "first_epoch", "flags"])
    return {"format": cl.EXPORT_FORMAT,
            "status": {"active": False, "has_data": True, "level": level, "complete": complete},
            "columns": cols, "rows": rows}


def parse_info(text):
    """{'DA': {line: count}, 'BRDA': {(line, branch): taken}, ...} of one record."""
    rec = {"DA": {}, "BRDA": {}, "SF": None, "counts": {}}
    for line in text.splitlines():
        tag, _, val = line.partition(":")
        if tag == "SF":
            rec["SF"] = val
        elif tag == "DA":
            ln, cnt = val.split(",")
            rec["DA"][int(ln)] = int(cnt)
        elif tag == "BRDA":
            ln, block, br, taken = val.split(",")
            rec["BRDA"][(int(ln), int(br))] = taken
        elif tag in ("BRF", "BRH", "LF", "LH"):
            rec["counts"][tag] = int(val)
    return rec


class ConvertTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, content, binary=False):
        path = os.path.join(self.d, name)
        with open(path, "wb") as f:
            f.write(content.encode("utf-8") if not binary else content)
        return path

    def run_convert(self, doc, listings=(), **kw):
        exp = self.write("cov.json", json.dumps(doc))
        out = os.path.join(self.d, "cov.info")
        summary = cl.convert(exp, out, list(listings), **kw)
        with open(out, encoding="utf-8") as f:
            return summary, f.read()

    def test_listing_lines(self):
        """Both listing formats give the same instruction addresses and conditionals."""
        for name, text, fmt in (("s.lst", SJASM_LST, "sjasmplus"), ("p.lst", PASMO_LST, "pasmo")):
            path = self.write(name, text)
            with open(path, encoding="utf-8") as f:
                self.assertEqual(cl.detect_format(f.read().split("\n")), fmt)
            lines = cl.parse_listing(path)
            self.assertEqual([l.addr for l in lines], CODE, name)
            self.assertEqual([l.addr for l in lines if l.conditional], COND, name)
            # Line numbers count "\n" only (the pasmo form feed is not a line break).
            raw = text.split("\n")
            for l in lines:
                self.assertIn(f"{l.addr:04X}", raw[l.line - 1], name)

    def test_detail_counts(self):
        """Detail level: DA = executions, BRDA = taken/not taken, '-' for unexecuted."""
        rows = [detail_row(0x3000, 1), detail_row(0x3002, 3),
                detail_row(0x3003, 3, taken=2, not_taken=1, key="20"),
                detail_row(0x3005, 1), detail_row(0x3008, 1),
                detail_row(0x300A, 1, taken=1, key="CA"),
                detail_row(0x3010, 1), detail_row(0x3013, 4, taken=3, not_taken=1, key="EDB0"),
                detail_row(0x3015, 500),
                # Two code versions at 3015h (another bank) are summed.
                detail_row(0x3015, 20, space="memext"),
                detail_row(0x1234, 7)]   # outside the listing
        summary, info = self.run_convert(export("detail", rows), [self.write("s.lst", SJASM_LST)])
        rec = parse_info(info)
        line = {l.addr: l.line for l in cl.parse_listing(os.path.join(self.d, "s.lst"))}
        self.assertEqual(rec["DA"][line[0x3002]], 3)
        self.assertEqual(rec["DA"][line[0x3015]], 520)
        self.assertEqual(rec["DA"][line[0x300D]], 0)       # CALL never executed
        self.assertEqual(rec["BRDA"][(line[0x3003], 0)], "2")
        self.assertEqual(rec["BRDA"][(line[0x3003], 1)], "1")
        self.assertEqual(rec["BRDA"][(line[0x300A], 1)], "0")  # executed, never not taken
        self.assertEqual(rec["BRDA"][(line[0x3017], 0)], "-")  # RET NC never executed
        self.assertEqual(rec["BRDA"][(line[0x302B], 1)], "-")  # RET Z (include) never executed
        self.assertEqual(rec["counts"], {"BRF": 10, "BRH": 5, "LF": len(CODE), "LH": 9})
        self.assertEqual(summary["outside"], 1)
        self.assertTrue(rec["SF"].endswith("s.lst"))
        self.assertTrue(info.rstrip().endswith("end_of_record"))

    def test_branch_level_and_pasmo(self):
        """Branch level: executed = 1, a seen branch result = 1."""
        rows = [{"pc": 0x3000, "state": "none", "int_entry": False, "first_epoch": 0},
                {"pc": 0x3003, "state": "both", "int_entry": False, "first_epoch": 0},
                {"pc": 0x300A, "state": "not_taken_only", "int_entry": False, "first_epoch": 0}]
        _, info = self.run_convert(export("branch", rows), [self.write("p.lst", PASMO_LST)],
                                   fmt="pasmo")
        rec = parse_info(info)
        line = {l.addr: l.line for l in cl.parse_listing(os.path.join(self.d, "p.lst"))}
        self.assertEqual(rec["DA"][line[0x3000]], 1)
        self.assertEqual(rec["BRDA"][(line[0x3003], 0)], "1")
        self.assertEqual(rec["BRDA"][(line[0x3003], 1)], "1")
        self.assertEqual(rec["BRDA"][(line[0x300A], 0)], "0")
        self.assertEqual(rec["BRDA"][(line[0x300A], 1)], "1")

    def test_offset(self):
        """--offset moves listing addresses (code run at another address)."""
        rows = [detail_row(0x5002, 9)]
        _, info = self.run_convert(export("detail", rows), [self.write("s.lst", SJASM_LST)],
                                   offset=0x2000)
        rec = parse_info(info)
        self.assertEqual(rec["DA"][4], 9)   # listing line of "dec b" (3002h)

    def test_pseudo_source(self):
        """Without a listing: pseudo file with one line per executed address."""
        rows = [detail_row(0x3013, 4, taken=3, not_taken=1, key="EDB0"), detail_row(0x3000, 2)]
        summary, info = self.run_convert(export("detail", rows))
        rec = parse_info(info)
        with open(summary["pseudo"], encoding="utf-8") as f:
            pseudo = f.read().splitlines()
        self.assertEqual(pseudo, ["3000h  none  [00]", "3013h  both  [EDB0]"])
        self.assertEqual(rec["DA"], {1: 2, 2: 4})
        self.assertEqual(rec["BRDA"], {(2, 0): "3", (2, 1): "1"})
        self.assertEqual(os.path.abspath(summary["pseudo"]), rec["SF"])

    def test_labels_and_data(self):
        """Operands named like directives stay instructions; colonless labels are stripped."""
        for src in ("call text", "JP HEX", "jr block", "djnz word", "ld a,(hl)",
                    "loop djnz loop", "start: ld b,3", "ret"):
            self.assertFalse(cl.is_data(src), src)
        for src in ("db 1,2,3", "msg DEFM \"AB\"", "text: db 1", "tab dw 1234h", "data:"):
            self.assertTrue(cl.is_data(src), src)
        for src, cond in (("loop djnz loop", True), ("again jr nz,again", True),
                          ("call text", False), ("x: ret z", True), ("ld b,3", False)):
            self.assertEqual(bool(cl._COND_RE.match(cl.instruction_text(src))), cond, src)

    def test_errors(self):
        """Not a coverage export, unknown listing format -> exit code 1."""
        bad = self.write("bad.json", json.dumps({"format": "other"}))
        self.assertEqual(cl.main([bad, "-o", os.path.join(self.d, "x.info")]), 1)
        good = self.write("cov.json", json.dumps(export("detail", [])))
        lst = self.write("x.lst", "nothing to see\n")
        self.assertEqual(cl.main([good, "-o", os.path.join(self.d, "x.info"), "--listing", lst]), 1)
        self.assertEqual(cl.main([good, "-o", os.path.join(self.d, "x.info"),
                                  "--listing", self.write("s.lst", SJASM_LST)]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
