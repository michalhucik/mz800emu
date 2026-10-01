# Building mz800emu on Windows (64-bit)

I use MSYS2 to compile the program for Windows (64 bit). Here is a step-by-step guide to install the environment, libraries, and create a distribution directory with the `mz800emu` program:

## 1) MSYS2

Download and install the MSYS2 environment from [https://www.msys2.org/](https://www.msys2.org/)

Immediately after installation, I recommend setting an exception in your antivirus system for the directory structure `C:\msys64`.

MSYS2 contains several pre-built environments. We will use the **MSYS2 MINGW64** environment to build `mz800emu`. I recommend pinning its icon to your desktop and choosing a more readable font for the terminal.

Your MSYS2 home directory is accessible from Windows as `C:\msys64\home\<username>`.

## 2) Environment update

Not only right after installation — it's always a good idea to keep your system packages up to date.

Synchronize the package database:

```sh
pacman -Syy
```

Then update it:

```sh
pacman -Syuu
```

> Please note that after the first installation you will probably need to repeat these steps several times.
> I recommend creating a file `do_update.sh` in your home directory and using it to perform the update.

## 3) Development packages

### Base development packages

```sh
pacman -S base-devel mingw-w64-x86_64-toolchain git doxygen
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf
```

### File manipulation and console editors (optional)

```sh
pacman -S mc nano vim
```

### Packages necessary for mz800emu

```sh
pacman -S mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-image
pacman -S mingw-w64-x86_64-glib2 mingw-w64-x86_64-json-glib
pacman -S mingw-w64-x86_64-curl mingw-w64-x86_64-minizip-ng
```

Notes:
- The build system uses CMake (3.20+) with Ninja as the preferred generator.
- `mingw-w64-x86_64-json-glib` is required since the `D.0.5.B.1` release
  (build/cmake commit `48ed161`).
- On MSYS2 `minizip-ng` is provided as a DLL package, so unlike on Linux there
  is no need to install `zlib` explicitly - it is pulled in transitively.
- The `Makefile` is a thin wrapper around CMake. It defaults to the **UCRT64**
  toolchain (modern CRT, better C99/C11 support) when invoked from a UCRT64
  shell or with `make MSYSTEM=UCRT64`. From a MINGW64 shell it picks MINGW64
  unless overridden. When switching toolchains, run `make clean` first to
  avoid mixed build cache.

## 4) Download latest mz800emu code

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

From a regular Windows PowerShell (including the integrated terminal used by
Codex or VS Code), use the checked-in wrapper instead. It supplies the native
Windows `PATH` required by GCC child processes and uses the UCRT64 toolchain:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\windows-dev.ps1 -Action Build
powershell -ExecutionPolicy Bypass -File .\tools\windows-dev.ps1 -Action Run
```

Use `-BuildType Release`, `-Target mz1500emu` (or another emulator target),
and `-Action Test` as needed. The default is a Debug build of `mz800emu`.

The build system automatically uses all available CPU cores for parallel compilation.

By default, `make` builds all supported targets (`mz800emu` and `mz1500emu`). You can also build a specific target:

```sh
make mz800emu
make mz1500emu
```

For a list of all available build, test, and i18n targets, run:

```sh
make help
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

## 4a) Compile locale files (optional)

If you want translations (Czech, German, Japanese, etc.), you need to compile `.po` files into `.mo` binary catalogs. The build system will remind you if `.mo` files are missing.

```sh
make i18n-compile-all
```

## 5) Running and distribution

If the program was successfully compiled, you can run it directly from the MSYS2 environment:

```sh
./mz800emu.exe
```

If you want to run the program separately in Windows without the presence of the MSYS2 environment, you need to create a distribution directory:

```sh
make dist
```

All necessary files (binaries, DLLs, docs, locale files) will be copied to the `./dist` directory. The `mz800emu` program is compiled to run without a text console by default.

If you want to run the program with a text console, create a Windows shortcut to the `mz800emu.exe` file in the `./dist` directory and add the `--console` parameter to the run. Alternatively, you can compile with `make FORCE_CONSOLE=1`.
