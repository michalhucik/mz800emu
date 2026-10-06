# mz800emu

[![Build](https://github.com/michalhucik/mz800emu/actions/workflows/build.yml/badge.svg)](https://github.com/michalhucik/mz800emu/actions/workflows/build.yml)

Open-source emulátor 8bitových počítačů Sharp MZ-800, MZ-700 a MZ-1500.

[English](README.md)

## Projekt se přesunul na GitHub

Projekt byl dříve hostován na SourceForge:
https://sourceforge.net/projects/mz800emu/

**Zdrojové kódy se na SourceForge již nebudou aktualizovat** - veškerý vývoj nyní probíhá zde na GitHubu. Na SourceForge se budu snažit nahrávat pouze archivy s novými release (binární distribuce pro uživatele, kteří preferují stahování odtamtud).

## Vlastnosti

- Emulace tří strojů řady Sharp MZ:
  - **Sharp MZ-800**
  - **Sharp MZ-700**
  - **Sharp MZ-1500**
- Cyklově přesná emulace procesoru Z80A (využívá knihovnu [z80-mz800](https://github.com/michalhucik/z80-mz800))
- Věrná emulace originálních čipů:
  - GDG WHID 65040-032 (video kontrolér)
  - i8253 CTC, Z80 PIO, i8255 PIO
  - SN76489AN PSG
- Přesné časování vnitřních signálů
- Široká podpora periferií:
  - Kazeta (CMT): MZF, MZT, TAP, WAV
  - Řadič disket (FDC WD279x)
  - Quick Disk (`.mzq` a staré, HxC i FlashFloppy `.qd` čtení/zápis)
  - RAM disk, paměťová rozšíření
  - Unicard
  - IDE8 rozhraní pro pevný disk
- Integrovaný Z80 debugger:
  - Disassembler s inline assemblerem
  - Prohlížeč paměti s heatmapou
  - Breakpointy, watchpointy, symboly, záložky, proměnné
- Multiplatformní GUI:
  - Virtuální klávesnice, autotype
  - Podpora joysticku
  - Volitelná rychlost emulace
  - Systém snapshotů (.mzs)
- Plná lokalizace do 10 jazyků (cs, de, en, es, fr, it, ja, nl, pl, sk, uk)

## Použité technologie

- C / C++ (C11 / C++17)
- [SDL3](https://www.libsdl.org/) pro video, zvuk, vstupy
- [Dear ImGui](https://github.com/ocornut/imgui) pro GUI
- [GLib](https://gitlab.gnome.org/GNOME/glib) pro utility
- [libcurl](https://curl.se/libcurl/) a [minizip-ng](https://github.com/zlib-ng/minizip-ng) pro I/O
- Build systém CMake (na MSYS2 funguje i klasický GNU make)

## Podporované platformy

- **Linux** (testováno na Ubuntu 24.04)
- **Windows** (toolchain MSYS2/MINGW64)
- BSD systémy mohou fungovat, ale nejsou pravidelně testovány

## Sestavení

Podrobné instrukce pro sestavení jsou v adresáři `docs/`:

- [docs/build_ubuntu.md](docs/build_ubuntu.md) - sestavení na Ubuntu / Linux
- [docs/build_windows.md](docs/build_windows.md) - sestavení na Windows přes MSYS2

Rychlé sestavení (pokud máte nainstalované závislosti):

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

## Dokumentace

- [docs/](docs/) - uživatelská dokumentace (build, použití, changelog)

## Související projekty

Emulátor stojí na řadě souvisejících knihoven a nástrojů ve stejném účtu:

- [z80-mz800](https://github.com/michalhucik/z80-mz800) - jádro emulace CPU Z80A a disassembler
- [TapeMZ](https://github.com/michalhucik/TapeMZ) - formát archivu pásek Sharp MZ
- [mzdisk](https://github.com/michalhucik/mzdisk) - nástroje pro práci s diskovými obrazy Sharp MZ

## Licence

GNU General Public License v3.0 (GPLv3). Viz [LICENSE](LICENSE).

## Autor

Michal Hučík (https://github.com/michalhucik)
