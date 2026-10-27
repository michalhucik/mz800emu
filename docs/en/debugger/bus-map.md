# Bus Map - who owns which I/O port and MMIO address

The debugger window **Bus Map** (menu **HW & Devices / Bus Map**, MZ-700,
MZ-800 and MZ-1500) shows the Z80 bus of the emulated machine from the
side of the addresses: for every I/O port 00h-0FFh and every address of
the memory-mapped window 0E000h-0E0FFh it tells which chip of the board
or which expansion device answers there, in which direction, how it is
connected to the data bus and what happens when two devices meet.
Expansion devices (devbus) exist on the MZ-700 only; on the MZ-800 and
MZ-1500 the window shows the chips and peripherals of the board. The I/O Ports panel is the view
"register by register"; the Bus Map is the view "address by address" and
both can be open side by side.

## Contents

- [What the window shows](#what-the-window-shows)
- [Ports tab](#ports-tab)
- [MMIO tab](#mmio-tab)
- [Owner list](#owner-list)
- [Detail of the selected address](#detail-of-the-selected-address)
- [Keyboard](#keyboard)
- [Output stage of a device](#output-stage-of-a-device)
- [Undriven bus and conflicts](#undriven-bus-and-conflicts)
- [Activity tracking](#activity-tracking)
- [Settings (INI)](#settings-ini)
- [Related](#related)

## What the window shows

```
[Ports] [MMIO]   Color by [Owners v] (?)   [x] Track activity
+-- status strip above the grid ---+||+-- Owner   Ports    Drive      State -------+
|     0  1  2  3 ... F             ||||[CH] CMT hack  01h-02h  none       present   |
| 00  .  CH CH  .  ...             ||||[U]  Unicard   50h-51h  tri-state  present   |
| 50  U  U  .   .  ... F1 F1 F1 F1 ||||[K8] K&P 80... 70h-73h  tri-state  [Connect] |
| 70  K8 K8 K8 K8  ...             ||||  ...  (ScrollY, sorted by the first port)    |
| F0  G  G  .   .  Q  Q  Q  Q  RD  |||+-- [K8] Port 70h  K&P 80 Column Card -------+
+----------------------------------+||| IN/s 0  OUT/s 0  ghost 0  conflicts 0       |
                                    ||| Last IN -   Last OUT -                       |
                                    ||| Peek: -  [Peek now]                          |
                                    ||| Flags: potential conflict                    |
                                    ||| [Show in I/O Ports]  [Reset activity]        |
                                    ||| [Break on IN]        [Break on OUT]          |
MZ-700 | 3 devices, 0 connected (?)
```

On the left a 16 x 16 grid (row = high nibble, column = low nibble of the
low address byte); the cell size follows the font, so the abbreviation
always fits. On the right the list of owners of the active grid and,
below it, the detail of the selected address. Each cell carries the
abbreviation of its owner (`U` Unicard, `F0` FDC 0, `M` memory mapper,
`K8` K&P 80 card, `P7` PCG-700, ...), a colour by the chosen mode and
badges (a dark circle with a white glyph, sitting on a corner of the cell
and never covering the abbreviation) that never rely on colour alone:

| Mark | Meaning |
|------|---------|
| hatched cell | the owner is not present (a peripheral disconnected in the HW & Devices menu, a chip that does not decode in the current mode) or the expansion device is not connected |
| `!` with a yellow ring (top right) | potential conflict: two owners would drive the same bits (connecting the second one fails with the default policy) |
| `!` with a red ring | a read with a conflict happened at run time (policy `and` / `first`) |
| `?` (top left) | the output stage of the owner is not documented in the knowledge base (`not documented`) |
| `~` (bottom left) | reads and writes go to different owners |
| dot (bottom right) | the program accessed an address without any owner while tracking was on (ghost access) |

The badges are spelled out in the cell tooltip and in the **Flags** line
of the detail.

Colour modes (the **Color by** combo): **Owners** (blue board chip, green
expansion device, dark and hatched not present / not connected, grey no
owner), **Drive** (output stage: tri-state, partial, open collector,
none, not documented), **Activity** (heat by accesses per second, the
same thresholds as in the I/O Ports panel; the **(?)** next to the combo
lists them). The mini cells in the owner list change colour with the
mode, so the list is the legend; there is no separate legend.

The strip above the grid normally describes the grid in a few words; it
is highlighted when activity tracking is off or when the upper area
0E000h is not mapped right now (MMIO tab). Hovering a cell shows a
structured tooltip (owners with direction, kind, state and output stage,
range, source of the declaration, mirrors, marks in words, last values
and activity); left click selects the address (and the row of its owner
in the list), right click opens a menu (Show in I/O Ports, Break on IN,
Break on OUT, Reset activity, Peek now).

## Ports tab

Ports 00h-0FFh by the low byte of the address, as the board decodes them.
Owners come from two sources: the chips and peripherals of the board
(static table of the platform: Unicard, FDC 0 / FDC 1, IDE8, RAM disks,
Quick Disk, memory mapper, memory extension, GDG registers, on the MZ-800
and MZ-1500 also the PSG and the Z80 PIO, on the MZ-800 the joysticks and,
in MZ-800 mode, the 8255 and 8253 on 0D0h-0D7h) and the expansion devices
(MZ-700 only: K&P 80 column card on 70h-73h, see
[../devices/kp80.md](../devices/kp80.md)). A peripheral that is
disconnected in the HW & Devices menu still owns its ports, but a read
returns the undriven bus (hatched cell, flag UNMAPPED); chips that do not
decode in the current mode are hatched the same way (the 8255 and 8253 on
the ports of an MZ-800 in MZ-700 mode, the second PSG without the HW
compatibility option).

A port with different owners for reads and writes carries the flag
DIR_SPLIT: on the MZ-800 that is 0F0h (OUT palette of the GDG, IN
joystick 1). Port 0CEh (OUT DMD, IN status) belongs to the GDG in both
directions and is therefore not split. The MZ-800 mapper also decodes IN
0E0h / 0E1h (VRAM / CG-ROM selection) but does not drive the bus; such a
read counts as a ghost access. On 0E9h-0EBh of the MZ-800 two owners
exist, the Pezik RAM disk at 0E8h and the standard RAM disk; the emulator
gives the Pezik priority, so the window reports a potential conflict.

## MMIO tab

The window 0E000h-0E0FFh: the 8255 PPI (0E000h-0E003h), the 8253 CTC
(0E004h-0E007h, the control word is write-only), the status / TEMPO
register 0E008h and, on the MZ-700, the expansion devices that watch
writes there (SUC 80-column kit 0E00Ch-0E00Fh, PCG-700 0E010h-0E012h).
On the MZ-800 the chips decode these addresses only with the DMD in
MZ-700 mode (hatched in MZ-800 mode), on the MZ-1500 whenever the upper
ROM is mapped with SPEC = 0. Every cell has
two halves: the left one shows the owner in the state "upper area
mapped" (after reset, `OUT (0E3h)`, `OUT (0E4h)`), the right one in the
state "DRAM at 0E000h" (after `OUT (0E1h)`); a grey half means nobody
decodes the address in that state. When DRAM is mapped right now the
whole grid is dimmed and the strip above the grid says so.

## Owner list

The right panel: one row per board chip or expansion device of the active
grid, sorted by the first port. Columns: a mini cell with the
abbreviation in the colour of the cell (the legend), name, address ranges
(at most three, more as "+N"; all of them in the tooltip), output stage,
state (present / not present / connected / not connected) and, for
expansion devices, a **Connect** / **Disconnect** button (the same action
as the HW & Devices menu; board chips are attached in that menu).
Hovering a row highlights its cells in the grid with a yellow frame,
clicking a row selects its first address, clicking a cell selects the row
of its owner. The row tooltip: full name, direction, all ranges, the
source of the output stage declaration, the mirror note (the emulator
decodes a port by the low address byte only, not measured on the real
machine; no note on the MZ-700) and, for devices, the number of ports,
MMIO addresses, memory regions and outputs. The list scrolls and the
selected row is always visible. On the MZ-800 and MZ-1500 (no expansion
devices) the button column is absent.

## Detail of the selected address

Below the list a panel of constant height (the list above it does not
move when the selection or the state changes): a title with the mini
cell and the owner name ("Port 70h  K&P 80 Column Card"; without an owner
"Port 20h - no owner"; the title tooltip lists all owners), a line about
the owner (direction and output stage, with partial drive also the driven
bits; other owners of the same address after **Also:**; without an owner
instead what a read returns: "A read returns the undriven bus: bus latch
5Ah"), an activity line in a monospaced font (IN/s, OUT/s, ghost,
conflicts; writes/s for an MMIO address), the last IN and OUT values with
their age in frames, **Peek** (the value the CPU would read now, without
side effects: devices answer through their peek handlers, board chips
through register mirrors; automatic while the emulator is paused with
the text "(automatic while paused)", on the **Peek now** button
otherwise; with partial drive also the mask of driven bits and the source
of the value), **Flags** in words and four buttons of equal width: **Show
in I/O Ports**, **Reset activity**, **Break on IN**, **Break on OUT**
(IORQ breakpoints are disabled for MMIO addresses). A long line is cut
rather than wrapped; its tooltip has the full text. With tracking off
the numbers are dashes and the line "Activity tracking is off" replaces
the last values.

Status line: platform and the number of expansion devices ("MZ-700 |
3 devices, 0 connected"; on the MZ-800 and MZ-1500 the platform only); a
non-default undriven-bus model (`const`) and conflict policy (`and`,
`first`) are appended. The **(?)** at the end shows the tracking state,
the undriven-bits model with its current value, the policy and the frame
number.

## Keyboard

Tab moves between the toolbar, the grid, the list and the detail
buttons; arrows in the grid move the selection (the focused cell has a
frame), Space or Enter on a cell selects it, Enter on the row of an
expansion device connects or disconnects it. Below its natural width
(grid + list) the window does not reflow, it gets a horizontal scrollbar;
the splitter between the grid and the list (a thin line, highlighted on
hover) can be dragged, a double click restores the automatic width. The
list has the same width on both tabs, switching Ports / MMIO moves
nothing.

## Output stage of a device

Every port range and MMIO range of a device declares how its hardware is
connected to the data bus:

| Output stage | Meaning | Examples |
|--------------|---------|----------|
| none (write only) | never drives the bus, only watches writes | PCG-700 registers, SUC latch, memory mapper, GDG registers, PSG |
| tri-state | drives all eight bits while its own address is decoded | 8255 PPI (datasheet), Z80 PIO (technical manual), Unicard (VHDL), K&P 80 card (74LS245 behind the board-select signal) |
| partial | drives only some bits, the rest float | joystick buffers of the MZ-800 (74LS365, bits 0-5; nobody drives bits 6-7 on the hardware, the emulator returns 0FFh without a joystick) |
| open collector | wired-AND with idle 1 | not found on the data lines of any MZ; kept for completeness |
| unknown | the knowledge base has no schematic or datasheet of the output stage | FDC card, IDE8, RAM disks, Quick Disk interface, GDG status (0CEh, 0E008h; custom LSI without a datasheet) |

The declaration comes from documents, never from a guess; `unknown` is
shown with `?` and treated as "may drive all bits".

## Undriven bus and conflicts

Bits nobody drives get the value of the **undriven-bus model** of the
emulator: by default the last byte the CPU read from memory (the bus
latch; `IN A,(n)` from a free port returns `n`), optionally a constant
such as 0FFh. **Neither model was measured on real hardware** - the
constant exists to compare period software against both assumptions.

Two devices may share an address when at most one of them drives it (a
card that only watches writes next to a chip of the board). Two drivers
on the same bits are a **conflict**. The default policy `refuse` does not
connect the second device (the HW & Devices menu and `emu_periph_attach`
report a conflict). The policies `and` (bitwise AND of the two values)
and `first` (the device connected first wins) allow the connection; every
read with a conflict is then marked in the Event Viewer (IORQ subtype
CONFLICT), counted in the inspector and reported to MCP clients (event
`bus_conflict`). These two policies are models - the real result of two
fighting bus drivers is undefined and they are meant for experiments, not
for faithful emulation. They have no switch in the GUI; see
[Settings](#settings-ini).

## Activity tracking

**Track activity** switches the CPU to logging callbacks (the same
mechanism as the I/O Ports panel; the two windows share it, closing one
does not stop the other) and fills accesses per second, last values,
totals and the ghost counters. With tracking off the grid still shows
owners, flags and the conflict counters of the bus core.

## Settings (INI)

The window keeps `color_mode`, `active_tab`, `list_width` (list width in
px, 0 = automatic) and `tracking_active` in the section `[BUS_MAP_PANEL]`. The
bus model lives in the section `[DEVBUS]` of the MZ-700 emulator (the only
platform with expansion devices) and has no switch in the GUI (it is also reachable through MCP `emu_settings_set`,
keys `DEVBUS/conflict`, `DEVBUS/bus_idle`, `DEVBUS/bus_idle_value`):

```ini
[DEVBUS]
conflict = refuse       ; refuse | and | first
bus_idle = latch        ; latch | const
bus_idle_value = 0xff   ; value for const
```

The policy and the undriven-bus model are stored in a replay file; a
playback with different settings warns.

## Related

- [io-ports.md](io-ports.md) - register by register, history, filters.
- [event-viewer.md](event-viewer.md) - IORQ events with the subtypes
  UNCONNECTED (ghost) and CONFLICT.
- [../devices/pcg700.md](../devices/pcg700.md),
  [../devices/suc80.md](../devices/suc80.md),
  [../devices/kp80.md](../devices/kp80.md) - the expansion devices.
- [../mcp-server/tools-overview.md](../mcp-server/tools-overview.md) -
  `emu_get_bus_map`, `emu_bus_peek`.
