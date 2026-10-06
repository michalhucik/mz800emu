# MCP Resources - přehled

MCP Resources jsou read-only `emulator://...` URI endpointy, které AI
klient čte přes `resources/read` (= bez nutnosti volat Tool). Resources
mají oproti Tools dvě výhody:

1. **Levnější dotaz** - klient nemusí pojmenovávat akci, jen čte URI.
2. **Cacheable** - MCP klient si může výsledek držet (TTL podle obsahu).

## Seznam Resources

### Stav a CPU

| URI | Obsah |
|-----|-------|
| `emulator://state` | Lightweight stav (running/paused + last_user_action) |
| `emulator://cpu/registers` | Plný Z80 register snapshot |
| `emulator://cpu/im2_vector` | IM2 vector snapshot |
| `emulator://cpu/interrupt_bus` | IRQ subsystem snapshot |
| `emulator://memory/{addr_hex}/{length}` | Template - čtení paměti |
| `emulator://memory/map` | Per-platform banking (16 x 4 KB slotů) |

### Debugger

| URI | Obsah |
|-----|-------|
| `emulator://breakpoints` | Aktivní BP s id/addr/type/hit |
| `emulator://callstack` | Single-shadow callstack snapshot |
| `emulator://profiler` | Per-funkce profile data (excl/incl cycles) |
| `emulator://symbols` | Symbol table výpis (cap 10 000, truncated flag) |
| `emulator://stack` | 32 LE slov od aktuálního SP směrem nahoru |
| `emulator://stack/history` | SP ring buffer (4096 vzorků) + slope |
| `emulator://stack/regions` | Definované stack regions + watermarks |
| `emulator://watch` | Aktivní watch řádky (cap 256, mode/type/value) |
| `emulator://watch/snapshot/{name}` | Template - per-watch statistiky (snap/current/delta/min/max) |
| `emulator://vars` | Smart BP proměnné (`$name`) |
| `emulator://bookmarks` | Bookmarks (id/input/resolved addr/owner) |

### Konfigurace a platforma

| URI | Obsah |
|-----|-------|
| `emulator://platform/info` | Platform + mode + TV systém + framerate + clocks + scanline (dynamic) |
| `emulator://config/mcp` | MCP server INI hodnoty |
| `emulator://config/settings` | Live emulátorová konfigurace (audio, video) |
| `emulator://config/peripherals` | Per-chip detail (stub, postupně rozšiřováno) |
| `emulator://cooperation/policy` | Cooperation hint state |
| `emulator://security/profile` | MCP security profile + capabilities |
| `emulator://memext/info` | Memory expansion adapter info |
| `emulator://media/state` | CMT/FDC/QD/IDE8 mount info |
| `emulator://speed` | Emulační rychlost (current_percent, max_speed, mode, status) |

### Periferie

| URI | Obsah |
|-----|-------|
| `emulator://periph/i8255` | Intel 8255 PPI - klávesnice + CMT + PSG audio gate |
| `emulator://periph/i8253` | Intel 8253 CTC - 3 časovače (per-kanál mode/state/counter) |
| `emulator://periph/z80_pio` | Zilog Z80 PIO - joystick + parallel + IM2 daisy chain (jen MZ-800 a MZ-1500) |
| `emulator://periph/sn76489` | SN76489 PSG - mono / stereo snapshot (MZ-700 vrací available=false) |
| `emulator://periph/ay3_8910` | AY-3-8910 PSG - placeholder (vždy available=false) |
| `emulator://periph/beeper` | Audio cesta CTC0 OUT přes GATE0 + PC0 (raw bity + dopočet audible) |
| `emulator://periph/gdg` | GDG video LSI - paleta, raster state, regDMD |
| `emulator://periph/wd1793` | WD279x FDC chip registry + 4 drive mount metadata |
| `emulator://periph/cmt` | CMT cassette - stop/play/record state, motor, polarita, image basename, `cmthack_enabled` (ROM patch instant load) |
| `emulator://periph/cmt/tape` | Výpis bloků naložené pásky - container_type (SINGLE/SIMPLE_TAPE), current_block, per-blok id/name/cmt_speed/type/playable/recordable |
| `emulator://periph/qd` | Quick Disk - state machine, image basename |

### Vstupy a video

| URI | Obsah |
|-----|-------|
| `emulator://input/keyboard/state` | Real HW + virtual injection matrix [10] + effective merge + pressed_keys |
| `emulator://input/keyboard/matrix_info` | Per-platform col x bit -> key_name tabulka |
| `emulator://input/joystick/state` | Per-port (0, 1) digital state bits + device name |
| `emulator://frame/framebuffer/info` | width/height/last_render_frame/dirty + 16-entry palette |
| `emulator://frame/screenshot.raw` | Base64 RGBA pixely (auto-downscale 1/2/4) + `fallback_source` |
| `emulator://frame/screenshot` | PNG base64 (`format: "png"`, plný frame, enkodér stb_image_write) |
| `emulator://video/text_dump` | MZ-700 mode 40x25 text dump (MZ-800 jen v 700 compat módu) |

### Dokumentace pro AI klienty

Statické EN reference markdown soubory pro pokročilé AI scénáře.
Obsah je v `docs/agent/` v repozitáři, distribuován s emulátorem.
Soubory se registrují **automaticky** (každý `*.md` kromě `README.md`
-> `emulator://docs/<jméno>`), takže přidání nového dokumentu nevyžaduje
úpravu kódu. Discovery (`resources/list`) je instantní (= čistě
file-based, žádný transport call).

| URI | Obsah |
|-----|-------|
| `emulator://docs/index` | Rozcestník - co kdy číst |
| `emulator://docs/memory_layout` | Per-platform Z80 paměťové mapy, banking 0xE0..0xE6, ROM entry pointy, IRQ vektory |
| `emulator://docs/bp_dsl` | BP condition expression syntax (registry, mem[], port[], operators, built-in funkce, $vars) |
| `emulator://docs/smart_vars` | `$name` user variables - action DSL, persistence, lifecycle |
| `emulator://docs/action_dsl` | BP Action DSL grammar - 11 příkazů (`log`, `set <reg>`, `poke`, `mark`, `$var` zápisy, `if/then/else`, `enable`/`disable`, `clear_vars`) + forwarding příkazy (`cdl_*`/`trace_*`/`snapshot`), `log` format specs, stop vs continue semantika |
| `emulator://docs/watch_dsl` | Watch expression syntax (`address` / `expr_scalar` / `expr_deref` + type tagy) |
| `emulator://docs/eventlog_mask` | EventLog 64-bit category mask - bit assignments + recipes |
| `emulator://docs/sharp_display_code` | Sharp MZ ASCII vs display code vs std ASCII (decode `video/text_dump` + `mzascii` watch) |
| `emulator://docs/mz800_keyboard` | Klávesnice MZ-800 - layout, matice, modifikátory, znakové vrstvy, jména kláves pro `emu_input_send_keys`, recepty na speciální znaky (MZ-700/1500 jako diff) |
| `emulator://docs/cmt_workflow` | CMT workflow - reálná páska vs cmthack, transport, WAV/LEP/L16 recording, rychlostní poměry, SIMPLE_TAPE multi-blok seek + per-blok speed |

AI klient by měl číst `emulator://docs/index` jako první - poskytne
přehled co se kde nachází a kdy to číst.

### Uživatelská knowledge base (`emulator://kb/*`)

Uživatel může vystavit vlastní Markdown poznámky bez zásahu do
repozitáře - nastavením složky přes env `MZ800EMU_USER_KB_DIR`
(typicky v `env` bloku `.mcp.json`). Soubory se prohledají rekurzivně a
zaregistrují jako `emulator://kb/<relativní cesta bez .md>` (oddělený
namespace od vestavěného `emulator://docs/*`). Detaily viz
[configuration.md](configuration.md) sekce "Uživatelská knowledge base".

## `emulator://platform/info` detail

Klíčový Resource pro AI klienta po MCP připojení - obsahuje **vše**
co klient potřebuje k pochopení emulovaného HW (platforma, TV systém,
přesné clocks, kompletní scanline geometrie).

### Top-level fields

| Field | Hodnota | Popis |
|-------|---------|-------|
| `platform` | `mz700` / `mz800` / `mz1500` | Compile-time binárka (= MZARCH_NAME; PAL/NTSC nerozlišuje - viz `tv_system`) |
| `full_name` | "MZ-700 (PAL)" / "MZ-800" / "MZ-1500" / atd. | Human-readable |
| `mode` | `native` / `compat700` | Runtime z GDG regDMD bit (MZ-700 vždy native) |
| `tv_system` | `PAL` / `NTSC` | Compile-time z MZTVSYS makra |
| `framerate_hz` | 50 / 60 | Vyplývá z TV systému |
| `pxclk_hz` | např. 14336640 / 17760000 | Simulovaný GDG base = `screen_ticks * screens_per_sec` |
| `mzarch` | shodný s `platform` | Legacy alias (= zachováno pro zpětnou kompatibilitu) |
| `mzarch_numeric` | 700 / 800 / 1500 | Číselná varianta platformy |
| `rom_version` | `unknown` | Plánováno pro budoucí verzi (= z ROM header signatury) |

### `capabilities` sub-object - compile-time HW podpora

Co je zakompilováno v binárce (= co lze hot-swapem připojit), ne co je
aktuálně runtime attached. Pro runtime stav viz `emulator://media/state`,
`emulator://config/peripherals`, `emulator://memext/info`.

| Field | Typ | Popis |
|-------|-----|-------|
| `has_pioz80` | bool | Z80 PIO chip (= MZ-1500 ano, jinde ne) |
| `psg_count` | int | Počet SN76489 PSG kanálů (= 0 pro MZ-700, 1 pro MZ-800, 2 pro MZ-1500) |
| `hwext_fdc` | bool | WD279x FDC slot |
| `hwext_ide8` | bool | 8-bit IDE adapter |
| `hwext_ramdisk` | bool | RAM disk extension |
| `hwext_qdisk` | bool | Quick Disk slot |
| `cpu_model` | string | "Z80" (= prakticky pro všechny varianty) |

### `clocks` sub-object - přesné CPU + chip frekvence

| Field | Popis | Typický rozsah |
|-------|-------|---------------|
| `gdg_base_hz` | Simulovaný GDG base | 14,336,640 (NTSC) / 17,760,000 (PAL) |
| `gdg_real_base_hz` | Reálná krystalová frekvence | 14,318,180 / 17,734,475 |
| `cpu_hz` | Z80 efektivní clock | 3,584,160 (NTSC) / 3,552,000 (PAL) |
| `cpu_divider` | GDGCLK2CPU_DIVIDER | 4 (NTSC) / 5 (PAL) |
| `ctc0_input_hz` | 8253 counter 0 input | 1,102,818 (700-NTSC, div=13) / 1,110,000 (PAL+MZ-800+MZ-1500, div=16) |
| `ctc0_divider` | GDGCLK_CTC0_DIVIDER | 13 (700-NTSC) / 16 (jinde) |
| `psg_input_hz` | SN76489 PSG input | cpu_hz / 16 |
| `psg_divider` | PSG_DIVIDER = 16 * cpu_divider | 64 (NTSC) / 80 (PAL) |
| `ctc1_input_hz` | **null** | 8253 counter 1 je cascade-driven |
| `ctc2_input_hz` | **null** | 8253 counter 2 je cascade-driven |
| `ctc12_note` | "read emulator://periph/i8253 for runtime state" | Per-platform routing |

**Pozn k MZ-1500 vs MZ-700 NTSC**: oba mají krystal 14.318 MHz, ale
různé CTC0 deličky (16 vs 13) → různé BASIC tempo + zvuk tóny.
`ctc0_divider` field tohle explicit rozliší.

### `scanline` sub-object - raster geometrie

| Field | Popis | Typický rozsah |
|-------|-------|---------------|
| `screen_total_width_ticks` | Celkový raster line (= sync + back + display + front porch) | 912 (NTSC) / 1056 (PAL) |
| `screen_total_height_lines` | Celkový počet scan lines | 262 (NTSC) / 312 (PAL) |
| `screen_total_ticks_per_frame` | width × height | 238944 / 329472 |
| `screens_per_sec` | = framerate_hz | 60 / 50 |
| `display_width` | Viditelný display (= canvas + border) | např. 704 (MZ-700 PAL) |
| `display_height` | Viditelný display | např. 232 |
| `canvas_width` | Kreslicí plocha bez border | 640 |
| `canvas_height` | Kreslicí plocha bez border | 200 |
| `border_left_width` / `border_right_width` | Border po stranách | 32 |
| `border_top_height` / `border_bottom_height` | Border nahoře/dole | 16 |
| `h_sync_ticks` | HSync interval | 80 (PAL) / 65 (NTSC) |
| `h_back_porch_ticks` | Back porch po HSync | 136 (PAL) / 104 (NTSC) |
| `h_front_porch_ticks` | Front porch před HSync | 216 (PAL) / 39 (NTSC) |

### Subset v hello capabilities

Po MCP `initialize` handshake klient hned dostane top-level fields
(`platform`, `full_name`, `mode`, `tv_system`, `framerate_hz`,
`pxclk_hz`) v `result.capabilities`. Plný `clocks` + `scanline`
detail je dostupný přes `resources/read` na `emulator://platform/info`.

### Příklad response (MZ-800 PAL v native módu)

```json
{
  "platform": "mz800",
  "full_name": "MZ-800",
  "mode": "native",
  "tv_system": "PAL",
  "framerate_hz": 50,
  "pxclk_hz": 17760000,
  "mzarch": "mz800",
  "mzarch_numeric": 800,
  "rom_version": "unknown",
  "capabilities": {
    "has_pioz80": false,
    "psg_count": 1,
    "hwext_fdc": true,
    "hwext_ide8": true,
    "hwext_ramdisk": true,
    "hwext_qdisk": true,
    "cpu_model": "Z80"
  },
  "clocks": {
    "gdg_base_hz": 17760000,
    "gdg_real_base_hz": 17734475,
    "cpu_hz": 3552000,
    "cpu_divider": 5,
    "ctc0_input_hz": 1110000,
    "ctc0_divider": 16,
    "psg_input_hz": 222000,
    "psg_divider": 80,
    "ctc1_input_hz": null,
    "ctc2_input_hz": null,
    "ctc12_note": "cascade from CTC0 / per-platform routing; read emulator://periph/i8253 for runtime state"
  },
  "scanline": {
    "screen_total_width_ticks": 1056,
    "screen_total_height_lines": 312,
    "screen_total_ticks_per_frame": 329472,
    "screens_per_sec": 50,
    "display_width": 704,
    "display_height": 232,
    "canvas_width": 640,
    "canvas_height": 200,
    "border_left_width": 32,
    "border_right_width": 32,
    "border_top_height": 16,
    "border_bottom_height": 16,
    "h_sync_ticks": 80,
    "h_back_porch_ticks": 106,
    "h_front_porch_ticks": 22
  }
}
```

## Příklady použití

### Python wrapper (FastMCP)

```python
# Claude Desktop / Code MCP klient automaticky discoveruje Resources
# přes resources/list. Pro programatický přístup:

from mcp.client.session import ClientSession

async with ClientSession(...) as session:
    resources = await session.list_resources()
    for r in resources:
        print(r.uri, r.name)

    # Read jednoho Resource:
    content = await session.read_resource("emulator://cpu/im2_vector")
    # content.contents[0].text = JSON string
```

### Přímé JSONL volání (debug)

Pokud máte JSONL terminál připojený na MCP backend, můžete volat
podkladové `get_*` příkazy přímo:

```jsonl
{"type":"request","id":1,"cmd":"get_cpu_im2_vector"}
```

Odpověď:

```json
{"type":"response","id":1,"success":true,
 "data":{"im":2,"i":64,"vec":128,"available":true,
         "isr_addr":16512,"isr_target":4660}}
```

### Příklad: per-watch snapshot

Resource `emulator://watch/snapshot/{name}` vrací statistiky
pojmenovaného watch řádku:

```json
{"name": "PLAYER_HP", "found": true, "row_id": 17, "type": "u16le",
 "snapshot_active": true, "min_max_valid": true,
 "snap_int": 4096, "cur_int": 4660, "delta_int": 564,
 "min_int": 256, "max_int": 4660, "change_count": 42}
```

Pokud watch s daným jménem neexistuje:

```json
{"name": "MISSING", "found": false}
```

Anonymní watch řádky (= bez `name`) nejsou přes URI adresovatelné;
pro celý seznam slouží `emulator://watch`.

## Omezení

- **Žádné subscribe** - klient čte pull-only; `notifications/resources/
  updated` zatím není k dispozici.
- **Symbols cap 10 000** - nad cap klient padá zpět na `symbol_list`
  Tool s prefix filterem.
- **Stack history pull-only** - při vypnutém recordingu Resource vrátí
  prázdné `samples`.
- **Callstack scope_state** - aktuální single-shadow callstack nemá
  per-scope BP; Resource vystavuje jen aggregate `active` + statistiky,
  ne scope_bp_id.
- **Watch bez owner pole** - `emulator://watch` neobsahuje per-row
  origin; po reloadu se watch storage `.watch` vrací na USER.
- **Stack pevné okno** - `emulator://stack` vrací 32 slov od SP. Pro
  jiné délky / lines_above je nutné použít plnou STACK_DUMP cestu
  přes Tool.
- **Bookmarks snapshot lifetime** - vrácená data jsou platná jen po
  dobu dispatch volání; klient si je musí zkopírovat sám.
- **i8255 Mode Set shadow** - `emulator://periph/i8255` dekóduje
  mode/dir fields jen pokud bit 7 mirror Control Word bytu je 1. Pro
  Bit Set/Reset (= bit 7 = 0) vrací `cw_decoded=false`.
- **Z80 PIO platform check** - na MZ-700 vrací
  `{"available": false, "reason": "platform has no Z80 PIO"}`.
- **Z80 PIO control sequencer** - `last_ctrl_byte` per port je jen
  poslední CPU write; dekódování Mode Set / IOMCW / ICW / IDW není.
- **i8253/i8255 last_cw_byte** - 8253 a 8255 hardware Control Word
  nelze přečíst z chipu (write-only sequencer); mirror v emu je
  accurate pro posledně zapsaný byte, ale per-bit dekódování není
  pro CTC CW poskytnuto (klient si dekóduje SC/RL/M/BCD sám).
- **PSG MZ-700 availability** - `emulator://periph/sn76489` na MZ-700
  vrací `{"available": false, "reason": "platform has no PSG"}`.
- **PSG mono vs stereo** - MZ-800 bootuje mono (`psg_count=1`);
  runtime přepnutí na stereo (přes I/O port) změní `psg_count=2`.
  Klient nesmí cachovat `psg_count` napříč dlouhými intervaly.
- **PSG write-only HW** - SN76489 nemá CPU read; Resource je čistý
  side-effect free snapshot interních emulator registrů.
- **AY-3-8910 neimplementován** - vždy
  `{"available": false, "reason": "AY-3-8910 not implemented in this
  emulator"}`. Resource je v API jen pro forward compat.
- **Beeper není 1-bit chip** - Sharp MZ nemá dedikovaný 1-bit
  reproduktor; "beeper" je audio cesta CTC0 OUT přes hradla GATE0
  a PC0 (`audible = ctc0_out AND gate0 AND pc0`). Pro audio frekvenci
  čtěte `emulator://periph/i8253` channel 0 `preset_value`.
- **Beeper GATE0 per platforma** - MZ-800 v 800 módu (DMD3=0) má
  GATE0 hardwarově trvale 1; ostatní platformy řídí GATE0 přes
  zápis na 0xE008 bit 0. Resource vrací raw bit napříč platformami;
  klient interpretuje sám.
- **GDG per-platforma palette layout** - `emulator://periph/gdg` má
  discriminator `platform` field. MZ-800 má 16-color palette přes
  PAL_GRP + PAL0..3; MZ-700 / MZ-1500 mají 8-entry mode color tabulku.
  `palette_count` říká kolik entries je platných (16 nebo 8).
- **GDG VRAM dump není dostupný** - Resource vrací jen registry +
  raster state. Pro VRAM obsah použijte `mem_read` Tool s
  banking-aware adresami.
- **FDC + QD runtime availability** - vrátí
  `{"available": false, "reason": "FDC not compiled or detached"}`
  (resp. QDisk) pokud chip není v buildu nebo runtime detached.
  Default build všech tří platforem má oba zapnuté.
- **Image paths jen jako basename** - FDC / CMT / QD `image_basename`
  vrací jen filename (bez adresářové cesty) - bezpečnostní omezení.
- **CMT bez sample stream** - Resource vrátí state + image basename
  + motor flag, ne tape signal samples.
- **QD VIRTUAL files listing chybí** - `virt_files_count` říká kolik
  MZF je v aktuálním VIRTUAL state, ale ne jejich jména.
- **Keyboard pressed_keys cap 32** - dekódované pole `pressed_keys`
  pojme max 32 současně držených pozic; navíc `pressed_truncated=true`.
  Raw 10-bytový `effective` matrix je vždy úplný.
- **Joystick bez analog os** - `emulator://input/joystick/state` je
  jen digitální. Emulátor nemá analog axis ani deadzone konfiguraci.
- **Framebuffer paleta 16 entries** - `DISPLAY_MZCOLORS = 16`. Pixel
  byty jsou maskovány `0x0F` při INDEX8 -> RGBA expand; horní 4 bity
  pixel byte jsou rezervované pro interní emulator use (border path,
  blink).
- **screenshot PNG** - `emulator://frame/screenshot` vrací plný frame
  jako PNG base64 (`format: "png"`, `data_b64`, `byte_size`). Enkód
  dělá vendorovaný `stb_image_write.h` (public domain) na emu vlákně =
  žádná runtime DLL závislost. Obsah je shodný se `screenshot.raw` při
  downscale 1, jen PNG kontejner. `available=false` jen když frame ještě
  nebyl vyrenderován / display neinicializován / enkód selhal.
- **screenshot.raw response size** - native MZ-800 framebuffer je
  928 x 288 x 4 = 1.07 MB raw, ~1.43 MB base64. Pokud transport
  vynutí menší per-message budget, backend auto-bumpne
  `downscale_factor` na 2 nebo 4 a reportuje effective factor.
  Klient nesmí předpokládat fixed scaling.
- **screenshot.raw `fallback_source` field** - response
  obsahuje string pole `fallback_source` s hodnotou `"sdl_snapshot"`
  nebo `"gdg_live"`. Primární zdroj je SDL display-ready snapshot
  (`g_iface_video->fbsnapshot_pixels`, naplňovaný emu vláknem per
  frame). Pokud je NULL (= headless mode bez SDL render threadu,
  nebo race condition v GUI mode mezi publish a consume), handler
  fallbackuje na GDG live buffer (`g_framebuffer.pixels`, statická
  BSS alokace, vždy dostupná v emu thread context). Obsah obou
  zdrojů je byte-identický po dokončeném frame; `gdg_live` může
  zachytit i partial frame uprostřed rasteru. Klient může pole
  použít pro diagnostiku (= nečekaný `gdg_live` v GUI mode indikuje
  vysoký dispatch latency).
- **Python klient příklad pro screenshot.raw → PIL** - viz
  `docs/cz/mcp-server/headless-mode.md` sekce "Screenshot v headless
  mode".
- **text_dump platformy** - čte 40x25 MZ-700 text layout (D000-D3FF
  chars, D800-DBFF attributes). MZ-800 vrací `available=false` když
  emulátor běží v 800 graphics módu (= je dostupný jen v 700 compat
  módu). MZ-1500 vždy available (sdílí 700-compat layout).
- **text_dump Sharp ASCII nekonvertován** - `chars_b64` nese raw
  8-bit Sharp ASCII byte stream. Klienti aplikují Sharp ASCII -> UTF-8
  mapping sami.
- **keyboard/matrix_info aliasy** - tabulka symbolických jmen
  obsahuje aliasy (RETURN/ENTER/CR; CTRL/CONTROL; ARROW_UP/UP/...).
  Stejný (col, bit) pár se objevuje vícekrát pod různými `name`
  hodnotami; klienti filtrující unique pozice musí dedupovat podle
  (col, bit).

## Security note

Resource `emulator://config/settings` respektuje MCP security profile:
v módu **observer** vrátí `filtered=true` + prázdný `sections` objekt
(= AI nesmí ani číst whitelistované klíče).

`emulator://security/profile` je vždy plně čitelný (= AI musí mít
možnost zjistit svá omezení).
