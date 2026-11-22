# CDL - export file format

## What CDL is

**CDL** (Code/Data Logger), internally also known as **Memory Heatmap**,
is a module of the mz800new emulator that counts CPU accesses to every
memory cell and every I/O port during program execution. For each cell
it stores four 32-bit counters:

- **R** (read) - number of data reads (excluding the M1 cycle and
  operand bytes of the currently decoded instruction)
- **W** (write) - number of writes outside the stack regions
- **X** (execute) - number of reads as part of the currently decoded
  instruction (M1 opcode fetch and trailing operand bytes)
- **S** (stack write) - number of writes into an active stack region.
  A write counts either as W or as S, never both. Without any stack
  region defined S stays 0.

The classic CDL bitmap (FCEUX-style) can be trivially derived from the
counters by `flag = (count > 0)`.

CDL recording is controlled from the debug window menu under
`Debugger Settings -> CDL`. It has three modes:

- `Off` (default) - no recording, no overhead.
- `Only With Debug Window` - records only while the debug window is open.
- `Always` - records continuously.

The `Export on Exit` and `Set directory...` options in the same menu
control automatic export on emulator shutdown and the target export
directory. All options are persistent in the emulator INI configuration
(section `[DEBUGGER]`, keys `cdl_mode`, `cdl_export_on_exit`,
`cdl_export_dir`).

Recording is active in the shared slow path with Trace Log (instruction
history) so that the hot path of the emulator without active diagnostics
stays unchanged relative to the vanilla version.

## Export directory structure

The export consists of one meta JSON file and a set of binary files,
all together in one directory. The path of the meta file is chosen by
the user (e.g. `./cdl-export/snap1.json`); the basename without
extension (`snap1`) is used as the prefix for region files:

```
<dir>/
    <name>.json                 # meta + regions table
    <name>_bus.cdl              # binary data per region
    <name>_ram.cdl
    <name>_rom-lower.cdl
    ...
```

- `<name>.json` - export metadata (architecture, cell layout, region list)
- `<name>_<region>.cdl` - one binary file per physical region

Per-region files are always referenced only via `regions[].file` in
JSON, so import tools do not need to know the naming convention - they
parse the JSON and open files exactly at the listed paths (relative to
the meta file directory).

Each `.cdl` file is a flat array of `st_MHMAP_CELL` structures:

```c
struct {
    uint32_t r;     // Little-endian
    uint32_t w;     // Little-endian
    uint32_t x;     // Little-endian
    uint32_t s;     // Little-endian
};
```

Cell size is 16 bytes. Write endianness is host byte order; on all
supported emulator platforms (Windows x86_64, Linux x86_64) this is
little-endian. This is explicitly declared in `meta.json` in the fields
`cell_size_bytes` and `cell_layout`.

File size = `size_cells * 16 B`. Specific sizes are arch-specific - see
tables below.

## meta.json

JSON file with the following structure:

```json
{
  "format_version": 2,
  "format": "memory-heatmap",
  "mzarch": 800,
  "created_at": "2026-05-01T00:38:42",
  "file_prefix": "snap1",
  "cell_size_bytes": 16,
  "cell_layout": "r:uint32_le, w:uint32_le, x:uint32_le, s:uint32_le",
  "regions": [
    { "name": "bus", "file": "snap1_bus.cdl", "size_cells": 65536, "size_bytes": 1048576 },
    { "name": "ram", "file": "snap1_ram.cdl", "size_cells": 65536, "size_bytes": 1048576 },
    ...
  ]
}
```

Key fields:

- `format_version` - format version, currently `2`. Incremented on an
  incompatible change of cell layout or metadata.
- `format` - format identifier, always `"memory-heatmap"`.
- `mzarch` - target emulator architecture (`800` or `1500`).
- `created_at` - ISO 8601 timestamp of export creation (local time).
  Optional for `format_version >= 2`; if missing, the importer shows
  `(no timestamp)`.
- `file_prefix` - basename of the meta file without `.json`. Serves
  only as human-readable identification; specific file paths are in
  `regions[].file`.
- `cell_size_bytes` - size of one cell in bytes, `16`. A reader should
  take the cell size from this field; the importer in the emulator
  rejects any other value.
- `cell_layout` - description of cell byte layout, for documentation.
- `regions[]` - list of exported regions. Each region has a symbolic
  `name`, relative `file` (against the meta file directory),
  `size_cells` and `size_bytes`.

The order of items in `regions[]` is stable for a given architecture -
tools can index by name (`name`).

### Changes between versions

| Version | Change |
|---------|--------|
| 1 | Original layout. Region files named directly `<region>.cdl` (without prefix), only the `meta.json` itself in the directory. |
| 2 | Added `created_at` and `file_prefix`. Region files named `<prefix>_<region>.cdl` in the meta file directory. Import must read `regions[].file` (must not derive from `name`). The cell has 16 B with the S counter (`cell_size_bytes` 16) in all released versions. |

## MZ-800 (and MZ-700 mode)

The MZ-800 emulator also covers MZ-700 mode, switched at runtime via
the DMD register. VRAM recording is split into two groups:

- **Physical view** (`vram.cdl`, 32 KB) - what actually happened with
  the physical VRAM banks regardless of the graphics mode. Recording
  by WE bitmask (write) or RF_PLANE (read) - reflects what the hardware
  actually did.
- **Mode-specific view** - what the program did in the given graphics
  mode from the perspective of logical planes. Recording by WF_PLANE
  / RF_PLANE.

Programs may deliberately switch modes to take advantage of different
arrangements of physical banks into logical planes. Per-mode recording
makes it possible to distinguish what was done in which mode; the
physical view reflects the sum of real HW operations.

Total **22 regions**, approximately **5 MB** of data (plus 8 MB of
`memext.cdl` with Memext connected):

| File | Cells | Bytes | Description |
|------|-------|-------|-------------|
| `bus.cdl` | 65536 | 1048576 | Logical CPU address 0000h-FFFFh |
| `ram.cdl` | 65536 | 1048576 | Main DRAM 64 KB (including under ROM) |
| `rom-lower.cdl` | 4096 | 65536 | ROM monitor 0000h-0FFFh |
| `rom-cg.cdl` | 4096 | 65536 | CG-ROM 1000h-1FFFh |
| `rom-upper.cdl` | 8192 | 131072 | ROM monitor E000h-FFFFh |
| `vram.cdl` | 32768 | 524288 | **Physical VRAM 32 KB - linear stack of 4 banks of 8 KB (VRAM1..4)** |
| `vram700-cg.cdl` | 4096 | 65536 | MZ-700 CG-RAM (bus C000-CFFF) |
| `vram700.cdl` | 4096 | 65536 | MZ-700 text+attr VRAM (bus D000-DFFF) |
| `vram800-320x200_16-I.cdl` | 8192 | 131072 | 320@HICOLOR mode, logical plane I |
| `vram800-320x200_16-II.cdl` | 8192 | 131072 | 320@HICOLOR, plane II |
| `vram800-320x200_16-III.cdl` | 8192 | 131072 | 320@HICOLOR, plane III |
| `vram800-320x200_16-IV.cdl` | 8192 | 131072 | 320@HICOLOR, plane IV |
| `vram800-320x200_4A-I.cdl` | 8192 | 131072 | 320@4 bank A, plane I |
| `vram800-320x200_4A-II.cdl` | 8192 | 131072 | 320@4 bank A, plane II |
| `vram800-320x200_4B-I.cdl` | 8192 | 131072 | 320@4 bank B, plane I |
| `vram800-320x200_4B-II.cdl` | 8192 | 131072 | 320@4 bank B, plane II |
| `vram800-640x200_4-I.cdl` | 16384 | 262144 | 640@4 mode, plane I |
| `vram800-640x200_4-II.cdl` | 16384 | 262144 | 640@4 mode, plane II |
| `vram800-640x200_2A.cdl` | 16384 | 262144 | 640@2 bank A |
| `vram800-640x200_2B.cdl` | 16384 | 262144 | 640@2 bank B |
| `iorq-8bit.cdl` | 256 | 4096 | 8-bit I/O ports 00h-FFh |
| `iorq-gdg.cdl` | 256 | 4096 | 16-bit GDG sub-functions, placeholder |
| `memext.cdl` | 524288 | 8388608 | Memext RAM, flat 512 KB (only if Memext is connected) |

### Layout of `vram.cdl` (physical 32 KB)

Linear stack of 4 physical banks of 8 KB:

| Offset | Bank | Size |
|--------|------|------|
| 0x0000-0x1FFF | VRAM1 | 8 KB |
| 0x2000-0x3FFF | VRAM2 | 8 KB |
| 0x4000-0x5FFF | VRAM3 | 8 KB |
| 0x6000-0x7FFF | VRAM4 | 8 KB |

Recording into `vram.cdl` happens on **every** VRAM access, regardless
of the current graphics mode. The bank-mask comes from hardware:

- **Write**: `WE` bitmask (= result of SINGLE/EXOR/OR/RESET/REPLACE/PSET
  mode evaluation + planar interleave)
- **Read**: `RF_PLANE` bitmask (= active plane in the RF register)

### Mapping of CPU bus to regions (MZ-800 mode)

| Bus | Banking condition | Recording |
|-----|-------------------|-----------|
| 0000h-0FFFh | ROM_0000 mapped | bus + rom-lower[bus_addr] |
| 0000h-0FFFh | ROM_0000 unmapped | bus + ram[bus_addr] |
| 1000h-1FFFh | ROM_1000 mapped | bus + rom-cg[bus_addr-1000h] |
| 1000h-1FFFh | ROM_1000 unmapped | bus + ram[bus_addr] |
| 2000h-7FFFh | (always) | bus + ram[bus_addr] |
| 8000h-9FFFh | VRAM_8000 mapped | bus + vram[bank+phy] + mode-specific (320 or 640 mode per DMD) |
| 8000h-9FFFh | VRAM_8000 unmapped | bus + ram[bus_addr] |
| A000h-BFFFh | VRAM_A000 mapped (640 mode) | bus + vram[bank+phy] + 640 mode-specific |
| A000h-BFFFh | VRAM_A000 unmapped | bus + ram[bus_addr] |
| E000h-FFFFh | ROM_E000 mapped + Prohibited (any DMD) | (no physical target - shadow 0x1A) |
| E000h-FFFFh | ROM_E000 mapped + 700 mode + addr <= E008h | (no physical target - mapped ports PIO/CTC/GDG) |
| E000h-FFFFh | ROM_E000 mapped + 800 native + addr <= E00Fh | (no physical target - mapped ports area "off", 0xFF) |
| E000h-FFFFh | ROM_E000 mapped, other (NOT Prohibited) | bus + rom-upper[bus_addr-E000h] |
| E000h-FFFFh | ROM_E000 unmapped | bus + ram[bus_addr] |

Note: "Prohibited" = state after OUT 0xE5, persists across OUT E0/E1/E2/E3
and across DMD bit 3 switch (700 <-> 800 native). Cleared by OUT 0xE6,
OUT 0xE4 or reset (on the MZ-800 measured on a real machine). In the
Prohibited state the CPU reads, over the entire $E000-$FFFF, the last byte
that was on the data bus (for a data read the last byte of the instruction)
and writes are dropped - no physical region (skip resolver).

### Mapping of CPU bus to regions (MZ-700 mode)

In MZ-700 mode, only VRAM1 (Plane I) is physically used. Recording
happens to the mode-specific MZ-700 files **and** to the global
`vram.cdl`:

| Bus | Banking condition | Recording |
|-----|-------------------|-----------|
| C000h-CFFFh | CGRAM mapped | bus + vram700-cg[bus_addr & FFFh] + vram[VRAM1_base + (bus_addr & FFFh)] |
| C000h-CFFFh | CGRAM unmapped | bus + ram[bus_addr] |
| D000h-DFFFh | VRAM_D000 mapped | bus + vram700[bus_addr & FFFh] + vram[VRAM1_base + 1000h + (bus_addr & FFFh)] |
| D000h-DFFFh | VRAM_D000 unmapped | bus + ram[bus_addr] |
| E000h-FFFFh | ROM_E000 mapped + Prohibited | (no physical target - 0x1A/0xFF shadow) |
| E000h-FFFFh | ROM_E000 mapped + 700 mode + addr <= E008h | (no physical target - mapped ports PIO/CTC/GDG) |
| E000h-FFFFh | ROM_E000 mapped + 800 native + addr <= E00Fh | (no physical target - mapped ports area "off") |
| E000h-FFFFh | ROM_E000 mapped, other (NOT Prohibited) | bus + rom-upper[bus_addr - E000h] |
| E000h-FFFFh | ROM_E000 unmapped | bus + ram[bus_addr] |

Note: see "Prohibited" section in the MZ-800 mapping above. Unified
banking $E5/$E6 behavior across MZ-700 and MZ-800.

### Mode-specific recording (MZ-800 graphics modes)

Recording into mode-specific files is governed by **DMD bits** (HICOLOR,
SCRW640) and **VBANK** (bit 4 in WF/RF registers). Rules for mapping
the plane bitmask to files:

| Mode | DMD bits | VBANK | Plane mapping |
|------|----------|-------|---------------|
| 320x200@16 (hicolor) | HICOLOR=1, SCRW640=0 | - | PLANE1->I, PLANE2->II, PLANE3->III, PLANE4->IV |
| 320x200@4 bank A | HICOLOR=0, SCRW640=0 | A=0 | PLANE1->I, PLANE2->II |
| 320x200@4 bank B | HICOLOR=0, SCRW640=0 | B=1 | PLANE3->I, PLANE4->II |
| 640x200@4 | HICOLOR=1, SCRW640=1 | - | PLANE1->I, PLANE3->II |
| 640x200@2 bank A | HICOLOR=0, SCRW640=1 | A=0 | PLANE1-> single plane |
| 640x200@2 bank B | HICOLOR=0, SCRW640=1 | B=1 | PLANE3-> single plane |

**Indexing of mode-specific files**: bus offset (= `bus_addr & 0x3fff`),
i.e. 0-0x1FFF (8 KB) for 320 modes, 0-0x3FFF (16 KB) for 640 modes.

**Plane bitmask** comes from:
- WF_PLANE for writes (= software intent, where the program wanted to
  write)
- RF_PLANE for reads (= active plane)

Mode-specific recording is activated **only in the given mode** - when
the program switches from 320@16 to 320@4A, further writes go to the
4A files, not to the 16 files. This makes it possible to distinguish
what the program did in each mode.

### Mode-specific vs physical view

Per-mode files capture **logical planes in the given mode**, indexed
by bus offset. `vram.cdl` captures **physical banks** (VRAM1..4)
indexed by plane offset (= addr after >>1 in 640 mode and hwscroll).

Example of a write to bus 0x8000 in 640x200@4 mode into both logical
planes:

| File | Offset | Reason |
|------|--------|--------|
| `bus.cdl` | 0x8000 | logical access |
| `vram.cdl` | bank+phy (4 possible per WE) | physical HW banks |
| `vram800-640x200_4-I.cdl` | 0x0000 | logical plane I |
| `vram800-640x200_4-II.cdl` | 0x0000 | logical plane II |

### IORQ regions

`iorq-8bit.cdl` records 8-bit I/O ports (`port & 0xFF`). Only R and W
counters are logged; X and S make no sense for ports (Z80 does not read
a port as an instruction).

`iorq-gdg.cdl` is reserved for 16-bit GDG sub-functions via the B
register high byte (the only device in MZ-800 with 16-bit IORQ
addressing). Currently it is a placeholder with 256 cells - the exact
layout will be specified later.

### Memext region

`memext.cdl` (512 KB, available for MZ-800 and MZ-1500) records access
to the RAM of the Memext peripheral. Recording happens **in addition**
to the log into `ram.cdl`, when a RAM access went through a mapped
Memext bank.

Indexing: flat 512 KB. The bus address is converted to an offset in
Memext RAM through the current banking map, plus `addr & 0x0FFF`
within the 4 KB bank window.

Recording happens only if Memext is actually connected
(`MEMEXT_TEST_CONNECTED`) and the access ended up in the Memext RAM
range (= not in WOM or outside). Otherwise the whole region is zero -
such an export can be ignored.

The region does not distinguish **type** (PEHU vs Luftner) - both are
logged into the same `memext.cdl`. For a disassembler/CDL flag analyzer
this does not matter (the type affects only mapping, not the RAM
layout).

## MZ-1500

The layout differs from MZ-800 - MZ-1500 has no pixel-oriented VRAM,
no WF/RF registers, no GDG 16-bit IORQ. Instead, it has 3 PCG banks
(programmable character generator) and CGROM seen on the bus through
SPEC bits in the banking map.

Total **9 regions**, approximately **2.9 MB** of data (plus 8 MB of
`memext.cdl` with Memext connected):

| File | Region | Cells | Bytes | Description |
|------|--------|-------|-------|-------------|
| `bus.cdl` | bus | 65536 | 1048576 | Logical CPU address 0000h-FFFFh |
| `ram.cdl` | ram | 65536 | 1048576 | Main DRAM 64 KB |
| `rom.cdl` | rom | 16384 | 262144 | Monitor ROM, 16 KB (lower 0000h + upper E010h-FFFFh in one blob) |
| `cgrom.cdl` | cgrom | 4096 | 65536 | CG-ROM 4 KB (mapped to D000h-DFFFh or E000h-EFFFh via SPEC=1) |
| `vram.cdl` | vram | 4096 | 65536 | Character + attribute VRAM, 4 KB |
| `pcg-1.cdl` | pcg-1 | 8192 | 131072 | PCG bank 1, 8 KB - in hardware carries the R component of sprite color |
| `pcg-2.cdl` | pcg-2 | 8192 | 131072 | PCG bank 2, 8 KB - G component |
| `pcg-3.cdl` | pcg-3 | 8192 | 131072 | PCG bank 3, 8 KB - B component |
| `iorq-8bit.cdl` | iorq-8bit | 256 | 4096 | 8-bit I/O ports 00h-FFh |
| `memext.cdl` | memext | 524288 | 8388608 | Memext RAM, flat 512 KB (only if Memext is connected) |

### Mapping of CPU bus to regions

| Bus | Banking | SPEC | Region | Offset |
|-----|---------|------|--------|--------|
| 0000h-0FFFh | ROM_0000 mapped | - | rom | bus_addr (= 0000h-0FFFh in ROM blob) |
| 0000h-0FFFh | ROM_0000 unmapped | - | ram | bus_addr |
| 1000h-CFFFh | (always) | - | ram | bus_addr |
| D000h-DFFFh | ROM_UPPER unmapped | - | ram | bus_addr |
| D000h-DFFFh | ROM_UPPER mapped | SPEC=0 (default) | vram | bus_addr & 0FFFh |
| D000h-DFFFh | ROM_UPPER mapped | SPEC=1 | cgrom | bus_addr & 0FFFh |
| D000h-DFFFh | ROM_UPPER mapped | SPEC=2..4 | pcg-1 / pcg-2 / pcg-3 | (bus_addr & 3FFFh) - 1000h = 0000h-0FFFh |
| E000h-EFFFh | ROM_UPPER unmapped | - | ram | bus_addr |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=0, addr <= E00Fh | (no physical target - bus map only) | memory-mapped ports |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=0, addr > E00Fh | rom | bus_addr & 3FFFh (= 2010h-2FFFh in ROM) |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=1 | cgrom | bus_addr & 0FFFh |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=2..4 | pcg-1..3 | (bus_addr & 3FFFh) - 1000h = 1000h-1FFFh |
| F000h-FFFFh | ROM_UPPER unmapped | - | ram | bus_addr |
| F000h-FFFFh | ROM_UPPER mapped | SPEC=0 | rom | bus_addr & 3FFFh (= 3000h-3FFFh in ROM) |
| F000h-FFFFh | ROM_UPPER mapped | SPEC<>0 | (returns 0xFF, no physical target) | - |

### PCG banks

A PCG bank is physically 8 KB. From the CPU bus, a 4 KB window of the
active bank is visible according to SPEC state:

- bus 0xD000-0xDFFF (4 KB) -> PCG offset 0x0000-0x0FFF
- bus 0xE000-0xEFFF (4 KB) -> PCG offset 0x1000-0x1FFF

The active PCG bank (1, 2 or 3) is selected by the SPEC value in the
banking map (SPEC=2 for PCG_1, SPEC=3 for PCG_2, SPEC=4 for PCG_3).

For MZ-1500 sprite graphics, PCG[1..3] hold three R, G, B color
components - combined per pixel into 8 colors (palette like MZ-700 text
mode).

## Usage

### From the UI

In the debug window menu `Debugger Settings -> CDL`:

1. Select recording mode (Off / Only With Debug Window / Always).
2. Optionally enable `Export on Exit` and set the target directory via
   `Set directory...`.
3. Run the emulated program. Counters are updated at runtime.
4. On emulator shutdown, the CDL directory is automatically exported
   (if `Export on Exit` was enabled).

### Loading data in Python (example)

The argument is the path to the meta JSON file (e.g.
`./cdl-export/snap1.json`). Region files are looked up in its parent
directory according to `regions[].file`:

```python
import json, struct
from pathlib import Path

def load_cdl(meta_path):
    meta_path = Path(meta_path)
    meta = json.loads(meta_path.read_text())
    base_dir = meta_path.parent
    k = meta["cell_size_bytes"] // 4          # uint32 counters per cell (4)
    cells = {}
    for region in meta["regions"]:
        data = (base_dir / region["file"]).read_bytes()
        n = region["size_cells"]
        counters = struct.unpack(f"<{k*n}I", data)
        # (r, w, x, s) per cell
        cells[region["name"]] = [tuple(counters[k*i:k*i+k]) for i in range(n)]
    return meta, cells

meta, cells = load_cdl("./cdl-export/snap1.json")
# Example: bytes in RAM that were ever executed as code
code_bytes = [i for i, c in enumerate(cells["ram"]) if c[2] > 0]
```
