# SUC 80-column kit (MZ-700)

The "MZ-700 80 Column Kit" of the Sharp Users Club UK (1990) converts the
MZ-700 main board to 80 columns x 25 rows. It has no video RAM of its own:
it uses the upper halves of the existing character and colour VRAM, which
the 40-column mode does not display. The emulator offers it on MZ-700
(PAL and NTSC) only.

> The author of the emulator has never seen this device. The model is based
> solely on research of information found on the internet (a period article
> with the circuit diagram); nothing was measured on real hardware. Details
> the sources do not decide are options, not verified behaviour of the
> hardware. The SUC CP/M software for the kit is not part of the emulator.

## Connecting the kit

- Menu **HW & Devices** - **SUC 80-column kit** - **Connected**.
- INI file, section `[SUC80]`, key `connected = 1`.
- MCP: `emu_periph_attach` with `kind = "suc80"`.

The kit is disconnected by default. When connected, the screen stays in
40 columns and looks exactly as without the kit until a program switches
to 80 columns. The state of the kit (40 or 80 columns) is saved in
snapshots.

## Switching 40 / 80 columns

Any write to the addresses 0E00Ch - 0E00Fh sets the mode by data bit D4;
the other bits do not matter:

| Value | D4 | Mode |
|-------|----|------|
| 28h (40) | 0 | 40 columns |
| 50h (80) | 1 | 80 columns |

```asm
        LD   A,50h
        LD   (0E00Ch),A     ; 80 columns
        ...
        LD   A,28h
        LD   (0E00Ch),A     ; 40 columns
```

- The kit only watches the write; the machine behaves as without the kit.
  The mode cannot be read back and reading the addresses does not change it.
- The kit reacts only while the memory-mapped I/O area is mapped at
  0E000h (after reset, `OUT (0E3h)`, `OUT (0E4h)`), not after `OUT (0E1h)`.
- RESET returns to 40 columns.
- A mode change applies from the next drawn display line; a change in the
  middle of a line is not emulated.

## VRAM layout in 80 columns

Each 40-column character position shows two characters: the first one from
the lower half of the character VRAM (block A, 0D000h - 0D3E7h), the second
one from the upper half (block B, 0D401h - 0D7E8h). For text row r = 0 - 24
and column c = 0 - 79, with `k = 40 * r + c / 2` (integer division):

| Column | Character | Colour attribute (default option) |
|--------|-----------|-----------------------------------|
| even c | 0D000h + k | 0D800h + k |
| odd c  | 0D401h + k | 0DC01h + k |

The order on the screen is therefore 0D000h, 0D401h, 0D001h, 0D402h, ...;
the last pair of the screen is 0D3E7h / 0D7E8h. The bytes 0D400h and
0DC00h are never displayed.

Characters keep the font and colours of the MZ-700, only the width of a
character is halved (8 dots of a 640-dot line). The attribute byte has the
usual meaning: bits 6-4 foreground, bits 2-0 background, bit 7 selects the
second character set.

If a card with a programmable character generator is connected as well,
the 80-column screen uses its characters, because the kit only changes the
addressing of the VRAM. Nobody has documented this combination on real
hardware.

## Option: Attributes in 80 columns

The sources do not say how colours behave in 80 columns. The option
**Attributes in 80 columns** (INI key `attributes` in `[SUC80]`) chooses:

| Value in the menu (INI) | Meaning |
|-------------------------|---------|
| Per character (`per_char`, default) | Every character has its own attribute: even columns 0D800h + k, odd columns 0DC01h + k. |
| Per pair of characters (`per_pair`) | Both characters of a pair use the attribute of block A (0D800h + k), including the character set (bit 7). |

The option applies immediately, also while a program runs. It has no effect
in 40 columns.

## Debugger, screenshots and video

- The I/O Ports window shows the entry **Device - SUC latch** (0E00Ch) in
  the section Expansion devices while the kit is connected. Its value is a
  mirror of the mode (10h = 80 columns, 0 = 40 columns), the real latch
  cannot be read.
- Screenshots, video recording and the MCP screenshot tools contain the
  80-column picture as shown on the screen.
