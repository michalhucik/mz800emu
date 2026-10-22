# Disassembly - section of the main debugger window and secondary windows

The **Disassembled** section in the main debug window plus 4 independent
secondary windows (**Disassembly #2 - #5**) display disassembled emulator
memory. The main instance has history and Follow PC; the secondary
windows are static debug views without auto-follow.

The main instance is officially **Disassembly #1** - the title of the
main debug window contains the suffix " - Disassembly #1" (e.g.
`MZ-800 Debugger - Disassembly #1`). The "Show in" submenu and the popup
of the clickable PC in I/O Ports History refer to it as
`Disassembly #1`.

## Section header

Instead of a simple `SeparatorText("Disassembled")` the section has a
**rich header**:

```
[Address or symbol  ] [v] Follow PC  [ ] T-states
```

### Text entry (address or symbol)

InputText (~16 chars), Enter triggers parsing:

1. The **hex parser** accepts the following formats:
   - `0x1234`, `0X1234`
   - `#1234`
   - `1234h`, `01234h`
   - bare hex `1234` (case-insensitive)

2. If hex parsing fails, a **symbol lookup** is performed in the symbol
   DB (case-sensitive). If a symbol with that name exists, its address
   is used.

3. On failure (= neither hex nor symbol): the InputText gets a **red
   background**, the buffer is not cleared (the user can correct and try
   again).

After a successful parse: focus_addr is set to the parsed address, the
buffer is cleared, focus remains on the entry for further input.

### "Follow PC" checkbox

Per-instance dynamic flag. If true and the emu is **running**, the
instance auto-jumps focus_addr to the current `PC` on every update.

| Instance | Default | Persist |
|----------|---------|---------|
| main     | ON      | yes (`disasm_main_follow_pc`) |
| #2-#5    | OFF     | yes (`disasm_extra<N>_follow_pc`) |

The UI checkbox can be toggled in both the main and secondary instances.

### "T-states" checkbox

Per-instance dynamic flag. If true, the render adds a 5th column after
MNEM with the instruction T-state count.

| Instance | Default | Persist |
|----------|---------|---------|
| main     | OFF     | yes (`disasm_main_show_tstates`) |
| #2-#5    | OFF     | yes (`disasm_extra<N>_show_tstates`) |

## Upper table - HISTORY

Rendered **only in the main instance** (the secondary windows do not have
it).

Data source: a ring buffer with 32 records of the most recently executed
instructions. Each record holds the address + up to 4 bytes from
execution time (= a snapshot at the moment of CPU fetch, not a live read
across a banking switch).

The columns are identical to the lower table: ICONS / ADDR / BYTES /
MNEM (+ TSTATES if the per-instance toggle is ON).

The splitter between history and the lower table is drag-resizable (the
ratio is remembered for the session).

### Recording mode (CPU Instruction History)

When the history ring buffer is filled is controlled by the `CPU
Instruction History` choice (a radio-button submenu in the debug window
menu). It has four modes:

| Mode | When the history is recorded |
|------|------------------------------|
| `Off` | Never - the history is not recorded (no overhead). |
| `Only With Debug Window` | Only while the debug window is open. |
| `With Debug Window or Breakpoints` (default) | While the debug window is open **or** at least one breakpoint is enabled. |
| `Always` | Continuously, regardless of window and breakpoint state. |

The `With Debug Window or Breakpoints` mode is **now the default**
(previously the default was `Only With Debug Window`). Rationale: the
history is then available even without an open window at the moment a
breakpoint stops execution - you can inspect the last 32 instructions
that preceded the hit. `Off` is respected even with enabled breakpoints
(= an explicit way to disable the history).

**Persistence:** ini module `DEBUGGER`, key `cpuhist_mode` (KEYWORD),
values `OFF` / `WITH_WINDOW` / `WITH_WINDOW_OR_BP` / `ALWAYS`, default
`WITH_WINDOW_OR_BP`.

Note: this is the informational instruction history (the last 32
instructions). For full tracking logs with timestamps and a memory dump
see the cputrack subsystem (Debugger Settings -> Trace Suite),
described in [formats/CPU-track_format.md](formats/CPU-track_format.md).

## Lower table - DISASSEMBLY

The main work area. Renders disassembled instructions from `focus_addr`,
the number of rows depending on the section height.

### ICONS column (gutter)

Holds branch arrows (left) and BPT + PC icons (right).

#### Branch arrows (only if `disasm_show_branch_arrows` is ON)

For each JR / JP / CALL / DJNZ / RST instruction with a **fixed target**
a vertical arrow is drawn from source to target (if the target is
visible). Max 4 parallel arrows at once.

Target state:

- **Target visible in window** -> full arrow connecting rows
- **Target outside window (higher address)** -> stub arrow downwards
- **Target outside window (lower address)** -> stub arrow upwards
- **Target == source** (`JR -2` / `JR $`) -> mini-loop glyph (tight
  loop)

Colors:

- **CALL** -> blue
- **JP / JR / DJNZ / RST** -> gray
- **Hover** (mouse over source or target row) -> yellow

Branch arrows are not implemented for `JP (HL)`, `JP (IX)`, `JP (IY)`,
`RET`, `RETI`, `RETN` (= no fixed target).

#### BPT and PC icons

**BPT (left slot):**

| State | Symbol |
|-------|--------|
| Active BPT on the starting address | red filled circle |
| Disabled BPT on the starting address | white empty circle |
| Active BPT inside the instruction (= addr+1..addr+len-1) | yellow filled circle |
| Disabled BPT inside the instruction | yellow empty circle |

**PC indicator (right slot):**

| State | Symbol |
|-------|--------|
| `PC == addr` | green triangle |
| `PC` inside the instruction (mid-instruction) | yellow arrow |

#### Code coverage mark

While code coverage data exist, a colored mark to the right of the PC
slot shows the coverage of the address: green = executed (or a
conditional instruction with both results), yellow = condition always
met, orange = condition never met, no mark = not executed. The gutter is
6 px wider while the marks are shown. The row tooltip gets a Coverage
section. Turned off in `Settings` -> `Disassembled` -> `Show code
coverage` (cfg key `disasm_show_coverage`). See
[code-coverage.md](code-coverage.md).

### ADDR column (5 characters)

Instruction address. Two variants:

- If a symbol exists for this address: the **symbol name** is shown.
- Otherwise: hex `XXXX:`.

**PC highlighting** is applied **only to this column**:

- `PC == addr` -> **green text**
- `PC` inside the instruction -> **yellow text**

The other columns of the row (BYTES, MNEM, TSTATES) keep the default
color.

### BYTES column (12 characters = `XX XX XX XX`)

**2-tone coloring:**

- **Opcode bytes** (= prefix CB/DD/ED/FD + main opcode) -> default color
- **Operand bytes** (= immediate values, displacement) -> **cyan**

The number of operand bytes is computed from the operand types:

| Operand type | Operand bytes |
|--------------|---------------|
| 8-bit immediate, 8-bit relative offset, IX/IY displacement | 1 |
| 16-bit immediate, 16-bit MEM | 2 |
| 8/16-bit register, condition, bit index, RST vector | 0 |

Edge case: the `DD CB d op` / `FD CB d op` instructions (4 bytes) have
the displacement in the middle of the sequence, not at the end. For
simplicity all 4 bytes are drawn in the default color in this case.

### MNEM column (15 characters)

**Categorical coloring of the mnemonic** (= the first word) by
instruction type:

| Category | Mnemonics | Color |
|----------|-----------|-------|
| **Flow** | jp, jr, call, ret, reti, retn, rst, djnz, halt | **orange** |
| **Stack** | push, pop | light blue |
| **Block** | ldi, ldd, ldir, lddr, cpi, cpd, cpir, cpdr | pink |
| **I/O** | in, out, ini, ind, inir, indr, outi, outd, otir, otdr | magenta |
| **Arith** | add, adc, sub, sbc, inc, dec, neg, cp, daa, and, or, xor, cpl, scf, ccf | yellow |
| **Bit** | bit, set, res, rlc, rl, rrc, rr, sla, sra, sll, srl, rld, rrd, rlca, rla, rrca, rra | cyan |
| **CPU control** | nop, di, ei, im | light gray |
| **Default** | ld, ex, exx, others | white (default text) |

**Operands** (= the rest of the mnemonic after the first space) are
tokenized by separators (`,`, `(`, `)`, ` `, `+`, `-`) and each token is
classified:

| Token | Classification | Color |
|-------|----------------|-------|
| `#1234`, `$FF`, `0x12`, `1234h` | hex literal | **cyan** |
| Pure decimal (`123`) | number | **cyan** |
| Register (a, b, ..., bc, hl, sp, ix, iy, ixh/l, iyh/l, af', bc', de', hl') | register | default |
| Condition (nz, z, nc, c, po, pe, p, m) | condition | default |
| Anything else (= identifier) | symbol | **cyan** |

Separators keep the default color.

Examples:

- `ld bc,#0000` -> `ld` default, `bc,` default, `#0000` cyan
- `call MAIN_LOOP` -> `call` orange, `MAIN_LOOP` cyan (= symbol)
- `jp z,#1234` -> `jp` orange, `z,` default, `#1234` cyan
- `ld (ix+5),#7` -> `ld` default, `(ix+5),` default, `#7` cyan

### TSTATES column (optional, ~5 characters)

Activated by the per-instance "T-states" checkbox in the header. Format:

- Unconditional instructions -> single number, e.g. `4`
- Conditional branches -> `taken/not`, e.g. `17/10` (CALL cc), `12/7`
  (JR cc), `13/8` (DJNZ), `21/16` (LDIR)

## Right-click context menu

Invoked by right-clicking a row in the lower table. Item order:

1. **Set/Remove Breakpoint** - toggle BPT at the row address
2. **Enable/Disable Breakpoint** - enable/disable an existing BPT
3. **Set as PC** - jump the emulator to this row (= write `PC = addr`)
4. **Focus to <target>** - only for a branch with a fixed target; the
   label contains the target address + symbol if it exists, e.g.
   `Focus to 0xE6DA (CALC_LOOP)`
5. **Focus to...** - opens a dialog to enter an address/symbol
6. **Focus to PC** - jump the lower table to the current `PC`
7. **Focus to register** - submenu with 11 items:
   - Primary pairs: AF, BC, DE, HL
   - Shadow pairs: AF', BC', DE', HL' (separated by a separator)
   - Index + SP: IX, IY, SP
   - Label: `BC = 0x1234 (SYMBOL)` (if a symbol exists for the value)
8. **Show in** - submenu with 5 items "Disassembly #1..#5":
   - #1 = main, #2-5 = secondary
   - Click ensures the window is open + sets focus to the row address
   - The current instance is disabled in the menu
9. **Add to bookmarks** - adds a bookmark for the row address:
   - User input = symbol name if it exists, otherwise `#XXXX` hex
   - Comment empty (user fills it in the Bookmarks window)
   - Opens the Bookmarks window if it was closed
10. **Edit row** - opens the Inline Assembler (modal dialog) to edit the
    instruction on the row

The right-click is available **even while the emu is running**.

## Auto-disable Follow PC on interaction

So that the user can manually drive focus while the emu runs, there are
defined situations in which the per-instance `follow_pc` flag is
automatically turned off (= permanently, persists):

| Action | Behavior |
|--------|----------|
| **LMB click on a row** | If `follow_pc && !is_paused` -> turn follow_pc off |
| **RMB click on a row** (before popup) | Same |
| **Focus to ...** in context menu | Same (auto-disable) |
| **Focus to PC** | Same |
| **Focus to register** (11 items) | Same |
| **Focus to <target>** | Same |
| **Add form Enter** (text entry) | Same |
| **Focus to... dialog Apply** | Same |
| **Bookmarks LMB / Show in** | Same |

Only in the paused state is `follow_pc` preserved (= the click is benign,
focus is not affected).

### Slider drag bypass (temporary, does not change the flag)

Special behavior for the vertical slider 0x0000..0xFFFF:

- If the user **holds LMB** on the slider and `follow_pc=ON` and the emu
  is running:
  - **Bypass** the Follow PC update **for the duration of the hold** (=
    focus_addr is not overwritten with PC)
  - The user can "peek" elsewhere without the window immediately
    snapping back to PC
- On **button release**:
  - The bypass ends, follow_pc again actively drives focus_addr
  - On the next frame focus_addr returns to the current PC

Unlike the other actions (click, menu), the slider drag **does not
change** the flag - after release Follow PC returns to normal.

## Per-row tooltip

Hovering on a row for ~500 ms shows a unified tooltip with these sections
(sections without data are left out):

### 1. Instruction info

```
E6CA: CD DA E6  call #e6da     <- header (bold, yellow)
CALL nn: push PC, PC = nn      <- description from instruction metadata
T-states: 17                    <- t_states / t_states2
Flags: S Z F5 H F3 P/V N C     <- labels, affected ones green
```

If no description is available for the instruction, it is omitted.

T-states: a single number for unconditional instructions, otherwise
`taken/not`.

Flags: 8 labels `S Z F5 H F3 P/V N C`. Each label is **green** if the
flag is affected by the instruction, default color otherwise.

### 2. Symbol info for the row address

Only if a symbol exists for the row address:

```
Symbol: CALC_LOOP
Address: 0xE6CA
Comment: main computation loop  <- if present
Module: main.asm                 <- if present (from a .map import)
```

### 3. Symbol info for the operands

Only if there is a fixed target (CALL/JP/JR/...) or a direct MEM access:

```
Target: 0xE6DA -> CALC_HELPER
Mem: (0x1234) -> SCREEN_BUFFER
```

### Code coverage

Only while the coverage marks are shown: the state of the address and, at
the detail level, the code versions at the address (cell, key bytes,
executions, condition met / not met). See [code-coverage.md](code-coverage.md).

### 4. Edit hints

Dimmed gray text:

```
Double-click a row or start typing to open the inline assembler.
Right-click for context menu.
```

### Snapshot strategy

On the first hover of a row, the row data is copied into a per-instance
snapshot buffer. The tooltip stays stable for the entire hover until the
user changes the hovered index.

The reason: while the emu is running, the cached disasm rows are
rewritten roughly every ~100 ms. Without a snapshot the tooltip would
flicker with updated data (mnemonic / T-states / flags would change at
the refresh tick rate). The snapshot captures the row
deterministically.

## Secondary Disassembly windows #2 - #5

Activated through the top menu **Debugger -> Other disassembly ->
Disassembly #N**. Default closed. Position and size are remembered via
`imgui.ini`, visibility is not serialized.

Each window hosts a separate view instance created without the upper
history table. Lazy create on first open, lazy destroy on close.

### Per-instance state

| State | Persistence | Cfg key |
|-------|-------------|---------|
| focus_addr | yes | `disasm_extra<N>_focus` |
| follow_pc | yes | `disasm_extra<N>_follow_pc` |
| show_tstates | yes | `disasm_extra<N>_show_tstates` |
| selected_row | no | - |
| visible_rows | no (= per current window height) | - |
| Window position/size | yes | imgui.ini |

### Shared global state

- Memory snapshot (cache rebuild reads the current banking)
- Symbol DB
- Breakpoints (a BPT in any window shows up in all)
- Global toggle for branch arrows
- Refresh tick

## Keyboard navigation

Active only on the selected (focused) row:

| Key | Action |
|-----|--------|
| Up / Down arrow | move selection by one row |
| PgUp / PgDown | move selection by one page |
| Enter | open the Inline Assembler (same as double-click) |
| Home | focus_addr = 0x0000 |
| End | focus_addr = 0xFFF0 |

The slider on the right (vertical 0x0000..0xFFFF) drives `focus_addr`.
Drag = jump to position, mouse wheel = scroll per instruction up/down.

## Persistence (summary of cfg keys)

Module `DEBUGGER`:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `disasm_show_branch_arrows` | bool | true | Global toggle for branch arrows |
| `disasm_show_coverage` | bool | true | Global toggle for code coverage marks |
| `disasm_main_focus_addr` | uint16 | 0 | Main window focus |
| `disasm_main_follow_pc` | bool | true | Main window Follow PC |
| `disasm_main_show_tstates` | bool | false | Main window T-states column |
| `disasm_extra<N>_focus` | uint16 | 0 | Secondary window N (2..5) focus |
| `disasm_extra<N>_follow_pc` | bool | false | Secondary window N Follow PC |
| `disasm_extra<N>_show_tstates` | bool | false | Secondary window N T-states |
| `cpuhist_mode` | KEYWORD | `WITH_WINDOW_OR_BP` | When to fill the history ring buffer (see Upper table - HISTORY) |

Layout (position / size / split ratio) is handled by ImGui itself via
`imgui.ini`.

## Known limitations

1. **DDCB/FDCB byte coloring** - 4-byte instructions with displacement
   in the middle have all bytes in the default color (= rare
   instructions, not covered).

2. **Race UI <-> emu thread on banking switch** - the cache rebuild
   reads 100 bytes in a loop. Between bytes the emu thread may switch
   the mapping (e.g. ROM_E000 <-> PROHIBITED) -> the buffer contains
   mixed bytes from different bank states. Disassembly from the buffer
   then returns a consistent result within a single rebuild tick, but
   **between rebuilds** (= ~100 ms apart) the result can differ.

3. **Mapped ports at 0xE000-0xE008 in MZ-700 mode** - PIO 8255 and GDG
   memop for DMD status do not honor the debug flag, so a debug read at
   these addresses modifies the chip state. CTC8253 does honor the flag.
   For a disasm scan at 0xE6D0+ (= rom_e000_efff, addr_low > 0x0F) the
   side effect is not triggered.

4. **The Inline Assembler is modal** - if the user wants to browse the
   IASM Help, it must be opened before opening the IASM dialog.

5. **Disassembly #1 = main** - in the "Show in" submenu item #1 targets
   the main debug window. If the main window is closed, a click opens
   it.

## Links to other panels

- **Breakpoints** - BPT icons in the ICONS gutter, context menu for
  set/remove/enable/disable. Details in
  [breakpoints/README.md](breakpoints/README.md).
- **Symbols** - resolves symbol names in the header text entry, ADDR
  column, MNEM operand tokenization, "Add to bookmarks" context menu.
  Details in [symbols.md](symbols.md).
- **Bookmarks** - context menu "Add to bookmarks" + Bookmarks LMB
  returns focus back to the main instance. Details in
  [bookmarks.md](bookmarks.md).
- **I/O Ports History** - the clickable PC in the Selected Event panel
  opens disasm in the chosen slot. Details in
  [io-ports.md](io-ports.md).
- **Callstack** - clicking a Callstack row opens disasm at the `Call`
  or `Target` address.
