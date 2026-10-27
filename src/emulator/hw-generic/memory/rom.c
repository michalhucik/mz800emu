/*
 * File:   rom.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 22. února 2016, 18:30
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rom.h"
#include "memory.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/devbus/devbus.h"

#include "emulator.h"
#include "cfgmain.h"

#include "baseui/baseui.h"
#include "fs_layer.h"

st_ROM g_rom;

/* Pomocná proměnná pro mapování cfgfile keyword index ↔ rom_cfg řádek */
static int s_rom_type_cfg_index;


static void rom_install_predefined ( uint8_t *mz700rom, uint8_t *cgrom, uint8_t *mz800rom ) {
    uint8_t *ROM;

    ROM = g_memory.ROM;
    memcpy ( ROM, mz700rom, ROM_SIZE_0000 );
    g_rom.mz700rom = mz700rom;

    ROM += sizeof (uint8_t ) * ROM_SIZE_CGROM;
    memcpy ( ROM, cgrom, ROM_SIZE_CGROM );
    g_rom.cgrom = cgrom;

    ROM += sizeof (uint8_t ) * ROM_SIZE_CGROM;
    /* rom_e000 je volitelná — NULL znamená vyplnit oblast 0xff */
    if ( mz800rom ) {
        memcpy ( ROM, mz800rom, ROM_SIZE_E000 );
    } else {
        memset ( ROM, 0xff, ROM_SIZE_E000 );
    }
    g_rom.mz800rom = mz800rom;
}


static int rom_user_defined_load_file ( void *dst, char *filepath, uint32_t size ) {
    if ( ( !filepath ) || ( strlen ( filepath ) == 0 ) ) {
        fprintf ( stderr, "%s():%d - Empty ROM file name\n", __func__, __LINE__ );
        return EXIT_FAILURE;
    };
    FILE *fh;
    FS_LAYER_FOPEN ( fh, filepath, FS_LAYER_FMODE_RO );
    if ( !fh ) {
        fprintf ( stderr, "%s():%d - Cant open file '%s'\n", __func__, __LINE__, filepath );
        return EXIT_FAILURE;
    };
    int ret = EXIT_SUCCESS;
    uint32_t readlen = 0;
    if ( FS_LAYER_FR_OK != FS_LAYER_FREAD ( fh, dst, size, &readlen ) ) {
        fprintf ( stderr, "%s():%d - Can't read %d bytes from '%s'\n", __func__, __LINE__, size, filepath );
        ret = EXIT_FAILURE;
    };
    FS_LAYER_FCLOSE ( fh );
    return ret;
}


int rom_user_defined_rom_area_load ( st_ROM_AREA *dst, en_ROM_BOOL allinone, char *allinone_fp, char *mz700_fp, char *cgrom_fp, char *mz800_fp ) {
    if ( allinone == ROM_BOOL_YES ) return rom_user_defined_load_file ( dst, allinone_fp, ROM_SIZE_TOTAL );
    if ( EXIT_SUCCESS != rom_user_defined_load_file ( &dst->mz700rom, mz700_fp, ROM_SIZE_0000 ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != rom_user_defined_load_file ( &dst->cgrom, cgrom_fp, ROM_SIZE_CGROM ) ) return EXIT_FAILURE;
    /* rom_e000 je volitelná — pokud není zadána, oblast se vyplní nulami */
    if ( !mz800_fp || ( strlen ( mz800_fp ) == 0 ) ) {
        memset ( &dst->mz800rom, 0x00, ROM_SIZE_E000 );
        return EXIT_SUCCESS;
    }
    return rom_user_defined_load_file ( &dst->mz800rom, mz800_fp, ROM_SIZE_E000 );
}


static void rom_cmthack_is_not_compatibile ( void ) {
    if ( CMTHACK_TEST_IS_INSTALLED ) {
        printf ( "CMTHACK is not compatibile with selected ROM - CMTHACK DISABLED\n" );
        cmthack_load_rom_patch ( 0 );
    };
}


/**
 * Najde řádek ROM konfigurace podle indexu v poli (jen ROM řádky).
 * Index odpovídá pořadí, v jakém byly řádky registrovány do cfgfile keywords.
 */
static st_ROM_CONFIG_ROW *rom_find_row_by_cfg_index ( int cfg_index ) {
    int idx = 0;
    st_ROM_CONFIG_ROW *row;
    for ( row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++ ) {
        if ( row->type != ROM_CONFIG_TYPE_ROM ) continue;
        if ( !row->enabled ) continue;
        if ( idx == cfg_index ) return row;
        idx++;
    }
    return NULL;
}


/**
 * Najde index řádku ROM konfigurace (jen ROM řádky) pro ukládání do cfgfile.
 */
static int rom_find_cfg_index_by_row ( st_ROM_CONFIG_ROW *target ) {
    int idx = 0;
    st_ROM_CONFIG_ROW *row;
    for ( row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++ ) {
        if ( row->type != ROM_CONFIG_TYPE_ROM ) continue;
        if ( !row->enabled ) continue;
        if ( row == target ) return idx;
        idx++;
    }
    return 0; // Výchozí
}


static void rom_install ( st_ROM_CONFIG_ROW *row ) {

    en_ROM_BOOL custom_rom_error = FALSE;

    if ( !row ) row = g_rom_config->default_rom;

    if ( row->flags & ROM_CONFIG_FLAG_CMTHACK_INCOMP ) {
        rom_cmthack_is_not_compatibile ( );
    }

    if ( row->flags & ROM_CONFIG_FLAG_USER_DEFINED ) {
        /* USER_DEFINED logika - načtení ROM ze souborů */
        if ( g_rom.user_defined_rom_loaded != ROM_BOOL_YES ) {
            st_ROM_AREA rom;
            st_ROM_AREA rom_cmthack;

            if ( EXIT_SUCCESS != rom_user_defined_rom_area_load ( &rom, g_rom.user_defined_allinone, g_rom.user_defined_allinone_fp, g_rom.user_defined_mz700_fp, g_rom.user_defined_cgrom_fp, g_rom.user_defined_mz800_fp ) ) {
                printf ( "Can't install user defined ROM!\n" );
                custom_rom_error = TRUE;
            } else {
                if ( g_rom.user_defined_cmthack_type == ROM_CMTHACK_CUSTOM ) {
                    if ( EXIT_SUCCESS != rom_user_defined_rom_area_load ( &rom_cmthack, g_rom.user_defined_cmthack_allinone, g_rom.user_defined_cmthack_allinone_fp, g_rom.user_defined_cmthack_mz700_fp, g_rom.user_defined_cmthack_cgrom_fp, g_rom.user_defined_cmthack_mz800_fp ) ) {
                        printf ( "Can't install user defined cmthack ROM!\n" );
                        custom_rom_error = TRUE;
                    }
                }

                if ( !custom_rom_error ) {
                    g_rom.user_defined_rom_loaded = ROM_BOOL_YES;
                    memcpy ( &g_rom.rom_user_defined, &rom, sizeof ( st_ROM_AREA ) );
                    memcpy ( &g_rom.rom_user_defined_cmthack, &rom_cmthack, sizeof ( st_ROM_AREA ) );
                }
            }
        }

        if ( !custom_rom_error ) {
            if ( g_rom.user_defined_cmthack_type == ROM_CMTHACK_DISABLED ) {
                rom_cmthack_is_not_compatibile ( );
            }

            if ( g_cmthack.load_patch_installed ) {
                rom_install_predefined ( g_rom.rom_user_defined_cmthack.mz700rom, g_rom.rom_user_defined_cmthack.cgrom, g_rom.rom_user_defined_cmthack.mz800rom );
            } else {
                rom_install_predefined ( g_rom.rom_user_defined.mz700rom, g_rom.rom_user_defined.cgrom, g_rom.rom_user_defined.mz800rom );
            }
        }
    } else {
        /* Předdefinovaný ROM - data přímo z rom_config */
        rom_install_predefined ( (uint8_t*) row->rom_0000, (uint8_t*) row->rom_cgrom, (uint8_t*) row->rom_e000 );
    }

    if ( custom_rom_error ) {
        row = g_rom_config->default_rom;
        rom_install_predefined ( (uint8_t*) row->rom_0000, (uint8_t*) row->rom_cgrom, (uint8_t*) row->rom_e000 );
        g_rom.rom_cfg = row;
    } else {
        g_rom.rom_cfg = row;
    };
}


void rom_reinstall ( st_ROM_CONFIG_ROW *row ) {
    rom_install ( row );
    cmthack_reinstall_rom_patch ( );
    /* Zařízení devbus si přestaví odvozená data (např. generátor znaků
     * PCG-700 z nové CG-ROM). Bez inicializovaného devbus (MZ-800, MZ-1500)
     * okamžitý návrat. */
    devbus_rom_changed ( );
}


int rom_user_defined_check_filepath ( char **filepath, en_ROM_BOOL clear ) {
    if ( baseui_tools_file_access ( *filepath, F_OK ) != -1 ) return EXIT_SUCCESS;
    if ( ROM_BOOL_YES == clear ) {
        *filepath = (char*) baseui_tools_mem_realloc ( *filepath, 1 );
        *filepath[0] = 0x00;
    };
    return EXIT_FAILURE;
}


int rom_user_defined_check_size ( char **filepath, uint32_t size, en_ROM_BOOL clear ) {
    FILE *fh;
    FS_LAYER_FOPEN ( fh, *filepath, FS_LAYER_FMODE_RO );
    if ( !fh ) {
        if ( ROM_BOOL_YES == clear ) {
            *filepath = (char*) baseui_tools_mem_realloc ( *filepath, 1 );
            *filepath[0] = 0x00;
        };
        return EXIT_FAILURE;
    };
    FS_LAYER_FSEEK_END ( fh, 0 );
    uint32_t file_size = FS_LAYER_FTELL ( fh );
    FS_LAYER_FCLOSE ( fh );
    if ( file_size < size ) {
        if ( ROM_BOOL_YES == clear ) {
            *filepath = (char*) baseui_tools_mem_realloc ( *filepath, 1 );
            *filepath[0] = 0x00;
        };
        return EXIT_FAILURE;
    };
    return EXIT_SUCCESS;
}


void rom_set_user_defined_filepath ( char **dst, char *src ) {
    int len = 1;
    if ( src ) {
        len = strlen ( src ) + 1;
    };
    *dst = baseui_tools_mem_realloc ( *dst, len );
    if ( len == 1 ) {
        *dst[0] = 0x00;
    } else {
        strncpy ( *dst, src, len );
    };
}


/* Callback volaný při propagaci konfigurace - mapuje keyword index na rom_cfg řádek */
static void rom_propagate_cb ( void *e, void *data ) {
    (void) data;
    /* Hodnota musí být čtena přímo z elementu — handler ji zkopíruje do statické
     * proměnné až PO callbacku, takže s_rom_type_cfg_index by zde ještě
     * obsahoval starou (výchozí) hodnotu. */
    s_rom_type_cfg_index = cfgelement_get_keyword_value ( (CFGELM *) e );
    st_ROM_CONFIG_ROW *row = rom_find_row_by_cfg_index ( s_rom_type_cfg_index );
    if ( !row ) row = g_rom_config->default_rom;
    g_rom.rom_cfg = row;
}


/* Callback volaný před uložením konfigurace - mapuje rom_cfg řádek na keyword index */
static void rom_save_cb ( void *e, void *data ) {
    (void) e;
    (void) data;
    s_rom_type_cfg_index = rom_find_cfg_index_by_row ( g_rom.rom_cfg );
}


void rom_init ( void ) {

    g_rom.user_defined_rom_loaded = ROM_BOOL_NO;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "ROM" );

    /* Najdeme první platný ROM řádek pro výchozí keyword */
    st_ROM_CONFIG_ROW *first_row = NULL;
    st_ROM_CONFIG_ROW *row;
    for ( row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++ ) {
        if ( row->type != ROM_CONFIG_TYPE_ROM ) continue;
        if ( !row->enabled ) continue;
        if ( ( row->flags & ROM_CONFIG_FLAG_DEVELOPMENT ) && !g_emulator.development_mode ) continue;
        first_row = row;
        break;
    }
    if ( !first_row ) first_row = g_rom_config->default_rom;

    /* Vytvoříme keyword element s prvním platným keyword, aby cfgelement_set_variable prošel */
    CFGELM *elm_rom_type = cfgmodule_register_new_element ( cmod, "rom_type", CFGENTYPE_KEYWORD, 0,
                                                             0, first_row->name,
                                                             -1 );

    /* Dynamicky přidáme zbylé keywords z g_rom_config */
    int idx = 1;
    for ( row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++ ) {
        if ( row->type != ROM_CONFIG_TYPE_ROM ) continue;
        if ( !row->enabled ) continue;
        if ( ( row->flags & ROM_CONFIG_FLAG_DEVELOPMENT ) && !g_emulator.development_mode ) continue;
        if ( row == first_row ) continue; // Už bylo přidáno
        cfgelement_property_add_keyword ( elm_rom_type->pkw, idx, row->name );
        idx++;
    }

    /* propagate_handler = NULL: callback sám synchronizuje s_rom_type_cfg_index.
     * save_handler zůstává — rom_save_cb nastaví s_rom_type_cfg_index, handler
     * ho zkopíruje do elementu před zápisem do INI. */
    cfgelement_set_handlers ( elm_rom_type, NULL, (void*) &s_rom_type_cfg_index );
    cfgelement_set_propagate_cb ( elm_rom_type, rom_propagate_cb, NULL );
    cfgelement_set_save_cb ( elm_rom_type, rom_save_cb, NULL );

    CFGELM *elm;

    elm = cfgmodule_register_new_element ( cmod, "user_defined_allinone", CFGENTYPE_BOOL, ROM_BOOL_YES );
    cfgelement_set_handlers ( elm, (void*) &g_rom.user_defined_allinone, (void*) &g_rom.user_defined_allinone );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_allinone_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_allinone_fp, (void*) &g_rom.user_defined_allinone_fp );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_mz700_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_mz700_fp, (void*) &g_rom.user_defined_mz700_fp );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_cgrom_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_cgrom_fp, (void*) &g_rom.user_defined_cgrom_fp );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_mz800_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_mz800_fp, (void*) &g_rom.user_defined_mz800_fp );


    elm = cfgmodule_register_new_element ( cmod, "user_defined_cmthack", CFGENTYPE_KEYWORD, ROM_CMTHACK_DISABLED,
                                           ROM_CMTHACK_DISABLED, "DISABLED",
                                           ROM_CMTHACK_DEFAULT, "DEFAULT",
                                           ROM_CMTHACK_CUSTOM, "CUSTOM",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_rom.user_defined_cmthack_type, (void*) &g_rom.user_defined_cmthack_type );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_cmthack_allinone", CFGENTYPE_BOOL, ROM_BOOL_YES );
    cfgelement_set_handlers ( elm, (void*) &g_rom.user_defined_cmthack_allinone, (void*) &g_rom.user_defined_cmthack_allinone );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_cmthack_allinone_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_cmthack_allinone_fp, (void*) &g_rom.user_defined_cmthack_allinone_fp );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_cmthack_mz700_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_cmthack_mz700_fp, (void*) &g_rom.user_defined_cmthack_mz700_fp );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_cmthack_cgrom_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_cmthack_cgrom_fp, (void*) &g_rom.user_defined_cmthack_cgrom_fp );

    elm = cfgmodule_register_new_element ( cmod, "user_defined_cmthack_mz800_filepath", CFGENTYPE_TEXT, "" );
    cfgelement_set_pointers ( elm, (void*) &g_rom.user_defined_cmthack_mz800_fp, (void*) &g_rom.user_defined_cmthack_mz800_fp );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

    if ( strlen ( g_rom.user_defined_allinone_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_allinone_fp, ROM_BOOL_YES );
    if ( strlen ( g_rom.user_defined_mz700_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_mz700_fp, ROM_BOOL_YES );
    if ( strlen ( g_rom.user_defined_cgrom_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_cgrom_fp, ROM_BOOL_YES );
    if ( strlen ( g_rom.user_defined_mz800_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_mz800_fp, ROM_BOOL_YES );
    if ( strlen ( g_rom.user_defined_cmthack_allinone_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_cmthack_allinone_fp, ROM_BOOL_YES );
    if ( strlen ( g_rom.user_defined_cmthack_mz700_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_cmthack_mz700_fp, ROM_BOOL_YES );
    if ( strlen ( g_rom.user_defined_cmthack_cgrom_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_cmthack_cgrom_fp, ROM_BOOL_YES );
    if ( strlen ( g_rom.user_defined_cmthack_mz800_fp ) ) rom_user_defined_check_filepath ( &g_rom.user_defined_cmthack_mz800_fp, ROM_BOOL_YES );

    rom_install ( g_rom.rom_cfg );
}
