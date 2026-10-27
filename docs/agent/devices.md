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
      "ports": [{"first": 112, "last": 115, "read": true, "write": true}],
      "mmio": [{"first": 57360, "last": 57362, "when": ["mapped"]}],
      "regions": [{"name": "Example RAM (2 KB)", "kind": "device",
                   "sub_id": 0, "size": 2048, "writable": true}],
      "video": {"glyph_source": true, "render_row": false},
      "outputs": []
    }
  ]
}
```

- `ports`: I/O ports by the low byte of the address (as on the board).
  Numbers are decimal in JSON.
- `mmio`: memory writes the device observes in the page 0E000h-0EFFFh.
  `when`: `mapped` = the upper area is mapped (after reset, `OUT (0E3h)`,
  `OUT (0E4h)`), `dram` = DRAM is there (after `OUT (0E1h)`). The device
  only watches the write, the board behaves as without the device.
- `video`: `glyph_source` = the device supplies the character generator,
  `render_row` = the device draws the screen rows itself (e.g. 80 columns).
- `outputs`: secondary video outputs (a second monitor) of the device.

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
