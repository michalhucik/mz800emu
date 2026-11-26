# Breakpoint types

The debugger supports 9 breakpoint types. This document is their
catalogue with semantics, used fields and typical use cases.

## Overview

| String ID | Description |
|-----------|-------------|
| `PC_EXEC` | execution at a CPU address |
| `MEM_R` | memory read |
| `MEM_W` | memory write |
| `IORQ_R` | I/O port read |
| `IORQ_W` | I/O port write |
| `IRQ` | post-dispatch IRQ |
| `HW_EVENT` | named HW event |
| `SP_THRESHOLD` | stack overflow / window |
| `GLOBAL` | without address, condition only |
| `IRQ_SIG` | pre-dispatch peripheral IRQ signal |

## PC_EXEC

**Semantics:** classic breakpoint - fires before executing the
instruction at the given address (= before fetch and decode).

**Relevant fields:**

- Address - primary address (also serves as the lower bound for RANGE)
- End Addr - upper bound for RANGE mode
- Match Mode - SINGLE / RANGE / MASK
- Mask - AND mask for MASK mode
- Zone and Bank ID (with its own Match Mode for bank) - banking-aware
  filter (see `match-modes.md`)
- common fields: enabled, name, condition expression, action, hit
  count, skip count, edge-triggered, colors

**UI fields:** Type dropdown, Address textbox (hex), End Addr (RANGE),
Mask (MASK), Zone dropdown, Bank ID (MMEXT_BANK).

**Match Mode support:** SINGLE / RANGE / MASK for address, SINGLE /
RANGE / MASK for bank ID (only in zone MMEXT_BANK).

**Use case:**

- Stop on reaching the entry point of a routine: `Type: PC_EXEC,
  Address: 0x1200`.
- Trace a whole code segment (RANGE): `Address: 0x1000, End Addr:
  0x10FF, Match Mode: RANGE, Action: log "PC=%X A=%X", PC, A`.
- Watch a dispatch table (MASK): `Address: 0x4000, Mask: 0xFFF0` -
  triggers for 0x4000..0x400F (= dispatch slot regardless of the low
  nibble).

**Detail:** `match-modes.md`.

## MEM_R

**Semantics:** fires when the CPU reads a byte from memory at the
given address. Reads by the debugger itself (disassembler, Memory
Browser, Watch, stack view, MCP `mem_read` and `dasm`) do not fire it.

**Relevant fields:** same as PC_EXEC (addr / addr_end / mask / zone /
bank). Zone awareness: `MEM_R` in zone `ROM_LOWER` fires only if the
read address is currently mapped in ROM (= banking-aware).

**Context for the condition expression:**

- `Address` = read address
- `Value` = the byte being read
- `IsRead = 1`, `IsWrite = 0`, `IsExec = 0`, `IsPort = 0`
- `BankAddr` = active zone for `Address`
- `InsnPC` = address of the reading instruction (`PC` is already behind it)

**Use case:**

- Watch a variable read/write: `Type: MEM_R, Address: 0xE100, Action:
  log "read $%X = %X (PC=%X)", Address, Value, InsnPC`.
- Detect a ROM dump attempt: `Type: MEM_R, Address: 0x0000, End Addr:
  0x0FFF, Match Mode: RANGE`.

**Detail:** `match-modes.md`, `expression-syntax.md`.

## MEM_W

**Semantics:** same as MEM_R but for a write. Fires before the
physical write (= the condition can read the old value). Writes by the
debugger (Memory Browser, MCP `mem_write` and `region_write`, byte
freeze) do not fire it; the `poke` action of the Action DSL does.

**Context:**

- `Address` = target address
- `Value` = the byte being written
- `IsRead = 0`, `IsWrite = 1`, `IsExec = 0`, `IsPort = 0`

**Use case:**

- Detect a write to a forbidden area: `Type: MEM_W, Address: 0xC000,
  End Addr: 0xCFFF, Match Mode: RANGE`.
- Trigger after a magic value is set: `Type: MEM_W, Address: 0xE000,
  Condition: Value == 0x42`.

## IORQ_R

**Semantics:** fires on an I/O port read instruction (`IN A,(n)`,
`IN r,(C)`, `INI`, `IND`, ...). Fires after the physical read (= the
condition sees the value already read).

**Relevant fields:**

- Port - primary port (also serves as the lower bound for RANGE)
- End Port - upper bound for RANGE
- Port Match Mode - SINGLE / RANGE / MASK
- Port Mask - AND mask
- Port Mode - **8BIT** (default) = match only the low byte of the
  port; **16BIT** = match the full BC pattern
- common fields

**Match Mode:** SINGLE / RANGE / MASK on port. Plus `port_mode`
(8BIT / 16BIT) - see `match-modes.md` section "IORQ port mode".

**Context:**

- `Address` = port value (8 or 16 bit according to `port_mode`)
- `Value` = the byte read
- `IsPort = 1`, `IsRead = 1`

**Use case:**

- Watch a CTC port: `Type: IORQ_R, Port: 0xCC, Port Mode: 8BIT`.
- Detect 16-bit IO probing: `Type: IORQ_R, Port: 0x42CE, Port Mode:
  16BIT, Match Mode: SINGLE`.

## IORQ_W

**Semantics:** ditto IORQ_R but for a write (`OUT (n),A`, `OUT (C),r`,
`OUTI`, `OUTD`, ...). Fires after the physical write.

**Context:**

- `Address` = port
- `Value` = the byte being written
- `IsPort = 1`, `IsWrite = 1`

**Use case:**

- Trace writes to the GDG palette: `Type: IORQ_W, Port: 0xF0, End
  Port: 0xF3, Match Mode: RANGE, Action: log "PAL[%X] = %X", Address
  & 3, Value`.

## IRQ

**Semantics:** fires **POST-dispatch** - after the Z80 INT
acknowledge cycle completes (= PC is already pointing at the ISR jump
target).

**Relevant fields:**

- IM 0/1/2 enabled - per-IM mode discriminator. At least one must be
  true (= UI validation), otherwise the BP never fires. The default
  for a new IRQ BP = all true.
- IM 0 RST mask - 8-bit bitmask for filtering the IM 0 RST opcode
  (bit 0 = RST 00h opcode 0xC7, bit 1 = RST 08h 0xCF, ... bit 7 =
  RST 38h 0xFF). Mask 0 = match-all.
- IM 2 Vector filter (enable + addr + Match Mode + end + mask) -
  filter on the IM 2 vector table address `(I << 8) | vec`. The whole
  address including bit 0 is compared (the Z80 does not mask bit 0 of
  the vector); bit 0 can be left out with MASK mode and mask 0xFFFE.
- IM 2 ISR filter (enable + addr + Match Mode + end + mask) - filter
  on the ISR jump target (= PC after dispatch).
- common fields

**Match Mode:** SINGLE / RANGE / MASK for both IM 2 vector and ISR.

**UI fields:** Type dropdown, IM 0/1/2 checkboxes, IM 0 RST opcode
checkboxes (8 bits), IM 2 Vector enable + addr + match mode, IM 2
ISR enable + addr + match mode.

**Context:**

- `Address` = vector_addr (for IM 2) or 0 (otherwise)
- `Value` = raw INT vector byte on the bus
- other fields standard

**Use case:**

- Stop on IM 1 dispatch (legacy MZ-700 BIOS hook): `Type: IRQ, IM1
  only`.
- Watch a specific IM 2 ISR: `Type: IRQ, IM2 only, IM2 ISR enabled,
  ISR addr: 0x8800`.
- Detect a runaway RST 38h (= NULL pointer call): `Type: IRQ, IM0
  only, RST mask bit 7 set`.

**Detail:** `irq-filter.md`.

## HW_EVENT

**Semantics:** fires on a named HW event from the emu code - e.g.
vsync, CTC zero-cross, GDG palette write, CMT signal change. 28
events total in 4 categories (signal / change / point-param /
point-noparam).

**Relevant fields:**

- Event name - persistence name (e.g. `vsync`, `ctc:zc0`,
  `raster:192`)
- Event Param - parameter (e.g. raster row for `raster:N`)
- Event Trigger - trigger condition (RISING / FALLING / CHANGED /
  LOW / HIGH); applied only to signal events. Default RISING.
- common fields

**Match Mode:** event-specific. Signal events - trigger condition.
Point events with a parameter - event_param value. Change events -
implicit "happened".

**UI fields:** Type dropdown, Event dropdown (28 named events), Param
textbox (only for `raster:N`), Trigger dropdown (only for signal
events).

**Context (per-kind):**

| Kind | `Address` | `Value` |
|------|-----------|---------|
| SIGNAL | 0 | current signal level (0/1) |
| CHANGE | 0 | new value (mode/palette/palgrp/border) |
| POINT_PARAM | parameter (e.g. raster row) | 0 |
| POINT_NOPARAM | 0 | info data (IM/IFF new value) |

**Use case:**

- Frame stop: `Type: HW_EVENT, Event: vsync, Trigger: rising` =
  stops at the start of every frame.
- Mid-frame palette change detect: `Type: HW_EVENT, Event:
  palette_change`.
- Raster line trigger: `Type: HW_EVENT, Event: raster, Param: 192`.
- NMI dispatch: `Type: HW_EVENT, Event: cpu:nmi`.

**Detail:** `hw-events.md`.

## SP_THRESHOLD

**Semantics:** a stack-related BP with edge-triggered logic. Two
modes:

- **SINGLE** - fires on a descending crossing of a single threshold
  (`old_sp >= sp_threshold && new_sp < sp_threshold`). A sustained
  state below the threshold does not fire again until SP rises above
  the threshold and drops again (= stack overflow detect without
  spam).
- **WINDOW** - fires if SP **leaves** the range
  `[sp_threshold..sp_upper]` (= was inside, is now outside).
  Edge-triggered.

Fires after every instruction if SP changed value.

**Relevant fields:**

- SP threshold - lower bound (SINGLE: threshold; WINDOW: lower bound)
- SP upper - upper bound (only WINDOW; if `hi < lo` the runtime
  defensively swaps)
- SP Mode - SINGLE / WINDOW
- common fields

**Match Mode:** the SP mode is a separate enum (no addr_match_mode).

**UI fields:** Type dropdown, SP Mode radio (Single / Window), SP
threshold textbox, SP Upper textbox (Window).

**Context:**

- `Address` = current SP (new_sp)
- `Value` = SP threshold (= the threshold)

**Use case:**

- Stack overflow detect: `Type: SP_THRESHOLD, SP Mode: Single, SP
  threshold: 0x4000` - fires if SP drops below 0x4000.
- Cross-task switch: `Type: SP_THRESHOLD, SP Mode: Window, lower:
  0xF000, upper: 0xFFFF`.

## GLOBAL

**Semantics:** a BP without address / port / event - just a
condition expression evaluated **per-instruction**. Expensive (=
per-instruction overhead), so by default OFF and evaluated only if at
least one GLOBAL BP exists.

Edge-triggered semantics: if `edge_triggered = true`, it fires only
on the condition transition `false -> true`.

**Relevant fields:**

- Condition expression (mandatory, otherwise the BP fires on every
  instruction)
- Edge-triggered flag
- common fields

**Match Mode:** none (= condition only).

**UI fields:** Type dropdown, Condition expression (mandatory), Edge
trigger checkbox.

**Context:** `Address = 0`, `Value = 0`, `Is*` all 0. CPU registers
and `BankPC` are available.

**Use case:**

- Watch a specific CPU state: `Type: GLOBAL, Condition: HL == 0x8000
  && A == 0x42, edge`.
- Detect an IFF1 transition: `Type: GLOBAL, Condition: IFF1, edge`.

## IRQ_SIG

**Semantics:** fires **PRE-dispatch** - before the Z80 INT
acknowledge, on the raise edge of the INT line (prev=0, curr=1) of a
specific peripheral source.

Complements IRQ (post-dispatch). Use case: detecting IRQ requests
that the CPU **does not dispatch** (= EI masking, or INT line raise
when IFF1=0) - IRQ BP would never see them.

**Relevant fields:**

- Source mask - 8-bit bitmask:
  - bit 0 = Z80 PIO port A
  - bit 1 = Z80 PIO port B
  - bit 2 = CTC channel 2
  - bit 3 = WD279x FDC
  - bit 4 = undetectable / bus latch (Other)
- common fields

**Match logic:** OR semantics. The BP fires if
`(source_mask & active_sources) != 0`. A multi-source BP = any of
the selected sources.

**UI fields:** Type dropdown, Source checkboxes (5 sources). At
least one must be checked (= UI validation), otherwise the BP never
fires.

**Context:** `Address = 0`, `Value = active_sources` (raw bitmask
from the current edge).

**Use case:**

- Watch FDC IRQ requests (= including masked): `Type: IRQ_SIG,
  Source: FDC`.
- Detect a race between PIOZ80 port A and CTC2: `Type: IRQ_SIG,
  Sources: PIOZ80_A + CTC2`.

**Detail:** `irq-sig.md`.

## Feature matrix

Which field is relevant for which type. **Y** = relevant, **-** =
ignored (= default 0/NULL).

| Field | PC_EXEC | MEM_R | MEM_W | IORQ_R | IORQ_W | IRQ | HW_EVENT | SP_THR | GLOBAL | IRQ_SIG |
|-------|---------|-------|-------|--------|--------|-----|----------|--------|--------|---------|
| addr | Y | Y | Y | - | - | - | - | - | - | - |
| addr_end | Y | Y | Y | - | - | - | - | - | - | - |
| addr_match_mode | Y | Y | Y | - | - | - | - | - | - | - |
| addr_mask | Y | Y | Y | - | - | - | - | - | - | - |
| port | - | - | - | Y | Y | - | - | - | - | - |
| port_end | - | - | - | Y | Y | - | - | - | - | - |
| port_match_mode | - | - | - | Y | Y | - | - | - | - | - |
| port_mask | - | - | - | Y | Y | - | - | - | - | - |
| port_mode | - | - | - | Y | Y | - | - | - | - | - |
| zone | Y | Y | Y | - | - | - | - | - | - | - |
| bank_id | Y | Y | Y | - | - | - | - | - | - | - |
| bank_match_mode | Y | Y | Y | - | - | - | - | - | - | - |
| bank_id_end | Y | Y | Y | - | - | - | - | - | - | - |
| bank_id_mask | Y | Y | Y | - | - | - | - | - | - | - |
| event_name | - | - | - | - | - | - | Y | - | - | - |
| event_trigger | - | - | - | - | - | - | Y* | - | - | - |
| event_param | - | - | - | - | - | - | Y* | - | - | - |
| sp_threshold | - | - | - | - | - | - | - | Y | - | - |
| sp_upper | - | - | - | - | - | - | - | Y* | - | - |
| sp_mode | - | - | - | - | - | - | - | Y | - | - |
| im0_enabled | - | - | - | - | - | Y | - | - | - | - |
| im1_enabled | - | - | - | - | - | Y | - | - | - | - |
| im2_enabled | - | - | - | - | - | Y | - | - | - | - |
| im0_rst_mask | - | - | - | - | - | Y* | - | - | - | - |
| im2_vector filter | - | - | - | - | - | Y* | - | - | - | - |
| im2_isr filter | - | - | - | - | - | Y* | - | - | - | - |
| irq_sig_source_mask | - | - | - | - | - | - | - | - | - | Y |
| condition expression | optional across all types |
| action | optional across all types |
| hit count, skip count | across all types |
| edge-triggered | mainly GLOBAL (per-instruction edge tracking) |

`Y*` = relevant only for a subset (signal events, IM 2 mode, IM 0
mode, etc. - see the per-type sections).

## Decision tree - which type when

```
What do you want to debug?
|
+-- a specific code address
|   |
|   +-- a single point        -> PC_EXEC + Match SINGLE
|   +-- a whole code block    -> PC_EXEC + Match RANGE
|   +-- a dispatch table      -> PC_EXEC + Match MASK
|
+-- a memory access
|   +-- read                  -> MEM_R
|   +-- write                 -> MEM_W
|   +-- (banking-aware: + Zone)
|
+-- an I/O port
|   +-- read                  -> IORQ_R (+ Port Mode 8BIT/16BIT)
|   +-- write                 -> IORQ_W
|
+-- an interrupt
|   +-- before dispatch (= including masked) -> IRQ_SIG (+ Source mask)
|   +-- after dispatch (= ISR target known)  -> IRQ (+ IM filter / vector / ISR)
|
+-- a named HW event
|   +-- vsync / hsync / blanking        -> HW_EVENT signal (+ trigger)
|   +-- CTC zero-cross / IRQ lines      -> HW_EVENT signal
|   +-- GDG palette / mode / border     -> HW_EVENT change
|   +-- raster line                     -> HW_EVENT raster:N
|   +-- CMT in/out / motor              -> HW_EVENT signal
|   +-- CPU NMI / DI / HALT / RESET     -> HW_EVENT cpu:*
|
+-- the stack
|   +-- single threshold (overflow)     -> SP_THRESHOLD SINGLE
|   +-- window (corruption / switch)    -> SP_THRESHOLD WINDOW
|
+-- a per-instruction expression
    +-- "whenever X"                    -> GLOBAL (+ edge_triggered)
```

## Related documents

- `match-modes.md` - SINGLE / RANGE / MASK detail
- `expression-syntax.md` - condition grammar
- `action-dsl.md` - action DSL commands
- `irq-filter.md` - IRQ post-dispatch detail
- `irq-sig.md` - IRQ_SIG pre-dispatch detail
- `hw-events.md` - 28 events vocabulary
- `persistence.md` - `.bpt` JSON schema
