# Breakpoints - documentation

This directory contains the user documentation for the breakpoint
subsystem of the mz800emu emulator. The goal of the subsystem is to
provide "smart" breakpoints that are more than a classic PC stop - they
have a type system (memory access, I/O, IRQ, HW event, ...), match
modes (single / range / mask), an optional condition expression and an
action mini-DSL.

## What breakpoints can do

- 9 BP types (PC_EXEC, MEM_R, MEM_W, IORQ_R, IORQ_W, IRQ, HW_EVENT,
  SP_THRESHOLD, GLOBAL, IRQ_SIG) - see `types.md`
- Match modes SINGLE / RANGE / MASK for address, port and bank fields -
  see `match-modes.md`
- IORQ port mode 8BIT / 16BIT - see `match-modes.md`
- IRQ post-dispatch filter with IM mode discriminator + IM 2 vector /
  ISR filter + IM 0 RST opcode mask - see `irq-filter.md`
- IRQ_SIG pre-dispatch peripheral source filter - see `irq-sig.md`
- HW_EVENT vocabulary of 28 events in 4 categories + trigger conditions -
  see `hw-events.md`
- Condition expression evaluator with signed 32-bit arithmetic, CPU
  registers, flags, shadow regs, memory deref, I/O port read, $vars -
  see `expression-syntax.md`
- Action mini-DSL with 11 commands (log, set, poke, $var, if, enable,
  disable, mark, ...) - see `action-dsl.md`
- Persistence in a JSON `.bpt` format with tolerance for older files -
  see `persistence.md`
- Live Test Eval with real CPU context

## Document map

| File | Description | Open when |
|------|-------------|-----------|
| `README.md` | this file (orientation) | first time |
| `types.md` | catalogue of 9 BP types, feature matrix, decision tree | choosing a type for a new BP |
| `match-modes.md` | SINGLE / RANGE / MASK + IORQ port_mode + SP WINDOW | setting an address range / mask |
| `expression-syntax.md` | condition expression grammar | writing a `condition` |
| `action-dsl.md` | commands of the action mini-DSL | writing a script into `action` |
| `hw-events.md` | 28 HW events vocabulary + trigger conditions | type HW_EVENT |
| `irq-filter.md` | post-dispatch IRQ filter (IM mode + vector + ISR + RST) | type IRQ |
| `irq-sig.md` | pre-dispatch peripheral source filter | type IRQ_SIG |
| `persistence.md` | `.bpt` JSON schema + BC table | editing `.bpt` manually / migrating |
| `groups.md` | BP groups (hierarchy + cascade enable + drag-drop) | organizing BPs into groups |
| `test-eval.md` | Live Test Eval (real ctx) | testing expressions before saving |
| `vars.md` | `$vars` user variables (storage, action, expression, file ops, UI panel) | using `$name` in conditions / actions |

## Quick-start - creating your first BP

1. Open the Breakpoints window (hotkey Alt+B or via menu).
2. In Breakpoints, right-click "Add Breakpoint Event...".
3. In the Edit BP dialog set:
   - **Type** - pick a type from the dropdown (default `PC_EXEC` =
     classic PC breakpoint). See `types.md` for other types.
   - **Address** (or Port / Event / SP threshold) - depends on the type.
   - **Name** - optional, otherwise generated automatically from the
     address (e.g. `Addr: 0x1234`).
4. Optionally:
   - **Match Mode** - SINGLE (default) / RANGE / MASK. For RANGE fill
     in the second `End Addr` field.
   - **Mode** (radio) - Stop / Log only / Custom DSL.
     - Stop = classic breakpoint, halts the emulator.
     - Log only = just prints to TTY and continues (equivalent to the
       action string `log "BP <name>"`).
     - Custom = opens the action textbox for the script - see
       `action-dsl.md`.
   - **Condition** - optional expression; the BP fires only if it
     returns non-zero - see `expression-syntax.md`.
   - **Hit count** - trigger only on the Nth hit (0 = every hit).
   - **Skip count** - skip the first N hits.
5. Press OK. The BP is active immediately.
6. The BP persists into the `.bpt` file on exit (if `auto_save = 1` in
   the configuration) or manually via Save BP.

## Architecture - high level

### Data model

The central breakpoint structure contains all fields for all 9 types.
Fields not used for a given type are ignored (typically they stay at
default). The main container holds an array of breakpoints and an
array of groups (hierarchy).

### Per-type enforcement

Each BP type has its own enforcement hook in the emu layer:

| Type | Called from |
|------|-------------|
| PC_EXEC | hot loop pre-instruction |
| MEM_R | CPU read in debugger active mode (only data reads - see M1 fetch filtering below) + `[addr]!` deref in an expression; reads by the debugger itself do not fire |
| MEM_W | CPU write in debugger active mode + STMT_POKE; writes by the debugger itself do not fire |
| IORQ_R | CPU port read |
| IORQ_W | CPU port write |
| IRQ | after Z80 INT dispatch (POST-dispatch) |
| IRQ_SIG | PRE-dispatch (edge detection on the INT bus) |
| HW_EVENT | hook sites in emu (vsync, ctc, palette, ...) |
| SP_THRESHOLD | hot loop after SP change |
| GLOBAL | per-instruction (default OFF) |

Hot path optimization: before calling enforce functions the caller
tests a per-type active flag (default false = zero overhead in the
default OFF state). HW_EVENT additionally has a per-event flag.

### MEM_R / MEM_W hook semantics (important)

The MEM_R and MEM_W hooks are called **BEFORE** the actual memory
access (read / write). The condition can read the "old" value via
`[addr]` no-side-effect deref.

This means:

- **The action cannot substitute the value being written.** For MEM_W:
  `value` is input (= what WILL be written), it cannot be overridden
  via a feedback.
- **The original write always happens** after returning from the
  enforce hook, regardless of the action.
- If the action `poke <addr> <value>` writes to the **same address**, it
  is a **separate memory write call** - it runs during the action, then
  control returns and the CPU completes the original write (= it
  effectively overwrites the `poke` value back with the CPU value).
- For real "intercept and modify" semantics a write-back through a
  pointer would be needed; the current breakpoints version does not
  support this.

### MEM_R fires only on data reads (M1 fetch filtering)

CPU read distinguishes 2 access types (reused from the CDL recording
classification):

- **X (eXecute)** - byte belonging to the instruction currently being
  decoded: M1 opcode fetch, M1 prefix opcode (DD/FD/ED/CB) inside the
  same instruction, sequential immediate operand byte.
- **R (data Read)** - any other read (e.g. `LD A,(HL)`, `LD A,(nn)`,
  `POP`, `IN A,(C)` data fetch, ...).

**MEM_R BP fires only for `access_kind == R`.** Without filtering every
instruction would trigger a MEM_R fire for each opcode/operand byte (=
spam, typically 2-4x per instruction), which devalues this BP type for
the "I want to know when the program reads data from X" use case.

The filtering applies **only on the CPU read path**. The `[addr]!`
expression deref has no M1 concept and fires always (= manual reads
should not be filtered); `[addr]` without `!` does not fire.

Note: an instruction fetch from a mid-instruction address (= e.g.
self-modifying code, ROM banking during decoding) is theoretically
still X as long as byte_position has not exceeded the limit and addr
is contiguous. For regular code this is not a problem.

### `poke` self-write recursion (caution)

`poke` in an action handler triggers a memory write, which can re-enter
the enforce layer. If a BP MEM_W on address X has the action `poke X,
Y`:

1. CPU starts the write to X -> hook fires -> action eval
2. Action `poke X, Y` -> memory write -> hook AGAIN
3. Second hook call with `value = Y` -> condition still matches ->
   action AGAIN

The **re-entry guard** uses a global depth counter with a limit of 4
levels. Once exceeded, a warning is printed to stderr and the action
is skipped. The emulator continues without a stack overflow, but the
user is informed that a run-away recursion is happening.

**Mitigation patterns** (= preferred, beyond the guard):

- `poke` to a **different address** (= write side-effect outside of
  the trigger addr): safe, no recursion.
- `disable_self` in the action **BEFORE** `poke X, Y`: after the first
  fire the BP disables itself and the recursive trigger does not fire.
- A condition expression that catches the self-write: e.g.
  `Value != 42` in the condition + the action `poke Address, 42` ->
  after the first poke the value is 42, the condition no longer
  matches, recurse exits.

For MEM_R the analogy is symmetric (= an action `poke` to the address
tracked by MEM_R fires the MEM_W hook, not MEM_R, but beware of MEM_R
recursive reads via a condition with `[addr]`).

### Match modes

For 16-bit fields (PC, MEM addr, IO port) and 8-bit fields (bank ID),
match modes SINGLE / RANGE / MASK are used. Semantics in
`match-modes.md`.

### Condition expression + Action DSL

The shared expression evaluator supports signed 32-bit arithmetic, Z80
CPU registers (main + shadow), flags via the `f` postfix, memory deref
`[addr]` / `{addr:w}`, I/O port read `port[N]`, $vars, 8 built-in
functions. Full grammar in `expression-syntax.md`.

The action mini-DSL has 11 commands: `log`, `continue`, `disable_self`,
`enable <name>`, `disable <name>`, `poke`, `set <reg>`, `mark`,
`$var = expr` (+ compound `+=` `-=` ... `>>=`), `clear_vars`,
`if <expr> then <stmt>`. Detail in `action-dsl.md`.

### HW events vs IRQ vs IRQ_SIG

Three different views on the interrupt subsystem:

- **HW_EVENT** with an event of type `irq:fdc` / `irq:pioz80_a` /
  `irq:ctc2` - an observer at the IRQ signal source, **before** any
  dispatch logic. The trigger condition (rising / falling / level) is
  in `event_trigger`.
- **IRQ_SIG** (pre-dispatch) - a per-source filter on the INT line
  raise edge. Called just **before** the Z80 INT acknowledge. Bitmask
  sources (PIOZ80_A, PIOZ80_B, CTC2, FDC, OTHER) - see `irq-sig.md`.
- **IRQ** (post-dispatch) - after the Z80 INT dispatch completes (=
  jump to the ISR address). Filters on IM mode, IM 0 RST opcode, IM 2
  vector address (`(I << 8) | vec`) and ISR target address -
  see `irq-filter.md`.

### Groups + vars

A hierarchy of groups (parent / child) with cascade enable - if a
parent is disabled, children are not evaluated. Color inheritance.
Persistence in the `.bpt` JSON `groups` section. Detail in `groups.md`.

`$vars` (per-session storage) shared across all BPs. Lifetime =
emulator session, persists into the `.bpt` JSON `vars` section plus a
per-arch standalone `.vars` file (`mz800.vars` / `mz1500.vars` /
`mz700.vars`). Read in an expression as `$name`, written in an action
as `$name op= expr` (= 10 compound operators). Plus a comment field
per-var + `persist_value` flag (= true stores the value, false stores
only name + comment and resets the value to 0 on load - the counter
pattern).

The UI panel `Variables` (= Debugger menu) allows Add / Edit / Delete /
Rename interactively + bulk ops + file ops (Save / Save As / Load From
/ Merge / Clear Values / Clear All). Detail in `vars.md`.

### Persistence

The `.bpt` file is a human-readable JSON (pretty print indent 2). The
path comes from the INI section `[BREAKPOINTS]` `default_file`.
Auto-save / auto-load are optional. Per-key BC fallback on loading of
older files (missing keys = defaults). Schema detail in
`persistence.md`.
