# Disassembly - sekce hlavního debugger okna a sekundární okna

Sekce **Disassembled** v hlavním debug okně + 4 nezávislá sekundární okna
(**Disassembly #2 - #5**) zobrazují disassemblovanou paměť emulátoru.
Hlavní instance má historii a Follow PC; sekundární okna jsou statické
debug pohledy bez auto-follow.

Hlavní instance je oficiálně **Disassembly #1** - title hlavního debug
okna obsahuje suffix " - Disassembly #1" (např. `MZ-800 Debugger -
Disassembly #1`). V "Show in" submenu i v popupu klikatelného PC v I/O
Ports History se na ni odkazuje jako `Disassembly #1`.

## Hlavička sekce

Místo jednoduchého `SeparatorText("Disassembled")` má sekce **rich
hlavičku**:

```
[Address or symbol  ] [v] Follow PC  [ ] T-states
```

### Text entry (adresa nebo symbol)

InputText (~16 znaků), Enter spouští parse:

1. **Hex parser** akceptuje formáty:
   - `0x1234`, `0X1234`
   - `#1234`
   - `1234h`, `01234h`
   - holé hex `1234` (case-insensitive)

2. Pokud hex parse selže, **symbol lookup** v symbol DB
   (case-sensitive). Pokud existuje symbol s daným jménem, použije se
   jeho adresa.

3. Při neúspěchu (= ani hex, ani symbol): **červené pozadí** InputText,
   buffer se nevyčistí (uživatel může opravit a zkusit znovu).

Po úspěšném parse: focus_addr se nastaví na parsed adresu, buffer se
vyčistí, focus zůstává na entry pro další zadání.

### Checkbox "Follow PC"

Per-instance dynamic flag. Pokud true a emu **běží**, instance auto-jumps
focus_addr na aktuální `PC` při každém update.

| Instance | Default | Persist |
|----------|---------|---------|
| main     | ON      | ano (`disasm_main_follow_pc`) |
| #2-#5    | OFF     | ano (`disasm_extra<N>_follow_pc`) |

UI checkbox lze přepnout v hlavní i sekundárních instancích.

### Checkbox "T-states"

Per-instance dynamic flag. Pokud true, render přidá 5. sloupec za MNEM
s počtem T-states instrukce.

| Instance | Default | Persist |
|----------|---------|---------|
| main     | OFF     | ano (`disasm_main_show_tstates`) |
| #2-#5    | OFF     | ano (`disasm_extra<N>_show_tstates`) |

## Horní tabulka - HISTORIE

Renderuje se **jen v hlavní instanci** (sekundární okna ji nemají).

Zdroj dat: ring buffer s 32 záznamy posledních vykonaných instrukcí.
Každý záznam má adresu + až 4 bajty z exec time (= snapshot v okamžiku
CPU fetch, ne live read přes banking switch).

Sloupce identické s dolní tabulkou: ICONS / ADDR / BYTES / MNEM (+
TSTATES pokud je per-instance toggle ON).

Záznam nese i registry CPU **před** instrukcí (AF, BC, DE, HL, IX, IY,
SP, WZ, I, R, IM, IFF1, IFF2). Ukáže je tooltip po najetí myší na
mnemoniku řádku. U zastaveného breakpointu tak jde zjistit, s jakými
hodnotami běželo posledních 32 instrukcí, bez zapnutého cputrack.

Splitter mezi historií a dolní tabulkou je drag-resizable (poměr se
pamatuje v rámci session).

### Režim zaznamenávání (CPU Instruction History)

Kdy se ring buffer historie plní, řídí volba `CPU Instruction History`
(submenu s radiobuttony v menu debug okna). Má čtyři režimy:

| Režim | Kdy se historie nahrává |
|-------|-------------------------|
| `Off` | Nikdy - historie se nezaznamenává (žádná režie). |
| `Only With Debug Window` | Jen když je debug okno otevřené. |
| `With Debug Window or Breakpoints` (default) | Když je debug okno otevřené **nebo** je aspoň jeden breakpoint enabled. |
| `Always` | Trvale, nezávisle na stavu okna i breakpointů. |

Režim `With Debug Window or Breakpoints` je **nově výchozí** (dříve byl
default `Only With Debug Window`). Důvod: historie je pak k dispozici
i bez otevřeného okna v okamžiku, kdy breakpoint zastaví běh - lze se
podívat na posledních 32 instrukcí, které zásahu předcházely. `Off` se
respektuje i při enabled breakpointech (= explicitní vypnutí historie).

**Persistence:** ini modul `DEBUGGER`, klíč `cpuhist_mode` (KEYWORD),
hodnoty `OFF` / `WITH_WINDOW` / `WITH_WINDOW_OR_BP` / `ALWAYS`, default
`WITH_WINDOW_OR_BP`.

Pozn.: Toto je informativní instrukční historie (32 posledních
instrukcí). Pro plnohodnotné tracking logy s časovými razítky a dumpem
paměti viz subsystém cputrack (Debugger Settings -> Trace Suite),
popsaný v [formats/CPU-track_format.md](formats/CPU-track_format.md).

## Dolní tabulka - DISASSEMBLY

Hlavní pracovní oblast. Render disassemblovaných instrukcí od
`focus_addr`, počet řádků závisí na výšce sekce.

### Sloupec ICONS (gutter)

Obsahuje branch arrows (vlevo) a BPT + PC ikonu (vpravo).

#### Branch arrows (jen pokud `disasm_show_branch_arrows` ON)

Pro každou JR / JP / CALL / DJNZ / RST instrukci s **fixed targetem** se
vykreslí svislá šipka ze zdroje na cíl (pokud je cíl viditelný). Limit
max 4 paralelní šipky najednou.

Stav cíle:

- **Cíl viditelný v okně** -> plná šipka spojující řádky
- **Cíl mimo okno (vyšší adresa)** -> stub šipka dolů
- **Cíl mimo okno (nižší adresa)** -> stub šipka nahoru
- **Cíl == zdroj** (`JR -2` / `JR $`) -> mini-loop glyph (tight loop)

Barvy:

- **CALL** -> modrá
- **JP / JR / DJNZ / RST** -> šedá
- **Hover** (myš nad zdrojovým nebo cílovým řádkem) -> žlutá

Branch arrows nejsou implementovány pro `JP (HL)`, `JP (IX)`, `JP (IY)`,
`RET`, `RETI`, `RETN` (= bez fixed targetu).

#### BPT a PC ikony

**BPT (levý slot):**

| Stav | Symbol |
|------|--------|
| Aktivní BPT na počáteční adrese | červený plný kruh |
| Deaktivovaný BPT na počáteční adrese | bílý prázdný kruh |
| Aktivní BPT uvnitř instrukce (= addr+1..addr+len-1) | žlutý plný kruh |
| Deaktivovaný BPT uvnitř instrukce | žlutý prázdný kruh |

**PC indikátor (pravý slot):**

| Stav | Symbol |
|------|--------|
| `PC == addr` | zelený trojúhelník |
| `PC` uvnitř instrukce (mid-instruction) | žlutá šipka |

#### Značka pokrytí kódu

Dokud existují data pokrytí kódu, barevná značka vpravo od PC slotu
ukazuje pokrytí adresy: zelená = vykonáno (nebo podmíněná instrukce
s oběma výsledky), žlutá = podmínka vždy splněna, oranžová = podmínka
nikdy splněna, bez značky = nevykonáno. Se značkami je gutter o 6 px
širší. Tooltip řádku dostane sekci Coverage. Vypíná se v `Settings` ->
`Disassembled` -> `Show code coverage` (cfg klíč `disasm_show_coverage`).
Viz [code-coverage.md](code-coverage.md).

### Sloupec ADDR (5 znaků)

Adresa instrukce. Dvě varianty:

- Pokud existuje symbol pro tuto adresu: zobrazí se **jméno symbolu**.
- Jinak: hex `XXXX:`.

**PC zvýraznění** se aplikuje **jen na tento sloupec**:

- `PC == addr` -> **zelený text**
- `PC` uvnitř instrukce -> **žlutý text**

Ostatní sloupce řádku (BYTES, MNEM, TSTATES) zůstávají default barvou.

### Sloupec BYTES (12 znaků = `XX XX XX XX`)

**2-tone barvení:**

- **Opcode bajty** (= prefix CB/DD/ED/FD + hlavní opcode) -> default
  barva
- **Operand bajty** (= immediate hodnoty, displacement) -> **cyan**

Počet operand bajtů se počítá podle typu operandů:

| Typ operandu | Operand bytes |
|--------------|---------------|
| 8-bit immediate, 8-bit relativní offset, IX/IY displacement | 1 |
| 16-bit immediate, 16-bit MEM | 2 |
| 8/16-bit registr, condition, bit index, RST vektor | 0 |

Edge case: instrukce `DD CB d op` / `FD CB d op` (4 bajty) má
displacement uprostřed sekvence, ne na konci. Pro jednoduchost se v
tomto případě všechny 4 bajty kreslí default barvou.

### Sloupec MNEM (15 znaků)

**Kategorické barvení mnemoniky** (= prvního slova) podle typu
instrukce:

| Kategorie | Mnemoniky | Barva |
|-----------|-----------|-------|
| **Flow** | jp, jr, call, ret, reti, retn, rst, djnz, halt | **oranžová** |
| **Stack** | push, pop | světle modrá |
| **Block** | ldi, ldd, ldir, lddr, cpi, cpd, cpir, cpdr | růžová |
| **I/O** | in, out, ini, ind, inir, indr, outi, outd, otir, otdr | magenta |
| **Arith** | add, adc, sub, sbc, inc, dec, neg, cp, daa, and, or, xor, cpl, scf, ccf | žlutá |
| **Bit** | bit, set, res, rlc, rl, rrc, rr, sla, sra, sll, srl, rld, rrd, rlca, rla, rrca, rra | cyan |
| **CPU control** | nop, di, ei, im | světle šedá |
| **Default** | ld, ex, exx, ostatní | bílá (default text) |

**Operandy** (= zbytek mnemoniky za první mezerou) se tokenizují podle
separátorů (`,`, `(`, `)`, ` `, `+`, `-`) a každý token se klasifikuje:

| Token | Klasifikace | Barva |
|-------|-------------|-------|
| `#1234`, `$FF`, `0x12`, `1234h` | hex literál | **cyan** |
| Pure decimal (`123`) | číslo | **cyan** |
| Registr (a, b, ..., bc, hl, sp, ix, iy, ixh/l, iyh/l, af', bc', de', hl') | registr | default |
| Podmínka (nz, z, nc, c, po, pe, p, m) | condition | default |
| Cokoli jiného (= identifikátor) | symbol | **cyan** |

Separátory zůstávají default barvou.

Příklady:

- `ld bc,#0000` -> `ld` default, `bc,` default, `#0000` cyan
- `call MAIN_LOOP` -> `call` oranž, `MAIN_LOOP` cyan (= symbol)
- `jp z,#1234` -> `jp` oranž, `z,` default, `#1234` cyan
- `ld (ix+5),#7` -> `ld` default, `(ix+5),` default, `#7` cyan

### Sloupec TSTATES (volitelný, ~5 znaků)

Aktivuje se per-instance checkboxem "T-states" v hlavičce. Format:

- Bezvětvové instrukce -> jediné číslo, např. `4`
- Podmíněné větvení -> `taken/not`, např. `17/10` (CALL cc), `12/7`
  (JR cc), `13/8` (DJNZ), `21/16` (LDIR)

## Right-click context menu

Vyvolá se pravým tlačítkem na řádek dolní tabulky. Pořadí položek:

1. **Set/Remove Breakpoint** - toggle BPT na adrese řádku
2. **Enable/Disable Breakpoint** - aktivace/deaktivace existujícího BPT
3. **Set as PC** - skok emulátoru na tento řádek (= zápis `PC = addr`)
4. **Focus to <target>** - jen pokud branch s fixed targetem; label
   obsahuje target adresu + symbol pokud existuje, např.
   `Focus to 0xE6DA (CALC_LOOP)`
5. **Focus to...** - otevře dialog pro zadání adresy/symbolu
6. **Focus to PC** - skok dolní tabulky na aktuální `PC`
7. **Focus to register** - submenu s 11 položkami:
   - Primární páry: AF, BC, DE, HL
   - Shadow páry: AF', BC', DE', HL' (oddělené separátorem)
   - Index + SP: IX, IY, SP
   - Label: `BC = 0x1234 (SYMBOL)` (pokud existuje symbol pro hodnotu)
8. **Show in** - submenu s 5 položkami "Disassembly #1..#5":
   - #1 = main, #2-5 = secondary
   - Klik zajistí otevření okna + nastaví focus na adresu řádku
   - Aktuální instance je v menu disabled
9. **Add to bookmarks** - přidá záložku na adresu řádku:
   - User input = symbol jméno pokud existuje, jinak `#XXXX` hex
   - Comment prázdný (uživatel doplní v okně Bookmarks)
   - Otevře okno Bookmarks pokud bylo zavřené
10. **Edit row** - otevře Inline Assembler (modal dialog) pro úpravu
    instrukce na řádku

Right-click je k dispozici **i za běhu emu**.

## Auto-disable Follow PC při interakci

Aby uživatel mohl manuálně řídit focus za běhu emu, jsou definované
situace, kdy se per-instance `follow_pc` flag automaticky vypne
(= permanentně, persistuje):

| Akce | Chování |
|------|---------|
| **LMB klik na řádek** | Pokud `follow_pc && !is_paused` -> vypnout follow_pc |
| **RMB klik na řádek** (před popup) | Stejně |
| **Focus to ...** v context menu | Stejně (auto-disable) |
| **Focus to PC** | Stejně |
| **Focus to register** (11 položek) | Stejně |
| **Focus to <target>** | Stejně |
| **Add form Enter** (text entry) | Stejně |
| **Focus to... dialog Apply** | Stejně |
| **Bookmarks LMB / Show in** | Stejně |

Jen v paused stavu se `follow_pc` zachová (= klik je benigní, focus se
neovlivní).

### Slider drag bypass (dočasný, nemění flag)

Speciální chování pro vertikální slider 0x0000..0xFFFF:

- Pokud uživatel **drží LMB** na slideru a `follow_pc=ON` a emu běží:
  - **Bypass** Follow PC update **po dobu držení** (= focus_addr se
    nepřepisuje na PC)
  - Uživatel může "nakouknout" jinam aniž by se okno hned vrátilo na PC
- Po **uvolnění tlačítka**:
  - Bypass přestane, follow_pc opět aktivně řídí focus_addr
  - Po dalším frame se focus_addr vrátí na aktuální PC

Na rozdíl od ostatních akcí (klik, menu) slider drag flag **nemění** -
po release se Follow PC vrací k normálu.

## Tooltip per řádek

Hover na řádku po ~500 ms zobrazí sjednocený tooltip s těmito sekcemi
(sekce bez dat se vynechají):

### 1. Instrukční info

```
E6CA: CD DA E6  call #e6da     <- header (tučně, žlutě)
CALL nn: push PC, PC = nn      <- description z metadat instrukce
T-states: 17                    <- t_states / t_states2
Flags: S Z F5 H F3 P/V N C     <- labels, ovlivněné zelené
```

Pokud pro instrukci není dostupný popis, popis se vynechá.

T-states: pokud není podmíněné větvení, jen jediné číslo. Jinak
`taken/not`.

Flagy: 8 labelů `S Z F5 H F3 P/V N C`. Každý label **zelený** pokud je
flag instrukcí ovlivněn, default barva pokud ne.

### 2. Symbol info pro adresu řádku

Jen pokud existuje symbol pro adresu řádku:

```
Symbol: CALC_LOOP
Address: 0xE6CA
Comment: hlavni vypocetni smycka  <- pokud je
Module: main.asm                   <- pokud je (z .map importu)
```

### 3. Symbol info pro operandy

Jen pokud existuje fixed target (CALL/JP/JR/...) nebo přímý MEM access:

```
Target: 0xE6DA -> CALC_HELPER
Mem: (0x1234) -> SCREEN_BUFFER
```

### Pokrytí kódu

Jen když se zobrazují značky pokrytí: stav adresy a u úrovně detail
verze kódu na adrese (buňka, bajty klíče, vykonání, podmínka splněna /
nesplněna). Viz [code-coverage.md](code-coverage.md).

### 4. Edit hints

Šedý dim text:

```
Double-click a row or start typing to open the inline assembler.
Right-click for context menu.
```

### Snapshot strategie

Při prvním hover na řádku se data řádku zkopírují do per-instance
snapshot bufferu. Tooltip drží stable po celou dobu hover, dokud
uživatel nezmění hovered index.

Důvod: při běžícím emu se cache disasm řádků přepisuje každých
~100 ms. Bez snapshot by tooltip blikal s aktualizovanými daty
(mnemonika / T-states / flagy by se měnily v tepu refresh ticku).
Snapshot zachytí řádek deterministicky.

## Sekundární okna Disassembly #2 - #5

Aktivace přes top menu **Debugger -> Other disassembly -> Disassembly
#N**. Default closed. Pozice a velikost se pamatuje přes `imgui.ini`,
visibility se neserializuje.

Každé okno hostuje samostatnou instanci pohledu vytvořenou bez horní
tabulky historie. Lazy create při prvním otevření, lazy destroy při
zavření.

### Per-instance state

| State | Persistence | Klíč v cfg |
|-------|-------------|------------|
| focus_addr | ano | `disasm_extra<N>_focus` |
| follow_pc | ano | `disasm_extra<N>_follow_pc` |
| show_tstates | ano | `disasm_extra<N>_show_tstates` |
| selected_row | ne | - |
| visible_rows | ne (= podle aktuální výšky okna) | - |
| Pozice/velikost okna | ano | imgui.ini |

### Sdílené globální stavy

- Memory snapshot (cache rebuild čte aktuální banking)
- Symbol DB
- Breakpoints (BPT v jakémkoli okně se projeví ve všech)
- Globální toggle pro branch arrows
- Refresh tick

## Klávesová navigace

Aktivní jen ve vybraném (focused) řádku:

| Klávesa | Akce |
|---------|------|
| Šipka nahoru / dolů | posun selekce o řádek |
| PgUp / PgDown | posun selekce o stránku |
| Enter | otevře Inline Assembler (stejné jako double-click) |
| Home | focus_addr = 0x0000 |
| End | focus_addr = 0xFFF0 |

Slider vpravo (vertikální 0x0000..0xFFFF) řídí `focus_addr`. Drag = jump
na pozici, kolečko myši = scroll po instrukci nahoru/dolů.

## Persistence (souhrn cfg klíčů)

Modul `DEBUGGER`:

| Klíč | Typ | Default | Popis |
|------|-----|---------|-------|
| `disasm_show_branch_arrows` | bool | true | Globální toggle pro branch arrows |
| `disasm_show_coverage` | bool | true | Globální toggle pro značky pokrytí kódu |
| `disasm_main_focus_addr` | uint16 | 0 | Hlavní okno focus |
| `disasm_main_follow_pc` | bool | true | Hlavní okno Follow PC |
| `disasm_main_show_tstates` | bool | false | Hlavní okno T-states sloupec |
| `disasm_extra<N>_focus` | uint16 | 0 | Sekundární okno N (2..5) focus |
| `disasm_extra<N>_follow_pc` | bool | false | Sekundární okno N Follow PC |
| `disasm_extra<N>_show_tstates` | bool | false | Sekundární okno N T-states |
| `cpuhist_mode` | KEYWORD | `WITH_WINDOW_OR_BP` | Kdy plnit ring buffer historie (viz Horní tabulka - HISTORIE) |

Layout (pozice / velikost / split ratio) řeší ImGui samostatně přes
`imgui.ini`.

## Známá omezení

1. **DDCB/FDCB byte coloring** - 4-bajtové instrukce s displacement
   uprostřed mají všechny bajty default barvou (= raritní instrukce,
   nepokryté).

2. **Race UI <-> emu vlákno na banking switch** - cache rebuild čte
   100 bajtů smyčkou. Mezi bajty může emu vlákno přepnout mapping (např.
   ROM_E000 <-> PROHIBITED) -> buffer obsahuje smíchané bajty z různých
   bank stavů. Disassemblace z bufferu pak vrátí konzistentní výsledek
   v rámci jednoho rebuild ticku, ale **mezi rebuilds** (= ~100 ms apart)
   se může výsledek různit.

3. **Mapped ports na 0xE000-0xE008 v MZ-700 mode** - PIO 8255 a GDG
   memop pro DMD status neresepktují debug flag, takže debug read na
   tyto adresy modifikuje stav chipů. CTC8253 flag respektuje. Pro
   disasm scan na 0xE6D0+ (= rom_e000_efff, addr_low > 0x0F) side effect
   není vyvoláván.

4. **Inline Assembler je modální** - pokud uživatel chce procházet IASM
   Help, musí ho otevřít před otevřením IASM dialogu.

5. **Disassembly #1 = main** - v "Show in" submenu položka #1 cílí na
   hlavní debug okno. Pokud je hlavní okno zavřené, klik ho otevře.

## Vazba na další panely

- **Breakpoints** - BPT ikony v ICONS gutteru, context menu pro
  set/remove/enable/disable. Detaily v
  [breakpoints/README.md](breakpoints/README.md).
- **Symbols** - resolve symbol jména v hlavičce text entry, ADDR
  sloupec, MNEM operand tokenizace, context menu "Add to bookmarks".
  Detaily v [symbols.md](symbols.md).
- **Bookmarks** - context menu "Add to bookmarks" + Bookmarks LMB
  vrací focus zpět do hlavní instance. Detaily v
  [bookmarks.md](bookmarks.md).
- **I/O Ports History** - klikatelné PC v Selected Event panelu otevírá
  disasm v zadaném slotu. Detaily v [io-ports.md](io-ports.md).
- **Callstack** - klik v Callstack řádku otevírá disasm na `Call` nebo
  `Target` adrese.
