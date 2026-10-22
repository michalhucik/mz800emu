# trace-suite - uživatelská dokumentace

Sada 5 nezávislých sekvenčních logovacích subsystémů emulátoru
mz800new pro hloubkovou analýzu vykonávaného kódu, I/O traffic,
interrupts, HW state changes a uživatelských markerů.

## 5 subsystémů

| Subsystém | Co loguje                                          | Bytes/event |
|-----------|----------------------------------------------------|-------------|
| cputrack  | Každou vykonanou Z80 instrukci (PC, bytes, WAIT)   | 12          |
| iorqlog   | Každý IORQ IN/OUT (port, value, source addr)       | 24          |
| intlog    | Změny INT pinu, IM/IFF state, EI                   | 24          |
| hwlog     | Změny HW chip stavu (GDG, PSG, CTC, PIO, MEMEXT, QD/FDC/RD) | 24 |
| marklog   | Uživatelské markery z `mark "name"` BP action      | 24          |

Každý subsystém je nezávislý - lze enable jen jeden, libovolnou
kombinaci, nebo všechny.

## Aktivace

### CLI options

```bash
./mz800emu.exe --cputrack-mode=always --cputrack-dir=./logs/run1
./mz800emu.exe --iorqlog-mode=window --intlog-mode=always
```

Per-subsystém options (každý subsystém má identické):
- `--<sys>-mode=off|window|always`
- `--<sys>-dir=<path>` (default: `./trace-suite/`)
- `--<sys>-name=<basename>` (default: shodné s sys, např. `cputrack`)
- `--<sys>-chunk-mb=<N>` (default: 64)
- `--<sys>-max-total-mb=<N>` (default: 2048 = 2 GB; 0 = bez omezení)
- `--<sys>-save-on-exit=on|off` (default: on)

`<sys>` = `cputrack` / `iorqlog` / `intlog` / `hwlog` / `marklog`.

Marklog navíc:
- `--marklog-stdout=on|off` (default: `on`) - back-compat printout
  `[BP-MARK] <name>` na stdout při fire STMT_MARK BP action.
  Nezávislé na `--marklog-mode` (= lze mít stdout bez binárního logu,
  nebo binární log bez stdout, nebo obě / ani jedno).

Hwlog navíc:
- `--hwlog-hs-decimation=<N>` (default: `0` = HS/HBLN edges OFF;
  `N > 0` emit každý N-tý edge HS/HBLN. Plný rate je ~31000 events/sec
  což zničí real-time perf - rozumné hodnoty `100`-`1000` pro
  troubleshooting nebo `1` pro plnou trasu krátkého úseku.)

Cputrack navíc (range-scoped tracking):
- `--cputrack-pc-lo=<addr>` a `--cputrack-pc-hi=<addr>` (default `0` a
  `0xFFFF` = celý adresový prostor, tj. bez filtru). Omezí záznam jen
  na instrukce, jejichž PC leží v rozsahu `[lo, hi]` včetně. Instrukce
  mimo rozsah se nezaznamenají. Typické použití: sledovat jen TPA
  (např. `--cputrack-pc-lo=0x100 --cputrack-pc-hi=0xBFFF`) a vynechat
  ROM/OS rutiny. Hodnoty přijímají dec i hex (`0x` prefix), ořežou se
  na 16 bitů. Filtr je jen pro cputrack - ostatní subsystémy logují vždy
  vše.
- `--cputrack-regs=on|off` (default `off`) - formát 3: u každé instrukce
  i registry CPU po ní (32 B na záznam místo 12). Stav před instrukcí je
  stav v předchozím záznamu; řetěz drží synchronizační záznamy na začátku
  záznamu, při návratu do rozsahu filtru PC a po změně registrů mimo
  instrukci. Formát popisuje
  [CPU-track_format.md](formats/CPU-track_format.md#formát-v3-záznam-s-registry-32-b).

#### Shorthand `--all-traces-*`

```bash
./mz800emu.exe --all-traces-mode=always --all-traces-dir=./logs/run-1/
./mz800emu.exe --all-traces-mode=always --hwlog-mode=off   # vše kromě hwlog
```

`--all-traces-mode <off|window|always>` aplikuje hodnotu na všech 5
subsystémů najednou, `--all-traces-dir <path>` totéž pro výstupní adresář.
Per-subsystém option (`--cputrack-mode` ad.) má precedenci - shorthand
vyplní pouze ty subsystémy, kde uživatel per-subsys option nepředal.

### GUI menu

Debugger Settings -> Trace Suite -> [All channels | CPU Track | IORQ Log | Interrupt Log | HW Log | Marker Log]

Per-subsystém: radio (Off / Only With Debug Window / Always), Save on Exit toggle,
pole "Max size [MB]" (limit velikosti záznamu na kanál; 0 = bez omezení), pole
"Chunk [MB]" (velikost RAM bufferu před flushem na disk) a "Set directory..."
(výběr cílového adresáře). Změny velikostí / adresáře se projeví až při
(znovu)spuštění záznamu daného kanálu. Basename (name) se nastavuje jen přes
CLI / INI.

Položka "All channels" nastaví mode / Save on Exit / Max size / Chunk / adresář
u všech 5 kanálů (cputrack / iorqlog / intlog / hwlog / marklog) najednou.

Submenu CPU Track má navíc přepínač "Record registers" (formát 3 s registry,
jako CLI `--cputrack-regs`; platí od dalšího startu záznamu) a pole
"PC range filter [lo,hi]:" se dvěma hex
vstupy (lo, hi) a položkou "Reset range (whole space)". Nastavuje stejný
range-scope filtr jako CLI `--cputrack-pc-lo` / `--cputrack-pc-hi`
(záznam jen pro PC v rozsahu). Reset vrátí rozsah na `0000`-`FFFF`
(= celý adresový prostor, bez filtru).

Submenu Marker Log má navíc přepínač "Print marks to stdout" (back-compat výpis
`[BP-MARK] <name>` na stdout, nezávislý na binárním logu).

#### Status indikátor v titulku okna

Pokud je alespoň jeden subsystém aktivně recordující, doplní se za
standardní text titulku SDL hlavního okna značka `[trace: <flags>]`
kde `<flags>` je seznam písmen oddělených mezerou:

| Písmeno | Subsystém |
|---------|-----------|
| `C`     | cputrack  |
| `I`     | iorqlog   |
| `N`     | intlog    |
| `H`     | hwlog     |
| `M`     | marklog   |

Pokud subsystém dosáhl `max_total_mb` limitu a recording byl
zastaven (truncated), za písmenem se objeví suffix `!` (např. `H!`).
Pokud žádný subsystém aktivní není, závorka se v titulku neobjeví
vůbec (= ve vypnutém stavu titulek beze změny).

Příklady:

```
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00 [trace: C I N H]
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00 [trace: C I]
MZ-800 - normal speed: 100.00 %, FB-FPS: 50.00 [trace: C I N H!]
```

Aktualizuje se periodicky přes SDL timer (~1x/s).

### INI config

Per-subsystém sekce v `mz800emu.ini` / `mz1500emu.ini`. Čísla
v konfiguračním souboru jsou **šestnáctková**, tak jak je emulátor zapisuje
(`chunk_mb = 0x40` znamená 64 MB); i hodnota bez předpony `0x` se čte
šestnáctkově (`chunk_mb = 64` by znamenalo 100 MB a `pc_range_hi = 65535`
je mimo rozsah, takže zůstane výchozí hodnota):

```ini
[TRACE_CPUTRACK]
mode=OFF
dir=trace-suite
name=cputrack
chunk_mb=0x40                    # 64 MB
max_total_mb=0x800               # 2048 MB
save_on_exit=1
pc_range_lo=0x0000               # range-scope filtr: dolní mez PC (default 0)
pc_range_hi=0xffff               # horní mez PC (default 0xFFFF = bez filtru)

[TRACE_IORQLOG]
... (stejná struktura)

[TRACE_INTLOG]
...

[TRACE_HWLOG]
...

[TRACE_MARKLOG]
mode=OFF
dir=trace-suite
name=marklog
chunk_mb=0x40
max_total_mb=0x800
save_on_exit=1
stdout_enabled=1                # back-compat printout [BP-MARK] na stdout
```

## Ovládání za běhu (MCP + Breakpoint Action)

Kromě statického nastavení (CLI / GUI / INI) lze trace záznam ovládat
i za běhu - buď z MCP klienta (AI agent), nebo automaticky z akce
breakpointu. Tím lze záznam přesně ohraničit kolem zajímavého úseku
bez nutnosti ukládat až při ukončení emulátoru.

### Přes MCP

MCP server vystavuje pro 5 binárních kanálů (`cputrack`, `iorqlog`,
`intlog`, `hwlog`, `marklog`) plný lifecycle a čtení záznamů:

| Tool | Co dělá |
|------|---------|
| `emu_trace_start` | Spustí záznam zvoleného kanálu (= režim ALWAYS) |
| `emu_trace_stop`  | Zastaví záznam kanálu (segment se uzavře a uloží) |
| `emu_trace_reset` | Zapíše rozpracovaný chunk a manifest na disk, záznam pokračuje (nic nezahodí) |
| `emu_trace_save`  | Uloží/přesměruje segment kanálu na novou cestu |
| `emu_trace_status` | Stav kanálů pro čtení (segment, počet záznamů, chunky) |
| `emu_trace_read`  | Stránka záznamů kanálu (dekódované řádky nebo surové záznamy) |

Lifecycle tooly mají povinný argument `channel` (jeden z
`cputrack`/`iorqlog`/`intlog`/`hwlog`/`marklog`). `emu_trace_save` má navíc
volitelný `path` (cílová cesta následujícího segmentu; bez něj se jen
uzavře a znovuotevře aktuální segment = "ulož teď"). Rozhraní zrcadlí
CDL lifecycle tooly (`emu_cdl_start` / `_stop` / `_reset` / `_export`).
Detaily a návratové hodnoty viz [Přehled MCP tools](../mcp-server/tools-overview.md).

Typický postup: warp přes nezajímavou část, `emu_trace_start` těsně
před sledovaným úsekem, doběh, `emu_trace_stop` a `emu_trace_save`
s názvem segmentu.

### Čtení záznamů přes MCP

`emu_trace_read` vrací záznamy kanálu po stránkách přímo přes MCP, bez
parsování souborů a i za běhu záznamu: čte jak chunky na disku, tak
data, která jsou zatím jen v RAM bufferu (krátký běh se tak dá číst
hned, nemusí se nejdřív zapsat na disk). Po `emu_trace_stop` zůstane
poslední segment čitelný až do dalšího startu kanálu. Stejně jde číst
i ring Event Vieweru (kanál `eventlog`) a paměť zachycení zvuku (kanály
`audio_psg_writes`, `audio_levels`, `audio_frames`, viz [Zachycení
zvuku](../audio-capture.md)).

- Záznamy mají pořadové číslo `seq` v rámci segmentu (jeden záznam od
  startu do stopu). Stránkuje se kurzorem `next_cursor`; `end_of_data`
  znamená, že stránka došla k nejnovějšímu záznamu - po dalším běhu
  emulace se čte dál stejným kurzorem.
- Kurzor ze staršího segmentu (kanál se mezitím znovu spustil) vrátí
  chybu začínající `segment_replaced:`; čte se znovu od začátku.
- Výchozí výstup jsou dekódované řádky (u cputrack i disassembly
  z bajtů v záznamu a symbol), velikost stránky hlídá rozpočet znaků
  (`max_chars`, výchozí 20 000). `format: raw_b64` vrací surové
  záznamy pro skripty.
- Filtry: rozsah PC (cputrack), port, směr a typ (iorqlog), čip
  (hwlog), značka (marklog), časový rozsah (`t`).
- Čas `t` je v taktech GDG od startu emulace. U cputrack (formát v2) se
  počítá z kotvy chunku a T-stavů záznamů (včetně záznamů přijetí
  přerušení); s nahrávacím filtrem PC (`pc_range_lo/hi`) chybí
  instrukce, a proto ho `emu_trace_status` hlásí jako `has_time: false`.

Parametry a tvar odpovědi viz [Přehled MCP tools](../mcp-server/tools-overview.md).

### Z akce breakpointu

Akce breakpointu (Custom mode) umí stejné záznamové operace spustit
automaticky při hitu BP - takže místo desítek ručních MCP volání stačí
jeden chytře umístěný breakpoint. Dostupné forward příkazy:

- `cdl_start` / `cdl_stop` / `cdl_reset` - lifecycle CDL.
- `cdl_export "soubor"` - export CDL do souboru.
- `trace_start <kanál>` / `trace_stop <kanál>` - start/stop trace kanálu
  (kanál = `cputrack`/`iorqlog`/`intlog`/`hwlog`, case-sensitive).
- `trace_save <kanál>, "soubor"` - uložení/přesměrování segmentu kanálu.
- `snapshot "soubor"` - uložení .mzs snapshotu (funguje i z pokračujícího
  breakpointu, který emulátor nezastavuje).

Název souboru u `cdl_export`, `trace_save` a `snapshot` je šablona se
stejnými specifikátory a `$proměnnými` jako příkaz `log` - takže lze
generovat číslované soubory, např.:

```
snapshot "snap-%d.mzs", $id
trace_save cputrack, "seg-%d.bin", $id
$id += 1
```

Kompletní popis akce breakpointu viz
[Breakpoint - akce při triggeru](breakpoints/action-dsl.md).

## Mode semantika

| Mode        | Aktivace                                            |
|-------------|------------------------------------------------------|
| OFF         | Subsystém vypnutý, žádná režie ani v hot path        |
| WITH_WINDOW | Aktivní jen pokud je debug okno (Alt+D) otevřené     |
| ALWAYS      | Aktivní trvale, nezávisle na stavu debug okna        |

## Output struktura

Per-recording adresář `<dir>/`:

```
trace-suite/
    cputrack.json                  # meta + chunks list
    cputrack_initial_ram.bin       # 64 KB CPU RAM dump na startu
    cputrack_initial_vram.bin      # 32 KB VRAM
    cputrack_initial_exvram.bin    # 32 KB EXVRAM (mz800)
    cputrack_initial_pcg.bin       # 24 KB PCG (mz1500)
    cputrack_initial_memext.bin    # 512 KB Memext (pokud připojeno)
    cputrack.000.bin               # chunk 0 (per-instr eventy)
    cputrack.001.bin               # chunk 1
    ...
    iorqlog.json
    iorqlog.000.bin
    ...
    intlog.json
    intlog.000.bin
    ...
    hwlog.json
    hwlog.000.bin
    ...
    marklog.json                   # meta + chunks list + markers registry
    marklog.000.bin                # chunk 0 (per-marker eventy 24 B)
    ...
```

## Formát manifestu `<name>.json`

Každý subsystém generuje vlastní manifest `<name>.json`:

```json
{
  "subsys": "cputrack",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 97344,
  "truncated": false,
  "truncated_reason": "",
  "chunks": [
    { "index": 0, "file": "cputrack.000.bin",
      "bytes": 67108864,
      "start_pxclk": 12345,
      "start_cpuclk": 2469,
      "start_screens": 0 },
    ...
  ],
  "subsys_header": {
    ...per-subsystém specifická pole...
  }
}
```

`chunks[i].start_*` umožňuje O(1) seek na chunk + lineární scan
uvnitř chunku pro rekonstrukci timestampu (cputrack: sumací
insn_T_states + wait_clk; iorqlog/intlog/hwlog: explicitní timestamp
v každém eventu).

## Per-subsystém formát eventů

### cputrack (12 B)

| Offset | Velikost | Pole                      |
|--------|----------|---------------------------|
| 0      | 2 B      | regPC (uint16 LE)         |
| 2      | 1 B      | insn_length (1-4; 0 = přijetí přerušení) |
| 3      | 4 B      | insn_bytes (jen prvních insn_length valid) |
| 7      | 4 B      | wait_clk (uint32 LE)      |
| 11     | 1 B      | t_std - standardní T-stavy (formát v2) |

`wait_clk == 0` = běžná instrukce, `0xFFFFFFFF` = saturated
(typ. dlouhá HALT smyčka >20 min při 3.5 MHz). Formát v2 (manifest
`event_record_version: 2`): bajt 11 nese standardní délku instrukce,
záznam s `insn_length` 0 je přijetí přerušení (`regPC` = adresa
obsluhy, `t_std` = T-stavy přijetí). Detail viz
[formát cputrack](formats/CPU-track_format.md).

HALT/self-loop collapse: opakované vykonání stejné instr (HALT, JP $,
JR $-2) se sbalí do jediného eventu s wait_clk = sumovaný čas.

### iorqlog (24 B)

| Offset | Velikost | Pole                                  |
|--------|----------|----------------------------------------|
| 0      | 8 B      | pxclk_total                            |
| 8      | 4 B      | screens_total                          |
| 12     | 4 B      | pxclk_in_screen                        |
| 16     | 1 B      | event_type (IORQ/MREQ/IORQ_UNCONNECTED) |
| 17     | 1 B      | direction (IN=0/OUT=1)                 |
| 18     | 2 B      | source_addr (regBC pro IORQ, regPC pro MREQ) |
| 20     | 2 B      | port_or_addr                           |
| 22     | 1 B      | value                                  |
| 23     | 1 B      | pulse_duration_pxclk                   |

Emitované event types:
- `IORQ` - běžný IN/OUT (každý port přístup přes Z80 IORQ cyklus)
- `MREQ_MAPPED` - memory access do 0xE000-0xE008 (PIO8255 / CTC8253 /
  GDG status). Pro MZ-1500 nativně bez podmínek; pro MZ-800 jen v MZ-700
  mode s namapovanou horní ROM
- `IORQ_UNCONNECTED` - "ghost bus" IN na nemapovaný port (vrácena hodnota
  z bus latch)

### intlog (24 B)

| Offset | Velikost | Pole                                  |
|--------|----------|----------------------------------------|
| 0      | 8 B      | pxclk_total                            |
| 8      | 4 B      | screens_total                          |
| 12     | 4 B      | pxclk_in_screen                        |
| 16     | 1 B      | event_class (PIN_EDGE/CPU_INT_STATE/PIO_STATE/IRQ_ACK_IM2) |
| 17     | 1 B      | source_chip                            |
| 18     | 1 B      | source_pin (0-15 pro PIOZ80@P[A,B][0..7]) |
| 19     | 1 B      | edge (RISING=1/FALLING=2)              |
| 20     | 4 B      | state_bitmask (IM/IFF1/IFF2/EI/RETI nebo (vector_table_addr \| isr_addr<<16) pro IRQ_ACK_IM2) |

Pokryté event class:
- `PIN_EDGE` - rising/falling na CPU INT pinu, source_chip = CTC2 /
  PIOZ80 / PIOZ80_PA0..PA7 / PIOZ80_PB0..PB7 / FDC. Per-pin PIOZ80
  granularita aktivní.
- `CPU_INT_STATE` - EI events s aktuálním IM/IFF1/IFF2; RETI events
  (bit `INTLOG_STATE_BIT_RETI`).
- `PIO_STATE` - změny PIOZ80 internal state (READY / ARMED / IM2_JUMP /
  RETI_APPLIED bits).
- `IRQ_ACK_IM2` - 32-bit encoded payload: low 16 bit = vector_table_addr
  (entry v IM 2 tabulce), high 16 bit = isr_addr (= ISR adresa, kterou
  Z80 nahraje do PC). Source_chip rozlišuje zdroj vector: `PIOZ80_PORT_A`
  / `_PORT_B` (vektor z PIOZ80 daisy chain), `CTC2` / `FDC` (INT pin
  tažený, vector = 0, dispatch na (I:00)), `VECTOR_BUS_LATCH` (jiný /
  neresolutable). Klíčové pro debug ISR dispatchu.

### hwlog (24 B)

Viz samostatný `docs/cz/debugger/formats/HW-log_format.md`.

Pokryté per-chip register write hooky pro:
- **GDG** (mz800 + mz1500) - MODE / BANKING / HWSCROLL / COLORS / WFRF
- **GDG_VIDEO** sync events (VBLN / VS edges; HS / HBLN s decimací)
- **PSG** (SN76489 register write) - audio engine RE
- **CTC8253** (control + counter writes)
- **PIO8255** (port A/B/C + control writes)
- **MEMEXT** (bank switch - LUFTNER + PEHU)
- **QD / FDC / RD** (register write events na disk operations)
- **PIOZ80** (Mode/Vector/INT_CTRL/Mask/IO_Select/Data writes,
  Data reads, Bus input change, M2 IRQ ack, RETI applied)

Plus initial state binary snapshot v `<dir>/<name>_initial_state.bin`
(TLV per chip - chip_id + length + payload, EOF marker 0xFF). Každý
payload je field-by-field little-endian serializace nezávislá na
compiler ABI - layout viz `docs/cz/debugger/formats/HW-log_INITIAL_STATE_format.md`.

Header obsahuje per-chip děličky:
`psgclk_divider_cpuclk`, `tempo_divider_screen_rows`,
`ctc0_clk1m1_divider_pxclk`, `pio8255_cursor_divider_screens`.

### marklog (24 B)

Viz samostatný `docs/cz/debugger/formats/MARK-log_format.md`.

Loguje uživatelské markery vyvolávané z BP action `mark "name"`. Marker
name je předem zaregistrované při BP parse a v binárce ho reprezentuje
stabilní `uint16 marker_id`. Mapování id -> name se dumpuje do manifestu `<name>.json`
`subsys_header.markers`. Marklog navíc má volitelný stdout printout
(`[BP-MARK] <name>`) řízený samostatným flagem `stdout_enabled`.

## Performance impact

- OFF stavy: zero impact (jediný if test, branch predictor naučí "vždy false")
- ON stavy: emulace se výrazně zpomalí (synchronní disk write při
  chunk swap, recording cesta přes pomalé callbacky). **Akceptovatelné -
  je to ladící mód.** Konzolová zpráva `[trace-suite] <subsys>: chunk N
  swap to disk (XX B)` upozorní na flush.

## Limity

- chunk-mb (default 64 MB): RAM buffer per subsystém. Vyšší =
  méně častý flush, vyšší peak RAM
- max-total-mb (default 2048 = 2 GB na kanál; 0 = unlimited): hard limit
  na celkovou velikost recordingu - pojistka proti zaplnění disku (cputrack
  jinak roste prakticky neomezeně). Při dosažení se subsystém zastaví, do
  manifest `<name>.json` se zapíše `"truncated": true, "truncated_reason": "max_total_mb"`,
  na konzoli zpráva `[trace-suite] <subsys>: max-total-mb=N reached,
  recording stopped`. Emulace běží dál. Limit objemu zápisů akcí
  breakpointů (`[BREAKPOINTS] fwd_byte_limit_mb`, ten emulaci
  zapauzuje) platí jen pro kanál, který spustila (`trace_start`) nebo
  ukládá (`trace_save`) akce breakpointu, ne pro nahrávání spuštěné
  z tohoto menu nebo přes MCP.

## Externí parsing

Externí tooling (Python, C, ...) může parsovat manifest `<name>.json` + binární
chunky podle dokumentovaných formátů. Žádné vendor-locked formáty.

Re-simulace: hackeři / aicoders / RE tooling si memory accesses
**dopočítají** deterministickou re-simulací z (initial RAM dump v
headeru cputrack + sekvence opcodů + IN values v iorqlog). Žádný HW
komponent na MZ-800/700/1500 nemění RAM autonomně bez Z80 instrukce.
