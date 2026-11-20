# MZ-800 Emulator

The project moved from SourceForge to GitHub. The latest source code and
releases are now available at <https://github.com/michalhucik/mz800emu>.

Older 1.0.x releases remain archived on the original SourceForge
page: <https://sourceforge.net/projects/mz800emu/>


## Windows

If you want to always have the console displayed on the Windows platform, you have three options:

- Run program from MSYS2 console
- Create shortcut for `mz800emu.exe` and add exec parameter `--console`
- Compile program with parameter `FORCE_CONSOLE=1`

The `--help` and `--version` options also print to the console the program was
started from (cmd.exe, PowerShell) - without `--console`. cmd.exe does not wait
for a windowed program, so the output may appear after the prompt; redirecting
to a file is cleaner, e.g. `mz800emu --version > version.txt`.


## Keyboard mapping

Most keys are located in the same positions as the MZ-800 — like in the emulator from Zdenek Adler.

### Specialities & Differences

| Sharp key    | PC key                  |
|--------------|-------------------------|
| `GRAPH`      | CapsLock                |
| `ALPHA`      | `\` (both keys, see below) |
| `BLANK_KEY`  | `~`                     |
| `ESC`        | Esc, or End             |
| `INST`       | Insert                  |
| `DEL`        | Backspace, or Delete    |
| `@`          | F6                      |
| `\`          | F7                      |
| `?`          | F8                      |
| `LIBRA`      | F9                      |

**Note on `ALPHA`:** On keyboards with a Czech (and generally ISO) layout there
are two physical keys that produce `\`: the standard Backslash and the ISO key
next to the left Shift (reported by the PC as "Oem102" / Non-US Backslash). Both
act as `ALPHA` - no need to worry about which one you have.


### Differences between models (MZ-800 vs MZ-700 / MZ-1500)

The MZ-700 and MZ-1500 keyboards differ from the MZ-800: they have no TAB
key. In its place sits a longer ALPHA key (same width as TAB on the MZ-800),
so the SHIFT row has one key fewer and the left SHIFT is the same size as the
right one. The host Tab key therefore acts as ALPHA on the MZ-700 and
MZ-1500.

| Sharp key    | MZ-800        | MZ-700 / MZ-1500          |
|--------------|---------------|---------------------------|
| `ALPHA`      | PC `\`        | PC `\` or PC Tab          |


## Command-line options

The emulator supports a small set of long options. Options not listed below
are rejected with an error - use `--help` for a generated listing.

| Option | Argument | Description |
|--------|----------|-------------|
| `--help` | - | Print the option list and exit. |
| `--version` | - | Print the version, revision, build date, source origin (branch, commit, uncommitted changes, when available), compiler, compiled-in features, SDL and GLib versions and the host OS, then exit. Works in every binary (`mz800emu`, `mz700emu-pal`, `mz700emu-ntsc`, `mz1500emu`) and with `--mcp-pipe`; neither the emulator nor a window is started. |
| `--console` | - | Windows only: allocate a console window for stdout/stderr. |
| `--run-mzf` | `<filepath>` | Automatically load and run the given MZF file after the emulator boots. The program starts as if the ROM had loaded it from the tape, without running the ROM loader; register BC holds the device code set in the menu HW & Devices > CMT > CMT Options > Device for Direct MZF Start (default CMT). |
| `--run-mzf-device` | `<cmt\|fd\|qd\|rd\|value>` | With `--run-mzf`: the device code in register BC, as if the program was loaded from that device: `cmt` (0100h, on MZ-700 0000h - the value the ROM leaves after a tape load), `fd` (0200h), `qd` (0300h), `rd` or `sram` (0000h), or a number 0-65535 (`512`, `0x0200` or `0200h`; a decimal number is written without leading zeros (`0200` is an error, write `0x0200` or `0200h`)). Some programs, for example the BASIC 1Z-016, choose their default device by it. Applies to this start only; the setting in the menu and in the INI file is kept. An invalid value or a missing `--run-mzf` is an error. |
| `--cmt` | `<filepath>` | Insert a tape into the virtual cassette recorder at startup: `.tmz`, `.tzx`, `.mzf`, `.m12`, `.mzt`, `.tap`, `.wav` or `.wave` (see [devices/cmt.md](devices/cmt.md)). If the CMT hack is on, it is turned off for this session only (the `.ini` keeps the original setting). In the window, the Virtual CMT window opens. Missing file or unknown extension = error and exit. |
| `--cmt-play` | - | With `--cmt`: press PLAY after inserting the tape. When the emulator boots through the ROM, the ROM stops the tape motor during its startup, so the tape waits in PLAY for the motor (Waiting for motor); it plays at once only together with `--run-mzf`. |
| `--cmt-play-pause` | - | With `--cmt`: the tape waits in PLAY for the motor (Waiting for motor) and starts moving when the ROM or the program turns the cassette motor on (e.g. `LOAD` in BASIC, loading from the monitor). Cannot be combined with `--cmt-play`. |
| `--cmt-boot` | - | MZ-800 only, with `--cmt`: like `--cmt-play-pause`, and the emulator holds the C key from start until the ROM turns the motor on, so the boot menu loads the program from the tape. Works in both positions of the mode switch. Cannot be combined with `--run-mzf` or `--cmt-play`. |
| `--fdd` | `<N>=<filepath>` | Insert a DSK image into floppy drive N at startup; the option can be repeated. N 0-3 = drives FDD0-FDD3 of the FDC controller (ports 0xD8 - 0xDF), 4-7 = drives FDD0-FDD3 of the secondary FDC1 controller (ports 0x58 - 0x5F, must be connected). It replaces the disk from the `.ini` in the same drive and the path is saved to the `.ini` like a disk inserted from the menu (unless you use `--no-save-ini`). A missing file, a number outside 0-7 or a drive given twice = error and exit; a disconnected FDC1 or an image that cannot be opened = error (in `--headless` exit with code 3, in the window a pause). See [devices/fdc.md](devices/fdc.md). |
| `--fdd0` .. `--fdd7` | `<filepath>` | Same as `--fdd <N>=<filepath>` for drive 0-7. |
| `--fdd-readonly` | `<N>` | Write-protect drive N (0-7) for this run only; the option can be repeated. Applies to a disk inserted from the command line or from the `.ini`; the `.ini` setting is not changed. |
| `--cdl-mode` | `<off\|window\|always>` | Set the CDL (Memory Heatmap) recording mode. |
| `--cdl-dir` | `<dirpath>` | Set the target directory for CDL export. The directory is created on export if it does not exist. |
| `--cdl-name` | `<basename>` | Set the CDL export basename (without `.json`). The full meta path is `<dir>/<name>.json`; per-region binary files are saved as `<dir>/<name>_<region>.cdl`. |
| `--cdl-save-on-exit` | `<on\|off>` | Enable/disable automatic CDL export when the emulator exits. |
| `--cputrack-mode` | `<off\|window\|always>` | Set the CPU Tracking Log recording mode (one of four trace-suite subsystems). |
| `--cputrack-dir` | `<dirpath>` | Target directory for cputrack export. |
| `--cputrack-name` | `<basename>` | Basename for cputrack export files. |
| `--cputrack-chunk-mb` | `<N>` | RAM chunk size before disk swap (default 64). |
| `--cputrack-max-total-mb` | `<N>` | Max total recording size in MB, 0 = unlimited (default 2048 = 2 GB). |
| `--cputrack-save-on-exit` | `<on\|off>` | Auto-finalize cputrack export when the emulator exits. |
| `--cputrack-regs` | `<on\|off>` | Record CPU registers with every instruction (format 3, 32 bytes per record instead of 12; default off). |
| `--iorqlog-mode` | `<off\|window\|always>` | Set the IORQ Log recording mode. |
| `--iorqlog-dir` | `<dirpath>` | Target directory for iorqlog export. |
| `--iorqlog-name` | `<basename>` | Basename for iorqlog export files. |
| `--iorqlog-chunk-mb` | `<N>` | RAM chunk size before disk swap (default 64). |
| `--iorqlog-max-total-mb` | `<N>` | Max total recording size in MB, 0 = unlimited (default 2048 = 2 GB). |
| `--iorqlog-save-on-exit` | `<on\|off>` | Auto-finalize iorqlog export when the emulator exits. |
| `--intlog-mode` | `<off\|window\|always>` | Set the Interrupt Log recording mode. |
| `--intlog-dir` | `<dirpath>` | Target directory for intlog export. |
| `--intlog-name` | `<basename>` | Basename for intlog export files. |
| `--intlog-chunk-mb` | `<N>` | RAM chunk size before disk swap (default 64). |
| `--intlog-max-total-mb` | `<N>` | Max total recording size in MB, 0 = unlimited (default 2048 = 2 GB). |
| `--intlog-save-on-exit` | `<on\|off>` | Auto-finalize intlog export when the emulator exits. |
| `--hwlog-mode` | `<off\|window\|always>` | Set the HW Log recording mode. |
| `--hwlog-dir` | `<dirpath>` | Target directory for hwlog export. |
| `--hwlog-name` | `<basename>` | Basename for hwlog export files. |
| `--hwlog-chunk-mb` | `<N>` | RAM chunk size before disk swap (default 64). |
| `--hwlog-max-total-mb` | `<N>` | Max total recording size in MB, 0 = unlimited (default 2048 = 2 GB). |
| `--hwlog-save-on-exit` | `<on\|off>` | Auto-finalize hwlog export when the emulator exits. |
| `--hwlog-hs-decimation` | `<N>` | Decimate hwlog `GDG_VIDEO` HS/HBLN edges: emit every N-th edge. `0` = OFF (default; full rate is ~31000 events/sec). |
| `--all-traces-mode` | `<off\|window\|always>` | Shorthand: set the recording mode for ALL four trace-suite subsystems (cputrack, iorqlog, intlog, hwlog) at once. Per-subsystem `--<sys>-mode` takes precedence. |
| `--all-traces-dir` | `<dirpath>` | Shorthand: set the target directory for ALL four trace-suite subsystems. Per-subsystem `--<sys>-dir` takes precedence. |
| `--no-save-ini` | - | Do not write the `.ini` file at exit. CLI overrides become session-only. |
| `--no-first-run-windows` | - | Suppress automatic opening of About + Version Check Setup windows on first run (when no `.ini` file exists). Useful for headless / scripted launches. |
| `--headless` | - | Run the emulator without a GUI window and without audio output. The SDL3 video and audio subsystems run in no-op mode (no SDL window, no audio device opened). The framebuffer is still rendered into memory (ready for later MCP frame Resources). Intended for CI / batch / subprocess scenarios with no display or audio device available. The process keeps running until SIGINT (Ctrl+C), an SDL quit event or an exit condition of an unattended run (see below). Recommended to combine with `--no-first-run-windows`. |
| `--maxspeed-bench` | - | Start directly in MAX SPEED and periodically (every 5 s) print the MAX SPEED benchmark report to the console (efficiency %, throughput, FB-FPS, distribution). Intended for headless emulation efficiency measurement. Combine with `--headless` and `--run-mzf`. See [`maxspeed-benchmark.md`](maxspeed-benchmark.md). |
| `--speed` | `<percent\|max>` | Set the emulation speed at start: `max` = MAX SPEED (no real-time pacing), an integer 1-4000 = custom speed in percent (100 = normal). Works in the window and with `--headless` (where emulation otherwise runs in real time). Invalid value = error and exit. Combined with `--maxspeed-bench`, MAX SPEED wins. |
| `--mode-switch` | `<700\|800>` | MZ-800 only: position of the rear mode switch (SW1) - `700` = MZ-700 mode, `800` = MZ-800 mode. The ROM reads the switch when it starts a program. Overrides the `[MZ800] mode_switch` value from the `.ini` (same as the Rear DIP Switch menu). Invalid value = error and exit. |
| `--record` | `<file.avi>` | Start video recording right after startup. See [`video-recording.md`](video-recording.md). |
| `--record-frames` | `<count>` | Stop video recording after the given number of frames; with `--headless` the emulator then exits. |
| `--audio-capture` | `<folder>` | Start an audio capture into the folder right after startup (channel levels, PSG writes, reference.wav, sdl.wav; the files are selected in Tools -> Audio Capture -> Settings...). See [`audio-capture.md`](audio-capture.md). |
| `--audio-capture-frames` | `<count>` | Stop the audio capture after the given number of frames; with `--headless` the emulator then exits. |
| `--exit-after-frames` | `<N>` | Unattended run: end after N emulated frames, exit code 0. See [Unattended runs](mcp-server/headless-mode.md#unattended-runs). |
| `--timeout-frames`, `--timeout-seconds` | `<N>`, `<S>` | Unattended run: exit code 2 (timeout) after N frames / S emulated seconds, unless another condition came first. |
| `--exit-on-pc`, `--fail-on-pc` | `<addr>[,skip=N][,code=C]` | Unattended run: end when the CPU executes the address; exit code C (default 0), or 1 for `--fail-on-pc`. Can be repeated. |
| `--exit-on-write`, `--exit-on-out` | `<addr>[=VAL]...`, `<port>...` | Unattended run: end when the program writes to the memory address / I/O port; exit code = the written value, or `code=C`. Can be repeated. |
| `--bpt` | `<file.bpt>` | Unattended run: load breakpoints from the file (the automatically loaded set and variables are not used in an unattended run, nothing is saved at exit). A breakpoint with the action `exit` ends the run. |
| `--result-json`, `--automation-dir` | `<file>`, `<folder>` | Unattended run: write the result (reason, exit code, time, registers, hashes, artifacts) as JSON; `--automation-dir` also saves `screen.png`. |
| `--exit-screenshot`, `--exit-snapshot` | `<file>` | Unattended run: save the final screen as PNG / the final state as a snapshot. |
| `--replay-record` | `<file>` | Record the inputs of the run into a replay file (`.mzr`) from the first frame; the file is written when the emulator exits. See [Replay](replay.md). |
| `--replay-hash-interval` | `<N>` | With `--replay-record`: frames between state hashes (1-100000, default 50). |
| `--replay` | `<file>` | Play a replay file back from its start snapshot; live input is ignored, the playback pauses at the end or at the first differing frame. |
| `--exit-at-replay-end`, `--replay-verify` | - | With `--replay`: end the run at the end of the playback - exit code 0, 4 when the state differs, 3 when the replay cannot start. `--replay-verify` also runs at MAX SPEED. |

### CDL export layout

A CDL export consists of one `meta.json` file plus one binary file per
memory region. With `--cdl-dir ./my-runs/` and `--cdl-name session1`
the produced layout is:

```
./my-runs/
    session1.json                # meta + region table (text)
    session1_bus.cdl             # 64K cells × 16 bytes (R, W, X, S uint32 LE)
    session1_ram.cdl
    session1_rom-lower.cdl
    ...
    session1_iorq-8bit.cdl
```

The `meta.json` contains the absolute filenames of the per-region files,
so the GUI Import action only needs the meta file - it parses the region
table and loads the referenced files from the same directory.

### Persistence

CLI options for the CDL settings (`--cdl-mode`, `--cdl-dir`, `--cdl-name`,
`--cdl-save-on-exit`) override the values loaded from the `.ini` file.
On exit the `.ini` is rewritten with the current (overridden) values, so
the override becomes the new default for next runs.

To test a setting without making it permanent, combine the override with
`--no-save-ini`:

```
mz800emu --cdl-mode always --no-save-ini
```

### Examples

```
mz800emu --help

# Boot with a specific MZF, record CDL while debug window is open,
# export CDL on exit (settings persisted to .ini):
mz800emu --run-mzf "Batman Demo.mzf" --cdl-mode window --cdl-save-on-exit on

# One-shot session: record CDL always, export on exit, do not change .ini:
mz800emu --cdl-mode always --cdl-save-on-exit on --no-save-ini

# Custom export directory + custom basename:
mz800emu --cdl-mode always \
         --cdl-dir ./my-cdl-runs/sapo-p/ \
         --cdl-name run-2026-05-01 \
         --cdl-save-on-exit on
```

For details about the CDL file format and per-file semantics see
[`../cz/debugger/formats/cdl_format.md`](../cz/debugger/formats/cdl_format.md) (Czech).


## trace-suite (CPU Tracking, IORQ, Interrupt, HW Logs)

Four independent sequential logging subsystems for deep analysis of
executed code, I/O traffic, interrupts and HW state changes. Each
subsystem can be activated independently via `--<sys>-mode=<off|window|always>`
where `<sys>` = `cputrack` / `iorqlog` / `intlog` / `hwlog`. Per-subsystem
options follow the same pattern as CDL (`-dir`, `-name`, `-save-on-exit`)
plus chunk/total limits for streaming recording.

Output structure (per recording directory):

```
./trace-suite/
    cputrack.json                    # meta + chunks anchor + initial regs/RAM dump
    cputrack_initial_ram.bin         # 64 KB CPU RAM snapshot
    cputrack_initial_vram.bin        # 32 KB VRAM (MZ-800)
    cputrack_initial_memext.bin      # 512 KB Memext (if attached)
    cputrack.000.bin                 # chunk 0 - 12 B per CPU instruction
    cputrack.001.bin                 # chunk 1
    ...
    iorqlog.json                     # meta + chunks anchor
    iorqlog.000.bin                  # 24 B per I/O event
    intlog.json
    intlog.000.bin                   # 24 B per interrupt event
    hwlog.json
    hwlog.000.bin                    # 24 B per HW state change event
```

Per-subsystem format details:

- [`../cz/debugger/formats/CPU-track_format.md`](../cz/debugger/formats/CPU-track_format.md) - CPU Tracking Log (Czech)
- [`../cz/debugger/formats/IORQ-log_format.md`](../cz/debugger/formats/IORQ-log_format.md) - IORQ Log (Czech)
- [`../cz/debugger/formats/INT-log_format.md`](../cz/debugger/formats/INT-log_format.md) - Interrupt Log (Czech)
- [`../cz/debugger/formats/HW-log_format.md`](../cz/debugger/formats/HW-log_format.md) - HW Log (Czech)
- [`../cz/debugger/Trace_Suite.md`](../cz/debugger/Trace_Suite.md) - User overview, GUI menu, INI persistence (Czech)

Other debugger documents:

- [`debugger/code-coverage.md`](debugger/code-coverage.md) - Code coverage:
  executed code and branch results (Coverage tab in CPU Profiler, marks in
  Disassembly, `--coverage*`, `[COVERAGE]`, lcov via `docs/tools/`)
- [`debugger/bus-map.md`](debugger/bus-map.md) - Bus Map (HW & Devices
  menu): who owns which I/O port and MMIO address (board chips, on the
  MZ-700 also expansion devices),
  output stage, conflicts, undriven-bus model, activity
- [`debugger/ram-write-log.md`](debugger/ram-write-log.md) - RAM Write Log:
  who wrote a byte, write history of a cell, newest writes
  (`--ramlog-*`, `[RAMLOG]`)
- [`debugger/memory-search.md`](debugger/memory-search.md) - Memory Search:
  find a variable (lives, score, timer) by narrowing candidates, then
  Watch, write breakpoint or Freeze (`[MEMSEARCH_WINDOW]`)
- [`debugger/disassembler-window.md`](debugger/disassembler-window.md) -
  standalone Disassembler window V1 (range disasm, S/L/D/W auto-labels,
  sym_db/CDL gating, export .asm/.s for pasmo/sjasmplus/sdcc-asz80)

### Examples

```
# Record CPU instructions during a session, into custom directory:
mz800emu --run-mzf program.mzf --cputrack-mode always \
         --cputrack-dir ./traces/batman/ --cputrack-name run-001 \
         --cputrack-save-on-exit on

# All four subsystems at once:
mz800emu --cputrack-mode always --iorqlog-mode always \
         --intlog-mode always --hwlog-mode always

# Same effect using the shorthand (active + shared directory):
mz800emu --all-traces-mode always --all-traces-dir ./logs/run-1/

# Mix shorthand with one per-subsystem override (everything always
# except hwlog, which stays off):
mz800emu --all-traces-mode always --hwlog-mode off

# Constrain disk usage with chunk size + total cap:
mz800emu --cputrack-mode always \
         --cputrack-chunk-mb 32 --cputrack-max-total-mb 512
```


## Overlay menu

The `F10` key (or `Alt` + left mouse button over the picture, or the
**Overlay Menu...** item of the classic menu) opens a
full-screen menu over the emulator picture; `F10` or `Esc` closes it. Opening
the menu pauses the emulation and closing it restores the state from before.
Six tabs (digits `1` to `6`): Media (floppy disks, cassette, Quick Disk,
recently inserted media), Machine, Display, Sound, Input and Capture. The
classic menu on the right mouse button works as before; over the open
overlay menu the right mouse button closes it and opens the classic menu.
For details see [`overlay-menu.md`](overlay-menu.md).

## Cassette recorder (Virtual CMT)

HW & Devices -> CMT -> Show Virtual CMT (`Alt + C`) opens the virtual
cassette recorder: TMZ, TZX, MZF, MZT, TAP and WAV tapes, playing the real
signal or loading at once through the CMT hack, recording what the computer
saves, saving the tape and programs to files, editing the tape and finding
programs in WAV recordings. For details see [`devices/cmt.md`](devices/cmt.md).

## Screenshots

Tools -> Screenshot -> Save Screenshot (`Alt + F`) saves a PNG of the picture
shown in the window; area (with or without the border) and scale (native dots
or 1x to 4x of the window) are set in Settings... Record Changed Frames
(`Alt + Shift + F`) saves every changed emulated frame, for finding animation
bugs. For details see [`screenshots.md`](screenshots.md).

## Sound

Interface -> Audio... opens the Audio Mixer window with the volume of the
speaker (CTC0) and of the PSG channels. The option **Hardware output filter**
(on by default, `[AUDIO] output_filter` in the INI file) turns on the low-pass
filter a real MZ-800 has between the sound chip and the amplifier (first
order, 346 Hz corner with the volume at maximum): the sound is duller, like
from the computer's own speaker. With the option off you get the previous,
brighter sound of the emulator. It can be switched at any time while running
and it also applies to the audio capture and the video recording.

The emulator makes up for the loss of volume behind the filter, as the
amplifier does on the real computer: by 8 dB with the filter on, by 4 dB with
it off (and in the MZ-700 emulators). A 440 Hz tone is therefore equally loud
in both states; low tones are 4 dB stronger with the filter on, high tones
weaker. The make-up gain does not change the ratios between the sources (the
speaker against the PSG). Peaks above 0.7 of full scale are softly
compressed, so even all sources at full level do not clip the output hard;
below that the output is exactly linear. The make-up gain is a choice of the
emulator, not a measured value: the loudness of the real computer is set by
its volume knob.

The indicators in the Audio Mixer window show the peak: a channel playing at
full level with its volume at 0 dB lights its whole indicator, a constant
level (which is not audible) lights nothing. The vertical indicator on the
right shows the peak of the whole output before the master volume and mute;
lit top LEDs mean that the peaks are being compressed.

The speaker (CTC0, beeps, music
through the 8253 and the tape signal) is AC coupled as on the real computer:
a constant level fades out within about a second. A constant level of the
whole output fades out the same way (MZ-800 and MZ-1500): there is a capacitor
in series behind the sound chip, the time constant of 0.57 s is calculated
from the schematic and oscilloscope measurements agree (0.55 s, range 0.44 to
0.69 s). When the computer is silent the output reaches zero within a few
seconds.

The master listening volume (**Master volume**) and **Mute** are in the
overlay menu on the Sound tab (`F10`, see [`overlay-menu.md`](overlay-menu.md))
and in the Audio Mixer window. The master volume has a range of 0 to 200 %
(100 % by default, marked on the slider; a double-click sets it back): up to
100 % it attenuates, above 100 % it boosts by up to 6 dB and the sound then
goes through a soft limiter, so the peaks are not cut off. The emulation is
paused while the overlay menu is open; to hear the changes at once, turn on the **Listen** switch on the
Sound tab. They are applied only at the sound output of
the emulator, so they do not change the channel volumes and do not get into
the sound of a video or into the audio capture. The master volume is saved
(`[AUDIO] volume_master` in the INI file for 0 to 100 % and
`volume_master_boost` for the per cent above 100 %, written in hexadecimal
in the file, `0x00` to `0x64`); mute is not saved, the emulator
always starts unmuted. While muted, the title of the main window carries the
mark **MUTED**, both while running and while paused.

How well founded the sound output is:

- **MZ-800** - measured on a real computer (levels, filter, speaker coupling).
- **MZ-1500** - derived from the MZ-800, not measured.
- **MZ-700** - estimated from the schematic, without measurements. According to
  the schematic the MZ-700 has no filter in front of the amplifier, so the
  MZ-700 emulators do not have the option Hardware output filter (neither in
  the window nor in the INI file) and the sound uses the previous filters of
  the emulator. On the real computer the speaker is DC coupled; the emulator
  still lets a constant speaker level fade out, because a sound card cannot
  reproduce it and steps would click (there is no series capacitor as in the
  MZ-800). The speaker volume is the same as for an MZ-800 in
  MZ-700 mode. An MZ-800 in MZ-700 mode uses the sound path of the MZ-800.

## Audio capture

Tools -> Audio Capture -> Record Audio Capture (`Alt + Q`) records into a new
folder what the sound hardware does: changes of the channel levels, PSG
writes, the exact sound and the sound you hear (two WAV files to compare) -
for finding sound bugs. Also from the command line (`--audio-capture`) and
over MCP. For details see [`audio-capture.md`](audio-capture.md).

## Replay

Replay -> Record Inputs... records the inputs of a run (keyboard, joystick,
resets, debugger writes) from a start snapshot; Replay -> Play Replay...
repeats exactly the same run, also in a new process and at any speed, so a
bug can be studied again and again with other breakpoints or trace, and
stops at the first frame where the state differs. `--replay-verify` checks
a recorded run in CI. For details see [`replay.md`](replay.md).

## Expansion devices (MZ-700)

The MZ-700 emulator can connect optional expansion units of the real MZ-700
from the menu **HW & Devices** (each unit has its own submenu with the
options and **Settings...**, a window with all options of the unit);
by default none is connected.

| Unit | Description |
|------|-------------|
| PCG-700 (HAL) | programmable character generator, 2 KB RAM for the characters 80h-FFh, see [devices/pcg700.md](devices/pcg700.md) |
| SUC 80-column kit | 80 columns x 25 rows from the existing VRAM (kit of the Sharp Users Club UK), see [devices/suc80.md](devices/suc80.md) |
| K&P 80 Column Card | 80-column card of Kersten & Partner with its own video RAM and an output for a second monitor (window K&P 80 Column Monitor), see [devices/kp80.md](devices/kp80.md) |

The expansion devices share the Z80 data bus as on the real machine: each
one declares how it drives the bus (write only, tri-state driver, some
bits), a write reaches every device decoding the address and a read is
composed from the answers of all of them. Bits nobody drives return the
undriven-bus model (by default the last byte read from memory; optionally
a constant, INI section `[DEVBUS]`, keys `bus_idle`, `bus_idle_value`) -
not measured on real hardware. Two drivers on the same bits are a
conflict; the default policy does not connect the second device
(`conflict = refuse`), the policies `and` and `first` are models for
experiments and report every conflicting read. The debugger window Bus
Map shows who owns which address ([debugger/bus-map.md](debugger/bus-map.md)).

## MAX SPEED benchmark

Measuring emulation efficiency in MAX SPEED mode - how much "real MZ-800 time"
gets executed per one real second (100 % = real hardware speed). Open the window
from the **Emulator -> MAX SPEED Benchmark...** menu; the `Alt + T` key prints the
report to the console and `Alt + Shift + T` resets the measurement. For headless
measurement use the `--maxspeed-bench` option.

For details see [`maxspeed-benchmark.md`](maxspeed-benchmark.md).


## Known issues

### ImGui

If you run into problems with the application remembering the wrong size or position of windows, you can edit or delete the `mz800emu-imgui.ini` file.
