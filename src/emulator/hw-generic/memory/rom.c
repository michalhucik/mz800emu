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


/**
 * @brief Vypne CMT hack, který vybraná ROM nepodporuje.
 *
 * Jako každé vypnutí hacku (cmt_loading_mode_apply()) zapomene i stav hacku
 * nad páskou (cmthack_tape_state_clear(): hlavička načtená bez těla, konec
 * pásky); jinak by po návratu kompatibilní ROM a zapnutí hacku příští LOAD
 * navázal na starou hlavičku.
 *
 * cmthack_load_rom_patch(0) příznak hacku nevynuluje, když ho právě
 * vybraná ROM sama nepodporuje (uživatelská ROM, které volbu změnil na
 * Disabled CMT Hack Patch zásah nastavení ještě před přeinstalováním:
 * cmthack_mzarch_load_rom_patch() pak nic neudělá). Příznak se proto
 * vynuluje i přímo; obsah paměti ROM vzápětí přepíše rom_install() z dat
 * nové ROM bez patche, vracet bajty patche netřeba.
 *
 * @pre Emu vlákno nebo inicializace (rom_install()), před instalací dat nové ROM.
 * @post CMT hack není nainstalovaný (!CMTHACK_TEST_IS_INSTALLED) ani patch
 *       zápisu (!CMTHACK_TEST_SAVE_INSTALLED); byl-li, stav hacku nad
 *       páskou je prázdný.
 */
static void rom_cmthack_is_not_compatibile ( void ) {
    if ( CMTHACK_TEST_IS_INSTALLED ) {
        printf ( "CMTHACK is not compatibile with selected ROM - CMTHACK DISABLED\n" );
        cmthack_load_rom_patch ( 0 );
        g_cmthack.load_patch_installed = 0;
        /* patch zápisu (Direct Save) jen s patchem čtení; bajty přepíše nová ROM */
        cmthack_save_patch_sync ( );
        cmthack_tape_state_clear ( );
    };
}


/** @brief Element `[ROM] rom_type` (rom_init()); přes něj se jméno ROM převádí na index klíčového slova. */
static CFGELM *s_rom_type_elm = NULL;


/**
 * @brief Má řádek konfigurace ROM při registraci klíčové slovo v INI?
 *
 * Podmínka registrace klíčových slov `[ROM] rom_type` v rom_init(): řádek
 * typu ROM, povolený, a vývojová ROM (ROM_CONFIG_FLAG_DEVELOPMENT) jen ve
 * vývojovém režimu. Převody mezi klíčovým slovem a řádkem tuto podmínku
 * nepoužívají: jdou přes jméno (rom_find_row_by_name(),
 * rom_keyword_index_of_row()), takže nezáleží na tom, zda se vývojový
 * režim za běhu přepnul.
 *
 * @param row Řádek konfigurace (ne NULL).
 * @return true = řádek dostane klíčové slovo.
 * @pre g_emulator.development_mode je načtený z INI (emulator_init() před
 *      rom_init()).
 */
static bool rom_row_has_keyword ( const st_ROM_CONFIG_ROW *row ) {
    if ( row->type != ROM_CONFIG_TYPE_ROM ) return false;
    if ( !row->enabled ) return false;
    if ( ( row->flags & ROM_CONFIG_FLAG_DEVELOPMENT ) && !g_emulator.development_mode ) return false;
    return true;
}


/**
 * @brief Najde povolený řádek ROM podle jména (klíčového slova `rom_type`).
 * @param name Jméno řádku (NULL = nenalezeno).
 * @return Řádek, nebo NULL.
 */
static st_ROM_CONFIG_ROW *rom_find_row_by_name ( const char *name ) {
    if ( !name ) return NULL;
    st_ROM_CONFIG_ROW *row;
    for ( row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++ ) {
        if ( ( row->type != ROM_CONFIG_TYPE_ROM ) || !row->enabled || !row->name ) continue;
        if ( 0 == strcmp ( row->name, name ) ) return row;
    }
    return NULL;
}


/**
 * @brief Najde řádek ROM tak, jak index klíčového slova četl kód před opravou (INI formátu 1.0).
 *
 * Tehdejší kód bral index klíčového slova jako pořadí mezi všemi
 * povolenými řádky ROM, i když vývojové ROM bez vývojového režimu klíčové
 * slovo neměly; na MZ-800 se tak ROM zapsala i četla pod jménem jiné
 * (JSS108C jako WILLY_EN). Staré INI (cfgmain_ini_has_legacy_rom_keywords())
 * se proto čte stejně, aby uživateli zůstala ROM, kterou skutečně měl.
 *
 * @param cfg_index Index klíčového slova přečteného z INI (od 0).
 * @return Řádek, nebo NULL pro index mimo rozsah.
 */
static st_ROM_CONFIG_ROW *rom_find_row_legacy ( int cfg_index ) {
    int idx = 0;
    st_ROM_CONFIG_ROW *row;
    for ( row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++ ) {
        if ( ( row->type != ROM_CONFIG_TYPE_ROM ) || !row->enabled ) continue;
        if ( idx == cfg_index ) return row;
        idx++;
    }
    return NULL;
}


/**
 * @brief Index klíčového slova `rom_type`, které nese jméno řádku ROM (ukládání do INI).
 *
 * @param target Vybraný řádek.
 * @return Index klíčového slova se jménem řádku; 0 (první volitelná ROM),
 *         když řádek klíčové slovo nemá (vývojová ROM, když emulátor
 *         startoval bez vývojového režimu) nebo element ještě není
 *         registrovaný.
 */
static int rom_keyword_index_of_row ( const st_ROM_CONFIG_ROW *target ) {
    if ( !s_rom_type_elm || !target || !target->name ) return 0;
    int value = cfgelement_property_get_value_by_keyword ( s_rom_type_elm->pkw, (char*) target->name );
    return ( value < 0 ) ? 0 : value;
}


/**
 * @brief Nainstaluje data ROM podle řádku konfigurace do g_memory.ROM a nastaví g_rom.rom_cfg.
 *
 * ROM bez podpory CMT hacku (příznak ROM_CONFIG_FLAG_CMTHACK_INCOMP,
 * uživatelská ROM s Disabled CMT Hack Patch) hack před instalací vypne
 * (rom_cmthack_is_not_compatibile()). Uživatelskou ROM načte ze souborů,
 * není-li načtená (g_rom.user_defined_rom_loaded); vlastní ROM pro CMT hack
 * načte a nainstaluje jen u varianty CUSTOM (se zapnutým hackem), jinak
 * hlavní ROM. Chyba načtení = výchozí ROM. Patch varianty DEFAULT a ROM
 * bez uživatelské varianty sem nepatří: dává ho cmthack_reinstall_rom_patch()
 * v rom_reinstall().
 *
 * Vedlejší účinky: g_rom.mz700rom, g_rom.cgrom a g_rom.mz800rom ukazují na
 * nainstalovaná data (rom_install_predefined(); z nich vrací
 * cmthack_mzarch_load_rom_patch(0) bajty patche); úspěšné načtení
 * uživatelské ROM nastaví g_rom.user_defined_rom_loaded a přepíše
 * g_rom.rom_user_defined (u CUSTOM i g_rom.rom_user_defined_cmthack).
 * Vypnutí hacku (rom_cmthack_is_not_compatibile()) běží se starou
 * g_rom.rom_cfg: byla-li to uživatelská ROM s CUSTOM, vnořené rom_reinstall()
 * z cmthack_mzarch_load_rom_patch(0) ji ještě jednou nainstaluje bez hacku,
 * než se nainstaluje nová ROM. Chyby načtení souborů vypisuje na stdout
 * a stderr (printf(), fprintf()); uživateli je nehlásí.
 *
 * @param row Řádek g_rom_config->roms; NULL = výchozí ROM.
 * @pre Emu vlákno pod device_state_lock() nebo inicializace (rom_init()).
 * @post Je-li vybraná ROM bez podpory CMT hacku, hack není nainstalovaný;
 *       do paměti ROM nikdy nejdou nenačtená data.
 */
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
                    /* rom_cmthack je načtená jen u CUSTOM; jinak by se kopíroval neinicializovaný zásobník */
                    if ( g_rom.user_defined_cmthack_type == ROM_CMTHACK_CUSTOM ) {
                        memcpy ( &g_rom.rom_user_defined_cmthack, &rom_cmthack, sizeof ( st_ROM_AREA ) );
                    }
                }
            }
        }

        if ( !custom_rom_error ) {
            if ( g_rom.user_defined_cmthack_type == ROM_CMTHACK_DISABLED ) {
                rom_cmthack_is_not_compatibile ( );
            }

            /* Vlastní ROM pro CMT hack jen u CUSTOM; u DEFAULT se instaluje hlavní
             * ROM a patch na ni dá cmthack_reinstall_rom_patch() (rom_reinstall()) */
            if ( g_cmthack.load_patch_installed && ( g_rom.user_defined_cmthack_type == ROM_CMTHACK_CUSTOM ) ) {
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


/**
 * @brief Propagace `[ROM] rom_type` po načtení INI: vybere řádek ROM podle klíčového slova.
 *
 * Řádek se hledá podle jména klíčového slova. INI formátu 1.0 se čte
 * tehdejším způsobem (rom_find_row_legacy()). Neznámé jméno nebo index
 * mimo rozsah = výchozí ROM. Po výběru se index elementu srovná se jménem
 * vybraného řádku, aby živá hodnota nastavení odpovídala vybrané ROM.
 * Volá se jen při startu (rom_init()).
 *
 * @param e Element `rom_type`. @param data Nepoužito.
 */
static void rom_propagate_cb ( void *e, void *data ) {
    (void) data;
    /* Hodnota musí být čtena přímo z elementu - handler ji zkopíruje do statické
     * proměnné až PO callbacku, takže s_rom_type_cfg_index by zde ještě
     * obsahoval starou (výchozí) hodnotu. */
    int value = cfgelement_get_keyword_value ( (CFGELM *) e );
    st_ROM_CONFIG_ROW *row;
    if ( cfgmain_ini_has_legacy_rom_keywords ( ) ) {
        row = rom_find_row_legacy ( value );
    } else {
        row = rom_find_row_by_name ( cfgelement_get_keyword_by_value ( (CFGELM *) e ) );
    };
    if ( !row ) row = g_rom_config->default_rom;
    g_rom.rom_cfg = row;
    s_rom_type_cfg_index = rom_keyword_index_of_row ( row );
}


/**
 * @brief Před uložením INI: index klíčového slova se jménem vybrané ROM.
 * @param e Nepoužito. @param data Nepoužito.
 */
static void rom_save_cb ( void *e, void *data ) {
    (void) e;
    (void) data;
    s_rom_type_cfg_index = rom_keyword_index_of_row ( g_rom.rom_cfg );
}


void rom_init ( void ) {

    g_rom.user_defined_rom_loaded = ROM_BOOL_NO;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "ROM" );

    /* Najdeme první platný ROM řádek pro výchozí keyword */
    st_ROM_CONFIG_ROW *first_row = NULL;
    st_ROM_CONFIG_ROW *row;
    for ( row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++ ) {
        if ( !rom_row_has_keyword ( row ) ) continue;
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
        if ( !rom_row_has_keyword ( row ) ) continue;
        if ( row == first_row ) continue; // Už bylo přidáno
        cfgelement_property_add_keyword ( elm_rom_type->pkw, idx, row->name );
        idx++;
    }

    /* propagate_handler = NULL: callback sám synchronizuje s_rom_type_cfg_index.
     * save_handler zůstává — rom_save_cb nastaví s_rom_type_cfg_index, handler
     * ho zkopíruje do elementu před zápisem do INI. */
    s_rom_type_elm = elm_rom_type;
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
