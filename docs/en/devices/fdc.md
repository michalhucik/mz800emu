# FDC (Floppy Disk Controller) - user documentation

The FDC subsystem emulates a floppy disk drive controller based on the
WD279x chip (Sharp uses the compatible MB8876A). The controller is
attached to ports 0xD8-0xDF and services up to 4 floppy drives
(FDD0..FDD3). It works with disk images in DSK format (standard CPC
DSK and Extended CPC DSK).

Sharp HW inverts the data bus between the CPU and the chip - the BUS
translation layer in the emulator handles this inversion, so the
chip's internal logic operates on "true-bus" values per the datasheet.

## Controller connection

From the menu HW & Devices / FD Controller, the controller can be logically
disconnected or connected:

| Option        | Meaning                                                      |
|---------------|--------------------------------------------------------------|
| Not Connected | FDC behaves as not installed - ports 0xD8-0xDF return the    |
|               | "open bus" state and the drive UI is grayed out.             |
| WD279x        | Standard emulated controller. Default.                       |

## Drive slots (FDD0..FDD3)

Unlike QuickDisk, which is physically a single drive, FDC can address
four independent drives (FDD0..FDD3). Each drive has its own mount
state, its own DSK image, its own R/O setting and its own storage
mode.

In the menu HW & Devices / FD Controller / "FDD N" the following items are
available for each drive:

- **Mount...** - opens the file chooser for selecting a DSK image
  (keyboard shortcut Alt+N, where N is the drive number)
- **Re-Mount...** - available when something is already mounted
- **Recent** - a submenu with at most 12 most recently inserted DSK
  images (file name, full path in the tooltip); a click inserts the image
  into this drive in the same way as a choice in **Mount...**. A missing
  file is reported only by the insertion. **Clear recent media** at the
  end deletes the history of all kinds of media (see overlay-menu.md,
  "The Recent media history")
- **Umount** - disconnects the image and clears the persistent record
  in INI (Alt+Shift+N)
- **Read-only** - per-drive R/O toggle (see "Read-only 3-state model");
  disabled while the same image is inserted in another drive that may
  write to it (see "The same image in more than one drive")
- **Storage mode** - per-drive choice (Cached / Direct / Discard)
- **Sync now** - explicit flush of the RAM buffer to the file (active
  only in Cached mode with pending changes)

In the menu, the drive name is shown as "FDD 0 (Empty)" / "FDD 0";
when an image is mounted, the expanded submenu shows the basename of
the file (truncated to 20 chars + "..." if longer).

## Storage mode (per drive)

The choice is persisted per drive in INI key
`wd279x_fddN_storage_mode`. Default is `cached`.

| Mode    | What happens                                                              |
|---------|---------------------------------------------------------------------------|
| CACHED  | The entire DSK image is loaded into RAM. Changes are kept in the RAM     |
|         | buffer; they are written to the file only on HW reset, umount, emu exit  |
|         | or manual "Sync now". Default. Low I/O overhead, risk of losing changes  |
|         | on crash.                                                                 |
| DIRECT  | No RAM buffer; every write goes directly to the file through the file   |
|         | driver. Higher I/O overhead, but writes are persistent immediately.       |
| DISCARD | The DSK image is loaded into RAM; all writes end up ONLY in the RAM     |
|         | buffer. On umount / exit / reset NOTHING is written back. For test runs |
|         | without modifying the source file.                                       |

### Switching storage mode at runtime

From the Storage mode menu (radio Cached / Direct / Discard) the mode
can be switched at any time. Switching for a mounted drive performs a
remount with the new storage_mode choice.

If you switch from DISCARD to CACHED or DIRECT and there are pending
changes in the RAM buffer (= writes were performed that would otherwise
be discarded), the modal popup "FDC: Unsaved RAM changes" opens with
three options:

1. **Save and switch** - forcibly write the RAM buffer to the file and
   only then switch mode. Nothing is written to the file when another
   drive holds it with write access (see "The same image in more than one
   drive") or when the drive refers to a different file than the one it
   loaded the image from (see "Snapshot compatibility"); the emulator
   reports it with a message and switches the mode without saving
2. **Switch and discard** - discard RAM changes and switch
3. **Cancel** - cancel the switch

When switching between CACHED and DIRECT without uncommitted RAM
changes, or to DISCARD, the popup is not shown - the switch happens
immediately.

## Disks from the command line

A disk can be inserted already when the emulator starts:

```
mz800emu --fdd 0=boot.dsk --fdd 1=data.dsk
mz800emu --fdd0 boot.dsk --fdd-readonly 0
```

The drives are numbered across both controllers: 0-3 = FDD0-FDD3 of the
FDC controller (ports 0xD8 - 0xDF), 4-7 = FDD0-FDD3 of the secondary
FDC1 controller (ports 0x58 - 0x5F). `--fdd <N>=<file>` can be repeated,
`--fdd0` to `--fdd7` are short forms of the same. The disk is inserted
the same way as from the menu: it replaces the disk from the INI in the
same drive and its path is saved to the INI at exit (`--no-save-ini`
turns saving off). The storage mode of the drive comes from the INI.

`--fdd-readonly <N>` write-protects drive N for this run only (like
user_readonly, see "Read-only 3-state model", but without writing to the
INI); it also applies to disks inserted into the drive later from the
menu.

A missing file, a drive number outside 0-7 or a drive given twice ends
the emulator with an error before it starts. A disk for a disconnected
controller (FDC1 is disconnected by default) or an image that cannot be
opened is an error: in `--headless` the emulator exits with code 3, in
the window the emulation stays paused and the error goes to the error
output (stderr).

## Read-only 3-state model

Effective R/O = `user_readonly || fs_readonly`, or the image is shared
with another drive (see "The same image in more than one drive").

| Field         | Meaning                                                                |
|---------------|------------------------------------------------------------------------|
| user_readonly | Persistent user choice. INI key `wd279x_fddN_readonly`. Mirrored in UI |
|               | as the "Read-only" checkbox in the drive menu.                         |
| fs_readonly   | Runtime auto-detection: determined by a test write access (W_OK) on   |
|               | mount. If write access to the file cannot be obtained, it is 1.        |
| readonly      | Effective value. When it is 1, the chip refuses WRITE SECTOR / WRITE  |
|               | TRACK (host sees write protect status).                                |

`fs_readonly` is recomputed on every mount / remount. `user_readonly`
is changed only by user action via the UI checkbox or by INI on
startup. If the image is FS write-protected, the menu shows a disabled
item "Read-only (FS write-protected)" instead of the standard checkbox,
fixed ON - the persistent user pref is not changed through this path.

On umount or close of the handler, all three flags are cleared (mount
cycle from a clean state).

## The same image in more than one drive

The same DSK file can be inserted into more than one drive (also across
the two controllers). To keep the image file safe, **only one drive may
write to one image file at any moment**. Drives are numbered across both
controllers as on the command line: FDD 0-3 belong to FDC0, FDD 4-7 to
FDC1.

- The drive that holds the file first keeps writing. Every other drive
  gets the image **write-protected**: the emulated computer sees a
  write-protected disk and nothing from that drive is ever written to the
  file.
- On insertion the emulator shows the message "The image is already
  inserted in FDD n. It was inserted write-protected here to keep the
  image file safe." Without a window (`--headless`, disks restored from
  the INI file or given on the command line at start, MCP, Unicard) the
  message goes only to the error output (stderr); MCP `emu_media_insert`
  also returns it in the field `warning`.
- The drive menu shows "Shared with FDD n, write-protected" under the
  file name and the item **Read-only** is checked; the overlay menu shows
  the same note in the drive row and **Write protect** is on.
- While another drive may write to the file, the protection cannot be
  turned off: **Read-only** and **Write protect** are disabled and give
  the reason "The image is also inserted in FDD n, which may write to
  it."
- When the first drive ejects the image, or gets **Read-only** itself,
  the protection of the other drive can be turned off. It never turns off
  by itself. To move writing to another drive, turn **Read-only** on in
  the first drive and then off in the second one; the first drive is then
  locked by the same rule.
- The protection belongs to the inserted image and is not saved to the
  INI file (the key `wd279x_fddN_readonly` does not change). Inserting a
  different image into the drive clears it. At start the disks from the
  INI file are inserted in the order FDD 0 to 7, so the first drive in
  this order writes and the others are protected, even if another drive
  was the writing one before the emulator was closed.

The same file is recognised by the identity of the file, not by the text
of the path: the absolute path without `.` and `..` (on Windows without
regard to upper and lower case and to the kind of slash) and, where the
file system provides it, the file number, which covers hard links,
symbolic links and 8.3 short names. Not recognised: the same file reached
through two network paths for which the system gives different file
numbers or none.

A protected drive in Cached or Discard mode works with the copy of the
image read when it was inserted: changes saved later by the writing
drive show up in it only after the image is inserted again.

## Side / Density / Motor (HW logic ports)

In addition to the CMDSTS / TRACK / SECTOR / DATA registers (ports
0xD8-0xDB, behind the inverter), the WD279x chip has external Sharp
logic ports 0xDC-0xDF (in front of the inverter):

| Port | Function                                                               |
|------|------------------------------------------------------------------------|
| 0xDC | MOTOR / drive select - lower 2 bits = ID of the active drive (0..3),  |
|      | bit 2 = EDR, bit 7 = motor on/off                                     |
| 0xDD | SIDE - head side selection (0 / 1)                                    |
| 0xDE | DENSITY - recording density selection (single / double)               |
| 0xDF | EINT - HD Patch interrupt enable                                      |

Side and density are controlled directly by the emulated SW (Sharp DOS
/ CP/M boot loaders, FDC ROM); they are not user-configurable options.
Current values can be inspected in the FDC State debugger window.

## FDC chip HW options

In the menu HW & Devices / FD Controller there is a "Hardware options"
section with these toggles. A change applies immediately, the
controller reads both options while running; no restart is needed.

| Option                      | Meaning                                                 |
|-----------------------------|---------------------------------------------------------|
| HD Patch enabled            | HD Patch circuit (port 0xDF EINT logic). INI key        |
|                             | `hd_patch`. Default ON - typical state of a real MZ-800 |
|                             | setup with the HD Patch circuit installed. Turn off for |
|                             | raw / unpatched FDC.                                    |
| Bus translation: invert     | Sharp default - XOR 0xFF between CPU bus and chip.      |
|                             | INI value `bus_xlate=invert`.                           |
| Bus translation: passthrough| Experimental - without inversion. NOT compatible with   |
|                             | Sharp system SW. Reserved for future "true bus" ROMs /  |
|                             | experiments. INI value `bus_xlate=passthrough`.         |

Both HW options can be overridden from the command line via
`--fdc-hd-patch=on|off` and `--fdc-bus-xlate=invert|passthrough`. The
CLI override is persisted to INI on exit.

## FDC State debugger window

From the menu HW & Devices / FD Controller / "FDC State (debugger)..." a
window opens with the complete FDC subsystem state (available only in
debug build of the emulator). The window refreshes automatically on
each ImGui render.

Window sections:

- **Connection**: controller state (yes / no)
- **Registers (true-bus)**: STATUS, COMMAND, TRACK, SECTOR, DATA,
  MOTOR, SIDE, DENSITY, EINT - each in hex + binary bit representation
- **Transfer state**: data_counter (bytes remaining for R/W),
  buffer_pos, current_sector_size, status_mode (Type I vs II/III),
  multiblock_rw, reading_status_counter, waitForInt (HD Patch INT
  throttle), positioned_track / positioned_side / positioned_sector
  (drive head position)
- **Sticky flags**: intrq_active, pending_busy_status, pending_drq,
  direction_latch (+1 = step in, -1 = step out)
- **Type III state (WRITE/READ TRACK)**: write_track_stage,
  write_track_counter, rt_sectors, rt_sector_bytes, rt_ssize_code,
  rt_cached_sec_idx
- **Drives**: per drive (FDD 0..3) - mount status, R/O, filename,
  storage mode, user_ro / fs_ro, geometry (tracks, sides, total,
  image size in bytes)

## Snapshot compatibility

The FDC subsystem snapshot saves:

- chip continuity: registers (STATUS, COMMAND, TRACK, SECTOR, DATA,
  MOTOR, SIDE, DENSITY, EINT), buffer_pos, data_counter, sticky flags,
  state machine for WRITE TRACK / READ TRACK
- informational info per drive: `filename`, `mounted` flag, effective
  `readonly` (on load they only serve the comparison with the inserted
  disks, see below)
- informational values of the config switches `hd_patch` and
  `bus_xlate`

**What is NOT saved:**

- DSK image content from the RAM buffer (= analogous to QD: dirty RAM
  changes in CACHED or DISCARD are **lost** on snapshot save+load.
  If you have CACHED with pending changes, we recommend a manual
  "Sync now" before snapshot save)
- User INI settings (`storage_mode`, `user_readonly`, mount paths) -
  INI is the SSOT for user pref; the snapshot is the SSOT for chip
  continuity

**After snapshot load:** an inserted disk belongs to the emulator, not
to the state of the emulated computer. Loading a snapshot does not
insert, exchange or eject disks: every drive keeps the image it held
before the load, with its file name, write protection, storage mode and
unsaved changes; an empty drive stays empty. Only the controller state
is restored from the snapshot (registers, head position, motor, the
command in progress). Unsaved changes are still saved to the file the
image was inserted from.

If different disks were inserted when the snapshot was saved (another
file, an empty drive instead of a loaded one or the other way round),
the emulator reports it after the load with one message that lists only
the drives that differ:

```
The snapshot was saved with different disks:
FDD 0: game.dsk (now: work.dsk)
FDD 1: data.dsk (now: empty)
The inserted disks were kept.
```

The message is shared with the Quick Disk: when it differs as well, a
line "Quick Disk: <name in the snapshot> (now: <name now>)" is added (see
qdisk.md). The names are shown without paths (the full path is shown only
when the files differ in the directory alone). The emulated program then
continues over different content than it expects; to continue exactly
where the snapshot was taken, insert the disks named in the message and
load the snapshot again. A repeated load with the same difference (for
example Quick Load several times in a row) does not show the message
again; in a run without a window the message goes to the standard error
output.

**A disk is never saved to a file other than the one it was loaded
from.** The controller has this safeguard in addition: should the file
name of a drive refer to a different file than the one the drive loaded
its image from, the emulator does not write the in-memory copy to the
file (Sync now, reset, eject, exit, "Save and switch") and reports "The
disk in FDD n was not saved: the drive now refers to a different image
file than the one it was loaded from. Re-insert the image to continue."
The unsaved changes then remain in memory only and are discarded on
eject, on an image exchange or when the emulator exits. This does not
happen after an ordinary snapshot load. It can happen after playing an
older input replay that carries no media list, and after going back in
time while an input replay is being recorded: there the emulator gives
the drive the file name from the snapshot so that the playback matches
the recording.

After a snapshot load the write protection follows the disk that is
really in the drive: the **Read-only** option, the protection of the
file on the host disk and the sharing of the image with another drive.
The `readonly` flag stored in the snapshot is not used for a drive with
an inserted file, so a snapshot load does not take the write access
from a drive with unsaved changes and does not remove the protection of
a shared image: after the load, still only one drive may write to one
image file.

## DSK format and broken header repair

FDC supports DSK images (standard CPC DSK and Extended CPC DSK). Image
geometry (track count, sides, total size) is read at mount; if reading
fails, the mount is not performed.

Some HD DSK images distributed in the Sharp MZ-800 community have a
broken `tsize` array in the Extended CPC DSK header (the declared
track size does not match the actual size). The emulator performs an
automatic header repair in memory at mount time in CACHED / DISCARD
mode (the repair is located by the `Track-Info` magic string at the
start of each track). The repairs are written back to the file only in
CACHED mode at sync time.

In DIRECT mode the header is not repaired (a repair would require
reading/walking the entire image through the file driver). If you use
DIRECT mode, DSK images should have a correct header.

## Known limitations

- **Header repair only in RAM modes** - in DIRECT mode, DSK headers
  with a broken tsize array are not repaired (see above).
- **Maximum sector size 512 B** - the chip's internal I/O buffer is
  512 B (= HD sector size). This is sufficient for SD (256 B) and HD
  (512 B) sectors of standard MZ-800 formats. 1024 B sectors are not
  common for MZ-800 and would not fit in this buffer.
- **Maximum mount filename 1023 chars** - longer paths are not allowed
  (UI shows "Sorry, filepath is too big").
- **Timing model is not complete** - step rate, motor spin-up, head
  settling are not modeled in the current version.
- **Snapshot does not save DSK content** - see "Snapshot compatibility".

## INI keys reference

All keys are in section `[FDC]`. Keys with `N` in the name exist in
the set N = 0..3 (per drive).

| Key                             | Type | Default  | Meaning                                 |
|---------------------------------|------|----------|-----------------------------------------|
| `hd_patch`                      | BOOL | 1        | HD Patch circuit (port 0xDF EINT logic) |
| `bus_xlate`                     | TEXT | "invert" | "invert" / "passthrough"                |
| `wd279x_fddN_dskpath`           | TEXT | ""       | Path to the DSK image for drive N       |
| `wd279x_fddN_readonly`          | BOOL | 0        | user_readonly for drive N               |
| `wd279x_fddN_storage_mode`      | TEXT | "cached" | "cached" / "direct" / "discard"         |
