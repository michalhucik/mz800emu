# Headless mode

The emulator can be started in **headless mode** via the CLI option
`--headless`. In this mode no graphics window is created and no audio
device is opened, but the emulation loop runs normally.

## Usage

```
mz800emu --headless
mz800emu --headless --run-mzf <path-to-program.mzf>
mz800emu --headless --no-first-run-windows
```

The process keeps running until externally terminated (`Ctrl+C` on
Linux, `taskkill /F /IM mz800emu.exe` on Windows, or an SDL quit
event), or until an exit condition of an unattended run is met (see
[Unattended runs](#unattended-runs)).

## When to use it

- **CI / batch test runner** - automated tests without requiring a
  display / X server / audio device
- **MCP subprocess** - the MCP client (Claude Code, Claude Desktop)
  spawns the emulator as a child process communicating over a named
  pipe
- **Headless benchmark** - performance measurement without GUI render
  overhead

## What is available in headless mode

| Component | State |
|-----------|-------|
| Z80 CPU emulation | runs normally |
| Peripherals (GDG, CTC, PPI, PSG, FDC, ...) | run normally |
| Framebuffer (in memory, BGRA 928×288) | rendered |
| Snapshot save/load | works |
| CMT hack (`--run-mzf`) | works |
| CMT hack load started by the ROM (boot menu C, `LOAD`) | **no** file dialog: the load is cancelled at once (the ROM reports Break) and a warning goes to stderr. Load the program with `media_load_mzf` or `--run-mzf`, or switch the CMT hack off (`cmt_hack_set`) and use the virtual tape |
| Console stdout/stderr | only in `FORCE_CONSOLE=1` builds (see below) |
| GUI window | **no** (skipped) |
| Audio output | **no** (no-op SDL audio device) |
| Emulation pace | system clock: 100 % = real time (MZ-800: 50 frames/s), custom speed and MAX SPEED as in windowed mode |
| ImGui debugger | **no** (context not created) |

Headless emulation therefore runs in real time. To run faster (tests, batches), set the speed at start with `--speed` (`--speed max` = MAX SPEED, `--speed 400` = four times real time; valid range 1-4000 %), or change it at runtime with the MCP `set_speed` tool. The `--speed` option applies to `mz800emu --headless`; with `--mcp-pipe` it is rejected (`Unknown option: --speed`, startup fails); in pipe mode set the speed with `set_speed`.

## Unattended runs

For CI and scripts without an MCP client, the emulator in `--headless`
ends by itself when a condition is met, writes the result to a JSON file
and returns an exit code.

```
mz800emu --headless --speed max --run-mzf test.mzf \
         --exit-on-pc 2000h --fail-on-pc 2100h --timeout-seconds 30 \
         --automation-dir out
```

### Exit conditions

| Option | Ends the run | Exit code |
|--------|--------------|-----------|
| `--exit-after-frames N` | after N emulated frames | 0 |
| `--timeout-frames N` | after N emulated frames, unless another condition came first | 2 |
| `--timeout-seconds S` | same, in emulated seconds (rounded up to whole frames) | 2 |
| `--exit-on-pc ADDR[,skip=N][,code=C]` | when the CPU executes ADDR (after skipping the first N passes) | C, default 0 |
| `--fail-on-pc ADDR[,skip=N]` | when the CPU executes ADDR | 1 |
| `--exit-on-write ADDR[=VAL][,skip=N][,code=C]` | when the program writes to memory address ADDR (only the value VAL, if given) | the written value, or C |
| `--exit-on-out PORT[,skip=N][,code=C]` | when the program writes to I/O port PORT (0-255) | the written value, or C |
| `--bpt FILE` | loads a breakpoint set; a breakpoint with the action `exit` ends the run | from the action |

- The first condition met wins. When `--exit-after-frames` and a timeout
  fall on the same frame, `--exit-after-frames` wins.
- `--exit-on-*` and `--fail-on-pc` can be repeated. They create normal
  breakpoints with the action `exit`; any breakpoint with this action
  works the same way (see
  [Action DSL](../debugger/breakpoints/action-dsl.md)). They and `--bpt`
  need a build with the debugger.
- Numbers: decimal, `0x2000` or `2000h`.
- Frames are counted from the start of the run, after `--run-mzf`. Time
  conditions are checked once per frame; they also work in a build
  without the debugger.
- Use `--timeout-*` as a safety net, so a stuck program does not block
  the CI job.
- The port for `--exit-on-out` is your choice. Pick one that the program
  and the hardware configuration you run do not use; writing to it still
  reaches the emulated hardware.
- In an unattended run (any of the options in this section) the
  breakpoints and `$` variables saved from earlier sessions are neither
  loaded nor saved, so leftovers from the GUI cannot change the result.
  Breakpoints come only from `--bpt` and the `--exit-on-*` /
  `--fail-on-pc` options.

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | success: `--exit-after-frames` done, `--exit-on-pc`, `exit 0` |
| 1 | test failed: `--fail-on-pc`, `exit 1` |
| 2 | timeout |
| 3 | error of the emulator or of the options (invalid option, missing file) |
| 4 | a replay played back with `--exit-at-replay-end` differs from the recording (`replay_desync`, see [../replay.md](../replay.md)) |
| 64-255 | free for codes written by the program (`--exit-on-out`, `exit Value`) |

Code 3 for an invalid option is used only when an unattended-run option is
given; otherwise an invalid option returns 1 as before.

### Result file and artifacts

| Option | Writes |
|--------|--------|
| `--result-json FILE` | the result as JSON |
| `--automation-dir DIR` | `DIR/result.json` and `DIR/screen.png` (the final screen); the folder is created |
| `--exit-screenshot FILE` | PNG of the final screen |
| `--exit-snapshot FILE` | snapshot (`.mzs`) of the final state |

The result is written at the end, also on a timeout or an error, so a
script can always read it. Video recording (`--record`) and audio capture
(`--audio-capture`) are finished before the process ends, whatever the
exit code.

Example (`--exit-after-frames 100 --automation-dir out`, MZ-800; hashes
and paths shortened):

```json
{
  "schema" : "mz800emu-result/1",
  "emulator" : { "version" : "2.0.6 devel", "build_revision" : -1, "commit" : "5ce317db",
                 "platform" : "MZ-800", "screens_per_second" : 50, "debugger" : true },
  "reason" : "frames",
  "exit_code" : 0,
  "message" : "100 frames done",
  "trigger" : { "kind" : "frames", "option" : "--exit-after-frames" },
  "time" : { "frames" : 100, "frames_total" : 100, "gdg_ticks" : 35442605,
             "gdg_ticks_total" : 35443245, "emulated_seconds" : 1.999966425153485,
             "wall_seconds" : 0.081809 },
  "cpu" : { "pc" : 33667, "sp" : 4334, "af" : 65356, "bc" : 1372, "de" : 842, "hl" : 842,
            "ix" : 16612, "iy" : 0, "af2" : 0, "bc2" : 0, "de2" : 0, "hl2" : 0,
            "i" : 0, "r" : 25, "im" : 1, "iff1" : 0, "iff2" : 0, "halted" : false },
  "raster" : { "beam_row" : 0, "frame_ticks" : 45 },
  "hashes" : { "algorithm" : "sha256", "ram" : "64090b03...", "memext" : "2fb5b470...",
               "vram" : "edf097e8...", "screen" : "f58b3934...", "cpu" : "9c1e07d2..." },
  "artifacts" : { "screenshot" : ".../out/screen.png", "snapshot" : null,
                  "audio_capture" : null, "video" : null },
  "inputs" : { "run_mzf" : ".../Gardner.mzf", "bpt" : null },
  "warnings" : [ ]
}
```

A run ended by a breakpoint has for example
`"reason" : "exit_action"` and
`"trigger" : { "kind" : "breakpoint", "option" : "--exit-on-pc", "bp_id" : 1, "bp_name" : "cli-exit-on-pc-1", "bp_type" : "PC_EXEC", "hits" : 1 }`
(`option` is null for a breakpoint that did not come from the command
line).

- `emulator.build_revision`: the revision number of an official build, -1
  for other builds.
- `reason`: `frames`, `timeout`, `exit_action` (breakpoint action `exit`),
  `error`, `quit` (the process was ended from outside before a
  condition, for example by `Ctrl+C` or after `--record-frames`), or
  `replay_end` / `replay_desync` (end of a replay played back with
  `--exit-at-replay-end` or `--replay-verify`).
- `replay`: `null`, or the result of the replay used in the run (`path`,
  `outcome`: `ended`, `desync`, `recorded`, ..., `outcome_frame`,
  `frames`, `deviated`, `desync_parts`, `message`).
- `coverage`: `null` without code coverage data, otherwise the coverage
  status as returned by `emu_coverage_status` (`level`, `complete`,
  `units`, `instructions`, `branches`, ...). Collect coverage from the
  start with `--coverage branch` or `--coverage detail` (or `[COVERAGE]
  mode` in the INI), write all rows at the end with
  `--coverage-save-on-exit FILE` (`.csv` = CSV, otherwise JSON, see
  [MCP tools overview](tools-overview.md); a relative path is relative to
  the current directory, as with `--result-json`). `--coverage-key` and
  `--coverage-capacity` set the `detail` level.
- `trigger.hits`: hits of the breakpoint counted after the skipped ones.
- `time.frames` and `time.gdg_ticks` count from the start of the run;
  `frames_total` and `gdg_ticks_total` from the start of the emulation.
  `emulated_seconds` is `gdg_ticks` in seconds of the emulated clock.
- `cpu`: registers as numbers (`af2` ... `hl2` are the shadow registers),
  `halted` true or false.
- `hashes`: SHA-256 of the internal RAM (64 KB), the MemExt RAM (only when
  connected), the VRAM (MZ-800: all four planes), the PCG (MZ-1500), the
  screen (the framebuffer as palette indices) and `cpu` (the Z80 registers
  including the hidden state: WZ, Q, EI delay, pending INT and NMI). Two
  runs with the same options give the same hashes, so a script can compare
  them with a reference.
- After an error before the emulation starts (invalid option), the file
  has only `schema`, `emulator`, `reason`, `exit_code`, `message`,
  `trigger`, `replay`, `artifacts`, `inputs` and `warnings`.

On Windows the standard build has no console output, so the result file
is the reliable channel for a CI job.

### Exit in the GUI and with MCP

The process ends only in `--headless`, and with an MCP TCP server only when
an unattended-run option is given. In the GUI, with `--mcp-pipe`, and in
`--headless` with an MCP TCP server but without such an option, an exit
condition only pauses the emulation: stderr shows the code and the MCP
event `paused` has `reason: "exit"` with `exit_code`, `exit_reason` and
`message`. `emu_run` then returns `stopped_by: "exit"`. The result file is
written if requested. The client decides what to do next.

## Console-enabled build (Windows)

The standard `mz800emu.exe` on Windows is linked as a GUI subsystem =
stdout / stderr are not visible in a pipe. For headless workflow where
you need to see logs:

```
mingw32-make mz800emu FORCE_CONSOLE=1
```

(Equivalent to `cmake -DMZ_FORCE_CONSOLE=ON`.) A `FORCE_CONSOLE=1`
build allocates a console at startup and stdout / stderr are visible
in the terminal or can be redirected to a file via a shell pipe.

The default remains the GUI subsystem (for end users with the Windows
Explorer icon experience).

## Known limitations

- **Window error log at startup** - 2-3 lines of `getSDL_Window_by_name():
  Failed to get window: main_window` - **benign**, callers have a
  `NULL` window guard and continue without error. The message can be
  safely ignored.

## Screenshot in headless mode

Resource `emulator://frame/screenshot.raw` works **also in headless
mode** (= `--mcp-pipe`, `--headless`). The handler
primarily reads the SDL display-ready snapshot
(`g_iface_video->fbsnapshot_pixels`); if NULL, it falls back to the
GDG live buffer (`g_framebuffer.pixels`, statically allocated BSS).

The response contains a `fallback_source` string field:

| `fallback_source` | Meaning |
|-------------------|---------|
| `"sdl_snapshot"` | Primary SDL display path (= typical in GUI mode) |
| `"gdg_live"` | Fallback to GDG live buffer (= typical in headless / race condition) |

### Python client example (raw RGBA → PIL Image)

```python
import base64
from PIL import Image

resp = emu.call('get_frame_screenshot_raw')
data = resp['data']
if not data.get('available'):
    raise RuntimeError(f"Screenshot unavailable: {data.get('reason')}")
print(f"Source: {data['fallback_source']}, "
      f"frame {data['source_screen_id']}")

raw = base64.b64decode(data['data_b64'])
img = Image.frombytes('RGBA', (data['width'], data['height']), raw)
img.save('mz_screen.png')
```

For INDEX8 processing (= without auto expand into RGBA) the
Resource `emulator://frame/framebuffer/info` exposes a 16-entry
palette; the raw pixels would have to be obtained via a different
Tool (= not via screenshot.raw, which performs the RGBA expand on
the backend).

### PNG to a file with area, scale and crop

`emu_screenshot_save_to_file` (command `screenshot_save_to_file`)
writes a PNG straight to a file on the server host. Optional parameters
select what is saved:

| Parameter | Values | Meaning |
|-----------|--------|---------|
| `area` | `full` (default), `canvas` | whole frame with the border, or the picture only |
| `scale` | `framebuffer` (default), `native`, `1` to `4` | see below |
| `crop` | `{"x", "y", "width", "height"}` | cut-out in output pixels, only with `framebuffer` or `native` |

- `framebuffer` - framebuffer pixels 1:1, one pixel per GDG clock
  horizontally and per picture line vertically (MZ-800 full frame
  928x288, canvas 640x200). This is the behaviour without parameters.
- `native` - one image pixel per dot of the computer. When all pairs of
  neighbouring framebuffer pixels in the area are equal (320-dot modes),
  the width is halved (MZ-800 canvas 320x200), otherwise it is kept
  (640x200); lines 1:1. The reply contains `hires`.
- `1` to `4` - the aspect ratio of the emulator window; `2` equals the
  window at 100 % (MZ-800 full frame 928x576). Scale `1` drops every
  second column in the 640-dot modes.

Example - the 256x192 middle of a 320x200 picture:

```python
emu_screenshot_save_to_file(path="C:/tmp/viewport.png", area="canvas",
                            scale="native",
                            crop={"x": 32, "y": 4, "width": 256, "height": 192})
```

An unknown value or a crop outside the image is an error, no file is
written.

### Frame series - every changed frame as PNG

For finding animation bugs, `emu_screenshot_series_start` saves a PNG of
every emulated frame that differs from the last saved one (commands
`screenshot_series_start`, `screenshot_series_stop`,
`screenshot_series_status`).

- Frames are taken in emulated time at the end of each frame, so no frame
  is skipped even at MAX SPEED. When writing cannot keep up, the emulation
  waits. The series only captures while the emulation runs.
- `area`, `scale` and `crop` work as with a single screenshot (default
  scale `native`). Only changes inside them count - with a crop you can
  follow one sprite and ignore the rest of the screen.
- Files: `frame_NNNNNNN.png`, where NNNNNNN is the emulated frame since
  the start of the series (gaps = unchanged frames), and `index.csv` with
  `frame,file,gdg_screen`.
- `only_changed` (default true), `frames` (stop after N emulated frames,
  0 = until stop), `max_files` (safety limit, default 10000), `dir`
  (default: a new folder in the screenshot output folder).

```python
emu_screenshot_series_start(area="canvas", frames=250)   # 5 s of PAL frames
emu_run(frames=250)
emu_screenshot_series_status()   # files, unchanged, dir, stop_reason
```

`emu_screenshot_series_stop(wait=True)` ends a series early and waits until
all frames are written.

### Known limitations

- **Race condition in GUI mode** - if the client requests
  screenshots very often (= at a frequency close to the 50 Hz
  frame rate), the handler may occasionally return
  `fallback_source: "gdg_live"` instead of `"sdl_snapshot"` in
  GUI mode (= the SDL render thread consumed between publish
  windows). The content is byte-identical, so clients typically
  do not need to special-case this.
- **Pre-emu screenshot** - before the first `frame_done` (= just
  after reset / power-on), the screenshot may return a zeroed
  buffer with `source_screen_id: 0`. Clients can detect this case
  via the `source_screen_id == 0` test.

## Related

- [MCP server overview](README.md)
