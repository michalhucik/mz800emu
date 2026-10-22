# Replay: recording and playing back inputs

Records the inputs of a run once and plays them back exactly - in the
same or a new emulator process, at any speed. Typical uses:

- play a game or a program until a bug shows up, then repeat exactly the
  same run as many times as needed with other breakpoints, trace or the
  RAM Write Log switched on;
- keep a recorded run as a regression test: `--replay-verify` checks in
  CI that a new build still runs it the same way;
- while recording, go back a few seconds and try another way; while
  playing back, jump to any frame (see [Checkpoints, rewind and
  seek](#checkpoints-rewind-and-seek)).

All platforms (MZ-800, MZ-700 PAL and NTSC, MZ-1500). A replay plays back
only in the binary of the same platform and TV system.

## How it works

A recording starts from a complete snapshot of the machine (always with
the RAM disk, whatever the snapshot settings say), taken and loaded back
at the start. Together with it the recording stores the contents of the
mounted media (see [Media](#media)). From then on it stores, in emulated
frames:

- every change of the keyboard (the whole key matrix, keyboard of the
  host, virtual keyboard window and MCP keys together) and of the
  joysticks (host joystick, numeric keypad, MCP),
- resets and autotype,
- tape control (play, stop, pause, eject, block selection, recording to
  tape) and the files chosen in the CMT hack dialog,
- writes of the debugger and of MCP tools (memory, memory regions,
  registers, CPU flags, I/O ports, injected IRQ and NMI),
- every N frames (default 50, one second on PAL) a SHA-256 hash of the
  CPU (registers including the hidden Z80 state), the RAM, the MemExt
  RAM, the VRAM, the PCG and the peripherals (CTC 8253, PIO 8255, Z80
  PIO, PSG, GDG, VRAM controller, floppy and Quick Disk controllers),
  together with the emulated time.

All inputs are taken at the boundary of a frame: a key pressed in the
middle of a frame reaches the emulated computer at the end of that frame,
both while recording and while playing back. The host joystick is read
once per frame as well. Playback loads the start snapshot, applies the
inputs at the same places and compares the hashes. The first frame whose
state differs stops the playback.

The Unicard clock shows, while recording and playing back, the time of
the start of the recording plus the emulated time, so the program sees
the same clock in the playback.

## Recording

| Way | How |
|-----|-----|
| Menu | **Replay -> Record Inputs...** asks for the file (`.mzr`) and starts. **Replay -> Stop Replay** saves it. The same buttons are in the **Replay Control** window (see below). |
| Command line | `--replay-record <file>` records from the first frame; the file is written when the emulator exits. `--replay-hash-interval <N>` sets the frames between hashes (1-100000). |
| MCP | `emu_replay_record_start`, `emu_replay_record_stop`, `emu_replay_rewind`, `emu_replay_status` (see [`mcp-server/tools-overview.md`](mcp-server/tools-overview.md)). |

If the emulation runs, it pauses at the next frame boundary, the
recording starts and the emulation runs on. While recording, the lower
left corner of the picture shows **REPLAY REC** and the frame number.

While recording:

- **Machine settings** (the Devices menu, platform, peripherals, memory
  expansions, ROM) and **loading a snapshot** are not available: the
  replay could not repeat them. Over MCP the tools that change the
  machine this way (snapshot load, media, settings) are refused with an
  explanation. A device change that does not come from the menu (the
  Alt+1..4 shortcut, a file dialog or a device window opened before the
  recording started) is refused with a message as well.
- **The tape** can be controlled as usual (Virtual CMT and Tape Filelist
  windows, MCP `emu_cmt_*` transport, block seek and recording to tape).
  Inserting another tape, the tape speed, custom pulses, polarity and the
  CMT hack switch are not available while recording.
- **Writes of the debugger** are recorded while the emulation runs and
  while it is paused at a frame boundary - after a pause (the pause
  button or MCP `emu_pause` pause at the end of the frame), after
  `emu_run(frames=N)` or a deadline. In a pause between frame boundaries
  (after a step or a breakpoint) a write is refused: run to the next
  frame boundary first. Tape control and a reset from such a pause wait
  for the next frame boundary.
- **Breakpoint actions** that write (`poke`, `set`) are not recorded;
  the recording gets a warning and plays back correctly only with the
  same breakpoints.
- Recording does not start while autotype runs, Freeze Bytes are active
  or the tape records.

## Playback

| Way | How |
|-----|-----|
| Menu | **Replay -> Play Replay...** asks for the file, loads the start snapshot and runs. **Replay -> Stop Replay** ends it. |
| Command line | `--replay <file>` plays it back after startup. `--exit-at-replay-end` ends the run at the end of the playback, `--replay-verify` does the same at MAX SPEED (see below). |
| MCP | `emu_replay_play` (`pause_after_load` to set breakpoints before it runs), `emu_replay_seek`, `emu_replay_play_stop`, `emu_replay_status`; the end is a pause with reason `replay_end` or `replay_desync` (`emu_wait_stop`). |

While playing back, the picture shows **REPLAY PLAY** with the frame and
the length of the replay. Live input (keyboard, virtual keyboard, host
joystick, MCP keys) is ignored and the tape windows are disabled - the
replay controls the tape. Breakpoints, trace, the RAM Write Log,
screenshots and video recording work as usual; pausing and stepping do
not change the result.

At the end the emulation pauses and a notification shows the result:

| Result | Meaning |
|--------|---------|
| ended | the replay was played to its end, every hash matched |
| differs at frame N | the state differs from the recording; the notification names the parts (`cpu`, `ram`, `memext`, `vram`, `pcg`, `periph`, `time`) |
| deviated | something wrote into the machine during playback (a debugger write, a breakpoint action `poke` / `set`, Freeze Bytes, a reset); the playback went on to the end, but the hashes were not checked after that |

A difference means that the emulated computer does not behave as during
the recording: other emulator version or settings, or input that is not
recorded (see below).

Playback does not start when a joystick port is connected in the
recording and not here (or the other way round), or when the CMT hack is
switched on in one of them and off in the other: change the setting and
play it again.

## Checkpoints, rewind and seek

While recording and while playing back the emulator keeps checkpoints:
every 250 frames (5 seconds on PAL) a snapshot of the machine in memory,
the last 60 of them (5 minutes). A checkpoint takes tens of kB; the
**Replay** menu shows how many are kept and for which frames.

| Action | Menu / key | MCP |
|--------|------------|-----|
| Rewind while recording | **Replay -> Rewind**, `Alt + Shift + J` | `emu_replay_rewind` |
| Rewind while playing back | **Replay -> Rewind**, `Alt + Shift + J` | `emu_replay_rewind` |
| Jump to a frame while playing back | **Replay -> Seek...** | `emu_replay_seek` |

**Rewind while recording** returns the machine to the previous
checkpoint and drops everything recorded after it; choosing Rewind again
goes one more checkpoint back. The emulation stays paused there and the
recording goes on from that point when it runs again - try another way,
the saved replay contains only the final path. The replay file counts the
rewinds (`rerecord_count`).

**Seek while playing back** jumps to a frame: forward the playback runs
at maximum speed and pauses at that frame; back it starts from the
nearest checkpoint (or from the beginning of the replay) and runs to the
frame. The speed returns to the previous setting afterwards. The hashes
are checked all the way, breakpoints stay active. While seeking, the
picture shows **REPLAY SEEK** and the target frame. Rewind while playing
back jumps to the previous checkpoint.

A checkpoint does not hold the contents of the media. When the emulated
program writes to a floppy disk, Quick Disk or IDE8 disk, records to tape
or the tape is ejected, the checkpoints before that can no longer be
used: a rewind past it while recording is refused (the disk files of the
user are never overwritten), a seek past it while playing back starts
the playback from its beginning with clean copies of the media. No
checkpoint is taken while autotype runs or the tape records.

Checkpoints exist only in memory, not in the replay file. A replay
played in a new process collects its own checkpoints as it runs; a seek
back before the first one starts from the beginning.

The interval and the number are in the `[REPLAY]` section of the
configuration (numbers are hexadecimal there, like everywhere in the
configuration file):

```
[REPLAY]
checkpoint_interval = 0xFA
checkpoint_count = 0x3C
```

`checkpoint_interval = 0` switches the checkpoints off. Over MCP both can
be set for one recording or playback (`checkpoint_interval`,
`checkpoint_count` of `emu_replay_record_start` and `emu_replay_play`).

## Replay Control window

**Replay -> Replay Control...** (`Alt + J`) opens a floating window that
can stay open while you play: the state (off, recording, playing,
seeking), the file, the frame and time (with the length of the replay
while playing back), the number of events and checkpoints, the rewinds,
a deviation and the last result. Its buttons start a recording or a
playback, stop it and rewind; the Seek field jumps the playback to a
frame. The keys are shown next to the buttons. The window stays open
after a restart of the emulator if it was open.

## Media

At the start of a recording the contents of all mounted media are stored
in the replay file:

| Medium | Stored |
|--------|--------|
| floppy disks (all drives of FDC and of the second controller FDC1) | the image as the emulator holds it, including changes not yet saved to the file |
| Quick Disk | the image (also the Unicard loader image) |
| IDE8 disks | the image file |
| tape | the tape file, the current block, the tape speed, custom pulses, polarity and block speeds |
| files chosen in the CMT hack dialog | each chosen file |

Playback mounts copies of these media - also in a new process without
the original files - and works on the copies: the emulated program can
write to a disk or record to tape, the original files never change and
the emulator settings (INI) stay as they are. Drives that were empty in
the recording are empty during the playback. After the playback the
media from the settings are mounted again; a tape that was inserted is
inserted again from its beginning.

All stored media of one recording together may have up to 64 MB. A
larger image (typically an IDE8 disk) is stored only as a SHA-256 hash
and the recording gets a warning; the playback then needs the same image
mounted (it checks the hash and refuses to start otherwise) and works on
a copy of it.

## Checking a replay in CI

```
mz800emu --headless --replay run.mzr --replay-verify --result-json result.json
```

`--replay-verify` plays the replay at MAX SPEED and ends the process at
its end: exit code 0 when every hash matched (`reason: "replay_end"`),
4 when the state differs (`replay_desync`, `replay.outcome_frame` and
`replay.desync_parts` in `result.json`), 3 when the replay cannot be
started (unreadable file, other platform or TV system, other joystick
or CMT hack setting, an image that is not stored and differs). See
[`mcp-server/headless-mode.md`](mcp-server/headless-mode.md).

Note: a correction of the emulator that changes timing changes the
result legitimately - a difference on a new build means "the behaviour
changed", not necessarily a bug.

## What is not recorded

| Input | What to do |
|-------|------------|
| inserting or ejecting a medium during the recording | not available while recording (the Devices menu is disabled, MCP refuses it); mount the media before the recording |
| Unicard SD card (a directory of the host) | play back with the same SD card contents; the recording gets a warning when the Unicard is connected |
| Quick Disk in the virtual (directory) mode | play back with the same directory; the recording gets a warning |
| emulator settings outside the snapshot (INI) | play back with the same settings |

## Replay file

A `.mzr` file is a ZIP archive:

| File | Content |
|------|---------|
| `manifest.json` | format, version, platform, frame length of the TV system (`screen_ticks`), length, hash interval, number of rewinds while recording (`rerecord_count`), warnings, list of media (`media`: slot, file, size, SHA-256, whether it is stored), tape state (`cmt`), start time of the Unicard clock (`rtc_base`), joystick types (`joystick_types`) |
| `start.mzs` | the start snapshot |
| `inputs.jsonl` | one event per line: frame `f`, position `at` (`b` frame boundary, `d` debugger write while running, `p` pause at a frame boundary), kind `k` (`kbd`, `joy`, `autotype`, `reset`, `cmt`, `cmthack`, `dbg`) and its data; the last line is `end` |
| `hashes.csv` | `frame,ticks,cpu,ram,memext,vram,pcg,periph` |
| `media/<slot>.bin` | contents of a stored medium (`fdc0.0`, `qd`, `ide8.0`, `cmt`, ...) |

`inputs.jsonl` is text: an event can be removed or moved to see what a
key changed. The hashes then show where the run differs.
