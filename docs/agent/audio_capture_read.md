# Analysing sound via the audio capture memory

Recipe for an AI client that has to find out what a program plays, or
whether the emulator's audio output differs from the emulated sound,
without hearing it. The audio capture keeps its data in memory; read it
with `emu_audio_capture_summary`, `emu_trace_read` (channels `audio_*`)
and `emu_audio_capture_pcm`. No file access is needed.

## Recipe

1. `emu_audio_capture_start(frames=N)` - `memory` is on by default, the
   PCM of the reference is kept (`pcm=["reference"]`). For a comparison
   with the output path add `"sdl"`: `pcm=["reference", "sdl"]`. To skip
   the files: `events=False, psg_writes=False, reference_wav=False,
   sdl_wav=False`.
2. Run the program (`emu_run(frames=N + 10)`; the capture starts at the
   end of the next frame, with the output path 6 frames later).
3. `emu_audio_capture_summary()` - start here. It answers "is there
   sound, from when to when, which notes, what does the speaker do, does
   the output path differ" in a few kB.
4. Details: `emu_trace_read("audio_psg_writes")` (every PSG write with
   `hz` and `note` for tone dividers), `emu_trace_read("audio_levels",
   filter={"channels": ["ctc0"]})` (speaker edges),
   `emu_trace_read("audio_frames", start="seq:100", limit=20)` (per-frame
   numbers around frame 100).
5. To give the sound to a person or a script: `emu_audio_capture_pcm(
   frame_from=100, frames=25)` returns a small WAV (11025 Hz mono) as
   base64; `rate=44100, stereo=True, encoding="pcm_b64"` returns the exact
   samples of reference.wav.

The memory stays readable after the capture ends, until the next
`emu_audio_capture_start` (a cursor from before returns
`segment_replaced:`).

## Reading the summary

- `sound_spans` / `silent_spans`: silent = reference RMS below
  `silence_rms` (int16, default 64). A speaker held at a constant level
  is silent (no change = no sound).
- `channels.<name>.est_hz_median`: level changes / 2 per second, median
  over frames with changes. Right for tones; for the noise channel and
  for speaker PCM it is not a pitch.
- `psg_notes`: replayed from the PSG writes, starting from the PSG state
  at the capture start. `psg_state_incomplete: true` = the ring dropped
  the oldest writes, notes at the beginning may be wrong. The pitch is the
  emulator's PSG formula (same as the PSG Audio Scope), not a measurement
  of real hardware. Tone divider 0 counts as 1024 (the lowest tone, as on
  the real chip); divider 1 is a constant level in the emulator and makes
  no note.
- `ctc0.likely` (`tone`, `pcm`, `irregular`, `silent`) is a heuristic
  estimate from the histogram of intervals between speaker edges
  (`likely_rule`). Check `edge_interval_hist_us` before relying on it.
- `ref_vs_sdl`: `lag_samples` is the delay of the output path (without
  an audio device exactly one frame). It is found from note onsets and
  volume changes; with only a steady tone in the compared part it cannot
  be determined (`lag_ambiguous: true`, pass `lag_frames` explicitly).
  `diff_rms` / `diff_max` are in -1..1 units of the mono mix;
  `worst_frames` lists frames where the output path differs most;
  `sdl_missing_blocks` > 0 = the output path skipped frames (MAX SPEED
  with an audio device).

## Limits

| Ring | Default size | When full |
|------|--------------|-----------|
| level changes | 1M records (16 MiB) | oldest dropped (`gap` in `emu_trace_read`) |
| PSG writes | 256k records (6 MiB) | oldest dropped, notes at the start may be wrong |
| frame summaries | 10 minutes (`memory_minutes`) | oldest frames dropped |
| PCM | 60 s per source (`pcm_seconds`), 10.6 MB per minute | oldest samples dropped |

A PCM window larger than `max_bytes` (default 32 KiB, below the 50 000
character limit for tool results) is cut: continue with
`sample_from=next_sample`.
