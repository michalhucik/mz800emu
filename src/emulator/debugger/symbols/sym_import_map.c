/*
 * File:   sym_import_map.c
 *
 * Importér sdldz80 linker map (.map).
 *
 * Format hlavičky:
 *   ASxxxx Linker V03.00/V05.40 + sdld,  page N.
 *   Hexadecimal  [32-Bits]
 *
 *   Area  ...
 *   ...
 *   .  .ABS.  ...
 *
 *         Value  Global                              Global Defined In Module
 *         -----  --------------------------------   ------------------------
 *        00000000  symbol_name                        module_name
 *        ...
 *
 * Strategie parseru:
 *   - State machine: idle / inside_global_section.
 *   - Trigger pro start global section: řádek obsahuje "Global Defined In Module"
 *     a následující řádek (= header dashes) se přeskočí.
 *   - End global section: prázdný řádek, "Area " prefix, nebo
 *     "ASxxxx Linker" header (= další stránka).
 *   - Per řádek extrahovat: hex value, identifier, module (volitelně).
 *
 * Skip kritéria:
 *   - Identifikátor začínající '.' (.__.ABS., .__.CPU. apod.) - artefakty
 *     linkeru, ne uživatelské symboly.
 *   - Identifikátor začínající "l__" / "s__" - linker section markers.
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
 * @brief Vrací true, pokud je řetězec jen z whitespace (= prázdná).
 */
static int is_blank ( const char *s ) {
    if ( !s ) return 1;
    while ( *s ) {
        if ( !isspace ( (unsigned char) *s ) ) return 0;
        s++;
    };
    return 1;
}


/**
 * @brief Vrací true, pokud je identifier "interní" (linker artefakt).
 *
 * Filtrujeme:
 *   - "." prefix (.__.ABS., .__.CPU., ...)
 *   - "l__" / "s__" prefix (l__BSEG, s__CODE, ...)
 */
static int is_internal_symbol ( const char *name ) {
    if ( !name || !*name ) return 1;
    if ( name [ 0 ] == '.' ) return 1;
    if ( strncmp ( name, "l__", 3 ) == 0 ) return 1;
    if ( strncmp ( name, "s__", 3 ) == 0 ) return 1;
    return 0;
}


/**
 * @brief Načte sdldz80 linker map.
 *
 * @return počet načtených symbolů, -1 při I/O chybě.
 */
int sym_db_load_map ( const char *path ) {
    if ( !path ) return -1;
    FILE *fp = fopen ( path, "r" );
    if ( !fp ) return -1;

    char buf [ 1024 ];
    int loaded = 0;
    int in_global_section = 0;
    int just_entered = 0;     /* = next řádek je dashes header, přeskočit */

    while ( fgets ( buf, sizeof ( buf ), fp ) ) {
        size_t len = strlen ( buf );
        while ( len > 0 && ( buf [ len - 1 ] == '\n' || buf [ len - 1 ] == '\r' ) ) {
            buf [ --len ] = '\0';
        };

        /* hledáme začátek global sekce */
        if ( !in_global_section ) {
            if ( strstr ( buf, "Global Defined In Module" ) != NULL ) {
                in_global_section = 1;
                just_entered = 1;
            };
            continue;
        };

        if ( just_entered ) {
            just_entered = 0;
            continue;  /* dashes header */
        };

        /* end konec global sekce: prázdný řádek nebo "Area " nebo nová stránka */
        if ( is_blank ( buf ) ||
             strstr ( buf, "ASxxxx Linker" ) != NULL ) {
            in_global_section = 0;
            continue;
        };
        if ( strncmp ( buf, "Area", 4 ) == 0 ) {
            in_global_section = 0;
            continue;
        };

        /* Parse řádku:
         *      <whitespace> <hex_value> <whitespace> <name> [whitespace <module>]
         */
        const char *p = buf;
        while ( *p && isspace ( (unsigned char) *p ) ) p++;
        if ( !*p ) continue;

        /* hex value */
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
        if ( is_internal_symbol ( name ) ) continue;

        /* optional module */
        while ( *p && isspace ( (unsigned char) *p ) ) p++;
        char module [ 256 ];
        size_t mi = 0;
        while ( *p && !isspace ( (unsigned char) *p ) && mi < sizeof ( module ) - 1 ) {
            module [ mi++ ] = *p++;
        };
        module [ mi ] = '\0';
        const char *mod_arg = ( mi > 0 ) ? module : NULL;

        if ( sym_db_add_internal ( name, (uint32_t) addr, 0,
                                   SYM_SOURCE_MAP, NULL, mod_arg ) ) {
            loaded++;
        };
    };

    fclose ( fp );
    return loaded;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
