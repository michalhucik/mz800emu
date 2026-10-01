# MCP docs - index

Reference documentation for AI clients of the mz800emu MCP server.
Read individual topics on demand via `resources/read`.

## Topics

| URI | What to look up here |
|-----|----------------------|
| `emulator://docs/error_handling` | How to read a failed tool response: which errors mean "not executed, safe to retry" (`Emulator busy: ...`), "partially executed" or a real failure; request timing and the 10 s queue timeout. |
| `emulator://docs/memory_layout` | Z80 64 KB address space layout per platform (MZ-700 / MZ-800 native / MZ-800 700-compat / MZ-1500). Banking ports `0xE0..0xE6`. Key ROM entry points, IRQ vectors, text/color VRAM regions. |
| `emulator://docs/bp_dsl` | Breakpoint condition expression language: registers, memory deref `[addr]`, port read `port[N]`, operators, built-in functions, `$user_vars`, examples. Same language is used by Watch expressions and Action DSL arguments. |
| `emulator://docs/smart_vars` | `$name` user variables: how to create / read / write from BP action DSL, persistence rules (`.bpt` + `.vars`), lifecycle, reading via `emulator://vars`. |
| `emulator://docs/action_dsl` | BP Action DSL grammar - what runs when a breakpoint fires. Commands: `log`, `set <reg>`, `poke`, `mark`, `$var` writes, `if/then/else`, `enable` / `disable` / `disable_self`, `clear_vars`, `continue`, plus the forwarding commands `cdl_start` / `cdl_stop` / `cdl_reset` / `cdl_export`, `trace_start` / `trace_stop` / `trace_save`, `snapshot` (one smart BP instead of repeated MCP calls). `log` format specs, stop vs continue semantics. |
| `emulator://docs/watch_dsl` | Watch expression syntax: three watch modes (`address` / `expr_scalar` / `expr_deref`), supported type tags (`u8`, `i16le`, `ascii`, ...), reading via `emulator://watch`. |
| `emulator://docs/eventlog_mask` | EventLog 64-bit category mask: bit assignments per `en_EVENTLOG_CATEGORY`, common mask recipes ("memory writes only", "IRQ only"), how to pass via `emu_eventlog_set_mask`. |
| `emulator://docs/sharp_display_code` | Sharp MZ ASCII vs display code vs standard ASCII. Mapping tables, inverse video, decoding `emulator://video/text_dump`. |
| `emulator://docs/mz800_keyboard` | Sharp MZ-800 keyboard: physical layout, per-key matrix position + emitted characters, modifiers (SHIFT / CTRL / GRAPH / ALPHA), the four character layers, canonical key names for the `emu_input_send_keys` injection tool, recipes for typing graphics / semigraphics. MZ-700 / MZ-1500 as a diff (no TAB, longer ALPHA). |
| `emulator://docs/cmt_workflow` | CMT cassette workflow: real tape (`emu_cmt_*`) vs cmthack instant load, transport flow, WAV/LEP/L16 recording, speed ratios / polarity / cpu boost / mzfsize check, SIMPLE_TAPE multi-block seek + per-block speed. Cross-refs `emulator://periph/cmt` and `emulator://periph/cmt/tape`. |

## When to read what

- A tool returned an `error` and you are deciding whether to retry ->
  `error_handling`.
- Setting up a breakpoint with a non-trivial condition -> `bp_dsl`.
- Writing the action that runs when a BP fires (log, poke, set,
  conditional) -> `action_dsl`.
- Need a hit counter / state machine across BPs -> `smart_vars`.
- Adding a memory observation in the Watch panel -> `watch_dsl`.
- Tuning EventLog filter to capture only interesting events ->
  `eventlog_mask`.
- Decoding text from `emulator://video/text_dump` or `emu_mem_read` of
  the text VRAM region -> `sharp_display_code`.
- Reading / writing memory and want to know what region is what ->
  `memory_layout`.
- Loading a program from cassette, recording to tape, or working with
  a multi-block tape -> `cmt_workflow`.
- Injecting keystrokes via `emu_input_send_keys` and need the exact key names,
  or want to type Sharp graphics / special characters -> `mz800_keyboard`.

## Live state resources (not docs)

These return current runtime state, not documentation. Read them for
context before / during analysis:

- `emulator://state` - paused / running, frame, transport.
- `emulator://platform/info` - architecture, mode, clocks, scanline geometry.
- `emulator://cpu/registers` - Z80 register snapshot.
- `emulator://memory/map` - current 16 x 4 KB banking snapshot.
- `emulator://memory/{addr_hex}/{length}` - template URI to read a memory range.
- `emulator://breakpoints`, `emulator://watch`, `emulator://vars` - debugger state.
- `emulator://video/text_dump` - 40x25 text VRAM dump (when applicable).

The full live list is in `emulator://config/peripherals` and is also
discoverable via the standard `resources/list` method.

## User knowledge base (`emulator://kb/*`)

If the user configured a knowledge base directory, their own Markdown
notes appear under the separate `emulator://kb/<topic>` namespace
(discoverable via `resources/list`). These are user-authored and not
part of this project reference; read them when present for
machine-specific or task-specific context.
