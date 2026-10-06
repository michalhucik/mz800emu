#!/usr/bin/env python3
"""Tests of the export script videorec_export.py.

Code comments and docstrings are in English (explicit exception from the
project rule of Czech comments, see the header of videorec_export.py).

Pure functions are tested without ffmpeg; the E2E tests are skipped when
ffmpeg is not available (PATH, VIDEOREC_FFMPEG variable).

The test lives in tests/tools/ (not next to the script): everything under
docs/ ships in `make dist`, so no test and no bytecode may end up there.
The script is imported from docs/tools/ via sys.path relative to this file;
writing bytecode is disabled so that no docs/tools/__pycache__ is created.
Registered in ctest as `videorec_export_py` (tests/tools/CMakeLists.txt).
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

# The imported script lives in docs/ (shipped in dist) - never write __pycache__ there.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir, "docs", "tools"))
import videorec_export as ve  # noqa: E402


def make_sidecar(segments, markers=(), parts=None, default="fade"):
    """Build a sidecar dict in the version 1 format (native 928x288 frame)."""
    return {
        "version": 1, "width": 928, "height": 288,
        "fps_num": 50, "fps_den": 1, "audio_rate": 48000,
        "default_transition": default, "transition_ms": 500,
        "parts": parts or [{"file": "a.avi", "first_frame": 0}],
        "segments": [{"start": s, "end": e, "transition_in": t}
                     for (s, e, t) in segments],
        "markers": [{"frame": f, "label": l} for (f, l) in markers],
    }


def make_sidecar_v2(segments, markers=(), parts=None, default="fade"):
    """Build a sidecar dict in the version 2 format (928x576 AVI with doubled lines)."""
    sc = make_sidecar(segments, markers, parts, default)
    sc.update({"version": 2, "height": 576, "line_doubled": True})
    return sc


def make_sidecar_v3(segments, markers=(), parts=None, default="fade", events=()):
    """Build a sidecar dict in the version 3 format (version 2 + state events)."""
    sc = make_sidecar_v2(segments, markers, parts, default)
    sc.update({"version": 3,
               "events": [{"frame": f, "kind": k, "value": v} for (f, k, v) in events]})
    return sc


#: Platform descriptions of sidecar version 4 as written by the emulators
#: (Task 25 recordings): AVI size, framebuffer, canvas and fps.
PLATFORMS_V4 = {
    "mz800": {"platform": "mz800", "tv_system": "pal", "width": 928, "height": 576,
              "framebuffer_width": 928, "framebuffer_height": 288,
              "canvas": {"x": 154, "y": 46, "width": 640, "height": 200}, "fps_num": 50},
    "mz700pal": {"platform": "mz700", "tv_system": "pal", "width": 704, "height": 464,
                 "framebuffer_width": 704, "framebuffer_height": 232,
                 "canvas": {"x": 32, "y": 16, "width": 640, "height": 200}, "fps_num": 50},
    "mz700ntsc": {"platform": "mz700", "tv_system": "ntsc", "width": 704, "height": 464,
                  "framebuffer_width": 704, "framebuffer_height": 232,
                  "canvas": {"x": 32, "y": 16, "width": 640, "height": 200}, "fps_num": 60},
    "mz1500": {"platform": "mz1500", "tv_system": "ntsc", "width": 704, "height": 464,
               "framebuffer_width": 704, "framebuffer_height": 232,
               "canvas": {"x": 32, "y": 16, "width": 640, "height": 200}, "fps_num": 60},
}


def make_sidecar_v4(platform, segments, markers=(), parts=None, default="fade", events=()):
    """Build a sidecar dict in the version 4 format (version 3 + platform description)."""
    sc = make_sidecar_v3(segments, markers, parts, default, events)
    sc.update({"version": 4, "line_doubled": True})
    sc.update(json.loads(json.dumps(PLATFORMS_V4[platform])))
    return sc


def find_ffmpeg():
    """Find ffmpeg (env VIDEOREC_FFMPEG, PATH, MSYS2 UCRT64)."""
    cand = os.environ.get("VIDEOREC_FFMPEG") or shutil.which("ffmpeg")
    if not cand and os.path.exists("C:/msys64/ucrt64/bin/ffmpeg.exe"):
        cand = "C:/msys64/ucrt64/bin/ffmpeg.exe"
    return cand


class PureTests(unittest.TestCase):
    def test_integer_scale_full_4k(self):
        self.assertEqual(ve.integer_scale(928, 288, "emulator", "2160p"), (3, 6))

    def test_integer_scale_canvas_1440p(self):
        self.assertEqual(ve.integer_scale(640, 200, "emulator", "1440p"), (3, 6))

    def test_integer_scale_tv43(self):
        self.assertEqual(ve.integer_scale(928, 288, "tv43", "2160p"), (2, 6))

    def test_crop_rect(self):
        self.assertEqual(ve.crop_rect("full", 928, 288), (0, 0, 928, 288))
        self.assertEqual(ve.crop_rect("canvas", 928, 288), (154, 46, 640, 200))
        self.assertEqual(ve.crop_rect("reduced", 928, 288), (122, 30, 704, 232))
        self.assertEqual(ve.crop_rect("canvas", 640, 200), (0, 0, 640, 200))

    def test_integer_scale_line_doubled(self):
        # v2: the lines are already doubled in the AVI -> scale (n, n), same result as v1
        self.assertEqual(ve.integer_scale(928, 576, "emulator", "2160p", True), (3, 3))
        self.assertEqual(ve.integer_scale(640, 400, "emulator", "1440p", True), (3, 3))
        self.assertEqual(ve.integer_scale(928, 576, "emulator", "1080p", True), (1, 1))
        self.assertEqual(ve.integer_scale(928, 288, "emulator", "1080p"), (1, 2))

    def test_integer_scale_tv43_line_doubled(self):
        # same resulting height (1728) and width as v1 (928*2 x 288*6)
        self.assertEqual(ve.integer_scale(928, 576, "tv43", "2160p", True), (2, 3))

    def test_crop_rect_line_doubled(self):
        self.assertEqual(ve.crop_rect("full", 928, 576, True), (0, 0, 928, 576))
        self.assertEqual(ve.crop_rect("canvas", 928, 576, True), (154, 92, 640, 400))
        self.assertEqual(ve.crop_rect("reduced", 928, 576, True), (122, 60, 704, 464))
        # unknown size -> whole frame
        self.assertEqual(ve.crop_rect("canvas", 640, 400, True), (0, 0, 640, 400))

    def test_load_sidecar_versions(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.cuts.json")
            for sc, doubled in ((make_sidecar([(0, 10, "none")]), False),
                                (make_sidecar_v2([(0, 10, "none")]), True),
                                (make_sidecar_v3([(0, 10, "none")]), True)):
                with open(p, "w") as f:
                    json.dump(sc, f)
                self.assertEqual(ve.is_line_doubled(ve.load_sidecar(p)), doubled)

    def test_load_sidecar_v3_events(self):
        # version 3: state events are loaded; older versions get an empty list
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.cuts.json")
            ev = ((0, "timebase", "realtime"), (25, "pause_start", "user"), (25, "pause_end", ""))
            with open(p, "w") as f:
                json.dump(make_sidecar_v3([(0, 10, "none")], events=ev), f)
            sc = ve.load_sidecar(p)
            self.assertEqual([(e["frame"], e["kind"], e["value"]) for e in sc["events"]], list(ev))
            with open(p, "w") as f:
                json.dump(make_sidecar_v2([(0, 10, "none")]), f)
            self.assertEqual(ve.load_sidecar(p)["events"], [])

    def test_load_sidecar_v3_bad_events_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.cuts.json")
            for bad in ({"frame": 1}, "x", {"frame": "1", "kind": "speed", "value": "max"}):
                sc = make_sidecar_v3([(0, 10, "none")])
                sc["events"] = [bad]
                with open(p, "w") as f:
                    json.dump(sc, f)
                with self.assertRaises(ve.ExportError):
                    ve.load_sidecar(p)

    def test_filtergraph_v3_same_as_v2(self):
        _, fg2, _ = ve.build_filtergraph(make_sidecar_v2([(0, 10, "none")]), ve.Options(crop="canvas"))
        _, fg3, _ = ve.build_filtergraph(
            make_sidecar_v3([(0, 10, "none")], events=((0, "speed", "max"),)), ve.Options(crop="canvas"))
        self.assertEqual(fg2, fg3)

    def test_filtergraph_v1_v2_same_output(self):
        # v1 and v2 give the same output picture: only the scale and crop differ
        for crop, v1, v2 in (
                ("full", "crop=928:288:0:0,scale=iw*3:ih*6", "crop=928:576:0:0,scale=iw*3:ih*3"),
                ("canvas", "crop=640:200:154:46,scale=iw*5:ih*10", "crop=640:400:154:92,scale=iw*5:ih*5"),
                ("reduced", "crop=704:232:122:30,scale=iw*4:ih*8", "crop=704:464:122:60,scale=iw*4:ih*4")):
            _, fg1, _ = ve.build_filtergraph(make_sidecar([(0, 10, "none")]), ve.Options(crop=crop))
            _, fg2, _ = ve.build_filtergraph(make_sidecar_v2([(0, 10, "none")]), ve.Options(crop=crop))
            self.assertIn(v1, fg1, crop)
            self.assertIn(v2, fg2, crop)

    def test_filtergraph_two_segments_fade(self):
        sc = make_sidecar([(0, 100, "none"), (100, 300, "fade")])
        _, fg, maps = ve.build_filtergraph(sc, ve.Options())
        for s in ("trim=start_frame=0:end_frame=100",
                  "trim=start_frame=100:end_frame=300",
                  "fade=t=out", "afade=t=in", "concat=n=2:v=1:a=1"):
            self.assertIn(s, fg)
        self.assertEqual(len(maps), 2)

    def test_filtergraph_crossfade_offset(self):
        sc = make_sidecar([(0, 100, "none"), (100, 300, "crossfade")])
        sc["transition_ms"] = 1000
        _, fg, _ = ve.build_filtergraph(sc, ve.Options())
        self.assertIn("xfade=transition=fade:duration=1.000:offset=1.000", fg)
        self.assertIn("acrossfade=d=1.000", fg)

    def test_card_requires_font_if_text(self):
        sc = make_sidecar([(0, 100, "none"), (100, 300, "card")])
        with self.assertRaises(ve.ExportError):
            ve.build_filtergraph(sc, ve.Options(card_text="Hi", font=None))

    def test_total_duration(self):
        segs = [(0, 100, "none"), (100, 300, "none")]
        for tr, expect in (("cut", 6.0), ("fade", 6.0),
                           ("crossfade", 5.5), ("card", 8.0)):
            sc = make_sidecar(segs)
            plan = ve.plan_timeline(sc, ve.Options(transition=tr))
            self.assertAlmostEqual(plan["total"], expect, msg=tr)

    def test_chapters(self):
        sc = make_sidecar([(0, 4000, "none")],
                          markers=[(250, "Level 2"), (3000, "Boss")])
        self.assertEqual(ve.chapter_lines(sc, ve.Options()),
                         ["00:00 Start", "00:05 Level 2", "01:00 Boss"])

    def test_chapters_after_cut(self):
        sc = make_sidecar([(0, 100, "none"), (500, 700, "none")],
                          markers=[(300, "gone"), (600, "Kept")])
        self.assertEqual(ve.chapter_lines(sc, ve.Options(transition="cut")),
                         ["00:00 Start", "00:04 Kept"])

    def test_short_middle_segment_rejected(self):
        for tr in ("fade", "crossfade", "card"):
            sc = make_sidecar([(0, 100, "none"), (100, 140, "none"), (140, 300, "none")])
            with self.assertRaises(ve.ExportError, msg=tr):
                ve.plan_timeline(sc, ve.Options(transition=tr, transition_ms=500))
        # a short segment at the end with a single transition is fine
        sc = make_sidecar([(0, 100, "none"), (100, 140, "none")])
        ve.plan_timeline(sc, ve.Options(transition="fade", transition_ms=500))

    def test_fade_clamped_to_short_segment(self):
        # A segment shorter than the transition with a transition at one end
        # only (seam shortly after the start or shortly before the end): the
        # fade is shortened to the segment length, a negative start must not
        # appear (ffmpeg: "Value -0.14 for parameter 'st'").
        sc = make_sidecar([(0, 18, "none"), (18, 23, "fade")])
        _, fg, _ = ve.build_filtergraph(sc, ve.Options())
        self.assertNotIn("st=-", fg)
        self.assertIn("fade=t=out:st=0.000:d=0.360", fg)
        self.assertIn("afade=t=out:st=0.000:d=0.360", fg)
        self.assertIn("fade=t=in:st=0:d=0.100", fg)
        self.assertIn("afade=t=in:st=0:d=0.100", fg)

    def test_chapter_warnings(self):
        self.assertEqual(ve.chapter_warnings(["00:00 Start", "00:30 A", "01:00 B"], 120), [])
        w = ve.chapter_warnings(["00:00 Start", "00:05 A"], 120)
        self.assertTrue(any("at least 3" in x for x in w))
        self.assertTrue(any("shorter than 10 s" in x and "Start" in x for x in w))
        w = ve.chapter_warnings(["00:00 Start", "00:30 A", "01:00 B"], 65)
        self.assertEqual(len(w), 1)
        self.assertIn("B", w[0])

    def test_fontfile_escaping(self):
        self.assertEqual(ve._esc_opt(r"C:\Windows\Fonts\consola.ttf"),
                         r"'C\:/Windows/Fonts/consola.ttf'")
        with self.assertRaises(ve.ExportError):
            ve._esc_opt("C:/it's.ttf")

    def test_rejects_unknown_version(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.cuts.json")
            with open(p, "w") as f:
                json.dump({"version": 5}, f)
            with self.assertRaises(ve.ExportError):
                ve.load_sidecar(p)

    def test_parts_list_escaping(self):
        sc = make_sidecar([(0, 1, "none")],
                          parts=[{"file": "it's.avi", "first_frame": 0}])
        txt = ve.build_parts_list(sc, "C:/rec")
        self.assertIn(r"file 'C:/rec/it'\''s.avi'", txt)

    def test_used_parts_skips_empty(self):
        # p3 has no frames (same first_frame as p4), p4 starts at the end of the
        # recording (retake to its first frame + stop): neither goes to concat
        sc = make_sidecar([(0, 100, "none"), (400, 600, "none")],
                          parts=[{"file": "p1.avi", "first_frame": 0},
                                 {"file": "p2.avi", "first_frame": 300},
                                 {"file": "p3.avi", "first_frame": 600},
                                 {"file": "p4.avi", "first_frame": 600}])
        self.assertEqual([p["file"] for p in ve.used_parts(sc)], ["p1.avi", "p2.avi"])
        txt = ve.build_parts_list(sc, "C:/rec")
        self.assertEqual(txt, "file 'C:/rec/p1.avi'\nfile 'C:/rec/p2.avi'\n")

    def test_used_parts_keeps_nonempty(self):
        sc = make_sidecar([(0, 100, "none"), (400, 550, "none")],
                          parts=[{"file": "p1.avi", "first_frame": 0},
                                 {"file": "p2.avi", "first_frame": 300}])
        self.assertEqual([p["file"] for p in ve.used_parts(sc)], ["p1.avi", "p2.avi"])

    def test_run_reports_missing_part(self):
        # a part with frames whose file is missing -> readable error, ffmpeg not run
        # (skipping it would shift the rest of the timeline)
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "p1.avi"), "wb").close()
            p = os.path.join(d, "x.cuts.json")
            with open(p, "w") as f:
                json.dump(make_sidecar([(0, 10, "none")],
                                       parts=[{"file": "p1.avi", "first_frame": 0},
                                              {"file": "gone.avi", "first_frame": 5}]), f)
            from io import StringIO
            err, old = StringIO(), sys.stderr
            sys.stderr = err
            try:
                rc = ve.run([p, "-o", os.path.join(d, "o.mp4"), "--ffmpeg", "no-such-ffmpeg"])
            finally:
                sys.stderr = old
            self.assertEqual(rc, 2)
            self.assertIn("gone.avi", err.getvalue())
            self.assertIn("not found", err.getvalue())


class StateOverlayTests(unittest.TestCase):
    """--state-overlay: state timeline from sidecar v3 events and its burn-in."""

    def test_timeline_defaults_without_events(self):
        # v1/v2 (no events) -> one interval with the default state, no overlay
        sc = make_sidecar_v2([(0, 100, "none")])
        tl = ve.state_timeline(sc)
        self.assertEqual(tl, [{"start": 0, "end": 100, "paused": False,
                               "speed": "100", "timebase": "emulated"}])
        self.assertEqual(ve.overlay_intervals(sc), [])

    def test_timeline_from_events(self):
        ev = ((0, "timebase", "realtime"), (0, "speed", "100"),
              (50, "pause_start", "user"), (80, "pause_end", ""),
              (100, "speed", "400"), (150, "speed", "max"), (170, "speed", "100"),
              (200, "timebase", "emulated"), (220, "speed", "400"))
        sc = make_sidecar_v3([(0, 300, "none")], events=ev)
        self.assertEqual(ve.overlay_intervals(sc), [
            (0, 50, False, "real-time"),
            (50, 80, True, "real-time"),
            (80, 100, False, "real-time"),
            (100, 150, False, "real-time  \u25b6\u25b6 \u00d74"),
            (150, 170, False, "real-time  \u25b6\u25b6 MAX"),
            (170, 200, False, "real-time"),
            # emulated time: the video plays at normal speed -> no speed icon
        ])

    def test_zero_length_pause_dropped(self):
        # pause in emulated time (or realtime skip) has start == end -> not shown
        ev = ((0, "timebase", "emulated"), (40, "pause_start", "user"), (40, "pause_end", ""),
              (60, "timebase", "realtime"), (90, "pause_start", "debugger"), (90, "pause_end", ""))
        sc = make_sidecar_v3([(0, 120, "none")], events=ev)
        self.assertEqual(ve.overlay_intervals(sc), [(60, 120, False, "real-time")])

    def test_pause_in_emulated_time_with_frames(self):
        # a pause with frames is shown in any time base (icon only)
        ev = ((10, "pause_start", "user"), (30, "pause_end", ""))
        sc = make_sidecar_v3([(0, 50, "none")], events=ev)
        self.assertEqual(ve.overlay_intervals(sc), [(10, 30, True, "")])

    def test_speed_labels(self):
        self.assertEqual(ve.speed_label("100"), "")
        self.assertEqual(ve.speed_label("400"), "\u25b6\u25b6 \u00d74")
        self.assertEqual(ve.speed_label("250"), "\u25b6\u25b6 \u00d72.5")
        self.assertEqual(ve.speed_label("50"), "\u25b6 \u00d70.5")
        self.assertEqual(ve.speed_label("max"), "\u25b6\u25b6 MAX")
        self.assertEqual(ve.speed_label("bogus"), "")

    def test_events_sorted_and_unknown_ignored(self):
        ev = ((20, "pause_end", ""), (5, "pause_start", "user"), (7, "reset", ""),
              (8, "snapshot", "seam"), (9, "future_kind", "x"))
        sc = make_sidecar_v3([(0, 30, "none")], events=ev)
        self.assertEqual(ve.overlay_intervals(sc), [(5, 20, True, "")])

    def test_filtergraph_none_unchanged(self):
        ev = ((0, "timebase", "realtime"), (10, "pause_start", "user"), (20, "pause_end", ""))
        sc = make_sidecar_v3([(0, 50, "none")], events=ev)
        _, fg_default, _ = ve.build_filtergraph(sc, ve.Options())
        _, fg_none, _ = ve.build_filtergraph(sc, ve.Options(state_overlay="none"))
        self.assertEqual(fg_default, fg_none)
        self.assertNotIn("drawbox", fg_none)
        self.assertNotIn("drawtext", fg_none)

    def test_filtergraph_icons(self):
        ev = ((0, "timebase", "realtime"), (60, "pause_start", "user"), (110, "pause_end", ""))
        sc = make_sidecar_v3([(0, 100, "none"), (100, 200, "fade")], events=ev)
        files = {"real-time": "C:/tmp/ov0.txt"}
        _, fg, _ = ve.build_filtergraph(
            sc, ve.Options(target="1080p", state_overlay="icons", font="C:/f.ttf"),
            overlay_textfiles=files)
        segs = fg.split(";")
        v0 = [s for s in segs if s.endswith("[v0]")][0]
        v1 = [s for s in segs if s.endswith("[v1]")][0]
        # segment 0 (frames 0..100): pause 60..100 -> local 1.19..1.99 s
        self.assertIn("drawbox", v0)
        self.assertIn("enable='gte(t,1.190)*lt(t,1.990)'", v0)
        self.assertIn("textfile='C\\:/tmp/ov0.txt'", v0)
        # segment 1 (frames 100..200): pause continues 100..110 -> local 0..0.19 s
        self.assertIn("enable='gte(t,-0.010)*lt(t,0.190)'", v1)
        # the overlay is drawn before the fade of the segment
        self.assertLess(v1.index("drawbox"), v1.index("fade=t=in"))

    def test_icons_pause_only_needs_no_font(self):
        ev = ((10, "pause_start", "user"), (30, "pause_end", ""))
        sc = make_sidecar_v3([(0, 50, "none")], events=ev)
        _, fg, _ = ve.build_filtergraph(sc, ve.Options(state_overlay="icons"))
        self.assertIn("drawbox", fg)
        self.assertNotIn("drawtext", fg)
        self.assertEqual(ve.overlay_texts(sc), [])

    def test_icons_text_needs_font(self):
        sc = make_sidecar_v3([(0, 50, "none")], events=((0, "timebase", "realtime"),))
        with self.assertRaises(ve.ExportError) as cm:
            ve.build_filtergraph(sc, ve.Options(state_overlay="icons"),
                                 overlay_textfiles={"real-time": "x.txt"})
        self.assertIn("--font", str(cm.exception))
        self.assertEqual(ve.overlay_texts(sc), ["real-time"])

    def test_icons_on_old_sidecar_no_error(self):
        # v1/v2 have no events: --state-overlay icons is accepted, nothing is drawn
        for sc in (make_sidecar([(0, 10, "none")]), make_sidecar_v2([(0, 10, "none")])):
            _, fg, _ = ve.build_filtergraph(sc, ve.Options(state_overlay="icons"))
            _, ref, _ = ve.build_filtergraph(sc, ve.Options())
            self.assertEqual(fg, ref)

    def test_run_reports_missing_drawtext(self):
        # an ffmpeg without drawtext -> readable error, exit code 2, ffmpeg not run
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.cuts.json")
            with open(p, "w") as f:
                json.dump(make_sidecar_v3([(0, 10, "none")], events=((0, "timebase", "realtime"),)), f)
            font = os.path.join(d, "f.ttf")
            with open(font, "wb") as f:
                f.write(b"x")
            orig = ve.ffmpeg_has_filter
            ve.ffmpeg_has_filter = lambda ffmpeg, name: False
            try:
                from io import StringIO
                err, old = StringIO(), sys.stderr
                sys.stderr = err
                try:
                    rc = ve.run([p, "-o", os.path.join(d, "o.mp4"), "--ffmpeg", "no-such-ffmpeg",
                                 "--state-overlay", "icons", "--font", font])
                finally:
                    sys.stderr = old
            finally:
                ve.ffmpeg_has_filter = orig
            self.assertEqual(rc, 2)
            self.assertIn("drawtext", err.getvalue())


    def test_run_reports_missing_font_file(self):
        # a --font path that does not exist -> readable error before ffmpeg runs
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.cuts.json")
            with open(p, "w") as f:
                json.dump(make_sidecar_v3([(0, 10, "none")], events=((0, "timebase", "realtime"),)), f)
            from io import StringIO
            err, old = StringIO(), sys.stderr
            sys.stderr = err
            try:
                rc = ve.run([p, "-o", os.path.join(d, "o.mp4"), "--ffmpeg", "no-such-ffmpeg",
                             "--state-overlay", "icons", "--font", os.path.join(d, "missing.ttf")])
            finally:
                sys.stderr = old
            self.assertEqual(rc, 2)
            self.assertIn("font file not found", err.getvalue())

    def test_timeline_after_retake(self):
        # sidecar after a retake to frame 1000: the emulator re-writes the state
        # valid from the snapshot point (speed 400 set in the discarded part
        # stays in effect), then the user switches to real time at 1100
        ev = ((0, "timebase", "emulated"), (0, "speed", "100"),
              (1000, "snapshot", "retake"), (1000, "timebase", "emulated"),
              (1000, "speed", "400"), (1100, "timebase", "realtime"))
        sc = make_sidecar_v3([(0, 1200, "none")], events=ev)
        self.assertEqual(ve.overlay_intervals(sc),
                         [(1100, 1200, False, "real-time  ▶▶ ×4")])


class PlatformV4Tests(unittest.TestCase):
    """Sidecar version 4: frame geometry and fps of MZ-700 PAL/NTSC, MZ-800 and MZ-1500."""

    def load(self, sc):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.cuts.json")
            with open(p, "w") as f:
                json.dump(sc, f)
            return ve.load_sidecar(p)

    def test_load_v4_all_platforms(self):
        for name, pf in PLATFORMS_V4.items():
            sc = self.load(make_sidecar_v4(name, [(0, 10, "none")]))
            self.assertTrue(ve.is_line_doubled(sc), name)
            g = ve.frame_geometry(sc)
            c = pf["canvas"]
            self.assertEqual(g["framebuffer"], (pf["framebuffer_width"], pf["framebuffer_height"]), name)
            self.assertEqual(g["canvas"], (c["x"], c["y"], c["width"], c["height"]), name)
            self.assertTrue(g["line_doubled"], name)

    def test_geometry_old_versions_are_mz800(self):
        # versions 1-3 have no platform description: always the MZ-800 frame
        for sc in (make_sidecar([(0, 10, "none")]), make_sidecar_v2([(0, 10, "none")]),
                   make_sidecar_v3([(0, 10, "none")])):
            g = ve.frame_geometry(sc)
            self.assertEqual(g["framebuffer"], (928, 288))
            self.assertEqual(g["canvas"], (154, 46, 640, 200))

    def test_v4_bad_geometry_rejected(self):
        bad = []
        sc = make_sidecar_v4("mz1500", [(0, 10, "none")])
        sc["canvas"]["x"] = 100                      # 100 + 640 > 704
        bad.append(sc)
        sc = make_sidecar_v4("mz1500", [(0, 10, "none")])
        del sc["canvas"]
        bad.append(sc)
        sc = make_sidecar_v4("mz1500", [(0, 10, "none")])
        sc["framebuffer_height"] = "232"
        bad.append(sc)
        sc = make_sidecar_v4("mz1500", [(0, 10, "none")])
        sc["height"] = 300                           # neither 232 nor 2 * 232
        bad.append(sc)
        for b in bad:
            with self.assertRaises(ve.ExportError):
                self.load(b)

    def test_crop_rect_mz700_mz1500(self):
        sc = make_sidecar_v4("mz1500", [(0, 10, "none")])
        self.assertEqual(ve.sidecar_crop_rect(sc, "full"), (0, 0, 704, 464))
        self.assertEqual(ve.sidecar_crop_rect(sc, "canvas"), (32, 32, 640, 400))
        # reduced = canvas + 32 px / 16 lines, clipped to the frame (MZ-700 border is that thin)
        self.assertEqual(ve.sidecar_crop_rect(sc, "reduced"), (0, 0, 704, 464))
        sc = make_sidecar_v4("mz800", [(0, 10, "none")])
        self.assertEqual(ve.sidecar_crop_rect(sc, "canvas"), (154, 92, 640, 400))
        self.assertEqual(ve.sidecar_crop_rect(sc, "reduced"), (122, 60, 704, 464))

    def test_crop_rect_generic_args(self):
        # crop_rect() with an explicit framebuffer/canvas (any platform)
        self.assertEqual(ve.crop_rect("canvas", 704, 232, False, (704, 232), (32, 16, 640, 200)),
                         (32, 16, 640, 200))
        self.assertEqual(ve.crop_rect("reduced", 704, 232, False, (704, 232), (32, 16, 640, 200)),
                         (0, 0, 704, 232))
        # an AVI of a different size than the framebuffer -> whole frame
        self.assertEqual(ve.crop_rect("canvas", 640, 400, True, (704, 232), (32, 16, 640, 200)),
                         (0, 0, 640, 400))

    def test_filtergraph_v4_mz800_same_as_v3(self):
        for crop in ("full", "reduced", "canvas"):
            _, fg3, _ = ve.build_filtergraph(make_sidecar_v3([(0, 10, "none")]), ve.Options(crop=crop))
            _, fg4, _ = ve.build_filtergraph(make_sidecar_v4("mz800", [(0, 10, "none")]),
                                             ve.Options(crop=crop))
            self.assertEqual(fg3, fg4, crop)

    def test_filtergraph_mz1500_60fps(self):
        sc = make_sidecar_v4("mz1500", [(0, 60, "none"), (90, 150, "fade")])
        _, fg, _ = ve.build_filtergraph(sc, ve.Options(crop="canvas"))
        # 640x400 into 3840x2160: n = min(6, 5) = 5 (square pixels after line doubling)
        self.assertIn("crop=640:400:32:32,scale=iw*5:ih*5", fg)
        # 48000 / 60 = 800 samples per frame
        self.assertIn("atrim=start_sample=72000:end_sample=120000", fg)
        self.assertIn("trim=start_frame=90:end_frame=150", fg)
        # fade of 0.5 s ends exactly at the end of the 1 s segment
        self.assertIn("fade=t=out:st=0.500:d=0.500", fg)
        plan = ve.plan_timeline(sc, ve.Options(transition="cut"))
        self.assertAlmostEqual(plan["total"], 2.0)

    def test_filtergraph_mz700_full_scale(self):
        sc = make_sidecar_v4("mz700ntsc", [(0, 10, "none")])
        _, fg, _ = ve.build_filtergraph(sc, ve.Options(crop="full", target="1080p"))
        self.assertIn("crop=704:464:0:0,scale=iw*2:ih*2", fg)
        _, fg, _ = ve.build_filtergraph(sc, ve.Options(crop="full", target="2160p"))
        self.assertIn("crop=704:464:0:0,scale=iw*4:ih*4", fg)

    def test_card_uses_platform_fps(self):
        sc = make_sidecar_v4("mz700ntsc", [(0, 100, "none"), (100, 300, "card")])
        _, fg, _ = ve.build_filtergraph(sc, ve.Options())
        self.assertIn("r=60/1", fg)

    def test_chapters_at_60fps(self):
        sc = make_sidecar_v4("mz1500", [(0, 4000, "none")], markers=[(300, "Level 2"), (3600, "Boss")])
        self.assertEqual(ve.chapter_lines(sc, ve.Options()), ["00:00 Start", "00:05 Level 2", "01:00 Boss"])

    def test_dry_run_60fps_output(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "a.avi"), "wb").close()
            p = os.path.join(d, "x.cuts.json")
            with open(p, "w") as f:
                json.dump(make_sidecar_v4("mz1500", [(0, 10, "none")]), f)
            from io import StringIO
            out, old = StringIO(), sys.stdout
            sys.stdout = out
            try:
                rc = ve.run([p, "-o", os.path.join(d, "o.mp4"), "--ffmpeg", "ffmpeg", "--dry-run"])
            finally:
                sys.stdout = old
            self.assertEqual(rc, 0)
            self.assertIn("-r 60/1", out.getvalue())


@unittest.skipUnless(find_ffmpeg(), "ffmpeg not available")
class E2ETests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ffmpeg = find_ffmpeg()
        cls.ffprobe = os.path.join(os.path.dirname(cls.ffmpeg),
                                   os.path.basename(cls.ffmpeg).replace("ffmpeg", "ffprobe"))
        if not os.path.exists(cls.ffprobe):
            cls.ffprobe = shutil.which("ffprobe")
        cls.tmp = tempfile.mkdtemp()
        for name, size, rate in (("p1.avi", "928x288", 50), ("p2.avi", "928x288", 50),
                                 ("d1.avi", "928x576", 50), ("n1.avi", "704x464", 60)):
            subprocess.run(
                [cls.ffmpeg, "-y", "-v", "error",
                 "-f", "lavfi", "-i", "testsrc2=s=%s:r=%d:d=6" % (size, rate),
                 "-f", "lavfi", "-i", "sine=f=440:r=48000:d=6",
                 "-ac", "2", "-c:v", "mpeg4", "-q:v", "2",
                 "-c:a", "pcm_s16le", os.path.join(cls.tmp, name)],
                check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def probe(self, path):
        out = subprocess.run(
            [self.ffprobe, "-v", "error", "-show_entries",
             "stream=codec_type,width,height,r_frame_rate:format=duration",
             "-of", "json", path], capture_output=True, text=True, check=True).stdout
        return json.loads(out)

    def export(self, transition, extra=(), sc=None):
        # two parts of 300 frames each, segments 0..100 and 400..550 (second part)
        if sc is None:
            sc = make_sidecar([(0, 100, "none"), (400, 550, "none")],
                              parts=[{"file": "p1.avi", "first_frame": 0},
                                     {"file": "p2.avi", "first_frame": 300}])
        side = os.path.join(self.tmp, "t.cuts.json")
        with open(side, "w") as f:
            json.dump(sc, f)
        out = os.path.join(self.tmp, "o_%s.mp4" % transition)
        rc = ve.run([side, "-o", out, "--target", "1080p", "--ffmpeg", self.ffmpeg,
                     "--transition", transition, "--transition-ms", "500"] + list(extra))
        self.assertEqual(rc, 0)
        return self.probe(out)

    def check(self, info, expected, rate="50/1"):
        v = [s for s in info["streams"] if s["codec_type"] == "video"][0]
        self.assertEqual((v["width"], v["height"]), (1920, 1080))
        self.assertEqual(v["r_frame_rate"], rate)
        self.assertAlmostEqual(float(info["format"]["duration"]), expected, delta=0.06)

    def test_e2e_fade(self):
        self.check(self.export("fade"), 5.0)

    def test_e2e_crossfade(self):
        self.check(self.export("crossfade"), 4.5)

    def test_e2e_cut(self):
        self.check(self.export("cut"), 5.0)

    def test_e2e_v2_line_doubled(self):
        # version 2: 928x576 AVI with doubled lines, canvas crop (640x400)
        sc = make_sidecar_v2([(0, 100, "none"), (150, 250, "none")],
                             parts=[{"file": "d1.avi", "first_frame": 0}])
        self.check(self.export("fade", ["--crop", "canvas"], sc), 4.0)

    def test_e2e_v4_mz1500_60fps(self):
        # version 4 (MZ-1500 / MZ-700 NTSC): 704x464 AVI at 60 fps, canvas crop,
        # 60 fps output; segments 0..120 and 180..300 = 4 s with a fade
        sc = make_sidecar_v4("mz1500", [(0, 120, "none"), (180, 300, "none")],
                             parts=[{"file": "n1.avi", "first_frame": 0}])
        self.check(self.export("fade", ["--crop", "canvas"], sc), 4.0, "60/1")

    def test_e2e_empty_part_missing(self):
        # empty parts whose files are gone: one in the middle (no frames - same
        # first_frame as the next part) and a trailing one listed by an older
        # emulator (retake to its first frame + stop); the export ignores both
        sc = make_sidecar([(0, 100, "none"), (400, 600, "none")],
                          parts=[{"file": "p1.avi", "first_frame": 0},
                                 {"file": "gone_002.avi", "first_frame": 300},
                                 {"file": "p2.avi", "first_frame": 300},
                                 {"file": "gone_004.avi", "first_frame": 600}])
        self.check(self.export("fade", sc=sc), 6.0)

    def test_e2e_card(self):
        font = "C:/Windows/Fonts/consola.ttf"
        if not os.path.exists(font):
            self.skipTest("no font")
        self.check(self.export("card", ["--card-text", "Level 2: it's 100%", "--font", font]), 7.0)

    def frame_rgb(self, path, t):
        """Decode one output frame at time t as raw RGB24 bytes (1920x1080)."""
        return subprocess.run(
            [self.ffmpeg, "-v", "error", "-ss", "%.3f" % t, "-i", path, "-frames:v", "1",
             "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
            capture_output=True, check=True).stdout

    @staticmethod
    def region_mean(rgb, x0, y0, x1, y1, width=1920):
        """Mean value of all RGB components in the rectangle [x0,x1) x [y0,y1)."""
        total, count = 0, 0
        for y in range(y0, y1):
            row = rgb[(y * width + x0) * 3:(y * width + x1) * 3]
            total += sum(row)
            count += len(row)
        return total / count

    def test_e2e_state_overlay_icons(self):
        # sidecar v3: pause with frozen picture at frames 50..150, real-time from 200
        font = "C:/Windows/Fonts/seguisym.ttf"
        if not os.path.exists(font):
            self.skipTest("no font with the play symbol")
        ev = ((0, "timebase", "emulated"), (0, "speed", "100"),
              (50, "pause_start", "user"), (150, "pause_end", ""),
              (200, "timebase", "realtime"))
        sc = make_sidecar_v3([(0, 250, "none")], parts=[{"file": "d1.avi", "first_frame": 0}],
                             events=ev)
        side = os.path.join(self.tmp, "ov.cuts.json")
        with open(side, "w") as f:
            json.dump(sc, f)
        out = os.path.join(self.tmp, "o_overlay.mp4")
        rc = ve.run([side, "-o", out, "--target", "1080p", "--ffmpeg", self.ffmpeg,
                     "--state-overlay", "icons", "--font", font])
        self.assertEqual(rc, 0)
        self.check(self.probe(out), 5.0)
        g = ve.overlay_geometry(1920, 1080)
        bx, by, bw, bh = g["bars"][0]
        bar = (bx + 1, by + 1, bx + bw - 1, by + bh - 1)
        # the corner of the 1080p output is black padding: pause bars only while paused
        self.assertGreater(self.region_mean(self.frame_rgb(out, 2.0), *bar), 200)
        self.assertLess(self.region_mean(self.frame_rgb(out, 0.5), *bar), 40)
        self.assertLess(self.region_mean(self.frame_rgb(out, 3.5), *bar), 40)
        # "real-time" text (with its box) appears in the top right corner from frame 200
        tx = (1920 - g["margin"] - 8 * g["unit"], g["margin"], 1920 - g["margin"], g["margin"] + g["unit"])
        self.assertGreater(self.region_mean(self.frame_rgb(out, 4.5), *tx),
                           self.region_mean(self.frame_rgb(out, 3.5), *tx) + 5)


if __name__ == "__main__":
    unittest.main()
