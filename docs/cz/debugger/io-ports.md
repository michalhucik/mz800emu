# I/O Ports panel

I/O Ports panel je strukturovaný pohled na všechny dokumentované I/O
porty Sharp MZ-800 / MZ-700 / MZ-1500 inspirovaný no$gba IO Map.
Zobrazí bit-by-bit popis registrů, live hodnoty, activity tracking
("heat map") a IORQ history.

## Obsah

- [Účel panelu](#účel-panelu)
- [Architektura](#architektura)
- [Naming konvence](#naming-konvence)
- [Zdroje dat](#zdroje-dat)
- [0xCF 16-bit CRTC family](#0xcf-16-bit-crtc-family)
- [Banking decoded view](#banking-decoded-view)
- [Cross-window navigation](#cross-window-navigation)
- [Activity tracking](#activity-tracking)
- [History ring buffer](#history-ring-buffer)
- [History tab UI](#history-tab-ui)
- [Filter syntax](#filter-syntax)
- [Memory-mapped I/O 0xE000-0xE008](#memory-mapped-io-0xe000-0xe008)
- [Persistence (cfg sekce)](#persistence-cfg-sekce)
- [Use cases](#use-cases)
- [Troubleshooting](#troubleshooting)
- [Související panely](#související-panely)

## Účel panelu

Panel slouží debuggeru k:

1. **Real-time pohled** na stav HW periferií (GDG, 8255 PPI, 8253 CTC,
   Z80 PIO, FDC, banking, PSG, joystick) bez nutnosti procházet emulátor
   internal state v hex vieweru.
2. **Bit-by-bit dekódování** každého registru - žádné raw byte hodnoty
   bez kontextu.
3. **Heat map** aktivity (= které porty CPU akce právě "dráždí") přes
   sliding window 1 s.
4. **History** - chronologický seznam IN/OUT/MR/MW eventů s filtrem
   podle PC, portu, frame, cyklu, hodnoty, adresy.
5. **Quick Add BP** - pravým klikem na port se vygeneruje smart
   breakpoint:
   - IORQ entry -> `IORQ_R` / `IORQ_W` (pre-fill `port` field)
   - MMIO entry (0xE000-0xE008) -> `MEM_R` / `MEM_W` (pre-fill `addr`
     field)

## Architektura

Tab system uvnitř panelu (Overview default, History tab pro chronologii):

```
+-- I/O Ports ---------------------------------------------+
| [Overview] [History]                                     |
+----------------------------------------------------------+
| Sticky header:                                           |
|   Filter:[___] [Clear]   (visible/total)                 |
|   [v] Track | [Reset Activity] | Capacity:[10000v]       |
|   [v] Auto-follow | [Latest] | [Clear history]           |
+----------------------------------------------------------+
| Section: GDG                                  [v]        |
|   Addr   | Name             | Hex | Bin    | R/W | Rec | Activ |
|   0xCC   | GDG - WF (W)     | ??  | --     | W   |  v  |   0/s |
|   0xCD   | GDG - RF (W)     | ??  | --     | W   |  v  |   0/s |
|   0xCE   | GDG - DMD (W)    | 0E  | 000... | W   |  v  |   2/s |
|   0xCE   | GDG - Status (R) | 80  | 100... | R   |  v  |   1/s |
|   ...                                                    |
| Section: 8255 PPI                              [v]       |
|   ...                                                    |
+----------------------------------------------------------+
```

Sticky header podle Variables panel vzoru: žádné `(?)` markery, tooltipy
přes hover. Tlačítka jsou inline (méně kliků oproti modal dialogu).

Hex sloupec zobrazí aktuální hodnotu portu. Pokud port nemá readable
mirror a ještě nezachytil žádný IORQ event, zobrazí `??` s tooltipem
"No data captured". Pokud hodnota pochází z cache a je starší než
500 frames (~10 s při 50 Hz), je zobrazena dimmed (TextDisabled)
s tooltipem "Last cached value N frames ago".

**Rec sloupec:** Per-port checkbox v Overview slouží jako selective
recording mask - odškrtnutí potlačí zápis IORQ na ten port do History
ring bufferu. Mask má rozsah 256 (low byte IORQ). Default je vše
recordováno. Použití: zaznamenat jen události, které sledujeme (jeden
konkrétní port, zbytek vypnutý) bez záplavy high-frequency portů
(klávesnice, status polling).

## Naming konvence

Pro každý port v katalogu platí formát:

```
<chip> - <function> (<dir>)
```

kde `<dir>` patří do {`R`, `W`, `R/W`}.

**Příklady:**

| Kategorie | Příklad name |
|-----------|--------------|
| GDG | `GDG - WF (W)`, `GDG - DMD (W)`, `GDG - Status (R)` |
| 8255 PPI | `8255 PPI - Port A (R/W)`, `8255 PPI - Control (W)` |
| 8253 CTC | `8253 CTC - Counter 0 (R/W)`, `8253 CTC - Control (W)` |
| Z80 PIO | `Z80 PIO - Port A data (R/W)` |
| FDC | `FDC - Status / Command (R/W)` |
| PSG (MZ-800) | `PSG - SN76489 (W)`, `PSG - Stereo left (W)` |
| PSG stereo (MZ-1500) | `PSG stereo - L Latch (W)`, `PSG stereo - R Latch (W)` (samostatná sekce "PSG (stereo SN76489 x2)" v Overview) |
| Memory banking | `Memory bank - E0 (R/W)` .. `Memory bank - E6 (R/W)` (Sharp banking přes GDG dispatch) |
| Memory expansion | `Memory ext - MEMEXT bank (R/W)` (port 0xE7, separátní MemExt karta pod Sharp bankingem) |
| Joystick | `JOY0 (R)`, `JOY1 (R)` |
| MZ-700 mmio | `MZ-700 mmio - PPI Port A (R/W)`, `MZ-700 mmio - GDG DMD/Status (R/W)` |
| CMT hack | `CMT hack - Load file (W)`, `CMT hack - Read MZF body (W)` |
| Unicard | `Unicard - CMD (R/W)`, `Unicard - DATA (R/W)` |
| Ramdisk | `Ramdisk - Pezik 68 bank 0 (R/W)`, `Ramdisk - Std read (R)` |
| IDE8 | `IDE8 - Data (R/W)`, `IDE8 - Status / Command (R/W)` |
| QDISK | `QDISK - SIO Data A (R/W)`, `QDISK - SIO Ctrl B (R/W)` |

**Multi-entry per adresa:** Pokud IORQ IN a IORQ OUT na téže adrese mají
různý význam, jsou v katalogu dva oddělené entries:

| Addr | OUT entry | IN entry |
|------|-----------|----------|
| 0xCE | `GDG - DMD (W)` | `GDG - Status (R)` |
| 0xF0 | `GDG - Palette (W)` | `JOY0 (R)` |

UI je zobrazí jako dva řádky se samostatnými bit popisy a direction tagy.

## Zdroje dat

Panel kombinuje tři informační zdroje:

1. **Statický katalog** (compile-time):
   - Adresa, name, direction, bit-by-bit popisy, decode helpers,
     architektura (MZ-700 / MZ-800 / MZ-1500 / ALL).

2. **Live emulator state** (runtime):
   - Pro každý port se hodnota čte přímo z interních struktur emulátoru
     **side-effect free**, nikdy přes IORQ dispatch (= side-effect na
     chip latch).
   - Pro write-only porty (WF/RF) emulátor drží mirror v interní
     struktuře - z té čteme.
   - Pokud port nemá readable mirror (skutečné write-only HW bez
     interního storage), UI zkusí poslední cached IORQ value (per směr
     R/W) zachycenou v aktivitě. Pokud ani cache nemá data, zobrazí `??`
     s tooltipem.
   - **Last-write cache pattern** (write-only sequencer registry): pro
     Control Word PPI/CTC/PIO není možný non-destructive read z chipu,
     ale debugger drží cache zachycující poslední CPU write. Hex sloupec
     v Overview tak ukazuje aktuální obsah Control Word i pro tyto
     registry.

3. **Dynamic activity / history** (runtime, gated):
   - Sliding window hits/s per port.
   - Ring buffer N IORQ + MMIO eventů.
   - Hot-path hook gated přes interní flag - panel zavřený = zero
     overhead.

## 0xCF 16-bit CRTC family

GDG má skupinu **16-bit IORQ portů** s sběrnicovou adresou `0xRRCF`:

```asm
; nastavení BCOL na černou
XOR  A
LD   BC, 06CFh    ; B=06h (register index), C=0xCF
OUT  (C), A
```

Z80 OUT (C),A pattern: B = register index, C = port = 0xCF.
**MZ konvence notace:** `0xCF<RR>` (= human-readable).

Sub-registry (všechny W-only):

| MZ notace | B reg | Význam |
|-----------|-------|--------|
| 0xCF01 | 01 | SOF0 (scroll offset low 8 bits) |
| 0xCF02 | 02 | SOF1 (scroll offset upper 2 bits, bity 0-1) |
| 0xCF03 | 03 | SW (scroll width) |
| 0xCF04 | 04 | SSA (scroll start address) |
| 0xCF05 | 05 | SEA (scroll end address) |
| 0xCF06 | 06 | BCOL (border color, 4 bity) |
| 0xCF07 | 07 | CKSW (Superimpose enable, bit 7) |

**UI zobrazení:**
- Sloupec "Addr" zobrazí MZ notaci `0xCF01`..`0xCF07`.
- Tooltip popisuje sběrnicovou adresu `0xRRCF`.
- Read mirror je k dispozici pro SOF0/SOF1, SW, SSA, SEA a BCOL. Pro
  CKSW mirror neexistuje; aktuální stav signálu CKSW lze číst v 0xCE
  Status registru, bit 2.

**IORQ_W BP integrace:** Při akci "Add IORQ W BP" z 0xCF<RR> entry UI
automaticky nastaví `port_mode = BP_PORT_16BIT`. Default 8-bit mód by
matchnul všech 7 0xCF<RR> portů (= jen low byte 0xCF).

**MZ-1500 nepodporuje** 0xCF family ani HW scroll (= dostupné jen pro
MZ-800).

## Banking decoded view

Banking porty 0xE0-0xE6 v Overview tabu zobrazují dekódovaný Sharp
banking state. Skutečný banking state není v IORQ portu (= write-only
dispatch do GDG), ale v bajtu mapy paměti (per-arch bitfield). MZ-800
navíc závisí na GDG DMD bit 3 (700 compat vs 800 native mode).

Všech 7 entries 0xE0-0xE6 sdílí stejnou hex hodnotu (= global state,
ne per-port; podobně jako WD279x, kde 4 různé porty ukazují různé
registry téhož chipu), plus per-arch popisky flagů.

**Per-arch bit popisky:**

| Arch | Flag dekódy |
|------|-------------|
| MZ-800 | 5 flagů: `ROM_0000`, `ROM_1000`, `CGRAM_VRAM`, `ROM_E000`, `PROHIBITED`. Bity 2-3 mají význam závislý na DMD bit 3 - popisek pokrývá oba módy (700/800). |
| MZ-700 | 3 flagy: `ROM_0000`, `ROM_E000`, `PROHIBITED` |
| MZ-1500 | 2 flagy + 3-bit `SPEC` pole: `ROM_0000`, `ROM_UPPER`, `SPEC` (0=None, 1=CGROM, 2=PCG1, 3=PCG2, 4=PCG3 v D000-EFFF). UI hint přeloží `SPEC` na symbolický název. |

**Naming split:** Sekce v Overview je rozdělená na **"Memory banking"**
(porty 0xE0-0xE6 = Sharp banking via GDG dispatch) a **"Memory expansion
(MemExt)"** (port 0xE7 = paměťová extenze 64 kB -> 512 kB SRAM, oddělená
HW karta pod Sharp bankingem).

## Cross-window navigation

Banking entries 0xE0-0xE6 + MemExt 0xE7 mají v Overview row right-click
menu položku **"Show in Memory Map"**, která otevře a zafokusuje Memory
Map debug okno.

Důvod: MemExt 0xE7 per-port mirror v Overview nedává smysl (16-byte
multi-page state); Memory Map okno už plně zobrazuje MemExt sloupec
per 4 kB stránku, takže cross-window klik je správné UX řešení i pro
Sharp banking porty.

Viz [memory-map.md](memory-map.md) pro detaily Memory Map okna.

## Activity tracking

Per port (full 16-bit IORQ adresa) sliding window:

- **Bucket array:** 60 bucketů, 1 bucket = 1 emulační frame (~20 ms při 50 Hz).
- **Window size:** posledních 50 bucketů ~ 1 s.
- **Advance:** posun bucketů probíhá na konci každého frame.
- **Reset:** "Reset Activity" v sticky header (všechny porty) nebo
  per-port přes right-click context menu.

UI sloupec "Activity" zobrazí `<N> hits/s` plus barvení řádku per heat
map prahy. Dva prahy jsou configurable přes cfg klíče v sekci
`[IO_PORTS_PANEL]`:

| Cfg klíč | Default | Smysl |
|----------|---------|-------|
| `heat_text_active` | 1 | hits/s práh pro zelený text aktivního řádku (= "port není mrtvý") |
| `heat_bg_hot` | 10000 | hits/s práh pro červený bg tint bottleneck řádku (= "hot port, podívat se") |

UI: inline **Heat...** popup v toolbaru (2x `InputInt` + Reset to defaults),
clamp do rozsahu povolených cfg hodnot.

**Counters per směr:** Activity je rozdělená na IN counter a OUT counter.
Pro port jako 0xF0 (W = GDG Palette, R = JOY0) ukazuje Overview aktivitu
zvlášť na W řádku a R řádku - tak že zápis na Palette nezvyšuje activity
JOY0 R řádku a naopak.

**Performance:** hot-path hook gated přes interní tracking flag. Default
je zapnuto, ale gated na otevřené okno - panel zavřený = jeden flag
check + branch predictor = zero overhead. Při otevřeném panelu = pole
indexace + uint16 saturated inkrement.

## History ring buffer

Per IORQ + MMIO event záznam obsahuje: frame, scanline, pixel column,
CPU cycle, PC, port (resp. MMIO adresu), value, flagy (READ / MEMORY).
Velikost záznamu je 20 bajtů.

**Capacity:** default 10000 events (~200 KB). Sticky header `Capacity:`
dropdown nabízí hodnoty 1000 / 5000 / 10000 / 25000 / 50000. Změna
realokuje buffer a resetuje historii.

**Ring layout:**
- `head` - next write index (modulo capacity).
- `count` - capped at capacity.
- `overflow` - příznak po prvním wrap-around.
- Logické indexování od 0 = nejstarší, count-1 = nejnovější.

**Hook:** gated přes stejný tracking flag jako activity.

## History tab UI

**Layout:**

```
+-- I/O Ports (History tab) ------------------------------+
| [Filter...] [Clear] (visible/total) | [Latest]         |
| [Auto-follow] | [Clear history]                        |
| Filter error: <msg>                       (červeně,     |
|                                            jen při parse|
|                                            fail)        |
+----------------------------------------------------------+
| Frame | Scanline | px | CPU Cycle | PC | Type | Port | Addr | Value | Description |
|-------|----------|----|-----------|----|------|------|------|-------|-------------|
| 1234  |   128    | 42 | 4567890   | 0x4042 | OUT | 0xCE | -   | 0x02 | GDG DMD: 320x200x16 |
| 1234  |   130    | 50 | 4567920   | 0x4055 | OUT | 0xCF06 | - | 0x05 | GDG BCOL = 5 |
| 1235  |    50    | 30 | 4571200   | 0x40A0 | MR  | -    | 0xE001 | 0xC2 | MMIO PPI Port B read |
+----------------------------------------------------------+
| Selected event:                                          |
|   Frame: 1235, Scanline: 50, PC: 0x40A0                  |
|   Port: 0xCE (GDG - Status (R)), Type: IN, Value: 0x80   |
+----------------------------------------------------------+
```

**Sloupce tabulky:**

| Sloupec | Význam |
|---------|--------|
| Frame | Číslo emulačního frame v okamžiku záznamu |
| Scanline | Beam row 0..311 v okamžiku záznamu |
| px | Pixel column ve scanline |
| CPU Cycle | Kumulativní T-state cycle CPU |
| PC | Adresa instrukce (klikatelná - viz níže) |
| Type | `IN` / `OUT` (IORQ) nebo `MR` / `MW` (MMIO) - 4-color row tinting |
| Port | IORQ MZ notace (`0xCE`, `0xCF06`); MMIO event = `-` |
| Addr | MMIO addr (`0xE001`); IORQ event = `-` |
| Value | `0x%02X` |
| Description | Smart decode per port + fallback na catalog name |

**Řádkové barvy (4-color tinting):**

- IN  = light green text  - IORQ read
- OUT = light orange text - IORQ write
- MR  = light cyan text   - MMIO read
- MW  = light yellow text - MMIO write

Aplikuje se na celý řádek (všech 10 sloupců). Barva je určená podle
kombinace flagů (READ, MEMORY).

**Description column:**

Smart per-port dekoder vrací krátký lidsky čitelný popis události. Pro
nejdůležitější porty (banking E0-E7, GDG DMD/BCOL/palette, PPI keyboard,
CTC, FDC, PSG, Z80 PIO, joystick) nabízí value-decoded popis. Pro porty
bez explicit decoderu se použije fallback z catalog name.

Příklady IORQ:
- 0xCE OUT 0x02 -> "GDG DMD: 320x200x16"
- 0xCF06 OUT 0x05 -> "GDG BCOL = 5"
- 0xE0 OUT -> "MEM: unmap ROM 0000 + ROM 1000"
- 0xE7 OUT 0x03 -> "MEM: MEMEXT bank 3"
- 0xD0 IN -> "PPI Port A (keyboard col read)"
- 0xF7 OUT -> "Z80 SIO (Quick Disk)"

Příklady MMIO:
- MR 0xE001 -> "MMIO PPI Port B read"
- MW 0xE008 0x0E -> "MMIO CTC0 GATE0 = 0 (audio gate)"
- MR 0xE004 -> "MMIO CTC counter 0 (audio)"

Headery tabulky jsou anglicky natvrdo (= konzistence s ostatními
debugger panely).

**Klikatelné PC v Selected Event panelu:**

`PC: 0xXXXX` v prvním řádku detail panelu je vykreslené jako tlačítko.
Akce:

- **LMB klik** -> focus do Disassembly #1 (primary), viz
  [disassembly.md](disassembly.md). Otevře debug okno pokud zavřené +
  auto-disable Follow PC pokud běží.
- **RMB klik** -> popup s položkami:

  1. **Focus to... ▶** submenu s 5 položkami "Disassembly #1..#5"
  2. **Add to bookmarks** - přidá záložku na PC adresu (symbol jméno
     pokud existuje, jinak `#XXXX` hex; comment prázdný; otevře
     Bookmarks okno)
  3. **Add breakpoint** - vytvoří PC_EXEC BP na PC

Tooltip na hover: "Focus to primary disassembly. Right-click for
additional actions."

**Click row = highlight + detail panel:**

Click řádku v tabulce:

1. Označí řádek visually.
2. Zobrazí detail panel pod tabulkou.
3. Pause auto-follow (= chce inspect, neskáče na nový event).

Detail panel pro IORQ event:

```
Frame: 130135  Scanline: 106  px: 42  Cycle: 4567890  PC: 0x2BCF
Port: 0xCE  Type: IN  Value: 0x70
Description: GDG Status (VBLN/HBLN/VSY/HSY)
```

Detail panel pro MMIO event:

```
Frame: 130135  Scanline: 106  px: 42  Cycle: 4567890  PC: 0x2BCF
Addr: 0xE001  Type: MR  Value: 0xC2
Description: MMIO PPI Port B read
```

Port name lookup používá multi-entry handling (= správně rozliší 0xCE
IN "Status" od 0xCE OUT "DMD").

**Selected event panel - splitter:**

Mezi tabulkou a Selected event panelem je horizontal splitter. User
může splitter drag vertikálně pro libovolný resize 60..400 px. Výška
je persistovaná v cfg klíči `detail_panel_height`.

**Auto-follow:**

- Default ON.
- Checkbox + tlačítko `[Latest]` jsou v History tab sticky header.
- Při auto-follow scrolluje tabulku na poslední řádek.
- Detect user scroll up: pokud scroll position je výše než 32 px nad
  spodkem, automaticky disable a nezavolá auto-scroll (= respektuje
  user akci).
- Tlačítko `[Latest]` re-enable + immediate scroll dolů (bezpodmínečně,
  i když user byl výše).

**Clear history:** tlačítko v sticky header zahodí všechny eventy.

## Filter syntax

Filter input v History tab podporuje **boolean expression** nad
atomickými tokeny (= leaf): AND / OR / NOT + závorky.

### Gramatika a operátory

```
expr     = or_expr
or_expr  = and_expr  ( ( "|" | "OR" ) and_expr )*
and_expr = unary     ( ( "&" | "AND" | WS )  unary )*
unary    = "!" unary | atom
atom     = "(" expr ")" | leaf_token
```

Operátory v sestupné precedenci:

| Operátor | Význam | Poznámka |
|----------|--------|----------|
| `(` `)` | grouping | nejvyšší precedence |
| `!` | unární NOT | nad leaf tokenem nebo nad závorkou |
| WS, `&`, `AND` | implicit / explicit AND | `AND` je case-sensitive uppercase keyword |
| `\|`, `OR` | OR | nejnižší precedence; `OR` je case-sensitive uppercase keyword |

**Case-sensitivita keywordů:** `AND` a `OR` jsou rezervovaná slova jen
pokud jsou psaná **velkými písmeny**. Lowercase `and` / `or` (případně
`And`, `oR`, ...) se interpretuje jako plain-text name match -
zachovává backward compat pro porty s "OR" v jméně. Symbolové varianty
`&` a `|` jsou samostatné znaky a žádnou case-sensitivitu nemají.

Filter `port:CE pc:4042` se interně parsuje shodně jako
`port:CE & pc:4042` nebo `port:CE AND pc:4042`. Per-token `!` prefix
(= `!port:CE`) je platný a interně se collapse-uje do leaf uzlu
s negate flagem.

### Atomické tokeny (= leaf v boolean výrazu)

| Filter | Význam |
|--------|--------|
| `port:CE` | **low byte match** - `(event.port & 0xFF) == 0xCE`, ignoruje random high byte (8-bit IORQ) |
| `port:C0-CF` | low byte range 0xC0..0xCF (inclusive) |
| `port16:00CE` | **full 16-bit match** - `event.port == 0x00CE` přesně |
| `port16:CF06` | full 16-bit 0xCF06 (= 0xCF family sub-registry) |
| `port16:CF00-CF0F` | full 16-bit range |
| `pc:4042` | PC == 0x4042 |
| `pc:4000-40FF` | PC range 0x4000..0x40FF |
| `frame:>100` | frame > 100 |
| `frame:<100` | frame < 100 |
| `frame:50-150` | frame range 50..150 |
| `frame:42` | frame == 42 |
| `value:42` | value == 0x42 |
| `value:00-7F` | value range 0x00..0x7F |
| `cycle:>1000000` | cumulative T-state cycle > 1M |
| `cycle:<500000` | cycle < 500000 |
| `cycle:1000-2000` | cycle range |
| `addr:E008` | jen MMIO eventy s addr 0xE008 |
| `addr:E000-E008` | MMIO addr range |
| `in` | IN (CPU read) eventy + MR (memory read) |
| `out` | OUT (CPU write) eventy + MW (memory write) |
| `mr` | jen MR (memory read 0xE000-0xE008) |
| `mw` | jen MW (memory write 0xE000-0xE008) |
| `text` | bez prefixu = case-insensitive substring na port name |
| `!port:CE` | exclude low byte 0xCE (token-level negace) |
| `!port16:00CE` | exclude full 16-bit 0x00CE |
| `!pc:4042` | exclude PC 0x4042 |
| `!frame:>100` | invertuje range -> match frame <= 100 |
| `!value:42` | exclude value 0x42 |
| `!in` / `!out` | exclude IN / exclude OUT |
| `!mr` / `!mw` | exclude MR / exclude MW |
| `!addr:E008` | exclude MMIO 0xE008 |
| `!text` | exclude porty jejichž name obsahuje text |

**`port:` vs `port16:`:** Z80 8-bit IORQ instrukce (`OUT (n),A` /
`IN A,(n)`) dávají na adresní bus `(B << 8) | n`, kde B je high byte
z A registru (resp. libovolný stav). Tj. stejná instrukce
`OUT (0xCE),A` se zapíše do historie s různým `event.port`
(0x20CE, 0x40CE, ...) podle aktuálního obsahu A. `port:` prefix
matchuje **jen low byte** a tyto random varianty zachycuje všechny.
`port16:` prefix porovnává **plnou 16-bit hodnotu** - vhodné pro
explicitní `LD BC,nn; OUT (C),A` kde B je deterministický (např.
0xCFXX sub-registry GDG).

Hex literály jsou case-insensitive (`port:CE` = `port:ce`).

### Negace `!`

Negace `!` se chová ve dvou variantách, sémanticky ekvivalentních:

1. **Token-level `!port:CE`** - per-token prefix. Parser interně
   collapse-uje do single LEAF uzlu s `negate=true` flagem.
2. **Sub-expression `!(...)`** - unární NOT nad celým podstromem.
   Invertuje boolean výsledek vyhodnocení podstromu. Příklad:
   `!(port:CE pc:42)` = NOT (port==CE AND pc==42).

**Sémantika pro ne-aplikabilní leaf:** Pokud leaf typu `addr:` /
`mr` / `mw` se vyhodnocuje proti **non-MMIO** eventu (= IORQ), raw
match je vždy false. NOT to flipne na true. Tj. `!addr:E008` projde
i pro čistý IORQ event (= "není v MMIO range 0xE008"). Pokud chceš
filter "jen MMIO eventy mimo 0xE008", kombinuj explicitně:
`!addr:E008 & (mr | mw)`.

### Sjednocená `in` / `out` semantika

- `in`  = matchuje IN i MR (= flags bit READ set)
- `out` = matchuje OUT i MW (= flags bit READ unset)

Pravidla:

- `addr:` filter rejectuje IORQ events.
- `mr` / `mw` rejectuje IORQ + opačný směr.

### Příklady použití

| Use case | Filter |
|----------|--------|
| Sledovat scroll registry | `port16:CF03` (= SW = scroll width) |
| Find paint loop po startu | `frame:>100 port:CE` |
| Border color změny | `port16:CF06` (= BCOL, deterministic B=0xCF) |
| Status reads během VBlank polling | `in port:CE` |
| Vše z PSG init kódu | `pc:4000-40FF out psg` |
| OUT na port CE nebo CF | `port:CE \| port:CF` |
| Dvě nezávislé situace | `(port:CE pc:4000-40FF) \| (port16:CF06 frame:>100)` |
| Hodnota v skupině | `value:42 \| value:43 \| value:44` |
| Vyloučit skupinu | `port:CE !(pc:4000-40FF)` |
| Jen MMIO bez IORQ | `!(in \| out)` (= ekvivalent `mr \| mw` přes negaci) |
| OR uvnitř, AND vně | `pc:4000-40FF & (port:CE \| port:CF)` |
| Dvojitá negace | `!!port:CE` (= `port:CE`) |

### UI quick-actions (right-click row)

Right-click na řádek v History tabulce otevře kontext menu se čtyřmi
sadami akcí, které manipulují s aktuálním filter stringem:

- **`Set: ...`** - přepíše filter (= nahradí celý obsah inputu).
- **`Add AND: ...`** - appendne token s mezerou před (= implicit AND).
- **`Add OR: ...`** - appendne s ` | ` před (= OR s celým existing
  filtrem). Pozor na operator precedence: `&` má vyšší prioritu než
  `|`, takže `port:CE | port:CF` + `Add AND: pc:42` ->
  `port:CE | port:CF pc:42` = `port:CE OR (port:CF AND pc:42)` (= CE
  bez pc:42 projde). Pro správné "zúžit celý OR řetězec AND-em" je
  varianta **`Add AND group:`** níže.
- **`Add AND group: ...`** - obalí celý dosavadní filter do
  závorky a připojí token přes `&`: `(current) & token`. Pro prázdný
  filter degraduje na bare token.
- **`Add OR group: ...`** - analogicky `(current) | token`.

Každá sada nabízí cca 6-12 variant (port low byte, port16 full, pc,
cycle, value, addr, type R/W, negate varianty). `Add ... group:` sada
je menší (= port/addr/pc + jejich negace), protože její typický use
case je hrubé zúžení nad existujícím chainem, ne fine-grained tweak.

Pod blokem filter quick-actions je oddělená sekce s akcí:

- **`Show port in Overview`** - přepne aktivní tab z History na
  Overview a nastaví Overview filter input tak, aby matchoval port
  aktuálního eventu. Logika hodnoty filtru:
  - **MMIO event** (Type = MR/MW, adresa 0xE000-0xE008): filter =
    full 4-digit hex adresy (např. `E001`). Overview substring
    match na MMIO entry je unikátní.
  - **IORQ event** (Type = IN/OUT): filter = 2-digit hex z low byte
    portu (např. `D8`). Overview ukáže všechny porty s tímto low
    byte, včetně případné 0xCF family.

Akce je symetrický counterpart k položce `Show in History tab`
v Overview row context menu (= opačný směr cross-tab navigace).
Stejně jako "Show in History tab" akce **přepíše** aktuální
Overview filter (= případný předchozí filter je ztracen).

### Limity

- **Filter string buffer:** 128 bajtů.
- **Max AST uzly:** 64. Při přesažení parser ohlásí
  `Expression too complex (max 64 nodes)`. Příliš hluboké vnoření je
  defenzivně chyceno tímtéž limitem.
- **Plain-text name token:** max 63 znaků. Delší ohlásí
  `Name token too long`.

### Parse error

Pokud parser selže, History tab zobrazí pod sticky header **červený
text** s error message, např.:

```
Filter error: Invalid port hex value (8-bit)
```

Žádné události nejsou skryty (= bezpečné chování při typu).

Možné parse errors:

| Error | Typická příčina |
|-------|-----------------|
| `Invalid port hex value (8-bit)` / `(16-bit)` | špatný hex (např. `port:XYZ`) |
| `Invalid port range (8-bit)` / `(16-bit)` | špatný range (např. `port:XX-YY`) |
| `Invalid pc hex value` / `Invalid pc range` | špatný hex / range za `pc:` |
| `Invalid addr hex value` / `Invalid addr range` | špatný hex / range za `addr:` |
| `Invalid <name> value` / `Invalid <name> range` | špatný hex pro frame/value/cycle |
| `Invalid <name>:>N value` / `Invalid <name>:<N value` | špatný compare literal |
| `Empty <name> value` | prefix bez hodnoty (např. `pc:`) |
| `Empty value after prefix` | obecná varianta téhož |
| `Empty token after '!'` | `!` bez následujícího atomu |
| `Empty expression` | prázdný filter v subexpression |
| `Empty expression before '\|'` / `before '&'` | `\| port:CE`, `& port:CE` |
| `Empty expression after '\|'` | `port:CE \|` (trailing OR) |
| `Empty expression before 'OR'` / `before 'AND'` | totéž s keyword variantou |
| `Empty expression before ')'` | `(port:CE \|)` apod. |
| `Empty expression between two 'OR'` | `port:CE \| \| port:CF` |
| `Empty expression between AND and 'OR'/'\|'` | `port:CE & \| port:CF` |
| `Trailing AND operator` | `port:CE &` |
| `Unexpected operator after AND` | `port:CE & &` |
| `Expected ')'` | nezavřená závorka |
| `Unexpected ')'` | nepárová zavírací závorka |
| `Unexpected token` | např. samostatný `&` na startu |
| `Unexpected trailing token` | text za syntakticky kompletním výrazem |
| `Empty expression in parens` | `()` nebo `!()` |
| `Expression too complex (max 64 nodes)` | příliš velký výraz |
| `Unknown filter prefix` | neznámý prefix před `:` (např. `foo:bar`) |
| `Name token too long` | plain-text token > 63 znaků |

## Memory-mapped I/O 0xE000-0xE008

V MZ-700 mode (banking E2/E0) je dolní polovina ROM space namapována na
mirror PIO/CTC/GDG na adresy 0xE000-0xE008. CPU instrukce
`LD A,(0E000h)` / `LD (0E008h),A` přistupují přes memory bus (MREQ),
ne přes IO bus (IORQ), ale fyzický cíl je stejný HW chip jako u IORQ
0xD0-0xD7 / 0xCE.

I/O Ports panel sleduje tyto MMIO eventy v jednom sjednoceném ringu
s IORQ events. Eventy mají flag `MEMORY` set a směr je rozlišován
flagem `READ`. Type sloupec v History tabulce zobrazí `MR` (memory
read) nebo `MW` (memory write).

**MMIO entries v katalogu:**

| Addr   | Name                              | Direction |
|--------|-----------------------------------|-----------|
| 0xE000 | MZ-700 mmio - PPI Port A (R/W)    | R/W       |
| 0xE001 | MZ-700 mmio - PPI Port B (R/W)    | R/W       |
| 0xE002 | MZ-700 mmio - PPI Port C (R/W)    | R/W       |
| 0xE003 | MZ-700 mmio - PPI Control (W)     | W         |
| 0xE004 | MZ-700 mmio - CTC Counter 0 (R/W) | R/W       |
| 0xE005 | MZ-700 mmio - CTC Counter 1 (R/W) | R/W       |
| 0xE006 | MZ-700 mmio - CTC Counter 2 (R/W) | R/W       |
| 0xE007 | MZ-700 mmio - CTC Control (W)     | W         |
| 0xE008 | MZ-700 mmio - GDG DMD/Status (R/W)| R/W       |

V Overview tabu je 9 entries 0xE000-0xE008 sdružených do collapsible
skupiny "MZ-700 mem-mapped IO".

**Filter syntax** pro MMIO viz [Filter syntax](#filter-syntax) -
relevantní tokeny: `addr:`, `mr`, `mw`, `!addr:`, `!mr`, `!mw` a
sjednocené `in` / `out`.

**Quick Add MEM BP:** Right-click menu nad MMIO entry v Overview nabízí
**Add MEM R BP** a **Add MEM W BP**, které otevřou Edit panel
breakpointu s pre-fill `addr` field (= analogie k Add IORQ R/W BP pro
IORQ entries). Viz [breakpoints/types.md](breakpoints/types.md).

## Persistence (cfg sekce)

Sekce `[IO_PORTS_PANEL]` v INI souboru emulátoru (default `mz800emu.ini`
v home dir). Klíče:

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `collapse_gdg` | BOOL | 0 | Overview - GDG sekce sbalená |
| `collapse_ppi8255` | BOOL | 0 | Overview - 8255 PPI sekce sbalená |
| `collapse_ctc8253` | BOOL | 0 | Overview - 8253 CTC sekce sbalená |
| `collapse_fdc` | BOOL | 0 | Overview - FDC sekce sbalená |
| `collapse_memory` | BOOL | 0 | Overview - Memory bank sekce sbalená |
| `collapse_psg` | BOOL | 0 | Overview - PSG sekce sbalená |
| `collapse_joystick` | BOOL | 0 | Overview - Joystick sekce sbalená |
| `collapse_pioz80` | BOOL | 0 | Overview - Z80 PIO sekce sbalená |
| `collapse_mz700_mmio` | BOOL | 0 | Overview - MZ-700 mem-mapped IO sekce sbalená |
| `collapse_cmthack` | BOOL | 0 | Overview - CMT loader hack sekce sbalená |
| `collapse_unicard` | BOOL | 0 | Overview - Unicard sekce sbalená |
| `collapse_ramdisk` | BOOL | 0 | Overview - Ramdisk (Pezik / Std) sekce sbalená |
| `collapse_ide8` | BOOL | 0 | Overview - IDE8 sekce sbalená |
| `collapse_qdisk` | BOOL | 0 | Overview - QDISK (Z80 SIO) sekce sbalená |
| `history_capacity` | UNSIGNED | 10000 | Velikost ringu (1000-50000) |
| `history_auto_follow` | BOOL | 1 | Default auto-follow ON v History tab |
| `tracking_active` | BOOL | 1 | IORQ + MMIO tracking (default ON; gated na otevřené okno) |
| `detail_panel_height` | UNSIGNED | 160 | Výška Selected event panelu px (60-400) |
| `heat_text_active` | UNSIGNED | 1 | hits/s práh pro zelený text aktivního řádku (rozsah 1..1000000) |
| `heat_bg_hot` | UNSIGNED | 10000 | hits/s práh pro červený bg tint hot řádku (rozsah 1..10000000) |
| `record_mask` | TEXT | (vše 1) | Per-port selective recording mask, 64-hex string = 256 bool flagů; změna z UI přes "Rec" sloupec v Overview |

**Tracking default ON:** klíč `tracking_active` má default `1`.
Tracking je **gated na otevřené okno** - hot-path hook se aktivuje jen
když je IO Ports panel otevřený. Při zavření okna se flag runtime
nastaví na 0 a callback path se vrátí na default (= zero overhead).
Cfg preference zůstává zachovaná pro další otevření.

Pokud uživatel chce **trvale tracking vypnout** (= žádný hot-path hook
ani při otevřeném okně), odškrtne checkbox "Track" v sticky header a
nastavení se uloží do INI (`tracking_active = 0`) při exit emulátoru.

**Pozn. cfgmodule UNSIGNED:** Parser parsuje hodnoty vždy jako hex
(kompatibilita se save formátem `0x%02x`). Tj. `history_capacity = 0x2710`
(= 10000 dec). User-editovaný INI s desítkovým literálem se interpretuje
jako hex.

## Use cases

Praktické scénáře použití History tabu při debugu MZ-800 software:

| Use case | Filter | Co očekávat |
|----------|--------|-------------|
| Sledovat scroll registry | `port16:CF03` | OUT events na SW (scroll width) - typicky pri startu obrazu nebo HW scroll efektu |
| Find paint loop po startu | `frame:>100 port:CE` | OUT na DMD po počátečním ROM bootu - obvykle main game loop |
| Border color změny | `port16:CF06` | OUT na BCOL (4-bit barva) - mid-frame raster efekty, animace flagů |
| Status reads během VBlank polling | `in port:CE` | IN z 0xCE Status (bit 7 = VBLN) - typický busy-wait před repaint |
| PSG init kódu | `pc:4000-40FF out psg` | OUT na 0xF2/F3 v rané ROM bootcestě |
| Watch keyboard scan loop | `port:E0` | OUT (8255 PPI Port C, KSTROBE) v keyboard scan |
| FDC operace | `port:DC-DF` | IN/OUT range pro WD279x čtení/zápis - během disk I/O |
| Banking změny | `port:E0-E4` | Memory banking switching - typicky PEHU/RAM/ROM přepínání |

**Workflow doporučení:**

1. Zaškrtnout `Track` v sticky header (= zapnout zaznamenávání).
2. Spustit emulátor v debugger active mode (= debugger loop musí být
   aktivní, jinak callbacky nejsou bound).
3. Reprodukovat sledovaný scénář v emulátoru (= load MZF, spustit hru).
4. Přepnout na History tab, aplikovat filter (viz tabulka výše).
5. Click řádek pro detail (Frame/PC/Port name/Type/Value).
6. Pro bližší analýzu PC adres skočit do disasm panelu kliknutím na
   PC v detail panelu (viz [disassembly.md](disassembly.md)).

## Troubleshooting

**Activity zůstává 0/s i při běhu emulátoru:**

- Zkontrolovat `Track` checkbox v sticky header. Default je ON, ale
  pokud byl dříve odškrtnut a uložen do cfg, zůstává OFF.
- Verify že debugger je v active mode. Pokud ne, hot-path hook se
  nikdy nezavolá. Track checkbox edge triggeruje swap callbacků
  automaticky.
- Verify že emulátor není pause (= žádné nové IORQ).

**History tab je prázdný:**

- Stejné jako "Activity 0/s" - verify Track ON + emulátor běží.
- Pokud Track byl zapnutý právě teď, history začne plnit od tohoto bodu.
  Buffer je in-memory ring, ne persistovaný (= restart emulátoru =
  clear).

**Některé porty zobrazují `??` v Hex sloupci:**

- Není to bug. `??` = port nemá readable mirror v emulátoru:
  - Write-only HW bez interního storage (= no side-effect-free read).
  - Side-effect read (= PPI Port B keyboard scan = strobe pulse).
- Tooltip nad `??` vysvětlí důvod.
- Aktuální stav lze často přečíst nepřímo: GDG status registr 0xCE
  bit 2 čte CKSW signál (= mirror pro 0xCF07).

**Filter "Filter error: ..." červený text:**

- Parser detekoval syntax chybu (= špatný hex literál, missing value,
  invalid range). Žádné události skryté nejsou (= bezpečné chování).
- Viz [Filter syntax](#filter-syntax) tabulka pro podporované tokeny.

**16-bit IORQ port nezachycen filtrem `port:CE`:**

- 16-bit IORQ (= 0xRRCF group) má v history plnou 16-bit adresu (např.
  `0x06CF` pro BCOL). Filter `port:CE` matchuje **low byte 0xCE**,
  neshoduje se s 0x06CF (low byte = 0xCF).
- Pro 16-bit port použij `port16:` prefix s plnou 16-bit hodnotou,
  např. `port16:06CF` nebo `port16:CF00-CFFF` pro celou rodinu.

**8-bit IORQ port nezachycen filtrem `port16:00CE`:**

- 8-bit IORQ (`OUT (n),A` / `IN A,(n)`) dává na bus `(B << 8) | n`,
  kde B je high byte z A registru, resp. random. Filter `port16:00CE`
  matchuje jen události s `event.port == 0x00CE` přesně - tj. jen
  případy kdy A=0 nebo B=0 deterministicky.
- Pro low-byte-only match (= zachytit všechny 8-bit IORQ s daným n
  bez ohledu na random B) použij `port:CE` prefix.

## Související panely

- **Events** ([`event-viewer.md`](event-viewer.md)) - real-time pohled
  na **všechny** eventy emulátoru (= nejen IORQ + MMIO, ale i CPU_INT,
  GDG, banking, PSG, BP fire, HALT/RST, USER_MARK). Vlastní in-memory
  ring 50000 eventů (default), 24 B per event. Use case: pokud
  potřebuješ vidět IORQ v širším kontextu CPU/HW dění (= co se v okolí
  IN/OUT dělo na CTC, INT pinech, BP), použij Events okno. Pokud
  potřebuješ úzce port-focused historii s rychlým "Add IORQ R/W BP"
  na řádku, použij History tab v I/O Ports panelu. Oba panely mají
  oddělené ringy a oddělenou hot-path gate (= zapnutí jednoho nezapne
  druhý).

- **Trace Suite** ([`Trace_Suite.md`](Trace_Suite.md)) - post-mortem
  souborový log per-subsystem (iorqlog / intlog / hwlog / cputrack /
  marklog). Vhodný pro dlouhé runy a offline RE pipeline. IORQ data
  jsou v `iorqlog` chunks shodná s tím, co je v I/O Ports History tab,
  jen v binárním formátu pro externí parser.

- **Memory Map** ([`memory-map.md`](memory-map.md)) - aktuální stav
  mapování paměti per 4 kB stránka včetně MemExt expansion. Cílem
  cross-window navigace z banking portů 0xE0-0xE7.

- **Breakpoints** ([`breakpoints/README.md`](breakpoints/README.md)) -
  IORQ_R / IORQ_W / MEM_R / MEM_W breakpoint typy mají v I/O Ports
  panelu Overview + History rychlou akci "Add BP" s pre-fill port /
  address fieldy.
