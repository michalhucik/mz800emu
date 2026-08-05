/* 
 * File:   unicard.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 21. června 2018, 16:07
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <glib/gstdio.h>

// Lokalizace
#include "i18n.h"

#include "mzarch/mzcommon_config.h"
#include "mzarch/mzhal.h"
#include "fs_layer.h"
#include "cfgmain.h"
#include "qdisk/qdisk.h"

#include "ff_result.h"
#include "unicard.h"
#include "unimgr.h"
#include "MGR_V24_MZF.h"
#include "MGR800_V211B_MZF.h"
#include "MGR1500_V211B_MZF.h"
#include "MGR700_V211B_MZF.h"
#include "MZFLOADER_MZF.h"
#include "SC3SROM_MZF.h"
#include "SDERR800_MZF.h"
#include "SDERR1500_MZF.h"
#include "emulator/mzarch/mzarch_platform.h"
#include "fdc/fdc.h"
#include "baseui/baseui.h"
#include "baseui/baseui_filechooser.h"

st_UNICARD g_unicard;

static CFGELM *g_elm_connected;
static CFGELM *g_elm_sd_root;
static CFGELM *g_elm_readonly;
static CFGELM *g_elm_fw_version;
static CFGELM *g_elm_runtime_check_on_connect;
static CFGELM *g_elm_managed_fdc;

/**
 * @brief Vrátí instanci FDC, kterou Unicard API spravuje, nebo NULL.
 *
 * Mapuje g_unicard.managed_fdc na ukazatel do pole g_fdc. NONE vrací
 * NULL (Unicard FDD příkazy se nepropagují do žádného řadiče).
 *
 * @return ukazatel na st_FDC, nebo NULL pokud managed_fdc == NONE.
 */
static st_FDC *unicard_managed_fdc_instance ( void ) {
    switch ( g_unicard.managed_fdc ) {
        case UNICARD_MANAGED_FDC_FDC0: return &g_fdc[FDC0];
        case UNICARD_MANAGED_FDC_FDC1: return &g_fdc[FDC1];
        default:                       return NULL;
    }
}

int g_unicard_runtime_mismatch_pending = 0;

/* Vnitřní flag - když je true, příští volání unicard_set_connected
 * přeskočí runtime check. Pousivá se po "Continue" volbě v mismatch
 * dialogu, aby user mohl projet připojení s vědomě mismatchnutým SD
 * obsahem bez okamžitého znovuotevření dialogu. Resetuje se na 0
 * uvnitř funkce po prvním použití (one-shot). */
static int s_unicard_bypass_runtime_check = 0;

void unicard_bypass_runtime_check_once ( void ) {
    s_unicard_bypass_runtime_check = 1;
}

/* Per-session counter pro potlačení opakovaných R/O warningů. */
static int g_unicard_ro_warned = 0;

/* Forward declarations interních helperů. */
static char* unicard_normalize_emu_path ( const char *emu_path, int *escape_detected );
static void unicard_time_to_fat ( time_t t, uint16_t *fdate, uint16_t *ftime );


char* unicard_get_sd_root_dirpath ( void ) {
    return cfgelement_get_text_value ( g_elm_sd_root );
}


void unicard_set_sd_root_dirpath ( char *sd_root ) {
    if ( ( !sd_root ) || ( sd_root[0] == 0x00 ) ) return;
    cfgelement_set_text_value ( g_elm_sd_root, sd_root );
    char *sd_root_locale = baseui_tools_file_name_locale_from_utf8 ( sd_root );
    printf ( "Unicard: new SD root is '%s'\n", sd_root_locale );
    g_free ( sd_root_locale );
}


en_UNICARD_FW unicard_get_fw ( void ) {
    return g_unicard.fw_emulated;
}


void unicard_set_fw ( en_UNICARD_FW fw ) {
    /* uc1 firmware existuje jen pro MZ-800 - na ostatních platformách
     * ignoruj pokus o přepnutí. */
    if ( fw == UNICARD_FW_UC1 && g_mzarch_platform_numeric != 800 ) {
        printf ( "WARNING: Unicard FW uc1 is only available on MZ-800; ignoring request\n" );
        return;
    }
    if ( fw == g_unicard.fw_emulated ) return;

    /* Force disconnect + reconnect když je Unicard aktivní - dispatch
     * logika několika UNIMGR příkazů závisí na FW verzi, klient by viděl
     * nekonzistentní stav (otevřené file handles, pending příkaz). */
    int was_connected = UNICARD_TEST_IS_CONNECTED;
    if ( was_connected ) {
        unicard_set_connected ( UNICARD_CONNECTION_DISCONNECTED );
    }

    g_unicard.fw_emulated = fw;
    cfgelement_set_bool_value ( g_elm_fw_version, (int) fw );
    printf ( "INFO: Unicard simulated firmware version set to %s\n",
             ( fw == UNICARD_FW_UC1 ) ? "uc1 (rev.60)" : "uc3 (v0.26)" );

    if ( was_connected ) {
        unicard_set_connected ( UNICARD_CONNECTION_CONNECTED );
    }
}


uint8_t unicard_get_uc3_pc_type ( void ) {
    switch ( g_mzarch_platform_numeric ) {
        case 800:  return 0;  /* PCTYPE_MZ800 */
        case 1500: return 3;  /* PCTYPE_MZ1500 */
        case 700:  return 4;  /* PCTYPE_MZ700 */
        case 2500: return 5;  /* PCTYPE_MZ2500 */
        /* MZX nemá mzarch_numeric definovaný; pokud někdy budeme,
         * doplnit case s návratem 6. */
    }
    return 0;  /* fallback = MZ800 */
}


int unicard_get_runtime_check_on_connect ( void ) {
    return g_unicard.runtime_check_on_connect;
}


uint32_t unicard_make_mzq_from_mzf ( const uint8_t *mzf_data, uint32_t mzf_size,
                                       uint8_t *out_buff, uint32_t out_buff_size ) {
    if ( !mzf_data || !out_buff ) return 0;
    if ( mzf_size < 128 ) {
        fprintf ( stderr, "%s():%d - MZF too small (%u B, need >= 128)\n",
                  __func__, __LINE__, mzf_size );
        return 0;
    }

    /* Standard Sharp MZ MZF header (jak je na disku/CMT/.mzf souboru):
     *   [0]      ftype
     *   [1..16]  fname[16]
     *   [17]     0x0d terminator (nebo 0x00)
     *   [18..19] size LE (deklarovaná velikost programu v RAM = body
     *            size, nebo větší pro BSS-style work area)
     *   [20..21] start LE (load address)
     *   [22..23] exec LE (entry address; 0x0000 = default na load)
     *   [24..127] comment (104 B)
     *
     * Velikost dat ke vložení do MZQ MZF_BODY bloku musí odpovídat
     * deklarované size z headeru (= klient čte tolik bajtů). Pokud
     * fyzický body v MZF souboru je MENŠÍ (časté u SROM-style MZF
     * s "work area BSS rozšířením"), padding nulami.
     * Pokud fyzický body je VĚTŠÍ než declared, použijeme menší
     * z obou - nechceme číst za konec MZF souboru. */
    uint32_t declared_size = (uint32_t) mzf_data[18] | ( (uint32_t) mzf_data[19] << 8 );
    uint32_t actual_body   = mzf_size - 128;
    uint32_t body_size     = ( declared_size > actual_body ) ? declared_size : actual_body;

    if ( body_size > 0xFFFF ) {
        fprintf ( stderr, "%s():%d - MZF body too large (%u B, max 65535)\n",
                  __func__, __LINE__, body_size );
        return 0;
    }
    uint32_t mzq_size = 92 + body_size;
    if ( out_buff_size < mzq_size ) {
        fprintf ( stderr, "%s():%d - output buffer too small (%u B, need %u)\n",
                  __func__, __LINE__, out_buff_size, mzq_size );
        return 0;
    }

    memset ( out_buff, 0, mzq_size );

    /* [0..7] QD_HEADER 8 B */
    out_buff[0] = 0x00; out_buff[1] = 0x16; out_buff[2] = 0x16; out_buff[3] = 0xa5;
    out_buff[4] = 0x02;             /* file_blocks_count = 2 (1 file × 2 bloky) */
    out_buff[5] = 'C'; out_buff[6] = 'R'; out_buff[7] = 'C';  /* placeholder */

    /* [8..81] MZF_HEAD block 74 B - struktura odlišná od standard MZF
     * (po fname_end následuje 2-byte unused1 SLOT před size). Pole
     * proto kopírujeme selektivně, ne raw memcpy 64B z MZF.
     *
     * MZQ MZF_HEAD struct vs souborové offsety:
     *   [8..11]  start_sign            = 00 16 16 a5
     *   [12]     mzf_header_sign       = 0x00
     *   [13..14] data_size             = 64 LE
     *   [15]     mzf_ftype             <- MZF[0]
     *   [16..31] mzf_fname[16]         <- MZF[1..16]
     *   [32]     mzf_fname_end         <- MZF[17]
     *   [33..34] unused1[2]            = 0x00 0x00 (v MZF chybí)
     *   [35..36] mzf_size              <- MZF[18..19]
     *   [37..38] mzf_start             <- MZF[20..21]
     *   [39..40] mzf_exec              <- MZF[22..23]
     *   [41..78] description[38]       <- MZF[24..61] (prvních 38B comment)
     *   [79..81] crc[3]                = "CRC" placeholder
     */
    out_buff[8] = 0x00; out_buff[9] = 0x16; out_buff[10] = 0x16; out_buff[11] = 0xa5;
    out_buff[12] = 0x00;            /* mzf_header_sign */
    out_buff[13] = 0x40; out_buff[14] = 0x00;  /* data_size = 64 LE */
    memcpy ( &out_buff[15], &mzf_data[0],  18 );  /* ftype + fname + terminator */
    /* out_buff[33..34] = unused1 - ponecháme 0x00 z memset */
    memcpy ( &out_buff[35], &mzf_data[18], 6 );   /* size + start + exec */
    memcpy ( &out_buff[41], &mzf_data[24], 38 );  /* description = comment[0..37] */
    out_buff[79] = 'C'; out_buff[80] = 'R'; out_buff[81] = 'C';

    /* [82..] MZF_BODY block. data_size = declared size (= co klient
     * očekává číst). Body data: zkopírujeme actual_body bajtů z MZF
     * (= co skutečně máme), zbytek nechá memset jako 0 (= padding pro
     * BSS work area, klient si data za declared end stejně přepíše). */
    out_buff[82] = 0x00; out_buff[83] = 0x16; out_buff[84] = 0x16; out_buff[85] = 0xa5;
    out_buff[86] = 0x05;            /* mzf_body_sign */
    out_buff[87] = (uint8_t) ( body_size & 0xff );
    out_buff[88] = (uint8_t) ( ( body_size >> 8 ) & 0xff );
    memcpy ( &out_buff[89], &mzf_data[128], actual_body );
    /* out_buff[89+actual_body .. 89+body_size-1] = 0 z memset */
    out_buff[89 + body_size + 0] = 'C';
    out_buff[89 + body_size + 1] = 'R';
    out_buff[89 + body_size + 2] = 'C';

    return mzq_size;
}


uint32_t unicard_build_fallback_mzq ( uint8_t *out_buff, uint32_t out_buff_size ) {
    /* Fallback je definován pouze pro uc3 ekosystém. uc1 (UNIBOOT V2.0)
     * nemá ekvivalentní chybový loader, proto pro uc1 vracíme 0 a
     * necháme volajícího skončit klasickou chybou (mzfloader.mzq chybí). */
    if ( g_unicard.fw_emulated != UNICARD_FW_UC3 ) return 0;

    const uint8_t *src_mzf;
    uint32_t src_mzf_size;

    if ( g_mzarch_platform_numeric == 1500 ) {
        src_mzf      = c_UNICARD_SDERR1500_MZF;
        src_mzf_size = UNICARD_SDERR1500_MZF_SIZE;
    } else {
        /* MZ-800 i MZ-700 sdílí SDERR-800. Ostatní (neznámé) mzarch
         * platformy taky zkusí SDERR-800 jako rozumný default - uc3 FW
         * se primárně cílí na 800/700/1500. */
        src_mzf      = c_UNICARD_SDERR800_MZF;
        src_mzf_size = UNICARD_SDERR800_MZF_SIZE;
    }

    return unicard_make_mzq_from_mzf ( src_mzf, src_mzf_size,
                                       out_buff, out_buff_size );
}


void unicard_set_runtime_check_on_connect ( int enabled ) {
    int normalized = ( enabled ) ? 1 : 0;
    if ( normalized == g_unicard.runtime_check_on_connect ) return;
    g_unicard.runtime_check_on_connect = normalized;
    cfgelement_set_bool_value ( g_elm_runtime_check_on_connect, normalized );
}


static int unicard_init_sd_dir ( char *dirpath, char *description ) {

    char *dirpath_locale = baseui_tools_file_name_locale_from_utf8 ( dirpath );

    if ( !g_file_test ( dirpath, G_FILE_TEST_EXISTS ) ) {
        printf ( "%s: '%s'\n", description, dirpath_locale );
        if ( -1 == g_mkdir_with_parents ( dirpath, UNICARD_DEFAULT_DIR_MODE ) ) {
            fprintf ( stderr, "%s():%d - Can't create directory '%s'\n", __func__, __LINE__, dirpath_locale );
            g_free ( dirpath_locale );
            return EXIT_FAILURE;
        };
    };

    if ( !g_file_test ( dirpath, G_FILE_TEST_IS_DIR ) ) {
        fprintf ( stderr, "%s():%d - This is not a directory '%s'\n", __func__, __LINE__, dirpath_locale );
        g_free ( dirpath_locale );
        return EXIT_FAILURE;
    };

    g_free ( dirpath_locale );
    return EXIT_SUCCESS;
}


static int unicard_init_sd_file ( char *filepath, const uint8_t *src, uint32_t size ) {

    if ( !g_file_test ( filepath, G_FILE_TEST_EXISTS ) ) {
        char *filepath_locale = baseui_tools_file_name_locale_from_utf8 ( filepath );
        printf ( "Unicard - create file: '%s'\n", filepath_locale );
        FILE *fh = g_fopen ( filepath, "wb" );
        if ( !fh ) {
            fprintf ( stderr, "%s():%d - Can't create file '%s'\n", __func__, __LINE__, filepath_locale );
            g_free ( filepath_locale );
            return EXIT_FAILURE;
        };

        uint32_t write_len = 0;
        int ret = fs_layer_fwrite ( &fh, (uint8_t*) src, size, &write_len );
        fclose ( fh );
        if ( ( ret != FS_LAYER_FR_OK ) || ( write_len != size ) ) {
            fprintf ( stderr, "%s():%d - Can't write file '%s'\n", __func__, __LINE__, filepath_locale );
            g_free ( filepath_locale );
            return EXIT_FAILURE;
        };

        g_free ( filepath_locale );
    };

    return EXIT_SUCCESS;
}


static int unicard_init_sd_for ( char *dirpath, en_UNICARD_FW fw ) {
    int ret;

    printf ( "%s():%d Unicard - SD root: '%s', fw=%s\n", __func__, __LINE__, dirpath,
             ( fw == UNICARD_FW_UC1 ) ? "uc1" : "uc3" );

    ret = unicard_init_sd_dir ( dirpath, "Unicard - create SD root" );
    if ( EXIT_SUCCESS != ret ) return EXIT_FAILURE;

    char *unimgr_dir = g_build_filename ( dirpath, UNICARD_UNIMGR_DIR, NULL );

    ret = unicard_init_sd_dir ( unimgr_dir, "Unicard - create unimgr directory" );
    baseui_tools_mem_free ( unimgr_dir );
    if ( EXIT_SUCCESS != ret ) return EXIT_FAILURE;

    char *filepath = NULL;

    /* mzfloader.mzq: zabalíme MZF do MZQ on-the-fly. Volba zdrojového
     * MZF závisí na simulované verzi FW:
     *   uc1: c_UNICARD_MZFLOADER_MZF - klasický UNIBOOT V2.0 (rev.60 ekosystém)
     *   uc3: c_UNICARD_SC3SROM_MZF   - SC3SROM bootloader (V2.11b ekosystém)
     * Rutina unicard_make_mzq_from_mzf vyrobí MZQ kontejner co Unicard
     * boot loader v qdisk emulaci očekává. Tím můžeme měnit obsah mzfloader
     * v MZF formě (= snazší údržba) a nemusíme držet hotovou MZQ konstantu. */
    {
        const uint8_t *src_mzf;
        uint32_t src_mzf_size;
        if ( fw == UNICARD_FW_UC1 ) {
            src_mzf = c_UNICARD_MZFLOADER_MZF;
            src_mzf_size = UNICARD_MZFLOADER_MZF_SIZE;
        } else {
            src_mzf = c_UNICARD_SC3SROM_MZF;
            src_mzf_size = UNICARD_SC3SROM_MZF_SIZE;
        }
        uint8_t mzq_buff[92 + 0x10000];  /* max MZQ = 92 hlavička + 64 KB body */
        uint32_t mzq_size = unicard_make_mzq_from_mzf (
            src_mzf, src_mzf_size, mzq_buff, sizeof ( mzq_buff ) );
        if ( mzq_size == 0 ) {
            fprintf ( stderr, "%s():%d - failed to build MZQ from embedded MZF\n",
                      __func__, __LINE__ );
            return EXIT_FAILURE;
        }
        filepath = g_build_filename ( dirpath, UNICARD_UNIMGR_DIR, UNICARD_UNIMGR_MZFLOADER_MZQ, NULL );
        ret = unicard_init_sd_file ( filepath, mzq_buff, mzq_size );
        baseui_tools_mem_free ( filepath );
        if ( EXIT_SUCCESS != ret ) return EXIT_FAILURE;
    }

    /* Manager: per-architektura + per-FW. uc1 (= MZ-800 only) instaluje
     * MGR V2.4 (mgr.mzf). uc3 instaluje per-mzarch V2.11b variantu:
     * MZ-800 -> mgr800.mzf, MZ-1500 -> mgr1500.mzf, MZ-700 -> mgr700.mzf.
     * (V2.11b oprava prosince 2021 mj. přidává podporu HDF/SASI mountu.)
     *
     * `fw` parametr přepisuje globální g_unicard.fw_emulated - dovoluje
     * uživateli ručně z menu zvolit jinou verzi pro install, než je
     * aktuální simulační FW (využito v Reinit dialogu na MZ-800). */
    const uint8_t *mgr_data;
    uint32_t mgr_size;
    const char *mgr_name;
    if ( fw == UNICARD_FW_UC1 ) {
        /* uc1 jen na MZ-800 (validováno v UI dispatch). */
        mgr_data = c_UNICARD_MGR_V24_MZF;
        mgr_size = UNICARD_MGR_V24_MZF_SIZE;
        mgr_name = UNICARD_UNIMGR_MGR_MZF;
    } else {
        /* uc3 per-mzarch. */
        if ( g_mzarch_platform_numeric == 800 ) {
            mgr_data = c_UNICARD_MGR800_V211B_MZF;
            mgr_size = UNICARD_MGR800_V211B_MZF_SIZE;
            mgr_name = UNICARD_UNIMGR_MGR800_MZF;
        } else if ( g_mzarch_platform_numeric == 700 ) {
            mgr_data = c_UNICARD_MGR700_V211B_MZF;
            mgr_size = UNICARD_MGR700_V211B_MZF_SIZE;
            mgr_name = UNICARD_UNIMGR_MGR700_MZF;
        } else {
            /* MZ-1500 (a fallback pro ostatní mzarch). */
            mgr_data = c_UNICARD_MGR1500_V211B_MZF;
            mgr_size = UNICARD_MGR1500_V211B_MZF_SIZE;
            mgr_name = UNICARD_UNIMGR_MGR1500_MZF;
        }
    };

    filepath = g_build_filename ( dirpath, UNICARD_UNIMGR_DIR, mgr_name, NULL );
    ret = unicard_init_sd_file ( filepath, mgr_data, mgr_size );
    baseui_tools_mem_free ( filepath );
    if ( EXIT_SUCCESS != ret ) return EXIT_FAILURE;

    filepath = g_build_filename ( dirpath, UNICARD_UNIMGR_DIR, UNICARD_UNIMGR_MZFLOADER_CFG, NULL );

    uint32_t len = 1 + strlen ( UNICARD_UNIMGR_DIR ) + 1 + strlen ( mgr_name ) + 2;
    char *buff = (char*) baseui_tools_mem_alloc0 ( len );
    snprintf ( buff, len, "/%s/%s%c", UNICARD_UNIMGR_DIR, mgr_name, 0x0d );

    ret = unicard_init_sd_file ( filepath, (uint8_t*) buff, len - 1 );
    baseui_tools_mem_free ( buff );
    baseui_tools_mem_free ( filepath );
    if ( EXIT_SUCCESS != ret ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


int unicard_reinit_sd_runtime ( void ) {
    /* Manuální reinicializace runtime souborů v {SD}/unicard/ - vytvoří
     * případné chybějící mgr.mzf / mzfloader.mzq / mzfloader.cfg (resp.
     * příslušný per-arch mgr1500.mzf). Existující soubory zůstávají
     * nedotčené (unicard_init_sd_file overwrite nedělá, jen create).
     * Vyžaduje aby Unicard byl connected (= máme platný SD root path). */
    if ( !UNICARD_TEST_IS_CONNECTED ) return EXIT_FAILURE;
    char *path = unicard_get_sd_root_dirpath ( );
    if ( !path || !*path ) return EXIT_FAILURE;
    return unicard_init_sd_for ( path, g_unicard.fw_emulated );
}


/* Detekuje typ boot loaderu uvnitř {SD}/unicard/mzfloader.cfg podle
 * cesty k MGR souboru, na kterou cfg ukazuje. Formát cfg souboru je
 * `/unicard/<mgr_name>\x0d` (vyprodukováno init_sd_for v unicard.c).
 *
 * Mapování pro aktuální mzarch:
 *   /unicard/mgr.mzf          -> uc1
 *   /unicard/mgr800.mzf       -> uc3 (jen na MZ-800)
 *   /unicard/mgr700.mzf       -> uc3 (jen na MZ-700)
 *   /unicard/mgr1500.mzf      -> uc3 (jen na MZ-1500)
 *   jakákoliv jiná cesta nebo cesta na mgrXXX pro jinou mzarch
 *     než aktuální            -> -1 (= inkonzistentní cfg)
 *   soubor chybí / prázdný    -> -1
 *
 * Záměrně NEnačítáme cfg pro VŠECHNY mzarch varianty: cfg má být
 * konzistentní s mzarch, na kterém Unicard běží. Pokud user vsunul SD
 * z jiné mzarch instalace, runtime check detekuje nesoulad.
 */
static int unicard_detect_mzfloader_cfg_variant ( const char *unimgr_dir ) {
    char *cfg_path = g_build_filename ( unimgr_dir, UNICARD_UNIMGR_MZFLOADER_CFG, NULL );
    int variant = -1;

    FILE *fp = baseui_tools_file_open ( cfg_path, "rb" );
    if ( fp ) {
        char buff[128];
        size_t n = fread ( buff, 1, sizeof ( buff ) - 1, fp );
        fclose ( fp );
        buff[n] = 0;

        /* Truncate na první 0x0d (firmware-level terminator). Zachová
         * případný NUL byte/binary garbage po 0x0d v patologických
         * souborech jako "ne-match" pro strcmp dole. */
        for ( size_t i = 0; i < n; i++ ) {
            if ( buff[i] == 0x0d ) { buff[i] = 0; break; }
        }

        const char *uc3_mgr_name;
        if ( g_mzarch_platform_numeric == 800 )      uc3_mgr_name = UNICARD_UNIMGR_MGR800_MZF;
        else if ( g_mzarch_platform_numeric == 700 ) uc3_mgr_name = UNICARD_UNIMGR_MGR700_MZF;
        else                                         uc3_mgr_name = UNICARD_UNIMGR_MGR1500_MZF;

        char expected_uc1[64];
        char expected_uc3[64];
        snprintf ( expected_uc1, sizeof ( expected_uc1 ), "/%s/%s",
                   UNICARD_UNIMGR_DIR, UNICARD_UNIMGR_MGR_MZF );
        snprintf ( expected_uc3, sizeof ( expected_uc3 ), "/%s/%s",
                   UNICARD_UNIMGR_DIR, uc3_mgr_name );

        if ( 0 == strcmp ( buff, expected_uc1 ) ) {
            variant = (int) UNICARD_FW_UC1;
        } else if ( 0 == strcmp ( buff, expected_uc3 ) ) {
            variant = (int) UNICARD_FW_UC3;
        }
    }
    g_free ( cfg_path );
    return variant;
}


/* Detekuje typ boot loaderu uvnitř {SD}/unicard/mzfloader.mzq podle
 * fname uloženého v MZQ MZF_HEAD bloku (bytes 16..31, ukončený 0x0d).
 *   "UNIBOOT V2.0" -> uc1 (klasický rev.60 loader, viz MZFLOADER_MZF)
 *   "SC3SROM"      -> uc3 (V2.11b ekosystém)
 *   jiné / soubor chybí -> -1 (neznámé)
 */
static int unicard_detect_mzfloader_variant ( const char *unimgr_dir ) {
    char *mzfloader_path = g_build_filename ( unimgr_dir, UNICARD_UNIMGR_MZFLOADER_MZQ, NULL );
    int variant = -1;

    FILE *fp = baseui_tools_file_open ( mzfloader_path, "rb" );
    if ( fp ) {
        uint8_t mzq_head[32];
        size_t n = fread ( mzq_head, 1, sizeof ( mzq_head ), fp );
        fclose ( fp );

        /* MZQ struct: bytes 0..7 = QD_HEADER, bytes 8..11 = MZF_HEAD start_sign,
         * byte 12 = sign, bytes 13..14 = data_size, byte 15 = ftype,
         * bytes 16..31 = fname[16] (UTF-8/ASCII, ukončené 0x0d). */
        if ( n == 32 ) {
            char fname[17];
            memcpy ( fname, &mzq_head[16], 16 );
            fname[16] = 0;
            /* Truncate na 0x0d. */
            for ( int i = 0; i < 16; i++ ) {
                if ( fname[i] == 0x0d ) { fname[i] = 0; break; }
            }
            if ( 0 == strcmp ( fname, "UNIBOOT V2.0" ) ) {
                variant = (int) UNICARD_FW_UC1;
            } else if ( 0 == strcmp ( fname, "SC3SROM" ) ) {
                variant = (int) UNICARD_FW_UC3;
            }
        }
    }
    g_free ( mzfloader_path );
    return variant;
}


en_UNICARD_RUNTIME_CHECK unicard_detect_runtime_fw ( void ) {
    char *sd_root = unicard_get_sd_root_dirpath ( );
    if ( !sd_root || !*sd_root ) return UNICARD_RUNTIME_CHECK_NO_DIR;

    char *unimgr_dir = g_build_filename ( sd_root, UNICARD_UNIMGR_DIR, NULL );
    if ( !g_file_test ( unimgr_dir, G_FILE_TEST_IS_DIR ) ) {
        g_free ( unimgr_dir );
        return UNICARD_RUNTIME_CHECK_NO_DIR;
    }

    /* 1) Detekce verze podle MGR souboru:
     * Per-mzarch očekávaný uc3 MGR název. Detekce hledá konkrétní
     * filename - na MZ-800 zajímá mgr800.mzf, ne mgr1500.mzf, atd. */
    const char *uc3_mgr_name;
    if ( g_mzarch_platform_numeric == 800 ) {
        uc3_mgr_name = UNICARD_UNIMGR_MGR800_MZF;
    } else if ( g_mzarch_platform_numeric == 700 ) {
        uc3_mgr_name = UNICARD_UNIMGR_MGR700_MZF;
    } else {
        uc3_mgr_name = UNICARD_UNIMGR_MGR1500_MZF;
    }

    char *uc3_path = g_build_filename ( unimgr_dir, uc3_mgr_name, NULL );
    char *uc1_path = g_build_filename ( unimgr_dir, UNICARD_UNIMGR_MGR_MZF, NULL );

    int uc3_mgr_exists = g_file_test ( uc3_path, G_FILE_TEST_IS_REGULAR );
    int uc1_mgr_exists = g_file_test ( uc1_path, G_FILE_TEST_IS_REGULAR );

    g_free ( uc3_path );
    g_free ( uc1_path );

    /* 2) Detekce verze podle obsahu mzfloader.mzq (fname uvnitř MZQ
     * MZF_HEAD bloku):
     *   "UNIBOOT V2.0" -> uc1
     *   "SC3SROM"      -> uc3
     *   soubor chybí nebo nelze rozpoznat -> -1. */
    int mzq_variant = unicard_detect_mzfloader_variant ( unimgr_dir );

    /* 3) Detekce verze podle obsahu mzfloader.cfg (cesta k MGR
     * souboru). cfg musí ukazovat na MGR variantu odpovídající uc1
     * nebo uc3 + aktuální mzarch; jinak -1. */
    int cfg_variant = unicard_detect_mzfloader_cfg_variant ( unimgr_dir );

    g_free ( unimgr_dir );

    /* 4) Disambiguace MGR variant při souběhu uc1 + uc3 MGR souborů
     * (uživatel ručně nakopíroval oba). Jako tiebreaker použijeme
     * mzq, pak cfg, pak default uc3. */
    int mgr_variant = -1;
    if ( uc1_mgr_exists && uc3_mgr_exists ) {
        if ( mzq_variant >= 0 )      mgr_variant = mzq_variant;
        else if ( cfg_variant >= 0 ) mgr_variant = cfg_variant;
        else                         mgr_variant = (int) UNICARD_FW_UC3;
    } else if ( uc1_mgr_exists ) {
        mgr_variant = (int) UNICARD_FW_UC1;
    } else if ( uc3_mgr_exists ) {
        mgr_variant = (int) UNICARD_FW_UC3;
    }

    /* 5) Strict policy: všechny tři indikátory (MGR, mzfloader.mzq,
     * mzfloader.cfg) musí být přítomné a vzájemně konzistentní.
     * Jakákoliv komponenta s -1 nebo neshoda variant mezi nimi vede
     * na UNKNOWN (= mismatch dialog vyzve uživatele k reinit). */
    if ( mgr_variant < 0 || mzq_variant < 0 || cfg_variant < 0 ) {
        fprintf ( stderr,
            "Unicard runtime check: incomplete install - "
            "MGR=%s, mzfloader.mzq=%s, mzfloader.cfg=%s\n",
            ( mgr_variant < 0 ) ? "missing"
                : ( ( mgr_variant == (int) UNICARD_FW_UC1 ) ? "uc1" : "uc3" ),
            ( mzq_variant < 0 ) ? "missing"
                : ( ( mzq_variant == (int) UNICARD_FW_UC1 ) ? "uc1" : "uc3" ),
            ( cfg_variant < 0 ) ? "missing"
                : ( ( cfg_variant == (int) UNICARD_FW_UC1 ) ? "uc1" : "uc3" ) );
        return UNICARD_RUNTIME_CHECK_UNKNOWN;
    }

    if ( mgr_variant != mzq_variant || mgr_variant != cfg_variant ) {
        fprintf ( stderr,
            "Unicard runtime check: inconsistent install - "
            "MGR=%s, mzfloader.mzq=%s, mzfloader.cfg=%s\n",
            ( mgr_variant == (int) UNICARD_FW_UC1 ) ? "uc1" : "uc3",
            ( mzq_variant == (int) UNICARD_FW_UC1 ) ? "uc1" : "uc3",
            ( cfg_variant == (int) UNICARD_FW_UC1 ) ? "uc1" : "uc3" );
        return UNICARD_RUNTIME_CHECK_UNKNOWN;
    }

    return ( mgr_variant == (int) UNICARD_FW_UC1 )
            ? UNICARD_RUNTIME_CHECK_DETECTED_UC1
            : UNICARD_RUNTIME_CHECK_DETECTED_UC3;
}


int unicard_reinit_sd_for_fw ( en_UNICARD_FW fw ) {
    /* Smaže všechny MGR varianty (mgr.mzf, mgr800.mzf, mgr700.mzf,
     * mgr1500.mzf) v {SD}/unicard/, pak znovu spustí init který
     * nainstaluje MGR odpovídající požadovanému fw + ostatní runtime
     * (mzfloader.mzq, mzfloader.cfg). */
    char *sd_root = unicard_get_sd_root_dirpath ( );
    if ( !sd_root || !*sd_root ) return EXIT_FAILURE;

    static const char *mgr_variants[] = {
        UNICARD_UNIMGR_MGR_MZF,
        UNICARD_UNIMGR_MGR800_MZF,
        UNICARD_UNIMGR_MGR700_MZF,
        UNICARD_UNIMGR_MGR1500_MZF,
        NULL
    };

    for ( int i = 0; mgr_variants[i] != NULL; i++ ) {
        char *path = g_build_filename ( sd_root, UNICARD_UNIMGR_DIR,
                                        mgr_variants[i], NULL );
        if ( g_file_test ( path, G_FILE_TEST_IS_REGULAR ) ) {
            if ( g_unlink ( path ) != 0 ) {
                fprintf ( stderr,
                    "WARNING: Unicard reinit: failed to remove '%s'\n",
                    mgr_variants[i] );
                /* pokračujeme - init_sd_for neoverwrituje, takže
                 * nenaháše starý a možná použije aktuální. Lepší než
                 * vrátit fail. */
            }
        }
        g_free ( path );
    }

    /* mzfloader.cfg ukazuje na konkrétní MGR (cesta /unicard/mgrXXX.mzf).
     * Pokud zůstal stará verze, klient na MZ by načetl starý mgr_name
     * který nyní fyzicky neexistuje. Smažeme ho taky, init_sd_for ho
     * znovu sestaví podle požadovaného fw. */
    char *cfg_path = g_build_filename ( sd_root, UNICARD_UNIMGR_DIR,
                                        UNICARD_UNIMGR_MZFLOADER_CFG, NULL );
    if ( g_file_test ( cfg_path, G_FILE_TEST_IS_REGULAR ) ) {
        g_unlink ( cfg_path );
    }
    g_free ( cfg_path );

    /* mzfloader.mzq: smažeme taky, init_sd_for ho vyrobí z embedded
     * MZF odpovídajícího fw (uc1 = UNIBOOT V2.0, uc3 = SC3SROM).
     * Důvod: unicard_init_sd_file neoverwrituje, takže pokud zůstala
     * stará / poškozená mzfloader.mzq, runtime checker by ji znovu
     * detekoval jako broken. Reinit by měl být destruktivní pro
     * runtime soubory, které Unicard generuje sám. */
    char *mzq_path = g_build_filename ( sd_root, UNICARD_UNIMGR_DIR,
                                        UNICARD_UNIMGR_MZFLOADER_MZQ, NULL );
    if ( g_file_test ( mzq_path, G_FILE_TEST_IS_REGULAR ) ) {
        g_unlink ( mzq_path );
    }
    g_free ( mzq_path );

    return unicard_init_sd_for ( sd_root, fw );
}


int unicard_reinit_sd_for_current_fw ( void ) {
    return unicard_reinit_sd_for_fw ( g_unicard.fw_emulated );
}


char* unicard_get_mzfloader_image_filepath ( void ) {
    if ( UNICARD_TEST_IS_CONNECTED ) {
        return g_build_filename ( unicard_get_sd_root_dirpath ( ), UNICARD_UNIMGR_DIR, UNICARD_UNIMGR_MZFLOADER_MZQ, NULL );
    };
    return (char*) baseui_tools_mem_alloc0 ( 1 );
}


/**
 * @brief Runtime default Unicard SD root ("SD-<arch>").
 *
 * Skládá se z g_mzhal.arch_name (mzhal krok 7) - hodnota identická
 * s dřívějším compile-time "SD-" MZ_PLATFORM_SUFFIX. Vrací ukazatel na
 * statický buffer naplněný při prvním volání (single-thread init cesta,
 * g_mzhal je const od load-time).
 *
 * @return Jméno adresáře relativní k cfg_dir; ukazatel je platný po
 *         celou dobu běhu, volající ho NEuvolňuje.
 */
static const char *unicard_default_sd_root ( void ) {
    static char dirname[32];
    if ( dirname[0] == 0x00 ) {
        snprintf ( dirname, sizeof ( dirname ), "SD-%s", g_mzhal.arch_name );
    };
    return dirname;
}


void unicard_set_connected ( en_UNICARD_CONNECTION conn ) {

    if ( strlen ( unicard_get_sd_root_dirpath ( ) ) == 0 ) {
        char *sd_root = (char*) baseui_tools_mem_alloc0 ( strlen ( unicard_default_sd_root ( ) ) + 1 );
        unicard_set_sd_root_dirpath ( sd_root );
        baseui_tools_mem_free ( sd_root );
    };

    if ( g_unicard.connected == conn ) return;

    if ( conn == UNICARD_CONNECTION_CONNECTED ) {
        char *path = unicard_get_sd_root_dirpath ( );
        /* Automatickou inicializaci SD (= vytvoření root dir + unicard/
         * subdir + runtime soubory mgr.mzf, mzfloader.mzq, mzfloader.cfg)
         * provedeme jen při prvotním setupu, kdy SD root neexistuje.
         * Pokud root existuje, předpokládáme že uživatel správu obsahu
         * řídí sám (= úmyslné smazání souboru se nesmí samočinně vrátit).
         * Pro manuální obnovu chybějících runtime souborů slouží akce
         * "Reinitialize Runtime" v topmenu (= unicard_reinit_sd_runtime). */
        if ( !g_file_test ( path, G_FILE_TEST_IS_DIR ) ) {
            if ( EXIT_SUCCESS != unicard_init_sd_for ( path, g_unicard.fw_emulated ) ) {
                return;
            };
        } else if ( g_unicard.runtime_check_on_connect && !s_unicard_bypass_runtime_check ) {
            /* SD root existuje a check je zapnutý. Detekuj stav runtime
             * v {SD}/unicard/ a porovnej s aktuální fw_emulated. Pokud
             * stav není OK (= mismatch / nekonzistentní / chybějící
             * podadresář), nastav pending dialog flag a NEpřipojuj.
             * UI vrstva (menu_unicard.cpp) v dalším render frame otevře
             * modal s nabídkou {Reinit, Continue, Cancel}.
             *
             * Klasifikace stavů a chování:
             *   DETECTED_UC* + odpovídá fw      -> OK, pokračuj
             *   DETECTED_UC* + neodpovídá fw    -> dialog (mismatch verze)
             *   UNKNOWN                         -> dialog (nekompletní /
             *                                      nekonzistentní instalace
             *                                      - některý z MGR/mzq/cfg
             *                                      chybí nebo neladí)
             *   NO_DIR                          -> dialog ({SD}/unicard/
             *                                      podadresář neexistuje)
             *
             * UNKNOWN i NO_DIR triggerují dialog nově (dříve mlčeli) -
             * runtime checker má aktivně hlídat veškerý obsah runtime
             * adresáře, ne jen detekci verze. */
            en_UNICARD_RUNTIME_CHECK rc = unicard_detect_runtime_fw ( );
            int needs_dialog = 0;
            if ( rc == UNICARD_RUNTIME_CHECK_DETECTED_UC1 &&
                 g_unicard.fw_emulated == UNICARD_FW_UC3 ) {
                needs_dialog = 1;
            } else if ( rc == UNICARD_RUNTIME_CHECK_DETECTED_UC3 &&
                        g_unicard.fw_emulated == UNICARD_FW_UC1 ) {
                needs_dialog = 1;
            } else if ( rc == UNICARD_RUNTIME_CHECK_UNKNOWN ) {
                needs_dialog = 1;
            } else if ( rc == UNICARD_RUNTIME_CHECK_NO_DIR ) {
                needs_dialog = 1;
            }
            if ( needs_dialog ) {
                g_unicard_runtime_mismatch_pending = (int) rc;
                return;
            }
        };
        g_unicard.connected = UNICARD_CONNECTION_CONNECTED;
        unimgr_init ( );
        qdisk_activate_unicard_boot_loader ( );
        printf ( "INFO: The Unicard device is connected. Press RESET (F12) + Q for boot into Unicard Manager\n" );
    } else {
        unimgr_exit ( ); // pozavirat otevrene deskriptory
        g_unicard.connected = UNICARD_CONNECTION_DISCONNECTED;
        qdisk_deactivate_unicard_boot_loader ( );
        printf ( "INFO: The Unicard device is disconnected\n" );
    };
    /* Bypass flag je one-shot - po jakémkoli set_connected resetujeme. */
    s_unicard_bypass_runtime_check = 0;
}


void unicard_init ( void ) {

    g_unicard.connected = UNICARD_CONNECTION_DISCONNECTED;
    g_unicard.readonly = 0;
    g_unicard.fw_emulated = UNICARD_FW_UC3;
    g_unicard.runtime_check_on_connect = 1;
    g_unicard.work_dir = (char*) baseui_tools_mem_alloc0 ( 2 );
    g_unicard.work_dir[0] = '/';
    unimgr_init ( );

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "UNICARD" );

    /* Default Unicard CONNECTED na všech platformách. Po implementaci
     * dual-FW (uc1 jen MZ-800, uc3 pro všechny) + SC3SROM mzfloader
     * + V2.11b managery per-mzarch je Unicard funkční na MZ-800, MZ-700
     * i MZ-1500 hned po fresh installu - není důvod ho defaultně vypínat.
     * Existující uživatelé s ini souborem si zachovají vlastní stav. */
    int default_conn = UNICARD_CONNECTION_CONNECTED;
    g_elm_connected = cfgmodule_register_new_element ( cmod, "connected", CFGENTYPE_BOOL, default_conn );
    cfgelement_set_handlers ( g_elm_connected, (void*) NULL, (void*) &g_unicard.connected );
    g_elm_sd_root = cfgmodule_register_new_element ( cmod, "sd_root", CFGENTYPE_TEXT, unicard_default_sd_root ( ) );

    /* Default R/W (0) - existující uživatelé bez záznamu si nepoznají
     * žádnou změnu chování. Bind oba směry pro persistenci přes
     * shutdown -> restart (propagate handler načte uloženou hodnotu
     * z INI do g_unicard.readonly při cfgmodule_propagate). */
    g_elm_readonly = cfgmodule_register_new_element ( cmod, "readonly", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( g_elm_readonly, (void*) &g_unicard.readonly, (void*) &g_unicard.readonly );

    /* Volba simulované verze Unicard firmware (uc1 / uc3).
     * BOOL semantika: 0 = UNICARD_FW_UC1, 1 = UNICARD_FW_UC3.
     *
     * Default prvního spuštění je per-platforma:
     *   - MZ-800: uc1 (rev.60). Firmware uc3 má potvrzenou chybu v binárním
     *     cmdREADDIR (pole LFN se nekopíruje) a emulace uc3 ji záměrně NEreplikuje
     *     (chová se "jak by mělo být, kdyby FW byl OK") - tedy v rozporu se
     *     zásadou přesné emulace. Proto je default uc1, který je věrný HW.
     *   - ostatní platformy: uc3 (uc1 existuje jen pro MZ-800; uložená UC1
     *     hodnota se na jiné platformě po parse přepíše na UC3, viz níže).
     *
     * Bind oba směry: propagate handler -> hodnota z INI se po cfgmodule_propagate
     * zapíše do g_unicard.fw_emulated; save handler -> při shutdown se z téhož
     * pointeru čte aktuální hodnota a uloží do INI. */
    int default_fw = ( g_mzarch_platform_numeric == 800 )
                     ? (int) UNICARD_FW_UC1 : (int) UNICARD_FW_UC3;
    g_elm_fw_version = cfgmodule_register_new_element ( cmod, "fw_version", CFGENTYPE_BOOL, default_fw );
    cfgelement_set_handlers ( g_elm_fw_version, (void*) &g_unicard.fw_emulated, (void*) &g_unicard.fw_emulated );

    /* Default 1 = při Connect ověřit, zda SD runtime odpovídá nastavené
     * verzi firmware. Mismatch triggeruje dialog s nabídkou reinit. */
    g_elm_runtime_check_on_connect = cfgmodule_register_new_element ( cmod, "runtime_check_on_connect", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( g_elm_runtime_check_on_connect, (void*) &g_unicard.runtime_check_on_connect, (void*) &g_unicard.runtime_check_on_connect );

    /* Který FDC řadič Unicard API spravuje (0=none, 1=FDC0, 2=FDC1).
     * Default FDC0 = zpětně kompatibilní (Unicard dosud spravoval primární
     * FDC). Pole je int, UNSIGNED handler zapisuje 4 B (hodnoty 0..2). */
    g_elm_managed_fdc = cfgmodule_register_new_element ( cmod, "managed_fdc", CFGENTYPE_UNSIGNED, UNICARD_MANAGED_FDC_FDC0 );
    cfgelement_set_handlers ( g_elm_managed_fdc, (void*) &g_unicard.managed_fdc, (void*) &g_unicard.managed_fdc );

    cfgmodule_parse ( cmod );
    /* Propaguj hodnoty z INI do target proměnných (g_unicard.*).
     * Existující connected/readonly to obchází manuálně (viz níže),
     * pro nové fw_version/runtime_check_on_connect to děláme cestou
     * standardní cfgmodule_propagate (= projde všechny elementy s
     * propagate_value_handler). */
    cfgmodule_propagate ( cmod );

    /* Sanity: uc1 firmware existuje jen pro MZ-800. Pokud má INI soubor
     * z předchozího běhu nastavené UC1 na jiné platformě (např. uživatel
     * přepnul mzarch), force UC3. */
    if ( g_mzarch_platform_numeric != 800 && g_unicard.fw_emulated == UNICARD_FW_UC1 ) {
        g_unicard.fw_emulated = UNICARD_FW_UC3;
    }

    en_UNICARD_CONNECTION conn = cfgelement_get_bool_value ( g_elm_connected );
    unicard_set_connected ( conn );

    if ( !UNICARD_TEST_IS_CONNECTED ) qdisk_deactivate_unicard_boot_loader ( );
}


void unicard_exit ( void ) {
    unimgr_exit ( );
    baseui_tools_mem_free ( g_unicard.work_dir );
}


void unicard_reset ( void ) {
    if ( !UNICARD_TEST_IS_CONNECTED ) return;
    unimgr_reset ( );

    /* QD bootstrap remount na machine reset:
     * Pokud je QD nakonfigurován jako Unicard bootstrap (type=UNICARD),
     * znovu otevře QD image. Důvod: typický scénář je, že emu při startu
     * nenašel {SD}/unicard/mzfloader.mzq a namountoval SDERR fallback;
     * uživatel mezitím soubor do SD doplnil a očekává, že po stisknutí
     * Reset se začne bootovat z reálného mzfloader.mzq, ne stále z SDERR.
     * qdisk_close + qdisk_open re-evaluuje existenci souboru a vybere
     * příslušný mount (file nebo fallback). */
    if ( QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_UNICARD ) {
        qdisk_close ( );
        qdisk_open ( );
    };
}


void unicard_write_byte ( uint16_t port, uint8_t data ) {
    switch ( port & 0xff ) {
        case 0x50:
        case 0x51:
            unimgr_write_byte ( port & 0x01, data );
            break;
        default:
            fprintf ( stderr, "%s():%d - Unsupported Unicard port 0x%02x\n", __func__, __LINE__, port );
    };
}


uint8_t unicard_read_byte ( uint16_t port ) {
    uint8_t ret = 0x00;

    switch ( port & 0xff ) {
        case 0x50:
        case 0x51:
            ret = unimgr_read_byte ( port & 0x01 );
            break;
        default:
            fprintf ( stderr, "%s():%d - Unsupported Unicard port 0x%02x\n", __func__, __LINE__, port );
    };
    return ret;
}


void unicard_read_rtc ( st_UNICARD_RTC *rtc ) {
    GTimeZone *tz = g_time_zone_new_local ( );
    GDateTime *datetime = g_date_time_new_now ( tz );
    rtc->year = g_date_time_get_year ( datetime );
    rtc->month = g_date_time_get_month ( datetime );
    rtc->day = g_date_time_get_day_of_month ( datetime );
    rtc->hour = g_date_time_get_hour ( datetime );
    rtc->min = g_date_time_get_minute ( datetime );
    rtc->sec = g_date_time_get_second ( datetime );
    g_time_zone_unref ( tz );
    g_date_time_unref ( datetime );
}


/**
 * Normalizace emu-supplied cesty s detekcí pokusu o escape ze SD root.
 *
 * Provádí standardní redukci cesty:
 *   - sjednocení '\\' a '/' na '/' (Windows-style backslash sanitization)
 *   - prepend work_dir pokud cesta nezačíná '/'
 *   - normalizace ".." (popne poslední komponentu)
 *   - normalizace "." a "//"
 *
 * Navíc: pokud parsování spotřebuje víc ".." segmentů, než kolik je
 * v cestě reálných komponent (= path by vedla mimo SD root), nastaví
 * *escape_detected = 1 a vrátí už normalizovaný výsledek (clamped na
 * '/' = SD root) - tj. funkce sama nedělá NULL návrat, jen příznak.
 * Vyhodnocení a logování dělá až unicard_jail_resolve_with_naked.
 *
 * @param emu_path           Vstupní cesta z emulovaného programu.
 * @param[out] escape_detected  1 pokud byla zachycena ".." escape sekvence,
 *                              0 jinak. Caller MUSÍ inicializovat na 0.
 * @return  Naked cesta (vede vždy zpod '/'), caller vlastní výsledek.
 */
static char* unicard_normalize_emu_path ( const char *emu_path, int *escape_detected ) {
    char *naked = (char*) baseui_tools_mem_alloc0 ( 2 );
    naked[0] = '/';

    if ( !emu_path || emu_path[0] == 0x00 ) return naked;

    /* Pre-sanitization: backslash -> forward slash. Reálná Unicarta
     * separátor backslash nezná, ale guest SW může omylem poslat
     * Windows-style cestu. Pro účely detekce traversal je nutné mít
     * jediný separator. */
    int in_len = strlen ( emu_path );
    char *input = (char*) baseui_tools_mem_alloc0 ( in_len + 1 );
    for ( int k = 0; k < in_len; k++ ) {
        input[k] = ( emu_path[k] == '\\' ) ? '/' : emu_path[k];
    }

    char *complete_filepath;
    int complete_len;

    if ( input[0] != '/' ) {
        complete_filepath = g_build_filename ( g_unicard.work_dir, input, NULL );
        complete_len = strlen ( complete_filepath );
    } else {
        complete_filepath = (char*) baseui_tools_mem_alloc0 ( in_len + 1 );
        memcpy ( complete_filepath, input, in_len );
        complete_len = in_len;
    }
    baseui_tools_mem_free ( input );

    int real_count_nodes = 0;
    char **real_node = (char**) baseui_tools_mem_alloc ( sizeof ( char* ) );
    int search_start = 1;
    int *path_components = (int*) baseui_tools_mem_alloc0 ( sizeof ( int ) );
    int count_path_components = 0;

    int i;
    for ( i = 0; i <= complete_len; i++ ) {
        if ( complete_filepath[i] == '/' ) {
            complete_filepath[i] = 0x00;
            search_start = 1;
        } else if ( search_start ) {
            if ( complete_filepath[i] == '.' ) {
                int j = i + 1;
                if ( ( complete_filepath[j] == '/' ) || ( complete_filepath[j] == 0x00 ) ) continue;
                if ( complete_filepath[j] == '.' ) {
                    int kk = j + 1;
                    if ( ( complete_filepath[kk] == '/' ) || ( complete_filepath[kk] == 0x00 ) ) {
                        if ( count_path_components ) {
                            count_path_components--;
                        } else {
                            /* Underflow: ".." přes kořen SD root = escape attempt. */
                            *escape_detected = 1;
                        }
                        continue;
                    }
                }
            }
            if ( strlen ( &complete_filepath[i] ) ) {
                search_start = 0;
                real_node = (char**) baseui_tools_mem_realloc ( real_node, sizeof ( char** ) * ( real_count_nodes + 1 ) );
                real_node[real_count_nodes] = &complete_filepath[i];
                path_components = (int*) baseui_tools_mem_realloc ( path_components, sizeof ( int ) * ( count_path_components + 1 ) );
                path_components[count_path_components++] = real_count_nodes;
                real_count_nodes++;
            }
        }
    }

    if ( count_path_components ) {
        /* Seed naked[0]='/' z inicializace zahodit: smyčka níže přidává
         * '/' PŘED každou komponentu, takže ponechaný seed by způsobil
         * vedoucí dvojité lomítko u nekořenové cesty (= "/foo" -> "//foo").
         * Reálná Unikarta (uc1 i uc3 přes FatFS f_getcwd) vrací single
         * slash, proto naked stavíme bez úvodního seedu. */
        naked[0] = 0x00;
        int naked_len = 1;
        for ( i = 0; i < count_path_components; i++ ) {
            int j = path_components[i];
            naked_len += strlen ( real_node[j] ) + 1 + 1;
            naked = (char*) baseui_tools_mem_realloc ( naked, naked_len );
            strncat ( naked, "/", naked_len );
            strncat ( naked, real_node[j], naked_len );
        }
    } else {
        /* Žádné komponenty (= SD root) - naked zůstává "/" z inicializace. */
    }

    g_free ( complete_filepath );
    g_free ( path_components );
    g_free ( real_node );

    return naked;
}


char* unicard_jail_resolve_with_naked ( const char *emu_path, char **naked_out ) {
    if ( naked_out ) *naked_out = NULL;

    int escape = 0;
    char *naked = unicard_normalize_emu_path ( emu_path, &escape );

    if ( escape ) {
        g_warning ( "Unicard SD jail violation: emu tried to access '%s' (escapes SD root '%s')",
                    emu_path ? emu_path : "(null)", unicard_get_sd_root_dirpath ( ) );
        baseui_tools_mem_free ( naked );
        return NULL;
    }

    char *full = g_build_filename ( unicard_get_sd_root_dirpath ( ), naked, NULL );

    if ( naked_out ) {
        *naked_out = naked;
    } else {
        baseui_tools_mem_free ( naked );
    }
    return full;
}


char* unicard_jail_resolve ( const char *emu_path ) {
    return unicard_jail_resolve_with_naked ( emu_path, NULL );
}


int unicard_jail_check_inside ( const char *naked_path ) {
    if ( !naked_path ) return 0;
    /* Naked path je výstup normalizace - nezačíná-li '/', je něco
     * špatně; pokud obsahuje "/.." segment, je to escape (normalizace
     * by ho měla absorbovat, tohle je pouze pojistka). */
    if ( naked_path[0] != '/' ) return 0;
    const char *p = naked_path;
    while ( ( p = strstr ( p, "/.." ) ) != NULL ) {
        char c = p[3];
        if ( c == '/' || c == 0x00 ) return 0;
        p++;
    }
    return 1;
}


void unicard_set_readonly ( int readonly ) {
    int prev = g_unicard.readonly;
    g_unicard.readonly = readonly ? 1 : 0;
    /* Reset per-session warning counteru při změně stavu, aby uživatel
     * dostal info po opakovaném zapnutí. */
    g_unicard_ro_warned = 0;
    if ( !prev && g_unicard.readonly ) {
        g_message ( "Unicard: SD card switched to read-only mode (write attempts will be blocked)" );
    } else if ( prev && !g_unicard.readonly ) {
        g_message ( "Unicard: SD card switched to read-write mode" );
    }
}

en_UNICARD_MANAGED_FDC unicard_get_managed_fdc ( void ) {
    return ( en_UNICARD_MANAGED_FDC ) g_unicard.managed_fdc;
}

void unicard_set_managed_fdc ( en_UNICARD_MANAGED_FDC managed_fdc ) {
    /* Nastav pole; persistence do INI proběhne přes SAVE handler cfg
     * elementu (čte g_unicard.managed_fdc při shutdown). Ovlivní jen
     * budoucí FDD operace - aktuální mounty zůstávají. */
    g_unicard.managed_fdc = ( int ) managed_fdc;
}


int unicard_test_readonly ( void ) {
    return g_unicard.readonly ? 1 : 0;
}


void unicard_warn_readonly_blocked ( void ) {
    if ( !g_unicard_ro_warned ) {
        g_warning ( "Unicard SD is read-only: write attempts blocked. Further warnings suppressed for this session." );
        g_unicard_ro_warned = 1;
    }
}


FRESULT unicard_chdir ( char *dirpath ) {

    //printf ( "CHDIR: '%s'\n", dirpath );

    char *naked_dirpath = NULL;
    char *full_dirpath = unicard_jail_resolve_with_naked ( dirpath, &naked_dirpath );
    if ( !full_dirpath ) return FR_NO_PATH; /* jail escape */

    char *full_dirpath_locale = baseui_tools_file_name_locale_from_utf8 ( full_dirpath );

    if ( !g_file_test ( full_dirpath, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR ) ) {
        fprintf ( stderr, "%s():%d - Error: dirpath '%s'\n", __func__, __LINE__, full_dirpath_locale );
        baseui_tools_mem_free ( naked_dirpath );
        baseui_tools_mem_free ( full_dirpath );
        baseui_tools_mem_free ( full_dirpath_locale );
        return FR_NO_PATH;
    };
    baseui_tools_mem_free ( full_dirpath );
    baseui_tools_mem_free ( full_dirpath_locale );

    /* Uvolnění předchozího work_dir - alokovaný v unicard_init nebo
     * předchozím chdir. */
    if ( g_unicard.work_dir ) baseui_tools_mem_free ( g_unicard.work_dir );
    g_unicard.work_dir = naked_dirpath;

    return FR_OK;
}


int unicard_get_cwd ( FRESULT *ff_res, char *buff, int buff_size ) {
    // na skutecne unikarte je potreba z vystupu getcwd odstranit prvni 2 znaky "0:" 
    // *ff_res = f_getcwd ( (char*) buff_local, buff_size );
    *ff_res = FR_OK;
    int len = strlen ( g_unicard.work_dir );
    if ( len < buff_size ) {
        memcpy ( buff, g_unicard.work_dir, len + 1 );
        return EXIT_SUCCESS;
    };
    return EXIT_FAILURE;
}


void unicard_dir_init ( st_UNICARD_DIR *dir ) {
    dir->dh = NULL;
    dir->dirpath = NULL;
    dir->forced_first_dir = NULL;
    dir->sfn_ctx.assigned = NULL;  /* bezpečný clear před prvním open */
}


FRESULT unicard_dir_close ( st_UNICARD_DIR *dir ) {
    if ( dir->dh != NULL ) {
        g_dir_close ( dir->dh );
        dir->dh = NULL;
        baseui_tools_mem_free ( dir->dirpath );
        dir->dirpath = NULL;
    };
    if ( dir->forced_first_dir != NULL ) {
        g_free ( dir->forced_first_dir );
        dir->forced_first_dir = NULL;
    };
    unicard_sfn_ctx_clear ( &dir->sfn_ctx );
    return FR_OK;
}


FRESULT unicard_dir_open ( st_UNICARD_DIR *dir, char *dirpath ) {

    if ( dir->dh != NULL ) unicard_dir_close ( dir );

    char *naked_dirpath = NULL;
    char *full_dirpath = unicard_jail_resolve_with_naked ( dirpath, &naked_dirpath );
    if ( !full_dirpath ) return FR_NO_PATH; /* jail escape */

    char *full_dirpath_locale = baseui_tools_file_name_locale_from_utf8 ( full_dirpath );

    if ( !g_file_test ( full_dirpath, G_FILE_TEST_IS_DIR ) ) {
        fprintf ( stderr, "%s():%d - This is not DIR '%s'\n", __func__, __LINE__, full_dirpath_locale );
        baseui_tools_mem_free ( full_dirpath );
        baseui_tools_mem_free ( full_dirpath_locale );
        baseui_tools_mem_free ( naked_dirpath );
        return FR_NO_PATH;
    };

    GError* err = NULL;
    dir->dh = g_dir_open ( full_dirpath, 0, &err );

    if ( err != NULL ) {
        fprintf ( stderr, "%s():%d - Error: %s\n", __func__, __LINE__, err->message );
        g_error_free ( err );
        baseui_tools_mem_free ( full_dirpath );
        baseui_tools_mem_free ( full_dirpath_locale );
        baseui_tools_mem_free ( naked_dirpath );
        return FR_DISK_ERR;
    };

    dir->dirpath = naked_dirpath;
    dir->position = 0;
    dir->forced_first_dir = NULL;

    /* Workaround pro bug v Unicard manager V2.11b - viz komentář u
     * `forced_first_dir` v unicard.h. Aplikuje se jen na root listing
     * v uc3 simulaci. Prescan adresář, najdi první entry. Pokud je file
     * a v adresáři existuje pozdější dir, ulož jeho jméno -> v read
     * funkci ho emitneme jako PRVNÍ. Bug v V2.11b sortu se tím obejde
     * (manager dostane filelist už ve formátu "dir first"). */
    int is_root = ( dir->dirpath[0] == 0x00 ) || ( 0 == strcmp ( dir->dirpath, "/" ) );
    if ( is_root && unicard_get_fw ( ) == UNICARD_FW_UC3 ) {
        const gchar *first_name = g_dir_read_name ( dir->dh );
        if ( first_name != NULL ) {
            gchar *first_full = g_build_filename ( full_dirpath, first_name, NULL );
            int first_is_dir = g_file_test ( first_full, G_FILE_TEST_IS_DIR );
            g_free ( first_full );

            if ( !first_is_dir ) {
                /* První entry je file. Hledej první dir. */
                const gchar *next_name;
                while ( ( next_name = g_dir_read_name ( dir->dh ) ) != NULL ) {
                    gchar *next_full = g_build_filename ( full_dirpath, next_name, NULL );
                    int next_is_dir = g_file_test ( next_full, G_FILE_TEST_IS_DIR );
                    g_free ( next_full );
                    if ( next_is_dir ) {
                        dir->forced_first_dir = g_strdup ( next_name );
                        printf ( "Unicard root listing workaround (V2.11b sort bug): "
                                 "forced first dir = '%s'\n", next_name );
                        break;
                    }
                }
            }
        }

        /* GDir byl prescanován (position posunutá). Reopen aby další
         * read_filelist/filinfo začaly od začátku. */
        g_dir_close ( dir->dh );
        dir->dh = g_dir_open ( full_dirpath, 0, &err );
        if ( err != NULL ) {
            fprintf ( stderr, "%s():%d - Reopen failed: %s\n", __func__, __LINE__, err->message );
            g_error_free ( err );
            baseui_tools_mem_free ( full_dirpath );
            baseui_tools_mem_free ( full_dirpath_locale );
            /* dir->dirpath ponechán - close zavolá free */
            unicard_dir_close ( dir );
            return FR_DISK_ERR;
        }
    }

    baseui_tools_mem_free ( full_dirpath );
    baseui_tools_mem_free ( full_dirpath_locale );

    /* Kolizní kontext pro ~N SFN aliasy - čistý start pro tento listing.
     * Buduje se inkrementálně v read_filinfo v pořadí iterace. */
    unicard_sfn_ctx_init ( &dir->sfn_ctx );

    return FR_OK;
}


FRESULT unicard_dir_read_filelist ( st_UNICARD_DIR *dir, char *buff, int buff_size ) {

    if ( dir->dh == NULL ) return FR_DISK_ERR;

    /* Synteticky pridana ".." polozka jako prvni radek non-root listu.
     *
     * Na realne unicard (FatFS) f_readdir vraci ".." automaticky.
     * Host filesystem pres GLib g_dir_read_name tuto polozku NEvraci -
     * kompenzujeme synteticky.
     *
     * uc1 (manager V2.4): ocekava ".." prislou od FW. Bez ni nema Back.
     * uc3 (manager V2.11b): pridava si ".." sam (novejsi UX). Pokud
     *   bychom ji poslali, klient by ji videl 2x (jednu z emu, jednu
     *   svou vlastni).
     *
     * Conditional na aktualnim FW - per-verzi spravne chovani. */
    if ( ( dir->position == 0 ) &&
         ( !( ( dir->dirpath[0] == 0x00 ) || ( 0 == strcmp ( dir->dirpath, "/" ) ) ) ) &&
         ( unicard_get_fw ( ) == UNICARD_FW_UC1 ) ) {
        dir->position = 1;
        snprintf ( buff, buff_size, "../%c0%c", 0x0d, 0x0d );
        return FR_OK;
    };

    /* Workaround pro V2.11b sort bug (viz st_UNICARD_DIR.forced_first_dir
     * v unicard.h): při position == 0 a forced_first_dir != NULL emitneme
     * ten dir jako PRVNÍ entry, aby root listing začal dirí. */
    if ( ( dir->position == 0 ) && ( dir->forced_first_dir != NULL ) ) {
        dir->position++;
        snprintf ( buff, buff_size, "%s/%c0%c", dir->forced_first_dir, 0x0d, 0x0d );
        return FR_OK;
    };

    const gchar *filename;
    int length;

    do {
        filename = g_dir_read_name ( dir->dh );

        if ( filename == NULL ) {
            buff[0] = 0x00;
            return FR_OK;
        };

        length = strlen ( filename );

        if ( length >= _MAX_LFN ) {
            fprintf ( stderr, "%s():%d - Filename is too long '%s'\n", __func__, __LINE__, filename );
            continue;
        };

        /* Pokud byl forced_first_dir už emitnut (workaround výše), skip
         * ho v normálním gdir read aby se nezopakoval. */
        if ( ( dir->forced_first_dir != NULL ) &&
             ( 0 == strcmp ( filename, dir->forced_first_dir ) ) ) {
            continue;
        }

        break;
    } while ( 1 );

    dir->position++;

    gchar *filepath = g_build_filename ( unicard_get_sd_root_dirpath ( ), dir->dirpath, filename, NULL );

    if ( g_file_test ( filepath, G_FILE_TEST_IS_DIR ) ) {
        snprintf ( buff, buff_size, "%s/%c0%c", filename, 0x0d, 0x0d );
    } else {
        GStatBuf statbuf;
        g_stat ( filepath, &statbuf );
        snprintf ( buff, buff_size, "%s%c%d%c", filename, 0x0d, (uint32_t) statbuf.st_size, 0x0d );
    };

    g_free ( filepath );
    return FR_OK;
}


/**
 * @brief Mapuje aktuální firmware Unicard na styl plnění offset 9.
 *
 * uc1 (MZ800UKP1, FatFS R0.09) a uc3 (Unicard3, FatFS R0.13a) mají jinou
 * sémantiku pole na guest offsetu 9 - viz en_UNICARD_SFN_STYLE.
 *
 * @return UNICARD_SFN_STYLE_UC1 pro uc1 firmware, jinak UNICARD_SFN_STYLE_UC3
 */
static en_UNICARD_SFN_STYLE unicard_sfn_style_for_fw ( void ) {
    return ( unicard_get_fw ( ) == UNICARD_FW_UC1 )
           ? UNICARD_SFN_STYLE_UC1 : UNICARD_SFN_STYLE_UC3;
}


/**
 * @brief Naplní FILINFO pole na offsetu 23 (a délku na offsetu 22).
 *
 * Sémantika offsetu 23 se liší per firmware (jiná FatFS verze):
 *
 * - uc1 (R0.09): off 23 = `lfname` (LFN). Plněno JEN když má soubor LFN
 *   directory entry (@p has_lfn = NS_LFN: lossy / smíšená velikost /
 *   non-ASCII); pro čistě 8.3 jména prázdné, lfn_strlen = 0.
 * - uc3 (R0.13a): off 23 = `fname` (primární jméno) - VŽDY vyplněné jménem
 *   souboru, lfn_strlen = jeho délka. (off 9 = altname je naopak prázdné,
 *   když je alias zbytečný.) "Opravený" uc3 - reálný bugový uc3 ho
 *   nekopíruje vůbec.
 *
 * @param finfo    cílová struktura (lfname/lfn_strlen)
 * @param name     jméno (UTF-8), zdroj
 * @param has_lfn  zda jméno potřebuje LFN entry (NS_LFN)
 * @param style    styl plnění (per-firmware)
 */
static void unicard_fill_lfname ( st_UNICARD_FILINFO *finfo, const char *name,
                                  gboolean has_lfn, en_UNICARD_SFN_STYLE style ) {
    /* uc3 plní off 23 vždy (primární jméno); uc1 jen pro jména s LFN. */
    gboolean fill = ( style == UNICARD_SFN_STYLE_UC3 ) ? TRUE : has_lfn;
    if ( fill ) {
        size_t len = strlen ( name );
        size_t lfn_copy = len < ( _MAX_LFN - 1 ) ? len : ( _MAX_LFN - 1 );
        memcpy ( finfo->lfname, name, lfn_copy );
        finfo->lfname[lfn_copy] = '\0';
        finfo->lfn_strlen = (uint8_t) lfn_copy;
    } else {
        finfo->lfname[0] = '\0';
        finfo->lfn_strlen = 0;
    }
}


FRESULT unicard_dir_read_filinfo ( st_UNICARD_DIR *dir, st_UNICARD_FILINFO *finfo, int *eof ) {
    if ( !dir || !finfo || !eof ) return FR_INVALID_PARAMETER;
    *eof = 0;
    memset ( finfo, 0, sizeof ( *finfo ) );

    const en_UNICARD_SFN_STYLE sfn_style = unicard_sfn_style_for_fw ( );

    if ( dir->dh == NULL ) return FR_DISK_ERR;

    const gchar *filename;
    int length;

    /* uc1 (manager V2.4) očekává ".." jako první položku v podadresáři -
     * reálný FW ji posílá. uc3 (manager V2.11b) si ji přidává sám, takže
     * ji NEposíláme (jinak by ji klient viděl 2x). Host adresář ".." nemá,
     * proto ji syntetizujeme. */
    int is_root = ( dir->dirpath[0] == 0x00 ) || ( 0 == strcmp ( dir->dirpath, "/" ) );
    if ( ( dir->position == 0 ) && ( sfn_style == UNICARD_SFN_STYLE_UC1 ) && !is_root ) {
        dir->position++;
        finfo->fsize = 0;
        finfo->fattrib = UNICARD_AM_DIR;
        strcpy ( finfo->fname, ".." );      /* offset 9 = ".." */
        unicard_fill_lfname ( finfo, "..", FALSE, sfn_style );  /* uc1: L prázdné */
        return FR_OK;
    }

    /* Stejný workaround jako v unicard_dir_read_filelist - při position
     * == 0 a forced_first_dir != NULL emitneme ten dir jako FILINFO. */
    if ( ( dir->position == 0 ) && ( dir->forced_first_dir != NULL ) ) {
        dir->position++;
        char *forced_full = g_build_filename ( unicard_get_sd_root_dirpath ( ),
                                                dir->dirpath, dir->forced_first_dir, NULL );
        GStatBuf statbuf;
        int stat_ok = ( g_stat ( forced_full, &statbuf ) == 0 );
        finfo->fsize = 0;
        if ( stat_ok ) {
            unicard_time_to_fat ( statbuf.st_mtime, &finfo->fdate, &finfo->ftime );
        }
        finfo->fattrib = UNICARD_AM_DIR;
        /* fname (offset 9) = 8.3 alias; L/LEN (offset 23/22) jen když má LFN. */
        gboolean has_lfn = FALSE;
        unicard_sfn_make ( dir->forced_first_dir, sfn_style, &dir->sfn_ctx, finfo->fname, &has_lfn );
        unicard_fill_lfname ( finfo, dir->forced_first_dir, has_lfn, sfn_style );
        g_free ( forced_full );
        return FR_OK;
    }

    /* Skip příliš dlouhých LFN + skip forced_first_dir (= už emitnut výše). */
    do {
        filename = g_dir_read_name ( dir->dh );
        if ( filename == NULL ) {
            *eof = 1;
            return FR_OK;
        };
        length = strlen ( filename );
        if ( length >= _MAX_LFN ) {
            fprintf ( stderr, "%s():%d - Filename too long, skip '%s'\n", __func__, __LINE__, filename );
            continue;
        };
        if ( ( dir->forced_first_dir != NULL ) &&
             ( 0 == strcmp ( filename, dir->forced_first_dir ) ) ) {
            continue;
        }
        break;
    } while ( 1 );

    dir->position++;

    char *full = g_build_filename ( unicard_get_sd_root_dirpath ( ), dir->dirpath, filename, NULL );

    GStatBuf statbuf;
    int stat_ok = ( g_stat ( full, &statbuf ) == 0 );

    finfo->fsize = stat_ok ? (uint32_t) statbuf.st_size : 0;
    if ( stat_ok ) {
        unicard_time_to_fat ( statbuf.st_mtime, &finfo->fdate, &finfo->ftime );
    };
    finfo->fattrib = g_file_test ( full, G_FILE_TEST_IS_DIR ) ? UNICARD_AM_DIR : 0;

    /* fname (offset 9) = 8.3 alias; L/LEN (offset 23/22) jen když má LFN. */
    gboolean has_lfn = FALSE;
    unicard_sfn_make ( filename, sfn_style, &dir->sfn_ctx, finfo->fname, &has_lfn );
    unicard_fill_lfname ( finfo, filename, has_lfn, sfn_style );

    g_free ( full );
    return FR_OK;
}


int unicard_dir_is_open ( st_UNICARD_DIR *dir ) {
    if ( dir->dh == NULL ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


void unicard_file_init ( st_UNICARD_FILE *file ) {
    file->fh = NULL;
    file->filepath = NULL;
}


FRESULT unicard_file_close ( st_UNICARD_FILE *file ) {
    //printf ( "%s():%d\n", __func__, __LINE__ );
    if ( file->fh == NULL ) return FR_OK;
    //printf ( "OK - close\n" );
    // na unikarte udelat sync
    fclose ( file->fh );
    file->fh = NULL;
    baseui_tools_mem_free ( file->filepath );
    file->filepath = NULL;
    return FR_OK;
}


FRESULT unicard_file_open ( st_UNICARD_FILE *file, char *filepath, uint8_t fa_mode ) {

    //printf ( "unicard_open_file: '%s', mode: 0x%02x\n", filepath, fa_mode );

    if ( file->fh != NULL ) unicard_file_close ( file );

    if ( !( fa_mode & ( FA_READ | FA_WRITE ) ) ) {
        fa_mode |= FA_READ;
    };

    /* R/O gate: blokovat všechny módy, které by mohly modifikovat FS
     * (write nebo create včetně FA_OPEN_ALWAYS - ten vytvoří soubor
     * pokud neexistuje). FA_READ + FA_OPEN_EXISTING projde. */
    if ( unicard_test_readonly ( ) &&
         ( fa_mode & ( FA_WRITE | FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS ) ) ) {
        unicard_warn_readonly_blocked ( );
        return FR_WRITE_PROTECTED;
    }

    char *naked_filepath = NULL;
    char *full_filepath = unicard_jail_resolve_with_naked ( filepath, &naked_filepath );
    if ( !full_filepath ) return FR_NO_PATH; /* jail escape */

    char *full_filepath_locale = baseui_tools_file_name_locale_from_utf8 ( full_filepath );

    /* Mapování FatFS fa_mode bitů na fopen mode string. Reálná Unikarta
     * (firmware r61, emu_MZFREPO.c:do_cmdOPEN) předává fa_mode přímo do
     * FatFS f_open(), který má atomické cesty pro všechny módy. C stdio
     * tyto cesty atomicky nemá (zejména FA_OPEN_ALWAYS = open existing
     * NEBO create new bez truncate), proto musíme pro FA_OPEN_ALWAYS
     * rozhodovat podle existence souboru.
     *
     * FatFS sémantika módů:
     *  FA_CREATE_NEW    - create new, fail pokud existuje
     *  FA_CREATE_ALWAYS - create new nebo truncate existing (vždy 0 B)
     *  FA_OPEN_ALWAYS   - open existing nebo create new (zachovat obsah)
     *  FA_OPEN_EXISTING - open existing, fail pokud neexistuje (= default) */
    const char *mode;

    if ( fa_mode & FA_CREATE_NEW ) {
        /* "Fail pokud existuje" se řeší explicitním g_file_test níže
         * (FR_EXIST). Mode string je čistý "wb"/"w+b" bez 'x' flag -
         * legacy msvcrt.dll (proti které linkujeme i v UCRT64 buildu)
         * 'x' flag NEPODPORUJE a fopen vrací EINVAL i pro neexistující
         * soubor. Manuální existence check je race-prone, ale Unicard
         * nemá konkurenční přístup k SD. */
        if ( fa_mode & FA_READ ) {
            mode = "w+b"; // RW
        } else {
            mode = "wb"; // W
        };
#if 0 // nemame v unikarte implementovano
    } else if ( fa_mode & FA_OPEN_APPEND ) {
        if ( fa_mode & FA_READ ) {
            mode = "a+b"; // RW
        } else {
            mode = "ab"; // W
        };
#endif
    } else if ( fa_mode & FA_CREATE_ALWAYS ) {
        /* Vždy začít s prázdným obsahem - fopen "wb"/"w+b" vytvoří
         * nový nebo zkrátí existující na 0 B. */
        if ( fa_mode & FA_READ ) {
            mode = "w+b"; // RW
        } else {
            mode = "wb"; // W
        };
    } else if ( fa_mode & FA_OPEN_ALWAYS ) {
        /* Existence-aware: pokud soubor existuje, otevřít bez truncate
         * ("r+b"); pokud ne, vytvořit nový ("w+b"/"wb"). C stdio nemá
         * jediný atomický mode pro tuto sémantiku. Pro pure-W bez
         * FA_READ použijeme stejně "r+b" - dovoluje write a zachová
         * existující obsah, což je sémantika FA_OPEN_ALWAYS. */
        if ( g_file_test ( full_filepath, G_FILE_TEST_EXISTS ) ) {
            mode = "r+b"; // RW bez truncate
        } else {
            if ( fa_mode & FA_READ ) {
                mode = "w+b"; // RW (create)
            } else {
                mode = "wb"; // W (create)
            };
        };
    } else {
        /* FA_OPEN_EXISTING (= default, fa_mode bez create/open bitů). */
        if ( fa_mode & FA_WRITE ) {
            mode = "r+b"; // RW
        } else {
            mode = "rb"; // R
        };
    };

    // fdc hack
    int fdcfg1_len = strlen ( UNICARD_FDCFG_FILE1 );
    if ( ( 0 == strncmp ( naked_filepath, UNICARD_FDCFG_FILE1, fdcfg1_len ) ) &&
         ( 0 == strcmp ( &naked_filepath[( fdcfg1_len + 1 )], UNICARD_FDCFG_FILE2 ) ) &&
         ( fa_mode == FA_READ ) ) {

        int drive_id = naked_filepath[fdcfg1_len] - '0';
        if ( ( drive_id >= 0 ) && ( drive_id <= 3 ) ) {
            /* Cesta k DSK se čte z FDC spravovaného Unicard API; NONE -> NULL. */
            st_FDC *managed_fdc = unicard_managed_fdc_instance ( );
            const char *dsk_filepath = managed_fdc ? fdc_get_dsk_filepath ( managed_fdc, drive_id ) : NULL;
            FILE *fh = g_fopen ( full_filepath, "wb" );
            if ( ( dsk_filepath ) && dsk_filepath[0] != 0x00 ) {
                int len = strlen ( unicard_get_sd_root_dirpath ( ) );
                if ( 0 == strncmp ( dsk_filepath, unicard_get_sd_root_dirpath ( ), len ) ) {
                    fprintf ( fh, "%s\n", &dsk_filepath[len] );
                } else {
                    char *basename = g_path_get_basename ( dsk_filepath );
                    fprintf ( fh, "***mz800emu path*** '%s'\n", basename );
                    baseui_tools_mem_free ( basename );
                };
            };
            fclose ( fh );
        };
    };

    /* Existence kontrola - msvcrt.dll fopen nemá ekvivalent FatFS sémantiky
     * pro FA_CREATE_NEW (fail-if-exists) ani pro FA_OPEN_EXISTING (fail-if-
     * not-exists v rb/r+b módu - "rb" sice fail-if-not-exists má, ale chceme
     * vlastní FR_NO_FILE z hlediska Unikarty). FA_CREATE_ALWAYS a FA_OPEN_ALWAYS
     * akceptují obě existence stavy a mode už byl vybrán podle nich výše. */
    int file_exists = g_file_test ( full_filepath, G_FILE_TEST_EXISTS );
    if ( ( fa_mode & FA_CREATE_NEW ) && file_exists ) {
        fprintf ( stderr, "%s():%d - File already exists '%s'\n", __func__, __LINE__, full_filepath_locale );
        baseui_tools_mem_free ( full_filepath );
        baseui_tools_mem_free ( full_filepath_locale );
        baseui_tools_mem_free ( naked_filepath );
        return FR_EXIST;
    };
    int require_existing = !( fa_mode & ( FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS ) );
    if ( require_existing && !file_exists ) {
        fprintf ( stderr, "%s():%d - File not exists '%s'\n", __func__, __LINE__, full_filepath_locale );
        baseui_tools_mem_free ( full_filepath );
        baseui_tools_mem_free ( full_filepath_locale );
        baseui_tools_mem_free ( naked_filepath );
        return FR_NO_FILE;
    };

    file->fh = g_fopen ( full_filepath, mode );
    if ( !file->fh ) {
        fprintf ( stderr, "%s():%d - Cant open: %s, in mode '%s'\n", __func__, __LINE__, full_filepath_locale, mode );
        baseui_tools_mem_free ( full_filepath );
        baseui_tools_mem_free ( full_filepath_locale );
        baseui_tools_mem_free ( naked_filepath );
        return FR_DISK_ERR;
    };

    baseui_tools_mem_free ( full_filepath );
    baseui_tools_mem_free ( full_filepath_locale );
    file->filepath = naked_filepath;

    return FR_OK;
}


FRESULT unicard_file_read ( st_UNICARD_FILE *file, uint8_t *buff, uint32_t buff_size, uint32_t *read_len ) {
    int ret = fs_layer_fread ( &file->fh, buff, buff_size, read_len );
    if ( ( ret == FS_LAYER_FR_OK ) || ( feof ( file->fh ) ) ) return FR_OK;
    return FR_DISK_ERR;
}


FRESULT unicard_file_write ( st_UNICARD_FILE *file, const uint8_t *buff, uint32_t buff_size, uint32_t *write_len ) {
    if ( !file || !buff || !write_len ) return FR_INVALID_PARAMETER;
    *write_len = 0;
    if ( file->fh == NULL ) return FR_INVALID_PARAMETER;

    /* R/O gate. Soubor sice byl otevřen s povoleným write (jinak by
     * file_open vrátil error), ale uživatel mohl R/O zapnout až po
     * open - zde zachytíme. */
    if ( unicard_test_readonly ( ) ) {
        unicard_warn_readonly_blocked ( );
        return FR_WRITE_PROTECTED;
    }

    int ret = fs_layer_fwrite ( &file->fh, (uint8_t*) buff, buff_size, write_len );
    if ( ret != FS_LAYER_FR_OK ) return FR_DISK_ERR;
    return FR_OK;
}


FRESULT unicard_file_sync ( st_UNICARD_FILE *file ) {
    if ( !file || file->fh == NULL ) return FR_INVALID_PARAMETER;
    if ( fflush ( file->fh ) != 0 ) return FR_DISK_ERR;
    /* OS-level sync: na Windows _commit, na POSIX fsync. Zde stačí
     * fflush - guest SW očekává konzistenci stavu file po cmdSYNC,
     * ne nutně flush kernel cache na disk. Pokud bude potřeba tvrdší
     * záruka, doplnit _commit/fsync. */
    return FR_OK;
}


#ifdef _WIN32
#include <io.h>     /* _chsize_s, _fileno */
#else
#include <unistd.h> /* ftruncate */
#endif

FRESULT unicard_file_truncate ( st_UNICARD_FILE *file ) {
    if ( !file || file->fh == NULL ) return FR_INVALID_PARAMETER;
    if ( unicard_test_readonly ( ) ) {
        unicard_warn_readonly_blocked ( );
        return FR_WRITE_PROTECTED;
    }
    /* Před truncate flush, aby pending writes neztratily konzistenci. */
    if ( fflush ( file->fh ) != 0 ) return FR_DISK_ERR;
    long pos = ftell ( file->fh );
    if ( pos < 0 ) return FR_DISK_ERR;

#ifdef _WIN32
    int fd = _fileno ( file->fh );
    if ( fd < 0 ) return FR_DISK_ERR;
    if ( _chsize_s ( fd, pos ) != 0 ) return FR_DISK_ERR;
#else
    int fd = fileno ( file->fh );
    if ( fd < 0 ) return FR_DISK_ERR;
    if ( ftruncate ( fd, pos ) != 0 ) return FR_DISK_ERR;
#endif

    return FR_OK;
}


int unicard_file_is_open ( st_UNICARD_FILE *file ) {
    if ( file->fh == NULL ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


int unicard_file_is_eof ( st_UNICARD_FILE *file ) {
    if ( file->fh == NULL ) return EXIT_FAILURE;
    if ( feof ( file->fh ) == 0 ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * Aktuální pozice v otevřeném souboru.
 *
 * Pokud soubor není otevřený, nebo ftell vrátí -1, vrací 0.
 */
uint32_t unicard_file_tell ( st_UNICARD_FILE *file ) {
    if ( file->fh == NULL ) return 0;
    long pos = ftell ( file->fh );
    if ( pos < 0 ) return 0;
    return (uint32_t) pos;
}


/**
 * Změna pozice v otevřeném souboru, 4 módy z firmware do_cmdSEEK().
 *
 * V mode 1 (END) firmware počítá `f_size - offset` - tj. offset je
 * vzdálenost OD KONCE směrem k začátku. mode=1, offset=0 → konec
 * souboru.
 */
FRESULT unicard_file_seek ( st_UNICARD_FILE *file, uint8_t mode, uint32_t offset ) {
    if ( file->fh == NULL ) return FR_INVALID_PARAMETER;

    long target = 0;
    switch ( mode ) {
        case 0: /* SET */
            target = (long) offset;
            break;
        case 1: { /* END - vzdálenost od konce */
            uint32_t sz = unicard_file_size ( file );
            target = (long) sz - (long) offset;
            if ( target < 0 ) target = 0;
            break;
        }
        case 2: { /* REL_UP - relativně dopředu */
            long cur = ftell ( file->fh );
            if ( cur < 0 ) return FR_DISK_ERR;
            target = cur + (long) offset;
            break;
        }
        case 3: { /* REL_DOWN - relativně dozadu */
            long cur = ftell ( file->fh );
            if ( cur < 0 ) return FR_DISK_ERR;
            target = cur - (long) offset;
            if ( target < 0 ) target = 0;
            break;
        }
        default:
            return FR_INVALID_PARAMETER;
    }

    if ( fseek ( file->fh, target, SEEK_SET ) != 0 ) return FR_DISK_ERR;
    return FR_OK;
}


/**
 * Velikost otevřeného souboru.
 *
 * Implementováno přes fseek SEEK_END + ftell + fseek zpět na původní
 * pozici. Vrací 0 pokud soubor není otevřený nebo některá fseek/ftell
 * operace selže (i když je v praxi soubor přístupný a velikost > 0,
 * tím to nepůjde rozlišit - status err vrací volající přes ff_res).
 */
uint32_t unicard_file_size ( st_UNICARD_FILE *file ) {
    if ( file->fh == NULL ) return 0;
    long orig = ftell ( file->fh );
    if ( orig < 0 ) return 0;
    if ( fseek ( file->fh, 0, SEEK_END ) != 0 ) return 0;
    long sz = ftell ( file->fh );
    /* vrátit pointer zpět; pokud selže, dál to není fatální pro volajícího */
    fseek ( file->fh, orig, SEEK_SET );
    if ( sz < 0 ) return 0;
    return (uint32_t) sz;
}


/**
 * Konverze unix time_t na FAT date+time word páru.
 *
 * FAT date format:  (year-1980)<<9 | month<<5 | day
 * FAT time format:  hour<<11 | min<<5 | sec/2
 *
 * Při time_t < epochy 1980-01-01 nebo > 2107 vrací 1980-01-01 00:00:00.
 */
static void unicard_time_to_fat ( time_t t, uint16_t *fdate, uint16_t *ftime ) {
    struct tm tm_buf;
#ifdef _WIN32
    struct tm *tm_ptr = localtime ( &t );
    if ( !tm_ptr ) {
        *fdate = ( ( 1980 - 1980 ) << 9 ) | ( 1 << 5 ) | 1;
        *ftime = 0;
        return;
    }
    tm_buf = *tm_ptr;
#else
    if ( !localtime_r ( &t, &tm_buf ) ) {
        *fdate = ( ( 1980 - 1980 ) << 9 ) | ( 1 << 5 ) | 1;
        *ftime = 0;
        return;
    }
#endif
    int year = tm_buf.tm_year + 1900;
    if ( year < 1980 ) year = 1980;
    if ( year > 2107 ) year = 2107;

    *fdate = (uint16_t) ( ( ( year - 1980 ) << 9 ) | ( ( tm_buf.tm_mon + 1 ) << 5 ) | tm_buf.tm_mday );
    *ftime = (uint16_t) ( ( tm_buf.tm_hour << 11 ) | ( tm_buf.tm_min << 5 ) | ( tm_buf.tm_sec / 2 ) );
}


/**
 * @brief Spočítá FatFS 8.3 alias souboru @p basename v adresáři @p dir_full.
 *
 * Projde hostitelský adresář @p dir_full ve stejném pořadí jako cmdREADDIR
 * a inkrementálně buduje kolizní kontext, takže ~N číslo aliasu @p basename
 * odpovídá tomu, co vrátí READDIR téhož adresáře. Když adresář nejde projít
 * nebo @p basename není mezi položkami (např. STAT na samotný adresář),
 * spadne na výpočet bez kolizního kontextu.
 *
 * @param dir_full   hostitelská cesta k adresáři (UTF-8)
 * @param basename   hledané jméno (UTF-8)
 * @param style        styl plnění offset 9 (per-firmware)
 * @param out          výstup min. 13 bajtů; NUL-terminovaný 8.3 alias nebo ""
 * @param out_has_lfn  volitelný (může být NULL); TRUE pokud má soubor LFN
 *
 * @pre dir_full, basename, out != NULL.
 * @post out obsahuje NUL-terminovaný řetězec délky 0..12.
 */
static void unicard_compute_sfn_in_dir ( const char *dir_full, const char *basename,
                                         en_UNICARD_SFN_STYLE style, char out[13],
                                         gboolean *out_has_lfn ) {
    out[0] = '\0';
    if ( out_has_lfn ) *out_has_lfn = FALSE;

    st_UNICARD_SFN_CTX ctx;
    unicard_sfn_ctx_init ( &ctx );

    GError *err = NULL;
    GDir *dh = g_dir_open ( dir_full, 0, &err );
    if ( dh != NULL ) {
        const gchar *name;
        while ( ( name = g_dir_read_name ( dh ) ) != NULL ) {
            char tmp[13];
            gboolean hl = FALSE;
            unicard_sfn_make ( name, style, &ctx, tmp, &hl );
            if ( 0 == strcmp ( name, basename ) ) {
                memcpy ( out, tmp, sizeof ( tmp ) );
                if ( out_has_lfn ) *out_has_lfn = hl;
                g_dir_close ( dh );
                unicard_sfn_ctx_clear ( &ctx );
                return;
            }
        }
        g_dir_close ( dh );
    }
    if ( err != NULL ) g_error_free ( err );
    unicard_sfn_ctx_clear ( &ctx );

    /* Fallback: alias bez kolizního kontextu. */
    st_UNICARD_SFN_CTX one;
    unicard_sfn_ctx_init ( &one );
    unicard_sfn_make ( basename, style, &one, out, out_has_lfn );
    unicard_sfn_ctx_clear ( &one );
}


FRESULT unicard_stat ( const char *path, st_UNICARD_FILINFO *finfo ) {
    if ( !finfo ) return FR_INVALID_PARAMETER;
    memset ( finfo, 0, sizeof ( *finfo ) );

    char *naked_path = NULL;
    char *full_path = unicard_jail_resolve_with_naked ( path, &naked_path );
    if ( !full_path ) return FR_NO_FILE; /* jail escape */

    if ( !g_file_test ( full_path, G_FILE_TEST_EXISTS ) ) {
        baseui_tools_mem_free ( naked_path );
        baseui_tools_mem_free ( full_path );
        return FR_NO_FILE;
    };

    GStatBuf statbuf;
    if ( g_stat ( full_path, &statbuf ) != 0 ) {
        baseui_tools_mem_free ( naked_path );
        baseui_tools_mem_free ( full_path );
        return FR_DISK_ERR;
    };

    finfo->fsize = (uint32_t) statbuf.st_size;
    unicard_time_to_fat ( statbuf.st_mtime, &finfo->fdate, &finfo->ftime );
    finfo->fattrib = g_file_test ( full_path, G_FILE_TEST_IS_DIR ) ? UNICARD_AM_DIR : 0;

    /* Basename pro fname (8.3 alias) a lfname. */
    char *basename = g_path_get_basename ( naked_path );

    /* fname (offset 9) = FatFS 8.3 alias konzistentní s cmdREADDIR téhož
     * adresáře. Projde parent dir kvůli správnému ~N koliznímu číslu;
     * vrací i has_lfn pro plnění pole LFN (offset 23/22). */
    char *parent_full = g_path_get_dirname ( full_path );
    gboolean has_lfn = FALSE;
    unicard_compute_sfn_in_dir ( parent_full, basename, unicard_sfn_style_for_fw ( ),
                                 finfo->fname, &has_lfn );
    g_free ( parent_full );

    /* L/LEN (offset 23/22): uc3 vždy (primární jméno), uc1 jen když má LFN. */
    unicard_fill_lfname ( finfo, basename, has_lfn, unicard_sfn_style_for_fw ( ) );

    g_free ( basename );
    baseui_tools_mem_free ( naked_path );
    baseui_tools_mem_free ( full_path );
    return FR_OK;
}


#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

FRESULT unicard_get_volume_info ( uint32_t *total_sect, uint32_t *free_sect ) {
    if ( !total_sect || !free_sect ) return FR_INVALID_PARAMETER;
    *total_sect = 0;
    *free_sect = 0;

    char *sd_root = unicard_get_sd_root_dirpath ( );
    if ( !sd_root || sd_root[0] == 0x00 ) return FR_DISK_ERR;

#ifdef _WIN32
    ULARGE_INTEGER free_avail, total_bytes, total_free;
    char *sd_root_locale = baseui_tools_file_name_locale_from_utf8 ( sd_root );
    BOOL ok = GetDiskFreeSpaceExA ( sd_root_locale, &free_avail, &total_bytes, &total_free );
    g_free ( sd_root_locale );
    if ( !ok ) return FR_DISK_ERR;

    /* 512B "sektory"; ořezat nad 4 GB pokud volume přesahuje uint32_t. */
    uint64_t tot = total_bytes.QuadPart / 512;
    uint64_t fre = total_free.QuadPart / 512;
    *total_sect = ( tot > 0xFFFFFFFFULL ) ? 0xFFFFFFFFu : (uint32_t) tot;
    *free_sect  = ( fre > 0xFFFFFFFFULL ) ? 0xFFFFFFFFu : (uint32_t) fre;
#else
    struct statvfs vfs;
    if ( statvfs ( sd_root, &vfs ) != 0 ) return FR_DISK_ERR;
    uint64_t tot = ( (uint64_t) vfs.f_blocks * vfs.f_frsize ) / 512;
    uint64_t fre = ( (uint64_t) vfs.f_bavail * vfs.f_frsize ) / 512;
    *total_sect = ( tot > 0xFFFFFFFFULL ) ? 0xFFFFFFFFu : (uint32_t) tot;
    *free_sect  = ( fre > 0xFFFFFFFFULL ) ? 0xFFFFFFFFu : (uint32_t) fre;
#endif

    return FR_OK;
}


FRESULT unicard_mkdir ( const char *path ) {
    if ( !path ) return FR_INVALID_PARAMETER;
    if ( unicard_test_readonly ( ) ) {
        unicard_warn_readonly_blocked ( );
        return FR_WRITE_PROTECTED;
    }
    char *full = unicard_jail_resolve ( path );
    if ( !full ) return FR_NO_PATH; /* jail escape */

    FRESULT ret;
    if ( g_file_test ( full, G_FILE_TEST_EXISTS ) ) {
        ret = FR_EXIST;
    } else if ( g_mkdir ( full, UNICARD_DEFAULT_DIR_MODE ) != 0 ) {
        ret = FR_DISK_ERR;
    } else {
        ret = FR_OK;
    };

    baseui_tools_mem_free ( full );
    return ret;
}


FRESULT unicard_unlink ( const char *path ) {
    if ( !path ) return FR_INVALID_PARAMETER;
    if ( unicard_test_readonly ( ) ) {
        unicard_warn_readonly_blocked ( );
        return FR_WRITE_PROTECTED;
    }
    char *full = unicard_jail_resolve ( path );
    if ( !full ) return FR_NO_FILE; /* jail escape */

    FRESULT ret;
    if ( !g_file_test ( full, G_FILE_TEST_EXISTS ) ) {
        ret = FR_NO_FILE;
    } else if ( g_remove ( full ) != 0 ) {
        /* g_remove selže např. u neprázdného adresáře. */
        ret = FR_DENIED;
    } else {
        ret = FR_OK;
    };

    baseui_tools_mem_free ( full );
    return ret;
}


FRESULT unicard_rename ( const char *old_path, const char *new_path ) {
    if ( !old_path || !new_path ) return FR_INVALID_PARAMETER;
    if ( unicard_test_readonly ( ) ) {
        unicard_warn_readonly_blocked ( );
        return FR_WRITE_PROTECTED;
    }
    char *full_old = unicard_jail_resolve ( old_path );
    if ( !full_old ) return FR_NO_FILE; /* jail escape (zdroj) */
    char *full_new = unicard_jail_resolve ( new_path );
    if ( !full_new ) {
        baseui_tools_mem_free ( full_old );
        return FR_NO_PATH; /* jail escape (cíl) */
    }

    FRESULT ret;
    if ( !g_file_test ( full_old, G_FILE_TEST_EXISTS ) ) {
        ret = FR_NO_FILE;
    } else if ( g_file_test ( full_new, G_FILE_TEST_EXISTS ) ) {
        ret = FR_EXIST;
    } else if ( g_rename ( full_old, full_new ) != 0 ) {
        ret = FR_DENIED;
    } else {
        ret = FR_OK;
    };

    baseui_tools_mem_free ( full_old );
    baseui_tools_mem_free ( full_new );
    return ret;
}


#ifdef _WIN32
#include <sys/utime.h> /* _utime, _utimbuf */
#else
#include <utime.h>     /* utime, utimbuf */
#endif

FRESULT unicard_utime ( const char *path, const uint8_t fat_dt[6] ) {
    if ( !path || !fat_dt ) return FR_INVALID_PARAMETER;
    if ( unicard_test_readonly ( ) ) {
        unicard_warn_readonly_blocked ( );
        return FR_WRITE_PROTECTED;
    }

    /* Validace datumu/času. */
    int day   = fat_dt[0];
    int month = fat_dt[1];
    int year  = fat_dt[2] + 1980;
    int hour  = fat_dt[3];
    int min   = fat_dt[4];
    int sec   = fat_dt[5];

    if ( day < 1 || day > 31 || month < 1 || month > 12 ||
         hour > 23 || min > 59 || sec > 59 ) return FR_INVALID_PARAMETER;

    char *full = unicard_jail_resolve ( path );
    if ( !full ) return FR_NO_FILE; /* jail escape */

    if ( !g_file_test ( full, G_FILE_TEST_EXISTS ) ) {
        baseui_tools_mem_free ( full );
        return FR_NO_FILE;
    };

    struct tm tm_val;
    memset ( &tm_val, 0, sizeof ( tm_val ) );
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon  = month - 1;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min  = min;
    tm_val.tm_sec  = sec;
    tm_val.tm_isdst = -1;

    time_t t = mktime ( &tm_val );
    if ( t == (time_t) -1 ) {
        baseui_tools_mem_free ( full );
        return FR_INVALID_PARAMETER;
    };

    FRESULT ret = FR_OK;
#ifdef _WIN32
    struct _utimbuf utb;
    utb.actime  = t;
    utb.modtime = t;
    char *full_locale = baseui_tools_file_name_locale_from_utf8 ( full );
    if ( _utime ( full_locale, &utb ) != 0 ) ret = FR_DISK_ERR;
    g_free ( full_locale );
#else
    struct utimbuf utb;
    utb.actime  = t;
    utb.modtime = t;
    if ( utime ( full, &utb ) != 0 ) ret = FR_DISK_ERR;
#endif

    baseui_tools_mem_free ( full );
    return ret;
}


FRESULT unicard_chmod ( const char *path, uint8_t attr, uint8_t mask ) {
    /* No-op (viz dokumentace v hlavičce). Validuje že cesta existuje
     * a vrací FR_OK. attr/mask se nepoužívají - hostitelský FS neumí
     * uchovat FAT atributy. */
    (void) attr;
    (void) mask;
    if ( !path ) return FR_INVALID_PARAMETER;

    char *full = unicard_jail_resolve ( path );
    if ( !full ) return FR_NO_FILE; /* jail escape */

    FRESULT ret = g_file_test ( full, G_FILE_TEST_EXISTS ) ? FR_OK : FR_NO_FILE;

    baseui_tools_mem_free ( full );
    return ret;
}


void unicard_fdc_mount ( uint8_t drive_id, char *filepath ) {
    //printf ( "unicard_fdc_mount (FD%d): '%s'\n", drive_id, filepath );

    /* Směruj na FDC řadič spravovaný Unicard API. Pokud je managed_fdc
     * NONE, příkaz se nepropaguje (no-op). Změna managed_fdc ovlivní jen
     * budoucí operace - již namountované obrazy zůstávají beze změny. */
    st_FDC *fdc = unicard_managed_fdc_instance ( );
    if ( !fdc ) {
        return;
    }

    if ( filepath[0] != 0x00 ) {
        char *full_filepath = unicard_jail_resolve ( filepath );
        if ( !full_filepath ) {
            /* jail escape - warning už vystavil unicard_jail_resolve. */
            return;
        }

        char *full_filepath_locale = baseui_tools_file_name_locale_from_utf8 ( full_filepath );

        if ( !g_file_test ( full_filepath, G_FILE_TEST_EXISTS ) ) {
            fprintf ( stderr, "%s():%d - File not exists '%s'\n", __func__, __LINE__, full_filepath_locale );
        } else {
            fdc_mount_dskfile ( fdc, drive_id, full_filepath );
        };
        baseui_tools_mem_free ( full_filepath );
        baseui_tools_mem_free ( full_filepath_locale );
    } else {
        fdc_umount ( fdc, drive_id );
    };
}

static void unicard_ui_select_sd_root_directory_cb(baseui_fchooser_t *fch)
{
    if (fch == NULL)
    {
        return;
    };

    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        return;
    };

    char *dirpath = fch->selected_path;
    if (!dirpath)
        return;

    /* Pokud uživatel vybral stejný adresář jako aktuální root, není co
     * dělat - vyhneme se zbytečnému disruptivnímu reconnectu (analogicky
     * early-return v unicard_set_fw). */
    char *cur_root = unicard_get_sd_root_dirpath ( );
    if ( cur_root && 0 == g_strcmp0 ( cur_root, dirpath ) ) {
        baseui_filechooser_destroy ( fch );
        return;
    }

    /* On-the-fly změna SD root za běhu. Pokud je Unicard připojen,
     * provedeme force disconnect + reconnect - stejný vzor jako
     * unicard_set_fw. Disconnect (unimgr_exit) zavře otevřené file/dir
     * handles starého rootu, reconnect resetuje CWD na "/" (unimgr_init ->
     * unicard_chdir) a re-armne QD boot loader z nového rootu.
     *
     * Bez reconnectu by živý engine zůstal viset na starém rootu
     * (otevřené handles míří do starého adresáře, CWD, QD loader armnutý
     * ze starého mzfloader.mzq). Když Unicard připojen není, jen uložíme
     * nový root - příští manuální Connect ho převezme. */
    int was_connected = UNICARD_TEST_IS_CONNECTED;
    if ( was_connected ) {
        unicard_set_connected ( UNICARD_CONNECTION_DISCONNECTED );
    }

    unicard_set_sd_root_dirpath ( dirpath );

    if ( was_connected ) {
        /* Pokud nový root ještě není inicializovaná SD (adresář neexistuje,
         * nebo existuje ale nemá {SD}/unicard/ runtime - typicky čerstvě
         * vybraný prázdný adresář), vytvoříme runtime stejně jako při
         * prvním startu. set_connected auto-init spouští jen když SAMOTNÝ
         * root adresář neexistuje; prázdný existující adresář by jinak
         * spadl do runtime-mismatch dialogu (NO_DIR) a Unicard by zůstala
         * odpojená. Inicializace fresh karty je bezpodmínečná, shodně se
         * startupem (chybějící root se vytvoří bez ohledu na runtime-check);
         * runtime-check nastavení řídí jen mismatch dialog pro EXISTUJÍCÍ
         * runtime jiné verze - ten necháme na set_connected. */
        char *root = unicard_get_sd_root_dirpath ( );
        int needs_init = ( !g_file_test ( root, G_FILE_TEST_IS_DIR ) )
                         || ( unicard_detect_runtime_fw ( ) == UNICARD_RUNTIME_CHECK_NO_DIR );
        if ( needs_init ) {
            unicard_init_sd_for ( root, g_unicard.fw_emulated );
        }
        unicard_set_connected ( UNICARD_CONNECTION_CONNECTED );
    }

    baseui_filechooser_destroy(fch);
}

void unicard_ui_select_sd_root_directory(void)
{
    baseui_fchooser_t *fch = baseui_filechooser_open_dir(_("Select SD root directory for Unicard"), NULL, NULL, unicard_get_sd_root_dirpath(), unicard_ui_select_sd_root_directory_cb, NULL);
    if (!fch) {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    }
}