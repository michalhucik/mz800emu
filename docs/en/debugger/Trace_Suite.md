# trace-suite - user documentation

A set of 5 independent sequential logging subsystems of the mz800new
emulator for in-depth analysis of executed code, I/O traffic,
interrupts, HW state changes and user markers.

## 5 subsystems

| Subsystem | What it logs                                       | Bytes/event |
|-----------|----------------------------------------------------|-------------|
| cputrack  | Every executed Z80 instruction (PC, bytes, WAIT)   | 12          |
| iorqlog   | Every IORQ IN/OUT (port, value, source addr)       | 24          |
| intlog    | INT pin changes, IM/IFF state, EI                  | 24          |
| hwlog     | Changes of HW chip state (GDG, PSG, CTC, PIO, MEMEXT, QD/FDC/RD) | 24 |
| marklog   | User markers from the `mark "name"` BP action      | 24          |

Each subsystem is independent - you can enable just one, any
combination, or all of them.

## Activation

### CLI options

```bash
./mz800emu.exe --cputrack-mode=always --cputrack-dir=./logs/run1
./mz800emu.exe --iorqlog-mode=window --intlog-mode=always
```

Per-subsystem options (each subsystem has identical ones):
- `--<sys>-mode=off|window|always`
- `--<sys>-dir=<path>` (default: `./trace-suite/`)
- `--<sys>-name=<basename>` (default: same as sys, e.g. `cputrack`)
- `--<sys>-chunk-mb=<N>` (default: 64)
- `--<sys>-max-total-mb=<N>` (default: 2048 = 2 GB; 0 = unlimited)
- `--<sys>-save-on-exit=on|off` (default: on)

`<sys>` = `cputrack` / `iorqlog` / `intlog` / `hwlog` / `marklog`.

Marklog also has:
- `--marklog-stdout=on|off` (default: `on`) - back-compat printout
  `[BP-MARK] <name>` to stdout when the STMT_MARK BP action fires.
  Independent of `--marklog-mode` (= you can have stdout without the
  binary log, or the binary log without stdout, or both / neither).

Hwlog also has:
- `--hwlog-hs-decimation=<N>` (default: `0` = HS/HBLN edges OFF;
  `N > 0` emit every Nth HS/HBLN edge. Full rate is ~31000 events/sec
  which destroys real-time performance - reasonable values are
  `100`-`1000` for troubleshooting or `1` for a full trace of a short
  section.)

Cputrack also has (range-scoped tracking):
- `--cputrack-pc-lo=<addr>` and `--cputrack-pc-hi=<addr>` (default `0`
  and `0xFFFF` = the whole address space, i.e. no filter). Limits the
  recording to only the instructions whose PC lies in the range
  `[lo, hi]` inclusive. Instructions outside the range are not recorded.
  Typical use: track only the TPA (e.g.
  `--cputrack-pc-lo=0x100 --cputrack-pc-hi=0xBFFF`) and skip the ROM/OS
  routines. Values accept dec or hex (`0x` prefix) and are masked to 16
  bits. The filter is cputrack-only - the other subsystems always log
  everything.
- `--cputrack-regs=on|off` (default `off`) - format 3: every instruction
  also carries the CPU registers after it (32 B per record instead of 12).
  The state before an instruction is the state in the previous record;
  sync records keep the chain at the start of the recording, on return
  into the PC range filter and after a register change outside an
  instruction. The format is described in
  [CPU-track_format.md](formats/CPU-track_format.md#format-v3-record-with-registers-32-b).

#### Shorthand `--all-traces-*`

```bash
./mz800emu.exe --all-traces-mode=always --all-traces-dir=./logs/run-1/
./mz800emu.exe --all-traces-mode=always --hwlog-mode=off   # everything except hwlog
```

`--all-traces-mode <off|window|always>` applies the value to all 5
subsystems at once; `--all-traces-dir <path>` does the same for the
output directory. A per-subsystem option (`--cputrack-mode` etc.) has
precedence - the shorthand fills in only those subsystems where the
user did not pass a per-subsys option.

### GUI menu

Debugger Settings -> Trace Suite -> [All channels | CPU Track | IORQ Log | Interrupt Log | HW Log | Marker Log]

Per-subsystem: radio (Off / Only With Debug Window / Always), a Save on Exit
toggle, a "Max size [MB]" field (max recording size per channel; 0 = unlimited),
a "Chunk [MB]" field (RAM buffer size before a flush to disk) and "Set
directory..." (choose the output directory). Size / directory changes take
effect when the channel recording (re)starts. The basename (name) is set via
CLI / INI only.

The "All channels" item sets the mode / Save on Exit / Max size / Chunk /
directory of all 5 channels (cputrack / iorqlog / intlog / hwlog / marklog) at
once with a single click.

The CPU Track submenu additionally has a "Record registers" toggle
(format 3 with registers, like CLI `--cputrack-regs`; applies from the
next start of recording) and a "PC range filter [lo,hi]:"
field with two hex inputs (lo, hi) and a "Reset range (whole space)"
item. It sets the same range-scope filter as the CLI
`--cputrack-pc-lo` / `--cputrack-pc-hi` (record only for PC in range).
Reset returns the range to `0000`-`FFFF` (= the whole address space, no
filter).

The Marker Log submenu additionally has a "Print marks to stdout" toggle
(back-compat printout `[BP-MARK] <name>` to stdout, independent of the binary
log).

#### Status indicator in the window title

If at least one subsystem is actively recording, a marker
`[trace: <flags>]` is appended after the standard text of the SDL main
window title, where `<flags>` is a space-separated list of letters:

| Letter | Subsystem |
|--------|-----------|
| `C`    | cputrack  |
| `I`    | iorqlog   |
| `N`    | intlog    |
| `H`    | hwlog     |
| `M`    | marklog   |

If a subsystem hit the `max_total_mb` limit and recording was stopped
(truncated), a suffix `!` appears after the letter (e.g. `H!`). If no
subsystem is active, the bracket does not appear in the title at all
(= in the off state the title is unchanged).

Examples:

```
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00 [trace: C I N H]
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00 [trace: C I]
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00 [trace: C I N H!]
```

Updated periodically via an SDL timer (~1x/s).

### INI config

Per-subsystem section in `mz800emu.ini` / `mz1500emu.ini`. Numbers in the
configuration file are **hexadecimal**, the way the emulator writes them
(`chunk_mb = 0x40` means 64 MB); a value without the `0x` prefix is read as
hexadecimal too (`chunk_mb = 64` would mean 100 MB, and
`pc_range_hi = 65535` is out of range and keeps the default):

```ini
[TRACE_CPUTRACK]
mode=OFF
dir=trace-suite
name=cputrack
chunk_mb=0x40                    # 64 MB
max_total_mb=0x800               # 2048 MB
save_on_exit=1
pc_range_lo=0x0000               # range-scope filter: lower PC bound (default 0)
pc_range_hi=0xffff               # upper PC bound (default 0xFFFF = no filter)

[TRACE_IORQLOG]
... (same structure)

[TRACE_INTLOG]
...

[TRACE_HWLOG]
...

[TRACE_MARKLOG]
mode=OFF
dir=trace-suite
name=marklog
chunk_mb=0x40
max_total_mb=0x800
save_on_exit=1
stdout_enabled=1                # back-compat printout [BP-MARK] to stdout
```

## Runtime control (MCP + Breakpoint Action)

Besides the static setup (CLI / GUI / INI), trace recording can also be
controlled at runtime - either from an MCP client (AI agent) or
automatically from a breakpoint action. This lets you bracket the
recording precisely around the section of interest, without having to
save only at emulator exit.

### Via MCP

The MCP server exposes a full lifecycle and record reading for the 5
binary channels (`cputrack`, `iorqlog`, `intlog`, `hwlog`, `marklog`):

| Tool | What it does |
|------|--------------|
| `emu_trace_start` | Starts recording the selected channel (= ALWAYS mode) |
| `emu_trace_stop`  | Stops recording the channel (the segment is closed and saved) |
| `emu_trace_reset` | Writes the pending chunk and the manifest to disk, recording continues (nothing discarded) |
| `emu_trace_save`  | Saves/redirects the channel segment to a new path |
| `emu_trace_status` | State of the channels for reading (segment, record count, chunks) |
| `emu_trace_read`  | One page of channel records (decoded rows or raw records) |

The lifecycle tools have a mandatory `channel` argument (one of
`cputrack`/`iorqlog`/`intlog`/`hwlog`/`marklog`). `emu_trace_save` additionally
has an optional `path` (target path of the following segment; without
it the current segment is just closed and reopened = "save now"). The
interface mirrors the CDL lifecycle tools (`emu_cdl_start` / `_stop` /
`_reset` / `_export`). For details and return values see the
[MCP tools overview](../mcp-server/tools-overview.md).

A typical flow: warp through the uninteresting part, `emu_trace_start`
just before the section of interest, run, then `emu_trace_stop` and
`emu_trace_save` with a segment name.

### Reading records via MCP

`emu_trace_read` returns the records of a channel page by page directly
via MCP, without parsing files and also while recording runs: it reads
both the chunks on disk and the data that is still only in the RAM
buffer (a short run can be read right away, it does not have to be
written to disk first). After `emu_trace_stop` the last segment stays
readable until the next start of the channel. The Event Viewer ring can
be read the same way (channel `eventlog`), as well as the memory of the
audio capture (channels `audio_psg_writes`, `audio_levels`,
`audio_frames`, see [Audio capture](../audio-capture.md)).

- Records have a sequence number `seq` within a segment (one recording
  from start to stop). Page through with the cursor `next_cursor`;
  `end_of_data` means the page reached the newest record - after more
  emulation, read on with the same cursor.
- A cursor from an older segment (the channel was restarted meanwhile)
  returns an error starting with `segment_replaced:`; read again from
  the start.
- The default output are decoded rows (for cputrack also the
  disassembly from the recorded bytes and the symbol); the page size is
  limited by a character budget (`max_chars`, default 20 000).
  `format: raw_b64` returns raw records for scripts.
- Filters: PC range (cputrack), port, direction and type (iorqlog),
  chip (hwlog), marker (marklog), time range (`t`).
- The time `t` is in GDG ticks since emulation start. For cputrack
  (format v2) it is computed from the chunk anchor and the T-states of
  the records (including interrupt acknowledge records); with the PC
  record filter (`pc_range_lo/hi`) instructions are missing, so
  `emu_trace_status` reports `has_time: false`.

For parameters and the response see the [MCP tools overview](../mcp-server/tools-overview.md).

### From a breakpoint action

A breakpoint action (Custom mode) can run the same recording operations
automatically when the BP fires - so instead of dozens of manual MCP
calls, a single well-placed breakpoint is enough. Available forward
commands:

- `cdl_start` / `cdl_stop` / `cdl_reset` - CDL lifecycle.
- `cdl_export "file"` - export the CDL to a file.
- `trace_start <channel>` / `trace_stop <channel>` - start/stop a trace
  channel (channel = `cputrack`/`iorqlog`/`intlog`/`hwlog`,
  case-sensitive).
- `trace_save <channel>, "file"` - save/redirect the channel segment.
- `snapshot "file"` - save a .mzs snapshot (works from a continuing
  breakpoint too, one that does not stop the emulator).

The file name in `cdl_export`, `trace_save` and `snapshot` is a
template with the same specifiers and `$variables` as the `log` command
- so you can generate numbered files, e.g.:

```
snapshot "snap-%d.mzs", $id
trace_save cputrack, "seg-%d.bin", $id
$id += 1
```

For the full description of the breakpoint action see
[Breakpoint - action on trigger](breakpoints/action-dsl.md).

## Mode semantics

| Mode        | Activation                                          |
|-------------|------------------------------------------------------|
| OFF         | Subsystem off, no overhead even in the hot path     |
| WITH_WINDOW | Active only while the debug window (Alt+D) is open  |
| ALWAYS      | Active continuously, regardless of the debug window |

## Output structure

Per-recording directory `<dir>/`:

```
trace-suite/
    cputrack.json                  # meta + chunks list
    cputrack_initial_ram.bin       # 64 KB CPU RAM dump at start
    cputrack_initial_vram.bin      # 32 KB VRAM
    cputrack_initial_exvram.bin    # 32 KB EXVRAM (mz800)
    cputrack_initial_pcg.bin       # 24 KB PCG (mz1500)
    cputrack_initial_memext.bin    # 512 KB Memext (if connected)
    cputrack.000.bin               # chunk 0 (per-instr events)
    cputrack.001.bin               # chunk 1
    ...
    iorqlog.json
    iorqlog.000.bin
    ...
    intlog.json
    intlog.000.bin
    ...
    hwlog.json
    hwlog.000.bin
    ...
    marklog.json                   # meta + chunks list + markers registry
    marklog.000.bin                # chunk 0 (per-marker events 24 B)
    ...
```

## Manifest `<name>.json` format

Each subsystem generates its own manifest `<name>.json`:

```json
{
  "subsys": "cputrack",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 97344,
  "truncated": false,
  "truncated_reason": "",
  "chunks": [
    { "index": 0, "file": "cputrack.000.bin",
      "bytes": 67108864,
      "start_pxclk": 12345,
      "start_cpuclk": 2469,
      "start_screens": 0 },
    ...
  ],
  "subsys_header": {
    ...per-subsystem specific fields...
  }
}
```

`chunks[i].start_*` enables O(1) seek to a chunk + linear scan inside
the chunk for timestamp reconstruction (cputrack: by summing
insn_T_states + wait_clk; iorqlog/intlog/hwlog: explicit timestamp in
every event).

## Per-subsystem event format

### cputrack (12 B)

| Offset | Size | Field                     |
|--------|------|---------------------------|
| 0      | 2 B  | regPC (uint16 LE)         |
| 2      | 1 B  | insn_length (1-4; 0 = interrupt acknowledge) |
| 3      | 4 B  | insn_bytes (only the first insn_length valid) |
| 7      | 4 B  | wait_clk (uint32 LE)      |
| 11     | 1 B  | t_std - standard T-states (format v2) |

`wait_clk == 0` = a normal instruction, `0xFFFFFFFF` = saturated
(typ. a long HALT loop >20 min at 3.5 MHz). Format v2 (manifest
`event_record_version: 2`): byte 11 carries the standard instruction
length, a record with `insn_length` 0 is an interrupt acknowledge
(`regPC` = handler address, `t_std` = acknowledge T-states). See the
[cputrack format](formats/CPU-track_format.md).

HALT/self-loop collapse: repeated execution of the same insn (HALT,
JP $, JR $-2) is collapsed into a single event with wait_clk = the
summed time.

### iorqlog (24 B)

| Offset | Size | Field                                  |
|--------|------|----------------------------------------|
| 0      | 8 B  | pxclk_total                            |
| 8      | 4 B  | screens_total                          |
| 12     | 4 B  | pxclk_in_screen                        |
| 16     | 1 B  | event_type (IORQ/MREQ/IORQ_UNCONNECTED) |
| 17     | 1 B  | direction (IN=0/OUT=1)                 |
| 18     | 2 B  | source_addr (regBC for IORQ, regPC for MREQ) |
| 20     | 2 B  | port_or_addr                           |
| 22     | 1 B  | value                                  |
| 23     | 1 B  | pulse_duration_pxclk                   |

Emitted event types:
- `IORQ` - regular IN/OUT (every port access via the Z80 IORQ cycle)
- `MREQ_MAPPED` - memory access into 0xE000-0xE008 (PIO8255 / CTC8253 /
  GDG status). For MZ-1500 unconditionally; for MZ-800 only in MZ-700
  mode with the upper ROM mapped in
- `IORQ_UNCONNECTED` - "ghost bus" IN to an unmapped port (the value
  returned comes from the bus latch)

### intlog (24 B)

| Offset | Size | Field                                  |
|--------|------|----------------------------------------|
| 0      | 8 B  | pxclk_total                            |
| 8      | 4 B  | screens_total                          |
| 12     | 4 B  | pxclk_in_screen                        |
| 16     | 1 B  | event_class (PIN_EDGE/CPU_INT_STATE/PIO_STATE/IRQ_ACK_IM2) |
| 17     | 1 B  | source_chip                            |
| 18     | 1 B  | source_pin (0-15 for PIOZ80@P[A,B][0..7]) |
| 19     | 1 B  | edge (RISING=1/FALLING=2)              |
| 20     | 4 B  | state_bitmask (IM/IFF1/IFF2/EI/RETI or (vector_table_addr \| isr_addr<<16) for IRQ_ACK_IM2) |

Covered event classes:
- `PIN_EDGE` - rising/falling on the CPU INT pin, source_chip = CTC2 /
  PIOZ80 / PIOZ80_PA0..PA7 / PIOZ80_PB0..PB7 / FDC. Per-pin PIOZ80
  granularity is active.
- `CPU_INT_STATE` - EI events with the current IM/IFF1/IFF2; RETI events
  (bit `INTLOG_STATE_BIT_RETI`).
- `PIO_STATE` - changes in the PIOZ80 internal state (READY / ARMED /
  IM2_JUMP / RETI_APPLIED bits).
- `IRQ_ACK_IM2` - 32-bit encoded payload: low 16 bits = vector_table_addr
  (entry in the IM 2 table), high 16 bits = isr_addr (= ISR address that
  the Z80 will load into PC). Source_chip distinguishes the vector
  source: `PIOZ80_PORT_A` / `_PORT_B` (vector from the PIOZ80 daisy
  chain), `CTC2` / `FDC` (INT pin pulled, vector = 0, dispatch onto
  (I:00)), `VECTOR_BUS_LATCH` (other / not resolvable). Key for
  debugging ISR dispatch.

### hwlog (24 B)

See the separate `docs/en/debugger/formats/HW-log_format.md`.

Covered per-chip register write hooks for:
- **GDG** (mz800 + mz1500) - MODE / BANKING / HWSCROLL / COLORS / WFRF
- **GDG_VIDEO** sync events (VBLN / VS edges; HS / HBLN with decimation)
- **PSG** (SN76489 register write) - audio engine RE
- **CTC8253** (control + counter writes)
- **PIO8255** (port A/B/C + control writes)
- **MEMEXT** (bank switch - LUFTNER + PEHU)
- **QD / FDC / RD** (register write events on disk operations)
- **PIOZ80** (Mode/Vector/INT_CTRL/Mask/IO_Select/Data writes, Data
  reads, Bus input change, M2 IRQ ack, RETI applied)

Plus an initial state binary snapshot in `<dir>/<name>_initial_state.bin`
(TLV per chip - chip_id + length + payload, EOF marker 0xFF). Each
payload is a field-by-field little-endian serialization independent of
compiler ABI - layout see `docs/en/debugger/formats/HW-log_INITIAL_STATE_format.md`.

The header contains per-chip dividers:
`psgclk_divider_cpuclk`, `tempo_divider_screen_rows`,
`ctc0_clk1m1_divider_pxclk`, `pio8255_cursor_divider_screens`.

### marklog (24 B)

See the separate `docs/en/debugger/formats/MARK-log_format.md`.

Logs user markers triggered from the BP action `mark "name"`. The
marker name is pre-registered during BP parse and is represented in the
binary by a stable `uint16 marker_id`. The id -> name mapping is dumped
into the manifest `<name>.json` `subsys_header.markers`. Marklog additionally has an
optional stdout printout (`[BP-MARK] <name>`) controlled by the
separate `stdout_enabled` flag.

## Performance impact

- OFF states: zero impact (a single if test, the branch predictor
  learns "always false")
- ON states: emulation slows down significantly (synchronous disk
  write on chunk swap, recording path through slow callbacks).
  **Acceptable - this is a debugging mode.** The console message
  `[trace-suite] <subsys>: chunk N swap to disk (XX B)` notifies of a
  flush.

## Limits

- chunk-mb (default 64 MB): RAM buffer per subsystem. Higher = less
  frequent flush, higher peak RAM
- max-total-mb (default 2048 = 2 GB per channel; 0 = unlimited): hard
  limit on the total recording size - a safeguard against filling the disk
  (cputrack otherwise grows essentially without bound). When reached, the
  subsystem stops, the manifest `<name>.json` gets
  `"truncated": true, "truncated_reason": "max_total_mb"` and a console
  message `[trace-suite] <subsys>: max-total-mb=N reached, recording
  stopped`. The emulation keeps running. The byte limit of breakpoint
  actions (`[BREAKPOINTS] fwd_byte_limit_mb`, it pauses the emulation)
  applies only to a channel that a breakpoint action started
  (`trace_start`) or saves (`trace_save`), not to a recording started
  from this menu or over MCP.

## External parsing

External tooling (Python, C, ...) can parse the manifest `<name>.json` + the binary
chunks according to the documented formats. No vendor-locked formats.

Re-simulation: hackers / aicoders / RE tooling can **compute** memory
accesses by deterministic re-simulation from (initial RAM dump in the
cputrack header + the sequence of opcodes + IN values in iorqlog). No
HW component on MZ-800/700/1500 modifies RAM autonomously without a
Z80 instruction.
