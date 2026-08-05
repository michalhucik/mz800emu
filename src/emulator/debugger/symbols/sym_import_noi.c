/*
 * File:   sym_import_noi.c
 *
 * Importér SDCC NoICE formátu (= sdldz80 .noi výstup).
 *
 * Format (per řádek):
 *   DEF <name> 0x<hex_address>
 *
 * Příklad (z mzdos all-areas.noi):
 *   DEF wboot 0xD8A2
 *   DEF cpm_bdos_shim 0xD9C4
 *
 * Liché řádky (komentáře, prázdné, jiné direktivy) se ignorují.
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

#include "sym_db.h"


/* Forward decl interního API z sym_db.c */
extern int sym_db_add_internal ( const char *name, uint32_t addr,
                                 uint8_t bank_id, en_SYM_SOURCE source,
                                 const char *comment, const char *module );


/**
 * @brief Načte SDCC NoICE format soubor.
 *
 * Parser je tolerantní: nesplňující řádky se přeskočí bez chyby.
 *
 * @return počet úspěšně načtených (= upsertnutých) symbolů, -1 při I/O chybě
 */
int sym_db_load_noi ( const char *path ) {
    if ( !path ) return -1;
    FILE *fp = fopen ( path, "r" );
    if ( !fp ) return -1;

    char buf [ 1024 ];
    int loaded = 0;

    while ( fgets ( buf, sizeof ( buf ), fp ) ) {
        /* odstranit trailing newline */
        size_t len = strlen ( buf );
        while ( len > 0 && ( buf [ len - 1 ] == '\n' || buf [ len - 1 ] == '\r' ) ) {
            buf [ --len ] = '\0';
        };

        char name [ 256 ];
        unsigned int addr;
        /* sscanf: "DEF <name> 0x<hex>" */
        if ( sscanf ( buf, "DEF %255s 0x%X", name, &addr ) == 2 ) {
            if ( sym_db_add_internal ( name, (uint32_t) addr, 0,
                                       SYM_SOURCE_NOI, NULL, NULL ) ) {
                loaded++;
            };
        };
    };

    fclose ( fp );
    return loaded;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
