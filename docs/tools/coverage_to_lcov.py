#!/usr/bin/env python3
"""Convert an emulator code coverage export (JSON) to an lcov tracefile.

Note on language: code comments and docstrings in this file are written in
English. This is an explicit exception from the project rule of Czech
comments (decision by the project owner), because the script is a
user-facing helper tool shipped in docs/tools/.

The input is the JSON written by the emulator code coverage export
(format "mz800emu-coverage/1": MCP emu_coverage_export, the Export button
of the Coverage tab in the CPU Profiler window, or --coverage-save-on-exit).
The emulator knows addresses, not source lines, so an assembler listing
maps addresses to lines:

- With --listing: the listing itself is the source file of the tracefile.
  Every listing line that emits instruction bytes is an instrumented line
  (DA with the execution count, 0 = not executed). A conditional
  instruction (JR/JP/CALL/RET with a condition, DJNZ, LDIR, CPIR, INIR,
  OTIR and the decrementing forms) gets two branches (BRDA): 0 = condition
  met (jump, call, return, repeat), 1 = condition not met. Lines that emit
  data (DB, DW, DS, DEFM, ...) are not instrumented.
- Without --listing: a pseudo source file is written next to the output
  (<output>.addresses.txt), one line per executed address in ascending
  order, and the tracefile refers to it.

Supported listing formats (checked against real output): sjasmplus
(--lst, version 1.22.0) and pasmo (--listing, version 0.6.0). Lines whose
address is not a start of an executed instruction count as not executed;
executed addresses outside the listing (ROM, other modules) are counted
and reported on stderr.

Counts: at the detail level, DA is the sum of executions of all code
versions at the address and BRDA the sums of taken / not taken. The branch
level has no counters: an executed address counts 1, a branch result that
was seen counts 1.

Usage:
    python coverage_to_lcov.py coverage.json -o coverage.info \\
        [--listing game.lst] [--format auto|sjasmplus|pasmo] [--offset N]
    genhtml coverage.info -o html

Python 3 standard library only. User-facing output is in English.
"""

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

#: Expected value of the "format" member of the export.
EXPORT_FORMAT = "mz800emu-coverage/1"

#: Data directives (upper case); a listing line emitting bytes through one
#: of them is data, not an instruction.
DATA_DIRECTIVES = {
    "DB", "DEFB", "DM", "DEFM", "DW", "DEFW", "DS", "DEFS", "DZ", "DC",
    "BYTE", "WORD", "DD", "DWORD", "DEFD", "BLOCK", "INCBIN", "BINARY",
    "ABYTE", "ABYTEC", "ABYTEZ", "DG", "DH", "HEX", "TEXT", "ASCII",
}

#: Z80 mnemonics (upper case, including undocumented SLL/SL1); a first token
#: that is one of them is not a label.
Z80_MNEMONICS = {
    "ADC", "ADD", "AND", "BIT", "CALL", "CCF", "CP", "CPD", "CPDR", "CPI", "CPIR",
    "CPL", "DAA", "DEC", "DI", "DJNZ", "EI", "EX", "EXX", "HALT", "IM", "IN",
    "INC", "IND", "INDR", "INI", "INIR", "JP", "JR", "LD", "LDD", "LDDR", "LDI",
    "LDIR", "NEG", "NOP", "OR", "OTDR", "OTIR", "OUT", "OUTD", "OUTI", "POP",
    "PUSH", "RES", "RET", "RETI", "RETN", "RL", "RLA", "RLC", "RLCA", "RLD", "RR",
    "RRA", "RRC", "RRCA", "RRD", "RST", "SBC", "SCF", "SET", "SLA", "SLL", "SL1",
    "SRA", "SRL", "SUB", "XOR",
}

#: Conditional instruction: mnemonic and the condition operand.
_COND_RE = re.compile(
    r"^(?:(?:JR)\s+(?:NZ|Z|NC|C)\s*,"
    r"|(?:JP|CALL)\s+(?:NZ|Z|NC|C|PO|PE|P|M)\s*,"
    r"|RET\s+(?:NZ|Z|NC|C|PO|PE|P|M)\b"
    r"|DJNZ\b"
    r"|(?:LDIR|LDDR|CPIR|CPDR|INIR|INDR|OTIR|OTDR)\b)",
    re.IGNORECASE)

#: sjasmplus: "<line>[+...] <addr> <bytes>  <source>", bytes = 2-digit groups.
_SJASM_RE = re.compile(
    r"^\s*\d+\+*\s+([0-9A-Fa-f]{4})((?: [0-9A-Fa-f]{2})+)(?:\s{2,}(.*))?\s*$")

#: pasmo: "  <addr>   <bytes>  [C ]<source>", bytes = 2- or 4-digit groups.
_PASMO_RE = re.compile(
    r"^ {2}([0-9A-Fa-f]{4}) {3}([0-9A-Fa-f]{2}(?:[0-9A-Fa-f]{2})?"
    r"(?: [0-9A-Fa-f]{2}(?:[0-9A-Fa-f]{2})?)*)(?:\s{2,}(.*))?\s*$")


class ConvertError(Exception):
    """Invalid input; the message is shown to the user."""


@dataclass
class ListingLine:
    """One instrumented listing line."""
    line: int          #: 1-based line number in the listing.
    addr: int          #: Address of the first emitted byte (with --offset).
    conditional: bool  #: The instruction is conditional (gets BRDA).


@dataclass
class AddrCoverage:
    """Coverage of one logical address merged over all its rows."""
    executed: int = 0             #: Executions (branch level: 1).
    decision: bool = False        #: Conditional instruction.
    taken: int = 0                #: Condition met (branch level: 0/1).
    not_taken: int = 0            #: Condition not met (branch level: 0/1).
    state: str = "none"           #: State of the first row (pseudo file text).
    keys: List[str] = field(default_factory=list)  #: Code version keys (detail).


def load_export(path: str) -> Tuple[dict, Dict[int, AddrCoverage]]:
    """Load the export and merge its rows by address.

    Returns (status, {address: AddrCoverage}). Raises ConvertError for a
    file that is not a coverage export.
    """
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise ConvertError(f"cannot read {path}: {e}") from e
    if not isinstance(doc, dict) or doc.get("format") != EXPORT_FORMAT:
        raise ConvertError(f"{path} is not a coverage export (format {EXPORT_FORMAT})")
    status = doc.get("status") or {}
    detail = status.get("level") == "detail"
    cov: Dict[int, AddrCoverage] = {}
    for row in doc.get("rows") or []:
        pc = row.get("pc")
        if not isinstance(pc, int) or not 0 <= pc <= 0xFFFF:
            raise ConvertError(f"{path}: row without a valid pc: {row!r}")
        state = row.get("state", "none")
        c = cov.get(pc)
        if c is None:
            c = cov[pc] = AddrCoverage(state=state)
        if state != "none":
            c.decision = True
        if detail:
            c.executed += int(row.get("exec") or 0)
            c.taken += int(row.get("taken") or 0)
            c.not_taken += int(row.get("not_taken") or 0)
            if row.get("key"):
                c.keys.append(str(row["key"]))
        else:
            c.executed = 1
            if state in ("both", "taken_only"):
                c.taken = 1
            if state in ("both", "not_taken_only"):
                c.not_taken = 1
    return status, cov


def detect_format(lines: List[str]) -> str:
    """Guess the listing format from its first lines."""
    head = lines[:20]
    if any(l.startswith("# file opened:") for l in head):
        return "sjasmplus"
    if any("pasmo" in l.lower() for l in head[:3]):
        return "pasmo"
    raise ConvertError("unknown listing format, use --format sjasmplus or pasmo")


def split_label(source: str) -> Tuple[str, str]:
    """Split the source text of a listing line into (label, statement).

    A label is the first token when it ends with a colon, or when it is not
    a Z80 mnemonic nor a data directive and something follows it ("msg DEFM
    'AB'", "loop DJNZ loop"). An instruction whose operand is named like a
    directive ("CALL TEXT", "JP HEX") keeps its mnemonic.
    """
    s = source.strip()
    m = re.match(r"^([A-Za-z_.@$][\w.@$]*):\s*", s)
    if m:
        return m.group(1), s[m.end():]
    parts = s.split(None, 1)
    if (len(parts) == 2 and parts[0].upper() not in Z80_MNEMONICS
            and parts[0].upper() not in DATA_DIRECTIVES):
        return parts[0], parts[1].strip()
    return "", s


def is_data(source: str) -> bool:
    """True if the source text of a listing line is a data directive.

    The text may start with a label (with or without a colon). For pasmo
    the leading "C" marker of included lines is skipped by the caller.
    """
    _, stmt = split_label(source)
    tokens = stmt.split()
    return not tokens or tokens[0].upper() in DATA_DIRECTIVES


def instruction_text(source: str) -> str:
    """Source text without the label (for the conditional instruction test)."""
    return split_label(source)[1]


def parse_listing(path: str, fmt: str = "auto", offset: int = 0) -> List[ListingLine]:
    """Read a listing and return its instruction lines.

    Lines without source text (continuation of a long DB) and data lines
    are skipped. offset is added to every address (code assembled at one
    address and run at another).
    """
    try:
        # Split on "\n" only (as lcov/genhtml count lines): str.splitlines()
        # would also split on the form feed of pasmo page headers.
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            lines = [l.rstrip("\r") for l in f.read().split("\n")]
    except OSError as e:
        raise ConvertError(f"cannot read {path}: {e}") from e
    if fmt == "auto":
        fmt = detect_format(lines)
    regex = {"sjasmplus": _SJASM_RE, "pasmo": _PASMO_RE}.get(fmt)
    if regex is None:
        raise ConvertError(f"unknown listing format {fmt}")
    out: List[ListingLine] = []
    for no, text in enumerate(lines, start=1):
        m = regex.match(text)
        if not m:
            continue
        source = (m.group(3) or "").strip()
        if fmt == "pasmo" and source.startswith("C "):
            source = source[2:].strip()
        if not source or is_data(source):
            continue
        addr = (int(m.group(1), 16) + offset) & 0xFFFF
        out.append(ListingLine(no, addr, bool(_COND_RE.match(instruction_text(source)))))
    return out


def record(test_name: str, source: str, da: List[Tuple[int, int]],
           brda: List[Tuple[int, int, str, str]]) -> List[str]:
    """Lines of one lcov record (TN, SF, BRDA, BRF, BRH, DA, LF, LH)."""
    out = [f"TN:{test_name}", f"SF:{source}"]
    for line, block, taken, not_taken in brda:
        out.append(f"BRDA:{line},{block},0,{taken}")
        out.append(f"BRDA:{line},{block},1,{not_taken}")
    out.append(f"BRF:{2 * len(brda)}")
    hit = sum((t not in ("-", "0")) + (n not in ("-", "0")) for _, _, t, n in brda)
    out.append(f"BRH:{hit}")
    for line, count in da:
        out.append(f"DA:{line},{count}")
    out.append(f"LF:{len(da)}")
    out.append(f"LH:{sum(1 for _, c in da if c > 0)}")
    out.append("end_of_record")
    return out


def listing_record(test_name: str, listing: str, lines: List[ListingLine],
                   cov: Dict[int, AddrCoverage]) -> Tuple[List[str], set]:
    """lcov record of one listing; returns (lines, addresses it covers)."""
    da: List[Tuple[int, int]] = []
    brda: List[Tuple[int, int, str, str]] = []
    seen = set()
    for ll in lines:
        c = cov.get(ll.addr)
        seen.add(ll.addr)
        da.append((ll.line, c.executed if c else 0))
        if ll.conditional or (c and c.decision):
            if c is None or c.executed == 0:
                brda.append((ll.line, 0, "-", "-"))
            else:
                brda.append((ll.line, 0, str(c.taken), str(c.not_taken)))
    return record(test_name, os.path.abspath(listing), da, brda), seen


def pseudo_record(test_name: str, pseudo_path: str,
                  cov: Dict[int, AddrCoverage]) -> Tuple[List[str], List[str]]:
    """lcov record and the pseudo source text (one line per executed address)."""
    text: List[str] = []
    da: List[Tuple[int, int]] = []
    brda: List[Tuple[int, int, str, str]] = []
    for no, addr in enumerate(sorted(cov), start=1):
        c = cov[addr]
        extra = f"  [{' '.join(c.keys)}]" if c.keys else ""
        text.append(f"{addr:04X}h  {c.state}{extra}")
        da.append((no, c.executed))
        if c.decision:
            brda.append((no, 0, str(c.taken), str(c.not_taken)))
    return record(test_name, os.path.abspath(pseudo_path), da, brda), text


def convert(export: str, output: str, listings: List[str], fmt: str = "auto",
            offset: int = 0, test_name: str = "mz800emu") -> dict:
    """Write the tracefile; returns a summary for the caller."""
    status, cov = load_export(export)
    out: List[str] = []
    covered = set()
    pseudo = None
    if listings:
        for lst in listings:
            rec, seen = listing_record(test_name, lst, parse_listing(lst, fmt, offset), cov)
            out += rec
            covered |= seen
    else:
        pseudo = os.path.splitext(output)[0] + ".addresses.txt"
        rec, text = pseudo_record(test_name, pseudo, cov)
        with open(pseudo, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(text) + ("\n" if text else ""))
        out += rec
        covered = set(cov)
    with open(output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    return {
        "level": status.get("level"),
        "complete": status.get("complete"),
        "addresses": len(cov),
        "outside": len(set(cov) - covered),
        "pseudo": pseudo,
    }


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Convert an emulator code coverage export (JSON) to an lcov tracefile.")
    ap.add_argument("export", help="coverage export (JSON, format mz800emu-coverage/1)")
    ap.add_argument("-o", "--output", required=True, help="output tracefile (.info)")
    ap.add_argument("--listing", action="append", default=[],
                    help="assembler listing (repeatable); without it a pseudo source "
                         "file with addresses is written")
    ap.add_argument("--format", default="auto", choices=["auto", "sjasmplus", "pasmo"],
                    help="listing format (default: detect)")
    ap.add_argument("--offset", default="0",
                    help="added to listing addresses (code run at another address), "
                         "decimal or 0x hex")
    ap.add_argument("--test-name", default="mz800emu", help="TN of the tracefile")
    args = ap.parse_args(argv)
    try:
        offset = int(args.offset, 0)
        summary = convert(args.export, args.output, args.listing, args.format, offset,
                          args.test_name)
    except (ConvertError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if summary["complete"] is False:
        print("warning: the coverage data are incomplete (see status in the export)",
              file=sys.stderr)
    if summary["outside"]:
        print(f"note: {summary['outside']} executed addresses are outside the listing",
              file=sys.stderr)
    if summary["pseudo"]:
        print(f"pseudo source: {summary['pseudo']}", file=sys.stderr)
    print(f"written {args.output} ({summary['addresses']} executed addresses, "
          f"level {summary['level']})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
