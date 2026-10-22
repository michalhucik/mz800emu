# Pokrytí kódu - který kód se vykonal a kudy šly větve

Pokrytí kódu za běhu programu zaznamenává, které instrukce se vykonaly
a kterým směrem šla každá podmíněná instrukce: jestli byla její podmínka
splněna (skok, volání nebo návrat se provedl, bloková instrukce se
opakovala), nesplněna, nebo obojí. Odpovídá na otázky "které části
programu test vůbec nespustil?" a "která větev se nikdy neprošla?".

Podmíněné instrukce: `JR`, `JP`, `CALL` a `RET` s podmínkou, `DJNZ`,
`LDIR`, `CPIR`, `INIR`, `OTIR` a sestupné varianty (`LDDR`, `CPDR`,
`INDR`, `OTDR`). Výsledek se bere ze stavu procesoru po instrukci, takže
i `JR Z,$+2` se započítá správně, přestože oba výsledky pokračují na stejné
adrese.

Dostupné na MZ-800, MZ-700 i MZ-1500: v okně CPU Profiler, jako značky
v oknech Disassembly, přes MCP, v akcích breakpointů a z příkazové řádky.

## Úrovně

- **branch** - podle logické adresy (PC): vykonané adresy, podmíněné
  instrukce a jejich výsledky, první instrukce obsluhy přerušení.
  Paměťové banky se nerozlišují: kód na stejné adrese v ROM a v RAM nebo
  ve dvou bankách MemExt se sčítá. Zpomalí emulaci zhruba o 16-29 %
  (měřeno v MAX SPEED).
- **detail** - navíc jeden záznam na každou verzi kódu: fyzická buňka
  paměti prvního bajtu (RAM, ROM, VRAM, PCG, banka MemExt), adresa
  a bajty opkódu. Samomodifikující kód, který změní instrukci, dostane
  novou verzi, změněný operand se označí. Každá verze má čítače
  (vykonání, podmínka splněna, nesplněna). Záznam zabírá 32 bajtů,
  tabulka má pevnou kapacitu a co se nevejde, se ohlásí. Zpomalí emulaci
  zhruba o 78-115 %, protože emulátor musí zaznamenávat bajty tak, jak je
  procesor načítá.

Data jsou **úplná**, když se nic nezahodilo (plná tabulka), všechny
bajty pocházejí z načtení procesorem a žádný čítač nepřetekl. Reset,
načtení snapshotu a skok při přehrávání data ponechají a započítají se
jako nespojitosti. Nečinný HALT (procesor čeká na přerušení) se
nepočítá.

## Záložka Coverage v okně CPU Profiler

Otevřete okno CPU Profiler (hlavní menu `Debugger` -> `CPU Profiler`,
`Alt+Shift+P`) a zvolte záložku **Coverage**.

Lišta nástrojů:

- **Level** - `branch` nebo `detail`; **Capacity** - velikost tabulky
  úrovně detail (počet záznamů). Obojí platí při spuštění sběru.
- **Start** spustí sběr. Pokud existují data se stejnou úrovní
  a kapacitou (i data sbíraná z příkazové řádky nebo přes MCP), pokračuje
  do nich, jinak začne s prázdnými daty. **Stop** sběr zastaví, data
  zůstanou.
- **Reset** smaže data (sběr pokračuje, pokud běží).
- **Mark** začne novou epochu. Filtr **New since mark** pak ukáže jen
  kód vykonaný poprvé po značce, třeba co přidá stisk klávesy nebo nová
  úroveň hry.
- **Export...** zapíše všechny řádky do souboru, JSON nebo CSV podle
  přípony (stejný formát jako MCP `emu_coverage_export`).

Filtr:

- **Show** - **Partial branches** (podmíněné instrukce jen s jedním
  výsledkem nebo zatím bez výsledku), **Both branches**, **All
  conditional**, **New since mark**, **All**.
- **Range** - jen adresy v rozsahu (hex).

Sloupce:

| Sloupec | Význam |
|---|---|
| Addr | adresa instrukce (logické PC) |
| Symbol | nejbližší načtený symbol na adrese nebo pod ní, `+N` = posun |
| State | `none` = nepodmíněná; `both`, `taken_only`, `not_taken_only` = které výsledky podmínky nastaly; `unobserved` = zatím žádný |
| Exec | počet vykonání této verze kódu (detail) |
| Taken / Not taken | kolikrát byla podmínka splněna / nesplněna (detail) |
| Cell | fyzická buňka paměti prvního bajtu a její offset (detail); `bus` = bez fyzické buňky (grafická VRAM, mapované porty) |
| Key | bajty, které rozlišují verze kódu (prefixy a opkód, detail) |
| Epoch | epocha prvního vykonání |
| Flags | `int` = první instrukce obsluhy přerušení, `smc` = operand se měnil, `cmt` = zasáhl CMT hack, `halt`, `fallback` = bajt se četl z paměti místo z načtení procesorem, `saturated` = čítač přetekl, `bus`, `prefixes`, `long` |

U úrovně branch má tabulka jeden řádek na vykonanou adresu a nemá
čítače, u úrovně detail jeden řádek na verzi kódu.

Levý klik na řádek otevře adresu v Disassembly #1, pravý klik nabídne
breakpoint a Disassembly #2-#5.

Stavový řádek ukazuje, jestli sběr běží, úroveň, počet instrukcí,
podmíněné instrukce a jejich výsledky, jestli jsou data úplná (tooltip
uvede, proč ne), epochu a počet zobrazených řádků.

Okno si za běhu sběru říká emulátoru o data zhruba dvakrát za sekundu
(tabulku úrovně detail jednou za sekundu), a to jen při zobrazené
záložce.

## Značky v oknech Disassembly

Dokud data pokrytí existují, okna Disassembly (#1-#5) ukazují u každé
vykonané instrukce barevnou značku vpravo od ikon breakpointu a PC:

| Značka | Význam |
|---|---|
| zelená | vykonáno (nepodmíněná), nebo podmíněná s oběma výsledky |
| žlutá | podmínka vždy splněna (jen skok) |
| oranžová | podmínka nikdy splněna (jen propad) |
| bez značky | nevykonáno |

Značky se řídí logickou adresou, takže se objeví i u kódu na stejné
adrese v jiné paměťové bance. Tooltip řádku má sekci **Coverage** se
stavem a u úrovně detail s verzemi kódu na adrese, jejich buňkami
a čítači.

Značky se vypínají v menu okna debuggeru `Settings` -> `Disassembled` ->
`Show code coverage` (uloží se do konfigurace).

## Příkazová řádka a konfigurace

- `--coverage branch|detail` - sbírat od startu.
- `--coverage-key opcode|full_bytes` - klíč úrovně detail: bajty opkódu
  (výchozí), nebo první 4 bajty včetně operandů (každá hodnota operandu
  je pak vlastní verze).
- `--coverage-capacity N` - velikost tabulky úrovně detail, 1-1048576
  záznamů (výchozí 65536).
- `--coverage-save-on-exit SOUBOR` - zapsat všechny řádky při ukončení
  emulátoru (`.csv` = CSV, jinak JSON; relativní cesta je vůči pracovnímu
  adresáři).

Stejná nastavení v sekci INI `[COVERAGE]`: `mode` (`OFF`, `BRANCH`,
`DETAIL`), `key`, `capacity`, `reset_on_discontinuity` (při resetu,
načtení snapshotu a skoku přehrávání data smazat místo započtení).
Bezhlavý běh s `--result-json` zapíše stav pokrytí do `result.json`
([Headless mode](../mcp-server/headless-mode.md)).

## Akce breakpointů

`coverage_start` (úroveň branch), `coverage_start detail`,
`coverage_stop`, `coverage_mark`. Příklad: spustit sběr, když hra vstoupí
do úrovně, a označit okamžik stisku klávesy. Viz
[Action DSL](breakpoints/action-dsl.md).

## MCP

`emu_coverage_start`, `emu_coverage_stop`, `emu_coverage_reset`,
`emu_coverage_status`, `emu_coverage_mark`, `emu_coverage_read` (stránky
s filtrem) a `emu_coverage_export`. Viz
[MCP nástroje](../mcp-server/tools-overview.md) a MCP dokument
`emulator://docs/coverage`.

## Pokrytí zdrojového kódu (lcov)

`docs/tools/coverage_to_lcov.py` převede export (JSON) na tracefile lcov,
ze kterého `genhtml` udělá HTML report a který umí zobrazit rozšíření
editorů pro pokrytí. S listingem assembleru (sjasmplus `--lst`, pasmo
`--listing`) dostanou řádky listingu počty vykonání a podmíněné instrukce
své dvě větve:

```bash
python3 docs/tools/coverage_to_lcov.py coverage.json -o coverage.info --listing hra.lst
genhtml coverage.info -o coverage-html
```

Bez listingu skript zapíše pseudo zdrojový soubor s řádkem na každou
vykonanou adresu. Podrobnosti: [docs/tools/README.md](../../tools/README.md).

## Meze

- Úroveň branch nerozlišuje paměťové banky; pro kód v bankách nebo
  overlaye použijte detail.
- U úrovně detail značky a počty v lcov sčítají všechny verze kódu na
  adrese.
- Se sběrem pokrytí je emulace pomalejší (viz Úrovně); po Stop běží
  emulace zase plnou rychlostí.
