# Interrupt Log - formát exportních souborů

## Co je intlog

**intlog** je jeden ze subsystémů trace-suite. Loguje **všechny
interrupt-related eventy** v emulátoru:

1. Změny stavu CPU INT pinu (rising/falling) + identifikace zdroje
   (CTC2, PIO@PC2, PIOZ80 obecně, PIOZ80@P[A,B][0..7])
2. Změny CPU Z80 interrupt stavu (IM 0/1/2, IFF1, IFF2, RETI, EI)
3. Změny PIO-Z80 internal state (ready, armed, IM2-jump, RETI-applied)

Slouží pro debug interrupt-driven kódu (ISR), identifikaci IRQ source,
analýzu race conditions mezi banking switching a IRQ delivery, kontrolu
správnosti EI/RETI sekvencí.

intlog se ovládá z menu `Debugger Settings -> Trace Suite -> Interrupt Log`
(Off / Only With Debug Window / Always + Save on Exit + Max size [MB] +
Chunk [MB] + Set directory...). Basename (name) jen přes CLI / INI.
Persistentní v ini sekci `[TRACE_INTLOG]`.

## Adresářová struktura exportu

```
<dir>/
    <name>.json                    # meta + chunks anchor + initial state
    <name>.000.bin                 # chunk 0 - per-event záznamy
    <name>.001.bin                 # chunk 1
    ...
```

Initial state interrupt sběrnice + PIO-Z80 + CPU IM/IFF stav je
v manifestu `<name>.json` `subsys_header` jako JSON pole (= žádné samostatné
binární dump soubory).

## Per-event záznam (24 B fixed)

| Offset | Velikost | Pole                                          |
|--------|----------|------------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                        |
| 8      | 4 B      | screens_total (uint32 LE)                      |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                    |
| 16     | 1 B      | event_class (viz níže)                         |
| 17     | 1 B      | source_chip (viz níže)                         |
| 18     | 1 B      | source_pin (0-15 pro PIOZ80@P[A,B][0..7], 0 jinak) |
| 19     | 1 B      | edge (RISING=1 / FALLING=2 / NONE=0)           |
| 20     | 4 B      | state_bitmask (uint32 LE)                      |

### Pole event_class

| Hodnota | Symbol | Popis |
|---------|--------|-------|
| `0` | `INTLOG_EVENT_PIN_EDGE` | Změna na CPU INT pinu (= někdo z chipů zatáhl/uvolnil INT). |
| `1` | `INTLOG_EVENT_CPU_INT_STATE` | Změna IM mode, IFF flagů, vykonání RETI nebo EI. |
| `2` | `INTLOG_EVENT_PIO_STATE` | Změna PIOZ80 internal state (ready/armed/IM2-jump/RETI-applied). |
| `3` | `INTLOG_EVENT_IRQ_ACK_IM2` | IM 2 IRQ acknowledge - obsahuje vector_table_addr + isr_addr (= adresa, kterou Z80 nahraje do PC). |

### Pole source_chip

| Hodnota | Symbol | Popis |
|---------|--------|-------|
| `0` | `INTLOG_CHIP_NONE` | (rezervováno) |
| `1` | `INTLOG_CHIP_CTC2` | CTC2 + PIO@PC2 maska (= "CTC channel 2 OR PIO bit PC2") |
| `2` | `INTLOG_CHIP_PIOZ80` | PIOZ80 obecně (souhrnný edge na CPU INT pinu z PIOZ80 zdroje) |
| `3..10` | `INTLOG_CHIP_PIOZ80_PA0..PA7` | PIOZ80 Port A bit 0-7 (per-pin edge na masked input) |
| `11..18` | `INTLOG_CHIP_PIOZ80_PB0..PB7` | PIOZ80 Port B bit 0-7 (per-pin edge na masked input) |
| `19` | `INTLOG_CHIP_FDC` | FDC chip |
| `20` | `INTLOG_CHIP_CPU` | CPU samotná (pro `CPU_INT_STATE` eventy) |
| `21` | `INTLOG_CHIP_PIOZ80_PORT_A` | PIOZ80 Port A jako celek (pro `IRQ_ACK_IM2` source = "vektor přišel z Portu A") |
| `22` | `INTLOG_CHIP_PIOZ80_PORT_B` | PIOZ80 Port B jako celek (pro `IRQ_ACK_IM2` source = "vektor přišel z Portu B") |
| `23` | `INTLOG_CHIP_VECTOR_BUS_LATCH` | IM 2 vector source = bus latch / žádný chip nedodal vector. Pro `IRQ_ACK_IM2` events kdy PIOZ80 nebyl ve stavu PENDING a žádný známý chip (CTC2, FDC) nedrží INT pin. CPU v takovém případě přečte poslední bajt na datové sběrnici a vezme záznam `(I << 8) \| tento bajt`. |

### Pole source_pin

Pro `INTLOG_CHIP_PIOZ80_PA0..PB7` rozlišuje konkrétní pin (0-15
= PA0..PA7 + PB0..PB7). Pro ostatní chipy se neinterpretuje (= 0).

Per-pin granularita je aktivní - poller čte PIOZ80 masked input PA / PB
proti uloženému předchozímu stavu a emituje per-pin edge event pro
každou změnu.

### Pole edge

| Hodnota | Symbol | Popis |
|---------|--------|-------|
| `0` | `INTLOG_EDGE_NONE` | Pro non-pin eventy (CPU_INT_STATE, PIO_STATE). |
| `1` | `INTLOG_EDGE_RISING` | INT pin → 1 (chip začal požadovat IRQ). |
| `2` | `INTLOG_EDGE_FALLING` | INT pin → 0 (chip uvolnil IRQ). |

### Pole state_bitmask

Bit-packed flags. Sémantika podle `event_class`:

#### Pro `event_class = CPU_INT_STATE`:

| Bit | Symbol | Význam |
|-----|--------|--------|
| 0 | `INTLOG_STATE_BIT_IM0` | CPU v IM 0 stavu |
| 1 | `INTLOG_STATE_BIT_IM1` | CPU v IM 1 stavu |
| 2 | `INTLOG_STATE_BIT_IM2` | CPU v IM 2 stavu |
| 3 | `INTLOG_STATE_BIT_IFF1` | IFF1 flag (= interrupts enabled) |
| 4 | `INTLOG_STATE_BIT_IFF2` | IFF2 flag (LD A,I/R kopíruje sem) |
| 5 | `INTLOG_STATE_BIT_RETI` | Edge: RETI právě vykonán |
| 6 | `INTLOG_STATE_BIT_EI` | Edge: EI právě vykonán |

Aktivní pokrytí:
- **EI events** - snapshot IFF1 / IFF2 / IM v okamžiku EI instrukce.
- **RETI events** - emit při RETI dispatchu na PIOZ80 daisy chain.

#### Pro `event_class = PIO_STATE`:

| Bit | Symbol | Význam |
|-----|--------|--------|
| 8 | `INTLOG_STATE_BIT_PIO_READY` | PIOZ80 ready stav |
| 9 | `INTLOG_STATE_BIT_PIO_ARMED` | PIOZ80 armed stav |
| 10 | `INTLOG_STATE_BIT_PIO_IM2_JUMP` | IM 2 IRQ ack proběhl, ISR jump nastavený. Emituje se souběžně s `IRQ_ACK_IM2` eventem. |
| 11 | `INTLOG_STATE_BIT_PIO_RETI_APPLIED` | RETI dispatched - PIOZ80 vrácen ze stavu INTERRUPT_RECEIVED do INTERRUPT_NONE. |

Polling READY/ARMED je aktivní (porovnává PIOZ80 interrupt + ICW
ENABLED flag proti předchozímu stavu a emituje při změně). Bity IM2_JUMP
a RETI_APPLIED jsou edge bits (= one-shot, snapshotem zaznamenané
v okamžiku události, neholdují žádný "stav" mezi events).

#### Pro `event_class = IRQ_ACK_IM2`:

state_bitmask se reusne jako 32-bit payload se dvěma packed 16-bit
hodnotami:

| Bity | Pole | Význam |
|------|------|--------|
| 0..15 | `vector_table_addr` | Adresa entry v IM 2 tabulce v paměti = `(cpu->I << 8) \| vector` (bit 0 se nemaskuje) |
| 16..31 | `isr_addr` | 16-bit ISR adresa přečtená z paměti na adrese `vector_table_addr` (LE), tj. ta hodnota, kterou Z80 nahraje do PC. |

Encode v emit: `encoded = (uint32_t)vector_table_addr | ((uint32_t)isr_addr << 16);`

Decode v parseru:
```python
vector_table_addr = state_bits & 0xFFFF
isr_addr = (state_bits >> 16) & 0xFFFF
```

`source_chip` rozlišuje zdroj vector na sběrnici:
- `21` = `PIOZ80_PORT_A` (vektor z PIOZ80 daisy chain, Port A)
- `22` = `PIOZ80_PORT_B` (vektor z PIOZ80 daisy chain, Port B)
- `1`  = `CTC2` (CTC2 stáhl INT, žádný intread_cb registrován pro CTC -
  vektor nedodá nikdo, CPU přečte poslední bajt na datové sběrnici)
- `19` = `FDC` (FDC stáhl INT, vektor = poslední bajt na sběrnici)
- `23` = `VECTOR_BUS_LATCH` (jiný / neresolutable zdroj, vektor = poslední bajt na sběrnici)

`source_pin` = 0, `edge` = `NONE`.

Centrální hook zachytí i IRQ z non-PIOZ80 zdrojů (CTC2, FDC, bus latch).

#### Pro `event_class = PIN_EDGE`:

state_bitmask = 0 (= pole nepoužité, edge je v poli `edge`).

## Manifest `<name>.json`

```json
{
  "subsys": "intlog",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 97344,
  "truncated": false,
  "truncated_reason": "",
  "chunks": [
    { "index": 0, "file": "intlog.000.bin", "bytes": 24576,
      "start_pxclk": 0, "start_cpuclk": 0, "start_screens": 0 }
  ],
  "subsys_header": {
    "start_pxclk": 0,
    "start_pxclk_in_screen": 0,
    "start_screens": 0,
    "start_cpuclk": 0,
    "event_record_size": 24,
    "initial_state": {
      "interrupt_bus": {
        "raw": "0x00",
        "ctc2_pio_pc2": false,
        "pioz80": false,
        "fdc": false
      },
      "cpu_int": {
        "im_mode": 1,
        "iff1": false,
        "iff2": false
      },
      "pioz80": {
        "armed": false,
        "ready": false,
        "interrupt_vector": "0x00"
      }
    }
  }
}
```

### Initial state (subsys_header.initial_state)

- `interrupt_bus` - snímek hodnoty interrupt sběrnice v okamžiku
  startu recordingu, s rozpisem aktivních zdrojů.
- `cpu_int` - počáteční CPU IM mode + IFF flagy.
- `pioz80` - počáteční PIO-Z80 stav (armed, ready, vektorová adresa).

Initial state + sekvence events = kompletní rekonstrukce interrupt
historie.

## Coverage

| Co | Pokrytí |
|----|---------|
| Pin edge detection (CTC2, PIOZ80 souhrn, FDC) | aktivní |
| EI events s IM/IFF state | aktivní |
| Per-pin PIOZ80@P[A,B][0..7] | aktivní (polling masked input PA/PB proti předchozímu stavu) |
| RETI events | aktivní (emit `INTLOG_STATE_BIT_RETI`) |
| PIO_STATE events (READY/ARMED) | aktivní (polling proti předchozímu stavu) |
| IRQ_ACK_IM2 events (vector + ISR addr) | aktivní, pokrývá PIOZ80 i non-PIOZ80 IRQ zdroje (CTC2, FDC, bus latch) |
| PIO_STATE bit IM2_JUMP | aktivní, emituje se jen pro IRQ kde PIOZ80 byl skutečným zdrojem dispatchu |
| PIO_STATE bit RETI_APPLIED | aktivní |

## Použití

### Z UI

`Debugger Settings -> Trace Suite -> Interrupt Log`. Stejný UX pattern
jako ostatní trace-suite subsystémy.

### Načtení dat v Pythonu (příklad)

```python
import json, struct
from pathlib import Path

CHIP_NAMES = {0: "NONE", 1: "CTC2", 2: "PIOZ80", 19: "FDC", 20: "CPU",
              21: "PIOZ80_PORT_A", 22: "PIOZ80_PORT_B",
              23: "VECTOR_BUS_LATCH"}
EVENT_CLASS_NAMES = {0: "PIN_EDGE", 1: "CPU_INT_STATE",
                     2: "PIO_STATE", 3: "IRQ_ACK_IM2"}

def load_intlog(meta_path):
    meta_path = Path(meta_path)
    meta = json.loads(meta_path.read_text())
    base_dir = meta_path.parent

    events = []
    for chunk in meta["chunks"]:
        data = (base_dir / chunk["file"]).read_bytes()
        n = chunk["bytes"] // 24
        for i in range(n):
            o = i * 24
            pxclk     = struct.unpack_from("<Q", data, o + 0)[0]
            screens   = struct.unpack_from("<I", data, o + 8)[0]
            px_in_scr = struct.unpack_from("<I", data, o + 12)[0]
            evcls     = data[o + 16]
            chip      = data[o + 17]
            pin       = data[o + 18]
            edge      = data[o + 19]
            state     = struct.unpack_from("<I", data, o + 20)[0]
            events.append((pxclk, screens, px_in_scr, evcls, chip, pin, edge, state))
    return meta, events

meta, events = load_intlog("./trace-suite/intlog.json")
# Příklad: kdy CPU naposledy provedlo EI (= bit 6 v state)
ei_events = [e for e in events if e[3] == 1 and (e[7] & (1<<6))]
print(f"Total EI events: {len(ei_events)}")
```
