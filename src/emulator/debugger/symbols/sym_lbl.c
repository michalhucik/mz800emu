/*
 * File:   sym_lbl.c
 *
 * Vlastní .lbl formát - user write-back symbolů s komentáři.
 *
 * Format line-based:
 *   # comment-only line (skip)
 *   <addr_hex> <name> <bank> [; per-symbol comment]
 *
 * Příklady:
 *   D8A2  wboot       0
 *   2A00  ccp_main    9   ; CCP entry, jen v PEHU bank 9
 *   # Toto je komentář souboru, ignoruje se
 *
 * Specifikace:
 *   - addr_hex je vždy hex (BEZ prefixu, BEZ '$', BEZ '0x'). Velká nebo
 *     malá písmena.
 *   - name = identifier, žádný whitespace.
 *   - bank je decimální 0-255. 0 = CPU view (= globální).
 *   - komentář = vše za prvním ';' do konce řádku, leading whitespace
 *     se trimne.
 *   - prázdné řádky a řádky začínající '#' jsou ignorovány.
 *
 * Serializer ukládá pouze SYM_SOURCE_LBL záznamy. Záznamy z jiných
 * zdrojů (NOI/MAP/SYM) zůstávají v session-only DB - .lbl soubor je
 * určen jako uživatelská anotace, ne jako re-export buildchain dat.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "sym_db.h"


extern int sym_db_add_internal ( const char *name, uint32_t addr,
                                 uint8_t bank_id, en_SYM_SOURCE source,
                                 const char *comment, const char *module );


/**
 * @brief Trimne trailing whitespace v bufferu.
 */
static void rtrim ( char *s ) {
    if ( !s ) return;
    size_t n = strlen ( s );
    while ( n > 0 && isspace ( (unsigned char) s [ n - 1 ] ) ) {
        s [ --n ] = '\0';
    };
}


/**
 * @brief Načte .lbl soubor.
 *
 * @return počet načtených symbolů, -1 při I/O chybě
 */
int sym_db_load_lbl ( const char *path ) {
    if ( !path ) return -1;
    FILE *fp = fopen ( path, "r" );
    if ( !fp ) return -1;

    char buf [ 1024 ];
    int loaded = 0;

    while ( fgets ( buf, sizeof ( buf ), fp ) ) {
        size_t len = strlen ( buf );
        while ( len > 0 && ( buf [ len - 1 ] == '\n' || buf [ len - 1 ] == '\r' ) ) {
            buf [ --len ] = '\0';
        };

        const char *p = buf;
        while ( *p && isspace ( (unsigned char) *p ) ) p++;
        if ( !*p || *p == '#' ) continue;

        /* addr hex */
        unsigned int addr = 0;
        int consumed = 0;
        if ( sscanf ( p, "%X%n", &addr, &consumed ) != 1 || consumed == 0 ) {
            continue;
        };
        p += consumed;
        while ( *p && isspace ( (unsigned char) *p ) ) p++;
        if ( !*p ) continue;

        /* name */
        char name [ 256 ];
        size_t ni = 0;
        while ( *p && !isspace ( (unsigned char) *p ) && ni < sizeof ( name ) - 1 ) {
            name [ ni++ ] = *p++;
        };
        name [ ni ] = '\0';
        if ( ni == 0 ) continue;

        /* bank (decimal) */
        while ( *p && isspace ( (unsigned char) *p ) ) p++;
        unsigned int bank = 0;
        if ( *p && *p != ';' ) {
            consumed = 0;
            if ( sscanf ( p, "%u%n", &bank, &consumed ) == 1 && consumed > 0 ) {
                p += consumed;
            };
        };

        /* optional comment after ';' */
        const char *comment_arg = NULL;
        char comment_buf [ 1024 ];
        comment_buf [ 0 ] = '\0';
        const char *semi = strchr ( p, ';' );
        if ( semi ) {
            const char *c = semi + 1;
            while ( *c && isspace ( (unsigned char) *c ) ) c++;
            size_t copylen = strlen ( c );
            if ( copylen >= sizeof ( comment_buf ) ) copylen = sizeof ( comment_buf ) - 1;
            memcpy ( comment_buf, c, copylen );
            comment_buf [ copylen ] = '\0';
            rtrim ( comment_buf );
            if ( comment_buf [ 0 ] != '\0' ) comment_arg = comment_buf;
        };

        if ( bank > 0xFF ) bank = 0xFF;
        if ( sym_db_add_internal ( name, (uint32_t) addr, (uint8_t) bank,
                                   SYM_SOURCE_LBL, comment_arg, NULL ) ) {
            loaded++;
        };
    };

    fclose ( fp );
    return loaded;
}


/**
 * @brief Serializuje SYM_SOURCE_LBL záznamy do .lbl souboru.
 *
 * Format výstupu:
 *   # mz800new debugger label file
 *   # <addr_hex>  <name>  <bank>  ; comment
 *   D8A2  wboot       0
 *   2A00  ccp_main    9   ; CCP entry
 *
 * @return 0 při úspěchu, -1 při chybě otevření / zápisu
 */
int sym_db_save_lbl ( const char *path ) {
    if ( !path ) return -1;
    FILE *fp = fopen ( path, "w" );
    if ( !fp ) return -1;

    fprintf ( fp, "# mz800new debugger label file\n" );
    fprintf ( fp, "# <addr_hex>  <name>  <bank>  ; comment\n" );

    size_t n = sym_db_count ( );
    int written = 0;
    for ( size_t i = 0; i < n; i++ ) {
        const st_SYMBOL *s = sym_db_get_by_index ( i );
        if ( !s || s->source != SYM_SOURCE_LBL ) continue;
        if ( !s->name ) continue;

        if ( s->comment && *s->comment ) {
            fprintf ( fp, "%04X  %-24s  %u  ; %s\n",
                      (unsigned) s->addr, s->name,
                      (unsigned) s->bank_id, s->comment );
        }
        else {
            fprintf ( fp, "%04X  %-24s  %u\n",
                      (unsigned) s->addr, s->name,
                      (unsigned) s->bank_id );
        };
        written++;
    };

    int err = ferror ( fp );
    fclose ( fp );
    if ( err ) return -1;
    (void) written;  /* informativní, neexponujeme */
    return 0;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
