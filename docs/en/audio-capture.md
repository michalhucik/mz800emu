# Audio capture

Records what the sound hardware of the emulated computer does into a folder,
in emulated time - for finding sound bugs of a program or of the emulator.
All platforms (MZ-800, MZ-700 PAL and NTSC, MZ-1500); the MZ-700 has only the
speaker.

## Starting and stopping

| Way | How |
|-----|-----|
| Menu | **Tools -> Audio Capture -> Record Audio Capture** (`Alt + Q`) starts the capture; the same item or key stops it. |
| Command line | `--audio-capture <folder>` starts it right after startup, `--audio-capture-frames <count>` stops it after the given number of frames; with `--headless` the emulator then exits. |
| MCP | `emu_audio_capture_start`, `emu_audio_capture_stop`, `emu_audio_capture_status`; reading the capture memory `emu_trace_read` (`audio_*`), `emu_audio_capture_summary`, `emu_audio_capture_pcm` (see [`mcp-server/tools-overview.md`](mcp-server/tools-overview.md) and below). |

From the menu each capture creates a new folder `<platform>_audio_YYYYMMDD_HHMMSS`
in the output folder (default: `audio_captures` next to the emulator; MCP
without `dir` uses it too). While it runs, the top left corner of the picture
shows **AUDIO** and the captured time; when it ends, a notification shows the
folder. **Open Output Folder** opens the output folder in the file manager.

**Tools -> Audio Capture -> Audio Capture Control...** (`Alt + Shift + Q`)
opens a floating window that can stay open while you play: the state
(capturing, starting, off), the captured time, the folder of the capture,
the number of level changes and PSG writes, the result of the last
capture, the files that will be written and the output folder. Its
buttons start or stop the capture (`Alt + Q`), open the output folder and
the settings. The window opens again after a restart of the emulator if
it was open.

The capture starts at the end of the next frame; when `sdl.wav` is written,
after six more frames (about 0.1 s) that let the audio output settle. It
ends with stop, after the given number of frames, after one hour, or when
the emulated time starts again (reset, loading a snapshot).

## Settings

**Tools -> Audio Capture -> Settings...** sets the output folder and selects
the files to write; saved in the configuration (section `[AUDIO_CAPTURE]`).
The command line uses the same file selection.

| Setting | INI key | Default |
|---------|---------|---------|
| Output folder | `output_dir` | empty = `audio_captures` next to the emulator |

| File | INI key | Content (all on by default) |
|------|---------|---------|
| `events.csv` | `events` | every change of the output level of a sound channel |
| `psg_writes.csv` | `psg_writes` | every write to the PSG, decoded |
| `reference.wav` | `reference_wav` | the sound computed exactly in emulated time |
| `sdl.wav` | `sdl_wav` | the sound of the audio output - what you hear |

`capture.json` with the description of the capture is always written.

## Files

Times in the CSV files: `ticks` = clock ticks of the video chip since the
start of the capture (the frequency is `gdg_clk_hz` in capture.json),
`time_us` = the same in microseconds, `frame` = the frame since the start
(20 ms PAL, 16.7 ms NTSC).

**events.csv** - `ticks,time_us,frame,channel,level`. Channels: `ctc0` (the
speaker, level 0, 15 or 31), `psg0_0` .. `psg0_3` (PSG, channel 3 = noise) and on
the MZ-1500 or an MZ-800 with the second PSG `psg1_0` .. `psg1_3`. Level 0-15
is the output of the channel (for the PSG 15 minus the attenuation while the
generator output is high, otherwise 0). Level 16-30 belongs to a PSG tone
channel with divider 1: a constant level of 0.49 of the full swing of step
`level - 15` (on the real chip the output toggles at 110.8 kHz, only the mean
level is audible; used to play samples through the volume register). Level 31
belongs only to the `ctc0` channel while counter CTC0 runs in mode 3 with
divider 2 and bit PC0 is 1: the 554 kHz square wave is not written edge by
edge, the channel holds a constant level of 0.64 of level 15 (measured on a
real MZ-800); with PC0 = 0 the level is 0. In the MZ-700 emulators level 31
means half of level 15 (0.50, derived from the schematic) and bit PC0 has no
effect on it. The first rows (`ticks` 0) are the
levels at the start.

**psg_writes.csv** - `ticks,time_us,frame,psg,byte,channel,reg,value`:
`psg` = target (1 = PSG0, 2 = PSG1, 3 = both), `byte` = written value (hex),
`channel` 0-3, `reg` and `value` from the state of the PSG after the write:

| reg | value |
|-----|-------|
| `tone_lo` | latch byte of a tone: the low 4 bits of the divider (the divider changes only with the next data byte) |
| `tone` | the complete 10-bit divider |
| `volume` | attenuation 0-15 (15 = off) |
| `noise` | `(type << 2) \| rate` |

**Levels in reference.wav and sdl.wav** - both files hold the output behind
the make-up gain and the soft limiter: the sum of the sources is multiplied
by 2.51 (+8 dB) with the output filter on and by 1.58 (+4 dB) with it off and
in the MZ-700 emulators. Up to a swing of 0.7 of full scale (22 937 of
32 767) the output is exactly linear, so the ratios between the sources hold
unchanged; larger peaks are smoothly compressed towards full scale and are
not clipped. One PSG channel at attenuation 0 has a settled peak-to-peak
swing of 0.25 times the multiplier, the speaker (CTC0) 1.49 times as much.
Compared with version 2.0.7 the levels are 8 dB (filter on) or 4 dB (filter
off) higher. The master volume and mute are not in the files.

**reference.wav** and **sdl.wav** - 44100 Hz, 16 bit, stereo, the same
format, so they can be compared directly. reference.wav is computed exactly
in emulated time; sdl.wav is what the emulator's audio output produced. Their
difference shows defects of the audio output. sdl.wav comes later: without
an audio device (`--headless`) by exactly one frame, with a device by its
buffering. With an audio device at MAX SPEED the audio output skips frames -
capture.json then has `sdl_blocks` lower than `frames`.

**capture.json** - platform, `gdg_clk_hz`, `frames_per_sec`, `wav_rate`,
channel names, counts (`frames`, `events`, `psg_writes`, `reference_samples`,
`sdl_blocks`, `sdl_samples`) and `stop_reason`. Further, times in GDG clock
ticks since the emulation started (the same timeline as `total_screens` *
`screen_total_ticks_per_frame` + `total_ticks` in MCP `emulator://periph/gdg`):

- `began` - the capture really started (`false` when it was stopped while
  still waiting for the end of a frame or for the audio output warm-up),
- `origin_ticks` - the start of the capture = `ticks` 0 in the CSV files and
  the first sample of reference.wav,
- `end_ticks` - the end of the last completed frame = the end of
  reference.wav,
- `last_event_ticks` - the time of the last CSV row; after a manual stop
  between frames it can lie after `end_ticks` (reference.wav has no sound
  for those events),

(without a start or without CSV rows the values are `null`) and `clocks` -
the platform clocks with the same names as in MCP `emulator://platform/info`:
`gdg_base_hz` (the rounded frequency the emulated time runs at),
`gdg_real_base_hz` (the real crystal), `cpu_divider`, `ctc0_divider`,
`psg_divider` (`null` without a PSG) and `screen_total_ticks_per_frame`.

## Reading the capture via MCP (memory)

A capture started via MCP also keeps its data in memory (`memory`, on by
default for MCP, off from the menu and the command line). An MCP client
reads it while the capture runs and after it ends, until the next capture
starts - without access to the files, so also over a network connection.
A capture into memory only (all four files off) creates no folder.

| What | Tool |
|------|------|
| PSG writes, level changes (the CSV rows) | `emu_trace_read` channels `audio_psg_writes`, `audio_levels` |
| one summary row per frame (level changes, time with level > 0 and RMS level per channel, RMS and peak of the reference and the audio output) | `emu_trace_read` channel `audio_frames` |
| what happened: silent and sound spans, notes played by the PSG, does the speaker play a tone or 1-bit PCM (an estimate), the delay and difference of the audio output against the reference | `emu_audio_capture_summary` |
| a short PCM window as WAV (resampled preview, or the exact samples of reference.wav / sdl.wav at 44100 Hz stereo) | `emu_audio_capture_pcm` |

Memory: level changes 1M records (16 MiB), PSG writes 256k (6 MiB), frame
summaries for 10 minutes, PCM of the reference for 60 s (10.6 MB per
minute; the audio output with `pcm` `sdl`). When full, the oldest records
are dropped. The PSG notes and the pitch are computed with the emulator's
PSG formula (the same as the PSG Audio Scope of the debugger).

## Example

```
mz800emu --headless --run-mzf game.mzf --audio-capture C:/tmp/snd --audio-capture-frames 500
```

Captures 10 seconds of the game (PAL) into `C:/tmp/snd` and exits.
