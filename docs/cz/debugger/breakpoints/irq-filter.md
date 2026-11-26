# Breakpoint - IRQ filter

`IRQ` breakpoint triggeruje při dispatchi IRQ Z80 procesorem
(= POST-dispatch). Bez sub-filtrů fire na **každý** dispatch v zapnutých
IM modech.

K dispozici jsou tyto filtry:

- **IM mode discriminator** (per-IM enable/disable)
- **IM 0 RST opcode filter**
- **IM 2 vector address filter** (s Match Mode)
- **IM 2 ISR address filter** (s Match Mode)

Pro **pre-dispatch** detekci peripheral signal raise existuje samostatný
typ `IRQ_SIG` - viz `irq-sig.md`.

## Pozadí: kdy se enforce volá

Hook se volá **POST-dispatch**, tj. po dokončení Z80 INT acknowledge
cyklu. V tom okamžiku jsou dostupné:

- `cpu->im` - aktuální IM mode (0 / 1 / 2)
- `cpu->i` - I register (= high byte vector_table_addr)
- `cpu->int_vector` - low byte z peripheralu (push při INTACK), v IM 0
  je to RST opcode (např. 0xFF = RST 38h)
- `cpu->pc` - ISR jump target

Pre-dispatch okamžik nemá smysluplný kontext (= jen raised mask),
post-dispatch má vector + ISR. Konzistentní semantika "BP fire when
IRQ dispatches" = intuitivní pro debug ISR handleru.

## IM mode discriminator

| Pole | Význam |
|------|--------|
| `im0_enabled` | true = BP fire na IM 0 dispatch |
| `im1_enabled` | true = BP fire na IM 1 dispatch |
| `im2_enabled` | true = BP fire na IM 2 dispatch |

**Default pro nový BP**: všechny 3 enabled (= fire-na-každý-IRQ-dispatch).

UI **vyžaduje aspoň 1** IM mode enabled (= jinak BP nikdy nefire,
validation zablokuje OK button).

```
IM modes: [x] IM 0   [x] IM 1   [x] IM 2     (?)
```

Po vypnutí IM checkboxu se příslušná sub-sekce skryje.

## IM 0 sub-filter: RST opcode mask

| Pole | Význam |
|------|--------|
| `im0_rst_mask` | bitmask 8 RST opcodů (0 = match-all) |

V IM 0 peripheral push 1-byte opcode na bus při INTACK. Standardně to
bývá `RST n` opcode (= 0xC7 + n*8 pro n = 0..7), ale technicky to může
být i jakýkoliv jiný 1-byte opcode (např. `NOP` nebo `EI`). RST mask
filtruje pouze RST varianty, non-RST opcody nikdy nematchují.

| Bit | RST | Opcode |
|-----|-----|--------|
| 0   | RST 00 | 0xC7 |
| 1   | RST 08 | 0xCF |
| 2   | RST 10 | 0xD7 |
| 3   | RST 18 | 0xDF |
| 4   | RST 20 | 0xE7 |
| 5   | RST 28 | 0xEF |
| 6   | RST 30 | 0xF7 |
| 7   | RST 38 | 0xFF |

Mask `0` = match-all (= fire na každý RST). Mask != 0 = fire pouze
pokud aktuální RST opcode má nastavený příslušný bit.

```
IM 0 RST filter (none = match all):
[ ] 0xC7 (RST 00)  [ ] 0xCF (RST 08)  [ ] 0xD7 (RST 10)  [ ] 0xDF (RST 18)
[ ] 0xE7 (RST 20)  [ ] 0xEF (RST 28)  [ ] 0xF7 (RST 30)  [x] 0xFF (RST 38)
```

V příkladu výše BP fire jen na `RST 38h` v IM 0 dispatchi (mask = 0x80).

**Use case MZ-800**: MZ-800 v native módu používá IM 1 (= dispatch na
0x0038), ale CP/M nebo custom firmware může používat IM 0 s vlastní RST
volbou. Filter dovolí trigger jen na specific RST handler.

## IM 1 sub-filter: žádný

V IM 1 Z80 vždy dispatchuje přes `RST 38h` (= jump na 0x0038). Není
žádný per-vector ani per-source sub-filter - pokud `im1_enabled = true`,
**každý** IM 1 dispatch fire BP (subject to dalším BP rules - condition,
hit_count, skip_count).

UI v této sub-sekci zobrazuje jen info text + tooltip "no extra filter
available".

## IM 2 sub-filter: vector + ISR

### Vector address filter

| Pole | Význam |
|------|--------|
| `im2_vector_enabled` | true = filter aktivní |
| `im2_vector_addr` | očekávaná hodnota `(I << 8) \| vec` |
| `im2_vector_match_mode` | SINGLE / RANGE / MASK (UI dropdown) |
| `im2_vector_addr_end` | RANGE upper bound (inclusive) |
| `im2_vector_mask` | MASK bitmask |

Porovnává se **celá adresa záznamu včetně bitu 0**. Z80 v IM 2 bit 0
vektoru nemaskuje (dokumentace Zilogu tvrdí opak, ale procesor bit 0
použije). Z80 PIO dodává vždy sudý vektor; lichý záznam vznikne, když
vektor nedodá nikdo (na MZ-800 přerušení z CTC2 nebo /EXINT) a procesor
přečte poslední bajt, který zůstal na datové sběrnici. Hodnota 0x40CF tedy
odpovídá jen záznamu 0x40CF, ne 0x40CE. Kdo chce bit 0 ignorovat, zvolí
režim MASK s maskou 0xFFFE.

Match modes (per `match-modes.md`):

| Mode | Sémantika |
|------|-----------|
| `SINGLE` | exact match `vector_addr == im2_vector_addr` |
| `RANGE`  | `im2_vector_addr <= vector_addr <= im2_vector_addr_end` |
| `MASK`   | `(vector_addr & im2_vector_mask) == (im2_vector_addr & im2_vector_mask)` |

UI layout v Edit BP dialogu (IRQ branch, IM 2 sub-section):

```
IM 2 sub-filters:
[x] Filter by IM2 vector address      (?)
    IM2 Vector:        0x [40CE]
    Vector Match Mode: [SINGLE  v]
```

Při přepnutí dropdownu na RANGE / MASK se dynamicky zobrazí druhý input
(End / Mask):

```
[x] Filter by IM2 vector address      (?)
    IM2 Vector:        0x [FE00]
    Vector Match Mode: [RANGE   v]
    Vector End:        0x [FE3F]      (?)
```

```
[x] Filter by IM2 vector address      (?)
    IM2 Vector:        0x [FE10]
    Vector Match Mode: [MASK    v]
    Vector Mask:       0x [FFE0]      (?)
```

OK button validation: RANGE = End musí být parsovatelný a `End >= Start`.
MASK = Mask musí být parsovatelný (libovolná
hodnota včetně 0xFFFF = identical to SINGLE).

Trigger pokud (SINGLE):
1. CPU dispatchuje INT,
2. `cpu->im == 2`,
3. `((cpu->i << 8) | cpu->int_vector) == im2_vector_addr`.

**Use case**: break na konkrétní zdroj přerušení (= peripheral A vs B
identifikuju podle vector slotu jejich INTACK push). RANGE/MASK pro
break na celou skupinu peripherálů (= např. všechny vectors v `0xFE00 -
0xFE3F` slot range).

### ISR address filter

| Pole | Význam |
|------|--------|
| `im2_isr_enabled` | true = filter aktivní |
| `im2_isr_addr` | očekávaná ISR adresa = `cpu->pc` po dispatchi |
| `im2_isr_match_mode` | SINGLE / RANGE / MASK (UI dropdown) |
| `im2_isr_addr_end` | RANGE upper bound (inclusive) |
| `im2_isr_mask` | MASK bitmask |

Match modes (per `match-modes.md`):

| Mode | Sémantika |
|------|-----------|
| `SINGLE` | exact match `isr_addr == im2_isr_addr` |
| `RANGE`  | `im2_isr_addr <= isr_addr <= im2_isr_addr_end` |
| `MASK`   | `(isr_addr & im2_isr_mask) == (im2_isr_addr & im2_isr_mask)` |

ISR adresa je hodnota, kterou CPU nahrál z IM2 vector table do PC.

UI layout (IM 2 sub-section, ISR řádek po Vector řádcích):

```
[x] Filter by IM2 ISR address         (?)
    IM2 ISR:        0x [0148]
    ISR Match Mode: [SINGLE  v]
```

```
[x] Filter by IM2 ISR address         (?)
    IM2 ISR:        0x [0100]
    ISR Match Mode: [RANGE   v]
    ISR End:        0x [01FF]         (?)
```

Use case RANGE: break když ISR skočí kdekoliv v rozsahu jedné ROM stránky
(např. user-defined IRQ handlery v `0xC000..0xC1FF`). Use case MASK:
break na konkrétní bit vzor v ISR adrese (= např. všechny ISR handlery
v dolní polovině 256B sloků).

Trigger pokud (SINGLE):
1. CPU dispatchuje INT,
2. `cpu->im == 2`,
3. `cpu->pc == im2_isr_addr` (po INT dispatch jumpu).

**Use case**: break na vstup do konkrétního ISR handleru bez ohledu na
to, který peripheral IRQ raised (= sdílený handler routes podle vlastních
mechanismů).

### Kombinace IM 2 sub-filtrů (AND)

Pokud jsou **oba** IM 2 sub-filtery zapnuté, fire vyžaduje shodu obou:

```
[x] Filter by IM2 vector address
    IM2 Vector:  0x 4060
[x] Filter by IM2 ISR address
    IM2 ISR:  0x 0148
```

Trigger jen pokud vector slot **i** ISR target sedí. Použitelné když
chceš ověřit, že vector table má očekávaný entry (= sanity check
correct vector wiring).

### IM 2 sub-filter mimo IM 2

| IM mode | Sub-filter OFF | Sub-filter ON |
|---------|----------------|----------------|
| IM 0    | fire (pokud im0_enabled) | filter explicit vyžaduje IM 2 - sub-filter je no-op |
| IM 1    | fire (pokud im1_enabled) | filter explicit vyžaduje IM 2 - sub-filter je no-op |
| IM 2    | fire (pokud im2_enabled) | fire jen pokud match |

Pozor: sub-filter `im2_vector_enabled` / `im2_isr_enabled` má smysl jen
v IM 2 dispatch. Pokud `im0_enabled = true` a `im2_vector_enabled = true`,
v IM 0 dispatchi BP fire (= sub-filter aktivní jen v IM 2).

## Persistence

Pole jsou serializována do `.bpt` JSON souboru pod klíči:

| Klíč | Default při missing |
|------|---------------------|
| `im0_enabled` | `true` (= fire-na-všechny IM) |
| `im1_enabled` | `true` |
| `im2_enabled` | `true` |
| `im0_rst_mask` | `0` (= match-all RSTs) |
| `im2_vector_enabled` | `false` |
| `im2_vector_addr` | `0` |
| `im2_vector_match_mode` | `SINGLE` |
| `im2_vector_addr_end` | `0` |
| `im2_vector_mask` | `0xFFFF` |
| `im2_isr_enabled` | `false` |
| `im2_isr_addr` | `0` |
| `im2_isr_match_mode` | `SINGLE` |
| `im2_isr_addr_end` | `0` |
| `im2_isr_mask` | `0xFFFF` |

Příklad `.bpt` JSON záznamu:

```json
{
  "type": "IRQ",
  "im0_enabled": true,
  "im1_enabled": false,
  "im2_enabled": true,
  "im0_rst_mask": 128,
  "im2_vector_enabled": true,
  "im2_vector_addr": 16590,
  "im2_isr_enabled": false,
  "im2_isr_addr": 0
}
```

## UI

Edit panel pro `IRQ` typ:

```
IRQ - CPU acknowledged INT (post-dispatch)
IM modes:  [x] IM 0   [x] IM 1   [x] IM 2     (?)

[Pokud IM 0 enabled:]
IM 0 RST filter (none = match all):
[ ] 0xC7 (RST 00)  [ ] 0xCF (RST 08)  [ ] 0xD7 (RST 10)  [ ] 0xDF (RST 18)
[ ] 0xE7 (RST 20)  [ ] 0xEF (RST 28)  [ ] 0xF7 (RST 30)  [ ] 0xFF (RST 38)

[Pokud IM 1 enabled:]
IM 1 always dispatches RST 38h (no extra filter)

[Pokud IM 2 enabled:]
IM 2 sub-filters:
[ ] Filter by IM2 vector address     (?)
    IM2 Vector:  0x [____]
[ ] Filter by IM2 ISR address        (?)
    IM2 ISR:     0x [____]
```

Validation OK button:
- aspoň 1 IM mode musí být enabled
- pokud IM 2 enabled + některý sub-filter on, hex addr musí parsovat

Hex inputy persistují hodnotu i v disabled stavu (= toggle off / on
neztratí dříve zadaný vector slot).

## Související dokumenty

- `irq-sig.md` - pre-dispatch IRQ signal source filter
- `match-modes.md` - SINGLE / RANGE / MASK detail
- `types.md` - katalog všech BP typů
