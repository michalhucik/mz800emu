/*
 * dbgapi.c — implementace Debugger API (oba kanály: CMDRQ + MSG)
 *
 * Obsahuje:
 * - Inicializace a destrukce CMDRQ fronty
 * - EMU strana: kontrola fronty, dequeue, dispatch, complete, send_msg
 * - UI strana: submit_cmd_sync, kontrola stavu, MSG callback registrace
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */
#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "app/app_thread.h"
#include "dbgapi_cmdrq.h"
#include "dbgapi_msg.h"
#include "dbgapi_emu.h"
#include "dbgapi_ui.h"
#include "dbgapi_regions.h"
#include "debugger.h"
#include "emulator.h"
#include "device_request.h"
#include "customspeed.h"
#include "bptmap.h"
#include "breakpoints.h"
#include "stack_regions.h"
#include "stack_history.h"
#include "callstack.h"
#include "profiler.h"
#include "profiler_pc.h"
#include "audio_capture.h"
#include "watch.h"
#include "watch_emu_cache.h"  /* V1.D.2.C: dispatch-side mirror lookup */
#include "bp_vars.h"
#include "bookmarks/bookmarks.h"
#include "bp_expr.h"
#include "bp_action.h"         /* značka kanálů trace řízených akcí BP */
#include "mhmap.h"
#include "freeze/freeze.h"
#include "memsearch/memsearch.h"
#include "run_control/run_control.h"
#include "../replay.h"
#include "run_control/run_log.h"
#include "dbgapi_cmd_props.h"
#include "state_rev.h"
#include "state_hash.h"
#include "png_encode.h"
#include "screenshot/screenshot_capture.h"
#include "screenshot/screenshot_series.h"
#include "trace/eventlog.h"
#include "trace/eventlog_trigger.h"
#include "eventlog_decoder.h"
#include "io_history.h"
#include "io_activity.h"
#include "trace/cputrack.h"
#include "trace/iorqlog.h"
#include "trace/intlog.h"
#include "trace/marklog.h"
#include "trace/hwlog.h"
#include "snapshot/snapshot.h"
#include "videorec/videorec.h"
#include "iface/iface_audio.h"
#include "symbols/sym_db.h"
#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform.h"
#include "mzarch/mzarch_platform_functions.h"
#include "libs/dasm-z80/z80_dasm.h"
#include "gdg/video.h"
#include "memory/memory.h"
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdg.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_gdg.h"
#elif MZARCH == 700
#include "mzarch/mz700/gdg/mz700_gdg.h"
#endif
#if HAVE_PIOZ80
#include "hw-generic/pioz80/pioz80.h"
#endif
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/ctc8253/ctc8253.h"
#if HAVE_PSG >= 1
#include "hw-generic/psg/psg.h"
#endif
#if MZARCH == 800
#include "mzarch/mz800/mz800_main.h"
#include "mzarch/mz800/mz800_iorq.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/mz1500_main.h"
#include "mzarch/mz1500/mz1500_iorq.h"
#elif MZARCH == 700
#include "mzarch/mz700/mz700_main.h"
#include "mzarch/mz700/mz700_iorq.h"
#endif
#include "libs/cpu-z80/z80.h"

/* V1.B.1 - Media Tools (mcp-server mutant). Handler v switchi pro
 * DBGAPI_CMD_MEDIA_* dispatchuje na jednotlivé hw-generic API. Periferie
 * jsou per-arch volitelné přes CFG_HWEXT_HAVE_* makra v mzarch_config.h. */
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmthack.h"
#include "libs/mztape/mztape.h"
/* fix mzdos 0008: media_load_mzf zrcadlí bootstrap.c plný load (header +
 * post-header mapping + body). Potřebujeme post_header per-arch + MZF
 * header strukturu. */
#include "mzarch/bootstrap.h"
#include "libs/mzf/mzf.h"
#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#endif
#if CFG_HWEXT_HAVE_QDISK
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/qdisk/qdisk_request.h"
#endif
#if CFG_HWEXT_HAVE_IDE8
#include "hw-generic/ide8/ide8.h"
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "mcp/event_bus.h"
/* V1.C.1 - HID Tools: VKBD matrix press/release + joystick state. */
#include "mcp/hid_keymap.h"
/* mcp-inbox-fixes: sekvence vstupu odměřená snímky emulace. */
#include "mcp/hid_script.h"
/* V1.D.1 - Core + CPU extras Resources: čte memext + memory + z80 state. */
#include "hw-generic/memory/memext.h"
/* V1.D.4 - Input + Frame Resources: joystick, framebuffer, VRAM read. */
#if HAVE_JOY
#include "hw-generic/joy/joy.h"
#endif
#include "iface/iface_video.h"
#include "hw-generic/gdg/framebuffer.h"

/**
 * @brief Porovnání adres symbolů pro qsort (vzestupně).
 */
static int dbgapi_profiler_pc_addr_cmp ( const void *a, const void *b )
{
    uint16_t x = *(const uint16_t *) a, y = *(const uint16_t *) b;
    return (int) x - (int) y;
}


/**
 * @brief Najde ke každé adrese nejbližší symbol (adresa symbolu <= adresa).
 *
 * Bere jen globální symboly (bank_id 0, CPU pohled), stejně jako MCP
 * symbol_lookup. Při více symbolech na jedné adrese rozhoduje priorita
 * sym_db_lookup_by_addr (LBL > MAP > NOI > SYM). Používá profiler podle
 * PC i DBGAPI_CMD_SYMBOL_RESOLVE_ADDRS (čtení trace).
 *
 * @param pcs  Adresy.
 * @param n    Počet adres.
 * @param out  Výstup n položek.
 * @post Adresa bez symbolu má jméno "" a offset 0.
 * @note Volá se v emu vlákně jako ostatní MCP příkazy nad sym_db
 *       (symbol_lookup, symbol_list).
 */
static void dbgapi_symbols_resolve ( const uint16_t *pcs, size_t n, st_DBGAPI_SYMBOL_HIT *out )
{
    for ( size_t i = 0; i < n; i++ ) {
        out[ i ].name[ 0 ] = '\0';
        out[ i ].offset = 0;
    };
    size_t count = sym_db_count ( );
    if ( n == 0 || count == 0 ) return;
    uint16_t *addrs = (uint16_t *) g_malloc ( count * sizeof ( uint16_t ) );
    size_t na = 0;
    st_SYM_DB_ITER it;
    sym_db_iter_init ( &it );
    const st_SYMBOL *s;
    while ( ( s = sym_db_iter_next ( &it ) ) != NULL && na < count ) {
        if ( s->bank_id != 0 || s->addr > 0xFFFFu ) continue;
        addrs[ na++ ] = (uint16_t) s->addr;
    };
    qsort ( addrs, na, sizeof ( uint16_t ), dbgapi_profiler_pc_addr_cmp );
    for ( size_t i = 0; i < n; i++ ) {
        uint16_t pc = pcs[ i ];
        /* Binární hledání poslední adresy <= pc. */
        size_t lo = 0, hi = na;
        while ( lo < hi ) {
            size_t mid = ( lo + hi ) / 2;
            if ( addrs[ mid ] <= pc ) lo = mid + 1;
            else hi = mid;
        };
        if ( lo == 0 ) continue;
        uint16_t sa = addrs[ lo - 1 ];
        const st_SYMBOL *sym = sym_db_lookup_by_addr ( sa, 0 );
        if ( !sym || !sym->name ) continue;
        g_strlcpy ( out[ i ].name, sym->name, sizeof ( out[ i ].name ) );
        out[ i ].offset = (uint32_t) ( pc - sa );
    };
    g_free ( addrs );
}


/* Jména značek v DBGAPI_CMD_TRACE_RESOLVE mají stejný limit jako registr marklog. */
G_STATIC_ASSERT ( DBGAPI_MARKER_NAME_MAX == MARKLOG_NAME_MAX );


/**
 * @brief Doplní nejbližší symbol (CPU pohled, banka 0) k řádkům profileru.
 *
 * @param rows  Řádky (pole symbol/sym_offset se přepíšou).
 * @param n     Počet řádků.
 * @pre Volat v emu vlákně (databáze symbolů).
 */
static void dbgapi_profiler_pc_fill_symbols ( st_DBGAPI_PROFILER_PC_ROW *rows, size_t n )
{
    if ( n == 0 ) return;
    uint16_t *pcs = (uint16_t *) g_malloc ( n * sizeof ( uint16_t ) );
    st_DBGAPI_SYMBOL_HIT *hits = (st_DBGAPI_SYMBOL_HIT *) g_malloc ( n * sizeof ( *hits ) );
    for ( size_t i = 0; i < n; i++ ) pcs[ i ] = rows[ i ].r.pc;
    dbgapi_symbols_resolve ( pcs, n, hits );
    for ( size_t i = 0; i < n; i++ ) {
        g_strlcpy ( rows[ i ].symbol, hits[ i ].name, sizeof ( rows[ i ].symbol ) );
        rows[ i ].sym_offset = hits[ i ].offset;
    };
    g_free ( hits );
    g_free ( pcs );
}


/**
 * @brief Převede délky pulzů (sekundy) na pole µs v pořadí hlavičky CMTSPEED.
 *
 * @param pulses Délky pulzů (nesmí být NULL).
 * @param[out] us Pole 4 hodnot: LONG high, LONG low, SHORT high, SHORT low.
 */
static void dbgapi_cmt_pulses_to_us ( const st_MZTAPE_PULSES_LENGTH *pulses, double us[ 4 ] )
{
    us[ 0 ] = pulses->long_pulse.high * 1e6;
    us[ 1 ] = pulses->long_pulse.low * 1e6;
    us[ 2 ] = pulses->short_pulse.high * 1e6;
    us[ 3 ] = pulses->short_pulse.low * 1e6;
}
#endif

/* V1.B.2 - cfgmain INI handle pro Settings + Periph attach handlery. */
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "libs/cfgfile/cfgcommon.h"
#include "debugger/trace/ramlog_hook.h"
#include "debugger/trace/bplog.h"
#include "debugger/coverage/coverage_hook.h"
#if MZARCH == 700
#include "mzarch/mz700/devbus/mz700_devbus.h"
#endif


#if CFG_HWEXT_HAVE_FDC
/**
 * @brief Mapuje FDC media slot na instanci řadiče a mechaniku.
 *
 * Sloty fdc0_fd0..3 -> g_fdc[FDC0] mechanika 0..3, fdc1_fd0..3 ->
 * g_fdc[FDC1] mechanika 0..3.
 *
 * @param slot      media slot enum.
 * @param out_fdc   OUT: ukazatel na instanci FDC (platný jen při návratu 1).
 * @param out_drive OUT: index mechaniky 0..3 (platný jen při návratu 1).
 * @return 1 pokud je `slot` FDC slot, 0 jinak.
 */
static int dbgapi_media_slot_to_fdc ( en_DBGAPI_MEDIA_SLOT slot,
                                      st_FDC **out_fdc, unsigned *out_drive )
{
    switch ( slot )
    {
        case DBGAPI_MEDIA_SLOT_FDC0_FD0: *out_fdc = &g_fdc[FDC0]; *out_drive = 0; return 1;
        case DBGAPI_MEDIA_SLOT_FDC0_FD1: *out_fdc = &g_fdc[FDC0]; *out_drive = 1; return 1;
        case DBGAPI_MEDIA_SLOT_FDC0_FD2: *out_fdc = &g_fdc[FDC0]; *out_drive = 2; return 1;
        case DBGAPI_MEDIA_SLOT_FDC0_FD3: *out_fdc = &g_fdc[FDC0]; *out_drive = 3; return 1;
        case DBGAPI_MEDIA_SLOT_FDC1_FD0: *out_fdc = &g_fdc[FDC1]; *out_drive = 0; return 1;
        case DBGAPI_MEDIA_SLOT_FDC1_FD1: *out_fdc = &g_fdc[FDC1]; *out_drive = 1; return 1;
        case DBGAPI_MEDIA_SLOT_FDC1_FD2: *out_fdc = &g_fdc[FDC1]; *out_drive = 2; return 1;
        case DBGAPI_MEDIA_SLOT_FDC1_FD3: *out_fdc = &g_fdc[FDC1]; *out_drive = 3; return 1;
        default: return 0;
    }
}
#endif


/* ============================================================================
 * GLOBÁLNÍ INSTANCE CMDRQ FRONTY
 * ============================================================================ */

st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


/* ============================================================================
 * REGISTROVANÝ MSG CALLBACK (UI strana, listener)
 *
 * Může být zaregistrován pouze jeden callback. Přístup chráněn tím,
 * že registrace/odregistrace probíhá pouze z UI vlákna a callback
 * se volá také pouze z UI vlákna (po thread switchi z dispatcher).
 * ============================================================================ */

static dbgapi_msg_callback_t s_msg_callback = NULL;
static void *s_msg_callback_user_data = NULL;


/* ============================================================================
 * REGISTROVANÝ MSG DISPATCHER (UI strana, thread switch)
 *
 * Dispatcher je registrován UI vrstvou při startup. EMU strana volá
 * dispatcher z dbgapi_emu_send_msg() pro doručení MSG do UI vlákna.
 * Implementace dispatcheru řeší thread switch (typicky SDL custom event)
 * a poté volá dbgapi_ui_invoke_msg_callback() v UI vlákně.
 *
 * Tímto způsobem dbgapi.c nezná SDL/sdlapp - thread switch je čistě
 * v UI vrstvě (src/ui-imgui/debugger/dbgapi_dispatcher.{cpp,h}).
 * ============================================================================ */

static dbgapi_msg_dispatcher_t s_msg_dispatcher = NULL;
static void *s_msg_dispatcher_user_data = NULL;


/* ============================================================================
 * LAST USER ACTION TRACKER (V1.D.1)
 *
 * Eviduje poslední CMDRQ s cmd_origin == DBGAPI_CMD_ORIGIN_USER, který
 * není čtením (dbgapi_cmd_is_read(): druh DBGAPI_CMD_KIND_READ v
 * dbgapi_cmd_props.c a čtecí operace příkazů s operací v parametru):
 * periodická čtení otevřených oken debuggeru (registry, paměť,
 * disassembler) by jinak záznam přepisovala pořád. Slouží AI klientovi
 * (přes emulator://state Resource) ke zjištění, co naposledy user v GUI
 * udělal - umožní AI lépe spolupracovat s uživatelem (= "user právě klikl
 * Run, počkám než se zastaví").
 *
 * Akce GUI mimo frontu (reset z GUI, načtení snímku, zásahy menu a oken
 * zařízení) zaznamená dbgapi_note_user_action() ze svého vstupního bodu.
 *
 * Thread-safety: nastavuje se z UI vlákna v dbgapi_ui_submit_cmd_sync_with_origin
 * po vložení slotu a v dbgapi_note_user_action() (= mimo emu thread). Čte se z emu vlákna v handleru
 * get_state. Pro V1.D.1 použijeme jednoduchý GLib mutex - příště lze
 * převést na atomic load/store struct (= per-field race-free, ale není
 * to kritické, last action je informativní).
 * ============================================================================ */

/** @brief Poslední akce uživatele (pod s_last_user_action_mutex). */
typedef struct st_DBGAPI_LAST_USER_ACTION
{
    bool          valid;        /**< false = ještě žádná USER akce nebyla. */
    en_DBGAPI_CMD cmd;          /**< Poslední USER CMD; DBGAPI_CMD_NONE u akce mimo frontu (dbgapi_note_user_action()). */
    char          kind[ DBGAPI_USER_ACTION_KIND_MAX ]; /**< Jméno akce: dbgapi_cmd_to_str(cmd) nebo jméno z dbgapi_note_user_action(). */
    uint64_t      timestamp_us; /**< g_get_monotonic_time() v okamžiku submit. */
} st_DBGAPI_LAST_USER_ACTION;

static st_DBGAPI_LAST_USER_ACTION s_last_user_action = {
    .valid = false,
    .cmd = DBGAPI_CMD_NONE,
    .kind = "",
    .timestamp_us = 0,
};
static GMutex s_last_user_action_mutex;
static bool   s_last_user_action_mutex_inited = false;


/**
 * @brief Inicializuje mutex pro last_user_action tracker (volat z dbgapi_init).
 */
static void dbgapi_last_user_action_init_mutex ( void )
{
    if ( !s_last_user_action_mutex_inited )
    {
        g_mutex_init ( &s_last_user_action_mutex );
        s_last_user_action_mutex_inited = true;
    };
}


/**
 * @brief Zaznamená nové USER CMD do trackeru.
 *
 * Volá se z dbgapi_ui_submit_cmd_sync_with_origin pro origin == USER
 * a příkaz jiného druhu než čtení.
 *
 * @param cmd Zaznamenávaný DBGAPI_CMD.
 */
static void dbgapi_track_last_user_action ( en_DBGAPI_CMD cmd )
{
    if ( !s_last_user_action_mutex_inited )
    {
        /* Defense in depth - kdyby někdo zavolal submit dřív než init.
         * V praxi by k tomu nemělo dojít, ale chceme failsafe. */
        return;
    };
    g_mutex_lock ( &s_last_user_action_mutex );
    s_last_user_action.valid        = true;
    s_last_user_action.cmd          = cmd;
    g_strlcpy ( s_last_user_action.kind, dbgapi_cmd_to_str ( cmd ),
                sizeof ( s_last_user_action.kind ) );
    s_last_user_action.timestamp_us = (uint64_t) g_get_monotonic_time ( );
    g_mutex_unlock ( &s_last_user_action_mutex );
}


void dbgapi_note_user_action ( const char *kind )
{
    if ( !s_last_user_action_mutex_inited || kind == NULL || kind[ 0 ] == '\0' )
    {
        return;
    };
    g_mutex_lock ( &s_last_user_action_mutex );
    s_last_user_action.valid        = true;
    s_last_user_action.cmd          = DBGAPI_CMD_NONE;
    g_strlcpy ( s_last_user_action.kind, kind, sizeof ( s_last_user_action.kind ) );
    s_last_user_action.timestamp_us = (uint64_t) g_get_monotonic_time ( );
    g_mutex_unlock ( &s_last_user_action_mutex );
}


bool dbgapi_get_last_user_action_kind ( char *out_kind, size_t out_size,
                                         uint64_t *out_timestamp_us )
{
    if ( out_kind && out_size ) out_kind[ 0 ] = '\0';
    if ( out_timestamp_us ) *out_timestamp_us = 0;
    if ( !s_last_user_action_mutex_inited ) return false;
    g_mutex_lock ( &s_last_user_action_mutex );
    bool valid = s_last_user_action.valid;
    if ( valid )
    {
        if ( out_kind && out_size ) g_strlcpy ( out_kind, s_last_user_action.kind, out_size );
        if ( out_timestamp_us ) *out_timestamp_us = s_last_user_action.timestamp_us;
    };
    g_mutex_unlock ( &s_last_user_action_mutex );
    return valid;
}


/**
 * @brief Vrátí kopii posledního USER action záznamu (= safe pro read z emu).
 *
 * @param[out] out_cmd          Poslední CMD (validní jen pokud return true).
 * @param[out] out_timestamp_us Timestamp v mikrosekundách (od g_get_monotonic_time).
 * @return true pokud byla zaznamenána aspoň jedna USER akce.
 */
bool dbgapi_get_last_user_action ( en_DBGAPI_CMD *out_cmd,
                                    uint64_t *out_timestamp_us )
{
    if ( !s_last_user_action_mutex_inited )
    {
        if ( out_cmd ) *out_cmd = DBGAPI_CMD_NONE;
        if ( out_timestamp_us ) *out_timestamp_us = 0;
        return false;
    };
    g_mutex_lock ( &s_last_user_action_mutex );
    bool valid = s_last_user_action.valid;
    if ( out_cmd ) *out_cmd = s_last_user_action.cmd;
    if ( out_timestamp_us )
        *out_timestamp_us = s_last_user_action.timestamp_us;
    g_mutex_unlock ( &s_last_user_action_mutex );
    return valid;
}


/* ============================================================================
 * INICIALIZACE A DESTRUKCE
 * ============================================================================ */

void dbgapi_init(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    /* Vynulovat celou frontu */
    memset(queue, 0, sizeof(st_DBGAPI_CMDRQ_QUEUE));

    /* Alokace queue mutex a condition */
    APP_MUTEX_CREATE(queue->queue_mutex);
    APP_COND_CREATE(queue->queue_cond);

    /* Alokace per-slot mutex a condition pro každý slot */
    for (int i = 0; i < DBGAPI_CMDRQ_QUEUE_SIZE; i++)
    {
        st_DBGAPI_CMDRQ *slot = &queue->cmdrq[i];
        slot->cmd_state = DBGAPI_CMDSTATE_NONE;
        slot->cmd = DBGAPI_CMD_NONE;
        slot->data_ptr = NULL;
        slot->result_ptr = NULL;
        slot->success = false;
        slot->dequeued = false;
        slot->done_early = false;
        APP_MUTEX_CREATE(slot->mutex);
        APP_COND_CREATE(slot->cond);
    };

    queue->head = 0;
    queue->tail = 0;
    queue->reply_state = DBGAPI_CMDREPLY_STATE_NONE;

    /* Reset MSG callbacku a dispatcheru */
    s_msg_callback = NULL;
    s_msg_callback_user_data = NULL;
    s_msg_dispatcher = NULL;
    s_msg_dispatcher_user_data = NULL;

    /* V1.D.1 - inicializace last_user_action tracker mutex. */
    dbgapi_last_user_action_init_mutex ( );
}

void dbgapi_destroy(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    /* Uvolnění per-slot mutex a condition */
    for (int i = 0; i < DBGAPI_CMDRQ_QUEUE_SIZE; i++)
    {
        st_DBGAPI_CMDRQ *slot = &queue->cmdrq[i];
        APP_COND_DESTROY(slot->cond);
        APP_MUTEX_DESTROY(slot->mutex);
    };

    /* Uvolnění queue mutex a condition */
    APP_COND_DESTROY(queue->queue_cond);
    APP_MUTEX_DESTROY(queue->queue_mutex);

    /* Reset MSG callbacku a dispatcheru */
    s_msg_callback = NULL;
    s_msg_callback_user_data = NULL;
    s_msg_dispatcher = NULL;
    s_msg_dispatcher_user_data = NULL;
}


/* ============================================================================
 * INTERNÍ POMOCNÉ FUNKCE
 * ============================================================================ */

/**
 * @brief Step Over - jeden krok přes CALL/RST/DJNZ/blokovou instrukci.
 *
 * Analyzuje opcode na aktuálním PC. Pro CALL nn / CALL cc,nn / RST /
 * DJNZ / ED-prefixed blokové instrukce (LDIR/LDDR/CPIR/CPDR/INIR/
 * INDR/OTIR/OTDR) nastaví dočasný breakpoint na (PC + délka instrukce)
 * a spustí emulaci přes mzarch_run_to_temporary_breakpoint(). Pro
 * ostatní instrukce degraduje na step into (debugger_step_call(1)).
 *
 * Replikuje logiku z dbg_iconbar.cpp::dbg_do_step_over() (= UI vrstva
 * po dobu hybridního modelu UI direct calls + dbgapi).
 *
 * @pre EMULATOR_TEST_PAUSED (= step over má smysl jen ze paused stavu;
 *      caller volá emulator_pause(true) pokud emu běží).
 */
static void dbgapi_emu_do_step_over ( void )
{
    uint16_t pc = g_mzarch_main.cpu->pc;
    uint8_t opcode = debugger_dasm_read_cb ( pc, NULL );

    bool is_step_over_target = false;
    int instr_len = 1;

    if ( opcode == 0xCD )
    {
        /* CALL nn — nepodmíněný CALL */
        is_step_over_target = true;
        instr_len = 3;
    }
    else if ( ( opcode & 0xC7 ) == 0xC4 )
    {
        /* CALL cc,nn — podmíněný CALL */
        is_step_over_target = true;
        instr_len = 3;
    }
    else if ( ( opcode & 0xC7 ) == 0xC7 )
    {
        /* RST xx */
        is_step_over_target = true;
        instr_len = 1;
    }
    else if ( opcode == 0x10 )
    {
        /* DJNZ e */
        is_step_over_target = true;
        instr_len = 2;
    }
    else if ( opcode == 0xED )
    {
        uint8_t opcode2 = debugger_dasm_read_cb ( (uint16_t) ( pc + 1 ), NULL );
        switch ( opcode2 )
        {
            case 0xB0: /* LDIR */
            case 0xB8: /* LDDR */
            case 0xB1: /* CPIR */
            case 0xB9: /* CPDR */
            case 0xB2: /* INIR */
            case 0xBA: /* INDR */
            case 0xB3: /* OTIR */
            case 0xBB: /* OTDR */
                is_step_over_target = true;
                instr_len = 2;
                break;
            default:
                break;
        };
    };

    if ( is_step_over_target )
    {
        bptmap_set_temporary_event ( (uint16_t) ( pc + instr_len ) );
        debugger_step_call ( 0 );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        mzarch_run_to_temporary_breakpoint ( );
#endif
    }
    else
    {
        debugger_step_call ( 1 );
    };
}


/* ============================================================================
 * EMU STRANA — KONTROLA A ZPRACOVÁNÍ FRONTY
 * ============================================================================ */

/*
 * Interní helper: zjistí, zda je fronta prázdná (bez zamykání).
 * Volat pouze s drženým queue_mutex.
 */
static inline bool dbgapi_emu_has_pending_unlocked(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    return (queue->head != queue->tail);
}

bool dbgapi_emu_has_pending(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    APP_MUTEX_LOCK(queue->queue_mutex);
    bool result = dbgapi_emu_has_pending_unlocked(queue);
    APP_MUTEX_UNLOCK(queue->queue_mutex);
    return result;
}

st_DBGAPI_CMDRQ *dbgapi_emu_dequeue(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    APP_MUTEX_LOCK(queue->queue_mutex);

    while (dbgapi_emu_has_pending_unlocked(queue))
    {
        /* Vyjmout slot z hlavy fronty */
        st_DBGAPI_CMDRQ *slot = &queue->cmdrq[queue->head];
        queue->head = (queue->head + 1) % DBGAPI_CMDRQ_QUEUE_SIZE;

        /* Slot už vykonaný mimo pořadí (dbgapi_emu_dequeue_matching()):
         * jen přeskočit. Stav slotu (PROCESSED / NONE) patří odesílateli. */
        if (slot->done_early)
        {
            slot->done_early = false;
            continue;
        };

        /* Slot zrušený odesílatelem po timeoutu: přeskočit bez provedení.
         * CANCELLED se nastavuje jen pod queue_mutex (který držíme), takže
         * čtení je konzistentní. Odesílatel už na slot nečeká ani na něj
         * nesahá, uvolníme ho tady. */
        if (slot->cmd_state == DBGAPI_CMDSTATE_CANCELLED)
        {
            slot->cmd_state = DBGAPI_CMDSTATE_NONE;
            continue;
        };

        /* Od teď ho odesílatel nesmí zrušit - čeká na dokončení. */
        slot->dequeued = true;
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        return slot;
    };

    APP_MUTEX_UNLOCK(queue->queue_mutex);
    return NULL;
}

st_DBGAPI_CMDRQ *dbgapi_emu_dequeue_matching(st_DBGAPI_CMDRQ_QUEUE *queue,
                                             bool (*match)(en_DBGAPI_CMD cmd))
{
    if (!match)
        return NULL;

    APP_MUTEX_LOCK(queue->queue_mutex);

    for (int i = queue->head; i != queue->tail; i = (i + 1) % DBGAPI_CMDRQ_QUEUE_SIZE)
    {
        st_DBGAPI_CMDRQ *slot = &queue->cmdrq[i];

        /* Už vykonaný mimo pořadí nebo zrušený odesílatelem (nevyzvednutý
         * slot je jen PENDING nebo CANCELLED): nechat na dbgapi_emu_dequeue(). */
        if (slot->done_early || slot->cmd_state != DBGAPI_CMDSTATE_PENDING)
            continue;
        if (!match((en_DBGAPI_CMD)(slot->cmd & DBGAPI_CMD_MASK)))
            continue;

        /* Od teď ho odesílatel nesmí zrušit - čeká na dokončení. Slot
         * v hlavě fronty se vyjme normálně, jinde ho head později přeskočí. */
        slot->dequeued = true;
        if (i == queue->head)
            queue->head = (queue->head + 1) % DBGAPI_CMDRQ_QUEUE_SIZE;
        else
            slot->done_early = true;
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        return slot;
    };

    APP_MUTEX_UNLOCK(queue->queue_mutex);
    return NULL;
}

/**
 * @brief Read callback pro z80_dasm() při dekódování Last instrukce.
 *
 * Čte bajty z lokálně uloženého `st_DEBUGGER_HISTORY_ROW.byte[]`, ne
 * z živé paměti (=  byte[] obsahují bajty platné v okamžiku M1 startu,
 * což je presné a nezávislé na pozdějších banking změnách).
 *
 * Mapování: offset = addr - row->addr. Pokud offset >= 4 (= za hranou
 * uloženého bufferu), vrací 0x00 jako sentinel - z80_dasm by neměl číst
 * víc než length-1 bajtů (= max 3 pro 4-byte instrukci).
 *
 * @param addr      Adresa, kterou disassembler žádá.
 * @param user_data Pointer na st_DEBUGGER_HISTORY_ROW.
 * @return Bajt z row->byte[offset] nebo 0 pri OOB.
 */
static uint8_t dbgapi_last_instr_read_cb(uint16_t addr, void *user_data)
{
    st_DEBUGGER_HISTORY_ROW *row = (st_DEBUGGER_HISTORY_ROW *)user_data;
    int off = (int)(addr - row->addr);
    if (off < 0 || off >= 4) return 0x00;
    return row->byte[off];
}


/**
 * @brief Pomocný handler pro CMD_BP_UPDATE a CMD_BP_CREATE_WITH_INIT.
 *
 * Aplikuje selektivní update polí BP podle p->update_mask voláním
 * existujících breakpoints_set_*() setterů.
 *
 * Pokud allow_create == true (= CMD_BP_CREATE_WITH_INIT):
 *  - p->id musí být -1 na vstupu (jiná hodnota = success false)
 *  - Volá breakpoints_add_auto(p->addr, p->name pokud UM_NAME, p->parent
 *    pokud UM_PARENT, jinak -1). Po úspěchu naplní p->id přiděleným ID.
 *  - Pokud add selže, vrátí false a p->id zůstane -1 (rollback není
 *    potřeba - nic se nevytvořilo).
 *  - Po úspěšném add aplikuje zbytek update_mask přes setter cestu níže.
 *
 * Pokud allow_create == false (= CMD_BP_UPDATE):
 *  - p->id musí ukazovat na existující BP. Pokud
 *    breakpoints_find_by_id(p->id) == NULL, vrátí false a žádnou změnu
 *    neaplikuje (= pre-check před iterací maskou).
 *
 * Jméno: `UM_NAME` bez `UM_AUTO_NAME` znamená vlastní jméno a vypne
 * automatické jméno (`auto_name = false`), jinak by ho setter adresy
 * (breakpoints_set_addr()) při zapnutém `auto_name` přepsal na
 * `Addr: 0x....`. Kdo pošle obě pole (GUI, automatizace), řídí
 * `auto_name` sám.
 *
 * Apply best-effort: pokud některý setter vrátí false (= např. nečekané),
 * helper pokračuje v zápisu ostatních polí a vrátí AND všech výsledků
 * (= konzistentní s dnešním working_copy_apply, který výsledky setterů
 * neřeší). Pre-check existence BP zachycuje hlavní failure case.
 *
 * @param p          Vstupní payload (id, update_mask, fieldy).
 * @param allow_create true = CMD_BP_CREATE_WITH_INIT (= naplní id),
 *                     false = CMD_BP_UPDATE (= read-only id).
 * @return true = pre-check OK + všechny aktivní settery vrátily true.
 */
static bool dbgapi_emu_bp_apply_update ( st_DBGAPI_BP_UPDATE_PARAM *p, bool allow_create )
{
    if ( !p ) return false;

    int target_id;

    if ( allow_create )
    {
        if ( p->id != -1 ) return false;
        uint16_t init_addr = ( p->update_mask & DBGAPI_BP_UM_ADDR ) ? p->addr : 0;
        const char *init_name =
            ( p->update_mask & DBGAPI_BP_UM_NAME ) ? p->name : NULL;
        int init_parent =
            ( p->update_mask & DBGAPI_BP_UM_PARENT ) ? p->parent : -1;
        int new_id = breakpoints_add_auto ( init_addr, init_name, init_parent );
        if ( new_id < 0 ) return false;
        p->id = new_id;
        target_id = new_id;
        /* Smazat UM_ADDR/_NAME/_PARENT z masky - add_auto je už nastavil.
         * Setter aplikace níže by je přepsala identickou hodnotou (idempotent),
         * takže maska se neořezává - ponecháme. */
    }
    else
    {
        if ( !breakpoints_find_by_id ( p->id ) ) return false;
        target_id = p->id;
    };

    uint64_t mask = p->update_mask;
    bool ok = true;

    /* === Identifikace === */
    if ( mask & DBGAPI_BP_UM_ENABLED )
        ok &= breakpoints_set_enabled ( target_id, p->enabled );
    if ( mask & DBGAPI_BP_UM_AUTO_NAME )
        ok &= breakpoints_set_auto_name ( target_id, p->auto_name );
    else if ( mask & DBGAPI_BP_UM_NAME )
        /* Vlastní jméno bez auto_name: vypnout automatické jméno, aby ho
         * setter adresy níže nepřepsal (MCP bp_create_with_init / bp_update). */
        ok &= breakpoints_set_auto_name ( target_id, false );
    if ( mask & DBGAPI_BP_UM_NAME )
        ok &= breakpoints_set_name ( target_id, p->name );
    if ( mask & DBGAPI_BP_UM_COLORS )
        ok &= breakpoints_set_colors ( target_id, p->bg_rgb, p->fg_rgb );
    if ( mask & DBGAPI_BP_UM_PARENT )
        ok &= breakpoints_set_parent ( target_id, p->parent );

    /* === Smart core === */
    if ( mask & DBGAPI_BP_UM_TYPE )
        ok &= breakpoints_set_type ( target_id, (en_BPT_TYPE)p->type );
    if ( mask & DBGAPI_BP_UM_ADDR )
        ok &= breakpoints_set_addr ( target_id, p->addr );
    if ( mask & DBGAPI_BP_UM_ADDR_END )
        ok &= breakpoints_set_addr_end ( target_id, p->addr_end );
    if ( mask & DBGAPI_BP_UM_ZONE )
        ok &= breakpoints_set_zone ( target_id, (en_BP_ZONE)p->zone );
    if ( mask & DBGAPI_BP_UM_BANK_ID )
        ok &= breakpoints_set_bank_id ( target_id, p->bank_id );
    if ( mask & DBGAPI_BP_UM_PORT )
        ok &= breakpoints_set_port ( target_id, p->port );
    if ( mask & DBGAPI_BP_UM_EVENT_NAME )
        ok &= breakpoints_set_event_name ( target_id, p->event_name );
    if ( mask & DBGAPI_BP_UM_EVENT_TRIGGER )
        ok &= breakpoints_set_event_trigger ( target_id, (en_BP_EVENT_TRIGGER)p->event_trigger );
    if ( mask & DBGAPI_BP_UM_SP_THRESHOLD )
        ok &= breakpoints_set_sp_threshold ( target_id, p->sp_threshold );
    if ( mask & DBGAPI_BP_UM_EXPR )
        ok &= breakpoints_set_expr ( target_id, p->expr );
    if ( mask & DBGAPI_BP_UM_ACTION )
        ok &= breakpoints_set_action ( target_id, p->action );
    if ( mask & DBGAPI_BP_UM_HIT_COUNT )
        ok &= breakpoints_set_hit_count ( target_id, p->hit_count );
    if ( mask & DBGAPI_BP_UM_SKIP_COUNT )
        ok &= breakpoints_set_skip_count ( target_id, p->skip_count );
    if ( mask & DBGAPI_BP_UM_EDGE_TRIGGERED )
        ok &= breakpoints_set_edge_triggered ( target_id, p->edge_triggered );

    /* === Match modes === */
    if ( mask & DBGAPI_BP_UM_ADDR_MATCH_MODE )
        ok &= breakpoints_set_addr_match_mode ( target_id, (en_BP_MATCH_MODE)p->addr_match_mode );
    if ( mask & DBGAPI_BP_UM_ADDR_MASK )
        ok &= breakpoints_set_addr_mask ( target_id, p->addr_mask );
    if ( mask & DBGAPI_BP_UM_PORT_MATCH_MODE )
        ok &= breakpoints_set_port_match_mode ( target_id, (en_BP_MATCH_MODE)p->port_match_mode );
    if ( mask & DBGAPI_BP_UM_PORT_END )
        ok &= breakpoints_set_port_end ( target_id, p->port_end );
    if ( mask & DBGAPI_BP_UM_PORT_MASK )
        ok &= breakpoints_set_port_mask ( target_id, p->port_mask );
    if ( mask & DBGAPI_BP_UM_PORT_MODE )
        ok &= breakpoints_set_port_mode ( target_id, (en_BP_PORT_MODE)p->port_mode );
    if ( mask & DBGAPI_BP_UM_BANK_MATCH_MODE )
        ok &= breakpoints_set_bank_match_mode ( target_id, (en_BP_MATCH_MODE)p->bank_match_mode );
    if ( mask & DBGAPI_BP_UM_BANK_ID_END )
        ok &= breakpoints_set_bank_id_end ( target_id, p->bank_id_end );
    if ( mask & DBGAPI_BP_UM_BANK_ID_MASK )
        ok &= breakpoints_set_bank_id_mask ( target_id, p->bank_id_mask );
    if ( mask & DBGAPI_BP_UM_ADDR_SPACE )
        ok &= breakpoints_set_bp_addr_space ( target_id, (en_BP_ADDR_SPACE)p->bp_addr_space );
    if ( mask & DBGAPI_BP_UM_SP_MODE )
        ok &= breakpoints_set_sp_mode ( target_id, (en_BP_SP_MODE)p->sp_mode );
    if ( mask & DBGAPI_BP_UM_SP_UPPER )
        ok &= breakpoints_set_sp_upper ( target_id, p->sp_upper );

    /* === IRQ A8 === */
    if ( mask & DBGAPI_BP_UM_IM2_VECTOR_FILTER )
        ok &= breakpoints_set_im2_vector_filter ( target_id, p->im2_vector_enabled, p->im2_vector_addr );
    if ( mask & DBGAPI_BP_UM_IM2_VECTOR_MATCH_MODE )
        ok &= breakpoints_set_im2_vector_match_mode ( target_id, (en_BP_MATCH_MODE)p->im2_vector_match_mode );
    if ( mask & DBGAPI_BP_UM_IM2_VECTOR_ADDR_END )
        ok &= breakpoints_set_im2_vector_addr_end ( target_id, p->im2_vector_addr_end );
    if ( mask & DBGAPI_BP_UM_IM2_VECTOR_MASK )
        ok &= breakpoints_set_im2_vector_mask ( target_id, p->im2_vector_mask );
    if ( mask & DBGAPI_BP_UM_IM2_ISR_FILTER )
        ok &= breakpoints_set_im2_isr_filter ( target_id, p->im2_isr_enabled, p->im2_isr_addr );
    if ( mask & DBGAPI_BP_UM_IM2_ISR_MATCH_MODE )
        ok &= breakpoints_set_im2_isr_match_mode ( target_id, (en_BP_MATCH_MODE)p->im2_isr_match_mode );
    if ( mask & DBGAPI_BP_UM_IM2_ISR_ADDR_END )
        ok &= breakpoints_set_im2_isr_addr_end ( target_id, p->im2_isr_addr_end );
    if ( mask & DBGAPI_BP_UM_IM2_ISR_MASK )
        ok &= breakpoints_set_im2_isr_mask ( target_id, p->im2_isr_mask );

    /* === IRQ A8.5 === */
    if ( mask & DBGAPI_BP_UM_IM0_ENABLED )
        ok &= breakpoints_set_im_enabled ( target_id, 0, p->im0_enabled );
    if ( mask & DBGAPI_BP_UM_IM1_ENABLED )
        ok &= breakpoints_set_im_enabled ( target_id, 1, p->im1_enabled );
    if ( mask & DBGAPI_BP_UM_IM2_ENABLED )
        ok &= breakpoints_set_im_enabled ( target_id, 2, p->im2_enabled );
    if ( mask & DBGAPI_BP_UM_IM0_RST_MASK )
        ok &= breakpoints_set_im0_rst_mask ( target_id, p->im0_rst_mask );

    /* === IRQ_SIG === */
    if ( mask & DBGAPI_BP_UM_IRQ_SIG_SOURCE_MASK )
        ok &= breakpoints_set_irq_sig_source_mask ( target_id, p->irq_sig_source_mask );

    /* === 0019 vrstva 2 - per-BP rate-limit override === */
    if ( mask & DBGAPI_BP_UM_FWD_MIN_INTERVAL_MS )
        ok &= breakpoints_set_fwd_min_interval_ms ( target_id, p->fwd_min_interval_ms );
    if ( mask & DBGAPI_BP_UM_FWD_MAX_FIRES )
        ok &= breakpoints_set_fwd_max_fires ( target_id, p->fwd_max_fires );

    return ok;
}


/* ============================================================================
 * CMD -> STRING HELPER (pro MCP_ACTION description, logy, debug)
 *
 * Převede en_DBGAPI_CMD na lidsky čitelný název ve formátu short token
 * (= bez prefixu DBGAPI_CMD_, lowercase). Použit pro
 * st_DBGAPI_MSG_MCP_ACTION_DATA.description default text - GUI Activity
 * Log pak smí augmentovat o detaily (adresy, hodnoty) podle msg.cmd.
 * ============================================================================ */

/**
 * @brief Převede CMDRQ příkaz na lidsky čitelný název.
 *
 * Tabulka enum -> string s short formátem (bez prefixu DBGAPI_CMD_,
 * lowercase). Vrací statický řetězec - volající ho NESMÍ uvolnit, jen
 * okamžitě použít nebo zkopírovat. Pro neznámý/budoucí příkaz vrací
 * "unknown".
 *
 * @param cmd  Příkaz bez BLOCKING flagu (= caller maskuje DBGAPI_CMD_MASK).
 * @return Statický řetězec, nikdy NULL.
 */
const char *dbgapi_cmd_to_str(en_DBGAPI_CMD cmd)
{
    switch (cmd)
    {
        case DBGAPI_CMD_NONE:                     return "none";
        case DBGAPI_CMD_IS_DEBUGGER_ACTIVE:        return "is_debugger_active";
        case DBGAPI_CMD_DEBUGGER_ACTIVATE:         return "debugger_activate";
        case DBGAPI_CMD_DEBUGGER_DEACTIVATE:       return "debugger_deactivate";
        case DBGAPI_CMD_PAUSE:                     return "pause";
        case DBGAPI_CMD_FORCE_PAUSE:               return "force_pause";
        case DBGAPI_CMD_RUN:                       return "run";
        case DBGAPI_CMD_IS_RUNNING:                return "is_running";
        case DBGAPI_CMD_STEP_INTO:                 return "step_into";
        case DBGAPI_CMD_STEP_OVER:                 return "step_over";
        case DBGAPI_CMD_RUN_TO:                    return "run_to";
        case DBGAPI_CMD_RUN_DEADLINE:              return "run_deadline";
        case DBGAPI_CMD_FRAME_HASH:                return "frame_hash";
        case DBGAPI_CMD_BP_TEST_EVAL:              return "bp_test_eval";
        case DBGAPI_CMD_STATE_OBSERVE:             return "state_observe";
        case DBGAPI_CMD_RESET:                     return "reset";
        case DBGAPI_CMD_GET_REG:                   return "get_reg";
        case DBGAPI_CMD_SET_REG:                   return "set_reg";
        case DBGAPI_CMD_GET_ALL_REGS:              return "get_all_regs";
        case DBGAPI_CMD_MEM_READ:                  return "mem_read";
        case DBGAPI_CMD_MEM_WRITE:                 return "mem_write";
        case DBGAPI_CMD_BP_ADD:                    return "bp_add";
        case DBGAPI_CMD_BP_REMOVE:                 return "bp_remove";
        case DBGAPI_CMD_BP_LIST:                   return "bp_list";
        case DBGAPI_CMD_BP_UPDATE:                 return "bp_update";
        case DBGAPI_CMD_BP_SET_ENABLED:            return "bp_set_enabled";
        case DBGAPI_CMD_BP_SET_PARENT:             return "bp_set_parent";
        case DBGAPI_CMD_BP_CREATE_WITH_INIT:       return "bp_create_with_init";
        case DBGAPI_CMD_BPGRP_ADD:                 return "bpgrp_add";
        case DBGAPI_CMD_BPGRP_REMOVE:              return "bpgrp_remove";
        case DBGAPI_CMD_BPGRP_UPDATE:              return "bpgrp_update";
        case DBGAPI_CMD_DASM:                      return "dasm";
        case DBGAPI_CMD_HISTORY_GET:               return "history_get";
        case DBGAPI_CMD_GET_CPU_FLAGS:             return "get_cpu_flags";
        case DBGAPI_CMD_SET_CPU_FLAGS:             return "set_cpu_flags";
        case DBGAPI_CMD_GET_IM2_VECTOR:            return "get_im2_vector";
        case DBGAPI_CMD_GET_RASTER_POS:            return "get_raster_pos";
        case DBGAPI_CMD_GET_LAST_INSTR:            return "get_last_instr";
        case DBGAPI_CMD_GET_CPU_PANEL_BATCH:       return "get_cpu_panel_batch";
        case DBGAPI_CMD_SET_USER_CYCLE_ORIGIN:     return "set_user_cycle_origin";
        case DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR: return "set_pioz80_interrupt_vector";
        case DBGAPI_CMD_MEM_WRITE_CHECKED:         return "mem_write_checked";
        case DBGAPI_CMD_STACK_DUMP:                return "stack_dump";
        case DBGAPI_CMD_STACK_REGIONS_LIST:        return "stack_regions_list";
        case DBGAPI_CMD_STACK_REGIONS_ADD:         return "stack_regions_add";
        case DBGAPI_CMD_STACK_REGIONS_REMOVE:      return "stack_regions_remove";
        case DBGAPI_CMD_STACK_REGIONS_RESET_WATERMARK: return "stack_regions_reset_watermark";
        case DBGAPI_CMD_STACK_REGIONS_EDIT:        return "stack_regions_edit";
        case DBGAPI_CMD_STACK_HISTORY_ENABLE:      return "stack_history_enable";
        case DBGAPI_CMD_STACK_HISTORY_GET:         return "stack_history_get";
        case DBGAPI_CMD_STACK_HISTORY_RESET:       return "stack_history_reset";
        case DBGAPI_CMD_EVENTLOG_START:            return "eventlog_start";
        case DBGAPI_CMD_EVENTLOG_STOP:             return "eventlog_stop";
        case DBGAPI_CMD_EVENTLOG_CLEAR:            return "eventlog_clear";
        case DBGAPI_CMD_EVENTLOG_SET_CAPACITY:     return "eventlog_set_capacity";
        case DBGAPI_CMD_EVENTLOG_SET_MASK:         return "eventlog_set_mask";
        case DBGAPI_CMD_EVENTLOG_GET_EVENT:        return "eventlog_get_event";
        case DBGAPI_CMD_GET_CALLSTACK:             return "get_callstack";
        case DBGAPI_CMD_GET_PROFILER:              return "get_profiler";
        case DBGAPI_CMD_PROFILER_SET_ACTIVE:       return "profiler_set_active";
        case DBGAPI_CMD_PROFILER_RESET:            return "profiler_reset";
        case DBGAPI_CMD_PROFILER_EXPORT:           return "profiler_export";
        case DBGAPI_CMD_SNAPSHOT_SAVE_FILE:        return "snapshot_save_file";
        case DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER:      return "snapshot_save_buffer";
        case DBGAPI_CMD_SNAPSHOT_LOAD_FILE:        return "snapshot_load_file";
        case DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER:      return "snapshot_load_buffer";
        case DBGAPI_CMD_SYMBOL_ADD:                return "symbol_add";
        case DBGAPI_CMD_SYMBOL_REMOVE:             return "symbol_remove";
        case DBGAPI_CMD_SYMBOL_LOOKUP:             return "symbol_lookup";
        case DBGAPI_CMD_SYMBOL_LIST:               return "symbol_list";
        case DBGAPI_CMD_STEP_OUT:                  return "step_out";
        case DBGAPI_CMD_IO_READ:                   return "io_read";
        case DBGAPI_CMD_IO_WRITE:                  return "io_write";
        case DBGAPI_CMD_IRQ_INJECT:                return "irq_inject";
        case DBGAPI_CMD_NMI_INJECT:                return "nmi_inject";
        case DBGAPI_CMD_MEM_WRITE_FORCE:           return "mem_write_force";
        /* V1.A.6 - Watch + CDL Tools */
        case DBGAPI_CMD_WATCH_ADD:                 return "watch_add";
        case DBGAPI_CMD_WATCH_REMOVE:              return "watch_remove";
        case DBGAPI_CMD_WATCH_LIST:                return "watch_list";
        case DBGAPI_CMD_WATCH_EVAL:                return "watch_eval";
        case DBGAPI_CMD_CDL_START:                 return "cdl_start";
        case DBGAPI_CMD_CDL_STOP:                  return "cdl_stop";
        case DBGAPI_CMD_CDL_RESET:                 return "cdl_reset";
        case DBGAPI_CMD_CDL_EXPORT:                return "cdl_export";
        /* 0017 FÁZE 1 - Tracking lifecycle (trace-suite) */
        case DBGAPI_CMD_TRACE_START:               return "trace_start";
        case DBGAPI_CMD_TRACE_STOP:                return "trace_stop";
        case DBGAPI_CMD_TRACE_RESET:               return "trace_reset";
        case DBGAPI_CMD_TRACE_SAVE:                return "trace_save";
        case DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE:  return "debugger_state_recompute";
        /* video-capture Task 15 - video záznam */
        case DBGAPI_CMD_VIDEOREC:                  return "videorec";
        /* ui-thread-writes T1 - historie a aktivita I/O */
        case DBGAPI_CMD_IO_HISTORY_SET_CAPACITY:   return "io_history_set_capacity";
        case DBGAPI_CMD_IO_HISTORY_CLEAR:          return "io_history_clear";
        case DBGAPI_CMD_IO_ACTIVITY_RESET:         return "io_activity_reset";
        /* ui-thread-writes T2 - hromadné operace s breakpointy */
        case DBGAPI_CMD_BP_CLEAR_ALL:              return "bp_clear_all";
        case DBGAPI_CMD_BP_LOAD_FILE:              return "bp_load_file";
        /* ui-thread-writes T3 - Memory Heatmap */
        case DBGAPI_CMD_MHMAP_RESET_REGION:        return "mhmap_reset_region";
        case DBGAPI_CMD_MHMAP_MERGE:               return "mhmap_merge";
        /* ui-thread-writes T4 - Memory Map */
        case DBGAPI_CMD_MEMMAP_SET:                return "memmap_set";
        /* ui-thread-writes T5a - Event Viewer */
        case DBGAPI_CMD_EVENTLOG_SET_MODE:         return "eventlog_set_mode";
        case DBGAPI_CMD_EVENTLOG_IMPORT_FILE:      return "eventlog_import_file";
        /* ui-thread-writes T6a - Freeze Bytes */
        case DBGAPI_CMD_FREEZE_ADD:                return "freeze_add";
        case DBGAPI_CMD_FREEZE_REMOVE:             return "freeze_remove";
        /* ui-thread-writes T6b - reset počítadla zásahů BP */
        case DBGAPI_CMD_BP_RESET_HITS:             return "bp_reset_hits";
        /* ui-thread-writes T6c - Callstack */
        case DBGAPI_CMD_CALLSTACK_SET_ACTIVE:      return "callstack_set_active";
        case DBGAPI_CMD_CALLSTACK_RESET:           return "callstack_reset";
        /* ui-thread-writes T6d - vynucený refresh obrazovky */
        case DBGAPI_CMD_SCREEN_REFRESH:            return "screen_refresh";
        /* ui-thread-writes T1b - reset aktivity jednoho portu */
        case DBGAPI_CMD_IO_ACTIVITY_RESET_PORT:    return "io_activity_reset_port";
        /* ui-thread-writes T5c - triggery okna Events */
        case DBGAPI_CMD_EVENTLOG_TRIGGER_SET:      return "eventlog_trigger_set";
        case DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES: return "eventlog_trigger_clear_matches";
        /* V1.B.1 - Media Tools */
        case DBGAPI_CMD_MEDIA_LOAD_MZF:            return "media_load_mzf";
        case DBGAPI_CMD_MEDIA_LOAD_BINARY:         return "media_load_binary";
        case DBGAPI_CMD_MEDIA_RUN_MZF:             return "media_run_mzf";
        case DBGAPI_CMD_MEDIA_INSERT:              return "media_insert";
        case DBGAPI_CMD_MEDIA_EJECT:               return "media_eject";
        case DBGAPI_CMD_MEDIA_STATE:               return "media_state";
        /* V1.B.2 - Platform + Config Tools */
        case DBGAPI_CMD_SETTINGS_GET:              return "settings_get";
        case DBGAPI_CMD_SETTINGS_SET:              return "settings_set";
        case DBGAPI_CMD_PLATFORM_SET:              return "platform_set";
        case DBGAPI_CMD_PERIPH_ATTACH:             return "periph_attach";
        case DBGAPI_CMD_PERIPH_DETACH:             return "periph_detach";
        /* V1.C.1 - HID Tools */
        case DBGAPI_CMD_INPUT_PRESS_KEY:           return "input_press_key";
        case DBGAPI_CMD_INPUT_RELEASE_KEY:         return "input_release_key";
        case DBGAPI_CMD_INPUT_RELEASE_ALL:         return "input_release_all";
        case DBGAPI_CMD_INPUT_JOY_SET:             return "input_joy_set";
        case DBGAPI_CMD_INPUT_JOY_CLEAR:           return "input_joy_clear";
        case DBGAPI_CMD_HID_SCRIPT_START:          return "hid_script_start";
        case DBGAPI_CMD_HID_SCRIPT_CANCEL:         return "hid_script_cancel";
        case DBGAPI_CMD_SCREENSHOT_SERIES:         return "screenshot_series";
        case DBGAPI_CMD_PROFILER_PC:               return "profiler_pc";
        case DBGAPI_CMD_AUDIO_CAPTURE:             return "audio_capture";
        case DBGAPI_CMD_TRACE_SNAPSHOT:            return "trace_snapshot";
        case DBGAPI_CMD_TRACE_RESOLVE:             return "trace_resolve";
        case DBGAPI_CMD_RAMLOG:                    return "ramlog";
        case DBGAPI_CMD_COVERAGE:                  return "coverage";
        case DBGAPI_CMD_BPLOG:                     return "bplog";
        case DBGAPI_CMD_RUN_UNTIL_TICKS:           return "run_until_ticks";
        case DBGAPI_CMD_MACHINE_HASH:              return "machine_hash";
        case DBGAPI_CMD_DEVBUS_LIST:               return "devbus_list";
        case DBGAPI_CMD_DEVBUS_VOUT_READ:          return "devbus_vout_read";
        case DBGAPI_CMD_MEMSEARCH_SAMPLE:          return "memsearch_sample";
        case DBGAPI_CMD_FREEZE_LIST:               return "freeze_list";
        /* V1.D.1 - Core + CPU extras Resources */
        case DBGAPI_CMD_GET_CPU_IM2_VECTOR:        return "get_cpu_im2_vector";
        case DBGAPI_CMD_GET_CPU_INTERRUPT_BUS:     return "get_cpu_interrupt_bus";
        case DBGAPI_CMD_GET_MEMORY_MAP:            return "get_memory_map";
        case DBGAPI_CMD_GET_MEMEXT_INFO:           return "get_memext_info";
        case DBGAPI_CMD_BP_VARS_LIST:              return "bp_vars_list";
        case DBGAPI_CMD_BOOKMARKS_LIST:            return "bookmarks_list";
        /* V1.D.3.A - IRQ chip Resources */
        case DBGAPI_CMD_GET_PERIPH_I8255:          return "get_periph_i8255";
        case DBGAPI_CMD_GET_PERIPH_I8253:          return "get_periph_i8253";
        case DBGAPI_CMD_GET_PERIPH_Z80_PIO:        return "get_periph_z80_pio";
        case DBGAPI_CMD_GET_PERIPH_SN76489:        return "get_periph_sn76489";
        case DBGAPI_CMD_GET_PERIPH_AY3_8910:       return "get_periph_ay3_8910";
        case DBGAPI_CMD_GET_PERIPH_BEEPER:         return "get_periph_beeper";
        /* V1.D.3.C - storage + display Resources */
        case DBGAPI_CMD_GET_PERIPH_GDG:            return "get_periph_gdg";
        case DBGAPI_CMD_GET_PERIPH_WD1793:         return "get_periph_wd1793";
        case DBGAPI_CMD_GET_PERIPH_CMT:            return "get_periph_cmt";
        case DBGAPI_CMD_GET_PERIPH_QD:             return "get_periph_qd";
        /* V1.D.4 - input + frame Resources */
        case DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE:       return "get_input_keyboard_state";
        case DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO: return "get_input_keyboard_matrix_info";
        case DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE:       return "get_input_joystick_state";
        case DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO:     return "get_frame_framebuffer_info";
        case DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW:       return "get_frame_screenshot_raw";
        case DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG:       return "get_frame_screenshot";
        case DBGAPI_CMD_GET_VIDEO_TEXT_DUMP:            return "get_video_text_dump";
        case DBGAPI_CMD_GET_WATCH_SNAPSHOT:             return "get_watch_snapshot";
        case DBGAPI_CMD_REGIONS_ENUMERATE:              return "regions_enumerate";
        case DBGAPI_CMD_REGIONS_READ:                   return "regions_read";
        case DBGAPI_CMD_REGIONS_WRITE:                  return "regions_write";
        /* BACKLOG D - emulation speed control */
        case DBGAPI_CMD_GET_SPEED:                      return "get_speed";
        case DBGAPI_CMD_SET_SPEED:                      return "set_speed";
        /* BACKLOG B - bookmark write */
        case DBGAPI_CMD_BOOKMARK_ADD:                   return "bookmark_add";
        case DBGAPI_CMD_BOOKMARK_REMOVE:                return "bookmark_remove";
        /* CMT-A - transport + recording + cmthack toggle */
        case DBGAPI_CMD_CMT_TRANSPORT:                  return "cmt_transport";
        case DBGAPI_CMD_CMT_RECORD:                     return "cmt_record";
        case DBGAPI_CMD_CMT_HACK_SET:                   return "cmt_hack_set";
        case DBGAPI_CMD_CMT_SET_PROPERTY:               return "cmt_set_property";
        case DBGAPI_CMD_CMT_OPEN:                       return "cmt_open";
        case DBGAPI_CMD_CMT_TAPE_SEEK:                  return "cmt_tape_seek";
        case DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED:           return "cmt_tape_block_speed";
        case DBGAPI_CMD_CMT_TAPE_LIST:                  return "cmt_tape_list";
        /* mcp-debug-control request 0021 - deterministický frame-bounded run */
        case DBGAPI_CMD_RUN_FRAMES:                     return "run_frames";
    };
    return "unknown";
}


/**
 * @brief Sestaví a vyšle DBGAPI_MSG_MCP_ACTION broadcast.
 *
 * Volat z dbgapi_emu_dispatch() po úspěšném zpracování příkazu pro
 * cmd_origin == DBGAPI_CMD_ORIGIN_MCP. Alokuje st_DBGAPI_MSG_DATA na heapu
 * (g_new0), vyplní cmd / description / timestamp_us a předá vlastnictví
 * dispatcheru přes dbgapi_emu_send_msg(). Pokud žádný dispatcher není
 * zaregistrován, data se uvolní v send_msg.
 *
 * V1.E.6.A rozšíření: pro entity-tvořící příkazy (BP_*, WATCH_ADD,
 * SYMBOL_ADD) emit-site z payloadu (rq->data_ptr) extrahuje cílovou
 * entitu (id nebo addr) a uloží do entity_id + entity_kind, aby
 * Activity okno mohlo přes dvojklik směrovat focus do příslušného
 * editoru. Pro ostatní příkazy zůstává entity_kind = NONE.
 *
 * Důležité: emit-site běží PO úspěšné dispatch handler větvi, takže
 * pro WATCH_ADD je out_index v payload struktuře už platný. Lookup
 * watch_get(out_index)->id pak vrátí stabilní runtime ID watch řádku
 * (= pole st_WATCH_ROW.id, monotonní counter).
 *
 * observe-cas (M10): podmíněný zápis, který emu vlákno odmítlo (stav se
 * od čtení změnil), se vysílá také, s příznakem `rejected`; Activity ho
 * ukáže jako odmítnutý a toast pro něj nevznikne.
 *
 * @param rq        Zpracovaný CMDRQ slot (cmd a cmd_origin už nastaveny).
 * @param rejected  true = podmíněný zápis odmítnutý pro nesplněnou podmínku.
 */
static void dbgapi_emit_mcp_action(st_DBGAPI_CMDRQ *rq, bool rejected)
{
    en_DBGAPI_CMD cmd = (en_DBGAPI_CMD)(rq->cmd & DBGAPI_CMD_MASK);
    st_DBGAPI_MSG_DATA *data = g_new0(st_DBGAPI_MSG_DATA, 1);
    data->msg_type = DBGAPI_MSG_MCP_ACTION;
    data->cmd = cmd;
    data->rejected = rejected ? 1 : 0;
    data->description = g_strdup(dbgapi_cmd_to_str(cmd));
    data->timestamp_us = (uint64_t)g_get_monotonic_time();

    /* V1.E.6.A: extrakce cílové entity z payloadu pro Activity routing.
     * Defaultně NONE; switch níže přepíše pro známé CMD. Pokud rq->data_ptr
     * je NULL (= caller nezvalidoval), entity_kind zůstane NONE a UI to
     * tiše ignoruje. */
    data->entity_kind = DBGAPI_ENTITY_KIND_NONE;
    data->entity_id   = 0;

    if (rq->data_ptr)
    {
        switch (cmd)
        {
            /* BP add/remove = st_DBGAPI_BP_PARAM (addr + id). */
            case DBGAPI_CMD_BP_ADD:
            case DBGAPI_CMD_BP_REMOVE:
            {
                const st_DBGAPI_BP_PARAM *p =
                    (const st_DBGAPI_BP_PARAM *)rq->data_ptr;
                data->entity_kind = DBGAPI_ENTITY_KIND_BP;
                data->entity_id   = (int32_t)p->id;
                break;
            }

            /* BP update/create = st_DBGAPI_BP_UPDATE_PARAM (id + ostatní pole).
             * Pro CREATE_WITH_INIT je vstupní id=-1, handler ho naplní na nový
             * monotonní BP ID (>= 1). Hodnotu čteme po dispatch větvi, takže
             * id už platí (= rq->success == true předpoklad caller flow). */
            case DBGAPI_CMD_BP_UPDATE:
            case DBGAPI_CMD_BP_CREATE_WITH_INIT:
            {
                const st_DBGAPI_BP_UPDATE_PARAM *p =
                    (const st_DBGAPI_BP_UPDATE_PARAM *)rq->data_ptr;
                data->entity_kind = DBGAPI_ENTITY_KIND_BP;
                data->entity_id   = (int32_t)p->id;
                break;
            }

            /* Watch add: payload má out_index po dispatch (= validní řádek).
             * Z indexu získáme st_WATCH_ROW.id (stabilní runtime counter).
             * Pokud out_index < 0 (= add selhal), kind zůstane NONE. */
            case DBGAPI_CMD_WATCH_ADD:
            {
                const st_DBGAPI_WATCH_ADD_PARAM *p =
                    (const st_DBGAPI_WATCH_ADD_PARAM *)rq->data_ptr;
                if (p->out_index >= 0)
                {
                    const st_WATCH_ROW *row =
                        watch_get((size_t)p->out_index);
                    if (row)
                    {
                        data->entity_kind = DBGAPI_ENTITY_KIND_WATCH;
                        data->entity_id   = (int32_t)row->id;
                    }
                }
                break;
            }

            /* Symbol add: payload má addr (uint16_t). Entity_id = adresa
             * cast na int32 (vždy fit). Symbol storage je addr-indexovaný,
             * lookup ve sym_window přes sym_db_lookup_by_addr(). */
            case DBGAPI_CMD_SYMBOL_ADD:
            {
                const st_DBGAPI_SYMBOL_PARAM *p =
                    (const st_DBGAPI_SYMBOL_PARAM *)rq->data_ptr;
                data->entity_kind = DBGAPI_ENTITY_KIND_SYMBOL;
                data->entity_id   = (int32_t)p->addr;
                break;
            }

            default:
                /* Ostatní CMD nemají cílovou entitu (= step, eventlog_*,
                 * media_*, atd.). entity_kind = NONE už nastaveno. */
                break;
        }
    }

    dbgapi_emu_send_msg(DBGAPI_MSG_MCP_ACTION, data);
}


/**
 * @brief Popis jednoho trace kanálu pro DBGAPI_CMD_TRACE_* handlery.
 *
 * Sjednocuje přístup ke 4 trace-suite subsystémům (cputrack/iorqlog/intlog/
 * hwlog) v dbgapi dispatchi - každý má vlastní mode pole, reset a save funkci.
 * Aktivace se neprovádí přímo, ale přes @c mzarch_platform_fn_debugger_state_changed
 * (= nastaví mode + recompute všech kanálů + swap CPU callbacků), analogicky
 * k @c mhmap_set_mode.
 *
 * @invariant @c mode_ptr ukazuje na první (mode) pole configu kanálu po celou
 *            dobu běhu; @c fn_save nesmí být NULL (všechny kanály save podporují).
 */
typedef struct st_DBGAPI_TRACE_CHAN_DESC
{
    int  *mode_ptr;                  /**< Ukazatel na mode kanálu (en_TLOG_MODE). */
    void (*fn_reset) ( void );       /**< Reset stavu segmentu (NULL = jen restart). */
    int  (*fn_save) ( const char * );/**< Uložit/přesměrovat segment na path. */
    const st_TLOG_WRITER *( *fn_writer ) ( void ); /**< Writer kanálu pro čtení proudu. */
} st_DBGAPI_TRACE_CHAN_DESC;

/**
 * @brief Vyřešit deskriptor trace kanálu podle en_DBGAPI_TRACE_CHANNEL.
 *
 * @param channel  Vybraný kanál.
 * @param out       Výstupní deskriptor (vyplněn jen při návratu true).
 * @return true při platném kanálu, false u neznámé hodnoty.
 */
static bool dbgapi_resolve_trace_channel ( en_DBGAPI_TRACE_CHANNEL channel,
                                           st_DBGAPI_TRACE_CHAN_DESC *out )
{
    switch ( channel )
    {
        case DBGAPI_TRACE_CHANNEL_CPUTRACK:
            out->mode_ptr = (int *) &g_cputrack_config.mode;
            out->fn_reset = cputrack_reset_collapse_state;
            out->fn_save  = cputrack_save_segment;
            out->fn_writer = cputrack_get_writer;
            return true;
        case DBGAPI_TRACE_CHANNEL_IORQLOG:
            out->mode_ptr = (int *) &g_iorqlog_config.mode;
            out->fn_reset = NULL;
            out->fn_save  = iorqlog_save_segment;
            out->fn_writer = iorqlog_get_writer;
            return true;
        case DBGAPI_TRACE_CHANNEL_INTLOG:
            out->mode_ptr = (int *) &g_intlog_config.mode;
            out->fn_reset = NULL;
            out->fn_save  = intlog_save_segment;
            out->fn_writer = intlog_get_writer;
            return true;
        case DBGAPI_TRACE_CHANNEL_HWLOG:
            out->mode_ptr = (int *) &g_hwlog_config.mode;
            out->fn_reset = NULL;
            out->fn_save  = hwlog_save_segment;
            out->fn_writer = hwlog_get_writer;
            return true;
        case DBGAPI_TRACE_CHANNEL_MARKLOG:
            out->mode_ptr = (int *) &g_marklog_config.mode;
            out->fn_reset = NULL;
            out->fn_save  = marklog_save_segment;
            out->fn_writer = marklog_get_writer;
            return true;
        default:
            return false;
    }
}


bool dbgapi_trace_channel_recording ( en_DBGAPI_TRACE_CHANNEL channel )
{
    st_DBGAPI_TRACE_CHAN_DESC d;
    if ( !dbgapi_resolve_trace_channel ( channel, &d ) )
    {
        return false;
    }
    return *d.mode_ptr != TLOG_MODE_OFF;
}


int dbgapi_trace_lifecycle ( en_DBGAPI_TRACE_CHANNEL channel,
                             en_DBGAPI_TRACE_OP op,
                             const char *path )
{
    st_DBGAPI_TRACE_CHAN_DESC d;
    if ( !dbgapi_resolve_trace_channel ( channel, &d ) )
    {
        return -1;
    }

    switch ( op )
    {
        case DBGAPI_TRACE_OP_START:
        case DBGAPI_TRACE_OP_STOP:
            /* Nastavit mode + recompute všech kanálů (swap CPU callbacků),
             * analogie mhmap_set_mode pro CDL. */
            *d.mode_ptr = ( op == DBGAPI_TRACE_OP_START )
                          ? TLOG_MODE_ALWAYS : TLOG_MODE_OFF;
            mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
            return 0;

        case DBGAPI_TRACE_OP_RESET:
            /* Subsystem-specifický reset stavu (cputrack collapse) + flush/
             * restart segmentu na stávající dir/name. */
            if ( d.fn_reset ) d.fn_reset ( );
            return d.fn_save ( NULL );

        case DBGAPI_TRACE_OP_SAVE:
            /* Uzavřít aktuální segment, volitelně přesměrovat na path. */
            return d.fn_save ( path );
    }

    return -1;
}


/**
 * @brief Fix C: přepočet gatingu logging callbacků po BP mutaci.
 *
 * Existence callback-dispatchovaného BP (MEM_R/W, IORQ_R/W) musí vynutit
 * swap na logging memory/port callbacky (viz TEST_DEBUGGER_NEED_DEBUG_CALLBACKS),
 * jinak takový BP tiše nestřílí, dokud není otevřené debug okno nebo aktivní
 * recording. Swap se jinak přepočítává jen při změně debug oken / trace / CDL,
 * ne při BP CRUD - proto ho voláme po BP mutačních příkazech. Běží na EMU
 * vlákně (dispatch), stejně jako DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE.
 * Idempotentní - když je stav callbacků už správný, swap je no-op.
 */
static void dbgapi_bp_recompute_cb_gating ( void )
{
    /* Historie: existence enabled BP zapíná CPU historii i bez okna
     * (TEST_DEBUGGER_CPUHIST_ACTIVE -> has_enabled_bp). Add/remove/enable
     * cesty nesyncují, proto flag přepočítáme tady, před swapem callbacků. */
    breakpoints_recompute_has_enabled ( );
    mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
}


bool dbgapi_emu_bp_create ( st_DBGAPI_BP_UPDATE_PARAM *p )
{
    bool ok = dbgapi_emu_bp_apply_update ( p, /* allow_create */ true );
    dbgapi_bp_recompute_cb_gating ( );
    return ok;
}


void dbgapi_emu_bp_recompute_gating ( void )
{
    dbgapi_bp_recompute_cb_gating ( );
}


/* Struktura st_DBGAPI_VIDEOREC_PARAM nese stav a druh události číselně (bez
 * závislosti na videorec.h); hlídáme, že čísla odpovídají enumům jádra. */
G_STATIC_ASSERT ( VIDEOREC_STATE_IDLE == 0 && VIDEOREC_STATE_RECORDING == 1 && VIDEOREC_STATE_PAUSED == 2 );
G_STATIC_ASSERT ( VIDEOREC_EVENT_STARTED == 1 && VIDEOREC_EVENT_SAVED == 2 && VIDEOREC_EVENT_FAILED == 3
                  && VIDEOREC_EVENT_RETAKE == 4 && VIDEOREC_EVENT_SEAM == 5 );
G_STATIC_ASSERT ( VIDEOREC_RETAKE_OFF == 0 && VIDEOREC_RETAKE_DISCARD == 1 && VIDEOREC_RETAKE_SEAM == 2 );
G_STATIC_ASSERT ( sizeof ( ( (st_VIDEOREC_EVENT *) 0 )->path ) == DBGAPI_VIDEOREC_PATH_MAX );
G_STATIC_ASSERT ( sizeof ( ( (st_VIDEOREC_STATUS *) 0 )->path ) == DBGAPI_VIDEOREC_PATH_MAX );
G_STATIC_ASSERT ( sizeof ( ( (st_VIDEOREC_EVENT *) 0 )->text ) == DBGAPI_VIDEOREC_TEXT_MAX );


/**
 * @brief Provede operaci DBGAPI_CMD_VIDEOREC a vyplní souhrnný stav (emu vlákno).
 *
 * Běží na emu vlákně z dbgapi_emu_dispatch(), tj. mimo hooky modulu videorec
 * (mezi zpracováním konců snímků). Kontrola "nahrává se?" a následný požadavek
 * proto nejsou v souběhu se zpracováním požadavků na konci snímku (to dělá
 * totéž vlákno); souběh je možný jen s UI vláknem (tlačítka nahrávání), který
 * řeší zámek modulu - nejhůř dojde k chybě "already running" nebo no-op.
 *
 * Operace:
 * - START: st_VIDEOREC_START s cestou, úrovněmi kanálů
 *   (iface_audio_build_videorec_levels(), stejně jako UI a CLI) a
 *   `stop_after_frames` = `frames`; `quit_after_stop` = false. Chyba z
 *   videorec_request_start() (videorec_get_last_error()) jde do out_error.
 *   videorec_request_start() vytváří cílový AVI soubor **synchronně na emu
 *   vlákně** (a u prázdné cesty ještě zkouší existenci vygenerovaných jmen
 *   `_2`, `_3`, ... a případně zakládá výstupní adresář) - emulace tedy po
 *   dobu těchto souborových operací stojí (jednorázově při startu).
 * - STOP: bez nahrávání a bez čekajícího startu => NOT_RUNNING; jinak
 *   videorec_request_stop() (čekající start zruší - out_start_cancelled).
 * - PAUSE / MARKER: vyžadují stav RECORDING nebo PAUSED, jinak NOT_RUNNING
 *   (s rozlišením čekajícího startu v textu chyby).
 * - STATUS: jen stav.
 * - TIMEBASE: videorec_request_timebase() (0 = emulated, 1 = realtime); funguje
 *   i bez nahrávání (změní nastavení pro příští start) - vždy OK.
 *
 * @param p Parametr (vstupy podle op); výstupy se vždy vyplní.
 * @return true pokud operace uspěla (out_result == DBGAPI_VIDEOREC_RESULT_OK).
 *
 * @par Side effects START vytvoří cílový soubor; marker/pauza/stop zařadí
 *      požadavek na nejbližší konec snímku (stop v pauze emulace zpracuje
 *      paused smyčka).
 */
static bool dbgapi_videorec_execute ( st_DBGAPI_VIDEOREC_PARAM *p )
{
    st_VIDEOREC_STATUS st;
    videorec_get_status ( &st );
    bool running = ( st.state != VIDEOREC_STATE_IDLE );

    p->out_result = DBGAPI_VIDEOREC_RESULT_OK;
    p->out_error[0] = '\0';
    p->out_label[0] = '\0';
    p->out_start_cancelled = 0;

    switch ( p->op )
    {
        case DBGAPI_VIDEOREC_OP_START:
        {
            st_VIDEOREC_START rec;
            memset ( &rec, 0, sizeof ( rec ) );
            g_strlcpy ( rec.path, p->path ? p->path : "", sizeof ( rec.path ) );
            iface_audio_build_videorec_levels ( rec.level, VIDEOREC_AUDIO_MAX_CHANNELS );
            rec.stop_after_frames = p->frames;
            rec.quit_after_stop = false;
            if ( !videorec_request_start ( &rec ) )
            {
                p->out_result = DBGAPI_VIDEOREC_RESULT_FAILED;
                g_strlcpy ( p->out_error, videorec_get_last_error ( ), sizeof ( p->out_error ) );
            };
            break;
        }

        case DBGAPI_VIDEOREC_OP_STOP:
            if ( !running && !st.start_pending )
            {
                p->out_result = DBGAPI_VIDEOREC_RESULT_NOT_RUNNING;
                g_strlcpy ( p->out_error, "Video recording is not running", sizeof ( p->out_error ) );
                break;
            };
            p->out_start_cancelled = ( !running && st.start_pending ) ? 1 : 0;
            videorec_request_stop ( );
            break;

        case DBGAPI_VIDEOREC_OP_PAUSE:
        case DBGAPI_VIDEOREC_OP_MARKER:
            if ( !running )
            {
                p->out_result = DBGAPI_VIDEOREC_RESULT_NOT_RUNNING;
                g_strlcpy ( p->out_error,
                            st.start_pending
                                ? "Video recording has not started yet: it starts at the end of the next emulated frame (run the emulation first)"
                                : "Video recording is not running",
                            sizeof ( p->out_error ) );
                break;
            };
            if ( p->op == DBGAPI_VIDEOREC_OP_PAUSE )
            {
                if ( p->paused < 0 )
                {
                    videorec_request_pause_toggle ( );
                }
                else
                {
                    (void) videorec_request_pause_set ( p->paused != 0 );
                };
            }
            else
            {
                if ( p->label && p->label[0] != '\0' )
                {
                    g_strlcpy ( p->out_label, p->label, sizeof ( p->out_label ) );
                }
                else
                {
                    /* Výchozí popisek jde do sidecaru (strojově čtený) - anglicky. */
                    g_snprintf ( p->out_label, sizeof ( p->out_label ), "Marker at frame %" G_GUINT64_FORMAT,
                                 (guint64) st.frames );
                };
                videorec_request_marker ( p->out_label );
            };
            break;

        case DBGAPI_VIDEOREC_OP_STATUS:
            break;

        case DBGAPI_VIDEOREC_OP_TIMEBASE:
            (void) videorec_request_timebase ( p->timebase ? VIDEOREC_TIMEBASE_REALTIME : VIDEOREC_TIMEBASE_EMULATED );
            break;

        default:
            p->out_result = DBGAPI_VIDEOREC_RESULT_FAILED;
            g_strlcpy ( p->out_error, "Unknown videorec operation", sizeof ( p->out_error ) );
            break;
    };

    /* Souhrnný stav po operaci. */
    videorec_get_status ( &st );
    p->out_supported = videorec_is_supported ( ) ? 1 : 0;
    p->out_state = (int) st.state;
    p->out_start_pending = st.start_pending ? 1 : 0;
    p->out_frames = st.frames;
    p->out_fps = videorec_get_fps ( );
    p->out_segment = st.segment;
    p->out_segment_open = st.segment_open ? 1 : 0;
    p->out_bytes = st.bytes;
    p->out_parts = st.parts;
    /* Režim retake: běžící (i čekající) session má kopii ze startu, jinak platí nastavení pro příští start. */
    p->out_retake_mode = ( st.state != VIDEOREC_STATE_IDLE || st.start_pending )
                             ? (int) st.retake_mode
                             : (int) g_videorec_settings.retake_mode;
    g_strlcpy ( p->out_path, st.path, sizeof ( p->out_path ) );
    g_strlcpy ( p->out_last_error, videorec_get_last_error ( ), sizeof ( p->out_last_error ) );
    p->out_timebase = (int) st.timebase;
    p->out_timebase_effective = (int) st.timebase_effective;
    p->out_rt_activity = (int) st.rt_activity;

    st_VIDEOREC_EVENT ev;
    uint32_t seq = videorec_get_event_seq ( );
    if ( seq != 0 && videorec_get_event ( seq, &ev ) )
    {
        p->out_event_seq = ev.seq;
        p->out_event_kind = (int) ev.kind;
        p->out_event_frame = ev.frame;
        g_strlcpy ( p->out_event_path, ev.path, sizeof ( p->out_event_path ) );
        g_strlcpy ( p->out_event_text, ev.text, sizeof ( p->out_event_text ) );
    }
    else
    {
        p->out_event_seq = 0;
        p->out_event_kind = 0;
        p->out_event_frame = 0;
        p->out_event_path[0] = '\0';
        p->out_event_text[0] = '\0';
    };

    return ( p->out_result == DBGAPI_VIDEOREC_RESULT_OK );
}


/**
 * @brief Testovací háček: doba spánku v ms před dispatchem armovaného
 *        příkazu (0 = vypnuto). Viz dbgapi_test_arm_dispatch_stall().
 */
static gint s_test_stall_ms = 0;

/** @brief Testovací háček: příkaz (en_DBGAPI_CMD), na který háček čeká. */
static gint s_test_stall_cmd = (gint)DBGAPI_CMD_NONE;


void dbgapi_test_arm_dispatch_stall(en_DBGAPI_CMD cmd, int ms)
{
    /* Napřed příkaz, potom doba: dispatch háček uplatní až po přečtení
     * nenulové doby, takže vždy vidí odpovídající příkaz. */
    g_atomic_int_set(&s_test_stall_cmd, (gint)cmd);
    g_atomic_int_set(&s_test_stall_ms, ms > 0 ? (gint)ms : 0);
}


/**
 * @brief Uplatní testovací háček zaseknutí, pokud je armovaný pro @p rq.
 *
 * Jednorázový: při shodě příkazu (a originu MCP) háček atomicky vypne
 * a emu vlákno uspí. V produkci háček nikdo nearmuje, cena je jedno
 * atomické čtení na příkaz dbgapi (ne na instrukci).
 *
 * @param rq  Právě dispatchovaný slot.
 * @param cmd Příkaz slotu bez BLOCKING flagu.
 */
static void dbgapi_test_apply_dispatch_stall(const st_DBGAPI_CMDRQ *rq,
                                             en_DBGAPI_CMD cmd)
{
    gint ms = g_atomic_int_get(&s_test_stall_ms);
    if (G_LIKELY(ms <= 0))
        return;
    if (rq->cmd_origin != DBGAPI_CMD_ORIGIN_MCP
        || g_atomic_int_get(&s_test_stall_cmd) != (gint)cmd)
        return;
    if (!g_atomic_int_compare_and_exchange(&s_test_stall_ms, ms, 0))
        return;
    fprintf(stderr, "[dbgapi] test hook: stalling emulator thread for %d ms\n",
            (int)ms);
    g_usleep((gulong)ms * 1000);
}


/**
 * @brief Vyplní hash obrazu, hash VRAM a pozici paprsku (DBGAPI_CMD_FRAME_HASH,
 *        screenshoty).
 *
 * Obraz: SHA-256 W x H bajtů indexů palety (INDEX8 & 0Fh, po řádcích),
 * za nimi 16 položek palety po 3 bajtech R, G, B (st_DBGAPI_FRAME_HASH_PARAM).
 * VRAM: state_hash_part(STATE_HASH_VRAM).
 *
 * @param[out] out Výstup (přepíše se celý).
 * @param src INDEX8 obraz VIDEO_DISPLAY_WIDTH x VIDEO_DISPLAY_HEIGHT, nebo NULL
 *        (obraz nedostupný: available false, image_sha256 prázdný).
 * @param source SCREENSHOT_SRC_SDL_SNAPSHOT / SCREENSHOT_SRC_GDG_LIVE.
 * @param screen_id fbsnapshot_screen_id zdroje.
 * @pre Emu vlákno (drain fronty); @p src platný po dobu volání (u SDL
 *      snímku volající drží fbsnapshot_pixels_mutex).
 */
static void dbgapi_frame_hash_fill ( st_DBGAPI_FRAME_HASH_PARAM *out, const uint8_t *src,
                                     uint8_t source, uint32_t screen_id )
{
    memset ( out, 0, sizeof ( *out ) );
    out->source = source;
    out->source_screen_id = screen_id;
    out->width = (uint32_t) VIDEO_DISPLAY_WIDTH;
    out->height = (uint32_t) VIDEO_DISPLAY_HEIGHT;
    out->frame = (uint32_t) g_gdg.total_elapsed.screens;
    out->scanline = (uint16_t) g_gdg.beam_row;
    out->column = (uint16_t) VIDEO_GET_SCREEN_COL ( g_gdg.total_elapsed.ticks );
    out->paused = EMULATOR_TEST_PAUSED ? true : false;
    (void) state_hash_part ( STATE_HASH_VRAM, out->vram_sha256 );

    uint32_t *colormap = display_get_default_color_schema ( );
    if ( !src || !colormap ) return;

    GChecksum *cs = g_checksum_new ( G_CHECKSUM_SHA256 );
    uint8_t row[ VIDEO_DISPLAY_WIDTH ];
    for ( uint32_t y = 0; y < (uint32_t) VIDEO_DISPLAY_HEIGHT; y++ )
    {
        const uint8_t *src_row = src + y * (uint32_t) VIDEO_DISPLAY_WIDTH;
        for ( uint32_t x = 0; x < (uint32_t) VIDEO_DISPLAY_WIDTH; x++ )
        {
            row[ x ] = src_row[ x ] & 0x0Fu;
        };
        g_checksum_update ( cs, row, sizeof ( row ) );
    };
    for ( unsigned i = 0; i < 16; i++ )
    {
        uint8_t rgb[ 3 ] = {
            (uint8_t) ( ( colormap[ i ] >> 16 ) & 0xFFu ),
            (uint8_t) ( ( colormap[ i ] >> 8 ) & 0xFFu ),
            (uint8_t) ( colormap[ i ] & 0xFFu ),
        };
        g_checksum_update ( cs, rgb, sizeof ( rgb ) );
    };
    g_strlcpy ( out->image_sha256, g_checksum_get_string ( cs ), sizeof ( out->image_sha256 ) );
    g_checksum_free ( cs );
    out->available = true;
}


/**
 * @brief Naplní snímek mapování 16 stránek 4 KiB (DBGAPI_CMD_GET_MEMORY_MAP).
 *
 * Sdílené plnění pro DBGAPI_CMD_GET_MEMORY_MAP a DBGAPI_CMD_STATE_OBSERVE.
 * Druh stránky (`kind`) určí memmap_query() podle aktuálního mapování
 * (stejně jako kontrola regionu u zápisů), zdroj a příznak zápisu se od
 * druhu odvodí:
 *  - RAM: zdroj RAM s offsetem stránky v RAM; s připojeným MemExtem
 *    MEMEXT_RAM / MEMEXT_FLASH a offset v bance MemExtu,
 *  - ROM (dolní, horní): ROM, jen čtení,
 *  - CG-ROM: CGROM, jen čtení,
 *  - VRAM, CG-RAM, PCG: VRAM, čtení i zápis,
 *  - mapované porty, Prohibited, nepřipojeno: zdroj 0 (unknown), jen
 *    čtení; podrobnost nese `kind`.
 *
 * @param[out] p Výstup; přepíše se celý.
 * @pre Emu vlákno (drain fronty).
 */
static void dbgapi_memory_map_fill ( st_DBGAPI_MEMORY_MAP_PARAM *p )
{
    memset ( p, 0, sizeof ( *p ) );

    const char *plat = "unknown";
    switch ( g_mzarch_platform_numeric )
    {
        case 700:  plat = "mz700"; break;
        case 800:  plat = "mz800"; break;
        case 1500: plat = "mz1500"; break;
        default:   plat = "unknown"; break;
    };
    g_strlcpy ( p->platform, plat, sizeof ( p->platform ) );
    g_strlcpy ( p->mode_note,
        "16 slots of 4 KB; kind from current banking, memext aware.",
        sizeof ( p->mode_note ) );

    for ( int i = 0; i < 16; i++ )
    {
        st_DBGAPI_MEMORY_MAP_SLOT *sl = &p->slots[ i ];
        sl->addr_start  = (uint16_t) ( i * 0x1000 );
        sl->addr_end    = (uint16_t) ( i * 0x1000 + 0x0FFF );
        sl->kind        = (uint8_t) memmap_query ( (uint8_t) i );
        sl->source      = 0; /* unknown */
        sl->ro_rw       = 0;
        sl->slot_offset = 0;

        switch ( (en_MEMMAP_REGION_KIND) sl->kind )
        {
            case MEMMAP_KIND_RAM:
                sl->source      = 3; /* RAM */
                sl->ro_rw       = 1;
                sl->slot_offset = (uint32_t) i * 0x1000;
                break;
            case MEMMAP_KIND_ROM_LOW:
            case MEMMAP_KIND_ROM_HIGH:
                sl->source = 1; /* ROM */
                break;
            case MEMMAP_KIND_CGROM:
                sl->source = 2; /* CGROM */
                break;
            case MEMMAP_KIND_VRAM_I:
            case MEMMAP_KIND_VRAM_II:
            case MEMMAP_KIND_VRAM_TEXT:
            case MEMMAP_KIND_CGRAM:
            case MEMMAP_KIND_PCG_1:
            case MEMMAP_KIND_PCG_2:
            case MEMMAP_KIND_PCG_3:
                sl->source = 4; /* VRAM */
                sl->ro_rw  = 1;
                break;
            default:
                break;
        };

        /* MemExt nahrazuje jen RAM; ROM, CG-ROM a VRAM mají přednost
         * (memmap_query je hlásí podle mapování bez ohledu na MemExt). */
        if ( sl->kind != MEMMAP_KIND_RAM || !MEMEXT_TEST_CONNECTED ) continue;

        /* g_memext.map[i] = raw bank index. Pro Luftner:
         * 0..0x7F = RAM, 0x80..0xFF = FLASH. Pro PEHU jen
         * 0..0x3F (RAM only). */
        uint32_t rb = g_memext.map[ i ];
        if ( MEMEXT_TEST_TYPE_LUFTNER )
        {
            if ( rb < MEMEXT_LUFTNER_BANKS )
            {
                sl->source = 5; /* MEMEXT_RAM */
                sl->ro_rw  = 1;
                sl->slot_offset = rb * MEMEXT_RAW_BANK_SIZE;
            }
            else
            {
                sl->source = 6; /* MEMEXT_FLASH */
                sl->ro_rw  = 0;
                sl->slot_offset = ( rb - MEMEXT_LUFTNER_BANKS ) * MEMEXT_RAW_BANK_SIZE;
            };
        }
        else if ( MEMEXT_TEST_TYPE_PEHU )
        {
            if ( rb < MEMEXT_PEHU_BANKS * 2 )
            {
                sl->source = 5; /* MEMEXT_RAM */
                sl->ro_rw  = 1;
                sl->slot_offset = rb * MEMEXT_RAW_BANK_SIZE;
            };
        };
    };
    p->slot_count = 16;
}


/**
 * @brief Naplní snímek panelu CPU (registry, příznaky, volitelné sekce).
 *
 * Společné plnění DBGAPI_CMD_GET_CPU_PANEL_BATCH a DBGAPI_CMD_STATE_OBSERVE,
 * aby obě odpovědi nesly registry a příznaky stejné definice. Regs,
 * flags, frame_number, user_cycle_origin a vektory PIO-Z80 (s HAVE_PIOZ80)
 * se plní vždy, sekce IM2, raster a poslední
 * instrukce jen s odpovídajícím bitem DBGAPI_CPU_PANEL_WANT_* ve @p which.
 * Paměť (tabulky vektorů, poslední instrukce z historie) se čte bez
 * vedlejších efektů (debugger_memory_read_byte()).
 *
 * @param[out] out   Výstup; přepíše se celý (memset), nevyžádané sekce
 *                   mají *_valid 0.
 * @param      which Bity DBGAPI_CPU_PANEL_WANT_*.
 * @pre Emu vlákno v drainu fronty (CPU mezi instrukcemi);
 *      `g_mzarch_main.cpu` existuje.
 */
static void dbgapi_cpu_panel_fill ( st_DBGAPI_CPU_PANEL_BATCH *out, uint32_t which )
{
    memset ( out, 0, sizeof ( *out ) );
    out->which = which;

    z80_t *cpu = g_mzarch_main.cpu;

    /* === Regs (vždy) === */
    for ( int i = 0; i < DBGAPI_REG_COUNT; i++ )
    {
        out->regs[ i ] = z80_get_reg ( cpu, (z80_reg_t) i );
    };
    out->regs_valid = 1;

    /* === Flags (vždy) === */
    out->flags.iff1         = cpu->iff1 ? 1 : 0;
    out->flags.iff2         = cpu->iff2 ? 1 : 0;
    out->flags.im           = cpu->im;
    out->flags.halted       = cpu->halted ? 1 : 0;
    out->flags.int_pending  = cpu->int_pending ? 1 : 0;
    out->flags.nmi_pending  = cpu->nmi_pending ? 1 : 0;
    out->flags.ei_delay     = cpu->ei_delay ? 1 : 0;
    out->flags.q            = cpu->q;
    out->flags.total_cycles = cpu->total_cycles;
    out->flags.cycles       = cpu->cycles;
    out->flags.op_tstate    = cpu->op_tstate;
    out->flags.update_mask  = 0;
    out->flags.i_reg        = cpu->i;
    out->flags.r_reg        = cpu->r;
    out->flags_valid = 1;

    /* === V3.1 core fieldy (vzdy plnene): frame_number a
     * user_cycle_origin. UI z toho odvozuje Frame cyc
     * (= total_cycles - snapshot pri zmene frame_number) a
     * User cyc (= total_cycles - user_cycle_origin) i pokud
     * "Cycles & raster" sekce je collapsed. */
    out->frame_number      = (uint32_t) g_gdg.total_elapsed.screens;
    out->user_cycle_origin = g_debugger.user_cycle_origin;

    /* === V3.3 core: PIO-Z80 interrupt vectors + ISR targets.
     * Vždy plněné při HAVE_PIOZ80 (MZ-800, MZ-1500), aby UI
     * sekce VECA/ISRA + VECB/ISRB dostala data v každém ticku.
     * Pro MZ-700 (HAVE_PIOZ80 == 0) má_pioz80 = 0; UI sekce
     * se na MZ-700 nezobrazuje. */
#if HAVE_PIOZ80
    {
        out->has_pioz80    = 1;
        uint8_t va = g_pioz80.port[ PIOZ80_PORT_A ].interrupt_vector;
        uint8_t vb = g_pioz80.port[ PIOZ80_PORT_B ].interrupt_vector;
        out->pio_int_vec_a = va;
        out->pio_int_vec_b = vb;
        uint16_t vec_a = (uint16_t) ( ( (uint16_t) cpu->i << 8 ) | (uint8_t) ( va & 0xFE ) );
        uint16_t vec_b = (uint16_t) ( ( (uint16_t) cpu->i << 8 ) | (uint8_t) ( vb & 0xFE ) );
        out->veca = vec_a;
        out->vecb = vec_b;
        {
            uint8_t lo_a = debugger_memory_read_byte ( vec_a );
            uint8_t hi_a = debugger_memory_read_byte ( (uint16_t) ( vec_a + 1 ) );
            out->isra = (uint16_t) ( lo_a | ( hi_a << 8 ) );
        };
        {
            uint8_t lo_b = debugger_memory_read_byte ( vec_b );
            uint8_t hi_b = debugger_memory_read_byte ( (uint16_t) ( vec_b + 1 ) );
            out->isrb = (uint16_t) ( lo_b | ( hi_b << 8 ) );
        };
    };
#else
    out->has_pioz80 = 0;
#endif

    /* === IM2 ISR vector (volitelne) === */
    if ( which & DBGAPI_CPU_PANEL_WANT_IM2 )
    {
        st_DBGAPI_IM2_VECTOR *im2 = &out->im2;
#if HAVE_PIOZ80
        im2->available  = 1;
        im2->im         = cpu->im;
        im2->i_register = cpu->i;
        im2->pio_irq_pending = 0;
        im2->pio_source      = 0;
        im2->vector_byte     = 0;
        if ( g_pioz80.interrupt == PIOZ80_INTERRUPT_PENDING )
        {
            for ( int pid = PIOZ80_PORT_A; pid < PIOZ80_PORT_COUNT; pid++ )
            {
                st_PIOZ80_PORT *port = &g_pioz80.port[ pid ];
                if ( port->port_int == PIOZ80_PORT_INT_PENDING
                     && port->icena == PIOZ80_ICENA_ENABLED )
                {
                    im2->pio_irq_pending = 1;
                    im2->pio_source      = (uint8_t) pid;
                    im2->vector_byte     = port->interrupt_vector;
                    break;
                };
            };
        };
        im2->isr_table_addr =
            (uint16_t) ( ( (uint16_t) im2->i_register << 8 ) | im2->vector_byte );
        {
            uint8_t lo = debugger_memory_read_byte ( im2->isr_table_addr );
            uint8_t hi = debugger_memory_read_byte ( (uint16_t) ( im2->isr_table_addr + 1 ) );
            im2->isr_target_addr = (uint16_t) ( lo | ( hi << 8 ) );
        };
#else
        im2->available = 0;
#endif
        out->im2_valid = 1;
    };

    /* === Raster pos + cycles (volitelne) === */
    if ( which & DBGAPI_CPU_PANEL_WANT_RASTER )
    {
        st_DBGAPI_RASTER_POS *r = &out->raster;
        r->frame_number = (uint32_t) g_gdg.total_elapsed.screens;
        r->scanline     = (uint16_t) g_gdg.beam_row;
        r->column_pixel = (uint16_t) VIDEO_GET_SCREEN_COL ( g_gdg.total_elapsed.ticks );
        r->total_cycles = cpu->total_cycles;
        r->frame_cycles = cpu->cycles;
        out->raster_valid = 1;
    };

    /* === Last instruction (volitelne) === */
    if ( which & DBGAPI_CPU_PANEL_WANT_LAST_INSTR )
    {
        st_DBGAPI_LAST_INSTR *li = &out->last_instr;
        unsigned pos = debugger_history_position ( g_debugger_history.position );
        st_DEBUGGER_HISTORY_ROW *row = &g_debugger_history.row[ pos ];
        bool empty = ( g_debugger_history.position == 0
                       && row->addr == 0
                       && row->byte[ 0 ] == 0
                       && row->byte[ 1 ] == 0
                       && row->byte[ 2 ] == 0
                       && row->byte[ 3 ] == 0 );
        if ( empty )
        {
            li->valid = 0;
        }
        else
        {
            li->valid = 1;
            li->addr  = row->addr;
            for ( int b = 0; b < 4; b++ )
                li->bytes[ b ] = row->byte[ b ];
            z80_dasm_inst_t inst;
            int len = z80_dasm ( &inst,
                                 dbgapi_last_instr_read_cb,
                                 row,
                                 row->addr );
            if ( len < 1 ) len = 1;
            if ( len > 4 ) len = 4;
            li->length = (uint8_t) len;
        };
        out->last_instr_valid = 1;
    };
}


G_STATIC_ASSERT ( DBGAPI_OBSERVE_HASH_PARTS == (unsigned) STATE_HASH_PART_COUNT );
G_STATIC_ASSERT ( DBGAPI_OBSERVE_PAGE_KINDS == (unsigned) MEMMAP_KIND_UNMAPPED + 1u );

/**
 * @brief Přečte jedno okno STATE_OBSERVE bez vedlejších efektů.
 *
 * Okno CPU čte přes aktuální mapování (debugger_memory_read_byte(), bez
 * MEM_R BP). Okno regionu dohledá podle druhu a `sub_id` ve výčtu
 * obnoveném volajícím a čte dbgapi_regions_read(). Nedostupný region
 * a přesah přes jeho konec okno vynulují a nastaví `status`.
 *
 * @param w Okno; `buf` má aspoň `len` bajtů.
 * @pre Emu vlákno; u okna regionu proběhl dbgapi_regions_refresh().
 */
static void dbgapi_observe_read_window ( st_DBGAPI_OBSERVE_WINDOW *w )
{
    w->status = (uint8_t) DBGAPI_OBSERVE_WINDOW_OK;
    w->region_size = 0;
    if ( !w->by_region )
    {
        for ( uint32_t i = 0; i < w->len; i++ )
        {
            w->buf[ i ] = debugger_memory_read_byte ( (uint16_t) ( w->start + i ) );
        };
        return;
    };
    st_REGION_DESC d;
    if ( !dbgapi_regions_find_kind ( (en_REGION_KIND) w->region_kind, w->sub_id, &d )
         || !d.connected )
    {
        w->status = (uint8_t) DBGAPI_OBSERVE_WINDOW_UNAVAILABLE;
    }
    else if ( w->start >= d.size || w->len > d.size - w->start )
    {
        w->region_size = d.size;
        w->status = (uint8_t) DBGAPI_OBSERVE_WINDOW_OUT_OF_RANGE;
    }
    else
    {
        w->region_size = d.size;
        if ( dbgapi_regions_read ( d.id, w->start, w->buf, w->len ) != (int) w->len )
        {
            w->status = (uint8_t) DBGAPI_OBSERVE_WINDOW_UNAVAILABLE;
        };
    };
    if ( w->status != (uint8_t) DBGAPI_OBSERVE_WINDOW_OK )
    {
        memset ( w->buf, 0, w->len );
    };
}


/**
 * @brief Vyplní výsledek DBGAPI_CMD_STATE_OBSERVE.
 *
 * Registry a příznaky (dbgapi_cpu_panel_fill()), stav běhu, číslo běhu,
 * čas, okna paměti, vyžádané hashe částí stavu (state_hash_part())
 * a mapu paměti, vše v jednom volání v emu vlákně, tedy z jednoho
 * okamžiku emulace. Nic se nezapisuje.
 *
 * @param p Parametr odesílatele.
 * @return false = neplatné parametry (počet oken, součet délek, okno bez
 *         bufferu, nulová délka, CPU okno přes 0FFFFh); nic se nečte.
 * @pre Emu vlákno v drainu na hranici snímku nebo v pauze.
 */
static bool dbgapi_state_observe_fill ( st_DBGAPI_STATE_OBSERVE_PARAM *p )
{
    if ( p->window_count > DBGAPI_OBSERVE_MAX_WINDOWS ) return false;
    if ( p->window_count > 0 && !p->windows ) return false;
    uint64_t total = 0;
    for ( uint32_t i = 0; i < p->window_count; i++ )
    {
        const st_DBGAPI_OBSERVE_WINDOW *w = &p->windows[ i ];
        if ( !w->buf || w->len == 0 ) return false;
        if ( !w->by_region && (uint64_t) w->start + w->len > 0x10000u ) return false;
        total += w->len;
    };
    if ( total > DBGAPI_OBSERVE_MAX_TOTAL ) return false;

    dbgapi_cpu_panel_fill ( &p->panel, p->which );
    p->running = EMULATOR_TEST_PAUSED ? 0 : 1;
    p->run_seq = run_log_current ( NULL );
    p->frame = (uint64_t) g_gdg.total_elapsed.screens;
    p->frame_ticks = (uint32_t) g_gdg.total_elapsed.ticks;
    p->ticks = gdg_get_total_ticks ( );
    p->total_cycles = g_mzarch_main.cpu->total_cycles;

    bool regions_fresh = false;
    for ( uint32_t i = 0; i < p->window_count; i++ )
    {
        st_DBGAPI_OBSERVE_WINDOW *w = &p->windows[ i ];
        if ( w->by_region && !regions_fresh )
        {
            /* Čerstvé příznaky connected a ID podle aktuální sestavy HW. */
            dbgapi_regions_refresh ( );
            regions_fresh = true;
        };
        dbgapi_observe_read_window ( w );
    };

    for ( unsigned part = 0; part < DBGAPI_OBSERVE_HASH_PARTS; part++ )
    {
        p->hash_ok[ part ] = 0;
        p->hash_hex[ part ][ 0 ] = '\0';
        if ( p->hash_mask & ( 1u << part ) )
        {
            p->hash_ok[ part ] = state_hash_part ( (en_STATE_HASH_PART) part,
                                                  p->hash_hex[ part ] ) ? 1 : 0;
        };
    };

    if ( p->want_memory_map )
    {
        dbgapi_memory_map_fill ( &p->memory_map );
        for ( uint8_t page = 0; page < 16; page++ )
        {
            p->page_kind[ page ] = (uint8_t) memmap_query ( page );
        };
    };
    return true;
}


/**
 * @brief SHA-256 rozsahu CPU adres přes aktuální mapování (bez vedlejších efektů).
 *
 * Čte debugger_memory_read_byte() (memory_read_byte_no_se, bez MEM_R BP),
 * stejně jako MEM_READ, takže hash odpovídá `sha256` z mem_read téhož
 * rozsahu.
 *
 * @param addr Počáteční adresa.
 * @param len  Počet bajtů (addr + len <= 10000h, jinak se adresa přetočí).
 * @param[out] out 32 bajtů hashe.
 * @pre Emu vlákno.
 */
static void dbgapi_hash_cpu_range ( uint16_t addr, uint32_t len, uint8_t out[ 32 ] )
{
    GChecksum *cs = g_checksum_new ( G_CHECKSUM_SHA256 );
    uint8_t chunk[ 256 ];
    uint32_t done = 0;
    while ( done < len )
    {
        uint32_t n = len - done;
        if ( n > sizeof ( chunk ) ) n = sizeof ( chunk );
        for ( uint32_t i = 0; i < n; i++ )
        {
            chunk[ i ] = debugger_memory_read_byte ( (uint16_t) ( addr + done + i ) );
        };
        g_checksum_update ( cs, chunk, n );
        done += n;
    };
    gsize dlen = 32;
    g_checksum_get_digest ( cs, out, &dlen );
    g_checksum_free ( cs );
}


/**
 * @brief Vyhodnotí podmínku zápisu nad hashem rozsahu před zápisem.
 *
 * @param c Podmínka (NULL = bez podmínky, vrací true).
 * @param before SHA-256 přepisovaného rozsahu, nebo NULL, pokud podmínka
 *        nic nepožaduje (pak se nic nepočítalo).
 * @return true = zapisovat; false = hash nesouhlasí s `expected`
 *         (nastaví precondition_failed, nic se nesmí zapsat).
 * @post Bez požadavku (has_expected i want_hashes 0) zůstanou výstupy
 *       nulové; jinak `before` a hashes_valid platí.
 */
static bool dbgapi_write_cond_check ( st_DBGAPI_WRITE_COND *c, const uint8_t *before )
{
    if ( !c ) return true;
    c->precondition_failed = 0;
    c->hashes_valid = 0;
    c->written = 0;
    if ( !before ) return true;
    memcpy ( c->before, before, 32 );
    c->hashes_valid = 1;
    if ( c->has_expected && memcmp ( before, c->expected, 32 ) != 0 )
    {
        c->precondition_failed = 1;
        return false;
    };
    return true;
}


/**
 * @brief Má podmínka zápisu spočítat hash před zápisem?
 * @param c Podmínka nebo NULL.
 * @return true = has_expected nebo want_hashes.
 */
static inline bool dbgapi_write_cond_wants_hash ( const st_DBGAPI_WRITE_COND *c )
{
    return c && ( c->has_expected || c->want_hashes );
}


/**
 * @brief Podmínka a hash před zápisem pro rozsah CPU adres.
 *
 * @param c Podmínka nebo NULL.
 * @param addr Počáteční adresa zápisu.
 * @param len Délka zápisu.
 * @return true = zapisovat; false = podmínka nesplněna.
 * @pre Emu vlákno, mezi kontrolou a zápisem se stav nemění.
 */
static bool dbgapi_write_cond_before_cpu ( st_DBGAPI_WRITE_COND *c, uint16_t addr, uint32_t len )
{
    if ( !dbgapi_write_cond_wants_hash ( c ) ) return dbgapi_write_cond_check ( c, NULL );
    uint8_t h[ 32 ];
    dbgapi_hash_cpu_range ( addr, len, h );
    return dbgapi_write_cond_check ( c, h );
}


/**
 * @brief Hash po zápisu pro rozsah CPU adres.
 * @param c Podmínka nebo NULL (pak nic).
 * @param addr Počáteční adresa zápisu.
 * @param len Délka zápisu.
 */
static void dbgapi_write_cond_after_cpu ( st_DBGAPI_WRITE_COND *c, uint16_t addr, uint32_t len )
{
    if ( !c || !c->hashes_valid ) return;
    dbgapi_hash_cpu_range ( addr, len, c->after );
    c->written = 1;
}


/**
 * @brief Hash bajtů regionu (bez vedlejších efektů, dbgapi_regions_read_gen()).
 *
 * @param region_id ID regionu.
 * @param layout_gen Generace výčtu (0 = bez kontroly).
 * @param offset Začátek v regionu.
 * @param len Požadovaná délka.
 * @param[out] out 32 bajtů hashe přes skutečně přečtené bajty.
 * @return Počet přečtených bajtů (po ořezu na konec regionu), < 0 = chyba
 *         čtení (neplatné ID, starší generace); hash pak neplatí.
 */
static int dbgapi_hash_region_range ( int region_id, uint32_t layout_gen, uint32_t offset,
                                      uint32_t len, uint8_t out[ 32 ] )
{
    uint8_t *buf = g_malloc ( len ? len : 1 );
    int n = dbgapi_regions_read_gen ( region_id, layout_gen, offset, buf, len );
    if ( n >= 0 )
    {
        GChecksum *cs = g_checksum_new ( G_CHECKSUM_SHA256 );
        g_checksum_update ( cs, buf, (gssize) n );
        gsize dlen = 32;
        g_checksum_get_digest ( cs, out, &dlen );
        g_checksum_free ( cs );
    };
    g_free ( buf );
    return n;
}


/**
 * @brief Podmínka a hash před zápisem do regionu (REGIONS_WRITE).
 *
 * Neplatné ID, starší generace a region jen pro čtení podmínku
 * nevyhodnotí (vrací true): zápis pak skončí vlastní chybou, která má
 * přednost před podmínkou.
 *
 * @param p Parametr zápisu (p->cond smí být NULL).
 * @return true = pokračovat zápisem; false = podmínka nesplněna.
 * @pre Emu vlákno.
 */
static bool dbgapi_write_cond_before_region ( st_DBGAPI_REGIONS_WRITE_PARAM *p )
{
    st_DBGAPI_WRITE_COND *c = p->cond;
    if ( !dbgapi_write_cond_wants_hash ( c ) ) return dbgapi_write_cond_check ( c, NULL );
    (void) dbgapi_write_cond_check ( c, NULL );
    uint8_t h[ 32 ];
    if ( dbgapi_hash_region_range ( p->region_id, p->layout_gen, p->offset, p->len, h ) < 0 )
        return true;
    st_REGION_DESC d;
    if ( !dbgapi_regions_get ( p->region_id, &d ) || !d.writable || !d.connected )
        return true;
    return dbgapi_write_cond_check ( c, h );
}


/**
 * @brief Hash po zápisu do regionu přes skutečně zapsané bajty.
 * @param p Parametr zápisu po dbgapi_regions_write_gen() (out_count).
 */
static void dbgapi_write_cond_after_region ( st_DBGAPI_REGIONS_WRITE_PARAM *p )
{
    st_DBGAPI_WRITE_COND *c = p->cond;
    if ( !c || !c->hashes_valid || p->out_count < 0 ) return;
    if ( dbgapi_hash_region_range ( p->region_id, p->layout_gen, p->offset,
                                    (uint32_t) p->out_count, c->after ) >= 0 )
    {
        c->written = 1;
    };
}


/**
 * @brief Skončil příkaz zápisu nesplněnou podmínkou (observe-cas M10)?
 *
 * Rozliší odmítnutý podmíněný zápis od ostatních neúspěchů, aby ho
 * Activity ukázala jako "rejected". Příkaz odmítnutý branou záznamu
 * vstupů má příznak nulový (obsluha neproběhla).
 *
 * @param rq Zpracovaný slot.
 * @return true = MEM_WRITE_CHECKED, MEM_WRITE_FORCE, REGIONS_WRITE,
 *         SET_REG nebo SET_CPU_FLAGS s nesplněnou podmínkou.
 */
static bool dbgapi_cmd_precondition_failed ( const st_DBGAPI_CMDRQ *rq )
{
    if ( !rq->data_ptr ) return false;
    switch ( (en_DBGAPI_CMD) ( rq->cmd & DBGAPI_CMD_MASK ) )
    {
        case DBGAPI_CMD_MEM_WRITE_CHECKED:
        {
            const st_DBGAPI_MEM_WRITE_CHECKED_PARAM *p = rq->data_ptr;
            return p->cond && p->cond->precondition_failed;
        }
        case DBGAPI_CMD_MEM_WRITE_FORCE:
        {
            const st_DBGAPI_MEM_PARAM *p = rq->data_ptr;
            return p->cond && p->cond->precondition_failed;
        }
        case DBGAPI_CMD_REGIONS_WRITE:
        {
            const st_DBGAPI_REGIONS_WRITE_PARAM *p = rq->data_ptr;
            return p->cond && p->cond->precondition_failed;
        }
        case DBGAPI_CMD_SET_REG:
            return ( (const st_DBGAPI_REG_PARAM *) rq->data_ptr )->precondition_failed != 0;
        case DBGAPI_CMD_SET_CPU_FLAGS:
            return ( (const st_DBGAPI_CPU_FLAGS *) rq->data_ptr )->precondition_failed != 0;
        default:
            return false;
    };
}


/**
 * @brief Vyhodnotí podmínku SET_CPU_FLAGS a vyplní předchozí hodnoty.
 *
 * @param p Parametr SET_CPU_FLAGS.
 * @param cpu CPU.
 * @return true = zapisovat; false = některé pole z expect_mask má jinou
 *         hodnotu (precondition_failed = 1).
 * @post prev_* nesou hodnoty před zápisem (vždy).
 */
static bool dbgapi_cpu_flags_cond_check ( st_DBGAPI_CPU_FLAGS *p, const z80_t *cpu )
{
    p->prev_iff1 = cpu->iff1 ? 1 : 0;
    p->prev_iff2 = cpu->iff2 ? 1 : 0;
    p->prev_im = cpu->im;
    p->prev_i = cpu->i;
    p->prev_r = cpu->r;
    p->precondition_failed = 0;
    uint16_t m = p->expect_mask;
    bool ok = true;
    if ( ( m & DBGAPI_CPU_FLAGS_UM_IFF1 ) && p->prev_iff1 != ( p->exp_iff1 ? 1 : 0 ) ) ok = false;
    if ( ( m & DBGAPI_CPU_FLAGS_UM_IFF2 ) && p->prev_iff2 != ( p->exp_iff2 ? 1 : 0 ) ) ok = false;
    if ( ( m & DBGAPI_CPU_FLAGS_UM_IM ) && p->prev_im != p->exp_im ) ok = false;
    if ( ( m & DBGAPI_CPU_FLAGS_UM_I ) && p->prev_i != p->exp_i ) ok = false;
    if ( ( m & DBGAPI_CPU_FLAGS_UM_R_FULL ) && p->prev_r != p->exp_r ) ok = false;
    else if ( ( m & DBGAPI_CPU_FLAGS_UM_R ) && ( p->prev_r & 0x7F ) != ( p->exp_r & 0x7F ) ) ok = false;
    if ( !ok ) p->precondition_failed = 1;
    return ok;
}


/**
 * @brief Hodnota proměnné, na kterou je element INI přímo navázaný.
 *
 * Element drží konfiguraci (načtenou při startu nebo zapsanou přes
 * SETTINGS_SET); zařízení pracuje s proměnnou navázanou přes
 * cfgelement_set_handlers(), do elementu se zkopíruje až při ukládání
 * INI. Čte jen číselné vazby (KEYWORD, BOOL, UNSIGNED, FLOAT) bez
 * volání callbacků a bez zápisu do elementu; TEXT a callbacky
 * (cfgelement_set_save_cb()) vynechává, protože mohou patřit stavu
 * jiného vlákna (UI).
 *
 * @param elm Element.
 * @return Text hodnoty (g_strdup, uvolňuje volající přes g_free), NULL
 *         = element přímou číselnou vazbu nemá.
 * @pre Emu vlákno. Čte jedno zarovnané číslo: proměnné zařízení mění
 *      emu vlákno (zásahy UI jdou přes frontu), proměnné zobrazení
 *      (DISPLAY) mění UI vlákno přímo, pak je výsledek stará nebo nová
 *      hodnota.
 */
static char *dbgapi_settings_live_value ( st_CFGELEMENT *elm )
{
    const void *var = elm->save_value_handler;
    if ( !var ) return NULL;
    switch ( elm->type )
    {
        case CFGENTYPE_UNSIGNED:
            return g_strdup_printf ( "%u", *(const unsigned *) var );
        case CFGENTYPE_BOOL:
            return g_strdup ( *(const int *) var ? "true" : "false" );
        case CFGENTYPE_KEYWORD:
        {
            char *kw = cfgelement_get_keyword_by_int ( elm, *(const int *) var );
            return kw ? g_strdup ( kw ) : g_strdup_printf ( "%d", *(const int *) var );
        }
        case CFGENTYPE_FLOAT:
            return g_strdup_printf ( "%g", (double) *(const float *) var );
        default:
            return NULL;
    };
}


/**
 * @brief Najde variantu nebo jednotku periferie podle jména.
 *
 * @param names Pole jmen zakončené NULL (index = hodnota).
 * @param value Jméno z MCP (NULL nebo "" = @p def), velikost písmen se
 *              nerozlišuje.
 * @param def Výchozí index.
 * @return Index v @p names, -1 = neznámé jméno.
 */
static int dbgapi_periph_option ( const char *const *names, const char *value, int def )
{
    if ( !value || !value[ 0 ] ) return def;
    for ( int i = 0; names[ i ]; i++ )
    {
        if ( !g_ascii_strcasecmp ( value, names[ i ] ) ) return i;
    };
    return -1;
}


/**
 * @brief Připojí nebo odpojí periferii za běhu (PERIPH_ATTACH / DETACH).
 *
 * Stejné funkce jako volba v menu Devices: memext_connect() /
 * memext_disconnect(), příznak `g_fdc[].connected` (jako
 * fdc_request_set_connected()), qdisk_apply_connection(),
 * ide8_drive_set_connected(). Prvky INI jsou na tyto proměnné navázané,
 * takže se stav uloží při ukončení jako volba v menu. Stav, který už
 * platí, se znovu neaplikuje (memext_connect() by jinak vynuloval mapu
 * bank, qdisk_apply_connection() znovu otevřel obraz).
 *
 * Varianty (p->option_value, viz st_DBGAPI_PERIPH_PARAM): memext
 * "pehu"/"luftner" (výchozí současný typ), qd "mz1f11"/"virtual"/"unicard"
 * (výchozí "mz1f11"), u odpojení se ignorují; jednotky fdc "fdc0"/"fdc1"
 * a ide8 "master"/"slave" (výchozí první) platí pro obě operace.
 *
 * Zařízení devbus (DBGAPI_PERIPH_KIND_DEVBUS, p->devbus_id): devbus_connect()
 * / devbus_disconnect(); -1 neznámé id, -4 kolize s připojeným zařízením,
 * -5 connect() selhal, -10 devbus na platformě není.
 *
 * @param p Parametr příkazu (IN kind, option_value; OUT out_*).
 * @param attach true = připojit, false = odpojit.
 * @return 0 = stav platí; -1 neplatný druh; -4 neplatná varianta nebo
 *         jednotka; -5 zařízení stav nepřijalo (obraz disku IDE8 nejde
 *         otevřít ani vytvořit, Quick Disk "unicard" bez připojeného
 *         Unicardu); -10 periferie v sestavě není.
 * @pre Emu vlákno, drží device_state_lock().
 * @post p->out_requires_restart = 0; při 0 platí p->out_type (statický
 *       řetězec) a p->out_changed.
 */
/**
 * @brief SETTINGS_SET pro volbu zařízení devbus.
 *
 * Text hodnoty převede devbus_option_parse() (KEYWORD klíčovým slovem,
 * BOOL true/false/0/1, UNSIGNED číslo v rozsahu), nastaví ji
 * devbus_option_set() a zapíše i do elementu INI. out_value = předchozí
 * hodnota v textové podobě, out_applies podle DEVBUS_OPT_APPLY.
 *
 * @param p    Parametr příkazu (IN module, element, new_value; OUT out_*).
 * @param elm  Element INI volby (sekce zařízení).
 * @param slot Slot zařízení (z devbus_option_key()).
 * @param opt  Index volby.
 * @return true = nastaveno (out_result 0); false = neplatná hodnota (-4).
 * @pre Emu vlákno (drain dbgapi).
 */
static bool dbgapi_settings_set_devbus ( st_DBGAPI_SETTINGS_PARAM *p, st_CFGELEMENT *elm,
                                         int slot, unsigned opt )
{
    const st_DEVBUS_OPTION *o = &devbus_desc ( slot )->options[ opt ];
    char buf[ 40 ];
    devbus_option_format ( slot, opt, devbus_option_get ( slot, opt ), buf, sizeof ( buf ) );
    p->out_value = g_strdup ( buf );
    p->out_type = ( o->type == DEVBUS_OPT_BOOL ) ? DBGAPI_SETTINGS_TYPE_BOOL
                : ( o->type == DEVBUS_OPT_KEYWORD ) ? DBGAPI_SETTINGS_TYPE_KEYWORD
                : DBGAPI_SETTINGS_TYPE_UNSIGNED;
    unsigned v = 0;
    if ( !devbus_option_parse ( slot, opt, p->new_value, &v ) ||
         devbus_option_set ( slot, opt, v ) != DEVBUS_OK )
    {
        p->out_result = -4;
        return false;
    };
    if ( o->type == DEVBUS_OPT_BOOL ) cfgelement_set_bool_value ( elm, (int) v );
    else if ( o->type == DEVBUS_OPT_KEYWORD ) cfgelement_set_keyword_value ( elm, (int) v );
    else cfgelement_set_unsigned_value ( elm, v );
    p->out_applies = ( o->apply == DEVBUS_APPLY_LIVE ) ? "now" : "on_connect";
    p->out_result = 0;
    return true;
}


static int dbgapi_periph_apply ( st_DBGAPI_PERIPH_PARAM *p, bool attach )
{
    p->out_requires_restart = 0;
    p->out_changed = 0;
    p->out_type = NULL;

    switch ( p->kind )
    {
        case DBGAPI_PERIPH_KIND_MEMEXT:
        {
            static const char *const names[] = { "pehu", "luftner", NULL };
            static const en_MEMEXT_TYPE types[] = { MEMEXT_TYPE_PEHU, MEMEXT_TYPE_LUFTNER };
            int def = MEMEXT_TEST_TYPE_LUFTNER ? 1 : 0;
            int idx = attach ? dbgapi_periph_option ( names, p->option_value, def ) : def;
            if ( idx < 0 ) return -4;
            if ( attach )
            {
                if ( !MEMEXT_TEST_CONNECTED || g_memext.type != types[ idx ] )
                {
                    memext_connect ( types[ idx ] );
                    p->out_changed = 1;
                };
            }
            else if ( MEMEXT_TEST_CONNECTED )
            {
                memext_disconnect ( );
                p->out_changed = 1;
            };
            p->out_type = names[ idx ];
            return 0;
        }

        case DBGAPI_PERIPH_KIND_FDC:
        {
#if CFG_HWEXT_HAVE_FDC
            static const char *const names[] = { "fdc0", "fdc1", NULL };
            int idx = dbgapi_periph_option ( names, p->option_value, 0 );
            if ( idx < 0 ) return -4;
            unsigned want = attach ? FDC_CONNECTED : FDC_DISCONNECTED;
            st_FDC *fdc = &g_fdc[ idx == 0 ? FDC0 : FDC1 ];
            if ( fdc->connected != want )
            {
                fdc->connected = want;
                p->out_changed = 1;
            };
            p->out_type = names[ idx ];
            return 0;
#else
            return -10;
#endif
        }

        case DBGAPI_PERIPH_KIND_QD:
        {
#if CFG_HWEXT_HAVE_QDISK
            static const char *const names[] = { "mz1f11", "virtual", "unicard", NULL };
            static const en_QDISK_REQ_CONNECTION conns[] = { QDISK_REQ_MZ1F11, QDISK_REQ_VIRTUAL, QDISK_REQ_UNICARD };
            static const int qtypes[] = { QDISK_TYPE_IMAGE, QDISK_TYPE_VIRTUAL, QDISK_TYPE_UNICARD };
            if ( attach )
            {
                int idx = dbgapi_periph_option ( names, p->option_value, 0 );
                if ( idx < 0 ) return -4;
                if ( !QDISK_TEST_CONNECTED || (int) g_qdisk.type != qtypes[ idx ] )
                {
                    if ( qdisk_apply_connection ( conns[ idx ] ) != 0 ) return -5;
                    p->out_changed = 1;
                };
                p->out_type = names[ idx ];
            }
            else
            {
                if ( QDISK_TEST_CONNECTED )
                {
                    if ( qdisk_apply_connection ( QDISK_REQ_DISCONNECTED ) != 0 ) return -5;
                    p->out_changed = 1;
                };
                p->out_type = "disconnected";
            };
            return 0;
#else
            return -10;
#endif
        }

        case DBGAPI_PERIPH_KIND_IDE8:
        {
#if CFG_HWEXT_HAVE_IDE8
            static const char *const names[] = { "master", "slave", NULL };
            static const en_IDE8_DRIVE drives[] = { IDE8_DRIVE_MASTER, IDE8_DRIVE_SLAVE };
            int idx = dbgapi_periph_option ( names, p->option_value, 0 );
            if ( idx < 0 ) return -4;
            st_IDE8_DRIVE *drive = &g_ide8.drive[ drives[ idx ] ];
            en_IDE8_STATE want = attach ? IDE8_STATE_CONNECTED : IDE8_STATE_DISCONNECTED;
            if ( drive->connected != want )
            {
                ide8_drive_set_connected ( drives[ idx ], want );
                /* Připojení otevře obraz disku (chybějící vytvoří, cesta
                 * vůči cfg_dir); když ho otevřít nejde, zůstane odpojeno. */
                if ( drive->connected != want ) return -5;
                p->out_changed = 1;
            };
            p->out_type = names[ idx ];
            return 0;
#else
            return -10;
#endif
        }

        case DBGAPI_PERIPH_KIND_GAL5:
            /* V emulátoru není; druh zůstává kvůli zpětné kompatibilitě. */
            return -10;

        case DBGAPI_PERIPH_KIND_DEVBUS:
        {
            /* Volitelné zařízení devbus (jen MZ-700): id místo statického
             * druhu, varianta se ignoruje. Připojení stejnou cestou jako
             * zásah z menu (devbus_connect() / devbus_disconnect()). */
            if ( !devbus_is_initialized ( ) ) return -10;
            int slot = devbus_find ( p->devbus_id );
            if ( slot < 0 ) return -1;
            if ( attach != devbus_is_connected ( slot ) )
            {
                int rc = attach ? devbus_connect ( slot ) : devbus_disconnect ( slot );
                if ( rc == DEVBUS_ERR_CONFLICT ) return -4;
                if ( rc != DEVBUS_OK ) return -5;
                p->out_changed = 1;
            };
            p->out_type = devbus_desc ( slot )->id;
            return 0;
        }

        default:
            return -1;
    };
}


/**
 * @brief true = příkaz mění pásku kazety nebo ROM a GUI ji čte pod zámkem
 *        stavu zařízení.
 *
 * Otevření a vysunutí pásky uvolňuje kontejner a bloky, které okna kazety
 * v UI vlákně čtou pod zámkem stavu zařízení (device_request.h); stejně
 * transport, výběr bloku, rychlost a CMT hack (přepis ROM, stav pásky).
 *
 * @param cmd Příkaz bez příznaku BLOCKING.
 * @return true = dispatch ho provede pod device_state_lock().
 */
static bool dbgapi_cmd_needs_device_lock(en_DBGAPI_CMD cmd)
{
    switch (cmd)
    {
        case DBGAPI_CMD_CMT_TRANSPORT:
        case DBGAPI_CMD_CMT_RECORD:
        case DBGAPI_CMD_CMT_HACK_SET:
        case DBGAPI_CMD_CMT_SET_PROPERTY:
        case DBGAPI_CMD_CMT_OPEN:
        case DBGAPI_CMD_CMT_TAPE_SEEK:
        case DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED:
            return true;
        default:
            return false;
    };
}


void dbgapi_emu_dispatch(st_DBGAPI_CMDRQ *rq)
{
    if (!rq)
        return;

    /* Extrahovat příkaz bez BLOCKING flagu */
    en_DBGAPI_CMD cmd = (en_DBGAPI_CMD)(rq->cmd & DBGAPI_CMD_MASK);

    /* Testovací háček (regresní test omezené odpovědi MCP). */
    dbgapi_test_apply_dispatch_stall(rq, cmd);

    /* Stav před příkazem pro záznam, kdo emulaci rozběhl / pozastavil
     * (run_control_after_cmd() za switchem). */
    const bool rc_was_paused = EMULATOR_TEST_PAUSED ? true : false;
    const bool rc_was_step = TEST_DEBUGGER_STEP_CALL ? true : false;

    /* Podmínka revize stavu (st_DBGAPI_SUBMIT_OPTS, state_rev.h): před
     * bránou záznamu vstupů, aby nesplněná podmínka nic nezaznamenala.
     * EXPECT_EQUAL za běhu nikdy neplatí (stav se mění bez zvednutí). */
    st_DBGAPI_SUBMIT_OPTS *opts = rq->opts;
    bool rev_failed = false;
    bool rev_skipped = false;
    if (opts)
    {
        opts->rev_failed = false;
        opts->rev_skipped = false;
        if (opts->rev_cond == DBGAPI_REV_COND_EXPECT_EQUAL)
        {
            rev_failed = !EMULATOR_TEST_PAUSED || g_state_rev != opts->rev_value;
        }
        else if (opts->rev_cond == DBGAPI_REV_COND_SKIP_IF_EQUAL)
        {
            rev_skipped = EMULATOR_TEST_PAUSED && g_state_rev == opts->rev_value;
        };
        opts->rev_failed = rev_failed;
        opts->rev_skipped = rev_skipped;
    };
    const bool rev_stop = rev_failed || rev_skipped;

    /* Zdroj zvednutí revize: vše uvnitř příkazu MCP klienta (i reset,
     * snapshot) je MCP (událost state_changed ho neohlašuje). */
    const uint32_t rev_src_prev = g_state_rev_src_override;
    if (rq->cmd_origin == DBGAPI_CMD_ORIGIN_MCP) g_state_rev_src_override = STATE_REV_SRC_MCP;

    /* Záznam vstupů (replay.h): při záznamu odmítne zásah, který záznam
     * neumí zopakovat, a zásah v pauze mimo hranici snímku. Bez záznamu
     * jedno čtení. */
    const bool rp_refused = !rev_stop && (g_replay_mode != REPLAY_MODE_OFF) && !replay_dbg_before(rq);

    /*
     * Dispatch příkazů — zatím základní implementace.
     * Konkrétní handlery budou doplněny až budou k dispozici
     * příslušné moduly emulátoru (debugger.h, bptmap.h, cpu-z80, ...).
     *
     * TODO: doplnit handlery pro jednotlivé příkazy
     */
    /* Příkazy kazety pod zámkem stavu zařízení (dbgapi_cmd_needs_device_lock()). */
    const bool dev_locked = !rev_stop && !rp_refused && dbgapi_cmd_needs_device_lock(cmd);
    if (dev_locked) device_state_lock();

    if (rev_stop)
    {
        /* Podmínka revize: nesplněná = chyba, přeskočení = úspěch bez práce. */
        rq->success = rev_skipped;
    }
    else if (rp_refused)
    {
        rq->success = false;
    }
    else switch (cmd)
    {
        case DBGAPI_CMD_NONE:
            /* Ping — bez efektu */
            rq->success = true;
            break;

        case DBGAPI_CMD_IS_DEBUGGER_ACTIVE:
            /* Vrátí stav g_debugger.active (= debug okno otevřené)
             * jako bool přes result_ptr. */
            if (rq->result_ptr)
            {
                *((bool *)rq->result_ptr) = TEST_DEBUGGER_ACTIVE;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_DEBUGGER_ACTIVATE:
            /* Aktivuje debugger (= nastaví g_debugger.active = 1) a na emu
             * vlákně přepočítá CPU callbacky a active flagy trace-suite
             * (stejně jako DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE).
             * Side effect: TEST_DEBUGGER_CPUHIST_ACTIVE / MHMAP_ACTIVE
             * v režimu WITH_WINDOW se zapne, takže CPU instrukční historie
             * a memory heatmap začnou zaznamenávat. Bez přepočtu by CPU
             * zůstalo na rychlých callbackách a záznam by neběžel. Okno
             * debuggeru se tím neotevírá. */
            g_debugger.active = 1;
            mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
            rq->success = true;
            break;

        case DBGAPI_CMD_DEBUGGER_DEACTIVATE:
            /* Deaktivuje debugger (= g_debugger.active = 0) a přepočítá
             * CPU callbacky (jinak by zůstaly pomalé logging callbacky).
             * Side effect: cpuhist a mhmap recording v WITH_WINDOW režimu
             * se vypne. Otevřené okno debuggeru se nezavírá. */
            g_debugger.active = 0;
            mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
            rq->success = true;
            break;

        case DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE:
            /* Přepočet debugger callbacků + trace/CDL/cpuhist active flagů na
             * EMU vlákně (per-frame safe-point v drain smyčce, mimo per-instrukční
             * hot-path append). UI vlákno si předem nastavilo mode/flagy (atomický
             * int zápis) a deleguje sem samotný recompute. Tím se trace start/stop
             * (vč. alloc/free writer bufferu v tlog_writer_*) provede na emu vlákně
             * a nevznikne use-after-free race s emu-thread tlog_writer_append.
             * Volá tutéž funkci jako přímá UI cesta dříve - chování beze změny,
             * jen jiné vlákno. */
            mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
            rq->success = true;
            break;

        case DBGAPI_CMD_PAUSE:
        {
            /* Pozastaví emulaci. Forward na emulator_pause(true).
             * Side effecty (z emulator.c:273): MZ800_MAIN_SET_EVENT
             * BREAK_EMULATION_PAUSED, audio pause, UI state update,
             * pokud TEST_DEBUGGER_ACTIVE pak hide spinner + reset
             * temporary BP.
             *
             * data_ptr (volitelný st_DBGAPI_PAUSE_PARAM*): s if_run_seq
             * zastaví jen běžící běh s tímto číslem (run_control).
             * Událost "paused" vyšle záznam zastavení na vstupu do pauzy
             * (run_control_on_stop()), jen když emulace opravdu stála
             * (pauza už pozastavené emulace žádné zastavení není). */
            st_DBGAPI_PAUSE_PARAM *pp = (st_DBGAPI_PAUSE_PARAM *) rq->data_ptr;
            bool running_now = !EMULATOR_TEST_PAUSED;
            bool do_pause = true;
            if ( pp )
            {
                pp->was_running = running_now;
                pp->run_seq = run_log_current ( NULL );
                pp->paused_now = false;
                if ( pp->has_if_run_seq
                     && ( !running_now || pp->run_seq != pp->if_run_seq ) )
                {
                    do_pause = false;
                };
            };
            if ( do_pause )
            {
                if ( running_now )
                {
                    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
                };
                emulator_pause ( true );
                if ( pp ) pp->paused_now = running_now;
            };
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_FORCE_PAUSE:
            /* Vynutí pauzu emulace. V současné implementaci shodné
             * s CMD_PAUSE - emulator_pause(true) nemá konkurenční
             * cestu která by ho mohla "přeskočit" (= žádný BP context
             * který by zámik bránil v aplikaci pauzy). Pokud v budoucnu
             * vznikne nepřeskočitelný kontext, FORCE_PAUSE bude bypass. */
            g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
            emulator_pause ( true );
            rq->success = true;
            break;

        case DBGAPI_CMD_RUN:
            /* Spustí emulaci. Forward na emulator_pause(false).
             * Side effecty (z emulator.c:273): audio resume, UI state
             * update, pokud TEST_DEBUGGER_ACTIVE pak show spinner +
             * window focus. Také vynuluje run_to_temporary_breakpoint
             * flag.
             *
             * result_ptr (volitelný st_DBGAPI_RUN_START_RESULT*): číslo
             * spuštěného (nebo už běžícího) běhu. */
            if ( rq->result_ptr )
            {
                st_DBGAPI_RUN_START_RESULT *r = (st_DBGAPI_RUN_START_RESULT *) rq->result_ptr;
                r->status = EMULATOR_TEST_PAUSED ? DBGAPI_RUN_TO_STARTED : DBGAPI_RUN_START_NOT_PAUSED;
                r->step = false;
                r->run_seq = run_control_seq_for_start ( );
            };
            emulator_pause ( false );
            rq->success = true;
            break;

        case DBGAPI_CMD_RUN_FRAMES:
            /* Frame-bounded run (mcp-debug-control request 0021):
             * deterministický stop emulace přesně na N-té frame hranici.
             *
             * data_ptr je int* = počet framů N (>= 1). Nastavíme cílovou
             * hodnotu run_frames_target = aktuální screens + N a aktivujeme
             * run_frames_active. Hot loop v mzarch_main (per-frame blok) po
             * dokončení každého framu zkontroluje screens >= target a sám
             * zavolá emulator_pause(true). Tím se emu zastaví na deterministické
             * frame hranici (= bezprostředně po inkrementu screens), nezávisle
             * na wall-clock-závislém timingu dispatch vlákna.
             *
             * Side effecty: emulator_pause(false) unpausne emulaci (audio
             * resume, UI state update, reset run_to_temporary_breakpoint flag).
             *
             * Pozn.: target používá modulární uint32_t aritmetiku stejně jako
             * porovnání >= v hot loopu; přetečení screens (po cca 2.7 roku
             * běhu při 50 Hz) by teoreticky zkreslilo cíl, ale frame-bounded
             * run je krátkodobá operace (N <= 1000), takže to není praktický
             * problém.
             *
             * result_ptr (volitelný, uint32_t*): handler do něj zapíše
             * výchozí hodnotu screens, od které se cíl počítá. Volající
             * (MCP run) z ní počítá actual_frames. Výchozí hodnotu nesmí
             * číst sám na svém vlákně před submitem: za běhu emulace se
             * mezi jeho čtením a tímto drainem (až po uzavření snímku
             * a inkrementu screens) dokončí ještě jeden snímek a delta by
             * vyšla N+1. */
            if ( !rq->data_ptr )
            {
                rq->success = false;
                break;
            };
            {
                int frames = *((int *)rq->data_ptr);
                if ( frames < 1 )
                {
                    rq->success = false;
                    break;
                };
                uint32_t start_screens = (uint32_t) g_gdg.total_elapsed.screens;
                g_debugger.run_frames_target = start_screens + (uint32_t) frames;
                g_debugger.run_frames_active = 1;
                if ( rq->result_ptr )
                {
                    *((uint32_t *)rq->result_ptr) = start_screens;
                };
            }
            emulator_pause ( false );
            rq->success = true;
            break;

        case DBGAPI_CMD_IS_RUNNING:
            /* Vrátí stav emulace (= negace EMULATOR_TEST_PAUSED) jako
             * bool přes result_ptr. */
            if (rq->result_ptr)
            {
                *((bool *)rq->result_ptr) = !EMULATOR_TEST_PAUSED;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STEP_INTO:
            /* Step Into - jeden krok přes aktuální instrukci.
             * Forward na debugger_step_call(1). Pokud emulace běží,
             * debugger_step_call() ji nejprve pozastaví a step se
             * neprovede (= musí volat client znovu po pause).
             * Side effecty: g_debugger.step_call = 1 → hot loop
             * mzarch.c:758 detekuje TEST_DEBUGGER_STEP_CALL po jedné
             * instrukci a opět zastaví. */
            if ( rq->result_ptr )
            {
                /* Za běhu debugger_step_call() nic neudělá (ani nepauzne). */
                st_DBGAPI_RUN_START_RESULT *r = (st_DBGAPI_RUN_START_RESULT *) rq->result_ptr;
                r->status = EMULATOR_TEST_PAUSED ? DBGAPI_RUN_TO_STARTED : DBGAPI_RUN_START_NOT_PAUSED;
                r->step = EMULATOR_TEST_PAUSED ? true : false;
                r->run_seq = run_control_seq_for_start ( );
            };
            debugger_step_call ( 1 );
            rq->success = true;
            break;

        case DBGAPI_CMD_STEP_OVER:
            /* Step Over - CALL/RST/DJNZ/blokové instrukce: nastaví temp BP
             * na addr+length a spustí run-to-temp-BP. Ostatní: step into.
             * Interní helper dbgapi_emu_do_step_over() (= replika logiky
             * z dbg_iconbar.cpp). Pokud emu běží, caller musí pause před
             * voláním (default UX).
             *
             * result_ptr (volitelný st_DBGAPI_RUN_START_RESULT*): krok
             * (step = true), běh k dočasnému BP (step = false), nebo jen
             * pauza (PAUSED_ONLY), a číslo běhu. */
            if ( EMULATOR_TEST_PAUSED )
            {
                uint64_t seq = run_control_seq_for_start ( );
                dbgapi_emu_do_step_over ( );
                if ( rq->result_ptr )
                {
                    st_DBGAPI_RUN_START_RESULT *r = (st_DBGAPI_RUN_START_RESULT *) rq->result_ptr;
                    r->status = DBGAPI_RUN_TO_STARTED;
                    r->step = TEST_DEBUGGER_STEP_CALL ? true : false;
                    r->run_seq = seq;
                };
            }
            else
            {
                if ( rq->result_ptr )
                {
                    st_DBGAPI_RUN_START_RESULT *r = (st_DBGAPI_RUN_START_RESULT *) rq->result_ptr;
                    r->status = DBGAPI_RUN_TO_PAUSED_ONLY;
                    r->step = false;
                    r->run_seq = run_log_current ( NULL );
                };
                emulator_pause ( true );
            };
            rq->success = true;
            break;

        case DBGAPI_CMD_RUN_TO:
            /* Run To Cursor / Run To Address: nastaví dočasný BP na
             * cílovou adresu a spustí emulaci přes
             * mzarch_run_to_temporary_breakpoint(). Cílová adresa v
             * data_ptr (= uint16_t*). Pokud emu běží, pause + return
             * (= UX z dbg_iconbar.cpp::dbg_do_run_to_cursor).
             *
             * result_ptr (volitelný st_DBGAPI_RUN_START_RESULT*,
             * mcp-inbox-fixes): co se stalo - DBGAPI_RUN_TO_STARTED, nebo
             * DBGAPI_RUN_TO_PAUSED_ONLY (emu běžel, jen se pauzl), a číslo
             * běhu. MCP podle něj pozná, že cíl nespustil, i když emu mezi
             * jeho kontrolou pauzy a drainem někdo rozběhl. */
            if ( !rq->data_ptr )
            {
                rq->success = false;
                break;
            };
            if ( !EMULATOR_TEST_PAUSED )
            {
                if ( rq->result_ptr )
                {
                    st_DBGAPI_RUN_START_RESULT *r = (st_DBGAPI_RUN_START_RESULT *) rq->result_ptr;
                    r->status = DBGAPI_RUN_TO_PAUSED_ONLY;
                    r->step = false;
                    r->run_seq = run_log_current ( NULL );
                };
                emulator_pause ( true );
                rq->success = true;
                break;
            };
            if ( rq->result_ptr )
            {
                st_DBGAPI_RUN_START_RESULT *r = (st_DBGAPI_RUN_START_RESULT *) rq->result_ptr;
                r->status = DBGAPI_RUN_TO_STARTED;
                r->step = false;
                r->run_seq = run_control_seq_for_start ( );
            };
            {
                uint16_t target = *((uint16_t *)rq->data_ptr);
                bptmap_set_temporary_event ( target );
                debugger_step_call ( 0 );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
                mzarch_run_to_temporary_breakpoint ( );
#endif
            }
            rq->success = true;
            break;

        case DBGAPI_CMD_RESET:
            /* Reset emulátoru - asynchronní přes
             * mzarch_platform_fn_reset_request(). Reset je proveden
             * v mzarch_main loopu při příští iteraci (= zámek
             * reset_request_mutex + flag). Side effecty: gdg_reset,
             * memory_reset, z80_reset, periferie reset, debugger
             * update. Z paused stavu je reset detekován v pause
             * loop okamžitě a zachová pause.
             *
             * V pauze bez záznamu vstupů se reset provede hned v příkazu
             * (emu vlákno, totéž co paused smyčka): výstup slotu i odpověď
             * MCP pak nesou revizi stavu po resetu (state_rev.h). Jinak
             * (za běhu, při záznamu, který reset odkládá na hranici
             * snímku) jen požadavek; zdroj pro revizi převezme
             * mzarch_main_reset(). */
            if ( EMULATOR_TEST_PAUSED && ( g_replay_mode == REPLAY_MODE_OFF ) )
            {
                mzarch_main_reset_now ( );
            }
            else
            {
                g_state_rev_reset_src = g_state_rev_src_override;
                mzarch_platform_fn_reset_request ( );
            };
            rq->success = true;
            break;

        case DBGAPI_CMD_GET_REG:
            /* Čtení 16bitové hodnoty Z80 registru přes z80_get_reg().
             * data_ptr: uint8_t* - reg_id (= z80_reg_t casted na uint8_t,
             * 0..13 dle z80.h:248-263, viz Z80_REG_AF...Z80_REG_IR).
             * result_ptr: uint16_t* - výstup hodnoty. Pro IR registr
             * vrací 16bitové (I << 8) | R kompozit. */
            if (rq->data_ptr && rq->result_ptr)
            {
                uint8_t reg_id = *((uint8_t *)rq->data_ptr);
                if (reg_id <= (uint8_t)Z80_REG_IR)
                {
                    *((uint16_t *)rq->result_ptr) =
                        z80_get_reg ( g_mzarch_main.cpu, (z80_reg_t)reg_id );
                    rq->success = true;
                }
                else
                {
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_SET_REG:
            /* Zápis 16bitové hodnoty do Z80 registru. data_ptr:
             * st_DBGAPI_REG_PARAM* (= reg_id + value). IR zapíše celý pár
             * I:R včetně bitu 7 R (jako LD I,A + LD R,A), aby šel přes MCP
             * věrně obnovit stav CPU; okno CPU edituje I a R zvlášť přes
             * SET_CPU_FLAGS (UM_I, UM_R). Forward přes přímý
             * z80_set_reg() bez pause check (= debugger_change funkce
             * dělá pause check pro UI vlákno; dbgapi je už v emu vlákně).
             *
             * (hypotéza) Volání z emu vlákna mimo z80_step() je bezpečné
             * (= debugger.c:582 to také dělá z UI vlákna pres pause). */
            if (rq->data_ptr)
            {
                st_DBGAPI_REG_PARAM *p = (st_DBGAPI_REG_PARAM *)rq->data_ptr;
                if (p->reg_id <= (uint8_t)Z80_REG_IR)
                {
                    /* Podmíněný zápis (observe-cas M10): stará hodnota
                     * vždy do previous, při neshodě s expected nic
                     * nezapsat a skončit neúspěchem (replay ho tak
                     * nezaznamená, Activity ho ukáže jako odmítnutý). */
                    p->previous = z80_get_reg ( g_mzarch_main.cpu, (z80_reg_t)p->reg_id );
                    p->precondition_failed = 0;
                    if (p->has_expected && p->previous != p->expected)
                    {
                        p->precondition_failed = 1;
                        rq->success = false;
                        break;
                    };
                    if ((z80_reg_t)p->reg_id == Z80_REG_IR)
                    {
                        /* Celý pár I:R (I = horní bajt, R = dolní bajt). */
                        z80_set_reg ( g_mzarch_main.cpu, Z80_REG_IR, p->value );
                    }
                    else
                    {
                        /* BUG2 fix: zapamatovat PC před zápisem, abychom
                         * HALT zrušili jen při SKUTEČNÉ změně PC (viz níže). */
                        uint16_t pc_before = g_mzarch_main.cpu->pc;
                        z80_set_reg ( g_mzarch_main.cpu,
                                      (z80_reg_t)p->reg_id, p->value );
                        /* BUG2 fix: ruční zápis PC z debuggeru musí
                         * probudit CPU z HALT. z80_set_reg() nastaví jen
                         * cpu->pc, halt latch (cpu->halted) nečistí. Bez
                         * vyčištění narazí následující STEP na early-exit
                         * pro halted ve z80_execute() (z80.c) a nevykoná
                         * žádnou instrukci - CPU zůstane "zamčené" v HALT
                         * dokud nepřijde IRQ/NMI. Latch proto rušíme zde
                         * (jen pro zápis PC) a ohlásíme HALT_EXIT kvůli
                         * konzistenci callstack/profiler/eventlog. Interní
                         * cpu->pc je už nastaven na novou adresu, kterou
                         * předáme i jako adresu události.
                         *
                         * Podmínka pc != pc_before: zápis STEJNÉ hodnoty PC
                         * (uživatel jen "potvrdil" regPC bez změny) nesmí
                         * HALT ukončit - CPU má zůstat ve stejném stavu. */
                        if ( (z80_reg_t)p->reg_id == Z80_REG_PC &&
                             g_mzarch_main.cpu->halted &&
                             g_mzarch_main.cpu->pc != pc_before )
                        {
                            g_mzarch_main.cpu->halted = false;
                            if ( g_mzarch_main.cpu->cpu_ctrl_event_cb )
                            {
                                g_mzarch_main.cpu->cpu_ctrl_event_cb (
                                    g_mzarch_main.cpu,
                                    (uint8_t)Z80_CPU_CTRL_HALT_EXIT,
                                    g_mzarch_main.cpu->pc,
                                    g_mzarch_main.cpu->cpu_ctrl_event_data );
                            };
                        };
                    };
                    /* Pokrytí kódu: změna PC nebo AF hostitelem mezi
                     * instrukcemi mění tok programu mimo něj. */
                    if ( ( (z80_reg_t)p->reg_id == Z80_REG_PC || (z80_reg_t)p->reg_id == Z80_REG_AF )
                         && p->value != p->previous )
                    {
                        coverage_note_host_state_edit ( );
                    };
                    rq->success = true;
                }
                else
                {
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_ALL_REGS:
            /* Dump všech Z80 registrů do uint16_t[DBGAPI_REG_COUNT] pole.
             * Pořadí dle z80_reg_t enum (Z80_REG_AF..Z80_REG_IR). Caller
             * alokuje pole; musí mít kapacitu DBGAPI_REG_COUNT * 2 byte. */
            if (rq->result_ptr)
            {
                uint16_t *out = (uint16_t *)rq->result_ptr;
                for (int i = 0; i < DBGAPI_REG_COUNT; i++)
                {
                    out[i] = z80_get_reg ( g_mzarch_main.cpu, (z80_reg_t)i );
                };
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_MEM_READ:
            /* Čtení bloku paměti respektujícího banking. data_ptr:
             * st_DBGAPI_MEM_PARAM* (= addr + len + buf). Buffer
             * vlastní caller, dispatch ho jen vyplní. Forward přes
             * debugger_memory_read_byte() (= memory_read_byte_no_se:
             * banking, bez MEM_R breakpointů). */
            if (rq->data_ptr)
            {
                st_DBGAPI_MEM_PARAM *p = (st_DBGAPI_MEM_PARAM *)rq->data_ptr;
                if (p->buf)
                {
                    for (uint32_t i = 0; i < p->len; i++)
                    {
                        p->buf[i] = debugger_memory_read_byte (
                            (uint16_t)(p->addr + i) );
                    };
                    rq->success = true;
                }
                else
                {
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_MEM_WRITE:
            /* Zápis bloku paměti respektujícího banking. data_ptr:
             * st_DBGAPI_MEM_PARAM* (= addr + len + buf). buf vlastní
             * caller, dispatch ho jen čte. Forward přes
             * debugger_memory_write_byte() (= memory_write_byte přes
             * banking + g_debugger.memop_call flag pro CDL recording). */
            if (rq->data_ptr)
            {
                st_DBGAPI_MEM_PARAM *p = (st_DBGAPI_MEM_PARAM *)rq->data_ptr;
                if (p->buf)
                {
                    for (uint32_t i = 0; i < p->len; i++)
                    {
                        debugger_memory_write_byte (
                            (uint16_t)(p->addr + i), p->buf[i] );
                    };
                    rq->success = true;
                }
                else
                {
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_BP_ADD:
            /* Přidá execution BP na adrese. data_ptr: st_DBGAPI_BP_PARAM*
             * (= addr + id; id se ignoruje - generuje breakpoints_add_auto).
             * Po návratu je p->id naplněno přiděleným ID (= caller ho
             * potřebuje pro REMOVE). breakpoints_add_auto vrátí -1 při
             * fatální chybě (přetečení ID), jinak vždy přidělí ID a v
             * případě konfliktu na adrese nastaví enabled=false.
             *
             * V1.C.3: po úspěšném add propagujeme rq->cmd_origin do
             * vytvořeného BP (= owner attribution pro GUI badge). */
            if (rq->data_ptr)
            {
                st_DBGAPI_BP_PARAM *p = (st_DBGAPI_BP_PARAM *)rq->data_ptr;
                int new_id = breakpoints_add_auto ( p->addr, NULL, -1 );
                if (new_id > 0)
                {
                    p->id = new_id;
                    /* Propaguj origin do nově vytvořeného BP. find_by_id()
                     * je v breakpoints.c static - použijeme lineární scan
                     * přes breakpoints_get_count() + breakpoints_get_by_index(). */
                    int total = (int)g_breakpoints.breakpoints->len;
                    for (int i = 0; i < total; i++)
                    {
                        st_BPT *bp = &g_array_index(g_breakpoints.breakpoints,
                                                     st_BPT, i);
                        if (bp->id == new_id)
                        {
                            bp->cmd_origin = rq->cmd_origin;
                            break;
                        }
                    }
                    rq->success = true;
                }
                else
                {
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
            dbgapi_bp_recompute_cb_gating ( );  /* historie: PC_EXEC BP -> cpuhist */
            break;

        case DBGAPI_CMD_BP_REMOVE:
            /* Odstraní BP podle ID. data_ptr: st_DBGAPI_BP_PARAM*
             * (= id; addr se ignoruje). breakpoints_remove() vrací
             * true při úspěchu, false pokud ID neexistuje. */
            if (rq->data_ptr)
            {
                st_DBGAPI_BP_PARAM *p = (st_DBGAPI_BP_PARAM *)rq->data_ptr;
                rq->success = breakpoints_remove ( p->id );
            }
            else
            {
                rq->success = false;
            };
            dbgapi_bp_recompute_cb_gating ( );  /* Fix C */
            break;

        case DBGAPI_CMD_BP_LIST:
            /* Vrátí seznam BP do result_ptr (st_DBGAPI_BP_LIST_RESULT*).
             * Caller alokuje strukturu s flexibilním polem bp[max_count]
             * a nastaví max_count. Dispatch naplní count a bp[] až do
             * max_count (= overflow ořízne, ale úspěch). */
            if (rq->result_ptr)
            {
                st_DBGAPI_BP_LIST_RESULT *r =
                    (st_DBGAPI_BP_LIST_RESULT *)rq->result_ptr;
                int total = (int)g_breakpoints.breakpoints->len;
                int n = ( total < r->max_count ) ? total : r->max_count;
                for (int i = 0; i < n; i++)
                {
                    st_BPT *b = &g_array_index ( g_breakpoints.breakpoints,
                                                 st_BPT, i );
                    r->bp[i].addr = b->addr;
                    /* Match mode + rozsah/maska: bez nich nebylo z bp_list
                     * poznat, že BP s addr_end, ale v režimu SINGLE hlídá
                     * jen addr. */
                    r->bp[i].addr_end = b->addr_end;
                    r->bp[i].addr_match_mode = (uint8_t)b->addr_match_mode;
                    r->bp[i].addr_mask = b->addr_mask;
                    r->bp[i].id = b->id;
                    r->bp[i].enabled = b->enabled;
                    r->bp[i].type = (uint8_t)b->type;
                    r->bp[i].zone = (uint8_t)b->zone;
                    r->bp[i].bank_id = b->bank_id;
                    r->bp[i].bank_id_end = b->bank_id_end;
                    r->bp[i].bank_match_mode = (uint8_t)b->bank_match_mode;
                    r->bp[i].bank_id_mask = b->bank_id_mask;
                    r->bp[i].hits = b->hits;
                    /* expr = condition výraz (NULL = unconditional). g_strdup
                     * - dispatch handler je povinen uvolnit g_free(). */
                    r->bp[i].condition =
                        ( b->expr && b->expr[0] != '\0' )
                        ? g_strdup ( b->expr ) : NULL;
                };
                r->count = n;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_BP_UPDATE:
            /* Selektivní update existujícího BP. data_ptr:
             * st_DBGAPI_BP_UPDATE_PARAM*. Handler iteruje bity update_mask
             * a volá odpovídající breakpoints_set_*() setter. Setteři
             * vracejí false jen pokud BP neexistuje - po prvním ověření
             * existence je každý setter úspěšný (= validace rozsahů je
             * v setteru, špatná hodnota se uloží ale parsed cache zůstane
             * NULL pro expr/action - identické s dnešním přímým voláním
             * z UI). success = false jen pokud BP s id neexistuje (=
             * žádná změna neaplikována) nebo pokud update_mask požaduje
             * neznámou enum hodnotu. update_mask == 0 = no-op success. */
            if (rq->data_ptr)
            {
                rq->success =
                    dbgapi_emu_bp_apply_update(
                        (st_DBGAPI_BP_UPDATE_PARAM *)rq->data_ptr,
                        /* allow_create */ false );
            }
            else
            {
                rq->success = false;
            };
            dbgapi_bp_recompute_cb_gating ( );  /* Fix C */
            break;

        case DBGAPI_CMD_BP_SET_ENABLED:
            /* Quick toggle. Forwarder na breakpoints_set_enabled(). */
            if (rq->data_ptr)
            {
                st_DBGAPI_BP_SET_ENABLED_PARAM *p =
                    (st_DBGAPI_BP_SET_ENABLED_PARAM *)rq->data_ptr;
                rq->success = breakpoints_set_enabled ( p->id, p->enabled );
            }
            else
            {
                rq->success = false;
            };
            dbgapi_bp_recompute_cb_gating ( );  /* Fix C */
            break;

        case DBGAPI_CMD_BP_SET_PARENT:
            /* Quick reparent. Forwarder na breakpoints_set_parent(). */
            if (rq->data_ptr)
            {
                st_DBGAPI_BP_SET_PARENT_PARAM *p =
                    (st_DBGAPI_BP_SET_PARENT_PARAM *)rq->data_ptr;
                rq->success = breakpoints_set_parent ( p->id, p->parent_id );
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_BP_CREATE_WITH_INIT:
            /* Atomický create + init. Caller předá st_DBGAPI_BP_UPDATE_PARAM
             * s id=-1. Handler vola breakpoints_add_auto(addr, name, parent)
             * - addr se bere z payload (UM_ADDR by neměl být v mask = handler
             * stejně bere addr přímo, ale pokud user CHCE addr pak UM_ADDR
             * se aplikuje znovu = idempotent). parent se vezme z UM_PARENT
             * pokud nastaveno, jinak default -1. Po úspěchu naplní p->id +
             * aplikuje zbytek update_mask. Pokud add selže, p->id zůstane -1
             * a success = false. */
            if (rq->data_ptr)
            {
                rq->success =
                    dbgapi_emu_bp_apply_update(
                        (st_DBGAPI_BP_UPDATE_PARAM *)rq->data_ptr,
                        /* allow_create */ true );
            }
            else
            {
                rq->success = false;
            };
            dbgapi_bp_recompute_cb_gating ( );  /* Fix C */
            break;

        case DBGAPI_CMD_BPGRP_ADD:
            /* Přidá novou skupinu. Forwarder na breakpoints_group_add(name,
             * parent). Po úspěchu naplní p->id, jinak p->id zůstane -1 a
             * success = false. */
            if (rq->data_ptr)
            {
                st_DBGAPI_BPGRP_ADD_PARAM *p =
                    (st_DBGAPI_BPGRP_ADD_PARAM *)rq->data_ptr;
                int new_id = breakpoints_group_add ( p->name, p->parent );
                if ( new_id >= 0 )
                {
                    p->id = new_id;
                    rq->success = true;
                }
                else
                {
                    p->id = -1;
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_BPGRP_REMOVE:
            /* Odstraní skupinu podle ID. Forwarder na breakpoints_group_remove.
             * Backendová logika hendluje sirotky (děti přesměrovává nebo
             * mazat, viz breakpoints.c). */
            if (rq->data_ptr)
            {
                st_DBGAPI_BPGRP_REMOVE_PARAM *p =
                    (st_DBGAPI_BPGRP_REMOVE_PARAM *)rq->data_ptr;
                rq->success = breakpoints_group_remove ( p->id );
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_BPGRP_UPDATE:
            /* Selektivní update existující skupiny. Handler iteruje
             * update_mask bity + volá existující breakpoints_group_set_*().
             * Pre-check existence skupiny - pokud find_by_id == NULL,
             * vrátí false bez aplikace. update_mask == 0 = no-op success. */
            if (rq->data_ptr)
            {
                st_DBGAPI_BPGRP_UPDATE_PARAM *p =
                    (st_DBGAPI_BPGRP_UPDATE_PARAM *)rq->data_ptr;
                if ( !breakpoints_group_find_by_id ( p->id ) )
                {
                    rq->success = false;
                }
                else
                {
                    uint64_t mask = p->update_mask;
                    bool ok = true;
                    if ( mask & DBGAPI_BPGRP_UM_ENABLED )
                        ok &= breakpoints_group_set_enabled ( p->id, p->enabled );
                    if ( mask & DBGAPI_BPGRP_UM_NAME )
                        ok &= breakpoints_group_set_name ( p->id, p->name );
                    if ( mask & DBGAPI_BPGRP_UM_COLORS )
                        ok &= breakpoints_group_set_colors ( p->id, p->bg_rgb, p->fg_rgb );
                    if ( mask & DBGAPI_BPGRP_UM_PARENT )
                        ok &= breakpoints_group_set_parent ( p->id, p->parent );
                    rq->success = ok;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_DASM:
            /* Disassembluje N po sobě jdoucích instrukcí od adresy.
             * data_ptr: st_DBGAPI_DASM_PARAM* (= addr + count).
             * result_ptr: st_DBGAPI_DASM_RESULT[count] - caller alokuje
             * pole o velikosti count. Pro každou instrukci vyplní:
             *   .addr (start), .bytes[4], .num_bytes (1-4), .mnemonic.
             * Forward přes z80_dasm() + debugger_dasm_read_cb (= banking
             * aware, no side effects). */
            if (rq->data_ptr && rq->result_ptr)
            {
                st_DBGAPI_DASM_PARAM *p = (st_DBGAPI_DASM_PARAM *)rq->data_ptr;
                st_DBGAPI_DASM_RESULT *out =
                    (st_DBGAPI_DASM_RESULT *)rq->result_ptr;
                uint16_t cur_addr = p->addr;
                for (int i = 0; i < p->count; i++)
                {
                    z80_dasm_inst_t inst;
                    int len = z80_dasm ( &inst, debugger_dasm_read_cb,
                                         NULL, cur_addr );
                    out[i].addr = cur_addr;
                    out[i].num_bytes = len;
                    for (int b = 0; b < 4; b++)
                    {
                        out[i].bytes[b] = inst.bytes[b];
                    };
                    char buf[ sizeof(out[i].mnemonic) ];
                    z80_dasm_to_str ( buf, (int)sizeof(buf), &inst, NULL );
                    /* z80_dasm_to_str zapíše i adresu/bajty - chceme jen
                     * mnemonic. Použijeme inst.mnemonic + operandy přes
                     * to_str se default formátem.
                     * (hypotéza) Pro V1 nám stačí default to_str výstup;
                     * konkrétní layout (s/bez adresy) může caller post-
                     * processovat. Dnešní UI debugger používá vlastní
                     * formátování v dbg_disassembled.cpp. */
                    g_strlcpy ( out[i].mnemonic, buf,
                                sizeof(out[i].mnemonic) );
                    cur_addr = (uint16_t)( cur_addr + len );
                };
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_HISTORY_GET:
            /* Snímá obsah debug history ring bufferu (= 32 posledních
             * dokončených instrukcí, pole g_debugger_history.row[]).
             * result_ptr je buffer pro st_DEBUGGER_HISTORY_ROW pole
             * o velikosti DEBUGGER_HISTORY_LENGTH (= 32 položek). Caller
             * si pole alokuje sám. Volitelný data_ptr (unsigned *) dostane
             * g_debugger_history.position ze stejného okamžiku jako řádky
             * (za běhu se ring mezi dvěma příkazy posune).
             *
             * (hypotéza) Pro V1 vystačíme s g_debugger_history rámcem;
             * pokud bude třeba bohatší metadata (T-states, registry
             * snapshot atd.), přejde V1.5+ na trace-suite cputrack
             * čtení skrz vlastní CMD. */
            if (rq->result_ptr)
            {
                memcpy ( rq->result_ptr,
                         g_debugger_history.row,
                         sizeof(g_debugger_history.row) );
                if ( rq->data_ptr )
                {
                    *(unsigned *) rq->data_ptr = g_debugger_history.position;
                };
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_CPU_FLAGS:
            /* Doplňkový stav CPU pro CPU window (IFF, IM, HALT, INT/NMI
             * pending, EI delay, Q reg, total/frame cycles, op_tstate).
             * result_ptr: st_DBGAPI_CPU_FLAGS* - caller alokuje. Caller
             * read-only používá fields - update_mask v V0 ignorován
             * (rezerva pro budoucí SET_CPU_FLAGS).
             *
             * Čtení probíhá z g_mzarch_main.cpu v emu vlákně - bez race
             * (jsme v dispatch loopu, žádná instrukce neběží).
             */
            if (rq->result_ptr)
            {
                st_DBGAPI_CPU_FLAGS *out =
                    (st_DBGAPI_CPU_FLAGS *)rq->result_ptr;
                z80_t *cpu = g_mzarch_main.cpu;
                out->iff1         = cpu->iff1 ? 1 : 0;
                out->iff2         = cpu->iff2 ? 1 : 0;
                out->im           = cpu->im;
                out->halted       = cpu->halted ? 1 : 0;
                out->int_pending  = cpu->int_pending ? 1 : 0;
                out->nmi_pending  = cpu->nmi_pending ? 1 : 0;
                out->ei_delay     = cpu->ei_delay ? 1 : 0;
                out->q            = cpu->q;
                out->total_cycles = cpu->total_cycles;
                out->cycles       = cpu->cycles;
                out->op_tstate    = cpu->op_tstate;
                out->update_mask  = 0;
                out->i_reg        = cpu->i;
                out->r_reg        = cpu->r;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_SET_CPU_FLAGS:
            /* Selektivni zapis CPU stavu (IFF1/IFF2/IM/I/R) dle update_mask.
             *
             * Pro kazdy bit ve flags->update_mask se aplikuje odpovidajici
             * pole z struct. Ostatni fieldy struct se ignoruji (= caller
             * nemusi vyplnovat, jen ten field ktery zapisuje + bit v mask).
             *
             * Bezpecnost: SET behem running stavu je v principu race
             * (modifikuje CPU stav mezi instrukcemi). Handler bezi v emu
             * vlakne v safepointu mezi instrukcemi - atomicita zajistena.
             * UI ridi pause pres dbg_autopause_silent pred submitem.
             *
             * IM validation: 0/1/2 jen, jine hodnoty success=false.
             */
            if (rq->data_ptr)
            {
                st_DBGAPI_CPU_FLAGS *p = (st_DBGAPI_CPU_FLAGS *)rq->data_ptr;
                z80_t *cpu = g_mzarch_main.cpu;
                bool ok = true;

                /* Podmíněný zápis (observe-cas M10): prev_* vždy, při
                 * neshodě polí z expect_mask nic nezapsat. */
                if (!dbgapi_cpu_flags_cond_check(p, cpu))
                {
                    rq->success = false;
                    break;
                };

                if (p->update_mask & DBGAPI_CPU_FLAGS_UM_IFF1)
                {
                    cpu->iff1 = p->iff1 ? 1 : 0;
                };
                if (p->update_mask & DBGAPI_CPU_FLAGS_UM_IFF2)
                {
                    cpu->iff2 = p->iff2 ? 1 : 0;
                };
                if (p->update_mask & DBGAPI_CPU_FLAGS_UM_IM)
                {
                    if (p->im <= 2)
                    {
                        cpu->im = p->im;
                    }
                    else
                    {
                        ok = false;
                    };
                };
                if (p->update_mask & DBGAPI_CPU_FLAGS_UM_I)
                {
                    cpu->i = p->i_reg;
                };
                if (p->update_mask & DBGAPI_CPU_FLAGS_UM_R_FULL)
                {
                    /* Celý bajt jako LD R,A - jediná instrukce, která mění
                     * bit 7 R (báze cpu/z80/09-r-register.md). Umožní věrně
                     * obnovit stav CPU přes MCP. */
                    cpu->r = p->r_reg;
                }
                else if (p->update_mask & DBGAPI_CPU_FLAGS_UM_R)
                {
                    /* Editace refresh čítače v okně CPU: dolních 7 bitů,
                     * bit 7 R (mění ho jen LD R,A, M1 cykly ne) zachovat. */
                    cpu->r = (uint8_t)(p->r_reg & 0x7F)
                           | (cpu->r & 0x80);
                };

                rq->success = ok;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_IM2_VECTOR:
            /* IM2 ISR vektor pro CPU window. Vrací stav PIO-Z80 IRQ
             * chainu + dekódovanou ISR table adresu a její dereferenci
             * (= cílovou adresu, kam by Z80 skočil pri IM 2 interruptu).
             *
             * Platformy bez PIO-Z80 (MZ-700) vrací available=0; UI
             * sekci pak skryje. Compile-time gating přes HAVE_PIOZ80
             * (= per-arch makro v mz{700,800,1500}_config.h).
             *
             * PIO-Z80 IRQ chain priorita: port A nad port B - shodné
             * s pioz80_interrupt_ack_im2_cb v hw-generic/pioz80/pioz80.c
             * (iteruje port_id = A; port_id < COUNT, break na první
             * PENDING && ICENA_ENABLED port).
             *
             * Memory dereference (isr_table_addr -> isr_target_addr)
             * čte přes debugger_memory_read_byte (= banking-aware bez
             * side effects), abychom respektovali ROM/RAM mapping.
             */
            if (rq->result_ptr)
            {
                st_DBGAPI_IM2_VECTOR *out =
                    (st_DBGAPI_IM2_VECTOR *)rq->result_ptr;
                memset(out, 0, sizeof(*out));
#if HAVE_PIOZ80
                z80_t *cpu = g_mzarch_main.cpu;
                out->available  = 1;
                out->im         = cpu->im;
                out->i_register = cpu->i;

                /* Detekce pending IRQ + výběr zdroje (port A > port B).
                 * Pokud žádný port není v PENDING && ICENA_ENABLED, vrátíme
                 * vector_byte = 0 (= pioz80_interrupt_ack_im2_cb chování
                 * mimo INTERRUPT_PENDING stav). */
                out->pio_irq_pending = 0;
                out->pio_source      = 0;
                out->vector_byte     = 0;
                if (g_pioz80.interrupt == PIOZ80_INTERRUPT_PENDING)
                {
                    for (int pid = PIOZ80_PORT_A; pid < PIOZ80_PORT_COUNT; pid++)
                    {
                        st_PIOZ80_PORT *port = &g_pioz80.port[pid];
                        if (port->port_int == PIOZ80_PORT_INT_PENDING
                            && port->icena == PIOZ80_ICENA_ENABLED)
                        {
                            out->pio_irq_pending = 1;
                            out->pio_source      = (uint8_t)pid;
                            out->vector_byte     = port->interrupt_vector;
                            break;
                        };
                    };
                };

                out->isr_table_addr  =
                    (uint16_t)(((uint16_t)out->i_register << 8) | out->vector_byte);
                /* Dereferenci provádíme bez ohledu na pending stav - když
                 * není pending, vector_byte=0 a ukazujeme table[0] (=
                 * adresa, kterou by Z80 nedostal, ale UI to vyznačí). */
                uint8_t lo = debugger_memory_read_byte(out->isr_table_addr);
                uint8_t hi = debugger_memory_read_byte((uint16_t)(out->isr_table_addr + 1));
                out->isr_target_addr = (uint16_t)(lo | (hi << 8));
#else
                /* MZ-700: PIO-Z80 nedostupné, IM 2 vector by se musel řešit
                 * jiným zdrojem dat (RST 38h v IM 1 / nepoužívá se). */
                out->available = 0;
#endif
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_LAST_INSTR:
            /* Vrací nejnovější záznam z g_debugger_history ringu (=
             * poslední dokončená instrukce). position field v ringu
             * ukazuje na slot posledního M1 startu - hodnota není
             * volně dostupná přes HISTORY_GET (= jen pole row[]).
             *
             * Délka instrukce se dopočítává přes z80_dasm_op nad
             * uloženými bajty (= row.byte[0..3]). Pokud history neaktivní
             * nebo prázdná (= addr+byte[0] obojí 0 ve slotu), vrátí valid=0. */
            if (rq->result_ptr)
            {
                st_DBGAPI_LAST_INSTR *out =
                    (st_DBGAPI_LAST_INSTR *)rq->result_ptr;
                memset(out, 0, sizeof(*out));

                unsigned pos = debugger_history_position(g_debugger_history.position);
                st_DEBUGGER_HISTORY_ROW *row = &g_debugger_history.row[pos];

                /* Defenzivni "neni co ukazat": vsechny bajty nuly + addr 0.
                 * Po resetu je ring vynulovany a position=0 - byte[0]=0 ale
                 * to muze byt validni NOP (00h) v cervence; po prvni
                 * instrukci uz position!=0 nebo byte[0]!=0. */
                bool empty = (g_debugger_history.position == 0
                              && row->addr == 0
                              && row->byte[0] == 0
                              && row->byte[1] == 0
                              && row->byte[2] == 0
                              && row->byte[3] == 0);
                if (empty)
                {
                    out->valid = 0;
                    rq->success = true;
                    break;
                };

                out->valid = 1;
                out->addr  = row->addr;
                for (int b = 0; b < 4; b++) {
                    out->bytes[b] = row->byte[b];
                };

                /* Dopocet delky z80_dasm() nad row.byte[] - read_fn cte
                 * z bufferu (offset = addr - row->addr), respektuje 4-byte
                 * limit historie. */
                z80_dasm_inst_t inst;
                int len = z80_dasm ( &inst,
                                     dbgapi_last_instr_read_cb,
                                     row, /* user_data = row pointer */
                                     row->addr );
                if (len < 1) len = 1;
                if (len > 4) len = 4;
                out->length = (uint8_t)len;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_RASTER_POS:
            /* Pozice rastru + Z80 cycle countery pro CPU window
             * "Cycles & raster" sekci. Čteme z g_gdg (= GDG state) +
             * g_mzarch_main.cpu->total_cycles/cycles.
             *
             * scanline = g_gdg.beam_row (= aktuální raster row, sjednoceno
             * s io_history záznamem).
             * column_pixel = VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks)
             * - poznámka: ticks je počet GDG ticks od začátku snímku,
             * VIDEO_GET_SCREEN_COL = ticks % VIDEO_SCREEN_WIDTH.
             */
            if (rq->result_ptr)
            {
                st_DBGAPI_RASTER_POS *out =
                    (st_DBGAPI_RASTER_POS *)rq->result_ptr;
                z80_t *cpu = g_mzarch_main.cpu;
                out->frame_number = (uint32_t)g_gdg.total_elapsed.screens;
                out->scanline     = (uint16_t)g_gdg.beam_row;
                out->column_pixel = (uint16_t)VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks);
                out->total_cycles = cpu->total_cycles;
                out->frame_cycles = cpu->cycles;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_CPU_PANEL_BATCH:
            /* Agregovaný snapshot pro CPU panel v jediném round-tripu.
             * Místo 5 separátních sync calls (= 5 čekání na emu safepoint)
             * UI submituje 1 batch a dostane všechna potřebná data v jedné
             * dispatch iteraci. Per-section gating přes which-mask
             * (data_ptr): UI ptá jen na sekce, které jsou expanded.
             *
             * Regs + flags se naplňují vždy (= core panel, levné čtení).
             * Volitelné sekce (IM2/raster/last_instr) jen pokud caller
             * nastavil odpovídající WANT_* bit ve which-mask.
             *
             * Plnění je ve sdílené dbgapi_cpu_panel_fill() (per-arch
             * HAVE_PIOZ80 gating, banking-aware mem read, history empty
             * detekce).
             */
            if (rq->result_ptr)
            {
                st_DBGAPI_CPU_PANEL_BATCH *out =
                    (st_DBGAPI_CPU_PANEL_BATCH *)rq->result_ptr;
                uint32_t which = 0;
                if (rq->data_ptr)
                    which = *((uint32_t *)rq->data_ptr);

                dbgapi_cpu_panel_fill(out, which);
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR:
            /* V3.3: zápis g_pioz80.port[id].interrupt_vector. Handler
             * maskuje bit 0 (= IVW spec - vždy 0 v Z80 PIO).
             *
             * Validace: port_id in {0, 1}, jinak success = false.
             * Na MZ-700 (HAVE_PIOZ80 == 0) success = false - PIO-Z80
             * v této architektuře neexistuje (g_pioz80 by ani nebylo
             * linkovatelné).
             *
             * Bezpečnost: handler běží v safepointu emu vlákna mezi
             * instrukcemi, atomicita zápisu uint8_t je triviální. UI
             * řídí pause přes dbg_autopause_silent před submitem (= edit
             * není v running, ale pro jistotu žádný extra lock potřeba). */
#if HAVE_PIOZ80
            if (rq->data_ptr)
            {
                st_DBGAPI_PIOZ80_VEC_PARAM *p =
                    (st_DBGAPI_PIOZ80_VEC_PARAM *)rq->data_ptr;
                if (p->port_id == PIOZ80_PORT_A
                    || p->port_id == PIOZ80_PORT_B)
                {
                    g_pioz80.port[p->port_id].interrupt_vector =
                        (uint8_t)(p->vector_byte & 0xFE);
                    rq->success = true;
                }
                else
                {
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
#else
            rq->success = false;
#endif
            break;

        case DBGAPI_CMD_MEM_WRITE_CHECKED:
            /* V3.3: zápis bloku do paměti s region check. Pro každou
             * adresu z [addr..addr+length-1] dotazujeme memmap_query
             * (= druh regionu 4 kB stránky podle aktuálního banking).
             * Pokud region patří mezi ne-zapisovatelné (ROM, CG-ROM,
             * VRAM v MZ-800 native módu, prohibited, unmapped, mapped
             * ports), žádný bajt se nezapíše a handler vrátí
             * success = 0 + first_failed_addr + first_failed_kind.
             *
             * Filozofie "all or nothing": region check je sekvenční,
             * první nezapisovatelná adresa = abort. UI typicky posílá
             * 2 bajty (ISR target little-endian) - pokud addr je v ROM,
             * tak je tam i addr+1 a check rozhoduje na první iteraci.
             *
             * Note: VRAM v MZ-800 native módu (VRAM_I, VRAM_II) je
             * technicky zapisovatelná z CPU strany (= memory_write_byte
             * by neselhal), ale Michalovo zadání ji explicit zakazuje
             * pro ISR target (= ISR vektor v plánové VRAM je nesmyslný,
             * VRAM se přepisuje hrami a ISR by se rozsypala). VRAM_TEXT
             * (MZ-700 / MZ-800 v 700 módu) je obyčejná RAM textového
             * režimu, ta povolena.
             *
             * Banking-aware write provádíme přes debugger_memory_write_byte
             * (= shoduje se s CMD_MEM_WRITE). */
            if (rq->data_ptr)
            {
                st_DBGAPI_MEM_WRITE_CHECKED_PARAM *p =
                    (st_DBGAPI_MEM_WRITE_CHECKED_PARAM *)rq->data_ptr;
                if (!p->data || p->length == 0)
                {
                    p->success = 0;
                    p->first_failed_addr = p->addr;
                    p->first_failed_kind = (uint8_t)MEMMAP_KIND_UNMAPPED;
                    rq->success = false;
                    break;
                };

                /* Fáze 1: region check pro každou adresu. */
                bool all_ok = true;
                for (uint32_t i = 0; i < p->length; i++)
                {
                    uint16_t a = (uint16_t)(p->addr + i);
                    en_MEMMAP_REGION_KIND kind = memmap_query((uint8_t)(a >> 12));
                    bool writable;
                    switch (kind)
                    {
                        case MEMMAP_KIND_RAM:
                        case MEMMAP_KIND_VRAM_TEXT:
                        case MEMMAP_KIND_CGRAM:
                        case MEMMAP_KIND_PCG_1:
                        case MEMMAP_KIND_PCG_2:
                        case MEMMAP_KIND_PCG_3:
                            writable = true;
                            break;
                        case MEMMAP_KIND_ROM_LOW:
                        case MEMMAP_KIND_ROM_HIGH:
                        case MEMMAP_KIND_CGROM:
                        case MEMMAP_KIND_VRAM_I:
                        case MEMMAP_KIND_VRAM_II:
                        case MEMMAP_KIND_MAPPED_PORTS:
                        case MEMMAP_KIND_PROHIBITED:
                        case MEMMAP_KIND_UNMAPPED:
                        default:
                            writable = false;
                            break;
                    };
                    if (!writable)
                    {
                        p->success = 0;
                        p->first_failed_addr = a;
                        p->first_failed_kind = (uint8_t)kind;
                        all_ok = false;
                        break;
                    };
                };

                if (!all_ok)
                {
                    /* Žádný bajt nezapsán - all-or-nothing semantika.
                     * rq->success = false: zamítnutý zápis není provedená
                     * akce. Jinak by ho Activity (MCP_ACTION broadcast)
                     * ukázala jako provedený a záznam vstupů (replay)
                     * by ho zaznamenal; přehrání by pak skončilo chybou
                     * "recorded debugger command failed". Důvod zamítnutí
                     * nesou p->first_failed_addr / first_failed_kind. */
                    rq->success = false;
                    break;
                };

                /* Fáze 1b: podmínka zápisu (observe-cas M10) až po
                 * kontrole regionu, aby nezapisovatelný cíl hlásil vždy
                 * chybu regionu a ne "stav se změnil". */
                if (!dbgapi_write_cond_before_cpu(p->cond, p->addr, p->length))
                {
                    p->success = 0;
                    rq->success = false;
                    break;
                };

                /* Fáze 2: vlastní zápis (region check prošel). */
                for (uint32_t i = 0; i < p->length; i++)
                {
                    debugger_memory_write_byte(
                        (uint16_t)(p->addr + i), p->data[i]);
                };
                dbgapi_write_cond_after_cpu(p->cond, p->addr, p->length);
                p->success = 1;
                p->first_failed_addr = 0;
                p->first_failed_kind = 0;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_SET_USER_CYCLE_ORIGIN:
            /* V3.1: nastavi g_debugger.user_cycle_origin na zadanou hodnotu.
             * UI typicky posila bud aktualni total_cycles (= "Reset" knoflik
             * -> User cyc display nasledne = 0) nebo total_cycles - new_value
             * pri user editu zobrazene hodnoty.
             *
             * data_ptr ukazuje na uint32_t (absolutni snapshot). Atomicita
             * 32-bit store na bezne 32+ bit platforme + emu vlakno v
             * safepointu mezi instrukcemi = bez additional locku.
             */
            if (rq->data_ptr)
            {
                uint32_t new_origin = *((uint32_t *)rq->data_ptr);
                g_debugger.user_cycle_origin = new_origin;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_DUMP:
            /* Stack monitor: hex dump paměti kolem SP.
             *
             * Dva režimy okénka (viz st_DBGAPI_STACK_DUMP_PARAM):
             *  - `lines_above > 0` (SP-anchored): handler spočítá
             *    `addr = sp + lines_above * 2` a zapíše ji zpět do
             *    paramu. Tím je okno vždy konzistentní s `sp_now`
             *    z téhož ticku (= odstraňuje 1-tick lag který by vznikl,
             *    kdyby UI počítala base z předchozího `sp_now`).
             *  - `lines_above == 0` (absolute): handler použije `addr`
             *    tak jak ji UI předala (legacy chování pro fixní inspekci).
             *
             * Handler naplní buffer banking-aware čtením (= shoda s
             * CMD_MEM_READ patternem) a navíc do `sp_now` zapíše aktuální
             * SP a do `sp_odd` flag liché hodnoty SP.
             *
             * Pozn.: čtení via debugger_memory_read_byte respektuje aktuální
             * banking (ROM / RAM / VRAM / CGROM podle portů $E0-$E4 na
             * MZ-800). Bez side effects pro VRAM / IORQ - debugger probe
             * se nesmí promítat do emu state. */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_DUMP_PARAM *p =
                    (st_DBGAPI_STACK_DUMP_PARAM *)rq->data_ptr;
                if (p->buf && p->len > 0)
                {
                    uint16_t sp = g_mzarch_main.cpu->sp;
                    if (p->lines_above > 0)
                    {
                        /* SP-anchored mode: handler spočítá base ze
                         * SP a buf naplní DESC (= buf[0] je bajt na
                         * adrese base, buf[i] na adrese base-i, ...).
                         * Render v UI bere buf[i*step] jako řádek i,
                         * který odpovídá adrese base - i*step, takže
                         * naplnění musí jít stejným směrem.
                         *
                         * Step (1 nebo 2) se odvozuje z parity SP:
                         * lichý SP = byte-oriented fallback (step=1),
                         * sudý SP = word-oriented default (step=2).
                         * Render v UI počítá step stejnou logikou, takže
                         * obě strany jsou synchronní bez state mismatch.
                         * Bez tohohle: pro lichý SP se base = SP +
                         * lines_above*2 posune dvakrát dál než render
                         * očekává (= SP marker mimo zobrazené okno). */
                        uint32_t step = (sp & 0x01u) ? 1u : 2u;
                        uint16_t base = (uint16_t)(
                            sp + (uint32_t)p->lines_above * step );
                        p->addr = base;
                        for (uint32_t i = 0; i < p->len; i++)
                        {
                            p->buf[i] = debugger_memory_read_byte (
                                (uint16_t)(base - i) );
                        };
                    }
                    else
                    {
                        /* Absolute mode (legacy): buf naplněn ASC od
                         * p->addr nahoru. Vhodné pro inspekci konkrétní
                         * adresy bez vazby na SP. */
                        for (uint32_t i = 0; i < p->len; i++)
                        {
                            p->buf[i] = debugger_memory_read_byte (
                                (uint16_t)(p->addr + i) );
                        };
                    };
                    p->sp_now = sp;
                    p->sp_odd = (uint8_t)(sp & 0x01u);

                    /* V3: disasm-back heuristika pro Decode sloupec.
                     * Vyplňuje se jen pokud caller dodal pole decode_buf
                     * a SP je sudý (word-mode). Pro lichý SP (byte-mode)
                     * není word kandidát definován - decode přeskočíme
                     * a UI ho ignoruje.
                     *
                     * Pro každý řádek tabulky (index `i` v rozsahu
                     * 0..decode_count) odpovídá adresa řádku
                     * `addr_i = p->addr - i*2` (DESC, word step). Word
                     * kandidát na této pozici je LE-word:
                     *   W = mem[addr_i] | (mem[addr_i+1] << 8)
                     * Sharp Z80 stack ukládá návratovou adresu LE
                     * (LSB na nižší adrese, MSB na vyšší).
                     *
                     * Detekce:
                     *  - mem[W-3] == 0xCD          -> CALL nn (3 bajty,
                     *      target = mem[W-2]|mem[W-1]<<8)
                     *  - mem[W-3] in {C4,CC,D4,DC,E4,EC,F4,FC}
                     *                              -> CALL cc,nn (target = idem)
                     *  - mem[W-1] in {C7,CF,D7,DF,E7,EF,F7,FF}
                     *                              -> RST n (target = opcode & 0x38)
                     * Jinak NONE.
                     *
                     * Banking-aware: použito debugger_memory_read_byte,
                     * respektuje aktuální mapping. Stejně jako CMD_MEM_READ
                     * pattern, bez side effects.
                     *
                     * Heuristika je nezávislá na lines_above mode - pracuje
                     * s naplněnou hodnotou p->addr (base). V absolute mode
                     * (lines_above == 0) je word kandidát počítaný stejně,
                     * jen base = původní p->addr před handlerem. */
                    if (p->decode_buf && p->decode_count > 0
                        && (sp & 0x01u) == 0)
                    {
                        uint16_t base = p->addr;
                        uint16_t max_lines = p->decode_count;
                        /* Omezit dle len - jen tolik řádků kolik se vejde
                         * do word-mode tabulky. */
                        uint16_t fit = (uint16_t)(p->len / 2u);
                        if (max_lines > fit) max_lines = fit;

                        for (uint16_t i = 0; i < max_lines; i++)
                        {
                            uint16_t addr_i = (uint16_t)(base - (uint32_t)i * 2u);
                            uint8_t lo = debugger_memory_read_byte(addr_i);
                            uint8_t hi = debugger_memory_read_byte(
                                (uint16_t)(addr_i + 1u));
                            uint16_t w = (uint16_t)(((uint16_t)hi << 8) | lo);

                            st_DBGAPI_STACK_DECODE_INFO *d = &p->decode_buf[i];
                            d->type   = DBGAPI_STACK_DECODE_NONE;
                            d->opcode = 0;
                            d->target = 0;

                            /* CALL family: opcode na W-3. */
                            uint8_t op3 = debugger_memory_read_byte(
                                (uint16_t)(w - 3u));
                            if (op3 == 0xCDu)
                            {
                                /* CALL nn - target ze dvou bajtů za opcode. */
                                uint8_t tlo = debugger_memory_read_byte(
                                    (uint16_t)(w - 2u));
                                uint8_t thi = debugger_memory_read_byte(
                                    (uint16_t)(w - 1u));
                                d->type   = DBGAPI_STACK_DECODE_CALL;
                                d->opcode = op3;
                                d->target = (uint16_t)(
                                    ((uint16_t)thi << 8) | tlo);
                            }
                            else if ((op3 & 0xC7u) == 0xC4u)
                            {
                                /* CALL cc,nn - opcody 0xC4/CC/D4/DC/E4/EC/F4/FC.
                                 * Bity 7..6 = 11, bity 2..0 = 100 -> mask 0xC7
                                 * porovnán s 0xC4. */
                                uint8_t tlo = debugger_memory_read_byte(
                                    (uint16_t)(w - 2u));
                                uint8_t thi = debugger_memory_read_byte(
                                    (uint16_t)(w - 1u));
                                d->type   = DBGAPI_STACK_DECODE_CALL_CC;
                                d->opcode = op3;
                                d->target = (uint16_t)(
                                    ((uint16_t)thi << 8) | tlo);
                            }
                            else
                            {
                                /* RST family: opcode na W-1. RST n má
                                 * masku 0xC7 == 0xC7 (= 11 nnn 111),
                                 * target = nnn * 8 (= opcode & 0x38). */
                                uint8_t op1 = debugger_memory_read_byte(
                                    (uint16_t)(w - 1u));
                                if ((op1 & 0xC7u) == 0xC7u)
                                {
                                    d->type   = DBGAPI_STACK_DECODE_RST;
                                    d->opcode = op1;
                                    d->target = (uint16_t)(op1 & 0x38u);
                                };
                            };
                        };

                        /* Zbytek decode_buf (i >= max_lines) zůstane
                         * nedotčen - caller je odpovědný za inicializaci
                         * nebo si pamatuje hranici z decode_count. Tady
                         * pro defenzivu nulujeme. */
                        for (uint16_t i = max_lines; i < p->decode_count; i++)
                        {
                            p->decode_buf[i].type   = DBGAPI_STACK_DECODE_NONE;
                            p->decode_buf[i].opcode = 0;
                            p->decode_buf[i].target = 0;
                        };
                    }
                    else if (p->decode_buf && p->decode_count > 0)
                    {
                        /* Lichý SP (byte-mode) - decode nedává smysl,
                         * nulujeme celé pole. */
                        for (uint16_t i = 0; i < p->decode_count; i++)
                        {
                            p->decode_buf[i].type   = DBGAPI_STACK_DECODE_NONE;
                            p->decode_buf[i].opcode = 0;
                            p->decode_buf[i].target = 0;
                        };
                    };

                    rq->success = true;
                }
                else
                {
                    rq->success = false;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_REGIONS_LIST:
            /* Stack monitor V1: snapshot definovaných stack regionů.
             * UI alokuje strukturu s polem regions[DBGAPI_STACK_REGIONS_MAX]
             * a handler ji naplní z g_stack_regions[]. Per-region flag
             * current_sp_in_region se počítá proti aktuálnímu SP. */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_REGIONS_LIST_PARAM *p =
                    (st_DBGAPI_STACK_REGIONS_LIST_PARAM *)rq->data_ptr;
                uint16_t sp = g_mzarch_main.cpu->sp;
                p->sp_now = sp;
                int n = g_stack_regions_count;
                if (n > DBGAPI_STACK_REGIONS_MAX) n = DBGAPI_STACK_REGIONS_MAX;
                p->count = n;
                for (int i = 0; i < n; i++)
                {
                    st_STACK_REGION *src = &g_stack_regions[i];
                    st_DBGAPI_STACK_REGION_INFO *dst = &p->regions[i];
                    /* Kopie jména s ochranou proti přetečení. */
                    memset(dst->name, 0, sizeof(dst->name));
                    strncpy(dst->name, src->name, sizeof(dst->name) - 1);
                    dst->base       = src->base;
                    dst->limit      = src->limit;
                    dst->watermark  = src->watermark;
                    dst->push_count = src->push_count;
                    dst->pop_count  = src->pop_count;
                    dst->current_sp_in_region  =
                        (sp >= src->limit && sp <= src->base) ? 1u : 0u;
                };
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_REGIONS_ADD:
            /* Stack monitor V1: přidat nový region. Validace v
             * stack_regions_add (= base > limit, name regex, full check).
             * Handler vrací nový index nebo -1 přes p->result_index +
             * rq->success. */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_REGIONS_ADD_PARAM *p =
                    (st_DBGAPI_STACK_REGIONS_ADD_PARAM *)rq->data_ptr;
                /* Zajisti '\0' termination - caller mohl předat
                 * bez explicitního konce. */
                p->name[sizeof(p->name) - 1] = '\0';
                int idx = stack_regions_add(p->name, p->base, p->limit);
                p->result_index = idx;
                rq->success = (idx >= 0);
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_REGIONS_REMOVE:
            /* Stack monitor V1: odebrat region na zadaném indexu. */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_REGIONS_REMOVE_PARAM *p =
                    (st_DBGAPI_STACK_REGIONS_REMOVE_PARAM *)rq->data_ptr;
                rq->success = stack_regions_remove(p->index);
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_REGIONS_RESET_WATERMARK:
            /* Stack monitor V1: reset watermark + counters jednoho regionu. */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_REGIONS_REMOVE_PARAM *p =
                    (st_DBGAPI_STACK_REGIONS_REMOVE_PARAM *)rq->data_ptr;
                rq->success = stack_regions_reset_watermark(p->index);
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_REGIONS_EDIT:
            /* Stack monitor V7: edit existujícího regionu. Validace +
             * overlap detekce v stack_regions_edit. Při úspěchu resetuje
             * watermark + counters (= staré stats nesedí na nový rozsah). */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_REGIONS_EDIT_PARAM *p =
                    (st_DBGAPI_STACK_REGIONS_EDIT_PARAM *)rq->data_ptr;
                /* Zajisti '\0' termination - caller mohl předat neukončený
                 * string. */
                p->name[sizeof(p->name) - 1] = '\0';
                bool ok = stack_regions_edit((int)p->idx, p->name,
                                              p->base, p->limit);
                p->success  = ok;
                rq->success = ok;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_HISTORY_ENABLE:
            /* Stack monitor V2: zapnout/vypnout SP history recording.
             * Vypnutí navíc vyprázdní ring buffer (= další zapnutí začne
             * s čistým stavem). Aktivační flag se promítne do hot-path
             * call site v mzarch.c (= zero overhead kdy default OFF). */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_HISTORY_ENABLE_PARAM *p =
                    (st_DBGAPI_STACK_HISTORY_ENABLE_PARAM *)rq->data_ptr;
                stack_history_set_active(p->enable != 0);
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_HISTORY_GET:
            /* Stack monitor V2: bulk snapshot SP history ring bufferu.
             *
             * Caller alokuje pole samples a předá pres `samples` pointer.
             * Handler ho v safepointu naplní vzorky oldest-first
             * (= samples[0] = nejstarší v okénku, samples[count-1] = nejnovější).
             * Velikost okénka = min(max_count, current_count).
             *
             * Slope se počítá v stejném safepointu (= žádný race vs.
             * záznam z hot-path).
             *
             * `samples` pointer drží caller (= UI alokuje); handler ho
             * jen čte/zapisuje, neuvolňuje. */
            if (rq->data_ptr)
            {
                st_DBGAPI_STACK_HISTORY_GET_PARAM *p =
                    (st_DBGAPI_STACK_HISTORY_GET_PARAM *)rq->data_ptr;
                if (p->samples && p->max_count > 0)
                {
                    /* Kopie přímo do caller bufferu - emu vzorek a
                     * dbgapi vzorek mají stejný layout (oba 8 B,
                     * cycles + sp + pad). Bezpečné typu-pun.
                     *
                     * Pozn.: stack_history_copy_recent očekává
                     * st_STACK_HISTORY_SAMPLE*, který je v hot-path
                     * (emu) headeru. Caller používá
                     * st_DBGAPI_STACK_HISTORY_SAMPLE = identický layout.
                     * Cast je triviální, není UB protože struct má
                     * standard-layout POD typ. */
                    p->count = stack_history_copy_recent(
                        (st_STACK_HISTORY_SAMPLE *)p->samples,
                        p->max_count);
                }
                else
                {
                    p->count = 0;
                };
                p->active = g_stack_history_active ? 1u : 0u;
                /* Slope: pokud caller nezadal window (= 0), použij 256
                 * jako rozumný default. */
                uint32_t win = p->slope_window;
                if (win == 0) win = 256;
                p->slope = stack_history_compute_slope(win);
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_STACK_HISTORY_RESET:
            /* Stack monitor V2: vyprázdnit ring buffer (recording flag
             * zachován). Vhodné pro UI "Reset history" tlačítko. */
            stack_history_reset();
            rq->success = true;
            break;

        /* ============================================================
         * Event Viewer (mutant event-viewer, Vlna 1 Commit 1)
         * ============================================================ */

        case DBGAPI_CMD_EVENTLOG_START:
            /* Spustit zápis do in-memory ringu. Vrací success podle
             * stavu ringu - pokud není alokovaný, eventlog_start vrátí
             * -1 (= chyba propagovaná do rq->success). */
            rq->success = ( eventlog_start ( ) == 0 );
            break;

        case DBGAPI_CMD_EVENTLOG_STOP:
            eventlog_stop ( );
            rq->success = true;
            break;

        case DBGAPI_CMD_EVENTLOG_CLEAR:
            eventlog_clear ( );
            rq->success = true;
            break;

        case DBGAPI_CMD_EVENTLOG_SET_CAPACITY:
            /* Resize ringu (zahodí předchozí data). Handler clampuje
             * hodnotu do [MIN..MAX] a vrátí výslednou velikost. */
            if ( rq->data_ptr )
            {
                st_DBGAPI_EVENTLOG_CAPACITY_PARAM *p =
                    (st_DBGAPI_EVENTLOG_CAPACITY_PARAM *) rq->data_ptr;
                eventlog_set_capacity ( (size_t) p->capacity );
                p->capacity_after = (uint32_t) g_eventlog.capacity;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_EVENTLOG_SET_MASK:
            /* Atomický přepis 64-bit kategorie masky. UI to volá při
             * každé změně checkboxu (= cheap operace, žádný overhead). */
            if ( rq->data_ptr )
            {
                st_DBGAPI_EVENTLOG_MASK_PARAM *p =
                    (st_DBGAPI_EVENTLOG_MASK_PARAM *) rq->data_ptr;
                g_eventlog_active_mask = p->mask;
                g_eventlog_config.categories_mask = p->mask;
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_EVENTLOG_GET_EVENT:
            /* Snapshot jednoho eventu z ringu na logickém indexu.
             * Pokud idx mimo count, found = 0 a obsah event polí
             * není definovaný (caller nesmí číst). */
            if ( rq->data_ptr )
            {
                st_DBGAPI_EVENTLOG_GET_EVENT_PARAM *p =
                    (st_DBGAPI_EVENTLOG_GET_EVENT_PARAM *) rq->data_ptr;
                const st_EVENTLOG_EVENT *e = eventlog_get_event ( (size_t) p->idx );
                if ( e != NULL )
                {
                    p->found           = 1u;
                    p->pxclk_total     = e->pxclk_total;
                    p->screens_total   = e->screens_total;
                    p->pxclk_in_screen = e->pxclk_in_screen;
                    p->category        = e->category;
                    p->subtype         = e->subtype;
                    p->pc              = e->pc;
                    p->payload         = e->payload;
                }
                else
                {
                    p->found = 0u;
                };
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_CALLSTACK:
            /* Callstack snapshot: alokuje pole entries přes callstack_snapshot_get
             * (callee-allocated, g_malloc). Caller (UI) MUSÍ pole uvolnit přes
             * callstack_snapshot_free po dokončení renderování.
             *
             * Statistiky kopírujeme inline do param (= žádná dodatečná alokace).
             * Při g_callstack_active == 0 vrací handler success = true s
             * count = 0 a entries = NULL (= UI ukáže prázdný stack + hint).
             */
            if ( rq->data_ptr )
            {
                st_DBGAPI_CALLSTACK_GET_PARAM *p =
                    (st_DBGAPI_CALLSTACK_GET_PARAM *) rq->data_ptr;
                st_CALLSTACK_ENTRY *entries = NULL;
                int count = 0;
                int rc = callstack_snapshot_get ( &entries, &count );
                if ( rc != 0 )
                {
                    /* Alokační chyba uvnitř callstack_snapshot_get. */
                    p->entries = NULL;
                    p->count = 0;
                    rq->success = false;
                }
                else
                {
                    p->entries = (void *) entries;
                    p->count   = count;
                    st_CALLSTACK_STATS s;
                    callstack_get_stats ( &s );
                    p->current_depth     = s.current_depth;
                    p->max_depth_reached = s.max_depth_reached;
                    p->divergence_count  = s.divergence_count;
                    p->diverg_trampoline = s.diverg_trampoline;
                    p->diverg_longjmp    = s.diverg_longjmp;
                    p->diverg_mismatch   = s.diverg_mismatch;
                    p->sp_swap_count     = s.sp_swap_count;
                    p->overflow_count    = s.overflow_count;
                    p->stack_discard_count = s.stack_discard_count;
                    p->active            = g_callstack_active;
                    /* cycles_now: cpu->total_cycles snapshot pro UI Cyc-in
                     * absolute výpočet (= cycles uvnitř každého frame od
                     * jeho push do teď). Bez CPU = 0. */
                    p->cycles_now = g_mzarch_main.cpu
                        ? (uint64_t) g_mzarch_main.cpu->total_cycles
                        : 0u;
                    rq->success = true;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_GET_PROFILER:
            /* Profiler snapshot: alokuje pole entries přes profiler_snapshot_get
             * (callee-allocated, g_malloc). Caller (UI) MUSÍ pole uvolnit
             * po dokončení renderování (= rekonstrukce st_PROF_SNAPSHOT
             * z entries + entry_count a profiler_snapshot_free).
             *
             * Statistiky kopírujeme inline (= bez další alokace). Při
             * g_profiler_active == 0 vrací handler success = true s
             * naposledy nasbíranými daty (= "data po Stopu jsou stále
             * viewable").
             */
            if ( rq->data_ptr )
            {
                st_DBGAPI_PROFILER_GET_PARAM *p =
                    (st_DBGAPI_PROFILER_GET_PARAM *) rq->data_ptr;
                st_PROF_SNAPSHOT snap;
                int rc = profiler_snapshot_get ( &snap );
                if ( rc != 0 )
                {
                    /* Alokační chyba uvnitř profiler_snapshot_get. */
                    p->entries = NULL;
                    p->entry_count = 0;
                    rq->success = false;
                }
                else
                {
                    p->entries           = (void *) snap.entries;
                    p->entry_count       = snap.entry_count;
                    p->active            = snap.stats.active ? 1u : 0u;
                    p->total_cycles_64   = snap.stats.total_cycles_64;
                    p->total_calls       = snap.stats.total_calls;
                    p->irq_entries       = snap.stats.irq_entries;
                    p->unmatched_returns = snap.stats.unmatched_returns;
                    p->max_depth_reached = snap.stats.max_depth_reached;
                    p->overflow_count    = snap.stats.overflow_count;
                    rq->success = true;
                };
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_PROFILER_SET_ACTIVE:
            /* Přepnutí profileru ON/OFF z UI. profiler_set_active je
             * idempotentní a v EMU vlákně safe (= listener slot
             * manipulace je tady mimo hot path). */
            if ( rq->data_ptr )
            {
                st_DBGAPI_PROFILER_SET_ACTIVE_PARAM *p =
                    (st_DBGAPI_PROFILER_SET_ACTIVE_PARAM *) rq->data_ptr;
                profiler_set_active ( p->active ? true : false );
                rq->success = true;
            }
            else
            {
                rq->success = false;
            };
            break;

        case DBGAPI_CMD_PROFILER_RESET:
            /* Vynulování agregátoru za běhu. Nezasahuje g_profiler_active
             * (= pokud běží, dál sbírá nová data). */
            profiler_reset ( );
            rq->success = true;
            break;

        case DBGAPI_CMD_PROFILER_EXPORT:
            /* Export agregátoru do souboru (mutant mcp-server V1.A.7).
             * Delegát na profiler_export_to_file. Funkce je read-only nad
             * stavem profileru (= snapshot + format-specific writer),
             * takže nezávisí na paused state. */
            if ( rq->data_ptr )
            {
                st_DBGAPI_PROFILER_EXPORT_PARAM *p =
                    (st_DBGAPI_PROFILER_EXPORT_PARAM *) rq->data_ptr;
                int n = 0;
                int rc = profiler_export_to_file ( p->filepath, p->format, &n );
                p->result = rc;
                p->entry_count = n;
                rq->success = ( rc == 0 );
            }
            else
            {
                rq->success = false;
            };
            break;

        /* --- Snapshot (mutant mcp-server V1.A.1) --- */
        case DBGAPI_CMD_SNAPSHOT_SAVE_FILE:
        {
            /* Uloží snapshot do souboru. Delegát na snapshot_save z
             * snapshot.h (= V-1.2 API). Snapshot vyžaduje paused emu,
             * kontroluje sama snapshot vrstva (vrací SNAPSHOT_ERR_NOT_PAUSED). */
            st_DBGAPI_SNAPSHOT_PARAM *p = (st_DBGAPI_SNAPSHOT_PARAM *)rq->data_ptr;
            if (!p || !p->filepath)
            {
                rq->success = false;
                break;
            }
            en_SNAPSHOT_RESULT r = snapshot_save(p->filepath, p->description);
            p->result = (int)r;
            rq->success = (r == SNAPSHOT_OK);
            break;
        }

        case DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER:
        {
            /* Uloží snapshot do paměťového bufferu (= MCP inline payload).
             * Handler alokuje p->buffer přes g_malloc-kompatibilní cestu,
             * volající uvolní g_free. */
            st_DBGAPI_SNAPSHOT_PARAM *p = (st_DBGAPI_SNAPSHOT_PARAM *)rq->data_ptr;
            if (!p)
            {
                rq->success = false;
                break;
            }
            uint8_t *out_data = NULL;
            size_t   out_size = 0;
            en_SNAPSHOT_RESULT r = snapshot_save_to_buffer(p->description,
                                                            &out_data,
                                                            &out_size);
            p->buffer      = out_data;
            p->buffer_size = out_size;
            p->result      = (int)r;
            rq->success    = (r == SNAPSHOT_OK);
            break;
        }

        case DBGAPI_CMD_SNAPSHOT_LOAD_FILE:
        {
            /* Načte snapshot ze souboru. Po success je nový state aktivní;
             * MCP klient typicky následně pošle get_state / get_registers. */
            st_DBGAPI_SNAPSHOT_PARAM *p = (st_DBGAPI_SNAPSHOT_PARAM *)rq->data_ptr;
            if (!p || !p->filepath)
            {
                rq->success = false;
                break;
            }
            en_SNAPSHOT_RESULT r = snapshot_load(p->filepath);
            p->result   = (int)r;
            rq->success = (r == SNAPSHOT_OK);
            break;
        }

        case DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER:
        {
            /* Načte snapshot z paměťového bufferu (= base64 dekódovaná data
             * z MCP requestu). Po success je nový state aktivní. */
            st_DBGAPI_SNAPSHOT_PARAM *p = (st_DBGAPI_SNAPSHOT_PARAM *)rq->data_ptr;
            if (!p || !p->buffer || p->buffer_size == 0)
            {
                rq->success = false;
                break;
            }
            en_SNAPSHOT_RESULT r = snapshot_load_from_buffer(p->buffer,
                                                              p->buffer_size);
            p->result   = (int)r;
            rq->success = (r == SNAPSHOT_OK);
            break;
        }

        /* --- Symbol DB (mutant mcp-server V1.A.2) ---
         * Delegace na sym_db API z symbols/sym_db.h. Reálné API podporuje
         * jen user-defined LBL kind (= source=SYM_SOURCE_LBL); kind parametr
         * z MCP wire je echo-only a v dbgapi vrstvě se neukládá.
         *
         * Threading: handler běží v emu vlákně v safe-pointu (= cmdrq
         * dispatch). Reálné sym_db API je UI-thread, ale safe-point
         * garantuje, že UI nepřistupuje současně - bezpečné.
         */
        case DBGAPI_CMD_SYMBOL_ADD:
        {
            /* Přidá nebo přepíše user-defined symbol (LBL source).
             *
             * V1.C.3: po úspěšném add propagujeme rq->cmd_origin do
             * symbol DB záznamu. Lookup podle jména - sym_db API
             * podporuje set_cmd_origin by_name. */
            st_DBGAPI_SYMBOL_PARAM *p = (st_DBGAPI_SYMBOL_PARAM *)rq->data_ptr;
            if (!p || !p->name || p->name[0] == '\0')
            {
                rq->success = false;
                break;
            }
            int r = sym_db_add_user_label((uint32_t)p->addr, p->name, p->comment);
            p->source   = (uint8_t)SYM_SOURCE_LBL;
            if (r == 0)
            {
                sym_db_set_cmd_origin(p->name, rq->cmd_origin);
            }
            rq->success = (r == 0);
            break;
        }

        case DBGAPI_CMD_SYMBOL_REMOVE:
        {
            /* Odebere symbol podle jména nebo podle adresy. Pokud name
             * je NULL, najdeme symbol přes lookup_by_addr a remove podle
             * jeho name (= reálné API podporuje remove jen by_name). */
            st_DBGAPI_SYMBOL_PARAM *p = (st_DBGAPI_SYMBOL_PARAM *)rq->data_ptr;
            if (!p)
            {
                rq->success = false;
                break;
            }
            int r = -1;
            if (p->name && p->name[0] != '\0')
            {
                r = sym_db_remove_user_label(p->name);
            }
            else
            {
                /* Remove by addr - dohledat symbol, použít jeho name. */
                const st_SYMBOL *s = sym_db_lookup_by_addr((uint32_t)p->addr, 0);
                if (s && s->name)
                {
                    r = sym_db_remove_user_label(s->name);
                }
            }
            rq->success = (r == 0);
            break;
        }

        case DBGAPI_CMD_SYMBOL_LOOKUP:
        {
            /* Vrátí 0 nebo 1 záznam do out_entries (out_max musí být >=1).
             * Hex vs name detekce dělá vyšší MCP vrstva - sem dorazí
             * buď name (string) nebo addr (uint16_t). */
            st_DBGAPI_SYMBOL_PARAM *p = (st_DBGAPI_SYMBOL_PARAM *)rq->data_ptr;
            if (!p || !p->out_entries || p->out_max == 0)
            {
                if (p) p->out_count = 0;
                rq->success = false;
                break;
            }
            const st_SYMBOL *s = NULL;
            if (p->name && p->name[0] != '\0')
            {
                s = sym_db_lookup_by_name(p->name);
            }
            else
            {
                s = sym_db_lookup_by_addr((uint32_t)p->addr, 0);
            }
            if (s)
            {
                p->out_entries[0].addr    = (uint16_t)s->addr;
                p->out_entries[0].name    = s->name ? g_strdup(s->name) : NULL;
                p->out_entries[0].comment = s->comment ? g_strdup(s->comment) : NULL;
                p->out_entries[0].source  = (uint8_t)s->source;
                p->out_count = 1;
            }
            else
            {
                p->out_count = 0;
            }
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_SYMBOL_LIST:
        {
            /* Iteruje sym_db v insertion-order, filtruje prefix, omezuje
             * na out_max. Heap kopie name/comment - caller uvolní. */
            st_DBGAPI_SYMBOL_PARAM *p = (st_DBGAPI_SYMBOL_PARAM *)rq->data_ptr;
            if (!p || !p->out_entries || p->out_max == 0)
            {
                if (p) p->out_count = 0;
                rq->success = false;
                break;
            }
            size_t written = 0;
            size_t prefix_len = (p->prefix && p->prefix[0]) ? strlen(p->prefix) : 0;
            st_SYM_DB_ITER it;
            sym_db_iter_init(&it);
            const st_SYMBOL *s;
            while ((s = sym_db_iter_next(&it)) != NULL && written < p->out_max)
            {
                if (!s->name) continue;
                if (prefix_len > 0 &&
                    strncmp(s->name, p->prefix, prefix_len) != 0)
                {
                    continue;
                }
                p->out_entries[written].addr    = (uint16_t)s->addr;
                p->out_entries[written].name    = g_strdup(s->name);
                p->out_entries[written].comment =
                    s->comment ? g_strdup(s->comment) : NULL;
                p->out_entries[written].source  = (uint8_t)s->source;
                written++;
            }
            p->out_count = written;
            rq->success  = true;
            break;
        }

        case DBGAPI_CMD_STEP_OUT:
        {
            /* Step Out (V1.A.3 mcp-server): vyhledá top frame v shadow
             * callstacku, nastaví temporary breakpoint na return_addr a
             * volá run_to_temporary_breakpoint. Asynchronní - emu po
             * úspěšném submit běží, klient pollí get_state.
             *
             * Předpoklady:
             *  - Callstack tracking aktivní (g_callstack_active == 1)
             *  - current_depth > 0 (= jsme v subroutine)
             *  - emu paused (= jinak return UX jako STEP_OVER/RUN_TO)
             *
             * Diagnostické status kódy do payload->status (viz
             * st_DBGAPI_STEP_OUT_PARAM v dbgapi_cmdrq.h):
             *  0 = OK, 1 = callstack inactive, 2 = empty stack,
             *  3 = emu running, 4 = snapshot alloc error.
             */
            st_DBGAPI_STEP_OUT_PARAM *p =
                (st_DBGAPI_STEP_OUT_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->return_addr = 0;
            p->status      = 0;

            if ( !g_callstack_active )
            {
                p->status   = 1;
                rq->success = false;
                break;
            };

            /* Pokud emu běží, vrátíme se po pause (= konzistentní UX
             * s STEP_OVER / RUN_TO). Klient musí znovu volat step_out. */
            if ( !EMULATOR_TEST_PAUSED )
            {
                emulator_pause ( true );
                p->status   = 3;
                rq->success = false;
                break;
            };

            /* Snapshot callstacku - nás zajímá jen top frame, ale API
             * vrací celé pole; uvolníme po vytažení return_addr. */
            st_CALLSTACK_ENTRY *entries = NULL;
            int count = 0;
            int rc = callstack_snapshot_get ( &entries, &count );
            if ( rc != 0 )
            {
                p->status   = 4;
                rq->success = false;
                break;
            };
            if ( count <= 0 )
            {
                callstack_snapshot_free ( entries );
                p->status   = 2;
                rq->success = false;
                break;
            };

            /* Top frame = entries[count-1] (= naposledy pushnutý
             * volaný frame; jeho return_addr je kam se RET vrátí). */
            uint16_t target = entries[ count - 1 ].return_addr;
            callstack_snapshot_free ( entries );

            p->return_addr = target;
            p->run_seq = run_control_seq_for_start ( );
            bptmap_set_temporary_event ( target );
            debugger_step_call ( 0 );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            mzarch_run_to_temporary_breakpoint ( );
#endif
            (void) p->max_cycles; /* informativní; omezení běhu je deadline (run_control.h) */
            p->status   = 0;
            rq->success = true;
            break;
        }

        /* ====================================================================
         * Mutant mcp-server V1.A.5: chip-level Tools (= fault injection)
         * ==================================================================== */

        case DBGAPI_CMD_IO_READ:
        {
            /* Z80 IN side-effect čtení portu - volá port_read_cb stejně
             * jak by ho zavolala instrukce Z80 IN. Side effecty na chipech
             * (PSG, FDC status flag clear, GDG DMD strobe) probíhají
             * normálně. Klient by si měl být vědom destruktivního
             * dopadu - viz tool description.
             *
             * Pozn.: NEpoužíváme port_read_no_se_cb (= side-effect-free
             * probe), protože MCP klient typicky chce reálné čtení s
             * efektem. Probe varianta je v scope V1.A.5b. */
            st_DBGAPI_IO_PARAM *p = (st_DBGAPI_IO_PARAM *) rq->data_ptr;
            if ( !p || !g_mzarch_main.cpu )
            {
                rq->success = false;
                break;
            };
#if MZARCH == 700
            /* MZ-700: čistý callback z adaptéru devbus (porty zařízení). */
            p->value = g_mz700_cpu_ops.pread ( g_mzarch_main.cpu, p->port, NULL );
#else
            p->value = port_read_cb ( g_mzarch_main.cpu, p->port, NULL );
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_IO_WRITE:
        {
            /* Z80 OUT side-effect zápis - volá port_write_cb stejně jako
             * Z80 OUT instrukce. Chipy reagují (PSG latch, FDC command
             * latch, GDG mode, PIO output). Pokud má event_bus
             * subscribery na "io_write", port_write_with_logging_cb
             * (= aktivovaný při běhu debuggeru) emituje event sám. */
            st_DBGAPI_IO_PARAM *p = (st_DBGAPI_IO_PARAM *) rq->data_ptr;
            if ( !p || !g_mzarch_main.cpu )
            {
                rq->success = false;
                break;
            };
#if MZARCH == 700
            /* MZ-700: čistý callback z adaptéru devbus (porty zařízení). */
            g_mz700_cpu_ops.pwrite ( g_mzarch_main.cpu, p->port, p->value, NULL );
#else
            port_write_cb ( g_mzarch_main.cpu, p->port, p->value, NULL );
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_IRQ_INJECT:
        {
            /* Force maskable IRQ. Pokud vector_valid != 0, použij
             * explicit vektor (= z80_irq). Jinak default cestou s
             * intread_cb (= z80_int). Skutečné přijetí IRQ závisí na
             * IFF1 (= EI stav) - pokud disabled, IRQ se zapamatuje do
             * cpu->int_pending a vykoná se po nejbližším EI. */
            st_DBGAPI_IRQ_INJECT_PARAM *p =
                (st_DBGAPI_IRQ_INJECT_PARAM *) rq->data_ptr;
            if ( !p || !g_mzarch_main.cpu )
            {
                rq->success = false;
                break;
            };
            if ( p->vector_valid )
            {
                z80_irq ( g_mzarch_main.cpu, p->vector );
            }
            else
            {
                z80_int ( g_mzarch_main.cpu );
            };
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_NMI_INJECT:
        {
            /* Force NMI. NMI je nemaskovatelné - vždy se přijme po
             * dokončení aktuální instrukce, skočí na 0x0066, uloží
             * IFF1 do IFF2 a vynuluje IFF1. */
            if ( !g_mzarch_main.cpu )
            {
                rq->success = false;
                break;
            };
            z80_nmi ( g_mzarch_main.cpu );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_MEM_WRITE_FORCE:
        {
            /* Raw memory write bez region checku - duplikuje chování
             * DBGAPI_CMD_MEM_WRITE (= banking-aware přes
             * debugger_memory_write_byte), ale je explicit oddělené v
             * MCP layeru pro audit (= klient výslovně volí destruktivní
             * variantu, která dovolí přepis ROM oblastí, pokud je
             * v daném banking namapovaná RAM, nebo přímo do RAM
             * stínu pod ROM).
             *
             * Pozn.: skutečný ROM override (= zápis pod read-only
             * ROM mapping) by vyžadoval bypass debugger_memory_write_byte
             * na nižší úroveň (RAM array). To je destruktivní krok mimo
             * scope V1.A.5 - zatím poskytujeme alespoň MCP entry point
             * který kopíruje semantiku MEM_WRITE bez region check, takže
             * klient nemusí volat ne-validační variantu jiným cmd. */
            if ( !rq->data_ptr )
            {
                rq->success = false;
                break;
            };
            st_DBGAPI_MEM_PARAM *p = (st_DBGAPI_MEM_PARAM *) rq->data_ptr;
            if ( !p->buf )
            {
                rq->success = false;
                break;
            };
            /* Podmíněný zápis (observe-cas M10). */
            if ( !dbgapi_write_cond_before_cpu ( p->cond, p->addr, p->len ) )
            {
                rq->success = false;
                break;
            };
            for ( uint32_t i = 0; i < p->len; i++ )
            {
                debugger_memory_write_byte (
                    (uint16_t) ( p->addr + i ), p->buf[ i ] );
            };
            dbgapi_write_cond_after_cpu ( p->cond, p->addr, p->len );
            rq->success = true;
            break;
        }

        /* === V1.A.6 - Watch + CDL Tools handlery ================== */

        case DBGAPI_CMD_WATCH_ADD:
        {
            /* Přidat watch řádek do watch storage (= UI-vlákno owned, ale
             * v sync handleru je EMU vlákno blokované a UI vlákno čeká na
             * odpověď, takže storage je v tu chvíli klidná). Mode ADDRESS
             * volá watch_add(), mode EXPR_* volá watch_add_expr().
             *
             * V1.C.3: po úspěšném add propagujeme rq->cmd_origin do
             * vytvořeného řádku. */
            st_DBGAPI_WATCH_ADD_PARAM *p =
                (st_DBGAPI_WATCH_ADD_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            int idx = -1;
            if ( p->mode == DBGAPI_WATCH_MODE_ADDRESS )
            {
                idx = watch_add ( p->name, p->addr, -1 );
                if ( idx >= 0 )
                {
                    watch_set_type ( idx, (en_WATCH_TYPE) p->type );
                };
            }
            else
            {
                en_WATCH_MODE m = ( p->mode == DBGAPI_WATCH_MODE_EXPR_DEREF )
                    ? WATCH_MODE_EXPR_DEREF
                    : WATCH_MODE_EXPR_SCALAR;
                idx = watch_add_expr ( p->name, p->expr_text, m,
                                        (en_WATCH_TYPE) p->type );
            };
            if ( idx >= 0 )
            {
                watch_set_cmd_origin ( idx, rq->cmd_origin );
            };
            p->out_index = idx;
            rq->success = ( idx >= 0 );
            break;
        }

        case DBGAPI_CMD_WATCH_REMOVE:
        {
            /* Odstraní watch řádek. Hledání podle name = lineární scan
             * watch_count() / watch_get(). Pokud name == NULL, použije se
             * index přímo. */
            st_DBGAPI_WATCH_REMOVE_PARAM *p =
                (st_DBGAPI_WATCH_REMOVE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            int idx = p->index;
            if ( p->name && p->name[0] != '\0' )
            {
                idx = -1;
                size_t cnt = watch_count();
                for ( size_t i = 0; i < cnt; i++ )
                {
                    const st_WATCH_ROW *row = watch_get ( i );
                    if ( row && row->name &&
                         strcmp ( row->name, p->name ) == 0 )
                    {
                        idx = (int) i;
                        break;
                    };
                };
            };
            if ( idx < 0 || (size_t) idx >= watch_count() )
            {
                p->out_removed = 0;
                p->index = -1;
                rq->success = false;
                break;
            };
            watch_remove ( idx );
            p->index = idx;
            p->out_removed = 1;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_WATCH_LIST:
        {
            /* Naplní out_entries[] z watch storage. Pro každý řádek
             * přečte aktuální hodnotu (watch_read_int / watch_read_bytes)
             * a zformátuje ji do value_str. Stringy g_strdup, caller
             * uvolňuje. */
            st_DBGAPI_WATCH_LIST_PARAM *p =
                (st_DBGAPI_WATCH_LIST_PARAM *) rq->data_ptr;
            if ( !p || !p->out_entries || p->out_max <= 0 )
            {
                if ( p ) p->out_count = 0;
                rq->success = false;
                break;
            };
            size_t cnt = watch_count();
            int max = p->out_max;
            int n = ( (int) cnt < max ) ? (int) cnt : max;
            for ( int i = 0; i < n; i++ )
            {
                const st_WATCH_ROW *row = watch_get ( (size_t) i );
                st_DBGAPI_WATCH_LIST_ENTRY *e = &p->out_entries[ i ];
                e->index = i;
                e->name = ( row && row->name ) ? g_strdup ( row->name ) : NULL;
                e->mode = (en_DBGAPI_WATCH_MODE) ( row ? row->mode : 0 );
                e->type = (en_DBGAPI_WATCH_TYPE) ( row ? row->type : 0 );
                e->addr = row ? row->addr : 0;
                e->expr_text = ( row && row->expr_text )
                    ? g_strdup ( row->expr_text ) : NULL;
                char buf[ 96 ];
                buf[ 0 ] = '\0';
                if ( row )
                {
                    if ( watch_type_has_length ( row->type ) )
                    {
                        uint8_t tmp[ WATCH_LENGTH_MAX_BYTES ];
                        size_t out_len = 0;
                        watch_read_bytes ( row, tmp, sizeof ( tmp ), &out_len );
                        if ( row->type == WATCH_TYPE_BYTES )
                        {
                            watch_format_bytes ( tmp, out_len, buf,
                                                  sizeof ( buf ), 16 );
                        }
                        else
                        {
                            watch_format_ascii ( tmp, out_len, buf,
                                                  sizeof ( buf ),
                                                  row->type == WATCH_TYPE_MZASCII );
                        };
                    }
                    else
                    {
                        uint64_t v = watch_read_int ( row );
                        watch_format_int ( row, v, buf, sizeof ( buf ) );
                    };
                };
                e->value_str = g_strdup ( buf );
            };
            p->out_count = n;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_WATCH_EVAL:
        {
            /* Eval existující watch nebo ad-hoc výraz. Pro ad-hoc parsuje
             * bp_expr_parse + bp_expr_eval s kontextem aktuálního cpu stavu.
             * Stringy out_value_str + out_error jsou g_strdup, caller
             * uvolňuje. */
            st_DBGAPI_WATCH_EVAL_PARAM *p =
                (st_DBGAPI_WATCH_EVAL_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_value_str = NULL;
            p->out_error = NULL;
            p->out_value_int = 0;

            if ( p->expr_text && p->expr_text[0] != '\0' )
            {
                /* Ad-hoc eval bez perzistence. */
                char errbuf[ 128 ];
                errbuf[ 0 ] = '\0';
                bp_expr_t *ast = bp_expr_parse ( p->expr_text,
                                                  errbuf, sizeof ( errbuf ) );
                if ( !ast )
                {
                    p->out_error = g_strdup ( errbuf );
                    p->out_value_str = g_strdup ( "" );
                    rq->success = false;
                    break;
                };
                bp_expr_ctx_t ctx;
                bp_expr_ctx_zero ( &ctx );
                ctx.cpu = g_mzarch_main.cpu;
                /* Znak `!` se ignoruje jako u uloženého watche (N67). */
                ctx.no_side_effects = true;
                int32_t v = bp_expr_eval ( ast, &ctx );
                bp_expr_free ( ast );
                p->out_value_int = v;
                char buf[ 32 ];
                g_snprintf ( buf, sizeof ( buf ), "%d", (int) v );
                p->out_value_str = g_strdup ( buf );
                rq->success = true;
                break;
            };

            /* Eval existujícího watche. */
            int idx = p->index;
            if ( p->name && p->name[0] != '\0' )
            {
                idx = -1;
                size_t cnt = watch_count();
                for ( size_t i = 0; i < cnt; i++ )
                {
                    const st_WATCH_ROW *row = watch_get ( i );
                    if ( row && row->name &&
                         strcmp ( row->name, p->name ) == 0 )
                    {
                        idx = (int) i;
                        break;
                    };
                };
            };
            if ( idx < 0 || (size_t) idx >= watch_count() )
            {
                p->out_error = g_strdup ( "watch not found" );
                p->out_value_str = g_strdup ( "" );
                rq->success = false;
                break;
            };
            const st_WATCH_ROW *row = watch_get ( (size_t) idx );
            char buf[ 96 ];
            buf[ 0 ] = '\0';
            if ( watch_type_has_length ( row->type ) )
            {
                uint8_t tmp[ WATCH_LENGTH_MAX_BYTES ];
                size_t out_len = 0;
                watch_read_bytes ( row, tmp, sizeof ( tmp ), &out_len );
                if ( row->type == WATCH_TYPE_BYTES )
                {
                    watch_format_bytes ( tmp, out_len, buf,
                                          sizeof ( buf ), 16 );
                }
                else
                {
                    watch_format_ascii ( tmp, out_len, buf,
                                          sizeof ( buf ),
                                          row->type == WATCH_TYPE_MZASCII );
                };
                p->out_value_int = 0;
            }
            else
            {
                uint64_t v = watch_read_int ( row );
                watch_format_int ( row, v, buf, sizeof ( buf ) );
                p->out_value_int = (int32_t) v;
            };
            p->out_value_str = g_strdup ( buf );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_CDL_START:
        {
            /* Spustit CDL recording (= Memory Heatmap v ALWAYS módu).
             * mhmap_set_mode triggeruje swap CPU callbacků - hot path
             * přejde na _with_logging variantu. */
            mhmap_set_mode ( DEBUGGER_MHMAP_MODE_ALWAYS );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_CDL_STOP:
        {
            /* Zastavit CDL recording (= mhmap_mode = OFF). Data zůstávají
             * zachovaná - user musí Reset zavolat explicit pro vynulování. */
            mhmap_set_mode ( DEBUGGER_MHMAP_MODE_OFF );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_CDL_RESET:
        {
            /* Vynulovat všechny CDL countery (mhmap_reset). Nezasahuje
             * mode - pokud byl ON, zůstane ON a recording pokračuje. */
            mhmap_reset();
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_CDL_EXPORT:
        {
            /* Export CDL dat do souborů. mhmap_export(meta_path) vyrobí
             * meta JSON + per-region binární soubory (*_bus.cdl, *_ram.cdl,
             * atd.) v parent adresáři meta cesty. */
            st_DBGAPI_CDL_EXPORT_PARAM *p =
                (st_DBGAPI_CDL_EXPORT_PARAM *) rq->data_ptr;
            if ( !p || !p->meta_path || p->meta_path[0] == '\0' )
            {
                if ( p )
                {
                    p->out_result = -1;
                    p->out_region_count = 0;
                };
                rq->success = false;
                break;
            };
            int rc = mhmap_export ( p->meta_path );
            p->out_result = rc;
            size_t reg_count = 0;
            mhmap_get_export_regions ( &reg_count );
            p->out_region_count = (int) reg_count;
            rq->success = ( rc == 0 );
            break;
        }

        /* --- Tracking lifecycle (0017 FÁZE 1, trace-suite) ---
         *
         * Vlastní logika je extrahována do dbgapi_trace_lifecycle(), aby ji
         * beze změny chování sdílel i BP-action DSL forwarding (Z17d). Handler
         * jen mapuje cmd -> op, validuje param a propisuje out_result. */
        case DBGAPI_CMD_TRACE_START:
        case DBGAPI_CMD_TRACE_STOP:
        case DBGAPI_CMD_TRACE_RESET:
        case DBGAPI_CMD_TRACE_SAVE:
        {
            st_DBGAPI_TRACE_PARAM *p = (st_DBGAPI_TRACE_PARAM *) rq->data_ptr;
            if ( !p ) {
                rq->success = false;
                break;
            }
            en_DBGAPI_TRACE_OP op;
            switch ( cmd ) {
                case DBGAPI_CMD_TRACE_START: op = DBGAPI_TRACE_OP_START; break;
                case DBGAPI_CMD_TRACE_STOP:  op = DBGAPI_TRACE_OP_STOP;  break;
                case DBGAPI_CMD_TRACE_RESET: op = DBGAPI_TRACE_OP_RESET; break;
                default:                     op = DBGAPI_TRACE_OP_SAVE;  break;
            }
            /* Formát cputrack pro nový segment (regs): běžící segment
             * v jiném formátu se nepřepíše, start selže. */
            if ( op == DBGAPI_TRACE_OP_START && p->channel == DBGAPI_TRACE_CHANNEL_CPUTRACK && p->regs >= 0 )
            {
                if ( cputrack_is_recording ( ) && cputrack_segment_has_regs ( ) != ( p->regs ? 1 : 0 ) )
                {
                    p->out_result = DBGAPI_TRACE_ERR_REGS_RUNNING;
                    rq->success = false;
                    break;
                };
                g_cputrack_config.regs = p->regs ? 1u : 0u;
            };
            /* SAVE bere path z param; ostatní op path ignorují. */
            const char *path = ( cmd == DBGAPI_CMD_TRACE_SAVE ) ? p->path : NULL;
            int rc = dbgapi_trace_lifecycle ( p->channel, op, path );
            /* Start/stop z GUI nebo MCP: kanál už neřídí akce BP, jeho
             * zápisy do byte backstopu akcí BP nepatří (bp_action.h). */
            if ( op == DBGAPI_TRACE_OP_START || op == DBGAPI_TRACE_OP_STOP )
            {
                bp_action_trace_channel_forget ( (int) p->channel );
            }
            p->out_result = rc;
            rq->success = ( rc == 0 );
            break;
        }

        /* --- Video záznam (video-capture Task 15) ---
         * Logika v dbgapi_videorec_execute(); handler jen validuje param. */
        case DBGAPI_CMD_VIDEOREC:
        {
            st_DBGAPI_VIDEOREC_PARAM *p = (st_DBGAPI_VIDEOREC_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            rq->success = dbgapi_videorec_execute ( p );
            break;
        }

        /* --- Historie a aktivita I/O (ui-thread-writes T1) ---
         * Ring io_history a tabulku g_io_activity plní emu vlákno
         * v port_* / memory_*_with_logging_cb. Mutace z okna I/O Ports
         * proto běží tady (drain fronty = mezi instrukcemi), ne na UI
         * vlákně. UI vlákno během příkazu synchronně čeká, takže ring
         * souběžně nečte. */
        case DBGAPI_CMD_IO_HISTORY_SET_CAPACITY:
        {
            st_DBGAPI_IO_HISTORY_CAPACITY_PARAM *p =
                (st_DBGAPI_IO_HISTORY_CAPACITY_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            /* Clamp do [MIN..MAX] dělá io_history_set_capacity; ring
             * realokuje a zahodí dosavadní události. */
            io_history_set_capacity ( (size_t) p->capacity );
            if ( g_io_history.events != NULL )
            {
                p->capacity_after = (uint32_t) g_io_history.capacity;
                rq->success = true;
            }
            else
            {
                /* calloc selhal - ring je bez bufferu; io_history_record
                 * se ho při dalším záznamu pokusí alokovat s výchozí
                 * kapacitou (auto-init). */
                p->capacity_after = 0;
                rq->success = false;
            };
            break;
        }

        case DBGAPI_CMD_IO_HISTORY_CLEAR:
            io_history_clear ( );
            rq->success = true;
            break;

        case DBGAPI_CMD_IO_ACTIVITY_RESET:
            io_activity_reset_all ( );
            rq->success = true;
            break;

        /* ui-thread-writes T1b - reset aktivity jednoho portu z kontextového
         * menu okna I/O Ports (8-bit = 256 high-byte slotů, jinak 1 slot). */
        case DBGAPI_CMD_IO_ACTIVITY_RESET_PORT:
        {
            const st_DBGAPI_IO_ACTIVITY_RESET_PORT_PARAM *p =
                (const st_DBGAPI_IO_ACTIVITY_RESET_PORT_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            if ( p->is_8bit )
            {
                io_activity_reset_port_8bit ( (uint8_t) ( p->port & 0xFFu ) );
            }
            else
            {
                io_activity_reset_port ( p->port );
            };
            rq->success = true;
            break;
        }

        /* --- Hromadné operace s breakpointy (ui-thread-writes T2) ---
         * breakpoints_clear_all a breakpoints_load_from_filepath uvolní
         * stringy a AST BP, zkrátí pole g_breakpoints a vyčistí bptmap;
         * emu vlákno tato data čte při vyhodnocení BP. Běží proto tady
         * (drain fronty = mezi instrukcemi), ne na UI vlákně. Po operaci
         * přepočet gatingu logging callbacků jako u ostatních BP mutací
         * (přímé volání z UI ho dřív vynechávalo). */
        case DBGAPI_CMD_BP_CLEAR_ALL:
            breakpoints_clear_all ( );
            dbgapi_bp_recompute_cb_gating ( );
            rq->success = true;
            break;

        case DBGAPI_CMD_BP_LOAD_FILE:
        {
            const st_DBGAPI_BP_LOAD_FILE_PARAM *p =
                (const st_DBGAPI_BP_LOAD_FILE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            /* Načtení vždy nejdřív smaže stávající data; neexistující nebo
             * nečitelný soubor = prázdný stav (sémantika loaderu). */
            if ( p->filepath && p->filepath[0] )
            {
                breakpoints_load_from_filepath ( p->filepath );
            }
            else
            {
                breakpoints_load_from_file ( );
            };
            dbgapi_bp_recompute_cb_gating ( );
            rq->success = true;
            break;
        }

        /* --- Memory Heatmap (ui-thread-writes T3) ---
         * Countery g_mhmap inkrementuje emu vlákno v logging callbaccích;
         * reset regionu a Add / Sub importovaných dat proto běží tady
         * (drain fronty = mezi instrukcemi), ne na UI vlákně. Režim se
         * nemění, callbacky se nepřepínají. */
        case DBGAPI_CMD_MHMAP_RESET_REGION:
        {
            const st_DBGAPI_MHMAP_RESET_REGION_PARAM *p =
                (const st_DBGAPI_MHMAP_RESET_REGION_PARAM *) rq->data_ptr;
            rq->success = ( p != NULL ) && mhmap_reset_region ( p->region_index );
            break;
        }

        case DBGAPI_CMD_MHMAP_MERGE:
        {
            const st_DBGAPI_MHMAP_MERGE_PARAM *p =
                (const st_DBGAPI_MHMAP_MERGE_PARAM *) rq->data_ptr;
            if ( !p || p->src_size != sizeof ( st_MHMAP ) )
            {
                rq->success = false;
                break;
            };
            rq->success = mhmap_merge ( (const st_MHMAP *) p->src,
                                        (en_MHMAP_MERGE_OP) p->op );
            break;
        }

        /* --- Memory Map: banking a DMD (ui-thread-writes T4) ---
         * Okno Memory Map dřív zapisovalo g_memory.map / g_gdg.regDMD
         * a volalo memory_reconnect_ram() přímo z UI vlákna souběžně
         * s přístupy CPU do paměti. Pořadí a sémantika viz
         * st_DBGAPI_MEMMAP_SET_PARAM. */
        case DBGAPI_CMD_MEMMAP_SET:
        {
            st_DBGAPI_MEMMAP_SET_PARAM *p =
                (st_DBGAPI_MEMMAP_SET_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
#if MZARCH != 800
            /* DMD registr má jen GDG MZ-800. */
            if ( p->dmd_write )
            {
                rq->success = false;
                break;
            };
#endif
            g_memory.map = (uint8_t) ( ( g_memory.map
                                         & (uint8_t) ~p->map_clear_mask )
                                       | p->map_set_mask );
#if MZARCH == 800
            if ( p->dmd_write )
            {
                /* Stejná cesta jako OUT (CEh) vč. CTC0 GATE0, latche
                 * MZ-700 a fast-path, ale bez hwlog / HWE BP - viz
                 * dokumentace parametru. Až po zápisu mapy: fast-path
                 * rebuild uvnitř čte už novou g_memory.map. */
                gdg_debug_set_regDMD ( p->dmd_value );
            };
#endif
            /* Přepojení RAM ukazatelů + (MZ-800) přepočet fast-path CPU. */
            memory_reconnect_ram ( );
            debugger_screen_refresh_if_enabled ( );

            p->map_after = g_memory.map;
#if MZARCH == 800
            p->dmd_after = (uint8_t) g_gdg.regDMD;
#else
            p->dmd_after = 0;
#endif
            rq->success = true;
            break;
        }

        /* --- Event Viewer: režim a import (ui-thread-writes T5a) ---
         * Okno Events dřív obojí volalo přímo z UI vlákna souběžně
         * s eventlog_record() (import = free + calloc a fread do ringu,
         * do kterého emu vlákno zapisuje). Tady běží v drainu fronty,
         * tedy mezi instrukcemi; UI vlákno během příkazu synchronně čeká. */
        case DBGAPI_CMD_EVENTLOG_SET_MODE:
        {
            st_DBGAPI_EVENTLOG_MODE_PARAM *p =
                (st_DBGAPI_EVENTLOG_MODE_PARAM *) rq->data_ptr;
            if ( !p || p->mode > (uint32_t) EVENTLOG_MODE_ALWAYS )
            {
                rq->success = false;
                break;
            };
            g_eventlog_config.mode = (en_EVENTLOG_MODE) p->mode;
            /* Parametr debugger_active eventlog nepoužívá (parita API s tlog). */
            eventlog_recompute_active ( 0 );
            p->active_after = g_eventlog_active ? 1u : 0u;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_EVENTLOG_IMPORT_FILE:
        {
            st_DBGAPI_EVENTLOG_IMPORT_PARAM *p =
                (st_DBGAPI_EVENTLOG_IMPORT_PARAM *) rq->data_ptr;
            if ( !p || !p->path || !p->path[0] )
            {
                rq->success = false;
                break;
            };
            p->rc = (int32_t) eventlog_import_from_file ( p->path );
            p->count_after = (uint32_t) g_eventlog.count;
            p->capacity_after = (uint32_t) g_eventlog.capacity;
            rq->success = ( p->rc == 0 );
            break;
        }

        /* --- Triggery okna Events (ui-thread-writes T5c) ---
         * Filtr triggeru vyhodnocuje callback v eventlog_record() na emu
         * vlákně. Výměna filtru, jména a gate tady v drainu fronty proto
         * nikdy nepotká rozběhnuté vyhodnocení; starý filtr dostane zpět
         * odesílatel a uvolní ho po návratu synchronního submitu. */
        case DBGAPI_CMD_EVENTLOG_TRIGGER_SET:
        {
            st_DBGAPI_EVENTLOG_TRIGGER_PARAM *p =
                (st_DBGAPI_EVENTLOG_TRIGGER_PARAM *) rq->data_ptr;
            if ( !p || p->kind >= (uint32_t) EVENTLOG_TRIGGER_COUNT )
            {
                rq->success = false;
                break;
            };
            st_EVENTLOG_FILTER *old = NULL;
            /* Při neúspěchu set nic nezmění a filter zůstává odesílateli. */
            rq->success = eventlog_trigger_set ( (en_EVENTLOG_TRIGGER_KIND) p->kind,
                                                 (st_EVENTLOG_FILTER *) p->filter,
                                                 p->name, &old );
            if ( rq->success ) p->old_filter = old;
            break;
        }

        case DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES:
        {
            const uint32_t *kind = (const uint32_t *) rq->data_ptr;
            rq->success = ( kind != NULL )
                          && *kind < (uint32_t) EVENTLOG_TRIGGER_COUNT
                          && eventlog_trigger_clear_matches ( (en_EVENTLOG_TRIGGER_KIND) *kind );
            break;
        }

        /* --- Freeze Bytes (ui-thread-writes T6a) ---
         * Tabulku čte freeze_apply_all() na emu vlákně jednou za snímek.
         * Memory Browser ji dřív měnil přímo z UI vlákna (bez synchronizace
         * zápisu polí slotu a in_use); tady běží v drainu fronty. */
        case DBGAPI_CMD_FREEZE_ADD:
        case DBGAPI_CMD_FREEZE_REMOVE:
        {
            st_DBGAPI_FREEZE_PARAM *p = (st_DBGAPI_FREEZE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            if ( ( rq->cmd & DBGAPI_CMD_MASK ) == DBGAPI_CMD_FREEZE_ADD )
            {
                p->result = freeze_add ( p->region_kind, p->sub_id,
                                         p->offset, p->value );
            }
            else
            {
                p->result = freeze_remove ( p->region_kind, p->sub_id,
                                            p->offset );
            };
            rq->success = p->result;
            break;
        }

        /* --- Výpis Freeze Bytes (memsearch, MCP freeze_list) ---
         * Tabulku mění jen emu vlákno (FREEZE_ADD / _REMOVE výše), kopie
         * v drainu je proto konzistentní. */
        case DBGAPI_CMD_FREEZE_LIST:
        {
            st_DBGAPI_FREEZE_LIST_PARAM *p = (st_DBGAPI_FREEZE_LIST_PARAM *) rq->data_ptr;
            G_STATIC_ASSERT ( FREEZE_MAX_ENTRIES <= G_N_ELEMENTS ( p->items ) );
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_count = 0;
            for ( size_t i = 0; i < FREEZE_MAX_ENTRIES; i++ )
            {
                const st_FREEZE_ENTRY *e = freeze_get_slot ( i );
                if ( !e ) continue;
                st_DBGAPI_FREEZE_LIST_ITEM *it = &p->items[ p->out_count++ ];
                it->region_kind = e->region_kind;
                it->sub_id = e->sub_id;
                it->offset = e->offset;
                it->value = e->value;
            };
            rq->success = true;
            break;
        }

        /* --- Běh do času v taktech GDG (replay-goto) ---
         * Zastavení na začátku první instrukce s časem >= cíl provede
         * breakpoints_enforce_global() přes bránu GLOBAL (run_control.h). */
        case DBGAPI_CMD_RUN_UNTIL_TICKS:
        {
            st_DBGAPI_RUN_UNTIL_TICKS_PARAM *p = (st_DBGAPI_RUN_UNTIL_TICKS_PARAM *) rq->data_ptr;
            st_DBGAPI_RUN_START_RESULT *r = (st_DBGAPI_RUN_START_RESULT *) rq->result_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->now_ticks = gdg_get_total_ticks ( );
            p->reached_now = false;
            if ( !EMULATOR_TEST_PAUSED )
            {
                if ( r )
                {
                    r->status = DBGAPI_RUN_TO_PAUSED_ONLY;
                    r->step = false;
                    r->run_seq = run_log_current ( NULL );
                };
                emulator_pause ( true );
                rq->success = true;
                break;
            };
            if ( p->now_ticks >= p->target_ticks )
            {
                /* V pauze stojí emulace na začátku instrukce: cíl dosažen. */
                p->reached_now = true;
                if ( r )
                {
                    r->status = DBGAPI_RUN_TO_STARTED;
                    r->step = false;
                    r->run_seq = run_log_current ( NULL );
                };
                rq->success = true;
                break;
            };
            if ( r )
            {
                r->status = DBGAPI_RUN_TO_STARTED;
                r->step = false;
                r->run_seq = run_control_seq_for_start ( );
            };
            run_control_stop_at_arm ( p->target_ticks, EMU_PAUSE_REASON_RUN_UNTIL_TICKS );
            emulator_pause ( false );
            rq->success = true;
            break;
        }

        /* --- Deadline běhu v emulovaném čase (run-control) ---
         * MCP ho nastaví v pauze těsně před příkazem, který spustí běh,
         * a při selhání toho příkazu ho zruší. Platí do nejbližší pauzy. */
        case DBGAPI_CMD_RUN_DEADLINE:
        {
            st_DBGAPI_RUN_DEADLINE_PARAM *p = (st_DBGAPI_RUN_DEADLINE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->ticks = 0;
            if ( p->arm )
            {
                p->valid = run_control_deadline_arm ( (en_RUN_DEADLINE_KIND) p->kind, p->value, &p->ticks );
            }
            else
            {
                run_control_deadline_clear ( );
                p->valid = true;
            };
            rq->success = p->valid;
            break;
        }

        /* --- Hash obrazu a VRAM bez přenosu dat (run-control) ---
         * Zdroj obrazu stejný jako u screenshotu (SDL snímek, jinak živý
         * framebuffer GDG). */
        case DBGAPI_CMD_FRAME_HASH:
        {
            st_DBGAPI_FRAME_HASH_PARAM *p = (st_DBGAPI_FRAME_HASH_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            APP_MUTEX_LOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            if ( g_iface_video->fbsnapshot_pixels != NULL )
            {
                dbgapi_frame_hash_fill ( p, g_iface_video->fbsnapshot_pixels,
                                         (uint8_t) SCREENSHOT_SRC_SDL_SNAPSHOT,
                                         g_iface_video->fbsnapshot_screen_id );
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            }
            else
            {
                uint32_t screen_id = g_iface_video->fbsnapshot_screen_id;
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
                dbgapi_frame_hash_fill ( p, g_framebuffer.pixels,
                                         (uint8_t) SCREENSHOT_SRC_GDG_LIVE, screen_id );
            };
            rq->success = true;
            break;
        }

        /* --- Vzorek prostoru hledání (memsearch) ---
         * Drain běží na hranici snímku nebo v pauze, všechny spany se tak
         * zkopírují ve stejném okamžiku emulace. Čtení regionů je bez
         * vedlejšího efektu (dbgapi_regions_read). */
        case DBGAPI_CMD_MEMSEARCH_SAMPLE:
        {
            st_DBGAPI_MEMSEARCH_SAMPLE_PARAM *p = (st_DBGAPI_MEMSEARCH_SAMPLE_PARAM *) rq->data_ptr;
            rq->success = ( p != NULL )
                          && memsearch_sample_fill ( (const st_MEMSEARCH_SPAN *) p->spans,
                                                     p->span_count,
                                                     (st_MEMSEARCH_SAMPLE *) p->sample );
            break;
        }

        /* --- Konzistentní čtení stavu (observe-cas M10) ---
         * Registry, příznaky, čas, číslo běhu, okna paměti, hashe částí
         * a mapa paměti z jednoho průchodu fronty: drain na hranici
         * snímku nebo v pauze (na hranici akce BP se nevyřizuje). Okna se
         * čtou bez vedlejších efektů, hash oken počítá odesílatel. */
        case DBGAPI_CMD_STATE_OBSERVE:
        {
            st_DBGAPI_STATE_OBSERVE_PARAM *p = (st_DBGAPI_STATE_OBSERVE_PARAM *) rq->data_ptr;
            rq->success = ( p != NULL ) && dbgapi_state_observe_fill ( p );
            break;
        }

        /* --- Výpis zařízení devbus (dev-api D1) ---
         * Dynamický stav (připojení, volby, MMIO masky) čte emu vlákno;
         * neměnný deskriptor si volající přečte sám. */
        case DBGAPI_CMD_DEVBUS_LIST:
        {
            st_DBGAPI_DEVBUS_LIST_PARAM *p = (st_DBGAPI_DEVBUS_LIST_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->initialized = devbus_is_initialized ( ) ? 1 : 0;
            p->count = devbus_count ( );
            for ( unsigned i = 0; i < p->count && i < DEVBUS_MAX_DEVICES; i++ )
            {
                const st_DEVBUS_DEVICE_DESC *d = devbus_desc ( (int) i );
                p->dev[ i ].slot = (int) i;
                p->dev[ i ].connected = devbus_is_connected ( (int) i ) ? 1 : 0;
                for ( unsigned o = 0; o < d->option_count; o++ )
                    p->dev[ i ].opt[ o ] = devbus_option_get ( (int) i, o );
                for ( unsigned m = 0; m < d->mmio_count; m++ )
                    p->dev[ i ].mmio_when[ m ] = devbus_mmio_get_when ( (int) i, m );
            };
            rq->success = true;
            break;
        }

        /* --- Snímek sekundárního výstupu zařízení devbus (kp80 D4) ---
         * Kopie posledního předaného snímku přes devbus_vout_read_last()
         * (druhý čtecí kanál emu vlákna, okno GUI čte nezávisle přes
         * fetch). Neznámý výstup = found 0; odpojené zařízení nebo zatím
         * žádný snímek = available 0. */
        case DBGAPI_CMD_DEVBUS_VOUT_READ:
        {
            st_DBGAPI_DEVBUS_VOUT_READ_PARAM *p = (st_DBGAPI_DEVBUS_VOUT_READ_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->output[ sizeof ( p->output ) - 1 ] = '\0';
            p->found = 0;
            p->connected = 0;
            p->available = 0;
            p->slot = -1;
            p->out_vout = NULL;
            p->seq = 0;
            p->pixels = NULL;
            st_DEVBUS_VOUT *o = NULL;
            for ( unsigned i = 0; i < devbus_vout_count ( ); i++ )
            {
                st_DEVBUS_VOUT *c = devbus_vout_at ( i );
                if ( p->output[ 0 ] )
                {
                    if ( strcmp ( devbus_vout_desc ( c )->id, p->output ) == 0 )
                    {
                        o = c;
                        break;
                    };
                }
                else if ( devbus_is_connected ( devbus_vout_slot ( c ) ) )
                {
                    o = c;
                    break;
                };
            };
            if ( o )
            {
                const st_DEVBUS_VOUT_DESC *vd = devbus_vout_desc ( o );
                size_t size = (size_t) vd->width * vd->height;
                p->found = 1;
                p->out_vout = o;
                p->slot = devbus_vout_slot ( o );
                p->connected = devbus_is_connected ( p->slot ) ? 1 : 0;
                if ( p->connected )
                {
                    p->pixels = g_malloc ( size );
                    if ( devbus_vout_read_last ( o, p->pixels, size, &p->seq ) )
                    {
                        p->available = 1;
                    }
                    else
                    {
                        g_free ( p->pixels );
                        p->pixels = NULL;
                    };
                };
            };
            rq->success = true;
            break;
        }

        /* --- Otisk stroje po částech (machine-hash M19) ---
         * Čte stav emulátoru bez zámku, proto v emu vlákně; stav nemění. */
        case DBGAPI_CMD_MACHINE_HASH:
        {
            st_DBGAPI_MACHINE_HASH_PARAM *p = (st_DBGAPI_MACHINE_HASH_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            gint64 t0 = g_get_monotonic_time ( );
            machine_hash_compute ( p->mask, p->flags, &p->result );
            p->elapsed_us = (uint64_t) ( g_get_monotonic_time ( ) - t0 );
            rq->success = true;
            break;
        }

        /* --- Reset počítadla zásahů BP (ui-thread-writes T6b) ---
         * hits++ a test hit_count dělá emu vlákno při vyhodnocení BP;
         * reset z editačního panelu proto běží tady (drain fronty = mezi
         * instrukcemi). Neexistující ID = neúspěch. */
        case DBGAPI_CMD_BP_RESET_HITS:
        {
            const int *p_id = (const int *) rq->data_ptr;
            if ( !p_id || !breakpoints_find_by_id ( *p_id ) )
            {
                rq->success = false;
                break;
            };
            breakpoints_reset_hits ( *p_id );
            rq->success = true;
            break;
        }

        /* --- Test Eval editačního panelu BP (M7 ui-thread-rest) ---
         * Výraz čte registry, paměť a porty, proto ho vyhodnotí emu vlákno
         * (drain fronty = mezi instrukcemi nebo v pauze). Kontext jako
         * u vyhodnocení BP mimo zásah: breakpoints_fill_global_ctx(),
         * self_id -1; no_side_effects = znak `!` se ignoruje (Test Eval
         * nesmí spustit MEM_R BP ani číst port se skutečným efektem).
         * Chyba parseru je úspěšně vyřízený příkaz s out_ok false. */
        case DBGAPI_CMD_BP_TEST_EVAL:
        {
            st_DBGAPI_BP_TEST_EVAL_PARAM *p =
                (st_DBGAPI_BP_TEST_EVAL_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            bp_expr_ctx_t ctx;
            bp_expr_ctx_zero ( &ctx );
            breakpoints_fill_global_ctx ( &ctx );
            ctx.self_id = -1;
            ctx.no_side_effects = true;
            p->out_value = 0;
            p->out_error[ 0 ] = '\0';
            p->out_ok = bp_expr_parse_eval ( p->expr_text ? p->expr_text : "",
                                             &ctx, &p->out_value,
                                             p->out_error, sizeof ( p->out_error ) );
            if ( !p->out_ok ) p->out_value = 0;
            rq->success = true;
            break;
        }

        /* --- Callstack (ui-thread-writes T6c) ---
         * Shadow stack mění emu vlákno v CALL/RET hoocích; zapnutí /
         * vypnutí (registrace Z80 hooků) i vyprázdnění proto běží tady
         * (drain fronty = mezi instrukcemi), ne na UI vlákně. */
        case DBGAPI_CMD_CALLSTACK_SET_ACTIVE:
        {
            st_DBGAPI_CALLSTACK_SET_ACTIVE_PARAM *p =
                (st_DBGAPI_CALLSTACK_SET_ACTIVE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            callstack_set_active ( p->active != 0 );
            p->active_after = g_callstack_active ? 1u : 0u;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_CALLSTACK_RESET:
            callstack_reset ( );
            rq->success = true;
            break;

        /* --- Vynucený refresh obrazovky (ui-thread-writes T6d) ---
         * Framebuffer plní a snímky dokončuje emu vlákno; Ctrl+R / menu
         * debuggeru proto refresh posílají sem (drain fronty = mezi
         * instrukcemi, v pauze hned). */
        case DBGAPI_CMD_SCREEN_REFRESH:
            mzarch_forced_full_screen_refresh ( );
            rq->success = true;
            break;

        /* --- Media Tools (mutant mcp-server V1.B.1) --- */
        case DBGAPI_CMD_MEDIA_LOAD_MZF:
        {
            /* Plný CMT hack load (fix mzdos 0008): header + post-header
             * mapping + body. Zrcadlí kanonickou sekvenci z
             * mzarch_bootstrap_run_mzf() (bootstrap.c:61-98), ALE:
             *
             *   - NEvolá mzarch_bootstrap_init() (= destruktivní reset
             *     8255/CTC/PIO). media_load_mzf je load mid-session, ne boot.
             *   - NEnastaví SP (= bootstrap to dělá jako run-prep, my jsme
             *     load-only).
             *   - NEnastaví PC (= práce volajícího; spuštění v definovaném
             *     stavu dělá DBGAPI_CMD_MEDIA_RUN_MZF).
             *
             * Mapping: na MZ-800 je po resetu header buffer 0x10F0 mapovaný
             * na CG-ROM (= ROM_1000), takže nejdřív uložíme g_memory.map a
             * nastavíme load-time map přes mzarch_platform_bootstrap_apply_load_map()
             * (= RAM na 0x10F0). Pro tělo s fstrt < 0x1000 navíc odmapujeme
             * dolní ROM přes mzarch_platform_load_prepare_body_map() (= jen
             * Bod 1 z post_header, BEZ mz800 GDG/DMD video přepnutí, aby
             * media_load_mzf nezměnil video mód běžícího programu). Na konci
             * g_memory.map obnovíme = load-only primitiv bez banking side
             * efektu.
             *
             * CPU scratch registry HL/BC/AF uložíme a obnovíme kolem load
             * sekvence (load mechanismus je klobruje - HL=adresa, BC=size,
             * AF=CARRY z cmthack_result), aby media_load_mzf neměl vedlejší
             * efekt na CPU stav.
             *
             * Detekce selhání: cmthack funkce nevrací status, ale nastavují
             * CARRY flag v AF (cmthack_result, cmthack.c:131). Fázi 1
             * navíc poznáme přes g_cmthack.mzf_handler.status READY bit
             * (= při selhání cmthack handler zavře). Fázi 2 přes CARRY z AF
             * čtený PŘED restorem. */
            st_DBGAPI_MEDIA_PARAM *p =
                (st_DBGAPI_MEDIA_PARAM *) rq->data_ptr;
            if ( !p || !p->filepath || p->filepath[0] == '\0' )
            {
                if ( p )
                {
                    p->out_result = -1;
                    p->out_size = 0;
                };
                rq->success = false;
                break;
            };

            /* Uložit CPU scratch registry pro pozdější restore. */
            uint16_t saved_af = z80_get_reg ( g_mzarch_main.cpu, Z80_REG_AF );
            uint16_t saved_hl = z80_get_reg ( g_mzarch_main.cpu, Z80_REG_HL );
            uint16_t saved_bc = z80_get_reg ( g_mzarch_main.cpu, Z80_REG_BC );

            /* Uložit aktuální memory map a nastavit kanonickou load-time map.
             * Bez toho je mid-session na MZ-800 header buffer 0x10F0 mapovaný
             * na CG-ROM (= ROM_1000) a cmthack MAPED zápis hlavičky se ztratí
             * (header garbage -> body load selže). bootstrap.c tento problém
             * nemá, protože mu předchází mzarch_bootstrap_init() s touto mapou.
             * Mapu obnovíme na konci (load-only primitiv = bez trvalého
             * banking side efektu). */
            unsigned saved_map = g_memory.map;
            mzarch_platform_bootstrap_apply_load_map ();

            /* --- Fáze 1: hlavička do RAM 0x10f0 ----------------------- */
            z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, 0x10f0 );
            cmthack_load_mzf_filename ( p->filepath );

            if ( !( g_cmthack.mzf_handler.status & HANDLER_STATUS_READY ) )
            {
                /* Soubor nešel otevřít nebo vadná hlavička - cmthack handler
                 * zavřel. Obnovit map + registry a vrátit chybu. */
                g_memory.map = saved_map;
                z80_set_reg ( g_mzarch_main.cpu, Z80_REG_AF, saved_af );
                z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, saved_hl );
                z80_set_reg ( g_mzarch_main.cpu, Z80_REG_BC, saved_bc );
                p->out_result = -2;
                p->out_size = 0;
                rq->success = false;
                break;
            };

            /* Přečíst 128B hlavičku zpět z RAM 0x10f0 (DEBUGGER větev jako
             * bootstrap.c:67-71). MCP build má debugger enabled. */
            st_MZF_HEADER mzf_header;
            for ( size_t i = 0; i < sizeof ( st_MZF_HEADER ); i++ )
            {
                uint8_t *hp = (uint8_t *) &mzf_header + i;
                *hp = debugger_memory_read_byte ( (uint16_t) ( 0x10f0 + i ) );
            };

            /* --- Body mapping (PŘED body) ----------------------------- */
            /* Jen odmapování dolní ROM pro fstrt < 0x1000 (= "Bod 1" z
             * post_header), BEZ mz800 GDG/DMD video přepnutí - to je
             * boot-prep, ne mid-session load. */
            mzarch_platform_load_prepare_body_map ( mzf_header.fstrt );

            /* --- Fáze 2: tělo do RAM na fstrt ------------------------- */
            z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, mzf_header.fstrt );
            z80_set_reg ( g_mzarch_main.cpu, Z80_REG_BC, mzf_header.fsize );
            cmthack_read_mzf_body ();

            /* CARRY flag (bit 0 AF) = signál selhání fáze 2 (cmthack_result
             * LOADRET_ERROR/BREAK). Číst PŘED restorem AF. */
            uint16_t af_after = z80_get_reg ( g_mzarch_main.cpu, Z80_REG_AF );
            bool body_failed = ( af_after & 0x01 ) != 0;

            /* Obnovit memory map + CPU scratch registry (load primitiv bez
             * trvalého banking/CPU side efektu). Body už je v RAM. */
            g_memory.map = saved_map;
            z80_set_reg ( g_mzarch_main.cpu, Z80_REG_AF, saved_af );
            z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, saved_hl );
            z80_set_reg ( g_mzarch_main.cpu, Z80_REG_BC, saved_bc );

            p->out_load_addr = mzf_header.fstrt;
            p->out_exec_addr = mzf_header.fexec;
            p->out_size = mzf_header.fsize;

            if ( body_failed )
            {
                p->out_result = -3;
                rq->success = false;
                break;
            };

            p->out_result = 0;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_MEDIA_RUN_MZF:
        {
            /* Spuštění MZF v definovaném stavu: reset stroje + bootstrap
             * jako CLI --run-mzf (mzarch_main_reset_and_run_mzf). Na rozdíl
             * od MEDIA_LOAD_MZF výsledek nezávisí na předchozím stavu stroje
             * (Z80 PIO, IM, paleta, CTC, ...).
             *
             * Soubor se ověří PŘED resetem (čitelná hlavička, tělo
             * o délce fsize za ní), aby chybná cesta nebo zkrácený soubor
             * nezresetovaly běžící program. Kódy out_result jako LOAD_MZF,
             * navíc -4 = soubor kratší než 128 + fsize (zjištěno před
             * resetem). -3 zde znamená selhání CMT hacku až po resetu.
             *
             * Reset v drainu fronty je stejná situace jako načtení snapshotu
             * (SNAPSHOT_LOAD_*): celý stav stroje se nahradí mezi
             * instrukcemi. */
            st_DBGAPI_MEDIA_PARAM *p =
                (st_DBGAPI_MEDIA_PARAM *) rq->data_ptr;
            if ( !p || !p->filepath || p->filepath[0] == '\0' )
            {
                if ( p )
                {
                    p->out_result = -1;
                    p->out_size = 0;
                };
                rq->success = false;
                break;
            };

            /* Hlavička a délka souboru. Pole hlavičky jsou little-endian. */
            uint8_t raw_hdr[sizeof ( st_MZF_HEADER )];
            bool hdr_ok = false;
            gint64 file_len = -1;
            FILE *fp = g_fopen ( p->filepath, "rb" );
            if ( fp )
            {
                hdr_ok = ( fread ( raw_hdr, 1, sizeof ( raw_hdr ), fp )
                           == sizeof ( raw_hdr ) );
                if ( fseek ( fp, 0, SEEK_END ) == 0 )
                {
                    file_len = (gint64) ftell ( fp );
                };
                fclose ( fp );
            };
            if ( !hdr_ok )
            {
                p->out_result = -2;
                p->out_size = 0;
                rq->success = false;
                break;
            };
            uint16_t fsize = (uint16_t) ( raw_hdr[offsetof ( st_MZF_HEADER, fsize )]
                             | ( raw_hdr[offsetof ( st_MZF_HEADER, fsize ) + 1] << 8 ) );
            uint16_t fstrt = (uint16_t) ( raw_hdr[offsetof ( st_MZF_HEADER, fstrt )]
                             | ( raw_hdr[offsetof ( st_MZF_HEADER, fstrt ) + 1] << 8 ) );
            uint16_t fexec = (uint16_t) ( raw_hdr[offsetof ( st_MZF_HEADER, fexec )]
                             | ( raw_hdr[offsetof ( st_MZF_HEADER, fexec ) + 1] << 8 ) );
            p->out_load_addr = fstrt;
            p->out_exec_addr = fexec;
            p->out_size = fsize;
            if ( file_len < (gint64) ( sizeof ( st_MZF_HEADER ) + fsize ) )
            {
                p->out_result = -4;
                rq->success = false;
                break;
            };

            mzarch_main_reset_and_run_mzf ( p->filepath );

            /* CARRY v AF = selhání CMT hacku (cmthack_result). Bootstrap po
             * načtení těla AF nemění. */
            if ( z80_get_reg ( g_mzarch_main.cpu, Z80_REG_AF ) & 0x01 )
            {
                p->out_result = -3;
                rq->success = false;
                break;
            };

            p->out_result = 0;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_MEDIA_LOAD_BINARY:
        {
            /* Raw binary load do Z80 paměti. Otevře soubor přes stdio,
             * zapíše bajt po bajtu přes debugger_memory_write_byte
             * (banking-aware, ale bez region checks). Velikost je dáná
             * souborem; kontrolujeme jen že se vejde do 0..65535 od
             * load_addr. */
            st_DBGAPI_MEDIA_PARAM *p =
                (st_DBGAPI_MEDIA_PARAM *) rq->data_ptr;
            if ( !p || !p->filepath || p->filepath[0] == '\0' )
            {
                if ( p ) p->out_result = -1;
                rq->success = false;
                break;
            };
            FILE *fp = g_fopen ( p->filepath, "rb" );
            if ( !fp )
            {
                p->out_result = -2;
                rq->success = false;
                break;
            };
            uint32_t addr = p->load_addr;
            uint32_t written = 0;
            int ch;
            while ( ( ch = fgetc ( fp ) ) != EOF )
            {
                if ( addr > 0xFFFF )
                {
                    /* Přetečení 64 KB - skončíme s warning, ale zatím
                     * úspěch nad částí, která se vešla. */
                    break;
                };
                debugger_memory_write_byte ( (uint16_t) addr, (uint8_t) ch );
                addr++;
                written++;
            };
            fclose ( fp );
            p->out_size = written;
            p->out_result = 0;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_MEDIA_INSERT:
        {
            /* Insert image do slotu. Dispatch podle slot enum. Path musí
             * být non-empty (= "" znamená eject; pro to existuje EJECT
             * CMD samostatně). */
            st_DBGAPI_MEDIA_PARAM *p =
                (st_DBGAPI_MEDIA_PARAM *) rq->data_ptr;
            if ( !p || !p->filepath || p->filepath[0] == '\0' )
            {
                if ( p ) p->out_result = -1;
                rq->success = false;
                break;
            };
            int rc = -1;
            /* Mount mění cesty a buffery, které čte menu zařízení v UI
             * vlákně pod stejným zámkem (device_request.h). */
            device_state_lock();
            switch ( p->slot )
            {
                case DBGAPI_MEDIA_SLOT_CMT:
                {
                    /* cmt_open_file_by_extension nemodifikuje argument,
                     * ale signatura ho deklaruje jako (char*) - cast. */
                    rc = cmt_open_file_by_extension ( (char *) p->filepath );
                    /* cmt API vrací 0=OK, nenula=error (interpretace
                     * závisí na implementaci). */
                    break;
                };
#if CFG_HWEXT_HAVE_FDC
                case DBGAPI_MEDIA_SLOT_FDC0_FD0:
                case DBGAPI_MEDIA_SLOT_FDC0_FD1:
                case DBGAPI_MEDIA_SLOT_FDC0_FD2:
                case DBGAPI_MEDIA_SLOT_FDC0_FD3:
                case DBGAPI_MEDIA_SLOT_FDC1_FD0:
                case DBGAPI_MEDIA_SLOT_FDC1_FD1:
                case DBGAPI_MEDIA_SLOT_FDC1_FD2:
                case DBGAPI_MEDIA_SLOT_FDC1_FD3:
                {
                    st_FDC *fdc = NULL;
                    unsigned drive = 0;
                    dbgapi_media_slot_to_fdc ( p->slot, &fdc, &drive );
                    fdc_mount_dskfile ( fdc, drive, (char *) p->filepath );
                    rc = FDC_TEST_DRIVE_ID_MOUNTED ( fdc, drive ) ? 0 : -3;
                    break;
                };
#else
                case DBGAPI_MEDIA_SLOT_FDC0_FD0:
                case DBGAPI_MEDIA_SLOT_FDC0_FD1:
                case DBGAPI_MEDIA_SLOT_FDC0_FD2:
                case DBGAPI_MEDIA_SLOT_FDC0_FD3:
                case DBGAPI_MEDIA_SLOT_FDC1_FD0:
                case DBGAPI_MEDIA_SLOT_FDC1_FD1:
                case DBGAPI_MEDIA_SLOT_FDC1_FD2:
                case DBGAPI_MEDIA_SLOT_FDC1_FD3:
                    rc = -10; /* FDC není v této arch sestavě */
                    break;
#endif
#if CFG_HWEXT_HAVE_QDISK
                case DBGAPI_MEDIA_SLOT_QD:
                {
                    /* Obraz QD připojí jen MZ-1F11 (typ IMAGE). Neexistující
                     * soubor odmítneme dřív, než se cokoliv změní: dialog
                     * v GUI nový obraz vytváří (qdisk_mount_path()), MCP
                     * má překlep v cestě hlásit. Jinak připojenou mechaniku
                     * přepneme stejnou posloupností jako volba MZ-1F11
                     * v menu (odpojí zavaděč Unicard v QD). */
                    if ( !g_file_test ( p->filepath, G_FILE_TEST_IS_REGULAR ) )
                    {
                        rc = -12;
                        break;
                    };
                    if ( !QDISK_TEST_CONNECTED || !QDISK_TEST_TYPE_IMAGE )
                    {
                        qdisk_apply_connection ( QDISK_REQ_MZ1F11 );
                        p->out_qd_switched = 1;
                    };
                    /* Ochrana jako přepínač v menu (uloží se do INI);
                     * otevření obrazu ji z INI převezme. */
                    qdisk_set_write_protected ( p->read_only ? 1 : 0 );
                    rc = qdisk_mount_path ( p->filepath, 0 );
                    if ( rc == 0 && g_qdisk.filename[ 0 ] == '\0' ) rc = -3;
                    break;
                };
#else
                case DBGAPI_MEDIA_SLOT_QD:
                    rc = -10;
                    break;
#endif
#if CFG_HWEXT_HAVE_IDE8
                case DBGAPI_MEDIA_SLOT_IDE8:
                {
                    rc = ide8_drive_open_image ( &g_ide8.drive[ 0 ],
                                                  (char *) p->filepath );
                    /* ide8 vrací 0 = OK. */
                    break;
                };
#else
                case DBGAPI_MEDIA_SLOT_IDE8:
                    rc = -10;
                    break;
#endif
                default:
                    rc = -1;
                    break;
            };
            device_state_unlock();
            p->out_result = rc;
            rq->success = ( rc == 0 );
            break;
        }

        case DBGAPI_CMD_MEDIA_EJECT:
        {
            st_DBGAPI_MEDIA_PARAM *p =
                (st_DBGAPI_MEDIA_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            int rc = -1;
            /* Mount mění cesty a buffery, které čte menu zařízení v UI
             * vlákně pod stejným zámkem (device_request.h). */
            device_state_lock();
            switch ( p->slot )
            {
                case DBGAPI_MEDIA_SLOT_CMT:
                    cmt_eject ( );
                    rc = 0;
                    break;
#if CFG_HWEXT_HAVE_FDC
                case DBGAPI_MEDIA_SLOT_FDC0_FD0:
                case DBGAPI_MEDIA_SLOT_FDC0_FD1:
                case DBGAPI_MEDIA_SLOT_FDC0_FD2:
                case DBGAPI_MEDIA_SLOT_FDC0_FD3:
                case DBGAPI_MEDIA_SLOT_FDC1_FD0:
                case DBGAPI_MEDIA_SLOT_FDC1_FD1:
                case DBGAPI_MEDIA_SLOT_FDC1_FD2:
                case DBGAPI_MEDIA_SLOT_FDC1_FD3:
                {
                    st_FDC *fdc = NULL;
                    unsigned drive = 0;
                    dbgapi_media_slot_to_fdc ( p->slot, &fdc, &drive );
                    fdc_umount ( fdc, drive );
                    rc = 0;
                    break;
                };
#else
                case DBGAPI_MEDIA_SLOT_FDC0_FD0:
                case DBGAPI_MEDIA_SLOT_FDC0_FD1:
                case DBGAPI_MEDIA_SLOT_FDC0_FD2:
                case DBGAPI_MEDIA_SLOT_FDC0_FD3:
                case DBGAPI_MEDIA_SLOT_FDC1_FD0:
                case DBGAPI_MEDIA_SLOT_FDC1_FD1:
                case DBGAPI_MEDIA_SLOT_FDC1_FD2:
                case DBGAPI_MEDIA_SLOT_FDC1_FD3:
                    rc = -10;
                    break;
#endif
#if CFG_HWEXT_HAVE_QDISK
                case DBGAPI_MEDIA_SLOT_QD:
                    qdisk_umount ( );
                    rc = 0;
                    break;
#else
                case DBGAPI_MEDIA_SLOT_QD:
                    rc = -10;
                    break;
#endif
#if CFG_HWEXT_HAVE_IDE8
                case DBGAPI_MEDIA_SLOT_IDE8:
                    ide8_drive_close_image ( &g_ide8.drive[ 0 ] );
                    rc = 0;
                    break;
#else
                case DBGAPI_MEDIA_SLOT_IDE8:
                    rc = -10;
                    break;
#endif
                default:
                    rc = -1;
                    break;
            };
            device_state_unlock();
            p->out_result = rc;
            rq->success = ( rc == 0 );
            break;
        }

        case DBGAPI_CMD_MEDIA_STATE:
        {
            /* Snapshot stavu všech slotů. Pořadí: CMT, FDC0, FDC1, QD, IDE8.
             * Pro nemountnuté / nepřítomné periferie inserted=0, filepath="". */
            st_DBGAPI_MEDIA_STATE_PARAM *p =
                (st_DBGAPI_MEDIA_STATE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            int idx = 0;

            /* CMT - g_cmt.ui_base_filename obsahuje aktuální cestu pokud
             * nahráno přes UI; cmt_get_ui_base_filename() vrací string. */
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_CMT;
            p->slots[ idx ].inserted = CMT_TEST_FILLED ? 1 : 0;
            p->slots[ idx ].read_only = 0;
            {
                const char *fn = cmt_get_ui_base_filename ( );
                if ( fn && fn[ 0 ] )
                {
                    strncpy ( p->slots[ idx ].filepath, fn,
                              sizeof ( p->slots[ idx ].filepath ) - 1 );
                };
            };
            idx++;

            /* FDC sloty: 2 řadiče (FDC0/FDC1) × 4 mechaniky = 8 slotů. */
            {
                static const en_DBGAPI_MEDIA_SLOT fdc_slot_ids[2][4] = {
                    { DBGAPI_MEDIA_SLOT_FDC0_FD0, DBGAPI_MEDIA_SLOT_FDC0_FD1,
                      DBGAPI_MEDIA_SLOT_FDC0_FD2, DBGAPI_MEDIA_SLOT_FDC0_FD3 },
                    { DBGAPI_MEDIA_SLOT_FDC1_FD0, DBGAPI_MEDIA_SLOT_FDC1_FD1,
                      DBGAPI_MEDIA_SLOT_FDC1_FD2, DBGAPI_MEDIA_SLOT_FDC1_FD3 },
                };
                for ( unsigned c = 0; c < 2; c++ )
                {
                    for ( unsigned d = 0; d < 4; d++ )
                    {
                        p->slots[ idx ].slot = fdc_slot_ids[ c ][ d ];
#if CFG_HWEXT_HAVE_FDC
                        st_FDC *fdc = &g_fdc[ c ];
                        int mounted = fdc_test_drive_id_mounted ( fdc, d );
                        p->slots[ idx ].inserted = mounted ? 1 : 0;
                        p->slots[ idx ].read_only = fdc->drive[ d ].readonly ? 1 : 0;
                        if ( mounted && fdc->drive[ d ].filename[ 0 ] )
                        {
                            strncpy ( p->slots[ idx ].filepath,
                                      fdc->drive[ d ].filename,
                                      sizeof ( p->slots[ idx ].filepath ) - 1 );
                        };
#endif
                        idx++;
                    };
                };
            };

#if CFG_HWEXT_HAVE_QDISK
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_QD;
            p->slots[ idx ].inserted = ( g_qdisk.filename[ 0 ] != '\0' ) ? 1 : 0;
            /* Efektivní ochrana vloženého obrazu (volba uživatele nebo
             * soubor bez práva zápisu, zavaděč Unicard vždy), ne uložená
             * volba z INI (qdisc_get_write_protected()): po vysunutí 0. */
            p->slots[ idx ].read_only = g_qdisk.readonly ? 1 : 0;
            if ( g_qdisk.filename[ 0 ] )
            {
                strncpy ( p->slots[ idx ].filepath, g_qdisk.filename,
                          sizeof ( p->slots[ idx ].filepath ) - 1 );
            };
            idx++;
#else
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_QD;
            idx++;
#endif

#if CFG_HWEXT_HAVE_IDE8
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_IDE8;
            p->slots[ idx ].inserted =
                IDE8_TEST_MASTER_CONNECTED ? 1 : 0;
            p->slots[ idx ].read_only = 0;
            {
                const char *fn = ide8_drive_get_filepath ( 0 );
                if ( fn && fn[ 0 ] )
                {
                    strncpy ( p->slots[ idx ].filepath, fn,
                              sizeof ( p->slots[ idx ].filepath ) - 1 );
                };
            };
            idx++;
#else
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_IDE8;
            idx++;
#endif

            p->count = idx;
            rq->success = true;
            break;
        }

        /* --- Platform + Config Tools (mutant mcp-server V1.B.2) --- */
        case DBGAPI_CMD_SETTINGS_GET:
        {
            /* Čte INI element. Pomocí cfgroot_get_module_by_name +
             * cfgmodule_get_element_by_name najde element a podle
             * jeho typu naplní out_value (heap-alokovaný g_strdup) a
             * out_type, u číselné vazby na proměnnou i out_live
             * (dbgapi_settings_live_value()). Caller (MCP dispatch)
             * uvolňuje out_value a out_live přes g_free. */
            st_DBGAPI_SETTINGS_PARAM *p =
                (st_DBGAPI_SETTINGS_PARAM *) rq->data_ptr;
            if ( !p || !p->module || !p->element )
            {
                if ( p ) p->out_result = -1;
                rq->success = false;
                break;
            };
            p->out_value = NULL;
            p->out_live = NULL;
            p->out_type = DBGAPI_SETTINGS_TYPE_UNKNOWN;
            CFGMOD *mod =
                cfgroot_get_module_by_name ( g_cfgmain, (char *) p->module );
            if ( !mod )
            {
                p->out_result = -2;
                rq->success = false;
                break;
            };
            st_CFGELEMENT *elm =
                cfgmodule_get_element_by_name ( mod, (char *) p->element );
            if ( !elm )
            {
                p->out_result = -3;
                rq->success = false;
                break;
            };
            p->out_live = dbgapi_settings_live_value ( elm );
            switch ( elm->type )
            {
                case CFGENTYPE_UNSIGNED:
                {
                    unsigned v = cfgelement_get_unsigned_value ( elm );
                    p->out_value = g_strdup_printf ( "%u", v );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_UNSIGNED;
                    break;
                };
                case CFGENTYPE_BOOL:
                {
                    int v = cfgelement_get_bool_value ( elm );
                    p->out_value = g_strdup ( v ? "true" : "false" );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_BOOL;
                    break;
                };
                case CFGENTYPE_TEXT:
                {
                    char *v = cfgelement_get_text_value ( elm );
                    p->out_value = g_strdup ( v ? v : "" );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_TEXT;
                    break;
                };
                case CFGENTYPE_KEYWORD:
                {
                    char *kw = cfgelement_get_keyword_by_value ( elm );
                    p->out_value = g_strdup ( kw ? kw : "" );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_KEYWORD;
                    break;
                };
                case CFGENTYPE_FLOAT:
                {
                    float v = cfgelement_get_float_value ( elm );
                    p->out_value = g_strdup_printf ( "%g", (double) v );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_FLOAT;
                    break;
                };
                default:
                    p->out_result = -3;
                    rq->success = false;
                    break;
            };
            if ( p->out_value )
            {
                p->out_result = 0;
                rq->success = true;
            }
            else
            {
                g_free ( p->out_live );
                p->out_live = NULL;
            };
            break;
        }

        case DBGAPI_CMD_SETTINGS_SET:
        {
            /* Zápis INI elementu. Před zápisem zachytí aktuální hodnotu
             * do out_value (audit / rollback). Pokud type-coerce
             * stringu selže, vrátí -4. Whitelist live-settable klíčů
             * NENÍ řešen zde (= delegováno na MCP vrstvu, která má
             * jednotný seznam).
             *
             * Samotný zápis mění jen hodnotu elementu; proměnná zařízení
             * se z ní plní jen při startu a při ukládání INI se naopak
             * element přepíše z proměnné. Po zápisu proto hodnotu
             * propagujeme (cfgelement_propagate(), stejně jako start),
             * jinak by zápis neměl účinek a při ukončení by se ztratil.
             * TEXT ne: propagate by alokoval nový řetězec bez uvolnění
             * starého (whitelist žádný TEXT klíč nemá). Povolené klíče
             * propagují jedno číslo: zesílení zvuku (float čtený audio
             * vláknem, menu ho píše z UI stejně), příznaky a FPS
             * zobrazení (int čtený UI vláknem každý snímek), meze PC
             * trasování (emu vlákno). */
            st_DBGAPI_SETTINGS_PARAM *p =
                (st_DBGAPI_SETTINGS_PARAM *) rq->data_ptr;
            if ( !p || !p->module || !p->element || !p->new_value )
            {
                if ( p ) p->out_result = -1;
                rq->success = false;
                break;
            };
            p->out_value = NULL;
            p->out_type = DBGAPI_SETTINGS_TYPE_UNKNOWN;
            CFGMOD *mod =
                cfgroot_get_module_by_name ( g_cfgmain, (char *) p->module );
            if ( !mod )
            {
                p->out_result = -2;
                rq->success = false;
                break;
            };
            st_CFGELEMENT *elm =
                cfgmodule_get_element_by_name ( mod, (char *) p->element );
            if ( !elm )
            {
                p->out_result = -3;
                rq->success = false;
                break;
            };
            /* Volba zařízení devbus: ověření a nastavení přes devbus (LIVE
             * volba připojeného zařízení se projeví hned), hodnota i do
             * elementu, aby settings_get a INI viděly totéž. */
            {
                unsigned dopt = 0;
                int dslot = devbus_option_key ( p->module, p->element, &dopt );
                if ( dslot >= 0 )
                {
                    rq->success = dbgapi_settings_set_devbus ( p, elm, dslot, dopt );
                    break;
                };
            }
            switch ( elm->type )
            {
                case CFGENTYPE_UNSIGNED:
                {
                    unsigned prev = cfgelement_get_unsigned_value ( elm );
                    p->out_value = g_strdup_printf ( "%u", prev );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_UNSIGNED;
                    char *endp = NULL;
                    unsigned long v =
                        strtoul ( p->new_value, &endp, 0 );
                    if ( !endp || *endp != '\0' )
                    {
                        p->out_result = -4;
                        rq->success = false;
                        break;
                    };
                    cfgelement_set_unsigned_value ( elm, (unsigned) v );
                    /* Live-apply range-scope filtru cputrack: cfgelement
                     * zápis sám nepropíše hodnotu do g_cputrack_config
                     * (uint16_t pole čtené hot-path hookem), proto ji
                     * propíšeme explicitně. Běžíme v emu vlákně => bezpečné. */
                    if ( !strcmp ( p->module, "TRACE_CPUTRACK" )
                         && ( !strcmp ( p->element, "pc_range_lo" )
                              || !strcmp ( p->element, "pc_range_hi" ) ) )
                    {
                        cputrack_apply_pc_range_live ( );
                    };
                    p->out_result = 0;
                    rq->success = true;
                    break;
                };
                case CFGENTYPE_BOOL:
                {
                    int prev = cfgelement_get_bool_value ( elm );
                    p->out_value = g_strdup ( prev ? "true" : "false" );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_BOOL;
                    int bv = -1;
                    if ( !g_ascii_strcasecmp ( p->new_value, "true" )
                         || !strcmp ( p->new_value, "1" ) )
                    {
                        bv = 1;
                    }
                    else if ( !g_ascii_strcasecmp ( p->new_value, "false" )
                              || !strcmp ( p->new_value, "0" ) )
                    {
                        bv = 0;
                    };
                    if ( bv < 0 )
                    {
                        p->out_result = -4;
                        rq->success = false;
                        break;
                    };
                    cfgelement_set_bool_value ( elm, bv );
                    p->out_result = 0;
                    rq->success = true;
                    break;
                };
                case CFGENTYPE_TEXT:
                {
                    char *prev = cfgelement_get_text_value ( elm );
                    p->out_value = g_strdup ( prev ? prev : "" );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_TEXT;
                    cfgelement_set_text_value ( elm, p->new_value );
                    p->out_result = 0;
                    rq->success = true;
                    break;
                };
                case CFGENTYPE_FLOAT:
                {
                    float prev = cfgelement_get_float_value ( elm );
                    p->out_value = g_strdup_printf ( "%g", (double) prev );
                    p->out_type  = DBGAPI_SETTINGS_TYPE_FLOAT;
                    char *endp = NULL;
                    float v = (float) g_ascii_strtod ( p->new_value, &endp );
                    if ( !endp || *endp != '\0' )
                    {
                        p->out_result = -4;
                        rq->success = false;
                        break;
                    };
                    cfgelement_set_float_value ( elm, v );
                    p->out_result = 0;
                    rq->success = true;
                    break;
                };
                case CFGENTYPE_KEYWORD:
                default:
                    /* KEYWORD vyžaduje znalost mapování keyword<->int,
                     * což V1.B.2 neexponuje. Whitelist v MCP vrstvě
                     * KEYWORD klíče zatím neobsahuje, takže by sem
                     * neměl dojít. Defense in depth = vrátit -4. */
                    {
                        char *kw = cfgelement_get_keyword_by_value ( elm );
                        p->out_value = g_strdup ( kw ? kw : "" );
                        p->out_type  = DBGAPI_SETTINGS_TYPE_KEYWORD;
                    };
                    p->out_result = -4;
                    rq->success = false;
                    break;
            };
            if ( p->out_result == 0 && elm->type != CFGENTYPE_TEXT )
            {
                cfgelement_propagate ( elm );
            };
            break;
        }

        case DBGAPI_CMD_PLATFORM_SET:
        {
            /* Runtime platform switch NENÍ podporován - mz800/mz700/
             * mz1500 jsou separátní binárky (= compile-time MZARCH).
             * Handler vrací out_active_kind a out_result = -10 vždy
             * kromě target == active (= no-op, out_result = 0). */
            st_DBGAPI_PLATFORM_PARAM *p =
                (st_DBGAPI_PLATFORM_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            /* Mapování compile-time MZARCH na enum: 700->1, 800->2, 1500->3. */
            int active = 0;
            switch ( g_mzarch_platform_numeric )
            {
                case 700:  active = 1; break;
                case 800:  active = 2; break;
                case 1500: active = 3; break;
                default:   active = 0; break;
            };
            p->out_active_kind = active;
            if ( p->target_kind == active )
            {
                /* No-op - cílová platforma je už aktivní. */
                p->out_result = 0;
                rq->success = true;
            }
            else
            {
                p->out_result = -10;
                rq->success = false;
            };
            break;
        }

        case DBGAPI_CMD_PERIPH_ATTACH:
        case DBGAPI_CMD_PERIPH_DETACH:
        {
            /* Připojení a odpojení za běhu stejnými funkcemi jako menu
             * Devices (viz dbgapi_periph_apply()). Menu a okna zařízení
             * čtou stav pod zámkem stavu zařízení. */
            st_DBGAPI_PERIPH_PARAM *p =
                (st_DBGAPI_PERIPH_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            device_state_lock ( );
            p->out_result = dbgapi_periph_apply ( p, cmd == DBGAPI_CMD_PERIPH_ATTACH );
            device_state_unlock ( );
            rq->success = ( p->out_result == 0 );
            break;
        }

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
        case DBGAPI_CMD_INPUT_PRESS_KEY:
        {
            /* Press klávesy v PIO8255 vkbd_matrix. Caller (= MCP
             * dispatch handler) už resolvoval key name nebo ASCII
             * znak na (col, bit, needs_shift). */
            st_DBGAPI_HID_KEY_PARAM *p =
                (st_DBGAPI_HID_KEY_PARAM *) rq->data_ptr;
            if ( !p || p->col < 0 || p->col > 9
                 || p->bit < 0 || p->bit > 7 )
            {
                rq->success = false;
                break;
            };
            hid_keymap_press ( p->col, p->bit, p->needs_shift );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_INPUT_RELEASE_KEY:
        {
            /* Release klávesy ve vkbd_matrix. SHIFT release závisí
             * na needs_shift flagu (caller rozhoduje). */
            st_DBGAPI_HID_KEY_PARAM *p =
                (st_DBGAPI_HID_KEY_PARAM *) rq->data_ptr;
            if ( !p || p->col < 0 || p->col > 9
                 || p->bit < 0 || p->bit > 7 )
            {
                rq->success = false;
                break;
            };
            hid_keymap_release ( p->col, p->bit, p->needs_shift );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_INPUT_RELEASE_ALL:
        {
            /* Vyplní celou vkbd_matrix 0xff (= všechny klávesy
             * uvolněné). Bez paramu. */
            hid_keymap_release_all();
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_INPUT_JOY_SET:
        {
            /* Nastaví g_joy.dev[port].state z user-friendly active-HIGH
             * masky (bit 0=UP / 1=DOWN / 2=LEFT / 3=RIGHT / 4=FIRE1 /
             * 5=FIRE2). hid_keymap_joystick_set provede konverzi na
             * active-LOW byte. */
            st_DBGAPI_HID_JOY_PARAM *p =
                (st_DBGAPI_HID_JOY_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            rq->success = hid_keymap_joystick_set ( p->port, p->mcp_mask );
            break;
        }

        case DBGAPI_CMD_INPUT_JOY_CLEAR:
        {
            /* Uvolní všechny joystick bity (= state byte 0xff). */
            st_DBGAPI_HID_JOY_PARAM *p =
                (st_DBGAPI_HID_JOY_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            rq->success = hid_keymap_joystick_clear ( p->port );
            break;
        }

        case DBGAPI_CMD_HID_SCRIPT_START:
        {
            /* Sekvence vstupu odměřená snímky emulace (mcp-inbox-fixes).
             * hid_script_start() provede první stisk a sekvenci si
             * zapamatuje; další kroky dělá per-frame bod hlavní smyčky
             * (hid_script_on_frame). Byla-li emulace v pauze, rozběhne ji
             * a po poslední události se emu sám pauzne na hranici snímku.
             * success=false = neplatná sekvence nebo už běží jiná. */
            rq->success = hid_script_start (
                (st_DBGAPI_HID_SCRIPT *) rq->data_ptr );
            break;
        }

        case DBGAPI_CMD_HID_SCRIPT_CANCEL:
        {
            /* Zrušení sekvence (uvolní drženou klávesu / joystick). Funguje
             * i v pauze - paused smyčka frontu dbgapi také vybírá. */
            rq->success = hid_script_cancel (
                (st_DBGAPI_HID_SCRIPT *) rq->data_ptr );
            break;
        }

        case DBGAPI_CMD_SCREENSHOT_SERIES:
        {
            /* Série screenshotů (screenshot_series.h): start, stop, stav.
             * Stav se vrací vždy, i při chybě operace. */
            st_DBGAPI_SCREENSHOT_SERIES_PARAM *p =
                (st_DBGAPI_SCREENSHOT_SERIES_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_result = DBGAPI_SCREENSHOT_SERIES_RESULT_OK;
            p->out_error[ 0 ] = 0;
            switch ( p->op )
            {
                case DBGAPI_SCREENSHOT_SERIES_OP_START:
                    if ( !screenshot_series_start ( &p->params, p->out_error, sizeof ( p->out_error ) ) )
                    {
                        p->out_result = DBGAPI_SCREENSHOT_SERIES_RESULT_FAILED;
                    };
                    break;
                case DBGAPI_SCREENSHOT_SERIES_OP_STOP:
                    if ( !screenshot_series_stop ( ) )
                    {
                        p->out_result = DBGAPI_SCREENSHOT_SERIES_RESULT_NOT_RUNNING;
                        g_strlcpy ( p->out_error, "No frame series is running", sizeof ( p->out_error ) );
                    };
                    break;
                case DBGAPI_SCREENSHOT_SERIES_OP_STATUS:
                default:
                    break;
            };
            screenshot_series_get_status ( &p->out_status );
            rq->success = ( p->out_result == DBGAPI_SCREENSHOT_SERIES_RESULT_OK );
            break;
        }

        case DBGAPI_CMD_AUDIO_CAPTURE:
        {
            /* Zachycení zvuku (audio_capture.h): start, stop, stav.
             * Stav se vrací vždy, i při chybě operace. */
            st_DBGAPI_AUDIO_CAPTURE_PARAM *p = (st_DBGAPI_AUDIO_CAPTURE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_error[ 0 ] = 0;
            switch ( p->op )
            {
                case DBGAPI_AUDIO_CAPTURE_OP_START:
                    (void) audio_capture_start ( &p->params, p->out_error, sizeof ( p->out_error ) );
                    break;
                case DBGAPI_AUDIO_CAPTURE_OP_STOP:
                    if ( !audio_capture_stop ( ) )
                    {
                        g_strlcpy ( p->out_error, "No audio capture is running", sizeof ( p->out_error ) );
                    };
                    break;
                case DBGAPI_AUDIO_CAPTURE_OP_STATUS:
                default:
                    break;
            };
            audio_capture_get_status ( &p->out_status );
            rq->success = ( p->out_error[ 0 ] == 0 );
            break;
        }

        case DBGAPI_CMD_TRACE_SNAPSHOT:
        {
            /* mcp-trace-read: snímek segmentu trace kanálu + kopie z RAM
             * (devdoc/trace/stream-read.md). Chunky na disku čte dispatch. */
            st_DBGAPI_TRACE_SNAPSHOT_PARAM *p = (st_DBGAPI_TRACE_SNAPSHOT_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_ok = false;
            p->out_pc_range_full = false;
            p->out_ram_prefix_t = 0;
            p->out_has_initial_regs = false;
            p->out_cpu_divider = tlog_common_get_cpu_divider ( );
            if ( p->channel == DBGAPI_TRACE_CHANNEL_RAMLOG )
            {
                /* Ring záznamu zápisů: vše "v RAM" (ramlog_copy), first_seq
                 * roste v režimu ring, nový segment po start/clear/resetu. */
                memset ( &p->out, 0, sizeof ( p->out ) );
                st_RAMLOG_STATUS rs;
                ramlog_get_status ( &rs );
                p->out.segment = rs.segment;
                p->out.live = rs.active ? 1 : 0;
                p->out.record_size = rs.allocated ? sizeof ( st_RAMLOG_REC ) : 0;
                p->out.next_seq = rs.next_seq;
                p->out.first_seq = rs.first_seq;
                p->out.ram_first_seq = rs.first_seq;
                p->out.chunks = g_array_new ( FALSE, FALSE, sizeof ( st_TLOG_CHUNK_META ) );
                uint64_t lo = ( p->from_seq > rs.first_seq ) ? p->from_seq : rs.first_seq;
                uint64_t want = ( lo < rs.next_seq ) ? rs.next_seq - lo : 0;
                if ( want > p->count ) want = p->count;
                uint64_t max_rec = p->max_copy_bytes / sizeof ( st_RAMLOG_REC );
                if ( want > max_rec ) want = max_rec;
                if ( want > 0 )
                {
                    p->out.ram_copy = (uint8_t *) g_try_malloc ( (size_t) want * sizeof ( st_RAMLOG_REC ) );
                    if ( !p->out.ram_copy )
                    {
                        tlog_read_snapshot_clear ( &p->out );
                        rq->success = false;
                        break;
                    };
                    uint64_t first = 0;
                    p->out.ram_copy_count = ramlog_copy ( lo, want, (st_RAMLOG_REC *) p->out.ram_copy, &first );
                    p->out.ram_copy_seq = first;
                };
                p->out_ok = true;
                p->out_pc_range_full = true;
                rq->success = true;
                break;
            };
            if ( p->channel == DBGAPI_TRACE_CHANNEL_BPLOG )
            {
                /* Ring výstupu akcí BP: vše "v RAM" (bplog_copy), kanál je
                 * vždy aktivní (live, segment >= 1, i před alokací ringu),
                 * first_seq roste po přetečení. */
                memset ( &p->out, 0, sizeof ( p->out ) );
                st_BPLOG_STATUS bs;
                bplog_get_status ( &bs );
                p->out.segment = bs.segment;
                p->out.live = 1;
                p->out.record_size = sizeof ( st_BPLOG_REC );
                p->out.next_seq = bs.next_seq;
                p->out.first_seq = bs.first_seq;
                p->out.ram_first_seq = bs.first_seq;
                p->out.chunks = g_array_new ( FALSE, FALSE, sizeof ( st_TLOG_CHUNK_META ) );
                uint64_t lo = ( p->from_seq > bs.first_seq ) ? p->from_seq : bs.first_seq;
                uint64_t want = ( lo < bs.next_seq ) ? bs.next_seq - lo : 0;
                if ( want > p->count ) want = p->count;
                uint64_t max_rec = p->max_copy_bytes / sizeof ( st_BPLOG_REC );
                if ( want > max_rec ) want = max_rec;
                if ( want > 0 )
                {
                    p->out.ram_copy = (uint8_t *) g_try_malloc ( (size_t) want * sizeof ( st_BPLOG_REC ) );
                    if ( !p->out.ram_copy )
                    {
                        tlog_read_snapshot_clear ( &p->out );
                        rq->success = false;
                        break;
                    };
                    uint64_t first = 0;
                    p->out.ram_copy_count = bplog_copy ( lo, want, (st_BPLOG_REC *) p->out.ram_copy, &first );
                    p->out.ram_copy_seq = first;
                };
                p->out_ok = true;
                p->out_pc_range_full = true;
                rq->success = true;
                break;
            };
            if ( p->channel == DBGAPI_TRACE_CHANNEL_EVENTLOG )
            {
                /* Ring Event Vieweru: vše "v RAM", nejstarší čitelný záznam
                 * first_seq = next_seq - count (starší ring přepsal). */
                memset ( &p->out, 0, sizeof ( p->out ) );
                const st_EVENTLOG_RING *ring = &g_eventlog;
                p->out.segment = ring->segment;
                p->out.live = g_eventlog_active ? 1 : 0;
                p->out.record_size = sizeof ( st_EVENTLOG_EVENT );
                p->out.next_seq = ring->next_seq;
                p->out.first_seq = ring->next_seq - ring->count;
                p->out.ram_first_seq = p->out.first_seq;
                p->out.chunks = g_array_new ( FALSE, FALSE, sizeof ( st_TLOG_CHUNK_META ) );
                uint64_t lo = ( p->from_seq > p->out.first_seq ) ? p->from_seq : p->out.first_seq;
                uint64_t hi = p->out.next_seq;
                if ( lo < hi && p->count < hi - lo ) hi = lo + p->count;
                uint64_t max_rec = p->max_copy_bytes / sizeof ( st_EVENTLOG_EVENT );
                if ( hi > lo && hi - lo > max_rec ) hi = lo + max_rec;
                if ( lo < hi && ring->events && ring->capacity )
                {
                    size_t n = (size_t) ( hi - lo );
                    p->out.ram_copy = (uint8_t *) g_try_malloc ( n * sizeof ( st_EVENTLOG_EVENT ) );
                    if ( !p->out.ram_copy )
                    {
                        tlog_read_snapshot_clear ( &p->out );
                        rq->success = false;
                        break;
                    };
                    /* Nejstarší event v ringu leží na (head - count) mod capacity. */
                    size_t oldest = ( ring->head + ring->capacity - ring->count ) % ring->capacity;
                    for ( size_t k = 0; k < n; k++ )
                    {
                        size_t idx = ( oldest + (size_t) ( lo - p->out.first_seq ) + k ) % ring->capacity;
                        memcpy ( p->out.ram_copy + k * sizeof ( st_EVENTLOG_EVENT ),
                                 &ring->events[ idx ], sizeof ( st_EVENTLOG_EVENT ) );
                    };
                    p->out.ram_copy_seq = lo;
                    p->out.ram_copy_count = n;
                };
                p->out_ok = true;
                rq->success = true;
                break;
            };
            st_DBGAPI_TRACE_CHAN_DESC d;
            if ( !dbgapi_resolve_trace_channel ( p->channel, &d ) || !d.fn_writer )
            {
                memset ( &p->out, 0, sizeof ( p->out ) );
                rq->success = false;
                break;
            };
            const st_TLOG_WRITER *w = d.fn_writer ( );
            if ( tlog_writer_read_snapshot ( w, p->from_seq, p->count, p->max_copy_bytes, &p->out ) != 0 )
            {
                rq->success = false;
                break;
            };
            if ( p->channel == DBGAPI_TRACE_CHANNEL_CPUTRACK )
            {
                p->out_pc_range_full = ( g_cputrack_config.pc_range_lo == 0 &&
                                         g_cputrack_config.pc_range_hi >= 0xFFFF );
                /* Součet T-stavů záznamů RAM bufferu před ram_copy_seq (nebo
                 * před from_seq bez kopie): báze času stránky v RAM.
                 * Velikost záznamu ze segmentu (12 B v2, 32 B v3);
                 * synchronizační záznam v3 čas nemá. */
                size_t rs = p->out.record_size;
                if ( p->want_time && p->out.live && w->buffer &&
                     ( rs == TLOG_RECORD_SIZE_CPUTRACK || rs == CPUTRACK_RECORD_SIZE_REGS ) )
                {
                    uint64_t until = p->out.ram_copy_count ? p->out.ram_copy_seq : p->from_seq;
                    if ( until > p->out.next_seq ) until = p->out.next_seq;
                    uint64_t sum = 0;
                    for ( uint64_t q = p->out.ram_first_seq; q < until; q++ )
                    {
                        const uint8_t *r = w->buffer + ( q - p->out.ram_first_seq ) * rs;
                        if ( r[ 2 ] == CPUTRACK_INSN_LEN_SYNC ) continue;
                        uint32_t wait = (uint32_t) r[ 7 ] | ( (uint32_t) r[ 8 ] << 8 ) |
                                        ( (uint32_t) r[ 9 ] << 16 ) | ( (uint32_t) r[ 10 ] << 24 );
                        sum += (uint64_t) r[ 11 ] + wait;
                    };
                    p->out_ram_prefix_t = sum;
                };
                p->out_has_initial_regs =
                    cputrack_get_initial_regs ( &p->out_initial_regs, &p->out_initial_q ) ? true : false;
            };
            p->out_ok = true;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_TRACE_RESOLVE:
        {
            /* mcp-trace-read: symboly adres a jména značek pro řádky trace
             * (sym_db i registr marklog se čtou jen v emu vlákně). */
            st_DBGAPI_TRACE_RESOLVE_PARAM *p = (st_DBGAPI_TRACE_RESOLVE_PARAM *) rq->data_ptr;
            if ( !p || ( p->n_addrs > 0 && ( !p->addrs || !p->out_syms ) ) ||
                 ( p->n_markers > 0 && ( !p->marker_ids || !p->out_markers ) ) ||
                 ( p->n_events > 0 && ( !p->events || !p->out_event_texts ) ) )
            {
                rq->success = false;
                break;
            };
            dbgapi_symbols_resolve ( p->addrs, p->n_addrs, p->out_syms );
            for ( size_t i = 0; i < p->n_markers; i++ )
            {
                const char *name = marklog_get_name ( p->marker_ids[ i ] );
                g_strlcpy ( p->out_markers[ i ], name ? name : "", DBGAPI_MARKER_NAME_MAX );
            };
            for ( size_t i = 0; i < p->n_events; i++ )
            {
                st_EVENTLOG_EVENT e;
                memcpy ( &e, p->events + i * sizeof ( st_EVENTLOG_EVENT ), sizeof ( e ) );
                eventlog_decode_detail ( &e, p->out_event_texts[ i ], DBGAPI_EVENT_TEXT_MAX );
            };
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_COVERAGE:
        {
            /* Pokrytí kódu (coverage_hook.h): start/stop přepíná callbacky
             * CPU a masku háčků po instrukci, proto jen v emu vlákně
             * (drain). Snímek kopíruje data z jednoho okamžiku. */
            st_DBGAPI_COVERAGE_PARAM *p = (st_DBGAPI_COVERAGE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_error[ 0 ] = 0;
            switch ( p->op )
            {
                case DBGAPI_COVERAGE_OP_START:
                    if ( !coverage_hook_start ( &p->cfg, p->reset ) )
                    {
                        g_strlcpy ( p->out_error, "Not enough memory for the coverage table", sizeof ( p->out_error ) );
                    };
                    break;
                case DBGAPI_COVERAGE_OP_STOP:
                    coverage_hook_stop ( );
                    break;
                case DBGAPI_COVERAGE_OP_RESET:
                    coverage_reset ( );
                    break;
                case DBGAPI_COVERAGE_OP_MARK:
                    p->out_epoch = coverage_mark ( );
                    break;
                case DBGAPI_COVERAGE_OP_STATUS:
                    break;
                case DBGAPI_COVERAGE_OP_SNAPSHOT:
                {
                    if ( !p->snap )
                    {
                        g_strlcpy ( p->out_error, "Missing snapshot buffer", sizeof ( p->out_error ) );
                        break;
                    };
                    if ( !coverage_snapshot_take ( p->snap, !p->snap_without_units ) )
                    {
                        g_strlcpy ( p->out_error, "Not enough memory for the coverage snapshot", sizeof ( p->out_error ) );
                    };
                    break;
                }
                default:
                    g_strlcpy ( p->out_error, "Unknown coverage operation", sizeof ( p->out_error ) );
                    break;
            };
            coverage_get_status ( &p->out_status );
            rq->success = ( p->out_error[ 0 ] == 0 );
            break;
        }

        case DBGAPI_CMD_BPLOG:
        {
            /* Ring výstupu akcí BP: zapisuje ho jen emu vlákno (akce BP),
             * proto i vyčištění běží tady (drain). */
            st_DBGAPI_BPLOG_PARAM *p = (st_DBGAPI_BPLOG_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            if ( p->op == DBGAPI_BPLOG_OP_CLEAR ) bplog_clear ( );
            bplog_get_status ( &p->out_status );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_RAMLOG:
        {
            /* Záznam zápisů do paměti (ramlog_hook.h): start/stop přepíná
             * callbacky CPU, proto jen v emu vlákně (drain). */
            st_DBGAPI_RAMLOG_PARAM *p = (st_DBGAPI_RAMLOG_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_error[ 0 ] = 0;
            switch ( p->op )
            {
                case DBGAPI_RAMLOG_OP_START:
                    if ( !ramlog_hook_start ( &p->cfg ) )
                    {
                        g_strlcpy ( p->out_error, "Not enough memory for the RAM write log", sizeof ( p->out_error ) );
                    };
                    break;
                case DBGAPI_RAMLOG_OP_STOP:
                    ramlog_hook_stop ( );
                    break;
                case DBGAPI_RAMLOG_OP_CLEAR:
                    ramlog_clear ( );
                    break;
                case DBGAPI_RAMLOG_OP_STATUS:
                    break;
                case DBGAPI_RAMLOG_OP_WHO_WROTE:
                {
                    st_RAMLOG_TARGET t[ RAMLOG_TARGETS_MAX ];
                    unsigned n = 0;
                    p->out_mmio = false;
                    if ( p->by_cpu )
                    {
                        n = ramlog_platform_query_cells ( p->cpu_from, t, &p->out_mmio );
                    }
                    else if ( p->by_region )
                    {
                        /* Region Memory Browseru -> buňka (stejný převod jako
                         * zápis přes regions_write). */
                        memset ( &t[ 0 ], 0, sizeof ( t[ 0 ] ) );
                        if ( !dbgapi_regions_ramlog_cell ( p->region_kind, p->region_sub, p->region_off,
                                                           &t[ 0 ].space, &t[ 0 ].offset ) )
                        {
                            g_strlcpy ( p->out_error, "The RAM write log does not cover this memory region",
                                        sizeof ( p->out_error ) );
                            p->out_n_cells = 0;
                            break;
                        };
                        if ( p->region_kind == REGION_KIND_VRAM_PHYS_PLANE )
                        {
                            /* Fyzický plán grafické VRAM MZ-800. */
                            t[ 0 ].has_plane = true;
                            t[ 0 ].plane = (uint8_t) p->region_sub;
                        };
                        n = 1;
                    }
                    else
                    {
                        memset ( &t[ 0 ], 0, sizeof ( t[ 0 ] ) );
                        t[ 0 ].space = p->space;
                        t[ 0 ].offset = p->off_from;
                        n = 1;
                    };
                    p->out_n_cells = n;
                    for ( unsigned i = 0; i < n; i++ )
                    {
                        st_DBGAPI_RAMLOG_CELL *c = &p->out_cells[ i ];
                        memset ( c, 0, sizeof ( *c ) );
                        c->space = t[ i ].space;
                        c->offset = t[ i ].offset;
                        c->has_plane = t[ i ].has_plane;
                        c->plane = t[ i ].plane;
                        c->has_last = ramlog_lastwriter ( (en_RAMLOG_SPACE) c->space, c->offset, &c->last );
                        if ( p->out_recs && p->limit )
                        {
                            c->n_recs = ramlog_who_wrote ( (en_RAMLOG_SPACE) c->space, c->offset,
                                                           p->out_seqs ? p->out_seqs + i * p->limit : NULL,
                                                           p->out_recs + i * p->limit, p->limit,
                                                           !p->include_unchanged );
                        };
                    };
                    break;
                }
                case DBGAPI_RAMLOG_OP_WRITERS:
                    p->out_n_writers = 0;
                    p->out_total_writers = 0;
                    if ( p->out_writers && p->limit )
                    {
                        p->out_n_writers = p->by_cpu
                            ? ramlog_writers_cpu ( p->cpu_from, p->cpu_to, p->out_writers, p->limit,
                                                   &p->out_total_writers, !p->include_unchanged )
                            : ramlog_writers ( (en_RAMLOG_SPACE) p->space, p->off_from, p->off_to,
                                               p->out_writers, p->limit, &p->out_total_writers,
                                               !p->include_unchanged );
                    };
                    break;
                case DBGAPI_RAMLOG_OP_RECENT:
                    /* Průchod omezený na 64Ki záznamů, aby okno v GUI při
                     * mnoha skrytých zápisech nebrzdilo emu vlákno. */
                    p->out_n_recs = ( p->out_recs && p->limit )
                        ? ramlog_recent ( p->out_seqs, p->out_recs, p->limit, !p->include_unchanged, 65536u )
                        : 0;
                    break;
                default:
                    g_strlcpy ( p->out_error, "Unknown ramlog operation", sizeof ( p->out_error ) );
                    break;
            };
            ramlog_get_status ( &p->out_status );
            rq->success = ( p->out_error[ 0 ] == 0 );
            break;
        }

        case DBGAPI_CMD_PROFILER_PC:
        {
            /* Profiler podle PC (profiler_pc.h). Souhrn se vrací vždy. */
            st_DBGAPI_PROFILER_PC_PARAM *p = (st_DBGAPI_PROFILER_PC_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_error[ 0 ] = 0;
            p->out_total = 0;
            p->out_written = 0;
            switch ( p->op )
            {
                case DBGAPI_PROFILER_PC_OP_START:
                    if ( p->reset ) profiler_pc_reset ( );
                    profiler_pc_set_active ( true );
                    break;
                case DBGAPI_PROFILER_PC_OP_STOP:
                    profiler_pc_set_active ( false );
                    break;
                case DBGAPI_PROFILER_PC_OP_RESET:
                    profiler_pc_reset ( );
                    break;
                case DBGAPI_PROFILER_PC_OP_GET:
                {
                    if ( !p->rows || p->rows_max == 0 )
                    {
                        p->out_total = profiler_pc_get_rows ( p->sort, p->lo, p->hi, NULL, 0 );
                        break;
                    };
                    st_PROFILER_PC_ROW *tmp = (st_PROFILER_PC_ROW *) g_malloc ( p->rows_max * sizeof ( *tmp ) );
                    p->out_total = profiler_pc_get_rows ( p->sort, p->lo, p->hi, tmp, p->rows_max );
                    p->out_written = ( p->out_total < p->rows_max ) ? p->out_total : p->rows_max;
                    for ( size_t i = 0; i < p->out_written; i++ )
                    {
                        p->rows[ i ].r = tmp[ i ];
                    };
                    g_free ( tmp );
                    if ( p->with_symbols )
                    {
                        dbgapi_profiler_pc_fill_symbols ( p->rows, p->out_written );
                    }
                    else
                    {
                        for ( size_t i = 0; i < p->out_written; i++ )
                        {
                            p->rows[ i ].symbol[ 0 ] = '\0';
                            p->rows[ i ].sym_offset = 0;
                        };
                    };
                    break;
                }
                case DBGAPI_PROFILER_PC_OP_EXPORT:
                {
                    int n = profiler_pc_export_csv ( p->path, p->sort, p->out_error, sizeof ( p->out_error ) );
                    if ( n >= 0 ) p->out_written = (size_t) n;
                    else if ( !p->out_error[ 0 ] ) g_strlcpy ( p->out_error, "export failed", sizeof ( p->out_error ) );
                    break;
                }
                case DBGAPI_PROFILER_PC_OP_BITMAP:
                    if ( !p->bitmap )
                    {
                        g_strlcpy ( p->out_error, "missing bitmap buffer", sizeof ( p->out_error ) );
                        break;
                    };
                    p->out_written = profiler_pc_get_bitmap ( p->bitmap );
                    break;
                default:
                    g_strlcpy ( p->out_error, "unknown operation", sizeof ( p->out_error ) );
                    break;
            };
            profiler_pc_get_summary ( &p->out_summary );
            rq->success = ( p->out_error[ 0 ] == 0 );
            break;
        }

        /* --- V1.D.1: Core + CPU extras Resources -------------------- */

        case DBGAPI_CMD_GET_CPU_IM2_VECTOR:
        {
            /* Snapshot Z80 IM2 stavu - I register, IM, poslední ACK vector.
             * Pro IM != 2 vyplníme available=0 a ostatní jsou platná, ale
             * isr_addr/isr_target ztrácí význam (= klient si přečte
             * available=0 a ignoruje je). */
            st_DBGAPI_CPU_IM2_VECTOR_PARAM *p =
                (st_DBGAPI_CPU_IM2_VECTOR_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            z80_t *cpu = g_mzarch_main.cpu;
            p->im       = cpu->im;
            p->i_reg    = cpu->i;
            p->last_vec = cpu->int_vector;
            if ( cpu->im == 2 )
            {
                p->available = 1;
                p->isr_addr  =
                    (uint16_t) ( ( (uint16_t) cpu->i << 8 )
                                 | (uint16_t) cpu->int_vector );
                /* Čteme cílovou adresu vektoru z paměti (= 2 bajty LE).
                 * Použijeme debugger_memory_read_byte - bez side effects. */
                uint8_t lo = debugger_memory_read_byte ( p->isr_addr );
                uint8_t hi = debugger_memory_read_byte (
                    (uint16_t) ( p->isr_addr + 1 ) );
                p->isr_target =
                    (uint16_t) ( (uint16_t) lo | ( (uint16_t) hi << 8 ) );
            }
            else
            {
                p->available  = 0;
                p->isr_addr   = 0;
                p->isr_target = 0;
            };
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_CPU_INTERRUPT_BUS:
        {
            /* Snapshot IRQ subsystému - Z80 core flags + per-platform note
             * + placeholder pro per-chip detail. V1.D.1 vystavuje pouze
             * Z80 core (vždy dostupné); daisy chain / non-chain / NMI
             * sources jsou available=0 + reason text. V1.D.2 (rozbor 5.1)
             * doplní per-chip data. */
            st_DBGAPI_CPU_IRQ_BUS_PARAM *p =
                (st_DBGAPI_CPU_IRQ_BUS_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            z80_t *cpu = g_mzarch_main.cpu;
            p->iff1       = cpu->iff1 ? 1 : 0;
            p->iff2       = cpu->iff2 ? 1 : 0;
            p->im         = cpu->im;
            p->halted     = cpu->halted ? 1 : 0;
            p->int_line   = cpu->int_pending ? 1 : 0;
            p->nmi_line   = cpu->nmi_pending ? 1 : 0;
            p->i_reg      = cpu->i;
            p->ei_pending = cpu->ei_delay ? 1 : 0;

            /* Per-platform note - statický popis IRQ topologie. */
            switch ( g_mzarch_platform_numeric )
            {
                case 700:
                    g_strlcpy ( p->platform_note,
                        "MZ-700: CTC 8253 + 8255 PPI (no Z80 PIO daisy chain).",
                        sizeof ( p->platform_note ) );
                    break;
                case 800:
                    g_strlcpy ( p->platform_note,
                        "MZ-800: GDG raster + CTC + Z80 PIO + non-chain non-mask sources.",
                        sizeof ( p->platform_note ) );
                    break;
                case 1500:
                    g_strlcpy ( p->platform_note,
                        "MZ-1500: GDG + CTC + Z80 PIO + PSG side IRQ.",
                        sizeof ( p->platform_note ) );
                    break;
                default:
                    g_strlcpy ( p->platform_note, "unknown platform",
                        sizeof ( p->platform_note ) );
                    break;
            };

            /* Per-chip detail - V1.D.1 placeholder, V1.D.2 implementace. */
            p->daisy_chain_available = 0;
            g_strlcpy ( p->daisy_chain_reason,
                "per-chip Z80 PIO daisy chain snapshot deferred to V1.D.2",
                sizeof ( p->daisy_chain_reason ) );

            p->non_chain_available = 0;
            g_strlcpy ( p->non_chain_reason,
                "per-source non-chain IRQ snapshot deferred to V1.D.2",
                sizeof ( p->non_chain_reason ) );

            p->nmi_sources_available = 0;
            g_strlcpy ( p->nmi_sources_reason,
                "per-source NMI snapshot deferred to V1.D.2 (memext PEHU etc.)",
                sizeof ( p->nmi_sources_reason ) );

            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_MEMORY_MAP:
        {
            /* Per-platform snapshot 16 slotů × 4 KB. V1.D.1 minimální
             * implementace: pro každý slot rozliší zda jde o MEMEXT
             * (přes memext_get_ram_offset_from_pointer) a vyplní
             * source/offset. Pro non-memext slot označí jako "unknown"
             * (= rozsáhlá per-platform banking detekce je out of scope
             * V1.D.1; klient si přečte addr_range a další detail z
             * dalších Resources). */
            st_DBGAPI_MEMORY_MAP_PARAM *p =
                (st_DBGAPI_MEMORY_MAP_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            dbgapi_memory_map_fill ( p );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_MEMEXT_INFO:
        {
            /* Snapshot Memory expansion adaptéru. Pokud memext odpojen
             * (= g_memext.connection == NO), vrátí type="none" +
             * connected=0; ostatní pole jsou 0. */
            st_DBGAPI_MEMEXT_INFO_PARAM *p =
                (st_DBGAPI_MEMEXT_INFO_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );

            if ( !MEMEXT_TEST_CONNECTED )
            {
                g_strlcpy ( p->type, "none", sizeof ( p->type ) );
                p->connected     = 0;
                p->map_available = 0;
                rq->success      = true;
                break;
            };

            p->connected     = 1;
            p->map_available = 1;
            for ( int i = 0; i < 16; i++ )
            {
                p->current_map[ i ] = g_memext.map[ i ];
            };

            if ( MEMEXT_TEST_TYPE_LUFTNER )
            {
                g_strlcpy ( p->type, "luftner", sizeof ( p->type ) );
                p->ram_banks       = MEMEXT_LUFTNER_BANKS;
                p->ram_bank_size   = MEMEXT_RAW_BANK_SIZE;
                p->flash_banks     = MEMEXT_LUFTNER_BANKS;
                p->flash_bank_size = MEMEXT_RAW_BANK_SIZE;
            }
            else if ( MEMEXT_TEST_TYPE_PEHU )
            {
                g_strlcpy ( p->type, "pehu", sizeof ( p->type ) );
                p->ram_banks       = MEMEXT_PEHU_BANKS;
                p->ram_bank_size   = MEMEXT_PEHU_BANK_SIZE;
                p->flash_banks     = 0;
                p->flash_bank_size = 0;
            }
            else
            {
                g_strlcpy ( p->type, "unknown", sizeof ( p->type ) );
            };
            rq->success = true;
            break;
        }
        case DBGAPI_CMD_BP_VARS_LIST:
        {
            /* V1.D.2.B - Snapshot bp_vars storage. Caller alokuje
             * entries[] o velikosti capacity, handler vyplní first
             * out_count záznamů + truncated flag. bp_vars storage je
             * EMU-thread writeable (per bp_vars.h:14-17 nemá read
             * protection); CMD běží na EMU thread per submit pattern,
             * takže read je consistent. */
            st_DBGAPI_BP_VARS_LIST_PARAM *p =
                (st_DBGAPI_BP_VARS_LIST_PARAM *) rq->data_ptr;
            if ( !p || !p->entries || p->capacity == 0 )
            {
                if ( p ) p->out_count = 0;
                rq->success = false;
                break;
            };
            size_t total = bp_vars_count ( );
            size_t n     = ( total > p->capacity ) ? p->capacity : total;
            for ( size_t i = 0; i < n; i++ )
            {
                const bp_var_t *v = bp_vars_get_by_index ( i );
                st_DBGAPI_BP_VAR_ENTRY *e = &p->entries[ i ];
                memset ( e, 0, sizeof ( *e ) );
                if ( v && v->name )
                {
                    g_strlcpy ( e->name, v->name, sizeof ( e->name ) );
                };
                e->value = v ? v->value : 0;
                if ( v && v->comment && v->comment[ 0 ] != '\0' )
                {
                    g_strlcpy ( e->comment, v->comment,
                                sizeof ( e->comment ) );
                    e->has_comment = 1;
                };
                e->persist_value = ( v && v->persist_value ) ? 1 : 0;
            };
            p->out_count = n;
            p->truncated = ( total > p->capacity ) ? 1 : 0;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_BOOKMARKS_LIST:
        {
            /* V1.D.2.B - Snapshot bookmarks storage. Backend
             * bookmarks_snapshot() interně serializuje pod GMutexem,
             * takže read je bezpečný i z MCP I/O threadu. Kopírujeme
             * z bookmark_t do fixed-size st_DBGAPI_BOOKMARK_ENTRY a
             * inline resolve-ujeme adresu přes bookmarks_resolve_addr. */
            st_DBGAPI_BOOKMARKS_LIST_PARAM *p =
                (st_DBGAPI_BOOKMARKS_LIST_PARAM *) rq->data_ptr;
            if ( !p || !p->entries || p->capacity == 0 )
            {
                if ( p ) p->out_count = 0;
                rq->success = false;
                break;
            };
            /* Lokálně alokujeme bookmark_t kopie pole o capacity záznamech,
             * bookmarks_snapshot() je naplní a vrátí out_count + truncated.
             * Pole je heap kvůli neznámé velikosti capacity (V1.5+ může
             * být velké). */
            bookmark_t *tmp = g_new0 ( bookmark_t, p->capacity );
            size_t n        = 0;
            bool   trunc    = false;
            bookmarks_snapshot ( tmp, p->capacity, &n, &trunc );
            for ( size_t i = 0; i < n; i++ )
            {
                const bookmark_t *b = &tmp[ i ];
                st_DBGAPI_BOOKMARK_ENTRY *e = &p->entries[ i ];
                memset ( e, 0, sizeof ( *e ) );
                e->id = b->id;
                if ( b->user_input )
                {
                    g_strlcpy ( e->user_input, b->user_input,
                                sizeof ( e->user_input ) );
                };
                if ( b->comment && b->comment[ 0 ] != '\0' )
                {
                    g_strlcpy ( e->comment, b->comment,
                                sizeof ( e->comment ) );
                    e->has_comment = 1;
                };
                uint16_t resolved = 0;
                if ( bookmarks_resolve_addr ( b->user_input, &resolved ) )
                {
                    e->addr          = resolved;
                    e->addr_resolved = 1;
                };
                e->cmd_origin = b->cmd_origin;
            };
            /* tmp drží jen shallow kopie char pointerů ze storage - NEFREE
             * stringy uvnitř (vlastní storage). Jen samotné pole. */
            g_free ( tmp );
            p->out_count = n;
            p->truncated = trunc ? 1 : 0;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_BOOKMARK_ADD:
        {
            /* BACKLOG B - přidá novou bookmark přes MCP. Po úspěšném
             * bookmarks_add propagujeme rq->cmd_origin do storage (= MCP),
             * stejný pattern jako SYMBOL_ADD. bookmarks_add interně
             * serializuje pod GMutexem; běžíme tu na EMU vlákně v safe-
             * pointu, takže ani UI vlákno nekoliduje. Po úspěchu resolve-
             * neme adresu pro echo (informativní, neukládá se). */
            st_DBGAPI_BOOKMARK_WRITE_PARAM *p =
                (st_DBGAPI_BOOKMARK_WRITE_PARAM *) rq->data_ptr;
            if ( !p || !p->user_input || p->user_input[ 0 ] == '\0' )
            {
                rq->success = false;
                break;
            };
            uint32_t id = bookmarks_add ( p->user_input, p->comment );
            if ( id == 0 )
            {
                rq->success = false;
                break;
            };
            bookmarks_set_cmd_origin ( id, rq->cmd_origin );
            p->out_id = id;
            uint16_t resolved = 0;
            if ( bookmarks_resolve_addr ( p->user_input, &resolved ) )
            {
                p->out_addr          = resolved;
                p->out_addr_resolved = 1;
            }
            else
            {
                p->out_addr          = 0;
                p->out_addr_resolved = 0;
            };
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_BOOKMARK_REMOVE:
        {
            /* BACKLOG B - smaže bookmark podle ID. bookmarks_remove vrací
             * true jen pokud záznam existoval. out_id je echo remove_id. */
            st_DBGAPI_BOOKMARK_WRITE_PARAM *p =
                (st_DBGAPI_BOOKMARK_WRITE_PARAM *) rq->data_ptr;
            if ( !p || p->remove_id == 0 )
            {
                rq->success = false;
                break;
            };
            p->out_id   = p->remove_id;
            rq->success = bookmarks_remove ( p->remove_id );
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_I8255:
        {
            /* V1.D.3.A - Snapshot Intel 8255 PPI. Kopíruje fields z
             * globálu g_pio8255. CMD běží na EMU vlákně (= submit pattern),
             * takže snapshot je consistent vůči klávesnicovému scanu
             * a CMT/PSG signalingu. Mode skupiny + directions dekódujeme
             * z mirror last_cw_byte podle 8255 datasheetu.
             *
             * 8255 Control Word Mode Set layout (bit 7 = 1):
             *   bit 7 = 1            (Mode Set flag)
             *   bits 6-5 = Mode A    (00=0, 01=1, 1x=2)
             *   bit 4    = PA dir    (1=in, 0=out)
             *   bit 3    = PC upper dir (1=in, 0=out)
             *   bit 2    = Mode B    (0=0, 1=1)
             *   bit 1    = PB dir    (1=in, 0=out)
             *   bit 0    = PC lower dir (1=in, 0=out)
             *
             * Když bit 7 = 0, CW je Bit Set/Reset operace na PC - shadow
             * Mode Set bytu emu nedrží, takže cw_decoded zůstává 0 a
             * mode_group/dir fields nesmí klient interpretovat. */
            st_DBGAPI_PERIPH_I8255_PARAM *p =
                (st_DBGAPI_PERIPH_I8255_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->port_a              = (uint8_t)( g_pio8255.signal_PA & 0xFF );
            p->port_b              = 0; /* MZ HW nemá samostatný PB read */
            p->port_c              = (uint8_t)( g_pio8255.signal_PC & 0xFF );
            p->control_word        = g_pio8255.last_cw_byte;
            if ( g_pio8255.last_cw_byte & 0x80 )
            {
                p->cw_decoded   = 1;
                uint8_t cw      = g_pio8255.last_cw_byte;
                uint8_t mode_a  = (uint8_t)( ( cw >> 5 ) & 0x03 );
                if ( mode_a > 2 ) mode_a = 2;
                p->mode_group_a = mode_a;
                p->mode_group_b = (uint8_t)( ( cw >> 2 ) & 0x01 );
                p->pa_dir       = (uint8_t)( ( cw >> 4 ) & 0x01 );
                p->pc_upper_dir = (uint8_t)( ( cw >> 3 ) & 0x01 );
                p->pb_dir       = (uint8_t)( ( cw >> 1 ) & 0x01 );
                p->pc_lower_dir = (uint8_t)( cw & 0x01 );
            };
            p->signal_pc00         = (uint8_t)( g_pio8255.signal_pc00 & 0x01 );
            p->signal_pc01         = (uint8_t)( g_pio8255.signal_pc01 & 0x01 );
            p->signal_pc02         = (uint8_t)( g_pio8255.signal_pc02 & 0x01 );
            p->signal_pc03         = (uint8_t)( g_pio8255.signal_pc03 & 0x01 );
            p->signal_pc04         = (uint8_t)( g_pio8255.signal_pc04 & 0x01 );
            p->pa_keyboard_column  =
                (uint8_t)( g_pio8255.signal_PA_keybord_column & 0x0F );
            p->pa_joy1_enabled     =
                (uint8_t)( g_pio8255.signal_PA_joy1_enabled & 0x01 );
            p->pa_joy2_enabled     =
                (uint8_t)( g_pio8255.signal_PA_joy2_enabled & 0x01 );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_I8253:
        {
            /* V1.D.3.A - Snapshot Intel 8253 CTC (3 časovače). Kopíruje
             * fields z g_ctc8253[0..2] do fixed-size st_DBGAPI_PERIPH_I8253_CHANNEL.
             * 8253 hardware Control Word neumí přečíst, mirror drží
             * g_ctc8253_last_cw_byte. */
            st_DBGAPI_PERIPH_I8253_PARAM *p =
                (st_DBGAPI_PERIPH_I8253_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            for ( int i = 0; i < 3; i++ )
            {
                const st_CTC8253 *c        = &g_ctc8253[ i ];
                st_DBGAPI_PERIPH_I8253_CHANNEL *out = &p->ch[ i ];
                out->value        = (uint16_t)( c->value & 0xFFFF );
                out->preset_value = (uint16_t)( c->preset_value & 0xFFFF );
                out->preset_latch = (uint16_t)( c->preset_latch & 0xFFFF );
                out->read_latch   = (uint16_t)( c->read_latch & 0xFFFF );
                out->out          = (uint8_t)( c->out & 0x01 );
                out->gate         = (uint8_t)( c->gate & 0x01 );
                out->mode         = (uint8_t)( c->mode );
                out->bcd          = (uint8_t)( c->bcd & 0x01 );
                out->rlf          = (uint8_t)( c->rlf );
                out->state        = (uint8_t)( c->state );
                out->load_done    = (uint8_t)( c->load_done & 0x01 );
                out->latch_op     = (uint8_t)( c->latch_op & 0x01 );
                out->rl_byte      = (uint8_t)( c->rl_byte & 0xFF );
            };
            p->last_cw_byte = g_ctc8253_last_cw_byte;
            rq->success     = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_Z80_PIO:
        {
            /* V1.D.3.A - Snapshot Z80 PIO. Na MZ-700 (HAVE_PIOZ80 == 0)
             * chip není přítomen; handler vyplní available = 0 a vrátí
             * success=true, aby klient dostal validní "není k dispozici"
             * JSON odpověď (= žádný error path). */
            st_DBGAPI_PERIPH_Z80_PIO_PARAM *p =
                (st_DBGAPI_PERIPH_Z80_PIO_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
#if HAVE_PIOZ80
            p->available         = 1;
            p->interrupt         = (uint8_t)( g_pioz80.interrupt & 0xFF );
            if ( g_pioz80.interrupt_port_id == PIOZ80_PORT_A
                 || g_pioz80.interrupt_port_id == PIOZ80_PORT_B )
            {
                p->interrupt_port_id =
                    (uint8_t)( g_pioz80.interrupt_port_id & 0x01 );
            }
            else
            {
                p->interrupt_port_id = 0xFF;
            };
            for ( int i = 0; i < 2; i++ )
            {
                const st_PIOZ80_PORT *src = &g_pioz80.port[ i ];
                st_DBGAPI_PERIPH_Z80_PIO_PORT *out =
                    ( i == 0 ) ? &p->port_a : &p->port_b;
                out->data_output    = src->data_output;
                out->masked_input   = src->masked_input;
                out->io_mask        = src->io_mask;
                out->mode           = (uint8_t)( src->mode );
                out->int_vec        = src->interrupt_vector;
                out->icmask         = src->icmask;
                out->icena          = (uint8_t)( src->icena );
                out->icfnc          = (uint8_t)( src->icfnc );
                out->iclvl          = (uint8_t)( src->iclvl );
                out->port_int       = (uint8_t)( src->port_int );
                out->last_ctrl_byte = ( i == 0 )
                    ? g_pioz80_port_a_last_ctrl_byte
                    : g_pioz80_port_b_last_ctrl_byte;
            };
#else
            p->available         = 0;
            p->interrupt_port_id = 0xFF;
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_SN76489:
        {
            /* V1.D.3.B - Snapshot SN76489 PSG. Handler používá výhradně
             * `psg_mirror_*()` getter API - žádný přímý přístup do
             * `g_psg_module.psg[i].channel[ch]` (= mirror funkce
             * garantují side-effect free čtení z EMU vlákna).
             *
             * Per platforma:
             *   - MZ-700 (HAVE_PSG=0): available=0, psg_count=0.
             *   - MZ-800 (HAVE_PSG=2): available=1, runtime stereo flag
             *     rozhoduje o psg_count (1 nebo 2).
             *   - MZ-1500 (HAVE_PSG=2, stereo nativně): available=1,
             *     psg_count=2.
             */
            st_DBGAPI_PERIPH_SN76489_PARAM *p =
                (st_DBGAPI_PERIPH_SN76489_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
#if HAVE_PSG >= 1
            p->available = 1;
            p->stereo    = (uint8_t)( g_psg_module.stereo ? 1 : 0 );
            p->psg_count = (uint8_t)( g_psg_module.stereo ? 2 : 1 );

            /* PSG0 (mono nebo levý) - vždy snapshot pokud available. */
            const st_PSG *psg0       = &g_psg_module.psg[ 0 ];
            p->psg0_latch_cs         = (uint8_t)( psg_mirror_latch_cs ( psg0 ) & 0x03 );
            p->psg0_latch_attn       = (uint8_t)( psg_mirror_latch_attn ( psg0 ) ? 1 : 0 );
            for ( unsigned ch = 0; ch < 4; ch++ )
            {
                st_DBGAPI_PERIPH_SN76489_CHANNEL *out = &p->psg0_ch[ ch ];
                out->type           = (uint8_t)( psg_mirror_channel_type ( psg0, ch ) );
                out->attenuation    = (uint8_t)( psg_mirror_channel_attn ( psg0, ch ) & 0x0F );
                out->tone_divider   = (uint16_t)( psg_mirror_channel_tone_divider ( psg0, ch ) & 0x03FFu );
                out->noise_div_type = (uint8_t)( psg_mirror_channel_noise_div_type ( psg0, ch ) );
                out->noise_type     = (uint8_t)( psg_mirror_channel_noise_type ( psg0, ch ) );
            };

            /* PSG1 (pravý) - jen pokud stereo. */
            if ( p->psg_count >= 2 )
            {
                const st_PSG *psg1   = &g_psg_module.psg[ 1 ];
                p->psg1_latch_cs     = (uint8_t)( psg_mirror_latch_cs ( psg1 ) & 0x03 );
                p->psg1_latch_attn   = (uint8_t)( psg_mirror_latch_attn ( psg1 ) ? 1 : 0 );
                for ( unsigned ch = 0; ch < 4; ch++ )
                {
                    st_DBGAPI_PERIPH_SN76489_CHANNEL *out = &p->psg1_ch[ ch ];
                    out->type           = (uint8_t)( psg_mirror_channel_type ( psg1, ch ) );
                    out->attenuation    = (uint8_t)( psg_mirror_channel_attn ( psg1, ch ) & 0x0F );
                    out->tone_divider   = (uint16_t)( psg_mirror_channel_tone_divider ( psg1, ch ) & 0x03FFu );
                    out->noise_div_type = (uint8_t)( psg_mirror_channel_noise_div_type ( psg1, ch ) );
                    out->noise_type     = (uint8_t)( psg_mirror_channel_noise_type ( psg1, ch ) );
                };
            };
#else
            /* HAVE_PSG == 0 (= MZ-700): chip není přítomen. */
            p->available = 0;
            p->psg_count = 0;
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_AY3_8910:
        {
            /* V1.D.3.B - Placeholder Resource. AY-3-8910 NENÍ
             * implementován v aktuální verzi emulátoru (= žádný symbol
             * v src/emulator/hw-generic/). Handler vrátí available=0
             * napříč platformami; pole zůstávají nulová. Klient musí
             * check `available` před interpretací.
             *
             * Struktura je zachována pro forward compat - pokud někdo
             * v budoucnu chip přidá (= nějaká MZ-1500 expanze nebo
             * podobně), layout rozšíříme bez wire protocol breaking
             * změny. */
            st_DBGAPI_PERIPH_AY3_8910_PARAM *p =
                (st_DBGAPI_PERIPH_AY3_8910_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->available = 0;
            rq->success  = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_BEEPER:
        {
            /* V1.D.3.B - Snapshot "beeperu". Sharp MZ NEMÁ dedikovaný
             * 1-bit beeper jako ZX Spectrum; audio cesta z CTC0 OUT
             * prochází přes AND hradla GATE0 a PC0 (= bit 0 portu C
             * 8255). Slyšitelná úroveň `level = ctc0_out & gate0 & pc0`.
             *
             * GATE0 zdroj:
             *   - MZ-800 v 800 módu: GATE0 trvale 1 (HW; přístupné jen
             *     v 700 módu mirror přes regct53g7).
             *   - MZ-700 / MZ-1500 / MZ-800 v 700 módu: GATE0 =
             *     `g_gdg.regct53g7` bit 0 (zápis na 0xE008 bit 0).
             *
             * Implementačně zde čteme raw `g_gdg.regct53g7` napříč
             * platformami; pro MZ-800 v 800 módu klient výslednou hodnotu
             * `audible` musí brát z `level` (= handler dopočte; pokud
             * `g_pio8255.signal_pc00` zapnuto a CTC0 OUT pulzuje, slyšet
             * je). Důležité: emulátor v MZ-800 800 módu nedrží regct53g7
             * vždy = 1; je to jen MZ-700 module state. Reference:
             * mz-800-knowledge/hw/06-ctc-8253.md (sekce CTC0 OUT). */
            st_DBGAPI_PERIPH_BEEPER_PARAM *p =
                (st_DBGAPI_PERIPH_BEEPER_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->available = 1;
            p->ctc0_out  = (uint8_t)( g_ctc8253[ 0 ].out & 0x01 );
            p->pc0       = (uint8_t)( g_pio8255.signal_pc00 & 0x01 );
            p->gate0     = (uint8_t)( gdg_get_regct53g7() & 0x01 );
            p->level     = (uint8_t)( p->ctc0_out & p->gate0 & p->pc0 );
            /* Source identifikátor pro debug log: "PC0" + NUL = 4 bajty,
             * source pole má 3 bajty (= bez NUL terminátoru, klient
             * dekóduje jako pevný array). */
            p->source[ 0 ] = 'P';
            p->source[ 1 ] = 'C';
            p->source[ 2 ] = '0';
            rq->success    = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_GDG:
        {
            /* V1.D.3.C - Snapshot GDG custom video LSI. Per-platforma
             * MZ-800 vs MZ-700 vs MZ-1500 mají různé `st_GDG` členy
             * (palette layout, regBOR/regPALGRP existence).
             *
             * Společná pole se kopírují bezpodmínečně, per-platforma
             * fields s `MZARCH` ifdef blocks. Handler nikdy nevolá
             * pomocné funkce s vedlejším efektem (= side-effect free
             * pro spolehlivé polling z MCP klienta).
             */
            st_DBGAPI_PERIPH_GDG_PARAM *p =
                (st_DBGAPI_PERIPH_GDG_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->available     = 1;
            /* Společné fields - existují u všech tří platforem. */
            p->regDMD        = (uint8_t)( g_gdg.regDMD & 0xFFu );
            p->regct53g7     = (uint8_t)( g_gdg.regct53g7 & 0xFFu );
            p->beam_row      = (uint32_t)g_gdg.beam_row;
            p->total_screens = (uint32_t)g_gdg.total_elapsed.screens;
            p->total_ticks   = (uint32_t)g_gdg.total_elapsed.ticks;
            p->sts_vsync     = (uint8_t)( g_gdg.sts_vsync & 0x01u );
            p->sts_hsync     = (uint8_t)( g_gdg.sts_hsync & 0x01u );
            p->hbln          = (uint8_t)( g_gdg.hbln & 0x01u );
            p->vbln          = (uint8_t)( g_gdg.vbln & 0x01u );
            p->tempo         = (uint32_t)g_gdg.tempo;
            p->tempo_divider = (uint32_t)g_gdg.tempo_divider;

#if MZARCH == 800
            /* MZ-800: 16-color palette přes regPALGRP + regPAL0..3,
             * border port + cksw + vram hot phase. */
            p->palette_count = 16;
            p->has_border_reg = 1;
            p->has_pal_group  = 1;
            p->has_cksw       = 1;
            p->regBOR    = (uint8_t)( g_gdg.regBOR & 0xFFu );
            p->regPALGRP = (uint8_t)( g_gdg.regPALGRP & 0xFFu );
            p->cksw      = (uint8_t)( g_gdg.cksw & 0x01u );
            /* Paleta: regPAL0..3 obsahuje 4 nibble-pair (per registr 2x4
             * bity = 4 entries po 4-bit). Pro klienta vyplníme 16 položek
             * podle pořadí ve kterém GDG resolvuje plane bits. Bezpečné je
             * vrátit raw bytes regPAL0..3 v lower bytes + zbytek 0 - klient
             * si zkonstruuje plnou paletu z regPAL bytes plus regPALGRP.
             * Pro názornost vyplníme entries 0..3 jako regPAL0..3 a entries
             * 4..15 jako 0 (= klient si reálnou 16-color tabulku spočítá
             * sám podle palette group bity). */
            p->palette[ 0 ] = (uint8_t)( g_gdg.regPAL0 & 0xFFu );
            p->palette[ 1 ] = (uint8_t)( g_gdg.regPAL1 & 0xFFu );
            p->palette[ 2 ] = (uint8_t)( g_gdg.regPAL2 & 0xFFu );
            p->palette[ 3 ] = (uint8_t)( g_gdg.regPAL3 & 0xFFu );
            /* entries 4..15 zůstávají 0 (placeholder; klient si dopočítá
             * podle regPAL0..3 + regPALGRP - viz hw/09-video-mz800-modes.md). */
            /* Řadič VRAM (WF/RF) a HW scroll - jen MZ-800. Scroll v jednotkách
             * portu (hwscroll_get_*), ne interní bajtové adresy plane. */
            p->has_vramctrl = 1;
            p->wf_mode      = (uint8_t)( g_vramctrl.regWF_MODE & 0x07u );
            p->wf_plane     = (uint8_t)( g_vramctrl.regWF_PLANE & 0x0Fu );
            p->rf_plane     = (uint8_t)( g_vramctrl.regRF_PLANE & 0x0Fu );
            p->rf_search    = (uint8_t)( g_vramctrl.regRF_SEARCH & 0x01u );
            p->wfrf_vbank   = (uint8_t)( g_vramctrl.regWFRF_VBANK & 0x01u );
            p->has_hwscroll = 1;
            p->hs_enabled   = (uint8_t)( g_hwscroll.enabled & 0x01u );
            p->hs_sof       = (uint16_t) hwscroll_get_sof ( );
            p->hs_ssa       = (uint8_t) hwscroll_get_ssa ( );
            p->hs_sea       = (uint8_t) hwscroll_get_sea ( );
            p->hs_sw        = (uint8_t) hwscroll_get_sw ( );
            memcpy ( p->platform, "mz800", 6 );
#elif MZARCH == 1500
            /* MZ-1500: 8-entry palette `mode1500_color[8]`, žádný regBOR
             * ani regPALGRP, žádný cksw. */
            p->palette_count  = 8;
            p->has_border_reg = 0;
            p->has_pal_group  = 0;
            p->has_cksw       = 0;
            for ( unsigned i = 0; i < 8; i++ )
            {
                p->palette[ i ] = (uint8_t)( g_gdg.mode1500_color[ i ] & 0xFFu );
            };
            memcpy ( p->platform, "mz1500", 7 );
#elif MZARCH == 700
            /* MZ-700: 8-entry palette `mode700_color[8]`, žádný regBOR
             * ani regPALGRP, žádný cksw. */
            p->palette_count  = 8;
            p->has_border_reg = 0;
            p->has_pal_group  = 0;
            p->has_cksw       = 0;
            for ( unsigned i = 0; i < 8; i++ )
            {
                p->palette[ i ] = (uint8_t)( g_gdg.mode700_color[ i ] & 0xFFu );
            };
            memcpy ( p->platform, "mz700", 6 );
#else
#error "Unknown MZARCH for GDG handler"
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_WD1793:
        {
            /* V1.D.3.C - Snapshot WD279x FDC chipu + 4 drives.
             *
             * Pokud build neměl CFG_HWEXT_HAVE_FDC, vrátíme available=0.
             * Pokud FDC compiled ale runtime detached (= connected != 1),
             * také available=0. Při available=1 kopírujeme registry
             * z g_fdc[FDC0].wd279x + mount metadata z g_fdc[FDC0].drive[4].
             *
             * Image_basename je jen filename (= basename z full path),
             * security per V1.D.1 precedent.
             */
            st_DBGAPI_PERIPH_WD1793_PARAM *p =
                (st_DBGAPI_PERIPH_WD1793_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
#if CFG_HWEXT_HAVE_FDC
            if ( g_fdc[FDC0].connected != FDC_CONNECTED )
            {
                p->available = 0;
                rq->success  = true;
                break;
            };
            p->available        = 1;
            p->bus_xlate_invert = (uint8_t)( g_fdc[FDC0].bus_xlate == FDC_BUS_XLATE_INVERT ? 1 : 0 );
            p->hd_patch         = (uint8_t)( g_fdc[FDC0].hd_patch ? 1 : 0 );
            p->reg_status       = g_fdc[FDC0].wd279x.regSTATUS;
            p->reg_command      = g_fdc[FDC0].wd279x.regCOMMAND;
            p->reg_track        = g_fdc[FDC0].wd279x.regTRACK;
            p->reg_sector       = g_fdc[FDC0].wd279x.regSECTOR;
            p->reg_data         = g_fdc[FDC0].wd279x.regDATA;
            p->motor            = g_fdc[FDC0].wd279x.MOTOR;
            p->side             = g_fdc[FDC0].wd279x.SIDE;
            p->density          = g_fdc[FDC0].wd279x.DENSITY;
            p->multiblock_rw    = g_fdc[FDC0].wd279x.multiblock_rw;
            p->direction_latch  = g_fdc[FDC0].wd279x.direction_latch;
            p->intrq_active     = g_fdc[FDC0].wd279x.intrq_active;
            p->positioned_track = g_fdc[FDC0].wd279x.positioned_track;
            p->positioned_sector = g_fdc[FDC0].wd279x.positioned_sector;
            p->positioned_side  = g_fdc[FDC0].wd279x.positioned_side;
            p->status_mode      = (uint8_t)g_fdc[FDC0].wd279x.status_mode;
            p->buffer_pos       = g_fdc[FDC0].wd279x.buffer_pos;
            p->data_counter     = g_fdc[FDC0].wd279x.data_counter;
            p->current_sector_size = g_fdc[FDC0].wd279x.current_sector_size;
            for ( unsigned d = 0; d < 4; d++ )
            {
                st_DBGAPI_PERIPH_FDC_DRIVE *out  = &p->drives[ d ];
                const st_FDDrive          *src  = &g_fdc[FDC0].drive[ d ];
                out->present        = (uint8_t)( src->mounted ? 1 : 0 );
                out->readonly       = (uint8_t)( src->readonly ? 1 : 0 );
                out->user_readonly  = (uint8_t)( src->user_readonly ? 1 : 0 );
                out->fs_readonly    = (uint8_t)( src->fs_readonly ? 1 : 0 );
                out->storage_mode   = (uint8_t)src->storage_mode;
                out->geometry_valid = (uint8_t)( src->geometry_valid ? 1 : 0 );
                if ( src->geometry_valid )
                {
                    out->tracks           = (uint16_t)src->geometry.tracks;
                    out->sides            = (uint16_t)src->geometry.sides;
                    out->total_data_bytes = (uint32_t)src->geometry.total_data_bytes;
                };
                /* Extrahuj basename - hledej poslední '/' nebo '\\' v full path. */
                const char *fn = src->filename;
                const char *base = fn;
                for ( const char *q = fn; *q; q++ )
                {
                    if ( *q == '/' || *q == '\\' ) base = q + 1;
                };
                /* Bezpečné zkopírování max 63 bajtů + NUL. */
                size_t blen = strlen ( base );
                if ( blen > sizeof ( out->image_basename ) - 1 )
                    blen = sizeof ( out->image_basename ) - 1;
                memcpy ( out->image_basename, base, blen );
                out->image_basename[ blen ] = '\0';
            };
#else
            p->available = 0;
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_CMT:
        {
            /* V1.D.3.C - Snapshot CMT modulu. CMT je u všech tří
             * platforem (= cassette interface je standard Sharp HW),
             * available je tedy vždy 1. Kopírujeme state, motor,
             * polarity, image basename.
             *
             * Image_basename z g_cmt.ui_base_filename (= UI base
             * filename, ne full path); pro V1.D.1 security to už je
             * basename, nicméně defenzivně basename z poslední cesty
             * stejně extrahujeme.
             */
            st_DBGAPI_PERIPH_CMT_PARAM *p =
                (st_DBGAPI_PERIPH_CMT_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->available         = 1;
            p->state             = (uint8_t)g_cmt.state;
            p->paused            = (uint8_t)( g_cmt.paused ? 1 : 0 );
            p->filled            = (uint8_t)( g_cmt.ext ? 1 : 0 );
            p->polarity_inverted = (uint8_t)( g_cmt.polarity == CMT_STREAM_POLARITY_INVERTED ? 1 : 0 );
            p->cmtspeed          = (uint8_t)g_cmt.mz_cmtspeed;
            p->cpu_boost         = (uint8_t)( g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED ? 1 : 0 );
            p->mzfsize_check     = (uint8_t)( g_cmt.mzfsize_check == CMT_MZFSIZE_CHECK_ENABLED ? 1 : 0 );
            p->output            = (uint8_t)( g_cmt.output & 0x01u );
            p->playsts           = (uint8_t)g_cmt.playsts;
            p->cmthack_enabled   = (uint8_t)( CMTHACK_TEST_IS_INSTALLED ? 1 : 0 );
            p->start_time        = (uint64_t)g_cmt.start_time;
            p->paused_time       = (uint64_t)g_cmt.paused_time;
            /* Image basename: prefer ui_base_filename, jinak last_filename. */
            const char *src = NULL;
            if ( g_cmt.ui_base_filename && g_cmt.ui_base_filename[ 0 ] )
                src = g_cmt.ui_base_filename;
            else if ( g_cmt.last_filename && g_cmt.last_filename[ 0 ] )
                src = g_cmt.last_filename;
            if ( src )
            {
                const char *base = src;
                for ( const char *q = src; *q; q++ )
                {
                    if ( *q == '/' || *q == '\\' ) base = q + 1;
                };
                size_t blen = strlen ( base );
                if ( blen > sizeof ( p->image_basename ) - 1 )
                    blen = sizeof ( p->image_basename ) - 1;
                memcpy ( p->image_basename, base, blen );
                p->image_basename[ blen ] = '\0';
            };
            /* délky pulzů: efektivní výchozí rychlosti a uložené vlastní */
            st_MZTAPE_PULSES_LENGTH dflt;
            if ( EXIT_SUCCESS == cmt_get_speed_pulses ( g_cmt.mz_cmtspeed, &g_cmt.mz_custom_pulses, &dflt ) )
                dbgapi_cmt_pulses_to_us ( &dflt, p->default_pulses_us );
            dbgapi_cmt_pulses_to_us ( &g_cmt.mz_custom_pulses, p->custom_pulses_us );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_PERIPH_QD:
        {
            /* V1.D.3.C - Snapshot Quick Disk (MZ-1F11) modulu.
             *
             * Pokud build neměl CFG_HWEXT_HAVE_QDISK, available=0.
             * Pokud QD compiled ale runtime detached, také available=0.
             * Při available=1 kopírujeme connected/status/type, R/O
             * příznaky, head position a per-mode meta (VIRTUAL counts,
             * IMAGE basename).
             */
            st_DBGAPI_PERIPH_QD_PARAM *p =
                (st_DBGAPI_PERIPH_QD_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
#if CFG_HWEXT_HAVE_QDISK
            if ( g_qdisk.connected != QDISK_CONNECTED )
            {
                p->available = 0;
                rq->success  = true;
                break;
            };
            p->available         = 1;
            p->type              = (uint8_t)( g_qdisk.type & 0xFFu );
            p->status            = (uint8_t)( g_qdisk.status & 0xFFu );
            p->readonly          = (uint8_t)( g_qdisk.readonly ? 1 : 0 );
            p->user_readonly     = (uint8_t)( g_qdisk.user_readonly ? 1 : 0 );
            p->fs_readonly       = (uint8_t)( g_qdisk.fs_readonly ? 1 : 0 );
            p->storage_mode      = (uint8_t)g_qdisk.storage_mode;
            p->vrtsts            = (uint8_t)g_qdisk.virt_status;
            p->image_position    = (uint32_t)g_qdisk.image_position;
            p->virt_files_count  = (uint32_t)g_qdisk.virt_files_count;
            p->virt_file_num     = (uint32_t)g_qdisk.virt_file_num;
            p->virt_mzfbody_size = (uint16_t)g_qdisk.virt_mzfbody_size;
            p->out_crc16         = (uint16_t)g_qdisk.out_crc16;
            /* Image basename: pro IMAGE/UNICARD mode bereme filename;
             * pro VIRTUAL mode pole zůstane prázdné. */
            if ( g_qdisk.type != QDISK_TYPE_VIRTUAL && g_qdisk.filename[ 0 ] )
            {
                const char *src = g_qdisk.filename;
                const char *base = src;
                for ( const char *q = src; *q; q++ )
                {
                    if ( *q == '/' || *q == '\\' ) base = q + 1;
                };
                size_t blen = strlen ( base );
                if ( blen > sizeof ( p->image_basename ) - 1 )
                    blen = sizeof ( p->image_basename ) - 1;
                memcpy ( p->image_basename, base, blen );
                p->image_basename[ blen ] = '\0';
            };
#else
            p->available = 0;
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE:
        {
            /* V1.D.4 - Snapshot klávesnice. Kopíruje keyboard_matrix +
             * vkbd_matrix z g_pio8255 a dopočítá effective (= AND obou,
             * == co CPU efektivně vidí při PORTB read). Decode aktivní
             * bity (= clear bity, Sharp matrix konvence) do pressed_keys
             * s reverse lookup jmen přes hid_keymap.
             *
             * Klávesová matice je shodná napříč MZ-700/MZ-800/MZ-1500
             * (= layout v iface_keyboard.c je sjednocený), proto bez
             * per-MZARCH větvení.
             */
            st_DBGAPI_INPUT_KBD_STATE_PARAM *p =
                (st_DBGAPI_INPUT_KBD_STATE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            for ( unsigned c = 0; c < 10; c++ )
            {
                p->real_matrix[ c ]    = g_pio8255.keyboard_matrix[ c ];
                p->virtual_matrix[ c ] = g_pio8255.vkbd_matrix[ c ];
                p->effective[ c ]      = (uint8_t)( p->real_matrix[ c ] & p->virtual_matrix[ c ] );
            };
            uint32_t pcount = 0;
            const uint32_t cap = (uint32_t)( sizeof ( p->pressed_keys ) / sizeof ( p->pressed_keys[ 0 ] ) );
            for ( unsigned c = 0; c < 10; c++ )
            {
                for ( unsigned b = 0; b < 8; b++ )
                {
                    /* Aktivní = bit clear (Sharp matrix konvence). */
                    if ( ( p->effective[ c ] & ( 1u << b ) ) == 0 )
                    {
                        pcount++;
                        if ( p->pressed_count < cap )
                        {
                            uint32_t idx = p->pressed_count;
                            p->pressed_keys[ idx ].col = (uint8_t)c;
                            p->pressed_keys[ idx ].bit = (uint8_t)b;
                            const char *nm = hid_keymap_reverse_lookup ( (int)c, (int)b );
                            if ( nm )
                            {
                                size_t nlen = strlen ( nm );
                                if ( nlen >= sizeof ( p->pressed_keys[ idx ].name ) )
                                    nlen = sizeof ( p->pressed_keys[ idx ].name ) - 1;
                                memcpy ( p->pressed_keys[ idx ].name, nm, nlen );
                                p->pressed_keys[ idx ].name[ nlen ] = '\0';
                            };
                            p->pressed_count++;
                        };
                    };
                };
            };
            if ( pcount > cap )
                p->pressed_truncated = 1;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO:
        {
            /* V1.D.4 - Statická popisná tabulka klávesnice. Sjednocená
             * napříč MZ-700/MZ-800/MZ-1500 v hid_keymap modulu. Pole
             * platform pro klient label, key_count + keys[] iterací.
             */
            st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM *p =
                (st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
#if MZARCH == 800
            memcpy ( p->platform, "mz800", 6 );
#elif MZARCH == 1500
            memcpy ( p->platform, "mz1500", 7 );
#elif MZARCH == 700
            memcpy ( p->platform, "mz700", 6 );
#endif
            const uint32_t cap = (uint32_t)( sizeof ( p->keys ) / sizeof ( p->keys[ 0 ] ) );
            int idx = 0;
            const char *nm = NULL;
            int col = 0, bit = 0;
            bool needs_shift = false;
            while ( idx < (int)cap
                    && hid_keymap_get_entry ( idx, &nm, &col, &bit, &needs_shift ) )
            {
                p->keys[ idx ].col          = (uint8_t)col;
                p->keys[ idx ].bit          = (uint8_t)bit;
                p->keys[ idx ].needs_shift  = (uint8_t)( needs_shift ? 1 : 0 );
                if ( nm )
                {
                    size_t nlen = strlen ( nm );
                    if ( nlen >= sizeof ( p->keys[ idx ].name ) )
                        nlen = sizeof ( p->keys[ idx ].name ) - 1;
                    memcpy ( p->keys[ idx ].name, nm, nlen );
                    p->keys[ idx ].name[ nlen ] = '\0';
                };
                idx++;
            };
            p->key_count = (uint32_t)idx;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE:
        {
            /* V1.D.4 - Snapshot joystick portů 0 a 1.
             *
             * Native g_joy.dev[].state je active-LOW (0 = bit stisknut);
             * MCP wire data jsou active-HIGH (= klient logičtější).
             * Decode podle JOY_STATEBIT_* enum.
             *
             * MZ-700 build: HAVE_JOY je obvykle 0 (= joystick HW není
             * standardní); handler vrátí oba porty connected=0. MZ-800
             * a MZ-1500: HAVE_JOY=1, runtime config rozhodne type.
             */
            st_DBGAPI_INPUT_JOY_STATE_PARAM *p =
                (st_DBGAPI_INPUT_JOY_STATE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
#if HAVE_JOY
            for ( unsigned i = 0; i < 2; i++ )
            {
                st_DBGAPI_INPUT_JOY_PORT *port = &p->port[ i ];
                en_JOY_TYPE typ = g_joy.dev[ i ].type;
                uint8_t native = g_joy.dev[ i ].state;
                port->native_state = native;
                if ( typ == JOY_TYPE_NONE )
                {
                    port->connected = 0;
                    memcpy ( port->device_name, "none", 5 );
                }
                else
                {
                    port->connected = 1;
                    /* Decode active-LOW -> active-HIGH bitmask. */
                    uint8_t bits = 0;
                    if ( !( native & ( 1u << JOY_STATEBIT_UP ) ) )    bits |= ( 1u << 0 );
                    if ( !( native & ( 1u << JOY_STATEBIT_DOWN ) ) )  bits |= ( 1u << 1 );
                    if ( !( native & ( 1u << JOY_STATEBIT_LEFT ) ) )  bits |= ( 1u << 2 );
                    if ( !( native & ( 1u << JOY_STATEBIT_RIGHT ) ) ) bits |= ( 1u << 3 );
                    if ( !( native & ( 1u << JOY_STATEBIT_TRIG1 ) ) ) bits |= ( 1u << 4 );
                    if ( !( native & ( 1u << JOY_STATEBIT_TRIG2 ) ) ) bits |= ( 1u << 5 );
                    port->state_bits = bits;
                    if ( typ == JOY_TYPE_NUM_KEYPAD )
                        memcpy ( port->device_name, "num_keypad", 11 );
                    else if ( typ == JOY_TYPE_JOYSTICK )
                        memcpy ( port->device_name, "joystick", 9 );
                    else
                        memcpy ( port->device_name, "unknown", 8 );
                };
            };
#else
            /* HAVE_JOY=0: oba porty connected=0, idle native_state=0xFF. */
            for ( unsigned i = 0; i < 2; i++ )
            {
                p->port[ i ].connected    = 0;
                p->port[ i ].native_state = 0xFF;
                memcpy ( p->port[ i ].device_name, "none", 5 );
            };
#endif
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO:
        {
            /* V1.D.4 - Shape metadata aktuálního framebufferu.
             *
             * width / height jsou compile-time konstanty per MZARCH
             * (VIDEO_DISPLAY_WIDTH/HEIGHT z gdg headeru). pixel_format=0
             * znamená INDEX8 (= MZ nativní). dirty flag odráží
             * fbsnapshot_framebuffer_state != FB_STATE_NOT_CHANGED.
             *
             * Palette vrátí 16-entry Sharp MZ tabulku 0x00RRGGBB
             * (= DISPLAY_MZCOLORS, INDEX8 nativní 4-bit barvy maskované
             * 0x0F). Klient potřebuje pro decode raw bufferu. Pokud
             * paleta není inicializovaná, vrátí 0 entries (palette_size=0).
             */
            st_DBGAPI_FRAME_FB_INFO_PARAM *p =
                (st_DBGAPI_FRAME_FB_INFO_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->width           = (uint32_t)VIDEO_DISPLAY_WIDTH;
            p->height          = (uint32_t)VIDEO_DISPLAY_HEIGHT;
            p->bytes_per_pixel = 1;
            p->pixel_format    = 0; /* INDEX8 */
            p->has_palette     = 1;
            APP_MUTEX_LOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            p->last_screen_id      = g_iface_video->fbsnapshot_screen_id;
            p->framebuffer_state   = (uint8_t)g_iface_video->fbsnapshot_framebuffer_state;
            APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            p->dirty = (uint8_t)( p->framebuffer_state != FB_STATE_NOT_CHANGED ? 1 : 0 );
            /* Palette: Sharp MZ má 16-entry colormap (DISPLAY_MZCOLORS).
             * Hodnoty jsou 0x00RRGGBB. Pokud display modul ještě nebyl
             * inicializován (= display_get_default_color_schema vrátí NULL),
             * vyplníme nuly. */
            uint32_t *g_video_colormap = display_get_default_color_schema();
            if ( g_video_colormap )
            {
                for ( unsigned i = 0; i < 16; i++ )
                {
                    p->palette[ i ] = g_video_colormap[ i ];
                };
                p->palette_size = 16;
            }
            else
            {
                p->palette_size = 0;
            };
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW:
        {
            /* V1.D.4 + V1.E.6.C headless fallback - kopie framebuffer
             * pixelů jako RGBA8888.
             *
             * Primary source: INDEX8 buffer g_iface_video->fbsnapshot_pixels
             * (= GUI cesta, SDL render thread publish/consume). Pokud
             * NULL (= headless mode bez SDL render loopu, nebo race mezi
             * publish a consume), fallback na GDG live buffer
             * g_framebuffer.pixels (= staticky alokovaný BSS, vždy
             * dostupný po framebuffer_init).
             *
             * Expand na RGBA děláme přes g_video_colormap[] paletu.
             * downscale_factor (1/2/4) bere každý N-tý pixel v obou osách.
             *
             * Velikost: width*height*4 / (factor*factor). Default 928x288x4
             * = ~1 MB. Při capacity menší než vypočítaná, handler factor
             * sám zvýší aby vešel; vrácená hodnota factor odráží použitou.
             *
             * Threading:
             *   - SDL source: lock fbsnapshot_pixels_mutex (= writer
             *     thread to teoreticky může mezi snapshoty přepsat).
             *   - GDG fallback: bez locku - handler běží v emu thread
             *     dispatch context (mzarch.c per-frame drain), GDG
             *     raster fill funkce taky v emu thread. Single-threaded
             *     executor = read race-free.
             *
             * Klient rozezná zdroj podle p->fallback_source
             * (SCREENSHOT_SRC_SDL_SNAPSHOT / SCREENSHOT_SRC_GDG_LIVE).
             */
            st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM *p =
                (st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM *) rq->data_ptr;
            if ( !p || !p->buffer || p->buffer_capacity == 0 )
            {
                rq->success = false;
                break;
            };
            p->available       = 0;
            p->pixel_format    = 0; /* RGBA8888 */
            p->fallback_source = (uint8_t)SCREENSHOT_SRC_SDL_SNAPSHOT;
            if ( p->downscale_factor != 1
                    && p->downscale_factor != 2
                    && p->downscale_factor != 4 )
            {
                p->downscale_factor = 1;
            };
            /* Auto-zvýšení faktoru aby vešel do bufferu. */
            uint32_t needed;
            for ( ;; )
            {
                uint32_t w = (uint32_t)VIDEO_DISPLAY_WIDTH / p->downscale_factor;
                uint32_t h = (uint32_t)VIDEO_DISPLAY_HEIGHT / p->downscale_factor;
                needed = w * h * 4;
                if ( needed <= p->buffer_capacity ) break;
                if ( p->downscale_factor == 1 )      p->downscale_factor = 2;
                else if ( p->downscale_factor == 2 ) p->downscale_factor = 4;
                else { rq->success = false; break; };
            };
            if ( !rq->success && p->downscale_factor > 4 )
            {
                break;
            };
            uint32_t w = (uint32_t)VIDEO_DISPLAY_WIDTH / p->downscale_factor;
            uint32_t h = (uint32_t)VIDEO_DISPLAY_HEIGHT / p->downscale_factor;
            uint32_t *g_video_colormap = display_get_default_color_schema();
            if ( !g_video_colormap )
            {
                /* Display modul ještě neinicializován - vrátíme available=0. */
                p->available = 0;
                p->width = 0;
                p->height = 0;
                rq->success = true;
                break;
            };
            /* Pokus o SDL snapshot path. Lock držíme jen krátce -
             * čteme pointer + screen_id, pokud NULL pak fallback bez
             * locku (= GDG live).
             *
             * Pozn.: V GUI mode může být fbsnapshot_pixels NULL i
             * tehdy, kdy framebuffer byl už emitnut (= SDL render
             * thread mezitím konzumoval, viz iface_video_sdl3.c:220).
             * V tom případě fallback na GDG live vrátí *stejný* obsah
             * jako poslední publikovaný frame (= g_framebuffer.pixels
             * po frame_done memcpy předchozího slotu, viz
             * mz*_framebuffer_done.h).
             */
            const uint8_t *src = NULL;
            uint32_t src_screen_id = 0;
            bool sdl_locked = false;

            APP_MUTEX_LOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            sdl_locked = true;
            if ( g_iface_video->fbsnapshot_pixels != NULL )
            {
                src                = g_iface_video->fbsnapshot_pixels;
                src_screen_id      = g_iface_video->fbsnapshot_screen_id;
                p->fallback_source = (uint8_t)SCREENSHOT_SRC_SDL_SNAPSHOT;
            }
            else
            {
                /* SDL snapshot prázdný - přepneme na GDG live buffer.
                 * Lock už nepotřebujeme (čteme jen g_framebuffer.pixels
                 * a screen_id, viz threading komentář výše). */
                src_screen_id = g_iface_video->fbsnapshot_screen_id;
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
                sdl_locked = false;
                src                = g_framebuffer.pixels;
                p->fallback_source = (uint8_t)SCREENSHOT_SRC_GDG_LIVE;
            };
            if ( !src )
            {
                /* Defensive - g_framebuffer.pixels by mělo být vždy
                 * non-NULL po framebuffer_init (statická BSS alokace).
                 * Pokud sem dojdeme, něco je vážně rozbité. */
                if ( sdl_locked )
                {
                    APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
                };
                p->available = 0;
                p->width = 0;
                p->height = 0;
                rq->success = true;
                break;
            };
            /* Hash obrazu ze stejného zdroje (plná velikost), dokud je
             * zdroj platný (u SDL snímku pod zámkem). */
            dbgapi_frame_hash_fill ( &p->hash, src, p->fallback_source, src_screen_id );
            uint8_t *dst = p->buffer;
            for ( uint32_t y = 0; y < h; y++ )
            {
                uint32_t sy = y * p->downscale_factor;
                if ( sy >= (uint32_t)VIDEO_DISPLAY_HEIGHT ) sy = (uint32_t)VIDEO_DISPLAY_HEIGHT - 1;
                const uint8_t *src_row = src + sy * (uint32_t)VIDEO_DISPLAY_WIDTH;
                for ( uint32_t x = 0; x < w; x++ )
                {
                    uint32_t sx = x * p->downscale_factor;
                    if ( sx >= (uint32_t)VIDEO_DISPLAY_WIDTH ) sx = (uint32_t)VIDEO_DISPLAY_WIDTH - 1;
                    /* Sharp MZ INDEX8 buffer používá nízkých 4 bitů
                     * jako paletní index (DISPLAY_MZCOLORS=16). Vyšší
                     * bity mohou nést status (= bordová cesta, blink),
                     * mask je nutný aby g_video_colormap nezasáhl mimo. */
                    uint8_t pix_idx = src_row[ sx ] & 0x0Fu;
                    uint32_t rgb = g_video_colormap[ pix_idx ];
                    dst[ 0 ] = (uint8_t)( ( rgb >> 16 ) & 0xFFu ); /* R */
                    dst[ 1 ] = (uint8_t)( ( rgb >> 8 ) & 0xFFu );  /* G */
                    dst[ 2 ] = (uint8_t)( rgb & 0xFFu );           /* B */
                    dst[ 3 ] = 0xFF;                                /* A */
                    dst += 4;
                };
            };
            if ( sdl_locked )
            {
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            };
            p->width            = w;
            p->height           = h;
            p->bytes_per_pixel  = 4;
            p->source_screen_id = src_screen_id;
            p->buffer_size      = needed;
            p->available        = 1;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG:
        {
            /* PNG screenshot přes jádro screenshotu (screenshot_encode_png):
             * oblast, měřítko a ořez podle p->opts (memset 0 = celý
             * framebuffer 1:1, dřívější chování).
             *
             * Pixel acquisition je IDENTICKÁ jako GET_FRAME_SCREENSHOT_RAW
             * (= konzistence obrazu): INDEX8 buffer
             * g_iface_video->fbsnapshot_pixels (GUI cesta, pod
             * fbsnapshot_pixels_mutex), fallback na g_framebuffer.pixels
             * (GDG live, headless). Threading shodný s raw handlerem (krátký
             * lock SDL path, GDG fallback bez locku v emu thread executoru).
             *
             * Buffer ownership: PNG stream (p->buffer) alokuje jádro přes
             * glib, dispatch ho po zápisu nebo base64 uvolní g_free.
             */
            st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM *p =
                (st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            /* Vstupní volby přežijí vynulování výstupů. */
            st_SCREENSHOT_OPTS opts = p->opts;
            memset ( p, 0, sizeof ( *p ) );
            p->opts = opts;

            /* Výběr zdroje pixelů - viz raw handler pro threading detaily. */
            const uint8_t *src = NULL;
            bool sdl_locked = false;
            uint8_t src_kind = (uint8_t) SCREENSHOT_SRC_SDL_SNAPSHOT;
            APP_MUTEX_LOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            sdl_locked = true;
            uint32_t src_screen_id = g_iface_video->fbsnapshot_screen_id;
            if ( g_iface_video->fbsnapshot_pixels != NULL )
            {
                src = g_iface_video->fbsnapshot_pixels;
            }
            else
            {
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
                sdl_locked = false;
                src = g_framebuffer.pixels;
                src_kind = (uint8_t) SCREENSHOT_SRC_GDG_LIVE;
            };
            if ( !src )
            {
                if ( sdl_locked )
                {
                    APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
                };
                p->available = 0;
                strcpy ( p->reason, "Framebuffer not yet rendered" );
                rq->success = true;
                break;
            };

            /* Hash obrazu ze stejného zdroje (celý framebuffer), dokud je
             * zdroj platný. */
            dbgapi_frame_hash_fill ( &p->hash, src, src_kind, src_screen_id );

            st_SCREENSHOT_PNG png;
            bool encoded = screenshot_encode_png ( src, VIDEO_DISPLAY_WIDTH, &opts, &png,
                                                   p->reason, sizeof ( p->reason ) );
            if ( sdl_locked )
            {
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            };
            if ( !encoded )
            {
                /* Neplatné volby, neinicializovaný displej nebo chyba
                 * enkódu - důvod je v p->reason. */
                p->available = 0;
                p->bad_opts  = png.bad_opts ? 1 : 0;
                rq->success = true;
                break;
            };

            p->buffer      = png.png;
            p->buffer_size = png.png_len;
            p->width       = png.width;
            p->height      = png.height;
            p->hires       = png.hires ? 1 : 0;
            p->available   = 1;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_VIDEO_TEXT_DUMP:
        {
            /* V1.D.4 - Text VRAM dump pro MZ-700 mode.
             *
             * MZ-700 a MZ-1500: 40x25 text mode, chars na D000-D3FF
             * (g_memory.VRAM[0x000..0x3FF]), atributy na D800-DBFF
             * (g_memory.VRAM[0x800..0xBFF]).
             *
             * MZ-800 v 700 mode (DMD bit MZ700 = 1): stejný layout.
             * MZ-800 v 800 mode: GDG grafický mode (různé rozlišení), bez
             * 40x25 textové struktury -> available=0.
             */
            st_DBGAPI_VIDEO_TEXT_DUMP_PARAM *p =
                (st_DBGAPI_VIDEO_TEXT_DUMP_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
#if MZARCH == 800
            memcpy ( p->platform, "mz800", 6 );
            if ( !GDG_MZ800_DMD_TEST_MZ700 )
            {
                p->available = 0;
                strcpy ( p->reason, "MZ-800 in 800 graphics mode (not text)" );
                rq->success = true;
                break;
            };
#elif MZARCH == 1500
            memcpy ( p->platform, "mz1500", 7 );
#elif MZARCH == 700
            memcpy ( p->platform, "mz700", 6 );
#endif
            p->cols = 40;
            p->rows = 25;
            p->cell_count = p->cols * p->rows;
            if ( p->cell_count > sizeof ( p->chars ) )
                p->cell_count = sizeof ( p->chars );
            for ( uint32_t i = 0; i < p->cell_count; i++ )
            {
                p->chars[ i ]      = g_memory.VRAM[ i ];
                p->attributes[ i ] = g_memory.VRAM[ 0x800 + i ];
            };
            p->available = 1;
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_GET_WATCH_SNAPSHOT:
        {
            /* V1.D.2.C - Per-watch snapshot statistik z dispatch-side
             * thread-safe mirroru (`watch_emu_cache`).
             *
             * Mirror je naplňován UI vláknem jednou per frame z watch
             * render loop. Tento handler jen kopíruje aktuální mirror
             * stav po jméně. 1-frame stale akceptováno per scope.
             *
             * Pokud řádek se zadaným jménem v mirror není, found=0 a
             * ostatní fields zůstávají 0. success=true vždy - chybějící
             * řádek není error.
             */
            st_DBGAPI_WATCH_SNAPSHOT_PARAM *p =
                (st_DBGAPI_WATCH_SNAPSHOT_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };

            /* Zachovej jméno z requestu (= IN), vynuluj OUT fields. */
            char name_in[ sizeof ( p->name ) ];
            memcpy ( name_in, p->name, sizeof ( name_in ) );
            name_in[ sizeof ( name_in ) - 1 ] = '\0';

            memset ( p, 0, sizeof ( *p ) );

            st_WATCH_EMU_SNAPSHOT snap;
            if ( watch_emu_cache_get_by_name ( name_in, &snap ) )
            {
                p->found           = 1;
                p->snapshot_active = snap.snapshot_active ? 1 : 0;
                p->min_max_valid   = snap.min_max_valid ? 1 : 0;
                p->row_id          = snap.row_id;
                p->type_snap       = snap.type_snap;
                p->snap_int        = snap.snap_int;
                p->cur_int         = snap.cur_int;
                p->delta_int       = snap.delta_int;
                p->min_int         = snap.min_int;
                p->max_int         = snap.max_int;
                p->change_count    = snap.change_count;
            };
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_REGIONS_ENUMERATE:
        {
            /* mzdos-support 0007 - Direct memory region enumerate.
             *
             * Tenký wrapper nad dbgapi_regions_enumerate() (= existující
             * backend, používaný GUI Memory browser). No-side-effect, safe
             * z emu vlákna.
             */
            st_DBGAPI_REGIONS_ENUM_PARAM *p =
                (st_DBGAPI_REGIONS_ENUM_PARAM *) rq->data_ptr;
            if ( p && !p->out )
            {
                /* Režim "všechny regiony": kopie bez stropu na počet
                 * (MCP regions_list; plná sestava MZ-800 má přes 500). */
                p->out_dup = dbgapi_regions_enumerate_dup_gen (
                    &p->out_count, &p->out_layout_gen );
                rq->success = true;
                break;
            };
            if ( !p || p->max_count <= 0 )
            {
                rq->success = false;
                break;
            };
            p->out_count = dbgapi_regions_enumerate (
                (st_REGION_DESC *) p->out, p->max_count );
            rq->success = ( p->out_count >= 0 );
            break;
        }

        case DBGAPI_CMD_REGIONS_READ:
        {
            /* mzdos-support 0007 - Direct memory region read.
             *
             * Tenký wrapper nad dbgapi_regions_read(). No-side-effect:
             * žádný auto-inc latch, žádný GDG RF dispatch, žádný IRQ
             * trigger. Caller dostává out_count = skutečně přečtená
             * délka (= clamp pokud offset+len > size regionu).
             * S layout_gen != 0 se ID ze starší sestavy HW odmítne
             * (out_count = DBGAPI_REGIONS_ERR_STALE).
             */
            st_DBGAPI_REGIONS_READ_PARAM *p =
                (st_DBGAPI_REGIONS_READ_PARAM *) rq->data_ptr;
            if ( !p || !p->buf || p->len == 0 )
            {
                rq->success = false;
                break;
            };
            p->out_count = dbgapi_regions_read_gen (
                p->region_id, p->layout_gen, p->offset, p->buf, p->len );
            rq->success = ( p->out_count >= 0 );
            break;
        }

        case DBGAPI_CMD_REGIONS_WRITE:
        {
            /* mzdos-support 0007 extended - Direct memory region write.
             *
             * Tenký wrapper nad dbgapi_regions_write(). Pro
             * REGION_KIND_MEMEXT_FLASH a REGION_KIND_PROHIBITED_SHADOW
             * backend vrátí -1 (= read-only z pohledu Memory Browseru).
             * Pro VRAM regiony žádný automatický screen refresh - to je
             * UI zodpovědnost při edit v pause modu. S layout_gen != 0
             * se ID ze starší sestavy HW odmítne (DBGAPI_REGIONS_ERR_STALE).
             */
            st_DBGAPI_REGIONS_WRITE_PARAM *p =
                (st_DBGAPI_REGIONS_WRITE_PARAM *) rq->data_ptr;
            if ( !p || !p->data || p->len == 0 )
            {
                rq->success = false;
                break;
            };
            /* Podmíněný zápis (observe-cas M10); chyby ID, generace
             * a zapisovatelnosti mají přednost (podmínka se nevyhodnotí). */
            if ( !dbgapi_write_cond_before_region ( p ) )
            {
                p->out_count = 0;
                rq->success = false;
                break;
            };
            p->out_count = dbgapi_regions_write_gen (
                p->region_id, p->layout_gen, p->offset, p->data, p->len );
            dbgapi_write_cond_after_region ( p );
            rq->success = ( p->out_count >= 0 );
            break;
        }

        case DBGAPI_CMD_GET_SPEED:
        {
            /* BACKLOG D - read-only snapshot emulační rychlosti.
             * Pure read z g_customspeed / g_emulator, žádný side effect. */
            st_DBGAPI_GET_SPEED_PARAM *p =
                (st_DBGAPI_GET_SPEED_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
            p->current_percent = (uint32_t) customspeed_get_current_speed ( );
            p->max_speed = EMULATOR_TEST_MAX_SPEED ? 1 : 0;
            if ( EMULATOR_TEST_MAX_SPEED )
                g_strlcpy ( p->mode, "max", sizeof ( p->mode ) );
            else if ( p->current_percent != 100 )
                g_strlcpy ( p->mode, "custom", sizeof ( p->mode ) );
            else
                g_strlcpy ( p->mode, "normal", sizeof ( p->mode ) );
            g_strlcpy ( p->status, emulator_get_speed_status_as_text ( ),
                        sizeof ( p->status ) );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_SET_SPEED:
        {
            /* BACKLOG D - nastavení emulační rychlosti dle mode.
             * Volá core funkce (emulator_*, customspeed_*) na emu vlákně.
             * max se zapíná mode=MAX, vypíná přechodem na NORMAL/CUSTOM.
             * STEP nemění warp flag, jen custom %. */
            st_DBGAPI_SET_SPEED_PARAM *p =
                (st_DBGAPI_SET_SPEED_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };

            switch ( p->mode )
            {
                case DBGAPI_SPEED_MODE_NORMAL:
                    emulator_switch_to_normal_speed ( );
                    rq->success = true;
                    break;

                case DBGAPI_SPEED_MODE_CUSTOM:
                    /* Přesné % - NEpoužíváme switch_to_custom_speed()
                     * (= ten obnovuje previous jen z 100 %), místo toho
                     * přímý set_request + vypnutí warpu. */
                    customspeed_set_request ( p->percent );
                    emulator_max_speed ( false );
                    rq->success = true;
                    break;

                case DBGAPI_SPEED_MODE_MAX:
                    emulator_max_speed ( true );
                    rq->success = true;
                    break;

                case DBGAPI_SPEED_MODE_STEP:
                    if ( p->step > 0 )
                        customspeed_step_up_request ( p->step );
                    else if ( p->step < 0 )
                        customspeed_step_down_request ( -p->step );
                    /* step == 0 = no-op, stále success. */
                    rq->success = true;
                    break;

                default:
                    rq->success = false;
                    break;
            };

            /* Echo nastaveného stavu po operaci (= klient nemusí volat
             * GET_SPEED zvlášť).
             *
             * POZOR: customspeed má dvě hodnoty - "requested" (co klient
             * právě nastavil) a "current" (= co je reálně aplikováno).
             * Current se z requested aktualizuje až při zpracování
             * MZEVENT_CUSTOM_SPEED_SYNCHRONISATION (= per snímek za běhu
             * emulace, customspeed_event.c). Pokud je emulace pozastavena,
             * current se neaktualizuje. Proto echo reportuje REQUESTED
             * (= deterministické potvrzení požadavku klienta). Pro reálný
             * aplikovaný stav slouží GET_SPEED (= current). max_speed flag
             * je aplikován okamžitě (g_emulator.max_speed), takže je v obou
             * konzistentní. */
            if ( rq->success )
            {
                p->out_current_percent =
                    (uint32_t) g_customspeed.speed_in_percentage_requested;
                p->out_max_speed = EMULATOR_TEST_MAX_SPEED ? 1 : 0;
                if ( EMULATOR_TEST_MAX_SPEED )
                    g_strlcpy ( p->out_mode, "max", sizeof ( p->out_mode ) );
                else if ( p->out_current_percent != 100 )
                    g_strlcpy ( p->out_mode, "custom",
                                sizeof ( p->out_mode ) );
                else
                    g_strlcpy ( p->out_mode, "normal",
                                sizeof ( p->out_mode ) );
            };
            break;
        }

        case DBGAPI_CMD_CMT_TRANSPORT:
        {
            /* CMT-A: ovládání transportu reálné páskové emulace.
             * Transport funkce (cmt_play/stop/pause/eject) běží na emu
             * vlákně a samy validují stav (= no-op pokud nelze provést).
             * Proto out_result = 0 a success = true i pro no-op.
             * stop/eject bez vložené pásky nejsou no-op: uvedou transport
             * do STOP (invariant st_CMT), takže success odpovídá stavu. */
            st_DBGAPI_CMT_TRANSPORT_PARAM *p =
                (st_DBGAPI_CMT_TRANSPORT_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_result = 0;
            switch ( p->action )
            {
                case DBGAPI_CMT_TRANSPORT_PLAY:
                    cmt_play ( );
                    break;
                case DBGAPI_CMT_TRANSPORT_PLAY_PAUSED:
                    cmt_play_paused ( );
                    break;
                case DBGAPI_CMT_TRANSPORT_STOP:
                    cmt_stop ( );
                    break;
                case DBGAPI_CMT_TRANSPORT_PAUSE:
                    cmt_pause ( p->pause_value ? 1 : 0 );
                    break;
                case DBGAPI_CMT_TRANSPORT_EJECT:
                    cmt_eject ( );
                    break;
                default:
                    p->out_result = -1;
                    rq->success = false;
                    break;
            };
            if ( p->out_result == 0 )
                rq->success = true;
            break;
        }

        case DBGAPI_CMD_CMT_RECORD:
        {
            /* CMT-A: zahájení WAV nahrávání do souboru bez file dialogu.
             * cmt_record_to_file vrátí EXIT_SUCCESS/EXIT_FAILURE; chyba
             * (nezapisovatelná cesta, špatný stav) -> success = false. */
            st_DBGAPI_CMT_RECORD_PARAM *p =
                (st_DBGAPI_CMT_RECORD_PARAM *) rq->data_ptr;
            if ( !p || !p->filepath || p->filepath[ 0 ] == '\0' )
            {
                if ( p ) p->out_result = -1;
                rq->success = false;
                break;
            };
            int rc = cmt_record_to_file ( p->filepath );
            if ( rc == EXIT_SUCCESS )
            {
                p->out_result = 0;
                rq->success = true;
            }
            else
            {
                p->out_result = -2;
                rq->success = false;
            };
            break;
        }

        case DBGAPI_CMD_CMT_HACK_SET:
        {
            /* CMT-A: zapnutí/vypnutí cmthack ROM patche (instant load).
             * mzarch varianta (= general cmthack_load_rom_patch na ni
             * jen deleguje, viz cmthack.c). out_installed echo reálného
             * stavu po operaci. */
            st_DBGAPI_CMT_HACK_SET_PARAM *p =
                (st_DBGAPI_CMT_HACK_SET_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            cmthack_mzarch_load_rom_patch ( p->enabled ? 1u : 0u );
            p->out_installed = (uint8_t)( CMTHACK_TEST_IS_INSTALLED ? 1 : 0 );
            rq->success = true;
            break;
        }

        case DBGAPI_CMD_CMT_SET_PROPERTY:
        {
            /* CMT-B: nastavení vlastnosti CMT (rychlost/polarita/cpu
             * boost/mzfsize check). SPEED validuje en_CMTSPEED rozsah
             * (1..9); ostatní vlastnosti berou boolean (0/1). Neplatná
             * property nebo hodnota -> out_result = -1, success = false. */
            st_DBGAPI_CMT_SET_PROPERTY_PARAM *p =
                (st_DBGAPI_CMT_SET_PROPERTY_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_result = 0;
            switch ( p->property )
            {
                case DBGAPI_CMT_PROP_SPEED:
                    /* poměr, nebo CMTSPEED_CUSTOM = uložené vlastní pulzy */
                    if ( ( !cmtspeed_is_valid ( (en_CMTSPEED) p->value ) )
                         && ( !cmtspeed_is_custom ( (en_CMTSPEED) p->value ) ) )
                    {
                        p->out_result = -1;
                        rq->success = false;
                    }
                    else
                    {
                        cmt_change_speed ( (en_CMTSPEED) p->value );
                        rq->success = true;
                    };
                    break;
                case DBGAPI_CMT_PROP_CUSTOM_PULSES:
                {
                    st_MZTAPE_PULSES_LENGTH pulses;
                    if ( ( EXIT_SUCCESS != mztape_pulses_set_us ( &pulses, p->pulses_us[ 0 ], p->pulses_us[ 1 ],
                                                                   p->pulses_us[ 2 ], p->pulses_us[ 3 ] ) )
                         || ( EXIT_SUCCESS != cmt_change_custom_pulses ( &pulses ) ) )
                    {
                        p->out_result = -1;
                        rq->success = false;
                    }
                    else
                    {
                        rq->success = true;
                    };
                    break;
                }
                case DBGAPI_CMT_PROP_POLARITY:
                    cmt_rear_dip_switch_cmt_inverted_polarity ( p->value ? 1u : 0u );
                    rq->success = true;
                    break;
                case DBGAPI_CMT_PROP_CPU_BOOST:
                    cmt_cpu_boost_set ( p->value ? CMT_CPU_BOOST_ENABLED
                                                 : CMT_CPU_BOOST_DISABLED );
                    rq->success = true;
                    break;
                case DBGAPI_CMT_PROP_MZFSIZE_CHECK:
                    cmt_mzfsize_check_set ( p->value ? CMT_MZFSIZE_CHECK_ENABLED
                                                     : CMT_MZFSIZE_CHECK_DISABLED );
                    rq->success = true;
                    break;
                default:
                    p->out_result = -1;
                    rq->success = false;
                    break;
            };
            break;
        }

        case DBGAPI_CMD_CMT_OPEN:
        {
            /* CMT-B: otevření CMT souboru (non-UI). cmt_open_file_by_
             * extension udělá eject + open; při play_immediately handler
             * navíc zavolá cmt_play (= jako cmt_ui_open_cb). Selhání
             * openu -> out_result = -2, success = false. Vždy vyplní
             * out_state / out_paused podle skutečného stavu. */
            st_DBGAPI_CMT_OPEN_PARAM *p =
                (st_DBGAPI_CMT_OPEN_PARAM *) rq->data_ptr;
            if ( !p || !p->filepath || p->filepath[ 0 ] == '\0' )
            {
                if ( p ) p->out_result = -1;
                rq->success = false;
                break;
            };
            /* cmt_open_file_by_extension bere char* (ne const); jen čte. */
            int rc = cmt_open_file_by_extension ( (char *) p->filepath );
            if ( rc == EXIT_SUCCESS )
            {
                if ( p->play_immediately )
                    cmt_play ( );
                p->out_result = 0;
                rq->success = true;
            }
            else
            {
                p->out_result = -2;
                rq->success = false;
            };
            /* Skutečný stav transportu po operaci - dispatch z něj hlásí
             * "playing" (dříve jen echo play_immediately). */
            p->out_state = (uint8_t) g_cmt.state;
            p->out_paused = (uint8_t) ( g_cmt.paused ? 1 : 0 );
            break;
        }

        case DBGAPI_CMD_CMT_TAPE_SEEK:
        {
            /* CMT-B: seek na blok pásky přes container->cb_open_block.
             * Vyžaduje naloženou pásku s containerem. Mimo rozsah nebo
             * bez pásky -> out_result != 0, success = false. */
            st_DBGAPI_CMT_TAPE_SEEK_PARAM *p =
                (st_DBGAPI_CMT_TAPE_SEEK_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            if ( ( !CMT_TEST_FILLED ) || ( !g_cmt.ext ) || ( !g_cmt.ext->container )
                 || ( !g_cmt.ext->container->cb_open_block ) )
            {
                p->out_result = -1;
                rq->success = false;
                break;
            };
            int rc = g_cmt.ext->container->cb_open_block ( p->block_id );
            if ( rc == EXIT_SUCCESS )
            {
                p->out_result = 0;
                rq->success = true;
            }
            else
            {
                p->out_result = -2;
                rq->success = false;
            };
            break;
        }

        case DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED:
        {
            /* CMT-B: per-blok cmt rychlost (= jediný per-blok parametr,
             * per Michal). Vyžaduje naloženou pásku s containerem a
             * platnou en_CMTSPEED hodnotu (1..9). */
            st_DBGAPI_CMT_TAPE_BLOCK_SPEED_PARAM *p =
                (st_DBGAPI_CMT_TAPE_BLOCK_SPEED_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            /* Validace (páska, SIMPLE_TAPE, rozsah, rychlost) a nastavení
             * blspeed = SET + cmtspeed dělá cmt_tape_set_block_cmt_speed()
             * (-1 = páska/blok, -2 = neplatná rychlost). */
            if ( p->use_pulses )
            {
                /* vlastní délky pulzů: blok dostane CMTSPEED_CUSTOM */
                st_MZTAPE_PULSES_LENGTH pulses;
                if ( EXIT_SUCCESS != mztape_pulses_set_us ( &pulses, p->pulses_us[ 0 ], p->pulses_us[ 1 ],
                                                            p->pulses_us[ 2 ], p->pulses_us[ 3 ] ) )
                    p->out_result = -2;
                else
                    p->out_result = cmt_tape_set_block_pulses ( p->block_id, &pulses );
            }
            else
            {
                p->out_result = cmt_tape_set_block_cmt_speed ( p->block_id,
                                                               (en_CMTSPEED) p->cmtspeed );
            };
            rq->success = ( p->out_result == 0 );
            break;
        }

        case DBGAPI_CMD_CMT_TAPE_LIST:
        {
            /* CMT-B: read-only výpis bloků pásky (backing pro resource
             * emulator://periph/cmt/tape). Pro nenaloženou pásku /
             * chybějící container available = 0, out_count = 0. Iterujeme
             * count_blocks, plníme fixní buffery (clamp name). */
            st_DBGAPI_CMT_TAPE_LIST_PARAM *p =
                (st_DBGAPI_CMT_TAPE_LIST_PARAM *) rq->data_ptr;
            if ( !p || !p->entries )
            {
                if ( p ) { p->out_count = 0; p->available = 0; }
                rq->success = false;
                break;
            };
            p->out_count      = 0;
            p->available      = 0;
            p->container_type = 0;
            p->current_block  = -1;
            if ( ( !CMT_TEST_FILLED ) || ( !g_cmt.ext ) )
            {
                rq->success = true; /* read = úspěch, jen available=0 */
                break;
            };
            st_CMTEXT_CONTAINER *container = cmtext_get_container ( g_cmt.ext );
            if ( !container )
            {
                rq->success = true;
                break;
            };
            en_CMTEXT_CONTAINER_TYPE ctype =
                cmtext_container_get_type ( container );
            p->available      = 1;
            p->container_type = (uint8_t) ctype;
            if ( g_cmt.ext->block )
                p->current_block = cmtext_block_get_block_id ( g_cmt.ext->block );
            /* cmtext_is_playable/recordable vracejí EXIT_SUCCESS (0)
             * pro "ano" - převod na bool (dříve obráceně: MZF hlásilo
             * playable = false, recordable = true). */
            bool playable   = ( EXIT_SUCCESS == cmtext_is_playable ( g_cmt.ext ) );
            bool recordable = ( EXIT_SUCCESS == cmtext_is_recordable ( g_cmt.ext ) );

            if ( ctype != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE )
            {
                /* SINGLE container nemá tape index (container->tape ==
                 * NULL); per-blok get_* funkce by dereferencovaly NULL.
                 * Reprezentujeme jako jeden syntetický blok: název z
                 * containeru, rychlost z aktuální default cmt speed. */
                if ( p->capacity >= 1 )
                {
                    st_DBGAPI_CMT_TAPE_BLOCK_ENTRY *e = &p->entries[ 0 ];
                    e->block_id   = 0;
                    e->cmtspeed   = (int) g_cmt.mz_cmtspeed;
                    e->type       = (uint8_t) CMTEXT_BLOCK_TYPE_MZF;
                    e->block_speed = (uint8_t) cmtext_block_get_block_speed ( g_cmt.ext->block );
                    st_MZTAPE_PULSES_LENGTH pulses;
                    e->has_pulses = (uint8_t)( 0 == cmt_tape_get_block_pulses ( 0, &pulses ) ? 1 : 0 );
                    if ( e->has_pulses )
                        dbgapi_cmt_pulses_to_us ( &pulses, e->pulses_us );
                    e->is_current = (uint8_t)( p->current_block == 0 ? 1 : 0 );
                    e->playable   = (uint8_t)( playable ? 1 : 0 );
                    e->recordable = (uint8_t)( recordable ? 1 : 0 );
                    const char *cname = cmtext_container_get_name ( container );
                    if ( cname )
                        g_strlcpy ( e->name, cname, sizeof ( e->name ) );
                    else
                        e->name[ 0 ] = '\0';
                    p->out_count = 1;
                };
                rq->success = true;
                break;
            };

            /* SIMPLE_TAPE: má tape index, per-blok get_* jsou bezpečné. */
            int count = cmtext_container_get_count_blocks ( container );
            for ( int i = 0; i < count; i++ )
            {
                if ( p->out_count >= p->capacity )
                    break;
                st_DBGAPI_CMT_TAPE_BLOCK_ENTRY *e = &p->entries[ p->out_count ];
                e->block_id   = i;
                e->cmtspeed   = (int) cmtext_container_get_block_cmt_speed ( container, i );
                e->type       = (uint8_t) cmtext_container_get_block_type ( container, i );
                e->block_speed = (uint8_t) cmtext_container_get_block_speed ( container, i );
                st_MZTAPE_PULSES_LENGTH pulses;
                e->has_pulses = (uint8_t)( 0 == cmt_tape_get_block_pulses ( i, &pulses ) ? 1 : 0 );
                if ( e->has_pulses )
                    dbgapi_cmt_pulses_to_us ( &pulses, e->pulses_us );
                e->is_current = (uint8_t)( i == p->current_block ? 1 : 0 );
                e->playable   = (uint8_t)( playable ? 1 : 0 );
                e->recordable = (uint8_t)( recordable ? 1 : 0 );
                const char *fname = cmtext_container_get_block_fname ( container, i );
                if ( fname )
                    g_strlcpy ( e->name, fname, sizeof ( e->name ) );
                else
                    e->name[ 0 ] = '\0';
                p->out_count++;
            };
            rq->success = true;
            break;
        }
#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */

        default:
            /* Neznámý příkaz */
            g_warning("dbgapi: unknown command %d", cmd);
            rq->success = false;
            break;
    };

    if (dev_locked) device_state_unlock();

    /* Revize stavu (state_rev.h, Z3): příkaz, který při vyřízení mohl
     * změnit stav stroje (tabulka vlastností, nezařazený také). Ne při
     * odmítnutí branou záznamu vstupů a ne při nesplněné podmínce zápisu
     * (nic se neprovedlo); jiný neúspěch zvedne (mohl provést část). */
    if (!rev_stop && !rp_refused && dbgapi_cmd_mutates(cmd) && !dbgapi_cmd_precondition_failed(rq))
    {
        STATE_REV_BUMP(STATE_REV_SRC_DEBUGGER);
    };
    g_state_rev_src_override = rev_src_prev;

    run_control_after_cmd ( (unsigned) cmd, (int) rq->cmd_origin, rc_was_paused, rc_was_step );

    /* Záznam vstupů: uložit zásah (`d` za běhu, `p` v pauze) nebo nový
     * vzorek vstupů; při přehrávání označit odchýlení. */
    if ((g_replay_mode != REPLAY_MODE_OFF) && !rp_refused && !rev_stop)
    {
        replay_dbg_after(rq, rc_was_paused);
    };

    /* MCP_ACTION broadcast hook - po úspěšném zpracování CMDRQ od MCP
     * klienta emitujeme notifikaci do UI (= budoucí Activity Log panel
     * V1.C). Pro chybové průchody (success == false) broadcast neposíláme,
     * aby Activity Log nezaplevelily failed pokusy. Výjimkou je podmíněný
     * zápis odmítnutý nesplněnou podmínkou (observe-cas M10): člověk má
     * vidět, že agent zapsat chtěl a stav se mu mezitím změnil. */
    if (rq->cmd_origin == DBGAPI_CMD_ORIGIN_MCP)
    {
        if (rq->success)
        {
            dbgapi_emit_mcp_action(rq, false);
        }
        else if (rev_failed || (!rp_refused && dbgapi_cmd_precondition_failed(rq)))
        {
            dbgapi_emit_mcp_action(rq, true);
        };
    };

    /* Výstupy pro volajícího z okamžiku po příkazu (revize, běh): MCP je
     * vrací spolu s výsledkem příkazu (N69: get_state z jednoho okamžiku). */
    if (opts)
    {
        opts->state_rev_after = g_state_rev;
        opts->running_after = !EMULATOR_TEST_PAUSED;
        opts->run_seq_after = run_log_current(NULL);
    };
}

unsigned dbgapi_emu_drain_paused(st_DBGAPI_CMDRQ_QUEUE *queue,
                                 void (*dispatch_fn)(st_DBGAPI_CMDRQ *rq))
{
    unsigned n = 0;
    while (!TEST_DEBUGGER_STEP_CALL && dbgapi_emu_has_pending(queue))
    {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(queue);
        if (!rq)
            continue;
        if (dispatch_fn)
            dispatch_fn(rq);
        else
            dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
        n++;
    };
    return n;
}

void dbgapi_emu_complete(st_DBGAPI_CMDRQ *rq)
{
    if (!rq)
        return;

    /* Zamknout slot, nastavit stav na PROCESSED, signalizovat čekající UI */
    APP_MUTEX_LOCK(rq->mutex);
    rq->cmd_state = DBGAPI_CMDSTATE_PROCESSED;
    APP_COND_SIGNAL(rq->cond);
    APP_MUTEX_UNLOCK(rq->mutex);
}

bool dbgapi_emu_wait_for_cmd(st_DBGAPI_CMDRQ_QUEUE *queue, int timeout_ms)
{
    APP_MUTEX_LOCK(queue->queue_mutex);

    /* Pokud ve frontě už něco je, hned vrátit */
    if (dbgapi_emu_has_pending_unlocked(queue))
    {
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        return true;
    };

    /* Čekat na signál (nový příkaz) nebo timeout */
    APP_COND_WAIT_TIMEOUT_MS(queue->queue_cond, queue->queue_mutex, timeout_ms);

    bool has_cmd = dbgapi_emu_has_pending_unlocked(queue);
    APP_MUTEX_UNLOCK(queue->queue_mutex);
    return has_cmd;
}

void dbgapi_emu_set_ending(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    APP_MUTEX_LOCK(queue->queue_mutex);
    queue->reply_state = DBGAPI_CMDREPLY_STATE_ENDING;
    APP_MUTEX_UNLOCK(queue->queue_mutex);
}


/* ============================================================================
 * EMU STRANA — ODESÍLÁNÍ MSG (EMU → UI)
 *
 * Volá registrovaný dispatcher (přes dbgapi_emu_register_msg_dispatcher)
 * pro doručení MSG do UI vlákna. dbgapi.c nezná SDL ani sdlapp - thread
 * switch řeší dispatcher v UI vrstvě (src/ui-imgui/debugger/dbgapi_dispatcher).
 * ============================================================================ */

void dbgapi_emu_register_msg_dispatcher(dbgapi_msg_dispatcher_t dispatcher,
                                          void *user_data)
{
    s_msg_dispatcher = dispatcher;
    s_msg_dispatcher_user_data = user_data;
}

void dbgapi_emu_send_msg(en_DBGAPI_MSG msg, st_DBGAPI_MSG_DATA *data)
{
    if (!s_msg_dispatcher)
    {
        /* Žádný dispatcher zaregistrován - zahodit MSG, uvolnit data.
         * Tento stav nastává např. v testovém prostředí bez UI, nebo
         * při shutdown po dbgapi_destroy(). Použít dbgapi_msg_data_free
         * aby se uvolnil i případný MCP_ACTION description. */
        dbgapi_msg_data_free(data);
        return;
    };

    /* Předat MSG dispatcheru. Dispatcher přebírá vlastnictví dat
     * (zodpovědnost za uvolnění po doručení nebo zahození). */
    s_msg_dispatcher(msg, data, s_msg_dispatcher_user_data);
}


/* ============================================================================
 * UI STRANA — ODESÍLÁNÍ CMDRQ (UI → EMU)
 * ============================================================================ */

/**
 * @brief Společná implementace synchronního submitu (viz
 *        dbgapi_ui_submit_cmd_sync_watched() a dbgapi_ui_submit_cmd_sync_ex()).
 *
 * @param queue           CMDRQ fronta.
 * @param cmd             Příkaz (volitelně s DBGAPI_CMDFLAG_BLOCKING).
 * @param origin          Zdroj příkazu.
 * @param data_ptr        Vstupní data (vlastní volající, platná do návratu).
 * @param result_ptr      Buffer odpovědi (vlastní volající, platný do návratu).
 * @param timeout_ms      Limit čekání na vyzvednutí emu vláknem (0 = bez limitu).
 * @param stall_ms        Dodatečná doba čekání na dokončení vyzvednutého
 *                        příkazu, po které se zavolá @p stall_cb.
 * @param stall_cb        Hlášení zaseknutého rozpracovaného příkazu (NULL =
 *                        nehlásit, čistá sémantika _ex).
 * @param stall_user_data Kontext pro @p stall_cb.
 * @return Viz en_DBGAPI_SUBMIT_STATUS.
 */
static en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_impl(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                     en_DBGAPI_CMD cmd,
                                                     en_DBGAPI_CMD_ORIGIN origin,
                                                     void *data_ptr,
                                                     void *result_ptr,
                                                     int timeout_ms,
                                                     int stall_ms,
                                                     dbgapi_submit_stall_cb_t stall_cb,
                                                     void *stall_user_data,
                                                     st_DBGAPI_SUBMIT_OPTS *opts)
{
    /* Kontrola: emulátor se neukončuje? */
    APP_MUTEX_LOCK(queue->queue_mutex);
    if (queue->reply_state == DBGAPI_CMDREPLY_STATE_ENDING)
    {
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        return DBGAPI_SUBMIT_ENDING;
    };

    /* Kontrola: fronta není plná? Když emu vlákno blokuje (např. CMT
     * hack čeká na FileBrowser odpověď), tail se vzdaluje od head a
     * fronta se rychle plní. UI klienti (CPU panel refresh tick 100ms)
     * by jinak spam-ovali warning každý sync call.
     *
     * Změna z g_warning na g_debug: pro koncového uživatele tiché (= ne
     * zaplavená console), pro dev viditelné jen s G_MESSAGES_DEBUG.
     * UI klienti by měli sami dělat self-rate-limit přes
     * dbgapi_ui_queue_is_full() PŘED submit (= preferovaná cesta). */
    int next_tail = (queue->tail + 1) % DBGAPI_CMDRQ_QUEUE_SIZE;
    if (next_tail == queue->head)
    {
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        g_debug("dbgapi: CMDRQ queue is full, command dropped");
        return DBGAPI_SUBMIT_QUEUE_FULL;
    };

    /* Slot na pozici tail. Pořadí zámků queue_mutex -> slot->mutex. */
    st_DBGAPI_CMDRQ *slot = &queue->cmdrq[queue->tail];
    APP_MUTEX_LOCK(slot->mutex);

    /* Slot může být ještě obsazený předchozím odesílatelem, který čeká na
     * dokončení rozpracovaného příkazu (emu ho vyzvedlo, head je za ním,
     * ale fronta mezitím obešla celé kolo). Přepsat ho nesmíme - chová se
     * to jako plná fronta. */
    if (slot->cmd_state != DBGAPI_CMDSTATE_NONE)
    {
        APP_MUTEX_UNLOCK(slot->mutex);
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        g_debug("dbgapi: CMDRQ slot still owned, command dropped");
        return DBGAPI_SUBMIT_QUEUE_FULL;
    };
    queue->tail = next_tail;

    /* Inicializace slotu */
    slot->cmd = cmd;
    slot->cmd_origin = origin;
    slot->cmd_state = DBGAPI_CMDSTATE_PENDING;
    slot->data_ptr = data_ptr;
    slot->result_ptr = result_ptr;
    slot->success = false;
    slot->dequeued = false;
    slot->done_early = false;
    slot->opts = opts;

    /* V1.D.1 - track last user action pro emulator://state Resource.
     * Zaznamenáváme jen origin == USER (= GUI klik / hotkey / menu).
     * MCP / TEST / INTERNAL origin ignorujeme - AI klient nás zajímá
     * jen co dělá human user. Čtení (dbgapi_cmd_is_read(): druh READ
     * a čtecí operace příkazů s operací, např. snímek pokrytí) ne: okna
     * debuggeru čtou stav periodicky a záznam by se měnil bez akce
     * člověka. Řízení běhu a konfigurace debuggeru (BP, watch) akcí
     * člověka jsou. */
    if ( origin == DBGAPI_CMD_ORIGIN_USER
         && !dbgapi_cmd_is_read ( (en_DBGAPI_CMD) ( cmd & DBGAPI_CMD_MASK ), data_ptr ) )
    {
        dbgapi_track_last_user_action ( cmd );
    };

    /* Signalizovat emulátoru, že ve frontě je nový příkaz */
    APP_COND_SIGNAL(queue->queue_cond);
    APP_MUTEX_UNLOCK(queue->queue_mutex);

    /* Čekat na zpracování příkazu emulátorem. Vždy ve smyčce nad stavem
     * slotu - podmínková proměnná se smí probudit i bez signálu. */
    if (timeout_ms > 0)
    {
        gint64 deadline_us = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
        while (slot->cmd_state != DBGAPI_CMDSTATE_PROCESSED)
        {
            gint64 left_us = deadline_us - g_get_monotonic_time();
            if (left_us <= 0)
                break;
            gint32 left_ms = (gint32)((left_us + 999) / 1000);
            APP_COND_WAIT_TIMEOUT_MS(slot->cond, slot->mutex, left_ms);
        };
    }
    else
    {
        /* Neomezené čekání */
        while (slot->cmd_state != DBGAPI_CMDSTATE_PROCESSED)
            APP_COND_WAIT(slot->cond, slot->mutex);
    };

    if (slot->cmd_state != DBGAPI_CMDSTATE_PROCESSED)
    {
        /* Timeout. O zrušení se rozhoduje pod queue_mutex (tam emu nastavuje
         * `dequeued`); kvůli pořadí zámků queue -> slot nejdřív pustíme slot. */
        APP_MUTEX_UNLOCK(slot->mutex);
        APP_MUTEX_LOCK(queue->queue_mutex);
        APP_MUTEX_LOCK(slot->mutex);

        if (!slot->dequeued && slot->cmd_state == DBGAPI_CMDSTATE_PENDING)
        {
            /* Emu slot ještě nevyzvedlo: zrušit. Emu ho přeskočí a uvolní
             * (dbgapi_emu_dequeue), data klienta už nikdo nečte. */
            slot->cmd_state = DBGAPI_CMDSTATE_CANCELLED;
            slot->cmd = DBGAPI_CMD_NONE;
            slot->cmd_origin = DBGAPI_CMD_ORIGIN_USER;
            slot->data_ptr = NULL;
            slot->result_ptr = NULL;
            slot->opts = NULL;
            APP_MUTEX_UNLOCK(slot->mutex);
            APP_MUTEX_UNLOCK(queue->queue_mutex);
            return DBGAPI_SUBMIT_TIMEOUT;
        };
        APP_MUTEX_UNLOCK(queue->queue_mutex);

        /* Emu příkaz právě zpracovává (nebo ho mezitím dokončilo). Slot
         * nesmíme opustit - emu pracuje s data_ptr/result_ptr klienta,
         * která typicky leží na zásobníku odesílatele. Proto se tu čeká
         * vždy až do dokončení; handlery dispatch na odesílatele nečekají.
         *
         * Volající, který potřebuje omezenou dobu odpovědi (MCP), si nechá
         * po dalších stall_ms ohlásit zaseknutí přes stall_cb. Odpověď
         * klientovi pak pošle jiné vlákno, toto vlákno (a s ním i data
         * na jeho zásobníku) dál čeká na dokončení slotu. */
        if (stall_cb && slot->cmd_state != DBGAPI_CMDSTATE_PROCESSED)
        {
            gint64 stall_deadline_us = g_get_monotonic_time()
                                       + (gint64)(stall_ms > 0 ? stall_ms : 0) * 1000;
            while (slot->cmd_state != DBGAPI_CMDSTATE_PROCESSED)
            {
                gint64 left_us = stall_deadline_us - g_get_monotonic_time();
                if (left_us <= 0)
                    break;
                gint32 left_ms = (gint32)((left_us + 999) / 1000);
                APP_COND_WAIT_TIMEOUT_MS(slot->cond, slot->mutex, left_ms);
            };
            if (slot->cmd_state != DBGAPI_CMDSTATE_PROCESSED)
            {
                /* Slot zůstává náš (cmd_state != NONE), nikdo jiný ho
                 * nepřevezme; zámek pouštíme jen kvůli callbacku, aby
                 * nemohl vzniknout lock-order problém se zámky volajícího. */
                APP_MUTEX_UNLOCK(slot->mutex);
                stall_cb(stall_user_data);
                APP_MUTEX_LOCK(slot->mutex);
            };
        };
        while (slot->cmd_state != DBGAPI_CMDSTATE_PROCESSED)
            APP_COND_WAIT(slot->cond, slot->mutex);
    };

    /* Přečíst výsledek */
    en_DBGAPI_SUBMIT_STATUS status = slot->success ? DBGAPI_SUBMIT_OK
                                                   : DBGAPI_SUBMIT_FAILED;

    /* Uvolnit slot */
    slot->cmd_state = DBGAPI_CMDSTATE_NONE;
    slot->cmd = DBGAPI_CMD_NONE;
    slot->cmd_origin = DBGAPI_CMD_ORIGIN_USER;
    slot->data_ptr = NULL;
    slot->result_ptr = NULL;
    slot->opts = NULL;

    APP_MUTEX_UNLOCK(slot->mutex);

    return status;
}


en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_ex(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                     en_DBGAPI_CMD cmd,
                                                     en_DBGAPI_CMD_ORIGIN origin,
                                                     void *data_ptr,
                                                     void *result_ptr,
                                                     int timeout_ms)
{
    return dbgapi_ui_submit_impl(queue, cmd, origin, data_ptr, result_ptr,
                                 timeout_ms, 0, NULL, NULL, NULL);
}


en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_watched(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                          en_DBGAPI_CMD cmd,
                                                          en_DBGAPI_CMD_ORIGIN origin,
                                                          void *data_ptr,
                                                          void *result_ptr,
                                                          int timeout_ms,
                                                          int stall_ms,
                                                          dbgapi_submit_stall_cb_t stall_cb,
                                                          void *stall_user_data)
{
    return dbgapi_ui_submit_impl(queue, cmd, origin, data_ptr, result_ptr,
                                 timeout_ms, stall_ms, stall_cb,
                                 stall_user_data, NULL);
}


en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_opts(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                       en_DBGAPI_CMD cmd,
                                                       en_DBGAPI_CMD_ORIGIN origin,
                                                       void *data_ptr,
                                                       void *result_ptr,
                                                       int timeout_ms,
                                                       int stall_ms,
                                                       dbgapi_submit_stall_cb_t stall_cb,
                                                       void *stall_user_data,
                                                       st_DBGAPI_SUBMIT_OPTS *opts)
{
    return dbgapi_ui_submit_impl(queue, cmd, origin, data_ptr, result_ptr,
                                 timeout_ms, stall_ms, stall_cb,
                                 stall_user_data, opts);
}


bool dbgapi_ui_submit_cmd_sync_with_origin(st_DBGAPI_CMDRQ_QUEUE *queue,
                                            en_DBGAPI_CMD cmd,
                                            en_DBGAPI_CMD_ORIGIN origin,
                                            void *data_ptr,
                                            void *result_ptr,
                                            int timeout_ms)
{
    return dbgapi_ui_submit_cmd_sync_ex(queue, cmd, origin, data_ptr,
                                        result_ptr, timeout_ms)
           == DBGAPI_SUBMIT_OK;
}


bool dbgapi_ui_submit_cmd_sync(st_DBGAPI_CMDRQ_QUEUE *queue,
                                en_DBGAPI_CMD cmd,
                                void *data_ptr,
                                void *result_ptr,
                                int timeout_ms)
{
    /* Backward compat wrapper - implicit origin USER pro existující GUI
     * callsites. MCP wrapper a test framework volají _with_origin přímo. */
    return dbgapi_ui_submit_cmd_sync_with_origin(queue, cmd,
                                                  DBGAPI_CMD_ORIGIN_USER,
                                                  data_ptr, result_ptr,
                                                  timeout_ms);
}

bool dbgapi_ui_queue_is_full(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    APP_MUTEX_LOCK(queue->queue_mutex);
    int next_tail = (queue->tail + 1) % DBGAPI_CMDRQ_QUEUE_SIZE;
    bool full = (next_tail == queue->head);
    APP_MUTEX_UNLOCK(queue->queue_mutex);
    return full;
}

bool dbgapi_ui_is_ending(st_DBGAPI_CMDRQ_QUEUE *queue)
{
    APP_MUTEX_LOCK(queue->queue_mutex);
    bool ending = (queue->reply_state == DBGAPI_CMDREPLY_STATE_ENDING);
    APP_MUTEX_UNLOCK(queue->queue_mutex);
    return ending;
}


/* ============================================================================
 * UI STRANA — REGISTRACE MSG CALLBACKU
 * ============================================================================ */

void dbgapi_ui_register_msg_callback(dbgapi_msg_callback_t callback, void *user_data)
{
    s_msg_callback = callback;
    s_msg_callback_user_data = user_data;
}

void dbgapi_ui_unregister_msg_callback(void)
{
    s_msg_callback = NULL;
    s_msg_callback_user_data = NULL;
}

void dbgapi_ui_invoke_msg_callback(en_DBGAPI_MSG msg, st_DBGAPI_MSG_DATA *data)
{
    if (!s_msg_callback)
    {
        /* Žádný listener není zaregistrován - data uvolnit a vrátit.
         * Použijeme dbgapi_msg_data_free aby se korektně uvolnil i
         * případný description (MCP_ACTION payload). */
        dbgapi_msg_data_free(data);
        return;
    };

    /* Volat registrovaný listener. Listener je zodpovědný za uvolnění
     * data (per kontrakt dbgapi_msg_callback_t) - doporučená cesta je
     * dbgapi_msg_data_free, která pokryje i MCP_ACTION description. */
    s_msg_callback(msg, data, s_msg_callback_user_data);
}


/* ============================================================================
 * UVOLNĚNÍ MSG DAT (vč. MCP_ACTION description)
 * ============================================================================ */

void dbgapi_msg_data_free(st_DBGAPI_MSG_DATA *data)
{
    if (!data)
        return;
    /* description je heap-alokovaný (g_strdup) pouze pro MCP_ACTION; pro
     * ostatní MSG je NULL. g_free(NULL) je bezpečné, ale pro čistotu
     * podmínku ponecháme. */
    if (data->description)
    {
        g_free(data->description);
        data->description = NULL;
    };
    g_free(data);
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
