# MZ-800 emulator Hot-Keys

Global keyboard shortcuts available across the entire emulator window.

## Reset and general

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `F12`                      | Emulated computer reset                                                     |
| `F11`                      | Joystick calibration (or Reset when emulator runs under Windows Debugger)   |
| `F10`                      | Open / close the overlay menu                                               |

## Overlay menu

A full-screen menu over the emulator picture; `Alt` + left mouse button over
the picture opens it as well. While it is open, the emulated computer
receives no keys and only those global shortcuts work that the menu offers
itself (`F12`, `Alt + P`, `Alt + N`, `Alt + M`, `Alt + [1..4]`,
`Alt + Shift + [1..4]`, `Alt + C`, `Alt + K`, `Alt + F6` to `Alt + F9`,
`Alt + F`, `Alt + Shift + F`, `Alt + O`, `Alt + Shift + O`, `Alt + U`,
`Alt + J`, `Alt + Ctrl + Enter`, `Alt + W`, `Alt + H`). For details see
[`overlay-menu.md`](overlay-menu.md).

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `F10`, `Esc`               | Close the overlay menu                                                      |
| `1` to `6`                 | Switch the tab: Media, Machine, Display, Sound, Input, Capture              |
| `Ctrl + Tab`               | Next tab                                                                    |
| `Ctrl + Shift + Tab`       | Previous tab                                                                |
| arrows                     | Move between the controls                                                   |
| `Enter`, space             | Select the control                                                          |

In the media exchange sheet (the buttons Change, Insert disk, Change tape on
the Media tab):

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `1` to `6`                 | Insert the recently used file with that number (while the filter is empty)  |
| typing, `Backspace`        | Filter the list by text                                                     |
| `Enter`                    | Insert the selected file                                                    |
| `Esc`                      | Close the sheet (the menu stays open)                                       |
| `F10`                      | Close the whole menu                                                        |

## Window and display

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + Ctrl + Enter`       | Toggle fullscreen / windowed mode                                           |
| `Alt + W`                  | Fix window aspect ratio by Width                                            |
| `Alt + H`                  | Fix window aspect ratio by Height                                           |

## Emulation speed

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + P`                  | Toggle Pause / Resume                                                       |
| `Alt + N`                  | Set Normal emulation speed (100 %)                                          |
| `Alt + M`                  | Switch between Max and (Normal or Custom) emulation speed                   |
| `Alt + Shift + M`          | Switch between Custom and (Normal or Max) emulation speed                   |
| `Alt + Up`                 | Increase Custom speed (step 1 % of Normal speed)                            |
| `Alt + Shift + Up`         | Increase Custom speed (step 10 % of Normal speed)                           |
| `Alt + Pg_Up`              | Increase Custom speed (step 100 % of Normal speed)                          |
| `Alt + Down`               | Decrease Custom speed (step 1 % of Normal speed)                            |
| `Alt + Shift + Down`       | Decrease Custom speed (step 10 % of Normal speed)                           |
| `Alt + Pg_Down`            | Decrease Custom speed (step 100 % of Normal speed)                          |
| `Alt + T`                  | MAX SPEED benchmark: print the emulation efficiency report to the console   |
| `Alt + Shift + T`          | MAX SPEED benchmark: reset the measurement                                  |

## Floppy disk (FDC)

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + [1..4]`             | Mount FD image into drive 1..4 of FDC0 (and enable WD279x)                  |
| `Alt + Shift + [1..4]`     | Eject FD image from drive 1..4 of FDC0                                      |

> Note: these hotkeys always operate on the primary controller **FDC0 (standard)**, ports 0xD8 - 0xDF. The secondary controller **FDC1** (ports 0x58 - 0x5F) is operated from the HW & Devices -> FD Controller menu.

## Cassette (CMT) and virtual keyboard

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + C`                  | Show / Hide Virtual CMT window                                              |
| `Alt + K`                  | Show / Hide Virtual Keyboard window                                         |

## Snapshots

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + F6`                 | Save Snapshot (open save dialog)                                            |
| `Alt + F7`                 | Load Snapshot (open load dialog)                                            |
| `Alt + F8`                 | Quick Save                                                                  |
| `Alt + F9`                 | Quick Load                                                                  |

## Video recording

All platforms. For details see [`video-recording.md`](video-recording.md).

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + O`                  | Start / stop video recording                                                |
| `Alt + Shift + O`          | Pause / resume recording (emulation keeps running)                          |
| `Alt + L`                  | Add a marker to the recording                                               |
| `Alt + Shift + L`          | Show / hide the recording remote control window                             |
| `Alt + U`                  | Switch the recording time base: emulated time / real time (also while recording) |

## Screenshot

All platforms. Area, scale and output folder are set in Tools -> Screenshot ->
Settings...; Tools -> Screenshot -> Save Screenshot As... asks for a file name. For details see
[`screenshots.md`](screenshots.md).

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + F`                  | Save a PNG screenshot of the picture shown in the window to the output folder |
| `Alt + Shift + F`          | Start / stop a frame series: a PNG of every changed emulated frame         |
| `Alt + G`                  | Show / hide the Screenshot Control window                                   |

## Audio capture

All platforms. The files to write are selected in Tools -> Audio Capture ->
Settings... For details see [`audio-capture.md`](audio-capture.md).

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + Q`                  | Start / stop an audio capture into a new folder                             |
| `Alt + Shift + Q`          | Show / hide the Audio Capture Control window                                |

## Replay (input recording)

All platforms. For details see [`replay.md`](replay.md).

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + J`                  | Show / hide the Replay Control window                                       |
| `Alt + Shift + J`          | Rewind: back to the previous checkpoint (while recording or playing back)   |

## Debugger

Available only in builds with `MZ800EMU_CFG_DEBUGGER_ENABLED`.

Several keys are dual purpose: pressed without `Shift` they perform their
non-debugger action (see above), pressed with `Shift` they toggle a debugger
window (e.g. `Alt + W` fixes aspect ratio, `Alt + Shift + W` toggles Watch).

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + D`                  | Show / Hide MZ-800 Debugger window                                          |
| `Alt + Shift + D`          | Show / Hide Disassembler window (range-based + export)                      |
| `Alt + B`                  | Show / Hide Breakpoints window                                              |
| `Alt + Shift + B`          | Show / Hide Bookmarks window (named address bookmarks)                      |
| `Alt + V`                  | Show / Hide Variables window                                                |
| `Alt + I`                  | Show / Hide I/O Ports window                                                |
| `Alt + E`                  | Show / Hide Memory Browser window                                           |
| `Alt + Y`                  | Show / Hide Symbols window (NoICE / sdldz80 `.map` / sjasmplus `.sym`)      |
| `Alt + Shift + R`          | Show / Hide CPU Registers window                                           |
| `Alt + S`                  | Show / Hide Stack Monitor window                                            |
| `Alt + Shift + S`          | Show / Hide Stack Regions window                                            |
| `Alt + Shift + H`          | Show / Hide Stack History window                                            |
| `Alt + Shift + W`          | Show / Hide Watch window (user-defined memory watches)                      |
| `Alt + Shift + P`          | Show / Hide CPU Profiler window                                             |

## Chip / hardware state windows

Per-chip "F1" state windows. Available only in builds with
`MZ800EMU_CFG_DEBUGGER_ENABLED`.

| Key                        | Action                                                                      |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + Shift + I`          | Show / Hide PPI 8255 State window                                           |
| `Alt + Shift + C`          | Show / Hide CTC 8253 State window                                           |
| `Alt + Shift + V`          | Show / Hide GDG State window                                                |
| `Alt + Shift + Z`          | Show / Hide Z80 PIO State window (MZ-800 / MZ-1500 only)                    |
| `Alt + Shift + G`          | Show / Hide PSG State window (MZ-800 / MZ-1500 only)                        |
| `Alt + Shift + A`          | Show / Hide PSG Audio Scope window (MZ-800 / MZ-1500 only)                  |
