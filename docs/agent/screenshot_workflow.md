# Screenshot workflow (AI saves and compares pictures)

Recipe for an AI client that needs a picture of the emulator screen: to
check what a program draws, to compare it with an expected picture, to
cut out one part of the screen, or to save every changed frame of an
animation (frame series).

Use `emu_screenshot_save_to_file`. It writes a PNG straight to a file on
the server host, so no large base64 payload goes through the context; read
the written file afterwards. (`emulator://frame/screenshot` returns the
whole framebuffer as a base64 PNG, only for small checks. A client talking
JSONL directly can pass the same `area`, `scale` and `crop` to the
`get_frame_screenshot` command.)

## Geometry

The emulator framebuffer has **one pixel per GDG clock horizontally and
one per picture line vertically**. The picture area (canvas) is 640
framebuffer pixels wide, so in the 320-dot modes one dot of the computer
is two framebuffer pixels side by side.

| Build | Framebuffer (with border) | Canvas (picture only) | Canvas offset |
|-------|---------------------------|-----------------------|---------------|
| MZ-800 | 928x288 | 640x200 | 154, 46 |
| MZ-700 PAL / NTSC | 704x232 | 640x200 | 32, 16 |
| MZ-1500 | 704x232 | 640x200 | 32, 16 |

## Parameters

| Parameter | Values | Meaning |
|-----------|--------|---------|
| `path` | file path | PNG to write (absolute path recommended) |
| `area` | `full` (default), `canvas` | whole frame with the border, or the picture only |
| `scale` | `framebuffer` (default), `native`, `"1"` to `"4"` | see below |
| `crop` | `{"x", "y", "width", "height"}` | cut-out in output pixels, only with `framebuffer` or `native` |

- `framebuffer` - framebuffer pixels 1:1 (MZ-800 full 928x288, canvas
  640x200). The behaviour without parameters.
- `native` - **one image pixel per dot of the computer**. When all pairs
  of neighbouring framebuffer pixels in the area are equal (320-dot
  modes), the width is halved: MZ-800 canvas 320x200, full 464x288;
  otherwise the width is kept (640x200, 928x288). Lines 1:1. The reply
  has `hires: true` when the 640 width was kept. Use this to compare with
  pixel data of the program (sprites, VRAM contents, a converted image).
- `"1"` to `"4"` - the aspect ratio of the emulator window; `"2"` equals
  the window at 100 % (MZ-800 full 928x576). For a human-looking picture.
  Scale `"1"` drops every second column in the 640-dot modes.

The reply: `{"available": true, "path", "format": "png", "width",
"height", "byte_size", "area", "scale"}` (+ `hires` with `native`).
`available: false` + `reason` = no frame rendered yet (just after start).
An unknown value or a crop outside the image is an error; no file is
written.

## Recipes

**Check what the program drew (MZ-800 graphics):**

```python
emu_run(frames=50)
emu_screenshot_save_to_file(path="C:/tmp/screen.png", area="canvas", scale="native")
# -> 320x200 (or 640x200 in the 640-dot modes); one pixel = one dot
```

**Only a part of the screen**, e.g. a 256x192 window in the middle of a
320x200 picture (a ZX Spectrum style playfield):

```python
emu_screenshot_save_to_file(path="C:/tmp/field.png", area="canvas", scale="native",
                            crop={"x": 32, "y": 4, "width": 256, "height": 192})
```

**A picture for a person** (looks like the emulator window):

```python
emu_screenshot_save_to_file(path="C:/tmp/shot.png", scale="2")
```

## Frame series - finding animation bugs

`emu_screenshot_series_start` saves a PNG of **every emulated frame that
differs from the last saved one**. Use it when something flickers, jumps
or tears in an animation and you need to see the exact frames.

| Tool | What it does |
|------|--------------|
| `emu_screenshot_series_start(dir="", area="full", scale="native", crop=None, only_changed=True, frames=0, max_files=0)` | Starts the series. Empty `dir` = a new `<platform>_frames_YYYYMMDD_HHMMSS` folder in the screenshot output folder. `frames` > 0 = stop after that many emulated frames. `max_files` = safety limit (0 = 10000). |
| `emu_screenshot_series_stop(wait=True)` | Stops; with `wait` returns after all queued frames are written. |
| `emu_screenshot_series_status()` | `active`, `writing`, `frames_seen`, `files`, `unchanged`, `dir`, `stop_reason` (`Stopped`, `Frame count reached`, `File limit reached`, `Error`), `last_error`. |

How it works:

- Frames are taken in **emulated time** at the end of every frame, so
  none is skipped even at MAX SPEED. When writing cannot keep up, the
  emulation waits. Nothing is captured while the emulation is paused.
- "Changed" is decided on the **resulting image**: only differences
  inside `area` and `crop` count. Crop to the sprite you follow and the
  rest of the screen (score, border, blinking cursor) is ignored.
- Files `frame_NNNNNNN.png`: NNNNNNN = emulated frame since the start of
  the series. Gaps = frames that did not change. `index.csv` has
  `frame,file,gdg_screen` (`gdg_screen` = absolute GDG frame counter).
- Only one series at a time; a second start is an error.

**Pausing in the middle of a frame** (a breakpoint, `emu_pause`, stepping
to a beam position) completes the picture with the machine state at the
pause: the rows the beam has already passed are drawn again with the
palette, mode and VRAM of that moment. When the run continues, the frame
that then ends keeps those rows as of the pause, so a program that changes
the palette or VRAM during the frame can show a picture that a continuous
run does not. For the picture
exactly as the machine draws it, take a frame series, or compare frames
reached by `emu_run(frames=N)` from a frame boundary without pausing in
between.

**Recipe - capture 3 seconds of an animation exactly:**

```python
emu_pause()                                     # at the scene to examine
emu_screenshot_series_start(dir="C:/tmp/anim", area="canvas", frames=150)
emu_run(frames=150)                             # 150 emulated frames = 3 s PAL
st = emu_screenshot_series_status()             # wait until active and writing are false
# read C:/tmp/anim/index.csv, open the frame_*.png files in order
```

With `only_changed=False` every frame is saved (to see how long a state
lasts).

## Related state

- `emulator://periph/gdg` - display mode (`regDMD`), palette, border; on
  the MZ-800 also `vramctrl` (VRAM write / read format WF / RF: write mode,
  planes, VBANK) and `hwscroll` (hardware scroll SOF, SSA, SEA, SW in port
  units, `enabled`). Useful when the picture does not match the expected
  VRAM contents.
- `emu_run(frames=N)` - advance exactly N emulated frames before taking
  the screenshot.
