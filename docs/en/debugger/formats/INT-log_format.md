# Interrupt Log - export file format

## What is intlog

**intlog** is one of the trace-suite subsystems. It logs **all
interrupt-related events** in the emulator:

1. CPU INT pin state changes (rising/falling) + source identification
   (CTC2, PIO@PC2, PIOZ80 in general, PIOZ80@P[A,B][0..7])
2. CPU Z80 interrupt state changes (IM 0/1/2, IFF1, IFF2, RETI, EI)
3. PIO-Z80 internal state changes (ready, armed, IM2-jump, RETI-applied)

Used to debug interrupt-driven code (ISR), identify the IRQ source,
analyze race conditions between banking switching and IRQ delivery,
and verify correctness of EI/RETI sequences.

intlog is controlled from the menu `Debugger Settings -> Trace Suite ->
Interrupt Log` (Off / Only With Debug Window / Always + Save on Exit +
Max size [MB] + Chunk [MB] + Set directory...). Basename (name) only via
CLI / INI. Persistent in INI section `[TRACE_INTLOG]`.

## Export directory structure

```
<dir>/
    <name>.json                    # meta + chunks anchor + initial state
    <name>.000.bin                 # chunk 0 - per-event records
    <name>.001.bin                 # chunk 1
    ...
```

The initial state of the interrupt bus + PIO-Z80 + CPU IM/IFF is in
the manifest `<name>.json` `subsys_header` as a JSON field (= no separate binary
dump files).

## Per-event record (24 B fixed)

| Offset | Size     | Field                                          |
|--------|----------|------------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                        |
| 8      | 4 B      | screens_total (uint32 LE)                      |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                    |
| 16     | 1 B      | event_class (see below)                        |
| 17     | 1 B      | source_chip (see below)                        |
| 18     | 1 B      | source_pin (0-15 for PIOZ80@P[A,B][0..7], 0 otherwise) |
| 19     | 1 B      | edge (RISING=1 / FALLING=2 / NONE=0)           |
| 20     | 4 B      | state_bitmask (uint32 LE)                      |

### Field event_class

| Value | Symbol | Description |
|-------|--------|-------------|
| `0` | `INTLOG_EVENT_PIN_EDGE` | Change on the CPU INT pin (= one of the chips pulled/released INT). |
| `1` | `INTLOG_EVENT_CPU_INT_STATE` | Change of IM mode, IFF flags, RETI execution, or EI execution. |
| `2` | `INTLOG_EVENT_PIO_STATE` | Change of PIOZ80 internal state (ready/armed/IM2-jump/RETI-applied). |
| `3` | `INTLOG_EVENT_IRQ_ACK_IM2` | IM 2 IRQ acknowledge - contains vector_table_addr + isr_addr (= the address the Z80 loads into PC). |

### Field source_chip

| Value | Symbol | Description |
|-------|--------|-------------|
| `0` | `INTLOG_CHIP_NONE` | (reserved) |
| `1` | `INTLOG_CHIP_CTC2` | CTC2 + PIO@PC2 mask (= "CTC channel 2 OR PIO bit PC2") |
| `2` | `INTLOG_CHIP_PIOZ80` | PIOZ80 in general (combined edge on the CPU INT pin from the PIOZ80 source) |
| `3..10` | `INTLOG_CHIP_PIOZ80_PA0..PA7` | PIOZ80 Port A bit 0-7 (per-pin edge on masked input) |
| `11..18` | `INTLOG_CHIP_PIOZ80_PB0..PB7` | PIOZ80 Port B bit 0-7 (per-pin edge on masked input) |
| `19` | `INTLOG_CHIP_FDC` | FDC chip |
| `20` | `INTLOG_CHIP_CPU` | CPU itself (for `CPU_INT_STATE` events) |
| `21` | `INTLOG_CHIP_PIOZ80_PORT_A` | PIOZ80 Port A as a whole (for `IRQ_ACK_IM2` source = "vector came from Port A") |
| `22` | `INTLOG_CHIP_PIOZ80_PORT_B` | PIOZ80 Port B as a whole (for `IRQ_ACK_IM2` source = "vector came from Port B") |
| `23` | `INTLOG_CHIP_VECTOR_BUS_LATCH` | IM 2 vector source = bus latch / no chip supplied a vector. For `IRQ_ACK_IM2` events where PIOZ80 was not in PENDING state and no known chip (CTC2, FDC) holds the INT pin. In that case the CPU reads the last byte left on the data bus and takes entry `(I << 8) \| that byte`. |

### Field source_pin

For `INTLOG_CHIP_PIOZ80_PA0..PB7` it distinguishes a specific pin
(0-15 = PA0..PA7 + PB0..PB7). For other chips it is not interpreted
(= 0).

Per-pin granularity is active - the poller reads PIOZ80 masked input
PA / PB against the previously stored state and emits a per-pin edge
event for each change.

### Field edge

| Value | Symbol | Description |
|-------|--------|-------------|
| `0` | `INTLOG_EDGE_NONE` | For non-pin events (CPU_INT_STATE, PIO_STATE). |
| `1` | `INTLOG_EDGE_RISING` | INT pin -> 1 (chip starts requesting IRQ). |
| `2` | `INTLOG_EDGE_FALLING` | INT pin -> 0 (chip releases IRQ). |

### Field state_bitmask

Bit-packed flags. Semantics according to `event_class`:

#### For `event_class = CPU_INT_STATE`:

| Bit | Symbol | Meaning |
|-----|--------|---------|
| 0 | `INTLOG_STATE_BIT_IM0` | CPU in IM 0 state |
| 1 | `INTLOG_STATE_BIT_IM1` | CPU in IM 1 state |
| 2 | `INTLOG_STATE_BIT_IM2` | CPU in IM 2 state |
| 3 | `INTLOG_STATE_BIT_IFF1` | IFF1 flag (= interrupts enabled) |
| 4 | `INTLOG_STATE_BIT_IFF2` | IFF2 flag (LD A,I/R copies here) |
| 5 | `INTLOG_STATE_BIT_RETI` | Edge: RETI just executed |
| 6 | `INTLOG_STATE_BIT_EI` | Edge: EI just executed |

Active coverage:
- **EI events** - snapshot of IFF1 / IFF2 / IM at the moment of EI
  instruction.
- **RETI events** - emit on RETI dispatch to the PIOZ80 daisy chain.

#### For `event_class = PIO_STATE`:

| Bit | Symbol | Meaning |
|-----|--------|---------|
| 8 | `INTLOG_STATE_BIT_PIO_READY` | PIOZ80 ready state |
| 9 | `INTLOG_STATE_BIT_PIO_ARMED` | PIOZ80 armed state |
| 10 | `INTLOG_STATE_BIT_PIO_IM2_JUMP` | IM 2 IRQ ack happened, ISR jump set. Emitted concurrently with the `IRQ_ACK_IM2` event. |
| 11 | `INTLOG_STATE_BIT_PIO_RETI_APPLIED` | RETI dispatched - PIOZ80 returned from INTERRUPT_RECEIVED to INTERRUPT_NONE. |

READY/ARMED polling is active (compares PIOZ80 interrupt + ICW ENABLED
flag against the previous state and emits on change). IM2_JUMP and
RETI_APPLIED bits are edge bits (= one-shot, captured by snapshot at
the moment of the event, they do not hold any "state" between events).

#### For `event_class = IRQ_ACK_IM2`:

state_bitmask is reused as a 32-bit payload with two packed 16-bit
values:

| Bits | Field | Meaning |
|------|-------|---------|
| 0..15 | `vector_table_addr` | Address of the entry in the IM 2 table in memory = `(cpu->I << 8) \| vector` (bit 0 is not masked) |
| 16..31 | `isr_addr` | 16-bit ISR address read from memory at `vector_table_addr` (LE), i.e. the value that Z80 loads into PC. |

Encode at emit: `encoded = (uint32_t)vector_table_addr | ((uint32_t)isr_addr << 16);`

Decode in parser:
```python
vector_table_addr = state_bits & 0xFFFF
isr_addr = (state_bits >> 16) & 0xFFFF
```

`source_chip` distinguishes the source of the vector on the bus:
- `21` = `PIOZ80_PORT_A` (vector from PIOZ80 daisy chain, Port A)
- `22` = `PIOZ80_PORT_B` (vector from PIOZ80 daisy chain, Port B)
- `1`  = `CTC2` (CTC2 pulled INT, no intread_cb registered for CTC -
  nobody supplies the vector, the CPU reads the last byte on the data bus)
- `19` = `FDC` (FDC pulled INT, vector = last byte on the bus)
- `23` = `VECTOR_BUS_LATCH` (other / unresolvable source, vector = last byte on the bus)

`source_pin` = 0, `edge` = `NONE`.

The central hook also captures IRQs from non-PIOZ80 sources (CTC2, FDC,
bus latch).

#### For `event_class = PIN_EDGE`:

state_bitmask = 0 (= the field is unused, the edge is in the `edge`
field).

## Manifest `<name>.json`

```json
{
  "subsys": "intlog",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 97344,
  "truncated": false,
  "truncated_reason": "",
  "chunks": [
    { "index": 0, "file": "intlog.000.bin", "bytes": 24576,
      "start_pxclk": 0, "start_cpuclk": 0, "start_screens": 0 }
  ],
  "subsys_header": {
    "start_pxclk": 0,
    "start_pxclk_in_screen": 0,
    "start_screens": 0,
    "start_cpuclk": 0,
    "event_record_size": 24,
    "initial_state": {
      "interrupt_bus": {
        "raw": "0x00",
        "ctc2_pio_pc2": false,
        "pioz80": false,
        "fdc": false
      },
      "cpu_int": {
        "im_mode": 1,
        "iff1": false,
        "iff2": false
      },
      "pioz80": {
        "armed": false,
        "ready": false,
        "interrupt_vector": "0x00"
      }
    }
  }
}
```

### Initial state (subsys_header.initial_state)

- `interrupt_bus` - snapshot of the interrupt bus value at the moment
  recording starts, with a breakdown of active sources.
- `cpu_int` - initial CPU IM mode + IFF flags.
- `pioz80` - initial PIO-Z80 state (armed, ready, vector address).

Initial state + event sequence = full reconstruction of interrupt
history.

## Coverage

| What | Coverage |
|------|----------|
| Pin edge detection (CTC2, PIOZ80 combined, FDC) | active |
| EI events with IM/IFF state | active |
| Per-pin PIOZ80@P[A,B][0..7] | active (polling masked input PA/PB against previous state) |
| RETI events | active (emits `INTLOG_STATE_BIT_RETI`) |
| PIO_STATE events (READY/ARMED) | active (polling against previous state) |
| IRQ_ACK_IM2 events (vector + ISR addr) | active, covers PIOZ80 and non-PIOZ80 IRQ sources (CTC2, FDC, bus latch) |
| PIO_STATE bit IM2_JUMP | active, emitted only for IRQs where PIOZ80 was the actual dispatch source |
| PIO_STATE bit RETI_APPLIED | active |

## Usage

### From the UI

`Debugger Settings -> Trace Suite -> Interrupt Log`. Same UX pattern
as the other trace-suite subsystems.

### Loading data in Python (example)

```python
import json, struct
from pathlib import Path

CHIP_NAMES = {0: "NONE", 1: "CTC2", 2: "PIOZ80", 19: "FDC", 20: "CPU",
              21: "PIOZ80_PORT_A", 22: "PIOZ80_PORT_B",
              23: "VECTOR_BUS_LATCH"}
EVENT_CLASS_NAMES = {0: "PIN_EDGE", 1: "CPU_INT_STATE",
                     2: "PIO_STATE", 3: "IRQ_ACK_IM2"}

def load_intlog(meta_path):
    meta_path = Path(meta_path)
    meta = json.loads(meta_path.read_text())
    base_dir = meta_path.parent

    events = []
    for chunk in meta["chunks"]:
        data = (base_dir / chunk["file"]).read_bytes()
        n = chunk["bytes"] // 24
        for i in range(n):
            o = i * 24
            pxclk     = struct.unpack_from("<Q", data, o + 0)[0]
            screens   = struct.unpack_from("<I", data, o + 8)[0]
            px_in_scr = struct.unpack_from("<I", data, o + 12)[0]
            evcls     = data[o + 16]
            chip      = data[o + 17]
            pin       = data[o + 18]
            edge      = data[o + 19]
            state     = struct.unpack_from("<I", data, o + 20)[0]
            events.append((pxclk, screens, px_in_scr, evcls, chip, pin, edge, state))
    return meta, events

meta, events = load_intlog("./trace-suite/intlog.json")
# Example: when did the CPU last execute EI (= bit 6 in state)
ei_events = [e for e in events if e[3] == 1 and (e[7] & (1<<6))]
print(f"Total EI events: {len(ei_events)}")
```
