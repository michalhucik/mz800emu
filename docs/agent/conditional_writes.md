---
title: Conditional writes (expected_sha256, expected)
description: Write memory, a region, a register or CPU flags only if they still hold what you read - expected_sha256 on emu_mem_write / emu_mem_write_force / emu_region_write, expected on emu_set_register / emu_set_cpu_flags; checked and written in one step in the emulator thread, refused with "Precondition failed:" otherwise.
---
# Conditional writes: "write only if it is still what I read"

An agent reads memory, thinks (seconds to minutes), then writes. In
between, the program may run, or a person in the GUI may edit the same
bytes or step the CPU. A plain write then overwrites their work or acts
on stale data. A **conditional write** carries what you expect to find;
the emulator thread compares and writes in **one step**, so nothing can
happen in between - also while the emulator runs (both happen at the same
frame boundary).

| Tool | Condition | Extra output |
|------|-----------|--------------|
| `emu_mem_write`, `emu_mem_write_force` | `expected_sha256`: SHA-256 of exactly the bytes the write overwrites (`addr` .. `addr + length - 1`, through the current memory map) | `before_sha256`, `after_sha256` |
| `emu_region_write` | `expected_sha256`: SHA-256 of the bytes the write overwrites in the region (after clipping at the region end) | `before_sha256`, `after_sha256` |
| `emu_set_register` | `expected`: the current 16-bit value | `previous` (always) |
| `emu_set_cpu_flags` | `expected`: object with the current values of the fields this call writes, e.g. `{"iff1": 1}` | `previous` (always) |
| all of the above and other tools that change the machine state | `expected_state_rev`: the `state_rev` of your read (only in pause) - refused after **any** change of the machine, see `emulator://docs/state_revision` | `state_rev` (always) |

`want_hashes=True` returns `before_sha256` and `after_sha256` without a
condition (a cheap confirmation of what the write did). Without the new
parameters the tools behave exactly as before.

## Recipe

1. Pause if you want a stable state (`emu_pause`), or work while running.
2. Read the range and take its hash: `emu_mem_read(addr, len,
   want_data=False)`, `emu_region_read(...)` or a window of
   `emu_state_observe` (also gives registers from the same moment).
   The hash must be over **the same range** you will write.
3. Write with the condition:

```
r = emu_mem_read(0x4F20, 2, want_data=False)
emu_mem_write(0x4F20, "0900", expected_sha256=r.sha256)
-> {"addr": 20256, "length": 2, "before_sha256": "<= r.sha256>", "after_sha256": "..."}
```

4. On `Precondition failed: ...` nothing was written. The error text
   carries the current value, e.g.
   `Precondition failed: memory changed (expected_sha256=..., current_sha256=...)`
   or `Precondition failed: register HL changed (expected=0x1234, current=0x5678)`.
   Read again, decide whether the write still makes sense, and retry with
   the new hash. Retrying the same request unchanged fails again.

Registers work the same way:

```
emu_set_register("PC", 0x8000, expected=0x1234)      # compare-and-swap
emu_set_cpu_flags(iff1=0, expected={"iff1": 1})
```

## Rules

- **Order of checks.** A write that could never succeed fails with its
  own error first: the region check of `emu_mem_write` (ROM, unmapped,
  MZ-800 VRAM), and an unknown or stale `region_id`, a different
  `layout_gen` or a read-only region for `emu_region_write`. Only a write
  that would be carried out is compared against the condition.
- **Hash range.** `expected_sha256` covers exactly the bytes being
  written. Reading 256 bytes and writing 2 needs a separate hash of the 2
  bytes (a 2-byte `emu_mem_read` or a 2-byte observe window).
- **Banking weakness of CPU addresses.** `emu_mem_write` writes through
  the current memory map. If the banking changed since your read (OUT
  0E0h-0E6h, MemExt map) and the new mapping happens to hold the same
  bytes, the condition holds and the write lands in a different physical
  cell. For a write that must hit one physical cell, use
  `emu_region_write` (physical region, no banking), or check the
  `memory_map` section of `emu_state_observe` together with the hash.
- **ABA.** `expected_sha256` and `expected` compare content. If the
  program changed the bytes and changed them back, the write is accepted.
  If that matters, pause and use `expected_state_rev`
  (`emulator://docs/state_revision`): it refuses after any change of the
  machine, also one that was undone or one elsewhere, and also catches a
  banking change. Both conditions can be used in one call; the revision
  is checked first.
- **Replay.** A refused write is not executed and is not recorded in an
  input recording; an accepted one is recorded as a plain write and
  replays exactly.
- **GUI.** The MCP Activity window shows a refused write as a row with
  "rejected: state changed since it was read"; no notification pops up.
