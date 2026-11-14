/**
 * @file cmt_cli.c
 * @brief CLI volby virtuální kazety: --cmt, --cmt-play, --cmt-play-pause, --cmt-boot.
 *
 * Popis voleb a průběhu viz cmt_cli.h. Výpisy pro uživatele jsou anglicky
 * (jádro emulátoru bez lokalizace).
 */

#include "main.h"

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>

#include "libs/sdlapp/sdlapp_options.h"
#include "mzarch/mzarch.h"
#include "hw-generic/pio8255/pio8255.h"
#include "cmt.h"
#include "cmtext.h"
#include "cmthack.h"
#include "cmt_cli.h"
#include "device_request.h"


/** Stav držení klávesy C volbou --cmt-boot (jen EMU vlákno). */
static struct {
    bool active;        /**< true = C je volbou --cmt-boot stisknutá */
    bool motor_was_off; /**< true = od stisku už byl motor vypnutý (PLAY + PAUSE) */
    int col;            /**< Sloupec klávesy C v matici klávesnice */
    uint8_t bit_mask;   /**< Bit klávesy C ve sloupci (1 << bit) */
    unsigned frames;    /**< Počet snímků od stisku */
} s_boot_hold;


/**
 * @brief Vypíše chybu CLI voleb kazety (anglicky, stderr) a vrátí false.
 */
static bool cmt_cli_fail ( const char *fmt, const char *arg ) {
    fprintf ( stderr, "Error: " );
    fprintf ( stderr, fmt, arg );
    fprintf ( stderr, "\n" );
    return false;
}


bool cmt_cli_validate ( void ) {
    bool has_cmt = sdlapp_option_present ( "--cmt" );
    bool play = sdlapp_option_present ( "--cmt-play" );
    bool play_pause = sdlapp_option_present ( "--cmt-play-pause" );
    bool boot = sdlapp_option_present ( "--cmt-boot" );

    if ( !has_cmt ) {
        if ( play ) return cmt_cli_fail ( "%s requires --cmt <file>", "--cmt-play" );
        if ( play_pause ) return cmt_cli_fail ( "%s requires --cmt <file>", "--cmt-play-pause" );
        if ( boot ) return cmt_cli_fail ( "%s requires --cmt <file>", "--cmt-boot" );
        return true;
    };

    if ( play && play_pause ) {
        return cmt_cli_fail ( "%s", "--cmt-play and --cmt-play-pause cannot be combined" );
    };
    if ( boot ) {
#if MZARCH != 800
        return cmt_cli_fail ( "%s", "--cmt-boot is only supported on MZ-800" );
#endif
        if ( play ) return cmt_cli_fail ( "%s", "--cmt-boot cannot be combined with --cmt-play" );
        if ( sdlapp_option_present ( "--run-mzf" ) ) {
            return cmt_cli_fail ( "%s", "--cmt-boot cannot be combined with --run-mzf" );
        };
    };

    gchar *path = mzarch_cli_option_value_utf8 ( "--cmt" );
    if ( !path || !g_file_test ( path, G_FILE_TEST_IS_REGULAR ) ) {
        cmt_cli_fail ( "--cmt: file not found: %s", path ? path : "" );
        g_free ( path );
        return false;
    };
    if ( !cmtext_is_playable_filename ( path ) ) {
        char exts[128];
        cmtext_get_playable_extensions_list ( exts, sizeof ( exts ) );
        fprintf ( stderr, "Error: --cmt: unsupported tape file extension: %s (supported: %s)\n", path, exts );
        g_free ( path );
        return false;
    };
    g_free ( path );
    return true;
}


/**
 * @brief Uvolní klávesu C drženou volbou --cmt-boot.
 */
static void cmt_cli_boot_release ( void ) {
    g_pio8255.vkbd_matrix[s_boot_hold.col] |= s_boot_hold.bit_mask;
    s_boot_hold.active = false;
}


/**
 * @brief Stiskne klávesu C ve virtuální matici klávesnice (--cmt-boot).
 */
static void cmt_cli_boot_press ( void ) {
    uint8_t row_mask = 0xff;
    bool needs_shift = false;
    int col = pio8255_autotype_get_matrix ( 'C', &row_mask, &needs_shift );
    if ( col < 0 || row_mask == 0xff ) {
        fprintf ( stderr, "Warning: --cmt-boot: key C not found in the keyboard matrix\n" );
        return;
    };
    /* row_mask je invertní maska: stisknutá klávesa = bit 0 */
    s_boot_hold.col = col;
    s_boot_hold.bit_mask = (uint8_t) ~row_mask;
    s_boot_hold.frames = 0;
    s_boot_hold.motor_was_off = false;
    g_pio8255.vkbd_matrix[col] &= row_mask;
    s_boot_hold.active = true;
}


/**
 * @brief Tělo cmt_cli_apply() (volající drží device_state_lock()).
 * @param path Cesta pásky z --cmt (UTF-8, vlastní ji volající).
 */
static void cmt_cli_apply_locked ( const gchar *path ) {

    if ( CMTHACK_TEST_IS_INSTALLED ) {
        cmthack_disable_for_session ( );
        g_print ( "CLI --cmt: CMT hack disabled for this session\n" );
    };

    if ( EXIT_SUCCESS != cmt_open_file_by_extension ( ( char * ) path ) ) {
        fprintf ( stderr, "CLI --cmt: cannot open tape file: %s\n", path );
        return;
    };
    g_print ( "CLI --cmt: %s\n", path );

    bool boot = sdlapp_option_present ( "--cmt-boot" );
    if ( sdlapp_option_present ( "--cmt-play" ) ) {
        cmt_play ( );
    } else if ( boot || sdlapp_option_present ( "--cmt-play-pause" ) ) {
        /* Bez pauzy uživatele je motor totéž co páska bez pauzy a každá
         * náběžná hrana PC3 ho přepne (pio8255.c, cmt_motor_toggle()). ROM všech
         * tří platforem při inicializaci zapíše do 0E003h 8Ah (mode set,
         * PC3 = 0) a hned 07h (BSR PC3 = 1, 0741h-0744h) - tahle hrana by
         * PLAY + PAUSE přepnula na běžící pásku ještě před LOAD. Když tedy
         * bootuje ROM (bez --run-mzf), vloží se PLAY a hrana ROM z něj
         * udělá PLAY + PAUSE. --run-mzf ROM přeskočí (bootstrap), tam se
         * PLAY + PAUSE nastaví rovnou. Při změně modelu motoru upravit. */
        if ( sdlapp_option_present ( "--run-mzf" ) ) {
            cmt_play_paused ( );
        } else {
            cmt_play ( );
        };
    };

    if ( boot ) {
        cmt_cli_boot_press ( );
    };
}


void cmt_cli_apply ( void ) {
    gchar *path = mzarch_cli_option_value_utf8 ( "--cmt" );
    if ( !path ) return;
    /* Vložení pásky uvolňuje g_cmt.last_filename a mění kontejner; UI vlákno
     * (menu, okno kazety) je čte pod zámkem stavu zařízení a v tu chvíli už
     * může kreslit. */
    device_state_lock ( );
    cmt_cli_apply_locked ( path );
    device_state_unlock ( );
    g_free ( path );
}


void cmt_cli_on_screen_done ( void ) {
    if ( !s_boot_hold.active ) return;

    /* Motor vypnutý = PLAY + PAUSE (po hraně PC3 z inicializace ROM, viz
     * cmt_cli_apply). Klávesa se pustí až při zapnutí motoru po něm. */
    if ( CMT_TEST_PLAY && CMT_TEST_PAUSED ) {
        s_boot_hold.motor_was_off = true;
    } else if ( s_boot_hold.motor_was_off && CMT_TEST_PLAY && !CMT_TEST_PAUSED ) {
        cmt_cli_boot_release ( );
        g_print ( "CLI --cmt-boot: tape motor started, key C released\n" );
        return;
    };
    if ( ++s_boot_hold.frames >= CMT_CLI_BOOT_HOLD_MAX_FRAMES ) {
        cmt_cli_boot_release ( );
        fprintf ( stderr, "Warning: --cmt-boot: the ROM did not start the tape motor within %u frames, key C released\n",
                  (unsigned) CMT_CLI_BOOT_HOLD_MAX_FRAMES );
    };
}


void cmt_cli_on_reset ( void ) {
    if ( s_boot_hold.active ) {
        cmt_cli_boot_release ( );
    };
}
