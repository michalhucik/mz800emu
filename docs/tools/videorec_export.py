#!/usr/bin/env python3
"""Export an emulator recording (AVI parts + *.cuts.json) to MP4 for YouTube.

Note on language: code comments and docstrings in this file are written in
English. This is an explicit exception from the project rule of Czech
comments (decision by the project owner), because the script is a
user-facing helper tool shipped in docs/tools/.

The input is the sidecar `<name>.cuts.json` (version 1, 2, 3 or 4) written by
the emulator (`videorec_sidecar_to_json`): dimensions, fps, the list of AVI
parts, segments (cuts with a transition type) and markers. Frame numbers are
global output indexes across the parts. The script builds a filtergraph for
an external ffmpeg and produces an MP4 (H.264 High, yuv420p, AAC 320k
48 kHz, faststart) and a chapters file `<output without extension>.chapters.txt`
(e.g. `video.mp4` -> `video.chapters.txt`) in the YouTube format.

Sidecar versions:
- version 1 (older recordings): the AVI has the native 928x288 framebuffer,
  the MZ-800 pixel is doubled vertically on display -> scale (n, 2n).
- version 2: `width`/`height` are the AVI dimensions and `"line_doubled": true`
  means every framebuffer line is stored twice in the AVI (928x576, same
  aspect as the emulator window) -> scale (n, n). The resulting MP4 is the
  same for both versions.
- version 3: version 2 plus `events` - emulator state marks
  `{frame, kind, value}` (timebase, pause_start/pause_end, speed, snapshot,
  reset). They are loaded and validated (older versions get an empty list).
  With `--state-overlay icons` they are burned into the picture as state
  indicators in the top right corner: a pause icon (two bars) while the
  emulation was paused and the frozen picture was recorded, "real-time"
  while the recording followed real time, and the speed (two play
  triangles + "x4", "MAX", or one triangle + "x0.5") in real time at a
  speed other than 100 %. The default `--state-overlay none` ignores the
  events; older versions have no events, so nothing is drawn (no error).
- version 4: version 3 plus the platform description - `platform` (mz700,
  mz800, mz1500), `tv_system` (pal, ntsc), `framebuffer_width`/`height`
  (the native frame) and `canvas {x, y, width, height}` (the picture area
  inside the native frame). The crops and the scale are computed from these
  values, so the export works for every platform: MZ-800 (928x288 frame,
  50 fps), MZ-700 PAL (704x232, 50 fps), MZ-700 NTSC and MZ-1500 (704x232,
  60 fps). The output keeps the fps of the recording (`fps_num/fps_den`).
  Versions 1-3 have no platform description and are always MZ-800 recordings
  (928x288 frame, canvas 154,46 640x200, 50 fps).

Usage:
    python videorec_export.py rec.cuts.json -o out.mp4 [--target 2160p] ...

Python 3 standard library only. User-facing output is in English.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

#: Target resolutions (width, height) selected by --target.
TARGETS = {"1080p": (1920, 1080), "1440p": (2560, 1440), "2160p": (3840, 2160)}

#: MZ-800 canvas inside the native 928x288 frame: (x, y, w, h), taken from
#: mz800_video.h. Used for sidecar versions 1-3 (always MZ-800); version 4
#: carries the canvas of its platform. For line-doubled frames (version 2+)
#: the vertical coordinates are multiplied by 2.
CANVAS_RECT = (154, 46, 640, 200)
#: Extra margin for the `reduced` crop (px on each side, in the native frame):
#: 32 horizontally, 16 vertically (the pixel is displayed 1:2, so 16 lines
#: ~ 32 px of width). The result is clipped to the frame (the MZ-700 and
#: MZ-1500 border is exactly that wide, so `reduced` = `full` there).
REDUCED_MARGIN = (32, 16)
#: Native MZ-800 framebuffer size (sidecar versions 1-3; for version 2+ with
#: line_doubled this corresponds to a 928x576 AVI).
FULL_SIZE = (928, 288)
#: Supported sidecar versions.
SIDECAR_VERSIONS = (1, 2, 3, 4)
#: Title card length in seconds.
CARD_SECONDS = 2.0
#: Text shown by --state-overlay icons while the recording follows real time.
REALTIME_TEXT = "real-time"
#: Fast-forward symbol (U+25B6 BLACK RIGHT-POINTING TRIANGLE twice).
FAST_SYMBOL = "▶▶"
#: Slow-motion symbol (one triangle) for speeds below 100 %.
SLOW_SYMBOL = "▶"
#: Multiplication sign (U+00D7) used in the speed indicator.
TIMES_SIGN = "×"


class ExportError(Exception):
    """Export error (bad sidecar, missing font, ...); the message is meant for the user."""


@dataclass
class Options:
    """Export options (match the CLI switches).

    transition: None = use `transition_in` from the sidecar; otherwise
        cut|fade|crossfade|card applies to all transitions between segments.
    transition_ms: None = value from the sidecar.
    state_overlay: none = ignore the sidecar events; icons = burn the state
        indicators (pause / real-time / speed) into the picture.
    """
    target: str = "2160p"
    crop: str = "full"
    aspect: str = "emulator"
    transition: Optional[str] = None
    transition_ms: Optional[int] = None
    card_text: Optional[str] = None
    font: Optional[str] = None
    crf: int = 12
    state_overlay: str = "none"


def _check_platform(sc: dict) -> None:
    """Validate the platform description of a version 4 sidecar (ExportError if bad).

    Requires integer `framebuffer_width`/`height` > 0, a `canvas` with integer
    x, y >= 0 and width, height > 0 lying inside the framebuffer, and an AVI
    size equal to the framebuffer (height doubled with `line_doubled`)."""
    fw, fh = sc.get("framebuffer_width"), sc.get("framebuffer_height")
    if not (isinstance(fw, int) and isinstance(fh, int) and fw > 0 and fh > 0):
        raise ExportError("sidecar has no valid framebuffer_width/framebuffer_height")
    c = sc.get("canvas")
    if not isinstance(c, dict) or not all(isinstance(c.get(k), int)
                                          for k in ("x", "y", "width", "height")):
        raise ExportError("sidecar has no valid canvas")
    if (c["x"] < 0 or c["y"] < 0 or c["width"] <= 0 or c["height"] <= 0
            or c["x"] + c["width"] > fw or c["y"] + c["height"] > fh):
        raise ExportError("sidecar canvas %r lies outside the %dx%d framebuffer" % (c, fw, fh))
    vf = 2 if sc.get("line_doubled") else 1
    if (sc["width"], sc["height"]) != (fw, fh * vf):
        raise ExportError("sidecar video size %dx%d does not match the %dx%d framebuffer"
                          % (sc["width"], sc["height"], fw, fh))


def load_sidecar(path: str) -> dict:
    """Load and validate the sidecar; raises ExportError for an unreadable
    file, a version other than 1-4, missing keys, malformed events or (version
    4) an inconsistent platform description (see _check_platform).

    The returned dict always has an `events` list (empty for versions 1
    and 2)."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            sc = json.load(f)
    except (OSError, ValueError) as e:
        raise ExportError("cannot read sidecar %s: %s" % (path, e))
    if not isinstance(sc, dict) or sc.get("version") not in SIDECAR_VERSIONS:
        raise ExportError("unsupported sidecar version: %r"
                          % (sc.get("version") if isinstance(sc, dict) else None))
    for k in ("width", "height", "fps_num", "fps_den", "audio_rate",
              "parts", "segments"):
        if k not in sc:
            raise ExportError("sidecar is missing key '%s'" % k)
    if not sc["parts"] or not sc["segments"]:
        raise ExportError("sidecar has no parts or no segments")
    if sc["version"] >= 4:
        _check_platform(sc)
    events = sc.get("events", []) if sc["version"] >= 3 else []
    if not isinstance(events, list):
        raise ExportError("sidecar 'events' is not a list")
    for e in events:
        if (not isinstance(e, dict) or not isinstance(e.get("frame"), int)
                or not isinstance(e.get("kind"), str) or not isinstance(e.get("value"), str)):
            raise ExportError("malformed sidecar event: %r" % (e,))
    sc["events"] = events
    return sc


def is_line_doubled(sidecar: dict) -> bool:
    """True if the recording AVI has doubled lines (version 2+ with `line_doubled`).

    Version 1 always has the native framebuffer (False)."""
    return sidecar.get("version") in (2, 3, 4) and bool(sidecar.get("line_doubled", False))


def frame_geometry(sidecar: dict) -> dict:
    """Native frame geometry of the recording.

    Returns {"framebuffer": (w, h), "canvas": (x, y, w, h), "line_doubled":
    bool} in native framebuffer pixels (lines not doubled). Version 4 takes
    the values from its platform description; versions 1-3 are always
    MZ-800 recordings (FULL_SIZE, CANVAS_RECT)."""
    if sidecar.get("version", 0) >= 4:
        c = sidecar["canvas"]
        fb = (sidecar["framebuffer_width"], sidecar["framebuffer_height"])
        canvas = (c["x"], c["y"], c["width"], c["height"])
    else:
        fb, canvas = FULL_SIZE, CANVAS_RECT
    return {"framebuffer": fb, "canvas": canvas, "line_doubled": is_line_doubled(sidecar)}


def integer_scale(w: int, h: int, aspect: str, target: str,
                  line_doubled: bool = False) -> Tuple[int, int]:
    """Return the integer scale (sx, sy) for a w x h frame in the `target`.

    `line_doubled` = False (native framebuffer, version 1): the vertical
    scale is 2n (the MZ-800 pixels are 1:2 - twice as tall). True (AVI with
    doubled lines, version 2): the lines are already doubled, the vertical
    scale is n.
    emulator: (n, 2n) or (n, n) respectively, with the largest n that makes
        the w x h frame fit into the target.
    tv43: sy the same; sx is an APPROXIMATION: h*sy*4/3/w rounded to an
        integer (min. 1), i.e. the resulting aspect ratio is not exactly
        4:3, only the closest one reachable with nearest-neighbor scaling.
    Raises ExportError if the frame does not fit into the target even at
    scale 1.
    """
    tw, th = TARGETS[target]
    vf = 1 if line_doubled else 2
    n = min(tw // w, th // (vf * h))
    if n < 1:
        raise ExportError("frame %dx%d does not fit into %s" % (w, h, target))
    sy = vf * n
    if aspect == "tv43":
        sx = max(1, round(h * sy * 4 / 3 / w))
        sx = min(sx, tw // w)
    else:
        sx = n
    return sx, sy


def crop_rect(crop: str, w: int, h: int, line_doubled: bool = False,
              framebuffer: Tuple[int, int] = FULL_SIZE,
              canvas: Tuple[int, int, int, int] = CANVAS_RECT) -> Tuple[int, int, int, int]:
    """Return the crop rectangle (x, y, w, h) within the w x h AVI frame.

    `framebuffer` is the native frame size and `canvas` the picture area in
    it (defaults: MZ-800). canvas/reduced are defined only when the AVI has
    the native framebuffer size (`line_doubled` False) or the framebuffer
    with doubled lines (`line_doubled` True - vertical coordinates x2); for
    any other size the whole frame is returned (full). `reduced` adds
    REDUCED_MARGIN around the canvas, clipped to the frame."""
    vf = 2 if line_doubled else 1
    fw, fh = framebuffer
    if crop == "full" or (w, h) != (fw, fh * vf):
        return (0, 0, w, h)
    cx, cy, cw, ch = canvas
    if crop == "reduced":
        mx, my = REDUCED_MARGIN
        x0, y0 = max(0, cx - mx), max(0, cy - my)
        x1, y1 = min(fw, cx + cw + mx), min(fh, cy + ch + my)
        cx, cy, cw, ch = x0, y0, x1 - x0, y1 - y0
    return (cx, cy * vf, cw, ch * vf)


def sidecar_crop_rect(sidecar: dict, crop: str) -> Tuple[int, int, int, int]:
    """crop_rect() for the AVI frame of a sidecar (geometry from frame_geometry())."""
    g = frame_geometry(sidecar)
    return crop_rect(crop, sidecar["width"], sidecar["height"], g["line_doubled"],
                     g["framebuffer"], g["canvas"])


def used_parts(sidecar: dict) -> List[dict]:
    """Return the parts that contain at least one frame of the recording.

    Part i holds the frames first_frame(i) .. first_frame(i+1) - 1; the last
    part holds first_frame .. total - 1, where total is the end of the last
    segment. A part with an empty range has no frames (e.g. an AVI part that
    a retake cut back to zero frames right before the recording stopped;
    older emulator versions listed it in the sidecar). Such parts are left
    out of the concat list - their file may be empty or missing - which does
    not shift the timeline, because they contribute no frames."""
    parts = sidecar["parts"]
    total = max(s["end"] for s in sidecar["segments"])
    out = []
    for i, p in enumerate(parts):
        end = parts[i + 1]["first_frame"] if i + 1 < len(parts) else total
        if p["first_frame"] < end:
            out.append(p)
    return out


def check_parts_exist(sidecar: dict, base_dir: str) -> None:
    """Raise ExportError if a part with frames (see used_parts) has no file.

    Such a part cannot be skipped: every later frame would move in time."""
    for p in used_parts(sidecar):
        path = os.path.join(base_dir, p["file"])
        if not os.path.isfile(path):
            raise ExportError("video file part not found: %s" % path)


def build_parts_list(sidecar: dict, base_dir: str) -> str:
    """Build the file content for the concat demuxer (one `file '...'` per line).

    Only parts with frames are listed (see used_parts). Relative part names
    are resolved against `base_dir` (the sidecar directory). Paths use `/`
    separators, an apostrophe is escaped as `'\\''`."""
    lines = []
    for p in used_parts(sidecar):
        path = os.path.join(base_dir, p["file"]).replace("\\", "/")
        lines.append("file '%s'" % path.replace("'", "'\\''"))
    return "\n".join(lines) + "\n"


def plan_timeline(sidecar: dict, opts: Options) -> dict:
    """Compute the output timeline.

    Returns {"segments": [{start,end,dur,trans,out_start}], "d": D, "total": s}.
    Effective transition of segment i>0: opts.transition, otherwise the
    segment's `transition_in` (`none`/missing = cut). A segment that has a
    transition at both ends must be at least 2*D long (otherwise
    ExportError). fade does not change the length, crossfade shortens it by
    D per transition, card adds CARD_SECONDS. out_start = start of the
    segment in the output (used for chapters)."""
    fps = sidecar["fps_num"] / sidecar["fps_den"]
    ms = opts.transition_ms if opts.transition_ms is not None \
        else sidecar.get("transition_ms", 500)
    d = ms / 1000.0
    segs = []
    cur = 0.0
    for i, s in enumerate(sidecar["segments"]):
        dur = (s["end"] - s["start"]) / fps
        if dur <= 0:
            raise ExportError("segment %d is empty" % i)
        if i == 0:
            tr = "none"
            out_start = 0.0
            cur = dur
        else:
            tr = opts.transition or s.get("transition_in") \
                or sidecar.get("default_transition", "cut")
            if tr == "none":
                tr = "cut"
            if tr not in ("cut", "fade", "crossfade", "card"):
                raise ExportError("unknown transition '%s'" % tr)
            if tr == "crossfade":
                if d > dur or d > cur:
                    raise ExportError("segment %d is shorter than the crossfade" % i)
                out_start = cur - d
                cur = cur - d + dur
            elif tr == "card":
                out_start = cur + CARD_SECONDS
                cur = out_start + dur
            else:
                out_start = cur
                cur += dur
        segs.append({"start": s["start"], "end": s["end"], "dur": dur,
                     "trans": tr, "out_start": out_start})
    soft = ("fade", "card", "crossfade")
    for i, sg in enumerate(segs):
        has_in = sg["trans"] in soft
        has_out = i + 1 < len(segs) and segs[i + 1]["trans"] in soft
        if has_in and has_out and sg["dur"] < 2 * d:
            raise ExportError(
                "segment %d is %.3f s long, shorter than twice the transition "
                "(%.3f s); use a shorter --transition-ms" % (i, sg["dur"], d))
    return {"segments": segs, "d": d, "total": cur}


def _fmt_time(sec: float) -> str:
    """Chapter time format: MM:SS, from one hour up H:MM:SS (YouTube)."""
    t = int(sec)
    h, rem = divmod(t, 3600)
    m, s = divmod(rem, 60)
    return "%d:%02d:%02d" % (h, m, s) if h else "%02d:%02d" % (m, s)


def chapter_lines(sidecar: dict, opts: Options) -> List[str]:
    """Chapter lines `MM:SS label` built from markers; the first is always `00:00 Start`.

    A marker is mapped to the output time through the segment that contains
    its frame ([start, end)); markers outside the segments are dropped. A
    marker exactly at time 0 replaces the Start line. The YouTube rule (min.
    3 chapters of >= 10 s each) is not enforced here."""
    plan = plan_timeline(sidecar, opts)
    fps = sidecar["fps_num"] / sidecar["fps_den"]
    items = []
    for m in sidecar.get("markers", []):
        for seg in plan["segments"]:
            if seg["start"] <= m["frame"] < seg["end"]:
                t = seg["out_start"] + (m["frame"] - seg["start"]) / fps
                items.append((t, m["label"]))
                break
    items.sort(key=lambda x: x[0])
    lines = []
    if not items or int(items[0][0]) > 0:
        lines.append("00:00 Start")
    lines += ["%s %s" % (_fmt_time(t), lab) for t, lab in items]
    return lines


def chapter_warnings(lines: List[str], total: float) -> List[str]:
    """Return warnings if the chapters violate the YouTube rules.

    Rules: at least 3 chapters and each at least 10 s long. `lines` is the
    output of chapter_lines, `total` is the total output length in seconds
    (the last chapter ends at that point)."""
    times = []
    for ln in lines:
        parts = [int(x) for x in ln.split(" ", 1)[0].split(":")]
        t = 0
        for x in parts:
            t = t * 60 + x
        times.append(t)
    warns = []
    if len(lines) < 3:
        warns.append("YouTube needs at least 3 chapters, found %d" % len(lines))
    ends = times[1:] + [total]
    for ln, a, b in zip(lines, times, ends):
        if b - a < 10:
            warns.append("chapter '%s' is shorter than 10 s" % ln)
    return warns


def speed_label(value: str) -> str:
    """Speed indicator text for a `speed` event value ("" = no indicator).

    "100" (normal speed), non-positive and unknown values give ""; "max"
    gives "<fast> MAX"; a percentage N gives "<fast> xF" above 100 % and
    "<slow> xF" below, F = N/100 without trailing zeros (e.g. "400" -> 4,
    "250" -> 2.5, "50" -> 0.5)."""
    if value == "max":
        return "%s MAX" % FAST_SYMBOL
    try:
        pct = int(value)
    except ValueError:
        return ""
    if pct <= 0 or pct == 100:
        return ""
    factor = ("%.2f" % (pct / 100.0)).rstrip("0").rstrip(".")
    return "%s %s%s" % (FAST_SYMBOL if pct > 100 else SLOW_SYMBOL, TIMES_SIGN, factor)


def state_timeline(sidecar: dict) -> List[dict]:
    """Emulator state for every frame of the recording, from the sidecar events.

    Returns consecutive non-empty intervals {start, end, paused, speed,
    timebase} covering [0, end of the last segment). The state before the
    first event is: not paused, speed "100", timebase "emulated". Events are
    applied in frame order (stable for the same frame, i.e. in file order);
    an event changes the state from its frame on. Kinds other than
    timebase / speed / pause_start / pause_end (snapshot, reset, unknown)
    do not change the state. Adjacent intervals with the same state are
    merged. Versions 1 and 2 (no events) give a single interval."""
    total = max(s["end"] for s in sidecar["segments"])
    state = {"paused": False, "speed": "100", "timebase": "emulated"}
    out: List[dict] = []
    pos = 0

    def emit(upto: int) -> None:
        upto = min(upto, total)
        if upto <= pos:
            return
        if out and out[-1]["end"] == pos and all(out[-1][k] == state[k] for k in state):
            out[-1]["end"] = upto
        else:
            out.append(dict(start=pos, end=upto, **state))

    for e in sorted(sidecar.get("events", []), key=lambda e: e["frame"]):
        f = max(0, e["frame"])
        emit(f)
        pos = max(pos, min(f, total))
        if e["kind"] == "timebase":
            state["timebase"] = e["value"]
        elif e["kind"] == "speed":
            state["speed"] = e["value"]
        elif e["kind"] == "pause_start":
            state["paused"] = True
        elif e["kind"] == "pause_end":
            state["paused"] = False
    emit(total)
    return out


def overlay_intervals(sidecar: dict) -> List[Tuple[int, int, bool, str]]:
    """Frame intervals that get a state indicator: (start, end, paused, text).

    `paused` = draw the pause icon. `text` = "real-time" while the timebase
    is realtime, followed by the speed indicator (speed_label()) when the
    speed is not 100 %. The speed is shown only in real time: in emulation
    time every emulated frame is one video frame, so the video plays at
    normal speed whatever the emulation speed was. A pause that produced no
    frames (emulation time, real-time "skip") has start == end and is not
    shown. Intervals without an icon and without text are left out;
    adjacent intervals with the same indicator are merged."""
    res: List[Tuple[int, int, bool, str]] = []
    for iv in state_timeline(sidecar):
        text = ""
        if iv["timebase"] == "realtime":
            parts = [REALTIME_TEXT]
            sp = speed_label(iv["speed"])
            if sp:
                parts.append(sp)
            text = "  ".join(parts)
        if not iv["paused"] and not text:
            continue
        if res and res[-1][1] == iv["start"] and res[-1][2:] == (iv["paused"], text):
            res[-1] = (res[-1][0], iv["end"], iv["paused"], text)
        else:
            res.append((iv["start"], iv["end"], iv["paused"], text))
    return res


def overlay_texts(sidecar: dict) -> List[str]:
    """Distinct non-empty indicator texts in the order of first use.

    run() writes each of them into a UTF-8 text file for drawtext
    (textfile=, so that the text needs no filtergraph escaping)."""
    seen: List[str] = []
    for _, _, _, text in overlay_intervals(sidecar):
        if text and text not in seen:
            seen.append(text)
    return seen


def overlay_geometry(tw: int, th: int) -> dict:
    """Layout of the state indicator in a tw x th output frame (top right corner).

    unit = icon size (th/30, min. 8 px), margin = distance from the frame
    edge, pad = border of the dark boxes, font = text size in px,
    icon = (x, y, w, h) of the pause icon slot, bars = the two (x, y, w, h)
    white bars of the pause icon, bg = (x, y, w, h) of the dark box behind
    the icon, text_right = {paused: right edge of the text} (the text moves
    left of the icon while paused)."""
    u = max(8, th // 30)
    m = u // 2
    pad = max(2, round(0.2 * u))
    x0, y0 = tw - m - u, m
    bw, bh = max(2, round(0.28 * u)), max(2, round(0.8 * u))
    by = y0 + round(0.1 * u)
    return {
        "unit": u, "margin": m, "pad": pad, "font": max(6, round(0.8 * u)),
        "icon": (x0, y0, u, u),
        "bars": [(x0 + round(0.15 * u), by, bw, bh), (x0 + round(0.57 * u), by, bw, bh)],
        "bg": (x0 - pad, y0 - pad, u + 2 * pad, u + 2 * pad),
        "text_right": {False: tw - m, True: x0 - 3 * pad},
    }


def overlay_filters(intervals: List[Tuple[int, int, bool, str]], seg: dict, fps: float,
                    tw: int, th: int, font: Optional[str],
                    textfiles: Dict[str, str]) -> List[str]:
    """Filters (drawbox / drawtext) burning the indicators into one segment.

    Each interval is clipped to the segment [start, end) and enabled by the
    segment-local time t (after trim + setpts): segment frames k0..k1-1 ->
    gte(t,(k0-0.5)/fps)*lt(t,(k1-0.5)/fps) (half a frame margin against
    rounding of the timestamps). The pause icon is a dark box + two white
    bars (drawbox, needs no font); the text is drawtext with
    `textfiles[text]` on a dark box. Raises ExportError when a text is
    needed and `font` is missing (or its text file is missing - internal
    error)."""
    g = overlay_geometry(tw, th)
    out: List[str] = []
    for a, b, paused, text in intervals:
        a, b = max(a, seg["start"]), min(b, seg["end"])
        if a >= b:
            continue
        en = "enable='gte(t,%.3f)*lt(t,%.3f)'" % (
            (a - seg["start"] - 0.5) / fps, (b - seg["start"] - 0.5) / fps)
        if paused:
            x, y, w, h = g["bg"]
            out.append("drawbox=x=%d:y=%d:w=%d:h=%d:color=black@0.6:t=fill:%s" % (x, y, w, h, en))
            for (x, y, w, h) in g["bars"]:
                out.append("drawbox=x=%d:y=%d:w=%d:h=%d:color=white:t=fill:%s" % (x, y, w, h, en))
        if text:
            if not font:
                raise ExportError("--state-overlay icons needs a font for the text indicators: "
                                  "pass --font FILE with the play symbol (e.g. "
                                  "C:/Windows/Fonts/seguisym.ttf or DejaVuSans.ttf)")
            if text not in textfiles:
                raise ExportError("internal: state overlay text file missing")
            out.append("drawtext=fontfile=%s:textfile=%s:expansion=none:fontcolor=white:"
                       "fontsize=%d:box=1:boxcolor=black@0.6:boxborderw=%d:"
                       "x=%d-text_w:y=%d:%s"
                       % (_esc_opt(font), _esc_opt(textfiles[text]), g["font"], g["pad"],
                          g["text_right"][paused], g["icon"][1] + round(0.1 * g["unit"]), en))
    return out


def _esc_opt(value: str) -> str:
    """Escape a filter option value (a path) and wrap it in apostrophes.

    Paths containing an apostrophe are not supported (ExportError)."""
    if "'" in value:
        raise ExportError("apostrophe in path is not supported: %s" % value)
    return "'%s'" % value.replace("\\", "/").replace(":", "\\:")


def build_filtergraph(sidecar: dict, opts: Options,
                      parts_txt: str = "parts.txt",
                      card_textfile: Optional[str] = None,
                      overlay_textfiles: Optional[Dict[str, str]] = None
                      ) -> Tuple[List[str], str, List[str]]:
    """Build the input arguments, `-filter_complex` and the output mappings.

    Input 0 = concat demuxer over `parts_txt`. Each segment is cut out
    (trim/atrim by global frame and sample indexes), processed (crop,
    nearest-neighbor scale, pad to the target, yuv420p) and joined according
    to its transition. The crop and scale follow the sidecar version and
    platform (frame_geometry(), is_line_doubled()), so version 1 and 2 of the
    same recording give the same picture and every platform gets its own
    canvas. Frame times use the fps of the sidecar (50 or 60).
    With opts.state_overlay == "icons" the state indicators from the sidecar
    events (overlay_intervals()) are drawn into each segment after scaling
    (in output pixels, before the segment fades); `overlay_textfiles` maps
    each text of overlay_texts() to a UTF-8 text file for drawtext.
    The title card (card) needs `card_textfile` only when card_text is
    given; a font is required with card_text. The fade (fade/card) of a
    segment shorter than D is shortened to the segment length.
    Raises ExportError for a missing font or invalid options."""
    plan = plan_timeline(sidecar, opts)
    segs = plan["segments"]
    n = len(segs)
    d = plan["d"]
    w, h = sidecar["width"], sidecar["height"]
    fps_n, fps_d = sidecar["fps_num"], sidecar["fps_den"]
    rate = sidecar["audio_rate"]
    spf = rate * fps_d / fps_n
    if abs(spf - round(spf)) > 1e-9:
        raise ExportError("audio samples per frame is not an integer")
    spf = int(round(spf))
    uses_card = any(s["trans"] == "card" for s in segs)
    if uses_card and opts.card_text and not opts.font:
        raise ExportError("card text needs a font: pass --font FILE "
                          "(e.g. C:/Windows/Fonts/consola.ttf or a DejaVuSans.ttf)")

    doubled = is_line_doubled(sidecar)
    cx, cy, cw, ch = sidecar_crop_rect(sidecar, opts.crop)
    sx, sy = integer_scale(cw, ch, opts.aspect, opts.target, doubled)
    tw, th = TARGETS[opts.target]
    vchain = ("crop=%d:%d:%d:%d,scale=iw*%d:ih*%d:flags=neighbor,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2,setsar=1,"
              "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,"
              "setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
              % (cw, ch, cx, cy, sx, sy, tw, th))
    achain = "aformat=sample_fmts=fltp:sample_rates=%d:channel_layouts=stereo" % rate

    overlay = overlay_intervals(sidecar) if opts.state_overlay == "icons" else []
    fps = fps_n / fps_d

    f = []
    if n > 1:
        f.append("[0:v]split=%d%s" % (n, "".join("[sv%d]" % i for i in range(n))))
        f.append("[0:a]asplit=%d%s" % (n, "".join("[sa%d]" % i for i in range(n))))
        vin = ["[sv%d]" % i for i in range(n)]
        ain = ["[sa%d]" % i for i in range(n)]
    else:
        vin, ain = ["[0:v]"], ["[0:a]"]

    def fade_next(i):
        return i + 1 < n and segs[i + 1]["trans"] in ("fade", "card")

    def fade_own(i):
        return segs[i]["trans"] in ("fade", "card")

    for i, s in enumerate(segs):
        vf = ["trim=start_frame=%d:end_frame=%d" % (s["start"], s["end"]),
              "setpts=PTS-STARTPTS", vchain]
        vf += overlay_filters(overlay, s, fps, tw, th, opts.font, overlay_textfiles or {})
        af = ["atrim=start_sample=%d:end_sample=%d" % (s["start"] * spf, s["end"] * spf),
              "asetpts=PTS-STARTPTS", achain]
        # A segment with a transition at one end only may be shorter than the
        # transition (seam shortly after the start / before the end): the fade
        # is shortened to the segment length. A transition at both ends is
        # guarded by plan_timeline (>= 2*D).
        fd = min(d, s["dur"])
        if fade_own(i):
            vf.append("fade=t=in:st=0:d=%.3f" % fd)
            af.append("afade=t=in:st=0:d=%.3f" % fd)
        if fade_next(i):
            vf.append("fade=t=out:st=%.3f:d=%.3f" % (s["dur"] - fd, fd))
            af.append("afade=t=out:st=%.3f:d=%.3f" % (s["dur"] - fd, fd))
        f.append("%s%s[v%d]" % (vin[i], ",".join(vf), i))
        f.append("%s%s[a%d]" % (ain[i], ",".join(af), i))

    cv, ca = "[v0]", "[a0]"
    cur = segs[0]["dur"]
    for i in range(1, n):
        tr = segs[i]["trans"]
        nv, na = "[cv%d]" % i, "[ca%d]" % i
        if tr == "crossfade":
            f.append("%s[v%d]xfade=transition=fade:duration=%.3f:offset=%.3f%s"
                     % (cv, i, d, cur - d, nv))
            f.append("%s[a%d]acrossfade=d=%.3f%s" % (ca, i, d, na))
            cur += segs[i]["dur"] - d
        elif tr == "card":
            card = "color=c=black:s=%dx%d:r=%d/%d:d=%.3f,setsar=1,format=yuv420p" % (
                tw, th, fps_n, fps_d, CARD_SECONDS)
            if opts.card_text:
                if not card_textfile:
                    raise ExportError("internal: card text file missing")
                card += (",drawtext=fontfile=%s:textfile=%s:expansion=none:"
                         "fontcolor=white:fontsize=h/12:x=(w-text_w)/2:y=(h-text_h)/2"
                         % (_esc_opt(opts.font), _esc_opt(card_textfile)))
            f.append("%s[cardv%d]" % (card, i))
            f.append("anullsrc=r=%d:cl=stereo:d=%.3f,%s[carda%d]" % (
                rate, CARD_SECONDS, achain, i))
            f.append("%s%s[cardv%d][carda%d][v%d][a%d]concat=n=3:v=1:a=1%s%s"
                     % (cv, ca, i, i, i, i, nv, na))
            cur += CARD_SECONDS + segs[i]["dur"]
        else:  # cut, fade
            f.append("%s%s[v%d][a%d]concat=n=2:v=1:a=1%s%s"
                     % (cv, ca, i, i, nv, na))
            cur += segs[i]["dur"]
        cv, ca = nv, na

    f.append("%snull[vout]" % cv)
    f.append("%sanull[aout]" % ca)
    inputs = ["-f", "concat", "-safe", "0", "-i", parts_txt]
    return inputs, ";".join(f), ["[vout]", "[aout]"]


def find_ffmpeg(explicit: Optional[str]) -> str:
    """Find ffmpeg: --ffmpeg, env VIDEOREC_FFMPEG, PATH; otherwise ExportError."""
    cand = explicit or os.environ.get("VIDEOREC_FFMPEG") or shutil.which("ffmpeg")
    if not cand:
        raise ExportError("ffmpeg not found (use --ffmpeg PATH or set VIDEOREC_FFMPEG)")
    return cand


def ffmpeg_has_filter(ffmpeg: str, name: str) -> bool:
    """True if `ffmpeg -filters` lists the filter `name` (False also when ffmpeg cannot run)."""
    try:
        out = subprocess.run([ffmpeg, "-hide_banner", "-filters"], capture_output=True,
                             text=True, errors="replace").stdout
    except OSError:
        return False
    return any(len(ln.split()) >= 2 and ln.split()[1] == name for ln in out.splitlines())


def run(argv: List[str]) -> int:
    """CLI entry point. Returns 0 on success, non-zero on error (message on stderr).

    Side effects: writes the MP4 and `<output without extension>.chapters.txt`,
    and a temporary parts.txt (plus the card text file) in a temporary
    directory; --dry-run does not run ffmpeg and does not write the chapters
    file. A --font path that is not an existing file is reported as an
    error before ffmpeg runs. With --state-overlay icons it also writes the indicator texts into
    the temporary directory. Before running ffmpeg it checks that ffmpeg has
    the drawtext filter when the filtergraph uses it (title card text, state
    overlay text) and reports a readable error otherwise, and that the file
    of every part with frames exists (empty parts are skipped, see
    used_parts)."""
    ap = argparse.ArgumentParser(description="Export emulator recording to YouTube-ready MP4.")
    ap.add_argument("sidecar", help="recording sidecar (*.cuts.json)")
    ap.add_argument("-o", "--output", required=True,
                    help="output MP4; chapters go to <output without extension>.chapters.txt")
    ap.add_argument("--target", choices=sorted(TARGETS), default="2160p")
    ap.add_argument("--crop", choices=("full", "reduced", "canvas"), default="full")
    ap.add_argument("--aspect", choices=("emulator", "tv43"), default="emulator")
    ap.add_argument("--transition", choices=("cut", "fade", "crossfade", "card"))
    ap.add_argument("--transition-ms", type=int)
    ap.add_argument("--card-text")
    ap.add_argument("--font", help="TTF font for --card-text")
    ap.add_argument("--crf", type=int, default=12)
    ap.add_argument("--state-overlay", choices=("none", "icons"), default="none",
                    help="burn emulator state indicators (pause, real-time, speed) "
                         "from the sidecar events into the picture")
    ap.add_argument("--ffmpeg")
    ap.add_argument("--dry-run", action="store_true", help="print the command only")
    a = ap.parse_args(argv)
    opts = Options(a.target, a.crop, a.aspect, a.transition, a.transition_ms,
                   a.card_text, a.font, a.crf, a.state_overlay)
    try:
        sc = load_sidecar(a.sidecar)
        if opts.font and not os.path.isfile(opts.font):
            raise ExportError("font file not found: %s" % opts.font)
        ffmpeg = find_ffmpeg(a.ffmpeg)
        base = os.path.dirname(os.path.abspath(a.sidecar))
        with tempfile.TemporaryDirectory(prefix="videorec_") as tmp:
            parts_txt = os.path.join(tmp, "parts.txt")
            card_txt = os.path.join(tmp, "card.txt")
            with open(parts_txt, "w", encoding="utf-8") as fh:
                fh.write(build_parts_list(sc, base))
            if opts.card_text:
                with open(card_txt, "w", encoding="utf-8") as fh:
                    fh.write(opts.card_text)
            ov_files = {}
            if opts.state_overlay == "icons":
                for i, text in enumerate(overlay_texts(sc)):
                    path = os.path.join(tmp, "overlay%d.txt" % i)
                    with open(path, "w", encoding="utf-8") as fh:
                        fh.write(text)
                    ov_files[text] = path.replace("\\", "/")
            inputs, fg, maps = build_filtergraph(
                sc, opts, parts_txt.replace("\\", "/"), card_txt.replace("\\", "/"), ov_files)
            if not a.dry_run and "drawtext=" in fg and not ffmpeg_has_filter(ffmpeg, "drawtext"):
                raise ExportError("this ffmpeg has no 'drawtext' filter (needed for --card-text "
                                  "and for the --state-overlay text); use an ffmpeg built with "
                                  "libfreetype, or export with --state-overlay none")
            check_parts_exist(sc, base)
            cmd =[ffmpeg, "-y", "-hide_banner"] + inputs + [
                "-filter_complex", fg,
                "-map", maps[0], "-map", maps[1],
                "-c:v", "libx264", "-profile:v", "high", "-crf", str(opts.crf),
                "-preset", "slow", "-pix_fmt", "yuv420p",
                "-colorspace", "bt709", "-color_primaries", "bt709",
                "-color_trc", "bt709", "-color_range", "tv",
                "-r", "%d/%d" % (sc["fps_num"], sc["fps_den"]),
                "-c:a", "aac", "-b:a", "320k", "-ar", str(sc["audio_rate"]),
                "-movflags", "+faststart", a.output]
            lines = chapter_lines(sc, opts)
            if a.dry_run:
                print(" ".join('"%s"' % c if (" " in c or ";" in c or "[" in c) else c
                               for c in cmd))
                print("\n".join(lines))
                return 0
            proc = subprocess.run(cmd)
            if proc.returncode != 0:
                print("ffmpeg failed with exit code %d" % proc.returncode, file=sys.stderr)
                return proc.returncode
        for wmsg in chapter_warnings(lines, plan_timeline(sc, opts)["total"]):
            print("warning: %s" % wmsg, file=sys.stderr)
        ch = os.path.splitext(a.output)[0] + ".chapters.txt"
        with open(ch, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        print("Wrote %s and %s" % (a.output, ch))
        return 0
    except ExportError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
