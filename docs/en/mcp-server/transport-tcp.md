# TCP transport

The TCP transport lets the **AI client attach to a running GUI
emulator** through a TCP socket. Unlike the [Pipe transport](transport-pipe.md),
no new headless instance is created here - the AI shares the same live
session as the user sitting at the GUI.

## When to use it

- **"Human in the loop" debug** - you debug in the GUI (stepping,
  inspecting memory, changing breakpoints), the AI analyzes state
  in parallel and suggests next steps.
- **Demonstration** - you show someone how the AI controls the
  emulator in real time, they watch the GUI to see what happens.
- **Reverse engineering session** - the AI walks ROM routines and
  annotates them; you manually switch the platform, load a different
  MZF, the AI continues from the new state.
- **Long-running session** - the emu does not restart per MCP call,
  state (snapshots, breakpoints, mounts) persists between calls.

## How to start it

### From the GUI

1. Launch `mz800emu.exe` normally (= with GUI).
2. Open menu **Tools -> MCP TCP Server**.
3. Click **Start**.
4. The title bar of the main window shows an indicator:

   ```
   mz800emu [MCP:23800 0 clients]
   ```

5. Once an AI client connects, the client count updates:

   ```
   mz800emu [MCP:23800 1 client]
   ```

### From the command line (auto-start)

If you want the TCP server to come up immediately at emu start:

```bash
mz800emu.exe --mcp-tcp-port=23800
```

Alternatively, set `auto_start_tcp = 1` in the INI configuration
(see [Configuration](configuration.md)) - then the TCP server starts
automatically on every emu launch.

## Title bar indicator

The main window title bar shows the current TCP server state:

| Title bar state | Meaning |
|-----------------|---------|
| `mz800emu` | TCP server off |
| `mz800emu [MCP:23800 0 clients]` | server up, waiting for client |
| `mz800emu [MCP:23800 1 client]` | one active client |
| `mz800emu [MCP:23800 N clients]` | N active clients (currently capped at 1) |

## Connecting a client

The Python wrapper connects via the env variable `MZ800EMU_TRANSPORT=tcp`:

```bash
export MZ800EMU_TRANSPORT=tcp
export MZ800EMU_TCP_HOST=127.0.0.1
export MZ800EMU_TCP_PORT=23800
python mcp_server.py
```

This configuration is preset in `.mcp.json` under the entry
`mz800emu-tcp`. Details: [Python wrapper](python-wrapper.md).

## Events over TCP

MCP events (`emu_event_subscribe` / `emu_event_poll`) work over the
TCP transport too, not only over the pipe. An AI client attached to a
live GUI session can subscribe to topics (`breakpoint_hit`, `paused`,
`step_done`, `io_write`) and poll pending events exactly as in pipe
mode.

## Limitations

- **Max 1 client** - the server accepts a connection, but a second
  concurrent client is rejected.
- **Localhost only** - default `bind_address = 127.0.0.1`. To allow
  remote connections set `bind_address = 0.0.0.0`, but see the
  security warning below.
- **No authentication** - anyone who can reach the bind address can
  control the emu.
- **No TLS** - the protocol runs as raw JSONL (line-delimited JSON)
  without encryption.

## Security warning

Setting `bind_address = 0.0.0.0` exposes the MCP server on **all
network interfaces**. Anyone on the same LAN (or through firewall
forwarding even from the internet) can:

- Read emu memory (= program and data contents)
- **Overwrite memory** via `mem_write` (= destructive operation)
- Pause / restart the emu
- Set breakpoints

Without authentication this means **full emu control for anyone on
the network**. Stay on `127.0.0.1` unless you have a specific reason
to open remote access, and in that case restrict access via firewall.

## Related

- [Pipe transport](transport-pipe.md) - alternative transport
  without GUI emu
- [Python wrapper](python-wrapper.md) - connecting from Claude
  Code
- [Configuration](configuration.md) - persistent TCP server
  settings in INI file
