# Memory Heatmap - user manual

Memory Heatmap (internal module name: **CDL** = Code/Data Logger) is a
debugger tool that during emulation aggregates access statistics for every
memory cell (and every I/O port) and visualizes them as a grid with a
color-coded scale. It serves to understand where a program reads data,
writes variables, executes code, and optionally to separate code from data
(classic CDL for disassembler workflow).

This window is standalone, dockable, and may be open simultaneously with
the running emulator and with the main debugger.

> Architectures: MZ-800 and MZ-1500. For MZ-700 the emulator always runs
> MZ-800 in MZ-700 mode, so the MZ-800 version is used there as well.

## Opening the window

- **Main emulator menu** -> `Debugger` -> `Memory Heatmap`
- **Debug window menu** -> `Debugger Settings` -> `CDL` -> `Show heatmap window`

Both paths open the same window. Visibility state is per-session (default
closed); position/size/dock state is remembered automatically by ImGui via
`imgui.ini`.

## Three recording modes (Mode)

CDL recording is expensive (per-access counter increment) and leaving it
on in the emulator hot path would make no sense. The three modes:

- **Off** (default) - no recording at all, no overhead. The hot path is
  bit-identical with the vanilla emulator (callback swap).
- **With Window** - recording happens only while the main debug window is
  open. Useful for short analysis runs.
- **Always** - recording runs continuously regardless of the debug window
  state. Useful for automated test runs with `--cdl-save-on-exit`.

Changing the mode triggers a CPU callback swap. The current state is shown
in the Status line below the top bar ("Recording active" green / "Recording
inactive" grey).

## Three access types (R / W / X)

Each cell holds a triple of 32-bit counters:

| Symbol | Meaning |
|--------|---------|
| **R** | data read - regular LOAD instructions outside the M1 fetch |
| **W** | write - any write (LD, PUSH, OUT) |
| **X** | execute - byte fetched as part of an instruction (M1 or its operands) |

Classic FCEUX-style CDL flags can be derived from this: flag = (counter > 0).

## Window layout

```
+-----------------------------------------------------+
| [Mode] [x] Export on Exit  [Reset] [Export] [Import]|
| Status: ...                                          |
+-----------------------------------------------------+
| [bus | ram | rom-lower | ... | iorq-8bit | iorq-gdg]|
+----------------------------------------+------------+
| <region stats>                         | <selected> |
| Color/Scale/Threshold/Zoom filter      |            |
+----------------------------------------+ <stats>    |
|                                        |            |
|       H E A T M A P    G R I D         | <reset>    |
|                                        |            |
+----------------------------------------+------------+
```

### Top control bar

| Element | Effect |
|---------|--------|
| **Mode** (radio) | switches Off / With Window / Always |
| **Export on Exit** (checkbox) | export CDL on emulator shutdown |
| **Reset** | clears counters in all regions (`mhmap_reset`) |
| **Export...** | opens a file dialog; writes `meta.json` + per-region `.cdl` files into the chosen directory |
| **Import...** | opens a file dialog; loads a CDL directory for visualization (read-only, does not affect the running recording) |
| **Hide/Show panel** | toggle the right side panel |

### Region tab bar

The tabs correspond to memory regions available for the current architecture:

#### MZ-800 (22 regions)

- `bus` - logical CPU address 0000-FFFF (64 KB)
- `ram` - main DRAM 64 KB
- `rom-lower` - monitor ROM 0000-0FFF (4 KB)
- `rom-cg` - CG-ROM 1000-1FFF (4 KB)
- `rom-upper` - monitor ROM E000-FFFF (8 KB)
- `vram` - physical VRAM 32 KB (4 banks x 8 KB stacked VRAM1..4)
- `vram700-cg` - MZ-700 mode CG-RAM C000-CFFF (4 KB)
- `vram700` - MZ-700 mode text+attr D000-DFFF (4 KB)
- `vram800-320x200_16-{I,II,III,IV}` - 320x200x16 hicolor, 4 planes (8 KB each)
- `vram800-320x200_4{A,B}-{I,II}` - 320x200x4, bank A/B, 2 planes (8 KB each)
- `vram800-640x200_4-{I,II}` - 640x200x4, 2 planes (16 KB each)
- `vram800-640x200_2{A,B}` - 640x200x2, bank A/B (16 KB each)
- `iorq-8bit` - 8-bit I/O ports 00-FF (256 cells)
- `iorq-gdg` - 16-bit GDG sub-functions (256 cells, placeholder)

VRAM has two views: **physical** (`vram`) reflects the state of the 4
hardware banks according to the WE bitmask during the write; **logical
mode-specific** (`vram800-...`) tracks which logical plane in the active
graphics mode was written (driven by the GDG WF/RF registers). The same
write therefore typically increments a cell in both views.

#### MZ-1500 (9 regions)

- `bus`, `ram`, `rom`, `cgrom`, `vram`, `pcg-1`, `pcg-2`, `pcg-3`,
  `iorq-8bit`

### Heatmap grid

The grid adapts in size to the region:

| Region size | Layout |
|-------------|--------|
| 64 KB (bus, ram) | 256 x 256 |
| 32 KB (vram MZ-800) | 256 x 128 |
| 16 KB (640x200 plane, MZ-1500 rom) | 256 x 64 |
| 8 KB (320x200 plane, vram-plane, pcg, rom-upper) | 128 x 64 |
| 4 KB (vram700, rom-cg/lower, cgrom, vram MZ-1500) | 64 x 64 |
| 256 cells (iorq) | 16 x 16 |

The on-screen size of a single cell is controlled by Zoom (1x = 1 px per
cell, 8x = 8 px). For large regions the window scrolls
horizontally/vertically.

#### Color coding (Color)

- **RGB** (default) - three channels: R counter -> red, W -> green,
  X -> blue. Combined colors indicate the ratio of access types (e.g.
  yellow = R+W, magenta = R+X).
- **R / W / X** - a single counter in grayscale (the higher the counter,
  the lighter the gray).

Each channel is normalized against its own maximum within the region
(independently), so a weak W counter can be just as visible as a strong R
counter.

#### Scale

- **Linear** - `intensity = count / max_in_region`
- **Log** - `intensity = log(count+1) / log(max+1)` (default).
  Logarithmic scaling helps to see detail in areas with a wide spread
  (rarely accessed cells next to frequently accessed ones).

#### Threshold

Cells where `max(R, W, X) < threshold` are not displayed (stay black).
Useful for filtering out noise (e.g. threshold=5 hides cells with fewer
than 5 accesses across the whole run).

#### Hover and click

- **Hover over a cell** - tooltip shows region, offset (hex+dec), R/W/X
- **Left click** - selects the cell (yellow frame); details appear in the
  side panel
- **Right click** - context menu (currently an empty placeholder for a
  future "Open in memory browser")

### Side panel (right side)

#### Show: Live / Imported

- **Live** (default) - displays current data from the running recording
- **Imported** (disabled until Import was performed) - displays a snapshot
  loaded from disk

The toggle does not modify recording - it only switches what is rendered.

#### Selected cell

Detail of the selected cell:
- **Addr** - bus address (only for the `bus` region; for physical regions
  it is not well defined and "n/a" is shown)
- **Region**, **Offset** (hex + dec)
- **R / W / X** - counter values + percentage of the region max

#### Region statistics

- **Active cells** - how many cells have `max(R,W,X) > 0` out of the total
- **Total R/W/X** - sum of counters across the entire region
- **Max R/W/X** - the largest single counter in the region

#### Reset region only

Clears counters only in the active region (faster than the global Reset).
Disabled in Imported mode (imported data is a snapshot, we do not modify
it).

## Export

The `Export...` button in the top bar opens a file dialog. After choosing
a target directory the following is created:

```
<dir>/
    meta.json                       # format_version, mzarch, region list
    bus.cdl                         # raw 12-byte cells (R, W, X uint32 LE)
    ram.cdl
    rom-lower.cdl
    ...
    iorq-8bit.cdl
    iorq-gdg.cdl
```

Each per-region file is raw binary: `size_cells * 12 bytes`, where the 12
bytes per cell = `r:uint32 LE, w:uint32 LE, x:uint32 LE`.

If the directory exists, files are overwritten without a warning. If it
does not exist, it is created (`g_mkdir_with_parents`).

The `meta.json` output looks like this:

```json
{
  "format_version": 2,
  "format": "memory-heatmap",
  "mzarch": 800,
  "cell_size_bytes": 12,
  "cell_layout": "r:uint32_le, w:uint32_le, x:uint32_le",
  "regions": [
    { "name": "bus", "file": "bus.cdl", "size_cells": 65536, "size_bytes": 786432 },
    ...
  ]
}
```

### Export on Exit

If the checkbox is active, on emulator shutdown (graceful, via window-close)
export is automatically invoked into the configured directory. The target
directory is the one most recently selected in the dialog (default
`./cdl-export/`).

## Import

The `Import...` button opens a file dialog. Pick a directory containing a
previously exported CDL dump.

Loading:
1. Validates `meta.json` - it must contain `"format_version": 2` and
   `"mzarch": <current arch>`
2. For every region in the table, loads the corresponding file
   (e.g. `bus.cdl`) into memory
3. If individual region files are missing they are tolerated (left
   zeroed) and a warning appears in the Import status line
4. On success the Show toggle is automatically switched to **Imported**

Import allocates a parallel buffer (~13.5 MB for MZ-800, ~11 MB for MZ-1500
and MZ-700).
The buffer lives until the window is closed or the emulator exits.

Important: Import is a **read-only visualization**. Live recording keeps
running; there is no merging with the imported data. To compare live vs
imported, toggle the Show switch.

## CLI options

CDL can be controlled from the command line for automated runs:

```
mz800emu --cdl-mode <off|window|always>     # start with the given mode
         --cdl-dir <dirpath>                # target export directory
         --cdl-save-on-exit <on|off>        # export on shutdown
```

Example: run an MZF program and, when finished (window closed), write CDL:

```
mz800emu --run-mzf game.mzf \
         --cdl-mode always \
         --cdl-dir ./game-cdl/ \
         --cdl-save-on-exit on
```

CLI overrides take precedence over values in `mz800emu.ini` (they are
applied after `cfgmodule_propagate`).

## Persistence (mz800emu.ini)

Section `[DEBUGGER]` in `mz800emu.ini`:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cdl_mode` | KEYWORD | `OFF` | OFF / WITH_WINDOW / ALWAYS |
| `cdl_export_on_exit` | BOOL | 0 | Auto-export on shutdown |
| `cdl_export_dir` | TEXT | `./cdl-export/` | Export target directory |
| `mhwindow_color_mode` | KEYWORD | `RGB` | RGB / R / W / X |
| `mhwindow_log_scale` | BOOL | 1 | Log normalization |
| `mhwindow_threshold` | UNSIGNED | 0 | Hide cells below threshold |
| `mhwindow_zoom` | UNSIGNED | 2 | 1, 2, 4, 8 |
| `mhwindow_side_panel_visible` | BOOL | 1 | Toggle the side panel |
| `mhwindow_selected_region_idx` | UNSIGNED | 0 | Active region tab idx |

The window's visibility itself is **not persisted** - after startup it is
always closed and the user opens it from the menu. Position / size / dock
state are managed automatically by ImGui via `imgui.ini` (its own file).

## Tips and typical workflows

### Disassembly: separate code from data

1. `Mode = Always`, `Color = RGB`, `Scale = Log`
2. Run the program, play it long enough, visit all screens / menus
3. `Export...` -> CDL directory
4. An external disassembler renders instructions only where X > 0; the
   remaining bytes are data

### Find frequently read data (= constants, tables)

1. `Color = R only`, `Scale = Log`, `Threshold = 100`
2. Filtering hides cells with fewer than 100 R; the remaining ones are
   hot data

### Find self-modifying code

1. `Color = RGB`
2. Look for cells that are both green (W > 0) **and** blue (X > 0).
   Cyan / white cells = byte that was written and later executed =
   self-mod

### Track the write pattern into VRAM

1. Select the `vram` tab (physical 32 KB)
2. `Color = W only`
3. You can see where the program writes pixels. For per-mode detail
   switch to `vram800-320x200_16-I` etc.

## Performance

The window renders the grid every frame - for 64K cells at 4x zoom that
is 1024x1024 px, tens of thousands of `AddRectFilled` calls per frame. On
common HW it works, but if the UI feels laggy:

- Lower the zoom (1x is the fastest)
- Apply a threshold (hidden cells = no render calls)
- Close the window when you are not using it (with Mode=With Window this
  also turns recording off)

## Related documents

- [cdl_format.md](formats/cdl_format.md) - detailed description of the CDL format
