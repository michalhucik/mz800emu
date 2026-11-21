# Breakpoint - HW eventy

Breakpoint typu `HW_EVENT` reaguje na pojmenovanou hardwarovou událost
v emulátoru (vsync, raster, INT linka periferie, zápis do GDG palety,
změna paměťového módu CPU, atd.). Na rozdíl od MEM/IORQ breakpointů,
které sledují konkrétní adresu, HW_EVENT BP triggeruje na sémantickou
událost - bez ohledu na to, kterou ROM/programovou cestou byla vyvolána.

K dispozici je 31 eventů ve čtyřech kategoriích. Pro signal-typové
eventy je k dispozici **trigger condition**
(low/high/rising/falling/changed).

## Vocabulary - 31 eventů ve 4 kategoriích

### Signal events (17) - s trigger condition

Signal eventy reprezentují digitální signál na úrovni 0/1
(syncy, blanking, INT linky, CMT). UI nabízí **trigger condition**
selector určující, kdy BP fire-uje:

| Trigger | Sémantika | Symbol v UI |
|---------|-----------|-------------|
| `Low`     | fire dokud signál je 0 | "0" |
| `High`    | fire dokud signál je 1 | "1" |
| `Rising`  | fire na přechodu 0 -> 1 | trojúhelník nahoru |
| `Falling` | fire na přechodu 1 -> 0 | trojúhelník dolů |
| `Changed` | fire na jakémkoliv přechodu | dvojtrojúhelník |

Default je `Rising` (= "fire-on-event").

| Persistence | UI label | Popis |
|-------------|----------|-------|
| `vsync`        | vsync       | GDG vsync signál |
| `hsync`        | hsync       | GDG hsync signál |
| `vbln`         | vbln        | GDG vertical blanking |
| `hbln`         | hbln        | GDG horizontal blanking |
| `ctc:zc0`      | ctc0        | CTC 8253 channel 0 OUT |
| `ctc:zc1`      | ctc1        | CTC 8253 channel 1 OUT |
| `ctc:zc2`      | ctc2        | CTC 8253 channel 2 OUT |
| `irq:ctc2`     | IRQ ctc2    | CTC2 INT linka |
| `irq:pioz80_a` | IRQ pio-z80 PA | PIO Z80 port A INT |
| `irq:pioz80_b` | IRQ pio-z80 PB | PIO Z80 port B INT |
| `irq:fdc`      | IRQ fdc     | FDC INT linka |
| `tempo`        | TEMPO       | TEMPO signál (~32 Hz) |
| `cursor`       | CURSOR      | CURSOR blink signál (edge-based per snímek) |
| `cmt:in`       | CMT_IN      | CMT tape input edge |
| `cmt:out`      | CMT_OUT     | CMT tape output (PC1 write) |
| `cmt:mstate`   | CMT_MSTATE  | CMT motion state (PC04 toggle / derived ze STOP/PAUSED) |
| `cmt:motor`    | CMT_MOTOR   | CMT motor enable (PC3 bit) |

**Pozn:** signál polarita - hodnota `1` v BP enforce vrstvě znamená
"signál je logicky aktivní" (= sync probíhá / blanking aktivní / INT
raised / motor on / signal high). Polarita HW signálů (= MZ má některé
signály invertované, např. VSYN_ACTIVE = 0) je převedena na společnou
konvenci.

### Change events (5) - implicit "happened"

Change eventy reprezentují diskrétní změnu hodnoty registru. Žádný
trigger selector - fire vždy při změně. Hodnota nové paměti je
dostupná v condition expression jako `Value`.

| Persistence       | UI label  | Popis |
|-------------------|-----------|-------|
| `mode_change`     | mode      | GDG DMD register (port 0xCE) |
| `palette_change`  | palette   | GDG palette index (port 0xF0, bit 6 = 0) |
| `palgrp_change`   | palgrp    | GDG palette group (port 0xF0, bit 6 = 1) |
| `border_change`   | border    | GDG border barva (port 0xCF, msb = 6) |
| `cksw_change`     | cksw      | GDG bit CKSW (port 0xCF, msb = 7); `Value` = 0 nebo 1, fire při každém zápisu |
| `cmt:state_change`| cmt state | CMT lifecycle (play / stop / pause / eject) |

Pozn: `cksw_change` existuje jen na MZ-800. MZ-1500 nemá `palgrp_change` ani `border_change` (= jednodušší
paletový model, border je pevný). BP s těmito eventy v MZ-1500 nikdy
nezafire.

**`cmt:state_change`:**

CMT lifecycle event. `Value` v condition expression = composite enum:

| Hodnota | Význam |
|---------|--------|
| 0 | tape stopped (= ejected nebo zastavený) |
| 1 | tape playing (paused == 0) |
| 2 | tape recording (paused == 0) |
| 3 | tape paused (override jakéhokoli underlying stavu) |

Příklad podmínky: `Value == 1` = stop jen při přechodu do PLAY stavu.
PAUSED je override - pokud user pause-uje recording, value = PAUSED, ne
RECORD. Při unpause přijde nové fire s value = PLAY/RECORD.

### Point event s parametrem (1)

| Persistence    | UI label | Param |
|----------------|----------|-------|
| `raster:N`     | raster:N | scanline number (N = 0..311) |

Fire jen pro konkrétní scanline N. Pro raster celého snímku (= každý
řádek) použij místo toho `hsync` event s trigger=Rising (= 311 fire/snímek).

### CPU events (8) - point/state bez trigger condition

| Persistence       | UI label   | Sémantika | Value v ctx |
|-------------------|------------|-----------|-------------|
| `cpu:nmi`         | NMI        | NMI assertion | 0 |
| `cpu:di`          | DI         | DI instrukce vykonána | 0 |
| `cpu:im_change`   | IM change  | IM 0/1/2 změna | new IM |
| `cpu:iff_change`  | IFF change | Agregovaný event pro IFF1 i IFF2; preferuj specifický `cpu:iff1_change` / `cpu:iff2_change` | new IFF |
| `cpu:iff1_change` | IFF1 change | IFF1 změna | new IFF1 (0/1) |
| `cpu:iff2_change` | IFF2 change | IFF2 změna | new IFF2 (0/1) |
| `cpu:halt`        | HALT       | HALT instrukce | 0 |
| `cpu:reset`       | RESET      | CPU reset | 0 |

Pro IM_CHANGE a IFF_CHANGE lze v condition expression filtrovat na
konkrétní hodnotu (= `Value == 2` pro IM 2 fire).

**IFF1 / IFF2 fire decision matrix:**

Změny IFF1/IFF2 mají reason indikující, co změnu vyvolalo (RESET, EI,
DI, INT_ACK, NMI_ACK, RETI, RETN). Fire matrix:

| Reason  | `iff_change` | `iff1_change` | `iff2_change` |
|---------|--------------|---------------|---------------|
| RESET   | ano          | ano           | ano           |
| EI      | ano          | ano           | ano           |
| DI      | ano          | ano           | ano           |
| INT_ACK | ano          | ano           | ano           |
| NMI_ACK | ano          | ano           | ano           |
| RETI    | -            | -             | -             |
| RETN    | ano          | ano           | -             |

- **RESET** firuje vše (CPU reset clearne oba IFF).
- **EI / DI**: oba IFF1+IFF2 -> 1 / 0; fire všech tří paralelně.
- **INT_ACK** (= IM 0/1/2 dispatch): IFF1 -> 0, IFF2 zachován; firuje
  IFF1_CHANGE + IFF_CHANGE + IFF2_CHANGE (na originálním Z80 zachován,
  ale event se posílá pro UI symetrii).
- **NMI_ACK**: IFF1 cleared, IFF2 zachován; IFF2_CHANGE fíruje
  forward-compat pro alternativní Z80 architektury.
- **RETI**: signal pro Z80 PIO, IFF beze změny; nefíruje žádný event.
- **RETN**: IFF1 obnoveno z IFF2; firuje IFF_CHANGE + IFF1_CHANGE
  (vždy, bez ohledu na to zda IFF1==IFF2 před).

Condition expression v BP DSL může filtrovat reason přes
`Reason == <symbol>` (= `reset` / `ei` / `di` / `int_ack` / `nmi_ack` /
`reti` / `retn` / `none`). Viz
[expression-syntax.md Reason vocabulary](expression-syntax.md#reason-vocabulary).

**Příklad:** BP na `cpu:iff_change` s condition `Reason == nmi_ack`
zachytí jen NMI ack moment. Nested-ISR detekce přes
`Reason == int_ack && SP < 0xFFE0`.

Filtraci IFF1 vs IFF2 lze udělat výběrem správného eventu při
registraci BP, ne v condition (= EI/DI fire oba současně).

## Trigger condition - sémantika

Pro signal events drží enforce vrstva **per-event prev signal level**.
Při každém fire eventu porovná aktuální hodnotu signálu proti
předchozí a podle BP trigger condition rozhodne fire/skip:

```
LOW:     fire = (curr == 0)
HIGH:    fire = (curr == 1)
RISING:  fire = (prev == 0 && curr == 1)
FALLING: fire = (prev == 1 && curr == 0)
CHANGED: fire = (prev != curr)
```

State cache se updatuje PO iteraci všech BP - další fire vidí curr
jako prev.

## Condition expression - per-kind ctx

Condition expression eval (= BP `Cond:` field) má per-kind sémantiku
pro pole `Address` a `Value`:

| Kind          | `Address`    | `Value`                                |
|---------------|--------------|----------------------------------------|
| SIGNAL        | 0            | curr_state (0 nebo 1)                  |
| CHANGE        | 0            | nová hodnota (mode/palette/palgrp/border value) |
| POINT_PARAM   | row (raster) | 0                                      |
| POINT_NOPARAM | 0            | info (IM/IFF new value, jinak 0)       |

Příklady condition pro change event:
```
# Mode_change BP, fire jen při přepnutí do MZ-700 modu (DMD=0x08):
Value == 0x08

# Palette_change, fire jen pro index 5 (= bity 4..7 PALGRP register):
(Value >> 4) & 0x07 == 5
```

## Příklady use case

### Stop na začátku rámu pro analýzu DMA-stylu

```
Type:    HW_EVENT
Event:   vsync
Trigger: Rising
Action:  Stop
```

Fire jednou za snímek (50 Hz) v okamžiku startu vsync pulzu. Klasický
debug přístup pro per-frame state analysis.

### Detekce mid-frame raster split

```
Type:    HW_EVENT
Event:   raster:192
Action:  Stop
```

Fire na začátku scanline 192 (= klasická pozice border raster effectů).
Užitečné pro analýzu mid-frame palette switch.

### Watchdog na zápis do palety

```
Type:    HW_EVENT
Event:   palette_change
Cond:    Value & 0x40 == 0
Action:  Log "PAL: $", Value, " at $", PC
```

Fire na každý zápis do palety (ne palgrp), loguje hodnotu + PC kontext
bez zastavení. Umožňuje sledovat ROM rutiny pro přepínání barev.

### Detekce CMT motor on/off pro tape loaders

```
Type:    HW_EVENT
Event:   cmt:motor
Trigger: Rising
Action:  Log "CMT motor ON at $", PC
```

Fire jen na 0->1 přechod PC3 bitu (= moment kdy program zapnul
přehrávač). Užitečné pro analýzu tape loader sekvencí.

### Sledování CTC2 INT line aktivace

```
Type:    HW_EVENT
Event:   irq:ctc2
Trigger: Rising
Action:  Stop
```

Fire jen při edge raise INT linky CTC2. Trigger=Changed = fire
i při deassertu (= debug toggle frequency v PSG audio engine).

## Persistence - zpětná kompatibilita

Existující `.bpt` soubory bez `event_trigger` klíče se loadnou s
default `rising` trigger (= zachová legacy fire-on-event chování).

Legacy event jména v BC alias tabulce (= starší `.bpt` soubory se
naloudí):

| Legacy string                 | Aktuální persistence        |
|-------------------------------|------------------------------|
| `pio:porta_int`               | `irq:pioz80_a`               |
| `pio:portb_int`               | `irq:pioz80_b`               |
| `fdc:irq`                     | `irq:fdc`                    |
| `tape:edge`                   | `cmt:in`                     |
| `nmi`, `di`, `im_change`, ... | `cpu:*` (s prefixem)         |

### Vyřazené eventy

Tyto eventy byly nedoděláno nebo expressivnější přes IORQ_W BP:

- `psg:int_pa5` - PSG INT je viditelný přes `irq:pioz80_b` nebo IRQ_SIG BP
- `psg:reg_write` - použij IORQ_W na PSG porty (0xF0/0xF2)
- `ppi:pa_write`, `ppi:pc_change` - použij IORQ_W na 8255 porty (0xD0-0xD3)
- `fdc:command`, `fdc:drq` - použij IORQ_W na FDC porty (0xD8-0xDB)
- `ctc:gate0_edge` - gate je input signál, ne output (= mimo HW listing)
- `tape:read_byte`, `tape:write_byte` - byte-level rozeznatelnost neexistuje na HW level
- `mmio:bank_switch`, `mmio:mode_change` - použij IORQ_W na E0-E6 banking porty

BP s těmito eventy ve starším `.bpt` souboru loadnou s neznámým
event_name - warning na stderr, BP zůstane viditelný v UI s původním
stringem ale nikdy nezafire.

### Doporučená migrace

1. Smaž BP s vyřazenými eventy a nahraď IORQ_W BP s odpovídajícím portem
   a optional condition expression filtrující hodnotu.
2. Pro existující vsync/hsync/raster BP zkontroluj trigger condition -
   default `Rising` zachovává legacy chování. Pokud chceš sledovat
   sustained level (ne jen edge), nastav `High` nebo `Low`.
