# Code coverage - which code ran and which way the branches went

Code coverage records, while a program runs, which instructions were
executed and which way every conditional instruction went: whether its
condition was met (the jump, call or return happened, the block
instruction repeated), was not met, or both. It answers "which parts of my
program did the test never reach?" and "which branch was never taken?".

Conditional instructions: `JR`, `JP`, `CALL` and `RET` with a condition,
`DJNZ`, `LDIR`, `CPIR`, `INIR`, `OTIR` and the decrementing forms
(`LDDR`, `CPDR`, `INDR`, `OTDR`). The result is taken from the CPU state
after the instruction, so `JR Z,$+2` is counted correctly although both
results continue at the same address.

Available on MZ-800, MZ-700 and MZ-1500: in the CPU Profiler window, as
marks in the Disassembly windows, over MCP, in breakpoint actions and from
the command line.

## Levels

- **branch** - by logical address (PC): executed addresses, conditional
  instructions and their results, the first instruction of an interrupt
  handler. Memory banks are not told apart: code at the same address in
  ROM and RAM, or in two MemExt banks, adds up. Slows the emulation by
  about 16-29 % (measured at MAX SPEED).
- **detail** - additionally one entry per code version: physical memory
  cell of the first byte (RAM, ROM, VRAM, PCG, MemExt bank), address and
  the opcode bytes. Self-modifying code that changes the instruction gets
  a new version, a changed operand is flagged. Each version has counters
  (executions, condition met, not met). The entries take 32 bytes each;
  the table has a fixed capacity, and what does not fit is reported. Slows
  the emulation by about 35-95 %, because the emulator has to record the
  bytes as the CPU fetches them.

The data are **complete** when nothing was dropped (full table), every
byte came from the CPU fetch and no counter overflowed. Reset, snapshot
load and replay seek keep the data and are counted as discontinuities.
An idle HALT (the CPU waiting for an interrupt) is not counted.

## Coverage tab in the CPU Profiler window

Open the CPU Profiler window (main menu `Debugger` -> `CPU Profiler`,
`Alt+Shift+P`) and select the **Coverage** tab.

Toolbar:

- **Level** - `branch` or `detail`; **Capacity** - size of the detail
  table (entries). Both apply when collecting starts.
- **Start** begins collecting. If data with the same level and capacity
  exist (also data collected from the command line or over MCP), it
  continues into them; otherwise it starts with empty data. **Stop**
  stops collecting, the data stay.
- **Reset** clears the data (collecting continues if it is running).
- **Mark** starts a new epoch. The filter **New since mark** then lists
  only code executed for the first time after the mark, for example what
  a key press or a new game level adds.
- **Export...** writes all rows into a file, JSON or CSV by the file
  extension (the same format as MCP `emu_coverage_export`).

Filter:

- **Show** - **Partial branches** (conditional instructions with one
  result only, or none yet), **Both branches**, **All conditional**,
  **New since mark**, **All**.
- **Range** - only addresses within the range (hex).

Columns:

| Column | Meaning |
|---|---|
| Addr | address of the instruction (logical PC) |
| Symbol | nearest loaded symbol at or below the address, `+N` = offset |
| State | `none` = not conditional; `both`, `taken_only`, `not_taken_only` = which results of the condition were seen; `unobserved` = none yet |
| Exec | executions of this code version (detail) |
| Taken / Not taken | how many times the condition was met / not met (detail) |
| Cell | physical memory cell of the first byte and its offset (detail); `bus` = no physical cell (graphic VRAM, mapped ports) |
| Key | bytes that tell code versions apart (prefixes and opcode, detail) |
| Epoch | epoch of the first execution |
| Flags | `int` = first instruction of an interrupt handler, `smc` = the operand changed, `cmt` = the CMT hack stepped in, `halt`, `fallback` = a byte was read from memory instead of the CPU fetch, `saturated` = a counter overflowed, `bus`, `prefixes`, `long` |

At the branch level the table has one row per executed address and no
counters; at the detail level one row per code version.

A left click on a row opens the address in Disassembly #1, a right click
offers a breakpoint and Disassembly #2-#5.

The status line shows whether collecting runs, the level, the number of
instructions, the conditional instructions and their results, whether the
data are complete (the tooltip lists why not), the epoch and the number
of rows shown.

The window asks the emulator for the data about twice a second while
collecting runs (the detail table once a second), only while the tab is
shown.

## Marks in the Disassembly windows

While coverage data exist, the Disassembly windows (#1-#5) show a colored
mark next to every executed instruction, to the right of the breakpoint
and PC icons:

| Mark | Meaning |
|---|---|
| green | executed (not conditional), or a conditional instruction with both results |
| yellow | condition always met (taken only) |
| orange | condition never met (not taken only) |
| no mark | not executed |

The marks follow the logical address, so they also appear on code at the
same address in another memory bank. The row tooltip has a **Coverage**
section with the state and, at the detail level, the code versions at
the address with their cells and counters.

The marks are turned off in the debugger window menu `Settings` ->
`Disassembled` -> `Show code coverage` (saved in the configuration).

## Command line and configuration

- `--coverage branch|detail` - collect from the start.
- `--coverage-key opcode|full_bytes` - detail key: opcode bytes (default)
  or the first 4 bytes including operands (every operand value is then
  its own version).
- `--coverage-capacity N` - detail table size, 1-1048576 entries
  (default 65536).
- `--coverage-save-on-exit FILE` - write all rows when the emulator exits
  (`.csv` = CSV, otherwise JSON; a relative path is relative to the
  current directory the emulator was started from, as with
  `--result-json`).

The same settings in the INI section `[COVERAGE]`: `mode` (`OFF`,
`BRANCH`, `DETAIL`), `key`, `capacity`, `reset_on_discontinuity` (clear
the data on reset, snapshot load and replay seek instead of counting
them). A headless run with `--result-json` puts the coverage status into
`result.json` ([Headless mode](../mcp-server/headless-mode.md)).

## Breakpoint actions

`coverage_start` (level branch), `coverage_start detail`,
`coverage_stop`, `coverage_mark`. Example: start collecting when the game
enters a level and mark the moment of a key press. See
[Action DSL](breakpoints/action-dsl.md).

## MCP

`emu_coverage_start`, `emu_coverage_stop`, `emu_coverage_reset`,
`emu_coverage_status`, `emu_coverage_mark`, `emu_coverage_read` (pages
with a filter) and `emu_coverage_export`. See
[MCP tools](../mcp-server/tools-overview.md) and the MCP document
`emulator://docs/coverage`.

## Coverage of the source code (lcov)

`docs/tools/coverage_to_lcov.py` converts an export (JSON) into an lcov
tracefile, which `genhtml` turns into an HTML report and which editor
coverage extensions can show. With the assembler listing (sjasmplus
`--lst`, pasmo `--listing`) the lines of the listing get their execution
counts and the conditional instructions their two branches:

```bash
python3 docs/tools/coverage_to_lcov.py coverage.json -o coverage.info --listing game.lst
genhtml coverage.info -o coverage-html
```

Without a listing the script writes a pseudo source file with one line
per executed address. Details: [docs/tools/README.md](../../tools/README.md).

## Limits

- The branch level does not tell memory banks apart; use detail for
  banked code or overlays.
- At the detail level the marks and the lcov counts add up all code
  versions at the address.
- With coverage collected, the emulation is slower (see Levels); after
  Stop the emulation runs at full speed again.
