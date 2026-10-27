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
#include "../debugger/png_encode.h"   /* device_screenshot (kp80 D4) */
#include "../debugger/bus_map.h"      /* get_bus_map, bus_peek (bus-model D6) */
/* mcp-trace-read: čtenář proudu záznamů (kurzor, stránka) */
#include "../debugger/trace/trace_reader.h"
#include "../audio_capture.h"
#include "../audio_analysis.h"
#include "../debugger/trace/ramlog_hook.h"
#include "../debugger/trace/bplog.h"
#include "../debugger/coverage/coverage.h"
#include "../debugger/coverage/coverage_export.h"
#include "../debugger/memsearch/memsearch.h"
#include "../debugger/run_control/run_log.h"
#include "../state_hash.h"
#include "../display.h"
#include <math.h>
/* CMTSPEED_CUSTOM pro cmt_tape_block_speed s vlastními pulzy */
#include "libs/cmtspeed/cmtspeed.h"
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

/* GDG header pro platform detection (= regDMD bit pro mode discriminator). */
#if MZARCH == 800
#include "../mzarch/mz800/gdg/mz800_gdg.h"
#elif MZARCH == 1500
#include "../mzarch/mz1500/gdg/mz1500_gdg.h"
#elif MZARCH == 700
#include "../mzarch/mz700/gdg/mz700_gdg.h"
#endif

/* mzarch_platform exportuje globální fields pro plný platform string,
 * MZARCH_NAME (= "mz700" / "mz800" / "mz1500"; PAL/NTSC nerozlišuje -
 * oba MZ-700 targety mají "mz700") a pixel clock. */
#include "../mzarch/mzarch_platform.h"

/* psg.h definuje PSG_DIVIDER (= 16 * GDGCLK2CPU_DIVIDER) - používané
 * v platform/info clocks sub-objektu pro reportování PSG vstupní
 * frekvence. Makro je definované unconditionally i pro platformy bez
 * PSG (= MZ-700, HAVE_PSG=0), tam ale report skipujeme. */
#include "../hw-generic/psg/psg.h"

/* en_SNAPSHOT_RESULT + snapshot_result_to_string() pro důvod chyby
 * snapshot_load / snapshot_load_buffer. Hlavička má jen standardní
 * include, takže jde i do testovacího buildu (funkci tam dodá stub). */
#include "../snapshot/snapshot.h"

/* PIO 8255 vkbd probe API (fix 0016 / cesta A) dispatch přímo nevolá:
 * sondu dosednutí ozbrojuje a vyhodnocuje emu vlákno v rámci sekvence
 * vstupu (hid_script.c, mcp-inbox-fixes), výsledek přijde v
 * st_DBGAPI_HID_SCRIPT_EVENT.landed. */

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
en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_ex(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                     en_DBGAPI_CMD cmd,
                                                     en_DBGAPI_CMD_ORIGIN origin,
                                                     void *data_ptr,
                                                     void *result_ptr,
                                                     int timeout_ms);
typedef void (*dbgapi_submit_stall_cb_t)(void *user_data);
en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_opts(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                       en_DBGAPI_CMD cmd,
                                                       en_DBGAPI_CMD_ORIGIN origin,
                                                       void *data_ptr,
                                                       void *result_ptr,
                                                       int timeout_ms,
                                                       int stall_ms,
                                                       dbgapi_submit_stall_cb_t stall_cb,
                                                       void *stall_user_data,
                                                       st_DBGAPI_SUBMIT_OPTS *opts);
bool dbgapi_cmd_mutates(en_DBGAPI_CMD cmd);
en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_watched(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                          en_DBGAPI_CMD cmd,
                                                          en_DBGAPI_CMD_ORIGIN origin,
                                                          void *data_ptr,
                                                          void *result_ptr,
                                                          int timeout_ms,
                                                          int stall_ms,
                                                          dbgapi_submit_stall_cb_t stall_cb,
                                                          void *stall_user_data);
#else
#include "../debugger/dbgapi_ui.h"
#include "../debugger/dbgapi_cmd_props.h"
/* V1.E.7 - blokující emu_run potřebuje sledovat inkrement framebuffer
 * counteru (wakeup čekání; HID nástroje od mcp-inbox-fixes čtou čítač
 * snímků emulace g_gdg.total_elapsed.screens). iface_video.h vystavuje
 * g_iface_video->fbsnapshot_screen_id + fbsnapshot_pixels_mutex/cond,
 * což je per-frame signalizace publikovaná emu vláknem v
 * iface_video_framebuffer_screen_done(). V test buildu (= bez emu
 * vlákna) je wait no-op. */
#include "../../iface/iface_video.h"
#include "../emulator.h"      /* EMULATOR_TEST_PAUSED, emulator_pause */
#endif

/* Záznam a přehrání vstupů (replay_*): v test buildu náhrada ve stubu. */
#include "../replay.h"
#include "../replay_ckpt.h"


/* ------------------------------------------------------------------ */
/* Konstanty a typy                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Timeout (ms) pro dbgapi sync call z MCP handleru.
 *
 * Omezuje jen čekání příkazu ve frontě na vyzvednutí emu vláknem (viz
 * dbgapi_ui_submit_cmd_sync_ex); rozpracovaný příkaz se dočká vždy.
 * Emu vlákno frontu obsluhuje jednou za snímek / za 20 ms v pauze, ale
 * měřením (experiment bp-remove-race, 2026-09) byly zjištěny občasné
 * prodlevy 0,3-1,5 s. Dřívější 1000 ms proto vedlo k falešným chybám;
 * 10 s dává rezervu a zároveň nezablokuje klienta natrvalo.
 */
#define MCP_DISPATCH_DBGAPI_TIMEOUT_MS 10000

/**
 * @brief Dodatečný limit (ms) pro dokončení příkazu, který emu vlákno už
 *        vyzvedlo.
 *
 * Rozpracovaný příkaz nelze zrušit (emu pracuje s daty na zásobníku
 * dispatchujícího vlákna), ale klient nesmí čekat bez konce. Když příkaz
 * do MCP_DISPATCH_DBGAPI_TIMEOUT_MS + tohoto limitu neskončí, běhový obal
 * (dispatch_runner.c) klientovi odpoví "Emulator busy: ... still running"
 * a dispatchující vlákno nechá čekat dál. Součet 20 s je pod 30s
 * timeoutem Python wrapperu (mcp_server.py, SEND_TIMEOUT_S), takže klient
 * dostane srozumitelnou chybu místo vlastního timeoutu. Příkazy se
 * souborovým I/O mají delší limit, viz MCP_DISPATCH_DBGAPI_STALL_LONG_MS.
 */
#define MCP_DISPATCH_DBGAPI_STALL_MS 10000

/**
 * @brief Dodatečný limit (ms) pro příkazy s legitimně dlouhým během
 *        v emu vlákně (souborové I/O, viz mcp_dispatch_stall_limit_ms()).
 *
 * Např. trace_save zapisuje až 2 GB, snapshot/CDL/profiler export a start
 * nebo stop video záznamu pracují se soubory. Tyto příkazy by se při
 * běžném limitu hlásily jako "still running", ačkoliv uspějí. 10 minut
 * dává rezervu; i zaseknutí takového příkazu se tak klientovi nakonec
 * ohlásí. Hodnota zvolená úvahou, ne měřením [neověřeno].
 */
#define MCP_DISPATCH_DBGAPI_STALL_LONG_MS 600000

/**
 * @brief Chybová zpráva pro příkaz, který emu vlákno převzalo, ale
 *        v limitu nedokončilo.
 *
 * Výsledek není znám (příkaz může doběhnout později), opakování proto
 * NENÍ bezpečné bez kontroly stavu.
 */
#define MCP_DISPATCH_MSG_STALLED \
    "Emulator busy: command still running in the emulator thread (no completion within the time limit); result unknown, check state before retrying"

/**
 * @brief Chybová zpráva pro příkaz zrušený dřív, než ho emu provedlo.
 *
 * Stabilní prefix "Emulator busy:" je dokumentovaný kontrakt pro klienty
 * (docs/agent): příkaz se NEprovedl a je bezpečné ho zopakovat.
 */
#define MCP_DISPATCH_MSG_NOT_EXECUTED \
    "Emulator busy: command not executed (timeout waiting for the emulator thread); safe to retry"

/**
 * @brief Chybová zpráva pro vícekrokový handler, jehož pozdější krok byl
 *        zrušen po provedení kroku dřívějšího.
 *
 * Stav emulátoru je změněný jen zčásti - opakování NENÍ bezpečné.
 */
#define MCP_DISPATCH_MSG_PARTIAL \
    "Emulator busy: command only partially executed (timeout waiting for the emulator thread); check state before retrying"

/** @brief Chybová zpráva pro plnou frontu příkazů (příkaz nezařazen). */
#define MCP_DISPATCH_MSG_QUEUE_FULL \
    "Emulator busy: command queue full, command not executed; safe to retry"

/** @brief Chybová zpráva při ukončování emulátoru (příkaz nezařazen). */
#define MCP_DISPATCH_MSG_ENDING \
    "Emulator is shutting down: command not executed"

/**
 * @brief Stav dbgapi submitů v rámci právě zpracovávaného MCP požadavku.
 *
 * Nuluje se na začátku `mcp_dispatch_request`, plní `_submit_dbgapi`,
 * čte `_err_response`, aby chybová odpověď pravdivě řekla, zda se příkaz
 * provedl. Thread-local, protože TCP server může dispatchovat z více
 * vláken současně.
 *
 * Členy:
 *  - last_status: výsledek posledního submitu (OK, pokud žádný nebyl),
 *  - executed:    aspoň jeden submit v tomto požadavku emu provedlo
 *                 (OK nebo FAILED),
 *  - have_rev, state_rev, run_seq, running: výstupy posledního provedeného
 *                 submitu z jednoho okamžiku (st_DBGAPI_SUBMIT_OPTS),
 *  - rev_failed / rev_skipped: podmínka revize stavu nesplněna / příkaz
 *                 přeskočen (if_state_rev_differs),
 *  - req_cond, req_value, req_cmd, req_pending: jednorázová podmínka
 *                 revize z požadavku; EXPECT_EQUAL použije první submit
 *                 příkazu, který mění stav (dbgapi_cmd_mutates(), handler
 *                 může před ním číst), SKIP_IF_EQUAL první submit příkazu
 *                 req_cmd,
 *  - add_state_rev: _ok_response() doplní do odpovědi `state_rev`.
 */
typedef struct {
    en_DBGAPI_SUBMIT_STATUS last_status;
    bool executed;
    bool have_rev;
    uint64_t state_rev;
    uint64_t run_seq;
    bool running;
    bool rev_failed;
    bool rev_skipped;
    uint8_t req_cond;
    uint64_t req_value;
    en_DBGAPI_CMD req_cmd;
    bool req_pending;
    bool add_state_rev;
} st_MCP_DISPATCH_SUBMIT_TRACK;

static _Thread_local st_MCP_DISPATCH_SUBMIT_TRACK s_submit_track;

/**
 * @brief Hlídání zaseknutí registrované pro aktuální vlákno (NULL = žádné).
 *
 * Viz mcp_dispatch_set_thread_stall_watch(). Thread-local, protože každý
 * požadavek běží na vlastním pracovním vlákně běhového obalu.
 */
static _Thread_local const st_MCP_DISPATCH_STALL_WATCH *s_stall_watch;

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
static en_MCP_DISPATCH_RESULT _handle_wait_stop(const st_JSONL_MESSAGE *req,
                                                char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_stop_history(const st_JSONL_MESSAGE *req,
                                                       char **out_response);
static en_MCP_DISPATCH_RESULT _handle_frame_hash(const st_JSONL_MESSAGE *req,
                                                 char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_record_start(const st_JSONL_MESSAGE *req,
                                                          char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_record_stop(const st_JSONL_MESSAGE *req,
                                                         char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_play(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_play_stop(const st_JSONL_MESSAGE *req,
                                                       char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_rewind(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_seek(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_status(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_replay_goto(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
/* machine-hash (M19) - obsahový otisk stroje po částech */
static en_MCP_DISPATCH_RESULT _handle_machine_hash(const st_JSONL_MESSAGE *req,
                                                   char **out_response);
/* dev-api (D1) - volitelná zařízení devbus */
static en_MCP_DISPATCH_RESULT _handle_get_devices(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
/* kp80 (D4) - snímek sekundárního výstupu zařízení devbus (druhý monitor) */
static en_MCP_DISPATCH_RESULT _handle_device_screenshot(const st_JSONL_MESSAGE *req,
                                                        char **out_response);
static en_MCP_DISPATCH_RESULT _handle_get_bus_map(const st_JSONL_MESSAGE *req,
                                                  char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bus_peek(const st_JSONL_MESSAGE *req,
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
/* mcp-trace-read - stránkované čtení trace fwd decls */
static en_MCP_DISPATCH_RESULT _handle_trace_status(const st_JSONL_MESSAGE *req,
                                                   char **out_response);
static en_MCP_DISPATCH_RESULT _handle_trace_read(const st_JSONL_MESSAGE *req,
                                                 char **out_response);
static en_MCP_DISPATCH_RESULT _handle_ramlog_start(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_ramlog_stop(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_ramlog_clear(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_ramlog_status(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_ramlog_who_wrote(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_ramlog_writers(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_bplog_clear(const st_JSONL_MESSAGE *req, char **out_response);

/* memsearch (M3 debug-gaps) - Freeze Bytes a Memory Search fwd decls */
static en_MCP_DISPATCH_RESULT _handle_freeze_add(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_freeze_remove(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_freeze_list(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_start(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_filter(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_sample(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_undo(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_redo(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_list(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_eliminate(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_memsearch_close(const st_JSONL_MESSAGE *req, char **out_response);
static void _ms_close_all(void);

/* video-capture Task 15 - video záznam fwd decls */
static en_MCP_DISPATCH_RESULT _handle_videorec_start(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
static en_MCP_DISPATCH_RESULT _handle_videorec_stop(const st_JSONL_MESSAGE *req,
                                                    char **out_response);
static en_MCP_DISPATCH_RESULT _handle_videorec_pause(const st_JSONL_MESSAGE *req,
                                                     char **out_response);
static en_MCP_DISPATCH_RESULT _handle_videorec_marker(const st_JSONL_MESSAGE *req,
                                                      char **out_response);
static en_MCP_DISPATCH_RESULT _handle_videorec_status(const st_JSONL_MESSAGE *req,
                                                      char **out_response);
static en_MCP_DISPATCH_RESULT _handle_videorec_timebase(const st_JSONL_MESSAGE *req,
                                                        char **out_response);
/* screenshot-series fwd decls */
static en_MCP_DISPATCH_RESULT _handle_screenshot_series_start(const st_JSONL_MESSAGE *req,
                                                              char **out_response);
static en_MCP_DISPATCH_RESULT _handle_screenshot_series_stop(const st_JSONL_MESSAGE *req,
                                                             char **out_response);
static en_MCP_DISPATCH_RESULT _handle_screenshot_series_status(const st_JSONL_MESSAGE *req,
                                                               char **out_response);
/* pc-stats - profiler podle PC fwd decls */
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_start(const st_JSONL_MESSAGE *req,
                                                        char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_stop(const st_JSONL_MESSAGE *req,
                                                       char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_reset(const st_JSONL_MESSAGE *req,
                                                        char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_get(const st_JSONL_MESSAGE *req,
                                                      char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_export(const st_JSONL_MESSAGE *req,
                                                         char **out_response);
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_bitmap(const st_JSONL_MESSAGE *req,
                                                         char **out_response);
/* audio-capture fwd decls */
static en_MCP_DISPATCH_RESULT _handle_audio_capture_start(const st_JSONL_MESSAGE *req,
                                                          char **out_response);
static en_MCP_DISPATCH_RESULT _handle_audio_capture_stop(const st_JSONL_MESSAGE *req,
                                                         char **out_response);
static en_MCP_DISPATCH_RESULT _handle_audio_capture_status(const st_JSONL_MESSAGE *req,
                                                           char **out_response);
static en_MCP_DISPATCH_RESULT _handle_audio_capture_summary(const st_JSONL_MESSAGE *req,
                                                            char **out_response);
static en_MCP_DISPATCH_RESULT _handle_audio_capture_pcm(const st_JSONL_MESSAGE *req,
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
static en_MCP_DISPATCH_RESULT _handle_media_run_mzf(const st_JSONL_MESSAGE *req,
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
static bool _screenshot_opts_from_json(JsonObject *obj, st_SCREENSHOT_OPTS *opts,
                                       char *err, size_t err_size);
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
/* observe-cas (M10) - konzistentní čtení registrů a oken paměti */
static en_MCP_DISPATCH_RESULT _handle_state_observe(
    const st_JSONL_MESSAGE *req, char **out_response);
/* coverage (M11) - pokrytí kódu */
static en_MCP_DISPATCH_RESULT _handle_coverage_start(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_coverage_stop(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_coverage_reset(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_coverage_status(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_coverage_mark(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_coverage_read(const st_JSONL_MESSAGE *req, char **out_response);
static en_MCP_DISPATCH_RESULT _handle_coverage_export(const st_JSONL_MESSAGE *req, char **out_response);
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
 * video-capture Task 15 přidal 5 Tools video záznamu (= videorec_start,
 * videorec_stop, videorec_pause, videorec_marker, videorec_status) na
 * KONEC tabulky.
 * 0017 FÁZE 1 přidal 4 Tracking lifecycle Tools (= trace_start,
 * trace_stop, trace_reset, trace_save) na KONEC tabulky.
 * video-capture Task 18 přidal videorec_timebase na KONEC tabulky - viz
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
    { "trace_status",            DBGAPI_CMD_TRACE_SNAPSHOT,           _handle_trace_status            },
    { "trace_read",              DBGAPI_CMD_TRACE_SNAPSHOT,           _handle_trace_read              },
    { "ramlog_start",            DBGAPI_CMD_RAMLOG,                   _handle_ramlog_start            },
    { "ramlog_stop",             DBGAPI_CMD_RAMLOG,                   _handle_ramlog_stop             },
    { "ramlog_clear",            DBGAPI_CMD_RAMLOG,                   _handle_ramlog_clear            },
    { "ramlog_status",           DBGAPI_CMD_RAMLOG,                   _handle_ramlog_status           },
    { "ramlog_who_wrote",        DBGAPI_CMD_RAMLOG,                   _handle_ramlog_who_wrote        },
    { "ramlog_writers",          DBGAPI_CMD_RAMLOG,                   _handle_ramlog_writers          },
    /* video-capture Task 15 - video záznam (všechny platformy). Přidáno na KONEC
     * tabulky kvůli stabilitě pozičního indexu v hello supported_commands. */
    { "videorec_start",          DBGAPI_CMD_VIDEOREC,                 _handle_videorec_start          },
    { "videorec_stop",           DBGAPI_CMD_VIDEOREC,                 _handle_videorec_stop           },
    { "videorec_pause",          DBGAPI_CMD_VIDEOREC,                 _handle_videorec_pause          },
    { "videorec_marker",         DBGAPI_CMD_VIDEOREC,                 _handle_videorec_marker         },
    { "videorec_status",         DBGAPI_CMD_VIDEOREC,                 _handle_videorec_status         },
    /* video-capture Task 18 - časová základna nahrávání. Přidáno na KONEC
     * tabulky kvůli stabilitě pozičního indexu v hello supported_commands. */
    { "videorec_timebase",       DBGAPI_CMD_VIDEOREC,                 _handle_videorec_timebase       },
    /* Spuštění MZF v definovaném stavu (reset + bootstrap jako --run-mzf).
     * Přidáno na KONEC tabulky kvůli stabilitě pozičního indexu v hello
     * supported_commands. */
    { "media_run_mzf",           DBGAPI_CMD_MEDIA_RUN_MZF,            _handle_media_run_mzf           },
    /* Série screenshotů (každý změněný emulovaný snímek jako PNG). Přidáno na
     * KONEC tabulky kvůli stabilitě pozičního indexu v hello supported_commands. */
    { "screenshot_series_start",  DBGAPI_CMD_SCREENSHOT_SERIES,       _handle_screenshot_series_start  },
    { "screenshot_series_stop",   DBGAPI_CMD_SCREENSHOT_SERIES,       _handle_screenshot_series_stop   },
    { "screenshot_series_status", DBGAPI_CMD_SCREENSHOT_SERIES,       _handle_screenshot_series_status },
    /* Profiler podle PC (počet, T-stavy a WAIT pro každou adresu instrukce).
     * Přidáno na KONEC tabulky kvůli stabilitě pozičního indexu v hello
     * supported_commands. */
    { "profiler_pc_start",        DBGAPI_CMD_PROFILER_PC,             _handle_profiler_pc_start        },
    { "profiler_pc_stop",         DBGAPI_CMD_PROFILER_PC,             _handle_profiler_pc_stop         },
    { "profiler_pc_reset",        DBGAPI_CMD_PROFILER_PC,             _handle_profiler_pc_reset        },
    { "profiler_pc_get",          DBGAPI_CMD_PROFILER_PC,             _handle_profiler_pc_get          },
    { "profiler_pc_export",       DBGAPI_CMD_PROFILER_PC,             _handle_profiler_pc_export       },
    /* Zachycení zvuku do složky (události kanálů, zápisy PSG, reference.wav,
     * sdl.wav). Přidáno na KONEC tabulky kvůli stabilitě pozičního indexu
     * v hello supported_commands. */
    { "audio_capture_start",      DBGAPI_CMD_AUDIO_CAPTURE,           _handle_audio_capture_start      },
    { "audio_capture_stop",       DBGAPI_CMD_AUDIO_CAPTURE,           _handle_audio_capture_stop       },
    { "audio_capture_status",     DBGAPI_CMD_AUDIO_CAPTURE,           _handle_audio_capture_status     },
    { "audio_capture_summary",    DBGAPI_CMD_NONE,                    _handle_audio_capture_summary    },
    { "audio_capture_pcm",        DBGAPI_CMD_NONE,                    _handle_audio_capture_pcm        },
    /* Bitmapa vykonaných adres profileru podle PC (pokrytí kódu). Přidáno
     * na KONEC tabulky kvůli stabilitě pozičního indexu v hello
     * supported_commands. */
    { "profiler_pc_bitmap",       DBGAPI_CMD_PROFILER_PC,             _handle_profiler_pc_bitmap       },
    /* memsearch (M3 debug-gaps) - Freeze Bytes přes MCP a postupné hledání
     * proměnných. Přidáno na KONEC tabulky kvůli stabilitě pozičního indexu
     * v hello supported_commands. */
    { "freeze_add",               DBGAPI_CMD_FREEZE_ADD,              _handle_freeze_add               },
    { "freeze_remove",            DBGAPI_CMD_FREEZE_REMOVE,           _handle_freeze_remove            },
    { "freeze_list",              DBGAPI_CMD_FREEZE_LIST,             _handle_freeze_list              },
    { "memsearch_start",          DBGAPI_CMD_MEMSEARCH_SAMPLE,        _handle_memsearch_start          },
    { "memsearch_filter",         DBGAPI_CMD_MEMSEARCH_SAMPLE,        _handle_memsearch_filter         },
    { "memsearch_sample",         DBGAPI_CMD_MEMSEARCH_SAMPLE,        _handle_memsearch_sample         },
    { "memsearch_undo",           DBGAPI_CMD_NONE,                    _handle_memsearch_undo           },
    { "memsearch_redo",           DBGAPI_CMD_NONE,                    _handle_memsearch_redo           },
    { "memsearch_list",           DBGAPI_CMD_NONE,                    _handle_memsearch_list           },
    { "memsearch_eliminate",      DBGAPI_CMD_NONE,                    _handle_memsearch_eliminate      },
    { "memsearch_close",          DBGAPI_CMD_NONE,                    _handle_memsearch_close          },
    /* run-control (M5): čekání na zastavení a historie zastavení (lokální). */
    { "wait_stop",                DBGAPI_CMD_NONE,                    _handle_wait_stop                },
    { "get_stop_history",         DBGAPI_CMD_NONE,                    _handle_get_stop_history         },
    /* run-control (M5): SHA-256 obrazu a VRAM bez přenosu dat. */
    { "frame_hash",               DBGAPI_CMD_FRAME_HASH,              _handle_frame_hash               },
    /* replay-core (M6a): záznam a přehrání vstupů (fronta požadavků replay.h). */
    { "replay_record_start",      DBGAPI_CMD_NONE,                    _handle_replay_record_start      },
    { "replay_record_stop",       DBGAPI_CMD_NONE,                    _handle_replay_record_stop       },
    { "replay_play",              DBGAPI_CMD_NONE,                    _handle_replay_play              },
    { "replay_play_stop",         DBGAPI_CMD_NONE,                    _handle_replay_play_stop         },
    { "replay_status",            DBGAPI_CMD_NONE,                    _handle_replay_status            },
    { "replay_rewind",            DBGAPI_CMD_NONE,                    _handle_replay_rewind            },
    { "replay_seek",              DBGAPI_CMD_NONE,                    _handle_replay_seek              },
    /* observe-cas (M10 debug-gaps) - registry, příznaky a okna paměti
     * z jednoho okamžiku (jeden průchod fronty dbgapi). */
    { "state_observe",            DBGAPI_CMD_STATE_OBSERVE,           _handle_state_observe            },
    /* coverage (M11 debug-gaps) - pokrytí kódu (instrukce, větve, verze
     * kódu). Přidáno na KONEC tabulky kvůli stabilitě pozičního indexu
     * v hello supported_commands. */
    { "coverage_start",           DBGAPI_CMD_COVERAGE,                _handle_coverage_start           },
    { "coverage_stop",            DBGAPI_CMD_COVERAGE,                _handle_coverage_stop            },
    { "coverage_reset",           DBGAPI_CMD_COVERAGE,                _handle_coverage_reset           },
    { "coverage_status",          DBGAPI_CMD_COVERAGE,                _handle_coverage_status          },
    { "coverage_mark",            DBGAPI_CMD_COVERAGE,                _handle_coverage_mark            },
    { "coverage_read",            DBGAPI_CMD_COVERAGE,                _handle_coverage_read            },
    { "coverage_export",          DBGAPI_CMD_COVERAGE,                _handle_coverage_export          },
    /* bp-log-mcp (M15 debug-gaps) - vyčištění ringu výstupu akcí BP (kanál
     * bplog čte trace_read). Přidáno na KONEC tabulky kvůli stabilitě
     * pozičního indexu v hello supported_commands. */
    { "bplog_clear",              DBGAPI_CMD_BPLOG,                   _handle_bplog_clear              },
    /* replay-goto (M17 debug-gaps): stav stroje v čase záznamu vstupů přes
     * přehrání (seek + zastavení na čase v emu vlákně). */
    { "replay_goto",              DBGAPI_CMD_NONE,                    _handle_replay_goto              },
    /* machine-hash (M19 debug-gaps): obsahový otisk stroje po částech
     * (test úplnosti revize stavu, "je stav stejný jako tehdy?"). */
    { "machine_hash",             DBGAPI_CMD_MACHINE_HASH,            _handle_machine_hash             },
    /* dev-api (D1 mz700-devices): volitelná zařízení devbus (id, volby,
     * porty, MMIO, regiony, výstupy). Na KONEC tabulky. */
    { "get_devices",              DBGAPI_CMD_DEVBUS_LIST,             _handle_get_devices              },
    /* kp80 (D4 mz700-devices): obraz sekundárního výstupu zařízení devbus
     * (druhý monitor karty K&P 80) jako PNG nebo indexy palety. */
    { "device_screenshot",        DBGAPI_CMD_DEVBUS_VOUT_READ,        _handle_device_screenshot        },
    /* bus-model (D6 mz700-devices): mapa vlastníků adres sběrnice (statické
     * čipy + devbus, budiče, příznaky, aktivita) a peek s rezolucí sběrnice. */
    { "get_bus_map",              DBGAPI_CMD_BUS_MAP,                 _handle_get_bus_map              },
    { "bus_peek",                 DBGAPI_CMD_BUS_PEEK,                _handle_bus_peek                 },
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
 * @brief Načte z JsonObject pole "pulses_us" - délky pulzů kazety v µs.
 *
 * Pořadí odpovídá hlavičce CMTSPEED zařízení UniCMT: LONG high, LONG low,
 * SHORT high, SHORT low. Rozsah hodnot kontroluje až emulátor
 * (mztape_pulses_set_us).
 *
 * @param obj Objekt s polem (smí být NULL).
 * @param[out] out Načtené délky; při neúspěchu se nemění.
 * @return TRUE, pokud pole existuje a obsahuje právě 4 čísla (int nebo
 *         double); jinak FALSE.
 */
static gboolean _obj_pulses_us(JsonObject *obj, double out[4]) {
    if (!obj || !json_object_has_member(obj, "pulses_us")) return FALSE;
    JsonNode *node = json_object_get_member(obj, "pulses_us");
    if (!node || json_node_get_node_type(node) != JSON_NODE_ARRAY) return FALSE;
    JsonArray *arr = json_node_get_array(node);
    if (json_array_get_length(arr) != 4) return FALSE;
    double tmp[4];
    for (guint i = 0; i < 4; i++) {
        JsonNode *el = json_array_get_element(arr, i);
        if (!el || json_node_get_node_type(el) != JSON_NODE_VALUE) return FALSE;
        GType t = json_node_get_value_type(el);
        if (t == G_TYPE_INT64) {
            tmp[i] = (double)json_node_get_int(el);
        } else if (t == G_TYPE_DOUBLE) {
            tmp[i] = json_node_get_double(el);
        } else {
            return FALSE;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
    return TRUE;
}


/**
 * @brief Sestaví JSON pole 4 délek pulzů v µs (pořadí jako _obj_pulses_us).
 *
 * @param us Délky v µs.
 * @return Nové pole (vlastnictví přebírá objekt, do kterého se vloží).
 */
static JsonArray *_pulses_us_array(const double us[4]) {
    JsonArray *arr = json_array_new();
    for (int i = 0; i < 4; i++) {
        json_array_add_double_element(arr, us[i]);
    }
    return arr;
}


/**
 * @brief Stabilní prefix chyby nesplněné podmínky zápisu (observe-cas M10).
 *
 * Podmíněný zápis (`expected_sha256`, `expected`) se v emu vlákně neprovedl,
 * protože stav se od čtení klienta změnil; viz docs/agent/error_handling.md.
 */
#define MCP_PRECONDITION_PREFIX "Precondition failed: "


/**
 * @brief Helper - sestaví error RESPONSE řádek s daným textem.
 *
 * Wrap kolem `jsonl_build_response(.., success=false, NULL, msg)`.
 * Vrací MCP_DISPATCH_OK pouze pokud se podařilo string sestavit.
 *
 * Volání: `return _err_response(req_id, "Invalid parameters",
 *         MCP_DISPATCH_INVALID_PARAMS, out_response);`
 *
 * U `MCP_DISPATCH_EMU_ERROR` po submitu, který emu neprovedlo (timeout ve
 * frontě, plná fronta, ukončování - viz `s_submit_track`), předřadí
 * zprávě handleru pravdivý popis (MCP_DISPATCH_MSG_*) ve tvaru
 * "<popis> [<původní zpráva>]". Původní text zůstává kvůli zpětné
 * kompatibilitě klientů, kteří testují podřetězec (např. "Emulator
 * unavailable" při startu). Chyby handleru (DBGAPI_SUBMIT_FAILED)
 * a validační chyby zůstávají beze změny.
 */
static en_MCP_DISPATCH_RESULT _err_response(int64_t req_id,
                                            const char *msg,
                                            en_MCP_DISPATCH_RESULT rc,
                                            char **out_response) {
    const char *reason = NULL;
    if (rc == MCP_DISPATCH_EMU_ERROR) {
        switch (s_submit_track.last_status) {
            case DBGAPI_SUBMIT_TIMEOUT:
                reason = s_submit_track.executed ? MCP_DISPATCH_MSG_PARTIAL
                                                 : MCP_DISPATCH_MSG_NOT_EXECUTED;
                break;
            case DBGAPI_SUBMIT_QUEUE_FULL:
                reason = s_submit_track.executed ? MCP_DISPATCH_MSG_PARTIAL
                                                 : MCP_DISPATCH_MSG_QUEUE_FULL;
                break;
            case DBGAPI_SUBMIT_ENDING:
                reason = MCP_DISPATCH_MSG_ENDING;
                break;
            default:
                break;
        }
    }
    /* Příkaz odmítla brána záznamu vstupů (replay.h): přidat proč. */
    char refusal[REPLAY_STATUS_TEXT_MAX] = "";
    if (rc == MCP_DISPATCH_EMU_ERROR && !reason && replay_is_active()) {
        replay_take_refusal(refusal, sizeof(refusal));
        if (refusal[0]) reason = refusal;
    }
    char *combined = reason ? g_strdup_printf("%s [%s]", reason, msg) : NULL;
    char *line = jsonl_build_response(req_id, false, NULL,
                                      combined ? combined : msg);
    g_free(combined);
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
 * U příkazů, které mění stav stroje (s_submit_track.add_state_rev), doplní
 * `state_rev` po posledním provedeném submitu, pokud ho data nemají
 * (při `data_obj` NULL založí nový objekt).
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
    /* Revize stavu po příkazu u vybraných nástrojů (zápisy, podmínka). */
    if (s_submit_track.add_state_rev && s_submit_track.have_rev) {
        if (!data_obj) data_obj = json_object_new();
        if (!json_object_has_member(data_obj, "state_rev")) {
            json_object_set_int_member(data_obj, "state_rev", (gint64)s_submit_track.state_rev);
        }
    }
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
 * @brief Volá dbgapi submit s origin=MCP a MCP timeoutem.
 *
 * Zkratka pro `dbgapi_ui_submit_cmd_sync_opts(&g_dbgapi_cmdrq_queue,
 *   cmd, DBGAPI_CMD_ORIGIN_MCP, data, result, MCP_DISPATCH_DBGAPI_TIMEOUT_MS,
 *   mcp_dispatch_stall_limit_ms(cmd), ..., &opts)`. Volby revize stavu:
 * čekající jednorázovou podmínku požadavku (s_submit_track.req_*) předá
 * prvnímu vhodnému submitu, výstupy (`state_rev`, `run_seq`, `running`,
 * `rev_failed`, `rev_skipped`) zapíše do s_submit_track. Je-li pro vlákno registrované
 * hlídání zaseknutí (s_stall_watch), předá jeho `on_stall`; je-li
 * požadavek už opuštěný, příkaz neodešle a vrátí ho jako neprovedený
 * (DBGAPI_SUBMIT_TIMEOUT). Bez registrace se zaseknutí nehlásí a
 * rozpracovaný příkaz se čeká bez limitu (chování přímého volání).
 *
 * Side effect: zaznamená výsledek do `s_submit_track` (per vlákno), podle
 * kterého `_err_response` u MCP_DISPATCH_EMU_ERROR zvolí pravdivou zprávu
 * (neprovedeno / částečně provedeno).
 *
 * @return true při úspěchu (DBGAPI_SUBMIT_OK), false jinak (timeout =
 *         neprovedeno / queue full / emu ending / handler vrátil
 *         success=false)
 */
static bool _submit_dbgapi(en_DBGAPI_CMD cmd, void *data, void *result) {
    const st_MCP_DISPATCH_STALL_WATCH *w = s_stall_watch;
    en_DBGAPI_SUBMIT_STATUS st;
    if (w && w->is_abandoned && w->is_abandoned(w->user_data)) {
        /* Klient už dostal odpověď "busy" (předchozí krok se zasekl).
         * Další kroky vícekrokového handleru neodesíláme, aby se po
         * odpovědi neobjevily opožděné vedlejší efekty. */
        st = DBGAPI_SUBMIT_TIMEOUT;
    } else {
        /* Volby revize stavu: jednorázová podmínka z požadavku (viz
         * st_MCP_DISPATCH_SUBMIT_TRACK), výstupy revize a běhu vždy. */
        st_DBGAPI_SUBMIT_OPTS opts;
        memset(&opts, 0, sizeof(opts));
        en_DBGAPI_CMD masked = (en_DBGAPI_CMD)(cmd & DBGAPI_CMD_MASK);
        if (s_submit_track.req_pending
            && (masked == s_submit_track.req_cmd
                || (s_submit_track.req_cond == DBGAPI_REV_COND_EXPECT_EQUAL
                    && dbgapi_cmd_mutates(masked)))) {
            opts.rev_cond = s_submit_track.req_cond;
            opts.rev_value = s_submit_track.req_value;
            s_submit_track.req_pending = false;
        }
        st = dbgapi_ui_submit_cmd_sync_opts(&g_dbgapi_cmdrq_queue,
                                            cmd,
                                            DBGAPI_CMD_ORIGIN_MCP,
                                            data,
                                            result,
                                            MCP_DISPATCH_DBGAPI_TIMEOUT_MS,
                                            mcp_dispatch_stall_limit_ms((int)cmd),
                                            (w && w->on_stall) ? w->on_stall : NULL,
                                            w ? w->user_data : NULL,
                                            &opts);
        if (st == DBGAPI_SUBMIT_OK || st == DBGAPI_SUBMIT_FAILED) {
            s_submit_track.have_rev = true;
            s_submit_track.state_rev = opts.state_rev_after;
            s_submit_track.run_seq = opts.run_seq_after;
            s_submit_track.running = opts.running_after;
            if (opts.rev_failed) s_submit_track.rev_failed = true;
            if (opts.rev_skipped) s_submit_track.rev_skipped = true;
        }
    }
    s_submit_track.last_status = st;
    if (st == DBGAPI_SUBMIT_OK || st == DBGAPI_SUBMIT_FAILED)
        s_submit_track.executed = true;
    return st == DBGAPI_SUBMIT_OK;
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
 * polem `paused` (= `!is_running`), `run_seq` (číslo běžícího, v pauze
 * posledního běhu, run_log.h) a `state_rev` (revize stavu, state_rev.h);
 * `running`, `run_seq` a `state_rev` jsou z jednoho okamžiku (výstupy
 * slotu IS_RUNNING, N69). Frame counter / cycles by mělo přidat V0.B
 * (vyžaduje GET_RASTER_POS nebo equivalentní dbgapi call).
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
    /* run-control: číslo běžícího (v pauze posledního) běhu, viz wait_stop.
     * Z téhož okamžiku jako `running` (výstup slotu IS_RUNNING, N69), spolu
     * s revizí stavu stroje (state_rev.h). */
    json_object_set_int_member(data, "run_seq",
                               (gint64)(s_submit_track.have_rev ? s_submit_track.run_seq
                                                                : run_log_current(NULL)));
    if (s_submit_track.have_rev) {
        json_object_set_int_member(data, "state_rev", (gint64)s_submit_track.state_rev);
    }

    /* V1.D.1 - last_user_action field. Pokud žádná USER akce nebyla,
     * vystavíme JSON null (= AI vidí "user nic neudělal od startu emu"). */
    char     last_kind[DBGAPI_USER_ACTION_KIND_MAX];
    uint64_t last_ts  = 0;
    bool     have_lua = dbgapi_get_last_user_action_kind(last_kind, sizeof(last_kind), &last_ts);
    if (have_lua) {
        JsonObject *lua = json_object_new();
        json_object_set_string_member(lua, "kind", last_kind);
        json_object_set_int_member(lua, "timestamp_us", (gint64)last_ts);
        json_object_set_object_member(data, "last_user_action", lua);
    } else {
        json_object_set_null_member(data, "last_user_action");
    }
    return _ok_response(req_id, data, out_response);
}


/* ------------------------------------------------------------------ */
/* run-control (M5): záznamy zastavení, deadline, čekání na zastavení  */
/* ------------------------------------------------------------------ */

/** @brief Nejdelší čekání `wait_stop` [ms] (pod 30 s limitem wrapperu MCP). */
#define MCP_WAIT_STOP_MAX_MS RUN_LOG_WAIT_MAX_MS

/** @brief Výchozí čekání `wait_stop` [ms]. */
#define MCP_WAIT_STOP_DEFAULT_MS 5000

/**
 * @brief Strop doby krokovacích smyček `run_until_raster`, `run_until_tstate`
 *        a `run_until_event` [ms] (N3).
 *
 * Smyčky běží v dispatch vlákně a nejdou zrušit; wrapper MCP čeká na
 * odpověď nejvýš 30 s. Po vypršení smyčka skončí s `time_limited: true`
 * a emulace stojí v pauze tam, kam došla.
 */
#define MCP_STEP_LOOP_TIME_LIMIT_MS 20000


/**
 * @brief Převede záznam zastavení na JSON objekt (MCP wait_stop, historie).
 *
 * @param rec Záznam (run_log.h).
 * @return Nový JsonObject (vlastník volající).
 */
static JsonObject *_stop_record_to_json(const st_RUN_LOG_RECORD *rec) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "run_seq", (gint64)rec->run_seq);
    json_object_set_string_member(o, "reason", rec->reason);
    json_object_set_int_member(o, "pc", rec->pc);
    json_object_set_boolean_member(o, "step", rec->step ? TRUE : FALSE);
    json_object_set_string_member(o, "started_by", rec->started_by);
    json_object_set_string_member(o, "start_cmd", rec->start_cmd);
    json_object_set_string_member(o, "stopped_by", rec->stopped_by);
    json_object_set_int_member(o, "start_ticks", (gint64)rec->start_ticks);
    json_object_set_int_member(o, "stop_ticks", (gint64)rec->stop_ticks);
    json_object_set_int_member(o, "elapsed_ticks", (gint64)rec->elapsed_ticks);
    json_object_set_int_member(o, "frame", (gint64)rec->stop_frame);
    if (rec->bp_id >= 0) json_object_set_int_member(o, "bp_id", rec->bp_id);
    if (strcmp(rec->reason, "exit") == 0) {
        json_object_set_int_member(o, "exit_code", rec->exit_code);
        json_object_set_string_member(o, "exit_reason", rec->exit_reason);
        if (rec->detail[0]) json_object_set_string_member(o, "message", rec->detail);
    } else if (strcmp(rec->reason, "bp_action_saturation") == 0) {
        json_object_set_int_member(o, "bytes", (gint64)rec->bytes);
        json_object_set_string_member(o, "detail", rec->detail);
    }
    return o;
}


/**
 * @brief Načte volitelný parametr `deadline` (objekt s právě jedním klíčem).
 *
 * Tvary: `{"frames": N}` (1..1000000), `{"emulated_ms": N}` (1..86400000),
 * `{"ticks": N}` (takty GDG, >= 1). Hodnoty druhu odpovídají
 * en_RUN_DEADLINE_KIND (run_control.h): 0 frames, 1 emulated_ms, 2 ticks.
 *
 * @param obj Objekt `data` požadavku (smí být NULL).
 * @param[out] out Parametr pro DBGAPI_CMD_RUN_DEADLINE (arm = true).
 * @param[out] present true = parametr je v požadavku.
 * @param[out] err Chybová hláška (anglicky) při návratu false.
 * @param err_size Velikost @p err.
 * @return false = neplatný tvar nebo hodnota.
 */
static bool _parse_deadline(JsonObject *obj, st_DBGAPI_RUN_DEADLINE_PARAM *out,
                            bool *present, char *err, size_t err_size) {
    memset(out, 0, sizeof(*out));
    *present = false;
    if (!obj || !json_object_has_member(obj, "deadline")) return true;
    JsonNode *n = json_object_get_member(obj, "deadline");
    if (!n || json_node_get_node_type(n) == JSON_NODE_NULL) return true;
    if (json_node_get_node_type(n) != JSON_NODE_OBJECT) {
        g_snprintf(err, err_size, "deadline must be an object: {frames|emulated_ms|ticks: N}");
        return false;
    }
    JsonObject *d = json_node_get_object(n);
    static const struct { const char *key; int kind; gint64 max; } KINDS[] = {
        { "frames",      0, 1000000LL },
        { "emulated_ms", 1, 86400000LL },
        { "ticks",       2, G_MAXINT64 / 4 },
    };
    int found = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(KINDS); i++) {
        if (!json_object_has_member(d, KINDS[i].key)) continue;
        found++;
        gint64 v = _obj_int_or(d, KINDS[i].key, -1);
        if (v < 1 || v > KINDS[i].max) {
            g_snprintf(err, err_size, "deadline.%s must be in range 1..%" G_GINT64_FORMAT,
                       KINDS[i].key, KINDS[i].max);
            return false;
        }
        out->kind = KINDS[i].kind;
        out->value = (uint64_t)v;
    }
    if (found != 1 || json_object_get_size(d) != 1) {
        g_snprintf(err, err_size, "deadline must have exactly one of: frames, emulated_ms, ticks");
        return false;
    }
    out->arm = true;
    *present = true;
    return true;
}


/**
 * @brief Nastaví deadline v emulátoru (DBGAPI_CMD_RUN_DEADLINE).
 *
 * @param p Parametr z _parse_deadline() (na výstupu `ticks`).
 * @return false = emulátor deadline nepřijal nebo příkaz neproběhl.
 */
static bool _arm_deadline(st_DBGAPI_RUN_DEADLINE_PARAM *p) {
    return _submit_dbgapi(DBGAPI_CMD_RUN_DEADLINE, p, NULL);
}


/** @brief Zruší deadline (příkaz, pro který byl nastavený, selhal). */
static void _clear_deadline(void) {
    st_DBGAPI_RUN_DEADLINE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.arm = false;
    (void)_submit_dbgapi(DBGAPI_CMD_RUN_DEADLINE, &p, NULL);
}


/**
 * @brief Doplní do odpovědi rozběhu délku deadlinu.
 *
 * @param resp Odpověď.
 * @param present Deadline byl v požadavku.
 * @param p Parametr po nastavení (`ticks` = délka v taktech GDG od nastavení).
 */
static void _deadline_to_json(JsonObject *resp, bool present,
                              const st_DBGAPI_RUN_DEADLINE_PARAM *p) {
    if (!present) return;
    json_object_set_int_member(resp, "deadline_ticks", (gint64)p->ticks);
}


/**
 * @brief `wait_stop` handler - počká, až skončí běh (nejvýš 25 s).
 *
 * Parametry v `data` (volitelné):
 *  - `run_seq` (int) - číslo běhu; bez něj aktuální (běžící) nebo poslední běh
 *  - `timeout_ms` (int, 0..25000, default 5000)
 *
 * Response:
 *  - `done` (bool) - běh skončil
 *  - `run_seq` (int) - číslo běhu, na který se čekalo
 *  - `current_run_seq` (int), `running` (bool) - stav v okamžiku odpovědi
 *  - `record` (objekt) - záznam zastavení (jen `done` a záznam v historii)
 *  - `history_lost` (bool) - `done`, ale záznam už historie přepsala
 *  - `shutdown` (bool) - emulátor končí, čekání přerušeno
 *
 * Lokální handler: čeká na dispatch vlákně (run_log_wait), frontu dbgapi
 * neblokuje.
 */
static en_MCP_DISPATCH_RESULT _handle_wait_stop(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                      ? json_node_get_object(data_node) : NULL;
    gint64 timeout_ms = obj ? _obj_int_or(obj, "timeout_ms", MCP_WAIT_STOP_DEFAULT_MS)
                            : MCP_WAIT_STOP_DEFAULT_MS;
    if (timeout_ms < 0 || timeout_ms > MCP_WAIT_STOP_MAX_MS) {
        return _err_response(req_id, "timeout_ms must be in range 0..25000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint64_t seq;
    if (obj && json_object_has_member(obj, "run_seq")) {
        gint64 v = _obj_int_or(obj, "run_seq", -1);
        if (v < 0) {
            return _err_response(req_id, "run_seq must be >= 0",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        seq = (uint64_t)v;
    } else {
        seq = run_log_current(NULL);
    }

    st_RUN_LOG_RECORD rec;
    bool found = false;
    en_RUN_LOG_WAIT w = run_log_wait(seq, (int)timeout_ms, &rec, &found);
    bool running = false;
    uint64_t cur = run_log_current(&running);

    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "done", (w == RUN_LOG_WAIT_DONE) ? TRUE : FALSE);
    json_object_set_int_member(resp, "run_seq", (gint64)seq);
    json_object_set_int_member(resp, "current_run_seq", (gint64)cur);
    json_object_set_boolean_member(resp, "running", running ? TRUE : FALSE);
    if (w == RUN_LOG_WAIT_DONE) {
        if (found) {
            json_object_set_object_member(resp, "record", _stop_record_to_json(&rec));
        } else {
            json_object_set_boolean_member(resp, "history_lost", TRUE);
        }
    }
    if (w == RUN_LOG_WAIT_SHUTDOWN) {
        json_object_set_boolean_member(resp, "shutdown", TRUE);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_stop_history` handler - posledních N záznamů zastavení.
 *
 * Parametr `limit` (int, 1..RUN_LOG_CAPACITY, default RUN_LOG_CAPACITY).
 * Response: `records` (od nejstaršího), `count`, `current_run_seq`,
 * `running`. Backing pro resource `emulator://run/history`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_stop_history(const st_JSONL_MESSAGE *req,
                                                       char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                      ? json_node_get_object(data_node) : NULL;
    gint64 limit = obj ? _obj_int_or(obj, "limit", RUN_LOG_CAPACITY) : RUN_LOG_CAPACITY;
    if (limit < 1 || limit > RUN_LOG_CAPACITY) {
        return _err_response(req_id, "limit must be in range 1..32",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_RUN_LOG_RECORD recs[RUN_LOG_CAPACITY];
    unsigned n = run_log_history(recs, (unsigned)limit);
    bool running = false;
    uint64_t cur = run_log_current(&running);
    JsonObject *resp = json_object_new();
    JsonArray *arr = json_array_sized_new(n);
    for (unsigned i = 0; i < n; i++) {
        json_array_add_object_element(arr, _stop_record_to_json(&recs[i]));
    }
    json_object_set_array_member(resp, "records", arr);
    json_object_set_int_member(resp, "count", (gint64)n);
    json_object_set_int_member(resp, "current_run_seq", (gint64)cur);
    json_object_set_boolean_member(resp, "running", running ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `pause` handler - pozastaví emulaci.
 *
 * Proxy na `DBGAPI_CMD_PAUSE` se st_DBGAPI_PAUSE_PARAM. Volitelný parametr
 * `if_run_seq` (int): pozastaví jen běžící běh s tímto číslem, takže agent
 * nezastaví běh, který mezitím spustil člověk v GUI.
 *
 * Response:
 *  - `paused` (bool) - emulace je po příkazu v pauze (bez `if_run_seq`
 *    vždy true)
 *  - `was_running` (bool), `paused_now` (bool) - běžela / příkaz ji zastavil
 *  - `run_seq` (int) - číslo běhu běžícího před příkazem (nebo posledního)
 *  - `skipped` (string, jen s `if_run_seq` bez zastavení) - "not_running"
 *    nebo "run_seq_mismatch"
 */
static en_MCP_DISPATCH_RESULT _handle_pause(const st_JSONL_MESSAGE *req,
                                            char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                      ? json_node_get_object(data_node) : NULL;
    st_DBGAPI_PAUSE_PARAM pp;
    memset(&pp, 0, sizeof(pp));
    if (obj && json_object_has_member(obj, "if_run_seq")) {
        gint64 v = _obj_int_or(obj, "if_run_seq", -1);
        if (v < 0) {
            return _err_response(req_id, "if_run_seq must be >= 0",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        pp.has_if_run_seq = true;
        pp.if_run_seq = (uint64_t)v;
    }
    if (!_submit_dbgapi(DBGAPI_CMD_PAUSE, &pp, NULL)) {
        return _err_response(req_id, "Pause failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    bool paused_after = !pp.was_running || pp.paused_now;
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "paused", paused_after ? TRUE : FALSE);
    json_object_set_boolean_member(data, "was_running", pp.was_running ? TRUE : FALSE);
    json_object_set_boolean_member(data, "paused_now", pp.paused_now ? TRUE : FALSE);
    json_object_set_int_member(data, "run_seq", (gint64)pp.run_seq);
    if (pp.has_if_run_seq && !pp.paused_now) {
        json_object_set_string_member(data, "skipped",
                                      pp.was_running ? "run_seq_mismatch" : "not_running");
    }
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `run` handler - spustí emulaci.
 *
 * Dvě cesty podle parametru `frames`:
 *  - **Async (legacy)** - frames chybí nebo == 0: jen submit
 *    `DBGAPI_CMD_RUN` (= unpause), response `{"running": true, "run_seq",
 *    "already_running"}`. Emulátor běží asynchronně dál. Volitelný
 *    `deadline` ({frames|emulated_ms|ticks: N}) běh zastaví v emulovaném
 *    čase (reason "deadline", odpověď navíc `deadline_ticks`); na konec
 *    běhu jde čekat přes `wait_stop`.
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
    JsonObject *obj = NULL;
    if (data_node
        && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        obj = json_node_get_object(data_node);
        frames = _obj_int_or(obj, "frames", 0);
    }

    /* Validace rozsahu. Horní limit 1000 (= match mcp_server.py Tool
     * argument validation). frames == 0 explicit = async path. */
    if (frames < 0 || frames > 1000) {
        return _err_response(req_id, "frames must be in range 0..1000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Deadline v emulovaném čase (run-control) jen u asynchronního běhu;
     * frames je sám deterministický limit. */
    st_DBGAPI_RUN_DEADLINE_PARAM dl;
    bool has_deadline = false;
    char dl_err[128];
    if (!_parse_deadline(obj, &dl, &has_deadline, dl_err, sizeof(dl_err))) {
        return _err_response(req_id, dl_err, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (has_deadline && frames > 0) {
        return _err_response(req_id, "deadline cannot be combined with frames",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    if (frames == 0) {
        /* Async path (legacy) - jen unpause, response `running: true`.
         * Side effect: emulator_pause(false) + audio resume + UI update.
         * S deadline se emulace sama zastaví (reason "deadline"); deadline
         * platí do nejbližší pauzy. */
        if (has_deadline && !_arm_deadline(&dl)) {
            return _err_response(req_id, "Invalid deadline",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        st_DBGAPI_RUN_START_RESULT rr;
        memset(&rr, 0, sizeof(rr));
        if (!_submit_dbgapi(DBGAPI_CMD_RUN, NULL, &rr)) {
            if (has_deadline) _clear_deadline();
            return _err_response(req_id, "Run failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        JsonObject *data = json_object_new();
        json_object_set_boolean_member(data, "running", TRUE);
        json_object_set_int_member(data, "run_seq", (gint64)rr.run_seq);
        json_object_set_boolean_member(data, "already_running",
                                       (rr.status == DBGAPI_RUN_START_NOT_PAUSED) ? TRUE : FALSE);
        _deadline_to_json(data, has_deadline, &dl);
        return _ok_response(req_id, data, out_response);
    }

    /* Blokující deterministická path. data_ptr je int* = počet framů.
     * Lokální proměnná je platná po celou dobu _submit_dbgapi (= synchronní
     * blokující call, počká na zpracování emu vláknem - viz
     * dbgapi_ui_submit_cmd_sync_with_origin). RUN_FRAMES handler nastaví
     * cílový screens counter a unpausne; emu se sám deterministicky pausne. */
    int frames_int = (int)frames;
    /* Výchozí screens counter dodá RUN_FRAMES handler přes result_ptr: je to
     * přesně hodnota, od které emu vlákno počítá cíl (run_frames_target =
     * screens + N). actual_frames měříme jako delta TÉHOŽ counteru - NE podle
     * fbsnapshot_screen_id, který počítá jen skutečně vykreslené framebuffery
     * (řidší, závislé na video módu).
     *
     * Čtení zde na dispatch vlákně by nestačilo: za běhu emulace se drain
     * dbgapi provádí až po uzavření snímku (za inkrementem screens), takže
     * mezi čtením a zpracováním příkazu proběhne ještě jeden snímek a delta
     * by vyšla N+1. Přímé čtení zůstává jen jako výchozí hodnota pro případ,
     * že handler result_ptr nevyplní (testovací stub dbgapi). */
    uint32_t start_screens = (uint32_t)g_gdg.total_elapsed.screens;
#ifndef MZ800EMU_MCP_TEST_BUILD
    /* Vynuluj důvod pauzy - po doběhnutí ho přečteme do stopped_by. Emu je
     * zde paused (stojí), takže zápis g_emulator.pause_reason je bezpečný.
     * (V testovacím buildu g_emulator/emulator.h není dostupné.) */
    g_emulator.pause_reason = EMU_PAUSE_REASON_NONE;
#endif
    if (!_submit_dbgapi(DBGAPI_CMD_RUN_FRAMES, &frames_int, &start_screens)) {
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

    /* actual_frames = delta screens counteru od výchozí hodnoty z handleru,
     * čteno po pauze (emu stabilní -> přesné). Při deterministickém doběhu je
     * to přesně N (emu pausnul při screens == start_screens + N) bez ohledu
     * na to, zda run začal z pauzy nebo za běhu; při safety timeoutu skutečný
     * počet proběhlých screens. */
    int actual = (int)((uint32_t)g_gdg.total_elapsed.screens - start_screens);

    /* stopped_by: proč se emulace zastavila. !done = safety timeout; jinak
     * důvod, který nastavil ten, kdo emu pausnul (frame target / breakpoint /
     * dočasný breakpoint / manuální pauza / žádost o konec "exit"). "unknown" = emu se zastavil z jiného důvodu, který
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
            case EMU_PAUSE_REASON_RUN_TO:     stopped_by = "run_to";     break;
            case EMU_PAUSE_REASON_EXIT:       stopped_by = "exit";       break;
            /* Byte backstop akcí BP (jméno jako run_control_reason_name()). */
            case EMU_PAUSE_REASON_BP_SATURATION:
                                              stopped_by = "bp_action_saturation"; break;
            case EMU_PAUSE_REASON_RUN_UNTIL_TICKS:
                                              stopped_by = "run_until_ticks"; break;
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
    /* Číslo běhu, který právě skončil (emulace stojí, run_log.h). */
    json_object_set_int_member(data, "run_seq", (gint64)run_log_current(NULL));
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
 *  - `expected` (int, 0..65535, volitelný; observe-cas M10) - zapsat jen,
 *    když má registr právě tuto hodnotu (porovnání a zápis v emu vlákně
 *    v jednom příkazu); jinak chyba `Precondition failed: register <reg>
 *    changed (expected=0x...., current=0x....)` a nic se nezapíše
 *
 * Response payload pri uspechu:
 *  - `reg` (string) - echo
 *  - `value` (int) - echo
 *  - `previous` (int) - hodnota registru před zápisem
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
    if (json_object_has_member(data_obj, "expected")) {
        JsonNode *en = json_object_get_member(data_obj, "expected");
        gint64 ev = -1;
        if (en && JSON_NODE_HOLDS_VALUE(en) && json_node_get_value_type(en) == G_TYPE_INT64) {
            ev = json_node_get_int(en);
        }
        if (ev < 0 || ev > 0xFFFF) {
            return _err_response(req_id, "Invalid parameters: expected must be in range 0..65535",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.has_expected = 1;
        param.expected = (uint16_t)ev;
    }
    if (!_submit_dbgapi(DBGAPI_CMD_SET_REG, &param, NULL)) {
        if (param.precondition_failed && s_submit_track.last_status == DBGAPI_SUBMIT_FAILED) {
            /* observe-cas M10: registr má jinou hodnotu, nic se nezapsalo. */
            gchar *msg = g_strdup_printf(MCP_PRECONDITION_PREFIX "register %s changed "
                                         "(expected=0x%04X, current=0x%04X)",
                                         reg_name, param.expected, param.previous);
            en_MCP_DISPATCH_RESULT rc = _err_response(req_id, msg, MCP_DISPATCH_EMU_ERROR,
                                                      out_response);
            g_free(msg);
            return rc;
        }
        return _err_response(req_id, "SET_REG failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_string_member(data, "reg", reg_name);
    json_object_set_int_member(data, "value", value);
    json_object_set_int_member(data, "previous", (gint64)param.previous);
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
 *    - `regs` (object) - registry před instrukcí (st_DEBUGGER_HISTORY_REGS):
 *      `af` .. `wz` hex16, `i`, `r` hex8, `iff1`, `iff2`, `im` čísla
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
    /* Pozice ringu ze stejného okamžiku jako řádky (čte ji emu vlákno
     * v témže příkazu); dřív se četla až tady, mimo emu vlákno. */
    unsigned position = 0;
    if (!_submit_dbgapi(DBGAPI_CMD_HISTORY_GET, &position, rows)) {
        return _err_response(req_id, "HISTORY_GET failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "current_position",
                               (gint64)(position & DEBUGGER_HISTORY_POSMASK));
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
        /* Registry před instrukcí (zachycené při M1, debugger.h). */
        const st_DEBUGGER_HISTORY_REGS *hr = &rows[i].regs;
        JsonObject *ro = json_object_new();
        static const char *const n16[8] = { "af", "bc", "de", "hl", "ix", "iy", "sp", "wz" };
        const uint16_t v16[8] = { hr->af, hr->bc, hr->de, hr->hl, hr->ix, hr->iy, hr->sp, hr->wz };
        char b[8];
        for (int k = 0; k < 8; k++) {
            g_snprintf(b, sizeof(b), "%04X", v16[k]);
            json_object_set_string_member(ro, n16[k], b);
        }
        g_snprintf(b, sizeof(b), "%02X", hr->i);
        json_object_set_string_member(ro, "i", b);
        g_snprintf(b, sizeof(b), "%02X", hr->r);
        json_object_set_string_member(ro, "r", b);
        json_object_set_int_member(ro, "iff1", (hr->flags & DEBUGGER_HISTORY_F_IFF1) ? 1 : 0);
        json_object_set_int_member(ro, "iff2", (hr->flags & DEBUGGER_HISTORY_F_IFF2) ? 1 : 0);
        json_object_set_int_member(ro, "im", (hr->flags & DEBUGGER_HISTORY_F_IM_MASK) >> DEBUGGER_HISTORY_F_IM_SHIFT);
        json_object_set_object_member(e, "regs", ro);
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


/** @brief Pole `set_cpu_flags` s bitem update_mask (pořadí výstupů). */
static const struct {
    const char *name;   /**< Klíč v JSON. */
    uint16_t bit;       /**< DBGAPI_CPU_FLAGS_UM_*. */
} s_cpu_flag_fields[] = {
    { "iff1", DBGAPI_CPU_FLAGS_UM_IFF1 },
    { "iff2", DBGAPI_CPU_FLAGS_UM_IFF2 },
    { "im",   DBGAPI_CPU_FLAGS_UM_IM },
    { "i",    DBGAPI_CPU_FLAGS_UM_I },
    { "r",    DBGAPI_CPU_FLAGS_UM_R_FULL },
};

/**
 * @brief Hodnota pole `set_cpu_flags` podle bitu z dané sady (exp_* / prev_*).
 * @param p Parametr.
 * @param bit DBGAPI_CPU_FLAGS_UM_*.
 * @param prev true = prev_*, false = exp_*.
 * @return Hodnota pole.
 */
static uint8_t _cpu_flag_value(const st_DBGAPI_CPU_FLAGS *p, uint16_t bit, bool prev) {
    switch (bit) {
        case DBGAPI_CPU_FLAGS_UM_IFF1: return prev ? p->prev_iff1 : p->exp_iff1;
        case DBGAPI_CPU_FLAGS_UM_IFF2: return prev ? p->prev_iff2 : p->exp_iff2;
        case DBGAPI_CPU_FLAGS_UM_IM:   return prev ? p->prev_im   : p->exp_im;
        case DBGAPI_CPU_FLAGS_UM_I:    return prev ? p->prev_i    : p->exp_i;
        default:                       return prev ? p->prev_r    : p->exp_r;
    }
}

/**
 * @brief Přečte volitelný objekt `expected` u `set_cpu_flags` (observe-cas M10).
 *
 * Klíče `iff1`, `iff2` (bool nebo 0/1), `im` (0..2), `i`, `r` (0..255);
 * smí být jen pole, která požadavek zároveň zapisuje. `r` se porovná
 * celým bajtem (MCP zapisuje R celý, UM_R_FULL).
 *
 * @param obj Parametry požadavku.
 * @param[in,out] p Parametr SET_CPU_FLAGS s hotovou update_mask; doplní
 *                expect_mask a exp_*.
 * @return NULL = OK, jinak statický text chyby parametrů.
 */
static const char *_parse_cpu_flags_expected(JsonObject *obj, st_DBGAPI_CPU_FLAGS *p) {
    if (!json_object_has_member(obj, "expected")) return NULL;
    JsonNode *en = json_object_get_member(obj, "expected");
    if (!en || !JSON_NODE_HOLDS_OBJECT(en)) return "expected must be an object";
    JsonObject *eo = json_node_get_object(en);
    GList *keys = json_object_get_members(eo);
    const char *err = NULL;
    for (GList *k = keys; k && !err; k = k->next) {
        const char *key = (const char *)k->data;
        unsigned f = 0;
        while (f < G_N_ELEMENTS(s_cpu_flag_fields) && strcmp(s_cpu_flag_fields[f].name, key) != 0) f++;
        if (f == G_N_ELEMENTS(s_cpu_flag_fields)) {
            err = "expected has an unknown field (iff1, iff2, im, i, r)";
            break;
        }
        uint16_t bit = s_cpu_flag_fields[f].bit;
        if (!(p->update_mask & bit)) {
            err = "expected may only contain fields that the request writes";
            break;
        }
        JsonNode *v = json_object_get_member(eo, key);
        gint64 iv = -1;
        if (v && JSON_NODE_HOLDS_VALUE(v)) {
            GType t = json_node_get_value_type(v);
            if (t == G_TYPE_BOOLEAN && (bit == DBGAPI_CPU_FLAGS_UM_IFF1 || bit == DBGAPI_CPU_FLAGS_UM_IFF2)) {
                iv = json_node_get_boolean(v) ? 1 : 0;
            } else if (t == G_TYPE_INT64) {
                iv = json_node_get_int(v);
            }
        }
        gint64 max = (bit == DBGAPI_CPU_FLAGS_UM_IFF1 || bit == DBGAPI_CPU_FLAGS_UM_IFF2) ? 1
                   : (bit == DBGAPI_CPU_FLAGS_UM_IM) ? 2 : 0xFF;
        if (iv < 0 || iv > max) {
            err = "expected value out of range (iff1/iff2 0..1, im 0..2, i/r 0..255)";
            break;
        }
        switch (bit) {
            case DBGAPI_CPU_FLAGS_UM_IFF1: p->exp_iff1 = (uint8_t)iv; break;
            case DBGAPI_CPU_FLAGS_UM_IFF2: p->exp_iff2 = (uint8_t)iv; break;
            case DBGAPI_CPU_FLAGS_UM_IM:   p->exp_im   = (uint8_t)iv; break;
            case DBGAPI_CPU_FLAGS_UM_I:    p->exp_i    = (uint8_t)iv; break;
            default:                       p->exp_r    = (uint8_t)iv; break;
        }
        p->expect_mask |= bit;
    }
    g_list_free(keys);
    return err;
}

/**
 * @brief Objekt `previous` odpovědi `set_cpu_flags`: hodnoty zapsaných polí před zápisem.
 * @param p Parametr po návratu z emu vlákna.
 * @return Nový JsonObject (vlastnictví předává).
 */
static JsonObject *_cpu_flags_previous_json(const st_DBGAPI_CPU_FLAGS *p) {
    JsonObject *o = json_object_new();
    for (unsigned f = 0; f < G_N_ELEMENTS(s_cpu_flag_fields); f++) {
        if (p->update_mask & s_cpu_flag_fields[f].bit) {
            json_object_set_int_member(o, s_cpu_flag_fields[f].name,
                                       (gint64)_cpu_flag_value(p, s_cpu_flag_fields[f].bit, true));
        }
    }
    return o;
}

/**
 * @brief Chyba nesplněné podmínky `set_cpu_flags` s hodnotami polí, která nesouhlasí.
 *
 * Text: `Precondition failed: cpu flags changed (iff1: expected=1,
 * current=0; r: expected=0x12, current=0x34)`.
 *
 * @param req_id ID požadavku.
 * @param p Parametr po návratu z emu vlákna (precondition_failed).
 * @param[out] out_response Odpověď.
 */
static en_MCP_DISPATCH_RESULT _cpu_flags_cond_err(int64_t req_id, const st_DBGAPI_CPU_FLAGS *p,
                                                  char **out_response) {
    GString *s = g_string_new(MCP_PRECONDITION_PREFIX "cpu flags changed (");
    bool first = true;
    for (unsigned f = 0; f < G_N_ELEMENTS(s_cpu_flag_fields); f++) {
        uint16_t bit = s_cpu_flag_fields[f].bit;
        if (!(p->expect_mask & bit)) continue;
        uint8_t ev = _cpu_flag_value(p, bit, false);
        uint8_t cv = _cpu_flag_value(p, bit, true);
        if (ev == cv) continue;
        bool hex = (bit == DBGAPI_CPU_FLAGS_UM_I || bit == DBGAPI_CPU_FLAGS_UM_R_FULL);
        g_string_append_printf(s, hex ? "%s%s: expected=0x%02X, current=0x%02X"
                                      : "%s%s: expected=%u, current=%u",
                               first ? "" : "; ", s_cpu_flag_fields[f].name, ev, cv);
        first = false;
    }
    g_string_append(s, ")");
    en_MCP_DISPATCH_RESULT rc = _err_response(req_id, s->str, MCP_DISPATCH_EMU_ERROR, out_response);
    g_string_free(s, TRUE);
    return rc;
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
 * Volitelně `expected` (objekt, observe-cas M10): očekávané hodnoty jen
 * zapisovaných polí; při neshodě se nic nezapíše a vrátí se chyba
 * `Precondition failed: cpu flags changed (...)` s hodnotami polí.
 *
 * Response payload:
 *  - `updated` (array of string) - jména polí, která byla zapsána
 *  - `values` (object) - mapping field -> finální hodnota (echo).
 *  - `previous` (object) - zapsaná pole -> hodnota před zápisem.
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
        /* MCP zapisuje celý bajt jako LD R,A (včetně bitu 7). */
        param.update_mask |= DBGAPI_CPU_FLAGS_UM_R_FULL;
    }

    if (param.update_mask == 0) {
        return _err_response(req_id,
                             "Invalid parameters - no field specified",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    const char *eerr = _parse_cpu_flags_expected(data_obj, &param);
    if (eerr) {
        gchar *msg = g_strdup_printf("Invalid parameters: %s", eerr);
        en_MCP_DISPATCH_RESULT rc = _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS,
                                                  out_response);
        g_free(msg);
        return rc;
    }

    if (!_submit_dbgapi(DBGAPI_CMD_SET_CPU_FLAGS, &param, NULL)) {
        if (param.precondition_failed && s_submit_track.last_status == DBGAPI_SUBMIT_FAILED) {
            return _cpu_flags_cond_err(req_id, &param, out_response);
        }
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
    if (param.update_mask & DBGAPI_CPU_FLAGS_UM_R_FULL) {
        json_array_add_string_element(updated, "r");
        json_object_set_int_member(values, "r", (gint64)param.r_reg);
    }
    json_object_set_array_member(data,  "updated", updated);
    json_object_set_object_member(data, "values",  values);
    json_object_set_object_member(data, "previous", _cpu_flags_previous_json(&param));
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
 * @brief Doplní do odpovědi `regs` a `flags` ze snímku panelu CPU.
 *
 * Společné pro `get_cpu_panel_batch` a `state_observe`: `regs` má jména
 * jako `get_registers` (AF..IR, alternativní sada s podtržítkem), `flags`
 * stejné schéma jako `get_cpu_flags`.
 *
 * @param data Odpověď, do které se přidají objekty `regs` a `flags`.
 * @param out  Snímek panelu (regs_valid a flags_valid jsou vždy 1).
 */
static void _cpu_panel_core_to_json(JsonObject *data,
                                    const st_DBGAPI_CPU_PANEL_BATCH *out) {
    /* regs (vždy naplněné) - shoduje se s pojmenováním z get_registers. */
    JsonObject *regs = json_object_new();
    static const char *const reg_names[14] = {
        "AF","BC","DE","HL","AF_","BC_","DE_","HL_",
        "IX","IY","SP","PC","WZ","IR"
    };
    for (int i = 0; i < 14; i++) {
        json_object_set_int_member(regs, reg_names[i], (gint64)out->regs[i]);
    }
    json_object_set_object_member(data, "regs", regs);

    /* flags (vždy naplněné) - shoduje se s get_cpu_flags response. */
    JsonObject *flags = json_object_new();
    json_object_set_boolean_member(flags, "iff1",        out->flags.iff1 ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "iff2",        out->flags.iff2 ? TRUE : FALSE);
    json_object_set_int_member(flags,    "im",           (gint64)out->flags.im);
    json_object_set_boolean_member(flags, "halted",      out->flags.halted ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "int_pending", out->flags.int_pending ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "nmi_pending", out->flags.nmi_pending ? TRUE : FALSE);
    json_object_set_boolean_member(flags, "ei_delay",    out->flags.ei_delay ? TRUE : FALSE);
    json_object_set_int_member(flags,    "q",            (gint64)out->flags.q);
    json_object_set_int_member(flags,    "total_cycles", (gint64)out->flags.total_cycles);
    json_object_set_int_member(flags,    "frame_cycles", (gint64)out->flags.cycles);
    json_object_set_int_member(flags,    "op_tstate",    (gint64)out->flags.op_tstate);
    json_object_set_int_member(flags,    "i",            (gint64)out->flags.i_reg);
    json_object_set_int_member(flags,    "r",            (gint64)out->flags.r_reg);
    json_object_set_object_member(data, "flags", flags);
}


/**
 * @brief Doplní do odpovědi volitelné sekce panelu CPU (`im2`, `raster`, `last_instr`).
 *
 * Sekce se přidá jen tehdy, když je ve snímku platná (*_valid), tj. byla
 * vyžádaná bitem DBGAPI_CPU_PANEL_WANT_*. Společné pro
 * `get_cpu_panel_batch` a `state_observe`.
 *
 * @param data Odpověď.
 * @param out  Snímek panelu.
 */
static void _cpu_panel_sections_to_json(JsonObject *data,
                                        const st_DBGAPI_CPU_PANEL_BATCH *out) {
    if (out->im2_valid) {
        JsonObject *im2 = json_object_new();
        json_object_set_boolean_member(im2, "available",
                                        out->im2.available ? TRUE : FALSE);
        json_object_set_int_member(im2, "im",         (gint64)out->im2.im);
        json_object_set_int_member(im2, "i",          (gint64)out->im2.i_register);
        json_object_set_int_member(im2, "vec",        (gint64)out->im2.vector_byte);
        json_object_set_int_member(im2, "isr_addr",   (gint64)out->im2.isr_table_addr);
        json_object_set_int_member(im2, "isr_target", (gint64)out->im2.isr_target_addr);
        json_object_set_boolean_member(im2, "pio_irq_pending",
                                        out->im2.pio_irq_pending ? TRUE : FALSE);
        json_object_set_int_member(im2, "pio_source", (gint64)out->im2.pio_source);
        json_object_set_object_member(data, "im2", im2);
    }
    if (out->raster_valid) {
        JsonObject *raster = json_object_new();
        json_object_set_int_member(raster, "frame_number", (gint64)out->raster.frame_number);
        json_object_set_int_member(raster, "scanline",     (gint64)out->raster.scanline);
        json_object_set_int_member(raster, "column_pixel", (gint64)out->raster.column_pixel);
        json_object_set_int_member(raster, "total_cycles", (gint64)out->raster.total_cycles);
        json_object_set_int_member(raster, "frame_cycles", (gint64)out->raster.frame_cycles);
        json_object_set_object_member(data, "raster", raster);
    }
    if (out->last_instr_valid) {
        JsonObject *li = json_object_new();
        json_object_set_boolean_member(li, "valid",
                                        out->last_instr.valid ? TRUE : FALSE);
        json_object_set_int_member(li, "addr",   (gint64)out->last_instr.addr);
        json_object_set_int_member(li, "length", (gint64)out->last_instr.length);
        char hex_buf[16] = {0};
        if (out->last_instr.valid && out->last_instr.length > 0) {
            int n = out->last_instr.length;
            if (n > 4) n = 4;
            int hex_pos = 0;
            for (int b = 0; b < n; b++) {
                hex_pos += g_snprintf(hex_buf + hex_pos,
                                      sizeof(hex_buf) - hex_pos,
                                      (b == 0) ? "%02X" : " %02X",
                                      out->last_instr.bytes[b]);
            }
        }
        json_object_set_string_member(li, "bytes_hex", hex_buf);
        json_object_set_object_member(data, "last_instr", li);
    }
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
    _cpu_panel_core_to_json(data, &out);

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
    _cpu_panel_sections_to_json(data, &out);

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
 *  Aktivace má vedlejší efekt: cpuhist a mhmap v režimu vázaném na okno
 *  (WITH_WINDOW, u cpuhist i výchozí WITH_WINDOW_OR_BP) začnou zaznamenávat,
 *  takže emu_get_last_instr / history_get začnou vracet smysluplná data.
 *  Deaktivace tento záznam vypne (WITH_WINDOW_OR_BP zaznamenává dál, dokud
 *  existuje povolený breakpoint).
 * ============================================================================ */


/**
 * @brief `debugger_activate` handler - programatické zapnutí debuggeru.
 *
 * Bez parametrů. Proxy na `DBGAPI_CMD_DEBUGGER_ACTIVATE`. Backend
 * nastaví `g_debugger.active = 1` a na emu vlákně přepočítá CPU callbacky
 * (`mzarch_platform_fn_debugger_state_changed`), takže v WITH_WINDOW
 * režimu implicitně zapne cpuhist + mhmap recording. Okno debuggeru
 * v GUI se neotevírá.
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
 * nastaví `g_debugger.active = 0` a přepočítá CPU callbacky. Side effect:
 * cpuhist + mhmap recording v WITH_WINDOW režimu se vypne. Otevřené okno
 * debuggeru v GUI se nezavírá.
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

#if HAVE_PIOZ80
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
#else
    /* MZ-700 path - PIO-Z80 v této platformě neexistuje. Vrátíme
     * available=false místo regulérní chyby, aby klient mohl Tool
     * volat platform-agnosticky a sám se rozhodnout co dál. */
    (void)req;
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "available", FALSE);
    json_object_set_string_member(data, "reason",
                                   "platform has no Z80 PIO");
    return _ok_response(req_id, data, out_response);
#endif
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
 *  - `warning` (string, jen někdy) - BP vznikl s `addr_end` různým od
 *    `addr`, ale `addr_match_mode` zůstal SINGLE (nebyl v `fields`
 *    nebo je SINGLE): BP hlídá jen `addr`, rozsah se neuplatní. Chování
 *    se nemění, jde jen o upozornění klienta (dříve tichá past).
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
    /* Nový BP má výchozí addr_match_mode SINGLE (breakpoints_add_auto);
     * addr_end se pak neuplatní. Upozorni, ale nic neměň. */
    bool mode_single = !(mask & DBGAPI_BP_UM_ADDR_MATCH_MODE)
                       || (p.addr_match_mode == (uint8_t)BP_MATCH_SINGLE);
    if ((mask & DBGAPI_BP_UM_ADDR_END) && (p.addr_end != p.addr) && mode_single) {
        char msg[200];
        g_snprintf(msg, sizeof(msg),
                   "addr_end (0x%04X) differs from addr (0x%04X), but "
                   "addr_match_mode is SINGLE: the breakpoint matches only "
                   "addr. Set addr_match_mode to RANGE to match the whole "
                   "range.",
                   (unsigned)p.addr_end, (unsigned)p.addr);
        json_object_set_string_member(resp, "warning", msg);
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
 * @brief Načte volitelný parametr `want_data` (default true).
 *
 * @param obj Objekt `data` požadavku (smí být NULL).
 * @param[out] out Hodnota parametru.
 * @return false = parametr není bool.
 */
static bool _parse_want_data(JsonObject *obj, bool *out) {
    *out = true;
    if (!obj || !json_object_has_member(obj, "want_data")) return true;
    JsonNode *n = json_object_get_member(obj, "want_data");
    if (!n || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return false;
    *out = json_node_get_boolean(n) ? true : false;
    return true;
}


/**
 * @brief Doplní do odpovědi čtení pole `sha256` (a `data_b64`, chce-li klient data).
 *
 * SHA-256 přes GLib (stejná implementace jako checksum snapshotu), 64 hex
 * znaků malými písmeny, nad přečtenými bajty.
 *
 * @param resp Odpověď.
 * @param buf Přečtená data.
 * @param len Délka dat.
 * @param want_data false = bez `data_b64` (jen hash).
 */
static void _read_result_to_json(JsonObject *resp, const uint8_t *buf, gsize len,
                                 bool want_data) {
    gchar *hex = g_compute_checksum_for_data(G_CHECKSUM_SHA256, buf, len);
    json_object_set_string_member(resp, "sha256", hex ? hex : "");
    g_free(hex);
    if (want_data) {
        gchar *b64 = g_base64_encode(buf, len);
        json_object_set_string_member(resp, "data_b64", b64 ? b64 : "");
        g_free(b64);
    }
}


/* ------------------------------------------------------------------ */
/* Podmíněné zápisy (observe-cas M10, V1)                              */
/* ------------------------------------------------------------------ */

/**
 * @brief SHA-256 (32 bajtů) jako 64 hex znaků malými písmeny.
 * @param d Hash.
 * @param[out] out Buffer aspoň 65 znaků.
 */
static void _sha256_to_hex(const uint8_t d[32], char out[65]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 0x0F];
    }
    out[64] = '\0';
}

/**
 * @brief Přečte volitelné `expected_sha256` a `want_hashes` zápisu paměti.
 *
 * `expected_sha256` = 64 hex znaků (velikost písmen libovolná), SHA-256
 * přesně přepisovaného rozsahu, stejný jako `sha256` z `mem_read` /
 * `region_read` / okna `state_observe` téhož rozsahu.
 *
 * @param obj Parametry požadavku.
 * @param[out] c Podmínka (vynuluje se).
 * @param[out] used true = klient podmínku nebo hashe chce (předat `c`
 *             emu vláknu), false = zápis beze změny chování.
 * @return NULL = OK, jinak statický text chyby parametrů.
 */
static const char *_parse_write_cond(JsonObject *obj, st_DBGAPI_WRITE_COND *c, bool *used) {
    memset(c, 0, sizeof(*c));
    *used = false;
    if (json_object_has_member(obj, "expected_sha256")) {
        JsonNode *n = json_object_get_member(obj, "expected_sha256");
        if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) {
            return "expected_sha256 must be a string of 64 hex digits";
        }
        const char *h = json_node_get_string(n);
        if (strlen(h) != 64) return "expected_sha256 must be a string of 64 hex digits";
        for (int i = 0; i < 32; i++) {
            int hi = g_ascii_xdigit_value(h[2 * i]);
            int lo = g_ascii_xdigit_value(h[2 * i + 1]);
            if (hi < 0 || lo < 0) return "expected_sha256 must be a string of 64 hex digits";
            c->expected[i] = (uint8_t)((hi << 4) | lo);
        }
        c->has_expected = 1;
        *used = true;
    }
    if (json_object_has_member(obj, "want_hashes")) {
        JsonNode *n = json_object_get_member(obj, "want_hashes");
        if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) {
            return "want_hashes must be a boolean";
        }
        if (json_node_get_boolean(n)) {
            c->want_hashes = 1;
            *used = true;
        }
    }
    return NULL;
}

/**
 * @brief Doplní do odpovědi zápisu `before_sha256` a `after_sha256`.
 * @param resp Odpověď.
 * @param c Podmínka po návratu z emu vlákna (bez hashů nic nepřidá).
 */
static void _write_cond_to_json(JsonObject *resp, const st_DBGAPI_WRITE_COND *c) {
    char hex[65];
    if (!c->hashes_valid) return;
    _sha256_to_hex(c->before, hex);
    json_object_set_string_member(resp, "before_sha256", hex);
    if (c->written) {
        _sha256_to_hex(c->after, hex);
        json_object_set_string_member(resp, "after_sha256", hex);
    }
}

/**
 * @brief Chybová odpověď nesplněné podmínky zápisu paměti.
 *
 * Text: `Precondition failed: memory changed (expected_sha256=<hex>,
 * current_sha256=<hex>)`; hodnoty jsou jen v textu, drát u chyby `data`
 * nenese.
 *
 * @param req_id ID požadavku.
 * @param c Podmínka po návratu z emu vlákna (precondition_failed).
 * @param[out] out_response Odpověď.
 */
static en_MCP_DISPATCH_RESULT _write_cond_err(int64_t req_id, const st_DBGAPI_WRITE_COND *c,
                                              char **out_response) {
    char want[65], cur[65];
    _sha256_to_hex(c->expected, want);
    _sha256_to_hex(c->before, cur);
    gchar *msg = g_strdup_printf(MCP_PRECONDITION_PREFIX "memory changed "
                                 "(expected_sha256=%s, current_sha256=%s)", want, cur);
    en_MCP_DISPATCH_RESULT rc = _err_response(req_id, msg, MCP_DISPATCH_EMU_ERROR, out_response);
    g_free(msg);
    return rc;
}


/**
 * @brief `mem_read` handler - čte blok paměti.
 *
 * Parametry v `data` poli requestu:
 *  - `addr` (int, 0..65535) - počáteční adresa
 *  - `len`  (int, 1..65536) - počet bajtů; součet addr+len musí být
 *    <= 0x10000 (celých 64 KiB jen od adresy 0), jinak INVALID_PARAMS
 *    (MEM_READ by adresu za FFFFh přetočil na 0000h, my takový blok
 *    odmítáme)
 *
 *  - `want_data` (bool, volitelný, default true) - false = jen hash
 *
 * Response payload:
 *  - `addr` (int) - echo
 *  - `len`  (int) - echo
 *  - `sha256` (string) - SHA-256 přečtených bajtů (64 hex, malá písmena)
 *  - `data_b64` (string, jen s want_data) - obsah paměti zakódovaný base64
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
    if (addr < 0 || addr > 0xFFFF || len <= 0 || len > 0x10000 ||
        (addr + len) > 0x10000) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool want_data = true;
    if (!_parse_want_data(data_obj, &want_data)) {
        return _err_response(req_id, "want_data must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint8_t *buf = g_malloc0((gsize)len);
    st_DBGAPI_MEM_PARAM param = {
        .addr = (uint16_t)addr,
        .len  = (uint32_t)len,
        .buf  = buf,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_MEM_READ, &param, NULL)) {
        g_free(buf);
        return _err_response(req_id, "MEM_READ failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr", addr);
    json_object_set_int_member(resp, "len",  len);
    _read_result_to_json(resp, buf, (gsize)len, want_data);
    g_free(buf);
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
 *    mezerou oddělené dvojice (`"DE AD BE EF"`). Počet bajtů 1..65536,
 *    `addr` + počet <= 0x10000 (celých 64 KiB jen od adresy 0).
 *
 * Implementace volá `DBGAPI_CMD_MEM_WRITE_CHECKED` (= zápis s region
 * verifikací). Pokud kterákoliv adresa padne do ROM, prohibited nebo
 * unmapped regionu, žádný bajt se nezapíše a handler vrátí success=false
 * s polem `first_failed_addr` v error payloadu.
 *
 * Volitelně (observe-cas M10, podmíněný zápis):
 *  - `expected_sha256` (string, 64 hex) - zapsat jen, když SHA-256
 *    přepisovaného rozsahu (addr..addr+délka-1, přes aktuální mapování)
 *    souhlasí; kontrola i zápis proběhnou v emu vlákně v jednom příkazu
 *  - `want_hashes` (bool) - vrátit `before_sha256` / `after_sha256`
 *    i bez podmínky
 *
 * Response data (success):
 *  - `addr`   (int) - echo
 *  - `length` (int) - počet zapsaných bajtů
 *  - `before_sha256`, `after_sha256` (string) - jen s `expected_sha256`
 *    nebo `want_hashes`
 *
 * Response data (failure region check):
 *  - JSONL `success=false` + `error="MEM_WRITE region check failed"`
 *    (kontrola regionu má přednost před podmínkou)
 *
 * Response (nesplněná podmínka): `Precondition failed: memory changed
 * (expected_sha256=..., current_sha256=...)`, nic se nezapsalo.
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
     * (1..65536) a chrání před monstrózními alokacemi z malicious vstupu. */
    const gsize MAX_LEN = 0x10000;
    uint8_t *bytes = g_malloc(MAX_LEN);
    int decoded = _decode_hex(data_hex, bytes, MAX_LEN);
    if (decoded <= 0 || (addr + decoded) > 0x10000) {
        g_free(bytes);
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_WRITE_COND cond;
    bool use_cond = false;
    const char *cerr = _parse_write_cond(data_obj, &cond, &use_cond);
    if (cerr) {
        g_free(bytes);
        gchar *msg = g_strdup_printf("Invalid parameters: %s", cerr);
        en_MCP_DISPATCH_RESULT rc = _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS,
                                                  out_response);
        g_free(msg);
        return rc;
    }

    st_DBGAPI_MEM_WRITE_CHECKED_PARAM param = {
        .addr               = (uint16_t)addr,
        .length             = (uint32_t)decoded,
        .data               = bytes,
        .success            = 0,
        .first_failed_addr  = 0,
        .first_failed_kind  = 0,
        .cond               = use_cond ? &cond : NULL,
    };
    bool submit_ok = _submit_dbgapi(DBGAPI_CMD_MEM_WRITE_CHECKED, &param, NULL);
    g_free(bytes);

    /* Zamítnutí kontrolou regionu vrací backend jako neúspěch příkazu
     * (DBGAPI_SUBMIT_FAILED) s vyplněným first_failed_addr/kind; ostatní
     * neúspěchy odeslání (timeout, plná fronta) jsou chyba emulátoru. */
    if (!submit_ok && s_submit_track.last_status != DBGAPI_SUBMIT_FAILED) {
        return _err_response(req_id, "MEM_WRITE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    /* Nesplněná podmínka (observe-cas M10): kontrola regionu prošla,
     * ale rozsah má jiný hash než expected_sha256; nic se nezapsalo. */
    if (!submit_ok && use_cond && cond.precondition_failed) {
        return _write_cond_err(req_id, &cond, out_response);
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
    if (use_cond) _write_cond_to_json(resp, &cond);
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
 *    `{id, addr, addr_end, addr_match_mode, addr_mask, enabled, type,
 *    zone, bank_id, bank_id_end, bank_match_mode, bank_id_mask, hits,
 *    condition}`. Bankové `bank_id_end` / `bank_id_mask` platí jen pro
 *    `bank_match_mode` RANGE / MASK (jména polí jako v .bpt exportu).
 *    `type` / `zone` jsou kanonické UPPER_SNAKE řetězce
 *    (bpt_type_to_string / bp_zone_to_string), `addr_match_mode` je
 *    "SINGLE" / "RANGE" / "MASK" (bp_match_mode_to_string); `addr_end`
 *    platí jen pro RANGE, `addr_mask` jen pro MASK. `condition` je expr
 *    výraz nebo null pokud je BP bezpodmínečný.
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
        json_object_set_int_member(item, "addr_end", result->bp[i].addr_end);
        json_object_set_string_member(item, "addr_match_mode",
            bp_match_mode_to_string(
                (en_BP_MATCH_MODE)result->bp[i].addr_match_mode));
        json_object_set_int_member(item, "addr_mask", result->bp[i].addr_mask);
        json_object_set_boolean_member(item, "enabled",
                                       result->bp[i].enabled);
        json_object_set_string_member(item, "type",
            bpt_type_to_string((en_BPT_TYPE)result->bp[i].type));
        json_object_set_string_member(item, "zone",
            bp_zone_to_string((en_BP_ZONE)result->bp[i].zone));
        json_object_set_int_member(item, "bank_id", result->bp[i].bank_id);
        json_object_set_int_member(item, "bank_id_end",
                                   result->bp[i].bank_id_end);
        json_object_set_string_member(item, "bank_match_mode",
            bp_match_mode_to_string(
                (en_BP_MATCH_MODE)result->bp[i].bank_match_mode));
        json_object_set_int_member(item, "bank_id_mask",
                                   result->bp[i].bank_id_mask);
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
 * @brief Počká, až emulátor dokončí vyžádaný krok jedné instrukce.
 *
 * Krok se provede po dokončení příkazu kroku, v pauze se fronta dbgapi
 * vybírá až po kroku (dbgapi_emu_drain_paused()). Prázdný příkaz
 * (DBGAPI_CMD_NONE) se tedy zpracuje až po kroku; po jeho návratu je
 * krok hotový.
 *
 * @return false = příkaz neproběhl (emulátor nedostupný).
 */
static bool _wait_step_done(void) {
    return _submit_dbgapi(DBGAPI_CMD_NONE, NULL, NULL);
}


/**
 * @brief Doplní do odpovědi kroku pole `completed`, `running`, `run_seq` (N4).
 *
 * @param resp Odpověď (už má `stepped: true` kvůli kompatibilitě).
 * @param rr Výsledek příkazu kroku.
 * @return true = krok jedné instrukce, na jehož dokončení se má počkat.
 */
static bool _step_result_to_json(JsonObject *resp, const st_DBGAPI_RUN_START_RESULT *rr) {
    bool single = (rr->status == DBGAPI_RUN_TO_STARTED) && rr->step;
    bool run = (rr->status == DBGAPI_RUN_TO_STARTED) && !rr->step;
    json_object_set_boolean_member(resp, "completed", single ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "running",
                                   (run || rr->status == DBGAPI_RUN_START_NOT_PAUSED) ? TRUE : FALSE);
    if (rr->status == DBGAPI_RUN_TO_PAUSED_ONLY) {
        json_object_set_boolean_member(resp, "paused_only", TRUE);
    }
    json_object_set_int_member(resp, "run_seq", (gint64)rr->run_seq);
    return single;
}


/**
 * @brief `step_into` handler - 1 instrukce krok (step into CALL/RST).
 *
 * Žádné parametry. Volá `DBGAPI_CMD_STEP_INTO`. Pokud emu běží (= není
 * pause), krok se neprovede a emulace běží dál (`completed: false`,
 * `running: true`); klient musí nejdřív pauznout (`emu_pause` ->
 * `emu_step_into`). V pauze handler počká na provedení kroku.
 *
 * Response:
 *  - `stepped` (bool) - true (kompatibilita, nic neříká o provedení)
 *  - `completed` (bool) - krok proběhl, stav je po kroku
 *  - `running` (bool) - emulace běží (krok se neprovedl)
 *  - `run_seq` (int) - číslo běhu kroku (nebo běžícího běhu)
 */
static en_MCP_DISPATCH_RESULT _handle_step_into(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_RUN_START_RESULT rr;
    memset(&rr, 0, sizeof(rr));
    if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, &rr)) {
        return _err_response(req_id, "step_into failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "stepped", TRUE);
    if (_step_result_to_json(resp, &rr) && !_wait_step_done()) {
        json_object_set_boolean_member(resp, "completed", FALSE);
    }
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `step_over` handler - 1 instrukce, ale CALL/RST jako atom.
 *
 * Žádné parametry. Volá `DBGAPI_CMD_STEP_OVER`. Pro non-call instrukce
 * se chová jako step_into (handler počká na provedení kroku); pro
 * CALL/RST/DJNZ/blokové instrukce nastaví dočasný BP na addr+length
 * a run-to: odpověď přijde hned (`completed: false`, `running: true`)
 * a konec běhu ohlásí událost `paused` (`reason: "run_to"`) nebo
 * `wait_stop`. Za běhu emulaci jen pauzne (`paused_only: true`).
 *
 * Response:
 *  - `stepped` (bool) - true (kompatibilita, nic neříká o provedení)
 *  - `completed` (bool), `running` (bool), `paused_only` (bool, jen při
 *    pauze běžící emulace), `run_seq` (int) - viz step_into
 */
static en_MCP_DISPATCH_RESULT _handle_step_over(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_RUN_START_RESULT rr;
    memset(&rr, 0, sizeof(rr));
    if (!_submit_dbgapi(DBGAPI_CMD_STEP_OVER, NULL, &rr)) {
        return _err_response(req_id, "step_over failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "stepped", TRUE);
    if (_step_result_to_json(resp, &rr) && !_wait_step_done()) {
        json_object_set_boolean_member(resp, "completed", FALSE);
    }
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
    /* Poslední krok se provede až po dokončení svého příkazu; odpověď
     * má vidět stav po něm. */
    if (done > 0) (void)_wait_step_done();
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
 *  - `max_cycles` (int, volitelný) - jen informativní, nic neomezuje
 *    (kvůli kompatibilitě se dál jen validuje a vrací). Omezení běhu je
 *    `deadline`.
 *  - `deadline` (objekt, volitelný) - {frames|emulated_ms|ticks: N}: běh
 *    se zastaví v emulovaném čase (reason "deadline"), pokud cíle
 *    nedosáhne dřív
 *
 * Response:
 *  - `addr` (int) - echo
 *  - `running` (bool) - true (emu nastartoval temp-BP + run; dosažení
 *    cíle vyšle událost `paused` s `reason: "run_to"`, na konec běhu jde
 *    čekat přes `wait_stop`)
 *  - `run_seq` (int) - číslo spuštěného běhu
 *  - `deadline_ticks` (int, jen s deadline) - délka deadlinu v taktech GDG
 *
 * Volá `DBGAPI_CMD_RUN_TO` s data_ptr = uint16_t* (cílová adresa)
 * a result_ptr = st_DBGAPI_RUN_START_RESULT* (status DBGAPI_RUN_TO_*).
 *
 * Předpoklad: emulace v pauze. Pokud běží, handler vrátí chybu
 * (MCP_DISPATCH_EMU_ERROR, "Emulator is running ...") a stav emulace
 * nemění - klient musí předem zavolat `pause`. (Dřív se to tak jen
 * popisovalo: dbgapi RUN_TO za běhu podle UX "Run to cursor" v GUI emu
 * jen pauzne a vrátí úspěch, takže MCP hlásilo `running: true`, emulace
 * stála mimo cíl a klient čekající na pauzu si myslel, že cíle dosáhla.)
 * Pokud emu někdo rozběhne mezi kontrolou a zpracováním příkazu, dbgapi
 * ho pauzne a vrátí DBGAPI_RUN_TO_PAUSED_ONLY - pak handler také vrátí
 * chybu a uvede, že emulace je teď v pauze.
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
    st_DBGAPI_RUN_DEADLINE_PARAM dl;
    bool has_deadline = false;
    char dl_err[128];
    if (!_parse_deadline(data_obj, &dl, &has_deadline, dl_err, sizeof(dl_err))) {
        return _err_response(req_id, dl_err, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
#ifndef MZ800EMU_MCP_TEST_BUILD
    if (!EMULATOR_TEST_PAUSED) {
        return _err_response(req_id,
                             "Emulator is running: run_until_addr needs a "
                             "paused emulator (pause it first, e.g. emu_pause)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
#endif
    /* Deadline se nastaví v pauze těsně před rozběhem a platí do nejbližší
     * pauzy; když se běh nespustí, zruší se. */
    if (has_deadline && !_arm_deadline(&dl)) {
        return _err_response(req_id, "Invalid deadline",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint16_t target = (uint16_t)addr;
    st_DBGAPI_RUN_START_RESULT run_to_result;
    memset(&run_to_result, 0, sizeof(run_to_result));
    run_to_result.status = DBGAPI_RUN_TO_STARTED;
    if (!_submit_dbgapi(DBGAPI_CMD_RUN_TO, &target, &run_to_result)) {
        if (has_deadline) _clear_deadline();
        return _err_response(req_id, "run_until_addr failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (run_to_result.status == DBGAPI_RUN_TO_PAUSED_ONLY) {
        if (has_deadline) _clear_deadline();
        return _err_response(req_id,
                             "Emulator was running: run_until_addr needs a "
                             "paused emulator; it has been paused now, call "
                             "run_until_addr again",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr", addr);
    json_object_set_int_member(resp, "max_cycles", max_cycles);
    json_object_set_boolean_member(resp, "running", TRUE);
    json_object_set_int_member(resp, "run_seq", (gint64)run_to_result.run_seq);
    _deadline_to_json(resp, has_deadline, &dl);
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
 * @brief Sestaví chybovou odpověď neúspěšného načtení snapshotu s důvodem.
 *
 * Text důvodu bere ze snapshot_result_to_string() (anglicky, bez
 * lokalizace). Pro SNAPSHOT_ERR_NOT_PAUSED přidá radu, že je třeba
 * emulaci nejdřív pauznout (snapshot_load / snapshot_load_buffer to
 * vyžadují, mcp-inbox-fixes). Kód 0 = handler neproběhl (timeout nebo
 * plná fronta dbgapi) - pak jen obecná zpráva.
 *
 * @param[in]  req_id        id requestu
 * @param[in]  op            jméno operace do zprávy ("snapshot_load", ...)
 * @param[in]  result_code   en_SNAPSHOT_RESULT z st_DBGAPI_SNAPSHOT_PARAM.result
 * @param[out] out_response  vlastněná odpověď (caller free)
 * @return výsledek _err_response (MCP_DISPATCH_EMU_ERROR)
 */
static en_MCP_DISPATCH_RESULT _snapshot_load_err_response(int64_t req_id,
                                                          const char *op,
                                                          int result_code,
                                                          char **out_response) {
    char *msg;
    if (result_code == SNAPSHOT_ERR_NOT_PAUSED) {
        msg = g_strdup_printf("%s failed: %s (pause it first, e.g. "
                              "emu_pause)", op,
                              snapshot_result_to_string(SNAPSHOT_ERR_NOT_PAUSED));
    } else if (result_code != SNAPSHOT_OK) {
        msg = g_strdup_printf("%s failed: %s (result_code %d)", op,
                              snapshot_result_to_string(
                                  (en_SNAPSHOT_RESULT)result_code),
                              result_code);
    } else {
        msg = g_strdup_printf("%s failed", op);
    }
    en_MCP_DISPATCH_RESULT rc = _err_response(req_id, msg,
                                              MCP_DISPATCH_EMU_ERROR,
                                              out_response);
    g_free(msg);
    return rc;
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
 * Předpoklad: emulace v pauze (snapshot_load() jinak vrátí
 * SNAPSHOT_ERR_NOT_PAUSED). Při chybě odpověď uvádí důvod
 * (_snapshot_load_err_response).
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
        return _snapshot_load_err_response(req_id, "snapshot_load",
                                           result_code, out_response);
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
 * Předpoklad: emulace v pauze (jako snapshot_load). Při chybě odpověď
 * uvádí důvod (_snapshot_load_err_response).
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
        return _snapshot_load_err_response(req_id, "snapshot_load_buffer",
                                           param.result, out_response);
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
 * Parametry v `data` (volitelné):
 *  - `max_cycles` (int, default 10000000) - jen informativní, nic
 *    neomezuje (validuje se a vrací kvůli kompatibilitě).
 *  - `deadline` (objekt) - {frames|emulated_ms|ticks: N}: běh se zastaví
 *    v emulovaném čase (reason "deadline"), pokud RET nedosáhne dřív.
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
 *  - `run_seq`     (int)  - číslo spuštěného běhu (čekání přes wait_stop)
 *  - `deadline_ticks` (int, jen s deadline) - délka deadlinu v taktech GDG
 *
 * Response při chybě:
 *  - error message + status code v JSONL `error` poli (anglicky).
 */
static en_MCP_DISPATCH_RESULT _handle_step_out(const st_JSONL_MESSAGE *req,
                                                char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    gint64 max_cycles = 10000000;
    JsonObject *data_obj = NULL;
    if (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT) {
        data_obj = json_node_get_object(data_node);
        max_cycles = _obj_int_or(data_obj, "max_cycles", 10000000);
        if (max_cycles < 1) {
            return _err_response(req_id, "Invalid max_cycles",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    st_DBGAPI_RUN_DEADLINE_PARAM dl;
    bool has_deadline = false;
    char dl_err[128];
    if (!_parse_deadline(data_obj, &dl, &has_deadline, dl_err, sizeof(dl_err))) {
        return _err_response(req_id, dl_err, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (has_deadline && !_arm_deadline(&dl)) {
        return _err_response(req_id, "Invalid deadline",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_STEP_OUT_PARAM param;
    memset(&param, 0, sizeof(param));
    param.max_cycles = (uint32_t)((max_cycles > 0xFFFFFFFFLL)
                                  ? 0xFFFFFFFFu : (uint32_t)max_cycles);
    bool ok = _submit_dbgapi(DBGAPI_CMD_STEP_OUT, &param, NULL);
    if (!ok) {
        if (has_deadline) _clear_deadline();
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
    json_object_set_int_member(resp, "run_seq", (gint64)param.run_seq);
    _deadline_to_json(resp, has_deadline, &dl);
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
    bool time_limited = false;
    uint32_t safety_iter = 0;
    const uint32_t SAFETY_MAX_ITER = 5000000;  /* anti-runaway hard cap */
    const gint64 t_end = g_get_monotonic_time()
                         + (gint64)MCP_STEP_LOOP_TIME_LIMIT_MS * G_TIME_SPAN_MILLISECOND;
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
        if (g_get_monotonic_time() >= t_end) {
            time_limited = true;
            break;
        }
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
    json_object_set_boolean_member(resp, "time_limited", time_limited ? TRUE : FALSE);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `run_until_tstate` s `target_ticks`: běh do času GDG (64 bit).
 *
 * Emulace v pauze se rozběhne a emu vlákno ji zastaví na začátku první
 * instrukce s časem GDG >= `target_ticks` (DBGAPI_CMD_RUN_UNTIL_TICKS,
 * run_control_stop_at_arm()); dispatch čeká na konec běhu (run_log_wait())
 * nejvýš MCP_STEP_LOOP_TIME_LIMIT_MS, potom emulaci pozastaví
 * (`time_limited`). Čas GDG reset nenuluje a nepřetéká (na rozdíl od
 * `total_cycles`, N84). `max_cycles` se v této cestě nepoužívá.
 *
 * Response: `ticks` (čas v okamžiku zastavení), `target_ticks`,
 * `delta_ticks` (od začátku příkazu), `exact` (ticks == target_ticks),
 * `reached`, `stopped_by` (důvod zastavení, např. "breakpoint", když běh
 * dřív zastavil uživatelský breakpoint), `time_limited`, `run_seq`.
 *
 * @param req_id       ID požadavku.
 * @param data_obj     Parametry požadavku (obsahují `target_ticks`).
 * @param out_response Výstupní JSONL řádek (volající `free()`).
 * @return MCP_DISPATCH_OK nebo chybový kód (neplatný parametr, běžící emulace).
 */
static en_MCP_DISPATCH_RESULT _run_until_ticks(int64_t req_id, JsonObject *data_obj,
                                               char **out_response) {
    if (json_object_has_member(data_obj, "target_total_cycles")) {
        return _err_response(req_id, "Use either target_ticks or target_total_cycles, not both",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 target = _obj_int_or(data_obj, "target_ticks", -1);
    if (target < 0) {
        return _err_response(req_id, "Invalid target_ticks (must be >= 0)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
#ifndef MZ800EMU_MCP_TEST_BUILD
    if (!EMULATOR_TEST_PAUSED) {
        return _err_response(req_id,
                             "Emulator is running: run_until_tstate needs a "
                             "paused emulator (pause it first, e.g. emu_pause)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
#endif
    st_DBGAPI_RUN_UNTIL_TICKS_PARAM p;
    memset(&p, 0, sizeof(p));
    p.target_ticks = (uint64_t)target;
    st_DBGAPI_RUN_START_RESULT r;
    memset(&r, 0, sizeof(r));
    if (!_submit_dbgapi(DBGAPI_CMD_RUN_UNTIL_TICKS, &p, &r)) {
        return _err_response(req_id, "run_until_tstate failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (r.status == DBGAPI_RUN_TO_PAUSED_ONLY) {
        return _err_response(req_id,
                             "Emulator was running: run_until_tstate needs a "
                             "paused emulator; it has been paused now, call "
                             "run_until_tstate again",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    bool reached = p.reached_now;
    bool time_limited = false;
    const char *stopped_by = "run_until_ticks";
    uint64_t landed = p.now_ticks;
    if (!p.reached_now) {
        st_RUN_LOG_RECORD rec;
        bool found = false;
        en_RUN_LOG_WAIT w = run_log_wait(r.run_seq, MCP_STEP_LOOP_TIME_LIMIT_MS, &rec, &found);
        if (w == RUN_LOG_WAIT_TIMEOUT) {
            /* Cíl je moc daleko: pauza zruší i zastavení na čase. */
            time_limited = true;
            _submit_dbgapi(DBGAPI_CMD_PAUSE, NULL, NULL);
            w = run_log_wait(r.run_seq, MCP_WAIT_STOP_MAX_MS, &rec, &found);
        }
        if (w != RUN_LOG_WAIT_DONE || !found) {
            return _err_response(req_id, "run_until_tstate: the run did not stop",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        landed = rec.stop_ticks;
        stopped_by = rec.reason;
        reached = !time_limited && (strcmp(rec.reason, "run_until_ticks") == 0);
    }

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "ticks", (gint64)landed);
    json_object_set_int_member(resp, "target_ticks", target);
    json_object_set_int_member(resp, "delta_ticks", (gint64)(landed - p.now_ticks));
    json_object_set_boolean_member(resp, "exact", (landed == (uint64_t)target) ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "reached", reached ? TRUE : FALSE);
    json_object_set_string_member(resp, "stopped_by", stopped_by);
    json_object_set_boolean_member(resp, "time_limited", time_limited ? TRUE : FALSE);
    json_object_set_int_member(resp, "run_seq", (gint64)r.run_seq);
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
 *
 * S parametrem `target_ticks` místo `target_total_cycles` běží cesta
 * _run_until_ticks() (čas v 64bitových taktech GDG, zastavení v emu
 * vlákně), viz tam.
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
    if (json_object_has_member(data_obj, "target_ticks")) {
        return _run_until_ticks(req_id, data_obj, out_response);
    }
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
    bool time_limited = false;
    uint32_t safety_iter = 0;
    const uint32_t SAFETY_MAX_ITER = 5000000;
    const gint64 t_end = g_get_monotonic_time()
                         + (gint64)MCP_STEP_LOOP_TIME_LIMIT_MS * G_TIME_SPAN_MILLISECOND;
    while (safety_iter < SAFETY_MAX_ITER) {
        if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL)) break;
        if (!_get_raster_now(&now)) break;
        if (now.total_cycles >= (uint32_t)target) {
            reached = true;
            break;
        }
        if ((now.total_cycles - start.total_cycles) >= (uint32_t)max_cycles) break;
        if (g_get_monotonic_time() >= t_end) {
            time_limited = true;
            break;
        }
        safety_iter++;
    }

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "total_cycles", (gint64)now.total_cycles);
    json_object_set_int_member(resp, "target",       target);
    json_object_set_int_member(resp, "delta_cycles",
                               (gint64)(now.total_cycles - start.total_cycles));
    json_object_set_boolean_member(resp, "reached", reached ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "time_limited", time_limited ? TRUE : FALSE);
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
        bool time_limited = false;
        uint32_t safety_iter = 0;
        const uint32_t SAFETY_MAX_ITER = 5000000;
        uint32_t target_frame = start.frame_number + (uint32_t)count;
        const gint64 t_end = g_get_monotonic_time()
                             + (gint64)MCP_STEP_LOOP_TIME_LIMIT_MS * G_TIME_SPAN_MILLISECOND;
        while (safety_iter < SAFETY_MAX_ITER) {
            if (!_submit_dbgapi(DBGAPI_CMD_STEP_INTO, NULL, NULL)) break;
            if (!_get_raster_now(&now)) break;
            if (now.frame_number >= target_frame) {
                reached = true;
                break;
            }
            if ((now.total_cycles - start.total_cycles)
                    >= (uint32_t)max_cycles) break;
            if (g_get_monotonic_time() >= t_end) {
                time_limited = true;
                break;
            }
            safety_iter++;
        }
        JsonObject *resp = json_object_new();
        json_object_set_string_member(resp, "kind", "frame_done");
        json_object_set_int_member(resp, "frames_done",
            (gint64)(now.frame_number - start.frame_number));
        json_object_set_int_member(resp, "delta_cycles",
            (gint64)(now.total_cycles - start.total_cycles));
        json_object_set_boolean_member(resp, "reached", reached ? TRUE : FALSE);
        json_object_set_boolean_member(resp, "time_limited", time_limited ? TRUE : FALSE);
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
        bool time_limited = false;
        uint32_t safety_iter = 0;
        const uint32_t SAFETY_MAX_ITER = 100000;  /* polling round-trip
                                                   * je drahý, méně iterací */
        const gint64 t_end = g_get_monotonic_time()
                             + (gint64)MCP_STEP_LOOP_TIME_LIMIT_MS * G_TIME_SPAN_MILLISECOND;
        while (safety_iter < SAFETY_MAX_ITER) {
            bool running = true;
            if (!_submit_dbgapi(DBGAPI_CMD_IS_RUNNING, NULL, &running)) break;
            if (!running) {
                reached = true;
                break;
            }
            if (!_get_raster_now(&now)) break;
            bool over_cycles = (now.total_cycles - start.total_cycles) >= (uint32_t)max_cycles;
            time_limited = !over_cycles && (g_get_monotonic_time() >= t_end);
            if (over_cycles || time_limited) {
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
        json_object_set_boolean_member(resp, "time_limited", time_limited ? TRUE : FALSE);
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
 * Parametry: `data.addr` (0..65535), `data.data_hex` (hex string,
 * 1..65536 bajtů, `addr` + počet <= 0x10000).
 * Identicky validuje vstup jako `mem_write`, ale forwarduje na
 * DBGAPI_CMD_MEM_WRITE_FORCE (= bez region check), takže zápis
 * proběhne i do ROM oblastí (pokud banking dovolí).
 * Volitelně `expected_sha256` a `want_hashes` jako u `mem_write`
 * (podmíněný zápis, observe-cas M10).
 *
 * Response: `{"addr": <int>, "length": <int>}`, s podmínkou nebo
 * `want_hashes` navíc `before_sha256` a `after_sha256`. Nesplněná
 * podmínka = chyba `Precondition failed: memory changed (...)`.
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
    st_DBGAPI_WRITE_COND cond;
    bool use_cond = false;
    const char *cerr = _parse_write_cond(data_obj, &cond, &use_cond);
    if (cerr) {
        g_free(bytes);
        gchar *msg = g_strdup_printf("Invalid parameters: %s", cerr);
        en_MCP_DISPATCH_RESULT rc = _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS,
                                                  out_response);
        g_free(msg);
        return rc;
    }
    st_DBGAPI_MEM_PARAM param = {
        .addr = (uint16_t)addr,
        .len  = (uint32_t)decoded,
        .buf  = bytes,
        .cond = use_cond ? &cond : NULL,
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_MEM_WRITE_FORCE, &param, NULL);
    g_free(bytes);
    if (!ok && use_cond && cond.precondition_failed
        && s_submit_track.last_status == DBGAPI_SUBMIT_FAILED) {
        return _write_cond_err(req_id, &cond, out_response);
    }
    if (!ok) {
        return _err_response(req_id, "MEM_WRITE_FORCE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "addr",   (gint64)addr);
    json_object_set_int_member(resp, "length", (gint64)decoded);
    if (use_cond) _write_cond_to_json(resp, &cond);
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
 * Akceptované hodnoty: "cputrack", "iorqlog", "intlog", "hwlog", "marklog".
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
    if (g_strcmp0(s, "marklog") == 0) {
        *out = DBGAPI_TRACE_CHANNEL_MARKLOG;
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
 * @param regs        Jen START cputrack: -1 = podle konfigurace, 0/1 = formát
 *                    nového segmentu (st_DBGAPI_TRACE_PARAM.regs).
 * @return Dispatch výsledek.
 */
static en_MCP_DISPATCH_RESULT _trace_start_stop_common(
        const st_JSONL_MESSAGE *req, char **out_response,
        en_DBGAPI_CMD cmd, const char *state_key, int regs) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    en_DBGAPI_TRACE_CHANNEL ch;
    if (!_trace_get_channel(req, &ch)) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_TRACE_PARAM param = { .channel = ch, .path = NULL,
                                    .regs = regs, .out_result = -1 };
    if (!_submit_dbgapi(cmd, &param, NULL)) {
        if (param.out_result == DBGAPI_TRACE_ERR_REGS_RUNNING) {
            return _err_response(req_id, "cputrack is already recording in the other format; "
                                 "stop it (trace_stop) before changing regs",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
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
 * Parametry: `channel` (cputrack/iorqlog/intlog/hwlog/marklog), `regs`
 * (bool, volitelný, jen cputrack: nový segment ve formátu v3 s registry,
 * false = v2; bez něj volba z konfigurace `[TRACE_CPUTRACK] regs`).
 * Forward na DBGAPI_CMD_TRACE_START. Response: `{"started": true}`.
 * Běží-li už cputrack v jiném formátu, než žádá `regs`, chyba (běžící
 * segment se nepřepíše).
 */
static en_MCP_DISPATCH_RESULT _handle_trace_start(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int regs = -1;
    JsonNode *dn = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *d = (dn && json_node_get_node_type(dn) == JSON_NODE_OBJECT)
                    ? json_node_get_object(dn) : NULL;
    if (d && json_object_has_member(d, "regs") && !json_node_is_null(json_object_get_member(d, "regs"))) {
        int64_t req_id = jsonl_msg_get_req_id(req);
        JsonNode *n = json_object_get_member(d, "regs");
        if (json_node_get_node_type(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_BOOLEAN) {
            return _err_response(req_id, "Invalid parameters: regs must be a boolean",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        en_DBGAPI_TRACE_CHANNEL ch;
        if (_trace_get_channel(req, &ch) && ch != DBGAPI_TRACE_CHANNEL_CPUTRACK) {
            return _err_response(req_id, "Invalid parameters: regs is only for cputrack",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        regs = json_node_get_boolean(n) ? 1 : 0;
    }
    return _trace_start_stop_common(req, out_response,
                                    DBGAPI_CMD_TRACE_START, "started", regs);
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
                                    DBGAPI_CMD_TRACE_STOP, "stopped", -1);
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
                                    .regs = -1, .out_result = -1 };
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
 *   - `channel` (string, povinný) - cputrack/iorqlog/intlog/hwlog/marklog.
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
                                    .regs = -1, .out_result = -1 };
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


/* ===========================================================================
 *  mcp-trace-read: trace_status / trace_read (devdoc/trace/stream-read.md)
 * =========================================================================== */

/** @brief Čitelné kanály v pořadí výpisu trace_status (= en_DBGAPI_TRACE_CHANNEL = en_TRACE_KIND). */
static const char *const s_trace_read_channels[] = {
    "cputrack", "iorqlog", "intlog", "hwlog", "marklog", "eventlog",
    "audio_psg_writes", "audio_levels", "audio_frames", "ramlog", "bplog"
};

G_STATIC_ASSERT(G_N_ELEMENTS(s_trace_read_channels) == TRACE_KIND_COUNT);
G_STATIC_ASSERT((int)DBGAPI_TRACE_CHANNEL_EVENTLOG == (int)TRACE_KIND_EVENTLOG);
G_STATIC_ASSERT(DBGAPI_EVENT_TEXT_MAX == TRACE_RESOLVE_TEXT_SIZE);
G_STATIC_ASSERT(DBGAPI_MARKER_NAME_MAX == TRACE_RESOLVE_NAME_SIZE);

/**
 * @brief Kanál pro čtení (trace_status / trace_read): Trace Suite, eventlog
 *        a proudy paměti zachycení zvuku (audio_*).
 *
 * @param s    Jméno kanálu.
 * @param out  Výstup (jen při TRUE).
 * @return TRUE = čitelný kanál.
 */
static gboolean _trace_read_parse_channel(const char *s, en_TRACE_KIND *out) {
    for (int i = 0; i < (int)G_N_ELEMENTS(s_trace_read_channels); i++) {
        if (g_strcmp0(s, s_trace_read_channels[i]) == 0) {
            *out = (en_TRACE_KIND)i;
            return TRUE;
        }
    }
    return FALSE;
}

/** @brief Kanál je proud paměti zachycení zvuku (ne dbgapi)? */
static bool _trace_kind_is_audio(en_TRACE_KIND k) {
    return k == TRACE_KIND_AUDIO_PSG_WRITES || k == TRACE_KIND_AUDIO_LEVELS
           || k == TRACE_KIND_AUDIO_FRAMES;
}

/**
 * @brief Verze formátu záznamu kanálu (cputrack 2 nebo 3, ostatní 1).
 *
 * @param k            Kanál.
 * @param record_size  Velikost záznamu segmentu (cputrack: 32 B = v3 s registry).
 */
static unsigned _trace_format_version(en_TRACE_KIND k, size_t record_size) {
    /* cputrack v2 (bajt 11, záznam přijetí přerušení), v3 (registry,
     * synchronizační záznamy), ostatní v1 */
    if (k != TRACE_KIND_CPUTRACK) return 1u;
    return (record_size == CPUTRACK_RECORD_SIZE_REGS) ? CPUTRACK_RECORD_VERSION_REGS : CPUTRACK_RECORD_VERSION;
}


/**
 * @brief Počáteční registry segmentu cputrack jako JSON objekt.
 *
 * Klíče a celočíselné hodnoty jako `initial_regs` v manifestu `<name>.json`.
 *
 * @param r  Stav CPU při startu segmentu.
 * @param q  Registr Q při startu.
 * @return Nový objekt (vlastní volající).
 */
static JsonObject *_trace_initial_regs_json(const st_CPUTRACK_REGS *r, uint8_t q) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "AF", r->af);
    json_object_set_int_member(o, "BC", r->bc);
    json_object_set_int_member(o, "DE", r->de);
    json_object_set_int_member(o, "HL", r->hl);
    json_object_set_int_member(o, "AF_alt", r->af2);
    json_object_set_int_member(o, "BC_alt", r->bc2);
    json_object_set_int_member(o, "DE_alt", r->de2);
    json_object_set_int_member(o, "HL_alt", r->hl2);
    json_object_set_int_member(o, "IX", r->ix);
    json_object_set_int_member(o, "IY", r->iy);
    json_object_set_int_member(o, "SP", r->sp);
    json_object_set_int_member(o, "PC", r->pc);
    json_object_set_int_member(o, "WZ", r->wz);
    json_object_set_int_member(o, "IR", ((unsigned)r->i << 8) | r->r);
    json_object_set_int_member(o, "I", r->i);
    json_object_set_int_member(o, "R", r->r);
    json_object_set_int_member(o, "IM", (r->flags & CPUTRACK_REGS_F_IM_MASK) >> CPUTRACK_REGS_F_IM_SHIFT);
    json_object_set_int_member(o, "IFF1", (r->flags & CPUTRACK_REGS_F_IFF1) ? 1 : 0);
    json_object_set_int_member(o, "IFF2", (r->flags & CPUTRACK_REGS_F_IFF2) ? 1 : 0);
    json_object_set_int_member(o, "Q", q);
    json_object_set_int_member(o, "halted", (r->flags & CPUTRACK_REGS_F_HALTED) ? 1 : 0);
    return o;
}

/**
 * @brief Pořídit snímek kanálu přes dbgapi (emu vlákno).
 *
 * @param p  Parametr (channel, from_seq, count, max_copy_bytes, want_time
 *           vyplní volající); výstup p->out uvolní tlog_read_snapshot_clear().
 * @return true OK.
 */
static bool _trace_snapshot(st_DBGAPI_TRACE_SNAPSHOT_PARAM *p) {
    memset(&p->out, 0, sizeof(p->out));
    if (!_submit_dbgapi(DBGAPI_CMD_TRACE_SNAPSHOT, p, NULL) || !p->out_ok) {
        tlog_read_snapshot_clear(&p->out);
        return false;
    }
    return true;
}

/**
 * @brief Pořídit snímek kanálu: Trace Suite a eventlog přes dbgapi, audio_*
 *        přímo z paměti zachycení (audio_capture_stream_copy() pod zámkem
 *        modulu, bez čekání na emu vlákno).
 *
 * U audio_* má snímek tvar ringu: bez chunků, ram_first_seq = first_seq,
 * kopie v ram_copy (vlastnictví přejde na snímek).
 *
 * @param kind      Kanál.
 * @param p         Parametr (from_seq, count, max_copy_bytes, want_time vyplní
 *                  volající); výstup p->out uvolní tlog_read_snapshot_clear().
 * @param origin    Výstup (smí být NULL): u audio_* absolutní čas začátku
 *                  zachycení [takty GDG], jinak 0.
 * @return true OK.
 */
static bool _trace_snapshot_kind(en_TRACE_KIND kind, st_DBGAPI_TRACE_SNAPSHOT_PARAM *p,
                                 uint64_t *origin) {
    if (origin) *origin = 0;
    if (kind == TRACE_KIND_RAMLOG) {
        p->channel = DBGAPI_TRACE_CHANNEL_RAMLOG;
        return _trace_snapshot(p);
    }
    if (kind == TRACE_KIND_BPLOG) {
        p->channel = DBGAPI_TRACE_CHANNEL_BPLOG;
        return _trace_snapshot(p);
    }
    if (!_trace_kind_is_audio(kind)) {
        p->channel = (en_DBGAPI_TRACE_CHANNEL)kind;
        return _trace_snapshot(p);
    }
    memset(&p->out, 0, sizeof(p->out));
    en_AUDIO_CAPTURE_STREAM st = (kind == TRACE_KIND_AUDIO_PSG_WRITES) ? AUDIO_CAPTURE_STREAM_PSG_WRITES
                               : (kind == TRACE_KIND_AUDIO_LEVELS) ? AUDIO_CAPTURE_STREAM_LEVELS
                               : AUDIO_CAPTURE_STREAM_FRAMES;
    st_AUDIO_CAPTURE_STREAM_COPY c;
    bool ok = audio_capture_stream_copy(st, p->from_seq, p->count, p->max_copy_bytes, &c);
    st_TLOG_READ_SNAPSHOT *s = &p->out;
    s->segment = c.segment;
    s->live = c.live ? 1 : 0;
    s->record_size = c.present ? c.record_size : 0;
    s->first_seq = c.first_seq;
    s->next_seq = c.next_seq;
    s->ram_first_seq = c.first_seq;
    s->ram_copy = c.data;
    s->ram_copy_seq = c.data_seq;
    s->ram_copy_count = c.data_count;
    c.data = NULL;
    p->out_ok = ok;
    p->out_pc_range_full = true;
    if (origin) *origin = c.origin_ticks;
    if (!ok) tlog_read_snapshot_clear(&p->out);
    return ok;
}

/** @brief Má kanál podle snímku časový sloupec? */
static bool _trace_has_time(en_TRACE_KIND k,
                            const st_DBGAPI_TRACE_SNAPSHOT_PARAM *p) {
    if (k != TRACE_KIND_CPUTRACK) return true;
    return p->out_pc_range_full;
}

/** @brief Vyplnit stav kanálu do objektu (společné pro status i read). */
static void _trace_fill_state(JsonObject *o, en_TRACE_KIND k,
                              const st_DBGAPI_TRACE_SNAPSHOT_PARAM *p) {
    const st_TLOG_READ_SNAPSHOT *s = &p->out;
    json_object_set_string_member(o, "channel", s_trace_read_channels[k]);
    json_object_set_int_member(o, "segment", (gint64)s->segment);
    json_object_set_boolean_member(o, "live", s->live ? TRUE : FALSE);
    json_object_set_boolean_member(o, "truncated", s->truncated ? TRUE : FALSE);
    json_object_set_int_member(o, "first_seq", (gint64)s->first_seq);
    json_object_set_int_member(o, "next_seq", (gint64)s->next_seq);
    /* Trace Suite nic nezahazuje; ring Event Vieweru přepsal first_seq záznamů. */
    json_object_set_int_member(o, "lost", (gint64)s->first_seq);
}

/**
 * @brief `trace_status` handler - stav kanálů Trace Suite pro čtení.
 *
 * Parametry: `channel` (string, volitelný; bez něj všechny kanály).
 * Response: `{"channels": [{channel, segment, live, truncated, first_seq,
 * next_seq, lost, record_size, format_version, chunks, bytes_on_disk,
 * dir, name, has_time}]}`; u cputrack navíc `has_regs` (segment v3)
 * a `initial_regs` (počáteční stav CPU posledního segmentu jako
 * v manifestu, null = ještě žádný segment).
 */
static en_MCP_DISPATCH_RESULT _handle_trace_status(const st_JSONL_MESSAGE *req,
                                                   char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data_obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                           ? json_node_get_object(data_node) : NULL;
    int only = -1;
    if (data_obj && json_object_has_member(data_obj, "channel")
        && !json_node_is_null(json_object_get_member(data_obj, "channel"))) {
        en_TRACE_KIND ch;
        if (!_obj_is_string(data_obj, "channel")
            || !_trace_read_parse_channel(json_object_get_string_member(data_obj, "channel"), &ch)) {
            return _err_response(req_id, "Invalid parameters: unknown channel",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        only = (int)ch;
    }
    JsonArray *arr = json_array_new();
    for (int i = 0; i < (int)G_N_ELEMENTS(s_trace_read_channels); i++) {
        if (only >= 0 && i != only) continue;
        st_DBGAPI_TRACE_SNAPSHOT_PARAM p;
        memset(&p, 0, sizeof(p));
        en_TRACE_KIND kind = (en_TRACE_KIND)i;
        if (!_trace_snapshot_kind(kind, &p, NULL)) {
            json_array_unref(arr);
            return _err_response(req_id, "trace_status failed",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        JsonObject *o = json_object_new();
        _trace_fill_state(o, kind, &p);
        uint64_t bytes = 0;
        guint chunks = p.out.chunks ? p.out.chunks->len : 0;
        for (guint k = 0; k < chunks; k++) {
            bytes += g_array_index(p.out.chunks, st_TLOG_CHUNK_META, k).bytes;
        }
        json_object_set_int_member(o, "record_size", (gint64)p.out.record_size);
        json_object_set_int_member(o, "format_version", (gint64)_trace_format_version(kind, p.out.record_size));
        json_object_set_int_member(o, "chunks", (gint64)chunks);
        json_object_set_int_member(o, "bytes_on_disk", (gint64)bytes);
        if (p.out.dir) json_object_set_string_member(o, "dir", p.out.dir);
        else json_object_set_null_member(o, "dir");
        if (p.out.name) json_object_set_string_member(o, "name", p.out.name);
        else json_object_set_null_member(o, "name");
        json_object_set_boolean_member(o, "has_time", _trace_has_time(kind, &p) ? TRUE : FALSE);
        if (kind == TRACE_KIND_CPUTRACK) {
            json_object_set_boolean_member(o, "has_regs",
                                           p.out.record_size == CPUTRACK_RECORD_SIZE_REGS ? TRUE : FALSE);
            if (p.out_has_initial_regs) {
                json_object_set_object_member(o, "initial_regs",
                                              _trace_initial_regs_json(&p.out_initial_regs, p.out_initial_q));
            } else {
                json_object_set_null_member(o, "initial_regs");
            }
        }
        json_array_add_object_element(arr, o);
        tlog_read_snapshot_clear(&p.out);
    }
    JsonObject *resp = json_object_new();
    json_object_set_array_member(resp, "channels", arr);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief Resolver symbolů a jmen značek pro trace_reader_page (přes dbgapi).
 */
static bool _trace_resolve_cb(void *user, st_TRACE_RESOLVE_REQ *rr) {
    (void)user;
    st_DBGAPI_SYMBOL_HIT *hits = rr->n_addrs ? g_new0(st_DBGAPI_SYMBOL_HIT, rr->n_addrs) : NULL;
    st_DBGAPI_TRACE_RESOLVE_PARAM p = {
        .addrs = rr->addrs, .n_addrs = rr->n_addrs, .out_syms = hits,
        .marker_ids = rr->markers, .n_markers = rr->n_markers,
        .out_markers = (char (*)[DBGAPI_MARKER_NAME_MAX])rr->marker_names,
        .events = rr->events, .n_events = rr->n_events,
        .out_event_texts = (char (*)[DBGAPI_EVENT_TEXT_MAX])rr->event_texts
    };
    bool ok = _submit_dbgapi(DBGAPI_CMD_TRACE_RESOLVE, &p, NULL);
    if (ok) {
        for (size_t i = 0; i < rr->n_addrs; i++) {
            g_strlcpy(rr->syms + i * TRACE_RESOLVE_NAME_SIZE, hits[i].name, TRACE_RESOLVE_NAME_SIZE);
            rr->sym_offs[i] = hits[i].offset;
        }
    }
    g_free(hits);
    return ok;
}

/**
 * @brief Hodnota registru ve filtru trace_read: číslo 0..65535 nebo řetězec
 *        ("0xC000" hex, "49152" desítkově).
 *
 * @param n    Uzel hodnoty.
 * @param out  Výstup (jen při true).
 * @return true = platná hodnota.
 */
static bool _trace_parse_reg_value(JsonNode *n, unsigned *out) {
    if (!n || json_node_get_node_type(n) != JSON_NODE_VALUE) return false;
    GType t = json_node_get_value_type(n);
    gint64 v;
    if (t == G_TYPE_INT64) {
        v = json_node_get_int(n);
    } else if (t == G_TYPE_STRING) {
        const char *sv = json_node_get_string(n);
        char *end = NULL;
        if (!sv || !sv[0]) return false;
        guint64 u = g_ascii_strtoull(sv, &end, 0);
        if (!end || *end || u > 0xFFFFu) return false;
        v = (gint64)u;
    } else {
        return false;
    }
    if (v < 0 || v > 0xFFFF) return false;
    *out = (unsigned)v;
    return true;
}

/**
 * @brief Naparsovat objekt `filter` trace_read.
 *
 * @return NULL OK, jinak statická chybová zpráva.
 */
static const char *_trace_parse_filter(JsonObject *f, en_TRACE_KIND kind, st_TRACE_FILTER *out) {
    trace_filter_init(out);
    if (!f) return NULL;
    if (json_object_has_member(f, "pc_from") || json_object_has_member(f, "pc_to")) {
        if (kind != TRACE_KIND_CPUTRACK && kind != TRACE_KIND_RAMLOG && kind != TRACE_KIND_BPLOG) {
            return "filter pc_from/pc_to is only for cputrack, ramlog and bplog";
        }
        out->has_pc = true;
        out->pc_from = (uint16_t)_obj_int_or(f, "pc_from", 0);
        out->pc_to = (uint16_t)_obj_int_or(f, "pc_to", 0xFFFF);
    }
    if (json_object_has_member(f, "port")) {
        if (kind != TRACE_KIND_IORQLOG) return "filter port is only for iorqlog";
        out->has_port = true;
        out->port = (uint16_t)_obj_int_or(f, "port", 0);
    }
    if (json_object_has_member(f, "dir")) {
        if (kind != TRACE_KIND_IORQLOG || !_obj_is_string(f, "dir")) return "filter dir must be in or out (iorqlog)";
        const char *d = json_object_get_string_member(f, "dir");
        if (g_strcmp0(d, "in") == 0) out->dir = 0;
        else if (g_strcmp0(d, "out") == 0) out->dir = 1;
        else return "filter dir must be in or out (iorqlog)";
    }
    if (json_object_has_member(f, "type")) {
        if (kind != TRACE_KIND_IORQLOG || !_obj_is_string(f, "type")) return "filter type must be iorq, mreq_mapped, iorq_unconnected or iorq_conflict (iorqlog)";
        const char *t = json_object_get_string_member(f, "type");
        if (g_strcmp0(t, "iorq") == 0) out->type = 0;
        else if (g_strcmp0(t, "mreq_mapped") == 0) out->type = 1;
        else if (g_strcmp0(t, "iorq_unconnected") == 0) out->type = 2;
        else if (g_strcmp0(t, "iorq_conflict") == 0) out->type = 3;
        else return "filter type must be iorq, mreq_mapped, iorq_unconnected or iorq_conflict (iorqlog)";
    }
    if (json_object_has_member(f, "chip")) {
        if (kind != TRACE_KIND_HWLOG) return "filter chip is only for hwlog";
        out->chip = (int)_obj_int_or(f, "chip", -1);
    }
    if (json_object_has_member(f, "sub")) {
        if (kind != TRACE_KIND_HWLOG) return "filter sub is only for hwlog";
        out->sub = (int)_obj_int_or(f, "sub", -1);
    }
    if (json_object_has_member(f, "marker")) {
        if (kind != TRACE_KIND_MARKLOG) return "filter marker is only for marklog";
        out->marker = (int)_obj_int_or(f, "marker", -1);
    }
    if (json_object_has_member(f, "addr_from") || json_object_has_member(f, "addr_to")) {
        if (kind != TRACE_KIND_RAMLOG) return "filter addr_from/addr_to is only for ramlog";
        out->has_addr = true;
        out->addr_from = (uint16_t)_obj_int_or(f, "addr_from", 0);
        out->addr_to = (uint16_t)_obj_int_or(f, "addr_to", 0xFFFF);
    }
    if (json_object_has_member(f, "space")) {
        int sp = (kind == TRACE_KIND_RAMLOG && _obj_is_string(f, "space"))
                 ? ramlog_space_from_name(json_object_get_string_member(f, "space")) : -1;
        if (sp < 0) return "filter space must be ram, vram, pcg, memext or rom (ramlog)";
        out->space = sp;
    }
    if (json_object_has_member(f, "src")) {
        int sr = -1;
        if (kind == TRACE_KIND_RAMLOG && _obj_is_string(f, "src")) {
            const char *v = json_object_get_string_member(f, "src");
            for (int i = 0; i < RAMLOG_SRC_COUNT; i++) if (g_strcmp0(v, ramlog_src_name((unsigned)i)) == 0) sr = i;
        }
        if (sr < 0) return "filter src must be insn, int_ack, nmi_ack, debugger, poke, freeze, cmt_hack or load (ramlog)";
        out->src = sr;
    }
    if (json_object_has_member(f, "all")) {
        if (kind != TRACE_KIND_RAMLOG) return "filter all is only for ramlog";
        out->all = json_object_get_boolean_member(f, "all");
    }
    if (json_object_has_member(f, "bp")) {
        /* bplog: id breakpointu (číslo), nebo přesné jméno (řetězec). */
        if (kind != TRACE_KIND_BPLOG) return "filter bp is only for bplog";
        if (_obj_is_string(f, "bp")) {
            const char *nm = json_object_get_string_member(f, "bp");
            if (!nm || !nm[0] || strlen(nm) >= sizeof(out->bp_name)) {
                return "filter bp must be a breakpoint id or a name of 1-31 characters (bplog)";
            }
            g_strlcpy(out->bp_name, nm, sizeof(out->bp_name));
        } else {
            JsonNode *bn = json_object_get_member(f, "bp");
            if (!JSON_NODE_HOLDS_VALUE(bn) || json_node_get_value_type(bn) != G_TYPE_INT64
                || json_node_get_int(bn) < 0 || json_node_get_int(bn) > G_MAXINT32) {
                return "filter bp must be a breakpoint id or a name of 1-31 characters (bplog)";
            }
            out->bp = (int)json_node_get_int(bn);
        }
    }
    if (json_object_has_member(f, "kind")) {
        int k = (kind == TRACE_KIND_BPLOG && _obj_is_string(f, "kind"))
                ? bplog_kind_from_name(json_object_get_string_member(f, "kind")) : -1;
        if (k < 0) return "filter kind must be log or stop (bplog)";
        out->bplog_kind = k;
    }
    if (json_object_has_member(f, "channels")) {
        if (kind != TRACE_KIND_AUDIO_LEVELS && kind != TRACE_KIND_AUDIO_PSG_WRITES) {
            return "filter channels is only for audio_levels and audio_psg_writes";
        }
        JsonNode *cn = json_object_get_member(f, "channels");
        if (json_node_get_node_type(cn) != JSON_NODE_ARRAY) return "filter channels must be an array of channel names (ctc0, psg0_0 .. psg1_3)";
        JsonArray *ca = json_node_get_array(cn);
        for (guint i = 0; i < json_array_get_length(ca); i++) {
            JsonNode *e = json_array_get_element(ca, i);
            const char *nm = (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                             ? json_node_get_string(e) : "";
            unsigned k = 0;
            while (k < 9 && g_strcmp0(nm, trace_audio_channel_name(k)) != 0) k++;
            if (k == 9) return "filter channels must be an array of channel names (ctc0, psg0_0 .. psg1_3)";
            out->channels |= 1u << k;
        }
    }
    if (json_object_has_member(f, "reg")) {
        static const char *const regs[4] = { "tone_lo", "tone", "volume", "noise" };
        if (kind != TRACE_KIND_AUDIO_PSG_WRITES || !_obj_is_string(f, "reg")) return "filter reg must be tone_lo, tone, volume or noise (audio_psg_writes)";
        const char *r = json_object_get_string_member(f, "reg");
        out->reg = -1;
        for (int i = 0; i < 4; i++) if (g_strcmp0(r, regs[i]) == 0) out->reg = i;
        if (out->reg < 0) return "filter reg must be tone_lo, tone, volume or noise (audio_psg_writes)";
    }
    if (json_object_has_member(f, "t_from") || json_object_has_member(f, "t_to")) {
        out->has_t = true;
        out->t_from = (uint64_t)_obj_int_or(f, "t_from", 0);
        out->t_to = (uint64_t)_obj_int_or(f, "t_to", G_MAXINT64);
    }
    /* cputrack v3: podmínky na registry ("hl": 49152 | "0xC000" | [lo, hi]). */
    size_t ncols = 0;
    const char *const *cols = trace_columns(TRACE_KIND_CPUTRACK, &ncols);
    for (unsigned i = 0; i < TRACE_CPUTRACK_REG_COUNT; i++) {
        const char *rn = cols[TRACE_COL_CPUTRACK_REG0 + i];
        if (!json_object_has_member(f, rn)) continue;
        if (kind != TRACE_KIND_CPUTRACK) return "register filters (af .. halted) are only for cputrack";
        if (out->n_regs >= G_N_ELEMENTS(out->regs)) return "too many register filters";
        unsigned lo = 0, hi = 0;
        JsonNode *n = json_object_get_member(f, rn);
        if (json_node_get_node_type(n) == JSON_NODE_ARRAY) {
            JsonArray *ra = json_node_get_array(n);
            if (json_array_get_length(ra) != 2
                || !_trace_parse_reg_value(json_array_get_element(ra, 0), &lo)
                || !_trace_parse_reg_value(json_array_get_element(ra, 1), &hi) || lo > hi) {
                return "register filter must be a value (number or \"0x..\" string) or [lo, hi]";
            }
        } else if (_trace_parse_reg_value(n, &lo)) {
            hi = lo;
        } else {
            return "register filter must be a value (number or \"0x..\" string) or [lo, hi]";
        }
        out->regs[out->n_regs].reg = (uint8_t)i;
        out->regs[out->n_regs].lo = (uint16_t)lo;
        out->regs[out->n_regs].hi = (uint16_t)hi;
        out->n_regs++;
    }
    return NULL;
}

/** @brief Chybová odpověď s formátovanou zprávou (zpráva se uvolní). */
static en_MCP_DISPATCH_RESULT _trace_err_owned(int64_t req_id, char *msg,
                                               en_MCP_DISPATCH_RESULT rc,
                                               char **out_response) {
    en_MCP_DISPATCH_RESULT r = _err_response(req_id, msg, rc, out_response);
    g_free(msg);
    return r;
}

/**
 * @brief Výkonná část `trace_read` nad objektem parametrů.
 *
 * Sdílí ji handler `trace_read` a `replay_goto` (převod `seq` záznamu
 * cputrack na čas a registry záznamu pro `trace_check`).
 *
 * @param req_id       ID požadavku (do chybové odpovědi).
 * @param d            Parametry jako `data` požadavku `trace_read`.
 * @param[out] out_data Při úspěchu datový objekt odpovědi (vlastní volající).
 * @param[out] out_response Při chybě chybová odpověď (volající `free()`).
 * @return MCP_DISPATCH_OK (out_data platný), jinak chybový kód (out_response).
 */
static en_MCP_DISPATCH_RESULT _trace_read_exec(int64_t req_id, JsonObject *d,
                                               JsonObject **out_data, char **out_response) {
    *out_data = NULL;
    en_TRACE_KIND ch;
    if (!d || !_obj_is_string(d, "channel")
        || !_trace_read_parse_channel(json_object_get_string_member(d, "channel"), &ch)) {
        return _err_response(req_id, "Invalid parameters: unknown channel",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *chname = s_trace_read_channels[ch];
    en_TRACE_KIND kind = ch;

    st_TRACE_PAGE_REQ pr;
    memset(&pr, 0, sizeof(pr));
    pr.kind = kind;
    const char *fmt = _obj_is_string(d, "format") ? json_object_get_string_member(d, "format") : "rows";
    if (g_strcmp0(fmt, "rows") == 0) pr.raw = false;
    else if (g_strcmp0(fmt, "raw_b64") == 0) pr.raw = true;
    else return _err_response(req_id, "Invalid parameters: format must be rows or raw_b64",
                              MCP_DISPATCH_INVALID_PARAMS, out_response);
    gint64 limit = _obj_int_or(d, "limit", pr.raw ? 0 : TRACE_READ_DEFAULT_LIMIT);
    if (!pr.raw && (limit < 1 || limit > TRACE_READ_MAX_LIMIT)) {
        return _err_response(req_id, "Invalid parameters: limit must be 1..5000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (pr.raw && limit < 0) {
        return _err_response(req_id, "Invalid parameters: limit must be >= 0",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    /* raw: limit = počet záznamů, 0 = co se vejde do 1 MiB */
    pr.limit = (unsigned)((limit > 0 && limit < G_MAXUINT) ? limit : (TRACE_READ_MAX_RAW_BYTES / 12u));
    gint64 max_chars = _obj_int_or(d, "max_chars", TRACE_READ_DEFAULT_MAX_CHARS);
    if (max_chars < 1000 || max_chars > TRACE_READ_MAX_MAX_CHARS) {
        return _err_response(req_id, "Invalid parameters: max_chars must be 1000..45000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    pr.max_chars = (size_t)max_chars;
    gint64 max_scan = _obj_int_or(d, "max_scan", TRACE_READ_DEFAULT_MAX_SCAN);
    if (max_scan < 1) {
        return _err_response(req_id, "Invalid parameters: max_scan must be >= 1",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    pr.max_scan = (uint64_t)max_scan;

    /* Sloupce */
    const char *bad = NULL;
    if (json_object_has_member(d, "columns") && !json_node_is_null(json_object_get_member(d, "columns"))) {
        JsonNode *cn = json_object_get_member(d, "columns");
        if (json_node_get_node_type(cn) != JSON_NODE_ARRAY) {
            return _err_response(req_id, "Invalid parameters: columns must be an array of strings",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonArray *ca = json_node_get_array(cn);
        guint n = json_array_get_length(ca);
        const char **names = g_new0(const char *, n + 1);
        for (guint i = 0; i < n; i++) {
            JsonNode *e = json_array_get_element(ca, i);
            names[i] = (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                       ? json_node_get_string(e) : "";
        }
        pr.col_mask = trace_columns_mask(kind, names, n, &bad);
        if (bad) {
            char *msg = g_strdup_printf("Invalid parameters: unknown column '%s'", bad);
            g_free(names);
            return _trace_err_owned(req_id, msg, MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        g_free(names);
    } else {
        pr.col_mask = trace_columns_mask(kind, NULL, 0, &bad);
    }

    /* Stav registrů cputrack v3: po záznamu (výchozí), nebo před ním. */
    if (_obj_is_string(d, "regs")) {
        const char *rv = json_object_get_string_member(d, "regs");
        if (kind != TRACE_KIND_CPUTRACK) {
            return _err_response(req_id, "Invalid parameters: regs is only for cputrack",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        if (g_strcmp0(rv, "before") == 0) pr.regs_before = true;
        else if (g_strcmp0(rv, "after") != 0) {
            return _err_response(req_id, "Invalid parameters: regs must be after or before",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    } else if (json_object_has_member(d, "regs") && !json_node_is_null(json_object_get_member(d, "regs"))) {
        return _err_response(req_id, "Invalid parameters: regs must be after or before",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Filtr */
    JsonObject *fo = NULL;
    if (json_object_has_member(d, "filter") && !json_node_is_null(json_object_get_member(d, "filter"))) {
        JsonNode *fn = json_object_get_member(d, "filter");
        if (json_node_get_node_type(fn) != JSON_NODE_OBJECT) {
            return _err_response(req_id, "Invalid parameters: filter must be an object",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        fo = json_node_get_object(fn);
    }
    const char *ferr = _trace_parse_filter(fo, kind, &pr.filter);
    if (ferr) {
        return _trace_err_owned(req_id, g_strdup_printf("Invalid parameters: %s", ferr),
                                MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Výchozí pozice: kurzor, nebo from */
    const char *cursor = _obj_is_string(d, "cursor") ? json_object_get_string_member(d, "cursor") : NULL;
    bool have_cursor = cursor && cursor[0];
    unsigned cur_seg = 0;
    uint64_t start = 0;
    bool newest = false;
    if (have_cursor) {
        if (!trace_cursor_decode(cursor, chname, &cur_seg, &start)) {
            return _err_response(req_id, "Invalid parameters: invalid cursor",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    } else {
        const char *from = _obj_is_string(d, "from") ? json_object_get_string_member(d, "from") : "oldest";
        if (g_strcmp0(from, "oldest") == 0) {
            start = 0;
        } else if (g_strcmp0(from, "newest") == 0) {
            newest = true;
        } else if (g_str_has_prefix(from, "seq:")) {
            char *end = NULL;
            start = g_ascii_strtoull(from + 4, &end, 10);
            if (!end || *end || end == from + 4) {
                return _err_response(req_id, "Invalid parameters: from must be oldest, newest or seq:N",
                                     MCP_DISPATCH_INVALID_PARAMS, out_response);
            }
        } else {
            return _err_response(req_id, "Invalid parameters: from must be oldest, newest or seq:N",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }

    st_DBGAPI_TRACE_SNAPSHOT_PARAM p;
    memset(&p, 0, sizeof(p));
    uint64_t audio_origin = 0;
    if (newest) {
        /* Napřed metadata, pak posledních `limit` záznamů. */
        if (!_trace_snapshot_kind(kind, &p, NULL)) {
            return _err_response(req_id, "trace_read failed", MCP_DISPATCH_EMU_ERROR, out_response);
        }
        uint64_t n = p.out.next_seq;
        start = (n > pr.limit) ? n - pr.limit : 0;
        if (start < p.out.first_seq) start = p.out.first_seq;
        tlog_read_snapshot_clear(&p.out);
    }
    p.from_seq = start;
    p.count = pr.raw ? pr.limit : pr.max_scan;
    if (kind == TRACE_KIND_CPUTRACK && !pr.raw && start > 0) {
        /* Stav před prvním řádkem stránky nese předchozí záznam: kopie
         * z RAM musí začít o záznam dřív (čas čtenář dopočítá). */
        p.from_seq = start - 1;
        p.count++;
    }
    p.max_copy_bytes = TRACE_READ_MAX_RAM_COPY;
    p.want_time = (kind == TRACE_KIND_CPUTRACK);
    if (!_trace_snapshot_kind(kind, &p, &audio_origin)) {
        return _err_response(req_id, "trace_read failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }

    if (have_cursor && cur_seg != p.out.segment) {
        char *msg = g_strdup_printf("segment_replaced: cursor segment %u, current segment %u; read again from the start",
                                    cur_seg, p.out.segment);
        tlog_read_snapshot_clear(&p.out);
        return _trace_err_owned(req_id, msg, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (start > p.out.next_seq) {
        tlog_read_snapshot_clear(&p.out);
        return _err_response(req_id, "Invalid parameters: invalid cursor (beyond next_seq)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    JsonObject *resp = json_object_new();
    _trace_fill_state(resp, ch, &p);
    if (start < p.out.first_seq) {
        /* Ring přepsal záznamy [start, first_seq): ne chyba, data od
         * first_seq a v odpovědi gap. */
        JsonObject *gap = json_object_new();
        json_object_set_int_member(gap, "from_seq", (gint64)start);
        json_object_set_int_member(gap, "to_seq", (gint64)p.out.first_seq);
        json_object_set_object_member(resp, "gap", gap);
        start = p.out.first_seq;
    } else {
        json_object_set_null_member(resp, "gap");
    }
    uint64_t next = start;
    if (kind == TRACE_KIND_CPUTRACK && p.out.record_size != 0
        && p.out.record_size != CPUTRACK_RECORD_SIZE_REGS && !pr.raw
        && ((pr.col_mask & TRACE_CPUTRACK_REG_COLS_MASK) || pr.filter.n_regs)) {
        json_object_unref(resp);
        tlog_read_snapshot_clear(&p.out);
        return _err_response(req_id, "Invalid parameters: register columns and filters need a cputrack "
                             "segment recorded with regs (trace_start regs=true, format_version 3)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (p.out.segment == 0 || p.out.record_size == 0) {
        /* Kanál nic nezaznamenal: prázdná stránka. */
        json_object_set_int_member(resp, "scanned", 0);
        json_object_set_int_member(resp, "matched", 0);
        json_object_set_boolean_member(resp, "scan_exhausted", FALSE);
        json_object_set_boolean_member(resp, "page_full", FALSE);
        json_object_set_boolean_member(resp, "end_of_data", TRUE);
        if (!pr.raw) {
            json_object_set_array_member(resp, "columns", json_array_new());
            json_object_set_array_member(resp, "rows", json_array_new());
        }
    } else {
        pr.from_seq = start;
        pr.cputrack_time = (kind == TRACE_KIND_CPUTRACK) && _trace_has_time(kind, &p);
        pr.ram_prefix_t = p.out_ram_prefix_t;
        pr.cpu_divider = p.out_cpu_divider;
        if (_trace_kind_is_audio(kind)) {
            pr.cpu_divider = GDGCLK2CPU_DIVIDER;
            pr.audio_origin = audio_origin;
            pr.gdg_hz = GDGCLK_BASE;
            pr.audio_channels = AUDIO_SRC_CHANNELS_COUNT;
        }
        if (trace_reader_page(&pr, &p.out, _trace_resolve_cb, NULL, resp, &next) != 0) {
            json_object_unref(resp);
            tlog_read_snapshot_clear(&p.out);
            return _err_response(req_id, "trace data unavailable: cannot read a chunk file",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        if (pr.raw) {
            json_object_set_int_member(resp, "format_version", (gint64)_trace_format_version(ch, p.out.record_size));
        }
        /* Kontrola segmentu po čtení disku: nový záznam se stejným jménem
         * mohl chunky přepsat. */
        st_DBGAPI_TRACE_SNAPSHOT_PARAM chk;
        memset(&chk, 0, sizeof(chk));
        if (!_trace_snapshot_kind(kind, &chk, NULL)) {
            json_object_unref(resp);
            tlog_read_snapshot_clear(&p.out);
            return _err_response(req_id, "trace_read failed", MCP_DISPATCH_EMU_ERROR, out_response);
        }
        unsigned seg_now = chk.out.segment;
        tlog_read_snapshot_clear(&chk.out);
        if (seg_now != p.out.segment) {
            char *msg = g_strdup_printf("segment_replaced: segment %u was replaced by %u while reading; read again from the start",
                                        p.out.segment, seg_now);
            json_object_unref(resp);
            tlog_read_snapshot_clear(&p.out);
            return _trace_err_owned(req_id, msg, MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    char *nc = trace_cursor_encode(chname, p.out.segment, next);
    json_object_set_string_member(resp, "next_cursor", nc);
    g_free(nc);
    tlog_read_snapshot_clear(&p.out);
    *out_data = resp;
    return MCP_DISPATCH_OK;
}


/**
 * @brief `trace_read` handler - stránka záznamů kanálu Trace Suite.
 *
 * Parametry a odpověď viz devdoc/trace/stream-read.md (channel, cursor,
 * from, limit, format, columns, filter, max_chars, max_scan). Postup:
 * snímek kanálu v emu vlákně (metadata + kopie RAM), čtení chunků
 * z disku a sestavení stránky v dispatch vlákně (trace_reader_page),
 * symboly a jména značek druhým dotazem v emu vlákně, nakonec kontrola,
 * že se segment mezitím nezměnil (jinak chyba `segment_replaced:`).
 */
static en_MCP_DISPATCH_RESULT _handle_trace_read(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *dn = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *d = ( dn && json_node_get_node_type(dn) == JSON_NODE_OBJECT ) ? json_node_get_object(dn) : NULL;
    JsonObject *data = NULL;
    en_MCP_DISPATCH_RESULT rc = _trace_read_exec(req_id, d, &data, out_response);
    return data ? _ok_response(req_id, data, out_response) : rc;
}


/* ---------------- Video záznam (video-capture Task 15) ---------------- */

/**
 * @brief Převede en_VIDEOREC_STATE (číselně v st_DBGAPI_VIDEOREC_PARAM) na řetězec.
 * @param state Hodnota out_state.
 * @return "idle", "recording", "paused" nebo "unknown".
 */
static const char *_videorec_state_str(int state) {
    switch (state) {
        case 0: return "idle";
        case 1: return "recording";
        case 2: return "paused";
    }
    return "unknown";
}

/**
 * @brief Převede en_VIDEOREC_EVENT (číselně v st_DBGAPI_VIDEOREC_PARAM) na řetězec.
 * @param kind Hodnota out_event_kind.
 * @return "started", "saved", "failed", "retake", "seam" nebo "none".
 */
static const char *_videorec_event_str(int kind) {
    switch (kind) {
        case 1: return "started";
        case 2: return "saved";
        case 3: return "failed";
        case 4: return "retake";
        case 5: return "seam";
    }
    return "none";
}

/**
 * @brief Převede en_VIDEOREC_RETAKE (číselně v st_DBGAPI_VIDEOREC_PARAM) na řetězec.
 * @param mode Hodnota out_retake_mode.
 * @return "off", "discard", "seam" nebo "unknown".
 */
static const char *_videorec_retake_str(int mode) {
    switch (mode) {
        case 0: return "off";
        case 1: return "discard";
        case 2: return "seam";
    }
    return "unknown";
}

/**
 * @brief Převede en_VIDEOREC_TIMEBASE (číselně) na řetězec.
 * @param tb Hodnota out_timebase / out_timebase_effective.
 * @return "emulated" nebo "realtime".
 */
static const char *_videorec_timebase_str(int tb) {
    return (tb == 1) ? "realtime" : "emulated";
}

/**
 * @brief Převede en_VIDEOREC_RT_ACTIVITY (číselně) na řetězec.
 * @param a Hodnota out_rt_activity.
 * @return "off", "live", "frozen", "skipping" nebo "unknown".
 */
static const char *_videorec_rt_activity_str(int a) {
    switch (a) {
        case 0: return "off";
        case 1: return "live";
        case 2: return "frozen";
        case 3: return "skipping";
    }
    return "unknown";
}

/**
 * @brief Nastaví řetězcový člen, prázdný řetězec jako JSON null.
 * @param obj Cílový objekt.
 * @param key Klíč.
 * @param val Hodnota (NULL nebo "" => null).
 */
static void _videorec_set_str_or_null(JsonObject *obj, const char *key,
                                      const char *val) {
    if (val && val[0] != '\0') {
        json_object_set_string_member(obj, key, val);
    } else {
        json_object_set_null_member(obj, key);
    }
}

/**
 * @brief Sestaví JSON objekt souhrnného stavu nahrávání z výstupu DBGAPI_CMD_VIDEOREC.
 *
 * Klíče: `supported`, `state` ("idle"/"recording"/"paused"), `start_pending`,
 * `frames`, `fps`, `duration_s` (frames / fps), `segment`, `segment_open`,
 * `bytes`, `parts`, `retake_mode` ("off"/"discard"/"seam"; běžící session,
 * bez nahrávání nastavení pro příští start), `path` (první AVI part nebo null), `sidecar`
 * (`<path bez .avi>.cuts.json` stejně jako jádro, nebo null), `last_error`
 * (text nebo null), `last_event` (null nebo objekt `seq`, `kind`, `frame`,
 * `path`, `text`), `timebase` a `timebase_effective` ("emulated"/"realtime";
 * požadovaná a skutečná časová základna, Task 18) a `rt_activity`
 * ("off"/"live"/"frozen"/"skipping").
 *
 * @param p Vyplněný parametr (výstupní pole).
 * @return Nový JsonObject (vlastnictví přechází na volajícího).
 */
static JsonObject *_videorec_status_json(const st_DBGAPI_VIDEOREC_PARAM *p) {
    JsonObject *o = json_object_new();
    json_object_set_boolean_member(o, "supported", p->out_supported != 0);
    json_object_set_string_member(o, "state", _videorec_state_str(p->out_state));
    json_object_set_boolean_member(o, "start_pending", p->out_start_pending != 0);
    json_object_set_int_member(o, "frames", (gint64) p->out_frames);
    json_object_set_int_member(o, "fps", (gint64) p->out_fps);
    json_object_set_double_member(o, "duration_s",
        p->out_fps ? (double) p->out_frames / (double) p->out_fps : 0.0);
    json_object_set_int_member(o, "segment", (gint64) p->out_segment);
    json_object_set_boolean_member(o, "segment_open", p->out_segment_open != 0);
    json_object_set_int_member(o, "bytes", (gint64) p->out_bytes);
    json_object_set_int_member(o, "parts", (gint64) p->out_parts);
    json_object_set_string_member(o, "retake_mode", _videorec_retake_str(p->out_retake_mode));
    _videorec_set_str_or_null(o, "path", p->out_path);
    if (p->out_path[0] != '\0') {
        /* Stejné odvození jako jádro (videorec.c vr_do_start). */
        size_t len = strlen(p->out_path);
        char *cuts = (len > 4 && g_ascii_strcasecmp(p->out_path + len - 4, ".avi") == 0)
            ? g_strdup_printf("%.*s.cuts.json", (int) (len - 4), p->out_path)
            : g_strdup_printf("%s.cuts.json", p->out_path);
        json_object_set_string_member(o, "sidecar", cuts);
        g_free(cuts);
    } else {
        json_object_set_null_member(o, "sidecar");
    }
    _videorec_set_str_or_null(o, "last_error", p->out_last_error);
    json_object_set_string_member(o, "timebase", _videorec_timebase_str(p->out_timebase));
    json_object_set_string_member(o, "timebase_effective", _videorec_timebase_str(p->out_timebase_effective));
    json_object_set_string_member(o, "rt_activity", _videorec_rt_activity_str(p->out_rt_activity));
    if (p->out_event_seq != 0) {
        JsonObject *ev = json_object_new();
        json_object_set_int_member(ev, "seq", (gint64) p->out_event_seq);
        json_object_set_string_member(ev, "kind", _videorec_event_str(p->out_event_kind));
        json_object_set_int_member(ev, "frame", (gint64) p->out_event_frame);
        _videorec_set_str_or_null(ev, "path", p->out_event_path);
        json_object_set_string_member(ev, "text", p->out_event_text);
        json_object_set_object_member(o, "last_event", ev);
    } else {
        json_object_set_null_member(o, "last_event");
    }
    return o;
}

/**
 * @brief Vrátí datový objekt requestu, nebo NULL (chybí / není objekt).
 * @param req Request.
 * @return JsonObject vlastněný requestem (neuvolňovat), nebo NULL.
 */
static JsonObject *_videorec_data_obj(const st_JSONL_MESSAGE *req) {
    JsonNode *n = (JsonNode *) jsonl_msg_get_data_node(req);
    if (!n || json_node_get_node_type(n) != JSON_NODE_OBJECT) return NULL;
    return json_node_get_object(n);
}

/**
 * @brief Zjistí typ volitelného členu: chybí/null, nebo hodnota daného GType.
 * @param obj  Datový objekt (smí být NULL).
 * @param key  Klíč.
 * @param type Očekávaný GType hodnoty (G_TYPE_INT64, G_TYPE_STRING, G_TYPE_BOOLEAN).
 * @return 0 = chybí nebo null, 1 = přítomen se správným typem, -1 = špatný typ.
 */
static int _videorec_member(JsonObject *obj, const char *key, GType type) {
    if (!obj || !json_object_has_member(obj, key)) return 0;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || json_node_is_null(n)) return 0;
    if (json_node_get_node_type(n) != JSON_NODE_VALUE) return -1;
    return (json_node_get_value_type(n) == type) ? 1 : -1;
}

/**
 * @brief Společné provedení DBGAPI_CMD_VIDEOREC a sestavení odpovědi.
 *
 * Při chybě operace (out_result != OK) vrátí error response s anglickým
 * textem z jádra (out_error), jinak success se stavem z _videorec_status_json()
 * doplněným o `extra_key` = true (např. "stop_requested") a o specifické klíče
 * podle operace (start: `stop_after_frames`; stop: `start_cancelled`;
 * marker: `label`; pause: `paused_target`).
 *
 * @param req_id       ID requestu.
 * @param p            Vyplněné vstupy (op, path, frames, paused, label).
 * @param extra_key    Klíč potvrzení operace (NULL = žádný, u status).
 * @param out_response Výstup.
 * @return Výsledek dispatch.
 */
static en_MCP_DISPATCH_RESULT _videorec_submit(int64_t req_id,
                                               st_DBGAPI_VIDEOREC_PARAM *p,
                                               const char *extra_key,
                                               char **out_response) {
    p->out_result = DBGAPI_VIDEOREC_RESULT_FAILED;
    p->out_error[0] = '\0';
    bool ok = _submit_dbgapi(DBGAPI_CMD_VIDEOREC, p, NULL);
    if (!ok) {
        const char *msg = (p->out_error[0] != '\0') ? p->out_error
                                                     : "videorec command failed";
        return _err_response(req_id, msg, MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = _videorec_status_json(p);
    if (extra_key) json_object_set_boolean_member(resp, extra_key, TRUE);
    switch (p->op) {
        case DBGAPI_VIDEOREC_OP_START:
            json_object_set_int_member(resp, "stop_after_frames", (gint64) p->frames);
            break;
        case DBGAPI_VIDEOREC_OP_STOP:
            json_object_set_boolean_member(resp, "start_cancelled",
                                           p->out_start_cancelled != 0);
            break;
        case DBGAPI_VIDEOREC_OP_MARKER:
            json_object_set_string_member(resp, "label", p->out_label);
            break;
        case DBGAPI_VIDEOREC_OP_PAUSE:
            if (p->paused < 0) {
                json_object_set_null_member(resp, "paused_target");
            } else {
                json_object_set_boolean_member(resp, "paused_target", p->paused != 0);
            }
            break;
        default:
            break;
    }
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `videorec_start` handler - zahájí video záznam (všechny platformy).
 *
 * Parametry (vše volitelné):
 *   - `path` (string) - cílový .avi; existující soubor se přepíše. Bez path
 *     (nebo "") vygenerované jméno `<platforma>_YYYYMMDD_HHMMSS.avi` ve výchozím
 *     adresáři nahrávek (nikdy nepřepíše existující nahrávku).
 *   - `frames` (int >= 0) - auto-stop po tolika zapsaných snímcích
 *     (mechanismus stop_after_frames jako CLI `--record-frames`, ale bez
 *     ukončení emulátoru); 0 = bez limitu.
 *
 * Soubor se vytvoří synchronně (chyba např. neexistujícího adresáře se vrátí
 * hned); vlastní nahrávání začne na nejbližším konci emulovaného snímku -
 * v pauze emulace tedy až po jejím rozběhnutí (`start_pending` = true).
 *
 * Response: stav (viz _videorec_status_json()) + `start_requested`,
 * `stop_after_frames`. Chyby (anglicky): parametry platformy nedávají celý
 * počet vzorků na snímek, nahrávání už běží, soubor nelze vytvořit; neplatné
 * typy parametrů => INVALID_PARAMS.
 */
static en_MCP_DISPATCH_RESULT _handle_videorec_start(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int has_path = _videorec_member(obj, "path", G_TYPE_STRING);
    int has_frames = _videorec_member(obj, "frames", G_TYPE_INT64);
    if (has_path < 0 || has_frames < 0) {
        return _err_response(req_id,
                             "Invalid parameters: path must be a string, frames an integer",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 frames = has_frames ? json_object_get_int_member(obj, "frames") : 0;
    if (frames < 0) {
        return _err_response(req_id, "Invalid parameters: frames must be >= 0",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_VIDEOREC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_VIDEOREC_OP_START;
    p.path = has_path ? json_object_get_string_member(obj, "path") : NULL;
    if (p.path && p.path[0] == '\0') p.path = NULL;
    p.frames = (uint64_t) frames;
    p.paused = -1;
    return _videorec_submit(req_id, &p, "start_requested", out_response);
}

/**
 * @brief `videorec_stop` handler - ukončí video záznam.
 *
 * Bez parametrů. Stop se provede na nejbližším konci snímku, v pauze emulace
 * hned v paused smyčce; čeká-li nezpracovaný start, zruší ho (soubor se
 * smaže, `start_cancelled` = true). Dokončení zápisu (AVI index, sidecar)
 * běží asynchronně ve writer vlákně - hotovou nahrávku ohlásí událost
 * `saved` (nebo `failed`) v `last_event` (viz `videorec_status`).
 *
 * Response: stav + `stop_requested`, `start_cancelled`. Bez nahrávání chyba
 * "Video recording is not running".
 */
static en_MCP_DISPATCH_RESULT _handle_videorec_stop(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    st_DBGAPI_VIDEOREC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_VIDEOREC_OP_STOP;
    p.paused = -1;
    return _videorec_submit(jsonl_msg_get_req_id(req), &p, "stop_requested",
                            out_response);
}

/**
 * @brief `videorec_pause` handler - record-pause (emulace běží dál, snímky se nezapisují).
 *
 * Parametr `paused` (bool, volitelný): true = pauza, false = pokračovat,
 * chybí/null = přepnout. Explicitní hodnota je idempotentní
 * (videorec_request_pause_set()). Provede se na nejbližším konci snímku;
 * pauza uzavře segment, pokračování otevře nový.
 *
 * Response: stav + `pause_requested`, `paused_target` (bool nebo null u
 * přepnutí). Bez nahrávání chyba "Video recording is not running".
 */
static en_MCP_DISPATCH_RESULT _handle_videorec_pause(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int has_paused = _videorec_member(obj, "paused", G_TYPE_BOOLEAN);
    if (has_paused < 0) {
        return _err_response(req_id, "Invalid parameters: paused must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_VIDEOREC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_VIDEOREC_OP_PAUSE;
    p.paused = has_paused ? (json_object_get_boolean_member(obj, "paused") ? 1 : 0) : -1;
    return _videorec_submit(req_id, &p, "pause_requested", out_response);
}

/**
 * @brief `videorec_marker` handler - vloží marker (kapitolu) do nahrávky.
 *
 * Parametr `label` (string, volitelný): popisek; bez něj "Marker at frame N".
 * Marker dostane index snímku, který se zapíše jako další (zpracuje se na
 * nejbližším konci snímku) a zapíše se do sidecaru (`markers`).
 *
 * Response: stav + `marker_requested`, `label`. Bez nahrávání (i při ještě
 * nezpracovaném startu) chyba z jádra.
 */
static en_MCP_DISPATCH_RESULT _handle_videorec_marker(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int has_label = _videorec_member(obj, "label", G_TYPE_STRING);
    if (has_label < 0) {
        return _err_response(req_id, "Invalid parameters: label must be a string",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_VIDEOREC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_VIDEOREC_OP_MARKER;
    p.paused = -1;
    p.label = has_label ? json_object_get_string_member(obj, "label") : NULL;
    return _videorec_submit(req_id, &p, "marker_requested", out_response);
}

/**
 * @brief `videorec_status` handler - souhrnný stav video záznamu.
 *
 * Bez parametrů. Response viz _videorec_status_json(). Volá se i bez
 * nahrávání a na nepodporované platformě (`supported` = false).
 */
static en_MCP_DISPATCH_RESULT _handle_videorec_status(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    st_DBGAPI_VIDEOREC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_VIDEOREC_OP_STATUS;
    p.paused = -1;
    return _videorec_submit(jsonl_msg_get_req_id(req), &p, NULL, out_response);
}


/**
 * @brief `videorec_timebase` handler - časová základna nahrávání (Task 18).
 *
 * Parametr `timebase` (string, povinný): "emulated" (jeden emulovaný snímek
 * = jeden video snímek) nebo "realtime" (podle reality: 50 snímků za sekundu
 * reálného času, co šlo na obrazovku, zvuk z výstupu SDL cesty). Platí pro
 * běžící nahrávku (přepnutí do realtime na nejbližším ticku vzorkovače,
 * zpět na nejbližším konci emulovaného snímku; hranice segmentu) i pro
 * příští start (nastavení INI `timebase`). Bez nahrávání není chyba.
 *
 * Response: stav + `timebase_requested`. Neplatná nebo chybějící hodnota
 * => INVALID_PARAMS.
 */
static en_MCP_DISPATCH_RESULT _handle_videorec_timebase(const st_JSONL_MESSAGE *req,
                                                        char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int has_tb = _videorec_member(obj, "timebase", G_TYPE_STRING);
    const char *tb = (has_tb > 0) ? json_object_get_string_member(obj, "timebase") : NULL;
    if (!tb || (strcmp(tb, "emulated") != 0 && strcmp(tb, "realtime") != 0)) {
        return _err_response(req_id,
                             "Invalid parameters: timebase must be \"emulated\" or \"realtime\"",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_VIDEOREC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_VIDEOREC_OP_TIMEBASE;
    p.paused = -1;
    p.timebase = (strcmp(tb, "realtime") == 0) ? 1 : 0;
    return _videorec_submit(req_id, &p, "timebase_requested", out_response);
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
 * @brief Společná část `media_load_mzf` a `media_run_mzf`.
 *
 * Parametry požadavku (právě jeden):
 *   - `path` (string) - filesystem cesta k .mzf
 *   - `bytes_b64` (string) - inline base64 obsah .mzf (dekóduje se do
 *     dočasného souboru, který se po provedení příkazu smaže)
 *
 * @param req          Požadavek MCP.
 * @param cmd          DBGAPI_CMD_MEDIA_LOAD_MZF nebo DBGAPI_CMD_MEDIA_RUN_MZF.
 * @param cmd_name     Jméno příkazu MCP pro obecnou chybovou zprávu.
 * @param out_response Výstup: alokovaná JSON odpověď.
 * @return MCP_DISPATCH_OK; MCP_DISPATCH_INVALID_PARAMS při chybě parametrů;
 *         MCP_DISPATCH_EMU_ERROR, když příkaz v emulátoru selhal. Kód
 *         out_result se mapuje na zprávu: -2 soubor/hlavička, -3 tělo,
 *         -4 soubor kratší než fsize (jen RUN_MZF, stroj se neresetoval).
 *
 * Při úspěchu vrací `{"ok":true, "load_addr", "exec_addr", "size",
 * "result_code"}`, pro RUN_MZF navíc `"reset":true`.
 */
static en_MCP_DISPATCH_RESULT _media_mzf_common(const st_JSONL_MESSAGE *req,
                                                 en_DBGAPI_CMD cmd,
                                                 const char *cmd_name,
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
    bool ok = _submit_dbgapi(cmd, &param, NULL);
    if (tmp_path) {
        g_unlink(tmp_path);
        g_free(tmp_path);
    }
    if (!ok) {
        int rc = param.out_result;
        g_free(path); g_free(b64);
        /* rc: -2 = soubor/hlavička selhala, -3 = tělo selhalo,
         * -4 = soubor kratší než fsize (ověřeno před resetem), -1 = param */
        char *fallback = g_strdup_printf("%s failed", cmd_name);
        const char *msg = (rc == -2) ? "Cannot open MZF file or invalid header"
                        : (rc == -3) ? "MZF body load failed (checksum/IO)"
                        : (rc == -4) ? "MZF file is shorter than the size in its header (machine was not reset)"
                                     : fallback;
        en_MCP_DISPATCH_RESULT res = _err_response(req_id, msg,
                                                   MCP_DISPATCH_EMU_ERROR,
                                                   out_response);
        g_free(fallback);
        return res;
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "load_addr", (gint64)param.out_load_addr);
    json_object_set_int_member(resp, "exec_addr", (gint64)param.out_exec_addr);
    json_object_set_int_member(resp, "size", (gint64)param.out_size);
    json_object_set_int_member(resp, "result_code", param.out_result);
    if (cmd == DBGAPI_CMD_MEDIA_RUN_MZF) {
        json_object_set_boolean_member(resp, "reset", TRUE);
    }
    g_free(path); g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `media_load_mzf` - rychlý CMT-hack load MZF do RAM.
 *
 * Parametry viz _media_mzf_common(). Volá `DBGAPI_CMD_MEDIA_LOAD_MZF`.
 * Handler vykoná plný dvoufázový load (hlavička + post-header mapping +
 * tělo) analogicky bootstrap.c, obchází CMT play emulaci. Stav stroje
 * (registry, periferie, mapování) nemění. Při selhání souboru/hlavičky
 * (out_result=-2) nebo těla (out_result=-3) vrací error, ne falešný ok.
 */
static en_MCP_DISPATCH_RESULT _handle_media_load_mzf(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    return _media_mzf_common(req, DBGAPI_CMD_MEDIA_LOAD_MZF, "media_load_mzf",
                             out_response);
}


/**
 * @brief `media_run_mzf` - reset stroje a spuštění MZF jako CLI `--run-mzf`.
 *
 * Parametry viz _media_mzf_common(). Volá `DBGAPI_CMD_MEDIA_RUN_MZF`:
 * úplný reset a bootstrap (stav jako po zavedení programu z pásky přes
 * ROM), PC = exec adresa. Stav pauzy emulace se nemění. Soubor se ověří
 * před resetem; chybný soubor (out_result -2 nebo -4) stroj nezresetuje.
 */
static en_MCP_DISPATCH_RESULT _handle_media_run_mzf(const st_JSONL_MESSAGE *req,
                                                     char **out_response) {
    return _media_mzf_common(req, DBGAPI_CMD_MEDIA_RUN_MZF, "media_run_mzf",
                             out_response);
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
 *   - `ro` (bool, optional) - R/O mount (informativní, závislé na slotu;
 *     u `qd` nastaví ochranu proti zápisu jako přepínač v menu)
 *
 * Slot `qd`: obraz MZ-1F11; když mechanika není připojená jako MZ-1F11,
 * přepne ji (odpojí zavaděč Unicard) a odpověď nese
 * `"switched_to_mz1f11": true`. Neexistující soubor = chyba beze změny.
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
        .out_qd_switched = 0,
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
            : (rc == -12)
                ? "File not found"
                : (slot == DBGAPI_MEDIA_SLOT_QD && param.out_qd_switched)
                    ? "media_insert failed (Quick Disk drive was switched to MZ-1F11)"
                    : "media_insert failed";
        return _err_response(req_id, msg,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "slot", _media_slot_str(slot));
    json_object_set_int_member(resp, "result_code", rc);
    if (slot == DBGAPI_MEDIA_SLOT_QD) {
        json_object_set_boolean_member(resp, "switched_to_mz1f11",
                                       param.out_qd_switched ? TRUE : FALSE);
    }
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
    /* QDISK tu není: obraz a ochranu proti zápisu nastavuje media_insert
     * se slotem qd (prvky INI mz1f11_* nemění živou mechaniku). */
    /* TRACE_CPUTRACK - range-scope filtr PC (live-apply do g_cputrack_config
     * v dbgapi SETTINGS_SET handleru; hot-path čte nové meze bez restartu). */
    "TRACE_CPUTRACK/pc_range_lo",
    "TRACE_CPUTRACK/pc_range_hi",
    /* DEVBUS - model sdílené datové sběrnice volitelných zařízení (jen
     * MZ-700): politika kolize dvou budičů a model nebuzené sběrnice.
     * Konfigurace mimo snapshot; jádro devbus ji aplikuje hned. */
    "DEVBUS/conflict",
    "DEVBUS/bus_idle",
    "DEVBUS/bus_idle_value",
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
 *        whitelistu (statický seznam, nebo volba zařízení devbus).
 */
static bool _settings_key_is_live(const char *key) {
    if (!key) return false;
    for (size_t i = 0; g_settings_live_keys[i] != NULL; i++) {
        if (strcmp(g_settings_live_keys[i], key) == 0) return true;
    }
    /* Volby zařízení devbus (sekce INI zařízení, bez "connected", ten mění
     * jen periph_*). Registr devbus je po startu neměnný, čtení bez zámku. */
    const char *slash = strchr(key, '/');
    if (slash && slash != key && slash[1]) {
        char *mod = g_strndup(key, (gsize)(slash - key));
        bool dev = devbus_option_key(mod, slash + 1, NULL) >= 0;
        g_free(mod);
        if (dev) return true;
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
 *   {"key": str, "value": str, "type": "unsigned|bool|text|keyword|float",
 *    "live": str}
 *
 * `value` je hodnota prvku INI (konfigurace načtená při startu nebo
 * zapsaná přes settings_set; změny z menu se do ní promítnou až při
 * ukládání INI). `live` je hodnota proměnné, se kterou pracuje
 * zařízení; jen u prvků s přímou číselnou vazbou, jinak chybí.
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
        .out_live   = NULL,
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
        g_free(param.out_value); g_free(param.out_live);
        return _err_response(req_id, msg,
                              MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "key", key);
    json_object_set_string_member(resp, "value",
                                   param.out_value ? param.out_value : "");
    json_object_set_string_member(resp, "type",
                                   _settings_type_str(param.out_type));
    if (param.out_live) {
        json_object_set_string_member(resp, "live", param.out_live);
    }
    g_free(key); g_free(mod); g_free(elm);
    g_free(param.out_value); g_free(param.out_live);
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
 *   + "applies": "now" | "on_connect" u volby zařízení devbus
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
    /* Vlastní FPS zobrazení: rozsah jako posuvník v menu. Mimo rozsah by
     * ho zobrazení při nejbližším přepnutí režimu tiše vrátilo na výchozí
     * hodnotu. */
    if (strcmp(key, "DISPLAY/custom_fps") == 0) {
        char *endp = NULL;
        unsigned long fv = strtoul(value, &endp, 0);
        if (!endp || *endp != '\0' || fv < DISPLAY_CUSTOM_FPS_MIN
            || fv > DISPLAY_CUSTOM_FPS_MAX) {
            g_free(key); g_free(value);
            return _err_response(req_id,
                                  "Value out of range (expected 50..1000)",
                                  MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
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
        .out_live   = NULL,
        .out_type   = DBGAPI_SETTINGS_TYPE_UNKNOWN,
        .out_result = 0,
        .out_applies = NULL,
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
    /* Volba zařízení devbus: kdy se změna projeví ("now" / "on_connect"). */
    if (param.out_applies) {
        json_object_set_string_member(resp, "applies", param.out_applies);
    }
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
 * Volá `DBGAPI_CMD_PERIPH_ATTACH` nebo `_DETACH` podle is_attach; dbgapi
 * periferii připojí nebo odpojí za běhu (jako menu Devices). Volitelné
 * `options.type` vybírá variantu nebo jednotku (st_DBGAPI_PERIPH_PARAM),
 * u detach jednotku fdc / ide8.
 *
 * Response:
 *   {"ok": true, "kind": str, "type": str, "changed": bool,
 *    "requires_restart": false, "result_code": 0}
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
        /* Jiný než statický druh = id zařízení devbus (get_devices); ověří
         * ho emulátor (neznámé id = chyba). */
        if (!kind_str || !kind_str[0]) {
            g_free(kind_str);
            return _err_response(req_id,
                                  "Invalid kind (allowed: memext, fdc, qd, "
                                  "ide8, gal5, or a device id from get_devices)",
                                  MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        kind = DBGAPI_PERIPH_KIND_DEVBUS;
    }
    /* options.type: varianta (memext, qd) nebo jednotka (fdc, ide8). */
    char *option_value = NULL;
    if (json_object_has_member(data_obj, "options")) {
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
        .out_changed          = 0,
        .out_type             = NULL,
        .out_result           = 0,
        .devbus_id            = (kind == DBGAPI_PERIPH_KIND_DEVBUS) ? kind_str : NULL,
    };
    en_DBGAPI_CMD cmd = is_attach
        ? DBGAPI_CMD_PERIPH_ATTACH
        : DBGAPI_CMD_PERIPH_DETACH;
    bool ok = _submit_dbgapi(cmd, &param, NULL);
    int rc = param.out_result;
    if (!ok) {
        const char *msg;
        en_MCP_DISPATCH_RESULT code = MCP_DISPATCH_EMU_ERROR;
        if (kind == DBGAPI_PERIPH_KIND_DEVBUS) {
            switch (rc) {
                case -1:
                    msg = "Invalid kind: unknown peripheral or device id "
                          "(see get_devices)";
                    code = MCP_DISPATCH_INVALID_PARAMS;
                    break;
                case -4:
                    msg = "Device conflicts with a connected device "
                          "(ports, MMIO range or video hook)";
                    break;
                case -5:
                    msg = "Device failed to connect";
                    break;
                case -10:
                    msg = "Expansion devices are not available on this platform";
                    break;
                default:
                    msg = is_attach ? "periph_attach failed" : "periph_detach failed";
                    break;
            }
            g_free(kind_str); g_free(option_value);
            return _err_response(req_id, msg, code, out_response);
        }
        switch (rc) {
            case -4:
                msg = "Invalid options.type (memext: pehu, luftner; "
                      "fdc: fdc0, fdc1; qd: mz1f11, virtual, unicard; "
                      "ide8: master, slave)";
                code = MCP_DISPATCH_INVALID_PARAMS;
                break;
            case -5:
                msg = "Device did not accept the change (IDE8 disk image "
                      "cannot be opened, qd unicard needs a connected Unicard)";
                break;
            case -10:
                msg = "Peripheral not available in this build";
                break;
            default:
                msg = is_attach ? "periph_attach failed" : "periph_detach failed";
                break;
        }
        g_free(kind_str); g_free(option_value);
        return _err_response(req_id, msg, code, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "kind", kind_str);
    json_object_set_string_member(resp, "type",
                                   param.out_type ? param.out_type : "");
    json_object_set_boolean_member(resp, "changed",
                                    param.out_changed ? TRUE : FALSE);
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
 *     (gal5 v emulátoru není, vždy chyba)
 *   - `options` (object, optional) - `type`: memext "pehu" | "luftner"
 *     (výchozí současný typ), fdc "fdc0" | "fdc1", qd "mz1f11" |
 *     "virtual" | "unicard", ide8 "master" | "slave" (výchozí první)
 *
 * Připojí za běhu, `requires_restart` je vždy false.
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
 *   - `options` (object, optional) - `type`: jednotka fdc "fdc0" | "fdc1",
 *     ide8 "master" | "slave" (výchozí první); u memext a qd se ignoruje
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
 *   input_press_key / input_release_key jsou jednorázové příkazy
 *   (DBGAPI_CMD_INPUT_PRESS_KEY / RELEASE_KEY / RELEASE_ALL).
 *
 *   Nástroje s držením (input_send_key, input_send_keys,
 *   input_send_keys_with_delays, input_send_joystick) sestaví sekvenci
 *   událostí (stisk, držení, uvolnění, mezera) a předají ji emu vláknu
 *   příkazem DBGAPI_CMD_HID_SCRIPT_START (hid_script.c). Emu vlákno
 *   provádí stisky a uvolnění samo na hranicích snímků emulace, dispatch
 *   jen čeká na konec (_hid_run_script).
 *
 * Frame timing (mcp-inbox-fixes):
 *   `frames`, `frame_per_key`, `hold_frames` a `gap_frames` jsou snímky
 *   EMULACE (g_gdg.total_elapsed.screens), nezávisle na rychlosti emulace
 *   (normal / MAX SPEED / pod reálnou rychlostí s CDL) a na tom, zda se
 *   snímek vykreslil. Dřívější čekání v dispatch vlákně na čítač
 *   vykreslených snímků (fbsnapshot_screen_id) dávalo při MAX SPEED stovky
 *   snímků emulace místo N (čítač roste nejvýš jednou za 20 ms reálného
 *   času) a na statické obrazovce končilo až safety timeoutem.
 *
 *   Za běhu emulace se nic nepauzuje. Byla-li emulace v pauze, sekvence ji
 *   rozběhne a po poslední události se emu sám pauzne na hranici snímku
 *   (frame-bounded stop jako emu_run).
 *
 *   Hard upper limit pro frames = 600 (= 12 s při 50 Hz) jako safety
 *   proti AI freezi.
 * ============================================================================ */

/** Bezpečnostní limit počtu framů per request (hold). */
#define HID_FRAMES_MAX 600

/** Bezpečnostní limit délky textu / klíče (input_send_keys). */
#define HID_TEXT_MAX_LEN 256

/** Bezpečnostní limit počtu eventů (send_keys_with_delays). */
#define HID_EVENTS_MAX 256

/**
 * Počet snímků bez stisku mezi klávesami input_send_keys. Guest musí
 * uvolnění vidět, jinak dvě stejné klávesy za sebou splynou v jeden
 * stisk. 1 snímek = dřívější chování za běhu emulace (drain fronty dbgapi
 * jednou za snímek mezi release a dalším press).
 */
#define HID_KEYS_GAP_FRAMES 1

/** Perioda dotazování dispatch vlákna na konec sekvence vstupu (us). */
#define HID_SCRIPT_POLL_US 1000

/**
 * Za jak dlouho (ms) bez jediného dokončeného snímku emulace se běžící
 * sekvence vstupu považuje za zaseknutou a zruší se. Nejde o limit délky
 * sekvence: dokud emulace běží (i pomalu), čeká se libovolně dlouho.
 */
#define HID_SCRIPT_STALL_MS 5000

/**
 * Jak dlouho (ms) dispatch po dokončení sekvence spuštěné z pauzy čeká,
 * než se emu sám pauzne (frame-bounded stop ve stejném průchodu smyčkou).
 */
#define HID_SCRIPT_PAUSE_WAIT_MS 2000


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
extern void hid_keymap_suggest(const char *name, char *out, size_t out_size);


/**
 * @brief Sestaví chybovou odpověď "Unknown key" s nápovědou.
 *
 * Zpráva má tvar `Unknown key 'name'. Closest valid names: A, B, C`
 * (pokud nic podobného není, místo návrhů odkaz na
 * emulator://docs/mz800_keyboard). Prefix "Unknown key" zůstává kvůli
 * zpětné kompatibilitě klientů testujících podřetězec.
 *
 * @param[in]  req_id        id requestu
 * @param[in]  key           neznámé jméno klávesy (nesmí být NULL)
 * @param[out] out_response  vlastněná odpověď (caller free)
 * @return výsledek _err_response (MCP_DISPATCH_INVALID_PARAMS)
 */
static en_MCP_DISPATCH_RESULT _unknown_key_response(int64_t req_id,
                                                    const char *key,
                                                    char **out_response) {
    char sugg[160];
    hid_keymap_suggest(key, sugg, sizeof(sugg));
    char *msg;
    if (sugg[0] != '\0') {
        msg = g_strdup_printf("Unknown key '%s'. Closest valid names: %s",
                              key, sugg);
    } else {
        msg = g_strdup_printf("Unknown key '%s'. See emulator://docs/"
                              "mz800_keyboard for valid key names", key);
    }
    en_MCP_DISPATCH_RESULT rc = _err_response(
        req_id, msg, MCP_DISPATCH_INVALID_PARAMS, out_response);
    g_free(msg);
    return rc;
}


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
 * @brief Počká, až emu vlákno samo deterministicky doběhne frame-bounded run.
 *
 * Použití výhradně z `_handle_run` blokující path po submitu
 * `DBGAPI_CMD_RUN_FRAMES`. Tato funkce NEMANIPULUJE pause stavem emulace -
 * emu se pausne SÁM v hot loopu (mzarch.c, frame-bounded check), jakmile
 * dosáhne cílové frame hranice. Dispatch jen čeká na dokončení.
 *
 * Proč se pause stavu nedotýká: dřívější helper (`_dispatch_wait_frames`,
 * odstraněn v mcp-inbox-fixes) při entry v pauze emu krátce unpausnul
 * ("step N frames" sémantika). Pro malá N a pomalé dispatch vlákno hrozil
 * race: emu mohl doběhnout cíl a pausnout se DŘÍV, než dispatch dorazil,
 * a helper by ho pak znovu unpausnul a emu by běžel za cílovou hranici
 * (= ztráta determinismu).
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
 * @brief Výsledek sekvence vstupu pro sestavení odpovědi HID nástroje.
 */
typedef struct st_HID_RUN_RESULT {
    int  events_done; /**< Počet provedených událostí (vstup stisknut, držen a uvolněn; viz st_DBGAPI_HID_SCRIPT.events_done). */
    int  landed;      /**< Kolik provedených událostí se sondou guest skutečně přečetl (naskenoval sloupec klávesy). */
    int  emu_frames;  /**< Snímky emulace od prvního stisku do konce (nebo zrušení) sekvence. */
    bool complete;    /**< true = proběhly všechny události. */
    bool interrupted; /**< true = sekvenci ukončila pauza zvenku (breakpoint, emu_pause, GUI), reset nebo zaseknutí emulace; držený vstup byl uvolněn. */
} st_HID_RUN_RESULT;


#ifndef MZ800EMU_MCP_TEST_BUILD
/**
 * @brief Počká na konec sekvence vstupu běžící v emu vlákně.
 *
 * Dotazuje se na `script->done` s periodou HID_SCRIPT_POLL_US. Sekvenci
 * zruší (DBGAPI_CMD_HID_SCRIPT_CANCEL, uvolní drženou klávesu), pokud
 * emulaci před koncem sekvence pauzne někdo jiný (breakpoint, emu_pause,
 * GUI) nebo pokud HID_SCRIPT_STALL_MS neskončí žádný snímek emulace.
 * Po řádném dokončení sekvence spuštěné z pauzy počká, než se emu sám
 * pauzne, aby klient dostal odpověď až v zastaveném stavu.
 *
 * @param[in] script sekvence úspěšně předaná příkazem
 *                   DBGAPI_CMD_HID_SCRIPT_START
 * @return true = emu vlákno strukturu už nepoužívá (`done` == 1, OUT pole
 *         platná); false = zrušení se nepodařilo odeslat (emu vlákno
 *         končí) a struktura se nesmí uvolnit.
 *
 * @note Volá se z MCP dispatch vlákna, nikdy z emu vlákna.
 */
static bool _hid_wait_script(st_DBGAPI_HID_SCRIPT *script) {
    uint32_t last_screens = (uint32_t)g_gdg.total_elapsed.screens;
    gint64 last_progress_us = g_get_monotonic_time();

    while (!g_atomic_int_get(&script->done)) {
        bool cancel = false;
        if (EMULATOR_TEST_PAUSED) {
            /* Emu nastaví done dřív, než se na konci sekvence sám pauzne;
             * pauza bez done = pauza zvenku. */
            if (g_atomic_int_get(&script->done)) break;
            cancel = true;
        } else {
            uint32_t now_screens = (uint32_t)g_gdg.total_elapsed.screens;
            gint64 now_us = g_get_monotonic_time();
            if (now_screens != last_screens) {
                last_screens = now_screens;
                last_progress_us = now_us;
            } else if (now_us - last_progress_us
                       > (gint64)HID_SCRIPT_STALL_MS * 1000) {
                cancel = true;
            }
        }
        if (cancel) {
            if (!_submit_dbgapi(DBGAPI_CMD_HID_SCRIPT_CANCEL, script, NULL)) {
                return false;
            }
            return true;
        }
        g_usleep(HID_SCRIPT_POLL_US);
    }

    if (script->pause_at_end && !script->cancelled) {
        gint64 deadline_us = g_get_monotonic_time()
                           + (gint64)HID_SCRIPT_PAUSE_WAIT_MS * 1000;
        while (!EMULATOR_TEST_PAUSED && g_get_monotonic_time() < deadline_us) {
            g_usleep(HID_SCRIPT_POLL_US);
        }
    }
    return true;
}
#endif


/**
 * @brief Provede sekvenci vstupu v emu vlákně a počká na její konec.
 *
 * Předá sekvenci příkazem DBGAPI_CMD_HID_SCRIPT_START (emu vlákno provede
 * stisky a uvolnění na hranicích snímků emulace, viz hid_script.h)
 * a blokuje dispatch vlákno do konce sekvence. Za běhu emulace se nic
 * nepauzuje; z pauzy se emulace rozběhne a po sekvenci se sama zastaví.
 *
 * Vlastnictví: funkce přebírá `events` (alokované g_new0) a uvolní je
 * spolu s interní strukturou sekvence. Výjimka: pokud se běžící sekvenci
 * nepodaří zrušit (emu vlákno končí), paměť se záměrně neuvolní, protože
 * ji emu vlákno může ještě používat.
 *
 * @param[in]  events  pole událostí (vlastnictví přechází na funkci)
 * @param[in]  count   počet událostí (>= 1)
 * @param[out] out     výsledek (vyplněn vždy, i při chybě)
 * @return true = sekvence proběhla nebo byla přerušena (viz `out`);
 *         false = emu vlákno sekvenci nepřijalo (už běží jiná sekvence,
 *         neplatná událost, joystick v emulátoru není) nebo submit selhal.
 *
 * @note V testovacím buildu (MZ800EMU_MCP_TEST_BUILD) sekvenci vykoná
 *       synchronně stub dbgapi a čekání odpadá.
 */
static bool _hid_run_script(st_DBGAPI_HID_SCRIPT_EVENT *events, int count,
                            st_HID_RUN_RESULT *out) {
    memset(out, 0, sizeof(*out));
    st_DBGAPI_HID_SCRIPT *script = g_new0(st_DBGAPI_HID_SCRIPT, 1);
    script->events = events;
    script->count = count;

    if (!_submit_dbgapi(DBGAPI_CMD_HID_SCRIPT_START, script, NULL)) {
        g_free(events);
        g_free(script);
        return false;
    }

    bool released = true;
#ifndef MZ800EMU_MCP_TEST_BUILD
    released = _hid_wait_script(script);
#endif
    if (!released) {
        /* Emu vlákno sekvenci možná ještě drží - nesahat, neuvolňovat. */
        out->interrupted = true;
        return true;
    }

    out->events_done = script->events_done;
    out->emu_frames = (int)(script->end_screens - script->start_screens);
    out->complete = !script->cancelled && script->events_done == count;
    out->interrupted = script->cancelled;
    for (int i = 0; i < script->events_done && i < count; i++) {
        if (events[i].probe && events[i].landed) out->landed++;
    }
    g_free(events);
    g_free(script);
    return true;
}


/**
 * @brief Vyplní událost sekvence pro jednu klávesu.
 *
 * @param[out] ev    událost
 * @param[in]  res   rezolvovaná klávesa
 * @param[in]  hold  počet snímků držení (>= 0)
 * @param[in]  gap   počet snímků mezery po uvolnění (>= 0)
 */
static void _hid_key_event(st_DBGAPI_HID_SCRIPT_EVENT *ev,
                           const st_HID_KEYMAP_RESOLVED *res,
                           int hold, int gap) {
    memset(ev, 0, sizeof(*ev));
    ev->type = DBGAPI_HID_EVENT_KEY;
    ev->key.col = res->col;
    ev->key.bit = res->bit;
    ev->key.needs_shift = res->needs_shift;
    ev->hold_frames = hold;
    ev->gap_frames = gap;
    /* Sonda dosednutí (fix 0016): sledujeme hlavní klávesu, SHIFT je
     * pomocný. S nulovým držením guest nemá kdy číst - landed zůstane
     * false. */
    ev->probe = true;
}


/**
 * @brief Zapíše do odpovědi společná pole o průběhu sekvence vstupu.
 *
 * `emu_frames` = skutečně uplynulé snímky emulace od prvního stisku do
 * konce sekvence, `complete` = proběhly všechny události, `interrupted`
 * = sekvenci ukončila pauza zvenku, reset nebo zaseknutí emulace.
 */
static void _hid_result_to_json(JsonObject *resp, const st_HID_RUN_RESULT *r) {
    json_object_set_int_member(resp, "emu_frames", r->emu_frames);
    json_object_set_boolean_member(resp, "complete", r->complete);
    json_object_set_boolean_member(resp, "interrupted", r->interrupted);
}


/** Chybová zpráva, když emu vlákno sekvenci vstupu nepřijme. */
#define HID_SCRIPT_REJECTED_MSG \
    "input sequence not started (another key/joystick sequence is in " \
    "progress, or the input is not available)"


/**
 * @brief `input_send_key` handler - press + hold + release jedné klávesy.
 *
 * Parametry data:
 *   key      (string) - jméno klávesy (RETURN, SHIFT, ARROW_UP, ...) nebo
 *                       "ASCII:<znak>" / single-character literal
 *   frames   (int)    - počet snímků emulace držet (default 3, max 600)
 *
 * Klávesu stiskne a po `frames` snímcích emulace uvolní emu vlákno
 * (sekvence vstupu, _hid_run_script), nezávisle na rychlosti emulace.
 *
 * Response: `{"key": "<resolved-name>", "col": N, "bit": N,
 *            "shift": bool, "frames": N, "sent": true,
 *            "landing_verified": bool, "emu_frames": N,
 *            "complete": bool, "interrupted": bool}`.
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
 * `emu_frames` = skutečně uplynulé snímky emulace mezi stiskem a
 * uvolněním (= frames, pokud držení nepřerušila pauza zvenku).
 *
 * Chyba: 422 (INVALID_PARAMS) pokud key chybí nebo není rezolvovatelná.
 *        500 (EMU_ERROR) pokud emu vlákno sekvenci nepřijme (už běží jiná
 *        sekvence) nebo dbgapi submit selže.
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
        return _unknown_key_response(req_id, key, out_response);
    }
    st_DBGAPI_HID_SCRIPT_EVENT *ev = g_new0(st_DBGAPI_HID_SCRIPT_EVENT, 1);
    _hid_key_event(ev, &res, (int)frames, 0);
    st_HID_RUN_RESULT run;
    if (!_hid_run_script(ev, 1, &run)) {
        return _err_response(req_id, "input_send_key failed: "
                             HID_SCRIPT_REJECTED_MSG,
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
                                   run.landed > 0 ? TRUE : FALSE);
    _hid_result_to_json(resp, &run);
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
 *                            Neznámé jméno = chyba 422 "Unknown key" s návrhy
 *                            PŘED odesláním jakékoli klávesy.
 *   encoding      (string) - "ascii" (default) nebo "key_names"
 *   frame_per_key (int)    - počet snímků emulace na klávesu (default 3)
 *
 * Response: `{"keys_sent": N, "keys_landed": N, "total_frames": N,
 *            "encoding": str, "landing_verified": bool,
 *            "emu_frames": N, "complete": bool, "interrupted": bool}`.
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
 * `total_frames` = naplánované snímky provedených kláves (držení
 * + mezery HID_KEYS_GAP_FRAMES mezi klávesami), při úplném doběhu rovno
 * `emu_frames`.
 *
 * Implementace: parsing sestaví sekvenci událostí (klávesa za klávesou,
 * mezi uvolněním a dalším stiskem HID_KEYS_GAP_FRAMES snímků), kterou
 * provede emu vlákno (_hid_run_script). Nerezolvovatelný ASCII znak se
 * přeskočí.
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

    /* Sekvence má nejvýš HID_TEXT_MAX_LEN kláves (oba encodingy ořezávají
     * na tuto délku). */
    st_DBGAPI_HID_SCRIPT_EVENT *events =
        g_new0(st_DBGAPI_HID_SCRIPT_EVENT, HID_TEXT_MAX_LEN);
    int count = 0;

    if (strcmp(encoding, "ascii") == 0) {
        size_t tlen = strlen(text);
        if (tlen > HID_TEXT_MAX_LEN) tlen = HID_TEXT_MAX_LEN;
        for (size_t i = 0; i < tlen; i++) {
            st_HID_KEYMAP_RESOLVED res;
            if (!hid_keymap_resolve_ascii(text[i], &res)) {
                continue; /* unresolvable char - skip, ne fail */
            }
            _hid_key_event(&events[count++], &res, (int)frame_per_key, 0);
        }
    } else if (strcmp(encoding, "key_names") == 0) {
        /* Parsing JSON array stringu inline - vyhneme se další json-glib
         * Parser instanci, protože text je už uvnitř requestu. */
        JsonParser *parser = json_parser_new();
        GError *err = NULL;
        if (!json_parser_load_from_data(parser, text, -1, &err)) {
            if (err) g_error_free(err);
            g_object_unref(parser);
            g_free(events);
            return _err_response(req_id, "Invalid parameters (text not JSON)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonNode *root = json_parser_get_root(parser);
        if (!root || json_node_get_node_type(root) != JSON_NODE_ARRAY) {
            g_object_unref(parser);
            g_free(events);
            return _err_response(req_id, "Invalid parameters (text not array)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonArray *arr = json_node_get_array(root);
        guint len = json_array_get_length(arr);
        if (len > HID_TEXT_MAX_LEN) len = HID_TEXT_MAX_LEN;
        /* Neznámé jméno = chyba s nápovědou PŘED odesláním jakékoli
         * klávesy (dřív se tiše přeskočilo). Sekvence se odesílá až po
         * průchodu celým polem, takže stačí jeden průchod. */
        for (guint i = 0; i < len; i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (!el || json_node_get_node_type(el) != JSON_NODE_VALUE) {
                continue;
            }
            const char *kname = json_node_get_string(el);
            if (!kname || kname[0] == '\0') continue;
            st_HID_KEYMAP_RESOLVED res;
            if (!hid_keymap_resolve(kname, &res)) {
                en_MCP_DISPATCH_RESULT erc =
                    _unknown_key_response(req_id, kname, out_response);
                g_object_unref(parser);
                g_free(events);
                return erc;
            }
            _hid_key_event(&events[count++], &res, (int)frame_per_key, 0);
        }
        g_object_unref(parser);
    } else {
        g_free(events);
        return _err_response(req_id, "Invalid parameters (bad encoding)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* Mezi klávesami 1 snímek bez stisku (HID_KEYS_GAP_FRAMES), aby guest
     * zaznamenal uvolnění a dvě stejné klávesy za sebou ("LL") nesplynuly
     * v jeden dlouhý stisk. Odpovídá dřívějšímu chování za běhu emulace
     * (release a další press šly přes drain fronty dbgapi, který proběhne
     * jednou za snímek). Za poslední klávesou se nečeká. */
    for (int i = 0; i + 1 < count; i++) {
        events[i].gap_frames = HID_KEYS_GAP_FRAMES;
    }

    st_HID_RUN_RESULT run;
    memset(&run, 0, sizeof(run));
    if (count == 0) {
        g_free(events); /* nic k odeslání (prázdný text / jen neznámé znaky) */
        run.complete = true;
    } else if (!_hid_run_script(events, count, &run)) {
        return _err_response(req_id, "input_send_keys failed: "
                             HID_SCRIPT_REJECTED_MSG,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    int keys_sent = run.events_done;
    int keys_landed = run.landed; /* kolik z keys_sent guest skutečně přečetl (fix 0016) */
    /* Naplánované snímky provedených kláves: držení + mezery (poslední
     * klávesa celé sekvence mezeru nemá). Při úplném doběhu = emu_frames. */
    int gaps = (keys_sent == count) ? keys_sent - 1 : keys_sent;
    if (gaps < 0) gaps = 0;
    gint64 total_frames = (gint64)keys_sent * frame_per_key
                        + (gint64)gaps * HID_KEYS_GAP_FRAMES;

    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "keys_sent", keys_sent);
    json_object_set_int_member(resp, "keys_landed", keys_landed);
    json_object_set_int_member(resp, "total_frames", total_frames);
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
    _hid_result_to_json(resp, &run);
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
        return _unknown_key_response(req_id, key, out_response);
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
        return _unknown_key_response(req_id, key, out_response);
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
 * Parametry: port (0..1), state (0..255 bitmask), frames (default 3,
 * snímky emulace).
 *
 * Nastavení i uvolnění provede emu vlákno na hranicích snímků emulace
 * (sekvence vstupu s jednou událostí, _hid_run_script).
 *
 * Response: `{"port": N, "state": N, "frames": N, "sent": true,
 *            "emu_frames": N, "complete": bool, "interrupted": bool}`.
 *
 * Chyba: 422 (INVALID_PARAMS) neplatný port/state.
 *        500 (EMU_ERROR) emulátor bez joysticku, běžící jiná sekvence
 *        nebo selhání submitu.
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

    st_DBGAPI_HID_SCRIPT_EVENT *ev = g_new0(st_DBGAPI_HID_SCRIPT_EVENT, 1);
    ev->type = DBGAPI_HID_EVENT_JOY;
    ev->joy.port = (int)port;
    ev->joy.mcp_mask = (uint8_t)state;
    ev->hold_frames = (int)frames;
    ev->gap_frames = 0;
    st_HID_RUN_RESULT run;
    if (!_hid_run_script(ev, 1, &run)) {
        return _err_response(req_id, "input_send_joystick failed: "
                             HID_SCRIPT_REJECTED_MSG,
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "port", port);
    json_object_set_int_member(resp, "state", state);
    json_object_set_int_member(resp, "frames", frames);
    json_object_set_boolean_member(resp, "sent", TRUE);
    _hid_result_to_json(resp, &run);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `input_send_keys_with_delays` handler - timing-controlled
 *        sekvence kláves.
 *
 * Parametry: events (JSON array). Každý event je objekt s poli:
 *   key          (string)
 *   hold_frames  (int, default 3) - snímky emulace držení
 *   gap_frames   (int, default 0) - snímky emulace mezi uvolněním
 *                                   a dalším stiskem (u posledního eventu
 *                                   se také odčeká)
 *
 * Celou sekvenci provede emu vlákno na hranicích snímků emulace
 * (_hid_run_script), takže časování je přesné i při MAX SPEED.
 * Event bez klíče `key` (nebo s prázdným) se přeskočí.
 *
 * Response: `{"events_processed": N, "total_frames": N,
 *            "emu_frames": N, "complete": bool, "interrupted": bool}`.
 * `total_frames` = součet hold+gap provedených eventů, `emu_frames` =
 * skutečně uplynulé snímky emulace (při úplném doběhu stejné).
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

    st_DBGAPI_HID_SCRIPT_EVENT *events =
        g_new0(st_DBGAPI_HID_SCRIPT_EVENT, len);
    int count = 0;
    for (guint i = 0; i < len; i++) {
        JsonNode *el = json_array_get_element(arr, i);
        if (!el || json_node_get_node_type(el) != JSON_NODE_OBJECT) {
            continue;
        }
        JsonObject *eobj = json_node_get_object(el);
        const char *key = json_object_has_member(eobj, "key")
            ? json_object_get_string_member(eobj, "key") : NULL;
        if (!key || key[0] == '\0') continue;
        st_HID_KEYMAP_RESOLVED res;
        if (!hid_keymap_resolve(key, &res)) {
            /* Chyba s nápovědou před odesláním čehokoli. */
            g_free(events);
            return _unknown_key_response(req_id, key, out_response);
        }
        gint64 hold = _obj_int_or(eobj, "hold_frames", 3);
        gint64 gap  = _obj_int_or(eobj, "gap_frames", 0);
        if (hold < 0) hold = 0;
        if (gap < 0) gap = 0;
        if (hold > HID_FRAMES_MAX) hold = HID_FRAMES_MAX;
        if (gap > HID_FRAMES_MAX) gap = HID_FRAMES_MAX;
        _hid_key_event(&events[count++], &res, (int)hold, (int)gap);
    }

    st_HID_RUN_RESULT run;
    memset(&run, 0, sizeof(run));
    gint64 total_frames = 0;
    if (count == 0) {
        g_free(events); /* žádný event s klávesou */
        run.complete = true;
    } else {
        /* Součet hold+gap provedených eventů spočítáme před předáním
         * (pole pak vlastní _hid_run_script). */
        gint64 *prefix = g_new0(gint64, count + 1);
        for (int i = 0; i < count; i++) {
            prefix[i + 1] = prefix[i] + events[i].hold_frames
                                      + events[i].gap_frames;
        }
        if (!_hid_run_script(events, count, &run)) {
            g_free(prefix);
            return _err_response(req_id, "send_keys_with_delays failed: "
                                 HID_SCRIPT_REJECTED_MSG,
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        int done = run.events_done;
        if (done < 0) done = 0;
        if (done > count) done = count;
        total_frames = prefix[done];
        g_free(prefix);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, "events_processed", run.events_done);
    json_object_set_int_member(resp, "total_frames", total_frames);
    _hid_result_to_json(resp, &run);
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
        case 3: return "ram";
        case 4: return "vram";
        case 5: return "memext_ram";
        case 6: return "memext_flash";
        default: return "unknown";
    }
}
/**
 * @brief Jména druhů stránek (`kind`) pro `get_memory_map` a `memory_map`
 *        v `state_observe`.
 *
 * Index = hodnota en_MEMMAP_REGION_KIND (hw-generic/memory/memory.h),
 * pořadí musí sedět s enumem. Počet hlídá DBGAPI_OBSERVE_PAGE_KINDS
 * (kontrola v dbgapi.c proti enumu, tady proti tabulce).
 */
static const char *const s_memmap_kind_names[] = {
    "ram", "rom_low", "rom_high", "cgrom", "vram_i", "vram_ii",
    "vram_text", "cgram", "pcg_1", "pcg_2", "pcg_3", "mapped_ports",
    "prohibited", "unmapped",
};
G_STATIC_ASSERT(G_N_ELEMENTS(s_memmap_kind_names) == DBGAPI_OBSERVE_PAGE_KINDS);


/**
 * @brief Vrátí jméno druhu stránky (en_MEMMAP_REGION_KIND).
 * @param k Hodnota druhu.
 * @return Statický řetězec; pro hodnotu mimo enum "unknown".
 */
static const char *_memmap_kind_str(uint8_t k) {
    return (k < G_N_ELEMENTS(s_memmap_kind_names)) ? s_memmap_kind_names[k] : "unknown";
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
        g_free(p.out_live);
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
 * Response: `{"platform": "mz800|...", "mode_note": "...", "slots": [...]}`,
 * slot `{addr_start, addr_end, kind, source, ro_rw, slot_offset}`; `kind`
 * je druh stránky podle aktuálního mapování (jména jako `state_observe`).
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
        json_object_set_string_member(item, "kind", _memmap_kind_str(s->kind));
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
 *     "mzfsize_check" / "custom_pulses"
 *   - `value` (int, povinné kromě "custom_pulses"): pro "speed"
 *     en_CMTSPEED hodnota 1..9 (poměr) nebo 10 (vlastní pulzy uložené
 *     dříve), pro ostatní boolean 0/1.
 *   - `pulses_us` (pole 4 čísel, povinné pro "custom_pulses"): vlastní
 *     délky pulzů v µs (LONG high, LONG low, SHORT high, SHORT low);
 *     výchozí rychlost se přepne na vlastní pulzy.
 *
 * Layout response: {"ok": true, "property": str, "value": int}, pro
 * "custom_pulses" {"ok": true, "property": str, "pulses_us": [4]}.
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
    st_DBGAPI_CMT_SET_PROPERTY_PARAM param;
    memset(&param, 0, sizeof(param));

    if (strcmp(property, "custom_pulses") == 0) {
        if (!_obj_pulses_us(data_obj, param.pulses_us)) {
            g_free(property);
            return _err_response(req_id,
                                 "Missing or invalid field: pulses_us "
                                 "(array of 4 numbers in microseconds)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.property = DBGAPI_CMT_PROP_CUSTOM_PULSES;
        if (!_submit_dbgapi(DBGAPI_CMD_CMT_SET_PROPERTY, &param, NULL)) {
            g_free(property);
            return _err_response(req_id,
                                 "cmt_set_property failed (each pulse length "
                                 "must be > 0 and <= 65535 us)",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        JsonObject *resp = json_object_new();
        json_object_set_boolean_member(resp, "ok", TRUE);
        json_object_set_string_member(resp, "property", property);
        json_object_set_array_member(resp, "pulses_us", _pulses_us_array(param.pulses_us));
        g_free(property);
        return _ok_response(req_id, resp, out_response);
    }

    if (!json_object_has_member(data_obj, "value")) {
        g_free(property);
        return _err_response(req_id, "Missing required field: value",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

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
                             "cpu_boost, mzfsize_check, custom_pulses)",
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
 * @brief Převede stav transportu CMT (en_CMT_STATE jako číslo) na řetězec.
 *
 * Sdílí ho `cmt_open` a `get_periph_cmt`, aby oba hlásily stav stejně.
 *
 * @param state 0 = STOP, 1 = PLAY, 2 = RECORD.
 * @return "stop" / "play" / "record"; neznámá hodnota -> "stop".
 *         Statický řetězec, neuvolňovat.
 */
static const char *_cmt_state_str(unsigned state) {
    if (state == 1) return "play";
    if (state == 2) return "record";
    return "stop";
}

/**
 * @brief `cmt_open` handler - otevření CMT souboru s volitelným play.
 *
 * Forwarduje na DBGAPI_CMD_CMT_OPEN. Vstupní JSON pole:
 *   - `path` (string, povinné): cesta k CMT souboru.
 *   - `play_immediately` (bool, default false): po openu spustit play.
 *
 * Layout response: {"ok": true, "path": str, "playing": bool,
 *                   "state": "stop"|"play"|"record", "paused": bool
 *                   [, "warning": str]}
 *
 * `playing` hlásí skutečný stav transportu po operaci (PLAY bez pauzy),
 * ne požadavek - dříve se vracelo echo `play_immediately`, takže
 * play_immediately nad páskou, která se nerozehrála, hlásil playing: true.
 * `state` / `paused` mají stejný význam jako v `get_periph_cmt`.
 * `warning` je přítomen, jen když play_immediately byl požadován, ale
 * páska po operaci nehraje. Chování openu se nemění.
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
    /* Skutečný stav transportu po operaci (vyplní dbgapi handler). */
    gboolean playing = (param.out_state == 1) && !param.out_paused;

    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_string_member(resp, "path", path);
    json_object_set_boolean_member(resp, "playing", playing);
    json_object_set_string_member(resp, "state",
                                  _cmt_state_str(param.out_state));
    json_object_set_boolean_member(resp, "paused",
                                   param.out_paused ? TRUE : FALSE);
    if (param.play_immediately && !playing) {
        json_object_set_string_member(resp, "warning",
            "play_immediately requested, but the tape is not playing "
            "after open (see state/paused)");
    }
    g_free(path);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `cmt_tape_seek` handler - seek na blok pásky.
 *
 * Forwarduje na DBGAPI_CMD_CMT_TAPE_SEEK. Vyžaduje int `block_id`.
 * Bez naložené pásky, u jednosouborového containeru (SINGLE: .mzf,
 * .wav - nemá cb_open_block, seek nepodporuje ani na blok 0) nebo
 * pro neplatný blok -> success = false.
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
                             "cmt_tape_seek failed (no tape, single-file "
                             "tape without seek support, or bad block)",
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
 * a buď int `speed` (en_CMTSPEED poměr 1..9), nebo `pulses_us` (pole
 * 4 čísel - vlastní délky pulzů v µs; blok pak má speed 10 =
 * CMTSPEED_CUSTOM). Bez pásky, MZ blok mimo rozsah nebo neplatná
 * rychlost/délky -> success = false.
 *
 * Layout response: {"ok": true, "block_id": int, "speed": int}, u
 * vlastních pulzů navíc "pulses_us": [4].
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
    gboolean has_pulses = json_object_has_member(data_obj, "pulses_us");
    if (!json_object_has_member(data_obj, "block_id")
        || (!json_object_has_member(data_obj, "speed") && !has_pulses)) {
        return _err_response(req_id, "Missing required fields: block_id, "
                             "speed or pulses_us", MCP_DISPATCH_INVALID_PARAMS,
                             out_response);
    }

    st_DBGAPI_CMT_TAPE_BLOCK_SPEED_PARAM param;
    memset(&param, 0, sizeof(param));
    param.block_id = (int)_obj_int_or(data_obj, "block_id", 0);
    if (has_pulses) {
        if (!_obj_pulses_us(data_obj, param.pulses_us)) {
            return _err_response(req_id,
                                 "Invalid field: pulses_us (array of 4 "
                                 "numbers in microseconds)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        param.use_pulses = 1;
        param.cmtspeed = CMTSPEED_CUSTOM;
    } else {
        param.cmtspeed = (int)_obj_int_or(data_obj, "speed", 0);
    }
    if (!_submit_dbgapi(DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED, &param, NULL)) {
        return _err_response(req_id,
                             "cmt_tape_block_speed failed (no tape, bad "
                             "block or bad speed / pulse lengths)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "ok", TRUE);
    json_object_set_int_member(resp, "block_id", (gint64)param.block_id);
    json_object_set_int_member(resp, "speed", (gint64)param.cmtspeed);
    if (param.use_pulses) {
        json_object_set_array_member(resp, "pulses_us", _pulses_us_array(param.pulses_us));
    }
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
 *       "playable", "recordable", "block_speed", "pulses_us"?} ... ]}
 *
 * "block_speed" je "none" / "default" / "set" (en_CMTEXT_BLOCK_SPEED);
 * "cmt_speed" platí pro "set" (10 = vlastní délky pulzů). "pulses_us"
 * (jen MZ bloky) jsou efektivní délky pulzů v µs, se kterými se blok
 * přehraje (LONG high, LONG low, SHORT high, SHORT low).
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
        static const char *const block_speed_txt[] = { "none", "default", "set" };
        json_object_set_string_member(item, "block_speed",
            (e->block_speed < 3) ? block_speed_txt[e->block_speed] : "none");
        if (e->has_pulses) {
            json_object_set_array_member(item, "pulses_us", _pulses_us_array(e->pulses_us));
        }
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
 * mz800-knowledge/public/reference/agent/hw/06-ctc-8253.md (CTC0 OUT),
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
 *    "cksw": int, "tempo": int, "tempo_divider": int,
 *    jen MZ-800: "vramctrl": {"wf_mode": str, "wf_mode_code": int,
 *      "wf_planes": int, "rf_planes": int, "rf_search": bool, "vbank": int},
 *    "hwscroll": {"enabled": bool, "sof": int, "ssa": int, "sea": int,
 *      "sw": int} (scroll v jednotkách portu)}
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
    /* MZ-800: řadič VRAM (WF/RF) - objekt jen tam, kde existuje. Kódy 5 a 7
     * emulátor neukládá (GDG při WM2 = 1 ignoruje WM0, báze
     * hw/10a-vram-rf-wf.md), tabulka je pro úplnost mapuje jako 4 a 6. */
    if (param.has_vramctrl) {
        static const char *const wf_mode_names[8] = {
            "single", "exor", "or", "reset", "replace", "replace", "pset", "pset"
        };
        JsonObject *vc = json_object_new();
        json_object_set_string_member(vc,  "wf_mode",      wf_mode_names[param.wf_mode & 0x07u]);
        json_object_set_int_member(vc,     "wf_mode_code", (gint64)param.wf_mode);
        json_object_set_int_member(vc,     "wf_planes",    (gint64)param.wf_plane);
        json_object_set_int_member(vc,     "rf_planes",    (gint64)param.rf_plane);
        json_object_set_boolean_member(vc, "rf_search",    param.rf_search ? TRUE : FALSE);
        json_object_set_int_member(vc,     "vbank",        (gint64)param.wfrf_vbank);
        json_object_set_object_member(resp, "vramctrl", vc);
    }
    /* MZ-800: HW scroll v jednotkách portu. */
    if (param.has_hwscroll) {
        JsonObject *hs = json_object_new();
        json_object_set_boolean_member(hs, "enabled", param.hs_enabled ? TRUE : FALSE);
        json_object_set_int_member(hs,     "sof",     (gint64)param.hs_sof);
        json_object_set_int_member(hs,     "ssa",     (gint64)param.hs_ssa);
        json_object_set_int_member(hs,     "sea",     (gint64)param.hs_sea);
        json_object_set_int_member(hs,     "sw",      (gint64)param.hs_sw);
        json_object_set_object_member(resp, "hwscroll", hs);
    }
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
 *    "image_basename": str, "default_pulses_us": [4],
 *    "custom_pulses_us": [4]}
 *
 * "cmtspeed" 10 = výchozí rychlost jsou vlastní délky pulzů
 * ("custom_pulses_us"). "default_pulses_us" jsou délky pulzů výchozí
 * rychlosti (v µs: LONG high, LONG low, SHORT high, SHORT low), se
 * kterými se přehrají bloky bez vlastní rychlosti.
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
    json_object_set_string_member(resp, "state",
                                  _cmt_state_str((unsigned)param.state));
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
    json_object_set_array_member(resp,   "default_pulses_us", _pulses_us_array(param.default_pulses_us));
    json_object_set_array_member(resp,   "custom_pulses_us",  _pulses_us_array(param.custom_pulses_us));
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
 * @brief Doplní do odpovědi hash obrazu, hash VRAM a pozici paprsku.
 *
 * @param resp Odpověď.
 * @param h Výsledek z emu vlákna (st_DBGAPI_FRAME_HASH_PARAM).
 */
static void _frame_hash_to_json(JsonObject *resp, const st_DBGAPI_FRAME_HASH_PARAM *h) {
    if (h->available) {
        json_object_set_string_member(resp, "image_sha256", h->image_sha256);
    }
    json_object_set_string_member(resp, "vram_sha256", h->vram_sha256);
    json_object_set_int_member(resp, "source_screen_id", (gint64)h->source_screen_id);
    json_object_set_string_member(resp, "image_source",
        (h->source == SCREENSHOT_SRC_GDG_LIVE) ? "gdg_live" : "sdl_snapshot");
    JsonObject *beam = json_object_new();
    json_object_set_int_member(beam, "frame", (gint64)h->frame);
    json_object_set_int_member(beam, "scanline", (gint64)h->scanline);
    json_object_set_int_member(beam, "column", (gint64)h->column);
    json_object_set_object_member(resp, "beam", beam);
    json_object_set_boolean_member(resp, "paused", h->paused ? TRUE : FALSE);
}


/**
 * @brief `frame_hash` handler - SHA-256 obrazu a VRAM bez přenosu dat.
 *
 * Bez parametrů. `image_sha256` je hash obrazu ze stejného zdroje jako
 * screenshot (SDL snímek, jinak živý framebuffer GDG): W x H bajtů indexů
 * palety (INDEX8 & 0Fh, po řádcích), za nimi 16 položek palety po 3 bajtech
 * R, G, B. `vram_sha256` je hash VRAM (state_hash.h: MZ-800 4 plány po
 * 8 KB, MZ-700 a MZ-1500 znaková + atributová VRAM). Obraz závisí i na
 * registrech GDG (paleta, režim, border), proto se hashe vracejí zvlášť.
 * V pauze uprostřed snímku je část obrazu ještě z minulého snímku - hash
 * pak závisí na pozici paprsku (`beam`).
 *
 * Response: `{available, image_sha256 (jen available), vram_sha256,
 * source_screen_id, image_source, width, height, beam {frame, scanline,
 * column}, paused}`.
 */
static en_MCP_DISPATCH_RESULT _handle_frame_hash(const st_JSONL_MESSAGE *req,
                                                 char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_FRAME_HASH_PARAM h;
    memset(&h, 0, sizeof(h));
    if (!_submit_dbgapi(DBGAPI_CMD_FRAME_HASH, &h, NULL)) {
        return _err_response(req_id, "frame_hash failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "available", h.available ? TRUE : FALSE);
    json_object_set_int_member(resp, "width", (gint64)h.width);
    json_object_set_int_member(resp, "height", (gint64)h.height);
    _frame_hash_to_json(resp, &h);
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
 *    "image_sha256", "vram_sha256", "image_source", "beam", "paused"
 *    (viz _handle_frame_hash), "data_b64": "<base64 RGBA bytes>"}
 *   pokud available=false:
 *   {"available": false, "reason": str}
 * Parametr `want_data` (bool, default true): false = bez `data_b64`.
 */
static en_MCP_DISPATCH_RESULT _handle_get_frame_screenshot_raw(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *raw_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *raw_obj = (raw_node && json_node_get_node_type(raw_node) == JSON_NODE_OBJECT)
                          ? json_node_get_object(raw_node) : NULL;
    bool want_data = true;
    if (!_parse_want_data(raw_obj, &want_data)) {
        return _err_response(req_id, "want_data must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
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
    gchar *b64 = want_data ? g_base64_encode(buf, param.buffer_size) : NULL;
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
    _frame_hash_to_json(resp, &param.hash);
    if (want_data) {
        json_object_set_string_member(resp, "data_b64", b64 ? b64 : "");
    }
    g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `get_frame_screenshot` handler - Resource pro
 *        `emulator://frame/screenshot`.
 *
 * PNG screenshot enkódovaný přes stb_image_write.h (viz
 * src/emulator/debugger/png_encode.{c,h}). Handler v emu vlákně
 * zenkóduje obraz podle voleb a vrátí glib-alokovaný PNG stream, který
 * dispatch po base64 enkódování uvolní přes g_free.
 *
 * Parametry (data, všechny volitelné, stejné jako screenshot_save_to_file):
 *   - `area`  (string) - "full" (default) / "canvas"
 *   - `scale` (string|int) - "framebuffer" (default), "native", 1..4
 *   - `crop`  (objekt) - {x, y, width, height} ve výstupních pixelech,
 *             jen se scale "framebuffer" nebo "native"
 *   Bez voleb = celý framebuffer 1:1 (stejný obsah jako screenshot.raw).
 *   Viz _screenshot_opts_from_json(); neplatné volby = INVALID_PARAMS
 *   (dřív se volby tiše ignorovaly).
 *   - `want_data` (bool, default true) - false = bez `data_b64`.
 *   Odpověď nese i `image_sha256` (celý framebuffer ze stejného zdroje,
 *   bez ohledu na oblast a měřítko), `vram_sha256`, `beam`, `paused`
 *   (viz _handle_frame_hash).
 *
 * Layout response (available=true):
 *   {"available": true, "format": "png", "width": N, "height": N,
 *    "byte_size": N, "area": str, "scale": str,
 *    "hires": bool (jen scale "native"), "data_b64": "<base64 PNG stream>"}
 *   pokud available=false:
 *   {"available": false, "reason": str}
 */
static en_MCP_DISPATCH_RESULT _handle_get_frame_screenshot(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);

    /* Oblast, měřítko a ořez (výchozí = celý framebuffer 1:1). */
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data_obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                           ? json_node_get_object(data_node) : NULL;
    st_SCREENSHOT_OPTS opts;
    char opts_err[128];
    if (!_screenshot_opts_from_json(data_obj, &opts, opts_err, sizeof(opts_err))) {
        return _err_response(req_id, opts_err, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool want_data = true;
    if (!_parse_want_data(data_obj, &want_data)) {
        return _err_response(req_id, "want_data must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM param;
    memset(&param, 0, sizeof(param));
    param.opts = opts;
    if (!_submit_dbgapi(DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG, &param, NULL)) {
        return _err_response(req_id, "get_frame_screenshot failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (!param.available && param.bad_opts) {
        /* Neplatné volby (např. ořez mimo obraz) - chyba parametrů. */
        return _err_response(req_id, param.reason, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *resp = json_object_new();
    if (!param.available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp,  "reason",    param.reason);
        /* buffer NULL při available=0 - nic neuvolňujeme. */
        return _ok_response(req_id, resp, out_response);
    }
    /* Handler alokoval PNG stream (glib) - po base64 ho uvolníme. */
    gchar *b64 = want_data ? g_base64_encode(param.buffer, param.buffer_size) : NULL;
    g_free(param.buffer);
    json_object_set_boolean_member(resp, "available", TRUE);
    json_object_set_string_member(resp,  "format",    "png");
    json_object_set_int_member(resp,     "width",     (gint64)param.width);
    json_object_set_int_member(resp,     "height",    (gint64)param.height);
    json_object_set_int_member(resp,     "byte_size", (gint64)param.buffer_size);
    json_object_set_string_member(resp,  "area",  screenshot_area_to_string(opts.area));
    json_object_set_string_member(resp,  "scale", screenshot_scale_to_string(opts.scale_mode, opts.window_scale));
    if (opts.scale_mode == SCREENSHOT_SCALE_NATIVE) {
        json_object_set_boolean_member(resp, "hires", param.hires ? TRUE : FALSE);
    }
    _frame_hash_to_json(resp, &param.hash);
    if (want_data) {
        json_object_set_string_member(resp, "data_b64", b64 ? b64 : "");
    }
    g_free(b64);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief Načte volby screenshotu (area, scale, crop) z parametrů requestu.
 *
 * - `area`  (string): "full" (výchozí) nebo "canvas",
 * - `scale` (string nebo int): "framebuffer" (výchozí), "native", 1..4
 *           (poměr stran okna, 2 = okno na 100 %),
 * - `crop`  (objekt {x, y, width, height}): výřez ve výstupních pixelech,
 *           jen se scale "framebuffer" nebo "native".
 *
 * Chybějící klíč = výchozí hodnota (dřívější chování: celý framebuffer 1:1).
 *
 * @param obj      Parametry requestu (může být NULL = výchozí volby).
 * @param opts     Výstup.
 * @param err      Buffer na anglický popis chyby.
 * @param err_size Velikost `err`.
 * @return true = volby načteny; false = neplatná hodnota (popis v `err`).
 */
static bool _screenshot_opts_from_json(JsonObject *obj, st_SCREENSHOT_OPTS *opts,
                                       char *err, size_t err_size) {
    screenshot_opts_init(opts);
    if (!obj) return true;

    if (json_object_has_member(obj, "area")) {
        char *area = _obj_str_dup(obj, "area");
        bool ok = area && screenshot_area_from_string(area, &opts->area);
        g_free(area);
        if (!ok) {
            g_strlcpy(err, "'area' must be 'full' or 'canvas'", err_size);
            return false;
        }
    }

    if (json_object_has_member(obj, "scale")) {
        JsonNode *n = json_object_get_member(obj, "scale");
        bool ok = false;
        if (n && json_node_get_node_type(n) == JSON_NODE_VALUE) {
            GType t = json_node_get_value_type(n);
            if (t == G_TYPE_STRING) {
                ok = screenshot_scale_from_string(json_node_get_string(n),
                                                  &opts->scale_mode, &opts->window_scale);
            } else if (t == G_TYPE_INT64) {
                gint64 v = json_node_get_int(n);
                if (v >= 1 && v <= SCREENSHOT_WINDOW_SCALE_MAX) {
                    opts->scale_mode = SCREENSHOT_SCALE_WINDOW;
                    opts->window_scale = (unsigned)v;
                    ok = true;
                }
            }
        }
        if (!ok) {
            g_strlcpy(err, "'scale' must be 'framebuffer', 'native' or 1 to 4", err_size);
            return false;
        }
    }

    if (json_object_has_member(obj, "crop")) {
        JsonNode *n = json_object_get_member(obj, "crop");
        if (!n || json_node_get_node_type(n) != JSON_NODE_OBJECT) {
            g_strlcpy(err, "'crop' must be an object {x, y, width, height}", err_size);
            return false;
        }
        JsonObject *c = json_node_get_object(n);
        gint64 x = _obj_int_or(c, "x", -1);
        gint64 y = _obj_int_or(c, "y", -1);
        gint64 w = _obj_int_or(c, "width", -1);
        gint64 h = _obj_int_or(c, "height", -1);
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x > 0xFFFF || y > 0xFFFF || w > 0xFFFF || h > 0xFFFF) {
            g_strlcpy(err, "'crop' needs non-negative x, y and positive width, height", err_size);
            return false;
        }
        opts->crop = true;
        opts->crop_x = (unsigned)x;
        opts->crop_y = (unsigned)y;
        opts->crop_w = (unsigned)w;
        opts->crop_h = (unsigned)h;
    }
    return true;
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
 *   - `area`   (string, volitelné) - "full" (default) / "canvas"
 *   - `scale`  (string|int, volitelné) - "framebuffer" (default), "native",
 *              1..4 (poměr stran okna, 2 = okno na 100 %)
 *   - `crop`   (objekt, volitelné) - {x, y, width, height} ve výstupních
 *              pixelech, jen se scale "framebuffer" nebo "native"
 *   Viz _screenshot_opts_from_json(); neplatné volby = INVALID_PARAMS.
 *
 * Layout response (available=true):
 *   {"available": true, "path": str, "format": "png",
 *    "width": N, "height": N, "byte_size": N,
 *    "area": str, "scale": str, "hires": bool (jen scale "native")}
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

    /* Oblast, měřítko a ořez (výchozí = celý framebuffer 1:1). */
    st_SCREENSHOT_OPTS opts;
    char opts_err[128];
    if (!_screenshot_opts_from_json(data_obj, &opts, opts_err, sizeof(opts_err))) {
        g_free(path);
        return _err_response(req_id, opts_err, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    /* PNG capture - stejná emu-thread cesta jako get_frame_screenshot. */
    st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM param;
    memset(&param, 0, sizeof(param));
    param.opts = opts;
    if (!_submit_dbgapi(DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG, &param, NULL)) {
        g_free(path);
        return _err_response(req_id, "get_frame_screenshot failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (!param.available && param.bad_opts) {
        /* Neplatné volby (např. ořez mimo obraz) - chyba parametrů. */
        g_free(path);
        return _err_response(req_id, param.reason, MCP_DISPATCH_INVALID_PARAMS, out_response);
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
    json_object_set_string_member(resp,  "area",  screenshot_area_to_string(opts.area));
    json_object_set_string_member(resp,  "scale", screenshot_scale_to_string(opts.scale_mode, opts.window_scale));
    if (opts.scale_mode == SCREENSHOT_SCALE_NATIVE) {
        json_object_set_boolean_member(resp, "hires", param.hires ? TRUE : FALSE);
    }
    /* Hash obrazu (celý framebuffer ze stejného zdroje), VRAM, paprsek. */
    _frame_hash_to_json(resp, &param.hash);
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

int mcp_dispatch_stall_limit_ms(int cmd) {
    switch ((en_DBGAPI_CMD)(cmd & DBGAPI_CMD_MASK)) {
        /* Souborové I/O nebo práce s velkými daty v emu vlákně. */
        case DBGAPI_CMD_TRACE_SAVE:
        case DBGAPI_CMD_TRACE_STOP:
        case DBGAPI_CMD_SNAPSHOT_SAVE_FILE:
        case DBGAPI_CMD_SNAPSHOT_LOAD_FILE:
        case DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER:
        case DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER:
        case DBGAPI_CMD_PROFILER_EXPORT:
        case DBGAPI_CMD_CDL_EXPORT:
        case DBGAPI_CMD_VIDEOREC:
        case DBGAPI_CMD_MEDIA_LOAD_MZF:
        case DBGAPI_CMD_MEDIA_RUN_MZF:
        case DBGAPI_CMD_MEDIA_LOAD_BINARY:
        case DBGAPI_CMD_MEDIA_INSERT:
        case DBGAPI_CMD_MEDIA_EJECT:
        case DBGAPI_CMD_CMT_OPEN:
        case DBGAPI_CMD_CMT_RECORD:
        case DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG:
            return MCP_DISPATCH_DBGAPI_STALL_LONG_MS;
        default:
            return MCP_DISPATCH_DBGAPI_STALL_MS;
    }
}


void mcp_dispatch_set_thread_stall_watch(const st_MCP_DISPATCH_STALL_WATCH *watch) {
    s_stall_watch = watch;
}


en_MCP_DISPATCH_RESULT mcp_dispatch_build_stalled_response(int64_t req_id,
                                                           char **out_response) {
    if (!out_response) return MCP_DISPATCH_ALLOC_ERROR;
    char *line = jsonl_build_response(req_id, false, NULL,
                                      MCP_DISPATCH_MSG_STALLED);
    *out_response = line;
    return line ? MCP_DISPATCH_EMU_ERROR : MCP_DISPATCH_ALLOC_ERROR;
}


#ifndef MZ800EMU_MCP_TEST_BUILD
/**
 * @brief Testovací háček: jednou pozdrží zpracování vybraného příkazu.
 *
 * Proměnné prostředí (čtou se jednou, při prvním požadavku):
 *  - MZ800EMU_TEST_STALL_MCP_CMD: wire jméno příkazu (např. "get_registers"),
 *  - MZ800EMU_TEST_STALL_MS: doba zaseknutí emu vlákna v ms.
 * Při prvním požadavku s tímto jménem armuje dbgapi háček
 * (dbgapi_test_arm_dispatch_stall()) na příslušný dbgapi příkaz; emu
 * vlákno pak uprostřed jeho dispatch spí. Jednorázové. Slouží jen
 * regresnímu testu tests/mcp/test_mcp_busy_stall.py, v běžném provozu
 * proměnné nejsou nastavené a háček nic nedělá.
 *
 * @param entry Záznam dispatch tabulky právě zpracovávaného příkazu.
 */
static void _test_hook_maybe_arm_stall(const st_MCP_CMD_MAP_ENTRY *entry) {
    static gsize s_init = 0;
    static char *s_cmd = NULL;
    static int s_ms = 0;
    static gint s_used = 0;

    if (g_once_init_enter(&s_init)) {
        const char *c = g_getenv("MZ800EMU_TEST_STALL_MCP_CMD");
        const char *m = g_getenv("MZ800EMU_TEST_STALL_MS");
        if (c && c[0] && m && m[0]) {
            s_cmd = g_strdup(c);
            s_ms = (int)g_ascii_strtoll(m, NULL, 10);
        }
        g_once_init_leave(&s_init, 1);
    }
    if (!s_cmd || s_ms <= 0 || entry->dbgapi_cmd == DBGAPI_CMD_NONE) return;
    if (strcmp(entry->name, s_cmd) != 0) return;
    if (!g_atomic_int_compare_and_exchange(&s_used, 0, 1)) return;
    dbgapi_test_arm_dispatch_stall(entry->dbgapi_cmd, s_ms);
}
#endif


en_MCP_DISPATCH_RESULT mcp_dispatch_request(const st_JSONL_MESSAGE *req,
                                            char **out_response) {
    if (!req || !out_response) {
        if (out_response) *out_response = NULL;
        return MCP_DISPATCH_NOT_A_REQUEST;
    }
    *out_response = NULL;

    /* Nový požadavek - vynulovat sledování submitů (viz s_submit_track). */
    memset(&s_submit_track, 0, sizeof(s_submit_track));
    s_submit_track.last_status = DBGAPI_SUBMIT_OK;

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
#ifndef MZ800EMU_MCP_TEST_BUILD
            _test_hook_maybe_arm_stall(&g_cmd_map[i]);
#endif
            /* Revize stavu (state_rev.h): nástroje, které mění stav stroje
             * (cílový dbgapi příkaz druhu MUTATES), vracejí `state_rev`
             * a přijímají podmínku `expected_state_rev`. */
            bool mutating = g_cmd_map[i].dbgapi_cmd != DBGAPI_CMD_NONE
                            && dbgapi_cmd_mutates(g_cmd_map[i].dbgapi_cmd);
            s_submit_track.add_state_rev = mutating;
            JsonNode *dn = (JsonNode *)jsonl_msg_get_data_node(req);
            JsonObject *dobj = (dn && json_node_get_node_type(dn) == JSON_NODE_OBJECT)
                               ? json_node_get_object(dn) : NULL;
            uint64_t expected_rev = 0;
            if (dobj && json_object_has_member(dobj, "expected_state_rev")) {
                if (!mutating) {
                    return _err_response(req_id, "Invalid parameters: expected_state_rev is "
                                         "accepted only by commands that change the machine state",
                                         MCP_DISPATCH_INVALID_PARAMS, out_response);
                }
                gint64 v = _obj_int_or(dobj, "expected_state_rev", -1);
                if (v < 1) {
                    return _err_response(req_id, "Invalid parameters: expected_state_rev must be "
                                         "a state_rev value (> 0)",
                                         MCP_DISPATCH_INVALID_PARAMS, out_response);
                }
                expected_rev = (uint64_t)v;
                s_submit_track.req_cond = DBGAPI_REV_COND_EXPECT_EQUAL;
                s_submit_track.req_value = expected_rev;
                s_submit_track.req_cmd = g_cmd_map[i].dbgapi_cmd;
                s_submit_track.req_pending = true;
            }
            en_MCP_DISPATCH_RESULT rc = g_cmd_map[i].handler(req, out_response);
            if (s_submit_track.rev_failed) {
                /* Podmínka revize nesplněna: nic se neprovedlo; jednotná
                 * chyba místo chyby handleru. */
                free(*out_response);
                *out_response = NULL;
                gchar *msg = s_submit_track.running
                    ? g_strdup_printf(MCP_PRECONDITION_PREFIX "emulator running "
                                      "(expected_state_rev=%" G_GUINT64_FORMAT ")", expected_rev)
                    : g_strdup_printf(MCP_PRECONDITION_PREFIX "state changed "
                                      "(expected_state_rev=%" G_GUINT64_FORMAT
                                      ", current_state_rev=%" G_GUINT64_FORMAT ")",
                                      expected_rev, s_submit_track.state_rev);
                rc = _err_response(req_id, msg, MCP_DISPATCH_EMU_ERROR, out_response);
                g_free(msg);
            }
            return rc;
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

#if MZTVSYS == MZTVSYS_PAL
    *tv_system_out    = "PAL";
    *framerate_hz_out = 50;
#elif MZTVSYS == MZTVSYS_NTSC
    *tv_system_out    = "NTSC";
    *framerate_hz_out = 60;
#else
    *tv_system_out    = "unknown";
    *framerate_hz_out = 0;
#endif

#if MZARCH == 700
    *mode_out = "native";   /* MZ-700 = vždy sám sebou */
#elif MZARCH == 800
    /* DMD bit 3 (REGISTER_DMD_FLAG_MZ700) = 1: režim MZ-700, 0: režim
     * MZ-800 (hw/04-io-ports.md, GDG_MZ800_DMD_TEST_MZ700). */
    bool native_800 = !GDG_MZ800_DMD_TEST_MZ700;
    *mode_out = native_800 ? "native" : "compat700";
#elif MZARCH == 1500
    /* MZ-1500 regDMD bit 0 = 1: 1500 native, 0: 700 sim (= reset default) */
    bool native_1500 = (g_gdg.regDMD & 0x01) != 0;
    *mode_out = native_1500 ? "native" : "compat700";
#else
    *mode_out = "unknown";
#endif
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
    _ms_close_all();
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
        case REGION_KIND_DEVICE:            return "device";
        case REGION_KIND_COUNT:             break;
    }
    return "unknown";
}


/**
 * @brief Generace rozložení výčtu z posledního `regions_list` (0 = žádný).
 *
 * `region_read` a `region_write` ji předávají backendu, pokud klient
 * nepošle vlastní `layout_gen`: ID z výčtu před změnou sestavy HW tak
 * skončí chybou "stale region id", ne čtením jiného regionu. Sdílená pro
 * všechny MCP klienty (výčet je jeden pro celý emulátor). Atomický přístup
 * (g_atomic_int_*), handlery běží ve vláknech MCP serveru.
 */
static gint s_mcp_regions_layout_gen = 0;

/** @brief Text chyby pro ID regionu ze starší generace rozložení. */
#define MCP_REGION_STALE_MSG \
    "stale region id: the hardware configuration changed since regions_list " \
    "(RAMdisk, MemExt, PEZIK, snapshot load); call regions_list again"

/**
 * @brief Generace rozložení pro `region_read` / `region_write`.
 *
 * @param obj Parametry požadavku.
 * @param[out] out_gen `layout_gen` z parametrů, jinak generace posledního
 *             `regions_list` (0 = klient výčet nedělal, bez kontroly).
 * @return false = `layout_gen` je v parametrech, ale není kladné celé číslo
 *         do 0xFFFFFFFF.
 */
static bool _region_layout_gen_param(JsonObject *obj, uint32_t *out_gen) {
    if (obj && json_object_has_member(obj, "layout_gen")) {
        JsonNode *n = json_object_get_member(obj, "layout_gen");
        if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) {
            return false;
        }
        gint64 v = json_node_get_int(n);
        if (v < 1 || v > 0xFFFFFFFFLL) return false;
        *out_gen = (uint32_t)v;
        return true;
    }
    *out_gen = (uint32_t)g_atomic_int_get(&s_mcp_regions_layout_gen);
    return true;
}

/**
 * @brief `regions_list` handler - enumerate všech regionů.
 *
 * Bez params. Vrátí JSON pole regionů s metadaty (id, kind, sub_id,
 * name, logical_base, size, writable, connected, mapped_now) a
 * `layout_gen` (generace rozložení výčtu). Klient si vybere region
 * a použije ID v `region_read`. ID platí, dokud se nezmění sestava HW
 * (RAMdisk, MemExt, PEZIK, načtení snapshotu s jinou sestavou): potom
 * `region_read` / `region_write` se starým ID skončí chybou
 * "stale region id" a klient musí výčet zopakovat.
 *
 * Výčet je úplný (bez stropu na počet): plná sestava MZ-800 (MemExt
 * Luftner, RAMdisk STD 16 MB, oba PEZIKy) má přes 500 regionů. Dřív
 * pevné pole 128 položek schovalo banky RAMdisku.
 *
 * Side effects: zapamatuje generaci výčtu pro následující `region_read`
 * a `region_write` bez vlastního `layout_gen`.
 */
static en_MCP_DISPATCH_RESULT _handle_regions_list(
    const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    /* out == NULL = backend vrátí kopii všech regionů v out_dup. */
    st_DBGAPI_REGIONS_ENUM_PARAM param = {
        .out = NULL,
        .max_count = 0,
        .out_count = 0,
        .out_dup = NULL,
        .out_layout_gen = 0,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_REGIONS_ENUMERATE, &param, NULL)) {
        g_free(param.out_dup);
        return _err_response(req_id, "REGIONS_ENUMERATE failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    g_atomic_int_set(&s_mcp_regions_layout_gen, (gint)param.out_layout_gen);
    const st_REGION_DESC *descs = (const st_REGION_DESC *)param.out_dup;
    int count = descs ? param.out_count : 0;
    JsonObject *data = json_object_new();
    JsonArray *arr = json_array_new();
    for (int i = 0; i < count; i++) {
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
    json_object_set_int_member(data, "count", count);
    json_object_set_int_member(data, "layout_gen", (gint64)param.out_layout_gen);
    g_free(param.out_dup);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `region_read` handler - raw read N bajtů z regionu.
 *
 * Parametry: `region_id` (= z poslední `regions_list`), `offset`
 * (= 0..size-1), `length` (= 1..65536, clamp na size regionu),
 * `want_data` (bool, default true; false = jen hash), volitelně
 * `layout_gen` (generace výčtu, ze kterého ID pochází; výchozí = generace
 * posledního `regions_list`).
 *
 * Response: `{region_id, offset, length, sha256, data_b64}`. `length`
 * v response = skutečně přečtená délka (= clamped), `sha256` je hash
 * přečtených bajtů, `data_b64` jen s want_data. Při chybě `{error: "..."}`;
 * ID ze starší sestavy HW = chyba "stale region id".
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
    /* Horní mez ID kontroluje backend proti výčtu (lookup_region_copy). */
    if (region_id < 0 || region_id > G_MAXINT
            || offset < 0 || offset > 0xFFFFFFFFu
            || length < 1 || length > 65536) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool want_data = true;
    if (!_parse_want_data(obj, &want_data)) {
        return _err_response(req_id, "want_data must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint32_t layout_gen = 0;
    if (!_region_layout_gen_param(obj, &layout_gen)) {
        return _err_response(req_id, "layout_gen must be a positive integer",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint8_t *buf = g_malloc0((gsize)length);
    st_DBGAPI_REGIONS_READ_PARAM param = {
        .region_id = (int)region_id,
        .offset = (uint32_t)offset,
        .buf = buf,
        .len = (uint32_t)length,
        .out_count = 0,
        .layout_gen = layout_gen,
    };
    if (!_submit_dbgapi(DBGAPI_CMD_REGIONS_READ, &param, NULL)) {
        /* _submit_dbgapi vrací (out_count >= 0). param.out_count rozliší
         * příčinu: backend nastaví -1 když region_id/offset/region je
         * neplatný (= read proběhl, ale selhal), DBGAPI_REGIONS_ERR_STALE
         * když ID pochází ze starší sestavy HW; ponechá initial 0 když
         * příkaz vůbec neproběhl (= emu nedostupný / fronta plná / timeout).
         * Cache se plní lazy (viz dbgapi_regions lookup_region), takže
         * prázdná enumerace už není příčinou - selhání = opravdu bad ID. */
        en_MCP_DISPATCH_RESULT rc = (param.out_count < 0)
            ? MCP_DISPATCH_INVALID_PARAMS : MCP_DISPATCH_EMU_ERROR;
        const char *msg = (param.out_count == DBGAPI_REGIONS_ERR_STALE)
            ? MCP_REGION_STALE_MSG
            : (param.out_count < 0)
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
    _read_result_to_json(data, buf, (gsize)param.out_count, want_data);
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
 *    - MZ-800: bit 3 = 0 native, 1 compat700 (režim MZ-700)
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
#if HAVE_PIOZ80
    json_object_set_boolean_member(caps, "has_pioz80", TRUE);
#else
    json_object_set_boolean_member(caps, "has_pioz80", FALSE);
#endif
#if HAVE_PSG
    json_object_set_int_member(caps, "psg_count", HAVE_PSG);
#else
    json_object_set_int_member(caps, "psg_count", 0);
#endif
    json_object_set_boolean_member(caps, "hwext_fdc",
                                   CFG_HWEXT_HAVE_FDC ? TRUE : FALSE);
    json_object_set_boolean_member(caps, "hwext_ide8",
                                   CFG_HWEXT_HAVE_IDE8 ? TRUE : FALSE);
    json_object_set_boolean_member(caps, "hwext_ramdisk",
                                   CFG_HWEXT_HAVE_RAMDISK ? TRUE : FALSE);
    json_object_set_boolean_member(caps, "hwext_qdisk",
                                   CFG_HWEXT_HAVE_QDISK ? TRUE : FALSE);
    json_object_set_string_member(caps, "cpu_model", "Z80");
    json_object_set_object_member(data, "capabilities", caps);

    /* Clock domains - per Sharp HW jsou všechny derivované z GDG base
     * clocku (= GDGCLK_BASE) přes integer dividery. CPU clock je
     * GDGCLK / GDGCLK2CPU_DIVIDER, CTC0 input je GDGCLK / CTC0_DIVIDER,
     * PSG input je CPU_CLOCK / 16 (= PSG_DIVIDER = 16 * CPU_DIVIDER).
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
                               (gint64)GDGCLK_BASE);
    json_object_set_int_member(clocks, "gdg_real_base_hz",
                               (gint64)GDGCLK_REAL_BASE);
    json_object_set_int_member(clocks, "cpu_hz",
                               (gint64)(GDGCLK_BASE / GDGCLK2CPU_DIVIDER));
    json_object_set_int_member(clocks, "cpu_divider", GDGCLK2CPU_DIVIDER);
    json_object_set_int_member(clocks, "ctc0_input_hz",
                               (gint64)(GDGCLK_BASE / GDGCLK_CTC0_DIVIDER));
    json_object_set_int_member(clocks, "ctc0_divider", GDGCLK_CTC0_DIVIDER);
#if HAVE_PSG
    json_object_set_int_member(clocks, "psg_input_hz",
                               (gint64)(GDGCLK_BASE / PSG_DIVIDER));
    json_object_set_int_member(clocks, "psg_divider", PSG_DIVIDER);
#else
    json_object_set_null_member(clocks, "psg_input_hz");
    json_object_set_null_member(clocks, "psg_divider");
#endif
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
                               (gint64)VIDEO_SCREEN_WIDTH);
    json_object_set_int_member(scanline, "screen_total_height_lines",
                               (gint64)VIDEO_SCREEN_HEIGHT);
    json_object_set_int_member(scanline, "screen_total_ticks_per_frame",
                               (gint64)VIDEO_SCREEN_TICKS);
    json_object_set_int_member(scanline, "screens_per_sec",
                               (gint64)VIDEO_SCREENS_PER_SEC);
    json_object_set_int_member(scanline, "display_width",
                               (gint64)VIDEO_DISPLAY_WIDTH);
    json_object_set_int_member(scanline, "display_height",
                               (gint64)VIDEO_DISPLAY_HEIGHT);
    json_object_set_int_member(scanline, "canvas_width",
                               (gint64)VIDEO_CANVAS_WIDTH);
    json_object_set_int_member(scanline, "canvas_height",
                               (gint64)VIDEO_CANVAS_HEIGHT);
    json_object_set_int_member(scanline, "border_left_width",
                               (gint64)VIDEO_BORDER_LEFT_WIDTH);
    json_object_set_int_member(scanline, "border_right_width",
                               (gint64)VIDEO_BORDER_RIGHT_WIDTH);
    json_object_set_int_member(scanline, "border_top_height",
                               (gint64)VIDEO_BORDER_TOP_HEIGHT);
    json_object_set_int_member(scanline, "border_bottom_height",
                               (gint64)VIDEO_BORDER_BOTOM_HEIGHT);
    json_object_set_int_member(scanline, "h_sync_ticks",
                               (gint64)VIDEO_H_SYNC_TICKS);
    json_object_set_int_member(scanline, "h_back_porch_ticks",
                               (gint64)VIDEO_H_BACK_PORCH_TICKS);
    json_object_set_int_member(scanline, "h_front_porch_ticks",
                               (gint64)VIDEO_H_FRONT_PORCH_TICKS);
    json_object_set_object_member(data, "scanline", scanline);

    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `region_write` handler - raw write N bajtů do regionu.
 *
 * Parametry: `region_id` (= z poslední `regions_list`), `offset`
 * (= 0..size-1), `data_b64` (= base64 bajty, max 65536 dekódovaných),
 * volitelně `layout_gen` (jako u `region_read`), `expected_sha256`
 * a `want_hashes` (podmíněný zápis, observe-cas M10: hash bajtů, které
 * zápis po ořezu na konec regionu přepíše, stejný jako `sha256`
 * z `region_read` téhož rozsahu).
 *
 * Response: `{region_id, offset, length, written}`. `length` = skutečně
 * zapsaná délka (= clamped). `written` = true pokud aspoň 1 bajt
 * prošel; false pokud region read-only nebo disconnected. ID ze starší
 * sestavy HW = chyba "stale region id", nic se nezapíše. S podmínkou
 * nebo `want_hashes` navíc `before_sha256` a `after_sha256`; nesplněná
 * podmínka = chyba `Precondition failed: memory changed (...)` (chyby
 * ID, generace a zapisovatelnosti mají přednost).
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
    /* Horní mez ID kontroluje backend proti výčtu (lookup_region_copy). */
    if (region_id < 0 || region_id > G_MAXINT
            || offset < 0 || offset > 0xFFFFFFFFu) {
        return _err_response(req_id, "Invalid parameters",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!json_object_has_member(obj, "data_b64")) {
        return _err_response(req_id, "Invalid parameters (data_b64 missing)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint32_t layout_gen = 0;
    if (!_region_layout_gen_param(obj, &layout_gen)) {
        return _err_response(req_id, "layout_gen must be a positive integer",
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
    st_DBGAPI_WRITE_COND cond;
    bool use_cond = false;
    const char *cerr = _parse_write_cond(obj, &cond, &use_cond);
    if (cerr) {
        g_free(decoded);
        gchar *msg = g_strdup_printf("Invalid parameters: %s", cerr);
        en_MCP_DISPATCH_RESULT rc = _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS,
                                                  out_response);
        g_free(msg);
        return rc;
    }
    st_DBGAPI_REGIONS_WRITE_PARAM param = {
        .region_id = (int)region_id,
        .offset = (uint32_t)offset,
        .data = decoded,
        .len = (uint32_t)decoded_len,
        .out_count = 0,
        .layout_gen = layout_gen,
        .cond = use_cond ? &cond : NULL,
    };
    bool submit_ok = _submit_dbgapi(DBGAPI_CMD_REGIONS_WRITE, &param, NULL);
    if (!submit_ok && use_cond && cond.precondition_failed
        && s_submit_track.last_status == DBGAPI_SUBMIT_FAILED) {
        g_free(decoded);
        return _write_cond_err(req_id, &cond, out_response);
    }
    if (!submit_ok) {
        /* Stejná logika jako region_read: param.out_count rozliší bad
         * region/read-only (-1, příkaz proběhl) a ID ze starší sestavy HW
         * (DBGAPI_REGIONS_ERR_STALE) od emu nedostupný (0, neproběhl).
         * Cache se plní lazy, takže prázdná enumerace už není příčinou
         * selhání. */
        en_MCP_DISPATCH_RESULT rc = (param.out_count < 0)
            ? MCP_DISPATCH_INVALID_PARAMS : MCP_DISPATCH_EMU_ERROR;
        const char *msg = (param.out_count == DBGAPI_REGIONS_ERR_STALE)
            ? MCP_REGION_STALE_MSG
            : (param.out_count < 0)
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
    if (use_cond) _write_cond_to_json(data, &cond);
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


/* ============================================================================
 *  screenshot-series: série PNG každého (změněného) emulovaného snímku
 * ============================================================================ */

/**
 * @brief Stav série jako JSON objekt (z out_status parametru dbgapi).
 * @param st Stav.
 * @return Nový JsonObject (vlastní volající).
 */
static JsonObject *_screenshot_series_status_json(const st_SCREENSHOT_SERIES_STATUS *st) {
    JsonObject *o = json_object_new();
    json_object_set_boolean_member(o, "active",  st->active);
    json_object_set_boolean_member(o, "writing", st->writing);
    json_object_set_int_member(o, "frames_seen", (gint64) st->frames_seen);
    json_object_set_int_member(o, "files",       (gint64) st->files);
    json_object_set_int_member(o, "unchanged",   (gint64) st->unchanged);
    json_object_set_string_member(o, "dir",         st->dir);
    json_object_set_string_member(o, "stop_reason", st->stop_reason);
    json_object_set_string_member(o, "last_error",  st->last_error);
    return o;
}

/**
 * @brief Provede DBGAPI_CMD_SCREENSHOT_SERIES.
 *
 * @param p       Parametr (op vyplněn).
 * @param err_out Výstup: anglický text chyby operace (ukazuje do p->out_error
 *                nebo na statický text), NULL = operace proběhla.
 * @return false = příkaz se nepodařilo předat emu vláknu (bez textu chyby).
 */
static bool _screenshot_series_submit(st_DBGAPI_SCREENSHOT_SERIES_PARAM *p, const char **err_out) {
    p->out_result = DBGAPI_SCREENSHOT_SERIES_RESULT_FAILED;
    p->out_error[0] = '\0';
    bool ok = _submit_dbgapi(DBGAPI_CMD_SCREENSHOT_SERIES, p, NULL);
    if (ok) {
        *err_out = NULL;
        return true;
    }
    /* Handler proběhl, ale operace selhala (out_error vyplněn) - jinak
     * se příkaz vůbec nepředal. */
    *err_out = p->out_error[0] ? p->out_error : NULL;
    return *err_out != NULL;
}

/**
 * @brief `screenshot_series_start` handler - spustí sérii screenshotů.
 *
 * Parametry (všechny volitelné):
 *   - `dir`          (string) - adresář série; chybí = nový podadresář výstupní
 *                    složky screenshotů (`<platforma>_frames_RRRRMMDD_HHMMSS`),
 *   - `area`, `scale`, `crop` - jako screenshot_save_to_file, ale výchozí
 *                    `scale` je "native",
 *   - `only_changed` (bool, výchozí true) - jen snímky odlišné od posledního
 *                    uloženého (posuzuje se výsledek v oblasti, měřítku a ořezu),
 *   - `frames`       (int >= 0) - po tolika emulovaných snímcích stop (0 = do stop),
 *   - `max_files`    (int >= 0) - pojistka počtu souborů (0 = 10000).
 *
 * Response: stav série (viz _screenshot_series_status_json) + area, scale,
 * only_changed. První snímek se zachytí na nejbližším konci emulovaného
 * snímku (série snímá jen za běhu emulace). Neplatné parametry =>
 * INVALID_PARAMS, běžící série nebo nelze vytvořit adresář => EMU_ERROR.
 */
static en_MCP_DISPATCH_RESULT _handle_screenshot_series_start(const st_JSONL_MESSAGE *req,
                                                              char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    st_DBGAPI_SCREENSHOT_SERIES_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_SCREENSHOT_SERIES_OP_START;
    screenshot_series_params_init(&p.params);

    char opts_err[128];
    if (!_screenshot_opts_from_json(obj, &p.params.opts, opts_err, sizeof(opts_err))) {
        return _err_response(req_id, opts_err, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (!obj || !json_object_has_member(obj, "scale")) {
        p.params.opts.scale_mode = SCREENSHOT_SCALE_NATIVE;
    }
    int has_dir = _videorec_member(obj, "dir", G_TYPE_STRING);
    int has_only = _videorec_member(obj, "only_changed", G_TYPE_BOOLEAN);
    int has_frames = _videorec_member(obj, "frames", G_TYPE_INT64);
    int has_max = _videorec_member(obj, "max_files", G_TYPE_INT64);
    if (has_dir < 0 || has_only < 0 || has_frames < 0 || has_max < 0) {
        return _err_response(req_id,
                             "Invalid parameters: dir must be a string, only_changed a boolean, "
                             "frames and max_files integers",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 frames = has_frames ? json_object_get_int_member(obj, "frames") : 0;
    gint64 max_files = has_max ? json_object_get_int_member(obj, "max_files") : 0;
    if (frames < 0 || max_files < 0 || max_files > G_MAXUINT) {
        return _err_response(req_id, "Invalid parameters: frames and max_files must be >= 0",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (has_dir) g_strlcpy(p.params.dir, json_object_get_string_member(obj, "dir"), sizeof(p.params.dir));
    if (has_only) p.params.only_changed = json_object_get_boolean_member(obj, "only_changed");
    p.params.frames = (uint64_t) frames;
    p.params.max_files = (unsigned) max_files;

    const char *err = NULL;
    if (!_screenshot_series_submit(&p, &err) || err) {
        return _err_response(req_id, err ? err : "screenshot_series command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = _screenshot_series_status_json(&p.out_status);
    json_object_set_string_member(resp, "area",  screenshot_area_to_string(p.params.opts.area));
    json_object_set_string_member(resp, "scale", screenshot_scale_to_string(p.params.opts.scale_mode,
                                                                            p.params.opts.window_scale));
    json_object_set_boolean_member(resp, "only_changed", p.params.only_changed);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `screenshot_series_stop` handler - ukončí snímání série.
 *
 * Parametr `wait` (bool, výchozí true): počkat (max 30 s), až zapisovací
 * vlákno dopíše frontu - pak `writing` = false a `files` je konečné.
 * Response: stav série. Bez běžící série chyba "No frame series is running".
 */
static en_MCP_DISPATCH_RESULT _handle_screenshot_series_stop(const st_JSONL_MESSAGE *req,
                                                             char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int has_wait = _videorec_member(obj, "wait", G_TYPE_BOOLEAN);
    if (has_wait < 0) {
        return _err_response(req_id, "Invalid parameters: wait must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool wait = has_wait ? json_object_get_boolean_member(obj, "wait") : true;

    st_DBGAPI_SCREENSHOT_SERIES_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_SCREENSHOT_SERIES_OP_STOP;
    const char *err = NULL;
    if (!_screenshot_series_submit(&p, &err) || err) {
        return _err_response(req_id, err ? err : "screenshot_series command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    for (int i = 0; wait && p.out_status.writing && i < 3000; i++) {
        g_usleep(10000);
        memset(&p, 0, sizeof(p));
        p.op = DBGAPI_SCREENSHOT_SERIES_OP_STATUS;
        if (!_screenshot_series_submit(&p, &err)) break;
    }
    return _ok_response(req_id, _screenshot_series_status_json(&p.out_status), out_response);
}

/**
 * @brief `screenshot_series_status` handler - stav běžící nebo poslední série.
 *
 * Bez parametrů. Response: stav série (active, writing, frames_seen, files,
 * unchanged, dir, stop_reason, last_error).
 */
static en_MCP_DISPATCH_RESULT _handle_screenshot_series_status(const st_JSONL_MESSAGE *req,
                                                               char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_SCREENSHOT_SERIES_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_SCREENSHOT_SERIES_OP_STATUS;
    const char *err = NULL;
    if (!_screenshot_series_submit(&p, &err)) {
        return _err_response(req_id, "screenshot_series command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, _screenshot_series_status_json(&p.out_status), out_response);
}




/* ============================================================================
 *  pc-stats: profiler podle PC (počet, T-stavy a WAIT na adresu instrukce)
 * ============================================================================ */

/**
 * @brief Souhrn profileru podle PC jako JSON objekt.
 * @param s Souhrn.
 * @return Nový JsonObject (vlastní volající).
 */
static JsonObject *_profiler_pc_summary_json(const st_PROFILER_PC_SUMMARY *s) {
    JsonObject *o = json_object_new();
    json_object_set_boolean_member(o, "active", s->active);
    json_object_set_int_member(o, "instructions",    (gint64) s->instructions);
    json_object_set_int_member(o, "tstates",         (gint64) s->tstates);
    json_object_set_int_member(o, "wait",            (gint64) s->wait);
    json_object_set_int_member(o, "halt_idle",       (gint64) s->halt_idle);
    json_object_set_int_member(o, "other_tstates",   (gint64) s->other_tstates);
    json_object_set_int_member(o, "elapsed_tstates", (gint64) s->elapsed_tstates);
    json_object_set_int_member(o, "unique_pcs",      (gint64) s->unique_pcs);
    return o;
}

/**
 * @brief Přečte volitelné `sort` ("tstates", "count", "wait", "addr").
 * @param obj  Data požadavku (může být NULL).
 * @param def  Řazení, když parametr chybí.
 * @param out  Výstup.
 * @return false = neplatná hodnota.
 */
static bool _profiler_pc_sort_param(JsonObject *obj, en_PROFILER_PC_SORT def, en_PROFILER_PC_SORT *out) {
    *out = def;
    int has = _videorec_member(obj, "sort", G_TYPE_STRING);
    if (has < 0) return false;
    if (!has) return true;
    const char *s = json_object_get_string_member(obj, "sort");
    if (strcmp(s, "tstates") == 0) { *out = PROFILER_PC_SORT_TSTATES; return true; }
    if (strcmp(s, "count") == 0)   { *out = PROFILER_PC_SORT_COUNT;   return true; }
    if (strcmp(s, "wait") == 0)    { *out = PROFILER_PC_SORT_WAIT;    return true; }
    if (strcmp(s, "addr") == 0)    { *out = PROFILER_PC_SORT_ADDR;    return true; }
    return false;
}

/**
 * @brief Provede operaci bez řádků a vrátí souhrn (start, stop, reset).
 * @param req          Požadavek.
 * @param p            Parametr (op vyplněn).
 * @param out_response Výstup JSON.
 * @return Výsledek dispatch.
 */
static en_MCP_DISPATCH_RESULT _profiler_pc_simple(const st_JSONL_MESSAGE *req,
                                                  st_DBGAPI_PROFILER_PC_PARAM *p,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_PC, p, NULL)) {
        return _err_response(req_id, p->out_error[0] ? p->out_error : "profiler_pc command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, _profiler_pc_summary_json(&p->out_summary), out_response);
}

/**
 * @brief `profiler_pc_start` handler - zapne sběr profileru podle PC.
 *
 * Parametr `reset` (bool, výchozí true): vynulovat data před startem.
 * Response: souhrn (viz _profiler_pc_summary_json).
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_start(const st_JSONL_MESSAGE *req,
                                                        char **out_response) {
    JsonObject *obj = _videorec_data_obj(req);
    int has_reset = _videorec_member(obj, "reset", G_TYPE_BOOLEAN);
    if (has_reset < 0) {
        return _err_response(jsonl_msg_get_req_id(req), "Invalid parameters: reset must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_PROFILER_PC_OP_START;
    p.reset = has_reset ? json_object_get_boolean_member(obj, "reset") : true;
    return _profiler_pc_simple(req, &p, out_response);
}

/**
 * @brief `profiler_pc_stop` handler - vypne sběr, data zůstanou.
 *
 * Bez parametrů. Response: souhrn.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_stop(const st_JSONL_MESSAGE *req,
                                                       char **out_response) {
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_PROFILER_PC_OP_STOP;
    return _profiler_pc_simple(req, &p, out_response);
}

/**
 * @brief `profiler_pc_reset` handler - vynuluje data (stav sběru nemění).
 *
 * Bez parametrů. Response: souhrn.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_reset(const st_JSONL_MESSAGE *req,
                                                        char **out_response) {
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_PROFILER_PC_OP_RESET;
    return _profiler_pc_simple(req, &p, out_response);
}

/**
 * @brief `profiler_pc_get` handler - nejvytíženější adresy inline.
 *
 * Parametry (volitelné): `sort` ("tstates" výchozí, "count", "wait", "addr"),
 * `limit` (1..DBGAPI_PROFILER_PC_ROWS_MAX, výchozí 50), `from` a `to`
 * (rozsah adres včetně, výchozí 0..65535).
 *
 * Response: souhrn + `sort`, `total_rows` (adres s vykonáním v rozsahu)
 * a `rows` [{pc, count, tstates, wait, tstates_pct, avg_tstates, symbol,
 * sym_offset}]; symbol = nejbližší symbol na adrese <= pc ("" = žádný).
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_get(const st_JSONL_MESSAGE *req,
                                                      char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    en_PROFILER_PC_SORT sort;
    if (!_profiler_pc_sort_param(obj, PROFILER_PC_SORT_TSTATES, &sort)) {
        return _err_response(req_id, "Invalid parameters: sort must be tstates, count, wait or addr",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    int has_limit = _videorec_member(obj, "limit", G_TYPE_INT64);
    int has_from = _videorec_member(obj, "from", G_TYPE_INT64);
    int has_to = _videorec_member(obj, "to", G_TYPE_INT64);
    if (has_limit < 0 || has_from < 0 || has_to < 0) {
        return _err_response(req_id, "Invalid parameters: limit, from and to must be integers",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 limit = has_limit ? json_object_get_int_member(obj, "limit") : 50;
    gint64 from = has_from ? json_object_get_int_member(obj, "from") : 0;
    gint64 to = has_to ? json_object_get_int_member(obj, "to") : 0xFFFF;
    if (limit < 1 || limit > DBGAPI_PROFILER_PC_ROWS_MAX || from < 0 || from > 0xFFFF
        || to < 0 || to > 0xFFFF || to < from) {
        return _err_response(req_id, "Invalid parameters: limit must be 1..1000, "
                             "0 <= from <= to <= 65535",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_PROFILER_PC_OP_GET;
    p.sort = sort;
    p.lo = (uint16_t) from;
    p.hi = (uint16_t) to;
    p.with_symbols = true;
    p.rows_max = (size_t) limit;
    p.rows = g_new0(st_DBGAPI_PROFILER_PC_ROW, p.rows_max);
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_PC, &p, NULL)) {
        g_free(p.rows);
        return _err_response(req_id, p.out_error[0] ? p.out_error : "profiler_pc_get failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = _profiler_pc_summary_json(&p.out_summary);
    static const char *const sort_names[] = { "tstates", "count", "wait", "addr" };
    json_object_set_string_member(resp, "sort", sort_names[sort]);
    json_object_set_int_member(resp, "total_rows", (gint64) p.out_total);
    JsonArray *arr = json_array_new();
    for (size_t i = 0; i < p.out_written; i++) {
        const st_DBGAPI_PROFILER_PC_ROW *r = &p.rows[i];
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "pc", r->r.pc);
        json_object_set_int_member(item, "count", (gint64) r->r.count);
        json_object_set_int_member(item, "tstates", (gint64) r->r.tstates);
        json_object_set_int_member(item, "wait", (gint64) r->r.wait);
        double pct = p.out_summary.tstates
                   ? 100.0 * (double) r->r.tstates / (double) p.out_summary.tstates : 0.0;
        json_object_set_double_member(item, "tstates_pct", pct);
        json_object_set_double_member(item, "avg_tstates",
                                      r->r.count ? (double) r->r.tstates / (double) r->r.count : 0.0);
        json_object_set_string_member(item, "symbol", r->symbol);
        json_object_set_int_member(item, "sym_offset", r->sym_offset);
        json_array_add_object_element(arr, item);
    }
    json_object_set_array_member(resp, "rows", arr);
    g_free(p.rows);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `profiler_pc_export` handler - všechny adresy do CSV.
 *
 * Parametry: `path` (string, povinný; soubor se přepíše), `sort` (volitelný,
 * výchozí "addr", jinak jako u profiler_pc_get). Response: souhrn + `path` a `rows` (zapsané
 * řádky). Chyba zápisu => EMU_ERROR s textem.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_export(const st_JSONL_MESSAGE *req,
                                                         char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    en_PROFILER_PC_SORT sort;
    if (!_profiler_pc_sort_param(obj, PROFILER_PC_SORT_ADDR, &sort)) {
        return _err_response(req_id, "Invalid parameters: sort must be tstates, count, wait or addr",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (_videorec_member(obj, "path", G_TYPE_STRING) != 1
        || !json_object_get_string_member(obj, "path")[0]) {
        return _err_response(req_id, "Invalid parameters: path must be a non-empty string",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *path = json_object_get_string_member(obj, "path");
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_PROFILER_PC_OP_EXPORT;
    p.sort = sort;
    p.path = path;
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_PC, &p, NULL)) {
        return _err_response(req_id, p.out_error[0] ? p.out_error : "profiler_pc_export failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = _profiler_pc_summary_json(&p.out_summary);
    json_object_set_string_member(resp, "path", path);
    json_object_set_int_member(resp, "rows", (gint64) p.out_written);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `profiler_pc_bitmap` handler - vykonané adresy jako bitmapa 8 KiB.
 *
 * Bez parametrů; funguje za běhu sběru i po něm, emulaci nepozastaví ani
 * neposune (čte se v emu vlákně mezi instrukcemi). Response: souhrn +
 * `format` ("pc-bitset-lsb0": bit `adresa & 7` bajtu `adresa >> 3`),
 * `map_bytes` (8192) a `bitmap_base64`. Počet nastavených bitů = `unique_pcs`.
 */
static en_MCP_DISPATCH_RESULT _handle_profiler_pc_bitmap(const st_JSONL_MESSAGE *req,
                                                         char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_PROFILER_PC_OP_BITMAP;
    p.bitmap = g_malloc0(PROFILER_PC_BITMAP_BYTES);
    if (!_submit_dbgapi(DBGAPI_CMD_PROFILER_PC, &p, NULL)) {
        g_free(p.bitmap);
        return _err_response(req_id, p.out_error[0] ? p.out_error : "profiler_pc_bitmap failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = _profiler_pc_summary_json(&p.out_summary);
    json_object_set_string_member(resp, "format", "pc-bitset-lsb0");
    json_object_set_int_member(resp, "map_bytes", (gint64) PROFILER_PC_BITMAP_BYTES);
    gchar *b64 = g_base64_encode(p.bitmap, PROFILER_PC_BITMAP_BYTES);
    json_object_set_string_member(resp, "bitmap_base64", b64);
    g_free(b64);
    g_free(p.bitmap);
    return _ok_response(req_id, resp, out_response);
}

/* ============================================================================
 *  audio-capture: zachycení zvuku do složky (events, PSG zápisy, 2x WAV)
 * ============================================================================ */

/** @brief Jména proudů paměti zachycení ve stavu (pořadí en_AUDIO_CAPTURE_STREAM). */
static const char *const s_ac_stream_names[AUDIO_CAPTURE_STREAM_COUNT] = {
    "psg_writes", "levels", "frames", "pcm_reference", "pcm_sdl"
};

/**
 * @brief Stav zachycení zvuku jako JSON objekt.
 * @param st Stav.
 * @return Nový JsonObject (vlastní volající).
 */
static JsonObject *_audio_capture_status_json(const st_AUDIO_CAPTURE_STATUS *st) {
    JsonObject *o = json_object_new();
    json_object_set_boolean_member(o, "pending", st->pending);
    json_object_set_boolean_member(o, "active",  st->active);
    json_object_set_int_member(o, "frames",            (gint64) st->frames);
    json_object_set_int_member(o, "events",            (gint64) st->events);
    json_object_set_int_member(o, "psg_writes",        (gint64) st->psg_writes);
    json_object_set_int_member(o, "reference_samples", (gint64) st->reference_samples);
    json_object_set_int_member(o, "sdl_blocks",        (gint64) st->sdl_blocks);
    json_object_set_int_member(o, "sdl_samples",       (gint64) st->sdl_samples);
    json_object_set_string_member(o, "dir",         st->dir);
    json_object_set_string_member(o, "stop_reason", st->stop_reason);
    json_object_set_string_member(o, "last_error",  st->last_error);
    /* Paměť zachycení (mcp-trace-read): segment, začátek, kanály, ringy. */
    json_object_set_int_member(o, "segment", (gint64) st->segment);
    if (st->began) json_object_set_int_member(o, "origin_ticks", (gint64) st->origin_ticks);
    else json_object_set_null_member(o, "origin_ticks");
    JsonArray *chs = json_array_new();
    for (unsigned i = 0; i < st->channels && i < AUDIO_CAPTURE_MAX_CHANNELS; i++) {
        json_array_add_string_element(chs, trace_audio_channel_name(i));
    }
    json_object_set_array_member(o, "channels", chs);
    json_object_set_boolean_member(o, "stereo", st->stereo);
    if (st->memory) {
        JsonObject *m = json_object_new();
        json_object_set_int_member(m, "bytes", (gint64) st->memory_bytes);
        JsonObject *streams = json_object_new();
        for (unsigned i = 0; i < AUDIO_CAPTURE_STREAM_COUNT; i++) {
            const st_AUDIO_CAPTURE_STREAM_INFO *si = &st->streams[i];
            if (!si->present) continue;
            JsonObject *x = json_object_new();
            json_object_set_int_member(x, "record_size", (gint64) si->record_size);
            json_object_set_int_member(x, "capacity", (gint64) si->capacity);
            json_object_set_int_member(x, "first_seq", (gint64) si->first_seq);
            json_object_set_int_member(x, "next_seq", (gint64) si->next_seq);
            json_object_set_object_member(streams, s_ac_stream_names[i], x);
        }
        json_object_set_object_member(m, "streams", streams);
        json_object_set_object_member(o, "memory", m);
    } else {
        json_object_set_null_member(o, "memory");
    }
    return o;
}

/**
 * @brief Provede DBGAPI_CMD_AUDIO_CAPTURE.
 *
 * @param p       Parametr (op vyplněn).
 * @param err_out Výstup: anglický text chyby operace (ukazuje do p->out_error),
 *                NULL = operace proběhla.
 * @return false = příkaz se nepodařilo předat emu vláknu (bez textu chyby).
 */
static bool _audio_capture_submit(st_DBGAPI_AUDIO_CAPTURE_PARAM *p, const char **err_out) {
    p->out_error[0] = '\0';
    bool ok = _submit_dbgapi(DBGAPI_CMD_AUDIO_CAPTURE, p, NULL);
    if (ok) {
        *err_out = NULL;
        return true;
    }
    *err_out = p->out_error[0] ? p->out_error : NULL;
    return *err_out != NULL;
}

/**
 * @brief `audio_capture_start` handler - naplánuje zachycení zvuku.
 *
 * Parametry (všechny volitelné):
 *   - `dir` (string) - složka; chybí = nová podsložka výstupní složky
 *     zachycení (`<platforma>_audio_RRRRMMDD_HHMMSS`, viz
 *     audio_capture_resolve_output_dir()),
 *   - `frames` (int >= 0) - po tolika zvukových snímcích stop (0 = do stop),
 *   - `events`, `psg_writes`, `reference_wav`, `sdl_wav` (bool, výchozí
 *     true) - které soubory zapisovat,
 *   - `memory` (bool, výchozí true) - držet paměť zachycení (čtení přes
 *     trace_read audio_*, audio_capture_summary, audio_capture_pcm),
 *   - `pcm` (pole "reference" / "sdl", výchozí ["reference"]) - PCM ringy,
 *   - `memory_minutes` (1..120, výchozí 10) - délka ringu souhrnů snímků,
 *   - `pcm_seconds` (1..3600, výchozí 60) - délka PCM ringů.
 *
 * Response: stav zachycení (pending = true; začne na konci nejbližšího
 * zvukového snímku). Neplatné parametry => INVALID_PARAMS, běžící
 * zachycení, žádný soubor nebo nejde vytvořit složka => EMU_ERROR.
 */
static en_MCP_DISPATCH_RESULT _handle_audio_capture_start(const st_JSONL_MESSAGE *req,
                                                          char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    st_DBGAPI_AUDIO_CAPTURE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_AUDIO_CAPTURE_OP_START;
    audio_capture_params_init(&p.params);

    static const char *const flags[4] = { "events", "psg_writes", "reference_wav", "sdl_wav" };
    bool *flag_ptr[4] = { &p.params.events, &p.params.psg_writes, &p.params.reference_wav, &p.params.sdl_wav };
    int has_dir = _videorec_member(obj, "dir", G_TYPE_STRING);
    int has_frames = _videorec_member(obj, "frames", G_TYPE_INT64);
    bool bad = (has_dir < 0 || has_frames < 0);
    for (int i = 0; i < 4 && !bad; i++) {
        int h = _videorec_member(obj, flags[i], G_TYPE_BOOLEAN);
        if (h < 0) bad = true;
        else if (h) *flag_ptr[i] = json_object_get_boolean_member(obj, flags[i]);
    }
    /* Paměť zachycení: přes MCP výchozí zapnutá (GUI a CLI ji nezapínají). */
    p.params.memory = true;
    p.params.pcm_reference = true;
    int has_mem = _videorec_member(obj, "memory", G_TYPE_BOOLEAN);
    int has_min = _videorec_member(obj, "memory_minutes", G_TYPE_INT64);
    int has_pcs = _videorec_member(obj, "pcm_seconds", G_TYPE_INT64);
    if (has_mem < 0 || has_min < 0 || has_pcs < 0) bad = true;
    if (has_mem > 0) p.params.memory = json_object_get_boolean_member(obj, "memory");
    gint64 minutes = has_min > 0 ? json_object_get_int_member(obj, "memory_minutes") : AUDIO_CAPTURE_MEMORY_MINUTES_DEFAULT;
    gint64 pcm_s = has_pcs > 0 ? json_object_get_int_member(obj, "pcm_seconds") : AUDIO_CAPTURE_PCM_SECONDS_DEFAULT;
    if (minutes < 1 || minutes > 120 || pcm_s < 1 || pcm_s > 3600) bad = true;
    if (!bad && obj && json_object_has_member(obj, "pcm") && !json_node_is_null(json_object_get_member(obj, "pcm"))) {
        JsonNode *pn = json_object_get_member(obj, "pcm");
        if (json_node_get_node_type(pn) != JSON_NODE_ARRAY) {
            bad = true;
        } else {
            p.params.pcm_reference = false;
            JsonArray *pa = json_node_get_array(pn);
            for (guint i = 0; i < json_array_get_length(pa) && !bad; i++) {
                JsonNode *e = json_array_get_element(pa, i);
                const char *v = (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                                ? json_node_get_string(e) : "";
                if (g_strcmp0(v, "reference") == 0) p.params.pcm_reference = true;
                else if (g_strcmp0(v, "sdl") == 0) p.params.pcm_sdl = true;
                else bad = true;
            }
        }
    }
    if (bad) {
        return _err_response(req_id,
                             "Invalid parameters: dir must be a string, frames an integer, "
                             "events, psg_writes, reference_wav, sdl_wav and memory booleans, "
                             "pcm a list of \"reference\" / \"sdl\", memory_minutes 1..120, pcm_seconds 1..3600",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    p.params.ring_frames = (uint32_t) (minutes * 60 * VIDEO_SCREENS_PER_SEC);
    p.params.ring_pcm = (uint32_t) (pcm_s * AUDIO_CAPTURE_WAV_RATE);
    gint64 frames = has_frames ? json_object_get_int_member(obj, "frames") : 0;
    if (frames < 0) {
        return _err_response(req_id, "Invalid parameters: frames must be >= 0",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (has_dir) g_strlcpy(p.params.dir, json_object_get_string_member(obj, "dir"), sizeof(p.params.dir));
    p.params.frames = (uint64_t) frames;

    const char *err = NULL;
    if (!_audio_capture_submit(&p, &err) || err) {
        return _err_response(req_id, err ? err : "audio_capture command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, _audio_capture_status_json(&p.out_status), out_response);
}

/**
 * @brief `audio_capture_stop` handler - ukončí zachycení a dopíše soubory.
 *
 * Bez parametrů. Response: stav (konečné počty, stop_reason "Stopped").
 * Bez běžícího zachycení chyba "No audio capture is running".
 */
static en_MCP_DISPATCH_RESULT _handle_audio_capture_stop(const st_JSONL_MESSAGE *req,
                                                         char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_AUDIO_CAPTURE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_AUDIO_CAPTURE_OP_STOP;
    const char *err = NULL;
    if (!_audio_capture_submit(&p, &err) || err) {
        return _err_response(req_id, err ? err : "audio_capture command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, _audio_capture_status_json(&p.out_status), out_response);
}

/**
 * @brief `audio_capture_status` handler - stav běžícího nebo posledního zachycení.
 *
 * Bez parametrů. Response: viz _audio_capture_status_json().
 */
static en_MCP_DISPATCH_RESULT _handle_audio_capture_status(const st_JSONL_MESSAGE *req,
                                                           char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_AUDIO_CAPTURE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_AUDIO_CAPTURE_OP_STATUS;
    const char *err = NULL;
    if (!_audio_capture_submit(&p, &err)) {
        return _err_response(req_id, "audio_capture command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, _audio_capture_status_json(&p.out_status), out_response);
}


/* ---------------- Paměť zachycení zvuku: souhrn a PCM (mcp-trace-read) ---------------- */

/** @brief Kus kopie proudu paměti zachycení na jedno držení zámku modulu [B]. */
#define AC_READ_CHUNK_BYTES (1u << 20)
/** @brief Nejvýš porovnávaných vzorků reference a SDL v souhrnu (2 s). */
#define AC_COMPARE_WINDOW (2u * AUDIO_CAPTURE_WAV_RATE)
/** @brief Výchozí strop dat PCM výřezu [B] (base64 se vejde pod 50 000 znaků). */
#define AC_PCM_DEFAULT_MAX_BYTES 32768u
/** @brief Nejvyšší povolený strop dat PCM výřezu [B]. */
#define AC_PCM_MAX_MAX_BYTES (4u << 20)

/** @brief Číslo zaokrouhlené na `dec` desetinných míst (pro JSON). */
static double _ac_round(double v, int dec) {
    double m = pow(10.0, dec);
    return floor(v * m + 0.5) / m;
}

/**
 * @brief Zkopíruje rozsah proudu [from, to) po kusech (zámek modulu jen na kus).
 *
 * @param st        Proud.
 * @param from      První požadovaný záznam.
 * @param to        Za posledním požadovaným záznamem.
 * @param out_rs    Výstup: velikost záznamu (0 = proud se nedrží).
 * @param out_seq   Výstup: seq prvního zkopírovaného záznamu (>= from; vyšší =
 *                  starší záznamy ring už přepsal).
 * @param out_count Výstup: počet souvisle zkopírovaných záznamů.
 * @param out_seg   Výstup: segment (zachycení) první kopie.
 * @return Záznamy (g_free volající), NULL = nic; při změně segmentu nebo
 *         díře uprostřed (ring přepisoval během kopie) skončí dřív.
 */
static uint8_t *_ac_copy_range(en_AUDIO_CAPTURE_STREAM st, uint64_t from, uint64_t to,
                               size_t *out_rs, uint64_t *out_seq, uint64_t *out_count,
                               unsigned *out_seg) {
    *out_rs = 0;
    *out_seq = from;
    *out_count = 0;
    *out_seg = 0;
    GByteArray *buf = NULL;
    uint64_t seq = from;
    bool first = true;
    while (seq < to) {
        st_AUDIO_CAPTURE_STREAM_COPY c;
        if (!audio_capture_stream_copy(st, seq, to - seq, AC_READ_CHUNK_BYTES, &c)) {
            audio_capture_stream_copy_clear(&c);
            break;
        }
        if (first) {
            *out_seg = c.segment;
            *out_rs = c.present ? c.record_size : 0;
            *out_seq = c.data_count ? c.data_seq : seq;
            first = false;
        } else if (c.segment != *out_seg || (c.data_count && c.data_seq != seq)) {
            audio_capture_stream_copy_clear(&c);
            break;
        }
        if (c.data_count == 0) {
            audio_capture_stream_copy_clear(&c);
            break;
        }
        if (!buf) buf = g_byte_array_new();
        g_byte_array_append(buf, c.data, (guint) (c.data_count * c.record_size));
        *out_count += c.data_count;
        seq = c.data_seq + c.data_count;
        audio_capture_stream_copy_clear(&c);
    }
    if (!buf) return NULL;
    return g_byte_array_free(buf, FALSE);
}

/** @brief Pořadí double pro qsort. */
static int _ac_cmp_double(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

/** @brief Nota se jménem kanálu pro souhrn. */
typedef struct st_AC_NOTE_OUT {
    st_AUDIO_NOTE n;   /**< Nota z detektoru. */
    unsigned src;      /**< Kanál audio logu (1..8). */
} st_AC_NOTE_OUT;

/** @brief Pořadí not podle začátku, pak kanálu. */
static int _ac_cmp_note(const void *a, const void *b) {
    const st_AC_NOTE_OUT *x = (const st_AC_NOTE_OUT *) a, *y = (const st_AC_NOTE_OUT *) b;
    if (x->n.from_frame != y->n.from_frame) return (x->n.from_frame > y->n.from_frame) ? 1 : -1;
    return (int) x->src - (int) y->src;
}

/** @brief Seznam úseků snímků jako JSON pole [[from, to], ...]. */
static JsonArray *_ac_spans_json(const uint8_t *flags, size_t n, uint64_t base, bool value,
                                 size_t max, size_t *total) {
    st_AUDIO_SPAN *sp = g_new0(st_AUDIO_SPAN, max ? max : 1);
    size_t k = audio_spans(flags, n, base, value, sp, max, total);
    JsonArray *a = json_array_new();
    for (size_t i = 0; i < k; i++) {
        JsonArray *s = json_array_new();
        json_array_add_int_element(s, (gint64) sp[i].from);
        json_array_add_int_element(s, (gint64) sp[i].to);
        json_array_add_array_element(a, s);
    }
    g_free(sp);
    return a;
}

/** @brief Index vzorku PCM (44100 Hz) pro absolutní čas [takty GDG]. */
static uint64_t _ac_sample_at(uint64_t t, uint64_t origin) {
    if (t <= origin) return 0;
    return (t - origin) * AUDIO_CAPTURE_WAV_RATE / GDGCLK_BASE;
}

/**
 * @brief Detektor not PSG nad zápisy do PSG v rozsahu snímků.
 *
 * Stav kanálů začíná stavem PSG na začátku zachycení a přehrává všechny
 * zápisy držené v ringu (i před `first`), aby byl stav na začátku rozsahu
 * správný. Když ring nejstarší zápisy přepsal, stav na začátku není znám
 * (*incomplete = true).
 */
static GArray *_ac_psg_notes(uint64_t first, uint64_t last, bool stereo, bool *incomplete) {
    GArray *notes = g_array_new(FALSE, FALSE, sizeof(st_AC_NOTE_OUT));
    *incomplete = false;
#if HAVE_PSG
    st_AUDIO_CAPTURE_PSG_INITIAL ini;
    audio_capture_get_psg_initial(&ini);
    st_AUDIO_NOTE_TRACK tr[2][4];
    unsigned chips = stereo ? 2u : 1u;
    for (unsigned c = 0; c < 2; c++) {
        for (unsigned k = 0; k < 4; k++) {
            if (ini.valid) audio_note_track_init(&tr[c][k], ini.divider[c][k], ini.attn[c][k], ini.noise[c][k], 0);
            else audio_note_track_init(&tr[c][k], 0, AUDIO_PSG_ATTN_OFF, k == 3, 0);
        }
    }
    if (!ini.valid) *incomplete = true;
    size_t rs = 0;
    uint64_t seq0 = 0, n = 0;
    unsigned seg = 0;
    uint8_t *data = _ac_copy_range(AUDIO_CAPTURE_STREAM_PSG_WRITES, 0, UINT64_MAX, &rs, &seq0, &n, &seg);
    if (seq0 > 0) *incomplete = true;
    st_AUDIO_NOTE done;
    for (uint64_t i = 0; i < n && rs == sizeof(st_AUDIO_CAPTURE_PSG_REC); i++) {
        const st_AUDIO_CAPTURE_PSG_REC *w = (const st_AUDIO_CAPTURE_PSG_REC *) (data + i * rs);
        if (w->frame > last) break;
        for (unsigned c = 0; c < chips; c++) {
            if (!(w->psg & (1u << c))) continue;
            st_AUDIO_NOTE_TRACK *t = &tr[c][w->channel & 3u];
            unsigned div = t->divider, attn = t->attn;
            if (w->reg == AUDIO_CAPTURE_PSG_REG_TONE) div = w->value;
            else if (w->reg == AUDIO_CAPTURE_PSG_REG_VOLUME) attn = w->value;
            else continue;
            if (audio_note_track_update(t, div, attn, t->noise, w->frame, &done) && done.to_frame >= first) {
                st_AC_NOTE_OUT o = { done, 1u + 4u * c + (w->channel & 3u) };
                g_array_append_val(notes, o);
            }
        }
    }
    for (unsigned c = 0; c < chips; c++) {
        for (unsigned k = 0; k < 4; k++) {
            if (audio_note_track_finish(&tr[c][k], last, &done) && done.to_frame >= first) {
                st_AC_NOTE_OUT o = { done, 1u + 4u * c + k };
                g_array_append_val(notes, o);
            }
        }
    }
    g_free(data);
    g_array_sort(notes, _ac_cmp_note);
#else
    (void) first; (void) last; (void) stereo;
#endif
    return notes;
}

/**
 * @brief `audio_capture_summary` handler - souhrn paměti zachycení zvuku.
 *
 * Parametry (volitelné): `frame_from` (výchozí 0), `frame_to` (-1 = poslední),
 * `silence_rms` (0..32767, výchozí 64), `max_notes` (1..2000, výchozí 200),
 * `lag_frames` (-1 = najít, jinak pevný posun SDL cesty), `max_lag_frames`
 * (1..60, výchozí 8). Běží v dispatch vlákně, data kopíruje z paměti
 * zachycení po kusech (audio_capture_stream_copy()), emu vlákno nečeká.
 *
 * Response: frames, fps, platforma, kanály, sound_spans / silent_spans,
 * channels (po kanálech), psg_notes, ctc0 (histogram intervalů a odhad),
 * ref_vs_sdl. Bez paměti zachycení nebo bez snímků => EMU_ERROR.
 */
static en_MCP_DISPATCH_RESULT _handle_audio_capture_summary(const st_JSONL_MESSAGE *req,
                                                            char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    const char *names[] = { "frame_from", "frame_to", "silence_rms", "max_notes", "lag_frames", "max_lag_frames" };
    gint64 val[] = { 0, -1, 64, 200, -1, 8 };
    for (int i = 0; i < 6; i++) {
        int h = _videorec_member(obj, names[i], G_TYPE_INT64);
        if (h < 0) {
            return _err_response(req_id, "Invalid parameters: frame_from, frame_to, silence_rms, max_notes, "
                                 "lag_frames and max_lag_frames must be integers",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        if (h) val[i] = json_object_get_int_member(obj, names[i]);
    }
    gint64 frame_from = val[0], frame_to = val[1], silence = val[2], max_notes = val[3];
    gint64 lag_frames = val[4], max_lag_frames = val[5];
    if (frame_from < 0 || frame_to < -1 || silence < 0 || silence > 32767 || max_notes < 1 || max_notes > 2000
        || lag_frames < -1 || lag_frames > 60 || max_lag_frames < 1 || max_lag_frames > 60) {
        return _err_response(req_id, "Invalid parameters: frame_from >= 0, frame_to >= -1, silence_rms 0..32767, "
                             "max_notes 1..2000, lag_frames -1..60, max_lag_frames 1..60",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    if (!st.memory) {
        return _err_response(req_id, "No audio capture memory: start the capture with memory=true",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    const st_AUDIO_CAPTURE_STREAM_INFO *fi = &st.streams[AUDIO_CAPTURE_STREAM_FRAMES];
    if (fi->next_seq == 0) {
        return _err_response(req_id, "No captured audio frames yet (run the emulation)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    uint64_t first = ((uint64_t) frame_from > fi->first_seq) ? (uint64_t) frame_from : fi->first_seq;
    uint64_t last = (frame_to < 0 || (uint64_t) frame_to >= fi->next_seq) ? fi->next_seq - 1 : (uint64_t) frame_to;
    if (first > last) {
        char *msg = g_strdup_printf("Invalid parameters: frame range outside the captured frames [%llu, %llu]",
                                    (unsigned long long) fi->first_seq, (unsigned long long) (fi->next_seq - 1));
        return _trace_err_owned(req_id, msg, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    size_t rs = 0;
    uint64_t fseq = 0, nfr = 0;
    unsigned seg = 0;
    uint8_t *fdata = _ac_copy_range(AUDIO_CAPTURE_STREAM_FRAMES, first, last + 1, &rs, &fseq, &nfr, &seg);
    if (!fdata || rs != sizeof(st_AUDIO_CAPTURE_FRAME_REC) || nfr == 0) {
        g_free(fdata);
        return _err_response(req_id, "Audio capture frames unavailable (overwritten or a new capture started)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    first = fseq;
    last = fseq + nfr - 1;
    const st_AUDIO_CAPTURE_FRAME_REC *fr = (const st_AUDIO_CAPTURE_FRAME_REC *) fdata;
    unsigned nch = st.channels;
    if (nch > AUDIO_CAPTURE_MAX_CHANNELS) nch = AUDIO_CAPTURE_MAX_CHANNELS;

    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "platform", MZARCH_NAME);
    json_object_set_int_member(resp, "fps", (gint64) VIDEO_SCREENS_PER_SEC);
    json_object_set_int_member(resp, "gdg_clk_hz", (gint64) GDGCLK_BASE);
    json_object_set_int_member(resp, "segment", (gint64) seg);
    json_object_set_boolean_member(resp, "live", st.pending || st.active);
    json_object_set_boolean_member(resp, "stereo", st.stereo);
    JsonArray *fa = json_array_new();
    json_array_add_int_element(fa, (gint64) first);
    json_array_add_int_element(fa, (gint64) last);
    json_object_set_array_member(resp, "frames", fa);
    JsonArray *av = json_array_new();
    json_array_add_int_element(av, (gint64) fi->first_seq);
    json_array_add_int_element(av, (gint64) (fi->next_seq - 1));
    json_object_set_array_member(resp, "frames_available", av);

    /* Ticho a zvuk po snímcích. */
    uint8_t *sound = g_new0(uint8_t, nfr);
    uint64_t ticks_total = 0;
    for (uint64_t i = 0; i < nfr; i++) {
        const st_AUDIO_CAPTURE_FRAME_REC *f = &fr[i];
        ticks_total += f->len_ticks;
        if (f->flags & AUDIO_CAPTURE_FRAME_FLAG_REF) {
            unsigned m = (f->ref_rms[0] > f->ref_rms[1]) ? f->ref_rms[0] : f->ref_rms[1];
            sound[i] = (m >= (unsigned) silence);
        } else {
            uint64_t e = 0;
            for (unsigned c = 0; c < nch; c++) e += f->ch[c].edges;
            sound[i] = (e > 0);
        }
    }
    size_t total = 0;
    json_object_set_array_member(resp, "sound_spans", _ac_spans_json(sound, nfr, first, true, 50, &total));
    json_object_set_int_member(resp, "sound_spans_total", (gint64) total);
    json_object_set_array_member(resp, "silent_spans", _ac_spans_json(sound, nfr, first, false, 50, &total));
    json_object_set_int_member(resp, "silent_spans_total", (gint64) total);
    json_object_set_string_member(resp, "silence_rule",
        (fr[0].flags & AUDIO_CAPTURE_FRAME_FLAG_REF) ? "reference RMS below silence_rms" : "no level change in any channel");
    g_free(sound);

    /* Kanály. */
    JsonObject *chs = json_object_new();
    double *hz = g_new0(double, nfr);
    for (unsigned c = 0; c < nch; c++) {
        uint64_t edges = 0, active = 0, on = 0;
        unsigned maxl = 0;
        size_t nh = 0;
        for (uint64_t i = 0; i < nfr; i++) {
            const st_AUDIO_CAPTURE_FRAME_CH *x = &fr[i].ch[c];
            edges += x->edges;
            on += x->on_ticks;
            if (x->max_level > maxl) maxl = x->max_level;
            if (x->edges) {
                active++;
                if (fr[i].len_ticks) hz[nh++] = (double) x->edges / 2.0 / ((double) fr[i].len_ticks / (double) GDGCLK_BASE);
            }
        }
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "active_frames", (gint64) active);
        json_object_set_int_member(o, "edges", (gint64) edges);
        json_object_set_int_member(o, "max_level", (gint64) maxl);
        json_object_set_double_member(o, "on_pct", ticks_total ? _ac_round(100.0 * (double) on / (double) ticks_total, 1) : 0.0);
        if (nh) {
            qsort(hz, nh, sizeof(double), _ac_cmp_double);
            json_object_set_double_member(o, "est_hz_median", _ac_round(hz[nh / 2], 1));
        } else {
            json_object_set_null_member(o, "est_hz_median");
        }
        json_object_set_object_member(chs, trace_audio_channel_name(c), o);
    }
    g_free(hz);
    json_object_set_object_member(resp, "channels", chs);
    json_object_set_string_member(resp, "est_hz_rule", "level changes / 2 per second, median over active frames");

    /* Noty PSG. */
    bool incomplete = false;
    GArray *notes = _ac_psg_notes(first, last, st.stereo, &incomplete);
    JsonArray *na = json_array_new();
    for (guint i = 0; i < notes->len && i < (guint) max_notes; i++) {
        const st_AC_NOTE_OUT *o = &g_array_index(notes, st_AC_NOTE_OUT, i);
        JsonObject *n = json_object_new();
        json_object_set_string_member(n, "ch", trace_audio_channel_name(o->src));
        if (o->n.noise) {
            json_object_set_string_member(n, "note", "noise");
        } else {
            double f = audio_psg_tone_hz(o->n.divider, GDGCLK_BASE, GDGCLK2CPU_DIVIDER);
            int pitch = 0, cents = 0;
            char nm[8];
            audio_hz_to_midi(f, &pitch, &cents);
            audio_midi_note_name(pitch, nm, sizeof(nm));
            json_object_set_string_member(n, "note", nm);
            json_object_set_int_member(n, "cents", cents);
            json_object_set_double_member(n, "hz", _ac_round(f, 2));
            json_object_set_int_member(n, "divider", (gint64) o->n.divider);
        }
        json_object_set_int_member(n, "from_frame", (gint64) o->n.from_frame);
        json_object_set_int_member(n, "to_frame", (gint64) o->n.to_frame);
        json_object_set_int_member(n, "max_level", (gint64) o->n.max_level);
        json_array_add_object_element(na, n);
    }
    json_object_set_array_member(resp, "psg_notes", na);
    json_object_set_int_member(resp, "psg_notes_total", (gint64) notes->len);
    json_object_set_boolean_member(resp, "psg_notes_truncated", notes->len > (guint) max_notes);
    json_object_set_boolean_member(resp, "psg_state_incomplete", incomplete);
    g_array_free(notes, TRUE);

    /* CTC0 (reproduktor): histogram intervalů mezi hranami. */
    {
        uint32_t hist[AUDIO_HIST_BUCKETS];
        memset(hist, 0, sizeof(hist));
        size_t lrs = 0;
        uint64_t lseq = 0, ln = 0;
        unsigned lseg = 0;
        uint8_t *ld = _ac_copy_range(AUDIO_CAPTURE_STREAM_LEVELS, 0, UINT64_MAX, &lrs, &lseq, &ln, &lseg);
        uint64_t edges = 0;
        bool have_prev = false;
        uint64_t prev_t = 0;
        for (uint64_t i = 0; i < ln && lrs == sizeof(st_AUDIO_CAPTURE_LEVEL_REC); i++) {
            const st_AUDIO_CAPTURE_LEVEL_REC *l = (const st_AUDIO_CAPTURE_LEVEL_REC *) (ld + i * lrs);
            if (l->src != 0 || l->frame < first) continue;
            if (l->frame > last) break;
            edges++;
            if (have_prev && l->t > prev_t) audio_hist_add(hist, (double) (l->t - prev_t) * 1e6 / (double) GDGCLK_BASE);
            prev_t = l->t;
            have_prev = true;
        }
        g_free(ld);
        double share = 0.0;
        double secs = (double) ticks_total / (double) GDGCLK_BASE;
        JsonObject *c0 = json_object_new();
        json_object_set_int_member(c0, "edges", (gint64) edges);
        JsonArray *ha = json_array_new();
        for (unsigned b = 0; b < AUDIO_HIST_BUCKETS; b++) {
            if (!hist[b]) continue;
            JsonArray *e = json_array_new();
            json_array_add_int_element(e, (gint64) audio_hist_bucket_us(b));
            json_array_add_int_element(e, (gint64) hist[b]);
            json_array_add_array_element(ha, e);
        }
        json_object_set_array_member(c0, "edge_interval_hist_us", ha);
        json_object_set_string_member(c0, "likely", audio_ctc0_guess(hist, edges, secs, &share));
        json_object_set_double_member(c0, "dominant_share", _ac_round(share, 3));
        json_object_set_string_member(c0, "likely_rule",
            "heuristic estimate: tone = two adjacent buckets hold >= 80 % of the intervals, "
            "pcm = otherwise with >= 2000 edges/s, else irregular");
        json_object_set_boolean_member(c0, "levels_complete", lseq == 0);
        json_object_set_object_member(resp, "ctc0", c0);
    }

    /* Reference vs SDL cesta (potřebuje oba PCM ringy). */
    const st_AUDIO_CAPTURE_STREAM_INFO *pr = &st.streams[AUDIO_CAPTURE_STREAM_PCM_REFERENCE];
    const st_AUDIO_CAPTURE_STREAM_INFO *ps = &st.streams[AUDIO_CAPTURE_STREAM_PCM_SDL];
    if (pr->present && ps->present) {
        uint64_t spf = AUDIO_CAPTURE_WAV_RATE / VIDEO_SCREENS_PER_SEC;
        uint64_t s0 = _ac_sample_at(fr[0].t, st.origin_ticks);
        uint64_t s1 = _ac_sample_at(fr[nfr - 1].t + fr[nfr - 1].len_ticks, st.origin_ticks);
        if (s1 > s0 + AC_COMPARE_WINDOW) s1 = s0 + AC_COMPARE_WINDOW;
        uint64_t max_lag = (lag_frames >= 0 ? (uint64_t) lag_frames : (uint64_t) max_lag_frames) * spf;
        size_t r1 = 0, r2 = 0;
        uint64_t q1 = 0, n1 = 0, q2 = 0, n2 = 0;
        unsigned g1 = 0, g2 = 0;
        uint8_t *ref = _ac_copy_range(AUDIO_CAPTURE_STREAM_PCM_REFERENCE, s0, s1, &r1, &q1, &n1, &g1);
        uint8_t *sdl = _ac_copy_range(AUDIO_CAPTURE_STREAM_PCM_SDL, q1, q1 + n1 + max_lag, &r2, &q2, &n2, &g2);
        JsonObject *rv = json_object_new();
        st_AUDIO_COMPARE cmp;
        bool ok = ref && sdl && n1 > 0 && q2 == q1;
        if (ok && lag_frames >= 0) {
            audio_diff((const int16_t *) ref, n1, (const int16_t *) sdl, n2, (int) max_lag, 0, n1, &cmp);
            ok = cmp.compared > 0;
        } else if (ok) {
            ok = audio_compare((const int16_t *) ref, n1, (const int16_t *) sdl, n2, (int) max_lag, 0, &cmp);
        }
        if (ok) {
            json_object_set_int_member(rv, "lag_samples", cmp.lag);
            json_object_set_double_member(rv, "lag_frames", _ac_round((double) cmp.lag / (double) spf, 2));
            json_object_set_boolean_member(rv, "lag_searched", lag_frames < 0);
            /* Bez nástupu nebo změny hlasitosti v okně (jen ustálený tón) posun
             * z dat určit nejde; lag_samples pak může být o periody tónu vedle. */
            json_object_set_boolean_member(rv, "lag_ambiguous", lag_frames < 0 && cmp.lag_ambiguous);
            json_object_set_int_member(rv, "compared_samples", (gint64) cmp.compared);
            json_object_set_int_member(rv, "from_sample", (gint64) q1);
            json_object_set_double_member(rv, "diff_rms", _ac_round(cmp.diff_rms, 5));
            json_object_set_double_member(rv, "diff_max", _ac_round(cmp.diff_max, 5));
            json_object_set_double_member(rv, "reference_rms", _ac_round(cmp.a_rms, 5));
            /* Nejhorší snímky porovnaného úseku. */
            JsonArray *worst = json_array_new();
            double best[5] = { -1, -1, -1, -1, -1 };
            uint64_t bf[5] = { 0 };
            for (uint64_t i = 0; i < nfr; i++) {
                uint64_t a = _ac_sample_at(fr[i].t, st.origin_ticks);
                if (a < q1) continue;
                if (a >= q1 + cmp.compared) break;
                st_AUDIO_COMPARE d;
                audio_diff((const int16_t *) ref, n1, (const int16_t *) sdl, n2, cmp.lag, (size_t) (a - q1), spf, &d);
                for (int k = 0; k < 5; k++) {
                    if (d.compared && d.diff_rms > best[k]) {
                        for (int m = 4; m > k; m--) { best[m] = best[m - 1]; bf[m] = bf[m - 1]; }
                        best[k] = d.diff_rms;
                        bf[k] = first + i;
                        break;
                    }
                }
            }
            for (int k = 0; k < 5 && best[k] >= 0; k++) {
                JsonArray *w = json_array_new();
                json_array_add_int_element(w, (gint64) bf[k]);
                json_array_add_double_element(w, _ac_round(best[k], 5));
                json_array_add_array_element(worst, w);
            }
            json_object_set_array_member(rv, "worst_frames", worst);
        } else {
            json_object_set_string_member(rv, "error", "reference and output path samples do not overlap in this range");
        }
        json_object_set_int_member(rv, "sdl_missing_blocks",
                                   (gint64) ((st.frames > st.sdl_blocks) ? st.frames - st.sdl_blocks : 0));
        json_object_set_object_member(resp, "ref_vs_sdl", rv);
        g_free(ref);
        g_free(sdl);
    } else {
        json_object_set_null_member(resp, "ref_vs_sdl");
        json_object_set_string_member(resp, "ref_vs_sdl_note",
                                      "needs pcm=[\"reference\", \"sdl\"] at emu_audio_capture_start");
    }
    g_free(fdata);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `audio_capture_pcm` handler - výřez PCM z paměti zachycení.
 *
 * Parametry: `source` ("reference" výchozí | "sdl"), začátek `sample_from`
 * (>= 0, vzorky 44100 Hz od začátku zachycení), jinak `frame_from`
 * (výchozí 0); délka `samples` (> 0), jinak `frames` (výchozí 25); `rate`
 * (1000..44100, výchozí 11025), `stereo` (výchozí false), `encoding`
 * ("wav_b64" výchozí | "pcm_b64"), `max_bytes` (1024..4 MiB, výchozí
 * 32768 = strop dat před base64).
 *
 * Převzorkování je průměrování (analytický náhled); `rate` 44100 se
 * `stereo` vrací bajtově totéž co reference.wav / sdl.wav. Response:
 * source, segment, live, rate, channels, from_sample, samples, frames_out,
 * next_sample, end_of_data, truncated, available [first, next), gap,
 * encoding, data_b64.
 */
static en_MCP_DISPATCH_RESULT _handle_audio_capture_pcm(const st_JSONL_MESSAGE *req,
                                                        char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int hs = _videorec_member(obj, "source", G_TYPE_STRING);
    int he = _videorec_member(obj, "encoding", G_TYPE_STRING);
    int hst = _videorec_member(obj, "stereo", G_TYPE_BOOLEAN);
    const char *inames[] = { "sample_from", "frame_from", "samples", "frames", "rate", "max_bytes" };
    gint64 iv[] = { -1, 0, 0, 25, 11025, AC_PCM_DEFAULT_MAX_BYTES };
    bool bad = (hs < 0 || he < 0 || hst < 0);
    for (int i = 0; i < 6 && !bad; i++) {
        int h = _videorec_member(obj, inames[i], G_TYPE_INT64);
        if (h < 0) bad = true;
        else if (h) iv[i] = json_object_get_int_member(obj, inames[i]);
    }
    const char *source = hs ? json_object_get_string_member(obj, "source") : "reference";
    const char *enc = he ? json_object_get_string_member(obj, "encoding") : "wav_b64";
    bool stereo = hst ? json_object_get_boolean_member(obj, "stereo") : false;
    bool sdl = (g_strcmp0(source, "sdl") == 0);
    bool wav = (g_strcmp0(enc, "wav_b64") == 0);
    if (bad || (!sdl && g_strcmp0(source, "reference") != 0) || (!wav && g_strcmp0(enc, "pcm_b64") != 0)
        || iv[1] < 0 || iv[2] < 0 || iv[3] < 1 || iv[4] < 1000 || iv[4] > (gint64) AUDIO_CAPTURE_WAV_RATE
        || iv[5] < 1024 || iv[5] > (gint64) AC_PCM_MAX_MAX_BYTES) {
        return _err_response(req_id, "Invalid parameters: source reference|sdl, encoding wav_b64|pcm_b64, "
                             "stereo boolean, sample_from/frame_from >= 0, samples >= 0, frames >= 1, "
                             "rate 1000..44100, max_bytes 1024..4194304",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    unsigned rate = (unsigned) iv[4];
    size_t max_bytes = (size_t) iv[5];
    en_AUDIO_CAPTURE_STREAM stream = sdl ? AUDIO_CAPTURE_STREAM_PCM_SDL : AUDIO_CAPTURE_STREAM_PCM_REFERENCE;

    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    const st_AUDIO_CAPTURE_STREAM_INFO *si = &st.streams[stream];
    if (!st.memory || !si->present) {
        return _err_response(req_id, sdl ? "No output path PCM in the audio capture memory: start with pcm=[\"sdl\"]"
                                         : "No reference PCM in the audio capture memory: start with pcm=[\"reference\"]",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }

    /* Začátek a délka ve vzorcích 44100 Hz. */
    uint64_t spf_num = AUDIO_CAPTURE_WAV_RATE, spf_den = VIDEO_SCREENS_PER_SEC;
    uint64_t from = 0, want = 0;
    if (iv[0] >= 0) {
        from = (uint64_t) iv[0];
    } else {
        /* Začátek snímku z jeho souhrnu (přesný čas), jinak podle fps. */
        st_AUDIO_CAPTURE_STREAM_COPY c;
        audio_capture_stream_copy(AUDIO_CAPTURE_STREAM_FRAMES, (uint64_t) iv[1], 1, sizeof(st_AUDIO_CAPTURE_FRAME_REC), &c);
        if (c.data_count == 1 && c.data_seq == (uint64_t) iv[1]) {
            from = _ac_sample_at(((const st_AUDIO_CAPTURE_FRAME_REC *) c.data)->t, st.origin_ticks);
        } else {
            from = (uint64_t) iv[1] * spf_num / spf_den;
        }
        audio_capture_stream_copy_clear(&c);
    }
    want = (iv[2] > 0) ? (uint64_t) iv[2] : (uint64_t) iv[3] * spf_num / spf_den;

    unsigned ch = stereo ? 2u : 1u;
    size_t hdr = wav ? 44u : 0u;
    size_t out_max = (max_bytes > hdr) ? (max_bytes - hdr) / (2u * ch) : 0;
    uint64_t in_max = (uint64_t) out_max * AUDIO_CAPTURE_WAV_RATE / rate;
    bool truncated = false;
    if (want > in_max) {
        want = in_max;
        truncated = true;
    }

    JsonObject *resp = json_object_new();
    st_AUDIO_CAPTURE_STREAM_COPY c;
    bool okc = audio_capture_stream_copy(stream, from, want, (size_t) (want * 4u), &c);
    if (!okc) {
        audio_capture_stream_copy_clear(&c);
        json_object_unref(resp);
        return _err_response(req_id, "Not enough memory for the PCM copy", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (from < c.first_seq) {
        JsonObject *gap = json_object_new();
        json_object_set_int_member(gap, "from_sample", (gint64) from);
        json_object_set_int_member(gap, "to_sample", (gint64) c.first_seq);
        json_object_set_object_member(resp, "gap", gap);
    } else {
        json_object_set_null_member(resp, "gap");
    }
    uint64_t got = c.data_count;
    uint64_t start = c.data_count ? c.data_seq : ((from > c.next_seq) ? c.next_seq : from);
    size_t nout = audio_resample_frames((size_t) got, AUDIO_CAPTURE_WAV_RATE, rate);
    uint8_t *buf = (uint8_t *) g_malloc(hdr + nout * 2u * ch + 2u);
    if (got) audio_resample_s16((const int16_t *) c.data, (size_t) got, AUDIO_CAPTURE_WAV_RATE, rate, stereo,
                                (int16_t *) (buf + hdr));
    if (wav) audio_wav_header(buf, rate, ch, nout);
    char *b64 = g_base64_encode(buf, hdr + nout * 2u * ch);
    json_object_set_string_member(resp, "source", sdl ? "sdl" : "reference");
    json_object_set_int_member(resp, "segment", (gint64) c.segment);
    json_object_set_boolean_member(resp, "live", c.live);
    json_object_set_int_member(resp, "rate", rate);
    json_object_set_int_member(resp, "channels", ch);
    json_object_set_int_member(resp, "from_sample", (gint64) start);
    json_object_set_int_member(resp, "samples", (gint64) got);
    json_object_set_int_member(resp, "frames_out", (gint64) nout);
    json_object_set_int_member(resp, "next_sample", (gint64) (start + got));
    json_object_set_boolean_member(resp, "end_of_data", start + got >= c.next_seq);
    json_object_set_boolean_member(resp, "truncated", truncated);
    JsonArray *av = json_array_new();
    json_array_add_int_element(av, (gint64) c.first_seq);
    json_array_add_int_element(av, (gint64) c.next_seq);
    json_object_set_array_member(resp, "available", av);
    json_object_set_string_member(resp, "encoding", wav ? "wav_b64" : "pcm_b64");
    json_object_set_string_member(resp, "data_b64", b64);
    g_free(b64);
    g_free(buf);
    audio_capture_stream_copy_clear(&c);
    return _ok_response(req_id, resp, out_response);
}


/* ---------------- Záznam zápisů do paměti: ramlog (M2 debug-gaps) ---------------- */

/** @brief Nejvýš záznamů historie na buňku v ramlog_who_wrote. */
#define RAMLOG_MCP_MAX_LIMIT 1000
/** @brief Nejvýš zapisovatelů v ramlog_writers. */
#define RAMLOG_MCP_MAX_WRITERS 5000

/** @brief Stav ramlogu jako JSON objekt. */
static JsonObject *_ramlog_status_json(const st_RAMLOG_STATUS *st) {
    JsonObject *o = json_object_new();
    json_object_set_boolean_member(o, "active", st->active);
    json_object_set_boolean_member(o, "allocated", st->allocated);
    json_object_set_int_member(o, "segment", (gint64)st->segment);
    json_object_set_string_member(o, "mode", st->mode == RAMLOG_MODE_FILL ? "fill" : "ring");
    json_object_set_int_member(o, "capacity", (gint64)st->capacity);
    json_object_set_int_member(o, "first_seq", (gint64)st->first_seq);
    json_object_set_int_member(o, "next_seq", (gint64)st->next_seq);
    json_object_set_int_member(o, "total", (gint64)st->total);
    json_object_set_int_member(o, "dropped", (gint64)st->dropped);
    json_object_set_int_member(o, "overwritten", (gint64)st->overwritten);
    json_object_set_int_member(o, "memory_bytes", (gint64)st->memory_bytes);
    JsonArray *ra = json_array_new();
    for (unsigned i = 0; i < st->n_ranges; i++) {
        const st_RAMLOG_RANGE *r = &st->ranges[i];
        JsonObject *x = json_object_new();
        if (r->phys) json_object_set_string_member(x, "space", ramlog_space_name(r->space));
        json_object_set_int_member(x, "from", (gint64)r->from);
        json_object_set_int_member(x, "to", (gint64)r->to);
        json_array_add_object_element(ra, x);
    }
    json_object_set_array_member(o, "ranges", ra);
    return o;
}

/**
 * @brief Provede DBGAPI_CMD_RAMLOG.
 * @return false = příkaz se nepodařilo předat; jinak true a chyba operace v p->out_error.
 */
static bool _ramlog_submit(st_DBGAPI_RAMLOG_PARAM *p) {
    p->out_error[0] = '\0';
    bool ok = _submit_dbgapi(DBGAPI_CMD_RAMLOG, p, NULL);
    return ok || p->out_error[0];
}

/** @brief Odpověď stavu nebo chyby po DBGAPI_CMD_RAMLOG. */
static en_MCP_DISPATCH_RESULT _ramlog_status_response(int64_t req_id, st_DBGAPI_RAMLOG_PARAM *p,
                                                      char **out_response) {
    if (!_ramlog_submit(p)) {
        return _err_response(req_id, "ramlog command failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (p->out_error[0]) {
        return _err_response(req_id, p->out_error, MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, _ramlog_status_json(&p->out_status), out_response);
}

/**
 * @brief Naparsuje jeden rozsah `{from, to}` (CPU) nebo `{space, from, to}` (fyzický).
 * @return NULL OK, jinak statická chybová zpráva.
 */
static const char *_ramlog_parse_range(JsonNode *n, st_RAMLOG_RANGE *out) {
    memset(out, 0, sizeof(*out));
    if (!n || json_node_get_node_type(n) != JSON_NODE_OBJECT) return "ranges items must be objects {from, to} or {space, from, to}";
    JsonObject *o = json_node_get_object(n);
    int hf = _videorec_member(o, "from", G_TYPE_INT64);
    int ht = _videorec_member(o, "to", G_TYPE_INT64);
    int hs = _videorec_member(o, "space", G_TYPE_STRING);
    if (hf <= 0 || ht < 0 || hs < 0) return "ranges items need integer from (and optional to, space name)";
    gint64 from = json_object_get_int_member(o, "from");
    gint64 to = ht ? json_object_get_int_member(o, "to") : from;
    if (hs) {
        int sp = ramlog_space_from_name(json_object_get_string_member(o, "space"));
        if (sp < 0 || sp == RAMLOG_SPACE_ROM) return "range space must be ram, vram, pcg or memext";
        out->phys = true;
        out->space = (uint8_t)sp;
        if (from < 0 || to < from || to > 0x7FFFF) return "physical range must be 0 <= from <= to <= 7FFFFh";
    } else if (from < 0 || to < from || to > 0xFFFF) {
        return "CPU range must be 0 <= from <= to <= FFFFh";
    }
    out->from = (uint32_t)from;
    out->to = (uint32_t)to;
    return NULL;
}

/**
 * @brief `ramlog_start` handler - spustí záznam zápisů do paměti.
 *
 * Parametry (volitelné): `ranges` (pole `{from, to}` CPU adres nebo
 * `{space, from, to}` buněk prostoru ram/vram/pcg/memext, nejvýš 16;
 * chybí = celá paměť), `mode` ("ring" výchozí | "fill"), `capacity`
 * (16..16777216 záznamů, výchozí 1048576). Předchozí záznam uvolní.
 */
static en_MCP_DISPATCH_RESULT _handle_ramlog_start(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_START;
    p.cfg.mode = RAMLOG_MODE_RING;
    p.cfg.capacity = RAMLOG_DEFAULT_CAPACITY;
    int hm = _videorec_member(obj, "mode", G_TYPE_STRING);
    int hc = _videorec_member(obj, "capacity", G_TYPE_INT64);
    if (hm < 0 || hc < 0) {
        return _err_response(req_id, "Invalid parameters: mode must be a string, capacity an integer",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (hm) {
        const char *m = json_object_get_string_member(obj, "mode");
        if (g_strcmp0(m, "ring") == 0) p.cfg.mode = RAMLOG_MODE_RING;
        else if (g_strcmp0(m, "fill") == 0) p.cfg.mode = RAMLOG_MODE_FILL;
        else return _err_response(req_id, "Invalid parameters: mode must be ring or fill",
                                  MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (hc) {
        gint64 c = json_object_get_int_member(obj, "capacity");
        if (c < RAMLOG_MIN_CAPACITY || c > RAMLOG_MAX_CAPACITY) {
            return _err_response(req_id, "Invalid parameters: capacity must be 16..16777216",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        p.cfg.capacity = (uint32_t)c;
    }
    if (obj && json_object_has_member(obj, "ranges") && !json_node_is_null(json_object_get_member(obj, "ranges"))) {
        JsonNode *rn = json_object_get_member(obj, "ranges");
        if (json_node_get_node_type(rn) != JSON_NODE_ARRAY) {
            return _err_response(req_id, "Invalid parameters: ranges must be an array",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonArray *ra = json_node_get_array(rn);
        guint n = json_array_get_length(ra);
        if (n > RAMLOG_MAX_RANGES) {
            return _err_response(req_id, "Invalid parameters: at most 16 ranges",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        for (guint i = 0; i < n; i++) {
            const char *e = _ramlog_parse_range(json_array_get_element(ra, i), &p.cfg.ranges[i]);
            if (e) {
                return _trace_err_owned(req_id, g_strdup_printf("Invalid parameters: %s", e),
                                        MCP_DISPATCH_INVALID_PARAMS, out_response);
            }
        }
        p.cfg.n_ranges = n;
    }
    return _ramlog_status_response(req_id, &p, out_response);
}

/** @brief `ramlog_stop` handler - konec záznamu, data zůstanou čitelná. */
static en_MCP_DISPATCH_RESULT _handle_ramlog_stop(const st_JSONL_MESSAGE *req, char **out_response) {
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_STOP;
    return _ramlog_status_response(jsonl_msg_get_req_id(req), &p, out_response);
}

/** @brief `ramlog_clear` handler - vyprázdní ring a mapu (nový segment). */
static en_MCP_DISPATCH_RESULT _handle_ramlog_clear(const st_JSONL_MESSAGE *req, char **out_response) {
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_CLEAR;
    return _ramlog_status_response(jsonl_msg_get_req_id(req), &p, out_response);
}

/** @brief `ramlog_status` handler - stav záznamu. */
static en_MCP_DISPATCH_RESULT _handle_ramlog_status(const st_JSONL_MESSAGE *req, char **out_response) {
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_STATUS;
    return _ramlog_status_response(jsonl_msg_get_req_id(req), &p, out_response);
}

/**
 * @brief `bplog_clear` handler - vyprázdní ring výstupu akcí BP (nový segment).
 *
 * Parametry: žádné. Response: `{segment, allocated, capacity, first_seq,
 * next_seq}` po vyčištění. Nad ještě nealokovaným ringem (žádná akce
 * `log` / `stop` nezapsala) nic nemění.
 */
static en_MCP_DISPATCH_RESULT _handle_bplog_clear(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_BPLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_BPLOG_OP_CLEAR;
    if (!_submit_dbgapi(DBGAPI_CMD_BPLOG, &p, NULL)) {
        return _err_response(req_id, "bplog_clear failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "segment", (gint64)p.out_status.segment);
    json_object_set_boolean_member(o, "allocated", p.out_status.allocated ? TRUE : FALSE);
    json_object_set_int_member(o, "capacity", (gint64)p.out_status.capacity);
    json_object_set_int_member(o, "first_seq", (gint64)p.out_status.first_seq);
    json_object_set_int_member(o, "next_seq", (gint64)p.out_status.next_seq);
    return _ok_response(req_id, o, out_response);
}

/** @brief Záznam ramlogu jako JSON objekt. */
static JsonObject *_ramlog_rec_json(const st_RAMLOG_REC *r, uint64_t seq, bool has_seq) {
    JsonObject *o = json_object_new();
    char b[8];
    if (has_seq) json_object_set_int_member(o, "seq", (gint64)seq);
    json_object_set_int_member(o, "t", (gint64)r->t);
    json_object_set_int_member(o, "frame", (gint64)r->frame);
    g_snprintf(b, sizeof(b), "%04X", r->insn_pc);
    json_object_set_string_member(o, "pc", b);
    if (r->flags & RAMLOG_FLAG_NO_CPU) {
        json_object_set_null_member(o, "addr");
    } else {
        g_snprintf(b, sizeof(b), "%04X", r->cpu_addr);
        json_object_set_string_member(o, "addr", b);
    }
    g_snprintf(b, sizeof(b), "%02X", r->old_value);
    json_object_set_string_member(o, "old", b);
    g_snprintf(b, sizeof(b), "%02X", r->new_value);
    json_object_set_string_member(o, "new", b);
    json_object_set_string_member(o, "src", ramlog_src_name(r->src));
    /* T-stav uvnitř instrukce v okamžiku zápisu (t = začátek instrukce). */
    if (r->src == RAMLOG_SRC_INSN) json_object_set_int_member(o, "tin", r->tin);
    if (r->flags & RAMLOG_FLAG_SUMMARY) json_object_set_int_member(o, "count", r->count ? r->count : 65536);
    if (r->flags & RAMLOG_FLAG_UNCHANGED) json_object_set_boolean_member(o, "unchanged", TRUE);
    if (r->flags & RAMLOG_FLAG_SUPPRESSED) json_object_set_boolean_member(o, "suppressed", TRUE);
    return o;
}

/**
 * @brief Naparsuje cíl dotazu: `addr` (CPU) nebo `space` + `offset`.
 * @return NULL OK, jinak statická zpráva.
 */
static const char *_ramlog_parse_cell(JsonObject *obj, st_DBGAPI_RAMLOG_PARAM *p) {
    int ha = _videorec_member(obj, "addr", G_TYPE_INT64);
    int hs = _videorec_member(obj, "space", G_TYPE_STRING);
    int ho = _videorec_member(obj, "offset", G_TYPE_INT64);
    if (ha < 0 || hs < 0 || ho < 0) return "addr and offset must be integers, space a string";
    if (ha) {
        gint64 a = json_object_get_int_member(obj, "addr");
        if (a < 0 || a > 0xFFFF) return "addr must be 0..FFFFh";
        p->by_cpu = true;
        p->cpu_from = (uint16_t)a;
        return NULL;
    }
    if (!hs || !ho) return "give addr (CPU address) or space + offset";
    int sp = ramlog_space_from_name(json_object_get_string_member(obj, "space"));
    gint64 off = json_object_get_int_member(obj, "offset");
    if (sp < 0) return "space must be ram, vram, pcg, memext or rom";
    if (off < 0 || off > 0x7FFFF) return "offset must be 0..7FFFFh";
    p->space = (uint8_t)sp;
    p->off_from = (uint32_t)off;
    return NULL;
}

/**
 * @brief `ramlog_who_wrote` handler - kdo zapsal buňku (mapa + historie z ringu).
 *
 * Parametry: `addr` (CPU adresa v aktuálním mapování; grafická VRAM MZ-800
 * = 4 plány) nebo `space` + `offset`; `limit` (0..1000, výchozí 20 záznamů
 * historie na buňku), `all` (i nezměněné a potlačené zápisy, výchozí false).
 */
static en_MCP_DISPATCH_RESULT _handle_ramlog_who_wrote(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_WHO_WROTE;
    const char *e = _ramlog_parse_cell(obj, &p);
    int hl = _videorec_member(obj, "limit", G_TYPE_INT64);
    int hall = _videorec_member(obj, "all", G_TYPE_BOOLEAN);
    gint64 limit = hl > 0 ? json_object_get_int_member(obj, "limit") : 20;
    if (!e && (hl < 0 || hall < 0)) e = "limit must be an integer, all a boolean";
    if (!e && (limit < 0 || limit > RAMLOG_MCP_MAX_LIMIT)) e = "limit must be 0..1000";
    if (e) {
        return _trace_err_owned(req_id, g_strdup_printf("Invalid parameters: %s", e),
                                MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    p.include_unchanged = hall > 0 && json_object_get_boolean_member(obj, "all");
    p.limit = (size_t)limit;
    if (p.limit) {
        p.out_recs = g_new0(st_RAMLOG_REC, p.limit * RAMLOG_TARGETS_MAX);
        p.out_seqs = g_new0(uint64_t, p.limit * RAMLOG_TARGETS_MAX);
    }
    if (!_ramlog_submit(&p) || p.out_error[0]) {
        g_free(p.out_recs);
        g_free(p.out_seqs);
        return _err_response(req_id, p.out_error[0] ? p.out_error : "ramlog command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active", p.out_status.active);
    json_object_set_int_member(resp, "segment", (gint64)p.out_status.segment);
    json_object_set_boolean_member(resp, "mmio", p.out_mmio);
    JsonArray *cells = json_array_new();
    for (unsigned i = 0; i < p.out_n_cells; i++) {
        const st_DBGAPI_RAMLOG_CELL *c = &p.out_cells[i];
        JsonObject *co = json_object_new();
        json_object_set_string_member(co, "space", ramlog_space_name(c->space));
        json_object_set_int_member(co, "offset", (gint64)c->offset);
        if (c->has_plane) json_object_set_int_member(co, "plane", c->plane);
        if (c->has_last) {
            JsonObject *lo = json_object_new();
            char b[8];
            g_snprintf(b, sizeof(b), "%04X", c->last.insn_pc);
            json_object_set_int_member(lo, "seq", (gint64)(c->last.seq1 - 1u));
            json_object_set_string_member(lo, "pc", b);
            json_object_set_string_member(lo, "src", ramlog_src_name(c->last.src));
            json_object_set_boolean_member(lo, "in_ring",
                (uint64_t)(c->last.seq1 - 1u) >= p.out_status.first_seq
                && (uint64_t)(c->last.seq1 - 1u) < p.out_status.next_seq);
            json_object_set_object_member(co, "last", lo);
        } else {
            json_object_set_null_member(co, "last");
        }
        JsonArray *hist = json_array_new();
        for (size_t k = 0; k < c->n_recs; k++) {
            json_array_add_object_element(hist, _ramlog_rec_json(&p.out_recs[i * p.limit + k],
                                                                 p.out_seqs[i * p.limit + k], true));
        }
        json_object_set_array_member(co, "history", hist);
        json_array_add_object_element(cells, co);
    }
    json_object_set_array_member(resp, "cells", cells);
    g_free(p.out_recs);
    g_free(p.out_seqs);
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `ramlog_writers` handler - kdo všechno zapisoval do rozsahu (souhrn podle instrukce).
 *
 * Parametry: `from` / `to` (CPU adresy zápisu v okamžiku zápisu) nebo
 * `space` + `from` / `to` (buňky prostoru); `limit` (1..5000, výchozí 50),
 * `all` (i nezměněné a potlačené zápisy).
 */
static en_MCP_DISPATCH_RESULT _handle_ramlog_writers(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_WRITERS;
    int hf = _videorec_member(obj, "from", G_TYPE_INT64);
    int ht = _videorec_member(obj, "to", G_TYPE_INT64);
    int hs = _videorec_member(obj, "space", G_TYPE_STRING);
    int hl = _videorec_member(obj, "limit", G_TYPE_INT64);
    int hall = _videorec_member(obj, "all", G_TYPE_BOOLEAN);
    const char *e = NULL;
    if (hf <= 0 || ht < 0 || hs < 0 || hl < 0 || hall < 0) e = "from (integer) is required; to, limit integers, space a string, all a boolean";
    gint64 from = hf > 0 ? json_object_get_int_member(obj, "from") : 0;
    gint64 to = ht > 0 ? json_object_get_int_member(obj, "to") : from;
    gint64 limit = hl > 0 ? json_object_get_int_member(obj, "limit") : 50;
    if (!e && (limit < 1 || limit > RAMLOG_MCP_MAX_WRITERS)) e = "limit must be 1..5000";
    if (!e && hs) {
        int sp = ramlog_space_from_name(json_object_get_string_member(obj, "space"));
        if (sp < 0) e = "space must be ram, vram, pcg, memext or rom";
        else if (from < 0 || to < from || to > 0x7FFFF) e = "physical range must be 0 <= from <= to <= 7FFFFh";
        p.space = (uint8_t)(sp < 0 ? 0 : sp);
        p.off_from = (uint32_t)from;
        p.off_to = (uint32_t)to;
    } else if (!e) {
        if (from < 0 || to < from || to > 0xFFFF) e = "CPU range must be 0 <= from <= to <= FFFFh";
        p.by_cpu = true;
        p.cpu_from = (uint16_t)from;
        p.cpu_to = (uint16_t)to;
    }
    if (e) {
        return _trace_err_owned(req_id, g_strdup_printf("Invalid parameters: %s", e),
                                MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    p.include_unchanged = hall > 0 && json_object_get_boolean_member(obj, "all");
    p.limit = (size_t)limit;
    p.out_writers = g_new0(st_RAMLOG_WRITER, p.limit);
    if (!_ramlog_submit(&p) || p.out_error[0]) {
        g_free(p.out_writers);
        return _err_response(req_id, p.out_error[0] ? p.out_error : "ramlog command failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "active", p.out_status.active);
    json_object_set_int_member(resp, "segment", (gint64)p.out_status.segment);
    json_object_set_int_member(resp, "total", (gint64)p.out_total_writers);
    json_object_set_boolean_member(resp, "truncated", p.out_total_writers > p.out_n_writers);
    JsonArray *wa = json_array_new();
    for (size_t i = 0; i < p.out_n_writers; i++) {
        const st_RAMLOG_WRITER *w = &p.out_writers[i];
        JsonObject *wo = json_object_new();
        char b[8];
        g_snprintf(b, sizeof(b), "%04X", w->insn_pc);
        json_object_set_string_member(wo, "pc", b);
        json_object_set_string_member(wo, "src", ramlog_src_name(w->src));
        json_object_set_int_member(wo, "count", (gint64)w->count);
        json_object_set_int_member(wo, "first_seq", (gint64)w->first_seq);
        json_object_set_int_member(wo, "last_seq", (gint64)w->last_seq);
        json_array_add_object_element(wa, wo);
    }
    json_object_set_array_member(resp, "writers", wa);
    g_free(p.out_writers);
    return _ok_response(req_id, resp, out_response);
}


/* ------------------------------------------------------------------ */
/* Freeze Bytes a Memory Search (memsearch, M3 debug-gaps)             */
/* ------------------------------------------------------------------ */

/**
 * @brief Druh regionu podle názvu z `regions_list` (`_region_kind_to_str`).
 * @param s Název ("ram", "memext_ram", ...).
 * @return en_REGION_KIND, nebo -1 pro neznámý název.
 */
static int _region_kind_from_str(const char *s) {
    if (!s) return -1;
    for (int k = 0; k < (int)REGION_KIND_COUNT; k++) {
        if (strcmp(_region_kind_to_str(k), s) == 0) return k;
    }
    return -1;
}

/**
 * @brief Plný výčet regionů přes DBGAPI_CMD_REGIONS_ENUMERATE.
 * @param[out] out_count Počet regionů.
 * @return Kopie výčtu (uvolnit g_free), NULL = prázdný výčet nebo chyba.
 */
static st_REGION_DESC *_ms_regions(int *out_count) {
    st_DBGAPI_REGIONS_ENUM_PARAM param = {
        .out = NULL, .max_count = 0, .out_count = 0, .out_dup = NULL,
    };
    *out_count = 0;
    if (!_submit_dbgapi(DBGAPI_CMD_REGIONS_ENUMERATE, &param, NULL)) {
        g_free(param.out_dup);
        return NULL;
    }
    if (!param.out_dup) return NULL;
    *out_count = param.out_count;
    return (st_REGION_DESC *)param.out_dup;
}

/**
 * @brief Najde region ve výčtu podle (kind, sub_id).
 * @return Ukazatel do @p regs, nebo NULL.
 */
static const st_REGION_DESC *_ms_find_region(const st_REGION_DESC *regs, int count,
                                             int kind, int sub_id) {
    for (int i = 0; i < count; i++) {
        if ((int)regs[i].kind == kind && regs[i].sub_id == sub_id) return &regs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* state_observe (observe-cas, M10 debug-gaps)                         */
/* ------------------------------------------------------------------ */

/** @brief Nejdelší štítek okna `name` (znaků bez nuly). */
#define MCP_OBSERVE_NAME_MAX 64

/**
 * @brief Části stavu přijímané v parametru `hashes` (bez `screen`, ten má `frame_hash`).
 */
static const struct {
    const char *name;           /**< Jméno v MCP (= state_hash_part_name()). */
    en_STATE_HASH_PART part;    /**< Část state_hash. */
} s_observe_hash_parts[] = {
    { "cpu",    STATE_HASH_CPU },
    { "ram",    STATE_HASH_RAM },
    { "vram",   STATE_HASH_VRAM },
    { "memext", STATE_HASH_MEMEXT },
    { "pcg",    STATE_HASH_PCG },
};

/** @brief Sekce parametru `include` v pořadí bitů masky. */
static const char *const s_observe_sections[] = { "raster", "im2", "last_instr", "memory_map" };

/**
 * @brief Údaje okna `state_observe`, které potřebuje jen dispatch vlákno.
 *
 * Doplňuje st_DBGAPI_OBSERVE_WINDOW o štítek a volbu dat.
 */
typedef struct st_MCP_OBSERVE_WIN_META {
    char  name[MCP_OBSERVE_NAME_MAX + 1]; /**< Štítek okna, prázdný = bez štítku. */
    bool  want_data;                      /**< true = vrátit i `data_b64`. */
} st_MCP_OBSERVE_WIN_META;

/**
 * @brief Celé číslo z parametrů, přísně (jen JSON integer).
 *
 * @param obj Objekt parametrů.
 * @param key Klíč.
 * @param def Výchozí hodnota pro chybějící klíč.
 * @param[out] out Hodnota.
 * @return false = klíč je přítomen, ale není celé číslo.
 */
static bool _observe_int(JsonObject *obj, const char *key, gint64 def, gint64 *out) {
    *out = def;
    if (!json_object_has_member(obj, key)) return true;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) {
        return false;
    }
    *out = json_node_get_int(n);
    return true;
}

/**
 * @brief Přečte jedno okno z parametru `windows[i]`.
 *
 * Forma CPU: `{addr, len}`, forma regionu: `{region: {kind, sub_id},
 * offset, length}`; volitelně `name` a `want_data`.
 *
 * @param node Prvek pole `windows`.
 * @param def_want_data Výchozí `want_data` (globální parametr).
 * @param[out] w Okno pro emu vlákno (bez `buf`).
 * @param[out] meta Štítek a volba dat.
 * @return NULL = OK, jinak statický popis chyby (anglicky, do textu chyby).
 */
static const char *_observe_parse_window(JsonNode *node, bool def_want_data,
                                         st_DBGAPI_OBSERVE_WINDOW *w,
                                         st_MCP_OBSERVE_WIN_META *meta) {
    memset(w, 0, sizeof(*w));
    memset(meta, 0, sizeof(*meta));
    meta->want_data = def_want_data;
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return "window must be an object";
    JsonObject *o = json_node_get_object(node);
    if (json_object_has_member(o, "name")) {
        JsonNode *n = json_object_get_member(o, "name");
        if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) {
            return "name must be a string";
        }
        const char *nm = json_node_get_string(n);
        if (strlen(nm) > MCP_OBSERVE_NAME_MAX) return "name is longer than 64 characters";
        g_strlcpy(meta->name, nm, sizeof(meta->name));
    }
    if (json_object_has_member(o, "want_data")) {
        JsonNode *n = json_object_get_member(o, "want_data");
        if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) {
            return "want_data must be a boolean";
        }
        meta->want_data = json_node_get_boolean(n) ? true : false;
    }
    bool has_addr = json_object_has_member(o, "addr");
    bool has_region = json_object_has_member(o, "region");
    if (has_addr == has_region) return "a window needs exactly one of addr or region";
    gint64 a, l;
    if (has_addr) {
        if (!_observe_int(o, "addr", -1, &a) || !_observe_int(o, "len", -1, &l)) {
            return "addr and len must be integers";
        }
        if (a < 0 || a > 0xFFFF) return "addr must be in range 0..65535";
        if (l < 1 || l > (gint64)DBGAPI_OBSERVE_MAX_TOTAL) return "len must be in range 1..65536";
        if (a + l > 0x10000) return "addr + len exceeds 0x10000";
        w->by_region = 0;
        w->start = (uint32_t)a;
        w->len = (uint32_t)l;
        return NULL;
    }
    JsonNode *rn = json_object_get_member(o, "region");
    if (!rn || !JSON_NODE_HOLDS_OBJECT(rn)) return "region must be an object {kind, sub_id}";
    JsonObject *ro = json_node_get_object(rn);
    JsonNode *kn = json_object_get_member(ro, "kind");
    if (!kn || !JSON_NODE_HOLDS_VALUE(kn) || json_node_get_value_type(kn) != G_TYPE_STRING) {
        return "region.kind must be a string";
    }
    int kind = _region_kind_from_str(json_node_get_string(kn));
    if (kind < 0) return "unknown region kind (see regions_list)";
    gint64 sub;
    if (!_observe_int(ro, "sub_id", 0, &sub) || sub < 0 || sub > G_MAXINT) {
        return "region.sub_id must be a non-negative integer";
    }
    if (!_observe_int(o, "offset", 0, &a) || !_observe_int(o, "length", -1, &l)) {
        return "offset and length must be integers";
    }
    if (a < 0 || a > 0xFFFFFFFFLL) return "offset must be in range 0..0xFFFFFFFF";
    if (l < 1 || l > (gint64)DBGAPI_OBSERVE_MAX_TOTAL) return "length must be in range 1..65536";
    w->by_region = 1;
    w->region_kind = kind;
    w->sub_id = (int)sub;
    w->start = (uint32_t)a;
    w->len = (uint32_t)l;
    return NULL;
}

/**
 * @brief Přečte pole jmen a převede ho na masku podle tabulky.
 *
 * @param obj Parametry.
 * @param key Klíč pole (`include`, `hashes`); chybějící klíč = prázdná maska.
 * @param names Tabulka povolených jmen.
 * @param n_names Počet jmen (nejvýš 32).
 * @param[out] mask Bit k = jméno names[k] je v poli.
 * @return false = hodnota není pole řetězců nebo obsahuje neznámé jméno.
 */
static bool _observe_name_list(JsonObject *obj, const char *key,
                               const char *const *names, unsigned n_names,
                               uint32_t *mask) {
    *mask = 0;
    if (!json_object_has_member(obj, key)) return true;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || !JSON_NODE_HOLDS_ARRAY(n)) return false;
    JsonArray *arr = json_node_get_array(n);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonNode *e = json_array_get_element(arr, i);
        if (!JSON_NODE_HOLDS_VALUE(e) || json_node_get_value_type(e) != G_TYPE_STRING) return false;
        const char *v = json_node_get_string(e);
        unsigned k = 0;
        while (k < n_names && strcmp(names[k], v) != 0) k++;
        if (k == n_names) return false;
        *mask |= 1u << k;
    }
    return true;
}

/**
 * @brief Doplní do odpovědi `state_observe` objekt `memory_map`.
 *
 * Schéma jako `get_memory_map` (`platform`, `slots` po 4 KiB s `source`,
 * `ro_rw`, `slot_offset`), u každého slotu navíc `kind`: druh stránky
 * podle aktuálního mapování (en_MEMMAP_REGION_KIND, stejný údaj, podle
 * kterého `mem_write` kontroluje zapisovatelnost).
 *
 * @param data Odpověď.
 * @param p Vyplněný výsledek STATE_OBSERVE s want_memory_map.
 */
static void _observe_memory_map_to_json(JsonObject *data,
                                        const st_DBGAPI_STATE_OBSERVE_PARAM *p) {
    JsonObject *mo = json_object_new();
    json_object_set_string_member(mo, "platform", p->memory_map.platform);
    JsonArray *sa = json_array_new();
    for (int i = 0; i < p->memory_map.slot_count && i < 16; i++) {
        const st_DBGAPI_MEMORY_MAP_SLOT *sl = &p->memory_map.slots[i];
        JsonObject *so = json_object_new();
        json_object_set_int_member(so, "addr_start", (gint64)sl->addr_start);
        json_object_set_int_member(so, "addr_end", (gint64)sl->addr_end);
        json_object_set_string_member(so, "kind", _memmap_kind_str(p->page_kind[i]));
        json_object_set_string_member(so, "source", _memory_source_str(sl->source));
        json_object_set_string_member(so, "ro_rw", sl->ro_rw ? "rw" : "r");
        json_object_set_int_member(so, "slot_offset", (gint64)sl->slot_offset);
        json_array_add_object_element(sa, so);
    }
    json_object_set_array_member(mo, "slots", sa);
    json_object_set_object_member(data, "memory_map", mo);
}

/**
 * @brief Převede okna `state_observe` na pole odpovědi.
 *
 * Okno nese echo vstupu (`name`, `addr`/`len` nebo `region`/`offset`/
 * `length`), `sha256` přečtených bajtů (stejný jako `mem_read` /
 * `region_read` téhož rozsahu) a `data_b64` s want_data. Nedostupné okno
 * má délku 0, `error: "region_unavailable"` a žádný hash.
 *
 * @param data Odpověď (přidá `windows` a `complete`).
 * @param wins Okna po návratu z emu vlákna.
 * @param meta Štítky a volby dat oken.
 * @param n Počet oken.
 */
static void _observe_windows_to_json(JsonObject *data, const st_DBGAPI_OBSERVE_WINDOW *wins,
                                     const st_MCP_OBSERVE_WIN_META *meta, guint n) {
    bool complete = true;
    JsonArray *wa = json_array_new();
    for (guint i = 0; i < n; i++) {
        const st_DBGAPI_OBSERVE_WINDOW *w = &wins[i];
        JsonObject *wo = json_object_new();
        if (meta[i].name[0]) json_object_set_string_member(wo, "name", meta[i].name);
        if (w->by_region) {
            JsonObject *ro = json_object_new();
            json_object_set_string_member(ro, "kind", _region_kind_to_str(w->region_kind));
            json_object_set_int_member(ro, "sub_id", w->sub_id);
            json_object_set_object_member(wo, "region", ro);
            json_object_set_int_member(wo, "offset", (gint64)w->start);
        } else {
            json_object_set_int_member(wo, "addr", (gint64)w->start);
        }
        const char *len_key = w->by_region ? "length" : "len";
        if (w->status == DBGAPI_OBSERVE_WINDOW_OK) {
            json_object_set_int_member(wo, len_key, (gint64)w->len);
            _read_result_to_json(wo, w->buf, (gsize)w->len, meta[i].want_data);
        } else {
            json_object_set_int_member(wo, len_key, 0);
            json_object_set_string_member(wo, "error", "region_unavailable");
            complete = false;
        }
        json_array_add_object_element(wa, wo);
    }
    json_object_set_array_member(data, "windows", wa);
    json_object_set_boolean_member(data, "complete", complete ? TRUE : FALSE);
}

/**
 * @brief Výkonná část `state_observe` nad objektem parametrů.
 *
 * Sdílí ji handler `state_observe` a `replay_goto` (výstup `observe`).
 *
 * @param req_id       ID požadavku (do chybové odpovědi).
 * @param obj          Parametry jako `data` požadavku `state_observe`.
 * @param[out] out_data Při úspěchu datový objekt odpovědi (vlastní volající).
 * @param[out] out_response Při chybě chybová odpověď (volající `free()`).
 * @return MCP_DISPATCH_OK (out_data platný), jinak chybový kód (out_response).
 */
static en_MCP_DISPATCH_RESULT _state_observe_exec(int64_t req_id, JsonObject *obj,
                                                  JsonObject **out_data, char **out_response) {
    *out_data = NULL;
    if (!obj) {
        return _err_response(req_id, "Invalid parameters: data must be an object",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonNode *wn = json_object_get_member(obj, "windows");
    if (!wn || !JSON_NODE_HOLDS_ARRAY(wn)) {
        return _err_response(req_id, "Invalid parameters: windows must be an array",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonArray *warr = json_node_get_array(wn);
    guint n_win = json_array_get_length(warr);
    if (n_win > DBGAPI_OBSERVE_MAX_WINDOWS) {
        return _err_response(req_id, "Invalid parameters: at most 16 windows",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool want_data = true;
    if (!_parse_want_data(obj, &want_data)) {
        return _err_response(req_id, "Invalid parameters: want_data must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint32_t sections = 0;
    if (!_observe_name_list(obj, "include", s_observe_sections,
                            G_N_ELEMENTS(s_observe_sections), &sections)) {
        return _err_response(req_id, "Invalid parameters: include must be a list of "
                             "raster, im2, last_instr, memory_map",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *hash_names[G_N_ELEMENTS(s_observe_hash_parts)];
    for (unsigned i = 0; i < G_N_ELEMENTS(s_observe_hash_parts); i++) {
        hash_names[i] = s_observe_hash_parts[i].name;
    }
    uint32_t hash_sel = 0;
    if (!_observe_name_list(obj, "hashes", hash_names, G_N_ELEMENTS(hash_names), &hash_sel)) {
        return _err_response(req_id, "Invalid parameters: hashes must be a list of "
                             "cpu, ram, vram, memext, pcg",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_STATE_OBSERVE_PARAM *p = g_new0(st_DBGAPI_STATE_OBSERVE_PARAM, 1);
    st_DBGAPI_OBSERVE_WINDOW *wins = g_new0(st_DBGAPI_OBSERVE_WINDOW, n_win ? n_win : 1);
    st_MCP_OBSERVE_WIN_META *meta = g_new0(st_MCP_OBSERVE_WIN_META, n_win ? n_win : 1);
    uint8_t *bufs = NULL;
    en_MCP_DISPATCH_RESULT rc;
    gchar *msg = NULL;
    uint64_t total = 0;
    for (guint i = 0; i < n_win && !msg; i++) {
        const char *perr = _observe_parse_window(json_array_get_element(warr, i), want_data,
                                                 &wins[i], &meta[i]);
        if (perr) msg = g_strdup_printf("Invalid parameters: windows[%u]: %s", i, perr);
        total += wins[i].len;
    }
    if (!msg && total > DBGAPI_OBSERVE_MAX_TOTAL) {
        msg = g_strdup("Invalid parameters: the sum of window lengths exceeds 65536 bytes");
    }
    if (msg) {
        rc = _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS, out_response);
        goto out;
    }
    bufs = g_malloc0(total ? (gsize)total : 1);
    uint64_t pos = 0;
    for (guint i = 0; i < n_win; i++) {
        wins[i].buf = bufs + pos;
        pos += wins[i].len;
    }
    p->which = ((sections & 1u) ? DBGAPI_CPU_PANEL_WANT_RASTER : 0)
             | ((sections & 2u) ? DBGAPI_CPU_PANEL_WANT_IM2 : 0)
             | ((sections & 4u) ? DBGAPI_CPU_PANEL_WANT_LAST_INSTR : 0);
    p->want_memory_map = (sections & 8u) ? 1 : 0;
    for (unsigned i = 0; i < G_N_ELEMENTS(s_observe_hash_parts); i++) {
        if (hash_sel & (1u << i)) p->hash_mask |= 1u << (unsigned)s_observe_hash_parts[i].part;
    }
    p->windows = n_win ? wins : NULL;
    p->window_count = n_win;

    if (!_submit_dbgapi(DBGAPI_CMD_STATE_OBSERVE, p, NULL)) {
        rc = _err_response(req_id, "STATE_OBSERVE failed",
                           MCP_DISPATCH_EMU_ERROR, out_response);
        goto out;
    }
    for (guint i = 0; i < n_win && !msg; i++) {
        if (wins[i].status == DBGAPI_OBSERVE_WINDOW_OUT_OF_RANGE) {
            msg = g_strdup_printf("Invalid parameters: windows[%u]: offset + length "
                                  "exceeds the region size %u", i, wins[i].region_size);
        }
    }
    if (msg) {
        rc = _err_response(req_id, msg, MCP_DISPATCH_INVALID_PARAMS, out_response);
        goto out;
    }

    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "run_seq", (gint64)p->run_seq);
    json_object_set_boolean_member(data, "running", p->running ? TRUE : FALSE);
    json_object_set_boolean_member(data, "stable", p->running ? FALSE : TRUE);
    /* Revize stavu z okamžiku čtení (výstup slotu, state_rev.h). */
    if (s_submit_track.have_rev) {
        json_object_set_int_member(data, "state_rev", (gint64)s_submit_track.state_rev);
    }
    json_object_set_string_member(data, "drain", p->running ? "frame" : "paused");
    JsonObject *tm = json_object_new();
    json_object_set_int_member(tm, "frame", (gint64)p->frame);
    json_object_set_int_member(tm, "frame_ticks", (gint64)p->frame_ticks);
    json_object_set_int_member(tm, "ticks", (gint64)p->ticks);
    json_object_set_int_member(tm, "total_cycles", (gint64)p->total_cycles);
    json_object_set_object_member(data, "time", tm);
    _cpu_panel_core_to_json(data, &p->panel);
    _cpu_panel_sections_to_json(data, &p->panel);
    _observe_windows_to_json(data, wins, meta, n_win);
    if (hash_sel) {
        JsonObject *ho = json_object_new();
        for (unsigned i = 0; i < G_N_ELEMENTS(s_observe_hash_parts); i++) {
            if (!(hash_sel & (1u << i))) continue;
            unsigned part = (unsigned)s_observe_hash_parts[i].part;
            if (part < DBGAPI_OBSERVE_HASH_PARTS && p->hash_ok[part]) {
                json_object_set_string_member(ho, s_observe_hash_parts[i].name, p->hash_hex[part]);
            } else {
                json_object_set_null_member(ho, s_observe_hash_parts[i].name);
            }
        }
        json_object_set_object_member(data, "hashes", ho);
    }
    if (p->want_memory_map) _observe_memory_map_to_json(data, p);
    *out_data = data;
    rc = MCP_DISPATCH_OK;

out:
    g_free(msg);
    g_free(bufs);
    g_free(meta);
    g_free(wins);
    g_free(p);
    return rc;
}


/**
 * @brief `state_observe` handler - registry, příznaky a okna paměti z jednoho okamžiku.
 *
 * Parametry v `data`:
 *  - `windows` (pole 0..16, povinné): okno CPU `{addr, len}` (aktuální
 *    mapování, addr + len <= 10000h) nebo regionu `{region: {kind, sub_id},
 *    offset, length}` (druh jako v `regions_list`, bez `region_id`, takže
 *    okno platí i po změně sestavy HW); volitelně `name` (štítek do
 *    64 znaků, vrací se zpět) a `want_data`. Součet délek nejvýš 65536 B.
 *  - `want_data` (bool, default true): výchozí volba dat pro okna.
 *  - `include` (pole): volitelné sekce `raster`, `im2`, `last_instr`
 *    (schéma `get_cpu_panel_batch`), `memory_map`.
 *  - `hashes` (pole): hashe částí stavu `cpu`, `ram`, `vram`, `memext`,
 *    `pcg` (state_hash.h, stejná definice jako replay a result.json).
 *
 * Vše čte emu vlákno v jednom průchodu fronty (DBGAPI_CMD_STATE_OBSERVE):
 * za běhu na hranici snímku, v pauze okamžitě. Hash oken (SHA-256) počítá
 * dispatch vlákno z kopie dat.
 *
 * Response: `run_seq`, `state_rev`, `running`, `stable` (= !running),
 * `drain` (`frame` za běhu, `paused`), `time` {`frame`, `frame_ticks`,
 * `ticks`, `total_cycles`}, `regs`, `flags`, `windows`, `complete` (false
 * = některé okno nedostupné), volitelně `hashes` (nedostupná část = null),
 * `raster`, `im2`, `last_instr`, `memory_map`.
 *
 * `if_state_rev_differs` (revize > 0): v pauze při shodné revizi stavu
 * slot příkaz přeskočí (SKIP_IF_EQUAL) a odpověď je jen `not_modified`,
 * `state_rev`, `run_seq`, `running` (false), `stable` (true).
 *
 * Chyby: neplatné parametry a okno regionu přes jeho konec =
 * `Invalid parameters: ...`; nic se nevrací, stav se nemění.
 */
static en_MCP_DISPATCH_RESULT _handle_state_observe(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                      ? json_node_get_object(data_node) : NULL;
    /* if_state_rev_differs: v pauze při shodné revizi nic nečíst, jen
     * `not_modified` (vzor HTTP 304); za běhu vždy plná odpověď. */
    if (obj && json_object_has_member(obj, "if_state_rev_differs")) {
        gint64 v = _obj_int_or(obj, "if_state_rev_differs", -1);
        if (v < 1) {
            return _err_response(req_id, "Invalid parameters: if_state_rev_differs must be "
                                 "a state_rev value (> 0)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        s_submit_track.req_cond = DBGAPI_REV_COND_SKIP_IF_EQUAL;
        s_submit_track.req_value = (uint64_t)v;
        s_submit_track.req_cmd = DBGAPI_CMD_STATE_OBSERVE;
        s_submit_track.req_pending = true;
    }
    JsonObject *data = NULL;
    en_MCP_DISPATCH_RESULT rc = _state_observe_exec(req_id, obj, &data, out_response);
    if (!data) return rc;
    if (s_submit_track.rev_skipped) {
        json_object_unref(data);
        data = json_object_new();
        json_object_set_boolean_member(data, "not_modified", TRUE);
        json_object_set_int_member(data, "state_rev", (gint64)s_submit_track.state_rev);
        json_object_set_int_member(data, "run_seq", (gint64)s_submit_track.run_seq);
        json_object_set_boolean_member(data, "running", FALSE);
        json_object_set_boolean_member(data, "stable", TRUE);
    }
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief Načte klíč bajtu (`kind`, `sub_id`, `offset`) z parametrů freeze_*.
 * @return NULL OK, jinak statická chybová zpráva.
 */
static const char *_freeze_parse_key(JsonObject *obj, int *kind, int *sub_id, uint32_t *offset) {
    int hk = _videorec_member(obj, "kind", G_TYPE_STRING);
    int hs = _videorec_member(obj, "sub_id", G_TYPE_INT64);
    int ho = _videorec_member(obj, "offset", G_TYPE_INT64);
    if (hk <= 0 || hs < 0 || ho <= 0) {
        return "Invalid parameters: kind (string from regions_list), offset (integer) and optional sub_id (integer) are required";
    }
    *kind = _region_kind_from_str(json_object_get_string_member(obj, "kind"));
    if (*kind < 0) return "Invalid parameters: unknown region kind (see regions_list)";
    gint64 s = hs ? json_object_get_int_member(obj, "sub_id") : 0;
    gint64 o = json_object_get_int_member(obj, "offset");
    if (s < 0 || s > G_MAXINT || o < 0 || o > 0xFFFFFFFFLL) {
        return "Invalid parameters: sub_id and offset must not be negative";
    }
    *sub_id = (int)s;
    *offset = (uint32_t)o;
    return NULL;
}

/**
 * @brief `freeze_add` handler - zafrozí bajt nebo souvislý úsek bajtů regionu.
 *
 * Parametry: `kind` (název z `regions_list`), `sub_id` (výchozí 0),
 * `offset`, a buď `value` (0-255), nebo `bytes` (pole 1-64 hodnot 0-255
 * od `offset`). Bajty se přepisují jednou za snímek emulace (Freeze
 * Bytes, tabulka nejvýš 256 bajtů); existující záznam se jen aktualizuje.
 * Region musí v sestavě existovat a být zapisovatelný.
 *
 * Odpověď `{added, frozen}` (frozen = počet zafrozených bajtů celkem).
 */
static en_MCP_DISPATCH_RESULT _handle_freeze_add(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int kind, sub_id;
    uint32_t offset;
    const char *e = _freeze_parse_key(obj, &kind, &sub_id, &offset);
    if (e) return _err_response(req_id, e, MCP_DISPATCH_INVALID_PARAMS, out_response);

    uint8_t bytes[64];
    guint n = 0;
    int hv = _videorec_member(obj, "value", G_TYPE_INT64);
    bool has_bytes = json_object_has_member(obj, "bytes")
                     && !json_node_is_null(json_object_get_member(obj, "bytes"));
    if (hv < 0 || (hv > 0) == has_bytes) {
        return _err_response(req_id, "Invalid parameters: give either value (0-255) or bytes (array of 0-255)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (hv > 0) {
        gint64 v = json_object_get_int_member(obj, "value");
        if (v < 0 || v > 255) {
            return _err_response(req_id, "Invalid parameters: value must be 0-255",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        bytes[0] = (uint8_t)v;
        n = 1;
    } else {
        JsonNode *bn = json_object_get_member(obj, "bytes");
        if (json_node_get_node_type(bn) != JSON_NODE_ARRAY) {
            return _err_response(req_id, "Invalid parameters: bytes must be an array",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonArray *ba = json_node_get_array(bn);
        n = json_array_get_length(ba);
        if (n < 1 || n > G_N_ELEMENTS(bytes)) {
            return _err_response(req_id, "Invalid parameters: bytes must have 1-64 items",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        for (guint i = 0; i < n; i++) {
            JsonNode *it = json_array_get_element(ba, i);
            if (json_node_get_node_type(it) != JSON_NODE_VALUE
                || json_node_get_value_type(it) != G_TYPE_INT64
                || json_node_get_int(it) < 0 || json_node_get_int(it) > 255) {
                return _err_response(req_id, "Invalid parameters: bytes items must be integers 0-255",
                                     MCP_DISPATCH_INVALID_PARAMS, out_response);
            }
            bytes[i] = (uint8_t)json_node_get_int(it);
        }
    }

    int rcount = 0;
    st_REGION_DESC *regs = _ms_regions(&rcount);
    const st_REGION_DESC *r = regs ? _ms_find_region(regs, rcount, kind, sub_id) : NULL;
    const char *re = NULL;
    if (!r) re = "freeze_add: region not present in this configuration (see regions_list)";
    else if (!r->writable) re = "freeze_add: region is read-only";
    else if (offset >= r->size || n > r->size - offset) re = "freeze_add: offset outside the region";
    g_free(regs);
    if (re) return _err_response(req_id, re, MCP_DISPATCH_INVALID_PARAMS, out_response);

    guint added = 0;
    for (guint i = 0; i < n; i++) {
        st_DBGAPI_FREEZE_PARAM p;
        memset(&p, 0, sizeof(p));
        p.region_kind = kind;
        p.sub_id = sub_id;
        p.offset = offset + i;
        p.value = bytes[i];
        if (!_submit_dbgapi(DBGAPI_CMD_FREEZE_ADD, &p, NULL)) break;
        added++;
    }
    if (added < n) {
        char msg[128];
        g_snprintf(msg, sizeof(msg), "freeze_add: freeze table full (256 bytes) or emulator unavailable: %u of %u bytes frozen",
                   added, n);
        return _err_response(req_id, msg, MCP_DISPATCH_EMU_ERROR, out_response);
    }
    st_DBGAPI_FREEZE_LIST_PARAM *lp = g_new0(st_DBGAPI_FREEZE_LIST_PARAM, 1);
    bool lok = _submit_dbgapi(DBGAPI_CMD_FREEZE_LIST, lp, NULL);
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "added", added);
    json_object_set_int_member(data, "frozen", lok ? (gint64)lp->out_count : -1);
    g_free(lp);
    return _ok_response(req_id, data, out_response);
}

/**
 * @brief `freeze_remove` handler - uvolní zafrozené bajty.
 *
 * Parametry: buď `all: true`, nebo `kind`, `sub_id` (výchozí 0), `offset`
 * a `length` (výchozí 1, nejvýš 256). Odpověď `{removed}`.
 */
static en_MCP_DISPATCH_RESULT _handle_freeze_remove(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int ha = _videorec_member(obj, "all", G_TYPE_BOOLEAN);
    if (ha < 0) {
        return _err_response(req_id, "Invalid parameters: all must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    guint removed = 0;
    if (ha > 0 && json_object_get_boolean_member(obj, "all")) {
        st_DBGAPI_FREEZE_LIST_PARAM *lp = g_new0(st_DBGAPI_FREEZE_LIST_PARAM, 1);
        if (!_submit_dbgapi(DBGAPI_CMD_FREEZE_LIST, lp, NULL)) {
            g_free(lp);
            return _err_response(req_id, "freeze_remove: emulator unavailable",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
        for (uint32_t i = 0; i < lp->out_count; i++) {
            st_DBGAPI_FREEZE_PARAM p;
            memset(&p, 0, sizeof(p));
            p.region_kind = lp->items[i].region_kind;
            p.sub_id = lp->items[i].sub_id;
            p.offset = lp->items[i].offset;
            if (_submit_dbgapi(DBGAPI_CMD_FREEZE_REMOVE, &p, NULL)) removed++;
        }
        g_free(lp);
    } else {
        int kind, sub_id;
        uint32_t offset;
        const char *e = _freeze_parse_key(obj, &kind, &sub_id, &offset);
        if (e) return _err_response(req_id, e, MCP_DISPATCH_INVALID_PARAMS, out_response);
        int hl = _videorec_member(obj, "length", G_TYPE_INT64);
        gint64 len = (hl > 0) ? json_object_get_int_member(obj, "length") : 1;
        if (hl < 0 || len < 1 || len > 256) {
            return _err_response(req_id, "Invalid parameters: length must be 1-256",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        for (gint64 i = 0; i < len; i++) {
            st_DBGAPI_FREEZE_PARAM p;
            memset(&p, 0, sizeof(p));
            p.region_kind = kind;
            p.sub_id = sub_id;
            p.offset = offset + (uint32_t)i;
            if (_submit_dbgapi(DBGAPI_CMD_FREEZE_REMOVE, &p, NULL)) removed++;
        }
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "removed", removed);
    return _ok_response(req_id, data, out_response);
}

/**
 * @brief `freeze_list` handler - zafrozené bajty.
 *
 * Bez parametrů. Odpověď `{count, max, entries: [{kind, sub_id, offset, value}]}`.
 */
static en_MCP_DISPATCH_RESULT _handle_freeze_list(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_FREEZE_LIST_PARAM *lp = g_new0(st_DBGAPI_FREEZE_LIST_PARAM, 1);
    if (!_submit_dbgapi(DBGAPI_CMD_FREEZE_LIST, lp, NULL)) {
        g_free(lp);
        return _err_response(req_id, "freeze_list: emulator unavailable",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    JsonArray *arr = json_array_new();
    for (uint32_t i = 0; i < lp->out_count; i++) {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "kind", _region_kind_to_str(lp->items[i].region_kind));
        json_object_set_int_member(o, "sub_id", lp->items[i].sub_id);
        json_object_set_int_member(o, "offset", (gint64)lp->items[i].offset);
        json_object_set_int_member(o, "value", lp->items[i].value);
        json_array_add_object_element(arr, o);
    }
    json_object_set_array_member(data, "entries", arr);
    json_object_set_int_member(data, "count", (gint64)lp->out_count);
    json_object_set_int_member(data, "max", (gint64)G_N_ELEMENTS(lp->items));
    g_free(lp);
    return _ok_response(req_id, data, out_response);
}


/** @brief Nejvyšší počet současně otevřených sezení memsearch přes MCP. */
#define MCP_MEMSEARCH_MAX_SESSIONS 8

/** @brief Výchozí a nejvyšší počet položek jedné stránky memsearch_list. */
#define MCP_MEMSEARCH_LIST_DEFAULT 100
#define MCP_MEMSEARCH_LIST_MAX     1000

/**
 * @brief Slot sezení memsearch otevřeného přes MCP.
 *
 * @invariant id > 0 právě když sess != NULL.
 */
typedef struct st_MCP_MEMSEARCH_SLOT {
    int                   id;    /**< ID sezení pro klienta (0 = volný slot). */
    st_MEMSEARCH_SESSION *sess;  /**< Sezení (vlastní slot). */
} st_MCP_MEMSEARCH_SLOT;

/**
 * @brief Sezení memsearch otevřená přes MCP (oddělená od okna GUI).
 *
 * Chráněno s_ms_lock po celou dobu handleru (sezení nemá vlastní zámek,
 * MCP TCP může obsluhovat více klientů). Statický GMutex nepotřebuje init.
 */
static st_MCP_MEMSEARCH_SLOT s_ms_slots[MCP_MEMSEARCH_MAX_SESSIONS];
static int    s_ms_next_id = 1;
static GMutex s_ms_lock;

/**
 * @brief Najde sezení podle ID z parametru `session_id`.
 * @pre Drží s_ms_lock.
 * @return Slot, nebo NULL.
 */
static st_MCP_MEMSEARCH_SLOT *_ms_slot(JsonObject *obj) {
    if (_videorec_member(obj, "session_id", G_TYPE_INT64) <= 0) return NULL;
    gint64 id = json_object_get_int_member(obj, "session_id");
    for (int i = 0; i < MCP_MEMSEARCH_MAX_SESSIONS; i++) {
        if (s_ms_slots[i].sess && s_ms_slots[i].id == id) return &s_ms_slots[i];
    }
    return NULL;
}

/**
 * @brief Uvolní všechna sezení memsearch (konec MCP dispatch).
 */
static void _ms_close_all(void) {
    g_mutex_lock(&s_ms_lock);
    for (int i = 0; i < MCP_MEMSEARCH_MAX_SESSIONS; i++) {
        memsearch_session_free(s_ms_slots[i].sess);
        s_ms_slots[i].sess = NULL;
        s_ms_slots[i].id = 0;
    }
    g_mutex_unlock(&s_ms_lock);
}

/**
 * @brief Pořídí nový vzorek prostoru sezení (DBGAPI_CMD_MEMSEARCH_SAMPLE).
 * @return Vzorek s refcount 1 (uvolnit memsearch_sample_unref), NULL = chyba.
 */
static st_MEMSEARCH_SAMPLE *_ms_take_sample(st_MEMSEARCH_SESSION *s) {
    st_MEMSEARCH_SAMPLE *smp = memsearch_sample_new(memsearch_space_size(s),
                                                    memsearch_span_count(s));
    if (!smp) return NULL;
    st_DBGAPI_MEMSEARCH_SAMPLE_PARAM p;
    p.spans = memsearch_span(s, 0);   /* spany sezení jsou souvislé pole */
    p.span_count = memsearch_span_count(s);
    p.sample = smp;
    if (!_submit_dbgapi(DBGAPI_CMD_MEMSEARCH_SAMPLE, &p, NULL)) {
        memsearch_sample_unref(smp);
        return NULL;
    }
    return smp;
}

/** @brief Společná pole stavu sezení do odpovědi. */
static void _ms_state_json(JsonObject *o, const st_MEMSEARCH_SESSION *s) {
    json_object_set_int_member(o, "candidates", (gint64)memsearch_candidate_count(s));
    json_object_set_int_member(o, "step", memsearch_step(s));
    json_object_set_int_member(o, "sample_frame", (gint64)memsearch_sample_frame(s));
    json_object_set_int_member(o, "undo_depth", memsearch_undo_depth(s));
    json_object_set_int_member(o, "redo_depth", memsearch_redo_depth(s));
}

/**
 * @brief Načte formát hodnoty z parametrů memsearch_start.
 *
 * `type` (u8 výchozí, s8, u16, s16, u24, s24, u32, s32, bcd, digits),
 * `width` (bcd 1-4, výchozí 2; digits 1-8, výchozí 4), `endian` ("le"
 * výchozí u čísel a BCD, "be" výchozí u číslic = pořadí tisku),
 * `aligned`, `digit_encoding` (raw, ascii, display výchozí),
 * `leading_space` (výchozí true).
 *
 * @return NULL OK, jinak statická chybová zpráva.
 */
static const char *_ms_parse_fmt(JsonObject *obj, st_MEMSEARCH_FMT *f) {
    memset(f, 0, sizeof(*f));
    int ht = _videorec_member(obj, "type", G_TYPE_STRING);
    int hw = _videorec_member(obj, "width", G_TYPE_INT64);
    int he = _videorec_member(obj, "endian", G_TYPE_STRING);
    int ha = _videorec_member(obj, "aligned", G_TYPE_BOOLEAN);
    int hd = _videorec_member(obj, "digit_encoding", G_TYPE_STRING);
    int hl = _videorec_member(obj, "leading_space", G_TYPE_BOOLEAN);
    if (ht < 0 || hw < 0 || he < 0 || ha < 0 || hd < 0 || hl < 0) {
        return "Invalid parameters: type, endian, digit_encoding must be strings, width an integer, aligned and leading_space booleans";
    }
    unsigned width = 1;
    f->type = MEMSEARCH_TYPE_UINT;
    if (ht && !memsearch_type_from_name(json_object_get_string_member(obj, "type"), &f->type, &width)) {
        return "Invalid parameters: type must be u8, s8, u16, s16, u24, s24, u32, s32, bcd or digits";
    }
    if (f->type == MEMSEARCH_TYPE_BCD || f->type == MEMSEARCH_TYPE_DIGITS) {
        width = (f->type == MEMSEARCH_TYPE_BCD) ? 2 : 4;
        if (hw) {
            gint64 w = json_object_get_int_member(obj, "width");
            width = (w < 0 || w > 64) ? 0 : (unsigned)w;
        }
    } else if (hw) {
        return "Invalid parameters: width applies only to bcd and digits (the integer width is in the type name)";
    }
    f->width = width;
    f->big_endian = (f->type == MEMSEARCH_TYPE_DIGITS);
    if (he) {
        const char *e = json_object_get_string_member(obj, "endian");
        if (g_strcmp0(e, "le") == 0) f->big_endian = false;
        else if (g_strcmp0(e, "be") == 0) f->big_endian = true;
        else return "Invalid parameters: endian must be le or be";
    }
    f->aligned = ha ? json_object_get_boolean_member(obj, "aligned") : false;
    f->digit_enc = MEMSEARCH_DIGITS_DISPLAY;
    if (hd) {
        int d = memsearch_digit_enc_from_name(json_object_get_string_member(obj, "digit_encoding"));
        if (d < 0) return "Invalid parameters: digit_encoding must be raw, ascii or display";
        f->digit_enc = (en_MEMSEARCH_DIGIT_ENC)d;
    }
    f->leading_space = hl ? json_object_get_boolean_member(obj, "leading_space") : true;
    const char *err = NULL;
    if (!memsearch_fmt_validate(f, &err)) return err;
    return NULL;
}

/**
 * @brief Načte filtr (`op`, `compare_to`, `value`, `value2`, `mask`, `negate`).
 *
 * Výchozí `compare_to`: "previous" u operací se vzorkem (inc, dec, ...),
 * jinak "value", když je zadané `value`, jinak "previous".
 *
 * @param obj     Parametry.
 * @param fmt     Formát sezení.
 * @param[out] f  Filtr.
 * @param[out] present false = `op` chybí (žádný filtr).
 * @return NULL OK, jinak chybová zpráva (statická nebo z memsearch).
 */
static const char *_ms_parse_filter(JsonObject *obj, const st_MEMSEARCH_FMT *fmt,
                                    st_MEMSEARCH_FILTER *f, bool *present) {
    memset(f, 0, sizeof(*f));
    *present = false;
    int ho = _videorec_member(obj, "op", G_TYPE_STRING);
    int hc = _videorec_member(obj, "compare_to", G_TYPE_STRING);
    int hv = _videorec_member(obj, "value", G_TYPE_INT64);
    int hv2 = _videorec_member(obj, "value2", G_TYPE_INT64);
    int hm = _videorec_member(obj, "mask", G_TYPE_INT64);
    int hn = _videorec_member(obj, "negate", G_TYPE_BOOLEAN);
    if (ho < 0 || hc < 0 || hv < 0 || hv2 < 0 || hm < 0 || hn < 0) {
        return "Invalid parameters: op and compare_to must be strings, value, value2 and mask integers, negate a boolean";
    }
    if (!ho) return NULL;
    int op = memsearch_op_from_name(json_object_get_string_member(obj, "op"));
    if (op < 0) {
        return "Invalid parameters: op must be eq, ne, lt, le, gt, ge, between, inc, dec, inc_by, dec_by, changed, unchanged, changed_by or mask_eq";
    }
    f->op = (en_MEMSEARCH_OP)op;
    f->value = hv ? json_object_get_int_member(obj, "value") : 0;
    f->value2 = hv2 ? json_object_get_int_member(obj, "value2") : 0;
    gint64 mask = hm ? json_object_get_int_member(obj, "mask") : 0xFFFFFFFFLL;
    if (mask < 0 || mask > 0xFFFFFFFFLL) return "Invalid parameters: mask must be 0..FFFFFFFFh";
    f->mask = (uint32_t)mask;
    f->negate = hn ? json_object_get_boolean_member(obj, "negate") : false;
    bool needs_ref = (f->op == MEMSEARCH_OP_INC || f->op == MEMSEARCH_OP_DEC
                      || f->op == MEMSEARCH_OP_INC_BY || f->op == MEMSEARCH_OP_DEC_BY
                      || f->op == MEMSEARCH_OP_CHANGED || f->op == MEMSEARCH_OP_UNCHANGED
                      || f->op == MEMSEARCH_OP_CHANGED_BY);
    f->compare_to = (!needs_ref && hv) ? MEMSEARCH_CMP_VALUE : MEMSEARCH_CMP_PREVIOUS;
    if (f->op == MEMSEARCH_OP_BETWEEN) f->compare_to = MEMSEARCH_CMP_VALUE;
    if (hc) {
        int c = memsearch_cmp_from_name(json_object_get_string_member(obj, "compare_to"));
        if (c < 0) return "Invalid parameters: compare_to must be value, previous or first";
        f->compare_to = (en_MEMSEARCH_CMP)c;
    }
    if (f->compare_to == MEMSEARCH_CMP_VALUE && !hv) {
        return "Invalid parameters: compare_to value needs value";
    }
    if (f->op == MEMSEARCH_OP_BETWEEN && !hv2) return "Invalid parameters: between needs value and value2";
    const char *err = NULL;
    if (!memsearch_filter_validate(f, fmt, &err)) return err;
    *present = true;
    return NULL;
}

/**
 * @brief Sestaví spany z parametru `space` (název předvolby nebo pole).
 *
 * Pole: `[{kind, sub_id, offset, length}]`, `sub_id` a `offset` výchozí 0,
 * `length` výchozí do konce regionu. Region musí v sestavě existovat.
 *
 * @param obj Parametry.
 * @param regs, count Výčet regionů.
 * @param[out] out Spany (alokuje se, uvolnit g_free).
 * @param[out] n Počet spanů.
 * @return NULL OK, jinak statická chybová zpráva.
 */
static const char *_ms_parse_space(JsonObject *obj, const st_REGION_DESC *regs, int count,
                                   st_MEMSEARCH_SPAN **out, unsigned *n) {
    *out = NULL;
    *n = 0;
    JsonNode *sn = (obj && json_object_has_member(obj, "space")) ? json_object_get_member(obj, "space") : NULL;
    if (!sn || json_node_is_null(sn)
        || (json_node_get_node_type(sn) == JSON_NODE_VALUE && json_node_get_value_type(sn) == G_TYPE_STRING)) {
        const char *name = sn && !json_node_is_null(sn) ? json_node_get_string(sn) : "ram";
        int p = memsearch_preset_from_name(name);
        if (p < 0) return "Invalid parameters: space must be ram, logical, vram, memext, ramdisk, all_writable or an array of {kind, sub_id, offset, length}";
        *out = g_new0(st_MEMSEARCH_SPAN, count > 0 ? count : 1);
        *n = memsearch_spans_from_preset((en_MEMSEARCH_PRESET)p, regs, count, *out);
        if (*n == 0) return "memsearch_start: the space preset has no connected region in this configuration";
        return NULL;
    }
    if (json_node_get_node_type(sn) != JSON_NODE_ARRAY) return "Invalid parameters: space must be a preset name or an array";
    JsonArray *a = json_node_get_array(sn);
    guint len = json_array_get_length(a);
    if (len < 1 || len > 1024) return "Invalid parameters: space array must have 1-1024 items";
    *out = g_new0(st_MEMSEARCH_SPAN, len);
    for (guint i = 0; i < len; i++) {
        JsonNode *it = json_array_get_element(a, i);
        if (json_node_get_node_type(it) != JSON_NODE_OBJECT) return "Invalid parameters: space items must be objects";
        JsonObject *io = json_node_get_object(it);
        int kind, sub_id;
        uint32_t offset = 0;
        if (_videorec_member(io, "offset", G_TYPE_INT64) <= 0) json_object_set_int_member(io, "offset", 0);
        const char *e = _freeze_parse_key(io, &kind, &sub_id, &offset);
        if (e) return e;
        const st_REGION_DESC *r = _ms_find_region(regs, count, kind, sub_id);
        if (!r) return "memsearch_start: a space region is not present in this configuration (see regions_list)";
        if (offset >= r->size) return "memsearch_start: span offset outside the region";
        int hl = _videorec_member(io, "length", G_TYPE_INT64);
        gint64 l = (hl > 0) ? json_object_get_int_member(io, "length") : (gint64)(r->size - offset);
        if (hl < 0 || l < 1 || l > (gint64)(r->size - offset)) return "memsearch_start: span length outside the region";
        (*out)[i].region_kind = kind;
        (*out)[i].sub_id = sub_id;
        (*out)[i].offset = offset;
        (*out)[i].length = (uint32_t)l;
    }
    *n = len;
    return NULL;
}

/**
 * @brief `memsearch_start` handler - založí sezení a pořídí první vzorek.
 *
 * Parametry: `space`, formát (`_ms_parse_fmt`), `allow_large` (strop
 * prostoru 32 MiB místo 4 MiB) a volitelný první filtr (`op`, `value`, ...,
 * jen proti hodnotě). Odpověď `{session_id, space_size, spans, candidates,
 * step, sample_frame, undo_depth, redo_depth}`.
 */
static en_MCP_DISPATCH_RESULT _handle_memsearch_start(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    st_MEMSEARCH_FMT fmt;
    const char *e = _ms_parse_fmt(obj, &fmt);
    if (e) return _err_response(req_id, e, MCP_DISPATCH_INVALID_PARAMS, out_response);
    st_MEMSEARCH_FILTER f;
    bool has_filter = false;
    e = _ms_parse_filter(obj, &fmt, &f, &has_filter);
    if (e) return _err_response(req_id, e, MCP_DISPATCH_INVALID_PARAMS, out_response);
    if (has_filter && f.compare_to != MEMSEARCH_CMP_VALUE) {
        return _err_response(req_id, "Invalid parameters: the first filter can only compare with a value",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    int hal = _videorec_member(obj, "allow_large", G_TYPE_BOOLEAN);
    if (hal < 0) {
        return _err_response(req_id, "Invalid parameters: allow_large must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool allow_large = hal ? json_object_get_boolean_member(obj, "allow_large") : false;

    int rcount = 0;
    st_REGION_DESC *regs = _ms_regions(&rcount);
    if (!regs) {
        return _err_response(req_id, "memsearch_start: region list unavailable",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    st_MEMSEARCH_SPAN *spans = NULL;
    unsigned n = 0;
    e = _ms_parse_space(obj, regs, rcount, &spans, &n);
    g_free(regs);
    if (e) {
        g_free(spans);
        return _err_response(req_id, e, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_MEMSEARCH_SESSION *s = memsearch_session_new(spans, n, &fmt,
        allow_large ? MEMSEARCH_HARD_MAX_SPACE : MEMSEARCH_DEFAULT_MAX_SPACE, &e);
    g_free(spans);
    if (!s) return _err_response(req_id, e ? e : "memsearch_start failed", MCP_DISPATCH_INVALID_PARAMS, out_response);

    st_MEMSEARCH_SAMPLE *smp = _ms_take_sample(s);
    if (!smp) {
        memsearch_session_free(s);
        return _err_response(req_id, "memsearch_start: memory sample failed (emulator unavailable)",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    bool ok = memsearch_start(s, smp, has_filter ? &f : NULL, &e);
    if (!ok) {
        memsearch_sample_unref(smp);
        memsearch_session_free(s);
        return _err_response(req_id, e ? e : "memsearch_start failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }

    g_mutex_lock(&s_ms_lock);
    st_MCP_MEMSEARCH_SLOT *slot = NULL;
    for (int i = 0; i < MCP_MEMSEARCH_MAX_SESSIONS && !slot; i++) {
        if (!s_ms_slots[i].sess) slot = &s_ms_slots[i];
    }
    if (!slot) {
        g_mutex_unlock(&s_ms_lock);
        memsearch_sample_unref(smp);
        memsearch_session_free(s);
        return _err_response(req_id, "memsearch_start: too many open sessions (8), close one with memsearch_close",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    slot->sess = s;
    slot->id = s_ms_next_id++;

    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "session_id", slot->id);
    json_object_set_int_member(data, "space_size", (gint64)memsearch_space_size(s));
    JsonArray *sa = json_array_new();
    for (unsigned i = 0; i < memsearch_span_count(s); i++) {
        const st_MEMSEARCH_SPAN *sp = memsearch_span(s, i);
        JsonObject *so = json_object_new();
        json_object_set_string_member(so, "kind", _region_kind_to_str(sp->region_kind));
        json_object_set_int_member(so, "sub_id", sp->sub_id);
        json_object_set_int_member(so, "offset", (gint64)sp->offset);
        json_object_set_int_member(so, "length", (gint64)sp->length);
        json_object_set_boolean_member(so, "connected", smp->span_connected[i] != 0);
        json_array_add_object_element(sa, so);
    }
    json_object_set_array_member(data, "spans", sa);
    _ms_state_json(data, s);
    g_mutex_unlock(&s_ms_lock);
    memsearch_sample_unref(smp);
    return _ok_response(req_id, data, out_response);
}

/** @brief Chyba "neznámé sezení". */
static en_MCP_DISPATCH_RESULT _ms_err_session(int64_t req_id, char **out_response) {
    return _err_response(req_id, "unknown memsearch session_id (start one with memsearch_start)",
                         MCP_DISPATCH_INVALID_PARAMS, out_response);
}

/**
 * @brief `memsearch_filter` handler - zúží kandidáty.
 *
 * Parametry: `session_id`, filtr (`_ms_parse_filter`, `op` povinný) a
 * `sample` (výchozí true = nejdřív nový vzorek; false = porovná poslední
 * vzorek s předchozím). Odpověď `{removed, candidates, step, ...}`.
 */
static en_MCP_DISPATCH_RESULT _handle_memsearch_filter(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int hs = _videorec_member(obj, "sample", G_TYPE_BOOLEAN);
    if (hs < 0) {
        return _err_response(req_id, "Invalid parameters: sample must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool take = hs ? json_object_get_boolean_member(obj, "sample") : true;

    g_mutex_lock(&s_ms_lock);
    st_MCP_MEMSEARCH_SLOT *slot = _ms_slot(obj);
    if (!slot) {
        g_mutex_unlock(&s_ms_lock);
        return _ms_err_session(req_id, out_response);
    }
    st_MEMSEARCH_FILTER f;
    bool present = false;
    const char *e = _ms_parse_filter(obj, memsearch_fmt(slot->sess), &f, &present);
    if (!e && !present) e = "Invalid parameters: op is required";
    if (e) {
        g_mutex_unlock(&s_ms_lock);
        return _err_response(req_id, e, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_MEMSEARCH_SAMPLE *smp = NULL;
    if (take) {
        smp = _ms_take_sample(slot->sess);
        if (!smp) {
            g_mutex_unlock(&s_ms_lock);
            return _err_response(req_id, "memsearch_filter: memory sample failed (emulator unavailable)",
                                 MCP_DISPATCH_EMU_ERROR, out_response);
        }
    }
    uint64_t removed = 0;
    bool ok = memsearch_filter(slot->sess, smp, &f, &removed, &e);
    memsearch_sample_unref(smp);
    if (!ok) {
        g_mutex_unlock(&s_ms_lock);
        return _err_response(req_id, e ? e : "memsearch_filter failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "removed", (gint64)removed);
    _ms_state_json(data, slot->sess);
    g_mutex_unlock(&s_ms_lock);
    return _ok_response(req_id, data, out_response);
}

/**
 * @brief `memsearch_sample` handler - nový vzorek bez filtrace.
 *
 * Parametr `session_id`. Předchozí vzorek se stane referencí pro další
 * filtr s `sample: false`. Odpověď stav sezení.
 */
static en_MCP_DISPATCH_RESULT _handle_memsearch_sample(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    g_mutex_lock(&s_ms_lock);
    st_MCP_MEMSEARCH_SLOT *slot = _ms_slot(obj);
    if (!slot) {
        g_mutex_unlock(&s_ms_lock);
        return _ms_err_session(req_id, out_response);
    }
    st_MEMSEARCH_SAMPLE *smp = _ms_take_sample(slot->sess);
    const char *e = smp ? NULL : "memsearch_sample: memory sample failed (emulator unavailable)";
    if (smp && !memsearch_sample(slot->sess, smp, &e) && !e) e = "memsearch_sample failed";
    memsearch_sample_unref(smp);
    if (e) {
        g_mutex_unlock(&s_ms_lock);
        return _err_response(req_id, e, MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *data = json_object_new();
    _ms_state_json(data, slot->sess);
    g_mutex_unlock(&s_ms_lock);
    return _ok_response(req_id, data, out_response);
}

/**
 * @brief Společné tělo memsearch_undo / memsearch_redo.
 * @param redo false = undo, true = redo.
 */
static en_MCP_DISPATCH_RESULT _ms_undo_redo(const st_JSONL_MESSAGE *req, char **out_response, bool redo) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    g_mutex_lock(&s_ms_lock);
    st_MCP_MEMSEARCH_SLOT *slot = _ms_slot(obj);
    if (!slot) {
        g_mutex_unlock(&s_ms_lock);
        return _ms_err_session(req_id, out_response);
    }
    bool ok = redo ? memsearch_redo(slot->sess) : memsearch_undo(slot->sess);
    if (!ok) {
        g_mutex_unlock(&s_ms_lock);
        return _err_response(req_id, redo ? "nothing to redo" : "nothing to undo",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "started", memsearch_started(slot->sess));
    _ms_state_json(data, slot->sess);
    g_mutex_unlock(&s_ms_lock);
    return _ok_response(req_id, data, out_response);
}

/** @brief `memsearch_undo` handler - vrátí poslední krok (kandidáty i referenční vzorky). */
static en_MCP_DISPATCH_RESULT _handle_memsearch_undo(const st_JSONL_MESSAGE *req, char **out_response) {
    return _ms_undo_redo(req, out_response, false);
}

/** @brief `memsearch_redo` handler - znovu provede vrácený krok. */
static en_MCP_DISPATCH_RESULT _handle_memsearch_redo(const st_JSONL_MESSAGE *req, char **out_response) {
    return _ms_undo_redo(req, out_response, true);
}

/** @brief Hodnota kandidáta do JSON (číslo, nebo null u neplatné hodnoty). */
static void _ms_set_value(JsonObject *o, const char *key, const st_MEMSEARCH_VALUE *v) {
    if (v->valid) json_object_set_int_member(o, key, v->value);
    else json_object_set_null_member(o, key);
}

/**
 * @brief Adresa Z80 a parametry write breakpointu pro kandidáta.
 *
 * Převod dělá memsearch_target() (stejný jako v okně Memory Search).
 * `bp_hint` = parametry pro emu_bp_create_with_init (MEM_W, zóna, u PEHU
 * bank_id + bp_addr_space bank_offset, u šířky > 1 rozsah), null = buňku
 * write breakpoint nezachytí.
 *
 * @param o     Položka výpisu.
 * @param it    Kandidát.
 * @param width Šířka hodnoty.
 * @param mi    Stav MemExt (NULL = nezjištěn).
 */
static void _ms_set_addr_hint(JsonObject *o, const st_MEMSEARCH_ITEM *it, unsigned width,
                              const st_DBGAPI_MEMEXT_INFO_PARAM *mi) {
    st_MEMSEARCH_MEMEXT mx;
    memset(&mx, 0, sizeof(mx));
    if (mi) {
        mx.connected = mi->connected != 0;
        mx.pehu = strcmp(mi->type, "pehu") == 0;
        mx.map_valid = mi->map_available != 0;
        memcpy(mx.map, mi->current_map, sizeof(mx.map));
    }
    st_MEMSEARCH_TARGET t;
    (void)memsearch_target(it->region_kind, it->sub_id, it->region_offset, width,
                           mi ? &mx : NULL, &t);
    if (t.has_z80) json_object_set_int_member(o, "z80_addr", t.z80_addr);
    else json_object_set_null_member(o, "z80_addr");
    if (t.bp_zone == MEMSEARCH_BPZONE_NONE) {
        json_object_set_null_member(o, "bp_hint");
        return;
    }
    JsonObject *h = json_object_new();
    JsonArray *fa = json_array_new();
    JsonObject *va = json_object_new();
    json_object_set_int_member(h, "addr", t.bp_addr);
    json_array_add_string_element(fa, "type");
    json_object_set_string_member(va, "type", "MEM_W");
    json_array_add_string_element(fa, "zone");
    json_object_set_string_member(va, "zone", memsearch_bpzone_name(t.bp_zone));
    if (t.bp_bank >= 0) {
        json_array_add_string_element(fa, "bank_id");
        json_object_set_int_member(va, "bank_id", t.bp_bank);
        json_array_add_string_element(fa, "bp_addr_space");
        json_object_set_string_member(va, "bp_addr_space", "bank_offset");
    }
    if (width > 1) {
        json_array_add_string_element(fa, "addr_end");
        json_object_set_int_member(va, "addr_end", t.bp_addr_end);
        json_array_add_string_element(fa, "addr_match_mode");
        json_object_set_string_member(va, "addr_match_mode", "RANGE");
    }
    json_object_set_array_member(h, "fields", fa);
    json_object_set_object_member(h, "values", va);
    json_object_set_object_member(o, "bp_hint", h);
}

/**
 * @brief `memsearch_list` handler - stránka kandidátů.
 *
 * Parametry: `session_id`, `offset` (pořadí prvního kandidáta, výchozí 0),
 * `limit` (výchozí 100, nejvýš 1000). Položka: `index`, `space_offset`,
 * `kind`, `sub_id`, `region_offset`, `connected`, `cur`, `prev`, `first`
 * (čísla, null u neplatné hodnoty), `cur_hex` (bajty v pořadí adres),
 * `z80_addr` a `bp_hint` (parametry pro emu_bp_create_with_init, nebo null).
 */
static en_MCP_DISPATCH_RESULT _handle_memsearch_list(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    int ho = _videorec_member(obj, "offset", G_TYPE_INT64);
    int hl = _videorec_member(obj, "limit", G_TYPE_INT64);
    gint64 off = ho > 0 ? json_object_get_int_member(obj, "offset") : 0;
    gint64 lim = hl > 0 ? json_object_get_int_member(obj, "limit") : MCP_MEMSEARCH_LIST_DEFAULT;
    if (ho < 0 || hl < 0 || off < 0 || lim < 1 || lim > MCP_MEMSEARCH_LIST_MAX) {
        return _err_response(req_id, "Invalid parameters: offset >= 0, limit 1-1000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_MEMEXT_INFO_PARAM mi;
    memset(&mi, 0, sizeof(mi));
    bool have_mi = false;

    g_mutex_lock(&s_ms_lock);
    st_MCP_MEMSEARCH_SLOT *slot = _ms_slot(obj);
    if (!slot) {
        g_mutex_unlock(&s_ms_lock);
        return _ms_err_session(req_id, out_response);
    }
    st_MEMSEARCH_SESSION *s = slot->sess;
    for (unsigned i = 0; i < memsearch_span_count(s) && !have_mi; i++) {
        if (memsearch_span(s, i)->region_kind == REGION_KIND_MEMEXT_RAM) {
            have_mi = _submit_dbgapi(DBGAPI_CMD_GET_MEMEXT_INFO, &mi, NULL);
            break;
        }
    }
    unsigned width = memsearch_fmt(s)->width;
    uint64_t total = memsearch_candidate_count(s);
    JsonObject *data = json_object_new();
    JsonArray *arr = json_array_new();
    for (uint64_t n = (uint64_t)off; n < total && n < (uint64_t)(off + lim); n++) {
        uint32_t so = 0;
        st_MEMSEARCH_ITEM it;
        if (!memsearch_nth_candidate(s, n, &so) || !memsearch_item(s, so, &it)) break;
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "index", (gint64)n);
        json_object_set_int_member(o, "space_offset", (gint64)so);
        json_object_set_string_member(o, "kind", _region_kind_to_str(it.region_kind));
        json_object_set_int_member(o, "sub_id", it.sub_id);
        json_object_set_int_member(o, "region_offset", (gint64)it.region_offset);
        json_object_set_boolean_member(o, "connected", it.connected);
        _ms_set_value(o, "cur", &it.cur);
        _ms_set_value(o, "prev", &it.prev);
        _ms_set_value(o, "first", &it.first);
        char hex[2 * MEMSEARCH_MAX_WIDTH + 1];
        for (unsigned b = 0; b < width; b++) g_snprintf(&hex[2 * b], 3, "%02X", it.cur.raw[b]);
        hex[2 * width] = '\0';
        json_object_set_string_member(o, "cur_hex", hex);
        _ms_set_addr_hint(o, &it, width, have_mi ? &mi : NULL);
        json_array_add_object_element(arr, o);
    }
    json_object_set_array_member(data, "items", arr);
    json_object_set_int_member(data, "total", (gint64)total);
    json_object_set_int_member(data, "offset", off);
    _ms_state_json(data, s);
    g_mutex_unlock(&s_ms_lock);
    return _ok_response(req_id, data, out_response);
}

/**
 * @brief `memsearch_eliminate` handler - ručně vyřadí kandidáty.
 *
 * Parametry: `session_id`, `space_offsets` (pole hodnot `space_offset`
 * z memsearch_list, nejvýš 1000). Odpověď `{removed, candidates, ...}`.
 */
static en_MCP_DISPATCH_RESULT _handle_memsearch_eliminate(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    JsonNode *an = (obj && json_object_has_member(obj, "space_offsets"))
                   ? json_object_get_member(obj, "space_offsets") : NULL;
    if (!an || json_node_get_node_type(an) != JSON_NODE_ARRAY) {
        return _err_response(req_id, "Invalid parameters: space_offsets must be an array of integers",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonArray *a = json_node_get_array(an);
    guint n = json_array_get_length(a);
    if (n > MCP_MEMSEARCH_LIST_MAX) {
        return _err_response(req_id, "Invalid parameters: at most 1000 space_offsets",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint32_t *offs = g_new0(uint32_t, n ? n : 1);
    for (guint i = 0; i < n; i++) {
        JsonNode *it = json_array_get_element(a, i);
        if (json_node_get_node_type(it) != JSON_NODE_VALUE
            || json_node_get_value_type(it) != G_TYPE_INT64
            || json_node_get_int(it) < 0 || json_node_get_int(it) > 0xFFFFFFFFLL) {
            g_free(offs);
            return _err_response(req_id, "Invalid parameters: space_offsets items must be non-negative integers",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        offs[i] = (uint32_t)json_node_get_int(it);
    }
    g_mutex_lock(&s_ms_lock);
    st_MCP_MEMSEARCH_SLOT *slot = _ms_slot(obj);
    if (!slot) {
        g_mutex_unlock(&s_ms_lock);
        g_free(offs);
        return _ms_err_session(req_id, out_response);
    }
    uint64_t removed = memsearch_eliminate(slot->sess, offs, n);
    g_free(offs);
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "removed", (gint64)removed);
    _ms_state_json(data, slot->sess);
    g_mutex_unlock(&s_ms_lock);
    return _ok_response(req_id, data, out_response);
}

/**
 * @brief `memsearch_close` handler - zavře sezení a uvolní jeho paměť.
 *
 * Parametr `session_id`. Odpověď `{closed: true}`.
 */
static en_MCP_DISPATCH_RESULT _handle_memsearch_close(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    g_mutex_lock(&s_ms_lock);
    st_MCP_MEMSEARCH_SLOT *slot = _ms_slot(obj);
    if (!slot) {
        g_mutex_unlock(&s_ms_lock);
        return _ms_err_session(req_id, out_response);
    }
    memsearch_session_free(slot->sess);
    slot->sess = NULL;
    slot->id = 0;
    g_mutex_unlock(&s_ms_lock);
    JsonObject *data = json_object_new();
    json_object_set_boolean_member(data, "closed", TRUE);
    return _ok_response(req_id, data, out_response);
}




/* ============================================================================
 * Záznam a přehrání vstupů (replay.h)
 * ============================================================================ */

/** @brief Nejdelší čekání na vyřízení požadavku záznamu (ms). */
#define MCP_REPLAY_REQUEST_TIMEOUT_MS 10000


/**
 * @brief Stav záznamu / přehrávání jako JSON objekt.
 *
 * Členy: `mode`, `frame`, `frames`, `events`, `hashes`, `hash_interval`,
 * `warnings`, `deviated`, `deviation`, `path`, `outcome`, `outcome_frame`,
 * `outcome_message`, `desync_parts` (pole jmen částí), `rerecord_count`,
 * `seeking`, `seek_target` a objekt `checkpoints` (`interval`, `capacity`,
 * `count`, `first_frame`, `last_frame`, `bytes`; snímky jen při `count` > 0).
 */
static JsonObject *_replay_status_to_json(const st_REPLAY_STATUS *st) {
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "mode", replay_mode_name(st->mode));
    json_object_set_int_member(o, "frame", (gint64)st->frame);
    json_object_set_int_member(o, "frames", (gint64)st->frames);
    json_object_set_int_member(o, "events", (gint64)st->events);
    json_object_set_int_member(o, "hashes", (gint64)st->hashes);
    json_object_set_int_member(o, "hash_interval", (gint64)st->hash_interval);
    json_object_set_int_member(o, "warnings", (gint64)st->warnings);
    json_object_set_boolean_member(o, "deviated", st->deviated ? TRUE : FALSE);
    json_object_set_string_member(o, "deviation", st->deviation);
    json_object_set_string_member(o, "path", st->path);
    json_object_set_string_member(o, "outcome", replay_outcome_name(st->outcome));
    json_object_set_int_member(o, "outcome_frame", (gint64)st->outcome_frame);
    json_object_set_string_member(o, "outcome_message", st->outcome_message);
    JsonArray *parts = json_array_new();
    if (st->desync_parts[0]) {
        char **v = g_strsplit(st->desync_parts, ",", -1);
        for (int i = 0; v[i]; i++) json_array_add_string_element(parts, v[i]);
        g_strfreev(v);
    }
    json_object_set_array_member(o, "desync_parts", parts);
    json_object_set_int_member(o, "rerecord_count", (gint64)st->rerecord_count);
    json_object_set_boolean_member(o, "seeking", st->seeking ? TRUE : FALSE);
    json_object_set_int_member(o, "seek_target", (gint64)st->seek_target);
    json_object_set_boolean_member(o, "goto_active", st->goto_active ? TRUE : FALSE);
    json_object_set_int_member(o, "goto_target_t", (gint64)st->goto_target);
    JsonObject *ck = json_object_new();
    json_object_set_int_member(ck, "interval", (gint64)st->checkpoint_interval);
    json_object_set_int_member(ck, "capacity", (gint64)st->checkpoint_capacity);
    json_object_set_int_member(ck, "count", (gint64)st->checkpoints);
    if (st->checkpoints) {
        json_object_set_int_member(ck, "first_frame", (gint64)st->checkpoint_first);
        json_object_set_int_member(ck, "last_frame", (gint64)st->checkpoint_last);
    }
    json_object_set_int_member(ck, "bytes", (gint64)st->checkpoint_bytes);
    json_object_set_object_member(o, "checkpoints", ck);
    return o;
}


/**
 * @brief Počká na vyřízení požadavku a vrátí odpověď (stav) nebo chybu.
 *
 * Chyba požadavku (`request_ok` false) = EMU_ERROR s textem z emu vlákna;
 * nevyřízení do MCP_REPLAY_REQUEST_TIMEOUT_MS = EMU_ERROR "not processed".
 */
static en_MCP_DISPATCH_RESULT _replay_finish_request(int64_t req_id, uint32_t seq,
                                                     char **out_response) {
    if (!replay_wait_request(seq, MCP_REPLAY_REQUEST_TIMEOUT_MS)) {
        return _err_response(req_id, "replay request was not processed in time",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    if (!st.request_ok) {
        return _err_response(req_id, st.request_message[0] ? st.request_message : "replay request failed",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, _replay_status_to_json(&st), out_response);
}


/** @brief Datový objekt požadavku nebo NULL. */
static JsonObject *_replay_data_obj(const st_JSONL_MESSAGE *req) {
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    return (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
           ? json_node_get_object(data_node) : NULL;
}


/**
 * @brief Volitelný bool parametr.
 * @return true = v pořádku (chybí -> @p def); false = parametr není bool.
 */
static bool _replay_obj_bool(JsonObject *obj, const char *key, bool def, bool *out) {
    *out = def;
    if (!obj || !json_object_has_member(obj, key)) return true;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || json_node_get_node_type(n) != JSON_NODE_VALUE
        || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return false;
    *out = json_node_get_boolean(n) ? true : false;
    return true;
}


/**
 * @brief Volitelné parametry checkpointů `checkpoint_interval` a `checkpoint_count`.
 *
 * @param[out] cfg Chybějící parametr = -1 (z INI).
 * @param[out] err Text chyby při neplatné hodnotě.
 * @return true = v pořádku; false = hodnota mimo rozsah (interval 0..100000,
 *         počet 1..REPLAY_CKPT_MAX_COUNT).
 */
static bool _replay_ckpt_params(JsonObject *obj, st_REPLAY_CKPT_CFG *cfg, const char **err) {
    cfg->interval = -1;
    cfg->count = -1;
    if (obj && json_object_has_member(obj, "checkpoint_interval")) {
        gint64 v = _obj_int_or(obj, "checkpoint_interval", -1);
        if (v < 0 || v > 100000) {
            *err = "checkpoint_interval must be in range 0..100000 (0 = no checkpoints)";
            return false;
        }
        cfg->interval = (int)v;
    }
    if (obj && json_object_has_member(obj, "checkpoint_count")) {
        gint64 v = _obj_int_or(obj, "checkpoint_count", -1);
        if (v < 1 || v > REPLAY_CKPT_MAX_COUNT) {
            *err = "checkpoint_count must be in range 1..1000";
            return false;
        }
        cfg->count = (int)v;
    }
    return true;
}


/**
 * @brief `replay_record_start` handler - start záznamu vstupů do souboru.
 *
 * Parametry: `path` (string, povinný, `.mzr`), `hash_interval` (int
 * 1..100000, default 50 snímků), `description` (string),
 * `checkpoint_interval` (0..100000 snímků, 0 = bez checkpointů, default
 * z INI = 250), `checkpoint_count` (1..1000, default z INI = 60). Za běhu
 * se emulace na nejbližší hranici snímku pozastaví, záznam začne a
 * emulace běží dál; v pauze zůstane v pauze. Response: stav (viz
 * replay_status).
 */
static en_MCP_DISPATCH_RESULT _handle_replay_record_start(const st_JSONL_MESSAGE *req,
                                                          char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _replay_data_obj(req);
    char *path = _obj_str_dup(obj, "path");
    if (!path || !path[0]) {
        g_free(path);
        return _err_response(req_id, "path is required", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 interval = _obj_int_or(obj, "hash_interval", 0);
    if (obj && json_object_has_member(obj, "hash_interval") && (interval < 1 || interval > 100000)) {
        g_free(path);
        return _err_response(req_id, "hash_interval must be in range 1..100000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_REPLAY_CKPT_CFG ckpt;
    const char *perr = NULL;
    if (!_replay_ckpt_params(obj, &ckpt, &perr)) {
        g_free(path);
        return _err_response(req_id, perr, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *desc = _obj_str_dup(obj, "description");
    uint32_t seq = replay_request_record_ex(path, (uint32_t)interval, desc, &ckpt);
    g_free(path);
    g_free(desc);
    return _replay_finish_request(req_id, seq, out_response);
}


/**
 * @brief Společná obsluha `replay_record_stop` a `replay_play_stop`.
 * @param want Režim, který má běžet; jinak chyba bez požadavku.
 */
static en_MCP_DISPATCH_RESULT _replay_stop(const st_JSONL_MESSAGE *req, en_REPLAY_MODE want,
                                           char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    if (st.mode != want) {
        return _err_response(req_id, want == REPLAY_MODE_RECORDING ? "no recording is running"
                                                                   : "no playback is running",
                             MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _replay_finish_request(req_id, replay_request_stop(), out_response);
}


/**
 * @brief `replay_record_stop` handler - konec záznamu a uložení souboru.
 *
 * Response: stav s `outcome` = `recorded` (nebo chyba uložení).
 */
static en_MCP_DISPATCH_RESULT _handle_replay_record_stop(const st_JSONL_MESSAGE *req,
                                                         char **out_response) {
    return _replay_stop(req, REPLAY_MODE_RECORDING, out_response);
}


/**
 * @brief `replay_play` handler - start přehrávání záznamu.
 *
 * Parametry: `path` (string, povinný), `pause_after_load` (bool, default
 * false: po načtení startovního snapshotu se emulace rozběhne),
 * `checkpoint_interval` a `checkpoint_count` (jako u replay_record_start;
 * checkpointy přehrávání urychlí seek zpět). Konec přehrávání = pauza
 * s důvodem `replay_end` nebo `replay_desync` (událost `paused`,
 * wait_stop). Response: stav (viz replay_status).
 */
static en_MCP_DISPATCH_RESULT _handle_replay_play(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _replay_data_obj(req);
    char *path = _obj_str_dup(obj, "path");
    if (!path || !path[0]) {
        g_free(path);
        return _err_response(req_id, "path is required", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool pause_after_load = false;
    if (!_replay_obj_bool(obj, "pause_after_load", false, &pause_after_load)) {
        g_free(path);
        return _err_response(req_id, "pause_after_load must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_REPLAY_CKPT_CFG ckpt;
    const char *perr = NULL;
    if (!_replay_ckpt_params(obj, &ckpt, &perr)) {
        g_free(path);
        return _err_response(req_id, perr, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    uint32_t seq = replay_request_play_ex(path, pause_after_load, &ckpt);
    g_free(path);
    return _replay_finish_request(req_id, seq, out_response);
}


/**
 * @brief `replay_play_stop` handler - ukončí přehrávání (outcome `stopped`).
 */
static en_MCP_DISPATCH_RESULT _handle_replay_play_stop(const st_JSONL_MESSAGE *req,
                                                       char **out_response) {
    return _replay_stop(req, REPLAY_MODE_PLAYING, out_response);
}


/**
 * @brief `replay_rewind` handler - návrat na checkpoint.
 *
 * Parametr `frames` (int >= 0, default 0 = o jeden checkpoint zpět; jinak
 * nejnovější checkpoint aspoň o tolik snímků zpět). Při záznamu se vstupy
 * po checkpointu zahodí (`rerecord_count` + 1) a emulace zůstane stát na
 * hranici snímku checkpointu; při přehrávání jde o seek zpět (viz
 * replay_seek). Odmítnutí (žádný checkpoint, zápis na médium po
 * checkpointu, nic neběží) = EMU_ERROR s vysvětlením. Response: stav.
 */
static en_MCP_DISPATCH_RESULT _handle_replay_rewind(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _replay_data_obj(req);
    gint64 frames = _obj_int_or(obj, "frames", 0);
    if (frames < 0 || frames > G_MAXUINT32) {
        return _err_response(req_id, "frames must be >= 0", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    return _replay_finish_request(req_id, replay_request_rewind((uint32_t)frames), out_response);
}


/**
 * @brief `replay_seek` handler - skok přehrávání na snímek.
 *
 * Parametr `frame` (int >= 0, povinný; za koncem záznamu se omezí na
 * konec). Zpět: nejbližší použitelný checkpoint, jinak přehrávání znovu od
 * začátku; dopředu (nebo od checkpointu) dohraje v MAX SPEED a zastaví na
 * hranici cílového snímku s důvodem pauzy `replay_seek` (událost `paused`,
 * emu_wait_stop). Odpověď přijde hned po spuštění: `seeking` true =
 * dohrávání běží, false = emulace už stojí na cíli. Response: stav.
 */
static en_MCP_DISPATCH_RESULT _handle_replay_seek(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _replay_data_obj(req);
    if (!obj || !json_object_has_member(obj, "frame")) {
        return _err_response(req_id, "frame is required", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 frame = _obj_int_or(obj, "frame", -1);
    if (frame < 0 || frame > G_MAXUINT32) {
        return _err_response(req_id, "frame must be >= 0", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    return _replay_finish_request(req_id, replay_request_seek((uint32_t)frame), out_response);
}


/** @brief Nejdelší čekání `replay_goto` na konec dojetí [ms] (pod 30 s limitem wrapperu MCP). */
#define MCP_REPLAY_GOTO_WAIT_MAX_MS 25000
/** @brief Výchozí čekání `replay_goto` na konec dojetí [ms]. */
#define MCP_REPLAY_GOTO_WAIT_DEFAULT_MS 20000

/** @brief Registry porovnávané `trace_check` (sloupec trace, klíč observe, dělení IR). */
static const struct {
    const char *col;   /**< Sloupec cputrack v3. */
    const char *reg;   /**< Klíč v `regs` observe (NULL = příznak ve `flags`). */
    int ir_part;       /**< 1 = I (horní bajt IR), 2 = R (dolní bajt IR), 0 = celý registr. */
} s_goto_check_regs[] = {
    { "af", "AF", 0 }, { "bc", "BC", 0 }, { "de", "DE", 0 }, { "hl", "HL", 0 },
    { "ix", "IX", 0 }, { "iy", "IY", 0 }, { "sp", "SP", 0 }, { "wz", "WZ", 0 },
    { "i", "IR", 1 }, { "r", "IR", 2 },
};


/**
 * @brief Řádek stránky trace_read podle `seq` jako objekt sloupec -> hodnota.
 *
 * @param page Datový objekt `trace_read` (`columns`, `rows`).
 * @param seq  Hledané pořadí záznamu.
 * @return Nový objekt (vlastní volající), NULL = řádek ve stránce není.
 */
static JsonObject *_goto_trace_row(JsonObject *page, int64_t seq) {
    JsonArray *cols = json_object_get_array_member(page, "columns");
    JsonArray *rows = json_object_get_array_member(page, "rows");
    if (!cols || !rows) return NULL;
    guint nc = json_array_get_length(cols);
    for (guint i = 0; i < json_array_get_length(rows); i++) {
        JsonArray *row = json_array_get_array_element(rows, i);
        if (!row || json_array_get_length(row) != nc) continue;
        JsonObject *o = json_object_new();
        for (guint c = 0; c < nc; c++) {
            json_object_set_member(o, json_array_get_string_element(cols, c),
                                   json_node_copy(json_array_get_element(row, c)));
        }
        if (json_object_has_member(o, "seq") && json_object_get_int_member(o, "seq") == seq) return o;
        json_object_unref(o);
    }
    return NULL;
}


/**
 * @brief Celočíselný sloupec řádku, nebo -1 (chybí, null).
 *
 * `trace_read` vrací adresy a registry (`pc`, `af` ...) jako hexadecimální
 * řetězec bez prefixu, čas a délky jako číslo; obojí se převede na číslo.
 */
static int64_t _goto_row_int(JsonObject *row, const char *col) {
    if (!row || !json_object_has_member(row, col)) return -1;
    JsonNode *n = json_object_get_member(row, col);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return -1;
    if (json_node_get_value_type(n) == G_TYPE_STRING) {
        const char *str = json_node_get_string(n);
        char *end = NULL;
        gint64 v = str ? g_ascii_strtoll(str, &end, 16) : -1;
        return (str && end != str && *end == '\0') ? v : -1;
    }
    return json_node_get_int(n);
}


/** @brief Druh záznamu (`kind`) řádku, "" = neznámý. */
static const char *_goto_row_kind(JsonObject *row) {
    if (!row || !json_object_has_member(row, "kind")) return "";
    const char *k = json_object_get_string_member(row, "kind");
    return k ? k : "";
}


/**
 * @brief Cíl goto z řádků cputrack: čas, očekávaný stav a `after_int_ack`.
 *
 * Cílový záznam R = N (`before`) nebo N+1 (`after`); čas cíle t(R). Stav
 * v cíli = registry po R-1; je-li R přijetí přerušení, zastavení dojede až
 * za něj (stav po přijetí = registry po R, `after_int_ack`); synchronizační
 * záznam nese stav sám. Chybí-li N+1 (konec segmentu), čas = t(N) + délka N.
 *
 * @param req_id       ID požadavku.
 * @param tr           Objekt `trace` z parametrů.
 * @param[out] out_t   Cílový čas v taktech GDG.
 * @param[out] out_after_int_ack true = cíl je přijetí přerušení.
 * @param[out] out_expect Očekávaný stav (sloupce řádku) pro trace_check, NULL = bez kontroly (v2).
 * @param[out] out_expect_pc PC očekávané v cíli, -1 = neznámé.
 * @param[out] out_response Chybová odpověď.
 * @return MCP_DISPATCH_OK, jinak chyba (out_response).
 */
static en_MCP_DISPATCH_RESULT _goto_trace_target(int64_t req_id, JsonObject *tr, uint64_t *out_t,
                                                 bool *out_after_int_ack, JsonObject **out_expect,
                                                 int64_t *out_expect_pc, char **out_response) {
    *out_expect = NULL;
    *out_expect_pc = -1;
    *out_after_int_ack = false;
    const char *chan = _obj_is_string(tr, "channel") ? json_object_get_string_member(tr, "channel") : "cputrack";
    if (strcmp(chan, "cputrack") != 0) {
        return _err_response(req_id, "Invalid parameters: trace.channel must be cputrack",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 seq = _obj_int_or(tr, "seq", -1);
    if (seq < 0) {
        return _err_response(req_id, "Invalid parameters: trace.seq must be >= 0",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool after = false;
    if (json_object_has_member(tr, "at")) {
        const char *at = _obj_is_string(tr, "at") ? json_object_get_string_member(tr, "at") : NULL;
        if (!at || (strcmp(at, "before") != 0 && strcmp(at, "after") != 0)) {
            return _err_response(req_id, "Invalid parameters: trace.at must be before or after",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        after = (strcmp(at, "after") == 0);
    }

    /* Řádky N-1 .. N+1 (s registry, segment v2 bez nich). */
    gint64 from = seq > 0 ? seq - 1 : 0;
    JsonObject *page = NULL;
    for (int with_regs = 1; with_regs >= 0 && !page; with_regs--) {
        JsonObject *q = json_object_new();
        json_object_set_string_member(q, "channel", "cputrack");
        char fs[48];
        g_snprintf(fs, sizeof(fs), "seq:%" G_GINT64_FORMAT, from);
        json_object_set_string_member(q, "from", fs);
        json_object_set_int_member(q, "limit", 3);
        JsonArray *cols = json_array_new();
        static const char *const base[] = { "seq", "pc", "t_std", "wait", "t", "kind" };
        for (unsigned i = 0; i < G_N_ELEMENTS(base); i++) json_array_add_string_element(cols, base[i]);
        if (with_regs) {
            for (unsigned i = 0; i < G_N_ELEMENTS(s_goto_check_regs); i++) {
                json_array_add_string_element(cols, s_goto_check_regs[i].col);
            }
            json_object_set_string_member(q, "regs", "after");
        }
        json_object_set_array_member(q, "columns", cols);
        char *err = NULL;
        en_MCP_DISPATCH_RESULT rc = _trace_read_exec(req_id, q, &page, &err);
        json_object_unref(q);
        if (!page && err && strstr(err, "beyond next_seq")) {
            free(err);
            return _err_response(req_id, "Invalid parameters: trace.seq is not in the current cputrack segment",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        if (!page && (with_regs == 0 || !err || !strstr(err, "register"))) {
            /* Chyba nesouvisí s registry (nebo už bez nich): vrátit ji. */
            *out_response = err;
            return rc;
        }
        free(err);
    }
    JsonObject *prev = (seq > 0) ? _goto_trace_row(page, seq - 1) : NULL;
    JsonObject *rn = _goto_trace_row(page, seq);
    JsonObject *rn1 = _goto_trace_row(page, seq + 1);
    en_MCP_DISPATCH_RESULT rc = MCP_DISPATCH_OK;
    if (!rn) {
        rc = _err_response(req_id, "Invalid parameters: trace.seq is not in the current cputrack segment",
                           MCP_DISPATCH_INVALID_PARAMS, out_response);
        goto out;
    }
    if (_goto_row_int(rn, "t") < 0) {
        rc = _err_response(req_id, "this cputrack segment has no time (PC filter): use t instead",
                           MCP_DISPATCH_INVALID_PARAMS, out_response);
        goto out;
    }
    bool has_regs = json_object_has_member(rn, "af");
    JsonObject *r_target = after ? rn1 : rn;      /* R */
    JsonObject *r_before = after ? rn : prev;     /* R-1 */
    JsonObject *expect = NULL;
    if (!r_target) {
        /* Za koncem segmentu: konec záznamu N. */
        *out_t = (uint64_t)_goto_row_int(rn, "t")
                 + ((uint64_t)_goto_row_int(rn, "t_std") + (uint64_t)_goto_row_int(rn, "wait"))
                   * (uint64_t)GDGCLK2CPU_DIVIDER;
        expect = rn;
    } else {
        *out_t = (uint64_t)_goto_row_int(r_target, "t");
        const char *k = _goto_row_kind(r_target);
        if (strcmp(k, "int") == 0) {
            *out_after_int_ack = true;
            expect = r_target;
            /* PC po přijetí = adresa obsluhy (pc záznamu přijetí). */
            *out_expect_pc = _goto_row_int(r_target, "pc");
        } else if (strcmp(k, "sync") == 0 || !r_before) {
            expect = r_target;
            *out_expect_pc = _goto_row_int(r_target, "pc");
        } else {
            expect = r_before;
            *out_expect_pc = _goto_row_int(r_target, "pc");
        }
    }
    if (has_regs && expect) *out_expect = json_object_ref(expect);
out:
    if (prev) json_object_unref(prev);
    if (rn) json_object_unref(rn);
    if (rn1) json_object_unref(rn1);
    json_object_unref(page);
    return rc;
}


/**
 * @brief trace_check: registry ze záznamu proti dosaženému stavu (observe).
 *
 * @return Objekt {match, mismatch: {reg: [expected, got]}} (vlastní volající).
 */
static JsonObject *_goto_trace_check(JsonObject *expect, int64_t expect_pc, JsonObject *observe) {
    JsonObject *o = json_object_new();
    JsonObject *mis = json_object_new();
    JsonObject *regs = json_object_get_object_member(observe, "regs");
    bool match = true;
    for (unsigned i = 0; regs && i < G_N_ELEMENTS(s_goto_check_regs); i++) {
        int64_t want = _goto_row_int(expect, s_goto_check_regs[i].col);
        int64_t got = json_object_get_int_member(regs, s_goto_check_regs[i].reg);
        if (s_goto_check_regs[i].ir_part == 1) got = (got >> 8) & 0xFF;
        else if (s_goto_check_regs[i].ir_part == 2) got &= 0xFF;
        if (want < 0 || want == got) continue;
        match = false;
        JsonArray *pair = json_array_new();
        json_array_add_int_element(pair, want);
        json_array_add_int_element(pair, got);
        json_object_set_array_member(mis, s_goto_check_regs[i].col, pair);
    }
    if (regs && expect_pc >= 0 && json_object_get_int_member(regs, "PC") != expect_pc) {
        match = false;
        JsonArray *pair = json_array_new();
        json_array_add_int_element(pair, expect_pc);
        json_array_add_int_element(pair, json_object_get_int_member(regs, "PC"));
        json_object_set_array_member(mis, "pc", pair);
    }
    json_object_set_boolean_member(o, "match", match ? TRUE : FALSE);
    json_object_set_object_member(o, "mismatch", mis);
    return o;
}


/**
 * @brief `replay_goto` handler - stav stroje v libovolném čase přehrávaného záznamu.
 *
 * Parametry v `data` (adresa právě jedna z):
 *  - `t` (int): čas v taktech GDG (sloupec `t` trace, `time.ticks`
 *    ze `state_observe`, `stop_ticks`),
 *  - `frame` + `frame_ticks` (int): absolutní snímek a takt ve snímku
 *    (`time.frame`, `time.frame_ticks` ze `state_observe`),
 *  - `trace` {`channel` "cputrack", `seq`, `at` "before" (výchozí) | "after"}:
 *    záznam aktuálního segmentu cputrack (čas z jeho `t`).
 * Další: `observe` (objekt parametrů `state_observe`, výchozí bez oken),
 * `breakpoints` "suppress" (výchozí) | "keep", `checkpoint` (bool, uložit
 * checkpoint na hranici snímku seeku), `timeout_ms` (0..25000, výchozí
 * 20000; 0 = nečekat na dojetí).
 *
 * Postup: replay_request_goto() (seek a zastavení na čase v emu vlákně),
 * čekání na vyřízení, pak na konec běhu (run_log_wait()); po zastavení
 * `state_observe` a u adresy `trace` nad segmentem v3 `trace_check`.
 *
 * Response: `done` (false = dojetí ještě běží, `run_seq` pro `wait_stop`),
 * `reached` (zastaveno na cíli), `stopped_by`, `target_t`, `t` (čas
 * zastavení), `exact`, `after_int_ack` (jen `trace`), `seek_from`
 * ("checkpoint" / "restart" / "none") a `seek_from_frame`,
 * `checkpoint_saved`, `replay_frame`, `run_seq`, `observe`, `trace_check`.
 */
static en_MCP_DISPATCH_RESULT _handle_replay_goto(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _replay_data_obj(req);
    if (!obj) {
        return _err_response(req_id, "Invalid parameters: give t, frame + frame_ticks or trace",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    int n_addr = (json_object_has_member(obj, "t") ? 1 : 0)
               + (json_object_has_member(obj, "frame") ? 1 : 0)
               + (json_object_has_member(obj, "trace") ? 1 : 0);
    if (n_addr != 1) {
        return _err_response(req_id, "Invalid parameters: give exactly one of t, frame + frame_ticks, trace",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    bool keep = false;
    if (json_object_has_member(obj, "breakpoints")) {
        const char *b = _obj_is_string(obj, "breakpoints") ? json_object_get_string_member(obj, "breakpoints") : NULL;
        if (!b || (strcmp(b, "suppress") != 0 && strcmp(b, "keep") != 0)) {
            return _err_response(req_id, "Invalid parameters: breakpoints must be suppress or keep",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        keep = (strcmp(b, "keep") == 0);
    }
    bool ckpt = false;
    if (json_object_has_member(obj, "checkpoint")) {
        JsonNode *n = json_object_get_member(obj, "checkpoint");
        if (!n || json_node_get_value_type(n) != G_TYPE_BOOLEAN) {
            return _err_response(req_id, "Invalid parameters: checkpoint must be a boolean",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        ckpt = json_node_get_boolean(n) ? true : false;
    }
    gint64 timeout_ms = _obj_int_or(obj, "timeout_ms", MCP_REPLAY_GOTO_WAIT_DEFAULT_MS);
    if (timeout_ms < 0 || timeout_ms > MCP_REPLAY_GOTO_WAIT_MAX_MS) {
        return _err_response(req_id, "Invalid parameters: timeout_ms must be in range 0..25000",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *obs_params = NULL;
    if (json_object_has_member(obj, "observe")) {
        JsonNode *n = json_object_get_member(obj, "observe");
        if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
            return _err_response(req_id, "Invalid parameters: observe must be an object (state_observe parameters)",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        /* Kopie: doplnění windows nesmí měnit objekt požadavku. */
        JsonNode *copy = json_node_copy(n);
        obs_params = json_node_dup_object(copy);
        json_node_unref(copy);
    } else {
        obs_params = json_object_new();
    }
    if (!json_object_has_member(obs_params, "windows")) {
        json_object_set_array_member(obs_params, "windows", json_array_new());
    }

    uint64_t target = 0;
    bool from_trace = false, after_int_ack = false;
    JsonObject *expect = NULL;
    int64_t expect_pc = -1;
    en_MCP_DISPATCH_RESULT rc = MCP_DISPATCH_OK;
    if (json_object_has_member(obj, "t")) {
        gint64 t = _obj_int_or(obj, "t", -1);
        if (t < 0) {
            rc = _err_response(req_id, "Invalid parameters: t must be >= 0",
                               MCP_DISPATCH_INVALID_PARAMS, out_response);
            goto out;
        }
        target = (uint64_t)t;
    } else if (json_object_has_member(obj, "frame")) {
        gint64 f = _obj_int_or(obj, "frame", -1);
        gint64 ft = _obj_int_or(obj, "frame_ticks", -1);
        uint32_t per = (uint32_t)VIDEO_SCREEN_TICKS;
        if (f < 0 || ft < 0 || (uint64_t)ft >= per) {
            rc = _err_response(req_id, "Invalid parameters: frame >= 0 and frame_ticks in range 0..ticks per frame - 1",
                               MCP_DISPATCH_INVALID_PARAMS, out_response);
            goto out;
        }
        target = (uint64_t)f * per + (uint64_t)ft;
    } else {
        JsonNode *n = json_object_get_member(obj, "trace");
        if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
            rc = _err_response(req_id, "Invalid parameters: trace must be an object {channel, seq, at}",
                               MCP_DISPATCH_INVALID_PARAMS, out_response);
            goto out;
        }
        rc = _goto_trace_target(req_id, json_node_get_object(n), &target, &after_int_ack,
                                &expect, &expect_pc, out_response);
        if (rc != MCP_DISPATCH_OK) goto out;
        from_trace = true;
    }

    {
        uint64_t seq0 = run_log_current(NULL);
        uint32_t rseq = replay_request_goto(target, keep, ckpt);
        if (!replay_wait_request(rseq, MCP_REPLAY_REQUEST_TIMEOUT_MS)) {
            rc = _err_response(req_id, "replay request was not processed in time",
                               MCP_DISPATCH_EMU_ERROR, out_response);
            goto out;
        }
        st_REPLAY_STATUS st;
        replay_get_status(&st);
        if (!st.request_ok) {
            rc = _err_response(req_id, st.request_message[0] ? st.request_message : "replay_goto failed",
                               MCP_DISPATCH_EMU_ERROR, out_response);
            goto out;
        }

        /* Běh goto: požadavek se vyřizuje v pauze (běžící emulace se pro něj
         * nejdřív zastaví), takže dojetí je vždy běh seq0 + 1. Vyřízení
         * požadavku se ohlásí ještě v pauze, před rozběhem: běh se pozná
         * podle goto_active (goto běží), nebo podle toho, že už začal
         * (rychlé dojetí mohlo i skončit). Bez běhu goto skončilo hned. */
        uint64_t goto_run = seq0 + 1;
        uint64_t cur = run_log_current(NULL);
        bool ran = st.goto_active || cur >= goto_run;
        bool done = true;
        st_RUN_LOG_RECORD rec;
        run_log_record_clear(&rec);
        bool have_rec = false;
        if (ran) {
            bool found = false;
            en_RUN_LOG_WAIT w = run_log_wait(goto_run, (int)timeout_ms, &rec, &found);
            done = (w == RUN_LOG_WAIT_DONE);
            have_rec = done && found;
            cur = goto_run;
        }
        replay_get_status(&st);

        JsonObject *resp = json_object_new();
        json_object_set_boolean_member(resp, "done", done ? TRUE : FALSE);
        json_object_set_int_member(resp, "target_t", (gint64)target);
        json_object_set_int_member(resp, "run_seq", (gint64)cur);
        if (st.goto_seek_from >= 0) {
            json_object_set_string_member(resp, "seek_from", "checkpoint");
            json_object_set_int_member(resp, "seek_from_frame", (gint64)st.goto_seek_from);
        } else {
            json_object_set_string_member(resp, "seek_from",
                st.goto_seek_from == REPLAY_GOTO_FROM_RESTART ? "restart" : "none");
        }
        json_object_set_boolean_member(resp, "checkpoint_saved", st.goto_checkpoint ? TRUE : FALSE);
        if (from_trace) json_object_set_boolean_member(resp, "after_int_ack", after_int_ack ? TRUE : FALSE);
        if (!done) {
            json_object_set_boolean_member(resp, "running", TRUE);
            rc = _ok_response(req_id, resp, out_response);
            goto out;
        }
        const char *stopped_by = have_rec ? rec.reason : "replay_goto";
        json_object_set_string_member(resp, "stopped_by", stopped_by);
        json_object_set_boolean_member(resp, "reached", strcmp(stopped_by, "replay_goto") == 0 ? TRUE : FALSE);

        JsonObject *observe = NULL;
        char *oerr = NULL;
        en_MCP_DISPATCH_RESULT orc = _state_observe_exec(req_id, obs_params, &observe, &oerr);
        if (!observe) {
            json_object_unref(resp);
            *out_response = oerr;
            rc = orc;
            goto out;
        }
        JsonObject *tm = json_object_get_object_member(observe, "time");
        uint64_t landed = tm ? (uint64_t)json_object_get_int_member(tm, "ticks") : 0;
        json_object_set_int_member(resp, "t", (gint64)landed);
        json_object_set_boolean_member(resp, "exact", landed == target ? TRUE : FALSE);
        json_object_set_int_member(resp, "replay_frame", (gint64)st.frame);
        if (expect) {
            json_object_set_object_member(resp, "trace_check", _goto_trace_check(expect, expect_pc, observe));
        }
        json_object_set_object_member(resp, "observe", observe);
        rc = _ok_response(req_id, resp, out_response);
    }
out:
    if (expect) json_object_unref(expect);
    json_object_unref(obs_params);
    return rc;
}


/**
 * @brief Co otisk stroje nevidí nikdy (machine_hash.h), jména pro `not_covered`.
 */
static const char *const s_machine_hash_never[] = {
    "screen", "audio", "plotter", "host_joystick_live", "unicard_directory", "qd_virtual_directory",
};


/**
 * @brief `machine_hash` handler - obsahový otisk stroje po částech.
 *
 * Parametry:
 *  - `parts` (pole jmen, volitelné): `cpu`, `ram`, `memext`, `vram`,
 *    `pcg`, `periph`, `memmap`, `rom`, `media`, `inputs`, `ramdisk`, `cmt`;
 *    chybí = všechny.
 *  - `include_media` (bool, default false): do částí `media` a `ramdisk`
 *    i obsah obrazů médií, FLASH MemExt a RAM disků (může být drahé).
 *
 * Emu vlákno spočítá otisk v jednom průchodu fronty
 * (DBGAPI_CMD_MACHINE_HASH): za běhu na hranici snímku, v pauze okamžitě.
 * Otisk má smysl porovnávat jen v pauze (`stable: true`) se stejnými
 * parametry.
 *
 * Response: `sha256` (celkový otisk vyžádaných částí), `parts` {jméno:
 * hex, null = část v sestavě není}, `covered` (jména částí s hashem),
 * `not_covered` (co otisk nevidí; bez `include_media` i `media_content`
 * a `ramdisk_content`), `include_media`, `state_rev`, `run_seq`,
 * `running`, `stable`, `elapsed_us` (doba výpočtu v emu vlákně).
 *
 * Chyby: neznámé jméno části nebo špatný typ parametru =
 * `Invalid parameters: ...`.
 */
static en_MCP_DISPATCH_RESULT _handle_machine_hash(const st_JSONL_MESSAGE *req,
                                                   char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                      ? json_node_get_object(data_node) : NULL;

    uint32_t mask = MACHINE_HASH_ALL_PARTS;
    if (obj && json_object_has_member(obj, "parts")
        && !json_node_is_null(json_object_get_member(obj, "parts"))) {
        JsonNode *pn = json_object_get_member(obj, "parts");
        if (json_node_get_node_type(pn) != JSON_NODE_ARRAY) {
            return _err_response(req_id, "Invalid parameters: parts must be an array of part names",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonArray *a = json_node_get_array(pn);
        mask = 0;
        for (guint i = 0; i < json_array_get_length(a); i++) {
            JsonNode *e = json_array_get_element(a, i);
            int part = -1;
            if (e && JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING) {
                part = machine_hash_part_from_name(json_node_get_string(e));
            }
            if (part < 0) {
                return _err_response(req_id, "Invalid parameters: unknown part in parts (allowed: cpu, ram, "
                                     "memext, vram, pcg, periph, memmap, rom, media, inputs, ramdisk, cmt)",
                                     MCP_DISPATCH_INVALID_PARAMS, out_response);
            }
            mask |= 1u << (unsigned)part;
        }
    }
    bool include_media = false;
    if (!_replay_obj_bool(obj, "include_media", false, &include_media)) {
        return _err_response(req_id, "Invalid parameters: include_media must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_MACHINE_HASH_PARAM *p = g_new0(st_DBGAPI_MACHINE_HASH_PARAM, 1);
    p->mask = mask;
    p->flags = include_media ? MACHINE_HASH_FLAG_INCLUDE_MEDIA : 0;
    if (!_submit_dbgapi(DBGAPI_CMD_MACHINE_HASH, p, NULL)) {
        g_free(p);
        return _err_response(req_id, "MACHINE_HASH failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }

    JsonObject *data = json_object_new();
    json_object_set_string_member(data, "sha256", p->result.total);
    JsonObject *parts = json_object_new();
    JsonArray *covered = json_array_new();
    for (int i = 0; i < MACHINE_HASH_PART_COUNT; i++) {
        if (!(p->result.requested & (1u << i))) continue;
        const char *name = machine_hash_part_name((en_MACHINE_HASH_PART)i);
        if (p->result.present & (1u << i)) {
            json_object_set_string_member(parts, name, p->result.part[i]);
            json_array_add_string_element(covered, name);
        } else {
            json_object_set_null_member(parts, name);
        }
    }
    json_object_set_object_member(data, "parts", parts);
    json_object_set_array_member(data, "covered", covered);
    JsonArray *never = json_array_new();
    for (guint i = 0; i < G_N_ELEMENTS(s_machine_hash_never); i++) {
        json_array_add_string_element(never, s_machine_hash_never[i]);
    }
    if (!include_media) {
        json_array_add_string_element(never, "media_content");
        json_array_add_string_element(never, "ramdisk_content");
    }
    json_object_set_array_member(data, "not_covered", never);
    json_object_set_boolean_member(data, "include_media", include_media ? TRUE : FALSE);
    /* Revize a běh z okamžiku výpočtu (výstupy slotu, state_rev.h). */
    if (s_submit_track.have_rev) {
        json_object_set_int_member(data, "state_rev", (gint64)s_submit_track.state_rev);
        json_object_set_int_member(data, "run_seq", (gint64)s_submit_track.run_seq);
        json_object_set_boolean_member(data, "running", s_submit_track.running ? TRUE : FALSE);
        json_object_set_boolean_member(data, "stable", s_submit_track.running ? FALSE : TRUE);
    }
    json_object_set_int_member(data, "elapsed_us", (gint64)p->elapsed_us);
    g_free(p);
    return _ok_response(req_id, data, out_response);
}


/**
 * @brief `replay_status` handler - stav záznamu a přehrávání (lokální, bez fronty).
 */
static en_MCP_DISPATCH_RESULT _handle_replay_status(const st_JSONL_MESSAGE *req,
                                                    char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    return _ok_response(req_id, _replay_status_to_json(&st), out_response);
}


/* ---------------- Pokrytí kódu (coverage, M11 debug-gaps) ---------------- */

/** @brief Výchozí počet řádků stránky coverage_read. */
#define COVERAGE_READ_DEFAULT_LIMIT 500
/** @brief Nejvyšší počet řádků stránky coverage_read. */
#define COVERAGE_READ_MAX_LIMIT 5000
/** @brief Rezerva znaků na obálku stránky (stav, kurzor) nad řádky. */
#define COVERAGE_READ_ENVELOPE_CHARS 600

/**
 * @brief Provede DBGAPI_CMD_COVERAGE.
 * @return false = příkaz se nepodařilo předat; jinak true a chyba operace v p->out_error.
 */
static bool _cov_submit(st_DBGAPI_COVERAGE_PARAM *p) {
    p->out_error[0] = '\0';
    bool ok = _submit_dbgapi(DBGAPI_CMD_COVERAGE, p, NULL);
    return ok || p->out_error[0];
}

/** @brief Provede operaci bez dat a odpoví stavem (u MARK s novou `epoch`). */
static en_MCP_DISPATCH_RESULT _cov_op_response(int64_t req_id, st_DBGAPI_COVERAGE_PARAM *p,
                                               char **out_response) {
    if (!_cov_submit(p)) {
        return _err_response(req_id, "coverage command failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (p->out_error[0]) {
        return _err_response(req_id, p->out_error, MCP_DISPATCH_EMU_ERROR, out_response);
    }
    return _ok_response(req_id, coverage_status_to_json(&p->out_status), out_response);
}

/**
 * @brief `coverage_start` handler - spustí sběr pokrytí kódu.
 *
 * Parametry (volitelné): `level` ("branch" výchozí | "detail"), `key`
 * ("opcode" výchozí | "full_bytes", jen detail), `capacity` (1..1048576
 * jednotek, výchozí 65536, jen detail), `reset` (bool, výchozí false =
 * pokračovat do stávajících dat se stejným nastavením),
 * `reset_on_discontinuity` (bool, výchozí false). Response: stav.
 */
static en_MCP_DISPATCH_RESULT _handle_coverage_start(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    st_DBGAPI_COVERAGE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_COVERAGE_OP_START;
    p.cfg.level = COVERAGE_LEVEL_BRANCH;
    p.cfg.key = COVERAGE_KEY_OPCODE;
    p.cfg.capacity = COVERAGE_DEFAULT_CAPACITY;
    int hl = _videorec_member(obj, "level", G_TYPE_STRING);
    int hk = _videorec_member(obj, "key", G_TYPE_STRING);
    int hc = _videorec_member(obj, "capacity", G_TYPE_INT64);
    int hr = _videorec_member(obj, "reset", G_TYPE_BOOLEAN);
    int hd = _videorec_member(obj, "reset_on_discontinuity", G_TYPE_BOOLEAN);
    if (hl < 0 || hk < 0 || hc < 0 || hr < 0 || hd < 0) {
        return _err_response(req_id, "Invalid parameters: level and key must be strings, capacity an integer, "
                             "reset and reset_on_discontinuity booleans",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (hl) {
        const char *l = json_object_get_string_member(obj, "level");
        if (g_strcmp0(l, "branch") == 0) p.cfg.level = COVERAGE_LEVEL_BRANCH;
        else if (g_strcmp0(l, "detail") == 0) p.cfg.level = COVERAGE_LEVEL_DETAIL;
        else return _err_response(req_id, "Invalid parameters: level must be branch or detail",
                                  MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (hk) {
        const char *k = json_object_get_string_member(obj, "key");
        if (g_strcmp0(k, "opcode") == 0) p.cfg.key = COVERAGE_KEY_OPCODE;
        else if (g_strcmp0(k, "full_bytes") == 0) p.cfg.key = COVERAGE_KEY_FULL_BYTES;
        else return _err_response(req_id, "Invalid parameters: key must be opcode or full_bytes",
                                  MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (hc) {
        gint64 c = json_object_get_int_member(obj, "capacity");
        if (c < 1 || c > (gint64)COVERAGE_MAX_CAPACITY) {
            return _err_response(req_id, "Invalid parameters: capacity must be 1..1048576",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        p.cfg.capacity = (uint32_t)c;
    }
    p.reset = hr ? json_object_get_boolean_member(obj, "reset") : false;
    p.cfg.reset_on_discontinuity = hd ? json_object_get_boolean_member(obj, "reset_on_discontinuity") : false;
    return _cov_op_response(req_id, &p, out_response);
}

/** @brief `coverage_stop` handler - zastaví sběr, data zůstanou čitelná. */
static en_MCP_DISPATCH_RESULT _handle_coverage_stop(const st_JSONL_MESSAGE *req, char **out_response) {
    st_DBGAPI_COVERAGE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_COVERAGE_OP_STOP;
    return _cov_op_response(jsonl_msg_get_req_id(req), &p, out_response);
}

/** @brief `coverage_reset` handler - smaže data (nová generace), sběr běží dál, pokud běžel. */
static en_MCP_DISPATCH_RESULT _handle_coverage_reset(const st_JSONL_MESSAGE *req, char **out_response) {
    st_DBGAPI_COVERAGE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_COVERAGE_OP_RESET;
    return _cov_op_response(jsonl_msg_get_req_id(req), &p, out_response);
}

/** @brief `coverage_status` handler - stav sběru a úplnost dat. */
static en_MCP_DISPATCH_RESULT _handle_coverage_status(const st_JSONL_MESSAGE *req, char **out_response) {
    st_DBGAPI_COVERAGE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_COVERAGE_OP_STATUS;
    return _cov_op_response(jsonl_msg_get_req_id(req), &p, out_response);
}

/** @brief `coverage_mark` handler - nová epocha; stav obsahuje její číslo (`epoch`). */
static en_MCP_DISPATCH_RESULT _handle_coverage_mark(const st_JSONL_MESSAGE *req, char **out_response) {
    st_DBGAPI_COVERAGE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_COVERAGE_OP_MARK;
    return _cov_op_response(jsonl_msg_get_req_id(req), &p, out_response);
}

/**
 * @brief Snímek dat pokrytí z emu vlákna.
 * @return Nový snímek (_cov_snapshot_free()), nebo NULL při chybě.
 */
static st_COVERAGE_SNAPSHOT *_cov_snapshot(void) {
    st_DBGAPI_COVERAGE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_COVERAGE_OP_SNAPSHOT;
    p.snap = g_new0(st_COVERAGE_SNAPSHOT, 1);
    if (!_cov_submit(&p) || p.out_error[0]) {
        coverage_snapshot_clear(p.snap);
        g_free(p.snap);
        return NULL;
    }
    return p.snap;
}

/** @brief Uvolní snímek z _cov_snapshot(). */
static void _cov_snapshot_free(st_COVERAGE_SNAPSHOT *s) {
    if (!s) return;
    coverage_snapshot_clear(s);
    g_free(s);
}

/**
 * @brief Naparsuje filtr coverage_read.
 * @param fo     Objekt `filter` (smí být NULL).
 * @param detail Data jsou úrovně detail (filtr `space` jinak nedává smysl).
 * @param out    Výstup.
 * @return NULL OK, jinak statická chybová zpráva.
 */
static const char *_cov_parse_filter(JsonObject *fo, bool detail, st_COVERAGE_FILTER *out) {
    coverage_filter_init(out);
    if (!fo) return NULL;
    int hf = _videorec_member(fo, "pc_from", G_TYPE_INT64);
    int ht = _videorec_member(fo, "pc_to", G_TYPE_INT64);
    int hs = _videorec_member(fo, "space", G_TYPE_STRING);
    int he = _videorec_member(fo, "new_since_epoch", G_TYPE_INT64);
    if (hf < 0 || ht < 0 || hs < 0 || he < 0) {
        return "pc_from, pc_to and new_since_epoch must be integers, space a string";
    }
    gint64 from = hf ? json_object_get_int_member(fo, "pc_from") : 0;
    gint64 to = ht ? json_object_get_int_member(fo, "pc_to") : 0xFFFF;
    if (from < 0 || to < from || to > 0xFFFF) return "filter must satisfy 0 <= pc_from <= pc_to <= 65535";
    out->pc_from = (uint16_t)from;
    out->pc_to = (uint16_t)to;
    if (hs) {
        if (!detail) return "filter space needs level detail";
        if (!coverage_space_parse(json_object_get_string_member(fo, "space"), &out->space)) {
            return "space must be ram, vram, pcg, memext, rom or bus";
        }
        out->has_space = true;
    }
    if (he) {
        gint64 e = json_object_get_int_member(fo, "new_since_epoch");
        if (e < 0 || e > COVERAGE_EPOCH_MAX) return "new_since_epoch must be 0..65535";
        out->has_epoch = true;
        out->epoch_from = (uint32_t)e;
    }
    if (json_object_has_member(fo, "state") && !json_node_is_null(json_object_get_member(fo, "state"))) {
        JsonNode *sn = json_object_get_member(fo, "state");
        if (JSON_NODE_HOLDS_VALUE(sn) && json_node_get_value_type(sn) == G_TYPE_STRING) {
            out->state_mask = coverage_state_bits(json_node_get_string(sn));
            if (!out->state_mask) return "unknown state (none, unobserved, taken_only, not_taken_only, both, partial, decision)";
        } else if (JSON_NODE_HOLDS_ARRAY(sn)) {
            JsonArray *a = json_node_get_array(sn);
            for (guint i = 0; i < json_array_get_length(a); i++) {
                JsonNode *e = json_array_get_element(a, i);
                unsigned b = (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                             ? coverage_state_bits(json_node_get_string(e)) : 0;
                if (!b) return "unknown state (none, unobserved, taken_only, not_taken_only, both, partial, decision)";
                out->state_mask |= b;
            }
        } else {
            return "state must be a string or an array of strings";
        }
    }
    if (json_object_has_member(fo, "flags") && !json_node_is_null(json_object_get_member(fo, "flags"))) {
        JsonNode *fn = json_object_get_member(fo, "flags");
        if (!JSON_NODE_HOLDS_ARRAY(fn)) return "flags must be an array of flag names";
        JsonArray *a = json_node_get_array(fn);
        for (guint i = 0; i < json_array_get_length(a); i++) {
            JsonNode *e = json_array_get_element(a, i);
            const char *nm = (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                             ? json_node_get_string(e) : NULL;
            uint16_t bit = coverage_flag_from_name(nm);
            if (!bit) return "unknown flag name";
            out->flags_any |= bit;
        }
    }
    return NULL;
}

/** @brief Délka JSON textu uzlu (místo, které zabere ve stránce). */
static size_t _cov_node_chars(JsonNode *n) {
    gchar *t = json_to_string(n, FALSE);
    size_t l = t ? strlen(t) : 0;
    g_free(t);
    return l;
}

/**
 * @brief `coverage_read` handler - stránka řádků pokrytí.
 *
 * Řádek = adresa (úroveň branch, jen vykonané adresy) nebo jednotka
 * (detail), seřazené podle PC (u detail dál podle buňky a klíče).
 * Parametry: `cursor` (z `next_cursor`), `columns` (pole jmen, výchozí
 * všechny sloupce úrovně), `filter` (`pc_from`, `pc_to`, `space`,
 * `state`, `new_since_epoch`, `flags`), `limit` (1..5000, výchozí 500),
 * `max_chars` (1000..45000, výchozí 20000). Kurzor má tvar `t1.` jako
 * trace_read (kanál "coverage", segment = generace dat, seq = PC << 16 |
 * pořadí řádku v rámci PC). Řádky přidané za běhu sběru na adresu před
 * kurzorem se v dalších stránkách neobjeví (čtení od začátku je ukáže).
 * Po coverage_reset nebo novém startu chyba `segment_replaced:`.
 */
static en_MCP_DISPATCH_RESULT _handle_coverage_read(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *d = _videorec_data_obj(req);
    gint64 limit = _obj_int_or(d, "limit", COVERAGE_READ_DEFAULT_LIMIT);
    if (limit < 1 || limit > COVERAGE_READ_MAX_LIMIT) {
        return _err_response(req_id, "Invalid parameters: limit must be 1..5000", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 max_chars = _obj_int_or(d, "max_chars", TRACE_READ_DEFAULT_MAX_CHARS);
    if (max_chars < 1000 || max_chars > TRACE_READ_MAX_MAX_CHARS) {
        return _err_response(req_id, "Invalid parameters: max_chars must be 1000..45000", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *cursor = (d && _obj_is_string(d, "cursor")) ? json_object_get_string_member(d, "cursor") : NULL;
    bool have_cursor = cursor && cursor[0];
    unsigned cur_gen = 0;
    uint64_t cur_seq = 0;
    if (have_cursor && !trace_cursor_decode(cursor, "coverage", &cur_gen, &cur_seq)) {
        return _err_response(req_id, "Invalid parameters: invalid cursor", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_COVERAGE_SNAPSHOT *snap = _cov_snapshot();
    if (!snap) {
        return _err_response(req_id, "coverage snapshot failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    size_t n = 0, n_all = 0, n_cols = 0;
    st_COVERAGE_ROW *rows = coverage_rows_build(snap, &n);
    bool detail = (snap->status.level == COVERAGE_LEVEL_DETAIL);
    const char *const *all = coverage_columns(snap->status.level, &n_all);
    const char *perr = NULL;
    char *owned_err = NULL;
    const char **cols = NULL;
    JsonObject *resp = NULL;

    if (have_cursor && cur_gen != snap->status.generation) {
        owned_err = g_strdup_printf("segment_replaced: coverage data generation %u replaced by %u; read again from the start",
                                    cur_gen, (unsigned)snap->status.generation);
        goto out;
    }

    /* Sloupce */
    if (d && json_object_has_member(d, "columns") && !json_node_is_null(json_object_get_member(d, "columns"))) {
        JsonNode *cn = json_object_get_member(d, "columns");
        if (!JSON_NODE_HOLDS_ARRAY(cn)) {
            perr = "Invalid parameters: columns must be an array of strings";
            goto out;
        }
        JsonArray *ca = json_node_get_array(cn);
        cols = g_new0(const char *, json_array_get_length(ca) + 1);
        for (guint i = 0; i < json_array_get_length(ca); i++) {
            JsonNode *e = json_array_get_element(ca, i);
            const char *nm = (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                             ? json_node_get_string(e) : "";
            const char *found = NULL;
            for (size_t k = 0; k < n_all; k++) {
                if (strcmp(nm, all[k]) == 0) found = all[k];
            }
            if (!found) {
                owned_err = g_strdup_printf("Invalid parameters: unknown column '%s' for level %s",
                                            nm, coverage_level_name(snap->status.level));
                goto out;
            }
            cols[n_cols++] = found;
        }
    } else {
        cols = g_new0(const char *, n_all + 1);
        for (size_t k = 0; k < n_all; k++) cols[n_cols++] = all[k];
    }

    /* Filtr */
    JsonObject *fo = NULL;
    if (d && json_object_has_member(d, "filter") && !json_node_is_null(json_object_get_member(d, "filter"))) {
        JsonNode *fn = json_object_get_member(d, "filter");
        if (!JSON_NODE_HOLDS_OBJECT(fn)) {
            perr = "Invalid parameters: filter must be an object";
            goto out;
        }
        fo = json_node_get_object(fn);
    }
    st_COVERAGE_FILTER f;
    const char *ferr = _cov_parse_filter(fo, detail, &f);
    if (ferr) {
        owned_err = g_strdup_printf("Invalid parameters: %s", ferr);
        goto out;
    }

    /* Začátek stránky podle kurzoru: první řádek s (pc, k) >= (seq >> 16, seq & FFFF). */
    size_t i = 0;
    if (have_cursor) {
        uint32_t cpc = (uint32_t)(cur_seq >> 16);
        uint32_t ck = (uint32_t)(cur_seq & 0xFFFFu);
        while (i < n && (rows[i].pc < cpc || (rows[i].pc == cpc && rows[i].k < ck))) i++;
    }

    JsonArray *out_rows = json_array_new();
    size_t chars = COVERAGE_READ_ENVELOPE_CHARS;
    gint64 returned = 0;
    bool page_full = false;
    while (i < n) {
        if (!coverage_row_match(snap, &rows[i], &f)) {
            i++;
            continue;
        }
        JsonArray *ra = json_array_new();
        for (size_t c = 0; c < n_cols; c++) json_array_add_element(ra, coverage_row_column(snap, &rows[i], cols[c]));
        JsonNode *rn = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(rn, ra);
        size_t row_chars = _cov_node_chars(rn) + 1;
        if (returned > 0 && (returned >= limit || chars + row_chars > (size_t)max_chars)) {
            json_node_free(rn);
            page_full = true;
            break;
        }
        json_array_add_element(out_rows, rn);
        chars += row_chars;
        returned++;
        i++;
    }

    resp = coverage_status_to_json(&snap->status);
    JsonArray *ca = json_array_new();
    for (size_t c = 0; c < n_cols; c++) json_array_add_string_element(ca, cols[c]);
    json_object_set_array_member(resp, "columns", ca);
    json_object_set_array_member(resp, "rows", out_rows);
    json_object_set_int_member(resp, "returned", returned);
    json_object_set_boolean_member(resp, "page_full", page_full);
    json_object_set_boolean_member(resp, "end_of_data", i >= n);
    uint64_t next_seq = (i < n) ? (((uint64_t)rows[i].pc << 16) | rows[i].k) : ((uint64_t)0x10000 << 16);
    char *nc = trace_cursor_encode("coverage", (unsigned)snap->status.generation, next_seq);
    json_object_set_string_member(resp, "next_cursor", nc);
    g_free(nc);

out:
    g_free(cols);
    g_free(rows);
    _cov_snapshot_free(snap);
    if (owned_err) {
        return _trace_err_owned(req_id, owned_err, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (perr) {
        return _err_response(req_id, perr, MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    return _ok_response(req_id, resp, out_response);
}

/**
 * @brief `coverage_export` handler - všechny řádky pokrytí do souboru.
 *
 * Parametry: `path` (povinný, soubor se přepíše), `format` ("json"
 * výchozí | "csv"); obsah viz coverage_export_write(). Filtr se
 * nepoužije. Response: stav + `path`, `format`, `rows`. Chyba zápisu =>
 * EMU_ERROR s textem.
 */
static en_MCP_DISPATCH_RESULT _handle_coverage_export(const st_JSONL_MESSAGE *req, char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonObject *obj = _videorec_data_obj(req);
    if (_videorec_member(obj, "path", G_TYPE_STRING) != 1 || !json_object_get_string_member(obj, "path")[0]) {
        return _err_response(req_id, "Invalid parameters: path must be a non-empty string", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    int hf = _videorec_member(obj, "format", G_TYPE_STRING);
    const char *fmt = (hf == 1) ? json_object_get_string_member(obj, "format") : "json";
    if (hf < 0 || (g_strcmp0(fmt, "json") != 0 && g_strcmp0(fmt, "csv") != 0)) {
        return _err_response(req_id, "Invalid parameters: format must be json or csv", MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    const char *path = json_object_get_string_member(obj, "path");
    st_COVERAGE_SNAPSHOT *snap = _cov_snapshot();
    if (!snap) {
        return _err_response(req_id, "coverage snapshot failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    size_t rows = 0;
    char *err = NULL;
    bool ok = coverage_export_write(snap, path, g_strcmp0(fmt, "csv") == 0, &rows, &err);
    JsonObject *resp = ok ? coverage_status_to_json(&snap->status) : NULL;
    _cov_snapshot_free(snap);
    if (!ok) {
        return _trace_err_owned(req_id, err, MCP_DISPATCH_EMU_ERROR, out_response);
    }
    json_object_set_string_member(resp, "path", path);
    json_object_set_string_member(resp, "format", fmt);
    json_object_set_int_member(resp, "rows", (gint64)rows);
    return _ok_response(req_id, resp, out_response);
}


/* ------------------------------------------------------------------ */
/* get_devices (dev-api D1): zařízení devbus                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Hodnota volby zařízení jako JSON (BOOL true/false, KEYWORD slovo, UNSIGNED číslo).
 * @param o     Volba.
 * @param value Hodnota (platná pro volbu).
 * @return Nový JsonNode (vlastní volající).
 */
static JsonNode *_devbus_option_value_node(const st_DEVBUS_OPTION *o, unsigned value) {
    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    if (o->type == DEVBUS_OPT_BOOL) {
        json_node_set_boolean(n, value ? TRUE : FALSE);
    } else if (o->type == DEVBUS_OPT_KEYWORD) {
        json_node_set_string(n, o->keywords[value]);
    } else {
        json_node_set_int(n, (gint64)value);
    }
    return n;
}


/**
 * @brief Pole stavů mapy MMIO ("mapped", "dram") podle masky.
 * @param when Maska DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM.
 * @return Nové pole (vlastní volající).
 */
static JsonArray *_devbus_when_array(unsigned when) {
    JsonArray *a = json_array_new();
    if (when & DEVBUS_MMIO_MAPPED) json_array_add_string_element(a, "mapped");
    if (when & DEVBUS_MMIO_DRAM) json_array_add_string_element(a, "dram");
    return a;
}


/**
 * @brief `get_devices` handler - volitelná zařízení devbus a jejich stav.
 *
 * Volá DBGAPI_CMD_DEVBUS_LIST (dynamický stav z emu vlákna); neměnné údaje
 * čte z deskriptorů (registr je po startu neměnný). Response:
 *   {"platform_supported": bool, "devices": [{"id", "name", "connected",
 *    "ini_section", "options": [{"key", "label", "type", "value",
 *    "default", "apply", "keywords" | "min"/"max"}], "ports": [{"first",
 *    "last", "read", "write", "drive", "drive_mask"}], "mmio": [{"first",
 *    "last", "when", "read", "write", "drive", "drive_mask"}],
 *    "regions": [{"name", "kind": "device", "sub_id", "size", "writable"}],
 *    "video": {"glyph_source", "render_row"}, "outputs": [{"id", "title",
 *    "width", "height"}]}]}
 * Na MZ-800 a MZ-1500 (devbus neinicializovaný) devices = [].
 */
static en_MCP_DISPATCH_RESULT _handle_get_devices(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    st_DBGAPI_DEVBUS_LIST_PARAM *p = g_new0(st_DBGAPI_DEVBUS_LIST_PARAM, 1);
    if (!_submit_dbgapi(DBGAPI_CMD_DEVBUS_LIST, p, NULL)) {
        g_free(p);
        return _err_response(req_id, "get_devices failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_boolean_member(resp, "platform_supported", p->initialized ? TRUE : FALSE);
    JsonArray *devs = json_array_new();
    for (unsigned i = 0; i < p->count && i < DEVBUS_MAX_DEVICES; i++) {
        const st_DEVBUS_DEVICE_DESC *d = devbus_desc((int)i);
        if (!d) continue;
        const st_DBGAPI_DEVBUS_DEV *s = &p->dev[i];
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "id", d->id);
        json_object_set_string_member(o, "name", d->name);
        json_object_set_boolean_member(o, "connected", s->connected ? TRUE : FALSE);
        json_object_set_string_member(o, "ini_section", d->ini_section);

        JsonArray *opts = json_array_new();
        for (unsigned k = 0; k < d->option_count; k++) {
            const st_DEVBUS_OPTION *op = &d->options[k];
            JsonObject *jo = json_object_new();
            json_object_set_string_member(jo, "key", op->key);
            json_object_set_string_member(jo, "label", op->label);
            json_object_set_string_member(jo, "type",
                op->type == DEVBUS_OPT_BOOL ? "bool" : op->type == DEVBUS_OPT_KEYWORD ? "keyword" : "unsigned");
            json_object_set_member(jo, "value", _devbus_option_value_node(op, s->opt[k]));
            json_object_set_member(jo, "default", _devbus_option_value_node(op, op->def));
            json_object_set_string_member(jo, "apply", op->apply == DEVBUS_APPLY_LIVE ? "live" : "connect");
            if (op->type == DEVBUS_OPT_KEYWORD) {
                JsonArray *kw = json_array_new();
                for (unsigned w = 0; op->keywords[w]; w++) json_array_add_string_element(kw, op->keywords[w]);
                json_object_set_array_member(jo, "keywords", kw);
            } else if (op->type == DEVBUS_OPT_UNSIGNED) {
                json_object_set_int_member(jo, "min", (gint64)op->min);
                json_object_set_int_member(jo, "max", (gint64)op->max);
            }
            json_array_add_object_element(opts, jo);
        }
        json_object_set_array_member(o, "options", opts);

        JsonArray *ports = json_array_new();
        for (unsigned k = 0; k < d->port_count; k++) {
            JsonObject *jp = json_object_new();
            json_object_set_int_member(jp, "first", d->ports[k].first);
            json_object_set_int_member(jp, "last", d->ports[k].last);
            json_object_set_boolean_member(jp, "read", d->ports[k].read ? TRUE : FALSE);
            json_object_set_boolean_member(jp, "write", d->ports[k].write ? TRUE : FALSE);
            json_object_set_string_member(jp, "drive", devbus_drive_name(d->ports[k].drive));
            json_object_set_int_member(jp, "drive_mask",
                (gint64) devbus_drive_mask(d->ports[k].drive, d->ports[k].drive_mask));
            json_array_add_object_element(ports, jp);
        }
        json_object_set_array_member(o, "ports", ports);

        JsonArray *mmio = json_array_new();
        for (unsigned k = 0; k < d->mmio_count; k++) {
            JsonObject *jm = json_object_new();
            json_object_set_int_member(jm, "first", d->mmio[k].first);
            json_object_set_int_member(jm, "last", d->mmio[k].last);
            json_object_set_array_member(jm, "when", _devbus_when_array(s->mmio_when[k]));
            json_object_set_boolean_member(jm, "read", d->mmio[k].read ? TRUE : FALSE);
            json_object_set_boolean_member(jm, "write", d->mmio[k].write ? TRUE : FALSE);
            json_object_set_string_member(jm, "drive", devbus_drive_name(d->mmio[k].drive));
            json_object_set_int_member(jm, "drive_mask",
                (gint64) devbus_drive_mask(d->mmio[k].drive, d->mmio[k].drive_mask));
            json_array_add_object_element(mmio, jm);
        }
        json_object_set_array_member(o, "mmio", mmio);

        JsonArray *regs = json_array_new();
        for (unsigned k = 0; k < d->region_count; k++) {
            JsonObject *jr = json_object_new();
            json_object_set_string_member(jr, "name", d->regions[k].name);
            json_object_set_string_member(jr, "kind", "device");
            json_object_set_int_member(jr, "sub_id", (gint64)(i * 16u + k));
            json_object_set_int_member(jr, "size", (gint64)d->regions[k].size);
            json_object_set_boolean_member(jr, "writable", d->regions[k].writable ? TRUE : FALSE);
            json_array_add_object_element(regs, jr);
        }
        json_object_set_array_member(o, "regions", regs);

        JsonObject *vid = json_object_new();
        json_object_set_boolean_member(vid, "glyph_source", (d->video && d->video->glyph_source) ? TRUE : FALSE);
        json_object_set_boolean_member(vid, "render_row", (d->video && d->video->render_row) ? TRUE : FALSE);
        json_object_set_object_member(o, "video", vid);

        JsonArray *outs = json_array_new();
        for (unsigned v = 0; v < devbus_vout_count(); v++) {
            st_DEVBUS_VOUT *vo = devbus_vout_at(v);
            if (devbus_vout_slot(vo) != (int)i) continue;
            const st_DEVBUS_VOUT_DESC *vd = devbus_vout_desc(vo);
            JsonObject *jv = json_object_new();
            json_object_set_string_member(jv, "id", vd->id);
            json_object_set_string_member(jv, "title", vd->title);
            json_object_set_int_member(jv, "width", vd->width);
            json_object_set_int_member(jv, "height", vd->height);
            json_array_add_object_element(outs, jv);
        }
        json_object_set_array_member(o, "outputs", outs);

        json_array_add_object_element(devs, o);
    }
    json_object_set_array_member(resp, "devices", devs);
    g_free(p);
    return _ok_response(req_id, resp, out_response);
}


/* ------------------------------------------------------------------ */
/* get_bus_map, bus_peek (bus-model D6): mapa vlastníků sběrnice        */
/* ------------------------------------------------------------------ */

/** @brief JSON jednoho vlastníka adresy snímku Bus Map. */
static JsonObject *_bus_owner_to_json(const st_BUS_OWNER *o, bool mmio) {
    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "id", o->id);
    json_object_set_string_member(j, "name", o->name);
    json_object_set_string_member(j, "kind", o->kind == BUS_OWNER_DEVBUS ? "devbus" : "static");
    if (o->kind == BUS_OWNER_DEVBUS) json_object_set_int_member(j, "slot", o->devbus_slot);
    json_object_set_string_member(j, "dir",
        o->dirs == BUS_DIR_RW ? "rw" : (o->dirs & BUS_DIR_R) ? "r" : "w");
    json_object_set_string_member(j, "drive", devbus_drive_name(o->drive));
    json_object_set_int_member(j, "drive_mask", o->drive_mask);
    json_object_set_boolean_member(j, "connected", o->connected ? TRUE : FALSE);
    JsonArray *range = json_array_new();
    json_array_add_int_element(range, o->range_first);
    json_array_add_int_element(range, o->range_last);
    json_object_set_array_member(j, "range", range);
    if (mmio) json_object_set_array_member(j, "when", _devbus_when_array(o->when_mask));
    if (o->source_note) json_object_set_string_member(j, "source_note", o->source_note);
    if (o->mirror_note) json_object_set_string_member(j, "mirror_note", o->mirror_note);
    return j;
}


/** @brief JSON pole jmen příznaků BUS_PORT_*. */
static JsonArray *_bus_flags_to_json(unsigned flags) {
    JsonArray *a = json_array_new();
    for (unsigned bit = 1; bit <= BUS_PORT_GHOST_SEEN; bit <<= 1) {
        const char *n = bus_map_flag_name(bit);
        if ((flags & bit) && n) json_array_add_string_element(a, n);
    }
    return a;
}


/**
 * @brief `get_bus_map` handler - mapa vlastníků adres sběrnice.
 *
 * Read-only, povolen při replay. Parametry (data, volitelné):
 *   - `include` (pole stringů) - které části: "ports", "mmio", "devices",
 *     "activity" (výchozí všechny); "activity" doplní čítače a poslední
 *     hodnoty (platné jen při `tracking_active`),
 *   - `only_mapped` (bool, výchozí false) - vynechat porty / buňky bez
 *     vlastníka.
 * Response: {"platform": "mz700", "supported": bool, "devbus_supported":
 *   bool, "conflict_policy", "bus_idle", "bus_idle_value", "idle_byte",
 *   "tracking_active", "mmio_mapped_now", "frame", "devbus": {"registered",
 *   "connected"}, "ports": [{"port", "owners": [...], "flags": [...],
 *   "last_in": {"value", "age_frames"} | null, "last_out": ..., "hits_in_per_sec",
 *   "hits_out_per_sec", "total_in", "total_out", "ghost_hits",
 *   "conflict_hits"}], "mmio": [{"addr", "owners", "flags", "when",
 *   "last_write", "hits_out_per_sec"}], "devices": [{"id", "name", "kind",
 *   "connected", "slot", "ports", "mmio", "drive", "glyph_source",
 *   "render_row", "regions", "outputs"}]}.
 * `supported` true na všech platformách (statická tabulka desky),
 * `devbus_supported` jen na MZ-700 (rozšiřující zařízení).
 */
static en_MCP_DISPATCH_RESULT _handle_get_bus_map(const st_JSONL_MESSAGE *req,
                                                  char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                      ? json_node_get_object(data_node) : NULL;
    unsigned want = 0;
    bool only_mapped = false;
    if (obj && json_object_has_member(obj, "include")) {
        JsonNode *n = json_object_get_member(obj, "include");
        if (!n || !JSON_NODE_HOLDS_ARRAY(n)) {
            return _err_response(req_id, "include must be an array of strings",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        JsonArray *a = json_node_get_array(n);
        for (guint i = 0; i < json_array_get_length(a); i++) {
            JsonNode *e = json_array_get_element(a, i);
            const char *s = (e && JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                            ? json_node_get_string(e) : NULL;
            if (!s) return _err_response(req_id, "include must be an array of strings",
                                         MCP_DISPATCH_INVALID_PARAMS, out_response);
            if (g_strcmp0(s, "ports") == 0) want |= BUS_WANT_PORTS;
            else if (g_strcmp0(s, "mmio") == 0) want |= BUS_WANT_MMIO;
            else if (g_strcmp0(s, "devices") == 0) want |= BUS_WANT_DEVICES;
            else if (g_strcmp0(s, "activity") == 0) want |= BUS_WANT_ACTIVITY;
            else return _err_response(req_id, "include item must be ports, mmio, devices or activity",
                                      MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    } else {
        want = BUS_WANT_ALL;
    }
    if (obj && json_object_has_member(obj, "only_mapped")) {
        JsonNode *n = json_object_get_member(obj, "only_mapped");
        if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) {
            return _err_response(req_id, "only_mapped must be a boolean",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
        only_mapped = json_node_get_boolean(n) ? true : false;
    }

    st_BUS_MAP_SNAPSHOT *snap = g_new0(st_BUS_MAP_SNAPSHOT, 1);
    st_DBGAPI_BUS_MAP_PARAM p;
    p.want = want;
    p.snap = snap;
    if (!_submit_dbgapi(DBGAPI_CMD_BUS_MAP, &p, NULL)) {
        g_free(snap);
        return _err_response(req_id, "get_bus_map failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }

    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "platform", MZARCH_NAME);
    json_object_set_boolean_member(resp, "supported", snap->supported ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "devbus_supported", snap->devbus_initialized ? TRUE : FALSE);
    json_object_set_string_member(resp, "conflict_policy", devbus_conflict_name(snap->conflict_policy));
    json_object_set_string_member(resp, "bus_idle", devbus_idle_name(snap->bus_idle));
    json_object_set_int_member(resp, "bus_idle_value", snap->bus_idle_value);
    json_object_set_int_member(resp, "idle_byte", snap->idle_byte);
    json_object_set_boolean_member(resp, "tracking_active", snap->tracking_active ? TRUE : FALSE);
    json_object_set_boolean_member(resp, "mmio_mapped_now", snap->mmio_mapped_now ? TRUE : FALSE);
    json_object_set_int_member(resp, "frame", (gint64) snap->frame);
    {
        JsonObject *d = json_object_new();
        json_object_set_int_member(d, "registered", (gint64) snap->devbus_registered);
        json_object_set_int_member(d, "connected", (gint64) snap->devbus_connected);
        json_object_set_object_member(resp, "devbus", d);
    }
    bool with_act = (want & BUS_WANT_ACTIVITY) != 0;
    if (want & BUS_WANT_PORTS) {
        JsonArray *ports = json_array_new();
        for (unsigned i = 0; i < 256; i++) {
            const st_BUS_PORT *bp = &snap->ports[i];
            if (only_mapped && bp->owner_count == 0) continue;
            JsonObject *jp = json_object_new();
            json_object_set_int_member(jp, "port", (gint64) i);
            JsonArray *owners = json_array_new();
            for (unsigned k = 0; k < bp->owner_count; k++) json_array_add_object_element(owners, _bus_owner_to_json(&bp->owners[k], false));
            json_object_set_array_member(jp, "owners", owners);
            json_object_set_array_member(jp, "flags", _bus_flags_to_json(bp->flags));
            if (with_act) {
                if (bp->last_read_valid) {
                    JsonObject *l = json_object_new();
                    json_object_set_int_member(l, "value", bp->last_read_value);
                    json_object_set_int_member(l, "age_frames", (gint64) bp->last_read_age);
                    json_object_set_object_member(jp, "last_in", l);
                } else {
                    json_object_set_null_member(jp, "last_in");
                }
                if (bp->last_write_valid) {
                    JsonObject *l = json_object_new();
                    json_object_set_int_member(l, "value", bp->last_write_value);
                    json_object_set_int_member(l, "age_frames", (gint64) bp->last_write_age);
                    json_object_set_object_member(jp, "last_out", l);
                } else {
                    json_object_set_null_member(jp, "last_out");
                }
                json_object_set_int_member(jp, "hits_in_per_sec", (gint64) bp->hits_in_per_sec);
                json_object_set_int_member(jp, "hits_out_per_sec", (gint64) bp->hits_out_per_sec);
                json_object_set_int_member(jp, "total_in", (gint64) bp->total_in);
                json_object_set_int_member(jp, "total_out", (gint64) bp->total_out);
                json_object_set_int_member(jp, "ghost_hits", (gint64) bp->ghost_hits);
            }
            json_object_set_int_member(jp, "conflict_hits", (gint64) bp->conflict_hits);
            json_array_add_object_element(ports, jp);
        }
        json_object_set_array_member(resp, "ports", ports);
    }
    if (want & BUS_WANT_MMIO) {
        JsonArray *cells = json_array_new();
        for (unsigned i = 0; i < 256; i++) {
            const st_BUS_MMIO_CELL *c = &snap->mmio[i];
            if (only_mapped && c->owner_count == 0) continue;
            JsonObject *jc = json_object_new();
            json_object_set_int_member(jc, "addr", (gint64) c->addr);
            JsonArray *owners = json_array_new();
            for (unsigned k = 0; k < c->owner_count; k++) json_array_add_object_element(owners, _bus_owner_to_json(&c->owners[k], true));
            json_object_set_array_member(jc, "owners", owners);
            json_object_set_array_member(jc, "flags", _bus_flags_to_json(c->flags));
            json_object_set_array_member(jc, "when", _devbus_when_array(c->when_mask));
            if (with_act) {
                if (c->last_write_valid) {
                    JsonObject *l = json_object_new();
                    json_object_set_int_member(l, "value", c->last_write_value);
                    json_object_set_int_member(l, "age_frames", (gint64) c->last_write_age);
                    json_object_set_object_member(jc, "last_write", l);
                } else {
                    json_object_set_null_member(jc, "last_write");
                }
                json_object_set_int_member(jc, "hits_out_per_sec", (gint64) c->hits_out_per_sec);
            }
            json_array_add_object_element(cells, jc);
        }
        json_object_set_array_member(resp, "mmio", cells);
    }
    if (want & BUS_WANT_DEVICES) {
        JsonArray *devs = json_array_new();
        for (unsigned i = 0; i < snap->device_count; i++) {
            const st_BUS_DEVICE *dv = &snap->devices[i];
            JsonObject *jd = json_object_new();
            json_object_set_string_member(jd, "id", dv->id);
            json_object_set_string_member(jd, "name", dv->name ? dv->name : "");
            json_object_set_string_member(jd, "kind", dv->kind == BUS_OWNER_DEVBUS ? "devbus" : "static");
            json_object_set_boolean_member(jd, "connected", dv->connected ? TRUE : FALSE);
            if (dv->kind == BUS_OWNER_DEVBUS) json_object_set_int_member(jd, "slot", dv->devbus_slot);
            json_object_set_int_member(jd, "ports", dv->port_count);
            json_object_set_int_member(jd, "mmio", dv->mmio_count);
            json_object_set_string_member(jd, "drive", devbus_drive_name(dv->drive));
            json_object_set_boolean_member(jd, "glyph_source", dv->has_glyph ? TRUE : FALSE);
            json_object_set_boolean_member(jd, "render_row", dv->has_row ? TRUE : FALSE);
            json_object_set_int_member(jd, "regions", dv->region_count);
            json_object_set_int_member(jd, "outputs", dv->vout_count);
            json_array_add_object_element(devs, jd);
        }
        json_object_set_array_member(resp, "devices", devs);
    }
    g_free(snap);
    return _ok_response(req_id, resp, out_response);
}


/**
 * @brief `bus_peek` handler - čtení adresy bez vedlejších efektů s rezolucí sběrnice.
 *
 * Read-only, povolen při replay. Parametry: `port` (0-65535, dekód podle
 * dolního bajtu) nebo `addr` (MMIO adresa 0E000h-0E0FFh, jen MZ-700);
 * právě jeden z nich.
 * Response: {"port" | "addr", "value", "driven_mask", "source": "idle" |
 *   "board" | "devbus"}. `driven_mask` 0 = nikdo nebudí, `value` je model
 * nebuzené sběrnice (latch nebo konstanta). Na rozdíl od `io_read` nemá
 * vedlejší efekty (peek zařízení, zrcadla katalogu).
 */
static en_MCP_DISPATCH_RESULT _handle_bus_peek(const st_JSONL_MESSAGE *req,
                                               char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                      ? json_node_get_object(data_node) : NULL;
    bool has_port = obj && json_object_has_member(obj, "port");
    bool has_addr = obj && json_object_has_member(obj, "addr");
    if (has_port == has_addr) {
        return _err_response(req_id, "exactly one of port or addr is required",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    gint64 a = _obj_int_or(obj, has_port ? "port" : "addr", -1);
    if (a < 0 || a > 0xFFFF) {
        return _err_response(req_id, "port / addr must be in range 0..65535",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (has_addr && (a < 0xE000 || a > 0xE0FF)) {
        return _err_response(req_id, "addr must be in the MMIO window 0xE000..0xE0FF",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    st_DBGAPI_BUS_PEEK_PARAM p;
    memset(&p, 0, sizeof(p));
    p.addr = (uint16_t) a;
    p.mmio = has_addr ? 1 : 0;
    if (!_submit_dbgapi(DBGAPI_CMD_BUS_PEEK, &p, NULL)) {
        return _err_response(req_id, "bus_peek failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (!p.supported) {
        return _err_response(req_id, "MMIO peek is available only on the MZ-700 core",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    JsonObject *resp = json_object_new();
    json_object_set_int_member(resp, has_port ? "port" : "addr", a);
    json_object_set_int_member(resp, "value", p.value);
    json_object_set_int_member(resp, "driven_mask", p.driven);
    json_object_set_string_member(resp, "source",
        p.source == DBGAPI_BUS_PEEK_SRC_DEVBUS ? "devbus" : p.source == DBGAPI_BUS_PEEK_SRC_BOARD ? "board" : "idle");
    return _ok_response(req_id, resp, out_response);
}


/* ------------------------------------------------------------------ */
/* device_screenshot (kp80 D4): obraz sekundárního výstupu zařízení    */
/* ------------------------------------------------------------------ */

/**
 * @brief `device_screenshot` handler - obraz sekundárního výstupu zařízení devbus.
 *
 * Sekundární výstup je vlastní obraz zařízení (druhý monitor karty K&P 80),
 * nezávislý na obrazu počítače. Handler pošle DBGAPI_CMD_DEVBUS_VOUT_READ
 * (emu vlákno zkopíruje poslední předaný snímek přes
 * devbus_vout_read_last()) a obraz převede podle neměnné palety výstupu.
 *
 * Parametry (data, všechny volitelné):
 *   - `output`    (string) - id výstupu z get_devices (`outputs[].id`);
 *                 chybí nebo "" = první výstup připojeného zařízení,
 *   - `format`    (string) - "png" (výchozí) nebo "indexed" (bajt na bod =
 *                 index palety, po řádcích shora; plus pole `palette`),
 *   - `path`      (string) - zapsat PNG do souboru na disku serveru místo
 *                 `data_b64` (jen s formátem "png"),
 *   - `want_data` (bool, výchozí true) - false = bez `data_b64`.
 *
 * Response (available = true):
 *   {"available": true, "output": str, "device": str, "width": N,
 *    "height": N, "pixel_aspect": float, "seq": N, "image_sha256": str,
 *    "lit_pixels": N, "format": "png" | "indexed", "byte_size": N,
 *    "data_b64": str | "path": str, "palette": ["#RRGGBB", ...] (indexed)}
 *   `image_sha256` = SHA-256 obrazu RGBA (4 B na bod po řádcích), tedy
 *   i barvy palety; `lit_pixels` = počet bodů s indexem palety různým od 0;
 *   `seq` = pořadí snímku výstupu (roste s každým nakresleným snímkem).
 * Response (available = false): {"available": false, "output": str,
 *   "device": str, "reason": str} - zařízení není připojené nebo výstup
 *   zatím nemá snímek.
 *
 * Chyby: neznámý výstup, žádné připojené zařízení s výstupem (bez
 * `output`), neplatný formát = INVALID_PARAMS; zápis souboru = EMU_ERROR.
 *
 * @param req          Požadavek.
 * @param out_response Výstup: JSON odpovědi (vlastní volající).
 * @return Výsledek dispatch.
 */
static en_MCP_DISPATCH_RESULT _handle_device_screenshot(const st_JSONL_MESSAGE *req,
                                                        char **out_response) {
    int64_t req_id = jsonl_msg_get_req_id(req);
    JsonNode *data_node = (JsonNode *)jsonl_msg_get_data_node(req);
    JsonObject *data_obj = (data_node && json_node_get_node_type(data_node) == JSON_NODE_OBJECT)
                           ? json_node_get_object(data_node) : NULL;

    bool want_data = true;
    if (!_parse_want_data(data_obj, &want_data)) {
        return _err_response(req_id, "want_data must be a boolean",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    char *output = data_obj ? _obj_str_dup(data_obj, "output") : NULL;
    char *format = data_obj ? _obj_str_dup(data_obj, "format") : NULL;
    char *path = data_obj ? _obj_str_dup(data_obj, "path") : NULL;
    bool indexed = false;
    if (format && format[0] && g_ascii_strcasecmp(format, "png") != 0) {
        if (g_ascii_strcasecmp(format, "indexed") == 0) {
            indexed = true;
        } else {
            g_free(output); g_free(format); g_free(path);
            return _err_response(req_id, "'format' must be 'png' or 'indexed'",
                                 MCP_DISPATCH_INVALID_PARAMS, out_response);
        }
    }
    g_free(format);
    if (path && !path[0]) { g_free(path); path = NULL; }
    if (path && indexed) {
        g_free(output); g_free(path);
        return _err_response(req_id, "'path' is supported only with format 'png'",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }
    if (output && strlen(output) > DEVBUS_ID_MAX) {
        g_free(output); g_free(path);
        return _err_response(req_id, "unknown 'output' (see get_devices outputs)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    st_DBGAPI_DEVBUS_VOUT_READ_PARAM *p = g_new0(st_DBGAPI_DEVBUS_VOUT_READ_PARAM, 1);
    if (output) g_strlcpy(p->output, output, sizeof(p->output));
    bool had_output = output && output[0];
    g_free(output);
    if (!_submit_dbgapi(DBGAPI_CMD_DEVBUS_VOUT_READ, p, NULL)) {
        g_free(p->pixels); g_free(p); g_free(path);
        return _err_response(req_id, "device_screenshot failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    if (!p->found || !p->out_vout) {
        g_free(p->pixels); g_free(p); g_free(path);
        return _err_response(req_id, had_output
                             ? "unknown 'output' (see get_devices outputs)"
                             : "no connected device has a video output (see get_devices)",
                             MCP_DISPATCH_INVALID_PARAMS, out_response);
    }

    const st_DEVBUS_VOUT_DESC *vd = devbus_vout_desc(p->out_vout);
    const st_DEVBUS_DEVICE_DESC *dd = devbus_desc(p->slot);
    JsonObject *resp = json_object_new();
    json_object_set_string_member(resp, "output", vd->id);
    json_object_set_string_member(resp, "device", dd ? dd->id : "");
    if (!p->available) {
        json_object_set_boolean_member(resp, "available", FALSE);
        json_object_set_string_member(resp, "reason", p->connected
                                      ? "the output has no frame yet"
                                      : "the device is not connected");
        g_free(p->pixels); g_free(p); g_free(path);
        return _ok_response(req_id, resp, out_response);
    }

    /* Indexy -> RGBA podle palety (index mimo paletu = černá). */
    size_t npix = (size_t)vd->width * vd->height;
    uint8_t *rgba = g_malloc(npix * 4);
    gint64 lit = 0;
    for (size_t i = 0; i < npix; i++) {
        uint8_t ix = p->pixels[i];
        uint32_t c = (ix < vd->palette_size) ? vd->palette[ix] : 0x000000FFu;
        if (ix) lit++;
        rgba[i * 4 + 0] = (uint8_t)(c >> 24);
        rgba[i * 4 + 1] = (uint8_t)(c >> 16);
        rgba[i * 4 + 2] = (uint8_t)(c >> 8);
        rgba[i * 4 + 3] = (uint8_t)c;
    }
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, rgba, npix * 4);
    json_object_set_boolean_member(resp, "available", TRUE);
    json_object_set_int_member(resp, "width", vd->width);
    json_object_set_int_member(resp, "height", vd->height);
    json_object_set_double_member(resp, "pixel_aspect", (double)vd->pixel_aspect);
    json_object_set_int_member(resp, "seq", (gint64)p->seq);
    json_object_set_string_member(resp, "image_sha256", sha);
    json_object_set_int_member(resp, "lit_pixels", lit);
    g_free(sha);

    if (indexed) {
        json_object_set_string_member(resp, "format", "indexed");
        json_object_set_int_member(resp, "byte_size", (gint64)npix);
        JsonArray *pal = json_array_new();
        for (unsigned i = 0; i < vd->palette_size; i++) {
            char hex[8];
            g_snprintf(hex, sizeof(hex), "#%06X", (unsigned)(vd->palette[i] >> 8));
            json_array_add_string_element(pal, hex);
        }
        json_object_set_array_member(resp, "palette", pal);
        if (want_data) {
            gchar *b64 = g_base64_encode(p->pixels, npix);
            json_object_set_string_member(resp, "data_b64", b64);
            g_free(b64);
        }
        g_free(rgba); g_free(p->pixels); g_free(p);
        return _ok_response(req_id, resp, out_response);
    }

    size_t png_len = 0;
    uint8_t *png = png_encode_rgba(rgba, vd->width, vd->height, &png_len);
    g_free(rgba);
    g_free(p->pixels);
    g_free(p);
    if (!png) {
        json_object_unref(resp);
        g_free(path);
        return _err_response(req_id, "PNG encoding failed", MCP_DISPATCH_EMU_ERROR, out_response);
    }
    json_object_set_string_member(resp, "format", "png");
    json_object_set_int_member(resp, "byte_size", (gint64)png_len);
    if (path) {
        GError *werr = NULL;
        gboolean wrote = g_file_set_contents(path, (const gchar *)png, (gssize)png_len, &werr);
        g_free(png);
        if (!wrote) {
            char *msg = g_strdup_printf("Cannot write screenshot to '%s': %s",
                                        path, werr ? werr->message : "unknown");
            if (werr) g_error_free(werr);
            g_free(path);
            json_object_unref(resp);
            en_MCP_DISPATCH_RESULT r = _err_response(req_id, msg, MCP_DISPATCH_EMU_ERROR, out_response);
            g_free(msg);
            return r;
        }
        json_object_set_string_member(resp, "path", path);
        g_free(path);
        return _ok_response(req_id, resp, out_response);
    }
    if (want_data) {
        gchar *b64 = g_base64_encode(png, png_len);
        json_object_set_string_member(resp, "data_b64", b64);
        g_free(b64);
    }
    g_free(png);
    return _ok_response(req_id, resp, out_response);
}
#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
