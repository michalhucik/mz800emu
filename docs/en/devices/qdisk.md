# QuickDisk (QD) - user documentation

QuickDisk is the Sharp MZ-1F11 serial disk subsystem connected via SIO
ports 0xF4-0xF7. The mz800new emulator supports three modes:

| Mode     | Description                                                                   |
|----------|-------------------------------------------------------------------------------|
| IMAGE    | logical `.MZQ` or physical `.qd` QuickDisk image                              |
| VIRTUAL  | a directory with .MZF files, dynamically presented as QD content              |
| UNICARD  | image managed by UNICARD FW (read-only from the emulator's perspective)       |

IMAGE and UNICARD run through the abstraction over the memory driver
and file driver - the same mechanism as FDC. VIRTUAL mode has a
separate I/O path.

IMAGE automatically detects legacy Sharp logical `.qd`, HxC `HXCQDDRV`,
and FlashFloppy physical `.qd` images. A physical image is decoded into an
internal logical working buffer when mounted. Saving rebuilds the original
container type; FlashFloppy output uses the canonical QDF/qdf2qd MFM layout
starting at offset `0x3A90`, so it remains usable on a real MZ-800. The
**Create New Image** dialog can create `.mzq` and all three `.qd` variants.
A new `.qd` entered as a previously non-existent file in the mount dialog is
FlashFloppy physical by default; legacy and HxC require an explicit format choice.

## Storage mode (IMAGE / UNICARD)

The choice is persisted in the INI key `mz1f11_storage_mode`.

| Mode    | What happens                                                              |
|---------|---------------------------------------------------------------------------|
| CACHED  | The entire image is loaded into RAM (memory driver). Changes are         |
|         | kept in the RAM buffer; they are written to the file only on motor-off, |
|         | umount, emu exit or manual "Save now". Default. Low I/O overhead, risk  |
|         | of loss on crash.                                                         |
| DIRECT  | No RAM buffer; every byte goes directly to the file (file driver).      |
|         | Higher I/O overhead, but writes are persistent immediately.               |
| DISCARD | The image is loaded into RAM (memory driver); all writes end up ONLY   |
|         | in the RAM buffer. On motor-off / close / exit NOTHING is written back. |
|         | For test runs without modifying the source file. The "Save now" button  |
|         | can do a one-off force-save even in this mode.                            |

Physical `.qd` images always use an internal RAM buffer because individual
logical SIO bytes cannot be written directly into an MFM container. DIRECT
mode is therefore disabled in the UI for a mounted `.qd`; CACHED rewrites a
complete validated container at synchronization points, while DISCARD leaves
the source file unchanged.

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
- Memory buffer content of the `.MZQ`/`.qd` image (= analogous to FDC: dirty
  RAM changes are **lost** on snapshot save+load; we recommend a manual
  "Save now" before snapshot save if you have CACHED + dirty)
- User INI settings (`storage_mode`, `user_readonly`) - INI is the
  SSOT for user pref; the snapshot is the SSOT for chip continuity

**After snapshot load:** the mount is performed from the current INI
settings (as on emu startup); the snapshot then writes the SIO chip
state to the mounted handler.

## Known limitations

- **VIRTUAL has no storage_mode** - its I/O path is always directly to
  the file per .MZF (no RAM cache). The Storage mode UI control is
  irrelevant in VIRTUAL mode.
- **No per-drive structure** - QD is physically a single drive; a
  second disk cannot be mounted (unlike FDC, which has two drive slots
  A/B).
- **Memory buffer of `.MZQ`/`.qd` is not persisted in the snapshot** - see
  above.

## INI keys reference

| Key                           | Type    | Default  | Meaning                             |
|-------------------------------|---------|----------|-------------------------------------|
| `mz1f11_connected`            | BOOL    | 0        | QD subsystem enabled                |
| `mz1f11_type`                 | INT     | 0        | 0=IMAGE, 1=VIRTUAL, 2=UNICARD       |
| `mz1f11_std_filepath`         | TEXT    | ""       | Path to an `.MZQ` or `.qd` image    |
| `mz1f11_virt_dirpath`         | TEXT    | ""       | Path to the directory with .MZF files |
| `mz1f11_write_protected`      | BOOL    | 0        | user_readonly                       |
| `mz1f11_storage_mode`         | TEXT    | "cached" | "cached" / "direct" / "discard"     |
