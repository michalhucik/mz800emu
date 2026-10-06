# Přehled MCP tools

MCP server vystavuje sadu tools (= callable RPC) pro řízení emulátoru.
Tato stránka je přehledový rozcestník - každý tool je popsán krátce,
s důrazem na co dělá, jaké má argumenty a jestli je destruktivní.
Resources (= read-only URI endpointy) jsou popsané v samostatném
dokumentu [Resources overview](resources-overview.md).

## Seznam tools

| Tool | Sensitive? | Popis |
|------|-----------|-------|
| `emu_status` | ne | Stav emu (running / paused, connected) |
| `emu_ping` | ne | Liveness check (`pong`) |
| `emu_pause` | ne | Pauzne emulaci |
| `emu_run` | ne | Blokující resume na N framů (1..1000) |
| `emu_reset` | ne | Power-on reset CPU + periferií |
| `emu_get_registers` | ne | 14 Z80 registrů (AF..IR) |
| `emu_set_register` | **ANO** | Zápis 16-bit hodnoty do registru - **destruktivní** |
| `emu_dasm` | ne | Disassembluje N instrukcí od adresy (banking-aware) |
| `emu_history_get` | ne | 32 posledních provedených instrukcí (ring buffer) |
| `emu_mem_read` | ne | Čte bajty z Z80 paměti (base64) |
| `emu_mem_write` | **ANO** | Zápis bajtů do RAM - region check, **destruktivní** |
| `emu_bp_add` | ne | Přidá breakpoint (exec, nebo typed memw/memr/ior/iow + condition) |
| `emu_bp_list` | ne | Vypíše breakpointy (id/addr/addr_end/addr_match_mode/addr_mask/enabled + type/zone/bank_id/bank_id_end/bank_match_mode/bank_id_mask/hits/condition) |
| `emu_bp_remove` | ne | Odebere konkrétní BP podle ID |
| `emu_bp_clear` | ne | Smaže všechny breakpointy najednou |
| `emu_bp_enable` | ne | Toggle BP enabled flagu (bez mazání) |
| `emu_step_into` | ne | 1 instrukce krok (step INTO CALL/RST) |
| `emu_step_over` | ne | 1 instrukce, CALL/RST jako atom |
| `emu_step_n` | ne | N kroků jako sekvence step_into (1..1000) |
| `emu_run_until_addr` | ne | Run dokud PC == addr (s temp BP) |
| `emu_snapshot_save` | **ANO** | Uloží .mzs snapshot na disk (přepíše soubor) |
| `emu_snapshot_save_buffer` | ne | Uloží snapshot jako inline base64 (bez disk I/O) |
| `emu_snapshot_load` | **ANO** | Načte .mzs ze souboru, **přepíše stav emu** |
| `emu_snapshot_load_buffer` | **ANO** | Načte snapshot z inline base64, přepíše stav |
| `emu_cooperation_hint_set` | ne | Self-binding hint AI klienta (read_only / paused_only / free) |
| `emu_symbol_add` | ne | Přidá user-defined symbol (named address) |
| `emu_symbol_remove` | ne | Odebere symbol (by name nebo by addr) |
| `emu_symbol_lookup` | ne | Vyhledá symbol by name nebo by hex address (read-only) |
| `emu_symbol_list` | ne | Vypíše symbols matching name prefix (max 1000) |
| `emu_bookmark_add` | ne | Přidá pojmenovanou adresovou záložku (hex literál nebo symbol), owner mcp |
| `emu_bookmark_remove` | ne | Odebere záložku podle ID |
| `emu_step_out` | ne | Run dokud RET z aktuální subroutine (callstack pop) |
| `emu_run_until_raster` | ne | Run do GDG raster pozice (line + optional col) |
| `emu_run_until_tstate` | ne | Run do absolutního Z80 cycle target |
| `emu_run_until_event` | ne | Run dokud event (`frame_done`, `breakpoint_hit`) |
| `emu_event_subscribe` | ne | Subscribe na seznam topics (`breakpoint_hit`, `paused`, `step_done`, `io_write`) |
| `emu_event_unsubscribe` | ne | Unsubscribe topics (empty list = vše) |
| `emu_event_poll` | ne | Vyzvedne pending eventy (timeout 0..60000 ms, max 1..100) |
| `emu_trap_respond` | ne | Odpověz na TRAP z `breakpoint_hit` eventu (continue/step_into/step_over/abort) |
| `emu_io_read` | **ANO** | Z80 IN port - reálná cesta s side-effecty, **destruktivní** |
| `emu_io_write` | **ANO** | Z80 OUT port - reálná cesta s side-effecty, **destruktivní** |
| `emu_irq_inject` | **ANO** | Force maskable IRQ (volitelný IM2 vector), **destruktivní** |
| `emu_nmi_inject` | **ANO** | Force NMI - skok na 0x0066, **destruktivní** |
| `emu_mem_write_force` | **ANO** | Zápis bez region checku (obchází ROM ochranu), **destruktivní** |
| `emu_watch_add` | ne | Přidá watch řádek (address / expr_scalar / expr_deref) |
| `emu_watch_remove` | ne | Odebere watch řádek (by name nebo index) |
| `emu_watch_list` | ne | Vrátí všechny watche s aktuálními hodnotami |
| `emu_watch_eval` | ne | Vyhodnotí watch nebo ad-hoc výraz |
| `emu_callstack_get` | ne | Snapshot shadow stacku |
| `emu_cdl_start` | ne | Start Code/Data Loggeru |
| `emu_cdl_stop` | ne | Stop CDL recording (data zachována) |
| `emu_cdl_reset` | ne | Vynuluje CDL countery |
| `emu_cdl_export` | ne | Export CDL bitmap + meta do souborů |
| `emu_trace_start` | ne | Spustí trace záznam kanálu (cputrack/iorqlog/intlog/hwlog) |
| `emu_trace_stop` | ne | Zastaví trace záznam kanálu (segment uzavřen) |
| `emu_trace_reset` | ne | Vynuluje aktuální segment trace kanálu |
| `emu_trace_save` | ne | Uloží/přesměruje segment trace kanálu (volitelný path) |
| `emu_videorec_start` | ano (vytvoří / přepíše soubor) | Spustí bezeztrátové nahrávání videa (volitelná cesta, automatický stop po N snímcích) |
| `emu_videorec_stop` | ne | Ukončí nahrávání videa, standardně počká na uložení souborů |
| `emu_videorec_pause` | ne | Pauza nahrávání (emulace běží dál): pauza / pokračování / přepnutí |
| `emu_videorec_marker` | ne | Marker (kapitola) na aktuální pozici nahrávky |
| `emu_videorec_status` | ne | Stav nahrávání videa (stav, snímky, segment, velikost, cesta, poslední událost) |
| `emu_videorec_timebase` | ne | Časová základna nahrávání: emulační čas nebo podle reality |
| `emu_profiler_start` | ne | Start CPU profileru (hot-path overhead) |
| `emu_profiler_stop` | ne | Stop profileru (data zachována) |
| `emu_profiler_reset` | ne | Vynuluje agregátor profileru |
| `emu_profiler_export` | ne | Export profile do CSV nebo JSON souboru |
| `emu_profiler_get` | ne | Inline JSON s entries + globální stats |
| `emu_media_load_mzf` | ne | CMT-hack instant load MZF (header + body) do RAM na LOAD_ADDR (path nebo bytes_b64) |
| `emu_media_run_mzf` | **ANO** | Load MZF + ROM disconnect + JP STRT (composite, autentic Sharp ROM Monitor LOAD handover) |
| `emu_media_load_binary` | **ANO** | Raw bajty z file do Z80 paměti, **destruktivní** |
| `emu_media_insert` | **ANO** | Vloží image do slotu (auto-eject pokud již vloženo) |
| `emu_media_eject` | ne | Vyjme image ze slotu |
| `emu_media_state` | ne | Snapshot stavu všech 11 slotů (cmt/fdc0_fd0..3/fdc1_fd0..3/qd/ide8) |
| `emu_settings_set` | **ANO** | Live INI write (whitelist), **mutuje globální stav** |
| `emu_settings_get` | ne | Read INI klíče + typ (open read, žádný whitelist) |
| `emu_platform_set` | **ANO** | Pokus o platform switch - vrací error (compile-time MZARCH) |
| `emu_periph_attach` | **ANO** | Aktivuje periferní modul (`requires_restart`) |
| `emu_periph_detach` | **ANO** | Deaktivuje periferní modul (`requires_restart`) |
| `emu_stop` | **ANO** | Hot-swap stop (pipe-only); ukončí emu, Python wrapper běží dál |
| `emu_start` | **ANO** | Hot-swap start (pipe-only); spawne nový mz800emu.exe child |
| `emu_input_send_key` | **ANO** | Press klávesy + N framů hold + release |
| `emu_input_send_keys` | **ANO** | Sekvence kláves (ascii nebo key_names encoding) |
| `emu_input_press_key` | **ANO** | Trvalý press bez auto-release (= SHIFT hold) |
| `emu_input_release_key` | **ANO** | Release konkrétní klávesy nebo všech kláves |
| `emu_input_send_joystick` | **ANO** | 8-bit joystick mask + hold + release |
| `emu_input_send_keys_with_delays` | **ANO** | Timing-controlled sekvence eventů |
| `emu_get_reg` | ne | Read jednoho Z80 registru (cheaper než `emu_get_registers`) |
| `emu_force_pause` | ne | Vynucená pauza emulace (bypass standardní pause) |
| `emu_set_user_cycle_origin` | **ANO** | Reset User cycle counter origin (mutuje debug stav) |
| `emu_get_im2_vector` | ne | Tool varianta IM2 ISR vector snapshotu (PIO-Z80 platformy) |
| `emu_get_raster_pos` | ne | Aktuální GDG raster pos + Z80 cycle countery |
| `emu_get_cpu_flags` | ne | Doplňkový CPU state (IFF/IM/HALT/INT/NMI/EI delay/Q/cycle countery/I/R) |
| `emu_set_cpu_flags` | **ANO** | Selektivní zápis IFF1/IFF2/IM/I/R (update_mask z přítomnosti polí) |
| `emu_get_last_instr` | ne | Poslední dokončená Z80 instrukce z history ringu (addr + bytes + length) |
| `emu_get_cpu_panel_batch` | ne | Atomic batch: regs + flags + raster + IM2 + last_instr v 1 round-tripu |
| `emu_debugger_activate` | **ANO** | Programaticky zapne debugger (= side effect: cpuhist + mhmap recording v WITH_WINDOW režimu) |
| `emu_debugger_deactivate` | **ANO** | Programaticky vypne debugger (= recording off) |
| `emu_is_debugger_active` | ne | Read-only probe `TEST_DEBUGGER_ACTIVE` |
| `emu_set_pioz80_interrupt_vector` | **ANO** | Override IM2 vector v PIO-Z80 portu A/B (MZ-800/MZ-1500; MZ-700 vrací `available:false`) |
| `emu_bp_create_with_init` | **ANO** | Atomický create + init BP polí (= `breakpoints_add_auto` + selektivní UPDATE v 1 safepointu) |
| `emu_bp_set_parent` | **ANO** | Quick reparent BP do skupiny (drag-drop sémantika) |
| `emu_bp_update` | **ANO** | Selektivní update polí existujícího BP přes `fields[]` array |
| `emu_bpgrp_add` | **ANO** | Přidat novou BP skupinu (name + parent) |
| `emu_bpgrp_remove` | **ANO** | Odebrat BP skupinu (cascading delete řeší backend) |
| `emu_bpgrp_update` | **ANO** | Selektivní update polí skupiny (enabled/name/colors/parent) |
| `emu_stack_history_enable` | **ANO** | Zapnout/vypnout SP history recording (disable flushne ring buffer) |
| `emu_stack_history_reset` | **ANO** | Flush SP history ring bufferu (recording flag zachován) |
| `emu_stack_regions_add` | **ANO** | Přidat monitorovaný stack region (name + base + limit) |
| `emu_stack_regions_edit` | **ANO** | Edit existujícího regionu (při úspěchu reset watermark + counters) |
| `emu_stack_regions_remove` | **ANO** | Odebrat region na indexu |
| `emu_stack_regions_reset_watermark` | **ANO** | Reset watermark + push/pop counters jednoho regionu |
| `emu_eventlog_start` | ne | Spustí Event Viewer recording |
| `emu_eventlog_stop` | ne | Zastaví recording (data v ringu zachována) |
| `emu_eventlog_clear` | **ANO** | Vyprázdní event ring buffer |
| `emu_eventlog_set_capacity` | **ANO** | Resize ringu (clamp na min/max) - discardne current content |
| `emu_eventlog_set_mask` | **ANO** | 64-bit categories filter bitmask |
| `emu_eventlog_get_event` | ne | Načte event[idx] - pxclk/screen/category/pc/payload |
| `emu_regions_list` | ne | Enumerate fyzických paměťových regionů (ROM, VRAM, CG-ROM, MemExt banks, ...) bypass Z80 banking |
| `emu_region_read` | ne | Raw no-side-effect read z konkrétního regionu (= ekvivalent GUI Memory browseru pro headless debug) |

## Sensitive operace

Tools označené jako **sensitive** mají v MCP schématu explicitní
WARNING o destruktivní povaze a MCP klient (Claude Code apod.) by se
měl uživatele zeptat na potvrzení (Claude Code defaultně řeší
permission per tool category).

Mezi sensitive tools patří:

- **Zápis do paměti / I/O / interrupts** - `emu_mem_write`,
  `emu_mem_write_force`, `emu_io_read`, `emu_io_write`,
  `emu_irq_inject`, `emu_nmi_inject`
- **Snapshot operace** - `emu_snapshot_save` (přepíše soubor),
  `emu_snapshot_load` a `emu_snapshot_load_buffer` (přepíší stav emu)
- **Media operace** - `emu_media_load_binary` (raw zápis do paměti),
  `emu_media_insert` (auto-eject přepíše mountnuté médium)
- **Konfigurace** - `emu_settings_set` (mutuje globální stav),
  `emu_platform_set`, `emu_periph_attach`, `emu_periph_detach`
- **Hot-swap** - `emu_stop`, `emu_start`
- **HID input** - všechny `emu_input_*` tools (user simulation může
  vyvolat nezamýšlené chování)
- **BP advanced** - `emu_bp_create_with_init`,
  `emu_bp_set_parent`, `emu_bp_update`, `emu_bpgrp_add`,
  `emu_bpgrp_remove`, `emu_bpgrp_update` (mutují `g_breakpoints` /
  `g_bp_groups` struktury debuggeru)
- **Stack analytics** - `emu_stack_history_enable`,
  `emu_stack_history_reset`, `emu_stack_regions_add`,
  `emu_stack_regions_edit`, `emu_stack_regions_remove`,
  `emu_stack_regions_reset_watermark` (mutují recording flag,
  ring buffer, regions definice + counters)

Pro `emu_mem_write` platí **region check** - cílová adresa musí ležet
v RAM regionu (ne ROM, CG-ROM, prohibited, unmapped). Pokud ne, tool
vrátí chybu `MEM_WRITE region check failed`. Pro explicit fault
injection (= zápis do regionů mimo RAM) je `emu_mem_write_force`.

## Návratová konvence při chybě

Při selhání backendu vrací každý tool objekt s polem `error`
(`{"error": "<popis>"}`). Dříve některé tooly v takovém případě
vracely holou prázdnou odpověď (`{}`), takže klient selhání nepoznal.
Nyní je chyba vždy explicitní - klient může spolehlivě testovat
přítomnost `error` v odpovědi. Na úspěch nese odpověď datová pole
konkrétního toolu (viz popisy níže), pole `error` v úspěšné odpovědi
není.

Text v `error` zároveň říká, zda emulátor příkaz provedl:

- `Emulator busy: command not executed ...` / `Emulator busy: command
  queue full ...` - příkaz se **neprovedl** (emulační vlákno ho do 10 s
  nevyzvedlo, nebo byla fronta plná); stav emulátoru je beze změny
  a volání je bezpečné zopakovat.
- `Emulator busy: command only partially executed ...` - vícekrokový
  tool skončil po provedení dřívějšího kroku; před opakováním
  zkontrolujte stav.
- `Emulator busy: command still running ...` - emulační vlákno příkaz
  převzalo, ale zhruba do 20 s (u nástrojů se souborovým I/O, např.
  trace_save nebo snapshot_save, do 10 min) ho nedokončilo; výsledek není znám
  (příkaz může doběhnout později). Před opakováním zkontrolujte stav.
- `Emulator is shutting down ...` - neprovedeno, další požadavky už
  neposílejte.
- jakýkoliv jiný text (např. `bp_remove failed (unknown id?)`) - příkaz
  se provedl a selhal (nebo byly odmítnuty parametry); opakování beze
  změny nepomůže.

Texty `Emulator busy: ...` / `Emulator is shutting down ...` končí
původní zprávou toolu v hranatých závorkách (kvůli kompatibilitě), např.
`... safe to retry [bp_remove failed (unknown id?)]`; testujte prefix.
Jakmile emulační vlákno příkaz vyzvedne, volání počká na jeho dokončení
a vrátí skutečný výsledek. Podrobnosti pro AI klienty:
`emulator://docs/error_handling`.

## Popis jednotlivých tools

### `emu_status`

Vrátí stav emulátoru: jestli běží nebo je zapauzovaný, kolik framů
už proběhlo, jestli je transport connected. Nedestruktivní, vhodné
jako první volání po připojení. Pokud emulátor nečekaně skončil,
výsledek je `{"running": false, "connected": false, "last_exit":
"..."}` (viz [Python wrapper](python-wrapper.md), sekce *Konec
emulátoru a automatický restart*).

### `emu_ping`

Vrátí `pong`. Slouží jako rychlý liveness check (= "drží bridge
ještě?") bez závislosti na stavu emu.

### `emu_pause`

Zapauzuje CPU emulaci. Tool se vrátí, až je pause potvrzená (= další
volání čte konzistentní stav). Žádné argumenty.

### `emu_run`

Resumne emulaci na konkrétní počet video framů (1-1000) a **blokuje**
dokud N framů neproběhne. Po doběhnutí N framů se emu automaticky
zapauzuje. Argument:

- `frames` (int, 1..1000) - kolik framů provést

Pokud `frames` přesáhne limit, tool vrátí chybu.

**Blokující sémantika:** Tool se vrátí až po skončení N framů, ne hned po
unpause. Emulátor se zastaví SÁM deterministicky přesně na N-té frame
hranici (emu-side frame-bounded stop), ne asynchronní pauzou z dispatch
vlákna. Běh stejného počtu framů ze stejného stavu je proto bitově
reprodukovatelný - důležité při trasování časově citlivých programů.
Pro PAL frame=50 Hz znamená `emu_run(frames=100)` cca 2 s blokování,
pro NTSC (60 Hz) cca 1.67 s.

Response obsahuje:

- `running: false` - emu je po doběhnutí pausnutá
- `actual_frames` (int) - skutečný počet proběhlých framů (= N, méně při
  zastavení breakpointem nebo safety timeoutu)
- `requested_frames` (int) - co bylo žádáno
- `stopped_by` (string) - důvod zastavení: `"frames"` (doběhl počet snímků),
  `"breakpoint"` (zastavil breakpoint), `"manual"` (manuální/UI pauza),
  `"timeout"` (safety timeout), `"unknown"` (jiný důvod)
- `complete` (bool) - true jen pokud doběhl plný počet snímků (= `stopped_by`
  je `"frames"`)

Pokud `frames == 0` (nebo chybí), tool jen submituje unpause a
vrací `running: true` async (= legacy fire-and-forget, pro
kompatibilitu se starými klienty).

Safety timeout: 2x očekávaný wallclock + 100 ms. Pro N=1000 framů
max blokování cca 40 s; po timeoutu vrátí `complete=false` s
actual_frames < requested.

### `emu_reset`

Provede power-on reset CPU a všech periferií. Stav RAM se vyresetuje
do default obsahu, registry se vynulují, PC se nastaví na ROM
entry point.

### `emu_get_registers`

Vrátí stav 14 Z80 registrů: `AF`, `BC`, `DE`, `HL`, `IX`, `IY`,
`SP`, `PC`, `AF'`, `BC'`, `DE'`, `HL'`, `I`, `R`. Hodnoty jako
unsigned hex stringy.

### `emu_set_register` (sensitive)

Přepíše 16-bitovou hodnotu jednoho Z80 registru. Argumenty:

- `reg` (string) - jméno registru, case-insensitive. Akceptuje
  `AF`, `BC`, `DE`, `HL`, `AF_` / `AF2`, `BC_` / `BC2`, `DE_` /
  `DE2`, `HL_` / `HL2`, `IX`, `IY`, `SP`, `PC`, `WZ`, `IR`.
- `value` (int, 0..65535) - nová hodnota. Pro `IR` se zapisuje jen
  dolní bajt (R), I se zachová.

**Destruktivní operace.** Zápis do `PC` mění tok provádění, zápis
do `SP` invaliduje aktuální call stack. Před experimenty doporučeno
uložit snapshot. Pro typický use case "skoč na adresu" stačí
`reg="PC", value=<addr>` - klient si pak může přečíst nové
registry přes `emu_get_registers`.

### `emu_mem_read`

Přečte N bajtů z Z80 paměti počínaje danou adresou. Argumenty:

- `address` (int, 0..0xFFFF) - start adresa
- `length` (int, 1..65536) - počet bajtů

Vrací base64-enkódovaná data. Region check NE - lze číst cokoliv
(ROM, RAM, peripheral mapped, ...). Banking se respektuje (= čte
se to, co by CPU vidělo na dané adrese).

### `emu_mem_write` (sensitive)

Zapíše bajty na Z80 adresu. **Destruktivní**. Argumenty:

- `address` (int, 0..0xFFFF) - start adresa
- `data` (base64 string) - bajty k zápisu

Aplikuje region check (= jen RAM). Pokud adresa není v RAM,
tool selže s `MEM_WRITE region check failed`.

### `emu_bp_add`

Přidá breakpoint. Ve výchozím stavu execution BP (= pause při dosažení
PC = address), ale umí i typed (memory / I/O) BP a podmínku.
Argumenty:

- `address` (int, 0..0xFFFF) - adresa BP (PC pro exec; sledovaná
  adresa / port pro typed)
- `type` (string, volitelný, default `"exec"`) - typ BP: `"exec"`
  (execution), `"memr"` (čtení paměti), `"memw"` (zápis do paměti),
  `"ior"` (I/O IN), `"iow"` (I/O OUT)
- `condition` (string, volitelný) - podmínkový výraz (bp_expr DSL,
  např. `A == 0x42`). Prázdné = bezpodmínečný BP.

Prostý exec BP bez podmínky jde lehkou cestou. Typed BP nebo BP
s podmínkou se transparentně vytvoří přes `emu_bp_create_with_init`
(= jednotná cesta pro typed breakpointy) - není potřeba volat
create ručně. Vrací handle (ID) breakpointu.

### `emu_bp_list`

Vrátí seznam aktuálních breakpointů. Návrat:
`{"count": <int>, "breakpoints": [...]}`, kde každý záznam nese:

- `id` (int) - handle breakpointu
- `addr` (int) - adresa
- `addr_end` (int) - horní mez rozsahu adres (včetně); platí jen pro
  `addr_match_mode` `RANGE`
- `addr_match_mode` (string) - `SINGLE` (jen `addr`), `RANGE`
  (`addr`..`addr_end`) nebo `MASK` (`(x & addr_mask) == (addr & addr_mask)`);
  týká se `PC_EXEC` / `MEM_R` / `MEM_W`
- `addr_mask` (int) - AND maska; platí jen pro `addr_match_mode` `MASK`
- `enabled` (bool)
- `type` (string) - kanonický UPPER_SNAKE typ (`PC_EXEC` / `MEM_R` /
  `MEM_W` / `IORQ_R` / `IORQ_W` / ...)
- `zone` (string) - paměťová zóna (`CPU_VIEW` / `RAM` / ...)
- `bank_id` (int) - index banky (pro zónu `MMEXT_BANK`)
- `bank_id_end` (int) - horní mez rozsahu bank (včetně); platí jen pro
  `bank_match_mode` `RANGE`
- `bank_match_mode` (string) - `SINGLE` / `RANGE` / `MASK` pro `bank_id`
- `bank_id_mask` (int) - AND maska pro `bank_id`; platí jen v režimu `MASK`
- `hits` (int) - počítadlo střelení BP
- `condition` (string nebo null, pokud bezpodmínečný)

### `emu_bp_remove`

Odstraní konkrétní breakpoint podle jeho ID. Argumenty:

- `id` (int) - ID získané z `emu_bp_add` nebo `emu_bp_list`

Vrací `{"id": <int>, "removed": true}` při úspěchu. Pokud ID
neexistuje, tool vrátí error.

### `emu_bp_clear`

Smaže všechny aktuální breakpointy najednou. Žádné argumenty.
Implementace dvoufázová (BP_LIST + N x BP_REMOVE), takže není
plně atomická - GUI by mohlo přidat BP mezi voláními, ten se
neodstraní (best-effort). Vrací `{"count": <int>, "cleared":
true}` s počtem skutečně odstraněných.

### `emu_bp_enable`

Toggle BP enabled flagu bez mazání ze seznamu. Užitečné pro
dočasné vypnutí BP, který chcete pak zase aktivovat (nemusíte
si pamatovat adresu). Argumenty:

- `id` (int) - ID breakpointu
- `enabled` (bool) - true = aktivní, false = ponechán v seznamu
  ale neaktivní

### `emu_step_into`

Provede přesně jednu Z80 instrukci. Pro CALL/RST je další
instrukce na cílové adrese (= step INTO subroutine, na rozdíl
od `emu_step_over`). Vyžaduje pause stav - pokud emu běží,
tool nejdřív zapauzuje a step se neprovede (klient musí
zavolat znovu).

### `emu_step_over`

Jako `emu_step_into` pro běžné instrukce, ale pro CALL/RST/DJNZ
a blokové instrukce nastaví dočasný BP na addr+length a run-to
(= subroutine se vykoná jako jeden krok). Vyžaduje pause stav.

### `emu_step_n`

Provede N instrukcí jako sekvenci `emu_step_into`. Argument:

- `count` (int, 1..1000) - počet kroků

Pokud kterýkoliv step selže mid-loop (= BP trefený, jiný důvod
pro pause), tool vrátí partial counter s `partial=true` flagem
v odpovědi. Návrat: `{"count": <int>, "requested": <int>,
"partial": <bool>}`.

### `emu_run_until_addr`

Nastaví dočasný BP na cílovou adresu a spustí emulaci. Emu se
zapauzuje, jakmile PC dosáhne `addr` (nebo zatím trefí jiný
permanentní BP). Argumenty:

- `addr` (int, 0..65535) - cílová PC adresa
- `max_cycles` (int, volitelný, default 10 mil.) - informativní
  timeout v T-states; klient může pollovat `emu_status` a sám
  zapauzovat, pokud doběhnutí trvá dlouho.

Vyžaduje pause stav.

### `emu_snapshot_save` (sensitive - přepíše soubor)

Uloží kompletní stav emulátoru do .mzs souboru na disk. Argumenty:

- `path` (string) - filesystem cesta k .mzs souboru
- `description` (string, volitelný) - lidsky čitelný popis vložený
  do metadat snapshotu

Vyžaduje pause stav. Odpověď:
`{"path": <str>, "ok": true, "result_code": 0}`.

### `emu_snapshot_save_buffer`

Stejné jako `emu_snapshot_save`, ale výsledný .mzs ZIP se vrátí jako
inline base64 string místo zápisu na disk. Užitečné když AI klient
nemůže/nechce psát na filesystém emu hosta. Argumenty:

- `description` (string, volitelný)

Odpověď: `{"bytes_b64": <str>, "size": <int>, "ok": true}`. Klient
si může `bytes_b64` uložit a později poslat zpět přes
`emu_snapshot_load_buffer`.

### `emu_snapshot_load` (sensitive - přepíše stav)

Načte .mzs ze souboru. Po úspěchu zůstává emulátor v pauze na PC
zachyceném ve snapshotu. Argument:

- `path` (string) - filesystem cesta k .mzs souboru

Odpověď: `{"path": <str>, "ok": true, "result_code": 0}`.

### `emu_snapshot_load_buffer` (sensitive)

Načte snapshot z inline base64 bufferu (typicky vyrobeného předchozím
`emu_snapshot_save_buffer`). Argument:

- `bytes_b64` (string) - base64-encoded .mzs ZIP

Odpověď: `{"size": <int>, "ok": true, "result_code": 0}`.

### `emu_cooperation_hint_set`

Self-binding instrukce, kterou si AI klient dobrovolně uloží do server
stavu, aby si sám sebe omezil mezi requesty (MCP je per-request
stateless). Server hint pouze persistne + broadcast UI notifikaci -
**žádné hard enforcement** (= AI hint může porušit, porušení skončí
v auditu).

Argumenty:

- `mode` (string, povinný) - jeden z:
    - `free` - bez omezení (clear)
    - `read_only` - AI bude volat jen read-only tools/resources
    - `paused_only` - AI bude volat tools jen pokud emu paused
- `until` (string, volitelný) - ISO 8601 timestamp nebo lidský popis
  ("next user message", "30 min"). Prázdné = otevřená délka.

Odpověď: `{"mode": <str>, "until": <str>, "ok": true}`.

### `emu_symbol_add`

Přidá user-defined symbol (named address) do symbol table. Pokud
symbol se stejným jménem existuje, je přepsán. Symboly jsou vidět
v disassembleru, breakpoint výrazech a další AI introspekci.

Argumenty:

- `addr` (int, povinný) - 0..65535
- `name` (string, povinný) - identifier z [A-Za-z0-9_.], no whitespace
- `comment` (string, volitelný) - human-readable komentář
- `kind` (string, volitelný) - "LABEL" / "DATA" / "BOOKMARK" (default
  "LABEL"). Aktuálně echo-only - storage vždy uloží jako user label.

Odpověď: `{"added": true, "addr": <int>, "name": <str>, "kind": <str>}`.

### `emu_symbol_remove`

Odebere symbol podle jména nebo podle adresy. Právě jeden z `name` /
`addr` musí být zadaný.

Argumenty:

- `name` (string, volitelný) - identifier
- `addr` (int, volitelný) - 0..65535 (`-1` = nepoužito)

Odpověď: `{"removed": <bool>, "name": <str>}` nebo `{"removed": <bool>,
"addr": <int>}` podle toho, co bylo zadané.

### `emu_symbol_lookup`

Read-only vyhledání jednoho symbolu. Argument `query` je
auto-detekovaný:

- hex string (`0x4242`, `4242h`, `$4242`) -> lookup by addr
- jinak -> lookup by name (case-sensitive)

Lookup vrací symbol s nejvyšší prioritou (user label > MAP > NOI >
SJASMPLUS).

Odpověď: `{"found": true, "addr": <int>, "name": <str>, "comment":
<str>, "source": <int>}` nebo `{"found": false}`. `source` je
číselný kód zdroje symbolu (0=SJASMPLUS, 1=NOI, 2=MAP, 3=user label).

### `emu_symbol_list`

Vypíše symboly matching name prefix. Užitečné pro AI prohlížení
symbol table (např. všechny `ROM_*` rutiny).

Argumenty:

- `prefix` (string, volitelný) - filter name prefix (default "" = vše)
- `limit` (int, volitelný) - max počet záznamů (1..1000, default 100)

Odpověď: `{"count": <int>, "items": [{"addr", "name", "comment",
"source"}, ...]}`.

### `emu_bookmark_add`

Přidá pojmenovanou adresovou záložku do debugger bookmark storage.
Záložky jsou vidět v debugger UI a v Resource `emulator://bookmarks`.
Vstup `input` se resolve na 16-bit adresu dynamicky: může to být hex
literál (`$1234`, `0x1234`, `#1234`, `1234h`, `1234`) nebo jméno
symbolu ze sym_db. Symbol jméno umožní, aby záložka sledovala symbol
i po změně jeho adresy. Záložka je označená ownerem `mcp`.

Argumenty:

- `input` (string, povinný) - hex literál nebo jméno symbolu
- `comment` (string, volitelný) - human-readable poznámka

Odpověď: `{"id": <int>, "input": <str>, "comment": <str>, "addr":
<int|null>, "addr_resolved": <bool>, "owner": "mcp"}`. `addr` je
`null`, pokud `input` nelze resolvovat (např. neznámý symbol) -
záložka se i tak uloží a resolve se zkusí později.

### `emu_bookmark_remove`

Odebere záložku podle ID. ID jsou monotonní a v rámci session se
nerecyklují; získat je lze z Resource `emulator://bookmarks`. Odebrání
neexistujícího ID není chyba - vrátí `removed=false`.

Argumenty:

- `id` (int, povinný) - ID záložky (>= 1)

Odpověď: `{"id": <int>, "removed": <bool>}`.

### `emu_step_out`

Run dokud Z80 nevrátí (RET) z aktuální subroutine. Implementačně:
získá top frame ze shadow callstacku, vezme jeho `return_addr`,
nastaví temporary breakpoint a spustí run-to.

Předpoklady:

- Callstack tracking musí být aktivní (zapnout v Callstack debugger
  okně nebo přes `--callstack` CLI flag).
- Emulátor musí být pausnutý před voláním.

Argumenty:

- `max_cycles` (int, volitelný, default 10000000) - informativní cap
  v T-states.

Odpověď: `{"return_addr": <int>, "max_cycles": <int>, "running": true}`
nebo error pokud callstack neaktivní / prázdný stack / emu už běží.

### `emu_run_until_raster`

Spustí emu a opakovaně step_into + raster snapshot, dokud GDG paprsek
nedorazí na cílovou scanline (a volitelně column). Užitečné pro
ladění mid-frame raster efektů (palette swap, scroll register write,
BCOL change) které jsou časované proti video signálu.

Argumenty:

- `line` (int, 0..511) - cílová scanline. Pro MZ-800 PAL frame má
  312 řádků, NTSC 262.
- `col` (int, volitelný, -1..2047, default -1) - cílový GDG column.
  -1 = libovolný column na cílové line.
- `max_cycles` (int, volitelný, default 10000000) - bezpečnostní cap
  na T-states polling smyčky.

Přesnost je omezena délkou Z80 instrukce (4-23 T-states ~ +/-10 GDG
ticks ~ +/-5 viditelných pixelů). Pro mid-frame efekty dostačující.

Odpověď: `{"scanline": <int>, "column_pixel": <int>, "frame_number":
<int>, "total_cycles": <int>, "delta_cycles": <int>, "reached":
<bool>}`. `reached=false` znamená timeout polling smyčky.

### `emu_run_until_tstate`

Spustí polling smyčku dokud absolutní Z80 cycle counter nedosáhne
target. Cycle counter (`total_cycles` v raster snapshotu) je
monotonicky rostoucí 32-bit T-state počet od resetu CPU.

Argumenty:

- `target_total_cycles` (int) - absolutní cílový cycle counter. **Musí
  být větší než aktuální** (server vrací chybu pro past targets).
- `max_cycles` (int, volitelný, default 10000000) - bezpečnostní cap
  delta T-states od začátku polling smyčky.

Pozn.: `total_cycles` je uint32, přetéká každých ~20 minut při 3.5 MHz.
Wraparound není ošetřen - typický klient cílí na delta v řádu sekund.

Odpověď: `{"total_cycles": <int>, "target": <int>, "delta_cycles":
<int>, "reached": <bool>}`.

### `emu_run_until_event`

Spustí polling smyčku dokud nenastane daný event. Podporované kindy:

- `"frame_done"` - uplynulo N video framů. `params`: `{"count": <int>}`
  (default 1, rozsah 1..10000).
- `"breakpoint_hit"` - spustí emu a čeká dokud BP nezpůsobí pause
  (nebo timeout). `params` může obsahovat `{"id": <int>}` (zatím
  ignorováno - jakákoliv pause splňuje čekání).

Argumenty:

- `kind` (string) - viz výše
- `params` (object, volitelný) - kind-specific
- `max_cycles` (int, volitelný, default 10000000) - bezpečnostní cap

Odpověď: `{"kind": <str>, "reached": <bool>, "delta_cycles": <int>,
...}` plus kind-specific fields (`frames_done` pro `frame_done`,
`paused` pro `breakpoint_hit`).

### `emu_io_read` (sensitive)

Z80 IN port - **reálné čtení s plnými side-effecty** na chipu
(PSG status flag reset, FDC IDX strobe, GDG DMD strobe). Argumenty:

- `port` (int, 0..65535)

Vrací `{"port": <int>, "value": <0..255>}`.

### `emu_io_write` (sensitive)

Z80 OUT port - **destruktivní zápis** přes plnou cestu Z80 OUT
(PSG latch, FDC command, GDG mode register, PIO output bity, ...).
Argumenty:

- `port` (int, 0..65535)
- `value` (int, 0..255)

Vrací echo `{"port": <int>, "value": <int>}`. Pokud klient
subscribuje topic `io_write`, každé OUT z hot pathu Z80 (= i mimo
tento Tool) emituje event s polem `{port, value, cycles}`.

### `emu_irq_inject` (sensitive)

Force maskable IRQ. Argumenty:

- `source` (string, default `"manual"`) - audit label
- `vector` (int, -1 nebo 0..255) - IM2 vektor; `-1` použije default
  vektor z chipu

Skutečné přijetí IRQ závisí na IFF1 (= pokud `DI`, IRQ se zapamatuje
a vykoná se po nejbližším `EI`). Vrací
`{"injected": true, "source": "...", "vector_used": <int|null>}`.

### `emu_nmi_inject` (sensitive)

Force NMI. Bez argumentů. Po dokončení aktuální instrukce CPU
skočí na 0x0066, uloží IFF1 do IFF2 a vynuluje IFF1. NMI je
**nemaskovatelné** - vždy se přijme. Vrací `{"injected": true}`.

### `emu_mem_write_force` (sensitive)

Zápis bajtů na Z80 adresu **bez region checku**. Identicky validuje
vstup jako `emu_mem_write`, ale neaplikuje filter na ROM/CG-ROM
oblasti - bajty se zapíšou přes banking-aware path, tedy do RAM
která je pod ROM namapovaná, nebo zachová ROM byte beze změny tam
kde ROM aktivní.

Argumenty:

- `addr` (int, 0..65535)
- `data_hex` (string, even-length hex)

Vrací `{"addr": <int>, "length": <int>}`. Pro skutečný "patch ROM"
zápis (= bypass ROM mappingu) je nutné dřív přepnout banking přes
`emu_io_write` na porty 0xE0..0xE4.

## Watch tools

Watch tools wrappují existující debugger Watch panel. Watche slouží
k dlouhodobému sledování konkrétní paměťové buňky nebo výrazu - klient
nemusí pokaždé volat `emu_mem_read`, stačí jedno `emu_watch_list` pro
snapshot všech registrovaných hodnot.

Watch řádek má tři módy:

- `address` - literal Z80 adresa + typ (default)
- `expr_scalar` - výraz se vyhodnotí jako int32, `type` určuje display
  formát (žádný extra memory read)
- `expr_deref` - výraz se vyhodnotí jako uint16 adresa, z té se čte
  podle `type` (analogicky jako `address`, jen adresa je dynamická)

Podporované typy (`type` argument): `u8` (default), `i8`, `u16le`,
`u16be`, `i16le`, `i16be`, `u32le`, `u32be`, `i32le`, `i32be`, `bit`,
`ascii`, `mzascii`, `bytes`.

### `emu_watch_add`

Přidá watch řádek do storage. Argumenty:

- `name` (string, optional) - jméno řádku (prázdné = anonymní)
- `addr` (int, optional, 0..65535) - jen pro mode=address
- `mode` (string, optional) - "address" / "expr_scalar" / "expr_deref"
- `expr` (string, optional) - výraz (povinný pro expr_* módy)
- `type` (string, optional, default "u8")

Vrací `{"index": <int>, "name", "mode", "type", "addr"}`.

### `emu_watch_remove`

Odebere watch řádek podle `name` nebo `index`. Vrací
`{"removed": <bool>, "index": <int>}`. `removed=false` = nenalezeno.

### `emu_watch_list`

Vrátí seznam všech watchů s aktuálními hodnotami:
`{"count": <int>, "items": [{"index", "name", "mode", "type", "addr",
"expr", "value"}, ...]}`. `value` je zformátovaný string podle typu
+ display formátu.

Limit 256 řádků (= praktický cap watch UI).

### `emu_watch_eval`

Vyhodnotí buď existující watch (přes `name` nebo `index`) nebo
ad-hoc výraz (`expr`). Při `expr` se výraz parsuje + vyhodnotí
bez perzistentního přidání.

Vrací `{"value_str": "...", "value_int": <int>, "error": ... | null}`.

## Callstack tool

### `emu_callstack_get`

Vrátí snapshot shadow stacku. Shadow stack pushuje na CALL / RST /
IRQ accept / NMI a popuje na RET / RETI / RETN nezávisle na CPU SP -
umožňuje sledovat hloubku volání i když Z80 stack obsahuje "garbage"
pushe (typicky CP/M BDOS dispatch přes PUSH+RET trampolínu).

Argumenty:

- `max_depth` (int, optional, 1..256, default 64) - omezí počet
  vrácených frames (vezme se top N).

Vrací `{"active", "count", "current_depth", "max_depth_reached",
"divergence_count", "overflow_count", "cycles_now", "frames": [...]}`.
Každý frame má `depth` (0 = top), `return_addr`, `call_site_addr`,
`target_addr`, `sp_at_entry`, `cycles_at_entry`, `kind` (string).

`kind` je jeden z: `call` / `rst` / `irq_im0` / `irq_im1` / `irq_im2`
/ `nmi` / `synthetic`.

Pokud `active=false`, Callstack subsystém je vypnutý - aktivace přes
GUI Settings nebo CLI `--callstack`. Bez aktivního subsystému je
`count` vždy 0.

## CDL tools

CDL = Code/Data Logger. Per-bajt klasifikace memory přístupů (Read /
Write / eXecute / Stack-write) postavená nad Memory Heatmap
subsystémem. FCEUX-style CDL bitmap je `counter > 0`.

### `emu_cdl_start`

Spustí CDL recording. Triggeruje swap CPU callbacků na slow path
s logging callbackem. Existující countery se nevynulují - klient by
měl volat `emu_cdl_reset` pokud chce čistý baseline. Vrací
`{"started": true, "mode": "always"}`.

### `emu_cdl_stop`

Zastaví CDL recording. Data zůstávají zachovaná pro pozdější export.
Vrací `{"stopped": true, "mode": "off"}`.

### `emu_cdl_reset`

Vynuluje všechny CDL countery. Mode se nemění - pokud bylo recording
aktivní, pokračuje od nuly. Vrací `{"reset": true}`.

### `emu_cdl_export`

Exportuje CDL data do sady souborů. Argument `path` ukazuje na
cílový meta JSON soubor (např. `/tmp/cdl-export/snap1.json`).
Exporter vytvoří parent adresář pokud neexistuje a zapíše:

- `snap1.json` - meta s region descriptory
- `snap1_bus.cdl` - binární counter array pro CPU bus mapu
- `snap1_ram.cdl` - pro hlavní RAM
- `snap1_rom_*.cdl` - pro ROM regiony
- `snap1_vram*.cdl` - pro VRAM (MZ-800 má víc per-mode regionů)
- `snap1_iorq_*.cdl` - pro I/O porty

Layout cell = 16 B (`r`, `w`, `x`, `s` jako uint32 LE counter).

Vrací `{"path": "...", "region_count": <int>}`. `region_count` je
počet exportovaných region souborů (= dependent na platformě).

## Trace tools

Lifecycle ovládání 4 binárních kanálů trace-suite (`cputrack`,
`iorqlog`, `intlog`, `hwlog`) za běhu. Rozhraní zrcadlí CDL tooly -
umožňuje AI klientovi ohraničit záznam přesně kolem sledovaného úseku
místo ukládání až při ukončení emulátoru. Detaily formátu výstupu viz
[Trace Suite](../debugger/Trace_Suite.md).

Všechny 4 tooly mají povinný argument:

- `channel` (string, povinný) - jeden z `cputrack` / `iorqlog` /
  `intlog` / `hwlog`.

### `emu_trace_start`

Spustí záznam kanálu (= režim ALWAYS). Vrací `{"started": true}`.

### `emu_trace_stop`

Zastaví záznam kanálu; segment se uzavře (flush + close). Vrací
`{"stopped": true}`.

### `emu_trace_reset`

Vynuluje aktuální segment kanálu (uzavře + znovuotevře segment;
u cputrack i reset HALT/self-loop collapse). Vrací `{"reset": true}`.

### `emu_trace_save`

Uloží / přesměruje segment kanálu. Argument:

- `path` (string, volitelný) - cílová cesta NÁSLEDUJÍCÍHO segmentu. Bez
  `path` se jen uzavře a znovuotevře aktuální segment na stávající
  dir/name (= "ulož teď").

Vrací `{"saved": true, "path": <str|null>}`.

## Video recording tools

Bezeztrátové nahrávání obrazu a zvuku emulovaného počítače (viz
[Nahrávání videa](../video-recording.md)) ovládané AI klientem: start,
hraní hry, markery, retake přes snapshot, stop. Celý postup včetně
exportu do MP4 je v MCP dokumentu `emulator://docs/videorec_workflow`.

Nahrávka standardně používá emulační čas (jeden emulovaný snímek = jeden
snímek videa, 50 snímků/s, na MZ-700 NTSC a MZ-1500 60), takže pauzy mezi voláními tools ani MAX SPEED
se ve videu neprojeví. Režim podle reality (`emu_videorec_timebase`)
nahrává naopak to, co bylo vidět a slyšet v reálném čase. Požadavky (start, pauza, marker, stop) se provedou na
konci nejbližšího emulovaného snímku; stop v pauze emulace se provede
hned. Dostupné ve všech buildech (MZ-800, MZ-700 PAL/NTSC, MZ-1500).

Každá úspěšná odpověď obsahuje stav nahrávání:

| Pole | Význam |
|------|--------|
| `supported` | Nahrávání je k dispozici (vždy `true`). |
| `state` | `idle`, `recording` nebo `paused` (pauza nahrávání). |
| `start_pending` | Start přijat, nahrávání začne na konci nejbližšího snímku. |
| `frames`, `fps`, `duration_s` | Počet nahraných snímků, snímková frekvence, délka v sekundách. |
| `segment`, `segment_open` | Číslo aktuálního segmentu (od 1) a zda je otevřený. |
| `bytes`, `parts` | Velikost a počet AVI souborů běžící nebo poslední nahrávky. |
| `retake_mode` | `discard`, `seam` nebo `off` - co udělá nahrání snapshotu. |
| `path`, `sidecar` | První AVI soubor a soubor `.cuts.json`. |
| `last_error` | Poslední chyba nebo `null`. |
| `last_event` | `null` nebo `{seq, kind, frame, path, text}`; `kind` = `started`, `saved`, `failed`, `retake`, `seam`. |
| `timebase`, `timebase_effective` | Požadovaná a skutečná časová základna (`emulated` / `realtime`); skutečná je `emulated` i při požadované `realtime`, když volba `realtime_speed = emulated_when_fast` a rychlost není 100 %. |
| `rt_activity` | Režim podle reality: `off` (emulační čas), `live` (živý obraz), `frozen` (pauza emulace, zamrzlý obraz), `skipping` (nezapisuje se). |

### `emu_videorec_start` (sensitive - vytvoří / přepíše soubor)

Argumenty:

- `path` (string, volitelný) - cílový `.avi`; existující soubor se
  přepíše. Bez něj se vygeneruje jméno `mz800_YYYYMMDD_HHMMSS.avi`
  ve výstupní složce nahrávek.
- `frames` (int, volitelný, výchozí 0) - automaticky ukončit po tolika
  nahraných snímcích (emulátor běží dál); 0 = bez limitu.

Soubor se vytvoří hned, takže chyba (např. `Cannot create video file:
<cesta>`) se vrátí okamžitě. Snímek, na kterém se start zpracuje, se
nenahraje. Vrací stav a navíc `start_requested` a `stop_after_frames`.
Chyba `Video recording is already running`, pokud nahrávání běží.

### `emu_videorec_stop`

Argumenty: `wait` (bool, výchozí `true`), `timeout_s` (výchozí 30).
S `wait` tool počká, až jsou soubory nahrávky hotové, a vrátí `saved`
(bool) a `recorded_frames`. Stop ještě nezpracovaného startu ho zruší
a soubor smaže (`start_cancelled: true`). Bez nahrávání: `Video recording
is not running`.

### `emu_videorec_pause`

Argument `paused` (bool, volitelný): `true` = pauza nahrávání, `false` =
pokračovat, bez argumentu = přepnout. Emulace běží dál; pokračování
začne nový segment (v exportu střih s přechodem). Vrací stav a navíc
`pause_requested` a `paused_target`.

### `emu_videorec_marker`

Argument `label` (string, volitelný; výchozí `Marker at frame N`). Marker
dostane index příštího nahraného snímku a při exportu z něj vznikne
kapitola. Markery v bodě snapshotu použitého pro retake a za ním se
zahodí spolu se zahozenou částí nahrávky.

### `emu_videorec_status`

Bez argumentů. Vrací stav popsaný výše.

### `emu_videorec_timebase`

Argument `timebase` (string, povinný): `emulated` nebo `realtime`.
Platí pro běžící nahrávku i pro příští start (INI `[VIDEOREC] timebase`).
Mění tedy trvalé uživatelské nastavení - stejně jako přepnutí v GUI
(`Alt + U`, okno ovládání nahrávání, dialog nastavení): při ukončení
emulátoru se uloží do INI (pokud neběží s `--no-save-ini`) a platí
i pro uživatele GUI po restartu. Agent, který přepnul do `realtime` na
sdílené instanci, by měl na konci vrátit `emulated`.
Do režimu podle reality se nahrávka přepne do jedné periody snímku (20 ms, při 60 snímcích/s 16,7 ms), zpět do emulačního
času na konci nejbližšího emulovaného snímku; každé přepnutí začne nový
segment. Podle reality se 50x (na MZ-700 NTSC a MZ-1500 60x) za sekundu reálného času zapíše to, co bylo
na obrazovce, se zvukem, který šel do reproduktoru (turbo je zrychlené,
pauza podle `realtime_pause`). Bez zvukového zařízení (`--headless`,
`--mcp-pipe`, nebo když se v okně nepodaří otevřít zvukové zařízení) se
zvuk vyrobí stejnou cestou, jen se nepřehraje. Vrací
stav a navíc `timebase_requested`; neplatná hodnota je chyba parametrů.

## Profiler tools

CPU profiler agreguje per-function statistiky (calls, exclusive
a inclusive Z80 cykly, min/max/avg). Stavěn nad Callstack listener
API - každý CALL / RST / IRQ / NMI vstup zaregistruje sample,
RET / RETI / RETN ukončí měření a započítá inclusive + exclusive
cycles.

**Hot-path overhead:** aktivní profiler přidává měřitelnou režii
v CPU smyčce. Pro production benchmarky doporučeno startovat
těsně před měřením a hned zase stopnout. Klient by neměl nechávat
profiler trvale zapnutý.

Profiler stavovou ownership:

- `emu_profiler_start` rozsvítí i Callstack subsystém pokud byl off.
- `emu_profiler_stop` vrátí Callstack do předchozího stavu (= jen
  pokud ho profiler sám aktivoval).
- `emu_profiler_reset` zachovává recording status - jen vynuluje
  agregátor.

### `emu_profiler_start`

Aktivuje profiler. Bez argumentů. Vrací `{"active": true}`.

### `emu_profiler_stop`

Vypne profiler. Data v agregátoru zůstávají - klient může pak volat
`emu_profiler_get` nebo `emu_profiler_export` před resetem. Vrací
`{"active": false}`.

### `emu_profiler_reset`

Vynuluje hash mapu entries, globální countery i baseline cycles
counter. Recording status se nemění. Vrací `{"reset": true}`.

### `emu_profiler_export`

Exportuje agregátor do souboru. Argumenty:

- `path` (string, povinný) - cílová filesystem cesta.
- `format` (string, optional, default "csv") - "csv" nebo "json".

Format detaily:

- CSV: UTF-8, LF line endings, locale-safe. Header `addr,kind,calls,
  excl_cycles,incl_cycles,min_cycles,max_cycles,avg_cycles`.
- JSON: `{"stats": {...}, "entries": [...]}` - globální stats +
  pole per-function records.

`min_cycles=0` značí entry bez matched exitu (= UI semantika).

Vrací `{"path": "...", "format": "csv"|"json", "entry_count": N}`.

### `emu_profiler_get`

Vrátí aktuální agregát jako inline JSON (bez file I/O). Argument:

- `limit` (int, optional, 1..1000, default 50) - max počet entries
  v response. `entry_count` v response je vždy total počet, nezávisle
  na limitu.

Response:

```json
{
  "active": bool, "entry_count": int, "limit": int,
  "total_cycles_64": int, "total_calls": int, "irq_entries": int,
  "unmatched_returns": int, "max_depth_reached": int,
  "overflow_count": int,
  "entries": [
    {"addr": int, "kind": "call"|"rst"|"irq_im0"|"irq_im1"|
                          "irq_im2"|"nmi"|"synthetic",
     "calls": int, "excl_cycles": int, "incl_cycles": int,
     "min_cycles": int, "max_cycles": int, "avg_cycles": int},
    ...
  ]
}
```

Pole `entries` je v pořadí hash-map iterace (= bez sortování).
Klient si sortuje podle vlastní metriky (typicky `excl_cycles`
descending).

## Media tools

Sjednocený přístup k media operacím napříč 11 podporovanými sloty:

| Slot | Význam |
|------|--------|
| `cmt` | CMT pásek (.mzf, .mzt) |
| `fdc0_fd0` .. `fdc0_fd3` | WD279x FDC0 (standard, porty 0xD8-0xDF) mechaniky 0..3 (.dsk) |
| `fdc1_fd0` .. `fdc1_fd3` | WD279x FDC1 (secondary, porty 0x58-0x5F) mechaniky 0..3 (.dsk) |
| `qd` | Quick Disk (.qd) |
| `ide8` | IDE8 master HDD (.img) |

Slot není dostupný pokud arch sestava nemá příslušný HW modul. V
takovém případě `emu_media_state` zařadí slot do response s
`inserted=false`, `path=""`.

**Inline base64:** `emu_media_load_mzf` a `emu_media_insert` přijímají
buď `path` NEBO `bytes_b64`. Pro `bytes_b64` server dekóduje obsah do
dočasného souboru a předá jeho cestu media subsystému. Tmp soubor se
po návratu pokouší smazat (best-effort - na Windows kde má drive open
handle zůstane do unmount).

### `emu_media_load_mzf`

CMT-hack instant load. Obejde tape emulaci a vykoná plný load (hlavička
+ tělo) přes ROM monitor patch (= program je v paměti okamžitě, bez
audio simulace). Tělo se uloží na LOAD_ADDR z MZF hlavičky.
Použití:

```
emu_media_load_mzf(path="/programs/gardener.mzf")
```

Nebo inline:

```
emu_media_load_mzf(bytes_b64="UVWdC...")
```

Vrací `{"ok": true, "load_addr": int, "exec_addr": int, "size": int,
"result_code": 0}` (`load_addr` = MZF fstrt, `exec_addr` = MZF fexec,
`size` = délka těla). Při chybě (soubor nejde otevřít / vadná hlavička /
selhání těla) vrací `{"error": ...}` - žádný falešný `ok`.

**Side effect:** pro program s LOAD_ADDR < 0x1000 se dolní ROM
(0x0000-0x0FFF) dočasně odmapuje během zápisu těla, aby program skončil
v RAM (jinak by se zapsal pod ROM a ztratil). Původní memory map se po
loadu obnoví, takže load nemá trvalý banking side effect. Je to čistý
load-only primitiv: nenastavuje PC ani SP a obnovuje použité scratch
registry HL/BC/AF.

CPU po `media_load_mzf` zůstává tam, kde byla (= typicky v ROM
Monitor scan loop) - data jsou v RAM, ale na ně se neskočí. Pokud
chcete program i spustit, použijte `emu_media_run_mzf` (= composite
níže).

### `emu_media_run_mzf` (sensitive)

Composite tool: nahraje MZF + odpojí dolní/horní ROM (= port
0xE0/0xE1) + skočí na MZF EXEC adresu (= STRT field, offset 0x16
v hlavičce). Autentický Sharp ROM Monitor LOAD handover bez čekání
na tape signal nebo ROM Monitor command prompt.

```
emu_media_run_mzf(path="/programs/mzdos.mzf")
```

Vrací:

```json
{
  "loaded": true,
  "header": {
    "file_type": 1, "filename": "mzdos",
    "file_size": 4096, "load_addr": 256, "exec_addr": 256
  },
  "rom_disconnected": true,
  "pc_set_to": 256
}
```

Implementace = Python composite (= `emu_media_load_mzf` +
`emu_io_write` x2 + `emu_set_register` PC). MZF hlavička se
parsuje klient-side (= rychlé, bez round-tripu pro header).

**Destruktivní:** CPU začne vykonávat kód z RAM s odpojenou ROM
ihned. Pokud nechcete tento autentic flow (= chcete ROM Monitor
LOAD process simulovaný přes klávesnici), použijte
`emu_media_insert(slot='cmt', path=...)` + `emu_input_send_keys`
sekvenci s "LOAD\r" (= pomalejší, ale projde plný ROM Monitor
state machine).

### `emu_media_load_binary` (sensitive)

Raw bajty z file do Z80 paměti na zadané adrese. Zapisuje přes
banking-aware path bez region checks - může přepsat ROM-shadow RAM,
video paměť atd. v aktuálním bankingu.

```
emu_media_load_binary(path="/build/sprite.bin", addr=0xC000)
```

Vrací `{"ok": true, "addr": 0xC000, "size": 256, "result_code": 0}`.

### `emu_media_insert` (sensitive)

Vloží image do slotu. Pokud je slot již obsazený, dojde k tichému
auto-eject (= ekvivalent eject + insert v jednom kroku).

```
emu_media_insert(slot="fdc0_fd0", path="/disks/cpm.dsk")
emu_media_insert(slot="fdc1_fd0", path="/disks/data.dsk")
emu_media_insert(slot="cmt", bytes_b64="...", ro=True)
```

Vrací `{"ok": true, "slot": "fdc0_fd0", "result_code": 0}`.

### `emu_media_eject`

Vyjme image ze slotu. No-op pokud je slot prázdný.

```
emu_media_eject(slot="fdc0_fd0")
```

Vrací `{"ok": true, "slot": "fdc0_fd0"}`.

### `emu_media_state`

Snapshot stavu všech 11 slotů.

```
emu_media_state()
```

Vrací:

```json
{
  "count": 11,
  "slots": [
    {"slot": "cmt",      "inserted": true,  "path": "/cassettes/sapo-p.mzf", "ro": false},
    {"slot": "fdc0_fd0", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd1", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd2", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc0_fd3", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd0", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd1", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd2", "inserted": false, "path": "", "ro": false},
    {"slot": "fdc1_fd3", "inserted": false, "path": "", "ro": false},
    {"slot": "qd",       "inserted": false, "path": "", "ro": false},
    {"slot": "ide8",     "inserted": true,  "path": "hdd0-mz800.img", "ro": false}
  ]
}
```

## Platform + Config tools

Klíčové designové rozhodnutí: **runtime platform switch NEEXISTUJE**
- mz700/mz800/mz1500 jsou separátní binárky podle compile-time
nastavení. `emu_platform_set` proto vrací error s informací, který
binární soubor uživatel musí spustit místo aktuálního.

### `emu_settings_set` (sensitive)

Zapíše live-settable INI klíč. Whitelist obsahuje klíče s okamžitou
aplikací (audio volume, video tweaks, QDISK path). Boot-time klíče
(paths emulátoru, snapshot defaults) jsou odmítnuty.

```
emu_settings_set(key="AUDIO/volume_8253", value="60")
emu_settings_set(key="DISPLAY/locked_window_aspect_ratio", value="true")
```

Vrací `{"key": ..., "previous_value": "50", "new_value": "60",
"type": "unsigned"}`. `previous_value` lze použít pro rollback.

Whitelist:

- `AUDIO/volume_8253` (unsigned)
- `AUDIO/volume_psg0` .. `AUDIO/volume_psg3` (unsigned)
- `AUDIO/volume_psg1_0` .. `AUDIO/volume_psg1_3` (MZ-1500, unsigned)
- `DISPLAY/forced_full_screen_redrawing` (bool)
- `DISPLAY/locked_window_aspect_ratio` (bool)
- `DISPLAY/custom_fps` (unsigned)
- `QDISK/filename` (text)
- `QDISK/write_protected` (bool)
- `TRACE_CPUTRACK/pc_range_lo` .. `TRACE_CPUTRACK/pc_range_hi` (unsigned, 0..65535; PC filtr trasování za běhu)

### `emu_settings_get`

Přečte aktuální hodnotu kteréhokoliv konfiguračního klíče (= read
whitelist je open). Vrací string reprezentaci + typový kód.

```
emu_settings_get(key="AUDIO/volume_8253")
-> {"key": "AUDIO/volume_8253", "value": "60", "type": "unsigned"}
```

Pro INI klíče s typem `keyword` se vrací textová varianta keyword.

### `emu_platform_set` (sensitive)

**Runtime platform switch není podporován.** Volání s
`kind != active_kind` vrací `ok=false` s `active_kind` (= aktuální
build) a vysvětlující error message. Uživatel musí spustit jinou
binárku (např. `mz1500emu.exe` místo `mz800emu.exe`).

Volání s `kind == active_kind` je no-op (= `ok=true, no_op=true`),
bez side-effects.

```
emu_platform_set(kind="mz1500")
-> {"ok": false, "active_kind": "mz800", "target_kind": "mz1500",
    "error": "Runtime platform switch not supported - mz700/mz800/
    mz1500 are separate binaries (compile-time MZARCH). To use a
    different platform, restart with the corresponding executable
    (e.g. mz1500emu.exe)."}
```

Parametry `mode` (`native` / `compat`) a `save_snapshot` (path k
.mzs) jsou pro forward compat akceptované, ale aktuálně ignorované.

### `emu_periph_attach` (sensitive)

Zapíše do konfigurace `active=true` pro daný periferní modul
(= `MEMEXT`, `FDC`, `QDISK`, `IDE8`, `GAL5`). Pro plnou aplikaci
vyžaduje restart emulátoru (= response carry
`requires_restart=true`).

```
emu_periph_attach(kind="memext", options={"type": "luftner4k"})
emu_periph_attach(kind="fdc")
```

Pro `memext` lze v `options.type` předat variantu (string).

Vrátí `{"ok": true, "kind": "memext", "requires_restart": true,
"result_code": 0}`. Pokud modul neexistuje (= periferie není
v této arch sestavě), vrátí error s detail.

### `emu_periph_detach` (sensitive)

Analogicky `active=false`. Také vyžaduje restart.

```
emu_periph_detach(kind="fdc")
```

### Příklad workflow - vložení Quick Disk obrazu

```
# 1. Nastavit cestu obrazu
emu_settings_set(key="QDISK/filename", value="/disks/cpm.qd")

# 2. Nastavit R/W mód
emu_settings_set(key="QDISK/write_protected", value="false")

# 3. Provést insert
emu_media_insert(slot="qd", path="/disks/cpm.qd")
```

## Hot-swap tools

Dvojice tools `emu_stop` + `emu_start` pro AI dev workflow: cyklus
stop -> rebuild mz800emu.exe -> start, bez restartu Claude Code
session a tedy bez ztráty AI kontextu. Oba tools jsou **pipe-only**
- v TCP režimu vracejí error, protože TCP attach jde proti živé
GUI session uživatele.

State preservation NENÍ automatický. AI klient si musí ručně uložit
stav před stop a obnovit ho po start:

```
emu_snapshot_save_buffer  ->  emu_stop  ->  (rebuild binary)
->  emu_start  ->  emu_snapshot_load_buffer
```

### `emu_stop` (sensitive, pipe-only)

Graceful shutdown emu child procesu. Wrapper `mcp_server.py` zůstává
naživu a drží MCP session s Claude klientem.

Response success:
```json
{"stopped": true, "transport": "pipe"}
```

Response error v TCP módu:
```json
{"error": "hot-swap requires pipe transport (current: tcp)"}
```

Argumenty: žádné.

### `emu_start` (sensitive, pipe-only)

Spawn nového emu child procesu. Counterpart `emu_stop` pro hot-swap.

Argumenty:
- `binary_path` (string, default ""): cesta k mz800emu.exe. Pokud
  prázdná, použije `MZ800EMU_EXE` env nebo default umístění vedle
  `mcp_server.py`.

Response success:
```json
{"started": true, "exe": "C:/.../mz800emu.exe", "commands": 65}
```

Pole `commands` udává počet tools v hello payload nového child
procesu (= pro AI klienta potvrzení, že nová binárka je živá).

Response error:
- `"hot-swap requires pipe transport"`
- `"emulator already running (call emu_stop first)"`
- `"mz800emu binary not found at ..."`

`emu_start` je **Python-only Tool** - emu binárka se nemůže spawnout
sama, jen wrapper má kontrolu nad subprocess lifecycle.

## HID input tools

Šest tools pro simulaci uživatelského vstupu - klávesnice + joystick.
Klíčové pro AI driven demos, automated testing a replay scenarios.

Press / release injekce probíhá přes **virtual keyboard matrix**,
která je paralelní k fyzické scan matrix. Z80 emulace AND-uje obě
matrix při čtení PORT B, takže virtual press se Z80 jeví jako reálná
klávesa držená uživatelem.

Klávesová jména (JEDNA tabulka sdílená nástroji `emu_input_send_key`,
`emu_input_send_keys` s `encoding=key_names`, `emu_input_press_key`,
`emu_input_release_key` a `emu_input_send_keys_with_delays`; bez ohledu na velikost písmen):

| Jméno / jména | Klávesa |
|---------------|---------|
| `RETURN`, `ENTER`, `CR` | CR (Enter) |
| `SPACE`, `TAB` (jen MZ-800), `BLANK`, `GRAPH`, `ALPHA` | klávesa stejného jména |
| `LIBRA` | `£` (= pozice SHIFT + DOWN_ARROW), `F9` = stejná pozice bez SHIFT |
| `INSERT`, `INS`, `INST` | INST |
| `DELETE`, `DEL`, `BACKSPACE` | DEL |
| `ARROW_UP`, `UP`, `CURSOR_UP` (obdobně `DOWN`, `LEFT`, `RIGHT`) | kurzorové klávesy (pohyb kurzoru) |
| `UP_ARROW`, `DOWN_ARROW` | znakové klávesy se šipkou (NE pohyb kurzoru) |
| `ESC`, `ESCAPE`, `BREAK`, `END` | ESC / BREAK |
| `CTRL`, `CONTROL`, `SHIFT` | modifikátory |
| `F1`..`F9` | funkční klávesy (`F6`-`F8` = `@`, `\`, `?`; `F9` = pozice LIBRA bez SHIFT, tj. DOWN_ARROW) |

Plus ASCII fallback (`"A"`, `"ASCII:A"`, ...). Názvy z
`emulator://docs/mz800_keyboard` (`CURSOR_*`, `INST`, ...) se akceptují jako
aliasy a starší názvy dál fungují. Neznámé jméno vrátí chybu
`Unknown key '<jméno>'. Closest valid names: ...` (u `send_keys` s
`key_names` a `send_keys_with_delays` selže celé volání ještě před odesláním
jakékoli klávesy).

Joystick state byte používá Sharp MZ standard:

- bit 0 = UP, 1 = DOWN, 2 = LEFT, 3 = RIGHT
- bit 4 = FIRE1, 5 = FIRE2
- bity 6-7 unused

MCP interface používá **active-HIGH** masku (0x01 = UP stisknuto).
Native HW state byte je active-LOW (0xFE = UP stisknuto). Bridge
konverze probíhá automaticky.

Frame timing default = 3 framy (~60 ms při 50 fps PAL). Maximum
600 framů (~12 s) jako safety proti AI freezi.

**Deterministická sémantika frames:** Mezi press a release backend
čeká na **N skutečných video framů** (= sleduje `fbsnapshot_screen_id`
counter inkrementovaný emu vláknem), ne wallclock sleep. Pokud byla
emulace pausnutá, helper ji krátce unpausne na dobu wait a restoruje.
Tím je garantováno, že ISR scan (= klávesová matrix čtení v PIO INT)
přesně N krát zachytí virtual press bit - žádný race condition mezi
async unpause a press timing.

Všechny HID tools nesou **WARNING** token v description - user
simulation může vyvolat nezamýšlené chování (= RUN+RETURN, BASIC
program přepsání, hra ovládaná AI v různých kontextech).

**Signalizace dosednutí kláves (`landing_verified`):** `emu_input_send_keys`
vrací `{"keys_sent": N, "keys_landed": N, "total_frames": N, "encoding": str,
"landing_verified": <bool>}`. `keys_sent` počítá host-side injekce do
virtual matrix; `keys_landed` počítá, kolik z nich guest skutečně přečetl.
Dříve tool hlásil úspěch, i když guest klávesy spolkl (běžící program s
odpojenou ROM skenuje jen úzkou podmnožinu matice) - to bylo tiché selhání.
Nově je `landing_verified` skutečný readback: po dobu držení klávesy emulátor
sleduje čtení PPI Port B a flag je `true` jen tehdy, když guest během držení
skenoval sloupec dané klávesy (= opravdu ji přečetl), jinak `false`. Pokud
klávesa nedosedne, podržte ji déle (`frame_per_key`), ujistěte se, že je guest
na promptu, který skenuje klávesnici, nebo emulátor resetujte.

### Praktické příklady

**ASCII encoding** (= default, jednoduchý text + `\r` pro RETURN):

```python
# Napsat "LOAD" + Enter (= Sharp ROM Monitor LOAD command)
emu.call('input_send_keys', text="LOAD\r", encoding='ascii',
         frame_per_key=3)

# BASIC program type-in (= multi-řádek)
emu.call('input_send_keys',
         text="10 PRINT \"HELLO\"\r20 GOTO 10\rRUN\r",
         encoding='ascii', frame_per_key=3)
```

**Key names encoding** (= JSON array string, explicit kontrola):

```python
import json
keys = ["G", "R", "A", "P", "H", "SPACE", "1", "RETURN"]
emu.call('input_send_keys', text=json.dumps(keys),
         encoding='key_names', frame_per_key=3)
```

**Modifier hold pattern** (= SHIFT + key):

```python
emu.call('input_press_key', key='SHIFT')   # hold SHIFT
emu.call('input_send_key', key='A', frames=3)  # press A while SHIFT held
emu.call('input_release_key', key='SHIFT')  # release SHIFT
```

**Joystick** (= bitmask game input):

```python
# Joystick port 0, UP + FIRE1 pro 5 framů
emu.call('input_send_joystick', port=0, state=0x11, frames=5)
```

**Timing-controlled sekvence** (= speedrun-style):

```python
events = [
    {"type": "key_press", "key": "RIGHT"},
    {"type": "wait_frames", "frames": 30},
    {"type": "key_release", "key": "RIGHT"},
    {"type": "key_press", "key": "SPACE"},
    {"type": "wait_frames", "frames": 2},
    {"type": "key_release", "key": "SPACE"},
]
emu.call('input_send_keys_with_delays', events=events)
```

### Reverse lookup tabulka kláves

Pro zjištění platných `key_names` pro konkrétní platformu načti:

```python
info = emu.call_resource('emulator://input/keyboard/matrix_info')
# vrátí { platform, keys: [{col, bit, name, ascii_char}], key_count }
```

Tabulka obsahuje aliasy (= `RETURN`/`ENTER`/`CR` ukazují na stejnou
pozici). Stejný `(col, bit)` se objevuje vícekrát pod různými jmény.

## Příklad použití z Claude Code

**Prompt:**

> Spusť emu, zapauzuj ho, podívej se na registry a vrať je.

**Co Claude udělá:**

1. `emu_status` -> ověří že je connected
2. `emu_pause` -> pauzne
3. `emu_get_registers` -> přečte registry
4. Vrátí uživateli formátovaný výpis registrů.

**Prompt:**

> Nastav breakpoint na 0x0000 a resetuj emu.

**Co Claude udělá:**

1. `emu_bp_add(address=0x0000)` -> přidá BP
2. `emu_reset` -> reset
3. `emu_run(frames=10)` -> emu pojede až narazí na BP nebo doběhne
   10 framů
4. `emu_status` -> reportne, jestli jsme na BP nebo doběhli framy

## BP advanced tools

Vrstva nad existujícími `emu_bp_add` / `emu_bp_list` / `emu_bp_remove`
přidává selektivní zápis polí BP a správu BP skupin (= hierarchie pro
seskupování breakpointů v UI).

### `emu_bp_create_with_init`

Atomický `breakpoints_add_auto(addr, name, parent)` + selektivní
`BP_UPDATE` pass v jediném DBGAPI safepointu. Klient nikdy neuvidí
částečně inicializovaný BP.

Args:

- `addr` (int) - počáteční Z80 adresa. UM_ADDR bit se přidá do
  `fields` implicitně, pokud chybí.
- `fields` (list[str]) - jména polí k aplikaci (subset
  `DBGAPI_BP_UM_*`). Validní názvy odpovídají maska bitům:
  `"enabled"`, `"auto_name"`, `"name"`, `"colors"`, `"parent"`,
  `"type"`, `"addr"`, `"addr_end"`, `"zone"`, `"bank_id"`,
  `"port"`, `"event_name"`, `"event_trigger"`, `"sp_threshold"`,
  `"expr"`, `"action"`, `"hit_count"`, `"skip_count"`,
  `"edge_triggered"`, `"addr_match_mode"`, `"addr_mask"`,
  `"port_match_mode"`, `"port_end"`, `"port_mask"`, `"port_mode"`,
  `"bank_match_mode"`, `"bank_id_end"`, `"bank_id_mask"`,
  `"bp_addr_space"`,
  `"sp_mode"`, `"sp_upper"`, `"im2_vector_filter"`,
  `"im2_vector_match_mode"`, `"im2_vector_addr_end"`,
  `"im2_vector_mask"`, `"im2_isr_filter"`,
  `"im2_isr_match_mode"`, `"im2_isr_addr_end"`, `"im2_isr_mask"`,
  `"im0_enabled"`, `"im1_enabled"`, `"im2_enabled"`,
  `"im0_rst_mask"`, `"irq_sig_source_mask"`,
  `"fwd_min_interval_ms"`, `"fwd_max_fires"`.
- `values` (dict) - map field name -> value. Hodnoty se použijí
  pouze pro pole zmíněná ve `fields`. String fieldy (`name`,
  `expr`, `action`, `event_name`) přijímají `None` jako clear.

Returns: JSON `{"id": int, "created": bool}`. `id` = -1 při
selhání. Pokud je `addr_end` nastaveno na jinou hodnotu než `addr`, ale
`addr_match_mode` není nastaven na `RANGE` (nový breakpoint začíná
v `SINGLE`), breakpoint se přesto vytvoří, hlídá jen `addr` a výsledek
nese navíc `warning`. Pro hlídání rozsahu uveď ve `fields` obě pole,
`"addr_end"` i `"addr_match_mode"` (hodnota `"RANGE"`).

Pole `fwd_min_interval_ms` a `fwd_max_fires` jsou per-BP override
rate-limitu forward akcí (snapshot / trace_save) - viz sekce
"Ochrana forward akcí (rate-limit + saturace)" níže.

### `emu_bp_set_parent`

Quick reparent BP do skupiny. Args: `id`, `parent_id` (-1 = root).
Returns: `{"updated": bool}`.

### `emu_bp_update`

Selektivní update polí existujícího BP. Args: `id` (BP ID),
`fields` (= jména polí), `values` (= map hodnot). Prázdný `fields`
seznam = no-op success. Returns: `{"updated": bool}`.

Stejně jako `emu_bp_create_with_init` přijímá i pole
`fwd_min_interval_ms` a `fwd_max_fires` pro per-BP override
rate-limitu forward akcí - viz sekce "Ochrana forward akcí
(rate-limit + saturace)" níže.

### Ochrana forward akcí (rate-limit + saturace)

Forward akce v BP skriptu, které zapisují na disk (snapshot,
trace_save), mají implicitní ochranu proti zahlcení emulátoru
a zaplavení disku, pokud BP "fajruje" velmi často:

- **Per-BP rate-limit.** `fwd_min_interval_ms` udává minimální
  prodlevu v ms mezi dvěma forward akcemi téhož BP; rychlejší
  opakování se tiše přeskočí. `fwd_max_fires` je tvrdý strop počtu
  úspěšných firů za session - po dosažení se BP sám zakáže. Obě pole
  jsou nastavitelná přes `emu_bp_create_with_init` / `emu_bp_update`
  (`fields: ["fwd_min_interval_ms", "fwd_max_fires"]`). Bez
  explicitního override platí globální default daný konfigurací.
- **Byte saturace.** Kumulativní byte-limit objemu zápisů z forward
  akcí (snapshot + trace, včetně flush-side účtování) - po překročení
  prahu emulátor sám zapauzuje a vydá saturation event / warning
  s důvodem. Práh je konfigurovatelný.
- **Robustnost control-plane.** I když pokračující BP spustí těžkou
  forward akci, příkazy pause / stop / vyčištění BP zaberou vždy -
  emulátor se nezasekne.

### `emu_bpgrp_add`

Přidá novou BP skupinu. Args: `name`, `parent` (default -1).
Returns: `{"id": int}` (= -1 při cyklu / chybějícím parentu).

### `emu_bpgrp_remove`

Odebrání skupiny. Cascading delete / reparent dětí (BPs +
sub-skupin) řeší backend (`breakpoints.c`). Args: `id`. Returns:
`{"removed": bool}`.

### `emu_bpgrp_update`

Selektivní update polí skupiny. Args: `id`, `fields` (subset
`"enabled"` / `"name"` / `"colors"` / `"parent"`), `enabled`,
`name`, `bg_rgb`, `fg_rgb`, `parent`. Returns: `{"updated": bool}`.

## Stack analytics tools

Wrappery nad SP history ring bufferem (= sekvence SP hodnot
v čase) a definovanými stack regiony (= pojmenované rozsahy adres
s push/pop counters + watermark).

### `emu_stack_history_enable`

Zapnutí / vypnutí SP history recording. Disable navíc flushne
ring buffer (= další enable startuje s čistým stavem). Aktivační
flag se promítá do hot-path call site v `mzarch.c` (= zero
overhead při default OFF).

Args: `enabled` (bool). Returns: `{"enabled": bool}`.

### `emu_stack_history_reset`

Flush ring bufferu bez vypnutí recording flagu (= UI "Reset
history" tlačítko před sledováním konkrétní code path). Returns:
`{"reset": true}`.

### `emu_stack_regions_add`

Přidá monitorovaný region. Args: `name` (max 31 znaků), `base`
(vrchol = nejvyšší addr), `limit` (dno; `base > limit`).
Validation v `stack_regions_add` (= overlap, duplicate). Returns:
`{"index": int, "added": bool}` (= -1 při invalidu).

### `emu_stack_regions_edit`

Edit regionu na indexu. Args: `index`, `name`, `base`, `limit`.
Při úspěchu resetuje watermark + push/pop counters (= staré stats
nesedí na nový rozsah). Returns: `{"updated": bool}`.

### `emu_stack_regions_remove`

Odebrání regionu. Args: `index`. Returns: `{"removed": bool}`.

### `emu_stack_regions_reset_watermark`

Reset watermark + push/pop counters jednoho regionu.
Konfigurační pole (name/base/limit) zůstávají. Args: `index`.
Returns: `{"reset": bool}`.

## Eventlog tools

Eventlog (= Event Viewer / TLOG) je ring buffer pro capture
chronologického streamu eventů (raster, IRQ ACK, IORQ IN/OUT,
MMIO R/W, GDG mode/banking/scroll/colors/video, PIO 8255 / CTC 8253
/ PIO Z80 events, PSG, FDC, MemExt, BP fire, user marks, atd.).
Klíčové pro AI klient analýzu emu chování bez nutnosti GUI Event
Viewer okna.

Filter: 64-bit bitmask per kategorie (= `en_EVENTLOG_CATEGORY`
v `eventlog.h`). Klient nastaví mask před start, pak emity respektují
filter.

### `emu_eventlog_start`

Bez params. Spustí recording (= následující emity v emu thread se
zapisují do ringu). Returns: `{"started": bool}`.

### `emu_eventlog_stop`

Bez params. Zastaví recording, content ringu zachován. Returns:
`{"stopped": bool}`.

### `emu_eventlog_clear` (sensitive)

Bez params. Vyprázdní ring buffer (= ztráta capture history). Returns:
`{"cleared": bool}`.

### `emu_eventlog_set_capacity` (sensitive)

Args: `capacity` (= požadovaná velikost ringu v eventech). Backend
clampuje na `[EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY]`
(= 10000..200000). Resize realokuje a discardne current content.

Returns: `{"capacity_after": int}` (= skutečně nastavená velikost po
clampu).

### `emu_eventlog_set_mask` (sensitive)

Args: `mask` (= 64-bit bitmask, bit i enable kategorii i). Akceptuje
**int** (= max 63 bitů kvůli signed gint64) nebo **hex string** (=
plný 64-bit rozsah včetně bitu 63, formát `"0xN..."` nebo `"N..."`,
podporuje `_` jako visual separator).

Returns: `{"mask_hex": "0xN..."}` (= 16-digit zero-padded hex string
pro unambiguous round-trip).

### `emu_eventlog_get_event`

Args: `idx` (= 0 = oldest, capacity-1 = newest). Pokud `idx >= count`:
`{"available": false, "idx": int}`. Jinak vrátí plný 24 B record:

```json
{
  "available": true, "idx": int,
  "pxclk_total": int,        // 64-bit pixel clock total since power-on
  "screens_total": int,      // 32-bit frame counter
  "pxclk_in_screen": int,    // 32-bit pixel clock pos v aktuálním framu
  "category": int,           // 8-bit en_EVENTLOG_CATEGORY
  "subtype": int,            // 8-bit category-specific subtype
  "pc": int,                 // 16-bit Z80 PC při emit
  "payload": int             // 32-bit category/subtype-specific data
}
```

Decoding `payload` per category je v `eventlog.h` (= klient si
mapping replikuje nebo dotáhne z knowledge base).

## Direct memory region tools

### `emu_regions_list`

Bez params. Enumerate všech fyzických paměťových regionů aktuální
architektury (= ekvivalent GUI Memory browser pro headless debug).

Returns:

```json
{
  "regions": [
    {"id": 0, "kind": "logical", "name": "Z80 view",
     "logical_base": 0, "size": 65536, "writable": true,
     "connected": true, "mapped_now": true},
    {"id": 1, "kind": "ram", "name": "RAM (raw 64K)",
     "logical_base": null, "size": 65536, ...},
    {"id": 2, "kind": "rom_lower", "name": "Monitor ROM (lower)",
     "logical_base": 0, "size": 4096, "writable": false, ...},
    {"id": 8, "kind": "memext_ram", "sub_id": 12,
     "name": "Memext RAM bank 12", "size": 4096, ...},
    ...
  ],
  "count": int
}
```

`kind` enum: `logical, ram, rom_lower, rom_upper, cgrom, cgram_700,
vram_700_char, vram_700_attr, vram_phys_plane (sub_id 0..3),
pcg_1500 (sub_id 0..2), memext_ram, memext_flash, ramdisk_std,
ramdisk_pezik, prohibited_shadow`.

`sub_id` disambiguuje regiony stejného kindu (= plane index, bank
index, PEZIK instance).

**Stabilita ID**: per session, NE per HW reconfigure. Po
`periph_attach/detach` nebo `media_insert/eject` klient musí volat
`regions_list` znovu pro nové IDs.

### `emu_region_read`

Raw no-side-effect read z konkrétního regionu (= bypass Z80 banking).
Žádný auto-inc latch, GDG RF dispatch ani IRQ trigger.

Args:
- `region_id` (= z poslední `regions_list` volání)
- `offset` (= byte offset v rámci regionu, 0..size-1)
- `length` (= 1..65536, clamp na region size pokud `offset+length > size`)

Returns:
```json
{
  "region_id": int,
  "offset": int,
  "length": int,        // skutečně přečtená délka po clampu
  "data_b64": "..."     // base64-encoded raw bajty
}
```

Hlavní use case: AI klient v headless mode (= bez GUI access)
inspectuje fyzické paměti emu - ROM monitor content, VRAM bitmap
planes, MemExt banks. Reply 0007 v mzdos requests-to-emu-team
obsahuje příklady použití.

## Speed control tools

Ovládání tempa emulace (= ekvivalent GUI menu Speed). Hlavní use case:
nechat dlouhou operaci (boot ROM, cassette/disk load, dlouhý výpočet)
rychle proběhnout přes max speed (warp), pak zpět na 100 %. Změna tempa
je viditelná akce (= objeví se v Activity logu jako MCP akce).

Aktuální stav rychlosti se čte přes Resource `emulator://speed`.

### `emu_set_speed`

Nastaví emulační rychlost dle `mode`.

Args:
- `mode` (povinné): jeden z
  - `normal` - běh na 100 % (reálné tempo, vypne warp)
  - `custom` - běh na přesných `percent` % (vypne warp)
  - `max` - warp / unthrottled, co nejrychleji (zapne warp)
  - `step` - relativní úprava custom % o `step` (warp nemění)
- `percent` (default 100): cílové % pro `mode=custom` (1..4000, clamp v jádru)
- `step` (default 0): relativní delta pro `mode=step` (kladné rychleji,
  záporné pomaleji, 0 = no-op)

Returns:
```json
{
  "ok": true,
  "mode": "max",           // výsledný režim (max/custom/normal)
  "current_percent": 100,  // aktuální % po operaci
  "max_speed": true        // warp flag po operaci
}
```

Pozn.: `emu_run(frames=N)` čeká na N snímků; při max speed se frame
counter točí rychleji, takže blokující `emu_run` doběhne v kratším
wallclock čase (chování zůstává korektní, jen rychlejší).

### `emu_speed_step`

Convenience wrapper kolem `emu_set_speed(mode="step", step=delta)`.

Args:
- `delta`: relativní delta v % (kladné rychleji, záporné pomaleji)

Returns: stejný echo objekt jako `emu_set_speed`.

### Příklad workflow - warp přes boot

```python
emu_set_speed(mode="max")        # zapni warp
emu_run(frames=600)              # nech doběhnout boot/load rychle
emu_set_speed(mode="normal")     # zpět na 100 %
```

## CMT kazetové tools

Ovládání kazetové pásky Sharp MZ (CMT). Záměrně rozlišujeme dva
oddělené koncepty:

- **Reálná páska** (`emu_cmt_play`, `emu_cmt_play_paused`,
  `emu_cmt_stop`, `emu_cmt_pause`, `emu_cmt_eject`, `emu_cmt_record`)
  pohání cyklicky přesnou emulaci kazety. Program čte/zapisuje signál
  pásky přes standardní ROM rutinu Sharp, přesně jako fyzický hardware.
  Toto je přesná cesta a funguje s libovolným programem.
- **cmthack** (`emu_cmt_hack_set`) je ROM-patch zkratka (instant load):
  patchnutá ROM load rutina zkopíruje soubor pásky přímo do RAM a
  obejde signál pásky. Je rychlejší, ale funguje jen pro programy, které
  načítají přes patchnuté ROM vstupní body.

Všechny tyto mění stav emulátoru (sensitive) a objeví se v Activity logu
jako MCP akce. Aktuální stav pásky, včetně `cmthack_enabled`, se čte přes
resource `emulator://periph/cmt`.

Před přehráváním musí být páskový obraz nejdřív vložen přes
`emu_media_insert(slot="cmt", path=...)`.

### `emu_cmt_play` (sensitive)

Spustí přehrávání reálné kazety (transport PLAY). No-op pokud není
vložena páska nebo přehrávání nelze v aktuálním stavu spustit.

Returns: `{"ok": true, "action": "play"}`.

### `emu_cmt_play_paused` (sensitive)

Jako `emu_cmt_play`, ale páska startuje v pauze; pro posun volej
`emu_cmt_pause(paused=False)`.

Returns: `{"ok": true, "action": "play_paused"}`.

### `emu_cmt_stop` (sensitive)

Zastaví transport (PLAY nebo RECORD -> STOP). No-op pokud už zastaveno.
Bez vložené pásky transport také uvede do STOP.

Returns: `{"ok": true, "action": "stop"}`.

### `emu_cmt_pause` (sensitive)

Pauzne (`paused=True`, default) nebo odpauzne (`paused=False`) běžící
transport bez resetu pozice.

Args:
- `paused` (default true): true = pauznout, false = odpauznout.

Returns: `{"ok": true, "action": "pause"}`.

### `emu_cmt_eject` (sensitive)

Zastaví transport (pokud běží) a vyjme vložený páskový obraz.
Zúžený ekvivalent `emu_media_eject(slot="cmt")`.

Returns: `{"ok": true, "action": "eject"}`.

### `emu_cmt_record` (sensitive)

Spustí nahrávání výstupu reálné kazety do WAV, LEP nebo L16. Nahrávání
startuje v pauze; pro zahájení záznamu volej
`emu_cmt_pause(paused=False)`. Formát určuje přípona (`.lep` používá
jednotky 50 us, `.l16` jednotky 16 us); bez přípony se použije WAV.
Vyžaduje stav pásky STOP a zapisovatelnou cestu.

Args:
- `path` (required): cílová cesta `.wav`, `.lep` nebo `.l16` (musí být zapisovatelná).

Returns: `{"ok": true, "path": "<path>"}`. Selže (`error`) při špatném
stavu nebo nezapisovatelné cestě.

### `emu_cmt_hack_set` (sensitive)

Zapne nebo vypne cmthack ROM patch (instant tape load). NENÍ reálná
páska - viz poznámka výše.

Args:
- `enabled` (required): true = nainstaluj ROM patch, false = odeber.

Returns: `{"ok": true, "installed": <bool>}` echo stavu patche po
operaci.

### Příklad workflow - nahrávání výstupu reálné pásky

```python
emu_media_insert(slot="cmt", path="game.mzf")  # zdrojová páska
emu_cmt_record(path="out.wav")     # arm nahrávání (startuje v pauze)
emu_cmt_pause(paused=False)        # zahaj záznam
# ... spusť program ...
emu_cmt_stop()                     # ukonči a zapiš zvolený formát
```

### `emu_cmt_set_speed` (sensitive)

Nastaví rychlostní poměr reálné pásky vůči 1200 Bd. Přijímá string klíč
nebo en_CMTSPEED int: `1:1` (1), `2:1` (2), `2:1_cpm` (3), `3:1` (4),
`3:2` (5), `7:3` (6), `8:3` (7), `9:7` (8), `25:14` (9). Mění výchozí
rychlost; per-blok override viz `emu_cmt_tape_set_block_speed`. Odráží
se v `cmtspeed` v `emulator://periph/cmt`.

Args:
- `speed` (required): poměr string ("2:1") nebo int (2).

Returns: `{"ok": true, "property": "speed", "value": <int>}`.

### `emu_cmt_set_polarity` (sensitive)

Nastaví polaritu páskového signálu (zadní DIP přepínač). Odráží se v
`polarity_inverted`.

Args:
- `inverted` (required): true = invertovaná, false = normální.

### `emu_cmt_set_cpu_boost` (sensitive)

Zapne/vypne CPU boost během transportu pásky (= běh na maximální
rychlosti pro rychlé dlouhé loady). Odráží se v `cpu_boost`.
Uživatelská preference: načtení snapshotu ji neobnovuje (snapshot ji
kvůli starším verzím emulátoru dál obsahuje).

Args:
- `enabled` (required): true = boost, false = reálný čas.

### `emu_cmt_set_mzfsize_check` (sensitive)

Zapne/vypne kontrolu konzistence velikosti MZF při loadu (body size vs
header file size). Odráží se v `mzfsize_check`.

Args:
- `enabled` (required): true = vynutit kontrolu, false = přeskočit.

### `emu_cmt_open` (sensitive)

CMT-specifický open souboru podle přípony (.mzf/.mzt/.wav/...). Na rozdíl
od `emu_media_insert(slot="cmt")` umí přes `play_immediately` rovnou
spustit přehrávání (= jeden round trip místo open + play). Reálná páska,
ne cmthack.

Args:
- `path` (required): cesta k CMT souboru (přípona vybírá backend).
- `play_immediately` (default false): spustit přehrávání po openu.

Returns: `{"ok": true, "path": <str>, "playing": <bool>, "state": <str>,
"paused": <bool>}`. `playing`, `state` (`"stop"` / `"play"` / `"record"`)
a `paused` hlásí skutečný stav transportu po operaci (stejný význam jako
v `emulator://periph/cmt`); `playing` je true jen pro PLAY bez pauzy. Pokud
byl požadován `play_immediately`, ale páska po operaci nehraje, výsledek
obsahuje navíc `warning`.

### `emu_cmt_tape_seek` (sensitive)

Seek na blok pásky (SIMPLE_TAPE multi-blok containery: .mzt, .tap).
Jednosouborový container (SINGLE: samostatné .mzf nebo .wav) seek
nepodporuje - volání selže i pro blok 0; pro přehrání od začátku použij
`emu_cmt_stop` + `emu_cmt_play`. Vyžaduje naloženou pásku. Seznam bloků
viz `emulator://periph/cmt/tape`.

Args:
- `block_id` (required): index bloku (0-based).

Returns: `{"ok": true, "block_id": <int>}`.

### `emu_cmt_tape_set_block_speed` (sensitive)

Per-blok rychlost (JEN cmt speed, žádné další parametry; SIMPLE_TAPE).
`speed` přijímá stejné klíče/int jako `emu_cmt_set_speed`. Vyžaduje
naloženou pásku.

Args:
- `block_id` (required): index bloku (0-based).
- `speed` (required): poměr string ("2:1") nebo int (2).

Returns: `{"ok": true, "block_id": <int>, "speed": <int>}`.

### Příklad workflow - multi-blok páska

```python
# viz emulator://periph/cmt/tape pro container_type a seznam bloků
emu_cmt_open(path="tape.mzt")
emu_cmt_tape_seek(block_id=2)       # vyber třetí program
emu_cmt_play()
```

Kompletní workflow (reálná páska vs cmthack, transport, bloky) je v
resource `emulator://docs/cmt_workflow`.

## Související

- [Python wrapper](python-wrapper.md) - jak tools volat z Claude Code
- [Konfigurace](configuration.md) - security profile, sensitive
  tool gating
- [Resources overview](resources-overview.md) - read-only URI
  endpointy doplňující tools
