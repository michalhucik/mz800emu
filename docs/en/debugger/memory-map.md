# Memory Map - per-arch banking + MemExt visualization

The debugger **Memory Map** window displays the current banking state of
the 64K Z80 address space and a parallel MemExt map. It is per-platform
(MZ-800, MZ-700, MZ-1500). Left- and right-clicking the Banking column
toggles the state of the banking flags; MZ-800 additionally gets a combo
box for switching the DMD mode (MZ-700 emulation mode vs MZ-800 native +
its 8 video sub-modes). The MExt column is an independent visualization
of the MemExt map with a button to open the "MemExt Map Settings"
configuration window.

## Purpose

- Visualize what is currently mapped in the Z80 address space (ROM / RAM
  / CG-ROM / CG-RAM / VRAM / MMIO / Prohibited shadow) without forcing
  the user to manually study the I/O ports and interpret banking flags.
- Detect banking changes "live" - switching ROM ($0000 / $E000),
  CG-ROM/VRAM, MZ-1500 SPEC area, entering Prohibited mode.
- Display the MemExt map (Luftner / PEHU) in parallel to Sharp banking
  so it is visible "if there were RAM on this 4K page, which memext bank
  it would be".
- Allow manually toggling banking + DMD mode via a UI click for the
  "what happens if..." debugging scenario.

## Display architecture

Each table row = a 4 KB page of the address space ($0000..$F000), 16 rows
total. Rendering reads the current state of the banking flags, the DMD
register and the MemExt map and draws 4 columns. For MZ-800 there is an
additional DMD mode combo above the table. Changes are applied in
response to user input (click), not periodically.

## Window layout

```
+------------------------------------+
| <DMD mode combo>    (MZ-800 only)  |
+------+---+---------+---------------+
| Addr |   | Banking |     MExt      |
+------+---+---------+---------------+
| $0000| O | ROM     |    [$00]      |
| $1000|   | CG-ROM  |    [$01]      |
| $2000|   | RAM     |    [$02]      |
| $3000|   | RAM     |    [$03]      |
| $4000|   | RAM     |    [$04]      |
| $5000|   | RAM     |    [$05]      |
| $6000|   | RAM     |    [$06]      |
| $7000|   | RAM     |    [$07]      |
| $8000| O | VRAM I  |    [$08]      |
| $9000| O | VRAM I  |    [$09]      |
| $A000|   | RAM     |    [$0A]      |
| $B000|   | RAM     |    [$0B]      |
| $C000|   | RAM     |    [$0C]      |
| $D000|   | RAM     |    [$0D]      |
| $E000| O | ROM     |    [$0E]      |
| $F000| O | ROM     |    [$0F]      |
+------+---+---------+---------------+
```

Description of the 4 columns:

| # | Column | Content |
|---|--------|---------|
| 1 | **Addr** | hex labels $0000..$F000 (16 rows of 4 kB) |
| 2 | **Marker** | narrow prefix sub-column; contains a white dot if the row is clickable; otherwise empty (shares background with Banking) |
| 3 | **Banking** | region text + color, fixed width so the window stays stable on toggle |
| 4 | **MExt** | button `$XX` (Luftner: 4 kB raw bank including bit 7 FLASH; PEHU: text on even rows, odd ones empty); disconnected MemExt = plain "--", inert |

Marker and Banking share a single row cell (a two-column pair). Clicking
anywhere within this cell - either over the marker or over the Banking
text - toggles the state of that area (rotation, see
[Left click](#left-click-rotation)). An RMB click over the whole pair
opens a popup (even over a non-clickable cell or over an empty marker).

## DMD mode combo (MZ-800 only)

Above the table there is a `Combo` widget with 9 items. It maps the
current DMD register value to a list item, and clicking an item writes
the selected value back.

| # | Label | DMD value |
|---|-------|-----------|
| 1 | MZ-700 | 0x08 |
| 2 | MZ-800 320x200 @ 4 / A | 0x00 |
| 3 | MZ-800 320x200 @ 4 / B | 0x01 |
| 4 | MZ-800 320x200 @ 16 | 0x02 |
| 5 | MZ-800 320x200 @ 16 / U | 0x03 |
| 6 | MZ-800 640x200 @ 2 / A | 0x04 |
| 7 | MZ-800 640x200 @ 2 / B | 0x05 |
| 8 | MZ-800 640x200 @ 4 | 0x06 |
| 9 | MZ-800 640x200 @ 4 / U | 0x07 |

Suffix `/ U` = undocumented combination of bits 1-0 = 11 (per
`hw/09a-undoc-dmd-modes.md`).

DMD -> item mapping on read:

- bit 3 = 1 -> "MZ-700" (bits 0-2 are ignored)
- bit 3 = 0 -> item based on bits 2-0

Clicking an item = write to the DMD register with the same effect as an
OUT (CEh) by the program (including the CTC0 timer coupling to the 700/800
mode), except that it is not written to the trace log and does not trigger
a mode change breakpoint. "MZ-700" writes 0x08 (bits 0-2 are not
preserved).

## Per-platform region maps

### MZ-800 native (DMD bit 3 = 0)

| Address | Region | Flag |
|---------|--------|------|
| $0000-$0FFF | ROM low / RAM | `ROM_0000` |
| $1000-$1FFF | CG-ROM / RAM | shares `CGRAM_VRAM` with $8000+ |
| $2000-$7FFF | always RAM | - |
| $8000-$9FFF | VRAM I / RAM | `CGRAM_VRAM` |
| $A000-$BFFF | only in 640x200 (DMD bit 2=1) VRAM II / RAM, otherwise always RAM | `CGRAM_VRAM` (gated by DMD) |
| $C000-$DFFF | always RAM | - |
| $E000-$FFFF | ROM high / RAM / Prohibited | `ROM_E000` + `PROHIBITED` |

### MZ-800 emul. MZ-700 mode (DMD bit 3 = 1)

| Address | Region | Flag |
|---------|--------|------|
| $0000-$0FFF | ROM / RAM | `ROM_0000` |
| $1000-$1FFF | CG-ROM / RAM | shares the flag with $C000 |
| $2000-$BFFF | always RAM | - |
| $C000-$CFFF | CG-RAM / RAM | shares the flag with $1000 |
| $D000-$DFFF | VRAM (text+attr) / RAM | shares the `ROM_E000` flag with $E000 |
| $E000-$FFFF | MMIO+ROM / RAM / Prohibited | `ROM_E000` + `PROHIBITED` |

### MZ-700 standalone

| Address | Region | Clickability |
|---------|--------|--------------|
| $0000-$0FFF | ROM / RAM | clickable |
| $1000-$1FFF | CG-ROM | **read-only** (click does not toggle) |
| $2000-$BFFF | always RAM | - |
| $C000-$CFFF | CG-RAM | read-only |
| $D000-$DFFF | VRAM | read-only |
| $E000-$FFFF | ROM+MMIO / RAM / Prohibited | clickable |

### MZ-1500

| Address | Region | Flag |
|---------|--------|------|
| $0000-$0FFF | ROM / RAM | `ROM_0000` |
| $1000-$CFFF | always RAM | - |
| $D000-$EFFF | SPEC area - rotation between NONE / CGROM / PCG1 / PCG2 / PCG3 | SPEC mask in map |
| $F000-$FFFF | ROM_UPPER (= ROM E800) / RAM | `ROM_UPPER` |

### Prohibited mode (MZ-800 + MZ-700)

- Activated by OUT 0E5h; the 0x1A shadow is read from $E009-$FFFF,
  $E000-$E008 remain functional MMIO
- Persists across E0/E1/E2/E3/E4 + DMD bit 3 switch
- Reset: OUT 0E6h, GDG reset
- UI marks it in red

## Interaction

### Left click (rotation)

A click (on the marker or the Banking content cell) of a **clickable**
row rotates the state of that area.

**Important HW invariant (MZ-800):** CG-ROM ($1000) and VRAM ($8000+) are
**always synchronous** in 800 mode - you cannot have just one. The
implementation toggles both `ROM_1000` + `CGRAM_VRAM` bits together (per
`hw/03-banking.md`).

Clickability is **DMD-aware** on MZ-800. The white marker dot appears /
disappears dynamically based on the current DMD mode (for example the
$A000 row is clickable only in 640x200 mode).

### Right click (popup menu)

A right click anywhere in the Banking column (even on a non-clickable
cell or on a marker cell) opens a global popup with the legitimate
options for the given mode:

**MZ-800:**

- ROM $0000 -> Mount / Umount
- CG-ROM $1000 -> Mount / Umount (= alias for CG-RAM/VRAM)
- CG-RAM/VRAM -> Mount / Umount
- ROM $E000 -> Mount / Umount / Inhibit (= activate Prohibited)
- ----
- Mount All / Umount All

**MZ-700:**

- ROM $0000 -> Mount / Umount
- ROM $E000 -> Mount / Umount / Inhibit
- ----
- Mount All / Umount All

**MZ-1500:**

- ROM $0000 -> Mount / Umount
- Upper area -> Mount / Umount
- D000 SPEC -> radio: NONE / CGROM / PCG1 / PCG2 / PCG3
- ----
- Mount All / Umount All

Semantics of **Mount All** = set banking such that all
ROM / CG-ROM / VRAM are connected. **Umount All** = everything
disconnected (RAM only).

### Header tooltip

Hovering the "Banking" header shows the tooltip "Left: rotate, Right: menu".

## MExt column

3 states depending on the type of connected MemExt:

| State | Render |
|-------|--------|
| Disconnected | plain `--`, inert (no click, no hover) |
| Luftner (4K) | 16 cells, each a `$XX` button (raw bank including bit 7 = FLASH/SRAM distinction) |
| PEHU (8K) | 8 dual cells, even row is a `$XX` button, odd row empty |

Clicking the button = open the "MemExt Map Settings" window (identical to
the matching item in the Devices menu).

Tooltip over the button: "MemExt remap...".

**Important:** the MExt column is **independent of the Banking column** -
it always shows the current MemExt map regardless of whether Sharp
banking covers a given position. It has no "covered" state, no gray /
strike-through cells, no colors. MemExt only says "if there were RAM
here, it would be this bank"; what really is there from the Z80
perspective is told by the Banking column.

## Cross-window navigation

The Memory Map window is reachable from the I/O Ports Overview panel via
the "Show in Memory Map" item in the right-click context menu for the
banking ports 0xE0-0xE6 and MemExt 0xE7. The window opens if it was
closed and gains OS focus.
