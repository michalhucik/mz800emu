/* 
 * File:   memext.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 17. července 2018, 20:05
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
#include "mzarch/mzarch_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "baseui/baseui_tools.h"
#include "fs_layer.h"
#include "memext.h"
#include "memory.h"

#include "cfgmain.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#endif

st_MEMEXT g_memext;

void memext_reset ( void ) {
    if ( ( MEMEXT_TEST_TYPE_LUFTNER ) && ( !MEMEXT_TEST_LUFTNER_AUTO_INIT ) ) return;
    int i;
    for ( i = 0; i < MEMEXT_RAW_MAP_SIZE; i++ ) {
        g_memext.map[i] = i;
    };
}


void memext_flash_reload ( void ) {
    if ( !MEMEXT_TEST_CONNECTED_LUFTNER ) return;
    if ( ( g_memext.flash_filepath ) && ( baseui_tools_file_access ( g_memext.flash_filepath, F_OK ) != -1 ) ) {
        FILE *fh;
        FS_LAYER_FOPEN ( fh, g_memext.flash_filepath, FS_LAYER_FMODE_RO );
        if ( fh ) {
            uint32_t readlen = 0;
            FS_LAYER_FREAD ( fh, g_memext.FLASH, sizeof (g_memext.FLASH ), &readlen );
            FS_LAYER_FCLOSE ( fh );
        } else {
            char *filepath_locale = baseui_tools_file_name_locale_from_utf8 ( g_memext.flash_filepath );
            fprintf ( stderr, "%s():%d - Can't open file '%s'\n", __func__, __LINE__, filepath_locale );
            baseui_tools_mem_free ( filepath_locale );
        };
    };
}


static void memext_init_luftner ( void ) {
    int i;
    for ( i = 0; i < MEMEXT_RAW_MAP_SIZE; i++ ) {
        g_memext.map[i] = rand ( ) % 0xff;
    };

    memset ( g_memext.FLASH, 0xff, sizeof (g_memext.FLASH ) );

    if ( MEMEXT_TEST_CONNECTED ) {
        memext_flash_reload ( );
    };
}


void memext_connect ( en_MEMEXT_TYPE type ) {
    g_memext.connection = MEMEXT_CONNECTION_YES;
    g_memext.type = type;
    if ( MEMEXT_TEST_TYPE_LUFTNER ) memext_init_luftner ( );
    memext_reset ( );
    memory_reconnect_ram ( );
}


void memext_disconnect ( void ) {
    g_memext.connection = MEMEXT_CONNECTION_NO;
    memory_reconnect_ram ( );
}


void memext_init ( void ) {

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "MEMEXT" );
    CFGELM *elm;

    /* Default memext připojen (PEHU - viz type default níže). Fresh
     * install dostane funkční rozšíření paměti bez nutnosti zásahu do
     * menu. Existující users s ini si zachovají vlastní stav. */
    elm = cfgmodule_register_new_element ( cmod, "connected", CFGENTYPE_BOOL, MEMEXT_CONNECTION_YES );
    cfgelement_set_handlers ( elm, (void*) &g_memext.connection, (void*) &g_memext.connection );

    elm = cfgmodule_register_new_element ( cmod, "type", CFGENTYPE_KEYWORD, MEMEXT_TYPE_PEHU,
                                           MEMEXT_TYPE_PEHU, "PEHU",
                                           MEMEXT_TYPE_LUFTNER, "LUFTNER",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_memext.type, (void*) &g_memext.type );

    elm = cfgmodule_register_new_element ( cmod, "flash_filepath", CFGENTYPE_TEXT, MEMEXT_DEFAULT_FLASH_FNAME );
    cfgelement_set_pointers ( elm, (void*) &g_memext.flash_filepath, (void*) &g_memext.flash_filepath );

    elm = cfgmodule_register_new_element ( cmod, "luftner_force_init", CFGENTYPE_BOOL, MEMEXT_CONNECTION_YES );
    cfgelement_set_handlers ( elm, (void*) &g_memext.init_luftner, (void*) &g_memext.init_luftner );

    elm = cfgmodule_register_new_element ( cmod, "filling_on_init", CFGENTYPE_KEYWORD, MEMEXT_INIT_MEM_NULL,
                                           MEMEXT_INIT_MEM_NULL, "NULL",
                                           MEMEXT_INIT_MEM_RANDOM, "RANDOM",
                                           MEMEXT_INIT_MEM_SHARP, "SHARP",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_memext.init_mem, (void*) &g_memext.init_mem );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

    srand ( time ( NULL ) );


    if ( g_memext.init_mem == MEMEXT_INIT_MEM_NULL ) {
        memset ( g_memext.RAM, 0x00, sizeof (g_memext.RAM ) );
    } else if ( g_memext.init_mem == MEMEXT_INIT_MEM_SHARP ) {
        uint32_t i;
        uint16_t *addr;

        for ( i = 0; i < ( MEMEXT_RAM_SIZE - 1 ); i += 4 ) {
            addr = ( uint16_t* ) & g_memext.RAM [ i ];
            *addr++ = 0xffff;
            *addr = 0x0000;
        };
    } else {
        int i;
        for ( i = 0; i < MEMEXT_RAM_SIZE; i++ ) {
            g_memext.RAM[i] = rand ( ) % 0xff;
        };
    };

    if ( MEMEXT_TEST_TYPE_LUFTNER ) memext_init_luftner ( );

    memext_reset ( );
}


void memext_map_pwrite ( int addr_point, uint8_t value ) {

    //printf ( "point: 0x%02x, value: %d\n", addr_point, value );

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat memext bank switch.
     *
     * Aktivováno jen pokud je Memext připojen (nepřipojený Memext vůbec
     * nedostane volání z mz800_iorq.c / mz1500_iorq.c, ale guard pro
     * jistotu).
     *
     * Payload (per HW-log_format_CZ.md):
     *   [0] = addr_point (= bus page index 0..15, 4 KB granularita)
     *   [1] = value (raw byte zapsaný do mapovacího portu)
     *   [2] = type (0 = LUFTNER, 1 = PEHU)
     *   [3..5] = rezervováno
     */
    if ( MEMEXT_TEST_CONNECTED && TEST_TRACE_HWLOG_DISPATCH ) {
        uint8_t type_id = MEMEXT_TEST_TYPE_PEHU ? 1 : 0;
        uint8_t payload[ 6 ] = {
            (uint8_t)( addr_point & 0xff ),
            value,
            type_id,
            0, 0, 0
        };
        hwlog_record ( HWLOG_CHIP_MEMEXT, HWLOG_MEMEXT_BANK_SWITCH, payload );
    }
#endif

    if ( MEMEXT_TEST_TYPE_LUFTNER ) {
        g_memext.map[addr_point] = value;
    } else if ( MEMEXT_TEST_TYPE_PEHU ) {
        int ap = addr_point & 0xfe;
        int rawbank = ( value & MEMEXT_PEHU_MASK ) * 2;
        //printf ( "PEHU point: 0x%02x, bank: %d\n", ap, rawbank );
        g_memext.map[ap] = rawbank;
        g_memext.map[( ap + 1 )] = rawbank + 1;
    };

    memory_reconnect_ram ( );
}


void memext_map_get_out_values ( uint8_t *values ) {
    if ( !values ) return;
    int i;
    for ( i = 0; i < MEMEXT_RAW_MAP_SIZE; i++ ) {
        if ( MEMEXT_TEST_TYPE_PEHU ) {
            /* map[] drží 4 KB raw banky páru (2n, 2n+1); OUT E7h bere n. */
            values[i] = (uint8_t) ( ( g_memext.map[( i & 0xfe )] >> 1 ) & MEMEXT_PEHU_MASK );
        } else {
            values[i] = (uint8_t) ( g_memext.map[i] & 0xff );
        };
    };
}


/* ===========================================================================
 *  Požadavek na přemapování z UI vlákna (okno MemExt Map Settings)
 * =========================================================================== */

gint g_memext_map_request_pending = 0;

/**
 * @brief Zámek kopie hodnot požadavku (s_map_request_values).
 *
 * Statický GMutex s nulovou inicializací nepotřebuje g_mutex_init.
 * Drží se jen po dobu kopírování 16 bajtů, nikdy během
 * memext_map_pwrite().
 */
static GMutex s_map_request_mutex;

/**
 * @brief Hodnoty čekajícího požadavku pro address pointy 0..15.
 *
 * Platné, jen když je g_memext_map_request_pending != 0. Chráněno
 * s_map_request_mutex.
 */
static uint8_t s_map_request_values[MEMEXT_RAW_MAP_SIZE];


void memext_map_request ( const uint8_t *values ) {
    if ( !values ) return;
    g_mutex_lock ( &s_map_request_mutex );
    memcpy ( s_map_request_values, values, sizeof ( s_map_request_values ) );
    g_atomic_int_set ( &g_memext_map_request_pending, 1 );
    g_mutex_unlock ( &s_map_request_mutex );
}


bool memext_map_request_process ( void ) {
    uint8_t values[MEMEXT_RAW_MAP_SIZE];

    g_mutex_lock ( &s_map_request_mutex );
    if ( !g_atomic_int_get ( &g_memext_map_request_pending ) ) {
        g_mutex_unlock ( &s_map_request_mutex );
        return false;
    };
    memcpy ( values, s_map_request_values, sizeof ( values ) );
    g_atomic_int_set ( &g_memext_map_request_pending, 0 );
    g_mutex_unlock ( &s_map_request_mutex );

    /* PEHU: jeden zápis na 8 KB pár (sudý point). Zápis lichého pointu
     * by HW zarovnal na sudý a přepsal celý pár hodnotou lichého řádku. */
    int step = MEMEXT_TEST_TYPE_PEHU ? 2 : 1;
    int i;
    for ( i = 0; i < MEMEXT_RAW_MAP_SIZE; i += step ) {
        memext_map_pwrite ( i, values[i] );
    };
    return true;
}


uint8_t* memext_get_ram_read_pointer_by_rawbank ( int rawbank ) {
    if ( rawbank & 0x80 ) {
        return &g_memext.FLASH[( ( rawbank & 0x7f ) * MEMEXT_RAW_BANK_SIZE )];
    };
    return &g_memext.RAM[( rawbank * MEMEXT_RAW_BANK_SIZE )];
}


uint8_t* memext_get_ram_read_pointer_by_addr_point ( int addr_point ) {
    return memext_get_ram_read_pointer_by_rawbank ( g_memext.map[addr_point] );
}


uint8_t* memext_get_ram_write_pointer_by_rawbank ( int rawbank ) {
    if ( rawbank & 0x80 ) {
        return g_memext.WOM;
    };
    return &g_memext.RAM[( rawbank * MEMEXT_RAW_BANK_SIZE )];
}


uint8_t* memext_get_ram_write_pointer_by_addr_point ( int addr_point ) {
    return memext_get_ram_write_pointer_by_rawbank ( g_memext.map[addr_point] );
}

void memext_luftner_set_flash_filepath ( const char *filepath ) {
    if(!filepath) return;
    int len = strlen ( filepath ) + 1;
    g_memext.flash_filepath = (char*) baseui_tools_mem_realloc ( g_memext.flash_filepath, len );
    strncpy ( g_memext.flash_filepath, filepath, len );
    memext_flash_reload ( );
}


int32_t memext_get_ram_offset_from_pointer ( const uint8_t *ptr ) {
    if ( !ptr ) return -1;
    /* Pointer arith mezi různými objekty není dle ISO C definováno - proto
     * srovnáváme přes uintptr_t. Na všech relevantních platformách
     * (MSYS2/Linux/x86_64) to dává očekávaný výsledek. */
    uintptr_t base = (uintptr_t) g_memext.RAM;
    uintptr_t p = (uintptr_t) ptr;
    if ( p < base ) return -1;
    uintptr_t off = p - base;
    if ( off >= MEMEXT_RAM_SIZE ) return -1;
    return (int32_t) off;
}
