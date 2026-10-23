---
title: State revision (state_rev, expected_state_rev, if_state_rev_differs)
description: One number that changes whenever the machine state may have changed (MCP, GUI, replay, devices) - state_rev in emu_get_state, emu_state_observe, write responses and paused / resumed / step_done events; expected_state_rev refuses a write if anything changed since your read; if_state_rev_differs answers not_modified without reading.
---
# State revision: "has anything changed since I looked?"

`state_rev` is a number that changes every time the state of the emulated
machine may have changed, whoever changed it: your MCP calls, another MCP
client, a person in the GUI (debugger windows, menus, device windows,
snapshot load, reset), input replay, or the emulated program itself
(every run and every step changes it).

**While the emulator is paused, an equal `state_rev` means nothing changed
the machine state.** A different value means something may have changed
(the revision can also move without a visible change; never the other way
round).

Machine state = CPU, memory and its mapping (banking, MemExt map), chips,
controllers, inserted media, ROM and hardware configuration. Debugger
configuration (breakpoints, watches, symbols, trace and log settings),
host outputs (screen, sound, files) and host options (speed) are not part
of it: changing them does not move the revision.

## Where you get it

| Where | Field |
|-------|-------|
| `emu_get_state` (`emu_status`) | `state_rev`, from the same moment as `running` and `run_seq` |
| `emu_state_observe` | `state_rev` of the moment the data were read |
| Tools that change the machine state (writes, register and flag changes, I/O, interrupt injection, snapshot load, media, reset, ...) | `state_rev` after the change |
| Events `paused`, `resumed`, `step_done` | `state_rev` after the stop / start / step |
| Event `state_changed` | `state_rev` after a change in pause that did not come from an MCP client |

The value is a large integer (below 2^53, safe in JSON). Compare only for
equality: it is not contiguous, and a new emulator process (restart, a new
pipe session) starts at a different random value, so a revision from an
old process never matches.

While the emulator runs, the program changes the state without moving the
revision. Starting and stopping a run both move it, so a revision read
while running never equals a revision of the paused state. Use the
revision in pause.

## Recipe 1: did anything change? (no download)

```
o = emu_state_observe(windows=[...])        # in pause; keep o.state_rev
... think ...
emu_state_observe(windows=[...], if_state_rev_differs=o.state_rev)
-> {"not_modified": true, "state_rev": <same>, "run_seq": ..., "running": false, "stable": true}
```

With `if_state_rev_differs` and an equal revision in pause, nothing is read
and the reply has only these fields. When the revision differs, or while
running, the full answer comes (with the new `state_rev`).

## Recipe 2: write only if nothing changed since my read

```
emu_pause()
o = emu_state_observe(windows=[{"addr": 0x4F20, "len": 2}])
... decide ...
w = emu_mem_write(0x4F20, "0900", expected_state_rev=o.state_rev)
-> {"addr": 20256, "length": 2, "state_rev": <new>}
emu_set_register("HL", 0x1234, expected_state_rev=w.state_rev)   # chain
```

If anything changed the machine state in between - also somewhere else (a
person stepped the CPU in the GUI, another client wrote a different
address, the banking changed) - nothing is done and the error is

```
Precondition failed: state changed (expected_state_rev=..., current_state_rev=...)
```

While the emulator runs the condition always fails with
`Precondition failed: emulator running (expected_state_rev=...)`: pause
first. Read again, decide again, then retry with the new revision;
retrying the same request unchanged fails again.

Compared with `expected_sha256` / `expected`
(`emulator://docs/conditional_writes`), the revision is stricter: it
refuses also after a change elsewhere and after a change that was undone
(ABA), and it catches a banking change that the content hash of CPU
addresses cannot see. It needs a paused emulator. Both conditions can be
used in one call.

## Event `state_changed`: a person changed something

`emu_event_subscribe(["state_changed"])` reports changes of the machine
state **in pause** that did not come from an MCP client and are not a
run or a step (those report `paused`, `resumed`, `step_done`):

```
{"topic": "state_changed", "data": {"state_rev": <new>, "sources": ["debugger"]}}
```

Changes of one pass of the emulator's pause loop are merged into one
event (editing memory byte by byte in the GUI gives one event, not one
per byte). `sources` names where the changes came from:

| Source | Change |
|--------|--------|
| `debugger` | a debugger window in the GUI (memory, registers, flags, I/O, ...) |
| `reset` | reset from the menu or a key |
| `snapshot` | snapshot Load / Quick Load in the GUI, or a snapshot load inside a replay request |
| `device` | a device window or menu (disk, tape deck settings, RAM disk, MemExt map, ROM, DIP switch, ...) |
| `replay` | a replay request (record, play, stop, seek, goto) - also your own `emu_replay_*` calls, which do not go through the command queue |
| `input` | virtual keyboard or autotype from the GUI |
| `tape` | a tape operation from the GUI |
| `mcp` | listed only together with another source in the same pass |

Your own MCP calls give no event (their responses carry `state_rev`);
another MCP client's calls give none either - compare `state_rev` if
several clients work at once. Changes while running are not reported:
the `paused` event carries the revision after the run.

## Which tools accept `expected_state_rev`

Every tool that changes the machine state: `emu_mem_write`,
`emu_mem_write_force`, `emu_region_write`, `emu_set_register`,
`emu_set_cpu_flags`, `emu_io_write`, `emu_io_read` (a port read can change
a chip), `emu_irq_inject`, `emu_nmi_inject`, `emu_snapshot_load`,
`emu_snapshot_load_buffer`, `emu_media_insert`, `emu_media_eject`; on the
JSONL protocol also `reset`, `media_load_mzf`, `media_load_binary`,
`media_run_mzf`, `input_*`, `cmt_*` state changes, `settings_set`,
`platform_set`, `periph_attach`, `periph_detach`, `freeze_add` and
`set_pioz80_interrupt_vector`. These tools also return `state_rev`. Other
tools refuse the parameter with `Invalid parameters: expected_state_rev is
accepted only by commands that change the machine state`.

Rules:

- **Order of checks.** Parameter errors come first. Then the emulator
  thread checks the revision before anything else, so a stale revision
  wins over the tool's own condition (`expected_sha256`, `expected`).
- **Tools of several steps** (`emu_input_send_keys`, a key press with
  hold frames, `media_run_mzf`) check the revision only at their first
  state change; later steps run on the state they created themselves.
- **Replay.** A refused call is not executed and not recorded in an input
  recording. During playback, state-changing tools are refused as before
  (`emulator://docs/replay`).
- **GUI.** The MCP Activity window shows a refused call as a row with
  "rejected: state changed since it was read".

## Known gaps

These changes do not move the revision yet:

- keys typed on the host keyboard into the emulator window, and the
  joystick on the numeric keypad, while no input recording runs,
- the MZ-1P16 plotter buttons Paper feed and Run drawing self-test.

If a person may type into the emulator window during your work, do not
rely on an equal revision alone for keyboard state.

## See also

- `emulator://docs/state_observe` - reading registers and memory from one
  moment.
- `emulator://docs/conditional_writes` - content conditions
  `expected_sha256` and `expected`.
- `emulator://docs/run_control` - `run_seq`, events `paused` / `resumed`,
  `emu_event_subscribe` / `emu_event_poll`.
- `emulator://docs/error_handling` - `Precondition failed:` and retries.
