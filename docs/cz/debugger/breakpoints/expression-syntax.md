# Breakpoint - syntaxe výrazů (Condition expression)

Výraz je jazyk používaný v poli `condition` breakpointu, v argumentech
většiny příkazů action mini-DSL (viz `action-dsl.md`) a v Live Test Eval
panelu. Podmínka breakpointu se vyhodnotí těsně před spuštěním akce -
pokud výraz vrátí 0, BP nestřelí; nenulová hodnota znamená "trigger".

Tento dokument popisuje gramatiku, lexikální pravidla a sémantiku
výrazu breakpoint systému.

## Datový typ

Evaluator pracuje s jediným typem - signed 32-bit integer (`int32_t`).
Booleanovská logika je vyjádřená jako "0 = false, ne-nula = true"
(C-style). Žádný floating point, žádné stringy, žádné agregátní typy.

## Literály

Konvence literálů odpovídá zápisu IASM (interní asembler emulátoru).

| Tvar | Příklad | Význam |
|------|---------|--------|
| Decimální | `42`, `1000` | desítkové číslo |
| Hexadecimální (C) | `0x42`, `0xFF00` | hex s prefixem `0x` / `0X` |
| Hexadecimální (Sharp) | `#42`, `#FF00` | hex s prefixem `#` |
| Binární | `%1010`, `%11110000` | binární s prefixem `%` |

Pozor: `%` je v lexeru ambivalentní - na začátku výrazu nebo po
operátoru / `(` / `[` znamená binární literál; po hodnotě / `)` / `]`
znamená modulo. Lexer disambiguuje podle pozice, pro uživatele je to
typicky neviditelné. Příklad: `%101 + a%2` je `5 + (a mod 2)`.

Žádné znakové (`'A'`) ani řetězcové literály - parser je odmítne.

## Identifikátory

Identifikátory jsou case-insensitive (= `A`, `a`, `pc`, `PC`, `Cf`,
`CF` jsou totožné). Maximální délka identifikátoru je 31 znaků;
přesahy se tiše ořežou.

### CPU registry (Z80)

| Identifikátor | Význam |
|---------------|--------|
| `A`, `B`, `C`, `D`, `E`, `H`, `L`, `F` | 8-bit hlavní registry |
| `BC`, `DE`, `HL`, `AF` | 16-bit páry |
| `IX`, `IY`, `SP`, `PC` | 16-bit index / stack / PC |
| `IXH`, `IXL`, `IYH`, `IYL` | 8-bit půlené index registry (high/low byte IX/IY) |
| `I`, `R` | interrupt vector / refresh registry (8-bit) |
| `IM` | aktuální interrupt mode (0/1/2) |
| `IFF1`, `IFF2` | interrupt flip-flopy (0/1) |

`PC` je živý registr. V PC_EXEC breakpointu se rovná adrese instrukce,
která se právě chystá vykonat. V MEM_R, MEM_W a IORQ breakpointu se
instrukce, která přístup dělá, už vykonává, takže `PC` ukazuje za ni:
`LD (4000h),A` na 3003h dá `PC = 3006h`. Adresu samotné instrukce dává
`InsnPC` (viz [Kontext breakpointu](#kontext-breakpointu)).

### Shadow registry

| Identifikátor | Význam |
|---------------|--------|
| `AF'`, `BC'`, `DE'`, `HL'` | 16-bit shadow páry (po `EXX` / `EX AF,AF'`) |

Apostrof je součástí identifikátoru. Lexer ho povoluje jako pokračovací
znak, takže `bc'` se naparsuje jako jeden token.

### Z80 příznaky (flags)

| Identifikátor | Bit ve `F` | Význam |
|---------------|-----------|--------|
| `Cf` | 0 | carry |
| `Nf` | 1 | add/subtract |
| `Pf` | 2 | parity / overflow (P/V) |
| `Hf` | 4 | half-carry |
| `Zf` | 6 | zero |
| `Sf` | 7 | sign |

Postfix `f` zabraňuje kolizi s registry `C` (= registr C, ne carry) a
`F` (= flag register jako celek). Hodnota je 0 nebo 1.

### Kontext breakpointu

Tato pole naplňuje breakpoint enforcement vrstva v okamžiku triggeru
podle typu BP. Pokud je výraz vyhodnocován mimo enforcement (= Test
Eval, ad-hoc), zůstávají nulová.

| Identifikátor | Význam |
|---------------|--------|
| `Address` | Memory adresa accessu (MEM_R/MEM_W/PC_EXEC); pro IORQ port hodnota |
| `Value` | Bajt čtený / zapisovaný v daném accessu |
| `IsRead`, `IsWrite`, `IsExec`, `IsPort` | Typ accessu (0/1) |
| `InsnPC` | Adresa právě vykonávané instrukce (její první bajt opkódu). V PC_EXEC shodná s `PC`; v MEM_R / MEM_W / IORQ instrukce, která přístup dělá (`PC` už je za ní). Platí i mimo enforcement (Test Eval). |
| `BankPC` | Aktivní banking zóna pro živé `PC` |
| `BankAddr` | Aktivní banking zóna pro `Address` (typicky shodná s `BankPC`) |
| `Reason` | IFF change reason - platí jen v rámci aktivního IFF*_CHANGE fire; mimo aktivní fire vrací `none` (0xFF). Viz [Reason vocabulary](#reason-vocabulary). |

Sémantika `Address` / `Value` se liší podle typu BP - viz `hw-events.md`
a `match-modes.md`. Pro IRQ / IRQ_SIG / HW_EVENT typy mohou být některá
pole irelevantní (= zůstanou 0).

### Reason vocabulary

`Reason` je ambient hodnota platná v okamžiku fire IFF*_CHANGE eventu
(typicky při změně IFF flip-flopu). Mimo aktivní fire vrací `none`
(0xFF).

| Identifikátor | Hodnota | Význam |
|---------------|---------|--------|
| `reason` | aktuální reason | čte ambient hodnotu; vrací `none` (0xFF) mimo aktivní fire |
| `reset` | 0 | IFF cleared v důsledku CPU reset |
| `ei` | 1 | IFF set v důsledku `EI` instrukce |
| `di` | 2 | IFF cleared v důsledku `DI` instrukce |
| `int_ack` | 3 | IFF cleared v důsledku INT acknowledgement (IM 0/1/2) |
| `nmi_ack` | 4 | IFF1 cleared v důsledku NMI acknowledgement (IFF2 zachováno) |
| `reti` | 5 | RETI - signal pro Z80 PIO, IFF beze změny |
| `retn` | 6 | IFF1 obnovené z IFF2 (RETN) |
| `none` | 0xFF | sentinel: mimo aktivní IFF fire |

Identifikátory jsou case-insensitive, takže `Reason`, `REASON`,
`int_ack`, `INT_ACK` jsou všechny ekvivalentní.

**Příklady použití:**

```text
Reason == nmi_ack                    ; zachytí jen NMI ack moment
Reason == reti                       ; PIO IRQ acknowledge signal
(Reason == int_ack) && (SP < 0xFFE0) ; nested-ISR detection
Reason != none                       ; je-li aktivní IFF fire stack
```

Pro decision matrix (= které reasony fírují které IFF*_CHANGE eventy)
viz [hw-events.md](hw-events.md#cpu-events).

### Globální emu state

| Identifikátor | Význam |
|---------------|--------|
| `Cycle` | Takty GDG (pixel ticks) od startu emulátoru na začátku aktuální instrukce (reset stroje čítač nenuluje, nahrání snapshotu ho nastaví na uloženou hodnotu). **Pozor:** není to čítač T-stavů Z80: jeden T-stav je 5 taktů GDG u PAL (MZ-800, MZ-700 PAL) a 4 u NTSC (MZ-700 NTSC, MZ-1500). Jako každá hodnota se ořízne na 32 bitů a po 2^31 taktech přeteče do záporných čísel, u PAL asi po 121 s emulovaného času (17 721 600 taktů za sekundu), u NTSC asi po 150 s. |
| `FrameCycle` | Takty GDG od začátku aktuálního snímku na začátku aktuální instrukce: 0 .. takty snímku - 1 (354 432 na snímek u PAL, 238 944 u NTSC). Nepřetéká, pro časování v rámci snímku. `Cycle = Frame * takty snímku + FrameCycle`, dokud `Cycle` nepřeteče. Na MZ-800 je snímek kratší, dokud je nastavený bit CKSW (port 0x07CF, v MZ notaci 0xCF07, bit 7): každý řádek trvá 1120 taktů místo 1136, celý snímek 349 440; po takovém řádku už vztah výše neplatí a `FrameCycle` snímku končí dřív. |
| `Frame` | Počet kompletních snímků od startu emulátoru (reset stroje ho nenuluje). Inkrementuje se na konci snímku. |
| `Scanline` | Aktuální GDG raster row (0..výška snímku-1). |

Per-arch zdroj: mz800 a mz1500 sdílí GDG strukturu, mz700 přebírá totéž.

### Uživatelské proměnné `$name`

`$name` v condition odkazuje na hodnotu uživatelské proměnné. Hodnota:

- existující var -> její `int32_t` hodnota
- neexistující var -> 0 (eval nikdy nevyhazuje chybu)

Jména `$name` jsou validovaná regex `^[a-zA-Z_][a-zA-Z0-9_]*$` (= první
znak alfa nebo `_`, pokračování alfa / číslice / `_`). Max 31 znaků.
Žádné reserved keywords (= `$` prefix je sám distinktivní, takže
`$if`, `$set`, `$log` jsou platná uživatelská jména).

Lifetime proměnných je per-emulator-session; persistence v `.bpt` JSON
sekci `"vars"` plus per-arch standalone `.vars` souboru (= `mz800.vars`
/ `mz1500.vars`). Storage je sdílený napříč BP - jeden BP může číst
proměnnou nastavenou jiným. Detail viz `vars.md`.

Zápis do proměnných probíhá výhradně z action DSL (`$name op= expr` -
viz `action-dsl.md`); ve výrazu samotném je `$name` jen čtení.

Z UI panelu `Variables` lze proměnné interaktivně Add / Edit / Delete
/ Rename, viz `vars.md`.

## Operátory

Precedence shora dolů (vyšší = přednost), asociativita standardní C-like.

| Skupina | Operátory | Asociativita |
|---------|-----------|--------------|
| Závorky | `( )` | - |
| Unární | `+`, `-`, `!`, `~` | pravá |
| Multiplikativní | `*`, `/`, `%` | levá |
| Aditivní | `+`, `-` | levá |
| Bit shift | `<<`, `>>` | levá |
| Relační | `<`, `<=`, `>`, `>=` | levá |
| Equality | `==`, `!=` | levá |
| Bit AND | `&` | levá |
| Bit XOR | `^` | levá |
| Bit OR | `\|` | levá |
| Logické AND | `&&` | levá, short-circuit |
| Logické OR | `\|\|` | levá, short-circuit |

Sémantika:

- Dělení / modulo nulou -> 0 (evaluator je tolerantní, žádná výjimka).
- Shift count se maskuje na 5 bitů (`r & 31`).
- Logické `&&` / `||` provádí short-circuit (= pravý operand se nevyčíslí,
  pokud levý určí výsledek). Užitečné pro guarding memory derefů.
- `!=` a `==` vrací 0/1, ne C-style boolean (= bezpečné v aritmetice).
- Single `=` není přiřazení a parser ho odmítne s chybou
  "expected '==' (single '=' is not assignment)".

## Memory dereference

| Tvar | Význam |
|------|--------|
| `[addr]` | Čtení 8 bitů z aktuální mapy paměti CPU (RAM, ROM, VRAM) bez spuštění MEM_R breakpointů |
| `[addr]!` | Stejné čtení, ale MEM_R breakpointy na `addr` se vyhodnotí (mohou se spustit a zastavit emulaci) |
| `{addr}` | Čtení 16 bitů (little-endian) - dvě 8bitová čtení jako `[addr]` |

Oba tvary respektují aktuální banking. V režimu MZ-700 se čipy namapované
na 0E000h-0E008h (8255, 8253, status GDG) v obou tvarech čtou bez
vedlejšího efektu: autotype se neposune, klávesnice ani joystick
hostitele se nenačtou a klopný obvod LSB/MSB 8253 se nepřepne. Čtení se
skutečnými vedlejšími efekty čipu dává `port[N]!`.

`{addr}` (16-bit word) nemá variantu se side-effect flagem.

Adresa může být libovolný výraz: `[HL]`, `[HL+1]`, `[0x8000 + A]`,
`{SP}` (= top stacku jako word).

## I/O port read

| Tvar | Význam |
|------|--------|
| `port[N]` | Side-effect-free probe IORQ portu N |
| `port[N]!` | Real read = stejné jako CPU `IN` instrukce (= má side effecty) |

Probe sémantika **per chip**:

| Port range | Chip | `port[N]` (probe) | `port[N]!` (real) |
|------------|------|-------------------|--------------------|
| 0xF0 / 0xF1 (MZ-800 only) | Joystick | real (čistá funkce bez side-effectů) | stejné jako probe |
| 0xE0-0xE6 | Memory mapper / banking | mirror cache (last-write) | bus latch (chip je write-only) |
| 0xCE | GDG DMD status | mirror cache / fallback | real read (má strobe side-effect) |
| 0xD0-D3 | PIO 8255 (PPI) | Port B keyboard row + Control Word cache | real read (PortC tape state side-effect) |
| 0xD4-D6 | CTC 8253 | counter decode + Control Word cache | real read (counter latch side-effect) |
| 0xD8-DB | FDC WDC | status / track / sector mirror | real read (status latch side-effect) |
| 0xFC-FF | PIO Z80 | Control Word last-write cache | real read (IRQ ack side-effect) |
| Ostatní (unconnected) | - | bus latch / 0xFF | bus latch |

**Použití:**

- `port[N]` (default, **doporučeno**): probe pro condition eval -
  safer, netriggeruje HW side-effects při každém volání. Pro porty,
  které mají mirror cache, vrací snapshot chip state. Pro
  side-effect-heavy nebo non-mirror porty vrací bus latch jako
  fallback.
- `port[N]!`: použít jen když uživatel explicit chce side-effect
  (= např. číst current GDG status, PIO 8255 PortC live state).
  V každé evaluaci podmínky se chová jako Z80 `IN` instrukce.

`port` je rezervované klíčové slovo (case-insensitive) a parser ho
rozezná před `[` jako I/O variantu - rozlišení od memory deref `[addr]`
funguje díky prefixu `port`.

## Built-in funkce

Identifikátory funkcí jsou case-insensitive. Argumenty jsou výrazy.

| Volání | Arita | Sémantika |
|--------|-------|-----------|
| `min(a, b)` | 2 | menší ze dvou |
| `max(a, b)` | 2 | větší ze dvou |
| `abs(x)` | 1 | absolutní hodnota |
| `if(cond, a, b)` | 3 | `cond ? a : b` (eval jen vybranou větev) |
| `bit(value, n)` | 2 | hodnota bitu n (0/1); n se maskuje na 0..31 |
| `s8(x)` | 1 | sign-extend low byte (= `(int8_t)(x & 0xFF)`) |
| `s16(x)` | 1 | sign-extend low word (= `(int16_t)(x & 0xFFFF)`) |
| `s32(x)` | 1 | identita pro 32-bit (= `x`) |
| `lastwriter(addr)` | 1 | adresa instrukce, která naposledy zapsala paměťovou buňku na CPU adrese `addr` (aktuální mapování; grafická VRAM MZ-800 = nejnovější z jejích plánů) podle [záznamu zápisů](../ram-write-log.md); `-1` = od spuštění nebo vyprázdnění záznamu nezapsána, záznam se nespustil, nebo port mapovaný do paměti (po zastavení záznamu hodnota zůstává) |

Špatná arita (= jiný počet argumentů) je parser-time chyba. Funkce
mimo seznam vrací parse error "unknown function 'name'".

`if(cond, a, b)` je jediná funkce s lazy evaluation - cond se vyčíslí
vždy, pak jen jedna z větví `a` / `b`. Užitečné pro guarding memory
derefů: `if(HL >= 0x8000, [HL], 0)`.

## Příklady

```
A == 0x42
```
Trigger jen když registr A obsahuje 0x42.

```
PC >= 0x1000 && PC < 0x2000
```
Range check pro PC.

```
HL && [HL] == 'X'
```
Trigger jen pokud HL je nenulové (= guard) a bajt na adrese HL je `'X'`
(= 0x58). `'X'` jako literál neexistuje, takto by selhalo - správně
`HL && [HL] == 0x58`.

```
{SP} == 0x1234
```
Top stacku (16-bit LE) je 0x1234.

```
Cf && IsWrite
```
Carry flag set a access je memory write.

```
$hits >= 5
```
Uživatelská proměnná `$hits` dosáhla 5 (proměnnou typicky inkrementuje
action DSL téhož nebo jiného BP).

```
bit(port[0xCE], 7) == 1
```
Bit 7 hodnoty čtené z portu 0xCE. Pro side-effect-heavy chipy (GDG DMD
je v této kategorii) probe vrací mirror cache nebo bus latch - pro
real strobe-driven status použijte `port[0xCE]!`.

```
if(HL >= 0x8000 && HL < 0xC000, [HL], 0) == 0xFF
```
Bezpečné čtení paměti s rozsahem (= mimo VRAM oblast vrátí 0).

```
AF' == 0
```
Shadow AF je nulové (= ještě neproběhl `EX AF,AF'`).

## Návratová hodnota výrazu

Výraz vrací jednu `int32_t` hodnotu. Pro condition pole BP platí:

- 0 = false -> BP nestřelí, hit counter nepřipočte
- ne-nula = true -> BP střelí, akce se provede

Pokud je condition prázdná, breakpoint se chová jako "always true"
(BP střelí při každém hit-u dispatch hooku).

## Chování při chybách

### Parse-time

Lexer / parser chyby (špatný literál, neuzavřená závorka, neznámá
funkce, špatná arita, single `=`, ...) zobrazí inline error pod
textboxem v Edit BP dialogu. BP s neparsovatelným condition se chová
jako "always true fallback" (= střelí při každém hit-u), dokud
neopravíte.

### Runtime

Evaluator je tolerantní:

- Neznámý identifikátor -> 0
- Dělení / modulo nulou -> 0
- NULL CPU kontext -> registry resolvují na 0
- `$name` neexistující -> 0

Žádná z těchto situací nezhroutí evaluator ani BP enforcement.

## Omezení výrazového jazyka

- Žádné loops / iteration ve výrazu.
- Žádné uživatelsky definované funkce (= jen built-iny).
- Žádné stringy nebo arrays (jen skalární `int32_t`).
- `Cycle` vrací GDG pixel ticks, ne čistý Z80 T-state counter, a asi po
  121 s emulovaného času (PAL) přeteče; v rámci snímku použijte
  `FrameCycle`.
- Žádný explicit `unsigned` typ - posuny a porovnání pracují signed,
  pro unsigned compare buď maskujte (`a & 0xFFFF`) nebo používejte
  `s8` / `s16` k explicit sign-extension.

## Reference

- Action DSL (zápis do `$name`, použití výrazu jako argumentu):
  `action-dsl.md`
- Sémantika `Address` / `Value` per typ BP: `hw-events.md`,
  `match-modes.md`
- IRQ-specifické kontextové pole: `irq-filter.md`, `irq-sig.md`
- Uživatelské proměnné: `vars.md`
- Globální symboly pro `%s` v action DSL: `../symbols.md`
