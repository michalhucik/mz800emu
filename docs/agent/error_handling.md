---
title: MCP error handling and retry rules
description: How to read failed tool responses - which errors mean "not executed, safe to retry", "partially executed" or a real failure, plus request timing expectations.
---
# MCP error handling and retry rules

Every request gets exactly one response. On failure the tool result
carries an `error` string (raw JSONL: `"success": false`, `"error"`).
The error text tells you whether the emulator **executed** the command,
which decides whether retrying is safe.

Through the bundled MCP server there are two shapes (as observed with the
MCP Python package 1.27; the argument check is done by that package, not
by the emulator, so another version may differ):

- **The tool ran and failed**: a normal result (`isError` false) whose
  text is the JSON `{"error": "..."}`; `structuredContent` is
  `{"result": "<the same JSON as a string>"}` (every tool's output is
  one string), so parse the text or the `result` string, not the object.
- **An argument cannot be converted to the tool's parameter type** (for
  example `1.5` or `"abc"` where an integer is expected, a missing
  required argument): the MCP library refuses the call before the tool
  runs, with `isError` true and a plain text message `Error executing
  tool <name>: ... validation error ...`, no JSON. The check is lenient:
  it converts what it can (`true` to 1, `"2"` to 2, `2.0` to 2) and the
  tool then runs with the converted value, so pass the exact JSON
  types.

## How a request is processed

Most tools are forwarded to the emulator thread through an internal
command queue. The emulator thread services the queue once per emulated
frame while running, and immediately while paused. The request waits up
to **10 s** for the emulator thread to pick the command up. Once picked
up, the command always runs to completion and its real result is
returned - a started command is never reported as failed just because
it took long. There is one upper bound: if a picked-up command is still
not finished **10 s after the 10 s queue limit** (so about 20 s after the
request at most), the request returns `Emulator busy: command still
running` instead of waiting forever; the emulator keeps processing other
requests meanwhile.

Commands that legitimately run long in the emulator thread (file I/O or
large data) get a much longer limit of **10 minutes** instead of 10 s:
`trace_stop`, `trace_save`, `snapshot_save`, `snapshot_save_buffer`,
`snapshot_load`, `snapshot_load_buffer`, `profiler_export`, `cdl_export`,
all `videorec_*` tools, `media_load_mzf`, `media_run_mzf`, `media_load_binary`,
`media_insert`, `media_eject`, `cmt_open`, `cmt_record`,
`cmt_tape_import`, `cmt_export_tape`, `cmt_tape_analyze`,
`get_frame_screenshot`, `screenshot_save_to_file` (and the wrapper tools
built on them). The 10 s queue limit applies to them as well. A client
with its own shorter timeout (the bundled Python wrapper waits 30 s per
request, 640 s for `emu_cmt_tape_analyze`) may give up earlier, although
the command still completes.

Pick-up is normally a few ms, but host-dependent stalls of the emulator
thread of up to ~1-2 s have been observed. They are not an error on
your side and need no special handling unless they exceed the 10 s
limit (see below).

## Error classes

| `error` text starts with | Executed? | What to do |
|--------------------------|-----------|------------|
| `Emulator busy: command not executed` | **No** - the command was cancelled before the emulator thread picked it up (10 s timeout). Emulator state is unchanged. | Safe to retry the same request. If it repeats, the emulator thread is blocked (e.g. waiting on a file dialog in the GUI); check `emulator://state`. |
| `Emulator busy: command queue full` | **No** - never queued. | Safe to retry after a short delay. |
| `Emulator busy: command still running` | **Unknown** - the emulator thread picked the command up but did not finish it within the limit (~20 s, 10 min for the long-running commands listed above); it may still complete later (its late result is discarded). Later steps of a multi-step tool are not sent. | Do **not** blindly retry. Wait, then check state (`emulator://state`, the resource the command affects). Repeated occurrences mean the emulator thread is stuck. |
| `Emulator busy: command only partially executed` | **Partly** - the tool consists of several internal steps; an earlier step ran, a later one timed out. | Do **not** blindly retry. Re-read the relevant state first (`emulator://breakpoints`, `emulator://cpu/registers`, ...), then decide. |
| `Emulator is shutting down` | **No** | Stop sending requests. |
| `emulator process exited (...)` / `emulator connection closed (...)` (bundled Python wrapper) | **Unknown** - the emulator process ended (e.g. crashed) or the TCP connection dropped while the request was pending. Pipe mode: all emulator state is lost. TCP mode: only the connection was lost, the GUI emulator may still run with its state. | Do not retry blindly. Pipe mode: the next call starts a fresh emulator and reports `restarted: true`; restore your state (e.g. `emu_snapshot_load`) before continuing. TCP mode: the next call reconnects; re-read the state you depend on. |
| `Precondition failed:` | **No** - the emulator thread checked the condition of a conditional write (`expected_sha256`, `expected`, `expected_state_rev`) and changed nothing: the memory, the register or the machine state differs from what you read. The current value is in the text, e.g. `Precondition failed: memory changed (expected_sha256=..., current_sha256=...)`, `Precondition failed: register HL changed (expected=0x1234, current=0x5678)`, `Precondition failed: state changed (expected_state_rev=..., current_state_rev=...)`, `Precondition failed: emulator running (expected_state_rev=...)`. | Do not retry unchanged. Read again (`emu_state_observe`, `emu_mem_read`), decide whether the write still makes sense, then write with the new hash, value or revision (pause first for `expected_state_rev`). See `emulator://docs/conditional_writes`, `emulator://docs/state_revision`. |
| `Invalid parameters`, `Missing required field: ...`, `Invalid id`, `Unknown command`, `frames must be ...` | **No** - rejected before reaching the emulator. | Fix the request; retrying unchanged fails again. |
| anything else, e.g. `bp_remove failed (unknown id?)`, `Pause failed`, `BP_LIST failed` | **Yes** - the emulator executed the command and it failed (unknown ID, invalid data, ...). | Retrying unchanged does not help. Inspect state (e.g. `emulator://breakpoints` for BP IDs). |

The prefixes `Emulator busy:`, `Emulator is shutting down` and
`Precondition failed:` are a stable contract; match on them, not on the
rest of the sentence. A `Precondition failed:` error carries the current
values only in its text (there is no `data` object on errors). An
`Emulator busy:` error (except `command still running`) ends with the tool's
original message in square brackets,
e.g. `Emulator busy: command not executed (...); safe to retry
[bp_remove failed (unknown id?)]` - the bracketed part is kept for
compatibility and does **not** mean the ID is unknown.

## Notes

- `emu_bp_clear` is best-effort: it removes breakpoints one by one and
  returns `count` = how many were actually removed. If it is lower than
  expected, re-read `emulator://breakpoints`.
- `emu_run` with `frames` blocks until the frames elapse or a
  breakpoint / pause stops the emulator (up to ~40 s for 1000 frames);
  its `stopped_by` field says why. Keep your own client timeout at
  least 30 s (the bundled Python wrapper uses 30 s per request).
- The key and joystick tools with a hold (`emu_input_send_key`,
  `emu_input_send_keys`, `emu_input_send_keys_with_delays`,
  `emu_input_send_joystick`) block until the whole sequence has been
  played in emulated frames (at normal speed 50 frames = 1 s, longer
  when the emulation runs slower). The bundled Python wrapper extends
  its timeout with the sequence length. `interrupted: true` in the
  reply means a breakpoint, a pause or a reset cut the sequence short.
- `emu_run_until_addr`, `emu_snapshot_load` and
  `emu_snapshot_load_buffer` need a paused emulator. On a running one
  they return an error saying so (`Emulator is running ...` /
  `... Emulator is not paused ...`) and change nothing; call
  `emu_pause` and repeat.
- `emu_wait_stop` waits at most 25 s per call (below the 30 s limit of
  the bundled wrapper, which extends its own timeout by `timeout_ms`);
  `done: false` is not an error, call it again. The stepping loops
  `emu_run_until_raster`, `emu_run_until_tstate` and
  `emu_run_until_event` end after at most 20 s of wall-clock time with
  `time_limited: true`. See `emulator://docs/run_control`.
- Send the next request only after the previous response arrived.
- `emu_snapshot_load` and `emu_snapshot_load_buffer` do not touch the
  floppy drives or the Quick Disk drive: every drive
  keeps the image that is inserted (file, write protection, unsaved
  changes) and an empty drive stays empty, whatever was inserted when
  the snapshot was saved. Only the controller state is restored (for the
  Quick Disk the SIO chip and the head position; disk presence and write
  protection seen by the machine follow the inserted medium). When
  the inserted disks differ from the snapshot, the load still succeeds;
  the difference is reported in a GUI message and on the emulator's
  stderr ("Snapshot [media]: the snapshot was saved with different disks,
  the inserted disks were kept:" and one line per drive, `FDD n: <name
  in the snapshot> (now: <name now or empty>)` or `Quick Disk: <name in
  the snapshot> (now: <name now or empty>)`), not in the tool result.
  A snapshot saved with another kind of Quick Disk drive (image or
  Unicard versus virtual directory) switches the drive kind for the run:
  the inserted medium is closed (unsaved changes are saved to its file)
  and the medium configured for the new kind is inserted.
  To reproduce a state exactly, insert the same images with
  `emu_media_insert` before the load and check `emu_media_state`. The
  tape is different: a snapshot load reopens it, see
  `emulator://docs/cmt_workflow`.
- `emu_media_insert` of a disk image whose file is already inserted in
  another floppy drive succeeds, but the image is inserted
  **write-protected** and the result carries `warning` ("The image is
  already inserted in FDD n. It was inserted write-protected here to
  keep the image file safe."; n = 0-3 for FDC0, 4-7 for FDC1). Only one
  drive may write to one image file: the first one that holds it.
  `emu_media_state` shows `ro: true` for the second slot and writes of
  the emulated program to that drive end with a write-protect error. The
  protection stays until the image leaves that drive: to write through
  the second drive, `emu_media_eject` the image from both drives and
  insert it into the second one only, or insert a copy of the file.
- Bundled Python wrapper: any tool result may carry `restarted: true`
  and `restart_reason` (a failed call: error text ending with
  `[restarted: true; ...]`). It means the previous backend ended
  unexpectedly and this call started a fresh emulator (pipe mode: RAM,
  registers, breakpoints and inserted media are back to defaults) or
  reconnected (TCP mode: the GUI emulator keeps its own state).
  `emu_status` reports the reason as `last_exit` until then.
