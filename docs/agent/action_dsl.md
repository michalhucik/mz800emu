# BP Action DSL

A breakpoint's action is a sequence of commands that runs when the BP
fires (= condition was true and hit / skip counts have elapsed). The
action grammar is referred to as the "Action mini-DSL".

For the authoritative document with all corner cases see
`docs/en/debugger/breakpoints/action-dsl.md`.

## Stop vs continue semantics

- **action == NULL or whitespace-only** -> emulator stops (classic BP).
- **action with any non-empty content** -> after evaluating all
  commands the emulator continues (implicit continue), unless it ran
  `stop` or `exit`.
- `stop` stops like an empty action, `exit <code>` ends the run with an
  exit code (headless) or only pauses (GUI, MCP: `emu_run` returns
  `stopped_by: "exit"`, the `paused` event has `reason: "exit"` and
  `exit_code`). Both end the action - commands after them do not run.

"Log and then stop": `stop "A=%X", A`. Stop on the 5th pass:
`$hits += 1; if $hits >= 5 then stop`.

## Lexical rules

- Commands separated by `;` or newline.
- `#` starts a line comment (ignored inside `"..."`).
- Whitespace outside strings is irrelevant.
- Identifiers are case-insensitive (consistent with expression
  evaluator).
- Expression-type arguments use the BP DSL grammar - see
  `emulator://docs/bp_dsl`.
- Parser is strict: unknown command, wrong arity or an unterminated
  string literal = error. A BP with an unparsable action shows an
  inline error in the UI and at runtime behaves as empty action
  (= stop fallback).

## Commands (cheat sheet)

| Command | One-liner | Example |
|---------|-----------|---------|
| `log "fmt"[, expr...]` | Printf-style message to stdout, max 8 args | `log "PC=%X A=%X", PC, A` |
| `continue` | No-op marker (default is already continue) | `continue` |
| `disable_self` | Disable the BP that just fired (one-shot pattern) | `disable_self` |
| `enable <name>` | Activate another BP by name (case-sensitive) | `enable trace_isr` |
| `disable <name>` | Deactivate another BP by name | `disable trace_isr` |
| `poke <addr> <value>` | Write 1 byte into Z80 memory map | `poke 0x8000 0x42` |
| `set <reg> <value>` | Write into Z80 register | `set HL 0x8000` |
| `mark "name"` | Trace-marker (marklog binary + optional stdout) | `mark "ISR enter"` |
| `$name = <expr>` (+ compound ops) | Assign to user variable | `$hits += 1` |
| `clear_vars` | Reset all `$name` values to 0 (entries preserved) | `clear_vars` |
| `if <expr> then <stmt> [else <stmt>]` | Single-line conditional | `if $hits >= 5 then disable_self` |
| `stop ["fmt"[, expr...]]` | Stop the emulator (optional message `[BP-STOP] ...`, also in the `bplog` channel), ends the action | `stop "HL=%X", HL` |
| `exit <expr>[, "fmt"[, expr...]]` | End the run with exit code 0-255 (headless without MCP; elsewhere only pause), ends the action | `exit Value` |
| `cdl_start` / `cdl_stop` / `cdl_reset` | CDL (Memory Heatmap) lifecycle | `cdl_start` |
| `cdl_export "fmt"[, expr...]` | Export CDL data to file (templated name) | `cdl_export "dump-%d.cdl", $id` |
| `trace_start <chan>` | Start a trace channel | `trace_start cputrack` |
| `trace_stop <chan>` | Stop a trace channel | `trace_stop cputrack` |
| `trace_save <chan>, "fmt"[, expr...]` | Save / redirect channel segment | `trace_save cputrack, "seg-%d.bin", $id` |
| `snapshot "fmt"[, expr...]` | Save `.mzs` snapshot (works from a continuing BP too) | `snapshot "snap-%X.mzs", PC` |
| `ramlog_start [<from>, <to>]` | Start the RAM write log: all memory, or one range of CPU addresses (expressions evaluated at hit) | `ramlog_start HL, HL + 15` |
| `ramlog_stop` / `ramlog_clear` | Stop / empty the RAM write log | `ramlog_stop` |
| `coverage_start [branch\|detail]` | Start code coverage (default `branch`); continues existing data with the same settings | `coverage_start detail` |
| `coverage_stop` / `coverage_mark` | Stop code coverage / start a new coverage epoch | `coverage_mark` |

## `log` format specifiers

The first argument is a double-quoted format string with escape
sequences `\n`, `\t`, `\"`, `\\`. Remaining args are BP DSL
expressions evaluated at fire time.

| Spec | Meaning |
|------|---------|
| `%X` | hex uppercase |
| `%x` | hex lowercase |
| `%d` | signed decimal |
| `%u` | unsigned decimal (32-bit) |
| `%b` | binary (no leading zeros) |
| `%c` | character (low byte as ASCII) |
| `%s` | arg = expression (address); looks up name in sym_db, prints symbol or `<no-symbol-at-0xADDR>` |
| `%%` | literal `%` |

**Width / padding (`%04X`, `%8d`) is NOT supported** - the parser
reads a single character spec. If you need zero-padded hex emit it
via multiple `log` calls or fixed-width construction externally.

Max 8 expression arguments per `log` call. Output goes to stdout as
`[BP-LOG] <formatted>\n` (under `--mcp-pipe` to stderr, since stdout
carries MCP JSONL) and into the channel `bplog`: read it with
`emu_trace_read(channel="bplog")` - one row per `log` with the text, time,
`insn_pc`, `pc` and the breakpoint id and name (see
`emulator://docs/trace_read`, section "Breakpoint action output"). The
ring keeps the last 4096 rows. In MEM_R / MEM_W / IORQ BPs log `InsnPC`,
not `PC` (the live PC is behind the accessing instruction).

```
log "PC=%X A=%X HL=%X", PC, A, HL
log "frame=%d cycle=%d", Frame, Cycle
log "JP target=%X (%s)", HL, HL          # %s looks up symbol at HL
log "ret to %X (%s)", {SP}, {SP}         # %s with 16-bit memory deref
```

## Register writes (`set <reg> <value>`)

`<reg>` is a Z80 register identifier (case-insensitive). `<value>`
is a BP DSL expression. Outside of BP enforcement (= Test Eval, no
CPU context) the command prints a stderr warning and is a no-op.

Supported register identifiers:

- 8-bit: `A`, `F`, `B`, `C`, `D`, `E`, `H`, `L`
- 16-bit: `AF`, `BC`, `DE`, `HL`, `IX`, `IY`, `SP`, `PC`
- 8-bit halves of index registers: `IXH`, `IXL`, `IYH`, `IYL`
- shadow pairs (16-bit): `AF'`, `BC'`, `DE'`, `HL'` (apostrophe is
  part of the name)
- special: `I`, `R`, `IM` (low 2 bits), `IFF1` (low bit),
  `IFF2` (low bit)

```
set A 0x42
set PC 0x100      # forced jump
set IFF1 1        # enable interrupts
set BC' 0x1234    # write to shadow BC'
set IXH 0xFF      # high byte of IX, low byte preserved
```

## Memory writes (`poke`)

`poke <addr> <value>` writes 1 byte through the Z80 memory map.
Both arguments are expressions separated by **whitespace** (not
comma). The split finds the first top-level whitespace outside
`(...)`, `[...]`, `{...}` or strings.

```
poke 0x8000 0x42
poke HL A
poke 0xE000 + Y A & 0x0F
```

`value` is masked to low byte (`& 0xFF`), `addr` to 16 bits. Writes
into ROM-mapped regions or unmapped pages follow the current banking
behavior (= typically no-op or bank-aliased write, see
`emulator://docs/memory_layout` for safe RAM ranges per platform).

## User variables (`$name`)

`$name = <expr>` or compound `$name <op>= <expr>`. The full
identifier rules, persistence, lifecycle and persist_value flag are
described in `emulator://docs/smart_vars`. Operators in the action
DSL: `=`, `+=`, `-=`, `*=`, `/=`, `<<=`, `>>=`, `&=`, `|=`, `^=`.
Non-existent `$name` reads as 0 on the LHS of a compound op.

`clear_vars` zeroes all existing `$name` values; entries (name,
comment, persist_value flag) stay in storage.

```
$hits += 1
$mask = 0xFF
$last_pc = PC
$accumulator |= 1 << bit_pos
clear_vars
```

## Single-line conditional

```
if <expr> then <stmt> [else <stmt>]
```

`else` is optional. No multi-statement block, no nested
`if/then/if/then`. For multi-statement bodies use several `if`s with
the same condition, or set a `$flag` first:

```
if PC >= 0x1000 && PC < 0x2000 then log "in code seg PC=%X", PC
if $hits >= 5 then disable_self
if A == 0 then log "zero" else log "nonzero=%X", A
if $state == 1 then enable trace_bp else disable trace_bp
```

The keywords `then` and `else` must be whole words at top-level depth
(not inside `()` / `[]` / `{}` / a string).

## Markers (`mark "name"`)

Trace-suite marker with optional stdout printout. `name` is a
double-quoted string literal (same escapes as `log`).

Two independent branches run when the BP fires:

1. **Marklog binary record** - if `[TRACE_MARKLOG] mode` is
   `WITH_WINDOW` or `ALWAYS` (and the writer is active), a 24 B
   record is written into `<dir>/<name>.NNN.bin` with a pre-resolved
   `marker_id`. ID -> name mapping is in the manifest `<name>.json`.
2. **Stdout printout** `[BP-MARK] <name>` - if
   `[TRACE_MARKLOG].stdout_enabled` is `1` (default).

Limits: max 63 chars per marker name, max 65535 unique markers per
session. Format details: `docs/cz/debugger/formats/MARK-log_format.md`.

## Cross-BP enable / disable

`enable <name>` and `disable <name>` look up another BP by its `name`
field (case-sensitive linear scan). The argument is **not quoted** -
the name is the rest of the line after the keyword + whitespace.

If the target BP does not exist, the command prints a stderr warning
and is a no-op. `disable_self` outside of BP enforcement (Test Eval
or manual trigger) also warns + no-ops.

## Full examples

### Trace without stopping

```
log "PC=%X HL=%X A=%X", PC, HL, A
```

### Hit counter with one-shot disable

```
$hits += 1
log "hit #%d at PC=%X", $hits, PC
if $hits >= 5 then disable_self
```

### Cross-BP state machine

BP `isr_enter`:
```
enable isr_trace
mark "ISR enter"
```

BP `isr_exit`:
```
disable isr_trace
mark "ISR exit"
```

BP `isr_trace` (initially disabled):
```
log "ISR step PC=%X", PC
```

### Forced jump after N hits

```
$count += 1
if $count >= 100 then set PC 0x8000
if $count >= 100 then $count = 0
```

## Forwarding commands (CDL / trace / snapshot)

These commands automate the "one smart BP instead of dozens of manual
MCP calls" workflow. When the BP fires they synchronously trigger the
exact same effect as the matching MCP / dbgapi call - the trace
commands share the very same core (`dbgapi_trace_lifecycle`) that the
MCP `trace_*` tools use, CDL commands call `mhmap_*` directly, and
`snapshot` calls `snapshot_save`. No drift between the DSL and MCP
paths.

### CDL (Memory Heatmap)

| Command | Effect |
|---------|--------|
| `cdl_start` | `mhmap_set_mode(ALWAYS)` - start recording |
| `cdl_stop` | `mhmap_set_mode(OFF)` - stop, data kept |
| `cdl_reset` | `mhmap_reset()` - zero all counters, mode unchanged |
| `cdl_export "fmt"[, args]` | `mhmap_export(rendered)` - write meta JSON + `*_bus.cdl`/`*_ram.cdl` files |

### Trace suite

`<chan>` is one of `cputrack`, `iorqlog`, `intlog`, `hwlog`, `marklog`
(case-sensitive). Unknown channel = parse error.

| Command | Effect |
|---------|--------|
| `trace_start <chan>` | Channel mode -> ALWAYS + callback recompute |
| `trace_stop <chan>` | Channel mode -> OFF |
| `trace_save <chan>, "fmt"[, args]` | Flush + redirect segment to the rendered path (NEXT segment) |

`trace_save` semantics follow the dbgapi `TRACE_SAVE` handler: it
closes the current segment and points the next one at the rendered
filename. Already-written chunk files are not renamed (the tlog writer
binds names at open time - see Z17a result).

### Snapshot

`snapshot "fmt"[, args]` writes a `.mzs` snapshot to the rendered path.
It works from BOTH a stopping BP and a **continuing** BP (non-empty
action = implicit continue). The action enforce callback runs on the
emulator thread between instructions (a safe-point, CPU not advancing),
so the `snapshot` command raises a dedicated `snapshot_safepoint` flag
for the duration of the save and clears it afterwards. This flag is an
independent channel from the `paused` guard (the save-time check in
`snapshot_mgr.c` accepts either), kept separate so it does not disturb
the headless frame counter the way a transient `paused` toggle did
(0018/0019). This realises the "one BP"
vision (0018): trace segment + `.mzs` snapshot from a single continuing
BP, no client round-trip.

### RAM write log

| Command | Effect |
|---------|--------|
| `ramlog_start` | Same as `emu_ramlog_start` without `ranges` (all memory, ring, default capacity); replaces a running log |
| `ramlog_start <from>, <to>` | One range of CPU addresses; both expressions evaluated when the BP fires, order of the two values does not matter |
| `ramlog_stop` | Same as `emu_ramlog_stop` (records stay readable) |
| `ramlog_clear` | Same as `emu_ramlog_clear` |

Use `lastwriter(addr)` in conditions (`emulator://docs/bp_dsl`), e.g.
`if lastwriter(0x4000) == 0x3009 then ramlog_stop`. Recipe:
`emulator://docs/ramlog`.

### Code coverage

| Command | Effect |
|---------|--------|
| `coverage_start` | Same as `emu_coverage_start(level="branch")` without `reset`: continues existing data with the same settings, otherwise starts new data |
| `coverage_start detail` | The same with `level="detail"` (key `opcode`, default capacity) |
| `coverage_stop` | Same as `emu_coverage_stop` (data stay readable) |
| `coverage_mark` | Same as `emu_coverage_mark`: new epoch, filter `new_since_epoch` then lists only code that ran after the mark |

Typical use: a breakpoint on the start of a level does `coverage_mark`,
the agent then reads what the level added. Started at `detail` inside an
instruction (a MEM_R / MEM_W breakpoint), that one step may count as
`fetch_fallback`. Recipe: `emulator://docs/coverage`.

### Filename templates and `$vars`

The filename argument of `cdl_export`, `trace_save` and `snapshot` is
a printf-style template using the **same renderer and specifiers as
`log`** (`%X %x %d %u %b %c %s %%`, no width/padding) with the same
comma-separated expression arguments. This makes numbered segments
trivial:

```
$id += 1
trace_save cputrack, "seg-%d.bin", $id
```

### Runtime errors

Forwarding commands are tolerant: an I/O failure, an unknown channel
or a snapshot on a running emulator prints a stderr warning
(`[BP-ACTION] <cmd> '<path>' failed ...`) and is a no-op; the emulator
keeps running.

### Rate limit on heavy forward actions (0019)

`snapshot` and `trace_save` are heavy disk I/O actions. To prevent a
hot breakpoint from saturating the disk, each breakpoint enforces a
per-BP rate limit before the action runs:

- `fwd_min_interval_ms` - minimum delay between two fires (default `0`
  = fall back to the global default, then the built-in `250` ms).
  Firing earlier is silently skipped (soft guard).
- `fwd_max_fires` - hard cap on successful fires per session (default
  `0` = unlimited). Reaching the cap disables the breakpoint itself.

Both override fields are settable from production via `emu_bp_update`
/ `emu_bp_create_with_init` (`fields: ["fwd_min_interval_ms",
"fwd_max_fires"]`) and persist into the `.bpt` file. The global
default for breakpoints without an explicit override is the INI key
`[BREAKPOINTS] fwd_default_min_interval_ms`. A cumulative byte
backstop (`[BREAKPOINTS] fwd_byte_limit_mb`, default 256 MB) auto-pauses
the emulator if heavy actions keep writing past the threshold (run result
`stopped_by: "bp_action_saturation"`). It counts snapshots and traces
saved by actions and the recording of trace channels an action started
(`trace_start`) or saves (`trace_save`); a trace started only with
`emu_trace_start` is limited by its own `max_total_mb` instead.

Control-plane robustness: a continuing BP running a heavy forward
action no longer wedges the emulator. At the BP action boundary only
`pause`, `force_pause` and `get_state` are served. The boundary can be
in the middle of an instruction (a MEM_W BP fires before the write), so
all other commands (register and memory reads, writes, breakpoint
changes) wait for the end of the frame or for the pause and never see a
half-executed instruction. Pause first, then change breakpoints.

## Vision 0017: one smart BP on `0x0005`

The classic mzdos-team workflow used dozens of manual MCP calls to
checkpoint state around the CP/M BDOS entry (`0x0005`). With the
forwarding commands a single BP does it synchronously:

BP on PC == `0x0005`, range-scoped (Z17b) to the area of interest,
action:

```
$id += 1
trace_save cputrack, "bdos-%d.bin", $id
snapshot "bdos-%d.mzs", $id
```

Each hit closes the previous trace segment, opens a fresh one and
(when the BP stops the emulator) writes a matching snapshot, all keyed
by the incrementing `$id`. No round-trip to an external MCP client per
event. Note that dynamic BP creation from an action (original 0017
item 5) is intentionally **not** implemented - the range-scope feature
(Z17b) covers that need.

## Common gotchas

- `log` format width / padding (`%04X`, `%8d`) is **not supported**.
- `log` max 8 expression arguments.
- `set <reg>` and `poke` use **whitespace** between args, not comma
  (in contrast to `log` which uses comma between fmt and exprs).
- `enable` / `disable` use the BP's `name` field, not its numeric
  ID. Names are case-sensitive.
- A single `=` in a condition expression is a parse error; in the
  action DSL it is the only valid form for `$name = expr`.
- `disable_self` only works inside real BP enforcement; in UI Test
  Eval it warns + no-ops.
- Division / modulo by zero in `$var` compound RHS or in argument
  expressions returns 0 (tolerant evaluator).
- `set PC <expr>` forces a jump on the **next** instruction step;
  the current instruction has already been fetched.
- `poke` honours current banking - to write into RAM mirrored
  underneath ROM you may need to flip the bank first (see
  `emulator://docs/memory_layout` and ports `0xE0..0xE6`).

## Error states

### Parse-time

Lexer / parser errors (bad keyword, unterminated string, bad `set` /
`poke` syntax, missing `then`, ...) appear as an inline error below
the action textbox in the Edit BP dialog. The BP runtime behaves as
empty action (= Stop fallback) until you fix the error.

### Runtime

Most runtime errors are tolerant with a stderr warning:

- `enable` / `disable` with non-existent target -> warning, no-op.
- `set` with unknown register -> warning, no-op.
- `set` without a CPU context -> warning, no-op.
- `disable_self` outside enforcement (= Test Eval) -> warning, no-op.
- `poke` outside addressable RAM -> typically no-op or bank-aliased
  write, depending on the memory subsystem.

## Related

- `emulator://docs/bp_dsl` - expression syntax used in every action
  argument (condition exprs, `log` args, `set <reg> <expr>`, etc.).
- `emulator://docs/smart_vars` - full `$name` user-variable model
  (storage, persistence, `.bpt` + `.vars` files, persist_value flag).
- `emulator://docs/memory_layout` - safe RAM ranges per platform,
  banking ports for `poke` planning.
- `docs/en/debugger/breakpoints/action-dsl.md` - authoritative
  reference with all corner cases.
- `docs/en/debugger/breakpoints/hw-events.md` - per-BP-type
  `Address` / `Value` semantics relevant for `log` arguments.
