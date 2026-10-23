/**
 * @file fdc_cli.c
 * @brief CLI volby disketových mechanik: --fdd, --fdd0 .. --fdd7, --fdd-readonly.
 *
 * Popis voleb a průběhu viz fdc_cli.h. Výpisy pro uživatele jsou anglicky
 * (jádro emulátoru bez lokalizace).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "libs/sdlapp/sdlapp_options.h"
#include "mzarch/mzarch.h"
#include "automation.h"
#include "fdc.h"
#include "fdc_cli.h"

G_STATIC_ASSERT ( FDC_CLI_DRIVES == FDC_INSTANCE_COUNT * FDC_NUM_DRIVES );


/**
 * @brief Stav voleb (plní fdc_cli_validate() v hlavním vlákně před startem
 *        emu vlákna, potom se jen čte).
 *
 * @invariant path[i] != NULL jen pro mechaniku zadanou v CLI (UTF-8, g_malloc).
 */
static struct {
    gchar *path[FDC_CLI_DRIVES];      /**< Cesta k DSK obrazu, NULL = nezadáno. */
    bool readonly[FDC_CLI_DRIVES];    /**< --fdd-readonly pro mechaniku. */
    gchar *failure;                   /**< První chyba vložení z fdc_cli_note_failure(), NULL = žádná. */
} s_cli;


/**
 * @brief Vypíše chybu CLI voleb mechanik (anglicky, stderr) a vrátí false.
 */
static bool fdc_cli_fail ( const char *fmt, const char *arg ) {
    fprintf ( stderr, "Error: " );
    fprintf ( stderr, fmt, arg );
    fprintf ( stderr, "\n" );
    return false;
}


/**
 * @brief Číslo mechaniky 0-7 z textu (jen jedna desítková číslice).
 *
 * @param s Text (nesmí být NULL).
 * @param len Délka textu.
 * @return Číslo mechaniky, nebo -1 pro neplatný text.
 */
static int fdc_cli_parse_drive ( const char *s, size_t len ) {
    if ( len != 1 || s[0] < '0' || s[0] >= '0' + FDC_CLI_DRIVES ) return -1;
    return s[0] - '0';
}


/**
 * @brief Zapamatuje cestu pro mechaniku a ověří soubor.
 *
 * @param opt Jméno volby pro chybovou zprávu.
 * @param drive Mechanika 0-7.
 * @param path Cesta (vlastnictví přechází na modul i při chybě).
 * @return true OK; false = chyba vypsaná na stderr.
 */
static bool fdc_cli_set_path ( const char *opt, int drive, gchar *path ) {
    if ( !path || !path[0] ) {
        g_free ( path );
        return fdc_cli_fail ( "%s: missing DSK file path", opt );
    };
    if ( s_cli.path[drive] ) {
        g_free ( path );
        char d[2] = { (char) ( '0' + drive ), 0 };
        return fdc_cli_fail ( "floppy drive %s is given more than once (--fdd, --fdd<N>)", d );
    };
    if ( !g_file_test ( path, G_FILE_TEST_IS_REGULAR ) ) {
        fprintf ( stderr, "Error: %s: DSK file not found: %s\n", opt, path );
        g_free ( path );
        return false;
    };
    s_cli.path[drive] = path;
    return true;
}


bool fdc_cli_validate ( void ) {
    /* --fdd <N>=<soubor> (opakovatelná) */
    int cnt = sdlapp_option_count ( "--fdd" );
    for ( int i = 0; i < cnt; i++ ) {
        gchar *spec = mzarch_cli_option_value_utf8_nth ( "--fdd", i );
        const char *eq = spec ? strchr ( spec, '=' ) : NULL;
        if ( !eq ) {
            fdc_cli_fail ( "--fdd: expected <N>=<file> with N 0-7, got: %s", spec ? spec : "" );
            g_free ( spec );
            return false;
        };
        int drive = fdc_cli_parse_drive ( spec, (size_t) ( eq - spec ) );
        if ( drive < 0 ) {
            fdc_cli_fail ( "--fdd: floppy drive number must be 0-7: %s", spec );
            g_free ( spec );
            return false;
        };
        gchar *path = g_strdup ( eq + 1 );
        g_free ( spec );
        if ( !fdc_cli_set_path ( "--fdd", drive, path ) ) return false;
    };

    /* --fdd0 .. --fdd7 <soubor> */
    for ( int drive = 0; drive < FDC_CLI_DRIVES; drive++ ) {
        char opt[8];
        g_snprintf ( opt, sizeof ( opt ), "--fdd%d", drive );
        if ( !sdlapp_option_present ( opt ) ) continue;
        if ( sdlapp_option_count ( opt ) > 1 ) {
            return fdc_cli_fail ( "%s is given more than once", opt );
        };
        if ( !fdc_cli_set_path ( opt, drive, mzarch_cli_option_value_utf8 ( opt ) ) ) return false;
    };

    /* --fdd-readonly <N> (opakovatelná) */
    cnt = sdlapp_option_count ( "--fdd-readonly" );
    for ( int i = 0; i < cnt; i++ ) {
        const char *v = sdlapp_option_value_nth ( "--fdd-readonly", i );
        int drive = v ? fdc_cli_parse_drive ( v, strlen ( v ) ) : -1;
        if ( drive < 0 ) {
            return fdc_cli_fail ( "--fdd-readonly: floppy drive number must be 0-7: %s", v ? v : "" );
        };
        s_cli.readonly[drive] = true;
    };
    return true;
}


/**
 * @brief Index mechaniky v tabulce modulu, nebo -1 mimo rozsah.
 */
static int fdc_cli_index ( unsigned fdc_index, unsigned drive_id ) {
    if ( fdc_index >= FDC_INSTANCE_COUNT || drive_id >= FDC_NUM_DRIVES ) return -1;
    return (int) ( fdc_index * FDC_NUM_DRIVES + drive_id );
}


const char *fdc_cli_dsk_path ( unsigned fdc_index, unsigned drive_id ) {
    int i = fdc_cli_index ( fdc_index, drive_id );
    return ( i < 0 ) ? NULL : s_cli.path[i];
}


bool fdc_cli_readonly ( unsigned fdc_index, unsigned drive_id ) {
    int i = fdc_cli_index ( fdc_index, drive_id );
    return ( i < 0 ) ? false : s_cli.readonly[i];
}


void fdc_cli_note_failure ( const char *message ) {
    fprintf ( stderr, "Error: %s\n", message ? message : "" );
    if ( !s_cli.failure ) s_cli.failure = g_strdup ( message ? message : "" );
}


bool fdc_cli_report ( void ) {
    if ( !s_cli.failure ) return true;
    automation_request_exit ( AUTOMATION_EXIT_ERROR, AUTOMATION_REASON_ERROR, s_cli.failure, NULL );
    return false;
}
