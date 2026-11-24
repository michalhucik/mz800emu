# HW Log - event format

The **hwlog** subsystem within the trace-suite. Logs HW chip state
changes with a pxCLK timestamp.

## Per-event record (24 B fixed)

| Offset | Size     | Field                                          |
|--------|----------|------------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                        |
| 8      | 4 B      | screens_total (uint32 LE)                      |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                    |
| 16     | 1 B      | chip_id (see table below)                      |
| 17     | 1 B      | sub_event_type (per-chip specific)             |
| 18     | 6 B      | payload (chip-specific layout)                 |

## Chip ID table

| ID   | Symbol                  | Description                              |
|------|-------------------------|------------------------------------------|
| 0x01 | HWLOG_CHIP_GDG_MODE     | DMD register write (0xCE)                |
| 0x02 | HWLOG_CHIP_GDG_BANKING  | Memory map / port E0-E6 OUT              |
| 0x03 | HWLOG_CHIP_GDG_HWSCROLL | HW scroll register (0x01CF-0x05CF)       |
| 0x04 | HWLOG_CHIP_GDG_COLORS   | Palette / PCG / border (0x06CF, 0xF0)    |
| 0x05 | HWLOG_CHIP_GDG_WFRF     | Write/Read Format register (0xCC, 0xCD)  |
| 0x06 | HWLOG_CHIP_GDG_VIDEO    | VS / VBLN edges + HS / HBLN with decimation |
| 0x10 | HWLOG_CHIP_PIO8255      | 8255 PPI write                           |
| 0x11 | HWLOG_CHIP_CTC8253      | 8253 CTC write                           |
| 0x12 | HWLOG_CHIP_PIOZ80       | Z80 PIO state events (write/read/IRQ ack/RETI/bus) |
| 0x20 | HWLOG_CHIP_PSG          | SN76489 PSG write                        |
| 0x30 | HWLOG_CHIP_QD           | Quick Disk SIO write                     |
| 0x31 | HWLOG_CHIP_FDC          | WD279x FDC write                         |
| 0x40 | HWLOG_CHIP_MEMEXT       | Memext bank switch                       |
| 0x41 | HWLOG_CHIP_RD           | Ramdisk write (STD + Pezik)              |

## Sub-event types and payload

### GDG_MODE (0x01)

sub_event_type: 0 (single type - DMD write)

Payload:
```
[0] addr_low (= 0xCE)
[1] addr_high
[2] value (4 lowest bits of DMD)
[3..5] reserved
```

### GDG_BANKING (0x02)

sub_event_type = low byte of port (E0..E6) - logically driven by GDG;
physically the map switching is done in the memory subsystem.

MZ-800 semantics:
- E0 = ROM 0000 OFF
- E1 = ROM E000 OFF (= disconnects the mapped ports area + ROM monitor
  upper)
- E2 = ROM 0000 ON
- E3 = ROM E000 ON
- E4 = ALL ON (= ROM 0000 + ROM E000, deactivates Prohibited)
- E5 = activates Prohibited mode (reads of $E000-$FFFF return 0x1A
  shadow byte)
- E6 = deactivates Prohibited mode (returns to default mapping per
  ROM_E000 / DMD)

MZ-700 semantics (after refactor unified with MZ-800):
- E0..E4 same as mz800
- E5 = activates Prohibited mode (= unmap ROM E800/code, reads of
  $E800-$FFFF return 0xFF; $F000-$FFFF also 0xFF)
- E6 = deactivates Prohibited mode (= remap ROM E800)

MZ-1500 semantics:
- E0..E4 same as mz800
- E5 = SPEC ON with value (= D000 mapping)
- E6 = SPEC OFF

Persistence of Prohibited (MZ-700 + MZ-800): holds across OUT
E0/E1/E2/E3 and across DMD bit 3 switch (700 <-> 800 native). Cleared
only by OUT E6, OUT E4 (= reset map), or HW reset.

Payload:
```
[0] mmap_port (E0..E6)
[1] value (raw byte; mz800/mz700 ignores, mz1500 uses for E5)
[2..5] reserved
```

### GDG_HWSCROLL (0x03)

sub_event_type: 1-5 (= addr_high byte, scroll register index)

Payload:
```
[0] addr_low (= 0xCF)
[1] addr_high (= sub_event_type)
[2] value
[3..5] reserved
```

### GDG_COLORS (0x04)

sub_event_type:
- `0x01` BORDER - port 0x06CF write
- `0x02` PALGRP - port 0xF0 write with bit 6 = 1
- `0x03` PAL - port 0xF0 write with bit 6 = 0 (palette value)
- `0x04` PCG - PCG write
- `0x05` PACKETGROUP - packet group write

Payload:
```
[0] addr_low (0xCF or 0xF0)
[1] addr_high
[2] value (raw written byte)
[3..5] reserved
```

### GDG_WFRF (0x05)

sub_event_type:
- `0x00` WF (Write Format register, port 0xCC)
- `0x01` RF (Read Format register, port 0xCD)

Payload:
```
[0] addr_low (0xCC or 0xCD)
[1] addr_high
[2] value
[3..5] reserved
```

### GDG_VIDEO (0x06)

Raster sync events. VS / VBLN edges are always emitted (~200 events/sec
at 50 fps). HS / HBLN edges (~31000 events/sec at full rate) are
emitted only if decimation is set (`hs_decimation > 0`).

sub_event_type:
- `0x01` VBLN_START - vertical blanking start
- `0x02` VBLN_END   - vertical blanking end
- `0x03` VS_START   - STS_VSYNC start
- `0x04` VS_END     - STS_VSYNC end
- `0x05` HBLN_START - horizontal blanking start (decimation)
- `0x06` HBLN_END   - horizontal blanking end (decimation)
- `0x07` HS_START   - STS_HSYNC start (decimation)
- `0x08` HS_END     - STS_HSYNC end (decimation)
- `0x09` CKSW       - write of the CKSW (Superimpose) bit, port 0x07CF (MZ-800); always emitted

Payload: 6 zeros for the sync edges (the timestamp in the event header
is sufficient). For CKSW: `[0]` addr_low, `[1]` addr_high, `[2]` the
byte written (bit 7 = CKSW).

#### HS / HBLN decimation

Configured via `[TRACE_HWLOG] hs_decimation = N` in INI or
`--hwlog-hs-decimation N` on CLI. Semantics:

| Value | Behavior |
|-------|----------|
| `0` (default) | HS/HBLN edges are NOT logged - safe default due to ~31000 events/sec at full rate. |
| `N > 0` | Every N-th edge is emitted (counter modulo N). The first edge after recording starts (counter=0) is always emitted for a consistent baseline. |

The counter is shared for all 4 sub-events (HS_START, HS_END,
HBLN_START, HBLN_END) - decimation is across types. If the external
parser needs per-type statistics, it computes them from the
sub_event_type field.

### PIO8255 (0x10)

sub_event_type:
- `0x01` PORT_A_WRITE - write to PA (keyboard column + JOY strobe +
  CURSOR reset bit 7)
- `0x02` PORT_B_WRITE - write to PB (keyboard data bus; typically
  read-only)
- `0x03` PORT_C_WRITE - write to PC (PC0..PC3 = output: audio blocking,
  CMT data, CTC2 INT blocking, CMT motor)
- `0x04` CONTROL_WRITE - write to CW register (Mode 0 init or bit
  set/reset)

Payload:
```
[0] addr (0..3)
[1] value (raw byte)
[2..5] reserved
```

The header contains the **CURSOR divider** in
`subsys_header.chip_dividers.pio8255_cursor_divider_screens` (= 25,
i.e. CURSOR signal toggles 1x per 25 screens; PA bit 7 = 0 resets
cursor_timer = blink synchronization).

### CTC8253 (0x11)

sub_event_type:
- `0x01` CONTROL_WRITE - write to CW register (= addr 3, format SC1/SC0
  RL1/RL0 M2/M1/M0 BCD)
- `0x02` COUNTER_WRITE - write to a data counter (= addr 0..2)

Payload:
```
[0] addr (0..3 = CTC0, CTC1, CTC2, CWREG)
[1] value (raw byte)
[2] pre-write rl_byte (only for COUNTER_WRITE - LSB/MSB order within
    the current CTC; for CONTROL_WRITE = 0)
[3..5] reserved
```

The header contains the **CTC0 1M1 divider** in
`subsys_header.chip_dividers.ctc0_clk1m1_divider_pxclk` (= 16 on
mz800/mz1500 = pxCLK/16 ~ 1.108 MHz on mz800) and its phase in
`ctc0_clk1m1_phase_pxclk`: the CLK0 falling edge is at
`pxclk_total % divider == phase` (6 on mz800, 0 on mz700 and mz1500).

### QD (0x30)

sub_event_type:
- `0x01` REGISTER_WRITE - write to QD SIO (ports 0xF4..0xF7)

Payload:
```
[0] SIO_addr (0..3 = data A, data B, ctrl A, ctrl B)
[1] value
[2..5] reserved
```

### FDC (0x31)

sub_event_type:
- `0x01` REGISTER_WRITE - write to WD279x (ports 0xD8..0xDB)

Payload:
```
[0] addroffset (0..3 = command/status, track, sector, data)
[1] value (raw byte in "normal" polarity - Sharp inverts data at the
    physical bus level, but io_data in fdc_write_byte is already in
    normalized form per the wd279x_write_byte convention)
[2..5] reserved
```

### RD (0x41)

sub_event_type:
- `0x01` STD_WRITE - write to std ramdisk (ports 0xE9, 0xEA, 0xEB, 0xFA)
- `0x02` PEZIK_WRITE - write to Pezik ramdisk (ports 0xE8, 0xEC..0xEF)

Payload:
```
[0] port low byte
[1] value
[2] port high byte (latch upper / address bits)
[3..5] reserved
```

### PIOZ80 (0x12)

A full state view of the Z80 PIO. We log all relevant changes:
write Mode/Vector/ICW/Mask/I/O Select, write/read data, external pin
change (CTC0 into PA4, VBLN into PA5), IM 2 IRQ ack and RETI completion.

sub_event_type:

| Hex  | Symbol                          | Trigger                               |
|------|---------------------------------|---------------------------------------|
| 0x01 | HWLOG_PIOZ80_MODE_WRITE         | write of Mode Control Word            |
| 0x02 | HWLOG_PIOZ80_VECTOR_WRITE       | write of Interrupt Vector (D0=0)      |
| 0x03 | HWLOG_PIOZ80_INT_CTRL_WRITE     | write of ICW or IDW (D3-D0 = 0111/0011) |
| 0x04 | HWLOG_PIOZ80_MASK_WRITE         | write of Mask Word (after ICW with MF=1) |
| 0x05 | HWLOG_PIOZ80_IO_SELECT_WRITE    | write of I/O Select Mask (after Mode 3) |
| 0x06 | HWLOG_PIOZ80_DATA_WRITE         | OUT to data port (0xFE/0xFF)          |
| 0x07 | HWLOG_PIOZ80_DATA_READ          | IN from data port (0xFE/0xFF)         |
| 0x08 | HWLOG_PIOZ80_BUS_INPUT_CHANGE   | external pin edge (PA4_CTC0, PA5_VBLN) |
| 0x09 | HWLOG_PIOZ80_IRQ_ACK_M2         | M2 IORQ INTA - interrupt vector returned |
| 0x0A | HWLOG_PIOZ80_RETI_APPLIED       | RETI passed PIO daisy chain           |

Payload (6 B):

```
[0]   port_id (0=A, 1=B, 0xFF=N/A for global events)
[1]   addr / sub_addr
        - DATA_WRITE/READ: addr 0..3 (0xFE/0xFF in hardware notation -
          actually only the low 2 bits of addr after dispatch)
        - control writes: control addr 0xFC/0xFD (= addr 0/1)
        - BUS_INPUT_CHANGE: bit pos of the pin (4 = PA4/CTC0, 5 = PA5/VBLN)
        - IRQ_ACK_M2: returned interrupt_vector (LSB)
        - RETI_APPLIED: 0
[2]   value
        - data writes/reads: raw byte
        - control writes: raw control byte written by the user
        - BUS_INPUT_CHANGE: new pin level (0 or 1)
        - IRQ_ACK_M2: copy of interrupt_vector (= byte returned to the CPU)
        - RETI_APPLIED: 0
[3..5] decoded_state_delta_bitmask (24 b LE) - bits HWLOG_PIOZ80_DELTA_*
```

#### Bits of decoded_state_delta_bitmask

Indicate what actually changed within the event. The external parser,
using the baseline (initial state PIOZ80 TLV - see
`HW-log_INITIAL_STATE_format.md`) + summation of changes, reconstructs
the state at any time.

| Bit  | Hex     | Symbol                            | Meaning                       |
|------|---------|-----------------------------------|-------------------------------|
| 0    | 0x00001 | HWLOG_PIOZ80_DELTA_MODE           | port->mode changed            |
| 1    | 0x00002 | HWLOG_PIOZ80_DELTA_IO_MASK        | port->io_mask changed         |
| 2    | 0x00004 | HWLOG_PIOZ80_DELTA_ICMASK         | port->icmask changed          |
| 3    | 0x00008 | HWLOG_PIOZ80_DELTA_ICENA          | port->icena changed           |
| 4    | 0x00010 | HWLOG_PIOZ80_DELTA_ICFNC          | port->icfnc (AND/OR) changed  |
| 5    | 0x00020 | HWLOG_PIOZ80_DELTA_ICLVL          | port->iclvl (HIGH/LOW) changed |
| 6    | 0x00040 | HWLOG_PIOZ80_DELTA_VECTOR         | port->interrupt_vector changed |
| 7    | 0x00080 | HWLOG_PIOZ80_DELTA_DATA_OUT       | port->data_output changed     |
| 8    | 0x00100 | HWLOG_PIOZ80_DELTA_PORT_INT       | port->port_int (INT FSM) changed |
| 9    | 0x00200 | HWLOG_PIOZ80_DELTA_MASKED_IN      | port->masked_input changed    |
| 10   | 0x00400 | HWLOG_PIOZ80_DELTA_INT_GLOBAL     | g_pioz80.interrupt changed    |
| 11   | 0x00800 | HWLOG_PIOZ80_DELTA_INT_PORT_ID    | g_pioz80.interrupt_port_id changed |
| 12   | 0x01000 | HWLOG_PIOZ80_DELTA_CTRL_EXPECT    | port->ctrl_expect changed     |

Bits 13-23 are reserved for future extensions (e.g. RETI re-arm
tracking, daisy chain forward).

#### Example event sequences

Initialization of PA to Mode 3 with I/O select 3Fh (typical MZ-800
BASIC pattern):

```
PIOZ80 MODE_WRITE       port_id=0 addr=0xFC value=0xCF delta=MODE|CTRL_EXPECT
PIOZ80 IO_SELECT_WRITE  port_id=0 addr=0xFC value=0x3F delta=IO_MASK|CTRL_EXPECT|MASKED_IN
```

Printing a byte to the printer (PB data + PA STROBE):

```
PIOZ80 DATA_WRITE  port_id=1 addr=0xFF value=0x41 delta=DATA_OUT
PIOZ80 DATA_WRITE  port_id=0 addr=0xFE value=0x7F delta=DATA_OUT
PIOZ80 DATA_WRITE  port_id=0 addr=0xFE value=0xFF delta=DATA_OUT
```

VBLN edge wakes INT (Mode 3 + EI):

```
PIOZ80 BUS_INPUT_CHANGE  port_id=0 addr=5 value=1 delta=MASKED_IN|PORT_INT|INT_GLOBAL
PIOZ80 IRQ_ACK_M2        port_id=0 addr=0x40 value=0x40 delta=PORT_INT|INT_GLOBAL|INT_PORT_ID
PIOZ80 RETI_APPLIED      port_id=0 addr=0 value=0 delta=PORT_INT|INT_GLOBAL|INT_PORT_ID
```

### MEMEXT (0x40)

sub_event_type:
- `0x01` BANK_SWITCH - write to the Memext mapping port (0xE0..0xE4
  on MZ-800 / MZ-1500). Writes a new raw bank for a 4 KB bus page.

Payload:
```
[0] addr_point (= bus page index, 0..15, 4 KB granularity)
[1] value (raw byte written to the mapping port)
[2] type (0 = LUFTNER, 1 = PEHU)
[3..5] reserved
```

The hook is activated only if `MEMEXT_TEST_CONNECTED`. The external
parser obtains the complete mapping state (16-entry Memext map) from
the initial state in the header + summation of BANK_SWITCH events. For
PEHU, 1 event = 2 adjacent entries (PEHU has 8 KB granularity = 2x 4 KB
pages).

### PSG (0x20)

sub_event_type:
- `0x01` REGISTER_WRITE - write a byte to the PSG data port (port
  0x70/0x71 on MZ-800; SN76489AN decodes internally via latch)

Payload:
```
[0] raw byte written by the user
[1] channel mask (mono = 0x01, stereo = bitmask PSG_CH_LEFT|RIGHT)
[2] stereo flag (0 = mono, 1 = stereo PSG module)
[3..5] reserved
```

Decoded info (latch_cs, attn flag, tone vs noise vs attn) is computed
by the external parser from the initial state in the header +
summation of REGISTER_WRITE events. The header contains **PSGCLK
divider** and **TEMPO divider** in `subsys_header.chip_dividers`.

## Chip coverage

Hooks are installed for: GDG_MODE, GDG_BANKING, GDG_HWSCROLL,
GDG_COLORS, GDG_WFRF, GDG_VIDEO, PSG, MEMEXT, CTC8253, PIO8255,
**PIOZ80**, QD, FDC, RD.

PIOZ80 is available both in hwlog (decoded state changes, bus events)
and in intlog (pin-edge and IRQ-flow events). Different abstraction
levels; dual logging is intentional and recommended.

## Recording header (manifest `<name>.json`)

```json
{
  "subsys": "hwlog",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 97344,
  "truncated": false,
  "chunks": [...],
  "subsys_header": {
    "start_pxclk": 12345,
    "start_pxclk_in_screen": 12345,
    "start_screens": 0,
    "start_cpuclk": 2469,
    "event_record_size": 24,
    "chip_dividers": {
      "psgclk_divider_cpuclk": 16,
      "tempo_divider_screen_rows": 229,
      "cpu_divider_pxclk": 5
    }
  }
}
```

`chip_dividers` contains constants for deriving timing:
- **psgclk_divider_cpuclk** - PSG step runs once per N CPU CLK (mz800 =
  16, mz1500 = 16 = same)
- **tempo_divider_screen_rows** - GDG TEMPO signal toggles once per N
  screen rows (~34 Hz on mz800/mz1500 = 229)
- **cpu_divider_pxclk** - CPU CLK = pxCLK / N (mz800 = 5, mz1500 = 4)

## Initial state binary snapshot

The file `<dir>/<name>_initial_state.bin` is created at the moment
recording starts and contains per-chip TLV records. **Per-chip payload
is serialized field-by-field in explicit little-endian format** -
parsing is independent of compiler ABI, alignment and padding.

```
[0]    chip_id        (1 B)
[1..4] payload_length (4 B uint32 LE)
[5...] payload        (per-chip layout, see HW-log_INITIAL_STATE_format.md)
```

EOF marker: chip_id = 0xFF, length = 0.

The dump contains:
- HWLOG_CHIP_PIO8255  - PA/PC signals + 10 B keyboard_matrix (= 50 B)
- HWLOG_CHIP_CTC8253  - 3x counter, 32 B per counter (= 96 B)
- HWLOG_CHIP_PSG      - stereo flag + 2x PSG state (variable, see spec)
- HWLOG_CHIP_MEMEXT   - connection/type/addr_mask + map[16] (= 68 B,
                        only if Memext is connected)
- HWLOG_CHIP_GDG_MODE - GDG core registers + raster state (architecture
                        marker further distinguishes MZ-800 vs MZ-1500
                        arch-specific fields)
- HWLOG_CHIP_PIOZ80   - per port (12 fields) + global (interrupt, port_id)

**Large data buffers NO** - main RAM (64 KB), VRAM (32 KB), Memext RAM
(512 KB), ramdisk content - these belong in **cputrack initial dump**
(a different subsystem with its own `_initial_*.bin` files). Hwlog
holds only register/config state.

Details of per-chip layout (offsets, field sizes, order) see the
separate document [HW-log_INITIAL_STATE_format.md](HW-log_INITIAL_STATE_format.md).

## External parser

The parser reads chunks in order (per `chunks[i].file`); each chunk is
a sequence of 24-byte records. The timestamp is explicitly present -
it does not have to be reconstructed. This differs from **cputrack**,
where it is reconstructed by summing wait_clk + insn_T_states.

## Shared 24 B layout with Event Viewer

The HW-log chunk shares a **bit-identical** per-event 24 B layout with
the in-memory ring of the **Event Viewer**.

Mapping field by field:

| HW-log offset | HW-log field         | Event Viewer field        | Size     |
|---------------|----------------------|----------------------------|----------|
| 0             | `pxclk_total`        | `pxclk_total`             | 8 B      |
| 8             | `screens_total`      | `screens_total`           | 4 B      |
| 12            | `pxclk_in_screen`    | `pxclk_in_screen`         | 4 B      |
| 16            | `chip_id`            | `category`                | 1 B      |
| 17            | `sub_event_type`     | `subtype`                 | 1 B      |
| 18..23        | `payload[6]`         | `pc` (2 B) + `payload` (4 B) | 6 B   |

The only difference is in the structure of the last 6 B:

- HW-log: 6 B raw payload bytes (per-chip layout)
- Event Viewer: 2 B CPU PC + 4 B uint32 payload

Cross-merging timestamps between HW-log and Event Viewer is 1:1 (= same
`pxclk_total` domain).

### Mapping chip_id -> Event Viewer category

Event Viewer uses its own enum of categories (24 values). Per-chip
translation:

| HWLOG_CHIP                | -> EVENTLOG_CAT                |
|---------------------------|--------------------------------|
| `HWLOG_CHIP_GDG_MODE`     | `EVENTLOG_CAT_GDG_MODE`        |
| `HWLOG_CHIP_GDG_BANKING`  | `EVENTLOG_CAT_GDG_BANKING`     |
| `HWLOG_CHIP_GDG_HWSCROLL` | `EVENTLOG_CAT_GDG_HWSCROLL`    |
| `HWLOG_CHIP_GDG_COLORS`   | `EVENTLOG_CAT_GDG_COLORS`      |
| `HWLOG_CHIP_GDG_WFRF`     | `EVENTLOG_CAT_GDG_WFRF`        |
| `HWLOG_CHIP_GDG_VIDEO`    | `EVENTLOG_CAT_GDG_VIDEO`       |
| `HWLOG_CHIP_PIO8255`      | `EVENTLOG_CAT_PIO8255`         |
| `HWLOG_CHIP_CTC8253`      | `EVENTLOG_CAT_CTC8253`         |
| `HWLOG_CHIP_PIOZ80`       | `EVENTLOG_CAT_PIOZ80`          |
| `HWLOG_CHIP_PSG`          | `EVENTLOG_CAT_PSG`             |
| `HWLOG_CHIP_QD`           | `EVENTLOG_CAT_QD`              |
| `HWLOG_CHIP_FDC`          | `EVENTLOG_CAT_FDC`             |
| `HWLOG_CHIP_MEMEXT`       | `EVENTLOG_CAT_MEMEXT`          |
| `HWLOG_CHIP_RD`           | `EVENTLOG_CAT_RD`              |

Sub_event_type values remain identical (= per-chip subtype enums are
the same in both subsystems).

### Mapping INTLOG / IORQLOG / marklog -> Event Viewer

Trace subsystems other than HW-log have their own per-record format,
but their hooks emit in parallel into the Event Viewer ring as well:

| Trace subsystem      | -> EVENTLOG_CAT             | Subtype source         |
|----------------------|------------------------------|------------------------|
| intlog CPU INT state | `EVENTLOG_CAT_CPU_INT`      | CPU INT reason         |
| intlog pin edge      | `EVENTLOG_CAT_CPU_PIN_EDGE` | pin index              |
| intlog IRQ ack IM2   | `EVENTLOG_CAT_IRQ_ACK_IM2`  | vector LSB             |
| iorqlog IN           | `EVENTLOG_CAT_IORQ_IN`      | IORQ subtype           |
| iorqlog OUT          | `EVENTLOG_CAT_IORQ_OUT`     | IORQ subtype           |
| marklog              | `EVENTLOG_CAT_USER_MARK`    | marker_id in payload   |

Plus Event Viewer specific categories with no parallel in the
trace-suite:

- `EVENTLOG_CAT_BP_FIRE` (BP fire events)
- `EVENTLOG_CAT_CPU_CTRL` (CPU control events)
- `EVENTLOG_CAT_MMIO_R` / `EVENTLOG_CAT_MMIO_W` (MMIO accesses)

Per-category details + filter syntax see [`../event-viewer.md`](../event-viewer.md).
