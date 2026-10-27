# MCP tools overview

The MCP server exposes a set of tools (= callable RPCs) for controlling
the emulator. This page is an overview - each tool is described
briefly, with a focus on what it does, what arguments it takes, and
whether it is destructive. Resources (= read-only URI endpoints) are
documented separately in [Resources overview](resources-overview.md).

## Tool list

| Tool | Sensitive? | Description |
|------|-----------|-------------|
| `emu_status` | no | Emu state (running / paused, connected) |
| `emu_ping` | no | Liveness check (`pong`) |
| `emu_pause` | no | Pauses emulation (optionally only the run `if_run_seq`) |
| `emu_resume` | no | Free run without blocking, optional deadline in emulated time |
| `emu_wait_stop` | no | Waits until a run stops (max 25 s per call): reason, PC, who started it |
| `emu_get_stop_history` | no | Newest 32 stop records (also `emulator://run/history`) |
| `emu_run` | no | Blocking resume for N frames (1..1000) |
| `emu_reset` | no | Power-on reset of CPU + peripherals |
| `emu_get_registers` | no | 14 Z80 registers (AF..IR) |
| `emu_set_register` | **YES** | Write 16-bit value to register (optionally only if it still holds `expected`) - **destructive** |
| `emu_dasm` | no | Disassemble N instructions from an address (banking-aware) |
| `emu_history_get` | no | 32 most recently executed instructions (ring buffer) with registers before each |
| `emu_mem_read` | no | Reads bytes from Z80 memory (base64 + SHA-256, or the hash only) |
| `emu_mem_write` | **YES** | Writes bytes into RAM - region checked, optionally only if the bytes still have `expected_sha256`, **destructive** |
| `emu_bp_add` | no | Adds a breakpoint (exec, or typed memw/memr/ior/iow + condition) |
| `emu_bp_list` | no | Lists breakpoints (id/addr/addr_end/addr_match_mode/addr_mask/enabled + type/zone/bank_id/bank_id_end/bank_match_mode/bank_id_mask/hits/condition) |
| `emu_bp_remove` | no | Removes a specific BP by ID |
| `emu_bp_clear` | no | Removes all breakpoints at once |
| `emu_bp_enable` | no | Toggles BP enabled flag (no removal) |
| `emu_step_into` | no | One Z80 instruction (steps INTO CALL/RST) |
| `emu_step_over` | no | One instruction, CALL/RST as atomic |
| `emu_step_n` | no | N steps as sequence of step_into (1..1000) |
| `emu_run_until_addr` | no | Run until PC == addr (uses temp BP), optional deadline |
| `emu_snapshot_save` | **YES** | Saves a .mzs snapshot to disk (overwrites) |
| `emu_snapshot_save_buffer` | no | Saves snapshot as inline base64 (no disk I/O) |
| `emu_snapshot_load` | **YES** | Loads .mzs from disk, **replaces emu state** |
| `emu_snapshot_load_buffer` | **YES** | Loads snapshot from inline base64, replaces state |
| `emu_cooperation_hint_set` | no | AI self-binding hint (read_only / paused_only / free) |
| `emu_symbol_add` | no | Adds a user-defined symbol (named address) |
| `emu_symbol_remove` | no | Removes a symbol (by name or by addr) |
| `emu_symbol_lookup` | no | Looks up a symbol by name or hex address (read-only) |
| `emu_symbol_list` | no | Lists symbols matching name prefix (max 1000) |
| `emu_bookmark_add` | no | Adds a named address bookmark (hex literal or symbol), owner mcp |
| `emu_bookmark_remove` | no | Removes a bookmark by ID |
| `emu_step_out` | no | Runs until RET from the current subroutine (callstack pop), optional deadline |
| `emu_run_until_raster` | no | Runs until the GDG raster reaches a target line (and optional column) |
| `emu_run_until_tstate` | no | Runs until a time: GDG ticks (`target_ticks`, the emulator stops itself) or the 32-bit T-state counter (`target_total_cycles`, stepping) |
| `emu_run_until_event` | no | Runs until an event fires (`frame_done`, `breakpoint_hit`) |
| `emu_event_subscribe` | no | Subscribe to a list of topics (`breakpoint_hit`, `paused`, `resumed`, `step_done`, `state_changed`, `io_write`) |
| `emu_event_unsubscribe` | no | Unsubscribe topics (empty list = all) |
| `emu_event_poll` | no | Retrieve pending events (timeout 0..60000 ms, max 1..100) |
| `emu_trap_respond` | no | Respond to a TRAP from a `breakpoint_hit` event (continue/step_into/step_over/abort) |
| `emu_io_read` | **YES** | Z80 IN port - real path with side effects, **destructive** |
| `emu_io_write` | **YES** | Z80 OUT port - real path with side effects, **destructive** |
| `emu_irq_inject` | **YES** | Force maskable IRQ (optional IM2 vector), **destructive** |
| `emu_nmi_inject` | **YES** | Force NMI - jumps to 0x0066, **destructive** |
| `emu_mem_write_force` | **YES** | Write bypassing region check (skips ROM safety), **destructive** |
| `emu_watch_add` | no | Adds a watch row (address / expr_scalar / expr_deref) |
| `emu_watch_remove` | no | Removes a watch row (by name or index) |
| `emu_watch_list` | no | Returns all watches with current values |
| `emu_watch_eval` | no | Evaluates a watch or ad-hoc expression |
| `emu_callstack_get` | no | Snapshot of the shadow stack |
| `emu_cdl_start` | no | Start Code/Data Logger |
| `emu_cdl_stop` | no | Stop CDL recording (data preserved) |
| `emu_cdl_reset` | no | Clear CDL counters |
| `emu_cdl_export` | no | Export CDL bitmap + meta to files |
| `emu_trace_start` | no | Start trace recording of a channel (cputrack/iorqlog/intlog/hwlog/marklog) |
| `emu_trace_stop` | no | Stop trace recording of a channel (segment closed) |
| `emu_trace_reset` | no | Flush the current trace channel segment to disk (nothing discarded) |
| `emu_trace_save` | no | Save/redirect a trace channel segment (optional path) |
| `emu_trace_status` | no | State of trace channels and the Event Viewer ring for reading |
| `emu_trace_read` | no | Page of trace / Event Viewer records (cursor, filter, character budget) |
| `emu_ramlog_start` | no | Start the RAM write log (ranges, ring / fill, capacity) |
| `emu_ramlog_stop` | no | Stop the RAM write log (records stay readable) |
| `emu_ramlog_clear` | no | Empty the RAM write log |
| `emu_ramlog_status` | no | State of the RAM write log |
| `emu_ramlog_who_wrote` | no | Last writer and write history of a memory cell |
| `emu_ramlog_writers` | no | Instructions that wrote into a range |
| `emu_bplog_clear` | no | Empty the `bplog` channel (output of BP actions `log` and `stop`) |
| `emu_videorec_start` | yes (creates / overwrites a file) | Start lossless video recording (optional path, auto-stop after N frames) |
| `emu_videorec_stop` | no | Stop video recording, by default wait until the files are saved |
| `emu_videorec_pause` | no | Recording pause (emulation keeps running): pause / resume / toggle |
| `emu_videorec_marker` | no | Marker (chapter) at the current recording position |
| `emu_videorec_status` | no | Video recording status (state, frames, segment, size, path, last event) |
| `emu_videorec_timebase` | no | Recording timebase: emulated time or real time |
| `emu_frame_hash` | no | SHA-256 of the screen picture and of the VRAM, beam position (no data) |
| `emu_replay_record_start` | yes (creates / overwrites a file) | Start recording inputs into a replay file (start snapshot, inputs at frame boundaries, state hashes) |
| `emu_replay_record_stop` | no | Stop recording and save the replay file |
| `emu_replay_play` | yes (replaces the emulator state) | Play a replay back from its start snapshot, stop at the first differing hash |
| `emu_replay_play_stop` | no | Stop a running playback |
| `emu_replay_rewind` | yes (replaces the emulator state) | Go back to a checkpoint: while recording drop the inputs after it and record on, while playing jump back |
| `emu_replay_seek` | yes (replaces the emulator state) | Jump the playback to a frame (back via a checkpoint or from the start, forward at MAX SPEED) |
| `emu_replay_goto` | yes (replaces the emulator state) | Machine state at any moment of the playing replay (time, frame and tick, or a cputrack trace record), output like `emu_state_observe` |
| `emu_replay_status` | no | Recording / playback status and the result of the last one |
| `emu_screenshot_save_to_file` | yes (creates / overwrites a file) | PNG of the screen to a file: area (full / canvas), scale (framebuffer / native dots / window 1-4), crop |
| `emu_screenshot_series_start` | yes (creates files) | Frame series: a PNG of every changed emulated frame (area, scale, crop, only_changed, frames, max_files) |
| `emu_screenshot_series_stop` | no | Stop the frame series, by default wait until all frames are written |
| `emu_screenshot_series_status` | no | Frame series status (frames seen, files, unchanged, folder, stop reason) |
| `emu_profiler_start` | no | Start CPU profiler (hot-path overhead) |
| `emu_profiler_stop` | no | Stop profiler (data preserved) |
| `emu_profiler_reset` | no | Clear profiler aggregator |
| `emu_profiler_export` | no | Export profile to CSV or JSON file |
| `emu_profiler_get` | no | Inline JSON with entries + global stats |
| `emu_profiler_pc_start` | no | Start the per-PC profiler (executions, T-states and WAIT per instruction address) |
| `emu_profiler_pc_stop` | no | Stop the per-PC profiler (data preserved) |
| `emu_profiler_pc_reset` | no | Clear the per-PC profiler data |
| `emu_profiler_pc_get` | no | Busiest instruction addresses with the nearest symbol (sort, limit, address range) |
| `emu_profiler_pc_export` | no | All executed addresses to a CSV file |
| `emu_profiler_pc_bitmap` | no | Executed addresses as an 8 KiB base64 bitmap (code coverage) |
| `emu_coverage_start` | no | Start code coverage: level `branch` (by PC) or `detail` (by physical cell and code version) |
| `emu_coverage_stop` | no | Stop code coverage (data stay readable) |
| `emu_coverage_reset` | no | Clear the code coverage data |
| `emu_coverage_status` | no | Code coverage state, completeness and branch summary |
| `emu_coverage_mark` | no | New coverage epoch (what runs for the first time after the mark) |
| `emu_coverage_read` | no | Page of coverage rows: branch states, counters, code versions (cursor, filter, character budget) |
| `emu_coverage_export` | no | All coverage rows into a JSON or CSV file |
| `emu_audio_capture_start` | yes (creates files) | Audio capture into a folder and/or memory: channel level changes, PSG writes, reference.wav and sdl.wav (frames, file selection, memory, pcm) |
| `emu_audio_capture_stop` | no | Stop the audio capture and finish its files |
| `emu_audio_capture_status` | no | Audio capture status (frames, events, PSG writes, samples, SDL blocks, folder, stop reason, memory) |
| `emu_audio_capture_summary` | no | Summary of the capture memory: silent / sound spans, channels, PSG notes, speaker tone or PCM, reference vs audio output |
| `emu_audio_capture_pcm` | no | PCM window from the capture memory (WAV or raw base64, resampled preview or exact 44100 Hz) |
| `emu_media_load_mzf` | no | CMT-hack instant load of MZF (header + body) into RAM at LOAD_ADDR (path or bytes_b64) |
| `emu_media_run_mzf` | **YES** | Reset + start MZF in the `--run-mzf` state (default); `bootstrap=false` = load + ROM disconnect + JP STRT without reset |
| `emu_media_load_binary` | **YES** | Raw bytes from file to Z80 memory, **destructive** |
| `emu_media_insert` | **YES** | Insert image into slot (auto-eject when already mounted) |
| `emu_media_eject` | no | Eject image from slot |
| `emu_media_state` | no | Snapshot of all 11 slots (cmt/fdc0_fd0..3/fdc1_fd0..3/qd/ide8) |
| `emu_cmt_open` | **YES** | Open a tape file (.mzf, .mzt, .wav, ...) into the real tape drive, optionally play |
| `emu_cmt_eject` | **YES** | Eject the tape from the real tape drive |
| `emu_cmt_play` | **YES** | Start tape playback (transport PLAY) |
| `emu_cmt_play_paused` | **YES** | Start tape playback paused (waits until resumed) |
| `emu_cmt_pause` | **YES** | Pause / resume the tape transport |
| `emu_cmt_stop` | **YES** | Stop the tape transport (PLAY or RECORD -> STOP) |
| `emu_cmt_record` | **YES** | Record the tape output to a WAV file |
| `emu_cmt_hack_set` | **YES** | Enable / disable cmthack (instant tape load ROM patch) |
| `emu_cmt_set_cpu_boost` | **YES** | Enable / disable CPU boost while the tape runs |
| `emu_cmt_set_mzfsize_check` | **YES** | Enable / disable the MZF size check on tape load |
| `emu_cmt_set_polarity` | **YES** | Tape signal polarity (rear DIP switch) |
| `emu_cmt_set_speed` | **YES** | Default playback speed of the virtual cassette (ratio or custom pulses) |
| `emu_cmt_tape_seek` | **YES** | Seek a multi-block tape to a block |
| `emu_cmt_tape_set_block_speed` | **YES** | Playback speed of a single tape block |
| `emu_set_speed` | no | Emulation speed: 100 %, custom percent or MAX SPEED |
| `emu_speed_step` | no | Change the custom emulation speed by a relative percentage |
| `emu_settings_set` | **YES** | Live INI write (whitelist); **mutates global state** |
| `emu_settings_get` | no | Read INI key + type + live value (open read, no whitelist) |
| `emu_platform_set` | **YES** | Platform switch attempt - returns error (compile-time MZARCH) |
| `emu_periph_attach` | **YES** | Attaches a peripheral at runtime (like the Devices menu) |
| `emu_periph_detach` | **YES** | Detaches a peripheral at runtime (like the Devices menu) |
| `emu_get_devices` | no | Expansion devices of the MZ-700 core: id, options, ports, MMIO, regions, outputs |
| `emu_device_screenshot` | no | Picture of an expansion device's own monitor (secondary output) as PNG or palette indices |
| `emu_stop` | **YES** | Hot-swap stop (pipe-only); terminates emu, Python wrapper keeps running |
| `emu_start` | **YES** | Hot-swap start (pipe-only); spawns fresh mz800emu.exe child |
| `emu_input_send_key` | **YES** | Press key + hold N frames + release |
| `emu_input_send_keys` | **YES** | Sequence of keys (ascii or key_names encoding) |
| `emu_input_press_key` | **YES** | Persistent press with no auto-release (= SHIFT hold) |
| `emu_input_release_key` | **YES** | Release specific key or all keys |
| `emu_input_send_joystick` | **YES** | 8-bit joystick mask + hold + release |
| `emu_input_send_keys_with_delays` | **YES** | Timing-controlled event sequence |
| `emu_get_reg` | no | Read a single Z80 register (cheaper than `emu_get_registers`) |
| `emu_force_pause` | no | Force-pause the emulator (bypass standard pause path) |
| `emu_set_user_cycle_origin` | **YES** | Reset the User cycle counter origin (mutates debug state) |
| `emu_get_im2_vector` | no | Tool variant of the IM2 ISR vector snapshot (PIO-Z80 platforms) |
| `emu_get_raster_pos` | no | Current GDG raster position + Z80 cycle counters |
| `emu_get_cpu_flags` | no | Auxiliary CPU state (IFF/IM/HALT/INT/NMI/EI delay/Q/cycle counters/I/R) |
| `emu_set_cpu_flags` | **YES** | Selective write of IFF1/IFF2/IM/I/R (update_mask from field presence), optionally only if the fields still hold `expected`; returns `previous` |
| `emu_get_last_instr` | no | Most recently completed Z80 instruction from the history ring (addr + bytes + length) |
| `emu_get_cpu_panel_batch` | no | Atomic batch: regs + flags + raster + IM2 + last_instr in one round-trip |
| `emu_state_observe` | no | Registers, flags, run number, time and up to 16 memory windows (CPU or region) from one moment, SHA-256 per window |
| `emu_machine_hash` | no | Content fingerprint of the whole machine (SHA-256 per part: CPU, memory, mapping, ROM, chips, media, inputs, tape, RAM disk) and one total |
| `emu_debugger_activate` | **YES** | Programmatically activate the debugger (side effect: cpuhist + mhmap recording in WITH_WINDOW preset) |
| `emu_debugger_deactivate` | **YES** | Programmatically deactivate the debugger (recording off) |
| `emu_is_debugger_active` | no | Read-only probe for `TEST_DEBUGGER_ACTIVE` |
| `emu_set_pioz80_interrupt_vector` | **YES** | Override the IM2 vector in PIO-Z80 port A/B (MZ-800/MZ-1500; MZ-700 returns `available:false`) |
| `emu_bp_create_with_init` | **YES** | Atomic create + init of breakpoint fields (= `breakpoints_add_auto` + selective UPDATE in one safepoint) |
| `emu_bp_set_parent` | **YES** | Quick reparent a breakpoint into a group (drag-drop semantics) |
| `emu_bp_update` | **YES** | Selectively update fields of an existing breakpoint via a `fields[]` array |
| `emu_bpgrp_add` | **YES** | Add a new breakpoint group (name + parent) |
| `emu_bpgrp_remove` | **YES** | Remove a breakpoint group (cascading delete handled by the backend) |
| `emu_bpgrp_update` | **YES** | Selectively update group fields (enabled/name/colors/parent) |
| `emu_stack_history_enable` | **YES** | Enable/disable SP history recording (disable flushes the ring buffer) |
| `emu_stack_history_reset` | **YES** | Flush the SP history ring buffer (recording flag is preserved) |
| `emu_stack_regions_add` | **YES** | Add a monitored stack region (name + base + limit) |
| `emu_stack_regions_edit` | **YES** | Edit an existing region (on success the watermark + counters are reset) |
| `emu_stack_regions_remove` | **YES** | Remove the region at a given index |
| `emu_stack_regions_reset_watermark` | **YES** | Reset watermark + push/pop counters of a single region |
| `emu_eventlog_start` | no | Start Event Viewer recording |
| `emu_eventlog_stop` | no | Stop recording (ring contents preserved) |
| `emu_eventlog_clear` | **YES** | Clear the event ring buffer |
| `emu_eventlog_set_capacity` | **YES** | Resize ring (clamp to min/max) - discards current content |
| `emu_eventlog_set_mask` | **YES** | 64-bit categories filter bitmask |
| `emu_eventlog_get_event` | no | Read event[idx] - pxclk/screen/category/pc/payload |
| `emu_regions_list` | no | Enumerate physical memory regions (ROM, VRAM, CG-ROM, MemExt banks, ...) bypassing Z80 banking |
| `emu_region_read` | no | Raw no-side-effect read from a specific region (= equivalent of the GUI Memory browser for headless debug) |
| `emu_region_write` | **YES** | Raw write into a physical memory region (bypass Z80 banking), max 65536 bytes |
| `emu_file_load_to_region` | **YES** | Load a file (or a part of it) into a memory region, max 65536 bytes |
| `emu_file_save_from_region` | yes (creates / overwrites a file) | Save a part of a memory region to a file, max 65536 bytes |
| `emu_freeze_add` | **YES** | Freeze a byte / bytes of a region (written back once per frame) |
| `emu_freeze_remove` | no | Unfreeze bytes (a range or all) |
| `emu_freeze_list` | no | Frozen bytes (also those frozen in the GUI) |
| `emu_memsearch_start` | no | Start a variable search (cheat search) over a space, first sample |
| `emu_memsearch_filter` | no | Narrow the candidates (eq, dec_by, changed, ... vs value / previous / first) |
| `emu_memsearch_sample` | no | New sample without filtering |
| `emu_memsearch_undo` | no | Undo the last search step (candidates and reference samples) |
| `emu_memsearch_redo` | no | Redo an undone search step |
| `emu_memsearch_list` | no | Page of candidates with values, CPU address and write-breakpoint hint |
| `emu_memsearch_eliminate` | no | Remove chosen candidates by hand |
| `emu_memsearch_close` | no | Close a search session |

## Sensitive operations

Tools marked **sensitive** carry an explicit WARNING in their MCP
schema description about their destructive nature; the MCP client
(Claude Code, etc.) should ask the user for confirmation (Claude Code
handles permission per tool category by default).

Sensitive tools include:

- **Memory / I/O / interrupts** - `emu_mem_write`, `emu_mem_write_force`,
  `emu_freeze_add`,
  `emu_io_read`, `emu_io_write`, `emu_irq_inject`, `emu_nmi_inject`
- **Snapshot operations** - `emu_snapshot_save` (overwrites a file),
  `emu_snapshot_load` and `emu_snapshot_load_buffer` (replace emu
  state)
- **Replay** - `emu_replay_record_start` (overwrites a file),
  `emu_replay_play` (replaces emu state)
- **Media operations** - `emu_media_load_binary` (raw RAM write),
  `emu_media_insert` (auto-eject overwrites mounted image)
- **Configuration** - `emu_settings_set` (mutates global state),
  `emu_platform_set`, `emu_periph_attach`, `emu_periph_detach`
- **Hot-swap** - `emu_stop`, `emu_start`
- **HID input** - all `emu_input_*` tools (user simulation may produce
  unintended behavior)
- **BP advanced** - `emu_bp_create_with_init`,
  `emu_bp_set_parent`, `emu_bp_update`, `emu_bpgrp_add`,
  `emu_bpgrp_remove`, `emu_bpgrp_update` (mutate `g_breakpoints` /
  `g_bp_groups` debugger structures)
- **Stack analytics** - `emu_stack_history_enable`,
  `emu_stack_history_reset`, `emu_stack_regions_add`,
  `emu_stack_regions_edit`, `emu_stack_regions_remove`,
  `emu_stack_regions_reset_watermark` (mutate recording flag, ring
  buffer, region definitions + counters)

For `emu_mem_write` a **region check** applies - the target address
must lie in a RAM region (not ROM, CG-ROM, prohibited, unmapped). If
not, the tool fails with `MEM_WRITE region check failed`. For explicit
fault injection (= writes into regions outside RAM) use
`emu_mem_write_force`.

## Error return convention

On a backend failure every tool returns an object with an `error`
field (`{"error": "<description>"}`). Previously some tools returned a
bare empty response (`{}`) in such a case, so the client could not tell
that the call had failed. Now an error is always explicit - the client
can reliably test for the presence of `error` in the response. On
success the response carries the tool-specific data fields (see the
descriptions below); a successful response has no `error` field.

The `error` text also tells whether the emulator executed the command:

- `Emulator busy: command not executed ...` / `Emulator busy: command
  queue full ...` - the command was **not** executed (the emulator
  thread did not pick it up within 10 s, or the queue was full); the
  emulator state is unchanged and the call is safe to retry.
- `Emulator busy: command only partially executed ...` - a multi-step
  tool stopped after an earlier step ran; check the state before
  retrying.
- `Emulator busy: command still running ...` - the emulator thread
  picked the command up but did not finish it within about 20 s (10 min
  for file I/O tools such as trace_save or snapshot_save); the
  result is unknown (it may still complete later). Check the state
  before retrying.
- `Emulator is shutting down ...` - not executed, stop sending requests.
- `Precondition failed: ...` - a conditional write (`expected_sha256`,
  `expected`, `expected_state_rev`) was checked by the emulator thread
  and **nothing was written**: the memory, the register or the machine
  state differs from what you read. The current value is in the text;
  read again before deciding.

**State revision.** `emu_status` / `get_state`, `emu_state_observe`, the
responses of tools that change the machine state and the events
`paused`, `resumed` and `step_done` carry `state_rev`: in pause an equal
value means nothing changed the machine state (MCP, GUI, replay,
devices). Tools that change the machine state accept
`expected_state_rev` (do it only if the revision still equals the value,
in pause; otherwise `Precondition failed: state changed (...)` or
`Precondition failed: emulator running (...)`), `emu_state_observe`
accepts `if_state_rev_differs` (reply `not_modified` without reading).
The event `state_changed` (`{state_rev, sources}`) reports changes in
pause that did not come from an MCP client (GUI, devices, replay),
merged per pass of the pause loop. `emu_machine_hash` gives a content
fingerprint of the whole machine by parts ("same state as before?").
Details: `emulator://docs/state_revision`.
- any other text (e.g. `bp_remove failed (unknown id?)`) - the command
  was executed and failed (or the parameters were rejected); retrying
  unchanged does not help.

The `Emulator busy: ...` / `Emulator is shutting down ...` texts end
with the tool's original message in square brackets (kept for
compatibility), e.g. `... safe to retry [bp_remove failed (unknown id?)]`;
match on the prefix. Once the emulator thread picks a command up, the
call waits for it to finish and returns its real result (at most about
20 s, or 10 min for file I/O tools, then `command still running`). Details for AI
clients: `emulator://docs/error_handling`.

## Per-tool description

### `emu_status`

Returns emulator state: whether it is running or paused, how many
frames have elapsed, whether the transport is connected, the run number
`run_seq` and the state revision `state_rev` (from the same moment as
`running`).
Non-destructive, suitable as the first call after connecting. When
the emulator ended unexpectedly, the result is
`{"running": false, "connected": false, "last_exit": "..."}` (see
[Python wrapper](python-wrapper.md), section *Emulator exit and
automatic restart*).

### `emu_ping`

Returns `pong`. A quick liveness check (= "is the bridge still up?")
that does not depend on emu state.

### `emu_pause`

Pauses CPU emulation. The tool returns once the pause is confirmed
(= the next call reads consistent state). Argument:

- `if_run_seq` (int, optional) - pause only while the run with this
  number runs (see `emu_wait_stop`); a run that someone else started
  meanwhile (the user in the GUI) is not stopped

Returns `{"paused": <bool>, "was_running": <bool>, "paused_now": <bool>,
"run_seq": <int>}`, plus `skipped` (`"run_seq_mismatch"` or
`"not_running"`) when `if_run_seq` prevented the pause. Pausing an
already paused emulator is not a new stop: no `paused` event.

### `emu_resume`

Resumes emulation without blocking and returns at once with
`{"running": true, "run_seq": <int>, "already_running": <bool>}`.
Optional deadline in emulated time, at most one of:

- `deadline_frames` (int, 1..1000000) - stop exactly at the N-th frame
  boundary
- `deadline_ms` (int, 1..86400000) - stop after N ms of emulated time
- `deadline_ticks` (int) - stop after N GDG clock ticks

The deadline is deterministic (emulated time, not wall clock); `ms` and
`ticks` stop at the first emulation event point at or after the
deadline, less than one frame late. The stop has the reason
`"deadline"`, the reply carries `deadline_ticks`. A deadline holds until
the next pause and survives a machine reset during the run. Wait for
the stop with `emu_wait_stop`.

### `emu_wait_stop`

Waits until a run stops and returns why and where. A run is the time
between leaving a pause and the next pause; every run has a number
`run_seq` (returned by the tools that start a run and by `emu_pause`,
`emu_get_state`). Arguments:

- `run_seq` (int, optional) - the run to wait for; without it the
  current run (or the last one when paused)
- `timeout_ms` (int, 0..25000, default 5000)

Returns `{"done": true, "run_seq", "record": {...}}` or, after the
timeout, `{"done": false, "running": true, ...}` - call again. The
record: `reason` (`run_to`, `breakpoint`, `deadline`, `frames`, `step`,
`manual`, `exit`, `bp_action_saturation`, `event_trigger`, `replay`,
`replay_end`, `replay_desync`), `pc`,
`bp_id`, `started_by` / `start_cmd` (`mcp` = an agent, `user` = the
GUI), `stopped_by` (for `manual`), `start_ticks` / `stop_ticks` (GDG
ticks since reset), `elapsed_ticks`, `frame`, `step`; for `exit` also
`exit_code`, `exit_reason`, `message`. `history_lost: true` = the run
stopped, but its record is no longer among the 32 newest. The event
`paused` carries the same fields. Recipe in `emulator://docs/run_control`.

### `emu_get_stop_history`

Newest stop records, oldest first. Argument `limit` (int, 1..32,
default 32). Returns `{"records": [...], "count", "current_run_seq",
"running"}`; same record fields as `emu_wait_stop`. Also available as
the resource `emulator://run/history`.

### `emu_run`

Resumes emulation for a specific number of video frames (1-1000)
and **blocks** until N frames have elapsed. After N frames the
emu pauses automatically. Argument:

- `frames` (int, 1..1000) - how many frames to advance

If `frames` exceeds the limit, the tool returns an error.

**Blocking semantics:** The tool returns only after N frames have
completed, not immediately after unpause. The emulator stops ITSELF
deterministically exactly at the N-th frame boundary (emu-side
frame-bounded stop), not via an asynchronous pause from the dispatch
thread. Running the same number of frames from the same state is therefore
bit-reproducible - important when tracing timing-sensitive programs. For
PAL (= 50 Hz) `emu_run(frames=100)` blocks for ~2 s; for NTSC (= 60 Hz)
~1.67 s.

Response contains:

- `running: false` - emu is paused after completion
- `actual_frames` (int) - actual frames that elapsed (= N, fewer if stopped
  by a breakpoint or a safety timeout)
- `requested_frames` (int) - what was requested
- `stopped_by` (string) - why it stopped: `"frames"` (ran the full count),
  `"breakpoint"` (a breakpoint paused it), `"manual"` (manual/UI pause),
  `"run_to"` (a temporary breakpoint: run to cursor, step over),
  `"exit"` (an exit condition: breakpoint action `exit`, `--exit-after-frames`,
  `--timeout-*` - the emulator only paused, see
  [Headless mode](headless-mode.md#unattended-runs)),
  `"bp_action_saturation"` (the byte limit of breakpoint actions,
  `[BREAKPOINTS] fwd_byte_limit_mb`, paused it),
  `"timeout"` (safety timeout), `"unknown"` (other)
- `complete` (bool) - true only if the full frame count was reached
  (= `stopped_by` is `"frames"`)

If `frames == 0` (or missing), the tool just submits unpause and
returns `running: true` asynchronously (= legacy fire-and-forget,
kept for backward compatibility with older clients).

Safety timeout: 2x expected wallclock + 100 ms. For N=1000 frames
maximum blocking is ~40 s; on timeout returns `complete=false`
with `actual_frames < requested`.

### `emu_reset`

Performs a power-on reset of CPU and all peripherals. RAM state is
reset to default contents, registers are zeroed, PC is set to the
ROM entry point.

### `emu_get_registers`

Returns state of 14 Z80 registers: `AF`, `BC`, `DE`, `HL`, `IX`, `IY`,
`SP`, `PC`, `AF'`, `BC'`, `DE'`, `HL'`, `I`, `R`. Values as unsigned
hex strings.

### `emu_set_register` (sensitive)

Overwrites a single 16-bit Z80 register. Arguments:

- `reg` (string) - register name, case-insensitive. Accepts `AF`,
  `BC`, `DE`, `HL`, `AF_` / `AF2`, `BC_` / `BC2`, `DE_` / `DE2`,
  `HL_` / `HL2`, `IX`, `IY`, `SP`, `PC`, `WZ`, `IR`.
- `value` (int, 0..65535) - new value. `IR` writes the complete I:R
  pair: I is the high byte, R the low byte including bit 7 (like
  LD I,A plus LD R,A).
- `expected` (int, 0..65535, optional) - conditional write
  (compare-and-swap): write only if the register still holds this
  value. Compared and written in one step in the emulator thread;
  otherwise `Precondition failed: register <reg> changed (expected=...,
  current=...)` and nothing is written.
- `expected_state_rev` (int, optional) - write only if the state
  revision still equals this value (in pause); see the state revision
  paragraph above.

Returns `{"reg", "value", "previous", "state_rev"}` - `previous` is the
value before the write, `state_rev` the revision after it.

**Destructive operation.** Writing to `PC` changes the execution
path; writing to `SP` invalidates the current call stack. Save a
snapshot before experimenting. Typical use case "jump to address" is
`reg="PC", value=<addr>`; verify with `emu_get_registers` afterward.

### `emu_mem_read`

Reads N bytes from Z80 memory starting at the given address. Arguments:

- `address` (int, 0..0xFFFF) - start address
- `length` (int, 1..65536) - number of bytes
- `want_data` (bool, optional, default true) - false = only the hash

Returns base64-encoded data (`data_b64`) and `sha256` (SHA-256 of the
bytes, 64 lowercase hex digits). With `want_data=false` only the hash:
compare it with an earlier read to see whether the memory changed
without downloading it. No region check - anything readable
by the CPU (ROM, RAM, peripheral mapped, ...) is returned. Banking
is respected (= you read what the CPU would see at that address).

### `emu_mem_write` (sensitive)

Writes bytes to a Z80 address. **Destructive**. Arguments:

- `addr` (int, 0..0xFFFF) - start address
- `data_hex` (hex string, e.g. `"DEADBEEF"` or `"DE AD BE EF"`) - bytes
  to write
- `expected_sha256` (string, optional) - conditional write: write only
  if the SHA-256 of exactly the bytes being overwritten still equals
  this value (the `sha256` of `emu_mem_read` or of an
  `emu_state_observe` window of the same range). Checked and written in
  one step in the emulator thread; otherwise `Precondition failed:
  memory changed (...)` and nothing is written.
- `want_hashes` (bool, optional) - return `before_sha256` and
  `after_sha256` also without a condition
- `expected_state_rev` (int, optional) - write only if the state
  revision still equals this value (in pause); checked before
  `expected_sha256`

Enforces a region check (= RAM only). If the address is not in RAM,
the tool fails with `MEM_WRITE region check failed` (the region check
comes before the content condition). Returns `{"addr", "length",
"state_rev"}`, with a
condition or `want_hashes` also `before_sha256` and `after_sha256`.
The address goes through the current memory map; for a write that must
hit one physical cell use `emu_region_write`. Recipe:
`emulator://docs/conditional_writes`.

### `emu_state_observe`

Reads registers, flags, run number (`run_seq`), emulated time and up to
16 memory windows in **one pass of the emulator thread**, so everything
comes from the same moment - also while the emulator runs (then the
moment is the start of a frame and the call waits up to one frame).
Two separate calls (`emu_get_cpu_panel_batch` + `emu_mem_read`) always
see two different frames while running. Arguments:

- `windows` (list, 0..16 items, at most 65536 bytes in total) - each item
  is a CPU range `{"addr": <int>, "len": <int>}` (current memory map) or
  a physical region `{"region": {"kind": "<kind>", "sub_id": <int>},
  "offset": <int>, "length": <int>}` (kinds from `emu_regions_list`, no
  region id, so the window stays valid after a hardware change);
  optional `name` (echoed back) and `want_data`
- `include` (list, optional) - `raster`, `im2`, `last_instr`,
  `memory_map`
- `hashes` (list, optional) - SHA-256 of state parts `cpu`, `ram`,
  `vram`, `memext`, `pcg` (same definition as the replay hashes)
- `want_data` (bool, default true) - false = only `sha256` per window
- `if_state_rev_differs` (int, optional) - `state_rev` of your previous
  read; in pause with an equal revision nothing is read and the reply is
  only `{"not_modified": true, "state_rev", "run_seq", "running": false,
  "stable": true}`

Returns `run_seq`, `state_rev`, `running`, `stable` (true only in pause), `drain`
(`frame` / `paused`), `time` (`frame`, `frame_ticks`, `ticks` = GDG ticks since the emulator started, `total_cycles`),
`regs`, `flags`, `windows` (echo, `sha256` - the same value as
`emu_mem_read` / `emu_region_read` of the range - and `data_b64`),
`complete` and the requested sections. A region that is not connected
gives the window `error: "region_unavailable"` and `complete: false`; the
call still succeeds. Invalid windows (too many, too long, past the end of
the address space or region, unknown kind) are `Invalid parameters`.
Reads have no side effects. Details: `emulator://docs/state_observe`.

### `emu_machine_hash`

Content fingerprint of the whole machine, computed by the emulator thread
in one pass (in pause right away, while running at a frame boundary).
Arguments:

- `parts` (list, optional) - part names `cpu`, `ram`, `memext`, `vram`,
  `pcg`, `periph`, `memmap`, `rom`, `media`, `inputs`, `ramdisk`, `cmt`;
  default all
- `include_media` (bool, default false) - also hash the content of disk,
  Quick Disk, IDE8 and tape images, MemExt FLASH and RAM disks

Returns `sha256` (over the requested parts), `parts` (`{name: hex}`,
`null` = the part does not exist in this build or configuration),
`covered`, `not_covered` (areas never covered: screen, sound, plotter,
...), `include_media`, `state_rev`, `run_seq`, `running`, `stable` and
`elapsed_us`. Compare two fingerprints taken in pause with the same
arguments. An unknown part name is `Invalid parameters`. Reads have no
side effects. Details: `emulator://docs/state_revision`.

### `emu_bp_add`

Adds a breakpoint. By default an execution BP (= pause when PC =
address), but it also supports typed (memory / I/O) BPs and a
condition. Arguments:

- `address` (int, 0..0xFFFF) - BP address (PC for exec; watched
  address / port for typed)
- `type` (string, optional, default `"exec"`) - BP type: `"exec"`
  (execution), `"memr"` (memory read), `"memw"` (memory write),
  `"ior"` (I/O IN), `"iow"` (I/O OUT)
- `condition` (string, optional) - condition expression (bp_expr DSL,
  e.g. `A == 0x42`). Empty = unconditional BP.

A plain exec BP without a condition takes the lightweight path. A typed
BP or a BP with a condition is transparently created via
`emu_bp_create_with_init` (= the single typed-breakpoint code path) -
there is no need to call create manually. Returns the breakpoint handle
(ID).

### `emu_bp_list`

Returns the list of current breakpoints. Return:
`{"count": <int>, "breakpoints": [...]}`, where each record carries:

- `id` (int) - breakpoint handle
- `addr` (int) - address
- `addr_end` (int) - upper bound of the address range (inclusive);
  used only when `addr_match_mode` is `RANGE`
- `addr_match_mode` (string) - `SINGLE` (only `addr`), `RANGE`
  (`addr`..`addr_end`) or `MASK` (`(x & addr_mask) == (addr & addr_mask)`);
  applies to `PC_EXEC` / `MEM_R` / `MEM_W`
- `addr_mask` (int) - AND mask; used only when `addr_match_mode` is `MASK`
- `enabled` (bool)
- `type` (string) - canonical UPPER_SNAKE type (`PC_EXEC` / `MEM_R` /
  `MEM_W` / `IORQ_R` / `IORQ_W` / ...)
- `zone` (string) - memory zone (`CPU_VIEW` / `RAM` / ...)
- `bank_id` (int) - bank index (for the `MMEXT_BANK` zone)
- `bank_id_end` (int) - upper bound of the bank range (inclusive); used
  only when `bank_match_mode` is `RANGE`
- `bank_match_mode` (string) - `SINGLE` / `RANGE` / `MASK` for `bank_id`
- `bank_id_mask` (int) - AND mask for `bank_id`; used only in `MASK` mode
- `hits` (int) - BP trigger counter
- `condition` (string or null if unconditional)

### `emu_bp_remove`

Removes a specific breakpoint by its ID. Arguments:

- `id` (int) - ID obtained from `emu_bp_add` or `emu_bp_list`

Returns `{"id": <int>, "removed": true}` on success. If the ID
does not exist, the tool returns an error.

### `emu_bp_clear`

Removes all current breakpoints at once. No arguments. The
implementation is two-phase (BP_LIST + N x BP_REMOVE), so it is
not fully atomic - a GUI could add a BP between calls and that
one would not be removed (best-effort). Returns
`{"count": <int>, "cleared": true}` with the actual removed count.

### `emu_bp_enable`

Toggles the BP enabled flag without removing the entry. Useful
for temporarily silencing a BP you want to reactivate later
(no need to remember its address). Arguments:

- `id` (int) - breakpoint ID
- `enabled` (bool) - true = active, false = kept in the list
  but inactive

### `emu_step_into`

Executes exactly one Z80 instruction. For CALL/RST the next
instruction is at the destination address (= steps INTO the
subroutine, unlike `emu_step_over`). Requires paused state: the tool
returns after the step executed (`completed: true`). On a running
emulator nothing happens and the emulator keeps running
(`completed: false`, `running: true`). Returns `{"stepped": true,
"completed", "running", "run_seq"}`; `stepped` is kept for
compatibility and is always true.

### `emu_step_over`

Like `emu_step_into` for ordinary instructions, but for
CALL/RST/DJNZ and block instructions a temporary breakpoint is
placed at addr+length and run-to executes (= subroutine becomes
one logical step). Requires paused state. An ordinary instruction
returns after the step (`completed: true`). Over CALL/RST/DJNZ/block
instructions the tool returns at once (`completed: false`,
`running: true`); when the run-to stops on its temporary breakpoint,
the `paused` event carries `reason: "run_to"` (see `emu_run_until_addr`),
wait with `emu_wait_stop(run_seq)`. On a running emulator the tool
only pauses it (`paused_only: true`).

### `emu_step_n`

Executes N instructions as a sequence of `emu_step_into`. Argument:

- `count` (int, 1..1000) - number of steps

If any step fails mid-loop (= BP hit, another reason for the
pause), the tool returns the partial counter with `partial=true`
in the response. Return value: `{"count": <int>, "requested":
<int>, "partial": <bool>}`. The tool returns after the last step
executed.

### `emu_run_until_addr`

Places a temporary BP at the target address and resumes
emulation. The emu pauses when PC reaches `addr` (or hits another
permanent BP first). Arguments:

- `addr` (int, 0..65535) - target PC address
- `max_cycles` (int, optional, default 10M) - informative only, echoed
  back, limits nothing
- `deadline_frames` / `deadline_ms` / `deadline_ticks` (int, optional,
  at most one) - stop the run in emulated time when `addr` is not
  reached (reason `"deadline"`), see `emu_resume`

The reply is `{"addr", "max_cycles", "running": true, "run_seq"}`
(plus `deadline_ticks`); wait for the stop with `emu_wait_stop(run_seq)`.

Requires paused state. On a running emulator the call returns the
error `Emulator is running: run_until_addr needs a paused emulator ...`
and the emulator keeps running unchanged (a freshly started emulator
runs, so call `emu_pause` first).

When PC reaches `addr`, the emulator emits the event `paused` with
`{"reason": "run_to", "pc": <addr>, "run_seq": ...}` (topic `paused`, see
`emu_event_subscribe`), so the client can wait with `emu_wait_stop` or
`emu_event_poll` instead of polling `emu_status`. If a permanent breakpoint stops the
emulation first, or sits on `addr` and stops there, the event is the
breakpoint's (`reason: "breakpoint"`), never both. A permanent
breakpoint on `addr` that does not stop (condition false, `skip_count`,
an action that continues) does not hide the target: the run still stops
at `addr` with `reason: "run_to"`.

### `emu_snapshot_save` (sensitive - overwrites a file)

Saves the full emulator state into a .mzs file on disk. Arguments:

- `path` (string) - filesystem path for the .mzs file
- `description` (string, optional) - human-readable label embedded
  in the snapshot metadata

Requires paused state. Response:
`{"path": <str>, "ok": true, "result_code": 0}`.

### `emu_snapshot_save_buffer`

Same as `emu_snapshot_save` but the resulting .mzs ZIP is returned
inline as a base64 string instead of being written to disk. Useful
when the AI client cannot or does not want to write to the emulator
host filesystem. Argument:

- `description` (string, optional)

Response: `{"bytes_b64": <str>, "size": <int>, "ok": true}`. The client
may keep the `bytes_b64` for later restore via
`emu_snapshot_load_buffer`.

### `emu_snapshot_load` (sensitive - replaces emu state)

Loads a .mzs file. After success the emu stays paused at the snapshot's
captured PC. Argument:

- `path` (string) - filesystem path to the .mzs file

Requires paused state (a freshly started emulator runs, so call
`emu_pause` first). An error names the reason, e.g.
`snapshot_load failed: Emulator is not paused (pause it first, e.g. emu_pause)`
or `snapshot_load failed: I/O error (result_code 1)`.

Response: `{"path": <str>, "ok": true, "result_code": 0}`.

### `emu_snapshot_load_buffer` (sensitive)

Loads a snapshot from an inline base64 buffer (typically produced by a
prior `emu_snapshot_save_buffer`). Argument:

- `bytes_b64` (string) - base64-encoded .mzs ZIP

Requires paused state, same as `emu_snapshot_load` (same error texts).

Response: `{"size": <int>, "ok": true, "result_code": 0}`.

### `emu_cooperation_hint_set`

A self-binding instruction the AI client voluntarily stores on the
server to constrain itself between requests (MCP is per-request
stateless). The server only persists the hint and broadcasts a UI
notification - **no hard enforcement** (= the AI may violate the hint,
the violation is recorded in the audit log).

Arguments:

- `mode` (string, required) - one of:
    - `free` - no constraint (clear)
    - `read_only` - AI will only call read-only tools/resources
    - `paused_only` - AI will only call tools while the emu is paused
- `until` (string, optional) - ISO 8601 timestamp or natural-language
  phrase ("next user message", "30 min"). Empty = open-ended.

Response: `{"mode": <str>, "until": <str>, "ok": true}`.

### `emu_symbol_add`

Adds a user-defined symbol (named address) into the symbol table. If a
symbol with the same name already exists, it is overwritten. Symbols
are visible in the disassembler view, breakpoint expressions, and
other AI introspection paths.

Arguments:

- `addr` (int, required) - 0..65535
- `name` (string, required) - identifier from [A-Za-z0-9_.], no whitespace
- `comment` (string, optional) - human-readable annotation
- `kind` (string, optional) - "LABEL" / "DATA" / "BOOKMARK" (default
  "LABEL"). Currently echo-only - storage always uses the user-label
  source kind.

Response: `{"added": true, "addr": <int>, "name": <str>, "kind": <str>}`.

### `emu_symbol_remove`

Removes a symbol by name or by address. Exactly one of `name` / `addr`
must be supplied.

Arguments:

- `name` (string, optional) - identifier
- `addr` (int, optional) - 0..65535 (`-1` means "not supplied")

Response: `{"removed": <bool>, "name": <str>}` or `{"removed": <bool>,
"addr": <int>}` depending on which key was supplied.

### `emu_symbol_lookup`

Read-only lookup of a single symbol. The `query` argument is
auto-detected:

- hex string (`0x4242`, `4242h`, `$4242`) -> lookup by address
- otherwise -> lookup by name (case-sensitive)

Returns the highest-priority symbol (user label > MAP > NOI >
SJASMPLUS).

Response: `{"found": true, "addr": <int>, "name": <str>, "comment":
<str>, "source": <int>}` or `{"found": false}`. `source` is a numeric
code (0=SJASMPLUS, 1=NOI, 2=MAP, 3=user label).

### `emu_symbol_list`

Lists symbols matching a name prefix. Useful for AI introspection of
the symbol table (e.g. all `ROM_*` routines).

Arguments:

- `prefix` (string, optional) - prefix filter (default "" = all)
- `limit` (int, optional) - max entries (1..1000, default 100)

Response: `{"count": <int>, "items": [{"addr", "name", "comment",
"source"}, ...]}`.

### `emu_bookmark_add`

Adds a named address bookmark to the debugger bookmark store. Bookmarks
are surfaced in the debugger UI and via the `emulator://bookmarks`
resource. The `input` is resolved to a 16-bit address dynamically: it
may be a hex literal (`$1234`, `0x1234`, `#1234`, `1234h`, `1234`) or a
symbol name from the symbol database. A symbol name lets the bookmark
follow the symbol if its address later changes. The bookmark is tagged
with the `mcp` owner.

Arguments:

- `input` (string, required) - hex literal or symbol name
- `comment` (string, optional) - human-readable note

Response: `{"id": <int>, "input": <str>, "comment": <str>, "addr":
<int|null>, "addr_resolved": <bool>, "owner": "mcp"}`. `addr` is `null`
when `input` cannot be resolved (e.g. unknown symbol) - the bookmark is
still stored and resolves lazily later.

### `emu_bookmark_remove`

Removes a bookmark by its ID. IDs are monotonic and never reused within
a session; obtain them from the `emulator://bookmarks` resource.
Removing a non-existent ID is not an error - it returns `removed=false`.

Arguments:

- `id` (int, required) - bookmark ID (>= 1)

Response: `{"id": <int>, "removed": <bool>}`.

### `emu_step_out`

Runs until the Z80 returns (RET) from the current subroutine.
Implementation: looks up the top frame in the shadow callstack, takes
its `return_addr`, installs a temporary breakpoint, and starts run-to.

Preconditions:

- Callstack tracking must be active (toggle in the Callstack debugger
  window or pass the `--callstack` CLI flag).
- The emulator must be paused before invocation.

Arguments:

- `max_cycles` (int, optional, default 10000000) - informative only,
  echoed back, limits nothing
- `deadline_frames` / `deadline_ms` / `deadline_ticks` (int, optional,
  at most one) - stop in emulated time when the function does not
  return (reason `"deadline"`), see `emu_resume`

Response: `{"return_addr": <int>, "max_cycles": <int>, "running": true,
"run_seq": <int>}` (plus `deadline_ticks`) or an error if callstack
tracking is inactive, the stack is empty, or the emulator is already
running. Reaching `return_addr` emits the `paused` event with
`reason: "run_to"` (see `emu_run_until_addr`); wait with
`emu_wait_stop(run_seq)`.

### `emu_run_until_raster`

Spins up the emulator and alternates step_into with raster snapshots
until the GDG beam reaches the target scanline (and optionally column).
Useful for debugging mid-frame raster effects (palette swaps, scroll
register writes, BCOL changes) timed against the video signal.

Arguments:

- `line` (int, 0..511) - target scanline. MZ-800 PAL frame has 312
  lines, NTSC has 262.
- `col` (int, optional, -1..2047, default -1) - target GDG column.
  -1 means "any column on the target line".
- `max_cycles` (int, optional, default 10000000) - safety cap on the
  polling loop's T-state budget.

Precision is bounded by Z80 instruction length (4-23 T-states ~
+/-10 GDG ticks ~ +/-5 visible pixels). Sufficient for mid-frame
effects.

The loop runs inside one request and cannot be cancelled; it ends
after at most 20 s of wall-clock time (`time_limited: true`), so that
the reply comes before the 30 s transport limit. The emulator then stays
paused where the loop got.

Response: `{"scanline": <int>, "column_pixel": <int>, "frame_number":
<int>, "total_cycles": <int>, "delta_cycles": <int>, "reached":
<bool>, "time_limited": <bool>}`. `reached=false` means the polling
loop stopped at the safety cap or at the time limit.

### `emu_run_until_tstate`

Runs until a time. The target is exactly one of:

- `target_ticks` (int >= 0, recommended) - time in GDG ticks since the
  emulator started (the `t` column of `emu_trace_read`, `time.ticks` of
  `emu_state_observe`, `stop_ticks`, `Cycle` in breakpoint conditions);
  it never wraps and a reset does not clear it. The (paused) emulator
  runs and stops itself at the start of the first instruction whose time
  is >= the target (stop reason `run_until_ticks`); an interrupt
  acknowledge in between belongs to the instruction before. A breakpoint
  may stop the run earlier. A target in the past or now returns at once
  with `reached: true`. Response: `{"ticks", "target_ticks",
  "delta_ticks", "exact" (an instruction started exactly at the target),
  "reached", "stopped_by", "time_limited", "run_seq"}`; after 20 s the
  emulation is paused (`time_limited: true`).
- `target_total_cycles` (int) - absolute target cycle counter
  (`total_cycles` in the raster snapshot, a 32-bit T-state count since
  CPU reset). **Must be greater than the current value** (the server
  returns an error for past targets). The server steps instruction by
  instruction.

More arguments:

- `max_cycles` (int, optional, default 10000000) - safety cap on delta
  T-states from the start of the polling loop (only with
  `target_total_cycles`).

Note: `total_cycles` is a uint32 that wraps roughly every 20 minutes
at 3.5 MHz. Wraparound across the boundary is not handled - typical
clients target deltas in the order of seconds.

Like `emu_run_until_raster` the loop ends after at most 20 s of
wall-clock time (`time_limited: true`).

Response: `{"total_cycles": <int>, "target": <int>, "delta_cycles":
<int>, "reached": <bool>, "time_limited": <bool>}`.

### `emu_run_until_event`

Runs a polling loop until a specific event fires. Supported kinds:

- `"frame_done"` - N video frames have elapsed. `params`: `{"count":
  <int>}` (default 1, range 1..10000).
- `"breakpoint_hit"` - starts the emulator and waits until a
  breakpoint pauses it (or the safety cap fires). `params` may
  contain `{"id": <int>}` (currently ignored - any pause satisfies
  the wait).

Arguments:

- `kind` (string) - see above
- `params` (object, optional) - kind-specific
- `max_cycles` (int, optional, default 10000000) - safety cap

The wait ends after at most 20 s of wall-clock time
(`time_limited: true`; for `breakpoint_hit` the emulator is then
paused). For long waits use `emu_resume` with a deadline and
`emu_wait_stop`.

Response: `{"kind": <str>, "reached": <bool>, "delta_cycles": <int>,
"time_limited": <bool>, ...}` plus kind-specific fields (`frames_done`
for `frame_done`, `paused` for `breakpoint_hit`).

### `emu_io_read` (sensitive)

Z80 IN port - **real read with full side effects** on the chip
(PSG status flag reset, FDC IDX strobe, GDG DMD strobe). Args:

- `port` (int, 0..65535)

Returns `{"port": <int>, "value": <0..255>}`.

### `emu_io_write` (sensitive)

Z80 OUT port - **destructive write** through the full Z80 OUT path
(PSG latch, FDC command, GDG mode register, PIO output bits, ...).
Args:

- `port` (int, 0..65535)
- `value` (int, 0..255)

Returns echo `{"port": <int>, "value": <int>}`. If the client
subscribes to topic `io_write`, every OUT from the Z80 hot path
(= even outside of this Tool) emits an event with
`{port, value, cycles}`.

### `emu_irq_inject` (sensitive)

Force maskable IRQ. Args:

- `source` (string, default `"manual"`) - audit label
- `vector` (int, -1 or 0..255) - IM2 vector; `-1` uses the chip's
  default vector callback

The actual acceptance depends on IFF1 (= when `DI`, the IRQ is
latched and accepted on the next `EI`). Returns
`{"injected": true, "source": "...", "vector_used": <int|null>}`.

### `emu_nmi_inject` (sensitive)

Force NMI. No args. After the current instruction completes the
CPU jumps to 0x0066, copies IFF1 to IFF2 and clears IFF1. NMI is
**non-maskable** - always accepted. Returns `{"injected": true}`.

### `emu_mem_write_force` (sensitive)

Write bytes to a Z80 address **without region check**. Input
validation is identical to `emu_mem_write`, but no filter on
ROM/CG-ROM regions is applied - bytes are written through the
banking-aware path, i.e. into RAM mapped under ROM, or remain
unchanged where ROM is currently mapped.

Args:

- `addr` (int, 0..65535)
- `data_hex` (string, even-length hex)
- `expected_sha256`, `want_hashes` (optional) - conditional write, as
  for `emu_mem_write`

Returns `{"addr": <int>, "length": <int>}` (plus `before_sha256` /
`after_sha256` with a condition or `want_hashes`). To actually patch ROM
(= bypass the ROM mapping), banking must be switched first via
`emu_io_write` on ports 0xE0..0xE4.

## Watch tools

Watch tools wrap the existing debugger Watch panel. Watches are
useful for long-running observation of a memory cell or expression -
instead of polling with `emu_mem_read` repeatedly, the client
registers a watch once and reads all current values with a single
`emu_watch_list` call.

Each watch row has one of three modes:

- `address` - literal Z80 address + type (default)
- `expr_scalar` - expression evaluated as int32, `type` controls
  display format only (no extra memory read)
- `expr_deref` - expression evaluated as uint16 address, then a read
  of size given by `type` is performed (like `address` but the
  address is dynamic)

Supported types (`type` argument): `u8` (default), `i8`, `u16le`,
`u16be`, `i16le`, `i16be`, `u32le`, `u32be`, `i32le`, `i32be`, `bit`,
`ascii`, `mzascii`, `bytes`.

### `emu_watch_add`

Add a watch row to the storage. Arguments:

- `name` (string, optional) - row name (empty = anonymous)
- `addr` (int, optional, 0..65535) - only for mode=address
- `mode` (string, optional) - "address" / "expr_scalar" / "expr_deref"
- `expr` (string, optional) - expression text (required for expr_*
  modes)
- `type` (string, optional, default "u8")

Returns `{"index": <int>, "name", "mode", "type", "addr"}`.

### `emu_watch_remove`

Remove a watch row by `name` or `index`. Returns
`{"removed": <bool>, "index": <int>}`. `removed=false` = not found.

### `emu_watch_list`

Return all currently registered watches with their current values:
`{"count": <int>, "items": [{"index", "name", "mode", "type", "addr",
"expr", "value"}, ...]}`. `value` is the formatted display string
according to the row's type + display format.

Cap: 256 rows (= a practical limit for an interactive watch UI).

### `emu_watch_eval`

Evaluate either an existing watch (via `name` or `index`) or an
ad-hoc expression (`expr`). When `expr` is supplied, the expression
is parsed and evaluated without being persisted.

Returns `{"value_str": "...", "value_int": <int>, "error": ... | null}`.

## Callstack tool

### `emu_callstack_get`

Return a snapshot of the shadow stack. The shadow stack pushes on
CALL / RST / IRQ accept / NMI and pops on RET / RETI / RETN,
independent of the Z80 SP. It allows tracking call depth even when
the Z80 stack contains transient pushes (typical pattern: CP/M BDOS
dispatch via PUSH+RET trampoline).

Arguments:

- `max_depth` (int, optional, 1..256, default 64) - cap the number of
  frames returned (the top N are emitted).

Returns `{"active", "count", "current_depth", "max_depth_reached",
"divergence_count", "overflow_count", "cycles_now", "frames": [...]}`.
Each frame holds `depth` (0 = top), `return_addr`, `call_site_addr`,
`target_addr`, `sp_at_entry`, `cycles_at_entry`, `kind` (string).

`kind` is one of: `call` / `rst` / `irq_im0` / `irq_im1` / `irq_im2`
/ `nmi` / `synthetic`.

When `active=false`, the Callstack subsystem is disabled - activation
goes through GUI Settings or the `--callstack` CLI flag. Without an
active subsystem, `count` is always 0.

## CDL tools

CDL = Code/Data Logger. Per-byte classification of memory accesses
(Read / Write / eXecute / Stack-write) built on the Memory Heatmap
subsystem. FCEUX-style CDL bitmap is `counter > 0`.

### `emu_cdl_start`

Start CDL recording. Triggers a CPU callback swap onto the slow path
with a logging callback. Existing counters are NOT zeroed - call
`emu_cdl_reset` if you need a clean baseline. Returns
`{"started": true, "mode": "always"}`.

### `emu_cdl_stop`

Stop CDL recording. Data is preserved until explicitly reset or
exported. Returns `{"stopped": true, "mode": "off"}`.

### `emu_cdl_reset`

Zero all CDL counters. Recording mode is unchanged - if it was active,
it continues from zero. Returns `{"reset": true}`.

### `emu_cdl_export`

Export CDL data to a set of files. The `path` argument points to the
target meta JSON file (e.g. `/tmp/cdl-export/snap1.json`). The
exporter creates the parent directory if missing and writes:

- `snap1.json` - meta with region descriptors
- `snap1_bus.cdl` - binary counter array for the CPU bus map
- `snap1_ram.cdl` - for main RAM
- `snap1_rom_*.cdl` - for ROM regions
- `snap1_vram*.cdl` - for VRAM (MZ-800 has multiple per-mode regions)
- `snap1_iorq_*.cdl` - for I/O ports

Cell layout = 16 B (`r`, `w`, `x`, `s` as uint32 LE counters).

Returns `{"path": "...", "region_count": <int>}`. `region_count` is
the number of region files emitted (platform-dependent).

## Trace tools

Runtime lifecycle control of the 5 binary trace-suite channels
(`cputrack`, `iorqlog`, `intlog`, `hwlog`, `marklog`). The interface mirrors the
CDL tools - it lets an AI client bracket the recording precisely around
the section of interest instead of saving only at emulator exit. For
the output format details see [Trace Suite](../debugger/Trace_Suite.md).

All 4 tools have a mandatory argument:

- `channel` (string, required) - one of `cputrack` / `iorqlog` /
  `intlog` / `hwlog` / `marklog` (`marklog` records the markers of the
  breakpoint action `mark`).

### `emu_trace_start`

Starts recording the channel (= ALWAYS mode). Argument `regs` (bool,
optional, cputrack only): `true` records the CPU registers after each
instruction (format 3, 32 B per record instead of 12), `false` format 2;
without it the configured default (`[TRACE_CPUTRACK] regs`) applies. While
cputrack already records in the other format the call fails (stop it
first). Returns `{"started": true}`.

### `emu_trace_stop`

Stops recording the channel; the segment is closed (flush + close).
Returns `{"stopped": true}`.

### `emu_trace_reset`

Writes the pending chunk and the manifest of the current channel segment
to disk; recording continues in the same segment and nothing recorded is
discarded. For cputrack it also resets the HALT/self-loop collapse: the
T-states of a collapsed run that are not written yet are dropped and the
next instruction starts a new record. A separate segment is
started by `emu_trace_save` with a new path, or by `emu_trace_stop`
and `emu_trace_start`. Returns `{"reset": true}`.

### `emu_trace_save`

Saves / redirects the channel segment. Argument:

- `path` (string, optional) - target path of the FOLLOWING segment.
  Without `path` the current segment is just closed and reopened on the
  existing dir/name (= "save now").

Returns `{"saved": true, "path": <str|null>}`.

### `emu_trace_status`

State of the channels for reading. Argument `channel` (optional; one of
the 5 trace channels or `eventlog` for the Event Viewer ring; empty =
all). Returns `{"channels": [...]}` with per channel: `segment` (0 =
nothing recorded yet), `live`, `truncated`, `first_seq`, `next_seq`,
`lost`, `record_size`, `format_version` (cputrack 2, with registers 3),
`chunks`, `bytes_on_disk`, `dir`, `name` and `has_time`; cputrack also
`has_regs` (segment with registers) and `initial_regs` (CPU state at the
start of the last segment, integers as in the manifest; null before the
first recording).

### `emu_trace_read`

One page of records of a channel (`cputrack`, `iorqlog`, `intlog`,
`hwlog`, `marklog`, `eventlog`, `ramlog` (RAM write log), `bplog` (output of BP actions `log` and `stop`), and from the audio capture memory
`audio_psg_writes`, `audio_levels`, `audio_frames`), also while recording runs - the data
still in the RAM buffer is readable too. After `emu_trace_stop` the
last segment stays readable until the next start. Arguments:

- `channel` (string, required).
- `cursor` (string) - `next_cursor` of the previous page; takes
  precedence over `start`.
- `start` (string, default `oldest`) - `oldest`, `newest` (last `limit`
  records) or `seq:N`.
- `limit` (int, default 200, 1-5000) - max rows; with `raw_b64` the
  record count (0 = as many as fit in 1 MiB).
- `format` (string) - `rows` (default, decoded) or `raw_b64`.
- `columns` (list) - subset of the channel columns (cputrack default:
  without `kind` and registers).
- `regs` (string, cputrack) - `after` (default) or `before`: which state
  the register columns and register filters show (`before` = the previous
  record; null where it is not known).
- `filter` (object) - `pc_from`/`pc_to` (cputrack), register names
  `af` .. `halted` with a value or `[lo, hi]` (cputrack with registers),
  `port` (<= 0xFF
  matches the low byte), `dir` (`in`/`out`), `type` (`iorq`,
  `mreq_mapped`, `iorq_unconnected`) (iorqlog), `chip`/`sub` (hwlog),
  `marker` (marklog id), `channels` (list of channel names `ctc0`,
  `psg0_0` .. `psg1_3`; audio_levels, audio_psg_writes), `reg` (`tone_lo`,
  `tone`, `volume`, `noise`; audio_psg_writes), `t_from`/`t_to` (channels
  with `t`); ramlog: `pc_from`/`pc_to` (writing instruction),
  `addr_from`/`addr_to` (CPU address), `space`, `src`, `all` (also writes
  that did not change the cell and writes under ROM - hidden by default);
  bplog: `pc_from`/`pc_to` (`insn_pc`), `bp` (breakpoint number, or its
  exact name as a string), `kind` (`log` or `stop`).
- `max_chars` (int, default 20000, 1000-45000) - character budget of
  the whole `rows` response.
- `max_scan` (int, default 2000000) - max records examined per call.

Columns (always in this order, `columns` in the response):

| Channel | Columns |
|---------|---------|
| `cputrack` | `seq`, `pc`, `bytes`, `asm`, `t_std`, `wait`, `t`, `sym` (default); on request `kind`, `af`, `bc`, `de`, `hl`, `ix`, `iy`, `sp`, `wz`, `i`, `r`, `iff1`, `iff2`, `im`, `halted` |
| `iorqlog` | `seq`, `t`, `frame`, `pos`, `type`, `dir`, `src`, `addr`, `value`, `pulse` |
| `intlog` | `seq`, `t`, `frame`, `pos`, `class`, `chip`, `pin`, `edge`, `state` |
| `hwlog` | `seq`, `t`, `frame`, `pos`, `chip`, `sub`, `payload` |
| `marklog` | `seq`, `t`, `frame`, `pos`, `id`, `name` |
| `eventlog` | `seq`, `t`, `frame`, `pos`, `cat`, `sub`, `pc`, `text` |
| `audio_psg_writes` | `seq`, `t`, `time_us`, `frame`, `psg`, `byte`, `ch`, `reg`, `value`, `hz`, `note` |
| `audio_levels` | `seq`, `t`, `time_us`, `frame`, `channel`, `level` |
| `audio_frames` | `seq`, `t`, `time_us`, `len_ticks`, `edges`, `on_pct`, `rms_level`, `max_level`, `ref_rms`, `ref_peak`, `sdl_rms`, `sdl_peak`, `sdl_blocks` |
| `ramlog` | `seq`, `t`, `frame`, `pc`, `sym`, `addr`, `space`, `offset`, `plane`, `old`, `new`, `src`, `flags`, `count`, `tin` |
| `bplog` | `seq`, `t`, `frame`, `insn_pc`, `sym`, `pc`, `bp`, `name`, `kind`, `text`, `flags` |

`t` is in GDG ticks since emulation start (cputrack: start of the
instruction, only with `has_time`); cputrack `asm` is decoded from the
recorded bytes, an interrupt acknowledge record shows `INT ACK`. cputrack
`kind` is `insn`, `loop`, `int` or `sync`; the register columns (16-bit
hex, `i` / `r` 8-bit hex, flags as numbers) need a segment recorded with
registers, otherwise the call fails. A `sync` row (`asm` `SYNC start`,
`SYNC pc_range`, `SYNC write`) carries the CPU state at the start of the
recording, on return into the PC range record filter and after a change
outside an instruction; it takes no time.
`audio_*`: `t` absolute, `time_us` since the capture start; `hz` / `note`
only for a new tone divider; the `audio_frames` arrays follow the
`channels` of `emu_audio_capture_status`, `ref_*` / `sdl_*` are [L, R]
int16 (null when not captured).

The response carries `segment`, `first_seq`, `next_seq`, `live`,
`truncated`, `lost`, `gap` (Event Viewer ring: `{from_seq, to_seq}`
when the cursor points before `first_seq`), `scanned`, `matched`,
`scan_exhausted` (the `max_scan` limit was hit - continue with the
cursor), `page_full`, `end_of_data` (the page reached `next_seq`),
`columns` + `rows` (or `record_size`, `format_version`, `from_seq`,
`count`, `data_b64`) and `next_cursor`. A cursor from an older segment
(the channel was restarted, the Event Viewer ring was cleared) returns
an error starting with `segment_replaced:`.

### `emu_bplog_clear`

The channel `bplog` holds the text of the breakpoint actions `log "..."`
and `stop "..."` (the same text the emulator prints as `[BP-LOG]` and
`[BP-STOP]`), so that an MCP client can read it with
`emu_trace_read(channel="bplog")`. It is always on, nothing needs to be
started; until an action has printed something it is empty (`next_seq`
0) and a cursor taken at that moment stays valid for later rows.
`t`, `frame`, `insn_pc` and `pc` are the values of `Cycle`, `Frame`,
`InsnPC` and `PC` in the breakpoint expression at the moment of the
action, `sym` is the symbol of `insn_pc`, `bp` the breakpoint number and
`name` its name, `kind` `log` or `stop`. Text longer than 239 bytes is
cut (`flags` `text_truncated`). The channel keeps the last 4096 rows
(older ones `lost`, `gap`); reset and snapshot load do not empty it.

`emu_bplog_clear` (no arguments) empties the channel and starts a new
segment (an old cursor returns `segment_replaced:`). It returns
`segment`, `allocated` (`false` = nothing written yet), `capacity`,
`first_seq`, `next_seq`.

## RAM write log tools

Record of the writes into memory ([RAM Write Log](../debugger/ram-write-log.md)):
who wrote a byte, which code writes into an area. Every record carries the
time `t` (GDG ticks at the start of the writing instruction, the same as
`t` of cputrack), `tin` (T-state counter of the instruction at the moment
of the write, `insn` only - an approximate position), `frame`, the address of the writing instruction
`pc`, the CPU address `addr`, the cell (`space` + `offset`: `ram`,
`vram`, `pcg`, `memext`, `rom` = write under ROM, dropped), `old` / `new`
and the source `src` (`insn`, `int_ack` / `nmi_ack` = return address
pushed when an interrupt is accepted, `pc` = that return address,
`debugger`, `poke`, `freeze`, `cmt_hack`, `load` = one summary record of
a loaded block). Reset and snapshot load empty the log. The recipe is in
the MCP document `emulator://docs/ramlog`.

### `emu_ramlog_start`

Starts the log; a running log is replaced. Arguments:

- `ranges` (list, optional, at most 16) - `{"from": 0x4000, "to": 0x40FF}`
  = CPU addresses at the moment of the write (`to` defaults to `from`),
  `{"space": "vram", "from": 0, "to": 0x1FFF}` = cells of a space (`ram`,
  `vram`, `pcg`, `memext`). Empty = all memory (slower).
- `mode` (string, default `ring`) - `ring` keeps the newest records,
  `fill` the first `capacity` records (the rest is counted in `dropped`).
- `capacity` (int, default 1048576, 16-16777216) - records, 32 B each.

Returns the status (see `emu_ramlog_status`).

### `emu_ramlog_stop`, `emu_ramlog_clear`

Stop the log (the records stay readable until the next start) / empty it
(new segment, a running log continues). Return the status.

### `emu_ramlog_status`

`{"active", "allocated", "segment", "mode", "capacity", "first_seq",
"next_seq", "total", "dropped", "overwritten", "memory_bytes",
"ranges"}`. `first_seq` > 0 = the ring overwrote its oldest records.

### `emu_ramlog_who_wrote`

Last writer and write history of a cell. Arguments: `addr` (CPU address
with the current mapping; the MZ-800 graphic VRAM gives its 4 planes) or
`space` + `offset`; `limit` (0-1000, default 20 records per cell);
`include_unchanged` (JSONL `all`, default false - also writes that did not
change the cell).

Returns `{"active", "segment", "mmio", "cells": [{"space", "offset",
"plane"?, "last": {"seq", "pc", "src", "in_ring"} | null, "history":
[{"seq", "t", "frame", "pc", "addr", "old", "new", "src", "tin"?, "count"?,
"unchanged"?, "suppressed"?}]}]}`, history newest first. `last` comes
from a map of last writers and is kept even after the ring overwrote the
record (`in_ring: false`). `mmio: true` = the address is a memory-mapped
port, no cell.

### `emu_ramlog_writers`

Instructions that wrote into a range, grouped by writing instruction and
sorted by the number of writes. Arguments: `from_addr`, `to_addr`
(default `from_addr`) - CPU addresses at the moment of the write, or
offsets in `space`; `limit` (1-5000, default 50); `include_unchanged`.
Returns `{"active", "segment", "total", "truncated", "writers": [{"pc",
"src", "count", "first_seq", "last_seq"}]}`.

The records themselves are read with `emu_trace_read(channel="ramlog")`.

## Video recording tools

Lossless recording of the picture and sound of the emulated computer (see
[Video recording](../video-recording.md)), controlled by an AI client:
start, play the game, markers, retake via snapshot, stop. The complete
workflow including the export to MP4 is in the MCP document
`emulator://docs/videorec_workflow`.

By default the recording uses emulation time (one emulated frame = one
video frame, 50 frames/s, 60 on MZ-700 NTSC and MZ-1500), so pauses between tool calls and MAX SPEED do
not show in the video. The real-time mode (`emu_videorec_timebase`)
records what was seen and heard in real time instead. Requests (start, pause, marker, stop) take effect at the end of
the next emulated frame; a stop while emulation is paused is executed
immediately. Available in every build (MZ-800, MZ-700 PAL/NTSC,
MZ-1500).

Every successful reply contains the recording status:

| Field | Meaning |
|-------|---------|
| `supported` | Recording is available (always `true`). |
| `state` | `idle`, `recording` or `paused` (recording pause). |
| `start_pending` | Start accepted, recording begins at the end of the next frame. |
| `frames`, `fps`, `duration_s` | Number of recorded frames, frame rate, length in seconds. |
| `segment`, `segment_open` | Current segment number (from 1) and whether it is open. |
| `bytes`, `parts` | Size and number of AVI files of the running or last recording. |
| `retake_mode` | `discard`, `seam` or `off` - what loading a snapshot does. |
| `path`, `sidecar` | First AVI file and the `.cuts.json` file. |
| `last_error` | Last error or `null`. |
| `last_event` | `null` or `{seq, kind, frame, path, text}`; `kind` = `started`, `saved`, `failed`, `retake`, `seam`. |
| `timebase`, `timebase_effective` | Requested and actual timebase (`emulated` / `realtime`); the actual one is `emulated` even with `realtime` requested when `realtime_speed = emulated_when_fast` and the speed is not 100 %. |
| `rt_activity` | Real-time mode: `off` (emulated time), `live` (live picture), `frozen` (emulation paused, frozen picture), `skipping` (nothing written). |

### `emu_videorec_start` (sensitive - creates / overwrites a file)

Arguments:

- `path` (string, optional) - target `.avi`; an existing file is
  overwritten. Without it the name `mz800_YYYYMMDD_HHMMSS.avi` is
  generated in the recordings output folder.
- `frames` (int, optional, default 0) - stop automatically after this
  many recorded frames (the emulator keeps running); 0 = no limit.

The file is created immediately, so an error (e.g. `Cannot create video
file: <path>`) is returned at once. The frame on which the start is
processed is not recorded. Returns the status plus `start_requested` and
`stop_after_frames`. Error `Video recording is already running` if a
recording is in progress.

### `emu_videorec_stop`

Arguments: `wait` (bool, default `true`), `timeout_s` (default 30). With
`wait` the tool waits until the recording files are complete and returns
`saved` (bool) and `recorded_frames`. Stopping a start that has not been
processed yet cancels it and deletes the file (`start_cancelled: true`).
Without a recording: `Video recording is not running`.

### `emu_videorec_pause`

Argument `paused` (bool, optional): `true` = pause recording, `false` =
resume, omitted = toggle. Emulation keeps running; resuming starts a new
segment (a cut with a transition in the export). Returns the status plus
`pause_requested` and `paused_target`.

### `emu_videorec_marker`

Argument `label` (string, optional; default `Marker at frame N`). The
marker gets the index of the next recorded frame and becomes a chapter on
export. Markers at or after the point of a snapshot used for a retake are
discarded together with the discarded part of the recording.

### `emu_videorec_status`

No arguments. Returns the status described above.

### `emu_videorec_timebase`

Argument `timebase` (string, required): `emulated` or `realtime`. Applies
to the running recording and to the next start (INI `[VIDEOREC]
timebase`). It therefore changes the persistent user setting - the same
as switching in the GUI (`Alt + U`, the recording control window, the
settings dialog): it is saved to the INI on exit (unless running with
`--no-save-ini`) and applies to the GUI user after a restart as well. An
agent that switched to `realtime` on a shared instance should switch
back to `emulated` when done. The recording switches to real time within one frame period (20 ms, 16.7 ms at 60 frames/s) and back to
emulated time at the end of the next emulated frame; every switch starts
a new segment. In real time, 50 times (60 times on MZ-700 NTSC and MZ-1500) per second of wall-clock time the
picture that was on the screen is written together with the sound that
went to the speaker (turbo plays fast, pauses follow `realtime_pause`).
Without an audio device (`--headless`, `--mcp-pipe`, or when the windowed
emulator cannot open an audio device) the sound is rendered by the same
path without being played. Returns the status plus
`timebase_requested`; an invalid value is a parameter error.

## Replay tools

Record the inputs of a run once and play them back exactly - in the same
or a new process, at any speed - to repeat a bug with other breakpoints,
trace or the RAM write log, or to check a run in CI. The full guide is
in [../replay.md](../replay.md) and in the MCP document
`emulator://docs/replay`.

A replay file (`.mzr`) holds a complete start snapshot (always with the
RAM disk), the contents of the mounted disks and the tape (up to 64 MB
together, larger images only as SHA-256), every change of the keyboard
matrix and joystick state at a frame boundary (the host joystick is read
once per frame), resets, autotype, tape control, debugger writes, and
SHA-256 hashes of the `cpu` (registers and hidden Z80 state), `ram`,
`memext`, `vram`, `pcg` and `periph` (CTC, PIO 8255, Z80 PIO, PSG, GDG,
VRAM controller, floppy and Quick Disk controllers) every N frames
together with the emulated time. Playback works on copies of the media,
compares the hashes and stops at the first frame that differs.

### `emu_replay_record_start` (sensitive - creates / overwrites a file)

Arguments: `path` (string, required), `hash_interval` (int 1..100000,
default 50 frames), `description` (string, optional),
`checkpoint_interval` (int 0..100000 frames, 0 = no checkpoints, default
from the `[REPLAY]` INI section: 250), `checkpoint_count` (int 1..1000,
default from the INI: 60). If the emulation runs, it pauses at the next
frame boundary, the recording starts and the emulation runs on; if it is
paused, it stays paused.

Every `checkpoint_interval` frames a checkpoint (a snapshot in memory,
tens of kB) is kept, the last `checkpoint_count` of them; see
`emu_replay_rewind`.

While recording, keyboard input from the GUI is taken at the next frame
boundary. Debugger writes (`emu_mem_write`, `emu_mem_write_force`,
region writes, `emu_set_register`, CPU flags, `emu_io_write`,
`emu_irq_inject`, `emu_nmi_inject`) and input tools are recorded while
running (taken at the frame boundary) and while paused **at** a frame
boundary (after `emu_run(frames=N)` or a `deadline`, or a pause sent
while running). In a pause **between** frame boundaries (after a step or
a breakpoint) they are refused with an explanation. Tape control
(`emu_cmt_play`, `emu_cmt_play_paused`, `emu_cmt_stop`, `emu_cmt_pause`,
`emu_cmt_eject`, `emu_cmt_tape_seek`, `emu_cmt_record`) is recorded the
same way. Tools that change the machine in a way the replay cannot
repeat are refused: snapshot load, media, inserting a tape
(`emu_cmt_open`), tape speed, pulses, polarity and boost, the CMT hack,
block speed, settings, platform, peripherals, memory map, `emu_io_read`
(side effects), `emu_freeze_add`. Starting is refused while autotype
runs, Freeze Bytes are active or the tape records. Breakpoint actions that
write (`poke`, `set`) are not recorded: the manifest gets a warning.

Returns the status (see `emu_replay_status`).

### `emu_replay_record_stop`

Stops the recording and writes the file. Stopped at a frame boundary,
the replay ends with a hash of the final state; stopped in a pause
between boundaries, it ends at the last boundary. Returns the status
with `outcome: "recorded"` and `outcome_message` (frames, events, file),
or an error (`no recording is running`, write error).

### `emu_replay_play` (sensitive - replaces the emulator state)

Arguments: `path` (string, required), `pause_after_load` (bool, default
false), `checkpoint_interval` and `checkpoint_count` (as for
`emu_replay_record_start`; checkpoints of the playback make a seek back
fast). Loads the start snapshot and plays the recorded inputs back;
`pause_after_load: true` stays paused after the load (set breakpoints,
then `emu_resume`). Live input (GUI keyboard, virtual keyboard, MCP
keys) is ignored. The end pauses the emulation with reason `replay_end`
(`outcome: "ended"`), the first differing hash with `replay_desync`
(`outcome: "desync"`, `outcome_frame`, `desync_parts`: `cpu`, `ram`,
`memext`, `vram`, `pcg`, `periph`, `time`). A write into the machine
during playback (debugger write, breakpoint `poke` / `set` action, Freeze
Bytes, reset, snapshot load, tape control) marks the playback
`deviated`: it goes on, but the hashes are no longer checked. The stored
media are mounted from copies (the original files and the INI do not
change; after the end the media from the INI return). Errors: unreadable
file, other platform or TV system, a replay with debugger events in a
build without the debugger, a joystick port connected only in one of the
two, the CMT hack set differently, an image that is not stored in the
replay and differs from the mounted one.

### `emu_replay_play_stop`

Stops a running playback (`outcome: "stopped"`). Error: `no playback is
running`.

### `emu_replay_rewind` (sensitive - replaces the emulator state)

Argument: `frames` (int >= 0, default 0). 0 = one checkpoint back (the
newest one before the current frame; call again to go further), N = the
newest checkpoint at least N frames back. While recording the machine
returns to the checkpoint, the inputs and hashes after it are dropped,
`rerecord_count` grows by one and the recording goes on from there; the
emulation stays paused at the frame boundary of the checkpoint. While
playing it is a seek back to that checkpoint (see `emu_replay_seek`).

Checkpoints do not hold the contents of the media. A checkpoint after
which a disk (FDC, Quick Disk, IDE8) or the tape was written, or the tape
was ejected, cannot be used: while recording the rewind is refused,
while playing the playback starts again from the beginning. No
checkpoint is taken while autotype runs or the tape records. Errors:
`checkpoints are off`, `no checkpoint at or before frame N (the oldest
kept is frame M)`, `a disk or the tape was written or ejected after frame
N; rewinding past a media change is not possible`, `nothing is recording
or playing`. Returns the status.

### `emu_replay_seek` (sensitive - replaces the emulator state)

Argument: `frame` (int >= 0, required; past the end limited to the end).
Forward: the playback runs on at MAX SPEED and stops at the frame
boundary of `frame` with pause reason `replay_seek` (event `paused`,
`emu_wait_stop`); the speed before the seek returns. Back: the newest
usable checkpoint at or before `frame` is loaded (otherwise the playback
starts again from the beginning with clean copies of the media) and the
rest is played at MAX SPEED. Hashes are checked all the way; breakpoints
stay active and may stop the seek early. The reply comes right after the
start: `seeking: true` = running to `seek_target`, `false` = already
standing at the frame. Error: `seek works only during playback`.
Returns the status.

### `emu_replay_goto` (sensitive - replaces the emulator state)

Machine state at any moment of the playing replay. The playback jumps to
the nearest usable checkpoint (or starts from the beginning), plays on at
MAX SPEED and the emulator stops itself at the start of the first
instruction whose time is >= the target (pause reason `replay_goto`);
then the tool reads the state like `emu_state_observe`. A target ahead in
the current frame is reached without a seek. Only while playing.
Recipes: `emulator://docs/replay`.

Target (exactly one):

- `t` (int) - time in GDG ticks,
- `frame` + `frame_ticks` (int) - absolute frame and tick in the frame
  (`time.frame`, `time.frame_ticks` of `emu_state_observe`),
- `trace` (object) - `{"seq": N, "at": "before" | "after", "channel":
  "cputrack"}`: the state before (default) or after a record of the
  current cputrack segment without a PC filter.

More arguments: `observe` (`emu_state_observe` parameters for the
reached state, default registers and time only), `breakpoints`
(`suppress` default = user breakpoints neither stop the jump nor run
their actions; `keep` = they stay active), `checkpoint` (bool, save a
checkpoint at the frame boundary where the approach starts; newer ones
stay), `timeout_ms` (0..25000, default 20000; 0 or a timeout = `done:
false`, then `emu_wait_stop` and `emu_state_observe`).

An interrupt acknowledge is not a stop point: a target right before it
returns the state after the acknowledge (`after_int_ack: true` for
`trace`). On a register trace `trace_check` compares the registers of the
record with the reached state.

Response: `{"done", "reached", "stopped_by", "target_t", "t", "exact",
"after_int_ack", "seek_from": "checkpoint" | "restart" | "none",
"seek_from_frame", "checkpoint_saved", "replay_frame", "run_seq",
"observe": {...}, "trace_check": {"match", "mismatch"}}`. Errors: no
playback (`goto works only during playback`), a time before the start or
after the end of the replay, a record outside the current trace segment,
a segment without time (PC filter).

### `emu_replay_status`

No arguments. Returns `{"mode": "off" | "recording" | "playing",
"frame", "frames" (length while playing), "events", "hashes",
"hash_interval", "warnings", "deviated", "deviation", "path", "outcome":
"none" | "recorded" | "ended" | "desync" | "stopped" | "error",
"outcome_frame", "outcome_message", "desync_parts": [...],
"rerecord_count", "seeking", "seek_target", "goto_active",
"goto_target_t", "checkpoints": {"interval", "capacity", "count",
"first_frame", "last_frame", "bytes"}}`. The
`outcome*` members describe the last finished recording or playback;
`first_frame` and `last_frame` are present only with `count` > 0.

## Screenshot tools

PNG pictures of the emulator screen written straight to a file on the
server host (no large base64 payload in the reply). The workflow with
recipes is in the MCP document `emulator://docs/screenshot_workflow`; the
parameters are also described in [headless-mode.md](headless-mode.md).

### `emu_frame_hash`

SHA-256 of the screen picture and of the video memory without
transferring them. No arguments. `image_sha256` hashes the picture from
the same source as a screenshot: the palette index of every pixel, row
by row (`width` x `height` bytes, INDEX8 & 0Fh), then the 16 palette
colours as R, G, B. `vram_sha256` hashes the video memory (MZ-800: all
four planes; MZ-700 / MZ-1500: character and attribute VRAM). The
picture also depends on GDG registers (palette, mode, border), the VRAM
hash does not. Paused in the middle of a frame, part of the picture is
still from the previous frame - the reply carries the beam position
(`beam`: `frame`, `scanline`, `column`) and `paused`. The screenshot
resources (`emulator://frame/screenshot.raw`, `emulator://frame/screenshot`)
return the same `image_sha256` (the PNG one for the full framebuffer,
whatever the area and scale). Returns `{"available", "image_sha256",
"vram_sha256", "source_screen_id", "image_source", "width", "height",
"beam", "paused"}`.

### `emu_screenshot_save_to_file` (sensitive - creates / overwrites a file)

Arguments:

- `path` (string, required) - PNG file to write.
- `format` (string, default `png`) - only `png` is supported.
- `area` (string, default `full`) - `full` = whole frame with the border,
  `canvas` = the picture only.
- `scale` (string, default `framebuffer`) - `framebuffer` = framebuffer
  pixels 1:1 (MZ-800 full frame 928x288); `native` = one image pixel per
  dot of the computer (MZ-800 picture 320x200, or 640x200 in the 640-dot
  modes); `"1"` to `"4"` = the aspect ratio of the emulator window, `"2"`
  = window at 100 % (MZ-800 full frame 928x576).
- `crop` (object, optional) - `{"x", "y", "width", "height"}` in output
  pixels, only with `framebuffer` or `native`.

Returns `{available, path, format, width, height, byte_size, area, scale}`
(+ `hires` with `native`), `{available: false, reason}` before the first
rendered frame. An invalid `area`, `scale` or `crop` (also a crop outside
the image) is a parameter error and no file is written.

### `emu_screenshot_series_start` (sensitive - creates files)

Saves a PNG of every emulated frame that differs from the last saved one -
for finding animation bugs. Arguments (all optional): `dir` (series
folder; empty = a new `<platform>_frames_YYYYMMDD_HHMMSS` folder in the
screenshot output folder), `area`, `scale` (default `native`), `crop` -
as above, only changes inside them count; `only_changed` (default `true`;
`false` = every frame), `frames` (stop after N emulated frames, 0 = until
stop), `max_files` (safety limit, 0 = 10000).

Frames are taken in emulated time at the end of every frame - none is
skipped even at MAX SPEED; when writing cannot keep up, the emulation
waits. Nothing is captured while the emulation is paused. Files
`frame_NNNNNNN.png` (NNNNNNN = emulated frame since the start, gaps =
unchanged frames) and `index.csv` (`frame,file,gdg_screen`). Returns the
series status plus `area`, `scale`, `only_changed`. Errors: a series is
already running, invalid parameters, the folder cannot be created.

### `emu_screenshot_series_stop`

Argument `wait` (bool, default `true`): wait (at most 30 s) until all
queued frames are written. Returns the series status. Without a series:
`No frame series is running`.

### `emu_screenshot_series_status`

No arguments. Returns `active` (capturing), `writing` (files still being
written), `frames_seen`, `files`, `unchanged`, `dir`, `stop_reason`
(`Stopped`, `Frame count reached`, `File limit reached`, `Error`, empty
while running) and `last_error`.

## Profiler tools

The CPU profiler aggregates per-function statistics (calls, exclusive
and inclusive Z80 cycles, min/max/avg). It is built on top of the
Callstack listener API - every CALL / RST / IRQ / NMI entry registers
a sample, RET / RETI / RETN terminates the measurement and adds the
inclusive and exclusive cycle counts.

**Hot-path overhead:** an active profiler adds measurable overhead to
the CPU loop. For production benchmarks it is recommended to start
the profiler just before the measurement and stop it right after.
The client should not leave the profiler permanently on.

Profiler state ownership:

- `emu_profiler_start` also enables the Callstack subsystem if it
  was off.
- `emu_profiler_stop` restores the prior Callstack state (= only
  if the profiler activated it).
- `emu_profiler_reset` does not change the recording status -
  only clears the aggregator.

### `emu_profiler_start`

Activate the profiler. No arguments. Returns `{"active": true}`.

### `emu_profiler_stop`

Deactivate the profiler. Aggregator data is preserved - the client
may still call `emu_profiler_get` or `emu_profiler_export` before
resetting. Returns `{"active": false}`.

### `emu_profiler_reset`

Clear the entries hash map, global counters, and the baseline
cycle counter. Recording status does not change. Returns
`{"reset": true}`.

### `emu_profiler_export`

Export the aggregator to a file. Arguments:

- `path` (string, required) - target filesystem path.
- `format` (string, optional, default "csv") - "csv" or "json".

Format details:

- CSV: UTF-8, LF line endings, locale-safe. Header
  `addr,kind,calls,excl_cycles,incl_cycles,min_cycles,max_cycles,avg_cycles`.
- JSON: `{"stats": {...}, "entries": [...]}` - global stats plus
  an array of per-function records.

`min_cycles=0` indicates an entry without a matched exit (= UI
semantics).

Returns `{"path": "...", "format": "csv"|"json", "entry_count": N}`.

### `emu_profiler_get`

Returns the current aggregator as inline JSON (no file I/O).
Argument:

- `limit` (int, optional, 1..1000, default 50) - maximum number
  of entries in the response. `entry_count` in the response is
  always the total count, independent of the limit.

Response:

```json
{
  "active": bool, "entry_count": int, "limit": int,
  "total_cycles_64": int, "total_calls": int, "irq_entries": int,
  "unmatched_returns": int, "max_depth_reached": int,
  "overflow_count": int,
  "entries": [
    {"addr": int, "kind": "call"|"rst"|"irq_im0"|"irq_im1"|
                          "irq_im2"|"nmi"|"synthetic",
     "calls": int, "excl_cycles": int, "incl_cycles": int,
     "min_cycles": int, "max_cycles": int, "avg_cycles": int},
    ...
  ]
}
```

The `entries` array is in hash-map iteration order (= not sorted).
The client is expected to sort it according to its own metric
(typically `excl_cycles` descending).

## Per-PC profiler tools

The per-PC profiler counts every executed instruction by its address
(logical PC): executions, T-states including wait states, and wait
states alone (VRAM access, PSG write). Unlike the function profiler
above it needs no CALL/RET pairing, so it also sees code that is
jumped to, interrupt handlers and self-contained loops - typically the
place to start when optimizing a program.

- Block instructions (LDIR, LDDR, ...) count every repetition as one
  execution (the CPU fetches them again).
- Memory banks are not told apart: code at the same address in
  different banks adds up.
- Time outside instructions is counted separately: `halt_idle` (the CPU
  waits in HALT, the HALT instruction itself counts at its address) and
  `other_tstates` (mainly interrupt acknowledge). Always
  `tstates + halt_idle + other_tstates == elapsed_tstates`.
- Overhead: one function call per instruction while it runs. With the
  profiler stopped the emulation is not slowed down.

Typical use:

```
emu_profiler_pc_start()          # clears previous data
emu_run(frames=50)               # one second of an MZ-800 (PAL)
emu_profiler_pc_get(limit=20)    # where the time went
emu_profiler_pc_stop()
```

All six tools return the summary `{"active", "instructions",
"tstates", "wait", "halt_idle", "other_tstates", "elapsed_tstates",
"unique_pcs"}` (plus the fields below).

### `emu_profiler_pc_start`

Start collecting. Argument `reset` (bool, default true): clear previous
data first; false continues adding to it.

### `emu_profiler_pc_stop`

Stop collecting; the data stays for `emu_profiler_pc_get` and
`emu_profiler_pc_export`.

### `emu_profiler_pc_reset`

Clear the data. Whether the profiler runs is not changed.

### `emu_profiler_pc_get`

The busiest addresses inline. Arguments (all optional):

- `sort` - `tstates` (default), `count`, `wait` or `addr`,
- `limit` - rows, 1..1000 (default 50),
- `from_addr`, `to_addr` - address range, inclusive (default 0..65535).

Adds `sort`, `total_rows` (executed addresses in the range, may exceed
`limit`) and `rows`:

```json
{"pc": 33667, "count": 109893, "tstates": 2307503, "wait": 0,
 "tstates_pct": 64.79, "avg_tstates": 21.0,
 "symbol": "fill_loop", "sym_offset": 3}
```

`tstates_pct` is the share of all instruction T-states. `symbol` is the
nearest loaded symbol at or below `pc` (CPU view, as
`emu_symbol_lookup`), `""` when there is none.

### `emu_profiler_pc_export`

Write all executed addresses to a CSV file. Arguments: `path` (required,
overwritten; the folder must exist) and `sort` (default `addr`). CSV:
UTF-8, LF line endings, decimal point regardless of the locale; header
`pc,count,tstates,wait,tstates_pct`, `pc` as 4 hex digits. Adds `path`
and `rows`.

### `emu_profiler_pc_bitmap`

The executed addresses as an 8 KiB bitmap - code coverage that can be
read often (for example after every step of an automated run) without
transferring rows. No arguments. Adds `format` (`pc-bitset-lsb0`),
`map_bytes` (8192) and `bitmap_base64`.

Bit `addr & 7` (bit 0 = lowest) of byte `addr >> 3` is 1 when at least
one instruction starting at the address was executed. These are the same
addresses as the rows of `emu_profiler_pc_get` and
`emu_profiler_pc_export`; the number of set bits is `unique_pcs`.
Operand bytes get no bit, an idle HALT adds nothing, a block instruction
marks its start; memory banks and versions of self-modified code at one
address are not told apart. Works while collecting and after
`emu_profiler_pc_stop`; it neither pauses nor advances the emulation.

## Code coverage tools

Code coverage answers which instructions ran, which way every
conditional instruction went (`JR` / `JP` / `CALL` / `RET` with a
condition, `DJNZ`, `LDIR` / `CPIR` / `INIR` / `OTIR` and the
decrementing forms) and, at the `detail` level, which version of the
code ran where. The result of a condition is derived from the CPU state
after the step (flags, B, PC back on a repeating block instruction), so
`JR Z,$+2` counts correctly although both results end at the same
address.

- `branch` - by logical PC: executed addresses, conditional
  instructions and their results, first instruction of an interrupt
  handler. Measured cost in MAX SPEED: about +16 to +29 % emulation time.
- `detail` - adds one unit per (physical cell of the first byte, PC,
  code version) with counters `exec`, `taken`, `not_taken`, the last
  operand and the last next PC. The same PC in RAM and ROM, or in
  another MemExt bank, is a different unit; a self-modified operand
  stays one unit flagged `operand_varied` (key `opcode`), with key
  `full_bytes` every operand value is its own unit. The bytes are taken
  as the CPU fetched them, which needs the slower memory path.
  Measured cost: about +35 to +95 % emulation time.
- An idle HALT is not counted; reset, snapshot load and replay seek /
  rewind keep the data and increase `discontinuities`.
- The same collection can be started from the command line
  (`--coverage`, `--coverage-save-on-exit`, see
  [Headless mode](headless-mode.md)), from the INI section `[COVERAGE]`
  and from a breakpoint action (`coverage_start`, `coverage_stop`,
  `coverage_mark`).

Recipe for AI clients: `emulator://docs/coverage`. In the GUI: Coverage
tab of the CPU Profiler window and marks in the Disassembly windows, see
[Code coverage](../debugger/code-coverage.md); lcov via
`docs/tools/coverage_to_lcov.py`.

Typical use:

```
emu_coverage_start(level="branch", reset=true)
emu_run(frames=500)
emu_coverage_read(filter={"state": "partial"})   # branches that went one way only
emu_coverage_stop()
```

All tools return the status `{"active", "has_data", "level", "key",
"reset_on_discontinuity", "complete", "units", "capacity", "dropped",
"fetch_fallback", "saturated", "discontinuities", "host_state_edits",
"epoch", "generation", "instructions", "halt_idle", "branches":
{"decisions", "both", "taken_only", "not_taken_only"}}` (plus the fields
below). `complete` is false when a unit did not fit (`dropped`), a byte
was not in the fetch record (`fetch_fallback`) or a counter saturated.

### `emu_coverage_start`

Arguments (all optional): `level` (`branch` default, `detail`), `key`
(`opcode` default, `full_bytes`; detail only), `capacity` (detail units,
1..1048576, default 65536, 32 bytes each), `reset` (default false:
continue existing data with the same settings), `reset_on_discontinuity`
(default false).

### `emu_coverage_stop`, `emu_coverage_reset`, `emu_coverage_status`

Stop collecting (data stay readable) / clear the data (new
`generation`, collection continues if it ran) / status only.

### `emu_coverage_mark`

Start a new epoch; the status carries its number in `epoch`. Every
address and unit remembers the epoch in which it first ran.

### `emu_coverage_read`

A page of rows sorted by PC. Rows are arrays in the order of `columns`.

- `branch` rows (executed addresses): `pc`, `state`, `int_entry`,
  `first_epoch`.
- `detail` rows (units): `pc`, `space` (`ram`, `memext`, `rom`, `vram`,
  `pcg`, `bus` = no physical cell, offset is the CPU address), `offset`,
  `key` (opcode bytes in hex), `len`, `exec`, `taken`, `not_taken`,
  `state`, `operand` (null without an operand), `next_pc`,
  `first_epoch`, `flags`.
- `state`: `none` (not conditional), `unobserved`, `taken_only`,
  `not_taken_only`, `both`.

Arguments: `cursor` (`next_cursor` of the previous page), `columns`,
`filter` (`pc_from`, `pc_to`, `space`, `state` - one name or a list,
also `partial` and `decision` -, `new_since_epoch`, `flags`), `limit`
(1..5000, default 500), `max_chars` (1000..45000, default 20000). Adds
`columns`, `rows`, `returned`, `page_full`, `end_of_data` and
`next_cursor`. After `emu_coverage_reset` or a new start an older
cursor fails with `segment_replaced:`.

### `emu_coverage_export`

All rows into a file. Arguments: `path` (required, overwritten; the
folder must exist) and `format` (`json` default, `csv`). JSON:
`{"format": "mz800emu-coverage/1", "status", "columns", "rows":
[{...}]}`; CSV: header with the column names, flags separated by `|`.
Adds `path`, `format` and `rows`.

## Audio capture tools

The audio capture writes what the sound hardware does into a folder, in
emulated time - for finding sound bugs of a program or of the emulator.
The same capture is available in the GUI (Tools -> Audio Capture, `Alt + Q`)
and on the command line (`--audio-capture`), see
[`../audio-capture.md`](../audio-capture.md).

| File | Content |
|---|---|
| `events.csv` | `ticks,time_us,frame,channel,level` - every change of the output level (0-15) of a sound channel |
| `psg_writes.csv` | `ticks,time_us,frame,psg,byte,channel,reg,value` - every write to the PSG, decoded |
| `reference.wav` | the sound rendered exactly in emulated time (the renderer of the video recording) |
| `sdl.wav` | the sound produced by the emulator's audio output path - what the user hears |
| `capture.json` | platform, `gdg_clk_hz`, `frames_per_sec`, channel names, counts, stop reason, absolute times `origin_ticks`, `end_ticks`, `last_event_ticks` and the platform clocks `clocks` (written at the end, see audio-capture.md) |

- `ticks` are GDG clock ticks since the start of the capture
  (`gdg_clk_hz` in capture.json), `time_us` the same in microseconds,
  `frame` the audio frame (= video frame, 20 ms PAL / 16.7 ms NTSC)
  since the start.
- Channels: `ctc0` (the speaker driven by CTC0 and the PIO, level 0 or
  15), `psg0_0`..`psg0_3` and, on the MZ-1500 or an MZ-800 with the
  second PSG, `psg1_0`..`psg1_3`. Channel 3 of a PSG is the noise
  channel. The first rows of events.csv (ticks 0) are the levels at the
  start.
- psg_writes.csv: `psg` is the target (1 = PSG0, 2 = PSG1, 3 = both),
  `byte` the written value in hex. Decoding from the PSG state after the
  write (for both PSGs from PSG0): `reg` = `tone_lo` (latch byte of a
  tone: the low 4 bits of the divider, the divider changes only with the
  data byte), `tone` (the complete 10-bit divider), `volume`
  (attenuation 0-15, 15 = off) or `noise` (`(type << 2) | rate`).
- Both WAV files are 44100 Hz, 16 bit, stereo. sdl.wav comes later than
  reference.wav: without an audio device (headless) by exactly one frame,
  with a device by its buffering. With a device at MAX SPEED the output
  path skips frames - `sdl_blocks` is then lower than `frames`.
- Start: the capture begins at the end of the next audio frame, with
  `sdl_wav` after 6 more frames that warm up the output path (its filters
  would otherwise start cold). Levels that did not change for a while
  start damped in both files, as in the output path.
- End: `emu_audio_capture_stop`, `frames` reached, a safety limit of one
  hour, a reset or a snapshot load (the emulated time jumps back), a write
  error, or the end of the emulator.
- It captures only while the emulation runs - combine with
  `emu_run(frames=N)`.

Typical use:

```
emu_audio_capture_start(dir="C:/tmp/snd", frames=250)   # 5 s PAL
emu_run(frames=260)
emu_audio_capture_status()      # stop_reason "Frame count reached"
```

### `emu_audio_capture_start`

Arguments (all optional): `dir` (folder, created; empty = a new
`<platform>_audio_<date>_<time>` folder in the audio capture output folder,
default `audio_captures` next to the emulator),
`frames` (stop after this many audio frames, 0 = until stop), `events`,
`psg_writes`, `reference_wav`, `sdl_wav` (bool, default true - which
files to write), `memory` (bool, default true - keep the capture memory
for `emu_trace_read` `audio_*`, `emu_audio_capture_summary` and
`emu_audio_capture_pcm`), `pcm` (list `"reference"` / `"sdl"`, default
`["reference"]` - PCM rings; both are needed for `ref_vs_sdl` in the
summary), `memory_minutes` (1-120, default 10 - frame summary ring),
`pcm_seconds` (1-3600, default 60 - PCM rings). A capture into memory
only (all four files false) creates no folder. Returns the status with
`pending: true`, or an error (already running, nothing selected, folder
cannot be created, not enough memory).

### `emu_audio_capture_stop`

Stop and finish the files. Returns the final status; error "No audio
capture is running" when there is none.

### `emu_audio_capture_status`

`{"pending", "active", "frames", "events", "psg_writes",
"reference_samples", "sdl_blocks", "sdl_samples", "dir", "stop_reason",
"last_error"}`. `stop_reason`: `Stopped`, `Frame count reached`,
`Safety limit reached`, `Emulation timeline restarted (reset or snapshot
load)`, `Error`, `Emulator exit`, empty while running.

Further `segment` (capture number since the emulator start; a new start
replaces the memory), `origin_ticks` (absolute GDG ticks of time 0, `null`
before the start), `channels` (channel names in the order of the
`audio_frames` arrays), `stereo` and `memory` (`null`, or `{"bytes",
"streams": {"psg_writes" | "levels" | "frames" | "pcm_reference" |
"pcm_sdl": {"record_size", "capacity", "first_seq", "next_seq"}}}`;
`first_seq` > 0 = the ring dropped its oldest records, PCM `seq` = sample
at 44100 Hz since the start).

### `emu_audio_capture_summary`

Summary of the capture memory, a few kB of text. Arguments (optional):
`frame_from` (default 0), `frame_to` (-1 = newest), `silence_rms`
(int16 RMS threshold of the reference, default 64), `max_notes` (default
200), `lag_frames` (-1 = search the delay of the audio output), and
`max_lag_frames` (default 8). Returns:

- `frames` [from, to], `frames_available`, `fps`, `platform`, `stereo`;
- `sound_spans` / `silent_spans` ([[from, to]], at most 50 each,
  `*_total` = all) - silent = reference RMS below `silence_rms`;
- `channels` - per channel `active_frames`, `edges`, `max_level`,
  `on_pct`, `est_hz_median` (level changes / 2 per second);
- `psg_notes` - `ch`, `note` (`"A4"`, or `"noise"`), `cents`, `hz`,
  `divider`, `from_frame`, `to_frame`, `max_level`, replayed from the PSG
  writes and the PSG state at the capture start; `psg_state_incomplete`
  = the oldest writes were dropped;
- `ctc0` - the speaker: `edges`, `edge_interval_hist_us` [[bucket_from_us,
  count]], `likely` (`tone`, `pcm`, `irregular`, `silent`) - a heuristic
  estimate, the rule is in `likely_rule`;
- `ref_vs_sdl` (needs `pcm` with `reference` and `sdl`) - `lag_samples`,
  `lag_frames`, `lag_ambiguous` (only a steady tone in the compared part,
  the delay cannot be determined), `diff_rms`, `diff_max` (-1..1, mono),
  `reference_rms`, `worst_frames` [[frame, diff_rms]],
  `sdl_missing_blocks`; at most 2 s are compared.

### `emu_audio_capture_pcm`

A PCM window from the capture memory. Arguments (optional): `source`
(`reference` or `sdl`), `frame_from` / `frames` (default 0 / 25) or
`sample_from` / `samples` (44100 Hz samples since the start), `rate`
(1000-44100, default 11025), `stereo` (default false = mono average),
`encoding` (`wav_b64` or `pcm_b64` = raw int16 little endian), `max_bytes`
(data limit before base64, default 32768, at most 4 MiB). Resampling
averages (an analysis preview); `rate` 44100 with `stereo` returns the
samples of reference.wav / sdl.wav byte for byte. Returns `source`,
`segment`, `live`, `rate`, `channels`, `from_sample`, `samples`,
`frames_out`, `next_sample` (continue there), `end_of_data`, `truncated`,
`available` [first, next], `gap`, `encoding`, `data_b64`.

## Media tools

Unified access to media operations across 11 supported slots:

| Slot | Meaning |
|------|---------|
| `cmt` | CMT cassette (.mzf, .mzt) |
| `fdc0_fd0` .. `fdc0_fd3` | WD279x FDC0 (standard, ports 0xD8-0xDF) drives 0..3 (.dsk) |
| `fdc1_fd0` .. `fdc1_fd3` | WD279x FDC1 (secondary, ports 0x58-0x5F) drives 0..3 (.dsk) |
| `qd` | Quick Disk (.qd) |
| `ide8` | IDE8 master HDD (.img) |

A slot is unavailable if the arch build does not include the
corresponding HW module. In that case `emu_media_state` still
includes the slot in the response with `inserted=false`, `path=""`.

**Inline base64:** `emu_media_load_mzf` and `emu_media_insert` accept
either `path` OR `bytes_b64`. For `bytes_b64` the server decodes the
content to a temporary file and passes its path to the media
subsystem. The temp file is best-effort unlinked after the call
(on Windows where the drive holds an open handle it persists until
unmount).

### `emu_media_load_mzf`

CMT-hack instant load. Bypasses the cassette emulation and performs
the full load (header + body) via a ROM monitor patch (= the program is
in memory immediately, no audio simulation). The body is placed at the
LOAD_ADDR from the MZF header. Usage:

```
emu_media_load_mzf(path="/programs/gardener.mzf")
```

Or inline:

```
emu_media_load_mzf(bytes_b64="UVWdC...")
```

Returns `{"ok": true, "load_addr": int, "exec_addr": int, "size": int,
"result_code": 0}` (`load_addr` = MZF fstrt, `exec_addr` = MZF fexec,
`size` = body length). On error (file cannot be opened / invalid header
/ body load failed) returns `{"error": ...}` - no false `ok`.

**Side effect:** for a program with LOAD_ADDR < 0x1000 the lower ROM
(0x0000-0x0FFF) is temporarily unmapped while the body is written so the
program lands in RAM (otherwise it would be written under ROM and lost).
The original memory map is restored after the load, so there is no
lasting banking side effect. It is a clean load-only primitive: it sets
neither PC nor SP and restores the HL/BC/AF scratch registers it uses.

After `media_load_mzf` the CPU stays where it was (= typically the
ROM Monitor scan loop) - data is in RAM but no jump is performed.
To also start executing the program use `emu_media_run_mzf` (below).

### `emu_media_run_mzf` (sensitive)

Loads an MZF and starts it from the EXEC address in the header
(offset 0x16). Two variants selected by the `bootstrap` parameter:

- **`bootstrap=true` (default)** - resets the machine and starts the
  program exactly like the CLI option `--run-mzf` (emulator command
  `media_run_mzf`). The machine is in the state the ROM leaves it in
  after loading a program from tape: CPU registers (IM 1, DI,
  SP = 0x10F0, the registers the ROM passes to the program), 8255,
  8253, Z80 PIO, PSG, GDG, VRAM and the monitor work area in RAM. The
  reset is the same as `emu_reset`, so the result does not depend on
  what ran in the emulator before. The pause state is kept: when paused,
  the program waits at its first instruction (PC = EXEC). The file is
  checked before the reset - a missing, unreadable or truncated file
  returns an error and the machine is not reset.
- **`bootstrap=false`** - the older composite without a reset:
  `emu_media_load_mzf` + disconnect lower/upper ROM (ports 0xE0/0xE1) +
  `emu_set_register` PC. Nothing else is initialised: interrupt mode,
  IFF, Z80 PIO, CTC, palette and all other state stay as the previous
  program left them. The program may then behave differently than after
  a tape load (for example it gets no interrupts).

```
emu_media_run_mzf(path="/programs/mzdos.mzf")
emu_media_run_mzf(path="/programs/mzdos.mzf", bootstrap=false)
```

Returns:

```json
{
  "loaded": true,
  "header": {
    "file_type": 1, "filename": "mzdos",
    "file_size": 4096, "load_addr": 256, "exec_addr": 256
  },
  "bootstrap": true,
  "reset": true,
  "pc_set_to": 256
}
```

With `bootstrap=false` the result has `"bootstrap": false,
"reset": false` and also `"rom_disconnected": true`. The MZF header is
parsed by the wrapper (Python).

**Destructive:** both variants replace the running program. If the
program must go through the real ROM path (IPL or monitor reading the
tape with its own loader), use `emu_media_insert(slot='cmt', path=...)`
or `emu_cmt_open` and the load command from the keyboard (slower, but
it runs the whole ROM code).

### `emu_media_load_binary` (sensitive)

Raw bytes from file to Z80 memory at the given address. Writes go
through the banking-aware path with no region checks - may overwrite
ROM-shadow RAM, video memory etc. in the current banking.

```
emu_media_load_binary(path="/build/sprite.bin", addr=0xC000)
```

Returns `{"ok": true, "addr": 0xC000, "size": 256, "result_code": 0}`.

### `emu_media_insert` (sensitive)

Insert image into slot. If the slot is already occupied, a silent
auto-eject happens (= equivalent of eject + insert in one step).

```
emu_media_insert(slot="fdc0_fd0", path="/disks/cpm.dsk")
emu_media_insert(slot="fdc1_fd0", path="/disks/data.dsk")
emu_media_insert(slot="cmt", bytes_b64="...", ro=True)
emu_media_insert(slot="qd", path="/disks/game.mzq", ro=True)
```

Returns `{"ok": true, "slot": "fdc0_fd0", "result_code": 0}`.

Slot `qd` mounts an MZ-1F11 Quick Disk image (`.mzq`). The file must
exist (the Quick Disk dialog in the GUI creates a new blank image).
If the drive is not connected as MZ-1F11 (the default is the Unicard
boot loader), it is switched the same way as the MZ-1F11 menu choice,
which disconnects the Unicard boot loader from the QD; the response
reports this with `"switched_to_mz1f11": true`. `ro` sets write
protection the same way as the menu toggle (the choice is saved to
the configuration).

### `emu_media_eject`

Eject image from slot. No-op when the slot is empty.

```
emu_media_eject(slot="fdc0_fd0")
```

Returns `{"ok": true, "slot": "fdc0_fd0"}`.

### `emu_media_state`

Snapshot of all 11 slots' state.

```
emu_media_state()
```

Returns:

```json
{
  "count": 11,
  "slots": [
    {"slot": "cmt",      "inserted": true,  "path": "/cassettes/sapo-p.mzf", "ro": false},
    {"slot": "fdc0_fd0", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd1", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd2", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd3", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd0", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd1", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd2", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd3", "inserted": false, "path": "", "ro": false},
    {"slot": "qd",       "inserted": false, "path": "", "ro": false},
    {"slot": "ide8",     "inserted": true,  "path": "hdd0-mz800.img", "ro": false}
  ]
}
```

`ro` is the effective write protection of the inserted medium (user
choice, a file without write permission, the Unicard boot loader in the
Quick Disk); `false` with no medium.

## Platform + Config tools

Key design decision: **runtime platform switch DOES NOT exist** -
mz700/mz800/mz1500 are separate binaries selected at compile time.
`emu_platform_set` therefore returns an error stating which executable
the user must launch instead of the current one.

### `emu_settings_set` (sensitive)

Writes a live-settable INI key. The value applies immediately (like a
change in the menu) and is saved to the INI file on exit. The whitelist
contains keys with immediate application (volume, display settings,
trace PC filter). Boot-time keys (emulator paths, snapshot defaults) are
rejected.

```
emu_settings_set(key="AUDIO/volume_8253", value="60")
emu_settings_set(key="DISPLAY/locked_window_aspect_ratio", value="true")
```

Returns `{"key": ..., "previous_value": "50", "new_value": "60",
"type": "unsigned"}`. The `previous_value` can be used for rollback.

Whitelist:

- `AUDIO/volume_8253` (unsigned)
- `AUDIO/volume_psg0` .. `AUDIO/volume_psg3` (unsigned)
- `AUDIO/volume_psg1_0` .. `AUDIO/volume_psg1_3` (MZ-1500, unsigned)
- `DISPLAY/forced_full_screen_redrawing` (bool)
- `DISPLAY/locked_window_aspect_ratio` (bool)
- `DISPLAY/custom_fps` (unsigned, 50..1000; used in the Custom frame rate mode)
- `TRACE_CPUTRACK/pc_range_lo` .. `TRACE_CPUTRACK/pc_range_hi` (unsigned, 0..65535; runtime PC trace filter)
- `<ini_section>/<key>` - an option of an MZ-700 expansion device
  (`emu_get_devices`); a keyword option takes the keyword. The response
  has `applies`: `now` (applied immediately) or `on_connect` (applied
  when the device is connected next time). The key `connected` is
  changed only by `emu_periph_*`. Details: `emulator://docs/devices`.

The Quick Disk image and its write protection are set with
`emu_media_insert(slot="qd", ...)`.

### `emu_settings_get`

Reads the value of any configuration key (= read whitelist is open).
Returns the string representation plus the type code.

```
emu_settings_get(key="AUDIO/volume_8253")
-> {"key": "AUDIO/volume_8253", "value": "60", "type": "unsigned"}
emu_settings_get(key="QDISK/mz1f11_type")
-> {"key": "QDISK/mz1f11_type", "value": "UNICARD", "type": "keyword",
    "live": "STANDARD"}
```

`value` is the configured value: loaded at startup or written with
`emu_settings_set`. Changes made in the menu or by other tools (e.g.
`emu_periph_attach`, `emu_media_insert`) reach it only when the INI file
is saved. `live` is the value the emulator is working with right now;
it is present only for keys bound directly to a numeric variable
(device connections and types, display settings), otherwise it is
missing.

For INI keys of type `keyword` the textual keyword variant is
returned.

### `emu_platform_set` (sensitive)

**Runtime platform switch is not supported.** A call with
`kind != active_kind` returns `ok=false` with `active_kind` (= the
current build) and an explanatory error message. The user must launch
a different binary (e.g. `mz1500emu.exe` instead of `mz800emu.exe`).

A call with `kind == active_kind` is a no-op (= `ok=true,
no_op=true`) without side effects.

```
emu_platform_set(kind="mz1500")
-> {"ok": false, "active_kind": "mz800", "target_kind": "mz1500",
    "error": "Runtime platform switch not supported - mz700/mz800/
    mz1500 are separate binaries (compile-time MZARCH). To use a
    different platform, restart with the corresponding executable
    (e.g. mz1500emu.exe)."}
```

The parameters `mode` (`native` / `compat`) and `save_snapshot`
(path to .mzs) are accepted for forward compatibility but currently
ignored.

### `emu_periph_attach` (sensitive)

Attaches a peripheral at runtime, the same way as the Devices menu;
the choice is saved to the INI file on exit. Refused while a replay is
being recorded or played back.

| `kind` | `options.type` (first = default) |
|--------|----------------------------------|
| `memext` | `pehu`, `luftner` (default = the current type); attaching resets the bank map |
| `fdc` | `fdc0` (ports 0D8h-0DFh), `fdc1` (secondary, ports 58h-5Fh) |
| `qd` | `mz1f11` (.MZQ image), `virtual`, `unicard` (Unicard boot loader, needs a connected Unicard) |
| `ide8` | `master`, `slave`; opens the configured disk image (`IDE8/hdd0_filepath`, `hdd1_filepath`) and creates it when missing |
| `gal5` | not emulated, always an error (kept for compatibility) |
| *device id* | an MZ-700 expansion device from `emu_get_devices`; `options` are ignored |

```
emu_periph_attach(kind="memext", options={"type": "luftner"})
emu_periph_attach(kind="fdc", options={"type": "fdc1"})
```

Returns `{"ok": true, "kind": "memext", "type": "luftner",
"changed": true, "requires_restart": false, "result_code": 0}`. A state
that already holds is not applied again (`changed: false`). An unknown
variant returns an error listing the allowed values.

A `kind` that is not one of the fixed kinds is taken as the id of an
expansion device and checked by the emulator. Errors: unknown id,
conflict with a connected device (ports, MMIO range, video hook), the
device failed to connect, expansion devices not available on this
platform (MZ-800, MZ-1500). Details: `emulator://docs/devices`.

### `emu_periph_detach` (sensitive)

Detaches a peripheral at runtime. For `fdc` and `ide8` the
`options.type` selects the unit (default `fdc0`, `master`); for
`memext` and `qd` it is ignored.

```
emu_periph_detach(kind="fdc")
emu_periph_detach(kind="ide8", options={"type": "slave"})
```

### `emu_get_devices`

Read-only list of the expansion devices of the MZ-700 core (optional
cards of the real MZ-700) and their state: `id` (the `kind` for
`emu_periph_attach`), `name`, `connected`, `ini_section` and `options`
(type, value, default, `apply`, keywords or range), `ports`, `mmio`
(addresses and map states `mapped` / `dram`), `regions` (kind `device`),
`video` hooks and secondary `outputs`.

```
emu_get_devices()
-> {"platform_supported": true, "devices": [
     {"id": "pcg700", "name": "PCG-700 (HAL)", "connected": false,
      "ini_section": "PCG700", "options": [...], ...},
     {"id": "suc80", "name": "SUC 80-column kit", "connected": false, ...},
     {"id": "kp80", "name": "K&P 80 Column Card", "connected": false, ...,
      "outputs": [{"id": "kp80", "title": "K&P 80 Column Monitor",
      "width": 640, "height": 250}]}]}
```

On MZ-800 and MZ-1500 `platform_supported` is false and `devices` is
empty. The MZ-700 core has the devices `pcg700` (PCG-700 by HAL,
[devices/pcg700.md](../devices/pcg700.md)), `suc80` (SUC 80-column kit,
[devices/suc80.md](../devices/suc80.md)) and `kp80` (K&P 80 column card,
[devices/kp80.md](../devices/kp80.md)). Details and a full example:
`emulator://docs/devices`.

### `emu_device_screenshot`

Read-only picture of the own video output of an expansion device (a
second monitor, for example the K&P 80 column card, output `kp80`). The
screenshot tools above show only the computer's picture. The picture is
the last frame the device drew; when paused, a change made through the
debugger or MCP (`emu_io_write`, `emu_region_write`) is drawn at once.

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `output` | first output of a connected device | output id from `emu_get_devices` (`outputs[].id`) |
| `format` | `png` | `png`, or `indexed` = one byte per dot (palette index, rows from the top) plus `palette` |
| `path` | - | write the PNG to this file on the server host instead of `data_b64` (only `png`) |
| `want_data` | true | false = only hashes and counts |

```
emu_device_screenshot(output="kp80", format="indexed", want_data=False)
-> {"available": true, "output": "kp80", "device": "kp80", "width": 640,
    "height": 250, "pixel_aspect": 0.52, "seq": 12,
    "image_sha256": "...", "lit_pixels": 1874, "format": "indexed",
    "byte_size": 160000, "palette": ["#000000", "#F0F0F0", "#33FF66",
    "#FFB000"]}
```

`image_sha256` hashes the RGBA picture (the phosphor colour counts),
`lit_pixels` counts dots with a non-zero palette index, `seq` grows with
every frame the device draws. A disconnected device returns
`{"available": false, "reason": ...}`; an unknown `output` is an error.

### Example workflow - mounting a Quick Disk image

```
# Mount the image writable (switches the drive to MZ-1F11 if needed)
emu_media_insert(slot="qd", path="/disks/cpm.mzq", ro=False)

# Check the slot state
emu_media_state()
```

## Hot-swap tools

A pair of tools `emu_stop` + `emu_start` for the AI dev workflow:
cycle stop -> rebuild mz800emu.exe -> start, without restarting the
Claude Code session (= no loss of AI context). Both tools are
**pipe-only** - in TCP mode they return an error because a TCP attach
goes against the user's live GUI session.

State preservation is NOT automatic. The AI client must explicitly
save state before stop and restore it after start:

```
emu_snapshot_save_buffer  ->  emu_stop  ->  (rebuild binary)
->  emu_start  ->  emu_snapshot_load_buffer
```

### `emu_stop` (sensitive, pipe-only)

Graceful shutdown of the emu child process. The `mcp_server.py`
wrapper stays alive and keeps the MCP session with the Claude client.

Success response:
```json
{"stopped": true, "transport": "pipe"}
```

Error response in TCP mode:
```json
{"error": "hot-swap requires pipe transport (current: tcp)"}
```

Arguments: none.

### `emu_start` (sensitive, pipe-only)

Spawns a fresh emu child process. Counterpart to `emu_stop` for the
hot-swap workflow.

Arguments:
- `binary_path` (string, default ""): path to mz800emu.exe. If empty,
  uses `MZ800EMU_EXE` env or the default location next to
  `mcp_server.py`.

Success response:
```json
{"started": true, "exe": "C:/.../mz800emu.exe", "commands": 65}
```

The `commands` field reports the number of tools in the hello payload
of the new child process (= AI-client-side confirmation that the new
binary is alive).

Error responses:
- `"hot-swap requires pipe transport"`
- `"emulator already running (call emu_stop first)"`
- `"mz800emu binary not found at ..."`

`emu_start` is a **Python-only tool** - the emu binary cannot spawn
itself; only the wrapper has control over subprocess lifecycle.

## HID input tools

Six tools for simulating user input - keyboard + joystick. Key for AI
driven demos, automated testing, and replay scenarios.

Press / release injection goes through a **virtual keyboard matrix**
parallel to the physical scan matrix. The Z80 emulation ANDs the two
matrices when reading PORT B, so a virtual press appears to the Z80
as a real key held by the user.

Key name vocabulary (ONE table shared by `emu_input_send_key`,
`emu_input_send_keys` with `encoding=key_names`, `emu_input_press_key`,
`emu_input_release_key` and `emu_input_send_keys_with_delays`; case-insensitive):

| Name(s) | Key |
|---------|-----|
| `RETURN`, `ENTER`, `CR` | CR (Enter) |
| `SPACE`, `TAB` (MZ-800 only), `BLANK`, `GRAPH`, `ALPHA` | the key of that name |
| `LIBRA` | `£` (= SHIFT + DOWN_ARROW position), `F9` = same position without SHIFT |
| `INSERT`, `INS`, `INST` | INST |
| `DELETE`, `DEL`, `BACKSPACE` | DEL |
| `ARROW_UP`, `UP`, `CURSOR_UP` (likewise `DOWN`, `LEFT`, `RIGHT`) | cursor movement keys |
| `UP_ARROW`, `DOWN_ARROW` | arrow-glyph character keys (NOT cursor movement) |
| `ESC`, `ESCAPE`, `BREAK`, `END` | ESC / BREAK |
| `CTRL`, `CONTROL`, `SHIFT` | modifiers |
| `F1`..`F9` | function keys (`F6`-`F8` map to `@`, `\`, `?`; `F9` = LIBRA position without SHIFT, i.e. DOWN_ARROW) |

Plus ASCII fallback (`"A"`, `"ASCII:A"`, ...). The names printed in
`emulator://docs/mz800_keyboard` (`CURSOR_*`, `INST`, ...) are accepted as
aliases, and the older names keep working. An unknown name is rejected with
`Unknown key '<name>'. Closest valid names: ...` (for `send_keys` with
`key_names` and `send_keys_with_delays` the whole call fails before any key
is sent).

The joystick state byte uses the Sharp MZ standard:

- bit 0 = UP, 1 = DOWN, 2 = LEFT, 3 = RIGHT
- bit 4 = FIRE1, 5 = FIRE2
- bits 6-7 unused

The MCP interface uses an **active-HIGH** mask (0x01 = UP pressed).
The native HW state byte is active-LOW (0xFE = UP pressed). Bridge
conversion happens automatically.

Frame timing default = 3 frames (60 ms of emulated time at 50 Hz PAL).
Maximum 600 frames per hold or gap as a safety against AI-induced freezes.

**Emulated frames semantics:** `frames`, `frame_per_key`, `hold_frames`
and `gap_frames` count **emulated frames**, exact at any emulation speed
(normal, MAX SPEED, slower than real time e.g. with CDL). The whole
press / hold / release / gap sequence is handed to the emulator thread,
which presses and releases the keys itself on frame boundaries. A paused
emulator runs exactly the length of the sequence and pauses again (like
`emu_run(frames=N)`); a running one keeps running. `emu_input_send_keys`
leaves 1 frame with no key pressed between consecutive keys (a repeated
key registers as two presses). The reply carries
`emu_frames` (emulated frames that actually elapsed), `complete` and
`interrupted` (`true` = cut short by a breakpoint, a pause or a reset;
the held key was released). At normal speed a sequence takes real time
(50 frames = 1 s).

All HID tools carry a **WARNING** token in their description - user
simulation can trigger unintended behavior (= RUN+RETURN, BASIC
program overwrite, AI-controlled game in unexpected contexts).

**Key landing readback (`landing_verified`):** `emu_input_send_keys`
returns `{"keys_sent": N, "keys_landed": N, "total_frames": N, "encoding": str,
"landing_verified": <bool>}`. `keys_sent` counts host-side injections
into the virtual matrix; `keys_landed` counts how many of them the guest
actually read. Previously the tool reported success even when the guest
swallowed the keys (a running program with the ROM disconnected scans only
a narrow subset of the matrix) - that was a silent failure. `landing_verified`
is now a real readback: while the key is held the emulator watches the PPI
Port B reads and the flag is `true` only if the guest scanned the key's column
during the hold (i.e. it actually read the key), `false` otherwise. If a key
does not land, hold it longer (`frame_per_key`), make sure the guest is at a
prompt that scans the keyboard, or reset the emulator.

### Practical examples

**ASCII encoding** (= default, simple text + `\r` for RETURN):

```python
# Type "LOAD" + Enter (= Sharp ROM Monitor LOAD command)
emu.call('input_send_keys', text="LOAD\r", encoding='ascii',
         frame_per_key=3)

# BASIC program type-in (= multi-line)
emu.call('input_send_keys',
         text="10 PRINT \"HELLO\"\r20 GOTO 10\rRUN\r",
         encoding='ascii', frame_per_key=3)
```

**Key names encoding** (= JSON array string, explicit control):

```python
import json
keys = ["G", "R", "A", "P", "H", "SPACE", "1", "RETURN"]
emu.call('input_send_keys', text=json.dumps(keys),
         encoding='key_names', frame_per_key=3)
```

**Modifier hold pattern** (= SHIFT + key):

```python
emu.call('input_press_key', key='SHIFT')   # hold SHIFT
emu.call('input_send_key', key='A', frames=3)  # press A while SHIFT held
emu.call('input_release_key', key='SHIFT')  # release SHIFT
```

**Joystick** (= bitmask game input):

```python
# Joystick port 0, UP + FIRE1 for 5 frames
emu.call('input_send_joystick', port=0, state=0x11, frames=5)
```

**Timing-controlled sequence** (= speedrun-style):

```python
# RIGHT held 30 frames, 5 frames nothing, SPACE held 2 frames
events = [
    {"key": "RIGHT", "hold_frames": 30, "gap_frames": 5},
    {"key": "SPACE", "hold_frames": 2},
]
emu.call('input_send_keys_with_delays', events=events)
# -> {"events_processed": 2, "total_frames": 37, "emu_frames": 37,
#     "complete": true, "interrupted": false}
```

Each event is `{"key", "hold_frames" (default 3), "gap_frames" (default 0)}`;
an event without `key` is skipped.

### Reverse lookup of keys

To enumerate valid `key_names` for a specific platform read:

```python
info = emu.call_resource('emulator://input/keyboard/matrix_info')
# returns { platform, keys: [{col, bit, name, ascii_char}], key_count }
```

The table contains aliases (= `RETURN`/`ENTER`/`CR` point to the same
position). The same `(col, bit)` appears under multiple names.

## Example usage from Claude Code

**Prompt:**

> Launch the emu, pause it, look at the registers, and return them.

**What Claude does:**

1. `emu_status` -> verify connected
2. `emu_pause` -> pause
3. `emu_get_registers` -> read registers
4. Return a formatted register dump to the user.

**Prompt:**

> Set a breakpoint at 0x0000 and reset the emu.

**What Claude does:**

1. `emu_bp_add(address=0x0000)` -> add BP
2. `emu_reset` -> reset
3. `emu_run(frames=10)` -> emu runs until BP hit or 10 frames done
4. `emu_status` -> report whether we hit the BP or completed the
   frames

## BP advanced tools

Layer on top of the existing `emu_bp_add` / `emu_bp_list` /
`emu_bp_remove` adding selective field writes and breakpoint group
management (= hierarchy for grouping breakpoints in the UI).

### `emu_bp_create_with_init`

Atomic `breakpoints_add_auto(addr, name, parent)` + selective
`BP_UPDATE` pass inside a single DBGAPI safepoint. The client never
observes the partially constructed breakpoint.

Args:

- `addr` (int) - initial Z80 address. The UM_ADDR bit is implicitly
  added to `fields` if omitted.
- `fields` (list[str]) - field names to apply (subset of
  `DBGAPI_BP_UM_*`). Valid names match the mask bits:
  `"enabled"`, `"auto_name"`, `"name"`, `"colors"`, `"parent"`,
  `"type"`, `"addr"`, `"addr_end"`, `"zone"`, `"bank_id"`,
  `"port"`, `"event_name"`, `"event_trigger"`, `"sp_threshold"`,
  `"expr"`, `"action"`, `"hit_count"`, `"skip_count"`,
  `"edge_triggered"`, `"addr_match_mode"`, `"addr_mask"`,
  `"port_match_mode"`, `"port_end"`, `"port_mask"`, `"port_mode"`,
  `"bank_match_mode"`, `"bank_id_end"`, `"bank_id_mask"`,
  `"bp_addr_space"`,
  `"sp_mode"`, `"sp_upper"`, `"im2_vector_filter"`,
  `"im2_vector_match_mode"`, `"im2_vector_addr_end"`,
  `"im2_vector_mask"`, `"im2_isr_filter"`,
  `"im2_isr_match_mode"`, `"im2_isr_addr_end"`, `"im2_isr_mask"`,
  `"im0_enabled"`, `"im1_enabled"`, `"im2_enabled"`,
  `"im0_rst_mask"`, `"irq_sig_source_mask"`,
  `"fwd_min_interval_ms"`, `"fwd_max_fires"`.
- `values` (dict) - map of field name to value. Only fields listed
  in `fields` are read. String fields (`name`, `expr`, `action`,
  `event_name`) accept `None` to clear. `name` without `auto_name`
  in `fields` turns the automatic name off (`Addr: 0x....`, renamed on
  every address change); `auto_name: true` brings it back. The same
  applies to `emu_bp_update`.

Returns: JSON `{"id": int, "created": bool}`. `id` is `-1` on
failure. If `addr_end` is set to a value different from `addr` but
`addr_match_mode` is not set to `RANGE` (a new breakpoint starts in
`SINGLE`), the breakpoint is still created, matches only `addr`, and the
result also carries `warning`. To watch a range, pass both
`"addr_end"` and `"addr_match_mode"` (value `"RANGE"`) in `fields`.

The `fwd_min_interval_ms` and `fwd_max_fires` fields are a per-BP
override of the forward-action (snapshot / trace_save) rate limit -
see the "Forward action protection (rate limit + saturation)" section
below.

### `emu_bp_set_parent`

Quick reparent a breakpoint into a group. Args: `id`, `parent_id`
(`-1` = root). Returns: `{"updated": bool}`.

### `emu_bp_update`

Selectively update fields of an existing breakpoint. Args: `id`,
`fields` (field names), `values` (value map). An empty `fields`
list is a successful no-op. Returns: `{"updated": bool}`.

Like `emu_bp_create_with_init`, it also accepts the
`fwd_min_interval_ms` and `fwd_max_fires` fields for a per-BP
override of the forward-action rate limit - see the "Forward action
protection (rate limit + saturation)" section below.

### Forward action protection (rate limit + saturation)

Forward actions in a BP script that write to disk (snapshot,
trace_save) have built-in protection against flooding the emulator
and the disk when a BP fires very frequently:

- **Per-BP rate limit.** `fwd_min_interval_ms` is the minimum delay
  in ms between two forward actions of the same BP; firing faster is
  silently skipped. `fwd_max_fires` is a hard cap on successful fires
  per session - once reached, the BP disables itself. Both fields are
  settable via `emu_bp_create_with_init` / `emu_bp_update`
  (`fields: ["fwd_min_interval_ms", "fwd_max_fires"]`). Without an
  explicit override the global default from configuration applies.
- **Byte saturation.** A cumulative byte limit on the volume written
  by forward actions (snapshots and traces saved by actions, plus the
  recording of trace channels an action started or saves) - once the
  threshold is exceeded the emulator auto-pauses and emits a saturation
  event / warning with a reason (`stopped_by: "bp_action_saturation"`).
  A trace started only with `emu_trace_start` or from the GUI is not
  counted; its own `max_total_mb` stops the recording and the emulation
  continues. The threshold is configurable.
- **Control-plane robustness.** Even when a continuing BP triggers a
  heavy forward action, `pause`, `force_pause` and `get_state` are
  served right after the action - the emulator does not get stuck.
  The action can run in the middle of an instruction (a MEM_W BP fires
  before the write), so other commands (register and memory reads,
  writes, breakpoint changes) wait for the end of the frame or for the
  pause and see a consistent state. To change breakpoints under heavy
  actions, pause first.

### `emu_bpgrp_add`

Add a new breakpoint group. Args: `name`, `parent` (default
`-1`). Returns: `{"id": int}` (= `-1` on cycle / missing parent).

### `emu_bpgrp_remove`

Remove a group. Cascading delete / reparent of children
(breakpoints + sub-groups) is handled by the backend
(`breakpoints.c`). Args: `id`. Returns: `{"removed": bool}`.

### `emu_bpgrp_update`

Selectively update group fields. Args: `id`, `fields` (subset of
`"enabled"` / `"name"` / `"colors"` / `"parent"`), `enabled`,
`name`, `bg_rgb`, `fg_rgb`, `parent`. Returns: `{"updated": bool}`.

## Stack analytics tools

Wrappers around the SP history ring buffer (= sequence of SP values
over time) and named stack regions (= labeled address ranges with
push/pop counters + watermark).

### `emu_stack_history_enable`

Enable/disable SP history recording. Disabling additionally flushes
the ring buffer (= the next enable starts with a clean state). The
active flag wires into a hot-path call site in `mzarch.c` (= zero
overhead while OFF, the default).

Args: `enabled` (bool). Returns: `{"enabled": bool}`.

### `emu_stack_history_reset`

Flush the ring buffer without disabling the recording flag (= UI
"Reset history" button before observing a specific code path).
Returns: `{"reset": true}`.

### `emu_stack_regions_add`

Add a monitored region. Args: `name` (max 31 chars), `base` (top =
highest addr), `limit` (bottom; `base > limit`). Validation in
`stack_regions_add` (= overlap, duplicate). Returns: `{"index":
int, "added": bool}` (= `-1` on invalid input).

### `emu_stack_regions_edit`

Edit the region at a given index. Args: `index`, `name`, `base`,
`limit`. On success the backend also resets the watermark + push/
pop counters (= old stats do not match the new range). Returns:
`{"updated": bool}`.

### `emu_stack_regions_remove`

Remove a region. Args: `index`. Returns: `{"removed": bool}`.

### `emu_stack_regions_reset_watermark`

Reset watermark + push/pop counters of a single region. The region
configuration (name/base/limit) is preserved. Args: `index`.
Returns: `{"reset": bool}`.

## Eventlog tools

Eventlog (= Event Viewer / TLOG) is a ring buffer for capturing the
chronological stream of events (raster, IRQ ACK, IORQ IN/OUT, MMIO
R/W, GDG mode/banking/scroll/colors/video, PIO 8255 / CTC 8253 / PIO
Z80 events, PSG, FDC, MemExt, BP fire, user marks, etc.). Key for AI
client analysis of emu behavior without the GUI Event Viewer window.

Filter: 64-bit bitmask per category (= `en_EVENTLOG_CATEGORY` in
`eventlog.h`). The client sets the mask before start, then emits
respect the filter.

### `emu_eventlog_start`

No params. Starts recording (= subsequent emits on the emu thread are
written into the ring). Returns: `{"started": bool}`.

### `emu_eventlog_stop`

No params. Stops recording, ring content preserved. Returns:
`{"stopped": bool}`.

### `emu_eventlog_clear` (sensitive)

No params. Clears the ring buffer (= loses capture history). Returns:
`{"cleared": bool}`.

### `emu_eventlog_set_capacity` (sensitive)

Args: `capacity` (= requested ring size in events). Backend clamps to
`[EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY]` (= 10000..200000).
Resize re-allocates and discards current content.

Returns: `{"capacity_after": int}` (= actual size after clamping).

### `emu_eventlog_set_mask` (sensitive)

Args: `mask` (= 64-bit bitmask, bit i enables category i). Accepts
**int** (= max 63 bits due to signed gint64) or **hex string** (=
full 64-bit range including bit 63, format `"0xN..."` or `"N..."`,
supports `_` as visual separator).

Returns: `{"mask_hex": "0xN..."}` (= 16-digit zero-padded hex string
for unambiguous round-trip).

### `emu_eventlog_get_event`

Args: `idx` (= 0 = oldest, capacity-1 = newest). If `idx >= count`:
`{"available": false, "idx": int}`. Otherwise returns full 24 B
record:

```json
{
  "available": true, "idx": int,
  "pxclk_total": int,        // 64-bit pixel clock total since power-on
  "screens_total": int,      // 32-bit frame counter
  "pxclk_in_screen": int,    // 32-bit pixel clock pos in current frame
  "category": int,           // 8-bit en_EVENTLOG_CATEGORY
  "subtype": int,            // 8-bit category-specific subtype
  "pc": int,                 // 16-bit Z80 PC at emit
  "payload": int             // 32-bit category/subtype-specific data
}
```

Decoding of `payload` per category is in `eventlog.h` (= client
replicates the mapping or fetches it from the knowledge base).

## Direct memory region tools

### `emu_regions_list`

No params. Enumerate all physical memory regions of the current
architecture (= equivalent of the GUI Memory browser for headless
debug).

Returns:

```json
{
  "regions": [
    {"id": 0, "kind": "logical", "name": "Z80 view",
     "logical_base": 0, "size": 65536, "writable": true,
     "connected": true, "mapped_now": true},
    {"id": 1, "kind": "ram", "name": "User RAM (64 KB)",
     "logical_base": null, "size": 65536, ...},
    {"id": 2, "kind": "rom_lower", "name": "Monitor ROM (lower)",
     "logical_base": 0, "size": 4096, "writable": false, ...},
    {"id": 8, "kind": "memext_ram", "sub_id": 12,
     "name": "Memext RAM bank 12", "size": 4096, ...},
    ...
  ],
  "count": int,
  "layout_gen": int
}
```

`kind` enum: `logical, ram, rom_lower, rom_upper, cgrom, cgram_700,
vram_700_char, vram_700_attr, vram_phys_plane (sub_id 0..3),
pcg_1500 (sub_id 0..2), memext_ram, memext_flash, ramdisk_std,
ramdisk_pezik, prohibited_shadow, device` (`device` = memory of a connected
MZ-700 expansion device, `sub_id` = slot * 16 + region index, see
`emu_get_devices`).

`sub_id` disambiguates regions of the same kind (= plane index, bank
index, PEZIK instance).

The `ram` region ("User RAM (64 KB)") is the 64 KB of RAM as the CPU
would see it if RAM were mapped in the whole address space (ROM and VRAM
do not apply). Without Memext it is the built-in DRAM. With Memext
connected it is the currently mapped Memext banks, not the built-in DRAM
(the CPU does not see that one). It can therefore differ from
`memory/ram.bin` in a `.mzs` snapshot, which always holds the built-in
DRAM - details in `emulator://docs/memory_layout` (Memext section).

**ID stability**: an `id` is an index into the list and stays valid
until the hardware configuration changes (RAMdisk, MemExt, PEZIK,
loading a snapshot with a different configuration). `layout_gen`
identifies the layout of the list; it changes whenever the list changes.
`emu_region_read` and `emu_region_write` check it: with an `id` from an
older list they fail with `stale region id: ...` and touch nothing. Call
`regions_list` again for new IDs. To keep an address across such
changes, store `kind` and `sub_id`, not `id`.

### `emu_region_read`

Raw no-side-effect read from a specific region (= bypass Z80
banking). No auto-inc latch, no GDG RF dispatch, no IRQ trigger.

Args:
- `region_id` (= from the last `regions_list` call)
- `offset` (= byte offset within the region, 0..size-1)
- `length` (= 1..65536, clamped to region size if
  `offset+length > size`)
- `want_data` (bool, optional, default true) - false = only the hash
- `layout_gen` (int, optional) - `layout_gen` of the list the `region_id`
  comes from; default = the last `regions_list` call of any MCP client.
  A different layout returns the error `stale region id: ...`.

Returns:
```json
{
  "region_id": int,
  "offset": int,
  "length": int,        // actually-read length after clamping
  "sha256": "...",      // SHA-256 of the bytes read (64 lowercase hex)
  "data_b64": "..."     // base64-encoded raw bytes (not with want_data=false)
}
```

Main use case: AI client in headless mode (= no GUI access) inspects
emu physical memory - ROM monitor content, VRAM bitmap planes,
MemExt banks.

### `emu_region_write` (sensitive)

Raw write into a physical memory region (= bypass Z80 banking), the
counterpart of `emu_region_read`.

Args:
- `region_id` (= from the last `regions_list` call)
- `offset` (= byte offset within the region, 0..size-1)
- `data_b64` (= base64 bytes, 1..65536 bytes after decoding; clamped at
  the end of the region)
- `layout_gen` (int, optional) - as for `emu_region_read`; with an `id`
  from an older list nothing is written
- `expected_sha256` (string, optional) - conditional write: write only
  if the SHA-256 of the bytes being overwritten (after clamping at the
  region end) still equals this value, e.g. the `sha256` of
  `emu_region_read` or of an `emu_state_observe` region window of the
  same range; otherwise `Precondition failed: memory changed (...)` and
  nothing is written. Errors of the id, `layout_gen` and read-only
  regions come first.
- `want_hashes` (bool, optional) - return the hashes also without a
  condition

Returns `{"region_id", "offset", "length", "written"}` - `length` is the
number of bytes actually written (after clamping), `written` = at least
one byte was written; with a condition or `want_hashes` also
`before_sha256` and `after_sha256`.

Read-only regions (`memext_flash`, `prohibited_shadow`), a disconnected
region or an invalid `region_id` / `offset` return a parameter error
`region write failed: read-only, disconnected, or invalid
region_id/offset`. ROM regions are writable as a debug fixture: the patch
lasts until the ROM is reloaded (e.g. after a reset); the ROM file on disk
does not change. The `logical` region (Z80 view) writes through the
current banking with the usual side effects (e.g. a VRAM write goes
through the GDG write format) - for editing VRAM use the physical plane
regions. A VRAM change shows on the next frame; a paused GUI needs a
refresh.

### `emu_file_load_to_region` (sensitive)

Loads a file into a memory region without base64 on the client side
(Python composite: file read + `emu_region_write`).

Args: `path`, `region_id`, `dest_offset` (default 0), `src_offset`
(skip N bytes of the file, default 0), `length` (bytes to copy, default
-1 = to the end of the file).

More than 65536 bytes is an error (`length N > 65536; split into multiple
calls`) - copy a large file in parts. On success returns the reply of
`emu_region_write` plus `src_file` and `src_offset`.

### `emu_file_save_from_region`

Saves a part of a memory region to a file (Python composite:
`emu_region_read` + file write) - ROM dumps, ramdisk backups, raw VRAM.

Args: `path` (overwritten), `region_id`, `src_offset` (default 0),
`length` (default 4096, max 65536).

Returns `{"region_id", "src_offset", "length", "dest_file"}`; `length` is
the number of bytes actually saved (clamped at the end of the region).

## Memory search and Freeze Bytes tools

Variable search (cheat search, RAM search) over memory regions: find
lives, score, timers or coordinates by narrowing a set of candidate
positions ([Memory Search](../debugger/memory-search.md)). The recipe is
in the MCP document `emulator://docs/memsearch_workflow`.

A session holds the candidates and three samples of the space: the
first sample of the search, the last one and the one before it. A sample
is taken by the emulator thread - while the emulation runs at the next
frame boundary, when paused at once - consistently for the whole space.
MCP sessions are separate from the GUI window; at most 8 are open, the
MCP server closes them when it stops.

### `emu_memsearch_start`

Arguments:

- `space` (default `"ram"`) - preset `ram` (User RAM 64 KB), `logical`
  (what the CPU sees now, incl. ROM and VRAM), `vram` (VRAM 700 text /
  attributes and the MZ-800 VRAM planes), `memext` (MemExt RAM banks),
  `ramdisk` (RAM disk STD and PEZIK banks), `all_writable` (all of these
  without `logical`); or a list of `{"kind", "sub_id", "offset",
  "length"}` (kinds as in `emu_regions_list`, `length` defaults to the
  end of the region). ROM, CG-ROM and FLASH are never searched.
- `type` (default `u8`) - `u8`, `s8`, `u16`, `s16`, `u24`, `s24`, `u32`,
  `s32`, `bcd` (packed, 2 digits per byte), `digits` (one digit per byte).
- `width` - bytes for `bcd` (1-4, default 2) and `digits` (1-8, default 4).
- `endian` - `le` (default for integers and BCD) or `be` (default for
  digits = print order).
- `aligned` - only offsets divisible by the width.
- `digit_encoding` - `raw` (00h-09h), `ascii` (30h-39h), `display`
  (Sharp MZ display code 20h-29h, default); `leading_space` (default
  true) reads leading spaces (ASCII 20h, display code 00h) as zeros.
- `op`, `value`, ... - optional first filter, value comparison only.
- `allow_large` - allow a space over 4 MiB (up to 32 MiB).

Returns `{"session_id", "space_size", "spans": [{"kind", "sub_id",
"offset", "length", "connected"}], "candidates", "step", "sample_frame",
"undo_depth", "redo_depth"}`.

### `emu_memsearch_filter`

Arguments: `session_id`, `op`, `value`, `compare_to`, `value2`, `mask`,
`negate`, `sample` (default true).

| `op` | Keeps the candidate when |
|------|---------------------------|
| `eq`, `ne`, `lt`, `le`, `gt`, `ge` | the value compares so with the reference |
| `between` | `value` <= value <= `value2` |
| `inc`, `dec` | the value grew / dropped against the reference |
| `inc_by`, `dec_by`, `changed_by` | it changed by exactly `value` |
| `changed`, `unchanged` | it changed / did not change |
| `mask_eq` | the bits under `mask` are equal (integer types) |

`compare_to`: `value`, `previous` (default for inc/dec/changed/...) or
`first`. With `sample: true` a new sample is compared with the last one;
with `sample: false` the last sample is compared with the one before it.
`negate` inverts the condition; an invalid BCD or digit value is removed
anyway. Arithmetic has no wrap-around (an u8 going 00h -> FFh is not
`dec_by 1`). Candidates in a region that was disconnected stay unchanged.

Returns `{"removed", "candidates", "step", "sample_frame", ...}`.

### `emu_memsearch_sample`, `emu_memsearch_undo`, `emu_memsearch_redo`

New sample without filtering / undo / redo the last step. Undo also
returns the reference samples (a filter after an undo compares with the
right previous value); undoing the start leaves the session without a
sample (`"started": false`).

### `emu_memsearch_list`

Arguments: `session_id`, `offset` (index of the first candidate),
`limit` (1-1000, default 100). Each item:

| Field | Meaning |
|-------|---------|
| `index`, `space_offset` | Order of the candidate, position in the search space (for `emu_memsearch_eliminate`) |
| `kind`, `sub_id`, `region_offset` | The cell (for `emu_freeze_add`, `emu_region_read`) |
| `cur`, `prev`, `first` | Decoded values (null = invalid BCD / digits or disconnected region) |
| `cur_hex` | Bytes in address order |
| `connected` | The region was connected at the last sample |
| `z80_addr` | CPU address (for `emu_watch_add`), null for a VRAM plane, CG-RAM, PCG, RAM disk or an unmapped MemExt part |
| `bp_hint` | `{"addr", "fields", "values"}` for `emu_bp_create_with_init`: a MEM_W breakpoint in zone `RAM` (User RAM), `VRAM_FB` (VRAM 700), `CPU_VIEW` (Logical, mapped Luftner part) or `MMEXT_BANK` with `bp_addr_space: "bank_offset"` (PEHU, bank = part / 2); a range for a value wider than 1 byte; null where a write breakpoint cannot match the cell |

### `emu_memsearch_eliminate`, `emu_memsearch_close`

`emu_memsearch_eliminate(session_id, space_offsets)` removes chosen
candidates (undoable). `emu_memsearch_close(session_id)` frees the
session.

### `emu_freeze_add` (sensitive)

Arguments: `kind`, `sub_id` (default 0), `offset` and either `value`
(0-255) or `bytes` (1-64 values from `offset` on). The bytes are written
back once per emulated frame (Freeze Bytes, at most 256 bytes): the
program sees its own write until the end of the frame. Returns
`{"added", "frozen"}`.

### `emu_freeze_remove`, `emu_freeze_list`

`emu_freeze_remove(kind, offset, length, sub_id)` or
`emu_freeze_remove(all=True)` returns `{"removed"}`; `emu_freeze_list`
returns `{"count", "max": 256, "entries": [{"kind", "sub_id", "offset",
"value"}]}`, including bytes frozen in the GUI.

## Speed control tools

Control of the emulation tempo (= equivalent of the GUI Speed menu).
Main use case: let a long operation (boot ROM, cassette/disk load, long
computation) finish quickly via max speed (warp), then back to 100%.
A tempo change is a visible action (= appears in the Activity log as an
MCP action).

The current speed state is read via the `emulator://speed` resource.

### `emu_set_speed`

Set the emulation speed according to `mode`.

Args:
- `mode` (required): one of
  - `normal` - run at 100% (real-time tempo, turns warp off)
  - `custom` - run at an exact `percent` % (turns warp off)
  - `max` - warp / unthrottled, as fast as the host allows (turns warp on)
  - `step` - relative adjust of the custom % by `step` (warp unchanged)
- `percent` (default 100): target % for `mode=custom` (1..4000, clamped
  by the core)
- `step` (default 0): relative delta for `mode=step` (positive = faster,
  negative = slower, 0 = no-op)

Returns:
```json
{
  "ok": true,
  "mode": "max",           // resulting mode (max/custom/normal)
  "current_percent": 100,  // current % after the operation
  "max_speed": true        // warp flag after the operation
}
```

Note: `emu_run(frames=N)` waits for N frames; with max speed the frame
counter advances faster, so a blocking `emu_run` finishes in less
wall-clock time (behaviour stays correct, just faster).

### `emu_speed_step`

Convenience wrapper around `emu_set_speed(mode="step", step=delta)`.

Args:
- `delta`: relative delta in % (positive = faster, negative = slower)

Returns: the same echo object as `emu_set_speed`.

### Example workflow - warp through boot

```python
emu_set_speed(mode="max")        # turn warp on
emu_run(frames=600)              # let boot/load finish quickly
emu_set_speed(mode="normal")     # back to 100%
```

## CMT cassette tape tools

Control of the Sharp MZ cassette tape (CMT). There are two distinct
concepts, kept separate on purpose:

- **Real tape** (`emu_cmt_play`, `emu_cmt_play_paused`, `emu_cmt_stop`,
  `emu_cmt_pause`, `emu_cmt_eject`, `emu_cmt_record`) drives the
  cycle-accurate emulated cassette. The program reads/writes the tape
  signal through the normal Sharp ROM routine, exactly like physical
  hardware. This is the accurate path and works with any program.
- **cmthack** (`emu_cmt_hack_set`) is a ROM-patch shortcut (instant
  load): the patched ROM load routine copies a tape file straight into
  RAM, skipping the tape signal. It is faster but only works for
  programs that load through the patched ROM entry points.

All of these change emulator state (sensitive) and appear in the
Activity log as MCP actions. The current tape state, including
`cmthack_enabled`, is read via the `emulator://periph/cmt` resource.

A tape image must be inserted first via `emu_media_insert(slot="cmt",
path=...)` before playback.

### `emu_cmt_play` (sensitive)

Start real cassette playback (transport PLAY). No-op if no tape is
loaded or playback cannot start in the current state.

Returns: `{"ok": true, "action": "play"}`.

### `emu_cmt_play_paused` (sensitive)

Same as `emu_cmt_play` but the tape begins paused; call
`emu_cmt_pause(paused=False)` to advance it.

Returns: `{"ok": true, "action": "play_paused"}`.

### `emu_cmt_stop` (sensitive)

Stop the transport (PLAY or RECORD -> STOP). No-op if already stopped.
Without a loaded tape it still puts the transport into STOP.

Returns: `{"ok": true, "action": "stop"}`.

### `emu_cmt_pause` (sensitive)

Pause (`paused=True`, default) or resume (`paused=False`) the running
transport without resetting its position.

Args:
- `paused` (default true): true = pause, false = resume.

Returns: `{"ok": true, "action": "pause"}`.

### `emu_cmt_eject` (sensitive)

Stop the transport (if running) and remove the loaded tape image.
Scoped equivalent of `emu_media_eject(slot="cmt")`.

Returns: `{"ok": true, "action": "eject"}`.

### `emu_cmt_record` (sensitive)

Start recording the real cassette output to a WAV file. Recording
starts paused; call `emu_cmt_pause(paused=False)` to begin capturing.
Only WAV output is supported. Requires the tape to be in the STOP
state and the path to be writable.

Args:
- `path` (required): target WAV file path (must be writable).

Returns: `{"ok": true, "path": "<path>"}`. Fails (`error`) on bad state
or a path that is not writable.

### `emu_cmt_hack_set` (sensitive)

Enable or disable the cmthack ROM patch (instant tape load). NOT the
real tape - see the note above.

Args:
- `enabled` (required): true = install the ROM patch, false = remove it.

Returns: `{"ok": true, "installed": <bool>}` echoing the patch state
after the operation.

### Example workflow - record real tape output

```python
emu_media_insert(slot="cmt", path="game.mzf")  # source tape
emu_cmt_record(path="out.wav")     # arm recording (starts paused)
emu_cmt_pause(paused=False)        # begin capturing
# ... run the program ...
emu_cmt_stop()                     # finish, flush WAV
```

### `emu_cmt_set_speed` (sensitive)

Sets the default speed of the virtual cassette: a ratio relative to
1200 Bd, or custom pulse lengths. Accepts a ratio string key or the
en_CMTSPEED int: `1:1` (1), `2:1` (2), `2:1_cpm` (3), `3:1` (4),
`3:2` (5), `7:3` (6), `8:3` (7), `9:7` (8), `25:14` (9), `custom`
(10 = the custom pulse lengths stored earlier). Instead of `speed` you
can pass `pulses_us` = `[long_high, long_low, short_high, short_low]` in
microseconds (long pulse = bit 1, short pulse = bit 0), each in
(0, 65535]; the order is that of the UniCMT CMTSPEED header (UniCMT 3x =
`[156, 164, 80, 92]`). The lengths are stored (also in the
configuration) and the default speed switches to `custom`. Changes the
default speed; per-block override via `emu_cmt_tape_set_block_speed`.
Reflected as `cmtspeed`, `default_pulses_us` and `custom_pulses_us` in
`emulator://periph/cmt`.

Args:
- `speed` (optional): ratio string ("2:1") or int (2).
- `pulses_us` (optional): 4 pulse lengths in microseconds (instead of `speed`).

Returns: `{"ok": true, "property": "speed", "value": <int>}`, for
`pulses_us` `{"ok": true, "property": "custom_pulses", "pulses_us": [...]}`.

### `emu_cmt_set_polarity` (sensitive)

Sets the tape signal polarity (rear DIP switch). Reflected as
`polarity_inverted`.

Args:
- `inverted` (required): true = inverted, false = normal.

### `emu_cmt_set_cpu_boost` (sensitive)

Enables/disables CPU boost during tape transport (run at max speed for
fast long loads). Reflected as `cpu_boost`.
User preference: snapshot load does not restore it (snapshots still
store it for older emulator versions).

Args:
- `enabled` (required): true = boost, false = real time.

### `emu_cmt_set_mzfsize_check` (sensitive)

Enables/disables the MZF size consistency check on load (body size vs
header file size). Reflected as `mzfsize_check`.

Args:
- `enabled` (required): true = enforce, false = skip.

### `emu_cmt_open` (sensitive)

CMT-specific open by file extension (.mzf/.mzt/.wav/...). Unlike
`emu_media_insert(slot="cmt")` it can start playback directly via
`play_immediately` (= one round trip instead of open + play). Real tape,
not cmthack.

Args:
- `path` (required): tape file path (extension selects the backend).
- `play_immediately` (default false): start playback after opening.

Returns: `{"ok": true, "path": <str>, "playing": <bool>, "state": <str>,
"paused": <bool>}`. `playing`, `state` (`"stop"` / `"play"` / `"record"`)
and `paused` report the actual transport state after the operation (same
meaning as in `emulator://periph/cmt`); `playing` is true only for PLAY
without pause. If `play_immediately` was requested but the tape is not
playing afterwards, the result also contains `warning`.

### `emu_cmt_tape_seek` (sensitive)

Seeks to a tape block (SIMPLE_TAPE multi-block containers: .mzt,
.tap). A single-file container (SINGLE: a plain .mzf or .wav) does not
support seeking - the call fails even for block 0; to start it from
the beginning again, use `emu_cmt_stop` + `emu_cmt_play`. Requires a
loaded tape. Block listing is in `emulator://periph/cmt/tape`.

Args:
- `block_id` (required): 0-based block index.

Returns: `{"ok": true, "block_id": <int>}`.

### `emu_cmt_tape_set_block_speed` (sensitive)

Per-block speed (cmt speed only, no other parameters; SIMPLE_TAPE).
`speed` accepts the same ratio keys / int 1..9 as `emu_cmt_set_speed`,
`pulses_us` custom pulse lengths of the block (MZ blocks only, the block
then has `speed` 10). Requires a loaded tape. The speed applies from the
next opening of the block (advancing to it during playback,
`emu_cmt_tape_seek`); the currently open block is not changed.

Blocks after a CMTSPEED header (UniCMT) in an `.mzt` tape already have
their own speed from the header when the tape is opened; the header
itself is not played and is not listed. The listing
`emulator://periph/cmt/tape` reports `block_speed` ("default" / "set" /
"none") and the effective `pulses_us` of each block.

Args:
- `block_id` (required): 0-based block index.
- `speed` (optional): ratio string ("2:1") or int (2).
- `pulses_us` (optional): 4 pulse lengths in microseconds (instead of `speed`).

Returns: `{"ok": true, "block_id": <int>, "speed": <int>}` (with
`"pulses_us": [...]` for `pulses_us`).

### Example workflow - multi-block tape

```python
# see emulator://periph/cmt/tape for container_type and block list
emu_cmt_open(path="tape.mzt")
emu_cmt_tape_seek(block_id=2)       # pick the third program
emu_cmt_play()
```

The full workflow (real tape vs cmthack, transport, blocks) is in the
resource `emulator://docs/cmt_workflow`.

## Related

- [Python wrapper](python-wrapper.md) - how to call these tools
  from Claude Code
- [Configuration](configuration.md) - security profile, sensitive
  tool gating
- [Resources overview](resources-overview.md) - read-only URI
  endpoints complementing tools
