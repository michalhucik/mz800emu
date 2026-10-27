# Memory Browser - Layers, Regions tree, Multi-view

## 1. Co je to a k čemu

Memory Browser má kromě hlavního hex view tři rozšiřující subsystémy,
které umožňují inspekci paměti nad rámec prostého bajtového dumpu:

- **Layers panel** (vpravo) - vrstvové vizualizace přes hex buňky
  (CDL, heatmap, snapshot Δ, frozen) + symboly a bookmarky v ASCII
  sloupci
- **Regions tree sidebar** (vlevo) - hierarchický strom paměťových
  oblastí per architektura, s ohledem na connected/disconnected HW
- **Multi-instance** - 5 nezávislých Memory Browser oken pro
  paralelní pohled na různé oblasti

Layers a Regions oba mají kolapsovatelné sidebars oddělené splitterem,
takže si ve výsledku každý uživatel rozmístí prostor podle potřeby.


## 2. Layers panel

Sidebar vpravo od hex view. Lze kolabovat a šířku měnit splitterem.
Obsahuje 6 vzájemně nezávislých přepínatelných vrstev (toggle ON/OFF).
Default je všechny **OFF**, stav persistuje per instance v INI.

### Přehled vrstev

| Vrstva | Vizualizace | Zdroj dat |
|--------|-------------|-----------|
| **CDL Code (X)** | světle modré pozadí buňky | Code/Data Logger - byte vykonán jako instrukce |
| **CDL Data Read (R)** | světle zelené pozadí buňky | CDL - byte přečten daty (LD A,(HL) apod.) |
| **CDL Data Write (W)** | světle červené pozadí buňky | CDL - byte zapsán daty |
| **Heatmap** | gradient cool->hot | access count s log normalizací (= chladné modré pro málo, horké červené pro hodně) |
| **Snapshot Δ** | magenta highlight | změněné byty od poslední manuální snapshot |
| **Frozen bytes** | tmavě fialové pozadí | byty zafrozené přes edit-tools ("lock byte" cheat) |

CDL vrstvy se mohou kombinovat - byte který byl jak instrukce tak data
(typický u SMC kódu) bude mít smíchané pozadí podle Z-order vykreslení.

### Heatmap normalizace

Heatmap používá log škálu, protože raw access count má v praxi řády
rozdílu (frekvenčně volaná rutina vs. inicializační kód). Bez log škály
by hot region drtil veškerý jiný signál na nulu.

### Snapshot Δ workflow

1. Klikni **Manual Snapshot** v Layers panelu - emulátor uloží
   baseline kopii aktuálního obsahu regionu
2. Pokračuj v emulaci / udělej akci, kterou chceš sledovat
3. Zapni **Snapshot Δ** vrstvu - magenta highlight označí byty,
   které se od baseline změnily

### Symbol overlay v ASCII column

Pokud sym_db obsahuje symbol odpovídající adrese řádku, ASCII sloupec
před vlastním textem ASCII zobrazí jméno symbolu žlutě (TextColored).
Hover nad symbolem ukáže tooltip s:

- **name** - identifikátor symbolu
- **kind** - label / map / NoICE / sjasmplus / bookmark
- **comment** - poznámka ze symbolu, pokud existuje

### Bookmark add/remove

Right-click na řádek v hex view -> kontextové menu **Add bookmark** /
**Remove bookmark**. Bookmark se uloží do sym_db jako záznam s kindem
**BOOKMARK**. V 1-znakovém mark sloupci se zobrazí marker `*`.

Bookmarky jsou sdílené napříč všemi Memory Browser instancemi (= sym_db
je globální).


## 3. Regions tree sidebar (V2)

Sidebar vlevo od hex view. Také kolapsovatelný a splitter-resizable.
Obsahuje hierarchický strom paměťových oblastí dostupných v aktuální
architektuře. Connected HW je zobrazený plnou barvou, disconnected
greyed out (= viditelné jako placeholder, ale nelze do něj přepnout).

### Stromová struktura (per arch)

Strom respektuje rozdíly mezi mz800, mz700 a mz1500:

- **Logical Z80** (64 KB) - to, co CPU vidí přes aktuální banking
- **User RAM** (64 KB) - bare RAM bez ROM overlay
- **Monitor ROM** - lower 4 KB + upper 8 KB
- **CG-ROM** (4 KB) - znakový generátor
- **VRAM**
  - Physical Planes I / II / III / IV (8 KB každý, MZ-800;
    III a IV jen pokud je exVRAM connected)
  - VRAM 700 char + attr (2 KB každý, jen v 700 / 700-compat módu)
- **CG-RAM** (4 KB, jen v 700 módu)
- **PCG bank 1 / 2 / 3** (8 KB každá, jen MZ-1500)
- **Memext** (Luftner / PEHU, jen pokud connected)
  - RAM bank 0x00 .. 0x7F (po 4 KB; 8 KB banka PEHU N je zobrazená jako dvě
    4 KB části 2*N a 2*N + 1)
  - FLASH bank 0x00 .. 0xN (Luftner, read-only)
- **Ramdisk STD** (pokud connected, počet bank podle typu)
- **Ramdisk PEZIK port 0x68** + **port 0xE8** (pokud connected)

### Per-row physical origin label

V **Logical Z80** view (= co CPU vidí přes banking) se za ASCII
sloupcem volitelně zobrazí TextDisabled label původu řádku:

```
... ASCII ...  | RAM
... ASCII ...  | ROM lower
... ASCII ...  | Memext bank 0x4A
```

Toggle ovládá `show_origin_labels`. Při ladění banking situací
(= "proč CPU čte z této adresy zrovna tohle") je tento label klíčový.

### Banking status bar (per arch)

Spodek okna obsahuje banking status bar, jehož obsah závisí na
architektuře:

| Arch | Indikátory |
|------|------------|
| **MZ-800** | `[PROHIBITED]`, `[SCRW640]`, `[HICOLOR]`, `[VBANK]`, **DMD: MZ-800/700 mode (0xXX)** |
| **MZ-700** | `[PROHIBITED]` (pokud aktivní) |
| **MZ-1500** | (nic, MZ-1500 nemá ekvivalentní banking flags) |

PROHIBITED značí stav, kdy ROM monitor write-protectoval RAM oblast
přes hardwarový mechanismus.

### Cross-window "Show in Memory Map"

Right-click v hex view -> **Show in Memory Map**. Otevře okno
**Memory Map** a focusne ho na 4 KB stránku odpovídající aktuální
adrese kurzoru. Užitečné při kombinaci Memory Browser + Memory Map
pro pochopení banking state.

### PEZIK byte order

U PEZIK ramdisku se zobrazí informativní indikátor byte order (BE/LE).
**Pozor**: toggle byte order v emulátoru zatím **chybí** - indikátor
je jen pro information, BE/LE převod si musí uživatel dělat v hlavě.


## 4. Multi-instance (V3)

Memory Browser je k dispozici v **5 nezávislých instancích**
(`MB_INSTANCE_COUNT = 5`):

- **main** - hlavní okno, otevírá se default z menu Debugger
- **#2 .. #5** - sekundární okna, otevírají se z menu
  **Debugger -> Other memory browsers -> Memory Browser #N**

### Per-instance state izolace

Každá instance má vlastní nezávislý stav pro:

- aktuální region (= co se zobrazuje)
- encoding (= jak interpretovat ASCII sloupec)
- pozice kurzoru, scroll, selection
- layers (= které vrstvy ON/OFF)
- splitter pozice (regions sidebar šířka, layers sidebar šířka)
- show_origin_labels toggle

### Sdílené napříč instancemi

Některé položky jsou logicky globální (= ne per-instance):

- **freeze bytes** - jeden globální seznam zafrozených bytů
- **bookmarks** - sym_db je jeden pro celý emulátor
- **annotations** - poznámky k bytům

Když přidáš bookmark v main instanci, objeví se okamžitě v #2..#5 too.

### Stable IDs

ImGui okna používají three-hash stable ID konvenci, aby title bar mohl
měnit dynamicky bez ztráty docking/position state:

```
"Memory Browser###mb_main"
"Memory Browser #2###mb_2"
"Memory Browser #3###mb_3"
"Memory Browser #4###mb_4"
"Memory Browser #5###mb_5"
```

### Persistence

Per-instance state se ukládá do cfgmain INI v separátních sekcích:

- `[MEMBROWSER_WINDOW_MAIN]`
- `[MEMBROWSER_WINDOW_2]` .. `[MEMBROWSER_WINDOW_5]`

### DBG Workplace integration

Každá instance má vlastní toggle ve **DBG Workplace** systému, takže
ji lze zapínat / vypínat s celou workplace konfigurací (= např.
"profiling workplace" má jen main + #2, "memory debug workplace" má
všech 5).


## 5. Typické use cases

### Najít všechen kód v Logical Z80

1. Region: **Logical Z80**
2. Zapni vrstvu **CDL Code (X)** ON
3. Scrolluj hex view - světle modré pozadí ukazuje, co Z80 vykonal
   jako instrukce
4. **Tip**: spárovat s **Heatmap** pro vizuální "hot paths" (= často
   vykonávané instrukce budou současně modré + horké)

### Sledování paměťových změn během gameplay

1. Region: **User RAM** (= banking-aware "co CPU vidí jako RAM")
2. **Pause** emulátoru
3. Klikni **Snapshot** v Layers panelu (baseline)
4. **Resume** + udělej krátkou herní akci (= jedna akce na sledování)
5. **Pause** + zapni vrstvu **Snapshot Δ** ON
6. Magenta highlight ukáže byty, které se změnily

### Cheat lock (freeze HP)

1. Najdi byte s HP (search v Memory Browser nebo manual scroll)
2. Right-click -> **Freeze byte at cursor (= 0xXX)**
3. Emulátor každý frame přepíše hodnotu zpět na zafrozený obsah
4. Zapni vrstvu **Frozen bytes** ON pro vizuální badge na zafrozených
   adresách

### Multi-window porovnání 2 oblastí

1. Otevři **main** Memory Browser
2. Otevři **#2** přes Menu Debugger -> Other memory browsers ->
   Memory Browser #2
3. Main přepni na region X, #2 na region Y
4. Roztahej okna v ImGui side-by-side pro paralelní pohled


## 6. Související

- [hex-view](hex-view.md) - core hex viewer + edit
- [search](search.md) - search engine
- [edit-tools](edit-tools.md) - undo/redo, fill, annotations, freeze
- [Memory Map](../memory-map.md) - banking + memext per stránku
- [Symbols](../symbols.md) - symbol browser (sym_db)
- [Bookmarks](../bookmarks.md) - bookmark editor
