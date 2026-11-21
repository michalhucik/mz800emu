# Breakpoint - expression syntax (Condition expression)

The expression is the language used in the breakpoint `condition`
field, in arguments of most action mini-DSL commands (see
`action-dsl.md`) and in the Live Test Eval panel. The breakpoint
condition is evaluated just before the action runs - if the expression
returns 0, the BP does not fire; a non-zero value means "trigger".

This document describes the grammar, lexical rules and semantics of
the expression for the breakpoint system.

## Data type

The evaluator works with a single type - signed 32-bit integer
(`int32_t`). Boolean logic is expressed as "0 = false, non-zero = true"
(C-style). No floating point, no strings, no aggregate types.

## Literals

Literal conventions match the notation of IASM (the emulator's
internal assembler).

| Form | Example | Meaning |
|------|---------|---------|
| Decimal | `42`, `1000` | decimal number |
| Hexadecimal (C) | `0x42`, `0xFF00` | hex with prefix `0x` / `0X` |
| Hexadecimal (Sharp) | `#42`, `#FF00` | hex with prefix `#` |
| Binary | `%1010`, `%11110000` | binary with prefix `%` |

Note: `%` is ambivalent in the lexer - at the start of an expression
or after an operator / `(` / `[` it means a binary literal; after a
value / `)` / `]` it means modulo. The lexer disambiguates by position,
which is typically invisible to the user. Example: `%101 + a%2` is
`5 + (a mod 2)`.

There are no character (`'A'`) or string literals - the parser rejects
them.

## Identifiers

Identifiers are case-insensitive (= `A`, `a`, `pc`, `PC`, `Cf`, `CF`
are all the same). The maximum identifier length is 31 characters;
anything beyond is silently truncated.

### CPU registers (Z80)

| Identifier | Meaning |
|------------|---------|
| `A`, `B`, `C`, `D`, `E`, `H`, `L`, `F` | 8-bit main registers |
| `BC`, `DE`, `HL`, `AF` | 16-bit pairs |
| `IX`, `IY`, `SP`, `PC` | 16-bit index / stack / PC |
| `IXH`, `IXL`, `IYH`, `IYL` | 8-bit halves of the index registers (high/low byte of IX/IY) |
| `I`, `R` | interrupt vector / refresh registers (8-bit) |
| `IM` | current interrupt mode (0/1/2) |
| `IFF1`, `IFF2` | interrupt flip-flops (0/1) |

`PC` is the live register. In a PC_EXEC breakpoint it equals the address
of the instruction about to execute. In MEM_R, MEM_W and IORQ breakpoints
the instruction that makes the access is already being executed, so `PC`
points behind it: `LD (4000h),A` at 3003h gives `PC = 3006h`. Use
`InsnPC` (see [Breakpoint context](#breakpoint-context)) for the address
of the instruction itself.

### Shadow registers

| Identifier | Meaning |
|------------|---------|
| `AF'`, `BC'`, `DE'`, `HL'` | 16-bit shadow pairs (after `EXX` / `EX AF,AF'`) |

The apostrophe is part of the identifier. The lexer allows it as a
continuation character, so `bc'` parses as a single token.

### Z80 flags

| Identifier | Bit in `F` | Meaning |
|------------|------------|---------|
| `Cf` | 0 | carry |
| `Nf` | 1 | add/subtract |
| `Pf` | 2 | parity / overflow (P/V) |
| `Hf` | 4 | half-carry |
| `Zf` | 6 | zero |
| `Sf` | 7 | sign |

The `f` postfix prevents a collision with the register `C` (= register
C, not carry) and `F` (= the flag register as a whole). The value is
0 or 1.

### Breakpoint context

These fields are filled by the breakpoint enforcement layer at the
moment of the trigger, according to the BP type. If the expression is
evaluated outside enforcement (= Test Eval, ad-hoc), they remain zero.

| Identifier | Meaning |
|------------|---------|
| `Address` | Memory access address (MEM_R/MEM_W/PC_EXEC); for IORQ the port value |
| `Value` | Byte read / written in the given access |
| `IsRead`, `IsWrite`, `IsExec`, `IsPort` | Access type (0/1) |
| `InsnPC` | Address of the instruction being executed (its first opcode byte). Same as `PC` in PC_EXEC; in MEM_R / MEM_W / IORQ the instruction that makes the access (`PC` is already behind it). Valid also outside enforcement (Test Eval). |
| `BankPC` | Active banking zone for the live `PC` |
| `BankAddr` | Active banking zone for `Address` (typically the same as `BankPC`) |
| `Reason` | IFF change reason - valid only within an active IFF*_CHANGE fire; outside an active fire returns `none` (0xFF). See [Reason vocabulary](#reason-vocabulary). |

The semantics of `Address` / `Value` differ by BP type - see
`hw-events.md` and `match-modes.md`. For IRQ / IRQ_SIG / HW_EVENT
types some fields may be irrelevant (= they remain 0).

### Reason vocabulary

`Reason` is an ambient value valid at the moment of an IFF*_CHANGE
event fire (typically when an IFF flip-flop changes). Outside an
active fire it returns `none` (0xFF).

| Identifier | Value | Meaning |
|------------|-------|---------|
| `reason` | the current reason | reads the ambient value; returns `none` (0xFF) outside an active fire |
| `reset` | 0 | IFF cleared due to CPU reset |
| `ei` | 1 | IFF set due to the `EI` instruction |
| `di` | 2 | IFF cleared due to the `DI` instruction |
| `int_ack` | 3 | IFF cleared due to INT acknowledgement (IM 0/1/2) |
| `nmi_ack` | 4 | IFF1 cleared due to NMI acknowledgement (IFF2 preserved) |
| `reti` | 5 | RETI - signal for Z80 PIO, IFFs unchanged |
| `retn` | 6 | IFF1 restored from IFF2 (RETN) |
| `none` | 0xFF | sentinel: outside an active IFF fire |

Identifiers are case-insensitive, so `Reason`, `REASON`, `int_ack`,
`INT_ACK` are all equivalent.

**Usage examples:**

```text
Reason == nmi_ack                    ; catches only the NMI ack moment
Reason == reti                       ; PIO IRQ acknowledge signal
(Reason == int_ack) && (SP < 0xFFE0) ; nested-ISR detection
Reason != none                       ; whether an IFF fire is active
```

For the decision matrix (= which reasons fire which IFF*_CHANGE events)
see [hw-events.md](hw-events.md#cpu-events).

### Global emu state

| Identifier | Meaning |
|------------|---------|
| `Cycle` | GDG pixel ticks since the emulator started, at the start of the current instruction (a machine reset does not restart the counter, loading a snapshot sets the saved value). **Note:** not a Z80 T-state counter: one T-state is 5 GDG ticks on PAL machines (MZ-800, MZ-700 PAL) and 4 on NTSC (MZ-700 NTSC, MZ-1500). Like every value it is truncated to 32 bits and wraps to negative numbers after 2^31 ticks, about 121 s of emulated time on PAL (17 721 600 ticks per second) and about 150 s on NTSC. |
| `FrameCycle` | GDG pixel ticks since the start of the current frame, at the start of the current instruction: 0 .. ticks per frame - 1 (354 432 per frame on PAL, 238 944 on NTSC). Does not wrap; use it for timing within a frame. `Cycle = Frame * ticks per frame + FrameCycle` while `Cycle` does not wrap. On the MZ-800 a frame is shorter while the CKSW bit (port 0x07CF, MZ notation 0xCF07, bit 7) is set: every line takes 1120 ticks instead of 1136, a whole frame 349 440; after such a line the relation above no longer holds and `FrameCycle` of a frame ends earlier. |
| `Frame` | Number of complete frames since the emulator started (a machine reset does not restart it). Incremented at the end of a frame. |
| `Scanline` | Current GDG raster row (0..frame_height-1). |

Per-arch source: mz800 and mz1500 share the GDG structure; mz700
inherits the same.

### User variables `$name`

`$name` in a condition refers to the value of a user variable. Value:

- existing var -> its `int32_t` value
- non-existent var -> 0 (the eval never throws an error)

`$name` names are validated by the regex `^[a-zA-Z_][a-zA-Z0-9_]*$` (=
first character alpha or `_`, continuation alpha / digits / `_`). Max
31 characters. No reserved keywords (= the `$` prefix is itself
distinctive, so `$if`, `$set`, `$log` are all valid user names).

Variable lifetime is per-emulator-session; persistence in the `.bpt`
JSON `"vars"` section plus a per-arch standalone `.vars` file
(= `mz800.vars` / `mz1500.vars`). Storage is shared across BPs - one
BP can read a variable set by another. Detail in `vars.md`.

Writes to variables happen exclusively from the action DSL (`$name op=
expr` - see `action-dsl.md`); in the expression itself `$name` is
read-only.

From the UI panel `Variables` you can Add / Edit / Delete / Rename
variables interactively, see `vars.md`.

## Operators

Precedence top to bottom (higher = stronger), associativity standard
C-like.

| Group | Operators | Associativity |
|-------|-----------|---------------|
| Parentheses | `( )` | - |
| Unary | `+`, `-`, `!`, `~` | right |
| Multiplicative | `*`, `/`, `%` | left |
| Additive | `+`, `-` | left |
| Bit shift | `<<`, `>>` | left |
| Relational | `<`, `<=`, `>`, `>=` | left |
| Equality | `==`, `!=` | left |
| Bit AND | `&` | left |
| Bit XOR | `^` | left |
| Bit OR | `\|` | left |
| Logical AND | `&&` | left, short-circuit |
| Logical OR | `\|\|` | left, short-circuit |

Semantics:

- Division / modulo by zero -> 0 (the evaluator is tolerant, no
  exception).
- The shift count is masked to 5 bits (`r & 31`).
- Logical `&&` / `||` short-circuit (= the right operand is not
  evaluated if the left side determines the result). Useful for
  guarding memory derefs.
- `!=` and `==` return 0/1, not a C-style boolean (= safe in
  arithmetic).
- A single `=` is not assignment and the parser rejects it with the
  error "expected '==' (single '=' is not assignment)".

## Memory dereference

| Form | Meaning |
|------|---------|
| `[addr]` | Read 8-bit from the current CPU memory map (RAM, ROM, VRAM) without triggering MEM_R breakpoints |
| `[addr]!` | Same read, but MEM_R breakpoints on `addr` are evaluated (they can fire and stop the emulation) |
| `{addr}` | Read 16-bit (little-endian) - two 8-bit reads, like `[addr]` |

Both forms respect the current banking. In MZ-700 mode the chips mapped
at 0E000h-0E008h (8255, 8253, GDG status) are read without side effects
in both forms: the autotype does not advance, the host keyboard and
joystick are not polled and the 8253 LSB/MSB flip-flop does not toggle.
For a read with the real chip side effects use `port[N]!`.

`{addr}` (16-bit word) has no variant with a side-effect flag.

The address may be any expression: `[HL]`, `[HL+1]`, `[0x8000 + A]`,
`{SP}` (= top of the stack as a word).

## I/O port read

| Form | Meaning |
|------|---------|
| `port[N]` | Side-effect-free probe of IORQ port N |
| `port[N]!` | Real read = same as the CPU `IN` instruction (= has side effects) |

Probe semantics **per chip**:

| Port range | Chip | `port[N]` (probe) | `port[N]!` (real) |
|------------|------|-------------------|--------------------|
| 0xF0 / 0xF1 (MZ-800 only) | Joystick | real (pure function without side effects) | same as probe |
| 0xE0-0xE6 | Memory mapper / banking | mirror cache (last-write) | bus latch (the chip is write-only) |
| 0xCE | GDG DMD status | mirror cache / fallback | real read (has strobe side-effect) |
| 0xD0-D3 | PIO 8255 (PPI) | Port B keyboard row + Control Word cache | real read (PortC tape state side-effect) |
| 0xD4-D6 | CTC 8253 | counter decode + Control Word cache | real read (counter latch side-effect) |
| 0xD8-DB | FDC WDC | status / track / sector mirror | real read (status latch side-effect) |
| 0xFC-FF | PIO Z80 | Control Word last-write cache | real read (IRQ ack side-effect) |
| Others (unconnected) | - | bus latch / 0xFF | bus latch |

**Usage:**

- `port[N]` (default, **recommended**): a probe for condition eval -
  safer, does not trigger HW side-effects on every call. For ports
  that have a mirror cache, returns a snapshot of the chip state. For
  side-effect-heavy or non-mirror ports, returns the bus latch as a
  fallback.
- `port[N]!`: use only when you explicitly want the side-effect (=
  e.g. read the current GDG status, PIO 8255 PortC live state). In
  every condition evaluation it behaves like the Z80 `IN` instruction.

`port` is a reserved keyword (case-insensitive) and the parser
recognizes it before `[` as the I/O variant - the distinction from
the memory deref `[addr]` works thanks to the `port` prefix.

## Built-in functions

Function identifiers are case-insensitive. Arguments are expressions.

| Call | Arity | Semantics |
|------|-------|-----------|
| `min(a, b)` | 2 | the smaller of two |
| `max(a, b)` | 2 | the larger of two |
| `abs(x)` | 1 | absolute value |
| `if(cond, a, b)` | 3 | `cond ? a : b` (only the selected branch evaluated) |
| `bit(value, n)` | 2 | the value of bit n (0/1); n is masked to 0..31 |
| `s8(x)` | 1 | sign-extend low byte (= `(int8_t)(x & 0xFF)`) |
| `s16(x)` | 1 | sign-extend low word (= `(int16_t)(x & 0xFFFF)`) |
| `s32(x)` | 1 | identity for 32-bit (= `x`) |
| `lastwriter(addr)` | 1 | address of the instruction that last wrote the memory cell at CPU address `addr` (current mapping; MZ-800 graphic VRAM = the newest of its planes) according to the [RAM write log](../ram-write-log.md); `-1` = not written since the log was started or emptied, no log was started, or a memory-mapped port (the value stays readable after the log is stopped) |

Wrong arity (= a different number of arguments) is a parser-time
error. A function not on the list yields the parse error "unknown
function 'name'".

`if(cond, a, b)` is the only function with lazy evaluation - cond is
always evaluated, then only one of the branches `a` / `b`. Useful for
guarding memory derefs: `if(HL >= 0x8000, [HL], 0)`.

## Examples

```
A == 0x42
```
Triggers only when register A contains 0x42.

```
PC >= 0x1000 && PC < 0x2000
```
Range check for PC.

```
HL && [HL] == 'X'
```
Triggers only if HL is non-zero (= guard) and the byte at address HL
is `'X'` (= 0x58). `'X'` does not exist as a literal, so this would
fail - correctly written as `HL && [HL] == 0x58`.

```
{SP} == 0x1234
```
Top of the stack (16-bit LE) is 0x1234.

```
Cf && IsWrite
```
Carry flag set and the access is a memory write.

```
$hits >= 5
```
The user variable `$hits` has reached 5 (the variable is typically
incremented by the action DSL of the same or another BP).

```
bit(port[0xCE], 7) == 1
```
Bit 7 of the value read from port 0xCE. For side-effect-heavy chips
(GDG DMD is in this category) the probe returns the mirror cache or
the bus latch - for the real strobe-driven status use `port[0xCE]!`.

```
if(HL >= 0x8000 && HL < 0xC000, [HL], 0) == 0xFF
```
Safe memory read with a range (= returns 0 outside the VRAM area).

```
AF' == 0
```
Shadow AF is zero (= `EX AF,AF'` has not happened yet).

## Expression return value

An expression returns a single `int32_t` value. For the BP condition
field:

- 0 = false -> the BP does not fire, hit counter does not increment
- non-zero = true -> the BP fires, the action runs

If the condition is empty, the breakpoint behaves as "always true"
(the BP fires on every hit of the dispatch hook).

## Behavior on errors

### Parse-time

Lexer / parser errors (bad literal, unclosed parenthesis, unknown
function, wrong arity, single `=`, ...) show an inline error below
the textbox in the Edit BP dialog. A BP with an unparsable condition
behaves as "always true fallback" (= fires on every hit) until you
fix it.

### Runtime

The evaluator is tolerant:

- Unknown identifier -> 0
- Division / modulo by zero -> 0
- NULL CPU context -> registers resolve to 0
- `$name` non-existent -> 0

None of these situations crash the evaluator or the BP enforcement.

## Expression language limits

- No loops / iteration in the expression.
- No user-defined functions (= only built-ins).
- No strings or arrays (only scalar `int32_t`).
- `Cycle` returns GDG pixel ticks, not a pure Z80 T-state counter, and
  wraps after about 121 s of emulated time (PAL); within a frame use
  `FrameCycle`.
- No explicit `unsigned` type - shifts and comparisons operate signed;
  for unsigned compare either mask (`a & 0xFFFF`) or use `s8` / `s16`
  for explicit sign-extension.

## Reference

- Action DSL (writes into `$name`, using an expression as an argument):
  `action-dsl.md`
- Semantics of `Address` / `Value` per BP type: `hw-events.md`,
  `match-modes.md`
- IRQ-specific context fields: `irq-filter.md`, `irq-sig.md`
- User variables: `vars.md`
- Global symbols for `%s` in the action DSL: `../symbols.md`
