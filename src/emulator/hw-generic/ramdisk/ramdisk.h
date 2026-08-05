/* 
 * File:   ramdisk.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 10. srpna 2015, 11:10
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

#ifndef RAMDISK_H
#define RAMDISK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "mzarch/mzcommon_config.h"

/* Default jmeno ramdisk image obsahuje arch suffix - kazda platforma
 * pouziva vlastni soubor (drive sdileny "rd.dat" pro vsechny archy
 * davaly zmateny vysledek pri prepinani targetu). */
/* Default jméno záložního souboru ("rd-<arch>.dat") se od mzhal kroku 7
 * skládá RUNTIME z g_mzhal.arch_name - viz ramdisk_default_filename()
 * v ramdisk.c; compile-time makro z MZ_PLATFORM_SUFFIX zrušeno. */
#define RAMDISK_PEZIK_E8_DEFAULT_FILENAME "pezik_e8.dat"
#define RAMDISK_PEZIK_68_DEFAULT_FILENAME "pezik_68.dat"


#define RAMDISK_IS_READONLY ( 1 << 1 )
#define RAMDISK_IS_IN_FILE ( 1 << 0 )

#define PEZIK_BACKUPED_NO       0
#define PEZIK_BACKUPED_YES      1


    typedef enum en_RAMDISK_TYPE {
        RAMDISK_TYPE_STD = 0,
        RAMDISK_TYPE_SRAM = RAMDISK_IS_IN_FILE,
        RAMDISK_TYPE_ROM = ( RAMDISK_IS_IN_FILE | RAMDISK_IS_READONLY )
    } en_RAMDISK_TYPE;

#define RAMDISK_PEZIK_E8 0x01
#define RAMDISK_PEZIK_68 0x00


    typedef enum en_RAMDISK_BANKMASK {
        RAMDISK_SIZE_64 = 0x00,
        RAMDISK_SIZE_256 = 0x03,
        RAMDISK_SIZE_512 = 0x07,
        RAMDISK_SIZE_1M = 0x0f,
        RAMDISK_SIZE_16M = 0xff
    } en_RAMDISK_BANKMASK;


#define RAMDISK_CONNECTED        1
#define RAMDISK_DISCONNECTED     0


    typedef struct st_RAMDISKPEZIK {
        unsigned connected;
        uint16_t latch;
        unsigned portmask; /* 0x01 - 0xff */
        uint8_t *memory;
        unsigned backuped;
        char *filepath;
    } st_RAMDISKPEZIK;


    typedef struct st_RAMDISKSTD {
        unsigned connected;
        en_RAMDISK_TYPE type;
        en_RAMDISK_BANKMASK size;
        char *filepath;
        uint16_t offset;
        uint8_t bank;
        uint8_t *memory;
    } st_RAMDISKSTD;


    typedef struct st_RAMDISK {
        st_RAMDISKSTD std;
        st_RAMDISKPEZIK pezik [ 2 ];
    } st_RAMDISK;

    extern st_RAMDISK g_ramdisk;

    extern void ramdisc_exit ( void );

    extern void ramdisk_init ( void );

    extern void ramdisk_std_init ( int connect, en_RAMDISK_TYPE type, en_RAMDISK_BANKMASK size, char *filepath );
    extern void ramdisk_std_disconnect ( void );
    extern void ramdisk_std_save ( void );

    extern void ramdisk_pezik_init ( int pezik_type, int connect, int portmask, int backuped, char *filepath );
    extern void ramdisk_pezik_connect ( int pezik_type );
    extern void ramdisk_pezik_disconnect ( int pezik_type );

    extern uint8_t ramdisk_std_read_byte ( unsigned addr );
    extern void ramdisk_std_write_byte ( unsigned addr, uint8_t value );

    extern uint8_t ramdisk_pezik_read_byte ( unsigned addr );
    extern void ramdisk_pezik_write_byte ( unsigned addr, uint8_t value );


#define RAMDISK_TEST_STD_CONNECTED ( g_ramdisk.std.connected )
#define RAMDISK_TEST_PEZIK_E8_CONNECTED ( g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected )
#define RAMDISK_TEST_PEZIK_68_CONNECTED ( g_ramdisk.pezik[RAMDISK_PEZIK_68].connected )
#define RAMDISK_TEST_PEZIK_ANY_CONNECTED ( RAMDISK_TEST_PEZIK_E8_CONNECTED || RAMDISK_TEST_PEZIK_68_CONNECTED )
#define RAMDISK_TEST_NOT_CONNECTED ( !RAMDISK_TEST_STD_CONNECTED && !RAMDISK_TEST_PEZIK_ANY_CONNECTED )

#define RAMDISK_TEST_STD_SIZE(value) ( g_ramdisk.std.size == value )
#define RAMDISK_TEST_STD_TYPE(value) ( g_ramdisk.std.type == value )

#define RAMDISK_DISCONNECT_ALL() ramdisk_pezik_disconnect(RAMDISK_PEZIK_E8); ramdisk_pezik_disconnect(RAMDISK_PEZIK_68); ramdisk_std_disconnect()

#ifdef __cplusplus
}
#endif

#endif /* RAMDISK_H */

