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
| `emu_pause` | ne | Pauzne emulaci (volitelně jen běh `if_run_seq`) |
| `emu_resume` | ne | Volný běh bez blokování, volitelně deadline v emulovaném čase |
| `emu_wait_stop` | ne | Počká, až běh skončí (nejvýš 25 s na volání): důvod, PC, kdo běh spustil |
| `emu_get_stop_history` | ne | Posledních 32 záznamů zastavení (i `emulator://run/history`) |
| `emu_run` | ne | Blokující resume na N framů (1..1000) |
| `emu_reset` | ne | Power-on reset CPU + periferií |
| `emu_get_registers` | ne | 14 Z80 registrů (AF..IR) |
| `emu_set_register` | **ANO** | Zápis 16-bit hodnoty do registru (volitelně jen při staré hodnotě `expected`) - **destruktivní** |
| `emu_dasm` | ne | Disassembluje N instrukcí od adresy (banking-aware) |
| `emu_history_get` | ne | 32 posledních provedených instrukcí (ring buffer) s registry před každou z nich |
| `emu_mem_read` | ne | Čte bajty z Z80 paměti (base64 + SHA-256, nebo jen hash) |
| `emu_mem_write` | **ANO** | Zápis bajtů do RAM - region check, volitelně jen při hashi `expected_sha256`, **destruktivní** |
| `emu_bp_add` | ne | Přidá breakpoint (exec, nebo typed memw/memr/ior/iow + condition) |
| `emu_bp_list` | ne | Vypíše breakpointy (id/addr/addr_end/addr_match_mode/addr_mask/enabled + type/zone/bank_id/bank_id_end/bank_match_mode/bank_id_mask/hits/condition) |
| `emu_bp_remove` | ne | Odebere konkrétní BP podle ID |
| `emu_bp_clear` | ne | Smaže všechny breakpointy najednou |
| `emu_bp_enable` | ne | Toggle BP enabled flagu (bez mazání) |
| `emu_step_into` | ne | 1 instrukce krok (step INTO CALL/RST) |
| `emu_step_over` | ne | 1 instrukce, CALL/RST jako atom |
| `emu_step_n` | ne | N kroků jako sekvence step_into (1..1000) |
| `emu_run_until_addr` | ne | Run dokud PC == addr (s temp BP), volitelně deadline |
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
| `emu_step_out` | ne | Run dokud RET z aktuální subroutine (callstack pop), volitelně deadline |
| `emu_run_until_raster` | ne | Run do GDG raster pozice (line + optional col) |
| `emu_run_until_tstate` | ne | Run do absolutního Z80 cycle target |
| `emu_run_until_event` | ne | Run dokud event (`frame_done`, `breakpoint_hit`) |
| `emu_event_subscribe` | ne | Subscribe na seznam topics (`breakpoint_hit`, `paused`, `resumed`, `step_done`, `io_write`) |
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
| `emu_trace_start` | ne | Spustí trace záznam kanálu (cputrack/iorqlog/intlog/hwlog/marklog) |
| `emu_trace_stop` | ne | Zastaví trace záznam kanálu (segment uzavřen) |
| `emu_trace_reset` | ne | Zapíše aktuální segment trace kanálu na disk (nic nezahodí) |
| `emu_trace_save` | ne | Uloží/přesměruje segment trace kanálu (volitelný path) |
| `emu_trace_status` | ne | Stav trace kanálů a ringu Event Vieweru pro čtení |
| `emu_trace_read` | ne | Stránka záznamů trace / Event Vieweru (kurzor, filtr, rozpočet znaků) |
| `emu_ramlog_start` | ne | Spustí záznam zápisů do paměti (rozsahy, ring / fill, kapacita) |
| `emu_ramlog_stop` | ne | Zastaví záznam zápisů (záznamy zůstanou čitelné) |
| `emu_ramlog_clear` | ne | Vyprázdní záznam zápisů |
| `emu_ramlog_status` | ne | Stav záznamu zápisů |
| `emu_ramlog_who_wrote` | ne | Poslední zapisovatel a historie zápisů buňky paměti |
| `emu_ramlog_writers` | ne | Instrukce, které zapisovaly do rozsahu |
| `emu_bplog_clear` | ne | Vyprázdní kanál `bplog` (výstup akcí BP `log` a `stop`) |
| `emu_videorec_start` | ano (vytvoří / přepíše soubor) | Spustí bezeztrátové nahrávání videa (volitelná cesta, automatický stop po N snímcích) |
| `emu_videorec_stop` | ne | Ukončí nahrávání videa, standardně počká na uložení souborů |
| `emu_videorec_pause` | ne | Pauza nahrávání (emulace běží dál): pauza / pokračování / přepnutí |
| `emu_videorec_marker` | ne | Marker (kapitola) na aktuální pozici nahrávky |
| `emu_videorec_status` | ne | Stav nahrávání videa (stav, snímky, segment, velikost, cesta, poslední událost) |
| `emu_videorec_timebase` | ne | Časová základna nahrávání: emulační čas nebo podle reality |
| `emu_frame_hash` | ne | SHA-256 obrazu a VRAM, pozice paprsku (bez dat) |
| `emu_replay_record_start` | ano (vytvoří / přepíše soubor) | Start záznamu vstupů do souboru (startovní snapshot, vstupy na hranici snímku, hashe stavu) |
| `emu_replay_record_stop` | ne | Konec záznamu a uložení souboru |
| `emu_replay_play` | ano (nahradí stav emulátoru) | Přehrání záznamu od startovního snapshotu, zastavení na prvním rozdílném hashi |
| `emu_replay_play_stop` | ne | Ukončení běžícího přehrávání |
| `emu_replay_rewind` | ano (nahradí stav emulátoru) | Návrat na checkpoint: při záznamu zahodí vstupy po něm a nahrává dál, při přehrávání skočí zpět |
| `emu_replay_seek` | ano (nahradí stav emulátoru) | Skok přehrávání na snímek (zpět přes checkpoint nebo od začátku, dopředu v MAX SPEED) |
| `emu_replay_status` | ne | Stav záznamu / přehrávání a výsledek posledního |
| `emu_screenshot_save_to_file` | ano (vytvoří / přepíše soubor) | PNG obrazovky do souboru: oblast (full / canvas), měřítko (framebuffer / nativní body / okno 1-4), ořez |
| `emu_screenshot_series_start` | ano (vytváří soubory) | Série snímků: PNG každého změněného emulovaného snímku (oblast, měřítko, ořez, only_changed, frames, max_files) |
| `emu_screenshot_series_stop` | ne | Ukončí sérii snímků, standardně počká na zápis všech snímků |
| `emu_screenshot_series_status` | ne | Stav série snímků (sledované snímky, soubory, nezměněné, složka, důvod konce) |
| `emu_profiler_start` | ne | Start CPU profileru (hot-path overhead) |
| `emu_profiler_stop` | ne | Stop profileru (data zachována) |
| `emu_profiler_reset` | ne | Vynuluje agregátor profileru |
| `emu_profiler_export` | ne | Export profile do CSV nebo JSON souboru |
| `emu_profiler_get` | ne | Inline JSON s entries + globální stats |
| `emu_profiler_pc_start` | ne | Start profileru podle PC (vykonání, T-stavy a WAIT na adresu instrukce) |
| `emu_profiler_pc_stop` | ne | Stop profileru podle PC (data zachována) |
| `emu_profiler_pc_reset` | ne | Smaže data profileru podle PC |
| `emu_profiler_pc_get` | ne | Nejvytíženější adresy instrukcí s nejbližším symbolem (řazení, limit, rozsah adres) |
| `emu_profiler_pc_export` | ne | Všechny vykonané adresy do CSV souboru |
| `emu_profiler_pc_bitmap` | ne | Vykonané adresy jako bitmapa 8 KiB v base64 (pokrytí kódu) |
| `emu_coverage_start` | ne | Spustí pokrytí kódu: úroveň `branch` (podle PC) nebo `detail` (podle fyzické buňky a verze kódu) |
| `emu_coverage_stop` | ne | Zastaví pokrytí kódu (data zůstanou čitelná) |
| `emu_coverage_reset` | ne | Smaže data pokrytí kódu |
| `emu_coverage_status` | ne | Stav pokrytí kódu, úplnost a souhrn větví |
| `emu_coverage_mark` | ne | Nová epocha pokrytí (co se poprvé vykoná po značce) |
| `emu_coverage_read` | ne | Stránka řádků pokrytí: stavy větví, čítače, verze kódu (kurzor, filtr, rozpočet znaků) |
| `emu_coverage_export` | ne | Všechny řádky pokrytí do souboru JSON nebo CSV |
| `emu_audio_capture_start` | ano (vytváří soubory) | Zachycení zvuku do složky a/nebo do paměti: změny úrovní kanálů, zápisy do PSG, reference.wav a sdl.wav (frames, výběr souborů, memory, pcm) |
| `emu_audio_capture_stop` | ne | Ukončí zachycení zvuku a dopíše soubory |
| `emu_audio_capture_status` | ne | Stav zachycení zvuku (snímky, události, zápisy PSG, vzorky, bloky SDL, složka, důvod konce, paměť) |
| `emu_audio_capture_summary` | ne | Souhrn paměti zachycení: úseky ticha a zvuku, kanály, noty PSG, tón nebo PCM reproduktoru, reference proti zvukovému výstupu |
| `emu_audio_capture_pcm` | ne | Okno PCM z paměti zachycení (WAV nebo surové v base64, převzorkovaný náhled nebo přesně 44100 Hz) |
| `emu_media_load_mzf` | ne | CMT-hack instant load MZF (header + body) do RAM na LOAD_ADDR (path nebo bytes_b64) |
| `emu_media_run_mzf` | **ANO** | Reset + spuštění MZF ve stavu jako po `--run-mzf` (výchozí); `bootstrap=false` = load + ROM disconnect + JP STRT bez resetu |
| `emu_media_load_binary` | **ANO** | Raw bajty z file do Z80 paměti, **destruktivní** |
| `emu_media_insert` | **ANO** | Vloží image do slotu (auto-eject pokud již vloženo) |
| `emu_media_eject` | ne | Vyjme image ze slotu |
| `emu_media_state` | ne | Snapshot stavu všech 11 slotů (cmt/fdc0_fd0..3/fdc1_fd0..3/qd/ide8) |
| `emu_cmt_open` | **ANO** | Otevře soubor pásky (.mzf, .mzt, .wav, ...) do skutečného kazetofonu, volitelně přehraje |
| `emu_cmt_eject` | **ANO** | Vysune pásku ze skutečného kazetofonu |
| `emu_cmt_play` | **ANO** | Spustí přehrávání pásky (transport PLAY) |
| `emu_cmt_play_paused` | **ANO** | Spustí přehrávání pásky v pauze (čeká na pokračování) |
| `emu_cmt_pause` | **ANO** | Pozastaví / obnoví transport pásky |
| `emu_cmt_stop` | **ANO** | Zastaví transport pásky (PLAY nebo RECORD -> STOP) |
| `emu_cmt_record` | **ANO** | Nahrává výstup pásky do WAV souboru |
| `emu_cmt_hack_set` | **ANO** | Zapne / vypne cmthack (patch ROM pro okamžité nahrání z pásky) |
| `emu_cmt_set_cpu_boost` | **ANO** | Zapne / vypne zrychlení CPU během běhu pásky |
| `emu_cmt_set_mzfsize_check` | **ANO** | Zapne / vypne kontrolu velikosti MZF při nahrávání |
| `emu_cmt_set_polarity` | **ANO** | Polarita signálu pásky (zadní DIP přepínač) |
| `emu_cmt_set_speed` | **ANO** | Výchozí rychlost přehrávání virtuálního kazetofonu (poměr nebo vlastní pulzy) |
| `emu_cmt_tape_seek` | **ANO** | Posune víceblokovou pásku na zvolený blok |
| `emu_cmt_tape_set_block_speed` | **ANO** | Rychlost přehrávání jednoho bloku pásky |
| `emu_set_speed` | ne | Rychlost emulace: 100 %, vlastní procenta nebo MAX SPEED |
| `emu_speed_step` | ne | Změní vlastní rychlost emulace o relativní počet procent |
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
| `emu_set_cpu_flags` | **ANO** | Selektivní zápis IFF1/IFF2/IM/I/R (update_mask z přítomnosti polí), volitelně jen při starých hodnotách `expected`; vrací `previous` |
| `emu_get_last_instr` | ne | Poslední dokončená Z80 instrukce z history ringu (addr + bytes + length) |
| `emu_get_cpu_panel_batch` | ne | Atomic batch: regs + flags + raster + IM2 + last_instr v 1 round-tripu |
| `emu_state_observe` | ne | Registry, příznaky, číslo běhu, čas a až 16 oken paměti (CPU nebo region) z jednoho okamžiku, SHA-256 u každého okna |
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
| `emu_region_write` | **ANO** | Raw zápis do fyzického regionu paměti (mimo banking Z80), max 65536 bajtů |
| `emu_file_load_to_region` | **ANO** | Nahraje soubor (nebo jeho část) do regionu paměti, max 65536 bajtů |
| `emu_file_save_from_region` | ano (vytvoří / přepíše soubor) | Uloží část regionu paměti do souboru, max 65536 bajtů |
| `emu_freeze_add` | **ANO** | Zafrozí bajt / bajty regionu (zpětný zápis jednou za snímek) |
| `emu_freeze_remove` | ne | Uvolní zafrozené bajty (úsek nebo vše) |
| `emu_freeze_list` | ne | Zafrozené bajty (i ty z GUI) |
| `emu_memsearch_start` | ne | Spustí hledání proměnné (cheat search) nad prostorem, první vzorek |
| `emu_memsearch_filter` | ne | Zúží kandidáty (eq, dec_by, changed, ... proti hodnotě / předchozímu / prvnímu) |
| `emu_memsearch_sample` | ne | Nový vzorek bez filtrace |
| `emu_memsearch_undo` | ne | Vrátí poslední krok hledání (kandidáty i referenční vzorky) |
| `emu_memsearch_redo` | ne | Znovu provede vrácený krok |
| `emu_memsearch_list` | ne | Stránka kandidátů s hodnotami, CPU adresou a nápovědou pro write breakpoint |
| `emu_memsearch_eliminate` | ne | Ručně vyřadí vybrané kandidáty |
| `emu_memsearch_close` | ne | Zavře sezení hledání |

## Sensitive operace

Tools označené jako **sensitive** mají v MCP schématu explicitní
WARNING o destruktivní povaze a MCP klient (Claude Code apod.) by se
měl uživatele zeptat na potvrzení (Claude Code defaultně řeší
permission per tool category).

Mezi sensitive tools patří:

- **Zápis do paměti / I/O / interrupts** - `emu_mem_write`,
  `emu_mem_write_force`, `emu_freeze_add`, `emu_io_read`, `emu_io_write`,
  `emu_irq_inject`, `emu_nmi_inject`
- **Snapshot operace** - `emu_snapshot_save` (přepíše soubor),
  `emu_snapshot_load` a `emu_snapshot_load_buffer` (přepíší stav emu)
- **Záznam vstupů** - `emu_replay_record_start` (přepíše soubor),
  `emu_replay_play` (nahradí stav emulátoru)
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
- `Precondition failed: ...` - podmíněný zápis (`expected_sha256`,
  `expected`) emulační vlákno zkontrolovalo a **nic nezapsalo**: paměť
  nebo registr se liší od toho, co klient přečetl. Aktuální hodnota je
  v textu; před dalším rozhodnutím je potřeba číst znovu.
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
volání čte konzistentní stav). Argument:

- `if_run_seq` (int, volitelný) - pauzni jen, když běží běh s tímto
  číslem (viz `emu_wait_stop`); běh, který mezitím spustil někdo jiný
  (uživatel v GUI), se nezastaví

Vrací `{"paused": <bool>, "was_running": <bool>, "paused_now": <bool>,
"run_seq": <int>}`, navíc `skipped` (`"run_seq_mismatch"` nebo
`"not_running"`), když pauzu `if_run_seq` nepustil. Pauza už pozastavené
emulace není nové zastavení: žádná událost `paused`.

### `emu_resume`

Spustí emulaci bez blokování a hned vrátí `{"running": true,
"run_seq": <int>, "already_running": <bool>}`. Volitelně deadline
v emulovaném čase, nejvýš jeden z:

- `deadline_frames` (int, 1..1000000) - zastav přesně na N-té hranici
  snímku
- `deadline_ms` (int, 1..86400000) - zastav po N ms emulovaného času
- `deadline_ticks` (int) - zastav po N taktech GDG

Deadline je deterministický (emulovaný čas, ne čas hostitele); `ms`
a `ticks` zastaví v prvním bodě událostí emulace na nebo za deadlinem,
méně než snímek pozdě. Zastavení má důvod `"deadline"`, odpověď nese
`deadline_ticks`. Deadline platí do nejbližší pauzy a přežije reset
stroje během běhu. Na konec běhu se čeká přes `emu_wait_stop`.

### `emu_wait_stop`

Počká, až běh skončí, a vrátí proč a kde. Běh je úsek od opuštění
pauzy do další pauzy; každý běh má číslo `run_seq` (vracejí ho nástroje,
které běh spouštějí, a `emu_pause`, `emu_get_state`). Argumenty:

- `run_seq` (int, volitelný) - běh, na který se čeká; bez něj aktuální
  běh (v pauze poslední)
- `timeout_ms` (int, 0..25000, default 5000)

Vrací `{"done": true, "run_seq", "record": {...}}`, nebo po timeoutu
`{"done": false, "running": true, ...}` - zavolat znovu. Záznam:
`reason` (`run_to`, `breakpoint`, `deadline`, `frames`, `step`,
`manual`, `exit`, `bp_action_saturation`, `event_trigger`, `replay`,
`replay_end`, `replay_desync`), `pc`,
`bp_id`, `started_by` / `start_cmd` (`mcp` = agent, `user` = GUI),
`stopped_by` (u `manual`), `start_ticks` / `stop_ticks` (takty GDG od
resetu), `elapsed_ticks`, `frame`, `step`; u `exit` navíc `exit_code`,
`exit_reason`, `message`. `history_lost: true` = běh skončil, ale jeho
záznam už mezi 32 posledními není. Stejná pole nese událost `paused`.
Recept v `emulator://docs/run_control`.

### `emu_get_stop_history`

Poslední záznamy zastavení, od nejstaršího. Argument `limit` (int,
1..32, default 32). Vrací `{"records": [...], "count",
"current_run_seq", "running"}`; pole záznamu jako u `emu_wait_stop`.
Také jako resource `emulator://run/history`.

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
  `"run_to"` (dočasný breakpoint: run to cursor, step over),
  `"exit"` (podmínka konce: akce breakpointu `exit`, `--exit-after-frames`,
  `--timeout-*` - emulátor se jen pozastavil, viz
  [Headless režim](headless-mode.md#automatické-běhy)),
  `"bp_action_saturation"` (zastavil limit objemu zápisů akcí
  breakpointů, `[BREAKPOINTS] fwd_byte_limit_mb`),
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
- `value` (int, 0..65535) - nová hodnota. `IR` zapíše celý pár I:R:
  I je horní bajt, R dolní bajt včetně bitu 7 (jako LD I,A a LD R,A).
- `expected` (int, 0..65535, volitelný) - podmíněný zápis
  (compare-and-swap): zapíše jen tehdy, když má registr pořád tuto
  hodnotu. Porovnání i zápis proběhnou v emulačním vlákně najednou,
  jinak chyba `Precondition failed: register <reg> changed (expected=...,
  current=...)` a nic se nezapíše.

Vrací `{"reg", "value", "previous"}` - `previous` je hodnota před
zápisem.

**Destruktivní operace.** Zápis do `PC` mění tok provádění, zápis
do `SP` invaliduje aktuální call stack. Před experimenty doporučeno
uložit snapshot. Pro typický use case "skoč na adresu" stačí
`reg="PC", value=<addr>` - klient si pak může přečíst nové
registry přes `emu_get_registers`.

### `emu_mem_read`

Přečte N bajtů z Z80 paměti počínaje danou adresou. Argumenty:

- `address` (int, 0..0xFFFF) - start adresa
- `length` (int, 1..65536) - počet bajtů
- `want_data` (bool, volitelný, default true) - false = jen hash

Vrací base64-enkódovaná data (`data_b64`) a `sha256` (SHA-256 bajtů,
64 hex znaků malými písmeny). S `want_data=false` jen hash: porovnáním
s dřívějším čtením se pozná, jestli se paměť změnila, bez stahování
dat. Region check NE - lze číst cokoliv
(ROM, RAM, peripheral mapped, ...). Banking se respektuje (= čte
se to, co by CPU vidělo na dané adrese).

### `emu_mem_write` (sensitive)

Zapíše bajty na Z80 adresu. **Destruktivní**. Argumenty:

- `addr` (int, 0..0xFFFF) - start adresa
- `data_hex` (hex řetězec, např. `"DEADBEEF"` nebo `"DE AD BE EF"`) -
  bajty k zápisu
- `expected_sha256` (string, volitelný) - podmíněný zápis: zapíše jen
  tehdy, když SHA-256 právě přepisovaných bajtů pořád odpovídá této
  hodnotě (`sha256` z `emu_mem_read` nebo z okna `emu_state_observe`
  téhož rozsahu). Kontrola i zápis proběhnou v emulačním vlákně
  najednou, jinak chyba `Precondition failed: memory changed (...)`
  a nic se nezapíše.
- `want_hashes` (bool, volitelný) - vrátí `before_sha256`
  a `after_sha256` i bez podmínky

Aplikuje region check (= jen RAM). Pokud adresa není v RAM,
tool selže s `MEM_WRITE region check failed` (region check má přednost
před podmínkou). Vrací `{"addr", "length"}`, s podmínkou nebo
`want_hashes` navíc `before_sha256` a `after_sha256`. Adresa jde přes
aktuální mapování paměti; zápis, který musí trefit jednu fyzickou
buňku, patří do `emu_region_write`. Recept:
`emulator://docs/conditional_writes`.

### `emu_state_observe`

Přečte registry, příznaky, číslo běhu (`run_seq`), emulovaný čas a až
16 oken paměti v **jednom průchodu emulačního vlákna**, takže vše
pochází z jednoho okamžiku - i za běhu (pak je okamžikem začátek snímku
a volání čeká nejvýš jeden snímek). Dvě samostatná volání
(`emu_get_cpu_panel_batch` + `emu_mem_read`) vidí za běhu vždy dva různé
snímky. Argumenty:

- `windows` (seznam 0..16 položek, celkem nejvýš 65536 bajtů) - položka
  je rozsah CPU adres `{"addr": <int>, "len": <int>}` (aktuální
  mapování) nebo fyzický region `{"region": {"kind": "<druh>", "sub_id":
  <int>}, "offset": <int>, "length": <int>}` (druhy z `emu_regions_list`,
  bez ID regionu, takže okno platí i po změně sestavy HW); volitelně
  `name` (vrací se zpět) a `want_data`
- `include` (seznam, volitelný) - `raster`, `im2`, `last_instr`,
  `memory_map`
- `hashes` (seznam, volitelný) - SHA-256 částí stavu `cpu`, `ram`,
  `vram`, `memext`, `pcg` (stejná definice jako hashe záznamu vstupů)
- `want_data` (bool, default true) - false = u oken jen `sha256`

Vrací `run_seq`, `running`, `stable` (true jen v pauze), `drain`
(`frame` / `paused`), `time` (`frame`, `frame_ticks`, `total_cycles`),
`regs`, `flags`, `windows` (echo, `sha256` - stejná hodnota jako
`emu_mem_read` / `emu_region_read` rozsahu - a `data_b64`), `complete`
a vyžádané sekce. Nepřipojený region dá oknu `error:
"region_unavailable"` a `complete: false`, volání ale uspěje. Neplatná
okna (moc oken, moc bajtů, přes konec adresního prostoru nebo regionu,
neznámý druh) jsou `Invalid parameters`. Čtení je bez vedlejších efektů.
Podrobně: `emulator://docs/state_observe`.

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
od `emu_step_over`). Vyžaduje pause stav: tool se vrátí po
provedení kroku (`completed: true`). Na běžícím emulátoru se nic
nestane a emulace běží dál (`completed: false`, `running: true`).
Vrací `{"stepped": true, "completed", "running", "run_seq"}`;
`stepped` zůstává kvůli kompatibilitě a je vždy true.

### `emu_step_over`

Jako `emu_step_into` pro běžné instrukce, ale pro CALL/RST/DJNZ
a blokové instrukce nastaví dočasný BP na addr+length a run-to
(= subroutine se vykoná jako jeden krok). Vyžaduje pause stav. Běžná
instrukce: tool se vrátí po kroku (`completed: true`). Přes
CALL/RST/DJNZ/blokové instrukce se vrátí hned (`completed: false`,
`running: true`); když run-to zastaví na svém dočasném BP, nese událost
`paused` hodnotu `reason: "run_to"` (viz `emu_run_until_addr`), čekat
přes `emu_wait_stop(run_seq)`. Na běžícím emulátoru emulaci jen
pauzne (`paused_only: true`).

### `emu_step_n`

Provede N instrukcí jako sekvenci `emu_step_into`. Argument:

- `count` (int, 1..1000) - počet kroků

Pokud kterýkoliv step selže mid-loop (= BP trefený, jiný důvod
pro pause), tool vrátí partial counter s `partial=true` flagem
v odpovědi. Návrat: `{"count": <int>, "requested": <int>,
"partial": <bool>}`. Tool se vrátí po provedení posledního kroku.

### `emu_run_until_addr`

Nastaví dočasný BP na cílovou adresu a spustí emulaci. Emu se
zapauzuje, jakmile PC dosáhne `addr` (nebo zatím trefí jiný
permanentní BP). Argumenty:

- `addr` (int, 0..65535) - cílová PC adresa
- `max_cycles` (int, volitelný, default 10 mil.) - jen informativní,
  vrací se zpět, nic neomezuje
- `deadline_frames` / `deadline_ms` / `deadline_ticks` (int, volitelný,
  nejvýš jeden) - zastaví běh v emulovaném čase, když `addr` nedosáhne
  (důvod `"deadline"`), viz `emu_resume`

Odpověď `{"addr", "max_cycles", "running": true, "run_seq"}` (plus
`deadline_ticks`); na zastavení se čeká přes `emu_wait_stop(run_seq)`.

Vyžaduje pause stav. Na běžícím emulátoru vrátí chybu
`Emulator is running: run_until_addr needs a paused emulator ...`
a emulátor běží dál beze změny (čerstvě spuštěný emulátor běží, proto
nejdřív `emu_pause`).

Když PC dosáhne `addr`, emulátor vyšle událost `paused`
s `{"reason": "run_to", "pc": <addr>, "run_seq": ...}` (topic `paused`,
viz `emu_event_subscribe`), takže klient může čekat přes `emu_wait_stop`
nebo `emu_event_poll` místo pollování `emu_status`. Zastaví-li emulaci dřív trvalý breakpoint,
nebo trvalý breakpoint na `addr`, je událost jeho (`reason: "breakpoint"`),
nikdy obě. Trvalý breakpoint na `addr`, který nezastaví (nesplněná
podmínka, `skip_count`, akce s pokračováním), cíl nezakryje: běh na `addr`
přesto zastaví s `reason: "run_to"`.

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

Vyžaduje pause stav (čerstvě spuštěný emulátor běží, proto nejdřív
`emu_pause`). Chyba uvádí důvod, např.
`snapshot_load failed: Emulator is not paused (pause it first, e.g. emu_pause)`
nebo `snapshot_load failed: I/O error (result_code 1)`.

Odpověď: `{"path": <str>, "ok": true, "result_code": 0}`.

### `emu_snapshot_load_buffer` (sensitive)

Načte snapshot z inline base64 bufferu (typicky vyrobeného předchozím
`emu_snapshot_save_buffer`). Argument:

- `bytes_b64` (string) - base64-encoded .mzs ZIP

Vyžaduje pause stav, stejně jako `emu_snapshot_load` (stejné texty chyb).

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

- `max_cycles` (int, volitelný, default 10000000) - jen informativní,
  vrací se zpět, nic neomezuje
- `deadline_frames` / `deadline_ms` / `deadline_ticks` (int, volitelný,
  nejvýš jeden) - zastaví běh v emulovaném čase, když se funkce
  nevrátí (důvod `"deadline"`), viz `emu_resume`

Odpověď: `{"return_addr": <int>, "max_cycles": <int>, "running": true,
"run_seq": <int>}` (plus `deadline_ticks`) nebo error pokud callstack
neaktivní / prázdný stack / emu už běží. Dosažení `return_addr` vyšle
událost `paused` s `reason: "run_to"` (viz `emu_run_until_addr`); čekat
přes `emu_wait_stop(run_seq)`.

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

Smyčka běží uvnitř jednoho požadavku a nejde zrušit; skončí nejpozději
po 20 s času hostitele (`time_limited: true`), aby odpověď přišla před
30 s limitem transportu. Emulace pak stojí v pauze tam, kam smyčka
došla.

Odpověď: `{"scanline": <int>, "column_pixel": <int>, "frame_number":
<int>, "total_cycles": <int>, "delta_cycles": <int>, "reached":
<bool>, "time_limited": <bool>}`. `reached=false` znamená, že smyčka
skončila na bezpečnostním stropu nebo na časovém limitu.

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

Stejně jako `emu_run_until_raster` skončí smyčka nejpozději po 20 s
času hostitele (`time_limited: true`).

Odpověď: `{"total_cycles": <int>, "target": <int>, "delta_cycles":
<int>, "reached": <bool>, "time_limited": <bool>}`.

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

Čekání skončí nejpozději po 20 s času hostitele (`time_limited: true`;
u `breakpoint_hit` je emulace pak v pauze). Na dlouhé čekání použij
`emu_resume` s deadlinem a `emu_wait_stop`.

Odpověď: `{"kind": <str>, "reached": <bool>, "delta_cycles": <int>,
"time_limited": <bool>, ...}` plus kind-specific fields (`frames_done`
pro `frame_done`, `paused` pro `breakpoint_hit`).

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
- `expected_sha256`, `want_hashes` (volitelné) - podmíněný zápis jako
  u `emu_mem_write`

Vrací `{"addr": <int>, "length": <int>}` (s podmínkou nebo
`want_hashes` navíc `before_sha256` / `after_sha256`). Pro skutečný "patch ROM"
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

Lifecycle ovládání 5 binárních kanálů trace-suite (`cputrack`,
`iorqlog`, `intlog`, `hwlog`, `marklog`) za běhu. Rozhraní zrcadlí CDL tooly -
umožňuje AI klientovi ohraničit záznam přesně kolem sledovaného úseku
místo ukládání až při ukončení emulátoru. Detaily formátu výstupu viz
[Trace Suite](../debugger/Trace_Suite.md).

Všechny 4 tooly mají povinný argument:

- `channel` (string, povinný) - jeden z `cputrack` / `iorqlog` /
  `intlog` / `hwlog` / `marklog` (`marklog` zaznamenává značky z akce
  breakpointu `mark`).

### `emu_trace_start`

Spustí záznam kanálu (= režim ALWAYS). Argument `regs` (bool, volitelný,
jen cputrack): `true` zaznamená po každé instrukci registry CPU (formát 3,
32 B na záznam místo 12), `false` formát 2; bez něj platí volba
z konfigurace (`[TRACE_CPUTRACK] regs`). Když cputrack už nahrává v druhém
formátu, volání selže (nejdřív ho zastavte). Vrací `{"started": true}`.

### `emu_trace_stop`

Zastaví záznam kanálu; segment se uzavře (flush + close). Vrací
`{"stopped": true}`.

### `emu_trace_reset`

Zapíše rozpracovaný chunk a manifest aktuálního segmentu kanálu na disk;
záznam pokračuje ve stejném segmentu a nic zaznamenaného se nezahodí.
U cputrack navíc vynuluje HALT/self-loop collapse: T-stavy sbaleného
běhu, které ještě nejsou zapsané, zahodí a další instrukce začne nový
záznam. Samostatný segment založí `emu_trace_save`
s novou cestou, nebo `emu_trace_stop` a `emu_trace_start`. Vrací
`{"reset": true}`.

### `emu_trace_save`

Uloží / přesměruje segment kanálu. Argument:

- `path` (string, volitelný) - cílová cesta NÁSLEDUJÍCÍHO segmentu. Bez
  `path` se jen uzavře a znovuotevře aktuální segment na stávající
  dir/name (= "ulož teď").

Vrací `{"saved": true, "path": <str|null>}`.

### `emu_trace_status`

Stav kanálů pro čtení. Argument `channel` (volitelný; jeden z 5 trace
kanálů nebo `eventlog` pro ring Event Vieweru; prázdný = všechny). Vrací
`{"channels": [...]}` s položkami: `segment` (0 = zatím nic
nezaznamenáno), `live`, `truncated`, `first_seq`, `next_seq`, `lost`,
`record_size`, `format_version` (cputrack 2, s registry 3), `chunks`,
`bytes_on_disk`, `dir`, `name` a `has_time`; u cputrack navíc `has_regs`
(segment s registry) a `initial_regs` (stav CPU na začátku posledního
segmentu, celá čísla jako v manifestu; před prvním záznamem null).

### `emu_trace_read`

Stránka záznamů kanálu (`cputrack`, `iorqlog`, `intlog`, `hwlog`,
`marklog`, `eventlog`, `ramlog` (záznam zápisů do paměti), `bplog` (výstup akcí BP `log` a `stop`) a z paměti zachycení zvuku `audio_psg_writes`,
`audio_levels`, `audio_frames`), i za běhu záznamu - čitelná jsou i data, která
jsou zatím jen v RAM bufferu. Po `emu_trace_stop` zůstane poslední
segment čitelný až do dalšího startu. Argumenty:

- `channel` (string, povinný).
- `cursor` (string) - `next_cursor` předchozí stránky; má přednost před
  `start`.
- `start` (string, výchozí `oldest`) - `oldest`, `newest` (posledních
  `limit` záznamů) nebo `seq:N`.
- `limit` (int, výchozí 200, 1-5000) - nejvýš řádků; u `raw_b64` počet
  záznamů (0 = kolik se vejde do 1 MiB).
- `format` (string) - `rows` (výchozí, dekódované) nebo `raw_b64`.
- `columns` (seznam) - výběr sloupců kanálu (u cputrack výchozí bez
  `kind` a registrů).
- `regs` (string, cputrack) - `after` (výchozí) nebo `before`: který stav
  ukazují sloupce a filtry registrů (`before` = předchozí záznam; null, kde
  není znám).
- `filter` (objekt) - `pc_from`/`pc_to` (cputrack), jména registrů
  `af` .. `halted` s hodnotou nebo `[lo, hi]` (cputrack s registry),
  `port` (<= 0xFF
  porovná dolní bajt), `dir` (`in`/`out`), `type` (`iorq`,
  `mreq_mapped`, `iorq_unconnected`) (iorqlog), `chip`/`sub` (hwlog),
  `marker` (id značky marklog), `channels` (seznam jmen kanálů `ctc0`,
  `psg0_0` .. `psg1_3`; audio_levels, audio_psg_writes), `reg` (`tone_lo`,
  `tone`, `volume`, `noise`; audio_psg_writes), `t_from`/`t_to` (kanály se
  sloupcem `t`); ramlog: `pc_from`/`pc_to` (zapisující instrukce),
  `addr_from`/`addr_to` (CPU adresa), `space`, `src`, `all` (i zápisy,
  které buňku nezměnily, a zápisy pod ROM - ve výchozím stavu skryté);
  bplog: `pc_from`/`pc_to` (`insn_pc`), `bp` (číslo breakpointu, nebo
  jeho přesné jméno jako řetězec), `kind` (`log` nebo `stop`).
- `max_chars` (int, výchozí 20000, 1000-45000) - rozpočet znaků celé
  odpovědi `rows`.
- `max_scan` (int, výchozí 2000000) - nejvýš prohledaných záznamů na
  dotaz.

Sloupce (vždy v tomto pořadí, `columns` v odpovědi):

| Kanál | Sloupce |
|-------|---------|
| `cputrack` | `seq`, `pc`, `bytes`, `asm`, `t_std`, `wait`, `t`, `sym` (výchozí); na požádání `kind`, `af`, `bc`, `de`, `hl`, `ix`, `iy`, `sp`, `wz`, `i`, `r`, `iff1`, `iff2`, `im`, `halted` |
| `iorqlog` | `seq`, `t`, `frame`, `pos`, `type`, `dir`, `src`, `addr`, `value`, `pulse` |
| `intlog` | `seq`, `t`, `frame`, `pos`, `class`, `chip`, `pin`, `edge`, `state` |
| `hwlog` | `seq`, `t`, `frame`, `pos`, `chip`, `sub`, `payload` |
| `marklog` | `seq`, `t`, `frame`, `pos`, `id`, `name` |
| `eventlog` | `seq`, `t`, `frame`, `pos`, `cat`, `sub`, `pc`, `text` |
| `audio_psg_writes` | `seq`, `t`, `time_us`, `frame`, `psg`, `byte`, `ch`, `reg`, `value`, `hz`, `note` |
| `audio_levels` | `seq`, `t`, `time_us`, `frame`, `channel`, `level` |
| `audio_frames` | `seq`, `t`, `time_us`, `len_ticks`, `edges`, `on_pct`, `rms_level`, `max_level`, `ref_rms`, `ref_peak`, `sdl_rms`, `sdl_peak`, `sdl_blocks` |
| `ramlog` | `seq`, `t`, `frame`, `pc`, `sym`, `addr`, `space`, `offset`, `plane`, `old`, `new`, `src`, `flags`, `count`, `tin` |
| `bplog` | `seq`, `t`, `frame`, `insn_pc`, `sym`, `pc`, `bp`, `name`, `kind`, `text`, `flags` |

`t` je v taktech GDG od startu emulace (cputrack: začátek instrukce,
jen s `has_time`); `asm` u cputrack je disassembly z bajtů záznamu,
záznam přijetí přerušení ukazuje `INT ACK`. `kind` u cputrack je `insn`,
`loop`, `int` nebo `sync`; sloupce registrů (16bitové hex, `i` / `r`
8bitové hex, příznaky jako čísla) potřebují segment nahraný s registry,
jinak volání selže. Řádek `sync` (`asm` `SYNC start`, `SYNC pc_range`,
`SYNC write`) nese stav CPU na začátku záznamu, při návratu do rozsahu
filtru PC a po změně mimo instrukci; čas nezabírá. `audio_*`: `t` absolutně,
`time_us` od začátku zachycení; `hz` / `note` jen u nového děliče tónu;
pole `audio_frames` jsou v pořadí `channels` z `emu_audio_capture_status`,
`ref_*` / `sdl_*` jsou [L, R] int16 (bez zachycení null).

Odpověď nese `segment`, `first_seq`, `next_seq`, `live`, `truncated`,
`lost`, `gap` (ring Event Vieweru: `{from_seq, to_seq}`, když kurzor
ukazuje před `first_seq`), `scanned`, `matched`, `scan_exhausted`
(došel limit `max_scan` - pokračovat kurzorem), `page_full`,
`end_of_data` (stránka došla k `next_seq`), `columns` + `rows` (nebo
`record_size`, `format_version`, `from_seq`, `count`, `data_b64`)
a `next_cursor`. Kurzor ze staršího segmentu (kanál se znovu spustil,
ring Event Vieweru se vymazal) vrátí chybu začínající
`segment_replaced:`.

### `emu_bplog_clear`

Kanál `bplog` drží text akcí breakpointů `log "..."` a `stop "..."`
(tentýž, který emulátor vypisuje jako `[BP-LOG]` a `[BP-STOP]`), aby ho
MCP klient přečetl přes `emu_trace_read(channel="bplog")`. Běží vždy,
nic se nezapíná; dokud žádná akce nic nevypsala, je prázdný (`next_seq`
0) a kurzor vzatý v tu chvíli platí i pro pozdější řádky.
`t`, `frame`, `insn_pc` a `pc` jsou hodnoty `Cycle`, `Frame`, `InsnPC`
a `PC` ve výrazu breakpointu v okamžiku akce, `sym` je symbol `insn_pc`,
`bp` číslo a `name` jméno breakpointu, `kind` `log` nebo `stop`. Text
delší než 239 bajtů se ořízne (`flags` `text_truncated`). Kanál drží
posledních 4096 řádků (starší `lost`, `gap`); reset ani načtení
snapshotu ho nevyprázdní.

`emu_bplog_clear` (bez argumentů) kanál vyprázdní a založí nový segment
(starý kurzor vrátí `segment_replaced:`). Vrací `segment`, `allocated`
(`false` = ještě nic zapsáno), `capacity`, `first_seq`, `next_seq`.

## Nástroje záznamu zápisů do paměti

Záznam zápisů do paměti ([RAM Write Log](../debugger/ram-write-log.md)):
kdo zapsal bajt, který kód zapisuje do oblasti. Každý záznam nese čas `t`
(takty GDG na začátku zapisující instrukce, stejně jako `t` cputrack),
`tin` (čítač T-stavů instrukce v okamžiku zápisu, jen `insn` - přibližná
poloha), `frame`, adresu zapisující instrukce `pc`, CPU adresu
`addr`, buňku (`space` + `offset`: `ram`, `vram`, `pcg`, `memext`, `rom`
= zápis pod ROM, zahozený), `old` / `new` a zdroj `src` (`insn`,
`int_ack` / `nmi_ack` = návratová adresa uložená při přijetí přerušení,
`pc` = ta návratová adresa, `debugger`, `poke`, `freeze`, `cmt_hack`,
`load` = jeden souhrnný záznam nahraného bloku). Reset a načtení snapshotu
záznam vyprázdní. Postup je v MCP dokumentu `emulator://docs/ramlog`.

### `emu_ramlog_start`

Spustí záznam; běžící záznam se nahradí. Argumenty:

- `ranges` (seznam, volitelný, nejvýš 16) - `{"from": 0x4000, "to": 0x40FF}`
  = CPU adresy v okamžiku zápisu (`to` výchozí = `from`),
  `{"space": "vram", "from": 0, "to": 0x1FFF}` = buňky prostoru (`ram`,
  `vram`, `pcg`, `memext`). Prázdný = celá paměť (pomalejší).
- `mode` (string, výchozí `ring`) - `ring` drží nejnovější záznamy,
  `fill` prvních `capacity` záznamů (zbytek se počítá do `dropped`).
- `capacity` (int, výchozí 1048576, 16-16777216) - položek po 32 B.

Vrací stav (viz `emu_ramlog_status`).

### `emu_ramlog_stop`, `emu_ramlog_clear`

Zastaví záznam (záznamy zůstanou čitelné do dalšího startu) / vyprázdní
ho (nový segment, běžící záznam pokračuje). Vrací stav.

### `emu_ramlog_status`

`{"active", "allocated", "segment", "mode", "capacity", "first_seq",
"next_seq", "total", "dropped", "overwritten", "memory_bytes",
"ranges"}`. `first_seq` > 0 = ring přepsal nejstarší záznamy.

### `emu_ramlog_who_wrote`

Poslední zapisovatel a historie zápisů buňky. Argumenty: `addr` (CPU
adresa podle aktuálního mapování; grafická VRAM MZ-800 dá 4 plány) nebo
`space` + `offset`; `limit` (0-1000, výchozí 20 záznamů na buňku);
`include_unchanged` (v JSONL `all`, výchozí false - i zápisy, které buňku
nezměnily).

Vrací `{"active", "segment", "mmio", "cells": [{"space", "offset",
"plane"?, "last": {"seq", "pc", "src", "in_ring"} | null, "history":
[{"seq", "t", "frame", "pc", "addr", "old", "new", "src", "tin"?, "count"?,
"unchanged"?, "suppressed"?}]}]}`, historie od nejnovějšího. `last`
pochází z mapy posledních zapisovatelů a drží se, i když ring záznam už
přepsal (`in_ring: false`). `mmio: true` = adresa je port mapovaný do
paměti, žádná buňka.

### `emu_ramlog_writers`

Instrukce, které zapisovaly do rozsahu, seskupené podle zapisující
instrukce a seřazené podle počtu zápisů. Argumenty: `from_addr`,
`to_addr` (výchozí `from_addr`) - CPU adresy v okamžiku zápisu, nebo
offsety v `space`; `limit` (1-5000, výchozí 50); `include_unchanged`.
Vrací `{"active", "segment", "total", "truncated", "writers": [{"pc",
"src", "count", "first_seq", "last_seq"}]}`.

Samotné záznamy se čtou přes `emu_trace_read(channel="ramlog")`.

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

## Nástroje záznamu a přehrání vstupů

Vstupy jednoho běhu se jednou nahrají a pak přesně přehrají - v témže
nebo novém procesu, při libovolné rychlosti - a chybu jde opakovat
s jinými breakpointy, trace nebo záznamem zápisů do paměti, případně
ověřit běh v CI. Celý návod je v [../replay.md](../replay.md)
a v dokumentu MCP `emulator://docs/replay`.

Soubor záznamu (`.mzr`) obsahuje kompletní startovní snapshot (vždy
s RAMdiskem), obsah vložených disků a pásky (dohromady nejvýš 64 MB,
větší obrazy jen jako SHA-256), každou změnu klávesnicové matice a stavu
joysticku na hranici snímku (joystick hostitele se čte jednou za snímek),
resety, autotype, ovládání kazety, zápisy debuggeru a každých N snímků
SHA-256 částí `cpu` (registry a skrytý stav Z80), `ram`, `memext`,
`vram`, `pcg` a `periph` (CTC, PIO 8255, Z80 PIO, PSG, GDG, řadič VRAM,
řadiče disket a Quick Disku) spolu s emulovaným časem. Přehrávání
pracuje nad kopiemi médií, hashe porovnává a zastaví se na prvním
snímku, který se liší.

### `emu_replay_record_start` (sensitive - vytvoří / přepíše soubor)

Argumenty: `path` (string, povinný), `hash_interval` (int 1..100000,
výchozí 50 snímků), `description` (string, volitelný),
`checkpoint_interval` (int 0..100000 snímků, 0 = bez checkpointů,
výchozí ze sekce `[REPLAY]` INI: 250), `checkpoint_count` (int 1..1000,
výchozí z INI: 60). Když emulace běží, pozastaví se na nejbližší hranici
snímku, záznam začne a emulace běží dál; v pauze zůstane v pauze.

Každých `checkpoint_interval` snímků vznikne checkpoint (snapshot
v paměti, desítky kB), drží se posledních `checkpoint_count`; viz
`emu_replay_rewind`.

Během záznamu se klávesnice z GUI převezme na nejbližší hranici snímku.
Zápisy debuggeru (`emu_mem_write`, `emu_mem_write_force`, zápis do
regionu, `emu_set_register`, příznaky CPU, `emu_io_write`,
`emu_irq_inject`, `emu_nmi_inject`) a vstupní nástroje se zaznamenají
za běhu (převzaté na hranici snímku) a v pauze **na** hranici snímku (po
`emu_run(frames=N)` nebo `deadline`, nebo po pauze poslané za běhu).
V pauze **mezi** hranicemi snímků (po kroku nebo breakpointu) se odmítnou
s vysvětlením. Ovládání kazety (`emu_cmt_play`, `emu_cmt_play_paused`,
`emu_cmt_stop`, `emu_cmt_pause`, `emu_cmt_eject`, `emu_cmt_tape_seek`,
`emu_cmt_record`) se zaznamená stejně. Nástroje, které stroj mění tak,
že to záznam nezopakuje, se odmítnou: načtení snapshotu, média, vložení
pásky (`emu_cmt_open`), rychlost, pulzy, polarita a boost pásky, CMT
hack, rychlost bloku, nastavení, platforma, periferie, mapa paměti,
`emu_io_read` (vedlejší efekty), `emu_freeze_add`. Start se odmítne při
běžícím autotype, aktivních Freeze Bytes nebo nahrávající kazetě. Zapisující akce breakpointů (`poke`, `set`) se
nezaznamenají: manifest dostane varování.

Vrací stav (viz `emu_replay_status`).

### `emu_replay_record_stop`

Ukončí záznam a zapíše soubor. Na hranici snímku končí záznam hashem
konečného stavu; v pauze mezi hranicemi končí na poslední hranici. Vrací
stav s `outcome: "recorded"` a `outcome_message` (snímky, události,
soubor), nebo chybu (`no recording is running`, chyba zápisu).

### `emu_replay_play` (sensitive - nahradí stav emulátoru)

Argumenty: `path` (string, povinný), `pause_after_load` (bool, výchozí
false), `checkpoint_interval` a `checkpoint_count` (jako u
`emu_replay_record_start`; checkpointy přehrávání zrychlí seek zpět).
Načte startovní snapshot a přehraje zaznamenané vstupy;
`pause_after_load: true` po načtení zůstane v pauze (nastavit
breakpointy, pak `emu_resume`). Živý vstup (klávesnice GUI, virtuální
klávesnice, klávesy z MCP) se ignoruje. Konec pozastaví emulaci
s důvodem `replay_end` (`outcome: "ended"`), první rozdílný hash
s důvodem `replay_desync` (`outcome: "desync"`, `outcome_frame`,
`desync_parts`: `cpu`, `ram`, `memext`, `vram`, `pcg`, `periph`,
`time`). Zápis do stroje během přehrávání (zápis debuggeru, akce
breakpointu `poke` / `set`, Freeze Bytes, reset, načtení snapshotu,
ovládání kazety) přehrávání označí jako `deviated`: pokračuje, ale hashe
se dál neověřují. Uložená média se nasadí z kopií (původní soubory
a INI se nemění, po konci se vrátí média podle INI). Chyby: nečitelný
soubor, jiná platforma nebo TV norma, záznam s událostmi debuggeru
v buildu bez debuggeru, port joysticku připojený jen v jednom z nich,
jinak nastavený CMT hack, neuložený obraz, který se liší od vloženého.

### `emu_replay_play_stop`

Ukončí běžící přehrávání (`outcome: "stopped"`). Chyba: `no playback is
running`.

### `emu_replay_rewind` (sensitive - nahradí stav emulátoru)

Argument: `frames` (int >= 0, výchozí 0). 0 = o jeden checkpoint zpět
(nejnovější před aktuálním snímkem; další volání jde dál), N = nejnovější
checkpoint aspoň o N snímků zpět. Při záznamu se stroj vrátí na
checkpoint, vstupy a hashe po něm se zahodí, `rerecord_count` se zvýší
o jedna a záznam pokračuje odtud; emulace zůstane stát na hranici snímku
checkpointu. Při přehrávání jde o seek zpět na ten checkpoint (viz
`emu_replay_seek`).

Checkpointy nenesou obsah médií. Checkpoint, po kterém se zapsalo na
disk (FDC, Quick Disk, IDE8) nebo na kazetu nebo se páska vysunula, použít
nejde: při záznamu se rewind odmítne, při přehrávání začne přehrávání
znovu od začátku. Při běžícím autotype ani při nahrávání na kazetu
checkpoint nevznikne. Chyby: `checkpoints are off`, `no checkpoint at or
before frame N (the oldest kept is frame M)`, `a disk or the tape was
written or ejected after frame N; rewinding past a media change is not
possible`, `nothing is recording or playing`. Vrací stav.

### `emu_replay_seek` (sensitive - nahradí stav emulátoru)

Argument: `frame` (int >= 0, povinný; za koncem se omezí na konec).
Dopředu: přehrávání pokračuje v MAX SPEED a zastaví na hranici snímku
`frame` s důvodem pauzy `replay_seek` (událost `paused`,
`emu_wait_stop`); rychlost před seekem se vrátí. Zpět: načte se
nejnovější použitelný checkpoint do `frame` (jinak přehrávání začne
znovu od začátku s čistými kopiemi médií) a zbytek se dohraje v MAX
SPEED. Hashe se ověřují celou dobu; breakpointy zůstávají aktivní a mohou
seek zastavit dřív. Odpověď přijde hned po spuštění: `seeking: true` =
dohrávání na `seek_target` běží, `false` = emulace už stojí na snímku.
Chyba: `seek works only during playback`. Vrací stav.

### `emu_replay_status`

Bez argumentů. Vrací `{"mode": "off" | "recording" | "playing",
"frame", "frames" (délka při přehrávání), "events", "hashes",
"hash_interval", "warnings", "deviated", "deviation", "path", "outcome":
"none" | "recorded" | "ended" | "desync" | "stopped" | "error",
"outcome_frame", "outcome_message", "desync_parts": [...],
"rerecord_count", "seeking", "seek_target", "checkpoints": {"interval",
"capacity", "count", "first_frame", "last_frame", "bytes"}}`. Členy
`outcome*` popisují poslední skončený záznam nebo přehrávání;
`first_frame` a `last_frame` jsou jen při `count` > 0.

## Nástroje screenshotu

PNG obrázky obrazovky emulátoru zapsané přímo do souboru na počítači se
serverem (v odpovědi není velký base64 obsah). Postup s recepty je
v MCP dokumentu `emulator://docs/screenshot_workflow`; parametry popisuje
i [headless-mode.md](headless-mode.md).

### `emu_frame_hash`

SHA-256 obrazu a video paměti bez jejich přenosu. Bez argumentů.
`image_sha256` je hash obrazu ze stejného zdroje jako screenshot: index
palety každého bodu po řádcích (`width` x `height` bajtů, INDEX8 & 0Fh),
za nimi 16 barev palety po bajtech R, G, B. `vram_sha256` je hash video
paměti (MZ-800: všechny čtyři plány; MZ-700 / MZ-1500: znaková
a atributová VRAM). Obraz závisí i na registrech GDG (paleta, režim,
border), hash VRAM ne. V pauze uprostřed snímku je část obrazu ještě
z minulého snímku - odpověď nese pozici paprsku (`beam`: `frame`,
`scanline`, `column`) a `paused`. Resources screenshotu
(`emulator://frame/screenshot.raw`, `emulator://frame/screenshot`)
vracejí stejný `image_sha256` (u PNG pro celý framebuffer bez ohledu na
oblast a měřítko). Vrací `{"available", "image_sha256", "vram_sha256",
"source_screen_id", "image_source", "width", "height", "beam",
"paused"}`.

### `emu_screenshot_save_to_file` (citlivý - vytvoří / přepíše soubor)

Argumenty:

- `path` (string, povinný) - PNG soubor k zápisu.
- `format` (string, výchozí `png`) - podporované je jen `png`.
- `area` (string, výchozí `full`) - `full` = celý obraz s borderem,
  `canvas` = jen obraz bez borderu.
- `scale` (string, výchozí `framebuffer`) - `framebuffer` = pixely
  framebufferu 1:1 (MZ-800 celý obraz 928x288); `native` = jeden pixel
  obrázku na bod počítače (MZ-800 obraz 320x200, v režimech 640 bodů
  640x200); `"1"` až `"4"` = poměr stran okna emulátoru, `"2"` = okno na
  100 % (MZ-800 celý obraz 928x576).
- `crop` (objekt, volitelný) - `{"x", "y", "width", "height"}` ve
  výstupních pixelech, jen s `framebuffer` nebo `native`.

Vrací `{available, path, format, width, height, byte_size, area, scale}`
(+ `hires` u `native`), před prvním vykresleným snímkem `{available:
false, reason}`. Neplatná `area`, `scale` nebo `crop` (i ořez mimo obraz)
je chyba parametrů a soubor se nezapíše.

### `emu_screenshot_series_start` (citlivý - vytváří soubory)

Ukládá PNG každého emulovaného snímku, který se liší od posledního
uloženého - pro hledání chyb v animacích. Argumenty (všechny volitelné):
`dir` (složka série; prázdný = nová složka
`<platforma>_frames_RRRRMMDD_HHMMSS` ve výstupní složce screenshotů),
`area`, `scale` (výchozí `native`), `crop` - jako výše, počítají se jen
změny uvnitř nich; `only_changed` (výchozí `true`; `false` = každý
snímek), `frames` (stop po N emulovaných snímcích, 0 = do stop),
`max_files` (pojistka, 0 = 10000).

Snímky se berou v emulovaném čase na konci každého snímku - ani při MAX
SPEED se žádný nevynechá; když zápis nestíhá, emulace počká. V pauze
emulace se nic nesnímá. Soubory `frame_NNNNNNN.png` (NNNNNNN = emulovaný
snímek od startu, mezery = nezměněné snímky) a `index.csv`
(`frame,file,gdg_screen`). Vrací stav série a navíc `area`, `scale`,
`only_changed`. Chyby: série už běží, neplatné parametry, složku nelze
vytvořit.

### `emu_screenshot_series_stop`

Argument `wait` (bool, výchozí `true`): počkat (nejvýš 30 s), až se
zapíšou všechny snímky ve frontě. Vrací stav série. Bez série:
`No frame series is running`.

### `emu_screenshot_series_status`

Bez argumentů. Vrací `active` (snímá), `writing` (soubory se ještě
zapisují), `frames_seen`, `files`, `unchanged`, `dir`, `stop_reason`
(`Stopped`, `Frame count reached`, `File limit reached`, `Error`, za běhu
prázdný) a `last_error`.

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

## Profiler podle PC

Profiler podle PC počítá každou vykonanou instrukci podle její adresy
(logické PC): počet vykonání, T-stavy včetně čekacích stavů a samotné
čekací stavy (přístup do VRAM, zápis do PSG). Na rozdíl od profileru po
funkcích výše nepotřebuje párování CALL/RET, takže vidí i kód, na který
se skáče, obsluhy přerušení a samostatné smyčky - obvykle první krok
při optimalizaci programu.

- Blokové instrukce (LDIR, LDDR, ...) se počítají při každém opakování
  jako jedno vykonání (CPU je načítá znovu).
- Paměťové banky se nerozlišují: kód na stejné adrese v různých bankách
  se sčítá.
- Čas mimo instrukce se vede zvlášť: `halt_idle` (CPU čeká v HALT,
  samotná instrukce HALT se počítá na své adrese) a `other_tstates`
  (hlavně přijetí přerušení). Vždy platí
  `tstates + halt_idle + other_tstates == elapsed_tstates`.
- Režie: jedno volání funkce na instrukci, dokud sběr běží. Se
  zastaveným profilerem se emulace nezpomalí.

Typické použití:

```
emu_profiler_pc_start()          # smaže předchozí data
emu_run(frames=50)               # jedna sekunda MZ-800 (PAL)
emu_profiler_pc_get(limit=20)    # kam čas odešel
emu_profiler_pc_stop()
```

Všech šest nástrojů vrací souhrn `{"active", "instructions", "tstates",
"wait", "halt_idle", "other_tstates", "elapsed_tstates", "unique_pcs"}`
(plus položky uvedené níže).

### `emu_profiler_pc_start`

Spustí sběr. Argument `reset` (bool, výchozí true): nejdřív smazat
předchozí data; false pokračuje v přičítání.

### `emu_profiler_pc_stop`

Zastaví sběr; data zůstanou pro `emu_profiler_pc_get`
a `emu_profiler_pc_export`.

### `emu_profiler_pc_reset`

Smaže data. Zda profiler běží, se nemění.

### `emu_profiler_pc_get`

Nejvytíženější adresy inline. Argumenty (všechny volitelné):

- `sort` - `tstates` (výchozí), `count`, `wait` nebo `addr`,
- `limit` - počet řádků, 1..1000 (výchozí 50),
- `from_addr`, `to_addr` - rozsah adres včetně (výchozí 0..65535).

Přidá `sort`, `total_rows` (vykonané adresy v rozsahu, může být víc než
`limit`) a `rows`:

```json
{"pc": 33667, "count": 109893, "tstates": 2307503, "wait": 0,
 "tstates_pct": 64.79, "avg_tstates": 21.0,
 "symbol": "fill_loop", "sym_offset": 3}
```

`tstates_pct` je podíl na T-stavech všech instrukcí. `symbol` je
nejbližší načtený symbol na adrese `pc` nebo pod ní (pohled CPU, jako
`emu_symbol_lookup`), `""` když žádný není.

### `emu_profiler_pc_export`

Zapíše všechny vykonané adresy do CSV. Argumenty: `path` (povinný,
přepíše se; složka musí existovat) a `sort` (výchozí `addr`). CSV:
UTF-8, konce řádků LF, desetinná tečka bez ohledu na locale; hlavička
`pc,count,tstates,wait,tstates_pct`, `pc` jako 4 hex číslice. Přidá
`path` a `rows`.

### `emu_profiler_pc_bitmap`

Vykonané adresy jako bitmapa 8 KiB - pokrytí kódu, které se dá číst
často (třeba po každém kroku automatického průchodu) bez přenosu řádků.
Bez argumentů. Přidá `format` (`pc-bitset-lsb0`), `map_bytes` (8192)
a `bitmap_base64`.

Bit `adresa & 7` (bit 0 = nejnižší) bajtu `adresa >> 3` je 1, když na
adrese začala aspoň jedna vykonaná instrukce. Jsou to tytéž adresy jako
řádky `emu_profiler_pc_get` a `emu_profiler_pc_export`, počet
nastavených bitů je `unique_pcs`. Operandy bit nedostanou, nečinný HALT
nic nepřidá, bloková instrukce označí svůj začátek; banky ani verze
samomodifikovaného kódu na stejné adrese se nerozlišují. Funguje za
běhu sběru i po `emu_profiler_pc_stop`, emulaci nepozastaví ani
neposune.

## Pokrytí kódu

Pokrytí kódu odpovídá, které instrukce se vykonaly, kterým směrem šla
každá podmíněná instrukce (`JR` / `JP` / `CALL` / `RET` s podmínkou,
`DJNZ`, `LDIR` / `CPIR` / `INIR` / `OTIR` a sestupné varianty) a na
úrovni `detail` i která verze kódu se kde vykonala. Výsledek podmínky se
odvozuje ze stavu CPU po kroku (příznaky, B, PC zpět na opakující se
blokové instrukci), takže `JR Z,$+2` se započte správně, i když oba
výsledky skončí na stejné adrese.

- `branch` - podle logického PC: vykonané adresy, podmíněné instrukce
  a jejich výsledky, první instrukce obsluhy přerušení. Naměřená cena v MAX SPEED: asi +16 až +29 % času emulace.
- `detail` - navíc jedna jednotka na (fyzickou buňku prvního bajtu, PC,
  verzi kódu) s čítači `exec`, `taken`, `not_taken`, posledním operandem
  a posledním následujícím PC. Stejné PC v RAM a v ROM nebo v jiné bance
  MemExt je jiná jednotka; samomodifikovaný operand zůstává jednou
  jednotkou s příznakem `operand_varied` (klíč `opcode`), s klíčem
  `full_bytes` je každá hodnota operandu samostatná jednotka. Bajty se
  berou tak, jak je CPU načetlo, což potřebuje pomalejší cestu přístupu
  do paměti. Naměřená cena: asi +35 až +95 % času emulace.
- Nečinný HALT se nepočítá; reset, načtení snapshotu a skok
  v přehrávání záznamu data ponechají a zvýší `discontinuities`.
- Stejný sběr jde spustit z příkazové řádky (`--coverage`,
  `--coverage-save-on-exit`, viz [Headless režim](headless-mode.md)),
  ze sekce INI `[COVERAGE]` a z akce breakpointu (`coverage_start`,
  `coverage_stop`, `coverage_mark`).

Postup pro AI klienty: `emulator://docs/coverage`. V GUI: záložka
Coverage v okně CPU Profiler a značky v oknech Disassembly, viz
[Pokrytí kódu](../debugger/code-coverage.md); lcov přes
`docs/tools/coverage_to_lcov.py`.

Typické použití:

```
emu_coverage_start(level="branch", reset=true)
emu_run(frames=500)
emu_coverage_read(filter={"state": "partial"})   # větve, které šly jen jedním směrem
emu_coverage_stop()
```

Všechny nástroje vracejí stav `{"active", "has_data", "level", "key",
"reset_on_discontinuity", "complete", "units", "capacity", "dropped",
"fetch_fallback", "saturated", "discontinuities", "host_state_edits",
"epoch", "generation", "instructions", "halt_idle", "branches":
{"decisions", "both", "taken_only", "not_taken_only"}}` (plus položky
níže). `complete` je false, když se jednotka nevešla (`dropped`), bajt
nebyl v záznamu načtení instrukce (`fetch_fallback`) nebo čítač
saturoval.

### `emu_coverage_start`

Argumenty (všechny volitelné): `level` (`branch` výchozí, `detail`),
`key` (`opcode` výchozí, `full_bytes`; jen detail), `capacity` (jednotky
detail, 1..1048576, výchozí 65536, každá 32 bajtů), `reset` (výchozí
false: pokračovat do dat se stejným nastavením), `reset_on_discontinuity`
(výchozí false).

### `emu_coverage_stop`, `emu_coverage_reset`, `emu_coverage_status`

Zastaví sběr (data zůstanou čitelná) / smaže data (nová `generation`,
sběr pokračuje, pokud běžel) / jen stav.

### `emu_coverage_mark`

Začne novou epochu; stav nese její číslo v `epoch`. Každá adresa
a jednotka si pamatuje epochu, ve které se poprvé vykonala.

### `emu_coverage_read`

Stránka řádků seřazených podle PC. Řádky jsou pole v pořadí `columns`.

- řádky `branch` (vykonané adresy): `pc`, `state`, `int_entry`,
  `first_epoch`.
- řádky `detail` (jednotky): `pc`, `space` (`ram`, `memext`, `rom`,
  `vram`, `pcg`, `bus` = bez fyzické buňky, offset je CPU adresa),
  `offset`, `key` (bajty opkódu hexadecimálně), `len`, `exec`, `taken`,
  `not_taken`, `state`, `operand` (null bez operandu), `next_pc`,
  `first_epoch`, `flags`.
- `state`: `none` (nepodmíněná), `unobserved`, `taken_only`,
  `not_taken_only`, `both`.

Argumenty: `cursor` (`next_cursor` předchozí stránky), `columns`,
`filter` (`pc_from`, `pc_to`, `space`, `state` - jedno jméno nebo
seznam, i `partial` a `decision` -, `new_since_epoch`, `flags`), `limit`
(1..5000, výchozí 500), `max_chars` (1000..45000, výchozí 20000). Přidá
`columns`, `rows`, `returned`, `page_full`, `end_of_data`
a `next_cursor`. Po `emu_coverage_reset` nebo novém startu skončí starší
kurzor chybou `segment_replaced:`.

### `emu_coverage_export`

Všechny řádky do souboru. Argumenty: `path` (povinný, přepíše se; složka
musí existovat) a `format` (`json` výchozí, `csv`). JSON: `{"format":
"mz800emu-coverage/1", "status", "columns", "rows": [{...}]}`; CSV:
hlavička se jmény sloupců, příznaky oddělené `|`. Přidá `path`, `format`
a `rows`.

## Zachycení zvuku

Zachycení zvuku zapisuje do složky, co dělá zvukový hardware, v emulovaném
čase - pro hledání chyb zvuku v programu i v emulátoru. Stejné zachycení je
i v GUI (Tools -> Audio Capture, `Alt + Q`) a v příkazové řádce
(`--audio-capture`), viz [`../audio-capture.md`](../audio-capture.md).

| Soubor | Obsah |
|---|---|
| `events.csv` | `ticks,time_us,frame,channel,level` - každá změna výstupní úrovně (0-15) zvukového kanálu |
| `psg_writes.csv` | `ticks,time_us,frame,psg,byte,channel,reg,value` - každý zápis do PSG s dekódováním |
| `reference.wav` | zvuk vyrenderovaný přesně v emulovaném čase (renderer video záznamu) |
| `sdl.wav` | zvuk ze zvukové výstupní cesty emulátoru - to, co slyší uživatel |
| `capture.json` | platforma, `gdg_clk_hz`, `frames_per_sec`, názvy kanálů, počty, důvod konce, absolutní časy `origin_ticks`, `end_ticks`, `last_event_ticks` a hodiny platformy `clocks` (zapisuje se na konci, viz audio-capture.md) |

- `ticks` jsou takty GDG od začátku zachycení (`gdg_clk_hz` v
  capture.json), `time_us` totéž v mikrosekundách, `frame` zvukový snímek
  (= snímek obrazu, 20 ms PAL / 16,7 ms NTSC) od začátku.
- Kanály: `ctc0` (reproduktor řízený CTC0 a PIO, úroveň 0 nebo 15),
  `psg0_0`..`psg0_3` a na MZ-1500 nebo MZ-800 s druhým PSG
  `psg1_0`..`psg1_3`. Kanál 3 každého PSG je šumový. První řádky
  events.csv (ticks 0) jsou úrovně na začátku.
- psg_writes.csv: `psg` je cíl (1 = PSG0, 2 = PSG1, 3 = oba), `byte`
  zapsaná hodnota hexadecimálně. Dekóduje se ze stavu PSG po zápisu (pro
  oba PSG z PSG0): `reg` = `tone_lo` (latch bajt tónu: spodní 4 bity
  děliče, dělič se mění až datovým bajtem), `tone` (celý 10bitový dělič),
  `volume` (útlum 0-15, 15 = vypnuto) nebo `noise` (`(typ << 2) | rychlost`).
- Oba WAV jsou 44100 Hz, 16 bit, stereo. sdl.wav je proti reference.wav
  opožděný: bez zvukového zařízení (headless) přesně o jeden snímek, se
  zařízením podle jeho bufferu. Se zařízením při MAX SPEED výstupní cesta
  snímky vynechává - `sdl_blocks` je pak menší než `frames`.
- Začátek: zachycení začne na konci nejbližšího zvukového snímku, se
  `sdl_wav` až po dalších 6 snímcích, které rozjedou výstupní cestu (jinak
  by její filtry začínaly ze studeného stavu). Úrovně, které se delší dobu
  nezměnily, začnou v obou souborech ztlumené, jako ve výstupní cestě.
- Konec: `emu_audio_capture_stop`, dosažení `frames`, pojistka jedné
  hodiny, reset nebo načtení snapshotu (emulovaný čas skočí zpět), chyba
  zápisu nebo ukončení emulátoru.
- Zachytává se jen za běhu emulace - kombinovat s `emu_run(frames=N)`.

Typické použití:

```
emu_audio_capture_start(dir="C:/tmp/snd", frames=250)   # 5 s PAL
emu_run(frames=260)
emu_audio_capture_status()      # stop_reason "Frame count reached"
```

### `emu_audio_capture_start`

Argumenty (všechny volitelné): `dir` (složka, vytvoří se; prázdný = nová
složka `<platforma>_audio_<datum>_<čas>` ve výstupní složce zachycení,
výchozí `audio_captures` vedle emulátoru),
`frames` (stop po tolika zvukových snímcích, 0 = do stop), `events`,
`psg_writes`, `reference_wav`, `sdl_wav` (bool, výchozí true - které
soubory zapisovat), `memory` (bool, výchozí true - držet paměť zachycení
pro `emu_trace_read` `audio_*`, `emu_audio_capture_summary`
a `emu_audio_capture_pcm`), `pcm` (seznam `"reference"` / `"sdl"`, výchozí
`["reference"]` - PCM ringy; pro `ref_vs_sdl` v souhrnu jsou potřeba oba),
`memory_minutes` (1-120, výchozí 10 - ring souhrnů snímků), `pcm_seconds`
(1-3600, výchozí 60 - PCM ringy). Zachycení jen do paměti (všechny čtyři
soubory false) nezakládá složku. Vrací stav s `pending: true`, nebo chybu
(už běží, nic nevybráno, složku nejde vytvořit, nedostatek paměti).

### `emu_audio_capture_stop`

Ukončí zachycení a dopíše soubory. Vrací konečný stav; chyba "No audio
capture is running", když žádné neběží.

### `emu_audio_capture_status`

`{"pending", "active", "frames", "events", "psg_writes",
"reference_samples", "sdl_blocks", "sdl_samples", "dir", "stop_reason",
"last_error"}`. `stop_reason`: `Stopped`, `Frame count reached`,
`Safety limit reached`, `Emulation timeline restarted (reset or snapshot
load)`, `Error`, `Emulator exit`, za běhu prázdný.

Dále `segment` (pořadí zachycení od spuštění emulátoru; nový start
nahradí paměť), `origin_ticks` (absolutní takty GDG času 0, před začátkem
`null`), `channels` (jména kanálů v pořadí polí `audio_frames`), `stereo`
a `memory` (`null`, nebo `{"bytes", "streams": {"psg_writes" | "levels" |
"frames" | "pcm_reference" | "pcm_sdl": {"record_size", "capacity",
"first_seq", "next_seq"}}}`; `first_seq` > 0 = ring zahodil nejstarší
záznamy, PCM `seq` = vzorek 44100 Hz od začátku).

### `emu_audio_capture_summary`

Souhrn paměti zachycení, několik kB textu. Argumenty (volitelné):
`frame_from` (výchozí 0), `frame_to` (-1 = nejnovější), `silence_rms`
(práh ticha jako RMS reference int16, výchozí 64), `max_notes` (výchozí
200), `lag_frames` (-1 = najít zpoždění zvukového výstupu)
a `max_lag_frames` (výchozí 8). Vrací:

- `frames` [od, do], `frames_available`, `fps`, `platform`, `stereo`;
- `sound_spans` / `silent_spans` ([[od, do]], nejvýš po 50, `*_total` =
  všechny) - ticho = RMS reference pod `silence_rms`;
- `channels` - po kanálech `active_frames`, `edges`, `max_level`,
  `on_pct`, `est_hz_median` (změny úrovně / 2 za sekundu);
- `psg_notes` - `ch`, `note` (`"A4"`, nebo `"noise"`), `cents`, `hz`,
  `divider`, `from_frame`, `to_frame`, `max_level`, přehrané ze zápisů do
  PSG a stavu PSG na začátku zachycení; `psg_state_incomplete` = nejstarší
  zápisy se zahodily;
- `ctc0` - reproduktor: `edges`, `edge_interval_hist_us` [[koš_od_us,
  počet]], `likely` (`tone`, `pcm`, `irregular`, `silent`) - odhad
  heuristikou, pravidlo je v `likely_rule`;
- `ref_vs_sdl` (potřebuje `pcm` s `reference` i `sdl`) - `lag_samples`,
  `lag_frames`, `lag_ambiguous` (v porovnaném úseku jen ustálený tón,
  zpoždění nejde určit), `diff_rms`, `diff_max` (-1..1, mono),
  `reference_rms`, `worst_frames` [[snímek, diff_rms]], `sdl_missing_blocks`;
  porovnají se nejvýš 2 s.

### `emu_audio_capture_pcm`

Okno PCM z paměti zachycení. Argumenty (volitelné): `source` (`reference`
nebo `sdl`), `frame_from` / `frames` (výchozí 0 / 25) nebo `sample_from`
/ `samples` (vzorky 44100 Hz od začátku), `rate` (1000-44100, výchozí
11025), `stereo` (výchozí false = mono průměr), `encoding` (`wav_b64` nebo
`pcm_b64` = surové int16 little endian), `max_bytes` (strop dat před
base64, výchozí 32768, nejvýš 4 MiB). Převzorkování průměruje (analytický
náhled); `rate` 44100 se `stereo` vrací vzorky reference.wav / sdl.wav
bajt po bajtu. Vrací `source`, `segment`, `live`, `rate`, `channels`,
`from_sample`, `samples`, `frames_out`, `next_sample` (odsud pokračovat),
`end_of_data`, `truncated`, `available` [první, příští], `gap`,
`encoding`, `data_b64`.

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
chcete program i spustit, použijte `emu_media_run_mzf` (níže).

### `emu_media_run_mzf` (sensitive)

Nahraje MZF a spustí ho od EXEC adresy z hlavičky (offset 0x16).
Dvě varianty podle parametru `bootstrap`:

- **`bootstrap=true` (výchozí)** - reset stroje a zavedení programu
  stejně jako CLI volba `--run-mzf` (příkaz emulátoru `media_run_mzf`).
  Stroj je ve stavu, v jakém ho ROM nechá po nahrání programu z pásky:
  registry CPU (IM 1, DI, SP = 0x10F0, registry, které ROM předává
  programu), 8255, 8253, Z80 PIO, PSG, GDG, VRAM a pracovní oblast
  monitoru v RAM. Reset je stejný jako `emu_reset`, takže výsledek
  nezávisí na tom, co v emulátoru běželo předtím. Stav pauzy se nemění:
  v pauze program čeká na první instrukci (PC = EXEC). Soubor se ověří
  před resetem - neexistující, nečitelný nebo zkrácený soubor vrátí chybu
  a stroj se nezresetuje.
- **`bootstrap=false`** - starší složené volání bez resetu:
  `emu_media_load_mzf` + odpojení dolní/horní ROM (porty 0xE0/0xE1) +
  `emu_set_register` PC. Nic dalšího se nenastavuje: režim přerušení,
  IFF, Z80 PIO, CTC, paleta a ostatní stav zůstanou po předchozím
  programu. Program se pak může chovat jinak než po nahrání z pásky
  (například mu nechodí přerušení).

```
emu_media_run_mzf(path="/programs/mzdos.mzf")
emu_media_run_mzf(path="/programs/mzdos.mzf", bootstrap=false)
```

Vrací:

```json
{
  "loaded": true,
  "header": {
    "file_type": 1, "filename": "mzdos",
    "file_size": 4096, "load_addr": 256, "exec_addr": 256
  },
  "bootstrap": true,
  "reset": true,
  "pc_set_to": 256
}
```

S `bootstrap=false` je `"bootstrap": false, "reset": false` a navíc
`"rom_disconnected": true`. MZF hlavička se parsuje na straně wrapperu
(Python).

**Destruktivní:** obě varianty přepíšou běžící program. Pokud má
program projít skutečnou cestou přes ROM (IPL nebo monitor čte pásku
přes vlastní loader), použijte `emu_media_insert(slot='cmt', path=...)`
nebo `emu_cmt_open` a příkaz pro nahrání z klávesnice (pomalejší, ale
projde celý kód ROM).

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

Frame timing default = 3 snímky (60 ms emulovaného času při 50 Hz PAL).
Maximum 600 snímků na jedno držení nebo mezeru jako safety proti AI freezi.

**Sémantika snímků emulace:** `frames`, `frame_per_key`, `hold_frames`
a `gap_frames` jsou **snímky emulace**, přesné při jakékoli rychlosti
emulace (normal, MAX SPEED, pomaleji než reálný čas např. se zapnutým
CDL). Celou sekvenci stisk / držení / uvolnění / mezera dostane vlákno
emulace a klávesy samo mačká a pouští na hranicích snímků. Emulátor
v pauze odehraje přesně délku sekvence a znovu se pauzne (jako
`emu_run(frames=N)`); běžící emulátor běží dál. `emu_input_send_keys`
nechává mezi klávesami 1 snímek bez stisku (opakovaná klávesa se tak
zaregistruje dvakrát). Odpověď nese
`emu_frames` (skutečně uplynulé snímky emulace), `complete`
a `interrupted` (`true` = sekvenci ukončil breakpoint, pauza nebo
reset; držená klávesa byla uvolněna). Při normální rychlosti trvá
sekvence reálný čas (50 snímků = 1 s).

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
# RIGHT drženo 30 snímků, 5 snímků nic, SPACE drženo 2 snímky
events = [
    {"key": "RIGHT", "hold_frames": 30, "gap_frames": 5},
    {"key": "SPACE", "hold_frames": 2},
]
emu.call('input_send_keys_with_delays', events=events)
# -> {"events_processed": 2, "total_frames": 37, "emu_frames": 37,
#     "complete": true, "interrupted": false}
```

Každý event je `{"key", "hold_frames" (default 3), "gap_frames" (default 0)}`;
event bez `key` se přeskočí.

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
  `name` bez `auto_name` ve `fields` vypne automatické jméno
  (`Addr: 0x....`, které se přejmenovává při každé změně adresy);
  `auto_name: true` ho vrátí. Platí i pro `emu_bp_update`.

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
  akcí (snapshoty a trace uložené akcemi a nahrávání trace kanálů, které
  akce spustila nebo ukládá) - po překročení prahu emulátor sám
  zapauzuje a vydá saturation event / warning s důvodem
  (`stopped_by: "bp_action_saturation"`). Trace spuštěný jen přes
  `emu_trace_start` nebo z GUI se nepočítá; hlídá ho vlastní
  `max_total_mb`, který zastaví nahrávání a emulace běží dál. Práh je
  konfigurovatelný.
- **Robustnost control-plane.** I když pokračující BP spustí těžkou
  forward akci, `pause`, `force_pause` a `get_state` se vyřídí hned po
  akci - emulátor se nezasekne. Akce může běžet uprostřed instrukce
  (MEM_W BP se vyhodnocuje před zápisem), proto ostatní příkazy (čtení
  registrů a paměti, zápisy, změny breakpointů) počkají na konec snímku
  nebo na pauzu a vidí konzistentní stav. Kdo chce při těžkých akcích
  měnit breakpointy, nejdřív zapauzuje.

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
    {"id": 1, "kind": "ram", "name": "User RAM (64 KB)",
     "logical_base": null, "size": 65536, ...},
    {"id": 2, "kind": "rom_lower", "name": "Monitor ROM (lower)",
     "logical_base": 0, "size": 4096, "writable": false, ...},
    {"id": 8, "kind": "memext_ram", "sub_id": 12,
     "name": "Memext RAM bank 12", "size": 4096, ...},
    ...
  ],
  "count": int,
  "layout_gen": int
}
```

`kind` enum: `logical, ram, rom_lower, rom_upper, cgrom, cgram_700,
vram_700_char, vram_700_attr, vram_phys_plane (sub_id 0..3),
pcg_1500 (sub_id 0..2), memext_ram, memext_flash, ramdisk_std,
ramdisk_pezik, prohibited_shadow`.

`sub_id` disambiguuje regiony stejného kindu (= plane index, bank
index, PEZIK instance).

Region `ram` ("User RAM (64 KB)") je 64 KB RAM tak, jak by ji viděl
CPU, kdyby byla RAM namapovaná v celém adresním prostoru (ROM a VRAM se
neuplatní). Bez Memextu je to vestavěná DRAM. Se zapojeným Memextem jsou
to banky Memextu, které jsou právě namapované, ne vestavěná DRAM (tu CPU
nevidí). Proto se může lišit od `memory/ram.bin` ve snapshotu `.mzs`,
který vždy obsahuje vestavěnou DRAM - podrobnosti v
`emulator://docs/memory_layout` (sekce o Memextu).

**Stabilita ID**: `id` je index do výčtu a platí, dokud se nezmění
sestava HW (RAMdisk, MemExt, PEZIK, načtení snapshotu s jinou
sestavou). `layout_gen` označuje rozložení výčtu a mění se při každé
jeho změně. `emu_region_read` a `emu_region_write` ho kontrolují: s `id`
ze staršího výčtu skončí chybou `stale region id: ...` a nic nečtou ani
nezapisují. Pro nová ID zavolej `regions_list` znovu. Kdo si chce adresu
pamatovat přes takové změny, ukládá `kind` a `sub_id`, ne `id`.

### `emu_region_read`

Raw no-side-effect read z konkrétního regionu (= bypass Z80 banking).
Žádný auto-inc latch, GDG RF dispatch ani IRQ trigger.

Args:
- `region_id` (= z poslední `regions_list` volání)
- `offset` (= byte offset v rámci regionu, 0..size-1)
- `length` (= 1..65536, clamp na region size pokud `offset+length > size`)
- `want_data` (bool, volitelný, default true) - false = jen hash
- `layout_gen` (int, volitelný) - `layout_gen` výčtu, ze kterého
  `region_id` pochází; výchozí je poslední `regions_list` kteréhokoli
  MCP klienta. Jiné rozložení vrátí chybu `stale region id: ...`.

Returns:
```json
{
  "region_id": int,
  "offset": int,
  "length": int,        // skutečně přečtená délka po clampu
  "sha256": "...",      // SHA-256 přečtených bajtů (64 hex, malá písmena)
  "data_b64": "..."     // base64-encoded raw bajty (ne s want_data=false)
}
```

Hlavní use case: AI klient v headless mode (= bez GUI access)
inspectuje fyzické paměti emu - ROM monitor content, VRAM bitmap
planes, MemExt banks. Reply 0007 v mzdos requests-to-emu-team
obsahuje příklady použití.

### `emu_region_write` (citlivý)

Raw zápis do fyzického regionu paměti (= mimo banking Z80), protějšek
`emu_region_read`.

Args:
- `region_id` (= z posledního volání `regions_list`)
- `offset` (= byte offset v rámci regionu, 0..size-1)
- `data_b64` (= base64 bajty, 1..65536 bajtů po dekódování; na konci
  regionu se zkrátí)
- `layout_gen` (int, volitelný) - jako u `emu_region_read`; s `id` ze
  staršího výčtu se nic nezapíše
- `expected_sha256` (string, volitelný) - podmíněný zápis: zapíše jen
  tehdy, když SHA-256 přepisovaných bajtů (po zkrácení na konci regionu)
  pořád odpovídá této hodnotě, např. `sha256` z `emu_region_read` nebo
  z okna regionu v `emu_state_observe`; jinak chyba `Precondition failed:
  memory changed (...)` a nic se nezapíše. Chyby ID, `layout_gen`
  a regionu jen pro čtení mají přednost.
- `want_hashes` (bool, volitelný) - hashe i bez podmínky

Vrací `{"region_id", "offset", "length", "written"}` - `length` je počet
skutečně zapsaných bajtů (po zkrácení), `written` = zapsal se aspoň
jeden bajt; s podmínkou nebo `want_hashes` navíc `before_sha256`
a `after_sha256`.

Regiony jen pro čtení (`memext_flash`, `prohibited_shadow`), odpojený
region nebo neplatný `region_id` / `offset` vrátí chybu parametrů
`region write failed: read-only, disconnected, or invalid
region_id/offset`. ROM regiony jsou zapisovatelné jako ladicí pomůcka:
úprava platí do nového nahrání ROM (např. po resetu), soubor ROM na disku
se nemění. Region `logical` (pohled Z80) zapisuje přes aktuální banking
s běžnými vedlejšími efekty (např. zápis do VRAM jde přes zápisový formát
GDG) - pro úpravu VRAM použijte fyzické regiony rovin. Změna VRAM se
ukáže v dalším snímku; GUI v pauze potřebuje překreslit.

### `emu_file_load_to_region` (citlivý)

Nahraje soubor do regionu paměti bez base64 na straně klienta (Python
složenina: čtení souboru + `emu_region_write`).

Args: `path`, `region_id`, `dest_offset` (výchozí 0), `src_offset`
(přeskočí N bajtů souboru, výchozí 0), `length` (počet bajtů, výchozí
-1 = do konce souboru).

Víc než 65536 bajtů je chyba (`length N > 65536; split into multiple
calls`) - velký soubor kopírujte po částech. Při úspěchu vrací odpověď
`emu_region_write` a navíc `src_file` a `src_offset`.

### `emu_file_save_from_region`

Uloží část regionu paměti do souboru (Python složenina: `emu_region_read`
+ zápis souboru) - výpisy ROM, zálohy ramdisku, raw VRAM.

Args: `path` (přepíše se), `region_id`, `src_offset` (výchozí 0),
`length` (výchozí 4096, max 65536).

Vrací `{"region_id", "src_offset", "length", "dest_file"}`; `length` je
počet skutečně uložených bajtů (na konci regionu se zkrátí).

## Memory search a Freeze Bytes tools

Hledání proměnných (cheat search, RAM search) v paměťových regionech:
životy, skóre, časovače nebo souřadnice se najdou zužováním množiny
kandidátních pozic ([Memory Search](../debugger/memory-search.md)).
Postup je v MCP dokumentu `emulator://docs/memsearch_workflow`.

Sezení drží kandidáty a tři vzorky prostoru: první vzorek hledání,
poslední a ten před ním. Vzorek pořizuje vlákno emulátoru - za běhu na
nejbližší hranici snímku, v pauze hned - pro celý prostor najednou.
Sezení MCP jsou oddělená od okna GUI; otevřených jich je nejvýš 8,
MCP server je při ukončení zavře.

### `emu_memsearch_start`

Argumenty:

- `space` (výchozí `"ram"`) - předvolba `ram` (User RAM 64 KB),
  `logical` (co CPU vidí teď, včetně ROM a VRAM), `vram` (VRAM 700 text
  / atributy a VRAM roviny MZ-800), `memext` (RAM banky MemExt),
  `ramdisk` (banky RAM disku STD a PEZIK), `all_writable` (vše z toho bez
  `logical`); nebo seznam `{"kind", "sub_id", "offset", "length"}` (druhy
  jako v `emu_regions_list`, `length` výchozí do konce regionu). ROM,
  CG-ROM ani FLASH se neprohledávají nikdy.
- `type` (výchozí `u8`) - `u8`, `s8`, `u16`, `s16`, `u24`, `s24`, `u32`,
  `s32`, `bcd` (packed, 2 číslice na bajt), `digits` (číslice na bajt).
- `width` - bajty pro `bcd` (1-4, výchozí 2) a `digits` (1-8, výchozí 4).
- `endian` - `le` (výchozí u čísel a BCD) nebo `be` (výchozí u číslic =
  pořadí tisku).
- `aligned` - jen offsety dělitelné šířkou.
- `digit_encoding` - `raw` (00h-09h), `ascii` (30h-39h), `display`
  (display kód Sharp MZ 20h-29h, výchozí); `leading_space` (výchozí true)
  čte mezery zleva (ASCII 20h, display kód 00h) jako nuly.
- `op`, `value`, ... - volitelný první filtr, jen porovnání s hodnotou.
- `allow_large` - povolí prostor nad 4 MiB (až 32 MiB).

Vrací `{"session_id", "space_size", "spans": [{"kind", "sub_id",
"offset", "length", "connected"}], "candidates", "step", "sample_frame",
"undo_depth", "redo_depth"}`.

### `emu_memsearch_filter`

Argumenty: `session_id`, `op`, `value`, `compare_to`, `value2`, `mask`,
`negate`, `sample` (výchozí true).

| `op` | Kandidát zůstane, když |
|------|------------------------|
| `eq`, `ne`, `lt`, `le`, `gt`, `ge` | hodnota je vůči referenci v daném vztahu |
| `between` | `value` <= hodnota <= `value2` |
| `inc`, `dec` | hodnota vůči referenci vzrostla / klesla |
| `inc_by`, `dec_by`, `changed_by` | změnila se přesně o `value` |
| `changed`, `unchanged` | změnila / nezměnila se |
| `mask_eq` | bity pod `mask` jsou stejné (jen celočíselné typy) |

`compare_to`: `value`, `previous` (výchozí u inc/dec/changed/...) nebo
`first`. Se `sample: true` se nový vzorek porovná s posledním, se
`sample: false` se poslední vzorek porovná s tím před ním. `negate`
podmínku obrátí; neplatná hodnota BCD nebo číslic vypadne vždy.
Aritmetika je bez přetečení (u8 z 00h na FFh není `dec_by 1`). Kandidáti
v regionu, který se odpojil, zůstávají beze změny.

Vrací `{"removed", "candidates", "step", "sample_frame", ...}`.

### `emu_memsearch_sample`, `emu_memsearch_undo`, `emu_memsearch_redo`

Nový vzorek bez filtrace / vrácení / znovuprovedení posledního kroku.
Undo vrací i referenční vzorky (filtr po undo porovná se správnou
předchozí hodnotou); vrácení startu nechá sezení bez vzorku
(`"started": false`).

### `emu_memsearch_list`

Argumenty: `session_id`, `offset` (pořadí prvního kandidáta), `limit`
(1-1000, výchozí 100). Položka:

| Pole | Význam |
|------|--------|
| `index`, `space_offset` | Pořadí kandidáta, pozice v prostoru hledání (pro `emu_memsearch_eliminate`) |
| `kind`, `sub_id`, `region_offset` | Buňka (pro `emu_freeze_add`, `emu_region_read`) |
| `cur`, `prev`, `first` | Dekódované hodnoty (null = neplatné BCD / číslice nebo odpojený region) |
| `cur_hex` | Bajty v pořadí adres |
| `connected` | Region byl při posledním vzorku připojený |
| `z80_addr` | CPU adresa (pro `emu_watch_add`), null u VRAM roviny, CG-RAM, PCG, RAM disku a nenamapované části MemExt |
| `bp_hint` | `{"addr", "fields", "values"}` pro `emu_bp_create_with_init`: MEM_W breakpoint v zóně `RAM` (User RAM), `VRAM_FB` (VRAM 700), `CPU_VIEW` (Logical, namapovaná část Luftner) nebo `MMEXT_BANK` s `bp_addr_space: "bank_offset"` (PEHU, banka = část / 2); u hodnoty širší než 1 bajt rozsah; null tam, kde write breakpoint buňku nezachytí |

### `emu_memsearch_eliminate`, `emu_memsearch_close`

`emu_memsearch_eliminate(session_id, space_offsets)` ručně vyřadí
vybrané kandidáty (jde vrátit). `emu_memsearch_close(session_id)`
uvolní sezení.

### `emu_freeze_add` (citlivý)

Argumenty: `kind`, `sub_id` (výchozí 0), `offset` a buď `value` (0-255),
nebo `bytes` (1-64 hodnot od `offset`). Bajty se zapisují zpět jednou za
emulovaný snímek (Freeze Bytes, nejvýš 256 bajtů): program svůj zápis
vidí až do konce snímku. Vrací `{"added", "frozen"}`.

### `emu_freeze_remove`, `emu_freeze_list`

`emu_freeze_remove(kind, offset, length, sub_id)` nebo
`emu_freeze_remove(all=True)` vrací `{"removed"}`; `emu_freeze_list`
vrací `{"count", "max": 256, "entries": [{"kind", "sub_id", "offset",
"value"}]}`, včetně bajtů zafrozených v GUI.

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

Spustí nahrávání výstupu reálné kazety do WAV souboru. Nahrávání
startuje v pauze; pro zahájení záznamu volej
`emu_cmt_pause(paused=False)`. Podporován je jen WAV výstup. Vyžaduje
stav pásky STOP a zapisovatelnou cestu.

Args:
- `path` (required): cílová cesta WAV souboru (musí být zapisovatelná).

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
emu_cmt_stop()                     # ukonči, flush WAV
```

### `emu_cmt_set_speed` (sensitive)

Nastaví výchozí rychlost virtuální kazety: poměr vůči 1200 Bd, nebo
vlastní délky pulzů. Přijímá string klíč nebo en_CMTSPEED int: `1:1` (1),
`2:1` (2), `2:1_cpm` (3), `3:1` (4), `3:2` (5), `7:3` (6), `8:3` (7),
`9:7` (8), `25:14` (9), `custom` (10 = dříve uložené vlastní délky
pulzů). Místo `speed` lze předat `pulses_us` = `[long_high, long_low,
short_high, short_low]` v µs (dlouhý pulz = bit 1, krátký = bit 0), každá
délka v (0, 65535]; pořadí odpovídá hlavičce CMTSPEED zařízení UniCMT
(UniCMT 3x = `[156, 164, 80, 92]`). Délky se uloží (i do konfigurace)
a výchozí rychlost se přepne na `custom`. Mění výchozí rychlost; per-blok
override viz `emu_cmt_tape_set_block_speed`. Odráží se v `cmtspeed`,
`default_pulses_us` a `custom_pulses_us` v `emulator://periph/cmt`.

Args:
- `speed` (optional): poměr string ("2:1") nebo int (2).
- `pulses_us` (optional): 4 délky pulzů v µs (místo `speed`).

Returns: `{"ok": true, "property": "speed", "value": <int>}`, pro
`pulses_us` `{"ok": true, "property": "custom_pulses", "pulses_us": [...]}`.

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
`speed` přijímá stejné klíče poměrů / int 1..9 jako `emu_cmt_set_speed`,
`pulses_us` vlastní délky pulzů bloku (jen MZ bloky, blok pak má
`speed` 10). Vyžaduje naloženou pásku. Rychlost platí od příštího
otevření bloku (přechod na blok při přehrávání, `emu_cmt_tape_seek`);
právě otevřený blok se nemění.

Bloky za hlavičkou CMTSPEED (UniCMT) v páskách `.mzt` mají vlastní
rychlost z hlavičky už po otevření pásky; samotná hlavička se nepřehrává
a ve výpisu bloků není. Výpis `emulator://periph/cmt/tape` hlásí u bloku
`block_speed` ("default" / "set" / "none") a efektivní `pulses_us`.

Args:
- `block_id` (required): index bloku (0-based).
- `speed` (optional): poměr string ("2:1") nebo int (2).
- `pulses_us` (optional): 4 délky pulzů v µs (místo `speed`).

Returns: `{"ok": true, "block_id": <int>, "speed": <int>}` (u
`pulses_us` navíc `"pulses_us": [...]`).

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
