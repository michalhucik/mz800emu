# Nahrávání videa

Bezztrátový záznam obrazu a zvuku z emulátoru (např. pro video na YouTube),
jednoduché střihy přímo při hraní a export hotového videa s přechody. Nahrávat
lze na všech platformách: MZ-800, MZ-700 (PAL i NTSC) a MZ-1500.

## K čemu to slouží

Emulátor zapisuje každý emulovaný snímek obrazu (celý obraz včetně borderu) a k němu
odpovídající zvuk do souboru AVI bez ztráty kvality. Záznam je "čistý": neobsahuje
okno emulátoru, menu ani oznámení, jen obraz a zvuk emulovaného počítače.

Každý řádek obrazu je v AVI zapsán dvakrát, takže AVI má stejný poměr stran
jako okno emulátoru (bod je na obrazovce dvakrát vyšší než širší). Barvy ani
body se nemění, jen se opakují řádky. Rozměry a snímková frekvence podle
platformy:

| Platforma | Obraz (včetně borderu) | Video v AVI | Plocha obrazu (canvas) | Snímků za sekundu | Zvuk |
|-----------|------------------------|-------------|------------------------|-------------------|------|
| MZ-800 | 928x288 | 928x576 | 640x200 | 50 | CTC a PSG; s druhým PSG stereo (viz níže) |
| MZ-700 PAL | 704x232 | 704x464 | 640x200 | 50 | jen CTC (oba kanály stejné) |
| MZ-700 NTSC | 704x232 | 704x464 | 640x200 | 60 | jen CTC (oba kanály stejné) |
| MZ-1500 | 704x232 | 704x464 | 640x200 | 60 | stereo: vlevo CTC a první PSG, vpravo CTC a druhý PSG |

Na MZ-800 je stereo nahrávka, když je zapnutý druhý PSG (v menu pod názvem "PSG1": **Devices -> HW
Compatibility Experiments -> Allow PSG1 (stereo)**); bez něj jsou oba kanály
stejné. Nahrávka obsahuje stejné rozložení zvuku vlevo a vpravo, jaké emulátor
přehrává.

Záznam je určen jako **master**. Z něj lze skriptem `videorec_export.py` vyrobit
MP4 pro YouTube (výřez, zvětšení, přechody, kapitoly) nebo ho zpracovat v externím
střihovém programu.

Obsluha je záměrně jednoduchá: start, stop, pauza nahrávání, značka (marker),
"retake" přes snapshot a přepínač mezi emulačním časem a časem podle reality. Co patří do střižny (titulky přes obraz, hudba, komentář,
přibližování), zůstává v externím programu.

## Ovládání

Menu **Tools -> Video Recording**:

| Položka menu       | Zkratka          | Význam |
|--------------------|------------------|--------|
| Start Recording / Stop Recording | `Alt + O` | Zahájí nahrávání, resp. ho ukončí a uloží soubory. |
| Pause Recording    | `Alt + Shift + O` | Pozastaví / obnoví zápis do souboru. Emulace běží dál. |
| Add Marker         | `Alt + L`        | Vloží značku "Marker N" na aktuální místo nahrávky. |
| Record in Real Time | `Alt + U`       | Přepne časovou základnu: zaškrtnuto = podle reality, nezaškrtnuto = emulační čas (viz [Časová základna](#časová-základna-emulační-čas-a-podle-reality)). Funguje kdykoli, i během nahrávání. |
| Settings...        | -                | Nastavení nahrávání (viz níže). |
| Open Output Folder | -                | Otevře adresář s nahrávkami ve správci souborů. |
| Remote Control... | `Alt + Shift + L` | Zobrazí / skryje okno dálkového ovládání nahrávání (viz níže). |

Při nahrávání se v pravém horním rohu obrazu emulátoru zobrazuje indikátor
**REC** s časem nahrávky a aktuální časovou základnou, např.
`REC 00:12:34 · real-time` nebo `REC 00:01:05 · emulated` (při pozastavení
nahrávání **PAUSE**; v českém prostředí je režim přeložený). Indikátor se do
videa nezapisuje. Zahájení, uložení, selhání, retake a šev ohlásí emulátor
oznámením.

Pokud zahájíte nahrávání při pozastavené emulaci (`Alt + P`), nahrávání se
rozběhne až po obnovení emulace.

Nahrávání může ovládat i AI klient přes MCP server (tooly
`emu_videorec_start`, `emu_videorec_stop`, `emu_videorec_pause`,
`emu_videorec_marker`, `emu_videorec_status` a `emu_videorec_timebase` pro
přepínání mezi emulačním časem a časem podle reality), viz
[Přehled MCP tools](mcp-server/tools-overview.md).

### Okno dálkového ovládání

Plovoucí okno **Recording Remote Control** (menu **Remote Control...** nebo
`Alt + Shift + L`) má všechno ovládání nahrávání pohromadě. Vedle každého
tlačítka je napsaná jeho klávesová zkratka, aby šla akce příště vyvolat
rovnou z klávesnice:

- **Start Recording / Stop Recording** (`Alt + O`), **Pause Recording /
  Resume Recording** (`Alt + Shift + O`), **Add Marker** (`Alt + L`).
  Do pole pod tlačítky lze napsat vlastní popisek značky (Enter ji rovnou
  vloží); prázdné pole = "Marker N".
- Stav nahrávání (IDLE / STARTING / REC / PAUSED barevně), čas nahrávky,
  počet snímků videa, velikost souborů (u více částí AVI i jejich počet),
  číslo aktuálního segmentu, časová základna a poslední událost (uložení,
  retake, šev, přepnutí časové základny, chyba). Velikost odpovídá datům,
  která už zapisovací vlákno skutečně zapsalo.
- Řádek **Time base**: bez nahrávání ukazuje nastavení pro příští start. Při
  nahrávání ukazuje skutečně použitou základnu, v režimu podle reality i to, co
  se zapisuje: *živě*, *zamrzlý obraz* (pozastavená emulace, viz nastavení
  pauzy) nebo *nezapisuje se* (přeskočená pauza, pozastavené nahrávání).
  *Emulační čas (požadováno podle reality)* se ukáže na okamžik při přepínání
  a po celou dobu, kdy emulace běží jinou než normální rychlostí a je zvoleno
  "Switch to emulated time".
- Přepínač **Time base** *Emulated time* / *Real time*, vedle něj napsaná
  zkratka `Alt + U`. Platí hned, i během nahrávání (přepnutí začne nový
  segment); bez nahrávání platí pro příští start.
- Rychlé přepnutí **Retake mode** - platí od dalšího zahájení nahrávání
  (běžící nahrávka si režim převzala při startu; liší-li se od zvoleného,
  okno ho vypíše).
- Tlačítka **Open Output Folder** a **Settings...**.

Zda je okno otevřené, si emulátor pamatuje i po restartu.

Příkazová řádka (viz [`README.md`](README.md)):

| Volba | Význam |
|-------|--------|
| `--record <soubor.avi>` | Zahájí nahrávání hned po startu do zadaného souboru. |
| `--record-frames <počet>` | Ukončí nahrávání po zadaném počtu snímků; v režimu `--headless` pak emulátor skončí. |

## Časová základna: emulační čas a podle reality

Nahrávka má dvě časové základny. Přepínat mezi nimi lze kdykoli (`Alt + U`,
položka menu **Record in Real Time**, okno dálkového ovládání, dialog Settings
nebo MCP tool `emu_videorec_timebase`), i uprostřed nahrávky. Každé přepnutí je
hranice segmentu (střih s výchozím přechodem, viz `cuts.json`). Formát AVI je
v obou základnách stejný (snímková frekvence a rozměr platformy, stejná
vzorkovací frekvence zvuku).

### Emulační čas (výchozí)

Jeden emulovaný snímek obrazu je jeden snímek videa (50 snímků videa za sekundu,
na MZ-700 NTSC a MZ-1500 60), zvuk 48 000 Hz (nebo 44 100 Hz, viz nastavení)
odpovídajícího množství vzorků na snímek. Výsledkem je dokonalý průchod hrou bez ohledu na to, jak rychle
emulátor skutečně běžel.

Zvuk nahrávky prochází stejnými filtry jako zvuk, který emulátor přehrává
(zjemnění ostrých hran a postupné ztlumení úrovně, která se déle než cca 45 ms
nemění), takže zní stejně jako výstup emulátoru. Ticho je v nahrávce nulová
úroveň.

- **Pauza emulace** (`Alt + P`) pozastaví i video. Během pauzy žádné snímky nevznikají.
- **Zrychlení a MAX SPEED se ve videu neprojeví.** Při zrychlené emulaci se zapíše
  každý snímek a výsledné video běží normální rychlostí. Při MAX SPEED může být
  emulace zpomalena tím, že zápis souboru nestačí; nic se ale neztratí.
- **Pauza nahrávání** (`Alt + Shift + O`) je něco jiného než pauza emulace: emulace
  běží dál, ale snímky se nezapisují. Po obnovení vznikne ve videu **střih**
  (hranice segmentu, viz dále). Takto lze vynechat nezajímavou nebo
  "prozrazující" část hry.

### Podle reality

Nahrávka sleduje hodiny na zdi: 50krát za sekundu (na MZ-700 NTSC a MZ-1500
60krát) vezme obraz, který byl naposledy na obrazovce, a zvuk, který šel do
reproduktorů. Hodí se pro návody
a živé ukázky, kde má divák vidět to, co jste viděli vy - včetně pauz,
zrychlení a práce v debuggeru.

- **Pauza emulace** se nahraje podle nastavení **Emulation paused**: *Skip*
  (výchozí; během pauzy se nic nezapisuje, video po ní plynule pokračuje),
  *Frozen picture and silence* (celá pauza je ve videu jako zamrzlý obraz
  a ticho) nebo *Frozen picture, at most the limit below* (zamrzlý obraz trvá
  nejvýš zadaný počet sekund, 1 až 60, výchozí 3; zbytek pauzy se přeskočí).
- **Rychlost jiná než normální**: *Record as seen* (výchozí; video ukazuje
  zrychlenou nebo zpomalenou emulaci přesně jako obrazovka) nebo *Switch to
  emulated time* (dokud rychlost není 100 %, nahrává se automaticky
  v emulačním čase; po návratu na 100 % zase podle reality - obojí je hranice
  segmentu).
- **Zvuk při rychlosti vyšší než normální** (při *Record as seen*): *As heard*
  (výchozí, jak zní), *Silence* (ticho) nebo *Attenuated* (zeslabený o 12 dB).
- **Debugger**: krokování, step over, run to cursor a zastavení na breakpointu
  se nahrají jako zamrzlý obraz jen při zaškrtnutém **Record debugger steps**;
  ve výchozím stavu se do nahrávky nedostanou.
- **Načtení snapshotu** v režimu podle reality vždy vytvoří šev (nový segment
  s výchozím přechodem); retake v režimu podle reality neexistuje. Snapshot
  uložený během nahrávání podle reality nahrávku nikdy nevrátí, ani po
  přepnutí zpět do emulačního času. Snapshot uložený dříve v úseku stejné
  nahrávky v emulačním čase lze pro retake použít dál.
- **Pauza nahrávání** (`Alt + Shift + O`) funguje stejně jako v emulačním čase.

Emulátor zapisuje do `cuts.json` také **značky stavu** (v obou základnách,
nastavení **Save state marks for export**): přepnutí časové základny, změny
rychlosti, pauzy emulace, načtení snapshotu a reset. Exportní skript je umí
vypálit do videa jako ikony (viz [Export do MP4](#export-do-mp4)). Při
**Automatic markers** (výchozí zapnuto) emulátor navíc vkládá značky
(markery), a tím i kapitoly exportovaného videa: "Reset" v obou základnách,
"Speed 400%", "Speed MAX", "Snapshot loaded" a "Pause" jen v úsecích
nahrávaných podle reality ("Pause" navíc jen pro pauzu, kterou sami uděláte).
V emulačním čase video hraje normální rychlostí, retake navazuje bez švu
a pauza ve videu není, takže změna rychlosti, načtení snapshotu ani pauza
značku nedostanou - zůstanou jen jako značky stavu v `cuts.json`. Pokud
kapitoly nechcete, vypněte **Automatic markers** nebo je smažte z `cuts.json`.

Naměřené vlastnosti režimu podle reality (dva testovací programy, jedno až tři
měření na variantu; obraz o 60 snímcích za sekundu v okně byl měřen jen na
MZ-1500, MZ-700 NTSC jen bez okna - berte je jako orientační, ne jako
záruku):

- Zvuk a obraz jsou sladěné v rámci jednoho snímku videa: průměrná odchylka
  zvuku proti obrazu byla při 50 i 60 snímcích za sekundu od -15 ms (zvuk dřív)
  do +12 ms (zvuk později); jednotlivá místa se liší až o jeden snímek videa.
- Po každé pauze emulace začne zvuk znovu s asi 60 až 80 ms ticha (asi
  4 snímky videa; měřeno na MZ-800).
- Nahrávka obsahuje přesně zvuk, který šel do reproduktorů. Proto se v ní
  může objevit i vlastnost zvukového výstupu emulátoru: tón PSG občas asi na
  jeden snímek "zamrzne" na stálé úrovni (změřeno na MZ-1500, v testu dvakrát
  za 5 s; všechny platformy s PSG používají stejný kód zvukového výstupu, na
  MZ-800 to ale nebylo měřeno). V emulačním čase to nahrávka nemá.

Jak to funguje (návrh, ne měření): aby zvuk zůstal sladěný s obrazem i v dlouhé
nahrávce, průběžně se upravuje jeho tempo; úprava je návrhem omezená nejvýš na
0,5 %. Pauza kratší než jeden snímek videa (20 ms, při 60 snímcích za sekundu
16,7 ms) se ve videu neprojeví.

## Retake přes snapshot

Pokud během nahrávání uložíte snapshot (např. rychlé uložení `Alt + F8`)
a později ho načtete (např. `Alt + F9`), protože se vám pasáž nepovedla,
záleží na nastavení **Retake mode** (platí pro emulační čas; v režimu podle
reality vytvoří načtení snapshotu vždy šev, viz [Podle reality](#podle-reality)):

| Režim | Chování |
|-------|---------|
| **Discard frames (seamless)** (výchozí) | Pokud snapshot vznikl během téže nahrávky, nahrávka se **vrátí** do bodu uložení snapshotu a vše mezi uložením a načtením se zahodí. Ve videu vznikne dokonalý průchod bez viditelného švu. Oznámení: "Retake: rewound to ČAS". |
| **Keep as cut with transition** | Nic se nezahodí. V místě načtení vznikne hranice segmentu (šev) s výchozím přechodem. |
| **Off** | Retake se nepoužívá. Načtení snapshotu vytvoří hranici segmentu (šev) s výchozím přechodem. |

Snapshot, který **nevznikl během téže nahrávky** (uložený dříve nebo při jiné
nahrávce), se vrátit zpět nedá. Stejně tak snapshot, jehož bod leží v části
nahrávky, kterou už zahodil dřívější retake (např. uložíte A, později B, vrátíte
se na A a hrajete dál - B teď patří zahozenému pokusu), snapshot uložený před
začátkem aktuálního souboru AVI (viz Soubory) a snapshot uložený hned po
načtení jiného snapshotu, dřív než se emulace rozběhla (např. načtení a uložení
v pauze - načtený stav ještě do nahrávky nevstoupil). V těchto případech vznikne
v každém režimu šev s výchozím přechodem.

Zvuk v bodě retake navazuje plynule, bez lupnutí: nahrávka pokračuje se stavem
zvuku z bodu uložení snapshotu. Platí to pro snapshoty uložené
během téže nahrávky (emulátor si pamatuje posledních 64 z nich).

Pozor: zvuk po načtení snapshotu se může od nepřerušené nahrávky nepatrně lišit
(fáze zvukového čipu PSG po načtení snapshotu [neověřeno, hypotéza]).

## Nastavení

Menu **Tools -> Video Recording -> Settings...** (uloží se do sekce `[VIDEOREC]`
konfiguračního souboru emulátoru; změny platí od další nahrávky, kromě časové
základny, která se po změně a OK přepne hned).
Čísla v konfiguračním souboru jsou **šestnáctková**, tak jak je emulátor zapisuje
(`transition_ms = 0x1f4` znamená 500 ms); i hodnota bez předpony `0x` se čte
šestnáctkově (`transition_ms = 500` by znamenalo 1280 ms). Hodnoty raději
měňte v dialogu Settings.

| Nastavení | INI klíč | Význam |
|-----------|----------|--------|
| Output folder | `output_dir` | Adresář nahrávek. Prázdné = podadresář `videos` v domovském adresáři emulátoru. |
| Audio sample rate | `audio_rate` | 48 000 Hz (výchozí, v INI `0xbb80`) nebo 44 100 Hz (`0xac44`). |
| Retake mode | `retake_mode` | `0x00` = Off, `0x01` = Discard frames (výchozí), `0x02` = Keep as cut. |
| Default transition | `default_transition` | Výchozí přechod na hranicích segmentů: `cut`, `fade` (výchozí), `crossfade`, `card`. |
| Transition length [ms] | `transition_ms` | Délka přechodu v ms (0 až 5000, výchozí 500 = `0x1f4`). |
| - | `keyframe_interval` | Interval klíčových snímků ve snímcích videa (1 až 3000, výchozí 250 = `0xfa`). Jen v INI souboru. |
| Time base | `timebase` | `emulated` (výchozí, emulační čas) nebo `realtime` (podle reality). Přepíná i `Alt + U`. |
| Emulation paused (real time) | `realtime_pause` | `skip` (výchozí), `freeze` (zamrzlý obraz a ticho), `freeze_capped` (zamrzlý obraz nejvýš po limit). |
| Frozen pause limit [s] | `realtime_pause_cap_s` | 1 až 60 s, výchozí 3 (`0x03`); platí pro `freeze_capped`. |
| Speed other than normal (real time) | `realtime_speed` | `as_seen` (výchozí, jak je vidět) nebo `emulated_when_fast` (přepnout na emulační čas). |
| Sound when faster than normal (real time) | `realtime_turbo_audio` | `as_heard` (výchozí), `silence`, `attenuate` (-12 dB). Platí pro `as_seen`. |
| Record debugger steps (real time) | `record_debugger_steps` | `0` = krokování v debuggeru vynechat (výchozí), `1` = nahrát jako zamrzlý obraz. |
| Save state marks for export | `state_marks` | `sidecar` (výchozí; značky stavu do `cuts.json`) nebo `none`. |
| Automatic markers | `auto_markers` | `1` = zapnuto (výchozí), `0` = vypnuto. |

### Předvolby

Tlačítka **Preset** v dialogu Settings nastaví několik hodnot najednou; každou
z nich pak můžete změnit samostatně. Dialog ukazuje, které předvolbě aktuální
nastavení odpovídá (nebo "custom" = vlastní).

| Předvolba | Nastaví | Určeno pro |
|-----------|---------|------------|
| **Gameplay showcase** (Ukázka hry) | časová základna `emulated`, pauza `skip`, značky stavu `sidecar` | Čistý průchod hrou: video běží normální rychlostí, pauzy v něm nejsou. |
| **Live / tutorial** (Živě / návod) | časová základna `realtime`, pauza `freeze_capped`, značky stavu `sidecar` | Návod nebo živá ukázka: video ukazuje, co jste viděli, pauzy krátce jako zamrzlý obraz. Doporučený export je s `--state-overlay icons`, aby divák viděl, kdy byla emulace pozastavená, zrychlená nebo nahrávaná podle reality. Předvolba nastavuje jen nahrávání - exportní skript ji nepozná (výchozí je `--state-overlay none`), parametr proto zadejte při exportu sami. |

## Soubory

Nahrávka `mz800_RRRRMMDD_HHMMSS` (jméno začíná platformou - `mz800`, `mz700`
nebo `mz1500` - a pokračuje datem a časem startu; při kolizi se přidá číslo)
tvoří:

| Soubor | Obsah |
|--------|-------|
| `mz800_RRRRMMDD_HHMMSS.avi` | Obraz (na MZ-800 928x576, na MZ-700 a MZ-1500 704x464; bezztrátový kodek ZMBV) a zvuk (PCM 16 bit stereo). |
| `mz800_RRRRMMDD_HHMMSS_002.avi`, ... | Další části ("party"): jeden soubor AVI je omezen asi na 1,75 GiB, po jeho naplnění nahrávka bez přerušení pokračuje v dalším souboru. |
| `mz800_RRRRMMDD_HHMMSS.cuts.json` | Popis segmentů, markerů a částí. Zapisuje se při ukončení nahrávání. |

Všechny soubory nahrávky musí zůstat pohromadě ve stejném adresáři (sidecar
odkazuje na AVI soubory jejich jménem).

Velikost: v testu s titulní obrazovkou hry (Bloxorz, MZ-800) vyšel soubor
7,9 MB na 1500 snímků = 30 s, tj. asi 15,8 MB na minutu záznamu. Většinu z toho
zabírá nekomprimovaný zvuk - jde o výpočet, ne měření: 48 000 vzorků/s x 4 B
(16 bit stereo) x 30 s = 5,76 MB, tj. asi 73 % souboru. U hry s rychle se
měnícím obrazem může být soubor větší [neověřeno].

## Úprava `cuts.json` ručně

`.cuts.json` je textový soubor (JSON) a lze ho upravit v textovém editoru:

```json
{ "version": 4, "platform": "mz800", "tv_system": "pal",
  "width": 928, "height": 576, "line_doubled": true,
  "framebuffer_width": 928, "framebuffer_height": 288,
  "canvas": { "x": 154, "y": 46, "width": 640, "height": 200 },
  "fps_num": 50, "fps_den": 1,
  "audio_rate": 48000, "default_transition": "fade", "transition_ms": 500,
  "parts":    [ { "file": "x.avi", "first_frame": 0 } ],
  "segments": [ { "start": 0, "end": 1500, "transition_in": "none" },
                { "start": 1500, "end": 4200, "transition_in": "fade" } ],
  "markers":  [ { "frame": 250, "label": "Marker 1" } ],
  "events":   [ { "frame": 0, "kind": "timebase", "value": "emulated" },
                { "frame": 0, "kind": "speed", "value": "100" } ] }
```

- `segments`: úseky nahrávky (čísla snímků od 0; `fps_num` snímků = 1 s, tj. 50
  nebo 60). `transition_in`
  je přechod na **začátku** segmentu: `none` (jen první segment), `cut`, `fade`,
  `crossfade` nebo `card`. Změňte jen přechod u jednotlivé hranice, pokud
  výchozí hodnota nevyhovuje.
- `transition_ms`: délka přechodů v ms.
- `markers`: značky. Při exportu z nich vzniknou kapitoly; `label` je jejich
  název a můžete ho přepsat (např. "Level 2").
- `events`: záznam stavu emulátoru během nahrávání (časová základna, pauza,
  rychlost, nahrání snapshotu, reset) - podklad pro export s indikátory stavu.
  Nemusíte ho upravovat.
- `width`, `height`, `line_doubled`: rozměr videa v AVI a příznak, že každý řádek
  obrazu je v AVI dvakrát. Neměňte je.
- `platform`, `tv_system`, `framebuffer_width`, `framebuffer_height`, `canvas`,
  `fps_num`, `fps_den`: platforma (`mz700`, `mz800`, `mz1500`), TV norma (`pal`,
  `ntsc`), rozměr obrazu emulátoru, plocha obrazu v něm (pro výřez `canvas`)
  a snímková frekvence. Neměňte je.
- Nahrávky ze starší verze emulátoru mají `"version": 1` (rozměr 928x288 bez
  zdvojených řádků), `"version": 2` (bez `events`) nebo `"version": 3` (bez
  popisu platformy); jsou to vždy nahrávky z MZ-800. Exportní skript zvládá
  všechny verze a výsledné MP4 vypadá stejně.

## Export do MP4

Export dělá skript `videorec_export.py` (součást distribuce i zdrojového
repozitáře emulátoru, adresář `docs/tools`). Vyžaduje **Python 3** a **ffmpeg**
(v `PATH`, nebo cestou přes `--ffmpeg` či proměnnou prostředí
`VIDEOREC_FFMPEG`). Přechod `card` s textem a textové indikátory
`--state-overlay icons` navíc vyžadují ffmpeg s filtrem `drawtext`.

```
python3 videorec_export.py nahravka.cuts.json -o video.mp4
```

Výstup je H.264 (High, yuv420p, BT.709), AAC 48 kHz, se snímkovou frekvencí
nahrávky (50 snímků/s, z MZ-700 NTSC a MZ-1500 60 snímků/s) a soubor kapitol
`video.chapters.txt` ve formátu pro popis YouTube. Obraz se zvětšuje celočíselně
metodou nearest neighbor (ostré pixely) a doplní se černými okraji na cílové
rozlišení.

Přepínače:

| Přepínač | Hodnoty | Výchozí |
|----------|---------|---------|
| `--target` | `1080p`, `1440p`, `2160p` | `2160p` |
| `--crop` | `full` (celý border), `reduced` (zmenšený border; na MZ-700 a MZ-1500, kde je border úzký, stejné jako `full`), `canvas` (jen plocha obrazu 640x200 bodů, v AVI 640x400) | `full` |
| `--aspect` | `emulator` (poměr jako v okně emulátoru), `tv43` (přibližně 4:3, [neověřeno]) | `emulator` |
| `--transition` | `cut`, `fade`, `crossfade`, `card` pro všechny hranice; bez něj platí hodnoty ze sidecaru | ze sidecaru |
| `--transition-ms` | délka přechodu v ms | ze sidecaru |
| `--card-text` | text mezistřihového okna (pro `card`) | - |
| `--font` | soubor fontu TTF; povinný s `--card-text` | - |
| `--crf` | kvalita H.264 (menší = lepší) | `12` |
| `--state-overlay` | `none`, `icons` - vypálí do obrazu indikátory stavu emulátoru ze značek stavu (viz níže) | `none` |
| `--dry-run` | jen vypíše příkaz ffmpeg | - |

Příklady:

```
# 4K, celý border, přechody podle sidecaru
python3 videorec_export.py nahravka.cuts.json -o video.mp4 --target 2160p

# 1080p, jen plocha obrazu (canvas)
python3 videorec_export.py nahravka.cuts.json -o video.mp4 --target 1080p --crop canvas

# na všech švech prolínačka 800 ms
python3 videorec_export.py nahravka.cuts.json -o video.mp4 --transition crossfade --transition-ms 800

# mezistřihové okno (2 s černá s textem) na každém švu
python3 videorec_export.py nahravka.cuts.json -o video.mp4 --transition card \
    --card-text "O 10 minut později" --font C:/Windows/Fonts/consola.ttf

# nahrávka "Live / tutorial": ikony stavu v pravém horním rohu
python3 videorec_export.py nahravka.cuts.json -o video.mp4 --target 1080p \
    --state-overlay icons --font C:/Windows/Fonts/seguisym.ttf
```

**Indikátory stavu** (`--state-overlay icons`) se objeví v pravém horním rohu
videa podle značek stavu, které emulátor při nahrávání uložil:

- **ikona pauzy** (dva pruhy), dokud byla emulace pozastavená a nahrával se
  zamrzlý obraz (přeskočená pauza nemá ve videu žádné snímky, není co označit);
- **real-time**, dokud nahrávka sledovala čas podle reality;
- **rychlost** v režimu podle reality, pokud nebyla 100 %: dva trojúhelníky
  a násobek rychlosti (např. `×4` pro 400 %, `MAX` pro MAX SPEED), jeden
  trojúhelník pro rychlost pod 100 % (např. `×0.5`). V emulačním čase se
  rychlost neukazuje, protože video tam běží normální rychlostí.

Ikona pauzy nepotřebuje font. Textové indikátory potřebují font se symbolem
trojúhelníku zadaný přes `--font` (např. `C:/Windows/Fonts/seguisym.ttf` ve
Windows nebo `DejaVuSans.ttf`) a ffmpeg s filtrem `drawtext`; bez nich skript
skončí s vysvětlením. Nahrávky bez značek stavu (ze starší verze emulátoru nebo
s `state_marks = none`) se exportují bez indikátorů a bez chyby. Texty
indikátorů jsou anglicky.

Typy přechodů: `cut` = tvrdý střih; `fade` = přes černou; `crossfade` = prolínačka
(video se zkrátí o délku přechodu); `card` = 2 s černá karta s textem (video se
prodlouží o 2 s). Segment, který je mezi dvěma měkkými přechody kratší než
dvojnásobek délky přechodu, skript odmítne a navrhne kratší `--transition-ms`.

Skript upozorní, pokud je kapitol méně než 3 nebo je některá kratší než 10 s
(YouTube takové kapitoly nemusí uznat [neověřeno]).

## Co dělat v externím střihovém programu

- **Hotové video z exportu** (MP4) otevře prakticky každý střihový program.
  Doporučený postup je vyrobit MP4 skriptem (i bez přechodů: `--transition cut`)
  a dál pracovat s ním.
- **Přímo AVI** (kodek ZMBV): dekóduje ho ffmpeg (ověřeno). Programy postavené
  na ffmpeg by ho měly otevřít [neověřeno]. **DaVinci Resolve** kodek ZMBV
  pravděpodobně neotevře [neověřeno] - použijte export do MP4.
- V externím programu patří: titulky přes obraz, hudba a komentář, přibližování,
  obraz v obraze (kamera), barevné korekce, přesné dostřihávání.

## Omezení

- Export vyžaduje Python 3 a ffmpeg; v emulátoru export není.
- Nahrávka nezahrnuje okno emulátoru, debugger ani oznámení.
- Poměr stran `tv43` je přibližný [neověřeno]; výchozí `emulator` odpovídá
  zobrazení v okně emulátoru.
- Videa jsou 50 snímků/s s přesnou frekvencí 50,000 (emulátor při 100 % běží
  přesně 50 snímků za sekundu, skutečný MZ-800 asi 50,04); z MZ-700 NTSC
  a MZ-1500 přesně 60 snímků/s (emulátor při 100 %).
- Podle reality: zvuk a obraz sladěné v rámci jednoho snímku videa, po každé
  pauze emulace asi 60 až 80 ms ticha (naměřeno, viz [Podle reality](#podle-reality)).
- Při nahrávání podle reality se může tón PSG občas asi na jeden snímek
  "zamrzne" na stálé úrovni (vlastnost zvukového výstupu emulátoru, týká se
  všech platforem s PSG); nahrávka v emulačním čase je čistá.
- Nahrávka obsahuje zvuk tak, jak ho emulátor vydává: hra, která je v emulátoru
  potichu (například některé programy pro MZ-1500), je potichu i v nahrávce.
