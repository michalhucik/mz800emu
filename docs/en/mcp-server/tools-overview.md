# MCP tools overview

The MCP server exposes a set of tools (= callable RPCs) for controlling
the emulator. This page is an overview - each tool is described
briefly, with a focus on what it does, what arguments it takes, and
whether it is destructive. Resources (= read-only URI endpoints) are
documented separately in [Resources overview](resources-overview.md).

## Tool list

| Tool | Sensitive? | Description |
|------|-----------|-------------|
| `emu_status` | no | Emu state (running / paused, connected) |
| `emu_ping` | no | Liveness check (`pong`) |
| `emu_pause` | no | Pauses emulation |
| `emu_run` | no | Blocking resume for N frames (1..1000) |
| `emu_reset` | no | Power-on reset of CPU + peripherals |
| `emu_get_registers` | no | 14 Z80 registers (AF..IR) |
| `emu_set_register` | **YES** | Write 16-bit value to register - **destructive** |
| `emu_dasm` | no | Disassemble N instructions from an address (banking-aware) |
| `emu_history_get` | no | 32 most recently executed instructions (ring buffer) |
| `emu_mem_read` | no | Reads bytes from Z80 memory (base64) |
| `emu_mem_write` | **YES** | Writes bytes into RAM - region checked, **destructive** |
| `emu_bp_add` | no | Adds a breakpoint (exec, or typed memw/memr/ior/iow + condition) |
| `emu_bp_list` | no | Lists breakpoints (id/addr/enabled + type/zone/bank_id/hits/condition) |
| `emu_bp_remove` | no | Removes a specific BP by ID |
| `emu_bp_clear` | no | Removes all breakpoints at once |
| `emu_bp_enable` | no | Toggles BP enabled flag (no removal) |
| `emu_step_into` | no | One Z80 instruction (steps INTO CALL/RST) |
| `emu_step_over` | no | One instruction, CALL/RST as atomic |
| `emu_step_n` | no | N steps as sequence of step_into (1..1000) |
| `emu_run_until_addr` | no | Run until PC == addr (uses temp BP) |
| `emu_snapshot_save` | **YES** | Saves a .mzs snapshot to disk (overwrites) |
| `emu_snapshot_save_buffer` | no | Saves snapshot as inline base64 (no disk I/O) |
| `emu_snapshot_load` | **YES** | Loads .mzs from disk, **replaces emu state** |
| `emu_snapshot_load_buffer` | **YES** | Loads snapshot from inline base64, replaces state |
| `emu_cooperation_hint_set` | no | AI self-binding hint (read_only / paused_only / free) |
| `emu_symbol_add` | no | Adds a user-defined symbol (named address) |
| `emu_symbol_remove` | no | Removes a symbol (by name or by addr) |
| `emu_symbol_lookup` | no | Looks up a symbol by name or hex address (read-only) |
| `emu_symbol_list` | no | Lists symbols matching name prefix (max 1000) |
| `emu_bookmark_add` | no | Adds a named address bookmark (hex literal or symbol), owner mcp |
| `emu_bookmark_remove` | no | Removes a bookmark by ID |
| `emu_step_out` | no | Runs until RET from the current subroutine (callstack pop) |
| `emu_run_until_raster` | no | Runs until the GDG raster reaches a target line (and optional column) |
| `emu_run_until_tstate` | no | Runs until the absolute Z80 cycle counter reaches a target |
| `emu_run_until_event` | no | Runs until an event fires (`frame_done`, `breakpoint_hit`) |
| `emu_event_subscribe` | no | Subscribe to a list of topics (`breakpoint_hit`, `paused`, `step_done`, `io_write`) |
| `emu_event_unsubscribe` | no | Unsubscribe topics (empty list = all) |
| `emu_event_poll` | no | Retrieve pending events (timeout 0..60000 ms, max 1..100) |
| `emu_trap_respond` | no | Respond to a TRAP from a `breakpoint_hit` event (continue/step_into/step_over/abort) |
| `emu_io_read` | **YES** | Z80 IN port - real path with side effects, **destructive** |
| `emu_io_write` | **YES** | Z80 OUT port - real path with side effects, **destructive** |
| `emu_irq_inject` | **YES** | Force maskable IRQ (optional IM2 vector), **destructive** |
| `emu_nmi_inject` | **YES** | Force NMI - jumps to 0x0066, **destructive** |
| `emu_mem_write_force` | **YES** | Write bypassing region check (skips ROM safety), **destructive** |
| `emu_watch_add` | no | Adds a watch row (address / expr_scalar / expr_deref) |
| `emu_watch_remove` | no | Removes a watch row (by name or index) |
| `emu_watch_list` | no | Returns all watches with current values |
| `emu_watch_eval` | no | Evaluates a watch or ad-hoc expression |
| `emu_callstack_get` | no | Snapshot of the shadow stack |
| `emu_cdl_start` | no | Start Code/Data Logger |
| `emu_cdl_stop` | no | Stop CDL recording (data preserved) |
| `emu_cdl_reset` | no | Clear CDL counters |
| `emu_cdl_export` | no | Export CDL bitmap + meta to files |
| `emu_trace_start` | no | Start trace recording of a channel (cputrack/iorqlog/intlog/hwlog) |
| `emu_trace_stop` | no | Stop trace recording of a channel (segment closed) |
| `emu_trace_reset` | no | Clear the current trace channel segment |
| `emu_trace_save` | no | Save/redirect a trace channel segment (optional path) |
| `emu_profiler_start` | no | Start CPU profiler (hot-path overhead) |
| `emu_profiler_stop` | no | Stop profiler (data preserved) |
| `emu_profiler_reset` | no | Clear profiler aggregator |
| `emu_profiler_export` | no | Export profile to CSV or JSON file |
| `emu_profiler_get` | no | Inline JSON with entries + global stats |
| `emu_media_load_mzf` | no | CMT-hack instant load of MZF (header + body) into RAM at LOAD_ADDR (path or bytes_b64) |
| `emu_media_run_mzf` | **YES** | Load MZF + ROM disconnect + JP STRT (composite, autentic Sharp ROM Monitor LOAD handover) |
| `emu_media_load_binary` | **YES** | Raw bytes from file to Z80 memory, **destructive** |
| `emu_media_insert` | **YES** | Insert image into slot (auto-eject when already mounted) |
| `emu_media_eject` | no | Eject image from slot |
| `emu_media_state` | no | Snapshot of all 11 slots (cmt/fdc0_fd0..3/fdc1_fd0..3/qd/ide8) |
| `emu_settings_set` | **YES** | Live INI write (whitelist); **mutates global state** |
| `emu_settings_get` | no | Read INI key + type (open read, no whitelist) |
| `emu_platform_set` | **YES** | Platform switch attempt - returns error (compile-time MZARCH) |
| `emu_periph_attach` | **YES** | Activates a peripheral module (`requires_restart`) |
| `emu_periph_detach` | **YES** | Deactivates a peripheral module (`requires_restart`) |
| `emu_stop` | **YES** | Hot-swap stop (pipe-only); terminates emu, Python wrapper keeps running |
| `emu_start` | **YES** | Hot-swap start (pipe-only); spawns fresh mz800emu.exe child |
| `emu_input_send_key` | **YES** | Press key + hold N frames + release |
| `emu_input_send_keys` | **YES** | Sequence of keys (ascii or key_names encoding) |
| `emu_input_press_key` | **YES** | Persistent press with no auto-release (= SHIFT hold) |
| `emu_input_release_key` | **YES** | Release specific key or all keys |
| `emu_input_send_joystick` | **YES** | 8-bit joystick mask + hold + release |
| `emu_input_send_keys_with_delays` | **YES** | Timing-controlled event sequence |
| `emu_get_reg` | no | Read a single Z80 register (cheaper than `emu_get_registers`) |
| `emu_force_pause` | no | Force-pause the emulator (bypass standard pause path) |
| `emu_set_user_cycle_origin` | **YES** | Reset the User cycle counter origin (mutates debug state) |
| `emu_get_im2_vector` | no | Tool variant of the IM2 ISR vector snapshot (PIO-Z80 platforms) |
| `emu_get_raster_pos` | no | Current GDG raster position + Z80 cycle counters |
| `emu_get_cpu_flags` | no | Auxiliary CPU state (IFF/IM/HALT/INT/NMI/EI delay/Q/cycle counters/I/R) |
| `emu_set_cpu_flags` | **YES** | Selective write of IFF1/IFF2/IM/I/R (update_mask from field presence) |
| `emu_get_last_instr` | no | Most recently completed Z80 instruction from the history ring (addr + bytes + length) |
| `emu_get_cpu_panel_batch` | no | Atomic batch: regs + flags + raster + IM2 + last_instr in one round-trip |
| `emu_debugger_activate` | **YES** | Programmatically activate the debugger (side effect: cpuhist + mhmap recording in WITH_WINDOW preset) |
| `emu_debugger_deactivate` | **YES** | Programmatically deactivate the debugger (recording off) |
| `emu_is_debugger_active` | no | Read-only probe for `TEST_DEBUGGER_ACTIVE` |
| `emu_set_pioz80_interrupt_vector` | **YES** | Override the IM2 vector in PIO-Z80 port A/B (MZ-800/MZ-1500; MZ-700 returns `available:false`) |
| `emu_bp_create_with_init` | **YES** | Atomic create + init of breakpoint fields (= `breakpoints_add_auto` + selective UPDATE in one safepoint) |
| `emu_bp_set_parent` | **YES** | Quick reparent a breakpoint into a group (drag-drop semantics) |
| `emu_bp_update` | **YES** | Selectively update fields of an existing breakpoint via a `fields[]` array |
| `emu_bpgrp_add` | **YES** | Add a new breakpoint group (name + parent) |
| `emu_bpgrp_remove` | **YES** | Remove a breakpoint group (cascading delete handled by the backend) |
| `emu_bpgrp_update` | **YES** | Selectively update group fields (enabled/name/colors/parent) |
| `emu_stack_history_enable` | **YES** | Enable/disable SP history recording (disable flushes the ring buffer) |
| `emu_stack_history_reset` | **YES** | Flush the SP history ring buffer (recording flag is preserved) |
| `emu_stack_regions_add` | **YES** | Add a monitored stack region (name + base + limit) |
| `emu_stack_regions_edit` | **YES** | Edit an existing region (on success the watermark + counters are reset) |
| `emu_stack_regions_remove` | **YES** | Remove the region at a given index |
| `emu_stack_regions_reset_watermark` | **YES** | Reset watermark + push/pop counters of a single region |
| `emu_eventlog_start` | no | Start Event Viewer recording |
| `emu_eventlog_stop` | no | Stop recording (ring contents preserved) |
| `emu_eventlog_clear` | **YES** | Clear the event ring buffer |
| `emu_eventlog_set_capacity` | **YES** | Resize ring (clamp to min/max) - discards current content |
| `emu_eventlog_set_mask` | **YES** | 64-bit categories filter bitmask |
| `emu_eventlog_get_event` | no | Read event[idx] - pxclk/screen/category/pc/payload |
| `emu_regions_list` | no | Enumerate physical memory regions (ROM, VRAM, CG-ROM, MemExt banks, ...) bypassing Z80 banking |
| `emu_region_read` | no | Raw no-side-effect read from a specific region (= equivalent of the GUI Memory browser for headless debug) |

## Sensitive operations

Tools marked **sensitive** carry an explicit WARNING in their MCP
schema description about their destructive nature; the MCP client
(Claude Code, etc.) should ask the user for confirmation (Claude Code
handles permission per tool category by default).

Sensitive tools include:

- **Memory / I/O / interrupts** - `emu_mem_write`, `emu_mem_write_force`,
  `emu_io_read`, `emu_io_write`, `emu_irq_inject`, `emu_nmi_inject`
- **Snapshot operations** - `emu_snapshot_save` (overwrites a file),
  `emu_snapshot_load` and `emu_snapshot_load_buffer` (replace emu
  state)
- **Media operations** - `emu_media_load_binary` (raw RAM write),
  `emu_media_insert` (auto-eject overwrites mounted image)
- **Configuration** - `emu_settings_set` (mutates global state),
  `emu_platform_set`, `emu_periph_attach`, `emu_periph_detach`
- **Hot-swap** - `emu_stop`, `emu_start`
- **HID input** - all `emu_input_*` tools (user simulation may produce
  unintended behavior)
- **BP advanced** - `emu_bp_create_with_init`,
  `emu_bp_set_parent`, `emu_bp_update`, `emu_bpgrp_add`,
  `emu_bpgrp_remove`, `emu_bpgrp_update` (mutate `g_breakpoints` /
  `g_bp_groups` debugger structures)
- **Stack analytics** - `emu_stack_history_enable`,
  `emu_stack_history_reset`, `emu_stack_regions_add`,
  `emu_stack_regions_edit`, `emu_stack_regions_remove`,
  `emu_stack_regions_reset_watermark` (mutate recording flag, ring
  buffer, region definitions + counters)

For `emu_mem_write` a **region check** applies - the target address
must lie in a RAM region (not ROM, CG-ROM, prohibited, unmapped). If
not, the tool fails with `MEM_WRITE region check failed`. For explicit
fault injection (= writes into regions outside RAM) use
`emu_mem_write_force`.

## Error return convention

On a backend failure every tool returns an object with an `error`
field (`{"error": "<description>"}`). Previously some tools returned a
bare empty response (`{}`) in such a case, so the client could not tell
that the call had failed. Now an error is always explicit - the client
can reliably test for the presence of `error` in the response. On
success the response carries the tool-specific data fields (see the
descriptions below); a successful response has no `error` field.

The `error` text also tells whether the emulator executed the command:

- `Emulator busy: command not executed ...` / `Emulator busy: command
  queue full ...` - the command was **not** executed (the emulator
  thread did not pick it up within 10 s, or the queue was full); the
  emulator state is unchanged and the call is safe to retry.
- `Emulator busy: command only partially executed ...` - a multi-step
  tool stopped after an earlier step ran; check the state before
  retrying.
- `Emulator is shutting down ...` - not executed, stop sending requests.
- any other text (e.g. `bp_remove failed (unknown id?)`) - the command
  was executed and failed (or the parameters were rejected); retrying
  unchanged does not help.

The `Emulator busy: ...` / `Emulator is shutting down ...` texts end
with the tool's original message in square brackets (kept for
compatibility), e.g. `... safe to retry [bp_remove failed (unknown id?)]`;
match on the prefix. Once the emulator thread picks a command up, the
call waits for it to finish and returns its real result. Details for AI
clients: `emulator://docs/error_handling`.

## Per-tool description

### `emu_status`

Returns emulator state: whether it is running or paused, how many
frames have elapsed, whether the transport is connected.
Non-destructive, suitable as the first call after connecting.

### `emu_ping`

Returns `pong`. A quick liveness check (= "is the bridge still up?")
that does not depend on emu state.

### `emu_pause`

Pauses CPU emulation. The tool returns once the pause is confirmed
(= the next call reads consistent state). No arguments.

### `emu_run`

Resumes emulation for a specific number of video frames (1-1000)
and **blocks** until N frames have elapsed. After N frames the
emu pauses automatically. Argument:

- `frames` (int, 1..1000) - how many frames to advance

If `frames` exceeds the limit, the tool returns an error.

**Blocking semantics:** The tool returns only after N frames have
completed, not immediately after unpause. The emulator stops ITSELF
deterministically exactly at the N-th frame boundary (emu-side
frame-bounded stop), not via an asynchronous pause from the dispatch
thread. Running the same number of frames from the same state is therefore
bit-reproducible - important when tracing timing-sensitive programs. For
PAL (= 50 Hz) `emu_run(frames=100)` blocks for ~2 s; for NTSC (= 60 Hz)
~1.67 s.

Response contains:

- `running: false` - emu is paused after completion
- `actual_frames` (int) - actual frames that elapsed (= N, fewer if stopped
  by a breakpoint or a safety timeout)
- `requested_frames` (int) - what was requested
- `stopped_by` (string) - why it stopped: `"frames"` (ran the full count),
  `"breakpoint"` (a breakpoint paused it), `"manual"` (manual/UI pause),
  `"timeout"` (safety timeout), `"unknown"` (other)
- `complete` (bool) - true only if the full frame count was reached
  (= `stopped_by` is `"frames"`)

If `frames == 0` (or missing), the tool just submits unpause and
returns `running: true` asynchronously (= legacy fire-and-forget,
kept for backward compatibility with older clients).

Safety timeout: 2x expected wallclock + 100 ms. For N=1000 frames
maximum blocking is ~40 s; on timeout returns `complete=false`
with `actual_frames < requested`.

### `emu_reset`

Performs a power-on reset of CPU and all peripherals. RAM state is
reset to default contents, registers are zeroed, PC is set to the
ROM entry point.

### `emu_get_registers`

Returns state of 14 Z80 registers: `AF`, `BC`, `DE`, `HL`, `IX`, `IY`,
`SP`, `PC`, `AF'`, `BC'`, `DE'`, `HL'`, `I`, `R`. Values as unsigned
hex strings.

### `emu_set_register` (sensitive)

Overwrites a single 16-bit Z80 register. Arguments:

- `reg` (string) - register name, case-insensitive. Accepts `AF`,
  `BC`, `DE`, `HL`, `AF_` / `AF2`, `BC_` / `BC2`, `DE_` / `DE2`,
  `HL_` / `HL2`, `IX`, `IY`, `SP`, `PC`, `WZ`, `IR`.
- `value` (int, 0..65535) - new value. For `IR` only the lower byte
  (R) is written; I keeps its previous value.

**Destructive operation.** Writing to `PC` changes the execution
path; writing to `SP` invalidates the current call stack. Save a
snapshot before experimenting. Typical use case "jump to address" is
`reg="PC", value=<addr>`; verify with `emu_get_registers` afterward.

### `emu_mem_read`

Reads N bytes from Z80 memory starting at the given address. Arguments:

- `address` (int, 0..0xFFFF) - start address
- `length` (int, 1..65536) - number of bytes

Returns base64-encoded data. No region check - anything readable
by the CPU (ROM, RAM, peripheral mapped, ...) is returned. Banking
is respected (= you read what the CPU would see at that address).

### `emu_mem_write` (sensitive)

Writes bytes to a Z80 address. **Destructive**. Arguments:

- `address` (int, 0..0xFFFF) - start address
- `data` (base64 string) - bytes to write

Enforces a region check (= RAM only). If the address is not in RAM,
the tool fails with `MEM_WRITE region check failed`.

### `emu_bp_add`

Adds a breakpoint. By default an execution BP (= pause when PC =
address), but it also supports typed (memory / I/O) BPs and a
condition. Arguments:

- `address` (int, 0..0xFFFF) - BP address (PC for exec; watched
  address / port for typed)
- `type` (string, optional, default `"exec"`) - BP type: `"exec"`
  (execution), `"memr"` (memory read), `"memw"` (memory write),
  `"ior"` (I/O IN), `"iow"` (I/O OUT)
- `condition` (string, optional) - condition expression (bp_expr DSL,
  e.g. `A == 0x42`). Empty = unconditional BP.

A plain exec BP without a condition takes the lightweight path. A typed
BP or a BP with a condition is transparently created via
`emu_bp_create_with_init` (= the single typed-breakpoint code path) -
there is no need to call create manually. Returns the breakpoint handle
(ID).

### `emu_bp_list`

Returns the list of current breakpoints. Return:
`{"count": <int>, "breakpoints": [...]}`, where each record carries:

- `id` (int) - breakpoint handle
- `addr` (int) - address
- `enabled` (bool)
- `type` (string) - canonical UPPER_SNAKE type (`PC_EXEC` / `MEM_R` /
  `MEM_W` / `IORQ_R` / `IORQ_W` / ...)
- `zone` (string) - memory zone (`CPU_VIEW` / `RAM` / ...)
- `bank_id` (int) - bank index (for the `MMEXT_BANK` zone)
- `hits` (int) - BP trigger counter
- `condition` (string or null if unconditional)

### `emu_bp_remove`

Removes a specific breakpoint by its ID. Arguments:

- `id` (int) - ID obtained from `emu_bp_add` or `emu_bp_list`

Returns `{"id": <int>, "removed": true}` on success. If the ID
does not exist, the tool returns an error.

### `emu_bp_clear`

Removes all current breakpoints at once. No arguments. The
implementation is two-phase (BP_LIST + N x BP_REMOVE), so it is
not fully atomic - a GUI could add a BP between calls and that
one would not be removed (best-effort). Returns
`{"count": <int>, "cleared": true}` with the actual removed count.

### `emu_bp_enable`

Toggles the BP enabled flag without removing the entry. Useful
for temporarily silencing a BP you want to reactivate later
(no need to remember its address). Arguments:

- `id` (int) - breakpoint ID
- `enabled` (bool) - true = active, false = kept in the list
  but inactive

### `emu_step_into`

Executes exactly one Z80 instruction. For CALL/RST the next
instruction is at the destination address (= steps INTO the
subroutine, unlike `emu_step_over`). Requires paused state - if
the emu is running, the tool first pauses it and the step does
NOT execute (the client must call again).

### `emu_step_over`

Like `emu_step_into` for ordinary instructions, but for
CALL/RST/DJNZ and block instructions a temporary breakpoint is
placed at addr+length and run-to executes (= subroutine becomes
one logical step). Requires paused state.

### `emu_step_n`

Executes N instructions as a sequence of `emu_step_into`. Argument:

- `count` (int, 1..1000) - number of steps

If any step fails mid-loop (= BP hit, another reason for the
pause), the tool returns the partial counter with `partial=true`
in the response. Return value: `{"count": <int>, "requested":
<int>, "partial": <bool>}`.

### `emu_run_until_addr`

Places a temporary BP at the target address and resumes
emulation. The emu pauses when PC reaches `addr` (or hits another
permanent BP first). Arguments:

- `addr` (int, 0..65535) - target PC address
- `max_cycles` (int, optional, default 10M) - informative timeout
  in T-states; the client can poll `emu_status` and pause itself
  if the run takes too long.

Requires paused state.

### `emu_snapshot_save` (sensitive - overwrites a file)

Saves the full emulator state into a .mzs file on disk. Arguments:

- `path` (string) - filesystem path for the .mzs file
- `description` (string, optional) - human-readable label embedded
  in the snapshot metadata

Requires paused state. Response:
`{"path": <str>, "ok": true, "result_code": 0}`.

### `emu_snapshot_save_buffer`

Same as `emu_snapshot_save` but the resulting .mzs ZIP is returned
inline as a base64 string instead of being written to disk. Useful
when the AI client cannot or does not want to write to the emulator
host filesystem. Argument:

- `description` (string, optional)

Response: `{"bytes_b64": <str>, "size": <int>, "ok": true}`. The client
may keep the `bytes_b64` for later restore via
`emu_snapshot_load_buffer`.

### `emu_snapshot_load` (sensitive - replaces emu state)

Loads a .mzs file. After success the emu stays paused at the snapshot's
captured PC. Argument:

- `path` (string) - filesystem path to the .mzs file

Response: `{"path": <str>, "ok": true, "result_code": 0}`.

### `emu_snapshot_load_buffer` (sensitive)

Loads a snapshot from an inline base64 buffer (typically produced by a
prior `emu_snapshot_save_buffer`). Argument:

- `bytes_b64` (string) - base64-encoded .mzs ZIP

Response: `{"size": <int>, "ok": true, "result_code": 0}`.

### `emu_cooperation_hint_set`

A self-binding instruction the AI client voluntarily stores on the
server to constrain itself between requests (MCP is per-request
stateless). The server only persists the hint and broadcasts a UI
notification - **no hard enforcement** (= the AI may violate the hint,
the violation is recorded in the audit log).

Arguments:

- `mode` (string, required) - one of:
    - `free` - no constraint (clear)
    - `read_only` - AI will only call read-only tools/resources
    - `paused_only` - AI will only call tools while the emu is paused
- `until` (string, optional) - ISO 8601 timestamp or natural-language
  phrase ("next user message", "30 min"). Empty = open-ended.

Response: `{"mode": <str>, "until": <str>, "ok": true}`.

### `emu_symbol_add`

Adds a user-defined symbol (named address) into the symbol table. If a
symbol with the same name already exists, it is overwritten. Symbols
are visible in the disassembler view, breakpoint expressions, and
other AI introspection paths.

Arguments:

- `addr` (int, required) - 0..65535
- `name` (string, required) - identifier from [A-Za-z0-9_.], no whitespace
- `comment` (string, optional) - human-readable annotation
- `kind` (string, optional) - "LABEL" / "DATA" / "BOOKMARK" (default
  "LABEL"). Currently echo-only - storage always uses the user-label
  source kind.

Response: `{"added": true, "addr": <int>, "name": <str>, "kind": <str>}`.

### `emu_symbol_remove`

Removes a symbol by name or by address. Exactly one of `name` / `addr`
must be supplied.

Arguments:

- `name` (string, optional) - identifier
- `addr` (int, optional) - 0..65535 (`-1` means "not supplied")

Response: `{"removed": <bool>, "name": <str>}` or `{"removed": <bool>,
"addr": <int>}` depending on which key was supplied.

### `emu_symbol_lookup`

Read-only lookup of a single symbol. The `query` argument is
auto-detected:

- hex string (`0x4242`, `4242h`, `$4242`) -> lookup by address
- otherwise -> lookup by name (case-sensitive)

Returns the highest-priority symbol (user label > MAP > NOI >
SJASMPLUS).

Response: `{"found": true, "addr": <int>, "name": <str>, "comment":
<str>, "source": <int>}` or `{"found": false}`. `source` is a numeric
code (0=SJASMPLUS, 1=NOI, 2=MAP, 3=user label).

### `emu_symbol_list`

Lists symbols matching a name prefix. Useful for AI introspection of
the symbol table (e.g. all `ROM_*` routines).

Arguments:

- `prefix` (string, optional) - prefix filter (default "" = all)
- `limit` (int, optional) - max entries (1..1000, default 100)

Response: `{"count": <int>, "items": [{"addr", "name", "comment",
"source"}, ...]}`.

### `emu_bookmark_add`

Adds a named address bookmark to the debugger bookmark store. Bookmarks
are surfaced in the debugger UI and via the `emulator://bookmarks`
resource. The `input` is resolved to a 16-bit address dynamically: it
may be a hex literal (`$1234`, `0x1234`, `#1234`, `1234h`, `1234`) or a
symbol name from the symbol database. A symbol name lets the bookmark
follow the symbol if its address later changes. The bookmark is tagged
with the `mcp` owner.

Arguments:

- `input` (string, required) - hex literal or symbol name
- `comment` (string, optional) - human-readable note

Response: `{"id": <int>, "input": <str>, "comment": <str>, "addr":
<int|null>, "addr_resolved": <bool>, "owner": "mcp"}`. `addr` is `null`
when `input` cannot be resolved (e.g. unknown symbol) - the bookmark is
still stored and resolves lazily later.

### `emu_bookmark_remove`

Removes a bookmark by its ID. IDs are monotonic and never reused within
a session; obtain them from the `emulator://bookmarks` resource.
Removing a non-existent ID is not an error - it returns `removed=false`.

Arguments:

- `id` (int, required) - bookmark ID (>= 1)

Response: `{"id": <int>, "removed": <bool>}`.

### `emu_step_out`

Runs until the Z80 returns (RET) from the current subroutine.
Implementation: looks up the top frame in the shadow callstack, takes
its `return_addr`, installs a temporary breakpoint, and starts run-to.

Preconditions:

- Callstack tracking must be active (toggle in the Callstack debugger
  window or pass the `--callstack` CLI flag).
- The emulator must be paused before invocation.

Arguments:

- `max_cycles` (int, optional, default 10000000) - informational
  T-state cap.

Response: `{"return_addr": <int>, "max_cycles": <int>, "running": true}`
or an error if callstack tracking is inactive, the stack is empty, or
the emulator is already running.

### `emu_run_until_raster`

Spins up the emulator and alternates step_into with raster snapshots
until the GDG beam reaches the target scanline (and optionally column).
Useful for debugging mid-frame raster effects (palette swaps, scroll
register writes, BCOL changes) timed against the video signal.

Arguments:

- `line` (int, 0..511) - target scanline. MZ-800 PAL frame has 312
  lines, NTSC has 262.
- `col` (int, optional, -1..2047, default -1) - target GDG column.
  -1 means "any column on the target line".
- `max_cycles` (int, optional, default 10000000) - safety cap on the
  polling loop's T-state budget.

Precision is bounded by Z80 instruction length (4-23 T-states ~
+/-10 GDG ticks ~ +/-5 visible pixels). Sufficient for mid-frame
effects.

Response: `{"scanline": <int>, "column_pixel": <int>, "frame_number":
<int>, "total_cycles": <int>, "delta_cycles": <int>, "reached":
<bool>}`. `reached=false` means the polling loop timed out.

### `emu_run_until_tstate`

Runs a polling loop until the absolute Z80 cycle counter reaches the
target. The cycle counter (`total_cycles` in the raster snapshot) is
a monotonically increasing 32-bit T-state count since CPU reset.

Arguments:

- `target_total_cycles` (int) - absolute target cycle counter. **Must
  be greater than the current value** (the server returns an error
  for past targets).
- `max_cycles` (int, optional, default 10000000) - safety cap on delta
  T-states from the start of the polling loop.

Note: `total_cycles` is a uint32 that wraps roughly every 20 minutes
at 3.5 MHz. Wraparound across the boundary is not handled - typical
clients target deltas in the order of seconds.

Response: `{"total_cycles": <int>, "target": <int>, "delta_cycles":
<int>, "reached": <bool>}`.

### `emu_run_until_event`

Runs a polling loop until a specific event fires. Supported kinds:

- `"frame_done"` - N video frames have elapsed. `params`: `{"count":
  <int>}` (default 1, range 1..10000).
- `"breakpoint_hit"` - starts the emulator and waits until a
  breakpoint pauses it (or the safety cap fires). `params` may
  contain `{"id": <int>}` (currently ignored - any pause satisfies
  the wait).

Arguments:

- `kind` (string) - see above
- `params` (object, optional) - kind-specific
- `max_cycles` (int, optional, default 10000000) - safety cap

Response: `{"kind": <str>, "reached": <bool>, "delta_cycles": <int>,
...}` plus kind-specific fields (`frames_done` for `frame_done`,
`paused` for `breakpoint_hit`).

### `emu_io_read` (sensitive)

Z80 IN port - **real read with full side effects** on the chip
(PSG status flag reset, FDC IDX strobe, GDG DMD strobe). Args:

- `port` (int, 0..65535)

Returns `{"port": <int>, "value": <0..255>}`.

### `emu_io_write` (sensitive)

Z80 OUT port - **destructive write** through the full Z80 OUT path
(PSG latch, FDC command, GDG mode register, PIO output bits, ...).
Args:

- `port` (int, 0..65535)
- `value` (int, 0..255)

Returns echo `{"port": <int>, "value": <int>}`. If the client
subscribes to topic `io_write`, every OUT from the Z80 hot path
(= even outside of this Tool) emits an event with
`{port, value, cycles}`.

### `emu_irq_inject` (sensitive)

Force maskable IRQ. Args:

- `source` (string, default `"manual"`) - audit label
- `vector` (int, -1 or 0..255) - IM2 vector; `-1` uses the chip's
  default vector callback

The actual acceptance depends on IFF1 (= when `DI`, the IRQ is
latched and accepted on the next `EI`). Returns
`{"injected": true, "source": "...", "vector_used": <int|null>}`.

### `emu_nmi_inject` (sensitive)

Force NMI. No args. After the current instruction completes the
CPU jumps to 0x0066, copies IFF1 to IFF2 and clears IFF1. NMI is
**non-maskable** - always accepted. Returns `{"injected": true}`.

### `emu_mem_write_force` (sensitive)

Write bytes to a Z80 address **without region check**. Input
validation is identical to `emu_mem_write`, but no filter on
ROM/CG-ROM regions is applied - bytes are written through the
banking-aware path, i.e. into RAM mapped under ROM, or remain
unchanged where ROM is currently mapped.

Args:

- `addr` (int, 0..65535)
- `data_hex` (string, even-length hex)

Returns `{"addr": <int>, "length": <int>}`. To actually patch ROM
(= bypass the ROM mapping), banking must be switched first via
`emu_io_write` on ports 0xE0..0xE4.

## Watch tools

Watch tools wrap the existing debugger Watch panel. Watches are
useful for long-running observation of a memory cell or expression -
instead of polling with `emu_mem_read` repeatedly, the client
registers a watch once and reads all current values with a single
`emu_watch_list` call.

Each watch row has one of three modes:

- `address` - literal Z80 address + type (default)
- `expr_scalar` - expression evaluated as int32, `type` controls
  display format only (no extra memory read)
- `expr_deref` - expression evaluated as uint16 address, then a read
  of size given by `type` is performed (like `address` but the
  address is dynamic)

Supported types (`type` argument): `u8` (default), `i8`, `u16le`,
`u16be`, `i16le`, `i16be`, `u32le`, `u32be`, `i32le`, `i32be`, `bit`,
`ascii`, `mzascii`, `bytes`.

### `emu_watch_add`

Add a watch row to the storage. Arguments:

- `name` (string, optional) - row name (empty = anonymous)
- `addr` (int, optional, 0..65535) - only for mode=address
- `mode` (string, optional) - "address" / "expr_scalar" / "expr_deref"
- `expr` (string, optional) - expression text (required for expr_*
  modes)
- `type` (string, optional, default "u8")

Returns `{"index": <int>, "name", "mode", "type", "addr"}`.

### `emu_watch_remove`

Remove a watch row by `name` or `index`. Returns
`{"removed": <bool>, "index": <int>}`. `removed=false` = not found.

### `emu_watch_list`

Return all currently registered watches with their current values:
`{"count": <int>, "items": [{"index", "name", "mode", "type", "addr",
"expr", "value"}, ...]}`. `value` is the formatted display string
according to the row's type + display format.

Cap: 256 rows (= a practical limit for an interactive watch UI).

### `emu_watch_eval`

Evaluate either an existing watch (via `name` or `index`) or an
ad-hoc expression (`expr`). When `expr` is supplied, the expression
is parsed and evaluated without being persisted.

Returns `{"value_str": "...", "value_int": <int>, "error": ... | null}`.

## Callstack tool

### `emu_callstack_get`

Return a snapshot of the shadow stack. The shadow stack pushes on
CALL / RST / IRQ accept / NMI and pops on RET / RETI / RETN,
independent of the Z80 SP. It allows tracking call depth even when
the Z80 stack contains transient pushes (typical pattern: CP/M BDOS
dispatch via PUSH+RET trampoline).

Arguments:

- `max_depth` (int, optional, 1..256, default 64) - cap the number of
  frames returned (the top N are emitted).

Returns `{"active", "count", "current_depth", "max_depth_reached",
"divergence_count", "overflow_count", "cycles_now", "frames": [...]}`.
Each frame holds `depth` (0 = top), `return_addr`, `call_site_addr`,
`target_addr`, `sp_at_entry`, `cycles_at_entry`, `kind` (string).

`kind` is one of: `call` / `rst` / `irq_im0` / `irq_im1` / `irq_im2`
/ `nmi` / `synthetic`.

When `active=false`, the Callstack subsystem is disabled - activation
goes through GUI Settings or the `--callstack` CLI flag. Without an
active subsystem, `count` is always 0.

## CDL tools

CDL = Code/Data Logger. Per-byte classification of memory accesses
(Read / Write / eXecute / Stack-write) built on the Memory Heatmap
subsystem. FCEUX-style CDL bitmap is `counter > 0`.

### `emu_cdl_start`

Start CDL recording. Triggers a CPU callback swap onto the slow path
with a logging callback. Existing counters are NOT zeroed - call
`emu_cdl_reset` if you need a clean baseline. Returns
`{"started": true, "mode": "always"}`.

### `emu_cdl_stop`

Stop CDL recording. Data is preserved until explicitly reset or
exported. Returns `{"stopped": true, "mode": "off"}`.

### `emu_cdl_reset`

Zero all CDL counters. Recording mode is unchanged - if it was active,
it continues from zero. Returns `{"reset": true}`.

### `emu_cdl_export`

Export CDL data to a set of files. The `path` argument points to the
target meta JSON file (e.g. `/tmp/cdl-export/snap1.json`). The
exporter creates the parent directory if missing and writes:

- `snap1.json` - meta with region descriptors
- `snap1_bus.cdl` - binary counter array for the CPU bus map
- `snap1_ram.cdl` - for main RAM
- `snap1_rom_*.cdl` - for ROM regions
- `snap1_vram*.cdl` - for VRAM (MZ-800 has multiple per-mode regions)
- `snap1_iorq_*.cdl` - for I/O ports

Cell layout = 16 B (`r`, `w`, `x`, `s` as uint32 LE counters).

Returns `{"path": "...", "region_count": <int>}`. `region_count` is
the number of region files emitted (platform-dependent).

## Trace tools

Runtime lifecycle control of the 4 binary trace-suite channels
(`cputrack`, `iorqlog`, `intlog`, `hwlog`). The interface mirrors the
CDL tools - it lets an AI client bracket the recording precisely around
the section of interest instead of saving only at emulator exit. For
the output format details see [Trace Suite](../debugger/Trace_Suite.md).

All 4 tools have a mandatory argument:

- `channel` (string, required) - one of `cputrack` / `iorqlog` /
  `intlog` / `hwlog`.

### `emu_trace_start`

Starts recording the channel (= ALWAYS mode). Returns
`{"started": true}`.

### `emu_trace_stop`

Stops recording the channel; the segment is closed (flush + close).
Returns `{"stopped": true}`.

### `emu_trace_reset`

Clears the current channel segment (closes + reopens the segment; for
cputrack also resets the HALT/self-loop collapse). Returns
`{"reset": true}`.

### `emu_trace_save`

Saves / redirects the channel segment. Argument:

- `path` (string, optional) - target path of the FOLLOWING segment.
  Without `path` the current segment is just closed and reopened on the
  existing dir/name (= "save now").

Returns `{"saved": true, "path": <str|null>}`.

## Profiler tools

The CPU profiler aggregates per-function statistics (calls, exclusive
and inclusive Z80 cycles, min/max/avg). It is built on top of the
Callstack listener API - every CALL / RST / IRQ / NMI entry registers
a sample, RET / RETI / RETN terminates the measurement and adds the
inclusive and exclusive cycle counts.

**Hot-path overhead:** an active profiler adds measurable overhead to
the CPU loop. For production benchmarks it is recommended to start
the profiler just before the measurement and stop it right after.
The client should not leave the profiler permanently on.

Profiler state ownership:

- `emu_profiler_start` also enables the Callstack subsystem if it
  was off.
- `emu_profiler_stop` restores the prior Callstack state (= only
  if the profiler activated it).
- `emu_profiler_reset` does not change the recording status -
  only clears the aggregator.

### `emu_profiler_start`

Activate the profiler. No arguments. Returns `{"active": true}`.

### `emu_profiler_stop`

Deactivate the profiler. Aggregator data is preserved - the client
may still call `emu_profiler_get` or `emu_profiler_export` before
resetting. Returns `{"active": false}`.

### `emu_profiler_reset`

Clear the entries hash map, global counters, and the baseline
cycle counter. Recording status does not change. Returns
`{"reset": true}`.

### `emu_profiler_export`

Export the aggregator to a file. Arguments:

- `path` (string, required) - target filesystem path.
- `format` (string, optional, default "csv") - "csv" or "json".

Format details:

- CSV: UTF-8, LF line endings, locale-safe. Header
  `addr,kind,calls,excl_cycles,incl_cycles,min_cycles,max_cycles,avg_cycles`.
- JSON: `{"stats": {...}, "entries": [...]}` - global stats plus
  an array of per-function records.

`min_cycles=0` indicates an entry without a matched exit (= UI
semantics).

Returns `{"path": "...", "format": "csv"|"json", "entry_count": N}`.

### `emu_profiler_get`

Returns the current aggregator as inline JSON (no file I/O).
Argument:

- `limit` (int, optional, 1..1000, default 50) - maximum number
  of entries in the response. `entry_count` in the response is
  always the total count, independent of the limit.

Response:

```json
{
  "active": bool, "entry_count": int, "limit": int,
  "total_cycles_64": int, "total_calls": int, "irq_entries": int,
  "unmatched_returns": int, "max_depth_reached": int,
  "overflow_count": int,
  "entries": [
    {"addr": int, "kind": "call"|"rst"|"irq_im0"|"irq_im1"|
                          "irq_im2"|"nmi"|"synthetic",
     "calls": int, "excl_cycles": int, "incl_cycles": int,
     "min_cycles": int, "max_cycles": int, "avg_cycles": int},
    ...
  ]
}
```

The `entries` array is in hash-map iteration order (= not sorted).
The client is expected to sort it according to its own metric
(typically `excl_cycles` descending).

## Media tools

Unified access to media operations across 11 supported slots:

| Slot | Meaning |
|------|---------|
| `cmt` | CMT cassette (.mzf, .mzt) |
| `fdc0_fd0` .. `fdc0_fd3` | WD279x FDC0 (standard, ports 0xD8-0xDF) drives 0..3 (.dsk) |
| `fdc1_fd0` .. `fdc1_fd3` | WD279x FDC1 (secondary, ports 0x58-0x5F) drives 0..3 (.dsk) |
| `qd` | Quick Disk (.qd) |
| `ide8` | IDE8 master HDD (.img) |

A slot is unavailable if the arch build does not include the
corresponding HW module. In that case `emu_media_state` still
includes the slot in the response with `inserted=false`, `path=""`.

**Inline base64:** `emu_media_load_mzf` and `emu_media_insert` accept
either `path` OR `bytes_b64`. For `bytes_b64` the server decodes the
content to a temporary file and passes its path to the media
subsystem. The temp file is best-effort unlinked after the call
(on Windows where the drive holds an open handle it persists until
unmount).

### `emu_media_load_mzf`

CMT-hack instant load. Bypasses the cassette emulation and performs
the full load (header + body) via a ROM monitor patch (= the program is
in memory immediately, no audio simulation). The body is placed at the
LOAD_ADDR from the MZF header. Usage:

```
emu_media_load_mzf(path="/programs/gardener.mzf")
```

Or inline:

```
emu_media_load_mzf(bytes_b64="UVWdC...")
```

Returns `{"ok": true, "load_addr": int, "exec_addr": int, "size": int,
"result_code": 0}` (`load_addr` = MZF fstrt, `exec_addr` = MZF fexec,
`size` = body length). On error (file cannot be opened / invalid header
/ body load failed) returns `{"error": ...}` - no false `ok`.

**Side effect:** for a program with LOAD_ADDR < 0x1000 the lower ROM
(0x0000-0x0FFF) is temporarily unmapped while the body is written so the
program lands in RAM (otherwise it would be written under ROM and lost).
The original memory map is restored after the load, so there is no
lasting banking side effect. It is a clean load-only primitive: it sets
neither PC nor SP and restores the HL/BC/AF scratch registers it uses.

After `media_load_mzf` the CPU stays where it was (= typically the
ROM Monitor scan loop) - data is in RAM but no jump is performed.
To also start executing the program use `emu_media_run_mzf`
(= composite below).

### `emu_media_run_mzf` (sensitive)

Composite tool: loads MZF + disconnects lower/upper ROM (= ports
0xE0/0xE1) + jumps to the MZF EXEC address (= STRT field at offset
0x16 of the header). Authentic Sharp ROM Monitor LOAD handover
without waiting for a tape signal or ROM Monitor command prompt.

```
emu_media_run_mzf(path="/programs/mzdos.mzf")
```

Returns:

```json
{
  "loaded": true,
  "header": {
    "file_type": 1, "filename": "mzdos",
    "file_size": 4096, "load_addr": 256, "exec_addr": 256
  },
  "rom_disconnected": true,
  "pc_set_to": 256
}
```

Implementation = Python composite (= `emu_media_load_mzf` +
`emu_io_write` x2 + `emu_set_register` PC). The MZF header is
parsed client-side (= fast, no round-trip for the header).

**Destructive:** The CPU starts executing code from RAM with the
ROM disconnected immediately. If you do NOT want this autentic
flow (= you want the ROM Monitor LOAD process simulated via
keyboard), use `emu_media_insert(slot='cmt', path=...)` +
`emu_input_send_keys` with "LOAD\r" (= slower, but exercises the
full ROM Monitor state machine).

### `emu_media_load_binary` (sensitive)

Raw bytes from file to Z80 memory at the given address. Writes go
through the banking-aware path with no region checks - may overwrite
ROM-shadow RAM, video memory etc. in the current banking.

```
emu_media_load_binary(path="/build/sprite.bin", addr=0xC000)
```

Returns `{"ok": true, "addr": 0xC000, "size": 256, "result_code": 0}`.

### `emu_media_insert` (sensitive)

Insert image into slot. If the slot is already occupied, a silent
auto-eject happens (= equivalent of eject + insert in one step).

```
emu_media_insert(slot="fdc0_fd0", path="/disks/cpm.dsk")
emu_media_insert(slot="fdc1_fd0", path="/disks/data.dsk")
emu_media_insert(slot="cmt", bytes_b64="...", ro=True)
```

Returns `{"ok": true, "slot": "fdc0_fd0", "result_code": 0}`.

### `emu_media_eject`

Eject image from slot. No-op when the slot is empty.

```
emu_media_eject(slot="fdc0_fd0")
```

Returns `{"ok": true, "slot": "fdc0_fd0"}`.

### `emu_media_state`

Snapshot of all 11 slots' state.

```
emu_media_state()
```

Returns:

```json
{
  "count": 11,
  "slots": [
    {"slot": "cmt",      "inserted": true,  "path": "/cassettes/sapo-p.mzf", "ro": false},
    {"slot": "fdc0_fd0", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd1", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd2", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd3", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd0", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd1", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd2", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd3", "inserted": false, "path": "", "ro": false},
    {"slot": "qd",       "inserted": false, "path": "", "ro": false},
    {"slot": "ide8",     "inserted": true,  "path": "hdd0-mz800.img", "ro": false}
  ]
}
```

## Platform + Config tools

Key design decision: **runtime platform switch DOES NOT exist** -
mz700/mz800/mz1500 are separate binaries selected at compile time.
`emu_platform_set` therefore returns an error stating which executable
the user must launch instead of the current one.

### `emu_settings_set` (sensitive)

Writes a live-settable INI key. The whitelist contains keys with
immediate application (audio volume, video tweaks, QDISK path).
Boot-time keys (emulator paths, snapshot defaults) are rejected.

```
emu_settings_set(key="AUDIO/volume_8253", value="60")
emu_settings_set(key="DISPLAY/locked_window_aspect_ratio", value="true")
```

Returns `{"key": ..., "previous_value": "50", "new_value": "60",
"type": "unsigned"}`. The `previous_value` can be used for rollback.

Whitelist:

- `AUDIO/volume_8253` (unsigned)
- `AUDIO/volume_psg0` .. `AUDIO/volume_psg3` (unsigned)
- `AUDIO/volume_psg1_0` .. `AUDIO/volume_psg1_3` (MZ-1500, unsigned)
- `DISPLAY/forced_full_screen_redrawing` (bool)
- `DISPLAY/locked_window_aspect_ratio` (bool)
- `DISPLAY/custom_fps` (unsigned)
- `QDISK/filename` (text)
- `QDISK/write_protected` (bool)
- `TRACE_CPUTRACK/pc_range_lo` .. `TRACE_CPUTRACK/pc_range_hi` (unsigned, 0..65535; runtime PC trace filter)

### `emu_settings_get`

Reads the current value of any configuration key (= read whitelist is
open). Returns the string representation plus the type code.

```
emu_settings_get(key="AUDIO/volume_8253")
-> {"key": "AUDIO/volume_8253", "value": "60", "type": "unsigned"}
```

For INI keys of type `keyword` the textual keyword variant is
returned.

### `emu_platform_set` (sensitive)

**Runtime platform switch is not supported.** A call with
`kind != active_kind` returns `ok=false` with `active_kind` (= the
current build) and an explanatory error message. The user must launch
a different binary (e.g. `mz1500emu.exe` instead of `mz800emu.exe`).

A call with `kind == active_kind` is a no-op (= `ok=true,
no_op=true`) without side effects.

```
emu_platform_set(kind="mz1500")
-> {"ok": false, "active_kind": "mz800", "target_kind": "mz1500",
    "error": "Runtime platform switch not supported - mz700/mz800/
    mz1500 are separate binaries (compile-time MZARCH). To use a
    different platform, restart with the corresponding executable
    (e.g. mz1500emu.exe)."}
```

The parameters `mode` (`native` / `compat`) and `save_snapshot`
(path to .mzs) are accepted for forward compatibility but currently
ignored.

### `emu_periph_attach` (sensitive)

Writes `active=true` in the configuration for the given peripheral
module (= `MEMEXT`, `FDC`, `QDISK`, `IDE8`, `GAL5`). For full
application a restart of the emulator is required (= response carries
`requires_restart=true`).

```
emu_periph_attach(kind="memext", options={"type": "luftner4k"})
emu_periph_attach(kind="fdc")
```

For `memext` the `options.type` can carry a variant (string).

Returns `{"ok": true, "kind": "memext", "requires_restart": true,
"result_code": 0}`. If the module does not exist (= the peripheral is
not in this architecture build), an error with detail is returned.

### `emu_periph_detach` (sensitive)

Analogously `active=false`. Also requires a restart.

```
emu_periph_detach(kind="fdc")
```

### Example workflow - mounting a Quick Disk image

```
# 1. Set the image path
emu_settings_set(key="QDISK/filename", value="/disks/cpm.qd")

# 2. Set R/W mode
emu_settings_set(key="QDISK/write_protected", value="false")

# 3. Perform insert
emu_media_insert(slot="qd", path="/disks/cpm.qd")
```

## Hot-swap tools

A pair of tools `emu_stop` + `emu_start` for the AI dev workflow:
cycle stop -> rebuild mz800emu.exe -> start, without restarting the
Claude Code session (= no loss of AI context). Both tools are
**pipe-only** - in TCP mode they return an error because a TCP attach
goes against the user's live GUI session.

State preservation is NOT automatic. The AI client must explicitly
save state before stop and restore it after start:

```
emu_snapshot_save_buffer  ->  emu_stop  ->  (rebuild binary)
->  emu_start  ->  emu_snapshot_load_buffer
```

### `emu_stop` (sensitive, pipe-only)

Graceful shutdown of the emu child process. The `mcp_server.py`
wrapper stays alive and keeps the MCP session with the Claude client.

Success response:
```json
{"stopped": true, "transport": "pipe"}
```

Error response in TCP mode:
```json
{"error": "hot-swap requires pipe transport (current: tcp)"}
```

Arguments: none.

### `emu_start` (sensitive, pipe-only)

Spawns a fresh emu child process. Counterpart to `emu_stop` for the
hot-swap workflow.

Arguments:
- `binary_path` (string, default ""): path to mz800emu.exe. If empty,
  uses `MZ800EMU_EXE` env or the default location next to
  `mcp_server.py`.

Success response:
```json
{"started": true, "exe": "C:/.../mz800emu.exe", "commands": 65}
```

The `commands` field reports the number of tools in the hello payload
of the new child process (= AI-client-side confirmation that the new
binary is alive).

Error responses:
- `"hot-swap requires pipe transport"`
- `"emulator already running (call emu_stop first)"`
- `"mz800emu binary not found at ..."`

`emu_start` is a **Python-only tool** - the emu binary cannot spawn
itself; only the wrapper has control over subprocess lifecycle.

## HID input tools

Six tools for simulating user input - keyboard + joystick. Key for AI
driven demos, automated testing, and replay scenarios.

Press / release injection goes through a **virtual keyboard matrix**
parallel to the physical scan matrix. The Z80 emulation ANDs the two
matrices when reading PORT B, so a virtual press appears to the Z80
as a real key held by the user.

Key name vocabulary: RETURN, BREAK, SHIFT, CONTROL, GRAPH, ALPHA,
ARROW_*, F1..F9, plus ASCII fallback (`"A"`, `"ASCII:A"`, ...).

The joystick state byte uses the Sharp MZ standard:

- bit 0 = UP, 1 = DOWN, 2 = LEFT, 3 = RIGHT
- bit 4 = FIRE1, 5 = FIRE2
- bits 6-7 unused

The MCP interface uses an **active-HIGH** mask (0x01 = UP pressed).
The native HW state byte is active-LOW (0xFE = UP pressed). Bridge
conversion happens automatically.

Frame timing default = 3 frames (~60 ms at 50 fps PAL). Maximum
600 frames (~12 s) as a safety against AI-induced freezes.

**Deterministic frames semantics:** Between press and release the
backend waits for **N real video frames** (= watches the
`fbsnapshot_screen_id` counter incremented by the emu thread), not
a wallclock sleep. If emulation was paused, the helper briefly
unpauses for the wait and restores. This guarantees that the ISR
scan (= keyboard matrix read in PIO INT) captures the virtual press
bit exactly N times - no race condition between async unpause and
press timing.

All HID tools carry a **WARNING** token in their description - user
simulation can trigger unintended behavior (= RUN+RETURN, BASIC
program overwrite, AI-controlled game in unexpected contexts).

**Key landing readback (`landing_verified`):** `emu_input_send_keys`
returns `{"keys_sent": N, "keys_landed": N, "total_frames": N, "encoding": str,
"landing_verified": <bool>}`. `keys_sent` counts host-side injections
into the virtual matrix; `keys_landed` counts how many of them the guest
actually read. Previously the tool reported success even when the guest
swallowed the keys (a running program with the ROM disconnected scans only
a narrow subset of the matrix) - that was a silent failure. `landing_verified`
is now a real readback: while the key is held the emulator watches the PPI
Port B reads and the flag is `true` only if the guest scanned the key's column
during the hold (i.e. it actually read the key), `false` otherwise. If a key
does not land, hold it longer (`frame_per_key`), make sure the guest is at a
prompt that scans the keyboard, or reset the emulator.

### Practical examples

**ASCII encoding** (= default, simple text + `\r` for RETURN):

```python
# Type "LOAD" + Enter (= Sharp ROM Monitor LOAD command)
emu.call('input_send_keys', text="LOAD\r", encoding='ascii',
         frame_per_key=3)

# BASIC program type-in (= multi-line)
emu.call('input_send_keys',
         text="10 PRINT \"HELLO\"\r20 GOTO 10\rRUN\r",
         encoding='ascii', frame_per_key=3)
```

**Key names encoding** (= JSON array string, explicit control):

```python
import json
keys = ["G", "R", "A", "P", "H", "SPACE", "1", "RETURN"]
emu.call('input_send_keys', text=json.dumps(keys),
         encoding='key_names', frame_per_key=3)
```

**Modifier hold pattern** (= SHIFT + key):

```python
emu.call('input_press_key', key='SHIFT')   # hold SHIFT
emu.call('input_send_key', key='A', frames=3)  # press A while SHIFT held
emu.call('input_release_key', key='SHIFT')  # release SHIFT
```

**Joystick** (= bitmask game input):

```python
# Joystick port 0, UP + FIRE1 for 5 frames
emu.call('input_send_joystick', port=0, state=0x11, frames=5)
```

**Timing-controlled sequence** (= speedrun-style):

```python
events = [
    {"type": "key_press", "key": "RIGHT"},
    {"type": "wait_frames", "frames": 30},
    {"type": "key_release", "key": "RIGHT"},
    {"type": "key_press", "key": "SPACE"},
    {"type": "wait_frames", "frames": 2},
    {"type": "key_release", "key": "SPACE"},
]
emu.call('input_send_keys_with_delays', events=events)
```

### Reverse lookup of keys

To enumerate valid `key_names` for a specific platform read:

```python
info = emu.call_resource('emulator://input/keyboard/matrix_info')
# returns { platform, keys: [{col, bit, name, ascii_char}], key_count }
```

The table contains aliases (= `RETURN`/`ENTER`/`CR` point to the same
position). The same `(col, bit)` appears under multiple names.

## Example usage from Claude Code

**Prompt:**

> Launch the emu, pause it, look at the registers, and return them.

**What Claude does:**

1. `emu_status` -> verify connected
2. `emu_pause` -> pause
3. `emu_get_registers` -> read registers
4. Return a formatted register dump to the user.

**Prompt:**

> Set a breakpoint at 0x0000 and reset the emu.

**What Claude does:**

1. `emu_bp_add(address=0x0000)` -> add BP
2. `emu_reset` -> reset
3. `emu_run(frames=10)` -> emu runs until BP hit or 10 frames done
4. `emu_status` -> report whether we hit the BP or completed the
   frames

## BP advanced tools

Layer on top of the existing `emu_bp_add` / `emu_bp_list` /
`emu_bp_remove` adding selective field writes and breakpoint group
management (= hierarchy for grouping breakpoints in the UI).

### `emu_bp_create_with_init`

Atomic `breakpoints_add_auto(addr, name, parent)` + selective
`BP_UPDATE` pass inside a single DBGAPI safepoint. The client never
observes the partially constructed breakpoint.

Args:

- `addr` (int) - initial Z80 address. The UM_ADDR bit is implicitly
  added to `fields` if omitted.
- `fields` (list[str]) - field names to apply (subset of
  `DBGAPI_BP_UM_*`). Valid names match the mask bits:
  `"enabled"`, `"auto_name"`, `"name"`, `"colors"`, `"parent"`,
  `"type"`, `"addr"`, `"addr_end"`, `"zone"`, `"bank_id"`,
  `"port"`, `"event_name"`, `"event_trigger"`, `"sp_threshold"`,
  `"expr"`, `"action"`, `"hit_count"`, `"skip_count"`,
  `"edge_triggered"`, `"addr_match_mode"`, `"addr_mask"`,
  `"port_match_mode"`, `"port_end"`, `"port_mask"`, `"port_mode"`,
  `"bank_match_mode"`, `"bank_id_end"`, `"bank_id_mask"`,
  `"bp_addr_space"`,
  `"sp_mode"`, `"sp_upper"`, `"im2_vector_filter"`,
  `"im2_vector_match_mode"`, `"im2_vector_addr_end"`,
  `"im2_vector_mask"`, `"im2_isr_filter"`,
  `"im2_isr_match_mode"`, `"im2_isr_addr_end"`, `"im2_isr_mask"`,
  `"im0_enabled"`, `"im1_enabled"`, `"im2_enabled"`,
  `"im0_rst_mask"`, `"irq_sig_source_mask"`,
  `"fwd_min_interval_ms"`, `"fwd_max_fires"`.
- `values` (dict) - map of field name to value. Only fields listed
  in `fields` are read. String fields (`name`, `expr`, `action`,
  `event_name`) accept `None` to clear.

Returns: JSON `{"id": int, "created": bool}`. `id` is `-1` on
failure.

The `fwd_min_interval_ms` and `fwd_max_fires` fields are a per-BP
override of the forward-action (snapshot / trace_save) rate limit -
see the "Forward action protection (rate limit + saturation)" section
below.

### `emu_bp_set_parent`

Quick reparent a breakpoint into a group. Args: `id`, `parent_id`
(`-1` = root). Returns: `{"updated": bool}`.

### `emu_bp_update`

Selectively update fields of an existing breakpoint. Args: `id`,
`fields` (field names), `values` (value map). An empty `fields`
list is a successful no-op. Returns: `{"updated": bool}`.

Like `emu_bp_create_with_init`, it also accepts the
`fwd_min_interval_ms` and `fwd_max_fires` fields for a per-BP
override of the forward-action rate limit - see the "Forward action
protection (rate limit + saturation)" section below.

### Forward action protection (rate limit + saturation)

Forward actions in a BP script that write to disk (snapshot,
trace_save) have built-in protection against flooding the emulator
and the disk when a BP fires very frequently:

- **Per-BP rate limit.** `fwd_min_interval_ms` is the minimum delay
  in ms between two forward actions of the same BP; firing faster is
  silently skipped. `fwd_max_fires` is a hard cap on successful fires
  per session - once reached, the BP disables itself. Both fields are
  settable via `emu_bp_create_with_init` / `emu_bp_update`
  (`fields: ["fwd_min_interval_ms", "fwd_max_fires"]`). Without an
  explicit override the global default from configuration applies.
- **Byte saturation.** A cumulative byte limit on the volume written
  by forward actions (snapshot + trace, including flush-side
  accounting) - once the threshold is exceeded the emulator
  auto-pauses and emits a saturation event / warning with a reason.
  The threshold is configurable.
- **Control-plane robustness.** Even when a continuing BP triggers a
  heavy forward action, pause / stop / clear-breakpoint commands
  always take effect - the emulator does not get stuck.

### `emu_bpgrp_add`

Add a new breakpoint group. Args: `name`, `parent` (default
`-1`). Returns: `{"id": int}` (= `-1` on cycle / missing parent).

### `emu_bpgrp_remove`

Remove a group. Cascading delete / reparent of children
(breakpoints + sub-groups) is handled by the backend
(`breakpoints.c`). Args: `id`. Returns: `{"removed": bool}`.

### `emu_bpgrp_update`

Selectively update group fields. Args: `id`, `fields` (subset of
`"enabled"` / `"name"` / `"colors"` / `"parent"`), `enabled`,
`name`, `bg_rgb`, `fg_rgb`, `parent`. Returns: `{"updated": bool}`.

## Stack analytics tools

Wrappers around the SP history ring buffer (= sequence of SP values
over time) and named stack regions (= labeled address ranges with
push/pop counters + watermark).

### `emu_stack_history_enable`

Enable/disable SP history recording. Disabling additionally flushes
the ring buffer (= the next enable starts with a clean state). The
active flag wires into a hot-path call site in `mzarch.c` (= zero
overhead while OFF, the default).

Args: `enabled` (bool). Returns: `{"enabled": bool}`.

### `emu_stack_history_reset`

Flush the ring buffer without disabling the recording flag (= UI
"Reset history" button before observing a specific code path).
Returns: `{"reset": true}`.

### `emu_stack_regions_add`

Add a monitored region. Args: `name` (max 31 chars), `base` (top =
highest addr), `limit` (bottom; `base > limit`). Validation in
`stack_regions_add` (= overlap, duplicate). Returns: `{"index":
int, "added": bool}` (= `-1` on invalid input).

### `emu_stack_regions_edit`

Edit the region at a given index. Args: `index`, `name`, `base`,
`limit`. On success the backend also resets the watermark + push/
pop counters (= old stats do not match the new range). Returns:
`{"updated": bool}`.

### `emu_stack_regions_remove`

Remove a region. Args: `index`. Returns: `{"removed": bool}`.

### `emu_stack_regions_reset_watermark`

Reset watermark + push/pop counters of a single region. The region
configuration (name/base/limit) is preserved. Args: `index`.
Returns: `{"reset": bool}`.

## Eventlog tools

Eventlog (= Event Viewer / TLOG) is a ring buffer for capturing the
chronological stream of events (raster, IRQ ACK, IORQ IN/OUT, MMIO
R/W, GDG mode/banking/scroll/colors/video, PIO 8255 / CTC 8253 / PIO
Z80 events, PSG, FDC, MemExt, BP fire, user marks, etc.). Key for AI
client analysis of emu behavior without the GUI Event Viewer window.

Filter: 64-bit bitmask per category (= `en_EVENTLOG_CATEGORY` in
`eventlog.h`). The client sets the mask before start, then emits
respect the filter.

### `emu_eventlog_start`

No params. Starts recording (= subsequent emits on the emu thread are
written into the ring). Returns: `{"started": bool}`.

### `emu_eventlog_stop`

No params. Stops recording, ring content preserved. Returns:
`{"stopped": bool}`.

### `emu_eventlog_clear` (sensitive)

No params. Clears the ring buffer (= loses capture history). Returns:
`{"cleared": bool}`.

### `emu_eventlog_set_capacity` (sensitive)

Args: `capacity` (= requested ring size in events). Backend clamps to
`[EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY]` (= 10000..200000).
Resize re-allocates and discards current content.

Returns: `{"capacity_after": int}` (= actual size after clamping).

### `emu_eventlog_set_mask` (sensitive)

Args: `mask` (= 64-bit bitmask, bit i enables category i). Accepts
**int** (= max 63 bits due to signed gint64) or **hex string** (=
full 64-bit range including bit 63, format `"0xN..."` or `"N..."`,
supports `_` as visual separator).

Returns: `{"mask_hex": "0xN..."}` (= 16-digit zero-padded hex string
for unambiguous round-trip).

### `emu_eventlog_get_event`

Args: `idx` (= 0 = oldest, capacity-1 = newest). If `idx >= count`:
`{"available": false, "idx": int}`. Otherwise returns full 24 B
record:

```json
{
  "available": true, "idx": int,
  "pxclk_total": int,        // 64-bit pixel clock total since power-on
  "screens_total": int,      // 32-bit frame counter
  "pxclk_in_screen": int,    // 32-bit pixel clock pos in current frame
  "category": int,           // 8-bit en_EVENTLOG_CATEGORY
  "subtype": int,            // 8-bit category-specific subtype
  "pc": int,                 // 16-bit Z80 PC at emit
  "payload": int             // 32-bit category/subtype-specific data
}
```

Decoding of `payload` per category is in `eventlog.h` (= client
replicates the mapping or fetches it from the knowledge base).

## Direct memory region tools

### `emu_regions_list`

No params. Enumerate all physical memory regions of the current
architecture (= equivalent of the GUI Memory browser for headless
debug).

Returns:

```json
{
  "regions": [
    {"id": 0, "kind": "logical", "name": "Z80 view",
     "logical_base": 0, "size": 65536, "writable": true,
     "connected": true, "mapped_now": true},
    {"id": 1, "kind": "ram", "name": "RAM (raw 64K)",
     "logical_base": null, "size": 65536, ...},
    {"id": 2, "kind": "rom_lower", "name": "Monitor ROM (lower)",
     "logical_base": 0, "size": 4096, "writable": false, ...},
    {"id": 8, "kind": "memext_ram", "sub_id": 12,
     "name": "Memext RAM bank 12", "size": 4096, ...},
    ...
  ],
  "count": int
}
```

`kind` enum: `logical, ram, rom_lower, rom_upper, cgrom, cgram_700,
vram_700_char, vram_700_attr, vram_phys_plane (sub_id 0..3),
pcg_1500 (sub_id 0..2), memext_ram, memext_flash, ramdisk_std,
ramdisk_pezik, prohibited_shadow`.

`sub_id` disambiguates regions of the same kind (= plane index, bank
index, PEZIK instance).

**ID stability**: per session, NOT per HW reconfigure. After
`periph_attach/detach` or `media_insert/eject` the client must call
`regions_list` again for new IDs.

### `emu_region_read`

Raw no-side-effect read from a specific region (= bypass Z80
banking). No auto-inc latch, no GDG RF dispatch, no IRQ trigger.

Args:
- `region_id` (= from the last `regions_list` call)
- `offset` (= byte offset within the region, 0..size-1)
- `length` (= 1..65536, clamped to region size if
  `offset+length > size`)

Returns:
```json
{
  "region_id": int,
  "offset": int,
  "length": int,        // actually-read length after clamping
  "data_b64": "..."     // base64-encoded raw bytes
}
```

Main use case: AI client in headless mode (= no GUI access) inspects
emu physical memory - ROM monitor content, VRAM bitmap planes,
MemExt banks.

## Speed control tools

Control of the emulation tempo (= equivalent of the GUI Speed menu).
Main use case: let a long operation (boot ROM, cassette/disk load, long
computation) finish quickly via max speed (warp), then back to 100%.
A tempo change is a visible action (= appears in the Activity log as an
MCP action).

The current speed state is read via the `emulator://speed` resource.

### `emu_set_speed`

Set the emulation speed according to `mode`.

Args:
- `mode` (required): one of
  - `normal` - run at 100% (real-time tempo, turns warp off)
  - `custom` - run at an exact `percent` % (turns warp off)
  - `max` - warp / unthrottled, as fast as the host allows (turns warp on)
  - `step` - relative adjust of the custom % by `step` (warp unchanged)
- `percent` (default 100): target % for `mode=custom` (1..4000, clamped
  by the core)
- `step` (default 0): relative delta for `mode=step` (positive = faster,
  negative = slower, 0 = no-op)

Returns:
```json
{
  "ok": true,
  "mode": "max",           // resulting mode (max/custom/normal)
  "current_percent": 100,  // current % after the operation
  "max_speed": true        // warp flag after the operation
}
```

Note: `emu_run(frames=N)` waits for N frames; with max speed the frame
counter advances faster, so a blocking `emu_run` finishes in less
wall-clock time (behaviour stays correct, just faster).

### `emu_speed_step`

Convenience wrapper around `emu_set_speed(mode="step", step=delta)`.

Args:
- `delta`: relative delta in % (positive = faster, negative = slower)

Returns: the same echo object as `emu_set_speed`.

### Example workflow - warp through boot

```python
emu_set_speed(mode="max")        # turn warp on
emu_run(frames=600)              # let boot/load finish quickly
emu_set_speed(mode="normal")     # back to 100%
```

## CMT cassette tape tools

Control of the Sharp MZ cassette tape (CMT). There are two distinct
concepts, kept separate on purpose:

- **Real tape** (`emu_cmt_play`, `emu_cmt_play_paused`, `emu_cmt_stop`,
  `emu_cmt_pause`, `emu_cmt_eject`, `emu_cmt_record`) drives the
  cycle-accurate emulated cassette. The program reads/writes the tape
  signal through the normal Sharp ROM routine, exactly like physical
  hardware. This is the accurate path and works with any program.
- **cmthack** (`emu_cmt_hack_set`) is a ROM-patch shortcut (instant
  load): the patched ROM load routine copies a tape file straight into
  RAM, skipping the tape signal. It is faster but only works for
  programs that load through the patched ROM entry points.

All of these change emulator state (sensitive) and appear in the
Activity log as MCP actions. The current tape state, including
`cmthack_enabled`, is read via the `emulator://periph/cmt` resource.

A tape image must be inserted first via `emu_media_insert(slot="cmt",
path=...)` before playback.

### `emu_cmt_play` (sensitive)

Start real cassette playback (transport PLAY). No-op if no tape is
loaded or playback cannot start in the current state.

Returns: `{"ok": true, "action": "play"}`.

### `emu_cmt_play_paused` (sensitive)

Same as `emu_cmt_play` but the tape begins paused; call
`emu_cmt_pause(paused=False)` to advance it.

Returns: `{"ok": true, "action": "play_paused"}`.

### `emu_cmt_stop` (sensitive)

Stop the transport (PLAY or RECORD -> STOP). No-op if already stopped.

Returns: `{"ok": true, "action": "stop"}`.

### `emu_cmt_pause` (sensitive)

Pause (`paused=True`, default) or resume (`paused=False`) the running
transport without resetting its position.

Args:
- `paused` (default true): true = pause, false = resume.

Returns: `{"ok": true, "action": "pause"}`.

### `emu_cmt_eject` (sensitive)

Stop the transport (if running) and remove the loaded tape image.
Scoped equivalent of `emu_media_eject(slot="cmt")`.

Returns: `{"ok": true, "action": "eject"}`.

### `emu_cmt_record` (sensitive)

Start recording the real cassette output to WAV, LEP, or L16. Recording
starts paused; call `emu_cmt_pause(paused=False)` to begin capturing.
The filename extension selects the format (`.lep` uses 50 us units,
`.l16` uses 16 us units); no extension defaults to WAV. Requires the
tape to be in the STOP state and the path to be writable.

Args:
- `path` (required): target `.wav`, `.lep`, or `.l16` path (must be writable).

Returns: `{"ok": true, "path": "<path>"}`. Fails (`error`) on bad state
or a path that is not writable.

### `emu_cmt_hack_set` (sensitive)

Enable or disable the cmthack ROM patch (instant tape load). NOT the
real tape - see the note above.

Args:
- `enabled` (required): true = install the ROM patch, false = remove it.

Returns: `{"ok": true, "installed": <bool>}` echoing the patch state
after the operation.

### Example workflow - record real tape output

```python
emu_media_insert(slot="cmt", path="game.mzf")  # source tape
emu_cmt_record(path="out.wav")     # arm recording (starts paused)
emu_cmt_pause(paused=False)        # begin capturing
# ... run the program ...
emu_cmt_stop()                     # finish and flush the selected format
```

### `emu_cmt_set_speed` (sensitive)

Sets the real tape speed ratio relative to 1200 Bd. Accepts a ratio
string key or the en_CMTSPEED int: `1:1` (1), `2:1` (2), `2:1_cpm` (3),
`3:1` (4), `3:2` (5), `7:3` (6), `8:3` (7), `9:7` (8), `25:14` (9).
Changes the default speed; per-block override via
`emu_cmt_tape_set_block_speed`. Reflected as `cmtspeed` in
`emulator://periph/cmt`.

Args:
- `speed` (required): ratio string ("2:1") or int (2).

Returns: `{"ok": true, "property": "speed", "value": <int>}`.

### `emu_cmt_set_polarity` (sensitive)

Sets the tape signal polarity (rear DIP switch). Reflected as
`polarity_inverted`.

Args:
- `inverted` (required): true = inverted, false = normal.

### `emu_cmt_set_cpu_boost` (sensitive)

Enables/disables CPU boost during tape transport (run at max speed for
fast long loads). Reflected as `cpu_boost`.

Args:
- `enabled` (required): true = boost, false = real time.

### `emu_cmt_set_mzfsize_check` (sensitive)

Enables/disables the MZF size consistency check on load (body size vs
header file size). Reflected as `mzfsize_check`.

Args:
- `enabled` (required): true = enforce, false = skip.

### `emu_cmt_open` (sensitive)

CMT-specific open by file extension (.mzf/.mzt/.wav/...). Unlike
`emu_media_insert(slot="cmt")` it can start playback directly via
`play_immediately` (= one round trip instead of open + play). Real tape,
not cmthack.

Args:
- `path` (required): tape file path (extension selects the backend).
- `play_immediately` (default false): start playback after opening.

Returns: `{"ok": true, "path": <str>, "playing": <bool>}`.

### `emu_cmt_tape_seek` (sensitive)

Seeks to a tape block (SIMPLE_TAPE multi-block containers). A
single-block container (e.g. a plain .mzf) has only block 0 and may not
support seeking. Requires a loaded tape. Block listing is in
`emulator://periph/cmt/tape`.

Args:
- `block_id` (required): 0-based block index.

Returns: `{"ok": true, "block_id": <int>}`.

### `emu_cmt_tape_set_block_speed` (sensitive)

Per-block speed (cmt speed only, no other parameters; SIMPLE_TAPE).
`speed` accepts the same keys/int as `emu_cmt_set_speed`. Requires a
loaded tape.

Args:
- `block_id` (required): 0-based block index.
- `speed` (required): ratio string ("2:1") or int (2).

Returns: `{"ok": true, "block_id": <int>, "speed": <int>}`.

### Example workflow - multi-block tape

```python
# see emulator://periph/cmt/tape for container_type and block list
emu_cmt_open(path="tape.mzt")
emu_cmt_tape_seek(block_id=2)       # pick the third program
emu_cmt_play()
```

The full workflow (real tape vs cmthack, transport, blocks) is in the
resource `emulator://docs/cmt_workflow`.

## Related

- [Python wrapper](python-wrapper.md) - how to call these tools
  from Claude Code
- [Configuration](configuration.md) - security profile, sensitive
  tool gating
- [Resources overview](resources-overview.md) - read-only URI
  endpoints complementing tools
