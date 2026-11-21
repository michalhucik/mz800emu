# Breakpoint persistence - `.bpt` JSON format

Breakpoints are saved into a JSON file with the `.bpt` extension
(default `mz800-breakpoints.bpt`, path from the INI section
`[BREAKPOINTS]` `default_file`). The file is human-readable (pretty
print indent 2), so it can be edited manually, version-controlled or
diffed.

This document is the complete format reference - top-level structure,
all fields of the `breakpoint` object, BC alias table, retired strings.

## Top-level structure

```json
{
  "schema_version": "V1.5",
  "version": 1,
  "groups": [ ... ],
  "breakpoints": [ ... ],
  "vars": [ ... ]
}
```

| Key | Type | Meaning |
|-----|------|---------|
| `schema_version` | string | A string tag of the schema. It differs from the numeric `version` - a readable label for forward compatibility. The loader tolerates a missing key and an unknown value (= warning, but parse continues). |
| `version` | int | Numeric format version. The loader tolerates a difference between the file version and the current one - it just logs info "proceeding best-effort". |
| `groups` | array | Group hierarchy (parent / child) - see "Group object". |
| `breakpoints` | array | The breakpoints themselves - see "Breakpoint object". |
| `vars` | array | $name user variables - see "Vars section". |

The order of keys at the top level is stable (the saver always uses
this order). The loader is permissive - keys may be missing (= empty
state) and unknown keys are ignored.

## Schema versioning

The current schema value is `"V1.5"`. The saver always emits this
value as the first key of the root object. The loader reacts as
follows:

| File state | Loader behavior |
|------------|-----------------|
| Key missing | Info log, parse continues. Covers all historical `.bpt` files written before schema_version was introduced. |
| Value matches current | Silent (= the usual state). |
| Different value | Warning to stderr, but parse continues (forward-compat: an older emu reads a newer file best-effort). |

A file written without `schema_version` is loaded with an info log
message but without errors. After the first save the key is added (=
silent migration on the next save).

**Future migration:**

On an incompatible structural change (= e.g. renamed keys, changed
field semantics) the schema tag is bumped and a strategy is chosen
(= per-version branching in the loader or a migration script). Small
changes (= added fields, new enum values) keep the per-key BC fallback
mechanism and the tag stays unchanged - a missing field is interpreted
as the default and the saver fills it in after a rewrite.

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

| Key | Type | Default on load | Meaning |
|-----|------|-----------------|---------|
| `id` | int | - (mandatory; missing = skip with a warning) | Unique ID of the group. |
| `parent_id` | int | -1 | -1 = root, otherwise ID of the parent group. |
| `name` | string | `"Group <id>"` | Display name. Empty / NULL = auto. |
| `enabled` | bool | true | Cascade enable - if false, all BPs in this group are not evaluated. |
| `order` | double | 0.0 | Display order in the UI. |
| `color_bg` | int | `0x000000` | RGB color of the label background (0xRRGGBB). |
| `color_fg` | int | `0xFFFFFF` | RGB color of the label text. |

## Breakpoint object - common fields

These fields are stored for all BP types regardless of relevance.
Fields irrelevant for a given type are ignored at enforce time but
still serialized (= consistent schema).

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

### Identification and hierarchy

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `id` | int | - (mandatory) | Unique BP ID. Missing / invalid = skip. |
| `parent_id` | int | -1 | -1 = root, otherwise ID of the parent group. |
| `name` | string | `"Addr: 0x<addr>"` | Display name. |
| `auto_name` | bool | true | If true, the name is regenerated when the address changes. |
| `enabled` | bool | true | BP enabled flag. |
| `color_bg` | int | `0x000000` | RGB background. |
| `color_fg` | int | `0xFFFFFF` | RGB text. |
| `hits` | int (uint64) | 0 | Hit counter (display only, statistics persistence). |

### Type and supplementary fields

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `type` | string | `"PC_EXEC"` | BP type - see `types.md`. Unknown -> fallback to PC_EXEC with a warning. |
| `expr` | string \| null | null | Condition expression - see `expression-syntax.md`. |
| `action` | string \| null | null | Action mini-DSL - see `action-dsl.md`. NULL = stop. |
| `hit_count` | int (uint32) | 0 | Trigger only on the Nth hit (0 = every). |
| `skip_count` | int (uint32) | 0 | Skip the first N hits. |
| `edge_triggered` | bool | false | Edge tracking (relevant mainly for GLOBAL). |

## Per-type specific fields

### Address fields (PC_EXEC, MEM_R, MEM_W)

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `addr` | int (uint16) | 0 | Primary address. |
| `addr_end` | int (uint16) | = `addr` | RANGE upper. |
| `addr_match_mode` | string | `"SINGLE"` | SINGLE / RANGE / MASK. |
| `addr_mask` | int (uint16) | 65535 (0xFFFF) | AND mask for MASK. |
| `zone` | string | `"CPU_VIEW"` | Banking zone - see "Zone strings" below. |
| `bank_id` | int (uint8) | 0 | Bank index for the MMEXT_BANK zone. |
| `bank_id_end` | int (uint8) | 0 | RANGE upper for bank. |
| `bank_match_mode` | string | `"SINGLE"` | SINGLE / RANGE / MASK for bank. |
| `bank_id_mask` | int (uint8) | 255 (0xFF) | AND mask for bank. |
| `bp_addr_space` | string | `"cpu_view"` | Interpretation of `addr` for MEM_R / MEM_W in the MMEXT_BANK zone (PEHU only): `cpu_view` = `addr` is a Z80 address; `bank_offset` = `addr` is an offset `0x0000-0x1FFF` inside the bank. See `match-modes.md`. |

### IORQ fields (IORQ_R, IORQ_W)

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `port` | int (uint16) | 0 | Primary port. |
| `port_end` | int (uint16) | 0 | RANGE upper. |
| `port_match_mode` | string | `"SINGLE"` | SINGLE / RANGE / MASK. |
| `port_mask` | int (uint16) | 65535 (0xFFFF) | AND mask. |
| `port_mode` | string | `"8BIT"` | 8BIT / 16BIT. |

### HW_EVENT fields

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `event_name` | string \| null | null | Event name (e.g. `"vsync"`, `"raster:192"`). Stable persistence - see "Event strings". |
| `event_trigger` | string | `"rising"` | Trigger condition for signal events: low / high / rising / falling / changed. |

Note: the parsed event (enum) and event_param are not serialized -
they are caches derived from `event_name` on load.

### SP_THRESHOLD fields

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `sp_threshold` | int (uint16) | 0 | Threshold / lower bound. |
| `sp_upper` | int (uint16) | 0 | WINDOW upper bound. |
| `sp_mode` | string | `"SINGLE"` | SINGLE / WINDOW. |

### IRQ fields

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `im0_enabled` | bool | true | Fire on IM 0 dispatch. |
| `im1_enabled` | bool | true | Fire on IM 1 dispatch. |
| `im2_enabled` | bool | true | Fire on IM 2 dispatch. |
| `im0_rst_mask` | int (uint8) | 0 | IM 0 RST opcode bitmask (0 = match-all). Bit i = RST opcode 0xC7 + i*8. |
| `im2_vector_enabled` | bool | false | Filter on the IM 2 vector address. |
| `im2_vector_addr` | int (uint16) | 0 | Expected `(I << 8) \| (vec & 0xFE)`. |
| `im2_isr_enabled` | bool | false | Filter on the IM 2 ISR target. |
| `im2_isr_addr` | int (uint16) | 0 | Expected ISR address. |

### IRQ_SIG fields

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `irq_sig_sources` | array of string | `[]` | Bitmask sources as an array of stable names - see "IRQ_SIG source strings". Empty = mask 0 = invalid (UI validation). |

## Vars section

`$name` user variables - shared across all BPs, persist across the
session. Detail in `vars.md`.

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

| Key | Type | Default if missing | Meaning |
|-----|------|--------------------|---------|
| `name` | string | (mandatory) | Identifier without the `$` prefix. Empty = skip with a warning. Validated by the regex `^[a-zA-Z_][a-zA-Z0-9_]*$`. |
| `value` | int (int32) | `0` | Current value. **Emitted only if `persist_value=true`** (= save). On load + `persist_value=false` the value is ignored (warning). |
| `comment` | string | `null` | User comment (max 256 chars). Emitted only if non-NULL and non-empty. |
| `persist_value` | bool | `true` | Save the value? `true` = save + restore on load. `false` = save only name + comment, value is always 0 on load (counter pattern). |

**Per-key BC fallback** when loading older files:
- Missing `comment` -> `NULL`.
- Missing `persist_value` -> `true` (= legacy behavior, value preserved).

On load the storage is first cleared, then the `vars` array is
applied. The order in the file = the insertion order (linear storage,
not sorted).

Plus a per-arch standalone `.vars` file (`mz800.vars` / `mz1500.vars`
/ `mz700.vars`) has an identical schema (= subset of the `.bpt` JSON
with only the `"vars"` section). Auto-load on emu start (cfg
`[BP_VARS] auto_load=1`) overrides the vars loaded from `.bpt` (=
explicit per-arch separation takes precedence).

## Stable string names

### `type` strings

| String | Meaning |
|--------|---------|
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

Loader fallback on an unknown string = PC_EXEC + a warning to stderr.

### `zone` strings

| String | Meaning |
|--------|---------|
| `CPU_VIEW` | default, banking-agnostic |
| `ROM_LOWER` | Monitor ROM - MZ-800: 0x0000-0x1FFF; MZ-700/1500: 0x0000-0x0FFF |
| `ROM_UPPER` | upper ROM 0xE000-0xFFFF; on MZ-700/1500 includes the mapped ports 0xE000-0xE00F |
| `RAM` | RAM |
| `VRAM_FB` | VRAM framebuffer banking-aware window |
| `PCG` | MZ-800 in MZ-700 mode = CG-RAM; MZ-1500 = PCG bank 1/2/3; MZ-700: always false |
| `MMEXT_BANK` | memory expansion overlay bank - platform-neutral PEHU detect |

BC aliases (existing `.bpt` files are accepted without a warning):
- `PEHU_BANK` -> `MMEXT_BANK`
- `VRAM_RF` -> `VRAM_FB` (RF/WF registers are an MZ-800-only concept;
  "framebuffer" is platform-neutral)

### `addr_match_mode`, `port_match_mode`, `bank_match_mode`

| String | Meaning |
|--------|---------|
| `SINGLE` | single value |
| `RANGE` | range lower..upper |
| `MASK` | AND mask |

### `bp_addr_space`

Applies only to MEM_R / MEM_W in the MMEXT_BANK zone (PEHU). See
`match-modes.md`.

| String | Meaning |
|--------|---------|
| `cpu_view` | default - `addr` is a Z80 address (banking-aware) |
| `bank_offset` | `addr` is an offset `0x0000-0x1FFF` inside the PEHU bank |

Files without this key are loaded with the default `cpu_view`.

### `sp_mode`

| String | Meaning |
|--------|---------|
| `SINGLE` | simple threshold |
| `WINDOW` | lower + upper bound |

### `port_mode`

| String | Meaning |
|--------|---------|
| `8BIT` | match only the low byte of the port |
| `16BIT` | match the full 16-bit BC on `IN r,(C)` |

### `event_trigger`

| String | Meaning |
|--------|---------|
| `rising` | rising edge |
| `falling` | falling edge |
| `changed` | any change |
| `low` | level low |
| `high` | level high |

### `event_name` strings

28 events in 4 categories. Detail in `hw-events.md`. Brief overview:

**Signal events (17)** - have a trigger condition:

`vsync`, `hsync`, `vbln`, `hbln`, `ctc:zc0`, `ctc:zc1`, `ctc:zc2`,
`irq:ctc2`, `irq:pioz80_a`, `irq:pioz80_b`, `irq:fdc`, `tempo`,
`cursor`, `cmt:in`, `cmt:out`, `cmt:mstate`, `cmt:motor`

**Change events (5)** - implicit "happened":

`mode_change`, `palette_change`, `palgrp_change`, `border_change`,
`cksw_change`

**Point event with a parameter (1)**:

`raster:N` (N = 0..65535, typically 0..311 for the MZ-800 raster row)

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

An array of stable names for resilience against a possible reorder of
the enum bits (the bits themselves stay stable, but the string array
is more readable in the file).

## Complete BC alias table

The loader accepts legacy strings and transparently maps them to the
current names. The saver always writes the current name.

### Zone aliases

| Legacy string | Current | Reason |
|---------------|---------|--------|
| `PEHU_BANK` | `MMEXT_BANK` | rename (memory expansion overlay) |
| `VRAM_RF` | `VRAM_FB` | RF/WF is an MZ-800-only concept; "framebuffer" is platform-neutral |

### Event aliases

| Legacy string | Current | Reason |
|---------------|---------|--------|
| `pio:porta_int` | `irq:pioz80_a` | HWE rename (namespace unification) |
| `pio:portb_int` | `irq:pioz80_b` | ditto |
| `fdc:irq` | `irq:fdc` | ditto |
| `tape:edge` | `cmt:in` | HWE rename (CMT = cassette tape) |
| `nmi` | `cpu:nmi` | pre-HWE CPU prefix legacy |
| `di` | `cpu:di` | ditto |
| `im_change` | `cpu:im_change` | ditto |
| `iff_change` | `cpu:iff_change` | ditto |
| `halt` | `cpu:halt` | ditto |
| `reset` | `cpu:reset` | ditto |

## Retired persistence strings

These event strings were **retired** as part of the HWE redesign
(reasons: redundancy with other types, bad granularity, unimplemented
hook sites). The loader responds to them with an "unknown event_name"
warning to stderr and the BP loads with event = NONE - **never fires**.

| Retired string | Reason |
|----------------|--------|
| `psg:int_pa5` | replaced by `irq:pioz80_b` (the PSG INT signal is on PIOZ80 port B) |
| `psg:reg_write` | the granularity of a PSG register write is not stable (= per-emu interpretation) |
| `ppi:pa_write` | replaced by IORQ_W on the specific port |
| `ppi:pc_change` | ditto |
| `fdc:command` | unstable (= per-WD279x revision interpretation) |
| `fdc:drq` | granularity - usually not needed |
| `ctc:gate0_edge` | the gate signal is not represented cycle-accurately in the emu |
| `tape:read_byte` | byte-level granularity; for the signal use `cmt:in` |
| `tape:write_byte` | ditto for `cmt:out` |
| `mmio:bank_switch` | replaced by IORQ_W on the banking ports |
| `mmio:mode_change` | replaced by `mode_change` (HW_EVENT change category) |

Recommendation for migration: if a `.bpt` contains a retired string,
manually reconfigure the BP to an alternative type (typically IORQ_W
on a specific port or an `irq:*` event).

## Per-type JSON examples

The examples are based on the save format (= they contain all common
fields). Fields irrelevant for a given type are at default - the
loader tolerates them and enforce ignores them.

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

PC_EXEC with a condition - stops at 0x1000 only if A == 0x42.

### MEM_R with RANGE

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

Watch reads of the VRAM area 0x8000-0x80FF (= a 256 byte range).

### MEM_W with MASK

```json
{
  "id": 3, "parent_id": -1, "type": "MEM_W",
  "addr": 53248, "addr_end": 53248,
  "addr_match_mode": "MASK", "addr_mask": 65520,
  "zone": "CPU_VIEW",
  "...": "(other defaults)"
}
```

`addr_mask = 0xFFF0` - watch writes to 0xD000..0xD00F (= upper 12 bits
match).

### IORQ_R with 16BIT port

```json
{
  "id": 4, "type": "IORQ_R",
  "port": 17102, "port_end": 0,
  "port_match_mode": "SINGLE", "port_mask": 65535,
  "port_mode": "16BIT",
  "...": "(other defaults)"
}
```

Matches only `IN r,(C)` with `BC = 0x42CE`. In `port_mode: 8BIT`
(default) it would match any IN for the low byte 0xCE.

### IORQ_W with RANGE

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

Traces writes to the GDG palette 0xF0-0xF3.

### IRQ with IM 2 vector filter

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

Fires only on IM 2 dispatch with vector `(I << 8) | (vec & 0xFE) =
0x6900`.

### IRQ with IM 0 RST 38h filter

```json
{
  "id": 7, "type": "IRQ",
  "im0_enabled": true, "im1_enabled": false, "im2_enabled": false,
  "im0_rst_mask": 128,
  "im2_vector_enabled": false, "im2_isr_enabled": false,
  "...": "(other defaults)"
}
```

`im0_rst_mask = 0x80` (bit 7) = only RST 38h opcode (0xFF). Detects a
runaway NULL pointer call.

### HW_EVENT signal with trigger

```json
{
  "id": 8, "type": "HW_EVENT",
  "event_name": "vsync", "event_trigger": "rising",
  "expr": null, "action": null,
  "...": "(other defaults)"
}
```

Frame stop on the rising edge of the vsync signal.

### HW_EVENT raster:N

```json
{
  "id": 9, "type": "HW_EVENT",
  "event_name": "raster:192", "event_trigger": "rising",
  "...": "(other defaults)"
}
```

Trigger on raster row 192. `event_param` is not stored separately -
it is in the suffix of `event_name`.

### SP_THRESHOLD WINDOW

```json
{
  "id": 10, "type": "SP_THRESHOLD",
  "sp_threshold": 61440, "sp_upper": 65535,
  "sp_mode": "WINDOW",
  "...": "(other defaults)"
}
```

Fires if SP leaves the window 0xF000..0xFFFF (= cross-task switch
detect).

### GLOBAL with edge

```json
{
  "id": 11, "type": "GLOBAL",
  "expr": "HL == 0x8000 && A == 0x42",
  "edge_triggered": true,
  "action": "log \"hit at PC=%X\", PC",
  "...": "(other defaults)"
}
```

Per-instruction trigger on an exact CPU state, edge-triggered (= only
on the false -> true transition).

### IRQ_SIG multi-source

```json
{
  "id": 12, "type": "IRQ_SIG",
  "irq_sig_sources": ["FDC", "CTC2"],
  "...": "(other defaults)"
}
```

Fires if the raise edge of the INT line includes FDC or CTC2 (OR
semantics).

## Load validation

After deserializing the JSON the loader runs two validation routines:

### Group parent validation

Protects against incorrectly edited `.bpt` files (= cycles or dangling
parent_id in the group hierarchy):

- **Dangling group parent** (parent points to a non-existent group):
  warning to stderr + reparent to root.
- **Self-loop** (group parent points to itself): warning + reparent
  to root.
- **Closed ring across multiple groups**: parent chain scan with a
  depth limit (max 32 levels). Two detection signals: return to the
  current group ID or exceeding the depth. Warning + reparent to root.
- **Dangling BP parent** (BP parent points to a non-existent group):
  warning + reparent to root.

Without this validation a manually crafted cyclic `.bpt` (= group A's
parent = B, B's parent = A) would cause a stack overflow on the first
BP enforce.

### Event address validation

Detects duplicated effectively enabled events on the same address (=
two BPs with the same `addr` and both `enabled` + cascade enabled).
The later BP is disabled, warning to stdout.

### Belt-and-suspenders runtime guard

Even if load validation fails, the runtime must not crash. The cascade
enable check has a depth limit (max 32); after it is exceeded it
returns `true` (= treat as enabled, a fallback safe for the emu - the
BP simply fires) + a warning to stderr.

## Versioning policy

- The numeric format version is not incremented on every added field -
  only on a schema that **breaks** backward compatibility (= rename of
  an existing key, change of value type, removal of a mandatory key).
- Adding a new key with a reasonable default = **non-breaking**, with
  a per-key BC fallback in the loader (default on missing).
- Removing a key = ignore on load, do not save it (= the file
  gradually "cleans itself up").

Loader strategy:

- File version lower than current = best-effort, use defaults for new
  keys.
- File version higher than current = best-effort, ignore unknown
  keys.
- No hard fail - always logs "proceeding best-effort" and continues.

Recommendations for the future:

- If a breaking change is needed (e.g. struct refactoring), bump the
  version and add an explicit migrator (= a per-version load path) or
  use a converter utility.
- Alias period - keep a legacy string for at least 2 versions (= 1
  release for detection, 1 release for migrating user files).
- Retired events ideally should have an explicit "deprecated" warning
  in the loader (currently they only get "unknown event_name" - see
  "Retired persistence strings").

## Related documents

- `README.md` - subsystem orientation
- `types.md` - catalogue of 9 BP types
- `match-modes.md` - SINGLE / RANGE / MASK detail
- `expression-syntax.md` - condition expression
- `action-dsl.md` - action DSL
- `hw-events.md` - HW event vocabulary detail
- `irq-filter.md` - IRQ post-dispatch filter detail
- `irq-sig.md` - IRQ_SIG pre-dispatch filter detail
- `vars.md` - $vars user variables
