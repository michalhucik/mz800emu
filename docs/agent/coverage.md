# Which code ran and which branches were taken (code coverage, emu_coverage_*)

Recipe for an AI client that needs to know which instructions of a
program ran, which conditional jumps went both ways and which only one
way, which code is new after an event (a key press, a level), or which
version of self-modifying code ran. Use `emu_coverage_start`,
`emu_coverage_read`, `emu_coverage_status`, `emu_coverage_mark`,
`emu_coverage_export`.

## Two levels

| Level | Key | What it records | Cost |
|-------|-----|-----------------|------|
| `branch` | logical PC | executed addresses; for every conditional instruction whether the condition was true and/or false; first instruction of an interrupt handler | about +16-29 % emulation time (bytes decoded from memory after the step) |
| `detail` | physical cell of the first byte + PC + code version | everything of `branch` plus one unit per code version with `exec`, `taken`, `not_taken`, last operand, last next PC, flags | about +78-115 % emulation time (bytes from the instruction fetch, forces the slow memory callbacks) |

Choose `branch` to find untested code and half-covered branches. Choose
`detail` when banking or self-modifying code matters, or when the
counters are needed.

## Recipe

1. `emu_coverage_start(level="branch", reset=true)`.
2. Run the program (`emu_run`, keystrokes, a replay of a recording).
3. `emu_coverage_status` - `complete`, `units`, `branches`
   (`decisions`, `both`, `taken_only`, `not_taken_only`).
4. `emu_coverage_read(filter={"state": "partial"})` - conditional
   instructions where only one branch ran. `filter={"state": "decision"}`
   lists all conditional instructions, `pc_from` / `pc_to` restrict the
   range. Continue with `cursor=<next_cursor>` until `end_of_data`.
5. `emu_coverage_stop` when done; the data stay readable.

What does an action add? `emu_coverage_mark()` before it, then
`emu_coverage_read(filter={"new_since_epoch": <epoch>})` lists only
code that ran for the first time after the mark.

Comparing two runs: export both (`emu_coverage_export(path, "json")`)
and compare the row sets.

## Branch state

| `state` | Meaning |
|---------|---------|
| `none` | not a conditional instruction (JP, CALL, RET, JR without condition, RST, everything else) |
| `unobserved` | conditional, but no result recorded |
| `taken_only` | the condition was only true: jump / call / return happened, or a block instruction (`LDIR`, `CPIR`, `INIR`, `OTIR` and the decrementing forms) repeated |
| `not_taken_only` | the condition was only false: execution continued after the instruction, or the block instruction finished in one step |
| `both` | both results seen |

Conditional instructions: `JR cc`, `JP cc`, `CALL cc`, `RET cc`, `DJNZ`
and the 8 repeating block instructions. The result is derived from the
CPU state after the step: the flags for `cc`, B for `DJNZ`, PC back on the
instruction for a block repeat. `JR Z,$+2` therefore counts correctly even
though both results end at the same address.

## Rows

Rows are arrays in the order of `columns` (default all columns of the
level), sorted by PC.

`branch`: `pc`, `state`, `int_entry` (first instruction of an accepted
interrupt / NMI handler), `first_epoch`.

`detail`:

| Column | Meaning |
|--------|---------|
| `pc` | logical address of the step |
| `space`, `offset` | physical cell of the first byte: `ram`, `memext` (with a connected MemExt the RAM pages are MemExt banks - the default setup), `rom` (offset = CPU address), `vram` (MZ-700 text VRAM / CG-RAM), `pcg` (MZ-1500), `bus` (MZ-800 graphic VRAM or mapped ports: no physical cell, offset = CPU address) |
| `key` | code version in hex: opcode bytes (prefixes, CB/ED and the opcode; for `DD CB d op` also the last byte), or the first 4 bytes of the step with `key="full_bytes"` |
| `len` | bytes of the step |
| `exec`, `taken`, `not_taken` | counters (saturate at 4294967295 with flag `saturated`) |
| `state` | branch state from the counters |
| `operand` | last operand (first two bytes after the opcode, little-endian; `null` without operand) |
| `next_pc` | PC after the last execution (target of `RET`, `JP (HL)`, `RST`) |
| `first_epoch` | epoch in which the unit appeared |
| `flags` | `decision`, `operand_varied` (the operand changed between executions - self-modifying code), `int_entry`, `cmt_trap` (the CMT hack loaded data during this instruction), `fetch_fallback`, `saturated`, `halt`, `cell_bus`, `prefix_split`, `key_truncated` |

## Things to know

- A CPU step is one row: a chain of `DD` / `FD` prefixes and the
  instruction after it are one step (up to 64 prefixes); a lone `DD 00`
  is one 2-byte step.
- Idle HALT (the CPU waiting for an interrupt) is not counted
  (`halt_idle` in the status); the HALT instruction itself is.
- `branch` decodes the bytes from memory after the step: an instruction
  that overwrites itself or switches the banking under itself is decoded
  from the new content. `detail` uses the fetched bytes. Its cell is
  taken from the mapping after the step.
- Completeness: `complete` is false when a unit did not fit
  (`dropped`, raise `capacity`), a byte was not in the fetch record
  (`fetch_fallback`) or a counter saturated.
- Reset, snapshot load and replay seek / rewind do not clear the data,
  they increase `discontinuities` (start with
  `reset_on_discontinuity=true` to clear instead). `host_state_edits`
  counts PC or AF changes by the debugger.
- `emu_coverage_reset` or a new start with other settings starts a new
  `generation`; an older cursor then fails with `segment_replaced:`.
- Rows added while collecting at an address before the cursor appear
  only when reading again from the start.
- Deterministic coverage of a run: record the inputs once
  (`emulator://docs/replay`) and collect coverage while replaying.
- The level `branch` keeps its own data; `emu_profiler_pc_bitmap` (the
  executed addresses of the PC profiler) stays independent.
- A user sees the same data in the GUI (Coverage tab of the CPU Profiler
  window, colored marks in the Disassembly windows); a collection started
  over MCP shows up there and the user can stop or reset it.
- Source line coverage: `docs/tools/coverage_to_lcov.py` converts an
  export (JSON) with a sjasmplus or pasmo listing into an lcov tracefile
  (`DA` per listing line, `BRDA` 0 = condition met, 1 = not met).
