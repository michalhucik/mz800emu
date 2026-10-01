# MCP Resources - overview

MCP Resources are read-only `emulator://...` URI endpoints that the AI
client reads via `resources/read` (without invoking a Tool). They have
two advantages over Tools:

1. **Cheaper request** - the client does not name an action, just reads
   the URI.
2. **Cacheable** - the MCP client may keep the result (TTL per resource
   semantics).

## Resource list

### State and CPU

| URI | Content |
|-----|---------|
| `emulator://state` | Lightweight state (running/paused + last_user_action) |
| `emulator://cpu/registers` | Full Z80 register snapshot |
| `emulator://cpu/im2_vector` | IM2 vector snapshot |
| `emulator://cpu/interrupt_bus` | IRQ subsystem snapshot |
| `emulator://memory/{addr_hex}/{length}` | Template - read memory bytes |
| `emulator://memory/map` | Per-platform banking (16 x 4 KB slots) |

### Debugger

| URI | Content |
|-----|---------|
| `emulator://breakpoints` | Active BPs with id/addr/type/hit |
| `emulator://callstack` | Single-shadow callstack snapshot |
| `emulator://profiler` | Per-function profile data (excl/incl cycles) |
| `emulator://symbols` | Symbol table listing (cap 10000, truncated flag) |
| `emulator://stack` | 32 LE words starting at the current SP |
| `emulator://stack/history` | SP ring buffer (4096 samples) + slope |
| `emulator://stack/regions` | Defined stack regions with watermarks |
| `emulator://watch` | Active watch rows (cap 256, mode/type/value) |
| `emulator://watch/snapshot/{name}` | Template - per-watch statistics (snap/current/delta/min/max) |
| `emulator://vars` | Smart BP variables (`$name`) |
| `emulator://bookmarks` | Bookmarks (id/input/resolved addr/owner) |

### Configuration and platform

| URI | Content |
|-----|---------|
| `emulator://platform/info` | Platform + mode + TV system + framerate + clocks + scanline (dynamic) |
| `emulator://config/mcp` | MCP server INI values |
| `emulator://config/settings` | Live emulator configuration (audio, video) |
| `emulator://config/peripherals` | Per-chip detail (stub, expanded over time) |
| `emulator://cooperation/policy` | Cooperation hint state |
| `emulator://security/profile` | MCP security profile + capabilities |
| `emulator://memext/info` | Memory expansion adapter info |
| `emulator://media/state` | CMT/FDC/QD/IDE8 mount info |
| `emulator://speed` | Emulation speed (current_percent, max_speed, mode, status) |

### Peripherals

| URI | Content |
|-----|---------|
| `emulator://periph/i8255` | Intel 8255 PPI - keyboard + CMT + PSG audio gate |
| `emulator://periph/i8253` | Intel 8253 CTC - 3 counters (per-channel mode/state/counter) |
| `emulator://periph/z80_pio` | Zilog Z80 PIO - joystick + parallel + IM2 daisy chain (MZ-800 and MZ-1500 only) |
| `emulator://periph/sn76489` | SN76489 PSG - mono / stereo snapshot (MZ-700 returns available=false) |
| `emulator://periph/ay3_8910` | AY-3-8910 PSG - placeholder (always available=false) |
| `emulator://periph/beeper` | CTC0 OUT audio path through GATE0 + PC0 (raw bits + derived audible) |
| `emulator://periph/gdg` | GDG video LSI - palette, raster state, regDMD |
| `emulator://periph/wd1793` | WD279x FDC chip registers + 4 drive mount metadata |
| `emulator://periph/cmt` | CMT cassette - stop/play/record state, motor, polarity, image basename, `cmthack_enabled` (ROM patch instant load) |
| `emulator://periph/cmt/tape` | Loaded tape block listing - container_type (SINGLE/SIMPLE_TAPE), current_block, per-block id/name/cmt_speed/type/playable/recordable |
| `emulator://periph/qd` | Quick Disk - state machine, image basename |

### Input and video

| URI | Content |
|-----|---------|
| `emulator://input/keyboard/state` | Keyboard matrix (real + virtual + effective) + decoded pressed keys |
| `emulator://input/keyboard/matrix_info` | Static symbolic key name table (col, bit, needs_shift) |
| `emulator://input/joystick/state` | Per-port (0, 1) connected flag, state bits, device name |
| `emulator://frame/framebuffer/info` | Framebuffer shape, pixel_format=index8, dirty flag, 16-entry palette |
| `emulator://frame/screenshot.raw` | RGBA8888 base64 buffer (auto downscale 1/2/4) + `fallback_source` |
| `emulator://frame/screenshot` | PNG base64 (`format: "png"`, full frame, stb_image_write encoder) |
| `emulator://video/text_dump` | 40x25 chars + attributes (Sharp ASCII) from MZ-700 text VRAM |

### AI reference docs

Static English markdown reference for advanced AI workflows. Content
lives in `docs/agent/` and is shipped with the emulator. The files are
registered **automatically** (every `*.md` except `README.md` ->
`emulator://docs/<name>`), so adding a new document needs no code
change. Discovery (`resources/list`) is instant (= pure file-based, no
transport call).

| URI | Content |
|-----|---------|
| `emulator://docs/index` | Topic index - when to read what |
| `emulator://docs/memory_layout` | Per-platform Z80 memory maps, banking 0xE0..0xE6, ROM entry points, IRQ vectors |
| `emulator://docs/bp_dsl` | Breakpoint condition expression syntax (registers, mem[], port[], operators, built-ins, $vars) |
| `emulator://docs/smart_vars` | `$name` user variables - action DSL, persistence, lifecycle |
| `emulator://docs/action_dsl` | BP Action DSL grammar - 11 commands (`log`, `set <reg>`, `poke`, `mark`, `$var` writes, `if/then/else`, `enable`/`disable`, `clear_vars`) + forwarding commands (`cdl_*`/`trace_*`/`snapshot`), `log` format specs, stop vs continue semantics |
| `emulator://docs/watch_dsl` | Watch expression syntax (`address` / `expr_scalar` / `expr_deref` + type tags) |
| `emulator://docs/eventlog_mask` | EventLog 64-bit category mask - bit assignments + recipes |
| `emulator://docs/sharp_display_code` | Sharp MZ ASCII vs display code vs std ASCII (decoding `video/text_dump` + `mzascii` watch) |
| `emulator://docs/mz800_keyboard` | MZ-800 keyboard - layout, matrix, modifiers, character layers, key names for `emu_input_send_keys`, special-character recipes (MZ-700/1500 as a diff) |
| `emulator://docs/cmt_workflow` | CMT workflow - real tape vs cmthack, transport, WAV/LEP/L16 recording, speed ratios, SIMPLE_TAPE multi-block seek + per-block speed |

AI clients should read `emulator://docs/index` first - it shows what
is documented and when to consult each topic.

### User knowledge base (`emulator://kb/*`)

A user can expose their own Markdown notes without touching the
repository - by setting a directory via the `MZ800EMU_USER_KB_DIR`
environment variable (typically in the `.mcp.json` `env` block). Files
are scanned recursively and registered as `emulator://kb/<relative path
without .md>` (a namespace separate from the built-in
`emulator://docs/*`). See [configuration.md](configuration.md), section
"User knowledge base", for details.

## `emulator://platform/info` detail

Key Resource for the AI client right after MCP connect - contains
**everything** needed to understand the emulated HW (platform, TV
system, exact clocks, full scanline geometry).

### Top-level fields

| Field | Value | Notes |
|-------|-------|-------|
| `platform` | `mz700` / `mz800` / `mz1500` | Compile-time binary (= MZARCH_NAME; does not distinguish PAL/NTSC - see `tv_system`) |
| `full_name` | "MZ-700 (PAL)" / "MZ-800" / "MZ-1500" / etc. | Human-readable |
| `mode` | `native` / `compat700` | Runtime from GDG regDMD bit (MZ-700 always native) |
| `tv_system` | `PAL` / `NTSC` | Compile-time from MZTVSYS macro |
| `framerate_hz` | 50 / 60 | Derived from TV system |
| `pxclk_hz` | e.g. 14336640 / 17760000 | Simulated GDG base = `screen_ticks * screens_per_sec` |
| `mzarch` | same as `platform` | Legacy alias (= kept for backward compatibility) |
| `mzarch_numeric` | 700 / 800 / 1500 | Numeric platform variant |
| `rom_version` | `unknown` | TBD in V2 from ROM header signature |

### `capabilities` sub-object - compile-time HW support

What is compiled into the binary (= what hot-swap can attach), not the
current runtime attached state. For runtime state see
`emulator://media/state`, `emulator://config/peripherals`,
`emulator://memext/info`.

| Field | Type | Notes |
|-------|------|-------|
| `has_pioz80` | bool | Z80 PIO chip (= MZ-1500 yes, others no) |
| `psg_count` | int | SN76489 PSG channel count (= 0 for MZ-700, 1 for MZ-800, 2 for MZ-1500) |
| `hwext_fdc` | bool | WD279x FDC slot |
| `hwext_ide8` | bool | 8-bit IDE adapter |
| `hwext_ramdisk` | bool | RAM disk extension |
| `hwext_qdisk` | bool | Quick Disk slot |
| `cpu_model` | string | "Z80" (= effectively for all variants) |

### `clocks` sub-object - exact CPU + chip frequencies

| Field | Notes | Typical range |
|-------|-------|---------------|
| `gdg_base_hz` | Simulated GDG base | 14,336,640 (NTSC) / 17,760,000 (PAL) |
| `gdg_real_base_hz` | Real crystal frequency | 14,318,180 / 17,734,475 |
| `cpu_hz` | Z80 effective clock | 3,584,160 (NTSC) / 3,552,000 (PAL) |
| `cpu_divider` | GDGCLK2CPU_DIVIDER | 4 (NTSC) / 5 (PAL) |
| `ctc0_input_hz` | 8253 counter 0 input | 1,102,818 (700-NTSC, div=13) / 1,110,000 (PAL+MZ-800+MZ-1500, div=16) |
| `ctc0_divider` | GDGCLK_CTC0_DIVIDER | 13 (700-NTSC) / 16 (rest) |
| `psg_input_hz` | SN76489 PSG input | cpu_hz / 16 |
| `psg_divider` | PSG_DIVIDER = 16 * cpu_divider | 64 (NTSC) / 80 (PAL) |
| `ctc1_input_hz` | **null** | 8253 counter 1 is cascade-driven |
| `ctc2_input_hz` | **null** | 8253 counter 2 is cascade-driven |
| `ctc12_note` | "read emulator://periph/i8253 for runtime state" | Per-platform routing |

**Note on MZ-1500 vs MZ-700 NTSC**: both use the 14.318 MHz crystal
but have different CTC0 dividers (16 vs 13), which produces different
BASIC tempo and sound tones. The `ctc0_divider` field makes this
explicit.

### `scanline` sub-object - raster geometry

| Field | Notes | Typical range |
|-------|-------|---------------|
| `screen_total_width_ticks` | Total raster line (= sync + back + display + front porch) | 912 (NTSC) / 1056 (PAL) |
| `screen_total_height_lines` | Total scan lines | 262 (NTSC) / 312 (PAL) |
| `screen_total_ticks_per_frame` | width × height | 238944 / 329472 |
| `screens_per_sec` | = framerate_hz | 60 / 50 |
| `display_width` | Visible display (= canvas + border) | e.g. 704 (MZ-700 PAL) |
| `display_height` | Visible display | e.g. 232 |
| `canvas_width` | Drawing area without border | 640 |
| `canvas_height` | Drawing area without border | 200 |
| `border_left_width` / `border_right_width` | Side borders | 32 |
| `border_top_height` / `border_bottom_height` | Top/bottom borders | 16 |
| `h_sync_ticks` | HSync interval | 80 (PAL) / 65 (NTSC) |
| `h_back_porch_ticks` | Back porch after HSync | 136 (PAL) / 104 (NTSC) |
| `h_front_porch_ticks` | Front porch before HSync | 216 (PAL) / 39 (NTSC) |

### Subset in hello capabilities

After the MCP `initialize` handshake the client immediately receives
the top-level fields (`platform`, `full_name`, `mode`, `tv_system`,
`framerate_hz`, `pxclk_hz`) in `result.capabilities`. The full
`clocks` + `scanline` detail is available via `resources/read` on
`emulator://platform/info`.

### Example response (MZ-800 PAL in native mode)

```json
{
  "platform": "mz800",
  "full_name": "MZ-800",
  "mode": "native",
  "tv_system": "PAL",
  "framerate_hz": 50,
  "pxclk_hz": 17760000,
  "mzarch": "mz800",
  "mzarch_numeric": 800,
  "rom_version": "unknown",
  "capabilities": {
    "has_pioz80": false,
    "psg_count": 1,
    "hwext_fdc": true,
    "hwext_ide8": true,
    "hwext_ramdisk": true,
    "hwext_qdisk": true,
    "cpu_model": "Z80"
  },
  "clocks": {
    "gdg_base_hz": 17760000,
    "gdg_real_base_hz": 17734475,
    "cpu_hz": 3552000,
    "cpu_divider": 5,
    "ctc0_input_hz": 1110000,
    "ctc0_divider": 16,
    "psg_input_hz": 222000,
    "psg_divider": 80,
    "ctc1_input_hz": null,
    "ctc2_input_hz": null,
    "ctc12_note": "cascade from CTC0 / per-platform routing; read emulator://periph/i8253 for runtime state"
  },
  "scanline": {
    "screen_total_width_ticks": 1056,
    "screen_total_height_lines": 312,
    "screen_total_ticks_per_frame": 329472,
    "screens_per_sec": 50,
    "display_width": 704,
    "display_height": 232,
    "canvas_width": 640,
    "canvas_height": 200,
    "border_left_width": 32,
    "border_right_width": 32,
    "border_top_height": 16,
    "border_bottom_height": 16,
    "h_sync_ticks": 80,
    "h_back_porch_ticks": 106,
    "h_front_porch_ticks": 22
  }
}
```

## Usage examples

### Python wrapper (FastMCP)

```python
# Claude Desktop / Code MCP client auto-discovers Resources via
# resources/list. For programmatic use:

from mcp.client.session import ClientSession

async with ClientSession(...) as session:
    resources = await session.list_resources()
    for r in resources:
        print(r.uri, r.name)

    # Read one resource:
    content = await session.read_resource("emulator://cpu/im2_vector")
    # content.contents[0].text = JSON string
```

### Raw JSONL (debug)

If you have a JSONL terminal attached to the MCP backend, the
underlying `get_*` commands can be invoked directly:

```jsonl
{"type":"request","id":1,"cmd":"get_cpu_im2_vector"}
```

Response:

```json
{"type":"response","id":1,"success":true,
 "data":{"im":2,"i":64,"vec":128,"available":true,
         "isr_addr":16512,"isr_target":4660}}
```

### Example: per-watch snapshot

The `emulator://watch/snapshot/{name}` template returns statistics for
a named watch row:

```json
{"name": "PLAYER_HP", "found": true, "row_id": 17, "type": "u16le",
 "snapshot_active": true, "min_max_valid": true,
 "snap_int": 4096, "cur_int": 4660, "delta_int": 564,
 "min_int": 256, "max_int": 4660, "change_count": 42}
```

When no watch with that name exists:

```json
{"name": "MISSING", "found": false}
```

Anonymous watch rows (= without `name`) are not addressable via the
URI; use `emulator://watch` for the full list.

## Limitations

- **No subscribe** - pull-only reads; `notifications/resources/updated`
  is not available yet.
- **Symbols cap 10000** - above the cap the client must fall back to
  the `symbol_list` Tool with a prefix filter.
- **Stack history pull-only** - when recording is off, the resource
  returns an empty `samples` array.
- **Callstack scope_state** - the current single-shadow tracker has no
  per-scope BP awareness; the Resource only exposes aggregate `active`
  flag and statistics, no scope_bp_id.
- **Watch without owner field** - `emulator://watch` does not expose
  per-row origin; watch storage `.watch` reverts to USER after reload.
- **Stack fixed window** - `emulator://stack` returns 32 words starting
  at SP. For different lengths or lines_above values use the full
  STACK_DUMP path via a Tool.
- **Bookmarks snapshot lifetime** - the returned data is valid only
  for the duration of the dispatch call; the client must copy it
  immediately.
- **i8255 Mode Set shadow** - `emulator://periph/i8255` decodes mode /
  direction fields only when bit 7 of the Control Word mirror byte is
  set. For Bit Set/Reset operations (bit 7 = 0) the response carries
  `cw_decoded=false`.
- **Z80 PIO platform check** - on MZ-700 returns
  `{"available": false, "reason": "platform has no Z80 PIO"}`.
- **Z80 PIO control sequencer** - per-port `last_ctrl_byte` is just
  the last CPU write; distinguishing Mode Set / IOMCW / ICW / IDW
  states is not provided.
- **i8253 / i8255 last_cw_byte** - 8253 and 8255 hardware does not let
  software read back the Control Word (it is a sequencer register);
  the emulator mirror is accurate for the last write but per-bit
  decoding is not provided for the CTC CW (the client decodes
  SC / RL / M / BCD bits on its own).
- **PSG MZ-700 availability** - `emulator://periph/sn76489` on MZ-700
  returns `{"available": false, "reason": "platform has no PSG"}`.
- **PSG mono vs stereo** - MZ-800 boots mono (`psg_count=1`); a
  runtime switch to stereo (via I/O port) flips `psg_count=2`.
  Clients must not cache `psg_count` over long intervals.
- **PSG write-only HW** - SN76489 has no CPU read; the Resource is a
  pure side-effect free snapshot of internal emulator registers.
- **AY-3-8910 not implemented** - always
  `{"available": false, "reason": "AY-3-8910 not implemented in this
  emulator"}`. Kept in the API for forward compatibility only.
- **Beeper is not a 1-bit chip** - the Sharp MZ has no dedicated 1-bit
  speaker; "beeper" denotes the audio path from CTC0 OUT through
  GATE0 and PC0 AND gates (`audible = ctc0_out AND gate0 AND pc0`).
  For the audio frequency read CTC0 `preset_value` from
  `emulator://periph/i8253`.
- **Beeper GATE0 per platform** - MZ-800 in 800 mode (DMD3=0) has
  GATE0 hardwired to 1; other platforms drive GATE0 via writes to
  0xE008 bit 0. The Resource returns the raw bit on every platform;
  the client interprets it.
- **GDG per-platform palette layout** - `emulator://periph/gdg` has a
  discriminator `platform` field. MZ-800 has a 16-color palette via
  PAL_GRP + PAL0..3; MZ-700 / MZ-1500 have an 8-entry mode color
  table. `palette_count` indicates how many entries are valid
  (16 or 8).
- **GDG VRAM dump not available** - the Resource only returns
  registers and raster state. To read VRAM content the client must
  use the `mem_read` Tool with banking-aware addresses.
- **FDC + QD runtime availability** - return
  `{"available": false, "reason": "FDC not compiled or detached"}`
  (or QDisk) when the chip is not in the build or is runtime detached.
  The default build of all three platforms has both enabled.
- **Image paths as basename only** - FDC / CMT / QD `image_basename`
  is filename only (no directory component) - security limitation.
- **CMT without sample stream** - the Resource returns state + image
  basename + motor flag, but not tape signal samples.
- **QD VIRTUAL files listing missing** - `virt_files_count` reports
  how many MZF are in the current VIRTUAL state, but not their names.
- **Keyboard pressed_keys cap 32** - the decoded `pressed_keys` array
  holds at most 32 simultaneously held positions; over the cap clients
  see `pressed_truncated=true`. The raw 10-byte `effective` matrix is
  always complete.
- **Joystick without analog axes** - `emulator://input/joystick/state`
  is digital only. The emulator has no analog axis or deadzone
  configuration.
- **Framebuffer palette is 16 entries** - `DISPLAY_MZCOLORS = 16`.
  Pixel bytes are masked with `0x0F` during the INDEX8 -> RGBA expand;
  the upper 4 bits of each pixel byte are reserved for emulator
  internal use (border path, blink).
- **screenshot PNG** - `emulator://frame/screenshot` returns the full
  frame as PNG base64 (`format: "png"`, `data_b64`, `byte_size`).
  Encoding is done by the vendored `stb_image_write.h` (public domain)
  on the emulator thread = no runtime DLL dependency. Content matches
  `screenshot.raw` at downscale 1, just a PNG container. `available=false`
  only when the frame has not been rendered yet / display not initialized
  / encode failed.
- **screenshot.raw response size** - the native MZ-800 framebuffer is
  928 x 288 x 4 = 1.07 MB raw, ~1.43 MB base64. If the transport
  enforces a smaller per-message budget, the backend auto-bumps
  `downscale_factor` to 2 or 4 and reports the effective factor.
  Clients should not assume a fixed scaling.
- **screenshot.raw `fallback_source` field** - the
  response contains a string field `fallback_source` with values
  `"sdl_snapshot"` or `"gdg_live"`. The primary source is the SDL
  display-ready snapshot (`g_iface_video->fbsnapshot_pixels`,
  populated by the emu thread per frame). When NULL (= headless
  mode without an SDL render thread, or a race condition in GUI
  mode between publish and consume), the handler falls back to the
  GDG live buffer (`g_framebuffer.pixels`, static BSS allocation,
  always available in emu thread context). The content of both
  sources is byte-identical after a completed frame; `gdg_live`
  may capture a partial frame mid-raster. Clients can use the
  field for diagnostics (= unexpected `gdg_live` in GUI mode
  indicates high dispatch latency).
- **Python client example for screenshot.raw → PIL** - see
  `docs/en/mcp-server/headless-mode.md` section "Screenshot in
  headless mode".
- **text_dump platforms** - reads the 40x25 MZ-700 text layout
  (D000-D3FF chars, D800-DBFF attributes). For MZ-800 the Resource
  returns `available=false` when the emulator runs in 800 graphics
  mode (= only available in 700 compat mode). MZ-1500 always returns
  available (shares the 700-compat layout).
- **text_dump Sharp ASCII not converted** - `chars_b64` carries the
  raw 8-bit Sharp ASCII byte stream. Clients apply the Sharp ASCII ->
  UTF-8 mapping themselves.
- **keyboard/matrix_info aliases** - the symbolic name table contains
  aliases (RETURN/ENTER/CR; CTRL/CONTROL; ARROW_UP/UP/...). The same
  (col, bit) pair appears multiple times under different `name`
  values; clients filtering for unique positions must dedupe by
  (col, bit).

## Security note

`emulator://config/settings` respects the MCP security profile: in
**observer** mode it returns `filtered=true` and an empty `sections`
object (= the AI may not even read whitelisted keys).

`emulator://security/profile` is always fully readable (= the AI must
be able to learn its own restrictions).
