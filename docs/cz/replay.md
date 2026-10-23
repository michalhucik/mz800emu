# Záznam a přehrání vstupů (Replay)

Vstupy jednoho běhu se jednou nahrají a pak přesně přehrají - ve stejném
nebo v novém procesu emulátoru, při libovolné rychlosti. Typické použití:

- hraji hru nebo program, dokud se neobjeví chyba, a pak přesně stejný
  běh opakuji, kolikrát potřebuji, s jinými breakpointy, trace nebo
  záznamem zápisů do paměti;
- nahraný běh slouží jako regresní test: `--replay-verify` v CI ověří,
  že ho nový build pořád odehraje stejně;
- při záznamu se vrátím o pár sekund a zkusím jinou cestu, při
  přehrávání skočím na libovolný snímek (viz [Checkpointy, návrat
  a skok](#checkpointy-návrat-a-skok)).

Všechny platformy (MZ-800, MZ-700 PAL i NTSC, MZ-1500). Záznam jde
přehrát jen v binárce téže platformy a TV normy.

## Jak to funguje

Záznam začíná kompletním snapshotem stroje (vždy s RAMdiskem, bez ohledu
na nastavení snapshotů), který se při startu uloží a hned znovu načte.
Spolu s ním záznam uloží obsah vložených médií (viz [Média](#média)). Od
té chvíle ukládá po emulovaných snímcích:

- každou změnu klávesnice (celou klávesnicovou matici: klávesnice
  počítače, okno virtuální klávesnice i klávesy z MCP dohromady)
  a joysticků (joystick hostitele, numerická klávesnice, MCP),
- resety a autotype,
- ovládání kazety (play, stop, pauza, vysunutí, výběr bloku, nahrávání
  na kazetu) a soubory vybrané v dialogu CMT hacku,
- zápisy debuggeru a nástrojů MCP (paměť, regiony paměti, registry,
  příznaky CPU, porty I/O, vyvolané IRQ a NMI),
- každých N snímků (výchozí 50, na PAL jedna sekunda) SHA-256 CPU
  (registry včetně skrytého stavu Z80), RAM, RAM MemExt, VRAM, PCG
  a periferií (CTC 8253, PIO 8255, Z80 PIO, PSG, GDG, řadič VRAM, řadiče
  disket a Quick Disku) spolu s emulovaným časem.

Všechny vstupy se přebírají na hranici snímku: klávesa stisknutá uprostřed
snímku se do emulovaného počítače dostane na jeho konci, při záznamu
i při přehrávání. Joystick hostitele se také čte jednou za snímek.
Přehrávání načte startovní snapshot, na stejných místech použije vstupy
a porovnává hashe. První snímek, jehož stav se liší, přehrávání zastaví.

Hodiny Unicardu ukazují při záznamu i přehrávání čas startu záznamu
a k němu emulovaný čas, takže program vidí při přehrání stejné hodiny.

## Záznam

| Způsob | Jak |
|--------|-----|
| Menu | **Replay -> Record Inputs...** se zeptá na soubor (`.mzr`) a začne. **Replay -> Stop Replay** ho uloží. Stejná tlačítka má i okno **Replay Control** (viz níže). |
| Příkazová řádka | `--replay-record <soubor>` nahrává od prvního snímku; soubor se zapíše při ukončení emulátoru. `--replay-hash-interval <N>` nastaví počet snímků mezi hashi (1-100000). |
| MCP | `emu_replay_record_start`, `emu_replay_record_stop`, `emu_replay_rewind`, `emu_replay_status` (viz [`mcp-server/tools-overview.md`](mcp-server/tools-overview.md)). |

Když emulace běží, pozastaví se na nejbližší hranici snímku, záznam začne
a emulace běží dál. Během záznamu je v levém dolním rohu obrazu
**REPLAY REC** a číslo snímku.

Během záznamu:

- **Nastavení stroje** (menu Devices, platforma, periferie, rozšíření
  paměti, ROM) a **načtení snapshotu** nejsou k dispozici: záznam by je
  nezopakoval. Přes MCP se nástroje, které stroj takto mění (načtení
  snapshotu, média, nastavení), odmítnou s vysvětlením. Změna zařízení,
  která nepřijde z menu (zkratka Alt+1..4, dialog výběru souboru, okno
  zařízení otevřené před začátkem záznamu nebo změna typu joysticku
  v okně Joystick Setup), se odmítne s hlášením také.
- **Kazetu** jde ovládat jako obvykle (okna Virtual CMT a Tape Filelist,
  MCP `emu_cmt_*` pro transport, přesun na blok a nahrávání na kazetu).
  Vložení jiné pásky, rychlost pásky, vlastní pulzy, polarita a přepínač
  CMT hacku během záznamu k dispozici nejsou.
- **Zápisy debuggeru** se zaznamenají, když emulace běží, a v pauze na
  hranici snímku - po pauze (tlačítko pauzy i MCP `emu_pause` pozastaví
  na konci snímku), po `emu_run(frames=N)` nebo po deadlinu. V pauze mezi
  hranicemi snímků (po kroku nebo breakpointu) se zápis odmítne: nejdřív
  doběhnout na další hranici snímku. Ovládání kazety a reset z takové
  pauzy počkají na další hranici snímku.
- **Akce breakpointů**, které zapisují (`poke`, `set`), se nezaznamenají;
  záznam dostane varování a správně se přehraje jen se stejnými
  breakpointy.
- Záznam nezačne, dokud běží autotype, jsou aktivní Freeze Bytes nebo
  kazeta nahrává.

## Přehrání

| Způsob | Jak |
|--------|-----|
| Menu | **Replay -> Play Replay...** se zeptá na soubor, načte startovní snapshot a rozběhne emulaci. **Replay -> Stop Replay** přehrávání ukončí. |
| Příkazová řádka | `--replay <soubor>` přehraje záznam po startu. `--exit-at-replay-end` ukončí běh na konci přehrávání, `--replay-verify` totéž v MAX SPEED (viz níže). |
| MCP | `emu_replay_play` (`pause_after_load` pro nastavení breakpointů před rozběhem), `emu_replay_seek`, `emu_replay_play_stop`, `emu_replay_status`; konec je pauza s důvodem `replay_end` nebo `replay_desync` (`emu_wait_stop`). |

Při přehrávání je v obraze **REPLAY PLAY** se snímkem a délkou záznamu.
Živý vstup (klávesnice, virtuální klávesnice, joystick hostitele, klávesy
z MCP) se ignoruje a okna kazety jsou zamčená - kazetu ovládá záznam.
Breakpointy, trace, záznam zápisů do paměti, screenshoty i nahrávání videa
fungují jako obvykle; pauza a krokování výsledek nemění.

Na konci se emulace pozastaví a oznámení ukáže výsledek:

| Výsledek | Význam |
|----------|--------|
| ended | záznam se přehrál do konce, všechny hashe souhlasily |
| differs at frame N | stav se od záznamu liší; oznámení vyjmenuje části (`cpu`, `ram`, `memext`, `vram`, `pcg`, `periph`, `time`) |
| deviated | během přehrávání něco zapsalo do stroje (zápis debuggeru, akce breakpointu `poke` / `set`, Freeze Bytes, reset); přehrávání doběhlo do konce, ale hashe se od té chvíle neověřovaly |

Rozdíl znamená, že se emulovaný počítač nechová jako při záznamu: jiná
verze nebo nastavení emulátoru, nebo vstup, který se nezaznamenává (viz
níže).

Přehrání nezačne, když je port joysticku v záznamu připojený a tady ne
(nebo naopak), nebo když je CMT hack v jednom z nich zapnutý a ve druhém
vypnutý: nastavení změnit a přehrát znovu.

## Checkpointy, návrat a skok

Při záznamu i přehrávání si emulátor drží checkpointy: každých 250
snímků (na PAL 5 sekund) snapshot stroje v paměti, posledních 60 (5
minut). Checkpoint zabere desítky kB; nabídka **Replay** ukazuje, kolik
jich je a pro které snímky.

| Akce | Menu / klávesa | MCP |
|------|----------------|-----|
| Návrat při záznamu | **Replay -> Rewind**, `Alt + Shift + J` | `emu_replay_rewind` |
| Návrat při přehrávání | **Replay -> Rewind**, `Alt + Shift + J` | `emu_replay_rewind` |
| Skok na snímek při přehrávání | **Replay -> Seek...** | `emu_replay_seek` |

**Návrat při záznamu** vrátí stroj na předchozí checkpoint a zahodí vše,
co se po něm nahrálo; další volba Rewind jde o checkpoint dál. Emulace
tam zůstane stát a záznam pokračuje od toho místa, jakmile se znovu
rozběhne - stačí zkusit jinou cestu, uložený záznam obsahuje jen tu
poslední. Soubor záznamu počítá návraty (`rerecord_count`).

**Skok při přehrávání** přejde na snímek: dopředu přehrávání běží
maximální rychlostí a na tom snímku se pozastaví; zpět začne od
nejbližšího checkpointu (nebo od začátku záznamu) a doběhne na snímek.
Rychlost se pak vrátí na předchozí nastavení. Hashe se ověřují celou
dobu, breakpointy zůstávají aktivní. Během skoku je v obraze **REPLAY
SEEK** a cílový snímek. Návrat při přehrávání skočí na předchozí
checkpoint.

Checkpoint nenese obsah médií. Když emulovaný program zapíše na disketu,
Quick Disk nebo disk IDE8, nahraje na kazetu nebo se páska vysune,
checkpointy před tím už použít nejde: návrat přes takový zápis se při
záznamu odmítne (soubory disků uživatele se nikdy nepřepisují), skok přes
něj při přehrávání začne přehrávání od začátku s čistými kopiemi médií.
Při běžícím autotype ani při nahrávání na kazetu checkpoint nevznikne.

Checkpointy jsou jen v paměti, ne v souboru záznamu. Záznam přehrávaný
v novém procesu si během přehrávání sbírá vlastní; skok před první z nich
začne od začátku.

Interval a počet jsou v sekci `[REPLAY]` konfigurace (čísla jsou tam
šestnáctková jako všude v konfiguračním souboru):

```
[REPLAY]
checkpoint_interval = 0xFA
checkpoint_count = 0x3C
```

`checkpoint_interval = 0` checkpointy vypne. Přes MCP jdou obě hodnoty
nastavit pro jeden záznam nebo přehrávání (`checkpoint_interval`,
`checkpoint_count` u `emu_replay_record_start` a `emu_replay_play`).

## Okno Replay Control

**Replay -> Replay Control...** (`Alt + J`) otevře plovoucí okno, které
může zůstat otevřené i při hraní: stav (vypnuto, záznam, přehrávání,
skok), soubor, snímek a čas (při přehrávání i s délkou záznamu), počet
událostí a checkpointů, návraty, odchýlení a poslední výsledek. Jeho
tlačítka spustí záznam nebo přehrávání, ukončí ho a vrátí na checkpoint;
pole Seek přesune přehrávání na snímek. Klávesy jsou vypsané vedle
tlačítek. Okno, které bylo otevřené, se po novém spuštění emulátoru
otevře znovu.

## Média

Při startu záznamu se do souboru záznamu uloží obsah všech vložených
médií:

| Médium | Uloží se |
|--------|----------|
| diskety (všechny mechaniky FDC i druhého řadiče FDC1) | obraz tak, jak ho drží emulátor, včetně změn dosud nezapsaných do souboru |
| Quick Disk | obraz (i obraz loaderu Unicardu) |
| disky IDE8 | soubor obrazu |
| kazeta | soubor pásky, aktuální blok, rychlost pásky, vlastní pulzy, polarita a rychlosti bloků |
| soubory vybrané v dialogu CMT hacku | každý vybraný soubor |

Přehrání nasadí kopie těchto médií - i v novém procesu bez původních
souborů - a pracuje nad nimi: emulovaný program může zapisovat na disk
nebo nahrávat na kazetu, původní soubory se nikdy nezmění a nastavení
emulátoru (INI) zůstane, jak bylo. Mechaniky, které byly při záznamu
prázdné, jsou při přehrávání prázdné. Po přehrání se vrátí média podle
nastavení; vložená páska se vloží znovu od začátku.

Uložená média jednoho záznamu mohou mít dohromady nejvýš 64 MB. Větší
obraz (typicky disk IDE8) se uloží jen jako hash SHA-256 a záznam dostane
varování; přehrání pak potřebuje vložený stejný obraz (ověří hash, jinak
odmítne začít) a pracuje nad jeho kopií.

## Ověření záznamu v CI

```
mz800emu --headless --replay run.mzr --replay-verify --result-json result.json
```

`--replay-verify` přehraje záznam v MAX SPEED a na jeho konci ukončí
proces: návratový kód 0, když všechny hashe souhlasily (`reason:
"replay_end"`), 4 při rozdílném stavu (`replay_desync`,
`replay.outcome_frame` a `replay.desync_parts` v `result.json`), 3 když
záznam nejde spustit (nečitelný soubor, jiná platforma nebo TV norma, jiné
nastavení joysticku nebo CMT hacku, neuložený obraz, který se liší).
Viz [`mcp-server/headless-mode.md`](mcp-server/headless-mode.md).

Pozor: oprava emulátoru, která změní časování, výsledek legitimně změní -
rozdíl na novém buildu znamená "chování se změnilo", ne nutně chybu.

## Co se nezaznamenává

| Vstup | Co dělat |
|-------|----------|
| vložení nebo vysunutí média během záznamu | během záznamu není k dispozici (menu Devices je zakázané, MCP ho odmítne); média vložit před záznamem |
| SD karta Unicardu (adresář hostitele) | přehrávat se stejným obsahem SD karty; záznam dostane varování, když je Unicard připojený |
| Quick Disk ve virtuálním (adresářovém) režimu | přehrávat se stejným adresářem; záznam dostane varování |
| nastavení emulátoru mimo snapshot (INI) | přehrávat se stejným nastavením |

## Soubor záznamu

Soubor `.mzr` je ZIP archiv:

| Soubor | Obsah |
|--------|-------|
| `manifest.json` | formát, verze, platforma, délka snímku TV normy (`screen_ticks`), délka záznamu, interval hashů, počet návratů při záznamu (`rerecord_count`), varování, seznam médií (`media`: slot, soubor, velikost, SHA-256, zda je uložené), stav kazety (`cmt`), čas startu hodin Unicardu (`rtc_base`), typy joysticků (`joystick_types`) |
| `start.mzs` | startovní snapshot |
| `inputs.jsonl` | jedna událost na řádek: snímek `f`, poloha `at` (`b` hranice snímku, `d` zápis debuggeru za běhu, `p` pauza na hranici snímku), druh `k` (`kbd`, `joy`, `autotype`, `reset`, `cmt`, `cmthack`, `dbg`) a jeho data; poslední řádek je `end` |
| `hashes.csv` | `frame,ticks,cpu,ram,memext,vram,pcg,periph` |
| `media/<slot>.bin` | obsah uloženého média (`fdc0.0`, `qd`, `ide8.0`, `cmt`, ...) |

`inputs.jsonl` je text: událost jde odebrat nebo posunout a zjistit, co
klávesa změnila. Hashe pak ukážou, kde se běh liší.
