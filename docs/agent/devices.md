# Expansion devices (MZ-700)

The MZ-700 core has one generic interface for optional expansion cards of
the real MZ-700 (for example a programmable character generator or an
80-column card). Every such device appears in `emu_get_devices` and is
controlled through the existing tools. On MZ-800 and MZ-1500 the interface
is not active: `emu_get_devices` returns `platform_supported: false` and an
empty list.

The MZ-700 binaries (PAL and NTSC) register these devices, all
disconnected by default:

| id | Device | Section below |
|----|--------|---------------|
| `pcg700` | PCG-700 by HAL Laboratory, programmable character generator | [PCG-700](#pcg-700-pcg700) |
| `suc80` | Sharp Users Club UK 80-column kit (80 x 25 from the existing VRAM) | [SUC 80-column kit](#suc-80-column-kit-suc80) |
| `kp80` | K&P 80 column card (Kersten & Partner): CRTC SY6845E, own VRAM and font, second monitor | [K&P 80 column card](#kp-80-column-card-kp80) |

## Discover the devices

`emu_get_devices` (read-only, allowed during replay):

```json
{
  "platform_supported": true,
  "devices": [
    {
      "id": "example",
      "name": "Example card",
      "connected": false,
      "ini_section": "EXAMPLE",
      "options": [
        {"key": "mode", "label": "Mode", "type": "keyword", "value": "off",
         "default": "off", "apply": "live", "keywords": ["off", "on"]},
        {"key": "fill", "label": "RAM after power on", "type": "unsigned",
         "value": 0, "default": 0, "apply": "connect", "min": 0, "max": 255}
      ],
      "ports": [{"first": 112, "last": 115, "read": true, "write": true,
                 "drive": "tristate", "drive_mask": 255}],
      "mmio": [{"first": 57360, "last": 57362, "when": ["mapped"],
                "read": false, "write": true, "drive": "none", "drive_mask": 0}],
      "regions": [{"name": "Example RAM (2 KB)", "kind": "device",
                   "sub_id": 0, "size": 2048, "writable": true}],
      "video": {"glyph_source": true, "render_row": false},
      "outputs": []
    }
  ]
}
```

- `ports`: I/O ports by the low byte of the address (as on the board).
  Numbers are decimal in JSON. `drive` is the output stage of the device
  on the data bus when read (`none` = write-only, never drives;
  `tristate` = all 8 bits while decoded; `partial` = only the bits in
  `drive_mask`; `open_collector`), declared from the schematic of the
  card. Two devices may share a port when at most one of them drives it;
  two drivers are a conflict (policy `DEVBUS/conflict`, default `refuse`).
  Bits nobody drives return the undriven-bus model (`DEVBUS/bus_idle`).
  The whole map with board chips: `emulator://docs/bus_map`.
- `mmio`: memory accesses the device sees in the page 0E000h-0EFFFh.
  `when`: `mapped` = the upper area is mapped (after reset, `OUT (0E3h)`,
  `OUT (0E4h)`), `dram` = DRAM is there (after `OUT (0E1h)`). With
  `drive: "none"` the device only watches the write, the board behaves as
  without the device; a range with another `drive` also answers reads
  (none of the built-in devices does).
- `video`: `glyph_source` = the device supplies the character generator,
  `render_row` = the device draws the screen rows itself (e.g. 80 columns).
- `outputs`: secondary video outputs (a second monitor) of the device;
  read the picture with `emu_device_screenshot` (see below).

## Connect and disconnect

`emu_periph_attach` / `emu_periph_detach` with `kind` = device `id`
(`options` are ignored). The change applies immediately, like the menu
HW & Devices, and is refused during replay recording or playback.

Errors:

| Error text | Meaning |
|------------|---------|
| `Invalid kind: unknown peripheral or device id` | No device with this id. |
| `Device conflicts with a connected device` | Another connected device uses the same port, MMIO address or video hook. |
| `Device failed to connect` | The device refused to start. |
| `Expansion devices are not available on this platform` | MZ-800, MZ-1500. |

A connected device resets with the machine (`emu_reset`); its RAM content
is set only when it is connected (power on).

## Options

Options live in the INI section of the device (`ini_section`). Read them
with `emu_settings_get("<ini_section>/<key>")` and change them with
`emu_settings_set`:

- `bool`: `true` / `false` (or `1` / `0`),
- `keyword`: one of `keywords`,
- `unsigned`: a number from `min` to `max`.

The `emu_settings_set` response has `applies`: `now` (the change applies
immediately, also to a running program) or `on_connect` (the change applies
the next time the device is connected). The key `connected` cannot be set
this way, use `emu_periph_*`.

## Memory of the device

Device memory is listed by `emu_regions_list` with `kind: "device"`
(only while the device is connected) and read / written with
`emu_region_read` / `emu_region_write` like any other region. A write
updates the device at once (for example the visible characters).

## Snapshots and replay

A snapshot contains the connected devices, their options and state. Loading
a snapshot connects the devices it lists and disconnects the others, so an
older snapshot (from a version without devices) gives a machine without
devices. The user's choice in the INI file stays. A recorded replay starts
from a snapshot, therefore devices are part of the recorded state; with no
device connected the replay hashes are the same as in older versions.

## Secondary outputs (a second monitor)

A device with its own monitor has an entry in `outputs` (`id`, `title`,
`width`, `height`). Its picture is separate from the computer's picture:
`emu_screenshot_save_to_file`, `emu_frame_hash` and the screenshot
resources do not contain it. Read it with `emu_device_screenshot`:

```
emu_device_screenshot(output="kp80", format="indexed")
-> {"available": true, "output": "kp80", "device": "kp80",
    "width": 640, "height": 250, "pixel_aspect": 0.52, "seq": 12,
    "image_sha256": "...", "lit_pixels": 1874, "format": "indexed",
    "byte_size": 160000, "palette": ["#000000", "#F0F0F0", "#33FF66",
    "#FFB000"], "data_b64": "..."}
```

- `format="png"` (default) returns a PNG in `data_b64`; with `path` the PNG
  is written to a file on the server host instead.
- `format="indexed"`: one byte per dot, rows from the top, value = index
  into `palette`.
- The picture is the last frame the device drew. While running, the device
  redraws at the end of an emulated frame when something changed; when
  paused, changes through `emu_io_write` or `emu_region_write` are drawn at
  once. `seq` grows with every drawn frame.
- A disconnected device returns `available: false`.

## PCG-700 (`pcg700`)

Programmable character generator: 2 KB RAM replaces the CG-ROM glyphs of
the codes 80h-FFh of both character sets (codes 00h-7Fh always come from
the CG-ROM). The emulation is based on research only (manual translation,
I/O maps, program code), nothing was measured on hardware; the disputed
details are options.

Registers (memory writes, write only; reads return the upper ROM as
without the card; 0E013h and up are not decoded):

| Address | Meaning |
|---------|---------|
| 0E010h | pattern byte, bit 7 = left dot |
| 0E011h | RAM address bits 7-0 |
| 0E012h | bits 1-0 address bits 9-8, bit 2 set (address bit 10), bit 3 1 = PCG off, bit 4 strobe, bit 5 copy from CG-ROM |

RAM address = `(code - 80h) * 8 + row`, + 400h for set 2. A byte is written
by data -> 0E010h, low address -> 0E011h, high address + 10h (+ 30h = copy
from the CG-ROM) -> 0E012h, then 0E012h without bit 4. Writing 00h to
0E012h (with SSW on) switches the PCG on, 08h switches it off. After
`emu_periph_attach`, power on and `emu_reset` the PCG is off (bit 3 = 1);
reset keeps the RAM.

Options (`emu_settings_set("PCG700/<key>", ...)`):

| Key | Values | Default | Applies |
|-----|--------|---------|---------|
| `hsw` | `nom`, `pcg` | `pcg` | now (`nom` = show the CG-ROM, writes still work) |
| `ssw` | `off`, `on` | `on` | now (`on` = bit 3 switches the PCG) |
| `gsw` | `off`, `on` | `on` | now (`off` = all register writes ignored) |
| `ssw_off` | `hsw`, `force_off` | `hsw` | now (with `ssw` off: `hsw` = PCG on, `force_off` = PCG off) |
| `strobe` | `rising`, `level`, `falling` | `rising` | now (when a byte is written relative to bit 4) |
| `decode_dram` | bool | `false` | now (`true` = writes are decoded also after `OUT (0E1h)`) |
| `poweron_ram` | `zero`, `cgrom` | `zero` | on connect |

Region `PCG-700 RAM (2 KB, CG-ROM bit order)` (kind `device`, 2048 B,
writable): the RAM in the emulator's CG-ROM format, left dot = **bit 0**,
so a byte written to 0E010h reads back bit-reversed. A `emu_region_write`
updates the shown characters at once.

Notes for agents:

- `emu_mem_write` refuses the page 0E000h while the upper area is mapped
  (region check); write the registers from a short program in RAM
  (`emu_mem_write` + `emu_set_register PC` + `emu_run`), or fill the RAM
  directly with `emu_region_write`.
- The picture changes only when a frame is drawn: after `emu_settings_set`
  or a register write in pause run at least one frame before
  `emu_frame_hash` / a screenshot.
- `emu_frame_hash` with the PCG off (bit 3 = 1, `hsw` = `nom`) equals the
  picture without the card.

## SUC 80-column kit (`suc80`)

Sharp Users Club UK "MZ-700 80 Column Kit": 80 x 25 characters from the
existing VRAM, no own memory. Model from research only (period article with
the circuit diagram), never measured on hardware; undecided details are
options. Disconnected by default.

| Item | Value |
|------|-------|
| `id` / INI section | `suc80` / `[SUC80]` |
| `mmio` | 57356 - 57359 (0E00Ch - 0E00Fh), `when: ["mapped"]` |
| `video` | `render_row: true`, `glyph_source: false` |
| `regions`, `ports`, `outputs` | none |
| option `attributes` | keyword `per_char` (default) / `per_pair`, `apply: live` |
| I/O catalog | `Device - SUC latch` at 0E00Ch (value = mode mirror, 10h = 80 columns) |

- Mode latch: any write to 0E00Ch - 0E00Fh stores data bit D4 (28h = 40,
  50h = 80 columns). Only while the upper area is mapped, not after
  `OUT (0E1h)`. Reads do not change it, the latch cannot be read. The
  write still reaches the board unchanged. `emu_reset` -> 40 columns;
  connecting -> 40 columns. `emu_mem_write` refuses the page 0E000h while
  the I/O area is mapped, so switch with a CPU program (e.g. `LD A,50h;
  LD (0E00Ch),A` in RAM, `emu_set_register PC`, `emu_run`).
- 80-column layout, row r = 0 - 24, column c = 0 - 79, `k = 40 * r + c div 2`:
  even c = char 0D000h + k, attribute 0D800h + k; odd c = char 0D401h + k,
  attribute 0DC01h + k (`per_pair`: 0D800h + k). 0D400h / 0DC00h are never
  shown. Glyphs are 8 dots wide in the 640-dot canvas; the character
  generator is the one the machine uses (CG-ROM, or the generator of a
  connected device that supplies glyphs).
- A mode change applies from the next drawn display line.
- `emu_frame_hash`, `emu_get_frame_screenshot*`, `emu_screenshot_save_to_file`
  and video recording see the 80-column picture (the framebuffer size does
  not change: 704 x 232 with borders, canvas 640 x 200 at 32, 16).
- In 40 columns the image with the kit connected is byte-identical to the
  image without it; with the kit disconnected, writes to 0E00Ch behave as
  in versions without devices.

## K&P 80 column card (`kp80`)

80-column card of Kersten & Partner for the MZ-700: CRTC SY6845E, own 2 KB
video RAM, own font and **its own video output for a second monitor**
(80 x 25 characters of 8 x 10 dots, 640 x 250, monochrome). The MZ-700
picture does not change. The model is based on research only (a period
article with the schematic, the CRTC datasheet, a description by an owner
of the card); it was not measured on real hardware. Disputed details are
options, not verified hardware behaviour.

- Connect: `emu_periph_attach(kind="kp80")`, INI `[KP80] connected = 1`.
- Ports (full decode, no mirrors): `70h` IN = byte of card VRAM at the
  update address; `71h` OUT = byte to card VRAM at the update address
  (address + 1), IN = selected CRTC register; `72h` OUT = select register
  (5 bits), IN = status (bit 7 update ready, bit 6 light pen = 0, bit 5
  vertical blanking); `73h` = selected register. VRAM is reached only
  through the transparent update address R18/R19, which takes effect on an
  access to R31 (select it with `OUT (72h),31`).
- After power on the registers are 0 (blank picture) and VRAM is 00h
  (shown as `@` once the program initialises the CRTC). `emu_reset`
  keeps registers and VRAM.

Options (`KP80/<key>`, all `applies: now`):

| Key | Default | Meaning |
|-----|---------|---------|
| `read_autoinc` | true | IN 70h increments the update address |
| `data_to_reg` | true | OUT 71h also writes the selected CRTC register |
| `addr_on_r31` | true | R18/R19 take effect on an access to R31 (false = at once) |
| `update_delay` | 0 | status bit 7 stays 0 this many character clocks (2 MHz) after an update |
| `wait` | 0 | extra WAIT T-states on IN/OUT 71h-73h (CPU only: `io_read` / `io_write` and breakpoint `port[N]!` add no WAIT) |
| `phosphor` | white | colour of the second monitor: white, green, amber |

Note: numbers in the INI file are read as hexadecimal (`wait = 10` is 16).

Regions (`kind: "device"`, while connected):

| Name | Size | Writable | Content |
|------|------|----------|---------|
| `K&P 80 VRAM (2 KB)` | 2048 | yes | character codes, address = CRTC address AND 7FFh |
| `K&P 80 font (2 KB)` | 2048 | no | 128 glyphs x 16 bytes (address = (code AND 7Fh) * 16 + row), left dot = bit 7 |
| `K&P 80 CRTC registers` | 32 | no | 0-19 R0-R19, 20 AR, 21 flags (bit 0 address waits for R31, bit 1 update ready, bit 2 vertical blanking), 22-23 update address, 24-25 display start R12:R13, 26-27 cursor R14:R15 (little-endian) |

Text of the screen without decoding the picture: read the register region
for the display start (bytes 24-25), the characters per row R1 (byte 1)
and the rows R6 (byte 6); character of row `y`, column `x` = VRAM byte at
`(start + y * R1 + x) AND 7FFh`. Codes 00h-7Fh follow the German ASCII
variant of the font (5Bh-5Dh = Ä Ö Ü, 7Bh-7Eh = ä ö ü ß); bit 7 shows the
character inverted.
