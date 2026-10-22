# CPU Tracking Log - formát exportních souborů

## Co je cputrack

**cputrack** je první ze čtyř subsystémů trace-suite (společně s
`iorqlog`, `intlog`, `hwlog`). Loguje **jeden záznam za každou
dokončenou Z80 instrukci** během běhu emulátoru. Slouží pro reverse
engineering, analýzu hot paths, identifikaci self-modifying kódu
a externí re-simulaci stavu paměti.

cputrack se ovládá z menu debug okna v položce
`Debugger Settings -> Trace Suite -> CPU Track`. Má tři režimy:

- `Off` (výchozí) - žádné zaznamenávání, žádná režie.
- `Only With Debug Window` - aktivní jen pokud je otevřené debug okno.
- `Always` - aktivní trvale.

V témže menu jsou dále volby `Save on Exit` (finalizace záznamu při
ukončení emulátoru), `Max size [MB]` (limit velikosti záznamu na kanál,
0 = bez omezení), `Chunk [MB]` (velikost RAM bufferu), `Set directory...`
(cílový adresář), `Record registers` (registry u každé instrukce, formát v3)
a PC range filtr (lo/hi). Basename (name) se nastavuje jen
přes CLI / INI. Všechny volby jsou persistentní v ini sekci `[TRACE_CPUTRACK]`
(klíče `mode`, `dir`, `name`, `chunk_mb`, `max_total_mb`, `save_on_exit`,
`regs`, `pc_range_lo`, `pc_range_hi`).

Recording běží ve společné pomalé cestě s CDL/cpuhist (callback swap),
hot path emulátoru bez aktivních diagnostik zůstává beze změny vůči
vanilní verzi.

## Adresářová struktura exportu

```
<dir>/
    <name>.json                    # meta + chunks anchor + initial regs
    <name>_initial_ram.bin         # 64 KB CPU RAM dump
    <name>_initial_vram.bin        # VRAM: 16 KB na MZ-800, 4 KB na MZ-700 a MZ-1500
    <name>_initial_exvram.bin      # 16 KB EXVRAM (jen MZ-800)
    <name>_initial_pcg.bin         # 24 KB PCG (jen MZ-1500)
    <name>_initial_memext.bin      # 512 KB Memext (pokud připojeno)
    <name>.000.bin                 # chunk 0 - per-instr eventy
    <name>.001.bin                 # chunk 1
    ...
```

Obsah RAMdisků se do počátečního stavu neukládá.

`<name>.json` je textový metadata soubor (viz níže). `<name>_initial_*.bin`
jsou snapshoty fyzických pamětí v okamžiku startu recordingu (= header).
`<name>.NNN.bin` jsou flat sekvence per-instrukčních eventů.

Velikost záznamu je v celém záznamu (segmentu) stejná: 12 B ve formátech v1
a v2, 32 B ve formátu v3 s registry (volba `regs`, viz
[Formát v3](#formát-v3-záznam-s-registry-32-b)). Platí hodnoty
`event_record_size` a `event_record_version` v manifestu.

## Per-event záznam (12 B, formáty v1 a v2)

| Offset | Velikost | Pole                                          |
|--------|----------|------------------------------------------------|
| 0      | 2 B      | regPC (uint16 LE)                              |
| 2      | 1 B      | insn_length (1-4)                              |
| 3      | 4 B      | insn_bytes (jen prvních insn_length valid)     |
| 7      | 4 B      | wait_clk (uint32 LE)                           |
| 11     | 1 B      | t_std - standardní T-stavy instrukce (formát v2) |

Endianita zápisu je host byte order; na všech podporovaných platformách
emulátoru (Windows x86_64, Linux x86_64) je to little-endian.

### Pole regPC

Adresa instrukce v okamžiku jejího provedení (= PC před vykonáním
instrukce, ne PC po něm). Pro `JP 1234h` je `regPC` adresou opcode `JP`,
ne cílem skoku.

### Pole insn_length

Počet platných bajtů v `insn_bytes`. Z80 instrukce mají délku 1-4 B
(prefix `CB`, `DD`, `ED`, `FD` + opcode + případné offset/imm bajty).

**Formát v2: `insn_length` 0 = přijetí přerušení.** Takový záznam nepatří
žádné instrukci: `regPC` je adresa obsluhy přerušení (PC po přijetí),
`insn_bytes` a `wait_clk` jsou 0 a `t_std` (bajt 11) nese T-stavy přijetí
(např. 19 u IM 2). Zapíše se mezi poslední instrukcí před přerušením
(nebo sbaleným záznamem HALT, který přerušení ukončilo) a první instrukcí
obsluhy. Díky němu součet `t_std + wait_clk` všech záznamů odpovídá
uplynulému času CPU.

### Pole insn_bytes

Až 4 bajty opcode + operandů. Bajty mimo `insn_length` jsou nedefinované
(pravděpodobně 0, ale parser nesmí spoléhat).

Jsou to bajty, které CPU při vykonání instrukce načetlo (fetch), takže
sedí i u instrukce, která přepíše vlastní bajty nebo pod sebou přepne
banking paměti (hned po instrukci může být na `regPC` v paměti něco
jiného).

Z `regPC` + `insn_bytes[0..insn_length-1]` lze plně rekonstruovat:
- co Z80 vykonal (= disassemble)
- standardní délku v T-states (Z80 ISA tabulka)
- které registry instrukce čte / zapisuje
- které memory operations dělá (z register stavu, který si externí
  re-simulátor sám trackuje)

### Pole wait_clk

Počet **WAIT T-states navíc** nad rámec standardní délky instrukce.

| Hodnota | Význam |
|---------|--------|
| `0` | Žádný WAIT (běžný případ pro většinu instrukcí v běžném stavu emulátoru). |
| `1..0xFFFFFFFE` | WAIT trval N CPU T-states. |
| `0xFFFFFFFF` | **Saturated sentinel** - WAIT byl delší než ~20 minut při 3.5 MHz, hodnota přetekla uint32. Externí parser ví, že "dlouho nic, nelze přesně určit kolik". Reálný Sharp by za tu dobu beztoho ztratil DRAM (refresh), emu může pauzu udržet do nekonečna. |

#### Zdroj WAIT T-states

Hodnota `wait_clk` pochází ze dvou hot-path míst, kde emulátor vkládá
WAIT cycles:

1. **Přístup CPU do VRAM přes GDG** - CPU čeká na volný cyklus paměti
   videa. Na MZ-800 v obou režimech (v režimu MZ-800 grafická VRAM,
   v režimu MZ-700 CG-RAM a textová VRAM): čtení čeká vždy, zápis jde
   přes jednobajtový buffer. Velikost závisí na pozici paprsku v řádku.
   Na MZ-700 a MZ-1500 jen přístup do VRAM ve viditelné části řádku.
2. **OUT na PSG** - WAIT kvůli synchronizaci CPU CLK s PSGCLK (PSG je
   1.108 MHz, CPU 3.546 MHz; write musí trefit PSG cycle boundary).

Akumulátor WAIT T-states je saturující uint32. Pokud HALT/self-loop
collapse je aktivní, akumulace pokračuje napříč iteracemi (= součet
všech WAIT + všech standard T-states od první iterace).

`insn_length`/`insn_bytes` v eventu odpovídá **standardní instrukci**
(= bez WAIT). Standardní délka v T-states je odvoditelná z opcode přes
Z80 ISA (viz dále).

### Pole t_std (bajt 11, formát v2)

Standardní délka instrukce v T-stavech bez WAIT, tak jak ji emulátor při
vykonání skutečně spočítal (u podmíněných skoků podle toho, jestli se
skočilo). U sbaleného záznamu HALT / smyčky na sebe je 0, protože celý
čas smyčky nese `wait_clk`.

Formát záznamu určuje `subsys_header.event_record_version` v manifestu:
2 = bajt 11 nese `t_std`. Manifest bez tohoto pole je formát v1, kde byl
bajt 11 rezerva (vždy 0) a délka instrukce se musela odvodit z opcode
podle tabulky Z80 (u podmíněných skoků ze stavu příznaků).

### HALT a self-loop collapse

HALT, `JP $`, `JR $-2`, `CALL` na sebe (= libovolná instrukce, která
zanechá PC na stejné adrese a opakuje se) se sbaluje do **jednoho
eventu** s `wait_clk` = celkový čas strávený ve smyčce (do exit přes
IRQ accept / state change).

Důsledek: tight-loop "wait for IRQ" idiom v MZ-800 monitoru / hrách
(`HALT` nebo `JR $-2` v polling smyčce) negeneruje desetitisíce events
za sekundu, ale jeden event s `wait_clk` ekvivalent doby čekání. To
je side-effect cputrack designu - **bez collapse by byl log na real
software nepoužitelný**.

Externí re-simulátor pozná collapsed HALT jednoduše: opcode = `0x76`
(HALT) + nenulové `wait_clk`. Pozná collapsed self-loop: instrukce
mění PC na PC, a `wait_clk` > 0.

## Formát v3: záznam s registry (32 B)

Se zapnutou volbou `regs` (menu `Record registers`, INI `regs = 1`, CLI
`--cputrack-regs on`, MCP `trace_start` s `regs: true`) zapisuje nový
záznam 32 B na událost. Volba platí od dalšího startu záznamu; jeden
záznam (segment) má vždy jednu velikost. Bajty 0-11 mají stejný význam
jako ve v2, za nimi je stav CPU **po** události záznamu:

| Offset | Velikost | Pole |
|--------|----------|------|
| 0-11   | 12 B     | jako v2 (regPC, insn_length, insn_bytes, wait_clk, t_std) |
| 12     | 16 B     | AF, BC, DE, HL, IX, IY, SP, WZ (uint16 LE, v tomto pořadí) |
| 28     | 1 B      | I |
| 29     | 1 B      | R (celý bajt včetně bitu 7) |
| 30     | 1 B      | příznaky: bit 0 IFF1, bit 1 IFF2, bity 2-3 IM, bit 4 HALT |
| 31     | 1 B      | druh záznamu (bity 0-3), důvod synchronizace (bity 4-6), bit 7 = stav před neznámý |

Druh záznamu (bajt 31, bity 0-3):

| Hodnota | Druh | Stav v bajtech 12-30 |
|---------|------|----------------------|
| 0 | instrukce | po instrukci |
| 1 | sbalená smyčka (HALT, smyčka na sebe; `t_std` 0, čas ve `wait_clk`) | po poslední sbalené iteraci |
| 2 | přijetí přerušení (`insn_length` 0) | po přijetí (PC = obsluha, SP o 2 níž, IFF vynulované) |
| 3 | synchronizace (`insn_length` 0FEh) | aktuální stav CPU |

**Stav před událostí** je stav v předchozím záznamu (libovolného druhu).
Pokud má záznam nastavený bit 7 bajtu 31, stav před ním znám není (řetěz
přerušil filtr PC a nešlo ho obnovit, např. přerušení obsloužené v rozsahu
filtru hned po instrukci mimo něj). První záznam záznamu je vždy
synchronizace, takže i první instrukce má stav před známý.

### Synchronizační záznam

Synchronizační záznam (`insn_length` 0FEh) nepatří žádné instrukci a do
času se nepočítá (`t_std` 0, pole `wait_clk` neplatí):

- `regPC` = PC příští instrukce,
- bajty 3-10 = alternativní sada AF', BC', DE', HL' (uint16 LE),
- bajty 12-30 = aktuální stav CPU,
- bity 4-6 bajtu 31 = důvod: 0 začátek záznamu, 1 návrat do rozsahu
  filtru PC, 2 změna stavu CPU mimo instrukci.

Zapíše se na začátku záznamu, když instrukce mimo rozsah filtru PC vede
do něj (první instrukce v rozsahu pak má stav před známý), a když se
registry změní mimo instrukci: zápis registru z debuggeru nebo přes MCP,
akce breakpointu `set` na adrese instrukce, reset, načtení snapshotu
(i při rewind a seek přehrávání), zápis registru přehraný ze záznamu
vstupů, spuštění MZF bootstrapem. Synchronizace
se zapíše jen při skutečné změně; pauza sama o sobě nic nezapíše.

Alternativní sadu nese jen synchronizační záznam. Mezi synchronizacemi ji
čtenář odvodí z instrukcí `EXX` a `EX AF,AF'` (prohazují ji s hlavní
sadou).

### Cena

Záznam s registry je 2,7krát větší než v2 (na MAX SPEED u programů
Gardener a Batman Demo asi 1,4-1,8 GB dat za půl minuty místo 0,6-0,8 GB)
a nahrávání trvá asi o 11-18 % déle než v2. Bez volby `regs` se záznam v2
nemění.

## Manifest `<name>.json`

```json
{
  "subsys": "cputrack",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 354432,
  "truncated": false,
  "truncated_reason": "",
  "chunks": [
    {
      "index": 0,
      "file": "cputrack.000.bin",
      "bytes": 96252,
      "start_pxclk": 354450,
      "start_cpuclk": 70890,
      "start_screens": 1
    }
  ],
  "subsys_header": {
    "start_pxclk": 354450,
    "start_pxclk_in_screen": 18,
    "start_screens": 1,
    "start_cpuclk": 70890,
    "initial_regs": {
      "AF": 36762, "BC": 10, "DE": 0, "HL": 57347,
      "AF_alt": 0, "BC_alt": 0, "DE_alt": 0, "HL_alt": 0,
      "IX": 0, "IY": 0, "SP": 4332, "PC": 58391,
      "WZ": 58391, "IR": 108, "I": 0, "R": 108,
      "IM": 1, "IFF1": 1, "IFF2": 1, "Q": 0, "halted": 0
    },
    "initial_ram_file": "cputrack_initial_ram.bin",
    "initial_vram_file": "cputrack_initial_vram.bin",
    "initial_exvram_file": "cputrack_initial_exvram.bin",
    "initial_memext_file": "cputrack_initial_memext.bin",
    "memext_size": 524288,
    "event_record_size": 12,
    "event_record_version": 2
  }
}
```

Příklad je skutečný manifest záznamu jednoho snímku na MZ-800 s připojeným
MemExtem. Delší záznam má v `chunks[]` další položky stejného tvaru.
Záznam s registry má `"event_record_size": 32` a
`"event_record_version": 3`.

### Klíčová pole

- `subsys` - vždy `"cputrack"`.
- `platform` - `"MZ-800"`, `"MZ-700"` nebo `"MZ-1500"`.
- `pxclk_freq` - frekvence pixel clock (Hz). MZ-800/700/1500 standard:
  17734475 (= 17.7 MHz). Lze odvodit i `cpu_freq = pxclk_freq /
  cpu_divider`.
- `cpu_divider` - dělič pxCLK pro CPU CLK. MZ-800: `5` (CPU 3.546895 MHz).
- `pxclk_per_screen` - počet pxCLK na jednu obrazovku (raster cycle).
  Použitelné pro raster-aligned analýzu.
- `chunks[]` - seznam chunků v pořadí. Každý chunk je nezávislý
  binární soubor.
- `truncated` - `true` pokud recording dosáhl `max_total_mb` limitu.
- `truncated_reason` - vysvětlení (`"max_total_mb"` nebo prázdné).

### chunks[i] anchor

Každý chunk má anchor s `start_pxclk`, `start_cpuclk`, `start_screens`
v okamžiku začátku chunku. To umožňuje **O(1) seek na chunk** + lineární
scan uvnitř pro rekonstrukci timestampu kterékoliv konkrétní instrukce:

```
cumul_cpuclk(N) = chunk.start_cpuclk + sum_{i=0..N}( insn_T_states[i] + wait_clk[i] )
cumul_pxclk(N)  = chunk.start_pxclk + (cumul_cpuclk(N) - chunk.start_cpuclk) * cpu_divider
screens_at(N)   = floor(cumul_pxclk(N) / pxclk_per_screen)
pxclk_in_screen_at(N) = cumul_pxclk(N) mod pxclk_per_screen
```

`insn_T_states[i]` je ve formátu v2 bajt 11 (`t_std`), ve v1 se odvozuje z opcode podle Z80 ISA tabulky. Ve v2 vzorec sedí přesně i s přerušeními (záznamy přijetí přerušení nesou jeho T-stavy); ve v1 chybí T-stavy přijetí každého přerušení. Ve v3 se synchronizační záznamy (`insn_length` 0FEh) do součtu nepočítají.

### subsys_header: počáteční stav

`initial_regs` obsahuje kompletní snímek Z80 CPU registrů v okamžiku
startu recordingu (před prvním logovaným eventem). Hodnoty jsou celá čísla
(desítkově): 16bitové páry (`AF` ... `PC`, `WZ`, `IR`), samostatně `I`
a `R`, režim přerušení `IM`, `IFF1`, `IFF2`, `Q` a `halted` (1 = CPU
stojí na HALT).

`initial_*_file` jsou jména souborů s počátečními dumpy fyzických pamětí,
relativně k adresáři manifestu: `initial_ram_file` a `initial_vram_file`
vždy, `initial_exvram_file` jen na MZ-800, `initial_pcg_file` jen na
MZ-1500. `initial_memext_file` je `null`, když MemExt není připojený;
jinak je u něj i `memext_size` (velikost dumpu v bajtech). Velikosti
ostatních dumpů jsou v tabulkách níže.

Initial state + sekvence events je **hermeticky uzavřený dataset** -
externí re-simulátor z toho deterministicky odvodí stav paměti
v každém okamžiku (= žádný HW na MZ-800/700/1500 nemění RAM autonomně,
veškerá data se objevují přes IORQ, který logujeme zvlášť v `iorqlog`).

## Initial memory dumps

Per-platforma se mohou lišit jednotlivé regiony. V hlavičce jsou jen
ty, které pro danou architekturu existují / jsou připojené.

### MZ-800

| Soubor | Velikost | Popis |
|--------|----------|-------|
| `<name>_initial_ram.bin` | 64 KB | Hlavní DRAM |
| `<name>_initial_vram.bin` | 16 KB | VRAM roviny I a II (po 8 KB); v režimu MZ-700 jsou v rovině I i CG-RAM a textová VRAM |
| `<name>_initial_exvram.bin` | 16 KB | EXVRAM, roviny III a IV (po 8 KB) |
| `<name>_initial_memext.bin` | 512 KB | Memext RAM (jen pokud připojen) |

ROM se **neukládá** - je deterministicky známá z compile-time embedded
ROM image (= verze v manifestu `<name>.json` přes `platform`). Obsah
RAMdisků se neukládá.

### MZ-700

| Soubor | Velikost | Popis |
|--------|----------|-------|
| `<name>_initial_ram.bin` | 64 KB | Hlavní DRAM |
| `<name>_initial_vram.bin` | 4 KB | Znaková a atributová VRAM |
| `<name>_initial_memext.bin` | 512 KB | Memext (pokud připojeno) |

### MZ-1500

| Soubor | Velikost | Popis |
|--------|----------|-------|
| `<name>_initial_ram.bin` | 64 KB | Hlavní DRAM |
| `<name>_initial_vram.bin` | 4 KB | Znaková a atributová VRAM |
| `<name>_initial_pcg.bin` | 24 KB | 3× 8 KB PCG bank (R/G/B) |
| `<name>_initial_memext.bin` | 512 KB | Memext (pokud připojeno) |

## Použití

### Z UI

`Debugger Settings -> Trace Suite -> CPU Track`:

1. Vybrat režim recording (Off / Only With Debug Window / Always).
2. Volitelně zapnout `Save on Exit` (default zapnuto) a `Record registers`
   (formát v3 s registry).
3. Volitelně nastavit `Max size [MB]` (limit), `Chunk [MB]`, cílový adresář
   (`Set directory...`) a PC range filtr; basename (name) přes CLI / INI.
4. Spustit emulovaný program. Recording běží na pozadí.

### Načtení dat v Pythonu (příklad)

```python
import json, struct
from pathlib import Path

def load_cputrack(meta_path):
    meta_path = Path(meta_path)
    meta = json.loads(meta_path.read_text())
    base_dir = meta_path.parent
    rs = meta["subsys_header"].get("event_record_size", 12)   # 12 nebo 32 (v3)

    events = []
    for chunk in meta["chunks"]:
        data = (base_dir / chunk["file"]).read_bytes()
        n_events = chunk["bytes"] // rs
        for i in range(n_events):
            o = i * rs
            pc          = struct.unpack_from("<H", data, o + 0)[0]
            insn_length = data[o + 2]
            insn_bytes  = data[o + 3 : o + 3 + insn_length]
            wait_clk    = struct.unpack_from("<I", data, o + 7)[0]
            events.append((pc, insn_length, bytes(insn_bytes), wait_clk))
    return meta, events

meta, events = load_cputrack("./trace-suite/cputrack.json")
# Příklad: kolik bylo HALT instrukcí
halt_count = sum(1 for (_, l, b, _) in events if l == 1 and b[0] == 0x76)
```
