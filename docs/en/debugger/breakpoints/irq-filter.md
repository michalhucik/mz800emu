# Breakpoint - IRQ filter

The `IRQ` breakpoint triggers when the Z80 CPU dispatches an IRQ
(= POST-dispatch). Without sub-filters it fires on **every** dispatch
in the enabled IM modes.

The following filters are available:

- **IM mode discriminator** (per-IM enable/disable)
- **IM 0 RST opcode filter**
- **IM 2 vector address filter** (with Match Mode)
- **IM 2 ISR address filter** (with Match Mode)

For **pre-dispatch** detection of a peripheral signal raise there is a
separate type `IRQ_SIG` - see `irq-sig.md`.

## Background: when the enforce is called

The hook is called **POST-dispatch**, i.e. after the Z80 INT
acknowledge cycle completes. At that moment the following are
available:

- `cpu->im` - the current IM mode (0 / 1 / 2)
- `cpu->i` - the I register (= high byte of vector_table_addr)
- `cpu->int_vector` - the low byte from the peripheral (pushed at
  INTACK); in IM 0 it is the RST opcode (e.g. 0xFF = RST 38h)
- `cpu->pc` - ISR jump target

The pre-dispatch moment has no meaningful context (= only a raised
mask); the post-dispatch moment has vector + ISR. The consistent
semantics "BP fires when IRQ dispatches" is intuitive for debugging
the ISR handler.

## IM mode discriminator

| Field | Meaning |
|-------|---------|
| `im0_enabled` | true = BP fires on IM 0 dispatch |
| `im1_enabled` | true = BP fires on IM 1 dispatch |
| `im2_enabled` | true = BP fires on IM 2 dispatch |

**Default for a new BP**: all 3 enabled (= fire-on-every-IRQ-dispatch).

The UI **requires at least 1** IM mode enabled (= otherwise the BP
would never fire; validation blocks the OK button).

```
IM modes: [x] IM 0   [x] IM 1   [x] IM 2     (?)
```

After turning off an IM checkbox the corresponding sub-section is
hidden.

## IM 0 sub-filter: RST opcode mask

| Field | Meaning |
|-------|---------|
| `im0_rst_mask` | bitmask of 8 RST opcodes (0 = match-all) |

In IM 0 the peripheral pushes a 1-byte opcode onto the bus during
INTACK. Typically this is a `RST n` opcode (= 0xC7 + n*8 for
n = 0..7), but technically it can be any other 1-byte opcode (e.g.
`NOP` or `EI`). The RST mask filters only RST variants; non-RST
opcodes never match.

| Bit | RST | Opcode |
|-----|-----|--------|
| 0   | RST 00 | 0xC7 |
| 1   | RST 08 | 0xCF |
| 2   | RST 10 | 0xD7 |
| 3   | RST 18 | 0xDF |
| 4   | RST 20 | 0xE7 |
| 5   | RST 28 | 0xEF |
| 6   | RST 30 | 0xF7 |
| 7   | RST 38 | 0xFF |

Mask `0` = match-all (= fires on every RST). Mask != 0 = fires only
if the current RST opcode has the matching bit set.

```
IM 0 RST filter (none = match all):
[ ] 0xC7 (RST 00)  [ ] 0xCF (RST 08)  [ ] 0xD7 (RST 10)  [ ] 0xDF (RST 18)
[ ] 0xE7 (RST 20)  [ ] 0xEF (RST 28)  [ ] 0xF7 (RST 30)  [x] 0xFF (RST 38)
```

In the example above the BP fires only on `RST 38h` in IM 0 dispatch
(mask = 0x80).

**MZ-800 use case**: MZ-800 in native mode uses IM 1 (= dispatch to
0x0038), but CP/M or custom firmware may use IM 0 with their own RST
choice. The filter allows triggering only on a specific RST handler.

## IM 1 sub-filter: none

In IM 1 the Z80 always dispatches via `RST 38h` (= jump to 0x0038).
There is no per-vector or per-source sub-filter - if `im1_enabled =
true`, **every** IM 1 dispatch fires the BP (subject to the rest of
the BP rules - condition, hit_count, skip_count).

The UI in this sub-section only shows an info text + tooltip "no
extra filter available".

## IM 2 sub-filter: vector + ISR

### Vector address filter

| Field | Meaning |
|-------|---------|
| `im2_vector_enabled` | true = filter active |
| `im2_vector_addr` | expected value of `(I << 8) \| vec` |
| `im2_vector_match_mode` | SINGLE / RANGE / MASK (UI dropdown) |
| `im2_vector_addr_end` | RANGE upper bound (inclusive) |
| `im2_vector_mask` | MASK bitmask |

The comparison uses the **whole entry address including bit 0**. In
IM 2 the Z80 does not mask bit 0 of the vector (Zilog's documentation
says otherwise, but the processor uses bit 0). The Z80 PIO always
supplies an even vector; an odd entry occurs when nobody supplies the
vector (on the MZ-800 an interrupt from CTC2 or /EXINT) and the
processor reads the last byte left on the data bus. The value 0x40CF
therefore matches only entry 0x40CF, not 0x40CE. To ignore bit 0, use
MASK mode with mask 0xFFFE.

Match modes (per `match-modes.md`):

| Mode | Semantics |
|------|-----------|
| `SINGLE` | exact match `vector_addr == im2_vector_addr` |
| `RANGE`  | `im2_vector_addr <= vector_addr <= im2_vector_addr_end` |
| `MASK`   | `(vector_addr & im2_vector_mask) == (im2_vector_addr & im2_vector_mask)` |

UI layout in the Edit BP dialog (IRQ branch, IM 2 sub-section):

```
IM 2 sub-filters:
[x] Filter by IM2 vector address      (?)
    IM2 Vector:        0x [40CE]
    Vector Match Mode: [SINGLE  v]
```

When the dropdown is switched to RANGE / MASK, a second input (End /
Mask) is shown dynamically:

```
[x] Filter by IM2 vector address      (?)
    IM2 Vector:        0x [FE00]
    Vector Match Mode: [RANGE   v]
    Vector End:        0x [FE3F]      (?)
```

```
[x] Filter by IM2 vector address      (?)
    IM2 Vector:        0x [FE10]
    Vector Match Mode: [MASK    v]
    Vector Mask:       0x [FFE0]      (?)
```

OK button validation: RANGE = End must parse and `End >= Start`.
MASK = Mask must parse (any value including
0xFFFF = identical to SINGLE).

Trigger if (SINGLE):
1. CPU dispatches INT,
2. `cpu->im == 2`,
3. `((cpu->i << 8) | cpu->int_vector) == im2_vector_addr`.

**Use case**: break on a specific interrupt source (= peripheral A vs
B is identified by the vector slot of their INTACK push). RANGE/MASK
for a break on a whole group of peripherals (= e.g. all vectors in
the `0xFE00 - 0xFE3F` slot range).

### ISR address filter

| Field | Meaning |
|-------|---------|
| `im2_isr_enabled` | true = filter active |
| `im2_isr_addr` | expected ISR address = `cpu->pc` after dispatch |
| `im2_isr_match_mode` | SINGLE / RANGE / MASK (UI dropdown) |
| `im2_isr_addr_end` | RANGE upper bound (inclusive) |
| `im2_isr_mask` | MASK bitmask |

Match modes (per `match-modes.md`):

| Mode | Semantics |
|------|-----------|
| `SINGLE` | exact match `isr_addr == im2_isr_addr` |
| `RANGE`  | `im2_isr_addr <= isr_addr <= im2_isr_addr_end` |
| `MASK`   | `(isr_addr & im2_isr_mask) == (im2_isr_addr & im2_isr_mask)` |

The ISR address is the value that the CPU loaded from the IM2 vector
table into PC.

UI layout (IM 2 sub-section, ISR line after the Vector lines):

```
[x] Filter by IM2 ISR address         (?)
    IM2 ISR:        0x [0148]
    ISR Match Mode: [SINGLE  v]
```

```
[x] Filter by IM2 ISR address         (?)
    IM2 ISR:        0x [0100]
    ISR Match Mode: [RANGE   v]
    ISR End:        0x [01FF]         (?)
```

RANGE use case: break when the ISR jumps anywhere within a range of
one ROM page (e.g. user-defined IRQ handlers in `0xC000..0xC1FF`).
MASK use case: break on a specific bit pattern in the ISR address
(= e.g. all ISR handlers in the lower half of 256B slots).

Trigger if (SINGLE):
1. CPU dispatches INT,
2. `cpu->im == 2`,
3. `cpu->pc == im2_isr_addr` (after the INT dispatch jump).

**Use case**: break on entry into a specific ISR handler regardless
of which peripheral raised the IRQ (= a shared handler that routes
through its own mechanisms).

### Combining the IM 2 sub-filters (AND)

If **both** IM 2 sub-filters are enabled, fire requires both to
match:

```
[x] Filter by IM2 vector address
    IM2 Vector:  0x 4060
[x] Filter by IM2 ISR address
    IM2 ISR:  0x 0148
```

Triggers only if the vector slot **and** the ISR target match. Useful
when you want to verify that the vector table has the expected entry
(= sanity check of correct vector wiring).

### IM 2 sub-filter outside IM 2

| IM mode | Sub-filter OFF | Sub-filter ON |
|---------|----------------|----------------|
| IM 0    | fires (if im0_enabled) | the filter explicitly requires IM 2 - sub-filter is a no-op |
| IM 1    | fires (if im1_enabled) | the filter explicitly requires IM 2 - sub-filter is a no-op |
| IM 2    | fires (if im2_enabled) | fires only on match |

Note: the sub-filters `im2_vector_enabled` / `im2_isr_enabled` only
make sense in IM 2 dispatch. If `im0_enabled = true` and
`im2_vector_enabled = true`, the BP fires on IM 0 dispatch (= the
sub-filter is active only in IM 2).

## Persistence

The fields are serialized into the `.bpt` JSON file under the keys:

| Key | Default if missing |
|-----|--------------------|
| `im0_enabled` | `true` (= fire-on-all IMs) |
| `im1_enabled` | `true` |
| `im2_enabled` | `true` |
| `im0_rst_mask` | `0` (= match-all RSTs) |
| `im2_vector_enabled` | `false` |
| `im2_vector_addr` | `0` |
| `im2_vector_match_mode` | `SINGLE` |
| `im2_vector_addr_end` | `0` |
| `im2_vector_mask` | `0xFFFF` |
| `im2_isr_enabled` | `false` |
| `im2_isr_addr` | `0` |
| `im2_isr_match_mode` | `SINGLE` |
| `im2_isr_addr_end` | `0` |
| `im2_isr_mask` | `0xFFFF` |

Example `.bpt` JSON record:

```json
{
  "type": "IRQ",
  "im0_enabled": true,
  "im1_enabled": false,
  "im2_enabled": true,
  "im0_rst_mask": 128,
  "im2_vector_enabled": true,
  "im2_vector_addr": 16590,
  "im2_isr_enabled": false,
  "im2_isr_addr": 0
}
```

## UI

Edit panel for the `IRQ` type:

```
IRQ - CPU acknowledged INT (post-dispatch)
IM modes:  [x] IM 0   [x] IM 1   [x] IM 2     (?)

[If IM 0 enabled:]
IM 0 RST filter (none = match all):
[ ] 0xC7 (RST 00)  [ ] 0xCF (RST 08)  [ ] 0xD7 (RST 10)  [ ] 0xDF (RST 18)
[ ] 0xE7 (RST 20)  [ ] 0xEF (RST 28)  [ ] 0xF7 (RST 30)  [ ] 0xFF (RST 38)

[If IM 1 enabled:]
IM 1 always dispatches RST 38h (no extra filter)

[If IM 2 enabled:]
IM 2 sub-filters:
[ ] Filter by IM2 vector address     (?)
    IM2 Vector:  0x [____]
[ ] Filter by IM2 ISR address        (?)
    IM2 ISR:     0x [____]
```

OK button validation:
- at least 1 IM mode must be enabled
- if IM 2 is enabled + any sub-filter is on, the hex addr must parse

The hex inputs keep their value even in the disabled state (= toggle
off / on does not lose the previously entered vector slot).

## Related documents

- `irq-sig.md` - pre-dispatch IRQ signal source filter
- `match-modes.md` - SINGLE / RANGE / MASK detail
- `types.md` - catalogue of all BP types
