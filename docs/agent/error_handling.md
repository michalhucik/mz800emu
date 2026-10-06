---
title: MCP error handling and retry rules
description: How to read failed tool responses - which errors mean "not executed, safe to retry", "partially executed" or a real failure, plus request timing expectations.
---
# MCP error handling and retry rules

Every request gets exactly one response. On failure the tool result
carries an `error` string (raw JSONL: `"success": false`, `"error"`).
The error text tells you whether the emulator **executed** the command,
which decides whether retrying is safe.

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
all `videorec_*` tools, `media_load_mzf`, `media_load_binary`,
`media_insert`, `media_eject`, `cmt_open`, `cmt_record`,
`get_frame_screenshot`, `screenshot_save_to_file` (and the wrapper tools
built on them). The 10 s queue limit applies to them as well. A client
with its own shorter timeout (the bundled Python wrapper waits 30 s per
request) may give up earlier, although the command still completes.

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
| `Invalid parameters`, `Missing required field: ...`, `Invalid id`, `Unknown command`, `frames must be ...` | **No** - rejected before reaching the emulator. | Fix the request; retrying unchanged fails again. |
| anything else, e.g. `bp_remove failed (unknown id?)`, `Pause failed`, `BP_LIST failed` | **Yes** - the emulator executed the command and it failed (unknown ID, invalid data, ...). | Retrying unchanged does not help. Inspect state (e.g. `emulator://breakpoints` for BP IDs). |

The prefixes `Emulator busy:` and `Emulator is shutting down` are a
stable contract; match on them, not on the rest of the sentence. Such
an error (except `command still running`) ends with the tool's
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
- Send the next request only after the previous response arrived.
- Bundled Python wrapper: any tool result may carry `restarted: true`
  and `restart_reason` (a failed call: error text ending with
  `[restarted: true; ...]`). It means the previous backend ended
  unexpectedly and this call started a fresh emulator (pipe mode: RAM,
  registers, breakpoints and inserted media are back to defaults) or
  reconnected (TCP mode: the GUI emulator keeps its own state).
  `emu_status` reports the reason as `last_exit` until then.
