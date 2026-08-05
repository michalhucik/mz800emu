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
#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
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
#include "customspeed.h"
#include "bptmap.h"
#include "breakpoints.h"
#include "stack_regions.h"
#include "stack_history.h"
#include "callstack.h"
#include "profiler.h"
#include "watch.h"
#include "watch_emu_cache.h"  /* V1.D.2.C: dispatch-side mirror lookup */
#include "bp_vars.h"
#include "bookmarks/bookmarks.h"
#include "bp_expr.h"
#include "mhmap.h"
#include "png_encode.h"
#include "trace/eventlog.h"
#include "trace/cputrack.h"
#include "trace/iorqlog.h"
#include "trace/intlog.h"
#include "trace/hwlog.h"
#include "snapshot/snapshot.h"
#include "symbols/sym_db.h"
#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform.h"
#include "mzarch/mzarch_platform_functions.h"
#include "libs/dasm-z80/z80_dasm.h"
#include "gdg/video.h"
#include "memory/memory.h"
#include "hw-generic/gdg/gdg_state.h"
#include "hw-generic/gdg/gdg.h" /* per-arch gettery (gdg_get_regct53g7) */
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/pio8255/pio8255.h"
#include "mzarch/mzhal.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/psg/psg.h"
/* Kontrakt: IORQ callbacky implementuje kazda architektura ve svem
 * mz*_iorq.c se shodnymi prototypy (viz mz*_iorq.h). */
extern uint8_t port_read_cb(z80_t *cpu, uint16_t addr, void *user_data);
extern void port_write_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data);
extern void mzarch_run_to_temporary_breakpoint(void);
#include "libs/cpu-z80/z80.h"

/* V1.B.1 - Media Tools (mcp-server mutant). Handler v switchi pro
 * DBGAPI_CMD_MEDIA_* dispatchuje na jednotlivé hw-generic API. Periferie
 * jsou per-arch volitelné přes CFG_HWEXT_HAVE_* makra v mzarch_config.h. */
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmthack.h"
/* fix mzdos 0008: media_load_mzf zrcadlí bootstrap.c plný load (header +
 * post-header mapping + body). Potřebujeme post_header per-arch + MZF
 * header strukturu. */
#include "mzarch/bootstrap.h"
#include "libs/mzf/mzf.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/ide8/ide8.h"

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "mcp/event_bus.h"
/* V1.C.1 - HID Tools: VKBD matrix press/release + joystick state. */
#include "mcp/hid_keymap.h"
/* V1.D.1 - Core + CPU extras Resources: čte memext + memory + z80 state. */
#include "hw-generic/memory/memext.h"
/* V1.D.4 - Input + Frame Resources: joystick, framebuffer, VRAM read. */
#include "hw-generic/joy/joy.h"
#include "iface/iface_video.h"
#include "hw-generic/gdg/framebuffer.h"
#endif

/* V1.B.2 - cfgmain INI handle pro Settings + Periph attach handlery. */
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "libs/cfgfile/cfgcommon.h"


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
 * Eviduje poslední CMDRQ s cmd_origin == DBGAPI_CMD_ORIGIN_USER.
 * Slouží AI klientovi (přes emulator://state Resource) ke zjištění, co
 * naposledy user v GUI udělal - umožní AI lépe spolupracovat s uživatelem
 * (= "user právě klikl Run, počkám než se zastaví").
 *
 * Thread-safety: nastavuje se z UI vlákna v dbgapi_ui_submit_cmd_sync_with_origin
 * po vložení slotu (= mimo emu thread). Čte se z emu vlákna v handleru
 * get_state. Pro V1.D.1 použijeme jednoduchý GLib mutex - příště lze
 * převést na atomic load/store struct (= per-field race-free, ale není
 * to kritické, last action je informativní).
 * ============================================================================ */

typedef struct st_DBGAPI_LAST_USER_ACTION
{
    bool          valid;        /**< false = ještě žádná USER akce nebyla. */
    en_DBGAPI_CMD cmd;          /**< Poslední USER CMD. */
    uint64_t      timestamp_us; /**< g_get_monotonic_time() v okamžiku submit. */
} st_DBGAPI_LAST_USER_ACTION;

static st_DBGAPI_LAST_USER_ACTION s_last_user_action = {
    .valid = false,
    .cmd = DBGAPI_CMD_NONE,
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
 * Volá se z dbgapi_ui_submit_cmd_sync_with_origin pro origin == USER.
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
    s_last_user_action.timestamp_us = (uint64_t) g_get_monotonic_time ( );
    g_mutex_unlock ( &s_last_user_action_mutex );
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

    /* Fronta prázdná? */
    if (!dbgapi_emu_has_pending_unlocked(queue))
    {
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        return NULL;
    };

    /* Vyjmout slot z hlavy fronty */
    st_DBGAPI_CMDRQ *slot = &queue->cmdrq[queue->head];
    queue->head = (queue->head + 1) % DBGAPI_CMDRQ_QUEUE_SIZE;

    APP_MUTEX_UNLOCK(queue->queue_mutex);
    return slot;
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
        /* V1.B.1 - Media Tools */
        case DBGAPI_CMD_MEDIA_LOAD_MZF:            return "media_load_mzf";
        case DBGAPI_CMD_MEDIA_LOAD_BINARY:         return "media_load_binary";
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
 * @param rq  Zpracovaný CMDRQ slot (cmd a cmd_origin už nastaveny).
 */
static void dbgapi_emit_mcp_action(st_DBGAPI_CMDRQ *rq)
{
    en_DBGAPI_CMD cmd = (en_DBGAPI_CMD)(rq->cmd & DBGAPI_CMD_MASK);
    st_DBGAPI_MSG_DATA *data = g_new0(st_DBGAPI_MSG_DATA, 1);
    data->msg_type = DBGAPI_MSG_MCP_ACTION;
    data->cmd = cmd;
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
            return true;
        case DBGAPI_TRACE_CHANNEL_IORQLOG:
            out->mode_ptr = (int *) &g_iorqlog_config.mode;
            out->fn_reset = NULL;
            out->fn_save  = iorqlog_save_segment;
            return true;
        case DBGAPI_TRACE_CHANNEL_INTLOG:
            out->mode_ptr = (int *) &g_intlog_config.mode;
            out->fn_reset = NULL;
            out->fn_save  = intlog_save_segment;
            return true;
        case DBGAPI_TRACE_CHANNEL_HWLOG:
            out->mode_ptr = (int *) &g_hwlog_config.mode;
            out->fn_reset = NULL;
            out->fn_save  = hwlog_save_segment;
            return true;
        default:
            return false;
    }
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


void dbgapi_emu_dispatch(st_DBGAPI_CMDRQ *rq)
{
    if (!rq)
        return;

    /* Extrahovat příkaz bez BLOCKING flagu */
    en_DBGAPI_CMD cmd = (en_DBGAPI_CMD)(rq->cmd & DBGAPI_CMD_MASK);

    /*
     * Dispatch příkazů — zatím základní implementace.
     * Konkrétní handlery budou doplněny až budou k dispozici
     * příslušné moduly emulátoru (debugger.h, bptmap.h, cpu-z80, ...).
     *
     * TODO: doplnit handlery pro jednotlivé příkazy
     */
    switch (cmd)
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
            /* Aktivuje debugger (= nastaví g_debugger.active = 1).
             * Side effect: TEST_DEBUGGER_CPUHIST_ACTIVE / MHMAP_ACTIVE
             * v default WITH_WINDOW režimu se zapne, takže CPU instrukční
             * historie a memory heatmap začnou zaznamenávat.
             * Forward na přímou manipulaci globálu (ekvivalent dnešního UI
             * volání debugger_show_main_window()). */
            g_debugger.active = 1;
            rq->success = true;
            break;

        case DBGAPI_CMD_DEBUGGER_DEACTIVATE:
            /* Deaktivuje debugger (= g_debugger.active = 0).
             * Side effect: cpuhist a mhmap recording v WITH_WINDOW režimu
             * se vypne. */
            g_debugger.active = 0;
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
            /* Pozastaví emulaci. Forward na emulator_pause(true).
             * Side effecty (z emulator.c:273): MZ800_MAIN_SET_EVENT
             * BREAK_EMULATION_PAUSED, audio pause, UI state update,
             * pokud TEST_DEBUGGER_ACTIVE pak hide spinner + reset
             * temporary BP. */
            g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
            emulator_pause ( true );
            rq->success = true;
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
            /* V1.A.4: MCP EVENT emit "paused" pro klienty na topicu.
             * Reason rozlišuje BP hit vs manual pause vs fatal -
             * tady jsme v dbgapi PAUSE handleru, takže reason=manual. */
            {
                JsonObject *p = json_object_new ( );
                json_object_set_string_member ( p, "reason", "manual" );
                json_object_set_int_member ( p, "pc",
                                              (int) g_mzarch_main.cpu->pc );
                event_bus_emit ( "paused", p );
            }
#endif
            break;

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
             * flag. */
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
             * problém. */
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
                g_debugger.run_frames_target =
                    g_gdg.total_elapsed.screens + (uint32_t) frames;
                g_debugger.run_frames_active = 1;
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
            debugger_step_call ( 1 );
            rq->success = true;
            break;

        case DBGAPI_CMD_STEP_OVER:
            /* Step Over - CALL/RST/DJNZ/blokové instrukce: nastaví temp BP
             * na addr+length a spustí run-to-temp-BP. Ostatní: step into.
             * Interní helper dbgapi_emu_do_step_over() (= replika logiky
             * z dbg_iconbar.cpp). Pokud emu běží, caller musí pause před
             * voláním (default UX). */
            if ( EMULATOR_TEST_PAUSED )
            {
                dbgapi_emu_do_step_over ( );
            }
            else
            {
                emulator_pause ( true );
            };
            rq->success = true;
            break;

        case DBGAPI_CMD_RUN_TO:
            /* Run To Cursor / Run To Address: nastaví dočasný BP na
             * cílovou adresu a spustí emulaci přes
             * mzarch_run_to_temporary_breakpoint(). Cílová adresa v
             * data_ptr (= uint16_t*). Pokud emu běží, pause + return
             * (= UX z dbg_iconbar.cpp::dbg_do_run_to_cursor). */
            if ( !rq->data_ptr )
            {
                rq->success = false;
                break;
            };
            if ( !EMULATOR_TEST_PAUSED )
            {
                emulator_pause ( true );
                rq->success = true;
                break;
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
             * loop (mzarch.c:594) okamžitě a zachová pause. */
            mzarch_platform_fn_reset_request ( );
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
             * st_DBGAPI_REG_PARAM* (= reg_id + value). Pro IR registr
             * je speciální handling (jen dolní bajt = R), shodný s
             * debugger_change_z80_register(). Forward přes přímý
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
                    if ((z80_reg_t)p->reg_id == Z80_REG_IR)
                    {
                        /* Specialni handling: nastavit jen dolni bajt R,
                         * I (vysoky bajt) zachovat - shodne s
                         * debugger_change_z80_register(). */
                        g_mzarch_main.cpu->r =
                            (uint8_t)(p->value & 0x7F) |
                            (g_mzarch_main.cpu->r & 0x80);
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
             * debugger_memory_read_byte() (= memory_read_byte přes
             * banking, BEZ side effects pro VRAM/IORQ ports). */
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
                    r->bp[i].id = b->id;
                    r->bp[i].enabled = b->enabled;
                    r->bp[i].type = (uint8_t)b->type;
                    r->bp[i].zone = (uint8_t)b->zone;
                    r->bp[i].bank_id = b->bank_id;
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
             * si pole alokuje sám.
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
                if (p->update_mask & DBGAPI_CPU_FLAGS_UM_R)
                {
                    /* R registr ma vrchni bit (bit 7) nemenne reservovany
                     * po RETI/N (zachovava ho i LD A,R). Zapisujeme jen
                     * dolnich 7 bitu + zachovavame bit 7 z cpu->r,
                     * shodne s logikou v DBGAPI_CMD_SET_REG pro Z80_REG_IR. */
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
                if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
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
                } else {
                /* MZ-700: PIO-Z80 nedostupné, IM 2 vector by se musel řešit
                 * jiným zdrojem dat (RST 38h v IM 1 / nepoužívá se). */
                out->available = 0;
                }
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
             * Implementace: jednoduchý copy-paste z původních samostatných
             * handlerů aby zůstaly identické sémantiky (per-arch HAVE_PIOZ80
             * gating, banking-aware mem read, history empty detekce).
             */
            if (rq->result_ptr)
            {
                st_DBGAPI_CPU_PANEL_BATCH *out =
                    (st_DBGAPI_CPU_PANEL_BATCH *)rq->result_ptr;
                uint32_t which = 0;
                if (rq->data_ptr)
                    which = *((uint32_t *)rq->data_ptr);

                /* Hlavní část - vyplnit core fieldy a vynulovat optional
                 * valid flagy pro případ, že caller nepředal which-mask. */
                memset(out, 0, sizeof(*out));
                out->which = which;

                z80_t *cpu = g_mzarch_main.cpu;

                /* === Regs (vždy) === */
                for (int i = 0; i < DBGAPI_REG_COUNT; i++)
                {
                    out->regs[i] = z80_get_reg ( cpu, (z80_reg_t)i );
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
                out->frame_number      = (uint32_t)g_gdg.total_elapsed.screens;
                out->user_cycle_origin = g_debugger.user_cycle_origin;

                /* === V3.3 core: PIO-Z80 interrupt vectors + ISR targets.
                 * Vždy plněné při HAVE_PIOZ80 (MZ-800, MZ-1500), aby UI
                 * sekce VECA/ISRA + VECB/ISRB dostala data v každém ticku.
                 * Pro MZ-700 (HAVE_PIOZ80 == 0) má_pioz80 = 0; UI sekce
                 * se na MZ-700 nezobrazuje. */
                if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
                {
                    out->has_pioz80    = 1;
                    uint8_t va = g_pioz80.port[PIOZ80_PORT_A].interrupt_vector;
                    uint8_t vb = g_pioz80.port[PIOZ80_PORT_B].interrupt_vector;
                    out->pio_int_vec_a = va;
                    out->pio_int_vec_b = vb;
                    uint16_t vec_a = (uint16_t)(((uint16_t)cpu->i << 8) | (uint8_t)(va & 0xFE));
                    uint16_t vec_b = (uint16_t)(((uint16_t)cpu->i << 8) | (uint8_t)(vb & 0xFE));
                    out->veca = vec_a;
                    out->vecb = vec_b;
                    {
                        uint8_t lo_a = debugger_memory_read_byte(vec_a);
                        uint8_t hi_a = debugger_memory_read_byte((uint16_t)(vec_a + 1));
                        out->isra = (uint16_t)(lo_a | (hi_a << 8));
                    };
                    {
                        uint8_t lo_b = debugger_memory_read_byte(vec_b);
                        uint8_t hi_b = debugger_memory_read_byte((uint16_t)(vec_b + 1));
                        out->isrb = (uint16_t)(lo_b | (hi_b << 8));
                    };
                };
                } else {
                out->has_pioz80 = 0;
                }

                /* === IM2 ISR vector (volitelne) === */
                if (which & DBGAPI_CPU_PANEL_WANT_IM2)
                {
                    st_DBGAPI_IM2_VECTOR *im2 = &out->im2;
                if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
                    im2->available  = 1;
                    im2->im         = cpu->im;
                    im2->i_register = cpu->i;
                    im2->pio_irq_pending = 0;
                    im2->pio_source      = 0;
                    im2->vector_byte     = 0;
                    if (g_pioz80.interrupt == PIOZ80_INTERRUPT_PENDING)
                    {
                        for (int pid = PIOZ80_PORT_A; pid < PIOZ80_PORT_COUNT; pid++)
                        {
                            st_PIOZ80_PORT *port = &g_pioz80.port[pid];
                            if (port->port_int == PIOZ80_PORT_INT_PENDING
                                && port->icena == PIOZ80_ICENA_ENABLED)
                            {
                                im2->pio_irq_pending = 1;
                                im2->pio_source      = (uint8_t)pid;
                                im2->vector_byte     = port->interrupt_vector;
                                break;
                            };
                        };
                    };
                    im2->isr_table_addr =
                        (uint16_t)(((uint16_t)im2->i_register << 8) | im2->vector_byte);
                    {
                        uint8_t lo = debugger_memory_read_byte(im2->isr_table_addr);
                        uint8_t hi = debugger_memory_read_byte((uint16_t)(im2->isr_table_addr + 1));
                        im2->isr_target_addr = (uint16_t)(lo | (hi << 8));
                    };
                } else {
                    im2->available = 0;
                }
                    out->im2_valid = 1;
                };

                /* === Raster pos + cycles (volitelne) === */
                if (which & DBGAPI_CPU_PANEL_WANT_RASTER)
                {
                    st_DBGAPI_RASTER_POS *r = &out->raster;
                    r->frame_number = (uint32_t)g_gdg.total_elapsed.screens;
                    r->scanline     = (uint16_t)g_gdg.beam_row;
                    r->column_pixel = (uint16_t)VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks);
                    r->total_cycles = cpu->total_cycles;
                    r->frame_cycles = cpu->cycles;
                    out->raster_valid = 1;
                };

                /* === Last instruction (volitelne) === */
                if (which & DBGAPI_CPU_PANEL_WANT_LAST_INSTR)
                {
                    st_DBGAPI_LAST_INSTR *li = &out->last_instr;
                    unsigned pos = debugger_history_position(g_debugger_history.position);
                    st_DEBUGGER_HISTORY_ROW *row = &g_debugger_history.row[pos];
                    bool empty = (g_debugger_history.position == 0
                                  && row->addr == 0
                                  && row->byte[0] == 0
                                  && row->byte[1] == 0
                                  && row->byte[2] == 0
                                  && row->byte[3] == 0);
                    if (empty)
                    {
                        li->valid = 0;
                    }
                    else
                    {
                        li->valid = 1;
                        li->addr  = row->addr;
                        for (int b = 0; b < 4; b++)
                            li->bytes[b] = row->byte[b];
                        z80_dasm_inst_t inst;
                        int len = z80_dasm ( &inst,
                                             dbgapi_last_instr_read_cb,
                                             row,
                                             row->addr );
                        if (len < 1) len = 1;
                        if (len > 4) len = 4;
                        li->length = (uint8_t)len;
                    };
                    out->last_instr_valid = 1;
                };

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
                if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
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
                } else {
            rq->success = false;
                }
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
                    /* Žádný bajt nezapsán - all-or-nothing semantika. */
                    rq->success = true; /* command sám prošel, jen write zamítnut */
                    break;
                };

                /* Fáze 2: vlastní zápis (region check prošel). */
                for (uint32_t i = 0; i < p->length; i++)
                {
                    debugger_memory_write_byte(
                        (uint16_t)(p->addr + i), p->data[i]);
                };
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
            bptmap_set_temporary_event ( target );
            debugger_step_call ( 0 );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            mzarch_run_to_temporary_breakpoint ( );
#endif
            (void) p->max_cycles; /* informativní; ne-enforced v V1.A.3 */
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
            p->value = port_read_cb ( g_mzarch_main.cpu, p->port, NULL );
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
            port_write_cb ( g_mzarch_main.cpu, p->port, p->value, NULL );
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
            for ( uint32_t i = 0; i < p->len; i++ )
            {
                debugger_memory_write_byte (
                    (uint16_t) ( p->addr + i ), p->buf[ i ] );
            };
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
            /* SAVE bere path z param; ostatní op path ignorují. */
            const char *path = ( cmd == DBGAPI_CMD_TRACE_SAVE ) ? p->path : NULL;
            int rc = dbgapi_trace_lifecycle ( p->channel, op, path );
            p->out_result = rc;
            rq->success = ( rc == 0 );
            break;
        }

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
             *   - NEnastaví PC (= práce composite emu_media_run_mzf / caller).
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
                case DBGAPI_MEDIA_SLOT_FDC0_FD0:
                case DBGAPI_MEDIA_SLOT_FDC0_FD1:
                case DBGAPI_MEDIA_SLOT_FDC0_FD2:
                case DBGAPI_MEDIA_SLOT_FDC0_FD3:
                case DBGAPI_MEDIA_SLOT_FDC1_FD0:
                case DBGAPI_MEDIA_SLOT_FDC1_FD1:
                case DBGAPI_MEDIA_SLOT_FDC1_FD2:
                case DBGAPI_MEDIA_SLOT_FDC1_FD3:
                {
                    /* Runtime capability (mzhal 11h): bez FDC vraci -10
                     * (= driv #else vetev). */
                    if ( !g_mzhal.have_fdc ) { rc = -10; break; }
                    st_FDC *fdc = NULL;
                    unsigned drive = 0;
                    dbgapi_media_slot_to_fdc ( p->slot, &fdc, &drive );
                    fdc_mount_dskfile ( fdc, drive, (char *) p->filepath );
                    rc = FDC_TEST_DRIVE_ID_MOUNTED ( fdc, drive ) ? 0 : -3;
                    break;
                };
                case DBGAPI_MEDIA_SLOT_QD:
                {
                    if ( !g_mzhal.have_qdisk ) { rc = -10; break; }
                    /* qdisk_open čte cestu z CFGELM (g_elm_qd_path) -
                     * V1.B.1 podporujeme jen runtime ekvivalent insert
                     * skrz cfgelement API. Pro V1.B.1 vrátíme -11
                     * (= "není implementováno", path setting jde přes
                     * V1.B.2 settings_set). */
                    (void) p;
                    rc = -11;
                    break;
                };
                case DBGAPI_MEDIA_SLOT_IDE8:
                {
                    if ( !g_mzhal.have_ide8 ) { rc = -10; break; }
                    rc = ide8_drive_open_image ( &g_ide8.drive[ 0 ],
                                                  (char *) p->filepath );
                    /* ide8 vrací 0 = OK. */
                    break;
                };
                default:
                    rc = -1;
                    break;
            };
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
            switch ( p->slot )
            {
                case DBGAPI_MEDIA_SLOT_CMT:
                    cmt_eject ( );
                    rc = 0;
                    break;
                case DBGAPI_MEDIA_SLOT_FDC0_FD0:
                case DBGAPI_MEDIA_SLOT_FDC0_FD1:
                case DBGAPI_MEDIA_SLOT_FDC0_FD2:
                case DBGAPI_MEDIA_SLOT_FDC0_FD3:
                case DBGAPI_MEDIA_SLOT_FDC1_FD0:
                case DBGAPI_MEDIA_SLOT_FDC1_FD1:
                case DBGAPI_MEDIA_SLOT_FDC1_FD2:
                case DBGAPI_MEDIA_SLOT_FDC1_FD3:
                {
                    if ( !g_mzhal.have_fdc ) { rc = -10; break; }
                    st_FDC *fdc = NULL;
                    unsigned drive = 0;
                    dbgapi_media_slot_to_fdc ( p->slot, &fdc, &drive );
                    fdc_umount ( fdc, drive );
                    rc = 0;
                    break;
                };
                case DBGAPI_MEDIA_SLOT_QD:
                    if ( !g_mzhal.have_qdisk ) { rc = -10; break; }
                    qdisk_umount ( );
                    rc = 0;
                    break;
                case DBGAPI_MEDIA_SLOT_IDE8:
                    if ( !g_mzhal.have_ide8 ) { rc = -10; break; }
                    ide8_drive_close_image ( &g_ide8.drive[ 0 ] );
                    rc = 0;
                    break;
                default:
                    rc = -1;
                    break;
            };
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
                if (g_mzhal.have_fdc) { /* runtime capability, mzhal krok 8 */
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
                }
                        idx++;
                    };
                };
            };

                if (g_mzhal.have_qdisk) { /* runtime capability, mzhal krok 8 */
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_QD;
            p->slots[ idx ].inserted = ( g_qdisk.filename[ 0 ] != '\0' ) ? 1 : 0;
            p->slots[ idx ].read_only = qdisc_get_write_protected ( ) ? 1 : 0;
            if ( g_qdisk.filename[ 0 ] )
            {
                strncpy ( p->slots[ idx ].filepath, g_qdisk.filename,
                          sizeof ( p->slots[ idx ].filepath ) - 1 );
            };
            idx++;
                } else {
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_QD;
            idx++;
                }

                if (g_mzhal.have_ide8) { /* runtime capability, mzhal krok 8 */
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
                } else {
            p->slots[ idx ].slot = DBGAPI_MEDIA_SLOT_IDE8;
            idx++;
                }

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
             * out_type. Caller (MCP dispatch) uvolňuje out_value
             * přes g_free. */
            st_DBGAPI_SETTINGS_PARAM *p =
                (st_DBGAPI_SETTINGS_PARAM *) rq->data_ptr;
            if ( !p || !p->module || !p->element )
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
            };
            break;
        }

        case DBGAPI_CMD_SETTINGS_SET:
        {
            /* Zápis INI elementu. Před zápisem zachytí aktuální hodnotu
             * do out_value (audit / rollback). Pokud type-coerce
             * stringu selže, vrátí -4. Whitelist live-settable klíčů
             * NENÍ řešen zde (= delegováno na MCP vrstvu, která má
             * jednotný seznam). */
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
            /* Attach/detach periferie. V1.B.2 minimal implementation -
             * zapíše cfgmain INI flag a vrátí out_requires_restart=1
             * (= aplikace plně až po restartu emulátoru). Hot-swap
             * (live re-init) je out of scope V1.B.2.
             *
             * INI mapování:
             *   memext -> CFGMOD "MEMEXT", element "type" (TEXT)
             *           pro attach + "active" (BOOL) flag.
             *   fdc    -> CFGMOD "FDC",    element "active" (BOOL)
             *   qd     -> CFGMOD "QDISK",  element "active" (BOOL)
             *   ide8   -> CFGMOD "IDE8",   element "active" (BOOL)
             *   gal5   -> CFGMOD "GAL5",   element "active" (BOOL)
             *
             * Pokud cfgmodule nebo element neexistuje (= periferie
             * není v této arch sestavě nebo zatím nepodporuje INI
             * active flag), handler vrátí -10 / -11. */
            st_DBGAPI_PERIPH_PARAM *p =
                (st_DBGAPI_PERIPH_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            p->out_requires_restart = 0;
            const char *mod_name = NULL;
            switch ( p->kind )
            {
                case DBGAPI_PERIPH_KIND_MEMEXT: mod_name = "MEMEXT"; break;
                case DBGAPI_PERIPH_KIND_FDC:    mod_name = "FDC";    break;
                case DBGAPI_PERIPH_KIND_QD:     mod_name = "QDISK";  break;
                case DBGAPI_PERIPH_KIND_IDE8:   mod_name = "IDE8";   break;
                case DBGAPI_PERIPH_KIND_GAL5:   mod_name = "GAL5";   break;
                default:
                    p->out_result = -1;
                    rq->success = false;
                    break;
            };
            if ( !mod_name )
            {
                /* default: case už nastavil out_result. */
                break;
            };
            CFGMOD *mod =
                cfgroot_get_module_by_name ( g_cfgmain, (char *) mod_name );
            if ( !mod )
            {
                /* Periferie není v této arch sestavě (= modul se
                 * vůbec neregistroval). */
                p->out_result = -10;
                rq->success = false;
                break;
            };
            /* "active" BOOL flag - musí existovat v modulu. Pokud
             * konkrétní modul tento flag nemá, periferie nepodporuje
             * runtime attach/detach. */
            st_CFGELEMENT *active_elm =
                cfgmodule_get_element_by_name ( mod, "active" );
            if ( !active_elm || active_elm->type != CFGENTYPE_BOOL )
            {
                p->out_result = -11;
                rq->success = false;
                break;
            };
            int new_state = ( cmd == DBGAPI_CMD_PERIPH_ATTACH ) ? 1 : 0;
            cfgelement_set_bool_value ( active_elm, new_state );
            /* Pro memext attach option_value (= type variant) zapíšeme
             * do "type" TEXT elementu pokud je předán. */
            if ( cmd == DBGAPI_CMD_PERIPH_ATTACH
                 && p->kind == DBGAPI_PERIPH_KIND_MEMEXT
                 && p->option_value && p->option_value[ 0 ] )
            {
                st_CFGELEMENT *type_elm =
                    cfgmodule_get_element_by_name ( mod, "type" );
                if ( type_elm && type_elm->type == CFGENTYPE_TEXT )
                {
                    cfgelement_set_text_value ( type_elm, p->option_value );
                };
            };
            p->out_requires_restart = 1;
            p->out_result = 0;
            rq->success = true;
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
                "16 slotov 4KB; banking + memext aware (V1.D.1 minimal).",
                sizeof ( p->mode_note ) );

            for ( int i = 0; i < 16; i++ )
            {
                p->slots[ i ].addr_start  = (uint16_t) ( i * 0x1000 );
                p->slots[ i ].addr_end    =
                    (uint16_t) ( i * 0x1000 + 0x0FFF );
                p->slots[ i ].source      = 0; /* unknown */
                p->slots[ i ].ro_rw       = 0;
                p->slots[ i ].slot_offset = 0;

                /* MEMEXT detekce - vyžaduje connected memext, jinak skip. */
                if ( MEMEXT_TEST_CONNECTED )
                {
                    /* g_memext.map[i] = raw bank index. Pro Luftner:
                     * 0..0x7F = RAM, 0x80..0xFF = FLASH. Pro PEHU jen
                     * 0..0x3F (RAM only). */
                    uint32_t rb = g_memext.map[ i ];
                    if ( MEMEXT_TEST_TYPE_LUFTNER )
                    {
                        if ( rb < MEMEXT_LUFTNER_BANKS )
                        {
                            p->slots[ i ].source = 5; /* MEMEXT_RAM */
                            p->slots[ i ].ro_rw  = 1;
                            p->slots[ i ].slot_offset =
                                rb * MEMEXT_RAW_BANK_SIZE;
                        }
                        else
                        {
                            p->slots[ i ].source = 6; /* MEMEXT_FLASH */
                            p->slots[ i ].ro_rw  = 0;
                            p->slots[ i ].slot_offset =
                                ( rb - MEMEXT_LUFTNER_BANKS )
                                * MEMEXT_RAW_BANK_SIZE;
                        };
                    }
                    else if ( MEMEXT_TEST_TYPE_PEHU )
                    {
                        if ( rb < MEMEXT_PEHU_BANKS * 2 )
                        {
                            p->slots[ i ].source = 5; /* MEMEXT_RAM */
                            p->slots[ i ].ro_rw  = 1;
                            p->slots[ i ].slot_offset =
                                rb * MEMEXT_RAW_BANK_SIZE;
                        };
                    };
                };
            };
            p->slot_count = 16;
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
                if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
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
                } else {
            p->available         = 0;
            p->interrupt_port_id = 0xFF;
                }
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
                if (g_mzhal.psg_count >= 1) { /* runtime capability, mzhal krok 8 */
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
                } else {
            /* HAVE_PSG == 0 (= MZ-700): chip není přítomen. */
            p->available = 0;
            p->psg_count = 0;
                }
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

            /* Runtime dle g_mzhal.arch (mzhal 11g). */
            g_strlcpy ( (char *) p->platform, g_mzhal.arch_name,
                        sizeof ( p->platform ) );
            if ( g_mzhal.arch == 800 ) {
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
            } else if ( g_mzhal.arch == 1500 ) {

            /* MZ-1500: 8-entry palette `mode_color[8]`, žádný regBOR
             * ani regPALGRP, žádný cksw. */
            p->palette_count  = 8;
            p->has_border_reg = 0;
            p->has_pal_group  = 0;
            p->has_cksw       = 0;
            for ( unsigned i = 0; i < 8; i++ )
            {
                p->palette[ i ] = (uint8_t)( g_gdg.mode_color[ i ] & 0xFFu );
            };
            } else {

            /* MZ-700: 8-entry palette `mode_color[8]`, žádný regBOR
             * ani regPALGRP, žádný cksw. */
            p->palette_count  = 8;
            p->has_border_reg = 0;
            p->has_pal_group  = 0;
            p->has_cksw       = 0;
            for ( unsigned i = 0; i < 8; i++ )
            {
                p->palette[ i ] = (uint8_t)( g_gdg.mode_color[ i ] & 0xFFu );
            };
            }
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
                if (g_mzhal.have_fdc) { /* runtime capability, mzhal krok 8 */
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
                } else {
            p->available = 0;
                }
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
                if (g_mzhal.have_qdisk) { /* runtime capability, mzhal krok 8 */
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
                } else {
            p->available = 0;
                }
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
            g_strlcpy ( (char *) p->platform, g_mzhal.arch_name,
                        sizeof ( p->platform ) );
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
             * Joystick subsystem je součástí všech tří platforem,
             * runtime config rozhodne type.
             */
            st_DBGAPI_INPUT_JOY_STATE_PARAM *p =
                (st_DBGAPI_INPUT_JOY_STATE_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );
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
            /* PNG screenshot - enkód plného framebufferu přes
             * stb_image_write.h (viz png_encode.{c,h}).
             *
             * Pixel acquisition je IDENTICKÁ jako GET_FRAME_SCREENSHOT_RAW
             * (= konzistence obrazu): INDEX8 buffer
             * g_iface_video->fbsnapshot_pixels (GUI cesta, pod
             * fbsnapshot_pixels_mutex), fallback na g_framebuffer.pixels
             * (GDG live, headless). Expand na RGBA8888 přes paletu
             * display_get_default_color_schema(), index = pixel & 0x0F.
             *
             * Na rozdíl od raw nepoužíváme downscale - PNG je vždy plný
             * frame (VIDEO_DISPLAY_WIDTH x VIDEO_DISPLAY_HEIGHT). Threading
             * shodný s raw handlerem (krátký lock SDL path, GDG fallback
             * bez locku v emu thread executoru).
             *
             * Buffer ownership: handler alokuje PNG stream (p->buffer)
             * přes glib, dispatch ho po base64 uvolní g_free.
             */
            st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM *p =
                (st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM *) rq->data_ptr;
            if ( !p )
            {
                rq->success = false;
                break;
            };
            memset ( p, 0, sizeof ( *p ) );

            uint32_t w = (uint32_t) VIDEO_DISPLAY_WIDTH;
            uint32_t h = (uint32_t) VIDEO_DISPLAY_HEIGHT;
            uint32_t *g_video_colormap = display_get_default_color_schema();
            if ( !g_video_colormap )
            {
                /* Display modul ještě neinicializován. */
                p->available = 0;
                strcpy ( p->reason, "Display not initialized" );
                rq->success = true;
                break;
            };

            /* Výběr zdroje pixelů - viz raw handler pro threading detaily. */
            const uint8_t *src = NULL;
            bool sdl_locked = false;
            APP_MUTEX_LOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            sdl_locked = true;
            if ( g_iface_video->fbsnapshot_pixels != NULL )
            {
                src = g_iface_video->fbsnapshot_pixels;
            }
            else
            {
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
                sdl_locked = false;
                src = g_framebuffer.pixels;
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

            /* RGBA8888 scratch buffer - expand INDEX8 -> RGBA. */
            uint8_t *rgba = (uint8_t *) g_malloc ( (gsize) w * h * 4 );
            uint8_t *dst = rgba;
            for ( uint32_t y = 0; y < h; y++ )
            {
                const uint8_t *src_row = src + (size_t) y * w;
                for ( uint32_t x = 0; x < w; x++ )
                {
                    uint8_t pix_idx = src_row[ x ] & 0x0Fu;
                    uint32_t rgb = g_video_colormap[ pix_idx ];
                    dst[ 0 ] = (uint8_t) ( ( rgb >> 16 ) & 0xFFu ); /* R */
                    dst[ 1 ] = (uint8_t) ( ( rgb >> 8 ) & 0xFFu );  /* G */
                    dst[ 2 ] = (uint8_t) ( rgb & 0xFFu );           /* B */
                    dst[ 3 ] = 0xFF;                                /* A */
                    dst += 4;
                };
            };
            if ( sdl_locked )
            {
                APP_MUTEX_UNLOCK ( g_iface_video->fbsnapshot_pixels_mutex );
            };

            /* Enkód do PNG streamu (glib alokovaný buffer). */
            size_t png_len = 0;
            uint8_t *png = png_encode_rgba ( rgba, w, h, &png_len );
            g_free ( rgba );
            if ( !png || png_len == 0 )
            {
                g_free ( png );
                p->available = 0;
                strcpy ( p->reason, "PNG encode failed" );
                rq->success = true;
                break;
            };

            p->buffer      = png;
            p->buffer_size = png_len;
            p->width       = w;
            p->height      = h;
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
            g_strlcpy ( (char *) p->platform, g_mzhal.arch_name,
                        sizeof ( p->platform ) );
            /* MZ-800: DMD bit 3 = 0 znamena native 800 graficky rezim -
             * textovy reader neni k dispozici. */
            if ( ( g_mzhal.arch == 800 ) && ( ( g_gdg.regDMD & 0x08 ) == 0 ) )
            {
                p->available = 0;
                strcpy ( p->reason, "MZ-800 in 800 graphics mode (not text)" );
                rq->success = true;
                break;
            };
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
            if ( !p || !p->out || p->max_count <= 0 )
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
             */
            st_DBGAPI_REGIONS_READ_PARAM *p =
                (st_DBGAPI_REGIONS_READ_PARAM *) rq->data_ptr;
            if ( !p || !p->buf || p->len == 0 )
            {
                rq->success = false;
                break;
            };
            p->out_count = dbgapi_regions_read (
                p->region_id, p->offset, p->buf, p->len );
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
             * UI zodpovědnost při edit v pause modu.
             */
            st_DBGAPI_REGIONS_WRITE_PARAM *p =
                (st_DBGAPI_REGIONS_WRITE_PARAM *) rq->data_ptr;
            if ( !p || !p->data || p->len == 0 )
            {
                rq->success = false;
                break;
            };
            p->out_count = dbgapi_regions_write (
                p->region_id, p->offset, p->data, p->len );
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
             * Proto out_result = 0 a success = true i pro no-op. */
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
                    if ( !cmtspeed_is_valid ( (en_CMTSPEED) p->value ) )
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
             * openu -> out_result = -2, success = false. */
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
            if ( ( !CMT_TEST_FILLED ) || ( !g_cmt.ext ) )
            {
                p->out_result = -1;
                rq->success = false;
                break;
            };
            if ( !cmtspeed_is_valid ( (en_CMTSPEED) p->cmtspeed ) )
            {
                p->out_result = -2;
                rq->success = false;
                break;
            };
            st_CMTEXT_CONTAINER *container = cmtext_get_container ( g_cmt.ext );
            if ( !container )
            {
                p->out_result = -1;
                rq->success = false;
                break;
            };
            /* Per-blok speed má smysl jen pro SIMPLE_TAPE (= má tape
             * index). SINGLE container nemá per-blok data; set funkce by
             * dereferencovala NULL container->tape. Také kontrolujeme
             * rozsah block_id (= set funkce má jen assert). */
            if ( ( cmtext_container_get_type ( container )
                   != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE )
                 || ( p->block_id < 0 )
                 || ( p->block_id >= cmtext_container_get_count_blocks ( container ) ) )
            {
                p->out_result = -1;
                rq->success = false;
                break;
            };
            cmtext_container_set_block_cmt_speed ( container, p->block_id,
                                                   (en_CMTSPEED) p->cmtspeed );
            p->out_result = 0;
            rq->success = true;
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
            int playable   = cmtext_is_playable ( g_cmt.ext );
            int recordable = cmtext_is_recordable ( g_cmt.ext );

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

    /* MCP_ACTION broadcast hook - po úspěšném zpracování CMDRQ od MCP
     * klienta emitujeme notifikaci do UI (= budoucí Activity Log panel
     * V1.C). Pro chybové průchody (success == false) broadcast neposíláme,
     * aby Activity Log nezaplevelily failed pokusy. */
    if (rq->success && rq->cmd_origin == DBGAPI_CMD_ORIGIN_MCP)
    {
        dbgapi_emit_mcp_action(rq);
    };
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

bool dbgapi_ui_submit_cmd_sync_with_origin(st_DBGAPI_CMDRQ_QUEUE *queue,
                                            en_DBGAPI_CMD cmd,
                                            en_DBGAPI_CMD_ORIGIN origin,
                                            void *data_ptr,
                                            void *result_ptr,
                                            int timeout_ms)
{
    /* Kontrola: emulátor se neukončuje? */
    APP_MUTEX_LOCK(queue->queue_mutex);
    if (queue->reply_state == DBGAPI_CMDREPLY_STATE_ENDING)
    {
        APP_MUTEX_UNLOCK(queue->queue_mutex);
        return false;
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
        return false;
    };

    /* Vložit příkaz do slotu na pozici tail */
    st_DBGAPI_CMDRQ *slot = &queue->cmdrq[queue->tail];
    queue->tail = next_tail;

    /* Inicializace slotu */
    APP_MUTEX_LOCK(slot->mutex);
    slot->cmd = cmd;
    slot->cmd_origin = origin;
    slot->cmd_state = DBGAPI_CMDSTATE_PENDING;
    slot->data_ptr = data_ptr;
    slot->result_ptr = result_ptr;
    slot->success = false;

    /* V1.D.1 - track last user action pro emulator://state Resource.
     * Zaznamenáváme jen origin == USER (= GUI klik / hotkey / menu).
     * MCP / TEST / INTERNAL origin ignorujeme - AI klient nás zajímá
     * jen co dělá human user. */
    if ( origin == DBGAPI_CMD_ORIGIN_USER )
    {
        dbgapi_track_last_user_action ( cmd );
    };

    /* Signalizovat emulátoru, že ve frontě je nový příkaz */
    APP_COND_SIGNAL(queue->queue_cond);
    APP_MUTEX_UNLOCK(queue->queue_mutex);

    /* Čekat na zpracování příkazu emulátorem */
    if (timeout_ms > 0)
    {
        /* Čekání s timeoutem */
        APP_COND_WAIT_TIMEOUT_MS(slot->cond, slot->mutex, timeout_ms);
    }
    else
    {
        /* Neomezené čekání */
        APP_COND_WAIT(slot->cond, slot->mutex);
    };

    /* Přečíst výsledek */
    bool success = (slot->cmd_state == DBGAPI_CMDSTATE_PROCESSED) && slot->success;

    /* Uvolnit slot */
    slot->cmd_state = DBGAPI_CMDSTATE_NONE;
    slot->cmd = DBGAPI_CMD_NONE;
    slot->cmd_origin = DBGAPI_CMD_ORIGIN_USER;
    slot->data_ptr = NULL;
    slot->result_ptr = NULL;

    APP_MUTEX_UNLOCK(slot->mutex);

    return success;
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
