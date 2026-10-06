# Video recording

Lossless recording of the picture and sound of the emulated computer (e.g. for a
YouTube video), simple cuts while playing, and export of the finished video with
transitions. Recording works on all platforms: MZ-800, MZ-700 (PAL and NTSC)
and MZ-1500.

## What it is for

The emulator writes every emulated video frame (the whole picture including the
border) together with its matching sound into an AVI file without any loss of
quality. The recording is "clean": it contains no emulator window, menu or
notifications, only the picture and sound of the emulated computer.

Every line of the picture is written twice into the AVI, so the AVI has the same
aspect ratio as the emulator window (a pixel is shown twice as tall as it is
wide). Colours and pixels are unchanged, lines are only repeated. Sizes and
frame rate per platform:

| Platform | Picture (with border) | AVI video | Picture area (canvas) | Frames per second | Sound |
|----------|-----------------------|-----------|-----------------------|-------------------|-------|
| MZ-800 | 928x288 | 928x576 | 640x200 | 50 | CTC and PSG; stereo with the second PSG (see below) |
| MZ-700 PAL | 704x232 | 704x464 | 640x200 | 50 | CTC only (both channels the same) |
| MZ-700 NTSC | 704x232 | 704x464 | 640x200 | 60 | CTC only (both channels the same) |
| MZ-1500 | 704x232 | 704x464 | 640x200 | 60 | stereo: left CTC and the first PSG, right CTC and the second PSG |

On the MZ-800 the recording is stereo when the second PSG is enabled
(called "PSG1" in the menu: **Devices -> HW Compatibility Experiments -> Allow PSG1 (stereo)**); without
it both channels are the same. The recording has the same left/right sound
layout the emulator plays.

The recording is meant as a **master**. The `videorec_export.py` script turns it
into an MP4 for YouTube (crop, scaling, transitions, chapters), or you can process
it in an external video editor.

The controls are deliberately simple: start, stop, recording pause, marker,
"retake" via snapshot and a switch between emulated time and real time. Everything that belongs in an editor (titles over the
picture, music, commentary, zoom) stays in an external program.

## Controls

Menu **Tools -> Video Recording**:

| Menu item          | Shortcut         | Meaning |
|--------------------|------------------|---------|
| Start Recording / Stop Recording | `Alt + O` | Starts recording, or stops it and saves the files. |
| Pause Recording    | `Alt + Shift + O` | Pauses / resumes writing to the file. Emulation keeps running. |
| Add Marker         | `Alt + L`        | Inserts a "Marker N" at the current point of the recording. |
| Record in Real Time | `Alt + U`       | Switches the time base: checked = real time, unchecked = emulated time (see [Time base](#time-base-emulated-time-and-real-time)). Works at any time, also while recording. |
| Settings...        | -                | Recording settings (see below). |
| Open Output Folder | -                | Opens the recordings folder in the file manager. |
| Remote Control... | `Alt + Shift + L` | Shows / hides the recording remote control window (see below). |

While recording, an **REC** indicator with the recording time and the current
time base is shown in the top right corner of the emulator picture, e.g.
`REC 00:12:34 · real-time` or `REC 00:01:05 · emulated` (**PAUSE** while the
recording is paused). The indicator is not written into the video. The emulator announces start, save,
failure, retake and seam with a notification.

If you start recording while emulation is paused (`Alt + P`), recording begins
when emulation is resumed.

Recording can also be controlled by an AI client through the MCP server
(tools `emu_videorec_start`, `emu_videorec_stop`, `emu_videorec_pause`,
`emu_videorec_marker`, `emu_videorec_status`, and `emu_videorec_timebase` for
switching between emulated time and real time), see
[MCP tools overview](mcp-server/tools-overview.md).

### Remote control window

The floating **Recording Remote Control** window (menu **Remote Control...**
or `Alt + Shift + L`) keeps all recording controls together. Each button has
its keyboard shortcut written next to it, so that next time you can trigger
the action straight from the keyboard:

- **Start Recording / Stop Recording** (`Alt + O`), **Pause Recording /
  Resume Recording** (`Alt + Shift + O`), **Add Marker** (`Alt + L`).
  The field below the buttons takes an optional marker label (Enter inserts
  the marker right away); an empty field = "Marker N".
- Recording state (IDLE / STARTING / REC / PAUSED in colour), recording time,
  number of video frames, file size (with the number of files when the AVI is
  split into parts), current segment number, the time base and the last event
  (save, retake, seam, time base switch, error). The size reflects data the
  writer thread has actually written.
- **Time base** row: without recording it shows the setting for the next start.
  While recording it shows the time base really in use; in real time also what
  is being written: *live*, *frozen picture* (emulation paused, see the pause
  setting) or *not writing* (pause skipped, recording paused). *Emulated time
  (real time requested)* appears for a moment while switching, and for the
  whole time the emulation runs at a speed other than normal with "Switch to
  emulated time" selected.
- **Time base** switch *Emulated time* / *Real time* with the shortcut
  `Alt + U` written next to it. It applies immediately, also during recording
  (the switch starts a new segment); without recording it applies to the next
  start.
- Quick switch of the **Retake mode** - applies from the next recording start
  (a running recording took the mode when it started; if it differs from
  the selected one, the window shows it).
- **Open Output Folder** and **Settings...** buttons.

The emulator remembers whether the window is open across restarts.

Command line (see [`README.md`](README.md)):

| Option | Meaning |
|--------|---------|
| `--record <file.avi>` | Start recording right after startup into the given file. |
| `--record-frames <count>` | Stop recording after the given number of frames; with `--headless` the emulator then exits. |

## Time base: emulated time and real time

The recording has two time bases. You can switch between them at any time
(`Alt + U`, menu **Record in Real Time**, the remote control window, the
Settings dialog or the MCP tool `emu_videorec_timebase`), also in the middle of
a recording. Every switch is a segment boundary (a cut with the default
transition, see `cuts.json`). The AVI format is the same in both time bases
(the frame rate and size of the platform, the same sound sample rate).

### Emulated time (default)

One emulated frame is one video frame (50 frames per second, 60 on the MZ-700
NTSC and MZ-1500), with sound at 48,000 Hz (or 44,100 Hz, see settings) and the
matching number of samples per frame. The result is a perfect run of the game regardless of how fast the
emulator really ran.

The recorded sound goes through the same filters as the sound the emulator plays
(softened sharp edges and a gradual fade of a level that does not change for
longer than about 45 ms), so it sounds the same as the emulator output. Silence
is a zero level in the recording.

- **Emulation pause** (`Alt + P`) pauses the video as well. No frames are produced
  during the pause.
- **Speed-up and MAX SPEED do not show in the video.** With accelerated emulation
  every frame is written and the resulting video plays at normal speed. In MAX SPEED
  the emulation may be slowed down when writing to the file cannot keep up; nothing
  is lost, though.
- **Recording pause** (`Alt + Shift + O`) is different from emulation pause:
  emulation keeps running, but frames are not written. When you resume, a **cut**
  (segment boundary, see below) appears in the video. This lets you leave out a
  boring or spoiling part of the game.

### Real time

The recording follows the clock on the wall: 50 times per second (60 times on
the MZ-700 NTSC and MZ-1500) it takes the picture that was last shown on the
screen, and the sound that went to the speakers. It is meant for tutorials and live sessions, where the viewer should
see what you saw - including pauses, speed-up and work in the debugger.

- **Emulation pause** is recorded according to the **Emulation paused** setting:
  *Skip* (default; nothing is written during the pause, the video continues
  seamlessly after it), *Frozen picture and silence* (the whole pause is in the
  video), or *Frozen picture, at most the limit below* (the frozen picture lasts
  at most the given number of seconds, 1 to 60, default 3; the rest of the
  pause is skipped).
- **Speed other than normal**: *Record as seen* (default; the video shows the
  emulation sped up or slowed down, exactly as on the screen) or *Switch to
  emulated time* (while the speed is not 100 % the recording automatically uses
  emulated time; when the speed returns to 100 %, it returns to real time - both
  are segment boundaries).
- **Sound when faster than normal** (with *Record as seen*): *As heard*
  (default), *Silence*, or *Attenuated* (quieter by 12 dB).
- **Debugger**: stepping, step over, run to cursor and a stop at a breakpoint
  are recorded as a frozen picture only when **Record debugger steps** is
  checked; by default they are left out of the recording.
- **Loading a snapshot** in real time always creates a seam (a new segment with
  the default transition); there is no retake in real time. A snapshot saved
  while recording in real time never rewinds the recording, even after you
  switch back to emulated time. A snapshot saved earlier in an emulated-time
  part of the same recording can still be used for a retake.
- **Recording pause** (`Alt + Shift + O`) works the same as in emulated time.

The emulator also writes **state marks** into `cuts.json` (in both time bases,
setting **Save state marks for export**): time base switches, speed changes,
emulation pauses, snapshot loads and resets. The export script can burn them
into the video as icons (see [Export to MP4](#export-to-mp4)). With **Automatic
markers** (default on) the emulator also inserts markers - and so chapters of
the exported video: "Reset" in both time bases, "Speed 400%", "Speed MAX",
"Snapshot loaded" and "Pause" only in parts recorded in real time ("Pause"
also only for a pause you make yourself). In emulated time the video plays at
normal speed, a retake continues without a seam and a pause does not show, so
a speed change, a snapshot load or a pause gets no marker - it stays only as
a state mark in `cuts.json`. If you do not want these chapters, uncheck
**Automatic markers** or delete them from `cuts.json`.

Measured properties of real time (two test programs, one to three measurements per
variant; 60 frames per second in the window was measured only on the MZ-1500,
the MZ-700 NTSC only without a window, so take them as an indication, not a
guarantee):

- Sound and picture are aligned to within one video frame: the mean offset of
  the sound against the picture was from -15 ms (sound early) to +12 ms (sound
  late) at both 50 and 60 frames per second; individual points differ by up to
  one video frame.
- After each emulation pause the sound starts again with about 60 to 80 ms of
  silence (about 4 video frames; measured on the MZ-800).
- The recording contains exactly the sound that went to the speakers. It can
  therefore also contain a property of the emulator sound output: a PSG tone
  sometimes "freezes" at a constant level for about one frame (measured on the
  MZ-1500, twice in 5 s in the test; all platforms with a PSG use the same sound
  output code, but it was not measured on the MZ-800). A recording in emulated
  time does not have it.

How it works (design, not a measurement): to stay aligned with the picture over
a long recording, the sound tempo is adjusted continuously; the adjustment is
limited to at most 0.5 % by design. A pause shorter than one video frame
(20 ms, 16.7 ms at 60 frames per second) does not show in the video.

## Retake via snapshot

If you save a snapshot during recording (e.g. quick save `Alt + F8`) and load it
later (e.g. `Alt + F9`) because a section did not go well, the result depends on
the **Retake mode** setting (this applies to emulated time; in real time
loading a snapshot always creates a seam, see [Real time](#real-time)):

| Mode | Behaviour |
|------|-----------|
| **Discard frames (seamless)** (default) | If the snapshot was made during the same recording, the recording **rewinds** to the point where the snapshot was saved and everything between saving and loading is discarded. The video shows a perfect run with no visible seam. Notification: "Retake: rewound to TIME". |
| **Keep as cut with transition** | Nothing is discarded. A segment boundary (seam) with the default transition appears at the point of loading. |
| **Off** | Retake is not used. Loading a snapshot creates a segment boundary (seam) with the default transition. |

A snapshot that **was not made during the same recording** (saved earlier or
during another recording) cannot be rewound to. Neither can a snapshot whose
point lies in a part of the recording that an earlier retake has already
discarded (e.g. you saved A, later B, went back to A and played on - B now
belongs to a discarded attempt), nor one saved before the start of the current
AVI file (see Files), nor one saved right after loading another snapshot
before the emulation continued (e.g. load and save while paused - the loaded
state has not entered the recording yet). In these cases a seam with the
default transition appears in every mode.

The sound continues smoothly at the retake point, without a click: the recording
continues with the sound state from the point where the snapshot was saved.
This applies to snapshots saved during the same recording (the emulator
remembers the last 64 of them).

Note: the sound after loading a snapshot may differ slightly from an
uninterrupted recording (phase of the PSG sound chip after loading a snapshot
[unverified, hypothesis]).

## Settings

Menu **Tools -> Video Recording -> Settings...** (stored in the `[VIDEOREC]`
section of the emulator configuration file; changes apply from the next
recording, except the time base, which switches immediately when you change it
and press OK).
Numbers in the configuration file are **hexadecimal**, the way the emulator writes
them (`transition_ms = 0x1f4` means 500 ms); a value without the `0x` prefix is
read as hexadecimal too (`transition_ms = 500` would mean 1280 ms). Preferably
change the values in the Settings dialog.

| Setting | INI key | Meaning |
|---------|---------|---------|
| Output folder | `output_dir` | Recordings folder. Empty = the `videos` subfolder in the emulator home directory. |
| Audio sample rate | `audio_rate` | 48,000 Hz (default, in INI `0xbb80`) or 44,100 Hz (`0xac44`). |
| Retake mode | `retake_mode` | `0x00` = Off, `0x01` = Discard frames (default), `0x02` = Keep as cut. |
| Default transition | `default_transition` | Default transition at segment boundaries: `cut`, `fade` (default), `crossfade`, `card`. |
| Transition length [ms] | `transition_ms` | Transition length in ms (0 to 5000, default 500 = `0x1f4`). |
| - | `keyframe_interval` | Keyframe interval in frames (1 to 3000, default 250 = `0xfa`). INI file only. |
| Time base | `timebase` | `emulated` (default) or `realtime`. Also switched by `Alt + U`. |
| Emulation paused (real time) | `realtime_pause` | `skip` (default), `freeze` (frozen picture and silence), `freeze_capped` (frozen picture, at most the limit). |
| Frozen pause limit [s] | `realtime_pause_cap_s` | 1 to 60 s, default 3 (`0x03`); used with `freeze_capped`. |
| Speed other than normal (real time) | `realtime_speed` | `as_seen` (default) or `emulated_when_fast` (switch to emulated time). |
| Sound when faster than normal (real time) | `realtime_turbo_audio` | `as_heard` (default), `silence`, `attenuate` (-12 dB). Used with `as_seen`. |
| Record debugger steps (real time) | `record_debugger_steps` | `0` = leave debugger stepping out (default), `1` = record it as a frozen picture. |
| Save state marks for export | `state_marks` | `sidecar` (default; state marks into `cuts.json`) or `none`. |
| Automatic markers | `auto_markers` | `1` = on (default), `0` = off. |

### Presets

The **Preset** buttons in the Settings dialog set several values at once; you
can then change any of them individually. The dialog shows which preset the
current settings match (or "custom").

| Preset | Sets | Intended for |
|--------|------|--------------|
| **Gameplay showcase** | time base `emulated`, pause `skip`, state marks `sidecar` | A clean run of a game: the video plays at normal speed, pauses are not in it. |
| **Live / tutorial** | time base `realtime`, pause `freeze_capped`, state marks `sidecar` | A tutorial or live session: the video shows what you saw, pauses briefly as a frozen picture. The recommended export uses `--state-overlay icons`, so that the viewer sees when the emulation was paused, sped up or recorded in real time. The preset only sets up the recording - the export script cannot tell which preset was used (the default is `--state-overlay none`), so pass the option yourself when exporting. |

## Files

A recording `mz800_YYYYMMDD_HHMMSS` (the name starts with the platform - `mz800`,
`mz700` or `mz1500` - followed by the start date and time; a number is appended
on a collision) consists of:

| File | Content |
|------|---------|
| `mz800_YYYYMMDD_HHMMSS.avi` | Picture (928x576 on the MZ-800, 704x464 on the MZ-700 and MZ-1500; lossless ZMBV codec) and sound (PCM 16-bit stereo). |
| `mz800_YYYYMMDD_HHMMSS_002.avi`, ... | Further parts: a single AVI file is limited to about 1.75 GiB; when it is full the recording continues without a gap in the next file. |
| `mz800_YYYYMMDD_HHMMSS.cuts.json` | Description of segments, markers and parts. Written when the recording stops. |

All files of a recording must stay together in the same folder (the sidecar
refers to the AVI files by name).

Size: in a test with a game title screen (Bloxorz, MZ-800) 1500 frames = 30 s
gave a 7.9 MB file, i.e. about 15.8 MB per minute of recording. Most of it is
uncompressed sound - a calculation, not a measurement: 48,000 samples/s x 4 B
(16-bit stereo) x 30 s = 5.76 MB, i.e. about 73 % of the file. A game with a
quickly changing picture may produce a larger file [unverified].

## Editing `cuts.json` by hand

`.cuts.json` is a text (JSON) file and can be edited in a text editor:

```json
{ "version": 4, "platform": "mz800", "tv_system": "pal",
  "width": 928, "height": 576, "line_doubled": true,
  "framebuffer_width": 928, "framebuffer_height": 288,
  "canvas": { "x": 154, "y": 46, "width": 640, "height": 200 },
  "fps_num": 50, "fps_den": 1,
  "audio_rate": 48000, "default_transition": "fade", "transition_ms": 500,
  "parts":    [ { "file": "x.avi", "first_frame": 0 } ],
  "segments": [ { "start": 0, "end": 1500, "transition_in": "none" },
                { "start": 1500, "end": 4200, "transition_in": "fade" } ],
  "markers":  [ { "frame": 250, "label": "Marker 1" } ],
  "events":   [ { "frame": 0, "kind": "timebase", "value": "emulated" },
                { "frame": 0, "kind": "speed", "value": "100" } ] }
```

- `segments`: parts of the recording (frame numbers from 0; `fps_num` frames =
  1 s, i.e. 50 or 60).
  `transition_in` is the transition at the **start** of the segment: `none` (first
  segment only), `cut`, `fade`, `crossfade` or `card`. Change just the transition
  at an individual boundary if the default does not suit you.
- `transition_ms`: length of the transitions in ms.
- `markers`: markers. They become chapters on export; `label` is the chapter name
  and you can rewrite it (e.g. "Level 2").
- `events`: record of the emulator state during the recording (timebase, pause,
  speed, snapshot load, reset) - input for an export with state indicators.
  You do not need to edit it.
- `width`, `height`, `line_doubled`: size of the AVI video and a flag saying that
  every picture line is in the AVI twice. Do not change them.
- `platform`, `tv_system`, `framebuffer_width`, `framebuffer_height`, `canvas`,
  `fps_num`, `fps_den`: platform (`mz700`, `mz800`, `mz1500`), TV standard
  (`pal`, `ntsc`), size of the emulator picture, the picture area inside it (for
  the `canvas` crop) and the frame rate. Do not change them.
- Recordings from an older emulator version have `"version": 1` (928x288
  without doubled lines), `"version": 2` (no `events`) or `"version": 3` (no
  platform description); they are always MZ-800 recordings. The export script
  handles all versions and the resulting MP4 looks the same.

## Export to MP4

Export is done by the `videorec_export.py` script (part of the distribution and
of the emulator source repository, folder `docs/tools`). It needs **Python 3**
and **ffmpeg** (in `PATH`, or given by `--ffmpeg` or the `VIDEOREC_FFMPEG`
environment variable). The `card` transition with text and the text indicators
of `--state-overlay icons` additionally need an ffmpeg with the `drawtext`
filter.

```
python3 videorec_export.py recording.cuts.json -o video.mp4
```

The output is H.264 (High, yuv420p, BT.709), AAC 48 kHz, at the frame rate of
the recording (50 frames/s, 60 frames/s from the MZ-700 NTSC and MZ-1500), and a
chapters file `video.chapters.txt` in the YouTube description format. The picture
is scaled by an integer factor using nearest neighbour (sharp pixels) and padded
with black borders to the target resolution.

Options:

| Option | Values | Default |
|--------|--------|---------|
| `--target` | `1080p`, `1440p`, `2160p` | `2160p` |
| `--crop` | `full` (whole border), `reduced` (reduced border; the same as `full` on the MZ-700 and MZ-1500, whose border is narrow), `canvas` (only the 640x200 picture area, 640x400 in the AVI) | `full` |
| `--aspect` | `emulator` (aspect ratio as in the emulator window), `tv43` (approximately 4:3, [unverified]) | `emulator` |
| `--transition` | `cut`, `fade`, `crossfade`, `card` for all boundaries; without it the sidecar values apply | from sidecar |
| `--transition-ms` | transition length in ms | from sidecar |
| `--card-text` | text of the title card (for `card`) | - |
| `--font` | TTF font file; required with `--card-text` | - |
| `--crf` | H.264 quality (lower = better) | `12` |
| `--state-overlay` | `none`, `icons` - burn emulator state indicators from the state marks into the picture (see below) | `none` |
| `--dry-run` | only print the ffmpeg command | - |

Examples:

```
# 4K, full border, transitions from the sidecar
python3 videorec_export.py recording.cuts.json -o video.mp4 --target 2160p

# 1080p, picture area only (canvas)
python3 videorec_export.py recording.cuts.json -o video.mp4 --target 1080p --crop canvas

# 800 ms crossfade at all seams
python3 videorec_export.py recording.cuts.json -o video.mp4 --transition crossfade --transition-ms 800

# title card (2 s black with text) at every seam
python3 videorec_export.py recording.cuts.json -o video.mp4 --transition card \
    --card-text "10 minutes later" --font C:/Windows/Fonts/consola.ttf

# Live / tutorial recording: state icons in the top right corner
python3 videorec_export.py recording.cuts.json -o video.mp4 --target 1080p \
    --state-overlay icons --font C:/Windows/Fonts/seguisym.ttf
```

**State indicators** (`--state-overlay icons`) appear in the top right corner
of the video, according to the state marks the emulator saved during recording:

- a **pause icon** (two bars) while the emulation was paused and the frozen
  picture was recorded (a skipped pause has no frames in the video, so there is
  nothing to mark);
- **real-time** while the recording followed real time;
- the **speed** in real time when it was not 100 %: two triangles and the speed
  factor (e.g. `×4` for 400 %, `MAX` for MAX SPEED), one triangle for a speed
  below 100 % (e.g. `×0.5`). In emulated time the speed is not shown, because
  the video plays at normal speed there.

The pause icon needs no font. The text indicators need a font that contains the
triangle symbol, passed by `--font` (e.g. `C:/Windows/Fonts/seguisym.ttf` on
Windows, or `DejaVuSans.ttf`), and an ffmpeg with the `drawtext` filter; without
them the script stops with an explanation. Recordings without state marks
(recordings from an older emulator version, or `state_marks = none`) are
exported without indicators and without an error.

Transition types: `cut` = hard cut; `fade` = through black; `crossfade` = dissolve
(the video gets shorter by the transition length); `card` = a 2 s black card with
text (the video gets longer by 2 s). A segment between two soft transitions that is
shorter than twice the transition length is rejected by the script, which suggests
a shorter `--transition-ms`.

The script warns when there are fewer than 3 chapters or any chapter is shorter
than 10 s (YouTube may not accept such chapters [unverified]).

## What to do in an external video editor

- **The finished export** (MP4) can be opened by practically any video editor. The
  recommended workflow is to produce an MP4 with the script (even without
  transitions: `--transition cut`) and continue with that.
- **The AVI directly** (ZMBV codec): ffmpeg decodes it (verified). Programs built
  on ffmpeg should open it [unverified]. **DaVinci Resolve** most likely does not
  open ZMBV [unverified] - use the MP4 export.
- The external program is the place for: titles over the picture, music and
  commentary, zoom, picture-in-picture (camera), colour correction, precise
  fine-tuning of cuts.

## Limitations

- Export needs Python 3 and ffmpeg; there is no export inside the emulator.
- The recording does not include the emulator window, debugger or notifications.
- The `tv43` aspect ratio is approximate [unverified]; the default `emulator`
  matches the display in the emulator window.
- Videos are 50 frames/s with an exact rate of 50.000 (the emulator at 100 % runs
  exactly 50 frames per second, a real MZ-800 about 50.04); from the MZ-700 NTSC
  and MZ-1500 exactly 60 frames/s (the emulator at 100 %).
- Real time: sound and picture aligned to within one video frame, about 60 to
  80 ms of silence after each emulation pause (measured, see [Real time](#real-time)).
- In real-time recordings a PSG tone can sometimes "freeze" at a constant level
  for about one frame (a property of the emulator sound output, affects all
  platforms with a PSG); recordings in emulated time are clean.
- The recording contains the sound as the emulator produces it: a game that is
  silent in the emulator (for example some MZ-1500 programs) is silent in the
  recording too.
