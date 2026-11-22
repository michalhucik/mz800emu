# CDL - formát exportních souborů

## Co je CDL

**CDL** (Code/Data Logger), interně též označovaný jako **Memory Heatmap**,
je modul mz800new emulátoru, který během běhu programu počítá přístupy CPU
ke každé paměťové buňce a každému I/O portu. Pro každou cellu ukládá
čtyři 32-bit čítače:

- **R** (read) - počet datových čtení (mimo M1 cyklus a operand bajt
  právě dekódované instrukce)
- **W** (write) - počet zápisů mimo stack regiony
- **X** (execute) - počet čtení jako součást právě dekódované instrukce
  (M1 opcode fetch a navazující operand bajty)
- **S** (stack write) - počet zápisů do aktivního stack regionu. Zápis
  se počítá buď jako W, nebo jako S, nikdy oboje. Bez definovaného stack
  regionu zůstává S nulové.

Klasický CDL bitmap (FCEUX-style) lze z counterů triviálně odvodit
předpisem `flag = (count > 0)`.

CDL recording se ovládá z menu debug okna v položce
`Debugger Settings -> CDL`. Má tři režimy:

- `Off` (výchozí) - žádné zaznamenávání, žádná režie.
- `Only With Debug Window` - zaznamenává se jen když je otevřené debug okno.
- `Always` - zaznamenává se trvale.

Volby `Export on Exit` a `Set directory...` v témže menu řídí automatický
export dat při ukončení emulátoru a cílový adresář pro export. Všechny
volby jsou persistentní v emulátorové ini konfiguraci (sekce `[DEBUGGER]`,
klíče `cdl_mode`, `cdl_export_on_exit`, `cdl_export_dir`).

Recording je aktivní ve společné pomalé cestě s Trace Log (instrukční
historií), aby hot path emulátoru bez aktivních diagnostik zůstal beze
změny vůči vanilní verzi.

## Adresářová struktura exportu

Export se sestává z jednoho meta JSON souboru a sady binárních souborů,
všechny společně v jednom adresáři. Cesta meta souboru je zvolená
uživatelem (např. `./cdl-export/snap1.json`); basename bez přípony
(`snap1`) se používá jako prefix pro region soubory:

```
<dir>/
    <name>.json                 # meta + tabulka regionů
    <name>_bus.cdl              # binární data per region
    <name>_ram.cdl
    <name>_rom-lower.cdl
    ...
```

- `<name>.json` - metadata exportu (architektura, layout cell, seznam regionů)
- `<name>_<region>.cdl` - jeden binární soubor per fyzický region

Per-region soubory jsou vždy jen referencované z `regions[].file` v JSON,
takže nástroje pro import nepotřebují znát konvenci pojmenování -
parsují JSON a otevírají soubory přesně podle uvedených cest (relativní
k adresáři meta souboru).

Každý `.cdl` soubor je flat pole struktur `st_MHMAP_CELL`:

```c
struct {
    uint32_t r;     // Little-endian
    uint32_t w;     // Little-endian
    uint32_t x;     // Little-endian
    uint32_t s;     // Little-endian
};
```

Velikost cell je 16 bajtů. Endianita zápisu je host byte order; na všech
podporovaných platformách emulátoru (Windows x86_64, Linux x86_64) je to
little-endian. Toto je explicitně deklarováno v `meta.json`
v polích `cell_size_bytes` a `cell_layout`.

Velikost souboru = `size_cells * 16 B`. Konkrétní velikosti jsou
arch-specific - viz tabulky níže.

## meta.json

JSON soubor s následující strukturou:

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

Klíčová pole:

- `format_version` - verze formátu, aktuálně `2`. Inkrementuje se při
  nekompatibilní změně layoutu cell nebo metadat.
- `format` - identifikátor formátu, vždy `"memory-heatmap"`.
- `mzarch` - cílová architektura emulátoru (`800` nebo `1500`).
- `created_at` - ISO 8601 timestamp vytvoření exportu (lokální čas).
  Volitelné pro `format_version >= 2`; pokud chybí, importér
  zobrazí `(no timestamp)`.
- `file_prefix` - basename meta souboru bez `.json`. Slouží jen jako
  human-readable identifikace; konkrétní cesty k souborům jsou v
  `regions[].file`.
- `cell_size_bytes` - velikost jedné cell v bajtech, `16`. Čtenář má
  velikost cell brát z tohoto pole; import v emulátoru jinou hodnotu
  odmítne.
- `cell_layout` - popis bajtového layoutu cell, slouží pro dokumentaci.
- `regions[]` - seznam exportovaných regionů. Každý region má
  symbolický `name`, relativní `file` (vůči adresáři meta souboru),
  `size_cells` a `size_bytes`.

Pořadí položek v `regions[]` je stabilní pro danou architekturu - nástroje
mohou indexovat podle jména (`name`).

### Změny mezi verzemi

| Verze | Změna |
|-------|-------|
| 1 | Původní layout. Region soubory pojmenované přímo `<region>.cdl` (bez prefixu), v adresáři jen samotný `meta.json`. |
| 2 | Přidán `created_at` a `file_prefix`. Region soubory pojmenované `<prefix>_<region>.cdl` v adresáři meta souboru. Import musí číst `regions[].file` (nikoli odvozovat z `name`). Cell má ve všech vydaných verzích 16 B s čítačem S (`cell_size_bytes` 16). |

## MZ-800 (a MZ-700 mód)

Emulátor MZ-800 pokrývá také MZ-700 mód, který se přepíná za běhu DMD
registrem. VRAM recording je rozdělen do dvou skupin:

- **Fyzický pohled** (`vram.cdl`, 32 KB) - co se reálně dělo s fyzickými
  VRAM banky bez ohledu na grafický režim. Recording dle WE bitmasky
  (zápis) resp. RF_PLANE (čtení) - reflektuje co hardware reálně udělal.
- **Mode-specific pohled** - co program dělal v daném grafickém režimu
  z pohledu logických plane. Recording dle WF_PLANE / RF_PLANE.

Programy mohou záměrně přepínat režimy, aby využily různé seskládání
fyzických bank do logických plane. Per-mode recording umožňuje rozlišit
co se dělalo v jakém režimu, fyzický pohled odráží sumu reálných HW operací.

Dohromady **22 regionů**, celkem cca **5 MB** dat (s připojeným Memextem
navíc 8 MB `memext.cdl`):

| Soubor | Cells | Bytes | Popis |
|--------|-------|-------|-------|
| `bus.cdl` | 65536 | 1048576 | Logická CPU adresa 0000h-FFFFh |
| `ram.cdl` | 65536 | 1048576 | Hlavní DRAM 64 KB (i pod ROM) |
| `rom-lower.cdl` | 4096 | 65536 | ROM monitor 0000h-0FFFh |
| `rom-cg.cdl` | 4096 | 65536 | CG-ROM 1000h-1FFFh |
| `rom-upper.cdl` | 8192 | 131072 | ROM monitor E000h-FFFFh |
| `vram.cdl` | 32768 | 524288 | **Fyzická VRAM 32 KB - linear stack 4 bank po 8 KB (VRAM1..4)** |
| `vram700-cg.cdl` | 4096 | 65536 | MZ-700 CG-RAM (bus C000-CFFF) |
| `vram700.cdl` | 4096 | 65536 | MZ-700 text+attr VRAM (bus D000-DFFF) |
| `vram800-320x200_16-I.cdl` | 8192 | 131072 | 320@HICOLOR mode, logická plane I |
| `vram800-320x200_16-II.cdl` | 8192 | 131072 | 320@HICOLOR, plane II |
| `vram800-320x200_16-III.cdl` | 8192 | 131072 | 320@HICOLOR, plane III |
| `vram800-320x200_16-IV.cdl` | 8192 | 131072 | 320@HICOLOR, plane IV |
| `vram800-320x200_4A-I.cdl` | 8192 | 131072 | 320@4 banka A, plane I |
| `vram800-320x200_4A-II.cdl` | 8192 | 131072 | 320@4 banka A, plane II |
| `vram800-320x200_4B-I.cdl` | 8192 | 131072 | 320@4 banka B, plane I |
| `vram800-320x200_4B-II.cdl` | 8192 | 131072 | 320@4 banka B, plane II |
| `vram800-640x200_4-I.cdl` | 16384 | 262144 | 640@4 mode, plane I |
| `vram800-640x200_4-II.cdl` | 16384 | 262144 | 640@4 mode, plane II |
| `vram800-640x200_2A.cdl` | 16384 | 262144 | 640@2 banka A |
| `vram800-640x200_2B.cdl` | 16384 | 262144 | 640@2 banka B |
| `iorq-8bit.cdl` | 256 | 4096 | 8-bit I/O porty 00h-FFh |
| `iorq-gdg.cdl` | 256 | 4096 | 16-bit GDG sub-funkce, placeholder |
| `memext.cdl` | 524288 | 8388608 | Memext RAM, flat 512 KB (jen pokud je Memext připojen) |

### Layout `vram.cdl` (fyzická 32 KB)

Linear stack 4 fyzických bank po 8 KB:

| Offset | Bank | Velikost |
|--------|------|----------|
| 0x0000-0x1FFF | VRAM1 | 8 KB |
| 0x2000-0x3FFF | VRAM2 | 8 KB |
| 0x4000-0x5FFF | VRAM3 | 8 KB |
| 0x6000-0x7FFF | VRAM4 | 8 KB |

Recording do `vram.cdl` proběhne pro **každý** VRAM přístup, bez ohledu
na aktuální grafický režim. Bank-mask přichází z hardware:

- **Zápis**: `WE` bitmask (= výsledek vyhodnocení SINGLE/EXOR/OR/RESET/
  REPLACE/PSET režimů + planar interleave)
- **Čtení**: `RF_PLANE` bitmask (= aktivní plane v RF registru)

### Mapování CPU sběrnice na regiony (MZ-800 mód)

| Sběrnice | Banking podmínka | Recording |
|----------|------------------|-----------|
| 0000h-0FFFh | ROM_0000 mapped | bus + rom-lower[bus_addr] |
| 0000h-0FFFh | ROM_0000 unmapped | bus + ram[bus_addr] |
| 1000h-1FFFh | ROM_1000 mapped | bus + rom-cg[bus_addr-1000h] |
| 1000h-1FFFh | ROM_1000 unmapped | bus + ram[bus_addr] |
| 2000h-7FFFh | (vždy) | bus + ram[bus_addr] |
| 8000h-9FFFh | VRAM_8000 mapped | bus + vram[bank+phy] + mode-specific (320 nebo 640 modus dle DMD) |
| 8000h-9FFFh | VRAM_8000 unmapped | bus + ram[bus_addr] |
| A000h-BFFFh | VRAM_A000 mapped (640 mód) | bus + vram[bank+phy] + 640 mode-specific |
| A000h-BFFFh | VRAM_A000 unmapped | bus + ram[bus_addr] |
| E000h-FFFFh | ROM_E000 mapped + Prohibited (libov. DMD) | (bez fyzického cíle - shadow 0x1A) |
| E000h-FFFFh | ROM_E000 mapped + 700 mode + addr <= E008h | (bez fyzického cíle - mapped ports PIO/CTC/GDG) |
| E000h-FFFFh | ROM_E000 mapped + 800 native + addr <= E00Fh | (bez fyzického cíle - mapped ports area "off", 0xFF) |
| E000h-FFFFh | ROM_E000 mapped, ostatní (NE Prohibited) | bus + rom-upper[bus_addr-E000h] |
| E000h-FFFFh | ROM_E000 unmapped | bus + ram[bus_addr] |

Pozn.: "Prohibited" = stav po OUT 0xE5. Persistuje přes OUT E0/E1/E2/E3
i přes DMD bit 3 switch (700 ↔ 800 native); ruší ho OUT 0xE6, OUT 0xE4 nebo
reset (na MZ-800 změřeno na skutečném stroji). V Prohibited stavu CPU čte
v celém $E000-$FFFF poslední bajt, který byl na datové sběrnici (u datového
čtení poslední bajt instrukce), a zápis se neprovede - žádný fyzický region
(skip resolveru).

### Mapování CPU sběrnice na regiony (MZ-700 mód)

V MZ-700 modu se používá fyzicky jen VRAM1 (Plane I). Recording probíhá
do mode-specific MZ-700 souborů **a** do globální `vram.cdl`:

| Sběrnice | Banking podmínka | Recording |
|----------|------------------|-----------|
| C000h-CFFFh | CGRAM mapped | bus + vram700-cg[bus_addr & FFFh] + vram[VRAM1_base + (bus_addr & FFFh)] |
| C000h-CFFFh | CGRAM unmapped | bus + ram[bus_addr] |
| D000h-DFFFh | VRAM_D000 mapped | bus + vram700[bus_addr & FFFh] + vram[VRAM1_base + 1000h + (bus_addr & FFFh)] |
| D000h-DFFFh | VRAM_D000 unmapped | bus + ram[bus_addr] |
| E000h-FFFFh | ROM_E000 mapped + Prohibited | (bez fyzického cíle - 0x1A/0xFF shadow) |
| E000h-FFFFh | ROM_E000 mapped + 700 mode + addr <= E008h | (bez fyzického cíle - mapped ports PIO/CTC/GDG) |
| E000h-FFFFh | ROM_E000 mapped + 800 native + addr <= E00Fh | (bez fyzického cíle - mapped ports area "off") |
| E000h-FFFFh | ROM_E000 mapped, ostatní (NE Prohibited) | bus + rom-upper[bus_addr - E000h] |
| E000h-FFFFh | ROM_E000 unmapped | bus + ram[bus_addr] |

Pozn.: viz "Prohibited" sekce u MZ-800 mapování výše. Chování samotných portů
$E5 a $E6 je napříč MZ-700 i MZ-800 shodné (E5 aktivuje, E6 ruší); shodné
**není** chování $E4 - detail v téže sekci.

### Mode-specific recording (MZ-800 grafické režimy)

Recording do mode-specific souborů řídí **DMD bity** (HICOLOR, SCRW640)
a **VBANK** (bit 4 v WF/RF registrech). Pravidla mapování plane bitmasky
na soubory:

| Mode | DMD bity | VBANK | Plane mapping |
|------|----------|-------|---------------|
| 320x200@16 (hicolor) | HICOLOR=1, SCRW640=0 | - | PLANE1→I, PLANE2→II, PLANE3→III, PLANE4→IV |
| 320x200@4 banka A | HICOLOR=0, SCRW640=0 | A=0 | PLANE1→I, PLANE2→II |
| 320x200@4 banka B | HICOLOR=0, SCRW640=0 | B=1 | PLANE3→I, PLANE4→II |
| 640x200@4 | HICOLOR=1, SCRW640=1 | - | PLANE1→I, PLANE3→II |
| 640x200@2 banka A | HICOLOR=0, SCRW640=1 | A=0 | PLANE1→ jediná plane |
| 640x200@2 banka B | HICOLOR=0, SCRW640=1 | B=1 | PLANE3→ jediná plane |

**Indexace mode-specific souborů**: sběrnicový offset (= `bus_addr & 0x3fff`),
tedy 0-0x1FFF (8 KB) pro 320 modes, 0-0x3FFF (16 KB) pro 640 modes.

**Plane bitmask** přichází z:
- WF_PLANE pro zápis (= software intent, kam program chtěl psát)
- RF_PLANE pro čtení (= aktivní plane)

Mode-specific recording se aktivuje **jen v daném modu** - když program přepne
z 320@16 na 320@4A, další zápisy už jdou do 4A souborů, ne do 16 souborů.
To umožňuje rozlišit co program dělal v jednotlivých režimech.

### Mode-specific vs fyzický pohled

Per-mode soubory zachycují **logické plane v daném modu**, indexované
sběrnicovým offsetem. `vram.cdl` zachycuje **fyzické banky** (VRAM1..4)
indexované plane offsetem (= addr po >>1 v 640 modu a hwscroll).

Příklad zápisu na bus 0x8000 v módu 640x200@4 do obou logických plane:

| Soubor | Offset | Důvod |
|--------|--------|-------|
| `bus.cdl` | 0x8000 | logický access |
| `vram.cdl` | bank+phy (4 možné dle WE) | fyzické HW banky |
| `vram800-640x200_4-I.cdl` | 0x0000 | logická plane I |
| `vram800-640x200_4-II.cdl` | 0x0000 | logická plane II |

### IORQ regiony

`iorq-8bit.cdl` zaznamenává 8-bit I/O porty (`port & 0xFF`). Logují se
jen R a W counters, X a S u portů nedávají smysl (Z80 nečte port jako
instrukci).

`iorq-gdg.cdl` je rezervován pro 16-bit GDG sub-funkce přes high byte
B registru (jediné zařízení v MZ-800 s 16-bit IORQ adresováním). Aktuálně
je placeholder se 256 cell - přesný layout se upřesní později.

### Memext region

`memext.cdl` (512 KB, dostupný pro MZ-800 i MZ-1500) zaznamenává přístup
do RAM Memext periferie. Recording probíhá **navíc** k logu do `ram.cdl`,
když RAM access prošel přes namapovanou Memext bank.

Indexování: flat 512 KB. Bus adresa se převede na offset v Memext RAM
přes aktuální banking mapu, plus `addr & 0x0FFF` v rámci 4 KB bank window.

Recording proběhne jen pokud je Memext skutečně připojen
(`MEMEXT_TEST_CONNECTED`) a access skončil v Memext RAM rozsahu (= ne
v WOM nebo mimo). Jinak je region celý nulový - takový export lze
ignorovat.

Region rozlišení **typu** (PEHU vs Luftner) se neuvádí - oba se logují
do téže `memext.cdl`. Pro disassembler/CDL flag analyzér to nevadí
(typ ovlivňuje jen mapping, ne layout RAM).

## MZ-1500

Layout je odlišný od MZ-800 - MZ-1500 nemá pixelově orientovanou VRAM,
nemá WF/RF registry, nemá GDG 16-bit IORQ. Místo toho má 3 PCG banky
(programovatelný character generator) a CGROM viděnou na sběrnici skrz
SPEC bity v banking mapě.

Dohromady **9 regionů**, celkem cca **2,9 MB** dat (s připojeným Memextem
navíc 8 MB `memext.cdl`):

| Soubor | Region | Cells | Bytes | Popis |
|--------|--------|-------|-------|-------|
| `bus.cdl` | bus | 65536 | 1048576 | Logická CPU adresa 0000h-FFFFh |
| `ram.cdl` | ram | 65536 | 1048576 | Hlavní DRAM 64 KB |
| `rom.cdl` | rom | 16384 | 262144 | Monitor ROM, 16 KB (lower 0000h + upper E010h-FFFFh v jednom blob) |
| `cgrom.cdl` | cgrom | 4096 | 65536 | CG-ROM 4 KB (mapovaná na D000h-DFFFh nebo E000h-EFFFh přes SPEC=1) |
| `vram.cdl` | vram | 4096 | 65536 | Znaková + atributová VRAM, 4 KB |
| `pcg-1.cdl` | pcg-1 | 8192 | 131072 | PCG bank 1, 8 KB - v hardware nese R složku barvy spritu |
| `pcg-2.cdl` | pcg-2 | 8192 | 131072 | PCG bank 2, 8 KB - G složka |
| `pcg-3.cdl` | pcg-3 | 8192 | 131072 | PCG bank 3, 8 KB - B složka |
| `iorq-8bit.cdl` | iorq-8bit | 256 | 4096 | 8-bit I/O porty 00h-FFh |
| `memext.cdl` | memext | 524288 | 8388608 | Memext RAM, flat 512 KB (jen pokud je Memext připojen) |

### Mapování CPU sběrnice na regiony

| Sběrnice | Banking | SPEC | Region | Offset |
|----------|---------|------|--------|--------|
| 0000h-0FFFh | ROM_0000 mapped | - | rom | bus_addr (= 0000h-0FFFh v ROM blob) |
| 0000h-0FFFh | ROM_0000 unmapped | - | ram | bus_addr |
| 1000h-CFFFh | (vždy) | - | ram | bus_addr |
| D000h-DFFFh | ROM_UPPER unmapped | - | ram | bus_addr |
| D000h-DFFFh | ROM_UPPER mapped | SPEC=0 (default) | vram | bus_addr & 0FFFh |
| D000h-DFFFh | ROM_UPPER mapped | SPEC=1 | cgrom | bus_addr & 0FFFh |
| D000h-DFFFh | ROM_UPPER mapped | SPEC=2..4 | pcg-1 / pcg-2 / pcg-3 | (bus_addr & 3FFFh) - 1000h = 0000h-0FFFh |
| E000h-EFFFh | ROM_UPPER unmapped | - | ram | bus_addr |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=0, addr <= E00Fh | (bez fyzického cíle - jen bus mapa) | memory-mapped porty |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=0, addr > E00Fh | rom | bus_addr & 3FFFh (= 2010h-2FFFh v ROM) |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=1 | cgrom | bus_addr & 0FFFh |
| E000h-EFFFh | ROM_UPPER mapped | SPEC=2..4 | pcg-1..3 | (bus_addr & 3FFFh) - 1000h = 1000h-1FFFh |
| F000h-FFFFh | ROM_UPPER unmapped | - | ram | bus_addr |
| F000h-FFFFh | ROM_UPPER mapped | SPEC=0 | rom | bus_addr & 3FFFh (= 3000h-3FFFh v ROM) |
| F000h-FFFFh | ROM_UPPER mapped | SPEC<>0 | (vrací 0xFF, bez fyzického cíle) | - |

### PCG banky

PCG bank má fyzicky 8 KB. Z CPU sběrnice se vidí 4 KB okno aktivní bank
podle SPEC stavu:

- bus 0xD000-0xDFFF (4 KB) -> PCG offset 0x0000-0x0FFF
- bus 0xE000-0xEFFF (4 KB) -> PCG offset 0x1000-0x1FFF

Aktivní PCG bank (1, 2 nebo 3) se vybírá hodnotou SPEC v banking mapě
(SPEC=2 pro PCG_1, SPEC=3 pro PCG_2, SPEC=4 pro PCG_3).

Pro grafiku spritu MZ-1500 obsazují PCG[1..3] tři barevné složky R, G, B
- proti pixelu se spojí na 8 barev (paleta jako MZ-700 textový mód).

## Použití

### Z UI

V debug okně menu `Debugger Settings -> CDL`:

1. Vybrat režim recording (Off / Only With Debug Window / Always).
2. Volitelně zapnout `Export on Exit` a nastavit cílový adresář
   přes `Set directory...`.
3. Spustit emulovaný program. Counters se aktualizují za běhu.
4. Při ukončení emulátoru se automaticky vyexportuje CDL adresář
   (pokud byl `Export on Exit` zapnut).

### Načtení dat v Pythonu (příklad)

Argumentem je cesta k meta JSON souboru (např. `./cdl-export/snap1.json`).
Region soubory se hledají v jeho parent adresáři podle `regions[].file`:

```python
import json, struct
from pathlib import Path

def load_cdl(meta_path):
    meta_path = Path(meta_path)
    meta = json.loads(meta_path.read_text())
    base_dir = meta_path.parent
    k = meta["cell_size_bytes"] // 4          # čítačů uint32 na cell (4)
    cells = {}
    for region in meta["regions"]:
        data = (base_dir / region["file"]).read_bytes()
        n = region["size_cells"]
        counters = struct.unpack(f"<{k*n}I", data)
        # (r, w, x, s) pro každou cell
        cells[region["name"]] = [tuple(counters[k*i:k*i+k]) for i in range(n)]
    return meta, cells

meta, cells = load_cdl("./cdl-export/snap1.json")
# Příklad: bajty v RAM, které byly někdy spuštěny jako kód
code_bytes = [i for i, c in enumerate(cells["ram"]) if c[2] > 0]
```
