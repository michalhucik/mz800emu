# Building mz800emu on macOS Tahoe 26.5.2 

This guide describes how to compile the `mz800emu` emulator on macOS. It will walk you through installing necessary development tools, downloading the source code, and building the executable.

## 0) Prerequisites

You need to know how to open and use the terminal.

You need XCode command line developer tools:
```xcode-select --install```

You need Homebrew https://brew.sh :
```/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"```

## 1) Update your system

Make sure your system is updated: 
```softwareupdate --install --all```

Make sure Homebrew is updated:
```
brew update
brew upgrade
```

## 2a) Install required development packages

Install the base development tools and libraries required to compile `mz800emu`:

```
brew install cmake ninja pkg-config git doxygen glib json-glib curl zlib
``` 

Notes:
- The build system uses CMake (3.20+) with Ninja as the preferred generator.
- `libjson-glib-dev` is required since the `D.0.5.B.1` release (build/cmake commit `48ed161`).
- `zlib1g-dev` is required because `minizip-ng` is typically available only as a static `.a`
  archive on Linux and `find_package(ZLIB)` in CMake links it explicitly to resolve
  `inflateEnd`/`deflateEnd` symbols.

## 2b) Install SDL3 and SDL3-image

```
brew install sdl3 sdl3_image
```

## 2c) Install minizip-ng

```
brew install minizip-ng 
```

## 3) Download the latest mz800emu code

The project moved from SourceForge to GitHub - the SourceForge SVN
repository is no longer updated. Clone the source code from GitHub:

```sh
git clone https://github.com/michalhucik/mz800emu.git
cd mz800emu
```

If you want to build a specific tagged release instead of the latest
development tip, check it out after cloning, for example:

```sh
git checkout v2.0.2-preview
```

Compile the program:

```sh
make
```

### Build without the debugger (optional)

The emulator ships with a built-in debugger (memory map, breakpoints, watch,
callstack, profiler, trace logs, ...). If you do not need it, you can build
a slimmed-down binary by passing `NO_DEBUGGER=1` on the `make` command line:

```sh
make clean
make NO_DEBUGGER=1
```

The CMake configure step prints `MZ_NO_DEBUGGER: ON (debugger subsystem
disabled)` to confirm the flag took effect. The resulting binary is roughly
**15 % smaller** (about -6.7 MB) and the emulator should run slightly faster
(no debug callbacks on the CPU hot path).

The `make clean` before re-configuring is required because CMake caches the
flag - without a clean build the previous configuration would be reused.

Internally `NO_DEBUGGER=1` is forwarded to CMake as `-DMZ_NO_DEBUGGER=ON`,
which defines the global `MZ800EMU_NO_DEBUGGER` macro. The macro suppresses
`MZ800EMU_CFG_DEBUGGER_ENABLED` in `src/emulator/mzarch/mzarch_config.h`.

## 3a) Compile locale files (optional)

If you want translations (Czech, German, Japanese, etc.), you need to compile `.po` files into `.mo` binary catalogs. The build system will remind you if `.mo` files are missing.

```sh
make i18n-compile-all
```

## 4) Running the program

If the program was successfully compiled, you can run it directly from the terminal:

```sh
./mz800emu
```

## 5) Creating a distribution directory

If you want to prepare a directory with all necessary files for distribution:

```sh
make dist
```

All required files will be copied into the `./dist` directory.
