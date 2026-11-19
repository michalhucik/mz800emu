# QuickDisk (QD) - user documentation

QuickDisk is the Sharp MZ-1F11 serial disk subsystem connected via SIO
ports 0xF4-0xF7. The mz800new emulator supports three modes:

| Mode     | Description                                                                   |
|----------|-------------------------------------------------------------------------------|
| IMAGE    | classic .MZQ image of a QD disk (typically 64 kB usable, max ~92 kB)          |
| VIRTUAL  | a directory with .MZF files, dynamically presented as QD content              |
| UNICARD  | image managed by UNICARD FW (read-only from the emulator's perspective)       |

IMAGE and UNICARD run through the abstraction over the memory driver
and file driver - the same mechanism as FDC. VIRTUAL mode has a
separate I/O path.

A medium is inserted in the menu HW & Devices > Quick Disk > QD Drive:
**Mount Image...** (IMAGE mode) or **Mount Directory...** (VIRTUAL mode)
and the **Recent** submenu with the most recently inserted images or
directories (at most 12, full path in the tooltip). Both are available
only with an empty drive. Recent offers only the items the drive accepts
in its current mode (an image file, or a directory); a file that no longer
exists is not inserted, it is removed from the history and the emulator
reports it. Right after the submenu opens, a click may end with the
message "Still checking whether the file exists. Try again in a moment"
until the emulator finds out in the background whether the files exist.

## Storage mode (IMAGE / UNICARD)

The choice is persisted in the INI key `mz1f11_storage_mode`.

| Mode    | What happens                                                              |
|---------|---------------------------------------------------------------------------|
| CACHED  | The entire .MZQ image is loaded into RAM (memory driver). Changes are    |
|         | kept in the RAM buffer; they are written to the file only on motor-off, |
|         | umount, emu exit or manual "Save now". Default. Low I/O overhead, risk  |
|         | of loss on crash.                                                         |
| DIRECT  | No RAM buffer; every byte goes directly to the file (file driver).      |
|         | Higher I/O overhead, but writes are persistent immediately.               |
| DISCARD | The image is loaded into RAM (memory driver); all writes end up ONLY   |
|         | in the RAM buffer. On motor-off / close / exit NOTHING is written back. |
|         | For test runs without modifying the source file. The "Save now" button  |
|         | can do a one-off force-save even in this mode.                            |

### Switching storage mode at runtime

From the UI menu "Storage mode" the mode can be switched at any time.
If unsaved changes exist (RAM dirty buffer in CACHED/DISCARD) and you
switch to another mode, a popup opens with three options:

1. **Save and switch** - flush the RAM buffer to the file and only then
   switch
2. **Switch and discard** - discard RAM changes and switch
3. **Cancel** - cancel the switch

## Read-only 3-state model

Effective R/O = `user_readonly || fs_readonly`.

| Field         | Meaning                                                                |
|---------------|------------------------------------------------------------------------|
| user_readonly | Persistent user choice. INI key `mz1f11_write_protected`. Mirrored in  |
|               | UI as the "Write Protected" checkbox.                                  |
| fs_readonly   | Runtime auto-detection: if write access to the file cannot be obtained,|
|               | it is 1. In the UI signaled by the label "[FS R/O]" below the checkbox.|
| readonly      | Effective value. When it is 1, a write through SIO fails (host sees a |
|               | write protect error).                                                  |

`fs_readonly` is recomputed on every image open. `user_readonly` is
changed only by user action via the UI checkbox or by INI on startup.
On image close, all three are cleared (mount cycle from a clean state).

## UNICARD lock

UNICARD mode (boot loader image managed by Unicard FW) has special
handling:

- `user_readonly` is **forced** to 1 on mount (the boot image is not
  written to from the emulated system)
- `storage_mode` is **forced** to CACHED on mount
- The INI values `mz1f11_write_protected` and `mz1f11_storage_mode` are
  **not overwritten** (the user pref for IMAGE mode stays intact)
- The Write Protected + Storage mode UI controls are grayed out in
  UNICARD mode (visual feedback that the values cannot be changed)

## QDisk State debugger window

From the menu DBG / "QDisk State" a window can be opened with the
complete QD subsystem state:

- **Connection**: type (IMAGE / VIRTUAL / UNICARD), connected flag
- **Status flags**: IMG_READY, HEAD_HOME, IMG_SYNC, IMG_READONLY
- **Channel A/B SIO**: current registers WR0-WR7, RR0-RR2
- **Image**: filename, image_position
- **Handler info** (IMAGE/UNICARD only): driver type (memory/file),
  R/O status, dirty flag
- **Storage mode + R/O 3-state**: user_readonly, fs_readonly, readonly
- **Has unsaved changes**: live indicator (dirty / clean)
- **Virtual scan** (VIRTUAL only): file count, current file_num, open
  MZF info

The window refreshes automatically on every ImGui render. Open/close
via menu DBG / "QDisk State" (toggle).

## Snapshot compatibility

The QD subsystem snapshot saves:

- chip continuity: SIO channel registers, image_position, status
- informational info: `filename`, effective `readonly`

**What is NOT saved:**
- Memory buffer content of the .MZQ image (a snapshot does not carry the
  disk content; unsaved changes stay in the drive after a snapshot load,
  see below)
- User INI settings (`storage_mode`, `user_readonly`) - INI is the
  SSOT for user pref; the snapshot is the SSOT for chip continuity
- The state of the virtual Quick Disk (which MZF file is being read); a
  snapshot saved in the middle of a read or write in virtual mode may
  therefore not continue correctly after it is loaded

**After snapshot load:** the inserted Quick Disk belongs to the emulator,
not to the state of the emulated computer. A snapshot load does not
insert, exchange or eject the disk: the drive keeps the image that was in
it before the load, with its file name, write protection, storage mode
and unsaved changes; an empty drive stays empty. The state of the SIO
chip and of the drive (head position, synchronisation) is restored from
the snapshot. Whether the emulated computer sees a disk in the drive and
whether it is write-protected follows what is really inserted, not the
snapshot. Unsaved changes are still saved to the file the image was
inserted from.

If a different Quick Disk was inserted when the snapshot was saved
(another file, an empty drive instead of a loaded one or the other way
round), the emulator reports it after the load with a line in the message
shared with the floppy disks (see fdc.md):

```
The snapshot was saved with different disks:
Quick Disk: game.mzq (now: work.mzq)
The inserted disks were kept.
```

The emulated program then continues over different content than it
expects; the head position stays as in the snapshot, so a program that
was in the middle of a read when the snapshot was saved reads the other
disk from the same place. To continue exactly where the snapshot was
taken, insert the disk named in the message and load the snapshot again.
A repeated load with the same difference does not show the message
again. The directory of a virtual Quick Disk is not stored in the
snapshot and is therefore not compared.

The drive type (image, virtual directory, Unicard) and its connection are
part of the computer state and are taken from the snapshot for the run
only; the INI keeps the user's choice. When the snapshot switches the
drive between an image (or Unicard) and a virtual directory, the inserted
medium does not belong to the new kind of drive: the emulator closes it
as on eject (unsaved changes are saved to its file) and inserts the
medium the INI holds for the new kind of drive, as on startup. Between
the image and Unicard types the inserted image stays in place.

**An image is never saved to a file other than the one it was loaded
from.** The drive has this safeguard in addition: should the file name of
the drive refer to a different file than the one the drive loaded its
image from, the emulator does not write the in-memory copy to the file
(Save now, motor off, eject, exit, "Save and switch") and reports "The
Quick Disk was not saved: the drive now refers to a different image file
than the one it was loaded from. Re-insert the image to continue." The
unsaved changes then remain in memory only and are discarded on eject, on
an image exchange or when the emulator exits. This does not happen after
an ordinary snapshot load. It can happen after playing an older input
replay that carries no media list, and after going back in time while an
input replay is being recorded: there the emulator gives the drive the
file name from the snapshot so that the playback matches the recording.

## Known limitations

- **VIRTUAL has no storage_mode** - its I/O path is always directly to
  the file per .MZF (no RAM cache). The Storage mode UI control is
  irrelevant in VIRTUAL mode.
- **No per-drive structure** - QD is physically a single drive; a
  second disk cannot be mounted (unlike FDC, which has two drive slots
  A/B).
- **Memory buffer of .MZQ is not persisted in the snapshot** - see
  above.
- **The state of the virtual Quick Disk is not stored in the snapshot** -
  see above.

## INI keys reference

| Key                           | Type    | Default  | Meaning                             |
|-------------------------------|---------|----------|-------------------------------------|
| `mz1f11_connected`            | BOOL    | 0        | QD subsystem enabled                |
| `mz1f11_type`                 | INT     | 0        | 0=IMAGE, 1=VIRTUAL, 2=UNICARD       |
| `mz1f11_std_filepath`         | TEXT    | ""       | Path to the .MZQ image              |
| `mz1f11_virt_dirpath`         | TEXT    | ""       | Path to the directory with .MZF files |
| `mz1f11_write_protected`      | BOOL    | 0        | user_readonly                       |
| `mz1f11_storage_mode`         | TEXT    | "cached" | "cached" / "direct" / "discard"     |
