# Breakpoint - action on trigger (Action mini-DSL)

A breakpoint action is a sequence of commands that runs when the BP
fires (= the `condition` returned true and the hit/skip counts have
elapsed). In the Edit BP dialog the action is selected by the Mode
radio button:

| Mode | Effect | Action string |
|------|--------|---------------|
| Stop | Classic BP - stops the emulator | (empty action, NULL) |
| Log only | Prints to TTY and continues | `log "BP <name>"` |
| Custom | Full mini-DSL script | the user's text |

This document describes the mini-DSL syntax for Custom mode in the
breakpoint system.

## "Stop vs continue" semantics

- **action == NULL or whitespace-only** -> the emulator stops
  (= classic BP stop).
- **action with any non-empty content** -> after evaluating all the
  commands the emulator continues (= implicit continue), unless the
  action ran `stop` or `exit`.
- **`stop`** stops the emulator like an empty action; **`exit`** ends the
  run with an exit code (see below). Both end the action: the commands
  after them do not run.

A BP that logs and then stops: `log "A=%X", A; stop`, or shorter
`stop "A=%X", A`. Stop on a condition: `if $hits >= 5 then stop`.

## Basic rules

- Commands are separated by a semicolon `;` or a newline.
- A comment starts at `#` and runs to the end of the physical line.
  A `#` inside `"..."` is not treated as a comment.
- Whitespace (outside strings) is ignored.
- Identifiers are case-insensitive (= consistent with the expression
  evaluator).
- Expression-type arguments are parsed by the same grammar as the
  condition - see `expression-syntax.md`.
- The parser is strict: an unknown command, wrong arity or an
  unterminated string literal = error; a BP with an unparsable action
  shows an inline error in the UI below the textbox, and the runtime
  behaves as an empty action (= stop fallback).

## Commands

The action mini-DSL provides 27 commands: 13 basic ones (log, continue,
disable_self, enable, disable, poke, set, mark, `$name` assignment,
clear_vars, if, stop, exit) and 14 forward commands for recording control (cdl_start,
cdl_stop, cdl_reset, cdl_export, trace_start, trace_stop, trace_save,
snapshot, ramlog_start, ramlog_stop, ramlog_clear, coverage_start,
coverage_stop, coverage_mark - see the "Forward
commands" section below).

### `log "fmt"[, expr...]`

Prints a formatted message to stdout (`[BP-LOG] ...\n`). The format
string is the first argument, a string literal in double quotes with
escape sequences `\n`, `\t`, `\"`, `\\`.

| Spec | Meaning |
|------|---------|
| `%X` | hex uppercase (`%04X` etc. **NOT supported** - only plain `%X`) |
| `%x` | hex lowercase |
| `%d` | signed decimal |
| `%u` | unsigned decimal (32-bit) |
| `%b` | binary (= no leading zeros, "0" for zero) |
| `%c` | character (low byte of the value as ASCII) |
| `%s` | arg = expression (address); lookup in sym_db, prints the symbol `name` or the placeholder `<no-symbol-at-0xADDR>` |
| `%%` | literal `%` |

Important: format width / padding (`%04X`, `%8d`) is **not supported**
by the parser - the spec is always a single character. If you want
zero-padded hex, create a fixed-width output externally (or use
multiple `log` calls).

Maximum of 8 arguments.

```
log "PC=%X A=%X HL=%X", PC, A, HL
log "frame=%d cycle=%d", Frame, Cycle
log "flags: C=%d Z=%d S=%d", Cf, Zf, Sf
log "JP target=%X (%s)", HL, HL          # %s lookup in sym_db
log "ret to %X (%s)", {SP}, {SP}         # %s with a 16-bit memory deref
```

Semantics of `%s`:

- arg is evaluated as a 16-bit address (low 16 bits, the rest is ignored)
- lookup in the global symbols ("CPU view", see `../symbols.md`)
- if a symbol is found, prints its `name` (= identifier from
  .lbl/.map/.noi/.sym)
- if not found, prints the placeholder `<no-symbol-at-0xADDR>` (4 hex
  digits)

A bank-specific lookup is not exposed in the action DSL.

The output goes to stdout. In the `--mcp-pipe` mode stdout carries the
MCP messages, so the emulator redirects its text output (including
`[BP-LOG]` lines) to stderr. The same text also goes into the channel
`bplog`, which an MCP client reads with `emu_trace_read(channel="bplog")`:
one row per `log` with the text, time, instruction address, PC and the
breakpoint number and name. The channel keeps the last 4096 rows, text
longer than 239 bytes is cut; `emu_bplog_clear` empties it (see
[tools-overview.md](../../mcp-server/tools-overview.md#emu_bplog_clear)).

In MEM_R / MEM_W / IORQ breakpoints `PC` is already behind the accessing
instruction; log `InsnPC` for the instruction address (see
[expression-syntax.md](expression-syntax.md#cpu-registers-z80)).

### `continue`

A no-side-effect marker. The default is already continue (see the
"Stop vs continue semantics" above), so `continue` only serves as
readable self-documentation for the script reader.

```
continue
```

### `disable_self`

Disables the BP that triggered the action (= one-shot pattern).

If the action is being evaluated outside of enforcement (= Test Eval,
manual trigger), the command prints a warning to stderr and is a no-op.

```
disable_self
```

### `enable <name>`

Activates the BP with the given name (= sets `enabled = true`). The
name is searched case-sensitively by linear scan over the breakpoint
`name`.

```
enable trace_isr
```

If the BP does not exist, the command prints a warning to stderr and
is a no-op. The argument is not quoted - the name is taken as the rest
of the line after the keyword + whitespace.

### `disable <name>`

Deactivates the BP with the given name (= `enabled = false`). Same
lookup semantics as `enable`.

```
disable trace_isr
```

The argument is mandatory; without it the parser reports the error
"disable: missing target name".

### `poke <addr> <value>`

Writes 1 byte into memory. Both arguments are expressions separated by
whitespace (= not by a comma). The split finds the first whitespace
outside parentheses / brackets / strings.

```
poke 0x8000 0x42
poke HL A
poke 0xE000 + Y A & 0x0F
```

`value` is masked to the low byte (`& 0xFF`); `addr` to 16 bits.

### `set <reg> <value>`

Writes a value into a Z80 register. `reg` is an identifier (alpha +
digits + `_`), case-insensitive. `value` is an expression.

Supported registers:

- 8-bit: `A`, `F`, `B`, `C`, `D`, `E`, `H`, `L`
- 16-bit: `AF`, `BC`, `DE`, `HL`, `IX`, `IY`, `SP`, `PC`
- 8-bit halves of index registers: `IXH`, `IXL`, `IYH`, `IYL`
- shadow pairs (16-bit): `AF'`, `BC'`, `DE'`, `HL'`
  (the apostrophe is part of the name; the parser allows it at the
  end of an identifier)
- special: `I`, `R`, `IM` (`& 3`), `IFF1` (`& 1`), `IFF2` (`& 1`)

```
set A 0x42
set HL 0x8000
set PC 0x100  # forced jump
set IFF1 1    # enable interrupts
set BC' 0x1234  # write into shadow BC'
set IXH 0xFF  # high byte of IX, low byte preserved
```

Outside of enforcement (without a CPU context) it prints a warning and
is a no-op.

### `mark "name"`

Marker into trace-suite + optional stdout printout. `name` is a string
literal in double quotes with escape sequences (like the `log` fmt).

```
mark "ISR enter"
mark "frame boundary"
```

**Two-branch behavior:**

When a BP with the `mark "name"` action fires, **each of the following
branches runs independently** according to configuration:

1. **Marklog binary record** - if `[TRACE_MARKLOG] mode` is
   `WITH_WINDOW` or `ALWAYS` (and the writer is active), a 24 B record
   is written into `<dir>/<name>.NNN.bin` with a pre-resolved
   `marker_id`. The id -> name mapping is in the manifest `<name>.json`.
2. **Stdout printout** `[BP-MARK] <name>` - if
   `[TRACE_MARKLOG].stdout_enabled` is `1` (default).

Default behavior without configuration = stdout only.

**Marker name limits:**

- Max 63 chars (+ NUL terminator = 64 B). Longer ones are truncated at
  parse time + a stderr warning.
- Max 65535 unique markers per session. Overflow = stderr warning, the
  marklog branch is a no-op for that BP action (stdout still works if
  cfg ON).

**Format details:** `docs/en/debugger/formats/MARK-log_format.md`

The marker ID is registered during BP parse. The hot-path fire then
calls no string operations - it writes a 24 B record from the
pre-resolved id.

### `$name = <expr>`, `$name <op>= <expr>`

Assignment to a user variable. `$name` is an identifier validated by
the regex `^[a-zA-Z_][a-zA-Z0-9_]*$` (= first character alpha or `_`,
continuation alpha / digits / `_`), max 31 characters. No reserved
keywords (= the `$` prefix is itself distinctive). Variable lifetime
is per-emulator-session; they are shared across all BPs and persist
into the `.bpt` JSON `vars` section plus a per-arch standalone
`.vars` file. Detail in `vars.md`.

Operators:

| Op | Semantics |
|----|-----------|
| `=` | plain assignment |
| `+=` | `cur + rhs` |
| `-=` | `cur - rhs` |
| `*=` | `cur * rhs` |
| `/=` | `cur / rhs` (division by zero -> 0) |
| `<<=` | shift left (rhs masked to 5 bits) |
| `>>=` | shift right |
| `&=` | bitwise AND |
| `\|=` | bitwise OR |
| `^=` | bitwise XOR |

If `$name` does not yet exist, the current value is 0 (= consistent
with the read-side default). A compound operator then works with 0 as
the left-hand side.

```
$hits += 1
$mask = 0xFF
$last_pc = PC
$accumulator |= 1 << bit_pos
```

A single `=` is rejected by the evaluator (= the condition expression
has no assignment); in the action DSL, on the contrary, assignment is
the only valid meaning.

### `clear_vars`

Clears the values of all existing `$name` variables (the entries
**remain** in storage; comment + persist_value flag are preserved).
Semantics "state machine reset" - the UI panel then sees the same
identifiers with value 0.

This is not the same as deleting the entries - to fully remove a
variable use the `x` button in the table row or the bulk "Delete"
button + selection.

```
clear_vars
```

### `if <expr> then <stmt> [else <stmt>]`

A single-line conditional. After evaluating `expr` (= an expression
per `expression-syntax.md`), if it is non-zero the `<stmt>` in the
then-body runs. If it is zero and an `else` branch is present, the
`<stmt>` in the else-body runs.

`else` is optional. There is no multi-statement block, no nested
`if/then/if/then` block (the parser does not technically forbid it,
but it is not tested).

For a multi-statement body use several separate `if`s with the same
condition, or a `$flag` variable:

```
if A == 0x42 then log "match A=0x42"
if A == 0x42 then $matched = 1
```

The keywords `then` and `else` must be whole words (= surrounded by
whitespace or at the end of the string), at the top-level depth (= not
inside `()` / `[]` / `{}` / a string).

```
if PC >= 0x1000 && PC < 0x2000 then log "in code seg PC=%X", PC
if $hits >= 5 then disable_self
if HL == 0 then mark "HL underflow"
if A == 0 then log "zero" else log "nonzero=%X", A
if $state == 1 then enable trace_bp else disable trace_bp
```

### `stop ["fmt"[, args...]]`

Stops the emulator like a BP with an empty action. The optional message
is a format string as in `log`, is printed as `[BP-STOP] ...` and goes
into the channel `bplog` (kind `stop`). The
commands after `stop` do not run.

```
stop
stop "buffer full at %X, HL=%X", PC, HL
if $hits >= 5 then stop "5th pass"
```

### `exit <expr>[, "fmt"[, args...]]`

Ends the run with the exit code `<expr>` (0-255; another value is
reduced to its low 8 bits with a warning). The optional message is a
format string as in `log`. The commands after `exit` do not run.

- In `--headless` without an MCP server (or with an unattended-run option,
  see [Headless mode](../../mcp-server/headless-mode.md#unattended-runs))
  the process ends with this code, writes the result file and finishes
  the video and audio recording.
- In the GUI, with `--mcp-pipe` and with an MCP TCP session the emulator
  only pauses: stderr shows the code, MCP clients get the event `paused`
  with `reason: "exit"` and `exit_code`, `emu_run` returns
  `stopped_by: "exit"`. The process keeps running.

Codes 0 (success), 1 (test failed), 2 (timeout) and 3 (error) have a
meaning in unattended runs; 64-255 are free for values reported by the
program.

```
exit 0
exit 1, "checksum mismatch: %X", HL
exit Value                     # IORQ_W / MEM_W BP: the code the program wrote
if A != 0 then exit 1 else exit 0
```

## Forward commands (recording control from a BP)

Forward commands let a breakpoint action run recording operations
automatically when the BP fires - the same effect as the corresponding
MCP / GUI call, but without manual intervention. The goal: a single
well-placed breakpoint instead of dozens of manual commands. On a hit
they run synchronously.

### `cdl_start`, `cdl_stop`, `cdl_reset`

Code/Data Logger (Memory Heatmap) lifecycle. No arguments. `cdl_start`
turns recording on, `cdl_stop` turns it off, `cdl_reset` zeroes the
counters.

```
cdl_start
cdl_reset
```

### `cdl_export "fmt"[, args...]`

Export the CDL data to a file. `fmt` is a file-name template with the
same specifiers and arguments as `log` (= `%X`, `%d`, `$variables`,
...).

```
cdl_export "cdl-dump.json"
cdl_export "cdl-%d.json", $id
```

### `trace_start <channel>`, `trace_stop <channel>`

Start / stop a binary trace-suite channel. `<channel>` is one of
`cputrack` / `iorqlog` / `intlog` / `hwlog` / `marklog` (case-sensitive, no quotes).

```
trace_start cputrack
trace_stop cputrack
```

### `trace_save <channel>, "fmt"[, args...]`

Save / redirect a trace channel segment. The channel name is followed
by a comma and a file-name template (same as the `log` fmt + args).

```
trace_save cputrack, "seg-%d.bin", $id
```

### `snapshot "fmt"[, args...]`

Save a .mzs snapshot. `fmt` is a file-name template (like the `log` fmt
+ args). Works from a continuing BP too (non-empty action = continue): the
action runs on the emulator thread between instructions (a safe-point), so
the snapshot is saved without manually pausing the emulator. This lets a
single continuing BP capture both a trace segment and a snapshot.

```
snapshot "snap.mzs"
snapshot "snap-%d.mzs", $id
```

### `ramlog_start [<from>, <to>]`, `ramlog_stop`, `ramlog_clear`

Control the RAM write log ([RAM Write Log](../ram-write-log.md)).
`ramlog_start` without arguments records writes into all memory; with
two expressions it records one range of CPU addresses. The expressions
are evaluated when the breakpoint hits (the order of the two values does
not matter). `ramlog_start` replaces a running log (ring mode, default
size). `ramlog_stop` stops recording (the records stay readable),
`ramlog_clear` empties the log.

```
ramlog_start
ramlog_start 0x4000, 0x40FF
ramlog_start HL, HL + 15
ramlog_stop
```

The expression function `lastwriter(addr)` returns the address of the
instruction that last wrote the cell at `addr`, e.g.
`if lastwriter(0x4000) == 0x3009 then ramlog_stop`.

### `coverage_start [branch|detail]`, `coverage_stop`, `coverage_mark`

Control code coverage (which instructions ran and which way the
conditional jumps went; read over MCP, see the `emu_coverage_*` tools
in [MCP tools overview](../../mcp-server/tools-overview.md)).
`coverage_start` without an argument collects the level `branch`,
`coverage_start detail` the level `detail`; data collected before with
the same settings are continued. `coverage_stop` stops collecting (the
data stay readable), `coverage_mark` starts a new epoch, so that the
code that runs for the first time after the mark can be listed.

```
coverage_start
coverage_start detail
coverage_mark
coverage_stop
```

All file-name templates (`cdl_export` / `trace_save` / `snapshot`)
share the same renderer as `log`, so you can generate numbered files
with `$variables`, e.g.:

```
snapshot "snap-%d.mzs", $id
$id += 1
```

For the MCP equivalents see the
[MCP tools overview](../../mcp-server/tools-overview.md); for trace
channel configuration see [Trace Suite](../Trace_Suite.md).

### Flood protection (rate limit + saturation)

Heavy forward actions (`snapshot`, `trace_save`) write to disk. To
keep a breakpoint that fires very frequently from flooding the
emulator or the disk, they have built-in protection:

- **Rate limit.** A minimum delay applies between two heavy actions of
  the same BP; firing faster is silently skipped. Optionally a hard
  cap on the number of saves per session can be set - once reached the
  BP disables itself. These limits are settable per BP (via MCP / GUI);
  otherwise the global default from configuration applies.
- **Byte saturation.** When the cumulative volume written by forward
  actions exceeds the threshold the emulator auto-pauses and emits a
  warning with a reason. It counts snapshots and traces saved by actions
  and the recording of trace channels that an action started or saves.
  A trace started only from the Trace Suite menu or over MCP is not
  counted; its own `max_total_mb` limit stops the recording and the
  emulation continues.
- Even when a continuing BP triggers a heavy action, pause and the
  emulator state query are served right after the action - the
  emulator does not get stuck. Other commands (reads, writes,
  breakpoint changes) wait for the end of the frame or for the pause,
  because the action can run in the middle of an instruction.

## Full examples

### Trace without stopping (= classic Log only equivalent)

```
log "PC=%X HL=%X A=%X", PC, HL, A
```

### Hit counter with one-shot disable

```
$hits += 1
log "hit #%d at PC=%X", $hits, PC
if $hits >= 5 then disable_self
```

### Conditional poke (= patch ROM call result)

```
if A == 0xFF then poke HL 0x00
if A == 0xFF then log "patched HL=%X", HL
```

### Cross-BP enable / disable (= state machine)

BP "isr_enter":
```
enable isr_trace
mark "ISR enter"
```

BP "isr_exit":
```
disable isr_trace
mark "ISR exit"
```

BP "isr_trace" (initially disabled):
```
log "ISR step PC=%X", PC
```

### Forced jump after N repetitions

```
$count += 1
if $count >= 100 then set PC 0x8000
if $count >= 100 then $count = 0
```

## Error states

### Parse-time

A lexer / parser error (bad keyword, unterminated string, bad `set` /
`poke` syntax, missing `then`, ...) is shown as an inline error below
the action textbox in the Edit BP dialog. The BP runtime behaves as
an empty action (= Stop fallback - the emulator stops) until you fix
the error.

### Runtime

Most runtime errors are tolerant with a stderr warning:

- `enable` / `disable` with a non-existent target -> warning, no-op.
- `set` with an unknown register -> warning, no-op.
- `set` without a CPU context -> warning, no-op.
- `disable_self` outside enforcement (= Test Eval) -> warning, no-op.
- `poke` outside addressable RAM (= banking) -> typically a no-op or a
  write into the bank-aliased area, depending on the memory subsystem
  behavior.

`/` and `%` in $var compound and in expr arguments: division by zero
-> 0.
