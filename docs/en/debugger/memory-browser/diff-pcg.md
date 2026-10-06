# Memory Browser - Memory Diff + PCG glyph editor

Two standalone windows built on top of the Memory Browser core.
**Memory Diff** compares two sources (live memory or a snapshot)
side-by-side and highlights the differences. **PCG glyph editor** draws 8x8 bitmap
characters into the PCG RAM on the MZ-1500.

Both windows are independent of the main Memory Browser window
(singleton) but share its region and layer definitions (see
[layers-regions](layers-regions.md)).


## 1. Memory Diff (V5)

Window for comparing two data sources (A and B) side by side. It
highlights the bytes that differ between A and B and shows a summary
statistic.

### 1.1 Opening

| Path | Action |
|---|---|
| **Menu** Debugger -> Memory Diff | Opens / closes the window (singleton) |

V5 has no dedicated keyboard shortcut - the window opens from the menu only.

### 1.2 Layout

From top to bottom:

1. Row **A** and row **B** - each source has its own region dropdown,
   mode dropdown, **Snapshot Now** button and a status: `(live)`, the
   size of the taken snapshot (`(65536 B)`) or an orange `(no snapshot)`.
2. The **Auto-snapshot at pause** checkbox and the **Refresh A+B** and
   **Export...** buttons.
3. Side-by-side hex view: offset within the region (8 hex digits),
   16 bytes of A, a `|` separator, 16 bytes of B. Bytes that differ
   between A and B are highlighted in magenta in both columns.
4. Bottom line with a summary statistic.

When opened, A = **Live** and B = **Snapshot**, both over the logical Z80
address space; B has no snapshot yet.

### 1.3 Source region and mode

- **Region** - dropdown over all registered regions (logical Z80 address
  space, physical RAM, VRAM, PCG bank, ...). A and B each have their own
  region; bytes at the same offset are compared.
- **Mode** - how the source gets its data:

| Mode | Meaning |
|---|---|
| **Live** | Current contents of the region, read every UI frame |
| **Snapshot** | Contents taken with **Snapshot Now** (or **Refresh A+B**); kept until you take it again |
| **Auto @ pause** | Like Snapshot, additionally refreshed automatically when the emulator pauses (see 1.5) |

Changing the region or the mode discards the snapshot of that source.
A region larger than 4 MB is clamped to its first 4 MB.

Typical combinations:

- A = **Live**, B = **Snapshot** - "what changed since the last Snapshot Now"
- A = **Snapshot**, B = **Snapshot** - comparing two different moments
  (each source taken at a different time)
- A = **Auto @ pause**, B = **Live** - "what changed since the last
  pause" (after the emulation is resumed)

### 1.4 Snapshot Now and Refresh A+B

- **Snapshot Now** (per source, enabled only in Snapshot and
  Auto @ pause mode) takes the current contents of that source's region.
  The previous snapshot is overwritten without confirmation.
- **Refresh A+B** takes a new snapshot for both sources that are not in
  Live mode.

### 1.5 Auto-snapshot at pause

A checkbox in the toolbar. When enabled, **every** transition of the
emulator into pause (manual stop, breakpoint hit, step) takes a new
snapshot for all sources in **Auto @ pause** mode. The snapshot therefore
captures the state **at the moment of the pause**. The bottom line shows
how many such snapshots were taken (`auto-snapshots taken: N`). Workflow:

1. Set A = **Auto @ pause**, B = **Live** and enable **Auto-snapshot at pause**.
2. Stop the emulator (A now holds the state at the moment of the pause).
3. Resume the emulation and perform an in-game action (e.g. pick up an item).
4. B shows the current state - magenta highlights what changed since the pause.

### 1.6 Export

The **Export...** button opens a dialog for saving a text report
(extensions `.txt`, `.diff`). Format:

```
# Memory Diff export
# Source A size: 65536, Source B size: 65536
# Changed: 3 of 65536 bytes
#
0000C010: 50 -> AA
0000C012: 13 -> 99
0000C014: 00 -> 01
```

One `offset: A -> B` line per differing byte (at most 1,000,000 lines).
If the sources differ in size, a `tail mismatch` note is appended.

Useful as an attachment to a bug report or for an audit log.

### 1.7 Statistic

Bottom line of the window:

- **Changed: X of Y (Z %)** - count of differing bytes out of the
  compared bytes.
- If A and B differ in size, **[size mismatch A=... B=...]** is added.

### 1.8 Use cases

| Scenario | Steps |
|---|---|
| Find where the game holds state (HP, score, position) | A = Snapshot, Snapshot Now, perform an in-game action, B = Live -> magenta bytes are candidates |
| Verify that a cheat / freeze holds | The same over the HP byte, let the player take damage -> value unchanged = freeze works |
| Compare two save states | Load `.mzs` A, A = Snapshot + Snapshot Now, load `.mzs` B, B = Live -> differences between save points |


## 2. PCG glyph editor (V6, MZ-1500 only)

The **PCG editor (MZ-1500)** window for editing 8x8 bitmap characters
stored in **PCG** (Programmable Character Generator) RAM. MZ-1500 specific
feature - 3 banks x 1024 characters x 8 B per character.

### 2.1 Opening

| Path | Action |
|---|---|
| **Memory Browser** -> region PCG bank 1/2/3 -> cursor on a byte -> right mouse button -> **Open in PCG editor...** | Opens the editor and focuses on the character containing the cursor (= addr / 8) |

There is no other way to open it - the editor has no menu item and no
keyboard shortcut. The window is closed with the cross in its title bar.

### 2.2 Layout

From top to bottom:

1. **Bank** dropdown (Bank 1/2/3), number input **Char #** for the
   character index and navigation buttons **[<] [>]**.
2. A row of operation buttons **Inverse / Mirror H / Mirror V /
   Rotate 90 CW / Clear / Fill**.
3. An 8x8 grid of clickable cells (= one glyph, a set pixel is yellow).
4. The **Raw:** line with the 8 bytes of the glyph in hex and the
   **Bank addr range:** line.

### 2.3 Bank selector

The **Bank** dropdown switches between the three MZ-1500 PCG banks
(sub_id 0/1/2). Each bank is 8 KB = 1024 characters x 8 B (one
character = one row of 8 bits per byte, 8 bytes total per glyph).

### 2.4 Character navigation

- **Char #** - number input for direct index entry, **decimal**
  (0-1023), including the **-** / **+** buttons. A value out of range
  is clamped to 0 or 1023.
- **[<] [>]** - prev/next character (jump by 8 B within the PCG
  bank). Stops at characters 0 and 1023 (no wraparound).
- Below the grid the editor shows the line **Raw:** with the 8 bytes
  of the current character in hex and the line **Bank addr range:**
  with the byte range of the character inside the bank
  (`char_idx * 8` .. `char_idx * 8 + 7`).

### 2.5 Drawing

- **Left click** on a cell in the 8x8 grid - toggle a bit (0/1, i.e.
  bg/fg pixel).
- The **Raw:** line under the grid shows the current 8 bytes of the
  glyph (= bytewise representation, one byte = one row of the bitmap,
  bit 7 = leftmost pixel). The line is read-only.

### 2.6 Whole-glyph operations

| Button | Effect |
|---|---|
| **Inverse** | Flip all 64 bits (= negative of the glyph) |
| **Mirror H** | Horizontal mirror (= reverse the bits within each of the 8 bytes) |
| **Mirror V** | Vertical mirror (= reverse the order of the 8 bytes) |
| **Rotate 90 CW** | 90 degree clockwise rotation |
| **Clear** | Sets all 8 bytes to zero (empty glyph) |
| **Fill** | Sets all 8 bytes to 0FFh (solid glyph) |

### 2.7 Writing into PCG RAM

The editor keeps no in-progress copy and has no Save / Reload buttons.
**Every change** (a click into the grid as well as an operation from 2.6)
is **written immediately** - all 8 B of the glyph go into the PCG bank at
address `char_idx * 8`. The change is reflected in the emulator video
output right away (if the glyph is displayed). An edit can only be undone
by the opposite operation or by redrawing it manually.

The editor reads the glyph from PCG RAM every frame, so it also shows
changes made meanwhile by the running program or by another window.

### 2.8 Per-architecture availability

| MZARCH | Behavior |
|---|---|
| **MZ-1500** | Fully functional (PCG is the central feature of the MZ-1500 video subsystem) |
| **MZ-800** | The window cannot be opened - there is no PCG bank region, so the **Open in PCG editor...** item is missing too |
| **MZ-700** | Same as MZ-800 - no PCG hardware support |

### 2.9 Use case: custom font glyph

1. Open Memory Browser, switch to region **PCG bank 1**.
2. Find a free slot (e.g. char 0x90 where the ROM has a gap or an
   unused character).
3. Right mouse button -> **Open in PCG editor...**.
4. In the editor click on cells - draw the desired glyph. Each click is
   written straight into PCG RAM. The **Char #** field shows the index in
   decimal (0x90 = 144).
5. A game or application that prints the character with code 0x90 via
   VRAM now sees the new glyph.


## 3. Related

- [memory-browser](README.md) - main Memory Browser window
- [layers-regions](layers-regions.md) - region and layer definitions
  (required for region selection in Diff and navigation in PCG)
- [search](search.md) - searching for byte patterns (useful together
  with Diff to locate state variables)
