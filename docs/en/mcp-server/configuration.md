# MCP server configuration

The MCP server is configurable from three sources, in priority order
from lowest to highest:

1. **INI file** `mz800emu.ini`, section `[MCP]` (= persistent)
2. **GUI Settings dialog** in the main window (= persistent, writes
   into INI)
3. **CLI overrides** at startup (= runtime, not persisted)

## INI section `[MCP]`

The INI file `mz800emu.ini` in the user config directory contains
section `[MCP]` with the following keys:

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `tcp_port` | unsigned | `23800` | TCP port for MCP server (1024-65535) |
| `bind_address` | keyword | `127.0.0.1` | `127.0.0.1` (localhost) or `0.0.0.0` (all) |
| `profile` | keyword | `wild` | Security profile: `wild` / `confined` / `sandboxed` / `observer` |
| `auto_start_tcp` | bool | `false` | Start TCP server automatically at emu start |
| `action_log_path` | text | `` (empty) | File path for disk-persisted MCP/USER actions; empty = disabled |
| `wrapper_log_level` | keyword | `off` | Python wrapper log level: `off` / `error` / `warning` / `info` / `debug` |
| `wrapper_log_path` | text | `` (empty) | Wrapper log file path; empty = default `mcp-server/mcp_server.log` |
| `wrapper_log_rotate_kind` | keyword | `none` | Rotation kind: `none` / `size` / `time` |
| `wrapper_log_rotate_size_mb` | unsigned | `10` | Size in MB before rotation (only for `size`, 1-10240) |
| `wrapper_log_rotate_when` | text | `midnight` | Token for `TimedRotatingFileHandler` (only for `time`) |
| `wrapper_log_rotate_keep` | unsigned | `5` | Number of rotated files (backupCount, 0-1000) |

Numbers (`unsigned`) are hexadecimal in the INI file, like everywhere in
the configuration file: the emulator saves them with the `0x` prefix and
reads them as hexadecimal even without it (`wrapper_log_rotate_size_mb = 50`
means 80 MB). `bool` values are `0` and `1`. The table gives the defaults
in decimal.

Example INI:

```ini
[MCP]
tcp_port=0x5cf8
bind_address=127.0.0.1
profile=wild
auto_start_tcp=1
action_log_path=/home/user/mz800-activity.log
wrapper_log_level=off
wrapper_log_rotate_kind=none
```

### action_log_path - disk persist Activity log

When a non-empty path is configured, every recorded MCP / USER
command is appended to a plain-text file using the format:

```
<ISO 8601 UTC> [<origin>] <cmd>: <description>
```

Example output:

```
2026-05-27T14:23:01Z [mcp] mem_read: addr=0x4000 len=16
2026-05-27T14:23:02Z [user] pause:
2026-05-27T14:23:05Z [mcp] bp_add: addr=0x5000 type=PC_EXEC
```

The `origin` tokens are `user` / `mcp` / `test` / `internal` and are
stable - safe to grep in shell scripts. Writes are best-effort: if
the file cannot be opened (= disk full, permission error), the entry
is still written into the Activity window RAM ring buffer and the
write error is silently ignored.

To disable: omit the key or set it to an empty string.

### MCP Wrapper Log - Python wrapper logging

The `wrapper_log_*` group controls logging of the Python MCP wrapper
(`mcp-server/mcp_server.py`). The C emu side merely persists the
values; the actual interpretation happens in Python on startup
(= mz800emu has no runtime control over the wrapper). These keys
replace the previously hardcoded INFO log behavior.

**The log is disabled by default** (`wrapper_log_level = off`) - no
file is created and the root logger gets a `NullHandler`. For
diagnostics the level must be enabled.

#### Rotation kinds

| `wrapper_log_rotate_kind` | Python handler | Active keys |
|---------------------------|----------------|-------------|
| `none` | `logging.FileHandler` | only `wrapper_log_path` |
| `size` | `logging.handlers.RotatingFileHandler` | `wrapper_log_rotate_size_mb`, `wrapper_log_rotate_keep` |
| `time` | `logging.handlers.TimedRotatingFileHandler` | `wrapper_log_rotate_when`, `wrapper_log_rotate_keep` |

Values for `wrapper_log_rotate_when` follow Python documentation:

- `S` - seconds
- `M` - minutes
- `H` - hours
- `D` - days
- `midnight` - rotate at midnight
- `W0` to `W6` - rotate on a given weekday (W0 = Monday)

#### Examples

Continuous INFO log to the default path:

```ini
[MCP]
wrapper_log_level = info
```

Size-rotated DEBUG log, 50 MB max, 10 historical files:

```ini
[MCP]
wrapper_log_level = debug
wrapper_log_path = C:/mz800-logs/wrapper.log
wrapper_log_rotate_kind = size
wrapper_log_rotate_size_mb = 0x32
wrapper_log_rotate_keep = 0x0a
```

Daily rotation with 30-day history:

```ini
[MCP]
wrapper_log_level = info
wrapper_log_path = /var/log/mz800/wrapper.log
wrapper_log_rotate_kind = time
wrapper_log_rotate_when = midnight
wrapper_log_rotate_keep = 0x1e
```

#### Error tolerance

The Python wrapper falls back to **logging disabled** (NullHandler)
in these situations:

- INI file missing
- INI cannot be parsed
- `[MCP]` section missing
- Unknown `wrapper_log_level` value
- File open fails (non-existent directory, permission denied)
- Unknown `wrapper_log_rotate_when` token (a plain FileHandler is
  used instead, no rotation)

The wrapper keeps running in all these cases, just without a log.

## User knowledge base (`MZ800EMU_USER_KB_DIR`)

Beyond the built-in AI-reference documents (`docs/agent/*.md`, exposed
as `emulator://docs/*`), a user can expose their own Markdown notes to
AI clients without touching the repository.

This is **wrapper-side configuration** - it does NOT belong in the
`[MCP]` section of `mz800emu.ini` (that section is owned by the C core
and rewritten on save, so an unknown key would be wiped). Instead it is
set via the **`MZ800EMU_USER_KB_DIR` environment variable**, typically
in the `env` block of the `.mcp.json` file the MCP client (Claude Code
etc.) uses to launch the server:

```json
"env": {
  "MZ800EMU_EXE": "...",
  "MZ800EMU_TRANSPORT": "pipe",
  "MZ800EMU_USER_KB_DIR": "C:/Users/me/my-mz-notes"
}
```

`mcpinit.sh` generates `.mcp.json` with an empty `MZ800EMU_USER_KB_DIR`
(= disabled); just fill in the path. Empty = no `kb` resources.

Every `*.md` under that directory is scanned **recursively** and
registered as `emulator://kb/<relative path without .md>` (path
separators become `/`). Example: `my-mz-notes/hw/vram.md` ->
`emulator://kb/hw/vram`. The `emulator://kb/*` namespace is separate
from the built-in `emulator://docs/*`, so user notes never collide with
the project reference.

The title and description (shown in `resources/list`) are taken from the
document: an optional front matter block (`title:` / `description:`
between `---`) takes precedence, otherwise the first `# H1` heading is
the title and the first text paragraph is the description.

Error tolerance: an unset variable or a missing directory does not crash
the server - no `kb` resources are registered.

## GUI Settings dialog

In the main window, **Tools -> MCP TCP Server -> Settings...** opens
a dialog with editable fields for all keys. Changes are saved back
to the INI when the dialog is confirmed via **OK**.

Fields:

- **TCP Port** - numeric input, range validated 1024-65535
- **Bind Address** - dropdown (`127.0.0.1` / `0.0.0.0`)
- **Security Profile** - dropdown (`wild` / `confined` / `sandboxed`
  / `observer`)
- **Auto-start TCP server** - checkbox

Sub-section **MCP Wrapper Log**:

- **Log Level** - dropdown (`OFF` / `ERROR` / `WARNING` / `INFO` / `DEBUG`)
- **Log Path** - text input (empty = default `mcp_server.log` in the wrapper directory)
- **Rotation** - dropdown (`none` / `size` / `time`)
- **Max Size (MB)** - numeric input (active only for `size`)
- **Time When** - text input (active only for `time`)
- **Keep Files** - numeric input

Disabled states follow the log level and rotation kind (= a disabled
log disables everything else, `none` rotation disables rotation
fields). Changes are persisted to the INI on **Save**, but **take
effect only at the next startup** of `mcp_server.py` - the C emu
side does not control the wrapper at runtime.

Changes to `tcp_port` or `bind_address` take effect only on the next
**Start** of the TCP server (= a server currently running keeps the
old values). To apply immediately: Stop -> Settings... -> Save ->
Start.

## CLI overrides

At emu startup CLI flags override INI values:

| Flag | Overrides INI key |
|------|-------------------|
| `--mcp-tcp-port=PORT` | `tcp_port` |
| `--mcp-bind=ADDR` | `bind_address` |
| `--mcp-profile=PROFILE` | `profile` |

Using `--mcp-tcp-port` additionally **implies `auto_start_tcp = 1`
for this run only** (= the server comes up even if INI has
`auto_start_tcp = 0`). The persistent `auto_start_tcp` state is
not modified.

Example:

```bash
# One-shot run with port 24000, bind on all interfaces,
# sandboxed profile (= INI values stay as they are):
mz800emu.exe --mcp-tcp-port=24000 --mcp-bind=0.0.0.0 --mcp-profile=sandboxed
```

## Persistence flow

```
+----------+   load    +--------------+  override  +-------+
| INI file | --------> | runtime conf | <--------- | CLI   |
+----------+           +--------------+            +-------+
                              |
                              | read
                              v
                  +-------------------------+
                  | TCP server, dispatcher  |
                  +-------------------------+
                              ^
                              | save on OK
                              |
                       +-------------+
                       | GUI dialog  |
                       +-------------+
```

1. At emu startup the INI is loaded first -> the runtime
   configuration holds persistent values.
2. If the user passed a CLI flag, it overrides the corresponding
   field.
3. The TCP server and dispatcher read current values from the
   runtime configuration.
4. If the user changes a value at runtime via the GUI Settings
   dialog, it is written back into the INI (= the next emu start
   already gets the new value, even without CLI flags).

## Security profile

The `profile` field is currently **only stored** in the INI and
propagated into the dispatch layer. Full enforcement (file
whitelisting, destructive tool blocking, audit log) is not available
in this release.

Status per profile:

| Profile | State |
|---------|----------|
| `wild` | active default - no restrictions beyond the default region check in `mem_write` |
| `confined` | persist-only - effectively no different from `wild` |
| `sandboxed` | persist-only - no different from `wild` |
| `observer` | persist-only - no different from `wild` |

Until full enforcement is available, stay on `wild` in a trusted
environment.

## Related

- [TCP transport](transport-tcp.md) - TCP server details, GUI
  Start/Stop
- [Python wrapper](python-wrapper.md) - transport selection on
  the client side
- [Tools overview](tools-overview.md) - which tools are sensitive
