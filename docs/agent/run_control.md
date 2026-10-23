---
title: Run control - waiting for a stop, deadlines, run numbers, hashes
description: How to start a run, wait until it stops and learn why and where (emu_wait_stop, run_seq, paused / resumed events), limit a run in emulated time (deadline), pause only your own run (if_run_seq), and compare memory or the screen by SHA-256 instead of downloading data.
---
# Run control: wait for a stop, deadlines, run numbers, hashes

Recipe for an AI client that runs the emulator and must know when a run
ended, why and where - without polling `emu_status` in a loop, without an
endless run when a target is never reached, and without stopping a run
that the user started in the GUI.

## Runs and run numbers

A **run** is the time between leaving a pause and the next pause: a free
run, a run to an address, a step over a CALL, or a single step. Every run
has a number `run_seq` (starts at 0 = the run since the emulator started,
then grows by one). Tools that start a run return its number:

| Tool | Returns |
|------|---------|
| `emu_resume` | `run_seq`, `already_running` |
| `emu_run_until_addr`, `emu_step_out` | `run_seq` (plus `deadline_ticks`) |
| `emu_step_into`, `emu_step_over` | `run_seq`, `completed`, `running` |
| `emu_run(frames)` | `run_seq` of the run that just ended |
| `emu_pause`, `emu_get_state` | `run_seq` of the current (or last) run |

Each stop writes a **stop record** and sends exactly one event `paused`
with the same fields:

| Field | Meaning |
|-------|---------|
| `run_seq` | number of the run that stopped |
| `reason` | `run_to` (target or step over reached), `breakpoint`, `deadline`, `frames` (`emu_run`), `step`, `manual` (pause), `exit` (breakpoint action `exit`), `bp_action_saturation`, `event_trigger`, `replay` (short pause while a replay starts), `replay_end` / `replay_desync` (end of a replay playback, see `emulator://docs/replay`), `replay_seek`, `replay_goto` (`emu_replay_seek` / `emu_replay_goto` reached the target), `run_until_ticks` (`emu_run_until_tstate` with `target_ticks`) |
| `pc` | PC at the stop (next instruction) |
| `bp_id` | breakpoint that stopped the run (`breakpoint`) |
| `started_by`, `start_cmd` | who started the run (`mcp` = an agent, `user` = the GUI) and how (`run_to`, `step_into`, `run`, `external` = Pause key) |
| `stopped_by` | who paused it (only for `manual`: `mcp` or `user`), otherwise `emulator` |
| `start_ticks`, `stop_ticks` | GDG clock ticks since the emulator started (same as `Cycle` in breakpoint expressions; a machine reset does not restart them, loading a snapshot sets the saved value) |
| `elapsed_ticks` | length of the run in GDG ticks |
| `frame` | completed frames since the emulator started, at the stop |
| `step` | the run was a single step |
| `state_rev` | revision of the machine state after the stop (only in the event, not in the stop history; see `emulator://docs/state_revision`) |

The event `resumed` (`run_seq`, `state_rev`, `pc`, `started_by`,
`start_cmd`, `start_ticks`, `deadline`) reports every run start except single steps,
also when the user resumes in the GUI. Subscribe with
`emu_event_subscribe(["paused", "resumed"])`.

## Recipe: run and wait

1. `emu_pause()` (a freshly started emulator runs).
2. Start: `r = emu_run_until_addr(addr=0x1234, deadline_frames=100)`.
3. Wait: `w = emu_wait_stop(run_seq=r["run_seq"], timeout_ms=10000)`.
   - `done: true` - `w["record"]` says why (`reason`) and where (`pc`).
   - `done: false` - the run still goes; call `emu_wait_stop` again
     (one call waits at most 25 s, below the 30 s transport limit).
4. Decide by `reason`: `run_to` = target reached, `deadline` = target not
   reached in time, `breakpoint` = another breakpoint first, `manual` with
   `stopped_by: "user"` = the user paused.

`emu_wait_stop` without `run_seq` waits for the current run (or returns
the last one when paused). `history_lost: true` means the run ended but
its record dropped out of the history (32 newest, `emu_get_stop_history`,
resource `emulator://run/history`).

## Deadlines (emulated time)

`emu_resume`, `emu_run_until_addr` and `emu_step_out` take at most one
of `deadline_frames`, `deadline_ms`, `deadline_ticks`:

- The deadline counts emulated time, not wall clock: the same program
  from the same state stops at the same point (measured: three runs of
  `deadline_frames=5` took exactly 5 x 354432 GDG ticks each).
- `deadline_frames` stops exactly at the N-th frame boundary.
  `deadline_ms` / `deadline_ticks` stop at the first emulation event
  point at or after the deadline, less than one frame late.
- A deadline holds until the next pause, then it is gone. A reset of the
  machine during the run does not cancel it (the GDG clock counter keeps
  counting over a reset).
- `max_cycles` of `emu_run_until_addr` / `emu_step_out` is informative
  only and limits nothing; use a deadline.

## Do not stop the user's run

`emu_pause(if_run_seq=N)` pauses only while run N is running. If the
user started another run in the GUI meanwhile, the call does nothing and
returns `paused: false`, `skipped: "run_seq_mismatch"`
(`"not_running"` when nothing runs). Check `started_by` in
`emu_get_stop_history` to see what the user did.

## Steps

- `emu_step_into` and `emu_step_over` on an ordinary instruction return
  after the step executed (`completed: true`); the next call sees the
  state after the step.
- `emu_step_over` on CALL / RST / DJNZ / a block instruction is a run to a
  temporary breakpoint: `completed: false`, `running: true`; wait with
  `emu_wait_stop(run_seq)`.
- On a running emulator `emu_step_into` does nothing (`completed: false`,
  `running: true`) and `emu_step_over` only pauses (`paused_only: true`).
  `stepped: true` is kept for compatibility and says nothing.
- `step_done.cycles` is a 32-bit T-state counter that wraps after about
  20 minutes of emulation; use `step_done.ticks` (64-bit GDG ticks).
  `step_done.state_rev` is the revision of the machine state after the
  step.

## Stepping loops

`emu_run_until_raster`, `emu_run_until_tstate` and `emu_run_until_event`
step instruction by instruction inside one request and cannot be
cancelled. They end after at most 20 s of wall-clock time
(`time_limited: true`). For long waits prefer `emu_resume` with a
deadline and `emu_wait_stop`.

Exception: `emu_run_until_tstate(target_ticks=T)` does not step. The
emulator runs and stops itself at the start of the first instruction
whose GDG time is >= T (stop reason `run_until_ticks`; `exact` = an
instruction started exactly at T), at full speed and without the 32-bit
wrap of `total_cycles`. A breakpoint may stop it earlier
(`stopped_by`). Take T from `time.ticks` of `emu_state_observe` or the
`t` column of a trace. A pause and resume never change when the next
interrupt is accepted, so stopping at a time and going on gives the same
run as not stopping.

## Compare instead of download (SHA-256)

- `emu_mem_read` and `emu_region_read` return `sha256` of the bytes read;
  with `want_data=false` only the hash (no `data_b64`). Same hash = same
  bytes.
- `emu_frame_hash` returns `image_sha256` (the picture from the same
  source as a screenshot: palette indices of every pixel, then the 16
  palette colours) and `vram_sha256` (the video memory: MZ-800 all four
  planes, MZ-700 / MZ-1500 character and attribute VRAM) plus the beam
  position. The screenshot tools return the same `image_sha256`.
- The picture also depends on GDG registers (palette, mode, border), the
  VRAM hash does not - compare both when needed.
- Paused in the middle of a frame, the upper part of the picture is from
  the new frame and the lower part from the previous one, so the
  picture hash depends on the beam position (`beam`). Compare pictures at
  a frame boundary (after `emu_run(frames)` or `deadline_frames`).

Recipe "wait until the screen stops changing": `emu_resume(deadline_frames=25)`,
`emu_wait_stop`, `emu_frame_hash`; repeat until `image_sha256` stays the
same twice.
