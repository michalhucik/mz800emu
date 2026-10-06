# CMT cassette workflow

Reference for the two distinct ways to get a program off a Sharp MZ
cassette image into the running machine, plus the real-tape transport,
recording and per-block controls.

## Two loading paths: real tape vs cmthack

There are two unrelated mechanisms. Pick deliberately.

| | Real tape (`emu_cmt_*`) | cmthack instant load |
|--|--|--|
| What it is | Cycle-accurate cassette signal emulation through the PIO line | A ROM patch that copies a tape file straight into RAM |
| How to trigger | `emu_cmt_open` / `emu_media_insert(slot="cmt")` then `emu_cmt_play`; the program reads the tape via its own loader | `emu_cmt_hack_set(true)` then `emu_media_load_mzf` (or `emu_media_run_mzf`) |
| Speed | Real time (or fast with `emu_cmt_set_cpu_boost(true)`) | Instant |
| Works for | Everything: custom loaders, copy protection, multi-stage loaders, recording | Only programs that load through the patched standard ROM entry points |
| Fidelity | High (real signal) | Low (bypasses the tape entirely) |

Rule of thumb: use cmthack for a quick "just boot this MZF". Use the
real tape transport when you care about the loader, want to record, or
the program does not load through standard ROM.

The cmthack patch state is readable as `cmthack_enabled` in
`emulator://periph/cmt`; toggle it with `emu_cmt_hack_set`.

A tape load started by the ROM itself (MZ-800 boot menu key C, BASIC
`LOAD`, monitor load) goes through the cmthack file dialog when the
patch is installed. Headless / pipe mode has no dialog: the load is
cancelled at once (the ROM reports Break) and a warning goes to stderr.
To load from the virtual tape there, turn the patch off first
(`emu_cmt_hack_set(false)`), then `emu_cmt_open` + `emu_cmt_play`.

## Real-tape transport flow

1. Load an image:
   - `emu_cmt_open(path, play_immediately=false)` - CMT-specific open by
     file extension (.mzf / .mzt / .wav / .lep / .l16 / ...). With
     `play_immediately=true` it starts playback in one step.
   - or the generic `emu_media_insert(slot="cmt", path=...)`.
2. Start the tape: `emu_cmt_play` (or `emu_cmt_play_paused` to arm it
   suspended).
3. Let the program read it. Pause/resume with
   `emu_cmt_pause(paused=...)`.
4. Stop with `emu_cmt_stop`; remove with `emu_cmt_eject`.

Transport state is in `emulator://periph/cmt`: `state`
("stop"/"play"/"record"), `paused`, `filled` (image loaded),
`output` (current bit on the PIO line). `state` and `paused` are
orthogonal: PLAY + paused=true means playback is suspended.

## Recording (WAV / LEP / L16)

1. `emu_cmt_record(path)` - opens `path` and arms recording. The filename
   extension selects WAV, LEP (50 us units), or L16 (16 us units); a path
   without an extension defaults to WAV. An unknown extension is rejected.
   The tape must be in STOP and the path must be writable.
2. Recording starts paused. Call `emu_cmt_pause(paused=false)` to begin
   capturing the signal the program writes to the cassette output.
3. `emu_cmt_stop` to finish.

## Tape properties

Set via `emu_cmt_set_*`, all reflected in `emulator://periph/cmt`:

| Tool | Resource field | Notes |
|--|--|--|
| `emu_cmt_set_speed(speed)` | `cmtspeed` | Default speed ratio. See speed table below. |
| `emu_cmt_set_polarity(inverted)` | `polarity_inverted` | Rear DIP switch signal polarity. |
| `emu_cmt_set_cpu_boost(enabled)` | `cpu_boost` | Run at max speed while the tape runs (PLAY or RECORD, not paused). A user preference, not machine state: snapshots store it (for older emulator versions) but `emu_snapshot_load` does not restore it - the current value stays, and MAX SPEED is re-applied against the restored deck state. Turns off only the MAX SPEED it turned on - MAX SPEED set via `emu_set_speed(mode="max")` stays. If the user turns MAX SPEED off during a boost, the boost turns it on again at the next pause/resume or option change. While recording, the boost switches MAX SPEED off after 5 s without tape writes and back on when writing resumes. |
| `emu_cmt_set_mzfsize_check(enabled)` | `mzfsize_check` | Reject MZF where body size != header size. |

### Speed ratios (en_CMTSPEED)

Relative to the 1200 Bd baseline. Tools accept a ratio string key or
the raw integer.

| Key | Int | Meaning |
|--|--|--|
| `1:1` | 1 | standard 1200 Bd |
| `2:1` | 2 | double |
| `2:1_cpm` | 3 | double, CP/M variant |
| `3:1` | 4 | triple |
| `3:2` | 5 | |
| `7:3` | 6 | Intercopy 10.2 |
| `8:3` | 7 | CP/M cmt.com |
| `9:7` | 8 | |
| `25:14` | 9 | |

## Tape blocks (multi-file tapes)

SIMPLE_TAPE containers hold several blocks (e.g. a multi-program tape);
SINGLE containers (a plain .mzf) hold one block.

- List blocks: `emulator://periph/cmt/tape` returns `available`,
  `container_type` (0=SINGLE, 1=SIMPLE_TAPE), `current_block`, and a
  `blocks` array (block_id, name, cmt_speed, type, is_current, playable,
  recordable). `type` is 0=WAV, 1=MZF, 2=TAPHEADER, 3=TAPDATA.
- Seek: `emu_cmt_tape_seek(block_id)` positions at a block (0-based).
  Only SIMPLE_TAPE containers support it; on a SINGLE container (plain
  .mzf or .wav) it fails even for block 0. To replay a SINGLE tape from
  the beginning, use `emu_cmt_stop` + `emu_cmt_play` (play from STOP
  always starts at the beginning).
- Per-block speed: `emu_cmt_tape_set_block_speed(block_id, speed)`. Only
  the cmt speed ratio is adjustable per block; there are no other
  per-block parameters.

When no tape is loaded the tape resource returns
`{"available": false, "blocks": []}`.

## When to use what

- "Just boot this MZF fast" -> `emu_cmt_hack_set(true)` +
  `emu_media_run_mzf`.
- "Run the program through its real loader" -> `emu_cmt_open` +
  `emu_cmt_play` (optionally `emu_cmt_set_cpu_boost(true)` to speed up a
  long load without losing fidelity).
- "Capture what the program writes to tape" -> `emu_cmt_record` +
  `emu_cmt_pause(paused=false)`.
- "Multi-program tape, pick the third one" -> read
  `emulator://periph/cmt/tape`, `emu_cmt_tape_seek(2)`, `emu_cmt_play`.

## Cross-references

- `emulator://periph/cmt` - live CMT module state (transport, props,
  cmthack_enabled).
- `emulator://periph/cmt/tape` - current tape block listing.
