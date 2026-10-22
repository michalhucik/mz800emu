# Reading trace records page by page (emu_trace_read)

Recipe for an AI client that needs the instruction trace, I/O accesses,
interrupt events, chip events, breakpoint markers or Event Viewer events
of a program run - without parsing the binary chunk files. Use
`emu_trace_status` and `emu_trace_read`.

## Channels

| Channel | One record per | Control |
|---------|----------------|---------|
| `cputrack` | executed instruction (and accepted interrupt) | `emu_trace_start` / `emu_trace_stop` |
| `iorqlog` | I/O port access (and MMIO 0E000h-0E008h in MZ-700 mode) | same |
| `intlog` | interrupt signal / state change | same |
| `hwlog` | chip event (GDG, PIO, CTC, PSG, FDC, ...) | same |
| `marklog` | breakpoint action `mark "name"` | same |
| `eventlog` | Event Viewer ring entry | `emu_eventlog_start` / `_stop` / `_clear`, `emu_eventlog_set_mask` |
| `ramlog` | write into memory (RAM write log) | `emu_ramlog_start` / `_stop` / `_clear` - see `emulator://docs/ramlog` |
| `audio_psg_writes`, `audio_levels`, `audio_frames` | audio capture memory: PSG write, level change, frame summary | `emu_audio_capture_start` / `_stop` - see `emulator://docs/audio_capture_read` |

## Recipe

1. `emu_trace_start(channel)` (or `emu_eventlog_start`), then run the
   section of interest (`emu_run`, a breakpoint, ...).
2. Optionally `emu_trace_status(channel)`: `next_seq` = number of
   records, `has_time` = rows carry the time column `t`.
3. `emu_trace_read(channel)` reads from the oldest record;
   `start="newest"` reads the last `limit` records. Continue with
   `cursor=<next_cursor>` until `end_of_data` is true.
4. Recording may keep running: `end_of_data` only means "nothing newer
   yet". Run the emulator again and read on with the same cursor.
5. `emu_trace_stop` closes the recording; the last segment stays
   readable until the channel is started again.

Records still in the RAM buffer are readable too, so even a short run
(well below the 64 MB chunk size) returns data - there is no need to
`emu_trace_save` first.

## Keeping pages small

- A `rows` page stays within `max_chars` (default 20 000 characters),
  below the size at which Claude Code moves a tool result into a file.
- Ask only for the columns you need (`columns=["seq","pc","asm"]`).
- Filter on the server: `filter={"pc_from":0x8000,"pc_to":0x80FF}`
  (cputrack), `{"port":0xCF,"dir":"out"}` (iorqlog),
  `{"chip":0x20}` (hwlog, PSG), `{"t_from":..., "t_to":...}`.
- A filter that matches nothing does not scan forever: after `max_scan`
  records the page returns `scan_exhausted: true`; continue with the
  cursor.

## Reading the rows

- `t` = GDG ticks since emulation start (the same clock in all channels,
  so cputrack rows can be matched with iorqlog rows by time). `frame` =
  emulated frame, `pos` = ticks since the start of the frame.
- cputrack `asm` is decoded from the bytes the CPU actually fetched (not
  from current memory - banking or self-modifying code cannot change
  it). `t_std` = standard T-states of the instruction, `wait` = extra
  WAIT T-states. A row with `asm` `INT ACK` is an accepted interrupt
  (`pc` = handler address, `t_std` = acknowledge T-states).
- cputrack `t` is missing (`null`, `has_time: false`) when the recording
  uses a PC range filter (`pc_range_lo/hi` in `[TRACE_CPUTRACK]`).
- HALT and a loop on itself (`JR $`) are one collapsed row whose `wait`
  is the whole time spent in the loop.
- Column `kind` (on request): `insn`, `loop` (collapsed row), `int`
  (interrupt acknowledge), `sync` (format 3 only, see below).

## Registers in the instruction trace

Questions like "what was HL just before this instruction" or "when did
SP become 10F0h" need a cputrack recording with registers:

1. `emu_trace_start("cputrack", regs=True)` - format 3, 32 bytes per
   record instead of 12 (`emu_trace_status`: `has_regs: true`,
   `format_version` 3). It fails while cputrack is already recording in
   format 2; stop it first.
2. Ask for register columns: `columns=["seq","pc","asm","hl","sp"]`
   (`af bc de hl ix iy sp wz` hex16, `i r` hex8, `iff1 iff2 im halted`
   numbers). They are not in the default columns.
3. `regs="after"` (default) shows the state after the record,
   `regs="before"` the state before it (= the previous record, also across
   pages; `null` where it is not known).
4. Find the moment a register got a value with a register filter:
   `filter={"hl":"0xC000"}` with `regs="after"` returns the records after
   which HL was C000h - the first row is where it got the value; a range
   is `{"sp":[0x1000,0x10FF]}`.
   Filters combine with `pc_from`/`pc_to`.

`sync` rows (`asm` `SYNC start`, `SYNC pc_range`, `SYNC write`) carry the
CPU state at the start of the recording, when execution returns into the
PC range record filter, and after a change outside an instruction (your
`emu_set_register`, a reset, a snapshot load, a breakpoint action `set`).
They take no time. `emu_trace_status` also returns `initial_regs`, the
state at the start of the last recording.

Without a trace, `emu_history_get` returns the registers before each of
the last 32 executed instructions (`regs` in every entry) whenever the
instruction history runs (debug window open, an enabled breakpoint, or
`[DEBUGGER] cpuhist_mode = ALWAYS`).

## Errors and gaps

- `segment_replaced: ...` - the channel was restarted (or the Event
  Viewer ring cleared) since the cursor was issued; read again from the
  start.
- `gap: {from_seq, to_seq}` - the Event Viewer ring overwrote these
  records before they were read; the page continues at `first_seq`.
  Trace Suite channels never lose records (`lost` is 0); they stop at
  `max_total_mb` (`truncated: true`) and the emulation keeps running.
  The byte limit of breakpoint actions (`[BREAKPOINTS] fwd_byte_limit_mb`,
  run result `stopped_by: "bp_action_saturation"`) pauses the emulation
  only for a channel that a breakpoint action started or saves, never
  for a trace started with `emu_trace_start`.
- `format="raw_b64"` returns the raw records (`record_size`,
  `format_version`) for scripts; the layouts are in the Trace Suite
  format documentation.
- `register columns and filters need a cputrack segment recorded with
  regs` - the recording is format 2; start a new one with `regs=True`.
