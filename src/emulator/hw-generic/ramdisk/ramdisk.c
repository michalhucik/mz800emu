/* 
 * File:   ramdisk.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 10. srpna 2015, 11:05
 * 
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

/*
 *
 *	Standardni ramdisk (max. velikost 256 bank * 64 KB = 16 MB)
 *
 *
 *	Rezim RD:
 *			-w 0xe9 - nastaveni stranky 0x00 - 0xff
 *			rw 0xea - R/W data + increment adresy v RD
 *			-w 0xeb - nastaveni dolnich 16 bitu adresy RD
 *			r- 0xf8 - reset adresy a stranky
 *
 *
 *	Rezim SRAM:
 *			r- 0xf8 - reset adresy
 *			r- 0xf9 - cteni dat + increment adresy
 *			-w 0xfa - zapis dat + increment adresy
 *
 *
 *
 *
 *
 *	Standartni PEZIK:
 *
 *
 *		Pouziva porty 0xe8 - 0xef
 *
 *		Cislo portu vzdy urcuje banku se kterou pracujeme.
 *
 *		Pri IORQ RD se vzdy do latch registru prenese horni cast sbernice.
 *		Po dokonceni RD operace je obsah tohoto latche vystaven jako spodni adresa offsetu.
 *
 */

#ifdef WINDOWS
#include<windows.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "mzarch/mzcommon_config.h"
#include "mzarch/mzhal.h"
#include "emulator.h"
#include "mzarch/mzarch.h"
#include "ramdisk.h"
#include "cfgmain.h"
#include "baseui/baseui.h"

//#define DBGLEVEL        ( DBGNON /* | DBGERR | DBGWAR | DBGINF */ )
//#define DBGLEVEL        ( DBGNON | DBGERR | DBGWAR | DBGINF )
#include "debug.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#endif

#define DEF_BANK_SIZE  0x10000

st_RAMDISK g_ramdisk;


/**
 * @brief Runtime default jméno záložního souboru MR-1R18 ("rd-<arch>.dat").
 *
 * Skládá se z g_mzhal.arch_name (mzhal krok 7) - hodnota identická
 * s dřívějším compile-time "rd-" MZ_PLATFORM_SUFFIX ".dat". Vrací
 * ukazatel na statický buffer naplněný při prvním volání (single-thread
 * init cesta, g_mzhal je const od load-time).
 *
 * @return Jméno souboru relativní k cfg_dir; ukazatel je platný po
 *         celou dobu běhu, volající ho NEuvolňuje.
 */
static const char *ramdisk_default_filename ( void ) {
    static char fname[32];
    if ( fname[0] == 0x00 ) {
        snprintf ( fname, sizeof ( fname ), "rd-%s.dat", g_mzhal.arch_name );
    };
    return fname;
}


void ramdisk_load_backup_file ( uint8_t *memory, char *filepath, unsigned ramdisk_size ) {

    FILE *fp;

    if ( baseui_tools_file_access ( filepath, F_OK ) != -1 ) {
        if ( ( fp = baseui_tools_file_open ( filepath, "rb" ) ) ) {
            unsigned filesize = baseui_tools_file_read ( memory, 1, ramdisk_size, fp );
            if ( filesize != ramdisk_size ) {
                baseui_show_message (0, "Your RD file has only %d bytes of requested %d bytes. Relax, this is not problem ... this is only warning :)", filesize, ramdisk_size );
            };
        } else {
            baseui_error ( "Can't open file '%s': %s", filepath, strerror ( errno ) );
        };
        fclose ( fp );
    };
}


void ramdisk_save_backup_file ( uint8_t *memory, char *filepath, unsigned ramdisk_size ) {

    FILE *fp;

    if ( ( fp = baseui_tools_file_open ( filepath, "wb" ) ) ) {
        unsigned filesize = baseui_tools_file_write ( memory, 1, ramdisk_size, fp );
        if ( filesize != ramdisk_size ) {
            baseui_error ( "Saved only %d bytes of %d - file '%s': %s", filesize, ramdisk_size, filepath, strerror ( errno ) );
        };
        fclose ( fp );

    } else {
        baseui_error ( "Can't open file '%s': %s", filepath, strerror ( errno ) );
    };
}


void ramdisk_std_save ( void ) {

    if ( ( g_ramdisk.std.connected ) && ( g_ramdisk.std.memory != NULL ) ) {
        if ( g_ramdisk.std.type == RAMDISK_TYPE_SRAM ) {
            unsigned ramdisk_size = ( g_ramdisk.std.size + 1 ) * DEF_BANK_SIZE;
            ramdisk_save_backup_file ( g_ramdisk.std.memory, g_ramdisk.std.filepath, ramdisk_size );
        };
    };
}


void ramdisk_pezik_save ( unsigned pezik_type ) {

    if ( ( g_ramdisk.pezik [ pezik_type ]. connected ) && ( g_ramdisk.pezik [ pezik_type ]. memory != NULL ) ) {
        if ( g_ramdisk.pezik [ pezik_type ].backuped == PEZIK_BACKUPED_YES ) {
            unsigned ramdisk_size = 8 * DEF_BANK_SIZE;
            ramdisk_save_backup_file ( g_ramdisk.pezik [ pezik_type ].memory, g_ramdisk.pezik [ pezik_type ].filepath, ramdisk_size );
        };
    };
}


void ramdisk_std_disconnect ( void ) {

    ramdisk_std_save ( );

    if ( g_ramdisk.std.memory != NULL ) {
        free ( g_ramdisk.std.memory );
        g_ramdisk.std.memory = NULL;
    };
    g_ramdisk.std.connected = RAMDISK_DISCONNECTED;
}


void ramdisk_std_init ( int connect, en_RAMDISK_TYPE type, en_RAMDISK_BANKMASK size, char *filepath ) {

    DBGPRINTF ( DBGINF, "connect = %d, type = %d, bank_mask = 0x%02x, file = %s\n", connect, type, size, filepath );

    if ( connect ) {
        /* nepovolena kombinace */
        if ( g_ramdisk.pezik [ RAMDISK_PEZIK_E8 ].connected ) {
            g_ramdisk.std.connected = RAMDISK_DISCONNECTED;
            return;
        };

        ramdisk_std_save ( );

        if ( g_ramdisk.std.memory == NULL ) {
            g_ramdisk.std.memory = malloc ( ( size + 1 ) * DEF_BANK_SIZE );

            if ( g_ramdisk.std.memory == NULL ) {
                fprintf ( stderr, "%s():%d - Could not allocate memory: %s\n", __func__, __LINE__, strerror ( errno ) );
                emulator_quit ( EXIT_FAILURE );
            };

            /* implicitni obsah nesmi byt 0x00 - jinak se pri bootu zacne nacitat program :) */
            /* TODO: radeji to jeste proverime */
            memset ( g_ramdisk.std.memory, 0xff, ( size + 1 ) * DEF_BANK_SIZE );
        } else {
            g_ramdisk.std.memory = realloc ( g_ramdisk.std.memory, ( size + 1 ) * DEF_BANK_SIZE );

            if ( g_ramdisk.std.memory == NULL ) {
                fprintf ( stderr, "%s():%d - Could not allocate memory: %s\n", __func__, __LINE__, strerror ( errno ) );
                emulator_quit ( EXIT_FAILURE );
            };
        };

        g_ramdisk.std.size = size;
        g_ramdisk.std.type = type;

        if ( g_ramdisk.std.type & RAMDISK_IS_IN_FILE ) {
            if ( filepath[0] != 0x00 ) {
                int len = strlen ( filepath ) + 1;
                g_ramdisk.std.filepath = (char*) baseui_tools_mem_realloc ( g_ramdisk.std.filepath, len );
                strncpy ( g_ramdisk.std.filepath, filepath, len );
            } else {
                const char *default_fname = ramdisk_default_filename ( );
                int len = strlen ( default_fname ) + 1;
                g_ramdisk.std.filepath = (char*) baseui_tools_mem_realloc ( g_ramdisk.std.filepath, len );
                strncpy ( g_ramdisk.std.filepath, default_fname, len );
            };

            unsigned ramdisk_size = ( g_ramdisk.std.size + 1 ) * DEF_BANK_SIZE;
            ramdisk_load_backup_file ( g_ramdisk.std.memory, g_ramdisk.std.filepath, ramdisk_size );
        };

    } else {
        ramdisk_std_disconnect ( );
        g_ramdisk.std.size = size;
        g_ramdisk.std.type = type;
        int len = strlen ( filepath ) + 1;
        g_ramdisk.std.filepath = (char*) baseui_tools_mem_realloc ( g_ramdisk.std.filepath, len );
        strncpy ( g_ramdisk.std.filepath, filepath, len );
    };
    g_ramdisk.std.connected = connect;
}

void ramdisk_pezik_disconnect ( int pezik_type ) {

    if ( RAMDISK_DISCONNECTED == g_ramdisk.pezik [ pezik_type ].connected ) return;

    if ( g_ramdisk.pezik [ pezik_type ].memory != NULL ) {

        if ( PEZIK_BACKUPED_YES == g_ramdisk.pezik [ pezik_type ].backuped ) {
            unsigned ramdisk_size = 8 * DEF_BANK_SIZE;
            ramdisk_save_backup_file ( g_ramdisk.pezik [ pezik_type ].memory, g_ramdisk.pezik [ pezik_type ].filepath, ramdisk_size );
        };

        free ( g_ramdisk.pezik [ pezik_type ].memory );
        g_ramdisk.pezik [ pezik_type ].memory = NULL;
    };

    g_ramdisk.pezik [ pezik_type ].connected = RAMDISK_DISCONNECTED;
}


void ramdisk_pezik_connect ( int pezik_type ) {

    if ( RAMDISK_CONNECTED == g_ramdisk.pezik [ pezik_type ].connected ) return;

    if ( ( pezik_type == RAMDISK_PEZIK_E8 ) && ( g_ramdisk.std.connected ) ) {
        /* nepovolena kombinace */
        g_ramdisk.pezik [ RAMDISK_PEZIK_E8 ]. connected = RAMDISK_DISCONNECTED;
        return;
    };

    unsigned ramdisk_size = 8 * DEF_BANK_SIZE;

    if ( g_ramdisk.pezik [ pezik_type ].memory == NULL ) {
        g_ramdisk.pezik [ pezik_type ].memory = malloc ( ramdisk_size );

        if ( g_ramdisk.pezik [ pezik_type ].memory == NULL ) {
            fprintf ( stderr, "%s():%d - Could not allocate memory: %s\n", __func__, __LINE__, strerror ( errno ) );
            emulator_quit ( EXIT_FAILURE );
        };
    };

    memset ( g_ramdisk.pezik [ pezik_type ].memory, 0xff, ramdisk_size );

    if ( PEZIK_BACKUPED_YES == g_ramdisk.pezik [ pezik_type ].backuped ) {
        ramdisk_load_backup_file ( g_ramdisk.pezik [ pezik_type ].memory, g_ramdisk.pezik [ pezik_type ].filepath, ramdisk_size );
    };

    g_ramdisk.pezik [ pezik_type ].connected = RAMDISK_CONNECTED;
}


void ramdisk_pezik_init ( int pezik_type, int connect, int portmask, int backuped, char *filepath ) {

    if ( ( !connect ) && ( g_ramdisk.pezik [ pezik_type ].connected ) ) {
        ramdisk_pezik_disconnect ( pezik_type );
    };

    g_ramdisk.pezik [ pezik_type ].portmask = portmask;

    unsigned len = strlen ( filepath );

    if ( len ) {
        g_ramdisk.pezik [ pezik_type ].backuped = backuped;
    } else {
        g_ramdisk.pezik [ pezik_type ].backuped = PEZIK_BACKUPED_NO;
    };

    len++;

    g_ramdisk.pezik[pezik_type].filepath = (char*) baseui_tools_mem_realloc ( g_ramdisk.pezik[pezik_type].filepath, len );

    strncpy ( g_ramdisk.pezik[pezik_type].filepath, filepath, len );

    if ( connect ) {
        ramdisk_pezik_connect ( pezik_type );
    };
}


void ramdisk_propagatecfg ( void *m, void *data ) {
    (void) data;
    
    unsigned pezik_pluged;
    unsigned pezik_portmask;
    unsigned pezik_backuped;
    char *pezik_filepath;

    pezik_pluged = cfgmodule_get_element_bool_value_by_name ( (CFGMOD *) m, "pezik_e8_pluged" );
    pezik_portmask = cfgmodule_get_element_unsigned_value_by_name ( (CFGMOD *) m, "pezik_e8_portmask" );
    pezik_backuped = cfgmodule_get_element_bool_value_by_name ( (CFGMOD *) m, "pezik_e8_backuped" );
    pezik_filepath = cfgmodule_get_element_text_value_by_name ( (CFGMOD *) m, "pezik_e8_filepath" );

    ramdisk_pezik_init ( RAMDISK_PEZIK_E8, pezik_pluged, pezik_portmask, pezik_backuped, pezik_filepath );

    pezik_pluged = cfgmodule_get_element_bool_value_by_name ( (CFGMOD *) m, "pezik_68_pluged" );
    pezik_portmask = cfgmodule_get_element_unsigned_value_by_name ( (CFGMOD *) m, "pezik_68_portmask" );
    pezik_backuped = cfgmodule_get_element_bool_value_by_name ( (CFGMOD *) m, "pezik_68_backuped" );
    pezik_filepath = cfgmodule_get_element_text_value_by_name ( (CFGMOD *) m, "pezik_68_filepath" );

    ramdisk_pezik_init ( RAMDISK_PEZIK_68, pezik_pluged, pezik_portmask, pezik_backuped, pezik_filepath );

    int mr1r18_pluged = cfgmodule_get_element_bool_value_by_name ( (CFGMOD *) m, "mr1r18_pluged" );
    en_RAMDISK_TYPE mr1r18_type = cfgmodule_get_element_keyword_value_by_name ( (CFGMOD *) m, "mr1r18_type" );
    en_RAMDISK_BANKMASK mr1r18_size = cfgmodule_get_element_keyword_value_by_name ( (CFGMOD *) m, "mr1r18_size" );
    char *mr1r18_filepath = cfgmodule_get_element_text_value_by_name ( (CFGMOD *) m, "mr1r18_filepath" );

    ramdisk_std_init ( mr1r18_pluged, mr1r18_type, mr1r18_size, mr1r18_filepath );
}


void ramdisk_init ( void ) {

    memset ( &g_ramdisk, 0x00, sizeof ( g_ramdisk ) );
    g_ramdisk.pezik[RAMDISK_PEZIK_E8].portmask = 0xff;
    g_ramdisk.pezik[RAMDISK_PEZIK_68].portmask = 0xff;
    g_ramdisk.std.filepath = baseui_tools_mem_alloc0 ( 1 );
    g_ramdisk.pezik[RAMDISK_PEZIK_E8].filepath = (char*) baseui_tools_mem_alloc0 ( 1 );
    g_ramdisk.pezik[RAMDISK_PEZIK_68].filepath = (char*) baseui_tools_mem_alloc0 ( 1 );

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "RAMDISK" );

    CFGELM *elm;

    /* MR1R18 */
    elm = cfgmodule_register_new_element ( cmod, "mr1r18_pluged", CFGENTYPE_BOOL, RAMDISK_CONNECTED );
    cfgelement_set_handlers ( elm, NULL, (void*) &g_ramdisk.std.connected );

    elm = cfgmodule_register_new_element ( cmod, "mr1r18_type", CFGENTYPE_KEYWORD, RAMDISK_TYPE_SRAM,
                                           RAMDISK_TYPE_STD, "STANDARD",
                                           RAMDISK_TYPE_SRAM, "SRAM",
                                           RAMDISK_TYPE_ROM, "ROM",
                                           -1 );
    cfgelement_set_handlers ( elm, NULL, (void*) &g_ramdisk.std.type );


    elm = cfgmodule_register_new_element ( cmod, "mr1r18_size", CFGENTYPE_KEYWORD, RAMDISK_SIZE_1M,
                                           RAMDISK_SIZE_64, "64K",
                                           RAMDISK_SIZE_256, "256K",
                                           RAMDISK_SIZE_512, "512K",
                                           RAMDISK_SIZE_1M, "1M",
                                           RAMDISK_SIZE_16M, "16M",
                                           -1 );
    cfgelement_set_handlers ( elm, NULL, (void*) &g_ramdisk.std.size );

    elm = cfgmodule_register_new_element ( cmod, "mr1r18_filepath", CFGENTYPE_TEXT, ramdisk_default_filename ( ) );
    cfgelement_set_pointers ( elm, NULL, (void*) &g_ramdisk.std.filepath );

    /* pezik e8 */
    elm = cfgmodule_register_new_element ( cmod, "pezik_e8_pluged", CFGENTYPE_BOOL, RAMDISK_DISCONNECTED );
    cfgelement_set_handlers ( elm, NULL, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected );

    elm = cfgmodule_register_new_element ( cmod, "pezik_e8_portmask", CFGENTYPE_UNSIGNED, 0xff, 0x01, 0xff );
    cfgelement_set_handlers ( elm, (void*) NULL, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_E8].portmask );

    elm = cfgmodule_register_new_element ( cmod, "pezik_e8_backuped", CFGENTYPE_BOOL, PEZIK_BACKUPED_NO );
    cfgelement_set_handlers ( elm, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_E8].backuped, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_E8].backuped );

    elm = cfgmodule_register_new_element ( cmod, "pezik_e8_filepath", CFGENTYPE_TEXT, RAMDISK_PEZIK_E8_DEFAULT_FILENAME );
    cfgelement_set_pointers ( elm, NULL, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_E8].filepath );

    /* pezik 68 */
    elm = cfgmodule_register_new_element ( cmod, "pezik_68_pluged", CFGENTYPE_BOOL, RAMDISK_DISCONNECTED );
    cfgelement_set_handlers ( elm, NULL, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_68].connected );

    elm = cfgmodule_register_new_element ( cmod, "pezik_68_portmask", CFGENTYPE_UNSIGNED, 0xff, 0x01, 0xff );
    cfgelement_set_handlers ( elm, (void*) NULL, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_68].portmask );

    elm = cfgmodule_register_new_element ( cmod, "pezik_68_backuped", CFGENTYPE_BOOL, PEZIK_BACKUPED_NO );
    cfgelement_set_handlers ( elm, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_68].backuped, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_68].backuped );

    elm = cfgmodule_register_new_element ( cmod, "pezik_68_filepath", CFGENTYPE_TEXT, RAMDISK_PEZIK_68_DEFAULT_FILENAME );
    cfgelement_set_pointers ( elm, NULL, (void*) &g_ramdisk.pezik[RAMDISK_PEZIK_68].filepath );

    cfgmodule_set_propagate_cb ( cmod, ramdisk_propagatecfg, NULL );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


void ramdisc_exit ( void ) {
    int i;

    ramdisk_std_save ( );
    ramdisk_pezik_save ( RAMDISK_PEZIK_E8 );
    ramdisk_pezik_save ( RAMDISK_PEZIK_68 );

    if ( ( g_ramdisk.std.connected ) && ( g_ramdisk.std.memory != NULL ) ) {
        free ( g_ramdisk.std.memory );
    };
    baseui_tools_mem_free ( g_ramdisk.std.filepath );

    for ( i = 0; i < 2; i++ ) {
        if ( ( g_ramdisk.pezik[i].connected ) && ( g_ramdisk.pezik[i].memory != NULL ) ) {
            free ( g_ramdisk.pezik[i].memory );
            baseui_tools_mem_free ( g_ramdisk.pezik[i].filepath );
        };
    };
}


uint8_t ramdisk_std_read_byte ( unsigned addr ) {

    int ramdisc_addr;

    DBGPRINTF ( DBGINF, "addr = 0x%02x\n", addr );

    switch ( addr ) {

        case 0xf8:
            g_ramdisk.std.bank = 0x00;
            g_ramdisk.std.offset = 0x0000;
            return g_mzarch_main.regDBUS_latch;

        case 0xea:
        case 0xf9:
            ramdisc_addr = ( g_ramdisk.std.bank << 16 ) | g_ramdisk.std.offset++;
            return g_ramdisk.std.memory [ ramdisc_addr ];
    };
    return 0;
}


void ramdisk_std_write_byte ( unsigned addr, uint8_t value ) {

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat std ramdisk write.
     *
     * Payload:
     *   [0] = port low byte (0xE9, 0xEA, 0xEB, 0xFA)
     *   [1] = value
     *   [2] = port high byte (může nést offset_high pro 0xEB)
     *   [3..5] = rezervováno
     */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        uint8_t payload[ 6 ] = {
            (uint8_t)( addr & 0xff ),
            value,
            (uint8_t)( ( addr >> 8 ) & 0xff ),
            0, 0, 0
        };
        hwlog_record ( HWLOG_CHIP_RD, HWLOG_RD_STD_WRITE, payload );
    }
#endif

    unsigned ramdisc_addr;

    switch ( addr & 0xff ) {

        case 0xe9:
            g_ramdisk.std.bank = value & g_ramdisk.std.size;
            break;

        case 0xea:
        case 0xfa:
            ramdisc_addr = ( g_ramdisk.std.bank << 16 ) | g_ramdisk.std.offset++;
            if ( !( g_ramdisk.std.type & RAMDISK_IS_READONLY ) ) {
                g_ramdisk.std.memory [ ramdisc_addr ] = value;
            }
            break;

        case 0xeb:
            g_ramdisk.std.offset = ( addr & 0xff00 ) | value;
            break;
    };
}


uint8_t ramdisk_pezik_read_byte ( unsigned addr ) {

    unsigned pezik_addr;
    unsigned pezik_type;
    unsigned pezik_bank;

    pezik_type = ( addr & 0x80 ) >> 7;
    pezik_bank = 1 << ( addr & 0x07 );

    if ( !( pezik_bank & g_ramdisk.pezik [ pezik_type ].portmask ) ) return g_mzarch_main.regDBUS_latch;

    pezik_addr = ( ( addr & 0x07 ) << 16 ) | g_ramdisk.pezik [ pezik_type ] . latch | ( ( addr >> 8 ) & 0x00ff );
    g_ramdisk.pezik [ pezik_type ] . latch = addr & 0xff00;

    return g_ramdisk.pezik [ pezik_type ] . memory [ pezik_addr ];
}


void ramdisk_pezik_write_byte ( unsigned addr, uint8_t value ) {

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat Pezik ramdisk write.
     *
     * Payload:
     *   [0] = port low byte (0xE8, 0xEC..0xEF)
     *   [1] = value
     *   [2] = port high byte (latch upper)
     *   [3..5] = rezervováno
     */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        uint8_t payload[ 6 ] = {
            (uint8_t)( addr & 0xff ),
            value,
            (uint8_t)( ( addr >> 8 ) & 0xff ),
            0, 0, 0
        };
        hwlog_record ( HWLOG_CHIP_RD, HWLOG_RD_PEZIK_WRITE, payload );
    }
#endif

    unsigned pezik_addr;
    unsigned pezik_type;
    unsigned pezik_bank;

    pezik_type = ( addr & 0x80 ) >> 7;
    pezik_bank = 1 << ( addr & 0x07 );

    if ( !( pezik_bank & g_ramdisk.pezik [ pezik_type ].portmask ) ) return;

    pezik_addr = ( ( addr & 0x07 ) << 16 ) | g_ramdisk.pezik [ pezik_type ] . latch | ( ( addr >> 8 ) & 0x00ff );

    g_ramdisk.pezik [ pezik_type ] . memory [ pezik_addr ] = value;
}
