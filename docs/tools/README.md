# docs/tools/

Offline helper tools shipped with the emulator (also copied into the
distribution under `docs/tools/`).

- `example_psg_write_log_to_midi.py` - example converter for the PSG Audio
  Scope debugger window (see below).
- `videorec_export.py` - exports a video recording (AVI parts +
  `*.cuts.json` sidecar) to a YouTube-ready MP4 (see below).

## example_psg_write_log_to_midi.py

Example offline converter that turns a PSG write hook log (TSV) into a
Standard MIDI File. The script replicates 1:1 the PSG state machine from
`src/emulator/hw-generic/psg/psg.c` (`psg_write_byte_core`) and the
note-event detector from
`src/ui-imgui/debugger/psg_audio_scope_window.cpp`, so its output should
match the MIDI exported directly from the in-emulator scope window.

The script is provided as an EXAMPLE. It demonstrates one possible
projection of raw PSG writes into MIDI events; it is not a production
ready tool. Fork it and adapt it for downstream post-processing as
needed.

### Requirements

- Python 3.10+
- [mido](https://mido.readthedocs.io/) package

Install in MSYS2:

```bash
/c/msys64/mingw64/bin/python.exe -m pip install --user \
    --break-system-packages mido
```

### Usage

The input TSV is produced by toggling **Log writes** in the PSG Audio
Scope window. The default output path is the input path with the `.mid`
suffix.

```bash
/c/msys64/mingw64/bin/python.exe \
    docs/tools/example_psg_write_log_to_midi.py \
    psg_writes_20260523_112205.tsv \
    -o /tmp/batman_offline.mid
```

CLI options:

- `-o, --output <path>` - output `.mid` path (default: input with `.mid`
  suffix)
- `--tempo <bpm>` - MIDI tempo, default 120
- `--ppqn <int>` - pulses per quarter note, default 480

### Input format

TSV file with a metadata header followed by tab-separated data rows. The
header is emitted by the emulator and looks like:

```
# PSG write log
# emulator_clock_hz=<pxCLK Hz>
# stereo=<0|1>
# columns: pxclk_ticks  channel_mask  raw_byte_hex
```

Data row columns:

| column         | meaning                                          |
|----------------|--------------------------------------------------|
| pxclk_ticks    | monotonic emulator pxCLK timestamp               |
| channel_mask   | 0x01 = PSG0, 0x02 = PSG1, 0x03 = both            |
| raw_byte_hex   | raw byte (hex, no prefix) as written by the CPU  |

`GDGCLK2CPU_DIVIDER` is derived from `emulator_clock_hz`:

| clock_hz       | platform              | divider |
|----------------|-----------------------|---------|
| >= 16 000 000  | MZ-800 (PAL/NTSC)     | 5       |
| <  16 000 000  | MZ-1500 / MZ-700      | 4       |

Tone frequency: `clock_hz / (32 * tone_divider * GDGCLK2CPU_DIVIDER)`.

### Output

- Standard MIDI File type 1, PPQN 480, default 120 BPM
- Track 0 = conductor (set_tempo + time_signature 4/4)
- Tracks 1..N = one track per used (chip, channel) pair
  - TONE channels: Program Change 80 (Lead 1 - Square Wave)
  - NOISE channels: Program Change 119 (Reverse Cymbal), routed to MIDI
    drum channel 9, fixed pitch 60 as a surrogate
- MIDI channel mapping:
  - chip 0 (mono / left) -> MIDI channels 0..3
  - chip 1 (right) -> MIDI channels 4..6 (NOISE always on channel 9)

### Limitations

- Raw 1:1 projection per PSG channel. No chord coalescing, no arpeggio
  detection. For musical post-processing (track merging, quantization,
  chord recognition) feed the output into an external DAW or MIDI tool.
- Velocity is taken from `attn` only at note_on. Attenuator changes
  during a held note (volume envelope) do NOT produce expression CC
  events. This matches the in-emulator MIDI export.
- Pitch is taken from the tone divider at note_on. A divider change
  during a held note is converted into note_off + note_on (split note),
  not pitch bend.
- The script is an example. If you need production grade output, fork
  it.

## videorec_export.py

Turns a recording made by Tools -> Video Recording (lossless AVI parts and
the `<name>.cuts.json` sidecar with segments and markers) into an MP4
(H.264 High, yuv420p, AAC 320k 48 kHz) plus a YouTube chapters file
`<output without extension>.chapters.txt`. Crop, integer upscaling, aspect
ratio and transitions (cut, fade, crossfade, title card) are selectable;
`--state-overlay icons` burns emulator state indicators (pause, speed,
real time) from the sidecar events into the picture.
Code comments in this script are in English (an explicit exception from
the project rule of Czech comments).

### Requirements

- Python 3 (standard library only)
- [ffmpeg](https://ffmpeg.org/) in `PATH`, or passed via `--ffmpeg PATH` or
  the `VIDEOREC_FFMPEG` environment variable (the `card` transition with
  text and the text indicators of `--state-overlay icons` also need the
  `drawtext` filter and a font passed by `--font`)

ffmpeg can be downloaded from <https://ffmpeg.org/download.html> (on
Windows e.g. a static build, or `pacman -S mingw-w64-ucrt-x86_64-ffmpeg` in
MSYS2). It must be on `PATH` or passed via `--ffmpeg` (or `VIDEOREC_FFMPEG`).

### Usage

Run from the distribution root (the folder that contains `docs/`):

```bash
python3 docs/tools/videorec_export.py recording.cuts.json -o video.mp4 --target 1080p
```

On Windows without a `python3` command use `python` or the launcher
`py -3` instead, e.g. `py -3 docs\tools\videorec_export.py ...`.

Run `python3 docs/tools/videorec_export.py --help` for all options
(`--target`, `--crop`, `--aspect`, `--transition`, `--transition-ms`,
`--card-text`, `--font`, `--crf`, `--state-overlay`, `--dry-run`). Full description:
`docs/en/video-recording.md`.
