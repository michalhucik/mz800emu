/*
 * File:   dispatch.c
 *
 * Implementace dispatch vrstvy MCP backendu (V0.A.3 - core).
 *
 * Mapuje JSONL REQUEST -> handler funkce -> JSONL RESPONSE. Handlery
 * volají buď čistě v MCP vrstvě (lokální: ping, get_state) nebo přes
 * `dbgapi_ui_submit_cmd_sync_with_origin(..., DBGAPI_CMD_ORIGIN_MCP, ...)`.
 *
 * Soubor je celý obalen guardem `MZ800EMU_CFG_MCP_SERVER_ENABLED`,
 * aby při buildu s `NO_MCP=1` zůstal prázdný.
 *
 * Pro standalone testy (`MZ800EMU_MCP_TEST_BUILD`) je dispatch zkrácen
 * tak, aby nezahrnoval `mzarch_config.h`. Test poskytuje vlastní stub
 * implementaci `dbgapi_ui_submit_cmd_sync_with_origin` a globální
 * `g_dbgapi_cmdrq_queue` symbol.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../mzarch/mzarch_config.h"
#endif

/* Hodnoty MZTVSYS_PAL/NTSC pro #if porovnání níže (platform/info).
 * Standalone hlavička - bezpečná i v MZ800EMU_MCP_TEST_BUILD (ten musí
 * definovat -DMZTVSYS, viz tests/mcp/CMakeLists.txt). */
#include "../mzarch/mztvsys.h"

/* HW layer platformy (capabilities, clocks, video geometrie) - mzhal
 * krok 8. Standalone hlavička bez per-arch závislostí. */
#include "../mzarch/mzhal.h"

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include "dispatch.h"
#include "jsonl_io.h"
#include "cooperation.h"
#include "event_bus.h"
#include "trap_manager.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>      /* _close na Windows */
#define close _close
#else
#include <unistd.h>
#endif

#include "../debugger/dbgapi_cmdrq.h"
/* history_get potřebuje st_DEBUGGER_HISTORY_ROW + g_debugger_history +
 * konstanty DEBUGGER_HISTORY_LENGTH / POSMASK / MAX_INSTR_BYTES. */
#include "../debugger/debugger.h"
/* V1.A.6 - callstack_get handler potřebuje typy st_CALLSTACK_ENTRY +
 * CS_KIND_* + funkci callstack_snapshot_free. callstack.h je standalone
 * (jen stdint.h + stdbool.h), takže se safely includuje i v testovacím
 * buildu - test stub poskytne shim implementaci callstack_snapshot_free. */
#include "../debugger/callstack.h"
/* bp_list serializace (F015a ROVINA C) potřebuje kanonické UPPER_SNAKE
 * konvertory bpt_type_to_string / bp_zone_to_string (= jediná pravda pro
 * názvy typu/zóny BP, sdílená s .bpt persistencí). */
#include "../debugger/breakpoints.h"

/* GDG superset stav pro platform detection (= regDMD pro mode
 * discriminator; větvení dle g_mzhal.arch je runtime). */
#include "../hw-generic/gdg/gdg_state.h"

/* mzarch_platform exportuje globální fields pro plný platform string,
 * MZARCH_NAME (= "mz700" / "mz800" / "mz1500"; PAL/NTSC nerozlišuje -
 * oba MZ-700 targety mají "mz700") a pixel clock. */
#include "../mzarch/mzarch_platform.h"

/* PSG mirror API; PSG vstupní frekvence pro platform/info clocks
 * sub-objekt se čte runtime z g_mzhal.psg_divider. */
#include "../hw-generic/psg/psg.h"

/* PIO 8255 vkbd probe API (fix 0016 / cesta A) - skutečný readback
 * dosednutí vstříknuté klávesy. Hlavička taháa mzarch závislosti, které
 * v testovacím buildu (MZ800EMU_MCP_TEST_BUILD) nejsou dostupné; v test
 * buildu se probe nepoužívá (landing_verified zůstane false, viz HID
 * handlery). */
#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../hw-generic/pio8255/pio8255.h"
#endif

/* V1.A.7 - profiler_get handler musí znát layout st_PROF_ENTRY pro
 * iteraci void* entries pole. profiler.h taháa mzarch_config.h, který
 * v testovacím buildu (MZ800EMU_MCP_TEST_BUILD) není dostupný.
 *
 * Lokální mirror layoutu st_PROF_ENTRY z src/emulator/debugger/profiler.h.
 * Pole musí být v identickém pořadí a velikostech, jinak iterace přes
 * void* pointer z DBGAPI_CMD_GET_PROFILER vrátí garbage data.
 *
 * @invariant Tento layout MUSÍ být synchronizovaný s st_PROF_ENTRY
 *            v profiler.h. Při změně profiler.h aktualizovat zde. */
typedef struct st_MCP_PROF_ENTRY_MIRROR {
    uint16_t target_addr;
    uint8_t  kind;
    uint8_t  _pad;
    uint64_t calls;
    uint64_t cycles_incl_sum;
    uint64_t cycles_excl_sum;
    uint64_t cycles_incl_min;
    uint64_t cycles_incl_max;
} st_MCP_PROF_ENTRY_MIRROR;

/* V1.A.7 lesson learned (Kontrolor): mirror layout MUSÍ mít přesně
 * 48 B (4 B implicit padding za uint8_t _pad pro uint64 alignment).
 * Pokud někdo upraví profiler.h a zde zapomene, profiler_get iterace
 * přes void* entries vrátí garbage (= shift). Hard build assert. */
_Static_assert(sizeof(st_MCP_PROF_ENTRY_MIRROR) == 48,
               "st_MCP_PROF_ENTRY_MIRROR layout mismatch "
               "(očekáváno 48 B, viz st_PROF_ENTRY v profiler.h)");

#ifndef MZ800EMU_MCP_TEST_BUILD
#include "mcp_config.h"
#endif

#ifdef MZ800EMU_MCP_TEST_BUILD
/* Standalone test mód: forward-declare jen ta dbgapi_ui API která
 * dispatch reálně volá. Reálnou definici poskytne test stub.
 *
 * Důvod: `dbgapi_ui.h` taháno přes `main.h` -> `mzarch_config.h`, což v
 * testovacím buildu způsobí redefinition warning na
 * MZ800EMU_CFG_MCP_SERVER_ENABLED (cmake mu force-definuje cli flagem). */
extern st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;
bool dbgapi_ui_submit_cmd_sync_with_origin(st_DBGAPI_CMDRQ_QUEUE *queue,
                                            en_DBGAPI_CMD cmd,
                                            en_DBGAPI_CMD_ORIGIN origin,
                                            void *data_ptr,
                                            void *result_ptr,
                                            int timeout_ms);
#else
#include "../debugger/dbgapi_ui.h"
/* V1.E.7 - blokující emu_run / HID frame wait potřebuje sledovat
 * inkrement framebuffer counteru. iface_video.h vystavuje
 * g_iface_video->fbsnapshot_screen_id + fbsnapshot_pixels_mutex/cond,
 * což je per-frame signalizace publikovaná emu vláknem v
 * iface_video_framebuffer_screen_done(). V test buildu (= bez emu
 * vlákna) je wait no-op. */
#include "../../iface/iface_video.h"
#include "../emulator.h"      /* EMULATOR_TEST_PAUSED, emulator_pause */
#endif


/* ------------------------------------------------------------------ */
/* Konstanty a typy                                                    */
/* ------------------------------------------------------------------ */

/** @brief Default timeout (ms) pro dbgapi sync call z MCP handleru. */
#define MCP_DISPATCH_DBGAPI_TIMEOUT_MS 1000

/**
 * @brief Sanity horní mez per-BP fwd_min_interval_ms (0019 v2) v ms.
 *
 * Slouží jen jako ochrana proti nesmyslně velké hodnotě od klienta (24 h).
 * Vyšší interval nemá praktický smysl - rate-limit je proti saturaci, ne
 * proti dlouhodobému plánování. Hodnota 0 (= global/built-in default) i
 * jakákoliv hodnota v rozsahu projdou beze změny.
 */
#define MCP_DISPATCH_BP_FWD_MIN_INTERVAL_MS_MAX (24u * 60u * 60u * 1000u)

/* Forward declarations - implementace dále v souboru. */
static bool _dispatch_wait_frames(int frames, int *out_actual);
static bool _dispatch_wait_run_frames_done(int frames, int *out_actual);

/** @brief Protokolová verze hello payload. */
#define MCP_DISPATCH_PROTOCOL_VERSION "1.0"


/**
 * @brief Signatura handleru jednoho příkazu.
 *
 * Každý handler je odpovědný za:
 *  - vytažení parametrů z `req` (přes `jsonl_msg_get_data_node` +
 *    json-glib API)
 *  - validaci typu / rozsahu parametrů (při chybě sestaví error
 *    response a vrátí `MCP_DISPATCH_INVALID_PARAMS`)
 *  - volání `dbgapi_ui_submit_cmd_sync_with_origin` s
 *    `DBGAPI_CMD_ORIGIN_MCP` (pro neproxy příkazy se přeskočí)
 *  - sestavení success / error JSONL RESPONSE řádku přes
 *    `jsonl_build_response`
 *
 * @param[in]  req           parsed REQUEST (validní, typ REQUEST)
 * @param[out] out_response  výstupní JSONL řádek (caller `free()`)
 * @return kód z `en_MCP_DISPATCH_RESULT`
 */
typedef en_MCP_DISPATCH_RESULT (*mcp_handler_fn)(const st_JSONL_MESSAGE *req,
                                                  char **out_response);


/**
 * @brief Jeden záznam v dispatch tabulce.
 *
 * Mapuje wire-level `cmd` jméno na handler funkci. Pole `dbgapi_cmd`
 * je informativní (= odpovídá `en_DBGAPI_CMD` který handler interně
 * použije; pro lokální handlery `DBGAPI_CMD_NONE`).
 */
typedef struct st_MCP_CMD_MAP_ENTRY {
    const char     *name;         /**< JSON `cmd` string, např. "pause". */
    en_DBGAPI_CMD   dbgapi_cmd;   /**< Cílový dbgapi příkaz, nebo NONE. */
    mcp_handler_fn  handler;      /**< Handler funkce (povinná). */
} st_MCP_CMD_MAP_ENTRY;


/* ------------------------------------------------------------------ */
/* Forward deklarace handlerů                                          */
/* ------------------------------------------------------------------ */

static en_MCP_DISPATCH_RESULT _handle_ping(const st_JSONL_MESSAGE *req,
                                           char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_state(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_pause(const st_JSONL_MESSAGE *req,
                                            char **out_response);
static en_MCP_DISPATCH_RESULT _handle_run(const st_JSONL_MESSAGE *req,
                                          char **out_response);
static en_MCP_DISPATCH_RESULT _handle_reset(const st_JSONL_MESSAGE *req,
                                            char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_registers(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_set_register(const st_JSONL_MESSAGE *req,
                                                   char **out_response);
static en_MCP_DISPATCH_RESULT _handle_dasm(const st_JSONL_MESSAGE *req,
                                           char **out_response);
static en_MCP_DISPATCH_RESULT _handle_history_get(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_mem_read(const st_JSONL_MESSAGE *req,
                                               char **out_response);
static en_MCP_DISPATCH_RESULT _handle_mem_write(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bp_add(const st_JSONL_MESSAGE *req,
                                             char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bp_list(const st_JSONL_MESSAGE *req,
                                              char **out_response);
static en_MCP_DISPATCH_RESULT _handle_shutdown(const st_JSONL_MESSAGE *req,
                                               char **out_response);
/* V0.B.6 - chybející V0 Tools */
static en_MCP_DISPATCH_RESULT _handle_bp_remove(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bp_clear(const st_JSONL_MESSAGE *req,
                                               char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bp_enable(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_step_into(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_step_over(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_step_n(const st_JSONL_MESSAGE *req,
                                             char **out_response);
static en_MCP_DISPATCH_RESULT _handle_run_until_addr(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
/* V0.B.7 - resource backing pro emulator://config/mcp */
static en_MCP_DISPATCH_RESULT _handle_get_mcp_config(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
/* V1.A.1 - snapshot tools + cooperation hint */
static en_MCP_DISPATCH_RESULT _handle_snapshot_save(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_snapshot_save_buffer(const st_JSONL_MESSAGE *req,
                                                            char **out_response);
static en_MCP_DISPATCH_RESULT _handle_snapshot_load(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_snapshot_load_buffer(const st_JSONL_MESSAGE *req,
                                                            char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cooperation_hint_set(const st_JSONL_MESSAGE *req,
                                                            char **out_response);
/* V1.A.2 - symbol management Tools */
static en_MCP_DISPATCH_RESULT _handle_symbol_add(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_symbol_remove(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
static en_MCP_DISPATCH_RESULT _handle_symbol_lookup(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
static en_MCP_DISPATCH_RESULT _handle_symbol_list(const st_JSONL_MESSAGE *req,
                                                   char **out_response);

/* V1.A.3 - step out + run_until_* Tools */
static en_MCP_DISPATCH_RESULT _handle_step_out(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_run_until_raster(const st_JSONL_MESSAGE *req,
                                                        char **out_response);
static en_MCP_DISPATCH_RESULT _handle_run_until_tstate(const st_JSONL_MESSAGE *req,
                                                        char **out_response);
static en_MCP_DISPATCH_RESULT _handle_run_until_event(const st_JSONL_MESSAGE *req,
                                                       char **out_response);

/* V1.A.4 - EVENT subscribe + TRAP forwarding Tools */
static en_MCP_DISPATCH_RESULT _handle_event_subscribe(const st_JSONL_MESSAGE *req,
                                                       char **out_response);
static en_MCP_DISPATCH_RESULT _handle_event_unsubscribe(const st_JSONL_MESSAGE *req,
                                                         char **out_response);
static en_MCP_DISPATCH_RESULT _handle_event_poll(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_trap_respond(const st_JSONL_MESSAGE *req,
                                                    char **out_response);

/* V1.A.5 - chip-level fault injection Tools */
static en_MCP_DISPATCH_RESULT _handle_io_read(const st_JSONL_MESSAGE *req,
                                               char **out_response);
static en_MCP_DISPATCH_RESULT _handle_io_write(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_irq_inject(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_nmi_inject(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_mem_write_force(const st_JSONL_MESSAGE *req,
                                                       char **out_response);

/* V1.A.6 - Watch + Callstack + CDL Tools fwd decls */
static en_MCP_DISPATCH_RESULT _handle_watch_add(const st_JSONL_MESSAGE *req,
                                                 char **out_response);
static en_MCP_DISPATCH_RESULT _handle_watch_remove(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_watch_list(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_watch_eval(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_callstack_get(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cdl_start(const st_JSONL_MESSAGE *req,
                                                 char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cdl_stop(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cdl_reset(const st_JSONL_MESSAGE *req,
                                                 char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cdl_export(const st_JSONL_MESSAGE *req,
                                                  char **out_response);

/* 0017 FÁZE 1 - Tracking lifecycle (trace-suite) fwd decls */
static en_MCP_DISPATCH_RESULT _handle_trace_start(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_trace_stop(const st_JSONL_MESSAGE *req,
                                                 char **out_response);
static en_MCP_DISPATCH_RESULT _handle_trace_reset(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_trace_save(const st_JSONL_MESSAGE *req,
                                                 char **out_response);

/* V1.A.7 - Profiler Tools fwd decls */
static en_MCP_DISPATCH_RESULT _handle_profiler_start(const st_JSONL_MESSAGE *req,
                                                      char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_stop(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_reset(const st_JSONL_MESSAGE *req,
                                                      char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_export(const st_JSONL_MESSAGE *req,
                                                       char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_get(const st_JSONL_MESSAGE *req,
                                                    char **out_response);

/* V1.B.1 - Media Tools fwd decls */
static en_MCP_DISPATCH_RESULT _handle_media_load_mzf(const st_JSONL_MESSAGE *req,
                                                      char **out_response);
static en_MCP_DISPATCH_RESULT _handle_media_load_binary(const st_JSONL_MESSAGE *req,
                                                         char **out_response);
static en_MCP_DISPATCH_RESULT _handle_media_insert(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_media_eject(const st_JSONL_MESSAGE *req,
                                                   char **out_response);
static en_MCP_DISPATCH_RESULT _handle_media_state(const st_JSONL_MESSAGE *req,
                                                   char **out_response);

/* V1.B.2 - Platform + Config Tools fwd decls */
static en_MCP_DISPATCH_RESULT _handle_settings_get(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_settings_set(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_platform_set(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_periph_attach(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
static en_MCP_DISPATCH_RESULT _handle_periph_detach(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
/* V1.B.3 - hot-swap workflow */
static en_MCP_DISPATCH_RESULT _handle_emu_stop(const st_JSONL_MESSAGE *req,
                                                char **out_response);

/* V1.C.1 - HID Tools fwd decls */
static en_MCP_DISPATCH_RESULT _handle_input_send_key(const st_JSONL_MESSAGE *req,
                                                      char **out_response);
static en_MCP_DISPATCH_RESULT _handle_input_send_keys(const st_JSONL_MESSAGE *req,
                                                       char **out_response);
static en_MCP_DISPATCH_RESULT _handle_input_press_key(const st_JSONL_MESSAGE *req,
                                                       char **out_response);
static en_MCP_DISPATCH_RESULT _handle_input_release_key(const st_JSONL_MESSAGE *req,
                                                         char **out_response);
static en_MCP_DISPATCH_RESULT _handle_input_send_joystick(const st_JSONL_MESSAGE *req,
                                                           char **out_response);
static en_MCP_DISPATCH_RESULT _handle_input_send_keys_with_delays(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.D.1 - Core + CPU extras Resources (8 read-only handlers) */
static en_MCP_DISPATCH_RESULT _handle_get_config_settings(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_media_state(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_cpu_im2_vector(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_cpu_interrupt_bus(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_cooperation_policy(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_security_profile(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_memory_map(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_memext_info(
    const st_JSONL_MESSAGE *req, char **out_response);
/* BACKLOG D - emulation speed control (get_speed + set_speed). */
static en_MCP_DISPATCH_RESULT _handle_get_speed(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_set_speed(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.D.2.A - Easy reuse Resources (5 read-only handlers nad existujícími
 *            DBGAPI_CMD wrappery z V1.A.6 / V1.A.7 / V1.A.2 / stack_history
 *            + stack_regions). */
static en_MCP_DISPATCH_RESULT _handle_get_callstack(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_profiler(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_symbols(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_stack_history(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_stack_regions(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.D.2.B - Medium debug Resources (4 read-only handlers, dva přes
 *            existující DBGAPI_CMD wrappery, dva přes nové
 *            BP_VARS_LIST + BOOKMARKS_LIST). */
static en_MCP_DISPATCH_RESULT _handle_get_watch(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_stack(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_vars(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_bookmarks(
    const st_JSONL_MESSAGE *req, char **out_response);
/* BACKLOG B - bookmark write (bookmark_add + bookmark_remove). */
static en_MCP_DISPATCH_RESULT _handle_bookmark_add(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bookmark_remove(
    const st_JSONL_MESSAGE *req, char **out_response);

/* CMT-A - CMT transport + recording + cmthack toggle. */
static en_MCP_DISPATCH_RESULT _handle_cmt_transport(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cmt_record(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cmt_hack_set(
    const st_JSONL_MESSAGE *req, char **out_response);

/* CMT-B - CMT vlastnosti + práce s páskou. */
static en_MCP_DISPATCH_RESULT _handle_cmt_set_property(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cmt_open(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cmt_tape_seek(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cmt_tape_block_speed(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_cmt_tape_list(
    const st_JSONL_MESSAGE *req, char **out_response);

/* V1.D.3.A - IRQ chip Resource handlery (i8255 PPI + i8253 CTC +
 * Z80 PIO). Read-only snapshoty stavu chipů přes nové DBGAPI_CMD
 * GET_PERIPH_* (= žádný refactor backend modulů). */
static en_MCP_DISPATCH_RESULT _handle_get_periph_i8255(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_periph_i8253(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_periph_z80_pio(
    const st_JSONL_MESSAGE *req, char **out_response);

/* V1.D.3.B - Audio chip Resource handlery (SN76489 PSG + AY-3-8910
 * placeholder + beeper). Read-only snapshoty stavu audio chipů přes
 * nové DBGAPI_CMD GET_PERIPH_*. */
static en_MCP_DISPATCH_RESULT _handle_get_periph_sn76489(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_periph_ay3_8910(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_periph_beeper(
    const st_JSONL_MESSAGE *req, char **out_response);

/* V1.D.3.C - storage + display Resource handlery (GDG per-platforma +
 * WD1793 FDC + CMT + Quick Disk). Read-only snapshoty přes nové
 * DBGAPI_CMD_GET_PERIPH_GDG / _WD1793 / _CMT / _QD. */
static en_MCP_DISPATCH_RESULT _handle_get_periph_gdg(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_periph_wd1793(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_periph_cmt(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_periph_qd(
    const st_JSONL_MESSAGE *req, char **out_response);

/* V1.D.4 - input + frame Resource handlery (keyboard state + matrix info,
 * joystick state, framebuffer info, screenshot raw + PNG,
 * video text dump). Read-only snapshoty přes nové DBGAPI_CMD_GET_INPUT_*
 * / _FRAME_* / _VIDEO_*. */
static en_MCP_DISPATCH_RESULT _handle_get_input_keyboard_state(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_input_keyboard_matrix_info(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_input_joystick_state(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_frame_framebuffer_info(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_frame_screenshot_raw(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_frame_screenshot(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_screenshot_save_to_file(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_video_text_dump(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_watch_snapshot(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.E.2 - CPU control + details Tools (Commit 1: 5 jednoduchých) */
static en_MCP_DISPATCH_RESULT _handle_get_reg(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_force_pause(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_set_user_cycle_origin(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_im2_vector(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_raster_pos(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.E.2 - CPU flags (Commit 2: 2 Tools) */
static en_MCP_DISPATCH_RESULT _handle_get_cpu_flags(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_set_cpu_flags(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.E.2 - last_instr + panel_batch (Commit 3: 2 Tools) */
static en_MCP_DISPATCH_RESULT _handle_get_last_instr(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_cpu_panel_batch(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.E.3 - debugger state Tools (Commit 1: 3 Tools) */
static en_MCP_DISPATCH_RESULT _handle_debugger_activate(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_debugger_deactivate(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_is_debugger_active(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.E.3 - PIO-Z80 interrupt vector override (Commit 2: 1 Tool) */
static en_MCP_DISPATCH_RESULT _handle_set_pioz80_interrupt_vector(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.E.4 - BP advanced Tools (Commit 1: 6 Tools) */
static en_MCP_DISPATCH_RESULT _handle_bp_create_with_init(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bp_set_parent(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bp_update(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bpgrp_add(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bpgrp_remove(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bpgrp_update(
    const st_JSONL_MESSAGE *req, char **out_response);
/* V1.E.4 - Stack analytics Tools (Commit 2: 6 Tools) */
static en_MCP_DISPATCH_RESULT _handle_stack_history_enable(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_stack_history_reset(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_stack_regions_add(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_stack_regions_edit(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_stack_regions_remove(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_stack_regions_reset_watermark(
    const st_JSONL_MESSAGE *req, char **out_response);

/* platform-info-fix - real dynamic platform detection (= ne hardcoded stub) */
static en_MCP_DISPATCH_RESULT _handle_get_platform_info(
    const st_JSONL_MESSAGE *req, char **out_response);

/* mzdos-support 0007 - Direct memory region read/write (3 Tools) */
static en_MCP_DISPATCH_RESULT _handle_regions_list(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_region_read(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_region_write(
    const st_JSONL_MESSAGE *req, char **out_response);

/* V1.E.5 - Eventlog/TLOG Tools (6) */
static en_MCP_DISPATCH_RESULT _handle_eventlog_start(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_eventlog_stop(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_eventlog_clear(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_eventlog_set_capacity(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_eventlog_set_mask(
    const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_eventlog_get_event(
    const st_JSONL_MESSAGE *req, char **out_response);


/* ------------------------------------------------------------------ */
/* Shutdown callback registr (V0.A.4 rozšíření)                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Aktuálně registrovaný shutdown callback.
 *
 * Použití viz `mcp_dispatch_set_shutdown_callback`. Single-threaded
 * setup - žádný atomic / mutex potřeba (typicky nastaveno z
 * `main_pipe.c` ještě před spuštěním stdin readeru).
 */
static mcp_dispatch_shutdown_cb_fn g_shutdown_cb = NULL;


/* ------------------------------------------------------------------ */
/* Transport kind registr (V1.B.3)                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Aktuálně registrovaný transport kind.
 *
 * Nastavuje `main_pipe.c` nebo `tcp_server.c` po `mcp_dispatch_init`.
 * Čte ho zejména `_handle_emu_stop`, který v TCP módu vrací error
 * (= hot-swap nesmí zabít živou GUI session).
 */
static en_MCP_DISPATCH_TRANSPORT g_transport_kind =
    MCP_DISPATCH_TRANSPORT_NONE;


/* ------------------------------------------------------------------ */
/* cmd_map[] - centrální dispatch tabulka                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Hlavní tabulka mapování JSON `cmd` -> handler.
 *
 * V0.A.3 obsahuje 9 záznamů. V0.A.4 přidal `shutdown` (= 10).
 * V0.B.3 přidal `mem_write` (= 11). V0.B.6 přidal 7 chybějících tools
 * (= bp_remove, bp_clear, bp_enable, step_into, step_over, step_n,
 * run_until_addr) - celkem 18 entries. V0.B.7 přidal `get_mcp_config`
 * (= 19 entries + sentinel) jako backing handler pro Python MCP
 * Resource `emulator://config/mcp`. V1.A.1 přidal 5 dalších záznamů
 * (= snapshot_save, snapshot_save_buffer, snapshot_load,
 * snapshot_load_buffer, cooperation_hint_set) - celkem 24 entries +
 * sentinel. V1.A.2 přidal 4 symbol management Tools (= symbol_add,
 * symbol_remove, symbol_lookup, symbol_list) - celkem 28 entries +
 * sentinel. V1.A.3 přidal 4 step/run_until Tools (= step_out,
 * run_until_raster, run_until_tstate, run_until_event) - celkem
 * 32 entries + sentinel. V1.A.4 přidal 4 EVENT subscribe + TRAP Tools
 * (= event_subscribe, event_unsubscribe, event_poll, trap_respond) -
 * celkem 36 entries + sentinel. V1.A.5 přidal 5 chip-level fault
 * injection Tools (= io_read, io_write, irq_inject, nmi_inject,
 * mem_write_force) - celkem 41 entries + sentinel.
 * V1.A.6 přidal 9 Watch + Callstack + CDL Tools (= watch_add,
 * watch_remove, watch_list, watch_eval, callstack_get, cdl_start,
 * cdl_stop, cdl_reset, cdl_export) - celkem 50 entries + sentinel.
 * V1.A.7 přidal 5 Profiler Tools (= profiler_start, profiler_stop,
 * profiler_reset, profiler_export, profiler_get) - celkem 55 entries +
 * sentinel.
 * V1.B.1 přidal 5 Media Tools (= media_load_mzf, media_load_binary,
 * media_insert, media_eject, media_state) - celkem 60 entries + sentinel.
 * V1.B.2 přidal 5 Platform + Config Tools (= settings_set, settings_get,
 * platform_set, periph_attach, periph_detach) - celkem 65 entries +
 * sentinel.
 * V1.B.3 přidal 1 hot-swap Tool (= emu_stop, pipe-only; emu_start je
 * Python-only protože vyžaduje subprocess spawn z wrapperu) - celkem
 * 66 entries + sentinel.
 * V1.C.1 přidal 6 HID Tools (= input_send_key, input_send_keys,
 * input_press_key, input_release_key, input_send_joystick,
 * input_send_keys_with_delays) - celkem 72 entries + sentinel.
 * V1.D.1 přidal 8 Resource backing handlers (= get_config_settings,
 * get_media_state, get_cpu_im2_vector, get_cpu_interrupt_bus,
 * get_cooperation_policy, get_security_profile, get_memory_map,
 * get_memext_info) - celkem 80 entries + sentinel.
 * V1.D.2.A přidal 5 easy reuse Resource backing handlers (=
 * get_callstack, get_profiler, get_symbols, get_stack_history,
 * get_stack_regions) - celkem 85 entries + sentinel. Všech 5 jen
 * reuse-uje existující DBGAPI_CMD wrappery z V1.A bez nového CMD enum.
 * V1.D.2.B krok 1 přidal 2 medium reuse Resource backing handlers
 * (= get_watch, get_stack), oba nad existujícími V1.A wrappery
 * (WATCH_LIST + STACK_DUMP) - celkem 87 entries + sentinel.
 * V1.D.2.B krok 2+3 přidal 2 medium Resources nad novými DBGAPI_CMD
 * (= get_vars přes BP_VARS_LIST, get_bookmarks přes BOOKMARKS_LIST) -
 * celkem 89 entries + sentinel.
 * V1.D.3.A přidal 3 IRQ chip Resources (= get_periph_i8255 + _i8253 +
 * _z80_pio) - celkem 92 entries + sentinel.
 * V1.D.3.B přidal 3 audio chip Resources (= get_periph_sn76489 +
 * _ay3_8910 + _beeper; AY-3-8910 vrací available=false napříč
 * platformami, beeper je agregát CTC0 OUT + GATE0 + PC0) - celkem
 * 95 entries + sentinel.
 * V1.D.3.C přidal 4 storage + display Resources (= get_periph_gdg
 * per-platforma + _wd1793 + _cmt + _qd; GDG má per-platforma palette
 * layout, WD1793/QD vrátí available=false pokud chip není compiled
 * nebo runtime detached) - celkem 99 entries + sentinel.
 * V1.E.3 přidal 3 debugger state Tools (= debugger_activate +
 * debugger_deactivate + is_debugger_active) - celkem 122 entries +
 * sentinel.
 * V1.E.3 commit 2 přidal 1 PIO-Z80 IM2 vector override Tool (=
 * set_pioz80_interrupt_vector; na MZ-700 vrací available=false) -
 * celkem 123 entries + sentinel.
 * V1.E.4 commit 1 přidal 6 BP advanced Tools (= bp_create_with_init,
 * bp_set_parent, bp_update, bpgrp_add, bpgrp_remove, bpgrp_update) -
 * celkem 129 entries + sentinel.
 * V1.E.4 commit 2 přidal 6 stack analytics Tools (= stack_history_enable,
 * stack_history_reset, stack_regions_add, stack_regions_edit,
 * stack_regions_remove, stack_regions_reset_watermark) - celkem
 * 135 entries + sentinel.
 * 0017 FÁZE 1 přidal 4 Tracking lifecycle Tools (= trace_start,
 * trace_stop, trace_reset, trace_save) na KONEC tabulky - viz
 * MCP_EXPECTED_CMD_COUNT v tests/mcp/test_dispatch.c pro aktuální total.
 * Tabulka je NULL-terminated (= `name == NULL` u sentinel
 * záznamu). Pořadí v poli definuje i pořadí v hello payload `commands`
 * array (deterministické pro klienty / testy).
 *
 * @invariant `name` non-NULL u všech non-sentinel záznamů
 * @invariant `handler` non-NULL u všech non-sentinel záznamů
 */
static const st_MCP_CMD_MAP_ENTRY g_cmd_map[] = {
    /* V0.A.3 + V0.A.4 + V0.B.3 (11) */
    { "ping",            DBGAPI_CMD_NONE,              _handle_ping          },
    { "get_state",       DBGAPI_CMD_IS_RUNNING,        _handle_get_state     },
    { "pause",           DBGAPI_CMD_PAUSE,             _handle_pause         },
    { "run",             DBGAPI_CMD_RUN,               _handle_run           },
    { "reset",           DBGAPI_CMD_RESET,             _handle_reset         },
    { "get_registers",   DBGAPI_CMD_GET_ALL_REGS,      _handle_get_registers },
    { "mem_read",        DBGAPI_CMD_MEM_READ,          _handle_mem_read      },
    { "mem_write",       DBGAPI_CMD_MEM_WRITE_CHECKED, _handle_mem_write     },
    { "bp_add",          DBGAPI_CMD_BP_ADD,            _handle_bp_add        },
    { "bp_list",         DBGAPI_CMD_BP_LIST,           _handle_bp_list       },
    { "shutdown",        DBGAPI_CMD_NONE,              _handle_shutdown      },
    /* V0.B.6 - 7 chybějících V0 Tools */
    { "bp_remove",       DBGAPI_CMD_BP_REMOVE,         _handle_bp_remove     },
    { "bp_clear",        DBGAPI_CMD_BP_LIST,           _handle_bp_clear      },
    { "bp_enable",       DBGAPI_CMD_BP_SET_ENABLED,    _handle_bp_enable     },
    { "step_into",       DBGAPI_CMD_STEP_INTO,         _handle_step_into     },
    { "step_over",       DBGAPI_CMD_STEP_OVER,         _handle_step_over     },
    { "step_n",          DBGAPI_CMD_STEP_INTO,         _handle_step_n        },
    { "run_until_addr",  DBGAPI_CMD_RUN_TO,            _handle_run_until_addr },
    /* V0.B.7 - backing handler pro emulator://config/mcp Resource */
    { "get_mcp_config",  DBGAPI_CMD_NONE,              _handle_get_mcp_config },
    /* V1.A.1 - snapshot Tools + cooperation hint (5 tools) */
    { "snapshot_save",          DBGAPI_CMD_SNAPSHOT_SAVE_FILE,   _handle_snapshot_save        },
    { "snapshot_save_buffer",   DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER, _handle_snapshot_save_buffer },
    { "snapshot_load",          DBGAPI_CMD_SNAPSHOT_LOAD_FILE,   _handle_snapshot_load        },
    { "snapshot_load_buffer",   DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER, _handle_snapshot_load_buffer },
    { "cooperation_hint_set",   DBGAPI_CMD_NONE,                 _handle_cooperation_hint_set },
    /* V1.A.2 - symbol management Tools (4 tools) */
    { "symbol_add",             DBGAPI_CMD_SYMBOL_ADD,           _handle_symbol_add           },
    { "symbol_remove",          DBGAPI_CMD_SYMBOL_REMOVE,        _handle_symbol_remove        },
    { "symbol_lookup",          DBGAPI_CMD_SYMBOL_LOOKUP,        _handle_symbol_lookup        },
    { "symbol_list",            DBGAPI_CMD_SYMBOL_LIST,          _handle_symbol_list          },
    /* V1.A.3 - step out + run_until_* Tools (4 tools) */
    { "step_out",               DBGAPI_CMD_STEP_OUT,             _handle_step_out             },
    { "run_until_raster",       DBGAPI_CMD_GET_RASTER_POS,       _handle_run_until_raster     },
    { "run_until_tstate",       DBGAPI_CMD_GET_RASTER_POS,       _handle_run_until_tstate     },
    { "run_until_event",        DBGAPI_CMD_NONE,                 _handle_run_until_event      },
    /* V1.A.4 - EVENT subscribe + TRAP forwarding Tools (4 tools) */
    { "event_subscribe",        DBGAPI_CMD_NONE,                 _handle_event_subscribe      },
    { "event_unsubscribe",      DBGAPI_CMD_NONE,                 _handle_event_unsubscribe    },
    { "event_poll",             DBGAPI_CMD_NONE,                 _handle_event_poll           },
    { "trap_respond",           DBGAPI_CMD_NONE,                 _handle_trap_respond         },
    /* V1.A.5 - chip-level fault injection Tools (5 tools) */
    { "io_read",                DBGAPI_CMD_IO_READ,              _handle_io_read              },
    { "io_write",               DBGAPI_CMD_IO_WRITE,             _handle_io_write             },
    { "irq_inject",             DBGAPI_CMD_IRQ_INJECT,           _handle_irq_inject           },
    { "nmi_inject",             DBGAPI_CMD_NMI_INJECT,           _handle_nmi_inject           },
    { "mem_write_force",        DBGAPI_CMD_MEM_WRITE_FORCE,      _handle_mem_write_force      },
    /* V1.A.6 - Watch + Callstack + CDL Tools (9 tools) */
    { "watch_add",              DBGAPI_CMD_WATCH_ADD,            _handle_watch_add            },
    { "watch_remove",           DBGAPI_CMD_WATCH_REMOVE,         _handle_watch_remove         },
    { "watch_list",             DBGAPI_CMD_WATCH_LIST,           _handle_watch_list           },
    { "watch_eval",             DBGAPI_CMD_WATCH_EVAL,           _handle_watch_eval           },
    { "callstack_get",          DBGAPI_CMD_GET_CALLSTACK,        _handle_callstack_get        },
    { "cdl_start",              DBGAPI_CMD_CDL_START,            _handle_cdl_start            },
    { "cdl_stop",               DBGAPI_CMD_CDL_STOP,             _handle_cdl_stop             },
    { "cdl_reset",              DBGAPI_CMD_CDL_RESET,            _handle_cdl_reset            },
    { "cdl_export",             DBGAPI_CMD_CDL_EXPORT,           _handle_cdl_export           },
    /* V1.A.7 - Profiler Tools (5 tools) */
    { "profiler_start",         DBGAPI_CMD_PROFILER_SET_ACTIVE,  _handle_profiler_start       },
    { "profiler_stop",          DBGAPI_CMD_PROFILER_SET_ACTIVE,  _handle_profiler_stop        },
    { "profiler_reset",         DBGAPI_CMD_PROFILER_RESET,       _handle_profiler_reset       },
    { "profiler_export",        DBGAPI_CMD_PROFILER_EXPORT,      _handle_profiler_export      },
    { "profiler_get",           DBGAPI_CMD_GET_PROFILER,         _handle_profiler_get         },
    /* V1.B.1 - Media Tools (5 tools) */
    { "media_load_mzf",         DBGAPI_CMD_MEDIA_LOAD_MZF,       _handle_media_load_mzf       },
    { "media_load_binary",      DBGAPI_CMD_MEDIA_LOAD_BINARY,    _handle_media_load_binary    },
    { "media_insert",           DBGAPI_CMD_MEDIA_INSERT,         _handle_media_insert         },
    { "media_eject",            DBGAPI_CMD_MEDIA_EJECT,          _handle_media_eject          },
    { "media_state",            DBGAPI_CMD_MEDIA_STATE,          _handle_media_state          },
    /* V1.B.2 - Platform + Config Tools (5 tools) */
    { "settings_set",           DBGAPI_CMD_SETTINGS_SET,         _handle_settings_set         },
    { "settings_get",           DBGAPI_CMD_SETTINGS_GET,         _handle_settings_get         },
    { "platform_set",           DBGAPI_CMD_PLATFORM_SET,         _handle_platform_set         },
    { "periph_attach",          DBGAPI_CMD_PERIPH_ATTACH,        _handle_periph_attach        },
    { "periph_detach",          DBGAPI_CMD_PERIPH_DETACH,        _handle_periph_detach        },
    /* V1.B.3 - hot-swap workflow (pipe transport only) */
    { "emu_stop",               DBGAPI_CMD_NONE,                 _handle_emu_stop             },
    /* V1.C.1 - HID Tools (6 tools) */
    { "input_send_key",                 DBGAPI_CMD_INPUT_PRESS_KEY, _handle_input_send_key              },
    { "input_send_keys",                DBGAPI_CMD_INPUT_PRESS_KEY, _handle_input_send_keys             },
    { "input_press_key",                DBGAPI_CMD_INPUT_PRESS_KEY, _handle_input_press_key             },
    { "input_release_key",              DBGAPI_CMD_INPUT_RELEASE_KEY, _handle_input_release_key         },
    { "input_send_joystick",            DBGAPI_CMD_INPUT_JOY_SET,   _handle_input_send_joystick         },
    { "input_send_keys_with_delays",    DBGAPI_CMD_INPUT_PRESS_KEY, _handle_input_send_keys_with_delays },
    /* V1.D.1 - Core + CPU extras Resources (8 read-only handlers) */
    { "get_config_settings",     DBGAPI_CMD_NONE,                  _handle_get_config_settings    },
    { "get_media_state",         DBGAPI_CMD_MEDIA_STATE,           _handle_get_media_state        },
    { "get_cpu_im2_vector",      DBGAPI_CMD_GET_CPU_IM2_VECTOR,    _handle_get_cpu_im2_vector     },
    { "get_cpu_interrupt_bus",   DBGAPI_CMD_GET_CPU_INTERRUPT_BUS, _handle_get_cpu_interrupt_bus  },
    { "get_cooperation_policy",  DBGAPI_CMD_NONE,                  _handle_get_cooperation_policy },
    { "get_security_profile",    DBGAPI_CMD_NONE,                  _handle_get_security_profile   },
    { "get_memory_map",          DBGAPI_CMD_GET_MEMORY_MAP,        _handle_get_memory_map         },
    { "get_memext_info",         DBGAPI_CMD_GET_MEMEXT_INFO,       _handle_get_memext_info        },
    /* BACKLOG D - emulation speed control (2 Tools) */
    { "get_speed",               DBGAPI_CMD_GET_SPEED,             _handle_get_speed              },
    { "set_speed",               DBGAPI_CMD_SET_SPEED,             _handle_set_speed              },
    /* V1.D.2.A - Easy reuse Resources (5 read-only handlers) */
    { "get_callstack",           DBGAPI_CMD_GET_CALLSTACK,         _handle_get_callstack          },
    { "get_profiler",            DBGAPI_CMD_GET_PROFILER,          _handle_get_profiler           },
    { "get_symbols",             DBGAPI_CMD_SYMBOL_LIST,           _handle_get_symbols            },
    { "get_stack_history",       DBGAPI_CMD_STACK_HISTORY_GET,     _handle_get_stack_history      },
    { "get_stack_regions",       DBGAPI_CMD_STACK_REGIONS_LIST,    _handle_get_stack_regions      },
    /* V1.D.2.B - Medium debug Resources reuse subset (2 read-only handlery
     *            nad existujícími DBGAPI_CMD wrappery; vars + bookmarks
     *            přidají další 2 entries v rámci téže fáze, ale ve
     *            samostatném commitu). */
    { "get_watch",               DBGAPI_CMD_WATCH_LIST,            _handle_get_watch              },
    { "get_stack",               DBGAPI_CMD_STACK_DUMP,            _handle_get_stack              },
    /* V1.D.2.B krok 2+3 - Medium debug Resources s novým CMD (bp_vars + bookmarks) */
    { "get_vars",                DBGAPI_CMD_BP_VARS_LIST,          _handle_get_vars               },
    { "get_bookmarks",           DBGAPI_CMD_BOOKMARKS_LIST,        _handle_get_bookmarks          },
    /* BACKLOG B - bookmark write (2 Tools) */
    { "bookmark_add",            DBGAPI_CMD_BOOKMARK_ADD,          _handle_bookmark_add           },
    { "bookmark_remove",         DBGAPI_CMD_BOOKMARK_REMOVE,       _handle_bookmark_remove        },
    /* CMT-A - CMT transport + recording + cmthack toggle (3 cmds) */
    { "cmt_transport",           DBGAPI_CMD_CMT_TRANSPORT,         _handle_cmt_transport          },
    { "cmt_record",              DBGAPI_CMD_CMT_RECORD,            _handle_cmt_record             },
    { "cmt_hack_set",            DBGAPI_CMD_CMT_HACK_SET,          _handle_cmt_hack_set           },
    /* CMT-B - CMT vlastnosti + práce s páskou (5 cmds) */
    { "cmt_set_property",        DBGAPI_CMD_CMT_SET_PROPERTY,      _handle_cmt_set_property       },
    { "cmt_open",                DBGAPI_CMD_CMT_OPEN,              _handle_cmt_open               },
    { "cmt_tape_seek",           DBGAPI_CMD_CMT_TAPE_SEEK,         _handle_cmt_tape_seek          },
    { "cmt_tape_block_speed",    DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED,  _handle_cmt_tape_block_speed   },
    { "cmt_tape_list",           DBGAPI_CMD_CMT_TAPE_LIST,         _handle_cmt_tape_list          },
    /* V1.D.3.A - IRQ chip Resources (3 read-only handlery) */
    { "get_periph_i8255",        DBGAPI_CMD_GET_PERIPH_I8255,      _handle_get_periph_i8255       },
    { "get_periph_i8253",        DBGAPI_CMD_GET_PERIPH_I8253,      _handle_get_periph_i8253       },
    { "get_periph_z80_pio",      DBGAPI_CMD_GET_PERIPH_Z80_PIO,    _handle_get_periph_z80_pio     },
    /* V1.D.3.B - Audio chip Resources (3 read-only handlery) */
    { "get_periph_sn76489",      DBGAPI_CMD_GET_PERIPH_SN76489,    _handle_get_periph_sn76489     },
    { "get_periph_ay3_8910",     DBGAPI_CMD_GET_PERIPH_AY3_8910,   _handle_get_periph_ay3_8910    },
    { "get_periph_beeper",       DBGAPI_CMD_GET_PERIPH_BEEPER,     _handle_get_periph_beeper      },
    /* V1.D.3.C - Storage + display Resources (4 read-only handlery) */
    { "get_periph_gdg",          DBGAPI_CMD_GET_PERIPH_GDG,        _handle_get_periph_gdg         },
    { "get_periph_wd1793",       DBGAPI_CMD_GET_PERIPH_WD1793,     _handle_get_periph_wd1793      },
    { "get_periph_cmt",          DBGAPI_CMD_GET_PERIPH_CMT,        _handle_get_periph_cmt         },
    { "get_periph_qd",           DBGAPI_CMD_GET_PERIPH_QD,         _handle_get_periph_qd          },
    /* V1.D.4 - Input + frame Resources (7 read-only handlery) */
    { "get_input_keyboard_state",       DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE,       _handle_get_input_keyboard_state       },
    { "get_input_keyboard_matrix_info", DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO, _handle_get_input_keyboard_matrix_info },
    { "get_input_joystick_state",       DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE,       _handle_get_input_joystick_state       },
    { "get_frame_framebuffer_info",     DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO,     _handle_get_frame_framebuffer_info     },
    { "get_frame_screenshot_raw",       DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW,       _handle_get_frame_screenshot_raw       },
    { "get_frame_screenshot",           DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG,       _handle_get_frame_screenshot           },
    { "get_video_text_dump",            DBGAPI_CMD_GET_VIDEO_TEXT_DUMP,            _handle_get_video_text_dump            },
    /* V1.D.2.C - per-watch snapshot Resource backing (mirror lookup) */
    { "get_watch_snapshot",             DBGAPI_CMD_GET_WATCH_SNAPSHOT,             _handle_get_watch_snapshot             },
    /* Doplněno mimo původní seskupení per CPU registry kvůli stabilitě
     * pořadí v hello supported_commands listu (= existující testy
     * indexují cmd_map pozičně). */
    { "set_register",    DBGAPI_CMD_SET_REG,           _handle_set_register  },
    { "dasm",            DBGAPI_CMD_DASM,              _handle_dasm          },
    { "history_get",     DBGAPI_CMD_HISTORY_GET,       _handle_history_get   },
    /* V1.E.2 - CPU control + details Tools (Commit 1: 5 jednoduchých) */
    { "get_reg",                DBGAPI_CMD_GET_REG,                 _handle_get_reg                },
    { "force_pause",            DBGAPI_CMD_FORCE_PAUSE,             _handle_force_pause            },
    { "set_user_cycle_origin",  DBGAPI_CMD_SET_USER_CYCLE_ORIGIN,   _handle_set_user_cycle_origin  },
    { "get_im2_vector",         DBGAPI_CMD_GET_IM2_VECTOR,          _handle_get_im2_vector         },
    { "get_raster_pos",         DBGAPI_CMD_GET_RASTER_POS,          _handle_get_raster_pos         },
    /* V1.E.2 - CPU flags (Commit 2: 2 Tools) */
    { "get_cpu_flags",          DBGAPI_CMD_GET_CPU_FLAGS,           _handle_get_cpu_flags          },
    { "set_cpu_flags",          DBGAPI_CMD_SET_CPU_FLAGS,           _handle_set_cpu_flags          },
    /* V1.E.2 - last_instr + panel_batch (Commit 3: 2 Tools) */
    { "get_last_instr",         DBGAPI_CMD_GET_LAST_INSTR,          _handle_get_last_instr         },
    { "get_cpu_panel_batch",    DBGAPI_CMD_GET_CPU_PANEL_BATCH,     _handle_get_cpu_panel_batch    },
    /* V1.E.3 - debugger state Tools (Commit 1: 3 Tools) */
    { "debugger_activate",      DBGAPI_CMD_DEBUGGER_ACTIVATE,       _handle_debugger_activate      },
    { "debugger_deactivate",    DBGAPI_CMD_DEBUGGER_DEACTIVATE,     _handle_debugger_deactivate    },
    { "is_debugger_active",     DBGAPI_CMD_IS_DEBUGGER_ACTIVE,      _handle_is_debugger_active     },
    /* V1.E.3 - PIO-Z80 IM2 vector override (Commit 2: 1 Tool) */
    { "set_pioz80_interrupt_vector", DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR, _handle_set_pioz80_interrupt_vector },
    /* V1.E.4 - BP advanced Tools (Commit 1: 6 Tools) */
    { "bp_create_with_init",    DBGAPI_CMD_BP_CREATE_WITH_INIT, _handle_bp_create_with_init },
    { "bp_set_parent",          DBGAPI_CMD_BP_SET_PARENT,       _handle_bp_set_parent       },
    { "bp_update",              DBGAPI_CMD_BP_UPDATE,           _handle_bp_update           },
    { "bpgrp_add",              DBGAPI_CMD_BPGRP_ADD,           _handle_bpgrp_add           },
    { "bpgrp_remove",           DBGAPI_CMD_BPGRP_REMOVE,        _handle_bpgrp_remove        },
    { "bpgrp_update",           DBGAPI_CMD_BPGRP_UPDATE,        _handle_bpgrp_update        },
    /* V1.E.4 - Stack analytics Tools (Commit 2: 6 Tools) */
    { "stack_history_enable",         DBGAPI_CMD_STACK_HISTORY_ENABLE,         _handle_stack_history_enable         },
    { "stack_history_reset",          DBGAPI_CMD_STACK_HISTORY_RESET,          _handle_stack_history_reset          },
    { "stack_regions_add",            DBGAPI_CMD_STACK_REGIONS_ADD,            _handle_stack_regions_add            },
    { "stack_regions_edit",           DBGAPI_CMD_STACK_REGIONS_EDIT,           _handle_stack_regions_edit           },
    { "stack_regions_remove",         DBGAPI_CMD_STACK_REGIONS_REMOVE,         _handle_stack_regions_remove         },
    { "stack_regions_reset_watermark", DBGAPI_CMD_STACK_REGIONS_RESET_WATERMARK, _handle_stack_regions_reset_watermark },
    /* V1.E.5 - Eventlog/TLOG Tools (6) */
    { "eventlog_start",         DBGAPI_CMD_EVENTLOG_START,        _handle_eventlog_start         },
    { "eventlog_stop",          DBGAPI_CMD_EVENTLOG_STOP,         _handle_eventlog_stop          },
    { "eventlog_clear",         DBGAPI_CMD_EVENTLOG_CLEAR,        _handle_eventlog_clear         },
    { "eventlog_set_capacity",  DBGAPI_CMD_EVENTLOG_SET_CAPACITY, _handle_eventlog_set_capacity  },
    { "eventlog_set_mask",      DBGAPI_CMD_EVENTLOG_SET_MASK,     _handle_eventlog_set_mask      },
    { "eventlog_get_event",     DBGAPI_CMD_EVENTLOG_GET_EVENT,    _handle_eventlog_get_event     },
    /* mzdos-support 0007 - Direct memory region read (2 Tools) */
    /* platform-info-fix - dynamic detection */
    { "get_platform_info",      DBGAPI_CMD_NONE,                  _handle_get_platform_info      },
    { "regions_list",           DBGAPI_CMD_REGIONS_ENUMERATE,     _handle_regions_list           },
    { "region_read",            DBGAPI_CMD_REGIONS_READ,          _handle_region_read            },
    { "region_write",           DBGAPI_CMD_REGIONS_WRITE,         _handle_region_write           },
    /* mzdos request 0009 - server-side zápis PNG screenshotu na disk
     * (obejde velký base64/TCP přenos). Přidáno na KONEC tabulky kvůli
     * stabilitě pozičního indexu v hello supported_commands. dbgapi_cmd
     * informativní; handler interně volá DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG. */
    { "screenshot_save_to_file", DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG, _handle_screenshot_save_to_file },
    /* 0017 FÁZE 1 - Tracking lifecycle (trace-suite). Přidáno na KONEC
     * tabulky kvůli stabilitě pozičního indexu v hello supported_commands. */
    { "trace_start",             DBGAPI_CMD_TRACE_START,              _handle_trace_start             },
    { "trace_stop",              DBGAPI_CMD_TRACE_STOP,               _handle_trace_stop              },
    { "trace_reset",             DBGAPI_CMD_TRACE_RESET,              _handle_trace_reset             },
    { "trace_save",              DBGAPI_CMD_TRACE_SAVE,               _handle_trace_save              },
    /* sentinel */
    { NULL,              DBGAPI_CMD_NONE,              NULL                  },
};

/**
 * @brief Dynamicky alokovaný snapshot jmen z `g_cmd_map[]`.
 *
 * Alokuje se ve `mcp_dispatch_init()`, uvolňuje v
 * `mcp_dispatch_shutdown()`. Pole je NULL-terminated (= count + 1
 * pointerů, každý string g_strdup z odpovídajícího `g_cmd_map[i].name`).
 *
 * Důvod přechodu z původního statického hardcoded pole:
 *   - V0.A.4 - V1.A.5 (= 41 entries) mirror nebyl udržován při přidání
 *     nových V1.A.6 / V1.A.7 / V1.B.1 / V1.B.2 příkazů,
 *   - hello payload pak advertisoval zastaralý seznam (klient si myslel,
 *     že 24 příkazů neexistuje, přestože je dispatch normálně přijímal),
 *   - nález Kontrolora V1.B.2.
 *
 * Pre-init stav: ukazuje na `g_empty_cmd_names` (jen sentinel NULL),
 * takže `mcp_dispatch_get_supported_commands()` je bezpečné volat
 * i bez init.
 */
static const char *g_empty_cmd_names[] = { NULL };
static char       **g_supported_cmd_names = NULL;
static gsize        g_supported_cmd_names_count = 0;


/* ------------------------------------------------------------------ */
/* Interní helpery                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Bezpečně vrátí int hodnotu pole z JsonObject nebo default.
 *
 * Caller předává root JsonObject `obj` (např. z `data_node`). Vrací
 * default pokud pole chybí, je null, není JSON value nebo není int.
 */
static gint64 _obj_int_or(JsonObject *obj, const char *key, gint64 def) {
    if (!obj || !json_object_has_member(obj, key)) return def;
    JsonNode *node = json_object_get_member(obj, key);
    if (!node || json_node_is_null(node)) return def;
    if (json_node_get_node_type(node) != JSON_NODE_VALUE) return def;
    return json_node_get_int(node);
}


/**
 * @brief Helper - sestaví error RESPONSE řádek s daným textem.
 *
 * Wrap kolem `jsonl_build_response(.., success=false, NULL, msg)`.
 * Vrací MCP_DISPATCH_OK pouze pokud se podařilo string sestavit.
 *
 * Volání: `return _err_response(req_id, "Invalid parameters",
 *         MCP_DISPATCH_INVALID_PARAMS, out_response);`
 */
static en_MCP_DISPATCH_RESULT _err_response(int64_t req_id,
                                            const char *msg,
                                            en_MCP_DISPATCH_RESULT rc,
                                            char **out_response) {
    char *line = jsonl_build_response(req_id, false, NULL, msg);
    if (!line) {
        *out_response = NULL;
        return MCP_DISPATCH_ALLOC_ERROR;
    }
    *out_response = line;
    return rc;
}


/**
 * @brief Helper - sestaví success RESPONSE z předaného JsonObject.
 *
 * Vlastnictví: funkce zabalí `data_obj` do nového `JsonNode`, předá ho
 * `jsonl_build_response` (která dělá deep-copy), a poté vlastní node i
 * obj uvolní. Caller už nesmí `data_obj` použít po návratu.
 *
 * @param[in] req_id     ID requestu
 * @param[in] data_obj   JsonObject s payload (vlastnictví přejímá funkce);
 *                       NULL pro response bez data
 * @param[out] out_response výstup
 */
static en_MCP_DISPATCH_RESULT _ok_response(int64_t req_id,
                                           JsonObject *data_obj,
                                           char **out_response) {
    char *line = NULL;
    if (data_obj) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, data_obj);
        line = jsonl_build_response(req_id, true, node, NULL);
        json_node_free(node);   /* uvolní i náš data_obj (take_object) */
    } else {
        line = jsonl_build_response(req_id, true, NULL, NULL);
    }
    if (!line) {
        *out_response = NULL;
        return MCP_DISPATCH_ALLOC_ERROR;
    }
    *out_response = line;
    return MCP_DISPATCH_OK;
}


/**
 * @brief Volá dbgapi submit s origin=MCP a default timeout.
 *
 * Zkratka pro `dbgapi_ui_submit_cmd_sync_with_origin(&g_dbgapi_cmdrq_queue,
 *   cmd, DBGAPI_CMD_ORIGIN_MCP, data, result, MCP_DISPATCH_DBGAPI_TIMEOUT_MS)`.
 *
 * @return true při úspěchu (rq->success), false jinak (timeout / queue
 *         full / emu ending / handler vrátil success=false)
 */
static bool _submit_dbgapi(en_DBGAPI_CMD cmd, void *data, void *result) {
    return dbgapi_ui_submit_cmd_sync_with_origin(&g_dbgapi_cmdrq_queue,
                                                  cmd,
                                                  DBGAPI_CMD_ORIGIN_MCP,
                                                  data,
                                                  result,
                                                  MCP_DISPATCH_DBGAPI_TIMEOUT_MS);
}


/* ------------------------------------------------------------------ */
/* Handlery                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief `ping` handler - lokální, vrátí `{"pong": true}`.
 *
 * Žádný dbgapi call. Slouží k ověření, že MCP backend žije a že JSONL
 * roundtrip funguje. Neblokuje emulátor.
 */
static en_MCP_DISPATCH_RESULT _handle_ping(const st_JSONL_MESSAGE *req,
                                           char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "pong", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `get_state` handler - vrací stav emulace.
 *
 * Volá `DBGAPI_CMD_IS_RUNNING` (result: bool*). Sestaví response s
 * polem `paused` (= `!is_running`). Frame counter / cycles by mělo
 * přidat V0.B (vyžaduje GET_RASTER_POS nebo equivalentní dbgapi call).
 *
 * Při selhání dbgapi sestaví error response.
 */
static en_MCP_DISPATCH_RESULT _handle_get_state(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    bool is_running = false;
    if (!_submit_dbgapi(DBGAPI_CMD_IS_RUNNING, NULL, &is_running)) {
        return _err_response(req_id, "Emulator unavailable",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "running", is_running);
    json_object_set_boolean_member(data, "paused", !is_running);

    /* V1.D.1 - last_user_action field. Pokud žádná USER akce nebyla,
     * vystavíme JSON null (= AI vidí "user nic neudělal od startu emu"). */
    en_DBGAPI_CMD last_cmd = DBGAPI_CMD_NONE;
    uint64_t      last_ts  = 0;
    bool          have_lua = dbgapi_get_last_user_action(&last_cmd, &last_ts);
    if (have_lua) {
        JsonObject *lua = json_object_new();
        json_object_set_string_member(lua, "kind",
                                       dbgapi_cmd_to_str(last_cmd));
        json_object_set_int_member(lua, "timestamp_us", (gint64)last_ts);
        json_object_set_object_member(data, "last_user_action", lua);
    } else {
        json_object_set_null_member(data, "last_user_action");
    }
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `pause` handler - pozastaví emulaci.
 *
 * Proxy na `DBGAPI_CMD_PAUSE`. Vrací `{"paused": true}` při úspěchu.
 */
static en_MCP_DISPATCH_RESULT _handle_pause(const st_JSONL_MESSAGE *req,
                                            char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_PAUSE, NULL, NULL)) {
        return _err_response(req_id, "Pause failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "paused", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `run` handler - spustí emulaci.
 *
 * Dvě cesty podle parametru `frames`:
 *  - **Async (legacy)** - frames chybí nebo == 0: jen submit
 *    `DBGAPI_CMD_RUN` (= unpause), response `{"running": true}`.
 *    Emulátor běží asynchronně dál.
 *  - **Blokující (deterministická)** - frames > 0: submit
 *    `DBGAPI_CMD_RUN_FRAMES`. Emu vlákno samo deterministicky doběhne N
 *    framů a pausne se přesně na N-té frame hranici (= emu-side stop
 *    v hot loopu, mzarch.c). Dispatch jen počká, až emu doběhne, přes
 *    `_dispatch_wait_run_frames_done`. Response obsahuje
 *    `running: false`, `actual_frames: N`. Frames se clampuje na
 *    rozsah 1..1000.
 *
 * Determinismus (mcp-debug-control request 0021): dříve blokující path
 * dělala RUN + `_dispatch_wait_frames` + async PAUSE. Mezi okamžikem
 * "video counter dosáhl N" a zpracováním PAUSE emu vláknem ale emu běžel
 * dál o wall-clock-závislý počet instrukcí -> zastavoval se na
 * nedeterministickém cycle bodě (empiricky kolísal koncový PC i IR při
 * konstantním actual_frames). Nová cesta nechá emu vlákno zastavit se
 * SAMO přesně po dokončení N-tého framu (vzor g_debugger.step_call), takže
 * koncový stav je deterministický.
 *
 * Safety fallback: pokud `_dispatch_wait_run_frames_done` skončí
 * safety timeoutem (= emu nestihl N framů, např. havaroval / deadlock),
 * emu může stále běžet s aktivním frame-bounded runem. V tom případě
 * pošleme `DBGAPI_CMD_PAUSE` jako pojistku, aby klient dostal zastavený
 * stav. PAUSE handler nezruší `run_frames_active` flag, ale to nevadí:
 * další RUN_FRAMES ho přepíše a obyčejný RUN/PAUSE ho nečte. Pokud by
 * mezitím emu doběhl cíl, frame-bounded check zkrátka pausne podruhé
 * (idempotentní).
 *
 * Pro detailní synchronizaci viz `_dispatch_wait_run_frames_done` doxy.
 *
 * Chyba: 422 (INVALID_PARAMS) pokud frames < 0 nebo > 1000.
 *        500 (EMU_ERROR) pokud dbgapi submit selže.
 */
static en_MCP_DISPATCH_RESULT _handle_run(const st_JSONL_MESSAGE *req,
                                          char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);

    /* Parse frames z optional data objektu. Default 0 = async (legacy). */
    gint64 frames = 0;
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (data_node
        && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *obj = json_node_get_object(data_node);
        frames = _obj_int_or(obj, "frames", 0);
    }

    /* Validace rozsahu. Horní limit 1000 (= match mcp_server.py Tool
     * argument validation). frames == 0 explicit = async path. */
    if (frames < 0 || frames > 1000) {
        return _err_response(req_id, "frames must be in range 0..1000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    if (frames == 0) {
        /* Async path (legacy) - jen unpause, response `running: true`.
         * Side effect: emulator_pause(false) + audio resume + UI update. */
        if (!_submit_dbgapi(DBGAPI_CMD_RUN, NULL, NULL)) {
            return _err_response(req_id, "Run failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        JsonObject *data = json_object_new();
        json_object_set_boolean_member(data, "running", TRUE);
        return _ok_response(req_id, data, out_response);
    }

    /* Blokující deterministická path. data_ptr je int* = počet framů.
     * Lokální proměnná je platná po celou dobu _submit_dbgapi (= synchronní
     * blokující call, počká na zpracování emu vláknem - viz
     * dbgapi_ui_submit_cmd_sync_with_origin). RUN_FRAMES handler nastaví
     * cílový screens counter a unpausne; emu se sám deterministicky pausne. */
    int frames_int = (int)frames;
    /* Změř výchozí screens counter PŘED spuštěním (emu je zde paused, takže
     * g_gdg.total_elapsed.screens je stabilní). actual_frames měříme jako delta
     * TÉHOŽ counteru, podle kterého se emu deterministicky zastaví
     * (run_frames_target = screens + N) - NE podle fbsnapshot_screen_id, který
     * počítá jen skutečně vykreslené framebuffery (řidší, závislé na video módu),
     * takže by actual neodpovídal requested. */
    uint32_t start_screens = (uint32_t)g_gdg.total_elapsed.screens;
#ifndef MZ800EMU_MCP_TEST_BUILD
    /* Vynuluj důvod pauzy - po doběhnutí ho přečteme do stopped_by. Emu je
     * zde paused (stojí), takže zápis g_emulator.pause_reason je bezpečný.
     * (V testovacím buildu g_emulator/emulator.h není dostupné.) */
    g_emulator.pause_reason = EMU_PAUSE_REASON_NONE;
#endif
    if (!_submit_dbgapi(DBGAPI_CMD_RUN_FRAMES, &frames_int, NULL)) {
        return _err_response(req_id, "Run failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    /* Počkej, až emu sám doběhne N framů a pausne se. Vrací true pokud
     * emu doběhl deterministicky (= je paused), false při safety timeoutu. */
    bool done = _dispatch_wait_run_frames_done(frames_int, NULL);

    /* Fallback: pokud wait skončil safety timeoutem, emu možná stále běží
     * s aktivním frame-bounded runem (run_frames_active=1). Pošleme PAUSE
     * jako pojistku, aby klient dostal zastavený stav (anti-hang). Při
     * deterministickém doběhu je emu už paused a PAUSE se neposílá (= žádný
     * extra cycle navíc, zachová se přesný deterministický stop). */
    bool pause_ok = true;
    if (!done) {
        pause_ok = _submit_dbgapi(DBGAPI_CMD_PAUSE, NULL, NULL);
    }

    /* actual_frames = delta screens counteru, čteno po pauze (emu stabilní →
     * přesné). Při deterministickém doběhu je to přesně N (emu pausnul při
     * screens == start_screens + N); při safety timeoutu skutečný počet
     * proběhlých screens. */
    int actual = (int)((uint32_t)g_gdg.total_elapsed.screens - start_screens);

    /* stopped_by: proč se emulace zastavila. !done = safety timeout; jinak
     * důvod, který nastavil ten, kdo emu pausnul (frame target / breakpoint /
     * manuální pauza). "unknown" = emu se zastavil z jiného důvodu, který
     * pause_reason nenastavuje (HALT, fatal). */
    const char *stopped_by;
    bool reached;
    if (!done) {
        stopped_by = "timeout";
        reached = false;
    } else {
#ifndef MZ800EMU_MCP_TEST_BUILD
        switch (g_emulator.pause_reason) {
            case EMU_PAUSE_REASON_FRAMES:     stopped_by = "frames";     break;
            case EMU_PAUSE_REASON_BREAKPOINT: stopped_by = "breakpoint"; break;
            case EMU_PAUSE_REASON_MANUAL:     stopped_by = "manual";     break;
            default:                          stopped_by = "unknown";    break;
        }
        reached = (g_emulator.pause_reason == EMU_PAUSE_REASON_FRAMES);
#else
        /* Test build: wait je no-op (považováno za úplný doběh). */
        stopped_by = "frames";
        reached = true;
#endif
    }

    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "running", FALSE);
    json_object_set_int_member(data, "actual_frames", actual);
    json_object_set_int_member(data, "requested_frames", frames);
    json_object_set_string_member(data, "stopped_by", stopped_by);
    /* complete = doběhl požadovaný počet framů (= stopped_by "frames").
     * Při zastavení breakpointem / manuální pauzou / safety timeoutu je
     * false, i když pause_ok - klient tak rozliší úplný doběh od přerušení
     * (dřív bylo true i při BP, mzdos 0022). */
    json_object_set_boolean_member(data, "complete", done && pause_ok && reached);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `reset` handler - resetuje CPU.
 *
 * Proxy na `DBGAPI_CMD_RESET`. Vrací `{"ok": true}` při úspěchu.
 */
static en_MCP_DISPATCH_RESULT _handle_reset(const st_JSONL_MESSAGE *req,
                                            char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_RESET, NULL, NULL)) {
        return _err_response(req_id, "Reset failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "ok", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `get_registers` handler - čte všechny Z80 registry.
 *
 * Proxy na `DBGAPI_CMD_GET_ALL_REGS`. Result = pole `uint16_t[14]` v
 * pořadí `z80_reg_t` (= 0=AF, 1=BC, 2=DE, 3=HL, 4=AF', 5=BC', 6=DE',
 * 7=HL', 8=IX, 9=IY, 10=SP, 11=PC, 12=WZ, 13=IR).
 *
 * Sestaví JSON objekt s pojmenovanými poli (decimální int hodnoty,
 * klient si vytiskne hex sám). Tento formát odpovídá JSONL stylu z
 * `cputrack.c` (= AF, BC, ..., PC, IR).
 */
static en_MCP_DISPATCH_RESULT _handle_get_registers(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    uint16_t regs[DBGAPI_REG_COUNT];
    memset(regs, 0, sizeof(regs));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_ALL_REGS, NULL, regs)) {
        return _err_response(req_id, "GET_ALL_REGS failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "AF",  regs[0]);
    json_object_set_int_member(data, "BC",  regs[1]);
    json_object_set_int_member(data, "DE",  regs[2]);
    json_object_set_int_member(data, "HL",  regs[3]);
    json_object_set_int_member(data, "AF_", regs[4]);
    json_object_set_int_member(data, "BC_", regs[5]);
    json_object_set_int_member(data, "DE_", regs[6]);
    json_object_set_int_member(data, "HL_", regs[7]);
    json_object_set_int_member(data, "IX",  regs[8]);
    json_object_set_int_member(data, "IY",  regs[9]);
    json_object_set_int_member(data, "SP",  regs[10]);
    json_object_set_int_member(data, "PC",  regs[11]);
    json_object_set_int_member(data, "WZ",  regs[12]);
    json_object_set_int_member(data, "IR",  regs[13]);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief Mapuje název Z80 registru na `z80_reg_t` enum.
 *
 * Akceptuje case-insensitive jména shodná s `get_registers` response:
 * AF, BC, DE, HL, AF_, BC_, DE_, HL_, IX, IY, SP, PC, WZ, IR.
 * Toleruje alias AF2/BC2/DE2/HL2 pro alternates.
 *
 * @param name vstupní jméno (NUL-terminated, max 8 znaků)
 * @param out_id výstupní reg_id pokud match
 * @return true pokud nalezeno, false pokud unknown
 */
static bool _parse_reg_name(const char *name, uint8_t *out_id) {
    if (!name || !out_id) return false;
    /* Normalizace - upper-case, trim. Stub bez allocace, max 4 znaky. */
    char buf[8] = {0};
    int n = 0;
    for (int i = 0; name[i] && n < (int)(sizeof(buf) - 1); i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[n++] = c;
    }
    buf[n] = '\0';
    struct { const char *n; uint8_t id; } map[] = {
        { "AF", 0 }, { "BC", 1 }, { "DE", 2 }, { "HL", 3 },
        { "AF_", 4 }, { "BC_", 5 }, { "DE_", 6 }, { "HL_", 7 },
        { "AF2", 4 }, { "BC2", 5 }, { "DE2", 6 }, { "HL2", 7 },
        { "IX", 8 }, { "IY", 9 }, { "SP", 10 }, { "PC", 11 },
        { "WZ", 12 }, { "IR", 13 },
        { NULL, 0 }
    };
    for (int i = 0; map[i].n; i++) {
        if (strcmp(buf, map[i].n) == 0) {
            *out_id = map[i].id;
            return true;
        }
    }
    return false;
}


/**
 * @brief `set_register` handler - zápis 16bitové hodnoty do Z80 registru.
 *
 * Parametry v `data` poli requestu:
 *  - `reg` (string) - jméno registru (case-insensitive), viz
 *    `_parse_reg_name` pro akceptovaná jména
 *  - `value` (int, 0..65535) - nová hodnota (pro 8bitové registry I/R
 *    je relevantní jen dolní bajt, pro IR registr backend nastavuje
 *    jen R, I se zachová)
 *
 * Response payload pri uspechu:
 *  - `reg` (string) - echo
 *  - `value` (int) - echo
 *
 * Side effects: mění CPU state, klient by měl typicky následně volat
 * `get_registers` pro verifikaci.
 */
static en_MCP_DISPATCH_RESULT _handle_set_register(const st_JSONL_MESSAGE *req,
                                                   char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    const gchar *reg_name = NULL;
    if (json_object_has_member(data_obj, "reg")) {
        JsonNode *r = json_object_get_member(data_obj, "reg");
        if (json_node_get_value_type(r) == G_TYPE_STRING) {
            reg_name = json_object_get_string_member(data_obj, "reg");
        }
    }
    gint64 value = _obj_int_or(data_obj, "value", -1);
    if (!reg_name || value < 0 || value > 0xFFFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint8_t reg_id = 0;
    if (!_parse_reg_name(reg_name, &reg_id)) {
        return _err_response(req_id, "Unknown register name",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_REG_PARAM param = {
        .reg_id = reg_id,
        .value  = (uint16_t)value,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_SET_REG, &param, NULL)) {
        return _err_response(req_id, "SET_REG failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_string_member(data, "reg", reg_name);
    json_object_set_int_member(data, "value", value);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `dasm` handler - disassembluje N po sobě jdoucích instrukcí.
 *
 * Parametry v `data` poli requestu:
 *  - `addr` (int, 0..65535) - počáteční adresa
 *  - `count` (int, 1..256) - počet instrukcí
 *
 * Response payload:
 *  - `addr` (int) - echo
 *  - `count` (int) - echo
 *  - `lines` (array) - jedna položka per instrukce:
 *    - `addr` (int) - adresa instrukce
 *    - `bytes_hex` (string) - bajty instrukce jako hex (např. "CD 34 12")
 *    - `num_bytes` (int) - délka instrukce (1..4)
 *    - `mnemonic` (string) - textová mnemonika z `z80_dasm_to_str`
 *
 * Side-effect free, používá banking-aware read přes
 * `debugger_dasm_read_cb`.
 */
static en_MCP_DISPATCH_RESULT _handle_dasm(const st_JSONL_MESSAGE *req,
                                           char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 addr  = _obj_int_or(data_obj, "addr",  -1);
    gint64 count = _obj_int_or(data_obj, "count", -1);
    if (addr < 0 || addr > 0xFFFF || count < 1 || count > 256) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_DASM_PARAM param = {
        .addr  = (uint16_t)addr,
        .count = (int)count,
    };
    st_DBGAPI_DASM_RESULT *results =
        g_malloc0(sizeof(st_DBGAPI_DASM_RESULT) * (gsize)count);
    if (!_submit_dbgapi(DBGAPI_CMD_DASM, &param, results)) {
        g_free(results);
        return _err_response(req_id, "DASM failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "addr",  addr);
    json_object_set_int_member(data, "count", count);
    JsonArray *lines = json_array_new();
    for (int i = 0; i < count; i++) {
        JsonObject *line = json_object_new();
        json_object_set_int_member(line, "addr", results[i].addr);
        json_object_set_int_member(line, "num_bytes", results[i].num_bytes);
        /* Bajty jako mezerami oddělený hex (1-4 bytes). */
        char hex_buf[16] = {0};
        int hex_pos = 0;
        for (int b = 0; b < results[i].num_bytes && b < 4; b++) {
            hex_pos += g_snprintf(hex_buf + hex_pos,
                                  sizeof(hex_buf) - hex_pos,
                                  (b == 0) ? "%02X" : " %02X",
                                  results[i].bytes[b]);
        }
        json_object_set_string_member(line, "bytes_hex", hex_buf);
        json_object_set_string_member(line, "mnemonic", results[i].mnemonic);
        json_array_add_object_element(lines, line);
    }
    json_object_set_array_member(data, "lines", lines);
    g_free(results);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `history_get` handler - vrátí debugger history ring buffer.
 *
 * Bez parametrů. Vrátí pole 32 posledních dokončených instrukcí
 * z `g_debugger_history` ring bufferu. Pro každou položku jen
 * adresa + bajty (mnemonic se rekonstruuje klientem přes
 * `emu_dasm` pokud potřebuje).
 *
 * Response payload:
 *  - `current_position` (int) - aktuální index zápisu v ringu (=
 *    `g_debugger_history.position` mod 32, klient si může spočítat
 *    chronologické pořadí od nejstaršího po nejnovější)
 *  - `length` (int) - počet entries (vždy 32)
 *  - `entries` (array) - per entry:
 *    - `addr` (int) - adresa instrukce
 *    - `bytes_hex` (string) - max 4 bajty jako hex
 *
 * Pole `entries[i]` odpovídá `g_debugger_history.row[i]` (= raw
 * ring layout, NE chronologicky seřazený). Klient si zorientuje
 * pomocí `current_position`.
 */
static en_MCP_DISPATCH_RESULT _handle_history_get(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DEBUGGER_HISTORY_ROW rows[DEBUGGER_HISTORY_LENGTH];
    memset(rows, 0, sizeof(rows));
    if (!_submit_dbgapi(DBGAPI_CMD_HISTORY_GET, NULL, rows)) {
        return _err_response(req_id, "HISTORY_GET failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "current_position",
                               (gint64)(g_debugger_history.position
                                        & DEBUGGER_HISTORY_POSMASK));
    json_object_set_int_member(data, "length",
                               (gint64)DEBUGGER_HISTORY_LENGTH);
    JsonArray *entries = json_array_new();
    for (int i = 0; i < DEBUGGER_HISTORY_LENGTH; i++) {
        JsonObject *e = json_object_new();
        json_object_set_int_member(e, "addr", rows[i].addr);
        char hex_buf[16] = {0};
        int hex_pos = 0;
        for (int b = 0; b < DEBUGGER_MAX_INSTR_BYTES; b++) {
            hex_pos += g_snprintf(hex_buf + hex_pos,
                                  sizeof(hex_buf) - hex_pos,
                                  (b == 0) ? "%02X" : " %02X",
                                  rows[i].byte[b]);
        }
        json_object_set_string_member(e, "bytes_hex", hex_buf);
        json_array_add_object_element(entries, e);
    }
    json_object_set_array_member(data, "entries", entries);
    return _ok_response(req_id, data, out_response);
}


/* ============================================================================
 *  V1.E.2 - CPU control + details Tools (Commit 1 - 5 jednoduchých Tools)
 *
 *  Sekce vystavuje 5 jednoduchých backendů jako MCP Tools nad existujícími
 *  dbgapi handlery. Reuse `_parse_reg_name` pro `get_reg`, ostatní bez
 *  parametrů nebo s minimálním JSON payloadem.
 * ============================================================================ */


/**
 * @brief `get_reg` handler - čte hodnotu jednoho Z80 registru.
 *
 * Parametry v `data` poli requestu:
 *  - `reg` (string) - jméno registru (case-insensitive), parsuje
 *    `_parse_reg_name` (= akceptuje AF, BC, DE, HL, AF_/AF2, BC_/BC2,
 *    DE_/DE2, HL_/HL2, IX, IY, SP, PC, WZ, IR).
 *
 * Response payload:
 *  - `reg` (string) - echo vstupního jména
 *  - `value` (int) - 16bitová hodnota registru (pro IR vrací kompozit
 *    (I << 8) | R z backendu).
 *
 * Side-effect free, používá `DBGAPI_CMD_GET_REG` s reg_id na vstupu
 * a uint16_t na výstupu.
 */
static en_MCP_DISPATCH_RESULT _handle_get_reg(const st_JSONL_MESSAGE *req,
                                              char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    const gchar *reg_name = NULL;
    if (json_object_has_member(data_obj, "reg")) {
        JsonNode *r = json_object_get_member(data_obj, "reg");
        if (json_node_get_value_type(r) == G_TYPE_STRING) {
            reg_name = json_object_get_string_member(data_obj, "reg");
        }
    }
    if (!reg_name) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint8_t reg_id = 0;
    if (!_parse_reg_name(reg_name, &reg_id)) {
        return _err_response(req_id, "Unknown register name",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint16_t value = 0;
    if (!_submit_dbgapi(DBGAPI_CMD_GET_REG, &reg_id, &value)) {
        return _err_response(req_id, "GET_REG failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_string_member(data, "reg",   reg_name);
    json_object_set_int_member(data,    "value", (gint64)value);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `force_pause` handler - vynucená pauza emulace (bypass cesta).
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_FORCE_PAUSE`. V aktuální
 * implementaci shodné s `pause` (= `DBGAPI_CMD_PAUSE`), liší se jen
 * sémanticky - kdyby v budoucnu vznikl pause-blokující kontext, force
 * variant ho obejde. Vrací `{"paused": true}` při úspěchu.
 */
static en_MCP_DISPATCH_RESULT _handle_force_pause(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_FORCE_PAUSE, NULL, NULL)) {
        return _err_response(req_id, "Force pause failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "paused", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `set_user_cycle_origin` handler - reset User cycle counter origin.
 *
 * Parametry v `data` poli requestu (volitelné):
 *  - `value` (int, 0..0xFFFFFFFF) - absolutní snapshot total_cycles,
 *    který se uloží jako nový origin. Pokud parametr chybí nebo je
 *    záporný, handler použije aktuální `cpu->total_cycles` (= "Reset
 *    na 0" varianta).
 *
 * Response payload:
 *  - `reset` (bool) - vždy true při úspěchu
 *  - `origin` (int) - hodnota, která byla zapsána do
 *    `g_debugger.user_cycle_origin`.
 *
 * Side effects: mění `g_debugger.user_cycle_origin`. Klient typicky
 * volá `get_cpu_flags` nebo `get_raster_pos` pro verifikaci.
 */
static en_MCP_DISPATCH_RESULT _handle_set_user_cycle_origin(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);

    /* Výchozí origin = aktuální total_cycles. Pokud klient nepředá
     * `value`, snapshot uděláme přes GET_CPU_FLAGS (pohodlnější než
     * přímý read mimo emu vlákno). */
    uint32_t new_origin = 0;
    bool have_explicit = false;
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        if (json_object_has_member(data_obj, "value")) {
            gint64 v = _obj_int_or(data_obj, "value", -1);
            if (v < 0 || v > 0xFFFFFFFFLL) {
                return _err_response(req_id, "Invalid parameters",
                                     MCP_DISPATCH_INVALID_PARAMS, out_response);
            }
            new_origin = (uint32_t)v;
            have_explicit = true;
        }
    }
    if (!have_explicit) {
        st_DBGAPI_CPU_FLAGS flags;
        memset(&flags, 0, sizeof(flags));
        if (!_submit_dbgapi(DBGAPI_CMD_GET_CPU_FLAGS, NULL, &flags)) {
            return _err_response(req_id, "GET_CPU_FLAGS failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        new_origin = flags.total_cycles;
    }
    if (!_submit_dbgapi(DBGAPI_CMD_SET_USER_CYCLE_ORIGIN, &new_origin, NULL)) {
        return _err_response(req_id, "SET_USER_CYCLE_ORIGIN failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "reset",  TRUE);
    json_object_set_int_member(data,    "origin", (gint64)new_origin);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `get_im2_vector` handler - Tool varianta IM2 ISR vector snapshotu.
 *
 * Tool varianta nad `DBGAPI_CMD_GET_IM2_VECTOR` (= jiný backend než
 * Resource handler `get_cpu_im2_vector`, který používá
 * `DBGAPI_CMD_GET_CPU_IM2_VECTOR`; oba čtou stejnou fyzikální oblast
 * stavu, ale liší se layoutem struct + sémantikou polí).
 *
 * Bez parametrů. Response payload:
 *  - `available` (bool) - 1 pokud arch má PIO-Z80, 0 pro MZ-700
 *  - `im` (int) - aktuální IM mód (0/1/2)
 *  - `i` (int) - hodnota registru I
 *  - `vec` (int) - vector_byte z PIO-Z80 (0 pokud nepending)
 *  - `isr_addr` (int) - (I << 8) | vec, adresa v ISR table
 *  - `isr_target` (int) - MEM[isr_addr] little-endian (cíl skoku)
 *  - `pio_irq_pending` (bool) - PIO-Z80 IRQ pending?
 *  - `pio_source` (int) - 0=PIO-A, 1=PIO-B (relevant jen při pending).
 */
static en_MCP_DISPATCH_RESULT _handle_get_im2_vector(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_IM2_VECTOR out;
    memset(&out, 0, sizeof(out));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_IM2_VECTOR, NULL, &out)) {
        return _err_response(req_id, "GET_IM2_VECTOR failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "available",
                                    out.available ? TRUE : FALSE);
    json_object_set_int_member(data, "im",         (gint64)out.im);
    json_object_set_int_member(data, "i",          (gint64)out.i_register);
    json_object_set_int_member(data, "vec",        (gint64)out.vector_byte);
    json_object_set_int_member(data, "isr_addr",   (gint64)out.isr_table_addr);
    json_object_set_int_member(data, "isr_target", (gint64)out.isr_target_addr);
    json_object_set_boolean_member(data, "pio_irq_pending",
                                    out.pio_irq_pending ? TRUE : FALSE);
    json_object_set_int_member(data, "pio_source", (gint64)out.pio_source);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `get_raster_pos` handler - aktuální pozice GDG rasteru + cycle countery.
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_GET_RASTER_POS`. Response payload:
 *  - `frame_number` (int) - pořadové číslo aktuálního snímku
 *  - `scanline` (int) - aktuální raster row (0..VIDEO_SCREEN_HEIGHT-1)
 *  - `column_pixel` (int) - pixel sloupec v aktuálním scanline
 *  - `total_cycles` (int) - kumulativní T-stavy Z80 od resetu
 *  - `frame_cycles` (int) - T-stavy v rámci aktuálního snímku.
 */
static en_MCP_DISPATCH_RESULT _handle_get_raster_pos(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_RASTER_POS out;
    memset(&out, 0, sizeof(out));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_RASTER_POS, NULL, &out)) {
        return _err_response(req_id, "GET_RASTER_POS failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "frame_number", (gint64)out.frame_number);
    json_object_set_int_member(data, "scanline",     (gint64)out.scanline);
    json_object_set_int_member(data, "column_pixel", (gint64)out.column_pixel);
    json_object_set_int_member(data, "total_cycles", (gint64)out.total_cycles);
    json_object_set_int_member(data, "frame_cycles", (gint64)out.frame_cycles);
    return _ok_response(req_id, data, out_response);
}


/* ============================================================================
 *  V1.E.2 - CPU flags (Commit 2: get + set)
 *
 *  Sekce vystavuje DBGAPI_CMD_GET_CPU_FLAGS a DBGAPI_CMD_SET_CPU_FLAGS jako
 *  MCP Tools. GET vrací plný state (IFF, IM, HALT, INT/NMI pending, EI delay,
 *  Q reg, cycle countery, I/R reg). SET je selektivní zápis - klient předá
 *  jen fieldy které chce změnit (IFF1/IFF2/IM/I/R) a handler odvodí
 *  update_mask sám z přítomnosti JSON polí v requestu.
 * ============================================================================ */


/**
 * @brief `get_cpu_flags` handler - plný snapshot doplňkového CPU stavu.
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_GET_CPU_FLAGS`. Response payload
 * pokrývá všechny fieldy `st_DBGAPI_CPU_FLAGS`:
 *  - `iff1`, `iff2` (bool) - master + shadow interrupt enable
 *  - `im` (int) - aktuální Interrupt Mode (0/1/2)
 *  - `halted` (bool) - CPU v HALT instrukci
 *  - `int_pending`, `nmi_pending` (bool) - čekající INT/NMI
 *  - `ei_delay` (bool) - EI delay flag (1 instrukce po EI)
 *  - `q` (int) - interní Q registr (F z poslední ALU operace)
 *  - `total_cycles` (int) - kumulativní T-stavy od resetu
 *  - `frame_cycles` (int) - T-stavy v rámci aktuálního snímku
 *  - `op_tstate` (int) - T-stavy od začátku aktuální instrukce
 *  - `i`, `r` (int) - Interrupt Vector a Memory Refresh registry.
 */
static en_MCP_DISPATCH_RESULT _handle_get_cpu_flags(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_CPU_FLAGS out;
    memset(&out, 0, sizeof(out));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_CPU_FLAGS, NULL, &out)) {
        return _err_response(req_id, "GET_CPU_FLAGS failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "iff1",        out.iff1 ? TRUE : FALSE);
    json_object_set_boolean_member(data, "iff2",        out.iff2 ? TRUE : FALSE);
    json_object_set_int_member(data,    "im",           (gint64)out.im);
    json_object_set_boolean_member(data, "halted",      out.halted ? TRUE : FALSE);
    json_object_set_boolean_member(data, "int_pending", out.int_pending ? TRUE : FALSE);
    json_object_set_boolean_member(data, "nmi_pending", out.nmi_pending ? TRUE : FALSE);
    json_object_set_boolean_member(data, "ei_delay",    out.ei_delay ? TRUE : FALSE);
    json_object_set_int_member(data,    "q",            (gint64)out.q);
    json_object_set_int_member(data,    "total_cycles", (gint64)out.total_cycles);
    json_object_set_int_member(data,    "frame_cycles", (gint64)out.cycles);
    json_object_set_int_member(data,    "op_tstate",    (gint64)out.op_tstate);
    json_object_set_int_member(data,    "i",            (gint64)out.i_reg);
    json_object_set_int_member(data,    "r",            (gint64)out.r_reg);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `set_cpu_flags` handler - selektivní zápis IFF1/IFF2/IM/I/R.
 *
 * Parametry v `data` poli requestu (všechny volitelné, zapíše se jen
 * to, co klient explicit předá):
 *  - `iff1` (bool nebo int 0/1) - master interrupt enable
 *  - `iff2` (bool nebo int 0/1) - shadow interrupt enable
 *  - `im` (int, 0..2) - Interrupt Mode (jiné hodnoty rejected)
 *  - `i` (int, 0..0xFF) - Interrupt Vector register
 *  - `r` (int, 0..0xFF) - Memory Refresh register (bit 7 zachován)
 *
 * Update_mask se sestavuje automaticky podle přítomnosti polí v JSON.
 * Pokud žádné z polí není přítomno, handler vrací invalid params.
 *
 * Response payload:
 *  - `updated` (array of string) - jména polí, která byla zapsána
 *  - `values` (object) - mapping field -> finální hodnota (echo).
 *
 * Bezpečnost: zápis IFF/IM/I/R během running emulace je technically
 * race; backend běží v safepointu mezi instrukcemi, takže atomicita
 * zápisu je zajištěna implicitně. Klient je odpovědný za pause stav
 * pokud chce konzistentní pohled (= viz `emu_pause` před voláním).
 */
static en_MCP_DISPATCH_RESULT _handle_set_cpu_flags(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);

    st_DBGAPI_CPU_FLAGS param;
    memset(&param, 0, sizeof(param));
    param.update_mask = 0;

    /* Bool nebo int 0/1 - parsujeme přes _obj_int_or, akceptuje obojí
     * (json-glib se chová stejně pro G_TYPE_BOOLEAN i G_TYPE_INT64
     * pokud cast pres get_int_member). Pro robustnost čteme manuálně. */
    if (json_object_has_member(data_obj, "iff1")) {
        JsonNode *v = json_object_get_member(data_obj, "iff1");
        GType t = json_node_get_value_type(v);
        if (t == G_TYPE_BOOLEAN) {
            param.iff1 = json_node_get_boolean(v) ? 1 : 0;
        } else if (t == G_TYPE_INT64) {
            param.iff1 = (json_node_get_int(v) != 0) ? 1 : 0;
        } else {
            return _err_response(req_id, "Invalid parameters",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.update_mask |= DBGAPI_CPU_FLAGS_UM_IFF1;
    }
    if (json_object_has_member(data_obj, "iff2")) {
        JsonNode *v = json_object_get_member(data_obj, "iff2");
        GType t = json_node_get_value_type(v);
        if (t == G_TYPE_BOOLEAN) {
            param.iff2 = json_node_get_boolean(v) ? 1 : 0;
        } else if (t == G_TYPE_INT64) {
            param.iff2 = (json_node_get_int(v) != 0) ? 1 : 0;
        } else {
            return _err_response(req_id, "Invalid parameters",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.update_mask |= DBGAPI_CPU_FLAGS_UM_IFF2;
    }
    if (json_object_has_member(data_obj, "im")) {
        gint64 im = _obj_int_or(data_obj, "im", -1);
        if (im < 0 || im > 2) {
            return _err_response(req_id, "Invalid parameters",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.im = (uint8_t)im;
        param.update_mask |= DBGAPI_CPU_FLAGS_UM_IM;
    }
    if (json_object_has_member(data_obj, "i")) {
        gint64 i = _obj_int_or(data_obj, "i", -1);
        if (i < 0 || i > 0xFF) {
            return _err_response(req_id, "Invalid parameters",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.i_reg = (uint8_t)i;
        param.update_mask |= DBGAPI_CPU_FLAGS_UM_I;
    }
    if (json_object_has_member(data_obj, "r")) {
        gint64 r = _obj_int_or(data_obj, "r", -1);
        if (r < 0 || r > 0xFF) {
            return _err_response(req_id, "Invalid parameters",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.r_reg = (uint8_t)r;
        param.update_mask |= DBGAPI_CPU_FLAGS_UM_R;
    }

    if (param.update_mask == 0) {
        return _err_response(req_id,
                             "Invalid parameters - no field specified",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    if (!_submit_dbgapi(DBGAPI_CMD_SET_CPU_FLAGS, &param, NULL)) {
        return _err_response(req_id, "SET_CPU_FLAGS failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    /* Echo zapsaných polí. updated[] obsahuje string-jména, values{}
     * obsahuje echo hodnot, které byly skutečně commitnuty. */
    JsonObject *data = json_object_new();
    JsonArray  *updated = json_array_new();
    JsonObject *values  = json_object_new();
    if (param.update_mask & DBGAPI_CPU_FLAGS_UM_IFF1) {
        json_array_add_string_element(updated, "iff1");
        json_object_set_int_member(values, "iff1", (gint64)param.iff1);
    }
    if (param.update_mask & DBGAPI_CPU_FLAGS_UM_IFF2) {
        json_array_add_string_element(updated, "iff2");
        json_object_set_int_member(values, "iff2", (gint64)param.iff2);
    }
    if (param.update_mask & DBGAPI_CPU_FLAGS_UM_IM) {
        json_array_add_string_element(updated, "im");
        json_object_set_int_member(values, "im", (gint64)param.im);
    }
    if (param.update_mask & DBGAPI_CPU_FLAGS_UM_I) {
        json_array_add_string_element(updated, "i");
        json_object_set_int_member(values, "i", (gint64)param.i_reg);
    }
    if (param.update_mask & DBGAPI_CPU_FLAGS_UM_R) {
        json_array_add_string_element(updated, "r");
        json_object_set_int_member(values, "r", (gint64)param.r_reg);
    }
    json_object_set_array_member(data,  "updated", updated);
    json_object_set_object_member(data, "values",  values);
    return _ok_response(req_id, data, out_response);
}


/* ============================================================================
 *  V1.E.2 - last_instr + cpu_panel_batch (Commit 3: 2 Tools)
 *
 *  get_last_instr - poslední dokončená instrukce z debugger history ringu;
 *  klient dostane address + bytes_hex + length + mnemonic-jako-bytes
 *  (mnemonic řetězec sám rekonstruuje přes emu_dasm pokud potřebuje).
 *
 *  get_cpu_panel_batch - atomic round-trip pro celý CPU panel (regs + flags
 *  + raster + IM2 + last_instr). Caller předá which-mask (= které volitelné
 *  sekce chce naplnit), handler vrací odpovídající sub-objekty + per-section
 *  valid flagy.
 * ============================================================================ */


/**
 * @brief `get_last_instr` handler - poslední dokončená Z80 instrukce.
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_GET_LAST_INSTR`. Vrací nejnovější
 * záznam z `g_debugger_history` ringu (= position - 1 mod 32) včetně
 * dopočítané délky instrukce přes `z80_dasm`.
 *
 * Response payload:
 *  - `valid` (bool) - 1 pokud history má záznam, 0 pokud prázdná /
 *    neaktivní
 *  - `addr` (int) - adresa instrukce (PC v okamžiku M1 startu)
 *  - `bytes_hex` (string) - bajty instrukce jako hex (1-4 bajty,
 *    mezerou oddělené)
 *  - `length` (int) - délka instrukce v bajtech (1..4).
 *
 * Mnemonic není v payloadu (= shoduje se s `history_get` minimalismem).
 * Klient si umí přes `emu_dasm(addr, 1)` mnemonic rekonstruovat.
 */
static en_MCP_DISPATCH_RESULT _handle_get_last_instr(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_LAST_INSTR out;
    memset(&out, 0, sizeof(out));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_LAST_INSTR, NULL, &out)) {
        return _err_response(req_id, "GET_LAST_INSTR failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "valid",  out.valid ? TRUE : FALSE);
    json_object_set_int_member(data,    "addr",   (gint64)out.addr);
    json_object_set_int_member(data,    "length", (gint64)out.length);

    /* Bajty jako mezerami oddělený hex (1..4 bajtů, podle length pokud
     * valid; pokud invalid, vracíme prázdný řetězec). */
    char hex_buf[16] = {0};
    if (out.valid && out.length > 0) {
        int n = out.length;
        if (n > 4) n = 4;
        int hex_pos = 0;
        for (int b = 0; b < n; b++) {
            hex_pos += g_snprintf(hex_buf + hex_pos,
                                  sizeof(hex_buf) - hex_pos,
                                  (b == 0) ? "%02X" : " %02X",
                                  out.bytes[b]);
        }
    }
    json_object_set_string_member(data, "bytes_hex", hex_buf);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `get_cpu_panel_batch` handler - atomic snapshot CPU panelu.
 *
 * Parametry v `data` poli requestu (volitelné):
 *  - `want_im2` (bool, default false) - naplnit IM2 sub-objekt
 *  - `want_raster` (bool, default false) - naplnit raster sub-objekt
 *  - `want_last_instr` (bool, default false) - naplnit last_instr
 *
 * Volitelné sekce sledují `DBGAPI_CPU_PANEL_WANT_*` bity. Core sekce
 * (regs + flags + frame_number + user_cycle_origin) se naplňují vždy.
 * Pokud platforma má PIO-Z80 (MZ-800/MZ-1500), `has_pioz80=true` a
 * fieldy `veca/vecb/isra/isrb` jsou vždy přítomné.
 *
 * Response payload:
 *  - `regs` (object) - 14 Z80 registrů (AF..IR) jako int hodnoty
 *  - `flags` (object) - shodné schéma s `get_cpu_flags`
 *  - `frame_number` (int) - aktuální číslo snímku
 *  - `user_cycle_origin` (int) - snapshot origin pro User cyc display
 *  - `has_pioz80` (bool) - 1 pro MZ-800/MZ-1500, 0 pro MZ-700
 *  - `veca`, `vecb`, `isra`, `isrb` (int) - PIO-Z80 IM2 vector chain
 *  - `pio_int_vec_a`, `pio_int_vec_b` (int) - raw interrupt_vector
 *  - `im2`, `raster`, `last_instr` (object | null) - volitelné sub-objekty,
 *    naplněné pokud `*_valid` ze struktury indikuje úspěch.
 *
 * Side-effect free, výpočet vždy v safepointu emu vlákna.
 */
static en_MCP_DISPATCH_RESULT _handle_get_cpu_panel_batch(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);

    /* Sestavíme which-mask z volitelných JSON polí. */
    uint32_t which = 0;
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        if (json_object_has_member(data_obj, "want_im2")) {
            JsonNode *v = json_object_get_member(data_obj, "want_im2");
            GType t = json_node_get_value_type(v);
            bool b = (t == G_TYPE_BOOLEAN) ? json_node_get_boolean(v)
                   : (t == G_TYPE_INT64)   ? (json_node_get_int(v) != 0)
                   : false;
            if (b) which |= DBGAPI_CPU_PANEL_WANT_IM2;
        }
        if (json_object_has_member(data_obj, "want_raster")) {
            JsonNode *v = json_object_get_member(data_obj, "want_raster");
            GType t = json_node_get_value_type(v);
            bool b = (t == G_TYPE_BOOLEAN) ? json_node_get_boolean(v)
                   : (t == G_TYPE_INT64)   ? (json_node_get_int(v) != 0)
                   : false;
            if (b) which |= DBGAPI_CPU_PANEL_WANT_RASTER;
        }
        if (json_object_has_member(data_obj, "want_last_instr")) {
            JsonNode *v = json_object_get_member(data_obj, "want_last_instr");
            GType t = json_node_get_value_type(v);
            bool b = (t == G_TYPE_BOOLEAN) ? json_node_get_boolean(v)
                   : (t == G_TYPE_INT64)   ? (json_node_get_int(v) != 0)
                   : false;
            if (b) which |= DBGAPI_CPU_PANEL_WANT_LAST_INSTR;
        }
    }

    st_DBGAPI_CPU_PANEL_BATCH out;
    memset(&out, 0, sizeof(out));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_CPU_PANEL_BATCH, &which, &out)) {
        return _err_response(req_id, "GET_CPU_PANEL_BATCH failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    JsonObject *data = json_object_new();

    /* regs (vždy naplněné) - shoduje se s pojmenováním z get_registers. */
    JsonObject *regs = json_object_new();
    static const char *const reg_names[14] = {
        "AF","BC","DE","HL","AF_","BC_","DE_","HL_",
        "IX","IY","SP","PC","WZ","IR"
    };
    for (int i = 0; i < 14; i++) {
        json_object_set_int_member(regs, reg_names[i], (gint64)out.regs[i]);
    }
    json_object_set_object_member(data, "regs", regs);

    /* flags (vždy naplněné) - shoduje se s get_cpu_flags response. */
    JsonObject *flags = json_object_new();
    json_object_set_boolean_member(flags, "iff1",        out.flags.iff1 ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "iff2",        out.flags.iff2 ? TRUE : FALSE);
    json_object_set_int_member(flags,    "im",           (gint64)out.flags.im);
    json_object_set_boolean_member(flags, "halted",      out.flags.halted ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "int_pending", out.flags.int_pending ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "nmi_pending", out.flags.nmi_pending ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "ei_delay",    out.flags.ei_delay ? TRUE : FALSE);
    json_object_set_int_member(flags,    "q",            (gint64)out.flags.q);
    json_object_set_int_member(flags,    "total_cycles", (gint64)out.flags.total_cycles);
    json_object_set_int_member(flags,    "frame_cycles", (gint64)out.flags.cycles);
    json_object_set_int_member(flags,    "op_tstate",    (gint64)out.flags.op_tstate);
    json_object_set_int_member(flags,    "i",            (gint64)out.flags.i_reg);
    json_object_set_int_member(flags,    "r",            (gint64)out.flags.r_reg);
    json_object_set_object_member(data, "flags", flags);

    /* Core fieldy vždy. */
    json_object_set_int_member(data, "frame_number",      (gint64)out.frame_number);
    json_object_set_int_member(data, "user_cycle_origin", (gint64)out.user_cycle_origin);

    /* PIO-Z80 sub-fieldy (vždy přítomné jako schéma). */
    json_object_set_boolean_member(data, "has_pioz80",    out.has_pioz80 ? TRUE : FALSE);
    json_object_set_int_member(data, "pio_int_vec_a",     (gint64)out.pio_int_vec_a);
    json_object_set_int_member(data, "pio_int_vec_b",     (gint64)out.pio_int_vec_b);
    json_object_set_int_member(data, "veca",              (gint64)out.veca);
    json_object_set_int_member(data, "vecb",              (gint64)out.vecb);
    json_object_set_int_member(data, "isra",              (gint64)out.isra);
    json_object_set_int_member(data, "isrb",              (gint64)out.isrb);

    /* Volitelné sub-objekty - přítomné jen pokud valid. */
    if (out.im2_valid) {
        JsonObject *im2 = json_object_new();
        json_object_set_boolean_member(im2, "available",
                                        out.im2.available ? TRUE : FALSE);
        json_object_set_int_member(im2, "im",         (gint64)out.im2.im);
        json_object_set_int_member(im2, "i",          (gint64)out.im2.i_register);
        json_object_set_int_member(im2, "vec",        (gint64)out.im2.vector_byte);
        json_object_set_int_member(im2, "isr_addr",   (gint64)out.im2.isr_table_addr);
        json_object_set_int_member(im2, "isr_target", (gint64)out.im2.isr_target_addr);
        json_object_set_boolean_member(im2, "pio_irq_pending",
                                        out.im2.pio_irq_pending ? TRUE : FALSE);
        json_object_set_int_member(im2, "pio_source", (gint64)out.im2.pio_source);
        json_object_set_object_member(data, "im2", im2);
    }
    if (out.raster_valid) {
        JsonObject *raster = json_object_new();
        json_object_set_int_member(raster, "frame_number", (gint64)out.raster.frame_number);
        json_object_set_int_member(raster, "scanline",     (gint64)out.raster.scanline);
        json_object_set_int_member(raster, "column_pixel", (gint64)out.raster.column_pixel);
        json_object_set_int_member(raster, "total_cycles", (gint64)out.raster.total_cycles);
        json_object_set_int_member(raster, "frame_cycles", (gint64)out.raster.frame_cycles);
        json_object_set_object_member(data, "raster", raster);
    }
    if (out.last_instr_valid) {
        JsonObject *li = json_object_new();
        json_object_set_boolean_member(li, "valid",
                                        out.last_instr.valid ? TRUE : FALSE);
        json_object_set_int_member(li, "addr",   (gint64)out.last_instr.addr);
        json_object_set_int_member(li, "length", (gint64)out.last_instr.length);
        char hex_buf[16] = {0};
        if (out.last_instr.valid && out.last_instr.length > 0) {
            int n = out.last_instr.length;
            if (n > 4) n = 4;
            int hex_pos = 0;
            for (int b = 0; b < n; b++) {
                hex_pos += g_snprintf(hex_buf + hex_pos,
                                      sizeof(hex_buf) - hex_pos,
                                      (b == 0) ? "%02X" : " %02X",
                                      out.last_instr.bytes[b]);
            }
        }
        json_object_set_string_member(li, "bytes_hex", hex_buf);
        json_object_set_object_member(data, "last_instr", li);
    }

    return _ok_response(req_id, data, out_response);
}


/* ============================================================================
 *  V1.E.3 - debugger state Tools (Commit 1: 3 Tools)
 *
 *  Vystavuje DBGAPI_CMD_DEBUGGER_ACTIVATE / _DEACTIVATE / IS_DEBUGGER_ACTIVE
 *  jako MCP Tools. Dosud se debugger state zapínal jen z UI (= otevření
 *  okna), tj. headless / MCP klient neměl jak debug funkce (history ring,
 *  memory heatmap) zapnout programaticky. Tato 3 Tools tu cestu otevírají.
 *
 *  Aktivace má vedlejší efekt: v default WITH_WINDOW režimu se zapne
 *  cpuhist + mhmap recording, takže emu_get_last_instr / history_get
 *  začnou vracet smysluplná data. Deaktivace recording opět vypne.
 * ============================================================================ */


/**
 * @brief `debugger_activate` handler - programatické zapnutí debuggeru.
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_DEBUGGER_ACTIVATE`. Backend
 * nastaví `g_debugger.active = 1` a v default WITH_WINDOW režimu
 * implicitně zapne cpuhist + mhmap recording.
 *
 * Response payload:
 *  - `active` (bool) - vždy true při úspěchu (= debugger je nyní aktivní).
 *
 * Idempotentní: opakované volání nemá další efekt nad rámec první
 * aktivace. Klient může před voláním ověřit stav přes
 * `is_debugger_active`, ale není to nutné.
 */
static en_MCP_DISPATCH_RESULT _handle_debugger_activate(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_DEBUGGER_ACTIVATE, NULL, NULL)) {
        return _err_response(req_id, "DEBUGGER_ACTIVATE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "active", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `debugger_deactivate` handler - programatické vypnutí debuggeru.
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_DEBUGGER_DEACTIVATE`. Backend
 * nastaví `g_debugger.active = 0`. Side effect: cpuhist + mhmap
 * recording v WITH_WINDOW režimu se vypne.
 *
 * Response payload:
 *  - `active` (bool) - vždy false při úspěchu.
 *
 * Idempotentní: opakované volání nemá další efekt.
 */
static en_MCP_DISPATCH_RESULT _handle_debugger_deactivate(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_DEBUGGER_DEACTIVATE, NULL, NULL)) {
        return _err_response(req_id, "DEBUGGER_DEACTIVATE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "active", FALSE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `is_debugger_active` handler - dotaz na stav debuggeru.
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_IS_DEBUGGER_ACTIVE`. Backend
 * naplní bool přes result_ptr (= `TEST_DEBUGGER_ACTIVE`).
 *
 * Response payload:
 *  - `active` (bool) - 1 pokud `g_debugger.active != 0`, jinak 0.
 *
 * Read-only, idempotentní. Lze volat libovolně často - žádný vliv
 * na běh emulátoru.
 */
static en_MCP_DISPATCH_RESULT _handle_is_debugger_active(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    bool active = false;
    if (!_submit_dbgapi(DBGAPI_CMD_IS_DEBUGGER_ACTIVE, NULL, &active)) {
        return _err_response(req_id, "IS_DEBUGGER_ACTIVE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "active", active ? TRUE : FALSE);
    return _ok_response(req_id, data, out_response);
}


/* ============================================================================
 *  V1.E.3 - PIO-Z80 IM2 interrupt vector override (Commit 2: 1 Tool)
 *
 *  Vystavuje DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR jako MCP Tool.
 *  Backend zapisuje `g_pioz80.port[port_id].interrupt_vector` a maskuje
 *  bit 0 (= IVW spec - vždy 0 v Z80 PIO daisy chain).
 *
 *  Platform: pouze MZ-800 / MZ-1500 (HAVE_PIOZ80 = 1). Na MZ-700 vrací
 *  Tool {"available": false, "reason": "platform has no Z80 PIO"}
 *  místo chyby - klient tak může nezávisle na platformě bezpečně volat.
 * ============================================================================ */


/**
 * @brief `set_pioz80_interrupt_vector` handler - override IM2 vektoru
 *        pro PIO-Z80 port A nebo B.
 *
 * Parametry v `data` poli requestu (povinné):
 *  - `port` (int, 0 nebo 1) - 0 = PIOZ80_PORT_A, 1 = PIOZ80_PORT_B
 *  - `vector` (int, 0..255) - nová hodnota interrupt_vector registru;
 *    backend automaticky vymaskuje bit 0 (= Z80 PIO IVW spec)
 *
 * Response payload:
 *  - `available` (bool) - false pro MZ-700 (= žádný PIO-Z80), jinak true
 *  - `port` (int) - echo (jen při available=true)
 *  - `vector` (int) - echo finální hodnoty po masce bitu 0 (jen při available=true)
 *  - `applied` (bool) - true pokud zápis proběhl (jen při available=true)
 *  - `reason` (string) - vysvětlení (jen při available=false)
 *
 * Side effects: mění `g_pioz80.port[port].interrupt_vector`. Backend
 * běží v safepointu mezi instrukcemi, atomicita zápisu uint8_t je
 * triviální. Pause emulace není povinná, ale klient typicky volá
 * `emu_pause` před tímto Toolem pro deterministický pohled.
 */
static en_MCP_DISPATCH_RESULT _handle_set_pioz80_interrupt_vector(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);

    /* Platformy bez PIO-Z80 (MZ-700): available=false misto regulerni
     * chyby, aby klient mohl Tool volat platform-agnosticky (mzhal
     * krok 8; drive compile-time #if HAVE_PIOZ80). */
    if (!g_mzhal.have_pioz80) {
        JsonObject *na = json_object_new();
        json_object_set_boolean_member(na, "available", FALSE);
        json_object_set_string_member(na, "reason",
                                       "platform has no Z80 PIO");
        return _ok_response(req_id, na, out_response);
    }

    /* MZ-800 / MZ-1500 path - parse params + submit. */
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 port   = _obj_int_or(data_obj, "port",   -1);
    gint64 vector = _obj_int_or(data_obj, "vector", -1);
    if (port < 0 || port > 1 || vector < 0 || vector > 0xFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_PIOZ80_VEC_PARAM param;
    memset(&param, 0, sizeof(param));
    param.port_id     = (uint8_t)port;
    param.vector_byte = (uint8_t)vector;
    if (!_submit_dbgapi(DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR,
                        &param, NULL)) {
        return _err_response(req_id, "SET_PIOZ80_INTERRUPT_VECTOR failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "available", TRUE);
    json_object_set_int_member(data,    "port",      (gint64)param.port_id);
    /* Backend vymaskoval bit 0 - echo finální hodnoty. */
    json_object_set_int_member(data,    "vector",
                                (gint64)((uint8_t)(vector & 0xFE)));
    json_object_set_boolean_member(data, "applied",  TRUE);
    return _ok_response(req_id, data, out_response);
}


/* ============================================================================
 *  V1.E.4 - BP advanced Tools (Commit 1: 6 Tools)
 *
 *  Vystavuje 6 wrapperů nad existujícími DBGAPI breakpoint příkazy:
 *    - BP_CREATE_WITH_INIT - atomický add + init polí
 *    - BP_SET_PARENT       - quick reparent (drag-drop)
 *    - BP_UPDATE           - selektivní update polí existujícího BP
 *    - BPGRP_ADD           - přidání skupiny
 *    - BPGRP_REMOVE        - odebrání skupiny
 *    - BPGRP_UPDATE        - selektivní update polí skupiny
 *
 *  update_mask filozofie: caller pošle pole `fields` (= array stringů jmen
 *  polí). Handler mapuje názvy na DBGAPI_BP_UM_* / DBGAPI_BPGRP_UM_* bity
 *  a sestaví update_mask. Tato cesta je čitelnější než syrový 64-bit int
 *  v JSON. Neznámá jména `fields` → invalid params.
 * ============================================================================ */


/**
 * @brief Mapování BP field jména na DBGAPI_BP_UM_* bit.
 *
 * Používá se v _handle_bp_create_with_init + _handle_bp_update. Klient
 * pošle array stringů `fields`, handler iteruje a OR-uje bity. Tabulka
 * pokrývá všech 45 bitů z dbgapi_cmdrq.h (vč. 0019 v2 rate-limit override).
 */
typedef struct {
    const char *name;
    uint64_t    bit;
} st_BP_UM_FIELD_MAP;

static const st_BP_UM_FIELD_MAP _bp_um_field_map[] = {
    /* Identifikace */
    { "enabled",                DBGAPI_BP_UM_ENABLED },
    { "auto_name",              DBGAPI_BP_UM_AUTO_NAME },
    { "name",                   DBGAPI_BP_UM_NAME },
    { "colors",                 DBGAPI_BP_UM_COLORS },
    { "parent",                 DBGAPI_BP_UM_PARENT },
    /* Smart core */
    { "type",                   DBGAPI_BP_UM_TYPE },
    { "addr",                   DBGAPI_BP_UM_ADDR },
    { "addr_end",               DBGAPI_BP_UM_ADDR_END },
    { "zone",                   DBGAPI_BP_UM_ZONE },
    { "bank_id",                DBGAPI_BP_UM_BANK_ID },
    { "port",                   DBGAPI_BP_UM_PORT },
    { "event_name",             DBGAPI_BP_UM_EVENT_NAME },
    { "event_trigger",          DBGAPI_BP_UM_EVENT_TRIGGER },
    { "sp_threshold",           DBGAPI_BP_UM_SP_THRESHOLD },
    { "expr",                   DBGAPI_BP_UM_EXPR },
    { "action",                 DBGAPI_BP_UM_ACTION },
    { "hit_count",              DBGAPI_BP_UM_HIT_COUNT },
    { "skip_count",             DBGAPI_BP_UM_SKIP_COUNT },
    { "edge_triggered",         DBGAPI_BP_UM_EDGE_TRIGGERED },
    /* Match modes */
    { "addr_match_mode",        DBGAPI_BP_UM_ADDR_MATCH_MODE },
    { "addr_mask",              DBGAPI_BP_UM_ADDR_MASK },
    { "port_match_mode",        DBGAPI_BP_UM_PORT_MATCH_MODE },
    { "port_end",               DBGAPI_BP_UM_PORT_END },
    { "port_mask",              DBGAPI_BP_UM_PORT_MASK },
    { "port_mode",              DBGAPI_BP_UM_PORT_MODE },
    { "bank_match_mode",        DBGAPI_BP_UM_BANK_MATCH_MODE },
    { "bank_id_end",            DBGAPI_BP_UM_BANK_ID_END },
    { "bank_id_mask",           DBGAPI_BP_UM_BANK_ID_MASK },
    { "bp_addr_space",          DBGAPI_BP_UM_ADDR_SPACE },
    { "sp_mode",                DBGAPI_BP_UM_SP_MODE },
    { "sp_upper",               DBGAPI_BP_UM_SP_UPPER },
    /* IRQ A8 */
    { "im2_vector_filter",      DBGAPI_BP_UM_IM2_VECTOR_FILTER },
    { "im2_vector_match_mode",  DBGAPI_BP_UM_IM2_VECTOR_MATCH_MODE },
    { "im2_vector_addr_end",    DBGAPI_BP_UM_IM2_VECTOR_ADDR_END },
    { "im2_vector_mask",        DBGAPI_BP_UM_IM2_VECTOR_MASK },
    { "im2_isr_filter",         DBGAPI_BP_UM_IM2_ISR_FILTER },
    { "im2_isr_match_mode",     DBGAPI_BP_UM_IM2_ISR_MATCH_MODE },
    { "im2_isr_addr_end",       DBGAPI_BP_UM_IM2_ISR_ADDR_END },
    { "im2_isr_mask",           DBGAPI_BP_UM_IM2_ISR_MASK },
    /* IRQ A8.5 */
    { "im0_enabled",            DBGAPI_BP_UM_IM0_ENABLED },
    { "im1_enabled",            DBGAPI_BP_UM_IM1_ENABLED },
    { "im2_enabled",            DBGAPI_BP_UM_IM2_ENABLED },
    { "im0_rst_mask",           DBGAPI_BP_UM_IM0_RST_MASK },
    /* IRQ_SIG */
    { "irq_sig_source_mask",    DBGAPI_BP_UM_IRQ_SIG_SOURCE_MASK },
    /* 0019 vrstva 2 - per-BP rate-limit override těžkých FWD akcí */
    { "fwd_min_interval_ms",    DBGAPI_BP_UM_FWD_MIN_INTERVAL_MS },
    { "fwd_max_fires",          DBGAPI_BP_UM_FWD_MAX_FIRES },
    { NULL, 0 },
};


/**
 * @brief Mapování BPGRP field jména na DBGAPI_BPGRP_UM_* bit.
 *
 * Používá se v _handle_bpgrp_update. Stejná logika jako _bp_um_field_map,
 * jen pro skupiny - 4 bity v ABI.
 */
static const st_BP_UM_FIELD_MAP _bpgrp_um_field_map[] = {
    { "enabled",  DBGAPI_BPGRP_UM_ENABLED },
    { "name",     DBGAPI_BPGRP_UM_NAME },
    { "colors",   DBGAPI_BPGRP_UM_COLORS },
    { "parent",   DBGAPI_BPGRP_UM_PARENT },
    { NULL, 0 },
};


/**
 * @brief Vyhledá update_mask bit pro dané field jméno.
 *
 * @param map  Mapovací tabulka ukončená záznamem `{NULL, 0}`.
 * @param name Hledané jméno (case-sensitive).
 * @return Bitová maska (jeden bit) nebo 0 pokud jméno nenalezeno.
 */
static uint64_t _um_lookup(const st_BP_UM_FIELD_MAP *map, const char *name) {
    if (!name) return 0;
    for (size_t i = 0; map[i].name != NULL; i++) {
        if (strcmp(map[i].name, name) == 0) {
            return map[i].bit;
        }
    }
    return 0;
}


/**
 * @brief Postaví update_mask z JSON array stringů.
 *
 * @param fields_node  JsonNode obsahující array stringů; může být NULL.
 * @param map          Mapovací tabulka jmen na bity.
 * @param out_mask     Výstupní bitová maska (akumulovaná OR-em).
 * @return TRUE pokud všechna jména byla rozpoznána, FALSE pokud něco
 *         neznámého nebo array obsahuje non-string element.
 */
static gboolean _um_build_from_fields(JsonNode *fields_node,
                                      const st_BP_UM_FIELD_MAP *map,
                                      uint64_t *out_mask) {
    *out_mask = 0;
    if (!fields_node || json_node_get_node_type(fields_node) != JSON_NODE_ARRAY) {
        /* Chybějící / není array - 0 mask je legitimní (= no-op). */
        return TRUE;
    }
    JsonArray *arr = json_node_get_array(fields_node);
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        JsonNode *el = json_array_get_element(arr, i);
        if (json_node_get_value_type(el) != G_TYPE_STRING) {
            return FALSE;
        }
        const char *fname = json_node_get_string(el);
        uint64_t bit = _um_lookup(map, fname);
        if (bit == 0) {
            return FALSE;
        }
        *out_mask |= bit;
    }
    return TRUE;
}


/**
 * @brief Zjistí, zda je hodnota pole v JSON objektu string node.
 *
 * Slouží k rozlišení, zda enum pole (type, zone, ...) přišlo jako
 * UPPER_SNAKE řetězec (= preferovaný kanonický tvar) nebo jako číselný
 * index (= zpětná kompatibilita s číselnými klienty).
 *
 * @param obj  JSON object s payload (nesmí být NULL).
 * @param key  Jméno pole.
 * @return TRUE pokud pole existuje, je JSON value a jeho value type je
 *         G_TYPE_STRING; jinak FALSE.
 */
static gboolean _obj_is_string(JsonObject *obj, const char *key) {
    if (!obj || !json_object_has_member(obj, key)) return FALSE;
    JsonNode *node = json_object_get_member(obj, key);
    if (!node || json_node_get_node_type(node) != JSON_NODE_VALUE) return FALSE;
    return json_node_get_value_type(node) == G_TYPE_STRING;
}


/**
 * @brief Naparsuje enum pole z JSON objektu (string přes konvertor, jinak int).
 *
 * Sjednocuje parsing celé BP enum rodiny (type, zone, event_trigger,
 * *_match_mode, port_mode, sp_mode). Pokud je hodnota pole JSON string,
 * převede ji zadaným `conv` konvertorem (= kanonický UPPER_SNAKE slovník
 * sdílený s `.bpt` persistencí); při neznámém řetězci selže tvrdou chybou
 * (NE tichý fallback na 0). Pokud je hodnota číslo, projde přes
 * `_obj_int_or` (zpětná kompat). Pokud pole v JSON není, ponechá `*out`
 * beze změny.
 *
 * Konvertor pracuje s konkrétním enum typem; volající ho předává přes
 * thunk vracející hodnotu jako `unsigned`, aby helper zůstal typově
 * generický nezávisle na cílovém enumu.
 *
 * @param obj   JSON object s payload (nesmí být NULL).
 * @param key   Jméno enum pole.
 * @param out   Výstup - 8-bit enum index. Měněn jen pokud pole existuje.
 * @param conv  Thunk: převede string na `unsigned` enum hodnotu, vrátí
 *              TRUE při úspěchu, FALSE při neznámém řetězci.
 * @return TRUE pokud pole chybí, je validní int, nebo je string úspěšně
 *         převeden; FALSE pokud je string a `conv` ho neumí přeložit.
 *
 * @note Out-of-range číselné hodnoty NEjsou tady odmítány - to řeší
 *       range-check v `breakpoints_set_*` na backendu. Tady jde jen o
 *       neznámé enum řetězce.
 */
static gboolean _bp_parse_enum8(JsonObject *obj, const char *key,
                                uint8_t *out,
                                gboolean (*conv)(const char *, unsigned *)) {
    if (!json_object_has_member(obj, key)) {
        return TRUE;
    }
    if (_obj_is_string(obj, key)) {
        const char *s = json_object_get_string_member(obj, key);
        unsigned v = 0;
        if (!conv(s, &v)) {
            return FALSE;
        }
        *out = (uint8_t)v;
        return TRUE;
    }
    *out = (uint8_t)_obj_int_or(obj, key, 0);
    return TRUE;
}


/* ---- Thunky enum konvertorů (string -> unsigned enum index) ----------
 *
 * Každý thunk obaluje kanonický *_from_string konvertor a vrací hodnotu
 * jako unsigned, aby šel předat genericky do _bp_parse_enum8. Slovník
 * (UPPER_SNAKE) je tak sdílen 1:1 s .bpt persistencí (breakpoints.c) -
 * žádné rozdvojení názvů. */

/** @brief Thunk pro `bpt_type_from_string` (en_BPT_TYPE). */
static gboolean _conv_bpt_type(const char *s, unsigned *out) {
    en_BPT_TYPE t;
    if (!bpt_type_from_string(s, &t)) return FALSE;
    *out = (unsigned)t;
    return TRUE;
}

/** @brief Thunk pro `bp_zone_from_string` (en_BP_ZONE, vč. legacy aliasů). */
static gboolean _conv_bp_zone(const char *s, unsigned *out) {
    en_BP_ZONE z;
    if (!bp_zone_from_string(s, &z)) return FALSE;
    *out = (unsigned)z;
    return TRUE;
}

/** @brief Thunk pro `bp_event_trigger_from_string` (en_BP_EVENT_TRIGGER). */
static gboolean _conv_bp_event_trigger(const char *s, unsigned *out) {
    en_BP_EVENT_TRIGGER t;
    if (!bp_event_trigger_from_string(s, &t)) return FALSE;
    *out = (unsigned)t;
    return TRUE;
}

/** @brief Thunk pro `bp_match_mode_from_string` (en_BP_MATCH_MODE). */
static gboolean _conv_bp_match_mode(const char *s, unsigned *out) {
    en_BP_MATCH_MODE m;
    if (!bp_match_mode_from_string(s, &m)) return FALSE;
    *out = (unsigned)m;
    return TRUE;
}

/** @brief Thunk pro `bp_port_mode_from_string` (en_BP_PORT_MODE). */
static gboolean _conv_bp_port_mode(const char *s, unsigned *out) {
    en_BP_PORT_MODE m;
    if (!bp_port_mode_from_string(s, &m)) return FALSE;
    *out = (unsigned)m;
    return TRUE;
}

/** @brief Thunk pro `bp_addr_space_from_string` (en_BP_ADDR_SPACE, feature D). */
static gboolean _conv_bp_addr_space(const char *s, unsigned *out) {
    en_BP_ADDR_SPACE a;
    if (!bp_addr_space_from_string(s, &a)) return FALSE;
    *out = (unsigned)a;
    return TRUE;
}

/** @brief Thunk pro `bp_sp_mode_from_string` (en_BP_SP_MODE). */
static gboolean _conv_bp_sp_mode(const char *s, unsigned *out) {
    en_BP_SP_MODE m;
    if (!bp_sp_mode_from_string(s, &m)) return FALSE;
    *out = (unsigned)m;
    return TRUE;
}


/**
 * @brief Naplní vybraná pole `st_DBGAPI_BP_UPDATE_PARAM` z JSON dat.
 *
 * Postupně se čte každý field který je v JSON přítomen. update_mask se
 * předává odděleně (= caller stanoví podle pole `fields[]`). Tato funkce
 * jen kopíruje hodnoty, nečte mask - bezpečné volat s prázdnou mask.
 *
 * Enum pole (type, zone, event_trigger, *_match_mode, port_mode, sp_mode)
 * přijímají PRIMÁRNĚ kanonický UPPER_SNAKE string (sdílený slovník s `.bpt`
 * persistencí) přes `_bp_parse_enum8`; číslo je akceptováno kvůli zpětné
 * kompatibilitě. Neznámý enum řetězec funkci přeruší (FALSE + `*err_field`)
 * - caller pak vrátí MCP invalid_params místo tichého fallbacku na 0.
 *
 * `irq_sig_source_mask` je bitová maska (ne prostý enum index), proto
 * zůstává int-only - string parse multi-source masky je netriviální a není
 * součástí tohoto sjednocení (viz D2 risk note).
 *
 * String pointers (`name`, `event_name`, `expr`, `action`) ukazují do
 * JSON parse stromu - lifetime trvá do `_send_request` návratu, což je
 * delší než `_submit_dbgapi` sync cmd. Pro NULL handler vyloží
 * "clear field" pokud je odpovídající bit v update_mask.
 *
 * @param obj       JSON object s payload.
 * @param p         Výstupní struktura. Musí být před voláním memset(0).
 * @param err_field Výstup - při návratu FALSE ukazatel na jméno pole s
 *                  neznámou enum hodnotou (statický string, neuvolňovat);
 *                  jinak nezměněn. Smí být NULL.
 * @return TRUE pokud byla všechna enum pole rozpoznána, FALSE při neznámém
 *         enum řetězci.
 */
static gboolean _bp_fill_param_from_json(JsonObject *obj,
                                         st_DBGAPI_BP_UPDATE_PARAM *p,
                                         const char **err_field) {
    if (json_object_has_member(obj, "enabled")) {
        p->enabled = json_object_get_boolean_member(obj, "enabled");
    }
    if (json_object_has_member(obj, "auto_name")) {
        p->auto_name = json_object_get_boolean_member(obj, "auto_name");
    }
    if (json_object_has_member(obj, "name")) {
        p->name = json_object_get_string_member(obj, "name");
    }
    if (json_object_has_member(obj, "bg_rgb")) {
        p->bg_rgb = (uint32_t)_obj_int_or(obj, "bg_rgb", 0);
    }
    if (json_object_has_member(obj, "fg_rgb")) {
        p->fg_rgb = (uint32_t)_obj_int_or(obj, "fg_rgb", 0);
    }
    if (json_object_has_member(obj, "parent")) {
        p->parent = (int)_obj_int_or(obj, "parent", -1);
    }
    if (!_bp_parse_enum8(obj, "type", &p->type, _conv_bpt_type)) {
        if (err_field) *err_field = "type";
        return FALSE;
    }
    if (json_object_has_member(obj, "addr")) {
        p->addr = (uint16_t)_obj_int_or(obj, "addr", 0);
    }
    if (json_object_has_member(obj, "addr_end")) {
        p->addr_end = (uint16_t)_obj_int_or(obj, "addr_end", 0);
    }
    if (!_bp_parse_enum8(obj, "zone", &p->zone, _conv_bp_zone)) {
        if (err_field) *err_field = "zone";
        return FALSE;
    }
    if (json_object_has_member(obj, "bank_id")) {
        p->bank_id = (uint8_t)_obj_int_or(obj, "bank_id", 0);
    }
    if (json_object_has_member(obj, "port")) {
        p->port = (uint16_t)_obj_int_or(obj, "port", 0);
    }
    if (json_object_has_member(obj, "event_name")) {
        p->event_name = json_object_get_string_member(obj, "event_name");
    }
    if (!_bp_parse_enum8(obj, "event_trigger", &p->event_trigger,
                         _conv_bp_event_trigger)) {
        if (err_field) *err_field = "event_trigger";
        return FALSE;
    }
    if (json_object_has_member(obj, "sp_threshold")) {
        p->sp_threshold = (uint16_t)_obj_int_or(obj, "sp_threshold", 0);
    }
    if (json_object_has_member(obj, "expr")) {
        p->expr = json_object_get_string_member(obj, "expr");
    }
    if (json_object_has_member(obj, "action")) {
        p->action = json_object_get_string_member(obj, "action");
    }
    if (json_object_has_member(obj, "hit_count")) {
        p->hit_count = (uint32_t)_obj_int_or(obj, "hit_count", 0);
    }
    if (json_object_has_member(obj, "skip_count")) {
        p->skip_count = (uint32_t)_obj_int_or(obj, "skip_count", 0);
    }
    if (json_object_has_member(obj, "edge_triggered")) {
        p->edge_triggered = json_object_get_boolean_member(obj, "edge_triggered");
    }
    if (!_bp_parse_enum8(obj, "addr_match_mode", &p->addr_match_mode,
                         _conv_bp_match_mode)) {
        if (err_field) *err_field = "addr_match_mode";
        return FALSE;
    }
    if (json_object_has_member(obj, "addr_mask")) {
        p->addr_mask = (uint16_t)_obj_int_or(obj, "addr_mask", 0);
    }
    if (!_bp_parse_enum8(obj, "port_match_mode", &p->port_match_mode,
                         _conv_bp_match_mode)) {
        if (err_field) *err_field = "port_match_mode";
        return FALSE;
    }
    if (json_object_has_member(obj, "port_end")) {
        p->port_end = (uint16_t)_obj_int_or(obj, "port_end", 0);
    }
    if (json_object_has_member(obj, "port_mask")) {
        p->port_mask = (uint16_t)_obj_int_or(obj, "port_mask", 0);
    }
    if (!_bp_parse_enum8(obj, "port_mode", &p->port_mode,
                         _conv_bp_port_mode)) {
        if (err_field) *err_field = "port_mode";
        return FALSE;
    }
    if (!_bp_parse_enum8(obj, "bank_match_mode", &p->bank_match_mode,
                         _conv_bp_match_mode)) {
        if (err_field) *err_field = "bank_match_mode";
        return FALSE;
    }
    if (json_object_has_member(obj, "bank_id_end")) {
        p->bank_id_end = (uint8_t)_obj_int_or(obj, "bank_id_end", 0);
    }
    if (json_object_has_member(obj, "bank_id_mask")) {
        p->bank_id_mask = (uint8_t)_obj_int_or(obj, "bank_id_mask", 0);
    }
    if (!_bp_parse_enum8(obj, "bp_addr_space", &p->bp_addr_space,
                         _conv_bp_addr_space)) {
        if (err_field) *err_field = "bp_addr_space";
        return FALSE;
    }
    if (!_bp_parse_enum8(obj, "sp_mode", &p->sp_mode, _conv_bp_sp_mode)) {
        if (err_field) *err_field = "sp_mode";
        return FALSE;
    }
    if (json_object_has_member(obj, "sp_upper")) {
        p->sp_upper = (uint16_t)_obj_int_or(obj, "sp_upper", 0);
    }
    if (json_object_has_member(obj, "im2_vector_enabled")) {
        p->im2_vector_enabled =
            json_object_get_boolean_member(obj, "im2_vector_enabled");
    }
    if (json_object_has_member(obj, "im2_vector_addr")) {
        p->im2_vector_addr = (uint16_t)_obj_int_or(obj, "im2_vector_addr", 0);
    }
    if (!_bp_parse_enum8(obj, "im2_vector_match_mode",
                         &p->im2_vector_match_mode, _conv_bp_match_mode)) {
        if (err_field) *err_field = "im2_vector_match_mode";
        return FALSE;
    }
    if (json_object_has_member(obj, "im2_vector_addr_end")) {
        p->im2_vector_addr_end =
            (uint16_t)_obj_int_or(obj, "im2_vector_addr_end", 0);
    }
    if (json_object_has_member(obj, "im2_vector_mask")) {
        p->im2_vector_mask = (uint16_t)_obj_int_or(obj, "im2_vector_mask", 0);
    }
    if (json_object_has_member(obj, "im2_isr_enabled")) {
        p->im2_isr_enabled =
            json_object_get_boolean_member(obj, "im2_isr_enabled");
    }
    if (json_object_has_member(obj, "im2_isr_addr")) {
        p->im2_isr_addr = (uint16_t)_obj_int_or(obj, "im2_isr_addr", 0);
    }
    if (!_bp_parse_enum8(obj, "im2_isr_match_mode",
                         &p->im2_isr_match_mode, _conv_bp_match_mode)) {
        if (err_field) *err_field = "im2_isr_match_mode";
        return FALSE;
    }
    if (json_object_has_member(obj, "im2_isr_addr_end")) {
        p->im2_isr_addr_end =
            (uint16_t)_obj_int_or(obj, "im2_isr_addr_end", 0);
    }
    if (json_object_has_member(obj, "im2_isr_mask")) {
        p->im2_isr_mask = (uint16_t)_obj_int_or(obj, "im2_isr_mask", 0);
    }
    if (json_object_has_member(obj, "im0_enabled")) {
        p->im0_enabled = json_object_get_boolean_member(obj, "im0_enabled");
    }
    if (json_object_has_member(obj, "im1_enabled")) {
        p->im1_enabled = json_object_get_boolean_member(obj, "im1_enabled");
    }
    if (json_object_has_member(obj, "im2_enabled")) {
        p->im2_enabled = json_object_get_boolean_member(obj, "im2_enabled");
    }
    if (json_object_has_member(obj, "im0_rst_mask")) {
        p->im0_rst_mask = (uint8_t)_obj_int_or(obj, "im0_rst_mask", 0);
    }
    if (json_object_has_member(obj, "irq_sig_source_mask")) {
        /* Bitová maska (ne prostý enum index) - string parse multi-source
         * masky je netriviální, proto int-only (viz D2 risk note). */
        p->irq_sig_source_mask =
            (uint8_t)_obj_int_or(obj, "irq_sig_source_mask", 0);
    }
    /* 0019 vrstva 2 - per-BP rate-limit override (těžké FWD akce).
     * Sémantika 0: min_interval_ms 0 = global/built-in default; max_fires
     * 0 = neomezeno. Hodnoty se klampují do rozumných mezí (sanity strop),
     * záporné JSON hodnoty -> 0 (= bezpečný default / vypnuto). */
    if (json_object_has_member(obj, "fwd_min_interval_ms")) {
        gint64 v = _obj_int_or(obj, "fwd_min_interval_ms", 0);
        if (v < 0) v = 0;
        if (v > MCP_DISPATCH_BP_FWD_MIN_INTERVAL_MS_MAX)
            v = MCP_DISPATCH_BP_FWD_MIN_INTERVAL_MS_MAX;
        p->fwd_min_interval_ms = (uint32_t)v;
    }
    if (json_object_has_member(obj, "fwd_max_fires")) {
        gint64 v = _obj_int_or(obj, "fwd_max_fires", 0);
        if (v < 0) v = 0;
        if (v > (gint64)UINT32_MAX) v = (gint64)UINT32_MAX;
        p->fwd_max_fires = (uint32_t)v;
    }
    return TRUE;
}


/**
 * @brief `bp_create_with_init` handler - atomický create + init BP.
 *
 * Parametry v `data` poli requestu:
 *  - `fields` (array of string, povinné) - jména polí k aplikaci, viz
 *    `_bp_um_field_map`. UM_ADDR / UM_PARENT / další pole musí caller
 *    explicitně uvést pokud chce hodnoty propsat.
 *  - `addr` (int, doporučené) - adresa nového BP, použito v
 *    breakpoints_add_auto + UM_ADDR bit.
 *  - Volitelně všechny ostatní fieldy z `st_DBGAPI_BP_UPDATE_PARAM`
 *    (= name, type, zone, expr, action, hit_count, ...).
 *
 * Response payload:
 *  - `id` (int) - ID nově vytvořeného BP nebo -1 při selhání.
 *  - `created` (bool) - true při úspěchu.
 *
 * Lifecycle: handler nastaví `p.id = -1`, zavolá CMD_BP_CREATE_WITH_INIT,
 * backend zavolá breakpoints_add_auto + aplikuje update_mask. Po návratu
 * je `p.id` ID nového BP (= dbgapi_emu_bp_apply_update naplní).
 */
static en_MCP_DISPATCH_RESULT _handle_bp_create_with_init(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);

    uint64_t mask = 0;
    JsonNode *fields_node = json_object_has_member(data_obj, "fields")
        ? json_object_get_member(data_obj, "fields") : NULL;
    if (!_um_build_from_fields(fields_node, _bp_um_field_map, &mask)) {
        return _err_response(req_id, "Invalid 'fields' entry",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_BP_UPDATE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.id = -1;
    p.parent = -1;
    p.update_mask = mask;
    const char *bad_field = NULL;
    if (!_bp_fill_param_from_json(data_obj, &p, &bad_field)) {
        char msg[96];
        g_snprintf(msg, sizeof(msg),
                   "Unknown enum value for field '%s'",
                   bad_field ? bad_field : "?");
        return _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS,
                             out_response);
    }

    bool ok = _submit_dbgapi(DBGAPI_CMD_BP_CREATE_WITH_INIT, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp,     "id",      (gint64)p.id);
    json_object_set_boolean_member(resp, "created", ok ? TRUE : FALSE);
    if (!ok) {
        json_object_unref(resp);
        return _err_response(req_id, "BP_CREATE_WITH_INIT failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bp_set_parent` handler - quick reparent BP do skupiny.
 *
 * Parametry v `data` poli requestu:
 *  - `id` (int, povinné) - ID existujícího BP.
 *  - `parent_id` (int, povinné) - -1 = root, jinak ID existující skupiny.
 *
 * Response payload:
 *  - `updated` (bool) - true při úspěchu, false pokud BP neexistuje.
 */
static en_MCP_DISPATCH_RESULT _handle_bp_set_parent(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "id") ||
        !json_object_has_member(data_obj, "parent_id")) {
        return _err_response(req_id, "Missing 'id' or 'parent_id'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_BP_SET_PARENT_PARAM p;
    memset(&p, 0, sizeof(p));
    p.id        = (int)_obj_int_or(data_obj, "id", -1);
    p.parent_id = (int)_obj_int_or(data_obj, "parent_id", -1);

    bool ok = _submit_dbgapi(DBGAPI_CMD_BP_SET_PARENT, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "updated", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bp_update` handler - selektivní update polí existujícího BP.
 *
 * Parametry v `data` poli requestu:
 *  - `id` (int, povinné) - ID existujícího BP.
 *  - `fields` (array of string, povinné) - jména polí k aplikaci, viz
 *    `_bp_um_field_map`. Prázdné pole = no-op success.
 *  - Volitelně pole odpovídající jménům ve `fields` (= jejich hodnoty).
 *
 * Response payload:
 *  - `updated` (bool) - true pokud změna proběhla nebo update_mask byl 0.
 */
static en_MCP_DISPATCH_RESULT _handle_bp_update(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "id")) {
        return _err_response(req_id, "Missing 'id'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    uint64_t mask = 0;
    JsonNode *fields_node = json_object_has_member(data_obj, "fields")
        ? json_object_get_member(data_obj, "fields") : NULL;
    if (!_um_build_from_fields(fields_node, _bp_um_field_map, &mask)) {
        return _err_response(req_id, "Invalid 'fields' entry",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_BP_UPDATE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.id = (int)_obj_int_or(data_obj, "id", -1);
    p.parent = -1;
    p.update_mask = mask;
    const char *bad_field = NULL;
    if (!_bp_fill_param_from_json(data_obj, &p, &bad_field)) {
        char msg[96];
        g_snprintf(msg, sizeof(msg),
                   "Unknown enum value for field '%s'",
                   bad_field ? bad_field : "?");
        return _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS,
                             out_response);
    }

    bool ok = _submit_dbgapi(DBGAPI_CMD_BP_UPDATE, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "updated", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bpgrp_add` handler - přidá novou BP skupinu.
 *
 * Parametry v `data` poli requestu:
 *  - `name` (string, povinné) - jméno nové skupiny.
 *  - `parent` (int, volitelné, default -1) - -1 = root, jinak ID
 *    existující rodičovské skupiny.
 *
 * Response payload:
 *  - `id` (int) - přidělené ID nebo -1 při selhání.
 */
static en_MCP_DISPATCH_RESULT _handle_bpgrp_add(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    const char *name = json_object_has_member(data_obj, "name")
        ? json_object_get_string_member(data_obj, "name") : NULL;
    if (!name) {
        return _err_response(req_id, "Missing 'name'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_BPGRP_ADD_PARAM p;
    memset(&p, 0, sizeof(p));
    p.name   = name;
    p.parent = (int)_obj_int_or(data_obj, "parent", -1);
    p.id     = -1;

    bool ok = _submit_dbgapi(DBGAPI_CMD_BPGRP_ADD, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "id", (gint64)p.id);
    if (!ok) {
        json_object_unref(resp);
        return _err_response(req_id, "BPGRP_ADD failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bpgrp_remove` handler - odebere BP skupinu podle ID.
 *
 * Parametry v `data` poli requestu:
 *  - `id` (int, povinné) - ID existující skupiny.
 *
 * Response payload:
 *  - `removed` (bool) - true při úspěchu, false pokud skupina neexistuje.
 *
 * Cascading delete / reparent dětí (BPs + sub-skupin) je v gesci
 * backend logiky (breakpoints.c).
 */
static en_MCP_DISPATCH_RESULT _handle_bpgrp_remove(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "id")) {
        return _err_response(req_id, "Missing 'id'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_BPGRP_REMOVE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.id = (int)_obj_int_or(data_obj, "id", -1);

    bool ok = _submit_dbgapi(DBGAPI_CMD_BPGRP_REMOVE, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "removed", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bpgrp_update` handler - selektivní update polí BP skupiny.
 *
 * Parametry v `data` poli requestu:
 *  - `id` (int, povinné) - ID existující skupiny.
 *  - `fields` (array of string, povinné) - jména polí k aplikaci:
 *    "enabled", "name", "colors", "parent".
 *  - `enabled` (bool, volitelné).
 *  - `name` (string, volitelné, NULL = clear).
 *  - `bg_rgb`, `fg_rgb` (int, volitelné, aplikují se společně přes
 *    UM_COLORS).
 *  - `parent` (int, volitelné, -1 = root).
 *
 * Response payload:
 *  - `updated` (bool) - true pokud změna proběhla nebo update_mask=0.
 */
static en_MCP_DISPATCH_RESULT _handle_bpgrp_update(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "id")) {
        return _err_response(req_id, "Missing 'id'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint64_t mask = 0;
    JsonNode *fields_node = json_object_has_member(data_obj, "fields")
        ? json_object_get_member(data_obj, "fields") : NULL;
    if (!_um_build_from_fields(fields_node, _bpgrp_um_field_map, &mask)) {
        return _err_response(req_id, "Invalid 'fields' entry",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_BPGRP_UPDATE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.id          = (int)_obj_int_or(data_obj, "id", -1);
    p.update_mask = mask;
    p.parent      = (int)_obj_int_or(data_obj, "parent", -1);
    if (json_object_has_member(data_obj, "enabled")) {
        p.enabled = json_object_get_boolean_member(data_obj, "enabled");
    }
    if (json_object_has_member(data_obj, "name")) {
        p.name = json_object_get_string_member(data_obj, "name");
    }
    if (json_object_has_member(data_obj, "bg_rgb")) {
        p.bg_rgb = (uint32_t)_obj_int_or(data_obj, "bg_rgb", 0);
    }
    if (json_object_has_member(data_obj, "fg_rgb")) {
        p.fg_rgb = (uint32_t)_obj_int_or(data_obj, "fg_rgb", 0);
    }

    bool ok = _submit_dbgapi(DBGAPI_CMD_BPGRP_UPDATE, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "updated", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/* ============================================================================
 *  V1.E.4 - Stack analytics Tools (Commit 2: 6 Tools)
 *
 *  Vystavuje 6 wrapperů nad stack monitoring příkazy:
 *    - STACK_HISTORY_ENABLE          - zapnout/vypnout SP history recording
 *    - STACK_HISTORY_RESET           - vyprázdnit ring buffer
 *    - STACK_REGIONS_ADD             - přidat region
 *    - STACK_REGIONS_EDIT            - edit existujícího regionu
 *    - STACK_REGIONS_REMOVE          - odebrat region
 *    - STACK_REGIONS_RESET_WATERMARK - reset watermark + counters jednoho
 *                                      regionu
 * ============================================================================ */


/**
 * @brief `stack_history_enable` handler - zapne/vypne SP history recording.
 *
 * Parametry v `data` poli requestu:
 *  - `enabled` (bool, povinné) - true = zapnout, false = vypnout + flush.
 *
 * Response payload:
 *  - `enabled` (bool) - echo finální hodnoty.
 *
 * Side effects: vypnutí navíc vyprázdní ring buffer (= další zapnutí
 * začne s čistým stavem). Aktivační flag se promítne do hot-path call
 * site v mzarch.c (= zero overhead když default OFF).
 */
static en_MCP_DISPATCH_RESULT _handle_stack_history_enable(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "enabled")) {
        return _err_response(req_id, "Missing 'enabled'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gboolean enabled = json_object_get_boolean_member(data_obj, "enabled");

    st_DBGAPI_STACK_HISTORY_ENABLE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.enable = enabled ? 1 : 0;

    if (!_submit_dbgapi(DBGAPI_CMD_STACK_HISTORY_ENABLE, &p, NULL)) {
        return _err_response(req_id, "STACK_HISTORY_ENABLE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "enabled", enabled);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `stack_history_reset` handler - vyprázdní SP history ring buffer.
 *
 * Bez parametrů. Recording flag zůstává nedotčen (= jen se zahodí dosud
 * nasbíraná data). Vhodné pro UI "Reset history" tlačítko.
 *
 * Response payload:
 *  - `reset` (bool) - vždy true při úspěchu.
 */
static en_MCP_DISPATCH_RESULT _handle_stack_history_reset(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    (void)req;
    if (!_submit_dbgapi(DBGAPI_CMD_STACK_HISTORY_RESET, NULL, NULL)) {
        return _err_response(req_id, "STACK_HISTORY_RESET failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "reset", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `stack_regions_add` handler - přidá nový stack region.
 *
 * Parametry v `data` poli requestu:
 *  - `name` (string, povinné) - label regionu, max 31 znaků + '\0'.
 *  - `base` (int, povinné) - vrchol regionu (= nejvyšší adresa).
 *  - `limit` (int, povinné) - dno regionu (base > limit).
 *
 * Response payload:
 *  - `index` (int) - index 0..MAX-1 při úspěchu, -1 při chybě
 *    (overlap / duplicate / invalid).
 *  - `added` (bool) - true při úspěchu.
 */
static en_MCP_DISPATCH_RESULT _handle_stack_regions_add(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    const char *name = json_object_has_member(data_obj, "name")
        ? json_object_get_string_member(data_obj, "name") : NULL;
    if (!name || !json_object_has_member(data_obj, "base") ||
        !json_object_has_member(data_obj, "limit")) {
        return _err_response(req_id, "Missing 'name', 'base' or 'limit'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 base  = _obj_int_or(data_obj, "base",  -1);
    gint64 limit = _obj_int_or(data_obj, "limit", -1);
    if (base < 0 || base > 0xFFFF || limit < 0 || limit > 0xFFFF) {
        return _err_response(req_id, "Invalid 'base' or 'limit'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_STACK_REGIONS_ADD_PARAM p;
    memset(&p, 0, sizeof(p));
    g_strlcpy(p.name, name, sizeof(p.name));
    p.base  = (uint16_t)base;
    p.limit = (uint16_t)limit;
    p.result_index = -1;

    bool ok = _submit_dbgapi(DBGAPI_CMD_STACK_REGIONS_ADD, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp,     "index", (gint64)p.result_index);
    json_object_set_boolean_member(resp, "added", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `stack_regions_edit` handler - edit existujícího regionu.
 *
 * Parametry v `data` poli requestu:
 *  - `index` (int, povinné) - index 0..count-1 editovaného regionu.
 *  - `name` (string, povinné) - nový label.
 *  - `base` (int, povinné) - nový vrchol.
 *  - `limit` (int, povinné) - nové dno (base > limit).
 *
 * Response payload:
 *  - `updated` (bool) - true při úspěchu. Při úspěchu se navíc resetuje
 *    watermark + counters (= staré stats neplatí pro nový rozsah).
 */
static en_MCP_DISPATCH_RESULT _handle_stack_regions_edit(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    const char *name = json_object_has_member(data_obj, "name")
        ? json_object_get_string_member(data_obj, "name") : NULL;
    if (!name ||
        !json_object_has_member(data_obj, "index") ||
        !json_object_has_member(data_obj, "base") ||
        !json_object_has_member(data_obj, "limit")) {
        return _err_response(req_id, "Missing 'index', 'name', 'base' or 'limit'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 idx   = _obj_int_or(data_obj, "index", -1);
    gint64 base  = _obj_int_or(data_obj, "base",  -1);
    gint64 limit = _obj_int_or(data_obj, "limit", -1);
    if (idx < 0 || idx > 255 ||
        base < 0 || base > 0xFFFF || limit < 0 || limit > 0xFFFF) {
        return _err_response(req_id, "Invalid 'index', 'base' or 'limit'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_STACK_REGIONS_EDIT_PARAM p;
    memset(&p, 0, sizeof(p));
    p.idx = (uint8_t)idx;
    g_strlcpy(p.name, name, sizeof(p.name));
    p.base  = (uint16_t)base;
    p.limit = (uint16_t)limit;

    bool ok = _submit_dbgapi(DBGAPI_CMD_STACK_REGIONS_EDIT, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "updated", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `stack_regions_remove` handler - odebere region na indexu.
 *
 * Parametry v `data` poli requestu:
 *  - `index` (int, povinné) - index existujícího regionu.
 *
 * Response payload:
 *  - `removed` (bool) - true při úspěchu, false pokud index out-of-range.
 */
static en_MCP_DISPATCH_RESULT _handle_stack_regions_remove(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "index")) {
        return _err_response(req_id, "Missing 'index'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 idx = _obj_int_or(data_obj, "index", -1);
    if (idx < 0 || idx > 255) {
        return _err_response(req_id, "Invalid 'index'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_STACK_REGIONS_REMOVE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.index = (int)idx;

    bool ok = _submit_dbgapi(DBGAPI_CMD_STACK_REGIONS_REMOVE, &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "removed", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `stack_regions_reset_watermark` handler - reset stats jednoho
 *        regionu (watermark + push/pop counters).
 *
 * Parametry v `data` poli requestu:
 *  - `index` (int, povinné) - index regionu.
 *
 * Response payload:
 *  - `reset` (bool) - true při úspěchu.
 *
 * Side effects: nuluje watermark, push_count a pop_count daného regionu.
 * Konfigurační pole (name, base, limit) zůstávají.
 */
static en_MCP_DISPATCH_RESULT _handle_stack_regions_reset_watermark(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "index")) {
        return _err_response(req_id, "Missing 'index'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 idx = _obj_int_or(data_obj, "index", -1);
    if (idx < 0 || idx > 255) {
        return _err_response(req_id, "Invalid 'index'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_STACK_REGIONS_REMOVE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.index = (int)idx;

    bool ok = _submit_dbgapi(DBGAPI_CMD_STACK_REGIONS_RESET_WATERMARK,
                             &p, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "reset", ok ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief Base64 encoder - hex-fallback alternativa byla zvážena, ale
 *        json-glib API nemá ready-made base64, takže používáme glib.
 *
 * GLib `g_base64_encode` vrací malloc-aware string který musíme
 * uvolnit přes `g_free`.
 */
static gchar *_b64_encode_take(const guint8 *data, gsize len) {
    return g_base64_encode(data, len);
}


/**
 * @brief `mem_read` handler - čte blok paměti.
 *
 * Parametry v `data` poli requestu:
 *  - `addr` (int, 0..65535) - počáteční adresa
 *  - `len`  (int, 1..65535) - počet bajtů; součet addr+len musí být
 *    <= 65536 jinak se neauthorizujeme (= MEM_READ adresuje 16-bit
 *    prostor s wrap-around v emulátoru, ale my odmítáme)
 *
 * Response payload:
 *  - `addr` (int) - echo
 *  - `len`  (int) - echo
 *  - `data_b64` (string) - obsah paměti zakódovaný base64
 */
static en_MCP_DISPATCH_RESULT _handle_mem_read(const st_JSONL_MESSAGE *req,
                                               char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 addr = _obj_int_or(data_obj, "addr", -1);
    gint64 len  = _obj_int_or(data_obj, "len",  -1);
    if (addr < 0 || addr > 0xFFFF || len <= 0 || len > 0xFFFF ||
        (addr + len) > 0x10000) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint8_t *buf = g_malloc0((gsize)len);
    st_DBGAPI_MEM_PARAM param = {
        .addr = (uint16_t)addr,
        .len  = (uint16_t)len,
        .buf  = buf,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_MEM_READ, &param, NULL)) {
        g_free(buf);
        return _err_response(req_id, "MEM_READ failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    gchar *b64 = _b64_encode_take(buf, (gsize)len);
    g_free(buf);
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr", addr);
    json_object_set_int_member(resp, "len",  len);
    json_object_set_string_member(resp, "data_b64", b64 ? b64 : "");
    g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief Dekóduje hex string (případně mezerou oddělené dvojice) do bufferu.
 *
 * Akceptuje "DEADBEEF", "DE AD BE EF" a malé/velké písmena. Vrací počet
 * úspěšně zapsaných bajtů; při výskytu non-hex znaku vrátí -1.
 *
 * @param[in]  hex     null-terminated vstup
 * @param[out] out     výstupní buffer (vlastní caller)
 * @param[in]  out_max maximální počet bajtů které smí funkce zapsat
 * @return počet zapsaných bajtů (>= 0) nebo -1 při chybě parsování
 */
static int _decode_hex(const char *hex, uint8_t *out, gsize out_max) {
    if (!hex || !out) return -1;
    gsize written = 0;
    int   nibble  = -1;
    for (const char *p = hex; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int v;
        if (c >= '0' && c <= '9')      v = c - '0';
        else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
        else return -1;
        if (nibble < 0) {
            nibble = v;
        } else {
            if (written >= out_max) return -1;
            out[written++] = (uint8_t)((nibble << 4) | v);
            nibble = -1;
        }
    }
    if (nibble >= 0) return -1; /* odd-length hex string */
    return (int)written;
}


/**
 * @brief `mem_write` handler - zápis bloku do Z80 paměti s region check.
 *
 * Parametry v `data` poli requestu:
 *  - `addr`     (int, 0..65535) - počáteční adresa
 *  - `data_hex` (string)        - hex reprezentace bajtů ke zápisu
 *    (např. `"DEADBEEF"` = 4 bajty 0xDE, 0xAD, 0xBE, 0xEF). Akceptuje i
 *    mezerou oddělené dvojice (`"DE AD BE EF"`).
 *
 * Implementace volá `DBGAPI_CMD_MEM_WRITE_CHECKED` (= zápis s region
 * verifikací). Pokud kterákoliv adresa padne do ROM, prohibited nebo
 * unmapped regionu, žádný bajt se nezapíše a handler vrátí success=false
 * s polem `first_failed_addr` v error payloadu.
 *
 * Response data (success):
 *  - `addr`   (int) - echo
 *  - `length` (int) - počet zapsaných bajtů
 *
 * Response data (failure region check):
 *  - JSONL `success=false` + `error="MEM_WRITE region check failed"`
 *
 * Pozn.: V0.B.3 neimplementuje žádný `force_rom` override (= scope V1+).
 * AI klient by si měl tuto destruktivní operaci nechat odsouhlasit
 * uživatelem - viz tool description v `mcp_server.py`.
 */
static en_MCP_DISPATCH_RESULT _handle_mem_write(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 addr = _obj_int_or(data_obj, "addr", -1);
    if (addr < 0 || addr > 0xFFFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!json_object_has_member(data_obj, "data_hex")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *hex_node = json_object_get_member(data_obj, "data_hex");
    if (!hex_node || json_node_is_null(hex_node) ||
        json_node_get_node_type(hex_node) != JSON_NODE_VALUE) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const gchar *data_hex = json_node_get_string(hex_node);
    if (!data_hex || data_hex[0] == '\0') {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Horní limit dekódovaného bloku - drží konzistenci s mem_read
     * (0..65535) a chrání před monstrózními alokacemi z malicious vstupu. */
    const gsize MAX_LEN = 0x10000;
    uint8_t *bytes = g_malloc(MAX_LEN);
    int decoded = _decode_hex(data_hex, bytes, MAX_LEN);
    if (decoded <= 0 || (addr + decoded) > 0x10000) {
        g_free(bytes);
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_MEM_WRITE_CHECKED_PARAM param = {
        .addr               = (uint16_t)addr,
        .length             = (uint16_t)decoded,
        .data               = bytes,
        .success            = 0,
        .first_failed_addr  = 0,
        .first_failed_kind  = 0,
    };
    bool submit_ok = _submit_dbgapi(DBGAPI_CMD_MEM_WRITE_CHECKED, &param, NULL);
    g_free(bytes);

    if (!submit_ok) {
        return _err_response(req_id, "MEM_WRITE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (!param.success) {
        /* Region check abortoval celý zápis. Klient dostane structured
         * error s adresou kde to selhalo. */
        gchar *msg = g_strdup_printf(
            "MEM_WRITE region check failed at 0x%04X (kind=%u)",
            param.first_failed_addr, param.first_failed_kind);
        en_MCP_DISPATCH_RESULT rc = _err_response(
            req_id, msg, MCP_DISPATCH_EMU_ERROR, out_response);
        g_free(msg);
        return rc;
    }

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr",   addr);
    json_object_set_int_member(resp, "length", decoded);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bp_add` handler - přidá execution breakpoint na adresu.
 *
 * Parametr v `data`:
 *  - `addr` (int, 0..65535) - adresa BP
 *
 * Response:
 *  - `id` (int) - ID přiděleného breakpointu
 *  - `addr` (int) - echo
 *
 * Poznámka: V0.A.3 podporuje jen execution BP (= addr-only). Typed
 * BPs (MEM_READ/WRITE, IO, condition) jsou v scope V0.B+/V1.
 */
static en_MCP_DISPATCH_RESULT _handle_bp_add(const st_JSONL_MESSAGE *req,
                                             char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 addr = _obj_int_or(data_obj, "addr", -1);
    if (addr < 0 || addr > 0xFFFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_BP_PARAM param = {
        .addr = (uint16_t)addr,
        .id   = 0,   /* handler v dbgapi naplní přidělené ID */
    };
    if (!_submit_dbgapi(DBGAPI_CMD_BP_ADD, &param, NULL)) {
        return _err_response(req_id, "BP_ADD failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "id",   param.id);
    json_object_set_int_member(resp, "addr", addr);
    return _ok_response(req_id, resp, out_response);
}


/** @brief Velikost ad-hoc bp_list buferu - dostatečné pro V0.A.3. */
#define MCP_DISPATCH_BP_LIST_MAX 256


/**
 * @brief Uvolní výsledek DBGAPI_CMD_BP_LIST včetně heap condition stringů.
 *
 * Sdílený cleanup pro VŠECHNY callery DBGAPI_CMD_BP_LIST. Handler v
 * dbgapi.c naplní bp[i].condition přes g_strdup() (ownership kontrakt,
 * viz st_DBGAPI_BP_LIST_RESULT v dbgapi_cmdrq.h) a caller je POVINEN
 * každé bp[i].condition uvolnit. Tato funkce projde celý rozsah
 * [0, count) a uvolní condition stringy, poté uvolní samotnou strukturu.
 *
 * @param result Výsledek BP_LIST (smí být NULL = no-op). Po návratu je
 *               ukazatel neplatný (caller jej už nesmí dereferencovat).
 *
 * Předpoklady: result->count odpovídá počtu naplněných prvků bp[]
 * (handler ho nastaví; pokud BP_LIST selhal a struktura byla
 * g_malloc0(), count == 0 a smyčka neuvolní nic = uniformně korektní).
 */
static void _free_bp_list_result(st_DBGAPI_BP_LIST_RESULT *result) {
    if (!result) {
        return;
    }
    for (int i = 0; i < result->count && i < result->max_count; i++) {
        g_free(result->bp[i].condition);
    }
    g_free(result);
}


/**
 * @brief `bp_list` handler - vrátí pole breakpointů s plnými atributy.
 *
 * Alokuje `st_DBGAPI_BP_LIST_RESULT` s flexibilním polem o velikosti
 * `MCP_DISPATCH_BP_LIST_MAX` (256). Response payload:
 *  - `count` (int) - počet vrácených BP
 *  - `breakpoints` (array) - každý prvek
 *    `{id, addr, enabled, type, zone, bank_id, hits, condition}`.
 *    `type` / `zone` jsou kanonické UPPER_SNAKE řetězce
 *    (bpt_type_to_string / bp_zone_to_string), `condition` je expr výraz
 *    nebo null pokud je BP bezpodmínečný.
 *
 * Pozn.: `bp[i].condition` je heap g_strdup() z dbgapi handleru, handler
 * jej po serializaci uvolní g_free().
 */
static en_MCP_DISPATCH_RESULT _handle_bp_list(const st_JSONL_MESSAGE *req,
                                              char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    const gsize struct_size = sizeof(st_DBGAPI_BP_LIST_RESULT)
                              + MCP_DISPATCH_BP_LIST_MAX
                              * sizeof(((st_DBGAPI_BP_LIST_RESULT *)0)->bp[0]);
    st_DBGAPI_BP_LIST_RESULT *result = g_malloc0(struct_size);
    result->max_count = MCP_DISPATCH_BP_LIST_MAX;
    result->count = 0;
    if (!_submit_dbgapi(DBGAPI_CMD_BP_LIST, NULL, result)) {
        _free_bp_list_result(result);
        return _err_response(req_id, "BP_LIST failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonArray *arr = json_array_new();
    for (int i = 0; i < result->count && i < result->max_count; i++) {
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "id",      result->bp[i].id);
        json_object_set_int_member(item, "addr",    result->bp[i].addr);
        json_object_set_boolean_member(item, "enabled",
                                       result->bp[i].enabled);
        json_object_set_string_member(item, "type",
            bpt_type_to_string((en_BPT_TYPE)result->bp[i].type));
        json_object_set_string_member(item, "zone",
            bp_zone_to_string((en_BP_ZONE)result->bp[i].zone));
        json_object_set_int_member(item, "bank_id", result->bp[i].bank_id);
        json_object_set_int_member(item, "hits",
                                   (gint64)result->bp[i].hits);
        if (result->bp[i].condition) {
            /* json-glib si string zkopíruje, vlastní uvolnění až
             * v _free_bp_list_result() níže. */
            json_object_set_string_member(item, "condition",
                                          result->bp[i].condition);
        } else {
            json_object_set_null_member(item, "condition");
        }
        json_array_add_object_element(arr, item);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", result->count);
    json_object_set_array_member(resp, "breakpoints", arr);
    /* Uvolní strukturu + všechna bp[i].condition (ownership kontrakt). */
    _free_bp_list_result(result);
    return _ok_response(req_id, resp, out_response);
}


/* ------------------------------------------------------------------ */
/* V0.B.6 - 7 chybějících V0 Tools                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief `bp_remove` handler - odstraní breakpoint podle ID.
 *
 * Parametr v `data`:
 *  - `id` (int) - ID existujícího breakpointu (vrácené dříve z `bp_add`
 *    nebo viditelné v `bp_list`).
 *
 * Response při úspěchu:
 *  - `id` (int) - echo
 *  - `removed` (bool) - true
 *
 * Při neplatném ID (= dbgapi handler vrátí success=false) vrací error
 * response s textem "bp_remove failed (unknown id?)".
 *
 * Volá `DBGAPI_CMD_BP_REMOVE` s `st_DBGAPI_BP_PARAM { .id = X }`
 * (addr se v dbgapi handleru ignoruje, viz dbgapi.c case BP_REMOVE).
 */
static en_MCP_DISPATCH_RESULT _handle_bp_remove(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: id",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "id")) {
        return _err_response(req_id, "Missing required field: id",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 bp_id = _obj_int_or(data_obj, "id", -1);
    if (bp_id < 0) {
        return _err_response(req_id, "Invalid id",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_BP_PARAM param = {
        .addr = 0,           /* dbgapi ignoruje při BP_REMOVE */
        .id   = (int)bp_id,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_BP_REMOVE, &param, NULL)) {
        return _err_response(req_id, "bp_remove failed (unknown id?)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "id", bp_id);
    json_object_set_boolean_member(resp, "removed", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bp_clear` handler - smaže všechny breakpointy hromadně.
 *
 * Žádné parametry. Implementace dvoufázová:
 *  1. `DBGAPI_CMD_BP_LIST` -> načte aktuální BP IDs
 *  2. Per BP zavolá `DBGAPI_CMD_BP_REMOVE` (= 1 + N dbgapi transakcí)
 *
 * Důvod loop: dbgapi nemá dedikovaný "remove all" handler v V0 scope
 * (= aby V0.B.6 nemusel zasahovat do dbgapi.c). Pokud bude V1+
 * potřebovat atomicity, lze přidat `DBGAPI_CMD_BP_REMOVE_ALL`.
 *
 * Response:
 *  - `count` (int) - kolik BP bylo odstraněno
 *  - `cleared` (bool) - true
 *
 * Pokud BP_LIST selže, vrací error response. Pokud BP_REMOVE některého
 * ID selže (= race s GUI mazáním), handler pokračuje (best-effort).
 */
static en_MCP_DISPATCH_RESULT _handle_bp_clear(const st_JSONL_MESSAGE *req,
                                               char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    /* Fáze 1: snapshot existujících BP. */
    const gsize struct_size = sizeof(st_DBGAPI_BP_LIST_RESULT)
                              + MCP_DISPATCH_BP_LIST_MAX
                              * sizeof(((st_DBGAPI_BP_LIST_RESULT *)0)->bp[0]);
    st_DBGAPI_BP_LIST_RESULT *result = g_malloc0(struct_size);
    result->max_count = MCP_DISPATCH_BP_LIST_MAX;
    result->count = 0;
    if (!_submit_dbgapi(DBGAPI_CMD_BP_LIST, NULL, result)) {
        _free_bp_list_result(result);
        return _err_response(req_id, "bp_clear failed (BP_LIST)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    /* Fáze 2: per-ID BP_REMOVE. */
    int removed = 0;
    for (int i = 0; i < result->count && i < result->max_count; i++) {
        st_DBGAPI_BP_PARAM param = {
            .addr = 0,
            .id   = result->bp[i].id,
        };
        if (_submit_dbgapi(DBGAPI_CMD_BP_REMOVE, &param, NULL)) {
            removed++;
        }
    }
    /* Uvolní strukturu + bp[i].condition (= dříve leak, F015a regrese). */
    _free_bp_list_result(result);
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", removed);
    json_object_set_boolean_member(resp, "cleared", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bp_enable` handler - toggle BP enabled flagu (bez mazání).
 *
 * Parametry v `data`:
 *  - `id` (int) - ID existujícího BP
 *  - `enabled` (bool) - nový stav (true = aktivní, false = zachován,
 *    ale neaktivní)
 *
 * Response:
 *  - `id` (int) - echo
 *  - `enabled` (bool) - echo
 *
 * Volá `DBGAPI_CMD_BP_SET_ENABLED` s `st_DBGAPI_BP_SET_ENABLED_PARAM`.
 */
static en_MCP_DISPATCH_RESULT _handle_bp_enable(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required fields: id, enabled",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "id") ||
        !json_object_has_member(data_obj, "enabled")) {
        return _err_response(req_id, "Missing required fields: id, enabled",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 bp_id = _obj_int_or(data_obj, "id", -1);
    if (bp_id < 0) {
        return _err_response(req_id, "Invalid id",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *enabled_node = json_object_get_member(data_obj, "enabled");
    if (!enabled_node || json_node_is_null(enabled_node) ||
        json_node_get_node_type(enabled_node) != JSON_NODE_VALUE) {
        return _err_response(req_id, "Invalid enabled flag",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gboolean enabled = json_node_get_boolean(enabled_node);
    st_DBGAPI_BP_SET_ENABLED_PARAM param = {
        .id      = (int)bp_id,
        .enabled = (bool)enabled,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_BP_SET_ENABLED, &param, NULL)) {
        return _err_response(req_id, "bp_enable failed (unknown id?)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "id", bp_id);
    json_object_set_boolean_member(resp, "enabled", enabled);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `step_into` handler - 1 instrukce krok (step into CALL/RST).
 *
 * Žádné parametry. Volá `DBGAPI_CMD_STEP_INTO`. Pokud emu běží (=
 * není pause), dbgapi handler ho nejdřív pause-uje a step se neprovede
 * (= klient musí volat znovu po pause). Caller (= AI klient) si tento
 * UX kontrakt musí ohlídat - typicky: `emu_pause` -> `emu_step_into`.
 *
 * Response:
 *  - `stepped` (bool) - true
 */
static en_MCP_DISPATCH_RESULT _handle_step_into(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL)) {
        return _err_response(req_id, "step_into failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "stepped", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `step_over` handler - 1 instrukce, ale CALL/RST jako atom.
 *
 * Žádné parametry. Volá `DBGAPI_CMD_STEP_OVER`. Pro non-call instrukce
 * se chová jako step_into; pro CALL/RST/DJNZ/blokové instrukce nastaví
 * dočasný BP na addr+length a run-to. Vyžaduje pause stav (= dbgapi
 * handler v dbgapi.c kontroluje `EMULATOR_TEST_PAUSED` - pokud běží,
 * akce se neprovede).
 *
 * Response:
 *  - `stepped` (bool) - true
 */
static en_MCP_DISPATCH_RESULT _handle_step_over(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_STEP_OVER, NULL, NULL)) {
        return _err_response(req_id, "step_over failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "stepped", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `step_n` handler - N instrukcí jako sekvence step_into.
 *
 * Parametr v `data`:
 *  - `count` (int, 1..1000) - počet kroků
 *
 * Implementace volá `DBGAPI_CMD_STEP_INTO` v cyklu. Pokud kterýkoliv
 * step selže (= submit vrátí false), handler ukončí smyčku a vrátí
 * counter dosud úspěšných kroků s `partial=true` flagem. To dovoluje
 * klientovi rozpoznat, že se nestihlo dokrokovat - např. proto, že
 * emu mezitím přešel do pause stavu z jiného důvodu (= breakpoint hit).
 *
 * Response:
 *  - `count` (int) - kolik kroků se skutečně provedlo (= min(requested,
 *    první failed))
 *  - `requested` (int) - echo původního count
 *  - `partial` (bool) - true pokud count < requested
 *
 * Pozn.: Limit 1..1000 shoda s `emu_run(frames)` limit z V0.B.3.
 * Vyšší hodnoty by zatěžovaly fronu dbgapi (= 1000 sync transakcí
 * trvá řádově sekundy).
 */
static en_MCP_DISPATCH_RESULT _handle_step_n(const st_JSONL_MESSAGE *req,
                                             char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: count",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 count = _obj_int_or(data_obj, "count", -1);
    if (count < 1 || count > 1000) {
        return _err_response(req_id, "Invalid count range (1..1000)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    int done = 0;
    for (gint64 i = 0; i < count; i++) {
        if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL)) {
            break;
        }
        done++;
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", done);
    json_object_set_int_member(resp, "requested", count);
    json_object_set_boolean_member(resp, "partial", done < count);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `run_until_addr` handler - run dokud PC == addr (s timeoutem).
 *
 * Parametry v `data`:
 *  - `addr` (int, 0..65535) - cílová adresa
 *  - `max_cycles` (int, volitelný) - informativní timeout v T-states
 *    pro klienta. dbgapi implementace `DBGAPI_CMD_RUN_TO` aktuálně
 *    timeout nezahrnuje (= temp BP + RUN, žádný cycle limit). Pole
 *    se v V0.B.6 do dbgapi nepropaguje; je tu pro budoucí rozšíření
 *    (V1.A `run_until_event` family). Klient může pollovat
 *    `get_state` a sám pauznout, pokud emulace zatím nedoběhla.
 *
 * Response:
 *  - `addr` (int) - echo
 *  - `running` (bool) - true (emu nastartoval temp-BP + run, dosáhnutí
 *    cílové addr signalizuje pause + breakpoint event; klient musí
 *    pollovat `get_state`)
 *
 * Volá `DBGAPI_CMD_RUN_TO` s data_ptr = uint16_t* (cílová adresa).
 *
 * Pokud emu už běží, dbgapi handler vrátí success=false (= pause +
 * return UX z dbg_iconbar.cpp). Klient musí předem zavolat `pause`.
 */
static en_MCP_DISPATCH_RESULT _handle_run_until_addr(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: addr",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 addr = _obj_int_or(data_obj, "addr", -1);
    if (addr < 0 || addr > 0xFFFF) {
        return _err_response(req_id, "Invalid addr range (0..65535)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* max_cycles je v V0.B.6 informativní (= prochází requestem, ale
     * dbgapi handler ho neimplementuje). Validace rozsahu pro budoucí
     * V1.A timeout cesta. */
    gint64 max_cycles = _obj_int_or(data_obj, "max_cycles", 10000000);
    if (max_cycles < 1) {
        return _err_response(req_id, "Invalid max_cycles",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint16_t target = (uint16_t)addr;
    if (!_submit_dbgapi(DBGAPI_CMD_RUN_TO, &target, NULL)) {
        return _err_response(req_id, "run_until_addr failed (emu running?)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr", addr);
    json_object_set_int_member(resp, "max_cycles", max_cycles);
    json_object_set_boolean_member(resp, "running", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_mcp_config` handler - vrátí runtime hodnoty MCP konfigurace.
 *
 * Lokální handler (= žádné dbgapi volání). Backing pro Python MCP
 * Resource `emulator://config/mcp`. Vrací JSON objekt s těmito poli:
 *
 *  - `tcp_port`        (int)    - aktuální port TCP listeneru.
 *  - `bind_address`    (string) - "127.0.0.1" nebo "0.0.0.0".
 *  - `profile`         (string) - "wild" / "confined" / "sandboxed" /
 *                                 "observer".
 *  - `auto_start_tcp`  (bool)   - true pokud má TCP listener naběhnout
 *                                 automaticky při startu emu.
 *  - `tcp_enabled`     (bool)   - true pokud byl emu zbuilden s
 *                                 `MZ800EMU_CFG_MCP_TCP_ENABLED`; pokud
 *                                 false, zbylá pole jsou default
 *                                 hodnoty (= bez INI persistence).
 *
 * Pri buildu s `NO_MCP_TCP=1` (kaskáda `NO_MCP=1` se ani sem
 * nedostane) `g_mcp_config` neexistuje. Handler v takovém režimu vrací
 * `tcp_enabled=false` + hardcoded defaults, aby Resource zůstalo
 * dostupné a klient se z payloadu dozvěděl, že TCP je vypnuté.
 *
 * V testovacím buildu (`MZ800EMU_MCP_TEST_BUILD`) se `mcp_config.h`
 * neincluduje (= dispatch.c je tam přeložen v izolaci), takže handler
 * běží také v "stub" větvi.
 *
 * @param[in]  req           parsed REQUEST (validní)
 * @param[out] out_response  výstupní JSONL řádek (caller `free()`)
 * @return MCP_DISPATCH_OK při úspěchu, MCP_DISPATCH_ALLOC_ERROR pokud
 *         se nepodařilo alokovat response řádek.
 */
static en_MCP_DISPATCH_RESULT _handle_get_mcp_config(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *data = json_object_new();

#if defined(MZ800EMU_CFG_MCP_TCP_ENABLED) && !defined(MZ800EMU_MCP_TEST_BUILD)
    json_object_set_int_member(data, "tcp_port",
                               (gint64)g_mcp_config.tcp_port);
    json_object_set_string_member(data, "bind_address",
        mcp_config_bind_addr_str(g_mcp_config.bind_addr));
    json_object_set_string_member(data, "profile",
        mcp_config_profile_str(g_mcp_config.profile));
    json_object_set_boolean_member(data, "auto_start_tcp",
        g_mcp_config.auto_start_tcp ? TRUE : FALSE);
    json_object_set_boolean_member(data, "tcp_enabled", TRUE);
#else
    /* Stub větev: TCP build vypnut nebo standalone test - vracíme
     * default hodnoty odpovídající `mcp_config.c` initializeru, plus
     * `tcp_enabled=false` jako příznak pro klienta. */
    json_object_set_int_member(data,    "tcp_port",       23800);
    json_object_set_string_member(data, "bind_address",   "127.0.0.1");
    json_object_set_string_member(data, "profile",        "wild");
    json_object_set_boolean_member(data, "auto_start_tcp", FALSE);
    json_object_set_boolean_member(data, "tcp_enabled",   FALSE);
#endif

    return _ok_response(req_id, data, out_response);
}


/* ------------------------------------------------------------------ */
/* V1.A.1 - Snapshot Tools + cooperation hint                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Vrátí copy string hodnoty z JsonObject nebo NULL.
 *
 * Pro mandatory / optional string parametry v V1.A.1 requestech. Caller
 * uvolní přes g_free. NULL znamená "klíč chybí" NEBO "klíč je null"
 * NEBO "klíč není string" - rozdíl není pro V1.A.1 podstatný.
 */
static char *_obj_str_dup(JsonObject *obj, const char *key) {
    if (!obj || !json_object_has_member(obj, key)) return NULL;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || json_node_is_null(n)) return NULL;
    if (json_node_get_node_type(n) != JSON_NODE_VALUE) return NULL;
    const char *s = json_node_get_string(n);
    return s ? g_strdup(s) : NULL;
}


/**
 * @brief `snapshot_save` handler - uloží snapshot do souboru na disk.
 *
 * Parametry v `data` poli:
 *   - `path` (string, povinný) - filesystem cesta k .mzs souboru.
 *   - `description` (string, optional) - popis snapshotu vložený do
 *     metadat. Default prázdný.
 *
 * Volá `DBGAPI_CMD_SNAPSHOT_SAVE_FILE` (= snapshot_save z V-1.2 API).
 * Snapshot vyžaduje paused emu - pokud emu běží, dbgapi handler vrátí
 * success=false a my odpovíme MCP_DISPATCH_EMU_ERROR.
 *
 * Response payload (success):
 *   - `path` (string) - echo cesty
 *   - `ok` (bool true)
 */
static en_MCP_DISPATCH_RESULT _handle_snapshot_save(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *path = _obj_str_dup(data_obj, "path");
    if (!path || path[0] == '\0') {
        g_free(path);
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *desc = _obj_str_dup(data_obj, "description");

    st_DBGAPI_SNAPSHOT_PARAM param = {
        .filepath    = path,
        .description = desc,    /* může být NULL = prázdný popis */
        .buffer      = NULL,
        .buffer_size = 0,
        .result      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SNAPSHOT_SAVE_FILE, &param, NULL);
    int  result_code = param.result;
    if (!ok) {
        g_free(path);
        g_free(desc);
        return _err_response(req_id, "snapshot_save failed (emu not paused?)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    /* Pozn.: echo cesty bereme z lokální path proměnné PŘED jejím
     * uvolněním - param.filepath po g_free(path) by ukazoval na
     * uvolněnou paměť. */
    json_object_set_string_member(resp, "path", path);
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "result_code", result_code);
    g_free(path);
    g_free(desc);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `snapshot_save_buffer` handler - uloží snapshot jako inline base64.
 *
 * Parametry:
 *   - `description` (string, optional) - popis snapshotu.
 *
 * Volá `DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER` (= snapshot_save_to_buffer z
 * V-1.2 API). Handler alokuje buffer, my ho převedeme na base64 a
 * uvolníme přes g_free.
 *
 * Response payload (success):
 *   - `bytes_b64` (string) - base64-encoded .mzs ZIP obsah
 *   - `size` (int) - velikost dekódovaných dat v bajtech
 *   - `ok` (bool true)
 */
static en_MCP_DISPATCH_RESULT _handle_snapshot_save_buffer(const st_JSONL_MESSAGE *req,
                                                            char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    /* data_node může být NULL (= request bez data sekce) */
    char *desc = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        desc = _obj_str_dup(json_node_get_object(data_node), "description");
    }

    st_DBGAPI_SNAPSHOT_PARAM param = {
        .filepath    = NULL,
        .description = desc,
        .buffer      = NULL,
        .buffer_size = 0,
        .result      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER, &param, NULL);
    g_free(desc);
    if (!ok || !param.buffer || param.buffer_size == 0) {
        if (param.buffer) g_free(param.buffer);
        return _err_response(req_id, "snapshot_save_buffer failed (emu not paused?)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    gchar *b64 = g_base64_encode(param.buffer, param.buffer_size);
    g_free(param.buffer);
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "bytes_b64", b64 ? b64 : "");
    json_object_set_int_member(resp, "size", (gint64)param.buffer_size);
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "result_code", param.result);
    g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `snapshot_load` handler - načte snapshot ze souboru.
 *
 * Parametry:
 *   - `path` (string, povinný) - filesystem cesta k .mzs souboru.
 *
 * Volá `DBGAPI_CMD_SNAPSHOT_LOAD_FILE`. Po success je nový emu state
 * aktivní; klient typicky následně volá get_state / get_registers.
 *
 * Response payload (success):
 *   - `path` (string) - echo
 *   - `ok` (bool true)
 */
static en_MCP_DISPATCH_RESULT _handle_snapshot_load(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *path = _obj_str_dup(data_obj, "path");
    if (!path || path[0] == '\0') {
        g_free(path);
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_SNAPSHOT_PARAM param = {
        .filepath    = path,
        .description = NULL,
        .buffer      = NULL,
        .buffer_size = 0,
        .result      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SNAPSHOT_LOAD_FILE, &param, NULL);
    int  result_code = param.result;
    if (!ok) {
        g_free(path);
        return _err_response(req_id, "snapshot_load failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    /* Pozn.: echo path z lokální proměnné před g_free (param.filepath
     * po uvolnění už není platný ukazatel). */
    json_object_set_string_member(resp, "path", path);
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "result_code", result_code);
    g_free(path);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `snapshot_load_buffer` handler - načte snapshot z inline base64.
 *
 * Parametry:
 *   - `bytes_b64` (string, povinný) - base64-encoded .mzs ZIP obsah.
 *
 * Dekóduje base64, volá `DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER`. Klient typicky
 * získal `bytes_b64` z dřívějšího `snapshot_save_buffer` volání.
 *
 * Response payload (success):
 *   - `size` (int) - dekódovaná velikost
 *   - `ok` (bool true)
 */
static en_MCP_DISPATCH_RESULT _handle_snapshot_load_buffer(const st_JSONL_MESSAGE *req,
                                                            char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: bytes_b64",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *b64 = _obj_str_dup(data_obj, "bytes_b64");
    if (!b64 || b64[0] == '\0') {
        g_free(b64);
        return _err_response(req_id, "Missing required field: bytes_b64",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gsize decoded_len = 0;
    guchar *decoded = g_base64_decode(b64, &decoded_len);
    g_free(b64);
    if (!decoded || decoded_len == 0) {
        if (decoded) g_free(decoded);
        return _err_response(req_id, "Invalid base64 payload",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_SNAPSHOT_PARAM param = {
        .filepath    = NULL,
        .description = NULL,
        .buffer      = decoded,
        .buffer_size = decoded_len,
        .result      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER, &param, NULL);
    g_free(decoded);
    if (!ok) {
        return _err_response(req_id, "snapshot_load_buffer failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "size", (gint64)decoded_len);
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "result_code", param.result);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `cooperation_hint_set` handler - self-binding instrukce AI klienta.
 *
 * Parametry:
 *   - `mode` (string, povinný) - "free" / "read_only" / "paused_only".
 *   - `until` (string, optional) - lidsky čitelný "until" hint (např.
 *     "next user message" nebo ISO timestamp). Default prázdný =
 *     otevřená délka.
 *
 * Lokální handler - žádné dbgapi volání. Nastaví g_cooperation_hint
 * v MCP vrstvě. Per rozboru sekce 3.3.2 je hint dobrovolný; V1.A.1
 * neenforcoval žádnou policy - jen state + budoucí notifikace pro UI
 * (Activity Log V1.C).
 *
 * Response payload (success):
 *   - `mode` (string) - echo přijatého módu
 *   - `until` (string) - echo (nebo prázdný)
 *   - `ok` (bool true)
 */
static en_MCP_DISPATCH_RESULT _handle_cooperation_hint_set(const st_JSONL_MESSAGE *req,
                                                            char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: mode",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *mode_str = _obj_str_dup(data_obj, "mode");
    if (!mode_str || mode_str[0] == '\0') {
        g_free(mode_str);
        return _err_response(req_id, "Missing required field: mode",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    en_COOPERATION_HINT mode;
    if (!cooperation_hint_mode_from_str(mode_str, &mode)) {
        g_free(mode_str);
        return _err_response(req_id,
            "Invalid mode (expected: free, read_only, paused_only)",
            MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *until = _obj_str_dup(data_obj, "until");
    cooperation_hint_set(mode, until);

    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "mode",
        cooperation_hint_mode_to_str(mode));
    json_object_set_string_member(resp, "until", until ? until : "");
    json_object_set_boolean_member(resp, "ok", TRUE);
    g_free(mode_str);
    g_free(until);
    return _ok_response(req_id, resp, out_response);
}


/* ====================================================================== */
/* V1.A.2 - Symbol management Tools                                        */
/* ====================================================================== */

/**
 * @brief Helper - pokus o parse hex string ("0x4242", "4242h", "$4242",
 *        "4242") na uint16_t adresu.
 *
 * Toleruje prefix 0x/0X/$, suffix h/H, jinak parsuje jako hex bez prefixu.
 * Decimal NEpodporuje (V1.A.2 scope - lookup hex je explicit konvence).
 *
 * @param[in]  str    Vstupní řetězec (non-NULL).
 * @param[out] out    Vyplněná adresa při úspěchu (0..65535).
 * @return true při úspěšném parse, false jinak.
 */
static bool _parse_hex_addr(const char *str, uint16_t *out) {
    if (!str || !str[0] || !out) return false;
    const char *p = str;
    /* Skip prefix */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (p[0] == '$') p += 1;
    /* Zjisti délku k případnému suffixu 'h'/'H' */
    size_t len = strlen(p);
    if (len == 0) return false;
    bool has_h_suffix = false;
    if (p[len - 1] == 'h' || p[len - 1] == 'H') {
        has_h_suffix = true;
        len -= 1;
    }
    if (len == 0 || len > 4) return false;
    /* Validace - všechny znaky musí být hex */
    char buf[5];
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
        buf[i] = c;
    }
    buf[len] = '\0';
    (void)has_h_suffix;   /* signál byl jen pro detekci; parser je hex */
    /* Použij strtoul s base 16 */
    char *endp = NULL;
    unsigned long v = strtoul(buf, &endp, 16);
    if (!endp || *endp != '\0' || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}


/**
 * @brief Zabudovaná validace symbol jména (= povolené znaky pro identifier).
 *
 * Reálné sym_db nemá strict whitelist znaků, ale MCP úroveň záměrně
 * odmítá whitespace a prázdný string (= problém pro parsery .lbl
 * a disassembler labely). Povolené: A-Z, a-z, 0-9, _, .
 */
static bool _is_valid_symbol_name(const char *name) {
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}


/**
 * @brief `symbol_add` handler - přidá user-defined symbol (LBL source).
 *
 * Parametry:
 *   - `addr` (int, povinný) - 0..65535.
 *   - `name` (string, povinný) - identifier (no whitespace).
 *   - `comment` (string, optional) - default "".
 *   - `kind` (string, optional) - default "LABEL". Echo-only, reálné
 *     sym_db ukládá vždy SYM_SOURCE_LBL.
 *
 * Volá `DBGAPI_CMD_SYMBOL_ADD` -> sym_db_add_user_label.
 *
 * Response payload (success):
 *   - `addr` (int) - echo
 *   - `name` (string) - echo
 *   - `kind` (string) - echo přijatého kind (= dnes vždy "LABEL")
 *   - `added` (bool true)
 */
static en_MCP_DISPATCH_RESULT _handle_symbol_add(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required fields: addr, name",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 addr_raw = _obj_int_or(data_obj, "addr", -1);
    if (addr_raw < 0 || addr_raw > 0xFFFF) {
        return _err_response(req_id, "addr must be 0..65535",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *name = _obj_str_dup(data_obj, "name");
    if (!_is_valid_symbol_name(name)) {
        g_free(name);
        return _err_response(req_id,
            "name must be non-empty and contain only [A-Za-z0-9_.]",
            MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *comment = _obj_str_dup(data_obj, "comment");
    char *kind    = _obj_str_dup(data_obj, "kind");
    /* Echo-only kind default "LABEL" pokud nezadáno. */
    const char *kind_echo = (kind && kind[0]) ? kind : "LABEL";

    st_DBGAPI_SYMBOL_PARAM param = {
        .addr        = (uint16_t)addr_raw,
        .name        = name,
        .comment     = comment,
        .prefix      = NULL,
        .out_entries = NULL,
        .out_max     = 0,
        .out_count   = 0,
        .source      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SYMBOL_ADD, &param, NULL);
    if (!ok) {
        g_free(name);
        g_free(comment);
        g_free(kind);
        return _err_response(req_id, "symbol_add failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr", (gint64)(uint16_t)addr_raw);
    json_object_set_string_member(resp, "name", name);
    json_object_set_string_member(resp, "kind", kind_echo);
    json_object_set_boolean_member(resp, "added", TRUE);
    g_free(name);
    g_free(comment);
    g_free(kind);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `symbol_remove` handler - odebere symbol podle jména nebo adresy.
 *
 * Parametry (právě jeden):
 *   - `name` (string) - remove by name.
 *   - `addr` (int)    - remove by addr (0..65535).
 *
 * Volá `DBGAPI_CMD_SYMBOL_REMOVE`. Reálné sym_db remove podporuje jen
 * by-name; pokud klient pošle addr, dbgapi handler nejdřív lookup_by_addr
 * a pak remove_user_label podle nalezeného jména.
 *
 * Response payload (success):
 *   - `removed` (bool) - true pokud sym_db return code == 0.
 *   - `name` (string) nebo `addr` (int) - echo identifikátoru.
 */
static en_MCP_DISPATCH_RESULT _handle_symbol_remove(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: name or addr",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *name = _obj_str_dup(data_obj, "name");
    /* Pokud klient explicitně neposlal addr, _obj_int_or vrátí default. */
    gint64 addr_raw = _obj_int_or(data_obj, "addr", -1);
    bool has_name = (name && name[0]);
    bool has_addr = (addr_raw >= 0 && addr_raw <= 0xFFFF);
    if (!has_name && !has_addr) {
        g_free(name);
        return _err_response(req_id, "either name or addr must be specified",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (has_name && has_addr) {
        g_free(name);
        return _err_response(req_id, "specify either name OR addr, not both",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_SYMBOL_PARAM param = {
        .addr        = has_addr ? (uint16_t)addr_raw : 0,
        .name        = has_name ? name : NULL,
        .comment     = NULL,
        .prefix      = NULL,
        .out_entries = NULL,
        .out_max     = 0,
        .out_count   = 0,
        .source      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SYMBOL_REMOVE, &param, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "removed", ok ? TRUE : FALSE);
    if (has_name) {
        json_object_set_string_member(resp, "name", name);
    } else {
        json_object_set_int_member(resp, "addr", (gint64)(uint16_t)addr_raw);
    }
    g_free(name);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `symbol_lookup` handler - read-only vyhledání symbolu.
 *
 * Parametr:
 *   - `query` (string, povinný) - auto-detekce:
 *       * hex (0x4242, 4242h, $4242) -> lookup by addr
 *       * jinak -> lookup by name
 *
 * Volá `DBGAPI_CMD_SYMBOL_LOOKUP` (out_max=1).
 *
 * Response payload (success):
 *   - `found` (bool)
 *   - `addr` (int), `name` (string), `comment` (string), `source` (int)
 *     - jen pokud found=true. Source: 0=SJASMPLUS, 1=NOI, 2=MAP, 3=LBL.
 */
static en_MCP_DISPATCH_RESULT _handle_symbol_lookup(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: query",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *query = _obj_str_dup(data_obj, "query");
    if (!query || !query[0]) {
        g_free(query);
        return _err_response(req_id, "Missing required field: query",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Auto-detekce hex vs name. */
    uint16_t addr_parsed = 0;
    bool is_hex = _parse_hex_addr(query, &addr_parsed);

    st_DBGAPI_SYMBOL_ENTRY entry = {0};
    st_DBGAPI_SYMBOL_PARAM param = {
        .addr        = addr_parsed,
        .name        = is_hex ? NULL : query,
        .comment     = NULL,
        .prefix      = NULL,
        .out_entries = &entry,
        .out_max     = 1,
        .out_count   = 0,
        .source      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SYMBOL_LOOKUP, &param, NULL);
    if (!ok) {
        g_free(query);
        /* Defensive cleanup pro případ, že handler částečně naplnil. */
        g_free(entry.name);
        g_free(entry.comment);
        return _err_response(req_id, "symbol_lookup failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    JsonObject *resp = json_object_new();
    if (param.out_count > 0) {
        json_object_set_boolean_member(resp, "found", TRUE);
        json_object_set_int_member(resp, "addr", (gint64)entry.addr);
        json_object_set_string_member(resp, "name",
            entry.name ? entry.name : "");
        json_object_set_string_member(resp, "comment",
            entry.comment ? entry.comment : "");
        json_object_set_int_member(resp, "source", (gint64)entry.source);
    } else {
        json_object_set_boolean_member(resp, "found", FALSE);
    }
    g_free(entry.name);
    g_free(entry.comment);
    g_free(query);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `symbol_list` handler - výpis symbolů s prefix filterem.
 *
 * Parametry:
 *   - `prefix` (string, optional) - filter name prefix (default "" = vše).
 *   - `limit`  (int, optional)    - max počet záznamů (1..1000, default 100).
 *
 * Volá `DBGAPI_CMD_SYMBOL_LIST` s out_max = limit.
 *
 * Response payload (success):
 *   - `count` (int)
 *   - `items` (array) - každá položka {addr, name, comment, source}
 */
static en_MCP_DISPATCH_RESULT _handle_symbol_list(const st_JSONL_MESSAGE *req,
                                                   char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    /* data_node může být NULL (= request bez data sekce). */
    char *prefix = NULL;
    gint64 limit = 100;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        prefix = _obj_str_dup(data_obj, "prefix");
        limit  = _obj_int_or(data_obj, "limit", 100);
    }
    if (limit < 1 || limit > 1000) {
        g_free(prefix);
        return _err_response(req_id, "limit must be 1..1000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Alokace fixní velikosti pole - max 1000 záznamů. */
    size_t cap = (size_t)limit;
    st_DBGAPI_SYMBOL_ENTRY *entries =
        g_new0(st_DBGAPI_SYMBOL_ENTRY, cap);
    st_DBGAPI_SYMBOL_PARAM param = {
        .addr        = 0,
        .name        = NULL,
        .comment     = NULL,
        .prefix      = (prefix && prefix[0]) ? prefix : NULL,
        .out_entries = entries,
        .out_max     = cap,
        .out_count   = 0,
        .source      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SYMBOL_LIST, &param, NULL);
    if (!ok) {
        for (size_t i = 0; i < cap; i++) {
            g_free(entries[i].name);
            g_free(entries[i].comment);
        }
        g_free(entries);
        g_free(prefix);
        return _err_response(req_id, "symbol_list failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", (gint64)param.out_count);
    JsonArray *arr = json_array_new();
    for (size_t i = 0; i < param.out_count; i++) {
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "addr", (gint64)entries[i].addr);
        json_object_set_string_member(item, "name",
            entries[i].name ? entries[i].name : "");
        json_object_set_string_member(item, "comment",
            entries[i].comment ? entries[i].comment : "");
        json_object_set_int_member(item, "source", (gint64)entries[i].source);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "items", arr);

    /* Uvolnit heap stringy z entries (vyplnil dbgapi handler). */
    for (size_t i = 0; i < cap; i++) {
        g_free(entries[i].name);
        g_free(entries[i].comment);
    }
    g_free(entries);
    g_free(prefix);
    return _ok_response(req_id, resp, out_response);
}


/* ------------------------------------------------------------------ */
/* V1.A.3 - step out + run_until_* Tools                              */
/* ------------------------------------------------------------------ */

/**
 * @brief `step_out` handler - run until RET z aktuální subroutine.
 *
 * Parametr v `data` (volitelný):
 *  - `max_cycles` (int, default 10000000) - informativní timeout v
 *    T-states. V V1.A.3 ne-enforced dbgapi handlerem (= temp BP nemá
 *    cycle cap), klient může pollovat get_state.
 *
 * Volá `DBGAPI_CMD_STEP_OUT` se st_DBGAPI_STEP_OUT_PARAM. Handler v
 * emu vláknu vyhledá top frame v shadow callstacku, získá return_addr
 * a nastaví temporary BP + run-to. Pokud callstack tracking neaktivní
 * (status=1), nebo callstack je prázdný (status=2), nebo emu už běží
 * (status=3) - handler vrátí success=false a status v payload.
 *
 * Response při úspěchu:
 *  - `return_addr` (int)  - cílová RET adresa (hex-friendly int)
 *  - `max_cycles`  (int)  - echo
 *  - `running`     (bool) - true (emu nastartoval temp BP + run)
 *
 * Response při chybě:
 *  - error message + status code v JSONL `error` poli (anglicky).
 */
static en_MCP_DISPATCH_RESULT _handle_step_out(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    gint64 max_cycles = 10000000;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        max_cycles = _obj_int_or(data_obj, "max_cycles", 10000000);
        if (max_cycles < 1) {
            return _err_response(req_id, "Invalid max_cycles",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    st_DBGAPI_STEP_OUT_PARAM param;
    memset(&param, 0, sizeof(param));
    param.max_cycles = (uint32_t)((max_cycles > 0xFFFFFFFFLL)
                                  ? 0xFFFFFFFFu : (uint32_t)max_cycles);
    bool ok = _submit_dbgapi(DBGAPI_CMD_STEP_OUT, &param, NULL);
    if (!ok) {
        const char *msg = "step_out failed";
        switch (param.status) {
            case 1: msg = "callstack tracking required for step_out"; break;
            case 2: msg = "callstack empty (already at top frame)"; break;
            case 3: msg = "emu running (pause first)"; break;
            case 4: msg = "callstack snapshot alloc failed"; break;
            default: break;
        }
        return _err_response(req_id, msg,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "return_addr", (gint64)param.return_addr);
    json_object_set_int_member(resp, "max_cycles",  max_cycles);
    json_object_set_boolean_member(resp, "running", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief Pomocný polling helper - načte aktuální raster + cycles snapshot.
 *
 * Wrapper přes `DBGAPI_CMD_GET_RASTER_POS`. Vrací false pokud submit
 * selže (= emu ending / queue full).
 */
static bool _get_raster_now(st_DBGAPI_RASTER_POS *out_raster) {
    return _submit_dbgapi(DBGAPI_CMD_GET_RASTER_POS, NULL, out_raster);
}


/**
 * @brief Heuristika - dosáhli jsme cílového raster bodu (line, col)?
 *
 * Pro `col == -1` srovnává jen scanline. Pro `col >= 0` srovnává
 * (frame, scanline, column_pixel) lexikograficky vzhledem ke startu
 * polling cyklu. Implementace: porovnáme aktuální (frame, line, col)
 * proti uloženému start framu + cíli. Předpokládáme že target line je
 * v aktuálním nebo příštím frame.
 *
 * Mezi-instrukční granularita Z80 = jednotky T-states ≈ jednotky
 * pixel clock ticků. Reálná přesnost ±10 GDG ticků.
 */
static bool _raster_reached(uint32_t start_frame,
                            const st_DBGAPI_RASTER_POS *now,
                            int target_line, int target_col)
{
    /* Pokud cíl je v aktuálním frame nebo dál: vyžadujeme aspoň
     * příchod do target_line. */
    if (now->frame_number == start_frame) {
        if ((int)now->scanline < target_line) return false;
        if ((int)now->scanline > target_line) return true;
        /* same line */
        if (target_col < 0) return true;
        return (int)now->column_pixel >= target_col;
    }
    /* Frame přetekl - jsme v dalším frame. Pokud target_line ještě
     * v tomto framu neprošel, čekáme dál. */
    if (now->frame_number > start_frame) {
        if ((int)now->scanline >= target_line) return true;
        /* už jsme v dalším frame, ale ještě před target line -
         * pokračujeme. */
        return false;
    }
    /* frame_number < start_frame: nemělo by se stát (counter je
     * monotonic). Bezpečnost - hlásíme dosaženo. */
    return true;
}


/**
 * @brief `run_until_raster` handler - polling loop přes STEP_INTO.
 *
 * Parametry v `data`:
 *  - `line` (int, 0..511) - cílová scanline
 *  - `col`  (int, -1..2047, default -1) - cílová raster column;
 *           -1 = libovolný sloupec na cílové scanline
 *  - `max_cycles` (int, default 10000000) - maximum Z80 T-states
 *           pro polling smyčku
 *
 * Implementace: získat startovní raster snapshot, opakovat
 * `DBGAPI_CMD_STEP_INTO` + `DBGAPI_CMD_GET_RASTER_POS` dokud
 * `_raster_reached` true, nebo dokud delta cycles >= max_cycles.
 *
 * Precision limit: Z80 instrukce mají 4..23 T-states, jeden T-state ≈
 * 2 pixel clock ticky. Reálná přesnost dosažení raster pozice je tedy
 * ±10 GDG ticků (= ±5 pixelů viditelné oblasti). Pro mid-frame
 * raster efekty (palette swap, scroll register write) dostatečné.
 *
 * Response:
 *  - `scanline`     (int)
 *  - `column_pixel` (int)
 *  - `frame_number` (int)
 *  - `total_cycles` (int)
 *  - `delta_cycles` (int) - kolik T-states polling spotřeboval
 *  - `reached`      (bool) - true = cíl dosažen, false = timeout
 */
static en_MCP_DISPATCH_RESULT _handle_run_until_raster(const st_JSONL_MESSAGE *req,
                                                        char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: line",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 line = _obj_int_or(data_obj, "line", -1);
    if (line < 0 || line > 511) {
        return _err_response(req_id, "Invalid line range (0..511)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 col = _obj_int_or(data_obj, "col", -1);
    if (col < -1 || col > 2047) {
        return _err_response(req_id, "Invalid col range (-1..2047)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 max_cycles = _obj_int_or(data_obj, "max_cycles", 10000000);
    if (max_cycles < 1) {
        return _err_response(req_id, "Invalid max_cycles",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Startovní snapshot pro detekci přetečení frame + cycle accounting. */
    st_DBGAPI_RASTER_POS start;
    memset(&start, 0, sizeof(start));
    if (!_get_raster_now(&start)) {
        return _err_response(req_id, "raster snapshot failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    /* Pokud již jsme na cílovém raster bodu, vrátíme okamžitě. */
    if (_raster_reached(start.frame_number, &start, (int)line, (int)col)) {
        JsonObject *resp = json_object_new();
        json_object_set_int_member(resp, "scanline",     (gint64)start.scanline);
        json_object_set_int_member(resp, "column_pixel", (gint64)start.column_pixel);
        json_object_set_int_member(resp, "frame_number", (gint64)start.frame_number);
        json_object_set_int_member(resp, "total_cycles", (gint64)start.total_cycles);
        json_object_set_int_member(resp, "delta_cycles", 0);
        json_object_set_boolean_member(resp, "reached",  TRUE);
        return _ok_response(req_id, resp, out_response);
    }

    /* Polling loop - STEP_INTO + raster check. */
    st_DBGAPI_RASTER_POS now = start;
    bool reached = false;
    uint32_t safety_iter = 0;
    const uint32_t SAFETY_MAX_ITER = 5000000;  /* anti-runaway hard cap */
    while (safety_iter < SAFETY_MAX_ITER) {
        if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL)) {
            /* emu nemůže krokovat (= ending / queue / not paused) */
            break;
        }
        if (!_get_raster_now(&now)) break;
        uint32_t delta = now.total_cycles - start.total_cycles;
        if (_raster_reached(start.frame_number, &now,
                            (int)line, (int)col)) {
            reached = true;
            break;
        }
        if (delta >= (uint32_t)max_cycles) break;
        safety_iter++;
    }

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "scanline",     (gint64)now.scanline);
    json_object_set_int_member(resp, "column_pixel", (gint64)now.column_pixel);
    json_object_set_int_member(resp, "frame_number", (gint64)now.frame_number);
    json_object_set_int_member(resp, "total_cycles", (gint64)now.total_cycles);
    json_object_set_int_member(resp, "delta_cycles",
                               (gint64)(now.total_cycles - start.total_cycles));
    json_object_set_boolean_member(resp, "reached", reached ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `run_until_tstate` handler - run do absolutního cycle counteru.
 *
 * Parametry v `data`:
 *  - `target_total_cycles` (int)   - absolutní target Z80 T-state counter
 *  - `max_cycles`          (int)   - bezpečnostní cap delta proti
 *                                    current_total_cycles (default 10M)
 *
 * Implementace: polling smyčka STEP_INTO + GET_RASTER_POS.total_cycles
 * dokud cycles >= target_total_cycles, nebo dokud delta >= max_cycles.
 * Pokud target_total_cycles <= current.total_cycles, vrací error
 * "target in past".
 *
 * Pozor: `total_cycles` v st_DBGAPI_RASTER_POS je uint32_t (= wraparound
 * každých ~20 minut při 3.5 MHz). Pro V1.A.3 ignorujeme overflow (=
 * typický klient cílí na delta v řádu sekund, kde overflow neprobíhá).
 *
 * Response:
 *  - `total_cycles` (int)  - dosažená cycles
 *  - `target`       (int)  - echo
 *  - `delta_cycles` (int)  - reálně utracené T-states
 *  - `reached`      (bool) - true pokud >= target
 */
static en_MCP_DISPATCH_RESULT _handle_run_until_tstate(const st_JSONL_MESSAGE *req,
                                                        char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: target_total_cycles",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 target = _obj_int_or(data_obj, "target_total_cycles", -1);
    if (target < 0 || target > 0xFFFFFFFFLL) {
        return _err_response(req_id,
                             "Invalid target_total_cycles (0..4294967295)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 max_cycles = _obj_int_or(data_obj, "max_cycles", 10000000);
    if (max_cycles < 1) {
        return _err_response(req_id, "Invalid max_cycles",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_RASTER_POS start;
    memset(&start, 0, sizeof(start));
    if (!_get_raster_now(&start)) {
        return _err_response(req_id, "raster snapshot failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if ((uint32_t)target <= start.total_cycles) {
        return _err_response(req_id,
                             "target_total_cycles in past "
                             "(must be > current total_cycles)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_RASTER_POS now = start;
    bool reached = false;
    uint32_t safety_iter = 0;
    const uint32_t SAFETY_MAX_ITER = 5000000;
    while (safety_iter < SAFETY_MAX_ITER) {
        if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL)) break;
        if (!_get_raster_now(&now)) break;
        if (now.total_cycles >= (uint32_t)target) {
            reached = true;
            break;
        }
        if ((now.total_cycles - start.total_cycles) >= (uint32_t)max_cycles) break;
        safety_iter++;
    }

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "total_cycles", (gint64)now.total_cycles);
    json_object_set_int_member(resp, "target",       target);
    json_object_set_int_member(resp, "delta_cycles",
                               (gint64)(now.total_cycles - start.total_cycles));
    json_object_set_boolean_member(resp, "reached", reached ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `run_until_event` handler - polling do daného event kindu.
 *
 * Parametry v `data`:
 *  - `kind`       (string) - "frame_done" | "breakpoint_hit" | "io_write"
 *  - `params`     (object, volitelný) - kind-specific:
 *      - frame_done:     `{count?: int}` (default 1) - N framů
 *      - breakpoint_hit: `{id?: int}` (V1.A.3 ignoruje id, čeká
 *                        na jakoukoliv pause - emu poběží, polling
 *                        sleduje is_running flag přes get_state)
 *      - io_write:       `{port: int}` - **V1.A.3 not implemented**
 *                        (vyžaduje eventlog IO category hookup,
 *                        plánováno V1.A.4)
 *  - `max_cycles` (int, default 10000000) - bezpečnostní cap
 *
 * Response:
 *  - `kind`         (string) - echo
 *  - `reached`      (bool)   - true pokud event detekován
 *  - `frames_done`  (int)    - pro frame_done: kolik framů uplynulo
 *  - `delta_cycles` (int)    - T-states spotřebované
 *  - `paused`       (bool)   - pro breakpoint_hit: emu je pausnutý
 */
static en_MCP_DISPATCH_RESULT _handle_run_until_event(const st_JSONL_MESSAGE *req,
                                                       char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: kind",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "kind")) {
        return _err_response(req_id, "Missing required field: kind",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *kind_node = json_object_get_member(data_obj, "kind");
    if (!kind_node || json_node_get_node_type(kind_node) != JSON_NODE_VALUE) {
        return _err_response(req_id, "Field 'kind' must be string",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *kind = json_node_get_string(kind_node);
    if (!kind) {
        return _err_response(req_id, "Field 'kind' must be string",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 max_cycles = _obj_int_or(data_obj, "max_cycles", 10000000);
    if (max_cycles < 1) {
        return _err_response(req_id, "Invalid max_cycles",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Extrahovat params sub-object (může chybět). */
    JsonObject *params_obj = NULL;
    if (json_object_has_member(data_obj, "params")) {
        JsonNode *pn = json_object_get_member(data_obj, "params");
        if (pn && json_node_get_node_type(pn) == JSON_NODE_OBJECT) {
            params_obj = json_node_get_object(pn);
        }
    }

    if (strcmp(kind, "io_write") == 0) {
        /* V1.A.3 nepodporujeme - vyžaduje EVENTLOG IO category subscribe. */
        return _err_response(req_id,
                             "io_write event requires eventlog hookup "
                             "(planned V1.A.4)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (strcmp(kind, "frame_done") == 0) {
        gint64 count = params_obj ? _obj_int_or(params_obj, "count", 1) : 1;
        if (count < 1 || count > 10000) {
            return _err_response(req_id,
                                 "Invalid frame count (1..10000)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        st_DBGAPI_RASTER_POS start;
        memset(&start, 0, sizeof(start));
        if (!_get_raster_now(&start)) {
            return _err_response(req_id, "raster snapshot failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        st_DBGAPI_RASTER_POS now = start;
        bool reached = false;
        uint32_t safety_iter = 0;
        const uint32_t SAFETY_MAX_ITER = 5000000;
        uint32_t target_frame = start.frame_number + (uint32_t)count;
        while (safety_iter < SAFETY_MAX_ITER) {
            if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL)) break;
            if (!_get_raster_now(&now)) break;
            if (now.frame_number >= target_frame) {
                reached = true;
                break;
            }
            if ((now.total_cycles - start.total_cycles)
                    >= (uint32_t)max_cycles) break;
            safety_iter++;
        }
        JsonObject *resp = json_object_new();
        json_object_set_string_member(resp, "kind", "frame_done");
        json_object_set_int_member(resp, "frames_done",
            (gint64)(now.frame_number - start.frame_number));
        json_object_set_int_member(resp, "delta_cycles",
            (gint64)(now.total_cycles - start.total_cycles));
        json_object_set_boolean_member(resp, "reached", reached ? TRUE : FALSE);
        return _ok_response(req_id, resp, out_response);
    }
    if (strcmp(kind, "breakpoint_hit") == 0) {
        /* V1.A.3: spustíme emu (run) a pollujeme is_running flag. Jakákoliv
         * pause (= hit BP, manual pause, ending) ukončí čekání. */
        st_DBGAPI_RASTER_POS start;
        memset(&start, 0, sizeof(start));
        if (!_get_raster_now(&start)) {
            return _err_response(req_id, "raster snapshot failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        /* Spustit emu. */
        if (!_submit_dbgapi(DBGAPI_CMD_RUN, NULL, NULL)) {
            return _err_response(req_id, "run failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        st_DBGAPI_RASTER_POS now = start;
        bool reached = false;
        uint32_t safety_iter = 0;
        const uint32_t SAFETY_MAX_ITER = 100000;  /* polling round-trip
                                                   * je drahý, méně iterací */
        while (safety_iter < SAFETY_MAX_ITER) {
            bool running = true;
            if (!_submit_dbgapi(DBGAPI_CMD_IS_RUNNING, NULL, &running)) break;
            if (!running) {
                reached = true;
                break;
            }
            if (!_get_raster_now(&now)) break;
            if ((now.total_cycles - start.total_cycles)
                    >= (uint32_t)max_cycles) {
                /* Timeout - zastavíme emu, aby klient měl deterministic stav. */
                _submit_dbgapi(DBGAPI_CMD_PAUSE, NULL, NULL);
                _get_raster_now(&now);
                break;
            }
            safety_iter++;
        }
        JsonObject *resp = json_object_new();
        json_object_set_string_member(resp, "kind", "breakpoint_hit");
        json_object_set_int_member(resp, "delta_cycles",
            (gint64)(now.total_cycles - start.total_cycles));
        json_object_set_boolean_member(resp, "reached", reached ? TRUE : FALSE);
        json_object_set_boolean_member(resp, "paused",  reached ? TRUE : FALSE);
        return _ok_response(req_id, resp, out_response);
    }

    /* Neznámý kind. */
    char errmsg[128];
    g_snprintf(errmsg, sizeof(errmsg),
               "Unsupported event kind: %s", kind);
    return _err_response(req_id, errmsg,
                         MCP_DISPATCH_INVALID_PARAMS, out_response);
}


/* ====================================================================== */
/* V1.A.4 - EVENT subscribe + TRAP forwarding Tools                         */
/* ====================================================================== */

/**
 * @brief Vytáhne pole stringů z `topics` membru data objektu.
 *
 * Vrací NULL-terminated pole g_strdup-ovaných stringů nebo NULL pokud
 * topics chybí / není array. Caller musí uvolnit přes g_strfreev.
 *
 * Tichá tolerance: ne-string prvky se přeskočí (= klient neposlal
 * platný topic), array je čisté.
 */
static char **_extract_topics(JsonObject *obj) {
    if (!obj || !json_object_has_member(obj, "topics")) return NULL;
    JsonNode *node = json_object_get_member(obj, "topics");
    if (!node || json_node_get_node_type(node) != JSON_NODE_ARRAY) return NULL;
    JsonArray *arr = json_node_get_array(node);
    if (!arr) return NULL;
    guint n = json_array_get_length(arr);
    char **out = g_new0(char *, (gsize)n + 1);
    guint cnt = 0;
    for (guint i = 0; i < n; i++) {
        JsonNode *el = json_array_get_element(arr, i);
        if (el && json_node_get_node_type(el) == JSON_NODE_VALUE) {
            const char *s = json_node_get_string(el);
            if (s && s[0]) {
                out[cnt++] = g_strdup(s);
            }
        }
    }
    out[cnt] = NULL;
    return out;
}


/**
 * @brief `event_subscribe` handler - subscribe na seznam topics.
 *
 * Parametry v `data`:
 *  - `topics` (array of string) - seznam topic jmen
 *
 * Response:
 *  - `subscribed` (array) - finální seznam topics po sloučení s
 *                            existujícími subscriptions
 *  - `topics_count` (int) - kolik topics se subscribovalo
 *
 * Conn_id = EVENT_BUS_CONN_PIPE (= 0) pro pipe transport. Budoucí TCP
 * multi-conn bude předávat per-connection ID.
 */
static en_MCP_DISPATCH_RESULT _handle_event_subscribe(const st_JSONL_MESSAGE *req,
                                                       char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        data = json_node_get_object(data_node);
    }

    char **topics = _extract_topics(data);
    if (!topics || !topics[0]) {
        if (topics) g_strfreev(topics);
        return _err_response(req_id,
                             "Missing or empty 'topics' array",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* V1.A.4: pipe transport má conn_id = 0. attach je idempotentní -
     * pokud byl už attached, no-op. */
    event_bus_attach(EVENT_BUS_CONN_PIPE);
    bool ok = event_bus_subscribe(EVENT_BUS_CONN_PIPE,
                                   (const char *const *)topics);

    JsonObject *resp = json_object_new();
    JsonArray *subs_arr = json_array_new();
    int cnt = 0;
    for (int i = 0; topics[i]; i++) {
        json_array_add_string_element(subs_arr, topics[i]);
        cnt++;
    }
    json_object_set_array_member(resp, "subscribed", subs_arr);
    json_object_set_int_member(resp, "topics_count", cnt);
    json_object_set_boolean_member(resp, "ok", ok ? TRUE : FALSE);

    g_strfreev(topics);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `event_unsubscribe` handler - unsubscribe topics.
 *
 * Parametry v `data`:
 *  - `topics` (array of string) - seznam topic jmen; pokud chybí nebo
 *                                  empty array, unsubscribe ALL
 *
 * Response:
 *  - `unsubscribed_all` (bool) - true pokud bylo všechno odhlášeno
 *  - `topics_count` (int) - kolik topics se reálně odhlásilo (informativní)
 */
static en_MCP_DISPATCH_RESULT _handle_event_unsubscribe(const st_JSONL_MESSAGE *req,
                                                         char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        data = json_node_get_object(data_node);
    }

    char **topics = _extract_topics(data);
    bool unsub_all = (topics == NULL) || (topics[0] == NULL);
    bool ok = event_bus_unsubscribe(EVENT_BUS_CONN_PIPE,
                                     (const char *const *)topics);

    int cnt = 0;
    if (topics) {
        for (int i = 0; topics[i]; i++) cnt++;
    }

    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "unsubscribed_all",
                                    unsub_all ? TRUE : FALSE);
    json_object_set_int_member(resp, "topics_count", cnt);
    json_object_set_boolean_member(resp, "ok", ok ? TRUE : FALSE);

    if (topics) g_strfreev(topics);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `event_poll` handler - vyzvedne pending eventy.
 *
 * Parametry v `data`:
 *  - `timeout_ms` (int, default 0) - max čekání na první event (0..60000)
 *  - `max_events` (int, default 10) - max počet vrácených eventů (1..100)
 *
 * Response:
 *  - `events` (array of event object) - každý event má
 *      { "topic": "...", "ts_us": <int>, "data": { ... } }
 *  - `count` (int) - počet vrácených eventů
 *  - `pending` (int) - kolik eventů ještě zůstalo v queue po polu
 *  - `dropped` (int) - akumulovaný drop counter (backpressure)
 */
static en_MCP_DISPATCH_RESULT _handle_event_poll(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        data = json_node_get_object(data_node);
    }

    gint64 timeout_ms = _obj_int_or(data, "timeout_ms", 0);
    gint64 max_events = _obj_int_or(data, "max_events", 10);
    if (timeout_ms < 0 || timeout_ms > 60000) {
        return _err_response(req_id,
                             "timeout_ms must be in range 0..60000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (max_events < 1 || max_events > 100) {
        return _err_response(req_id,
                             "max_events must be in range 1..100",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Auto-attach pokud klient pollne bez prior subscribe (= benign no-op
     * pokud už attached). Vrátí prázdný array. */
    event_bus_attach(EVENT_BUS_CONN_PIPE);
    JsonArray *events = event_bus_poll(EVENT_BUS_CONN_PIPE,
                                        (int)timeout_ms, (int)max_events);
    if (!events) events = json_array_new();
    int count = (int)json_array_get_length(events);
    int pending = event_bus_get_pending_count(EVENT_BUS_CONN_PIPE);
    int dropped = event_bus_get_dropped_count(EVENT_BUS_CONN_PIPE);

    JsonObject *resp = json_object_new();
    json_object_set_array_member(resp, "events", events);
    json_object_set_int_member(resp, "count", count);
    json_object_set_int_member(resp, "pending", pending < 0 ? 0 : pending);
    json_object_set_int_member(resp, "dropped", dropped < 0 ? 0 : dropped);

    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `trap_respond` handler - odpověz na TRAP (BP hit pause).
 *
 * Parametry v `data`:
 *  - `trap_id` (int) - ID z eventu `breakpoint_hit`
 *  - `action`  (string) - "continue" | "step_into" | "step_over" | "abort"
 *
 * Response:
 *  - `ok`         (bool)   - true pokud trap_id byl známý a CMD odeslán
 *  - `action`     (string) - akce která byla provedena
 *
 * Mapování action -> CMD:
 *  - continue   -> DBGAPI_CMD_RUN
 *  - step_into  -> DBGAPI_CMD_STEP_INTO
 *  - step_over  -> DBGAPI_CMD_STEP_OVER
 *  - abort      -> DBGAPI_CMD_RUN (V1.A.4 minimum; explicit abort = V1.A.5+)
 *
 * Pokud trap_id už není v pending mapě (= duplicitní respond nebo
 * mezitím vyřešen jinou cestou), handler vrátí ok=false ale CMD už
 * neodesílá - klient by mohl trefit non-paused emu.
 */
static en_MCP_DISPATCH_RESULT _handle_trap_respond(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        data = json_node_get_object(data_node);
    }

    gint64 trap_id = _obj_int_or(data, "trap_id", 0);
    if (trap_id <= 0) {
        return _err_response(req_id,
                             "Missing or invalid 'trap_id'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    const char *action_str = NULL;
    if (data && json_object_has_member(data, "action")) {
        JsonNode *an = json_object_get_member(data, "action");
        if (an && json_node_get_node_type(an) == JSON_NODE_VALUE) {
            action_str = json_node_get_string(an);
        }
    }
    en_TRAP_ACTION action = trap_manager_parse_action(action_str);
    if (action == TRAP_ACTION_INVALID) {
        return _err_response(req_id,
                             "Invalid 'action' "
                             "(continue|step_into|step_over|abort)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    bool consumed = trap_manager_consume(trap_id);
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "trap_id", trap_id);
    json_object_set_string_member(resp, "action",
                                   action_str ? action_str : "");
    json_object_set_boolean_member(resp, "ok", consumed ? TRUE : FALSE);

    if (!consumed) {
        /* Trap_id neznámý - neposíláme žádný CMD (= bezpečné default,
         * klient si může pollnout get_state a zkusit znovu). */
        return _ok_response(req_id, resp, out_response);
    }

    /* Mapování akce na dbgapi CMD. */
    bool emu_ok = false;
    switch (action) {
        case TRAP_ACTION_CONTINUE:
            emu_ok = _submit_dbgapi(DBGAPI_CMD_RUN, NULL, NULL);
            break;
        case TRAP_ACTION_STEP_INTO:
            emu_ok = _submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL);
            break;
        case TRAP_ACTION_STEP_OVER:
            emu_ok = _submit_dbgapi(DBGAPI_CMD_STEP_OVER, NULL, NULL);
            break;
        case TRAP_ACTION_ABORT:
            /* V1.A.4: abort mapuje na RUN (= V1.A.5+ může mapovat na
             * graceful shutdown). Klient ví, že emu pokračuje. */
            emu_ok = _submit_dbgapi(DBGAPI_CMD_RUN, NULL, NULL);
            break;
        default:
            emu_ok = false;
            break;
    }
    json_object_set_boolean_member(resp, "emu_cmd_ok",
                                    emu_ok ? TRUE : FALSE);

    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `shutdown` handler - signál transport vrstvě k ukončení procesu.
 *
 * Lokální handler (= žádné dbgapi volání). Sestaví success RESPONSE
 * `{"shutdown": true}` a teprve poté zavolá registrovaný callback,
 * který transport vrstvě (`main_pipe.c`) řekne, že má rozjet graceful
 * exit. Tím se zajistí, že klient ještě stihne přečíst odpověď před
 * tím, než server zavře stdout / vypne emu thread.
 *
 * Pokud žádný callback registrován není (= dispatch volaný mimo pipe
 * transport, např. unit testy), handler jen vrátí response a neudělá
 * nic dalšího.
 */
static en_MCP_DISPATCH_RESULT _handle_shutdown(const st_JSONL_MESSAGE *req,
                                               char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "shutdown", TRUE);
    en_MCP_DISPATCH_RESULT r = _ok_response(req_id, data, out_response);
    if (r == MCP_DISPATCH_OK && g_shutdown_cb != NULL) {
        g_shutdown_cb();
    }
    return r;
}


/* ------------------------------------------------------------------ */
/* V1.B.3 - hot-swap workflow handler                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief `emu_stop` handler - graceful shutdown emu pro hot-swap.
 *
 * Sémantický variant `shutdown` určený výhradně pro pipe transport
 * a hot-swap workflow (AI dev cyklus: stop -> rebuild -> start, bez
 * restartu Claude session).
 *
 * Rozdíl oproti `shutdown`:
 *   - Pokud transport != PIPE (= TCP attach k existující GUI), vrátí
 *     error a nezavolá shutdown callback. Důvod: TCP varianta by zabila
 *     živou GUI session uživatele, což je destruktivní.
 *   - Sémantika je orientována na Python wrapper (`mcp_server.py`),
 *     který drží subprocess handle child procesu a po `emu_stop`
 *     spawne nový mz800emu.exe přes `emu_start` (= Python-only Tool,
 *     bez C protějšku).
 *
 * Mechanika v PIPE módu: identická se `shutdown` (= zavolá registrovaný
 * shutdown callback `_on_shutdown_command` z `main_pipe.c`, který
 * signalizuje `sdlapp_quit()` a emu thread se ukončí). Hot-swap rozdíl
 * je čistě v Python wrapper lifecycle (= drží subprocess handle, spawn
 * znovu).
 *
 * Response success: `{"stopped": true, "transport": "pipe"}`
 * Response error (TCP): `{"error": "hot-swap requires pipe transport"}`
 * Response error (no callback): `{"error": "no shutdown callback registered"}`
 *
 * @par Side effects:
 *   V PIPE módu trigger graceful shutdown emu thread (= identicky s
 *   `shutdown` handlerem). Klient nesmí očekávat další responses po
 *   úspěšném `emu_stop`.
 *
 * @par Stav před voláním:
 *   AI klient by měl explicit `snapshot_save_buffer` pro zachování
 *   stavu - tento handler žádné auto-preservation neprovádí.
 */
static en_MCP_DISPATCH_RESULT _handle_emu_stop(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);

    /* Transport check - hot-swap zákaz v TCP módu. */
    en_MCP_DISPATCH_TRANSPORT kind = mcp_dispatch_get_transport_kind();
    if (kind != MCP_DISPATCH_TRANSPORT_PIPE) {
        return _err_response(req_id,
                             "hot-swap requires pipe transport",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Bez registrovaného shutdown callback by emu_stop nic neudělal -
     * to je v unit testu OK (= žádný main, žádný transport), ale za
     * běhu by to byla tichá chyba. Vrátíme error, aby klient věděl. */
    if (g_shutdown_cb == NULL) {
        return _err_response(req_id,
                             "no shutdown callback registered",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "stopped", TRUE);
    json_object_set_string_member(data, "transport", "pipe");
    en_MCP_DISPATCH_RESULT r = _ok_response(req_id, data, out_response);
    if (r == MCP_DISPATCH_OK) {
        /* Trigger shutdown callback identicky se `shutdown` handlerem.
         * main_pipe.c -> _on_shutdown_command -> _request_shutdown ->
         * sdlapp_quit + cond signal. Klient ještě stihne odpověď přečíst
         * (response je už sestavena před triggerem). */
        g_shutdown_cb();
    }
    return r;
}


/* ------------------------------------------------------------------ */
/* V1.A.5 - chip-level fault injection handlery                        */
/* ------------------------------------------------------------------ */


/**
 * @brief `io_read` handler - Z80 IN s plnými side-effecty.
 *
 * Parametr `data.port` (int, 0..65535). Handler forwarduje na
 * DBGAPI_CMD_IO_READ - emu vlákno zavolá `port_read_cb` (= reálná
 * cesta Z80 IN přes daný arch iorq dispatcher). Po návratu vyplní
 * `value` ve struktuře.
 *
 * Response: `{"port": addr, "value": <0..255>}`
 *
 * @par Side effects:
 *   Některé chipy mají read side effects (PSG status flag reset,
 *   FDC IDX strobe, GDG DMD register). MCP klient by si měl být
 *   vědom destruktivního dopadu (= viz tool description s WARNING).
 */
static en_MCP_DISPATCH_RESULT _handle_io_read(const st_JSONL_MESSAGE *req,
                                              char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 port = _obj_int_or(data_obj, "port", -1);
    if (port < 0 || port > 0xFFFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_IO_PARAM param = { .port = (uint16_t)port, .value = 0 };
    if (!_submit_dbgapi(DBGAPI_CMD_IO_READ, &param, NULL)) {
        return _err_response(req_id, "IO_READ failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "port",  (gint64)port);
    json_object_set_int_member(resp, "value", (gint64)param.value);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `io_write` handler - Z80 OUT s plnými side-effecty.
 *
 * Parametry `data.port` (0..65535) a `data.value` (0..255). Forward
 * na DBGAPI_CMD_IO_WRITE - emu vlákno zavolá `port_write_cb`. Chipy
 * reagují přesně jako by šlo o instrukci Z80 OUT (PSG latch, FDC
 * command, GDG mode, PIO output bity).
 *
 * Response: `{"port": addr, "value": <byte>}` (echo).
 *
 * @warning Destruktivní operace - může změnit stav banking, video,
 *          floppy, sound. AI klient si nechá odsouhlasit uživatelem
 *          při kritických chipech.
 */
static en_MCP_DISPATCH_RESULT _handle_io_write(const st_JSONL_MESSAGE *req,
                                               char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 port  = _obj_int_or(data_obj, "port",  -1);
    gint64 value = _obj_int_or(data_obj, "value", -1);
    if (port < 0 || port > 0xFFFF || value < 0 || value > 0xFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_IO_PARAM param = {
        .port  = (uint16_t)port,
        .value = (uint8_t)value,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_IO_WRITE, &param, NULL)) {
        return _err_response(req_id, "IO_WRITE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "port",  (gint64)port);
    json_object_set_int_member(resp, "value", (gint64)value);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `irq_inject` handler - force maskable IRQ.
 *
 * Parametry:
 *  - `source` (string, default "manual") - audit label, propaguje se
 *     do dbgapi struktury (zatím jen pro logging, ne enforce).
 *  - `vector` (int, -1 = bez vektoru, jinak 0..255) - explicit IM2
 *     vektor. -1 použije default intread_cb.
 *
 * Response: `{"injected": true, "source": "...", "vector_used": <int|null>}`.
 *
 * @warning Destruktivní - může změnit IFF1, PC, SP. Doporučeno paused
 *          stav před voláním. Skutečné přijetí IRQ závisí na EI/DI
 *          (= pokud IFF1=0, IRQ se zapamatuje do int_pending a vykoná
 *          se po nejbližším EI).
 */
static en_MCP_DISPATCH_RESULT _handle_irq_inject(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    const char *src = "manual";
    gint64 vector = -1;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        if (json_object_has_member(data_obj, "source")) {
            JsonNode *sn = json_object_get_member(data_obj, "source");
            if (sn && !json_node_is_null(sn) &&
                json_node_get_node_type(sn) == JSON_NODE_VALUE) {
                const char *v = json_node_get_string(sn);
                if (v) src = v;
            }
        }
        vector = _obj_int_or(data_obj, "vector", -1);
    }
    if (vector != -1 && (vector < 0 || vector > 0xFF)) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_IRQ_INJECT_PARAM param = {
        .source       = src,
        .vector       = (vector >= 0) ? (uint8_t)vector : 0,
        .vector_valid = (vector >= 0) ? 1 : 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_IRQ_INJECT, &param, NULL)) {
        return _err_response(req_id, "IRQ_INJECT failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "injected", TRUE);
    json_object_set_string_member(resp, "source", src);
    if (vector >= 0) {
        json_object_set_int_member(resp, "vector_used", vector);
    } else {
        json_object_set_null_member(resp, "vector_used");
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `nmi_inject` handler - force NMI.
 *
 * Bez parametrů. Forward na DBGAPI_CMD_NMI_INJECT - emu vlákno
 * zavolá `z80_nmi()`, čímž po dokončení aktuální instrukce CPU
 * skočí na 0x0066, IFF1 se uloží do IFF2 a IFF1=0.
 *
 * Response: `{"injected": true}`.
 *
 * @warning Destruktivní - vždy se přijme (NMI je nemaskovatelné).
 */
static en_MCP_DISPATCH_RESULT _handle_nmi_inject(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_NMI_INJECT, NULL, NULL)) {
        return _err_response(req_id, "NMI_INJECT failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "injected", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `mem_write_force` handler - raw memory write bez region check.
 *
 * Parametry: `data.addr` (0..65535), `data.data_hex` (hex string).
 * Identicky validuje vstup jako `mem_write`, ale forwarduje na
 * DBGAPI_CMD_MEM_WRITE_FORCE (= bez region check), takže zápis
 * proběhne i do ROM oblastí (pokud banking dovolí).
 *
 * Response: `{"addr": <int>, "length": <int>}`.
 *
 * @warning Velmi destruktivní - obchází ochranu ROM/VRAM zapisů.
 *          Doporučeno jen pro fault injection v test scenariu.
 */
static en_MCP_DISPATCH_RESULT _handle_mem_write_force(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 addr = _obj_int_or(data_obj, "addr", -1);
    if (addr < 0 || addr > 0xFFFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!json_object_has_member(data_obj, "data_hex")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *hex_node = json_object_get_member(data_obj, "data_hex");
    if (!hex_node || json_node_is_null(hex_node) ||
        json_node_get_node_type(hex_node) != JSON_NODE_VALUE) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const gchar *data_hex = json_node_get_string(hex_node);
    if (!data_hex || data_hex[0] == '\0') {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const gsize MAX_LEN = 0x10000;
    uint8_t *bytes = g_malloc(MAX_LEN);
    int decoded = _decode_hex(data_hex, bytes, MAX_LEN);
    if (decoded <= 0 || (addr + decoded) > 0x10000) {
        g_free(bytes);
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_MEM_PARAM param = {
        .addr = (uint16_t)addr,
        .len  = (uint16_t)decoded,
        .buf  = bytes,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEM_WRITE_FORCE, &param, NULL);
    g_free(bytes);
    if (!ok) {
        return _err_response(req_id, "MEM_WRITE_FORCE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr",   (gint64)addr);
    json_object_set_int_member(resp, "length", (gint64)decoded);
    return _ok_response(req_id, resp, out_response);
}


/* ====================================================================== */
/* V1.A.6 - Watch + Callstack + CDL Tools handlery                        */
/* ====================================================================== */

/* Sdílená tabulka mapování string -> en_DBGAPI_WATCH_TYPE.
 * Pořadí + enum hodnoty musí být shodné s en_DBGAPI_WATCH_TYPE
 * v dbgapi_cmdrq.h a en_WATCH_TYPE v watch.h. */
static const struct {
    const char *name;
    en_DBGAPI_WATCH_TYPE type;
} g_watch_type_names[] = {
    { "u8",      DBGAPI_WATCH_TYPE_U8 },
    { "i8",      DBGAPI_WATCH_TYPE_I8 },
    { "u16le",   DBGAPI_WATCH_TYPE_U16LE },
    { "u16be",   DBGAPI_WATCH_TYPE_U16BE },
    { "i16le",   DBGAPI_WATCH_TYPE_I16LE },
    { "i16be",   DBGAPI_WATCH_TYPE_I16BE },
    { "u32le",   DBGAPI_WATCH_TYPE_U32LE },
    { "u32be",   DBGAPI_WATCH_TYPE_U32BE },
    { "i32le",   DBGAPI_WATCH_TYPE_I32LE },
    { "i32be",   DBGAPI_WATCH_TYPE_I32BE },
    { "bit",     DBGAPI_WATCH_TYPE_BIT },
    { "ascii",   DBGAPI_WATCH_TYPE_ASCII },
    { "mzascii", DBGAPI_WATCH_TYPE_MZASCII },
    { "bytes",   DBGAPI_WATCH_TYPE_BYTES },
    { NULL,      DBGAPI_WATCH_TYPE_U8 },
};

/**
 * @brief Parse text typu na enum hodnotu.
 *
 * @param s    Vstup ("u8", "i16le", "bit", ...). NULL nebo neznámý = U8 default.
 * @param out  Výstup (vždy zapsáno).
 * @return true pokud bylo s rozpoznáno, false pro NULL / neznámý.
 */
static bool _parse_watch_type(const char *s, en_DBGAPI_WATCH_TYPE *out) {
    *out = DBGAPI_WATCH_TYPE_U8;
    if (!s || s[0] == '\0') return false;
    for (size_t i = 0; g_watch_type_names[i].name != NULL; i++) {
        if (strcmp(g_watch_type_names[i].name, s) == 0) {
            *out = g_watch_type_names[i].type;
            return true;
        }
    }
    return false;
}

/**
 * @brief Vrátí stabilní string reprezentaci typu.
 *
 * @param t  Enum hodnota.
 * @return Konstantní string (lifetime = program lifetime), "u8" pro
 *         neznámou hodnotu (= defenzivní fallback).
 */
static const char *_watch_type_to_str(en_DBGAPI_WATCH_TYPE t) {
    for (size_t i = 0; g_watch_type_names[i].name != NULL; i++) {
        if (g_watch_type_names[i].type == t) {
            return g_watch_type_names[i].name;
        }
    }
    return "u8";
}

/**
 * @brief Vrátí stabilní string reprezentaci watch módu.
 *
 * @param m  Enum hodnota.
 * @return "address" / "expr_scalar" / "expr_deref" (nebo "address" pro
 *         neznámou hodnotu).
 */
static const char *_watch_mode_to_str(en_DBGAPI_WATCH_MODE m) {
    switch (m) {
        case DBGAPI_WATCH_MODE_ADDRESS:     return "address";
        case DBGAPI_WATCH_MODE_EXPR_SCALAR: return "expr_scalar";
        case DBGAPI_WATCH_MODE_EXPR_DEREF:  return "expr_deref";
    }
    return "address";
}


/**
 * @brief `watch_add` handler - přidá watch řádek do storage.
 *
 * Parametry:
 *   - name (string, optional) - jméno řádku (NULL/prázdné = anonymní)
 *   - mode (string, optional) - "address" (default) / "expr_scalar" / "expr_deref"
 *   - addr (int, optional)    - 0..65535 (jen pro mode=address)
 *   - expr (string, optional) - výraz (povinný pro mode=expr_*)
 *   - type (string, optional) - "u8" (default) / "i8" / "u16le" / ...
 *
 * Response: `{"index": <int>, "name": ..., "mode": ..., "type": ...}`.
 */
static en_MCP_DISPATCH_RESULT _handle_watch_add(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    const char *name = NULL;
    if (json_object_has_member(data_obj, "name")) {
        JsonNode *n = json_object_get_member(data_obj, "name");
        if (n && !json_node_is_null(n) &&
            json_node_get_node_type(n) == JSON_NODE_VALUE) {
            name = json_node_get_string(n);
        }
    }
    const char *mode_str = "address";
    if (json_object_has_member(data_obj, "mode")) {
        JsonNode *n = json_object_get_member(data_obj, "mode");
        if (n && !json_node_is_null(n) &&
            json_node_get_node_type(n) == JSON_NODE_VALUE) {
            const char *v = json_node_get_string(n);
            if (v) mode_str = v;
        }
    }
    en_DBGAPI_WATCH_MODE mode;
    if (strcmp(mode_str, "address") == 0) {
        mode = DBGAPI_WATCH_MODE_ADDRESS;
    } else if (strcmp(mode_str, "expr_scalar") == 0) {
        mode = DBGAPI_WATCH_MODE_EXPR_SCALAR;
    } else if (strcmp(mode_str, "expr_deref") == 0) {
        mode = DBGAPI_WATCH_MODE_EXPR_DEREF;
    } else {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 addr = _obj_int_or(data_obj, "addr", 0);
    if (mode == DBGAPI_WATCH_MODE_ADDRESS &&
        (addr < 0 || addr > 0xFFFF)) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *expr_text = NULL;
    if (json_object_has_member(data_obj, "expr")) {
        JsonNode *n = json_object_get_member(data_obj, "expr");
        if (n && !json_node_is_null(n) &&
            json_node_get_node_type(n) == JSON_NODE_VALUE) {
            expr_text = json_node_get_string(n);
        }
    }
    if (mode != DBGAPI_WATCH_MODE_ADDRESS &&
        (!expr_text || expr_text[0] == '\0')) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    en_DBGAPI_WATCH_TYPE type = DBGAPI_WATCH_TYPE_U8;
    if (json_object_has_member(data_obj, "type")) {
        JsonNode *n = json_object_get_member(data_obj, "type");
        if (n && !json_node_is_null(n) &&
            json_node_get_node_type(n) == JSON_NODE_VALUE) {
            const char *v = json_node_get_string(n);
            if (v && !_parse_watch_type(v, &type)) {
                return _err_response(req_id, "Invalid parameters",
                                     MCP_DISPATCH_INVALID_PARAMS, out_response);
            }
        }
    }
    st_DBGAPI_WATCH_ADD_PARAM param = {
        .mode      = mode,
        .name      = name,
        .addr      = (uint16_t)addr,
        .expr_text = expr_text,
        .type      = type,
        .out_index = -1,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_WATCH_ADD, &param, NULL)) {
        return _err_response(req_id, "watch_add failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "index", (gint64)param.out_index);
    if (name) {
        json_object_set_string_member(resp, "name", name);
    } else {
        json_object_set_null_member(resp, "name");
    }
    json_object_set_string_member(resp, "mode", _watch_mode_to_str(mode));
    json_object_set_string_member(resp, "type", _watch_type_to_str(type));
    if (mode == DBGAPI_WATCH_MODE_ADDRESS) {
        json_object_set_int_member(resp, "addr", (gint64)addr);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `watch_remove` handler - odebere watch řádek.
 *
 * Parametry (jeden z):
 *   - name (string) - odstraní první řádek s tímto jménem
 *   - index (int) - odstraní řádek na daném indexu
 *
 * Response: `{"removed": <bool>, "index": <int>}`.
 */
static en_MCP_DISPATCH_RESULT _handle_watch_remove(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    const char *name = NULL;
    int idx = -1;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        if (json_object_has_member(data_obj, "name")) {
            JsonNode *n = json_object_get_member(data_obj, "name");
            if (n && !json_node_is_null(n) &&
                json_node_get_node_type(n) == JSON_NODE_VALUE) {
                name = json_node_get_string(n);
            }
        }
        idx = (int)_obj_int_or(data_obj, "index", -1);
    }
    if ((!name || name[0] == '\0') && idx < 0) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_WATCH_REMOVE_PARAM param = {
        .name = name,
        .index = idx,
        .out_removed = 0,
    };
    /* watch_remove handler vrací success=false pokud nenalezen - to není
     * dispatch error, jen "nenalezeno" => odpovíme s removed=false. */
    _submit_dbgapi(DBGAPI_CMD_WATCH_REMOVE, &param, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "removed",
                                    param.out_removed ? TRUE : FALSE);
    json_object_set_int_member(resp, "index", (gint64)param.index);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `watch_list` handler - vrátí seznam všech watchů.
 *
 * Parametry: žádné.
 * Response: `{"count": <int>, "items": [{"index", "name", "mode", "type",
 *           "addr", "expr", "value"}, ...]}`.
 *
 * Limit: 256 řádků (= praktický cap, watch storage je interaktivní).
 */
static en_MCP_DISPATCH_RESULT _handle_watch_list(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    const int MAX_ITEMS = 256;
    st_DBGAPI_WATCH_LIST_ENTRY *entries =
        g_new0(st_DBGAPI_WATCH_LIST_ENTRY, MAX_ITEMS);
    st_DBGAPI_WATCH_LIST_PARAM param = {
        .out_entries = entries,
        .out_max     = MAX_ITEMS,
        .out_count   = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_WATCH_LIST, &param, NULL)) {
        g_free(entries);
        return _err_response(req_id, "watch_list failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", (gint64)param.out_count);
    JsonArray *arr = json_array_new();
    for (int i = 0; i < param.out_count; i++) {
        st_DBGAPI_WATCH_LIST_ENTRY *e = &entries[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "index", (gint64)e->index);
        if (e->name) {
            json_object_set_string_member(item, "name", e->name);
        } else {
            json_object_set_null_member(item, "name");
        }
        json_object_set_string_member(item, "mode",
                                       _watch_mode_to_str(e->mode));
        json_object_set_string_member(item, "type",
                                       _watch_type_to_str(e->type));
        json_object_set_int_member(item, "addr", (gint64)e->addr);
        if (e->expr_text) {
            json_object_set_string_member(item, "expr", e->expr_text);
        } else {
            json_object_set_null_member(item, "expr");
        }
        json_object_set_string_member(item, "value",
                                       e->value_str ? e->value_str : "");
        json_array_add_object_element(arr, item);
        g_free(e->name);
        g_free(e->expr_text);
        g_free(e->value_str);
    }
    json_object_set_array_member(resp, "items", arr);
    g_free(entries);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `watch_eval` handler - vyhodnotí watch nebo ad-hoc výraz.
 *
 * Parametry (alespoň jeden z):
 *   - name (string) - eval existing watch by name
 *   - index (int) - eval existing watch by index
 *   - expr (string) - ad-hoc eval (parsuje + vyhodnotí výraz bez add)
 *
 * Pokud `expr` zadán, má přednost před name/index.
 *
 * Response: `{"value_str": "...", "value_int": <int>, "error": ... | null}`.
 */
static en_MCP_DISPATCH_RESULT _handle_watch_eval(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    const char *name = NULL;
    int idx = -1;
    const char *expr_text = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        if (json_object_has_member(data_obj, "name")) {
            JsonNode *n = json_object_get_member(data_obj, "name");
            if (n && !json_node_is_null(n) &&
                json_node_get_node_type(n) == JSON_NODE_VALUE) {
                name = json_node_get_string(n);
            }
        }
        idx = (int)_obj_int_or(data_obj, "index", -1);
        if (json_object_has_member(data_obj, "expr")) {
            JsonNode *n = json_object_get_member(data_obj, "expr");
            if (n && !json_node_is_null(n) &&
                json_node_get_node_type(n) == JSON_NODE_VALUE) {
                expr_text = json_node_get_string(n);
            }
        }
    }
    if ((!expr_text || expr_text[0] == '\0') &&
        (!name || name[0] == '\0') && idx < 0) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_WATCH_EVAL_PARAM param = {
        .name = name,
        .index = idx,
        .expr_text = expr_text,
        .out_value_int = 0,
        .out_value_str = NULL,
        .out_error = NULL,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_WATCH_EVAL, &param, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "value_str",
                                   param.out_value_str ? param.out_value_str : "");
    json_object_set_int_member(resp, "value_int",
                                (gint64)param.out_value_int);
    if (param.out_error) {
        json_object_set_string_member(resp, "error", param.out_error);
    } else {
        json_object_set_null_member(resp, "error");
    }
    g_free(param.out_value_str);
    g_free(param.out_error);
    if (!ok) {
        /* Eval selhal (parse error / watch not found). Pošleme přesto
         * success=true s `error` polem - klient si tak může chybu
         * číst strukturovaně. Pokud byl ad-hoc parse error, error
         * pole obsahuje krátký technický popis. */
    }
    return _ok_response(req_id, resp, out_response);
}


/* ---------------- Callstack Tool (1) ---------------- */

/**
 * @brief Sdílená kopie kind enum hodnot na string (sjednocené s callstack.h).
 *
 * @param kind  Hodnota cast na uint8_t.
 * @return Konstantní string label, "unknown" pro neznámou hodnotu.
 */
static const char *_cs_kind_to_str(uint8_t kind) {
    switch (kind) {
        case CS_KIND_CALL:      return "call";
        case CS_KIND_RST:       return "rst";
        case CS_KIND_IRQ_IM0:   return "irq_im0";
        case CS_KIND_IRQ_IM1:   return "irq_im1";
        case CS_KIND_IRQ_IM2:   return "irq_im2";
        case CS_KIND_NMI:       return "nmi";
        case CS_KIND_SYNTHETIC: return "synthetic";
    }
    return "unknown";
}

/**
 * @brief `callstack_get` handler - snapshot shadow stacku.
 *
 * Parametry:
 *   - max_depth (int, optional, 1..256, default 64) - omezení počtu frames
 *
 * Reuse existující DBGAPI_CMD_GET_CALLSTACK (V-1.3 + V1.A.3 implementace).
 *
 * Response: `{"active": bool, "count": int, "current_depth": int,
 *            "max_depth_reached": int, "cycles_now": int,
 *            "frames": [{"depth", "return_addr", "call_site_addr",
 *                        "target_addr", "kind", "cycles_at_entry",
 *                        "sp_at_entry"}, ...]}`.
 */
static en_MCP_DISPATCH_RESULT _handle_callstack_get(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    gint64 max_depth = 64;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        max_depth = _obj_int_or(data_obj, "max_depth", 64);
    }
    if (max_depth < 1 || max_depth > 256) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_CALLSTACK_GET_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_CALLSTACK, &param, NULL)) {
        if (param.entries) {
            callstack_snapshot_free((st_CALLSTACK_ENTRY *)param.entries);
        }
        return _err_response(req_id, "callstack_get failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active",
                                    param.active ? TRUE : FALSE);
    json_object_set_int_member(resp, "current_depth",
                                (gint64)param.current_depth);
    json_object_set_int_member(resp, "max_depth_reached",
                                (gint64)param.max_depth_reached);
    json_object_set_int_member(resp, "divergence_count",
                                (gint64)param.divergence_count);
    json_object_set_int_member(resp, "overflow_count",
                                (gint64)param.overflow_count);
    json_object_set_int_member(resp, "cycles_now",
                                (gint64)param.cycles_now);
    /* Limit frames podle max_depth - vezmeme top max_depth frames
     * (= entries[count-1] = top stacku, entries[0] = nejstarší). */
    int total = param.count;
    int emit = total;
    if (emit > (int)max_depth) emit = (int)max_depth;
    int start = total - emit;
    if (start < 0) start = 0;
    json_object_set_int_member(resp, "count", (gint64)emit);
    JsonArray *arr = json_array_new();
    st_CALLSTACK_ENTRY *entries = (st_CALLSTACK_ENTRY *)param.entries;
    for (int i = 0; i < emit; i++) {
        st_CALLSTACK_ENTRY *e = &entries[start + i];
        JsonObject *item = json_object_new();
        /* depth=0 = top stacku, takže emit-1 - i. */
        json_object_set_int_member(item, "depth", (gint64)(emit - 1 - i));
        json_object_set_int_member(item, "return_addr",
                                    (gint64)e->return_addr);
        json_object_set_int_member(item, "call_site_addr",
                                    (gint64)e->call_site_addr);
        json_object_set_int_member(item, "target_addr",
                                    (gint64)e->target_addr);
        json_object_set_int_member(item, "sp_at_entry",
                                    (gint64)e->sp_at_entry);
        json_object_set_int_member(item, "cycles_at_entry",
                                    (gint64)e->cycles_at_entry);
        json_object_set_string_member(item, "kind",
                                       _cs_kind_to_str(e->kind));
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "frames", arr);
    if (entries) {
        callstack_snapshot_free(entries);
    }
    return _ok_response(req_id, resp, out_response);
}


/* ---------------- CDL Tools (4) ---------------- */

/**
 * @brief `cdl_start` handler - aktivuje Memory Heatmap recording.
 *
 * Bez parametrů. Forward na DBGAPI_CMD_CDL_START - mhmap_set_mode(ALWAYS)
 * triggeruje swap CPU callbacků (slow path s logging cb).
 *
 * Response: `{"started": true, "mode": "always"}`.
 */
static en_MCP_DISPATCH_RESULT _handle_cdl_start(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_CDL_START, NULL, NULL)) {
        return _err_response(req_id, "cdl_start failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "started", TRUE);
    json_object_set_string_member(resp, "mode", "always");
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `cdl_stop` handler - vypne Memory Heatmap recording.
 *
 * Bez parametrů. Forward na DBGAPI_CMD_CDL_STOP. Data zůstávají
 * zachovaná v g_mhmap - klient může před stop volat `cdl_export`
 * nebo později `cdl_reset`.
 *
 * Response: `{"stopped": true, "mode": "off"}`.
 */
static en_MCP_DISPATCH_RESULT _handle_cdl_stop(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_CDL_STOP, NULL, NULL)) {
        return _err_response(req_id, "cdl_stop failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "stopped", TRUE);
    json_object_set_string_member(resp, "mode", "off");
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `cdl_reset` handler - vynuluje CDL countery.
 *
 * Bez parametrů. Forward na DBGAPI_CMD_CDL_RESET - mhmap_reset().
 * Recording mode se nemění - pokud byl ALWAYS, zůstane ALWAYS a
 * sběr pokračuje od 0.
 *
 * Response: `{"reset": true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_cdl_reset(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_CDL_RESET, NULL, NULL)) {
        return _err_response(req_id, "cdl_reset failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "reset", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `cdl_export` handler - exportuje CDL data do souborů.
 *
 * Parametry:
 *   - path (string) - cílová cesta k meta JSON souboru. Region soubory
 *                     (`*_bus.cdl`, `*_ram.cdl`, ...) se vytvoří v parent
 *                     adresáři s prefixem odvozeným z basename.
 *
 * Pokud parent adresář neexistuje, vytvoří se přes g_mkdir_with_parents.
 *
 * Response: `{"path": "...", "region_count": <int>}`.
 */
static en_MCP_DISPATCH_RESULT _handle_cdl_export(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "path")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *pn = json_object_get_member(data_obj, "path");
    if (!pn || json_node_is_null(pn) ||
        json_node_get_node_type(pn) != JSON_NODE_VALUE) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *path = json_node_get_string(pn);
    if (!path || path[0] == '\0') {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_CDL_EXPORT_PARAM param = {
        .meta_path = path,
        .out_result = -1,
        .out_region_count = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_CDL_EXPORT, &param, NULL)) {
        return _err_response(req_id, "cdl_export failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "path", path);
    json_object_set_int_member(resp, "region_count",
                                (gint64)param.out_region_count);
    return _ok_response(req_id, resp, out_response);
}


/* ---------------- Tracking lifecycle (0017 FÁZE 1) ---------------- */

/**
 * @brief Převést channel string na en_DBGAPI_TRACE_CHANNEL.
 *
 * Akceptované hodnoty: "cputrack", "iorqlog", "intlog", "hwlog".
 *
 * @param s    Channel string (může být NULL = nevalidní).
 * @param out  Výstupní enum (vyplněn jen při návratu TRUE).
 * @return TRUE při platném kanálu, FALSE jinak.
 */
static gboolean _trace_parse_channel(const char *s,
                                     en_DBGAPI_TRACE_CHANNEL *out) {
    if (!s) {
        return FALSE;
    }
    if (g_strcmp0(s, "cputrack") == 0) {
        *out = DBGAPI_TRACE_CHANNEL_CPUTRACK;
        return TRUE;
    }
    if (g_strcmp0(s, "iorqlog") == 0) {
        *out = DBGAPI_TRACE_CHANNEL_IORQLOG;
        return TRUE;
    }
    if (g_strcmp0(s, "intlog") == 0) {
        *out = DBGAPI_TRACE_CHANNEL_INTLOG;
        return TRUE;
    }
    if (g_strcmp0(s, "hwlog") == 0) {
        *out = DBGAPI_TRACE_CHANNEL_HWLOG;
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Vytáhnout povinný channel string z data objektu requestu.
 *
 * @param req      MCP request.
 * @param out_ch   Výstupní kanál (vyplněn jen při návratu TRUE).
 * @return TRUE pokud byl nalezen platný "channel", FALSE jinak (volající
 *         vrátí INVALID_PARAMS).
 */
static gboolean _trace_get_channel(const st_JSONL_MESSAGE *req,
                                   en_DBGAPI_TRACE_CHANNEL *out_ch) {
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return FALSE;
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "channel")
        || !_obj_is_string(data_obj, "channel")) {
        return FALSE;
    }
    const char *ch = json_object_get_string_member(data_obj, "channel");
    return _trace_parse_channel(ch, out_ch);
}

/**
 * @brief Společný handler pro trace_start / trace_stop.
 *
 * Oba sdílí strukturu: vyžadují "channel", forwardují na příslušné DBGAPI
 * cmd s naplněným st_DBGAPI_TRACE_PARAM. Liší se jen cmd a tvar odpovědi.
 *
 * @param req         MCP request.
 * @param out_response Výstupní JSON odpověď.
 * @param cmd         DBGAPI_CMD_TRACE_START nebo DBGAPI_CMD_TRACE_STOP.
 * @param state_key   Klíč boolean stavu v odpovědi ("started"/"stopped").
 * @return Dispatch výsledek.
 */
static en_MCP_DISPATCH_RESULT _trace_start_stop_common(
        const st_JSONL_MESSAGE *req, char **out_response,
        en_DBGAPI_CMD cmd, const char *state_key) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    en_DBGAPI_TRACE_CHANNEL ch;
    if (!_trace_get_channel(req, &ch)) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_TRACE_PARAM param = { .channel = ch, .path = NULL,
                                    .out_result = -1 };
    if (!_submit_dbgapi(cmd, &param, NULL)) {
        return _err_response(req_id, "trace command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, state_key, TRUE);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `trace_start` handler - aktivuje trace recording kanálu (ALWAYS).
 *
 * Parametry: `channel` (cputrack/iorqlog/intlog/hwlog). Forward na
 * DBGAPI_CMD_TRACE_START. Response: `{"started": true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_trace_start(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    return _trace_start_stop_common(req, out_response,
                                    DBGAPI_CMD_TRACE_START, "started");
}

/**
 * @brief `trace_stop` handler - vypne trace recording kanálu (OFF).
 *
 * Parametry: `channel`. Forward na DBGAPI_CMD_TRACE_STOP. Data segmentu se
 * uzavřou (flush+close). Response: `{"stopped": true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_trace_stop(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    return _trace_start_stop_common(req, out_response,
                                    DBGAPI_CMD_TRACE_STOP, "stopped");
}

/**
 * @brief `trace_reset` handler - vynuluje aktuální trace segment kanálu.
 *
 * Parametry: `channel`. Forward na DBGAPI_CMD_TRACE_RESET (uzavře+znovuotevře
 * segment, u cputrack i collapse reset). Response: `{"reset": true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_trace_reset(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    en_DBGAPI_TRACE_CHANNEL ch;
    if (!_trace_get_channel(req, &ch)) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_TRACE_PARAM param = { .channel = ch, .path = NULL,
                                    .out_result = -1 };
    if (!_submit_dbgapi(DBGAPI_CMD_TRACE_RESET, &param, NULL)) {
        return _err_response(req_id, "trace_reset failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "reset", TRUE);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `trace_save` handler - uloží/přesměruje trace segment kanálu.
 *
 * Parametry:
 *   - `channel` (string, povinný) - cputrack/iorqlog/intlog/hwlog.
 *   - `path` (string, volitelný) - cílová cesta NÁSLEDNÉHO segmentu. Bez
 *     `path` se jen uzavře a znovuotevře aktuální segment na stávající
 *     dir/name (= force save now).
 *
 * Forward na DBGAPI_CMD_TRACE_SAVE. Response: `{"saved": true, "path": <str|null>}`.
 */
static en_MCP_DISPATCH_RESULT _handle_trace_save(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    en_DBGAPI_TRACE_CHANNEL ch;
    if (!_trace_get_channel(req, &ch)) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* path je volitelný; pokud chybí nebo je null, posílá se NULL. */
    const char *path = NULL;
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data_obj = json_node_get_object(data_node);
    if (json_object_has_member(data_obj, "path")
        && _obj_is_string(data_obj, "path")) {
        const char *p = json_object_get_string_member(data_obj, "path");
        if (p && p[0] != '\0') {
            path = p;
        }
    }
    st_DBGAPI_TRACE_PARAM param = { .channel = ch, .path = path,
                                    .out_result = -1 };
    if (!_submit_dbgapi(DBGAPI_CMD_TRACE_SAVE, &param, NULL)) {
        return _err_response(req_id, "trace_save failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "saved", TRUE);
    if (path) {
        json_object_set_string_member(resp, "path", path);
    } else {
        json_object_set_null_member(resp, "path");
    }
    return _ok_response(req_id, resp, out_response);
}


/* ---------------- Profiler Tools (5, V1.A.7) ---------------- */

/**
 * @brief Pomocný překlad PROF kind enum hodnoty na krátký řetězec.
 *
 * Sjednocené s `prof_kind_to_str` v profiler.c a `_cs_kind_to_str` výše.
 * Drženo lokálně aby dispatch nemusel includovat profiler.h
 * (= dispatch test build nevidí mzarch_config.h).
 *
 * @param kind  Hodnota cast na uint8_t.
 * @return Konstantní string label ("unknown" pro neznámou hodnotu).
 */
static const char *_prof_kind_to_str(uint8_t kind) {
    /* Hodnoty z en_CALLSTACK_ENTRY_KIND (callstack.h - viditelné). */
    switch (kind) {
        case CS_KIND_CALL:      return "call";
        case CS_KIND_RST:       return "rst";
        case CS_KIND_IRQ_IM0:   return "irq_im0";
        case CS_KIND_IRQ_IM1:   return "irq_im1";
        case CS_KIND_IRQ_IM2:   return "irq_im2";
        case CS_KIND_NMI:       return "nmi";
        case CS_KIND_SYNTHETIC: return "synthetic";
    }
    return "unknown";
}


/**
 * @brief `profiler_start` handler - aktivuje per-function CPU profiler.
 *
 * Bez parametrů. Forward na DBGAPI_CMD_PROFILER_SET_ACTIVE s active=1.
 * profiler_set_active interně vynutí callstack_set_active(true) pokud
 * nebyl aktivní (= ownership tracking pro symetrický auto-off).
 *
 * Response: `{"active": true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_start(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PROFILER_SET_ACTIVE_PARAM param;
    memset(&param, 0, sizeof(param));
    param.active = 1;
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_SET_ACTIVE, &param, NULL)) {
        return _err_response(req_id, "profiler_start failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `profiler_stop` handler - vypne per-function CPU profiler.
 *
 * Bez parametrů. Forward na DBGAPI_CMD_PROFILER_SET_ACTIVE s active=0.
 * Data v agregátoru zůstávají zachovaná (= klient může pak volat
 * `profiler_get` / `profiler_export` před resetem).
 *
 * Response: `{"active": false}`.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_stop(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PROFILER_SET_ACTIVE_PARAM param;
    memset(&param, 0, sizeof(param));
    param.active = 0;
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_SET_ACTIVE, &param, NULL)) {
        return _err_response(req_id, "profiler_stop failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active", FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `profiler_reset` handler - vynuluje agregátor profileru.
 *
 * Bez parametrů. Forward na DBGAPI_CMD_PROFILER_RESET - profiler_reset()
 * vynuluje hash mapu entries, globální countery i baseline cycles.
 * g_profiler_active se nemění (= pokud běží, dále sbírá nová data).
 *
 * Response: `{"reset": true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_reset(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_RESET, NULL, NULL)) {
        return _err_response(req_id, "profiler_reset failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "reset", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `profiler_export` handler - exportuje agregátor do souboru.
 *
 * Parametry:
 *   - path (string, povinný) - cílová cesta.
 *   - format (string, optional, default "csv") - "csv" nebo "json".
 *
 * Forward na DBGAPI_CMD_PROFILER_EXPORT. Handler v EMU vlákně volá
 * profiler_export_to_file (= snapshot + format-specific writer).
 *
 * Response: `{"path": "...", "format": "csv"|"json", "entry_count": N}`.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_export(const st_JSONL_MESSAGE *req,
                                                       char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "path")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *pn = json_object_get_member(data_obj, "path");
    if (!pn || json_node_is_null(pn) ||
        json_node_get_node_type(pn) != JSON_NODE_VALUE) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *path = json_node_get_string(pn);
    if (!path || path[0] == '\0') {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* format - default "csv" (= 0). */
    int format_int = 0;
    const char *format_str = "csv";
    if (json_object_has_member(data_obj, "format")) {
        JsonNode *fn = json_object_get_member(data_obj, "format");
        if (fn && !json_node_is_null(fn) &&
            json_node_get_node_type(fn) == JSON_NODE_VALUE) {
            const char *s = json_node_get_string(fn);
            if (s && s[0] != '\0') {
                if (strcmp(s, "csv") == 0) {
                    format_int = 0;
                    format_str = "csv";
                } else if (strcmp(s, "json") == 0) {
                    format_int = 1;
                    format_str = "json";
                } else {
                    return _err_response(req_id, "Invalid parameters",
                                         MCP_DISPATCH_INVALID_PARAMS,
                                         out_response);
                }
            }
        }
    }
    st_DBGAPI_PROFILER_EXPORT_PARAM param;
    memset(&param, 0, sizeof(param));
    param.filepath = path;
    param.format   = format_int;
    param.result   = -1;
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_EXPORT, &param, NULL)) {
        return _err_response(req_id, "profiler_export failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "path", path);
    json_object_set_string_member(resp, "format", format_str);
    json_object_set_int_member(resp, "entry_count",
                                (gint64)param.entry_count);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `profiler_get` handler - vrátí aktuální agregát inline.
 *
 * Parametry:
 *   - limit (int, optional, 1..1000, default 50) - max entries v
 *     response (sort by exclusive cycles descending).
 *
 * Forward na DBGAPI_CMD_GET_PROFILER. Snapshot pole entries (callee
 * alokované) se po sestavení JSONu uvolní přes g_free (= dispatch je
 * vlastník per kontrakt st_DBGAPI_PROFILER_GET_PARAM).
 *
 * Response:
 * ```
 * {
 *   "active": bool, "entry_count": int, "limit": int,
 *   "total_cycles_64": int, "total_calls": int, "irq_entries": int,
 *   "unmatched_returns": int, "max_depth_reached": int,
 *   "overflow_count": int,
 *   "entries": [ { "addr": int, "kind": "call"|..., "calls": int,
 *                  "excl_cycles": int, "incl_cycles": int,
 *                  "min_cycles": int, "max_cycles": int,
 *                  "avg_cycles": int }, ... ]
 * }
 * ```
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_get(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    gint64 limit = 50;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *data_obj = json_node_get_object(data_node);
        limit = _obj_int_or(data_obj, "limit", 50);
    }
    if (limit < 1 || limit > 1000) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_PROFILER_GET_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PROFILER, &param, NULL)) {
        if (param.entries) {
            g_free(param.entries);
        }
        return _err_response(req_id, "profiler_get failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active",
                                    param.active ? TRUE : FALSE);
    json_object_set_int_member(resp, "entry_count",
                                (gint64)param.entry_count);
    json_object_set_int_member(resp, "limit", limit);
    json_object_set_int_member(resp, "total_cycles_64",
                                (gint64)param.total_cycles_64);
    json_object_set_int_member(resp, "total_calls",
                                (gint64)param.total_calls);
    json_object_set_int_member(resp, "irq_entries",
                                (gint64)param.irq_entries);
    json_object_set_int_member(resp, "unmatched_returns",
                                (gint64)param.unmatched_returns);
    json_object_set_int_member(resp, "max_depth_reached",
                                (gint64)param.max_depth_reached);
    json_object_set_int_member(resp, "overflow_count",
                                (gint64)param.overflow_count);
    /* Iterace přes void* entries pole - cast na lokální mirror layout
     * (= synchronizovaný s st_PROF_ENTRY z profiler.h). Vybere prvních
     * `emit` entries (= limit nebo entry_count). Sort dělá až klient
     * (= V1 nevolá qsort kvůli předvídatelnému overhead - dataset bývá
     * 100-1000 entries, klient si seřadí podle libosti). */
    int total = param.entry_count;
    int emit = total;
    if (emit > (int)limit) emit = (int)limit;
    JsonArray *arr = json_array_new();
    const st_MCP_PROF_ENTRY_MIRROR *entries =
        (const st_MCP_PROF_ENTRY_MIRROR *)param.entries;
    for (int i = 0; i < emit; i++) {
        const st_MCP_PROF_ENTRY_MIRROR *e = &entries[i];
        uint64_t avg = e->calls ? (e->cycles_incl_sum / e->calls) : 0;
        uint64_t mn  = (e->cycles_incl_min == UINT64_MAX) ? 0
                                                           : e->cycles_incl_min;
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "addr",
                                    (gint64)e->target_addr);
        json_object_set_string_member(item, "kind",
                                       _prof_kind_to_str(e->kind));
        json_object_set_int_member(item, "calls", (gint64)e->calls);
        json_object_set_int_member(item, "excl_cycles",
                                    (gint64)e->cycles_excl_sum);
        json_object_set_int_member(item, "incl_cycles",
                                    (gint64)e->cycles_incl_sum);
        json_object_set_int_member(item, "min_cycles", (gint64)mn);
        json_object_set_int_member(item, "max_cycles",
                                    (gint64)e->cycles_incl_max);
        json_object_set_int_member(item, "avg_cycles", (gint64)avg);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "entries", arr);
    /* Uvolnění pole entries - caller (= dispatch) vlastní per kontrakt
     * DBGAPI_CMD_GET_PROFILER. g_free protože alokátor je g_malloc
     * (= profiler_snapshot_get). */
    if (param.entries) {
        g_free(param.entries);
    }
    return _ok_response(req_id, resp, out_response);
}


/* ==================================================================== */
/* V1.B.1 - Media Tools handlers                                         */
/* ==================================================================== */

/**
 * @brief Parsovat slot string z JSON do en_DBGAPI_MEDIA_SLOT.
 *
 * Whitelist validace - neznámá hodnota vrátí DBGAPI_MEDIA_SLOT_NONE.
 *
 * @param s Slot identifikátor ("cmt", "fdc0_fd0".."fdc0_fd3",
 *          "fdc1_fd0".."fdc1_fd3", "qd", "ide8").
 * @return Příslušná enum hodnota nebo NONE.
 */
static en_DBGAPI_MEDIA_SLOT _parse_media_slot(const char *s) {
    if (!s) return DBGAPI_MEDIA_SLOT_NONE;
    if (strcmp(s, "cmt") == 0)      return DBGAPI_MEDIA_SLOT_CMT;
    if (strcmp(s, "fdc0_fd0") == 0) return DBGAPI_MEDIA_SLOT_FDC0_FD0;
    if (strcmp(s, "fdc0_fd1") == 0) return DBGAPI_MEDIA_SLOT_FDC0_FD1;
    if (strcmp(s, "fdc0_fd2") == 0) return DBGAPI_MEDIA_SLOT_FDC0_FD2;
    if (strcmp(s, "fdc0_fd3") == 0) return DBGAPI_MEDIA_SLOT_FDC0_FD3;
    if (strcmp(s, "fdc1_fd0") == 0) return DBGAPI_MEDIA_SLOT_FDC1_FD0;
    if (strcmp(s, "fdc1_fd1") == 0) return DBGAPI_MEDIA_SLOT_FDC1_FD1;
    if (strcmp(s, "fdc1_fd2") == 0) return DBGAPI_MEDIA_SLOT_FDC1_FD2;
    if (strcmp(s, "fdc1_fd3") == 0) return DBGAPI_MEDIA_SLOT_FDC1_FD3;
    if (strcmp(s, "qd") == 0)       return DBGAPI_MEDIA_SLOT_QD;
    if (strcmp(s, "ide8") == 0)     return DBGAPI_MEDIA_SLOT_IDE8;
    return DBGAPI_MEDIA_SLOT_NONE;
}


/**
 * @brief Zpětný překlad slot enum na string pro response.
 *
 * @param s Slot enum.
 * @return Konstantní string ("cmt", "fdc0_fd0", ...). Pro NONE vrátí "".
 */
static const char *_media_slot_str(en_DBGAPI_MEDIA_SLOT s) {
    switch (s) {
        case DBGAPI_MEDIA_SLOT_CMT:      return "cmt";
        case DBGAPI_MEDIA_SLOT_FDC0_FD0: return "fdc0_fd0";
        case DBGAPI_MEDIA_SLOT_FDC0_FD1: return "fdc0_fd1";
        case DBGAPI_MEDIA_SLOT_FDC0_FD2: return "fdc0_fd2";
        case DBGAPI_MEDIA_SLOT_FDC0_FD3: return "fdc0_fd3";
        case DBGAPI_MEDIA_SLOT_FDC1_FD0: return "fdc1_fd0";
        case DBGAPI_MEDIA_SLOT_FDC1_FD1: return "fdc1_fd1";
        case DBGAPI_MEDIA_SLOT_FDC1_FD2: return "fdc1_fd2";
        case DBGAPI_MEDIA_SLOT_FDC1_FD3: return "fdc1_fd3";
        case DBGAPI_MEDIA_SLOT_QD:       return "qd";
        case DBGAPI_MEDIA_SLOT_IDE8:     return "ide8";
        default: return "";
    }
}


/**
 * @brief Dekódovat base64 do dočasného souboru a vrátit path.
 *
 * Pro buffer variantu MCP load - media subsystém nemá in-memory load API,
 * proto base64 dekódujeme do tmp souboru a předáme cestu dbgapi
 * handleru. Caller volat `g_unlink + g_free` po use.
 *
 * @param b64 Base64-encoded data.
 * @param[out] out_path Heap-alokovaná cesta (g_strdup); NULL při chybě.
 * @return TRUE = OK, FALSE = decode nebo I/O chyba.
 */
static bool _b64_to_tempfile(const char *b64, char **out_path) {
    *out_path = NULL;
    if (!b64 || b64[0] == '\0') return false;
    gsize decoded_len = 0;
    guchar *decoded = g_base64_decode(b64, &decoded_len);
    if (!decoded || decoded_len == 0) {
        if (decoded) g_free(decoded);
        return false;
    }
    GError *err = NULL;
    gchar *tmp_path = NULL;
    gint   fd = g_file_open_tmp("mcp-media-XXXXXX", &tmp_path, &err);
    if (fd < 0 || !tmp_path) {
        if (err) g_error_free(err);
        g_free(decoded);
        return false;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        g_unlink(tmp_path);
        g_free(tmp_path);
        g_free(decoded);
        return false;
    }
    size_t written = fwrite(decoded, 1, decoded_len, fp);
    fclose(fp);
    g_free(decoded);
    if (written != decoded_len) {
        g_unlink(tmp_path);
        g_free(tmp_path);
        return false;
    }
    *out_path = tmp_path; /* vlastnictví předáno callerovi */
    return true;
}


/**
 * @brief `media_load_mzf` - rychlý CMT-hack load MZF do RAM.
 *
 * Parametry (právě jeden):
 *   - `path` (string) - filesystem cesta k .mzf
 *   - `bytes_b64` (string) - inline base64 obsah .mzf
 *
 * Volá `DBGAPI_CMD_MEDIA_LOAD_MZF`. Handler vykoná plný dvoufázový load
 * (hlavička + post-header mapping + tělo) analogicky bootstrap.c, obchází
 * CMT play emulaci. Při selhání souboru/hlavičky (out_result=-2) nebo těla
 * (out_result=-3) vrací error, ne falešný ok.
 */
static en_MCP_DISPATCH_RESULT _handle_media_load_mzf(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id,
                             "Provide exactly one of: path, bytes_b64",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *path = _obj_str_dup(data_obj, "path");
    char *b64  = _obj_str_dup(data_obj, "bytes_b64");
    bool has_path = (path && path[0] != '\0');
    bool has_b64  = (b64  && b64[0]  != '\0');
    if (has_path == has_b64) {
        g_free(path); g_free(b64);
        return _err_response(req_id,
                             "Provide exactly one of: path, bytes_b64",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *tmp_path = NULL;
    if (has_b64) {
        if (!_b64_to_tempfile(b64, &tmp_path)) {
            g_free(path); g_free(b64);
            return _err_response(req_id, "Invalid base64 payload",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    const char *target = has_path ? path : tmp_path;

    st_DBGAPI_MEDIA_PARAM param = {
        .slot          = DBGAPI_MEDIA_SLOT_NONE,
        .filepath      = target,
        .load_addr     = 0,
        .read_only     = 0,
        .out_size      = 0,
        .out_load_addr = 0,
        .out_exec_addr = 0,
        .out_result    = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEDIA_LOAD_MZF, &param, NULL);
    if (tmp_path) {
        g_unlink(tmp_path);
        g_free(tmp_path);
    }
    if (!ok) {
        int rc = param.out_result;
        g_free(path); g_free(b64);
        /* rc: -2 = soubor/hlavička selhala, -3 = tělo selhalo, -1 = param */
        const char *msg = (rc == -2) ? "Cannot open MZF file or invalid header"
                        : (rc == -3) ? "MZF body load failed (checksum/IO)"
                                     : "media_load_mzf failed";
        return _err_response(req_id, msg,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "load_addr", (gint64)param.out_load_addr);
    json_object_set_int_member(resp, "exec_addr", (gint64)param.out_exec_addr);
    json_object_set_int_member(resp, "size", (gint64)param.out_size);
    json_object_set_int_member(resp, "result_code", param.out_result);
    g_free(path); g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `media_load_binary` - raw bytes z file do Z80 paměti na addr.
 *
 * Parametry (povinné):
 *   - `path` (string) - filesystem cesta k binárce
 *   - `addr` (int) - Z80 adresa 0..65535
 *
 * Volá `DBGAPI_CMD_MEDIA_LOAD_BINARY`. Bajt po bajtu zapisuje přes
 * debugger_memory_write_byte (banking-aware, žádný region check).
 *
 * WARNING (klient): destructivní operace - může přepsat ROM stínovou
 * RAM, video paměť, atd. v závislosti na aktuálním bankingu.
 */
static en_MCP_DISPATCH_RESULT _handle_media_load_binary(const st_JSONL_MESSAGE *req,
                                                         char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required fields: path, addr",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *path = _obj_str_dup(data_obj, "path");
    if (!path || path[0] == '\0') {
        g_free(path);
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 addr = _obj_int_or(data_obj, "addr", -1);
    if (addr < 0 || addr > 0xFFFF) {
        g_free(path);
        return _err_response(req_id, "Invalid addr (must be 0..65535)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_MEDIA_PARAM param = {
        .slot       = DBGAPI_MEDIA_SLOT_NONE,
        .filepath   = path,
        .load_addr  = (uint16_t)addr,
        .read_only  = 0,
        .out_size   = 0,
        .out_result = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEDIA_LOAD_BINARY, &param, NULL);
    if (!ok) {
        int rc = param.out_result;
        g_free(path);
        const char *msg = (rc == -2) ? "Cannot open file"
                                      : "media_load_binary failed";
        return _err_response(req_id, msg,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "addr", (gint64)addr);
    json_object_set_int_member(resp, "size", (gint64)param.out_size);
    json_object_set_int_member(resp, "result_code", param.out_result);
    g_free(path);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `media_insert` - vložit média do slotu.
 *
 * Parametry:
 *   - `slot` (string, povinný) - "cmt" | "fdc0_fd0".."fdc0_fd3" |
 *     "fdc1_fd0".."fdc1_fd3" | "qd" | "ide8"
 *   - `path` (string) NEBO `bytes_b64` (string) - právě jeden
 *   - `ro` (bool, optional) - R/O mount (informativní, závislé na slotu)
 *
 * Volá `DBGAPI_CMD_MEDIA_INSERT`. Slot whitelist validace probíhá zde
 * v dispatch vrstvě před vložením do dbgapi fronty.
 *
 * WARNING (klient): pokud je ve slotu jíž jiné médium, dojde k jeho
 * automatickému umount (= ekvivalent eject + insert).
 */
static en_MCP_DISPATCH_RESULT _handle_media_insert(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id,
                             "Missing required fields: slot, path|bytes_b64",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *slot_str = _obj_str_dup(data_obj, "slot");
    en_DBGAPI_MEDIA_SLOT slot = _parse_media_slot(slot_str);
    g_free(slot_str);
    if (slot == DBGAPI_MEDIA_SLOT_NONE) {
        return _err_response(req_id,
                             "Invalid slot (allowed: cmt, fdc0, fdc1, qd, ide8)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *path = _obj_str_dup(data_obj, "path");
    char *b64  = _obj_str_dup(data_obj, "bytes_b64");
    bool has_path = (path && path[0] != '\0');
    bool has_b64  = (b64  && b64[0]  != '\0');
    if (has_path == has_b64) {
        g_free(path); g_free(b64);
        return _err_response(req_id,
                             "Provide exactly one of: path, bytes_b64",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gboolean ro = FALSE;
    if (json_object_has_member(data_obj, "ro")) {
        ro = json_object_get_boolean_member(data_obj, "ro");
    }
    char *tmp_path = NULL;
    if (has_b64) {
        if (!_b64_to_tempfile(b64, &tmp_path)) {
            g_free(path); g_free(b64);
            return _err_response(req_id, "Invalid base64 payload",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    const char *target = has_path ? path : tmp_path;

    st_DBGAPI_MEDIA_PARAM param = {
        .slot       = slot,
        .filepath   = target,
        .load_addr  = 0,
        .read_only  = (uint8_t)(ro ? 1 : 0),
        .out_size   = 0,
        .out_result = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEDIA_INSERT, &param, NULL);
    int rc = param.out_result;
    if (tmp_path) {
        /* Pozor: pro slot kde subsystem drží file open (fdc, ide8) tmp
         * soubor po unlink může zůstat využitelný (POSIX), ale na
         * Windows MZ-800 emu drží handle, takže delete může selhat.
         * Pro V1.B.1 přijatelné - mount drží data, smazání je best-effort. */
        g_unlink(tmp_path);
        g_free(tmp_path);
    }
    if (!ok) {
        g_free(path); g_free(b64);
        const char *msg = (rc == -10)
            ? "Slot not available in this architecture build"
            : (rc == -11)
                ? "QD insert via path not yet supported (use settings_set in V1.B.2)"
                : "media_insert failed";
        return _err_response(req_id, msg,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "slot", _media_slot_str(slot));
    json_object_set_int_member(resp, "result_code", rc);
    g_free(path); g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `media_eject` - vyjmout obsah ze slotu.
 *
 * Parametry:
 *   - `slot` (string, povinný) - "cmt" | "fdc0_fd0".."fdc0_fd3" |
 *     "fdc1_fd0".."fdc1_fd3" | "qd" | "ide8"
 *
 * Volá `DBGAPI_CMD_MEDIA_EJECT`.
 */
static en_MCP_DISPATCH_RESULT _handle_media_eject(const st_JSONL_MESSAGE *req,
                                                   char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: slot",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *slot_str = _obj_str_dup(data_obj, "slot");
    en_DBGAPI_MEDIA_SLOT slot = _parse_media_slot(slot_str);
    g_free(slot_str);
    if (slot == DBGAPI_MEDIA_SLOT_NONE) {
        return _err_response(req_id,
                             "Invalid slot (allowed: cmt, fdc0, fdc1, qd, ide8)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_MEDIA_PARAM param = {
        .slot       = slot,
        .filepath   = NULL,
        .load_addr  = 0,
        .read_only  = 0,
        .out_size   = 0,
        .out_result = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEDIA_EJECT, &param, NULL);
    int rc = param.out_result;
    if (!ok) {
        const char *msg = (rc == -10)
            ? "Slot not available in this architecture build"
            : "media_eject failed";
        return _err_response(req_id, msg,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "slot", _media_slot_str(slot));
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `media_state` - snapshot stavu všech slotů.
 *
 * Žádné parametry. Response obsahuje pole `slots` s objekty
 * {slot, inserted, path, ro}. Slot není přítomen v arch sestavě =
 * inserted=false, path="".
 */
static en_MCP_DISPATCH_RESULT _handle_media_state(const st_JSONL_MESSAGE *req,
                                                   char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_MEDIA_STATE_PARAM param;
    memset(&param, 0, sizeof(param));
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEDIA_STATE, &param, NULL);
    if (!ok) {
        return _err_response(req_id, "media_state failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    JsonArray  *arr = json_array_new();
    for (int i = 0; i < param.count; i++) {
        const st_DBGAPI_MEDIA_SLOT_INFO *s = &param.slots[i];
        JsonObject *item = json_object_new();
        json_object_set_string_member(item, "slot", _media_slot_str(s->slot));
        json_object_set_boolean_member(item, "inserted", s->inserted ? TRUE : FALSE);
        json_object_set_string_member(item, "path",
                                       s->filepath[0] ? s->filepath : "");
        json_object_set_boolean_member(item, "ro", s->read_only ? TRUE : FALSE);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "slots", arr);
    json_object_set_int_member(resp, "count", (gint64)param.count);
    return _ok_response(req_id, resp, out_response);
}


/* ====================================================================
 * V1.B.2 - Platform + Config Tools (settings, platform, periph)
 * ==================================================================== */

/**
 * @brief Whitelist live-settable INI klíčů.
 *
 * Klíče jsou ve formátu "MODULE/element" (case sensitive). Boot-time
 * klíče (= paths, toolchain, snapshot defaults) zde NEJSOU - jejich
 * změna by vyžadovala restart emulátoru. Tento whitelist je
 * konzervativní - V1.B.2 vystavuje pouze položky, kde je aplikace
 * okamžitá nebo nevyžaduje re-init periferie.
 *
 * @note Klíče platí napříč architekturami; pokud konkrétní arch modul
 * nemá, settings_set vrátí -2 (= module not found) z dbgapi vrstvy.
 */
static const char *const g_settings_live_keys[] = {
    /* AUDIO - volume per chip */
    "AUDIO/volume_8253",
    "AUDIO/volume_psg0",
    "AUDIO/volume_psg1",
    "AUDIO/volume_psg2",
    "AUDIO/volume_psg3",
    "AUDIO/volume_psg1_0",
    "AUDIO/volume_psg1_1",
    "AUDIO/volume_psg1_2",
    "AUDIO/volume_psg1_3",
    /* DISPLAY - runtime tweaks */
    "DISPLAY/forced_full_screen_redrawing",
    "DISPLAY/locked_window_aspect_ratio",
    "DISPLAY/custom_fps",
    /* QDISK - path setting (V1.B.1 blocker pro QD insert) */
    "QDISK/filename",
    "QDISK/write_protected",
    /* TRACE_CPUTRACK - range-scope filtr PC (live-apply do g_cputrack_config
     * v dbgapi SETTINGS_SET handleru; hot-path čte nové meze bez restartu). */
    "TRACE_CPUTRACK/pc_range_lo",
    "TRACE_CPUTRACK/pc_range_hi",
    NULL,
};


/**
 * @brief Whitelist klíčů pro settings_get.
 *
 * Pro čtení je whitelist širší - boot-time hodnoty smí číst, jen
 * nesmí být měněny live. NULL = libovolný klíč existující v cfgmain
 * je čitelný. V1.B.2 záměrně NULL (= open read), aby AI mohla
 * inspect celou konfiguraci.
 */
static const char *const *g_settings_read_keys = NULL;


/**
 * @brief Vrátí true pokud "MODULE/element" klíč je v live-settable
 *        whitelistu.
 */
static bool _settings_key_is_live(const char *key) {
    if (!key) return false;
    for (size_t i = 0; g_settings_live_keys[i] != NULL; i++) {
        if (strcmp(g_settings_live_keys[i], key) == 0) return true;
    }
    return false;
}


/**
 * @brief Rozdělí "MODULE/element" klíč na dva komponenty.
 *
 * Caller musí uvolnit *out_module a *out_element přes g_free pokud
 * funkce vrátí true. Při false jsou oba NULL.
 */
static bool _settings_split_key(const char *key,
                                 char **out_module,
                                 char **out_element) {
    if (!key || !out_module || !out_element) return false;
    *out_module = NULL;
    *out_element = NULL;
    const char *slash = strchr(key, '/');
    if (!slash || slash == key || slash[1] == '\0') return false;
    *out_module  = g_strndup(key, (size_t)(slash - key));
    *out_element = g_strdup(slash + 1);
    return true;
}


/**
 * @brief Vrátí string reprezentaci en_DBGAPI_SETTINGS_TYPE pro response.
 */
static const char *_settings_type_str(en_DBGAPI_SETTINGS_TYPE t) {
    switch (t) {
        case DBGAPI_SETTINGS_TYPE_UNSIGNED: return "unsigned";
        case DBGAPI_SETTINGS_TYPE_BOOL:     return "bool";
        case DBGAPI_SETTINGS_TYPE_TEXT:     return "text";
        case DBGAPI_SETTINGS_TYPE_KEYWORD:  return "keyword";
        case DBGAPI_SETTINGS_TYPE_FLOAT:    return "float";
        default:                            return "unknown";
    }
}


/**
 * @brief `settings_get` handler - přečte INI klíč.
 *
 * Parametry:
 *   - `key` (string, povinný) - klíč ve tvaru "MODULE/element".
 *
 * Volá `DBGAPI_CMD_SETTINGS_GET`. Response:
 *   {"key": str, "value": str, "type": "unsigned|bool|text|keyword|float"}
 *
 * Chyby:
 *   - Missing key, invalid format - INVALID_PARAMS
 *   - Klíč neexistuje (module nebo element) - EMU_ERROR s detail msg
 */
static en_MCP_DISPATCH_RESULT _handle_settings_get(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: key",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *key = _obj_str_dup(data_obj, "key");
    if (!key || !key[0]) {
        g_free(key);
        return _err_response(req_id, "Missing required field: key",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* Volitelný read whitelist (V1.B.2 = NULL = open read). */
    if (g_settings_read_keys) {
        bool found = false;
        for (size_t i = 0; g_settings_read_keys[i] != NULL; i++) {
            if (strcmp(g_settings_read_keys[i], key) == 0) {
                found = true; break;
            }
        }
        if (!found) {
            g_free(key);
            return _err_response(req_id,
                                  "Key not in read whitelist",
                                  MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    char *mod = NULL, *elm = NULL;
    if (!_settings_split_key(key, &mod, &elm)) {
        g_free(key);
        return _err_response(req_id,
                              "Invalid key format (expected 'MODULE/element')",
                              MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_SETTINGS_PARAM param = {
        .module     = mod,
        .element    = elm,
        .new_value  = NULL,
        .out_value  = NULL,
        .out_type   = DBGAPI_SETTINGS_TYPE_UNKNOWN,
        .out_result = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SETTINGS_GET, &param, NULL);
    int rc = param.out_result;
    if (!ok) {
        const char *msg = (rc == -2) ? "Module not found"
                       : (rc == -3) ? "Element not found in module"
                       : "settings_get failed";
        g_free(key); g_free(mod); g_free(elm);
        g_free(param.out_value);
        return _err_response(req_id, msg,
                              MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "key", key);
    json_object_set_string_member(resp, "value",
                                   param.out_value ? param.out_value : "");
    json_object_set_string_member(resp, "type",
                                   _settings_type_str(param.out_type));
    g_free(key); g_free(mod); g_free(elm);
    g_free(param.out_value);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `settings_set` handler - zapíše INI klíč.
 *
 * Parametry:
 *   - `key` (string, povinný) - klíč ve tvaru "MODULE/element".
 *   - `value` (string, povinný) - nová hodnota (string, type-coerce
 *     v dbgapi handleru dle typu elementu).
 *
 * Volá `DBGAPI_CMD_SETTINGS_SET`. Před zápisem ověří whitelist
 * live-settable klíčů. Response:
 *   {"key": str, "previous_value": str, "new_value": str, "type": str}
 *
 * Chyby:
 *   - Klíč není live-settable - INVALID_PARAMS
 *   - Module/element neexistuje - EMU_ERROR
 *   - Type-coerce selhal - EMU_ERROR ("Invalid value for type")
 */
static en_MCP_DISPATCH_RESULT _handle_settings_set(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required fields: key, value",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *key   = _obj_str_dup(data_obj, "key");
    char *value = _obj_str_dup(data_obj, "value");
    if (!key || !key[0] || !value) {
        g_free(key); g_free(value);
        return _err_response(req_id, "Missing required fields: key, value",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!_settings_key_is_live(key)) {
        g_free(key); g_free(value);
        return _err_response(req_id,
                              "Key is not live-settable (boot-time keys "
                              "require emulator restart)",
                              MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* Range-scope filtr cputrack: PC je 16bitový adresový prostor, hodnota
     * musí ležet v 0..0xFFFF. Ořez v jádře je sice tichý (uint16_t pole),
     * ale uživateli vracíme explicitní chybu místo nečekaného oříznutí. */
    if (strcmp(key, "TRACE_CPUTRACK/pc_range_lo") == 0 ||
        strcmp(key, "TRACE_CPUTRACK/pc_range_hi") == 0) {
        char *endp = NULL;
        unsigned long pv = strtoul(value, &endp, 0);
        if (!endp || *endp != '\0' || pv > 0xFFFFul) {
            g_free(key); g_free(value);
            return _err_response(req_id,
                                  "Value out of range (expected 0..65535)",
                                  MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    char *mod = NULL, *elm = NULL;
    if (!_settings_split_key(key, &mod, &elm)) {
        g_free(key); g_free(value);
        return _err_response(req_id,
                              "Invalid key format (expected 'MODULE/element')",
                              MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_SETTINGS_PARAM param = {
        .module     = mod,
        .element    = elm,
        .new_value  = value,
        .out_value  = NULL,
        .out_type   = DBGAPI_SETTINGS_TYPE_UNKNOWN,
        .out_result = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_SETTINGS_SET, &param, NULL);
    int rc = param.out_result;
    if (!ok) {
        const char *msg = (rc == -2) ? "Module not found"
                       : (rc == -3) ? "Element not found in module"
                       : (rc == -4) ? "Invalid value for type "
                                      "(coerce failed)"
                       : "settings_set failed";
        g_free(key); g_free(value); g_free(mod); g_free(elm);
        g_free(param.out_value);
        return _err_response(req_id, msg,
                              MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "key", key);
    json_object_set_string_member(resp, "previous_value",
                                   param.out_value ? param.out_value : "");
    json_object_set_string_member(resp, "new_value", value);
    json_object_set_string_member(resp, "type",
                                   _settings_type_str(param.out_type));
    g_free(key); g_free(value); g_free(mod); g_free(elm);
    g_free(param.out_value);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `platform_set` handler - pokus o platform switch.
 *
 * V1.B.2 vrací error, runtime platform switch není podporován
 * (= mz800/mz700/mz1500 jsou separátní binárky per MZARCH).
 * Handler dotáže dbgapi vrstvu pouze pro out_active_kind (= aktuální
 * compile-time platforma).
 *
 * Parametry:
 *   - `kind` (string, povinný) - "mz700" | "mz800" | "mz1500"
 *   - `mode` (string, optional) - "native" | "compat" (jen mz800)
 *   - `save_snapshot` (string, optional) - path k .mzs (V1.B.2 ignore)
 *
 * Response při neshodě s active = error msg + active_kind.
 * Response při shodě (target == active) = ok, no-op.
 */
static en_MCP_DISPATCH_RESULT _handle_platform_set(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: kind",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *kind_str = _obj_str_dup(data_obj, "kind");
    if (!kind_str || !kind_str[0]) {
        g_free(kind_str);
        return _err_response(req_id, "Missing required field: kind",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    int kind = 0;
    if (strcmp(kind_str, "mz700") == 0)       kind = 1;
    else if (strcmp(kind_str, "mz800") == 0)  kind = 2;
    else if (strcmp(kind_str, "mz1500") == 0) kind = 3;
    if (kind == 0) {
        g_free(kind_str);
        return _err_response(req_id,
                              "Invalid kind (allowed: mz700, mz800, mz1500)",
                              MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_PLATFORM_PARAM param = {
        .target_kind     = kind,
        .out_active_kind = 0,
        .out_result      = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_PLATFORM_SET, &param, NULL);
    /* Vrátí string name aktivní platformy (kvůli AI klientovi). */
    const char *active_name =
        (param.out_active_kind == 1) ? "mz700" :
        (param.out_active_kind == 2) ? "mz800" :
        (param.out_active_kind == 3) ? "mz1500" : "unknown";
    if (!ok || param.out_result == -10) {
        JsonObject *resp = json_object_new();
        json_object_set_boolean_member(resp, "ok", FALSE);
        json_object_set_string_member(resp, "active_kind", active_name);
        json_object_set_string_member(resp, "target_kind", kind_str);
        json_object_set_string_member(resp, "error",
            "Runtime platform switch not supported - mz700/mz800/mz1500 "
            "are separate binaries (compile-time MZARCH). To use a "
            "different platform, restart with the corresponding "
            "executable (e.g. mz1500emu.exe).");
        g_free(kind_str);
        /* Použít _ok_response s ok=false aby AI klient dostal i payload,
         * ne jen error string. */
        return _ok_response(req_id, resp, out_response);
    }
    /* target == active - no-op success. */
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "active_kind", active_name);
    json_object_set_boolean_member(resp, "no_op", TRUE);
    g_free(kind_str);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief Mapuje string kind na en_DBGAPI_PERIPH_KIND.
 *
 * Vrací DBGAPI_PERIPH_KIND_UNKNOWN pro NULL / neznámou hodnotu.
 */
static en_DBGAPI_PERIPH_KIND _parse_periph_kind(const char *s) {
    if (!s) return DBGAPI_PERIPH_KIND_UNKNOWN;
    if (strcmp(s, "memext") == 0) return DBGAPI_PERIPH_KIND_MEMEXT;
    if (strcmp(s, "fdc")    == 0) return DBGAPI_PERIPH_KIND_FDC;
    if (strcmp(s, "qd")     == 0) return DBGAPI_PERIPH_KIND_QD;
    if (strcmp(s, "ide8")   == 0) return DBGAPI_PERIPH_KIND_IDE8;
    if (strcmp(s, "gal5")   == 0) return DBGAPI_PERIPH_KIND_GAL5;
    return DBGAPI_PERIPH_KIND_UNKNOWN;
}


/**
 * @brief Společný handler pro periph_attach + periph_detach.
 *
 * Volá `DBGAPI_CMD_PERIPH_ATTACH` nebo `_DETACH` podle is_attach.
 *
 * Pro attach navíc předá `options.type` (= např. "luftner4k" pro
 * memext) do dbgapi handleru, který ho zapíše do CFGMOD/type
 * elementu.
 *
 * Response:
 *   {"ok": bool, "kind": str, "requires_restart": bool, "result_code": int}
 */
static en_MCP_DISPATCH_RESULT _handle_periph_common(const st_JSONL_MESSAGE *req,
                                                     char **out_response,
                                                     bool is_attach) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: kind",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *kind_str = _obj_str_dup(data_obj, "kind");
    en_DBGAPI_PERIPH_KIND kind = _parse_periph_kind(kind_str);
    if (kind == DBGAPI_PERIPH_KIND_UNKNOWN) {
        g_free(kind_str);
        return _err_response(req_id,
                              "Invalid kind (allowed: memext, fdc, qd, "
                              "ide8, gal5)",
                              MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* options.type pro attach (= memext variant). */
    char *option_value = NULL;
    if (is_attach && json_object_has_member(data_obj, "options")) {
        JsonNode *opts_node = json_object_get_member(data_obj, "options");
        if (opts_node && json_node_get_node_type(opts_node) == JSON_NODE_OBJECT) {
            JsonObject *opts = json_node_get_object(opts_node);
            option_value = _obj_str_dup(opts, "type");
        }
    }
    st_DBGAPI_PERIPH_PARAM param = {
        .kind                 = kind,
        .option_value         = option_value,
        .out_requires_restart = 0,
        .out_result           = 0,
    };
    en_DBGAPI_CMD cmd = is_attach
        ? DBGAPI_CMD_PERIPH_ATTACH
        : DBGAPI_CMD_PERIPH_DETACH;
    bool ok = _submit_dbgapi(cmd, &param, NULL);
    int rc = param.out_result;
    if (!ok) {
        const char *msg = (rc == -10)
            ? "Peripheral not available in this architecture build"
            : (rc == -11)
                ? "Peripheral does not support runtime attach via INI"
                : (is_attach ? "periph_attach failed" : "periph_detach failed");
        g_free(kind_str); g_free(option_value);
        return _err_response(req_id, msg,
                              MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "kind", kind_str);
    json_object_set_boolean_member(resp, "requires_restart",
                                    param.out_requires_restart ? TRUE : FALSE);
    json_object_set_int_member(resp, "result_code", rc);
    g_free(kind_str); g_free(option_value);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `periph_attach` handler.
 *
 * Parametry:
 *   - `kind` (string, povinný) - "memext" | "fdc" | "qd" | "ide8" | "gal5"
 *   - `options` (object, optional) - per-periferie config:
 *       * memext: {type: "luftner4k" | ...} (string, mapuje na
 *         CFGMOD MEMEXT/type)
 *
 * Vrátí `requires_restart: true` pokud aplikace vyžaduje restart
 * emulátoru (= V1.B.2 vždy true).
 */
static en_MCP_DISPATCH_RESULT _handle_periph_attach(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    return _handle_periph_common(req, out_response, true);
}


/**
 * @brief `periph_detach` handler.
 *
 * Parametry:
 *   - `kind` (string, povinný) - "memext" | "fdc" | "qd" | "ide8" | "gal5"
 */
static en_MCP_DISPATCH_RESULT _handle_periph_detach(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    return _handle_periph_common(req, out_response, false);
}


/* ============================================================================
 * V1.C.1 - HID Tools (input_send_key / send_keys / press / release /
 *                     send_joystick / send_keys_with_delays)
 *
 * Princip:
 *   Klávesnice injekce probíhá přes PIO8255 vkbd_matrix (= virtuální
 *   matrix paralelní s fyzickou). Press / release bit operuje v
 *   `g_pio8255.vkbd_matrix[col] bit b`, viz `hid_keymap.c`.
 *
 *   Pro press + hold + release v jednom requestu (= input_send_key)
 *   handler synchronně provede:
 *     1) submit DBGAPI_CMD_INPUT_PRESS_KEY (= clear bit v vkbd_matrix)
 *     2) g_usleep podle frames (= frames * 1e6 / 50 us pro PAL 50 fps)
 *     3) submit DBGAPI_CMD_INPUT_RELEASE_KEY (= set bit zpět na 1)
 *   Emu vlákno běží paralelně, takže sleep blokuje pouze MCP dispatch
 *   thread - emu pokračuje v normální rychlosti. Klient (= AI agent)
 *   čeká na response.
 *
 * Frame timing:
 *   Default frame_rate = 50 fps (PAL MZ-800). NTSC i.e. MZ-1500 NTSC by
 *   bylo 60 fps - nicméně sleep z host side stačí přibližně, emulátor
 *   sám frame counter neřeší v reálném čase. Pro precise frame counter
 *   sync se používá V1.A.3 run_until_raster.
 *
 *   Hard upper limit pro frames = 600 (= ~12 sekund) jako safety
 *   proti AI freezi.
 * ============================================================================ */

/** Default rychlost framu (mikrosekund). 50 fps = 20000 us / frame. */
#define HID_FRAME_USEC 20000

/** Bezpečnostní limit počtu framů per request (hold). */
#define HID_FRAMES_MAX 600

/** Bezpečnostní limit délky textu / klíče (input_send_keys). */
#define HID_TEXT_MAX_LEN 256

/** Bezpečnostní limit počtu eventů (send_keys_with_delays). */
#define HID_EVENTS_MAX 256


/* Vnitřní symbol z hid_keymap.h (= MCP test build poskytne stub).
 *
 * Forward-declarujeme jen co tento dispatch.c reálně potřebuje k
 * resolve klávesy - press/release samotné dělá dbgapi handler. */
typedef struct st_HID_KEYMAP_RESOLVED {
    int  col;
    int  bit;
    bool needs_shift;
} st_HID_KEYMAP_RESOLVED;
extern bool hid_keymap_resolve(const char *name, st_HID_KEYMAP_RESOLVED *out);
extern bool hid_keymap_resolve_ascii(char c, st_HID_KEYMAP_RESOLVED *out);


/**
 * @brief Submit press/release pro jednu rezolvovanou klávesu.
 *
 * Sestaví st_DBGAPI_HID_KEY_PARAM a předá do dbgapi. Pokud handler
 * uspěje, vrací true.
 *
 * @param[in] cmd      DBGAPI_CMD_INPUT_PRESS_KEY nebo RELEASE_KEY
 * @param[in] res      resolved klávesa
 * @return true při úspěchu
 */
static bool _hid_submit_key(en_DBGAPI_CMD cmd,
                             const st_HID_KEYMAP_RESOLVED *res) {
    st_DBGAPI_HID_KEY_PARAM p = {
        .col          = res->col,
        .bit          = res->bit,
        .needs_shift  = res->needs_shift,
    };
    return _submit_dbgapi(cmd, &p, NULL);
}


/**
 * @brief Helper - blokující čekání na N video framů emulátoru.
 *
 * Sleduje `g_iface_video->fbsnapshot_screen_id` (= per-frame counter
 * inkrementovaný v emu vlákně v `iface_video_framebuffer_screen_done`)
 * a vrátí se až po N inkrementech, případně po safety timeoutu.
 *
 * Zásadní rozdíl oproti původní implementaci (= `g_usleep`): wallclock
 * sleep běžel paralelně s emu vláknem bez vazby na frame timing. Pokud
 * byla emulace pausnutá, sleep proběhl naprázdno (= žádný ISR scan),
 * což činilo `input_send_keys` nedeterministickým (klávesa nemusela
 * být zachycena v ISR). Wait na `fbsnapshot_screen_id` garantuje, že
 * mezi press a release proběhne přesně N video framů s ISR scanem.
 *
 * Synchronizace: dispatch vlákno (MCP I/O) zamkne
 * `fbsnapshot_pixels_mutex`, čte counter, čeká na cond se slice
 * timeoutem 50 ms. Emu vlákno per-frame inkrementuje counter a
 * signalizuje cond (= iface_video.h:104-111).
 *
 * Pokud je emulace paused při entry, funkce krátce unpausne ji,
 * čeká N framů a pak ji opět pausne (= deterministická "step N frames"
 * sémantika pro HID i emu_run).
 *
 * Pokud user explicit pausne během wait (= EMULATOR_TEST_PAUSED se
 * nastaví zvenku, např. UI klik), wait early-exitne s actual_frames
 * menším než requested (= caller dostane neúplný delta).
 *
 * Safety timeout: 2x očekávaný wallclock + 100 ms. Pro N=600 (= max
 * HID_FRAMES_MAX) je to ~24 s; pro emu_run max=1000 framů ~40 s.
 * Pokud emu thread havaroval / je v deadlocku, dispatch se nezavěsí
 * navždy.
 *
 * @param[in] frames  počet framů k čekání. <= 0 = no-op, return true.
 *                    > HID_FRAMES_MAX se ořeže na HID_FRAMES_MAX
 *                    (= 600).
 * @param[out] out_actual  pokud != NULL, naplní skutečný počet
 *                    proběhlých framů (= delta counter, může být
 *                    < frames při early-exit).
 * @return true pokud byl dosažen requested počet framů. false při
 *         timeoutu, early-exit (= user pause), nebo když g_iface_video
 *         není dostupný.
 *
 * @note V test buildu (= MZ800EMU_MCP_TEST_BUILD) je no-op (= vrací
 *       true a out_actual = frames). g_iface_video v testu neexistuje
 *       a žádný emu thread neproduuje frame ticks.
 *
 * @note Funkce nesmí být volána z emu vlákna - vyvolá deadlock
 *       (= emu vlákno samo signalizuje counter, takže by čekalo na
 *       sebe). Dispatch vrstva běží v MCP I/O vlákně, takže to platí.
 */
static bool _dispatch_wait_frames(int frames, int *out_actual) {
#ifdef MZ800EMU_MCP_TEST_BUILD
    /* Test build: žádný emu thread, žádné frame ticks. No-op s
     * "úspěchem", aby test scénáře neblokovaly. */
    if (out_actual) *out_actual = (frames > 0) ? frames : 0;
    (void)frames;
    return true;
#else
    if (frames <= 0) {
        if (out_actual) *out_actual = 0;
        return true;
    }
    if (frames > HID_FRAMES_MAX) frames = HID_FRAMES_MAX;
    if (!g_iface_video) {
        if (out_actual) *out_actual = 0;
        return false;
    }

    /* Pokud byla emulace paused, krátce unpausneme - jinak emu vlákno
     * nikdy neinkrementuje counter a wait vyteče safety timeoutem.
     * Zapamatujeme původní stav, abychom ho restorovali. */
    bool was_paused = EMULATOR_TEST_PAUSED;
    if (was_paused) {
        emulator_pause(false);
    }

    APP_MUTEX_LOCK(g_iface_video->fbsnapshot_pixels_mutex);
    uint32_t start = g_iface_video->fbsnapshot_screen_id;

    /* Safety: 2x očekávaný wallclock + 100 ms. PAL frame = 20 ms,
     * NTSC ~16.7 ms; bereme 40 ms/frame jako horní odhad (= reálně
     * je to méně, takže timeout je konzervativní). */
    gint64 deadline_us = g_get_monotonic_time()
                       + ((gint64)frames * 40 + 100) * 1000;

    while ((g_iface_video->fbsnapshot_screen_id - start)
           < (uint32_t)frames) {
        gint64 now_us = g_get_monotonic_time();
        if (now_us >= deadline_us) break;
        gint32 remaining_ms = (gint32)((deadline_us - now_us) / 1000);
        if (remaining_ms < 1) remaining_ms = 1;
        if (remaining_ms > 50) remaining_ms = 50;
        APP_COND_WAIT_TIMEOUT_MS(g_iface_video->fbsnapshot_pixels_cond,
                                  g_iface_video->fbsnapshot_pixels_mutex,
                                  remaining_ms);
        /* Early-exit: user explicit PAUSE submitnul během wait
         * (= GUI klik, hotkey). Respektujeme to a vracíme actual delta. */
        if (EMULATOR_TEST_PAUSED) break;
    }

    uint32_t delta = g_iface_video->fbsnapshot_screen_id - start;
    APP_MUTEX_UNLOCK(g_iface_video->fbsnapshot_pixels_mutex);

    /* Restore: pokud byla pausnuta a user ji během wait nepřepausoval,
     * vracíme do paused stavu. Pokud user pausnul (= EMULATOR_TEST_PAUSED
     * je true), nic neděláme. */
    if (was_paused && !EMULATOR_TEST_PAUSED) {
        emulator_pause(true);
    }

    if (out_actual) *out_actual = (int)delta;
    return delta >= (uint32_t)frames;
#endif
}


/**
 * @brief Počká, až emu vlákno samo deterministicky doběhne frame-bounded run.
 *
 * Použití výhradně z `_handle_run` blokující path po submitu
 * `DBGAPI_CMD_RUN_FRAMES`. Na rozdíl od `_dispatch_wait_frames` tato funkce
 * NEMANIPULUJE pause stavem emulace - emu se pausne SÁM v hot loopu
 * (mzarch.c, frame-bounded check), jakmile dosáhne cílové frame hranice.
 * Dispatch jen čeká na dokončení.
 *
 * Proč ne `_dispatch_wait_frames`: ten při entry kontroluje
 * `was_paused = EMULATOR_TEST_PAUSED` a pokud je emu paused, krátce ho
 * UNPAUSNE (= jeho "step N frames" sémantika). Pro malá N a pomalé dispatch
 * vlákno hrozí race: emu by mohl doběhnout cíl a pausnout se DŘÍV, než sem
 * dispatch dorazí; `_dispatch_wait_frames` by ho pak znovu unpausnul a emu
 * by běžel za cílovou hranici (= ztráta determinismu). Tato funkce se proto
 * pause stavu nedotýká.
 *
 * Synchronizace: čeká na `g_iface_video->fbsnapshot_pixels_cond` (signál
 * per dokončený video frame) pro výpočet actual_frames a wakeup. Primární
 * exit podmínka je ale `EMULATOR_TEST_PAUSED` (= emu se sám deterministicky
 * pausnul po N framech), takže funkce skončí i kdyby se video frame counter
 * (`fbsnapshot_screen_id`) rozcházel s emu frame counterem
 * (`g_gdg.total_elapsed.screens`) - video frame se v MAX SPEED nemusí
 * vykreslit každý snímek, ale screens se inkrementuje vždy.
 *
 * Safety timeout: 2x očekávaný wallclock + 100 ms (= konzervativní).
 * Pokud emu thread havaroval / je v deadlocku, dispatch se nezavěsí navždy.
 * Caller (`_handle_run`) pak pošle fallback PAUSE.
 *
 * @param[in] frames  očekávaný počet framů (pro výpočet safety timeoutu).
 *                    <= 0 = no-op, return true. Bez HID_FRAMES_MAX clampu -
 *                    emu_run rozsah je 0..1000 framů.
 * @param[out] out_actual  pokud != NULL, naplní počet proběhlých video framů
 *                    (= delta `fbsnapshot_screen_id`, může být < frames při
 *                    MAX SPEED nevykreslení nebo timeoutu).
 * @return true pokud se emu deterministicky pausnul (= EMULATOR_TEST_PAUSED).
 *         false při safety timeoutu (= emu stále běží, caller musí PAUSE) nebo
 *         když `g_iface_video` není dostupný.
 *
 * @note V test buildu (= MZ800EMU_MCP_TEST_BUILD) je no-op (vrací true,
 *       out_actual = frames). V testu neexistuje emu thread ani g_iface_video.
 *
 * @note Nesmí být volána z emu vlákna (deadlock - emu sám signalizuje cond).
 *       Dispatch běží v MCP I/O vlákně, takže to platí.
 */
static bool _dispatch_wait_run_frames_done(int frames, int *out_actual) {
#ifdef MZ800EMU_MCP_TEST_BUILD
    /* Test build: žádný emu thread, žádné frame ticks. No-op s úspěchem. */
    if (out_actual) *out_actual = (frames > 0) ? frames : 0;
    (void)frames;
    return true;
#else
    if (frames <= 0) {
        if (out_actual) *out_actual = 0;
        return true;
    }
    if (!g_iface_video) {
        if (out_actual) *out_actual = 0;
        return false;
    }

    APP_MUTEX_LOCK(g_iface_video->fbsnapshot_pixels_mutex);
    uint32_t start = g_iface_video->fbsnapshot_screen_id;

    /* Safety: 2x očekávaný wallclock + 100 ms. PAL frame = 20 ms,
     * bereme 40 ms/frame jako konzervativní horní odhad. emu_run rozsah je
     * 0..1000 framů (na rozdíl od HID 0..600), proto timeout počítáme z plného
     * N - jinak by pro N v 600..1000 mohl fallback PAUSE přijít předčasně. */
    gint64 deadline_us = g_get_monotonic_time()
                       + ((gint64)frames * 40 + 100) * 1000;

    bool emu_paused = false;
    while (1) {
        /* Primární exit: emu se sám deterministicky pausnul po N framech
         * (frame-bounded check v mzarch.c). */
        if (EMULATOR_TEST_PAUSED) { emu_paused = true; break; }

        gint64 now_us = g_get_monotonic_time();
        if (now_us >= deadline_us) break;   /* safety timeout */
        gint32 remaining_ms = (gint32)((deadline_us - now_us) / 1000);
        if (remaining_ms < 1) remaining_ms = 1;
        if (remaining_ms > 50) remaining_ms = 50;
        APP_COND_WAIT_TIMEOUT_MS(g_iface_video->fbsnapshot_pixels_cond,
                                  g_iface_video->fbsnapshot_pixels_mutex,
                                  remaining_ms);
    }

    uint32_t delta = g_iface_video->fbsnapshot_screen_id - start;
    APP_MUTEX_UNLOCK(g_iface_video->fbsnapshot_pixels_mutex);

    if (out_actual) *out_actual = (int)delta;
    return emu_paused;
#endif
}


/**
 * @brief Helper - clamp frames a blokující čekání podle frame counteru.
 *
 * Wrapper kolem `_dispatch_wait_frames` se ztrátou out_actual (= HID
 * call site se nezajímá o skutečné delta, jen o "wait done"). Zachovává
 * starou signaturu void(int) pro existující call sites z HID handlerů.
 *
 * Pro frames <= 0 nedělá nic. Frames > HID_FRAMES_MAX se ořeže.
 * V test buildu (= MCP_TEST_BUILD) je no-op (= testy neblokujeme).
 */
static void _hid_sleep_frames(int frames) {
    (void)_dispatch_wait_frames(frames, NULL);
}


/**
 * @brief Společná logika press + hold + release pro jednu klávesu.
 *
 * Pokud `frames > 0`, mezi press a release vloží sleep. Pokud
 * `frames == 0`, jen press (= trvalý hold).
 *
 * Readback dosednutí (fix 0016 / cesta A): pokud `release == true`,
 * před press ozbrojí PIO 8255 probe na cílový sloupec klávesy a po
 * sleepu zjistí, zda guest během držení cílový sloupec naskenoval
 * (= klávesa byla skutečně přečtena). Výsledek se zapíše do
 * `*out_landed`. Při `release == false` (trvalý hold bez sleepu) se
 * probe nepoužívá a `*out_landed` se nastaví na false (= nelze ověřit
 * bez okna držení).
 *
 * V testovacím buildu (MZ800EMU_MCP_TEST_BUILD) probe API není dostupné
 * a `*out_landed` je vždy false.
 *
 * @param[in]  res        resolvovaná klávesa
 * @param[in]  frames     počet framů držet (0 = trvalý hold, no release)
 * @param[in]  release    true = po sleep poslat i release (default pro
 *                        send_key); false = press only (press_key)
 * @param[out] out_landed pokud != NULL, naplní true/false dle toho, zda
 *                        guest během držení naskenoval cílový sloupec
 * @return true při úspěchu, false pokud kterýkoliv submit selže
 */
static bool _hid_press_hold_release(const st_HID_KEYMAP_RESOLVED *res,
                                     int frames,
                                     bool release,
                                     bool *out_landed) {
    if (out_landed) *out_landed = false;

#ifndef MZ800EMU_MCP_TEST_BUILD
    bool probe_armed = false;
    if (release) {
        /* Sledujeme hlavní klávesu (col/bit); shift je pomocný a jeho
         * readback není nutný (spec bod 7). */
        pio8255_vkbd_probe_arm(res->col, res->bit);
        probe_armed = true;
    }
#endif

    if (!_hid_submit_key(DBGAPI_CMD_INPUT_PRESS_KEY, res)) {
#ifndef MZ800EMU_MCP_TEST_BUILD
        if (probe_armed) pio8255_vkbd_probe_disarm();
#endif
        return false;
    }
    if (!release) {
        return true;
    }
    _hid_sleep_frames(frames);

#ifndef MZ800EMU_MCP_TEST_BUILD
    if (probe_armed) {
        bool landed = pio8255_vkbd_probe_check();
        pio8255_vkbd_probe_disarm();
        if (out_landed) *out_landed = landed;
    }
#endif

    return _hid_submit_key(DBGAPI_CMD_INPUT_RELEASE_KEY, res);
}


/**
 * @brief `input_send_key` handler - press + hold + release jedné klávesy.
 *
 * Parametry data:
 *   key      (string) - jméno klávesy (RETURN, SHIFT, ARROW_UP, ...) nebo
 *                       "ASCII:<znak>" / single-character literal
 *   frames   (int)    - počet framů držet (default 3, max 600)
 *
 * Response: `{"key": "<resolved-name>", "col": N, "bit": N,
 *            "shift": bool, "frames": N, "sent": true,
 *            "landing_verified": bool}`.
 *
 * `landing_verified` (fix 0016 / cesta A) je skutečný signál dosednutí:
 * true = guest během držení klávesy naskenoval cílový sloupec
 * klávesnice (přes PIO 8255 probe, viz pio8255_vkbd_probe_*) a tedy
 * vstříknutý bit fyzicky přečetl; false = nedosedlo (guest cílový
 * sloupec během okna držení neskenoval - např. běží program s odpojenou
 * ROM skenující jen úzkou podmnožinu sloupců, je idle, nebo frames=0).
 * `sent` = true znamená jen, že host-side injekce proběhla; teprve
 * `landing_verified` říká, zda ji guest přečetl. V testovacím buildu
 * (MZ800EMU_MCP_TEST_BUILD) je probe nedostupný a flag je vždy false.
 *
 * Chyba: 422 (INVALID_PARAMS) pokud key chybí nebo není rezolvovatelná.
 *        500 (EMU_ERROR) pokud dbgapi submit selže.
 */
static en_MCP_DISPATCH_RESULT _handle_input_send_key(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    if (!json_object_has_member(obj, "key")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *key = json_object_get_string_member(obj, "key");
    if (!key || key[0] == '\0') {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 frames = _obj_int_or(obj, "frames", 3);
    if (frames < 0) frames = 0;
    if (frames > HID_FRAMES_MAX) frames = HID_FRAMES_MAX;

    st_HID_KEYMAP_RESOLVED res;
    if (!hid_keymap_resolve(key, &res)) {
        return _err_response(req_id, "Unknown key",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool landed = false;
    if (!_hid_press_hold_release(&res, (int)frames, true, &landed)) {
        return _err_response(req_id, "input_send_key failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "key", key);
    json_object_set_int_member(resp, "col", res.col);
    json_object_set_int_member(resp, "bit", res.bit);
    json_object_set_boolean_member(resp, "shift", res.needs_shift);
    json_object_set_int_member(resp, "frames", (gint64)frames);
    json_object_set_boolean_member(resp, "sent", TRUE);
    /* Skutečný readback (fix 0016 / cesta A): true = guest během držení
     * naskenoval cílový sloupec klávesnice (= klávesu přečetl); false =
     * nedosedlo (guest sloupec neskenuje / je idle / frames=0). */
    json_object_set_boolean_member(resp, "landing_verified",
                                   landed ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `input_send_keys` handler - sekvence kláves.
 *
 * Parametry:
 *   text          (string) - text k odeslání. Pro encoding=ascii
 *                            normální string (např. "RUN\r"). Pro
 *                            encoding=key_names JSON array string
 *                            (např. "[\"RUN\",\"RETURN\"]") - kde
 *                            položky jsou jména kláves.
 *   encoding      (string) - "ascii" (default) nebo "key_names"
 *   frame_per_key (int)    - počet framů na klávesu (default 3)
 *
 * Response: `{"keys_sent": N, "keys_landed": N, "total_frames": N,
 *            "encoding": str, "landing_verified": bool}`.
 *
 * `keys_sent` počítá host-side injekce (press/hold/release do vkbd
 * matrix). `keys_landed` (fix 0016 / cesta A) počítá, kolik z nich guest
 * během držení skutečně přečetl (= naskenoval cílový sloupec přes PIO
 * 8255 probe). `landing_verified` je true jen když dosedly VŠECHNY
 * odeslané klávesy (keys_landed == keys_sent && keys_sent > 0); jinak
 * false. Guest se klávesy nemusí dočkat, pokud neskenuje cílový sloupec
 * (běžící program s odpojenou ROM skenuje jen úzkou podmnožinu, fix 0016
 * / R3 VRSTVA 1). V testovacím buildu (MZ800EMU_MCP_TEST_BUILD) je probe
 * nedostupný a keys_landed je vždy 0 (landing_verified false).
 *
 * Implementace: parsing rozhoduje, pak iterace press → sleep → release.
 */
static en_MCP_DISPATCH_RESULT _handle_input_send_keys(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    if (!json_object_has_member(obj, "text")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *text = json_object_get_string_member(obj, "text");
    if (!text) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *encoding = "ascii";
    if (json_object_has_member(obj, "encoding")) {
        const char *e = json_object_get_string_member(obj, "encoding");
        if (e) encoding = e;
    }
    gint64 frame_per_key = _obj_int_or(obj, "frame_per_key", 3);
    if (frame_per_key < 0) frame_per_key = 0;
    if (frame_per_key > HID_FRAMES_MAX) frame_per_key = HID_FRAMES_MAX;

    int keys_sent = 0;
    int keys_landed = 0; /* kolik z keys_sent guest skutečně přečetl (fix 0016) */

    if (strcmp(encoding, "ascii") == 0) {
        size_t tlen = strlen(text);
        if (tlen > HID_TEXT_MAX_LEN) tlen = HID_TEXT_MAX_LEN;
        for (size_t i = 0; i < tlen; i++) {
            st_HID_KEYMAP_RESOLVED res;
            if (!hid_keymap_resolve_ascii(text[i], &res)) {
                continue; /* unresolvable char - skip, ne fail */
            }
            bool landed = false;
            if (!_hid_press_hold_release(&res, (int)frame_per_key, true,
                                         &landed)) {
                return _err_response(req_id, "input_send_keys submit failed",
                                     MCP_DISPATCH_EMU_ERROR, out_response);
            }
            keys_sent++;
            if (landed) keys_landed++;
        }
    } else if (strcmp(encoding, "key_names") == 0) {
        /* Parsing JSON array stringu inline - vyhneme se další json-glib
         * Parser instanci, protože text je už uvnitř requestu. */
        JsonParser *parser = json_parser_new();
        GError *err = NULL;
        if (!json_parser_load_from_data(parser, text, -1, &err)) {
            if (err) g_error_free(err);
            g_object_unref(parser);
            return _err_response(req_id, "Invalid parameters (text not JSON)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonNode *root = json_parser_get_root(parser);
        if (!root || json_node_get_node_type(root) != JSON_NODE_ARRAY) {
            g_object_unref(parser);
            return _err_response(req_id, "Invalid parameters (text not array)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonArray *arr = json_node_get_array(root);
        guint len = json_array_get_length(arr);
        if (len > HID_TEXT_MAX_LEN) len = HID_TEXT_MAX_LEN;
        for (guint i = 0; i < len; i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (!el || json_node_get_node_type(el) != JSON_NODE_VALUE) {
                continue;
            }
            const char *kname = json_node_get_string(el);
            if (!kname || kname[0] == '\0') continue;
            st_HID_KEYMAP_RESOLVED res;
            if (!hid_keymap_resolve(kname, &res)) {
                continue;
            }
            bool landed = false;
            if (!_hid_press_hold_release(&res, (int)frame_per_key, true,
                                         &landed)) {
                g_object_unref(parser);
                return _err_response(req_id, "input_send_keys submit failed",
                                     MCP_DISPATCH_EMU_ERROR, out_response);
            }
            keys_sent++;
            if (landed) keys_landed++;
        }
        g_object_unref(parser);
    } else {
        return _err_response(req_id, "Invalid parameters (bad encoding)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "keys_sent", keys_sent);
    json_object_set_int_member(resp, "keys_landed", keys_landed);
    json_object_set_int_member(resp, "total_frames",
                                keys_sent * frame_per_key);
    json_object_set_string_member(resp, "encoding", encoding);
    /* Skutečný readback (fix 0016 / cesta A): keys_sent = host-side
     * injekce, keys_landed = kolik z nich guest během držení skutečně
     * přečetl (naskenoval cílový sloupec). landing_verified je true jen
     * když dosedly VŠECHNY odeslané klávesy (a alespoň jedna byla
     * odeslána); jinak false. Klient může z keys_landed/keys_sent zjistit
     * částečné dosednutí. */
    bool all_landed = (keys_sent > 0) && (keys_landed == keys_sent);
    json_object_set_boolean_member(resp, "landing_verified",
                                   all_landed ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `input_press_key` handler - trvalý press bez auto-release.
 *
 * Parametry: key (string). Klávesa zůstává držena dokud klient
 * nezavolá `input_release_key` se stejným key (nebo bez argumentu pro
 * release-all).
 *
 * Response: `{"key": "<name>", "col": N, "bit": N, "shift": bool}`.
 */
static en_MCP_DISPATCH_RESULT _handle_input_press_key(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    const char *key = json_object_has_member(obj, "key")
        ? json_object_get_string_member(obj, "key") : NULL;
    if (!key || key[0] == '\0') {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_HID_KEYMAP_RESOLVED res;
    if (!hid_keymap_resolve(key, &res)) {
        return _err_response(req_id, "Unknown key",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!_hid_submit_key(DBGAPI_CMD_INPUT_PRESS_KEY, &res)) {
        return _err_response(req_id, "input_press_key failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "key", key);
    json_object_set_int_member(resp, "col", res.col);
    json_object_set_int_member(resp, "bit", res.bit);
    json_object_set_boolean_member(resp, "shift", res.needs_shift);
    json_object_set_boolean_member(resp, "pressed", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `input_release_key` handler - release konkrétní klávesy
 *        nebo všech kláves.
 *
 * Parametry: key (string, optional). Pokud chybí nebo je prázdný,
 * uvolní VŠECHNY klávesy ve vkbd_matrix (= release-all).
 *
 * Response: `{"key": "<name>"|null, "released_all": bool}`.
 */
static en_MCP_DISPATCH_RESULT _handle_input_release_key(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    const char *key = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        JsonObject *obj = json_node_get_object(data_node);
        if (json_object_has_member(obj, "key")) {
            key = json_object_get_string_member(obj, "key");
        }
    }
    if (!key || key[0] == '\0') {
        /* Release-all */
        if (!_submit_dbgapi(DBGAPI_CMD_INPUT_RELEASE_ALL, NULL, NULL)) {
            return _err_response(req_id, "input_release_key (all) failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        JsonObject *resp = json_object_new();
        json_object_set_null_member(resp, "key");
        json_object_set_boolean_member(resp, "released_all", TRUE);
        return _ok_response(req_id, resp, out_response);
    }
    st_HID_KEYMAP_RESOLVED res;
    if (!hid_keymap_resolve(key, &res)) {
        return _err_response(req_id, "Unknown key",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!_hid_submit_key(DBGAPI_CMD_INPUT_RELEASE_KEY, &res)) {
        return _err_response(req_id, "input_release_key failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "key", key);
    json_object_set_boolean_member(resp, "released_all", FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `input_send_joystick` handler - press joystick state + hold +
 *        release.
 *
 * Parametry: port (0..1), state (0..255 bitmask), frames (default 3).
 *
 * Response: `{"port": N, "state": N, "frames": N, "sent": true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_input_send_joystick(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    gint64 port = _obj_int_or(obj, "port", -1);
    gint64 state = _obj_int_or(obj, "state", -1);
    gint64 frames = _obj_int_or(obj, "frames", 3);
    if (port < 0 || port > 1 || state < 0 || state > 255) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (frames < 0) frames = 0;
    if (frames > HID_FRAMES_MAX) frames = HID_FRAMES_MAX;

    st_DBGAPI_HID_JOY_PARAM set_p = {
        .port = (int)port, .mcp_mask = (uint8_t)state
    };
    if (!_submit_dbgapi(DBGAPI_CMD_INPUT_JOY_SET, &set_p, NULL)) {
        return _err_response(req_id, "input_send_joystick set failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    _hid_sleep_frames((int)frames);
    st_DBGAPI_HID_JOY_PARAM clr_p = {
        .port = (int)port, .mcp_mask = 0
    };
    if (!_submit_dbgapi(DBGAPI_CMD_INPUT_JOY_CLEAR, &clr_p, NULL)) {
        return _err_response(req_id, "input_send_joystick clear failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "port", port);
    json_object_set_int_member(resp, "state", state);
    json_object_set_int_member(resp, "frames", frames);
    json_object_set_boolean_member(resp, "sent", TRUE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `input_send_keys_with_delays` handler - timing-controlled
 *        sekvence kláves.
 *
 * Parametry: events (JSON array). Každý event je objekt s poli:
 *   key          (string)
 *   hold_frames  (int, default 3)
 *   gap_frames   (int, default 0)
 *
 * Per event: resolve → press → sleep hold_frames → release → sleep
 * gap_frames.
 *
 * Response: `{"events_processed": N, "total_frames": N}`.
 */
static en_MCP_DISPATCH_RESULT _handle_input_send_keys_with_delays(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    if (!json_object_has_member(obj, "events")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *events_node = json_object_get_member(obj, "events");
    if (!events_node ||
        json_node_get_node_type(events_node) != JSON_NODE_ARRAY) {
        return _err_response(req_id, "Invalid parameters (events not array)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonArray *arr = json_node_get_array(events_node);
    guint len = json_array_get_length(arr);
    if (len == 0) {
        return _err_response(req_id, "Invalid parameters (empty events)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (len > HID_EVENTS_MAX) len = HID_EVENTS_MAX;

    int events_processed = 0;
    gint64 total_frames = 0;
    for (guint i = 0; i < len; i++) {
        JsonNode *el = json_array_get_element(arr, i);
        if (!el || json_node_get_node_type(el) != JSON_NODE_OBJECT) {
            continue;
        }
        JsonObject *eobj = json_node_get_object(el);
        const char *key = json_object_has_member(eobj, "key")
            ? json_object_get_string_member(eobj, "key") : NULL;
        if (!key || key[0] == '\0') continue;
        gint64 hold = _obj_int_or(eobj, "hold_frames", 3);
        gint64 gap  = _obj_int_or(eobj, "gap_frames", 0);
        if (hold < 0) hold = 0;
        if (gap < 0) gap = 0;
        if (hold > HID_FRAMES_MAX) hold = HID_FRAMES_MAX;
        if (gap > HID_FRAMES_MAX) gap = HID_FRAMES_MAX;

        st_HID_KEYMAP_RESOLVED res;
        if (!hid_keymap_resolve(key, &res)) {
            continue;
        }
        if (!_hid_submit_key(DBGAPI_CMD_INPUT_PRESS_KEY, &res)) {
            return _err_response(req_id, "send_keys_with_delays press failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        _hid_sleep_frames((int)hold);
        if (!_hid_submit_key(DBGAPI_CMD_INPUT_RELEASE_KEY, &res)) {
            return _err_response(req_id, "send_keys_with_delays release failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        _hid_sleep_frames((int)gap);
        events_processed++;
        total_frames += hold + gap;
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "events_processed", events_processed);
    json_object_set_int_member(resp, "total_frames", total_frames);
    return _ok_response(req_id, resp, out_response);
}


/* ==================================================================== */
/* V1.D.1 - Core + CPU extras Resources (8 read-only handlers)          */
/* ==================================================================== */

/**
 * @brief Pomocná: vrátí název enum hodnoty source pro memory/map Resource.
 *
 * Mapování source byte -> string. Hodnoty viz dbgapi_cmdrq.h (komentář
 * u st_DBGAPI_MEMORY_MAP_SLOT). Pro neznámou hodnotu vrátí "unknown".
 */
static const char *_memory_source_str(uint8_t s) {
    switch (s) {
        case 1: return "rom";
        case 2: return "cgrom";
        case 3: return "sram";
        case 4: return "vram";
        case 5: return "memext_ram";
        case 6: return "memext_flash";
        default: return "unknown";
    }
}


/**
 * @brief Pomocná: serializace en_COOPERATION_HINT na wire string.
 *
 * Sdílená logika s cooperation_hint_mode_to_str, ale s fallback "unknown"
 * pro out-of-range hodnoty (= defense in depth).
 */
#if defined(MZ800EMU_CFG_MCP_SERVER_ENABLED) && !defined(MZ800EMU_MCP_TEST_BUILD)
static const char *_coop_mode_to_str_safe(en_COOPERATION_HINT m) {
    const char *s = cooperation_hint_mode_to_str(m);
    return s ? s : "unknown";
}
#endif


/**
 * @brief `get_config_settings` handler - dispatcher-local, čte cfgmain
 *        INI hodnoty pro whitelist klíčů.
 *
 * V1.D.1 vystavuje hierarchickou strukturu `{section: {key: value}}`.
 * Reuse `DBGAPI_CMD_SETTINGS_GET` (= per-key submit) - pomalé pokud by
 * whitelist byl velký, ale klíčů je < 20, takže OK. Pokud security
 * profile == OBSERVER, vrátí filtered=true + prázdný settings objekt
 * (= AI by neměl číst cesty / dev keys).
 *
 * Response: `{"profile": "wild|...", "filtered": bool, "sections": {...}}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_config_settings(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *resp = json_object_new();

#if defined(MZ800EMU_CFG_MCP_TCP_ENABLED) && !defined(MZ800EMU_MCP_TEST_BUILD)
    en_MCP_PROFILE prof = g_mcp_config.profile;
    const char *prof_str = mcp_config_profile_str(prof);
    json_object_set_string_member(resp, "profile",
                                   prof_str ? prof_str : "wild");
    if (prof == MCP_PROFILE_OBSERVER) {
        /* Observer profile: žádné settings hodnoty (= AI by mohlo
         * přečíst paths / dev klíče). */
        json_object_set_boolean_member(resp, "filtered", TRUE);
        JsonObject *empty = json_object_new();
        json_object_set_object_member(resp, "sections", empty);
        return _ok_response(req_id, resp, out_response);
    }
#else
    json_object_set_string_member(resp, "profile", "wild");
#endif
    json_object_set_boolean_member(resp, "filtered", FALSE);

    /* Iterace whitelistu - klíče tvaru "MODULE/element". Pro každý
     * provede DBGAPI_CMD_SETTINGS_GET a hodnotu uloží do
     * sections[MODULE][element]. */
    static const char *const v1d1_whitelist[] = {
        "AUDIO/volume_8253",
        "AUDIO/volume_psg0",
        "AUDIO/volume_psg1",
        "AUDIO/volume_psg2",
        "AUDIO/volume_psg3",
        "DISPLAY/forced_full_screen_redrawing",
        "DISPLAY/locked_window_aspect_ratio",
        "DISPLAY/custom_fps",
        NULL,
    };

    JsonObject *sections = json_object_new();
    for (size_t i = 0; v1d1_whitelist[i] != NULL; i++) {
        const char *key = v1d1_whitelist[i];
        const char *slash = strchr(key, '/');
        if (!slash || slash == key || slash[1] == '\0') continue;
        char *module  = g_strndup(key, (gsize)(slash - key));
        char *element = g_strdup(slash + 1);

        st_DBGAPI_SETTINGS_PARAM p;
        memset(&p, 0, sizeof(p));
        p.module    = module;
        p.element   = element;
        p.new_value = NULL;
        bool ok = _submit_dbgapi(DBGAPI_CMD_SETTINGS_GET, &p, NULL);
        if (ok && p.out_result == 0 && p.out_value) {
            /* Sekce v sections (lazy create). */
            JsonObject *sec = NULL;
            JsonNode *sec_node = json_object_has_member(sections, module)
                ? json_object_get_member(sections, module) : NULL;
            if (sec_node &&
                json_node_get_node_type(sec_node) == JSON_NODE_OBJECT) {
                sec = json_node_get_object(sec_node);
            } else {
                sec = json_object_new();
                json_object_set_object_member(sections, module, sec);
            }
            json_object_set_string_member(sec, element, p.out_value);
        }
        if (p.out_value) g_free(p.out_value);
        g_free(module);
        g_free(element);
    }
    json_object_set_object_member(resp, "sections", sections);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_media_state` handler - alias pro media_state Tool jako
 *        Resource backing. Reuse `_handle_media_state` payloadu, ale
 *        wrapne ho do Resource-friendly struktury.
 *
 * V1.D.1 minimal: zachová existující slot pole, přidá `note` field
 * indikující že rozšířené metadata (motor / head_pos) přijdou v V1.D.2.
 *
 * Response: `{"slots": [...], "count": N, "note": "..."}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_media_state(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_MEDIA_STATE_PARAM param;
    memset(&param, 0, sizeof(param));
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEDIA_STATE, &param, NULL);
    if (!ok) {
        return _err_response(req_id, "get_media_state failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    JsonArray  *arr = json_array_new();
    for (int i = 0; i < param.count; i++) {
        const st_DBGAPI_MEDIA_SLOT_INFO *s = &param.slots[i];
        JsonObject *item = json_object_new();
        json_object_set_string_member(item, "slot", _media_slot_str(s->slot));
        json_object_set_boolean_member(item, "inserted",
                                        s->inserted ? TRUE : FALSE);
        json_object_set_string_member(item, "path",
                                       s->filepath[0] ? s->filepath : "");
        json_object_set_boolean_member(item, "ro",
                                        s->read_only ? TRUE : FALSE);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "slots", arr);
    json_object_set_int_member(resp, "count", (gint64)param.count);
    json_object_set_string_member(resp, "note",
        "extended fields (motor, head_pos) planned for V1.D.2");
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_cpu_im2_vector` handler - Z80 IM2 vector snapshot.
 *
 * Response: `{"im": int, "i": int, "vec": int, "available": bool,
 *             "isr_addr": int, "isr_target": int}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_cpu_im2_vector(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_CPU_IM2_VECTOR_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_CPU_IM2_VECTOR, &param, NULL)) {
        return _err_response(req_id, "get_cpu_im2_vector failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp,    "im",         (gint64)param.im);
    json_object_set_int_member(resp,    "i",          (gint64)param.i_reg);
    json_object_set_int_member(resp,    "vec",        (gint64)param.last_vec);
    json_object_set_boolean_member(resp, "available", param.available ? TRUE : FALSE);
    json_object_set_int_member(resp,    "isr_addr",   (gint64)param.isr_addr);
    json_object_set_int_member(resp,    "isr_target", (gint64)param.isr_target);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_cpu_interrupt_bus` handler - IRQ subsystem snapshot.
 *
 * V1.D.1 minimal: Z80 core flags + per-platform note. Per-chip sub-sekce
 * (daisy_chain, non_chain_sources, nmi_sources) jsou available=0 s reason
 * - implementace V1.D.2.
 *
 * Response: `{"z80_state": {...}, "platform_note": "...", "daisy_chain":
 * {"available": false, "reason": "..."}, "non_chain_sources": {...},
 * "nmi_sources": {...}, "recent_acks": []}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_cpu_interrupt_bus(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_CPU_IRQ_BUS_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_CPU_INTERRUPT_BUS, &param, NULL)) {
        return _err_response(req_id, "get_cpu_interrupt_bus failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();

    /* z80_state pod-objekt */
    JsonObject *z80s = json_object_new();
    json_object_set_int_member(z80s,    "iff1",       (gint64)param.iff1);
    json_object_set_int_member(z80s,    "iff2",       (gint64)param.iff2);
    json_object_set_int_member(z80s,    "im",         (gint64)param.im);
    json_object_set_boolean_member(z80s, "halted",    param.halted ? TRUE : FALSE);
    json_object_set_boolean_member(z80s, "int_line",  param.int_line ? TRUE : FALSE);
    json_object_set_boolean_member(z80s, "nmi_line",  param.nmi_line ? TRUE : FALSE);
    json_object_set_int_member(z80s,    "i",          (gint64)param.i_reg);
    json_object_set_boolean_member(z80s, "ei_pending",
                                    param.ei_pending ? TRUE : FALSE);
    json_object_set_object_member(resp, "z80_state", z80s);

    json_object_set_string_member(resp, "platform_note", param.platform_note);

    /* daisy_chain (V1.D.1 placeholder) */
    JsonObject *dc = json_object_new();
    json_object_set_boolean_member(dc, "available",
                                    param.daisy_chain_available ? TRUE : FALSE);
    json_object_set_string_member(dc, "reason", param.daisy_chain_reason);
    json_object_set_object_member(resp, "daisy_chain", dc);

    /* non_chain_sources (V1.D.1 placeholder) */
    JsonObject *nc = json_object_new();
    json_object_set_boolean_member(nc, "available",
                                    param.non_chain_available ? TRUE : FALSE);
    json_object_set_string_member(nc, "reason", param.non_chain_reason);
    json_object_set_object_member(resp, "non_chain_sources", nc);

    /* nmi_sources (V1.D.1 placeholder) */
    JsonObject *nmi = json_object_new();
    json_object_set_boolean_member(nmi, "available",
                                    param.nmi_sources_available ? TRUE : FALSE);
    json_object_set_string_member(nmi, "reason", param.nmi_sources_reason);
    json_object_set_object_member(resp, "nmi_sources", nmi);

    /* recent_acks prázdné pole - V1.D.2 ring buffer. */
    JsonArray *racks = json_array_new();
    json_object_set_array_member(resp, "recent_acks", racks);

    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_cooperation_policy` handler - dispatcher-local snapshot
 *        cooperation hint state.
 *
 * Reuse V1.A.1 globální g_cooperation_hint. Pole `set_by` ve V1.D.1
 * vždy "ai" (= jediný možný setter je AI klient přes cooperation_hint_set
 * Tool); pokud bude V1.D.2 přidávat GUI overlay, doplníme rozlišení.
 *
 * Response: `{"mode": str, "until": str|null, "set_by": "ai",
 *             "set_at_us": int}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_cooperation_policy(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *resp = json_object_new();
#if defined(MZ800EMU_CFG_MCP_SERVER_ENABLED) && !defined(MZ800EMU_MCP_TEST_BUILD)
    json_object_set_string_member(resp, "mode",
        _coop_mode_to_str_safe(g_cooperation_hint.mode));
    if (g_cooperation_hint.until && g_cooperation_hint.until[0]) {
        json_object_set_string_member(resp, "until",
                                       g_cooperation_hint.until);
    } else {
        json_object_set_null_member(resp, "until");
    }
    json_object_set_string_member(resp, "set_by", "ai");
    json_object_set_int_member(resp, "set_at_us",
                               (gint64)g_cooperation_hint.set_at_us);
#else
    /* Stub build (testovací) - vrátíme defaults aby JSON layout byl
     * konzistentní pro klienta. */
    json_object_set_string_member(resp, "mode",    "free");
    json_object_set_null_member(resp,   "until");
    json_object_set_string_member(resp, "set_by",  "ai");
    json_object_set_int_member(resp,    "set_at_us", 0);
#endif
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_security_profile` handler - dispatcher-local snapshot
 *        MCP security profile + capabilities.
 *
 * V0.B.4 ukládá jen profile string; full enforcement (whitelist,
 * capability gating) zatím není - klient o tom dostane note.
 *
 * Response: `{"profile": str, "capabilities": [...], "file_access_paths":
 *             [...], "auth": {"required": false}, "enforcement_note": "..."}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_security_profile(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *resp = json_object_new();

    const char *prof_str = "wild";
#if defined(MZ800EMU_CFG_MCP_TCP_ENABLED) && !defined(MZ800EMU_MCP_TEST_BUILD)
    prof_str = mcp_config_profile_str(g_mcp_config.profile);
    if (!prof_str) prof_str = "wild";
#endif
    json_object_set_string_member(resp, "profile", prof_str);

    /* Capabilities derived per profile - V1.D.1 statický derived list.
     * Pro WILD vrátíme všechna (= dev default), pro OBSERVER jen read. */
    JsonArray *caps = json_array_new();
    if (strcmp(prof_str, "observer") == 0) {
        json_array_add_string_element(caps, "read_state");
        json_array_add_string_element(caps, "read_memory");
        json_array_add_string_element(caps, "read_resources");
    } else {
        json_array_add_string_element(caps, "read_state");
        json_array_add_string_element(caps, "read_memory");
        json_array_add_string_element(caps, "read_resources");
        json_array_add_string_element(caps, "write_memory");
        json_array_add_string_element(caps, "control_emu");
        json_array_add_string_element(caps, "input_inject");
        json_array_add_string_element(caps, "media_load");
        json_array_add_string_element(caps, "snapshot");
    }
    json_object_set_array_member(resp, "capabilities", caps);

    /* file_access_paths - V0.B.4 nepoužívá whitelist, jen note. */
    JsonArray *paths = json_array_new();
    json_object_set_array_member(resp, "file_access_paths", paths);

    /* auth - V1.D.1 vždy {required: false}. Plný auth je V2. */
    JsonObject *auth = json_object_new();
    json_object_set_boolean_member(auth, "required", FALSE);
    json_object_set_object_member(resp, "auth", auth);

    json_object_set_string_member(resp, "enforcement_note",
        "V0.B.4 stores profile only - full enforcement (whitelist, "
        "capability gating, auth) deferred to V2.");
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_memory_map` handler - per-platform banking snapshot.
 *
 * Forwarduje na DBGAPI_CMD_GET_MEMORY_MAP a serializuje 16 × 4 KB slotů.
 *
 * Response: `{"platform": "mz800|...", "mode_note": "...", "slots": [...]}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_memory_map(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_MEMORY_MAP_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_MEMORY_MAP, &param, NULL)) {
        return _err_response(req_id, "get_memory_map failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "platform",  param.platform);
    json_object_set_string_member(resp, "mode_note", param.mode_note);
    JsonArray *arr = json_array_new();
    for (int i = 0; i < param.slot_count; i++) {
        const st_DBGAPI_MEMORY_MAP_SLOT *s = &param.slots[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "addr_start",  (gint64)s->addr_start);
        json_object_set_int_member(item, "addr_end",    (gint64)s->addr_end);
        json_object_set_string_member(item, "source",
                                       _memory_source_str(s->source));
        json_object_set_string_member(item, "ro_rw",
                                       s->ro_rw ? "rw" : "r");
        json_object_set_int_member(item, "slot_offset",
                                    (gint64)s->slot_offset);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "slots", arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_memext_info` handler - Memory expansion adapter info.
 *
 * Forwarduje na DBGAPI_CMD_GET_MEMEXT_INFO; pokud memext odpojen, vrátí
 * type="none" + connected=false.
 *
 * Response: `{"type": str, "connected": bool, "ram_banks": int,
 *             "ram_bank_size": int, "flash_banks": int|null,
 *             "flash_bank_size": int|null, "current_map": [16],
 *             "map_available": bool}`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_memext_info(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_MEMEXT_INFO_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_MEMEXT_INFO, &param, NULL)) {
        return _err_response(req_id, "get_memext_info failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp,  "type",      param.type);
    json_object_set_boolean_member(resp, "connected",
                                    param.connected ? TRUE : FALSE);
    json_object_set_int_member(resp, "ram_banks",     (gint64)param.ram_banks);
    json_object_set_int_member(resp, "ram_bank_size", (gint64)param.ram_bank_size);
    if (param.flash_banks > 0) {
        json_object_set_int_member(resp, "flash_banks",
                                    (gint64)param.flash_banks);
        json_object_set_int_member(resp, "flash_bank_size",
                                    (gint64)param.flash_bank_size);
    } else {
        json_object_set_null_member(resp, "flash_banks");
        json_object_set_null_member(resp, "flash_bank_size");
    }
    JsonArray *map = json_array_new();
    if (param.map_available) {
        for (int i = 0; i < 16; i++) {
            json_array_add_int_element(map, (gint64)param.current_map[i]);
        }
    }
    json_object_set_array_member(resp, "current_map", map);
    json_object_set_boolean_member(resp, "map_available",
                                    param.map_available ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/* ==================================================================== */
/* BACKLOG D - emulation speed control (get_speed + set_speed)           */
/*                                                                       */
/* AI klient potřebuje ovládat tempo emulace (= warp pro rychlý boot /   */
/* load / dlouhý výpočet, pak zpět na 100 %). Speed funkce běží na emu   */
/* vlákně, proto přes _submit_dbgapi (= CMDRQ na emu thread).            */
/* ==================================================================== */

/**
 * @brief `get_speed` handler - read-only snapshot emulační rychlosti.
 *
 * Proxy na `DBGAPI_CMD_GET_SPEED`. Vrací JSON
 * `{current_percent, max_speed, mode, status}`. Žádný side effect na
 * emu stavu.
 */
static en_MCP_DISPATCH_RESULT _handle_get_speed(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_GET_SPEED_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_SPEED, &param, NULL)) {
        return _err_response(req_id, "get_speed failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "current_percent",
                               (gint64)param.current_percent);
    json_object_set_boolean_member(resp, "max_speed",
                                   param.max_speed ? TRUE : FALSE);
    json_object_set_string_member(resp, "mode", param.mode);
    json_object_set_string_member(resp, "status", param.status);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `set_speed` handler - nastaví emulační rychlost dle mode.
 *
 * Proxy na `DBGAPI_CMD_SET_SPEED`. Vstupní JSON pole:
 *   - `mode` (string, povinné): "normal" / "custom" / "max" / "step"
 *   - `percent` (int, default 100): cílové % pro mode=custom (1..4000)
 *   - `step` (int, default 0): relativní delta pro mode=step
 *
 * Po úspěchu vrací echo aktuálního stavu
 * `{ok, mode, current_percent, max_speed}`. mode=max zapne warp;
 * mode=normal/custom warp vypne; mode=step jen mění custom %.
 *
 * Viditelná akce: emitne MCP_ACTION broadcast (Activity log).
 */
static en_MCP_DISPATCH_RESULT _handle_set_speed(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: mode",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *mode_str = _obj_str_dup(data_obj, "mode");
    if (!mode_str || !mode_str[0]) {
        g_free(mode_str);
        return _err_response(req_id, "Missing required field: mode",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_SET_SPEED_PARAM param;
    memset(&param, 0, sizeof(param));
    param.percent = (int)_obj_int_or(data_obj, "percent", 100);
    param.step    = (int)_obj_int_or(data_obj, "step", 0);

    if (strcmp(mode_str, "normal") == 0)      param.mode = DBGAPI_SPEED_MODE_NORMAL;
    else if (strcmp(mode_str, "custom") == 0) param.mode = DBGAPI_SPEED_MODE_CUSTOM;
    else if (strcmp(mode_str, "max") == 0)    param.mode = DBGAPI_SPEED_MODE_MAX;
    else if (strcmp(mode_str, "step") == 0)   param.mode = DBGAPI_SPEED_MODE_STEP;
    else {
        g_free(mode_str);
        return _err_response(req_id,
                             "Invalid mode (allowed: normal, custom, "
                             "max, step)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    g_free(mode_str);

    if (!_submit_dbgapi(DBGAPI_CMD_SET_SPEED, &param, NULL)) {
        return _err_response(req_id, "set_speed failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "mode", param.out_mode);
    json_object_set_int_member(resp, "current_percent",
                               (gint64)param.out_current_percent);
    json_object_set_boolean_member(resp, "max_speed",
                                   param.out_max_speed ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/* ==================================================================== */
/* CMT-A - CMT transport + recording + cmthack toggle                    */
/*                                                                       */
/* Oddělení konceptů: cmt_transport / cmt_record ovládají reálnou        */
/* páskovou emulaci (= stavový automat CMT), cmt_hack_set zapíná/vypíná  */
/* okrajový cmthack ROM-patch instant-load (= obejde reálnou pásku).     */
/* Všechny tři jsou mutující -> backend emituje MCP_ACTION broadcast     */
/* pro origin == MCP (Activity log) automaticky.                         */
/* ==================================================================== */

/**
 * @brief `cmt_transport` handler - ovládání transportu reálné pásky.
 *
 * Forwarduje na DBGAPI_CMD_CMT_TRANSPORT. Akce se předává jako string
 * `action` (play/play_paused/stop/pause/eject). Pro `pause` lze navíc
 * předat bool `pause` (= true pauznout, false odpauzovat; default true).
 *
 * Layout response: {"ok": true, "action": str}
 */
static en_MCP_DISPATCH_RESULT _handle_cmt_transport(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: action",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *action = _obj_str_dup(data_obj, "action");
    if (!action || !action[0]) {
        g_free(action);
        return _err_response(req_id, "Missing required field: action",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_CMT_TRANSPORT_PARAM param;
    memset(&param, 0, sizeof(param));
    if (strcmp(action, "play") == 0) {
        param.action = DBGAPI_CMT_TRANSPORT_PLAY;
    } else if (strcmp(action, "play_paused") == 0) {
        param.action = DBGAPI_CMT_TRANSPORT_PLAY_PAUSED;
    } else if (strcmp(action, "stop") == 0) {
        param.action = DBGAPI_CMT_TRANSPORT_STOP;
    } else if (strcmp(action, "pause") == 0) {
        param.action = DBGAPI_CMT_TRANSPORT_PAUSE;
        /* pause flag: bool nebo int 0/1, default 1 (pauznout). */
        param.pause_value = (uint8_t)(_obj_int_or(data_obj, "pause", 1) ? 1 : 0);
    } else if (strcmp(action, "eject") == 0) {
        param.action = DBGAPI_CMT_TRANSPORT_EJECT;
    } else {
        g_free(action);
        return _err_response(req_id,
                             "Invalid action (allowed: play, play_paused, "
                             "stop, pause, eject)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    if (!_submit_dbgapi(DBGAPI_CMD_CMT_TRANSPORT, &param, NULL)) {
        g_free(action);
        return _err_response(req_id, "cmt_transport failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "action", action);
    g_free(action);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `cmt_record` handler - zahájení WAV nahrávání do souboru.
 *
 * Forwarduje na DBGAPI_CMD_CMT_RECORD. Vyžaduje string `path` (cílový
 * WAV soubor). Nahrávání startuje v pauze (= klient musí následně
 * cmt_transport pause=false pro reálný zápis). Nezapisovatelná cesta
 * nebo špatný stav CMT (= není STOP) -> success=false.
 *
 * Layout response: {"ok": true, "path": str}
 */
static en_MCP_DISPATCH_RESULT _handle_cmt_record(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *path = _obj_str_dup(data_obj, "path");
    if (!path || !path[0]) {
        g_free(path);
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_CMT_RECORD_PARAM param;
    memset(&param, 0, sizeof(param));
    param.filepath = path;
    if (!_submit_dbgapi(DBGAPI_CMD_CMT_RECORD, &param, NULL)) {
        g_free(path);
        return _err_response(req_id,
                             "cmt_record failed (bad state or path not "
                             "writable)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "path", path);
    g_free(path);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `cmt_hack_set` handler - zapnutí/vypnutí cmthack ROM patche.
 *
 * Forwarduje na DBGAPI_CMD_CMT_HACK_SET. Vyžaduje bool `enabled`.
 * Response echo skutečného stavu patche po operaci (`installed`).
 *
 * Layout response: {"ok": true, "installed": bool}
 */
static en_MCP_DISPATCH_RESULT _handle_cmt_hack_set(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: enabled",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "enabled")) {
        return _err_response(req_id, "Missing required field: enabled",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_CMT_HACK_SET_PARAM param;
    memset(&param, 0, sizeof(param));
    param.enabled = (uint8_t)(_obj_int_or(data_obj, "enabled", 0) ? 1 : 0);
    if (!_submit_dbgapi(DBGAPI_CMD_CMT_HACK_SET, &param, NULL)) {
        return _err_response(req_id, "cmt_hack_set failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_boolean_member(resp, "installed",
                                   param.out_installed ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/* ==================================================================== */
/* CMT-B - CMT vlastnosti + práce s páskou                               */
/*                                                                       */
/* Vlastnosti (set_property) a páskové operace (open/seek/block_speed)   */
/* jsou mutující -> backend emituje MCP_ACTION broadcast pro origin ==   */
/* MCP (Activity log) automaticky. cmt_tape_list je read-only backing    */
/* pro resource emulator://periph/cmt/tape; prochází stejným broadcast   */
/* hookem jako ostatní read get_periph_* cmd (= existující chování, ne   */
/* CMT-B specifikum).                                                    */
/* ==================================================================== */

/**
 * @brief `cmt_set_property` handler - nastavení jedné vlastnosti CMT.
 *
 * Forwarduje na DBGAPI_CMD_CMT_SET_PROPERTY. Vstupní JSON pole:
 *   - `property` (string, povinné): "speed" / "polarity" / "cpu_boost" /
 *     "mzfsize_check"
 *   - `value` (int, povinné): pro "speed" en_CMTSPEED hodnota 1..9,
 *     pro ostatní boolean 0/1.
 *
 * Layout response: {"ok": true, "property": str, "value": int}
 */
static en_MCP_DISPATCH_RESULT _handle_cmt_set_property(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: property",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *property = _obj_str_dup(data_obj, "property");
    if (!property || !property[0]) {
        g_free(property);
        return _err_response(req_id, "Missing required field: property",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!json_object_has_member(data_obj, "value")) {
        g_free(property);
        return _err_response(req_id, "Missing required field: value",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_CMT_SET_PROPERTY_PARAM param;
    memset(&param, 0, sizeof(param));
    param.value = (int)_obj_int_or(data_obj, "value", 0);
    if (strcmp(property, "speed") == 0) {
        param.property = DBGAPI_CMT_PROP_SPEED;
    } else if (strcmp(property, "polarity") == 0) {
        param.property = DBGAPI_CMT_PROP_POLARITY;
    } else if (strcmp(property, "cpu_boost") == 0) {
        param.property = DBGAPI_CMT_PROP_CPU_BOOST;
    } else if (strcmp(property, "mzfsize_check") == 0) {
        param.property = DBGAPI_CMT_PROP_MZFSIZE_CHECK;
    } else {
        g_free(property);
        return _err_response(req_id,
                             "Invalid property (allowed: speed, polarity, "
                             "cpu_boost, mzfsize_check)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    if (!_submit_dbgapi(DBGAPI_CMD_CMT_SET_PROPERTY, &param, NULL)) {
        g_free(property);
        return _err_response(req_id,
                             "cmt_set_property failed (invalid value)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "property", property);
    json_object_set_int_member(resp, "value", (gint64)param.value);
    g_free(property);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `cmt_open` handler - otevření CMT souboru s volitelným play.
 *
 * Forwarduje na DBGAPI_CMD_CMT_OPEN. Vstupní JSON pole:
 *   - `path` (string, povinné): cesta k CMT souboru.
 *   - `play_immediately` (bool, default false): po openu spustit play.
 *
 * Layout response: {"ok": true, "path": str, "playing": bool}
 */
static en_MCP_DISPATCH_RESULT _handle_cmt_open(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *path = _obj_str_dup(data_obj, "path");
    if (!path || !path[0]) {
        g_free(path);
        return _err_response(req_id, "Missing required field: path",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_CMT_OPEN_PARAM param;
    memset(&param, 0, sizeof(param));
    param.filepath = path;
    param.play_immediately =
        (uint8_t)(_obj_int_or(data_obj, "play_immediately", 0) ? 1 : 0);
    if (!_submit_dbgapi(DBGAPI_CMD_CMT_OPEN, &param, NULL)) {
        g_free(path);
        return _err_response(req_id,
                             "cmt_open failed (unknown extension or open "
                             "error)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "path", path);
    json_object_set_boolean_member(resp, "playing",
                                   param.play_immediately ? TRUE : FALSE);
    g_free(path);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `cmt_tape_seek` handler - seek na blok pásky.
 *
 * Forwarduje na DBGAPI_CMD_CMT_TAPE_SEEK. Vyžaduje int `block_id`.
 * Bez naložené pásky nebo neplatný blok -> success = false.
 *
 * Layout response: {"ok": true, "block_id": int}
 */
static en_MCP_DISPATCH_RESULT _handle_cmt_tape_seek(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: block_id",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "block_id")) {
        return _err_response(req_id, "Missing required field: block_id",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_CMT_TAPE_SEEK_PARAM param;
    memset(&param, 0, sizeof(param));
    param.block_id = (int)_obj_int_or(data_obj, "block_id", 0);
    if (!_submit_dbgapi(DBGAPI_CMD_CMT_TAPE_SEEK, &param, NULL)) {
        return _err_response(req_id,
                             "cmt_tape_seek failed (no tape or bad block)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "block_id", (gint64)param.block_id);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `cmt_tape_block_speed` handler - per-blok cmt rychlost.
 *
 * Forwarduje na DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED. Vyžaduje int `block_id`
 * a int `speed` (en_CMTSPEED 1..9). Bez pásky nebo neplatná rychlost ->
 * success = false.
 *
 * Layout response: {"ok": true, "block_id": int, "speed": int}
 */
static en_MCP_DISPATCH_RESULT _handle_cmt_tape_block_speed(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required fields: block_id, "
                             "speed", MCP_DISPATCH_INVALID_PARAMS,
                             out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    if (!json_object_has_member(data_obj, "block_id")
        || !json_object_has_member(data_obj, "speed")) {
        return _err_response(req_id, "Missing required fields: block_id, "
                             "speed", MCP_DISPATCH_INVALID_PARAMS,
                             out_response);
    }

    st_DBGAPI_CMT_TAPE_BLOCK_SPEED_PARAM param;
    memset(&param, 0, sizeof(param));
    param.block_id = (int)_obj_int_or(data_obj, "block_id", 0);
    param.cmtspeed = (int)_obj_int_or(data_obj, "speed", 0);
    if (!_submit_dbgapi(DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED, &param, NULL)) {
        return _err_response(req_id,
                             "cmt_tape_block_speed failed (no tape or bad "
                             "speed)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "block_id", (gint64)param.block_id);
    json_object_set_int_member(resp, "speed", (gint64)param.cmtspeed);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `cmt_tape_list` handler - read-only výpis bloků pásky.
 *
 * Forwarduje na DBGAPI_CMD_CMT_TAPE_LIST. Backing pro resource
 * emulator://periph/cmt/tape. Caller alokuje pole bloků (cap
 * MCP_CMT_TAPE_LIST_CAP). Response payload:
 *   {"available": bool, "container_type": int, "current_block": int,
 *    "count": int, "truncated": bool, "blocks": [
 *      {"block_id", "name", "cmt_speed", "type", "is_current",
 *       "playable", "recordable"} ... ]}
 *
 * `available=false` znamená nenaloženou pásku nebo chybějící container;
 * blocks je pak prázdné. Read-only (= nemění stav), ale prochází stejným
 * MCP_ACTION broadcast hookem jako ostatní read get_periph_* cmd.
 */
#define MCP_CMT_TAPE_LIST_CAP 256
static en_MCP_DISPATCH_RESULT _handle_cmt_tape_list(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_CMT_TAPE_BLOCK_ENTRY *entries =
        g_new0(st_DBGAPI_CMT_TAPE_BLOCK_ENTRY, MCP_CMT_TAPE_LIST_CAP);
    st_DBGAPI_CMT_TAPE_LIST_PARAM param;
    memset(&param, 0, sizeof(param));
    param.entries  = entries;
    param.capacity = MCP_CMT_TAPE_LIST_CAP;
    if (!_submit_dbgapi(DBGAPI_CMD_CMT_TAPE_LIST, &param, NULL)) {
        g_free(entries);
        return _err_response(req_id, "cmt_tape_list failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "available",
                                   param.available ? TRUE : FALSE);
    json_object_set_int_member(resp, "container_type",
                               (gint64)param.container_type);
    json_object_set_int_member(resp, "current_block",
                               (gint64)param.current_block);
    json_object_set_int_member(resp, "count", (gint64)param.out_count);
    json_object_set_boolean_member(resp, "truncated",
        (param.out_count >= MCP_CMT_TAPE_LIST_CAP) ? TRUE : FALSE);
    JsonArray *arr = json_array_new();
    for (size_t i = 0; i < param.out_count; i++) {
        const st_DBGAPI_CMT_TAPE_BLOCK_ENTRY *e = &entries[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "block_id", (gint64)e->block_id);
        json_object_set_string_member(item, "name", e->name);
        json_object_set_int_member(item, "cmt_speed", (gint64)e->cmtspeed);
        json_object_set_int_member(item, "type", (gint64)e->type);
        json_object_set_boolean_member(item, "is_current",
                                       e->is_current ? TRUE : FALSE);
        json_object_set_boolean_member(item, "playable",
                                       e->playable ? TRUE : FALSE);
        json_object_set_boolean_member(item, "recordable",
                                       e->recordable ? TRUE : FALSE);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "blocks", arr);
    g_free(entries);
    return _ok_response(req_id, resp, out_response);
}


/* ==================================================================== */
/* V1.D.2.A - Easy reuse Resources (5 read-only handlers)                */
/*                                                                       */
/* Všech 5 handlerů reuse-uje existující DBGAPI_CMD wrappery z V1.A      */
/* fáze (V1.A.6 callstack, V1.A.7 profiler, V1.A.2 sym, plus             */
/* stack_history / stack_regions). Žádný nový enum, žádný refactor       */
/* backend modulů - V1.D.2.A je čistě tenký JSON serializér nad          */
/* hotovými DBGAPI payload strukturami.                                  */
/*                                                                       */
/* Threading: handler běží v MCP I/O vlákně (dispatch). Skrz             */
/* `_submit_dbgapi` (= dbgapi_ui_submit_cmd_sync_with_origin) se         */
/* dotaz bouncuje na EMU vlákno safe-point; výsledná payload pak         */
/* serializuje JSON v dispatch vlákně. Kontrakt ownership pro pole       */
/* entries / samples kopíruje dosavadní _handle_callstack_get /          */
/* _handle_profiler_get / _handle_symbol_list pattern.                    */
/* ==================================================================== */


/**
 * @brief Překlad `en_SYM_SOURCE` na MCP wire string.
 *
 * Mapování (= per sym_db.h):
 *   0 SYM_SOURCE_SJASMPLUS -> "sym_file"
 *   1 SYM_SOURCE_NOI       -> "noi_file"
 *   2 SYM_SOURCE_MAP       -> "map_file"
 *   3 SYM_SOURCE_LBL       -> "user"
 * Jiné hodnoty -> "unknown" (defenzivně, current sym_db enum jen 0-3).
 *
 * @param source Hodnota `st_DBGAPI_SYMBOL_ENTRY::source` (uint8_t).
 * @return Konstantní C string, nikdy NULL.
 */
static const char *_sym_source_str(uint8_t source) {
    switch (source) {
        case 0: return "sym_file";
        case 1: return "noi_file";
        case 2: return "map_file";
        case 3: return "user";
        default: return "unknown";
    }
}


/**
 * @brief `get_callstack` handler - V1.D.2.A Resource backing pro
 *        `emulator://callstack`.
 *
 * Forwarduje na DBGAPI_CMD_GET_CALLSTACK (V1.A.6 wrapper) a JSON-uje
 * st_CALLSTACK_ENTRY pole + agregované statistiky. Bez parametrů
 * (Resource semantika) - vrací plný shadow stack jak je. Limit max
 * délky pole je dán CALLSTACK_MAX_DEPTH (256) v callstack.h, takže
 * payload se nikdy nestane neúměrně velkým.
 *
 * Layout response:
 *   {
 *     "active": bool,                    // callstack subsystem aktivní?
 *     "current_depth": int,              // aktuální hloubka shadow
 *     "max_depth_reached": int,
 *     "divergence_count": int,           // total = trampoline+longjmp+mismatch
 *     "overflow_count": int,
 *     "cycles_now": int,
 *     "count": int,                      // počet frames v poli
 *     "frames": [                        // top-of-stack first (depth=0)
 *       {"depth": int, "return_addr": int, "call_site_addr": int,
 *        "target_addr": int, "sp_at_entry": int, "cycles_at_entry": int,
 *        "kind": "call|rst|irq_im0|irq_im1|irq_im2|nmi|synthetic"}
 *     ]
 *   }
 *
 * Ownership: param.entries je callee-allocated (= g_malloc v V1.A.6
 * handleru); po serializaci ho uvolňujeme přes callstack_snapshot_free
 * (shim alias na g_free v test buildu, viz V1.A.6 pattern).
 *
 * Pole `scope_state` (briefem zmiňované) v st_DBGAPI_CALLSTACK_GET_PARAM
 * neexistuje (V1 single-shadow nemá per-scope BP). V1.D.2.A vystavuje
 * jen `active` + statistiky; full scope payload by vyžadoval nový
 * dbgapi enum (= V1.D.2.B / pozdější).
 */
static en_MCP_DISPATCH_RESULT _handle_get_callstack(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_CALLSTACK_GET_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_CALLSTACK, &param, NULL)) {
        if (param.entries) {
            callstack_snapshot_free((st_CALLSTACK_ENTRY *)param.entries);
        }
        return _err_response(req_id, "get_callstack failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active",
                                    param.active ? TRUE : FALSE);
    json_object_set_int_member(resp, "current_depth",
                                (gint64)param.current_depth);
    json_object_set_int_member(resp, "max_depth_reached",
                                (gint64)param.max_depth_reached);
    json_object_set_int_member(resp, "divergence_count",
                                (gint64)param.divergence_count);
    json_object_set_int_member(resp, "overflow_count",
                                (gint64)param.overflow_count);
    json_object_set_int_member(resp, "cycles_now",
                                (gint64)param.cycles_now);
    /* count = total emit (= reuse pattern _handle_callstack_get). */
    int total = param.count;
    json_object_set_int_member(resp, "count", (gint64)total);
    JsonArray *arr = json_array_new();
    st_CALLSTACK_ENTRY *entries = (st_CALLSTACK_ENTRY *)param.entries;
    for (int i = 0; i < total; i++) {
        /* entries[total-1] = top stacku (depth=0). */
        st_CALLSTACK_ENTRY *e = &entries[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "depth",
                                    (gint64)(total - 1 - i));
        json_object_set_int_member(item, "return_addr",
                                    (gint64)e->return_addr);
        json_object_set_int_member(item, "call_site_addr",
                                    (gint64)e->call_site_addr);
        json_object_set_int_member(item, "target_addr",
                                    (gint64)e->target_addr);
        json_object_set_int_member(item, "sp_at_entry",
                                    (gint64)e->sp_at_entry);
        json_object_set_int_member(item, "cycles_at_entry",
                                    (gint64)e->cycles_at_entry);
        json_object_set_string_member(item, "kind",
                                       _cs_kind_to_str(e->kind));
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "frames", arr);
    if (entries) {
        callstack_snapshot_free(entries);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_profiler` handler - V1.D.2.A Resource backing pro
 *        `emulator://profiler`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PROFILER (V1.A.7 wrapper). Bez parametrů.
 * Layout shoduje se s `profiler_get` Tool ale `limit` interně ne-cap
 * (= Resource vrátí všechny entries z hash mapy). Pokud klient chce
 * paginaci, použije `profiler_get` Tool s explicit `limit` argumentem.
 *
 * Layout response:
 *   {
 *     "active": bool,
 *     "entry_count": int,                // počet unique (target,kind) bucketů
 *     "total_cycles_64": int,
 *     "total_calls": int,
 *     "irq_entries": int,
 *     "unmatched_returns": int,
 *     "max_depth_reached": int,
 *     "overflow_count": int,
 *     "entries": [
 *       {"addr": int, "kind": "call|rst|irq_im*|nmi", "calls": int,
 *        "excl_cycles": int, "incl_cycles": int,
 *        "min_cycles": int, "max_cycles": int, "avg_cycles": int}
 *     ]
 *   }
 *
 * Brief pole `name`, `last_call_frame`, `since_frame`, `sample_count`
 * v st_PROF_ENTRY / st_PROF_STATS neexistují - V1.D.2.A je nevystavuje.
 * Symbol-decoded `name` by vyžadoval sym_db lookup-by-addr per entry,
 * což je threading-fragile (= sym_db je UI-vlákno only); ponecháno pro
 * pozdější fázi až bude EMU-side mirror sym tabulky.
 *
 * Ownership: param.entries je callee-allocated (g_malloc), uvolňujeme
 * přes g_free (kontrakt totožný s _handle_profiler_get).
 */
static en_MCP_DISPATCH_RESULT _handle_get_profiler(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PROFILER_GET_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PROFILER, &param, NULL)) {
        if (param.entries) {
            g_free(param.entries);
        }
        return _err_response(req_id, "get_profiler failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active",
                                    param.active ? TRUE : FALSE);
    json_object_set_int_member(resp, "entry_count",
                                (gint64)param.entry_count);
    json_object_set_int_member(resp, "total_cycles_64",
                                (gint64)param.total_cycles_64);
    json_object_set_int_member(resp, "total_calls",
                                (gint64)param.total_calls);
    json_object_set_int_member(resp, "irq_entries",
                                (gint64)param.irq_entries);
    json_object_set_int_member(resp, "unmatched_returns",
                                (gint64)param.unmatched_returns);
    json_object_set_int_member(resp, "max_depth_reached",
                                (gint64)param.max_depth_reached);
    json_object_set_int_member(resp, "overflow_count",
                                (gint64)param.overflow_count);
    JsonArray *arr = json_array_new();
    const st_MCP_PROF_ENTRY_MIRROR *entries =
        (const st_MCP_PROF_ENTRY_MIRROR *)param.entries;
    for (int i = 0; i < param.entry_count; i++) {
        const st_MCP_PROF_ENTRY_MIRROR *e = &entries[i];
        uint64_t avg = e->calls ? (e->cycles_incl_sum / e->calls) : 0;
        uint64_t mn  = (e->cycles_incl_min == UINT64_MAX) ? 0
                                                           : e->cycles_incl_min;
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "addr",
                                    (gint64)e->target_addr);
        json_object_set_string_member(item, "kind",
                                       _prof_kind_to_str(e->kind));
        json_object_set_int_member(item, "calls", (gint64)e->calls);
        json_object_set_int_member(item, "excl_cycles",
                                    (gint64)e->cycles_excl_sum);
        json_object_set_int_member(item, "incl_cycles",
                                    (gint64)e->cycles_incl_sum);
        json_object_set_int_member(item, "min_cycles", (gint64)mn);
        json_object_set_int_member(item, "max_cycles",
                                    (gint64)e->cycles_incl_max);
        json_object_set_int_member(item, "avg_cycles", (gint64)avg);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "entries", arr);
    if (param.entries) {
        g_free(param.entries);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_symbols` handler - V1.D.2.A Resource backing pro
 *        `emulator://symbols`.
 *
 * Forwarduje na DBGAPI_CMD_SYMBOL_LIST (V1.A.2 wrapper) se size capem
 * 10000 záznamů. Bez parametrů (Resource semantika) - vrátí celou sym_db.
 * Při překročení capu `truncated` = true a klient pošle `symbol_list`
 * Tool s explicit `prefix` filterem (= existující paginační cesta).
 *
 * Layout response:
 *   {
 *     "count": int,                  // počet vrácených symbolů
 *     "truncated": bool,             // true pokud sym_db > MAX
 *     "max_returned": int,           // = MAX cap (= 10000)
 *     "symbols": [
 *       {"addr": int, "name": str, "comment": str,
 *        "source": "user|map_file|noi_file|sym_file"}
 *     ]
 *   }
 *
 * Pole `kind` (briefem požadované jako "function|data|label") v sym_db
 * neexistuje - každý symbol je v V1 vždy label. V1.D.2.A pole vynechává
 * (= pozdější rozšíření vyžaduje sym_db schema change).
 *
 * Pole `owner` (V1.C.3 cmd_origin) v st_DBGAPI_SYMBOL_ENTRY rovněž
 * neexistuje (= per-entry origin tracking je deferred bod V1.D.2.C).
 * V1.D.2.A pole vynechává.
 *
 * Ownership: handler alokuje pole st_DBGAPI_SYMBOL_ENTRY, dbgapi heap-
 * duplikuje name/comment stringy; uvolnění přes g_free per řádek.
 */
#define MCP_GET_SYMBOLS_MAX 10000
static en_MCP_DISPATCH_RESULT _handle_get_symbols(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    size_t cap = MCP_GET_SYMBOLS_MAX;
    st_DBGAPI_SYMBOL_ENTRY *entries =
        g_new0(st_DBGAPI_SYMBOL_ENTRY, cap);
    st_DBGAPI_SYMBOL_PARAM param = {
        .addr        = 0,
        .name        = NULL,
        .comment     = NULL,
        .prefix      = NULL,    /* prázdný filter = vše */
        .out_entries = entries,
        .out_max     = cap,
        .out_count   = 0,
        .source      = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_SYMBOL_LIST, &param, NULL)) {
        for (size_t i = 0; i < cap; i++) {
            g_free(entries[i].name);
            g_free(entries[i].comment);
        }
        g_free(entries);
        return _err_response(req_id, "get_symbols failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", (gint64)param.out_count);
    /* sym_db API nevrací total count přes prefix=NULL+out_max cap, takže
     * `truncated` je true jen pokud out_count == cap (= heuristika, ne
     * exact). Klient s tím počítá - pokud potřebuje přesný total, volá
     * `symbol_list` Tool s explicit prefix. */
    bool truncated = (param.out_count >= cap);
    json_object_set_boolean_member(resp, "truncated",
                                    truncated ? TRUE : FALSE);
    json_object_set_int_member(resp, "max_returned", (gint64)cap);
    JsonArray *arr = json_array_new();
    for (size_t i = 0; i < param.out_count; i++) {
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "addr",
                                    (gint64)entries[i].addr);
        json_object_set_string_member(item, "name",
            entries[i].name ? entries[i].name : "");
        json_object_set_string_member(item, "comment",
            entries[i].comment ? entries[i].comment : "");
        json_object_set_string_member(item, "source",
                                       _sym_source_str(entries[i].source));
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "symbols", arr);

    for (size_t i = 0; i < cap; i++) {
        g_free(entries[i].name);
        g_free(entries[i].comment);
    }
    g_free(entries);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_stack_history` handler - V1.D.2.A Resource backing pro
 *        `emulator://stack/history`.
 *
 * Forwarduje na DBGAPI_CMD_STACK_HISTORY_GET. Bez parametrů: handler
 * alokuje samples pole na max_count = DBGAPI_STACK_HISTORY_MAX (= 4096
 * dle dbgapi_cmdrq.h) a fixní slope_window = 256 (= identické s UI
 * stack_history_window defaultem).
 *
 * Layout response:
 *   {
 *     "enabled": bool,                // recording aktivní?
 *     "count": int,                   // počet samplů v poli (0..max)
 *     "slope_window": int,            // 256
 *     "slope": float,                 // SP/cycle (0 pokud nedostatek dat)
 *     "samples": [                    // oldest -> newest
 *       {"cycles": int, "sp": int}
 *     ]
 *   }
 *
 * Pole `op` (briefem zmiňované: "push|pop|call|ret|ret_taken") v
 * st_DBGAPI_STACK_HISTORY_SAMPLE neexistuje (= sample je raw {cycles,
 * sp} pair, není op klasifikace v ring bufferu). V1.D.2.A pole vynechává.
 *
 * Pokud `active == 0`, vrátí `enabled: false` + prázdné samples
 * (= ring buffer mohl mít data před vypnutím, ale stub semantika briefu
 * říká empty array pro disabled state).
 *
 * Ownership: samples pole alokujeme my, uvolňujeme my (g_free).
 */
static en_MCP_DISPATCH_RESULT _handle_get_stack_history(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_STACK_HISTORY_GET_PARAM param;
    memset(&param, 0, sizeof(param));
    param.max_count    = DBGAPI_STACK_HISTORY_MAX;
    param.slope_window = 256;
    param.samples = g_new0(st_DBGAPI_STACK_HISTORY_SAMPLE,
                            DBGAPI_STACK_HISTORY_MAX);
    if (!_submit_dbgapi(DBGAPI_CMD_STACK_HISTORY_GET, &param, NULL)) {
        g_free(param.samples);
        return _err_response(req_id, "get_stack_history failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "enabled",
                                    param.active ? TRUE : FALSE);
    json_object_set_int_member(resp, "slope_window",
                                (gint64)param.slope_window);
    json_object_set_double_member(resp, "slope", (gdouble)param.slope);
    JsonArray *arr = json_array_new();
    if (param.active) {
        uint32_t cnt = param.count;
        if (cnt > DBGAPI_STACK_HISTORY_MAX) cnt = DBGAPI_STACK_HISTORY_MAX;
        for (uint32_t i = 0; i < cnt; i++) {
            JsonObject *item = json_object_new();
            json_object_set_int_member(item, "cycles",
                                        (gint64)param.samples[i].cycles);
            json_object_set_int_member(item, "sp",
                                        (gint64)param.samples[i].sp);
            json_array_add_object_element(arr, item);
        }
        json_object_set_int_member(resp, "count", (gint64)cnt);
    } else {
        /* Disabled: prázdné pole + count = 0 (= brief acceptance). */
        json_object_set_int_member(resp, "count", 0);
    }
    json_object_set_array_member(resp, "samples", arr);
    g_free(param.samples);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_stack_regions` handler - V1.D.2.A Resource backing pro
 *        `emulator://stack/regions`.
 *
 * Forwarduje na DBGAPI_CMD_STACK_REGIONS_LIST. Vrátí všechny aktuálně
 * definované stack regions (max DBGAPI_STACK_REGIONS_MAX = 8).
 *
 * Layout response:
 *   {
 *     "count": int,                   // počet regions
 *     "sp_now": int,                  // aktuální SP v okamžiku snapshotu
 *     "regions": [
 *       {"name": str, "base": int, "limit": int, "watermark": int,
 *        "push_count": int, "pop_count": int,
 *        "current_sp_in_region": bool}
 *     ]
 *   }
 *
 * Pole `first_seen_cycle / last_seen_cycle / last_r_pc / last_w_pc`
 * (briefem zmiňované) v st_DBGAPI_STACK_REGION_INFO neexistují. V1 API
 * drží watermark + push/pop counters; per-cycle/per-PC tracking by
 * vyžadoval nový backend field (= V1.D.2.B+).
 */
static en_MCP_DISPATCH_RESULT _handle_get_stack_regions(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_STACK_REGIONS_LIST_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_STACK_REGIONS_LIST, &param, NULL)) {
        return _err_response(req_id, "get_stack_regions failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", (gint64)param.count);
    json_object_set_int_member(resp, "sp_now", (gint64)param.sp_now);
    JsonArray *arr = json_array_new();
    int n = param.count;
    if (n < 0) n = 0;
    if (n > DBGAPI_STACK_REGIONS_MAX) n = DBGAPI_STACK_REGIONS_MAX;
    for (int i = 0; i < n; i++) {
        const st_DBGAPI_STACK_REGION_INFO *r = &param.regions[i];
        JsonObject *item = json_object_new();
        json_object_set_string_member(item, "name", r->name);
        json_object_set_int_member(item, "base", (gint64)r->base);
        json_object_set_int_member(item, "limit", (gint64)r->limit);
        json_object_set_int_member(item, "watermark",
                                    (gint64)r->watermark);
        json_object_set_int_member(item, "push_count",
                                    (gint64)r->push_count);
        json_object_set_int_member(item, "pop_count",
                                    (gint64)r->pop_count);
        json_object_set_boolean_member(item, "current_sp_in_region",
            r->current_sp_in_region ? TRUE : FALSE);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "regions", arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_watch` handler - V1.D.2.B Resource backing pro
 *        `emulator://watch`.
 *
 * Forwarduje na DBGAPI_CMD_WATCH_LIST (V1.A.6 wrapper) se size capem
 * 256 záznamů (= dostatečné pro typický UI panel watch listu).
 *
 * Layout response:
 *   {
 *     "count": int,
 *     "watches": [
 *       {"index": int, "name": str|null, "mode": str, "type": str,
 *        "addr": int, "expr": str|null, "value": str}
 *     ]
 *   }
 *
 * Pole `owner` (= cmd_origin per V1.C.3 storage) je vynecháno - V1.C.3
 * sice doplnilo `cmd_origin` do interní watch_row_t, ale nevystavilo ho
 * v st_DBGAPI_WATCH_LIST_ENTRY. Briefem zakázáno refactor watch API,
 * proto V1.D.2.B vynechává; harvest do V1.D.2.C (= rozšířit
 * WATCH_LIST_ENTRY o cmd_origin pole).
 *
 * Ownership: handler alokuje pole, dbgapi heap-duplikuje name/expr/value
 * stringy, uvolnění per řádek + final g_free pole.
 */
static en_MCP_DISPATCH_RESULT _handle_get_watch(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    const int MAX_ITEMS = 256;
    st_DBGAPI_WATCH_LIST_ENTRY *entries =
        g_new0(st_DBGAPI_WATCH_LIST_ENTRY, MAX_ITEMS);
    st_DBGAPI_WATCH_LIST_PARAM param = {
        .out_entries = entries,
        .out_max     = MAX_ITEMS,
        .out_count   = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_WATCH_LIST, &param, NULL)) {
        g_free(entries);
        return _err_response(req_id, "get_watch failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", (gint64)param.out_count);
    JsonArray *arr = json_array_new();
    for (int i = 0; i < param.out_count; i++) {
        st_DBGAPI_WATCH_LIST_ENTRY *e = &entries[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "index", (gint64)e->index);
        if (e->name) {
            json_object_set_string_member(item, "name", e->name);
        } else {
            json_object_set_null_member(item, "name");
        }
        json_object_set_string_member(item, "mode",
                                       _watch_mode_to_str(e->mode));
        json_object_set_string_member(item, "type",
                                       _watch_type_to_str(e->type));
        json_object_set_int_member(item, "addr", (gint64)e->addr);
        if (e->expr_text) {
            json_object_set_string_member(item, "expr", e->expr_text);
        } else {
            json_object_set_null_member(item, "expr");
        }
        json_object_set_string_member(item, "value",
            e->value_str ? e->value_str : "");
        json_array_add_object_element(arr, item);
        g_free(e->name);
        g_free(e->expr_text);
        g_free(e->value_str);
    }
    json_object_set_array_member(resp, "watches", arr);
    g_free(entries);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_stack` handler - V1.D.2.B Resource backing pro
 *        `emulator://stack`.
 *
 * Forwarduje na DBGAPI_CMD_STACK_DUMP v absolute mode (= lines_above=0,
 * addr je vypočtena z aktuálního SP). Default vrátí 32 slov (= 64 bajtů)
 * od aktuálního SP směrem nahoru (= rostoucí adresy, tj. starší push
 * frames jsou v poli výše).
 *
 * Postup:
 *   1. Submit DBGAPI_CMD_STACK_DUMP s absolute mode = handler vyplní
 *      sp_now do paramu.
 *   2. Po prvním submitu máme reálnou sp_now, ale buffer ještě nemá
 *      data od SP - proto druhý submit s addr=sp_now, lines_above=0.
 *   3. Alternativně lze první submit udělat s len=0 jen pro sp_now,
 *      ale handler ignoruje len=0 jako edge case. Volíme proto
 *      lines_above=1 trik (= base = sp + 2), pak ASC dump z sp_now.
 *
 * Zjednodušená implementace: jeden submit v absolute mode s addr=0 +
 * len=0 by handler odmítl. Místo toho zavoláme jednou s
 * lines_above=N=32, handler napíše sp_now do paramu a buffer naplní
 * DESC od sp+N*2. Pak v JSONu invertujeme pořadí (= ASC od sp_now nahoru),
 * což odpovídá briefu "32 slov od SP".
 *
 * Layout response:
 *   {
 *     "sp_now": int,
 *     "sp_odd": bool,
 *     "count_words": int,
 *     "words": [
 *       {"addr": int, "value": int}
 *     ]
 *   }
 *
 * Pokud sp_odd=true, value pole je word read z dvou byte (= little-endian).
 * Klient může detekovat odd SP a interpretovat byte-by-byte.
 *
 * Ownership: handler alokuje buffer, uvolňuje g_free po serializaci.
 */
#define MCP_GET_STACK_WORDS 32
static en_MCP_DISPATCH_RESULT _handle_get_stack(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    /* SP-anchored mode: handler vypočítá base = sp + N*2 a DESC buffer
     * (buf[0] = mem[base], buf[i] = mem[base - i]). Tj. buf[N*2 - 1]
     * = mem[sp] = top of stack. */
    const uint16_t N = MCP_GET_STACK_WORDS;
    const uint16_t LEN = N * 2;
    uint8_t *buf = g_new0(uint8_t, LEN);
    st_DBGAPI_STACK_DUMP_PARAM param = {
        .addr        = 0,
        .len         = LEN,
        .lines_above = N,
        .buf         = buf,
        .sp_now      = 0,
        .sp_odd      = 0,
        .decode_buf  = NULL,
        .decode_count = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_STACK_DUMP, &param, NULL)) {
        g_free(buf);
        return _err_response(req_id, "get_stack failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "sp_now", (gint64)param.sp_now);
    json_object_set_boolean_member(resp, "sp_odd",
                                    param.sp_odd ? TRUE : FALSE);
    json_object_set_int_member(resp, "count_words", (gint64)N);
    JsonArray *arr = json_array_new();
    /* buf je DESC od base = sp + N*2 dolů. Top stacku (= mem[sp]) je v
     * buf[LEN - 1]. Konvertujeme na ASC od sp_now (= words[0] = nejnižší
     * adresa = sp, words[N-1] = sp + 2*(N-1)). */
    for (uint16_t i = 0; i < N; i++) {
        uint16_t addr = (uint16_t)(param.sp_now + (uint16_t)(i * 2));
        /* mem[addr] = buf[LEN - 1 - 2*i], mem[addr+1] = buf[LEN - 2 - 2*i]. */
        uint16_t lo_idx = (uint16_t)(LEN - 1 - (uint16_t)(2 * i));
        uint16_t hi_idx = (uint16_t)(LEN - 2 - (uint16_t)(2 * i));
        uint8_t lo = (lo_idx < LEN) ? buf[lo_idx] : 0;
        uint8_t hi = (hi_idx < LEN) ? buf[hi_idx] : 0;
        uint16_t value = (uint16_t)(lo | (hi << 8));
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "addr", (gint64)addr);
        json_object_set_int_member(item, "value", (gint64)value);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "words", arr);
    g_free(buf);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief Helper - en_DBGAPI_CMD_ORIGIN -> "user"/"mcp"/"test"/"internal".
 */
static const char *_dispatch_origin_to_str(en_DBGAPI_CMD_ORIGIN o) {
    switch (o) {
        case DBGAPI_CMD_ORIGIN_USER:     return "user";
        case DBGAPI_CMD_ORIGIN_MCP:      return "mcp";
        case DBGAPI_CMD_ORIGIN_TEST:     return "test";
        case DBGAPI_CMD_ORIGIN_INTERNAL: return "internal";
    }
    return "user";
}


/**
 * @brief `get_vars` handler - V1.D.2.B Resource backing pro
 *        `emulator://vars`.
 *
 * Forwarduje na nový DBGAPI_CMD_BP_VARS_LIST. Caller alokuje pole
 * entries[] o velikosti 256 (= typický horní limit user var v session).
 * Pokud bylo víc, handler vyplní 256 + truncated=true.
 *
 * Layout response:
 *   {
 *     "count": int,
 *     "truncated": bool,
 *     "vars": [
 *       {"name": str, "value": int, "comment": str,
 *        "has_comment": bool, "persist_value": bool}
 *     ]
 *   }
 *
 * Hodnota `value` je int32_t (signed) v rozsahu storage; JSON přenos
 * jako 64-bit signed (= bez ztráty).
 */
#define MCP_GET_VARS_CAP 256
static en_MCP_DISPATCH_RESULT _handle_get_vars(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_BP_VAR_ENTRY *entries =
        g_new0(st_DBGAPI_BP_VAR_ENTRY, MCP_GET_VARS_CAP);
    st_DBGAPI_BP_VARS_LIST_PARAM param = {
        .entries   = entries,
        .capacity  = MCP_GET_VARS_CAP,
        .out_count = 0,
        .truncated = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_BP_VARS_LIST, &param, NULL)) {
        g_free(entries);
        return _err_response(req_id, "get_vars failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", (gint64)param.out_count);
    json_object_set_boolean_member(resp, "truncated",
                                    param.truncated ? TRUE : FALSE);
    JsonArray *arr = json_array_new();
    for (size_t i = 0; i < param.out_count; i++) {
        const st_DBGAPI_BP_VAR_ENTRY *e = &entries[i];
        JsonObject *item = json_object_new();
        json_object_set_string_member(item, "name", e->name);
        json_object_set_int_member(item, "value", (gint64)e->value);
        json_object_set_string_member(item, "comment", e->comment);
        json_object_set_boolean_member(item, "has_comment",
                                        e->has_comment ? TRUE : FALSE);
        json_object_set_boolean_member(item, "persist_value",
                                        e->persist_value ? TRUE : FALSE);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "vars", arr);
    g_free(entries);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_bookmarks` handler - V1.D.2.B Resource backing pro
 *        `emulator://bookmarks`.
 *
 * Forwarduje na nový DBGAPI_CMD_BOOKMARKS_LIST. Caller alokuje pole
 * entries[] o velikosti 1024 (= bookmarks bývají v menším počtu, ale
 * pojistka pro velký import). Truncated flag říká, zda bylo víc.
 *
 * Layout response:
 *   {
 *     "count": int,
 *     "truncated": bool,
 *     "bookmarks": [
 *       {"id": int, "input": str, "comment": str, "has_comment": bool,
 *        "addr": int|null, "addr_resolved": bool, "owner": str}
 *     ]
 *   }
 *
 * `addr` je null, pokud `addr_resolved=false` (= user_input nelze
 * převést na 16-bit adresu - např. neexistující symbol). `owner` je
 * "user"/"mcp"/"test"/"internal" podle cmd_origin v storage.
 */
#define MCP_GET_BOOKMARKS_CAP 1024
static en_MCP_DISPATCH_RESULT _handle_get_bookmarks(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_BOOKMARK_ENTRY *entries =
        g_new0(st_DBGAPI_BOOKMARK_ENTRY, MCP_GET_BOOKMARKS_CAP);
    st_DBGAPI_BOOKMARKS_LIST_PARAM param = {
        .entries   = entries,
        .capacity  = MCP_GET_BOOKMARKS_CAP,
        .out_count = 0,
        .truncated = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_BOOKMARKS_LIST, &param, NULL)) {
        g_free(entries);
        return _err_response(req_id, "get_bookmarks failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "count", (gint64)param.out_count);
    json_object_set_boolean_member(resp, "truncated",
                                    param.truncated ? TRUE : FALSE);
    JsonArray *arr = json_array_new();
    for (size_t i = 0; i < param.out_count; i++) {
        const st_DBGAPI_BOOKMARK_ENTRY *e = &entries[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "id", (gint64)e->id);
        json_object_set_string_member(item, "input", e->user_input);
        json_object_set_string_member(item, "comment", e->comment);
        json_object_set_boolean_member(item, "has_comment",
                                        e->has_comment ? TRUE : FALSE);
        if (e->addr_resolved) {
            json_object_set_int_member(item, "addr", (gint64)e->addr);
        } else {
            json_object_set_null_member(item, "addr");
        }
        json_object_set_boolean_member(item, "addr_resolved",
                                        e->addr_resolved ? TRUE : FALSE);
        json_object_set_string_member(item, "owner",
            _dispatch_origin_to_str(e->cmd_origin));
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "bookmarks", arr);
    g_free(entries);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bookmark_add` handler - BACKLOG B, write bookmark přes MCP.
 *
 * Parametry:
 *   - `input` (string, povinný) - hex literál ($1234 / 0x1234 / #1234 /
 *     1234h / 1234) nebo jméno symbolu z sym_db. Adresa se resolve
 *     dynamicky, takže symbol může přežít změnu adresy.
 *   - `comment` (string, optional) - default "".
 *
 * Volá `DBGAPI_CMD_BOOKMARK_ADD`. cmd_origin (= MCP) se propaguje do
 * storage v dbgapi handleru; MCP_ACTION broadcast se emituje automaticky
 * v dbgapi_emu_dispatch po úspěchu (cmd_origin == MCP).
 *
 * Response payload (success):
 *   - `id` (int) - nové monotonic ID záložky (>= 1)
 *   - `input` (string) - echo
 *   - `comment` (string) - echo
 *   - `addr` (int|null) - resolved 16-bit adresa nebo null pokud nelze
 *   - `addr_resolved` (bool)
 *   - `owner` (string) - vždy "mcp" (= origin tohoto write)
 */
static en_MCP_DISPATCH_RESULT _handle_bookmark_add(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: input",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *input = _obj_str_dup(data_obj, "input");
    if (!input || !input[0]) {
        g_free(input);
        return _err_response(req_id, "input must be non-empty",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *comment = _obj_str_dup(data_obj, "comment");

    st_DBGAPI_BOOKMARK_WRITE_PARAM param = {
        .user_input        = input,
        .comment           = (comment && comment[0]) ? comment : NULL,
        .remove_id         = 0,
        .out_id            = 0,
        .out_addr          = 0,
        .out_addr_resolved = 0,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_BOOKMARK_ADD, &param, NULL);
    if (!ok) {
        g_free(input);
        g_free(comment);
        return _err_response(req_id, "bookmark_add failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "id", (gint64)param.out_id);
    json_object_set_string_member(resp, "input", input);
    json_object_set_string_member(resp, "comment", comment ? comment : "");
    if (param.out_addr_resolved) {
        json_object_set_int_member(resp, "addr", (gint64)param.out_addr);
    } else {
        json_object_set_null_member(resp, "addr");
    }
    json_object_set_boolean_member(resp, "addr_resolved",
                                    param.out_addr_resolved ? TRUE : FALSE);
    json_object_set_string_member(resp, "owner",
        _dispatch_origin_to_str(DBGAPI_CMD_ORIGIN_MCP));
    g_free(input);
    g_free(comment);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bookmark_remove` handler - BACKLOG B, smaže bookmark podle ID.
 *
 * Parametr:
 *   - `id` (int, povinný) - ID existující záložky (>= 1).
 *
 * Volá `DBGAPI_CMD_BOOKMARK_REMOVE`. MCP_ACTION broadcast se emituje
 * automaticky po úspěchu. Pokud ID neexistuje, bookmarks_remove vrátí
 * false a CMDRQ success=false -> handler vrací removed=false (= NE error,
 * jen no-op). Pro konzistenci s ostatními tooly to vracíme jako success
 * response s removed flagem.
 *
 * Response payload (success):
 *   - `id` (int) - echo
 *   - `removed` (bool) - true pokud záložka existovala a byla odebrána
 */
static en_MCP_DISPATCH_RESULT _handle_bookmark_remove(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Missing required field: id",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    gint64 id_raw = _obj_int_or(data_obj, "id", -1);
    if (id_raw < 1) {
        return _err_response(req_id, "id must be >= 1",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_BOOKMARK_WRITE_PARAM param = {
        .user_input        = NULL,
        .comment           = NULL,
        .remove_id         = (uint32_t)id_raw,
        .out_id            = 0,
        .out_addr          = 0,
        .out_addr_resolved = 0,
    };
    /* _submit_dbgapi vrací rq->success: false pokud ID neexistovalo.
     * To NENÍ chyba kanálu, jen no-op remove. Mapujeme na removed=false. */
    bool removed = _submit_dbgapi(DBGAPI_CMD_BOOKMARK_REMOVE, &param, NULL);
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "id", id_raw);
    json_object_set_boolean_member(resp, "removed", removed ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/* ==================================================================== */
/* V1.D.3.A - IRQ chip Resources (3 read-only handlery)                  */
/*                                                                       */
/* Tři Resources doplňující per-chip detail k V1.D.1                     */
/* emulator://cpu/interrupt_bus (= IRQ subsystem snapshot bez per-chip   */
/* introspekce). Handlery forwardují na nové DBGAPI_CMD_GET_PERIPH_*     */
/* a serializují fixed-size payload do JSON.                             */
/*                                                                       */
/* Threading: handler běží na MCP I/O vlákně, _submit_dbgapi bouncuje    */
/* na EMU safe-point. Žádný refactor pio8255 / ctc8253 / pioz80 API.    */
/* ==================================================================== */


/**
 * @brief Mapuje 8253 mode hodnotu na human-readable string.
 *
 * Per ctc8253.h en_CTC_MODE: 0=mode0 ... 5=mode5. Vyšší hodnoty (= chyba
 * v storage) mapuje na "unknown".
 */
static const char *_i8253_mode_str(uint8_t mode) {
    switch (mode) {
        case 0: return "mode0";
        case 1: return "mode1";
        case 2: return "mode2";
        case 3: return "mode3";
        case 4: return "mode4";
        case 5: return "mode5";
        default: return "unknown";
    }
}


/**
 * @brief Mapuje 8253 Read/Load Format na string.
 *
 * Per ctc8253.h en_CTC_RLF: 1=LSB, 2=MSB, 3=LSB+MSB. Hodnota 0 (= žádné
 * RLF nastaveno před prvním zápisem) se vrací jako "none"; ostatní jako
 * "unknown".
 */
static const char *_i8253_rlf_str(uint8_t rlf) {
    switch (rlf) {
        case 0: return "none";
        case 1: return "lsb";
        case 2: return "msb";
        case 3: return "lsb_msb";
        default: return "unknown";
    }
}


/**
 * @brief Mapuje 8253 stav sequenceru na string.
 *
 * Per ctc8253.h en_CTC_STATE; pokrýváme 11 hodnot. Vyšší hodnoty jsou
 * "unknown" (= defenzivně).
 */
static const char *_i8253_state_str(uint8_t state) {
    switch (state) {
        case 0:  return "init";
        case 1:  return "init_done";
        case 2:  return "load";
        case 3:  return "preset_error";
        case 4:  return "load_done";
        case 5:  return "wait_gate1";
        case 6:  return "preset";
        case 7:  return "preset32";
        case 8:  return "countdown";
        case 9:  return "mode1_trigger_error";
        case 10: return "blind_count";
        default: return "unknown";
    }
}


/**
 * @brief Mapuje Z80 PIO Port Mode na string.
 *
 * Per pioz80.h en_PIOZ80_PORT_MODE: 0=output, 1=input, 2=bidir (jen
 * Port A), 3=user (Mode 3 = bit-by-bit dle io_mask). Ostatní = unknown.
 */
static const char *_z80_pio_mode_str(uint8_t mode) {
    switch (mode) {
        case 0: return "output";
        case 1: return "input";
        case 2: return "bidir";
        case 3: return "user";
        default: return "unknown";
    }
}


/**
 * @brief Mapuje agregátní stav PIO IRQ na string.
 *
 * Per pioz80.h en_PIOZ80_INTERRUPT: 0=none, 1=nextprio (neemulujeme,
 * "nextprio_unemulated"), 2=received, 3=pending.
 */
static const char *_z80_pio_interrupt_str(uint8_t interrupt) {
    switch (interrupt) {
        case 0: return "none";
        case 1: return "nextprio_unemulated";
        case 2: return "received";
        case 3: return "pending";
        default: return "unknown";
    }
}


/**
 * @brief Mapuje en_PIOZ80_PORT_INT na string.
 *
 * Per pioz80.h: 0=none, 1=pending, 2=received, 3=repending.
 */
static const char *_z80_pio_port_int_str(uint8_t v) {
    switch (v) {
        case 0: return "none";
        case 1: return "pending";
        case 2: return "received";
        case 3: return "repending";
        default: return "unknown";
    }
}


/**
 * @brief Serializace jednoho Z80 PIO portu do JsonObject.
 *
 * Společná pomocná funkce pro Port A i Port B. Klient dostane mode/
 * port_int stringy + raw integer pro int_vec / icmask / io_mask
 * (= bitové masky, ne enum).
 */
static JsonObject *_z80_pio_port_to_json(
    const st_DBGAPI_PERIPH_Z80_PIO_PORT *port) {
    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj,    "data_output",    (gint64)port->data_output);
    json_object_set_int_member(obj,    "masked_input",   (gint64)port->masked_input);
    json_object_set_int_member(obj,    "io_mask",        (gint64)port->io_mask);
    json_object_set_string_member(obj, "mode",           _z80_pio_mode_str(port->mode));
    json_object_set_int_member(obj,    "int_vec",        (gint64)port->int_vec);
    json_object_set_int_member(obj,    "icmask",         (gint64)port->icmask);
    json_object_set_boolean_member(obj, "int_enable",    port->icena ? TRUE : FALSE);
    json_object_set_int_member(obj,    "icfnc",          (gint64)port->icfnc);
    json_object_set_int_member(obj,    "iclvl",          (gint64)port->iclvl);
    json_object_set_string_member(obj, "port_int",       _z80_pio_port_int_str(port->port_int));
    json_object_set_int_member(obj,    "last_ctrl_byte", (gint64)port->last_ctrl_byte);
    return obj;
}


/**
 * @brief `get_periph_i8255` handler - V1.D.3.A Resource backing pro
 *        `emulator://periph/i8255`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_I8255. Response obsahuje
 * port A/B/C, Control Word + decoded mode/direction fields a PC
 * signal bity (CMT motor, CTC0 audio gate, CTC2 IRQ enable).
 *
 * Layout response:
 *   {"port_a": int, "port_b": int, "port_c": int,
 *    "control_word": int, "cw_decoded": bool,
 *    "mode_group_a": int, "mode_group_b": int,
 *    "pa_dir": "input"|"output", "pb_dir": ..., "pc_upper_dir": ...,
 *    "pc_lower_dir": ...,
 *    "signal_pc00": int, "signal_pc01": int, "signal_pc02": int,
 *    "signal_pc03": int, "signal_pc04": int,
 *    "pa_keyboard_column": int,
 *    "pa_joy1_enabled": bool, "pa_joy2_enabled": bool}
 *
 * `cw_decoded=false` znamená, že poslední CPU write byla Bit Set/Reset
 * (= mode/dir fields nesmí klient interpretovat).
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_i8255(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_I8255_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_I8255, &param, NULL)) {
        return _err_response(req_id, "get_periph_i8255 failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp,    "port_a",        (gint64)param.port_a);
    json_object_set_int_member(resp,    "port_b",        (gint64)param.port_b);
    json_object_set_int_member(resp,    "port_c",        (gint64)param.port_c);
    json_object_set_int_member(resp,    "control_word",  (gint64)param.control_word);
    json_object_set_boolean_member(resp, "cw_decoded",   param.cw_decoded ? TRUE : FALSE);
    json_object_set_int_member(resp,    "mode_group_a",  (gint64)param.mode_group_a);
    json_object_set_int_member(resp,    "mode_group_b",  (gint64)param.mode_group_b);
    json_object_set_string_member(resp, "pa_dir",        param.pa_dir       ? "input" : "output");
    json_object_set_string_member(resp, "pb_dir",        param.pb_dir       ? "input" : "output");
    json_object_set_string_member(resp, "pc_upper_dir",  param.pc_upper_dir ? "input" : "output");
    json_object_set_string_member(resp, "pc_lower_dir",  param.pc_lower_dir ? "input" : "output");
    json_object_set_int_member(resp,    "signal_pc00",   (gint64)param.signal_pc00);
    json_object_set_int_member(resp,    "signal_pc01",   (gint64)param.signal_pc01);
    json_object_set_int_member(resp,    "signal_pc02",   (gint64)param.signal_pc02);
    json_object_set_int_member(resp,    "signal_pc03",   (gint64)param.signal_pc03);
    json_object_set_int_member(resp,    "signal_pc04",   (gint64)param.signal_pc04);
    json_object_set_int_member(resp,    "pa_keyboard_column",
                                (gint64)param.pa_keyboard_column);
    json_object_set_boolean_member(resp, "pa_joy1_enabled",
                                    param.pa_joy1_enabled ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "pa_joy2_enabled",
                                    param.pa_joy2_enabled ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_i8253` handler - V1.D.3.A Resource backing pro
 *        `emulator://periph/i8253`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_I8253. Response obsahuje per-channel
 * (CTC0/1/2) counter / preset / mode / state + agregátní last_cw_byte.
 *
 * Layout response:
 *   {"last_cw_byte": int,
 *    "channels": [
 *       {"index": int, "value": int, "preset_value": int,
 *        "preset_latch": int, "read_latch": int,
 *        "out": int, "gate": int,
 *        "mode": "mode0".."mode5", "bcd": bool,
 *        "rlf": "lsb"|"msb"|"lsb_msb"|"none",
 *        "state": str, "load_done": bool, "latch_op": bool,
 *        "rl_byte": int}
 *    ]}
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_i8253(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_I8253_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_I8253, &param, NULL)) {
        return _err_response(req_id, "get_periph_i8253 failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "last_cw_byte", (gint64)param.last_cw_byte);
    JsonArray *arr = json_array_new();
    for (int i = 0; i < 3; i++) {
        const st_DBGAPI_PERIPH_I8253_CHANNEL *c = &param.ch[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item,    "index",         (gint64)i);
        json_object_set_int_member(item,    "value",         (gint64)c->value);
        json_object_set_int_member(item,    "preset_value",  (gint64)c->preset_value);
        json_object_set_int_member(item,    "preset_latch",  (gint64)c->preset_latch);
        json_object_set_int_member(item,    "read_latch",    (gint64)c->read_latch);
        json_object_set_int_member(item,    "out",           (gint64)c->out);
        json_object_set_int_member(item,    "gate",          (gint64)c->gate);
        json_object_set_string_member(item, "mode",          _i8253_mode_str(c->mode));
        json_object_set_boolean_member(item, "bcd",          c->bcd ? TRUE : FALSE);
        json_object_set_string_member(item, "rlf",           _i8253_rlf_str(c->rlf));
        json_object_set_string_member(item, "state",         _i8253_state_str(c->state));
        json_object_set_boolean_member(item, "load_done",    c->load_done ? TRUE : FALSE);
        json_object_set_boolean_member(item, "latch_op",     c->latch_op ? TRUE : FALSE);
        json_object_set_int_member(item,    "rl_byte",       (gint64)c->rl_byte);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "channels", arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_z80_pio` handler - V1.D.3.A Resource backing pro
 *        `emulator://periph/z80_pio`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_Z80_PIO. Na MZ-700, kde Z80 PIO
 * není přítomen, vrací `{"available": false,
 * "reason": "platform has no Z80 PIO"}`.
 *
 * Pro MZ-800/MZ-1500 (`available=true`) response obsahuje agregátní
 * IRQ stav + per-port snapshot (data_output, masked_input, io_mask,
 * mode, int_vec, icmask, int_enable, icfnc, iclvl, port_int,
 * last_ctrl_byte).
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_z80_pio(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_Z80_PIO_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_Z80_PIO, &param, NULL)) {
        return _err_response(req_id, "get_periph_z80_pio failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp, "reason",
                                       "platform has no Z80 PIO");
        return _ok_response(req_id, resp, out_response);
    }
    json_object_set_boolean_member(resp, "available", TRUE);
    json_object_set_string_member(resp, "interrupt",
                                   _z80_pio_interrupt_str(param.interrupt));
    if (param.interrupt_port_id == 0xFF) {
        json_object_set_null_member(resp, "interrupt_port_id");
    } else {
        json_object_set_int_member(resp, "interrupt_port_id",
                                    (gint64)param.interrupt_port_id);
    }
    json_object_set_object_member(resp, "port_a",
                                   _z80_pio_port_to_json(&param.port_a));
    json_object_set_object_member(resp, "port_b",
                                   _z80_pio_port_to_json(&param.port_b));
    return _ok_response(req_id, resp, out_response);
}


/* ====================================================================== */
/* V1.D.3.B - audio chip Resource helpers                                  */
/* ====================================================================== */

/**
 * @brief Mapování `en_PSG_CHTYPE` -> JSON string.
 *
 * @param v raw type byte (0=TONE, 1=NOISE)
 * @return statický řetězec ("tone"/"noise"/"unknown")
 */
static const char *_psg_channel_type_str(uint8_t v) {
    switch (v) {
        case 0:  return "tone";
        case 1:  return "noise";
        default: return "unknown";
    }
}

/**
 * @brief Mapování `en_NOISE_DIV_TYPE` -> JSON string.
 *
 * 4 hodnoty per psg.h:
 *   0 = /16 (cca 6.928 kHz), 1 = /32 (3.464 kHz),
 *   2 = /64 (1.732 kHz), 3 = řízeno tone_divider channel 2.
 *
 * @param v raw enum byte (0..3)
 * @return statický řetězec
 */
static const char *_psg_noise_div_str(uint8_t v) {
    switch (v) {
        case 0: return "div_16";
        case 1: return "div_32";
        case 2: return "div_64";
        case 3: return "tone2_controlled";
        default: return "unknown";
    }
}

/**
 * @brief Mapování `en_NOISE_TYPE` -> JSON string.
 *
 * @param v raw enum byte (0=periodic, 1=white)
 * @return statický řetězec
 */
static const char *_psg_noise_type_str(uint8_t v) {
    switch (v) {
        case 0:  return "periodic";
        case 1:  return "white";
        default: return "unknown";
    }
}

/**
 * @brief Serializace jednoho PSG kanálu do JsonObject.
 *
 * Klient dostane:
 *   - `type`: "tone" / "noise"
 *   - `attenuation`: 0..15 (0 = max volume, 15 = silent)
 *   - `tone_divider`: 10-bit (0..1023); smysluplné jen pro type=tone
 *   - `noise_div_type`: "div_16" / "div_32" / "div_64" /
 *     "tone2_controlled"; smysluplné jen pro type=noise
 *   - `noise_type`: "periodic" / "white"; smysluplné jen pro type=noise
 *
 * @param ch ukazatel na `st_DBGAPI_PERIPH_SN76489_CHANNEL` (NESMÍ být NULL)
 * @return nový JsonObject; vlastnictví předáváno volajícímu
 */
static JsonObject *_psg_channel_to_json(
    const st_DBGAPI_PERIPH_SN76489_CHANNEL *ch) {
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "type",           _psg_channel_type_str(ch->type));
    json_object_set_int_member(obj,    "attenuation",    (gint64)ch->attenuation);
    json_object_set_int_member(obj,    "tone_divider",   (gint64)ch->tone_divider);
    json_object_set_string_member(obj, "noise_div_type", _psg_noise_div_str(ch->noise_div_type));
    json_object_set_string_member(obj, "noise_type",     _psg_noise_type_str(ch->noise_type));
    return obj;
}

/**
 * @brief `get_periph_sn76489` handler - V1.D.3.B Resource backing pro
 *        `emulator://periph/sn76489`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_SN76489. Na MZ-700 (HAVE_PSG=0)
 * vrací `{"available": false, "reason": "platform has no PSG"}`.
 * Pro MZ-800/MZ-1500 (`available=true`) response obsahuje per-instance
 * latch state + per-channel array (4 kanály per PSG; ch0..ch2 TONE,
 * ch3 NOISE). MZ-800 mono = jen `psg0`; MZ-800 stereo + MZ-1500 =
 * `psg0` + `psg1`.
 *
 * Layout response:
 *   {"available": true, "psg_count": 1|2, "stereo": bool,
 *    "psg0": {"latch_cs": int, "latch_attn": bool,
 *             "channels": [{...} x4]},
 *    "psg1": {...} | absent}
 *
 * Layout response (available=false):
 *   {"available": false, "reason": "platform has no PSG"}
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_sn76489(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_SN76489_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_SN76489, &param, NULL)) {
        return _err_response(req_id, "get_periph_sn76489 failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp, "reason",
                                       "platform has no PSG");
        return _ok_response(req_id, resp, out_response);
    }
    json_object_set_boolean_member(resp, "available", TRUE);
    json_object_set_int_member(resp,    "psg_count", (gint64)param.psg_count);
    json_object_set_boolean_member(resp, "stereo",   param.stereo ? TRUE : FALSE);
    /* PSG0 - vždy přítomen pokud available. */
    JsonObject *psg0 = json_object_new();
    json_object_set_int_member(psg0,     "latch_cs",   (gint64)param.psg0_latch_cs);
    json_object_set_boolean_member(psg0, "latch_attn", param.psg0_latch_attn ? TRUE : FALSE);
    JsonArray *ch0_arr = json_array_new();
    for (int i = 0; i < 4; i++) {
        JsonObject *obj = _psg_channel_to_json(&param.psg0_ch[i]);
        json_object_set_int_member(obj, "index", (gint64)i);
        json_array_add_object_element(ch0_arr, obj);
    }
    json_object_set_array_member(psg0, "channels", ch0_arr);
    json_object_set_object_member(resp, "psg0", psg0);
    /* PSG1 - jen pokud psg_count >= 2. */
    if (param.psg_count >= 2) {
        JsonObject *psg1 = json_object_new();
        json_object_set_int_member(psg1,     "latch_cs",   (gint64)param.psg1_latch_cs);
        json_object_set_boolean_member(psg1, "latch_attn", param.psg1_latch_attn ? TRUE : FALSE);
        JsonArray *ch1_arr = json_array_new();
        for (int i = 0; i < 4; i++) {
            JsonObject *obj = _psg_channel_to_json(&param.psg1_ch[i]);
            json_object_set_int_member(obj, "index", (gint64)i);
            json_array_add_object_element(ch1_arr, obj);
        }
        json_object_set_array_member(psg1, "channels", ch1_arr);
        json_object_set_object_member(resp, "psg1", psg1);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_ay3_8910` handler - V1.D.3.B Resource backing pro
 *        `emulator://periph/ay3_8910`.
 *
 * AY-3-8910 NENÍ v současné verzi emulátoru implementován. Handler
 * forwarduje na DBGAPI_CMD_GET_PERIPH_AY3_8910 (placeholder, jen
 * vyplní available=0) a vrátí klientovi:
 *
 *   {"available": false,
 *    "reason": "AY-3-8910 not implemented in this emulator"}
 *
 * Klient nesmí na další pole spoléhat. Resource zůstává v API kvůli
 * forward compat - pokud někdo později chip implementuje, vrátí
 * registry, ale klient s available check pak nepotřebuje upgrade.
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_ay3_8910(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_AY3_8910_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_AY3_8910, &param, NULL)) {
        return _err_response(req_id, "get_periph_ay3_8910 failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    /* Vždy false v aktuální verzi - viz handler dbgapi.c. */
    json_object_set_boolean_member(resp, "available", FALSE);
    json_object_set_string_member(resp, "reason",
                                   "AY-3-8910 not implemented in this emulator");
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_beeper` handler - V1.D.3.B Resource backing pro
 *        `emulator://periph/beeper`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_BEEPER. Response obsahuje
 * agregovanou audible level + raw signální bity CTC0 OUT, GATE0 a
 * PC0 - klient může debugovat audio cestu krok po kroku.
 *
 * Pozn.: Sharp MZ nemá dedikovaný 1-bit beeper jako ZX Spectrum;
 * "beeper" je pracovní termín pro audio cestu CTC0 OUT přes hradla
 * GATE0 + PC0. Reference:
 * mz800-knowledge/reference/agent/hw/06-ctc-8253.md (CTC0 OUT),
 * hw/05-pio-8255.md (PC0 audio gate).
 *
 * Layout response:
 *   {"available": true, "level": 0|1,
 *    "ctc0_out": 0|1, "gate0": 0|1, "pc0": 0|1,
 *    "source": "PC0"}
 *
 * `level = ctc0_out AND gate0 AND pc0`. Klient může také vlastní
 * dopočet provést pokud chce filtrovat (např. ignorovat GATE0 pro
 * MZ-800 v 800 módu, kde je GATE0 vždy 1 hardware-wise).
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_beeper(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_BEEPER_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_BEEPER, &param, NULL)) {
        return _err_response(req_id, "get_periph_beeper failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "available", param.available ? TRUE : FALSE);
    json_object_set_int_member(resp, "level",    (gint64)param.level);
    json_object_set_int_member(resp, "ctc0_out", (gint64)param.ctc0_out);
    json_object_set_int_member(resp, "gate0",    (gint64)param.gate0);
    json_object_set_int_member(resp, "pc0",      (gint64)param.pc0);
    /* source field je pevné 3-byte ASCII identifikátor v param; pro
     * klienta serializujeme jako NUL-terminated string. */
    char src_str[4];
    src_str[0] = (char)param.source[0];
    src_str[1] = (char)param.source[1];
    src_str[2] = (char)param.source[2];
    src_str[3] = '\0';
    json_object_set_string_member(resp, "source", src_str);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_gdg` handler - V1.D.3.C Resource backing pro
 *        `emulator://periph/gdg`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_GDG. Response obsahuje
 * per-platforma palette layout (16-color regPAL* u MZ-800, 8-entry
 * mode_color u MZ-700/MZ-1500), raster state, regDMD a (volitelně)
 * regBOR + regPALGRP + cksw u MZ-800.
 *
 * Layout response:
 *   {"available": true, "platform": "mz800"|"mz700"|"mz1500",
 *    "palette_count": 16|8, "palette": [n bytes],
 *    "has_border_reg": bool, "has_pal_group": bool, "has_cksw": bool,
 *    "regDMD": int, "regBOR": int, "regPALGRP": int, "regct53g7": int,
 *    "beam_row": int, "total_screens": int, "total_ticks": int,
 *    "sts_vsync": int, "sts_hsync": int, "hbln": int, "vbln": int,
 *    "cksw": int, "tempo": int, "tempo_divider": int}
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_gdg(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_GDG_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_GDG, &param, NULL)) {
        return _err_response(req_id, "get_periph_gdg failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "available", param.available ? TRUE : FALSE);
    /* platform pole má 12 bajtů; je NUL-terminated z handleru. */
    char plat[16];
    memcpy(plat, param.platform, sizeof(param.platform));
    plat[sizeof(param.platform)] = '\0';
    json_object_set_string_member(resp, "platform", plat);
    json_object_set_int_member(resp,    "palette_count", (gint64)param.palette_count);
    json_object_set_boolean_member(resp, "has_border_reg", param.has_border_reg ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "has_pal_group",  param.has_pal_group ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "has_cksw",       param.has_cksw ? TRUE : FALSE);
    json_object_set_int_member(resp,    "regDMD",       (gint64)param.regDMD);
    json_object_set_int_member(resp,    "regBOR",       (gint64)param.regBOR);
    json_object_set_int_member(resp,    "regPALGRP",    (gint64)param.regPALGRP);
    json_object_set_int_member(resp,    "regct53g7",    (gint64)param.regct53g7);
    json_object_set_int_member(resp,    "beam_row",     (gint64)param.beam_row);
    json_object_set_int_member(resp,    "total_screens", (gint64)param.total_screens);
    json_object_set_int_member(resp,    "total_ticks",  (gint64)param.total_ticks);
    json_object_set_int_member(resp,    "sts_vsync",    (gint64)param.sts_vsync);
    json_object_set_int_member(resp,    "sts_hsync",    (gint64)param.sts_hsync);
    json_object_set_int_member(resp,    "hbln",         (gint64)param.hbln);
    json_object_set_int_member(resp,    "vbln",         (gint64)param.vbln);
    json_object_set_int_member(resp,    "cksw",         (gint64)param.cksw);
    json_object_set_int_member(resp,    "tempo",        (gint64)param.tempo);
    json_object_set_int_member(resp,    "tempo_divider", (gint64)param.tempo_divider);
    /* Paleta array, jen palette_count platných entries. */
    JsonArray *pal_arr = json_array_new();
    unsigned pn = param.palette_count;
    if (pn > 16) pn = 16;
    for (unsigned i = 0; i < pn; i++) {
        json_array_add_int_element(pal_arr, (gint64)param.palette[i]);
    }
    json_object_set_array_member(resp, "palette", pal_arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_wd1793` handler - V1.D.3.C Resource backing pro
 *        `emulator://periph/wd1793`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_WD1793. Response obsahuje
 * registry chipu (status, command, track, sector, data), motor/side/
 * density port states, state machine fields (intrq, multiblock_rw,
 * direction_latch, positioned_*) a pole 4 drives s mount metadaty
 * (image_basename jen filename per V1.D.1 security).
 *
 * Layout response (available=true):
 *   {"available": true, "bus_xlate_invert": bool, "hd_patch": bool,
 *    "reg_status": int, "reg_command": int, "reg_track": int,
 *    "reg_sector": int, "reg_data": int, "motor": int, "side": int,
 *    "density": int, "multiblock_rw": int, "direction_latch": int,
 *    "intrq_active": int, "positioned_track": int,
 *    "positioned_sector": int, "positioned_side": int,
 *    "status_mode": int, "buffer_pos": int, "data_counter": int,
 *    "current_sector_size": int,
 *    "drives": [{"index": int, "present": bool, "readonly": bool,
 *                "user_readonly": bool, "fs_readonly": bool,
 *                "storage_mode": int, "tracks": int, "sides": int,
 *                "total_data_bytes": int, "image_basename": str} x4]}
 *
 * Layout response (available=false):
 *   {"available": false,
 *    "reason": "FDC not compiled or detached"}
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_wd1793(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_WD1793_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_WD1793, &param, NULL)) {
        return _err_response(req_id, "get_periph_wd1793 failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp, "reason",
                                       "FDC not compiled or detached");
        return _ok_response(req_id, resp, out_response);
    }
    json_object_set_boolean_member(resp, "available",         TRUE);
    json_object_set_boolean_member(resp, "bus_xlate_invert",  param.bus_xlate_invert ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "hd_patch",          param.hd_patch ? TRUE : FALSE);
    json_object_set_int_member(resp, "reg_status",          (gint64)param.reg_status);
    json_object_set_int_member(resp, "reg_command",         (gint64)param.reg_command);
    json_object_set_int_member(resp, "reg_track",           (gint64)param.reg_track);
    json_object_set_int_member(resp, "reg_sector",          (gint64)param.reg_sector);
    json_object_set_int_member(resp, "reg_data",            (gint64)param.reg_data);
    json_object_set_int_member(resp, "motor",               (gint64)param.motor);
    json_object_set_int_member(resp, "side",                (gint64)param.side);
    json_object_set_int_member(resp, "density",             (gint64)param.density);
    json_object_set_int_member(resp, "multiblock_rw",       (gint64)param.multiblock_rw);
    json_object_set_int_member(resp, "direction_latch",     (gint64)param.direction_latch);
    json_object_set_int_member(resp, "intrq_active",        (gint64)param.intrq_active);
    json_object_set_int_member(resp, "positioned_track",    (gint64)param.positioned_track);
    json_object_set_int_member(resp, "positioned_sector",   (gint64)param.positioned_sector);
    json_object_set_int_member(resp, "positioned_side",     (gint64)param.positioned_side);
    json_object_set_int_member(resp, "status_mode",         (gint64)param.status_mode);
    json_object_set_int_member(resp, "buffer_pos",          (gint64)param.buffer_pos);
    json_object_set_int_member(resp, "data_counter",        (gint64)param.data_counter);
    json_object_set_int_member(resp, "current_sector_size", (gint64)param.current_sector_size);
    JsonArray *drv_arr = json_array_new();
    for (int d = 0; d < 4; d++) {
        const st_DBGAPI_PERIPH_FDC_DRIVE *src = &param.drives[d];
        JsonObject *drv = json_object_new();
        json_object_set_int_member(drv,     "index",            (gint64)d);
        json_object_set_boolean_member(drv, "present",          src->present ? TRUE : FALSE);
        json_object_set_boolean_member(drv, "readonly",         src->readonly ? TRUE : FALSE);
        json_object_set_boolean_member(drv, "user_readonly",    src->user_readonly ? TRUE : FALSE);
        json_object_set_boolean_member(drv, "fs_readonly",      src->fs_readonly ? TRUE : FALSE);
        json_object_set_int_member(drv,     "storage_mode",     (gint64)src->storage_mode);
        json_object_set_int_member(drv,     "tracks",           (gint64)src->tracks);
        json_object_set_int_member(drv,     "sides",            (gint64)src->sides);
        json_object_set_int_member(drv,     "total_data_bytes", (gint64)src->total_data_bytes);
        json_object_set_string_member(drv,  "image_basename",   src->image_basename);
        json_array_add_object_element(drv_arr, drv);
    }
    json_object_set_array_member(resp, "drives", drv_arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_cmt` handler - V1.D.3.C Resource backing pro
 *        `emulator://periph/cmt`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_CMT. Response obsahuje state
 * (stop/play/record), motor activity (= state != STOP), paused flag,
 * polarity, cmt speed, cpu_boost, image basename (= jen filename per
 * V1.D.1 security). CMT je u všech tří platforem dostupný.
 *
 * Layout response:
 *   {"available": true, "state": "stop"|"play"|"record",
 *    "paused": bool, "filled": bool, "polarity_inverted": bool,
 *    "cmtspeed": int, "cpu_boost": bool, "mzfsize_check": bool,
 *    "output": int, "playsts": int, "cmthack_enabled": bool,
 *    "start_time": int, "paused_time": int,
 *    "image_basename": str}
 *
 * Pole "cmthack_enabled" (CMT-A) reflektuje stav cmthack ROM patche
 * (= instant load), nezávisle na reálném stavu pásky.
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_cmt(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_CMT_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_CMT, &param, NULL)) {
        return _err_response(req_id, "get_periph_cmt failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "available", param.available ? TRUE : FALSE);
    /* state enum -> string. */
    const char *state_s = "stop";
    if (param.state == 1) state_s = "play";
    else if (param.state == 2) state_s = "record";
    json_object_set_string_member(resp, "state",   state_s);
    json_object_set_boolean_member(resp, "paused",            param.paused ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "filled",            param.filled ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "polarity_inverted", param.polarity_inverted ? TRUE : FALSE);
    json_object_set_int_member(resp,     "cmtspeed",          (gint64)param.cmtspeed);
    json_object_set_boolean_member(resp, "cpu_boost",         param.cpu_boost ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "mzfsize_check",     param.mzfsize_check ? TRUE : FALSE);
    json_object_set_int_member(resp,     "output",            (gint64)param.output);
    json_object_set_int_member(resp,     "playsts",           (gint64)param.playsts);
    /* CMT-A: stav cmthack ROM patche (instant load) - oddělený od reálné
     * páskové emulace, ale součást CMT subsystému, proto v tomto resource. */
    json_object_set_boolean_member(resp, "cmthack_enabled",   param.cmthack_enabled ? TRUE : FALSE);
    json_object_set_int_member(resp,     "start_time",        (gint64)param.start_time);
    json_object_set_int_member(resp,     "paused_time",       (gint64)param.paused_time);
    json_object_set_string_member(resp,  "image_basename",    param.image_basename);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_periph_qd` handler - V1.D.3.C Resource backing pro
 *        `emulator://periph/qd`.
 *
 * Forwarduje na DBGAPI_CMD_GET_PERIPH_QD. Response obsahuje type
 * (image/virtual/unicard), connected status, R/O příznaky, image
 * position, VIRTUAL mode counts a image basename (= jen filename per
 * V1.D.1 security).
 *
 * Layout response (available=true):
 *   {"available": true, "type": "image"|"virtual"|"unicard",
 *    "status": int, "readonly": bool, "user_readonly": bool,
 *    "fs_readonly": bool, "storage_mode": int, "vrtsts": int,
 *    "image_position": int, "virt_files_count": int,
 *    "virt_file_num": int, "virt_mzfbody_size": int,
 *    "out_crc16": int, "image_basename": str}
 *
 * Layout response (available=false):
 *   {"available": false,
 *    "reason": "QDisk not compiled or detached"}
 */
static en_MCP_DISPATCH_RESULT _handle_get_periph_qd(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PERIPH_QD_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_PERIPH_QD, &param, NULL)) {
        return _err_response(req_id, "get_periph_qd failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp, "reason",
                                       "QDisk not compiled or detached");
        return _ok_response(req_id, resp, out_response);
    }
    json_object_set_boolean_member(resp, "available", TRUE);
    const char *type_s = "image";
    if (param.type == 1) type_s = "virtual";
    else if (param.type == 2) type_s = "unicard";
    json_object_set_string_member(resp,  "type",              type_s);
    json_object_set_int_member(resp,     "status",            (gint64)param.status);
    json_object_set_boolean_member(resp, "readonly",          param.readonly ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "user_readonly",     param.user_readonly ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "fs_readonly",       param.fs_readonly ? TRUE : FALSE);
    json_object_set_int_member(resp,     "storage_mode",      (gint64)param.storage_mode);
    json_object_set_int_member(resp,     "vrtsts",            (gint64)param.vrtsts);
    json_object_set_int_member(resp,     "image_position",    (gint64)param.image_position);
    json_object_set_int_member(resp,     "virt_files_count",  (gint64)param.virt_files_count);
    json_object_set_int_member(resp,     "virt_file_num",     (gint64)param.virt_file_num);
    json_object_set_int_member(resp,     "virt_mzfbody_size", (gint64)param.virt_mzfbody_size);
    json_object_set_int_member(resp,     "out_crc16",         (gint64)param.out_crc16);
    json_object_set_string_member(resp,  "image_basename",    param.image_basename);
    return _ok_response(req_id, resp, out_response);
}


/* ====================================================================== */
/* V1.D.4 - Input + Frame Resource handlery                                */
/* ====================================================================== */

/**
 * @brief `get_input_keyboard_state` handler - V1.D.4 Resource backing pro
 *        `emulator://input/keyboard/state`.
 *
 * Forwarduje na DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE. Response obsahuje
 * 3 bitové matice (real_matrix, virtual_matrix, effective) zakódované
 * jako JSON pole 10 int hodnot (= jeden bajt per sloupec, 0xff=idle) a
 * decode pressed_keys array s {col, bit, name}.
 *
 * Layout response:
 *   {"real_matrix": [255, ...], "virtual_matrix": [255, ...],
 *    "effective": [255, ...], "pressed_count": int,
 *    "pressed_truncated": bool,
 *    "pressed_keys": [{"col": 8, "bit": 0, "name": "SHIFT"}, ...]}
 */
static en_MCP_DISPATCH_RESULT _handle_get_input_keyboard_state(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_INPUT_KBD_STATE_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE, &param, NULL)) {
        return _err_response(req_id, "get_input_keyboard_state failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    JsonArray *real_arr = json_array_sized_new(10);
    JsonArray *virt_arr = json_array_sized_new(10);
    JsonArray *eff_arr  = json_array_sized_new(10);
    for (int c = 0; c < 10; c++) {
        json_array_add_int_element(real_arr, (gint64)param.real_matrix[c]);
        json_array_add_int_element(virt_arr, (gint64)param.virtual_matrix[c]);
        json_array_add_int_element(eff_arr,  (gint64)param.effective[c]);
    }
    json_object_set_array_member(resp, "real_matrix",    real_arr);
    json_object_set_array_member(resp, "virtual_matrix", virt_arr);
    json_object_set_array_member(resp, "effective",      eff_arr);
    json_object_set_int_member(resp,   "pressed_count",  (gint64)param.pressed_count);
    json_object_set_boolean_member(resp, "pressed_truncated",
                                   param.pressed_truncated ? TRUE : FALSE);
    JsonArray *keys_arr = json_array_new();
    for (uint32_t i = 0; i < param.pressed_count; i++) {
        JsonObject *ko = json_object_new();
        json_object_set_int_member(ko,    "col",  (gint64)param.pressed_keys[i].col);
        json_object_set_int_member(ko,    "bit",  (gint64)param.pressed_keys[i].bit);
        json_object_set_string_member(ko, "name", param.pressed_keys[i].name);
        json_array_add_object_element(keys_arr, ko);
    }
    json_object_set_array_member(resp, "pressed_keys", keys_arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_input_keyboard_matrix_info` handler - V1.D.4 Resource pro
 *        `emulator://input/keyboard/matrix_info`.
 *
 * Statická popisná tabulka kláves (= sjednocená napříč MZ-700/MZ-800/
 * MZ-1500). Resource je hodnotově neměnný za runtime; klient typicky
 * čte jednou a cachuje.
 *
 * Layout response:
 *   {"platform": "mz800", "key_count": 40,
 *    "keys": [{"name": "RETURN", "col": 0, "bit": 0,
 *              "needs_shift": false}, ...]}
 */
static en_MCP_DISPATCH_RESULT _handle_get_input_keyboard_matrix_info(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO, &param, NULL)) {
        return _err_response(req_id, "get_input_keyboard_matrix_info failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "platform",  param.platform);
    json_object_set_int_member(resp,    "key_count", (gint64)param.key_count);
    JsonArray *keys_arr = json_array_new();
    for (uint32_t i = 0; i < param.key_count; i++) {
        JsonObject *ko = json_object_new();
        json_object_set_string_member(ko,  "name",        param.keys[i].name);
        json_object_set_int_member(ko,     "col",         (gint64)param.keys[i].col);
        json_object_set_int_member(ko,     "bit",         (gint64)param.keys[i].bit);
        json_object_set_boolean_member(ko, "needs_shift",
                                       param.keys[i].needs_shift ? TRUE : FALSE);
        json_array_add_object_element(keys_arr, ko);
    }
    json_object_set_array_member(resp, "keys", keys_arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_input_joystick_state` handler - V1.D.4 Resource pro
 *        `emulator://input/joystick/state`.
 *
 * Per-port (0 a 1) snapshot. Port není připojen pokud type = NONE
 * (= MZ-700 typicky). state_bits je active-HIGH MCP bitmask, native_state
 * je raw active-LOW chip value (pro debug). device_name "none"/"num_keypad"/
 * "joystick".
 *
 * Layout response:
 *   {"ports": [{"index": 0, "connected": bool, "state_bits": int,
 *               "native_state": int, "device_name": str}, ...]}
 */
static en_MCP_DISPATCH_RESULT _handle_get_input_joystick_state(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_INPUT_JOY_STATE_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE, &param, NULL)) {
        return _err_response(req_id, "get_input_joystick_state failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    JsonArray *ports_arr = json_array_sized_new(2);
    for (int i = 0; i < 2; i++) {
        JsonObject *po = json_object_new();
        json_object_set_int_member(po,     "index",        (gint64)i);
        json_object_set_boolean_member(po, "connected",
                                       param.port[i].connected ? TRUE : FALSE);
        json_object_set_int_member(po,     "state_bits",   (gint64)param.port[i].state_bits);
        json_object_set_int_member(po,     "native_state", (gint64)param.port[i].native_state);
        json_object_set_string_member(po,  "device_name",  param.port[i].device_name);
        json_array_add_object_element(ports_arr, po);
    }
    json_object_set_array_member(resp, "ports", ports_arr);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_frame_framebuffer_info` handler - V1.D.4 Resource pro
 *        `emulator://frame/framebuffer/info`.
 *
 * Shape + dirty flag + 16-entry palette. pixel_format "index8" je jediná
 * varianta v aktuální verzi (Sharp MZ nativní). Klient si může cachovat
 * width/height (= compile-time konstanty), ale paletu má číst čerstvě
 * (= měnitelná za runtime).
 *
 * Layout response:
 *   {"width": 928, "height": 288, "last_screen_id": int,
 *    "framebuffer_state": int, "dirty": bool, "pixel_format": "index8",
 *    "bytes_per_pixel": 1, "has_palette": true,
 *    "palette": [0xRRGGBB, ...] (16 entries)}
 */
static en_MCP_DISPATCH_RESULT _handle_get_frame_framebuffer_info(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_FRAME_FB_INFO_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO, &param, NULL)) {
        return _err_response(req_id, "get_frame_framebuffer_info failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp,    "width",             (gint64)param.width);
    json_object_set_int_member(resp,    "height",            (gint64)param.height);
    json_object_set_int_member(resp,    "last_screen_id",    (gint64)param.last_screen_id);
    json_object_set_int_member(resp,    "framebuffer_state", (gint64)param.framebuffer_state);
    json_object_set_boolean_member(resp,"dirty",             param.dirty ? TRUE : FALSE);
    json_object_set_string_member(resp, "pixel_format",      "index8");
    json_object_set_int_member(resp,    "bytes_per_pixel",   (gint64)param.bytes_per_pixel);
    json_object_set_boolean_member(resp,"has_palette",
                                   param.has_palette ? TRUE : FALSE);
    JsonArray *pal_arr = json_array_sized_new(param.palette_size);
    for (uint32_t i = 0; i < param.palette_size; i++) {
        json_array_add_int_element(pal_arr, (gint64)param.palette[i]);
    }
    json_object_set_array_member(resp, "palette", pal_arr);
    json_object_set_int_member(resp,   "palette_size", (gint64)param.palette_size);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_frame_screenshot_raw` handler - V1.D.4 Resource pro
 *        `emulator://frame/screenshot.raw`.
 *
 * RGBA8888 raw pixel buffer base64-zakódovaný. Dispatch alokuje
 * intermediate buffer pro celý 928x288x4 (= cca 1 MB) a předává handleru
 * k vyplnění.
 *
 * V1.E.6.C - headless fallback (mzdos request 0006):
 * Pokud SDL snapshot není dostupný (= headless mode bez SDL render
 * threadu, nebo race mezi publish/consume), handler v emu vlákně
 * fallbackuje na GDG live buffer. Klient pozná podle `fallback_source`
 * pole, který zdroj byl použit.
 *
 * Layout response (available=true):
 *   {"available": true, "width": 928, "height": 288,
 *    "bytes_per_pixel": 4, "pixel_format": "rgba8888",
 *    "downscale_factor": 1, "source_screen_id": int,
 *    "fallback_source": "sdl_snapshot" | "gdg_live",
 *    "data_b64": "<base64 RGBA bytes>"}
 *   pokud available=false:
 *   {"available": false, "reason": str}
 */
static en_MCP_DISPATCH_RESULT _handle_get_frame_screenshot_raw(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    /* Alokujeme buffer dynamicky (= velký, na stack se nevejde). Cap
     * 4 MB rezerva pro factor=1 (max ~928*288*4 = 1.07 MB pro MZ-800). */
    const size_t cap = 4 * 1024 * 1024;
    uint8_t *buf = (uint8_t *)g_malloc(cap);
    if (!buf) {
        return _err_response(req_id, "screenshot_raw alloc failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM param;
    memset(&param, 0, sizeof(param));
    param.buffer           = buf;
    param.buffer_capacity  = cap;
    param.downscale_factor = 1;
    if (!_submit_dbgapi(DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW, &param, NULL)) {
        g_free(buf);
        return _err_response(req_id, "get_frame_screenshot_raw failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp,  "reason",
                                       "Framebuffer not yet rendered");
        g_free(buf);
        return _ok_response(req_id, resp, out_response);
    }
    gchar *b64 = g_base64_encode(buf, param.buffer_size);
    g_free(buf);
    /* Mapování enum (uint8_t) -> string field pro klienta. Defaultně
     * "sdl_snapshot" (= pre-V1.E.6.C kompatibilní behavior s memset()
     * struktury na 0). */
    const char *fallback_str = "sdl_snapshot";
    if (param.fallback_source == SCREENSHOT_SRC_GDG_LIVE) {
        fallback_str = "gdg_live";
    }
    json_object_set_boolean_member(resp, "available",         TRUE);
    json_object_set_int_member(resp,     "width",             (gint64)param.width);
    json_object_set_int_member(resp,     "height",            (gint64)param.height);
    json_object_set_int_member(resp,     "bytes_per_pixel",   (gint64)param.bytes_per_pixel);
    json_object_set_string_member(resp,  "pixel_format",      "rgba8888");
    json_object_set_int_member(resp,     "downscale_factor",  (gint64)param.downscale_factor);
    json_object_set_int_member(resp,     "source_screen_id",  (gint64)param.source_screen_id);
    json_object_set_string_member(resp,  "fallback_source",   fallback_str);
    json_object_set_string_member(resp,  "data_b64",          b64 ? b64 : "");
    g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_frame_screenshot` handler - Resource pro
 *        `emulator://frame/screenshot`.
 *
 * PNG screenshot enkódovaný přes stb_image_write.h (viz
 * src/emulator/debugger/png_encode.{c,h}). Handler v emu vlákně
 * zenkóduje plný framebuffer (= stejný obsah jako screenshot.raw, jen
 * PNG kontejner) a vrátí glib-alokovaný PNG stream, který dispatch po
 * base64 enkódování uvolní přes g_free.
 *
 * Layout response (available=true):
 *   {"available": true, "format": "png", "width": N, "height": N,
 *    "byte_size": N, "data_b64": "<base64 PNG stream>"}
 *   pokud available=false:
 *   {"available": false, "reason": str}
 */
static en_MCP_DISPATCH_RESULT _handle_get_frame_screenshot(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG, &param, NULL)) {
        return _err_response(req_id, "get_frame_screenshot failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp,  "reason",    param.reason);
        /* buffer NULL při available=0 - nic neuvolňujeme. */
        return _ok_response(req_id, resp, out_response);
    }
    /* Handler alokoval PNG stream (glib) - po base64 ho uvolníme. */
    gchar *b64 = g_base64_encode(param.buffer, param.buffer_size);
    g_free(param.buffer);
    json_object_set_boolean_member(resp, "available", TRUE);
    json_object_set_string_member(resp,  "format",    "png");
    json_object_set_int_member(resp,     "width",     (gint64)param.width);
    json_object_set_int_member(resp,     "height",    (gint64)param.height);
    json_object_set_int_member(resp,     "byte_size", (gint64)param.buffer_size);
    json_object_set_string_member(resp,  "data_b64",  b64 ? b64 : "");
    g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `screenshot_save_to_file` handler - server-side zápis PNG na disk.
 *
 * Reakce na mzdos request 0009. `get_frame_screenshot*` posílá PNG/RGBA
 * jako base64 přes TCP, což je problematické (velikost payloadu, base64
 * v kontextu AI klienta). Tento command vyrenderuje PNG stejně jako
 * `get_frame_screenshot`, ale místo base64 ho zapíše přímo do souboru
 * `path` na disku serveru. Response je malý (= imunní vůči transportu
 * i timeoutu); AI klient pak soubor přečte z disku.
 *
 * Parametry (data):
 *   - `path`   (string, povinné)   - cílová cesta na disku serveru
 *   - `format` (string, volitelné) - jen "png" (default); jiné = chyba
 *
 * Layout response (available=true):
 *   {"available": true, "path": str, "format": "png",
 *    "width": N, "height": N, "byte_size": N}
 *   available=false (framebuffer ještě nevykreslen):
 *   {"available": false, "reason": str}
 *
 * Bezpečnost: V0 profil "wild" = bez whitelistu cest (path enforcement
 * až V1.A, viz mcp_settings_dialog). Zápis je atomický (g_file_set_contents,
 * temp + rename); chyba IO vrací error response.
 */
static en_MCP_DISPATCH_RESULT _handle_screenshot_save_to_file(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);

    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Provide 'path' (string)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    char *path = _obj_str_dup(data_obj, "path");
    if (!path || path[0] == '\0') {
        g_free(path);
        return _err_response(req_id, "Provide 'path' (string)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* format volitelný; zatím podporujeme jen PNG. */
    char *format = _obj_str_dup(data_obj, "format");
    if (format && format[0] != '\0' &&
        g_ascii_strcasecmp(format, "png") != 0) {
        g_free(path);
        g_free(format);
        return _err_response(req_id, "Only format 'png' is supported",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    g_free(format);

    /* PNG capture - stejná emu-thread cesta jako get_frame_screenshot. */
    st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG, &param, NULL)) {
        g_free(path);
        return _err_response(req_id, "get_frame_screenshot failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (!param.available) {
        JsonObject *resp = json_object_new();
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp,  "reason",    param.reason);
        /* buffer NULL při available=0 - nic neuvolňujeme. */
        g_free(path);
        return _ok_response(req_id, resp, out_response);
    }

    /* Zápis PNG streamu na disk. param.buffer vlastní handler; po zápisu
     * (i při chybě) ho uvolníme. */
    GError *werr = NULL;
    gboolean wrote = g_file_set_contents(path, (const gchar *)param.buffer,
                                         (gssize)param.buffer_size, &werr);
    g_free(param.buffer);
    if (!wrote) {
        char *msg = g_strdup_printf("Cannot write screenshot to '%s': %s",
                                    path, werr ? werr->message : "unknown");
        if (werr) g_error_free(werr);
        g_free(path);
        en_MCP_DISPATCH_RESULT r = _err_response(req_id, msg,
                                       MCP_DISPATCH_EMU_ERROR, out_response);
        g_free(msg);
        return r;
    }

    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "available", TRUE);
    json_object_set_string_member(resp,  "path",      path);
    json_object_set_string_member(resp,  "format",    "png");
    json_object_set_int_member(resp,     "width",     (gint64)param.width);
    json_object_set_int_member(resp,     "height",    (gint64)param.height);
    json_object_set_int_member(resp,     "byte_size", (gint64)param.buffer_size);
    g_free(path);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_video_text_dump` handler - V1.D.4 Resource pro
 *        `emulator://video/text_dump`.
 *
 * 40x25 text dump pro MZ-700 mode. chars[] obsahuje raw Sharp ASCII
 * bajty (= klient si může převést na UTF-8 přes Sharp ASCII mapping).
 * attributes[] obsahuje raw attribute byte per cell.
 *
 * Layout response (available=true):
 *   {"available": true, "platform": str, "cols": 40, "rows": 25,
 *    "cell_count": 1000, "chars_b64": "<base64 1000 bytes>",
 *    "attributes_b64": "<base64 1000 bytes>"}
 *   pokud available=false (MZ-800 v 800 mode):
 *   {"available": false, "platform": str, "reason": str}
 */
static en_MCP_DISPATCH_RESULT _handle_get_video_text_dump(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_VIDEO_TEXT_DUMP_PARAM param;
    memset(&param, 0, sizeof(param));
    if (!_submit_dbgapi(DBGAPI_CMD_GET_VIDEO_TEXT_DUMP, &param, NULL)) {
        return _err_response(req_id, "get_video_text_dump failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "platform", param.platform);
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp,  "reason",    param.reason);
        return _ok_response(req_id, resp, out_response);
    }
    json_object_set_boolean_member(resp, "available",  TRUE);
    json_object_set_int_member(resp,     "cols",       (gint64)param.cols);
    json_object_set_int_member(resp,     "rows",       (gint64)param.rows);
    json_object_set_int_member(resp,     "cell_count", (gint64)param.cell_count);
    gchar *chars_b64 = g_base64_encode(param.chars,      param.cell_count);
    gchar *attrs_b64 = g_base64_encode(param.attributes, param.cell_count);
    json_object_set_string_member(resp, "chars_b64",      chars_b64 ? chars_b64 : "");
    json_object_set_string_member(resp, "attributes_b64", attrs_b64 ? attrs_b64 : "");
    g_free(chars_b64);
    g_free(attrs_b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_watch_snapshot` handler - V1.D.2.C Resource backing pro
 *        `emulator://watch/snapshot/{name}`.
 *
 * Lookup statistik watch řádku podle jména v EMU-side mirror
 * (`watch_emu_cache`). UI vlákno publikuje stav jednou per frame; tento
 * handler předá jméno přes DBGAPI_CMD_GET_WATCH_SNAPSHOT do EMU vlákna,
 * které mirror přečte. Akceptováno 1-frame stale per scope V1.D.2.C.
 *
 * Parametry v `data` poli requestu:
 *   - `name` (string, povinný) - case-sensitive jméno watch řádku
 *
 * Layout response:
 *   - pokud nalezeno (= found):
 *     {"name": str, "found": true, "row_id": int, "type": str,
 *      "snapshot_active": bool, "min_max_valid": bool,
 *      "snap_int": int, "cur_int": int, "delta_int": int,
 *      "min_int": int, "max_int": int, "change_count": int}
 *   - pokud nenalezeno (= žádný takový řádek v mirroru):
 *     {"name": str, "found": false}
 *
 * Chyby:
 *   - INVALID_PARAMS pokud `name` chybí nebo je prázdný
 *   - EMU_ERROR pokud DBGAPI submit selže
 *
 * Ownership: caller (jsonl_msg) drží request; handler vrací heap response
 * řetězec přes _ok_response/_err_response.
 */
static en_MCP_DISPATCH_RESULT _handle_get_watch_snapshot(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data_obj = json_node_get_object(data_node);
    const char *name = NULL;
    if (json_object_has_member(data_obj, "name")) {
        name = json_object_get_string_member(data_obj, "name");
    }
    if (!name || !name[0]) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_WATCH_SNAPSHOT_PARAM param;
    memset(&param, 0, sizeof(param));
    g_strlcpy(param.name, name, sizeof(param.name));

    if (!_submit_dbgapi(DBGAPI_CMD_GET_WATCH_SNAPSHOT, &param, NULL)) {
        return _err_response(req_id, "get_watch_snapshot failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "name", name);
    if (!param.found) {
        json_object_set_boolean_member(resp, "found", FALSE);
        return _ok_response(req_id, resp, out_response);
    }
    json_object_set_boolean_member(resp, "found", TRUE);
    json_object_set_int_member(resp, "row_id", (gint64)param.row_id);
    /* type_snap je en_WATCH_TYPE; DBGAPI helper bere en_DBGAPI_WATCH_TYPE,
     * obě enum hodnoty jsou hodnotově shodné (0..13). */
    json_object_set_string_member(resp, "type",
        _watch_type_to_str((en_DBGAPI_WATCH_TYPE)param.type_snap));
    json_object_set_boolean_member(resp, "snapshot_active",
        param.snapshot_active ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "min_max_valid",
        param.min_max_valid ? TRUE : FALSE);
    json_object_set_int_member(resp, "snap_int",     (gint64)param.snap_int);
    json_object_set_int_member(resp, "cur_int",      (gint64)param.cur_int);
    json_object_set_int_member(resp, "delta_int",    (gint64)param.delta_int);
    json_object_set_int_member(resp, "min_int",      (gint64)param.min_int);
    json_object_set_int_member(resp, "max_int",      (gint64)param.max_int);
    json_object_set_int_member(resp, "change_count",
        (gint64)param.change_count);
    return _ok_response(req_id, resp, out_response);
}


/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

en_MCP_DISPATCH_RESULT mcp_dispatch_request(const st_JSONL_MESSAGE *req,
                                            char **out_response) {
    if (!req || !out_response) {
        if (out_response) *out_response = NULL;
        return MCP_DISPATCH_NOT_A_REQUEST;
    }
    *out_response = NULL;

    if (jsonl_msg_get_type(req) != JSONL_MSG_REQUEST) {
        return MCP_DISPATCH_NOT_A_REQUEST;
    }

    const char *cmd_name = jsonl_msg_get_cmd(req);
    int64_t     req_id   = jsonl_msg_get_req_id(req);
    if (!cmd_name || cmd_name[0] == '\0') {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    for (size_t i = 0; g_cmd_map[i].name != NULL; i++) {
        if (strcmp(g_cmd_map[i].name, cmd_name) == 0) {
            return g_cmd_map[i].handler(req, out_response);
        }
    }

    return _err_response(req_id, "Unknown command",
                         MCP_DISPATCH_UNKNOWN_CMD, out_response);
}


/**
 * @brief Detekce aktuálně emulované MZ platformy + módu + TV systému.
 *
 * Platforma je zjištěna z compile-time `MZARCH_NAME` (= "mz700" /
 * "mz800" / "mz1500"; PAL/NTSC NErozlišuje - oba MZ-700 targety mají
 * "mz700"). PAL vs NTSC rozlišuje výhradně `MZTVSYS` (= PAL=50 Hz,
 * NTSC=60 Hz), viz tv_system_out. Mód (native vs compat700) je runtime
 * z GDG `regDMD` bit.
 *
 * Časování informace je klíčová pro klienta - MZ-700 PAL vs NTSC mají
 * **rozdílné krystaly + CTC0 deličky** (= jiné BASIC tempo, jiné
 * raster intervals). MZ-1500 ≠ MZ-700 NTSC v CTC0 deličce i přes
 * stejný 14.318 MHz krystal.
 *
 * @param[out] platform_out   "mz700" / "mz800" / "mz1500"
 *                            (= MZARCH_NAME; PAL/NTSC nerozlišuje,
 *                            viz tv_system_out).
 * @param[out] full_name_out  "MZ-700 (PAL)" / "MZ-700 (NTSC)" / "MZ-800" /
 *                            "MZ-1500" (= g_mzarch_full_name).
 * @param[out] mode_out       "native" / "compat700" (= runtime GDG regDMD).
 * @param[out] tv_system_out  "PAL" / "NTSC".
 * @param[out] framerate_hz_out  50 / 60 (= per MZTVSYS).
 * @param[out] pxclk_hz_out   Pixel clock v Hz (= g_mzarch_platform_pxclk).
 */
static void _dispatch_detect_platform(const char **platform_out,
                                      const char **full_name_out,
                                      const char **mode_out,
                                      const char **tv_system_out,
                                      int *framerate_hz_out,
                                      uint32_t *pxclk_hz_out) {
#ifdef MZ800EMU_MCP_TEST_BUILD
    /* V standalone test buildu nemáme přístup k g_mzarch_* / g_gdg
     * (mzarch_platform.o ani gdg.o nejsou linkovány). Vrátíme
     * deterministické placeholder hodnoty - real platform/info handlery
     * se v testech neověřují (= jsou závislé na live emu state). */
    *platform_out     = "test";
    *full_name_out    = "TEST";
    *mode_out         = "native";
    *tv_system_out    = "PAL";
    *framerate_hz_out = 50;
    *pxclk_hz_out     = 0;
#else
    *platform_out  = g_mzarch_platform_name;
    *full_name_out = g_mzarch_full_name;
    *pxclk_hz_out  = g_mzarch_platform_pxclk;

    /* Runtime z g_mzhal (mzhal 11f). */
    *tv_system_out    = g_mzhal.tvsys_name;
    *framerate_hz_out = (int)g_mzhal.video_screens_per_sec;

    if (g_mzhal.arch == 700) {
        *mode_out = "native";   /* MZ-700 = vždy sám sebou */
    } else if (g_mzhal.arch == 800) {
        /* DMD bit 3 = 1: native 800 mode, 0: 700 compat */
        *mode_out = ((g_gdg.regDMD & 0x08) != 0) ? "native" : "compat700";
    } else if (g_mzhal.arch == 1500) {
        /* MZ-1500 regDMD bit 0 = 1: 1500 native, 0: 700 sim (= reset default) */
        *mode_out = ((g_gdg.regDMD & 0x01) != 0) ? "native" : "compat700";
    } else {
        *mode_out = "unknown";
    }
#endif /* MZ800EMU_MCP_TEST_BUILD */
}


en_MCP_DISPATCH_RESULT mcp_dispatch_build_hello(char **out_hello) {
    if (!out_hello) return MCP_DISPATCH_NOT_A_REQUEST;
    *out_hello = NULL;

    /* Capabilities JsonObject - vlastní node který poté předáme
     * jsonl_build_hello (která dělá deep-copy). Po návratu uvolníme.
     *
     * Klient potřebuje vědět JAKÝ MZ HW je emulovaný ihned po
     * připojení (= před prvním tool/resource callem), proto platform
     * + mode patří do hello capabilities.
     */
    const char *platform     = "unknown";
    const char *full_name    = "unknown";
    const char *mode         = "unknown";
    const char *tv_system    = "unknown";
    int         framerate_hz = 0;
    uint32_t    pxclk_hz     = 0;
    _dispatch_detect_platform(&platform, &full_name, &mode, &tv_system,
                              &framerate_hz, &pxclk_hz);

    JsonObject *caps_obj = json_object_new();
    json_object_set_string_member(caps_obj, "protocol",     "JSONL/MCP");
    json_object_set_string_member(caps_obj, "transport",    "pipe");
    json_object_set_int_member(caps_obj,    "phase",        0);   /* V0 */
    json_object_set_string_member(caps_obj, "platform",     platform);
    json_object_set_string_member(caps_obj, "full_name",    full_name);
    json_object_set_string_member(caps_obj, "mode",         mode);
    json_object_set_string_member(caps_obj, "tv_system",    tv_system);
    json_object_set_int_member(caps_obj,    "framerate_hz", framerate_hz);
    json_object_set_int_member(caps_obj,    "pxclk_hz",     (gint64)pxclk_hz);

    JsonNode *caps_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(caps_node, caps_obj);

    /* Pole získáme přes accessor (= rezistentní na pre-init stav,
     * třeba unit testy bez init - vrátí prázdný NULL-terminated vector). */
    const char *const *cmd_names = mcp_dispatch_get_supported_commands();
    char *line = jsonl_build_hello(MCP_DISPATCH_PROTOCOL_VERSION,
                                   caps_node,
                                   cmd_names);
    json_node_free(caps_node);   /* uvolní i caps_obj (take_object) */

    if (!line) {
        return MCP_DISPATCH_ALLOC_ERROR;
    }
    *out_hello = line;
    return MCP_DISPATCH_OK;
}


const char *const *mcp_dispatch_get_supported_commands(void) {
    if (g_supported_cmd_names == NULL) {
        /* Pre-init / post-shutdown stav - vrátíme prázdný NULL-only
         * vector, aby caller mohl rovnou iterovat for(p; *p; p++). */
        return g_empty_cmd_names;
    }
    return (const char *const *)g_supported_cmd_names;
}


void mcp_dispatch_init(void) {
    if (g_supported_cmd_names != NULL) {
        /* Idempotence - druhé volání je no-op. */
        return;
    }

    /* Spočítáme entries v g_cmd_map[] (NULL-terminated). */
    gsize count = 0;
    for (gsize i = 0; g_cmd_map[i].name != NULL; i++) {
        count++;
    }

    /* Alokace + naplnění. Posledni slot je NULL terminator (= caller
     * iteruje for(p = arr; *p; p++)). */
    g_supported_cmd_names = g_new0(char *, count + 1);
    for (gsize i = 0; i < count; i++) {
        g_supported_cmd_names[i] = g_strdup(g_cmd_map[i].name);
    }
    g_supported_cmd_names[count] = NULL;
    g_supported_cmd_names_count = count;
}


void mcp_dispatch_shutdown(void) {
    if (g_supported_cmd_names == NULL) {
        return;
    }
    for (gsize i = 0; i < g_supported_cmd_names_count; i++) {
        g_free(g_supported_cmd_names[i]);
    }
    g_free(g_supported_cmd_names);
    g_supported_cmd_names = NULL;
    g_supported_cmd_names_count = 0;
}


void mcp_dispatch_set_transport_kind(en_MCP_DISPATCH_TRANSPORT kind) {
    g_transport_kind = kind;
}


en_MCP_DISPATCH_TRANSPORT mcp_dispatch_get_transport_kind(void) {
    return g_transport_kind;
}


void mcp_dispatch_set_shutdown_callback(mcp_dispatch_shutdown_cb_fn cb) {
    g_shutdown_cb = cb;
}


/* ====================================================================== */
/* mzdos-support 0007 - Direct memory region read (2 Tools)              */
/* ====================================================================== */

#include "../debugger/dbgapi_regions.h"


/**
 * @brief Konvertuje en_REGION_KIND enum na ASCII jméno pro JSON.
 *
 * Zachovává názvy z dbgapi_regions.h doc - klient může parsovat
 * deterministicky.
 */
static const char *_region_kind_to_str(int kind) {
    switch ((en_REGION_KIND)kind) {
        case REGION_KIND_LOGICAL:           return "logical";
        case REGION_KIND_RAM:               return "ram";
        case REGION_KIND_ROM_LOWER:         return "rom_lower";
        case REGION_KIND_ROM_UPPER:         return "rom_upper";
        case REGION_KIND_CGROM:             return "cgrom";
        case REGION_KIND_CGRAM_700:         return "cgram_700";
        case REGION_KIND_VRAM_700_CHAR:     return "vram_700_char";
        case REGION_KIND_VRAM_700_ATTR:     return "vram_700_attr";
        case REGION_KIND_VRAM_PHYS_PLANE:   return "vram_phys_plane";
        case REGION_KIND_PCG_1500:          return "pcg_1500";
        case REGION_KIND_MEMEXT_RAM:        return "memext_ram";
        case REGION_KIND_MEMEXT_FLASH:      return "memext_flash";
        case REGION_KIND_RAMDISK_STD:       return "ramdisk_std";
        case REGION_KIND_RAMDISK_PEZIK:     return "ramdisk_pezik";
        case REGION_KIND_PROHIBITED_SHADOW: return "prohibited_shadow";
        case REGION_KIND_COUNT:             break;
    }
    return "unknown";
}


/**
 * @brief `regions_list` handler - enumerate všech regionů.
 *
 * Bez params. Vrátí JSON pole regionů s metadaty (id, kind, sub_id,
 * name, logical_base, size, writable, connected, mapped_now). Klient
 * si vybere region a použije ID v `region_read`. ID jsou stable v
 * rámci session, NE per HW reconfigure (= memext/ramdisk attach/detach
 * invaliduje, klient musí re-enumerate).
 */
#define MCP_REGIONS_MAX 128
static en_MCP_DISPATCH_RESULT _handle_regions_list(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_REGION_DESC descs[MCP_REGIONS_MAX];
    memset(descs, 0, sizeof(descs));
    st_DBGAPI_REGIONS_ENUM_PARAM param = {
        .out = descs,
        .max_count = MCP_REGIONS_MAX,
        .out_count = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_REGIONS_ENUMERATE, &param, NULL)) {
        return _err_response(req_id, "REGIONS_ENUMERATE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    JsonArray *arr = json_array_new();
    for (int i = 0; i < param.out_count; i++) {
        const st_REGION_DESC *d = &descs[i];
        JsonObject *r = json_object_new();
        json_object_set_int_member(r, "id", d->id);
        json_object_set_string_member(r, "kind", _region_kind_to_str(d->kind));
        json_object_set_int_member(r, "sub_id", d->sub_id);
        json_object_set_string_member(r, "name", d->name);
        if (d->logical_base != 0xFFFFFFFFu) {
            json_object_set_int_member(r, "logical_base",
                                       (gint64)d->logical_base);
        } else {
            json_object_set_null_member(r, "logical_base");
        }
        json_object_set_int_member(r, "size", (gint64)d->size);
        json_object_set_boolean_member(r, "writable", d->writable ? TRUE : FALSE);
        json_object_set_boolean_member(r, "connected", d->connected ? TRUE : FALSE);
        json_object_set_boolean_member(r, "mapped_now", d->mapped_now ? TRUE : FALSE);
        json_array_add_object_element(arr, r);
    }
    json_object_set_array_member(data, "regions", arr);
    json_object_set_int_member(data, "count", param.out_count);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `region_read` handler - raw read N bajtů z regionu.
 *
 * Parametry: `region_id` (= z poslední `regions_list`), `offset`
 * (= 0..size-1), `length` (= 1..65536, clamp na size regionu).
 *
 * Response: `{region_id, offset, length, data_b64, kind}`. `length`
 * v response = skutečně přečtená délka (= clamped). Při chybě
 * `{error: "..."}`.
 *
 * No-side-effect read - bypass Z80 banking (= klient vidí raw paměti
 * jako GUI Memory browser, bez disruptivních io_write).
 */
static en_MCP_DISPATCH_RESULT _handle_region_read(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    gint64 region_id = _obj_int_or(obj, "region_id", -1);
    gint64 offset    = _obj_int_or(obj, "offset",    0);
    gint64 length    = _obj_int_or(obj, "length",    -1);
    if (region_id < 0 || region_id > MCP_REGIONS_MAX
            || offset < 0 || offset > 0xFFFFFFFFu
            || length < 1 || length > 65536) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint8_t *buf = g_malloc0((gsize)length);
    st_DBGAPI_REGIONS_READ_PARAM param = {
        .region_id = (int)region_id,
        .offset = (uint32_t)offset,
        .buf = buf,
        .len = (uint32_t)length,
        .out_count = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_REGIONS_READ, &param, NULL)) {
        /* _submit_dbgapi vrací (out_count >= 0). param.out_count rozliší
         * příčinu: backend nastaví -1 když region_id/offset/region je
         * neplatný (= read proběhl, ale selhal); ponechá initial 0 když
         * příkaz vůbec neproběhl (= emu nedostupný / fronta plná / timeout).
         * Cache se plní lazy (viz dbgapi_regions lookup_region), takže
         * prázdná enumerace už není příčinou - selhání = opravdu bad ID. */
        en_MCP_DISPATCH_RESULT rc = (param.out_count < 0)
            ? MCP_DISPATCH_INVALID_PARAMS : MCP_DISPATCH_EMU_ERROR;
        const char *msg = (param.out_count < 0)
            ? "region read failed: invalid region_id, offset, or "
              "disconnected region (use regions_list to enumerate valid IDs)"
            : "region read failed: emulator unavailable";
        g_free(buf);
        return _err_response(req_id, msg, rc, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "region_id", region_id);
    json_object_set_int_member(data, "offset", offset);
    json_object_set_int_member(data, "length", param.out_count);
    gchar *b64 = g_base64_encode(buf, (gsize)param.out_count);
    json_object_set_string_member(data, "data_b64", b64);
    g_free(b64);
    g_free(buf);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `get_platform_info` handler - dynamic platform + mode detekce.
 *
 * Bez params. Vrátí JSON `{platform, mode, mzarch, rom_version}`:
 *  - `platform`: "mz700" / "mz800" / "mz1500" (= compile-time MZARCH)
 *  - `mode`: "native" / "compat700" (= runtime z GDG regDMD bit)
 *    - MZ-700: vždy "native"
 *    - MZ-800: bit 3 = 1 native, 0 compat700
 *    - MZ-1500: bit 0 = 1 native, 0 compat700 (= reset default)
 *  - `mzarch`: identický s `platform` (= legacy field pro V0.B.7 klienty)
 *  - `rom_version`: TBD (= V2 z ROM header signature)
 *
 * Klíčové pro MCP klient ihned po připojení - klient si nesmí předpokládat
 * konkrétní platformu nebo mode bez vyžádání tohoto info.
 */
static en_MCP_DISPATCH_RESULT _handle_get_platform_info(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    const char *platform     = "unknown";
    const char *full_name    = "unknown";
    const char *mode         = "unknown";
    const char *tv_system    = "unknown";
    int         framerate_hz = 0;
    uint32_t    pxclk_hz     = 0;
    _dispatch_detect_platform(&platform, &full_name, &mode, &tv_system,
                              &framerate_hz, &pxclk_hz);
    JsonObject *data = json_object_new();
    json_object_set_string_member(data, "platform",     platform);
    json_object_set_string_member(data, "full_name",    full_name);
    json_object_set_string_member(data, "mode",         mode);
    json_object_set_string_member(data, "tv_system",    tv_system);
    json_object_set_int_member(data,    "framerate_hz", framerate_hz);
    json_object_set_int_member(data,    "pxclk_hz",     (gint64)pxclk_hz);
    json_object_set_string_member(data, "mzarch",       platform);
#ifdef MZ800EMU_MCP_TEST_BUILD
    json_object_set_int_member(data,    "mzarch_numeric", 0);
#else
    json_object_set_int_member(data,    "mzarch_numeric",
                               (gint64)g_mzarch_platform_numeric);
#endif
    json_object_set_string_member(data, "rom_version",  "unknown");

    /* Capabilities - compile-time HW podpora dané platformy. Co je
     * zakompilováno do binárky, ne co je runtime attached. */
    JsonObject *caps = json_object_new();
    /* Runtime z g_mzhal (mzhal krok 8) - hodnoty per platforma beze
     * zmeny (hlidano golden fixtures platform_info). */
    json_object_set_boolean_member(caps, "has_pioz80",
                                   g_mzhal.have_pioz80 ? TRUE : FALSE);
    json_object_set_int_member(caps, "psg_count", (gint64)g_mzhal.psg_count);
    json_object_set_boolean_member(caps, "hwext_fdc",
                                   g_mzhal.have_fdc ? TRUE : FALSE);
    json_object_set_boolean_member(caps, "hwext_ide8",
                                   g_mzhal.have_ide8 ? TRUE : FALSE);
    json_object_set_boolean_member(caps, "hwext_ramdisk",
                                   g_mzhal.have_ramdisk ? TRUE : FALSE);
    json_object_set_boolean_member(caps, "hwext_qdisk",
                                   g_mzhal.have_qdisk ? TRUE : FALSE);
    json_object_set_string_member(caps, "cpu_model", "Z80");
    json_object_set_object_member(data, "capabilities", caps);

    /* Clock domains - per Sharp HW jsou všechny derivované z GDG base
     * clocku (= GDGCLK_BASE) přes integer dividery. CPU clock je
     * GDGCLK / GDGCLK2CPU_DIVIDER, CTC0 input je GDGCLK / CTC0_DIVIDER,
     * PSG input je CPU_CLOCK / 16 (= g_mzhal.psg_divider = 16 * CPU_DIVIDER).
     *
     * GDGCLK_BASE = simulovaná frekvence (= clean násobky pro
     * timing matematiku). GDGCLK_REAL_BASE = skutečná krystalová
     * frekvence (= 14.318 MHz NTSC, 17.734 MHz PAL).
     *
     * CTC1+CTC2 vstupy jsou cascade-driven (= výstup CTC0 nebo
     * per-platform routing); klient čte aktuální mode + count
     * v emulator://periph/i8253 Resource. */
    JsonObject *clocks = json_object_new();
    json_object_set_int_member(clocks, "gdg_base_hz",
                               (gint64)g_mzhal.gdgclk_base);
    json_object_set_int_member(clocks, "gdg_real_base_hz",
                               (gint64)g_mzhal.gdgclk_real_base);
    json_object_set_int_member(clocks, "cpu_hz",
                               (gint64)g_mzhal.cpu_hz);
    json_object_set_int_member(clocks, "cpu_divider",
                               (gint64)g_mzhal.gdgclk2cpu_divider);
    json_object_set_int_member(clocks, "ctc0_input_hz",
                               (gint64)g_mzhal.ctc0_input_hz);
    json_object_set_int_member(clocks, "ctc0_divider",
                               (gint64)g_mzhal.gdgclk_ctc0_divider);
    if (g_mzhal.psg_count > 0) {
        json_object_set_int_member(clocks, "psg_input_hz",
                                   (gint64)(g_mzhal.gdgclk_base / g_mzhal.psg_divider));
        json_object_set_int_member(clocks, "psg_divider",
                                   (gint64)g_mzhal.psg_divider);
    } else {
        json_object_set_null_member(clocks, "psg_input_hz");
        json_object_set_null_member(clocks, "psg_divider");
    }
    json_object_set_null_member(clocks, "ctc1_input_hz");
    json_object_set_null_member(clocks, "ctc2_input_hz");
    json_object_set_string_member(clocks, "ctc12_note",
                                  "cascade from CTC0 / per-platform routing; "
                                  "read emulator://periph/i8253 for runtime state");
    json_object_set_object_member(data, "clocks", clocks);

    /* Scanline / raster timing - per-platform compile-time konstanty.
     * Hodnoty zahrnují kompletní raster (= sync + blanking + display)
     * i logical display area (= canvas s border). */
    JsonObject *scanline = json_object_new();
    json_object_set_int_member(scanline, "screen_total_width_ticks",
                               (gint64)g_mzhal.video_screen_width);
    json_object_set_int_member(scanline, "screen_total_height_lines",
                               (gint64)g_mzhal.video_screen_height);
    json_object_set_int_member(scanline, "screen_total_ticks_per_frame",
                               (gint64)g_mzhal.video_screen_ticks);
    json_object_set_int_member(scanline, "screens_per_sec",
                               (gint64)g_mzhal.video_screens_per_sec);
    json_object_set_int_member(scanline, "display_width",
                               (gint64)g_mzhal.video_display_width);
    json_object_set_int_member(scanline, "display_height",
                               (gint64)g_mzhal.video_display_height);
    json_object_set_int_member(scanline, "canvas_width",
                               (gint64)g_mzhal.video_canvas_width);
    json_object_set_int_member(scanline, "canvas_height",
                               (gint64)g_mzhal.video_canvas_height);
    json_object_set_int_member(scanline, "border_left_width",
                               (gint64)g_mzhal.video_border_left_width);
    json_object_set_int_member(scanline, "border_right_width",
                               (gint64)g_mzhal.video_border_right_width);
    json_object_set_int_member(scanline, "border_top_height",
                               (gint64)g_mzhal.video_border_top_height);
    json_object_set_int_member(scanline, "border_bottom_height",
                               (gint64)g_mzhal.video_border_bottom_height);
    json_object_set_int_member(scanline, "h_sync_ticks",
                               (gint64)g_mzhal.video_h_sync_ticks);
    json_object_set_int_member(scanline, "h_back_porch_ticks",
                               (gint64)g_mzhal.video_h_back_porch_ticks);
    json_object_set_int_member(scanline, "h_front_porch_ticks",
                               (gint64)g_mzhal.video_h_front_porch_ticks);
    json_object_set_object_member(data, "scanline", scanline);

    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `region_write` handler - raw write N bajtů do regionu.
 *
 * Parametry: `region_id` (= z poslední `regions_list`), `offset`
 * (= 0..size-1), `data_b64` (= base64 bajty, max 65536 dekódovaných).
 *
 * Response: `{region_id, offset, length, written}`. `length` = skutečně
 * zapsaná délka (= clamped). `written` = true pokud aspoň 1 bajt
 * prošel; false pokud region read-only nebo disconnected.
 *
 * **Destruktivní.** Backend respektuje writable flagy:
 * REGION_KIND_MEMEXT_FLASH a REGION_KIND_PROHIBITED_SHADOW vrátí error.
 * Pro VRAM žádný automatický screen refresh.
 */
static en_MCP_DISPATCH_RESULT _handle_region_write(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    gint64 region_id = _obj_int_or(obj, "region_id", -1);
    gint64 offset    = _obj_int_or(obj, "offset",    0);
    if (region_id < 0 || region_id > MCP_REGIONS_MAX
            || offset < 0 || offset > 0xFFFFFFFFu) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!json_object_has_member(obj, "data_b64")) {
        return _err_response(req_id, "Invalid parameters (data_b64 missing)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *b64 = json_object_get_string_member(obj, "data_b64");
    if (!b64) {
        return _err_response(req_id, "Invalid parameters (data_b64 null)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gsize decoded_len = 0;
    guchar *decoded = g_base64_decode(b64, &decoded_len);
    if (!decoded || decoded_len == 0 || decoded_len > 65536) {
        g_free(decoded);
        return _err_response(req_id, "Invalid parameters (data_b64 decode)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_REGIONS_WRITE_PARAM param = {
        .region_id = (int)region_id,
        .offset = (uint32_t)offset,
        .data = decoded,
        .len = (uint32_t)decoded_len,
        .out_count = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_REGIONS_WRITE, &param, NULL)) {
        /* Stejná logika jako region_read: param.out_count rozliší bad
         * region/read-only (-1, příkaz proběhl) od emu nedostupný (0,
         * neproběhl). Cache se plní lazy, takže prázdná enumerace už není
         * příčinou selhání. */
        en_MCP_DISPATCH_RESULT rc = (param.out_count < 0)
            ? MCP_DISPATCH_INVALID_PARAMS : MCP_DISPATCH_EMU_ERROR;
        const char *msg = (param.out_count < 0)
            ? "region write failed: read-only, disconnected, or invalid "
              "region_id/offset (use regions_list to enumerate valid IDs)"
            : "region write failed: emulator unavailable";
        g_free(decoded);
        return _err_response(req_id, msg, rc, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "region_id", region_id);
    json_object_set_int_member(data, "offset", offset);
    json_object_set_int_member(data, "length", param.out_count);
    json_object_set_boolean_member(data, "written", param.out_count > 0);
    g_free(decoded);
    return _ok_response(req_id, data, out_response);
}


/* ====================================================================== */
/* V1.E.5 - Eventlog/TLOG Tools (6)                                       */
/* ====================================================================== */


/**
 * @brief `eventlog_start` handler - spustí event recording.
 *
 * Bez params. Forward na `DBGAPI_CMD_EVENTLOG_START`.
 */
static en_MCP_DISPATCH_RESULT _handle_eventlog_start(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_EVENTLOG_START, NULL, NULL)) {
        return _err_response(req_id, "EVENTLOG_START failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "started", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `eventlog_stop` handler - zastaví event recording.
 *
 * Recorded data zůstanou v ringu pro pozdější čtení.
 */
static en_MCP_DISPATCH_RESULT _handle_eventlog_stop(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_EVENTLOG_STOP, NULL, NULL)) {
        return _err_response(req_id, "EVENTLOG_STOP failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "stopped", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `eventlog_clear` handler - vyprázdní event ring buffer.
 *
 * Bez params. Nezastavuje recording (recording flag zůstává).
 */
static en_MCP_DISPATCH_RESULT _handle_eventlog_clear(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_EVENTLOG_CLEAR, NULL, NULL)) {
        return _err_response(req_id, "EVENTLOG_CLEAR failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "cleared", TRUE);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `eventlog_set_capacity` handler - resize ringu.
 *
 * Backend clampuje capacity na [EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY].
 * Response obsahuje `capacity_after` (= skutečně nastavená velikost).
 */
static en_MCP_DISPATCH_RESULT _handle_eventlog_set_capacity(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    gint64 capacity = _obj_int_or(obj, "capacity", -1);
    if (capacity < 0 || capacity > 0x7FFFFFFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_EVENTLOG_CAPACITY_PARAM param = {
        .capacity = (uint32_t)capacity,
        .capacity_after = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_EVENTLOG_SET_CAPACITY, &param, NULL)) {
        return _err_response(req_id, "EVENTLOG_SET_CAPACITY failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "capacity_after",
                               (gint64)param.capacity_after);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `eventlog_set_mask` handler - filter categories bitmask.
 *
 * Bitmask je 64-bit unsigned (= per kategorie 1 bit). JSON nemá
 * nativní 64-bit unsigned typ - akceptujeme dvě formy:
 *
 *  - **integer** v poli `mask` (= JSON number, max 63 bitů kvůli
 *    signed gint64 interpretaci; bit 63 by se chytil jako sign +
 *    overflow)
 *  - **hex string** v poli `mask` (= např. `"0xFFFFFFFFFFFFFFFF"`
 *    nebo `"ffff_ffff"`; podporuje plný 64-bit rozsah včetně bitu 63)
 *
 * Response echo `mask` jako 64-bit unsigned vždy v hex stringu pro
 * konzistenci (= klient nemusí spekulovat).
 *
 * Parametry data:
 *  - `mask` (int nebo hex string) - 64-bit bitmask
 *
 * Response payload:
 *  - `mask_hex` (string) - výsledná hodnota jako `"0xN..."`
 *
 * Side effect: mutuje active filter; following eventlog emity respektují
 * nový mask.
 */
static en_MCP_DISPATCH_RESULT _handle_eventlog_set_mask(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    if (!json_object_has_member(obj, "mask")) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint64_t mask = 0;
    JsonNode *mask_node = json_object_get_member(obj, "mask");
    GType vtype = json_node_get_value_type(mask_node);
    if (vtype == G_TYPE_STRING) {
        /* Hex string parse - akceptujeme "0xN...", "N..." (= base 16),
         * podtržítka jako visual separator strip. Plný 64-bit rozsah. */
        const char *s = json_node_get_string(mask_node);
        if (!s) {
            return _err_response(req_id, "Invalid parameters (mask null)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        char buf[24] = {0};
        size_t bi = 0;
        for (size_t i = 0; s[i] && bi < sizeof(buf) - 1; i++) {
            if (s[i] != '_') buf[bi++] = s[i];
        }
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = g_ascii_strtoull(buf, &end, 16);
        if (errno != 0 || (end && *end != '\0' && *end != '\0')) {
            return _err_response(req_id,
                                 "Invalid parameters (mask hex parse)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        mask = (uint64_t)parsed;
    } else {
        /* Numeric path - signed gint64, klient může poslat max 63 bitů. */
        gint64 m = json_object_get_int_member(obj, "mask");
        mask = (uint64_t)m;
    }
    st_DBGAPI_EVENTLOG_MASK_PARAM param = {
        .mask = mask,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_EVENTLOG_SET_MASK, &param, NULL)) {
        return _err_response(req_id, "EVENTLOG_SET_MASK failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    char hex_out[24];
    g_snprintf(hex_out, sizeof(hex_out), "0x%016llX",
               (unsigned long long)mask);
    json_object_set_string_member(data, "mask_hex", hex_out);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `eventlog_get_event` handler - načte event[idx] z ringu.
 *
 * Pokud idx >= count, response `{available: false, idx}`.
 * Jinak vrátí plný event payload per `st_DBGAPI_EVENTLOG_GET_EVENT_PARAM`.
 */
static en_MCP_DISPATCH_RESULT _handle_eventlog_get_event(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    if (!data_node || json_node_get_node_type(data_node) != JSON_NODE_OBJECT) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obj = json_node_get_object(data_node);
    gint64 idx = _obj_int_or(obj, "idx", -1);
    if (idx < 0 || idx > 0x7FFFFFFF) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_EVENTLOG_GET_EVENT_PARAM param = { 0 };
    param.idx = (uint32_t)idx;
    if (!_submit_dbgapi(DBGAPI_CMD_EVENTLOG_GET_EVENT, &param, NULL)) {
        return _err_response(req_id, "EVENTLOG_GET_EVENT failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "idx", idx);
    if (!param.found) {
        json_object_set_boolean_member(data, "available", FALSE);
        return _ok_response(req_id, data, out_response);
    }
    json_object_set_boolean_member(data, "available", TRUE);
    json_object_set_int_member(data, "pxclk_total",
                               (gint64)param.pxclk_total);
    json_object_set_int_member(data, "screens_total",
                               (gint64)param.screens_total);
    json_object_set_int_member(data, "pxclk_in_screen",
                               (gint64)param.pxclk_in_screen);
    json_object_set_int_member(data, "category", param.category);
    json_object_set_int_member(data, "subtype", param.subtype);
    json_object_set_int_member(data, "pc", param.pc);
    json_object_set_int_member(data, "payload", (gint64)param.payload);
    return _ok_response(req_id, data, out_response);
}


#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
