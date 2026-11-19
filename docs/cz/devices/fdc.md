# FDC (Floppy Disk Controller) - uživatelská dokumentace

FDC subsystém emuluje řadič disketových mechanik na bázi čipu WD279x
(Sharp používá kompatibilní MB8876A). Řadič je připojen na porty
0xD8-0xDF a obsluhuje až 4 disketové mechaniky (FDD0..FDD3). Pracuje
s obrazy disket ve formátu DSK (standardní CPC DSK i Extended CPC DSK).

Sharp HW invertuje datovou sběrnici mezi CPU a chipem - vrstva BUS
translation v emulátoru tuto inverzi zajišťuje, takže vnitřní logika
chipu pracuje s "true-bus" hodnotami podle datasheetu.

## Připojení radiče

Z menu HW & Devices / FD Controller lze řadič logicky odpojit nebo
připojit:

| Volba         | Význam                                                       |
|---------------|--------------------------------------------------------------|
| Not Connected | FDC se chová jako neosazený - porty 0xD8-0xDF vrací sběrnicový |
|               | "open bus" stav a UI mechanik je gray-out.                  |
| WD279x        | Standardní emulovaný řadič. Default.                         |

## Drive sloty (FDD0..FDD3)

Na rozdíl od QuickDisku, který je fyzicky jedna mechanika, FDC umí
adresovat čtyři nezávislé mechaniky (FDD0..FDD3). Každá mechanika má
vlastní mount stav, vlastní DSK obraz, vlastní R/O nastavení a vlastní
storage mode.

V menu HW & Devices / FD Controller / "FDD N" jsou pro každou mechaniku
dostupné položky:

- **Mount...** - otevře file chooser pro výběr DSK obrazu (klávesová
  zkratka Alt+N, kde N je číslo mechaniky)
- **Re-Mount...** - dostupné, pokud je už něco namountováno
- **Recent** - podnabídka s nejvýš 12 naposledy vloženými obrazy DSK
  (jméno souboru, celá cesta v tooltipu); klik vloží obraz do této
  mechaniky stejně jako výběr v **Mount...**. Chybějící soubor ohlásí až
  vložení. **Clear recent media** na konci smaže historii všech druhů
  médií (viz overlay-menu.md, "Historie Recent media")
- **Umount** - odpojí obraz a vyčistí persistentní záznam v INI
  (Alt+Shift+N)
- **Read-only** - per-drive R/O toggle (viz "Read-Only 3-state model");
  zakázaný, dokud je tentýž obraz vložený v jiné mechanice, která do něj
  smí zapisovat (viz "Stejný obraz ve více mechanikách")
- **Storage mode** - per-drive volba (Cached / Direct / Discard)
- **Sync now** - vyžádaný flush RAM bufferu do souboru (aktivní jen
  v Cached módu s pending změnami)

V menu se název mechaniky zobrazí jako "FDD 0 (Empty)" / "FDD 0", při
namountovaném obrazu se v rozbaleném podmenu zobrazí basename souboru
(zkrácený na 20 znaků + "..." pokud je delší).

## Disky z příkazové řádky

Disk jde vložit už při startu emulátoru:

```
mz800emu --fdd 0=boot.dsk --fdd 1=data.dsk
mz800emu --fdd0 boot.dsk --fdd-readonly 0
```

Mechaniky se číslují přes oba řadiče: 0-3 = FDD0-FDD3 řadiče FDC (porty
0xD8 - 0xDF), 4-7 = FDD0-FDD3 sekundárního řadiče FDC1 (porty 0x58 -
0x5F). `--fdd <N>=<soubor>` jde opakovat, `--fdd0` až `--fdd7` jsou
zkrácené tvary téhož. Disk se vloží stejně jako z menu: nahradí disk
z INI ve stejné mechanice a jeho cesta se při ukončení uloží do INI
(`--no-save-ini` uložení vypne). Storage mode mechaniky platí z INI.

`--fdd-readonly <N>` chrání mechaniku N proti zápisu jen pro toto
spuštění (jako user_readonly, viz "Read-Only 3-state model", ale bez
zápisu do INI); platí i pro disky vložené do mechaniky později z menu.

Chybějící soubor, číslo mechaniky mimo 0-7 nebo mechanika zadaná dvakrát
ukončí emulátor s chybou ještě před startem. Disk pro odpojený řadič
(FDC1 je ve výchozím stavu odpojený) nebo obraz, který nejde otevřít,
je chyba: v `--headless` emulátor skončí s kódem 3, v okně zůstane
emulace v pauze a chyba se vypíše na chybový výstup (stderr).

## Storage mode (per drive)

Volba se persistuje per mechanika v INI klíči
`wd279x_fddN_storage_mode`. Default je `cached`.

| Mode    | Co se děje                                                                |
|---------|---------------------------------------------------------------------------|
| CACHED  | Celý DSK obraz se načte do RAM. Změny se drží v RAM bufferu, do souboru   |
|         | se zapíšou až při HW reset, umount, exitu emu nebo manuálním "Sync now".  |
|         | Default. Nízká I/O režie, riziko ztráty změn při crashi.                  |
| DIRECT  | Žádný RAM buffer, každý zápis jde přímo na soubor přes file driver. Vyšší |
|         | I/O režie, ale write je perzistentní okamžitě.                            |
| DISCARD | DSK obraz se načte do RAM, všechny zápisy končí JEN v RAM bufferu. Při    |
|         | umount / exit / reset se NIC nezapíše zpět. Pro testovací běhy bez        |
|         | modifikace zdrojového souboru.                                            |

### Přepnutí storage mode za běhu

Z menu Storage mode (radio Cached / Direct / Discard) lze přepnout mode
kdykoliv. Přepnutí pro mountovaný drive provede remount s novou volbou
storage_mode.

Pokud přepínáte z DISCARD do CACHED nebo DIRECT a v RAM bufferu jsou
pending změny (= byly provedeny zápisy, které by se jinak zahodily),
otevře se modální popup "FDC: Unsaved RAM changes" se třemi volbami:

1. **Save and switch** - vynuceně zapiš RAM buffer do souboru a teprve
   pak přepni mode. Do souboru se nezapíše nic, když ho se zápisem drží
   jiná mechanika (viz "Stejný obraz ve více mechanikách") nebo když
   mechanika odkazuje na jiný soubor, než ze kterého obraz načetla (viz
   "Snapshot kompatibilita"); emulátor to ohlásí zprávou a režim přepne
   bez uložení
2. **Switch and discard** - zahodit RAM změny a přepnout
3. **Cancel** - zrušit přepnutí

Při přepnutí mezi CACHED a DIRECT bez nepojených RAM změn, nebo do
DISCARD, se popup nezobrazí - přepnutí proběhne okamžitě.

## Read-Only 3-state model

Effective R/O = `user_readonly || fs_readonly`, nebo je obraz sdílený
s jinou mechanikou (viz "Stejný obraz ve více mechanikách").

| Pole          | Význam                                                                 |
|---------------|------------------------------------------------------------------------|
| user_readonly | Persistentní user volba. INI klíč `wd279x_fddN_readonly`. Mirror v UI  |
|               | jako "Read-only" checkbox v menu mechaniky.                            |
| fs_readonly   | Runtime auto-detekce: zjišťuje se přes test write access (W_OK) při    |
|               | mount. Pokud k souboru nelze získat write access, je 1.                |
| readonly      | Effective hodnota. Když je 1, chip odmítá WRITE SECTOR / WRITE TRACK   |
|               | (host vidí write protect status).                                      |

`fs_readonly` se přepočte při každém mount / remount. `user_readonly`
se mění výhradně user akcí přes UI checkbox nebo INI při startu. Pokud
je obraz FS write-protected, je v menu místo standardního checkboxu
zobrazena disabled položka "Read-only (FS write-protected)" s pevně ON
- persistent user pref se touto cestou nemění.

Při umount nebo zavření handleru se všechny tři příznaky vynulují
(mount cycle z čistého stavu).

## Stejný obraz ve více mechanikách

Tentýž soubor DSK jde vložit do více mechanik (i přes oba řadiče). Aby se
soubor obrazu nepoškodil, **smí do jednoho souboru v každém okamžiku
zapisovat jen jedna mechanika**. Mechaniky se číslují přes oba řadiče
jako v příkazové řádce: FDD 0-3 patří FDC0, FDD 4-7 patří FDC1.

- Mechanika, která soubor drží jako první, si zápis ponechává. Každá
  další mechanika dostane obraz **chráněný proti zápisu**: emulovaný
  počítač vidí disketu chráněnou proti zápisu a z této mechaniky se do
  souboru nikdy nic nezapíše.
- Při vložení emulátor ukáže zprávu "The image is already inserted in
  FDD n. It was inserted write-protected here to keep the image file
  safe." Bez okna (`--headless`, disky obnovené z INI souboru nebo zadané
  na příkazové řádce při startu, MCP, Unicard) jde zpráva jen na chybový
  výstup (stderr); MCP `emu_media_insert` ji navíc vrátí v poli
  `warning`.
- Menu mechaniky ukazuje pod jménem souboru "Shared with FDD n,
  write-protected" a položka **Read-only** je zaškrtnutá; overlay menu
  ukazuje stejnou poznámku v řádku mechaniky a **Write protect** je
  zapnutý.
- Dokud smí do souboru zapisovat jiná mechanika, ochranu nejde vypnout:
  **Read-only** i **Write protect** jsou zakázané a uvádějí důvod "The
  image is also inserted in FDD n, which may write to it."
- Když první mechanika obraz vysune, nebo sama dostane **Read-only**,
  jde ochranu druhé mechaniky vypnout. Sama se nevypne nikdy. Zápis se
  dá převést na jinou mechaniku: v první zapněte **Read-only** a ve druhé
  ho pak vypněte; první mechanika je potom zamčená stejným pravidlem.
- Ochrana patří k vloženému obrazu a do INI souboru se neukládá (klíč
  `wd279x_fddN_readonly` se nemění). Vložení jiného obrazu do mechaniky
  ji zruší. Při startu se disky z INI souboru vkládají v pořadí FDD 0 až
  7, takže zapisuje první mechanika v tomto pořadí a ostatní jsou
  chráněné, i když před ukončením emulátoru zapisovala jiná.

Tentýž soubor se pozná podle identity souboru, ne podle textu cesty:
absolutní cesta bez `.` a `..` (na Windows bez ohledu na velká a malá
písmena a na druh lomítka) a, pokud ho souborový systém vydá, číslo
souboru, které pokrývá pevné odkazy, symbolické odkazy a krátká jména
8.3. Nepozná se tentýž soubor dosažený přes dvě síťové cesty, pro které
systém vydá různá čísla souboru nebo žádná.

Chráněná mechanika v režimu Cached nebo Discard pracuje s kopií obrazu
načtenou při vložení: změny, které zapisující mechanika uloží později, se
v ní projeví až po novém vložení obrazu.

## Side / Density / Motor (HW logic porty)

WD279x čip má kromě registrů CMDSTS / TRACK / SECTOR / DATA (porty
0xD8-0xDB, za invertorem) i externí Sharp logic porty 0xDC-0xDF (před
invertorem):

| Port | Funkce                                                                 |
|------|------------------------------------------------------------------------|
| 0xDC | MOTOR / drive select - dolní 2 bity = ID aktivní mechaniky (0..3),    |
|      | bit 2 = EDR, bit 7 = motor on/off                                     |
| 0xDD | SIDE - výběr strany hlavičky (0 / 1)                                  |
| 0xDE | DENSITY - výběr hustoty záznamu (single / double)                     |
| 0xDF | EINT - HD Patch interrupt enable                                      |

Side a density jsou ovládané přímo emulovaným SW (Sharp DOS / CP/M
boot loadery, FDC ROM), nejsou to user-nastavitelné volby. Aktuální
hodnoty lze sledovat v FDC State debugger okně.

## HW volby FDC chipu

V menu HW & Devices / FD Controller je sekce "Hardware options" s těmito
přepínači. Změna platí hned, řadič čte obě volby za běhu; restart
emulátoru není potřeba.

| Volba                       | Význam                                                  |
|-----------------------------|---------------------------------------------------------|
| HD Patch enabled            | HD Patch obvod (port 0xDF EINT logika). INI klíč        |
|                             | `hd_patch`. Default ON - typický stav reálné MZ-800     |
|                             | sestavy s instalovaným HD Patch obvodem. Vypněte pro    |
|                             | raw / unpatched FDC.                                    |
| Bus translation: invert     | Sharp default - XOR 0xFF mezi CPU sběrnicí a chipem.    |
|                             | INI hodnota `bus_xlate=invert`.                         |
| Bus translation: passthrough| Experimentální - bez inverze. NEKompatibilní se Sharp   |
|                             | systémovým SW. Vyhrazeno pro budoucí "true bus" ROMy /  |
|                             | experimenty. INI hodnota `bus_xlate=passthrough`.       |

Obě HW volby lze přepsat z command line přes `--fdc-hd-patch=on|off`
a `--fdc-bus-xlate=invert|passthrough`. CLI override se persistuje do
INI při exitu.

## FDC State debugger window

Z menu HW & Devices / FD Controller / "FDC State (debugger)..." se otevře
okno s kompletním stavem FDC subsystému (k dispozici jen v debug build
emulátoru). Okno se refreshuje automaticky při každém ImGui rendru.

Sekce okna:

- **Connection**: stav řadiče (yes / no)
- **Registers (true-bus)**: STATUS, COMMAND, TRACK, SECTOR, DATA,
  MOTOR, SIDE, DENSITY, EINT - každý hex + binární reprezentace bitů
- **Transfer state**: data_counter (zbývajících bytů pro R/W),
  buffer_pos, current_sector_size, status_mode (Type I vs II/III),
  multiblock_rw, reading_status_counter, waitForInt (HD Patch INT
  throttle), positioned_track / positioned_side / positioned_sector
  (drive head position)
- **Sticky flags**: intrq_active, pending_busy_status, pending_drq,
  direction_latch (+1 = step in, -1 = step out)
- **Type III state (WRITE/READ TRACK)**: write_track_stage,
  write_track_counter, rt_sectors, rt_sector_bytes, rt_ssize_code,
  rt_cached_sec_idx
- **Drives**: per mechanika (FDD 0..3) - mount status, R/O, filename,
  storage mode, user_ro / fs_ro, geometrie (tracks, sides, total,
  image size v bajtech)

## Snapshot kompatibilita

Snapshot FDC subsystému ukládá:

- chip kontinuita: registry (STATUS, COMMAND, TRACK, SECTOR, DATA,
  MOTOR, SIDE, DENSITY, EINT), buffer_pos, data_counter, sticky flags,
  state machine pro WRITE TRACK / READ TRACK
- informativní info per drive: `filename`, `mounted` flag, effective
  `readonly` (při načtení slouží jen k porovnání s vloženými disketami,
  viz níže)
- informativní hodnoty config switchů `hd_patch` a `bus_xlate`

**Co se NEUKLÁDÁ:**

- Obsah DSK obrazu z RAM bufferu (= analogie QD: dirty RAM změny
  v CACHED nebo DISCARD se snapshot save+load **ztratí**. Pokud máte
  CACHED s pending změnami, doporučujeme před snapshot save manuální
  "Sync now")
- User INI nastavení (`storage_mode`, `user_readonly`, mount cesty) -
  INI je SSOT pro user pref; snapshot je SSOT pro chip kontinuitu

**Po snapshot load:** vložená disketa patří k emulátoru, ne ke stavu
emulovaného počítače. Načtení snapshotu diskety nevkládá, nevyměňuje ani
nevysouvá: každá mechanika si nechá obraz, který v ní byl před načtením,
i s jeho jménem souboru, ochranou proti zápisu, režimem ukládání a
neuloženými změnami; prázdná mechanika zůstane prázdná. Ze snapshotu se
obnoví jen stav řadiče (registry, poloha hlavy, motor, rozpracovaný
příkaz). Neuložené změny se dál ukládají do souboru, ze kterého byl obraz
vložen.

Pokud byly při uložení snapshotu vložené jiné diskety než teď (jiný
soubor, prázdná mechanika místo plné nebo naopak), emulátor to po načtení
ohlásí jednou zprávou, která vyjmenuje jen mechaniky s rozdílem:

```
The snapshot was saved with different disks:
FDD 0: game.dsk (now: work.dsk)
FDD 1: data.dsk (now: empty)
The inserted disks were kept.
```

Zpráva je společná s Quick Diskem: liší-li se i ten, přibude řádek
"Quick Disk: <jméno ze snapshotu> (now: <jméno nyní>)" (viz qdisk.md).
Jména jsou bez cest (celá cesta se uvede jen tehdy, když se soubory liší
pouze adresářem). Emulovaný program pak pokračuje nad jiným obsahem, než
s jakým počítal; chcete-li pokračovat přesně tam, kde snapshot vznikl,
vložte diskety uvedené ve zprávě a snapshot načtěte znovu. Opakované
načtení se stejným rozdílem (například Quick Load několikrát po sobě)
zprávu znovu neukazuje; v běhu bez okna jde zpráva na standardní chybový
výstup.

**Disk se neuloží do jiného souboru, než ze kterého byl načten.** Tuto
pojistku má řadič navíc: kdyby jméno souboru u mechaniky označovalo jiný
soubor, než ze kterého mechanika svůj obraz načetla, emulátor kopii
z paměti do souboru nezapíše (Sync now, reset, vysunutí, ukončení, "Save
and switch") a ohlásí "The disk in FDD n was not saved: the drive now
refers to a different image file than the one it was loaded from.
Re-insert the image to continue." Neuložené změny pak zůstávají jen
v paměti a při vysunutí, výměně obrazu nebo ukončení emulátoru se zahodí.
Po běžném načtení snapshotu k tomu nedochází. Nastat to může po přehrání
staršího záznamu vstupů, který nenese seznam médií, a po návratu zpět
v čase během nahrávání záznamu vstupů: tam emulátor mechanice vrací jméno
souboru ze snapshotu, aby přehrání odpovídalo nahrávce.

Ochrana proti zápisu se po načtení snapshotu řídí disketou, která
v mechanice skutečně je: volbou **Read-only**, ochranou souboru na disku
a sdílením obrazu s jinou mechanikou. Příznak `readonly` uložený ve
snapshotu se pro mechaniku s vloženým souborem nepoužije, takže načtení
snapshotu nevezme zápis mechanice, která má neuložené změny, a nezruší
ochranu sdíleného obrazu: i po načtení smí do jednoho souboru obrazu
zapisovat jen jedna mechanika.

## DSK formát a oprava vadných hlaviček

FDC podporuje DSK image (standardní CPC DSK i Extended CPC DSK).
Geometrie obrazu (počet stop, stran, celková velikost) se načítá při
mount; pokud načtení selže, mount se neprovede.

Některé HD DSK obrazy distribuované v Sharp MZ-800 komunitě mají vadný
`tsize` array v Extended CPC DSK hlavičce (deklarovaná velikost stopy
neodpovídá reálné velikosti). Emulátor provede automatickou opravu
hlavičky v paměti při mount v CACHED / DISCARD módu (oprava se hledá
podle `Track-Info` magic stringu na začátku každé stopy). Opravy se
propíší zpět do souboru jen v CACHED módu při sync.

V DIRECT módu se hlavička neopravuje (oprava by vyžadovala read/walk
celého obrazu přes file driver). Pokud používáte DIRECT mode, DSK
obrazy by měly mít korektní hlavičku.

## Známé limity

- **Header repair pouze v RAM módech** - v DIRECT módu se DSK hlavičky
  s vadným tsize array neopravují (viz výše).
- **Velikost sektoru max 512 B** - interní I/O buffer chipu je 512 B
  (= velikost HD sektoru). Postačí pro SD (256 B) a HD (512 B) sektory
  standardních MZ-800 formátů. 1024 B sektory nejsou pro MZ-800 běžné
  a v tomto bufferu by se nevešly.
- **Mount filename max 1023 znaků** - delší cesta není povolena
  (UI zobrazí "Sorry, filepath is too big").
- **Timing model není kompletní** - step rate, motor spin-up, head
  settling nejsou v aktuální verzi modelované.
- **Snapshot neukládá obsah DSK** - viz "Snapshot kompatibilita".

## Referenční INI klíče

Všechny klíče jsou v sekci `[FDC]`. Klíče s `N` v názvu existují
v sadě N = 0..3 (per mechanika).

| Klíč                            | Typ  | Default  | Význam                                  |
|---------------------------------|------|----------|-----------------------------------------|
| `hd_patch`                      | BOOL | 1        | HD Patch obvod (port 0xDF EINT logika)  |
| `bus_xlate`                     | TEXT | "invert" | "invert" / "passthrough"                |
| `wd279x_fddN_dskpath`           | TEXT | ""       | Cesta k DSK obrazu pro mechaniku N      |
| `wd279x_fddN_readonly`          | BOOL | 0        | user_readonly pro mechaniku N           |
| `wd279x_fddN_storage_mode`      | TEXT | "cached" | "cached" / "direct" / "discard"         |
