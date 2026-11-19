
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
- gdb support


## Emulation

- Re-measure on real HW: `VIDEO_H_BACK_PORCH_TICKS`, `VIDEO_H_FRONT_PORCH_TICKS` (currently 104+39=143, but the exact split is not verified)
- Consider emulating partially populated VRAM on MZ-800
- MZ-1500 gdg: verify absence of VRAM latch

### 8253 counter: known differences from the real computer

The counter is fixed according to measurements with a logic analyzer on an MZ-800. Two cases still behave differently from the real chip:

- **Reading a running counter without a latch command.** A program can read the counter in two ways: send a latch command first (the value is frozen and then read at leisure), or read it directly while it runs. On the real computer a direct read sometimes returns garbage: CTC0 changes 1.1 million times a second, the read pulse lasts 550 ns, the value often changes in the middle of the read and some bits come from the old value and some from the new one (for example F8h was measured instead of FBh). The emulator always returns a clean value. The rule for when and how the value breaks up could not be derived from the measurements: a broken value appears even when no clock edge falls into the read, and only some of them are a mix of the old and the new value. Consequence: a program that reads without latching runs more reliably in the emulator than on the real computer. Programs that use the latch command do not differ. A measurement including the data bus would be needed to refine this.
- **A two-byte divider interrupted by a control word for another counter.** A 16-bit divider is written in two steps, the low byte and then the high byte. When on the real computer a control word for another counter comes between the two bytes (measured: divider of CTC0, control word 54h for CTC1), the low byte is lost: instead of the written divider 0020h, CTC0 started with a divider between 13 and 255, a different one in every repetition. A latch command for the same counter between the bytes does no harm. The emulator loads the divider correctly, as if there were no control word between the bytes. What the chip replaces the low byte with is not known, and the value looks random. Consequence: a program with this bug would play a wrong tone or have wrong timing on the real computer; in the emulator it runs correctly.


# Bugs awaiting a fix

- **topmenu**: When the user uses arrow keys for navigation, they are picked up as keyboard input to the emulation (consider whether this behavior can be suppressed)
