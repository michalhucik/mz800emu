# Memory Browser - Layers, Regions tree, Multi-view

## 1. What it is and what it is for

Memory Browser provides three subsystems on top of the main hex view
that allow memory inspection beyond a plain byte dump:

- **Layers panel** (right) - layered visualizations over hex cells
  (CDL, heatmap, snapshot Delta, frozen) + symbols and bookmarks in
  the ASCII column
- **Regions tree sidebar** (left) - hierarchical tree of memory areas
  per architecture, aware of connected/disconnected HW
- **Multi-instance** - 5 independent Memory Browser windows for
  parallel views on different areas

Layers and Regions both have collapsible sidebars separated by
splitters, so each user lays out the space according to preference.


## 2. Layers panel

Sidebar to the right of the hex view. Can be collapsed and width
adjusted via a splitter. Contains 6 mutually independent toggleable
layers (ON/OFF). The default is all **OFF**, state persisted per
instance in INI.

### Layer overview

| Layer | Visualization | Data source |
|--------|-------------|-----------|
| **CDL Code (X)** | light blue cell background | Code/Data Logger - byte executed as instruction |
| **CDL Data Read (R)** | light green cell background | CDL - byte read as data (LD A,(HL) etc.) |
| **CDL Data Write (W)** | light red cell background | CDL - byte written as data |
| **Heatmap** | gradient cool->hot | access count with log normalization (= cool blue for few, hot red for many) |
| **Snapshot Delta** | magenta highlight | bytes changed since the last manual snapshot |
| **Frozen bytes** | dark purple background | bytes frozen via edit-tools ("lock byte" cheat) |

CDL layers can be combined - a byte that was both an instruction and
data (typical for SMC code) will have a mixed background according
to the Z-order of rendering.

### Heatmap normalization

The heatmap uses a log scale because in practice raw access counts
differ by orders of magnitude (frequently called routine vs. init
code). Without the log scale, the hot region would squash all other
signal to zero.

### Snapshot Delta workflow

1. Click **Manual Snapshot** in the Layers panel - the emulator stores
   a baseline copy of the current region contents
2. Continue emulating / perform the action you want to track
3. Enable the **Snapshot Delta** layer - magenta highlight marks
   bytes that changed since the baseline

### Symbol overlay in the ASCII column

If sym_db contains a symbol matching the row address, the ASCII
column shows the symbol name in yellow (TextColored) before the actual
ASCII text. Hovering over a symbol shows a tooltip with:

- **name** - symbol identifier
- **kind** - label / map / NoICE / sjasmplus / bookmark
- **comment** - note from the symbol, if any

### Bookmark add/remove

Right-click on a row in the hex view -> context menu **Add bookmark**
/ **Remove bookmark**. The bookmark is stored in sym_db as an entry
with kind **BOOKMARK**. The 1-char mark column shows the `*` marker.

Bookmarks are shared across all Memory Browser instances (= sym_db is
global).


## 3. Regions tree sidebar (V2)

Sidebar to the left of the hex view. Also collapsible and
splitter-resizable. Contains a hierarchical tree of memory areas
available in the current architecture. Connected HW is shown in full
color, disconnected greyed out (= visible as a placeholder but cannot
be switched into).

### Tree structure (per arch)

The tree reflects differences between mz800, mz700 and mz1500:

- **Logical Z80** (64 KB) - what the CPU sees through current banking
- **User RAM** (64 KB) - bare RAM without ROM overlay
- **Monitor ROM** - lower 4 KB + upper 8 KB
- **CG-ROM** (4 KB) - character generator
- **VRAM**
  - Physical Planes I / II / III / IV (8 KB each, MZ-800;
    III and IV only if exVRAM is connected)
  - VRAM 700 char + attr (2 KB each, only in 700 / 700-compat mode)
- **CG-RAM** (4 KB, only in 700 mode)
- **PCG bank 1 / 2 / 3** (8 KB each, only MZ-1500)
- **Memext** (Luftner / PEHU, only if connected)
  - RAM bank 0x00 .. 0x7F (4 KB each; a PEHU 8 KB bank N is shown as the two
    4 KB parts 2*N and 2*N + 1)
  - FLASH bank 0x00 .. 0xN (Luftner, read-only)
- **Ramdisk STD** (if connected, bank count depends on type)
- **Ramdisk PEZIK port 0x68** + **port 0xE8** (if connected)

### Per-row physical origin label

In the **Logical Z80** view (= what the CPU sees through banking) an
optional TextDisabled label of the row origin is shown after the
ASCII column:

```
... ASCII ...  | RAM
... ASCII ...  | ROM lower
... ASCII ...  | Memext bank 0x4A
```

The toggle is controlled by `show_origin_labels`. When debugging
banking situations (= "why does the CPU read this from this address")
this label is essential.

### Banking status bar (per arch)

The bottom of the window contains a banking status bar whose contents
depend on the architecture:

| Arch | Indicators |
|------|------------|
| **MZ-800** | `[PROHIBITED]`, `[SCRW640]`, `[HICOLOR]`, `[VBANK]`, **DMD: MZ-800/700 mode (0xXX)** |
| **MZ-700** | `[PROHIBITED]` (if active) |
| **MZ-1500** | (nothing, MZ-1500 has no equivalent banking flags) |

PROHIBITED denotes a state where the ROM monitor write-protected a
RAM area through a hardware mechanism.

### Cross-window "Show in Memory Map"

Right-click in hex view -> **Show in Memory Map**. Opens the
**Memory Map** window and focuses it on the 4 KB page corresponding
to the current cursor address. Useful when combining Memory Browser
+ Memory Map to understand the banking state.

### PEZIK byte order

For the PEZIK ramdisk an informational byte order indicator is shown
(BE/LE). **Note**: the byte order toggle in the emulator is
**missing** so far - the indicator is informational only, BE/LE
conversion has to be done by the user mentally.


## 4. Multi-instance (V3)

Memory Browser is available in **5 independent instances**
(`MB_INSTANCE_COUNT = 5`):

- **main** - main window, opened by default from the Debugger menu
- **#2 .. #5** - secondary windows, opened from the menu
  **Debugger -> Other memory browsers -> Memory Browser #N**

### Per-instance state isolation

Each instance has its own independent state for:

- current region (= what is shown)
- encoding (= how to interpret the ASCII column)
- cursor position, scroll, selection
- layers (= which layers ON/OFF)
- splitter positions (regions sidebar width, layers sidebar width)
- show_origin_labels toggle

### Shared across instances

Some items are logically global (= not per-instance):

- **freeze bytes** - one global list of frozen bytes
- **bookmarks** - sym_db is single for the whole emulator
- **annotations** - notes attached to bytes

When you add a bookmark in the main instance, it immediately appears
in #2..#5 too.

### Stable IDs

ImGui windows use the three-hash stable ID convention so the title
bar can change dynamically without losing docking/position state:

```
"Memory Browser###mb_main"
"Memory Browser #2###mb_2"
"Memory Browser #3###mb_3"
"Memory Browser #4###mb_4"
"Memory Browser #5###mb_5"
```

### Persistence

Per-instance state is saved into cfgmain INI in separate sections:

- `[MEMBROWSER_WINDOW_MAIN]`
- `[MEMBROWSER_WINDOW_2]` .. `[MEMBROWSER_WINDOW_5]`

### DBG Workplace integration

Each instance has its own toggle in the **DBG Workplace** system, so
it can be turned on / off with the entire workplace configuration
(= e.g. the "profiling workplace" has only main + #2, the "memory
debug workplace" has all 5).


## 5. Typical use cases

### Find all code in Logical Z80

1. Region: **Logical Z80**
2. Turn on the **CDL Code (X)** layer
3. Scroll the hex view - light blue background shows what the Z80
   executed as instructions
4. **Tip**: pair with **Heatmap** for visual "hot paths" (= frequently
   executed instructions will be simultaneously blue + hot)

### Tracking memory changes during gameplay

1. Region: **User RAM** (= banking-aware "what the CPU sees as RAM")
2. **Pause** the emulator
3. Click **Snapshot** in the Layers panel (baseline)
4. **Resume** + perform a short game action (= one action to track)
5. **Pause** + enable the **Snapshot Delta** layer
6. Magenta highlight shows the bytes that changed

### Cheat lock (freeze HP)

1. Find the HP byte (search in Memory Browser or manual scroll)
2. Right-click -> **Freeze byte at cursor (= 0xXX)**
3. The emulator overwrites the value back to the frozen content
   every frame
4. Enable the **Frozen bytes** layer for a visual badge on frozen
   addresses

### Multi-window comparison of 2 areas

1. Open the **main** Memory Browser
2. Open **#2** via Menu Debugger -> Other memory browsers ->
   Memory Browser #2
3. Switch main to region X, #2 to region Y
4. Arrange windows in ImGui side-by-side for a parallel view


## 6. Related

- [hex-view](hex-view.md) - core hex viewer + edit
- [search](search.md) - search engine
- [edit-tools](edit-tools.md) - undo/redo, fill, annotations, freeze
- [Memory Map](../memory-map.md) - banking + memext per page
- [Symbols](../symbols.md) - symbol browser (sym_db)
- [Bookmarks](../bookmarks.md) - bookmark editor
