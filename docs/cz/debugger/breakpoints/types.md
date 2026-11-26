# Breakpoint typy

Debugger podporuje 9 typů breakpointů. Tento dokument je jejich katalog
se sémantikou, použitými poli a typickými use cases.

## Přehled

| String ID | Popis |
|-----------|-------|
| `PC_EXEC` | execution na CPU adrese |
| `MEM_R` | memory read |
| `MEM_W` | memory write |
| `IORQ_R` | I/O port read |
| `IORQ_W` | I/O port write |
| `IRQ` | post-dispatch IRQ |
| `HW_EVENT` | pojmenovaný HW event |
| `SP_THRESHOLD` | stack overflow / window |
| `GLOBAL` | bez adresy, jen condition |
| `IRQ_SIG` | pre-dispatch peripheral IRQ signal |

## PC_EXEC

**Sémantika:** klasický breakpoint - fire před vykonáním instrukce na
dané adrese (= před fetch a decode).

**Relevantní pole:**

- Address - primární adresa (pro RANGE i jako lower bound)
- End Addr - upper bound pro RANGE mode
- Match Mode - SINGLE / RANGE / MASK
- Mask - AND mask pro MASK mode
- Zone a Bank ID (s vlastním Match Mode pro bank) - banking-aware filter
  (viz `match-modes.md`)
- společná pole: enabled, name, condition expression, action, hit count,
  skip count, edge-triggered, barvy

**UI pole:** Type dropdown, Address textbox (hex), End Addr (RANGE),
Mask (MASK), Zone dropdown, Bank ID (MMEXT_BANK).

**Match Mode support:** SINGLE / RANGE / MASK pro adresu, SINGLE /
RANGE / MASK pro bank ID (jen v zone MMEXT_BANK).

**Use case:**

- Zastavit při dosažení vstupního bodu rutiny: `Type: PC_EXEC, Address:
  0x1200`.
- Trace celého code segmentu (RANGE): `Address: 0x1000, End Addr:
  0x10FF, Match Mode: RANGE, Action: log "PC=%X A=%X", PC, A`.
- Watch dispatch table (MASK): `Address: 0x4000, Mask: 0xFFF0` -
  trigger pro 0x4000..0x400F (= dispatch slot bez ohledu na low nibble).

**Detail:** `match-modes.md`.

## MEM_R

**Sémantika:** fire když CPU čte byte z paměti na zadané adrese.
Čtení samotným debuggerem (disassembler, Memory Browser, Watch, zásobník,
MCP `mem_read` a `dasm`) ho nespustí.

**Relevantní pole:** stejné jako PC_EXEC (addr / addr_end / mask /
zone / bank). Zone awareness: `MEM_R` v zone `ROM_LOWER` střelí jen
pokud čtená adresa je aktuálně mapovaná v ROM (= banking-aware).

**Kontext pro condition expression:**

- `Address` = čtená adresa
- `Value` = bajt který se právě čte
- `IsRead = 1`, `IsWrite = 0`, `IsExec = 0`, `IsPort = 0`
- `BankAddr` = aktivní zone pro `Address`
- `InsnPC` = adresa čtoucí instrukce (`PC` už je za ní)

**Use case:**

- Watch zápisu/čtení proměnné: `Type: MEM_R, Address: 0xE100, Action:
  log "read $%X = %X (PC=%X)", Address, Value, InsnPC`.
- Detekovat ROM dump pokus: `Type: MEM_R, Address: 0x0000, End Addr:
  0x0FFF, Match Mode: RANGE`.

**Detail:** `match-modes.md`, `expression-syntax.md`.

## MEM_W

**Sémantika:** stejné jako MEM_R, ale pro zápis. Fire před fyzickým
zápisem (= condition může číst starou hodnotu). Zápis debuggerem (Memory
Browser, MCP `mem_write` a `region_write`, zmrazení bajtů) ho nespustí,
akce `poke` z Action DSL ano.

**Kontext:**

- `Address` = cílová adresa
- `Value` = zapisovaný bajt
- `IsRead = 0`, `IsWrite = 1`, `IsExec = 0`, `IsPort = 0`

**Use case:**

- Detekovat zápis do nepřípustné oblasti: `Type: MEM_W, Address:
  0xC000, End Addr: 0xCFFF, Match Mode: RANGE`.
- Trigger po naplnění magic value: `Type: MEM_W, Address: 0xE000,
  Condition: Value == 0x42`.

## IORQ_R

**Sémantika:** fire při I/O port read instrukci (`IN A,(n)`,
`IN r,(C)`, `INI`, `IND`, ...). Fire po fyzickém čtení (= condition
vidí už přečtenou hodnotu).

**Relevantní pole:**

- Port - primární port (pro RANGE i jako lower bound)
- End Port - upper bound pro RANGE
- Port Match Mode - SINGLE / RANGE / MASK
- Port Mask - AND mask
- Port Mode - **8BIT** (default) = match jen low byte portu; **16BIT**
  = match plný BC pattern
- společná pole

**Match Mode:** SINGLE / RANGE / MASK na portu. Plus `port_mode`
(8BIT / 16BIT) - viz `match-modes.md` sekce "IORQ port mode".

**Kontext:**

- `Address` = port hodnota (8 nebo 16 bit dle `port_mode`)
- `Value` = přečtený bajt
- `IsPort = 1`, `IsRead = 1`

**Use case:**

- Watch CTC port: `Type: IORQ_R, Port: 0xCC, Port Mode: 8BIT`.
- Detekovat 16-bit IO probing: `Type: IORQ_R, Port: 0x42CE, Port Mode:
  16BIT, Match Mode: SINGLE`.

## IORQ_W

**Sémantika:** dtto IORQ_R, ale pro zápis (`OUT (n),A`, `OUT (C),r`,
`OUTI`, `OUTD`, ...). Fire po fyzickém zápisu.

**Kontext:**

- `Address` = port
- `Value` = zapisovaný bajt
- `IsPort = 1`, `IsWrite = 1`

**Use case:**

- Trace zápisů do GDG palety: `Type: IORQ_W, Port: 0xF0, End Port:
  0xF3, Match Mode: RANGE, Action: log "PAL[%X] = %X", Address & 3,
  Value`.

## IRQ

**Sémantika:** fire **POST-dispatch** - po dokončení Z80 INT
acknowledge cyklu (= PC už ukazuje na ISR jump target).

**Relevantní pole:**

- IM 0/1/2 enabled - per-IM mode discriminator. Aspoň jeden musí být
  true (= UI validation), jinak BP nikdy nefire. Default pro nový IRQ
  BP = all true.
- IM 0 RST mask - 8-bit bitmask pro filter IM 0 RST opcode (bit 0 =
  RST 00h opcode 0xC7, bit 1 = RST 08h 0xCF, ... bit 7 = RST 38h 0xFF).
  Mask 0 = match-all.
- IM 2 Vector filter (enable + addr + Match Mode + end + mask) - filter
  na IM 2 vector table address `(I << 8) | vec`. Porovnává se celá
  adresa včetně bitu 0 (Z80 bit 0 vektoru nemaskuje); bit 0 lze vynechat
  režimem MASK s maskou 0xFFFE.
- IM 2 ISR filter (enable + addr + Match Mode + end + mask) - filter
  na ISR jump target (= PC po dispatch).
- společná pole

**Match Mode:** SINGLE / RANGE / MASK pro IM 2 vector i ISR.

**UI pole:** Type dropdown, IM 0/1/2 checkboxy, IM 0 RST opcode
checkboxy (8 bitů), IM 2 Vector enable + addr + match mode,
IM 2 ISR enable + addr + match mode.

**Kontext:**

- `Address` = vector_addr (pro IM 2) nebo 0 (jinak)
- `Value` = raw INT vector byte na sběrnici
- ostatní pole standard

**Use case:**

- Zastavit při dispatch IM 1 (legacy MZ-700 BIOS hook): `Type: IRQ,
  IM1 only`.
- Watch konkrétní IM 2 ISR: `Type: IRQ, IM2 only, IM2 ISR enabled,
  ISR addr: 0x8800`.
- Detect runaway RST 38h (= NULL pointer call): `Type: IRQ, IM0 only,
  RST mask bit 7 set`.

**Detail:** `irq-filter.md`.

## HW_EVENT

**Sémantika:** fire při pojmenovaném HW eventu z emu kódu - např.
vsync, CTC zero-cross, GDG palette write, CMT signal change. Celkem
28 events ve 4 kategoriích (signal / change / point-param /
point-noparam).

**Relevantní pole:**

- Event name - persistence název (např. `vsync`, `ctc:zc0`, `raster:192`)
- Event Param - parametr (např. raster row pro `raster:N`)
- Event Trigger - trigger condition (RISING / FALLING / CHANGED /
  LOW / HIGH); aplikuje se jen pro signal eventy. Default RISING.
- společná pole

**Match Mode:** event-specific. Signal eventy - trigger condition. Point
events s parametrem - event_param hodnota. Change events - implicit
"happened".

**UI pole:** Type dropdown, Event dropdown (28 named events), Param
textbox (jen pro `raster:N`), Trigger dropdown (jen pro signal events).

**Kontext (per-kind):**

| Kind | `Address` | `Value` |
|------|-----------|---------|
| SIGNAL | 0 | aktuální signal level (0/1) |
| CHANGE | 0 | nová hodnota (mode/palette/palgrp/border) |
| POINT_PARAM | parametr (např. raster row) | 0 |
| POINT_NOPARAM | 0 | info data (IM/IFF new value) |

**Use case:**

- Frame stop: `Type: HW_EVENT, Event: vsync, Trigger: rising` =
  zastaví na začátku každého frame.
- Mid-frame palette change detect: `Type: HW_EVENT, Event:
  palette_change`.
- Raster line trigger: `Type: HW_EVENT, Event: raster, Param: 192`.
- NMI dispatch: `Type: HW_EVENT, Event: cpu:nmi`.

**Detail:** `hw-events.md`.

## SP_THRESHOLD

**Sémantika:** stack-related BP s edge-triggered logikou. Dva módy:

- **SINGLE** - fire při sestupném crossingu jednoho prahu
  (`old_sp >= sp_threshold && new_sp < sp_threshold`). Sustained stav
  pod prahem už nefire dokud SP nestoupne nad threshold a znovu
  neklesne (= stack overflow detect bez spamu).
- **WINDOW** - fire pokud SP **opustí** rozsah `[sp_threshold..sp_upper]`
  (= byl uvnitř, teď je venku). Edge-triggered.

Fire po každé instrukci, pokud SP změnil hodnotu.

**Relevantní pole:**

- SP threshold - dolní hranice (SINGLE: práh; WINDOW: lower bound)
- SP upper - horní hranice (jen WINDOW; pokud `hi < lo`, runtime
  defenzivně prohodí)
- SP Mode - SINGLE / WINDOW
- společná pole

**Match Mode:** SP mode je oddělený enum (žádný addr_match_mode).

**UI pole:** Type dropdown, SP Mode radio (Single / Window),
SP threshold textbox, SP Upper textbox (Window).

**Kontext:**

- `Address` = aktuální SP (new_sp)
- `Value` = SP threshold (= práh)

**Use case:**

- Stack overflow detect: `Type: SP_THRESHOLD, SP Mode: Single,
  SP threshold: 0x4000` - fire pokud SP klesne pod 0x4000.
- Cross-task switch: `Type: SP_THRESHOLD, SP Mode: Window, lower:
  0xF000, upper: 0xFFFF`.

## GLOBAL

**Sémantika:** BP bez adresy / portu / eventu - jen condition expression
vyhodnocovaná **per-instruction**. Drahý (= per-instruction overhead),
takže default OFF a vyhodnocuje se jen pokud aspoň jeden GLOBAL BP
existuje.

Edge-triggered semantika: pokud `edge_triggered = true`, fire jen při
přechodu condition `false -> true`.

**Relevantní pole:**

- Condition expression (povinný, jinak BP fire každou instrukci)
- Edge-triggered flag
- společná pole

**Match Mode:** žádný (= jen condition).

**UI pole:** Type dropdown, Condition expression (mandatory), Edge
trigger checkbox.

**Kontext:** `Address = 0`, `Value = 0`, `Is*` všechny 0. K dispozici
jsou registry CPU a `BankPC`.

**Use case:**

- Watch konkrétního stavu CPU: `Type: GLOBAL, Condition: HL == 0x8000
  && A == 0x42, edge`.
- Detect IFF1 transition: `Type: GLOBAL, Condition: IFF1, edge`.

## IRQ_SIG

**Sémantika:** fire **PRE-dispatch** - před Z80 INT acknowledge, na
edge raise INT line (prev=0, curr=1) konkrétního peripheral source.

Doplňuje IRQ (post-dispatch). Use case: detekovat IRQ requests které
CPU **nedispatchne** (= EI maskování, nebo INT line raise když IFF1=0)
- IRQ BP by je nikdy neviděl.

**Relevantní pole:**

- Source mask - 8-bit bitmask:
  - bit 0 = Z80 PIO port A
  - bit 1 = Z80 PIO port B
  - bit 2 = CTC channel 2
  - bit 3 = WD279x FDC
  - bit 4 = nedetekovatelný / bus latch (Other)
- společná pole

**Match logic:** OR semantics. BP fire pokud
`(source_mask & active_sources) != 0`. Multi-source BP
= libovolný z vybraných.

**UI pole:** Type dropdown, Source checkboxes (5 sources). Aspoň
jeden musí být zaškrtnutý (= UI validation), jinak BP nikdy nefire.

**Kontext:** `Address = 0`, `Value = active_sources` (raw bitmask
z aktuálního edge).

**Use case:**

- Watch FDC IRQ requests (= včetně masked): `Type: IRQ_SIG, Source:
  FDC`.
- Detect race mezi PIOZ80 port A a CTC2: `Type: IRQ_SIG, Sources:
  PIOZ80_A + CTC2`.

**Detail:** `irq-sig.md`.

## Feature matrix

Které pole je relevantní pro který typ. **Y** = relevantní, **-** =
ignorováno (= default 0/NULL).

| Field | PC_EXEC | MEM_R | MEM_W | IORQ_R | IORQ_W | IRQ | HW_EVENT | SP_THR | GLOBAL | IRQ_SIG |
|-------|---------|-------|-------|--------|--------|-----|----------|--------|--------|---------|
| addr | Y | Y | Y | - | - | - | - | - | - | - |
| addr_end | Y | Y | Y | - | - | - | - | - | - | - |
| addr_match_mode | Y | Y | Y | - | - | - | - | - | - | - |
| addr_mask | Y | Y | Y | - | - | - | - | - | - | - |
| port | - | - | - | Y | Y | - | - | - | - | - |
| port_end | - | - | - | Y | Y | - | - | - | - | - |
| port_match_mode | - | - | - | Y | Y | - | - | - | - | - |
| port_mask | - | - | - | Y | Y | - | - | - | - | - |
| port_mode | - | - | - | Y | Y | - | - | - | - | - |
| zone | Y | Y | Y | - | - | - | - | - | - | - |
| bank_id | Y | Y | Y | - | - | - | - | - | - | - |
| bank_match_mode | Y | Y | Y | - | - | - | - | - | - | - |
| bank_id_end | Y | Y | Y | - | - | - | - | - | - | - |
| bank_id_mask | Y | Y | Y | - | - | - | - | - | - | - |
| event_name | - | - | - | - | - | - | Y | - | - | - |
| event_trigger | - | - | - | - | - | - | Y* | - | - | - |
| event_param | - | - | - | - | - | - | Y* | - | - | - |
| sp_threshold | - | - | - | - | - | - | - | Y | - | - |
| sp_upper | - | - | - | - | - | - | - | Y* | - | - |
| sp_mode | - | - | - | - | - | - | - | Y | - | - |
| im0_enabled | - | - | - | - | - | Y | - | - | - | - |
| im1_enabled | - | - | - | - | - | Y | - | - | - | - |
| im2_enabled | - | - | - | - | - | Y | - | - | - | - |
| im0_rst_mask | - | - | - | - | - | Y* | - | - | - | - |
| im2_vector filter | - | - | - | - | - | Y* | - | - | - | - |
| im2_isr filter | - | - | - | - | - | Y* | - | - | - | - |
| irq_sig_source_mask | - | - | - | - | - | - | - | - | - | Y |
| condition expression | volitelný napříč všemi typy |
| action | volitelný napříč všemi typy |
| hit count, skip count | napříč všemi typy |
| edge-triggered | hlavně GLOBAL (per-instruction edge tracking) |

`Y*` = relevantní jen pro podmnožinu (signal events, IM 2 mode, IM 0
mode, atd. - viz per-typ sekce).

## Decision tree - kdy který typ

```
Co chcete debugovat?
|
+-- specifická adresa kódu
|   |
|   +-- jeden bod          -> PC_EXEC + Match SINGLE
|   +-- celý code blok     -> PC_EXEC + Match RANGE
|   +-- dispatch table     -> PC_EXEC + Match MASK
|
+-- přístup k paměti
|   +-- čtení              -> MEM_R
|   +-- zápis              -> MEM_W
|   +-- (s banking-aware: + Zone)
|
+-- I/O port
|   +-- čtení              -> IORQ_R (+ Port Mode 8BIT/16BIT)
|   +-- zápis              -> IORQ_W
|
+-- interrupt
|   +-- před dispatch (= včetně masked) -> IRQ_SIG (+ Source mask)
|   +-- po dispatch (= ISR target known) -> IRQ (+ IM filter / vector / ISR)
|
+-- pojmenovaný HW event
|   +-- vsync / hsync / blanking        -> HW_EVENT signal (+ trigger)
|   +-- CTC zero-cross / IRQ lines      -> HW_EVENT signal
|   +-- GDG palette / mode / border     -> HW_EVENT change
|   +-- raster line                     -> HW_EVENT raster:N
|   +-- CMT in/out / motor              -> HW_EVENT signal
|   +-- CPU NMI / DI / HALT / RESET     -> HW_EVENT cpu:*
|
+-- stack
|   +-- single threshold (overflow)     -> SP_THRESHOLD SINGLE
|   +-- window (corruption / switch)    -> SP_THRESHOLD WINDOW
|
+-- per-instruction expression
    +-- "kdykoliv X"                    -> GLOBAL (+ edge_triggered)
```

## Související dokumenty

- `match-modes.md` - SINGLE / RANGE / MASK detail
- `expression-syntax.md` - condition gramatika
- `action-dsl.md` - action DSL příkazy
- `irq-filter.md` - IRQ post-dispatch detail
- `irq-sig.md` - IRQ_SIG pre-dispatch detail
- `hw-events.md` - 28 events vocabulary
- `persistence.md` - `.bpt` JSON schema
