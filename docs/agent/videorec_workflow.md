# Video recording workflow (AI records a gameplay video)

Recipe for an AI client that records a gameplay video in the emulator by
itself: start recording, play the game with keystrokes, mark
chapters, redo a failed attempt from a snapshot (retake), stop, and export
the result to MP4.

Video recording is available in every build: MZ-800, MZ-700 PAL,
MZ-700 NTSC and MZ-1500 (`emu_videorec_status` -> `supported: true`,
`fps` = frames per second of the build).

| Build | Frame (with border) | AVI | fps | Sound |
|-------|---------------------|-----|-----|-------|
| MZ-800 | 928x288 | 928x576 | 50 | CTC + PSG; stereo with the second PSG (menu item "Allow PSG1 (stereo)") |
| MZ-700 PAL | 704x232 | 704x464 | 50 | CTC only (left = right) |
| MZ-700 NTSC | 704x232 | 704x464 | 60 | CTC only (left = right) |
| MZ-1500 | 704x232 | 704x464 | 60 | stereo: left CTC + first PSG, right CTC + second PSG |

## What gets recorded

- Every **emulated** frame (50 or 60 fps, see the table) of the whole
  screen including the border, lossless (ZMBV AVI, every picture line
  twice), plus the emulator sound (PCM 16-bit stereo, 48 kHz by default).
- Emulation time, not wall-clock time: emulation pauses (between your tool
  calls, `emu_pause`), MAX SPEED or a slow host do **not** show in the
  video. You can think as long as you like between moves.
- Files: `<name>.avi` (+ `<name>_002.avi`, ... for very long recordings)
  and the sidecar `<name>.cuts.json` (segments, markers, parts) written on
  stop. Keep them together.

## Tools

| Tool | What it does |
|------|--------------|
| `emu_videorec_start(path="", frames=0)` | Creates the AVI and arms the recording. Empty `path` = generated `<platform>_YYYYMMDD_HHMMSS.avi` (`mz800_...`, `mz700_...`, `mz1500_...`) in the configured output folder. `frames` > 0 = auto-stop after that many recorded frames (the emulator keeps running). |
| `emu_videorec_stop(wait=True)` | Stops; with `wait` returns after the files are complete (`saved: true`, `recorded_frames`). |
| `emu_videorec_pause(paused=None)` | Record-pause (emulation runs, frames are not written): `true` / `false` / omitted = toggle. Creates a cut (new segment) in the video. |
| `emu_videorec_marker(label="")` | Chapter marker at the next recorded frame; becomes a YouTube chapter on export. |
| `emu_videorec_status()` | `state` (`idle` / `recording` / `paused`), `start_pending`, `frames`, `fps`, `duration_s`, `segment`, `bytes`, `parts`, `retake_mode`, `path`, `sidecar`, `last_error`, `last_event`, `timebase`, `timebase_effective`, `rt_activity`. |
| `emu_videorec_timebase(timebase)` | `"emulated"` (default, one emulated frame = one video frame) or `"realtime"` (what was on screen, 50 or 60 frames per wall-clock second). Applies now and to the next start; every switch starts a new segment. Changes the persistent user setting (saved to the INI on exit, like the GUI switch) - switch back to `emulated` when done on a shared instance. |

Every successful `emu_videorec_*` reply also contains the full status.
`last_event.kind` is one of `started`, `saved`, `failed`, `retake`,
`seam`; `last_event.frame` is the recording frame of the event (for
`saved` / `failed` the number of frames of the recording).

## Recipe

1. **Prepare the game.** Load it (e.g. `emu_media_insert` a disk and
   `emu_reset`, or `emu_media_run_mzf`) and run it to the point where the
   video should begin (title screen). `emu_pause`.
2. **Start.** `emu_videorec_start(path="C:/videos/game.avi")`. The reply
   has `start_pending: true` and already the final `path` and `sidecar`.
   Recording begins at the end of the next emulated frame; that frame is
   not recorded. Until then `emu_videorec_marker` / `emu_videorec_pause`
   answer `Video recording has not started yet ...`.
3. **Play.** Advance the game with `emu_run(frames=N)` (max 1000 per call,
   the emulator pauses itself afterwards) and give input with
   `emu_input_send_keys` (typing; `frame_per_key` frames per key) or
   `emu_input_press_key` / `emu_input_release_key` around `emu_run` for
   held keys (movement). Everything that is emulated is recorded. Look at
   the screen with `emu_screenshot_save_to_file` or
   `emulator://video/text_dump` to decide the next move. See
   `emulator://docs/mz800_keyboard` for key names (`emu_input_press_key`
   accepts the same names as `emu_input_send_keys`: `CR`, `CURSOR_UP`, `CURSOR_DOWN`,
   `CURSOR_LEFT`, `CURSOR_RIGHT` (or the short `UP`, `DOWN`, ...), ...).
   - **Check `actual_frames`** in the `emu_run` reply: `stopped_by` other
     than `"frames"` (breakpoint, safety timeout) means fewer frames were
     emulated. At normal speed `emu_run` takes real time (`fps` frames = 1 s,
     also in headless mode). To get through long parts faster use
     `emu_set_speed(mode="max")` - MAX SPEED does not change the video (it
     records emulated frames).
   - For an exact number of frames per move prefer
     `emu_input_press_key` + `emu_run(frames=N)` + `emu_input_release_key`.
     `emu_input_send_keys` waits in real time, so with MAX SPEED it may let
     more frames pass than `frame_per_key` (observed: about 66 frames for
     one key with `frame_per_key=5`).
4. **Chapters.** `emu_videorec_marker(label="Level 2")` at the start of
   each part worth a chapter. YouTube wants at least 3 chapters, each at
   least 10 s long [unverified].
5. **Checkpoint and retake.** Before a risky part, while paused (after
   `emu_run` the emulation is paused): `emu_snapshot_save(path=".../cp1.mzs")`.
   If the attempt fails: `emu_snapshot_load(path=".../cp1.mzs")`, then go
   on playing. With `retake_mode: "discard"` (default) the recording
   rewinds to the snapshot point - the failed attempt is not in the video
   and there is no visible seam. The rewind happens at the end of the next
   emulated frame: after the next `emu_run`, `last_event` is
   `{"kind": "retake", "frame": <snapshot point>}` and `frames` dropped
   back. The same checkpoint can be loaded again and again.
   - Markers at or after the snapshot point belong to the discarded
     attempt and are removed. Add a marker after at least one frame past
     the checkpoint, or add it again after the retake.
   - A snapshot taken before this recording started, or in a part already
     discarded by an earlier retake, cannot be rewound to: loading it
     creates a seam (`last_event.kind: "seam"`, new segment with a
     transition) instead.
   - With `retake_mode` `"seam"` or `"off"` (user setting) loading a
     snapshot always creates a seam and nothing is discarded.
6. **Skip boring parts.** `emu_videorec_pause(paused=true)`, run through
   e.g. loading screens, `emu_videorec_pause(paused=false)`. The video gets
   a cut there (export joins the segments with a transition, fade by
   default). Takes effect at the end of the next emulated frame.
7. **Stop.** `emu_videorec_stop()` -> `saved: true`, `recorded_frames`,
   `path`, `sidecar`. If `saved` is `false`, read `last_event.text` /
   `last_error`.
8. **Export** - see the next section.

Alternative for a fixed length: `emu_videorec_start(frames=1500)` (30 s
at 50 fps, 25 s at 60 fps) records exactly 1500 frames and stops by itself; wait for
`last_event.kind == "saved"` with `emu_videorec_status`.

**Keep the emulated timebase for agent-driven recordings.** The
`realtime` timebase (`emu_videorec_timebase`) records wall-clock time:
the pauses between your tool calls would show up (frozen picture or a
jump, depending on the INI option `realtime_pause`) and MAX SPEED would
play fast. It is meant for a human playing live; with `emu_run` and
`frames` it still works (frame counts are wall-clock frames), but the
video is not deterministic. The sidecar (version 4) also gets `events`
(timebase, pause, speed, snapshot, reset) unless disabled in the INI
(`state_marks`). Automatic markers (INI `auto_markers`, default on)
depend on the effective timebase: in emulated time only "Reset" gets
one - `emu_set_speed(mode="max")` and retake snapshot loads do NOT add
chapters (they stay in `events` only), so your own
`emu_videorec_marker()` calls are the chapters. In realtime parts
"Speed N%" / "Speed MAX", "Snapshot loaded" and "Pause" (user pause)
are added as well.

## Export to MP4

The recording is a lossless master; the MP4 for YouTube (scaling,
crop, transitions, chapters file) is made by the export script of the
emulator, `docs/tools/videorec_export.py` (Python 3 + ffmpeg),
run on the host - it is not an MCP tool:

```
python3 docs/tools/videorec_export.py C:/videos/game.cuts.json -o C:/videos/game.mp4 --target 1080p
```

Input is the **sidecar** (`sidecar` from the stop reply). Output:
`game.mp4` and `game.chapters.txt` (YouTube chapters from the markers).
The script reads the platform, frame size, picture area (canvas) and fps
from the sidecar (version 4); the MP4 keeps the fps of the recording
(60 from MZ-700 NTSC and MZ-1500).
Useful options: `--target 1080p|1440p|2160p`, `--crop full|reduced|canvas`,
`--transition cut|fade|crossfade|card`, `--dry-run`. Run it with `--help`
for the full list.

## Errors (all English, `{"error": "..."}`)

| Message | Meaning / what to do |
|---------|----------------------|
| `Video recording is already running` | Stop the current recording first (`emu_videorec_status` shows it). |
| `Cannot create video file: <path>` | Folder does not exist or is not writable. Use an existing folder. |
| `Video recording is not running` | Stop / pause / marker without a recording. |
| `Video recording has not started yet: ...` | Start is pending - run at least one frame. |
| `Invalid parameters: ...` | Wrong argument type (e.g. `frames` < 0). |
| `Recording failed: <reason>` (in `last_event.text`) | Writing failed (e.g. disk full); the recording ended. |

Tool calls can also fail with `Emulator busy: ...` (not executed, safe to
retry) - see `emulator://docs/error_handling`.

## Raw JSONL (clients without the Python wrapper)

The same commands exist on the emulator's own MCP transport (pipe / TCP):
`videorec_start` (`path`, `frames`), `videorec_stop`, `videorec_pause`
(`paused`: bool or null), `videorec_marker` (`label`), `videorec_status`,
`videorec_timebase` (`timebase`: `"emulated"` / `"realtime"`).

```
{"type":"request","req_id":1,"cmd":"videorec_start","data":{"path":"C:/videos/game.avi"}}
{"type":"request","req_id":2,"cmd":"run","data":{"frames":100}}
{"type":"request","req_id":3,"cmd":"videorec_marker","data":{"label":"Level 1"}}
{"type":"request","req_id":4,"cmd":"videorec_stop","data":{}}
{"type":"request","req_id":5,"cmd":"videorec_status","data":{}}
```

The raw `videorec_stop` does not wait: poll `videorec_status` until
`state` is `idle` and `last_event.kind` is `saved` or `failed`.
