# MCP docs - index

Reference documentation for AI clients of the mz800emu MCP server.
Read individual topics on demand via `resources/read`.

## Topics

| URI | What to look up here |
|-----|----------------------|
| `emulator://docs/error_handling` | How to read a failed tool response: which errors mean "not executed, safe to retry" (`Emulator busy: ...`), "partially executed" or a real failure; request timing, the 10 s queue timeout and the ~20 s limit for a started command. |
| `emulator://docs/run_control` | Run control: start a run and wait until it stops (`emu_wait_stop`, `run_seq`, events `paused` / `resumed` with reason, PC, who started the run), deadlines in emulated time (`deadline_frames` / `deadline_ms`), `emu_pause(if_run_seq)` so as not to stop the user's run, `completed` of steps, 20 s limit of stepping loops, SHA-256 of memory and of the screen (`want_data=false`, `emu_frame_hash`). |
| `emulator://docs/state_observe` | Registers, flags, run number, emulated time and up to 16 memory windows (CPU addresses or physical regions by kind and `sub_id`) from **one moment**, with SHA-256 per window (`emu_state_observe`); optional raster, memory map and state part hashes. Use instead of separate register and memory reads while the emulator runs. |
| `emulator://docs/conditional_writes` | Write only if the memory or register still holds what you read: `expected_sha256` on `emu_mem_write` / `emu_mem_write_force` / `emu_region_write`, `expected` on `emu_set_register` / `emu_set_cpu_flags`; `Precondition failed:` errors, order of checks, banking weakness of CPU addresses. |
| `emulator://docs/replay` | Record a run once and repeat it exactly: `emu_replay_record_start` / `_stop`, `emu_replay_play` (also in a new process, any speed), `emu_replay_status`; what is recorded, writes only at frame boundaries, refused tools, `deviated`, first differing frame (`replay_desync`, `desync_parts`), `inputs.jsonl` format, CI check `--replay-verify` (exit code 4). |
| `emulator://docs/memory_layout` | Z80 64 KB address space layout per platform (MZ-700 / MZ-800 native / MZ-800 700-compat / MZ-1500). Banking ports `0xE0..0xE6`. Memory expansion (Memext) replacing the RAM, memory files inside a `.mzs` snapshot. Key ROM entry points, IRQ vectors, text/color VRAM regions. |
| `emulator://docs/bp_dsl` | Breakpoint condition expression language: registers, memory deref `[addr]`, port read `port[N]`, operators, built-in functions, `$user_vars`, examples. Same language is used by Watch expressions and Action DSL arguments. |
| `emulator://docs/smart_vars` | `$name` user variables: how to create / read / write from BP action DSL, persistence rules (`.bpt` + `.vars`), lifecycle, reading via `emulator://vars`. |
| `emulator://docs/action_dsl` | BP Action DSL grammar - what runs when a breakpoint fires. Commands: `log`, `set <reg>`, `poke`, `mark`, `$var` writes, `if/then/else`, `enable` / `disable` / `disable_self`, `clear_vars`, `continue`, `stop`, `exit`, plus the forwarding commands `cdl_start` / `cdl_stop` / `cdl_reset` / `cdl_export`, `trace_start` / `trace_stop` / `trace_save`, `snapshot`, `ramlog_*`, `coverage_start` / `coverage_stop` / `coverage_mark` (one smart BP instead of repeated MCP calls). `log` format specs (the text is readable via `emu_trace_read` channel `bplog`), stop vs continue semantics. |
| `emulator://docs/watch_dsl` | Watch expression syntax: three watch modes (`address` / `expr_scalar` / `expr_deref`), supported type tags (`u8`, `i16le`, `ascii`, ...), reading via `emulator://watch`. |
| `emulator://docs/eventlog_mask` | EventLog 64-bit category mask: bit assignments per `en_EVENTLOG_CATEGORY`, common mask recipes ("memory writes only", "IRQ only"), how to pass via `emu_eventlog_set_mask`. |
| `emulator://docs/sharp_display_code` | Sharp MZ ASCII vs display code vs standard ASCII. Mapping tables, inverse video, decoding `emulator://video/text_dump`. |
| `emulator://docs/mz800_keyboard` | Sharp MZ-800 keyboard: physical layout, per-key matrix position + emitted characters, modifiers (SHIFT / CTRL / GRAPH / ALPHA), the four character layers, canonical key names for the `emu_input_send_keys` injection tool, recipes for typing graphics / semigraphics. MZ-700 / MZ-1500 as a diff (no TAB, longer ALPHA). |
| `emulator://docs/cmt_workflow` | CMT cassette workflow: real tape (`emu_cmt_*`) vs cmthack instant load, starting a program in a defined state (`emu_media_run_mzf`), transport flow, WAV recording, speed ratios / polarity / cpu boost / mzfsize check, SIMPLE_TAPE multi-block seek + per-block speed. Cross-refs `emulator://periph/cmt` and `emulator://periph/cmt/tape`. |
| `emulator://docs/videorec_workflow` | Recording a gameplay video (all builds): `emu_videorec_*` tools, recipe start -> play with keystrokes -> markers -> snapshot retake -> stop, error messages, export of the recording to MP4. |
| `emulator://docs/trace_read` | Reading trace records page by page: `emu_trace_status` / `emu_trace_read` for cputrack, iorqlog, intlog, hwlog, marklog, the Event Viewer ring and the breakpoint action output `bplog` (text of `log` / `stop`) - recipe, cursor, filters, character budget, columns (time `t`, `asm` from recorded bytes, `INT ACK`), `segment_replaced` and `gap`. |
| `emulator://docs/ramlog` | Who wrote a byte: RAM write log `emu_ramlog_start` / `_who_wrote` / `_writers` / `_stop`, channel `ramlog` of `emu_trace_read` - recipe, record fields (`pc` of the writing instruction, physical cell, old / new, source), banking and VRAM planes, hidden unchanged writes, last-writer map after ring wrap. |
| `emulator://docs/coverage` | Which code ran and which branches were taken: code coverage `emu_coverage_start` (`branch` by PC, `detail` by physical cell and code version) / `_read` / `_status` / `_mark` / `_export` - recipe, branch states (`taken_only`, `not_taken_only`, `both`), self-modifying code and banking, completeness, epochs ("what is new after a key press"), cursor. |
| `emulator://docs/memsearch_workflow` | Find a variable (lives, score, timer) without disassembling: `emu_memsearch_start` / `_filter` / `_list` (cheat search over User RAM, VRAM, MemExt, RAM disks; u8-s32, BCD, digits in display code), which filter for which situation (irregular timers: `le` previous + `lt` first), then `emu_watch_add`, `emu_bp_create_with_init` with `bp_hint`, `emu_freeze_add`. |
| `emulator://docs/audio_capture_read` | Analysing sound without hearing it: audio capture memory read via MCP - `emu_audio_capture_summary` (silence, PSG notes, speaker tone or PCM estimate, reference vs output path delay and difference), `emu_trace_read` channels `audio_psg_writes` / `audio_levels` / `audio_frames`, `emu_audio_capture_pcm` (short WAV window, exact 44100 Hz copy); recipe, memory limits, what is heuristic. |
| `emulator://docs/unattended_runs` | Unattended runs for CI without an MCP client: `--headless` with `--exit-on-pc` / `--fail-on-pc` / `--exit-on-write` / `--exit-on-out` / `--exit-after-frames` / `--timeout-*` / `--bpt`, exit codes 0-4 (4 = a replay differs, `--replay-verify`), `result.json` (reason, time, registers, SHA-256 hashes, artifacts); the breakpoint action `exit` in an MCP session only pauses (`stopped_by: "exit"`). |
| `emulator://docs/screenshot_workflow` | Saving and comparing pictures of the screen: `emu_screenshot_save_to_file` with `area` (full / canvas), `scale` (framebuffer / native dots / window 1-4) and `crop`, picture sizes per build, recipes (what the program drew, a part of the screen, a picture for a person); frame series `emu_screenshot_series_*` - every changed emulated frame as PNG for finding animation bugs. |

## When to read what

- A tool returned an `error` and you are deciding whether to retry ->
  `error_handling`.
- Running the emulator and waiting for it to stop, limiting a run in
  emulated time, comparing memory or the screen by hash -> `run_control`.
- Reading registers and memory that must belong together (a pointer and
  its data) while the emulator runs -> `state_observe`.
- Writing memory or a register only if nobody changed it since you read
  it (a person in the GUI, the running program) -> `conditional_writes`.
- Repeating a run exactly (a bug with other breakpoints, trace, ramlog),
  finding the first frame where two runs differ, CI check of a recorded
  run -> `replay`.
- Setting up a breakpoint with a non-trivial condition -> `bp_dsl`.
- Writing the action that runs when a BP fires (log, poke, set,
  conditional) -> `action_dsl`.
- Need a hit counter / state machine across BPs -> `smart_vars`.
- Adding a memory observation in the Watch panel -> `watch_dsl`.
- Tuning EventLog filter to capture only interesting events ->
  `eventlog_mask`.
- Decoding text from `emulator://video/text_dump` or `emu_mem_read` of
  the text VRAM region -> `sharp_display_code`.
- Reading / writing memory and want to know what region is what, or
  reading RAM from a `.mzs` snapshot (Memext!) -> `memory_layout`.
- Loading a program from cassette, starting a program in the same state
  as after a tape load, recording to tape, or working with a multi-block
  tape -> `cmt_workflow`.
- Injecting keystrokes via `emu_input_send_keys` and need the exact key names,
  or want to type Sharp graphics / special characters -> `mz800_keyboard`.
- Recording a video of a game (with chapters and retakes) and exporting it
  for YouTube -> `videorec_workflow`.
- Reading what the CPU executed, which ports it touched, interrupts or
  Event Viewer events after a run -> `trace_read`.
- Logging registers or values every time a breakpoint fires ("SP at every
  entry into X") without a full trace -> breakpoint action `log`
  (`action_dsl`) and channel `bplog` (`trace_read`).
- Finding out which code wrote a value into memory, or which code writes
  into an area -> `ramlog`.
- Finding untested code or conditional jumps that went only one way,
  which code a key press or a level adds, or which version of
  self-modifying code ran -> `coverage`.
- Finding where a program keeps a value (lives, score, a timer) or making
  it constant (cheat) -> `memsearch_workflow`.
- Finding out what a program plays (notes, silence, speaker PCM) or whether
  the audio output differs from the emulated sound -> `audio_capture_read`.
- Preparing a test run for CI or a script that ends by itself with an exit
  code and a result file, or a breakpoint action `exit` stopped your MCP
  session with `stopped_by: "exit"` -> `unattended_runs`.
- Checking what a program draws, comparing the screen with expected pixels
  or saving a part of the screen, or catching an animation bug frame by
  frame (frame series) -> `screenshot_workflow`.

## Live state resources (not docs)

These return current runtime state, not documentation. Read them for
context before / during analysis:

- `emulator://state` - paused / running, frame, transport.
- `emulator://platform/info` - architecture, mode, clocks, scanline geometry.
- `emulator://cpu/registers` - Z80 register snapshot.
- `emulator://memory/map` - current 16 x 4 KB banking snapshot.
- `emulator://memory/{addr_hex}/{length}` - template URI to read a memory range.
- `emulator://breakpoints`, `emulator://watch`, `emulator://vars` - debugger state.
- `emulator://video/text_dump` - 40x25 text VRAM dump (when applicable).

The full live list is in `emulator://config/peripherals` and is also
discoverable via the standard `resources/list` method.

## User knowledge base (`emulator://kb/*`)

If the user configured a knowledge base directory, their own Markdown
notes appear under the separate `emulator://kb/<topic>` namespace
(discoverable via `resources/list`). These are user-authored and not
part of this project reference; read them when present for
machine-specific or task-specific context.
