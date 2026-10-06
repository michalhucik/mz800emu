# docs/agent - reference documentation for AI MCP clients

This directory contains dense English reference documents that are
exposed to MCP clients (Claude Code, custom AI tooling) via Resources
of the form `emulator://docs/<topic>`. Files are plain Markdown.

The Python MCP wrapper (`mcp-server/mcp_server.py`) registers these
files **automatically** at startup - every `*.md` here except this
`README.md` becomes `emulator://docs/<filename without .md>`. There is
no per-file registration code, so adding a new document is enough; just
drop a `.md` in this directory.

The `title` and `description` shown in `resources/list` are extracted
from the document: an optional front matter block takes precedence,
otherwise the first `# H1` heading is the title and the first text
paragraph is the description.

## File index

| URI | File |
|-----|------|
| `emulator://docs/index` | `index.md` |
| `emulator://docs/error_handling` | `error_handling.md` |
| `emulator://docs/memory_layout` | `memory_layout.md` |
| `emulator://docs/bp_dsl` | `bp_dsl.md` |
| `emulator://docs/smart_vars` | `smart_vars.md` |
| `emulator://docs/action_dsl` | `action_dsl.md` |
| `emulator://docs/watch_dsl` | `watch_dsl.md` |
| `emulator://docs/eventlog_mask` | `eventlog_mask.md` |
| `emulator://docs/sharp_display_code` | `sharp_display_code.md` |
| `emulator://docs/mz800_keyboard` | `mz800_keyboard.md` |
| `emulator://docs/cmt_workflow` | `cmt_workflow.md` |
| `emulator://docs/videorec_workflow` | `videorec_workflow.md` |

This table is informative only - the actual resource list is built by
scanning this directory, so it always matches the files present.

## Optional front matter

A document may start with a small front matter block to override the
auto-extracted metadata (otherwise the `# H1` heading and the first
paragraph are used):

```markdown
---
title: Human readable title
description: One-line summary shown in resources/list.
---
# Heading
...
```

Only `title` and `description` are recognized. The block must be the
very first thing in the file, delimited by `---` lines.

## User knowledge base

Beyond these built-in documents, an end user can expose their own
Markdown notes to AI clients without touching the repository. Point the
server at a directory via the `MZ800EMU_USER_KB_DIR` environment
variable - typically in the `env` block of the `.mcp.json` the MCP
client uses to launch the server (this is wrapper-side config, kept out
of the C-owned `[MCP]` INI section on purpose):

```json
"env": {
  "MZ800EMU_USER_KB_DIR": "C:/Users/me/my-mz-notes"
}
```

Every `*.md` under that directory is scanned **recursively** and exposed
as `emulator://kb/<relative path without .md>` (path separators become
`/`). Example: `my-mz-notes/hw/vram.md` -> `emulator://kb/hw/vram`. The
`emulator://kb/*` namespace is separate from the built-in
`emulator://docs/*`, so user notes never collide with project reference.
Title / description extraction and front matter work the same way. If
the directory is unset or missing, no `kb` resources are registered.

## Conventions

- English only (= MCP wire protocol is locale-agnostic, AI clients
  consume the text directly).
- Hex notation `0x42` (C-style, matches expression syntax).
- No typographic dashes / smart quotes - plain ASCII `-` and `"`.
- Tables for register / port / category enumerations.
- Cross-reference other `emulator://docs/*` and live resources
  (`emulator://memory/map`, `emulator://watch`, `emulator://vars`).
- Mark unverified facts with `[unverified]` or `TODO`.

## Distribution

`make dist` copies the whole `docs/` tree (including `docs/agent/`) into
`dist/docs/`. The Python wrapper resolves the directory relative to
its own location (`mcp-server/.. /docs/agent/`), so the same code path
works in source tree and in distribution layout.
