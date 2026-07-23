# mz800emu

Open-source emulator of the 8-bit Sharp MZ-800, MZ-700 and MZ-1500 home computers.

[Česky](README_cz.md)

## Project has moved to GitHub

The project was previously hosted on SourceForge:
https://sourceforge.net/projects/mz800emu/

**Source code is no longer updated on SourceForge** - all development now happens here on GitHub. The SourceForge page will continue to receive release archives only (binary distributions for users who prefer to download from there).

## Features

- Emulation of three Sharp MZ-series machines:
  - **Sharp MZ-800**
  - **Sharp MZ-700**
  - **Sharp MZ-1500**
- Cycle-accurate Z80A CPU emulation (uses the [z80-mz800](https://github.com/michalhucik/z80-mz800) library)
- Faithful emulation of original chips:
  - GDG WHID 65040-032 (video controller)
  - i8253 CTC, Z80 PIO, i8255 PIO
  - SN76489AN PSG
- Precise internal signal timing
- Wide range of supported peripherals:
  - Cassette (CMT): MZF, MZT, TAP, WAV
  - Floppy disk controller (FDC WD279x)
  - Quick Disk
  - RAM disk, memory extensions
  - Unicard
  - IDE8 hard disk interface
- Integrated Z80 debugger:
  - Disassembler with inline assembler
  - Memory browser with heatmap
  - Breakpoints, watchpoints, symbols, bookmarks, variables
- Multi-platform GUI:
  - Virtual keyboard, autotype
  - Joystick support
  - Variable emulation speed
  - Snapshot system (.mzs)
- Full localization into 10 languages (cs, de, en, es, fr, it, ja, nl, pl, sk, uk)

## Technology

- C / C++ (C11 / C++17)
- [SDL3](https://www.libsdl.org/) for video, audio, input
- [Dear ImGui](https://github.com/ocornut/imgui) for the GUI
- [GLib](https://gitlab.gnome.org/GNOME/glib) for utilities
- [libcurl](https://curl.se/libcurl/) and [minizip-ng](https://github.com/zlib-ng/minizip-ng) for I/O
- CMake build system (also supports legacy GNU make on MSYS2)

## Supported platforms

- **Linux** (tested on Ubuntu 24.04)
- **Windows** (MSYS2/MINGW64 toolchain)
- BSD systems may work but are not regularly tested

## Building

Detailed build instructions are available in `docs/`:

- [docs/build_ubuntu.md](docs/build_ubuntu.md) - building on Ubuntu / Linux
- [docs/build_windows.md](docs/build_windows.md) - building on Windows via MSYS2
- [docs/build_osx.md](docs/build_osx.md) - building on macOS

Quick build (when dependencies are already installed):

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

## Documentation

- [docs/](docs/) - user-facing build and usage documentation, changelog

## Related projects

The emulator builds on a family of related libraries and tools maintained in the same author's account:

- [z80-mz800](https://github.com/michalhucik/z80-mz800) - Z80A CPU core and disassembler
- [TapeMZ](https://github.com/michalhucik/TapeMZ) - tape archive format for Sharp MZ
- [mzdisk](https://github.com/michalhucik/mzdisk) - disk image tools for Sharp MZ

## License

GNU General Public License v3.0 (GPLv3). See [LICENSE](LICENSE).

## Author

Michal Hučík (https://github.com/michalhucik)
