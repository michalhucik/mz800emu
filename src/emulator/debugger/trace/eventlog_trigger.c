/**
 * @file   eventlog_trigger.c
 * @brief  Implementace triggerů Event Vieweru (Pause / Auto-mark on match).
 *
 * Viz @ref eventlog_trigger.h pro kontrakty a model vlastnictví: veškerý
 * stav čtený callbacky mění jen emu vlákno.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 *
 * Licence: GPLv3
 */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "eventlog_trigger.h"

#include <stdio.h>
#include <string.h>

#include "emulator/emulator.h"


/**
 * @brief Stav pause-on-match triggeru.
 *
 * @field filter                Aktivní filtr (vlastní modul) nebo @c NULL
 *                              = trigger vypnutý.
 * @field has_match             Shoda od posledního vymazání.
 * @field last_match_frame      @c screens_total poslední shody.
 * @field last_match_pxclk      @c pxclk_in_screen poslední shody.
 * @field last_match_event_idx  Logický index shody v ringu (-1 = žádný).
 *
 * Invariant: @c filter != NULL <=> @ref g_eventlog_pause_trigger_active
 * != 0 (mimo @ref eventlog_destroy(), který gate vynuluje sám). Zapisuje
 * jen emu vlákno.
 */
typedef struct st_EVTRIG_PAUSE {
    st_EVENTLOG_FILTER *filter;
    bool                has_match;
    uint32_t            last_match_frame;
    uint32_t            last_match_pxclk;
    int64_t             last_match_event_idx;
} st_EVTRIG_PAUSE;

/**
 * @brief Stav auto-mark on match triggeru.
 *
 * @field filter           Aktivní filtr (vlastní modul) nebo @c NULL
 *                         = trigger vypnutý.
 * @field name             Jméno markeru (kopie z posledního nastavení).
 * @field marker_id        ID z marklog_register() nebo MARKLOG_INVALID_ID.
 * @field marker_resolved  Registrace pro aktuální @c name už proběhla
 *                         (i neúspěšně - aby se při každé shodě nevolala
 *                         znovu a nespamovala stderr).
 * @field has_match        Shoda od posledního vymazání.
 * @field total_marks      Počet zapsaných markerů od posledního vymazání.
 *
 * Invariant: @c filter != NULL => @c name neprázdné; @c filter != NULL
 * <=> @ref g_eventlog_automark_trigger_active != 0 (mimo
 * @ref eventlog_destroy()). Zapisuje jen emu vlákno.
 */
typedef struct st_EVTRIG_AUTOMARK {
    st_EVENTLOG_FILTER *filter;
    char                name[ EVENTLOG_TRIGGER_NAME_MAX ];
    uint16_t            marker_id;
    bool                marker_resolved;
    bool                has_match;
    uint64_t            total_marks;
} st_EVTRIG_AUTOMARK;

/** @brief Stav pause triggeru (jen emu vlákno zapisuje). */
static st_EVTRIG_PAUSE s_pause = {
    .filter               = NULL,
    .last_match_event_idx = -1,
};

/** @brief Stav automark triggeru (jen emu vlákno zapisuje). */
static st_EVTRIG_AUTOMARK s_automark = {
    .filter    = NULL,
    .marker_id = MARKLOG_INVALID_ID,
};


/**
 * @brief Callback pause triggeru volaný z eventlog_record() (emu vlákno).
 *
 * Při shodě zaznamená pozici události a požádá o pauzu emulace
 * (emulator_pause(true) - nastaví příznak pauzy a událost pro hlavní
 * smyčku, stejná cesta jako breakpointy).
 *
 * @param e  Právě zapsaná událost v ringu (jen čtení).
 */
static void evtrig_pause_callback ( const st_EVENTLOG_EVENT *e )
{
    if ( !e || !s_pause.filter ) return;
    if ( !eventlog_filter_match ( s_pause.filter, e ) ) return;

    s_pause.has_match        = true;
    s_pause.last_match_frame = e->screens_total;
    s_pause.last_match_pxclk = e->pxclk_in_screen;
    /* eventlog_record už posunul head, nejnovější událost má logický
     * index count - 1. */
    s_pause.last_match_event_idx = ( g_eventlog.count > 0 )
                                   ? (int64_t) ( g_eventlog.count - 1 )
                                   : -1;
    emulator_pause ( true );
}


/**
 * @brief Callback automark triggeru volaný z eventlog_record() (emu vlákno).
 *
 * Při shodě zapíše marker (marklog_record), při první shodě pod aktuálním
 * jménem marker nejdřív zaregistruje. Události USER_MARK přeskakuje:
 * marklog_record() je sám zapisuje do ringu přes eventlog_record(), bez
 * přeskočení by vznikla nekonečná rekurze.
 *
 * @param e  Právě zapsaná událost v ringu (jen čtení).
 */
static void evtrig_automark_callback ( const st_EVENTLOG_EVENT *e )
{
    if ( !e || !s_automark.filter ) return;
    if ( e->category == EVENTLOG_CAT_USER_MARK ) return;
    if ( !eventlog_filter_match ( s_automark.filter, e ) ) return;

    if ( !s_automark.marker_resolved ) {
        s_automark.marker_id       = marklog_register ( s_automark.name );
        s_automark.marker_resolved = true;
    }
    if ( s_automark.marker_id == MARKLOG_INVALID_ID ) return;

    marklog_record ( s_automark.marker_id );
    s_automark.has_match = true;
    s_automark.total_marks++;
}


bool eventlog_trigger_filter_is_armable ( const st_EVENTLOG_FILTER *f )
{
    if ( !f ) return false;
    if ( eventlog_filter_get_error ( f ) != NULL ) return false;
    if ( eventlog_filter_has_temporal ( f ) ) return false;
    return true;
}


bool eventlog_trigger_set ( en_EVENTLOG_TRIGGER_KIND kind,
                            st_EVENTLOG_FILTER *filter,
                            const char *name,
                            st_EVENTLOG_FILTER **out_old )
{
    if ( filter && !eventlog_trigger_filter_is_armable ( filter ) ) return false;

    switch ( kind ) {
        case EVENTLOG_TRIGGER_PAUSE:
        {
            st_EVENTLOG_FILTER *old = s_pause.filter;
            if ( filter ) {
                /* Gate i callback mění jen emu vlákno, callback teď neběží
                 * (jsme na emu vlákně mimo eventlog_record), pořadí zápisů
                 * proto nehraje roli. */
                s_pause.filter = filter;
                g_eventlog_pause_callback = evtrig_pause_callback;
                g_eventlog_pause_trigger_active = 1;
            } else {
                g_eventlog_pause_trigger_active = 0;
                s_pause.filter = NULL;
            }
            if ( out_old ) *out_old = old;
            return true;
        }

        case EVENTLOG_TRIGGER_AUTOMARK:
        {
            if ( filter && ( !name || name[0] == '\0' ) ) return false;
            st_EVENTLOG_FILTER *old = s_automark.filter;
            if ( filter ) {
                if ( strncmp ( s_automark.name, name,
                               sizeof ( s_automark.name ) - 1 ) != 0 ) {
                    snprintf ( s_automark.name, sizeof ( s_automark.name ),
                               "%s", name );
                    /* Nové jméno = nový marker, zaregistruje se při první
                     * shodě (lazy, jako dřív v okně Events). */
                    s_automark.marker_id       = MARKLOG_INVALID_ID;
                    s_automark.marker_resolved = false;
                }
                s_automark.filter = filter;
                g_eventlog_automark_callback = evtrig_automark_callback;
                g_eventlog_automark_trigger_active = 1;
            } else {
                g_eventlog_automark_trigger_active = 0;
                s_automark.filter = NULL;
            }
            if ( out_old ) *out_old = old;
            return true;
        }

        default:
            return false;
    }
}


bool eventlog_trigger_clear_matches ( en_EVENTLOG_TRIGGER_KIND kind )
{
    switch ( kind ) {
        case EVENTLOG_TRIGGER_PAUSE:
            s_pause.has_match            = false;
            s_pause.last_match_event_idx = -1;
            return true;
        case EVENTLOG_TRIGGER_AUTOMARK:
            s_automark.has_match   = false;
            s_automark.total_marks = 0;
            return true;
        default:
            return false;
    }
}


void eventlog_trigger_get_status ( en_EVENTLOG_TRIGGER_KIND kind,
                                   st_EVENTLOG_TRIGGER_STATUS *out )
{
    if ( !out ) return;
    memset ( out, 0, sizeof ( *out ) );
    out->marker_id            = MARKLOG_INVALID_ID;
    out->last_match_event_idx = -1;

    switch ( kind ) {
        case EVENTLOG_TRIGGER_PAUSE:
            out->armed                = ( g_eventlog_pause_trigger_active != 0 );
            out->has_match            = s_pause.has_match;
            out->last_match_frame     = s_pause.last_match_frame;
            out->last_match_pxclk     = s_pause.last_match_pxclk;
            out->last_match_event_idx = s_pause.last_match_event_idx;
            break;
        case EVENTLOG_TRIGGER_AUTOMARK:
            out->armed       = ( g_eventlog_automark_trigger_active != 0 );
            out->has_match   = s_automark.has_match;
            out->marker_id   = s_automark.marker_id;
            out->total_marks = s_automark.total_marks;
            break;
        default:
            break;
    }
}


void eventlog_trigger_shutdown ( void )
{
    g_eventlog_pause_trigger_active    = 0;
    g_eventlog_automark_trigger_active = 0;

    eventlog_filter_free ( s_pause.filter );
    s_pause.filter               = NULL;
    s_pause.has_match            = false;
    s_pause.last_match_event_idx = -1;

    eventlog_filter_free ( s_automark.filter );
    s_automark.filter          = NULL;
    s_automark.name[0]         = '\0';
    s_automark.marker_id       = MARKLOG_INVALID_ID;
    s_automark.marker_resolved = false;
    s_automark.has_match       = false;
    s_automark.total_marks     = 0;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
