# CPU Profiler - per-funkce agregace T-states

## 1. Co je to a k čemu

**CPU Profiler** je samostatné dokovatelné ImGui okno, které agreguje
T-states (= takty Z80) per funkce. Pro každý unikátní cíl volání
(CALL / RST / IRQ accept / NMI) drží:

- **Calls** - počet vstupů
- **Inclusive cycles** - součet T-states včetně času stráveného ve vnořených
  voláních
- **Exclusive cycles** - součet T-states bez času vnořených volání
  (= "vlastní" práce funkce)
- **Min/Max inclusive** - nejkratší a nejdelší jeden call

Profiler odpovídá na otázky typu "ve které rutině trávím nejvíc času",
"kolikrát se ta rutina volá za frame", "jak dlouho běží IRQ handler".

Na záložce Functions není granularita per instrukce, ale per CALL (= per entry
adresa funkce). Čas po jednotlivých instrukcích ukáže záložka **Per PC**
(sekce 10). Pro analýzu jednotlivých scanline rutin použij sibling
okno **Callstack** + **Stack Monitor**.

Profiler je postavený jako listener nad callstack subsystémem, tedy
automaticky zdědí všechny záběry callstacku (CALL/RST/IRQ accept/RETI),
ale i jeho limity (= multi-stack OS, banking, viz sekce 6).


## 2. Aktivace subsystému

Profiler je **vypnutý** při startu emulátoru. Zapnout lze:

| Cesta | Co se stane |
|---|---|
| **INI klíč** `[PROFILER] active=1` | Subsystém aktivován při startu, persistuje přes save/load INI |
| **CLI flag** `--profiler` | Force ON nad INI hodnotu při startu |
| **Menu** Debug -> CPU Profiler | Otevře okno a aktivuje subsystém |
| **Klávesa** Alt+Shift+P | Toggle CPU Profiler okno |
| **UI Start tlačítko** v toolbaru | Start měření za běhu emulátoru |

Profiler je listener nad callstack subsystémem. Zapnutí profileru
automaticky vynutí zapnutí callstacku (= callstack se zapne i když ho
uživatel explicitně nezapnul). Při deaktivaci profileru callstack:

- vrátí zpět do **OFF**, pokud ho zapnul profiler (= ownership)
- ponechá **ON**, pokud ho měl uživatel zapnutý už před zapnutím profileru

Tím se profiler chová předvídatelně - žádné nečekané "callstack stále
běží" po Stop Profileru.


## 3. Workflow

Typický průchod:

1. **Načti symboly** (volitelné, ale doporučené) - File -> Load symbols,
   formáty `.sym` / `.lbl` / `.map` / NoICE. Profiler ukáže jména
   funkcí místo `func_C123`.
2. **Otevři okno** přes menu Debug -> CPU Profiler, nebo přes
   Alt+Shift+P.
3. **Klikni Start** v toolbaru. Status bar přejde do `RUNNING`.
4. **V emulátoru spusť program** nebo herní úroveň, kterou chceš
   profilovat. Cycles se průběžně sčítají.
5. **Klikni Stop**. Status bar přejde do `STOPPED`. Data v tabulce
   zůstávají k dispozici (= Stop nezahodí agregátor).
6. **Prohlédni tabulku**. Sort podle Excl% / Calls / Max - klikem na
   hlavičku sloupce.
7. **Export CSV** (volitelně) - pro post-process v Excelu / Python.

**Reset** explicitně vymaže agregátor. Bez Reset Profiler kumuluje
data od posledního Start (= další Start bez Reset pokračuje v
agregaci).


## 4. Metriky a sloupce tabulky

| Sloupec | Význam |
|---|---|
| **Function** | Symbol z databáze (NoICE / .map / .sym / .lbl), jinak `func_XXXX` |
| **Address** | Vstupní adresa funkce (CALL target) v hex |
| **Kind** | CALL / RST / IRQ-IM0 / IRQ-IM1 / IRQ-IM2 / NMI |
| **Calls** | Počet párovaných on_enter eventů |
| **Excl%** | Exclusive cycles jako % z celkového exclusive součtu všech entry |
| **Excl** | Exclusive cycles (= bez vnořených volání) |
| **Incl%** | Inclusive cycles jako % z celkového inclusive |
| **Incl** | Inclusive cycles (= včetně vnořených) |
| **Avg** | Inclusive / Calls (= průměrný inclusive čas jedné instance) |
| **Min** | Nejkratší jeden call (inclusive) |
| **Max** | Nejdelší jeden call (inclusive) |

### Inclusive vs Exclusive - klíčové rozlišení

Mnemonika přes slovní význam:

- **Incl = INCLUDING children** = "celkový čas od entry funkce po její
  return, **včetně** vnořených volání"
- **Excl = EXCLUDING children** = "**jen vlastní práce** této funkce,
  **bez** času potomků"

Vztah: `Excl = Incl - sum Incl(potomků volaných uvnitř funkce)`.
Tj. `Excl <= Incl` vždy. Pro listovou funkci (= nic nevolá) platí
`Excl == Incl`.

Příklad:

```
parent:                  ; entry CALL parent
    LD A, 5              ;   2 T  vlastní
    LD B, 3              ;   2 T  vlastní
    CALL child           ;  17 T  vlastní (call setup) + 50 T uvnitř child
    LD C, A              ;   1 T  vlastní
    RET                  ;  10 T  vlastní
                         ; -----
                         ;  32 T vlastní + 50 T v child = 82 T celkem
child:
    ; (= 50 T strávených uvnitř)
    RET
```

| Funkce | Calls | Excl | Incl | Avg |
|--------|------:|-----:|-----:|----:|
| parent | 1     | 32   | **82** | 82  |
| child  | 1     | 50   | **50** | 50  |

`parent.Incl = 82` (= celkem od CALL po RET)
`parent.Excl = 32` (= 82 - 50 children = vlastní instrukce parenta)

### Kdy se na co dívat

| Otázka | Sort by | Hledej |
|--------|---------|--------|
| Co reálně spaluje CPU? | `Excl%` desc | Hot-spoty - funkce s velkým Excl jsou úzkým hrdlem |
| Kde se tráví čas v call stromu? | `Incl%` desc | Funkce s vysokým Incl ale nízkým Excl jsou "dispatchers" - hodně volají, vlastní práce málo |
| Krátké volání tisíckrát vs jedno dlouhé? | `Calls` desc + `Avg` | Vysoký Calls + nízký Avg = micro-overhead |
| Variabilita rasterových rutin? | filter na rutinu | `Min ~ Max` = stabilní; `Max >> Min` = občasný slow path |

### Suma sloupců

- `sum Excl` všech entry = **Total cycles** (= 100 % CPU času v
  rozpoznaných voláních, zbytek = kód mezi calls a IRQ)
- `sum Incl` > Total cycles - vnořená volání se počítají v každém
  rodiči + zvlášť jako jejich vlastní entry. Proto je sloupec
  `Incl%` jiné měřítko než `Excl%`.


## 5. UI ovládání

### Toolbar

- **Start / Stop** - vypínač měření (= equivalent INI active toggle)
- **Reset** - vyčistí agregátor; status counters jdou na 0
- **Snapshot** - explicit refresh tabulky (= mimo auto polling)
- **Export CSV** - uloží data do souboru (sekce 7)

### Filter / kind toolbar

- **Filter:** substring search nad Function name nebo Address (hex).
  Case-insensitive.
- **Kind checkboxy:** CALL / RST / IRQ / NMI - skrytí kategorií z
  pohledu (data zůstávají v agregátoru, jen se nezobrazují).

### Sort

Klikni na hlavičku sloupce pro sort. Druhý klik obrátí směr. Sort
spec persistuje v INI mezi sessions.

### Hover, levý klik, pravý klik na řádku

- **Hover (~250 ms)** zobrazí tooltip s názvem funkce, adresou,
  kindem, počty calls/cycles a min/max/avg
- **Levý klik** otevře / zaměří **Disassembly #1** na entry adrese
  funkce
- **Pravý klik** otevře context menu:
  - **Set Breakpoint** - vloží BP na entry adresu
  - **Add Bookmark** - přidá bookmark s názvem sym nebo `#XXXX`
    fallback a otevře Bookmarks okno
  - **Show in Disassembly #2 / #3 / #4 / #5** - otevře / zaměří
    sekundární disasm slot na entry adrese

### Status bar

Spodní řádek okna:

- **Status** `RUNNING` / `STOPPED` - běh subsystému
- **Total cycles** - celkový profilovaný čas (T-states) od posledního
  resetu, = `sum Excl` všech bucketů
- **Calls** - celkem on_enter eventů (= součet sloupce Calls)
- **IRQ** - kolik z `Calls` bylo IRQ/NMI accept (= ne CALL/RST)
- **Unmatched** - viz níže
- **Buckets** - viz níže

### Unmatched

Counter exit eventů, kde callstack subsystém nemohl spárovat
`RET`/`RETI`/`RETN` s odpovídajícím `CALL`/`RST`/`IRQ accept`.
Tedy "návrat bez známého volání".

Vzniká v patternech:

1. **PUSH+RET trampolína** - kód si na stack pushne adresu a udělá
   `RET` místo `JP/JR`:
   ```asm
   LD HL, target
   PUSH HL
   RET            ; skok na target, ale callstack to neviděl jako CALL
   ```
   Pak target zavolá `RET` -> callstack má prázdný shadow stack ->
   "unmatched". Typicky **BDOS dispatch tabulky v CP/M**.

2. **longjmp pattern** - skok přes několik framů zpět (např. error
   handler co restoruje SP do hloubky N framů zpět a vrátí se).

3. **Self-modifying RET adresa** - kód si přepíše návratovou adresu
   na stacku -> callstack vidí jiný RET cíl než očekává.

4. **ISR-via-RET** - starší programy občas končí ISR `RET` místo
   `RETI`/`RETN` (pro Z80 PIO bez kaskádování IRQ ekvivalentní).

Profiler tyto eventy zahodí (`++unmatched_returns`) a **ignoruje je
v agregátoru** (= žádný update Calls/Cycles). Pro tento exit nemáme
párový enter, takže nelze spočítat inclusive cycles.

Interpretace hodnoty:

| Unmatched | Význam |
|-----------|--------|
| 0 nebo pár jednotek | OK, data spolehlivá |
| Roste pravidelně | Multi-stack OS pattern (CP/M BDOS, NIPOS) - profiler data **nejsou přesná** |
| Pravidelné desítky/s | Zvaž zda profilovaný kód není heavy PUSH+RET dispatch (DSL interprety, FORTH) |

### Buckets

Počet unikátních `(target_addr, kind)` záznamů v agregátoru.
Profiler drží hash mapu kde:

- **klíč** = vstupní adresa funkce (= CALL target / RST vector /
  IRQ ISR start) + kind (CALL/RST/IRQ-IM0/.../NMI)
- **hodnota** = řádek tabulky s metrikami (calls, excl, incl, ...)

Bucket vzniká **při prvním vstupu** do dané (adresa, kind)
kombinace. Mazají se jen explicit Reset.

Sanity check:

| Buckets | Význam |
|---------|--------|
| 0 | Profiler ještě nic nezachytil (po reset / před start) |
| desítky až stovky | Typický rozsah pro Z80 hru nebo demo |
| tisíce a roste | Možný SMC kód / banked code se mísí - data agregátoru zaplaveny adresami které nejsou stabilní symboly |

Paměťová stopa: každý bucket ~64 B. 10 000 bucketů ~ 640 KB,
prakticky neomezené.


## 6. Omezení

### Multi-stack OS (CP/M BDOS, NIPOS, multitasking)

Callstack vede jeden shadow stack. Když OS přepne SP do jiné rutiny
(typicky BDOS volání do TPA programu), callstack vidí divergenci a
emituje synthetic DIVERGENT exit. Profiler tyto exity skipne (= do
`unmatched_returns`). Důsledek: u CP/M aplikací je profiler měření
**nepřesné** - jména/cycles se začnou míchat mezi TPA a BDOS scope.

Workaround: profiluj jen ne-CP/M kód (= IPL bootloader, single-stack
hry, ROM monitor).

### Banking

Profiler dělá bucket podle `(target_addr, kind)` - banking state se do
klíče nezahrnuje. Pokud stejná adresa znamená různý kód v různých bank
state (= typicky 0x0000-0x0FFF v MZ-800: ROM monitor vs RAM po SW1),
agregátor je smíchá do jednoho bucketu.

### SMC (self-modifying code)

Agregace per entry adresa - SMC kód = jeden bucket. Pokud rutina
přepisuje sama sebe a vrací z různých míst, profiler měří entrypoint,
ne aktuální cesty.

### Cycles wraparound

Interní counter cyklů je uint32 (~1224 sekund při 3.5 MHz, ~5.7 min při
14 MHz). Profiler si vede vlastní 64-bit baseline s wraparound detekcí
- bez ztráty přesnosti při dlouhých session. Po hard CPU reset by se
však baseline mohl zmást; v takovém případě udělej Profiler Reset.

### Pouze párovaná volání

Pokud rutina dělá `RET` přes jiný mechanismus (PUSH addr + RET
trampolína), inclusive cycles se vrátí do parent přes longjmp
divergence handling - ale samotný divergent exit profiler skipne
(unmatched_returns++). Pro neobvyklé control-flow patterny mohou
chybět data.


## 7. CSV export

Tlačítko **Export CSV** v toolbaru otevře save dialog. Výstup je:

- **Encoding:** čisté UTF-8 bez BOM (moderní Excel UTF-8 detekuje sám,
  BOM by se v nástrojích bez BOM-awareness zobrazoval jako patvar)
- **Separator:** `,` (čárka)
- **Decimal:** `.` (= 12.34, ne 12,34) - locale-safe formátování, takže
  česká locale `cs_CZ` nerozbije strukturu
- **Line endings:** podle platformy (LF na Unix, CRLF na Windows; Excel/
  LibreOffice umí oboje)

Sloupce v CSV:

```
Name,Addr,Kind,Calls,Excl_cyc,Incl_cyc,Excl_pct,Incl_pct,Min,Max,Avg
```

První řádek je hlavička. Filter a kind checkboxy CSV export
**neovlivňují** - vždy se exportuje kompletní agregátor.

Vzor použití pro Python post-process:

```python
import pandas as pd
df = pd.read_csv('profile.csv')
hot = df.sort_values('Excl_cyc', ascending=False).head(10)
print(hot[['Name', 'Calls', 'Excl_cyc', 'Avg']])
```


## 8. Persistence

Profiler data jsou **volatile** - mezi sessions emulátoru se nepřenáší.
Explicitní Export CSV je jediná cesta pro long-term storage.

V INI sekci `[PROFILER]` se ukládají jen:

- `active` - bool, auto-start subsystému při dalším spuštění emu
- `show_window` - bool, auto-open okna

Sort spec tabulky (= seřazení sloupců) ukládá ImGui sám do své INI.


## 9. Tipy

- **Načti symboly PŘED Start** - jinak vidíš `func_XXXX` jména. Pokud
  symboly načteš až po Start, jména se v dalším Snapshot doplní (=
  lookup proběhne při render, ne při push).
- **Pro hot-spot hledání** - sort podle Excl% desc. Nejhořejší entry
  = největší kandidát na optimalizaci.
- **Pro raster timing** - profiler není ten správný nástroj (granularita
  je per-CALL, ne per-scanline). Použij Stack Monitor + breakpointy na
  scanline pozici.
- **IRQ jako parent** - default je inclusive parent obsahuje IRQ čas.
  Tj. když IRQ přeruší main loop, čas IRQ se započítá do main inclusive
  i jako vlastní IRQ entry.
- **Reset před benchmark sekvencí** - jistota že měříš jen relevantní
  okno, ne historii.
- **Watch okno + Profiler současně** - hot-spot v Profileru -> Watch
  na promenné té rutiny -> uvidíš co rutina dělá s daty.


## 10. Záložka Per PC (profiler podle PC)

Okno CPU Profiler má tři záložky: **Functions** (vše popsané výše),
**Per PC** a **Coverage** (oddíl 11). Záložka Per PC počítá každou vykonanou instrukci podle
její adresy, bez ohledu na CALL a RET. Vidí proto i kód, na který se
skáče, obsluhy přerušení a smyčky v hlavním programu - to, co záložka
Functions přičte celé jedné funkci, tu je rozložené po jednotlivých
instrukcích.

Ovládání:

- **Start** začne sbírat a smaže předchozí data, **Stop** sběr zastaví
  (data zůstanou), **Reset** data smaže.
- **Range** - zobrazit jen adresy v rozsahu (hex, např. `8000` - `8FFF`).
- **Export CSV...** zapíše všechny vykonané adresy (ne jen zobrazené),
  sloupce `pc,count,tstates,wait,tstates_pct`.
- Levý klik na řádek otevře adresu v Disassembly #1, pravý klik nabídne
  breakpoint a Disassembly #2-#5.

Sloupce:

| Sloupec | Význam |
|---|---|
| Addr | adresa instrukce (logické PC) |
| Symbol | nejbližší načtený symbol na adrese nebo pod ní, `+N` = posun |
| Count | kolikrát se instrukce vykonala |
| T% | podíl na T-stavech všech instrukcí |
| T-states | T-stavy strávené v instrukci včetně čekacích stavů |
| Avg T | průměr na jedno vykonání |
| WAIT | čekací stavy vložené hardwarem (přístup do VRAM, zápis do PSG) |

Tabulka ukazuje nejvýš 1000 řádků podle zvoleného sloupce řazení
(výchozí T-states sestupně, tedy nejdražší instrukce nahoře).

Stavový řádek rozdělí uplynulý emulovaný čas od Startu na **instrukce**,
**HALT** (CPU stojí v instrukci HALT a čeká na přerušení) a **ostatní**
(hlavně přijetí přerušení). Čas v HALT je rezerva výkonu; program, který
na snímek čeká smyčkou místo HALT, ukáže tutéž rezervu jako instrukce
čekací smyčky.

Omezení:

- Blokové instrukce (LDIR, LDDR, ...) se počítají při každém opakování
  jako jedno vykonání.
- Paměťové banky se nerozlišují - kód na stejné adrese v různých
  bankách se sčítá.
- Za běhu sběru stojí každá instrukce jedno volání funkce navíc; po
  Stop se emulace nezpomaluje.

Stejná data jsou dostupná přes MCP (`emu_profiler_pc_*`, viz
mcp-server/tools-overview.md), vykonané adresy i jako bitmapa pro
pokrytí kódu (`emu_profiler_pc_bitmap`).

## 11. Záložka Coverage (pokrytí kódu)

Záložka **Coverage** sbírá pokrytí kódu: které instrukce se vykonaly
a kterým směrem šly podmíněné instrukce (obě větve, jen skok, jen
propad), na úrovni `branch` podle adresy, nebo na úrovni `detail` podle
verze kódu s čítači. Ovládá sběr (Start, Stop, Reset, Mark, Export...),
filtruje řádky (neúplné větve, obě větve, všechny podmíněné, nové od
značky) a otevírá řádek v oknech Disassembly, která stejná data ukazují
barevnými značkami. Úplný popis: [code-coverage.md](code-coverage.md).
