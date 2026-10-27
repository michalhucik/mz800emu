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
     file extension (.mzf / .mzt / .wav / ...). With
     `play_immediately=true` it starts playback in one step. The result
     reports the actual transport state after the call (`playing`,
     `state`, `paused`), not the request; if playback was requested but
     did not start, it also carries `warning`.
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

## Recording (WAV only)

1. `emu_cmt_record(path)` - opens `path` as a WAV file and arms
   recording. The tape must be in STOP and the path must be writable.
   Recording is WAV output only; no other format is supported.
2. Recording starts paused. Call `emu_cmt_pause(paused=false)` to begin
   capturing the signal the program writes to the cassette output.
3. `emu_cmt_stop` to finish.

## Tape properties

Set via `emu_cmt_set_*`, all reflected in `emulator://periph/cmt`:

| Tool | Resource field | Notes |
|--|--|--|
| `emu_cmt_set_speed(speed)` or `emu_cmt_set_speed(pulses_us=[...])` | `cmtspeed`, `default_pulses_us`, `custom_pulses_us` | Default speed: a ratio (table below) or custom pulse lengths. |
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
| `custom` | 10 | custom pulse lengths (see below) |

### Custom pulse lengths and UniCMT speed headers

Besides the ratios the virtual cassette can play exact pulse lengths:
`pulses_us = [long_high, long_low, short_high, short_low]` in
microseconds (long pulse = bit 1, short pulse = bit 0), each in
(0, 65535]. The order is the same as in the CMTSPEED header of the
UniCMT cassette replacement:

| UniCMT header file | `pulses_us` |
|--|--|
| `1xspeed.mzf` | `[470, 494, 240, 278]` |
| `2xspeed.mzf` | `[235, 247, 120, 139]` |
| `3xspeed.mzf` | `[156, 164, 80, 92]` |

- `emu_cmt_set_speed(pulses_us=[...])` stores the lengths and makes them
  the default speed (`cmtspeed` 10); `emu_cmt_set_speed("custom")` later
  switches back to the stored lengths. They are saved in the
  configuration.
- `emu_cmt_tape_set_block_speed(block_id, pulses_us=[...])` sets them for
  one block.
- An .mzt tape may contain CMTSPEED headers (a 128-byte MZF header of type
  00h named `CMTSPEED` with no body) between its parts. The emulator does
  not play such a header and does not list it as a block; every following
  block plays with the header's pulse lengths (`block_speed` "set",
  `cmt_speed` 10) until the next header. Blocks before the first header
  use the default speed. A header with a zero length is ignored. The real
  UniCMT does not play a tape whose first block is a CMTSPEED header; the
  emulator applies it anyway.
- The pulses are generated with the resolution of the GDG clock, so a
  fast loader sees the lengths from the header, not a rounded ratio. The
  ROM loader samples each bit at a fixed time and expects 1:1 timing
  (UniCMT 1x); faster parts are meant for programs with their own loader.

## Tape blocks (multi-file tapes)

SIMPLE_TAPE containers hold several blocks (e.g. a multi-program tape);
SINGLE containers (a plain .mzf) hold one block.

- List blocks: `emulator://periph/cmt/tape` returns `available`,
  `container_type` (0=SINGLE, 1=SIMPLE_TAPE), `current_block`, and a
  `blocks` array (block_id, name, cmt_speed, type, is_current, playable,
  recordable, block_speed, pulses_us). `type` is 0=WAV, 1=MZF,
  2=TAPHEADER, 3=TAPDATA. `block_speed` is "default" (follows the
  default speed), "set" (own `cmt_speed`) or "none"; `pulses_us` are the
  effective pulse lengths of an MZ block in microseconds.
- Seek: `emu_cmt_tape_seek(block_id)` positions at a block (0-based).
  Only SIMPLE_TAPE containers support it; on a SINGLE container (plain
  .mzf or .wav) it fails even for block 0. To replay a SINGLE tape from
  the beginning, use `emu_cmt_stop` + `emu_cmt_play` (play from STOP
  always starts at the beginning).
- Per-block speed: `emu_cmt_tape_set_block_speed(block_id, speed)` or
  `emu_cmt_tape_set_block_speed(block_id, pulses_us=[...])`. Only the cmt
  speed is adjustable per block; there are no other per-block
  parameters.

When no tape is loaded the tape resource returns
`{"available": false, "blocks": []}`.

## Inserting a tape from the command line

The emulator can start with a tape already in the virtual cassette
recorder. These options also work with `--mcp-pipe`. `--run-mzf` does
not: pipe mode accepts only `--help`, `--version`, `--headless`,
`--home-dir`, `--cfg-dir`, `--work-dir`, `--config`, `--no-save-ini`,
`--no-first-run-windows`, the `--cmt*` options below and the `--fdd*`
options; any other option ends the emulator with `Unknown option: <option>`
on stderr.

| Option | Effect |
|--|--|
| `--cmt <file>` | Insert the tape (`.mzf`, `.m12`, `.mzt`, `.tap`, `.wav`, `.wave`) in STOP. A CMT hack that is on is turned off for this session only; the `.ini` keeps the original setting. |
| `--cmt-play` | Also press PLAY. |
| `--cmt-play-pause` | The tape waits in PLAY + PAUSE until the ROM or the program turns the cassette motor on. |
| `--cmt-boot` | MZ-800 only: like `--cmt-play-pause`, and the C key is held from start until the motor turns on, so the boot menu loads the first program from the tape. |

The emulated motor follows the 8255 motor line (PC3): every rising edge
toggles it. The ROM of all three computers makes such an edge during its
startup, which stops a playing tape. So with a normal ROM boot,
`--cmt-play` ends in PLAY + PAUSE as well; the tape keeps playing only
together with `--run-mzf` (that skips the ROM). For a program that loads
its further parts through the ROM loader:

- `mz800emu --headless --mcp-tcp-port=<port> --run-mzf <part1.mzf> --cmt
  <rest.mzt> --cmt-play-pause` (MCP over TCP), or
- in pipe mode `--mcp-pipe --cmt <rest.mzt> --cmt-play-pause` and then
  `emu_media_run_mzf(part1.mzf)`. The tape stays in PLAY + PAUSE after
  `emu_media_run_mzf` and starts when the program turns the motor on.

Check the result in `emulator://periph/cmt` (`filled`, `state`,
`paused`, `cmthack_enabled`). Invalid combinations (a modifier without
`--cmt`, `--cmt-play` with `--cmt-play-pause`, `--cmt-boot` with
`--run-mzf` or `--cmt-play`) and a missing or unknown tape file end the
emulator before it starts, with an error on stderr.

## Starting a program in a defined state

A program may behave differently depending on what ran in the machine
before it (interrupt mode, Z80 PIO interrupt vector, CTC, palette, ...).
To get the same state as after a real tape load, use one of:

- `emu_media_run_mzf(path)` with the default `bootstrap=true`: resets
  the machine and starts the program exactly like the CLI option
  `--run-mzf` (the state the ROM leaves after loading from tape, PC =
  exec address). The result does not depend on the previous state. When
  the emulation is paused, the program waits at its first instruction,
  so you can set breakpoints before `emu_run`.
- `emu_reset`, let the ROM boot (`emu_run` with `frames`), then load
  from the virtual tape through the ROM (`emu_cmt_open` + the load key
  or command + `emu_cmt_play`). Slowest, but it runs the real ROM and
  loader code.

Avoid `emu_media_run_mzf(bootstrap=false)` and `emu_media_load_mzf` +
`emu_set_register` PC when the previous state matters: they only load
the file and jump, everything else stays as the previous program left
it. Example: a game part started this way over the running first part
of the same game stopped getting interrupts, while the same procedure
right after the emulator start worked.

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
