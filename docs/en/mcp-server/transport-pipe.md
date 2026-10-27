# Pipe transport

The pipe transport is the default way the Python wrapper talks to the
emulator. The AI client (via the Python wrapper) **launches the emulator
as a subprocess** and communicates with it through an anonymous pipe
attached to `stdin`/`stdout`.

## When to use it

- **CI / batch tests** - automated runs without a live GUI session.
- **Claude Desktop** - the client typically spawns its own MCP server,
  it does not need a shared session with the user.
- **Headless workflows** - regression suite, fault injection, AI runs
  through a defined scenario without a human in the loop.
- **Per-session isolation** - each MCP session has its own clean
  instance of the emulator, free of state contamination from previous
  runs.

If you instead want to share a **live GUI session** with the AI (the
user can see and intervene), use the [TCP transport](transport-tcp.md).

## Command

```bash
mz800emu.exe --mcp-pipe --headless --no-first-run-windows
```

| Flag | Meaning |
|------|---------|
| `--mcp-pipe` | enables MCP backend in stdio mode; emu reads JSONL from `stdin`, writes to `stdout` |
| `--headless` | does not render a GUI window (see [Headless mode](headless-mode.md)) |
| `--no-first-run-windows` | suppresses first-run dialogs that would otherwise block headless start |

The Python wrapper runs this command **automatically** as a subprocess
- the user normally does not invoke it directly. Manual usage is useful
for debugging (= verifying the emu responds to a manually crafted
JSONL request).

### Accepted options

Pipe mode accepts only these command line options (a fixed list); any other
option is rejected with `Unknown option: <option>` on stderr and the
emulator does not start:

| Option | Meaning |
|--------|---------|
| `--mcp-pipe`, `--headless` | pipe mode, no window (headless is always on) |
| `--help`, `--version` | print the options or the version and exit |
| `--home-dir`, `--cfg-dir`, `--work-dir` | override the directories (assets, configuration, default dialog path) |
| `--config <file>` | path to the main INI file |
| `--no-save-ini`, `--no-first-run-windows` | do not save the INI at exit, no first-run windows |
| `--cmt <file>`, `--cmt-play`, `--cmt-play-pause`, `--cmt-boot` | tape in the virtual cassette recorder from the start |
| `--fdd <N>=<file>`, `--fdd0` to `--fdd7`, `--fdd-readonly <N>` | DSK images in the floppy drives from the start |

Other `mz800emu.exe` options (e.g. `--run-mzf`, `--speed`, `--mode-switch`,
`--mcp-tcp-port`) are not available in pipe mode. Start a program with the
MCP tool `emu_media_run_mzf` (the same bootstrap as `--run-mzf`) and set the
speed with `emu_set_speed`. If you need `--run-mzf` on the command line, run
the emulator as `mz800emu.exe --headless --mcp-tcp-port=<port> --run-mzf
<file>` and connect through the [TCP transport](transport-tcp.md).

## What happens at startup

1. Emu is configured as headless (no SDL window, no-op audio device).
2. The MCP backend registers a `stdin` reader thread and `stdout`
   writer.
3. CPU emulation runs normally on the emulator thread.
4. The client sends a JSONL request to `stdin`, the MCP backend parses
   it, dispatches into the handler table, and returns a JSONL response
   on `stdout`.
5. When the client closes `stdin` (= ends the MCP session), the emu
   detects EOF and shuts down cleanly.

## Lifetime

The emu instance lifetime is tied to the MCP session lifetime:

- The client ends the session - emu gets EOF and exits.
- The client opens a new session - the Python wrapper spawns a new
  emu instance.

For a persistent session where the emu state survives between
connections, use the [TCP transport](transport-tcp.md).

## Limitations

- **1 client per session** - the pipe is point-to-point, it cannot
  be shared between multiple clients.
- **No human in the loop** - the GUI emu does not run; the user
  sees no screen and hears no audio.
- **No persistent state** - after the session ends, snapshots,
  breakpoints, and media mounts are gone. Use the MCP `state_save`
  tool into a file if you want to carry state between sessions.

## Output channel separation (stdout vs stderr)

In pipe mode `stdout` is **reserved exclusively for the JSONL protocol**
- each line is exactly one JSON-RPC message (hello, response, event).
All other emulator output (informational messages, keyboard hints, speed
messages, peripheral banners, error messages) goes to `stderr`.

The emulator writes a lot to stdout during startup and runtime. To stop
this plain text from corrupting the JSONL stream at the client, the emu
redirects its stdout to stderr at the very start of pipe mode and keeps
the original stdout for the protocol only. Consequences for integration:

- **Parse only `stdout`** as JSONL - every line is valid JSON.
- **`stderr` is the log** - log it separately or discard it; it is not
  part of the protocol and its format is not stable.
- **Never merge the two channels** into one stream (e.g. `2>&1`) - that
  would break JSONL parsing.

## Diagnostics

If emu crashes immediately on startup, try running it manually without
the wrapper and split the channels into files:

```bash
mz800emu.exe --mcp-pipe --headless --no-first-run-windows 1>out.jsonl 2>err.log
# (will hang waiting for JSONL on stdin - terminate with Ctrl+C or EOF)
```

`out.jsonl` must contain only JSONL (the hello message at the start),
`err.log` all logs including initialization. If emu does not even reach
the first log line in `err.log`, the problem is outside the MCP backend
(= a failure in emu initialization itself). For visible logs in the
console on Windows you need a `FORCE_CONSOLE=1` build (see
[Headless mode](headless-mode.md)).

## Related

- [Python wrapper](python-wrapper.md) - how the Python wrapper uses
  this transport via FastMCP
- [TCP transport](transport-tcp.md) - alternative transport for
  human in the loop
- [Headless mode](headless-mode.md) - independent use of the
  `--headless` flag
