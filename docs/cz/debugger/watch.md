# Watch - uživatelsky definované paměťové hlídky

Okno **Watch** umožňuje sledovat libovolné množství adres v paměti CPU
(nebo dynamicky vyhodnocovaných výrazů) v reálném čase. Hodnoty se
přepočítávají každý ImGui frame; při pause emulátoru zůstávají stálé.
Záznamy persistují přes restart emulátoru (per-arch JSON soubor).

## Otevření okna

- **Menu Debugger -> Watch**
- **Klávesová zkratka** Alt+Shift+W (toggle)

Alt+W bez Shift je obsazená pro "Fix window aspect ratio by Width";
Watch toggle proto vyžaduje Shift modifier (stejné jako Alt+Shift+R pro
CPU Registers a Alt+Shift+S pro Stack Regions).

## Layout okna

| Toolbar řádek | Obsah |
|---------------|-------|
| 1 | `[+ Add]` + počet záznamů |
| 2 | `[Save]` `[Save As...]` `[Load From...]` `[Clear All...]` + status |

Tabulka řádků (6 sloupců):

| Sloupec | Význam |
|---------|--------|
| (drag) | Drag handle pro drag-reorder |
| Name | Volitelný jmenný label (double-click = inline rename) |
| Addr / Expr | Hex adresa (mode=Address) nebo zkrácený text výrazu (mode=Expr*) s tooltipem pro plný expr + cílovou adresu pro deref |
| Type | Typ + meta (`bit.N`, `ascii[N]`, ...) |
| Value | Live polled hodnota (pravý klik = format menu pro int typy) |
| (delete) | `x` button - okamžitý smaz bez konfirmace |

## Add dialog (Mode + Type + parametry)

### Mode dropdown

| Mode | Popis | Chování value cell |
|------|-------|--------------------|
| **Address** | Literal adresa + typ | Čte se byte z paměti bez side-efektu podle typu |
| **Expression (scalar)** | Výraz se vyhodnotí jako int32 | Hodnota = výsledek výrazu, type určuje display šířku/format |
| **Expression (pointer deref)** | Výraz se vyhodnotí jako uint16 adresa | Adresa = výsledek výrazu, pak se z ní čte podle typu (analogicky Address) |

### Address input (mode=Address)

Vstup pro adresu - akceptuje:

- C-style hex: `0xC080` / `0xc080`
- IASM hex: `0C080h` / `C080h` (suffix `h`/`H`, musí začínat číslicí)
- Decimal: `49280`
- Symbol jméno (resolve přes symbol DB, case-sensitive)

### Expression input (mode=Expr*)

Vstup pro výraz - syntax je shodná s engine používaným pro podmínky
smart breakpointů. Cheat sheet níže.

### Type dropdown

| Typ | Popis | Velikost | Display |
|-----|-------|----------|---------|
| `u8` | Unsigned 8-bit | 1 B | dec / hex / bin / char |
| `i8` | Signed 8-bit | 1 B | dec / hex / bin |
| `u16le` / `u16be` | Unsigned 16-bit LE / BE | 2 B | dec / hex / bin |
| `i16le` / `i16be` | Signed 16-bit LE / BE | 2 B | dec / hex / bin |
| `u32le` | Unsigned 32-bit LE | 4 B | dec / hex |
| `bit` | Jeden bit na adresa.bit | 1 B | `0` / `1` |
| `ascii N` | ASCII řetězec, N bajtů | N B | string s `\xNN` escapy |
| `mzascii N` | Sharp MZ ASCII (EU), N bajtů | N B | string přes Sharp -> UTF-8 konverzi |
| `bytes N` | Hex dump, N bajtů (max 16 inline + tooltip) | N B | `12 34 56 ...` |

### Length spinner

Zobrazuje se jen pro variabilní typy (`ascii`, `mzascii`, `bytes`).
Rozsah: 1 .. 64 pro string typy, 1 .. 256 pro `bytes`.

### Bit index spinner

Zobrazuje se jen pro typ `bit`. Rozsah 0-7.

### Name (optional)

Volitelný textový label, truncate na 63 znaků. Prázdný = anonymní řádek
(zobrazuje se jako `(anon)`).

## Display format menu (kontextové)

Pro int typy (u8/i8/u16/i16/u32) pravý klik na buňku Value otevře menu
"Display format" se 4 volbami:

- `dec` - decimal (signed pro `i*`, unsigned pro `u*`)
- `hex` - `0xNN` / `0xNNNN` / `0xNNNNNNNN`
- `bin` - `0b10101010` (přesná šířka podle typu)
- `char` - `'A'` (jen pro u8/i8; non-printable nebo multibyte = hex fallback)

Default formát:

- Unsigned int -> `hex`
- Signed int -> `dec`

## Expression syntax (cheat sheet)

Subset nejčastěji použitelný ve Watch:

| Konstrukce | Význam |
|------------|--------|
| `0xNN` / `#NN` | Hex literál |
| `%1010` | Binární literál |
| `NN` | Decimal literál |
| `[addr]` | Byte z paměti (no side-effect) |
| `{addr}` | Word LE z paměti (no side-effect) |
| `[addr]!` | Ve Watch stejné jako `[addr]` (`!` se ignoruje) |
| `port[N]` | I/O port byte (no side-effect) |
| `port[N]!` | Ve Watch stejné jako `port[N]` (`!` se ignoruje) |
| `A` `B` `C` `D` `E` `H` `L` `F` | Z80 8-bit registry |
| `BC` `DE` `HL` `AF` `IX` `IY` `SP` `PC` | 16-bit registry |
| `I` `R` `IM` `IFF1` `IFF2` | Special registry |
| `AF'` `BC'` `DE'` `HL'` | Shadow registry |
| `Cf` `Zf` `Sf` `Pf` `Hf` `Nf` | Z80 flagy (0/1) |
| `Cycle` `Frame` `Scanline` | Globální emu counters |
| `$name` | User var z `$vars` panelu (chybějící = 0) |
| `+ - * / %` | Aritmetika |
| `<< >>` | Shift |
| `< <= > >= == !=` | Relační |
| `& \| ^` | Bitové |
| `&& \|\|` | Logické (short-circuit) |
| `~ !` | Unární |
| `min max abs if bit s8 s16 s32` | Built-in funkce |

**Bez vedlejších efektů:** Watch stav stroje nikdy nemění. `[addr]`,
`{addr}` i `port[N]` čte bez vedlejších efektů, takže Watch **nespouští
MEM R ani IORQ breakpointy** a port nečte skutečným IN. Suffix `!`
(čtení s vedlejším efektem v podmínkách a akcích breakpointů) se ve Watch
ignoruje, stejně jako v Test Eval breakpointu a v hledání výrazem
v Memory Browseru. Čtení portu s vedlejším efektem jde přes akci
breakpointu nebo MCP `emu_io_read`.

## Parse error display

Pokud výraz neprojde parserem, řádek se přesto vloží (uživatel může
opravit), ale buňka Value zobrazí červený text `!parse: <error>` místo
hodnoty. Update přes editaci výrazu reparsuje a vyčistí error.

## Persistence

Per-arch JSON soubor (`mz800.watch` / `mz1500.watch` / `mz700.watch`),
auto-save při exit a auto-load při startu.

Cfg sekce `[WATCH_PANEL]`:

- `watch_file` (TEXT) - název per-arch souboru (default `mz<N>.watch`)
- `auto_save` (BOOL, default 1)
- `auto_load` (BOOL, default 1)

Schema JSON:

```json
{
  "watch": [
    {
      "name": "score",
      "addr": 49280,
      "bank": -1,
      "type": "u8",
      "fmt": "hex"
    },
    {
      "name": "deref_HL",
      "addr": 0,
      "bank": -1,
      "type": "u8",
      "fmt": "hex",
      "mode": "expr_deref",
      "expr": "[HL]"
    }
  ]
}
```

Chybějící pole (`fmt` / `length` / `bit_index` / `mode` / `expr`) se při
loadu nahradí defaulty.

## Save / Load tlačítka

| Tlačítko | Akce |
|----------|------|
| **Save** | Uloží do default per-arch souboru bez dialogu |
| **Save As...** | Otevře file dialog pro výběr cílové cesty |
| **Load From...** | Otevře file dialog + konfirmace (REPLACE = wipe + load) |
| **Clear All...** | Konfirmace + smaže všechny řádky |

Status hláška (např. "Saved to ..." nebo "Load failed: ...") se zobrazuje
inline za tlačítky a zůstává až do další akce.

## Příklady použití

### 1. Sledovat scratch byte hry

Mode `Address`, addr `0xC080`, type `u8`, fmt `hex`. Hodnota se mění za
běhu jak hra zapisuje. Pravý klik na Value -> přepnout na `dec` pro
lidsky čitelnější zobrazení skóre.

### 2. Sledovat 16-bit score s little-endian uložením

Mode `Address`, addr `0x4080`, type `u16le`, fmt `dec`. Watch přečte 2
bajty na adrese a interpretuje je jako LE word.

### 3. Sledovat byte na adrese, kterou ukazuje HL

Mode `Expression (pointer deref)`, expr `HL`, type `u8`. Pozor: `[HL]`
v deref módu znamená double-indirect (= z paměti na adrese `*HL` se
vezme dalši adresa). Pro single-indirect ("byte na pozici HL") použijte
prostý `HL` v deref módu.

### 4. Sledovat user var `$framecnt`

Mode `Expression (scalar)`, expr `$framecnt`, type `i32` (nebo `u8` /
`u16le` podle rozsahu). Hodnota = aktuální obsah user var z `$vars`
panelu, vhodné pro počítadla nastavená smart BP akcemi.

### 5. Sledovat I/O port `0xCE` (Border/BCOL)

Mode `Expression (scalar)`, expr `port[0xCE]`, type `u8`, fmt `bin`.
Vidíme bitové pole portu bez side-efektu (nesimuluje IORQ read).

## Vazba na další panely

- **Symbols** - resolve symbol jména v Address inputu probíhá přes
  symbol DB.
- **Breakpoints / smart BP podmínky** - sdílejí expression engine,
  identické konstrukce v podmínkách smart BP.
- **`$vars` panel** - identifikátor `$name` ve výrazu čte user var
  nastavený smart BP akcemi.
- **Memory Search** - pravé tlačítko na kandidátovi -> **Add to Watch**
  přidá nalezenou proměnnou sem (viz [Memory Search](memory-search.md)).
