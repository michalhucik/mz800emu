
# Planned features

- debugger Step variants (Scanline step, until ROM exit, until SP unwind, ...)
- VRAM viewer, especially for MZ-800 with the ability to switch display modes and modify palettes
- MZ-800: CG-RAM editor
- MZ-1500: PCG viewer - overview of all characters of a PCG bank at once (the single-character editor is already part of the Memory Browser)
- LUA scripting
- dynamic IORQ bus, plug-and-play
- ability to attach external modules/libraries to IORQ
- Reference search (where exactly is this read/written)
- Back step
- complete overscreen menu
- conversion of WAV recordings in non-standard tape formats (TURBO, FASTIPL, BSD, CP/M) to files, and conversion while the tape is playing
- gdb support


## Emulation

- Re-measure on real HW: `VIDEO_H_BACK_PORCH_TICKS`, `VIDEO_H_FRONT_PORCH_TICKS` (currently 104+39=143, but the exact split is not verified)
- Consider emulating partially populated VRAM on MZ-800
- MZ-1500 gdg: verify absence of VRAM latch


# Bugs awaiting a fix

- **topmenu**: When the user uses arrow keys for navigation, they are picked up as keyboard input to the emulation (consider whether this behavior can be suppressed)
- **MZ-800 emulation bug**: For a very long time there has been a feeling that when the Flappy demo is playing, the music tempo speeds up during HW scroll. That would suggest that the GDG has some additional undocumented state causing CPU WAIT. This needs to be verified by measurement on real HW.
