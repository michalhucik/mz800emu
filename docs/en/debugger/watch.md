# Watch - user-defined memory watches

The **Watch** window lets you observe an arbitrary number of addresses
in CPU memory (or dynamically evaluated expressions) in real time. The
values are re-computed every ImGui frame; when the emulator is paused
they stay constant. Entries persist across emulator restarts (per-arch
JSON file).

## Opening the window

- **Menu Debugger -> Watch**
- **Keyboard shortcut** Alt+Shift+W (toggle)

Alt+W without Shift is taken by "Fix window aspect ratio by Width";
the Watch toggle therefore requires the Shift modifier (same as
Alt+Shift+R for CPU Registers and Alt+Shift+S for Stack Regions).

## Window layout

| Toolbar row | Contents |
|-------------|----------|
| 1 | `[+ Add]` + entry count |
| 2 | `[Save]` `[Save As...]` `[Load From...]` `[Clear All...]` + status |

Table of rows (6 columns):

| Column | Meaning |
|--------|---------|
| (drag) | Drag handle for drag-reorder |
| Name | Optional name label (double-click = inline rename) |
| Addr / Expr | Hex address (mode=Address) or shortened text of the expression (mode=Expr*) with a tooltip for the full expr + target address for deref |
| Type | Type + meta (`bit.N`, `ascii[N]`, ...) |
| Value | Live polled value (right click = format menu for int types) |
| (delete) | `x` button - immediate delete without confirmation |

## Add dialog (Mode + Type + parameters)

### Mode dropdown

| Mode | Description | Value cell behavior |
|------|-------------|---------------------|
| **Address** | Literal address + type | A byte is read from memory without side-effect according to the type |
| **Expression (scalar)** | The expression is evaluated as int32 | Value = expression result, the type determines display width/format |
| **Expression (pointer deref)** | The expression is evaluated as a uint16 address | Address = expression result, then a value is read from it according to the type (analogous to Address) |

### Address input (mode=Address)

Address input - accepts:

- C-style hex: `0xC080` / `0xc080`
- IASM hex: `0C080h` / `C080h` (suffix `h`/`H`, must start with a digit)
- Decimal: `49280`
- Symbol name (resolve via the symbol DB, case-sensitive)

### Expression input (mode=Expr*)

Expression input - the syntax is identical to the engine used for
smart breakpoint conditions. Cheat sheet below.

### Type dropdown

| Type | Description | Size | Display |
|------|-------------|------|---------|
| `u8` | Unsigned 8-bit | 1 B | dec / hex / bin / char |
| `i8` | Signed 8-bit | 1 B | dec / hex / bin |
| `u16le` / `u16be` | Unsigned 16-bit LE / BE | 2 B | dec / hex / bin |
| `i16le` / `i16be` | Signed 16-bit LE / BE | 2 B | dec / hex / bin |
| `u32le` | Unsigned 32-bit LE | 4 B | dec / hex |
| `bit` | A single bit at address.bit | 1 B | `0` / `1` |
| `ascii N` | ASCII string, N bytes | N B | string with `\xNN` escapes |
| `mzascii N` | Sharp MZ ASCII (EU), N bytes | N B | string via Sharp -> UTF-8 conversion |
| `bytes N` | Hex dump, N bytes (max 16 inline + tooltip) | N B | `12 34 56 ...` |

### Length spinner

Shown only for variable-length types (`ascii`, `mzascii`, `bytes`).
Range: 1 .. 64 for string types, 1 .. 256 for `bytes`.

### Bit index spinner

Shown only for the `bit` type. Range 0-7.

### Name (optional)

An optional text label, truncated to 63 characters. Empty = anonymous
row (shown as `(anon)`).

## Display format menu (context)

For int types (u8/i8/u16/i16/u32) a right click on the Value cell opens
the "Display format" menu with 4 options:

- `dec` - decimal (signed for `i*`, unsigned for `u*`)
- `hex` - `0xNN` / `0xNNNN` / `0xNNNNNNNN`
- `bin` - `0b10101010` (exact width by type)
- `char` - `'A'` (u8/i8 only; non-printable or multibyte = hex
  fallback)

Default format:

- Unsigned int -> `hex`
- Signed int -> `dec`

## Expression syntax (cheat sheet)

A subset most commonly used in Watch:

| Construct | Meaning |
|-----------|---------|
| `0xNN` / `#NN` | Hex literal |
| `%1010` | Binary literal |
| `NN` | Decimal literal |
| `[addr]` | Byte from memory (no side-effect) |
| `{addr}` | Word LE from memory (no side-effect) |
| `[addr]!` | In Watch the same as `[addr]` (`!` is ignored) |
| `port[N]` | I/O port byte (no side-effect) |
| `port[N]!` | In Watch the same as `port[N]` (`!` is ignored) |
| `A` `B` `C` `D` `E` `H` `L` `F` | Z80 8-bit registers |
| `BC` `DE` `HL` `AF` `IX` `IY` `SP` `PC` | 16-bit registers |
| `I` `R` `IM` `IFF1` `IFF2` | Special registers |
| `AF'` `BC'` `DE'` `HL'` | Shadow registers |
| `Cf` `Zf` `Sf` `Pf` `Hf` `Nf` | Z80 flags (0/1) |
| `Cycle` `Frame` `Scanline` | Global emu counters |
| `$name` | User var from the `$vars` panel (missing = 0) |
| `+ - * / %` | Arithmetic |
| `<< >>` | Shift |
| `< <= > >= == !=` | Relational |
| `& \| ^` | Bitwise |
| `&& \|\|` | Logical (short-circuit) |
| `~ !` | Unary |
| `min max abs if bit s8 s16 s32` | Built-in functions |

**No side effects:** Watch never changes the machine state. `[addr]`,
`{addr}` and `port[N]` are read without side effects, so Watch **does
not trigger MEM R or IORQ breakpoints** and does not read a port with a
real IN. The `!` suffix (a read with a side effect in breakpoint
conditions and actions) is ignored in Watch, as in the breakpoint Test
Eval and in the expression search of the Memory Browser. Read a port
with its side effect through a breakpoint action or MCP `emu_io_read`.

## Parse error display

If an expression fails to parse, the row is still inserted (the user
can fix it), but the Value cell shows red text
`!parse: <error>` instead of a value. Updating the expression
re-parses and clears the error.

## Persistence

Per-arch JSON file (`mz800.watch` / `mz1500.watch` / `mz700.watch`),
auto-save on exit and auto-load at startup.

Cfg section `[WATCH_PANEL]`:

- `watch_file` (TEXT) - name of the per-arch file (default
  `mz<N>.watch`)
- `auto_save` (BOOL, default 1)
- `auto_load` (BOOL, default 1)

JSON schema:

```json
{
  "watch": [
    {
      "name": "score",
      "addr": 49280,
      "bank": -1,
      "type": "u8",
      "fmt": "hex"
    },
    {
      "name": "deref_HL",
      "addr": 0,
      "bank": -1,
      "type": "u8",
      "fmt": "hex",
      "mode": "expr_deref",
      "expr": "[HL]"
    }
  ]
}
```

Missing fields (`fmt` / `length` / `bit_index` / `mode` / `expr`) are
replaced with defaults on load.

## Save / Load buttons

| Button | Action |
|--------|--------|
| **Save** | Saves into the default per-arch file without a dialog |
| **Save As...** | Opens a file dialog to choose the target path |
| **Load From...** | Opens a file dialog + confirmation (REPLACE = wipe + load) |
| **Clear All...** | Confirmation + deletes all rows |

The status message (e.g. "Saved to ..." or "Load failed: ...") is
displayed inline next to the buttons and stays until the next action.

## Usage examples

### 1. Watch a game's scratch byte

Mode `Address`, addr `0xC080`, type `u8`, fmt `hex`. The value changes
at runtime as the game writes to it. Right click on Value -> switch to
`dec` for a more human-readable score display.

### 2. Watch a 16-bit score with little-endian storage

Mode `Address`, addr `0x4080`, type `u16le`, fmt `dec`. Watch reads 2
bytes at the address and interprets them as an LE word.

### 3. Watch the byte at the address pointed to by HL

Mode `Expression (pointer deref)`, expr `HL`, type `u8`. Caveat:
`[HL]` in deref mode means a double-indirect (= from the memory at
the address `*HL` another address is taken). For a single-indirect
("byte at position HL") use a plain `HL` in deref mode.

### 4. Watch the user var `$framecnt`

Mode `Expression (scalar)`, expr `$framecnt`, type `i32` (or `u8` /
`u16le` based on range). The value = the current contents of the user
var from the `$vars` panel, useful for counters set by smart BP
actions.

### 5. Watch I/O port `0xCE` (Border/BCOL)

Mode `Expression (scalar)`, expr `port[0xCE]`, type `u8`, fmt `bin`.
We see the port's bit field without a side-effect (it does not
simulate an IORQ read).

## Coupling to other panels

- **Symbols** - resolution of symbol names in the Address input goes
  through the symbol DB.
- **Breakpoints / smart BP conditions** - they share the expression
  engine, identical constructs are used in smart BP conditions.
- **`$vars` panel** - the identifier `$name` in the expression reads a
  user var set by smart BP actions.
- **Memory Search** - right-click a candidate -> **Add to Watch** adds the
  found variable here (see [Memory Search](memory-search.md)).
