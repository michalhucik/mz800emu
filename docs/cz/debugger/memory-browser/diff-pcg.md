# Memory Browser - Memory Diff + PCG glyph editor

Dvě samostatná okna nad Memory Browser jádrem. **Memory Diff** porovnává
side-by-side dva zdroje (živou paměť nebo snapshot) a zvýrazňuje rozdíly. **PCG glyph
editor** kreslí 8x8 bitmapové znaky do PCG RAM na MZ-1500.

Obě okna jsou nezávislá na hlavním Memory Browser oknu (singleton), ale
sdílí s ním definici regionů a layerů (viz [layers-regions](layers-regions.md)).


## 1. Memory Diff (V5)

Okno pro porovnání dvou zdrojů dat (A a B) side-by-side. Zvýrazní byty,
které se mezi A a B liší, a ukáže souhrnnou statistiku.

### 1.1 Otevření

| Cesta | Akce |
|---|---|
| **Menu** Debugger -> Memory Diff | Otevře / zavře okno (singleton) |

V5 nemá vyhrazenou klávesovou zkratku - okno se otevírá výhradně přes menu.

### 1.2 Layout

Odshora dolů:

1. Řádek **A** a řádek **B** - každý zdroj má vlastní dropdown regionu,
   dropdown režimu, tlačítko **Snapshot Now** a stav: `(live)`, velikost
   pořízeného snapshotu (`(65536 B)`) nebo oranžové `(no snapshot)`.
2. Checkbox **Auto-snapshot at pause**, tlačítka **Refresh A+B**
   a **Export...**.
3. Side-by-side hex view: offset v regionu (8 hex číslic), 16 bytů A,
   oddělovač `|`, 16 bytů B. Byty, které se mezi A a B liší, jsou
   zvýrazněny magentou v obou sloupcích.
4. Spodní řádek se souhrnnou statistikou.

Po otevření je A = **Live** a B = **Snapshot**, oba nad logickým adresním
prostorem Z80; B zatím nemá snapshot.

### 1.3 Region a režim zdroje

- **Region** - dropdown přes všechny registrované regiony (logický adresní
  prostor Z80, fyzická RAM, VRAM, PCG bank, ...). A i B mají region
  každý svůj, porovnávají se byty se stejným offsetem.
- **Režim** - jak zdroj získává data:

| Režim | Význam |
|---|---|
| **Live** | Aktuální obsah regionu, čte se v každém snímku UI |
| **Snapshot** | Obsah pořízený tlačítkem **Snapshot Now** (nebo **Refresh A+B**); drží se, dokud ho nepořídíš znovu |
| **Auto @ pause** | Jako Snapshot, navíc se obnoví automaticky při pauze emulátoru (viz 1.5) |

Změna regionu nebo režimu zahodí snapshot daného zdroje. Region větší
než 4 MB se ořízne na prvních 4 MB.

Typické kombinace:

- A = **Live**, B = **Snapshot** - "co se změnilo od posledního Snapshot Now"
- A = **Snapshot**, B = **Snapshot** - porovnání dvou různých momentů
  (každý zdroj pořízený jindy)
- A = **Auto @ pause**, B = **Live** - "co se změnilo od poslední pauzy"
  (po pokračování emulace)

### 1.4 Snapshot Now a Refresh A+B

- **Snapshot Now** (u každého zdroje, aktivní jen v režimu Snapshot
  a Auto @ pause) pořídí aktuální obsah regionu daného zdroje. Předchozí
  snapshot se přepíše bez potvrzení.
- **Refresh A+B** pořídí nový snapshot u obou zdrojů, které nejsou
  v režimu Live.

### 1.5 Auto-snapshot at pause

Checkbox v toolbaru. Je-li zapnutý, při **každém** přechodu emulátoru do
pauzy (manuální stop, hit breakpointu, krok) pořídí nový snapshot všechny
zdroje v režimu **Auto @ pause**. Snapshot tedy zachycuje stav
**v okamžiku pauzy**. Počet takto pořízených snapshotů ukazuje spodní
řádek (`auto-snapshots taken: N`). Workflow:

1. Nastav A = **Auto @ pause**, B = **Live** a zapni **Auto-snapshot at pause**.
2. Stopni emulátor (A teď drží stav v okamžiku pauzy).
3. Pokračuj v emulaci a vykonej akci ve hře (např. seber předmět).
4. B ukazuje aktuální stav - magenta zvýrazní, co se od pauzy změnilo.

### 1.6 Export

Tlačítko **Export...** otevře dialog pro uložení textového reportu
(přípony `.txt`, `.diff`). Formát:

```
# Memory Diff export
# Source A size: 65536, Source B size: 65536
# Changed: 3 of 65536 bytes
#
0000C010: 50 -> AA
0000C012: 13 -> 99
0000C014: 00 -> 01
```

Řádek `offset: A -> B` pro každý odlišný byte (nejvýš 1 000 000 řádků).
Mají-li zdroje různou velikost, na konci je poznámka `tail mismatch`.

Vhodné jako příloha k bug reportu nebo pro audit log.

### 1.7 Statistika

Spodní řádek okna:

- **Changed: X of Y (Z %)** - počet odlišných bytů z porovnaných bytů.
- Mají-li A a B různou velikost, přibude
  **[size mismatch A=... B=...]**.

### 1.8 Use cases

| Scénář | Postup |
|---|---|
| Najít, kde hra drží stav (HP, skóre, pozici) | A = Snapshot, Snapshot Now, vykonat akci ve hře, B = Live -> magenta byty jsou kandidáti |
| Ověřit, že cheat / freeze drží | Totéž nad bytem HP, nechat hráče utrpět damage -> hodnota stejná = freeze funguje |
| Porovnat dva save stavy | Load `.mzs` A, A = Snapshot + Snapshot Now, load `.mzs` B, B = Live -> rozdíly mezi save body |


## 2. PCG glyph editor (V6, jen MZ-1500)

Okno **PCG editor (MZ-1500)** pro editaci 8x8 bitmapových znaků uložených
v **PCG** (Programmable Character Generator) RAM. MZ-1500 specifický rys -
3 banky x 1024 znaků x 8 B na znak.

### 2.1 Otevření

| Cesta | Akce |
|---|---|
| **Memory Browser** -> region PCG bank 1/2/3 -> kurzor na byte -> pravé tlačítko myši -> **Open in PCG editor...** | Otevře editor a zaměří na znak obsahující kurzor (= addr / 8) |

Jiná cesta k otevření není - editor nemá položku v menu ani klávesovou
zkratku. Okno se zavírá křížkem v titulku.

### 2.2 Layout

Odshora dolů:

1. **Bank** dropdown (Bank 1/2/3), číselný vstup **Char #** pro index
   znaku a navigační tlačítka **[<] [>]**.
2. Řada tlačítek operací **Inverse / Mirror H / Mirror V / Rotate 90 CW /
   Clear / Fill**.
3. 8x8 grid klikatelných cells (= jeden glyph, rozsvícený pixel žlutě).
4. Řádek **Raw:** s 8 bajty glyfu v hex a řádek **Bank addr range:**.

### 2.3 Bank selektor

Dropdown **Bank** přepíná mezi třemi PCG bankami MZ-1500 (sub_id 0/1/2).
Každá banka je 8 KB = 1024 znaků x 8 B (jeden znak = jeden řádek 8 bitů
per byte, celkem 8 bytů na glyph).

### 2.4 Navigace po znacích

- **Char #** - číselný vstup pro přímé zadání indexu, **dekadicky**
  (0-1023), včetně tlačítek **-** / **+**. Hodnota mimo rozsah se ořízne
  na 0 nebo 1023.
- **[<] [>]** - prev/next znak (jump po 8 B v PCG bank). Na znaku 0
  a 1023 se zastaví (bez wraparoundu).
- Pod gridem editor ukazuje řádek **Raw:** s 8 bajty aktuálního znaku
  v hex a řádek **Bank addr range:** s rozsahem bajtů znaku v bance
  (`char_idx * 8` .. `char_idx * 8 + 7`).

### 2.5 Kreslení

- **Levý klik** na cell v 8x8 gridu - toggle bitu (0/1, tj.
  bg/fg pixel).
- Řádek **Raw:** pod gridem ukazuje aktuálních 8 bytů glyfu (= bytewise
  reprezentace, jeden byte = jeden řádek bitmapy, bit 7 = levý pixel).
  Řádek je jen pro čtení.

### 2.6 Operace nad celým glyfem

| Tlačítko | Efekt |
|---|---|
| **Inverse** | Flip všech 64 bitů (= negativ glyfu) |
| **Mirror H** | Horizontální zrcadlení (= reverse bitů uvnitř každého ze 8 bytů) |
| **Mirror V** | Vertikální zrcadlení (= reverse pořadí 8 bytů) |
| **Rotate 90 CW** | Rotace o 90 stupňů ve směru hodinových ručiček |
| **Clear** | Vynuluje všech 8 bytů (prázdný glyph) |
| **Fill** | Nastaví všech 8 bytů na 0FFh (plný glyph) |

### 2.7 Zápis do PCG RAM

Editor nemá rozpracovanou kopii ani tlačítka Save / Reload. **Každá
změna** (klik do gridu i operace z 2.6) se **okamžitě zapíše** celých
8 B glyfu do PCG banky na adresu `char_idx * 8`. Změna se hned projeví
ve videovýstupu emulátoru (pokud je glyph zobrazen). Úpravu nejde vrátit
jinak než opačnou operací nebo ručním překreslením.

Editor čte glyph z PCG RAM v každém snímku, takže ukazuje i změny, které
mezitím provedl běžící program nebo jiné okno.

### 2.8 Per-architektura dostupnost

| MZARCH | Chování |
|---|---|
| **MZ-1500** | Plně funkční (PCG je hlavní rys video subsystému MZ-1500) |
| **MZ-800** | Okno nejde otevřít - region PCG bank neexistuje, takže chybí i položka **Open in PCG editor...** |
| **MZ-700** | Stejně jako MZ-800 - žádná PCG hardwarová podpora |

### 2.9 Use case: custom font glyph

1. Otevři Memory Browser, přepni na region **PCG bank 1**.
2. Najdi volný slot (např. char 0x90, kde je v ROM mezera nebo nepoužitý
   znak).
3. Pravé tlačítko myši -> **Open in PCG editor...**.
4. V editoru klikej na cells - vykresli požadovaný glyph. Každý klik se
   rovnou zapisuje do PCG RAM. V poli **Char #** je index dekadicky
   (0x90 = 144).
5. Hra nebo aplikace, která vypíše znak s kódem 0x90 přes VRAM, nyní
   uvidí nový glyph.


## 3. Související

- [memory-browser](README.md) - hlavní okno Memory Browser
- [layers-regions](layers-regions.md) - definice regionů a layerů
  (nutné pro výběr regionu v Diff i navigaci v PCG)
- [search](search.md) - hledání bytových vzorů (užitečné společně
  s Diff pro lokalizaci state proměnných)
