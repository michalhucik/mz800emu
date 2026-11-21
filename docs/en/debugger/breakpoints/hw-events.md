# Breakpoint - HW events

A breakpoint of type `HW_EVENT` reacts to a named hardware event in
the emulator (vsync, raster, peripheral INT line, write to the GDG
palette, change of the CPU memory mode, etc.). Unlike MEM/IORQ
breakpoints, which monitor a specific address, an HW_EVENT BP triggers
on a semantic event - regardless of which ROM/program path caused it.

31 events are available in four categories. For signal-type events a
**trigger condition** (low/high/rising/falling/changed) is available.

## Vocabulary - 31 events in 4 categories

### Signal events (17) - with trigger condition

Signal events represent a digital signal at level 0/1 (syncs,
blanking, INT lines, CMT). The UI offers a **trigger condition**
selector that determines when the BP fires:

| Trigger | Semantics | Symbol in UI |
|---------|-----------|--------------|
| `Low`     | fires while the signal is 0 | "0" |
| `High`    | fires while the signal is 1 | "1" |
| `Rising`  | fires on the 0 -> 1 transition | triangle up |
| `Falling` | fires on the 1 -> 0 transition | triangle down |
| `Changed` | fires on any transition | double triangle |

The default is `Rising` (= "fire-on-event").

| Persistence | UI label | Description |
|-------------|----------|-------------|
| `vsync`        | vsync       | GDG vsync signal |
| `hsync`        | hsync       | GDG hsync signal |
| `vbln`         | vbln        | GDG vertical blanking |
| `hbln`         | hbln        | GDG horizontal blanking |
| `ctc:zc0`      | ctc0        | CTC 8253 channel 0 OUT |
| `ctc:zc1`      | ctc1        | CTC 8253 channel 1 OUT |
| `ctc:zc2`      | ctc2        | CTC 8253 channel 2 OUT |
| `irq:ctc2`     | IRQ ctc2    | CTC2 INT line |
| `irq:pioz80_a` | IRQ pio-z80 PA | PIO Z80 port A INT |
| `irq:pioz80_b` | IRQ pio-z80 PB | PIO Z80 port B INT |
| `irq:fdc`      | IRQ fdc     | FDC INT line |
| `tempo`        | TEMPO       | TEMPO signal (~32 Hz) |
| `cursor`       | CURSOR      | CURSOR blink signal (edge-based per frame) |
| `cmt:in`       | CMT_IN      | CMT tape input edge |
| `cmt:out`      | CMT_OUT     | CMT tape output (PC1 write) |
| `cmt:mstate`   | CMT_MSTATE  | CMT motion state (PC04 toggle / derived from STOP/PAUSED) |
| `cmt:motor`    | CMT_MOTOR   | CMT motor enable (PC3 bit) |

**Note:** signal polarity - the value `1` in the BP enforce layer
means "the signal is logically active" (= sync in progress / blanking
active / INT raised / motor on / signal high). The polarity of HW
signals (= MZ has some signals inverted, e.g. VSYN_ACTIVE = 0) is
converted to a common convention.

### Change events (5) - implicit "happened"

Change events represent a discrete change of a register value. No
trigger selector - they always fire on change. The new memory value
is available in the condition expression as `Value`.

| Persistence       | UI label  | Description |
|-------------------|-----------|-------------|
| `mode_change`     | mode      | GDG DMD register (port 0xCE) |
| `palette_change`  | palette   | GDG palette index (port 0xF0, bit 6 = 0) |
| `palgrp_change`   | palgrp    | GDG palette group (port 0xF0, bit 6 = 1) |
| `border_change`   | border    | GDG border color (port 0xCF, msb = 6) |
| `cksw_change`     | cksw      | GDG CKSW bit (port 0xCF, msb = 7); `Value` = 0 or 1, fires on every write |
| `cmt:state_change`| cmt state | CMT lifecycle (play / stop / pause / eject) |

Note: `cksw_change` exists on the MZ-800 only. MZ-1500 has neither `palgrp_change` nor `border_change` (= a
simpler palette model, fixed border). BPs with these events on
MZ-1500 never fire.

**`cmt:state_change`:**

CMT lifecycle event. `Value` in the condition expression = composite
enum:

| Value | Meaning |
|-------|---------|
| 0 | tape stopped (= ejected or stopped) |
| 1 | tape playing (paused == 0) |
| 2 | tape recording (paused == 0) |
| 3 | tape paused (overrides any underlying state) |

Example condition: `Value == 1` = stop only on transition to the PLAY
state. PAUSED is an override - if the user pauses recording, value =
PAUSED, not RECORD. On unpause a new fire arrives with value =
PLAY/RECORD.

### Point event with a parameter (1)

| Persistence    | UI label | Param |
|----------------|----------|-------|
| `raster:N`     | raster:N | scanline number (N = 0..311) |

Fires only for the specific scanline N. For a raster over the whole
frame (= every row), use the `hsync` event with trigger=Rising
instead (= 311 fires/frame).

### CPU events (8) - point/state without a trigger condition

| Persistence       | UI label   | Semantics | Value in ctx |
|-------------------|------------|-----------|--------------|
| `cpu:nmi`         | NMI        | NMI assertion | 0 |
| `cpu:di`          | DI         | DI instruction executed | 0 |
| `cpu:im_change`   | IM change  | IM 0/1/2 change | new IM |
| `cpu:iff_change`  | IFF change | Aggregated event for both IFF1 and IFF2; prefer the specific `cpu:iff1_change` / `cpu:iff2_change` | new IFF |
| `cpu:iff1_change` | IFF1 change | IFF1 change | new IFF1 (0/1) |
| `cpu:iff2_change` | IFF2 change | IFF2 change | new IFF2 (0/1) |
| `cpu:halt`        | HALT       | HALT instruction | 0 |
| `cpu:reset`       | RESET      | CPU reset | 0 |

For IM_CHANGE and IFF_CHANGE you can filter in the condition
expression on a specific value (= `Value == 2` for an IM 2 fire).

**IFF1 / IFF2 fire decision matrix:**

Changes of IFF1/IFF2 have a reason indicating what triggered the
change (RESET, EI, DI, INT_ACK, NMI_ACK, RETI, RETN). Fire matrix:

| Reason  | `iff_change` | `iff1_change` | `iff2_change` |
|---------|--------------|---------------|---------------|
| RESET   | yes          | yes           | yes           |
| EI      | yes          | yes           | yes           |
| DI      | yes          | yes           | yes           |
| INT_ACK | yes          | yes           | yes           |
| NMI_ACK | yes          | yes           | yes           |
| RETI    | -            | -             | -             |
| RETN    | yes          | yes           | -             |

- **RESET** fires everything (CPU reset clears both IFFs).
- **EI / DI**: both IFF1+IFF2 -> 1 / 0; all three fire in parallel.
- **INT_ACK** (= IM 0/1/2 dispatch): IFF1 -> 0, IFF2 preserved; fires
  IFF1_CHANGE + IFF_CHANGE + IFF2_CHANGE (on the original Z80 it is
  preserved, but the event is emitted for UI symmetry).
- **NMI_ACK**: IFF1 cleared, IFF2 preserved; IFF2_CHANGE fires
  forward-compat for alternative Z80 architectures.
- **RETI**: a signal for the Z80 PIO, IFFs unchanged; emits no event.
- **RETN**: IFF1 restored from IFF2; fires IFF_CHANGE + IFF1_CHANGE
  (always, regardless of whether IFF1==IFF2 before).

A condition expression in the BP DSL can filter on reason via
`Reason == <symbol>` (= `reset` / `ei` / `di` / `int_ack` / `nmi_ack`
/ `reti` / `retn` / `none`). See
[expression-syntax.md Reason vocabulary](expression-syntax.md#reason-vocabulary).

**Example:** a BP on `cpu:iff_change` with the condition `Reason ==
nmi_ack` catches only the NMI ack moment. Nested-ISR detection via
`Reason == int_ack && SP < 0xFFE0`.

Filtering IFF1 vs IFF2 can be done by selecting the right event when
registering the BP, not in the condition (= EI/DI fire both at once).

## Trigger condition - semantics

For signal events the enforce layer holds a **per-event prev signal
level**. On every event fire it compares the current signal value
against the previous one and, based on the BP trigger condition,
decides fire/skip:

```
LOW:     fire = (curr == 0)
HIGH:    fire = (curr == 1)
RISING:  fire = (prev == 0 && curr == 1)
FALLING: fire = (prev == 1 && curr == 0)
CHANGED: fire = (prev != curr)
```

The state cache is updated AFTER iterating over all BPs - the next
fire sees curr as prev.

## Condition expression - per-kind ctx

Condition expression eval (= BP `Cond:` field) has per-kind semantics
for the `Address` and `Value` fields:

| Kind          | `Address`    | `Value`                                |
|---------------|--------------|----------------------------------------|
| SIGNAL        | 0            | curr_state (0 or 1)                    |
| CHANGE        | 0            | new value (mode/palette/palgrp/border value) |
| POINT_PARAM   | row (raster) | 0                                      |
| POINT_NOPARAM | 0            | info (IM/IFF new value, otherwise 0)   |

Example conditions for change events:
```
# Mode_change BP, fires only on switch to MZ-700 mode (DMD=0x08):
Value == 0x08

# Palette_change, fires only for index 5 (= bits 4..7 of the PALGRP register):
(Value >> 4) & 0x07 == 5
```

## Use case examples

### Stop at the start of a frame for DMA-style analysis

```
Type:    HW_EVENT
Event:   vsync
Trigger: Rising
Action:  Stop
```

Fires once per frame (50 Hz) at the start of the vsync pulse.
Classical debug approach for per-frame state analysis.

### Detecting a mid-frame raster split

```
Type:    HW_EVENT
Event:   raster:192
Action:  Stop
```

Fires at the start of scanline 192 (= classic position for border
raster effects). Useful for analyzing a mid-frame palette switch.

### Watchdog on writes to the palette

```
Type:    HW_EVENT
Event:   palette_change
Cond:    Value & 0x40 == 0
Action:  Log "PAL: $", Value, " at $", PC
```

Fires on every write to the palette (not palgrp), logs the value +
PC context without stopping. Allows tracking ROM routines that switch
colors.

### Detecting CMT motor on/off for tape loaders

```
Type:    HW_EVENT
Event:   cmt:motor
Trigger: Rising
Action:  Log "CMT motor ON at $", PC
```

Fires only on the 0->1 transition of the PC3 bit (= the moment when
the program turned on the player). Useful for analyzing tape loader
sequences.

### Tracking CTC2 INT line activation

```
Type:    HW_EVENT
Event:   irq:ctc2
Trigger: Rising
Action:  Stop
```

Fires only on the raise edge of the CTC2 INT line. Trigger=Changed =
fires also on deassert (= debug toggle frequency in the PSG audio
engine).

## Persistence - backward compatibility

Existing `.bpt` files without the `event_trigger` key are loaded with
the default `rising` trigger (= preserves the legacy fire-on-event
behavior).

Legacy event names in the BC alias table (= older `.bpt` files load):

| Legacy string                 | Current persistence         |
|-------------------------------|------------------------------|
| `pio:porta_int`               | `irq:pioz80_a`               |
| `pio:portb_int`               | `irq:pioz80_b`               |
| `fdc:irq`                     | `irq:fdc`                    |
| `tape:edge`                   | `cmt:in`                     |
| `nmi`, `di`, `im_change`, ... | `cpu:*` (with prefix)        |

### Retired events

These events were either unfinished or are more expressive via an
IORQ_W BP:

- `psg:int_pa5` - the PSG INT is visible through `irq:pioz80_b` or an IRQ_SIG BP
- `psg:reg_write` - use IORQ_W on the PSG ports (0xF0/0xF2)
- `ppi:pa_write`, `ppi:pc_change` - use IORQ_W on the 8255 ports (0xD0-0xD3)
- `fdc:command`, `fdc:drq` - use IORQ_W on the FDC ports (0xD8-0xDB)
- `ctc:gate0_edge` - gate is an input signal, not output (= outside the HW listing)
- `tape:read_byte`, `tape:write_byte` - byte-level distinction does not exist at the HW level
- `mmio:bank_switch`, `mmio:mode_change` - use IORQ_W on the E0-E6 banking ports

BPs with these events in an older `.bpt` file load with an unknown
event_name - a stderr warning, the BP remains visible in the UI with
the original string but never fires.

### Recommended migration

1. Delete BPs with retired events and replace them with IORQ_W BPs
   on the corresponding port and an optional condition expression
   filtering on the value.
2. For existing vsync/hsync/raster BPs check the trigger condition -
   the default `Rising` preserves the legacy behavior. If you want to
   monitor a sustained level (not just an edge), set `High` or `Low`.
