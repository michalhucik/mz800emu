# Building mz800emu on Ubuntu 24.04

This guide describes how to compile the `mz800emu` emulator on Ubuntu. It will walk you through installing necessary development tools, downloading the source code, and building the executable.

## 1) Update your system

Before installing any packages, update your package database and upgrade existing packages:

```sh
sudo apt update
sudo apt upgrade
```

## 2a) Install required development packages

Install the base development tools and libraries required to compile `mz800emu`:

```sh
sudo apt install build-essential cmake ninja-build pkg-config git doxygen
sudo apt install libglib2.0-dev libjson-glib-dev libcurl4-openssl-dev zlib1g-dev libgl-dev
```

Notes:
- The build system uses CMake (3.20+) with Ninja as the preferred generator.
- `libjson-glib-dev` is required since the `D.0.5.B.1` release (build/cmake commit `48ed161`).
- `zlib1g-dev` is required because `minizip-ng` is typically available only as a static `.a`
  archive on Linux and `find_package(ZLIB)` in CMake links it explicitly to resolve
  `inflateEnd`/`deflateEnd` symbols. zlib is also used directly by video recording
  (ZMBV codec).

## 2b) Install SDL3 and SDL3-image

SDL3 and SDL3-image now is not available as oficial Ubuntu package.
Tady je můj postup jak nakompilovat vlastní SDL3 a SDL3_Image, nicméně lepší bude, když si ověříte oficiální postup zde https://www.libsdl.org/

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build python3-pip libx11-dev libxext-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libxi-dev libwayland-dev wayland-protocols libdrm-dev libgbm-dev \
  libasound2-dev libpulse-dev libaudio-dev libxrender-dev libxfixes-dev libxss-dev libdbus-1-dev \
  libudev-dev libgles2-mesa-dev libegl1-mesa-dev libibus-1.0-dev fcitx-libs-dev libsamplerate0-dev \
  libpipewire-0.3-dev libdecor-0-dev git



cd /usr/local/src
git clone https://github.com/libsdl-org/SDL.git
cd SDL
git checkout main
mkdir build
cd build
cmake .. -G Ninja -DSDL_TEST=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF
ninja
sudo ninja install
sudo ldconfig
pkg-config --modversion sdl3
```

Build SDL3_Image:

```sh
cd /usr/local/src
git clone https://github.com/libsdl-org/SDL_image.git
cd SDL_image
git checkout main
mkdir build
cd build
cmake .. -DSDL3IMAGE_VENDORED=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
make -j$(nproc)
sudo make install
pkg-config --modversion sdl3-image
```

## 2c) Install minizip-ng

minizip-ng is not available as an official Ubuntu package, so it must be compiled from source.
Only zlib compression (DEFLATE) is needed — all other compression and encryption modules must be
disabled to avoid additional dependencies (zstd, bzip2, lzma, ppmd, openssl):

```sh
cd /usr/local/src
sudo git clone https://github.com/zlib-ng/minizip-ng.git
cd minizip-ng
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DMZ_COMPAT=OFF \
  -DMZ_ZSTD=OFF \
  -DMZ_BZIP2=OFF \
  -DMZ_LZMA=OFF \
  -DMZ_PPMD=OFF \
  -DMZ_PKCRYPT=OFF \
  -DMZ_WZAES=OFF \
  -DMZ_OPENSSL=OFF \
  -DMZ_LIBBSD=OFF
cmake --build build -j$(nproc)
sudo cmake --install build
sudo ldconfig
pkg-config --modversion minizip-ng
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
