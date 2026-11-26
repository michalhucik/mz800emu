# Breakpointy - dokumentace

Tato složka obsahuje uživatelskou dokumentaci k breakpoint subsystému
emulátoru mz800emu. Cílem subsystému je poskytnout "smart" breakpointy,
které jsou víc než klasický PC stop - mají typový aparát (memory access,
I/O, IRQ, HW event, ...), match módy (single / range / mask), volitelnou
condition expression a action mini-DSL.

## Co breakpointy umí

- 9 BP typů (PC_EXEC, MEM_R, MEM_W, IORQ_R, IORQ_W, IRQ, HW_EVENT,
  SP_THRESHOLD, GLOBAL, IRQ_SIG) - viz `types.md`
- Match módy SINGLE / RANGE / MASK pro adresové, portové a bank fieldy -
  viz `match-modes.md`
- IORQ port mode 8BIT / 16BIT - viz `match-modes.md`
- IRQ post-dispatch filter s IM mode discriminator + IM 2 vector / ISR
  filter + IM 0 RST opcode mask - viz `irq-filter.md`
- IRQ_SIG pre-dispatch peripheral source filter - viz `irq-sig.md`
- HW_EVENT vocabulary 28 events ve 4 kategoriích + trigger conditions -
  viz `hw-events.md`
- Condition expression evaluator se signed 32-bit aritmetikou, CPU
  registry, flagy, shadow regs, memory deref, I/O port read, $vars -
  viz `expression-syntax.md`
- Action mini-DSL s 11 příkazy (log, set, poke, $var, if, enable,
  disable, mark, ...) - viz `action-dsl.md`
- Persistence v JSON `.bpt` formátu s tolerancí starších souborů -
  viz `persistence.md`
- Live Test Eval s reálným CPU kontextem

## Mapa dokumentů

| Soubor | Popis | Otevřete když |
|--------|-------|---------------|
| `README.md` | tento soubor (orientace) | poprvé |
| `types.md` | katalog 9 BP typů, feature matrix, decision tree | vyberete typ pro nový BP |
| `match-modes.md` | SINGLE / RANGE / MASK + IORQ port_mode + SP WINDOW | nastavujete adresový rozsah / mask |
| `expression-syntax.md` | gramatika condition expression | píšete podmínku do `condition` |
| `action-dsl.md` | příkazy action mini-DSL | píšete skript do `action` |
| `hw-events.md` | 28 HW events vocabulary + trigger conditions | typ HW_EVENT |
| `irq-filter.md` | post-dispatch IRQ filter (IM mode + vector + ISR + RST) | typ IRQ |
| `irq-sig.md` | pre-dispatch peripheral source filter | typ IRQ_SIG |
| `persistence.md` | `.bpt` JSON schema + BC tabulka | editujete `.bpt` ručně / migrujete |
| `groups.md` | BP skupiny (hierarchie + cascade enable + drag-drop) | organizujete BPs do skupin |
| `test-eval.md` | Live Test Eval (real ctx) | testujete výrazy před uložením |
| `vars.md` | `$vars` user variables (storage, action, expression, file ops, UI panel) | používáte `$name` v podmínkách / akcích |

## Quick-start - vytvoření prvního BP

1. Otevřete Breakpoints okno (klávesa Alt+B nebo menu).
2. V Breakpoints klikněte pravým tlačítkem myši "Add Breakpoint Event...".
3. V Edit BP dialogu nastavte:
   - **Type** - vyberte typ z dropdownu (default `PC_EXEC` = klasický
     PC breakpoint). Viz `types.md` pro popis ostatních typů.
   - **Address** (nebo Port / Event / SP threshold) - závisí na typu.
   - **Name** - volitelné, jinak se vygeneruje automaticky podle adresy
     (např. `Addr: 0x1234`).
4. Volitelně:
   - **Match Mode** - SINGLE (default) / RANGE / MASK. Pro RANGE
     vyplňte druhé pole `End Addr`.
   - **Mode** (radio) - Stop / Log only / Custom DSL.
     - Stop = klasický breakpoint, zastaví emulátor.
     - Log only = jen vypíše do TTY a pokračuje (ekvivalent action
       string `log "BP <jmeno>"`).
     - Custom = otevře action textbox pro skript - viz `action-dsl.md`.
   - **Condition** - volitelný výraz, BP fire jen pokud vrátí non-zero -
     viz `expression-syntax.md`.
   - **Hit count** - trigger až po N. hitu (0 = každý hit).
   - **Skip count** - skip prvních N hitů.
5. Stiskněte OK. BP je okamžitě aktivní.
6. BP persist do `.bpt` souboru při exitu (pokud `auto_save = 1` v
   konfiguraci) nebo manuálně přes Save BP.

## Architektura - vysoká úroveň

### Datový model

Centrální struktura breakpointu obsahuje všechny fieldy pro všech
9 typů. Pole nepoužívaná pro daný typ se ignorují (typicky zůstávají
na default). Hlavní kontejner obsahuje pole breakpointů a pole skupin
(hierarchie).

### Per-typ enforcement

Každý BP typ má vlastní enforcement hook v emu vrstvě:

| Typ | Volá se z |
|-----|-----------|
| PC_EXEC | hot loop pre-instruction |
| MEM_R | CPU read v debugger active mode (jen data reads - viz filtrace M1 fetch níže) + `[addr]!` deref ve výrazu; čtení samotným debuggerem nefiruje |
| MEM_W | CPU write v debugger active mode + STMT_POKE; zápis samotným debuggerem nefiruje |
| IORQ_R | CPU port read |
| IORQ_W | CPU port write |
| IRQ | po Z80 INT dispatch (POST-dispatch) |
| IRQ_SIG | PRE-dispatch (edge detection na INT bus) |
| HW_EVENT | hook sites v emu (vsync, ctc, palette, ...) |
| SP_THRESHOLD | hot loop po SP změně |
| GLOBAL | per-instruction (default OFF) |

Hot path optimalizace: před voláním enforce funkcí caller testuje
per-typ aktivní flag (default false = zero overhead v default OFF
stavu). HW_EVENT navíc per-event flag.

### MEM_R / MEM_W hook semantics (důležité)

Hooky MEM_R a MEM_W jsou volány **PŘED** samotným memory access (čtením
/ zápisem). Condition může číst "starou" hodnotu přes `[addr]`
no-side-effect deref.

To znamená:

- **Akce nemůže substituovat zapisovanou hodnotu.** Pro MEM_W: `value`
  je input (= co BUDE zapsáno), nelze přepsat zpětnou vazbou.
- **Original write proběhne vždy** po návratu z enforce hooku, bez
  ohledu na akci.
- Pokud akce `poke <addr> <value>` zapíše na **stejnou adresu**, je to
  **separátní memory write volání** - proběhne v rámci akce, pak se
  vrátí kontrola a CPU dokončí původní write (= efektivně přepíše
  hodnotu z `poke` zpět na CPU value).
- Pro skutečné "intercept and modify" semantics by byl potřeba
  write-back přes pointer; aktuální verze breakpoints to neumí.

### MEM_R fire jen na data reads (filtrace M1 fetch)

CPU read rozlišuje 2 typy přístupu (reuse z CDL recording klasifikace):

- **X (eXecute)** - bajt patřící právě dekódované instrukci: M1 opcode
  fetch, M1 prefix opcode (DD/FD/ED/CB) uvnitř téže instrukce, sekvenční
  immediate operand bajt.
- **R (data Read)** - jakýkoliv jiný read (např. `LD A,(HL)`,
  `LD A,(nn)`, `POP`, `IN A,(C)` data fetch, ...).

**MEM_R BP fire jen pro `access_kind == R`.** Bez filtrace by každá
instrukce vyvolala MEM_R fire pro každý opcode/operand byte (= spam,
typicky 2-4x per instrukce), což znehodnocuje BP type pro use-case
"chci vědět kdy program čte data z X".

Filtrace platí **jen v CPU read path**. Deref `[addr]!` ve výrazu nemá
M1 koncept a fire vždy (= manuální čtení by nemělo být filtrováno);
`[addr]` bez `!` nefiruje.

Pozor: instruction fetch z mid-instruction adresy (= např.
self-modifying code, ROM banking během dekódování) je teoreticky stále
X, dokud byte_position nepřekročí limit a addr navazuje. Pro běžný
kód není problém.

### `poke` self-write rekurze (pozor)

`poke` v action handleru vyvolá memory write, což může reentrovat
enforce vrstvu. Pokud BP MEM_W na adrese X má action `poke X, Y`:

1. CPU začne zápis na X -> hook fire -> action eval
2. Action `poke X, Y` -> memory write -> hook ZNOVU
3. Druhé volání hook s `value = Y` -> condition stále match -> action ZNOVU

**Re-entry guard** používá globální depth counter s limitem 4 úrovní.
Po překročení vypíše warning na stderr a action skipuje. Emulátor
pokračuje bez stack overflow, ale uživatel se dozví, že má run-away
rekurzi.

**Mitigation patterns** (= preferované, mimo guard):

- `poke` na **jinou adresu** (= write side-effect mimo trigger addr):
  bezpečné, žádná rekurze.
- `disable_self` v action **PŘED** `poke X, Y`: po prvním fire BP se
  vypne, recursive trigger nezafire.
- Condition expression chytá self-write: např. `Value != 42` v condition
  + action `poke Address, 42` -> po prvním poke je hodnota 42, condition
  nepasuje, recurse exit.

Pro MEM_R je analogie symetrická (= action `poke` write na MEM_R
sledovanou adresu fire MEM_W hook, ne MEM_R, ale pozor na MEM_R
recursive read přes condition `[addr]`).

### Match modes

Pro 16-bit fieldy (PC, MEM addr, IO port) i 8-bit (bank ID) se
používají match módy SINGLE / RANGE / MASK. Semantika v
`match-modes.md`.

### Condition expression + Action DSL

Sdílený expression evaluator podporuje signed 32-bit aritmetiku, CPU
registry (Z80 hlavní + shadow), flagy postfix `f`, memory deref
`[addr]` / `{addr:w}`, I/O port read `port[N]`, $vars, 8 built-in
funkcí. Plná gramatika v `expression-syntax.md`.

Action mini-DSL má 11 příkazů: `log`, `continue`, `disable_self`,
`enable <name>`, `disable <name>`, `poke`, `set <reg>`, `mark`,
`$var = expr` (+ compound `+=` `-=` ... `>>=`), `clear_vars`,
`if <expr> then <stmt>`. Detail v `action-dsl.md`.

### HW events vs IRQ vs IRQ_SIG

Tři různé pohledy na interrupt subsystém:

- **HW_EVENT** s eventem typu `irq:fdc` / `irq:pioz80_a` / `irq:ctc2` -
  observer u zdroje IRQ signálu, **před** jakoukoliv dispatch logikou.
  Trigger condition (rising / falling / level) za `event_trigger`.
- **IRQ_SIG** (pre-dispatch) - per-source filter na edge raise INT
  line. Volá se těsně **před** Z80 INT acknowledge. Bitmask sources
  (PIOZ80_A, PIOZ80_B, CTC2, FDC, OTHER) - viz `irq-sig.md`.
- **IRQ** (post-dispatch) - po dokončení Z80 INT dispatch (= jump na
  ISR adresu). Filter na IM mode, IM 0 RST opcode, IM 2 vector address
  (`(I << 8) | vec`) a ISR target adresa - viz `irq-filter.md`.

### Groups + vars

Hierarchie skupin (parent / child) s cascade enable - pokud parent
disabled, children se nevyhodnocují. Color inheritance. Persistence v
`.bpt` JSON sekci `groups`. Detail v `groups.md`.

`$vars` (per-session storage) sdílené napříč všemi BP. Životnost =
emulator session, persist do `.bpt` JSON sekce `vars` plus per-arch
standalone `.vars` file (`mz800.vars` / `mz1500.vars` / `mz700.vars`).
Read v expression `$name`, write v action `$name op= expr` (= 10
compound operátorů). Plus comment field per-var + `persist_value` flag
(= true uloží value, false ukládá jen name + comment a value reset
na 0 při load - pattern pro counter).

UI panel `Variables` (= menu Debugger) umožňuje Add / Edit / Delete /
Rename interaktivně + bulk ops + file ops (Save / Save As / Load From
/ Merge / Clear Values / Clear All). Detail v `vars.md`.

### Persistence

`.bpt` soubor je human-readable JSON (pretty print indent 2). Cesta z
INI sekce `[BREAKPOINTS]` `default_file`. Auto-save / auto-load
volitelné. Per-klíč BC fallback při načítání starších souborů
(chybějící klíče = defaulty). Detail schématu v `persistence.md`.
