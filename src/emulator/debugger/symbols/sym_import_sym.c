/*
 * File:   sym_import_sym.c
 *
 * Importér .sym formátu (sjasmplus + pasmo).
 *
 * Format (per řádek):
 *   <NAME> EQU <value>
 *
 * Tolerance:
 *   - case-insensitive klíčové slovo "EQU"
 *   - separator (whitespace) může být mezera i tabulátor
 *   - komentáře ';' a '#' (na začátku i na konci řádku) se odstraňují
 *
 * Hodnota může být v těchto formátech:
 *   - hex s '$' prefixem:        $4042       (sjasmplus styl)
 *   - hex s '0x' prefixem:       0x4042      (robustní, ne nativní)
 *   - hex s 'H'/'h' suffixem:    0C000H      (pasmo styl)
 *   - decimální:                 16450
 *
 * Příklady:
 *   sjasmplus:  print_char EQU $4042
 *   sjasmplus:  some_const EQU 0x0010
 *   pasmo:      start          EQU 0C000H
 *   obecné:     counter    EQU 100
 *
 * Liché řádky (komentáře, prázdné, makra) se přeskočí. Skutečný
 * zdroj symbolu (pasmo vs sjasmplus) parser nerozlišuje - oba se
 * ukládají s en_SYM_SOURCE = SYM_SOURCE_SJASMPLUS (= nejnižší
 * priorita v sym_db, viz sym_db.h).
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
 * @brief Detekuje pasmo-styl hex suffix 'H'/'h' v tokenu.
 *
 * Token je platný pasmo hex pokud:
 *   - poslední znak je 'H' nebo 'h'
 *   - všechny předchozí znaky jsou hex cifry [0-9A-Fa-f]
 *   - délka >= 2 (alespoň jedna cifra + suffix)
 *
 * Pasmo prefixuje neambiguálně - leading znak musí být cifra (např.
 * `0C000H` ne `C000H`), ale toto kontrolujeme implicitně přes
 * předchozí filtraci identifikátoru: hodnota začínající písmenem by
 * byla zaměnitelná s identifikátorem, takže pasmo i sjasmplus vždy
 * dávají leading 0 pro hex hodnoty které začínají písmenem.
 *
 * @param tok    Pointer na začátek tokenu (po trim).
 * @param toklen Délka tokenu bez whitespace/komentáře.
 * @return 1 pokud token je pasmo hex, 0 jinak
 */
static int is_pasmo_hex ( const char *tok, size_t toklen ) {
    if ( toklen < 2 ) return 0;
    char suffix = tok [ toklen - 1 ];
    if ( suffix != 'H' && suffix != 'h' ) return 0;
    for ( size_t i = 0; i < toklen - 1; i++ ) {
        if ( !isxdigit ( (unsigned char) tok [ i ] ) ) return 0;
    };
    return 1;
}


/**
 * @brief Pokusí se naparsovat hodnotu (hex/dec).
 *
 * Podporované formáty:
 *   - $NNNN     (sjasmplus hex prefix)
 *   - 0xNNNN    (C-styl hex prefix)
 *   - NNNNH     (pasmo hex suffix; první znak musí být cifra)
 *   - NNNN      (decimal fallback)
 *
 * Trailing komentáře (`;`, `#`) i whitespace se před parsováním
 * odstraní z konce tokenu.
 *
 * @return 1 při úspěchu, 0 při chybě
 */
static int parse_value ( const char *s, uint32_t *out ) {
    if ( !s || !*s ) return 0;
    while ( *s && isspace ( (unsigned char) *s ) ) s++;
    if ( !*s ) return 0;

    /* Najdi konec tokenu = whitespace nebo začátek komentáře. */
    size_t toklen = 0;
    while ( s [ toklen ] && !isspace ( (unsigned char) s [ toklen ] )
            && s [ toklen ] != ';' && s [ toklen ] != '#' ) {
        toklen++;
    };
    if ( toklen == 0 ) return 0;

    unsigned int v = 0;
    int consumed = 0;

    if ( s [ 0 ] == '$' ) {
        if ( sscanf ( s + 1, "%X%n", &v, &consumed ) != 1 || consumed == 0 ) return 0;
        *out = (uint32_t) v;
        return 1;
    };
    if ( s [ 0 ] == '0' && toklen >= 2 && ( s [ 1 ] == 'x' || s [ 1 ] == 'X' ) ) {
        if ( sscanf ( s + 2, "%X%n", &v, &consumed ) != 1 || consumed == 0 ) return 0;
        *out = (uint32_t) v;
        return 1;
    };
    /* Pasmo styl: NNNNH / NNNNh suffix. */
    if ( is_pasmo_hex ( s, toklen ) ) {
        /* Zkopíruj bez suffixu do lokálního bufferu pro sscanf. */
        char hexbuf [ 32 ];
        size_t copylen = toklen - 1;
        if ( copylen >= sizeof ( hexbuf ) ) copylen = sizeof ( hexbuf ) - 1;
        memcpy ( hexbuf, s, copylen );
        hexbuf [ copylen ] = '\0';
        if ( sscanf ( hexbuf, "%X%n", &v, &consumed ) != 1 || consumed == 0 ) return 0;
        *out = (uint32_t) v;
        return 1;
    };
    /* decimal default */
    if ( sscanf ( s, "%u%n", &v, &consumed ) != 1 || consumed == 0 ) return 0;
    *out = (uint32_t) v;
    return 1;
}


/**
 * @brief Načte .sym soubor (sjasmplus nebo pasmo).
 *
 * Parser je společný pro oba assemblery - rozlišuje se přes hex
 * notaci (sjasmplus `$NNNN`/`0xNNNN`, pasmo `NNNNH`). Symboly se
 * ukládají vždy s en_SYM_SOURCE = SYM_SOURCE_SJASMPLUS (nejnižší
 * priorita v sym_db).
 *
 * @return počet načtených symbolů, -1 při I/O chybě.
 */
int sym_db_load_sym ( const char *path ) {
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
        if ( !*p || *p == ';' || *p == '#' ) continue;

        /* identifier */
        char name [ 256 ];
        size_t ni = 0;
        while ( *p && !isspace ( (unsigned char) *p ) && ni < sizeof ( name ) - 1 ) {
            name [ ni++ ] = *p++;
        };
        name [ ni ] = '\0';
        if ( ni == 0 ) continue;

        /* whitespace */
        while ( *p && isspace ( (unsigned char) *p ) ) p++;

        /* "EQU" keyword (case-insensitive) */
        if ( !( ( p [ 0 ] == 'E' || p [ 0 ] == 'e' ) &&
                ( p [ 1 ] == 'Q' || p [ 1 ] == 'q' ) &&
                ( p [ 2 ] == 'U' || p [ 2 ] == 'u' ) &&
                ( p [ 3 ] == '\0' || isspace ( (unsigned char) p [ 3 ] ) ) ) ) {
            continue;
        };
        p += 3;
        while ( *p && isspace ( (unsigned char) *p ) ) p++;

        uint32_t addr = 0;
        if ( !parse_value ( p, &addr ) ) continue;

        if ( sym_db_add_internal ( name, addr, 0,
                                   SYM_SOURCE_SJASMPLUS, NULL, NULL ) ) {
            loaded++;
        };
    };

    fclose ( fp );
    return loaded;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
