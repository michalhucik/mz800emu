# Bus Map: who owns an address on the Z80 bus (`emu_get_bus_map`, `bus_peek`)

Every core (MZ-700, MZ-800, MZ-1500) carries a static table of the chips
and peripherals of its board: which address each one decodes, in which
direction and with which **output stage** (`drive`). The MZ-700 core also
models the shared Z80 data bus of the optional expansion devices
(`emulator://docs/devices`): every port range and MMIO range of a device
declares its `drive`, a write is delivered to every owner of the address,
and a read is resolved from the contributions of all owners.
`emu_get_bus_map` shows the resulting map for all 256 ports and the MMIO
window 0E000h-0E0FFh; `emu_bus_peek` reads one address without side
effects through the same resolution.

Use `emu_get_bus_map` when you need to know **which chip or device answers
at an address**, whether two devices overlap, which ports a program touches
without any owner, or what the emulator returns for an undriven read. Use
`emu_get_devices` for the device list, options and regions; use
`emu_io_read` only when you want the real read with side effects.

Availability: `supported` is true on all three cores. `devbus_supported`
(expansion devices, conflict policy, `bus_idle` settings) is true on the
MZ-700 core only; on the MZ-800 and MZ-1500 the owners are the board chips
and peripherals and the undriven read is the bus latch of the emulator.
Read-only, allowed during replay, not sensitive.

Platform notes:

- MZ-800: `ppi8255` and `ctc8253` own 0D0h-0D7h with `connected` true only
  while the DMD is in MZ-800 mode (`emu_get_platform_info` `mode` =
  `native`); in MZ-700 mode they decode at 0E000h-0E007h instead (MMIO
  owners carry `connected` false in MZ-800 mode). `joy0` / `joy1` own the
  reads of 0F0h / 0F1h with `drive: partial`, `drive_mask: 63` (74LS365
  drives DT0-DT5 only); the write of 0F0h is the GDG palette, hence
  `dir_split`. The `gdg` owns 0CCh-0CFh writes and the 0CEh read (status,
  `drive: unknown`: custom LSI without a datasheet). `mapper` decodes IN
  0E0h / 0E1h (VRAM / CG-ROM selection) with `drive: none`: nothing drives
  the bus, the read is a ghost. `psg_stereo` (0F3h, 0F9h) is present only
  with the HW compatibility option allow_psg1. `pezik_e8` and `ramdisk`
  both own 0E9h-0EBh (the emulator gives the Pezik priority), so the port
  shows `potential_conflict`.
- MZ-1500: `psg` owns 0E9h (both chips), 0F2h (left) and 0F3h (right),
  `gdg` owns 0F0h-0F1h writes; 0CEh and 0D0h-0D7h have no owner (the 8255,
  8253 and status are reachable only at 0E000h-0E008h). The emulator
  decodes the Quick Disk at 0F4h-0F7h, the same ports as the MZ-700 and
  MZ-800 (knowledge base `hw/15-quick-disk.md`; not measured on a real
  MZ-1500).
- `mirror_note` of every port owner on the MZ-800 and MZ-1500 says that the
  emulator decodes the low address byte only (a port is mirrored over
  A8-A15) and that this is not measured on the real machine.

## Output stage (`drive`)

| `drive` | Meaning | Example |
|---------|---------|---------|
| `none` | never drives the bus; the device only observes writes | PCG-700 registers, SUC latch, memory mapper |
| `tristate` | drives all 8 bits while its own address is decoded | 8255 PPI, Unicard, K&P 80 (74LS245) |
| `partial` | drives only the bits in `drive_mask`, the rest float | joystick buffers (bits 0-5) |
| `open_collector` | wired-AND with idle 1 | not used on MZ data lines; kept for completeness |
| `unknown` | the knowledge base has no schematic or datasheet of the output stage | FDC card, IDE8, RAM disk, Quick Disk interface |

`drive` describes the schematic, never a guess: static owners carry a
`source_note` with the datasheet, VHDL or the sentence that is missing.
`unknown` means exactly that, treat it as "may drive all bits".

## Undriven bits (`bus_idle`)

Bits nobody drives take the platform's **idle model**: `latch` (default) =
the last byte read from memory (`regDBUS_latch`, the long-standing emulator
model: `IN A,(n)` from a free port returns `n`), or `const` = the fixed
`bus_idle_value` (for example 0FFh). This is a model of the emulator, **not
measured on real hardware**; it is configured in the INI section `[DEVBUS]`
and through `emu_settings_set("DEVBUS/bus_idle", "latch" | "const")` and
`emu_settings_set("DEVBUS/bus_idle_value", "0xff")`. `idle_byte` in the
response is the value in effect now.

## Conflicts (`conflict_policy`)

Two drivers on the same bit of the same address are a conflict. Policy
`refuse` (default) does not connect the second device
(`emu_periph_attach` fails with a conflict). `and` and `first` allow it and
every read with a conflict raises the event `bus_conflict` (subscribe with
`emu_event_subscribe`), counts in `conflict_hits` and marks the port with
`conflict_seen`; the value is a model (`and` of the bits, or the first
connected owner), the real hardware result of two fighting drivers is not
known. Change with `emu_settings_set("DEVBUS/conflict", "refuse" | "and" |
"first")`; the policy and the idle model are stored in a replay's
`manifest.json` and playback warns when they differ.

## `emu_get_bus_map`

```
emu_get_bus_map(include=["ports", "mmio", "devices", "activity"], only_mapped=False)
-> {"platform": "mz700", "supported": true, "devbus_supported": true,
    "conflict_policy": "refuse", "bus_idle": "latch", "bus_idle_value": 255,
    "idle_byte": 112, "tracking_active": false, "mmio_mapped_now": true,
    "frame": 1234, "devbus": {"registered": 3, "connected": 1},
    "ports": [
      {"port": 112, "owners": [
         {"id": "kp80", "name": "K&P 80 Column Card", "kind": "devbus", "slot": 2,
          "dir": "rw", "drive": "tristate", "drive_mask": 255, "connected": true,
          "range": [112, 115]}],
       "flags": [], "last_in": null, "last_out": null,
       "hits_in_per_sec": 0, "hits_out_per_sec": 0, "total_in": 0, "total_out": 0,
       "ghost_hits": 0, "conflict_hits": 0},
      {"port": 224, "owners": [
         {"id": "mapper", "name": "Memory mapper (banking)", "kind": "static",
          "dir": "w", "drive": "none", "drive_mask": 0, "connected": true,
          "range": [224, 230], "source_note": "write-only memory mapper ..."}],
       "flags": [], ...},
      {"port": 116, "owners": [], "flags": ["unmapped"], ...}, ...],
    "mmio": [
      {"addr": 57344, "owners": [{"id": "ppi8255", "kind": "static", "dir": "rw",
         "drive": "tristate", "when": ["mapped"], ...}],
       "flags": [], "when": ["mapped"], "last_write": null, "hits_out_per_sec": 0}, ...],
    "devices": [
      {"id": "ppi8255", "name": "8255 PPI", "kind": "static", "connected": true,
       "ports": 0, "mmio": 4, "drive": "tristate", ...},
      {"id": "kp80", "kind": "devbus", "slot": 2, "connected": true, "ports": 4,
       "mmio": 0, "drive": "tristate", "glyph_source": false, "render_row": false,
       "regions": 3, "outputs": 1}, ...]}
```

- `include` (default all): `ports`, `mmio`, `devices`, `activity`. Without
  `activity` the per-port counters and last values are omitted (they are
  live only while `tracking_active` is true, i.e. while the I/O Ports or
  Bus Map window tracks activity; `conflict_hits` comes from the bus core
  and is always present). `only_mapped` drops ports and cells without any
  owner (registered but disconnected devices count as owners).
- `owners[].kind`: `static` = a chip or peripheral of the board (its
  `connected` follows the peripheral: a disconnected FDC still owns its
  ports but reads return the idle value); `devbus` = an expansion device
  (`connected` = attached). `dir`: `r`, `w` or `rw`. `range` = the address
  range of the owner that contains this address. MMIO owners carry `when`
  (`mapped` / `dram`: in which map state the owner decodes).
- `flags`: `unmapped` (no present owner: a read returns `idle_byte`),
  `dir_split` (reads and writes go to different owners), `potential_conflict`
  (two drivers overlap on some bit, possibly a registered but disconnected
  device: attaching it would fail with policy `refuse`), `conflict_seen`
  (a read with a conflict happened), `ghost_seen` (the program accessed the
  address without any owner while tracking was active).
- `last_in` / `last_out` / `last_write`: `{"value", "age_frames"}` or null.

## `emu_bus_peek`

```
emu_bus_peek(port=0x72)   -> {"port": 114, "value": 128, "driven_mask": 255, "source": "devbus"}
emu_bus_peek(port=0x74)   -> {"port": 116, "value": 112, "driven_mask": 0, "source": "idle"}
emu_bus_peek(addr=0xE000) -> {"addr": 57344, "value": 255, "driven_mask": 255, "source": "board"}
```

Exactly one of `port` (0-65535, decoded by the low byte) or `addr` (MMIO
window 0E000h-0E0FFh, all cores). No side effects: devices answer through
their `peek` handlers, board chips through the catalog mirrors or the
memory peek; `driven_mask` 0 means nobody drives and `value` is the idle
model. `source`: `devbus`, `board` or `idle`. Unlike `emu_io_read` it does
not trigger the chip (no FDC status clear, no CRTC update strobe).

## Recipes

- **Which ports does a program read that nobody owns?** Open the Bus Map or
  I/O Ports window in the GUI (tracking is on while it is open), run the
  program, then `emu_get_bus_map(include=["ports", "activity"],
  only_mapped=False)` and pick ports with `ghost_seen` in `flags` /
  `ghost_hits > 0`. Those reads depend on the undriven-bus model.
- **Why does `emu_periph_attach` report a conflict?** Look at the ports of
  the device in `emu_get_bus_map`: another owner with `drive` other than
  `none` on an overlapping `drive_mask` and `potential_conflict` in `flags`.
- **What would the CPU read here, without touching the chip?** `emu_bus_peek`.
- **What does the emulator assume about an undriven read?** `bus_idle`,
  `bus_idle_value`, `idle_byte` in the header; change with
  `emu_settings_set("DEVBUS/...")` to compare period software against both
  models.

Related: `emulator://docs/devices` (devices, options, regions, outputs),
`emulator://docs/trace_read` (iorqlog type `iorq_conflict`, Event Viewer
subtype `CONFLICT`).
