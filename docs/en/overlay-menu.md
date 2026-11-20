# Overlay menu

A full-screen menu over the emulator picture on all platforms (MZ-800,
MZ-700 PAL and NTSC, MZ-1500). It puts what changes most often while playing
within one or two key presses: media, speed, snapshots, picture, sound, input
and capture. It is most useful in fullscreen, where the tool windows are not
visible.

The classic menu on the right mouse button over the picture works as before
and is the only place with the rarely changed hardware settings (ROM
settings, memory disks, memory extensions, IDE, DIP switches, the second
floppy controller and others). The right mouse button anywhere over the open
overlay menu closes it and opens the classic menu (the same as **Open classic
menu** on the Machine tab); the text "Right-click: classic menu" in the
footer is a reminder of that. It does not apply over an open media exchange
sheet, over an open drop-down list, or while a dialog is open.

## Opening and closing

| Action | How |
|--------|-----|
| Open | the `F10` key, left `Alt` + left mouse button over the picture, or the **Overlay Menu...** item of the classic menu (right mouse button over the picture, the first item) |
| Close | `F10`, `Esc`, or the **Close** button |

The **Overlay Menu...** item of the classic menu shows both other ways in its
shortcut column ("F10, Alt+Click").

`F10` opens the menu when the emulator picture has the focus (not a tool
window such as the debugger) and none of `Shift`, `Ctrl`, `Alt` and `Super`
is held.

**The top bar** shows the six tabs on the left and the status labels on the
right: the platform (for example MZ-800), the speed (a percentage or Max
speed), **Muted** (only while the sound is muted) and **Paused** (only while
the emulation is paused). The **Close** button is at the far right. In a
narrower window the bar gradually drops the platform and speed labels, then
the captions of the inactive tabs; the mute label shrinks to its icon, then
the caption of the Close button and the whole mute label go. When even that
is not enough (a small window and a language with long words), the pause
label shrinks to its icon and finally the caption of the active tab goes
too. **The footer** shows the keys that apply at the moment.

**Pause.** Opening the menu pauses a running emulation and closing it
restores the state from before: an emulation that was running runs again, an
emulation that was already paused stays paused. Exceptions:

- **Resume** closes the menu and always runs the emulation, even when it was
  paused before the menu was opened.
- The **Emulation** row (Running / Paused) on the Machine tab and the
  `Alt + P` key switch the pause directly; the state chosen there stays after
  the menu is closed. An emulation started this way runs under the open menu.
- When somebody else (the debugger, MCP) runs or steps the emulation while
  the menu is open, closing the menu leaves it as it is. A pause requested
  from elsewhere while the emulation is already stopped by the open menu is
  not noticed, though, and closing the menu runs the emulation.
- The **Sound** tab has a **Listen** switch. Turned on, it runs the
  emulation so that sound changes are audible at once; turned off, it pauses
  the emulation again. It is always off after the menu is opened and on every
  arrival at the tab, and it turns itself off when you switch to another tab.
  When you close the menu with the switch on, the emulation keeps running (it
  was running before the menu was opened). This applies only to an emulation
  paused by the menu itself. An emulation that was paused before the menu was
  opened, or that you paused yourself (the **Emulation** row, `Alt + P`), is
  not started by the switch, which is disabled then. The **Paused** label in
  the top bar and the window title show the real state.

**Keyboard.** While the menu is open, the emulated computer receives no keys,
not even while the emulation runs for listening on the Sound tab. The global keyboard shortcuts of the emulator (see
[`README-hotkeys.md`](README-hotkeys.md)) are off except for those the menu
offers itself; they are shown on the key labels next to the buttons and in
the table below.

**Joystick.** While the menu is open, the emulated computer reads the
joystick at rest (no direction, no button), not even while the emulation
runs for listening on the Sound tab. This holds for the host joystick or gamepad and for
the joystick on the numeric keypad, on both ports. After the menu closes,
the emulated computer gets the joystick back once you release all its
buttons and directions, so a button held while closing does not reach the
emulated program. The menu can still be operated with a gamepad.

## Controls

| Key | Action |
|-----|--------|
| `1` to `6` | Switch the tab (Media, Machine, Display, Sound, Input, Capture) |
| `Ctrl + Tab`, `Ctrl + Shift + Tab` | Next / previous tab |
| arrows | Move between the controls |
| `Enter`, space | Select the control |
| `Esc`, `F10` | Close the menu |

Everything can be done with the mouse as well. A control that cannot be used
at the moment (for example a media card while inputs are being recorded, or
a drive of a disconnected controller) is greyed out and shows the reason when
the mouse hovers over it.

Action shortcuts that work on any tab while the menu is open:

| Key | Action |
|-----|--------|
| `F12` | Reset (the menu closes) |
| `Alt + P` | Pause / run the emulation |
| `Alt + N` | Normal speed |
| `Alt + M` | Maximum speed on / off |
| `Alt + [1..4]` | File dialog to insert a disk into FDD 0 to 3 |
| `Alt + Shift + [1..4]` | Eject the disk from FDD 0 to 3 |
| `Alt + C` | Virtual CMT window (the menu closes) |
| `Alt + K` | Virtual keyboard (the menu closes) |
| `Alt + F6`, `Alt + F7` | Save Snapshot... / Load Snapshot... |
| `Alt + F8`, `Alt + F9` | Quick Save / Quick Load |
| `Alt + F`, `Alt + Shift + F` | Screenshot / frame series (Record Changed Frames) |
| `Alt + O`, `Alt + Shift + O` | Start and stop / pause the video recording |
| `Alt + U` | Record in Real Time |
| `Alt + J` | Replay Control window (the menu closes) |
| `Alt + Ctrl + Enter` | Fullscreen / window |
| `Alt + W`, `Alt + H` | Fix the window aspect ratio by width / height |

The other global shortcuts (debugger windows, changing the custom speed with
the arrow keys and others) do not work while the menu is open.

Actions that open a tool window or change the run of the emulator (Reset,
Quick Load, Load Snapshot..., Open tape window, Snapshot Setup..., Virtual
keyboard, Autotype buffer, Joystick Setup..., Settings..., Record Inputs...,
Play Replay..., Replay Control..., Open classic menu) close the menu. File
dialogs and questions (for example about unsaved tape changes) open above
the menu.

## Tab 1 - Media

The initial tab: the state of all media and their exchange.

**Floppy drives** - the drives FDD 0 to FDD 3 of the first floppy controller
(FDC0). A drive row shows the name of the inserted image, its size, Read-only
and unsaved changes (Unsaved changes) where they apply, or Empty. The
**Change** button (**Insert disk** for an empty drive) opens the media
exchange sheet (see below); further buttons eject the disk (Eject) and save
its changes (Save changes). The drives of the second controller (FDC1) are
still handled from the classic menu.

**Cassette** - the tape in the virtual cassette deck: its name, block and
position, the transport Rewind, Play, Pause, Stop and Record, and the loading
mode **Tape / Boost / Direct** (for the modes see
[`devices/cmt.md`](devices/cmt.md)). **Change tape** opens the tape exchange
sheet, **Open tape window** closes the menu and shows the Virtual CMT window
with the remaining functions of the deck.

**Quick Disk** - the inserted disk or directory, Eject and Save changes;
**Insert disk** opens the media exchange sheet. A Quick Disk can be inserted
only into an empty drive. The card is disabled when the Quick Disk is not
connected or is controlled by Unicard.

**Recent media** - a strip of the most recently inserted media of all kinds,
newest first. Clicking a tape or a Quick Disk inserts it at once; for a disk
image the drive choice **Insert into** opens (keys `1` to `4`, `Enter`
inserts into the selected one). **Browse files...** opens the file dialog
for a disk into the first empty drive (FDD 0 when all are occupied).
**Clear history** deletes the history of all kinds of media (the inserted media are
not touched); it is shown only when the history is not empty.

**Action row** - Resume, Speed (maximum speed on / off; the label shows the
current speed), Quick Save, Quick Load, Screenshot, Fullscreen and Reset.

While inputs are being recorded or played back (Replay) the media cards are
disabled.

A disk image that is already inserted in another drive is inserted
write-protected, so that only one drive can write to the image file. The
drive row then shows "Shared with FDD n, write-protected" and **Write
protect** in the sheet cannot be turned off while the other drive may
write to the file. See devices/fdc.md, section "The same image in more
than one drive".

### Exchanging a medium

**Change**, **Insert disk** and **Change tape** open a sheet with the most
recently used files of that kind (six at most):

| Key | Action |
|-----|--------|
| `1` to `6` | Insert the file with that number; only while the filter is empty (with a non-empty filter the digit is typed into the filter). The key decides, not the character, so the digits work with any keyboard layout |
| typing | Filter the list by text; `Backspace` deletes the last character |
| arrows, `Enter` | Select a file and insert it |
| `Esc` | Close the sheet (the menu stays open) |
| `F10` | Close the whole menu |

Below the list there are **Browse files...** (the file dialog as in the
classic menu), **Eject** and, for a drive and the Quick Disk, the **Write
protect** switch. The "Now:" line shows what is inserted now. A file that no
longer exists is marked File not found; an attempt to insert it removes it
from the history. The questions about unsaved changes and a running tape
recording are not skipped.

### The Recent media history

The emulator remembers up to 12 most recently inserted files for each kind of
medium (disk image, tape, Quick Disk). A medium is written to the history
when the user inserts it: from the overlay menu, from the classic menu, with
a keyboard shortcut or with a command-line option (except in a run without
a window, `--headless` and `--mcp-pipe`, where nothing is written to the
history). Not written are disks
restored from the configuration at startup, media inserted by an input replay
playback, a tape loaded with a snapshot, and insertions over MCP or from the
emulated Unicard manager.

The classic menu (right mouse button) offers the same history in a
**Recent** submenu: for every floppy drive (HW & Devices > FD Controller >
FDD n, both controllers), for the cassette (HW & Devices > CMT) and for the
Quick Disk (HW & Devices > Quick Disk > QD Drive). It shows at most 12 file
names of that kind, the most recently inserted first, with the full path in
the tooltip; a click inserts the medium in the same way as a choice in the
file dialog, including the questions about unsaved changes. An empty history
shows a disabled "(empty)" item. **Clear recent media** at the end of the
submenu deletes the history of all kinds, like **Clear history** in the
overlay menu.

The history is saved with the configuration in the INI file, section
`[RECENT_MEDIA]`: keys `dsk_1` to `dsk_12`, `cmt_1` to `cmt_12` and `qd_1` to
`qd_12` (the path, `1` = inserted last) and for each of them a key
`<key>_time` (the time of insertion in seconds since 1970-01-01 UTC). When
started with `--no-save-ini` the history is not saved.

## Tab 2 - Machine

| Card | Contents |
|------|----------|
| Machine | **Emulation** Running / Paused, **Reset**, the **ROM** selection |
| Speed | **CPU speed** Normal / Custom / Max and **Custom speed** with a 10 % step |
| Snapshots | Quick Save, Quick Load, Save Snapshot..., Load Snapshot..., Snapshot Setup... |
| More hardware | **Open classic menu** closes the overlay and opens the classic menu |

Custom is available only after a custom speed has been set. The ROM
selection and loading a snapshot are not available while inputs are being
recorded or played back.

## Tab 3 - Display

The cards are in one column at the left edge and the menu background is more
transparent on this tab, so the rest of the emulator picture stays visible.
The cards whose effect shows in the picture at once are at the top. In a
smaller window the tab content scrolls vertically.

| Card | Contents |
|------|----------|
| Colors | **Color schema** Normal / Grayscale / Green |
| CRT scanlines | **Enable scanlines** and **Intensity** (can be changed only while scanlines are enabled) |
| Window | **Fullscreen** (in the classic menu Interface > Display > Fullscreen), **Window size on startup** (Half, Native, Normal, Bigger, Big, Fullscreen), **Lock window aspect ratio**, **Fix aspect ratio** By width / By height |
| Frame rate | **Frame rate control** (Slave, Swap With V-Sync, Swap With V-Sync (Adaptive), Immediate Swap, Custom FPS), in the Custom FPS mode a **Custom FPS** row with the value in Hz and 1 Hz step buttons, and **Forced redraw every screen** |

Changes apply immediately.

## Tab 4 - Sound

**Output:**

- **Listen** - listening while the menu is open. On the Sound tab the
  emulation is paused as on the other tabs; the switch runs it, so changes of
  the volume, mute, the filter and the channels are audible at once. Details
  below.
- **Master volume** - the master listening volume, 0 to 200 %, 100 % by
  default. Up to 100 % it attenuates, above 100 % it boosts (by up to 6 dB;
  the peaks are then softly compressed, not cut off). The slider has a mark
  at 100 %: dragging with the mouse snaps to it and a double-click sets the
  value back to 100 %.
- **Mute** - mutes the sound; the master volume and the channel volumes do
  not change, so after unmuting the emulator plays as loud as before. While
  the sound is muted, the **Muted** label in the top bar of the menu shows it
  on every tab and the title of the main window carries the mark **MUTED**.
  Mute is not saved: the emulator always starts unmuted.
- **Hardware output filter** - the output filter of the real MZ-800 (see
  [`README.md`](README.md), section Sound); the MZ-700 emulators do not have
  this option.

The master volume and mute are applied only at the sound output of the
emulator: they do not get into recordings (the sound of a video, the audio
capture), and a recording has the full volume even while listening is muted.
The master volume is saved in the INI file, section `[AUDIO]`, key
`volume_master` (an integer 0 to 4294967295, the same scale as the other
`volume_*` keys, the default is the highest value = 100 %) and key
`volume_master_boost` (per cent above 100 %, 0 to 100, default 0); the
volume is their sum. Both values are written in hexadecimal in the file
(`0xffffffff`, `0x00` to `0x64`). The same
**Mute** and **Master volume** are also in the window Interface -> Audio...
(Audio Mixer).

**Channels** - the volume of the speaker (CTC0) and of the individual PSG
channels, the same as in the window Interface -> Audio...; **Reset channels**
sets all of them back to 100 %.

**Listening (Listen).** The switch is off every time the menu is opened and
every time you arrive at the Sound tab, and the emulation is paused; the
sentence under the Output card reads "Turn on Listen to hear the changes
while the menu is open. The emulated computer keeps running without keyboard
input.". With the switch on the emulation runs (the sentence "The emulation
is running so that you can hear the changes.") and the **Paused** label in
the top bar goes away; the emulated computer still receives no keys and reads
the joystick at rest. Listening ends and the emulation is paused again when
you turn the switch off or switch to another tab. When you close the menu,
the emulation keeps running. The state of the switch is not saved.

The switch is disabled, with the reason in its tooltip, when the pause does
not belong to the menu:

- the emulation was paused before the menu was opened, or you paused it
  yourself (the **Emulation** row on the Machine tab, `Alt + P`): "The
  emulation was paused before the menu was opened. Resume it to listen.";
  the changes become audible once you resume the emulation;
- the emulation runs for another reason than listening (you started it with
  the **Emulation** row or `Alt + P`): the switch shows on and the tooltip
  reads "The emulation is running."; changes are audible at once.

When somebody else pauses the emulation while you listen (`Alt + P`, the
debugger, MCP), the switch turns off and the emulation stays paused even
after the menu is closed. Saving a snapshot (Quick Save) while listening
turns the switch off as well; when the save is done, the emulation is paused
by the open menu as it was before listening was turned on.

## Tab 5 - Input

- **Keyboard** - **Virtual keyboard** and **Autotype buffer** close the menu
  and open the respective window.
- **Joystick** - **Device** (None / Numeric keypad / Joystick) and **Joystick
  Setup...** for mapping and calibration.

## Tab 6 - Capture

| Card | Contents |
|------|----------|
| Screenshot | Save Screenshot, Save Screenshot As..., Record Changed Frames, Open Output Folder, Settings... (see [`screenshots.md`](screenshots.md)) |
| Video Recording | Start Recording / Stop Recording, Pause Recording, Record in Real Time, Open Output Folder, Settings... (see [`video-recording.md`](video-recording.md)) |
| Replay | the state of the input recording, Record Inputs..., Play Replay..., Stop Replay, Replay Control... (see [`replay.md`](replay.md)) |

The other functions of these tools (the Screenshot Control and Remote Control
windows, video markers, Rewind and seeking in an input recording, the audio
capture) stay in the classic menu and under their keyboard shortcuts.
