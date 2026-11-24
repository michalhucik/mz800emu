# HW Log - formát eventů

Subsystém **hwlog** v trace-suite. Loguje stavové změny HW chipů
s pxCLK timestamp.

## Per-event záznam (24 B fixed)

| Offset | Velikost | Pole                                          |
|--------|----------|------------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                        |
| 8      | 4 B      | screens_total (uint32 LE)                      |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                    |
| 16     | 1 B      | chip_id (viz tabulka níže)                     |
| 17     | 1 B      | sub_event_type (per-chip specific)             |
| 18     | 6 B      | payload (chip-specific layout)                 |

## Chip ID tabulka

| ID   | Symbol                  | Popis                                   |
|------|-------------------------|------------------------------------------|
| 0x01 | HWLOG_CHIP_GDG_MODE     | DMD register write (0xCE)                |
| 0x02 | HWLOG_CHIP_GDG_BANKING  | Memory map / port E0-E6 OUT              |
| 0x03 | HWLOG_CHIP_GDG_HWSCROLL | HW scroll register (0x01CF-0x05CF)       |
| 0x04 | HWLOG_CHIP_GDG_COLORS   | Paleta / PCG / border (0x06CF, 0xF0)     |
| 0x05 | HWLOG_CHIP_GDG_WFRF     | Write/Read Format register (0xCC, 0xCD)  |
| 0x06 | HWLOG_CHIP_GDG_VIDEO    | VS / VBLN edges + HS / HBLN s decimací   |
| 0x10 | HWLOG_CHIP_PIO8255      | 8255 PPI write                           |
| 0x11 | HWLOG_CHIP_CTC8253      | 8253 CTC write                           |
| 0x12 | HWLOG_CHIP_PIOZ80       | Z80 PIO state events (write/read/IRQ ack/RETI/bus) |
| 0x20 | HWLOG_CHIP_PSG          | SN76489 PSG write                        |
| 0x30 | HWLOG_CHIP_QD           | Quick Disk SIO write                     |
| 0x31 | HWLOG_CHIP_FDC          | WD279x FDC write                         |
| 0x40 | HWLOG_CHIP_MEMEXT       | Memext bank switch                       |
| 0x41 | HWLOG_CHIP_RD           | Ramdisk write (STD + Pezik)              |

## Sub-event typy a payload

### GDG_MODE (0x01)

sub_event_type: 0 (jediný typ - DMD write)

Payload:
```
[0] addr_low (= 0xCE)
[1] addr_high
[2] value (4 nejnižší bity DMD)
[3..5] zarezervováno
```

### GDG_BANKING (0x02)

sub_event_type = low byte portu (E0..E6) - logicky řízeno GDG, fyzicky se
přepínání map dělá v memory subsystému.

MZ-800 sémantika:
- E0 = ROM 0000 OFF
- E1 = ROM E000 OFF (= odpojí mapped ports area + ROM monitor upper)
- E2 = ROM 0000 ON
- E3 = ROM E000 ON
- E4 = ALL ON (= ROM 0000 + ROM E000, deaktivuje Prohibited)
- E5 = aktivuje Prohibited mode (cteni $E000-$FFFF vrací 0x1A shadow byte)
- E6 = deaktivuje Prohibited mode (vrátí na default mapping per ROM_E000 / DMD)

MZ-700 sémantika (po refaktoru sjednoceno s MZ-800):
- E0..E4 stejné jako mz800
- E5 = aktivuje Prohibited mode (= unmap ROM E800/code, čtení $E800-$FFFF
  vrací 0xFF; $F000-$FFFF rovněž 0xFF)
- E6 = deaktivuje Prohibited mode (= remap ROM E800)

MZ-1500 sémantika:
- E0..E4 stejné jako mz800
- E5 = SPEC ON s value (= D000 mapping)
- E6 = SPEC OFF

Persistence Prohibited (MZ-700 + MZ-800): drží přes OUT E0/E1/E2/E3 i přes
DMD bit 3 switch (700 ↔ 800 native). Ruší jen OUT E6, OUT E4 (= reset map),
nebo HW reset.

Payload:
```
[0] mmap_port (E0..E6)
[1] value (raw byte; mz800/mz700 ignoruje, mz1500 používá pro E5)
[2..5] zarezervováno
```

### GDG_HWSCROLL (0x03)

sub_event_type: 1-5 (= addr_high byte, scroll register index)

Payload:
```
[0] addr_low (= 0xCF)
[1] addr_high (= sub_event_type)
[2] value
[3..5] zarezervováno
```

### GDG_COLORS (0x04)

sub_event_type:
- `0x01` BORDER - port 0x06CF write
- `0x02` PALGRP - port 0xF0 write s bit 6 = 1
- `0x03` PAL - port 0xF0 write s bit 6 = 0 (paleta hodnota)
- `0x04` PCG - PCG write
- `0x05` PACKETGROUP - packet group write

Payload:
```
[0] addr_low (0xCF nebo 0xF0)
[1] addr_high
[2] value (raw zapsaný byte)
[3..5] zarezervováno
```

### GDG_WFRF (0x05)

sub_event_type:
- `0x00` WF (Write Format register, port 0xCC)
- `0x01` RF (Read Format register, port 0xCD)

Payload:
```
[0] addr_low (0xCC nebo 0xCD)
[1] addr_high
[2] value
[3..5] zarezervováno
```

### GDG_VIDEO (0x06)

Raster sync events. VS / VBLN edges se emitují vždy (~200 events/sec
při 50 fps). HS / HBLN edges (~31000 events/sec při full rate) jsou
emitovány pouze pokud je nastavena decimace (`hs_decimation > 0`).

sub_event_type:
- `0x01` VBLN_START - vertical blanking start
- `0x02` VBLN_END   - vertical blanking end
- `0x03` VS_START   - STS_VSYNC start
- `0x04` VS_END     - STS_VSYNC end
- `0x05` HBLN_START - horizontal blanking start (decimace)
- `0x06` HBLN_END   - horizontal blanking end (decimace)
- `0x07` HS_START   - STS_HSYNC start (decimace)
- `0x08` HS_END     - STS_HSYNC end (decimace)
- `0x09` CKSW       - zápis bitu CKSW (Superimpose), port 0x07CF (MZ-800); emituje se vždy

Payload: 6 nul u hran synchronizace (timestamp v hlavičce eventu je
dostatečný). U CKSW: `[0]` addr_low, `[1]` addr_high, `[2]` zapsaný
bajt (bit 7 = CKSW).

#### Decimace HS / HBLN

Konfiguruje se přes `[TRACE_HWLOG] hs_decimation = N` v INI nebo
`--hwlog-hs-decimation N` na CLI. Sémantika:

| Hodnota | Chování |
|---------|---------|
| `0` (default) | HS/HBLN edges se NElogují - bezpečný default kvůli ~31000 events/sec při full rate. |
| `N > 0` | Každý N-tý edge se emituje (counter modulo N). První edge po startu recordingu (counter=0) se vždy emituje pro konzistentní baseline. |

Counter je sdílený pro všechny 4 sub-events (HS_START, HS_END,
HBLN_START, HBLN_END) - decimace napříč typy. Externí parser pokud
potřebuje per-typ statistiku, dopočítá si ze sub_event_type pole.

### PIO8255 (0x10)

sub_event_type:
- `0x01` PORT_A_WRITE - write do PA (klávesnice column + JOY strobe +
  CURSOR reset bit 7)
- `0x02` PORT_B_WRITE - write do PB (data sběrnice klávesnice; obvykle
  read-only)
- `0x03` PORT_C_WRITE - write do PC (PC0..PC3 = output: blokovani audio,
  data CMT, blokovani INT z CTC2, motor CMT)
- `0x04` CONTROL_WRITE - write do CW registru (Mode 0 init nebo bit
  set/reset)

Payload:
```
[0] addr (0..3)
[1] value (raw byte)
[2..5] zarezervováno
```

Header obsahuje **CURSOR dělička** v `subsys_header.chip_dividers.pio8255_cursor_divider_screens`
(= 25, tj. CURSOR signal toggle 1× za 25 screens; PA bit 7 = 0 resetuje
cursor_timer = synchronizace blink).

### CTC8253 (0x11)

sub_event_type:
- `0x01` CONTROL_WRITE - write do CW registru (= addr 3, formát SC1/SC0
  RL1/RL0 M2/M1/M0 BCD)
- `0x02` COUNTER_WRITE - write do datového counteru (= addr 0..2)

Payload:
```
[0] addr (0..3 = CTC0, CTC1, CTC2, CWREG)
[1] value (raw byte)
[2] pre-write rl_byte (jen pro COUNTER_WRITE - LSB/MSB pořadí v rámci
    aktuálního CTC; pro CONTROL_WRITE = 0)
[3..5] zarezervováno
```

Header obsahuje **CTC0 1M1 dělička** v `subsys_header.chip_dividers.ctc0_clk1m1_divider_pxclk`
(= 16 na mz800/mz1500 = pxCLK/16 ≈ 1.108 MHz na mz800) a jeho fázi v `ctc0_clk1m1_phase_pxclk`:
sestupná hrana CLK0 leží v `pxclk_total % dělička == fáze` (6 na mz800, 0 na mz700 a mz1500).

### QD (0x30)

sub_event_type:
- `0x01` REGISTER_WRITE - write do QD SIO (porty 0xF4..0xF7)

Payload:
```
[0] SIO_addr (0..3 = data A, data B, ctrl A, ctrl B)
[1] value
[2..5] zarezervováno
```

### FDC (0x31)

sub_event_type:
- `0x01` REGISTER_WRITE - write do WD279x (porty 0xD8..0xDB)

Payload:
```
[0] addroffset (0..3 = command/status, track, sector, data)
[1] value (raw byte v "normální" polaritě - Sharp invertuje data na
    fyzické úrovni sběrnice, ale io_data v fdc_write_byte je už
    v normalizované formě dle wd279x_write_byte konvence)
[2..5] zarezervováno
```

### RD (0x41)

sub_event_type:
- `0x01` STD_WRITE - write do std ramdisku (porty 0xE9, 0xEA, 0xEB, 0xFA)
- `0x02` PEZIK_WRITE - write do Pezik ramdisku (porty 0xE8, 0xEC..0xEF)

Payload:
```
[0] port low byte
[1] value
[2] port high byte (latch upper / address bits)
[3..5] zarezervováno
```

### PIOZ80 (0x12)

Plnohodnotný stavový pohled na Z80 PIO. Logujeme všechny relevantní změny:
write Mode/Vector/ICW/Mask/I/O Select, write/read data, externí změnu pinů
(CTC0 do PA4, VBLN do PA5), IM 2 IRQ ack a RETI dokončení.

sub_event_type:

| Hex  | Symbol                          | Trigger                               |
|------|---------------------------------|---------------------------------------|
| 0x01 | HWLOG_PIOZ80_MODE_WRITE         | zápis Mode Control Word               |
| 0x02 | HWLOG_PIOZ80_VECTOR_WRITE       | zápis Interrupt Vector (D0=0)         |
| 0x03 | HWLOG_PIOZ80_INT_CTRL_WRITE     | zápis ICW nebo IDW (D3-D0 = 0111/0011) |
| 0x04 | HWLOG_PIOZ80_MASK_WRITE         | zápis Mask Word (po ICW s MF=1)       |
| 0x05 | HWLOG_PIOZ80_IO_SELECT_WRITE    | zápis I/O Select Mask (po Mode 3)     |
| 0x06 | HWLOG_PIOZ80_DATA_WRITE         | OUT na data port (0xFE/0xFF)          |
| 0x07 | HWLOG_PIOZ80_DATA_READ          | IN z data portu (0xFE/0xFF)           |
| 0x08 | HWLOG_PIOZ80_BUS_INPUT_CHANGE   | externí pin edge (PA4_CTC0, PA5_VBLN) |
| 0x09 | HWLOG_PIOZ80_IRQ_ACK_M2         | M2 IORQ INTA - vrácen interrupt vector |
| 0x0A | HWLOG_PIOZ80_RETI_APPLIED       | RETI prošlo PIO daisy chain           |

Payload (6 B):

```
[0]   port_id (0=A, 1=B, 0xFF=N/A pro globální events)
[1]   addr / sub_addr
        - DATA_WRITE/READ: addr 0..3 (0xFE/0xFF v hardwarové notaci - reálně
          jen low 2 bity addr po dispatch)
        - control writes: control addr 0xFC/0xFD (= addr 0/1)
        - BUS_INPUT_CHANGE: bit pos pinu (4 = PA4/CTC0, 5 = PA5/VBLN)
        - IRQ_ACK_M2: vrácený interrupt_vector (LSB)
        - RETI_APPLIED: 0
[2]   value
        - data writes/reads: raw byte
        - control writes: raw control byte zapsaný uživatelem
        - BUS_INPUT_CHANGE: nová úroveň pinu (0 nebo 1)
        - IRQ_ACK_M2: kopie interrupt_vector (= byte vrácený CPU)
        - RETI_APPLIED: 0
[3..5] decoded_state_delta_bitmask (24 b LE) - bity HWLOG_PIOZ80_DELTA_*
```

#### Bity decoded_state_delta_bitmask

Indikují co se v rámci eventu reálně změnilo. Externí parser podle baseline
(initial state PIOZ80 TLV - viz `HW-log_INITIAL_STATE_format.md`) +
sumace změn rekonstruuje stav v libovolném čase.

| Bit  | Hex     | Symbol                            | Význam                        |
|------|---------|-----------------------------------|-------------------------------|
| 0    | 0x00001 | HWLOG_PIOZ80_DELTA_MODE           | port->mode změněn             |
| 1    | 0x00002 | HWLOG_PIOZ80_DELTA_IO_MASK        | port->io_mask změněn          |
| 2    | 0x00004 | HWLOG_PIOZ80_DELTA_ICMASK         | port->icmask změněn           |
| 3    | 0x00008 | HWLOG_PIOZ80_DELTA_ICENA          | port->icena změněn            |
| 4    | 0x00010 | HWLOG_PIOZ80_DELTA_ICFNC          | port->icfnc (AND/OR) změněn   |
| 5    | 0x00020 | HWLOG_PIOZ80_DELTA_ICLVL          | port->iclvl (HIGH/LOW) změněn |
| 6    | 0x00040 | HWLOG_PIOZ80_DELTA_VECTOR         | port->interrupt_vector změněn |
| 7    | 0x00080 | HWLOG_PIOZ80_DELTA_DATA_OUT       | port->data_output změněn      |
| 8    | 0x00100 | HWLOG_PIOZ80_DELTA_PORT_INT       | port->port_int (INT FSM) změněn |
| 9    | 0x00200 | HWLOG_PIOZ80_DELTA_MASKED_IN      | port->masked_input změněn     |
| 10   | 0x00400 | HWLOG_PIOZ80_DELTA_INT_GLOBAL     | g_pioz80.interrupt změněn     |
| 11   | 0x00800 | HWLOG_PIOZ80_DELTA_INT_PORT_ID    | g_pioz80.interrupt_port_id změněn |
| 12   | 0x01000 | HWLOG_PIOZ80_DELTA_CTRL_EXPECT    | port->ctrl_expect změněn      |

Bity 13-23 jsou rezervovány pro budoucí rozšíření (např. RETI re-arm
sledování, daisy chain forward).

#### Příklady event sekvencí

Inicializace PA do Mode 3 s I/O select 3Fh (typický MZ-800 BASIC vzor):

```
PIOZ80 MODE_WRITE       port_id=0 addr=0xFC value=0xCF delta=MODE|CTRL_EXPECT
PIOZ80 IO_SELECT_WRITE  port_id=0 addr=0xFC value=0x3F delta=IO_MASK|CTRL_EXPECT|MASKED_IN
```

Tisk bajtu na tiskárnu (PB data + PA STROBE):

```
PIOZ80 DATA_WRITE  port_id=1 addr=0xFF value=0x41 delta=DATA_OUT
PIOZ80 DATA_WRITE  port_id=0 addr=0xFE value=0x7F delta=DATA_OUT
PIOZ80 DATA_WRITE  port_id=0 addr=0xFE value=0xFF delta=DATA_OUT
```

VBLN edge probuzuje INT (Mode 3 + EI):

```
PIOZ80 BUS_INPUT_CHANGE  port_id=0 addr=5 value=1 delta=MASKED_IN|PORT_INT|INT_GLOBAL
PIOZ80 IRQ_ACK_M2        port_id=0 addr=0x40 value=0x40 delta=PORT_INT|INT_GLOBAL|INT_PORT_ID
PIOZ80 RETI_APPLIED      port_id=0 addr=0 value=0 delta=PORT_INT|INT_GLOBAL|INT_PORT_ID
```

### MEMEXT (0x40)

sub_event_type:
- `0x01` BANK_SWITCH - write do mapovacího portu Memextu (0xE0..0xE4
  v MZ-800 / MZ-1500). Zapisuje se nová raw bank pro 4 KB bus page.

Payload:
```
[0] addr_point (= bus page index, 0..15, 4 KB granularita)
[1] value (raw byte zapsaný do mapovacího portu)
[2] type (0 = LUFTNER, 1 = PEHU)
[3..5] zarezervováno
```

Hook se aktivuje jen pokud `MEMEXT_TEST_CONNECTED`. Externí parser
získá kompletní mapovací stav (16-entry Memext map) z initial state
v hlavičce + sumace BANK_SWITCH eventů. Pro PEHU se 1 event = 2
sousedící entries (PEHU 8 KB granularita = 2× 4 KB stránky).

### PSG (0x20)

sub_event_type:
- `0x01` REGISTER_WRITE - write byte do PSG datového portu (port 0x70/0x71
  v MZ-800; SN76489AN dekóduje interně přes latch)

Payload:
```
[0] raw byte zapsaný uživatelem
[1] channel mask (mono = 0x01, stereo = bitmask PSG_CH_LEFT|RIGHT)
[2] stereo flag (0 = mono, 1 = stereo PSG modul)
[3..5] zarezervováno
```

Decoded info (latch_cs, attn flag, tone vs noise vs attn) dopočítá
externí parser z initial state v hlavičce + sumace REGISTER_WRITE
eventů. Header obsahuje **PSGCLK divider** a **TEMPO divider**
v `subsys_header.chip_dividers`.

## Coverage chipů

Hooky jsou nainstalované pro: GDG_MODE, GDG_BANKING, GDG_HWSCROLL,
GDG_COLORS, GDG_WFRF, GDG_VIDEO, PSG, MEMEXT, CTC8253, PIO8255, **PIOZ80**,
QD, FDC, RD.

PIOZ80 je k dispozici jak v hwlog (decoded state changes, bus events),
tak v intlog (pin-edge a IRQ-flow events). Různé abstrakční úrovně,
duální logování je úmyslné a doporučené.

## Hlavička recordingu (manifest `<name>.json`)

```json
{
  "subsys": "hwlog",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 97344,
  "truncated": false,
  "chunks": [...],
  "subsys_header": {
    "start_pxclk": 12345,
    "start_pxclk_in_screen": 12345,
    "start_screens": 0,
    "start_cpuclk": 2469,
    "event_record_size": 24,
    "chip_dividers": {
      "psgclk_divider_cpuclk": 16,
      "tempo_divider_screen_rows": 229,
      "cpu_divider_pxclk": 5
    }
  }
}
```

`chip_dividers` obsahuje konstanty pro odvození časování:
- **psgclk_divider_cpuclk** - PSG step běží 1× za N CPU CLK (mz800 = 16,
  mz1500 = 16 = stejné)
- **tempo_divider_screen_rows** - GDG TEMPO signal toggle 1× za N
  screen rows (~34 Hz na mz800/mz1500 = 229)
- **cpu_divider_pxclk** - CPU CLK = pxCLK / N (mz800 = 5, mz1500 = 4)

## Initial state binary snapshot

Soubor `<dir>/<name>_initial_state.bin` se vytvoří v okamžiku startu
recordingu a obsahuje per-chip TLV záznamy. **Per-chip payload je
serializován field-by-field v explicitním little-endian formátu** -
parser je nezávislý na compiler ABI, alignmentu a paddingu.

```
[0]    chip_id        (1 B)
[1..4] payload_length (4 B uint32 LE)
[5...] payload        (per-chip layout, viz HW-log_INITIAL_STATE_format.md)
```

EOF marker: chip_id = 0xFF, length = 0.

Dump obsahuje:
- HWLOG_CHIP_PIO8255  - PA/PC signály + 10 B keyboard_matrix (= 50 B)
- HWLOG_CHIP_CTC8253  - 3x counter, per counter 32 B (= 96 B)
- HWLOG_CHIP_PSG      - stereo flag + 2x PSG state (variabilní, viz spec)
- HWLOG_CHIP_MEMEXT   - connection/type/addr_mask + map[16] (= 68 B,
                        jen pokud je Memext připojen)
- HWLOG_CHIP_GDG_MODE - GDG core registry + raster state (architecture
                        marker rozliší MZ-800 vs MZ-1500 dále arch-specific
                        pole)
- HWLOG_CHIP_PIOZ80   - per port (12 polí) + globální (interrupt, port_id)

**Velké data buffery NE** - hlavní RAM (64 KB), VRAM (32 KB), Memext RAM
(512 KB), ramdisk obsah - patří do **cputrack initial dump** (jiný
subsystém s vlastními soubory `_initial_*.bin`). Hwlog drží jen
register/config state.

Detail per-chip layoutu (offsety, velikost polí, pořadí) viz samostatný
dokument [HW-log_INITIAL_STATE_format.md](HW-log_INITIAL_STATE_format.md).

## Externí parser

Parser čte chunks v pořadí (per `chunks[i].file`), každý chunk je
sequence 24-byte recordů. Timestamp je explicitně uvedený - nemusí
se rekonstruovat. To se liší od **cputrack**, kde se rekonstruuje
sumací wait_clk + insn_T_states.

## Sdílený 24 B layout s Event Vieweru

HW-log chunk sdílí **bit-identický** per-event 24 B layout s in-memory
ringem **Event Vieweru**.

Mapping pole na pole:

| HW-log offset | HW-log pole          | Event Viewer pole         | Velikost |
|---------------|----------------------|----------------------------|----------|
| 0             | `pxclk_total`        | `pxclk_total`             | 8 B      |
| 8             | `screens_total`      | `screens_total`           | 4 B      |
| 12            | `pxclk_in_screen`    | `pxclk_in_screen`         | 4 B      |
| 16            | `chip_id`            | `category`                | 1 B      |
| 17            | `sub_event_type`     | `subtype`                 | 1 B      |
| 18..23        | `payload[6]`         | `pc` (2 B) + `payload` (4 B) | 6 B   |

Rozdíl je jen ve struktuře posledních 6 B:

- HW-log: 6 B raw payload bytes (per-chip layout)
- Event Viewer: 2 B CPU PC + 4 B uint32 payload

Cross-merge timestampů mezi HW-log a Event Vieweru je 1:1 (= stejný
`pxclk_total` domain).

### Mapping chip_id -> Event Viewer kategorie

Event Viewer používá vlastní enum kategorií (24 hodnot). Per-chip překlad:

| HWLOG_CHIP                | -> EVENTLOG_CAT                |
|---------------------------|--------------------------------|
| `HWLOG_CHIP_GDG_MODE`     | `EVENTLOG_CAT_GDG_MODE`        |
| `HWLOG_CHIP_GDG_BANKING`  | `EVENTLOG_CAT_GDG_BANKING`     |
| `HWLOG_CHIP_GDG_HWSCROLL` | `EVENTLOG_CAT_GDG_HWSCROLL`    |
| `HWLOG_CHIP_GDG_COLORS`   | `EVENTLOG_CAT_GDG_COLORS`      |
| `HWLOG_CHIP_GDG_WFRF`     | `EVENTLOG_CAT_GDG_WFRF`        |
| `HWLOG_CHIP_GDG_VIDEO`    | `EVENTLOG_CAT_GDG_VIDEO`       |
| `HWLOG_CHIP_PIO8255`      | `EVENTLOG_CAT_PIO8255`         |
| `HWLOG_CHIP_CTC8253`      | `EVENTLOG_CAT_CTC8253`         |
| `HWLOG_CHIP_PIOZ80`       | `EVENTLOG_CAT_PIOZ80`          |
| `HWLOG_CHIP_PSG`          | `EVENTLOG_CAT_PSG`             |
| `HWLOG_CHIP_QD`           | `EVENTLOG_CAT_QD`              |
| `HWLOG_CHIP_FDC`          | `EVENTLOG_CAT_FDC`             |
| `HWLOG_CHIP_MEMEXT`       | `EVENTLOG_CAT_MEMEXT`          |
| `HWLOG_CHIP_RD`           | `EVENTLOG_CAT_RD`              |

Sub_event_type hodnoty zůstávají identické (= per-chip subtype enumy
jsou shodné v obou subsystémech).

### Mapping INTLOG / IORQLOG / marklog -> Event Viewer

Trace subsystémy mimo HW-log mají vlastní per-record formát, ale jejich
hooky paralelně emitují i do Event Viewer ringu:

| Trace subsystem      | -> EVENTLOG_CAT             | Subtype zdroj         |
|----------------------|------------------------------|------------------------|
| intlog CPU INT state | `EVENTLOG_CAT_CPU_INT`      | CPU INT reason         |
| intlog pin edge      | `EVENTLOG_CAT_CPU_PIN_EDGE` | pin index              |
| intlog IRQ ack IM2   | `EVENTLOG_CAT_IRQ_ACK_IM2`  | vector LSB             |
| iorqlog IN           | `EVENTLOG_CAT_IORQ_IN`      | IORQ subtype           |
| iorqlog OUT          | `EVENTLOG_CAT_IORQ_OUT`     | IORQ subtype           |
| marklog              | `EVENTLOG_CAT_USER_MARK`    | marker_id v payload    |

Plus Event Viewer specifické kategorie bez paralely v trace-suite:

- `EVENTLOG_CAT_BP_FIRE` (BP fire events)
- `EVENTLOG_CAT_CPU_CTRL` (CPU control events)
- `EVENTLOG_CAT_MMIO_R` / `EVENTLOG_CAT_MMIO_W` (MMIO accesses)

Detail per-kategorie + filter syntax viz [`../event-viewer.md`](../event-viewer.md).
