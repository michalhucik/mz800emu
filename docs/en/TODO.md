
# Planned features

- debugger: more stepping variants in the debugger window - to the next display line, until leaving the ROM, until return from a subroutine (MCP already has emu_step_out and emu_run_until_raster)
- VRAM viewer, especially for MZ-800 with the ability to switch display modes and modify palettes
- MZ-800: CG-RAM editor
- MZ-1500: PCG viewer - overview of all characters of a PCG bank at once (the single-character editor is already part of the Memory Browser)
- LUA scripting
- expansion unit bus (so far MZ-700 only) for MZ-800 and MZ-1500 as well
- ability to attach external modules/libraries to IORQ
- Reference search: everywhere an address is read (writes are already shown by the RAM Write Log)
- Back step: step back by instructions in the debugger (going back by frames is possible with Replay)
- overlay menu: the remaining hardware settings as well
- gdb support


## Emulation

- MZ-700 NTSC (Japanese version): the position of the picture in the line, the wait on VRAM access and the CTC0 counter input are taken from the MZ-1500, which has the same crystal, and the wait state on reads from the monitor ROM from the European MZ-700; they are not measured on a Japanese MZ-700.
- Exact clock, remaining parts: the emulation pace, live audio, audio capture and the readouts already follow the crystal of the real computer (PAL 50.036 frames per second, NTSC 59.923). The tape (conversion of WAV and other formats to ticks, off by 0.07 % on PAL and 0.13 % on NTSC) and the AVI file (header with 50 or 60 frames per second, pitch in the file 1.3 cents lower on PAL and 2.2 cents higher on NTSC) still use the nominal 50 and 60 frames per second. For the tape this changes the behaviour of the emulated computer with a tape, so it is handled separately.

### 8253 counter: known differences from the real computer

The counter is fixed according to measurements with a logic analyzer on an MZ-800. Two cases still behave differently from the real chip, and the third point concerns the MZ-700 and MZ-1500:

- **Reading a running counter without a latch command.** A program can read the counter in two ways: send a latch command first (the value is frozen and then read at leisure), or read it directly while it runs. On the real computer a direct read sometimes returns garbage: CTC0 changes 1.1 million times a second, the read pulse lasts 550 ns, the value often changes in the middle of the read and some bits come from the old value and some from the new one (for example F8h was measured instead of FBh). The emulator always returns a clean value. The rule for when and how the value breaks up could not be derived from the measurements: a broken value appears even when no clock edge falls into the read, and only some of them are a mix of the old and the new value. Consequence: a program that reads without latching runs more reliably in the emulator than on the real computer. Programs that use the latch command do not differ. A measurement including the data bus would be needed to refine this.
- **A two-byte divider interrupted by a control word for another counter.** A 16-bit divider is written in two steps, the low byte and then the high byte. When on the real computer a control word for another counter comes between the two bytes (measured: divider of CTC0, control word 54h for CTC1), the low byte is lost: instead of the written divider 0020h, CTC0 started with a divider between 13 and 255, a different one in every repetition. A latch command for the same counter between the bytes does no harm. The emulator loads the divider correctly, as if there were no control word between the bytes. What the chip replaces the low byte with is not known, and the value looks random. Consequence: a program with this bug would play a wrong tone or have wrong timing on the real computer; in the emulator it runs correctly.
- **Phase of the counter input clock relative to the display line.** On the MZ-800 the moment at which the CTC0 input clock ticks relative to the display line is set according to measurements. On the MZ-700 and MZ-1500 it is not measured. It can only show in a program that follows the beam position and at the same time loads the CTC0 divider: the new divider may then take effect one input clock period (about 1 microsecond) differently than on the real computer.

### Not measured or kept on purpose

These differences from the real computers are known. Either nobody has measured them yet, or they stay in the emulator on purpose.

- **Longer writes to ports 0E0h and 0E4h (MZ-700 PAL and MZ-1500).** On a real MZ-700 PAL a write to port 0E0h takes 1 CPU clock longer and a write to 0E4h 2 clocks longer. On the MZ-1500 a write to 0E4h takes 1 clock longer, a write to 0E0h takes the normal time. This was measured only with a Unicard card connected; the emulator does not lengthen these writes. Most likely the card lengthens them, not the computer (an MZ-800 without the card does not lengthen them). A measurement without the card will decide.
- **MZ-1500 palette after reset.** Which palette (port 0F1h) a real MZ-1500 has after reset is not measured.
- **Attenuation of the PSG channels.** The volume of a PSG channel is set by four attenuation bits. In the emulator they are exactly 2, 4, 8 and 16 dB as in the datasheet; on a real MZ-800 1.89, 3.96, 7.75 and 16.11 dB were measured. The difference is at most 0.25 dB.
- **PSG channel with tone divider 001h.** The real chip produces a 110.8 kHz square wave, which behind the output filter acts as a constant level of about half the swing (0.49); this is how samples are played. The emulator does not generate the square wave and outputs this constant level directly.
- **Output filter.** On a real MZ-800 the filter depends on the position of the volume knob, and the sound is further changed by the amplifier and the speaker. The emulator has (option Hardware output filter) a fixed first-order filter with a corner at 346 Hz, measured with the volume at maximum; it does not model the amplifier and the speaker.
- **Sound of the MZ-1500 and MZ-700.** On the MZ-1500 the output filter and the AC couplings are taken from the MZ-800 and are not measured. The sound of the standalone MZ-700 is not measured at all: that the output has no filter comes from the schematic, and the AC coupling that prevents clicks on a constant level is an aid of the emulator, not a property of the computer.
- **Tape: length of pulses written from ROM.** The lengths of the pulses that the ROM routines of the MZ-700 and MZ-1500 write to tape are not measured on the real computers. In the emulator they result from running the ROM with the wait state on reads from the monitor ROM, which is measured.


# Bugs awaiting a fix

- **topmenu**: When the user uses arrow keys for navigation, they are picked up as keyboard input to the emulation (consider whether this behavior can be suppressed)
