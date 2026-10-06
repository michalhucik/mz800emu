# Sharp MZ-800 Keyboard

**Status:** reference (verified against ROM source and live emulator via MCP).
**Purpose:** Reference for AI agents driving the emulator over the MCP server -
how the keyboard is laid out, what each key does, and how to type special
characters (Sharp graphics, semigraphics). Other platforms (MZ-700, MZ-1500)
are described only as a diff against this document.

---

## 1. Physical layout

The MZ-800 keyboard has 5 main rows (numbered 0-5). Row 0 holds a separate block
of function keys F1-F5 on the left and two separate special keys INST and DEL.
A separate cursor block sits to the right of the main keyboard.

Each key is referred to by its **primary name** (what is printed on it; see the
table in section 2). `UP_ARROW` / `DOWN_ARROW` are the **character** arrow keys
(they emit the up/down arrow glyphs), NOT cursor movement - the cursor keys are
`CURSOR_UP/DOWN/LEFT/RIGHT`.

Layout (approximate, per the emulator Virtual Keyboard window):

```
 [F1][F2][F3][F4][F5]                              [INST][DEL]

 [GRAPH][1][2][3][4][5][6][7][8][9][0][-][UP_ARROW][\][ESC]
 [ TAB  ][Q][W][E][R][T][Y][U][I][O][P][@][[][DOWN_ARROW][BLANK]
 [ CTRL  ][A][S][D][F][G][H][J][K][L][;][:][]][ CR ]            [CURSOR_UP]
 [SHIFT][ALPHA][Z][X][C][V][B][N][M][,][.][/][?][SHIFT]  [CURSOR_LEFT][CURSOR_RIGHT]
              [           SPACE           ]                    [CURSOR_DOWN]
```

Note: there are physically two SHIFT keys (left + right) in row 4, but both are
scanned on the same matrix bit and are indistinguishable.

### Key categories and colors

- **Normal keys** - white keycap, black label (letters, digits, punctuation).
- **Special keys** - gray keycap. Label is black, **except F1-F5 which have a
  blue label**.

Special (gray) keys: `F1-F5`, `INST`, `DEL`, `GRAPH`, `TAB`, `CTRL`, `SHIFT`,
`ALPHA`, `ESC`, `BLANK`, `CR`, `CURSOR_UP/LEFT/RIGHT/DOWN`. All other keys
(including `UP_ARROW`, `DOWN_ARROW`, `@`, `[`, `]`, `\`, `;`, `:`, `,`, `.`,
`/`, `?`, `-`) are normal (white) keys.

---

## 2. Key overview (primary name + matrix + characters)

Central keyboard table. Each row is one physical key: its primary name and
position in the keyboard matrix (column = value written to 8255 PA0-3 during
scanning, 0-9; bit = position in the read PB byte, 0-7).

For each modifier state (base / SHIFT / GRAPH / GRAPH+SHIFT) there are three
columns: **ASCII** = HEX Sharp MZ ASCII code the key produces (output of ROM
`@GETKY`); **disp** = corresponding MZ display code (from ROM tables
`!KBD`/`!KBDS`/`!KBDG`/`!KBDGS` at 0xBEA/0xC2A/0xC6A/0xCE9); **desc** = standard
ASCII character, or a description, or blank (graphic/semigraphic glyph with no
plain ASCII equivalent). `--` = key produces no character (modifiers, F1-F5;
TAB is ignored by the ROM). Verified via MCP (`@GETKD`/`@GETKY`).

| Key | Col | Bit | base ASCII | disp | desc | SHIFT ASCII | disp | desc | GRAPH ASCII | disp | desc | G+SH ASCII | disp | desc |
|---|:--:|:--:|:--:|:--:|---|:--:|:--:|---|:--:|:--:|---|:--:|:--:|---|
| F1 | 9 | 7 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| F2 | 9 | 6 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| F3 | 9 | 5 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| F4 | 9 | 4 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| F5 | 9 | 3 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| INST | 7 | 7 | 61 | C8 | INSERT | 16 | C6 | CLS | 61 | C8 | INSERT | 16 | C6 | CLS |
| DEL | 7 | 6 | 60 | C7 | DELETE | 15 | C5 | HOME | 60 | C7 | DELETE | 15 | C5 | HOME |
| GRAPH | 0 | 6 | 63 | CA | SET GRAPHIC | 63 | CA | SET GRAPHIC | -- | -- |  | -- | -- |  |
| 1 | 5 | 7 | 31 | 21 | 1 | 21 | 61 | ! | D4 | 37 | left bar (2 px) | CA | 3F | right bar (2 px) |
| 2 | 5 | 6 | 32 | 22 | 2 | 22 | 62 | " | CF | 3E | bottom bar (2 px) | D7 | 36 | top bar (2 px) |
| 3 | 5 | 5 | 33 | 23 | 3 | 23 | 63 | # | D6 | 7F | right bar (3 px) | D9 | 7E | bottom bar (3 px) |
| 4 | 5 | 4 | 34 | 24 | 4 | 24 | 64 | $ | D5 | 7B | left half (filled) | C1 | 3B | right half (filled) |
| 5 | 5 | 3 | 35 | 25 | 5 | 25 | 65 | % | C2 | 3A | bottom half (filled) | D8 | 7A | top half (filled) |
| 6 | 5 | 2 | 36 | 26 | 6 | 26 | 66 | & | D3 | 5E | approx. line junction | CB | 1E | approx. T-junction |
| 7 | 5 | 1 | 37 | 27 | 7 | 27 | 67 | ' | D1 | 1F | approx. T-junction | D2 | 5F | approx. T-junction |
| 8 | 5 | 0 | 38 | 28 | 8 | 28 | 68 | ( | 81 | BD | cross (+) | AC | A2 | approx. two vertical lines |
| 9 | 6 | 2 | 39 | 29 | 9 | 29 | 69 | ) | A7 | A1 | approx. two horizontal lines | 91 | A3 | grid (#) |
| 0 | 6 | 3 | 30 | 20 | 0 | FF | 60 | π | 99 | 9C | approx. short diagonal | 82 | 9D | approx. diagonal (step) |
| - | 6 | 5 | 2D | 2A | - | 3D | 2B | = | 72 | D2 |  | 71 | D1 |  |
| UP_ARROW | 6 | 6 | 5E | 50 | ↑ | 94 | A5 | ~ | 87 | 9E | approx. diagonal (step) | 8C | 9F | approx. short diagonal |
| \ | 6 | 7 | 5C | 59 | \ | 80 | 80 | } | 74 | D4 |  | 73 | D3 |  |
| ESC | 8 | 7 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| TAB | 0 | 3 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| Q | 2 | 7 | 51 | 11 | Q | A0 | 91 | q | C4 | 3C | horizontal line (row 7, bottom) | F2 | 7C | horizontal line (row 6) |
| W | 2 | 1 | 57 | 17 | W | A3 | 97 | w | E6 | 38 | horizontal line (row 5) | E0 | 78 | horizontal line (row 4) |
| E | 4 | 3 | 45 | 05 | E | 92 | 85 | e | E3 | 34 | horizontal line (row 3) | E4 | 74 | horizontal line (row 2) |
| R | 2 | 6 | 52 | 12 | R | 9D | 92 | r | E5 | 30 | horizontal line (row 1) | C3 | 70 | horizontal line (row 0, top) |
| T | 2 | 4 | 54 | 14 | T | 96 | 94 | t | C5 | 71 | vertical line (column 0, left) | F4 | 31 | vertical line (column 1) |
| Y | 1 | 7 | 59 | 19 | Y | BD | 99 | y | E7 | 75 | vertical line (column 2) | E2 | 35 | vertical line (column 3) |
| U | 2 | 3 | 55 | 15 | U | A5 | 95 | u | FD | 79 | vertical line (column 4, center) | E8 | 39 | vertical line (column 5) |
| I | 3 | 7 | 49 | 09 | I | A6 | 89 | i | F9 | 7D | vertical line (column 6) | C7 | 3D | vertical line (column 7, right) |
| O | 3 | 1 | 4F | 0F | O | B7 | 8F | o | B1 | B0 | approx. diagonal line | 75 | D5 |  |
| P | 3 | 0 | 50 | 10 | P | 9E | 90 | p | 76 | D6 |  | 83 | B1 | approx. diagonal line |
| @ | 1 | 5 | 40 | 55 | @ | 93 | A4 | approx. short diagonal | 88 | B2 | approx. diagonal line | 77 | D7 |  |
| [ | 1 | 4 | 5B | 52 | [ | BE | BC | { | 78 | D8 |  | 8D | B3 | approx. diagonal line |
| DOWN_ARROW | 0 | 5 | FC | 58 | ↓ | FB | 1B | £ | 6C | E5 |  | 68 | CF | face (smiley) |
| BLANK | 0 | 7 | 90 | BF | _ | 90 | BF | _ | 90 | BF | _ | 90 | BF | _ |
| CTRL | 8 | 6 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| A | 4 | 7 | 41 | 01 | A | A1 | 81 | a | F3 | 53 | ♡ | F8 | 46 | ♧ |
| S | 2 | 5 | 53 | 13 | S | A4 | 93 | s | FA | 44 | ♢ | E1 | 41 | ♤ |
| D | 4 | 4 | 44 | 04 | D | 9C | 84 | d | F1 | 47 | filled circle | F7 | 48 | ring (empty circle) |
| F | 4 | 2 | 46 | 06 | F | AA | 86 | f | CC | 4A | frame (square) | C8 | 43 | full block |
| G | 4 | 1 | 47 | 07 | G | 97 | 87 | g | DB | 4B | approx. short diagonal | DC | 4C | approx. short diagonal |
| H | 4 | 0 | 48 | 08 | H | 98 | 88 | h | EF | 72 | approx. corner (top + left vertical) | F0 | 73 | approx. corner (top + right vertical) |
| J | 3 | 6 | 4A | 0A | J | AF | 8A | j | D0 | 5C | approx. corner (small) | CE | 5D | approx. corner (small) |
| K | 3 | 5 | 4B | 0B | K | A9 | 8B | k | DF | 5B | approx. checkerboard blocks | DE | 6C | approx. checkerboard blocks |
| L | 3 | 4 | 4C | 0C | L | B8 | 8C | l | 86 | B4 | approx. diagonal line | C9 | 56 | triangle (diagonal) |
| ; | 0 | 2 | 3B | 2C | ; | 2B | 6A | + | FE | 42 | triangle (diagonal) | 84 | B5 | approx. diagonal line |
| : | 0 | 1 | 3A | 4F | : | 2A | 6B | * | 89 | B6 | approx. diagonal line | E9 | 4D | triangle (diagonal) |
| ] | 1 | 3 | 5D | 54 | ] | C0 | 40 | \ | | F5 | 4E |  | 8E | B7 |  |
| CR | 0 | 0 | 66 | CD | CR | 66 | CD | CR | 66 | CD | CR | 66 | CD | CR |
| SHIFT | 8 | 0 | -- | -- |  | -- | -- |  | -- | -- |  | -- | -- |  |
| ALPHA | 0 | 4 | 62 | C9 | SET ALPHANUM | 62 | C9 | SET ALPHANUM | 62 | C9 | SET ALPHANUM | 62 | C9 | SET ALPHANUM |
| Z | 1 | 6 | 5A | 1A | Z | A2 | 9A | z | EE | 76 | diagonal line (/) | ED | 77 | diagonal line (\) |
| X | 2 | 0 | 58 | 18 | X | 9B | 98 | x | F6 | 6D | approx. hourglass (X) | 7D | DD | approx. diagonal (step) |
| C | 4 | 5 | 43 | 03 | C | 9F | 83 | c | 7E | DE | approx. diagonal (step) | 79 | D9 | approx. diagonal (step) |
| V | 2 | 2 | 56 | 16 | V | AB | 96 | v | 7A | DA | approx. diagonal (step) | 95 | A6 |  |
| B | 4 | 6 | 42 | 02 | B | 9A | 82 | b | EA | 6F | approx. short diagonal | EB | 6E | approx. short diagonal |
| N | 3 | 2 | 4E | 0E | N | B0 | 8E | n | EC | 32 | approx. corner (left vertical + bottom) | DA | 33 | approx. corner (right vertical + bottom) |
| M | 3 | 3 | 4D | 0D | M | B3 | 8D | m | CD | 1C | approx. corner/junction | DD | 1D | approx. corner/junction |
| , | 6 | 1 | 2C | 2F | , | 3C | 51 | < | 63 | CA | SET GRAPHIC | 70 | D0 |  |
| . | 6 | 0 | 2E | 2E | . | 3E | 57 | > | BF | B8 | approx. short diagonal | 85 | B9 | approx. diagonal (step) |
| / | 7 | 0 | 2F | 2D | / | 5F | 45 | ← | 7B | DB | approx. small ring | 8B | BE | ^ |
| ? | 7 | 1 | 3F | 49 | ? | C6 | 5A | → | 8A | BA | approx. diagonal (step) | 8F | BB | approx. diagonal |
| SPACE | 6 | 4 | 20 | 00 | blank (all off) | 20 | 00 | blank (all off) | 20 | 00 | blank (all off) | 20 | 00 | blank (all off) |
| CURSOR_UP | 7 | 5 | 12 | C2 | CURSOR UP | 12 | C2 | CURSOR UP | 12 | C2 | CURSOR UP | 12 | C2 | CURSOR UP |
| CURSOR_LEFT | 7 | 2 | 14 | C4 | CURSOR LEFT | 14 | C4 | CURSOR LEFT | 14 | C4 | CURSOR LEFT | 14 | C4 | CURSOR LEFT |
| CURSOR_RIGHT | 7 | 3 | 13 | C3 | CURSOR RIGHT | 13 | C3 | CURSOR RIGHT | 13 | C3 | CURSOR RIGHT | 13 | C3 | CURSOR RIGHT |
| CURSOR_DOWN | 7 | 4 | 11 | C1 | CURSOR DOWN | 11 | C1 | CURSOR DOWN | 11 | C1 | CURSOR DOWN | 11 | C1 | CURSOR DOWN |

Note: Sharp ASCII values `0x10`-`0x16` and `0x60`-`0x66` are not printable
characters but **DPCT control codes** (CURSOR_*, HOME, CLS/SCROLL, INSERT,
DELETE, SET ALPHANUM/GRAPHIC, CR). So INST/DEL/GRAPH/ALPHA/CR and the cursor
keys emit a control command, not a glyph - see the desc column.

Note: `SHIFT` has both left and right keys at the same position (8, 0). Column 1
bits 0-2, column 8 bits 1-5 and column 9 bits 0-2 are unpopulated.

---

## 3. Modifier and special keys

| Key | Function |
|-----|----------|
| `SHIFT` | Modifier - upper glyph of a key / uppercase letters. Two keys (left + right), functionally identical (same matrix bit). |
| `CTRL` | Modifier - control codes (Ctrl+key). |
| `GRAPH` | In MZ-800 ROM and MZ-800 BASIC = switch into graphic (semigraphic) mode. Emits DPCT SET GRAPHIC (display 0xCA, see table). Graphic mode is visually indicated by a **full cursor split evenly by a cross into 4 quadrants** (display code **0xFF**). |
| `ALPHA` | In MZ-800 ROM and MZ-800 BASIC = return to normal (alphanumeric) mode. Emits DPCT SET ALPHANUM (display 0xC9). |
| `CR` | Carriage Return (Enter) - confirm line. |
| `ESC` / `BREAK` | Tap = soft Escape (occasionally, rarely used). **SHIFT+ESC** = hard Break (interrupt the running program). |
| `INST` | Insert - insert a character. |
| `DEL` | Delete - delete a character. |
| `F1`-`F5` | Function keys. In the ROM they have **no meaning** (not read as a character). In BASIC / CP/M they act as programmable macros, extended to F6-F10 via SHIFT; newer systems add combinations with further modifiers. Some programs occasionally give them a special function, but they usually stay unused. |
| `TAB` | Tab. (Note: MZ-700/1500 do NOT have this key - see the diff document.) |

GRAPH/ALPHA relate to the DPCT control codes `SET GRAPHIC MODE` (display 0xCA)
and `SET ALPHANUMERIC MODE` (display 0xC9).

**Important - valid only in ROM and BASIC:** GRAPH, ALPHA and keyboard mode
switching (and the graphic/alphanumeric mode in general) work **only in the ROM
monitor and MZ-800 BASIC**. In the CP/M operating system and other user
programs there was usually never a reason to implement them - they are either
**ignored** or have a completely different, **program-specific meaning**.

---

## 4. Character layers (base / SHIFT / GRAPH / G+SH)

Each key can carry several characters depending on the modifier (all four layers
are listed in the table in section 2):

- **base** - the key alone
- **SHIFT + key** - upper symbol / uppercase letter
- **GRAPH + key** - graphic / semigraphic glyph (graphic mode)
- **GRAPH+SHIFT + key** - additional semigraphic glyph

Example (verified via MCP): key `DOWN_ARROW` (column 0, bit 5) emits a down arrow
on base and `£` (pound) with SHIFT - hence the MCP name `LIBRA` has
`needs_shift=true`.

---

## 5. Keyboard matrix (HW)

10 columns x 8 bits, scanned via 8255 PIO (PA0-3 select column, read PB,
**active = 0**). Addresses: 800 mode IORQ `0xD0` (PA) / `0xD1` (PB); 700 mode
memory-mapped `0xE000` / `0xE001`.

| Bit \ Col | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | CR | | X | P | H | 8 | . | / | SHIFT | |
| 1 | : | | W | O | G | 7 | , | ? | | |
| 2 | ; | | V | N | F | 6 | 9 | CURSOR_LEFT | | |
| 3 | TAB | ] | U | M | E | 5 | 0 | CURSOR_RIGHT | | F5 |
| 4 | ALPHA | [ | T | L | D | 4 | SPACE | CURSOR_DOWN | | F4 |
| 5 | DOWN_ARROW | @ | S | K | C | 3 | - | CURSOR_UP | | F3 |
| 6 | GRAPH | Z | R | J | B | 2 | UP_ARROW | DEL | CTRL | F2 |
| 7 | BLANK | Y | Q | I | A | 1 | \ | INST | BREAK | F1 |

Note: `CURSOR_*` in column 7 are cursor arrows (movement). `UP_ARROW` (col 6,
bit 6) and `DOWN_ARROW` (col 0, bit 5) are character keys (arrow glyphs, not
cursor movement).

---

## 6. Key names for MCP key injection

The agent sends keys via the MCP tools `emu_input_send_key`, `emu_input_send_keys`
(`encoding=key_names`), `emu_input_press_key`, `emu_input_release_key` and
`emu_input_send_keys_with_delays`. **All of them use ONE shared key-name table**
(one resolver in the emulator), so a name valid in one tool is valid in all of
them. Names are case-insensitive; an unknown name is rejected with
`Unknown key '<name>'. Closest valid names: ...`. Supported symbolic names:

| Name(s) | Key |
|---------|-----|
| `BLANK` | BLANK (special) |
| `GRAPH` | GRAPH |
| `LIBRA` | `£` (= SHIFT + DOWN_ARROW) |
| `ALPHA` | ALPHA |
| `TAB` | TAB (MZ-800 only) |
| `RETURN`, `ENTER`, `CR` | CR (Enter) |
| `SPACE` | space |
| `INSERT`, `INS` | INST |
| `DELETE`, `DEL`, `BACKSPACE` | DEL |
| `ARROW_UP`/`UP`/`CURSOR_UP`, `ARROW_DOWN`/`DOWN`/`CURSOR_DOWN`, `ARROW_LEFT`/`LEFT`/`CURSOR_LEFT`, `ARROW_RIGHT`/`RIGHT`/`CURSOR_RIGHT` | CURSOR_UP/DOWN/LEFT/RIGHT (cursor movement keys, col 7) |
| `INST` (alias of `INSERT`, `INS`) | INST |
| `UP_ARROW`, `DOWN_ARROW` | the arrow-glyph character keys (col 6 bit 6, col 0 bit 5) - NOT cursor movement, so `UP_ARROW` is not `ARROW_UP`; `DOWN_ARROW` shares its position with `F9` |
| `ESC`, `ESCAPE`, `BREAK`, `END` | ESC / BREAK |
| `CTRL`, `CONTROL` | CTRL |
| `SHIFT` | SHIFT |
| `F1`-`F5` | function keys |
| `F6`-`F9` | aliases of other keys: `F6` = `@`, `F7` = `\`, `F8` = `?`, `F9` = LIBRA position without SHIFT |

Single ASCII characters can also be sent (`{"key":"A"}` or `{"key":"ASCII:@"}`);
they are translated to the (column, bit, shift) matrix position automatically.

Multi-key typing uses `emu_input_send_keys` (`encoding` = `ascii` or `key_names`;
with `key_names` the text is a JSON array of the names above, and an unknown
name fails the whole call before any key is sent).

The primary names of the section 2 table (`CURSOR_*`, `INST`, `DEL`, `CR`, `ESC`,
`TAB`, `SPACE`, `F1`-`F5`, `SHIFT`, `CTRL`, `GRAPH`, `ALPHA`, `BLANK`,
`UP_ARROW`, `DOWN_ARROW`) are all accepted as written. Older names (`UP`,
`ARROW_RIGHT`, `RETURN`, `INSERT`, ...) keep working as aliases.

**Landing readback.** A successful response means the host-side
press/hold/release injection into the virtual keyboard matrix happened. Whether
the guest actually read the key is reported by the `landing_verified` field
(and `keys_landed` for `emu_input_send_keys`): while the key is held the emulator
watches the PPI Port B reads, and `landing_verified` is `true` only if the guest
scanned the key's column during the hold (i.e. it read the injected key), `false`
otherwise. A running program with the ROM unmapped may scan only a narrow subset
of keyboard columns and not read keys aimed at other columns - that now shows as
`landing_verified: false` (no more silent swallow). If a key does not land, hold
it longer (`frames`), make sure the guest is at a prompt that scans the keyboard,
or reset the emulator (which remaps the ROM monitor with a full keyboard scan).

---

## 7. Typing special characters (recipes)

All values verified via MCP. To type a character, press the key in the given
modifier state (e.g. hold SHIFT, press the key).

| Character | Key combination | Sharp ASCII |
|-----------|-----------------|-------------|
| π (PI) | SHIFT + `0` | 0xFF |
| £ (pound) | SHIFT + `DOWN_ARROW` (MCP name `LIBRA`) | 0xFB |
| ↓ (down arrow glyph) | `DOWN_ARROW` (base) | 0xFC |
| ↑ (up arrow glyph) | `UP_ARROW` (base) | 0x5E |
| → (right arrow glyph) | SHIFT + `?` | 0xC6 |
| ← (left arrow glyph) | SHIFT + `/` | 0x5F |
| lowercase letters | SHIFT + letter (base = uppercase) | 0xA1.. |

**Semigraphics:** enter graphic mode (press `GRAPH` / DPCT 0xCA), then the
letter/digit/punctuation keys produce semigraphic glyphs - see the GRAPH and
GRAPH+SHIFT columns in the section 2 table (e.g. `D` GRAPH = filled circle,
`D` GRAPH+SHIFT = ring, `DOWN_ARROW` GRAPH+SHIFT = smiley face). Return to text
with `ALPHA` (DPCT 0xC9). Graphic mode is only honored in the ROM monitor and
BASIC (see section 3).
