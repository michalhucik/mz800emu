# Overlay menu

Celoobrazovkové menu nad obrazem emulátoru na všech platformách (MZ-800,
MZ-700 PAL a NTSC, MZ-1500). Dává na jedno až dvě stisknutí to, co se při
hraní mění nejčastěji: média, rychlost, snapshoty, obraz, zvuk, vstup a
záznam. Hodí se hlavně ve fullscreenu, kde nejsou vidět okna nástrojů.

Klasické menu na pravé tlačítko myši nad obrazem funguje dál beze změny a
jako jediné obsahuje zřídka měněné nastavení hardwaru (nastavení ROM,
paměťové disky, rozšíření paměti, IDE, DIP přepínače, druhý řadič disket
a další). Pravé tlačítko myši kdekoli nad otevřeným overlay menu menu zavře
a klasické menu otevře (stejně jako **Open classic menu** na tabu Machine);
připomíná to text "Right-click: classic menu" v patičce. Neplatí to nad
otevřeným listem výměny média, nad rozbaleným seznamem a při otevřeném
dialogu.

## Otevření a zavření

| Akce | Jak |
|------|-----|
| Otevřít | klávesa `F10`, levý `Alt` + levé tlačítko myši nad obrazem, nebo položka **Overlay Menu...** klasického menu (pravé tlačítko myši nad obrazem, první položka) |
| Zavřít | `F10`, `Esc`, nebo tlačítko **Close** |

Položka **Overlay Menu...** klasického menu ukazuje ve sloupci zkratek obě
ostatní cesty ("F10, Alt+Click").

`F10` menu otevře, když má fokus obraz emulátoru (ne okno nástroje,
například debugger) a není stisknutý `Shift`, `Ctrl`, `Alt` ani `Super`.

**Horní lišta** ukazuje vlevo šest tabů a vpravo stavové štítky: platformu
(například MZ-800), rychlost (procenta nebo Max speed), **Muted** (jen při
zapnutém ztlumení zvuku) a **Paused** (jen když emulace stojí). Úplně vpravo
je tlačítko **Close**. V užším okně lišta postupně vypouští štítek platformy
a rychlosti, potom popisky neaktivních tabů; ze štítku ztlumení zůstane jen
ikona a potom zmizí popisek tlačítka Close a štítek ztlumení celý. Když ani
to nestačí (malé okno a jazyk s dlouhými slovy), zůstane ze štítku pauzy jen
ikona a nakonec zmizí i popisek aktivního tabu.
**Patička** ukazuje klávesy, které právě platí.

**Pauza.** Při otevření se běžící emulace zastaví a při zavření se vrátí do
stavu před otevřením: emulace, která běžela, se rozběhne, a emulace, která
už stála, zůstane stát. Výjimky:

- **Resume** menu zavře a emulaci rozběhne vždy, i když před otevřením stála.
- Řádek **Emulation** (Running / Paused) na tabu Machine a klávesa `Alt + P`
  přepínají pauzu přímo; stav zvolený tady po zavření menu zůstane. Emulace
  rozběhnutá tímto způsobem běží pod otevřeným menu.
- Když emulaci během otevřeného menu rozběhne nebo krokuje někdo jiný
  (debugger, MCP), menu ji při zavření nechá ve stavu, v jakém je. Pauza
  vyžádaná odjinud ve chvíli, kdy emulace stojí kvůli otevřenému menu, se
  ale nepozná a zavření menu emulaci rozběhne.
- Na tabu **Sound** je přepínač **Listen**. Zapnutý emulaci rozběhne, aby
  byly změny zvuku slyšet hned; vypnutý ji znovu zastaví. Po otevření menu
  i po příchodu na tab je vždy vypnutý a sám se vypne při přepnutí na jiný
  tab. Když menu zavřete se zapnutým přepínačem, emulace běží dál (běžela
  i před otevřením). Platí to jen pro emulaci, kterou zastavilo samo menu.
  Emulaci, která stála už před otevřením nebo kterou jste zastavili sami
  (řádek **Emulation**, `Alt + P`), přepínač nerozbíhá a je zakázaný.
  Štítek **Paused** v horní liště a titulek okna ukazují skutečný stav.

**Klávesnice.** Dokud je menu otevřené, emulovaný počítač nedostává žádné
klávesy, ani když emulace kvůli poslechu na tabu Sound běží. Globální klávesové zkratky emulátoru (viz
[`README-hotkeys.md`](README-hotkeys.md)) jsou vypnuté kromě těch, které
menu samo nabízí; ty jsou vypsané na štítcích u tlačítek a v tabulce níže.

**Joystick.** Dokud je menu otevřené, čte emulovaný počítač joystick
v klidu (žádný směr, žádné tlačítko), ani když emulace kvůli poslechu na
tabu Sound běží.
Platí pro joystick i gamepad hostitele a pro joystick na numerické
klávesnici, na obou portech. Po zavření menu se joystick emulovanému
počítači vrátí, až pustíte všechna jeho tlačítka a směry; tlačítko držené
při zavření se tak do emulovaného programu nedostane. Gamepadem jde menu
dál ovládat.

## Ovládání

| Klávesa | Akce |
|---------|------|
| `1` až `6` | Přepnout tab (Media, Machine, Display, Sound, Input, Capture) |
| `Ctrl + Tab`, `Ctrl + Shift + Tab` | Další / předchozí tab |
| šipky | Pohyb mezi prvky |
| `Enter`, mezerník | Zvolit prvek |
| `Esc`, `F10` | Zavřít menu |

Vše jde ovládat i myší. Prvek, který teď nejde použít (například karta médií
při záznamu vstupů nebo mechanika odpojeného řadiče), je zašedlý a důvod
ukáže po najetí myší.

Zkratky akcí, které fungují při otevřeném menu na kterémkoli tabu:

| Klávesa | Akce |
|---------|------|
| `F12` | Reset (menu se zavře) |
| `Alt + P` | Pauza / běh emulace |
| `Alt + N` | Normální rychlost |
| `Alt + M` | Maximální rychlost zapnout / vypnout |
| `Alt + [1..4]` | Souborový dialog pro vložení diskety do FDD 0 až 3 |
| `Alt + Shift + [1..4]` | Vysunout disketu z FDD 0 až 3 |
| `Alt + C` | Okno Virtual CMT (menu se zavře) |
| `Alt + K` | Virtuální klávesnice (menu se zavře) |
| `Alt + F6`, `Alt + F7` | Save Snapshot... / Load Snapshot... |
| `Alt + F8`, `Alt + F9` | Quick Save / Quick Load |
| `Alt + F`, `Alt + Shift + F` | Screenshot / série snímků (Record Changed Frames) |
| `Alt + O`, `Alt + Shift + O` | Nahrávání videa spustit a ukončit / pozastavit |
| `Alt + U` | Record in Real Time |
| `Alt + J` | Okno Replay Control (menu se zavře) |
| `Alt + Ctrl + Enter` | Fullscreen / okno |
| `Alt + W`, `Alt + H` | Opravit poměr stran okna podle šířky / výšky |

Ostatní globální zkratky (okna debuggeru, změna vlastní rychlosti šipkami
a další) při otevřeném menu nefungují.

Akce, které otevírají okno nástroje nebo mění běh emulátoru (Reset, Quick
Load, Load Snapshot..., Open tape window, Snapshot Setup..., Virtual
keyboard, Autotype buffer, Joystick Setup..., Settings..., Record Inputs...,
Play Replay..., Replay Control..., Open classic menu), menu zavřou.
Souborové dialogy a dotazy (například na neuložené změny pásky) se otevřou
nad menu.

## Tab 1 - Media

Úvodní tab: stav všech médií a jejich výměna.

**Floppy drives** - mechaniky FDD 0 až FDD 3 prvního řadiče disket (FDC0).
Řádek mechaniky ukazuje jméno vloženého obrazu, jeho velikost, případně
Read-only a neuložené změny (Unsaved changes), nebo Empty. Tlačítko
**Change** (u prázdné mechaniky **Insert disk**) otevře list výměny média
(viz níže); další tlačítka disketu vysunou (Eject) a uloží změny (Save
changes). Mechaniky druhého řadiče (FDC1) se dál obsluhují z klasického menu.

**Cassette** - páska ve virtuálním kazetofonu: jméno, blok a poloha,
ovládání Rewind, Play, Pause, Stop a Record a režim načítání **Tape /
Boost / Direct** (význam režimů viz [`devices/cmt.md`](devices/cmt.md)).
**Change tape** otevře list výměny pásky, **Open tape window** zavře menu
a ukáže okno Virtual CMT, kde jsou ostatní funkce kazetofonu.

**Quick Disk** - vložený disk nebo adresář, Eject a Save changes; **Insert
disk** otevře list výměny média. Quick Disk jde vložit jen do prázdné
mechaniky. Karta je zakázaná, když Quick Disk není připojený nebo ho řídí
Unicard.

**Recent media** - pruh naposledy vložených médií všech druhů od
nejnovějšího. Klepnutí na pásku nebo Quick Disk je vloží rovnou; u obrazu
diskety se otevře volba mechaniky **Insert into** (klávesy `1` až `4`,
`Enter` vloží do vybrané). **Browse files...** otevře souborový dialog pro
disketu do první prázdné mechaniky (jsou-li všechny obsazené, do FDD 0).
**Clear history** smaže historii všech druhů médií (na vložená média nesahá); je
vidět, jen když historie není prázdná.

**Řada akcí** - Resume, Speed (maximální rychlost zapnout / vypnout; popisek
ukazuje současnou rychlost), Quick Save, Quick Load, Screenshot, Fullscreen
a Reset.

Při záznamu nebo přehrávání vstupů (Replay) jsou karty médií zakázané.

Obraz diskety, který už je vložený v jiné mechanice, se vloží chráněný
proti zápisu, aby do souboru obrazu mohla zapisovat jen jedna mechanika.
Řádek mechaniky pak ukazuje "Shared with FDD n, write-protected"
a **Write protect** v listu nejde vypnout, dokud druhá mechanika smí do
souboru zapisovat. Viz devices/fdc.md, oddíl "Stejný obraz ve více
mechanikách".

### Výměna média

**Change**, **Insert disk** a **Change tape** otevřou list s naposledy
použitými soubory daného druhu (nejvýš šest):

| Klávesa | Akce |
|---------|------|
| `1` až `6` | Vložit soubor s tímto číslem; jen dokud je filtr prázdný (při neprázdném filtru se číslice píše do filtru). Rozhoduje klávesa, ne znak, takže číslice fungují na každém rozložení klávesnice |
| psaní | Filtrovat seznam podle textu; `Backspace` maže poslední znak |
| šipky, `Enter` | Vybrat soubor a vložit ho |
| `Esc` | Zavřít list (menu zůstane otevřené) |
| `F10` | Zavřít celé menu |

Pod seznamem jsou **Browse files...** (souborový dialog jako v klasickém
menu), **Eject** a u mechaniky a Quick Disku přepínač **Write protect**.
Řádek "Now:" ukazuje, co je vložené teď. Soubor, který už neexistuje, je
označený File not found; při pokusu o vložení se z historie odstraní.
Dotazy na neuložené změny a běžící nahrávání pásky se nepřeskakují.

### Historie Recent media

Emulátor si pamatuje až 12 naposledy vložených souborů pro každý druh média
(obraz diskety, páska, Quick Disk). Do historie se zapíše médium, které
vložil uživatel: z overlay menu, z klasického menu, klávesovou zkratkou nebo
volbou na příkazové řádce (kromě běhu bez okna, `--headless` a `--mcp-pipe`,
kde se do historie nezapisuje nic). Nezapisují se diskety obnovené po startu
z konfigurace, média vkládaná přehráváním záznamu vstupů, páska při načtení
snapshotu ani vložení přes MCP nebo z emulovaného správce Unicard.

Stejnou historii nabízí i klasické menu (pravé tlačítko myši) v podnabídce
**Recent**: u každé disketové mechaniky (HW & Devices > FD Controller >
FDD n, oba řadiče), u kazety (HW & Devices > CMT) a u Quick Disku (HW &
Devices > Quick Disk > QD Drive). Ukazuje nejvýš 12 jmen souborů daného
druhu od naposledy vloženého, celou cestu má tooltip; klik médium vloží
stejně jako výběr v souborovém dialogu, včetně dotazů na neuložené změny.
Prázdná historie ukáže zakázanou položku "(empty)". Položka **Clear recent
media** na konci podnabídky smaže historii všech druhů jako **Clear
history** v overlay menu.

Historie se ukládá s konfigurací do INI souboru, sekce `[RECENT_MEDIA]`:
klíče `dsk_1` až `dsk_12`, `cmt_1` až `cmt_12` a `qd_1` až `qd_12` (cesta,
`1` = naposledy vložené) a ke každému klíč `<klíč>_time` (čas vložení
v sekundách od 1. 1. 1970 UTC). Při spuštění s `--no-save-ini` se historie
neuloží.

## Tab 2 - Machine

| Karta | Obsah |
|-------|-------|
| Machine | **Emulation** Running / Paused, **Reset**, výběr **ROM** |
| Speed | **CPU speed** Normal / Custom / Max a **Custom speed** s krokem 10 % |
| Snapshots | Quick Save, Quick Load, Save Snapshot..., Load Snapshot..., Snapshot Setup... |
| More hardware | **Open classic menu** zavře overlay a otevře klasické menu |

Volba Custom je dostupná, až když je nastavená vlastní rychlost. Výběr ROM
a načtení snapshotu nejsou dostupné při záznamu nebo přehrávání vstupů.

## Tab 3 - Display

Karty jsou v jednom sloupci u levého okraje a plocha menu je na tomto tabu
průhlednější, takže zbytek obrazu emulátoru zůstává vidět. Nahoře jsou karty,
jejichž účinek je na obraze vidět hned. V menším okně se obsah tabu svisle
roluje.

| Karta | Obsah |
|-------|-------|
| Colors | **Color schema** Normal / Grayscale / Green |
| CRT scanlines | **Enable scanlines** a **Intensity** (jde měnit jen při zapnutých scanlines) |
| Window | **Fullscreen** (v klasickém menu Interface > Display > Fullscreen), **Window size on startup** (Half, Native, Normal, Bigger, Big, Fullscreen), **Lock window aspect ratio**, **Fix aspect ratio** By width / By height |
| Frame rate | **Frame rate control** (Slave, Swap With V-Sync, Swap With V-Sync (Adaptive), Immediate Swap, Custom FPS), v režimu Custom FPS řádek **Custom FPS** s hodnotou v Hz a tlačítky po 1 Hz, a **Forced redraw every screen** |

Změny se projeví hned ("Changes apply immediately.").

## Tab 4 - Sound

**Output:**

- **Listen** - poslech při otevřeném menu. Emulace na tabu Sound stojí jako
  na ostatních tabech; zapnutý přepínač ji rozběhne, takže změny hlasitosti,
  ztlumení, propusti i kanálů jsou slyšet hned. Podrobnosti níže.
- **Master volume** - hlavní hlasitost poslechu 0 až 200 %, výchozí 100 %.
  Do 100 % ubírá, nad 100 % přidává (až 6 dB; špičky se přitom měkce
  stlačují, neoříznou se). Na 100 % je na posuvníku značka: tažení myší se
  u ní přichytí a dvojklik hodnotu na 100 % vrátí.
- **Mute** - ztlumení; hlavní hlasitost ani hlasitosti kanálů se nemění,
  po zrušení ztlumení hraje emulátor stejně hlasitě jako předtím. Zapnuté
  ztlumení připomíná štítek **Muted** v horní liště menu na všech tabech
  a značka **MUTED** v titulku hlavního okna. Ztlumení se neukládá: emulátor
  se vždy spustí neztlumený.
- **Hardware output filter** - výstupní propust skutečného MZ-800 (viz
  [`README.md`](README.md), oddíl Zvuk); v emulátorech MZ-700 volba není.

Hlavní hlasitost a ztlumení se uplatní až na zvukovém výstupu emulátoru:
do nahrávek (zvuk videa, zachycení zvuku) se nepromítají a nahrávka má
plnou hlasitost i při ztlumeném poslechu. Hlavní hlasitost se ukládá do INI
souboru, sekce `[AUDIO]`, klíč `volume_master` (celé číslo 0 až 4294967295,
stejné měřítko jako ostatní klíče `volume_*`, výchozí je nejvyšší hodnota
= 100 %) a klíč `volume_master_boost` (procenta nad 100 %, 0 až 100,
výchozí 0); hlasitost je jejich součet. V souboru jsou obě hodnoty
šestnáctkově (`0xffffffff`, `0x00` až `0x64`). Stejné **Mute** a **Master volume** jsou i v okně Interface ->
Audio... (Audio Mixer).

**Channels** - hlasitost reproduktoru (CTC0) a jednotlivých kanálů PSG,
stejné jako v okně Interface -> Audio...; **Reset channels** vrátí všechny
na 100 %.

**Poslech (Listen).** Přepínač je po každém otevření menu i po každém
příchodu na tab Sound vypnutý a emulace stojí; pod kartou Output je věta
"Turn on Listen to hear the changes while the menu is open. The emulated
computer keeps running without keyboard input.". Po zapnutí emulace běží
(věta "The emulation is running so that you can hear the changes.") a štítek
**Paused** v horní liště zmizí; emulovaný počítač přitom dál nedostává
klávesy a joystick čte v klidu. Poslech skončí a emulace se znovu zastaví,
když přepínač vypnete nebo přepnete na jiný tab. Když menu zavřete, emulace
běží dál. Stav přepínače se neukládá.

Přepínač je zakázaný a důvod ukazuje v tooltipu, když pauza nepatří menu:

- emulace stála už před otevřením menu nebo jste ji zastavili sami (řádek
  **Emulation** na tabu Machine, `Alt + P`): "The emulation was paused before
  the menu was opened. Resume it to listen."; změny uslyšíte, až emulaci
  rozběhnete;
- emulace běží z jiného důvodu než kvůli poslechu (rozběhli jste ji řádkem
  **Emulation** nebo `Alt + P`): přepínač ukazuje zapnuto a tooltip říká
  "The emulation is running."; změny jsou slyšet hned.

Když emulaci během poslechu zastaví někdo jiný (`Alt + P`, debugger, MCP),
přepínač se vypne a emulace zůstane stát i po zavření menu. Uložení
snapshotu (Quick Save) během poslechu přepínač také vypne; po jeho dokončení
emulace stojí kvůli otevřenému menu jako před zapnutím poslechu.

## Tab 5 - Input

- **Keyboard** - **Virtual keyboard** a **Autotype buffer** zavřou menu
  a otevřou příslušné okno.
- **Joystick** - **Device** (None / Numeric keypad / Joystick) a **Joystick
  Setup...** pro mapování a kalibraci.

## Tab 6 - Capture

| Karta | Obsah |
|-------|-------|
| Screenshot | Save Screenshot, Save Screenshot As..., Record Changed Frames, Open Output Folder, Settings... (viz [`screenshots.md`](screenshots.md)) |
| Video Recording | Start Recording / Stop Recording, Pause Recording, Record in Real Time, Open Output Folder, Settings... (viz [`video-recording.md`](video-recording.md)) |
| Replay | stav záznamu vstupů, Record Inputs..., Play Replay..., Stop Replay, Replay Control... (viz [`replay.md`](replay.md)) |

Ostatní funkce těchto nástrojů (okna Screenshot Control a Remote Control,
značky ve videu, Rewind a posun v záznamu vstupů, zachycení zvuku) zůstávají
v klasickém menu a pod svými klávesovými zkratkami.
