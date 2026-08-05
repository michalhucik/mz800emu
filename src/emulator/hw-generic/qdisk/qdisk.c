/* 
 * File:   qdisk.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 11. února 2016, 18:03
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

#ifdef WINDOWS
#define COMPILE_FOR_EMULATOR
#undef COMPILE_FOR_UNICARD
#undef FS_LAYER_FATFS
#elif defined(LINUX)
#define COMPILE_FOR_EMULATOR
#undef COMPILE_FOR_UNICARD
#undef FS_LAYER_FATFS
#else
#undef COMPILE_FOR_EMULATOR
#define COMPILE_FOR_UNICARD
#define FS_LAYER_FATFS
#endif


//#define DBGLEVEL        ( DBGNON /* | DBGERR | DBGWAR | DBGINF */ )
//#define DBGLEVEL        ( DBGNON | DBGERR | DBGWAR | DBGINF )
#include "debug.h"

#include "../fs_layer.h"

#include "qdisk.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"


#ifdef COMPILE_FOR_UNICARD
#include "hal.h"
#include "monitor.h"
#include "mzint.h"
#else
#include "mzarch/mzcommon_config.h"
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <glib.h>

// Lokalizace
#include "i18n.h"

#include "cfgmain.h"
#include "unicard/unicard.h"
#include "baseui/baseui.h"

/* generic_driver migrace (qdisk-rewrite, Faze 1) */
#include "generic_driver/memory_driver.h"
#include "generic_driver/file_driver.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#endif
#include "baseui/baseui_filechooser.h"

#define ui_qdisk_set_std_path( x ) {g_free( g_qdisk.ui_std_filepath ); g_qdisk.ui_std_filepath = (x) ? g_strdup( x ) : g_strdup( "" );}
#define ui_qdisk_set_virt_path( x ) {g_free( g_qdisk.ui_virt_filepath ); g_qdisk.ui_virt_filepath = (x) ? g_strdup( x ) : g_strdup( "" );}

#endif


st_QDISK g_qdisk;

CFGELM *g_elm_std_fp;
CFGELM *g_elm_virt_fp;
CFGELM *g_elm_wrprt;
#ifdef COMPILE_FOR_EMULATOR
/** CFGELM pro `mz1f11_storage_mode` (TEXT: "cached"|"direct"|"discard"). Faze 3. */
CFGELM *g_elm_storage_mode;
#endif


#ifdef COMPILE_FOR_EMULATOR
/**
 * @brief Parsuj string z INI na hodnotu en_QDISK_STORAGE_MODE.
 *
 * Akceptuje "direct", "discard" a vše ostatní (vč. NULL / prázdného / neznámé
 * hodnoty) je interpretováno jako CACHED (= bezpečný default).
 *
 * @param s Vstupní string (smí být NULL).
 * @return Odpovídající hodnota en_QDISK_STORAGE_MODE.
 */
static en_QDISK_STORAGE_MODE qdisk_parse_storage_mode ( const char *s ) {
    if ( s == NULL ) return QDISK_STORAGE_CACHED;
    if ( 0 == strcmp ( s, "direct" ) )  return QDISK_STORAGE_DIRECT;
    if ( 0 == strcmp ( s, "discard" ) ) return QDISK_STORAGE_DISCARD;
    return QDISK_STORAGE_CACHED;
}

/**
 * @brief Vrať kanonický INI string pro daný en_QDISK_STORAGE_MODE.
 *
 * Inverzní operace k qdisk_parse_storage_mode(). Pro neznámou hodnotu vrací
 * "cached" (bezpečný fallback).
 *
 * @param m Hodnota enum.
 * @return Konstantní string ("cached" / "direct" / "discard"). Nikdy NULL.
 */
static const char *qdisk_storage_mode_to_string ( en_QDISK_STORAGE_MODE m ) {
    switch ( m ) {
        case QDISK_STORAGE_DIRECT:  return "direct";
        case QDISK_STORAGE_DISCARD: return "discard";
        case QDISK_STORAGE_CACHED:
        default:                    return "cached";
    };
}
#endif


uint8_t qdisk_virt_scan_directory ( void ) {

    char *dirpath = cfgelement_get_text_value ( g_elm_virt_fp );

    if ( strlen ( dirpath ) == 0 ) {
        return 0x00;
    };

    FS_LAYER_DIR_HANDLE *dh = NULL;
    FS_LAYER_DIR_ITEM *ditem;
    unsigned mzf_files_count = 0;


    dh = FS_LAYER_DIR_OPEN ( dirpath );
    if ( dh == NULL ) {
#ifdef COMPILE_FOR_EMULATOR
        baseui_error ( "Can't open dir '%s': %s", dirpath, FS_LAYER_GET_ERROR_MESSAGE ( ) );
#endif
        return 0x00;
    };

    while ( ( ditem = FS_LAYER_DIR_READ ( dh ) ) != NULL ) {
        if ( FS_LAYER_DITEM_IS_FILE ( dirpath, ditem ) ) {

            const char *fname = FS_LAYER_DITEM_GET_NAME ( ditem );
            unsigned len = strlen ( fname );

            if ( len > 4 ) {
                if ( 0 == strcasecmp ( &fname [ len - 4 ], ".mzf" ) ) {
                    //printf ( "QDISK VIRT FOUND: %d. [%s]\n", mzf_files_count, fname );
                    mzf_files_count++;
                    if ( mzf_files_count == 127 ) break; /* max pocet souboru (254 bloku) */
                };
            };
        };
    };
    FS_LAYER_DIR_CLOSE ( dh );

    return mzf_files_count;
}


void qdisk_virt_close_mzf ( ) {
    if ( g_qdisk.mzf_fp != NULL ) {
        FS_LAYER_FSYNC ( g_qdisk.mzf_fp );
        FS_LAYER_FCLOSE ( g_qdisk.mzf_fp );
        g_qdisk.mzf_fp = NULL;
    };
}


void qdisk_virt_open_mzf_for_read ( const char *dirpath, const char *filename ) {
    const char *filepath = baseui_tools_build_filepath ( dirpath, filename );
    if ( FS_LAYER_FR_OK != FS_LAYER_FOPEN ( g_qdisk.mzf_fp, (char*) filepath, FS_LAYER_FMODE_RW ) ) {
        DBGPRINTF ( DBGERR, "fopen()\n" );
#ifdef COMPILE_FOR_EMULATOR
        baseui_error ( "Can't create open file '%s': %s", filepath, strerror ( errno ) );
#endif
    };
    baseui_tools_free_filepath ( filepath );
}


void qdisk_virt_prepare_mzf_head ( void ) {

    qdisk_virt_close_mzf ( );

    char *dirpath = cfgelement_get_text_value ( g_elm_virt_fp );

    FS_LAYER_DIR_HANDLE *dh = NULL;
    FS_LAYER_DIR_ITEM *ditem;
    unsigned mzf_files_count = 0;

    dh = FS_LAYER_DIR_OPEN ( dirpath );
    if ( dh == NULL ) {
#ifdef COMPILE_FOR_EMULATOR
        baseui_error ( "Can't open dir '%s': %s", dirpath, FS_LAYER_GET_ERROR_MESSAGE ( ) );
#endif
        return;
    };

    while ( ( ditem = FS_LAYER_DIR_READ ( dh ) ) != NULL ) {
        if ( FS_LAYER_DITEM_IS_FILE ( dirpath, ditem ) ) {

            const char *fname = FS_LAYER_DITEM_GET_NAME ( ditem );
            unsigned len = strlen ( fname );

            if ( 0 == strcasecmp ( &fname [ len - 4 ], ".mzf" ) ) {

                if ( g_qdisk.virt_saved_filename[0] == 0x00 ) {

                    if ( mzf_files_count == g_qdisk.virt_file_num ) {
                        qdisk_virt_open_mzf_for_read ( (const char*) dirpath, FS_LAYER_DITEM_GET_NAME ( ditem ) );
                        break;
                    };

                } else if ( mzf_files_count == ( g_qdisk.virt_files_count - 1 ) ) {
                    /* po tom co jsme zapsali soubor, provadime kontrolu ctenim - zapsany soubor tedy potrebujeme cist jako posledni */
                    qdisk_virt_open_mzf_for_read ( (const char*) dirpath, g_qdisk.virt_saved_filename );
                    break;

                } else if ( mzf_files_count == g_qdisk.virt_file_num ) {
                    qdisk_virt_open_mzf_for_read ( (const char*) dirpath, FS_LAYER_DITEM_GET_NAME ( ditem ) );
                    break;
                };

                mzf_files_count++;
                if ( mzf_files_count == 127 ) break; /* max pocet souboru (254 bloku) */
            };
        };
    };
    FS_LAYER_DIR_CLOSE ( dh );

    if ( g_qdisk.mzf_fp == NULL ) {
#ifdef COMPILE_FOR_EMULATOR
        baseui_error ( "Can't prepare MZF file num '%d'", g_qdisk.virt_file_num );
#endif        
    }
}


void qdisk_virt_prepare_mzf_body ( void ) {

    g_qdisk.virt_mzfbody_size = 0;

    if ( g_qdisk.mzf_fp == NULL ) {
        return;
    };

    /* precteme fsize z mzf */
    if ( FS_LAYER_FR_OK != FS_LAYER_FSEEK ( g_qdisk.mzf_fp, 18 ) ) {
        DBGPRINTF ( DBGERR, "fseek() error\n" );
        return;
    };

    unsigned int readlen;
    uint8_t rbuf;

    FS_LAYER_FREAD ( g_qdisk.mzf_fp, &rbuf, 1, &readlen );
    if ( 1 != readlen ) {
        DBGPRINTF ( DBGERR, "fread() error\n" );
        g_qdisk.virt_mzfbody_size = 0;
        return;
    };

    g_qdisk.virt_mzfbody_size = rbuf;

    FS_LAYER_FREAD ( g_qdisk.mzf_fp, &rbuf, 1, &readlen );
    if ( 1 != readlen ) {
        DBGPRINTF ( DBGERR, "fread() error\n" );
        g_qdisk.virt_mzfbody_size = 0;
        return;
    };

    g_qdisk.virt_mzfbody_size |= rbuf << 8;

    /* nastavime se na pozici mzf body */
    if ( FS_LAYER_FR_OK != FS_LAYER_FSEEK ( g_qdisk.mzf_fp, 0x80 ) ) {
        DBGPRINTF ( DBGERR, "fseek() error\n" );
        g_qdisk.virt_mzfbody_size = 0;
        return;
    };
}


void qdisk_drive_reset ( void ) {
    g_qdisk.image_position = 0;
    g_qdisk.status |= QDSTS_HEAD_HOME;
    if ( g_qdisk.type == QDISK_TYPE_VIRTUAL ) {
        qdisk_virt_close_mzf ( );
        g_qdisk.virt_status = QDISK_VRTSTS_QDHEADER;
        g_qdisk.virt_files_count = 0;
        g_qdisk.virt_file_num = 0;
    };
}


/* Faze 4: staticky helper qdisk_flush_image_to_file() byl prepsan jako verejne
 * API qdisk_sync_drive() (viz konec souboru). Zustavajici volajici (qdisk_close
 * + motor-off vetev v qdisk_write_byte) volaji rovnou qdisk_sync_drive(). */


void qdisk_close ( void ) {
    if ( g_qdisk.connected == QDISK_CONNECTED ) {
        if ( g_qdisk.status & QDSTS_IMG_READY ) {

            if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {
                /* standard image */
#ifdef COMPILE_FOR_EMULATOR
                if ( g_qdisk.handler_valid ) {
                    /* Cesta přes generic_driver. Před close flushne případné
                     * dirty memory buffer zpět do souboru, jinak by se RAM
                     * změny po unmount nebo exitu emu ztratily. Konsolidace
                     * přes veřejné sync API (Faze 4). */
                    ( void ) qdisk_sync_drive ( );
                    generic_driver_close ( &g_qdisk.handler );
                    g_qdisk.handler_valid = 0;
                    g_qdisk.filename[0] = 0x00;
                    /* Faze 2: reset 3-state R/O polí (handler je pryč, fs/effective
                     * nedává smysl udržovat; user_readonly se obnoví z CFGELM
                     * při dalším mount). */
                    g_qdisk.user_readonly = 0;
                    g_qdisk.fs_readonly = 0;
                    g_qdisk.readonly = 0;
                    /* Faze 3: storage_mode se přepočte při příštím mount z
                     * CFGELM. Reset na CACHED (= default) je bezpečný - žádný
                     * další kód by ho neměl číst mezi close a open. */
                    g_qdisk.storage_mode = QDISK_STORAGE_CACHED;
                };
#else
                /* UC FW build - FS_LAYER cesta nad FATFS. Generic_driver
                 * není v UC FW dostupný. */
                if ( g_qdisk.image_fp != NULL ) {
                    if ( FS_LAYER_FR_OK != FS_LAYER_FSYNC ( g_qdisk.image_fp ) ) {
                        DBGPRINTF ( DBGERR, "fsync()\n" );
                    };
                    FS_LAYER_FCLOSE ( g_qdisk.image_fp );
                    g_qdisk.image_fp = NULL;
                };
#endif

            } else {
                /* virtual qdisk */
                qdisk_virt_close_mzf ( );
            };

            g_qdisk.status &= ~QDSTS_IMG_READY;
        };
    };
}

#define QDISK_CREATE_BLOCK_SIZE 100


void qdisk_create_image ( char *filename ) {

    FILE *fp;

    printf ( "\nQuick Disk: create new QD image '%s'\n", filename );

    if ( FS_LAYER_FR_OK != FS_LAYER_FOPEN ( fp, filename, FS_LAYER_FMODE_W ) ) {
        DBGPRINTF ( DBGERR, "fopen()\n" );
#ifdef COMPILE_FOR_EMULATOR
        baseui_error ( "Can't create file '%s': %s", filename, strerror ( errno ) );
#endif
        return;
    };

    uint8_t block [ QDISK_CREATE_BLOCK_SIZE ];
    memset ( &block, 0x00, sizeof ( block ) );

    unsigned i;
    unsigned len;

    for ( i = QDISK_FORMAT_SIZE; i >= QDISK_CREATE_BLOCK_SIZE; i -= QDISK_CREATE_BLOCK_SIZE ) {
        if ( FS_LAYER_FR_OK != FS_LAYER_FWRITE ( fp, &block, sizeof ( block ), &len ) ) {
            DBGPRINTF ( DBGERR, "fwrite() error\n" );
        };
    };

    if ( i ) {
        if ( FS_LAYER_FR_OK != FS_LAYER_FWRITE ( fp, &block, i, &len ) ) {
            DBGPRINTF ( DBGERR, "fwrite() error\n" );
        };
    };

    if ( FS_LAYER_FR_OK != FS_LAYER_FSYNC ( fp ) ) {
        DBGPRINTF ( DBGERR, "fsync()\n" );
#ifdef COMPILE_FOR_EMULATOR
        baseui_error ( "Can't sync file '%s': %s", filename, strerror ( errno ) );
#endif        
    };

    FS_LAYER_FCLOSE ( fp );
}


void qdisk_virt_open_directory ( char *dirpath ) {


    if ( g_qdisk.connected == QDISK_CONNECTED ) {

        g_qdisk.status = QDSTS_NO_DISC;

        qdisk_drive_reset ( );
        qdisk_close ( );

        if ( strlen ( dirpath ) != 0 ) {

            unsigned read_only_flag;
            if ( 0 != cfgelement_get_bool_value ( g_elm_wrprt ) ) {
                read_only_flag = QDSTS_IMG_READONLY;
            } else {
                read_only_flag = 0;
            };

            FS_LAYER_DIR_HANDLE *dh = FS_LAYER_DIR_OPEN ( dirpath );

            if ( dh != NULL ) {
                FS_LAYER_DIR_CLOSE ( dh );
                g_qdisk.status = QDSTS_IMG_READY | QDSTS_HEAD_HOME | read_only_flag;
                cfgelement_set_text_value ( g_elm_virt_fp, dirpath );

            } else {
                cfgelement_set_text_value ( g_elm_virt_fp, "" );
#ifdef COMPILE_FOR_EMULATOR
                baseui_error ( "Can't open dir '%s': %s", dirpath, FS_LAYER_GET_ERROR_MESSAGE ( ) );
#endif
            };
        } else {
#ifdef COMPILE_FOR_EMULATOR
            cfgelement_set_text_value ( g_elm_virt_fp, "" );
#endif
        };

    } else {
        g_qdisk.status = QDSTS_NO_DISC;
    };
}


#ifdef COMPILE_FOR_EMULATOR
/**
 * @brief Interní mount IMAGE / UNICARD souboru s volitelným override storage
 *        mode a user_readonly.
 *
 * Sdílená implementace pro veřejné `qdisk_open_image()` (defaults z CFGELM) a
 * pro Fázi 5 UNICARD lock (force CACHED + RO bez zápisu do INI).
 *
 * Override parametry umožňují vynutit runtime hodnoty (RO / storage mode) bez
 * toho aby se sahalo do CFGELM - user pref tak zůstane neporušený. Po unmount
 * UNICARD modu nás čeká reopen jako IMAGE, kde se hodnoty zase načtou z INI.
 *
 * @param filepath Cesta k .mzq souboru. Prázdný string = vyčistit CFGELM
 *                 `mz1f11_std_filepath` a skončit.
 * @param mode_override `(en_QDISK_STORAGE_MODE)` hodnota = force, `-1` =
 *                 default (parse `g_elm_storage_mode` z INI).
 * @param force_user_readonly `0` nebo `1` = force, `-1` = default (čti z
 *                 `g_elm_wrprt`).
 * @param suppress_cfg_write `1` = neměnit `g_elm_std_fp` (CFGELM user pref).
 *                 Pro UNICARD branch, kde se mountuje Unicard loader image z
 *                 jiné cesty než kterou má user nastavenou v INI; user pref
 *                 (`mz1f11_std_filepath`) musí zůstat netknutý, aby ho byl k
 *                 dispozici po deactivate. `0` = standardní chování (zapíše
 *                 cestu do CFGELM).
 *
 * @note Volat z UI/lifecycle threadu (ne z hot path - sahá na FS).
 * @note Naplní `g_qdisk.filename`, `g_qdisk.storage_mode`, `g_qdisk.user_readonly`,
 *       `g_qdisk.fs_readonly`, `g_qdisk.readonly`, `g_qdisk.handler*` a
 *       `g_qdisk.status`.
 */
static void qdisk_open_image_internal ( const char *filepath,
                                        int mode_override,
                                        int force_user_readonly,
                                        int suppress_cfg_write ) {

    if ( g_qdisk.connected == QDISK_CONNECTED ) {

        g_qdisk.status = QDSTS_NO_DISC;

        qdisk_drive_reset ( );
        qdisk_close ( );

        if ( strlen ( filepath ) != 0 ) {

            unsigned read_only_flag;
            int readonly_effective;

            /* Faze 2: 3-state R/O model.
             *   user_readonly = persistentni CFGELM (user pref) NEBO force
             *   fs_readonly   = runtime W_OK access check
             *   readonly      = user || fs (effective)
             * QDSTS_IMG_READONLY i handler.status R/O bit reflektuji effective. */
            if ( force_user_readonly >= 0 ) {
                g_qdisk.user_readonly = force_user_readonly ? 1 : 0;
            } else {
                g_qdisk.user_readonly = cfgelement_get_bool_value ( g_elm_wrprt ) ? 1 : 0;
            };
            g_qdisk.fs_readonly = ( baseui_tools_file_access ( filepath, W_OK ) == -1 ) ? 1 : 0;
            g_qdisk.readonly = ( g_qdisk.user_readonly || g_qdisk.fs_readonly ) ? 1 : 0;
            readonly_effective = g_qdisk.readonly;
            read_only_flag = readonly_effective ? QDSTS_IMG_READONLY : 0;

            /* Naplnime filename ULOZIT pred open - pro pripadnou diagnostiku
             * pri selhani a pro pozdejsi flush. */
            size_t maxlen = sizeof ( g_qdisk.filename ) - 1;
            strncpy ( g_qdisk.filename, filepath, maxlen );
            g_qdisk.filename[maxlen] = 0x00;

            /* Faze 3 / Faze 5: volba storage mode. Override (>= 0) ma prednost
             * pred CFGELM hodnotou - pouziva se z UNICARD branch (force CACHED).
             * Jinak parse z INI. DIRECT = file_driver (write-through na FILE*);
             * CACHED / DISCARD = oba memory_driver, rozdil je az v
             * qdisk_sync_drive() (DISCARD se nikdy nesynchronizuje zpet). */
            if ( mode_override >= 0 ) {
                g_qdisk.storage_mode = (en_QDISK_STORAGE_MODE) mode_override;
            } else {
                g_qdisk.storage_mode = qdisk_parse_storage_mode ( cfgelement_get_text_value ( g_elm_storage_mode ) );
            };

            st_DRIVER *driver;
            st_HANDLER *h;
            if ( g_qdisk.storage_mode == QDISK_STORAGE_DIRECT ) {
                driver = &g_file_driver;
                en_FILE_DRIVER_OPEN_MODE om = readonly_effective ? FILE_DRIVER_OPMODE_RO : FILE_DRIVER_OPMODE_RW;
                h = generic_driver_open_file ( &g_qdisk.handler, driver, (char*) filepath, om );
            } else {
                driver = &g_memory_driver_static;
                h = generic_driver_open_memory_from_file ( &g_qdisk.handler, driver, (char*) filepath );
            };

            if ( ( h == NULL ) || ( driver->err != GENERIC_DRIVER_ERROR_NONE ) || ( g_qdisk.handler.err != HANDLER_ERROR_NONE ) ) {
                fprintf ( stderr, "%s(%d): failed to open QD image '%s': %s\n",
                          __FILE__, __LINE__, filepath,
                          generic_driver_error_message ( &g_qdisk.handler, driver ) );
                baseui_error ( "Can't open file '%s': %s", filepath,
                               generic_driver_error_message ( &g_qdisk.handler, driver ) );
                g_qdisk.filename[0] = 0x00;
                if ( ! suppress_cfg_write ) {
                    cfgelement_set_text_value ( g_elm_std_fp, "" );
                };
            } else {
                g_qdisk.handler_valid = 1;
                generic_driver_set_handler_readonly_status ( &g_qdisk.handler, readonly_effective );
                g_qdisk.status = QDSTS_IMG_READY | QDSTS_HEAD_HOME | read_only_flag;
                if ( ! suppress_cfg_write ) {
                    cfgelement_set_text_value ( g_elm_std_fp, (char*) filepath );
                };
            };
        } else {
            if ( ! suppress_cfg_write ) {
                cfgelement_set_text_value ( g_elm_std_fp, "" );
            };
        };

    } else {
        g_qdisk.status = QDSTS_NO_DISC;
    };
}


/**
 * @brief Mount QD image z RAM bufferu (žádný file backing) jako CACHED + R/O.
 *
 * Slouží jako alternativa k `qdisk_open_image_internal` pro situace, kdy
 * zdrojem QD image není soubor na disku, ale data drížená v paměti
 * emulátoru. Aktuálně se používá pro uc3 SDERR fallback: pokud při
 * UNICARD QD mountu chybí `{SD}/unicard/mzfloader.mzq`, vyrobíme MZQ
 * z embedded SDERR MZF a namountujeme ho odsud.
 *
 * Vlastnosti mountu (fixed, žádné CFGELM lookup):
 *   - storage_mode = `QDISK_STORAGE_CACHED` (writes končí v RAM kopii)
 *   - user_readonly = 1, fs_readonly = 0, readonly (effective) = 1
 *   - suppress_cfg_write = vždy: nemodifikujeme `g_elm_std_fp` (user
 *     pref pro IMAGE mode musí zůstat netknutý)
 *   - `g_qdisk.filename` se naplní `virtual_filename` (= informativní
 *     pseudo-cesta pro diagnostiku, není to skutečný soubor)
 *
 * @param data              ukazatel na MZQ data v paměti
 * @param size              velikost MZQ (= 92 + body_size)
 * @param virtual_filename  pseudo-cesta zapsaná do `g_qdisk.filename`
 *                          (např. `"[unicard:sderr-fallback.mzq]"`); ne NULL
 *
 * @note Volat z UI/lifecycle threadu (ne z hot path).
 * @note Pokud `g_qdisk.connected != QDISK_CONNECTED`, nastaví jen
 *       `g_qdisk.status = QDSTS_NO_DISC` a skončí.
 */
static void qdisk_open_image_from_buffer_internal ( const uint8_t *data,
                                                    uint32_t size,
                                                    const char *virtual_filename ) {

    if ( g_qdisk.connected != QDISK_CONNECTED ) {
        g_qdisk.status = QDSTS_NO_DISC;
        return;
    };

    g_qdisk.status = QDSTS_NO_DISC;

    qdisk_drive_reset ( );
    qdisk_close ( );

    if ( ( data == NULL ) || ( size == 0 ) ) return;

    /* Fixed 3-state R/O: memory image nemá file backing, fs_readonly = 0;
     * volající si R/O vynucuje přes user_readonly = 1 (= CFGELM-bypass). */
    g_qdisk.user_readonly = 1;
    g_qdisk.fs_readonly = 0;
    g_qdisk.readonly = 1;
    unsigned read_only_flag = QDSTS_IMG_READONLY;

    /* Pseudo-cesta jen pro diagnostiku - není to skutečný soubor. */
    size_t maxlen = sizeof ( g_qdisk.filename ) - 1;
    strncpy ( g_qdisk.filename, virtual_filename, maxlen );
    g_qdisk.filename[maxlen] = 0x00;

    /* Vždy CACHED (memory_driver) - DIRECT (= file backing) by neměl
     * smysl, žádný soubor neexistuje. */
    g_qdisk.storage_mode = QDISK_STORAGE_CACHED;

    st_DRIVER *driver = &g_memory_driver_static;
    st_HANDLER *h = generic_driver_open_memory ( &g_qdisk.handler, driver, size );

    if ( ( h == NULL ) || ( driver->err != GENERIC_DRIVER_ERROR_NONE ) || ( g_qdisk.handler.err != HANDLER_ERROR_NONE ) ) {
        fprintf ( stderr, "%s(%d): failed to open in-memory QD image '%s' (%u B): %s\n",
                  __FILE__, __LINE__, virtual_filename, size,
                  generic_driver_error_message ( &g_qdisk.handler, driver ) );
        baseui_error ( "Can't open in-memory image '%s': %s", virtual_filename,
                       generic_driver_error_message ( &g_qdisk.handler, driver ) );
        g_qdisk.filename[0] = 0x00;
        return;
    };

    /* Naplníme alokovaný memory buffer dodanými daty. memspec.ptr platí
     * po úspěšném open_memory (= viz generic_driver_open_memory v
     * libs/generic_driver/generic_driver.c). */
    memcpy ( g_qdisk.handler.spec.memspec.ptr, data, size );

    g_qdisk.handler_valid = 1;
    generic_driver_set_handler_readonly_status ( &g_qdisk.handler, 1 );
    g_qdisk.status = QDSTS_IMG_READY | QDSTS_HEAD_HOME | read_only_flag;
}
#endif /* COMPILE_FOR_EMULATOR */


void qdisk_open_image ( char *filepath ) {
#ifdef COMPILE_FOR_EMULATOR
    /* Veřejné API - defaultní mount, hodnoty z CFGELM. UNICARD lock branch
     * (Faze 5) volá qdisk_open_image_internal() s explicitními override
     * hodnotami, ne přes tuto funkci. */
    qdisk_open_image_internal ( filepath, -1, -1, 0 );
#else
    /* Unicard FW build - generic_driver neni dostupny, ponecháváme původní
     * FS_LAYER cestu beze změny (3-state R/O pole neexistují, storage mode
     * není zaveden). */
    if ( g_qdisk.connected == QDISK_CONNECTED ) {

        g_qdisk.status = QDSTS_NO_DISC;

        qdisk_drive_reset ( );
        qdisk_close ( );

        if ( strlen ( filepath ) != 0 ) {

            unsigned read_only_flag;
            int readonly_effective;

            if ( ( 0 != cfgelement_get_bool_value ( g_elm_wrprt ) ) || ( baseui_tools_file_access ( filepath, W_OK ) == -1 ) ) {
                read_only_flag = QDSTS_IMG_READONLY;
                readonly_effective = 1;
            } else {
                read_only_flag = 0;
                readonly_effective = 0;
            };

            char *open_file_mode = readonly_effective ? FS_LAYER_FMODE_RO : FS_LAYER_FMODE_RW;
            if ( FS_LAYER_FR_OK == FS_LAYER_FOPEN ( g_qdisk.image_fp, filepath, open_file_mode ) ) {
                g_qdisk.status = QDSTS_IMG_READY | QDSTS_HEAD_HOME | read_only_flag;
            };
        };

    } else {
        g_qdisk.status = QDSTS_NO_DISC;
    };
#endif
}


void qdisk_open ( void ) {
    char *filepath;
    if ( g_qdisk.type == QDISK_TYPE_IMAGE ) {
        filepath = cfgelement_get_text_value ( g_elm_std_fp );
        qdisk_open_image ( filepath );
#ifdef COMPILE_FOR_EMULATOR
        filepath = cfgelement_get_text_value ( g_elm_std_fp );
        ui_qdisk_set_std_path ( filepath );
    } else if ( g_qdisk.type == QDISK_TYPE_UNICARD ) {
        if ( UNICARD_TEST_IS_CONNECTED ) {
            filepath = unicard_get_mzfloader_image_filepath ( );

            /* uc3 SDERR fallback: pokud {SD}/unicard/mzfloader.mzq
             * neexistuje, sestavíme MZQ image v RAM z embedded SDERR
             * MZF a namountujeme ho jako CACHED + R/O. Velikost
             * bufferu = 92 (MZQ wrapper) + 64 KB (max MZF body),
             * SDERR-800/1500 jsou ~1 KB takže to s rezervou pokryje.
             * Pro uc1 nebo když fallback není aplikovatelný vrací
             * unicard_build_fallback_mzq 0 a padá se na klasickou
             * file mount cestu (kde existující chyba zůstává). */
            uint8_t fallback_buff[ 92 + 0x10000 ];
            uint32_t fallback_size = 0;
            if ( !g_file_test ( filepath, G_FILE_TEST_IS_REGULAR ) ) {
                fallback_size = unicard_build_fallback_mzq ( fallback_buff,
                                                             sizeof ( fallback_buff ) );
            };

            if ( fallback_size > 0 ) {
                fprintf ( stderr,
                          "WARNING: Unicard QD bootstrap: '%s' not found, "
                          "mounting embedded SDERR fallback image (R/O, in-memory, %u B)\n",
                          filepath, fallback_size );
                qdisk_open_image_from_buffer_internal ( fallback_buff,
                                                        fallback_size,
                                                        "[unicard:sderr-fallback.mzq]" );
            } else {
                /* Faze 5 (qdisk-rewrite): UNICARD lock.
                 * Vynutíme R/O + CACHED storage mode bez ohledu na hodnoty v INI:
                 *   - Unicard loader image spravuje Unicard FW, není určen k zápisu
                 *     z emulovaného systému (mz1f11_write_protected musí zůstat
                 *     intaktní = user pref pro IMAGE mode se nepřepisuje).
                 *   - storage_mode = CACHED znamená, že případné writes končí jen
                 *     v RAM (= žádný flush do souboru loaderu); v kombinaci s
                 *     readonly=1 navíc generic_driver writes rovnou zamítne.
                 * suppress_cfg_write=1: cesta k Unicard loader image se nesmí
                 * propisovat do mz1f11_std_filepath (= user image pref). */
                qdisk_open_image_internal ( filepath,
                                            QDISK_STORAGE_CACHED,
                                            1 /* force user_readonly */,
                                            1 /* suppress CFGELM write */ );
            };
            baseui_tools_mem_free ( filepath );
        };
#endif
    } else {
        /* TODO: */
        filepath = cfgelement_get_text_value ( g_elm_virt_fp );
        qdisk_virt_open_directory ( filepath );
#ifdef COMPILE_FOR_EMULATOR
        filepath = cfgelement_get_text_value ( g_elm_virt_fp );
        ui_qdisk_set_virt_path ( filepath );
#endif
    };
    g_qdisk.virt_saved_filename[0] = 0x00;
}


static void qdisk_ui_mount_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        return;
    };

    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        return;
    };

    if ( g_qdisk.type == QDISK_TYPE_IMAGE ) {
        char *filename = fch->selected_filePathName;
        if ( filename ) 
        {
            fch->selected_filePathName = NULL;
            if ( baseui_tools_file_access ( filename, F_OK ) == -1 ) {
                /* soubor neexistuje - vyrobime novy */
                //printf ( "create new: '%s'\n", filename );
                qdisk_create_image ( filename );
            };

            qdisk_open_image ( filename );
            ui_qdisk_set_std_path ( cfgelement_get_text_value ( g_elm_std_fp ) );
        };
    } else {
        /* virtual QDISK */
        char *dirpath = fch->selected_path;
        if ( dirpath )
        {
            fch->selected_path = NULL;
            qdisk_virt_open_directory ( dirpath );
            ui_qdisk_set_virt_path ( cfgelement_get_text_value ( g_elm_virt_fp ) );
        };
    };

    baseui_filechooser_destroy(fch);
}


void qdisk_ui_mount ( void ) {

    baseui_fchooser_t *fch = NULL;

    if ( g_qdisk.type == QDISK_TYPE_IMAGE ) {
        fch = baseui_filechooser_open_rw_file(_("Select existing .MZQ file or create new QD image"), ".mzq", NULL, NULL, cfgelement_get_text_value ( g_elm_std_fp ), qdisk_ui_mount_cb, NULL);
    } else {
        fch = baseui_filechooser_open_dir(_("Select directory to mount as virtual Quick Disk"), NULL, NULL, cfgelement_get_text_value ( g_elm_virt_fp ), qdisk_ui_mount_cb, NULL);
    };

    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };

}


void qdisk_umount ( void ) {
    qdisk_close ( );
    if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {
        cfgelement_set_text_value ( g_elm_std_fp, "" );
        ui_qdisk_set_std_path ( "" );
    } else {
        cfgelement_set_text_value ( g_elm_virt_fp, "" );
        ui_qdisk_set_virt_path ( "" );
    };
}


void qdisk_set_write_protected ( int value ) {
    int new_user_ro = value ? 1 : 0;

    /* Persist user pref do CFGELM - jinak by se po restartu emu ztratila
     * (cfgroot_save() v cfgmain_exit() uklada jen CFGELM hodnoty, ne pole
     * g_qdisk). */
    cfgelement_set_bool_value ( g_elm_wrprt, new_user_ro );

#ifdef COMPILE_FOR_EMULATOR
    /* Faze 2: re-eval effective readonly bez remountu.
     *
     * Pozor: pri user_ro=0 muze byt fs_readonly stale 1 (soubor na disku
     * je write-protected) - effective readonly zustane 1 a user to vidi
     * v UI jako [FS R/O] label. */
    g_qdisk.user_readonly = new_user_ro;
    g_qdisk.readonly = ( g_qdisk.user_readonly || g_qdisk.fs_readonly ) ? 1 : 0;

    /* Propagace effective stavu do status flagu a do handleru. */
    if ( g_qdisk.readonly ) {
        g_qdisk.status |= QDSTS_IMG_READONLY;
    } else {
        g_qdisk.status &= ~QDSTS_IMG_READONLY;
    };
    if ( g_qdisk.handler_valid ) {
        generic_driver_set_handler_readonly_status ( &g_qdisk.handler, g_qdisk.readonly );
    };
#else
    /* Unicard FW build: 3-state pole nejsou - fallback na puvodni full
     * remount, aby fopen() prebral novy open_mode. */
    if ( g_qdisk.status & QDSTS_IMG_READY ) {
        qdisk_close ( );
        qdisk_open ( );
    };
#endif
}


/* Pri ukonceni je potreba uzavrit vsechny otevrene soubory */
void qdisk_exit ( void ) {
    qdisk_close ( );
#ifdef COMPILE_FOR_EMULATOR
    if ( g_qdisk.ui_std_filepath != NULL ) {
        g_free ( g_qdisk.ui_std_filepath );
        g_qdisk.ui_std_filepath = NULL;
    };

    if ( g_qdisk.ui_virt_filepath != NULL ) {
        g_free ( g_qdisk.ui_virt_filepath );
        g_qdisk.ui_virt_filepath = NULL;
    };
    #endif
}


void qdisk_init ( void ) {
    memset ( &g_qdisk, 0x00, sizeof ( st_QDISK ) );
    g_qdisk.channel[0].name = 'A';
    g_qdisk.channel[1].name = 'B';
    qdisk_drive_reset ( );
    g_qdisk.mzf_fp = NULL;
#ifdef COMPILE_FOR_EMULATOR
    g_qdisk.ui_std_filepath = g_new0 ( char, 1 );
    g_qdisk.ui_virt_filepath = g_new0 ( char, 1 );
#endif

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "QDISK" );

    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "mz1f11_connected", CFGENTYPE_BOOL, QDISK_CONNECTED );
    cfgelement_set_handlers ( elm, (void*) &g_qdisk.connected, (void*) &g_qdisk.connected );

    /* Default QD = Unicard Boot Loader na všech platformách. Po
     * implementaci dual-FW + SC3SROM mzfloader + V2.11b managery
     * per-mzarch je Unicard funkční na MZ-800, MZ-700 i MZ-1500
     * hned po fresh installu (= UNICARD modul má default "connected"
     * pro všechny). User s ini si zachová vlastní volbu. */
    int default_qdisk_type = QDISK_TYPE_UNICARD;
    elm = cfgmodule_register_new_element ( cmod, "mz1f11_type", CFGENTYPE_KEYWORD, default_qdisk_type,
                                           QDISK_TYPE_IMAGE, "STANDARD",
                                           QDISK_TYPE_VIRTUAL, "VIRTUAL",
                                           QDISK_TYPE_UNICARD, "UNICARD",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_qdisk.type, (void*) &g_qdisk.type );

    g_elm_std_fp = cfgmodule_register_new_element ( cmod, "mz1f11_std_filepath", CFGENTYPE_TEXT, "" );
    g_elm_virt_fp = cfgmodule_register_new_element ( cmod, "mz1f11_virtual_filepath", CFGENTYPE_TEXT, "" );
    g_elm_wrprt = cfgmodule_register_new_element ( cmod, "mz1f11_write_protected", CFGENTYPE_BOOL, 0 );
#ifdef COMPILE_FOR_EMULATOR
    /* Fáze 3: persistentní volba storage mode pro IMAGE / UNICARD. UC FW
     * build storage mode nezná (FS_LAYER cesta je vždy ekvivalent DIRECT). */
    g_elm_storage_mode = cfgmodule_register_new_element ( cmod, "mz1f11_storage_mode", CFGENTYPE_TEXT, "cached" );
#endif

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

#ifdef COMPILE_FOR_EMULATOR
    /* Fáze 2: init 3-state R/O polí. user_readonly se inicializuje z CFGELM
     * (= persistentní user pref). fs_readonly a effective readonly se
     * dopočtou až v qdisk_open_image() po naplnění filename. */
    g_qdisk.user_readonly = cfgelement_get_bool_value ( g_elm_wrprt ) ? 1 : 0;
    g_qdisk.fs_readonly = 0;
    g_qdisk.readonly = 0;

    /* Fáze 3: init storage_mode z perzistentního INI klíče. Skutečnou volbu
     * driveru provede qdisk_open_image() (čte g_elm_storage_mode znovu pro
     * případ runtime switche). */
    g_qdisk.storage_mode = qdisk_parse_storage_mode ( cfgelement_get_text_value ( g_elm_storage_mode ) );
#endif

    qdisk_open ( );
}


uint8_t qdisk_read_byte_from_drive ( void ) {
    unsigned int readlen;
    uint8_t retval;

    /* pokud neni pripojen img - jdeme pryc */
    if ( 0 == ( g_qdisk.status & QDSTS_IMG_READY ) ) {
        return 0xff;
    };

    /* pokud nebezi motor - jdeme pryc */
    if ( ( g_qdisk.channel[ QDSIO_CHANNEL_B ].Wreg[ QDSIO_REGADDR_5 ] & 0x80 ) == 0x00 ) {
        return 0xff;
    };


    if ( QDISK_IMAGE_MAX_SIZE <= g_qdisk.image_position ) {
        if ( QDISK_IMAGE_MAX_SIZE == g_qdisk.image_position ) g_qdisk.image_position++;
        return 0xff;
    };


    if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {

#ifdef COMPILE_FOR_EMULATOR
        retval = 0xff;
        if ( g_qdisk.handler_valid ) {
            if ( EXIT_SUCCESS != generic_driver_read ( &g_qdisk.handler, g_qdisk.image_position, &retval, 1 ) ) {
                DBGPRINTF ( DBGERR, "generic_driver_read() error: %s\n",
                            generic_driver_error_message ( &g_qdisk.handler, g_qdisk.handler.driver ) );
                retval = 0xff;
            };
        };
        ( void ) readlen; /* unused v nove ceste */
#else
        FS_LAYER_FREAD ( g_qdisk.image_fp, &retval, 1, &readlen );

        if ( 1 != readlen ) {
            DBGPRINTF ( DBGERR, "fread() error\n" );
        };
#endif

        g_qdisk.image_position++;

    } else {

        retval = 0xff;

        static char CRC[] = "CRC";
        static char *crc_ptr = CRC;

        if ( g_qdisk.image_position < 3 ) {
            printf ( "Err: read in sync area? (%d)\n", g_qdisk.image_position );

        } else if ( g_qdisk.image_position == 3 ) {
            /* konec synchronizacni znacky */
            retval = 0xa5;
            g_qdisk.image_position++;

        } else if ( g_qdisk.virt_status == QDISK_VRTSTS_QDHEADER ) {

            if ( g_qdisk.image_position == 4 ) {
                /* probiha normalni cteni - razeni souboru bude takove, jak jsou ulozeny na disku (saved_filename nas nezajima) */
                g_qdisk.virt_saved_filename[0] = 0x00;
                g_qdisk.virt_files_count = qdisk_virt_scan_directory ( );
                //printf ( "QDISK VIRT: scan ( 0x%02x)\n", g_qdisk.virt_files_count );
                retval = g_qdisk.virt_files_count << 1;
                g_qdisk.image_position++;
                crc_ptr = CRC;

            } else if ( g_qdisk.image_position <= 7 ) {
                g_qdisk.image_position++;
                retval = *crc_ptr++;
            };
            /* disk header je precten */

        } else if ( g_qdisk.virt_status == QDISK_VRTSTS_FREE_FILEAREA ) {
            if ( g_qdisk.image_position & 1 ) {
                retval = 0xaa;
                g_qdisk.image_position = 4;
            } else {
                retval = 0x55;
                g_qdisk.image_position = 5;
            };

        } else if ( g_qdisk.virt_status == QDISK_VRTSTS_MZFHEAD ) {

            if ( 4 == g_qdisk.image_position ) {
                /* mzf header sign */
                retval = 0x00;
                g_qdisk.image_position++;

            } else if ( 6 >= g_qdisk.image_position ) {
                /* mzf header size 0x0040 */
                retval = 0x40 * ( 6 - g_qdisk.image_position );
                g_qdisk.image_position++;

            } else if ( 24 >= g_qdisk.image_position ) {
                /* mzf ftype, fname[16], 0x0d */
                FS_LAYER_FREAD ( g_qdisk.mzf_fp, &retval, 1, &readlen );

                if ( 1 != readlen ) {
                    DBGPRINTF ( DBGERR, "fread() error\n" );
                };

                g_qdisk.image_position++;

            } else if ( 26 >= g_qdisk.image_position ) {
                /* 2 bajty unused */
                retval = 0x00;
                g_qdisk.image_position++;

            } else if ( 70 >= g_qdisk.image_position ) {
                /* size, start, exec, comment[38] */
                FS_LAYER_FREAD ( g_qdisk.mzf_fp, &retval, 1, &readlen );

                if ( 1 != readlen ) {
                    DBGPRINTF ( DBGERR, "fread() error\n" );
                };
                g_qdisk.image_position++;
                crc_ptr = CRC;

            } else if ( 73 >= g_qdisk.image_position ) {
                g_qdisk.image_position++;
                retval = *crc_ptr++;
            };
            /* hlavicka je prectena */

        } else if ( g_qdisk.virt_status == QDISK_VRTSTS_MZFBODY ) {

            if ( 4 == g_qdisk.image_position ) {
                /* mzf body sign */
                retval = 0x05;
                g_qdisk.image_position++;

            } else if ( 5 == g_qdisk.image_position ) {
                /* mzf body size + 10 */
                retval = g_qdisk.virt_mzfbody_size & 0xff;
                g_qdisk.image_position++;

            } else if ( 6 == g_qdisk.image_position ) {
                /* mzf body size + 10 */
                retval = ( g_qdisk.virt_mzfbody_size >> 8 ) & 0xff;
                g_qdisk.image_position++;

            } else if ( 7 == g_qdisk.image_position ) {

                FS_LAYER_FREAD ( g_qdisk.mzf_fp, &retval, 1, &readlen );

                if ( 1 != readlen ) {
                    DBGPRINTF ( DBGERR, "fread() error\n" );
                };

                g_qdisk.virt_mzfbody_size--;
                if ( 0 == g_qdisk.virt_mzfbody_size ) {
                    g_qdisk.image_position++;
                    qdisk_virt_close_mzf ( );
                    crc_ptr = CRC;
                };

            } else if ( 10 >= g_qdisk.image_position ) {

                retval = *crc_ptr++;
                g_qdisk.image_position++;
            };
            /* telo precteno */
        };
    };

    //printf ( "\tread: ( %d, %d) = 0x%02x\n", g_qdisk.virt_status, g_qdisk.image_position - 1, retval );
    return retval;
}


int qdisk_test_disk_is_writeable ( void ) {

    /* pokud neni pripojen img - jdeme pryc */
    if ( 0 == ( g_qdisk.status & QDSTS_IMG_READY ) ) {
        return 0;
    };

    /* pokud je img write protected - jdeme pryc */
    if ( g_qdisk.status & QDSTS_IMG_READONLY ) {
        return 0;
    };

    /* pokud nebezi motor - jdeme pryc */
    if ( ( g_qdisk.channel[ QDSIO_CHANNEL_B ].Wreg[ QDSIO_REGADDR_5 ] & 0x80 ) == 0x00 ) {
        return 0;
    };

    /* pokud neni nastaven output mode - jdeme pryc */
    if ( ( g_qdisk.channel[ QDSIO_CHANNEL_A ].Wreg[ QDSIO_REGADDR_5 ] & 0x08 ) == 0x00 ) {
        return 0;
    };

    return 1;
}


void qdisk_write_byte_into_drive ( uint8_t value ) {
    unsigned len;

    if ( 0 == qdisk_test_disk_is_writeable ( ) ) return;

    if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {

        if ( QDISK_IMAGE_MAX_SIZE <= g_qdisk.image_position ) {
            if ( QDISK_IMAGE_MAX_SIZE == g_qdisk.image_position ) g_qdisk.image_position++;
            return;
        };

#ifdef COMPILE_FOR_EMULATOR
        if ( g_qdisk.handler_valid ) {
            if ( EXIT_SUCCESS != generic_driver_write ( &g_qdisk.handler, g_qdisk.image_position, &value, 1 ) ) {
                DBGPRINTF ( DBGERR, "generic_driver_write() error: %s\n",
                            generic_driver_error_message ( &g_qdisk.handler, g_qdisk.handler.driver ) );
                baseui_error ( "generic_driver_write error: %s",
                               generic_driver_error_message ( &g_qdisk.handler, g_qdisk.handler.driver ) );
            };
        };
        ( void ) len; /* unused v nove ceste */
#else
        if ( FS_LAYER_FR_OK != FS_LAYER_FWRITE ( g_qdisk.image_fp, &value, 1, &len ) ) {
            DBGPRINTF ( DBGERR, "fwrite() error\n" );
        };
#endif

        g_qdisk.image_position++;

    } else {

        //printf ( "\twrite: ( %d, %d) = 0x%02x\n", g_qdisk.virt_status, g_qdisk.image_position, value );

        if ( g_qdisk.virt_status == QDISK_VRTSTS_QDHEADER ) {
            if ( g_qdisk.image_position == 4 ) {
                if ( value == 0 ) {
                    //printf ( "Formating...\n" );
                    g_qdisk.virt_status = QDISK_VRTSTS_FORMATING;
                } else {
                    /* bude probihat kontrolni cteni - saved_filename musi byt precten vzdy jako posledni! (nenulujeme jej) */
                    //printf ( "Write file done...\n" );
                    g_qdisk.virt_files_count = qdisk_virt_scan_directory ( );
                    //printf ( "QDISK VIRT: scan ( 0x%02x)\n", g_qdisk.virt_files_count );
                };
            };
            g_qdisk.image_position++;

        } else if ( g_qdisk.virt_status == QDISK_VRTSTS_WR_MZFHEAD ) {

            if ( ( ( g_qdisk.image_position >= 7 ) && ( g_qdisk.image_position < 25 ) ) || ( ( g_qdisk.image_position >= 27 ) && ( g_qdisk.image_position < 71 ) ) ) {

                if ( FS_LAYER_FR_OK != FS_LAYER_FWRITE ( g_qdisk.mzf_fp, &value, 1, &len ) ) {
                    DBGPRINTF ( DBGERR, "fwrite() error\n" );
                };

            };
            g_qdisk.image_position++;

        } else if ( g_qdisk.virt_status == QDISK_VRTSTS_WR_MZFBODY ) {

            if ( g_qdisk.image_position <= 6 ) {

                if ( g_qdisk.image_position == 5 ) {
                    g_qdisk.virt_mzfbody_size = value;

                } else if ( g_qdisk.image_position == 6 ) {
                    g_qdisk.virt_mzfbody_size |= value << 8;
                };

                g_qdisk.image_position++;

            } else {

                g_qdisk.virt_mzfbody_size--;

                if ( FS_LAYER_FR_OK != FS_LAYER_FWRITE ( g_qdisk.mzf_fp, &value, 1, &len ) ) {
                    DBGPRINTF ( DBGERR, "fwrite() error\n" );
                };

                if ( g_qdisk.virt_mzfbody_size == 0 ) {

                    if ( FS_LAYER_FR_OK != FS_LAYER_FSEEK ( g_qdisk.mzf_fp, 1 ) ) {
                        DBGPRINTF ( DBGERR, "fseek() error\n" );
                    };

                    unsigned int readlen;
                    uint8_t mzf_fname [ QDISK_MZF_FILENAME_LENGTH ];

                    FS_LAYER_FREAD ( g_qdisk.mzf_fp, mzf_fname, sizeof ( mzf_fname ), &readlen );
                    if ( sizeof ( mzf_fname ) != readlen ) {
                        DBGPRINTF ( DBGERR, "fread() error\n" );
                    };

                    qdisk_virt_close_mzf ( );



                    size_t i;
                    for ( i = 0; i < sizeof ( g_qdisk.virt_saved_filename ) - 4; i++ ) {
                        if ( mzf_fname [ i ] < 0x20 ) break;
                        g_qdisk.virt_saved_filename [ i ] = sharpmz_cnv_from ( mzf_fname [ i ] );
                    };
                    g_qdisk.virt_saved_filename [ i ] = 0x00;
                    strcat ( g_qdisk.virt_saved_filename, ".mzf" );

                    char *dirpath = cfgelement_get_text_value ( g_elm_virt_fp );
                    if ( 0 != baseui_tools_file_rename ( dirpath, QDISK_VIRT_TEMP_FNAME, g_qdisk.virt_saved_filename ) ) {
                        DBGPRINTF ( DBGERR, "rename() error\n" );
                    };

                    g_qdisk.virt_status = QDISK_VRTSTS_FREE_FILEAREA;
                };
            }
        };

    };
}


uint8_t qdisk_read_byte ( en_QDSIO_ADDR SIO_addr ) {


    uint8_t retval = 0x00;

    st_QDSIO_CHANNEL *channel = &g_qdisk.channel[ SIO_addr & 0x01 ];

    switch ( SIO_addr ) {

        case QDSIO_ADDR_CTRL_A:

            /* hunt phase */
            if ( ( channel->Wreg [ QDSIO_REGADDR_3 ] & 0x11 ) == 0x11 ) {

                channel->Rreg [ QDSIO_REGADDR_0 ] |= 0x10;
                g_qdisk.status &= ~QDSTS_IMG_SYNC;

                if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {

                    uint8_t sync1, sync2;

                    sync1 = qdisk_read_byte_from_drive ( );

                    int i;
                    for ( i = 0; i < 8; i++ ) {

                        sync2 = qdisk_read_byte_from_drive ( );

                        if ( ( sync1 == channel->Wreg [ QDSIO_REGADDR_6 ] ) && ( sync2 == channel->Wreg [ QDSIO_REGADDR_7 ] ) ) {
                            channel->Rreg [ QDSIO_REGADDR_0 ] &= 0xef; // inverze huntphase bitu a konec
                            g_qdisk.status |= QDSTS_IMG_SYNC;
                            break;
                        }

                        sync1 = sync2;
                    };

                } else {

                    //printf ( "OK: hunt phase ( %d, %d)\n", g_qdisk.virt_status, g_qdisk.image_position );

                    if ( g_qdisk.image_position == 0 ) {
                        /* automaticky predpokladame g_qdisk.virt_status == QDISK_VRTSTS_MZFHEAD */

                        g_qdisk.image_position = 3;
                        channel->Rreg [ QDSIO_REGADDR_0 ] &= 0xef; // inverze huntphase bitu a konec
                        g_qdisk.status |= QDSTS_IMG_SYNC;

                    } else {

                        if ( g_qdisk.virt_status != QDISK_VRTSTS_FREE_FILEAREA ) {

                            if ( g_qdisk.virt_status == QDISK_VRTSTS_MZFHEAD ) {
                                qdisk_virt_prepare_mzf_body ( );
                                g_qdisk.virt_status = QDISK_VRTSTS_MZFBODY;

                            } else {

                                if ( g_qdisk.virt_status == QDISK_VRTSTS_QDHEADER ) {

                                    if ( ( g_qdisk.image_position > 4 ) && ( g_qdisk.virt_files_count != 0 ) ) {

                                        /* otevrit mzf a nastavit se na zacatek headeru */
                                        qdisk_virt_prepare_mzf_head ( );
                                        g_qdisk.virt_status = QDISK_VRTSTS_MZFHEAD;

                                    } else {
                                        /* zadne soubory na disku nemame */
                                        g_qdisk.virt_status = QDISK_VRTSTS_FREE_FILEAREA;
                                    };


                                } else if ( g_qdisk.virt_status == QDISK_VRTSTS_MZFBODY ) {

                                    if ( ( g_qdisk.virt_files_count - 1 ) > g_qdisk.virt_file_num ) {

                                        /* otevrit dalsi mzf a nastavit se na zacatek headeru */
                                        g_qdisk.virt_file_num++;
                                        qdisk_virt_prepare_mzf_head ( );
                                        g_qdisk.virt_status = QDISK_VRTSTS_MZFHEAD;

                                    } else {
                                        /* zadne dalsi soubory na disku nemame */
                                        g_qdisk.virt_status = QDISK_VRTSTS_FREE_FILEAREA;
                                    };
                                };
                            };

                            g_qdisk.image_position = 3;
                            channel->Rreg [ QDSIO_REGADDR_0 ] &= 0xef; // inverze huntphase bitu a konec
                            g_qdisk.status |= QDSTS_IMG_SYNC;

                        };
                    };
                };
            };

            channel->Rreg [ QDSIO_REGADDR_0 ] |= 0x01; /* v prijimacim bufferu je alespon jeden bajt */
            channel->Rreg [ QDSIO_REGADDR_0 ] |= 0x04; /* output buffer je prazdny */

            if ( g_qdisk.status & QDSTS_IMG_READY ) {
                channel->Rreg [ QDSIO_REGADDR_0 ] |= 0x08; /* DCD 1: disk je pritomen */
            } else {
                channel->Rreg [ QDSIO_REGADDR_0 ] &= ~0x08;
            };

            if ( g_qdisk.status & QDSTS_IMG_READONLY ) {
                channel->Rreg [ QDSIO_REGADDR_0 ] &= ~0x20; /* CTS 0: disk chranen proti zapisu */
            } else {
                channel->Rreg [ QDSIO_REGADDR_0 ] |= 0x20;
            };

            /* pokud jsme prekrocili velikost media, tk zahlasime CRC error */
            if ( QDISK_IMAGE_MAX_SIZE < g_qdisk.image_position ) {
                channel->Rreg [ QDSIO_REGADDR_1 ] |= 0x40; /* CTS 1: CRC error */
            } else {
                channel->Rreg [ QDSIO_REGADDR_1 ] &= ~0x40;
            };

            retval = channel->Rreg [ channel->REG_addr & 0x03 ];

            //printf ( "%s(): channel: '%c', port: 0x%02x, retval: 0x%02x, (PC = 0x%04x)\n", __func__, channel->name, SIO_addr + 0xf4, retval, z80ex_get_reg ( g_mz800_main.cpu, regPC ) );

            break;

        case QDSIO_ADDR_CTRL_B:

            if ( g_qdisk.status & QDSTS_HEAD_HOME ) {
                channel->Rreg [ QDSIO_REGADDR_0 ] = 0x08;
            } else {
                channel->Rreg [ QDSIO_REGADDR_0 ] = 0x00;
            };

            if ( ( g_qdisk.channel[ QDSIO_CHANNEL_A ].Wreg[ QDSIO_REGADDR_5 ] & 0x1a ) == 0x0a ) {
                if ( g_qdisk.out_crc16 != 0 ) {
                    qdisk_write_byte_into_drive ( 'C' );
                    qdisk_write_byte_into_drive ( 'R' );
                    qdisk_write_byte_into_drive ( 'C' );
                };
            };

            if ( QDSIO_REGADDR_0 == channel->REG_addr ) {
                retval = 0xff;
            } else {
                retval = channel->Rreg [ channel->REG_addr & 0x03 ];
            };

            break;

        case QDSIO_ADDR_DATA_A:

            g_qdisk.status &= ~QDSTS_HEAD_HOME;

            if ( g_qdisk.status & QDSTS_IMG_READY ) {
                retval = qdisk_read_byte_from_drive ( );
            };
            break;

        case QDSIO_ADDR_DATA_B:
            retval = 0xff;
            break;
    };

    channel->REG_addr = QDSIO_REGADDR_0;

    return retval;
}


void qdisk_write_byte ( en_QDSIO_ADDR SIO_addr, uint8_t value ) {

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat QD write (SIO).
     *
     * Payload:
     *   [0] = SIO_addr (0..3 = data A, data B, ctrl A, ctrl B)
     *   [1] = value
     *   [2..5] = rezervováno
     */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        uint8_t payload[ 6 ] = {
            (uint8_t)( SIO_addr & 0xff ), value, 0, 0, 0, 0
        };
        hwlog_record ( HWLOG_CHIP_QD, HWLOG_QD_REGISTER_WRITE, payload );
    }
#endif

    st_QDSIO_CHANNEL *channel = &g_qdisk.channel[ SIO_addr & 0x01 ];


    /* zapis na CTRL [ A / B ] */
    if ( SIO_addr & 0x02 ) {

        channel->Wreg[ channel->REG_addr ] = value;

        if ( QDSIO_REGADDR_0 == channel->REG_addr ) {

            channel->REG_addr = value & 0x07;
            en_QDSIO_WR0CMD wr0cmd = ( value >> 3 ) & 0x07;

            /* reset vypoctu CRC odchozich dat */
            if ( ( value & 0xc0 ) == 0x80 ) {
                g_qdisk.out_crc16 = 0;
            };

            switch ( wr0cmd ) {

                case QDSIO_WR0CMD_RESET:
                    memset ( &channel->Wreg, 0x00, sizeof ( channel->Wreg ) );
                    break;

                case QDSIO_WR0CMD_NONE:
                case QDSIO_WR0CMD_RESET_INTF:
                case QDSIO_WR0CMD_SDLC_STOP:
                case QDSIO_WR0CMD_ENABLE_INT:
                case QDSIO_WR0CMD_RESET_OUTBUF_INT:
                case QDSIO_WR0CMD_RESET_ERRFL:
                case QDSIO_WR0CMD_RETI:
                    break;
            };

        } else {

            switch ( channel->REG_addr ) {


                case QDSIO_REGADDR_2:

                    /* nastaveni interrupt vectoru ( lze jen u kanalu B ) */
                    if ( channel->name == 'B' ) {
                        channel->Rreg [ channel->REG_addr ] = value;
                    }
                    break;

                    /* Rx CTRL*/
                case QDSIO_REGADDR_3:
                    if ( channel->Wreg [ QDSIO_REGADDR_3 ] & 0x10 ) {
                        /* vstup do rezimu Hunt */
                        channel->Rreg [ QDSIO_REGADDR_0 ] |= 0x10;
                    };
                    break;

                    /* Tx CTRL */
                case QDSIO_REGADDR_5:

                    if ( channel->name == 'B' ) {

                        if ( ( channel->Wreg[ QDSIO_REGADDR_5 ] & 0x80 ) == 0x00 ) {
                            /* QD motor nenaktivni */

                            if ( g_qdisk.status & QDSTS_IMG_READY ) {

                                if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {

#ifdef COMPILE_FOR_EMULATOR
                                    /* Flush dirty RAM bufferu zpet do
                                     * souboru pri vypnuti motoru.
                                     * No-op kdyz handler ne-MEMORY, R/O,
                                     * nebo memspec.updated == 0. Konsolidace
                                     * pres verejne sync API (Faze 4). */
                                    ( void ) qdisk_sync_drive ( );
#else
                                    if ( FS_LAYER_FR_OK != FS_LAYER_FSEEK ( g_qdisk.image_fp, 0 ) ) {
                                        DBGPRINTF ( DBGERR, "fseek() error\n" );
                                    };

                                    FS_LAYER_FSYNC ( g_qdisk.image_fp );
#endif
                                };
                            };

                            qdisk_drive_reset ( );

                        };

                    } else {

                        if ( ( channel->Wreg[ QDSIO_REGADDR_5 ] & 0x18 ) == 0x18 ) {

                            /* signal preruseni vysilani + povoleno odesilani dat */

                            if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {
                                qdisk_write_byte_into_drive ( 0x00 );
                            };

                        } else if ( ( channel->Wreg[ QDSIO_REGADDR_5 ] & 0x1a ) == 0x0a ) {
                            /* signal preruseni vysilani + povoleno odesilani dat + signal RTS */
                            /* zapis synchronizacni znacky */

                            if ( ( g_qdisk.type == QDISK_TYPE_IMAGE ) || ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {
                                qdisk_write_byte_into_drive ( channel->Wreg[ QDSIO_REGADDR_6 ] );
                                qdisk_write_byte_into_drive ( channel->Wreg[ QDSIO_REGADDR_7 ] );
                            } else {
                                //printf ( "WR SYNC: ( %d, %d)\n", g_qdisk.virt_status, g_qdisk.image_position );

                                if ( ( ( g_qdisk.virt_status == QDISK_VRTSTS_QDHEADER ) && ( g_qdisk.image_position == 0 ) ) || ( g_qdisk.virt_status == QDISK_VRTSTS_FORMATING ) ) {

                                    g_qdisk.image_position = 3;
                                } else {

                                    g_qdisk.image_position = 3;

                                    if ( 0 != qdisk_test_disk_is_writeable ( ) ) {

                                        if ( g_qdisk.virt_status != QDISK_VRTSTS_WR_MZFHEAD ) {

                                            qdisk_virt_close_mzf ( );

                                            char *dirpath = cfgelement_get_text_value ( g_elm_virt_fp );
                                            const char *filepath = baseui_tools_build_filepath ( dirpath, QDISK_VIRT_TEMP_FNAME );

                                            if ( FS_LAYER_FR_OK != FS_LAYER_FOPEN ( g_qdisk.mzf_fp, (char*) filepath, FS_LAYER_FMODE_W ) ) {
                                                DBGPRINTF ( DBGERR, "fopen()\n" );
#ifdef COMPILE_FOR_EMULATOR
                                                baseui_error ( "Can't create open file '%s': %s", filepath, strerror ( errno ) );
#endif
                                            } else {
                                                baseui_tools_free_filepath ( filepath );
                                                g_qdisk.virt_status = QDISK_VRTSTS_WR_MZFHEAD;
                                            };

                                        } else {
                                            g_qdisk.virt_status = QDISK_VRTSTS_WR_MZFBODY;

                                            unsigned len;
                                            uint8_t mzf_cmnt[ 104 - 38 ];

                                            memset ( mzf_cmnt, 0x00, sizeof ( mzf_cmnt ) );
                                            if ( FS_LAYER_FR_OK != FS_LAYER_FWRITE ( g_qdisk.mzf_fp, mzf_cmnt, sizeof ( mzf_cmnt ), &len ) ) {
                                                DBGPRINTF ( DBGERR, "fwrite() error\n" );
                                            };
                                        };

                                    };

                                };
                            }
                        };

                    };
                    break;

                case QDSIO_REGADDR_0:
                case QDSIO_REGADDR_1:
                case QDSIO_REGADDR_4:
                case QDSIO_REGADDR_6:
                case QDSIO_REGADDR_7:
                    break;

            };

            channel->REG_addr = QDSIO_REGADDR_0;
        };

    } else {
        if ( channel->name == 'A' ) {
            g_qdisk.out_crc16 ^= value;
            qdisk_write_byte_into_drive ( value );
        };
    };

}


void qdisk_deactivate_unicard_boot_loader ( void ) {
    if ( ( g_qdisk.connected == QDISK_CONNECTED ) && ( g_qdisk.type == QDISK_TYPE_UNICARD ) ) {
        /* Faze 5 (qdisk-rewrite): NEzapisovat do CFGELM mz1f11_write_protected.
         * Force R/O byl jen runtime přes qdisk_open_image_internal, user pref
         * (= mz1f11_write_protected v INI) zůstal po celou dobu UNICARD modu
         * netknutý. qdisk_open() pro IMAGE branch ho při reopen načte zpět
         * skrz standardní defaultní cestu (qdisk_open_image -> CFGELM read). */
        qdisk_close ( );
        g_qdisk.type = QDISK_TYPE_IMAGE;
        qdisk_open_image ( "" );
        qdisk_open ( );
    };
}


void qdisk_activate_unicard_boot_loader ( void ) {
    if ( g_qdisk.connected == QDISK_CONNECTED ) {
        qdisk_close ( );
    };
    g_qdisk.connected = QDISK_CONNECTED;
    g_qdisk.type = QDISK_TYPE_UNICARD;
    /* Faze 5 (qdisk-rewrite): NEvolat qdisk_set_write_protected(1) - to by
     * zapsalo do INI (mz1f11_write_protected=1) a přepsalo user pref pro IMAGE
     * mód. Force R/O řeší qdisk_open() UNICARD branch runtime přes
     * qdisk_open_image_internal(force_user_readonly=1). */
    qdisk_open ( );
}

int qdisc_get_write_protected(void)
{
    return cfgelement_get_bool_value ( g_elm_wrprt );
}


#ifdef COMPILE_FOR_EMULATOR
const char *qdisk_get_storage_mode_str ( void ) {
    /* Faze 3: CFGELM default je "cached", takze get_text_value nikdy nevrati
     * NULL pro existujici klic. Pro extra bezpecnost normalizujeme pres
     * parse + tostring (= fallback na "cached" pri neznamem stringu). */
    const char *raw = cfgelement_get_text_value ( g_elm_storage_mode );
    return qdisk_storage_mode_to_string ( qdisk_parse_storage_mode ( raw ) );
}


void qdisk_apply_storage_mode_switch ( const char *target_mode ) {
    /* Faze 3: zapis perzistentni volby + remount (pokud je co remountovat).
     * Volat z UI thread (nikdy z hot path Z80 emulace). */
    if ( target_mode == NULL ) return;

    /* Normalizujeme pres parser - zamezi ulozeni neznameho stringu do INI. */
    const char *normalized = qdisk_storage_mode_to_string ( qdisk_parse_storage_mode ( target_mode ) );
    cfgelement_set_text_value ( g_elm_storage_mode, (char*) normalized );

    /* Remount: jen pokud je drive aktualne mountnuty s image / unicard.
     * VIRTUAL rezim storage mode nepouziva - nedotykame se. */
    if ( ( g_qdisk.connected != QDISK_CONNECTED ) ) return;
    if ( ( g_qdisk.type != QDISK_TYPE_IMAGE ) && ( g_qdisk.type != QDISK_TYPE_UNICARD ) ) return;
    if ( ! ( g_qdisk.status & QDSTS_IMG_READY ) ) return;

    /* qdisk_close + qdisk_open: open precte aktualni filepath z CFGELM
     * (mz1f11_std_filepath pro IMAGE, unicard_get_mzfloader_image_filepath
     * pro UNICARD) a aplikuje novy storage_mode pres novou volbu driveru.
     *
     * POZN: existujici dirty RAM zmeny CACHED -> jiny mode by se ztratily
     * bez prompt. UI vrstva (menu_qdisk.cpp switch popup) detekuje tento
     * pripad pres qdisk_drive_has_ram_changes() a nabidne Save/Discard/Cancel
     * pred volanim teto funkce. */
    qdisk_close ( );
    qdisk_open ( );
}


int qdisk_drive_has_ram_changes ( void ) {
    if ( ! g_qdisk.handler_valid ) return 0;
    if ( g_qdisk.handler.type != HANDLER_TYPE_MEMORY ) return 0;
    return g_qdisk.handler.spec.memspec.updated ? 1 : 0;
}


int qdisk_drive_has_unsaved_changes ( void ) {
    /* Faze 4: stejna sada guardu jako qdisk_flush_image_to_file() - vrati 1
     * pouze pokud by skutecny flush sahl na disk. CFG default zachova drive
     * neoznaceny dirty, dokud opravdu neproblehne zapis pres
     * generic_driver_write (memory_driver pak nastavi memspec.updated = 1). */
    if ( ! g_qdisk.handler_valid )                              return 0;
    if ( g_qdisk.handler.type != HANDLER_TYPE_MEMORY )          return 0;
    if ( g_qdisk.storage_mode != QDISK_STORAGE_CACHED )         return 0;
    if ( g_qdisk.status & QDSTS_IMG_READONLY )                  return 0;
    if ( g_qdisk.filename[0] == 0x00 )                          return 0;
    return g_qdisk.handler.spec.memspec.updated ? 1 : 0;
}


int qdisk_sync_drive ( void ) {
    /* Faze 4: verejny equivalent qdisk_flush_image_to_file(). Pouziva stejnou
     * guard logiku pres qdisk_drive_has_unsaved_changes(), takze no-op pripady
     * (DIRECT, DISCARD, R/O, no handler, no dirty) projdou jako uspech bez
     * zapisu. Po flushi vynuluje memspec.updated. */
    if ( ! qdisk_drive_has_unsaved_changes ( ) ) return 1;

    if ( EXIT_SUCCESS != generic_driver_save_memory ( &g_qdisk.handler, g_qdisk.filename ) ) {
        fprintf ( stderr, "%s(%d): qdisk_sync_drive: failed to flush QD image '%s' to file\n",
                  __FILE__, __LINE__, g_qdisk.filename );
        return 0;
    };
    g_qdisk.handler.spec.memspec.updated = 0;
    return 1;
}


int qdisk_drive_force_save_to_file ( void ) {
    /* Faze 4: bypass storage_mode gate - pro "Save and switch" akci ve
     * storage switch popupu, kde user explicitne chce zapsat RAM buffer
     * i kdyz je aktualne DISCARD mode. Ostatni guards (handler valid,
     * MEMORY type, !R/O, filename non-empty) zustavaji - bez nich nelze
     * fyzicky zapsat. */
    if ( ! g_qdisk.handler_valid )                              return 0;
    if ( g_qdisk.handler.type != HANDLER_TYPE_MEMORY )          return 0;
    if ( g_qdisk.status & QDSTS_IMG_READONLY )                  return 0;
    if ( g_qdisk.filename[0] == 0x00 )                          return 0;

    if ( EXIT_SUCCESS != generic_driver_save_memory ( &g_qdisk.handler, g_qdisk.filename ) ) {
        fprintf ( stderr, "%s(%d): qdisk_drive_force_save_to_file: failed for '%s'\n",
                  __FILE__, __LINE__, g_qdisk.filename );
        return 0;
    };
    g_qdisk.handler.spec.memspec.updated = 0;
    return 1;
}
#endif