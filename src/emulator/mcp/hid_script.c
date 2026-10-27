/**
 * @file hid_script.c
 * @brief Sekvence vstupu MCP odměřená snímky emulace (emu vlákno).
 *
 * Implementace hid_script.h. Stav běžící sekvence drží statické proměnné
 * modulu; dispatch o průběhu ví jen přes OUT pole st_DBGAPI_HID_SCRIPT
 * a atomický příznak `done`.
 *
 * Časová osa jedné události (hold = H, gap = G, S = snímek stisku):
 *
 *     stisk @ S  ->  uvolnění @ S + H  ->  další stisk @ S + H + G
 *
 * Klávesa se SHIFT (key.needs_shift) a H >= 2: SHIFT se stiskne @ S,
 * hlavní klávesa až @ S + 1 (fáze HID_SCRIPT_PHASE_SHIFT), obě se uvolní
 * @ S + H. Délka události se nemění. Program, který čte sloupec SHIFT
 * v jiném okamžiku skenu než klávesu (K&P Disk BASIC), jinak při
 * současném stisku na hranici snímku občas dostal klávesu bez SHIFT.
 * Stejné pořadí používá autotype GUI (pio8255.c, vkbd_autotype_keydown).
 *
 * Všechny tři body leží v per-frame bodě hlavní smyčky (nebo v drainu
 * fronty dbgapi při START), tedy vždy na hranici snímku mezi instrukcemi.
 */

#include "../mzarch/mzarch_config.h"

#if defined(MZ800EMU_CFG_MCP_SERVER_ENABLED) && defined(MZ800EMU_CFG_DEBUGGER_ENABLED)

#include <glib.h>

#include "hid_script.h"
#include "hid_keymap.h"

#include "../emulator.h"
#include "../debugger/debugger.h"
#include "../hw-generic/pio8255/pio8255.h"

#if MZARCH == 800
#include "../mzarch/mz800/gdg/mz800_gdg.h"
#elif MZARCH == 1500
#include "../mzarch/mz1500/gdg/mz1500_gdg.h"
#elif MZARCH == 700
#include "../mzarch/mz700/gdg/mz700_gdg.h"
#endif


st_DBGAPI_HID_SCRIPT *g_hid_script_active = NULL;


/** @brief Fáze aktuální události běžící sekvence. */
typedef enum en_HID_SCRIPT_PHASE
{
    HID_SCRIPT_PHASE_HOLD = 0, /**< Vstup je stisknutý, čeká se na uvolnění. */
    HID_SCRIPT_PHASE_GAP,      /**< Vstup je uvolněný, čeká se na konec mezery. */
    HID_SCRIPT_PHASE_SHIFT,    /**< Jen SHIFT klávesy se SHIFT je stisknutý, hlavní klávesa za 1 snímek. */
} en_HID_SCRIPT_PHASE;

/** @brief Sloupec matice klávesy SHIFT (col 8). */
#define HID_SCRIPT_SHIFT_COL 8
/** @brief Bit klávesy SHIFT ve sloupci 8. */
#define HID_SCRIPT_SHIFT_BIT 0

/** @brief Index aktuální události v g_hid_script_active->events. */
static int s_index = 0;

/** @brief Fáze aktuální události. */
static en_HID_SCRIPT_PHASE s_phase = HID_SCRIPT_PHASE_HOLD;

/**
 * @brief Snímek (g_gdg.total_elapsed.screens), ve kterém skončí aktuální fáze.
 *
 * Porovnává se rozdílem se znaménkem, takže přetečení unsigned čítače
 * nevadí.
 */
static uint32_t s_next_screens = 0;


/** @brief Aktuální hodnota čítače snímků emulace. */
static inline uint32_t hid_script_now ( void ) {
    return (uint32_t) g_gdg.total_elapsed.screens;
}


/**
 * @brief Ověří, že událost jde provést.
 * @return true = platná klávesa (col 0..9, bit 0..7) nebo joystick
 *         (port 0..1, emulátor s joystickem) a nezáporné časy.
 */
static bool hid_script_event_valid ( const st_DBGAPI_HID_SCRIPT_EVENT *ev ) {
    if ( ev->hold_frames < 0 || ev->gap_frames < 0 ) return false;
    switch ( ev->type ) {
        case DBGAPI_HID_EVENT_KEY:
            return ( ev->key.col >= 0 && ev->key.col <= 9
                     && ev->key.bit >= 0 && ev->key.bit <= 7 );
        case DBGAPI_HID_EVENT_JOY:
#ifdef HAVE_JOY
            return ( ev->joy.port >= 0 && ev->joy.port <= 1 );
#else
            return false;
#endif
        default:
            return false;
    };
}


/**
 * @brief Stiskne vstup události; u klávesy s `probe` ozbrojí sondu dosednutí.
 *
 * Sonda sleduje jen hlavní klávesu (col/bit); případný SHIFT je pomocný.
 */
static void hid_script_press ( st_DBGAPI_HID_SCRIPT_EVENT *ev ) {
    if ( ev->type == DBGAPI_HID_EVENT_KEY ) {
        if ( ev->probe ) {
            pio8255_vkbd_probe_arm ( ev->key.col, ev->key.bit );
        };
        hid_keymap_press ( ev->key.col, ev->key.bit, ev->key.needs_shift );
    } else {
        (void) hid_keymap_joystick_set ( ev->joy.port, ev->joy.mcp_mask );
    };
}


/**
 * @brief Začne událost: nastaví fázi a cílový snímek prvního kroku.
 *
 * Klávesa se SHIFT s držením aspoň 2 snímky: stiskne jen SHIFT, fáze
 * HID_SCRIPT_PHASE_SHIFT na 1 snímek (hlavní klávesu stiskne
 * hid_script_advance()). Jinak stiskne celý vstup, fáze HOLD na
 * hold_frames snímků.
 *
 * @param[in] ev Událost, která právě začíná.
 * @post s_phase a s_next_screens odpovídají prvnímu kroku události.
 */
static void hid_script_begin ( st_DBGAPI_HID_SCRIPT_EVENT *ev ) {
    if ( ev->type == DBGAPI_HID_EVENT_KEY && ev->key.needs_shift && ev->hold_frames >= 2 ) {
        hid_keymap_press ( HID_SCRIPT_SHIFT_COL, HID_SCRIPT_SHIFT_BIT, false );
        s_phase = HID_SCRIPT_PHASE_SHIFT;
        s_next_screens = hid_script_now ( ) + 1u;
        return;
    };
    hid_script_press ( ev );
    s_phase = HID_SCRIPT_PHASE_HOLD;
    s_next_screens = hid_script_now ( ) + (uint32_t) ev->hold_frames;
}


/**
 * @brief Uvolní vstup události; u klávesy s `probe` zapíše `landed`
 *        a sondu odzbrojí.
 */
static void hid_script_release ( st_DBGAPI_HID_SCRIPT_EVENT *ev ) {
    if ( ev->type == DBGAPI_HID_EVENT_KEY ) {
        if ( ev->probe ) {
            ev->landed = pio8255_vkbd_probe_check ( );
            pio8255_vkbd_probe_disarm ( );
        };
        hid_keymap_release ( ev->key.col, ev->key.bit, ev->key.needs_shift );
    } else {
        (void) hid_keymap_joystick_clear ( ev->joy.port );
    };
}


/**
 * @brief Ukončí běžící sekvenci a předá ji zpět volajícímu.
 *
 * Zapíše OUT pole, při řádném dokončení s `pause_at_end` za běhu emulace
 * aktivuje frame-bounded stop na aktuálním snímku (emu se pauzne ve stejném
 * průchodu hlavní smyčkou s důvodem EMU_PAUSE_REASON_FRAMES) a jako
 * poslední krok atomicky nastaví `done` = 1. Po něm už modul na strukturu
 * nesahá. Pokud emulace stojí (sekvence doběhla už při START, ještě před
 * rozběhem), stop se neaktivuje - emu je v pauze, kde má být.
 *
 * @param[in] cancelled true = zrušení (bez pauzy na konci)
 */
static void hid_script_finish ( bool cancelled ) {
    st_DBGAPI_HID_SCRIPT *s = g_hid_script_active;
    g_hid_script_active = NULL;

    s->end_screens = hid_script_now ( );
    s->cancelled = cancelled;

    if ( !cancelled && s->pause_at_end && !EMULATOR_TEST_PAUSED ) {
        g_debugger.run_frames_target = s->end_screens;
        g_debugger.run_frames_active = 1;
    };

    g_atomic_int_set ( &s->done, 1 );
}


/**
 * @brief Provede všechny kroky, jejichž cílový snímek už nastal.
 *
 * Smyčka zpracuje i nulové držení a mezery v jednom volání (uvolnění,
 * resp. další stisk ve stejném bodě).
 */
static void hid_script_advance ( void ) {
    while ( g_hid_script_active
            && (int32_t) ( hid_script_now ( ) - s_next_screens ) >= 0 ) {
        st_DBGAPI_HID_SCRIPT *s = g_hid_script_active;
        st_DBGAPI_HID_SCRIPT_EVENT *ev = &s->events[s_index];

        if ( s_phase == HID_SCRIPT_PHASE_SHIFT ) {
            /* SHIFT drží 1 snímek, teď hlavní klávesa (zbytek držení). */
            hid_script_press ( ev );
            s_phase = HID_SCRIPT_PHASE_HOLD;
            s_next_screens = hid_script_now ( ) + (uint32_t) ( ev->hold_frames - 1 );
            continue;
        };

        if ( s_phase == HID_SCRIPT_PHASE_HOLD ) {
            hid_script_release ( ev );
            s_phase = HID_SCRIPT_PHASE_GAP;
            s_next_screens = hid_script_now ( ) + (uint32_t) ev->gap_frames;
            continue;
        };

        /* Konec mezery: událost je úplně provedená. */
        s->events_done = s_index + 1;
        s_index++;
        if ( s_index >= s->count ) {
            hid_script_finish ( false );
            return;
        };
        ev = &s->events[s_index];
        hid_script_begin ( ev );
    };
}


bool hid_script_start ( st_DBGAPI_HID_SCRIPT *script ) {
    if ( !script || !script->events || script->count < 1 ) return false;
    if ( g_hid_script_active ) return false;
    for ( int i = 0; i < script->count; i++ ) {
        if ( !hid_script_event_valid ( &script->events[i] ) ) return false;
    };

    for ( int i = 0; i < script->count; i++ ) {
        script->events[i].landed = false;
    };
    script->events_done = 0;
    script->cancelled = false;
    script->pause_at_end = ( EMULATOR_TEST_PAUSED ) ? true : false;
    script->start_screens = hid_script_now ( );
    script->end_screens = script->start_screens;
    g_atomic_int_set ( &script->done, 0 );

    g_hid_script_active = script;
    s_index = 0;
    hid_script_begin ( &script->events[0] );

    /* Nulová držení a mezery proběhnou hned. */
    hid_script_advance ( );

    if ( g_hid_script_active && script->pause_at_end ) {
        /* Sekvence potřebuje čas: rozběhnout emulaci, zastaví se sama na
         * konci (hid_script_finish -> frame-bounded stop). Důvod pauzy
         * vynulujeme, aby ho dispatch po doběhu mohl vyhodnotit. */
        g_emulator.pause_reason = EMU_PAUSE_REASON_NONE;
        emulator_pause ( false );
    };
    return true;
}


void hid_script_on_frame ( void ) {
    hid_script_advance ( );
}


bool hid_script_cancel ( st_DBGAPI_HID_SCRIPT *script ) {
    if ( !script ) return false;
    if ( g_hid_script_active != script ) {
        /* Skončila dřív (nebo nikdy neběžela) - emu vlákno ji nepoužívá. */
        return true;
    };
    if ( s_phase == HID_SCRIPT_PHASE_HOLD ) {
        hid_script_release ( &script->events[s_index] );
    } else if ( s_phase == HID_SCRIPT_PHASE_SHIFT ) {
        /* Hlavní klávesa ještě nebyla stisknutá (sonda neozbrojená):
         * uvolnit jen SHIFT, událost se nepočítá jako provedená. */
        hid_keymap_release ( HID_SCRIPT_SHIFT_COL, HID_SCRIPT_SHIFT_BIT, false );
    } else {
        /* Zrušeno v mezeře: vstup události byl stisknut i uvolněn. */
        script->events_done = s_index + 1;
    };
    hid_script_finish ( true );
    return true;
}


void hid_script_on_reset ( void ) {
    if ( g_hid_script_active ) {
        (void) hid_script_cancel ( g_hid_script_active );
    };
}

#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED && MZ800EMU_CFG_DEBUGGER_ENABLED */
