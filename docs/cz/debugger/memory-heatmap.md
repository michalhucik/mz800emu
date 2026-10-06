# Memory Heatmap - uživatelská příručka

Memory Heatmap (interní jméno modulu: **CDL** = Code/Data Logger) je nástroj
debuggeru, který během běhu emulace agreguje statistiku přístupů do každé
paměťové buňky (a každého I/O portu) a vizualizuje ji jako grid s barevnou
kódovací škálou. Slouží k pochopení, kde program kdy čte data, zapisuje
proměnné, vykonává kód, případně k oddělení kódu od dat (klasické CDL pro
disassembler workflow).

Toto okno je samostatné, dokovatelné a může být otevřené souběžně s běžícím
emulátorem i s hlavním debuggerem.

> Architektury: MZ-800 a MZ-1500. Pro MZ-700 je v emulátoru vždy MZ-800
> v MZ-700 modu, takže tam stejně jede MZ-800 verze.

## Otevření okna

- **Hlavní menu emulátoru** -> `Debugger` -> `Memory Heatmap`
- **Debug okno menu** -> `Debugger Settings` -> `CDL` -> `Show heatmap window`

Obě cesty otevírají totéž okno. Stav viditelnosti je per-session (default
zavřeno), pozici/velikost/dock state si pamatuje ImGui automaticky přes
`imgui.ini`.

## Tři režimy nahrávání (Mode)

CDL nahrávání je drahé (per-access counter inkrement) a vypnuté v hot path
emulátoru by bylo nesmyslné. Tři režimy:

- **Off** (default) - vůbec se nenahrává, žádná režie. Hot path je
  bit-identický s vanilla emulátorem (callback swap).
- **With Window** - nahrává se pouze pokud je otevřené hlavní debug okno.
  Užitečné pro krátké analyzační průchody.
- **Always** - nahrává se trvale, nezávisle na stavu debug okna. Užitečné
  pro automatické testovací běhy s `--cdl-save-on-exit`.

Změna režimu triggeruje swap CPU callbacků. Aktuální stav vidíte v Status
řádku pod top barem ("Recording active" zeleně / "Recording inactive"
šedě).

## Tři typy přístupů (R / W / X)

Každá buňka drží trojici 32-bit counterů:

| Symbol | Význam |
|--------|--------|
| **R** | data read - běžné LOAD instrukce mimo M1 fetch |
| **W** | write - libovolný zápis (LD, PUSH, OUT) |
| **X** | execute - bajt načtený jako součást instrukce (M1 nebo její operandy) |

Klasické CDL flagy ve stylu FCEUX se z toho dají odvodit: flag = (counter > 0).

## Layout okna

```
+-----------------------------------------------------+
| [Mode] [x] Export on Exit  [Reset] [Export] [Import]|
| Status: ...                                          |
+-----------------------------------------------------+
| [bus | ram | rom-lower | ... | iorq-8bit | iorq-gdg]|
+----------------------------------------+------------+
| <region stats>                         | <selected> |
| Color/Scale/Threshold/Zoom filter      |            |
+----------------------------------------+ <stats>    |
|                                        |            |
|       H E A T M A P    G R I D         | <reset>    |
|                                        |            |
+----------------------------------------+------------+
```

### Top control bar

| Prvek | Účinek |
|-------|--------|
| **Mode** (radio) | přepíná Off / With Window / Always |
| **Export on Exit** (checkbox) | při ukončení emulátoru exportovat CDL |
| **Reset** | vynuluje counters ve všech regionech (`mhmap_reset`) |
| **Export...** | otevře file dialog, do vybraného adresáře zapíše `meta.json` + per-region `.cdl` soubory |
| **Import...** | otevře file dialog, načte CDL adresář pro vizualizaci (read-only, neovlivní běžící recording) |
| **Hide/Show panel** | toggle pravého side panelu |

### Region tab bar

Záložky odpovídají paměťovým regionům dostupným pro aktuální architekturu:

#### MZ-800 (22 regionů)

- `bus` - logická CPU adresa 0000-FFFF (64 KB)
- `ram` - hlavní DRAM 64 KB
- `rom-lower` - monitor ROM 0000-0FFF (4 KB)
- `rom-cg` - CG-ROM 1000-1FFF (4 KB)
- `rom-upper` - monitor ROM E000-FFFF (8 KB)
- `vram` - fyzická VRAM 32 KB (4 banky x 8 KB stack VRAM1..4)
- `vram700-cg` - MZ-700 mód CG-RAM C000-CFFF (4 KB)
- `vram700` - MZ-700 mód text+attr D000-DFFF (4 KB)
- `vram800-320x200_16-{I,II,III,IV}` - 320x200x16 hicolor, 4 plane (8 KB každá)
- `vram800-320x200_4{A,B}-{I,II}` - 320x200x4, banka A/B, 2 plane (8 KB každá)
- `vram800-640x200_4-{I,II}` - 640x200x4, 2 plane (16 KB každá)
- `vram800-640x200_2{A,B}` - 640x200x2, banka A/B (16 KB každá)
- `iorq-8bit` - 8-bit I/O porty 00-FF (256 cell)
- `iorq-gdg` - 16-bit GDG sub-funkce (256 cell, placeholder)

VRAM má dva pohledy: **fyzický** (`vram`) odráží stav 4 hardware bank
podle WE bitmask během zápisu, **logický mode-specific** (`vram800-...`)
sleduje, do které logické plane v aktivním grafickém modu se zapsalo
(řízeno WF/RF registry GDG). Stejný zápis tedy obvykle inkrementuje cell
v obou pohledech.

#### MZ-1500 (9 regionů)

- `bus`, `ram`, `rom`, `cgrom`, `vram`, `pcg-1`, `pcg-2`, `pcg-3`,
  `iorq-8bit`

### Heatmap grid

Grid se velikostí přizpůsobuje regionu:

| Velikost regionu | Layout |
|------------------|--------|
| 64 KB (bus, ram) | 256 x 256 |
| 32 KB (vram MZ-800) | 256 x 128 |
| 16 KB (640x200 plane, MZ-1500 rom) | 256 x 64 |
| 8 KB (320x200 plane, vram-plane, pcg, rom-upper) | 128 x 64 |
| 4 KB (vram700, rom-cg/lower, cgrom, vram MZ-1500) | 64 x 64 |
| 256 cell (iorq) | 16 x 16 |

Velikost jednoho cell na obrazovce řídí Zoom (1x = 1 px per cell, 8x = 8 px).
U velkých regionů se okno horizontálně/vertikálně scrolluje.

#### Barevné kódování (Color)

- **RGB** (default) - tříkanálové: R counter -> red, W -> green, X -> blue.
  Kombinace barev říká poměr access types (např. žlutý = R+W, fialový = R+X).
- **R / W / X** - jediný counter v grayscale (čím vyšší counter, tím
  světlejší šedá).

Každý kanál se normalizuje proti svému max v regionu (nezávisle), takže
slabý W counter může být stejně vidět jako silný R counter.

#### Měřítko (Scale)

- **Linear** - `intensity = count / max_in_region`
- **Log** - `intensity = log(count+1) / log(max+1)` (default).
  Logaritmické měřítko pomůže vidět detail v oblastech s velkým rozptylem
  (málo přístupných cell oproti často přístupným).

#### Threshold

Cell s `max(R, W, X) < threshold` se nezobrazí (zůstane černá). Užitečné
pro filtrování šumu (např. threshold=5 schová cell s méně než 5 přístupy
za celý běh).

#### Hover a klik

- **Hover nad cell** - tooltip ukáže region, offset (hex+dec), R/W/X
- **Levý klik** - vybere cell (žlutý rámeček), detail se objeví v side panelu
- **Pravý klik** - kontextové menu (zatím prázdné placeholder pro budoucí
  "Open in memory browser")

### Side panel (pravá strana)

#### Show: Live / Imported

- **Live** (default) - zobrazí aktuální data z běžícího recordingu
- **Imported** (disabled dokud Import nebyl proveden) - zobrazí načtený
  snapshot z disku

Toggle nemodifikuje recording - jen mění to, co se vykresluje.

#### Selected cell

Detail vybrané cell:
- **Addr** - bus adresa (jen u regionu `bus`; pro fyzické regiony není
  dobře definovaná, zobrazí se "n/a")
- **Region**, **Offset** (hex + dec)
- **R / W / X** - counter hodnoty + procento z region max

#### Region statistics

- **Active cells** - kolik cell má `max(R,W,X) > 0` z celkového počtu
- **Total R/W/X** - sumace counterů přes celý region
- **Max R/W/X** - největší jednotlivý counter v regionu

#### Reset region only

Vynuluje counters jen v aktivním regionu (rychlejší než globální Reset).
V Imported módu disabled (importovaná data jsou snapshot, neměníme je).

## Export

Tlačítko `Export...` v top baru otevře file dialog. Po výběru cílového
adresáře se vytvoří:

```
<dir>/
    meta.json                       # format_version, mzarch, region list
    bus.cdl                         # raw 12-byte cells (R, W, X uint32 LE)
    ram.cdl
    rom-lower.cdl
    ...
    iorq-8bit.cdl
    iorq-gdg.cdl
```

Per-region soubor je raw binary: `size_cells * 12 bytes`, kde 12 bytes per
cell = `r:uint32 LE, w:uint32 LE, x:uint32 LE`.

Pokud adresář existuje, soubory se přepisují bez warningu. Pokud
neexistuje, vytvoří se (`g_mkdir_with_parents`).

`meta.json` výstup vypadá takhle:

```json
{
  "format_version": 2,
  "format": "memory-heatmap",
  "mzarch": 800,
  "cell_size_bytes": 12,
  "cell_layout": "r:uint32_le, w:uint32_le, x:uint32_le",
  "regions": [
    { "name": "bus", "file": "bus.cdl", "size_cells": 65536, "size_bytes": 786432 },
    ...
  ]
}
```

### Export on Exit

Pokud je checkbox aktivní, při ukončení emulátoru (graceful, přes
window-close) se automaticky zavolá export do nastaveného adresáře.
Cílový adresář je ten naposledy vybraný v dialogu (default `./cdl-export/`).

## Import

Tlačítko `Import...` otevře file dialog. Vyberete adresář s dříve
vyexportovaným CDL exportem.

Načítání:
1. Validuje `meta.json` - musí obsahovat `"format_version": 2` a
   `"mzarch": <aktuální arch>`
2. Pro každý region v tabulce načte soubor (např. `bus.cdl`) do paměti
3. Pokud chybí jednotlivé region soubory, jsou tolerovány (zůstanou
   vynulované) a zobrazí se warning v Import status řádku
4. Po úspěchu se Show toggle automaticky přepne na **Imported**

Import alokuje paralelní buffer (~13,5 MB pro MZ-800, ~11 MB pro MZ-1500
a MZ-700).
Buffer žije, dokud se okno nezavře nebo emulátor neukončí.

Důležité: Import je **read-only vizualizace**. Live recording běží dál,
nedochází k mergování s importovanými daty. Pro porovnání live vs
imported přepínejte Show toggle.

## CLI options

CDL lze ovládat z příkazové řádky pro automatizované běhy:

```
mz800emu --cdl-mode <off|window|always>     # spustit s daným režimem
         --cdl-dir <dirpath>                # cílový adresář exportu
         --cdl-save-on-exit <on|off>        # exportovat při ukončení
```

Příklad: spustit MZF program a po dokončení (zavření okna) zapsat CDL:

```
mz800emu --run-mzf game.mzf \
         --cdl-mode always \
         --cdl-dir ./game-cdl/ \
         --cdl-save-on-exit on
```

CLI override má přednost před hodnotami v `mz800emu.ini` (aplikuje se
po `cfgmodule_propagate`).

## Persistence (mz800emu.ini)

Sekce `[DEBUGGER]` v `mz800emu.ini`:

| Klíč | Typ | Default | Popis |
|------|-----|---------|-------|
| `cdl_mode` | KEYWORD | `OFF` | OFF / WITH_WINDOW / ALWAYS |
| `cdl_export_on_exit` | BOOL | 0 | Auto-export při ukončení |
| `cdl_export_dir` | TEXT | `./cdl-export/` | Cílový adresář exportu |
| `mhwindow_color_mode` | KEYWORD | `RGB` | RGB / R / W / X |
| `mhwindow_log_scale` | BOOL | 1 | Log normalizace |
| `mhwindow_threshold` | UNSIGNED | 0 | Hide cells pod prahem |
| `mhwindow_zoom` | UNSIGNED | 2 | 1, 2, 4, 8 |
| `mhwindow_side_panel_visible` | BOOL | 1 | Toggle side panelu |
| `mhwindow_selected_region_idx` | UNSIGNED | 0 | Aktivní region tab idx |

Viditelnost samotného okna se **nepersistuje** - po startu je vždy
zavřené a uživatel ho otevře z menu. Pozice / velikost / dock state
spravuje ImGui automaticky přes `imgui.ini` (vlastní soubor).

## Tipy a typické workflow

### Disassembly: oddělit kód od dat

1. `Mode = Always`, `Color = RGB`, `Scale = Log`
2. Spustit program, hrát ho dlouho, navštívit všechny obrazovky / menu
3. `Export...` -> CDL adresář
4. Externí disassembler nakreslí instrukce jen tam, kde X > 0; ostatní
   bajty jsou data

### Najít často čtená data (= constants, tabulky)

1. `Color = R only`, `Scale = Log`, `Threshold = 100`
2. Filtrace skryje cells s méně než 100 R; zbylé jsou hot data

### Najít self-modifikující kód

1. `Color = RGB`
2. Hledat cells, které mají zelenou (W > 0) **a zároveň** modrou (X > 0).
   Cyan / bílé cells = bajt který byl zapsán a později vykonán = self-mod

### Sledovat write pattern do VRAM

1. Vybrat tab `vram` (fyzický 32 KB)
2. `Color = W only`
3. Vidíte kam program zapisuje pixely. Pro detail per-mode přepněte na
   `vram800-320x200_16-I` atd.

## Performance

Okno renderuje grid každý frame - pro 64K cell @ 4x zoom je to 1024x1024 px,
desítky tisíc `AddRectFilled` volání per frame. Na běžném HW funguje, ale
pokud je UI laggy:

- Snižte zoom (1x je nejrychlejší)
- Aplikujte threshold (skrytá cells = bez render volání)
- Zavřete okno když ho nepoužíváte (s Mode=With Window vypne i recording)

## Související dokumenty

- [cdl_format.md](formats/cdl_format.md) - detailní popis CDL formátu
