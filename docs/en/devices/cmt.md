# Virtual CMT (cassette recorder) - user documentation

The Virtual CMT emulates the computer's cassette recorder. A program in
the emulated computer reads the tape signal the same way as from a real
cassette, and what the computer writes to the cassette is recorded onto
the tape in the deck. In the emulator a tape is a list of blocks; it comes
from a file (TMZ, TZX, MZF, MZT, TAP, WAV) and can be edited and saved.

Open the window from the menu **HW & Devices > CMT > Show Virtual CMT**
or with Alt+C. The labels of the controls are quoted in this text as the
program shows them in English.

## Quick start

### Load a program from a file

1. Open the Virtual CMT window (Alt+C).
2. With the **Open...** button (without a tape **Open tape...**, shortcut
   Ctrl+O) choose a tape file, for example `game.mzf` or `games.tmz`.
   The tape is in the deck and stands (Stopped; in Direct mode the
   display shows Direct load).
3. Choose the loading mode on the right next to **Loading** (see Loading
   mode):
   - **Tape** - the tape signal plays in real time, as from a real
     cassette recorder; works with every loader,
   - **Boost** - the same, but the emulator runs at maximum speed while
     the cassette motor runs,
   - **Direct** - the program is copied into memory at once without
     playing the signal; works only for programs that load through the
     ROM routines.
4. In the computer enter the load command (for example loading from the
   monitor, on the MZ-800 the C key in the boot menu, or `LOAD` in a
   BASIC). A ROM load routine that does not manage to start the cassette
   motor shows the prompt PLAY and waits. A program with its own tape
   routines loads only in Tape or Boost mode: for example the cassette
   BASICs 1Z-016 and 1Z-013B read the tape themselves, and in Direct mode
   their `LOAD` waits and the tape does not move (see The CMT hack and the
   tape).
5. In Tape or Boost mode press **Play** (Ctrl+Space). The tape plays
   until the program stops it by turning the motor off; the display shows
   Playing, after the motor is turned off Waiting for motor. In Direct
   mode Play is disabled: the program loads straight from the block the
   tape stands on.

A tape with several programs: the next load command loads the next
program. Pick a particular block in the block list (the **Blocks**
button) by a double click (plays it), **Ctrl+Enter** (only moves the tape
to it) or the context menu item **Load directly** (switches to Direct mode
and selects it).

The main menu offers the most recently inserted tapes in the submenu
**HW & Devices > CMT > Recent** (at most 12, full path in the tooltip): a
click opens the tape in the same way as **Open...** in the Virtual CMT
window, including the questions about unsaved changes and a running
recording.

A faster way from the main menu: **HW & Devices > CMT > Fast Open CMT
File & Play...** opens a tape file and plays it at once (Play). The tape
then plays from the moment it is opened; a ROM load that finds the motor
already running shows no PLAY prompt and reads the tape from wherever it
is. In Tape and Boost mode use it therefore when the computer already
waits for the tape (after the load command), or in Direct mode.

### Record a program saved by the computer and get a file

In Direct mode a save through the ROM routines (for example the save of
the monitor) without Record is stored at once as a new block at the end
of the tape (Direct Save, see The CMT hack and the tape); continue with
step 4. With Record pressed everything the computer saves is recorded as
signal, in every loading mode. Record is needed in Tape and Boost mode,
for programs with their own tape routines (for example the cassette
BASICs 1Z-016 and 1Z-013B) and with Direct Save off.

1. Press **Record** (Ctrl+R). It works in every loading mode. Without a
   tape the recording goes onto a new empty tape (shown as `(new tape)`),
   with a tape the recording is added after its last block.
2. In the computer start the save (for example `SAVE` in BASIC). The
   recording starts when the program turns the cassette motor on.
3. After the save press **Stop recording** (the same button, Ctrl+R) or
   **Stop**. The message line reports the result, for example `Recorded
   "NAME", 4096 B`, and the tape stands at the first recorded block.
4. Save the program to a file: **Tape > Export block as MZF...** saves the
   current block as an MZF file; **Tape > Save as...** saves the whole
   tape (TMZ, TZX, MZT or MZF).

A program that was not recorded in the standard ROM format without an
error stays on the tape as a raw recording (see Recording); the Analyze
tool finds the programs in it (see Analysing a recording).

## The Virtual CMT window

### Layouts

The window has two layouts:

- **Compact** - only the deck: the tape, the counter, the buttons, the
  loading mode and the speed. Enough to insert a tape and load a program.
- **Full** - the deck with the list of the tape's blocks and the
  properties of the selected block below it. For a tape with several
  programs, editing, recording and export.

Switch the layout with the **Blocks** button (pressed in Full; disabled
without a tape) or in the menu **View > Compact / Full**. Each layout
remembers its own window size. With the default font the default height
of the Full layout fits a screen 768 pixels high and shows at least five
rows of the block list. The smallest window size is computed from the
length of the texts, so they are not cut off.

The window title carries the tape name and an asterisk when the tape has
unsaved changes (`Virtual CMT - games.tmz *`); a long name is shortened
in the title with three dots.

The dialogs of the window (delete confirmation, the question about unsaved
changes, the pulse length editor, the tools over the tape) open in the
middle of the Virtual CMT window.

### The deck

From top to bottom:

1. **Tape**: the file name (a long name is shortened in the middle, the
   full path is in the tooltip), the word **Modified** for a tape with
   unsaved changes, on the right the buttons **Open...**, **Eject** and
   **Blocks**. Without a tape it shows **No tape** and the buttons **Open
   tape...**, **New tape** and a disabled **Blocks**.
2. **Display**: on the left the large counter, on the right the block
   number (`Block 2 / 5`), the block name (a block without a name shows
   its kind, for example `Raw recording`), the progress of the current
   block and below it the state of the deck and of the motor.
3. **Buttons**: **Rewind** (to the first playable block, the playback
   state does not change), **Previous block**, **Play / Pause** (one
   button showing the action it performs), **Stop**, **Next block** and
   on the right **Record** (while recording **Stop recording**). Previous
   block and Next block skip control blocks and blocks that cannot be
   played. A disabled button gives the reason in its tooltip, for example
   `Stop the tape first.`.
4. **Signal speed** (the default signal speed, see Speed) and on the
   right **Loading** (the loading mode, see Loading mode). For a block
   with its own speed (from the file or set with **Set speed**) the selector is replaced by a
   greyed-out frame with that speed and the note `(set in file)`, for a
   raw recording `(recorded signal)`, in Direct mode `Not used in Direct
   mode`.

The window numbers blocks from 1 (display, block list, messages). The MCP
tools number blocks from 0: block 3 in the window is block 2 in MCP.

### The counter

The counter shows the time from the start of the tape (**Elapsed**);
clicking it switches to the time to the end of the tape (**Remaining**)
and back. Both times need the lengths of the blocks. The emulator
computes the lengths of blocks that have not played yet only while the
tape is stopped, so until then the counter shows the time in the current
block (**Block time**) and **Remaining** cannot be switched on. While
recording it shows the recording time (**Recording time**) and the
number of recorded pulses instead of the progress, also in Direct mode.
Without a tape and in Direct mode (except while recording) it shows
`--:--.-`.

### Deck state and motor

The deck state (always text and colour):

| State | When |
|-------|------|
| **No tape** | there is no tape in the deck |
| **Stopped** | a tape is inserted and stands |
| **Playing** | the tape plays and the motor runs |
| **Waiting for motor** | playback or recording waits until the program turns the cassette motor on |
| **Paused** | paused with the Pause button |
| **Recording** | recording and the motor runs |
| **Direct load** | loading mode Direct (takes precedence over the other states, also over No tape; while recording the deck shows Recording, Waiting for motor or Paused as in the other modes) |

Next to the state there is **Motor on** or **Motor off** when the
emulator knows the motor state (not for a stopped tape, without a tape
and in Direct mode except while recording).

The program in the computer turns the cassette motor on and off. The
MZ-821 and MZ-1T04 data recorders have no pause button; the deck in the
emulator adds one so that the tape can be held independently of the
program. The pause with the **Pause** button is independent of the motor: while the
pause is on, the program's motor commands do not move the tape. **Play**
clears the pause and leaves the motor as the program set it: when the
program turned the motor off, the tape keeps waiting for the motor
(Waiting for motor) and moves when the program turns it on. Pausing and
resuming a tape that waits for the motor therefore leaves it waiting. A
tape waiting for the motor is started by the program turning the motor
on, or by **Stop** and **Play** (the current block then plays from its
start). A recording waiting for the motor (a program that does not turn
the motor on itself) is started by **Play**.

### The block list

The block list is a table in the Full layout; it shows whole rows only
and scrolls by whole rows. Default columns: the mark of the current block
(a blue triangle), **#**, **Name**, **Type**, **Length** and **Status**.
The columns **Size**, **Load**, **Exec** and **Speed** are hidden; switch
them on in the menu **View > Columns** or with the right mouse button on
the table header. A column width can be changed by dragging its border in
the header, the emulator remembers it; **View > Reset column widths**
sets the widths by the content of the current tape.

The **Type** column of a block with a program shows the file type from
its header as a number, for the known types with a word: `01 machine
code`, `02 BASIC program`, `03 data`, `04 BASIC data`, `22 CP/M file`;
other types only as a number. A BASIC program can also have the type
`05`: for example the cassette BASICs 1Z-016 and 1Z-013B save their
programs with it.

The **Status** column:

| Text | Meaning |
|------|---------|
| `OK` (dimmed) | the block plays |
| `Not decoded` (dimmed) | a raw recording: a signal the emulator did not decode (a WAV file, a recording); plays as it was recorded |
| `Unsaved` (orange) | the block is not in the tape file yet (recorded or imported, also a raw recording or a control block); after saving the tape its real state is shown |
| `Cannot play` (red) | a block the emulator cannot play; it is skipped during playback |
| a description, for example `Loop start x3` | a control block (loop, jump, stop the tape, information); the row is dimmed, its name is `Control block` and it cannot be played |

While recording there is a row `Recording...` at the end of the list. For
an empty tape the text `This tape is empty.` with a hint is below the
header.

Operation:

- Selected rows have a blue background and a lighter blue bar at the left
  edge.
- A **click** selects a row; the tape does not move (the selection may
  differ from the current block). **Ctrl+click** adds a row to the
  selection or removes it, **Shift+click** selects a range of rows and
  **Ctrl+A** selects all blocks. The **arrow keys** move the selection.
- A **double click** moves the tape to the block and plays it; when the
  tape is paused with the Pause button, it moves it to the start of the
  block and the pause stays.
- **Ctrl+Enter** moves the tape to the selected block and does not
  change the playback state.
- **Dragging a row** with the mouse and a file dropped on the list - see
  Editing the tape.

The context menu of a row (right mouse button):

| Item | What it does |
|------|--------------|
| **Play from here** | plays the block from its start (like a double click) |
| **Go to this block** (Ctrl+Enter) | moves the tape to the start of the block, does not change the playback state |
| **Load directly** | selects the block and switches to Direct mode (see Loading mode); only with the tape stopped and for a block with an MZF program |
| **Export as MZF...**, **Export as WAV...** | saves the block to a file (see Saving and export) |
| **Set speed** | the speed of the block (see Speed) |
| **Analyze...** | finds programs in a raw recording (see Analysing a recording) |
| **Convert to profile...** | builds the signal of the block with another profile (see Converting a block to another profile) |
| **Move up** (Alt+Up), **Move down** (Alt+Down) | moves the block by one place (see Editing the tape) |
| **Import file here...** | inserts the blocks of a file before this row (see Editing the tape) |
| **Delete** (Del) | deletes the block; with several selected blocks **Delete N blocks** (see Editing the tape) |
| **Properties** | selects only this row and expands the properties |

**Delete** deletes all selected blocks when the menu's row is among them,
otherwise only the block of the menu's row; the other items work with the
row on which the menu was opened. A disabled item gives the reason in its
tooltip.

### Properties

Below the block list there is a properties section, collapsed by default;
expand it by clicking its header or with the context menu item
**Properties** (the window remembers the state). The expanded properties
get as many whole rows as fit next to three rows of the list, the rest
scrolls.

- **Block properties** (one selected block) - depending on the kind of
  block for example **Type**, **Size**, **Load address**, **Exec
  address**, **Speed**, **Long pulse (µs)** and **Short pulse (µs)**
  (upper / lower half-wave), **Length** and **Source**; for a raw
  recording **Sample rate** and **Pulse count**.
- **Source**: `Recorded` - a block recorded in the emulator, as long as
  the tape stays inserted; `Imported` - a block inserted by importing a
  file, until you save the tape, load a snapshot or play an input
  replay; other blocks by the file the tape was opened from: `Imported
  from MZF` from an MZF or MZT file, otherwise `From tape file`.
- **Tape properties** (no selection) - **File**, **Blocks**, **Total
  length**, **Format**; while recording **Blocks** also counts the block
  being recorded, the same as `Block 6 / 6` on the display.
- With several selected blocks only their number (`3 blocks selected`).

### Menus

The **Tape** menu:

| Item | What it does | When available |
|------|--------------|----------------|
| **New tape** | ejects the tape and inserts a new empty one | tape stopped |
| **Open...** (Ctrl+O) | opens a tape file (see Tapes and files) | not while recording |
| **Import file...** (Ctrl+I) | inserts the blocks of another file at the end of the tape, without a tape into a new tape (see Editing the tape) | tape stopped |
| **Save** (Ctrl+S) | saves the tape to its file (see Saving and export) | tape stopped |
| **Save as...** | saves the tape to a new file: TMZ (default), TZX, MZT or MZF | tape stopped |
| **Export block as MZF...** | saves the program of the current block as an MZF file | tape stopped, block with a program |
| **Export block as WAV...** | saves the signal of the current block as a WAV file | tape stopped |
| **Export tape as WAV...** | saves the whole tape as a WAV file as it would play | tape stopped with at least one block |
| **Analyze recording...** | finds programs in the selected raw recording (see Analysing a recording) | tape stopped, one selected raw recording |
| **Find files while playing** | checkbox: a raw recording that plays is analysed in the background (see Analysing a recording) | always |
| **Tape information...** | the title, author and other texts of the tape and the target machine (see Tape information) | tape stopped |
| **Check tape** | lists problems in the structure of the tape (see Checking the tape) | tape inserted, in any state |
| **Eject** (Ctrl+E) | ejects the tape | not while recording |

While an input replay is recorded only **New tape**, **Eject** and
**Check tape** of the Tape menu are available, while an input replay is
played only **Check tape** (see Input replay). Exporting another block
than the current one is in the context menu of its row.

The **View** menu: **Compact**, **Full**, **Columns** (the columns of the
block list; **Show all** switches all of them on at once) and **Reset
column widths**; Columns and Reset column widths only in the Full layout.

### Keyboard shortcuts

The shortcuts work while the Virtual CMT window has the focus. The window
shortcuts use Ctrl, so that typing does not trigger anything; the arrow
keys, Ctrl+A, Del and Alt+arrows work only in the block list (after
clicking into the list). While the window has the focus, keys do not go
to the emulated computer (click into the computer's picture). A shortcut
whose action is not available now does nothing; the tape editing
shortcuts (Ctrl+I, Del, Alt+Up, Alt+Down) show the reason as a warning in
the message line (for example `Stop the tape first.`).

| Shortcut | Action |
|----------|--------|
| Ctrl+Space | Play / Pause |
| Ctrl+. | Stop |
| Ctrl+R | Record (while recording Stop recording) |
| Ctrl+Home | Rewind |
| Ctrl+PageUp / Ctrl+PageDown | previous / next block |
| Ctrl+O | Open... |
| Ctrl+I | Import file... |
| Ctrl+S | Save |
| Ctrl+E | Eject |
| arrow keys | selection in the block list |
| Ctrl+A | select all blocks in the list |
| Del | delete the selected blocks |
| Alt+Up / Alt+Down | move the selected block one place up / down |
| Ctrl+Enter | move the tape to the selected block in the list |
| Alt+C | closes the window (also opens it from the main window) |

Escape does not close the window.

### Messages

The results of the window's actions appear in the message line at the
bottom of the window: the result of a recording (for example `Recorded
"NAME", 4096 B`), saving the tape and exports, opening a tape (`Opened
games.tmz, blocks: 5`), importing a file (`Imported prog.mzf, blocks: 1`),
deleting blocks (`Deleted 2 blocks.`), the results of the tools over the
tape (`Files added from the recording: 2`, `Tape information updated.`,
`Block 3 converted to ROM-800-NOMINAL.`), a change of the loading mode and
a refused action. The last message stays dimmed until another one
replaces it; an error is red, a warning orange. A message that does not
fit is shortened with three dots (a file path in the middle, so the file
name stays visible) and the full text is in the tooltip.

When the window is closed or collapsed, errors and warnings appear in a
message window (Error, Warning) and information is dropped (it is not
shown when the window opens). An error opening a tape always appears in
a message window, as does a failed save of the tape while the emulator
is being closed.

Something other than the window can change the tape in the meantime (for
example an MCP tool or the end of the tape). A window action therefore
works only with the tape and the state the window showed when you chose
it:

- when the tape is replaced in the meantime (another tape, an edit
  through MCP), the action is not done and the warning `The tape changed;
  the action was cancelled.` appears; an open context menu of a row
  closes with the same warning,
- when the state of the deck changes between the click and the action
  (for example the tape has run to its end or started moving), the action
  is not done and the warning `The tape state changed; the action was
  cancelled.` appears.

Texts that come from the emulator core (reasons in the analysis, findings
of the check, profile descriptions and some warnings) are in English in
all languages of the emulator.

## Tapes and files

### Formats

Every file is a tape with blocks - even a single MZF program or a WAV
recording is a tape with one block.

| File | Blocks of the tape | Saving (Save, Save as...) |
|------|--------------------|---------------------------|
| `.tmz` (Sharp MZ tape format) | the blocks of the file | keeps all blocks |
| `.tzx` (ZX Spectrum tape format) | the blocks of the file | keeps all blocks; a tape with Sharp MZ blocks is written in the TMZ format (with a warning) |
| `.mzt` | one block per program in the file | programs only (see Saving and export) |
| `.mzf`, `.m12` | one block with the program | only a tape with a single program |
| `.tap` | one block per TAP block (header or data) | the emulator does not save to TAP |
| `.wav`, `.wave` | one block with the recording (PCM, IEEE float and WAVE_FORMAT_EXTENSIBLE) | the emulator does not save a tape to WAV, it only exports the signal (see Saving and export) |

The open dialog offers the filter `Tape (*.tmz *.tzx *.mzf *.m12 *.mzt
*.tap *.wav)`.

### Opening a tape

**Open...** (Ctrl+O, in the Tape menu and as a button) opens a tape
file. The file is read and checked first: a tape that cannot be opened
(a missing, damaged or unsupported file) does not change the tape in the
deck - it stays inserted, including its unsaved changes and a recording
in progress, and only an error is reported. A WAV file in which the
signal level never changes is not opened ("WAV contains no signal").

When a tape is opened, the emulator writes a warning to the log when:

- the import of the file ran into a problem,
- the tape has blocks of a kind the emulator cannot play (for example
  TZX 0x19 or CSW with Z-RLE compression) - the warning lists them,
- the tape has no playable block (or the first block cannot be opened).

A block of a supported kind that still gives no signal (for example with
data outside the player's limits) is reported in the log only when
playback reaches it, and it is skipped.

After opening a tape whose only block is a raw recording, the message
line suggests the analysis: `Opened recording.wav, blocks: 1. Use Analyze
to find files.`

A file dropped from Windows Explorer on a part of the Virtual CMT window
other than the block list (the deck, the menu, the properties, the
message line; in the Compact layout the whole window) is opened as a
tape, the same as with **Open...**, including the question about unsaved
changes; this also works for a collapsed window. When the file cannot be
opened now, the message line shows the reason as a warning (while
recording `Stop the recording first.`). Of several dropped files only the
first one is opened (`Only one file can be opened as a tape.`). A file
dropped while a dialog or a menu is open or during another question
about the tape is not used (`Finish the open dialog first.`). A file
dropped elsewhere on the main emulator window is not used. Dropping files
is meant for the Virtual CMT window inside the main emulator window: a
Virtual CMT window dragged out of the main window (as a separate window)
may not accept a dropped file - then nothing happens.

**MZF size check**: when the item **HW & Devices > CMT > CMT Options >
Enable MZF Size Check** is on (on by default) and an `.mzf` or `.m12`
file longer than its header says (program size + 128 bytes of header)
is opened, the emulator opens the window **MZF Size Warning** with the
file name and the number of exceeding bytes. Such a file comes for
example from an export from CP/M. **Ignore** leaves the file, **Truncate**
truncates the original file to the correct length, **Save As** saves a
truncated copy under a new name (a name without the extension `.mzf`,
`.m12` or `.mzt` gets the extension `.mzf`). The checkbox **MZF size
check enabled** in this window switches the check off or on. The tape is
opened in any case. The same check runs when the CMT hack loads an MZF
file chosen in its file dialog (see The CMT hack and the tape).

With the item **HW & Devices > CMT > CMT Options > Fix MZF Size
Automatically** on (off by default; the same as the checkbox **Fix
automatically next time** in the MZF Size Warning window) the emulator
does not ask: it truncates the original file to the correct length at
once and reports it in the message line of the Virtual CMT window. If
the file cannot be truncated, the MZF Size Warning window opens as
usual. The item works only with Enable MZF Size Check on.

### Blocks

- A **block with a program** carries an MZF file (from MZF, MZT, TMZ
  program blocks, a recorded program). Only such a block can be exported
  as MZF and loaded in Direct mode; it cannot be analysed.
- **TAP blocks** (header, data) are ZX Spectrum tape blocks. A TAP block
  with a flag byte other than 00h / FFh is accepted.
- A **raw recording** (Status `Not decoded`) is a signal the emulator did
  not decode: a WAV file or a recording the emulator did not recognise as
  a program. It plays exactly as it was recorded.
- TMZ and TZX **control blocks** (loops, jumps, calls, groups, stop the
  tape, information blocks) have no signal: they are in the block list
  but cannot be opened. TMZ and TZX tapes play with loops, jumps and calls
  of blocks.
- A block the player cannot play (Status `Cannot play`) stays in the
  block list and is skipped during playback.

**Pauses between blocks** come from the tape itself: 1000 ms after an MZF
and MZT program, for TAP 350 ms after a header and 1000 ms after data,
none after the last block; for TMZ and TZX as in the file.

**Stopping the tape** (a Pause 0 block or the "stop the tape" block in
TMZ / TZX) pauses the deck: the tape stands at the block after the command
and waits for the motor (Waiting for motor) until the program turns it on
again. From the window **Stop** and **Play** start it (the current block
plays from its start); the **Pause** button only pauses it (see Deck state
and motor).

**End of the tape**: the deck stops (Stopped) and stays at the last
block. With the option `autorewind = 1` in the `[CMT]` section of the
`.ini` file the tape rewinds to its first block instead (see Settings in
the .ini file).

### Unsaved changes

Edits in the emulator change the tape in memory; the tape file changes
only when you save it (**Save**, **Save as...**). A tape that differs from
its file has unsaved changes: the word **Modified** in the window, **\***
in the window title. Blocks that are not in the tape file yet have the
Status `Unsaved`.

Unsaved changes come from recording, importing, deleting or moving blocks,
changing the speed of a block (to another speed than it has), adding files
from the analysis, writing the tape information and converting a block to
another profile. A change of the default speed (**Signal speed**),
playback, moving on the tape and exports do not mark the tape as unsaved.

A tape with unsaved changes is stored whole in a snapshot and in an input
replay (see Snapshots and input replay).

### Questions before losing changes

Before an action that would discard a recording or unsaved changes -
ejecting, opening another tape (also **Fast Open CMT File & Play...** in
the menu), **New tape**, loading a snapshot (**Load Snapshot...**,
**Quick Load**, also with their keyboard shortcuts), starting the
playback of an input replay (**Play Replay...**) or closing the emulator
window - the emulator asks:

- during a recording: **Stop recording** (the recording is finished and
  added to the tape, then the question about unsaved changes follows),
  **Discard recording** or **Cancel**,
- with unsaved changes: **Save tape**, **Discard changes** or **Cancel**.
  After **Save tape** the action continues only when the save succeeded.

In the Virtual CMT window ejecting, opening and a new tape are disabled
during a recording (`Stop the recording first.`); the question during a
recording therefore comes from actions of the main menu, snapshots, input
replay and closing the emulator.

When the action has a file dialog (opening a tape, loading a snapshot,
playing an input replay), the state of the tape is checked again after
the file is chosen: when a recording started or the tape got unsaved
changes in the meantime, the emulator asks again. Messages that arrive
during such a question wait and appear after it. When a menu or another
dialog is open while the window is being closed, the question appears
right after it closes.

Exiting the emulator without a window (headless, through MCP, automation)
discards the changes with a warning in the log. The MCP tools and the
command line never ask: ejecting or opening another tape is done (the MCP
tool then returns the warning "Unsaved tape changes were discarded"). The behaviour
during an input replay is described in Snapshots and input replay.

### Saving and export

**Save** (Ctrl+S) saves the tape to its file. For a new tape, a format the
emulator does not save to (WAV, TAP), or a format that cannot hold the
tape (for example an opened MZF file with another recorded block, or a
tape with tape information opened from MZF), it opens **Save as...**
instead. **Save as...** saves the tape to a new file: TMZ (default), TZX,
MZT or MZF. After saving, the tape belongs to the new file and has no
unsaved changes.

What each format keeps:

- **TMZ** keeps all blocks, speeds, pauses and the tape information.
- **TZX** too; a tape with Sharp MZ blocks is written in the TMZ format
  (with a warning).
- **MZT** carries programs only: every block must be a program (so no
  tape information either). It does not keep the pauses between blocks
  (1000 ms after reopening), stores the speed of a block as UniCMT pulse
  lengths and cannot store a speed of the first block other than 1:1 nor
  the 2:1 CP/M speed (the warning names the block; the block is saved
  without its speed, so after reopening the file the first block plays at
  the default speed and a later block at the speed of the blocks before
  it). A block with the default speed is written to MZT as 1:1, not at the
  speed set by **Signal speed**: after reopening it plays at the default
  speed set at that time; when it lies in the file after a block with a
  speed other than 1:1, it plays at 1:1.
- **MZF** only for a tape with a single program.

Saving to TMZ and TZX writes every block at the speed it plays, also a
block with the default speed (at the speed set by **Signal speed** at
that time): the reopened file plays the same as the tape before saving,
regardless of a later **Signal speed**. Custom pulse lengths are stored
in whole microseconds (fractional values are rounded).

The file is saved through a temporary file: when saving fails (a full
disk, a read-only file), the original file stays whole and the tape keeps
its unsaved changes. The same holds for exports.

**Exporting a block**: **Tape > Export block as MZF...** and **Tape >
Export block as WAV...** work with the current block, **Export as MZF...**
and **Export as WAV...** in the context menu with the block of the row.
MZF works only for a block with a program; WAV (44.1 kHz) contains the
signal of the block. An export does not change the tape, its position or
its unsaved changes.

**Exporting the whole tape**: **Tape > Export tape as WAV...** writes the
whole tape to a WAV file (44.1 kHz, 8 bit, mono) as the deck would play it
from the start: blocks with their pauses, loops, jumps and calls, blocks
with their own speed at that speed. Blocks the emulator cannot play are
skipped with a warning; stopping the tape (a Pause 0 block, "stop the
tape") does not interrupt the export. A tape longer than 4 hours is not
exported. The polarity switch does not affect blocks from a WAV file in
the export. The file dialog offers the tape name with the extension
`.wav` (`tape.wav` for a tape without a file). When the tape changes or
starts moving while the file dialog is open, nothing is exported (see
Messages). A tape that gives no signal at all (for example only control
blocks) is not exported (`The tape gives no signal, not exported`); a
loop of control blocks without a signal ends the export with a warning; a
block that cannot be built at its speed stops the export with an error.

## Recording

The Virtual CMT records what the computer writes to the cassette (for
example the SAVE command of the monitor or BASIC) onto the tape in the
deck, just like a real cassette recorder. The steps are in the Quick start.

- **Record** (with the deck stopped, in every loading mode) starts
  recording at once, with or without a tape in the deck. Without a tape it records
  onto a new empty tape, shown as **(new tape)**; with a tape the
  recording is added after its last block. The deck waits until the
  program turns the cassette motor on (SAVE does it itself); time with
  the motor stopped is not recorded. For a program that does not turn the
  motor on itself, the **Play** button starts a recording waiting for the
  motor (Waiting for motor).
- While recording, everything the computer writes is recorded as signal,
  in Direct mode also a save through the ROM routines: Direct Save
  applies only without Record (see The CMT hack and the tape).
- **Stop recording** or **Stop** ends the recording. The emulator decodes
  the recording and adds it to the end of the tape:
  - a program saved without an error in the standard ROM format becomes
    an ordinary block with a program (it can be loaded, exported and its
    speed changed like a block from an MZF file); a SAVE from the ROM
    gives exactly the saved program, header and body,
  - anything else (another format, an error, a recording stopped in the
    middle, a header without its program) becomes a **raw recording**:
    the signal exactly as it was recorded. It plays back the same way,
    the polarity switch does not invert it and its speed cannot be
    changed. A very short recording (fewer than 64 signal pulses) adds no
    block.

  The result is reported in the message line (for example `Recorded
  "NAME", 4096 B`) and the deck stands at the first new block, ready to
  play it. Stopping does not eject the tape. The new blocks are `Unsaved`
  and the tape has unsaved changes.
- **Automatic stop**: the option `rec_autostop_s` in the `[CMT]` section
  of the `.ini` file (seconds, 0 to 600, default 0 = off) ends the
  recording like Stop after that many seconds without a signal while the
  cassette motor runs, once something has been recorded. Recording also
  stops by itself after 4 hours or 16,000,000 signal edges. A computer
  reset does not stop the recording.

## Editing the tape

The tape in the deck can be edited: delete blocks, change their order and
insert blocks from another file. Deleting and moving blocks are in the
block list (Full layout), importing a file also in the **Tape** menu (both
layouts). Edits change the tape in memory and the tape then has unsaved
changes (see Unsaved changes).

Rules for all edits (also for the tools that change the tape):

- Only a **stopped tape** can be edited (the display shows Stopped, or
  Direct load in Direct mode): not while it plays, is paused or
  records (`Stop the tape first.`), and not while an input replay is
  recorded or played. A disabled menu item gives the reason in its
  tooltip, a disabled editing shortcut in the message line.
- In Direct mode an edit forgets the program header that a program in the
  emulator has already loaded: when the program then goes on loading the
  body, it gets a load break (Break) and the body is not loaded; a new
  load starts with the header of the block the deck stands at.
- The numbers of the blocks after the edited place shift. The deck stays
  at the block it stood at (from its start); when that block was deleted,
  at the block that took its place, or at the last block. When that is a
  control block or a block that cannot be played, the deck stands at the
  nearest following playable block (or the nearest previous one); when
  the tape stood past its end before the edit, it rewinds to the start.
- A block with a program whose speed can be changed (see Block speed) and
  that has no pause after it (typically the last block of an imported
  file or of a recording) gets a pause of 1000 ms when another block
  comes after it by a move, an import or an analysis.
- When after an edit a program block (a TMZ MZ File block) plays at a
  different computer clock than before, because a different TMZ machine
  information block applies to it (a deleted or moved information block,
  a program moved or imported across one), the message line reports it
  with the warning `The tape clock setting now applies to different
  blocks; check their speed.` (in parentheses after the result); check
  the speed of the blocks. The edit is done anyway.
- When the tape is replaced or its state changes during a confirmation
  question or before the edit is done, the edit is not done (see
  Messages).

**Deleting**: **Delete** in the context menu or the **Del** key deletes
the selected blocks. Blocks stored in the tape file are deleted at once.
When the selection contains unsaved blocks (Status `Unsaved`), the
emulator asks first: `Delete this unsaved block? Its data is not stored
anywhere else.` when a single unsaved block is selected, `Delete 2 unsaved
blocks? Their data is not stored anywhere else.` when all selected blocks
are unsaved, `Delete blocks: 5.
Unsaved: 2. Data of the unsaved blocks is not stored anywhere else.` when
only some are; buttons **Delete blocks** and **Cancel**. The default
choice for Enter is **Cancel** and it is highlighted when the question
opens; Escape cancels the question too. The message line reports the
result (`Deleted block 3.`, `Deleted 2 blocks.`) and the block in the
place of the first deleted one (or the last block) is selected. A
selection of very many separate groups of blocks cannot be deleted at
once (`Too many separate blocks selected.`); delete it in parts. Deleting
all blocks leaves an empty tape in the deck (`This tape is empty.`); the
tape is not ejected.

**Moving**: **Move up** / **Move down** in the context menu or **Alt+Up**
/ **Alt+Down** in the block list moves a block by one place; only one
selected block is moved. Another Alt+Up or Alt+Down pressed before the
previous move has finished is not lost: the block moves once more. A
block can also be dragged with the mouse to another place in the list:
while dragging, a blue line between the rows shows where the block will
be inserted, the block number and name are at the bottom right of the
pointer. Near the top or bottom edge of the list the list scrolls (the
closer to the edge, the faster). Releasing outside the list or Escape
while dragging moves nothing; the editing shortcuts do nothing while
dragging. When the tape changes while dragging, the drag is cancelled
with the warning `The tape changed; the action was cancelled.` and
nothing is moved. The moved block stays selected.

**Importing a file**: inserts all blocks of another tape file into the
tape (the same formats as **Open...**, converted the same way as when
opening; a WAV file as one block with the recording).

- **Tape > Import file...** (Ctrl+I) inserts the blocks at the end of the
  tape; without a tape it creates a new tape with the imported blocks.
  When an import into an empty deck fails, the deck stays empty.
- **Import file here...** in the context menu of a row inserts the blocks
  before that row.
- **Dropping a file** (for example from Windows Explorer) on the block
  list inserts its blocks at the place under the pointer (a blue line
  shows where), below the last row or above the table header at the end
  of the tape. Several dropped files are imported one after another in the
  order the system passes them. The import stops at the first file that
  cannot be imported: the message line shows its error with the sentence
  `The remaining files were not imported.` and the files after it are not
  imported. When an import is not possible now (for example the tape
  plays), the message line shows the reason as a warning; a file dropped
  while a dialog or a menu is open is not used (`Finish the open dialog
  first.`). A file dropped elsewhere on the window is opened as a tape
  (see Opening a tape).
- The message line reports the result (`Imported prog.mzf, blocks: 1`)
  and the first imported block is selected. The imported blocks are
  `Unsaved` until you save the tape. Import warnings are in parentheses in
  the message (for example when the imported blocks cannot be played).
  With several files the warning of an earlier file stays in the message
  line.
- A new import while another import has not finished yet is not done
  (`Another import is still in progress.`). When the result of an import
  does not arrive, the import ends with the message `The import could not
  be completed.`.
- The polarity switch affects an imported WAV block like an opened WAV
  file only until it is saved (see Polarity).

## Tools over the tape

Four tools of the Virtual CMT window work with the tape in the deck:
**Analyze** finds programs in a raw recording and turns them into blocks
with a program, **Tape information** writes the title, the author and
other texts into the tape, **Check tape** lists problems in the structure
of the tape and **Convert to profile** builds the signal of a block with a
program in another way. The analysis (adding files), the tape information
and the conversion change the tape and follow the rules of editing (see
Editing the tape); new or changed blocks are `Unsaved`. Check tape only
reads the tape.

### Analysing a recording

**Analyze...** in the context menu of the row of a raw recording, or
**Tape > Analyze recording...** with that single row selected, opens the
dialog `Analyze recording - block N`.

**While playing**: with **Tape > Find files while playing** on (the
default, key `auto_analyze`) a raw recording is analysed in the background
as soon as it starts to play, with the default options. The tape is not
changed. When the recording has played or the tape is stopped and files
that can be added were found, the message line says so, for example
`Block 2: files found in the recording: 3. Use Analyze recording... to add
them to the tape.`; the dialog then opens with the result at once. A
recording is analysed this way once; it is not analysed while an input
replay is recorded or played, and only while the Virtual CMT window is
shown.

The analysis starts at once and runs in the background: the emulation and
the window keep working, the dialog shows `Analyzing...` and the elapsed
time. **Cancel**, Escape or closing the Virtual CMT window leaves the
dialog at any time; a running analysis is left, finishes in the background
and changes nothing.

The result: a sentence with the number of files found (and how many of
them have a checksum error), a dimmed line with the block name, its length
and its pulse count, and a table with one row per file in the order of
the recording: a checkbox, **#**, **Name**, **Type**, **Format** (`Sharp
standard` for the standard format of the ROM SAVE command, otherwise the
format name, for example `TURBO`), **Start** (time from the start of the
recording), **Size**, **Checksum** (`OK`, `Header error`, `Body error`,
`-` when it cannot be verified) and **Result** (`Will be added`, `Will be
added (repaired)`, `Stays raw`, `Cannot add`). A part of the signal of at
least 64 pulses without a file is a row `Unknown signal`. For the selected
row a line below the table says why the file can or cannot be added and
what can be tried (for example `Cannot be added. You can try "Repair
damaged body" and Analyze again.`). When nothing is found, the dialog says
`No files found.` and expands **Signal details**.

**What can be added**: a file with a correct header and body checksum,
or a file repaired by an enabled repair option, in one of these formats:

| Format | Recording | The block is built by |
|--------|-----------|-----------------------|
| `Sharp standard` | the ROM SAVE command | the emulator's own signal at the default speed |
| `CPM-CMT` | CMT.COM under CP/M (standard format at 2400 Bd) | the emulator's own signal at speed 2:1 (CP/M) |
| `TURBO` | TurboCopy, Intercopy TURBO | profile TC-1.21 or IC-10.2-TURBO with the recorded speed; with an unknown writer a turbo block at the nearest speed |
| `FASTIPL` | Intercopy FAST IPL | profile IC-10.2-FASTIPL with the recorded speed; with an unknown writer a FASTIPL block at the nearest speed |
| `CPM-TAPE` | TAPE.COM (Pezik / MarVan), Intercopy CPM | profile PEZIK-2.0A (a body up to 16 KB) or IC-10.2-CPM |
| `BSD` | a data file of BASIC (up to 254 pieces of 256 B) | profile BASIC-1Z016 |

Such files are checked; the checkbox (or the space bar on the row)
decides which ones are added. A block of a format other than the standard
one is written again by the writer profile: it carries the same file (it
can be exported as MZF, except the BSD data file) and plays a clean
signal, but the timing of the original recording is not kept. The line
below the table names the profile. A file with a checksum error or
incomplete stays in the recording (`Stays raw`); files in other formats
(MZ-80B, SINCLAIR, FSK, SLOW, DIRECT, ZX Spectrum) and a file the profile
cannot write (for example a TAPE.COM body over 16 KB) cannot be added as
blocks (`Cannot add`) and also stay in the recording, where they keep
playing as they were recorded.

**Options**: **Leader tolerance** (2 to 35 %, default 20 %, **Reset**
restores it) says how much the pulse lengths in the leader tone may
differ; raise it for worn or unstable recordings. Repair options:
**Repair damaged header** keeps a file with an intact header whose body
is missing (the body is filled with 00h); **Repair damaged body** keeps a
file with an incomplete body or merges two damaged copies (missing bytes
are filled with 00h); **Repair incomplete BSD file** keeps an incomplete
BSD data file (missing pieces are filled with 00h). A repaired file
is `Will be added (repaired)`; its data may have 00h in place of lost
bytes. After an option changes the results are outdated: the table is
dimmed and nothing can be added until **Analyze again** runs the analysis
with the new options.

**Signal details** (collapsed by default) show a histogram of the pulse
lengths of the selected row, without a selected row of the whole
recording; for a file it marks its short and long pulse. Very long pulses
(pauses, silence) are not drawn; the dialog says from which length.

**Add files after recording** (the default button, Enter) inserts the
checked files as new blocks right after the recording; the recording
stays as it was. **Replace recording** replaces the recording by the
checked files; every other part of the signal (unchecked files, files
with an error, other formats, unknown signal) stays on the tape in raw
recording blocks in the original order, so nothing is lost that does not
become a block with a program. Only very short pieces next to a file
(fewer than 64 pulses, typically silence) are left out, as when
recording. When the recording is not saved in the tape file yet, the
dialog warns `This recording is not saved yet. Replacing it cannot be
undone.`: after Replace its signal cannot be brought back (a recording
saved in the tape file stays in that file until you save the tape).

The blocks with a program are built like a recorded program: the
emulator's own signal at the default speed (it follows **Signal speed**
and **Set speed** can change it), a pause of 1000 ms between two of them;
their signal is created again from the decoded data, it is not a copy of
the recording. After Add or Replace the dialog closes, the message line
shows `Files added from the recording: 2` and the first new block is
selected. When a TMZ machine information block with a computer clock
other than the default lies before the recording, the new program blocks
play at that clock and the message line warns in parentheses after the
result `The tape clock setting now applies to different blocks; check
their speed.`; then check the speed of the blocks. When the tape changes
while the dialog is open, the dialog says `The tape changed. Close this
dialog and analyze again.` and adds nothing.

### Tape information

**Tape > Tape information...** edits the texts stored in the tape:
**Title**, **Publisher**, **Author**, **Year**, **Language**, **Type**,
**Price**, **Protection**, **Origin**, **Comment** (also several lines)
and **Target machine** (MZ-700, MZ-800, MZ-1500, MZ-80B or Not
specified). They are stored in information blocks at the start of the
tape (a TZX Archive Info block and a TMZ machine information block); in
the block list they are `Control block` rows, so adding them shifts the
numbers of the other blocks.

- Each text holds at most 255 characters (the field takes no more). The
  texts are stored in ISO 8859-1 (Latin-1): a character outside this set
  (for example the Czech č, ř, ě, ů or Cyrillic) is stored as `?` and the
  message line warns which fields are affected; á, é, í, ó, ú and ý are
  stored.
- **OK** writes only when something changed (message `Tape information
  updated.`); without a change it closes the dialog and the tape stays
  untouched. **Cancel** and Escape close the dialog without writing. A
  cleared text removes its entry; entries the dialog does not show stay.
- **Target machine** records which computer the tape is for. Choosing a
  machine on a tape without a machine block adds a block with the default
  clock, so the timing of the tape does not change; **Not specified**
  removes the block. When the removed block had a different CPU clock, the
  blocks after it play at a different clock and the message line warns
  `The tape clock setting now applies to different blocks; check their
  speed.`. A tape whose machine block names another machine shows Not
  specified and the block stays unchanged until you choose a machine.
- When the tape changes while the dialog is open, the dialog says `The
  tape changed. Press Reload to read it again; your changes are kept.`;
  when another tape was inserted in the meantime (another name or file),
  it says `A different tape is in the deck now. Press Reload to read it;
  your changes are kept.`. **Reload** reads the tape in the deck, the
  fields you changed stay, the others take the new values and the title
  shows the name of this tape; **OK** then writes into it.
- When the tape has a further Archive Info block, the dialog says so; that
  block is not changed.
- The information is kept when the tape is saved as TMZ and as TZX (with
  a target machine the file is written in the TMZ format with a warning,
  like other Sharp MZ blocks). A tape opened from another format (MZF,
  MZT, WAV, TAP) cannot keep it in its file: the dialog says `This tape
  file format cannot store this information. Save the tape as TMZ to keep
  it.` and **Save** then opens **Save as...**.

### Checking the tape

**Tape > Check tape** opens the window **Tape check**. It works with a
tape in the deck in any state, also during playback and during an input
replay. It checks the structure of the tape: loops and groups without an
end or a start and nested ones, jumps and calls to blocks outside the
tape, calls without a return block on the tape and returns without a
call, unknown block IDs, program blocks that cannot be read, a header
without the name terminator, an unknown file type and a body length
different from the size in the header (warnings), and profiles the
emulator does not know (information). It does not check the body checksum
and does not play the tape.

- The top line is a summary: `Blocks checked: 5. Warnings: 2.` or `No
  problems found. Blocks checked: 3.`. Each finding is a row with the
  severity (Warning, Info), the number and name of the block and a text.
  At most 500 findings are listed; with more the summary says `more than`.
- A click on a finding selects its block in the block list of the Virtual
  CMT window (the window switches to Full and a collapsed window expands;
  the tape does not move).
- The check reads the tape only when the window opens and with the
  **Check again** button. When the tape changes afterwards (an edit, tape
  information, eject, another tape), the window says `The tape changed.
  Run the check again.` and dims the findings.

### Converting a block to another profile

A program block of the TMZ type MZ File carries a profile: the way its signal
is created - the timing of a particular way of saving (for example SAVE
from the MZ-800 ROM, TurboCopy, Intercopy) or MZ800EMU-CMT, the
emulator's own signal, which blocks from MZF and MZT files, from
recording and from the analysis have (they are MZ File blocks too). The conversion keeps the program
(header and body) unchanged byte for byte and builds its signal with
another profile, for example so that it plays with the original ROM
timing or for a turbo loader.

**Convert to profile...** in the context menu of a row (one selected
block; only an MZ File block whose header size matches its body, otherwise `This block
cannot be converted to another profile.`) opens the dialog `Convert to
profile - block N` with the current profile and a list of profiles with
descriptions (the current one is marked `(current)`; `[unverified]` in a
description marks a profile detail that is not verified). **Speed** can
be chosen only for a profile that accepts a speed (`Profile default` or
a ratio); other profiles show `Set by the profile`.

- Settings of the previous profile that the new profile accepts (the
  pause after the block, the speed and others) are carried over. What
  cannot be carried over is reported in the message line as a warning,
  for example `Settings of profile MZ800EMU-CMT not carried over: speed`,
  otherwise the message is `Block 3 converted to ROM-800-NOMINAL.`.
- The same profile without another speed changes nothing (`Nothing to
  change.`).
- A block of another profile than MZ800EMU-CMT plays at the speed set by
  the conversion; **Set speed** is not available for it and **Signal
  speed** does not apply to it (the dialog reminds you). Only another
  conversion changes its speed; converted back to MZ800EMU-CMT with
  `Profile default` it follows **Signal speed** and **Set speed** works
  again.

## Speed and loading mode

### Loading mode

The **Loading** switch in the Virtual CMT window:

| Mode | What it does |
|------|--------------|
| **Tape** | plays the signal in real time |
| **Boost** | plays the signal, but the emulator runs at maximum speed while the cassette motor runs |
| **Direct** | a patched ROM copies the program into memory without playing the signal and stores a save through the ROM routines straight onto the tape (the CMT hack, see below) |

- Switching to or from Direct mode is possible only with the tape
  stopped. Direct is not available when the tape stands at a block that
  carries no MZF program (without a tape and for an empty tape it can
  always be switched on). Between Tape and Boost you can switch also
  while the tape moves.
- In Direct mode Play is disabled (Stop works when the tape plays, for
  example after **Fast Open CMT File & Play...**), **Signal speed** is not
  used and below the block name there is `Next LOAD takes this block` or
  `This block cannot be loaded directly`. **Record** works as in the other
  modes: while recording, the program's motor commands start and stop the
  recording and the display shows the recording (Recording, Waiting for
  motor, the recording time); **Play** starts a recording waiting for the
  motor; this holds for a save through the ROM routines too (Direct
  Save applies only without Record). Rewind, Previous
  block, Next block and jumping to a block in the list choose the block
  the next load takes.
- **Load directly** in the context menu of a row selects the block and
  switches to Direct mode in one step (only with the tape stopped and for
  a block with an MZF program).
- With a ROM that cannot load directly (the Willy ROMs on the MZ-800, the
  MZ-NEWMON monitor on the MZ-1500, a user-defined ROM with `Disabled CMT
  Hack Patch`) **Direct**, **Load directly** and **Use CMT Hack** are
  disabled and their tooltip says `The selected ROM does not support
  direct loading.`
- The mode cannot be changed while an input replay is recorded or played.
- The main menu items change the mode too, each only one option: **HW &
  Devices > CMT > Use CMT Hack** switches the CMT hack (Direct) on and
  off; after it is switched off the window shows Boost or Tape by the
  option **HW & Devices > CMT > CMT Options > Enable CPU Boost**. Enable
  CPU Boost chooses between Boost and Tape; with the CMT hack on the mode
  stays Direct and the option takes effect only after the hack is
  switched off. The switch in the window sets both options at once (Tape
  and Direct switch CPU Boost off). The same rules apply to them: the CMT hack can be switched
  on or off only with the tape stopped (otherwise the change is refused
  with a message), and while an input replay is recorded or played both
  items are unavailable because the whole HW & Devices menu is
  unavailable.
- Without an `.ini` file (the first start) the CMT hack is on, so the
  window shows Direct. The command line option `--cmt` switches the CMT
  hack off for that session only (see Command line).

### The CMT hack and the tape

The CMT hack (**Direct** mode) loads a program through a patched ROM at
once, without playing the signal. It applies to loads through the ROM
tape routines (the boot menu of the MZ-800 and MZ-1500, loading from the
monitor); a program with its own loader does not use it. For example the
cassette BASICs 1Z-016 V1.0A (MZ-800) and 1Z-013B V1.0A (MZ-700), loaded
from the tape, and a disk BASIC read and write the cassette with their
own routines: with Direct on their `LOAD` from the cassette waits and the
tape does not move (SHIFT+BREAK ends the wait in the cassette BASICs);
load with them in Tape or Boost mode. There are many BASICs and each may
behave differently, so other BASICs and other versions may differ.

With **Direct Save** on (default) and without Record the CMT hack also
patches the ROM save routines. A save through them (for example the save
of the monitor) stores the program at once as a new block at the end of
the tape; the ROM still prints `WRITING` and the file name. Without a
tape a new empty tape is inserted, a playing tape (also paused) is
stopped, and the tape then stands at the new block. The verify routine of
the ROM compares the memory with the program of the block the tape stands
at (right after a save it is the saved program); it does not move the
tape, and a playing tape is stopped only when that block carries a
program. With **Record** pressed the ROM save routines are the original
ones: everything is recorded as signal in Direct mode the same way as in
Tape mode (the program's motor commands start and stop the recording),
with the ROM routines as well as with a program's own save routine (for
example the cassette BASICs named above, which always need **Record**).

- With a tape in the deck, the CMT hack takes the program from the
  current block of the tape and after loading the program body moves the
  tape to the next block with a program. The next load thus gets the next
  program. The deck is not started; a playing tape (also paused) is
  stopped by the first load. The block speed does not apply.
- When the program header announces a longer body than the block
  contains, the load ends with an error and the tape moves to the next
  program (like a real tape). To try it again, move the tape back to that
  block (**Previous block**, or in the block list Ctrl+Enter or **Go to
  this block**).
- When the current block carries no program (TAP, WAV, another TZX block),
  when the last program on the tape has already been loaded, or when
  there is no tape in the deck, the file dialog for an MZF file opens.
- A computer reset forgets the end of the tape, the tape stays where it
  is: the next load after a reset takes the program from the block the
  tape stands at (after the last program, the same program again).
- The snapshot remembers where the CMT hack is on the tape (also the end
  of the tape): loading after a snapshot load continues as if the
  snapshot had not been loaded.
- Switching the CMT hack off (switching to Tape or Boost, or Use CMT Hack
  in the menu) forgets where the hack stopped on the tape (the end of the
  tape and a loaded header without its body); the next load after
  switching it on takes the program from the block the tape stands at.
- **HW & Devices > CMT > CMT Options > Fix FNAME Terminator** (on by
  default): when the CMT hack loads a header whose name has no 0Dh
  terminator, it adds it to the header in the computer's memory.
- **HW & Devices > CMT > CMT Options > Direct Save** (on by default, key
  `direct_save` in the `[CMTHACK]` section of the `.ini` file) switches
  the direct save on and off, only with the tape stopped. Switched off, a
  save through the ROM routines in Direct mode needs **Record** as in Tape
  mode. The item is disabled when the CMT hack is off and with a ROM
  without the standard tape routines.
- A save through the ROM routines that writes only the header (without
  the program) stores nothing in Direct Save; a computer reset, a
  snapshot load or pressing Record forgets such a header.
- **Stop recording** in the middle of a save through the ROM routines in
  Direct mode (after the header, before the program): the recording keeps
  the header as a raw block, and the program written after the stop is
  stored directly as a complete block after it.

### Default speed (Signal speed)

The default speed applies to blocks with a program that have no speed of
their own (for example recorded programs and programs from the analysis).
Set it with the **Signal speed** selector in the Virtual CMT window (the
ratios 1:1 to 3:1 and **Custom pulses...**) or in the menu **HW & Devices
> CMT > Set Default CMT MZ Speed**; it changes only with the tape stopped.
The change rebuilds the current block when it follows the default speed;
other blocks take it when they are opened. A change of the default speed
does not mark the tape as unsaved.

**Custom pulses...** opens the editor **Custom Tape Pulses** with the
pulse lengths in microseconds in the order of the UniCMT CMTSPEED header:
**Long pulse high (bit 1)**, **Long pulse low (bit 1)**, **Short pulse
high (bit 0)** and **Short pulse low (bit 0)**. The **Presets** buttons
(UniCMT 1x, UniCMT 2x, UniCMT 3x) fill in the lengths, **Revert** restores
the lengths from when the editor opened. The line **Equivalent speed**
shows the matching ratio and the speed in Bd. Each length must be greater
than 0 and at most 65535 µs; otherwise **OK** is disabled. The speed
changes only after **OK**.

### Block speed

**Set speed** in the context menu of a row (only with the tape stopped)
gives the block its own speed; the **Speed** column shows it. Only blocks
from MZF, MZT and TAP programs and TMZ blocks with the profile
MZ800EMU-CMT (the emulator's own signal) can be changed; other TMZ / TZX
blocks and a WAV recording play as recorded (`The speed of this block is
given by the file.`). For a block with a program the item **Default**
returns the block to the default speed (Signal speed); for a TAP block
there are only the ratios 1:1, 9:7, 3:2 and 25:14. A block
converted to another profile plays at the speed of the conversion (see
Converting a block to another profile). The choice **Custom pulses...**
opens the pulse length editor (see above).

Changing the speed of the open block rebuilds it and rewinds it to its
start. Changing the speed of a block (to another speed than it has) is an
edit of the tape: the tape then has unsaved changes (see Unsaved
changes).

### Polarity

The switch **HW & Devices > Rear DIP Switch > CMT Polarity** (Normal /
Inverted, changed only with the tape stopped) inverts the signal of an
audio recording from a WAV file, so digital silence reads as the high
level under inverted polarity. It does not affect other blocks, a raw
recording made by recording, or the export of the whole tape. It affects
an imported WAV block only until it is saved: a tape saved to TMZ or TZX
is an ordinary tape and after it is opened the switch no longer affects
the audio recording. A snapshot and an input replay keep the effect of
the switch for an opened WAV file.

### Echo

With the **Echo** checkbox of the Virtual CMT window (at the right end
of its menu bar) or the item **HW & Devices > CMT > CMT Options > Echo** on (off
by default) the signal of the tape is heard from the speaker while the tape
is playing (Tape and Boost mode) and while it is recording. A direct
load and a direct save (Direct mode) have no signal, so they are silent.
The echo shares the sound channel of the computer's beeper (the 8253
timer), so it is as loud as the beeper and a sound recording or a video
recording of the emulator contains it too. It does not change what the
program reads from the tape or what is recorded.

## Starting an MZF file directly

The command line option `--run-mzf` and the MCP tool `emu_media_run_mzf`
start an MZF file directly: the program is put into memory and started
as if the ROM had loaded it from the tape, without playing the tape. The
chips, the memory map, the screen, the monitor work area and the stack
are as the ROM leaves them after a tape load; of the processor registers only BC (see below) and the register
with the start address are set as by the ROM, the others may differ.
A program loaded by the ROM gets in register BC the code of the device it
was loaded from; some programs choose their default device by it (for
example the BASIC 1Z-016 V1.0A with the Quick Disk code uses the Quick
Disk as its default device). The device code for a directly started
program is set in **HW & Devices > CMT > CMT Options > Device for Direct
MZF Start**:

| Item | BC | Meaning |
|------|----|---------|
| **CMT - cassette** (default) | 0100h, on MZ-700 0000h | as after loading from the tape (the MZ-800 and MZ-1500 ROM passes 0100h, the MZ-700 monitor leaves 0000h) |
| **FD - floppy disk** | 0200h | as if loaded from a floppy disk |
| **QD - Quick Disk** | 0300h | as if loaded from a Quick Disk |
| **RD - SRAM** | 0000h | as if loaded from the RD device (SRAM) |
| **Custom value...** | 0 to 65535 | any value, entered in decimal without leading zeros (`512`) or in hexadecimal (`0x0200` or `0200h`); `0200` is refused, because a device code is easily meant as hexadecimal; the dialog shows the current value in hexadecimal |

The value applies from the next direct start; a program that is already
running is not changed. It is saved in the `.ini` file (`[CMT]
run_mzf_device`). For one start only, the command line option
`--run-mzf-device` (see the README) and the parameter `device` of
`emu_media_run_mzf` override it. A load through the ROM (for example the
boot menu or Direct mode) does not use this setting: the ROM sets BC
itself.

## Snapshots and input replay

### Snapshot

A snapshot remembers the path to the tape file, the current block, the
position in it, the block speeds, the deck state (also the pause and the
motor), the playback state (loops, calls) and where the CMT hack is on
the tape. The content of a tape that matches its file is not stored in
the snapshot.

- A tape with unsaved changes or a new tape without a file is stored
  whole in the snapshot (at most 16 MB; a larger one is not stored and a
  warning goes to the log). Loading inserts it again as a tape with
  unsaved changes without a file name (shown as (new tape)), without
  reading the file; the original file name is shown only in the message
  "Unsaved tape restored from the snapshot (was: name)". Save then asks
  for a file name, so a snapshot from someone else cannot decide where
  the tape is saved. When the deck holds a tape with the same file at load
  time (for example Quick Save and Quick Load), the file name stays.
- A tape stored only by its path is inserted from that path when the
  snapshot is loaded (when the same tape with the same blocks is already
  inserted, it is used) and the block, the position, the speeds and the
  deck state are restored.
- When the file is missing, there is no tape in the deck, the state is
  Stopped and a warning goes to the log.
- When the file has different blocks in the meantime (a different number
  or kind), the tape is inserted, stopped at the first block, and a
  warning goes to the log. A change of the block contents with the same
  number and kind of blocks is not detected.
- A tape on a network path (starting with `\\` or `//`) is not opened by
  the snapshot itself: it behaves like a missing file and a warning goes
  to the log. Insert such a tape first and then load the snapshot - the
  inserted tape with the same path is used. Local disks and mapped drive
  letters are opened normally. A tape with unsaved changes from a network
  path also gives a warning when loaded.
- A recording in progress is not stored: a snapshot taken during a
  recording loads with the deck stopped (a warning in the log), and
  loading any snapshot discards a recording in progress.
- Loading a snapshot does not change the default speed, the polarity,
  Boost or the MZF size check you have set. A block that played at a
  different default speed in the snapshot than you have now gets it as
  its own speed.

### Input replay

While an input replay (Replay) is **recorded**, the cassette can be
controlled as usual (transport, eject, new tape, recording), but the tape
cannot be edited, saved, exported or replaced by another one, and the
loading mode and the speeds cannot be changed. A tape with unsaved changes
is stored in the replay; when it does not fit the limit of the replay, the
recording does not start and asks you to save the tape. The questions
before losing changes (see Questions before losing changes) stay, only
**Save tape** is not offered, because the tape cannot be saved then
(**Discard changes** or **Cancel**).

While an input replay is **played**, the tape belongs to the playback
(your tape returns after it) and the Virtual CMT window is locked (`Not
available while an input replay is played.`); that is why the emulator
does not ask about the tape. Closing the emulator window during playback
first stops the playback, so your tape returns, and then asks as usual
when it has unsaved changes. The tape check works also during an input
replay.

## Settings in the .ini file

| Section and key | Meaning | Default |
|-----------------|---------|---------|
| `[CMT] mz_cmtspeed` | default speed (Signal speed), the values the window offers: `SPEED_1/1`, `SPEED_2/1`, `SPEED_2/1_CPM`, `SPEED_7/3`, `SPEED_8/3`, `SPEED_3/1`, `SPEED_CUSTOM` | `SPEED_1/1` |
| `[CMT] mz_custom_pulse_long_high_us`, `mz_custom_pulse_long_low_us`, `mz_custom_pulse_short_high_us`, `mz_custom_pulse_short_low_us` | custom pulse lengths in µs (Custom pulses) | 470, 494, 240, 278 |
| `[CMT] cmt_polarity_inverted` | CMT Polarity: 1 = Inverted | 0 |
| `[CMT] cpu_boost` | Enable CPU Boost (Boost mode) | 1 |
| `[CMT] mzfsize_check` | Enable MZF Size Check | 1 |
| `[CMT] mzfsize_autofix` | Fix MZF Size Automatically: 1 = a file longer than its header says is truncated without asking | 0 |
| `[CMT] echo` | Echo: 1 = the tape signal is heard from the speaker | 0 |
| `[CMT] autorewind` | 1 = the tape rewinds to its first block after it has played to its end | 0 |
| `[CMT] rec_autostop_s` | automatic stop of a recording after that many seconds of silence (0 to 600, 0 = off) | 0 |
| `[CMT] run_mzf_device` | Device for Direct MZF Start: the device code in register BC of a program started directly (`cmt`, `fd`, `qd`, `rd`, `sram` or a number 0 to 65535 written as in the dialog, see Starting an MZF file directly; an invalid value is reported and `cmt` is used) | `cmt` |
| `[CMTHACK] enable` | Use CMT Hack (Direct mode) | 1 |
| `[CMTHACK] fix_fname_terminator` | Fix FNAME Terminator | 1 |
| `[CMTHACK] direct_save` | Direct Save: with the CMT hack and without Record a save through the ROM routines is stored straight onto the tape | 1 |
| `[VIRTUAL_CMT_UI] layout` | window layout: 0 = Compact, 1 = Full | 0 |
| `[VIRTUAL_CMT_UI] compact_w`, `compact_h`, `full_w`, `full_h` | window size of each layout in pixels, 0 = default size | 0 |
| `[VIRTUAL_CMT_UI] props_open` | properties: 0 = collapsed, 1 = expanded | 0 |
| `[VIRTUAL_CMT_UI] auto_analyze` | Find files while playing: 1 = a raw recording that plays is analysed in the background | 1 |

## Command line

The options `--cmt`, `--cmt-play`, `--cmt-play-pause`, `--cmt-boot`,
`--run-mzf` and `--run-mzf-device` are described in the
[README](../README.md#command-line-options).

## Limits

- **Large files**: opening, importing, saving and exporting run in one go,
  so with a large file (for example a long WAV recording) the emulation
  and the emulator window stop for the time of the conversion. The export
  holds the whole audio signal of the tape in memory, so a very long tape
  takes a lot of memory during the export. The analysis runs in the
  background.
- **Recording**: at most 4 hours or 16,000,000 signal edges.
- **Export of the whole tape**: at most 4 hours.
- **Snapshot**: tape content at most 16 MB.
- **Tape information**: 255 characters per text, only ISO 8859-1
  characters.
- **Tape check**: at most 500 findings.
- **Deleting**: a selection of very many separate groups of blocks cannot
  be deleted at once.
- **Network paths**: a snapshot does not open a tape from a network path
  itself.
- **Dropping files**: a Virtual CMT window dragged out of the main window
  may not accept a dropped file.
- The **CMT hack** works only with loading through the ROM routines; the
  block speed does not apply to it.
- **Polarity** affects only an audio recording from a WAV file.
- **Texts from the emulator core** (reasons in the analysis, findings of
  the check, profile descriptions, some warnings) are in English in all
  languages.
