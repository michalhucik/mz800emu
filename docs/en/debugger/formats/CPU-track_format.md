# CPU Tracking Log - export file format

## What is cputrack

**cputrack** is the first of four trace-suite subsystems (together with
`iorqlog`, `intlog`, `hwlog`). It logs **one record per completed Z80
instruction** during emulator run. Used for reverse engineering, hot
path analysis, identification of self-modifying code and external
re-simulation of memory state.

cputrack is controlled from the menu `Debugger Settings -> Trace Suite
-> CPU Track`. It has three modes:

- `Off` (default) - no recording, no overhead.
- `Only With Debug Window` - active only when the debug window is open.
- `Always` - always active.

The same menu also has `Save on Exit` (finalize the recording when the
emulator exits), `Max size [MB]` (max recording size per channel,
0 = unlimited), `Chunk [MB]` (RAM buffer size), `Set directory...` (target
directory), `Record registers` (registers with every instruction, format
v3) and a PC range filter (lo/hi). The basename (name) is set via
CLI / INI only. All options are persistent in the INI section
`[TRACE_CPUTRACK]` (keys `mode`, `dir`, `name`, `chunk_mb`, `max_total_mb`,
`save_on_exit`, `regs`, `pc_range_lo`, `pc_range_hi`).

Recording runs in the shared slow path with CDL/cpuhist (callback
swap); the emulator hot path without active diagnostics stays unchanged
relative to the vanilla version.

## Export directory structure

```
<dir>/
    <name>.json                    # meta + chunks anchor + initial regs
    <name>_initial_ram.bin         # 64 KB CPU RAM dump
    <name>_initial_vram.bin        # VRAM: 16 KB on MZ-800, 4 KB on MZ-700 and MZ-1500
    <name>_initial_exvram.bin      # 16 KB EXVRAM (MZ-800 only)
    <name>_initial_pcg.bin         # 24 KB PCG (MZ-1500 only)
    <name>_initial_memext.bin      # 512 KB Memext (if connected)
    <name>.000.bin                 # chunk 0 - per-instr events
    <name>.001.bin                 # chunk 1
    ...
```

The content of RAM disks is not part of the initial state.

`<name>.json` is the text metadata file (see below). `<name>_initial_*.bin`
are snapshots of physical memories at the moment recording starts
(= header). `<name>.NNN.bin` are flat sequences of per-instruction
events.

The record size is the same within one recording (segment): 12 B in
formats v1 and v2, 32 B in format v3 with registers (option `regs`, see
[Format v3](#format-v3-record-with-registers-32-b)). The manifest values
`event_record_size` and `event_record_version` apply.

## Per-event record (12 B, formats v1 and v2)

| Offset | Size     | Field                                          |
|--------|----------|------------------------------------------------|
| 0      | 2 B      | regPC (uint16 LE)                              |
| 2      | 1 B      | insn_length (1-4)                              |
| 3      | 4 B      | insn_bytes (only the first insn_length bytes are valid) |
| 7      | 4 B      | wait_clk (uint32 LE)                           |
| 11     | 1 B      | t_std - standard T-states of the instruction (format v2) |

Endianness of the write is host byte order; on all supported emulator
platforms (Windows x86_64, Linux x86_64) this is little-endian.

### Field regPC

Address of the instruction at the moment of its execution (= PC before
executing the instruction, not PC after it). For `JP 1234h`, `regPC`
is the address of the `JP` opcode, not the jump target.

### Field insn_length

Number of valid bytes in `insn_bytes`. Z80 instructions have length 1-4 B
(prefix `CB`, `DD`, `ED`, `FD` + opcode + optional offset/imm bytes).

**Format v2: `insn_length` 0 = interrupt acknowledge.** Such a record
belongs to no instruction: `regPC` is the address of the interrupt
handler (PC after the acknowledge), `insn_bytes` and `wait_clk` are 0 and
`t_std` (byte 11) carries the T-states of the acknowledge (e.g. 19 for
IM 2). It is written between the last instruction before the interrupt
(or the collapsed HALT record the interrupt ended) and the first
instruction of the handler. Thanks to it, the sum of `t_std + wait_clk`
over all records matches the elapsed CPU time.

### Field insn_bytes

Up to 4 bytes of opcode + operands. Bytes beyond `insn_length` are
undefined (likely 0, but the parser must not rely on it).

The bytes are the ones the CPU fetched when it executed the instruction,
so they are correct also for an instruction that overwrites its own bytes
or switches the memory banking under itself (the memory may hold other
bytes at `regPC` right after the instruction).

From `regPC` + `insn_bytes[0..insn_length-1]` one can fully reconstruct:
- what the Z80 executed (= disassemble)
- the standard length in T-states (Z80 ISA table)
- which registers the instruction reads / writes
- which memory operations it does (from the register state that an
  external re-simulator tracks itself)

### Field wait_clk

Number of **extra WAIT T-states** beyond the standard length of the
instruction.

| Value | Meaning |
|-------|---------|
| `0` | No WAIT (common case for most instructions in the usual emulator state). |
| `1..0xFFFFFFFE` | WAIT lasted N CPU T-states. |
| `0xFFFFFFFF` | **Saturated sentinel** - WAIT was longer than ~20 minutes at 3.5 MHz, the value overflowed uint32. The external parser knows "long nothing, cannot determine exactly how long". A real Sharp would by then have lost DRAM anyway (refresh); the emu can hold the pause indefinitely. |

#### Source of WAIT T-states

The `wait_clk` value comes from two hot-path places where the emulator
inserts WAIT cycles:

1. **CPU access to the VRAM through the GDG** - the CPU waits for a free
   cycle of the video memory. On MZ-800 in both modes (the graphic VRAM in
   the MZ-800 mode, CG-RAM and text VRAM in the MZ-700 mode): a read
   always waits, a write goes through a one-byte buffer. The size depends
   on the beam position in the line. On MZ-700 and MZ-1500 only a VRAM
   access in the visible part of a line.
2. **OUT to PSG** - WAIT due to synchronization of CPU CLK with PSGCLK
   (PSG is 1.108 MHz, CPU 3.546 MHz; write must hit the PSG cycle
   boundary).

The WAIT T-state accumulator is a saturating uint32. If HALT/self-loop
collapse is active, accumulation continues across iterations (= the
sum of all WAIT + all standard T-states from the first iteration).

`insn_length`/`insn_bytes` in the event corresponds to the **standard
instruction** (= without WAIT). The standard length in T-states is
derivable from the opcode via Z80 ISA (see below).

### Field t_std (byte 11, format v2)

The standard length of the instruction in T-states without WAIT, as the
emulator actually counted it on execution (for conditional jumps
according to whether the jump was taken). For a collapsed HALT /
self-loop record it is 0, because `wait_clk` carries the whole time of
the loop.

The record format is given by `subsys_header.event_record_version` in
the manifest: 2 = byte 11 carries `t_std`. A manifest without this field
is format v1, where byte 11 was reserved (always 0) and the instruction
length had to be derived from the opcode via the Z80 table (for
conditional jumps from the flag state).

### HALT and self-loop collapse

HALT, `JP $`, `JR $-2`, `CALL` to itself (= any instruction that leaves
PC at the same address and repeats) is collapsed into **a single event**
with `wait_clk` = total time spent in the loop (until exit via IRQ
accept / state change).

Consequence: the tight-loop "wait for IRQ" idiom in MZ-800 monitor /
games (`HALT` or `JR $-2` in a polling loop) does not generate tens of
thousands of events per second, but one event with `wait_clk` equivalent
to the waiting time. This is a side-effect of cputrack design - **without
collapse, the log would be unusable on real software**.

An external re-simulator detects a collapsed HALT trivially: opcode =
`0x76` (HALT) + non-zero `wait_clk`. It detects a collapsed self-loop:
the instruction changes PC to PC, and `wait_clk` > 0.

## Format v3: record with registers (32 B)

With the option `regs` on (menu `Record registers`, INI `regs = 1`, CLI
`--cputrack-regs on`, MCP `trace_start` with `regs: true`) a new
recording writes 32 B per event. The option applies from the next start
of recording; one recording (segment) always has one record size. Bytes
0-11 mean the same as in v2, followed by the CPU state **after** the
event of the record:

| Offset | Size | Field |
|--------|------|-------|
| 0-11   | 12 B | as v2 (regPC, insn_length, insn_bytes, wait_clk, t_std) |
| 12     | 16 B | AF, BC, DE, HL, IX, IY, SP, WZ (uint16 LE, in this order) |
| 28     | 1 B  | I |
| 29     | 1 B  | R (full byte including bit 7) |
| 30     | 1 B  | flags: bit 0 IFF1, bit 1 IFF2, bits 2-3 IM, bit 4 HALT |
| 31     | 1 B  | record kind (bits 0-3), sync reason (bits 4-6), bit 7 = state before unknown |

Record kind (byte 31, bits 0-3):

| Value | Kind | State in bytes 12-30 |
|-------|------|----------------------|
| 0 | instruction | after the instruction |
| 1 | collapsed loop (HALT, loop on itself; `t_std` 0, time in `wait_clk`) | after the last collapsed iteration |
| 2 | interrupt acknowledge (`insn_length` 0) | after the acknowledge (PC = handler, SP 2 lower, IFF cleared) |
| 3 | sync (`insn_length` 0FEh) | current CPU state |

**The state before an event** is the state in the previous record (of
any kind). When bit 7 of byte 31 is set, the state before the record is
not known (the PC range filter broke the chain and it could not be
restored, e.g. an interrupt handled inside the filter range right after
an instruction outside it). The first record of a recording is always a
sync, so even the first instruction has a known state before it.

### Sync record

A sync record (`insn_length` 0FEh) belongs to no instruction and takes no
time (`t_std` 0, the `wait_clk` field does not apply):

- `regPC` = PC of the next instruction,
- bytes 3-10 = alternate set AF', BC', DE', HL' (uint16 LE),
- bytes 12-30 = current CPU state,
- bits 4-6 of byte 31 = reason: 0 start of recording, 1 return into the
  PC range filter, 2 CPU state changed outside an instruction.

It is written at the start of a recording, when an instruction outside
the PC range filter leads into it (the first instruction in the range then
has a known state before it), and when registers change outside an
instruction: a register write from the debugger or MCP, the breakpoint
action `set` at an instruction address, reset, snapshot load (also replay
rewind and seek), a register write played back from an input recording,
running an MZF by bootstrap. A sync is written only on
an actual change; a pause alone writes nothing.

Only sync records carry the alternate set. Between syncs a reader derives
it from the instructions `EXX` and `EX AF,AF'` (they swap it with the main
set).

### Cost

A record with registers is 2.7 times larger than v2 (at MAX SPEED with
the programs Gardener and Batman Demo about 1.4-1.8 GB of data per half
a minute instead of 0.6-0.8 GB) and recording takes about 11-18 % longer
than v2. Without the option `regs` the v2 recording does not change.

## Manifest `<name>.json`

```json
{
  "subsys": "cputrack",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 354432,
  "truncated": false,
  "truncated_reason": "",
  "chunks": [
    {
      "index": 0,
      "file": "cputrack.000.bin",
      "bytes": 96252,
      "start_pxclk": 354450,
      "start_cpuclk": 70890,
      "start_screens": 1
    }
  ],
  "subsys_header": {
    "start_pxclk": 354450,
    "start_pxclk_in_screen": 18,
    "start_screens": 1,
    "start_cpuclk": 70890,
    "initial_regs": {
      "AF": 36762, "BC": 10, "DE": 0, "HL": 57347,
      "AF_alt": 0, "BC_alt": 0, "DE_alt": 0, "HL_alt": 0,
      "IX": 0, "IY": 0, "SP": 4332, "PC": 58391,
      "WZ": 58391, "IR": 108, "I": 0, "R": 108,
      "IM": 1, "IFF1": 1, "IFF2": 1, "Q": 0, "halted": 0
    },
    "initial_ram_file": "cputrack_initial_ram.bin",
    "initial_vram_file": "cputrack_initial_vram.bin",
    "initial_exvram_file": "cputrack_initial_exvram.bin",
    "initial_memext_file": "cputrack_initial_memext.bin",
    "memext_size": 524288,
    "event_record_size": 12,
    "event_record_version": 2
  }
}
```

The example is a real manifest of a one-frame recording on MZ-800 with
MemExt connected. A longer recording has more entries of the same shape
in `chunks[]`. A recording with registers has `"event_record_size": 32`
and `"event_record_version": 3`.

### Key fields

- `subsys` - always `"cputrack"`.
- `platform` - `"MZ-800"`, `"MZ-700"` or `"MZ-1500"`.
- `pxclk_freq` - pixel clock frequency (Hz). MZ-800/700/1500 standard:
  17734475 (= 17.7 MHz). `cpu_freq = pxclk_freq / cpu_divider` can be
  derived.
- `cpu_divider` - pxCLK divider for CPU CLK. MZ-800: `5` (CPU 3.546895 MHz).
- `pxclk_per_screen` - number of pxCLK per one screen (raster cycle).
  Usable for raster-aligned analysis.
- `chunks[]` - list of chunks in order. Each chunk is an independent
  binary file.
- `truncated` - `true` if recording reached the `max_total_mb` limit.
- `truncated_reason` - explanation (`"max_total_mb"` or empty).

### chunks[i] anchor

Each chunk has an anchor with `start_pxclk`, `start_cpuclk`,
`start_screens` at the moment of chunk start. This enables **O(1) seek
to a chunk** + linear scan inside it to reconstruct the timestamp of
any specific instruction:

```
cumul_cpuclk(N) = chunk.start_cpuclk + sum_{i=0..N}( insn_T_states[i] + wait_clk[i] )
cumul_pxclk(N)  = chunk.start_pxclk + (cumul_cpuclk(N) - chunk.start_cpuclk) * cpu_divider
screens_at(N)   = floor(cumul_pxclk(N) / pxclk_per_screen)
pxclk_in_screen_at(N) = cumul_pxclk(N) mod pxclk_per_screen
```

`insn_T_states[i]` is byte 11 (`t_std`) in format v2; in v1 it is
derived from opcode according to the Z80 ISA table. In v2 the formula is
exact including interrupts (interrupt acknowledge records carry their
T-states); in v1 the T-states of each interrupt acknowledge are missing.
In v3 sync records (`insn_length` 0FEh) do not count in the sum.

`start_pxclk` is the start time of the first record of the chunk, so
record times continue seamlessly across chunk boundaries (chunk anchor =
anchor of the previous chunk + the sum of its records). When emulated
time jumps during recording (snapshot load, seek or rewind of an input
replay, a debugger bus operation that advances time), a new chunk starts
with an anchor at the real time; therefore always sum within one chunk
only. With a PC filter (`pc_range_lo` / `pc_range_hi` other than the
full range) records carry no time and the anchors are approximate.

### subsys_header: initial state

`initial_regs` contains a complete snapshot of Z80 CPU registers at the
moment recording starts (before the first logged event). Values are
integers (decimal): the 16-bit pairs (`AF` ... `PC`, `WZ`, `IR`), `I` and
`R` separately, the interrupt mode `IM`, `IFF1`, `IFF2`, `Q` and `halted`
(1 = the CPU stands on HALT).

`initial_*_file` are file names of the initial dumps of physical
memories, relative to the manifest directory: `initial_ram_file` and
`initial_vram_file` always, `initial_exvram_file` only on MZ-800,
`initial_pcg_file` only on MZ-1500. `initial_memext_file` is `null` when
MemExt is not connected; otherwise `memext_size` (dump size in bytes)
comes with it. Sizes of the other dumps are in the tables below.

The initial state + event sequence is a **hermetically closed dataset**
- an external re-simulator deterministically derives the memory state
at any moment from it (= no HW on MZ-800/700/1500 changes RAM
autonomously; all data appears via IORQ, which we log separately in
`iorqlog`).

## Initial memory dumps

Individual regions may differ per platform. The header contains only
those that exist / are connected for the given architecture.

### MZ-800

| File | Size | Description |
|------|------|-------------|
| `<name>_initial_ram.bin` | 64 KB | Main DRAM |
| `<name>_initial_vram.bin` | 16 KB | VRAM planes I and II (8 KB each); in the MZ-700 mode plane I also holds CG-RAM and the text VRAM |
| `<name>_initial_exvram.bin` | 16 KB | EXVRAM, planes III and IV (8 KB each) |
| `<name>_initial_memext.bin` | 512 KB | Memext RAM (only if connected) |

ROM is **not stored** - it is deterministically known from compile-time
embedded ROM image (= version in the manifest `<name>.json` via `platform`).
RAM disk content is not stored.

### MZ-700

| File | Size | Description |
|------|------|-------------|
| `<name>_initial_ram.bin` | 64 KB | Main DRAM |
| `<name>_initial_vram.bin` | 4 KB | Character and attribute VRAM |
| `<name>_initial_memext.bin` | 512 KB | Memext (if connected) |

### MZ-1500

| File | Size | Description |
|------|------|-------------|
| `<name>_initial_ram.bin` | 64 KB | Main DRAM |
| `<name>_initial_vram.bin` | 4 KB | Character and attribute VRAM |
| `<name>_initial_pcg.bin` | 24 KB | 3x 8 KB PCG bank (R/G/B) |
| `<name>_initial_memext.bin` | 512 KB | Memext (if connected) |

## Usage

### From the UI

`Debugger Settings -> Trace Suite -> CPU Track`:

1. Select recording mode (Off / Only With Debug Window / Always).
2. Optionally enable `Save on Exit` (default on) and `Record registers`
   (format v3 with registers).
3. Optionally set `Max size [MB]` (size limit), `Chunk [MB]`, the target
   directory (`Set directory...`) and the PC range filter; the basename
   (name) is set via CLI / INI.
4. Run the emulated program. Recording runs in the background.

### Loading data in Python (example)

```python
import json, struct
from pathlib import Path

def load_cputrack(meta_path):
    meta_path = Path(meta_path)
    meta = json.loads(meta_path.read_text())
    base_dir = meta_path.parent
    rs = meta["subsys_header"].get("event_record_size", 12)   # 12 or 32 (v3)

    events = []
    for chunk in meta["chunks"]:
        data = (base_dir / chunk["file"]).read_bytes()
        n_events = chunk["bytes"] // rs
        for i in range(n_events):
            o = i * rs
            pc          = struct.unpack_from("<H", data, o + 0)[0]
            insn_length = data[o + 2]
            insn_bytes  = data[o + 3 : o + 3 + insn_length]
            wait_clk    = struct.unpack_from("<I", data, o + 7)[0]
            events.append((pc, insn_length, bytes(insn_bytes), wait_clk))
    return meta, events

meta, events = load_cputrack("./trace-suite/cputrack.json")
# Example: how many HALT instructions there were
halt_count = sum(1 for (_, l, b, _) in events if l == 1 and b[0] == 0x76)
```
