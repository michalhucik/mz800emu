---
title: CMT cassette workflow - loading, recording, editing and analysing tapes
description: The virtual cassette recorder over MCP - real tape signal vs Direct load vs straight into memory, tapes and 0-based block numbers, every field of the two cassette resources, recording a SAVE into an MZF, editing, WAV analysis, what is refused when, snapshots, limits and recipes.
---
# CMT cassette workflow

Reference for an agent that controls the emulator through MCP and has to
work with the cassette recorder (CMT): load a program from a tape, record
what a program saves, edit a tape, turn a WAV recording into files.

The cassette is driven by the 27 tools `emu_cmt_*`, by
`emu_media_insert` / `emu_media_eject` with `slot="cmt"`, and observed
through two resources: `emulator://periph/cmt` (the deck) and
`emulator://periph/cmt/tape` (the tape and its blocks). A tool that runs
and fails returns `{"error": "..."}` as its text. The arguments are
checked by the MCP library of the server before the tool runs (observed
with the MCP Python package 1.27): an argument it cannot convert to the
tool's parameter type (for example `block_id=1.5`) is refused there as
an error result (`isError` true) with a plain text message. How to read errors in general is in
`emulator://docs/error_handling`. Calls are written in the Python style
of the bundled wrapper (`True`, `False`, `None`); on the JSONL protocol
the same values are `true`, `false`, `null`.

Recipes 1-3 below (load through the ROM, Direct, SAVE) were checked by a
run on the MZ-800, the MZ-700 (PAL and NTSC) and the MZ-1500 with their
standard ROMs; all six recipes were run on the MZ-800 through the MCP
server, the other machines only through the JSONL protocol. The SAVE
recipe (Direct loading left on) was run only through the JSONL
protocol: the monitor save on the MZ-800 (before Direct Save), the ROM
save entries on all four machines (with Direct Save on and off; with
Record pressed the signal is recorded in both cases). The Direct Save
recipe was run only through the JSONL protocol: the monitor save and the
ROM verify entry on all four machines.

## Three ways to get a program into the machine

| | Real tape signal | Direct (CMT hack) | Straight into memory |
|--|--|--|--|
| What happens | The tape plays a cycle-exact signal on the cassette input; the program's own loader reads it | A patch of the ROM load routine copies the MZF of the current tape block into RAM at once | The MZF file is written into RAM without any tape |
| Tools | `emu_cmt_open` (or `emu_media_insert(slot="cmt")`), `emu_cmt_play_paused` / `emu_cmt_play`, then the load command of the ROM or program | `emu_cmt_load_directly(block_id)` or `emu_cmt_hack_set(enabled=True)`, then the ROM load command | `emu_media_run_mzf(path)` (reset + start like `--run-mzf`), `emu_media_load_mzf(path)` (load only) |
| Speed | Real time; `emu_cmt_set_cpu_boost(enabled=True)` runs the emulation at maximum speed while the tape moves | Instant | Instant |
| Works for | Every loader: ROM, turbo, copy protection, multi-part loaders | Only programs that load through the patched ROM routines | One MZF file |
| Fidelity | High | The loader code is skipped | The loader and the ROM are skipped |

Rule of thumb: `emu_media_run_mzf` to just start a program; the real
tape when the loader matters, the program loads further parts itself or
you want to record; Direct when the program loads through the ROM and
you want the parts of a tape fast.

### A defined state for a program

A program may behave differently depending on what ran before it
(interrupt mode, Z80 PIO vector, CTC, palette, ...). To get the state a
real tape load leaves:

- `emu_media_run_mzf(path)` with the default `bootstrap=True`: resets the
  machine and starts the program like the command line option
  `--run-mzf`, PC = exec address. The chips, the memory map, the screen,
  the monitor work area, IM 1, DI and SP are as the ROM leaves them after
  a tape load. Of the CPU registers only BC (the device code, below) and
  the jump register (HL on the MZ-700 and MZ-1500, IX on the MZ-800) are
  set like the ROM; the others are not (a comparison found AF, and on the
  MZ-800 HL and the alternate DE' and HL', on the MZ-700 DE, different
  from the ROM). The result does not depend on the previous state. When
  the emulation is paused, the program waits at its first instruction, so
  breakpoints can be set before `emu_run`.
- BC is the code of the device the program was loaded from; some
  programs choose their default device by it (the BASIC 1Z-016 uses the
  Quick Disk with 0300h). It comes from the setting `CMT/run_mzf_device`
  (`emu_settings_get` / `emu_settings_set`; default `cmt`) or, for one
  call, from `emu_media_run_mzf(path, device=...)`: `cmt` = 0100h, on the
  MZ-700 0000h (what the ROM leaves after a tape load), `fd` = 0200h,
  `qd` = 0300h, `rd` or `sram` = 0000h, or a number 0-65535 (`"512"`,
  `"0x0200"`, `"0200h"`). A decimal number is written without leading
  zeros: `"0200"` is an error, write `"0x0200"` or `"0200h"`. The result
  has `device`
  (the choice used, read by the emulator when it starts the program) and
  `bc`. An invalid `device` is an error and the machine is not reset. The
  command line has `--run-mzf-device`; `result.json` of an automated run
  records it in `inputs.run_mzf_device` and `inputs.run_mzf_bc`.
- `emu_reset`, let the ROM start (`emu_run` with `frames`), then load from
  the tape through the ROM. Slowest, but it runs the real ROM and loader.

`emu_media_run_mzf(bootstrap=False)` and `emu_media_load_mzf` +
`emu_set_register` PC only load the file and jump: everything else stays
as the previous program left it.

## The tape model

Every tape file is converted to a tape in memory and played block by
block. A single MZF or WAV is a tape with one block.

| File | Blocks of the tape |
|--|--|
| `.tmz` | the blocks of the TMZ file (Sharp MZ tape format) |
| `.tzx` | the blocks of the TZX file (ZX Spectrum tape format) |
| `.mzf`, `.m12` | one block with the MZF |
| `.mzt` | one block per MZF in the file; UniCMT CMTSPEED headers are not blocks (see Speeds) |
| `.tap` | one block per TAP block (header or data) |
| `.wav`, `.wave` | one block with the recorded signal (PCM, IEEE float, WAVE_FORMAT_EXTENSIBLE) |

Each block has a `type` and a `status` (fields of
`emulator://periph/cmt/tape`):

| `type` | Block |
|--|--|
| 0 | WAV: the signal of an opened or imported WAV file (the polarity switch applies to it) |
| 1 | MZF: a block that carries an MZF file (MZF and MZT files, TMZ blocks 0x40, 0x41, 0x46) |
| 2 | TAP header |
| 3 | TAP data |
| 4 | another block with a signal (other TZX / TMZ blocks, also a raw recording made by the emulator) |
| 5 | control: a block without a signal (loops, jumps, calls, information, Pause 0, stop-the-tape, unsupported block IDs) |

| `status` | Meaning |
|--|--|
| `ok` | plays |
| `not_decoded` | a raw signal (WAV or TZX 0x18): plays as recorded; `emu_cmt_tape_analyze` finds the files in it |
| `cannot_play` | the player cannot play this block; playback passes over it |
| `control` | a block without a signal; cannot be opened |

- `playable` is true for `ok` and `not_decoded`. Only such blocks can be
  opened (`emu_cmt_tape_seek`, `emu_cmt_tape_step`, playback).
- TMZ / TZX loops, jumps and calls are followed during playback.
- **Pause 0** and the **stop-the-tape** block (0x2A) stop the motor: the
  deck stays in PLAY with `paused` true (`user_paused` false) at the
  block after the command, until the program turns the motor on again or
  `emu_cmt_pause(paused=False)` is called.
- Pauses between blocks come from the blocks (an MZF block opened from
  an MZF or MZT file has a 1000 ms pause; a TAP block 350 ms after a
  header, 1000 ms after data).
- **End of the tape**: the deck stops (STOP) on the last block. With
  `[CMT] autorewind = 1` in the INI file the tape goes back to its first
  block with a signal (default 0).

## Block numbers

**MCP numbers blocks from 0.** This holds for every parameter
(`block_id`, `block`, `block_ids`, `to`, `at`), every result
(`current_block`, `first_added`, `block_id` of findings, `row` of
analysis rows) and every error or warning text of MCP tools (`Block 7
does not exist` means the eighth block).

**The Virtual CMT window numbers blocks from 1**: its display (`Block 2 /
5`), its block list and its messages. When a person reports "block 3"
from the window, it is `block_id` 2 in MCP.

After a deletion, move, import, analysis with `apply`, or a tape
information change that adds or removes the Archive / Machine Info
blocks, the numbers of the following blocks change. Read
`emulator://periph/cmt/tape` again before the next call that takes a
block number. **Pass block numbers as JSON integers.** Through the MCP
server the MCP library first converts the arguments to the tool's
parameter types in a lenient way (observed with the MCP Python package
1.27): `true` becomes 1, `"2"` becomes 2, `0.0` becomes 0, and the tool
then works with that block - a wrong type selects another block instead
of failing. Only a value that cannot be read as a whole number (for
example `1.5` or `"abc"`) is an argument error. On the JSONL protocol the emulator
itself accepts only JSON integers: a string, a decimal number or a
boolean is an invalid parameter there (it is never read as block 0).

## Reading the state

Both resources are read-only, cost nothing in the emulator state and can
be read in any state, also during an input replay.

### `emulator://periph/cmt` - the deck

<!-- field-table: emulator://periph/cmt -->
| Field | Meaning |
|--|--|
| `available` | always true (every machine has the cassette interface) |
| `state` | `"stop"`, `"play"` or `"record"` |
| `paused` | the tape stands in PLAY or RECORD, for any reason |
| `user_paused` | the user's own pause (`emu_cmt_pause`, the deck's Pause button) |
| `motor_on` | without the CMT hack: the motor as the program switched it, `null` = not known (tape stopped); with the CMT hack on: only the motor sense the patch shows the program (every motor command of the program toggles it, true after a start without a tape), it says nothing about the deck |
| `filled` | a tape is inserted (any tape, also a new empty one) |
| `cmthack_enabled` | the CMT hack ROM patch is installed (loading mode Direct) |
| `direct_save` | the patch of the ROM save routines is in ROM now (Direct Save): a ROM SAVE is stored at once as a new block, the ROM VERIFY compares with the current block; false without the CMT hack, with the setting off, with a ROM without the standard tape routines and while the deck records (`state` `"record"`: everything is recorded as signal) |
| `direct_save_enabled` | the setting `CMTHACK/direct_save` (INI `[CMTHACK] direct_save`) |
| `cpu_boost` | maximum speed while the tape moves |
| `polarity_inverted` | the rear DIP switch: signal of WAV blocks inverted |
| `mzfsize_check` | the MZF size check of `.mzf` / `.m12` files (see Speeds and profiles) |
| `cmtspeed` | the default speed (see Speeds; 10 = custom pulse lengths) |
| `default_pulses_us` | pulse lengths of the default speed in microseconds (long high, long low, short high, short low) |
| `custom_pulses_us` | the stored custom pulse lengths, used when `cmtspeed` is 10 |
| `output` | the current bit on the cassette input of the computer |
| `playsts` | internal player phase (a number, for diagnostics only) |
| `start_time`, `paused_time` | GDG clock ticks taken at transport changes |
| `image_basename` | file name of the tape (or of the last opened tape file) without the directory; `""` = none |

`state` and `paused` are independent: PLAY + `paused` true means the
tape stands. Whether the tape moves is always `paused`. Without the CMT
hack it stands while the motor is off or the user's pause is on, and
moves only with the motor on and no user's pause. With the CMT hack on
the program's motor commands do not move it at all (see Loading mode).

### `emulator://periph/cmt/tape` - the tape and its blocks

<!-- field-table: emulator://periph/cmt/tape -->
| Field | Meaning |
|--|--|
| `available` | a tape is inserted |
| `container_type` | 1 = tape with a block list (every inserted tape); 0 without a tape |
| `path` | full path of the tape file; `null` for a new tape without a file (and without a tape) |
| `modified` | the tape has changes not saved to its file |
| `count` | number of listed blocks (at most 256) |
| `truncated` | true when `count` reached 256: the tape may have more blocks |
| `current_block` | the open block; -1 = none (no tape, a new empty tape, a tape with control blocks only) |
| `position_s` | position from the start of the tape in seconds (all blocks before the open one, also beyond the 256 listed, plus the position in it); `null` when unknown |
| `block_position_s` | position in the open block in seconds; `null` when unknown |
| `last_recording` | outcome of the most recent recording that finished or was discarded since the emulator started, `null` if none (see Recording) |
| `blocks` | the block list, entries below |
| `profiles` | the writer profiles a block can be converted to (also without a tape) |

`position_s` is `null` while recording, without a tape, and when the
length of a playable block before the open one is not known yet;
`block_position_s` is `null` while recording, without a tape and when no
block is open.

<!-- field-table: emulator://periph/cmt/tape blocks -->
| Block field | Meaning |
|--|--|
| `block_id` | block number (from 0) |
| `name` | block name (a file name, or the kind of block) |
| `type` | block type, see The tape model |
| `status` | `ok`, `not_decoded`, `cannot_play`, `control` |
| `playable` | the block can be played and opened |
| `is_current` | this is the open block |
| `size`, `load_addr`, `exec_addr`, `file_type` | from the MZF header of a block with an MZF file (file type 1 = machine code, 2 and 5 = BASIC programs - the cassette BASICs 1Z-016 V1.0A and 1Z-013B V1.0A were seen to save their programs with 5, ...); `null` for other blocks |
| `length_s` | length of the block signal in seconds once the emulator knows it (the block was opened or recorded, or the Virtual CMT window computed it), 0 for a control block, else `null`; reading the resource does not compute it |
| `unsaved` | the block is not in the tape file yet (recorded or imported since the tape was opened or saved) |
| `block_speed` | `"default"` (follows the default speed), `"set"` (own speed in `cmt_speed`) or `"none"` (the speed comes from the file) |
| `cmt_speed` | present for every block; the block's own speed only when `block_speed` is `"set"` (1 .. 9 ratio, 10 = custom pulses); for other blocks it does not say what the block plays with |
| `pulses_us` | effective pulse lengths of an MZ block in microseconds (only MZ blocks) |
| `profile` | writer profile of a TMZ 0x46 block (for example `MZ800EMU-CMT`), `null` for other blocks; a block opened from an `.mzf`, `.m12` or `.mzt` file is such a block of `MZ800EMU-CMT` (`convertible` true) |
| `convertible` | `emu_cmt_tape_convert_block` can rebuild the block |
| `recordable` | always false (recording goes into the tape, not into a block) |

<!-- field-table: emulator://periph/cmt/tape last_recording -->
| `last_recording` field | Meaning |
|--|--|
| `serial` | grows with every finished or discarded recording: compare it with the value before a recording |
| `blocks`, `mzf`, `raw` | blocks appended by the recording (all, MZF blocks, raw blocks) |
| `discarded` | the recording was thrown away (eject, open, new tape, snapshot load) |
| `message` | text of the outcome, for example `Recorded "NAME", 1024 B` |

<!-- field-table: emulator://periph/cmt/tape profiles -->
| `profiles` field | Meaning |
|--|--|
| `id`, `description`, `accepts_speed` | a profile: its ID, text, and whether a conversion to it takes a `speed` |

Without a tape: `available` false, `blocks` empty, `count` 0,
`current_block` -1, `path`, `position_s`, `block_position_s` null;
`modified`, `last_recording` and `profiles` are filled as with a tape.

## Transport

| Tool | Effect |
|--|--|
| `emu_cmt_open(path, play_immediately=False)` | Inserts a tape file (extension decides the format). The file is read and checked before the inserted tape is ejected: a file that cannot be opened returns an error and the inserted tape, its unsaved changes and a running recording stay. `play_immediately=True` starts playback in the same call. |
| `emu_media_insert(slot="cmt", path=...)` | The same insertion through the generic media tool (it accepts `expected_state_rev`). |
| `emu_cmt_play()` | STOP -> PLAY. Does nothing (and still answers `ok`) without a tape or when the deck is not in STOP. With Direct loading on, `warning` (see Loading mode). |
| `emu_cmt_play_paused()` | STOP -> PLAY waiting for the motor: the tape moves when the program turns the motor on (not with Direct loading on: then `warning`, see Loading mode). |
| `emu_cmt_pause(paused=True)` | On a moving tape: the user's pause (motor commands of the program then do not move the tape). On a stopped tape: like `emu_cmt_play_paused`. |
| `emu_cmt_pause(paused=False)` | Clears the user's pause and leaves the motor as the program set it; without a user's pause it starts the motor (a tape or recording waiting for the motor starts). |
| `emu_cmt_stop()` | PLAY or RECORD -> STOP. Ends a recording (see Recording). Without a tape it still puts the transport into STOP. |
| `emu_cmt_eject()`, `emu_media_eject(slot="cmt")` | Stops and removes the tape; a running recording is discarded. |
| `emu_cmt_new_tape()` | Ejects the tape and inserts a new empty one (no file, no blocks). |
| `emu_cmt_tape_seek(block_id)` | Opens a block. The transport state is kept; while playing or paused the block plays from its start. A control block or a block that cannot be played cannot be opened. Not while recording. |
| `emu_cmt_tape_step(direction)` | `"next"` / `"prev"` move to the next / previous playable block (the deck's Next / Previous block buttons), `"start"` opens the first playable block (Rewind). Control and unplayable blocks are skipped; the transport is kept. An empty tape gives `The tape is empty`, a tape without a playable block `The tape has no playable block`, the end of the tape `There is no playable block after this one` / `before this one`. |

Results: `emu_cmt_open` reports the real transport after the call
(`playing`, `state`, `paused`), not the request. Its `warning` (texts
joined by `; `, also written to the log) is present when the import of
the file reported a problem, when the tape has blocks the player cannot
play (`Blocks the player cannot play (skipped): 3 (ID 0x19), ...`, at
most 8 listed, then `and N more`), when no block is open (`The tape has
no playable block`, `Block N could not be opened, no block is open`,
`Block N could not be rebuilt at the requested speed, no block is
open`), when `play_immediately` did not start the tape, and with
`Unsaved tape changes were discarded` when the previous tape had unsaved
changes or a recording ran. A block of a supported kind that still gives
no signal is reported only in the log when playback reaches it, and is
passed over.

**Losing changes.** `emu_cmt_open`, `emu_cmt_eject`, `emu_cmt_new_tape`,
`emu_media_insert` / `emu_media_eject` with slot `cmt` and a snapshot
load are done even when the tape has unsaved changes or a recording
runs; the tape tools then return `warning` `Unsaved tape changes were
discarded`. MCP never asks; the Virtual CMT window asks first.

**The motor.** The program switches the cassette motor through the 8255
(every rising edge of PC3 toggles it). Without the CMT hack a program
that starts the motor makes a tape in PLAY waiting for the motor move, and one
that stops it makes the tape stand (`paused` true, `motor_on` false,
`user_paused` false). With the CMT hack on the motor commands do not
reach a tape in PLAY (see Loading mode); a recording they drive also
with the hack on.

## Loading mode: Tape, Boost, Direct

The window's Loading switch has three modes; MCP sets the same state with
two tools. A new configuration has the CMT hack on (`[CMTHACK] enable =
1`) and `cpu_boost` on (`[CMT] cpu_boost = 1`): a fresh emulator is in
Direct mode with `cpu_boost` true. `--cmt` turns the hack off for that
session; a ROM that does not support the hack switches it off.

| Window mode | MCP state |
|--|--|
| Tape | `cmthack_enabled` false, `cpu_boost` false |
| Boost | `cmthack_enabled` false, `cpu_boost` true (`emu_cmt_set_cpu_boost`) |
| Direct | `cmthack_enabled` true, `cpu_boost` true or false: a fresh emulator has true; the window's switch and `emu_cmt_load_directly` set it to false when they turn Direct on; `emu_cmt_hack_set` switches only the patch |

**Direct loading and the motor.** With the CMT hack on, the patch only
answers the ROM's motor sense during playback: the emulated program's
motor commands do not start or stop a playing tape. A tape waiting for
the motor (after `emu_cmt_play_paused`) never starts by itself.
Recording is not affected: in RECORD the program's motor commands drive
the deck and the motor sense reads the deck also with the hack on, so a
SAVE records with Direct on (`emu_cmt_record` returns no `warning`),
through a program's own routine and through the ROM routines alike: in
RECORD the Direct Save patch is not in ROM. Without a recording a SAVE
through the ROM routines is stored directly while `direct_save` is true
(see Direct Save below).
`emu_cmt_play`, `emu_cmt_play_paused`, `emu_cmt_pause(paused=True)`
from STOP and `emu_cmt_open(play_immediately=True)`
therefore return `warning` when the hack is on at the time of the call
and the call really started the tape (a call that did
nothing - no tape, tape already running - returns
none):
`Direct loading (CMT hack) is on: a ROM load reads the tape block directly and the emulated program's motor commands do not start or stop the tape. To let the program drive the motor, stop the tape and call emu_cmt_hack_set(enabled=false); to run the tape regardless of the motor, call emu_cmt_pause(paused=false)` (joined with other warnings by `; `). To let the program
drive the motor during playback (the program's own loader), stop the
tape and switch Direct off first; `emu_cmt_pause(paused=False)`
instead runs the tape regardless of the motor.

`emu_cmt_hack_set(enabled)`:

- has the rules of the main menu item "Use CMT Hack", shared with the
  window's Loading switch: changes the patch only with the deck in STOP;
  otherwise the error `tape is not stopped` and nothing changes. A
  request that changes nothing succeeds in any state. Without a tape the
  patch is switched. Only the window's switch additionally refuses Direct
  when the current block carries no MZF file; the tool does not check
  the block.
- with a ROM that does not support the patch (MZ-800 Willy, MZ-1500
  MZ-NEWMON, a user ROM set without the hack) switching it on is the
  error `The selected ROM does not support the CMT hack` and nothing changes
  (the window cannot switch to Direct then either).
- switching the patch off forgets what it read from the tape (a header
  without its body, the end of the tape).
- does not change `cpu_boost`.
- result `{"ok": true, "installed": bool}`.

`emu_cmt_load_directly(block_id)` is the window's "Load directly" in one
step, all or nothing: the deck must be in STOP and the block must carry
an MZF file (the same test as the window and the `size` field: a file
block whose MZF cannot be assembled does not count). The block is
opened, the patch is switched on and `cpu_boost` off (`mode_changed`
true); when the patch was already on (a fresh emulator), only the block
changes and the boost stays (`mode_changed` false). Result `{"ok": true, "block_id",
"mode_changed", "installed", "current_block"}`. Errors: `No tape
inserted`, `tape is not stopped`, `Block N does not exist`, `Block N
does not contain an MZF file` (control, raw, TAP and other blocks, a
file block with `size` null), `Block N cannot be opened`, `The selected ROM does not support the CMT hack`
(nothing changes, not even the block or `cpu_boost`).

### What the CMT hack does with a tape

A load through the ROM's tape routines goes through the patch while it
is installed: the boot menu key C of the MZ-800 and the MZ-1500, the load
command of the monitors (for example `L` of the MZ-700 monitor), and a
program that calls the ROM entries for the header and the body. A
program with its own tape routines does not. Seen so far: the cassette
BASICs 1Z-016 V1.0A (MZ-800) and 1Z-013B V1.0A (MZ-700), loaded from the
tape into RAM, and a disk BASIC with `SAVE "CMT:name"` / `LOAD
"CMT:name"`. Their SAVE does not reach the ROM body writer at 0475h, and
with Direct on their LOAD waits and the deck stays in STOP (the cassette
BASICs print no `Found`, and SHIFT+BREAK ends the wait with `Break`), so
switch Direct off for them. There are many BASICs and versions and other ones
may behave differently; a LOAD that waits with Direct on while the deck
stays in STOP is the sign of a program with its own tape routines. The
rules for a load through the patch:

- **Tape inserted, current block with an MZF**: the header and then the
  body are copied from the block at once, without the signal (the block
  speed does not matter). A deck that is not in STOP is stopped first.
  After the body the tape moves to the next block with an MZF (in STOP),
  so the next load gets the next program. Blocks with an MZF: type 1
  blocks that can be opened (a TMZ 0x46 block whose MZF cannot be
  assembled, for example a header size different from its body, is not
  type 1).
- **Only the header was read** (a load by name that skips other files):
  the next load first moves past that block.
- **Header size larger than the body** of a 0x40 / 0x41 block: the ROM
  gets a load error and the tape moves on to the next program; seek back
  to try the block again. A smaller header size loads that many bytes.
- **Tape moved between header and body** (`emu_cmt_tape_seek`,
  `emu_cmt_tape_step`, the window): the body still belongs to the header
  the program got, and the tape stays where it was moved.
- **Current block without an MZF, end of the tape (the last program has
  been read) or no tape**: the file dialog as without a tape. Without a
  GUI (headless, pipe) there is no dialog: the load is cancelled at once
  (the ROM reports Break) and a warning goes to stderr. Moving the tape
  clears the end of the tape.
- **While recording** the patch does not use the tape: a ROM load then
  behaves as without a tape (file dialog, Break without a GUI).
- **Reset** (`emu_reset`) forgets the end of the tape and a header read
  without its body; the tape stays where it is, so the next load takes
  the block it stands on.
- Every tape edit, switching the patch off, `emu_media_load_mzf` and
  `emu_media_run_mzf` forget this state too; snapshots store it.

With the patch on, `emu_cmt_tape_seek` and `emu_cmt_tape_step` choose the
block the next load takes. The window disables Play in Direct (except
for a recording waiting for the motor, which Play starts); over MCP
`emu_cmt_play` is accepted, but returns the Direct `warning` and the
program's motor commands do not move the tape (see above); a ROM load
that reads from the tape stops it. To play the signal itself, stop the
deck and `emu_cmt_hack_set(enabled=False)` first. Recording works in
Direct as without it, in the window and over MCP, also for a SAVE
through the ROM routines (see Direct Save). The command line option
`--cmt` turns the patch off for that session.

### Direct Save: SAVE and VERIFY through the ROM

With the CMT hack on, the setting `CMTHACK/direct_save` true (INI
`[CMTHACK] direct_save`, default 1, `direct_save_enabled`) and the deck
not recording, the ROM save and verify routines are patched too
(`direct_save` true in `emulator://periph/cmt`). The rule: without a
recording a ROM SAVE is stored at once; with `emu_cmt_record` everything
is recorded as signal. The patch is
in the routine bodies, so the entries 0021h (@WHEAD), 0024h (@WDATA) and
002Dh (@VERFY) and the monitors' save commands that call the bodies
directly all go through it; a program with its own save routine (the
cassette BASICs named above) does not. Observable behaviour:

- **Save** (header, then body): the ROM still prints `WRITING` and the
  name, the cassette motor is not used, and when the body is written the
  file is appended at once as a new MZF block (`type` 1, `unsaved`) at the
  end of the tape; no `emu_cmt_record` is needed. Without a tape a new
  empty tape is inserted (`path` null), a playing tape (also paused) is
  stopped first. With the deck in STOP the tape then stands at the new
  block (`current_block`). The block is built like a recorded MZF block
  (default speed, `block_speed` `"default"`). The ROM gets OK; when the
  block cannot be added, Break and a line on stderr.
- A header with size 0 is stored at once with an empty body (the ROM
  writes no body then). A header written without its body stores
  nothing; a reset or a snapshot load forgets such a header, and a body
  without a header takes the header at 10F0h.
- **Save while recording** (`emu_cmt_record` before the SAVE): the patch
  is not in ROM in RECORD, the ROM SAVE drives the motor and is recorded
  as signal (the stop decodes it as an MZF block as usual; with a `path`
  the file is written). Entering RECORD forgets a header written without
  its body; after the stop the patch is back.
- **Verify**: compares the memory with the body of the MZF in the current
  block: equal = OK, a different byte or a shorter body = the ROM's error
  (carry set, A = 1). No tape, no current block, a block without an MZF
  = Break (carry set, A = 2) and a line on stderr. A playing tape is
  stopped first, only when the current block carries an MZF; the tape
  does not move. In RECORD the ROM verify routine is the original one.
- A direct save is not a recording: `last_recording` in
  `emulator://periph/cmt/tape` and the result of `emu_cmt_stop` do not
  change. Watch `count` of `emulator://periph/cmt/tape` instead.
- Switch it off with `emu_settings_set(key="CMTHACK/direct_save",
  value="false")`; the patch is removed at once and the ROM bytes are
  the original ones (the setting is saved to the INI file on exit). A
  ROM SAVE then needs `emu_cmt_record` as in Tape mode. Like the main
  menu item CMT Options > Direct Save it works only with the deck in STOP
  (else the error `Stop the tape to switch Direct Save on or off.`) and
  not while an input replay is recorded or played (`Direct Save cannot be
  changed while an input replay is recorded or played.`; while recording
  the replay gate of `emu_settings_set` answers first). An input replay
  stores the setting; with the CMT hack on, playback is refused when it
  differs (`Direct Save of the CMT hack was on when recording, here it is
  off; ...`); a replay recorded before Direct Save (no key) counts as
  off.
- A ROM without the standard tape routines (signature check) gets no
  save patch: `direct_save` stays false (the CMT hack load works as
  before).
- **Verify after a header read** (for example a monitor verify command
  that reads the header through @RHEAD first, then calls @VERFY): the
  verify compares with the MZF of that header and forgets the header
  without moving the tape, so a second verify or a load finds the same
  block.
- **The patch changing while the CPU is inside the routine** (recording
  started or stopped with the machine paused in the middle of a SAVE or
  VERIFY, for example at a breakpoint): when the patch is removed and
  the PC lies inside its bytes, the PC is moved to the next instruction
  boundary of the original code (stdout line `PC 0x.... -> 0x....`): a
  save interrupted before the trap continues as a signal save, after the
  trap it ends with the trap's result. When the patch is put back while
  an original verify is running, that verify can end with OK without
  comparing; an original save interrupted that way ends early or is
  stored directly.

## Recording

Recording captures what the program writes to the cassette output and
appends it to the tape in memory, like a recorder writes on the tape that
is in it.

1. Optional: `emu_cmt_new_tape()`. Without a tape `emu_cmt_record`
   creates a new empty one; with a tape the recording is appended after
   its last block.
2. `emu_cmt_record()` (deck in STOP) arms recording: `state` `"record"`,
   `paused` true. The program's motor command (a ROM SAVE does it) starts
   capturing, and time with the motor off is not recorded - with or
   without Direct loading. This holds for the ROM's save routines (in
   RECORD the Direct Save patch is not in ROM) and for a program's own
   save routine that switches the motor through the 8255. A program that does not switch
   the motor: `emu_cmt_pause(paused=False)` runs the recording (then it
   records continuously, motor or not).
3. Let the program save, then `emu_cmt_stop()`. The recording is decoded
   and appended; the result has `recorded`:
   `{"blocks", "mzf", "raw", "message"}`.
   - A file in the ROM format without errors (header and body with
     correct checksums) becomes an MZF block (`type` 1, `status` `ok`,
     `block_speed` `"default"`).
   - Anything else (another format, a load error, a header without its
     body, a stop in the middle) becomes a raw block (`type` 4, `status`
     `not_decoded`, `block_speed` `"none"`) that plays back as written.
     Fewer than 64 pulses give no block.
   - The deck stands at the first new block, in STOP. The tape is never
     ejected by the stop.
4. The tape is `modified`, the new blocks `unsaved`. Keep it with
   `emu_cmt_save(path)` or take one block out with
   `emu_cmt_export_block(block_id, path)`.

A recording that ended by itself or was discarded has no `emu_cmt_stop`
result: read `last_recording` in `emulator://periph/cmt/tape` and compare
its `serial` with the value from before the recording. It survives
inserting and ejecting tapes.

Automatic end:

- the length limit: 4 hours of recording or 16,000,000 edges (message
  `Recording stopped: length limit reached; ...`);
- `rec_autostop_s` (seconds, 0 .. 600, default 0 = off): after at least
  one edge, that many seconds of emulated time without an edge while the
  motor runs end the recording (`Recording stopped automatically after N
  s of silence; ...`). INI `[CMT] rec_autostop_s`, at run time
  `emu_settings_set(key="CMT/rec_autostop_s", value="N")` (`value` is
  read as a C integer: a leading `0x` is hex, a leading `0` octal; the
  result's `new_value` repeats the text you sent (for example
  `"0x10"`); the number used is shown by `emu_settings_get` or by
  `previous_value` of the next call; the
  value is saved to the INI file on exit; set while an input replay is
  played, it holds only until the playback ends, then the value from
  before the playback is back). Time with the motor off does
  not count, so this does not end a recording after a ROM SAVE, which
  stops the motor at its end;
- a machine reset does not end a recording.

`emu_cmt_record(path)` also writes a file at the stop: `.wav` = the
recording as a 44.1 kHz WAV, `.tmz`, `.tzx`, `.mzt`, `.mzf`, `.m12` =
the whole tape saved (as `emu_cmt_save`). The path must be writable and
the extension one of these, else the start fails (`cmt_record failed
(not in STOP, path not writable or unsupported file type)`). Nothing is
written when nothing was recorded or the recording was discarded.

## Saving and exporting

| Tool | Writes | Notes |
|--|--|--|
| `emu_cmt_save(path=None)` | the tape | Format by extension: `.tmz`, `.tzx` = the whole tape (MZ blocks in a `.tzx` are written in TMZ format with the warning `Tape contains MZ blocks; file saved in TMZ format with .tzx extension`); `.mzt` = the MZF of every block (every block must carry one, else `Tape contains blocks that cannot be stored as MZT`; own block speeds are written as UniCMT CMTSPEED headers, a speed that cannot be written gives the warning `Speed of block N cannot be stored in MZT`); `.mzf`, `.m12` = only a tape with a single MZF block (`Only a tape with a single MZF block can be saved as MZF`). Without `path` the tape's own file (`Tape has no file name` for a new tape). Afterwards `path` is the tape's file, `modified` and all `unsaved` are false. |
| `emu_cmt_export_block(block_id, path)` | one block | `.mzf` / `.m12` = the MZF of the block (error for a block without one), `.wav` = the block signal, 44.1 kHz. The parameter is also accepted as `block` (not both). |
| `emu_cmt_export_tape(path)` | the whole tape as `.wav` | 44.1 kHz 8 bit mono, as the player plays it (pauses, loops, jumps, calls, own block speeds). Blocks that cannot be played are skipped and listed in `warning`. A tape longer than 4 hours is an error (`The tape is longer than 4 hours, not exported`); `The tape is empty` for an empty tape. A recording in progress is not included. |

All three write through a temporary file and a rename: on an error the
old file stays intact. Exports never change the tape, its position or
`modified`.

## Editing the tape

Edits change the tape in memory; the file changes only with
`emu_cmt_save`.

| Tool | Effect |
|--|--|
| `emu_cmt_tape_delete_blocks(block_ids)` | Deletes the listed blocks (numbers of the tape before the deletion, any order, no repeats). |
| `emu_cmt_tape_move_block(block_id, to)` | Moves one block so that it ends at position `to` (0 = first, count - 1 = last). Moving to its own place changes nothing. |
| `emu_cmt_tape_import(path, at=None)` | Inserts all blocks of a file (`.mzf`, `.m12`, `.mzt`, `.tap`, `.wav`, `.tmz`, `.tzx`, converted as by `emu_cmt_open`) before block `at`; omitted, `None` or -1 = at the end. Without a tape it creates a new one (`new_tape` true) when `at` is omitted, `None` or -1; a position 0 or higher without a tape is the error `no tape in the deck`. |

Rules:

- **Deck in STOP**, no recording, no input replay (see What is refused
  when). On any error nothing changes; the text says why (`Block 7 does
  not exist`, `Target position 5 is out of range (0 to 1)`, `Can't import
  <path>: <reason>`).
- **Where the deck stands**: at the block it stood at (at its new
  number), from the start; when that block was deleted, at the block that
  took its place (or the last block). After an import into an empty tape,
  at the first block.
- **Flags**: every edit makes the tape `modified`; imported blocks are
  `unsaved`; moving and deleting keep the `unsaved` flags of the other
  blocks; saving clears both.
- **Pauses**: a block of the emulator profile `MZ800EMU-CMT` with a zero
  pause that gets a block behind it by an import, a move or an analysis
  gets a 1000 ms pause. Deleting does not change pauses.
- **Control blocks** (loops, groups, jumps, calls) can be deleted and
  moved like any block; an unbalanced structure is not refused
  (`emu_cmt_tape_check` reports it).
- **CPU clock warning**: when an edit makes an MZ file block (0x46) play
  at a different CPU clock (a machine info block 0x43 deleted, moved, or
  a block moved or imported across one), `warning` is `The tape clock
  setting now applies to different blocks; check their speed.`
- **Warnings belong to one call**: a later call without a warning returns
  none.

## Tools over the tape

| Tool | Reads or changes |
|--|--|
| `emu_cmt_tape_analyze(block_id, tolerance=0.20, repair_header=False, repair_body=False, repair_bsd=False, apply="none")` | `apply="none"` reads; `"add"` / `"replace"` change the tape |
| `emu_cmt_tape_info(set=None)` | without `set` reads; with `set` changes the tape |
| `emu_cmt_tape_check()` | reads |
| `emu_cmt_tape_convert_block(block_id, profile, speed=None)` | changes the tape |

The reading variants work in any transport state and during an input
replay. The changing variants follow the rules of Editing the tape and
make new or rebuilt blocks `unsaved`.

**Analysis** finds tape files in a raw signal block (`status`
`not_decoded`: an opened or imported WAV, a recording the emulator could
not decode). Each file is a row (`kind` `"file"`); a part of the signal
without a file and at least 64 pulses long is a row with `kind`
`"unknown"`. `result`:

- `addable` - a file with correct header and body checksums in a format
  the emulator can write again as a block: `"NORMAL"` (the standard Sharp
  format), `"CPM-CMT"`, `"TURBO"`, `"FASTIPL"`, `"CPM-TAPE"` or `"BSD"`.
  `profile` names what builds the block: `MZ800EMU-CMT` (NORMAL at the
  default tape speed, CPM-CMT at 2:1 CP/M), `TC-1.21`, `IC-10.2-TURBO`,
  `IC-10.2-FASTIPL`, `PEZIK-2.0A`, `IC-10.2-CPM`, `BASIC-1Z016`, or
  `MZ Turbo Data` (a TURBO or FASTIPL file whose writer was not
  recognized: block 0x41 at the nearest speed). A block of a format
  other than NORMAL carries the same file, but its signal is generated
  again by the profile - the timing of the recording is not kept;
- `addable_repaired` - such a file repaired by an enabled option
  (`repair_body`: an incomplete body filled with 00h, or two damaged
  copies merged; `repair_header`: a header without its body;
  `repair_bsd`: an incomplete BSD data file);
- `stays_raw` - a NORMAL file whose body has a checksum error (a row
  with `kind` `"file"`, its name, `body` `"checksum_error"`, `reason`
  `Checksum error in the body`), and every unknown part;
- `cannot_add` - a format that cannot be written as a block (MZ-80B,
  SINCLAIR, FSK, SLOW, DIRECT, ZX Spectrum), or a file its writer
  profile refuses (`reason` `The file cannot be rebuilt as a tape block
  in this format`, for example a TAPE.COM body over 16 KB or a BSD file
  over 254 pieces); `profile` is `""`.

A file whose signal ends inside its body is **not** reported as a file
without `repair_body`: that part is a row with `kind` `"unknown"`,
`name` `""`, `header` and `body` `"not_available"`, `result`
`stays_raw`, `reason` `No file found in this part of the signal`. With
`repair_body=True` it becomes a row with its name, `header` `"ok"`,
`body` `"checksum_error"`, `recovery` `"partial body"`, `result`
`addable_repaired` (the missing bytes are 00h). A header without a
single byte of body is a file row only with `repair_header=True`. So an
unknown row in a recording that should hold a file is worth one more
analysis with `repair_body=True`.

Every call analyses the block again with **its own** options, and
`apply` adds exactly the rows that are `addable` or `addable_repaired`
in that analysis: give the same repair options in the call with `apply`,
otherwise the repaired file is not added (`applied` 0).

`reason` says why a row cannot be added or that a repair was used (`""`
for `addable`). `apply="add"` inserts every addable row as a file block
right after the raw block (the raw block stays); `apply="replace"`
replaces the raw block with the file blocks and keeps every row that is
not added in raw blocks, in the original order (unknown parts shorter
than 64 pulses next to a file are dropped). File blocks use the profile
`MZ800EMU-CMT` at the default speed, 1000 ms apart. `tolerance` (leader
detection) must be 0.02 .. 0.35. `analysis_error` appears when the block
has no signal; a block that is not a raw signal is an error (`Block 0 is
not a raw signal block (CSW recording)`).

**Tape information** is the TZX Archive Info block and the target machine
(Machine Info). The ten texts are `title`, `publisher`, `author`,
`year`, `language`, `type`, `price`, `protection`, `origin`, `comment`.
`set` writes the given fields in one step (fields not given keep their
value): a text `""` removes the entry, `"machine": null` removes the
machine block. Texts are ISO 8859-1, at most 255 characters; other
characters become `?` and `warning` names the fields. The Archive Info
block is the first block of the tape and Machine Info follows it; they
are inserted when missing and removed when empty, so other block numbers
shift. A tape opened from `.mzf`, `.m12` or `.mzt` (`storable` false) can
keep the information only when saved as `.tmz` or `.tzx`.

**Check** lists structural problems: unbalanced loops and groups, jumps
and calls outside the tape, calls without a return, unknown block IDs, MZ
file blocks that cannot be parsed, a header without the 0x0D name
terminator, an unknown file type, a body length different from the
header size (`severity` `"warning"`), and profiles the emulator does not
know (`"info"`). Body checksums are not checked. At most 500 findings; a
final entry with `block_id` -1 says the list was cut.

**Profile conversion** rebuilds a TMZ 0x46 block (`convertible` true)
with another writer profile (an `id` from `profiles`); the file bytes
stay, the signal changes. Settings of the old profile that the new one
accepts are carried over, the rest is listed in `warning`. `speed` (a
ratio key or 1 .. 9, no custom pulses) only for profiles with
`accepts_speed` (`Profile X does not accept a speed`); `Unknown profile:
X` for a wrong ID. The block's own profile without a speed changes
nothing (`changed` false).

### Result fields

<!-- result-table -->
| Tool | Result fields (besides `ok`) |
|--|--|
| `emu_cmt_open` | `path`, `playing`, `state`, `paused`, `warning` |
| `emu_cmt_stop` | `action`, `recorded` (`blocks`, `mzf`, `raw`, `message`) |
| `emu_cmt_eject` | `action`, `warning` |
| `emu_cmt_play` | `action`, `warning` |
| `emu_cmt_play_paused` | `action`, `warning` |
| `emu_cmt_record` | `path` |
| `emu_cmt_hack_set` | `installed` |
| `emu_cmt_load_directly` | `block_id`, `mode_changed`, `installed`, `current_block` |
| `emu_cmt_tape_step` | `direction`, `block_id` |
| `emu_cmt_tape_seek` | `block_id` |
| `emu_cmt_save` | `path`, `message`, `warning` |
| `emu_cmt_export_block` | `block`, `block_id`, `path`, `message` |
| `emu_cmt_export_tape` | `path`, `message`, `warning` |
| `emu_cmt_tape_delete_blocks` | `deleted`, `count`, `current_block`, `warning` |
| `emu_cmt_tape_move_block` | `block_id`, `to`, `count`, `current_block`, `warning` |
| `emu_cmt_tape_import` | `path`, `at`, `imported`, `count`, `current_block`, `new_tape`, `warning` |
| `emu_cmt_tape_analyze` | `block_id`, `apply`, `sample_rate`, `pulse_count`, `length_s`, `analysis_error`, `rows`, `applied`, `first_added`, `count`, `current_block`, `warning` |
| `emu_cmt_tape_analyze` | rows: `row`, `kind`, `name`, `type`, `format`, `start_s`, `length_s`, `size`, `header`, `body`, `result`, `reason`, `recovery` |
| `emu_cmt_tape_info` | the ten texts, `machine`, `machine_code`, `storable`, `other_entries`, `other_archive_blocks`, `other_archive_block`; after a write also `changed`, `warning` |
| `emu_cmt_tape_check` | `blocks_checked`, `count`, `findings` (`block_id`, `severity`, `text`) |
| `emu_cmt_tape_convert_block` | `block_id`, `profile`, `changed`, `count`, `current_block`, `warning` |
| `emu_cmt_tape_set_block_speed` | `block_id`, `speed`, `pulses_us` |
| `emu_cmt_set_speed` | `property`, `value`, `pulses_us` |

`count` and `current_block` in edit results describe the tape after the
call (`current_block` -1 = no open block). `first_added` is the number
of the first added file block (`null` when none). `machine` is
`"generic"`, `"mz700"`, `"mz800"`, `"mz1500"`, `"mz80b"` or `null`;
`machine_code` is the raw value (-1 = none). Tools that change the
machine state also return `state_rev` (see Limits and costs).

## Speeds and profiles

The default speed applies to blocks with `block_speed` `"default"`. Set
it with `emu_cmt_set_speed(speed)` (a ratio key or its number) or
`emu_cmt_set_speed(pulses_us=[...])`:

| Key | Number | Note |
|--|--|--|
| `1:1` | 1 | standard 1200 Bd |
| `2:1` | 2 | |
| `2:1_cpm` | 3 | CP/M variant |
| `3:1` | 4 | |
| `3:2` | 5 | |
| `7:3` | 6 | Intercopy 10.2 |
| `8:3` | 7 | CP/M cmt.com |
| `9:7` | 8 | |
| `25:14` | 9 | |
| `custom` | 10 | the stored custom pulse lengths |

- `pulses_us = [long_high, long_low, short_high, short_low]` in
  microseconds, each in (0, 65535], the order of the UniCMT CMTSPEED
  header (UniCMT 1x = `[470, 494, 240, 278]`, 3x = `[156, 164, 80, 92]`).
  `emu_cmt_set_speed(pulses_us=[...])` stores them and switches the
  default to 10; `emu_cmt_set_speed("custom")` switches back to the
  stored lengths later.
- With the deck in STOP a change of the default speed rebuilds the open
  block when it follows the default speed and rewinds it to its start (a
  position set in STOP is lost); other blocks take it when they are
  opened. The known lengths of the blocks with the default speed are
  dropped, so their `length_s`, and `position_s` behind them, can be
  `null` until they are known again.
- `emu_cmt_tape_set_block_speed(block_id, speed)` (ratio 1 .. 9) or
  `emu_cmt_tape_set_block_speed(block_id, pulses_us=[...])` gives one
  block its own speed (`block_speed` `"set"`). It works in any transport
  state and applies from the next opening of the block; the open block is
  not rebuilt. A real change marks the tape `modified`; saving writes the
  block at its new speed. Pulse lengths are stored in whole microseconds.
  The known block lengths are dropped (`length_s` and `position_s` can be
  `null` afterwards).
- Speed can change only for blocks from MZF, MZT and TAP files and TMZ
  0x46 blocks of the profile `MZ800EMU-CMT`; TAP blocks take ratios only.
  Other blocks and WAV play as recorded (`block_speed` `"none"`); convert
  a block of another profile back to `MZ800EMU-CMT` to change its speed.
- In an `.mzt` file, a UniCMT CMTSPEED header (an MZF header named
  `CMTSPEED` with no body) is not a block: the following blocks play with
  its pulse lengths (`block_speed` `"set"`, `cmt_speed` 10).
- `emu_cmt_set_polarity(inverted)` inverts the signal of WAV blocks
  (`type` 0) only; other blocks, the recording and `emu_cmt_export_tape`
  are not affected.
- `emu_cmt_set_mzfsize_check(enabled)`: when a `.mzf` / `.m12` file longer
  than its header says (header size + 128 bytes) is opened as a tape, or
  loaded by `emu_media_load_mzf`, `emu_media_run_mzf` or the CMT hack file
  dialog, a line `MZF file size check: ...` goes to the emulator output
  and a dialog of the emulator GUI offers to fix the file; the file is
  used either way. Nothing is refused, but in the GUI the user gets the
  dialog. On by default (`[CMT] mzfsize_check = 1`).
- `emu_cmt_set_cpu_boost(enabled)`: maximum speed while the tape moves
  (PLAY or RECORD without pause; while recording the boost pauses after
  5 s without tape writes). It switches off only the maximum speed it
  switched on itself.
- These options are user settings, not machine state: the default speed
  and custom pulses, polarity, CPU boost, MZF size check, `autorewind`,
  `rec_autostop_s` (INI section `[CMT]`) and the CMT hack (`[CMTHACK]
  enable`, so also after `emu_cmt_load_directly`) are saved to the INI
  file on exit (unless the emulator runs with `--no-save-ini`).

## What is refused when

| Situation | Refused | Error text |
|--|--|--|
| No tape | `emu_cmt_tape_step`, `emu_cmt_load_directly`, `emu_cmt_tape_check`, `emu_cmt_tape_info`, edits (except an import at the end), `emu_cmt_tape_analyze`, `emu_cmt_tape_convert_block`, `emu_cmt_save`, `emu_cmt_export_block`, `emu_cmt_export_tape` | `No tape inserted` |
| No tape | `emu_cmt_tape_import` with `at` >= 0 | `no tape in the deck` |
| No tape | `emu_cmt_tape_seek`, `emu_cmt_tape_set_block_speed` | `cmt_tape_seek failed (...)`, `cmt_tape_block_speed failed (...)` |
| No tape | `emu_cmt_play`, `emu_cmt_play_paused` | not refused: `ok`, but nothing happens - check `state` |
| Deck not in STOP (PLAY, PLAY paused, RECORD) | edits, `emu_cmt_tape_analyze` with `apply`, `emu_cmt_tape_info` with `set`, `emu_cmt_tape_convert_block`, `emu_cmt_load_directly`, `emu_cmt_hack_set` when it would change the patch | `tape is not stopped` |
| Deck not in STOP | `emu_cmt_record` | `cmt_record failed (not in STOP, path not writable or unsupported file type)` |
| Deck not in STOP | `emu_cmt_play` | not refused: `ok`, nothing happens (resume with `emu_cmt_pause(paused=False)`) |
| Recording (RECORD) | `emu_cmt_tape_step` | `Not available while recording`; the checks run in the order `No tape inserted`, `The tape is empty`, `Not available while recording`, `The tape has no playable block`, so a recording onto an empty tape answers `The tape is empty` |
| Recording (RECORD) | `emu_cmt_tape_seek` | `cmt_tape_seek failed (no tape, recording, ...)` |
| Input replay being recorded | `emu_cmt_open`, `emu_cmt_set_speed`, `emu_cmt_set_polarity`, `emu_cmt_set_cpu_boost`, `emu_cmt_set_mzfsize_check`, `emu_cmt_tape_set_block_speed`, `emu_cmt_hack_set`, `emu_cmt_load_directly`, `emu_cmt_save`, edits, `emu_cmt_tape_analyze` with `apply`, `emu_cmt_tape_info` with `set`, `emu_cmt_tape_convert_block`, `emu_media_insert` / `emu_media_eject`, `emu_settings_set`, `emu_snapshot_load` | `command <name> is not allowed while recording a replay [...]` (`<name>` is the internal command, for example `cmt_set_property`, `cmt_tape_analyze_apply`, `cmt_tape_info_set`) |
| Input replay being recorded, emulator paused between frame boundaries | `emu_cmt_play`, `emu_cmt_play_paused`, `emu_cmt_stop`, `emu_cmt_pause`, `emu_cmt_eject`, `emu_cmt_record`, `emu_cmt_new_tape`, `emu_cmt_tape_seek`, `emu_cmt_tape_step` | `command <name> changes the machine in a pause between frame boundaries; ...` (run to a boundary: `emu_run(frames=1)`) |
| Input replay being played | `emu_cmt_save` | `Saving the tape is not allowed while a replay is played` |
| Input replay being played | `emu_cmt_record` with `path` | `Recording to a file is not allowed while a replay is played` |
| Input replay being played (deck in STOP) | edits, `emu_cmt_tape_analyze` with `apply`, `emu_cmt_tape_info` with `set`, `emu_cmt_tape_convert_block` | `The tape cannot be edited while an input replay is recorded or played` |
| Input replay recorded or played | `emu_cmt_hack_set`, `emu_cmt_load_directly` | `The loading mode cannot be changed while an input replay is recorded or played` (while recording the replay gate answers first, see above) |
| ROM without CMT hack support (MZ-800 Willy, MZ-1500 MZ-NEWMON, a user ROM set without the hack) | `emu_cmt_hack_set(enabled=True)` when the patch is off, `emu_cmt_load_directly` when the patch is off | `The selected ROM does not support the CMT hack` (nothing changes) |

Always allowed (also during an input replay, not recorded in it): both
resources, `emu_cmt_export_block`, `emu_cmt_export_tape`,
`emu_cmt_tape_analyze` without `apply`, `emu_cmt_tape_info` without
`set`, `emu_cmt_tape_check`. Recorded into an input replay (as tape
control events): `emu_cmt_play`, `emu_cmt_play_paused`, `emu_cmt_stop`,
`emu_cmt_pause`, `emu_cmt_eject`, `emu_cmt_record`, `emu_cmt_new_tape`,
`emu_cmt_tape_seek`, `emu_cmt_tape_step`. During playback of an input
replay every tool of this document that is neither refused above nor
always allowed runs and marks the playback `deviated` - also the tape
control just listed (`emu_cmt_play`, `emu_cmt_tape_step`, ...).

## Snapshots and input replay

**A snapshot stores** the transport (with the user's pause and the
motor), the tape path as it was given, the open block and the position in
it, the block speeds and the player state (loops, calls), and the CMT
hack state on the tape. The content of a tape is stored only when the
tape has unsaved changes or no file (up to 16 MB); otherwise only the
path. The options `cpu_boost`, `polarity_inverted`, `cmtspeed` and
`mzfsize_check` are written but not restored: the current values stay
(a block that followed a different default speed in the snapshot gets
that speed as its own).

**Loading a snapshot** (`emu_snapshot_load`):

- reopens the tape file from the stored path and restores block,
  position, speeds and transport; a tape already in the deck with the
  same path and block count is used as it is;
- a stored tape content is inserted without a path (`path` null,
  `modified` true) unless the deck holds a tape with exactly that path;
- discards a recording in progress; a snapshot taken while recording
  loads in STOP;
- fires no `cmt:state_change` / `cmt:mstate` breakpoint events.

Problems do not fail the load; they are warnings on the emulator's stderr
(`Snapshot [cmt]: WARN: ...`), not in the tool result, so read
`emulator://periph/cmt/tape` after a load. The cassette warnings:

| Warning | Result |
|--|--|
| `Snapshot tape file not found: <path>`, `Snapshot tape file cannot be opened: <path>` | no tape, STOP |
| `Snapshot tape does not match file: <path>` | tape inserted, STOP at the first block |
| `Snapshot tape on a network path is not opened automatically: <path>` | no tape (paths starting with `\\` or `//`); insert that tape with exactly that path first, then load |
| `Unsaved tape restored from the snapshot (was: <name>)` | stored tape content inserted, `path` null |
| `Snapshot tape path on a network location was dropped` | stored content inserted without its network path |
| `Snapshot embedded tape is invalid: <path>`, `Snapshot tape was too large to embed: <path>` | no tape, STOP |
| `Snapshot embedded tape: <text>` | a warning of opening the stored content |
| `Snapshot tape player state is invalid, playback continues from block N` | loops and calls not restored |
| `Snapshot CMT hack tape state is invalid, ignored` | the CMT hack starts without the header or end of tape it had |
| `Snapshot was taken while recording; the recording was not restored` | STOP |
| `Tape transport was active in the snapshot, but no matching tape is inserted - transport set to STOP` | STOP |
| `Cassette reference file '<file>' recorded, but will not be auto-opened` | a snapshot with a tape path but without tape data: the file is not opened, only the transport state is applied |

When saving, `Snapshot: unsaved tape is too large to embed` or `Snapshot:
unsaved tape cannot be embedded` means the content of a tape with unsaved
changes is not in the snapshot.

**Input replay** (`emulator://docs/replay`): the tape is stored at the
start of the recording with its blocks and speeds, playback works on a
copy (your tape file is never written), and going back to a checkpoint
returns the tape exactly (blocks appended by a recording are removed). A
tape with unsaved changes that does not fit into the replay refuses the
start with `Tape with unsaved changes is too large for an input
recording; save the tape first`. Edit, save and convert the tape before
starting an input recording; which tools are refused or recorded is in
What is refused when.

## Command line

The emulator can start with a tape in the deck. The options also work
with `--mcp-pipe`.

| Option | Effect |
|--|--|
| `--cmt <file>` | Insert the tape (`.tmz`, `.tzx`, `.mzf`, `.m12`, `.mzt`, `.tap`, `.wav`, `.wave`) in STOP. A CMT hack that is on is turned off for this session only (the INI keeps its value). |
| `--cmt-play` | Also press PLAY. A normal ROM start stops the motor, so the tape is then in PLAY waiting for the motor (`paused` true, `user_paused` false); it plays at once only together with `--run-mzf`. |
| `--cmt-play-pause` | PLAY waiting for the motor (`paused` true, `user_paused` false): the tape moves when the ROM turns the motor on (for example after LOAD). |
| `--cmt-boot` | MZ-800 only: like `--cmt-play-pause`, and the C key is held from reset until the motor turns on, so the boot menu loads the first program from the tape. |

A modifier without `--cmt`, `--cmt-play` with `--cmt-play-pause`,
`--cmt-boot` with `--cmt-play` or `--run-mzf`, and a missing file or an
unknown extension end the emulator before it starts, with an error on
stderr. Pipe mode does not accept `--run-mzf` (any option it does not
know ends it with `Unknown option: <option>`); start the first part with
`emu_media_run_mzf` instead. For a program that loads further parts
through the ROM: `--mcp-pipe --cmt rest.mzt --cmt-play-pause`, then
`emu_media_run_mzf(path="part1.mzf")`; the tape is in PLAY waiting for
the motor (`paused` true, `user_paused` false) until the program turns
the motor on. Check `filled`, `state`, `paused`,
`cmthack_enabled` in `emulator://periph/cmt`.

## Limits and costs

- **Synchronous work stops the emulation.** Opening or importing a file
  (a long WAV), saving, exporting and the analysis run in the emulator
  thread: the emulation waits until they finish. A whole-tape export is
  built in memory and written at once (estimated several hundred MB for
  a 4-hour tape); the emulation and the emulator window wait. The analysis keeps
  the window drawing.
- **Time limits.** The emulator answers a started command within about
  20 s, or about 610 s for `emu_cmt_open`, `emu_cmt_record`,
  `emu_cmt_tape_import`, `emu_cmt_export_tape`, `emu_cmt_tape_analyze`
  and `emu_media_insert` / `emu_media_eject`; after that the answer is `Emulator busy:
  command still running ...` and the result is unknown (the command may
  still finish). The bundled Python wrapper waits 30 s per request, 640 s
  for `emu_cmt_tape_analyze`. After any timeout read
  `emulator://periph/cmt/tape` before repeating a changing call.
- **Block listing**: at most 256 blocks in `emulator://periph/cmt/tape`
  (`truncated` true). Blocks beyond can still be used by number;
  `position_s` counts them.
- **Recording**: 4 hours or 16,000,000 edges; `rec_autostop_s` 0 .. 600.
- **Export of the whole tape**: at most 4 hours.
- **Check**: at most 500 findings. **Tape information texts**: 255
  characters, ISO 8859-1.
- **Snapshot**: tape content up to 16 MB.
- **State revision** (`emulator://docs/state_revision`): tools that
  change the machine (transport, recording, loading mode, speeds, open,
  new tape, save, edits, analysis with `apply`, information with `set`,
  conversion) return `state_rev`. The reading tools (resources,
  `emu_cmt_export_block`, `emu_cmt_export_tape`, analysis without
  `apply`, information without `set`, check) return none and do not move
  it. The `emu_cmt_*` tools have no `expected_state_rev` parameter; on
  the JSONL protocol the changing `cmt_*` commands accept it, the reading
  ones refuse it.

## MCP compared with the Virtual CMT window

The window and MCP change the same deck with the same rules, except:

- The window numbers blocks from 1, MCP from 0 (see Block numbers).
- The window asks before discarding unsaved changes; MCP never asks and
  returns `warning`.
- The window allows Export tape as WAV only in STOP; `emu_cmt_export_tape`
  works in any state.
- In Direct mode the window disables Play; `emu_cmt_play` still works
  (with the Direct `warning`).
- The window changes the signal speed and a block's speed only in STOP
  and rebuilds the open block; `emu_cmt_set_speed` and
  `emu_cmt_tape_set_block_speed` work in any state (see Speeds).
- The window runs the analysis in the background; MCP runs it
  synchronously.
- The window computes block lengths for its counter while the tape is
  stopped; the resource reports only lengths already known
  (`length_s` null otherwise).
- The fix offered by the MZF size check is a dialog of the GUI; MCP only
  sets the option.
- Not in MCP: the main menu option Fix FNAME Terminator, a seek to a
  time inside a block.

## Recipes

The recipes are written for a fresh MZ-800 emulator (Direct loading and
CPU boost on) that shows its boot menu (after the start or `emu_reset`;
`emulator://video/text_dump` shows the screen). On the other machines
the keys differ (two other differences, the MZ-700 monitor's name with a
leading space and the stop while typing, are described at the recipes).
The column "Run code" starts a program placed in memory, for example one
that saves through the ROM entries:

| Machine | After the start | Load from tape | Monitor | Run code at 6000h | Save in the monitor (4000h-4C9Fh, exec 4340h) |
|--|--|--|--|--|--|
| MZ-800 | boot menu | `C` | `M` | `G6000\n` | `S\nNAME\n4000\n4C9F\n4340\n` |
| MZ-700 | monitor (prompt `*`), no boot menu | `L\n` (loads and starts the program) | already there | `J6000\n` | `S40004C9F4340\nNAME\n` (start, end, exec address, then the name at `FILENAME?`) |
| MZ-1500 | boot menu | `C` | `M` | `J6000\n` | `S\nNAME\n4000\n4C9F\n4340\n` |

Keys are typed with `emu_input_send_keys`: `\n` types Enter (`\r` and
`\r\n` do too); a character the keyboard cannot type is skipped and
counted in the result's `skipped`, with a `warning`; one call types at
most 256 keys, the rest is counted in `truncated`, with a `warning`.
`keys_landed` and `landing_verified` only say that the program read the
keyboard while the key was held, not that it acted on the key: the boot
menu of the MZ-800 and the MZ-1500 misses a key held for 3 frames at
some moments (with `keys_landed` 1), and took it every time when held
for 6 frames, the default of `emu_input_send_keys` (see
`emulator://docs/mz800_keyboard`). Check the screen. The boot menu
itself shows the line `M:MONITOR`, so
the word MONITOR on the screen does not prove the monitor: check that
the menu lines (the `C:` cassette line, for example) are gone. A key the menu
did not take is simply typed again.

**Waiting for a stop after typed keys.** `emu_run(frames)` runs at most
1000 frames per call, so the recipes wait for long operations with
`emu_resume(deadline_frames=...)` and repeated `emu_wait_stop` (at most
25 s of real time per call); a deadline of 30000 frames is 10 minutes of
emulated time at 50 Hz (8 min 20 s at 60 Hz: MZ-700 NTSC, MZ-1500), with
the CPU boost it passes in seconds. A breakpoint can
stop the machine while the keys are still being typed (Direct loading
with the MZ-700 monitor's `L` loads and starts the program right after
Enter): then `emu_input_send_keys` returns `interrupted` true, the
machine is paused at that stop, and resuming would run past it. The
recipes therefore use this helper after typing:

```python
def run_and_wait(k, frames):                  # k = the result of emu_input_send_keys
    if k["interrupted"]:                      # stopped while the keys were typed
        return emu_wait_stop()                # paused: the record of that stop
    s = emu_resume(deadline_frames=frames)
    for i in range(30):                       # at most 30 x 25 s of real time
        w = emu_wait_stop(run_seq=s["run_seq"], timeout_ms=25000)
        if w["done"]: break
    return w
```

`interrupted` is also true after a pause from outside (the record's
`reason` is then `"manual"`) or a machine reset. After a reset the
machine keeps running and `emu_wait_stop()` returns `done` false after
its 5 s: check `w["done"]` before reading `w["record"]`, and pause the
emulator (`emu_pause()`) when it is false.

### Load a program from a tape file through the ROM

```python
emu_pause()
emu_cmt_stop()                                # a known deck state: STOP
c = emulator://periph/cmt                     # a fresh emulator: c["cmthack_enabled"] true
if c["cmthack_enabled"]: emu_cmt_hack_set(enabled=False)   # the real signal needs Direct off
emu_cmt_open(path="game.mzt")                 # -> {"state": "stop", "playing": false, ...}
t = emulator://periph/cmt/tape                # t["current_block"]: the block the load reads
e = t["blocks"][t["current_block"]]["exec_addr"]
bp = emu_bp_add(addr=e)                       # end mark: the program starts
emu_cmt_play_paused()                         # waits for the motor; no "warning" with Direct off
k = emu_input_send_keys(text="C")             # boot menu: C = cassette (MZ-700 monitor: "L\n")
w = run_and_wait(k, 30000)
# w["done"] and w["record"]["reason"] "breakpoint" and w["record"]["pc"] == e: loaded and started;
# "deadline": not started in time - look at emulator://periph/cmt and the screen
emu_bp_remove(id=bp["id"])
```

During the load `state` is `"play"` and `paused` false. The ROM stops
the motor after the header and again after the body (`paused` true,
`motor_on` false, `user_paused` false), so a stopped motor alone does not
mean the end; `current_block` / `position_s` show where the tape stands.
A program that a BASIC loads does not start: use the BASIC prompt on
the screen as the end mark instead of the exec address (and a BASIC with
its own tape routines, such as 1Z-016 V1.0A or 1Z-013B V1.0A, needs
Direct off, see What the CMT hack does). With `--cmt <file>
--cmt-play-pause` on the command line the tape is already waiting at
start.

### Load one block directly (Direct)

```python
emu_pause()
emu_cmt_stop()                                # the deck must be in STOP
c = emulator://periph/cmt                     # remember c["cmthack_enabled"], c["cpu_boost"]
emu_cmt_open(path="games.mzt")
t = emulator://periph/cmt/tape
# choose the block by its fields, not by a fixed number:
n = first b in t["blocks"] with b["size"] != None and b["name"] == "GAME"   # -> b["block_id"]
r = emu_cmt_load_directly(block_id=n)         # -> {"block_id": n, "installed": true, "current_block": n,
                                              #     "mode_changed": false in a fresh emulator (Direct was on)}
bp = emu_bp_add(addr=t["blocks"][n]["exec_addr"])
k = emu_input_send_keys(text="C")             # boot menu: the ROM load copies header and body at once
w = run_and_wait(k, 3000)                     # MZ-700 "L\n": the stop comes while typing (interrupted)
# w["done"] and w["record"]["reason"] "breakpoint", w["record"]["pc"] == the exec address
emu_bp_remove(id=bp["id"])
```

`mode_changed` is true when Direct was off before; the call then also
switched `cpu_boost` off. After the load `current_block` is the next
block with an MZF (or the same block at the end of the tape). Back to the
state from before: `emu_cmt_stop()`, then
`emu_cmt_hack_set(enabled=c["cmthack_enabled"])` and
`emu_cmt_set_cpu_boost(enabled=c["cpu_boost"])`.

### Save through the ROM with Direct Save and get an MZF file

In a fresh emulator (`cmthack_enabled` and `direct_save` true) a SAVE
through the ROM routines needs no recording: the file is a new block at
once (see Direct Save).

```python
emu_pause()
emu_cmt_stop()                                # a known deck state: STOP
c = emulator://periph/cmt                     # c["direct_save"] true: the ROM SAVE is stored directly
t = emulator://periph/cmt/tape                # t["modified"] true: save the user's tape first
emu_cmt_new_tape()                            # an empty tape: the saved file is block 0
# start the SAVE; here the MZ-800 monitor (boot menu key M): S, name, start, end, exec address
k = emu_input_send_keys(text="S\nNAME\n4000\n4C9F\n4340\n")
w = run_and_wait(k, 600)                      # the SAVE takes no tape time; "deadline" is expected
t = emulator://periph/cmt/tape                # t["count"] 1, t["blocks"][0]["type"] 1: the file is there
emu_cmt_export_block(block_id=0, path="saved.mzf")
```

A program that checks the file through the ROM entry 002Dh (@VERFY)
compares the memory with the block the tape stands at, which right after
the SAVE is the saved file. Without `emu_cmt_new_tape`
the file is appended after the last block of the inserted tape and the
tape stands at it (`current_block`). A program with its own save routine
does not go through the patch: record it as below.

### Record a SAVE and get an MZF file

Direct loading and Direct Save may stay on: with `emu_cmt_record` a
SAVE records the same way in every loading mode, through the ROM
routines as well as through a program's own save routine (see
Recording).

**When is the SAVE finished?** Not when the motor stops: the ROM writes
the header, switches the motor off, and switches it on again for the
body, so `paused` true also appears between the two parts. Stopping the
deck there leaves a raw block with only the header; with Direct loading
and Direct Save on, the stop puts the patch back into ROM, so the body
the ROM then writes is stored directly as a complete MZF block after the
raw header block (header taken from 10F0h) and the tape stands at it.
The SAVE is
finished when the program returns from writing the body. In the standard
ROM of all three machines the body writer starts at 0475h and the entry
0024h (@WDATA) is `JP 0475h`; the monitors' save commands call 0475h
directly, not through 0024h. A breakpoint at 0475h therefore stops both
ways, while the lower ROM is mapped, and the word on the stack there is
the return address of the call. A program with its own save routine (a
turbo saver, a game, the `SAVE` of the cassette BASICs 1Z-016 V1.0A and
1Z-013B V1.0A, a disk BASIC's `SAVE "CMT:name"`) does not get there: the
run ends with `deadline`, and the end of its SAVE is its own mark (its
prompt such as `Ready`, a message on the screen, a breakpoint where it
continues). Its recording works with Direct on as well, as long as it
switches the motor through the 8255. (The two cassette BASICs were run
with Direct off and with Direct and Direct Save on: their `SAVE` recorded
a clean MZF block, file type 5, in both cases.)

A breakpoint is set on an address, not on the ROM: a program that maps
RAM at 0000h (both cassette BASICs above do) has its own code at the ROM
addresses, and a breakpoint there stops in that code - with 1Z-013B a
breakpoint at the ROM address 04D8h stopped while the `LOAD` command was
still being typed. So check at the stop that the lower ROM is mapped:
`emulator://memory/map` gives the banking of that moment, and its first
slot (0000h-0FFFh) has `kind` `"rom_low"` only when the lower ROM is
there.

```python
emu_pause()
emu_cmt_stop()                                # a known deck state: STOP
t = emulator://periph/cmt/tape                # t["modified"] true: save the user's tape first
emu_cmt_new_tape()                            # takes the inserted tape out of the deck (its file
                                              # is untouched) and inserts an empty one
emu_cmt_record()                              # -> {"path": null}, no "warning"; state "record", paused true
bp = emu_bp_add(addr=0x0475)                  # the ROM's body writer
# start the SAVE; here the MZ-800 monitor (boot menu key M): S, name, start, end, exec address
k = emu_input_send_keys(text="S\nNAME\n4000\n4C9F\n4340\n")
w = run_and_wait(k, 30000)                    # the header is written meanwhile
ok = w["done"] and w["record"]["reason"] == "breakpoint" and w["record"]["pc"] == 0x0475
if ok:
    mm = emulator://memory/map                # the banking at the stop
    ok = mm["slots"][0]["kind"] == "rom_low"  # RAM there: the program's own code, not the ROM
if ok:
    regs = emu_get_registers()
    m = emu_mem_read(addr=regs["SP"], length=2)   # m["data_b64"]: return address, little endian
    ret = int.from_bytes(base64.b64decode(m["data_b64"]), "little")
emu_bp_remove(id=bp["id"])                    # on every path
if ok:
    u = emu_run_until_addr(addr=ret, deadline_frames=30000)
    for i in range(30):
        w = emu_wait_stop(run_seq=u["run_seq"], timeout_ms=25000)
        if w["done"]: break                   # w["record"]["reason"] "run_to": the body is written
# not ok: the program did not reach 0475h in the ROM ("deadline", or a stop in its own
# code) - let it run on and wait for its own end mark instead
r = emu_cmt_stop()                            # r["recorded"] = {"blocks", "mzf", "raw", "message"}
# r["recorded"]["mzf"] == 0: no clean ROM file was recorded - nothing to export
t = emulator://periph/cmt/tape
n = first b in t["blocks"] with b["unsaved"] and b["type"] == 1   # -> b["block_id"]
emu_cmt_export_block(block_id=n, path="saved.mzf")
```

The breakpoint at 0475h is normally not reached while the keys are
typed (the header comes first); `run_and_wait` handles it if it is. The
MZ-700 monitor stores the name with a leading space (`" NAME"`): choose
the block by its fields, as above, not by its name.

Why not `current_block`: after the stop the deck stands at the first new
block that can be opened, which can be a raw block; when nothing was
appended (fewer than 64 pulses) it stays at the block it stood at, and an
export of that block would write an older program without an error. A
new empty tape and the choice by `unsaved` and `type` avoid both.

`recorded.raw` > 0 means part of the signal was not a clean ROM file:
such a block stays raw (`not_decoded`); try `emu_cmt_tape_analyze` on it,
or keep it with `emu_cmt_save(path="raw.tmz")`. If the recording ended by
itself, `emu_cmt_stop` has no `recorded`: read `last_recording` and
compare its `serial` with the value from before.

### Turn a WAV into file blocks

```python
emu_cmt_stop()
emu_cmt_open(path="recording.wav")            # one block 0: type 0, status "not_decoded"
r = emu_cmt_tape_analyze(block_id=0)          # r["rows"]: kind, name, result, reason per row
repair = any(x["kind"] == "unknown" for x in r["rows"])
# an "unknown" row can be a file whose signal ends inside its body: look again with repair_body
if repair: r = emu_cmt_tape_analyze(block_id=0, repair_body=True)
# apply with the same options as the analysis you accept
r = emu_cmt_tape_analyze(block_id=0, repair_body=repair, apply="replace")
# r["applied"] file blocks from r["first_added"]; r["count"] blocks in total
c = emu_cmt_tape_check()                      # c["findings"]: problems of the tape, if any
emu_cmt_save(path="recording.tmz")            # modified -> false
```

A row with `result` `addable_repaired` has 00h in place of the bytes the
recording lacks; look at `recovery` before you trust the file. For a long
WAV the open and the analysis can take minutes; the emulation waits. The
bundled wrapper waits 30 s for `emu_cmt_open` (640 s for the analysis):
after a timeout read `emulator://periph/cmt/tape` before you repeat
anything (see Limits and costs). The rows that stay raw remain on the
tape and still play as recorded.

### Edit a tape and save it

```python
emu_cmt_stop()
t = emulator://periph/cmt/tape                # t["modified"] true: save the user's tape first
emu_cmt_new_tape()                            # takes the user's tape out of the deck (file untouched)
a = emu_cmt_tape_import(path="loader.mzf")    # a["at"] 0, a["imported"] 1, a["new_tape"] false
g = emu_cmt_tape_import(path="game.mzt")      # blocks g["at"] .. g["at"] + g["imported"] - 1
# reorder / drop by numbers taken from the results or from the block list
emu_cmt_tape_move_block(block_id=g["at"] + 1, to=g["at"])
t = emulator://periph/cmt/tape                # numbers after each edit
emu_cmt_tape_delete_blocks(block_ids=[b["block_id"] for b in t["blocks"] if b["name"] == "UNUSED"])
emu_cmt_save(path="assembled.tmz")            # modified and unsaved -> false
emu_cmt_export_tape(path="assembled.wav")     # e.g. for a real recorder
```

Before: no input replay running, `state` `"stop"`. After each edit:
`count`, `current_block` in the result, and the block list again (numbers
shift). An error leaves the tape unchanged. Importing a long WAV and
exporting a long tape can take longer than the 30 s the bundled wrapper
waits: after a timeout read the tape listing (see Limits and costs).

### Write the tape information

```python
emu_cmt_stop()
i = emu_cmt_tape_info()                       # current texts, machine, storable
r = emu_cmt_tape_info(set={"title": "My tape", "author": "Me", "machine": "mz800"})
# r["changed"] true; r["warning"] when characters were replaced
emu_cmt_save(path="my.tmz")                   # i["storable"] false: save as .tmz or .tzx
```

Before: a tape in the deck (`No tape inserted` otherwise). After:
Archive Info is block 0 and Machine Info block 1 when they were added,
so other numbers moved; `modified` true.

## When to use what

- Start a program from an MZF file -> `emu_media_run_mzf`.
- Run a program through its real loader -> Direct loading off,
  `emu_cmt_open`, `emu_cmt_play_paused`, the load command (with
  `emu_cmt_set_cpu_boost(enabled=True)` the load runs at maximum speed).
- Several programs on one tape, pick one -> read
  `emulator://periph/cmt/tape`; real signal: `emu_cmt_tape_seek(block_id)`
  then play; fast: `emu_cmt_load_directly(block_id)`.
- Next / previous / first program like the deck buttons ->
  `emu_cmt_tape_step(direction="next" | "prev" | "start")`.
- Capture what a program saves -> Direct loading off, a new tape,
  `emu_cmt_record()`, the program's SAVE until it returns from the body
  writer (not just until the motor stops), `emu_cmt_stop()`, then
  `emu_cmt_export_block` or `emu_cmt_save`.
- Build or fix a tape -> `emu_cmt_tape_import`, `emu_cmt_tape_move_block`,
  `emu_cmt_tape_delete_blocks`, then `emu_cmt_save`.
- Whole tape as audio -> `emu_cmt_export_tape`; one block ->
  `emu_cmt_export_block(block_id, path="x.wav")`.
- WAV or undecoded recording into files -> `emu_cmt_tape_analyze`, then
  with `apply="replace"`.
- Name the tape, set the target machine -> `emu_cmt_tape_info(set={...})`.
- A tape does not load or plays oddly -> `emu_cmt_tape_check()`.
- A block should load with the original ROM timing or a turbo loader ->
  `emu_cmt_tape_convert_block(block_id, profile)` with an `id` from
  `profiles`.

## See also

- `emulator://periph/cmt`, `emulator://periph/cmt/tape` - live state.
- `emulator://docs/replay` - input recording and playback, tape in a
  replay.
- `emulator://docs/state_revision` - `state_rev`, `expected_state_rev`,
  the `cmt` part of `emu_machine_hash`.
- `emulator://docs/error_handling` - `Emulator busy:` and retries.
- `emulator://docs/mz800_keyboard` - key names for typing a load
  command.
