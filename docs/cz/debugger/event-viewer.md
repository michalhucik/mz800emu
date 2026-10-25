# Events panel (Event Viewer)

Events panel je real-time pohled na významné HW a CPU eventy emulátoru
(IORQ / MMIO, INT / NMI / RETI, banking, GDG mode/palette, PSG, CTC,
PIO, FDC, BP fire, user mark, HALT/RST, SYS lifecycle). Inspirováno
**Mesen2 Event Viewer**.

## Obsah

- [Účel panelu](#účel-panelu)
- [Otevření okna](#otevření-okna)
- [Toolbar](#toolbar)
- [Log tab](#log-tab)
- [Strip tab](#strip-tab)
- [Bookmarky](#bookmarky)
- [Saved filter presety](#saved-filter-presety)
- [Row coloring](#row-coloring)
- [Hotkeys](#hotkeys)
- [Kategorie a subtypy](#kategorie-a-subtypy)
- [Filter syntax](#filter-syntax)
- [Pause-on-match (stream BP)](#pause-on-match-stream-bp)
- [Auto-mark on match](#auto-mark-on-match)
- [Detail sloupec dekódéry](#detail-sloupec-dekódéry)
- [Sub sloupec short codes](#sub-sloupec-short-codes)
- [Group by v Log tabu](#group-by-v-log-tabu)
- [Heatmapa per-frame](#heatmapa-per-frame)
- [Export / Import ringu](#export--import-ringu)
- [FDC command tracking](#fdc-command-tracking)
- [SYS kategorie](#sys-kategorie)
- [Interakce - klik a context menu](#interakce---klik-a-context-menu)
- [Follow tail](#follow-tail)
- [Persistence (cfg sekce)](#persistence-cfg-sekce)
- [Související panely](#související-panely)

## Účel panelu

Events panel doplňuje Trace Suite (= post-mortem souborový log) o
**online live pohled** na to, co se právě v emulátoru děje. Slouží k:

1. **Live debug** - "co se právě teď děje v emulátoru" v reálném čase,
   bez nutnosti zastavit emu a procházet stavy chip po chipu.
2. **Cross-window navigace** - klik na řádek = pause emu a skok do
   disassembleru na PC, kde event nastal.
3. **Filter-first analýza** - kategorií je 24, eventů za sekundu
   tisíce; filter syntax a per-kategorie checkboxy oddělují signál od
   šumu.

Rozdíl proti dalším existujícím panelům:

| Panel             | Co řeší                                  | Kdy ho použít                   |
|-------------------|------------------------------------------|---------------------------------|
| I/O Ports History | Posledních ~10000 IORQ + MMIO eventů     | Specificky pro porty            |
| Trace Suite       | Disk soubor (cputrack/intlog/hwlog/...)  | Post-mortem RE pipeline         |
| **Events**        | Posledních 50000 HW/CPU eventů (in-mem)  | Live "co se zrovna děje"        |

## Otevření okna

Menu **Debugger -> Events** (klávesová zkratka zatím nedefinována).
Okno je dokovatelné, persistované přes cfg klíč
`[EVENT_VIEWER_WINDOW] window_open`.

**První kroky po otevření:**

1. V toolbaru přepni **Mode** combo z `OFF` (= default) na
   `WHEN_WINDOW_OPEN` nebo `ALWAYS`. V `OFF` se do ringu nezapisuje
   (= tabulka je prázdná).
2. Spusť / dál nech běžet emu - eventy se začnou objevovat v tabulce.
3. Pokud potřebuješ jen určité kategorie, klikni na **Categories**
   dropdown v toolbaru a odškrtni zbytek.

## Toolbar

```
Mode: [WHEN_WINDOW_OPEN v]   Capacity: [50000     ]   [Clear]
Categories [v]   [Only IRQ] [Only banking] [Only video] [Only PSG] [Only memory] [Hide noise]
Filter: [.........................................] [?]
Follow tail: [x]
Counters:  Total: 12450   Filtered: 230   CPU_INT: 12   IORQ: 1840   GDG: 80 ...
```

### Mode combo

| Hodnota             | Aktivní kdy                                | Použití                  |
|---------------------|--------------------------------------------|--------------------------|
| `OFF`               | Nikdy                                      | Default, žádná režie     |
| `WHEN_WINDOW_OPEN`  | Pouze pokud je Events okno otevřené v UI   | Standard debug           |
| `ALWAYS`            | Vždy během běhu emu                        | Power user, vždy v paměti|

Default je `OFF` aby běžní uživatelé neměli žádnou hot-path režii.
Po prvním otevření okna typicky přepneš na `WHEN_WINDOW_OPEN`.

### Capacity

Slider v rozsahu 10 000 - 200 000 eventů. Default 50 000. Změna
realokuje ring a **zahodí předchozí data** (= ImGui zobrazí potvrzovací
hlášení).

Paměťová stopa:

- 10 000 events = 240 KB
- 50 000 events = 1.2 MB
- 200 000 events = 4.8 MB

### Clear button

Vyprázdní ring (= count = 0, ring zůstane alokovaný). Vhodné po změně
testovaného scénáře.

### Categories dropdown

Per-kategorie visibility (24 zaškrtávacích položek). Vypnutá kategorie
**vůbec nevstupuje do ringu** (= bitmask v hot-path gate, nulový
overhead pro odfiltrovaná data).

Quick toolbar buttony **Only IRQ / Only banking / ...** přednastaví
masku na typický subset (= jeden klik namísto manuálního odškrtávání).

### Filter textbox

Text-based filter (viz [Filter syntax](#filter-syntax) níže). Aplikuje
se v UI vrstvě nad ringem (= vidíš jen řádky, které matchnou). Filtr
funguje jako AND k Categories - oba musí dovolit, aby se řádek
zobrazil.

**?** popup vedle textboxu ukazuje stručný cheatsheet syntaxe.

### Counters

Live statistiky:

- **Total** = počet eventů v ringu (max = capacity)
- **Filtered** = počet eventů po aplikaci Categories + Filter
- **Per-kategorie** = počet eventů per kategorie v ringu (pomáhá poznat
  "co tu vlastně je nejvíc")

## Log tab

Tabulkový pohled. Sloupce:

| Sloupec | Význam                                                          |
|---------|-----------------------------------------------------------------|
| Pxclk   | Pixel clock counter (NE CPU T-states) - sjednocený timestamp    |
| Frame   | Číslo snímku (= `screens_total`)                                |
| Sline   | Scanline v rámci aktuálního snímku                              |
| Px      | Pixel column v rámci aktuálního scanline                        |
| PC      | CPU PC v okamžiku eventu (hex 16-bit)                           |
| Cat     | Název kategorie (= `cpu_int`, `iorq_in`, `gdg_mode`, ...)       |
| Sub     | Subtype short code (max 8 znaků, např. `HALT_E`, `CTL_W`)       |
| Detail  | Krátký dekódovaný popis (rich per-kategorie dekódér)            |

### Význam Pxclk

`Pxclk` je hodnota z **pixel clock domény** (= GDG vnitřní oscilátor,
MZ-800 17.7345 MHz). NEodpovídá CPU T-states (= MZ-800 3.5469 MHz =
pxCLK / 5). Důvod: timing v Event Vieweru je sjednocený s trace-suite
formátem (= cross-mergovatelné s hwlog/intlog disk chunky), a GDG
events (= VBLN/VS/HS) musí být zachytitelné s pixel přesností.

Přepočet: CPU cycle = `pxclk / 5` (MZ-800/700), `pxclk / 4` (MZ-1500).

### Sline / Px = raster pozice

`Sline` = `pxclk_in_screen / screen_width` (typicky 320 pro MZ-800 H40
nebo MZ-700, 640 pro MZ-800 H80).

`Px` = `pxclk_in_screen % screen_width`.

Použití: pro raster effects debug (= "v jaké scanline mi BP fíruje?"),
mid-frame palette switching, border raster effekty.

### Detail decoder

Detail sloupec ukazuje human-readable text formátovaný podle kategorie
eventu - kompletní tabulka v sekci
[Detail sloupec dekódéry](#detail-sloupec-dekódéry).

Příklady:

- `port=0xCE (GDG DMD) val=0x08`
- `DMD=0x08 (MZ-700 mode)`
- `PIOZ80_PA vec=0x40 isr=0x4042`
- `BP #5 reason=2 MARK`
- `"isr_entry"`
- `HALT enter`
- `RST 0x38`

## Strip tab

2D mapa eventů aktuálního snímku. Inspirováno Mesen2 Event Viewerem.
Osa X = pixel column 0..VIDEO_SCREEN_WIDTH-1, osa Y = scanline
0..VIDEO_SCREEN_HEIGHT-1. Každý event = jeden bod, barva podle
kategorie, velikost podle priority.

### K čemu

- "Kde v rasteru se mi BP fíruje?" - vidíš BP_FIRE body překryté na 2D
  mapu, hned víš sline + px.
- Per-frame raster effects debug - mid-frame palette switching ukáže
  shluk GDG_COLORS bodů napříč scanlines.
- Border raster efekty ukáží barevné pruhy bodů na konkrétních
  scanlines.
- IRQ timing (IM2 dema) ukáže IRQ_ACK_IM2 jako pravidelné červené body.

### Toolbar Strip tabu

```
Mode: [Fit to window v]   Zoom: [1.00x ====]   [x] Show previous frame   [ ] Grid   [x] Legend
Frame: 1234  Events drawn: 487
> Strip colors  (collapsible per-kategorie color picker)
```

### Mode combo

| Hodnota         | Význam                                                        |
|-----------------|---------------------------------------------------------------|
| Fit to window   | Canvas se proporčně škáluje do dostupného prostoru. Zoom slider grayed out. Default. |
| 1:1             | Canvas má fixní velikost 1 logický px = 1 fyzický px x zoom. Scrollbars pokud nevejde do okna. |

### Zoom slider

Funkční jen v "1:1" módu. Rozsah 0.5x..4.00x, default 1.00x. V canvas
hover funguje **Ctrl+kolečko myši** = zoom in / out (v 1:1 módu).

### Show previous frame

Toggle. Pokud ON, Strip kreslí body z **dvou** snímků zároveň:

- Aktuální snímek = plná alpha.
- Předchozí snímek (= current - 1) = poloviční alpha (ghost overlay).

Při hoveru tooltip ukáže hlavičku "(previous frame)" pokud event pochází
z N-1.

### Grid (toggle)

Volitelná mřížka v canvas souřadnicích:

- Sekundární čáry každých 64 px / 32 sline (jemný šedý).
- Primární zvýrazněné každých 256 px / 128 sline (sytější).

Pomáhá při orientaci - rychle vidíš "tento bod je kolem sline 96".

### Legend (toggle)

Default OFF (= šetří vertikální prostor v malém okně). Po zapnutí
kreslí pod canvas (= mezi toolbar a canvas) dva bloky:

1. **Dot size legend** - "large/medium/normal/small/tiny" + popis
   kategorií které danou velikost mají.
2. **Color legend** - 3-sloupcová tabulka jen viditelných kategorií +
   barevné tečky v reálné velikosti + jméno.

### Velikost teček per kategorie

| Velikost | Poloměr | Kategorie                                          |
|----------|---------|----------------------------------------------------|
| large    | 4 px    | BP_FIRE, USER_MARK (debugger markery)              |
| medium   | 3 px    | IRQ_ACK_IM2 (IM2 dispatch)                         |
| normal   | 2.5 px  | CPU_INT, CPU_CTRL (IM/IFF, HALT/RST)               |
| small    | 1.5 px  | default (většina HW events)                        |
| tiny     | 1 px    | GDG_VIDEO (HBLN/HS edges - jinak by přehluštil)    |

### Color picker s Reset

V toolbaru "> Strip colors" collapsible. Per-kategorie ColorEdit3 ve
4-sloupcovém gridu. "Reset to defaults" tlačítko obnoví celou paletu
na default Mesen-inspired scheme.

**Důležité:** Barva je **sdílená s Log tabem row coloring** - změna v
Strip Colors picker okamžitě prosvítí v Log tabu (= jedna SSOT pro oba
pohledy).

### Scanline cursor

Žlutý cross-hair na aktuální pozici elektronového paprsku. Vertikální
čára = aktuální pixel column, horizontální = aktuální scanline. Vedle
průsečíku malý text "sline=N px=M".

Při běhu emu kurzor "letí" rasterem; při pauznutém emu (Ctrl+F5 nebo
Alt+P) stojí na přesné pozici, kde emu zamrzl. Užitečné pro raster
timing debug.

### Visible screen rect vs active canvas rect

Canvas zobrazí dva polotransparentní rámečky:

- **Bílý rámeček** = visible display area (= včetně borderu, MZ-800 PAL
  928 x 288 px).
- **Šedý rámeček** = active canvas / aktivní pixel oblast (= bez
  borderu, MZ-800 H40 320x200 ve středu).

Eventy mimo visible rect = blanking interval (HSYNC, VSYNC) - tam se
běžné HW events také logují, ale border raster effects se typicky
odehrávají právě v border zóně mezi visible rect a canvas rect.

### Hover tooltip

Hover nad bodem zobrazí tooltip:

```
(previous frame)         <- pokud event z N-1
---
Pxclk: 12345678
Frame: 1234  Sline: 96  Px: 240
PC: 0x4042
---
Cat: bp_fire
Sub: MARK
---
BP #5 reason=2 action=MARK
```

Tooltip obsahuje i plný popis subtype (= reuse stejných stringů, které
ukazuje Log tab hover na Sub buňce).

### Klik UX

| Akce             | Co dělá                                                                 |
|------------------|-------------------------------------------------------------------------|
| Single click     | Select highlight (bílý outline kolem bodu).                             |
| Double click     | Pause emu + show in disasm na PC eventu.                                |
| Right click      | Context menu (Pause+disasm / Pause here / Bookmark toggle / Show in Log tab / Copy event). |
| Ctrl+wheel hover | Zoom in / out (jen v 1:1 módu).                                         |

### "Show in Log tab"

Context menu položka. Přepne TabBar na Log tab a auto-scroll na řádek
příslušného eventu (= ve středu viewportu). Užitečné pro cross-tab
přechod "ve Stripu vidím podezřelý bod -> chci ho zobrazit v
tabulkovém detailu".

## Bookmarky

Per-event hvězdičkový marker pro označení zajímavých eventů pro
pozdější návrat. Identifikováno klíčem `(frame, pxclk_in_screen)`.

### Jak přidat / odebrat

- **Log tab** - sloupec s hvězdou. Klik = toggle. Žlutá = označený,
  šedá = ne.
- **Strip tab** - right click na bod -> context menu "Bookmark this
  event" / "Remove bookmark".

### Jak navigovat

V toolbaru bookmark control řádek:

```
* 5    < Prev   Next >   [ ] Show only *   Clear all
```

- **< Prev** - skok na předchozí bookmark od aktuálně vybraného řádku.
  Hotkey: **Ctrl+Shift+B**.
- **Next >** - skok na následující bookmark. Hotkey: **Ctrl+B**.
- **Show only *** - filter override. Skryje neoznačené eventy
  (nezávisle na aktuálním filter výrazu).
- **Clear all** - mass delete s confirm popupem "Remove N bookmarks?".

Skok na bookmark **vypne Follow tail** (= uživatel explicitně vybral
pozici a nechce ji ztratit příštím auto-scrollem). Vrátí se na ON jen
explicit klik na "Follow tail" v toolbaru nebo `F` hotkey.

### Persistence

Bookmarky jsou udržovány v rámci aktuální session. Po restartu
emulátoru se seznam zahodí.

### Drží přes ring overflow

Bookmark klíč `(frame, pxclk)` zůstává registrovaný i když daný event
už není v ringu (= ring přepsal). Po wrap se Next/Prev navigace
jednoduše přeskočí (= žádný match v aktuálním ringu). Když chceš
úklid, "Clear all" smaže celý seznam.

## Saved filter presety

User-defined library filter výrazů s perzistencí v cfg sekci
`[EVENT_LOG_FILTERS]`. Max 32 presetů per session.

### Save current as... workflow

1. Napiš filter výraz do Filter textboxu (např. `cat:cpu_int pc:38-FF`).
2. Klikni **"Save as..."** vedle Saved filters combo.
3. Modal popup "Save preset" -> zadej **Preset name** (např.
   `isr_trace`).
4. Klikni **Save**. Preset se uloží do combo + označí jako active.

Pokud zadané jméno již existuje, popup se přepne na **Replace confirm**
("Preset 'X' already exists. Replace?"). Confirm = update expr
existujícího slotu.

Validace:

- **Empty name** -> Save tlačítko disabled.
- **Empty filter expression** -> warning hlášení "Warning: saving an
  empty filter (matches all events)." Save jde dál (= legitimní case
  pro "Clear filter" preset).

### Quick filter vs Saved filter

| Aspekt        | Quick filter dropdown                    | Saved filters                |
|---------------|------------------------------------------|------------------------------|
| Definice      | Hardcoded compile-time (Only IRQ apod.)  | User-defined runtime         |
| Persistence   | Žádná                                    | Cfg `[EVENT_LOG_FILTERS]`    |
| Max count     | 10 buttonů                               | 32 slotů                     |
| Editace       | Ne                                       | "Save as..." / "Delete"      |
| Tooltip       | Expression preview per item              | Expression preview per item  |

Oba dropdowny žijí vedle sebe v toolbaru a jsou plně nezávislé. Quick
filter slouží jako "ready-made" recepty, Saved jako personalizovaná
knihovna.

### Delete UX

1. Aktivuj target preset v "Saved filters" combo (= klik na položku).
2. Klikni **"Delete"** vedle combo.
3. Confirm popup "Delete preset 'X'?" -> potvrď.

Po smazání se combo preview resetuje na "(none)", filter textbox
zůstane nezměněný.

### Cfg sekce

```ini
[EVENT_LOG_FILTERS]
preset_00_name = isr_trace
preset_00_expr = cat:cpu_int,irq_ack_im2,cpu_ctrl
preset_01_name = video_debug
preset_01_expr = cat:gdg_mode,gdg_colors sline:0-200
preset_02_name = audio_debug
preset_02_expr = cat:psg
; sloty 03..31 prázdné = nepoužité
```

Cfg sekce 32 slotů `preset_NN_name` + `preset_NN_expr`, prázdné name =
slot ignorován. Sync probíhá při každé mutaci (Save / Delete) a před
cfg save.

## Row coloring

Toggle "Color rows" v toolbaru (vedle "Follow tail"). Default ON, cfg
klíč `row_coloring`.

Když ON, řádky Log tabulky mají tmavé barevné pozadí podle kategorie
eventu (alpha 96 = výrazné ale text bílý zůstane čitelný). Sdílí barvu
s Strip tab "Strip colors" pickerem.

Když OFF, řádky jsou standardní zebra striping (alternující světlejší /
tmavší šedá).

**Tip:** Pro vlastní paletu otevři Strip tab -> "Strip colors"
collapsible -> uprav `ColorEdit3` per kategorie. Změna se okamžitě
projeví v Log tabu při dalším renderu.

## Hotkeys

Tabulka klávesových zkratek dostupných v Events okně:

| Hotkey            | Akce                                  | Scope                              |
|-------------------|---------------------------------------|------------------------------------|
| `Alt+P`           | Pause / resume emu toggle             | Events window focus                |
| `F`               | Follow tail toggle                    | Events okno focus (mimo textbox)   |
| `Ctrl+B`          | Skok na další bookmark (Next)         | Events window focus                |
| `Ctrl+Shift+B`    | Skok na předchozí bookmark (Prev)     | Events window focus                |
| `Ctrl+kolečko`    | Zoom in / out Strip canvas (1:1 mode) | Strip canvas hover                 |
| Double click      | Pause + show in disasm                | Log řádek / Strip bod              |
| Right click       | Context menu                          | Log řádek / Strip bod              |

Hotkey `F` je gated tak, aby psaní `f` do filter textboxu neotoggleovalo
Follow tail.

## Kategorie a subtypy

24 stabilních kategorií. Sloupec `Sub` v Log tabu zobrazuje **short code**
(max 8 znaků, např. `HALT_E`, `BORDER`, `CTL_W`) - kompletní tabulka
v sekci [Sub sloupec short codes](#sub-sloupec-short-codes). Hover nad
buňkou Sub ukáže plný popis v tooltipu. Numerické hodnoty subtype
zůstávají k user reference:

### IORQ kategorie (3 = `iorq_in`, 4 = `iorq_out`)

| Sub | Význam                                       |
|-----|----------------------------------------------|
| 0   | NORMAL - port mapován                        |
| 1   | UNCONNECTED - ghost cyklus na nemapovaném portu |

### BP_FIRE kategorie (18 = `bp_fire`)

| Sub | Význam                                              |
|-----|-----------------------------------------------------|
| 0   | HALT - action prázdná, emu halt                     |
| 1   | MARK - action `mark "name"`                         |
| 2   | CONTINUE - log / poke / set / var / continue        |
| 3   | IGNORE (rezervováno)                                |
| 4   | ENABLE - action `enable <bp>`                       |
| 5   | DISABLE - action `disable` nebo `disable_self`      |

### CPU_CTRL kategorie (20 = `cpu_ctrl`)

| Sub | Význam                              |
|-----|-------------------------------------|
| 0   | HALT_ENTER - HALT opcode dispatch   |
| 1   | HALT_EXIT - IRQ/NMI probudilo HALT  |
| 2   | RST_00                              |
| 3   | RST_08                              |
| 4   | RST_10                              |
| 5   | RST_18                              |
| 6   | RST_20                              |
| 7   | RST_28                              |
| 8   | RST_30                              |
| 9   | RST_38                              |

### Ostatní kategorie (hwlog passthrough)

Subtypy pro HWLOG kategorie (`gdg_mode`, `gdg_banking`, `pio8255`,
`ctc8253`, `pioz80`, `psg`, `fdc`, `memext`, `qd`, `rd`, `gdg_*`,
`cpu_int`, `cpu_pin_edge`, `irq_ack_im2`) **odpovídají 1:1**
sub_event_type hodnotám v HW-log formátu - viz
[`formats/HW-log_format.md`](formats/HW-log_format.md) per-chip
sub_event_type tabulky.

Konkrétní hodnoty - rychlý souhrn:

- `gdg_colors`: 1=BORDER, 2=PALGRP, 3=PAL, 4=PCG, 5=PACKETGROUP
- `gdg_video`: 1=VBLN_START, 2=VBLN_END, 3=VS_START, 4=VS_END,
  5=HBLN_START, 6=HBLN_END, 7=HS_START, 8=HS_END
- `pio8255`: 1=PORT_A_W, 2=PORT_B_W, 3=PORT_C_W, 4=CONTROL_W
- `ctc8253`: 1=CONTROL_W, 2=COUNTER_W
- `pioz80`: 1=MODE_W, 2=VECTOR_W, 3=INT_CTRL_W, 4=MASK_W, 5=IO_SELECT_W,
  6=DATA_W, 7=DATA_R, 8=BUS_INPUT_CHANGE, 9=IRQ_ACK_M2, A=RETI_APPLIED

## Filter syntax

Filter syntax podporuje tři vrstvy atomů: základní atomy (`cat:`, `pc:`,
`frame:` ...), symbol-aware atomy (`sym:`, `from_sym:`, `to_sym:`)
a state-aware + temporal atomy (`if iff1:N`, `before(N) <expr>` ...).

### Základní pravidla

- **Mezery = AND** (= všechny tokeny musí matchnout)
- **`!token` = negace** tokenu
- **`( a or b )` = OR** group (jen uvnitř závorky, ne na top-level)
- **`!( ... )` = negace** celé skupiny

### Operátory (struktura výrazu)

| Operátor       | Význam                                          | Příklad                          |
|----------------|-------------------------------------------------|----------------------------------|
| whitespace     | AND (implicitní)                                | `cat:cpu_int pc:4000-40FF`       |
| `!`            | negace následujícího atomu                      | `!cat:gdg_video`                 |
| `( ... or ... )` | OR group (závorky povinné, jen uvnitř závorek) | `( cat:cpu_int or cat:bp_fire )` |
| `!( ... )`     | negace celé skupiny                             | `!( pc:38 or pc:66 )`            |

### Základní atomy

| Token       | Hodnota                                | Příklad                  |
|-------------|----------------------------------------|--------------------------|
| `cat:`      | name[,name]* z 24 jmen kategorií       | `cat:cpu_int,gdg_mode`   |
| `sub:`      | num[,num]* (0..255)                    | `sub:0,1`                |
| `pc:`       | hex (0..FFFF) nebo hex-hex range       | `pc:4000-40FF`           |
| `frame:`    | dec, `>N`, `<N` nebo dec-dec range     | `frame:>100`             |
| `cycle:`    | dec, `>N`, `<N`, range; suffix `k`/`M` | `cycle:>1M`              |
| `sline:`    | dec nebo dec-dec range (0..311 PAL)    | `sline:50-150`           |
| `px:`       | dec nebo dec-dec range (0..1135 PAL)   | `px:160-200`             |
| `payload:`  | hex (`0x` prefix volitelný)            | `payload:0xCE`           |

### Hodnoty pro `cat:`

Jména kategorií (= lower case + underscore):

`cpu_int`, `cpu_pin_edge`, `irq_ack_im2`, `iorq_in`, `iorq_out`,
`mmio_r`, `mmio_w`, `gdg_mode`, `gdg_banking`, `gdg_hwscroll`,
`gdg_colors`, `gdg_wfrf`, `gdg_video`, `pio8255`, `ctc8253`, `pioz80`,
`psg`, `qd`, `fdc`, `memext`, `rd`, `bp_fire`, `user_mark`, `cpu_ctrl`,
`sys`.

### Symbol-aware atomy

Symboly se nahrávají z `.lbl` souboru přes disasm window "Load symbols".

| Token              | Význam                                  | Příklad                    |
|--------------------|-----------------------------------------|----------------------------|
| `sym:NAME`         | PC == addr symbolu `NAME` (exact)       | `sym:isr_handler`          |
| `sym:PREFIX_*`     | PC == addr libovolného `PREFIX_*` sym.  | `sym:isr_*`                |
| `from_sym:A to_sym:B` | PC v rozsahu `[A.addr, B.addr]`      | `from_sym:start to_sym:end`|

Symboly jsou bodové (= jedna adresa). `sym:NAME` matchuje **jen** pokud
`event.pc == NAME.addr` - pro rozsah skupiny rutiny použij
`from_sym:A to_sym:B`.

Negace `!sym:isr_*` matchuje všechny eventy KROMĚ těch jejichž PC je
adresou některého `isr_*` symbolu.

#### Cache a re-parse

Filter parser pre-resolvuje `sym:` názvy na konkrétní adresy při Apply
(= stisk Enter v filter textboxu). Pokud později změníš symbol DB
(= načteš další `.lbl`, přidáš user label), cached adresy jsou **stale**
dokud filter znovu neaplikuješ (= klik do textboxu + Enter).

Pokud `NAME` v DB neexistuje, leaf nematchne žádný event (= filter
fakticky všechno vyfiltruje).

### State-aware atomy

Klíčové slovo `if` introduckuje state-aware atom (= odliší od `cat:`,
`sym:` atd.). Filtry testují HW stav v okamžiku emit eventu.

| Token             | Význam                            | Příklad             |
|-------------------|-----------------------------------|---------------------|
| `if iff1:N`       | CPU IFF1 stav v okamžiku eventu   | `if iff1:1`         |
| `if im:N`         | Z80 IM mode (0 / 1 / 2)           | `if im:2`           |
| `if reason:NAME`  | BP / IFF fire reason              | `if reason:int_ack` |
| `if banking:NAME` | Memory banking summary (per-arch) | `if banking:cgrom`  |

Reason hodnoty:

| Jméno     | Význam                                  |
|-----------|-----------------------------------------|
| `reset`   | CPU reset (= IFF cleared)               |
| `ei`      | EI instrukce                            |
| `di`      | DI instrukce                            |
| `int_ack` | INT acknowledgement (= IFF cleared)     |
| `nmi_ack` | NMI acknowledgement (= IFF1 cleared)    |
| `reti`    | RETI dispatch                           |
| `retn`    | RETN (IFF1 obnoveno z IFF2)             |
| `none`    | Žádný relevantní reason                 |

Banking hodnoty (kanonická + UX synonyma):

| Kanonické jméno   | Synonyma                | MZ-800 význam         |
|-------------------|-------------------------|-----------------------|
| `default`         | -                       | ROM+VRAM výchozí      |
| `all_ram`         | `ram`                   | ROM_LOW + ROM_HIGH OFF |
| `rom_low_off`     | -                       | ROM low OFF (MZ-800)  |
| `rom_high_off`    | -                       | ROM high OFF (MZ-800) |
| `cgrom`           | -                       | CGROM viditelný       |
| `vram_640`        | `vram`, `vram_low`      | SCRW640 / PCG_1       |
| `pcg_high`        | `pcg_1`, `pcg_2`, `pcg_3` | MZ-1500 SPEC=2/3/4  |
| `other`           | -                       | Neznámá konfigurace   |

Lookup je case-insensitive. Neznámé jméno = parse error.

Příklady:

```
cat:irq_ack_im2 if iff1:1
   # IRQ ack eventy které proběhly při IFF1 ON (= legitimní ack)
cat:cpu_int if reason:int_ack
   # CPU_INT eventy klasifikované jako INT acknowledgement
if banking:all_ram
   # vše co se dělo při plně RAM mapping
( cat:gdg_colors if im:2 )
   # palette changes prováděné v IM2 módu
```

### Temporal atomy

Temporal node obalí pod-výraz a vyhledá v ringu eventy v daném okně
pxclk kolem každého **reference** match-u sub-výrazu.

| Token             | Význam                                              | Příklad                            |
|-------------------|-----------------------------------------------------|------------------------------------|
| `before(N) <expr>` | eventy v posledních N pxclk před match-em `<expr>`  | `before(1000) cat:irq_ack_im2`     |
| `after(M) <expr>`  | eventy v M pxclk po match-u `<expr>`                | `after(500) cat:cpu_ctrl sub:reti` |
| `near(K) <expr>`   | eventy v okně +-K pxclk kolem match-u `<expr>`      | `near(200) sym:isr_main`           |

Argumenty `N` / `M` / `K` jsou v pxclk units, suffixy:

| Suffix | Násobitel |
|--------|-----------|
| (žádný) | x1        |
| `k`     | x1000     |
| `M`     | x10^6     |

Max nesting depth temporal nodes = **2** úrovně (= `before(1000) (cat:X
AND sym:Y)` OK, hlubší vnoření = parse error).

**Pozor**: temporal filtry vyžadují stabilní ring snapshot. Jsou
dostupné **jen v UI Log tab a Strip tab filterech**. V
**pause-on-match a auto-mark trigger** filtrech je temporal node
**zablokovaný** (= UI ho odmítne s warning hláškou), protože hot-path
hook eval probíhá v EMU vlákně bez stabilního snapshotu ringu.

Příklady:

```
before(2000) cat:irq_ack_im2
   # cokoliv co se dělo v posledních 2000 pxclk před IM2 ack
near(500) sym:print_char
   # eventy v +-500 pxclk kolem volání print_char
after(10k) cat:gdg_mode sub:mode_change
   # cokoliv v 10000 pxclk po DMD mode switch
before(1k) ( cat:cpu_int and if reason:int_ack )
   # context před každým INT ack v posledním 1000 pxclk
```

#### Performance temporal

Match s temporal nodem iteruje per event přes celý ring scope -
celkové N x R operací per render (R = počet reference matches). Pro
typický scénář (N=50000, R~100) je render scan 5-15 ms per frame. Pro
extreme load (R=10000+) se UI cítí lehce trhaně - doporučení: zužte
sub-expr filterem na konkrétní kategorii.

### Příklady

```
cat:cpu_int                              # jen CPU_INT eventy
cat:cpu_int pc:4000-40FF                 # CPU_INT a PC v range (AND)
( cat:cpu_int or cat:bp_fire )           # CPU_INT nebo BP_FIRE
!cat:gdg_video                           # vše KROMĚ GDG_VIDEO (= "Hide noise")
frame:>100 cycle:>1M                     # po 100. snímku a 1 mil. cyklu
cat:iorq_out payload:0xCE                # OUT na GDG MODE port
( cat:bp_fire or cat:user_mark ) sub:1   # BP fire (sub=MARK) nebo USER_MARK
```

### Quick filter presety v toolbaru

ComboBox v toolbaru s **10 přednastavenými filtry** (= predefined, klik
nasadí expression do Filter textbox). Pro user-definovaný seznam viz
[Saved filter presety](#saved-filter-presety).

| Skupina | Preset | Expression |
|---|---|---|
| HW Events | Only IRQ | `cat:cpu_int,cpu_pin_edge,irq_ack_im2` |
| | Only banking | `cat:gdg_banking,memext` |
| | Only video | `cat:gdg_mode,gdg_hwscroll,gdg_colors,gdg_video,gdg_wfrf` |
| | Only PSG | `cat:psg` |
| | Only FDC | `cat:fdc` |
| | Only memory | `cat:mmio_r,mmio_w` |
| | Only SYS | `cat:sys` |
| Code scope | Only marks/BPs | `cat:user_mark,bp_fire` |
| | ISR scope (PC 0038-00FF) | `pc:38-FF` |
| Hide/reset | Hide noise | `!cat:gdg_video` |
| | Clear filter | (empty) |

## Pause-on-match (stream BP)

V toolbaru je sekce **"Pause on match"** - filter expression funguje
jako stream-level breakpoint nad eventlog ringem. První event, který
padne na filter, pauzne emu (= `Pause` button equivalent).

### Toolbar UI

```
[ ] Pause on match: [filter expression......................] [Test parse: OK]
                    Last match: frame=42 pxclk=18432   [Go to match]
```

- **Checkbox** vlevo aktivuje trigger. Off = žádný overhead.
- **Filter expression textbox** - stejná syntax jako hlavní filter.
- **Test parse badge** - "OK" / "Syntax error: ..." validuje filter bez
  Apply. Zelený OK = filter připraven.
- **Last match indicator** - pokud trigger už padl, ukazuje
  `frame=N pxclk=M`. Klik = scroll Log na ten event a select.

### Use cases

| Filter expression                       | Co pauzne                                |
|-----------------------------------------|------------------------------------------|
| `cat:gdg_colors`                        | první palette write (BORDER nebo PAL)    |
| `cat:bp_fire sub:0`                     | první classic BP halt (sub=HALT)         |
| `pc:38-FF`                              | první vstup do IM 1 ISR (vector 0x38)    |
| `cat:psg payload:0x80`                  | první PSG latch na ch A period lo        |
| `cat:iorq_out payload:0xCE`             | první OUT na GDG DMD port                |
| `sym:isr_*`                             | první PC v libovolném ISR rutině         |

### One-shot semantika

Po prvním match se trigger **automaticky vypne** (= checkbox zůstane
checked, ale internal gate je clear). Pro další match je potřeba znovu
aktivovat (= odznačit a znovu označit checkbox, nebo klik na "Re-arm").

Pause je async - emu doběhne do dalšího "safe" bodu (= konec aktuální
instrukce) a teprve potom skutečně zastaví. V UI to není znát (= halt
proběhne v desítkách mikrosekund max).

## Auto-mark on match

Druhá toolbar sekce vedle Pause-on-match. Místo pauzování emu
vygeneruje synthetic **USER_MARK** event s nastaveným jménem - každý
matching event tedy zanechá viditelnou stopu v ringu.

### Toolbar UI

```
[ ] Auto-mark on match: 'name......'  expr: [filter.....................]  Marker ID: 5
                                       Marked: 23                          [Clear]
```

- **Checkbox** aktivuje trigger. Off = žádný overhead.
- **Name textbox** - libovolný string (typicky krátký 4-12 znaků).
  Sdílí marker registry s BP DSL action `mark "name"` - stejné jméno =
  stejný marker_id.
- **Filter expression** - jako u pause-on-match.
- **Marker ID** - allocated ID po prvním record (lazy register).
- **Counter** - kolik USER_MARK eventů auto-mark vygeneroval.
- **Clear** - reset counter.

### Use cases

| Name        | Filter expression               | Co označí                      |
|-------------|---------------------------------|--------------------------------|
| `psg_w`     | `cat:psg`                       | každý PSG write                |
| `bord_chg`  | `cat:gdg_colors sub:1`          | každá změna BORDER             |
| `dmd_chg`   | `cat:gdg_mode`                  | každá změna DMD (= mode switch)|
| `isr_ent`   | `cat:irq_ack_im2`               | každý vstup do IM 2 ISR        |
| `mem_w`     | `cat:mmio_w pc:E000-FFFF`       | MMIO writes v horním banku     |

Marker_id je stable per name napříč session - opakované zapnutí
auto-mark se stejným name použije stejné ID.

### Synthetic USER_MARK eventy

Generated eventy mají:

- `category = USER_MARK` (= 19)
- `subtype = 0` (= regular mark)
- `payload` = encoded marker_id
- `pc` = původní `pc` triggrového eventu (= kde se to stalo)
- `frame` / `pxclk` = původní timestamp

### Žádný infinite loop

Auto-mark callback explicitně skipuje eventy s `category == USER_MARK` -
vlastní synthetic marky tedy netriggrují další auto-mark (= jednosměrný
flow).

## Detail sloupec dekódéry

Detail sloupec ukazuje human-readable text formátovaný per kategorie.

| Kategorie    | Detail příklad                                        |
|--------------|-------------------------------------------------------|
| CPU_INT      | `IM=2 IFF1=1 IFF2=1 RETI`                             |
| CPU_PIN_EDGE | `PIOZ80@PA4 rising`                                   |
| IRQ_ACK_IM2  | `PIOZ80_PA vec=0x40 isr=0x4042`                       |
| IORQ_IN/OUT  | `port=0xCE (GDG DMD) val=0x08`                        |
| MMIO_R / W   | `addr=0xE003 val=0x80`                                |
| GDG_MODE     | `DMD=0x08 (MZ-700 mode)`                               |
| GDG_BANKING  | `port 0xE0 (ROM bottom OFF)`                          |
| GDG_HWSCROLL | `SOF=0x4000` / `WID=80` / ...                         |
| GDG_COLORS   | `BORDER=0xE0` / `PAL[2]=0x9F`                         |
| GDG_VIDEO    | `VBLN start` (= jen subtype label)                    |
| GDG_WFRF     | `WF=0x08` / `RF=0xFF`                                 |
| PIO8255      | `Port A=0x42` / `CW=0x80 (mode 0)`                    |
| CTC8253      | `CW: counter 2 mode 3 BIN` / `CTC1=0x12`              |
| PIOZ80       | `A MODE 2 (bidir)` / `A ICW 0xB7 ...`                 |
| PSG          | `0x80 (latch ch A period lo)`                         |
| FDC REG_W    | `reg=0 (CMD)=0x80 (Restore)`                          |
| FDC CMD      | `Read Sector side=0 T=10 S=5`                         |
| MEMEXT       | `page=4 bank=0x12 (Luftner)`                          |
| QD           | `reg=2 (ctrl A)=0x18`                                 |
| RD           | `port=0xFA val=0x42`                                  |
| BP_FIRE      | `BP #5 reason=2 MARK`                                 |
| USER_MARK    | `"isr_entry"`                                         |
| CPU_CTRL     | `HALT enter` / `RST 0x38`                             |
| SYS          | `Snapshot save: hash=0xNNNNNNNN`                      |

Decoder neopouští informace z 24 B eventu - pohled nezáleží na pořadí
čtení a aktuálním emu state. Tooltip nad buňkou ukáže ten samý text
(= žádné dodatečné informace v tooltipu).

## Sub sloupec short codes

Sloupec `Sub` zobrazuje **krátký kód** (max 8 viditelných znaků) per
(kategorie, subtype). Hover nad buňkou ukáže **plný popis** v tooltipu.

### Příklady short codes

| Kategorie    | Subtype           | Short code   | Tooltip (plný)              |
|--------------|-------------------|--------------|------------------------------|
| CPU_CTRL     | HALT_ENTER (0)    | `HALT_E`     | `HALT entry`                |
| CPU_CTRL     | HALT_EXIT (1)     | `HALT_X`     | `HALT exit (IRQ/NMI)`       |
| CPU_CTRL     | RST_00 (2)        | `RST_00`     | `RST 0x00`                  |
| CPU_CTRL     | RST_38 (9)        | `RST_38`     | `RST 0x38`                  |
| BP_FIRE      | HALT (0)          | `HALT`       | `BP fire - halt`            |
| BP_FIRE      | MARK (1)          | `MARK`       | `BP fire - mark action`     |
| BP_FIRE      | CONTINUE (2)      | `CONT`       | `BP fire - continue`        |
| BP_FIRE      | ENABLE (4)        | `ENABLE`     | `BP fire - enable target`   |
| BP_FIRE      | DISABLE (5)       | `DISABL`     | `BP fire - disable target`  |
| IORQ_IN/OUT  | NORMAL (0)        | `NORMAL`     | `Mapovaný port`             |
| IORQ_IN/OUT  | UNCONNECTED (1)   | `UNCONN`     | `Neobsazený port`           |
| GDG_COLORS   | BORDER (1)        | `BORDER`     | `Border color write`        |
| GDG_COLORS   | PALGRP (2)        | `PALGRP`     | `Palette group write`       |
| GDG_COLORS   | PAL (3)           | `PAL`        | `Palette register write`    |
| GDG_COLORS   | PCG (4)           | `PCG`        | `PCG color write (MZ-1500)` |
| GDG_VIDEO    | VBLN_START (1)    | `VBLNs`      | `Vertical blanking start`   |
| GDG_VIDEO    | VBLN_END (2)      | `VBLNe`      | `Vertical blanking end`     |
| GDG_VIDEO    | HBLN_START (5)    | `HBLNs`      | `Horizontal blanking start` |
| PIO8255      | PORT_A (1)        | `PORT_A`     | `Port A write`              |
| PIO8255      | PORT_B (2)        | `PORT_B`     | `Port B write`              |
| PIO8255      | CONTROL_W (4)     | `CTL_W`      | `Control register write`    |
| CTC8253      | CONTROL_WRITE (1) | `CTL_W`      | `Control register write`    |
| CTC8253      | COUNTER_WRITE (2) | `CNT_W`      | `Counter register write`    |
| PIOZ80       | MODE_W (1)        | `MODE_W`     | `Mode register write`       |
| PIOZ80       | VECTOR_W (2)      | `VEC_W`      | `Vector register write`     |
| PIOZ80       | ICW_W (3)         | `ICW_W`      | `Interrupt control write`   |
| FDC          | REGISTER_WRITE (1) | `REG_W`     | `WD279x FDC register write` |
| FDC          | COMMAND_ISSUED (2) | `CMD`       | `WD279x command dispatch`   |
| SYS          | COLD_RESET (0)    | `COLD_RST`  | `Cold reset`                |
| SYS          | SNAPSHOT_SAVE (2) | `SNAP_SAV`  | `Snapshot save`             |
| SYS          | SNAPSHOT_LOAD (3) | `SNAP_LD`   | `Snapshot load`             |
| SYS          | MZF_INJECT (4)    | `MZF_IN`    | `MZF inject`                |

Pro neznámou kombinaci kategorie+subtype fallback je decimální zápis
(= `"42"`). Tooltip pak vrátí `"short (subtype N)"` fallback string.

## Group by v Log tabu

V toolbaru Log tabu je dropdown **"Group by"** s hodnotami:

| Hodnota   | Význam                                              |
|-----------|-----------------------------------------------------|
| `None`    | Chronologická tabulka (= default)                   |
| `Frame`   | Per-frame skupiny (= group klíč `screens_total`)    |
| `Category` | Per-kategorie skupiny (= klíč `category`)           |
| `PC`      | Per-PC skupiny (= klíč `pc`)                        |

Při ne-None hodnotě se Log render přepne na **CollapsingHeader per
skupinu** s vlastní mini-tabulkou uvnitř. Header text obsahuje group
key + počet eventů (např. `Frame 142 (45 events)`).

V mini-tabulce uvnitř skupiny se **vypustí sloupec** odpovídající
group klíči (= duplicitní s header textem):

- `Frame` group -> skryje sloupec **Frame**
- `Category` group -> skryje sloupec **Cat**
- `PC` group -> skryje sloupec **PC**

Klik na řádek funguje stejně jako v None módu (= pause + skok do
disasm). Filter expression se aplikuje **před** groupováním.

Stav dropdownu persistuje v cfg `[EVENT_VIEWER_WINDOW] group_by = 0..3`.

**Performance**: Pre-pass O(N) + render. Pro N=50000 a typický počet
groups (300 pro Frame, 24 pro Category) render ~5-8 ms per frame. Group
by PC s chaotickým trace (G=10000+) se může cítit pomalejší (8-15 ms).

## Heatmapa per-frame

Toggle **"Heatmap"** v toolbaru zobrazí nad Log tabulkou mini histogram
distribuce eventů v rámci snímku. Default OFF.

- **64 binů** rovnoměrně po pxclk doméně snímku (= per-arch frame width)
- Výška sloupce = celkový počet eventů v binu (= self-normalizing, max
  bin = full height)
- Color stack per kategorie (= reuse Strip color picker barev)
- Hover tooltip nad sloupcem ukáže bin range + top kategorie v binu
  (např. `Bin 23 (pxclk 25800..26900): total 145 events, CPU_INT:80
  IORQ_OUT:45 ...`)
- **Klik bin** = scroll Log tabulky na první event v daném binu

Filter expression se aplikuje **před** binning - heatmapa ukazuje
distribuci filtrovaných eventů.

Stav toggle persistuje v cfg `[EVENT_VIEWER_WINDOW] heatmap = 0..1`.

## Export / Import ringu

Pro post-mortem analýzu je možné aktuální obsah ringu vyexportovat do
binárního souboru a později ho re-importovat (= "replay").

### UI toolbar

V toolbaru Events okna mezi **Capacity Clear** a **Follow tail** jsou
dvě tlačítka:

- **Export** otevře modal popup s text inputem pro filename (default
  `eventlog_dump.evlog`) + Confirm tlačítko. Po Confirm zapíše ring do
  souboru.
- **Import** otevře modal popup s text inputem pro filename + Confirm.
  Po Confirm volá **Confirm dialog** "Replace current ring?" - po
  potvrzení vyčistí ring a načte data ze souboru.

Pro deterministický export se doporučuje:

1. Pauzovat emu (Settings -> Pause) nebo přepnout Mode -> OFF
2. Teprve pak Export

(= jinak může emu zapisovat do ringu paralelně s exportem a poslední
event může být ztracen nebo extra.)

### CLI replay

Z příkazové řádky lze ring naimportovat při startu emu pomocí flagu
`--eventlog-replay`:

```
mz800emu --eventlog-replay /path/to/dump.evlog
```

Soubor se načte po inicializaci debugger subsystému, ring má pak obsah
z dumpu. UI Log tab okamžitě po otevření okna zobrazí naimportované
eventy.

### Binární formát

Hlavička 32 B (magic "MZEVTLOG" + version + record_size + record_count
+ unix timestamp) + sekvence records (= 32 B each).

**Endianness**: native (= little-endian na podporovaných platformách
Win MSYS2 / Linux x86_64 / ARM64). Soubory NEJSOU přenositelné mezi
big-endian a little-endian systémy.

**Capacity auto-resize**: pokud soubor obsahuje víc records než aktuální
ring capacity, import zvětší capacity na max(record_count,
MIN_CAPACITY), clamped na MAX_CAPACITY = 200000. Records nad limit se
zahodí (= warning ve stderr, ne error).

## FDC command tracking

FDC kategorie emituje při každém zápisu na WD279x command/status
register **dvojici eventů** na stejné pxclk pozici:

1. **REG_W** subtype = raw BUS byte zápis (= před Sharp inverzí).
   Detail string ukáže `reg=0 (CMD)=0x80 (Read Sector)`. Užitečné pro
   ověření fyzické úrovně bytu na sběrnici.
2. **CMD** subtype = **dekódovaný command** s aktuální polohou hlavy
   (Track / Sector registr před dispatchem). Detail string ukáže např.
   `Read Sector side=0 T=10 S=5`.

Zápis na ostatní FDC registry (Track / Sector / Data) emituje pouze
REG_W.

Filter `cat:fdc sub:cmd` ukáže jen dekódované commandy. Filter
`cat:fdc sub:reg_w` jen raw zápisy.

Strip tab tooltip obsahuje plný subtype text (= reuse stejných stringů,
které ukazuje Log tab hover na Sub buňce). Tooltip pak ukáže např.:

```
frame=42 pxclk=18432
sline=128 px=160
CTC8253 / CTL_W (Control register write)
CW: counter 2 mode 3 BIN
```

## SYS kategorie

Kategorie `cat:sys` zachycuje lifecycle eventy emulátoru, které nepatří
do žádného HW subsystému.

| Subtype           | Short kód   | Detail string                       |
|-------------------|-------------|-------------------------------------|
| COLD_RESET        | `COLD_RST`  | `Cold reset`                        |
| SNAPSHOT_SAVE     | `SNAP_SAV`  | `Snapshot save: hash=0xNNNNNNNN`    |
| SNAPSHOT_LOAD     | `SNAP_LD`   | `Snapshot load: hash=0xNNNNNNNN`    |
| MZF_INJECT        | `MZF_IN`    | `MZF inject: hash=0xNNNNNNNN`       |

Filename hash je djb2 z **basename** (= za posledním `/` nebo `\`), ne
plné cesty. Stejný soubor z jiného adresáře dá stejný hash.

**Quick filter preset** "Only SYS" je v Strip Quick Filter dropdown.
Strip render barva = šedo-zlatá pro kontrast vůči ostatním kategoriím.
SYS markery jsou rare (= 1-10 per session), zobrazují se jako thicker
vertical bars pro viditelnost.

## Interakce - klik a context menu

### Single click na řádek

Označí řádek (highlight). Žádná další akce.

### Double click na řádek

**Pause emu + show in disasm** na PC eventu. Klíčové online UX: "vidím
něco divného -> klik -> vidím to v kódu".

### Right click na řádek

Context menu:

- **Pause and show in disasm** - identicky s double click
- **Pause here** - jen pause, žádný skok
- **Show in disasm** - skok bez pause
- **Show port in Overview** (jen pro IORQ_IN / IORQ_OUT / MMIO_R /
  MMIO_W) - otevře I/O Ports Overview tab + nastaví filter na konkrétní
  port
- **Copy as text** - schránka

## Follow tail

Toolbar toggle. Když je zapnutý, tabulka automaticky scrolluje na
nejnovější řádek (= "live tail" jako `tail -f`).

**Interakce s click-pause:**

- Manuální scroll uživatelem (= scrollbar drag, kolečko myši) dočasně
  potlačí auto-scroll do dalšího renderu, kdy je tabulka na konci.
- Click + pause emu zachová pozici scrollu - po resume se auto-scroll
  spustí znovu.

Default = ON. Persistované v cfg `[EVENT_VIEWER_WINDOW] follow_tail`.

## Persistence (cfg sekce)

### `[EVENT_LOG]`

| Klíč               | Default        | Význam                             |
|--------------------|----------------|------------------------------------|
| `mode`             | `OFF`          | OFF / WHEN_WINDOW_OPEN / ALWAYS    |
| `capacity`         | 50000          | Velikost ringu (clamp 10000..200000) |
| `categories_mask`  | `0x00FFFFFF`   | Bit per kategorie (= 24 ON)        |

### `[EVENT_VIEWER_WINDOW]`

| Klíč                | Default | Význam                                       |
|---------------------|---------|----------------------------------------------|
| `window_open`       | 0       | Visibilita Events okna                       |
| `filter_expression` | `""`    | Poslední filter v textboxu                   |
| `follow_tail`       | 1       | Auto-scroll na nejnovější                    |
| `group_by`          | 0       | Group by (0=None / 1=Frame / 2=Cat / 3=PC)   |
| `heatmap`           | 0       | Heatmapa nad Log tabulkou (0=OFF / 1=ON)     |
| `row_coloring`      | 1       | Barevné pozadí řádků podle kategorie         |

### `[EVENT_LOG_FILTERS]`

Sloty `preset_NN_name` + `preset_NN_expr` (NN = 00..31), prázdné name
= slot ignorován. Viz [Saved filter presety](#saved-filter-presety).

## Související panely

- **I/O Ports** ([io-ports.md](io-ports.md)) - History tab v I/O Ports
  má vlastní in-memory ring jen pro IORQ + MMIO eventy (~10000 eventů,
  20 B per event), s vlastním filterem a starší než Event Viewer. Pro
  real-time pohled napříč všemi kategoriemi (= včetně CPU_INT, GDG, BP
  fire) použij Events okno. Pro úzce port-focused workflow s
  "Add IORQ R/W BP" akcí použij I/O Ports History tab.

- **Trace Suite** ([Trace_Suite.md](Trace_Suite.md)) - post-mortem
  souborový log (cputrack / iorqlog / intlog / hwlog / marklog).
  Eventlog ring sdílí 24 B per-event layout (= cross-mergovatelné
  timestampy s hwlog/intlog chunky).

- **Breakpoints** ([breakpoints/README.md](breakpoints/README.md)) -
  každé fíření BP generuje `BP_FIRE` event v Event Vieweru včetně
  klasifikace akce (HALT / MARK / CONTINUE / ENABLE / DISABLE). Klíčové
  pro "kdy se mi BP odpaluje" debug.

- **Disassembly** ([disassembly.md](disassembly.md)) - cíl skoku z
  Events okna (= double click / context menu "Show in disasm").

- **Stack window**, **CPU window**, **Memory map** - související debug
  view bez přímé interakce s Events oknem.
