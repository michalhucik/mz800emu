# Breakpoint persistence - `.bpt` JSON formát

Breakpointy se ukládají do JSON souboru s příponou `.bpt` (default
`mz800-breakpoints.bpt`, cesta z INI sekce `[BREAKPOINTS]` `default_file`).
Soubor je human-readable (pretty print indent 2), takže lze editovat
ručně, version-control-ovat nebo diffovat.

Tento dokument je úplná reference formátu - top-level struktura, všechna
pole `breakpoint` objektu, BC alias tabulka, vyřazené stringy.

## Top-level struktura

```json
{
  "schema_version": "V1.5",
  "version": 1,
  "groups": [ ... ],
  "breakpoints": [ ... ],
  "vars": [ ... ]
}
```

| Klíč | Typ | Význam |
|------|-----|--------|
| `schema_version` | string | Řetězcový tag schématu. Liší se od číselného `version` - readable label pro forward compatibility. Loader toleruje missing klíč i neznámou hodnotu (= warning, ale parse pokračuje). |
| `version` | int | Číselná verze formátu. Loader toleruje rozdíl mezi soubor verzí a aktuální - jen logne info "proceeding best-effort". |
| `groups` | array | Hierarchie skupin (parent / child) - viz "Group object". |
| `breakpoints` | array | Vlastní breakpointy - viz "Breakpoint object". |
| `vars` | array | $name uživatelské proměnné - viz "Vars sekce". |

Pořadí klíčů na top-level je stable (saver vždy v tomto pořadí). Loader
je permisivní - klíče mohou chybět (= prázdný stav) a neznámé klíče se
ignorují.

## Schema versioning

Aktuální hodnota schématu je `"V1.5"`. Saver vždy emituje tuto hodnotu
jako první klíč root objektu. Loader na ni reaguje takto:

| Stav v souboru | Chování loaderu |
|----------------|-----------------|
| Klíč chybí | Info log, parse pokračuje. Pokrývá všechny historické `.bpt` soubory zapsané před zavedením schema_version. |
| Hodnota odpovídá aktuální | Silent (= běžný stav). |
| Jiná hodnota | Warning na stderr, ale parse pokračuje (forward-compat: starší emu best-effort načte novější soubor). |

Soubor zapsaný bez `schema_version` se načte s info log message, ale
bez chyb. Po prvním save se klíč doplní (= silent migration při
následujícím save).

**Future migration:**

Při incompatible structural change (= např. přejmenované klíče, změněné
sémantiky polí) se navýší tag schématu a rozhodne strategie (= per-version
větvení v loaderu nebo migration script). Drobnosti (= přidaná pole,
nová enum hodnota) zachovat per-klíč BC fallback mechanismus a tag
neměnit - missing pole se interpretuje jako default a saver po rewritu
doplní.

## Group object

```json
{
  "id": 1,
  "parent_id": -1,
  "name": "ROM monitor",
  "enabled": true,
  "order": 1.0,
  "color_bg": 0,
  "color_fg": 16777215
}
```

| Klíč | Typ | Default při loadu | Význam |
|------|-----|-------------------|--------|
| `id` | int | - (povinný; chybí = skip s warningem) | Unikátní ID skupiny. |
| `parent_id` | int | -1 | -1 = root, jinak ID rodičovské skupiny. |
| `name` | string | `"Group <id>"` | Display name. Prázdný / NULL = auto. |
| `enabled` | bool | true | Cascade enable - pokud false, všechny BP v této skupině se nevyhodnocují. |
| `order` | double | 0.0 | Pořadí zobrazení v UI. |
| `color_bg` | int | `0x000000` | RGB barva pozadí labelu (0xRRGGBB). |
| `color_fg` | int | `0xFFFFFF` | RGB barva textu labelu. |

## Breakpoint object - společná pole

Tato pole se ukládají pro všechny BP typy bez ohledu na relevanci.
Pole irelevantní pro daný typ se ignorují při enforce, ale stále se
serializují (= konzistentní schema).

```json
{
  "id": 1,
  "parent_id": -1,
  "type": "PC_EXEC",
  "addr": 4660,
  "addr_end": 4660,
  "addr_match_mode": "SINGLE",
  "addr_mask": 65535,
  "zone": "CPU_VIEW",
  "bank_id": 0,
  "bank_id_end": 0,
  "bank_match_mode": "SINGLE",
  "bank_id_mask": 255,
  "bp_addr_space": "cpu_view",
  "port": 0,
  "port_end": 0,
  "port_match_mode": "SINGLE",
  "port_mask": 65535,
  "port_mode": "8BIT",
  "event_name": null,
  "event_trigger": "rising",
  "sp_threshold": 0,
  "sp_upper": 0,
  "sp_mode": "SINGLE",
  "im0_enabled": true,
  "im1_enabled": true,
  "im2_enabled": true,
  "im0_rst_mask": 0,
  "im2_vector_enabled": false,
  "im2_vector_addr": 0,
  "im2_isr_enabled": false,
  "im2_isr_addr": 0,
  "irq_sig_sources": [],
  "expr": null,
  "action": null,
  "hit_count": 0,
  "skip_count": 0,
  "edge_triggered": false,
  "auto_name": true,
  "name": "Addr: 0x1234",
  "enabled": true,
  "color_bg": 0,
  "color_fg": 16777215,
  "hits": 0
}
```

### Identifikace a hierarchie

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `id` | int | - (povinný) | Unikátní ID BP. Chybí / neplatný = skip. |
| `parent_id` | int | -1 | -1 = root, jinak ID rodičovské skupiny. |
| `name` | string | `"Addr: 0x<addr>"` | Display name. |
| `auto_name` | bool | true | Pokud true, name se regeneruje při změně adresy. |
| `enabled` | bool | true | BP enabled flag. |
| `color_bg` | int | `0x000000` | RGB pozadí. |
| `color_fg` | int | `0xFFFFFF` | RGB text. |
| `hits` | int (uint64) | 0 | Hit counter (display only, persist statistik). |

### Typ a doplňkové pole

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `type` | string | `"PC_EXEC"` | BP typ - viz `types.md`. Neznámý -> fallback PC_EXEC s warningem. |
| `expr` | string \| null | null | Condition expression - viz `expression-syntax.md`. |
| `action` | string \| null | null | Action mini-DSL - viz `action-dsl.md`. NULL = stop. |
| `hit_count` | int (uint32) | 0 | Trigger až po N. hitu (0 = každý). |
| `skip_count` | int (uint32) | 0 | Skip prvních N hitů. |
| `edge_triggered` | bool | false | Edge tracking (relevantní hlavně pro GLOBAL). |

## Per-typ specifická pole

### Adresové pole (PC_EXEC, MEM_R, MEM_W)

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `addr` | int (uint16) | 0 | Primární adresa. |
| `addr_end` | int (uint16) | = `addr` | RANGE upper. |
| `addr_match_mode` | string | `"SINGLE"` | SINGLE / RANGE / MASK. |
| `addr_mask` | int (uint16) | 65535 (0xFFFF) | AND mask pro MASK. |
| `zone` | string | `"CPU_VIEW"` | Banking zone - viz "Zone strings" níže. |
| `bank_id` | int (uint8) | 0 | Bank index pro MMEXT_BANK zone. |
| `bank_id_end` | int (uint8) | 0 | RANGE upper pro bank. |
| `bank_match_mode` | string | `"SINGLE"` | SINGLE / RANGE / MASK pro bank. |
| `bank_id_mask` | int (uint8) | 255 (0xFF) | AND mask pro bank. |
| `bp_addr_space` | string | `"cpu_view"` | Interpretace `addr` pro MEM_R / MEM_W v zóně MMEXT_BANK (jen PEHU): `cpu_view` = `addr` je Z80 adresa; `bank_offset` = `addr` je offset `0x0000-0x1FFF` v bance. Viz `match-modes.md`. |

### IORQ pole (IORQ_R, IORQ_W)

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `port` | int (uint16) | 0 | Primární port. |
| `port_end` | int (uint16) | 0 | RANGE upper. |
| `port_match_mode` | string | `"SINGLE"` | SINGLE / RANGE / MASK. |
| `port_mask` | int (uint16) | 65535 (0xFFFF) | AND mask. |
| `port_mode` | string | `"8BIT"` | 8BIT / 16BIT. |

### HW_EVENT pole

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `event_name` | string \| null | null | Event jméno (např. `"vsync"`, `"raster:192"`). Persistence stable - viz "Event strings". |
| `event_trigger` | string | `"rising"` | Trigger condition pro signal events: low / high / rising / falling / changed. |

Pozn.: parsed event (enum) a event_param se neserializují - jsou cache
odvozená z `event_name` při loadu.

### SP_THRESHOLD pole

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `sp_threshold` | int (uint16) | 0 | Práh / lower bound. |
| `sp_upper` | int (uint16) | 0 | WINDOW upper bound. |
| `sp_mode` | string | `"SINGLE"` | SINGLE / WINDOW. |

### IRQ pole

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `im0_enabled` | bool | true | Fire na IM 0 dispatch. |
| `im1_enabled` | bool | true | Fire na IM 1 dispatch. |
| `im2_enabled` | bool | true | Fire na IM 2 dispatch. |
| `im0_rst_mask` | int (uint8) | 0 | IM 0 RST opcode bitmask (0 = match-all). Bit i = RST opcode 0xC7 + i*8. |
| `im2_vector_enabled` | bool | false | Filter na IM 2 vector address. |
| `im2_vector_addr` | int (uint16) | 0 | Očekávaný `(I << 8) \| (vec & 0xFE)`. |
| `im2_isr_enabled` | bool | false | Filter na IM 2 ISR target. |
| `im2_isr_addr` | int (uint16) | 0 | Očekávaná ISR adresa. |

### IRQ_SIG pole

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `irq_sig_sources` | array of string | `[]` | Bitmask sources jako pole stable jmen - viz "IRQ_SIG source strings". Prázdné = mask 0 = invalid (UI validation). |

## Vars sekce

`$name` uživatelské proměnné - sdílené napříč všemi BP, persist přes
session. Detail viz `vars.md`.

```json
"vars": [
  {
    "name": "hits",
    "value": 42,
    "comment": "trigger counter",
    "persist_value": true
  },
  {
    "name": "frame_runtime",
    "comment": "reset on each load",
    "persist_value": false
  }
]
```

| Klíč | Typ | Default při missing | Význam |
|------|-----|--------------------|--------|
| `name` | string | (povinné) | Identifikátor bez `$` prefixu. Prázdné = skip s warning. Validace regex `^[a-zA-Z_][a-zA-Z0-9_]*$`. |
| `value` | int (int32) | `0` | Aktuální hodnota. **Emit jen pokud `persist_value=true`** (= save). Při load + `persist_value=false` se hodnota ignoruje (warning). |
| `comment` | string | `null` | User komentář (max 256 znaků). Emit jen pokud non-NULL & non-empty. |
| `persist_value` | bool | `true` | Uložit value? `true` = uloží + restore při load. `false` = uloží jen name + comment, value vždy 0 při load (counter pattern). |

**Per-klíč BC fallback** při načítání starších souborů:
- Missing `comment` -> `NULL`.
- Missing `persist_value` -> `true` (= legacy chování, value zachována).

Při loadu se storage nejprve vyčistí, pak se aplikuje pole `vars`.
Pořadí v souboru = pořadí insertu (lineární storage, není sortováno).

Plus per-arch standalone `.vars` file (`mz800.vars` / `mz1500.vars` /
`mz700.vars`) má identické schema (= subset `.bpt` JSON s jen sekcí
`"vars"`). Auto-load při startu emu (cfg `[BP_VARS] auto_load=1`)
přepíše vars načtené z `.bpt` (= explicit per-arch separace má přednost).

## String stable names

### `type` strings

| String | Význam |
|--------|--------|
| `PC_EXEC` | PC execution breakpoint |
| `MEM_R` | Memory read |
| `MEM_W` | Memory write |
| `IORQ_R` | I/O port read |
| `IORQ_W` | I/O port write |
| `IRQ` | Post-dispatch IRQ filter |
| `HW_EVENT` | HW event observer |
| `SP_THRESHOLD` | Stack pointer threshold |
| `GLOBAL` | Per-instruction global condition |
| `IRQ_SIG` | Pre-dispatch IRQ source filter |

Loader fallback při neznámém stringu = PC_EXEC + warning na stderr.

### `zone` strings

| String | Význam |
|--------|--------|
| `CPU_VIEW` | default, banking-agnostic |
| `ROM_LOWER` | Monitor ROM - MZ-800: 0x0000-0x1FFF; MZ-700/1500: 0x0000-0x0FFF |
| `ROM_UPPER` | horní ROM 0xE000-0xFFFF; na MZ-700/1500 zahrnuje i mapped ports 0xE000-0xE00F |
| `RAM` | RAM |
| `VRAM_FB` | VRAM framebuffer banking-aware okno |
| `PCG` | MZ-800 v MZ-700 modu = CG-RAM; MZ-1500 = PCG bank 1/2/3; MZ-700: vždy false |
| `MMEXT_BANK` | memory expansion overlay bank - platform-neutral PEHU detect |

BC aliasy (existující `.bpt` soubory se akceptují bez warningu):
- `PEHU_BANK` -> `MMEXT_BANK`
- `VRAM_RF` -> `VRAM_FB` (RF/WF registry jsou MZ-800-only koncept,
  "framebuffer" je platform-neutrální)

### `addr_match_mode`, `port_match_mode`, `bank_match_mode`

| String | Význam |
|--------|--------|
| `SINGLE` | single hodnota |
| `RANGE` | rozsah lower..upper |
| `MASK` | AND mask |

### `bp_addr_space`

Platí jen pro MEM_R / MEM_W v zóně MMEXT_BANK (PEHU). Viz `match-modes.md`.

| String | Význam |
|--------|--------|
| `cpu_view` | default - `addr` je Z80 adresa (banking-aware) |
| `bank_offset` | `addr` je offset `0x0000-0x1FFF` v PEHU bance |

Soubory bez tohoto klíče se načtou s defaultem `cpu_view`.

### `sp_mode`

| String | Význam |
|--------|--------|
| `SINGLE` | jednoduchý threshold |
| `WINDOW` | dolní + horní mez |

### `port_mode`

| String | Význam |
|--------|--------|
| `8BIT` | match jen low byte portu |
| `16BIT` | match celé 16-bit BC při `IN r,(C)` |

### `event_trigger`

| String | Význam |
|--------|--------|
| `rising` | náběžná hrana |
| `falling` | sestupná hrana |
| `changed` | jakákoliv změna |
| `low` | level low |
| `high` | level high |

### `event_name` strings

28 events ve 4 kategoriích. Detail v `hw-events.md`. Stručný přehled:

**Signal events (17)** - mají trigger condition:

`vsync`, `hsync`, `vbln`, `hbln`, `ctc:zc0`, `ctc:zc1`, `ctc:zc2`,
`irq:ctc2`, `irq:pioz80_a`, `irq:pioz80_b`, `irq:fdc`, `tempo`,
`cursor`, `cmt:in`, `cmt:out`, `cmt:mstate`, `cmt:motor`

**Change events (5)** - implicit "happened":

`mode_change`, `palette_change`, `palgrp_change`, `border_change`,
`cksw_change`

**Point event s parametrem (1)**:

`raster:N` (N = 0..65535, typicky 0..311 pro MZ-800 raster row)

**CPU events (6)** - point/state:

`cpu:nmi`, `cpu:di`, `cpu:im_change`, `cpu:iff_change`, `cpu:halt`,
`cpu:reset`

### `irq_sig_sources` strings

| String | Bit |
|--------|-----|
| `PIOZ80_A` | 0 (0x01) |
| `PIOZ80_B` | 1 (0x02) |
| `CTC2` | 2 (0x04) |
| `FDC` | 3 (0x08) |
| `OTHER` | 4 (0x10) |

Pole array stable jmen pro odolnost vůči případnému reorderu enum bitů
(bity samy zůstávají stable, ale string array je čitelnější v souboru).

## BC alias kompletní tabulka

Loader akceptuje legacy stringy a transparentně mapuje na aktuální
jména. Saver vždy ukládá aktuální jméno.

### Zone aliasy

| Legacy string | Aktuální | Důvod |
|---------------|----------|-------|
| `PEHU_BANK` | `MMEXT_BANK` | rename (memory expansion overlay) |
| `VRAM_RF` | `VRAM_FB` | RF/WF je MZ-800-only koncept, "framebuffer" je platform-neutrální |

### Event aliasy

| Legacy string | Aktuální | Důvod |
|---------------|----------|-------|
| `pio:porta_int` | `irq:pioz80_a` | HWE rename (sjednocení namespace) |
| `pio:portb_int` | `irq:pioz80_b` | dtto |
| `fdc:irq` | `irq:fdc` | dtto |
| `tape:edge` | `cmt:in` | HWE rename (CMT = cassette tape) |
| `nmi` | `cpu:nmi` | pre-HWE CPU prefix legacy |
| `di` | `cpu:di` | dtto |
| `im_change` | `cpu:im_change` | dtto |
| `iff_change` | `cpu:iff_change` | dtto |
| `halt` | `cpu:halt` | dtto |
| `reset` | `cpu:reset` | dtto |

## Vyřazené persistence stringy

Tyto event stringy byly **vyřazeny** v rámci HWE redesignu (důvody:
redundance s jinými typy, špatná granularita, neimplementované hook
sites). Loader na ně reaguje "unknown event_name" warningem na stderr
a BP se loadne s event = NONE - **fire never**.

| Vyřazený string | Důvod |
|-----------------|-------|
| `psg:int_pa5` | nahrazen `irq:pioz80_b` (PSG INT signal je na PIOZ80 portu B) |
| `psg:reg_write` | granularita PSG register write není stabilní (= per-emu interpretace) |
| `ppi:pa_write` | nahrazeno IORQ_W na konkrétním portu |
| `ppi:pc_change` | dtto |
| `fdc:command` | nestabilní (= per-WD279x revision interpretation) |
| `fdc:drq` | granularita - obvykle není potřeba |
| `ctc:gate0_edge` | gate signal není v emu cycle-accurately reprezentován |
| `tape:read_byte` | byte-level granularita; pro signal use `cmt:in` |
| `tape:write_byte` | dtto pro `cmt:out` |
| `mmio:bank_switch` | nahrazeno IORQ_W na banking portech |
| `mmio:mode_change` | nahrazeno `mode_change` (HW_EVENT change kategorie) |

Doporučení pro migraci: pokud `.bpt` obsahuje vyřazený string, BP
ručně překonfigurujte na alternativní typ (typicky IORQ_W na konkrétní
port nebo `irq:*` event).

## Per-typ JSON příklady

Příklady vychází ze save formátu (= obsahují všechna společná pole).
Pole irelevantní pro daný typ jsou na default - loader je toleruje
a enforce je ignoruje.

### PC_EXEC

```json
{
  "id": 1, "parent_id": -1,
  "type": "PC_EXEC",
  "addr": 4096, "addr_end": 4096,
  "addr_match_mode": "SINGLE", "addr_mask": 65535,
  "zone": "CPU_VIEW", "bank_id": 0,
  "bank_id_end": 0, "bank_match_mode": "SINGLE", "bank_id_mask": 255,
  "bp_addr_space": "cpu_view",
  "port": 0, "port_end": 0, "port_match_mode": "SINGLE", "port_mask": 65535, "port_mode": "8BIT",
  "event_name": null, "event_trigger": "rising",
  "sp_threshold": 0, "sp_upper": 0, "sp_mode": "SINGLE",
  "im0_enabled": true, "im1_enabled": true, "im2_enabled": true, "im0_rst_mask": 0,
  "im2_vector_enabled": false, "im2_vector_addr": 0,
  "im2_isr_enabled": false, "im2_isr_addr": 0,
  "irq_sig_sources": [],
  "expr": "A == 0x42", "action": null,
  "hit_count": 0, "skip_count": 0, "edge_triggered": false,
  "auto_name": false, "name": "main entry",
  "enabled": true, "color_bg": 0, "color_fg": 16777215, "hits": 3
}
```

PC_EXEC s condition - zastaví na 0x1000 jen pokud A == 0x42.

### MEM_R s RANGE

```json
{
  "id": 2, "parent_id": -1, "type": "MEM_R",
  "addr": 32768, "addr_end": 33023,
  "addr_match_mode": "RANGE", "addr_mask": 65535,
  "zone": "CPU_VIEW", "bank_id": 0, "bank_id_end": 0,
  "bank_match_mode": "SINGLE", "bank_id_mask": 255,
  "bp_addr_space": "cpu_view",
  "port": 0, "port_end": 0, "port_match_mode": "SINGLE", "port_mask": 65535, "port_mode": "8BIT",
  "event_name": null, "event_trigger": "rising",
  "sp_threshold": 0, "sp_upper": 0, "sp_mode": "SINGLE",
  "im0_enabled": true, "im1_enabled": true, "im2_enabled": true, "im0_rst_mask": 0,
  "im2_vector_enabled": false, "im2_vector_addr": 0,
  "im2_isr_enabled": false, "im2_isr_addr": 0,
  "irq_sig_sources": [],
  "expr": null, "action": "log \"VRAM read [%X]=%X PC=%X\", Address, Value, PC",
  "hit_count": 0, "skip_count": 0, "edge_triggered": false,
  "auto_name": true, "name": "VRAM watch",
  "enabled": true, "color_bg": 0, "color_fg": 16777215, "hits": 0
}
```

Watch čtení VRAM oblasti 0x8000-0x80FF (= range 256 bajtů).

### MEM_W s MASK

```json
{
  "id": 3, "parent_id": -1, "type": "MEM_W",
  "addr": 53248, "addr_end": 53248,
  "addr_match_mode": "MASK", "addr_mask": 65520,
  "zone": "CPU_VIEW",
  "...": "(other defaults)"
}
```

`addr_mask = 0xFFF0` - watch zápisy na 0xD000..0xD00F (= horní 12 bitů
shody).

### IORQ_R s 16BIT port

```json
{
  "id": 4, "type": "IORQ_R",
  "port": 17102, "port_end": 0,
  "port_match_mode": "SINGLE", "port_mask": 65535,
  "port_mode": "16BIT",
  "...": "(other defaults)"
}
```

Match jen `IN r,(C)` s `BC = 0x42CE`. V `port_mode: 8BIT` (default) by
match jakékoliv IN pro low byte 0xCE.

### IORQ_W s RANGE

```json
{
  "id": 5, "type": "IORQ_W",
  "port": 240, "port_end": 243,
  "port_match_mode": "RANGE", "port_mask": 65535, "port_mode": "8BIT",
  "expr": null,
  "action": "log \"PAL[%X] = %X\", Address & 3, Value",
  "...": "(other defaults)"
}
```

Trace zápisy do GDG palety 0xF0-0xF3.

### IRQ s IM 2 vector filter

```json
{
  "id": 6, "type": "IRQ",
  "im0_enabled": false, "im1_enabled": false, "im2_enabled": true,
  "im0_rst_mask": 0,
  "im2_vector_enabled": true, "im2_vector_addr": 26880,
  "im2_isr_enabled": false, "im2_isr_addr": 0,
  "...": "(other defaults)"
}
```

Fire jen pro IM 2 dispatch s vector `(I << 8) | (vec & 0xFE) = 0x6900`.

### IRQ s IM 0 RST 38h filter

```json
{
  "id": 7, "type": "IRQ",
  "im0_enabled": true, "im1_enabled": false, "im2_enabled": false,
  "im0_rst_mask": 128,
  "im2_vector_enabled": false, "im2_isr_enabled": false,
  "...": "(other defaults)"
}
```

`im0_rst_mask = 0x80` (bit 7) = jen RST 38h opcode (0xFF). Detect
runaway NULL pointer call.

### HW_EVENT signal s trigger

```json
{
  "id": 8, "type": "HW_EVENT",
  "event_name": "vsync", "event_trigger": "rising",
  "expr": null, "action": null,
  "...": "(other defaults)"
}
```

Frame stop na rising edge vsync signálu.

### HW_EVENT raster:N

```json
{
  "id": 9, "type": "HW_EVENT",
  "event_name": "raster:192", "event_trigger": "rising",
  "...": "(other defaults)"
}
```

Trigger na raster row 192. `event_param` se neukládá samostatně - je v
suffixu `event_name`.

### SP_THRESHOLD WINDOW

```json
{
  "id": 10, "type": "SP_THRESHOLD",
  "sp_threshold": 61440, "sp_upper": 65535,
  "sp_mode": "WINDOW",
  "...": "(other defaults)"
}
```

Fire pokud SP opustí okno 0xF000..0xFFFF (= cross-task switch detect).

### GLOBAL s edge

```json
{
  "id": 11, "type": "GLOBAL",
  "expr": "HL == 0x8000 && A == 0x42",
  "edge_triggered": true,
  "action": "log \"hit at PC=%X\", PC",
  "...": "(other defaults)"
}
```

Per-instruction trigger na exact CPU stav, edge-triggered (= jen na
přechod false -> true).

### IRQ_SIG multi-source

```json
{
  "id": 12, "type": "IRQ_SIG",
  "irq_sig_sources": ["FDC", "CTC2"],
  "...": "(other defaults)"
}
```

Fire pokud edge raise INT line zahrnuje FDC nebo CTC2 (OR semantics).

## Load validation

Po deserializaci JSON loader spouští dvě validační rutiny:

### Group parent validation

Chrání proti chybně editovaným `.bpt` souborům (= cykly nebo dangling
parent_id v hierarchii skupin):

- **Dangling group parent** (parent ukazuje na neexistující skupinu):
  warning na stderr + reparent na root.
- **Self-loop** (group parent ukazuje na sebe sama): warning + reparent
  na root.
- **Uzavřený kruh přes víc skupin**: scan parent řetězce s depth
  limitem (max 32 úrovní). Dva detection signály: návrat na current
  group ID nebo překročení depth. Warning + reparent na root.
- **Dangling BP parent** (BP parent ukazuje na neexistující skupinu):
  warning + reparent na root.

Bez této validace by ručně vytvořený cyklický `.bpt` (= group A parent =
B, B parent = A) způsobil stack overflow při prvním BP enforce.

### Event address validation

Detekuje duplikované efektivně povolené eventy na stejné adrese (= dva
BP se stejnou `addr` a obě `enabled` + cascade enabled). Pozdější BP
se disabled, warning na stdout.

### Belt-and-suspenders runtime guard

I při selhání load validace runtime nesmí spadnout. Cascade enable
check má depth limit (max 32); po překročení vrátí `true` (= treat as
enabled, fallback bezpečný pro emu - BP se prostě fire) + warning na
stderr.

## Versioning policy

- Číselná verze formátu se neinkrementuje při každém přidání pole - jen
  při schématu, které **lomí** zpětnou kompatibilitu (= rename
  existujícího klíče, změna typu hodnoty, odebrání povinného klíče).
- Přidání nového klíče s rozumným defaultem = **non-breaking**,
  per-klíč BC fallback v loaderu (default při missing).
- Odebrání klíče = ignorovat při loadu, neukládat při savu (= soubor
  postupně "vyčistí" sám).

Loader strategie:

- Soubor verze nižší než aktuální = best-effort, použít defaulty pro
  nové klíče.
- Soubor verze vyšší než aktuální = best-effort, ignorovat neznámé klíče.
- Žádný hard fail - vždy logne "proceeding best-effort" a pokračuje.

Doporučení pro budoucnost:

- Pokud bude potřeba breaking změna (např. struct refaktoring),
  inkrementovat verzi, doplnit explicit migrator (= per-version load
  cesta) nebo použít convertor utility.
- Alias period - legacy string držet aspoň 2 verze (= 1 release na
  detekci, 1 release na migraci uživatelských souborů).
- Vyřazené eventy by ideálně měly mít explicit "deprecated" warning v
  loaderu (aktuálně dostanou jen "unknown event_name" - viz "Vyřazené
  persistence stringy").

## Související dokumenty

- `README.md` - orientace v subsystému
- `types.md` - katalog 9 BP typů
- `match-modes.md` - SINGLE / RANGE / MASK detail
- `expression-syntax.md` - condition expression
- `action-dsl.md` - action DSL
- `hw-events.md` - HW event vocabulary detail
- `irq-filter.md` - IRQ post-dispatch filter detail
- `irq-sig.md` - IRQ_SIG pre-dispatch filter detail
- `vars.md` - $vars user variables
