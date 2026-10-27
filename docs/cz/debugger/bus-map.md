# Bus Map - kdo vlastní který port a MMIO adresu

Okno debuggeru **Bus Map** (menu **HW & Devices / Bus Map**, MZ-700,
MZ-800 i MZ-1500) ukazuje sběrnici Z80 emulovaného stroje ze strany adres:
pro každý port 00h-0FFh a každou adresu okna mapovaných portů
0E000h-0E0FFh říká, který čip desky nebo které rozšiřující zařízení tam
odpovídá, v jakém směru, jak je připojené k datové sběrnici a co se stane,
když se dvě zařízení potkají. Rozšiřující zařízení (devbus) má jen
MZ-700; na MZ-800 a MZ-1500 okno ukazuje čipy a periferie desky. Panel I/O Ports je pohled "registr po registru", Bus Map pohled
"adresa po adrese"; obě okna mohou být otevřená vedle sebe.

## Obsah

- [Co okno ukazuje](#co-okno-ukazuje)
- [Záložka Ports](#záložka-ports)
- [Záložka MMIO](#záložka-mmio)
- [Seznam vlastníků](#seznam-vlastníků)
- [Detail vybrané adresy](#detail-vybrané-adresy)
- [Klávesnice](#klávesnice)
- [Výstupní stupeň zařízení](#výstupní-stupeň-zařízení)
- [Nebuzená sběrnice a kolize](#nebuzená-sběrnice-a-kolize)
- [Sledování aktivity](#sledování-aktivity)
- [Nastavení (INI)](#nastavení-ini)
- [Související](#související)

## Co okno ukazuje

```
[Ports] [MMIO]   Color by [Owners v] (?)   [x] Track activity
+-- pruh stavu nad mřížkou --------+||+-- Owner   Ports    Drive      State -------+
|     0  1  2  3 ... F             ||||[CH] CMT hack  01h-02h  none       present   |
| 00  .  CH CH  .  ...             ||||[U]  Unicard   50h-51h  tri-state  present   |
| 50  U  U  .   .  ... F1 F1 F1 F1 ||||[K8] K&P 80... 70h-73h  tri-state  [Connect] |
| 70  K8 K8 K8 K8  ...             ||||  ...  (ScrollY, řazení podle prvního portu) |
| F0  G  G  .   .  Q  Q  Q  Q  RD  |||+-- [K8] Port 70h  K&P 80 Column Card -------+
+----------------------------------+||| IN/s 0  OUT/s 0  ghost 0  conflicts 0       |
                                    ||| Last IN -   Last OUT -                       |
                                    ||| Peek: -  [Peek now]                          |
                                    ||| Flags: potential conflict                    |
                                    ||| [Show in I/O Ports]  [Reset activity]        |
                                    ||| [Break on IN]        [Break on OUT]          |
MZ-700 | 3 devices, 0 connected (?)
```

Vlevo mřížka 16 x 16 (řádek = horní, sloupec = dolní polovina dolního
bajtu adresy); velikost buňky se odvozuje z písma, takže zkratka se vždy
vejde. Vpravo seznam vlastníků aktivní mřížky a pod ním detail vybrané
adresy. Každá buňka nese zkratku vlastníka (`U` Unicard, `F0` FDC 0, `M`
paměťový mapper, `K8` karta K&P 80, `P7` PCG-700, ...), barvu podle
zvoleného režimu a odznaky (tmavý kroužek s bílým glyfem, sedí na rohu
buňky a zkratku nepřekrývá), které nikdy nespoléhají jen na barvu:

| Značka | Význam |
|--------|--------|
| šrafovaná buňka | vlastník není přítomný (periferie odpojená v menu HW & Devices, čip nedekódující v aktuálním režimu) nebo rozšiřující zařízení není připojené |
| `!` se žlutým prstencem (vpravo nahoře) | potenciální kolize: dva vlastníci by budili tytéž bity (připojení druhého s výchozí politikou selže) |
| `!` s červeným prstencem | za běhu došlo ke čtení s kolizí (politika `and` / `first`) |
| `?` (vlevo nahoře) | výstupní stupeň vlastníka není v bázi znalostí doložen (`not documented`) |
| `~` (vlevo dole) | čtení a zápis obsluhují různí vlastníci |
| tečka (vpravo dole) | program sáhl na adresu bez vlastníka při zapnutém sledování (ghost přístup) |

Význam odznaků je slovy v tooltipu buňky a v řádku **Flags** detailu.

Režimy obarvení (combo **Color by**): **Owners** (modrá čip desky,
zelená rozšiřující zařízení, tmavá šrafovaná nepřítomné / nepřipojené,
šedá bez vlastníka), **Drive** (výstupní stupeň: tri-state, partial, open
collector, none, not documented), **Activity** (teplo podle přístupů za
sekundu, stejné prahy jako v panelu I/O Ports; **(?)** vedle comba je
vypíše). Mini buňky v seznamu vlastníků mění barvu s režimem, takže
seznam je zároveň legenda; samostatná legenda není.

Pruh nad mřížkou normálně krátce popisuje mřížku; zvýrazní se, když je
sledování aktivity vypnuté nebo když horní oblast 0E000h právě není
mapovaná (záložka MMIO). Najetí na buňku ukáže strukturovaný tooltip
(vlastníci se směrem, druhem, stavem a výstupním stupněm, rozsah, zdroj
deklarace, zrcadla, značky slovy, poslední hodnoty a aktivita); levý klik
vybere adresu (a řádek jejího vlastníka v seznamu), pravý klik otevře menu
(Show in I/O Ports, Break on IN, Break on OUT, Reset activity, Peek now).

## Záložka Ports

Porty 00h-0FFh podle dolního bajtu adresy, jak je dekóduje deska.
Vlastníci jsou dvojího původu: čipy a periferie desky (statická tabulka
platformy: Unicard, FDC 0 / FDC 1, IDE8, RAM disky, Quick Disk, paměťový
mapper, paměťová expanze, registry GDG, na MZ-800 a MZ-1500 také PSG a Z80
PIO, na MZ-800 joysticky a v režimu MZ-800 8255 a 8253 na 0D0h-0D7h)
a rozšiřující zařízení (jen MZ-700: karta K&P 80 na 70h-73h, viz
[../devices/kp80.md](../devices/kp80.md)). Periferie odpojená v menu HW &
Devices své porty dál vlastní, čtení ale vrátí nebuzenou sběrnici
(šrafovaná buňka, příznak UNMAPPED); stejně se šrafují čipy, které
v aktuálním režimu nedekódují (8255 a 8253 na portech MZ-800 v režimu
MZ-700, druhý PSG bez volby HW kompatibility).

Port s jiným vlastníkem pro čtení a zápis má příznak DIR_SPLIT: na MZ-800
je to 0F0h (OUT paleta GDG, IN joystick 1). Port 0CEh (OUT DMD, IN stav)
patří v obou směrech GDG, proto rozdělený není. Mapper MZ-800 dekóduje
i IN 0E0h / 0E1h (volba VRAM / CG-ROM), sběrnici při něm ale nebudí;
takové čtení se počítá jako ghost přístup. Na 0E9h-0EBh MZ-800 má dva
vlastníky RAM disk Pezik na 0E8h a standardní RAM disk; emulátor dává
přednost Peziku, okno proto hlásí potenciální kolizi.

## Záložka MMIO

Okno 0E000h-0E0FFh: 8255 PPI (0E000h-0E003h), 8253 CTC
(0E004h-0E007h, řídicí slovo jen pro zápis), registr stavu / TEMPO 0E008h
a na MZ-700 rozšiřující zařízení, která tam sledují zápisy (kit SUC 80
0E00Ch-0E00Fh, PCG-700 0E010h-0E012h). Na MZ-800 čipy na těchto adresách
dekódují jen s DMD v režimu MZ-700 (v režimu MZ-800 jsou šrafované), na
MZ-1500 vždy, když je horní ROM mapovaná se SPEC = 0. Každá buňka má dvě půlky: levá
ukazuje vlastníka ve stavu "horní oblast mapovaná" (po resetu,
`OUT (0E3h)`, `OUT (0E4h)`), pravá ve stavu "DRAM na 0E000h" (po
`OUT (0E1h)`); šedá půlka znamená, že v tom stavu adresu nikdo nedekóduje.
Když je právě mapovaná DRAM, je celá mřížka ztlumená a pruh nad mřížkou
to říká.

## Seznam vlastníků

Pravý panel: jeden řádek na čip desky nebo rozšiřující zařízení aktivní
mřížky, řazený podle prvního portu. Sloupce: mini buňka se zkratkou
v barvě buňky (legenda), název, rozsahy adres (nejvýš tři, další jako
"+N"; všechny v tooltipu), výstupní stupeň, stav (present / not present /
connected / not connected) a u rozšiřujících zařízení tlačítko
**Connect** / **Disconnect** (stejná akce jako menu HW & Devices; čipy
desky se připojují v tom menu). Najetí na řádek zvýrazní jeho buňky
v mřížce žlutým rámečkem, klik na řádek vybere jeho první adresu, klik na
buňku vybere řádek jejího vlastníka. Tooltip řádku: úplný název, směr,
všechny rozsahy, zdroj deklarace výstupního stupně, poznámka o zrcadlech
(emulátor dekóduje port jen dolním bajtem adresy, na skutečném stroji
neměřeno; pro MZ-700 bez poznámky), u zařízení počet portů, MMIO adres,
regionů a výstupů. Seznam roluje, vybraný řádek je vždy vidět. Na MZ-800
a MZ-1500 (bez rozšiřujících zařízení) sloupec s tlačítkem není.

## Detail vybrané adresy

Pod seznamem panel stálé výšky (seznam nad ním se při změně výběru ani
stavu neposouvá): titulek s mini buňkou a názvem vlastníka ("Port 70h
K&P 80 Column Card"; bez vlastníka "Port 20h - no owner"; tooltip titulku
vypíše všechny vlastníky), řádek o vlastníkovi (směr a výstupní stupeň,
u částečného i buzené bity; další vlastníci téže adresy za **Also:**; bez
vlastníka místo toho, co čtení vrátí: "A read returns the undriven bus:
bus latch 5Ah"), řádek aktivity v neproporcionálním písmu (IN/s, OUT/s,
ghost, conflicts; u MMIO adresy writes/s), poslední IN a OUT se stářím ve
snímcích, **Peek** (hodnota, kterou by CPU právě přečetl, bez vedlejších
efektů: zařízení odpovídají přes své peek handlery, čipy desky přes
zrcadla registrů; v pauze emulátoru automaticky s textem "(automatic
while paused)", za běhu tlačítkem **Peek now**; u částečného buzení
i maska buzených bitů a zdroj hodnoty), **Flags** slovy a čtyři tlačítka
stejné šířky: **Show in I/O Ports**, **Reset activity**, **Break on IN**,
**Break on OUT** (u MMIO adres jsou breakpointy IORQ vypnuté). Dlouhý
řádek se neořezává zalomením, ale useknutím; celé znění je v jeho
tooltipu. S vypnutým sledováním jsou místo čísel pomlčky a místo
posledních hodnot řádek "Activity tracking is off".

Stavový řádek: platforma a počet rozšiřujících zařízení ("MZ-700 |
3 devices, 0 connected"; na MZ-800 a MZ-1500 jen platforma); nestandardní
model nebuzené sběrnice (`const`) a politika kolize (`and`, `first`) se
připojí za svislítko. **(?)** na konci ukáže stav sledování, model
nebuzených bitů s aktuální hodnotou, politiku a číslo snímku.

## Klávesnice

Tab přechází mezi toolbarem, mřížkou, seznamem a tlačítky detailu; šipky
v mřížce posouvají výběr (zaměřená buňka má rámeček), Space nebo Enter na
buňce ji vybere, Enter na řádku rozšiřujícího zařízení ho připojí nebo
odpojí. Okno se pod přirozenou šířkou (mřížka + seznam) nepřeskládává,
jen dostane vodorovný posuvník; oddělovač mezi mřížkou a seznamem (tenká
čára, při najetí zvýrazněná) jde táhnout, dvojklik na něj vrátí
automatickou šířku. Šířka seznamu je pro obě záložky stejná, přepnutí
Ports / MMIO nic neposune.

## Výstupní stupeň zařízení

Každý rozsah portů a MMIO rozsah zařízení deklaruje, jak je jeho hardware
připojen k datové sběrnici:

| Výstupní stupeň | Význam | Příklady |
|-----------------|--------|----------|
| none (write only) | sběrnici nikdy nebudí, jen sleduje zápisy | registry PCG-700, latch SUC, paměťový mapper, registry GDG, PSG |
| tri-state | budí všech osm bitů, dokud je dekódovaná jeho adresa | 8255 PPI, 8253 CTC (datasheet), Z80 PIO (technický manuál), Unicard (VHDL), karta K&P 80 (74LS245 za signálem výběru desky) |
| partial | budí jen některé bity, ostatní plavou | budiče joysticku MZ-800 (74LS365, bity 0-5; bity 6-7 na HW nikdo nebudí, emulátor bez joysticku vrací 0FFh) |
| open collector | wired-AND s klidovou 1 | na datových linkách žádného MZ nenalezen; jen pro úplnost |
| unknown | báze znalostí nemá schéma ani datasheet výstupního stupně | karta FDC, IDE8, RAM disky, rozhraní Quick Disk, stav GDG (0CEh, 0E008h; zákaznický obvod bez datasheetu) |

Deklarace vychází z dokumentů, nikdy z odhadu; `unknown` se ukazuje
s `?` a bere se jako "může budit všechny bity".

## Nebuzená sběrnice a kolize

Bity, které nikdo nebudí, dostanou hodnotu **modelu nebuzené sběrnice**
emulátoru: ve výchozím stavu poslední bajt, který CPU přečetl z paměti
(latch sběrnice; `IN A,(n)` z volného portu vrátí `n`), volitelně
konstantu, například 0FFh. **Ani jeden model nebyl změřen na skutečném
hardwaru** - konstanta existuje pro porovnání dobového software proti
oběma předpokladům.

Dvě zařízení smějí sdílet adresu, když ji budí nejvýš jedno z nich (karta,
která jen sleduje zápisy, vedle čipu desky). Dva budiče na týchž bitech
jsou **kolize**. Výchozí politika `refuse` druhé zařízení nepřipojí (menu
HW & Devices i `emu_periph_attach` ohlásí kolizi). Politiky `and` (bitový
AND obou hodnot) a `first` (vyhrává dřív připojené zařízení) připojení
dovolí; každé čtení s kolizí se pak označí v Event Vieweru (podtyp IORQ
CONFLICT), započítá v inspektoru a ohlásí klientům MCP (událost
`bus_conflict`). Obě politiky jsou modely - skutečný výsledek dvou
soupeřících budičů je nedefinovaný; slouží k pokusům, ne k věrné emulaci.
V GUI přepínač nemají; viz [Nastavení](#nastavení-ini).

## Sledování aktivity

**Track activity** přepne CPU na logovací callbacky (stejný mechanismus
jako panel I/O Ports; obě okna ho sdílejí, zavření jednoho druhé
nevypne) a plní přístupy za sekundu, poslední hodnoty, součty a čítače
ghost přístupů. S vypnutým sledováním mřížka dál ukazuje vlastníky,
příznaky a čítače kolizí z jádra sběrnice.

## Nastavení (INI)

Okno ukládá `color_mode`, `active_tab`, `list_width` (šířka seznamu v px,
0 = automaticky) a `tracking_active` v sekci `[BUS_MAP_PANEL]`. Model sběrnice je v sekci
`[DEVBUS]` emulátoru MZ-700 (jediná platforma s rozšiřujícími
zařízeními) a v GUI přepínač nemá (je dostupný i přes MCP
`emu_settings_set`, klíče `DEVBUS/conflict`, `DEVBUS/bus_idle`,
`DEVBUS/bus_idle_value`):

```ini
[DEVBUS]
conflict = refuse       ; refuse | and | first
bus_idle = latch        ; latch | const
bus_idle_value = 0xff   ; hodnota pro const
```

Politika a model nebuzené sběrnice se ukládají do souboru záznamu vstupů;
přehrání s jiným nastavením varuje.

## Související

- [io-ports.md](io-ports.md) - registr po registru, historie, filtry.
- [event-viewer.md](event-viewer.md) - události IORQ s podtypy UNCONNECTED
  (ghost) a CONFLICT.
- [../devices/pcg700.md](../devices/pcg700.md),
  [../devices/suc80.md](../devices/suc80.md),
  [../devices/kp80.md](../devices/kp80.md) - rozšiřující zařízení.
- [../mcp-server/tools-overview.md](../mcp-server/tools-overview.md) -
  `emu_get_bus_map`, `emu_bus_peek`.
