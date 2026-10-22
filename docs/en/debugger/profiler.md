# CPU Profiler - per-function T-state aggregation

## 1. What it is and what it is for

The **CPU Profiler** is a standalone dockable ImGui window that aggregates
T-states (= Z80 clock cycles) per function. For every unique call target
(CALL / RST / IRQ accept / NMI) it holds:

- **Calls** - number of entries
- **Inclusive cycles** - sum of T-states including time spent in nested
  calls
- **Exclusive cycles** - sum of T-states excluding time of nested calls
  (= "own" work of the function)
- **Min/Max inclusive** - shortest and longest single call

The profiler answers questions like "in which routine am I spending the
most time", "how many times does this routine get called per frame",
"how long does the IRQ handler run".

On the Functions tab the granularity is not per instruction but per CALL (= per
function entry address). Time per single instruction is shown by the
**Per PC** tab (section 10). For analysis of individual scanline routines
use the sibling **Callstack** + **Stack Monitor** windows.

The profiler is built as a listener on top of the callstack subsystem,
which means it automatically inherits all callstack captures
(CALL/RST/IRQ accept/RETI), as well as its limits (= multi-stack OS,
banking, see section 6).


## 2. Subsystem activation

The profiler is **disabled** at emulator startup. It can be enabled by:

| Path | What happens |
|---|---|
| **INI key** `[PROFILER] active=1` | Subsystem activated at startup, persists across INI save/load |
| **CLI flag** `--profiler` | Force ON over the INI value at startup |
| **Menu** Debug -> CPU Profiler | Opens the window and activates the subsystem |
| **Hotkey** Alt+Shift+P | Toggle the CPU Profiler window |
| **UI Start button** in the toolbar | Start measuring during emulator run |

The profiler is a listener on top of the callstack subsystem. Turning the
profiler on automatically forces the callstack on (= the callstack will
be enabled even if the user did not enable it explicitly). When the
profiler is deactivated, the callstack:

- returns back to **OFF** if it was enabled by the profiler (= ownership)
- stays **ON** if the user had it enabled already before the profiler
  was turned on

This makes the profiler behave predictably - no unexpected "callstack is
still running" after Stop Profiler.


## 3. Workflow

A typical run:

1. **Load symbols** (optional but recommended) - File -> Load symbols,
   formats `.sym` / `.lbl` / `.map` / NoICE. The profiler will then show
   function names instead of `func_C123`.
2. **Open the window** via menu Debug -> CPU Profiler, or via
   Alt+Shift+P.
3. **Click Start** in the toolbar. The status bar switches to `RUNNING`.
4. **Run the program in the emulator** or the game level you want to
   profile. Cycles accumulate continuously.
5. **Click Stop**. The status bar switches to `STOPPED`. Data in the
   table remain available (= Stop does not discard the aggregator).
6. **Inspect the table**. Sort by Excl% / Calls / Max by clicking on the
   column header.
7. **Export CSV** (optional) - for post-processing in Excel / Python.

**Reset** explicitly clears the aggregator. Without Reset, the Profiler
accumulates data from the last Start (= another Start without Reset
continues the aggregation).


## 4. Metrics and table columns

| Column | Meaning |
|---|---|
| **Function** | Symbol from the database (NoICE / .map / .sym / .lbl), otherwise `func_XXXX` |
| **Address** | Function entry address (CALL target) in hex |
| **Kind** | CALL / RST / IRQ-IM0 / IRQ-IM1 / IRQ-IM2 / NMI |
| **Calls** | Number of paired on_enter events |
| **Excl%** | Exclusive cycles as % of the total exclusive sum of all entries |
| **Excl** | Exclusive cycles (= without nested calls) |
| **Incl%** | Inclusive cycles as % of the total inclusive |
| **Incl** | Inclusive cycles (= including nested) |
| **Avg** | Inclusive / Calls (= average inclusive time of one instance) |
| **Min** | Shortest single call (inclusive) |
| **Max** | Longest single call (inclusive) |

### Inclusive vs Exclusive - the key distinction

Mnemonic via the literal meaning:

- **Incl = INCLUDING children** = "total time from function entry to its
  return, **including** nested calls"
- **Excl = EXCLUDING children** = "**only the own work** of this
  function, **without** time of children"

Relationship: `Excl = Incl - sum Incl(children called inside the function)`.
That is, `Excl <= Incl` always. For a leaf function (= calls nothing)
`Excl == Incl`.

Example:

```
parent:                  ; entry CALL parent
    LD A, 5              ;   2 T  own
    LD B, 3              ;   2 T  own
    CALL child           ;  17 T  own (call setup) + 50 T inside child
    LD C, A              ;   1 T  own
    RET                  ;  10 T  own
                         ; -----
                         ;  32 T own + 50 T in child = 82 T total
child:
    ; (= 50 T spent inside)
    RET
```

| Function | Calls | Excl | Incl | Avg |
|----------|------:|-----:|-----:|----:|
| parent   | 1     | 32   | **82** | 82  |
| child    | 1     | 50   | **50** | 50  |

`parent.Incl = 82` (= total from CALL to RET)
`parent.Excl = 32` (= 82 - 50 children = parent's own instructions)

### When to look at what

| Question | Sort by | Look for |
|----------|---------|----------|
| What is actually burning CPU? | `Excl%` desc | Hot spots - functions with large Excl are the bottleneck |
| Where is time spent in the call tree? | `Incl%` desc | Functions with high Incl but low Excl are "dispatchers" - they call a lot, own work small |
| Short call thousands of times vs one long? | `Calls` desc + `Avg` | High Calls + low Avg = micro-overhead |
| Variability of raster routines? | filter on the routine | `Min ~ Max` = stable; `Max >> Min` = occasional slow path |

### Column sums

- `sum Excl` of all entries = **Total cycles** (= 100 % CPU time in
  recognized calls; the rest = code between calls and IRQs)
- `sum Incl` > Total cycles - nested calls are counted in each parent +
  separately as their own entry. That is why the `Incl%` column is a
  different scale than `Excl%`.


## 5. UI controls

### Toolbar

- **Start / Stop** - measurement on/off (= equivalent to the INI active toggle)
- **Reset** - clears the aggregator; status counters go back to 0
- **Snapshot** - explicit table refresh (= outside the auto polling)
- **Export CSV** - saves data to a file (section 7)

### Filter / kind toolbar

- **Filter:** substring search over Function name or Address (hex).
  Case-insensitive.
- **Kind checkboxes:** CALL / RST / IRQ / NMI - hides categories from the
  view (data remains in the aggregator, it just is not displayed).

### Sort

Click a column header to sort. A second click reverses the direction.
The sort spec persists in INI across sessions.

### Hover, left click, right click on a row

- **Hover (~250 ms)** displays a tooltip with the function name, address,
  kind, calls/cycles counts and min/max/avg
- **Left click** opens / focuses **Disassembly #1** on the function
  entry address
- **Right click** opens the context menu:
  - **Set Breakpoint** - inserts a BP at the entry address
  - **Add Bookmark** - adds a bookmark with the symbol name or `#XXXX`
    fallback and opens the Bookmarks window
  - **Show in Disassembly #2 / #3 / #4 / #5** - opens / focuses the
    secondary disasm slot at the entry address

### Status bar

The bottom row of the window:

- **Status** `RUNNING` / `STOPPED` - subsystem run state
- **Total cycles** - total profiled time (T-states) since the last reset,
  = `sum Excl` of all buckets
- **Calls** - total of on_enter events (= sum of the Calls column)
- **IRQ** - how many of the `Calls` were IRQ/NMI accept (= not CALL/RST)
- **Unmatched** - see below
- **Buckets** - see below

### Unmatched

Counter of exit events where the callstack subsystem could not pair
`RET`/`RETI`/`RETN` with the corresponding `CALL`/`RST`/`IRQ accept`.
That is "a return without a known call".

It appears in patterns:

1. **PUSH+RET trampoline** - code pushes an address onto the stack and
   does `RET` instead of `JP/JR`:
   ```asm
   LD HL, target
   PUSH HL
   RET            ; jump to target, but the callstack did not see a CALL
   ```
   Then target executes `RET` -> callstack has an empty shadow stack ->
   "unmatched". Typically **BDOS dispatch tables in CP/M**.

2. **longjmp pattern** - jump back over several frames (e.g. an error
   handler that restores SP to N frames back and returns).

3. **Self-modifying RET address** - code overwrites the return address
   on the stack -> callstack sees a different RET target than expected.

4. **ISR-via-RET** - older programs sometimes end an ISR with `RET`
   instead of `RETI`/`RETN` (for Z80 PIO without IRQ cascading equivalent).

The profiler discards these events (`++unmatched_returns`) and
**ignores them in the aggregator** (= no update of Calls/Cycles). For
this exit we have no paired enter, so inclusive cycles cannot be
computed.

Value interpretation:

| Unmatched | Meaning |
|-----------|---------|
| 0 or a few units | OK, data is reliable |
| Grows steadily | Multi-stack OS pattern (CP/M BDOS, NIPOS) - profiler data **is not accurate** |
| Tens per second regularly | Consider whether the profiled code is a heavy PUSH+RET dispatch (DSL interpreters, FORTH) |

### Buckets

Number of unique `(target_addr, kind)` records in the aggregator. The
profiler keeps a hash map where:

- **key** = function entry address (= CALL target / RST vector / IRQ ISR
  start) + kind (CALL/RST/IRQ-IM0/.../NMI)
- **value** = table row with metrics (calls, excl, incl, ...)

A bucket is created **on the first entry** to the given (address, kind)
combination. They are cleared only by explicit Reset.

Sanity check:

| Buckets | Meaning |
|---------|---------|
| 0 | The profiler has not captured anything yet (after reset / before start) |
| tens to hundreds | Typical range for a Z80 game or demo |
| thousands and growing | Possible SMC code / banked code is mixing in - the aggregator data is flooded with addresses that are not stable symbols |

Memory footprint: each bucket ~64 B. 10 000 buckets ~ 640 KB,
effectively unlimited.


## 6. Limitations

### Multi-stack OS (CP/M BDOS, NIPOS, multitasking)

The callstack maintains a single shadow stack. When the OS switches SP
into another routine (typically a BDOS call into a TPA program), the
callstack sees a divergence and emits a synthetic DIVERGENT exit. The
profiler skips such exits (= into `unmatched_returns`). Consequence: for
CP/M applications profiler measurement is **inaccurate** - names/cycles
start mixing between TPA and BDOS scope.

Workaround: profile only non-CP/M code (= IPL bootloader, single-stack
games, ROM monitor).

### Banking

The profiler buckets by `(target_addr, kind)` - the banking state is not
included in the key. If the same address means different code in
different bank states (= typically 0x0000-0x0FFF on MZ-800: ROM monitor
vs RAM after SW1), the aggregator merges them into a single bucket.

### SMC (self-modifying code)

Aggregation is per entry address - SMC code = one bucket. If a routine
overwrites itself and returns from various places, the profiler measures
the entrypoint, not the actual paths.

### Cycles wraparound

The internal cycle counter is uint32 (~1224 seconds at 3.5 MHz, ~5.7
minutes at 14 MHz). The profiler maintains its own 64-bit baseline with
wraparound detection - no precision loss across long sessions. After a
hard CPU reset, however, the baseline could be confused; in that case
do a Profiler Reset.

### Only paired calls

If a routine performs `RET` through another mechanism (PUSH addr + RET
trampoline), inclusive cycles return to the parent through longjmp
divergence handling - but the divergent exit itself is skipped by the
profiler (unmatched_returns++). For unusual control-flow patterns data
may be missing.


## 7. CSV export

The **Export CSV** button in the toolbar opens a save dialog. The
output is:

- **Encoding:** plain UTF-8 without BOM (modern Excel detects UTF-8 on
  its own; the BOM would appear as garbage in tools without BOM awareness)
- **Separator:** `,` (comma)
- **Decimal:** `.` (= 12.34, not 12,34) - locale-safe formatting, so a
  Czech locale `cs_CZ` will not break the structure
- **Line endings:** by platform (LF on Unix, CRLF on Windows; Excel /
  LibreOffice handle both)

Columns in CSV:

```
Name,Addr,Kind,Calls,Excl_cyc,Incl_cyc,Excl_pct,Incl_pct,Min,Max,Avg
```

The first row is a header. The Filter and kind checkboxes **do not
affect** the CSV export - the complete aggregator is always exported.

Example usage for Python post-processing:

```python
import pandas as pd
df = pd.read_csv('profile.csv')
hot = df.sort_values('Excl_cyc', ascending=False).head(10)
print(hot[['Name', 'Calls', 'Excl_cyc', 'Avg']])
```


## 8. Persistence

Profiler data is **volatile** - it is not carried over between emulator
sessions. Explicit Export CSV is the only path for long-term storage.

In the INI section `[PROFILER]` only the following are stored:

- `active` - bool, auto-start the subsystem on the next emulator launch
- `show_window` - bool, auto-open the window

The table sort spec (= column ordering) is stored by ImGui itself in
its own INI.


## 9. Tips

- **Load symbols BEFORE Start** - otherwise you see `func_XXXX` names.
  If you load symbols only after Start, names get filled in on the next
  Snapshot (= the lookup happens on render, not on push).
- **For hot-spot hunting** - sort by Excl% desc. The topmost entry is
  the strongest optimization candidate.
- **For raster timing** - the profiler is not the right tool
  (granularity is per-CALL, not per-scanline). Use Stack Monitor +
  breakpoints at the scanline position.
- **IRQ as parent** - by default the inclusive parent contains the IRQ
  time. That is, when an IRQ interrupts the main loop, the IRQ time is
  counted into the main inclusive as well as a separate IRQ entry.
- **Reset before a benchmark sequence** - to be sure you measure only
  the relevant window, not history.
- **Watch window + Profiler together** - hot spot in the Profiler ->
  Watch on the variables of that routine -> you see what the routine
  does with the data.


## 10. Per PC tab (per-PC profiler)

The CPU Profiler window has three tabs: **Functions** (everything
described above), **Per PC** and **Coverage** (section 11). The Per PC tab counts every executed
instruction by its address, regardless of CALL and RET. It therefore
also sees code that is jumped to, interrupt handlers and loops in the
main program - what the Functions tab adds to one whole function is
split here into single instructions.

Controls:

- **Start** begins collecting and clears the previous data, **Stop**
  stops collecting (the data stays), **Reset** clears the data.
- **Range** - show only addresses within the range (hex, e.g. `8000` -
  `8FFF`).
- **Export CSV...** writes all executed addresses (not only the shown
  ones), columns `pc,count,tstates,wait,tstates_pct`.
- A left click on a row opens the address in Disassembly #1, a right
  click offers a breakpoint and Disassembly #2-#5.

Columns:

| Column | Meaning |
|---|---|
| Addr | address of the instruction (logical PC) |
| Symbol | nearest loaded symbol at or below the address, `+N` = offset |
| Count | how many times the instruction was executed |
| T% | share of the T-states of all instructions |
| T-states | T-states spent in the instruction, including wait states |
| Avg T | average per execution |
| WAIT | wait states inserted by the hardware (VRAM access, PSG write) |

The table shows at most 1000 rows of the chosen sort column (default
T-states descending, i.e. the most expensive instructions on top).

The status line splits the emulated time since Start into
**instructions**, **HALT** (the CPU stands in a HALT instruction waiting
for an interrupt) and **other** (mainly interrupt acknowledge). Time in
HALT is spare time; a program that waits for the frame in a loop instead
of HALT shows the same spare time as the instructions of that loop.

Limits:

- Block instructions (LDIR, LDDR, ...) count every repetition as one
  execution.
- Memory banks are not told apart - code at the same address in
  different banks adds up.
- While collecting, every instruction costs one extra function call;
  after Stop the emulation is not slowed down.

The same data is available over MCP (`emu_profiler_pc_*`, see
mcp-server/tools-overview.md), the executed addresses also as a bitmap
for code coverage (`emu_profiler_pc_bitmap`).

## 11. Coverage tab (code coverage)

The **Coverage** tab collects code coverage: which instructions ran and
which way the conditional instructions went (both branches, taken only,
not taken only), at the `branch` level by address or at the `detail`
level by code version with counters. It controls collecting (Start,
Stop, Reset, Mark, Export...), filters the rows (partial branches, both
branches, all conditional, new since mark) and opens a row in the
Disassembly windows, which show the same data as colored marks. Full
description: [code-coverage.md](code-coverage.md).
