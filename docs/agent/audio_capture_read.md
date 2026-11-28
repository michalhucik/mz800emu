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

- Time base: all times and frequencies (`time_us`, `hz`, `est_hz_median`,
  `edge_interval_hist_us`, PCM samples) are real time of the machine,
  following the crystal (`real_clk_hz`, 17 734 475 Hz PAL, 14 318 180 Hz
  NTSC), not the rounded clock (`gdg_clk_hz`). A frame has the same number
  of ticks as always but 881.36 samples (735.95 in NTSC); at 100 % frame N
  of the reference starts at sample floor(N * 44100 * ticks_per_frame /
  real_clk_hz), the output path makes blocks of 881 or 882 samples (735
  or 736) on one continuous time line (no phase jump between blocks), and
  `lag_frames` is lag_samples divided by 881.36 (735.95). The two sample
  grids have the same spacing but are shifted against each other by a
  fraction of a sample that changes from capture to capture (measured
  -0.48 to +0.51 sample); to compare the streams sample by sample, align
  them by a fractional shift first. Both streams average a PSG channel
  over the interval of each sample, so tones up to about 4 kHz have the
  same folded bass in both (within about 2 dB). Higher PSG tones (above
  about 5 kHz, divider 20 and lower) keep in
  sdl.wav a weak extra folded bass that depends on the grid phase (at
  divider 2 the strongest line up to about 50 dB below the tone): the log
  of a frame knows the PSG only up to its last PSG step, so the last
  output sample of a block may contain up to 79 cycles (MZ-1500 63) of a
  held value. In captures made with version 2.0.7 sdl.wav took one point
  per sample and had strong false low tones under high PSG tones.
- A capture frame is one synchronisation event of the emulation: a
  picture frame at 100 %, at a custom speed a stretch of speed times a
  frame. For it the reference gets the samples of its emulated time
  (about 441 at 50 %, 1763 at 200 % on PAL), the output path one block
  of 881 or 882 samples at any speed. `emu_audio_capture_pcm(frame_from=f,
  frames=k)` returns the samples of frames f to f + k - 1 of the chosen
  stream exactly (from the first sample of frame f to the first sample of
  frame f + k), so the sample ranges of windows read frame by frame join
  without a gap or an overlap. The boundaries come from an index the
  capture keeps for as long as the PCM holds the frame (independent of the
  frame summary memory, at any speed and after a speed change), so a
  frame read after its summary was dropped returns the same samples as
  before. Frame windows cover closed frames only: `frame_range` in the
  answer says what was returned (`requested`, `returned`,
  `closed_frames`, `held_from`, `status` complete / partial / empty,
  `reasons`). A window starting at the frame still open (or after the
  capture stopped, at frame `closed_frames`) is empty with reason
  `start_after_last_closed_frame` and `from_sample` at the end of the
  closed frames; PCM after that point (the output path may have one more
  block than closed frames) is read with `sample_from`. A window reaching
  past the last closed frame ends there (`end_after_last_closed_frame`),
  so asking for frames N-1 and N together gives the same as N-1 alone. A
  frame whose boundary is no longer held (its PCM was overwritten) is left
  out with `start_boundary_not_held` - never replaced by sound from
  another time. `status` and `reasons` tell only whether the frame
  BOUNDARIES are known. The SOUND of a frame can be overwritten while its
  boundaries are still known (the index is shared by both streams and
  kept while either of them needs the frame). A frame window is fixed:
  the answer has only those samples of the window that the PCM memory
  still holds, never samples of the next frame. `frame_range` reports it
  separately: `samples_requested` [from, to) is the sample window of the
  returned frames (with `samples` it starts at the first frame; its end,
  like the end of `requested`, is capped at 9223372036854775807 =
  INT64_MAX, so even `samples` = INT64_MAX gives no negative number;
  all parameters of the tool are integers up to INT64_MAX),
  `samples_held` [from, to) the part the PCM memory held when it was
  copied, `audio_status` `complete` (whole window held), `partial` (part
  of it overwritten, or not captured yet), `lost` (no sample held, the
  answer has 0 samples and `from_sample` = `next_sample` = end of the
  window when it was overwritten, start of the window when it was not
  captured yet) or `none` (empty window), and `audio_reasons` (`pcm_overwritten`,
  `pcm_not_captured`). `gap` [from, to) lists the overwritten samples
  inside the requested window only. Frames read one by one therefore still
  join without overlap: a partly overwritten frame returns its kept end,
  the next frame starts where it ended. The limit `max_bytes` applies to
  the kept part (`truncated: true`); continue with `sample_from` =
  `next_sample` and `samples` = the rest up to the end of the window. In
  a live capture the intersection is taken together with the copy (one
  lock), so it cannot move between them; `available` in the same answer
  is the held range at that moment. Reading by `sample_from` (a cursor)
  works as before: when its start was overwritten it continues from the
  oldest held sample and returns `samples` samples from there (`gap` =
  the skipped part). With `rate=44100,
  stereo=True` the joined data equal one window over the same frames byte
  for byte; with another `rate` every window is resampled on its own, so
  the joined data may differ from one window (length and values, measured: 12
  windows at 11025 Hz mono 2640 instead of 2644 values, most of them
  different) - read one window to get a continuous resampled stretch. Without an
  audio device the output path window of frame f holds the block of
  frame f - 1 (the delay of one frame).

- `sound_spans` / `silent_spans`: silent = reference RMS below
  `silence_rms` (int16, default 64). A speaker held at a constant level
  is silent once the level has faded (no change = no sound).
- `channels.<name>.est_hz_median`: level changes / 2 per second, median
  over frames with changes. Right for tones; for the noise channel and
  for speaker PCM it is not a pitch.
- `psg_notes`: replayed from the PSG writes, starting from the PSG state
  at the capture start. `psg_state_incomplete: true` = the ring dropped
  the oldest writes, notes at the beginning may be wrong. The pitch is the
  emulator's PSG formula (same as the PSG Audio Scope), not a measurement
  of real hardware. Tone divider 0 counts as 1024 (the lowest tone, as on
  the real chip); divider 1 makes no note: the channel is a constant level
  of 0.49 of its full swing (as measured on a real MZ-800), used to play
  samples through the volume register.
- `audio_levels` / events.csv `level`: 0 = silent, 1-15 = full swing of
  the volume step (15 minus attenuation), 16-30 = 0.49 of the swing of
  step `level - 15` (a PSG tone channel with divider 1), 31 = 0.64 of the
  swing of step 15 (only the `ctc0` channel, while CTC0 runs in mode 3
  with divider 2 and PC0 is 1: the 554 kHz square wave is not logged edge
  by edge, the channel holds this constant level instead, as measured on
  a real MZ-800; PC0 = 0 gives 0; in the MZ-700 emulators level 31 is
  0.50 of step 15 and PC0 has no effect). In `audio_frames` and in the summary
  `max_level` is the volume step 0-15 (15 for level 31) and `rms_level`
  counts levels 16-31 with their real amplitude.
- Levels between sources: the speaker (`ctc0`, level 15) is 1.49 times
  the swing of one PSG channel at attenuation 0 (+3.5 dB, measured on a
  real MZ-800); the user volumes in the INI multiply this.
- Absolute PCM levels (`reference.wav`, `sdl.wav`, `rms`/`peak` of the
  `audio_frames` rows): the sum of the sources is multiplied by a make-up
  gain, 2.51 (+8 dB) with `AUDIO/output_filter` on and 1.58 (+4 dB) with
  it off and in the MZ-700 emulators, and then goes through a soft
  limiter. Up to 0.7 of full scale (int16 22937) the output is exactly
  linear, so level ratios can be read directly; above that the peaks are
  smoothly compressed towards full scale (never clipped), so do not derive
  ratios from samples above 22937. One PSG channel at attenuation 0 has a
  settled peak-to-peak swing of 0.25 times the gain, the speaker 1.49
  times as much; one or two full PSG channels or the speaker alone stay
  linear once settled, three full PSG channels or the speaker together
  with full PSG channels reach the limiter.
  Captures made with version 2.0.7 are 8 dB (filter on) or 4 dB (filter
  off) lower. The make-up gain is a choice of the emulator, not a measured
  property of the machine.
- The speaker (`ctc0`, also the tape echo) is AC coupled with a time
  constant of 0.85 s: a square wave is symmetric around zero in the PCM
  and a level held constant decays to silence (it is not cut off after
  45 ms any more), so `sound_spans` may extend about a second past the
  last speaker edge.
- The whole output of the MZ-800 and MZ-1500 is AC coupled as well (time
  constant 0.57 s, from the schematic, measurements agree): PSG tones are
  symmetric around zero in the PCM and a PSG channel held at a constant
  level (also a divider-1 channel with its volume set) decays to silence
  within a few seconds instead of being cut off after 45 ms. `sound_spans`
  can therefore extend up to about 3 s past the last change.
- `AUDIO/output_filter` (`emu_settings_set`, default `true`): the PCM of
  the reference and of the output path goes through the low-pass filter
  of the real MZ-800 output (first order, 346 Hz). A square wave tone
  then looks rounded and tones above a few kHz are much weaker (-20 dB at
  3.5 kHz). Set it to `false` to compare with captures made before, or
  when the exact edges matter. The level events (`audio_levels`) are not
  affected. The MZ-700 emulators have no such key (`settings_set` fails):
  that machine has no output filter, the PCM always uses the older
  per-channel smoothing.
- The user's master volume and mute (overlay menu, Sound tab, and the
  Audio Mixer window) are applied only to what the sound device plays.
  The capture files (`reference.wav`, `sdl.wav`), the level events and
  the sound of a video recording do not contain them: a capture has the
  full level even while the user has muted the emulator. The master
  volume (0 to 200 %) is stored in the INI keys `[AUDIO] volume_master`
  (0 to 4294967295 = 0 to 100 %) and `volume_master_boost` (per cent
  above 100 %, 0 to 100); they are not in the `emu_settings_set`
  whitelist, `emu_settings_get` reads them. Mute is not
  a setting: it is not saved, has no key, and the emulator always starts
  unmuted.
- How well founded the sound path is: MZ-800 measured on a real machine,
  MZ-1500 derived from the MZ-800, MZ-700 estimated from the schematic
  (nothing measured). On the MZ-700 the AC coupling of the speaker is an
  aid of the emulator, the real speaker is DC coupled.
- `ctc0.likely` (`tone`, `pcm`, `irregular`, `silent`) is a heuristic
  estimate from the histogram of intervals between speaker edges
  (`likely_rule`). Check `edge_interval_hist_us` before relying on it.
- `ref_vs_sdl`: `status` is `ok`, `no_overlap` or `not_comparable`;
  without numbers (`no_overlap`, `not_comparable`) the object always has
  `error` (as in version 2.0.7, which had no `status`) and `reason`.
  `window` [from, to) is the sample window of the chosen frames in
  reference indexes (not present with `not_comparable`). It is built from
  the frame boundaries the capture stored (the same indexes
  `emu_audio_capture_pcm(frame_from=...)` returns in
  `frame_range.samples_requested`), so the window of a frame equals its PCM
  window and the windows of adjacent frame ranges join without a gap or an
  overlap. Only pairs (reference sample
  i, output path sample i + lag) that both PCM memories still hold inside
  this window are compared: `from_sample` is the first compared reference
  index, `compared_samples` the number of pairs (at most 2 s = 88200 from
  `from_sample`). With `lag_frames` fixed the pairs use that delay; with
  the search (`lag_frames` -1) the compared part starts at the first index
  held in both streams (delay 0) and the delay is searched from there. A
  window whose samples one of the streams no longer holds gives
  `no_overlap` (`reason` names the stream), never a number computed from
  samples outside the window. At the oldest end of the PCM memory the
  reference may still hold the end of a window while the output path holds
  only later indexes (the sound of the same moment is there only after its
  delay): the search then has no common start and the answer is
  `no_overlap` with its own `reason` ("no start for the delay search"),
  `fixed_lag_min_samples` (smallest delay with at least one pair) and
  `fixed_lag_min_frames` (the smallest fixed `lag_frames` 1-60 that pairs
  held samples, null if none). Only this value is promised to work: a
  larger `lag_frames` may already run past the held output path (measured
  on PAL: 1 to 51 compared, 52 to 60 had no pair). The delay is
  deliberately not estimated there: the held part is shorter than the
  output path delay (measured 32 and 678 pairs at the edge on MZ-800 and
  MZ-700 NTSC), too short for a reliable estimate; pass
  `lag_frames` = `fixed_lag_min_frames`, or the real delay found on a
  later window if it still pairs, to compare it. In captures read with
  version 2.0.7 a frame whose PCM was already overwritten could give `ok`
  with numbers from later samples, and a partly overwritten frame could
  give `no_overlap` although both streams held its end.
  `not_comparable` has two causes. (1) A frame of the range, or a
  frame before it, ran at a custom speed: the reference holds the emulated
  time of each frame, the output path one block per frame, so one sample
  window would compare different moments; instead of `diff_rms` the answer
  has `error`, `reason`, `frames_off_nominal`, `nominal_frame_ticks` and
  `sdl_minus_ref_samples`. The same frame length rule (1/1000 of a frame)
  applies to the frames after the window that the fixed `lag_frames`
  reaches, or that the delay search can reach (the whole `max_lag_frames`
  range, not only the delay it finds): one such frame at a custom speed
  gives `not_comparable` with the same `error`, a `reason` naming the fixed
  lag or the delay search, `frames_off_nominal` (frames of that reach off
  the nominal length), `first_off_nominal_frame`, `nominal_frame_ticks`,
  `sdl_minus_ref_samples` and `sdl_missing_blocks`, no numbers and no
  `window`. A smaller `max_lag_frames`, or a fixed `lag_frames` whose reach
  ends before `first_off_nominal_frame`, can be compared; a speed change
  after the reach does not affect the answer. So the search near a speed
  change is refused rather than limited (a search cut short could return a
  wrong delay without notice). The output path samples after the last
  stored frame record (the open frame of a live capture, the rest after
  stop) have no frame length yet; this check does not cover them, they are
  compared as before. (Check (2) alone does not catch this: at 90 or 110 %
  the stream indexes drift apart by only tens of samples per frame, under
  its half-block tolerance.) To compare the sound at a custom speed read
  both streams frame by frame with `emu_audio_capture_pcm` (the output
  path then plays the same emulated time faster or slower, its pitch
  differs). (2) Output path blocks are missing in the compared part or in
  the part after it that the fixed or searched delay reaches: with an
  audio device, MAX SPEED skips whole output blocks (headless it does not),
  so from that frame on the output path is shifted against the reference
  by whole blocks; the answer has `error`, `reason` ("output path blocks
  are missing ..."), `sdl_shift_frame` (first frame where the shift
  changes), `sdl_empty_frames` (frames of the checked part without an
  output block) and `sdl_minus_ref_samples`, no numbers and no `window`.
  The check uses the stored frame boundaries of both streams; a tolerance
  of half a block covers the 0 to 2 samples by which the stream starts
  differ in a normal capture. Blocks missing later in the capture do not
  affect an earlier window. A continuous part after the gap is compared
  with the output path samples from the real start of its first frame
  (shifted by the missing blocks), so it gives `ok` again. In captures read
  with version 2.0.7 a window after (or reaching into) missing blocks gave
  `ok` with output path samples of another frame.
  With `ok`, `lag_samples` is the delay of the output path (without
  an audio device exactly one frame) and `sdl_from_sample` the first
  compared index of the output path stream (reference index + delay, after
  missing blocks + the real shift). A searched delay is an ESTIMATE:
  `lag_ambiguous: false` only means that the search found no equally good
  rival, not a proof of the best alignment. To decide about a regression
  pass a fixed `lag_frames` (without an audio device 1). Parts up to 2048
  pairs (with the default range of 8 frames; with a larger
  `max_lag_frames` proportionally fewer, the work is limited to 2048 x
  7051 sample pairs, a few ms) are searched exactly on the original
  samples over every delay; longer parts by the envelope (average |x|
  over 256 samples) with a coarse step of 16 samples, refined on the
  original samples by +-8 - there an alignment off the grid can be missed.
  Only delays that leave at least half of the largest
  possible overlap are tried (with the usual call the output path part is
  longer than the reference part by the searched range, so nothing is cut;
  `compared_samples` is never below that half). `lag_ambiguous: true`
  means the data do not determine the delay and `lag_samples` is only the
  first of equally good ones; pass `lag_frames` explicitly then. It is
  set when:
  - only a steady tone, silence or a constant level is in the compared
    part (every delay fits equally, also in a short part such as 32
    samples);
  - another delay fits as well: a local minimum within 1 % of the median
    score of all delays (exact search: any other delay, also one sample
    away, for example two exact copies or a tone that matches at every
    period; longer parts, on the envelope: only minima more than 256
    samples away), or no minimum below half of the median;
  - for longer parts, a delay up to 256 samples from the chosen one is a
    separate minimum on the original samples (more than 8 samples away)
    and at most 5 % worse at the same overlap length (two exact copies
    close to each other; a tone onset one period away is about 3.6 times
    worse and stays unambiguous);
  - with the exact search, the best delay leaves more than a quarter of
    the energy of both parts (independent signals, a chance minimum).

  The rules are heuristics of the emulator; they only set the flag.
  Limits of the envelope search (longer parts only): an exact alignment
  off the 16-sample grid and more than 8 samples from the refined delay
  can be missed (then the chosen delay is not the best one although the
  flag may be `false`), two equally good delays closer than 9 samples to
  each other are not recognised, and a near minimum whose envelope score
  is more than 25 % of the median above the best is not checked on the
  original samples. Measured on synthetic copies (periods 2 to 320
  samples, 882 pairs, delay up to 881): the exact search flagged all 1914
  cases with two exact copies and found all 957 single copies with
  `false`; the envelope search had flagged 1434 and chose 3 wrong delays
  as certain. Version 2.0.7 used a weaker rule: a short part (below
  256 samples) never had `lag_ambiguous: true`, for example 32 silent
  samples, and a steady tone over a few frames could have `false`
  (measured on MZ-800 with a CTC0 tone over frames 124 to 126: delay 1984
  reported as certain, while 681, 1583, 2485 and 1082 fit the samples as
  well or better).
  `diff_rms` / `diff_max` are in -1..1 units of the mono mix;
  `worst_frames` lists up to five frames where the output path differs
  most, as [frame, diff_rms] pairs (the format of version 2.0.7);
  `worst_frames_detail` lists the same frames in the same order as objects
  `{frame, diff_rms, compared_samples}`. Every frame is rated on the part
  of it that was compared (its stored boundaries, as in the PCM windows),
  also a frame whose start lies before `from_sample` (overwritten) or whose
  end lies after the 2 s limit; `compared_samples` of an item is the number
  of compared pairs of that frame (fewer than its 881 / 882 or 735 / 736
  samples for a partly compared frame). In captures read with version
  2.0.7 a frame starting before the compared part was left out (a single
  partly held frame gave an empty list);
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
