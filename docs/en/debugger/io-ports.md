# I/O Ports panel

The I/O Ports panel is a structured view of all documented I/O ports of
Sharp MZ-800 / MZ-700 / MZ-1500, inspired by the no$gba IO Map. It shows
bit-by-bit register descriptions, live values, activity tracking ("heat
map") and IORQ history.

## Contents

- [Panel purpose](#panel-purpose)
- [Architecture](#architecture)
- [Naming convention](#naming-convention)
- [Data sources](#data-sources)
- [0xCF 16-bit CRTC family](#0xcf-16-bit-crtc-family)
- [Banking decoded view](#banking-decoded-view)
- [Cross-window navigation](#cross-window-navigation)
- [Activity tracking](#activity-tracking)
- [History ring buffer](#history-ring-buffer)
- [History tab UI](#history-tab-ui)
- [Filter syntax](#filter-syntax)
- [Memory-mapped I/O 0xE000-0xE008](#memory-mapped-io-0xe000-0xe008)
- [Persistence (cfg section)](#persistence-cfg-section)
- [Use cases](#use-cases)
- [Troubleshooting](#troubleshooting)
- [Related panels](#related-panels)

## Panel purpose

The panel serves the debugger to:

1. **Real-time view** of HW peripheral state (GDG, 8255 PPI, 8253 CTC,
   Z80 PIO, FDC, banking, PSG, joystick) without having to browse the
   emulator internal state in a hex viewer.
2. **Bit-by-bit decoding** of each register - no raw byte values without
   context.
3. **Heat map** of activity (= which ports the CPU is currently
   "tickling") via a 1 s sliding window.
4. **History** - chronological list of IN/OUT/MR/MW events with a filter
   by PC, port, frame, cycle, value, address.
5. **Quick Add BP** - right-click on a port generates a smart
   breakpoint:
   - IORQ entry -> `IORQ_R` / `IORQ_W` (pre-fills the `port` field)
   - MMIO entry (0xE000-0xE008) -> `MEM_R` / `MEM_W` (pre-fills the
     `addr` field)

## Architecture

Tab system inside the panel (Overview default, History tab for
chronology):

```
+-- I/O Ports ---------------------------------------------+
| [Overview] [History]                                     |
+----------------------------------------------------------+
| Sticky header:                                           |
|   Filter:[___] [Clear]   (visible/total)                 |
|   [v] Track | [Reset Activity] | Capacity:[10000v]       |
|   [v] Auto-follow | [Latest] | [Clear history]           |
+----------------------------------------------------------+
| Section: GDG                                  [v]        |
|   Addr   | Name             | Hex | Bin    | R/W | Rec | Activ |
|   0xCC   | GDG - WF (W)     | ??  | --     | W   |  v  |   0/s |
|   0xCD   | GDG - RF (W)     | ??  | --     | W   |  v  |   0/s |
|   0xCE   | GDG - DMD (W)    | 0E  | 000... | W   |  v  |   2/s |
|   0xCE   | GDG - Status (R) | 80  | 100... | R   |  v  |   1/s |
|   ...                                                    |
| Section: 8255 PPI                              [v]       |
|   ...                                                    |
+----------------------------------------------------------+
```

The sticky header follows the Variables panel pattern: no `(?)` markers,
tooltips on hover. Buttons are inline (fewer clicks compared to a modal
dialog).

The Hex column shows the current value of the port. If the port has no
readable mirror and has not yet captured any IORQ event, `??` is shown
with the tooltip "No data captured". If the value comes from the cache
and is older than 500 frames (~10 s at 50 Hz), it is shown dimmed
(TextDisabled) with the tooltip "Last cached value N frames ago".

**Rec column:** A per-port checkbox in Overview serves as a selective
recording mask - unchecking suppresses writing IORQ on that port to the
History ring buffer. The mask has a range of 256 (IORQ low byte). The
default is to record everything. Usage: log only the events being
watched (one specific port, the rest off) without a flood from
high-frequency ports (keyboard, status polling).

## Naming convention

Each port in the catalog follows the format:

```
<chip> - <function> (<dir>)
```

where `<dir>` is in {`R`, `W`, `R/W`}.

**Examples:**

| Category | Example name |
|----------|--------------|
| GDG | `GDG - WF (W)`, `GDG - DMD (W)`, `GDG - Status (R)` |
| 8255 PPI | `8255 PPI - Port A (R/W)`, `8255 PPI - Control (W)` |
| 8253 CTC | `8253 CTC - Counter 0 (R/W)`, `8253 CTC - Control (W)` |
| Z80 PIO | `Z80 PIO - Port A data (R/W)` |
| FDC | `FDC - Status / Command (R/W)` |
| PSG (MZ-800) | `PSG - SN76489 (W)`, `PSG - Stereo left (W)` |
| PSG stereo (MZ-1500) | `PSG stereo - L Latch (W)`, `PSG stereo - R Latch (W)` (separate "PSG (stereo SN76489 x2)" section in Overview) |
| Memory banking | `Memory bank - E0 (R/W)` .. `Memory bank - E6 (R/W)` (Sharp banking via GDG dispatch) |
| Memory expansion | `Memory ext - MEMEXT bank (R/W)` (port 0xE7, separate MemExt card under Sharp banking) |
| Joystick | `JOY0 (R)`, `JOY1 (R)` |
| MZ-700 mmio | `MZ-700 mmio - PPI Port A (R/W)`, `MZ-700 mmio - GDG DMD/Status (R/W)` |
| CMT hack | `CMT hack - Load file (W)`, `CMT hack - Read MZF body (W)` |
| Unicard | `Unicard - CMD (R/W)`, `Unicard - DATA (R/W)` |
| Ramdisk | `Ramdisk - Pezik 68 bank 0 (R/W)`, `Ramdisk - Std read (R)` |
| IDE8 | `IDE8 - Data (R/W)`, `IDE8 - Status / Command (R/W)` |
| QDISK | `QDISK - SIO Data A (R/W)`, `QDISK - SIO Ctrl B (R/W)` |

**Multi-entry per address:** If IORQ IN and IORQ OUT at the same address
have a different meaning, the catalog has two separate entries:

| Addr | OUT entry | IN entry |
|------|-----------|----------|
| 0xCE | `GDG - DMD (W)` | `GDG - Status (R)` |
| 0xF0 | `GDG - Palette (W)` | `JOY0 (R)` |

The UI shows them as two rows with separate bit descriptions and
direction tags.

## Data sources

The panel combines three information sources:

1. **Static catalog** (compile-time):
   - Address, name, direction, bit-by-bit descriptions, decode helpers,
     architecture (MZ-700 / MZ-800 / MZ-1500 / ALL).

2. **Live emulator state** (runtime):
   - For each port the value is read directly from the emulator's
     internal structures **side-effect free**, never through the IORQ
     dispatch (= side effect on the chip latch).
   - For write-only ports (WF/RF) the emulator keeps a mirror in an
     internal structure - that is what we read.
   - If a port has no readable mirror (truly write-only HW without
     internal storage), the UI falls back to the last cached IORQ value
     (per R/W direction) captured by activity. If even the cache has no
     data, `??` is shown with a tooltip.
   - **Last-write cache pattern** (write-only sequencer registers): for
     the PPI/CTC/PIO Control Word a non-destructive read from the chip
     is impossible, but the debugger holds a cache that captures the
     last CPU write. The Hex column in Overview thus shows the current
     contents of the Control Word for these registers as well.

3. **Dynamic activity / history** (runtime, gated):
   - Sliding window hits/s per port.
   - Ring buffer of N IORQ + MMIO events.
   - Hot-path hook gated by an internal flag - panel closed = zero
     overhead.

## 0xCF 16-bit CRTC family

The GDG has a group of **16-bit IORQ ports** with bus address `0xRRCF`:

```asm
; set BCOL to black
XOR  A
LD   BC, 06CFh    ; B=06h (register index), C=0xCF
OUT  (C), A
```

Z80 OUT (C),A pattern: B = register index, C = port = 0xCF.
**MZ notation convention:** `0xCF<RR>` (= human-readable).

Sub-registers (all W-only):

| MZ notation | B reg | Meaning |
|-------------|-------|---------|
| 0xCF01 | 01 | SOF0 (scroll offset low 8 bits) |
| 0xCF02 | 02 | SOF1 (scroll offset upper 2 bits, bits 0-1) |
| 0xCF03 | 03 | SW (scroll width) |
| 0xCF04 | 04 | SSA (scroll start address) |
| 0xCF05 | 05 | SEA (scroll end address) |
| 0xCF06 | 06 | BCOL (border color, 4 bits) |
| 0xCF07 | 07 | CKSW (Superimpose enable, bit 7) |

**UI display:**
- The "Addr" column shows the MZ notation `0xCF01`..`0xCF07`.
- The tooltip describes the bus address `0xRRCF`.
- A read mirror is available for SOF0/SOF1, SW, SSA, SEA and BCOL. There
  is no mirror for CKSW; the current state of the CKSW signal can be
  read in the 0xCE Status register, bit 2.

**IORQ_W BP integration:** When triggering "Add IORQ W BP" from a
0xCF<RR> entry, the UI automatically sets `port_mode = BP_PORT_16BIT`.
The default 8-bit mode would match all 7 0xCF<RR> ports (= only the low
byte 0xCF).

**MZ-1500 does not support** the 0xCF family or HW scroll (= available
only on MZ-800).

## Banking decoded view

The banking ports 0xE0-0xE6 in the Overview tab show the decoded Sharp
banking state. The actual banking state does not live in the IORQ port
(= write-only dispatch to GDG), but in a memory map byte (per-arch
bitfield). MZ-800 additionally depends on GDG DMD bit 3 (700 compat vs
800 native mode).

All 7 entries 0xE0-0xE6 share the same hex value (= global state, not
per-port; similar to WD279x, where 4 different ports show different
registers of the same chip), plus per-arch flag descriptions.

**Per-arch bit descriptions:**

| Arch | Flag decode |
|------|-------------|
| MZ-800 | 5 flags: `ROM_0000`, `ROM_1000`, `CGRAM_VRAM`, `ROM_E000`, `PROHIBITED`. Bits 2-3 have a meaning dependent on DMD bit 3 - the description covers both modes (700/800). |
| MZ-700 | 3 flags: `ROM_0000`, `ROM_E000`, `PROHIBITED` |
| MZ-1500 | 2 flags + 3-bit `SPEC` field: `ROM_0000`, `ROM_UPPER`, `SPEC` (0=None, 1=CGROM, 2=PCG1, 3=PCG2, 4=PCG3 in D000-EFFF). The UI hint translates `SPEC` to a symbolic name. |

**Naming split:** The Overview section is split into **"Memory
banking"** (ports 0xE0-0xE6 = Sharp banking via GDG dispatch) and
**"Memory expansion (MemExt)"** (port 0xE7 = memory extension 64 kB ->
512 kB SRAM, a separate HW card under Sharp banking).

## Cross-window navigation

The banking entries 0xE0-0xE6 + MemExt 0xE7 have an item **"Show in
Memory Map"** in the Overview row right-click menu, which opens and
focuses the Memory Map debug window.

The reason: a per-port MemExt 0xE7 mirror in Overview does not make
sense (16-byte multi-page state); the Memory Map window already fully
displays the MemExt column per 4 kB page, so a cross-window click is
the right UX solution for the Sharp banking ports as well.

See [memory-map.md](memory-map.md) for details of the Memory Map
window.

## Activity tracking

Per port (full 16-bit IORQ address) sliding window:

- **Bucket array:** 60 buckets, 1 bucket = 1 emulation frame (~20 ms at
  50 Hz).
- **Window size:** last 50 buckets ~ 1 s.
- **Advance:** bucket shift happens at the end of each frame.
- **Reset:** "Reset Activity" in the sticky header (all ports) or
  per-port via the right-click context menu.

The "Activity" UI column shows `<N> hits/s` plus row coloring per heat
map thresholds. Two thresholds are configurable via cfg keys in the
`[IO_PORTS_PANEL]` section:

| Cfg key | Default | Meaning |
|---------|---------|---------|
| `heat_text_active` | 1 | hits/s threshold for green text on an active row (= "the port is not dead") |
| `heat_bg_hot` | 10000 | hits/s threshold for a red bg tint on a bottleneck row (= "hot port, take a look") |

UI: inline **Heat...** popup in the toolbar (2x `InputInt` + Reset to
defaults), clamped to the allowed cfg value range.

**Counters per direction:** Activity is split into IN counter and OUT
counter. For a port like 0xF0 (W = GDG Palette, R = JOY0) the Overview
shows activity separately on the W row and the R row - so writes to
Palette do not increase the activity on the JOY0 R row and vice versa.

**Performance:** The hot-path hook is gated by an internal tracking
flag. The default is on, but gated on the panel being open - panel
closed = a single flag check + branch predictor = zero overhead. With
the panel open = array index + uint16 saturated increment.

## History ring buffer

Each IORQ + MMIO event record contains: frame, scanline, pixel column,
CPU cycle, PC, port (or MMIO address), value, flags (READ / MEMORY).
Record size is 20 bytes.

**Capacity:** default 10000 events (~200 KB). The sticky header
`Capacity:` dropdown offers values 1000 / 5000 / 10000 / 25000 / 50000.
A change reallocates the buffer and resets history.

**Ring layout:**
- `head` - next write index (modulo capacity).
- `count` - capped at capacity.
- `overflow` - flag after the first wrap-around.
- Logical indexing from 0 = oldest, count-1 = newest.

**Hook:** gated by the same tracking flag as activity.

## History tab UI

**Layout:**

```
+-- I/O Ports (History tab) ------------------------------+
| [Filter...] [Clear] (visible/total) | [Latest]         |
| [Auto-follow] | [Clear history]                        |
| Filter error: <msg>                       (red,         |
|                                            only on parse|
|                                            fail)        |
+----------------------------------------------------------+
| Frame | Scanline | px | CPU Cycle | PC | Type | Port | Addr | Value | Description |
|-------|----------|----|-----------|----|------|------|------|-------|-------------|
| 1234  |   128    | 42 | 4567890   | 0x4042 | OUT | 0xCE | -   | 0x02 | GDG DMD: 320x200x16 |
| 1234  |   130    | 50 | 4567920   | 0x4055 | OUT | 0xCF06 | - | 0x05 | GDG BCOL = 5 |
| 1235  |    50    | 30 | 4571200   | 0x40A0 | MR  | -    | 0xE001 | 0xC2 | MMIO PPI Port B read |
+----------------------------------------------------------+
| Selected event:                                          |
|   Frame: 1235, Scanline: 50, PC: 0x40A0                  |
|   Port: 0xCE (GDG - Status (R)), Type: IN, Value: 0x80   |
+----------------------------------------------------------+
```

**Table columns:**

| Column | Meaning |
|--------|---------|
| Frame | Emulation frame number at the moment of recording |
| Scanline | Beam row 0..311 at the moment of recording |
| px | Pixel column within the scanline |
| CPU Cycle | Cumulative T-state CPU cycle |
| PC | Instruction address (clickable - see below) |
| Type | `IN` / `OUT` (IORQ) or `MR` / `MW` (MMIO) - 4-color row tinting |
| Port | IORQ MZ notation (`0xCE`, `0xCF06`); MMIO event = `-` |
| Addr | MMIO addr (`0xE001`); IORQ event = `-` |
| Value | `0x%02X` |
| Description | Smart per-port decode + fallback to catalog name |

**Row colors (4-color tinting):**

- IN  = light green text  - IORQ read
- OUT = light orange text - IORQ write
- MR  = light cyan text   - MMIO read
- MW  = light yellow text - MMIO write

Applied to the whole row (all 10 columns). The color is determined by
the combination of flags (READ, MEMORY).

**Description column:**

A smart per-port decoder returns a short human-readable description of
the event. For the most important ports (banking E0-E7, GDG
DMD/BCOL/palette, PPI keyboard, CTC, FDC, PSG, Z80 PIO, joystick) it
offers a value-decoded description. For ports without an explicit
decoder the fallback is the catalog name.

IORQ examples:
- 0xCE OUT 0x02 -> "GDG DMD: 320x200x16"
- 0xCF06 OUT 0x05 -> "GDG BCOL = 5"
- 0xE0 OUT -> "MEM: unmap ROM 0000 + ROM 1000"
- 0xE7 OUT 0x03 -> "MEM: MEMEXT bank 3"
- 0xD0 IN -> "PPI Port A (keyboard col read)"
- 0xF7 OUT -> "Z80 SIO (Quick Disk)"

MMIO examples:
- MR 0xE001 -> "MMIO PPI Port B read"
- MW 0xE008 0x0E -> "MMIO CTC0 GATE0 = 0 (audio gate)"
- MR 0xE004 -> "MMIO CTC counter 0 (audio)"

Table headers are hardcoded in English (= consistency with the other
debugger panels).

**Clickable PC in the Selected Event panel:**

The `PC: 0xXXXX` in the first row of the detail panel is rendered as a
button. Actions:

- **LMB click** -> focus into Disassembly #1 (primary), see
  [disassembly.md](disassembly.md). Opens the debug window if closed +
  auto-disables Follow PC if running.
- **RMB click** -> popup with items:

  1. **Focus to... >** submenu with 5 items "Disassembly #1..#5"
  2. **Add to bookmarks** - adds a bookmark for the PC address (symbol
     name if it exists, otherwise `#XXXX` hex; comment empty; opens the
     Bookmarks window)
  3. **Add breakpoint** - creates a PC_EXEC BP at PC

Hover tooltip: "Focus to primary disassembly. Right-click for additional
actions."

**Click row = highlight + detail panel:**

Clicking a row in the table:

1. Marks the row visually.
2. Shows the detail panel below the table.
3. Pauses auto-follow (= the user wants to inspect, no jump to a new
   event).

Detail panel for an IORQ event:

```
Frame: 130135  Scanline: 106  px: 42  Cycle: 4567890  PC: 0x2BCF
Port: 0xCE  Type: IN  Value: 0x70
Description: GDG Status (VBLN/HBLN/VSY/HSY)
```

Detail panel for an MMIO event:

```
Frame: 130135  Scanline: 106  px: 42  Cycle: 4567890  PC: 0x2BCF
Addr: 0xE001  Type: MR  Value: 0xC2
Description: MMIO PPI Port B read
```

Port name lookup uses multi-entry handling (= correctly distinguishes
0xCE IN "Status" from 0xCE OUT "DMD").

**Selected event panel - splitter:**

Between the table and the Selected event panel there is a horizontal
splitter. The user can drag the splitter vertically for arbitrary
resize 60..400 px. The height is persisted in the cfg key
`detail_panel_height`.

**Auto-follow:**

- Default ON.
- Checkbox + button `[Latest]` are in the History tab sticky header.
- With auto-follow on, the table scrolls to the last row.
- Detection of user scroll up: if the scroll position is more than 32
  px above the bottom, it is automatically disabled and auto-scroll is
  not invoked (= user action is respected).
- The `[Latest]` button re-enables + immediate scroll down
  (unconditionally, even if the user was above).

**Clear history:** the sticky-header button discards all events.

## Filter syntax

The filter input in the History tab supports a **boolean expression**
over atomic tokens (= leaves): AND / OR / NOT + parentheses.

### Grammar and operators

```
expr     = or_expr
or_expr  = and_expr  ( ( "|" | "OR" ) and_expr )*
and_expr = unary     ( ( "&" | "AND" | WS )  unary )*
unary    = "!" unary | atom
atom     = "(" expr ")" | leaf_token
```

Operators in descending precedence:

| Operator | Meaning | Note |
|----------|---------|------|
| `(` `)` | grouping | highest precedence |
| `!` | unary NOT | over a leaf token or over a parenthesized expression |
| WS, `&`, `AND` | implicit / explicit AND | `AND` is a case-sensitive uppercase keyword |
| `\|`, `OR` | OR | lowest precedence; `OR` is a case-sensitive uppercase keyword |

**Keyword case sensitivity:** `AND` and `OR` are reserved words only
when written in **uppercase letters**. Lowercase `and` / `or` (or
`And`, `oR`, ...) is interpreted as a plain-text name match - this
preserves backward compatibility for ports with "OR" in the name. The
symbol variants `&` and `|` are separate characters and have no case
sensitivity.

The filter `port:CE pc:4042` is internally parsed identically to
`port:CE & pc:4042` or `port:CE AND pc:4042`. The per-token `!` prefix
(= `!port:CE`) is valid and is internally collapsed into a leaf node
with the negate flag.

### Atomic tokens (= leaves in the boolean expression)

| Filter | Meaning |
|--------|---------|
| `port:CE` | **low byte match** - `(event.port & 0xFF) == 0xCE`, ignores random high byte (8-bit IORQ) |
| `port:C0-CF` | low byte range 0xC0..0xCF (inclusive) |
| `port16:00CE` | **full 16-bit match** - `event.port == 0x00CE` exactly |
| `port16:CF06` | full 16-bit 0xCF06 (= 0xCF family sub-register) |
| `port16:CF00-CF0F` | full 16-bit range |
| `pc:4042` | PC == 0x4042 |
| `pc:4000-40FF` | PC range 0x4000..0x40FF |
| `frame:>100` | frame > 100 |
| `frame:<100` | frame < 100 |
| `frame:50-150` | frame range 50..150 |
| `frame:42` | frame == 42 |
| `value:42` | value == 0x42 |
| `value:00-7F` | value range 0x00..0x7F |
| `cycle:>1000000` | cumulative T-state cycle > 1M |
| `cycle:<500000` | cycle < 500000 |
| `cycle:1000-2000` | cycle range |
| `addr:E008` | MMIO events with addr 0xE008 only |
| `addr:E000-E008` | MMIO addr range |
| `in` | IN (CPU read) events + MR (memory read) |
| `out` | OUT (CPU write) events + MW (memory write) |
| `mr` | only MR (memory read 0xE000-0xE008) |
| `mw` | only MW (memory write 0xE000-0xE008) |
| `text` | without a prefix = case-insensitive substring on the port name |
| `!port:CE` | exclude low byte 0xCE (token-level negation) |
| `!port16:00CE` | exclude full 16-bit 0x00CE |
| `!pc:4042` | exclude PC 0x4042 |
| `!frame:>100` | inverts range -> matches frame <= 100 |
| `!value:42` | exclude value 0x42 |
| `!in` / `!out` | exclude IN / exclude OUT |
| `!mr` / `!mw` | exclude MR / exclude MW |
| `!addr:E008` | exclude MMIO 0xE008 |
| `!text` | exclude ports whose name contains the text |

**`port:` vs `port16:`:** Z80 8-bit IORQ instructions (`OUT (n),A` /
`IN A,(n)`) place `(B << 8) | n` on the address bus, where B is the
high byte of the A register (or any other state). That is, the same
instruction `OUT (0xCE),A` is recorded into history with a different
`event.port` (0x20CE, 0x40CE, ...) depending on the current contents of
A. The `port:` prefix matches **only the low byte** and captures all
these random variants. The `port16:` prefix compares the **full 16-bit
value** - suitable for explicit `LD BC,nn; OUT (C),A` where B is
deterministic (e.g. 0xCFXX sub-registers of GDG).

Hex literals are case-insensitive (`port:CE` = `port:ce`).

### Negation `!`

Negation `!` behaves in two variants, semantically equivalent:

1. **Token-level `!port:CE`** - per-token prefix. The parser internally
   collapses it into a single LEAF node with the `negate=true` flag.
2. **Sub-expression `!(...)`** - unary NOT over the whole sub-tree.
   Inverts the boolean result of the sub-tree evaluation. Example:
   `!(port:CE pc:42)` = NOT (port==CE AND pc==42).

**Semantics for a non-applicable leaf:** If a leaf of type `addr:` /
`mr` / `mw` is evaluated against a **non-MMIO** event (= IORQ), the raw
match is always false. NOT flips it to true. So `!addr:E008` passes
even for a pure IORQ event (= "is not in the MMIO range 0xE008"). If
you want a "MMIO events outside 0xE008 only" filter, combine
explicitly: `!addr:E008 & (mr | mw)`.

### Unified `in` / `out` semantics

- `in`  = matches IN and MR (= flags READ bit set)
- `out` = matches OUT and MW (= flags READ bit unset)

Rules:

- The `addr:` filter rejects IORQ events.
- `mr` / `mw` reject IORQ + opposite direction.

### Usage examples

| Use case | Filter |
|----------|--------|
| Watch scroll registers | `port16:CF03` (= SW = scroll width) |
| Find paint loop after start | `frame:>100 port:CE` |
| Border color changes | `port16:CF06` (= BCOL, deterministic B=0xCF) |
| Status reads during VBlank polling | `in port:CE` |
| All from PSG init code | `pc:4000-40FF out psg` |
| OUT on port CE or CF | `port:CE \| port:CF` |
| Two independent situations | `(port:CE pc:4000-40FF) \| (port16:CF06 frame:>100)` |
| Value in a group | `value:42 \| value:43 \| value:44` |
| Exclude a group | `port:CE !(pc:4000-40FF)` |
| MMIO only, no IORQ | `!(in \| out)` (= equivalent to `mr \| mw` via negation) |
| OR inside, AND outside | `pc:4000-40FF & (port:CE \| port:CF)` |
| Double negation | `!!port:CE` (= `port:CE`) |

### UI quick-actions (right-click row)

Right-clicking a row in the History table opens a context menu with
four sets of actions that manipulate the current filter string:

- **`Set: ...`** - overwrites the filter (= replaces the whole input
  contents).
- **`Add AND: ...`** - appends a token with a space before (= implicit
  AND).
- **`Add OR: ...`** - appends with ` | ` before (= OR with the entire
  existing filter). Beware of operator precedence: `&` has higher
  priority than `|`, so `port:CE | port:CF` + `Add AND: pc:42` ->
  `port:CE | port:CF pc:42` = `port:CE OR (port:CF AND pc:42)` (= CE
  passes without pc:42). For correct "narrow the whole OR chain with an
  AND" use the variant **`Add AND group:`** below.
- **`Add AND group: ...`** - wraps the entire current filter in
  parentheses and appends the token via `&`: `(current) & token`. For
  an empty filter it degrades to a bare token.
- **`Add OR group: ...`** - analogously `(current) | token`.

Each set offers about 6-12 variants (port low byte, port16 full, pc,
cycle, value, addr, type R/W, negate variants). The `Add ... group:`
set is smaller (= port/addr/pc + their negations), because its typical
use case is a rough narrowing over an existing chain, not a
fine-grained tweak.

Below the filter quick-actions block there is a separate section with
the action:

- **`Show port in Overview`** - switches the active tab from History to
  Overview and sets the Overview filter input so that it matches the
  port of the current event. Filter value logic:
  - **MMIO event** (Type = MR/MW, address 0xE000-0xE008): filter =
    full 4-digit hex address (e.g. `E001`). The Overview substring
    match on the MMIO entry is unique.
  - **IORQ event** (Type = IN/OUT): filter = 2-digit hex of the port
    low byte (e.g. `D8`). Overview shows all ports with that low byte,
    including any 0xCF family.

The action is the symmetric counterpart to the `Show in History tab`
item in the Overview row context menu (= the opposite direction of
cross-tab navigation). Like the "Show in History tab" action it
**overwrites** the current Overview filter (= any previous filter is
lost).

### Limits

- **Filter string buffer:** 128 bytes.
- **Max AST nodes:** 64. On overflow the parser reports
  `Expression too complex (max 64 nodes)`. Too-deep nesting is
  defensively caught by the same limit.
- **Plain-text name token:** max 63 characters. Longer ones report
  `Name token too long`.

### Parse error

If the parser fails, the History tab shows a **red text** below the
sticky header with the error message, e.g.:

```
Filter error: Invalid port hex value (8-bit)
```

No events are hidden (= safe behavior while typing).

Possible parse errors:

| Error | Typical cause |
|-------|---------------|
| `Invalid port hex value (8-bit)` / `(16-bit)` | bad hex (e.g. `port:XYZ`) |
| `Invalid port range (8-bit)` / `(16-bit)` | bad range (e.g. `port:XX-YY`) |
| `Invalid pc hex value` / `Invalid pc range` | bad hex / range after `pc:` |
| `Invalid addr hex value` / `Invalid addr range` | bad hex / range after `addr:` |
| `Invalid <name> value` / `Invalid <name> range` | bad hex for frame/value/cycle |
| `Invalid <name>:>N value` / `Invalid <name>:<N value` | bad compare literal |
| `Empty <name> value` | prefix without a value (e.g. `pc:`) |
| `Empty value after prefix` | general variant of the same |
| `Empty token after '!'` | `!` without a following atom |
| `Empty expression` | empty filter in a sub-expression |
| `Empty expression before '\|'` / `before '&'` | `\| port:CE`, `& port:CE` |
| `Empty expression after '\|'` | `port:CE \|` (trailing OR) |
| `Empty expression before 'OR'` / `before 'AND'` | the same with the keyword variant |
| `Empty expression before ')'` | `(port:CE \|)` etc. |
| `Empty expression between two 'OR'` | `port:CE \| \| port:CF` |
| `Empty expression between AND and 'OR'/'\|'` | `port:CE & \| port:CF` |
| `Trailing AND operator` | `port:CE &` |
| `Unexpected operator after AND` | `port:CE & &` |
| `Expected ')'` | unclosed parenthesis |
| `Unexpected ')'` | unmatched closing parenthesis |
| `Unexpected token` | e.g. a stray `&` at the start |
| `Unexpected trailing token` | text after a syntactically complete expression |
| `Empty expression in parens` | `()` or `!()` |
| `Expression too complex (max 64 nodes)` | expression too large |
| `Unknown filter prefix` | unknown prefix before `:` (e.g. `foo:bar`) |
| `Name token too long` | plain-text token > 63 characters |

## Memory-mapped I/O 0xE000-0xE008

In MZ-700 mode (banking E2/E0) the lower half of the ROM space is
mapped to a mirror of PIO/CTC/GDG at addresses 0xE000-0xE008. The CPU
instructions `LD A,(0E000h)` / `LD (0E008h),A` access through the
memory bus (MREQ), not through the IO bus (IORQ), but the physical
target is the same HW chip as for IORQ 0xD0-0xD7 / 0xCE.

The I/O Ports panel tracks these MMIO events in a single unified ring
with the IORQ events. Events have the `MEMORY` flag set and the
direction is distinguished by the `READ` flag. The Type column in the
History table shows `MR` (memory read) or `MW` (memory write).

**MMIO entries in the catalog:**

| Addr   | Name                              | Direction |
|--------|-----------------------------------|-----------|
| 0xE000 | MZ-700 mmio - PPI Port A (R/W)    | R/W       |
| 0xE001 | MZ-700 mmio - PPI Port B (R/W)    | R/W       |
| 0xE002 | MZ-700 mmio - PPI Port C (R/W)    | R/W       |
| 0xE003 | MZ-700 mmio - PPI Control (W)     | W         |
| 0xE004 | MZ-700 mmio - CTC Counter 0 (R/W) | R/W       |
| 0xE005 | MZ-700 mmio - CTC Counter 1 (R/W) | R/W       |
| 0xE006 | MZ-700 mmio - CTC Counter 2 (R/W) | R/W       |
| 0xE007 | MZ-700 mmio - CTC Control (W)     | W         |
| 0xE008 | MZ-700 mmio - GDG DMD/Status (R/W)| R/W       |

In the Overview tab the 9 entries 0xE000-0xE008 are grouped into the
collapsible group "MZ-700 mem-mapped IO".

**Filter syntax** for MMIO see [Filter syntax](#filter-syntax) -
relevant tokens: `addr:`, `mr`, `mw`, `!addr:`, `!mr`, `!mw` and the
unified `in` / `out`.

**Quick Add MEM BP:** The right-click menu on an MMIO entry in Overview
offers **Add MEM R BP** and **Add MEM W BP**, which open the
breakpoint Edit panel with a pre-filled `addr` field (= analog to Add
IORQ R/W BP for IORQ entries). See
[breakpoints/types.md](breakpoints/types.md).

## Persistence (cfg section)

Section `[IO_PORTS_PANEL]` in the emulator INI file (default
`mz800emu.ini` in the home dir). Keys:

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `collapse_gdg` | BOOL | 0 | Overview - GDG section collapsed |
| `collapse_ppi8255` | BOOL | 0 | Overview - 8255 PPI section collapsed |
| `collapse_ctc8253` | BOOL | 0 | Overview - 8253 CTC section collapsed |
| `collapse_fdc` | BOOL | 0 | Overview - FDC section collapsed |
| `collapse_memory` | BOOL | 0 | Overview - Memory bank section collapsed |
| `collapse_psg` | BOOL | 0 | Overview - PSG section collapsed |
| `collapse_joystick` | BOOL | 0 | Overview - Joystick section collapsed |
| `collapse_pioz80` | BOOL | 0 | Overview - Z80 PIO section collapsed |
| `collapse_mz700_mmio` | BOOL | 0 | Overview - MZ-700 mem-mapped IO section collapsed |
| `collapse_cmthack` | BOOL | 0 | Overview - CMT loader hack section collapsed |
| `collapse_unicard` | BOOL | 0 | Overview - Unicard section collapsed |
| `collapse_ramdisk` | BOOL | 0 | Overview - Ramdisk (Pezik / Std) section collapsed |
| `collapse_ide8` | BOOL | 0 | Overview - IDE8 section collapsed |
| `collapse_qdisk` | BOOL | 0 | Overview - QDISK (Z80 SIO) section collapsed |
| `history_capacity` | UNSIGNED | 10000 | Ring size (1000-50000) |
| `history_auto_follow` | BOOL | 1 | Default auto-follow ON in the History tab |
| `tracking_active` | BOOL | 1 | IORQ + MMIO tracking (default ON; gated on the open window) |
| `detail_panel_height` | UNSIGNED | 160 | Selected event panel height in px (60-400) |
| `heat_text_active` | UNSIGNED | 1 | hits/s threshold for green text on an active row (range 1..1000000) |
| `heat_bg_hot` | UNSIGNED | 10000 | hits/s threshold for a red bg tint on a hot row (range 1..10000000) |
| `record_mask` | TEXT | (all 1s) | Per-port selective recording mask, 64-hex string = 256 bool flags; changed from the UI via the "Rec" column in Overview |

**Tracking default ON:** the `tracking_active` key has default `1`.
Tracking is **gated on the window being open** - the hot-path hook is
activated only when the I/O Ports panel is open. When the window is
closed the flag is set to 0 at runtime and the callback path falls back
to the default (= zero overhead). The cfg preference remains preserved
for the next opening.

If the user wants to **permanently disable tracking** (= no hot-path
hook even with the window open), they uncheck the "Track" checkbox in
the sticky header and the setting is saved to the INI
(`tracking_active = 0`) on emulator exit.

**Note on cfgmodule UNSIGNED:** The parser always parses values as hex
(compatibility with the save format `0x%02x`). That is, `history_capacity
= 0x2710` (= 10000 dec). A user-edited INI with a decimal literal is
interpreted as hex.

## Use cases

Practical scenarios for using the History tab while debugging MZ-800
software:

| Use case | Filter | What to expect |
|----------|--------|----------------|
| Watch scroll registers | `port16:CF03` | OUT events on SW (scroll width) - typically at picture start or during HW scroll effects |
| Find paint loop after start | `frame:>100 port:CE` | OUT on DMD after the initial ROM boot - usually the main game loop |
| Border color changes | `port16:CF06` | OUT on BCOL (4-bit color) - mid-frame raster effects, flag animations |
| Status reads during VBlank polling | `in port:CE` | IN from 0xCE Status (bit 7 = VBLN) - typical busy-wait before repaint |
| PSG init code | `pc:4000-40FF out psg` | OUT on 0xF2/F3 on the early ROM boot path |
| Watch keyboard scan loop | `port:E0` | OUT (8255 PPI Port C, KSTROBE) in the keyboard scan |
| FDC operations | `port:DC-DF` | IN/OUT range for WD279x read/write - during disk I/O |
| Banking changes | `port:E0-E4` | Memory banking switching - typically PEHU/RAM/ROM switching |

**Workflow recommendation:**

1. Check `Track` in the sticky header (= enable recording).
2. Run the emulator in debugger active mode (= the debugger loop must
   be active, otherwise callbacks are not bound).
3. Reproduce the scenario in the emulator (= load MZF, start a game).
4. Switch to the History tab, apply the filter (see table above).
5. Click a row for detail (Frame/PC/Port name/Type/Value).
6. For closer analysis of PC addresses jump into the disasm panel by
   clicking on PC in the detail panel (see
   [disassembly.md](disassembly.md)).

## Troubleshooting

**Activity stays 0/s even while the emulator runs:**

- Check the `Track` checkbox in the sticky header. The default is ON,
  but if it was previously unchecked and saved to the cfg, it stays
  OFF.
- Verify that the debugger is in active mode. If not, the hot-path hook
  is never called. The Track checkbox edge triggers the callback swap
  automatically.
- Verify that the emulator is not paused (= no new IORQ).

**The History tab is empty:**

- Same as "Activity 0/s" - verify Track ON + emulator running.
- If Track was just turned on, history starts filling from this point.
  The buffer is an in-memory ring, not persisted (= emulator restart =
  clear).

**Some ports show `??` in the Hex column:**

- This is not a bug. `??` = the port has no readable mirror in the
  emulator:
  - Write-only HW without internal storage (= no side-effect-free
    read).
  - Side-effect read (= PPI Port B keyboard scan = strobe pulse).
- The tooltip over `??` explains the reason.
- The current state can often be read indirectly: the GDG status
  register 0xCE bit 2 reads the CKSW signal (= mirror for 0xCF07).

**"Filter error: ..." red text on the filter:**

- The parser detected a syntax error (= bad hex literal, missing value,
  invalid range). No events are hidden (= safe behavior).
- See the [Filter syntax](#filter-syntax) table for supported tokens.

**16-bit IORQ port not captured by the filter `port:CE`:**

- 16-bit IORQ (= 0xRRCF group) has the full 16-bit address in history
  (e.g. `0x06CF` for BCOL). The filter `port:CE` matches the **low
  byte 0xCE**, which does not match 0x06CF (low byte = 0xCF).
- For a 16-bit port use the `port16:` prefix with the full 16-bit
  value, e.g. `port16:06CF` or `port16:CF00-CFFF` for the whole family.

**8-bit IORQ port not captured by the filter `port16:00CE`:**

- 8-bit IORQ (`OUT (n),A` / `IN A,(n)`) places `(B << 8) | n` on the
  bus, where B is the high byte of the A register, i.e. random. The
  filter `port16:00CE` matches only events with `event.port == 0x00CE`
  exactly - i.e. only cases where A=0 or B=0 deterministically.
- For a low-byte-only match (= capture all 8-bit IORQ with the given n
  regardless of random B) use the `port:CE` prefix.

## Related panels

- **Events** ([`event-viewer.md`](event-viewer.md)) - real-time view of
  **all** emulator events (= not just IORQ + MMIO, but also CPU_INT,
  GDG, banking, PSG, BP fire, HALT/RST, USER_MARK). Its own in-memory
  ring of 50000 events (default), 24 B per event. Use case: if you need
  to see IORQ in the broader context of CPU/HW activity (= what was
  happening around the IN/OUT on CTC, INT pins, BP), use the Events
  window. If you need a narrowly port-focused history with a quick
  "Add IORQ R/W BP" on the row, use the History tab in the I/O Ports
  panel. Both panels have separate rings and separate hot-path gates
  (= enabling one does not enable the other).

- **Trace Suite** ([`Trace_Suite.md`](Trace_Suite.md)) - a post-mortem
  file log per subsystem (iorqlog / intlog / hwlog / cputrack /
  marklog). Suitable for long runs and an offline RE pipeline. The
  IORQ data in `iorqlog` chunks is identical to what is in the I/O
  Ports History tab, only in binary form for an external parser.

- **Memory Map** ([`memory-map.md`](memory-map.md)) - current memory
  mapping state per 4 kB page including MemExt expansion. Target of
  cross-window navigation from banking ports 0xE0-0xE7.

- **Breakpoints** ([`breakpoints/README.md`](breakpoints/README.md)) -
  IORQ_R / IORQ_W / MEM_R / MEM_W breakpoint types have a quick "Add
  BP" action with pre-filled port / address fields in the I/O Ports
  panel Overview + History.
