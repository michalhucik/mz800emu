# Breakpoint condition expression language

Used in the `condition` field of `emu_bp_add` / `emu_bp_create_with_init`,
in arguments of Action DSL commands, and in Watch `expr_*` modes.

The evaluator returns a single `int32_t`. **0 = false** (BP does not
fire), **non-zero = true** (BP fires). Empty condition behaves as
"always true".

For the authoritative document with all corner cases see
`docs/en/debugger/breakpoints/expression-syntax.md`.

## Numeric literals

| Form | Example | Meaning |
|------|---------|---------|
| Decimal | `42`, `1000` | base 10 |
| Hex C-style | `0x42`, `0xFF00` | prefix `0x` / `0X` |
| Hex Sharp/IASM | `#42`, `#FF00` | prefix `#` |
| Binary | `%1010`, `%11110000` | prefix `%` |

The `%` operator means modulo when it follows a value / `)` / `]`. No
character (`'A'`) or string literals.

## Identifiers (case-insensitive)

### Z80 registers

| Form | Width |
|------|-------|
| `A`, `B`, `C`, `D`, `E`, `H`, `L`, `F` | 8-bit |
| `AF`, `BC`, `DE`, `HL` | 16-bit pair |
| `IX`, `IY`, `SP`, `PC` | 16-bit |
| `IXH`, `IXL`, `IYH`, `IYL` | 8-bit halves of IX/IY |
| `I`, `R`, `IM` | 8-bit |
| `IFF1`, `IFF2` | 0/1 |
| `AF'`, `BC'`, `DE'`, `HL'` | 16-bit shadow set |

`C` is the C register, **not** carry. Use `Cf` for carry.

### Flag bits (suffix `f`)

| Identifier | Bit in F | Meaning |
|------------|----------|---------|
| `Cf` | 0 | carry |
| `Nf` | 1 | add/subtract |
| `Pf` | 2 | parity / overflow |
| `Hf` | 4 | half-carry |
| `Zf` | 6 | zero |
| `Sf` | 7 | sign |

### Breakpoint hit context

Filled by the BP enforcement layer at hit time. Zero outside a hit
(= ad-hoc Test Eval).

| Identifier | Meaning |
|------------|---------|
| `Address` | Access address (MEM_R / MEM_W / PC_EXEC); port value for IORQ. |
| `Value` | Byte read / written. |
| `IsRead`, `IsWrite`, `IsExec`, `IsPort` | Access type flags (0/1). |
| `BankPC`, `BankAddr` | Banking zone IDs for PC / target. |
| `Reason` | IFF change reason - valid only inside IFF*_CHANGE fire. |

### Global emulator state

| Identifier | Meaning |
|------------|---------|
| `Cycle` | GDG pixel ticks since reset (16x faster than Z80 T-state at 3.5 MHz). |
| `Frame` | Completed frames since reset. |
| `Scanline` | Current GDG raster row (0..frame_height-1). |

### User variables

`$name` - signed 32-bit storage shared across BPs. Read-only in the
expression; writes happen via the Action DSL (`set $name op= expr`).
Non-existent `$name` reads as 0. See `emulator://docs/smart_vars`.

## Memory deref

| Form | Meaning |
|------|---------|
| `[addr]` | Read 8-bit byte from current Z80 memory map. |
| `[addr]!` | Same, with explicit side-effect marker (parsed, currently behaves like `[addr]`). |
| `{addr}` | Read 16-bit little-endian word (two byte reads). |

`addr` is any expression: `[HL]`, `[HL+1]`, `[0x8000 + A]`, `{SP}`.

## I/O port read

| Form | Meaning |
|------|---------|
| `port[N]` | Side-effect-free probe (mirror cache / last-write / bus latch fallback). **Recommended for conditions.** |
| `port[N]!` | Real CPU `IN`-equivalent read (= has side effects, fires on every eval). |

Probe semantics per chip (default `port[N]`):

| Range | Chip | Probe returns |
|-------|------|---------------|
| `0xF0`-`0xF1` | Joystick (MZ-800 only) | real read (pure function) |
| `0xE0`-`0xE6` | Memory banking | mirror cache |
| `0xCE` | GDG DMD status | mirror cache |
| `0xD0`-`0xD3` | PPI 8255 | PortB keyboard row + control cache |
| `0xD4`-`0xD6` | CTC 8253 | counter decode + control cache |
| `0xD8`-`0xDB` | FDC WD279x | status / track / sector mirror |
| `0xFC`-`0xFF` | Z80 PIO | control word cache |
| other | unconnected | bus latch / `0xFF` |

## Operators (highest to lowest precedence)

| Group | Operators |
|-------|-----------|
| Parentheses | `( )` |
| Unary | `+x`, `-x`, `!x`, `~x` |
| Multiplicative | `*`, `/`, `%` |
| Additive | `+`, `-` |
| Bit shift | `<<`, `>>` |
| Relational | `<`, `<=`, `>`, `>=` |
| Equality | `==`, `!=` |
| Bit AND | `&` |
| Bit XOR | `^` |
| Bit OR | `\|` |
| Logical AND | `&&` (short-circuit) |
| Logical OR | `\|\|` (short-circuit) |

Notes:
- Division / modulo by zero -> 0 (tolerant, no exception).
- Shift count masked to 5 bits.
- Single `=` rejected with parser error.
- Short-circuit is useful to guard memory reads: `HL >= 0x8000 && [HL] == 0xFF`.

## Built-in functions

| Call | Meaning |
|------|---------|
| `min(a, b)` | smaller of two |
| `max(a, b)` | larger of two |
| `abs(x)` | absolute value |
| `if(cond, a, b)` | `cond ? a : b` (only the selected branch evaluated) |
| `bit(value, n)` | n-th bit of value (0/1); n masked to 0..31 |
| `s8(x)`, `s16(x)`, `s32(x)` | sign-extend low byte / low word / no-op |

## Examples

```text
A == 0x42                                ; A holds 0x42
PC >= 0x1000 && PC < 0x2000              ; PC inside range
HL && [HL] == 0xFF                       ; non-null HL points to 0xFF
{SP} == 0x1234                           ; top of stack equals 0x1234
Cf && IsWrite                            ; carry set and write access
$hits >= 5                               ; user counter reached 5
bit(port[0xCE], 7) == 1                  ; bit 7 of GDG status probe
if(HL >= 0x8000 && HL < 0xC000, [HL], 0) == 0xFF   ; safe read with range
Reason == nmi_ack                        ; only NMI ack in IFF1_CHANGE
```

## Common pitfalls

- `C` is register C; use `Cf` for carry flag.
- Use `port[N]` (probe) by default; `port[N]!` triggers real HW side
  effects on **every** condition eval.
- Sign vs unsigned: comparisons are signed `int32_t`. For unsigned
  semantics either mask (`a & 0xFFFF`) or use `s8` / `s16`.
- `$name` is read-only in conditions. Write via Action DSL.
- `Cycle` is GDG pixel ticks, not a pure Z80 T-state counter.
- Address ranges are not part of the condition: a range breakpoint
  (`PC_EXEC` / `MEM_R` / `MEM_W`) needs both `addr_end` and
  `addr_match_mode: "RANGE"` in `fields` of `emu_bp_create_with_init` /
  `emu_bp_update`. With `addr_end` alone the mode stays `SINGLE` and the
  breakpoint matches only `addr`; create returns a `warning` in that case.
  `emu_bp_list` shows `addr_end`, `addr_match_mode` and `addr_mask`
  (and, for the `MMEXT_BANK` zone, `bank_id_end`, `bank_match_mode`,
  `bank_id_mask` - the bank range works the same way).

## Related

- `emulator://docs/smart_vars` - `$name` variables, persistence, action DSL.
- `emulator://docs/watch_dsl` - same language inside Watch `expr_*` modes.
- `docs/en/debugger/breakpoints/action-dsl.md` - action DSL writing `$name`.
- `docs/en/debugger/breakpoints/hw-events.md` - per-BP-type `Address` / `Value` semantics.
