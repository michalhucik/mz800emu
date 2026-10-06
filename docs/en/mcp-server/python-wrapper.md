# Python wrapper (mcp_server.py)

The Python wrapper `mcp_server.py` is a FastMCP client that bridges
MCP clients (Claude Code, Claude Desktop, ...) to the MCP backend
built into the emulator. The wrapper communicates with emu over a
JSONL transport (pipe or TCP) and itself exposes a standard MCP
JSON-RPC API over stdio.

## Architecture

```
+----------------+        MCP JSON-RPC stdio        +-----------------+
|  Claude Code   |  <----------------------------> |  mcp_server.py  |
+----------------+                                 +--------+--------+
                                                            |
                                          JSONL over        |
                                  pipe (subprocess) OR      |
                                  TCP socket (GUI emu)      |
                                                            v
                  +---------------- pipe transport ----------------+
                  |   subprocess: mz800emu.exe --mcp-pipe          |
                  |   --headless --no-first-run-windows            |
                  |   (each session has its own instance)          |
                  +------------------------------------------------+

                  +---------------- tcp transport -----------------+
                  |   running GUI mz800emu.exe                     |
                  |   with MCP TCP Server -> Start (port 23800)    |
                  |   (shared live session for user + AI)          |
                  +------------------------------------------------+
```

## Installation

Prerequisites: MSYS2/MinGW64 or Linux, Python 3.10+, a clone of the
repository.

### 1. Emulator build

The MCP backend (= JSONL transport + dispatch layer inside the emu) is
**enabled by default** in the standard build. Just:

```bash
# UCRT64 shell (= MSYSTEM=UCRT64)
mingw32-make mz800emu
# After build mz800emu.exe must exist in the repo root
ls mz800emu.exe
```

Verify the binary exposes the MCP switches:

```bash
./mz800emu.exe --help | grep -i mcp
# Expected:
#   --mcp-pipe                  start the MCP server over stdio pipe
#   --mcp-tcp-port=N            start the MCP TCP listener on port N
```

#### Compile-time toggles

| Flag | Default | Meaning |
|------|---------|---------|
| (none) | MCP enabled | Standard build, MCP backend embedded in the binary |
| `NO_MCP=1` | MCP disabled | `make NO_MCP=1 mz800emu` - binary without MCP, no `--mcp-*` switches |
| `NO_MCP_TCP=1` | TCP enabled | TCP listener disabled, pipe transport remains |
| `NO_DEBUGGER=1` | debugger enabled | Implies `NO_MCP=1` (MCP requires the debugger subsystem) |

Cascade: `NO_DEBUGGER=1` ⇒ `NO_MCP=1` ⇒ `NO_MCP_TCP=1`. If you do
not know why you ended up with a build without MCP, it is possible
that you (or a dist maintainer) passed the `NO_DEBUGGER` flag.
The standard `make mz800emu` provides full MCP functionality.

### 2. Initialize venv + generate `.mcp.json`

```bash
cd mcp-server/
./mcpinit.sh
```

The script is idempotent and does everything required:

- creates `.venv` (skipped if it already exists)
- installs `requirements.txt` via the venv Python
- sanity check `import mcp`
- generates `mcp-server/.mcp.json` with absolute paths matching this
  install (= venv Python, `mcp_server.py`, repo root, `mz800emu.exe`).
  If a previous `.mcp.json` existed with different content, it is
  backed up as `.mcp.json.bak`.

You do **not** need to activate the venv in your shell - Claude Code
spawns `mcp_server.py` as a subprocess using the explicit venv Python
path from `.mcp.json`.

Note: under MSYS2 mingw64 Python see "MSYS2 caveats" below (= specific
issue with `pydantic-core` during `mcp[cli]` install).

#### Manual procedure without the script

```bash
cd mcp-server/
python -m venv .venv
source .venv/bin/activate     # MSYS2: source .venv/Scripts/activate
pip install -r requirements.txt
# .mcp.json then needs manual editing - see step 3 below for the
# structure template, plus fill in absolute paths (= venv Python,
# mcp_server.py, repo root, mz800emu.exe).
```

### 3. Connecting Claude Code

The generated `mcp-server/.mcp.json` contains two entries:

| Entry name | Transport | Use case |
|------------|-----------|----------|
| `mz800emu`     | pipe | CI, batch sessions, AI without GUI emu |
| `mz800emu-tcp` | tcp  | "Human in the loop" with a shared GUI session |

Copy the file to its target location:

- `~/.claude/.mcp.json` (= user-level), or
- `<project>/.claude/.mcp.json` (= project-level)

Restart Claude Code. The `/mcp` command in chat lists both servers
and `tools/list` returns the available `emu_*` tools.

#### .mcp.json structure

For reference, the generated file looks roughly like this (paths
vary by install location):

```json
{
  "mcpServers": {
    "mz800emu": {
      "command": "<absolute path to venv Python>",
      "args":    ["<absolute path to mcp_server.py>"],
      "cwd":     "<absolute path to repo root>",
      "env": {
        "MZ800EMU_EXE":       "<absolute path to mz800emu.exe>",
        "MZ800EMU_TRANSPORT": "pipe"
      }
    },
    "mz800emu-tcp": {
      "command": "<absolute path to venv Python>",
      "args":    ["<absolute path to mcp_server.py>"],
      "cwd":     "<absolute path to repo root>",
      "env": {
        "MZ800EMU_TRANSPORT": "tcp",
        "MZ800EMU_TCP_HOST":  "127.0.0.1",
        "MZ800EMU_TCP_PORT":  "23800"
      }
    }
  }
}
```

## MSYS2 caveats / mingw Python

MSYS2 mingw/ucrt Python (platform `mingw_x86_64_*`) **cannot pip-build**
the Rust-native dependencies of `mcp[cli]`. It is not just
`pydantic-core` - `mcp[cli]` has several (`pydantic-core`, `rpds-py` via
jsonschema, ...), all failing with `Unsupported platform: mingw_*` (the
Rust toolchain does not support that target). Installing them one by one
from pacman is whack-a-mole and some have no pacman package.

**Recommended fix: use vanilla Windows Python from python.org** (it has
prebuilt wheels for all of these, no Rust build).

`mcpinit.sh` handles this **automatically**: if `python` on PATH is a
mingw build, the script falls back to the `py -3` launcher (= python.org
Python). Just have a python.org Python installed. You can also force it:

```bash
PYTHON='C:/path/to/python.exe' ./mcpinit.sh
```

Manual procedure (without the script) using a python.org Python:

```bash
cd mcp-server/
py -3 -m venv .venv          # or the full path to python.org python.exe
.venv/Scripts/python -m pip install -r requirements.txt
```

Last-resort (fragile) - stay on mingw Python and pacman-install **every**
Rust dep + a venv with `--system-site-packages`:

```bash
pacman -S mingw-w64-ucrt-x86_64-python-pydantic \
          mingw-w64-ucrt-x86_64-python-rpds-py
python -m venv --system-site-packages .venv
.venv/bin/python -m pip install -r requirements.txt
```

(Prefix per MSYSTEM: UCRT64 = `mingw-w64-ucrt-x86_64-`, MINGW64 =
`mingw-w64-x86_64-`. If some dep lacks a pacman package this path fails -
python.org is the safer route.)

## Transport selection

The wrapper reads transport from env variables:

| Env var | Default | Values / meaning |
|---------|---------|------------------|
| `MZ800EMU_TRANSPORT` | `pipe` | `pipe`, `tcp` |
| `MZ800EMU_EXE` | `../mz800emu.exe` | path to binary (pipe only) |
| `MZ800EMU_WORK_DIR` | (empty) | work directory of the spawned emulator, passed as `--work-dir` (pipe only); relative outputs such as the CDL export on exit go there |
| `MZ800EMU_TCP_HOST` | `127.0.0.1` | hostname (tcp only) |
| `MZ800EMU_TCP_PORT` | `23800` | port number (tcp only) |

In `.mcp.json` these are set via `env`:

```json
{
  "mz800emu-tcp": {
    "command": ".../mcp-server/.venv/Scripts/python.exe",
    "args": [".../mcp-server/mcp_server.py"],
    "env": {
      "MZ800EMU_TRANSPORT": "tcp",
      "MZ800EMU_TCP_HOST": "127.0.0.1",
      "MZ800EMU_TCP_PORT": "23800"
    }
  }
}
```

Transport details: [Pipe](transport-pipe.md) | [TCP](transport-tcp.md).

## Lazy connect

The transport is not established at MCP server startup - it is
deferred **until the first tool call**. Discovery (`tools/list`) from
the Claude client is therefore instant. This means:

- Claude Code shows the tool list immediately after `/mcp` query.
- The first real tool call (e.g. `emu_ping`) initializes the transport
  (= pipe spawn or TCP connect).
- If the transport fails, you see the error at the first tool call,
  not at discovery.

## Emulator exit and automatic restart

If the emulator process ends while a tool call is waiting for its
response (crash, or the process exited for another reason), the call
fails **immediately** with an error such as
`emulator process exited (exit code 3221225477 = 0xC0000005) while
waiting for the response (cmd=run); ...` - it does not wait for the
30 s request timeout. In pipe mode the emulator state is lost. In TCP
mode the text is `emulator connection closed (...)`; only the
connection was lost - the GUI emulator may still be running with its
state.

After such an unexpected end:

- `emu_status` returns `{"running": false, "connected": false,
  "last_exit": "<reason>"}`.
- The next tool call that needs the emulator starts a fresh emulator
  process (pipe; RAM, registers, breakpoints and inserted media are
  back to defaults) or reconnects (TCP). The tool call that triggered
  the restart reports it: its result carries `"restarted": true` and
  `"restart_reason"`; if that call fails, its error text ends with
  `[restarted: true; <reason>]`. Only that one call reports it, also
  when several calls run at the same time. If the restart was triggered
  by reading a resource, the next tool result carries the fields.
- `emu_stop` followed by another tool call, or an explicit
  `emu_start`, is not reported as a restart.

## Logging

Wrapper logging is driven by the `[MCP]` section of `mz800emu.ini`
via the `wrapper_log_*` keys (see
[MCP server configuration](configuration.md), sub-section
**MCP Wrapper Log**). **The log is disabled by default** - no file
is created until the user explicitly enables a level.

Important constraint: the log **must not go to stdout** - stdout is
reserved for the MCP wire protocol (JSON-RPC). The wrapper therefore
directs all log output exclusively to a file via `FileHandler` /
`RotatingFileHandler` / `TimedRotatingFileHandler`.

If the INI key `wrapper_log_path` is empty, the default is
`mcp-server/mcp_server.log` (next to `mcp_server.py`):

```bash
# Enable minimal logging (single INI edit):
# [MCP]
# wrapper_log_level = info

tail -f mcp-server/mcp_server.log
```

The wrapper reads the INI at its startup - changes made through the
GUI Settings (Tools -> MCP TCP Server -> Settings... -> MCP Wrapper
Log) **take effect only on the next startup** of `mcp_server.py`
(= Claude Code typically restarting the MCP host or switching
workspaces).

For env-based INI path override during tests: `MZ800EMU_INI=/path/to/test.ini`.

### Error tolerance

On failure (missing INI, invalid values, file open failure) the
wrapper does not crash - it installs a `NullHandler` and continues
without logging. Diagnostics then require fixing the INI and
restarting the wrapper.

### DEBUG wire trace (TX/RX)

At `wrapper_log_level = DEBUG` the wrapper additionally logs the raw
JSON-RPC wire bytes in both directions:

- `TX: <json>` - outgoing JSONL request to the emulator (from
  `_send_request`)
- `RX: <json>` - incoming JSONL line from the emulator (from the stdout
  reader task; also captures async broadcasts without `req_id`, e.g.
  MCP_ACTION)

The wire trace is **DEBUG only** - INFO and lower levels stay clean
(only connect / EOF / error events), so the regular operational log is
not flooded with every protocol message. Long payloads (base64
screenshot, mem dump) are truncated in the log to the first 500
characters with a `...[N chars]` marker so the log does not grow by
megabytes.

## Running manually

```bash
# Assumes mcp[cli] is installed (= venv active):
python mcp_server.py
# The server listens on stdio - without an MCP client it just hangs.
# Ctrl+C exits.
```

FastMCP CLI test:

```bash
mcp --help
# FastMCP also has dev mode tooling:
mcp dev mcp_server.py
```

## Troubleshooting

### "mz800emu binary not found at ..."

The pipe transport spawn failed - the wrapper did not find the binary.
Diagnosis:

1. Verify `mz800emu.exe` exists in the repo root:
   ```bash
   ls mz800emu.exe
   ```
2. If not, build it: `mingw32-make mz800emu`.
3. If the binary lives elsewhere, set env: `MZ800EMU_EXE=/full/path/to/mz800emu.exe`.

### TCP transport: "Connection refused"

The wrapper in tcp mode cannot establish a connection. Diagnosis:

1. In the GUI emu check: Tools -> MCP TCP Server -> status (Start).
2. Or launch the GUI with: `mz800emu.exe --mcp-tcp-port=23800`.
3. Verify port 23800 is not blocked by firewall or held by another
   process (`netstat -ano | grep 23800` on Windows).

### Claude Code: tools not showing in `/mcp`

1. Verify `.mcp.json` was copied to its target location
   (`~/.claude/.mcp.json` or `<project>/.claude/.mcp.json`) and contains
   absolute paths. If you used `mcpinit.sh` the absolute paths are
   guaranteed; if you edited manually, double-check.
2. Check Claude Code logs (Help -> Show logs).
3. Test the bridge manually: `python mcp_server.py < /dev/null` should
   print an MCP `initialize` error (= bridge came up). If `ImportError`
   or `ModuleNotFoundError`, FastMCP is missing from the active Python
   (= run `./mcpinit.sh` or manual `pip install -r requirements.txt`).
4. Check `mcp_server.log` - if empty, the server never reached
   `main()` (= wrong interpreter in `.mcp.json command`).

### Subprocess spawn issue (CreateProcess fail)

Windows specific: if `.mcp.json` `command` points to a shell script
or `python.exe` without a full path, CreateProcess may fail with
WinError 193 (`%1 is not a valid Win32 application`). Always use
the full path to `python.exe` (= `.../.venv/Scripts/python.exe`).

### `mem_write` returns "MEM_WRITE region check failed"

The target address landed in a non-RAM region (ROM, CG-ROM,
prohibited, unmapped). This is expected - regular `mem_write` only
writes into RAM. For forced writes into ROM/CG-ROM use
`mem_write_force` (sensitive tool). The RAM region typically covers
0x1000..0x7FFF in MZ-800 banking, but depends on the current banking
state.

## Related

- [Pipe transport](transport-pipe.md)
- [TCP transport](transport-tcp.md)
- [Configuration](configuration.md) - INI keys in section `[MCP]`
- [Tools overview](tools-overview.md) - available tools, sensitive flag
- [Resources overview](resources-overview.md) - read-only resources
