# Screenshots

PNG pictures of the emulator screen on all platforms (MZ-800, MZ-700 PAL
and NTSC, MZ-1500). Menu **Tools -> Screenshot**:

| Item | Key | What it does |
|------|-----|--------------|
| Save Screenshot | `Alt + F` | Saves a PNG of the picture shown in the window into the output folder, named `<platform>_YYYYMMDD_HHMMSS.png` (e.g. `mz800_20261013_183005.png`). A notification shows the size and the path. |
| Save Screenshot As... | | The same, but asks for the file name. |
| Record Changed Frames | `Alt + Shift + F` | Starts / stops a frame series (see below). |
| Screenshot Control... | `Alt + G` | Shows / hides the Screenshot Control window (see below). |
| Settings... | | Area, scale and output folder (see below). |
| Open Output Folder | | Opens the output folder in the file manager. |

The screenshot is exactly the picture the window shows at that moment, without
the CRT scanlines (menu **Display -> CRT Scanlines**), which only darken the
picture in the window.

**Screenshot Control** is a floating window that can stay open while you
play: it shows the area, the scale, the output folder, the last
screenshot (file and size, or the error) and the frame series (running:
files and frames so far; after the end: the result and its folder). Its
buttons save a screenshot, save it under a name, start or stop the frame
series, open the output folder and the settings; the keys are shown next
to the buttons. The window opens again after a restart of the emulator
if it was open.

## Settings

Menu **Tools -> Screenshot -> Settings...**, saved in the configuration
(section `[SCREENSHOT]`):

| Setting | Values | INI key |
|---------|--------|---------|
| Area | Whole frame with border / Picture without border | `area` = `full` / `canvas` |
| Scale | Native / 1x / 2x / 3x / 4x | `scale` = `native` / `1` .. `4` (default `2`) |
| Output folder | empty = the `screenshots` folder next to the emulator | `output_dir` |

- **Native** - one image pixel per dot of the computer: the MZ-800 picture
  is 320x200 (640x200 in the 640-dot modes), the whole frame with the
  border 464x288 (928x288). Good for comparing with graphics data.
- **1x to 4x** - the aspect ratio of the emulator window; **2x** is the
  window at 100 % (MZ-800 whole frame 928x576, picture 640x400). 1x drops
  every second column in the 640-dot modes.

The dialog shows the resulting image size.

## Frame series

For finding animation bugs: **Record Changed Frames** (`Alt + Shift + F`)
saves a PNG of every emulated frame that differs from the previous one,
until you press it again. Area and scale are those from Settings.

- Frames are taken in emulated time, so none is skipped even at MAX
  SPEED; when the disk cannot keep up, the emulation slows down.
- Files go to a new folder `<platform>_frames_YYYYMMDD_HHMMSS` in the
  output folder: `frame_NNNNNNN.png`, where NNNNNNN is the emulated frame
  since the start (gaps = frames that did not change), and `index.csv`.
- While the series runs, a counter `FRAMES files / frames` is shown in the
  top left corner of the picture; when it ends, a notification shows the
  folder.
- Settings... -> **Frame series: only changed frames** (INI key
  `series_only_changed`, default on). Unchecked = every frame.

## MCP

AI clients save screenshots with `emu_screenshot_save_to_file` (same
area and scale, plus a crop) and frame series with
`emu_screenshot_series_start` / `_stop` / `_status`, see
[mcp-server/headless-mode.md](mcp-server/headless-mode.md) and the MCP
document `emulator://docs/screenshot_workflow`.
