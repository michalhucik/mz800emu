# Virtual CMT (kazetofon) - uživatelská dokumentace

Virtual CMT emuluje kazetofon počítače. Program v emulovaném počítači čte
signál pásky stejně jako ze skutečné kazety a to, co počítač zapisuje na
kazetu, se nahraje na pásku v kazetofonu. Páska je v emulátoru seznam
bloků; vzniká ze souboru (TMZ, TZX, MZF, MZT, TAP, WAV) a dá se upravovat
a uložit.

Okno se otevře z menu **HW & Devices > CMT > Show Virtual CMT** nebo
klávesou Alt+C. Popisky ovládacích prvků jsou v tomto textu uvedené tak,
jak je program ukazuje anglicky.

## Rychlý start

### Nahrát program ze souboru

1. Otevřete okno Virtual CMT (Alt+C).
2. Tlačítkem **Open...** (bez pásky **Open tape...**, zkratka Ctrl+O)
   vyberte soubor pásky, například `game.mzf` nebo `games.tmz`. Páska je
   v kazetofonu a stojí (Stopped; v režimu Direct displej ukazuje Direct
   load).
3. Zvolte režim načítání vpravo vedle **Loading** (viz Režim načítání):
   - **Tape** - signál pásky se přehrává ve skutečném čase, jako ze
     skutečného kazetofonu; funguje s každým zavaděčem,
   - **Boost** - totéž, ale emulátor běží nejvyšší rychlostí, dokud běží
     motor kazetofonu,
   - **Direct** - program se zkopíruje do paměti okamžitě bez přehrávání
     signálu; funguje jen pro programy, které se nahrávají rutinami ROM.
4. V počítači zadejte příkaz pro nahrání (například nahrávání
   z monitoru, na MZ-800 klávesa C v bootovací nabídce, nebo `LOAD`
   v BASICu). Nahrávací rutina ROM, které se motor kazetofonu nepodaří
   rozjet, vypíše výzvu PLAY a čeká. Program s vlastními rutinami pro
   kazetu se nahraje jen v režimu Tape nebo Boost: například kazetové
   BASICy 1Z-016 a 1Z-013B čtou pásku samy a v režimu Direct jejich `LOAD`
   čeká a páska se nerozjede (viz CMT hack a páska).
5. V režimu Tape nebo Boost stiskněte **Play** (Ctrl+Space). Páska hraje,
   dokud ji program nezastaví vypnutím motoru; displej ukazuje Playing,
   po vypnutí motoru Waiting for motor. V režimu Direct je Play zakázané:
   program se nahraje rovnou z bloku, na kterém páska stojí.

Páska s více programy: další program nahraje další příkaz pro nahrání.
Konkrétní blok vyberete v seznamu bloků (tlačítko **Blocks**) dvojklikem
(pustí ho), **Ctrl+Enter** (jen na něj pásku přesune) nebo položkou **Load
directly** v kontextovém menu (přepne do režimu Direct a vybere ho).

Naposledy vložené pásky nabízí hlavní menu v podnabídce **HW & Devices >
CMT > Recent** (nejvýš 12, celá cesta v tooltipu): klik pásku otevře
stejně jako **Open...** v okně Virtual CMT, včetně dotazů na neuložené
změny a běžící nahrávání.

Rychlejší cesta z hlavního menu: **HW & Devices > CMT > Fast Open CMT
File & Play...** otevře soubor pásky a hned ho pustí (Play). Páska pak
hraje od chvíle otevření; nahrávání přes ROM, které najde motor už
běžící, výzvu PLAY nevypíše a čte pásku od místa, kde právě je.
V režimu Tape a Boost ji proto použijte, až počítač na pásku čeká (po
příkazu pro nahrání), nebo v režimu Direct.

### Nahrát program uložený počítačem a získat soubor

V režimu Direct se uložení rutinami ROM (například uložení z monitoru)
bez Record hned uloží jako nový blok na konec pásky (Direct Save, viz CMT
hack a páska); pokračujte krokem 4. Se stisknutým Record se vše, co
počítač ukládá, nahrává jako signál, v každém režimu načítání. Record je
potřeba v režimu Tape a Boost, u programů s vlastními rutinami pro kazetu
(například kazetové BASICy 1Z-016 a 1Z-013B) a s vypnutým Direct Save.

1. Stiskněte **Record** (Ctrl+R). Funguje v každém režimu načítání. Bez
   pásky se nahrává na novou prázdnou pásku (zobrazenou jako
   `(new tape)`), s páskou se záznam přidá za její poslední blok.
2. V počítači spusťte uložení (například `SAVE` v BASICu). Nahrávání
   začne, až program zapne motor kazetofonu.
3. Po uložení stiskněte **Stop recording** (to samé tlačítko, Ctrl+R) nebo
   **Stop**. Řádek zpráv ohlásí výsledek, například `Recorded "NAME",
   4096 B`, a páska stojí na prvním nahraném bloku.
4. Program uložte do souboru: **Tape > Export block as MZF...** uloží
   aktuální blok jako soubor MZF; **Tape > Save as...** uloží celou pásku
   (TMZ, TZX, MZT nebo MZF).

Program, který se nenahrál ve standardním formátu ROM bez chyby, zůstane
na pásce jako surový záznam (viz Nahrávání); programy v něm najde nástroj
Analyze (viz Analýza záznamu).

## Okno Virtual CMT

### Rozvržení

Okno má dvě rozvržení:

- **Compact** - jen kazetofon: páska, počítadlo, tlačítka, režim
  načítání a rychlost. Stačí na vložení pásky a nahrání programu.
- **Full** - kazetofon, pod ním seznam bloků pásky a vlastnosti
  vybraného bloku. Pro práci s páskou s více programy, úpravy, nahrávání
  a export.

Rozvržení se přepíná tlačítkem **Blocks** (ve Full je zamáčknuté; bez
pásky je zakázané) nebo v menu **View > Compact / Full**. Každé
rozvržení si pamatuje svou velikost okna. Výchozí výška rozvržení Full se
při výchozím písmu vejde na obrazovku vysokou 768 pixelů a ukáže aspoň
pět řádků seznamu bloků. Nejmenší velikost okna se počítá z délky textů,
aby se neořízly.

Titulek okna nese jméno pásky a hvězdičku, když páska má neuložené změny
(`Virtual CMT - games.tmz *`); dlouhé jméno se v titulku zkrátí třemi
tečkami.

Dialogy okna (potvrzení mazání, dotaz na neuložené změny, editor délek
pulzů, nástroje nad páskou) se otevírají uprostřed okna Virtual CMT.

### Kazetofon

Shora dolů:

1. **Páska**: jméno souboru (dlouhé jméno se zkrátí uprostřed, celá
   cesta je v tooltipu), u pásky s neuloženými změnami slovo **Modified**,
   vpravo tlačítka **Open...**, **Eject** a **Blocks**. Bez pásky je tam
   **No tape** a tlačítka **Open tape...**, **New tape** a zakázané
   **Blocks**.
2. **Displej**: vlevo velké počítadlo, vpravo číslo bloku (`Block 2 / 5`),
   jméno bloku (blok bez jména ukazuje svůj druh, například `Raw
   recording`), průběh aktuálního bloku a pod ním stav kazetofonu
   a motoru.
3. **Tlačítka**: **Rewind** (na první přehratelný blok, stav přehrávání
   se nemění), **Previous block**, **Play / Pause** (jedno tlačítko
   ukazuje akci, kterou provede), **Stop**, **Next block** a vpravo
   **Record** (při nahrávání **Stop recording**). Previous block a Next
   block přeskakují řídicí bloky a bloky, které nejde přehrát. Zakázané
   tlačítko má v tooltipu důvod, například `Stop the tape first.`.
4. **Signal speed** (výchozí rychlost signálu, viz Rychlost) a vpravo
   **Loading** (režim načítání, viz Režim načítání). U bloku s vlastní
   rychlostí (ze souboru nebo nastavenou přes **Set speed**) je místo výběru v zašedlém rámečku jeho rychlost
   s poznámkou `(set in file)`, u surového záznamu `(recorded signal)`,
   v režimu Direct `Not used in Direct mode`.

Okno čísluje bloky od 1 (displej, seznam bloků, zprávy). Nástroje MCP
číslují bloky od 0: blok 3 v okně je v MCP blok 2.

### Počítadlo

Počítadlo ukazuje čas od začátku pásky (**Elapsed**); kliknutí na něj
přepne na čas do konce pásky (**Remaining**) a zpět. Oba časy potřebují
znát délky bloků. Délky bloků, které ještě nehrály, emulátor dopočítává
jen při zastavené pásce, takže do té doby počítadlo ukazuje čas
v aktuálním bloku (**Block time**) a **Remaining** nejde zapnout. Při
nahrávání ukazuje dobu nahrávání (**Recording time**) a místo průběhu
počet nahraných pulzů, i v režimu Direct. Bez pásky a v režimu Direct
(mimo nahrávání) ukazuje `--:--.-`.

### Stav kazetofonu a motor

Stav kazetofonu (vždy text i barva):

| Stav | Kdy |
|------|-----|
| **No tape** | v kazetofonu není páska |
| **Stopped** | páska je vložená a stojí |
| **Playing** | páska hraje a motor běží |
| **Waiting for motor** | přehrávání nebo nahrávání čeká, až program zapne motor kazetofonu |
| **Paused** | pauza tlačítkem Pause |
| **Recording** | nahrávání a motor běží |
| **Direct load** | režim načítání Direct (má přednost před ostatními stavy, i před No tape; při nahrávání kazetofon ukazuje Recording, Waiting for motor nebo Paused jako v ostatních režimech) |

Vedle stavu je **Motor on** nebo **Motor off**, když emulátor stav motoru
zná (ne u zastavené pásky, bez pásky a v režimu Direct mimo nahrávání).

Motor kazetofonu zapíná a vypíná program v počítači. Datarekordéry
MZ-821 a MZ-1T04 tlačítko pauzy nemají; kazetofon v emulátoru ho má
navíc, aby šlo pásku podržet nezávisle na programu. Pauza tlačítkem
**Pause** je na motoru nezávislá: dokud pauza trvá, povely programu pro
motor pásku nerozjedou. **Play** pauzu zruší a motor nechá tak, jak ho
nastavil program: když program motor vypnul, páska dál čeká na motor
(Waiting for motor) a rozjede se, až ho program zapne. Pauza
a pokračování nad páskou čekající na motor ji tedy nechají čekat. Pásku
čekající na motor rozjede program zapnutím motoru, nebo **Stop** a **Play**
(aktuální blok pak hraje od začátku). Nahrávání čekající na motor (program,
který motor sám nezapne) rozjede **Play**.

### Seznam bloků

Seznam bloků je tabulka v rozvržení Full; ukazuje jen celé řádky
a posouvá se po celých řádcích. Výchozí sloupce: značka aktuálního bloku
(modrý trojúhelník), **#**, **Name**, **Type**, **Length** a **Status**.
Sloupce **Size**, **Load**, **Exec** a **Speed** jsou skryté; zapínají
se v menu **View > Columns** nebo pravým tlačítkem na hlavičce tabulky.
Šířku sloupce jde změnit tažením hranice v hlavičce, emulátor si ji
pamatuje; **View > Reset column widths** nastaví šířky podle obsahu
aktuální pásky.

Sloupec **Type** ukazuje u bloku s programem typ souboru z jeho hlavičky
číslem, u známých typů se slovem: `01 machine code`, `02 BASIC program`,
`03 data`, `04 BASIC data`, `22 CP/M file`; ostatní typy jen číslem.
Program BASICu může mít i typ `05`: s ním ukládají své programy například
kazetové BASICy 1Z-016 a 1Z-013B.

Sloupec **Status**:

| Text | Význam |
|------|--------|
| `OK` (ztlumeně) | blok hraje |
| `Not decoded` (ztlumeně) | surový záznam: signál, který emulátor nedekódoval (soubor WAV, nahrávka); hraje tak, jak se nahrál |
| `Unsaved` (oranžově) | blok ještě není v souboru pásky (nahraný nebo importovaný, i surový záznam nebo řídicí blok); po uložení pásky se ukáže jeho skutečný stav |
| `Cannot play` (červeně) | blok, který emulátor přehrát neumí; při přehrávání se přeskočí |
| popis, například `Loop start x3` | řídicí blok (smyčka, skok, zastavení pásky, informace); řádek je ztlumený, jmenuje se `Control block` a nejde přehrát |

Při nahrávání je na konci seznamu řádek `Recording...`. U prázdné pásky
je pod hlavičkou `This tape is empty.` s nápovědou.

Ovládání:

- Vybrané řádky mají modré pozadí a u levého okraje světlejší modrý pruh.
- **Klik** řádek vybere; páska se neposune (výběr se může lišit od
  aktuálního bloku). **Ctrl+klik** řádek k výběru přidá nebo ho z výběru
  ubere, **Shift+klik** vybere rozsah řádků a **Ctrl+A** vybere všechny
  bloky. **Šipky** posouvají výběr.
- **Dvojklik** pásku přesune na blok a pustí ho; je-li páska pozastavená
  tlačítkem Pause, přesune ji na začátek bloku a pauza zůstane.
- **Ctrl+Enter** přesune pásku na vybraný blok a stav přehrávání nemění.
- **Tažení řádku** myší a soubor přetažený nad seznam - viz Úpravy
  pásky.

Kontextové menu řádku (pravé tlačítko):

| Položka | Co dělá |
|---------|---------|
| **Play from here** | pustí blok od začátku (jako dvojklik) |
| **Go to this block** (Ctrl+Enter) | přesune pásku na začátek bloku, stav přehrávání nemění |
| **Load directly** | vybere blok a přepne do režimu Direct (viz Režim načítání); jen při zastavené pásce a u bloku s programem MZF |
| **Export as MZF...**, **Export as WAV...** | uloží blok do souboru (viz Uložení a export) |
| **Set speed** | rychlost bloku (viz Rychlost) |
| **Analyze...** | najde programy v surovém záznamu (viz Analýza záznamu) |
| **Convert to profile...** | sestaví signál bloku jiným profilem (viz Převod bloku na jiný profil) |
| **Move up** (Alt+Up), **Move down** (Alt+Down) | posune blok o jedno místo (viz Úpravy pásky) |
| **Import file here...** | vloží bloky souboru před tento řádek (viz Úpravy pásky) |
| **Delete** (Del) | smaže blok; při více vybraných blocích **Delete N blocks** (viz Úpravy pásky) |
| **Properties** | vybere jen tento řádek a rozbalí vlastnosti |

**Delete** smaže všechny vybrané bloky, je-li řádek menu mezi nimi, jinak
jen blok řádku menu; ostatní položky pracují s řádkem, na kterém se menu
otevřelo. Zakázaná položka má v tooltipu důvod.

### Vlastnosti

Pod seznamem bloků je sekce vlastností, výchozí sbalená; rozbalí se
kliknutím na její hlavičku nebo položkou **Properties** kontextového menu
(stav si okno pamatuje). Rozbalené vlastnosti dostanou tolik celých
řádků, kolik se vedle tří řádků seznamu vejde, zbytek se posouvá.

- **Block properties** (jeden vybraný blok) - podle druhu bloku
  například **Type**, **Size**, **Load address**, **Exec address**,
  **Speed**, **Long pulse (µs)** a **Short pulse (µs)** (horní / dolní
  půlvlna), **Length** a **Source**; u surového záznamu **Sample rate**
  a **Pulse count**.
- **Source**: `Recorded` - blok nahraný v emulátoru, dokud páska zůstane
  vložená; `Imported` - blok vložený importem souboru, dokud pásku
  neuložíte, nenačtete snapshot nebo nepřehrajete záznam vstupů; ostatní
  bloky podle souboru, ze kterého byla páska otevřena: `Imported from
  MZF` ze souboru MZF nebo MZT, jinak `From tape file`.
- **Tape properties** (bez výběru) - **File**, **Blocks**, **Total
  length**, **Format**; při nahrávání **Blocks** počítá i nahrávaný blok,
  stejně jako `Block 6 / 6` na displeji.
- Při více vybraných blocích jen jejich počet (`3 blocks selected`).

### Nabídky

Nabídka **Tape**:

| Položka | Co dělá | Kdy jde |
|---------|---------|---------|
| **New tape** | vysune pásku a vloží novou prázdnou | zastavená páska |
| **Open...** (Ctrl+O) | otevře soubor pásky (viz Pásky a soubory) | ne během nahrávání |
| **Import file...** (Ctrl+I) | vloží bloky jiného souboru na konec pásky, bez pásky do nové pásky (viz Úpravy pásky) | zastavená páska |
| **Save** (Ctrl+S) | uloží pásku do jejího souboru (viz Uložení a export) | zastavená páska |
| **Save as...** | uloží pásku do nového souboru: TMZ (výchozí), TZX, MZT nebo MZF | zastavená páska |
| **Export block as MZF...** | uloží program aktuálního bloku jako soubor MZF | zastavená páska, blok s programem |
| **Export block as WAV...** | uloží signál aktuálního bloku jako soubor WAV | zastavená páska |
| **Export tape as WAV...** | uloží celou pásku jako soubor WAV tak, jak by hrála | zastavená páska s aspoň jedním blokem |
| **Analyze recording...** | najde programy ve vybraném surovém záznamu (viz Analýza záznamu) | zastavená páska, jeden vybraný surový záznam |
| **Find files while playing** | zaškrtávátko: hrající surový záznam se analyzuje na pozadí (viz Analýza záznamu) | vždy |
| **Tape information...** | název, autor a další texty pásky a cílový stroj (viz Informace o pásce) | zastavená páska |
| **Check tape** | vypíše problémy ve struktuře pásky (viz Kontrola pásky) | vložená páska, v každém stavu |
| **Eject** (Ctrl+E) | vysune pásku | ne během nahrávání |

Během nahrávání záznamu vstupů jdou z nabídky Tape jen **New tape**,
**Eject** a **Check tape**, během přehrávání záznamu vstupů jen **Check
tape** (viz Záznam vstupů). Export jiného než aktuálního bloku je
v kontextovém menu jeho řádku.

Nabídka **View**: **Compact**, **Full**, **Columns** (sloupce seznamu
bloků; **Show all** zapne všechny najednou) a **Reset column widths**;
Columns a Reset column widths jen v rozvržení Full.

### Klávesové zkratky

Zkratky platí, když má okno Virtual CMT fokus. Zkratky okna jsou s Ctrl,
aby psaní na klávesnici nic nespustilo; šipky, Ctrl+A, Del a Alt+šipky
platí jen v seznamu bloků (po kliknutí do seznamu). Když má okno fokus,
klávesy do emulovaného počítače nejdou (klikněte do obrazu počítače).
Zkratka, jejíž akce teď není k dispozici, nic neudělá; zkratky úprav
pásky (Ctrl+I, Del, Alt+Up, Alt+Down) ukážou důvod jako varování v řádku
zpráv (například `Stop the tape first.`).

| Zkratka | Akce |
|---------|------|
| Ctrl+Space | Play / Pause |
| Ctrl+. | Stop |
| Ctrl+R | Record (při nahrávání Stop recording) |
| Ctrl+Home | Rewind |
| Ctrl+PageUp / Ctrl+PageDown | předchozí / další blok |
| Ctrl+O | Open... |
| Ctrl+I | Import file... |
| Ctrl+S | Save |
| Ctrl+E | Eject |
| šipky | výběr v seznamu bloků |
| Ctrl+A | výběr všech bloků v seznamu |
| Del | smazání vybraných bloků |
| Alt+Up / Alt+Down | přesun vybraného bloku o jedno místo nahoru / dolů |
| Ctrl+Enter | přesun pásky na vybraný blok v seznamu |
| Alt+C | zavře okno (otevře ho i z hlavního okna) |

Escape okno nezavírá.

### Zprávy

Výsledky akcí okna se ukazují v řádku zpráv dole v okně: výsledek
nahrávání (například `Recorded "NAME", 4096 B`), uložení pásky a exportu,
otevření pásky (`Opened games.tmz, blocks: 5`), import souboru
(`Imported prog.mzf, blocks: 1`), smazání bloků (`Deleted 2 blocks.`),
výsledky nástrojů nad páskou (`Files added from the recording: 2`, `Tape
information updated.`, `Block 3 converted to ROM-800-NOMINAL.`), změna
režimu načítání a odmítnutá akce. Poslední zpráva zůstává ztlumeně, dokud
ji nenahradí další; chyba je červená, varování oranžové. Zpráva, která se
nevejde, se zkrátí třemi tečkami (u cesty k souboru uprostřed, aby zůstalo
vidět jméno souboru) a celý text je v tooltipu.

Když je okno zavřené nebo sbalené, chyby a varování se ukážou v okně
zprávy (Error, Warning) a informace se zahodí (po otevření okna se
neukážou). Chyba při otevření pásky se ukazuje v okně zprávy vždy, stejně
jako neúspěšné uložení pásky při zavírání emulátoru.

Pásku může mezitím změnit i něco jiného než okno (například nástroj MCP
nebo konec pásky). Akce okna proto pracuje jen s páskou a stavem, které
okno ukazovalo, když jste ji zvolili:

- když se páska mezitím vymění (jiná páska, úprava přes MCP), akce se
  neprovede s varováním `The tape changed; the action was cancelled.`;
  otevřené kontextové menu řádku se zavře se stejným varováním,
- když se mezi kliknutím a provedením akce změní stav kazetofonu
  (například páska mezitím doběhla nebo se rozjela), akce se neprovede
  s varováním `The tape state changed; the action was cancelled.`.

Texty, které pocházejí z jádra emulátoru (důvody v analýze, nálezy
kontroly, popisy profilů a některá varování), jsou ve všech jazycích
emulátoru anglicky.

## Pásky a soubory

### Formáty

Každý soubor je páska s bloky - i samotný program MZF nebo nahrávka WAV
je páska s jedním blokem.

| Soubor | Bloky pásky | Uložení (Save, Save as...) |
|--------|-------------|----------------------------|
| `.tmz` (formát pásek Sharp MZ) | bloky souboru | uchová všechny bloky |
| `.tzx` (formát pásek ZX Spectrum) | bloky souboru | uchová všechny bloky; páska s bloky Sharp MZ se zapíše ve formátu TMZ (s varováním) |
| `.mzt` | jeden blok na každý program v souboru | jen programy (viz Uložení a export) |
| `.mzf`, `.m12` | jeden blok s programem | jen páska s jediným programem |
| `.tap` | jeden blok na každý blok TAP (hlavička nebo data) | emulátor do TAP neukládá |
| `.wav`, `.wave` | jeden blok s nahrávkou (PCM, IEEE float i WAVE_FORMAT_EXTENSIBLE) | emulátor do WAV neukládá pásku, jen exportuje signál (viz Uložení a export) |

Dialog pro otevření nabízí filtr `Tape (*.tmz *.tzx *.mzf *.m12 *.mzt
*.tap *.wav)`.

### Otevření pásky

**Open...** (Ctrl+O, v menu Tape i tlačítkem) otevře soubor pásky.
Soubor se nejdřív přečte a ověří: páska, kterou nejde otevřít
(chybějící, poškozený nebo nepodporovaný soubor), pásku v kazetofonu
nezmění - zůstane vložená včetně neuložených změn i probíhajícího
nahrávání a ohlásí se jen chyba. Soubor WAV, ve kterém se úroveň signálu
ani jednou nezmění, se neotevře ("WAV contains no signal").

Při otevření pásky vypíše emulátor varování do logu, když:

- import souboru narazil na problém,
- páska má bloky druhu, který emulátor přehrát neumí (například TZX 0x19
  nebo CSW s kompresí Z-RLE) - varování je vyjmenuje,
- páska nemá žádný přehratelný blok (nebo první blok nejde otevřít).

Blok podporovaného druhu, který přesto nedá signál (například s daty mimo
meze přehrávače), se ohlásí v logu až ve chvíli, kdy na něj přehrávání
dojde, a přeskočí se.

Po otevření pásky, jejímž jediným blokem je surový záznam, řádek zpráv
navrhne analýzu: `Opened recording.wav, blocks: 1. Use Analyze to find
files.`

Soubor přetažený z Průzkumníka Windows nad jinou část okna Virtual CMT
než seznam bloků (kazetofon, menu, vlastnosti, řádek zpráv; v rozvržení
Compact celé okno) se otevře jako páska, stejně jako přes **Open...**,
včetně dotazu na neuložené změny; platí to i pro sbalené okno. Když soubor
teď otevřít nejde, řádek zpráv ukáže důvod jako varování (během nahrávání
`Stop the recording first.`). Z více přetažených souborů se otevře jen
první (`Only one file can be opened as a tape.`). Soubor přetažený při
otevřeném dialogu nebo menu nebo během jiného dotazu k pásce se nepoužije
(`Finish the open dialog first.`). Soubor přetažený jinam do hlavního okna
emulátoru se nepoužije. Přetažení souboru je určené pro okno Virtual CMT
uvnitř hlavního okna emulátoru: okno Virtual CMT vytažené mimo hlavní okno
(jako samostatné okno) přetažený soubor přijmout nemusí - pak se nestane
nic.

**Kontrola velikosti MZF**: když je zapnutá položka **HW & Devices > CMT
> CMT Options > Enable MZF Size Check** (výchozí zapnutá) a otevírá se
soubor `.mzf` nebo `.m12` delší, než udává jeho hlavička (velikost
programu + 128 bajtů hlavičky), emulátor otevře okno **MZF Size Warning**
se jménem souboru a počtem přebývajících bajtů. Takový soubor vzniká
například exportem z CP/M. **Ignore** soubor nechá, **Truncate** zkrátí
původní soubor na správnou délku, **Save As** uloží zkrácenou kopii pod
novým jménem (jméno bez přípony `.mzf`, `.m12` nebo `.mzt` dostane
příponu `.mzf`). Zaškrtávátko **MZF size check enabled** v tomto okně
kontrolu vypne nebo zapne. Páska se otevře v každém případě. Stejná
kontrola proběhne, když CMT hack nahrává soubor MZF vybraný v dialogu
(viz CMT hack a páska).

Se zapnutou položkou **HW & Devices > CMT > CMT Options > Fix MZF Size
Automatically** (výchozí vypnutá; totéž co zaškrtávátko **Fix
automatically next time** v okně MZF Size Warning) se emulátor neptá:
původní soubor hned zkrátí na správnou délku a ohlásí to v řádku zpráv
okna Virtual CMT. Když soubor zkrátit nejde, otevře se okno MZF Size
Warning jako obvykle. Položka platí jen se zapnutou Enable MZF Size
Check.

### Bloky

- **Blok s programem** nese soubor MZF (z MZF, MZT, programové bloky TMZ,
  nahraný program). Jen takový blok jde exportovat jako MZF a nahrát
  v režimu Direct; analyzovat ho nejde.
- **Bloky TAP** (hlavička, data) jsou bloky pásky ZX Spectrum. Blok TAP
  s jiným bajtem příznaku než 00h / FFh se přijme.
- **Surový záznam** (Status `Not decoded`) je signál, který emulátor
  nedekódoval: soubor WAV nebo nahrávka, kterou emulátor nerozpoznal
  jako program. Hraje přesně tak, jak se nahrál.
- **Řídicí bloky** TMZ a TZX (smyčky, skoky, volání, skupiny, zastavení
  pásky, informační bloky) nemají signál: jsou v seznamu bloků, ale
  otevřít nejdou. Pásky TMZ a TZX se přehrávají i se smyčkami, skoky
  a voláními bloků.
- Blok, který přehrávač neumí přehrát (Status `Cannot play`), v seznamu
  zůstává a při přehrávání se přeskočí.

**Pauzy mezi bloky** dává sama páska: za programem MZF a MZT 1000 ms,
u TAP 350 ms za hlavičkou a 1000 ms za daty, za posledním blokem žádná;
u TMZ a TZX podle souboru.

**Zastavení pásky** (blok Pause 0 nebo blok "stop the tape" v TMZ / TZX)
kazetofon pozastaví: páska stojí na bloku za příkazem a čeká na motor
(Waiting for motor), dokud ho program znovu nezapne. Z okna ji rozjede
**Stop** a **Play** (aktuální blok hraje od začátku); tlačítko **Pause**
ji jen pozastaví (viz Stav kazetofonu a motor).

**Konec pásky**: kazetofon se zastaví (Stopped) a zůstane na posledním
bloku. S volbou `autorewind = 1` v sekci `[CMT]` souboru `.ini` se páska
místo toho převine na první blok (viz Nastavení v souboru .ini).

### Neuložené změny

Úpravy pásky v emulátoru mění pásku v paměti; soubor pásky se změní až
uložením (**Save**, **Save as...**). Páska, která se liší od svého souboru,
má neuložené změny: v okně slovo **Modified**, v titulku okna **\***.
Bloky, které ještě nejsou v souboru pásky, mají Status `Unsaved`.

Neuložené změny vzniknou nahráváním, importem, smazáním nebo přesunem
bloků, změnou rychlosti bloku (na jinou, než blok má), přidáním souborů
z analýzy, zápisem informací o pásce a převodem bloku na jiný profil.
Změna výchozí rychlosti (**Signal speed**), přehrávání, posun po pásce
a export pásku jako neuloženou neoznačí.

Páska s neuloženými změnami se ukládá do snapshotu a do záznamu vstupů
celá (viz Snapshot a záznam vstupů).

### Dotazy před ztrátou změn

Před akcí, která by zahodila nahrávání nebo neuložené změny - vysunutí,
otevření jiné pásky (i **Fast Open CMT File & Play...** v menu), **New
tape**, načtení snapshotu (**Load Snapshot...**, **Quick Load**, i jejich
klávesovými zkratkami), start přehrávání záznamu vstupů (**Play
Replay...**) nebo zavření okna emulátoru - se emulátor zeptá:

- během nahrávání: **Stop recording** (nahrávání se dokončí a přidá na
  pásku, pak následuje dotaz na neuložené změny), **Discard recording**
  nebo **Cancel**,
- při neuložených změnách: **Save tape**, **Discard changes** nebo
  **Cancel**. Po **Save tape** akce pokračuje, jen když se uložení
  povedlo.

V okně Virtual CMT jsou vysunutí, otevření a nová páska během nahrávání
zakázané (`Stop the recording first.`); dotaz během nahrávání tedy
ukážou akce z hlavního menu, snapshot, záznam vstupů a zavření emulátoru.

Má-li akce dialog pro výběr souboru (otevření pásky, načtení snapshotu,
přehrání záznamu vstupů), stav pásky se po výběru souboru zkontroluje
znovu: když mezitím začalo nahrávání nebo páska dostala neuložené změny,
emulátor se zeptá znovu. Zprávy, které přijdou během takového dotazu,
počkají a ukážou se po něm. Když je při zavření okna otevřené menu nebo
jiný dialog, dotaz se ukáže hned po jeho zavření.

Ukončení emulátoru bez okna (headless, přes MCP, automatizace) změny
zahodí s varováním v logu. Nástroje MCP a příkazová řádka se neptají:
vysunutí nebo otevření jiné pásky se provede (nástroj MCP pak vrátí
varování "Unsaved tape changes were discarded"). Chování během záznamu vstupů popisuje oddíl
Snapshot a záznam vstupů.

### Uložení a export

**Save** (Ctrl+S) uloží pásku do jejího souboru. U nové pásky, u formátu,
do kterého emulátor neukládá (WAV, TAP), nebo u formátu, který pásku
neunese (například otevřený soubor MZF s dalším nahraným blokem nebo
páska s informacemi o pásce otevřená z MZF), otevře místo toho **Save
as...**. **Save as...** uloží pásku do nového souboru: TMZ (výchozí),
TZX, MZT nebo MZF. Po uložení páska patří k novému souboru a nemá
neuložené změny.

Co který formát uchová:

- **TMZ** uchová všechny bloky, rychlosti, pauzy a informace o pásce.
- **TZX** také; páska s bloky Sharp MZ se zapíše ve formátu TMZ
  (s varováním).
- **MZT** nese jen programy: každý blok musí být program (takže ani
  informace o pásce). Neuchová pauzy mezi bloky (po znovuotevření
  1000 ms), rychlost bloku uloží jako délky pulzů UniCMT a neuloží
  rychlost prvního bloku jinou než 1:1 ani rychlost 2:1 CP/M (varování
  blok jmenuje; blok se uloží bez své rychlosti, takže po znovuotevření
  souboru hraje první blok výchozí rychlostí a pozdější blok rychlostí
  bloků před ním). Blok s výchozí rychlostí se do MZT zapíše jako 1:1, ne
  rychlostí podle **Signal speed**: po znovuotevření hraje výchozí
  rychlostí nastavenou v té chvíli; leží-li v souboru za blokem
  s rychlostí jinou než 1:1, hraje 1:1.
- **MZF** jen u pásky s jediným programem.

Uložení do TMZ a TZX zapíše každý blok rychlostí, kterou hraje, i blok
s výchozí rychlostí (rychlostí podle **Signal speed** v té chvíli): znovu
otevřený soubor hraje stejně jako páska před uložením, bez ohledu na
pozdější **Signal speed**. Vlastní délky pulzů se ukládají v celých
mikrosekundách (desetinné hodnoty se zaokrouhlí).

Soubor se ukládá přes dočasný soubor: když uložení selže (plný disk,
soubor jen pro čtení), původní soubor zůstane celý a páska si neuložené
změny ponechá. Totéž platí pro export.

**Export bloku**: **Tape > Export block as MZF...** a **Tape > Export
block as WAV...** pracují s aktuálním blokem, **Export as MZF...**
a **Export as WAV...** v kontextovém menu s blokem řádku. MZF jde jen
u bloku s programem; WAV (44,1 kHz) obsahuje signál bloku. Export pásku,
její polohu ani neuložené změny nemění.

**Export celé pásky**: **Tape > Export tape as WAV...** zapíše celou
pásku do souboru WAV (44,1 kHz, 8 bitů, mono) tak, jak by ji kazetofon
přehrál od začátku: bloky s pauzami, smyčky, skoky a volání, bloky
s vlastní rychlostí touto rychlostí. Bloky, které emulátor přehrát neumí,
se přeskočí s varováním; zastavení pásky (blok Pause 0, "stop the tape")
export nepřeruší. Páska delší než 4 hodiny se neexportuje. Přepínač
polarity na bloky ze souboru WAV v exportu nepůsobí. Dialog souboru
nabídne jméno pásky s příponou `.wav` (u pásky bez souboru `tape.wav`).
Když se páska během otevřeného dialogu souboru změní nebo rozjede, nic se
neexportuje (viz Zprávy). Páska, která nedá vůbec žádný signál (například
jen řídicí bloky), se neexportuje (`The tape gives no signal, not
exported`); smyčka řídicích bloků bez signálu export ukončí s varováním;
blok, který nejde sestavit jeho rychlostí, export zastaví s chybou.

## Nahrávání

Virtual CMT nahraje to, co počítač zapisuje na kazetu (například příkaz
SAVE monitoru nebo BASICu), na pásku v kazetofonu, stejně jako skutečný
kazetofon. Postup je v Rychlém startu.

- **Record** (při zastaveném kazetofonu, v každém režimu načítání)
  zahájí nahrávání hned, s páskou v kazetofonu i bez ní. Bez pásky se nahrává na
  novou prázdnou pásku, zobrazenou jako **(new tape)**; s páskou se záznam
  přidá za její poslední blok. Kazetofon čeká, až program zapne motor
  kazetofonu (SAVE to udělá sám); doba se zastaveným motorem se
  nenahrává. U programu, který motor sám nezapne, rozjede nahrávání
  čekající na motor (Waiting for motor) tlačítko **Play**.
- Při nahrávání se vše, co počítač zapisuje, nahrává jako signál,
  v režimu Direct i uložení rutinami ROM: Direct Save platí jen bez
  Record (viz CMT hack a páska).
- **Stop recording** nebo **Stop** nahrávání ukončí. Emulátor záznam
  dekóduje a přidá na konec pásky:
  - program uložený bez chyby ve standardním formátu ROM se stane běžným
    blokem s programem (jde nahrát, exportovat a měnit mu rychlost jako
    bloku ze souboru MZF); SAVE z ROM dá přesně uložený program, hlavičku
    i tělo,
  - cokoli jiného (jiný formát, chyba, nahrávání zastavené uprostřed,
    hlavička bez programu) se stane **surovým záznamem**: signálem přesně
    tak, jak se nahrál. Přehraje se stejně, přepínač polarity ho
    neobrací a rychlost mu změnit nejde. Velmi krátký záznam (méně než
    64 pulzů signálu) žádný blok nepřidá.

  Výsledek se ohlásí v řádku zpráv (například `Recorded "NAME", 4096 B`)
  a kazetofon stojí na prvním novém bloku, připravený ho přehrát.
  Zastavení pásku nevysouvá. Nové bloky jsou `Unsaved` a páska má
  neuložené změny.
- **Automatické zastavení**: volba `rec_autostop_s` v sekci `[CMT]`
  souboru `.ini` (sekundy, 0 až 600, výchozí 0 = vypnuto) ukončí
  nahrávání jako Stop po tolika sekundách bez signálu při běžícím motoru
  kazetofonu, jakmile se už něco nahrálo. Nahrávání se samo zastaví také
  po 4 hodinách nebo 16 000 000 hranách signálu. Reset počítače nahrávání
  nezastaví.

## Úpravy pásky

Pásku v kazetofonu jde upravovat: mazat bloky, měnit jejich pořadí
a vkládat do ní bloky z jiného souboru. Mazání a přesun bloků jsou
v seznamu bloků (rozvržení Full), import souboru i v menu **Tape** (obě
rozvržení). Úpravy mění pásku v paměti a páska pak má neuložené změny
(viz Neuložené změny).

Pravidla pro všechny úpravy (i pro nástroje, které pásku mění):

- Upravovat jde jen **zastavenou pásku** (displej ukazuje Stopped,
  v režimu Direct ukazuje Direct load): ne při přehrávání, v pauze ani
  při nahrávání (`Stop the tape first.`), a ne během nahrávání nebo
  přehrávání záznamu vstupů. Zakázaná položka menu má důvod v tooltipu,
  zakázaná zkratka úprav v řádku zpráv.
- V režimu Direct úprava pásky zapomene hlavičku programu, kterou už
  načetl program v emulátoru: když program pak pokračuje v načítání těla,
  dostane přerušení načítání (Break) a tělo se nenačte; nové nahrávání
  začne hlavičkou bloku, na kterém kazetofon stojí.
- Čísla bloků za upraveným místem se posunou. Kazetofon zůstane na
  bloku, na kterém stál (od jeho začátku); když se ten blok smazal, na
  bloku, který přišel na jeho místo, nebo na posledním bloku. Je-li to
  řídicí blok nebo blok, který nejde přehrát, stojí kazetofon na nejbližším
  dalším přehratelném bloku (nebo na nejbližším předchozím); stála-li
  páska před úpravou za svým koncem, převine se na začátek.
- Blok s programem, jehož rychlost jde měnit (viz Rychlost bloku), a který
  nemá za sebou pauzu (typicky poslední blok importovaného souboru nebo
  nahrávky), dostane pauzu 1000 ms, když se za něj přesunem, importem nebo
  analýzou dostane další blok.
- Když po úpravě hraje některý blok programu (blok TMZ MZ File) jiným
  taktem počítače než předtím, protože se k němu vztahuje jiný informační
  blok TMZ o stroji (smazaný nebo přesunutý informační blok, program
  přesunutý nebo importovaný přes něj), řádek zpráv to ohlásí varováním
  `The tape clock setting now applies to different blocks; check their
  speed.` (u výsledku v závorce); zkontrolujte rychlost bloků. Úprava se
  i tak provede.
- Když se páska během potvrzovacího dotazu nebo před provedením úpravy
  vymění nebo změní stav, úprava se neprovede (viz Zprávy).

**Mazání**: **Delete** v kontextovém menu nebo klávesa **Del** smaže
vybrané bloky. Bloky uložené v souboru pásky smaže hned. Jsou-li mezi
vybranými neuložené bloky (Status `Unsaved`), emulátor se nejdřív zeptá:
`Delete this unsaved block? Its data is not stored anywhere else.`, když
je vybraný jediný neuložený blok, `Delete 2 unsaved blocks? Their data is
not stored anywhere else.`, když jsou neuložené všechny vybrané bloky,
`Delete blocks: 5. Unsaved: 2. Data
of the unsaved blocks is not stored anywhere else.`, když jen některé;
tlačítka **Delete blocks** a **Cancel**. Výchozí volba pro Enter je
**Cancel** a po otevření dotazu je zvýrazněná; Escape dotaz také zruší.
Výsledek ohlásí řádek zpráv (`Deleted block 3.`, `Deleted 2 blocks.`)
a vybraný je blok na místě prvního smazaného (nebo poslední blok). Výběr
z velmi mnoha oddělených skupin bloků najednou smazat nejde (`Too many
separate blocks selected.`); smažte ho po částech. Smazání všech bloků
nechá v kazetofonu prázdnou pásku (`This tape is empty.`); páska se
nevysune.

**Přesun**: **Move up** / **Move down** v kontextovém menu nebo
**Alt+Up** / **Alt+Down** v seznamu bloků posune blok o jedno místo;
přesouvá se jen jeden vybraný blok. Další stisk Alt+Up nebo Alt+Down
dřív, než předchozí přesun skončil, se neztratí: blok se posune ještě
jednou. Blok jde také přetáhnout myší na jiné místo seznamu: během
tažení ukazuje modrá čára mezi řádky, kam se blok vloží, číslo a jméno
bloku jsou vpravo pod kurzorem. U horního nebo dolního okraje seznamu se
seznam posouvá (čím blíž okraji, tím rychleji). Puštění mimo seznam nebo
Escape během tažení nic nepřesune; zkratky úprav během tažení nic
nedělají. Když se páska během tažení změní, tažení se zruší s varováním
`The tape changed; the action was cancelled.` a nic se nepřesune.
Přesunutý blok zůstane vybraný.

**Import souboru**: vloží do pásky všechny bloky jiného souboru pásky
(stejné formáty jako **Open...**, převedené stejně jako při otevření;
soubor WAV jako jeden blok s nahrávkou).

- **Tape > Import file...** (Ctrl+I) vloží bloky na konec pásky; bez
  pásky vytvoří novou pásku s importovanými bloky. Když import do
  prázdného kazetofonu selže, kazetofon zůstane prázdný.
- **Import file here...** v kontextovém menu řádku vloží bloky před tento
  řádek.
- **Přetažení souboru** (například z Průzkumníka Windows) nad seznam
  bloků vloží jeho bloky na místo pod kurzorem (modrá čára ukazuje kam),
  pod poslední řádek nebo nad hlavičku tabulky na konec pásky. Více
  přetažených souborů se importuje postupně v pořadí, v jakém je systém
  předá. Import se zastaví u prvního souboru, který nejde importovat:
  řádek zpráv ukáže jeho chybu s větou `The remaining files were not
  imported.` a soubory za ním se neimportují. Když import teď nejde
  (například páska hraje), řádek zpráv ukáže důvod jako varování; soubor
  přetažený při otevřeném dialogu nebo menu se nepoužije (`Finish the open
  dialog first.`). Soubor přetažený jinam do okna se otevře jako páska
  (viz Otevření pásky).
- Výsledek ohlásí řádek zpráv (`Imported prog.mzf, blocks: 1`) a vybraný
  je první importovaný blok. Importované bloky jsou `Unsaved`, dokud
  pásku neuložíte. Varování importu jsou ve zprávě v závorce (například
  když importované bloky nejdou přehrát). U více souborů zůstane varování
  dřívějšího souboru v řádku zpráv.
- Nový import během jiného ještě neskončeného importu se neprovede
  (`Another import is still in progress.`). Když výsledek importu nepřijde,
  import skončí zprávou `The import could not be completed.`.
- Na importovaný blok WAV působí přepínač polarity jako na otevřený soubor
  WAV jen do uložení (viz Polarita).

## Nástroje nad páskou

Čtyři nástroje okna Virtual CMT pracují s páskou v kazetofonu: **Analyze**
najde v surovém záznamu programy a udělá z nich bloky s programem, **Tape
information** zapíše do pásky název, autora a další texty, **Check tape**
vypíše problémy ve struktuře pásky a **Convert to profile** sestaví signál
bloku s programem jiným způsobem. Analýza (přidání souborů), informace
o pásce a převod mění pásku a platí pro ně pravidla úprav (viz Úpravy
pásky); nové nebo změněné bloky jsou `Unsaved`. Check tape pásku jen čte.

### Analýza záznamu

**Analyze...** v kontextovém menu řádku surového záznamu, nebo **Tape >
Analyze recording...** s tímto jediným vybraným řádkem, otevře dialog
`Analyze recording - block N`.

**Při hraní**: se zapnutou volbou **Tape > Find files while playing**
(výchozí, klíč `auto_analyze`) se surový záznam analyzuje na pozadí, hned
jak začne hrát, s výchozími volbami. Páska se tím nemění. Když záznam
dohraje nebo se páska zastaví a našly se soubory, které jde přidat, řekne
to řádek zpráv, například `Block 2: files found in the recording: 3. Use
Analyze recording... to add them to the tape.`; dialog se pak otevře rovnou
s výsledkem. Záznam se takto analyzuje jednou; neanalyzuje se při nahrávání
ani přehrávání záznamu vstupů a jen se zobrazeným oknem Virtual CMT.

Analýza začne hned a běží na pozadí: emulace i okno dál pracují, dialog
ukazuje `Analyzing...` a uplynulý čas. **Cancel**, Escape nebo zavření okna
Virtual CMT dialog kdykoli opustí; rozběhnutá analýza se opustí, doběhne na
pozadí a nic nezmění.

Výsledek: věta s počtem nalezených souborů (a kolik z nich má chybu
součtu), ztlumený řádek se jménem bloku, jeho délkou a počtem pulzů
a tabulka s jedním řádkem na soubor v pořadí záznamu: zaškrtávátko, **#**,
**Name**, **Type**, **Format** (`Sharp standard` pro standardní formát
příkazu SAVE z ROM, jinak jméno formátu, například `TURBO`), **Start** (čas
od začátku záznamu), **Size**, **Checksum** (`OK`, `Header error`, `Body
error`, `-`, když ho nejde ověřit) a **Result** (`Will be added`, `Will be
added (repaired)`, `Stays raw`, `Cannot add`). Úsek signálu o aspoň 64
pulzech bez souboru je řádek `Unknown signal`. Pro vybraný řádek řádek pod
tabulkou říká, proč soubor jde nebo nejde přidat a co jde zkusit (například
`Cannot be added. You can try "Repair damaged body" and Analyze again.`).
Když se nic nenajde, dialog napíše `No files found.` a rozbalí **Signal
details**.

**Co jde přidat**: soubor se správným součtem hlavičky i těla, nebo
soubor, který opravila zapnutá volba opravy, v některém z těchto formátů:

| Formát | Nahrávka | Blok sestaví |
|--------|----------|--------------|
| `Sharp standard` | příkaz SAVE z ROM | vlastní signál emulátoru ve výchozí rychlosti |
| `CPM-CMT` | CMT.COM pod CP/M (standardní formát při 2400 Bd) | vlastní signál emulátoru v rychlosti 2:1 (CP/M) |
| `TURBO` | TurboCopy, Intercopy TURBO | profil TC-1.21 nebo IC-10.2-TURBO s nahranou rychlostí; s neznámým zapisovačem turbo blok v nejbližší rychlosti |
| `FASTIPL` | Intercopy FAST IPL | profil IC-10.2-FASTIPL s nahranou rychlostí; s neznámým zapisovačem blok FASTIPL v nejbližší rychlosti |
| `CPM-TAPE` | TAPE.COM (Pezik / MarVan), Intercopy CPM | profil PEZIK-2.0A (tělo do 16 KB) nebo IC-10.2-CPM |
| `BSD` | datový soubor BASICu (do 254 kusů po 256 B) | profil BASIC-1Z016 |

Takové soubory jsou zaškrtnuté; zaškrtávátko (nebo mezerník na řádku)
určuje, které se přidají. Blok jiného než standardního formátu zapíše
profil zapisovače znovu: nese stejný soubor (jde exportovat jako MZF, kromě
datového souboru BSD) a hraje čistý signál, ale časování původní nahrávky
se nezachová. Řádek pod tabulkou profil jmenuje. Soubor s chybou součtu
nebo neúplný zůstane v záznamu (`Stays raw`); soubory v jiných formátech
(MZ-80B, SINCLAIR, FSK, SLOW, DIRECT, ZX Spectrum) a soubor, který profil
zapsat neumí (například tělo TAPE.COM nad 16 KB), jako bloky přidat nejde
(`Cannot add`) a také zůstanou v záznamu, kde dál hrají tak, jak se
nahrály.

**Volby**: **Leader tolerance** (2 až 35 %, výchozí 20 %, **Reset** ji
vrátí) říká, jak moc se smějí lišit délky pulzů v zaváděcím tónu; zvyšte
ji u opotřebených nebo nestabilních nahrávek. Volby opravy: **Repair
damaged header** ponechá soubor s neporušenou hlavičkou, jehož tělo chybí
(tělo se vyplní 00h); **Repair damaged body** ponechá soubor s neúplným
tělem nebo spojí dvě poškozené kopie (chybějící bajty se vyplní 00h);
**Repair incomplete BSD file** ponechá neúplný datový soubor BSD
(chybějící kusy se vyplní 00h). Opravený soubor je `Will be added (repaired)`;
jeho data mohou mít místo ztracených bajtů 00h. Po změně volby jsou
výsledky zastaralé: tabulka je ztlumená a nic nejde přidat, dokud
**Analyze again** nespustí analýzu s novými volbami.

**Signal details** (výchozí sbalené) ukazují histogram délek pulzů
vybraného řádku, bez vybraného řádku celého záznamu; u souboru značí jeho
krátký a dlouhý pulz. Velmi dlouhé pulzy (pauzy, ticho) se nekreslí;
dialog napíše, od jaké délky.

**Add files after recording** (výchozí tlačítko, Enter) vloží zaškrtnuté
soubory jako nové bloky hned za záznam; záznam zůstane, jak byl.
**Replace recording** nahradí záznam zaškrtnutými soubory; každá jiná část
signálu (nezaškrtnuté soubory, soubory s chybou, jiné formáty, neznámý
signál) zůstane na pásce v blocích surového záznamu v původním pořadí, takže
se neztratí nic, co se nestane blokem s programem. Vynechají se jen velmi
krátké úseky vedle souboru (méně než 64 pulzů, typicky ticho), stejně jako
při nahrávání. Když záznam ještě není uložený v souboru pásky, dialog
varuje `This recording is not saved yet. Replacing it cannot be undone.`:
po Replace jeho signál vrátit nejde (záznam uložený v souboru pásky v tom
souboru zůstane, dokud pásku neuložíte).

Bloky s programem se sestaví jako nahraný program: vlastní signál emulátoru
výchozí rychlostí (řídí se **Signal speed** a **Set speed** je může
změnit), mezi dvěma z nich pauza 1000 ms; jejich signál se vytvoří znovu
z dekódovaných dat, není to kopie záznamu. Po Add nebo Replace se dialog
zavře, řádek zpráv ukáže `Files added from the recording: 2` a vybraný je
první nový blok. Leží-li před záznamem informační blok TMZ o stroji s jiným
taktem počítače, než je výchozí, hrají nové bloky s programem tímto taktem
a řádek zpráv za výsledkem v závorce varuje `The tape clock setting now
applies to different blocks; check their speed.`; pak zkontrolujte
rychlost bloků. Když se páska během otevřeného dialogu změní, dialog
napíše `The tape changed. Close this dialog and analyze again.` a nic
nepřidá.

### Informace o pásce

**Tape > Tape information...** upravuje texty uložené v pásce: **Title**,
**Publisher**, **Author**, **Year**, **Language**, **Type**, **Price**,
**Protection**, **Origin**, **Comment** (i více řádků) a **Target machine**
(MZ-700, MZ-800, MZ-1500, MZ-80B nebo Not specified). Ukládají se do
informačních bloků na začátku pásky (blok TZX Archive Info a blok TMZ
s informací o stroji); v seznamu bloků jsou to řádky `Control block`, takže
jejich přidání posune čísla ostatních bloků.

- Každý text unese nejvýš 255 znaků (pole víc nepřijme). Texty se ukládají
  v ISO 8859-1 (Latin-1): znak mimo tuto sadu (například česká č, ř, ě, ů
  nebo azbuka) se uloží jako `?` a řádek zpráv varuje, kterých polí se to
  týká; á, é, í, ó, ú a ý se uloží.
- **OK** zapisuje, jen když se něco změnilo (zpráva `Tape information
  updated.`); beze změny dialog zavře a páska zůstane nedotčená. **Cancel**
  a Escape dialog zavřou bez zápisu. Vymazaný text odstraní svou položku;
  položky, které dialog neukazuje, zůstanou.
- **Target machine** zaznamená, pro jaký počítač páska je. Volba stroje na
  pásce bez bloku stroje přidá blok s výchozím taktem, takže se časování
  pásky nezmění; **Not specified** blok odstraní. Měl-li odstraněný blok
  jiný takt CPU, bloky za ním hrají s jiným taktem a řádek zpráv varuje
  `The tape clock setting now applies to different blocks; check their
  speed.`. Páska, jejíž blok stroje jmenuje jiný stroj, ukazuje Not
  specified a blok zůstane beze změny, dokud stroj nevyberete.
- Když se páska během otevřeného dialogu změní, dialog napíše `The tape
  changed. Press Reload to read it again; your changes are kept.`; byla-li
  mezitím vložená jiná páska (jiné jméno nebo soubor), napíše `A different
  tape is in the deck now. Press Reload to read it; your changes are
  kept.`. **Reload** přečte pásku v kazetofonu, pole, která jste změnili,
  zůstanou, ostatní převezmou nové hodnoty a titulek ukáže jméno této
  pásky; **OK** pak zapisuje do ní.
- Má-li páska dál další blok Archive Info, dialog to napíše; ten blok se
  nemění.
- Informace se zachovají při uložení pásky jako TMZ i jako TZX (s cílovým
  strojem se soubor zapíše ve formátu TMZ s varováním, stejně jako jiné
  bloky Sharp MZ). Páska otevřená z jiného formátu (MZF, MZT, WAV, TAP) je
  ve svém souboru udržet neumí: dialog napíše `This tape file format cannot
  store this information. Save the tape as TMZ to keep it.` a **Save** pak
  otevře **Save as...**.

### Kontrola pásky

**Tape > Check tape** otevře okno **Tape check**. Jde s páskou
v kazetofonu v každém stavu, i při přehrávání a během záznamu vstupů.
Kontroluje strukturu pásky: smyčky a skupiny bez konce nebo začátku
a vnořené, skoky a volání na bloky mimo pásku, volání bez bloku návratu na
pásce a návraty bez volání, neznámá ID bloků, bloky s programem, které
nejde přečíst, hlavičku bez zakončení jména, neznámý typ souboru a délku
těla jinou než velikost v hlavičce (varování) a profily, které emulátor
nezná (informace). Součet těla nekontroluje a pásku nepřehrává.

- Horní řádek je souhrn: `Blocks checked: 5. Warnings: 2.` nebo `No
  problems found. Blocks checked: 3.`. Každý nález je řádek se závažností
  (Warning, Info), číslem a jménem bloku a textem. Vypíše se nejvýš 500
  nálezů; při větším počtu souhrn říká `more than`.
- Klik na nález vybere jeho blok v seznamu bloků okna Virtual CMT (okno
  přepne do Full a sbalené se rozbalí; páska se neposune).
- Kontrola pásku čte jen při otevření okna a tlačítkem **Check again**.
  Když se páska potom změní (úprava, informace o pásce, vysunutí, jiná
  páska), okno napíše `The tape changed. Run the check again.` a nálezy
  ztlumí.

### Převod bloku na jiný profil

Blok s programem typu TMZ MZ File nese profil: způsob, jak se vytváří jeho
signál - časování určitého způsobu ukládání (například SAVE z ROM MZ-800,
TurboCopy, Intercopy) nebo MZ800EMU-CMT, vlastní signál emulátoru, který
mají bloky ze souborů MZF a MZT, z nahrávání a z analýzy (i to jsou bloky
MZ File). Převod nechá
program (hlavičku i tělo) bajtově beze změny a sestaví jeho signál jiným
profilem, například aby hrál s původním časováním ROM nebo pro turbo
zavaděč.

**Convert to profile...** v kontextovém menu řádku (jeden vybraný blok;
jen blok MZ File, jehož velikost v hlavičce odpovídá tělu, jinak `This block cannot be converted
to another profile.`) otevře dialog `Convert to profile - block N`
s aktuálním profilem a seznamem profilů s popisy (aktuální je označený
`(current)`; `[unverified]` v popisu označuje údaj o profilu, který není
ověřený). **Speed** jde zvolit jen u profilu, který rychlost přijímá
(`Profile default` nebo poměr); ostatní profily ukazují `Set by the
profile`.

- Nastavení dosavadního profilu, která nový profil přijme (pauza za
  blokem, rychlost a další), se přenesou. Co se přenést nedá, ohlásí řádek
  zpráv jako varování, například `Settings of profile MZ800EMU-CMT not
  carried over: speed`, jinak je zpráva `Block 3 converted to
  ROM-800-NOMINAL.`.
- Tentýž profil bez jiné rychlosti nic nezmění (`Nothing to change.`).
- Blok jiného profilu než MZ800EMU-CMT hraje rychlostí, kterou určil
  převod; **Set speed** pro něj není k dispozici a **Signal speed** se ho
  netýká (dialog to připomene). Jeho rychlost změní jen další převod;
  převedený zpět na MZ800EMU-CMT s `Profile default` se řídí **Signal
  speed** a **Set speed** zase funguje.

## Rychlost a režim načítání

### Režim načítání

Přepínač **Loading** v okně Virtual CMT:

| Režim | Co dělá |
|-------|---------|
| **Tape** | přehrává signál ve skutečném čase |
| **Boost** | přehrává signál, ale emulátor běží nejvyšší rychlostí, dokud běží motor kazetofonu |
| **Direct** | upravená ROM zkopíruje program do paměti bez přehrávání signálu a uložení rutinami ROM uloží rovnou na pásku (CMT hack, viz níže) |

- Do režimu Direct a z něj jde přepnout jen při zastavené pásce. Direct
  je nedostupný, když páska stojí na bloku, který nenese program MZF (bez
  pásky a u prázdné pásky jde zapnout vždy). Mezi Tape a Boost jde
  přepínat i při běžící pásce.
- V režimu Direct je Play zakázané (Stop funguje, když páska hraje,
  například po **Fast Open CMT File & Play...**), **Signal speed** se
  nepoužívá a pod jménem bloku je `Next LOAD takes this block` nebo
  `This block cannot be loaded directly`. **Record** funguje jako
  v ostatních režimech: při nahrávání povely motoru programu nahrávání
  rozjíždějí a zastavují a displej ukazuje nahrávání (Recording, Waiting
  for motor, dobu nahrávání); **Play** rozjede nahrávání čekající na
  motor; platí to i pro uložení rutinami ROM (Direct Save platí jen bez
  Record). Rewind, Previous block, Next
  block a skok na blok v seznamu vybírají blok, který vezme příští
  nahrávání.
- **Load directly** v kontextovém menu řádku vybere blok a přepne do
  režimu Direct jedním krokem (jen při zastavené pásce a u bloku
  s programem MZF).
- S ROM, která přímé načtení neumí (na MZ-800 Willy ROMs, na MZ-1500
  monitor MZ-NEWMON, uživatelská ROM s volbou `Disabled CMT Hack Patch`),
  jsou **Direct**, **Load directly** i **Use CMT Hack** zakázané a jejich
  tooltip říká `The selected ROM does not support direct loading.`
- Během nahrávání nebo přehrávání záznamu vstupů režim změnit nejde.
- Režim mění i položky hlavního menu, každá jen jednu volbu: **HW &
  Devices > CMT > Use CMT Hack** zapíná a vypíná CMT hack (Direct); po jeho
  vypnutí okno ukáže Boost nebo Tape podle volby **HW & Devices > CMT >
  CMT Options > Enable CPU Boost**. Enable CPU Boost volí mezi Boost
  a Tape; při zapnutém CMT hacku zůstane režim Direct a volba se projeví
  až po vypnutí hacku. Přepínač v okně nastaví obě volby najednou
  (Tape a Direct vypnou CPU Boost). Platí pro ně stejná pravidla: CMT hack jde zapnout nebo vypnout
  jen při zastavené pásce (jinak se změna odmítne s hlášením) a během
  nahrávání nebo přehrávání záznamu vstupů jsou obě položky nedostupné,
  protože je nedostupné celé menu HW & Devices.
- Bez souboru `.ini` (první spuštění) je CMT hack zapnutý, takže okno
  ukazuje Direct. Volba `--cmt` z příkazové řádky CMT hack vypne jen pro
  toto spuštění (viz Příkazová řádka).

### CMT hack a páska

CMT hack (režim **Direct**) nahraje program přes upravenou ROM okamžitě,
bez přehrávání signálu. Týká se nahrávání rutinami ROM pro kazetu
(bootovací nabídka MZ-800 a MZ-1500, nahrávání z monitoru); program
s vlastním zavaděčem ho nepoužije. Například kazetové BASICy 1Z-016 V1.0A
(MZ-800) a 1Z-013B V1.0A (MZ-700), nahrané z pásky, a diskový BASIC čtou
a zapisují kazetu vlastními rutinami: se zapnutým Direct jejich `LOAD`
z kazety čeká a páska se nerozjede (u kazetových BASICů čekání ukončí
SHIFT+BREAK); nahrávejte s nimi v režimu Tape nebo Boost. BASICů je
mnoho a každý se může chovat jinak, takže jiné BASICy a jiné verze se
mohou lišit.

Se zapnutým **Direct Save** (výchozí) a bez Record CMT hack upraví
i ukládací rutiny ROM. Uložení přes ně (například uložení z monitoru)
uloží program hned jako nový blok na konec pásky; ROM přitom dál vypíše
`WRITING` a jméno souboru. Bez pásky se vloží nová prázdná páska, hrající
páska (i v pauze) se zastaví a páska pak stojí na novém bloku. Rutina
ROM pro ověření (verify) porovná paměť s programem bloku, na kterém páska
stojí (hned po uložení je to uložený program); páskou nehýbe a hrající
pásku zastaví, jen nese-li ten blok program. Se stisknutým **Record**
jsou ukládací rutiny ROM původní: vše se v režimu Direct nahraje jako
signál stejně jako v režimu Tape (povely motoru programu nahrávání
rozjíždějí a zastavují), rutinami ROM i vlastní ukládací rutinou programu
(například kazetové BASICy uvedené výše, které **Record** potřebují
vždy).

- Je-li v kazetofonu páska, CMT hack vezme program z aktuálního bloku
  pásky a po nahrání těla programu posune pásku na další blok
  s programem. Další nahrávání tak dostane další program. Kazetofon se
  přitom nespouští; hraje-li páska (i v pauze), první nahrávání ji
  zastaví. Rychlost bloku se při tom neuplatní.
- Hlásí-li hlavička programu delší tělo, než blok obsahuje, skončí
  nahrávání chybou a páska se posune na další program (jako skutečná
  páska). Znovu ho lze zkusit tak, že pásku vrátíte na ten blok
  (**Previous block**, nebo v seznamu bloků Ctrl+Enter či **Go to this
  block**).
- Když aktuální blok program nenese (TAP, WAV, jiný blok TZX), když už byl
  nahrán poslední program na pásce, nebo když v kazetofonu páska není,
  otevře se dialog pro výběr souboru MZF.
- Reset počítače konec pásky zapomene, páska zůstane, kde je: další
  nahrávání po resetu vezme program z bloku, na kterém páska stojí (po
  posledním programu tedy tentýž program znovu).
- Snapshot si pamatuje, kde CMT hack na pásce je (i konec pásky):
  nahrávání po načtení snapshotu pokračuje stejně, jako by se snapshot
  nenačítal.
- Vypnutí CMT hacku (přepnutí na Tape nebo Boost, nebo Use CMT Hack
  v menu) zapomene, kde hack na pásce skončil (konec pásky i načtenou
  hlavičku bez těla); další nahrávání po jeho zapnutí vezme program
  z bloku, na kterém páska stojí.
- **HW & Devices > CMT > CMT Options > Fix FNAME Terminator** (výchozí
  zapnutá): když CMT hack nahrává hlavičku, jejíž jméno nemá zakončení
  0Dh, doplní ho do hlavičky v paměti počítače.
- **HW & Devices > CMT > CMT Options > Direct Save** (výchozí zapnutá,
  klíč `direct_save` v sekci `[CMTHACK]` souboru `.ini`) přímé ukládání
  zapíná a vypíná, jen při zastavené pásce. Vypnuté znamená, že uložení
  rutinami ROM v režimu Direct potřebuje **Record** jako v režimu Tape.
  Položka je zakázaná, když je CMT hack vypnutý, a s ROM bez standardních
  rutin pro kazetu.
- Uložení rutinami ROM, které zapíše jen hlavičku (bez programu), při
  Direct Save nic neuloží; reset počítače, načtení snapshotu nebo stisk
  Record takovou hlavičku zapomene.
- **Stop recording** uprostřed uložení rutinami ROM v režimu Direct (po
  hlavičce, před programem): nahrávka uchová hlavičku jako surový blok
  a program zapsaný po zastavení se uloží rovnou jako úplný blok za ním.

### Výchozí rychlost (Signal speed)

Výchozí rychlost platí pro bloky s programem, které nemají vlastní
rychlost (například nahrané programy a programy z analýzy). Nastavuje se
výběrem **Signal speed** v okně Virtual CMT (poměry 1:1 až 3:1 a **Custom
pulses...**) nebo v menu **HW & Devices > CMT > Set Default CMT MZ Speed**
a mění se jen při zastavené pásce. Změna přestaví aktuální blok, řídí-li se
výchozí rychlostí; ostatní bloky ji převezmou, když se otevřou. Změna
výchozí rychlosti pásku jako neuloženou neoznačí.

**Custom pulses...** otevře editor **Custom Tape Pulses** s délkami pulzů
v mikrosekundách v pořadí hlavičky UniCMT CMTSPEED: **Long pulse high
(bit 1)**, **Long pulse low (bit 1)**, **Short pulse high (bit 0)**
a **Short pulse low (bit 0)**. Tlačítka **Presets** (UniCMT 1x, UniCMT 2x,
UniCMT 3x) délky vyplní, **Revert** vrátí délky při otevření editoru.
Řádek **Equivalent speed** ukazuje odpovídající poměr a rychlost v Bd.
Každá délka musí být větší než 0 a nejvýš 65535 µs; jinak je **OK**
zakázané. Rychlost se změní až po **OK**.

### Rychlost bloku

**Set speed** v kontextovém menu řádku (jen při zastavené pásce) dá bloku
vlastní rychlost; sloupec **Speed** ji ukazuje. Měnit jde jen bloky
z programů MZF, MZT a TAP a bloky TMZ s profilem MZ800EMU-CMT (vlastní
signál emulátoru); ostatní bloky TMZ / TZX a nahrávka WAV hrají tak, jak
jsou nahrané (`The speed of this block is given by the file.`). U bloku
s programem položka **Default** vrátí blok k výchozí rychlosti (Signal
speed); u bloku TAP jsou jen poměry 1:1, 9:7, 3:2 a 25:14. Blok převedený na jiný profil hraje rychlostí
převodu (viz Převod bloku na jiný profil). Volba **Custom pulses...**
otevře editor délek pulzů (viz výše).

Změna rychlosti otevřeného bloku ho přestaví a převine na jeho začátek.
Změna rychlosti bloku (na jinou, než blok má) je úprava pásky: páska má
pak neuložené změny (viz Neuložené změny).

### Polarita

Přepínač **HW & Devices > Rear DIP Switch > CMT Polarity** (Normal /
Inverted, mění se jen při zastavené pásce) obrací signál zvukového záznamu
ze souboru WAV, takže digitální ticho se při obrácené polaritě čte jako
vysoká úroveň. Na ostatní bloky, na surový záznam pořízený nahráváním ani
na export celé pásky nepůsobí. Na importovaný blok WAV působí jen do
uložení: páska uložená do TMZ nebo TZX je obyčejná páska a po jejím
otevření přepínač na zvukový záznam už nepůsobí. Snapshot a záznam vstupů
působení přepínače u otevřeného souboru WAV zachovají.

### Echo

Se zapnutým zaškrtávátkem **Echo** okna Virtual CMT (na pravém konci
jeho lišty menu) nebo položkou **HW & Devices > CMT > CMT Options > Echo** (výchozí
vypnutá) je signál pásky slyšet z reproduktoru, když páska hraje (režimy
Tape a Boost) a když nahrává. Přímé načtení a přímé uložení (režim
Direct) signál nemají, takže jsou tiché. Echo sdílí zvukový kanál
pípáku počítače (časovač 8253), takže je stejně hlasité jako pípák
a obsahuje ho i zvukový záznam a videozáznam emulátoru. To, co program
z pásky čte a co se nahraje, echo nemění.

## Přímé spuštění souboru MZF

Volba `--run-mzf` z příkazové řádky a nástroj MCP `emu_media_run_mzf`
spouštějí soubor MZF přímo: program se uloží do paměti a spustí, jako by
ho z pásky nahrála ROM, bez přehrávání pásky. Obvody, mapování paměti,
obrazovka, pracovní oblast monitoru a zásobník jsou, jak je ROM nechá po
nahrání z pásky; z registrů procesoru nastaví jako ROM jen BC (viz níže)
a registr se startovní adresou, ostatní se mohou lišit. Program, který
nahraje ROM, dostane v registru BC kód zařízení, ze kterého byl nahrán;
některé programy podle něj volí výchozí zařízení (například BASIC 1Z-016
V1.0A s kódem Quick Disku používá Quick Disk jako výchozí zařízení). Kód zařízení pro přímo
spuštěný program se nastavuje v **HW & Devices > CMT > CMT Options >
Device for Direct MZF Start**:

| Položka | BC | Význam |
|---------|----|--------|
| **CMT - cassette** (výchozí) | 0100h, na MZ-700 0000h | jako po nahrání z pásky (ROM MZ-800 a MZ-1500 předá 0100h, monitor MZ-700 nechá 0000h) |
| **FD - floppy disk** | 0200h | jako po nahrání z diskety |
| **QD - Quick Disk** | 0300h | jako po nahrání z Quick Disku |
| **RD - SRAM** | 0000h | jako po nahrání ze zařízení RD (SRAM) |
| **Custom value...** | 0 až 65535 | libovolná hodnota zadaná desítkově bez úvodních nul (`512`) nebo šestnáctkově (`0x0200` nebo `0200h`); `0200` se odmítne, protože kód zařízení se snadno myslí šestnáctkově; dialog ukazuje aktuální hodnotu šestnáctkově |

Hodnota platí od příštího přímého spuštění; už běžící program se nemění.
Ukládá se do souboru `.ini` (`[CMT] run_mzf_device`). Jen pro jedno
spuštění ji přebije volba `--run-mzf-device` z příkazové řádky (viz
README) a parametr `device` nástroje `emu_media_run_mzf`. Nahrávání přes
ROM (například bootovací nabídka nebo režim Direct) toto nastavení
nepoužívá: BC nastaví ROM sama.

## Snapshot a záznam vstupů

### Snapshot

Snapshot si pamatuje cestu k souboru pásky, aktuální blok, místo v něm,
rychlosti bloků, stav kazetofonu (i pauzu a motor), stav přehrávání
(smyčky, volání) a to, kde CMT hack na pásce je. Obsah pásky, která
odpovídá svému souboru, se do snapshotu neukládá.

- Páska s neuloženými změnami nebo nová páska bez souboru se do snapshotu
  uloží celá (nejvýš 16 MB; větší se neuloží a do logu se vypíše
  varování). Načtení ji znovu vloží jako pásku s neuloženými změnami bez
  jména souboru (zobrazí se jako (new tape)), bez čtení souboru; původní
  jméno souboru ukáže jen hlášení "Unsaved tape restored from the snapshot
  (was: jméno)". Save se pak na jméno souboru zeptá, takže snapshot
  od někoho jiného nemůže určit, kam se páska uloží. Je-li při načtení
  v kazetofonu páska se stejným souborem (například Quick Save a Quick
  Load), jméno souboru zůstane.
- Pásku uloženou jen cestou snapshot při načtení vloží z té cesty (je-li
  už vložená stejná páska se stejnými bloky, použije se) a obnoví blok,
  místo, rychlosti i stav kazetofonu.
- Když soubor chybí, v kazetofonu páska nebude, stav je Stopped a do logu
  se vypíše varování.
- Když má soubor mezitím jiné bloky (jiný počet nebo druh), páska je
  vložená, zastavená na prvním bloku a do logu se vypíše varování.
  Změnu obsahu bloků při stejném počtu a druhu emulátor nepozná.
- Pásku ze síťové cesty (začíná `\\` nebo `//`) snapshot sám neotevře:
  zachová se jako u chybějícího souboru a do logu se vypíše varování.
  Takovou pásku nejdřív vložte a pak snapshot načtěte - vložená páska se
  stejnou cestou se použije. Místní disky a namapovaná písmena disků se
  otevírají normálně. Páska s neuloženými změnami ze síťové cesty dá při
  načtení navíc varování.
- Probíhající nahrávání se neukládá: snapshot pořízený během nahrávání se
  načte se zastaveným kazetofonem (varování v logu) a načtení kteréhokoli
  snapshotu probíhající nahrávání zahodí.
- Načtení snapshotu nezmění výchozí rychlost, polaritu, Boost ani kontrolu
  velikosti MZF, které máte nastavené. Blok, který ve snapshotu hrál jinou
  výchozí rychlostí, než máte teď, ji dostane jako vlastní rychlost.

### Záznam vstupů

Během **nahrávání** záznamu vstupů (Replay) jde kazetu ovládat jako
obvykle (transport, vysunutí, nová páska, nahrávání), ale pásku nejde
upravovat, ukládat, exportovat ani otevřít jinou a režim načítání
a rychlosti změnit nejde.
Páska s neuloženými změnami se uloží do záznamu; když se nevejde do
limitu záznamu, nahrávání nezačne a vyzve k uložení pásky. Dotazy před
ztrátou změn (viz Dotazy před ztrátou změn) zůstávají, jen **Save tape**
se nenabízí, protože pásku tehdy uložit nejde (**Discard changes** nebo
**Cancel**).

Během **přehrávání** záznamu vstupů patří páska přehrávání (vaše páska se
po něm vrátí) a okno Virtual CMT je zamčené
(`Not available while an input replay is played.`); proto se emulátor na
pásku neptá. Zavření okna emulátoru během přehrávání nejdřív přehrávání
zastaví, takže se vaše páska vrátí, a pak se jako obvykle zeptá, má-li
neuložené změny. Kontrola pásky jde i během záznamu vstupů.

## Nastavení v souboru .ini

| Sekce a klíč | Význam | Výchozí |
|--------------|--------|---------|
| `[CMT] mz_cmtspeed` | výchozí rychlost (Signal speed), hodnoty jako v okně: `SPEED_1/1`, `SPEED_2/1`, `SPEED_2/1_CPM`, `SPEED_7/3`, `SPEED_8/3`, `SPEED_3/1`, `SPEED_CUSTOM` | `SPEED_1/1` |
| `[CMT] mz_custom_pulse_long_high_us`, `mz_custom_pulse_long_low_us`, `mz_custom_pulse_short_high_us`, `mz_custom_pulse_short_low_us` | vlastní délky pulzů v µs (Custom pulses) | 470, 494, 240, 278 |
| `[CMT] cmt_polarity_inverted` | CMT Polarity: 1 = Inverted | 0 |
| `[CMT] cpu_boost` | Enable CPU Boost (režim Boost) | 1 |
| `[CMT] mzfsize_check` | Enable MZF Size Check | 1 |
| `[CMT] mzfsize_autofix` | Fix MZF Size Automatically: 1 = soubor delší, než udává hlavička, se zkrátí bez dotazu | 0 |
| `[CMT] echo` | Echo: 1 = signál pásky je slyšet z reproduktoru | 0 |
| `[CMT] autorewind` | 1 = páska se po dohrání převine na první blok | 0 |
| `[CMT] rec_autostop_s` | automatické zastavení nahrávání po tolika sekundách ticha (0 až 600, 0 = vypnuto) | 0 |
| `[CMT] run_mzf_device` | Device for Direct MZF Start: kód zařízení v registru BC programu spuštěného přímo (`cmt`, `fd`, `qd`, `rd`, `sram` nebo číslo 0 až 65535 zapsané jako v dialogu, viz Přímé spuštění souboru MZF; neplatná hodnota se ohlásí a použije se `cmt`) | `cmt` |
| `[CMTHACK] enable` | Use CMT Hack (režim Direct) | 1 |
| `[CMTHACK] fix_fname_terminator` | Fix FNAME Terminator | 1 |
| `[CMTHACK] direct_save` | Direct Save: s CMT hackem a bez Record se uložení rutinami ROM uloží rovnou na pásku | 1 |
| `[VIRTUAL_CMT_UI] layout` | rozvržení okna: 0 = Compact, 1 = Full | 0 |
| `[VIRTUAL_CMT_UI] compact_w`, `compact_h`, `full_w`, `full_h` | velikost okna každého rozvržení v pixelech, 0 = výchozí velikost | 0 |
| `[VIRTUAL_CMT_UI] props_open` | vlastnosti: 0 = sbalené, 1 = rozbalené | 0 |
| `[VIRTUAL_CMT_UI] auto_analyze` | Find files while playing: 1 = hrající surový záznam se analyzuje na pozadí | 1 |

## Příkazová řádka

Volby `--cmt`, `--cmt-play`, `--cmt-play-pause`, `--cmt-boot`,
`--run-mzf` a `--run-mzf-device` popisuje
[README](../README.md#volby-z-příkazové-řádky).

## Omezení

- **Velké soubory**: otevření, import, uložení a export běží najednou, takže
  u velkého souboru (například dlouhé nahrávky WAV) se emulace i okno
  emulátoru na dobu převodu zastaví. Export drží celý zvukový signál pásky
  v paměti, takže velmi dlouhá páska zabere při exportu hodně paměti.
  Analýza běží na pozadí.
- **Nahrávání**: nejvýš 4 hodiny nebo 16 000 000 hran signálu.
- **Export celé pásky**: nejvýš 4 hodiny.
- **Snapshot**: obsah pásky nejvýš 16 MB.
- **Informace o pásce**: 255 znaků na text, jen znaky ISO 8859-1.
- **Kontrola pásky**: nejvýš 500 nálezů.
- **Mazání**: výběr z velmi mnoha oddělených skupin bloků najednou smazat
  nejde.
- **Síťové cesty**: snapshot pásku ze síťové cesty sám neotevře.
- **Přetažení souboru**: okno Virtual CMT vytažené mimo hlavní okno
  přetažený soubor přijmout nemusí.
- **CMT hack** funguje jen s nahráváním přes rutiny ROM; rychlost bloku
  se v něm neuplatní.
- **Polarita** působí jen na zvukový záznam ze souboru WAV.
- **Texty z jádra emulátoru** (důvody v analýze, nálezy kontroly, popisy
  profilů, některá varování) jsou anglicky ve všech jazycích.
