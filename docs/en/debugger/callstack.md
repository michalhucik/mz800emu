# Callstack window - shadow stack of called routines

## 1. What it is and what it is for

**Callstack** is a standalone dockable window that displays the
reconstruction of the hierarchy of currently in-progress calls (CALL /
RST / IRQ / NMI). It serves to visualize the depth of nested calls and
to diagnose atypical transitions (PUSH+RET trampolines, longjmp
patterns, IRQ accept interrupting a computation).

**It is not a view of the raw stack memory** - that is provided by the
sibling window **Stack Monitor** ([stack-window.md](stack-window.md)).
Stack Monitor shows **what actually lies on the CPU stack in RAM**;
Callstack shows **a reconstruction of who called whom**, built in
parallel with the CPU state. Both windows are used simultaneously -
Stack Monitor for RAM data (PUSH/POP values), Callstack for the call
hierarchy.

### Why a shadow buffer and not parsing SP?

The Z80 has no ABI - no frame pointer, no prologue/epilogue convention.
It is not possible to deterministically reconstruct the call hierarchy
from the CPU stack memory (a routine may POP its own return address as
data, manipulate SP, push arbitrary values, etc.). Therefore the
Callstack **maintains its own shadow buffer**, into which it pushes on
CALL/RST/IRQ accept and pops on RET/RETI/RETN. It is a heuristic, not
a perfect reconstruction - see the Limitations section.

### Covered events

It covers CALL/RET (Z80 `CALL nn`, `CALL cc,nn`, `RET`, `RET cc`),
RST 00h..38h, IRQ accept in IM 0/1/2 and RETI/RETN unwind. NMI accept
is provided in the API contract, but on MZ-800/700/1500 there is no
NMI signal source in the hot path.


## 2. Subsystem activation

The Callstack is **disabled** at emulator startup (zero hot-path
overhead - the Z80 CALL/RET hooks hold NULL). It can be enabled via:

| Path | What happens |
|---|---|
| **INI key** `[CALLSTACK] active=1` | The subsystem is activated at startup, persists |
| **CLI flag** `--callstack` | Force ON over the INI value at startup |
| **UI Active checkbox** in the toolbar | Hot-toggle at runtime |

UI toggle `Active`:

- **on ON**: registers the Z80 CALL/RET hooks and resets the shadow +
  stats (= clean start)
- **on OFF**: deregisters the hooks and keeps the shadow for the last
  view (= you can inspect the last state)

The race window during a toggle while the emu loop is running is
minimal (pointer assignment is atomic on x86_64). In the worst case
1 frame is lost during the transition; no crash.


## 3. Opening / closing

- **Menu:** Debugger -> Callstack (toggle visibility)
- **DBG Workplace:** flag `wp_callstack` (default OFF) - if enabled,
  the window opens together with the main debugger window
- **Default visibility:** closed at startup. After opening, the
  position/size is remembered via `imgui.ini`

The subsystem **runs even when the window is closed** (= the shadow is
updated continuously); the window is a read-only view.


## 4. Window layout

```
+--- Callstack ---------------------------------------------------+
| [x] Active   [Reset]   [ ] Hide IRQ   [ ] Hide Synth            |
+-----------------------------------------------------------------+
| #  | Ret    | Call   | Target | Sym             | Kind | Cyc-in |
| 0  | 1234h  | 1231h  | 4000h  | sound_play      | CALL | 245    |
| 1  | 2345h  | 2342h  | 5000h  | render_sprite   | CALL | 1248   |
| 2  | 9000h  | 4321h  | 9000h  | ISR_VBL         | IRQ2 | 8194   |
| 3  | 0028h  | 1003h  | 0028h  | KEY_HANDLER     | RST  | 12388  |
| 4  | 0000h  | 0000h  | 0000h  | (no sym)        | SYNT | 25102  |
+-----------------------------------------------------------------+
| Depth: 5 / max=12 | Diverg: 1  SP-swap: 0  Overflow: 0          |
+-----------------------------------------------------------------+
```

### 4.1 Toolbar

| Element | Meaning |
|---|---|
| **Active** checkbox | Subsystem toggle (see section 2). On ON registers hooks + resets the shadow. On OFF deregisters + keeps the last snapshot. |
| **Reset** button | Empties the shadow + zeroes all counters (= like emu reset, but for the callstack only). The UI table is cleared immediately. |
| **Hide IRQ** | Hides rows of type IRQ_IM0/IM1/IM2 and NMI (= UI filter; the subsystem keeps tracking them). |
| **Hide Synth** | Hides rows of type SYNTHETIC (divergent emit). The `Diverg` counter stays visible. |

The filters persist across emu restarts (cfg section
`[CALLSTACK_PANEL]`, keys `hide_irq`, `hide_synth`).

### 4.2 Table

**Row order = GDB / Visual Studio convention**:

- **#0 (top row) = TOP** = **youngest** frame = **the function the CPU
  is currently in**
- higher `#` = **older** = caller toward the root entry (typically the
  ROM monitor / IRQ vector)
- "Path down the table" = "path up back to the caller"

Columns:

| Column | Meaning |
|---|---|
| **#** | Depth index. #0 = TOP (current), #N = deepest (oldest) frame. |
| **Ret** | Return address - **where RET returns from** this frame. For CALL = `call_site + 3`, for RST = `call_site + 1`, for IRQ/NMI = PC at the moment of IRQ accept (= address after the interrupted instruction). |
| **Call** | Address of the CALL/RST opcode - **where** in memory the instruction that created this frame was. For an IRQ frame it has the same value as Ret (= the IRQ accept happened **between** instructions). |
| **Target** | Target call address = **entry of the routine** that was jumped into. For RST `(opcode - 0xC7) / 8 * 8` (= 0x00, 0x08, 0x10, ... 0x38), for NMI always `0066h`. For IM 2 IRQ = address loaded from `(I<<8) | vector`. |
| **Sym** | Symbol resolved against `target`. If no symbol, `(no sym)`. |
| **Kind** | Frame type (see table below). Divergent rows show the Kind in yellow. |
| **Cyc-in** | Real T-states inside the frame = `cycles_now - cycles_at_entry`. The youngest frame (#0) has the smallest value, the oldest (#N) the largest (= it contains the cycles of the entire calling path below it). |

**Resizable + Reorderable + Hideable** - drag the border between
columns to change the width, drag the header to change the order,
right-click on the header to hide a column.

#### Kind values

| Kind | Meaning |
|---|---|
| **CALL** | Z80 `CALL nn` or `CALL cc,nn` - taken. |
| **RST** | Z80 `RST n` opcode, Target is `n*8` (0x00..0x38). |
| **IRQ0** | IM 0 IRQ accept - bus latch byte interpreted as RST 38h on MZ-800. |
| **IRQ1** | IM 1 IRQ accept - hardware fixed call to 0x0038. Default MZ-800 ROM ISR. |
| **IRQ2** | IM 2 IRQ accept - call to the address loaded from `(I<<8) | vector`. Tooltip shows the IM2 vector. |
| **NMI** | NMI accept - call to 0x0066. The MZ HW has no NMI signal source. |
| **SYNT** | Synthetic frame - emitted on RET over an empty shadow (= divergence). Yellow color. |

### 4.3 Footer (statistics)

| Counter | Meaning |
|---|---|
| **Depth: N / max=M** | Current shadow depth / historical maximum since the last reset. |
| **Diverg** | Sum of `Trampoline + Longjmp + Mismatch`. Hover over the value shows the breakdown. |
| **SP-swap** | Count of manual SP manipulations (`LD SP,nn`, `EX (SP),HL`, ...). |
| **Overflow** | Count of rejected push attempts when the maximum shadow depth was exceeded. The emu is not affected, only the shadow stops growing. |
| **Discard** | Heuristic signal of stack reset events (= RET where SP flew above the deepest tracked frame, typically warm boot / ROM reset / exception). The shadow is NOT auto-cleared. On sporadic growth (e.g. after ^C) press Reset for a clean start. |

#### Diverg breakdown (hover)

| Sub-counter | Meaning |
|---|---|
| **Trampoline** | RET over an empty shadow or an SP-nested trampoline (= PUSH+RET dispatch in the style of a CP/M BDOS jump table). The shadow stays intact if SP-nested. |
| **Longjmp** | RET pop across multiple frames at once (= match deep in the shadow via SP or return_addr). Also covers the ISR-via-RET pattern (= an ISR returning with RET instead of RETI). |
| **Mismatch** | Top mismatch + no deep match (= self-modifying RET address or unknown pattern). Conservative top pop. |

Resetting all counters: the `Reset` button in the toolbar (= also
empties the shadow). The counters are also reset on emu reset
(F5 / snapshot load).


## 5. Row actions

### Left click / Shift+click / Double-click

| Action | What it does |
|---|---|
| **LMB click** | Focus the main Disassembly (#1) at the `Call` address (= **where the function was called from**). |
| **Shift+LMB** | Focus the main Disassembly at the `Target` address (= **entry** of the function). |
| **Double-click** | Open/focus the secondary Disassembly (#2) at `Call`. |
| **Hover** | Tooltip with the shortcut hint and entry details (Ret/Call/Target/SP/Kind, IM2 vector, DIVERGENT warning). |

### Right-click context menu

Full menu shared across the debugger:

- Focus disasm at Call / Target (slot #1)
- Open Disasm #2 at Call / Target (slot #2)
- Add bookmark at Call / Target (with automatic symbol resolution into
  the name)
- Set BP at Call / Target
- Copy hex address Call / Target / Return (to the clipboard)


## 6. Worked example - how to read the table

Example from the layout in section 4:

```
#  | Ret    | Call   | Target | Sym             | Kind | Cyc-in
0  | 1234h  | 1231h  | 4000h  | sound_play      | CALL | 245
1  | 2345h  | 2342h  | 5000h  | render_sprite   | CALL | 1248
2  | 9000h  | 4321h  | 9000h  | ISR_VBL         | IRQ2 | 8194
3  | 0028h  | 1003h  | 0028h  | KEY_HANDLER     | RST  | 12388
4  | 0000h  | 0000h  | 0000h  | (no sym)        | SYNT | 25102
```

We read **top to bottom = from the current state to the oldest caller**:

- **Row #0**: the CPU is currently inside the function `sound_play`
  (entry 0x4000). The function was called by a CALL instruction at
  address 0x1231; after RET it returns to 0x1234. It has spent 245
  T-states there.
- **Row #1**: the caller of `sound_play` was the routine
  `render_sprite` (entry 0x5000). It itself was called by a CALL at
  0x2342; after its RET the CPU returns to 0x2345. In `render_sprite`
  we have so far spent 1248 T-states (= the difference 1248 - 245 =
  1003 T-states is the time in `render_sprite` **outside** of
  `sound_play`).
- **Row #2**: `render_sprite` was called from the IRQ2 ISR `ISR_VBL`
  (entry 0x9000). The IM 2 IRQ accept happened between instructions -
  PC was 0x4321 and the ISR entry is at 0x9000. When the ISR does a
  RETI, it returns to 0x9000... wait, **Ret and Target are the same**
  - this means that at the moment of IRQ accept PC = 0x9000 (= the
  IRQ interrupted normal computation right before the instruction at
  0x9000). Cyc-in = 8194 = total time in the IRQ handler + its calls.
- **Row #3**: RST 28h called from 0x1003 - the `KEY_HANDLER` routine
  (entry 0x28). I.e. before IRQ_VBL the program was in the RST 28h
  handler. Cyc-in = 12388 T-states.
- **Row #4**: a SYNTHETIC frame (yellow Kind) - it arose from a
  divergence. Most likely the shadow was empty (= the callstack was
  activated mid-computation) and the first RET created this marker.
  Cyc-in = 25102 = since the push of this frame, 25102 T-states have
  elapsed.

**Note on IRQ entry nesting**: RETI from #2 pops that frame and the CPU
returns to 0x9000... but `render_sprite` (row #1) is deeper than
ISR_VBL. This is the **correct** nesting: `KEY_HANDLER` (#3) was called
by RST 28 -> does its work -> IRQ2 interrupts it -> IRQ_VBL calls
`render_sprite` -> `render_sprite` calls `sound_play` -> the CPU is in
`sound_play`. RETI ends only the IRQ frame (= #2), then the CPU
continues in `KEY_HANDLER` from its RET to 0x0028h. Meanwhile
`render_sprite` and `sound_play` were "guests" inside the ISR and will
finish with their RETs **before** the RETI.

### Nesting depth vs Cyc-in display

Cyc-in grows with distance from the TOP (#0 is the youngest = the
fewest cycles). It is not "exclusive time inside that function" - it
is the **total elapsed since the frame push until now**.


## 7. SYNTHETIC frame - when and why

A SYNTHETIC frame arises on a **divergence** - i.e. when RET (or
RETI/RETN) cannot find a matching CALL/IRQ entry in the shadow stack.

### Typical causes

1. **PUSH+RET trampoline** (jump trick):
   ```
       LD HL, target
       PUSH HL
       RET             ; effectively a JP target
   ```
   The shadow stack does not see this sequence as a CALL (PUSH is not
   captured). When the RET arrives, the shadow top contains no match
   -> emit a SYNTHETIC frame, Diverg counter +1.

2. **Manual call frame**:
   ```
       PUSH return_to
       PUSH func_ptr
       RET             ; indirect CALL via RET
   ```
   The same principle.

3. **POP+JP HL** (the routine takes its own return address as data):
   ```
   sub:
       POP HL          ; HL = return address
       INC HL          ; skip 1 byte after the CALL (= immediate data inline)
       JP HL
   ```
   The shadow stack stays consistent (the CALL registered it), but
   subsequent "RET-like" behavior is not captured.

4. **Longjmp pattern** - RET to an entry found deep in the shadow: pop
   N frames at once, Diverg++ (once). Without SYNTHETIC.

5. **Activation mid-execution**: if you turn on the Active toggle while
   the CPU is inside a call, the shadow is empty and the first RET
   emits a SYNTHETIC. This is **expected** behavior and after a few
   seconds the shadow re-stabilizes (the Diverg counter will however
   show these initial events).

### What to do with SYNTHETIC

- You see it as a yellow `SYNT` Kind in the table
- Hover shows "DIVERGENT (push+ret trampoline or longjmp)"
- It can be filtered via Hide Synth (but the Diverg counter remains)
- It is not an emulator bug, it is a marker of the imperfect callstack
  heuristic


## 8. Limitations and suitable use

| Limitation | Detail |
|---|---|
| `LD SP, ...` detection | Requires opcode classification; the SP-swap counter is currently always 0. |
| NMI accept push | The API contract is in place, but MZ-800/700/1500 has no NMI source in the hot path. If an NMI were to occur, the push would be missing; the unwind via RETN is auto-balanced by divergence handling. |
| Multi-listener | There is only a single listener slot. Multiple consumers = fan-out in the layer above. |
| `total_cycles` wraparound | After a 32-bit cycle counter wraparound (~1224 s at 3.5 MHz) Cyc-in returns a nonsensical value. |
| Pre-activation IRQ frames | If you turn on the Active toggle in the middle of an IRQ handler, the shadow is empty; RETI/RETN will emit SYNTHETIC + Diverg. After stabilization press Reset. |

### Suitable use and limitations for OS-like programs

The single-shadow approach tracks **one global** call stack. For a
typical single-stack program (= game, demo, monolithic utility, ROM
analysis, custom Z80 code with consistent stack discipline including
common ISRs) the heuristic works excellently:

- **CALL/RET pairs** match cleanly (= Depth oscillates reasonably,
  Diverg ~ 0)
- **IRQ accept + RETI/RETN** unwinds correctly (= ISR frame push/pop
  without divergence)
- **ISR-via-RET** (= an ISR returning with `RET` instead of `RETI`) is
  captured via the longjmp branch (= return_addr match on the IRQ
  frame). No false discard.
- **PUSH+RET trampoline** (e.g. a tabular dispatch) is classified as
  the Trampoline counter, the shadow stays intact.

### Where the heuristic hits its limits

| Pattern | Consequence | Recommendation |
|---|---|---|
| **CP/M BDOS/BIOS** - each service has its own stack discipline, switching SP between user-stack and BDOS/BIOS stacks. | Diverg + Discard grow continuously (typically 1-2 / BDOS call). The shadow is recently accurate for the user program scope. | Use **Stack Monitor** (raw SP view) for CP/M analysis. |
| **Multitasking systems** (NIPOS, P-CP/M80, custom RTOS) - per-task stack switching. | The shadow falls apart on a task switch. | Not usable. |
| **HW exception handlers** with stack reset (= warm boot, vector restart) | The Discard counter grows, the shadow stays intact (= no auto-clear). | After detection click Reset for a clean start. |

### What the counters mean during analysis

| Counter growth rate | Interpretation |
|---|---|
| **Diverg = 0** while the program runs | The heuristic is perfectly in sync with the program. Single-stack discipline. |
| **Trampoline > 0**, the rest ~ 0 | The program uses PUSH+RET dispatch tables. OK, the heuristic understands it. |
| **Longjmp > 0**, sporadically | The program uses a longjmp/setjmp pattern or ISR-via-RET. OK. |
| **Mismatch grows regularly** | The heuristic does not understand the program pattern. Either self-modifying RET addresses, or OS-like multi-stack. Recommendation: compare with Stack Monitor. |
| **Discard > 0** sporadically (e.g. after ^C) | A stack reset event (warm boot, exception). After detection press Reset. |
| **Discard grows continuously** | Probably an OS with multi-stack discipline (CP/M). Use Stack Monitor. |


## 9. Coupling to other subsystems

| Subsystem | Coupling |
|---|---|
| **Symbols** | Sym lookup per render frame for `target`. Fully dependent on loaded `.lbl` / `.map` / `.sym` files. |
| **Stack Monitor** | A sibling raw SP view ([stack-window.md](stack-window.md)). The Callstack and Stack Monitor do not complement each other via API; the user reads them in parallel. |
| **Eventlog** | The Callstack does not emit to the eventlog. The IRQ accept / RST paths go separately through the eventlog. |
| **Breakpoints** | No direct coupling. |
| **Disassembly** | Click/Shift/Double-click on a Callstack row opens Disasm at `Call`/`Target`. |
| **Bookmarks** | Right-click -> "Add bookmark at Call/Target" - automatic symbol resolution into the bookmark name. |
| **Profiler** | A consumer of the listener API. From the `on_enter`/`on_exit` pair it computes inclusive/exclusive cycles per function. |
