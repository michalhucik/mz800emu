# Events panel (Event Viewer)

The Events panel is a real-time view of significant HW and CPU events of
the emulator (IORQ / MMIO, INT / NMI / RETI, banking, GDG mode/palette,
PSG, CTC, PIO, FDC, BP fire, user mark, HALT/RST, SYS lifecycle).
Inspired by **Mesen2 Event Viewer**.

## Contents

- [Panel purpose](#panel-purpose)
- [Opening the window](#opening-the-window)
- [Toolbar](#toolbar)
- [Log tab](#log-tab)
- [Strip tab](#strip-tab)
- [Bookmarks](#bookmarks)
- [Saved filter presets](#saved-filter-presets)
- [Row coloring](#row-coloring)
- [Hotkeys](#hotkeys)
- [Categories and subtypes](#categories-and-subtypes)
- [Filter syntax](#filter-syntax)
- [Pause-on-match (stream BP)](#pause-on-match-stream-bp)
- [Auto-mark on match](#auto-mark-on-match)
- [Detail column decoders](#detail-column-decoders)
- [Sub column short codes](#sub-column-short-codes)
- [Group by in the Log tab](#group-by-in-the-log-tab)
- [Per-frame heatmap](#per-frame-heatmap)
- [Export / Import of the ring](#export--import-of-the-ring)
- [FDC command tracking](#fdc-command-tracking)
- [SYS category](#sys-category)
- [Interaction - click and context menu](#interaction---click-and-context-menu)
- [Follow tail](#follow-tail)
- [Persistence (cfg section)](#persistence-cfg-section)
- [Related panels](#related-panels)

## Panel purpose

The Events panel complements Trace Suite (= post-mortem file log) with
an **online live view** of what is happening in the emulator right now.
It serves for:

1. **Live debugging** - "what is happening right now in the emulator"
   in real time, without having to stop the emu and walk through chip
   states one by one.
2. **Cross-window navigation** - clicking a row = pause emu and jump
   into the disassembler at the PC where the event occurred.
3. **Filter-first analysis** - there are 24 categories, thousands of
   events per second; the filter syntax and per-category checkboxes
   separate the signal from the noise.

Difference compared to other existing panels:

| Panel             | What it covers                            | When to use it                   |
|-------------------|-------------------------------------------|----------------------------------|
| I/O Ports History | Last ~10000 IORQ + MMIO events            | Specifically for ports           |
| Trace Suite       | Disk file (cputrack/intlog/hwlog/...)     | Post-mortem RE pipeline          |
| **Events**        | Last 50000 HW/CPU events (in-memory)      | Live "what is going on right now"|

## Opening the window

Menu **Debugger -> Events** (keyboard shortcut not yet defined). The
window is dockable, persisted via the cfg key
`[EVENT_VIEWER_WINDOW] window_open`.

**First steps after opening:**

1. In the toolbar switch the **Mode** combo from `OFF` (= default) to
   `WHEN_WINDOW_OPEN` or `ALWAYS`. In `OFF` nothing is written to the
   ring (= the table is empty).
2. Start / continue running the emu - events start to appear in the
   table.
3. If you only need certain categories, click the **Categories**
   dropdown in the toolbar and uncheck the rest.

## Toolbar

```
Mode: [WHEN_WINDOW_OPEN v]   Capacity: [50000     ]   [Clear]
Categories [v]   [Only IRQ] [Only banking] [Only video] [Only PSG] [Only memory] [Hide noise]
Filter: [.........................................] [?]
Follow tail: [x]
Counters:  Total: 12450   Filtered: 230   CPU_INT: 12   IORQ: 1840   GDG: 80 ...
```

### Mode combo

| Value               | Active when                                | Use                       |
|---------------------|--------------------------------------------|---------------------------|
| `OFF`               | Never                                      | Default, no overhead      |
| `WHEN_WINDOW_OPEN`  | Only while the Events window is open       | Standard debugging        |
| `ALWAYS`            | Always while the emu is running            | Power user, always in mem |

The default is `OFF` so that ordinary users get no hot-path overhead.
After the first time you open the window you typically switch to
`WHEN_WINDOW_OPEN`.

### Capacity

Slider in the range 10 000 - 200 000 events. Default 50 000. A change
reallocates the ring and **discards the previous data** (= ImGui shows a
confirmation message).

Memory footprint:

- 10 000 events = 240 KB
- 50 000 events = 1.2 MB
- 200 000 events = 4.8 MB

### Clear button

Empties the ring (= count = 0, the ring stays allocated). Useful after
changing the tested scenario.

### Categories dropdown

Per-category visibility (24 checkbox items). A disabled category **does
not enter the ring at all** (= bitmask in the hot-path gate, zero
overhead for filtered-out data).

The quick toolbar buttons **Only IRQ / Only banking / ...** preset the
mask to a typical subset (= one click instead of unchecking manually).

### Filter textbox

Text-based filter (see [Filter syntax](#filter-syntax) below). Applied
in the UI layer over the ring (= you see only rows that match). The
filter acts as AND with Categories - both must allow a row for it to
appear.

The **?** popup next to the textbox shows a brief cheatsheet of the
syntax.

### Counters

Live statistics:

- **Total** = number of events in the ring (max = capacity)
- **Filtered** = number of events after applying Categories + Filter
- **Per category** = number of events per category in the ring (helps
  you know "what is here the most")

## Log tab

Table view. Columns:

| Column | Meaning                                                            |
|--------|--------------------------------------------------------------------|
| Pxclk  | Pixel clock counter (NOT CPU T-states) - unified timestamp         |
| Frame  | Frame number (= `screens_total`)                                   |
| Sline  | Scanline within the current frame                                  |
| Px     | Pixel column within the current scanline                           |
| PC     | CPU PC at the moment of the event (hex 16-bit)                     |
| Cat    | Category name (= `cpu_int`, `iorq_in`, `gdg_mode`, ...)            |
| Sub    | Subtype short code (max 8 chars, e.g. `HALT_E`, `CTL_W`)           |
| Detail | Short decoded description (rich per-category decoder)              |

### Meaning of Pxclk

`Pxclk` is a value from the **pixel clock domain** (= GDG internal
oscillator, MZ-800 17.7345 MHz). It does NOT match CPU T-states (=
MZ-800 3.5469 MHz = pxCLK / 5). Reason: timing in the Event Viewer is
unified with the trace-suite format (= cross-mergeable with hwlog/intlog
disk chunks), and GDG events (= VBLN/VS/HS) must be capturable with
pixel precision.

Conversion: CPU cycle = `pxclk / 5` (MZ-800/700), `pxclk / 4`
(MZ-1500).

### Sline / Px = raster position

`Sline` = `pxclk_in_screen / screen_width` (typically 320 for MZ-800
H40 or MZ-700, 640 for MZ-800 H80).

`Px` = `pxclk_in_screen % screen_width`.

Use: for raster effects debug (= "on which scanline does my BP fire?"),
mid-frame palette switching, border raster effects.

### Detail decoder

The Detail column shows human-readable text formatted by event
category - the complete table is in the section
[Detail column decoders](#detail-column-decoders).

Examples:

- `port=0xCE (GDG DMD) val=0x08`
- `DMD=0x08 (MZ-700 mode)`
- `PIOZ80_PA vec=0x40 isr=0x4042`
- `BP #5 reason=2 MARK`
- `"isr_entry"`
- `HALT enter`
- `RST 0x38`

## Strip tab

A 2D map of events of the current frame. Inspired by Mesen2 Event
Viewer. X-axis = pixel column 0..VIDEO_SCREEN_WIDTH-1, Y-axis =
scanline 0..VIDEO_SCREEN_HEIGHT-1. Each event = one point, color by
category, size by priority.

### What for

- "Where in the raster does my BP fire?" - you see BP_FIRE points
  overlaid on a 2D map, immediately seeing sline + px.
- Per-frame raster effects debug - mid-frame palette switching shows a
  cluster of GDG_COLORS points across scanlines.
- Border raster effects show colored stripes of points on specific
  scanlines.
- IRQ timing (IM2 demos) shows IRQ_ACK_IM2 as regular red points.

### Strip tab toolbar

```
Mode: [Fit to window v]   Zoom: [1.00x ====]   [x] Show previous frame   [ ] Grid   [x] Legend
Frame: 1234  Events drawn: 487
> Strip colors  (collapsible per-category color picker)
```

### Mode combo

| Value           | Meaning                                                        |
|-----------------|----------------------------------------------------------------|
| Fit to window   | Canvas scales proportionally to the available space. Zoom slider grayed out. Default. |
| 1:1             | Canvas has a fixed size 1 logical px = 1 physical px * zoom. Scrollbars if it does not fit in the window. |

### Zoom slider

Functional only in "1:1" mode. Range 0.5x..4.00x, default 1.00x. On
canvas hover **Ctrl+mouse wheel** = zoom in / out works (in 1:1 mode).

### Show previous frame

Toggle. If ON, Strip draws points from **two** frames at once:

- Current frame = full alpha.
- Previous frame (= current - 1) = half alpha (ghost overlay).

On hover the tooltip shows the header "(previous frame)" if the event
comes from N-1.

### Grid (toggle)

Optional grid in canvas coordinates:

- Secondary lines every 64 px / 32 sline (light gray).
- Primary highlighted lines every 256 px / 128 sline (more saturated).

Helps with orientation - you quickly see "this point is around sline
96".

### Legend (toggle)

Default OFF (= saves vertical space in a small window). When enabled it
draws below the canvas (= between the toolbar and the canvas) two
blocks:

1. **Dot size legend** - "large/medium/normal/small/tiny" + description
   of categories that have that size.
2. **Color legend** - 3-column table of visible categories only +
   colored dots in real size + name.

### Dot size per category

| Size   | Radius  | Categories                                              |
|--------|---------|---------------------------------------------------------|
| large  | 4 px    | BP_FIRE, USER_MARK (debugger markers)                   |
| medium | 3 px    | IRQ_ACK_IM2 (IM2 dispatch)                              |
| normal | 2.5 px  | CPU_INT, CPU_CTRL (IM/IFF, HALT/RST)                    |
| small  | 1.5 px  | default (most HW events)                                |
| tiny   | 1 px    | GDG_VIDEO (HBLN/HS edges - otherwise they would drown others) |

### Color picker with Reset

In the toolbar "> Strip colors" collapsible. Per-category ColorEdit3 in
a 4-column grid. The "Reset to defaults" button restores the whole
palette to the default Mesen-inspired scheme.

**Important:** The color is **shared with the Log tab row coloring** -
a change in the Strip Colors picker immediately shows in the Log tab
(= one SSOT for both views).

### Scanline cursor

A yellow cross-hair on the current electron beam position. The vertical
line = current pixel column, horizontal = current scanline. Next to the
intersection a small text "sline=N px=M".

While the emu is running the cursor "flies" across the raster; while
the emu is paused (Ctrl+F5 or Alt+P) it stands at the exact position
where the emu froze. Useful for raster timing debug.

### Visible screen rect vs active canvas rect

The canvas displays two semi-transparent rectangles:

- **White rectangle** = visible display area (= including the border,
  MZ-800 PAL 928 x 288 px).
- **Gray rectangle** = active canvas / active pixel area (= without the
  border, MZ-800 H40 320x200 in the middle).

Events outside the visible rect = blanking interval (HSYNC, VSYNC) -
common HW events are also logged there, but border raster effects
typically happen in the border zone between the visible rect and the
canvas rect.

### Hover tooltip

Hovering over a point shows a tooltip:

```
(previous frame)         <- if event from N-1
---
Pxclk: 12345678
Frame: 1234  Sline: 96  Px: 240
PC: 0x4042
---
Cat: bp_fire
Sub: MARK
---
BP #5 reason=2 action=MARK
```

The tooltip also contains the full subtype description (= reuses the
same strings shown by the Log tab hover on the Sub cell).

### Click UX

| Action            | What it does                                                          |
|-------------------|-----------------------------------------------------------------------|
| Single click      | Select highlight (white outline around the point).                    |
| Double click      | Pause emu + show in disasm at the event PC.                           |
| Right click       | Context menu (Pause+disasm / Pause here / Bookmark toggle / Show in Log tab / Copy event). |
| Ctrl+wheel hover  | Zoom in / out (only in 1:1 mode).                                     |

### "Show in Log tab"

A context menu item. Switches the TabBar to the Log tab and
auto-scrolls to the row of the corresponding event (= centered in the
viewport). Useful for the cross-tab transition "I see a suspicious
point in the Strip -> I want to see it in tabular detail".

## Bookmarks

A per-event star marker for tagging interesting events for later
return. Identified by the key `(frame, pxclk_in_screen)`.

### How to add / remove

- **Log tab** - star column. Click = toggle. Yellow = marked, gray =
  not.
- **Strip tab** - right click on a point -> context menu "Bookmark this
  event" / "Remove bookmark".

### How to navigate

In the toolbar the bookmark control row:

```
* 5    < Prev   Next >   [ ] Show only *   Clear all
```

- **< Prev** - jump to the previous bookmark from the currently
  selected row. Hotkey: **Ctrl+Shift+B**.
- **Next >** - jump to the next bookmark. Hotkey: **Ctrl+B**.
- **Show only *** - filter override. Hides unmarked events
  (independently of the current filter expression).
- **Clear all** - mass delete with a confirm popup "Remove N
  bookmarks?".

Jumping to a bookmark **turns off Follow tail** (= the user explicitly
picked a position and does not want to lose it on the next auto-scroll).
It comes back to ON only on explicit click on "Follow tail" in the
toolbar or the `F` hotkey.

### Persistence

Bookmarks are maintained within the current session. After an emulator
restart the list is discarded.

### Survives ring overflow

The bookmark key `(frame, pxclk)` stays registered even when the given
event is no longer in the ring (= the ring overwrote it). After wrap
the Next/Prev navigation simply skips (= no match in the current ring).
When you want to clean up, "Clear all" deletes the whole list.

## Saved filter presets

A user-defined library of filter expressions with persistence in the
cfg section `[EVENT_LOG_FILTERS]`. Max 32 presets per session.

### Save current as... workflow

1. Type a filter expression into the Filter textbox (e.g. `cat:cpu_int
   pc:38-FF`).
2. Click **"Save as..."** next to the Saved filters combo.
3. Modal popup "Save preset" -> enter a **Preset name** (e.g.
   `isr_trace`).
4. Click **Save**. The preset is saved into the combo + marked as
   active.

If the entered name already exists, the popup switches to **Replace
confirm** ("Preset 'X' already exists. Replace?"). Confirm = update the
expr of the existing slot.

Validation:

- **Empty name** -> Save button disabled.
- **Empty filter expression** -> warning message "Warning: saving an
  empty filter (matches all events)." Save still proceeds (= legitimate
  case for a "Clear filter" preset).

### Quick filter vs Saved filter

| Aspect        | Quick filter dropdown                       | Saved filters                |
|---------------|---------------------------------------------|------------------------------|
| Definition    | Hardcoded compile-time (Only IRQ etc.)      | User-defined runtime         |
| Persistence   | None                                        | Cfg `[EVENT_LOG_FILTERS]`    |
| Max count     | 10 buttons                                  | 32 slots                     |
| Editing       | No                                          | "Save as..." / "Delete"      |
| Tooltip       | Expression preview per item                 | Expression preview per item  |

Both dropdowns live side by side in the toolbar and are fully
independent. Quick filter serves as "ready-made" recipes, Saved as a
personalized library.

### Delete UX

1. Activate the target preset in the "Saved filters" combo (= click an
   item).
2. Click **"Delete"** next to the combo.
3. Confirm popup "Delete preset 'X'?" -> confirm.

After deletion the combo preview resets to "(none)", the filter
textbox stays unchanged.

### Cfg section

```ini
[EVENT_LOG_FILTERS]
preset_00_name = isr_trace
preset_00_expr = cat:cpu_int,irq_ack_im2,cpu_ctrl
preset_01_name = video_debug
preset_01_expr = cat:gdg_mode,gdg_colors sline:0-200
preset_02_name = audio_debug
preset_02_expr = cat:psg
; slots 03..31 empty = unused
```

A cfg section with 32 slots `preset_NN_name` + `preset_NN_expr`, empty
name = slot ignored. Sync happens on every mutation (Save / Delete) and
before cfg save.

## Row coloring

Toggle "Color rows" in the toolbar (next to "Follow tail"). Default ON,
cfg key `row_coloring`.

When ON, Log table rows have a dark colored background according to
event category (alpha 96 = pronounced but white text stays readable).
Shares color with the Strip tab "Strip colors" picker.

When OFF, rows use standard zebra striping (alternating
lighter/darker gray).

**Tip:** For a custom palette open the Strip tab -> "Strip colors"
collapsible -> edit `ColorEdit3` per category. The change takes effect
in the Log tab on the next render.

## Hotkeys

Table of keyboard shortcuts available in the Events window:

| Hotkey            | Action                                  | Scope                              |
|-------------------|-----------------------------------------|------------------------------------|
| `Alt+P`           | Pause / resume emu toggle               | Events window focus                |
| `F`               | Follow tail toggle                      | Events window focus (outside textbox) |
| `Ctrl+B`          | Jump to next bookmark (Next)            | Events window focus                |
| `Ctrl+Shift+B`    | Jump to previous bookmark (Prev)        | Events window focus                |
| `Ctrl+wheel`      | Zoom in / out Strip canvas (1:1 mode)   | Strip canvas hover                 |
| Double click      | Pause + show in disasm                  | Log row / Strip point              |
| Right click       | Context menu                            | Log row / Strip point              |

The `F` hotkey is gated so that typing `f` in the filter textbox does
not toggle Follow tail.

## Categories and subtypes

24 stable categories. The `Sub` column in the Log tab shows a **short
code** (max 8 chars, e.g. `HALT_E`, `BORDER`, `CTL_W`) - the complete
table is in the section [Sub column short codes](#sub-column-short-codes).
Hovering on the Sub cell shows the full description in the tooltip. The
numeric subtype values remain for user reference:

### IORQ categories (3 = `iorq_in`, 4 = `iorq_out`)

| Sub | Meaning                                            |
|-----|----------------------------------------------------|
| 0   | NORMAL - port is mapped                            |
| 1   | UNCONNECTED - ghost cycle on an unmapped port      |

### BP_FIRE category (18 = `bp_fire`)

| Sub | Meaning                                                |
|-----|--------------------------------------------------------|
| 0   | HALT - empty action, emu halt                          |
| 1   | MARK - action `mark "name"`                            |
| 2   | CONTINUE - log / poke / set / var / continue           |
| 3   | IGNORE (reserved)                                      |
| 4   | ENABLE - action `enable <bp>`                          |
| 5   | DISABLE - action `disable` or `disable_self`           |

### CPU_CTRL category (20 = `cpu_ctrl`)

| Sub | Meaning                              |
|-----|--------------------------------------|
| 0   | HALT_ENTER - HALT opcode dispatch    |
| 1   | HALT_EXIT - IRQ/NMI woke HALT        |
| 2   | RST_00                               |
| 3   | RST_08                               |
| 4   | RST_10                               |
| 5   | RST_18                               |
| 6   | RST_20                               |
| 7   | RST_28                               |
| 8   | RST_30                               |
| 9   | RST_38                               |

### Other categories (hwlog passthrough)

Subtypes for HWLOG categories (`gdg_mode`, `gdg_banking`, `pio8255`,
`ctc8253`, `pioz80`, `psg`, `fdc`, `memext`, `qd`, `rd`, `gdg_*`,
`cpu_int`, `cpu_pin_edge`, `irq_ack_im2`) **map 1:1** to the
sub_event_type values in the HW-log format - see
[`formats/HW-log_format.md`](formats/HW-log_format.md) per-chip
sub_event_type tables.

Specific values - a quick summary:

- `gdg_colors`: 1=BORDER, 2=PALGRP, 3=PAL, 4=PCG, 5=PACKETGROUP
- `gdg_video`: 1=VBLN_START, 2=VBLN_END, 3=VS_START, 4=VS_END,
  5=HBLN_START, 6=HBLN_END, 7=HS_START, 8=HS_END
- `pio8255`: 1=PORT_A_W, 2=PORT_B_W, 3=PORT_C_W, 4=CONTROL_W
- `ctc8253`: 1=CONTROL_W, 2=COUNTER_W
- `pioz80`: 1=MODE_W, 2=VECTOR_W, 3=INT_CTRL_W, 4=MASK_W, 5=IO_SELECT_W,
  6=DATA_W, 7=DATA_R, 8=BUS_INPUT_CHANGE, 9=IRQ_ACK_M2, A=RETI_APPLIED

## Filter syntax

The filter syntax supports three layers of atoms: basic atoms (`cat:`,
`pc:`, `frame:` ...), symbol-aware atoms (`sym:`, `from_sym:`,
`to_sym:`) and state-aware + temporal atoms (`if iff1:N`,
`before(N) <expr>` ...).

### Basic rules

- **Spaces = AND** (= all tokens must match)
- **`!token` = negation** of the token
- **`( a or b )` = OR** group (only inside parentheses, not at the top
  level)
- **`!( ... )` = negation** of the whole group

### Operators (expression structure)

| Operator         | Meaning                                          | Example                          |
|------------------|--------------------------------------------------|----------------------------------|
| whitespace       | AND (implicit)                                   | `cat:cpu_int pc:4000-40FF`       |
| `!`              | negation of the following atom                   | `!cat:gdg_video`                 |
| `( ... or ... )` | OR group (parentheses mandatory, only inside)    | `( cat:cpu_int or cat:bp_fire )` |
| `!( ... )`       | negation of the whole group                      | `!( pc:38 or pc:66 )`            |

### Basic atoms

| Token       | Value                                  | Example                  |
|-------------|----------------------------------------|--------------------------|
| `cat:`      | name[,name]* of 24 category names      | `cat:cpu_int,gdg_mode`   |
| `sub:`      | num[,num]* (0..255)                    | `sub:0,1`                |
| `pc:`       | hex (0..FFFF) or hex-hex range         | `pc:4000-40FF`           |
| `frame:`    | dec, `>N`, `<N` or dec-dec range       | `frame:>100`             |
| `cycle:`    | dec, `>N`, `<N`, range; suffix `k`/`M` | `cycle:>1M`              |
| `sline:`    | dec or dec-dec range (0..311 PAL)      | `sline:50-150`           |
| `px:`       | dec or dec-dec range (0..1135 PAL)     | `px:160-200`             |
| `payload:`  | hex (`0x` prefix optional)             | `payload:0xCE`           |

### Values for `cat:`

Category names (= lower case + underscore):

`cpu_int`, `cpu_pin_edge`, `irq_ack_im2`, `iorq_in`, `iorq_out`,
`mmio_r`, `mmio_w`, `gdg_mode`, `gdg_banking`, `gdg_hwscroll`,
`gdg_colors`, `gdg_wfrf`, `gdg_video`, `pio8255`, `ctc8253`, `pioz80`,
`psg`, `qd`, `fdc`, `memext`, `rd`, `bp_fire`, `user_mark`, `cpu_ctrl`,
`sys`.

### Symbol-aware atoms

Symbols are loaded from a `.lbl` file via the disasm window "Load
symbols".

| Token                 | Meaning                                  | Example                    |
|-----------------------|------------------------------------------|----------------------------|
| `sym:NAME`            | PC == addr of symbol `NAME` (exact)      | `sym:isr_handler`          |
| `sym:PREFIX_*`        | PC == addr of any `PREFIX_*` symbol      | `sym:isr_*`                |
| `from_sym:A to_sym:B` | PC in range `[A.addr, B.addr]`           | `from_sym:start to_sym:end`|

Symbols are point-like (= a single address). `sym:NAME` matches **only**
if `event.pc == NAME.addr` - for the range of a routine group use
`from_sym:A to_sym:B`.

Negation `!sym:isr_*` matches all events EXCEPT those whose PC is the
address of some `isr_*` symbol.

#### Cache and re-parse

The filter parser pre-resolves `sym:` names to concrete addresses on
Apply (= Enter in the filter textbox). If you later change the symbol
DB (= load another `.lbl`, add a user label), the cached addresses are
**stale** until you re-apply the filter (= click into the textbox +
Enter).

If `NAME` does not exist in the DB, the leaf matches no event (= the
filter effectively filters everything out).

### State-aware atoms

The keyword `if` introduces a state-aware atom (= distinguishes it from
`cat:`, `sym:` etc.). The filters test the HW state at the moment of
the event emit.

| Token             | Meaning                            | Example             |
|-------------------|------------------------------------|---------------------|
| `if iff1:N`       | CPU IFF1 state at the event moment | `if iff1:1`         |
| `if im:N`         | Z80 IM mode (0 / 1 / 2)            | `if im:2`           |
| `if reason:NAME`  | BP / IFF fire reason               | `if reason:int_ack` |
| `if banking:NAME` | Memory banking summary (per arch)  | `if banking:cgrom`  |

Reason values:

| Name      | Meaning                                  |
|-----------|------------------------------------------|
| `reset`   | CPU reset (= IFF cleared)                |
| `ei`      | EI instruction                           |
| `di`      | DI instruction                           |
| `int_ack` | INT acknowledgement (= IFF cleared)      |
| `nmi_ack` | NMI acknowledgement (= IFF1 cleared)     |
| `reti`    | RETI dispatch                            |
| `retn`    | RETN (IFF1 restored from IFF2)           |
| `none`    | No relevant reason                       |

Banking values (canonical + UX synonyms):

| Canonical name    | Synonyms                | MZ-800 meaning        |
|-------------------|-------------------------|-----------------------|
| `default`         | -                       | ROM+VRAM default      |
| `all_ram`         | `ram`                   | ROM_LOW + ROM_HIGH OFF |
| `rom_low_off`     | -                       | ROM low OFF (MZ-800)  |
| `rom_high_off`    | -                       | ROM high OFF (MZ-800) |
| `cgrom`           | -                       | CGROM visible         |
| `vram_640`        | `vram`, `vram_low`      | SCRW640 / PCG_1       |
| `pcg_high`        | `pcg_1`, `pcg_2`, `pcg_3` | MZ-1500 SPEC=2/3/4  |
| `other`           | -                       | Unknown configuration |

Lookup is case-insensitive. An unknown name = parse error.

Examples:

```
cat:irq_ack_im2 if iff1:1
   # IRQ ack events that occurred with IFF1 ON (= legitimate ack)
cat:cpu_int if reason:int_ack
   # CPU_INT events classified as INT acknowledgement
if banking:all_ram
   # everything that happened with full RAM mapping
( cat:gdg_colors if im:2 )
   # palette changes performed in IM2 mode
```

### Temporal atoms

A temporal node wraps a sub-expression and finds events in the ring
within a given pxclk window around each **reference** match of the
sub-expression.

| Token              | Meaning                                              | Example                            |
|--------------------|------------------------------------------------------|------------------------------------|
| `before(N) <expr>` | events in the last N pxclk before a match of `<expr>` | `before(1000) cat:irq_ack_im2`     |
| `after(M) <expr>`  | events in M pxclk after a match of `<expr>`          | `after(500) cat:cpu_ctrl sub:reti` |
| `near(K) <expr>`   | events in the window +-K pxclk around a match of `<expr>` | `near(200) sym:isr_main`           |

The arguments `N` / `M` / `K` are in pxclk units, suffixes:

| Suffix  | Multiplier |
|---------|------------|
| (none)  | x1         |
| `k`     | x1000      |
| `M`     | x10^6      |

Max nesting depth of temporal nodes = **2** levels (= `before(1000)
(cat:X AND sym:Y)` OK, deeper nesting = parse error).

**Note**: temporal filters require a stable ring snapshot. They are
available **only in the UI Log tab and Strip tab filters**. In
**pause-on-match and auto-mark trigger** filters a temporal node is
**disabled** (= the UI rejects it with a warning), because the
hot-path hook eval runs in the EMU thread without a stable snapshot of
the ring.

Examples:

```
before(2000) cat:irq_ack_im2
   # anything that happened in the last 2000 pxclk before an IM2 ack
near(500) sym:print_char
   # events in +-500 pxclk around calls to print_char
after(10k) cat:gdg_mode sub:mode_change
   # anything in 10000 pxclk after a DMD mode switch
before(1k) ( cat:cpu_int and if reason:int_ack )
   # context before each INT ack in the last 1000 pxclk
```

#### Temporal performance

A match with a temporal node iterates per event through the whole ring
scope - total N x R operations per render (R = number of reference
matches). For a typical scenario (N=50000, R~100) the render scan is
5-15 ms per frame. For extreme load (R=10000+) the UI feels a bit
jittery - recommendation: narrow the sub-expr filter to a specific
category.

### Examples

```
cat:cpu_int                              # CPU_INT events only
cat:cpu_int pc:4000-40FF                 # CPU_INT and PC in range (AND)
( cat:cpu_int or cat:bp_fire )           # CPU_INT or BP_FIRE
!cat:gdg_video                           # everything EXCEPT GDG_VIDEO (= "Hide noise")
frame:>100 cycle:>1M                     # after the 100th frame and 1M cycle
cat:iorq_out payload:0xCE                # OUT on the GDG MODE port
( cat:bp_fire or cat:user_mark ) sub:1   # BP fire (sub=MARK) or USER_MARK
```

### Quick filter presets in the toolbar

A ComboBox in the toolbar with **10 predefined filters** (= predefined,
click applies the expression into the Filter textbox). For the
user-defined list see [Saved filter presets](#saved-filter-presets).

| Group | Preset | Expression |
|---|---|---|
| HW Events | Only IRQ | `cat:cpu_int,cpu_pin_edge,irq_ack_im2` |
| | Only banking | `cat:gdg_banking,memext` |
| | Only video | `cat:gdg_mode,gdg_hwscroll,gdg_colors,gdg_video,gdg_wfrf` |
| | Only PSG | `cat:psg` |
| | Only FDC | `cat:fdc` |
| | Only memory | `cat:mmio_r,mmio_w` |
| | Only SYS | `cat:sys` |
| Code scope | Only marks/BPs | `cat:user_mark,bp_fire` |
| | ISR scope (PC 0038-00FF) | `pc:38-FF` |
| Hide/reset | Hide noise | `!cat:gdg_video` |
| | Clear filter | (empty) |

## Pause-on-match (stream BP)

In the toolbar there is a section **"Pause on match"** - the filter
expression acts as a stream-level breakpoint over the eventlog ring.
The first event that matches the filter pauses the emu (= `Pause`
button equivalent).

### Toolbar UI

```
[ ] Pause on match: [filter expression......................] [Test parse: OK]
                    Last match: frame=42 pxclk=18432   [Go to match]
```

- **Checkbox** on the left activates the trigger. Off = no overhead.
- **Filter expression textbox** - same syntax as the main filter.
- **Test parse badge** - "OK" / "Syntax error: ..." validates the
  filter without Apply. Green OK = filter ready.
- **Last match indicator** - if the trigger has already fired, shows
  `frame=N pxclk=M`. Click = scroll the Log to that event and select.

### Use cases

| Filter expression                       | What it pauses                            |
|-----------------------------------------|-------------------------------------------|
| `cat:gdg_colors`                        | first palette write (BORDER or PAL)       |
| `cat:bp_fire sub:0`                     | first classic BP halt (sub=HALT)          |
| `pc:38-FF`                              | first entry into the IM 1 ISR (vector 0x38) |
| `cat:psg payload:0x80`                  | first PSG latch on ch A period lo         |
| `cat:iorq_out payload:0xCE`             | first OUT on the GDG DMD port             |
| `sym:isr_*`                             | first PC in any ISR routine               |

### One-shot semantics

After the first match the trigger **automatically turns off** (= the
checkbox stays checked, but the internal gate is cleared). For another
match it has to be re-activated (= uncheck and re-check the checkbox,
or click "Re-arm").

The pause is async - the emu finishes up to the next "safe" point (=
end of the current instruction) and only then actually stops. It is not
noticeable in the UI (= the halt happens in tens of microseconds at
most).

## Auto-mark on match

The second toolbar section next to Pause-on-match. Instead of pausing
the emu it generates a synthetic **USER_MARK** event with a set name -
every matching event thus leaves a visible trace in the ring.

### Toolbar UI

```
[ ] Auto-mark on match: 'name......'  expr: [filter.....................]  Marker ID: 5
                                       Marked: 23                          [Clear]
```

- **Checkbox** activates the trigger. Off = no overhead.
- **Name textbox** - arbitrary string (typically a short 4-12
  characters). Shares the marker registry with the BP DSL action `mark
  "name"` - the same name = the same marker_id.
- **Filter expression** - as for pause-on-match.
- **Marker ID** - allocated ID after the first record (lazy register).
- **Counter** - how many USER_MARK events auto-mark has generated.
- **Clear** - reset the counter.

### Use cases

| Name        | Filter expression               | What it marks                  |
|-------------|---------------------------------|--------------------------------|
| `psg_w`     | `cat:psg`                       | every PSG write                |
| `bord_chg`  | `cat:gdg_colors sub:1`          | every BORDER change            |
| `dmd_chg`   | `cat:gdg_mode`                  | every DMD change (= mode switch)|
| `isr_ent`   | `cat:irq_ack_im2`               | every entry into the IM 2 ISR  |
| `mem_w`     | `cat:mmio_w pc:E000-FFFF`       | MMIO writes in the upper bank  |

The marker_id is stable per name across the session - repeated
enabling of auto-mark with the same name uses the same ID.

### Synthetic USER_MARK events

Generated events have:

- `category = USER_MARK` (= 19)
- `subtype = 0` (= regular mark)
- `payload` = encoded marker_id
- `pc` = original `pc` of the trigger event (= where it happened)
- `frame` / `pxclk` = original timestamp

### No infinite loop

The auto-mark callback explicitly skips events with `category ==
USER_MARK` - its own synthetic marks therefore do not trigger another
auto-mark (= one-way flow).

## Detail column decoders

The Detail column shows human-readable text formatted per category.

| Category     | Detail example                                        |
|--------------|-------------------------------------------------------|
| CPU_INT      | `IM=2 IFF1=1 IFF2=1 RETI`                             |
| CPU_PIN_EDGE | `PIOZ80@PA4 rising`                                   |
| IRQ_ACK_IM2  | `PIOZ80_PA vec=0x40 isr=0x4042`                       |
| IORQ_IN/OUT  | `port=0xCE (GDG DMD) val=0x08`                        |
| MMIO_R / W   | `addr=0xE003 val=0x80`                                |
| GDG_MODE     | `DMD=0x08 (MZ-700 mode)`                               |
| GDG_BANKING  | `port 0xE0 (ROM bottom OFF)`                          |
| GDG_HWSCROLL | `SOF=0x4000` / `WID=80` / ...                         |
| GDG_COLORS   | `BORDER=0xE0` / `PAL[2]=0x9F`                         |
| GDG_VIDEO    | `VBLN start` (= subtype label only)                   |
| GDG_WFRF     | `WF=0x08` / `RF=0xFF`                                 |
| PIO8255      | `Port A=0x42` / `CW=0x80 (mode 0)`                    |
| CTC8253      | `CW: counter 2 mode 3 BIN` / `CTC1=0x12`              |
| PIOZ80       | `A MODE 2 (bidir)` / `A ICW 0xB7 ...`                 |
| PSG          | `0x80 (latch ch A period lo)`                         |
| FDC REG_W    | `reg=0 (CMD)=0x80 (Restore)`                          |
| FDC CMD      | `Read Sector side=0 T=10 S=5`                         |
| MEMEXT       | `page=4 bank=0x12 (Luftner)`                          |
| QD           | `reg=2 (ctrl A)=0x18`                                 |
| RD           | `port=0xFA val=0x42`                                  |
| BP_FIRE      | `BP #5 reason=2 MARK`                                 |
| USER_MARK    | `"isr_entry"`                                         |
| CPU_CTRL     | `HALT enter` / `RST 0x38`                             |
| SYS          | `Snapshot save: hash=0xNNNNNNNN`                      |

The decoder does not drop any information from the 24 B event - the
view does not depend on read order or the current emu state. The
tooltip over the cell shows the same text (= no additional information
in the tooltip).

## Sub column short codes

The `Sub` column shows a **short code** (max 8 visible characters) per
(category, subtype). Hover over the cell shows the **full description**
in the tooltip.

### Examples of short codes

| Category     | Subtype           | Short code   | Tooltip (full)              |
|--------------|-------------------|--------------|-----------------------------|
| CPU_CTRL     | HALT_ENTER (0)    | `HALT_E`     | `HALT entry`                |
| CPU_CTRL     | HALT_EXIT (1)     | `HALT_X`     | `HALT exit (IRQ/NMI)`       |
| CPU_CTRL     | RST_00 (2)        | `RST_00`     | `RST 0x00`                  |
| CPU_CTRL     | RST_38 (9)        | `RST_38`     | `RST 0x38`                  |
| BP_FIRE      | HALT (0)          | `HALT`       | `BP fire - halt`            |
| BP_FIRE      | MARK (1)          | `MARK`       | `BP fire - mark action`     |
| BP_FIRE      | CONTINUE (2)      | `CONT`       | `BP fire - continue`        |
| BP_FIRE      | ENABLE (4)        | `ENABLE`     | `BP fire - enable target`   |
| BP_FIRE      | DISABLE (5)       | `DISABL`     | `BP fire - disable target`  |
| IORQ_IN/OUT  | NORMAL (0)        | `NORMAL`     | `Mapped port`               |
| IORQ_IN/OUT  | UNCONNECTED (1)   | `UNCONN`     | `Unmapped port`             |
| GDG_COLORS   | BORDER (1)        | `BORDER`     | `Border color write`        |
| GDG_COLORS   | PALGRP (2)        | `PALGRP`     | `Palette group write`       |
| GDG_COLORS   | PAL (3)           | `PAL`        | `Palette register write`    |
| GDG_COLORS   | PCG (4)           | `PCG`        | `PCG color write (MZ-1500)` |
| GDG_VIDEO    | VBLN_START (1)    | `VBLNs`      | `Vertical blanking start`   |
| GDG_VIDEO    | VBLN_END (2)      | `VBLNe`      | `Vertical blanking end`     |
| GDG_VIDEO    | HBLN_START (5)    | `HBLNs`      | `Horizontal blanking start` |
| PIO8255      | PORT_A (1)        | `PORT_A`     | `Port A write`              |
| PIO8255      | PORT_B (2)        | `PORT_B`     | `Port B write`              |
| PIO8255      | CONTROL_W (4)     | `CTL_W`      | `Control register write`    |
| CTC8253      | CONTROL_WRITE (1) | `CTL_W`      | `Control register write`    |
| CTC8253      | COUNTER_WRITE (2) | `CNT_W`      | `Counter register write`    |
| PIOZ80       | MODE_W (1)        | `MODE_W`     | `Mode register write`       |
| PIOZ80       | VECTOR_W (2)      | `VEC_W`      | `Vector register write`     |
| PIOZ80       | ICW_W (3)         | `ICW_W`      | `Interrupt control write`   |
| FDC          | REGISTER_WRITE (1) | `REG_W`     | `WD279x FDC register write` |
| FDC          | COMMAND_ISSUED (2) | `CMD`       | `WD279x command dispatch`   |
| SYS          | COLD_RESET (0)    | `COLD_RST`  | `Cold reset`                |
| SYS          | SNAPSHOT_SAVE (2) | `SNAP_SAV`  | `Snapshot save`             |
| SYS          | SNAPSHOT_LOAD (3) | `SNAP_LD`   | `Snapshot load`             |
| SYS          | MZF_INJECT (4)    | `MZF_IN`    | `MZF inject`                |

For an unknown category+subtype combination the fallback is the
decimal form (= `"42"`). The tooltip then returns the
`"short (subtype N)"` fallback string.

## Group by in the Log tab

In the Log tab toolbar there is a dropdown **"Group by"** with the
values:

| Value      | Meaning                                              |
|------------|------------------------------------------------------|
| `None`     | Chronological table (= default)                      |
| `Frame`    | Per-frame groups (= group key `screens_total`)       |
| `Category` | Per-category groups (= key `category`)               |
| `PC`       | Per-PC groups (= key `pc`)                           |

For a non-None value the Log render switches to a **CollapsingHeader
per group** with its own mini-table inside. The header text contains
the group key + the number of events (e.g. `Frame 142 (45 events)`).

In the mini-table inside the group the **column corresponding to the
group key is omitted** (= duplicated by the header text):

- `Frame` group -> hide the **Frame** column
- `Category` group -> hide the **Cat** column
- `PC` group -> hide the **PC** column

Clicking a row works the same as in None mode (= pause + jump into
disasm). The filter expression is applied **before** grouping.

The dropdown state is persisted in cfg `[EVENT_VIEWER_WINDOW] group_by
= 0..3`.

**Performance**: O(N) pre-pass + render. For N=50000 and a typical
number of groups (300 for Frame, 24 for Category) the render is ~5-8
ms per frame. Group by PC with a chaotic trace (G=10000+) may feel
slower (8-15 ms).

## Per-frame heatmap

The toggle **"Heatmap"** in the toolbar displays a mini histogram of
event distribution within the frame above the Log table. Default OFF.

- **64 bins** evenly spread over the pxclk domain of the frame (=
  per-arch frame width)
- Bar height = total number of events in the bin (= self-normalizing,
  max bin = full height)
- Color stack per category (= reuse of the Strip color picker colors)
- Hover tooltip on a bar shows the bin range + top categories in the
  bin (e.g. `Bin 23 (pxclk 25800..26900): total 145 events, CPU_INT:80
  IORQ_OUT:45 ...`)
- **Click a bin** = scroll the Log table to the first event in that bin

The filter expression is applied **before** binning - the heatmap
shows the distribution of filtered events.

The toggle state is persisted in cfg `[EVENT_VIEWER_WINDOW] heatmap =
0..1`.

## Export / Import of the ring

For post-mortem analysis the current contents of the ring can be
exported into a binary file and re-imported later (= "replay").

### UI toolbar

In the toolbar of the Events window, between **Capacity Clear** and
**Follow tail**, there are two buttons:

- **Export** opens a modal popup with a text input for filename
  (default `eventlog_dump.evlog`) + a Confirm button. After Confirm the
  ring is written to the file.
- **Import** opens a modal popup with a text input for filename +
  Confirm. After Confirm a **Confirm dialog** "Replace current ring?"
  is shown - on confirmation the ring is cleared and data is loaded
  from the file.

For a deterministic export it is recommended to:

1. Pause the emu (Settings -> Pause) or switch Mode -> OFF
2. Only then Export

(= otherwise the emu may write to the ring in parallel with the export
and the last event may be lost or extra.)

### CLI replay

From the command line the ring can be imported at emu startup via the
flag `--eventlog-replay`:

```
mz800emu --eventlog-replay /path/to/dump.evlog
```

The file is loaded after the debugger subsystem is initialized, and the
ring then holds the contents of the dump. The UI Log tab immediately
shows the imported events once the window is opened.

### Binary format

Header 32 B (magic "MZEVTLOG" + version + record_size + record_count +
unix timestamp) + a sequence of records (= 32 B each).

**Endianness**: native (= little-endian on the supported platforms
Win MSYS2 / Linux x86_64 / ARM64). Files are NOT portable between
big-endian and little-endian systems.

**Capacity auto-resize**: if the file contains more records than the
current ring capacity, the import grows the capacity to
max(record_count, MIN_CAPACITY), clamped to MAX_CAPACITY = 200000.
Records above the limit are discarded (= warning on stderr, not
error).

## FDC command tracking

The FDC category emits **a pair of events** at the same pxclk position
on every write to a WD279x command/status register:

1. **REG_W** subtype = raw BUS byte write (= before Sharp inversion).
   The Detail string shows `reg=0 (CMD)=0x80 (Read Sector)`. Useful for
   verifying the physical byte level on the bus.
2. **CMD** subtype = **decoded command** with the current head position
   (Track / Sector register before dispatch). The Detail string shows
   e.g. `Read Sector side=0 T=10 S=5`.

A write to other FDC registers (Track / Sector / Data) emits only
REG_W.

The filter `cat:fdc sub:cmd` shows decoded commands only. The filter
`cat:fdc sub:reg_w` shows raw writes only.

The Strip tab tooltip contains the full subtype text (= reuse of the
same strings shown by the Log tab hover on the Sub cell). The tooltip
then shows e.g.:

```
frame=42 pxclk=18432
sline=128 px=160
CTC8253 / CTL_W (Control register write)
CW: counter 2 mode 3 BIN
```

## SYS category

The category `cat:sys` captures emulator lifecycle events that do not
belong to any HW subsystem.

| Subtype           | Short code  | Detail string                       |
|-------------------|-------------|-------------------------------------|
| COLD_RESET        | `COLD_RST`  | `Cold reset`                        |
| SNAPSHOT_SAVE     | `SNAP_SAV`  | `Snapshot save: hash=0xNNNNNNNN`    |
| SNAPSHOT_LOAD     | `SNAP_LD`   | `Snapshot load: hash=0xNNNNNNNN`    |
| MZF_INJECT        | `MZF_IN`    | `MZF inject: hash=0xNNNNNNNN`       |

The filename hash is djb2 over the **basename** (= after the last `/`
or `\`), not the full path. The same file from a different directory
yields the same hash.

The **quick filter preset** "Only SYS" is in the Strip Quick Filter
dropdown. The Strip render color = gray-gold for contrast against the
other categories. SYS markers are rare (= 1-10 per session) and are
drawn as thicker vertical bars for visibility.

## Interaction - click and context menu

### Single click on a row

Marks the row (highlight). No other action.

### Double click on a row

**Pause emu + show in disasm** at the event PC. Key online UX: "I see
something strange -> click -> I see it in the code".

### Right click on a row

Context menu:

- **Pause and show in disasm** - identical to double click
- **Pause here** - just pause, no jump
- **Show in disasm** - jump without pause
- **Show port in Overview** (only for IORQ_IN / IORQ_OUT / MMIO_R /
  MMIO_W) - opens the I/O Ports Overview tab + sets the filter to the
  specific port
- **Copy as text** - clipboard

## Follow tail

Toolbar toggle. When enabled, the table automatically scrolls to the
newest row (= "live tail" like `tail -f`).

**Interaction with click-pause:**

- Manual scrolling by the user (= scrollbar drag, mouse wheel)
  temporarily suppresses auto-scroll until the next render at which the
  table is at the bottom.
- Click + pause emu preserves the scroll position - after resume
  auto-scroll runs again.

Default = ON. Persisted in cfg `[EVENT_VIEWER_WINDOW] follow_tail`.

## Persistence (cfg section)

### `[EVENT_LOG]`

| Key                | Default        | Meaning                            |
|--------------------|----------------|------------------------------------|
| `mode`             | `OFF`          | OFF / WHEN_WINDOW_OPEN / ALWAYS    |
| `capacity`         | 50000          | Ring size (clamp 10000..200000)    |
| `categories_mask`  | `0x00FFFFFF`   | Bit per category (= 24 ON)         |

### `[EVENT_VIEWER_WINDOW]`

| Key                 | Default | Meaning                                       |
|---------------------|---------|-----------------------------------------------|
| `window_open`       | 0       | Events window visibility                      |
| `filter_expression` | `""`    | Last filter in the textbox                    |
| `follow_tail`       | 1       | Auto-scroll to the newest                     |
| `group_by`          | 0       | Group by (0=None / 1=Frame / 2=Cat / 3=PC)    |
| `heatmap`           | 0       | Heatmap above the Log table (0=OFF / 1=ON)    |
| `row_coloring`      | 1       | Colored row background by category            |

### `[EVENT_LOG_FILTERS]`

Slots `preset_NN_name` + `preset_NN_expr` (NN = 00..31), empty name =
slot ignored. See [Saved filter presets](#saved-filter-presets).

## Related panels

- **I/O Ports** ([io-ports.md](io-ports.md)) - the History tab in I/O
  Ports has its own in-memory ring only for IORQ + MMIO events
  (~10000 events, 20 B per event), with its own filter and older than
  the Event Viewer. For a real-time view across all categories (=
  including CPU_INT, GDG, BP fire) use the Events window. For a
  narrowly port-focused workflow with an "Add IORQ R/W BP" action use
  the I/O Ports History tab.

- **Trace Suite** ([Trace_Suite.md](Trace_Suite.md)) - post-mortem
  file log (cputrack / iorqlog / intlog / hwlog / marklog). The
  eventlog ring shares the 24 B per-event layout (= cross-mergeable
  timestamps with hwlog/intlog chunks).

- **Breakpoints** ([breakpoints/README.md](breakpoints/README.md)) -
  every BP firing generates a `BP_FIRE` event in the Event Viewer,
  including the action classification (HALT / MARK / CONTINUE / ENABLE
  / DISABLE). Key for "when is my BP firing" debugging.

- **Disassembly** ([disassembly.md](disassembly.md)) - jump target
  from the Events window (= double click / context menu "Show in
  disasm").

- **Stack window**, **CPU window**, **Memory map** - related debug
  views without direct interaction with the Events window.
