---
title: Reading registers and memory from one moment (state_observe)
description: emu_state_observe returns registers, flags, run number, emulated time and up to 16 memory windows (CPU addresses or physical regions) sampled at the same moment, with SHA-256 of each window - use it instead of separate register and memory reads while the emulator runs, and as the read half of a conditional write.
---
# Reading registers and memory from one moment: `emu_state_observe`

While the emulator runs, every MCP call is served at the next frame
boundary. Two calls - `emu_get_cpu_panel_batch` and then `emu_mem_read` -
therefore always see two different frames: a register and the memory it
points to do not belong together. `emu_state_observe` reads everything in
**one pass of the emulator thread**, so registers, flags, time and all
windows come from the same moment.

| Situation | Separate calls | `emu_state_observe` |
|-----------|----------------|---------------------|
| Emulator runs, you need a pointer register and the data it points to | two different frames, inconsistent | one frame, consistent |
| Emulator paused | consistent, but a person in the GUI may step between your calls | one call; `run_seq` shows whether the emulator ran meanwhile |
| You want to write only if nothing changed | - | `sha256` of a window = `expected_sha256` of a conditional write |

While running, the moment is the **start of a video frame** (beam on
line 0) and the call waits up to one frame (about 20 ms). For a state in
the middle of a frame, stop there first (`emu_run_until_raster`, a
breakpoint) and observe in pause.

## Request

```json
{"windows": [
   {"name": "ptr",   "addr": 12288, "len": 2},
   {"name": "plane", "region": {"kind": "vram_phys_plane", "sub_id": 0},
    "offset": 0, "length": 256, "want_data": false}],
 "include": ["raster", "memory_map"],
 "hashes": ["cpu", "ram"],
 "want_data": true}
```

| Field | Meaning |
|-------|---------|
| `windows` | 0 to 16 windows, at most 65536 bytes in total. Required (an empty list reads only registers and time). |
| window `addr`, `len` | CPU address range through the **current memory map** (banking applies), `addr + len` at most 0x10000. Same bytes as `emu_mem_read`. |
| window `region`, `offset`, `length` | Physical region by `kind` (names from `emu_regions_list`) and `sub_id` (bank, plane, instance; default 0). No `region_id`: a window stays valid after a hardware change (RAM disk, MemExt, snapshot load). Same bytes as `emu_region_read`. |
| window `name` | Optional label (max 64 characters), echoed back. |
| window `want_data` | Overrides the global `want_data` for this window. |
| `want_data` | Default `true`. `false` = only `sha256` per window, no `data_b64`. Use it above a few KiB. |
| `include` | Optional sections: `raster`, `im2`, `last_instr` (same schema as `emu_get_cpu_panel_batch`) and `memory_map`. |
| `hashes` | Optional SHA-256 of whole state parts: `cpu` (registers and hidden Z80 state), `ram`, `vram`, `memext`, `pcg`. Same definition as the replay hashes and `result.json`. |

All reads have no side effects: no MEM_R breakpoint fires, mapped ports
at 0E000h-0E008h (MZ-700 mode) are read without the chip side effect.
Keyboard, joystick, tape and CTC0 inputs show the state of their last
synchronisation, which may differ from what the CPU would read at that
exact moment.

## Response

```json
{"run_seq": 41, "running": false, "stable": true, "drain": "paused",
 "time": {"frame": 12345, "frame_ticks": 26, "ticks": 4375361066, "total_cycles": 987654321},
 "regs": {"AF": 0, "BC": 0, "PC": 8197, "...": 0},
 "flags": {"iff1": false, "im": 1, "halted": false, "...": 0},
 "windows": [
   {"name": "ptr", "addr": 12288, "len": 2, "sha256": "...", "data_b64": "..."},
   {"name": "plane", "region": {"kind": "vram_phys_plane", "sub_id": 0},
    "offset": 0, "length": 256, "sha256": "..."}],
 "complete": true,
 "hashes": {"cpu": "...", "ram": "..."},
 "raster": {"scanline": 0, "...": 0},
 "memory_map": {"platform": "mz800", "slots": [
   {"addr_start": 0, "addr_end": 4095, "kind": "rom_low", "source": "unknown",
    "ro_rw": "r", "slot_offset": 0}, "..."]}}
```

| Field | Meaning |
|-------|---------|
| `run_seq` | Number of the running (in pause: the last) run, as in `emu_wait_stop`. If it differs from your previous read, the emulator ran in between (also a single step). |
| `running`, `stable` | `stable` is `true` only in pause: the state stays as read until something changes it. While running the data are consistent with each other but already old when you get them. |
| `drain` | Where the read was served: `frame` (frame boundary while running) or `paused`. |
| `time` | `frame` (frame number), `frame_ticks` (GDG ticks since the start of the frame), `ticks` (GDG ticks since the emulator started, 64-bit: the clock of `t` in `emu_trace_read`, `stop_ticks`, `Cycle` in breakpoint conditions, target of `emu_run_until_tstate(target_ticks=...)` and `emu_replay_goto(t=...)`), `total_cycles` (CPU T-states, a 32-bit counter that wraps and that a reset clears). |
| `regs`, `flags` | Same as `emu_get_cpu_panel_batch`. |
| window `sha256` | SHA-256 of the window bytes, 64 lowercase hex digits - the same value `emu_mem_read` / `emu_region_read` return for the same range. |
| window `error` | `region_unavailable`: the region does not exist or is not connected now (MemExt detached, RAM disk bank beyond its size). The window has `length` 0 and no hash; the call still succeeds and `complete` is `false`. |
| `hashes` | Requested parts; a part the machine does not have (MemExt detached, PCG outside MZ-1500) is `null`. |
| `memory_map.slots[].kind` | Page kind of the **current** banking for each 4 KiB slot (`ram`, `rom_low`, `rom_high`, `cgrom`, `vram_i`, `vram_ii`, `vram_text`, `cgram`, `pcg_1`..`pcg_3`, `mapped_ports`, `prohibited`, `unmapped`) - tells you what a CPU window actually read. `source`, `ro_rw`, `slot_offset` as in `emu_get_memory_map`. |

## Errors

All parameter errors return `Invalid parameters: ...` and read nothing:
more than 16 windows, more than 65536 bytes, a window with both or
neither of `addr` / `region`, `addr + len` over 0x10000, an unknown region
kind, an unknown `include` section or `hashes` part. A region window that
reaches past the end of its region is also `Invalid parameters` (with the
region size in the text) - a clipped window could not be used as
`expected_sha256`.

## Recipes

**Pointer and data while running.** A program keeps a pointer in HL and
writes through it. One call gives both:

```
emu_state_observe(windows=[{"addr": 0x3000, "len": 16}])
-> regs.HL, windows[0].data_b64  (same moment)
```

**Did anything happen since my last look?** Observe with
`want_data=False` and compare `run_seq` and the window hashes with the
previous answer.

**Read, think, write only if unchanged.** Observe the range, decide, then
write with the window hash as the condition (see
`emulator://docs/conditional_writes`):

```
o = emu_state_observe(windows=[{"name": "lives", "addr": 0x4F20, "len": 1}])
emu_mem_write(0x4F20, "09", expected_sha256=o.windows[0].sha256)
```

If a person or the program changed the byte in between, the write is
refused with `Precondition failed:` and nothing changes.

**Same check as the replay.** `hashes: ["cpu", "ram", "vram"]` gives the
same part hashes as a replay checkpoint or `result.json`. Hashing whole
parts holds the emulator thread (about 2 ms for `cpu`, `ram`, `vram` and
a 512 KB MemExt together in one measurement, about a tenth of a frame),
so request them only when needed.
