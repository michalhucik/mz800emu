/**
 * @file   tmz.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.14.0
 * @brief  Implementace TMZ knihovny - MZ-specificka nadstavba nad TZX.
 *
 * Obsahuje funkce specificke pro TMZ format:
 * - tmz_header_init() - inicializace hlavicky s TMZ signaturou "TapeMZ!"
 * - tmz_header_is_tmz() - overeni TMZ signatury
 * - tmz_block_id_name() - wrapper nad tzx_block_id_name() + MZ bloky
 * - tmz_block_is_mz_extension() - detekce MZ extenzi (0x40-0x4F)
 *
 * Souborove I/O (load/save/free) je v tzx knihovne (tzx.h/tzx.c).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../tzx/tzx.h"

#include "tmz.h"


/* ========================================================================= */
/*  Operace s hlavickou                                                      */
/* ========================================================================= */

/**
 * @brief Inicializuje TMZ hlavicku na vychozi hodnoty.
 *
 * Nastavi signaturu "TapeMZ!", EOF marker 0x1A a verzi 1.0.
 *
 * @param header Ukazatel na hlavicku k inicializaci.
 *
 * @pre header nesmi byt NULL.
 * @post Hlavicka obsahuje platne TMZ hodnoty.
 */
void tmz_header_init ( st_TZX_HEADER *header ) {
    if ( !header ) return;
    memcpy ( header->signature, TMZ_SIGNATURE, TZX_SIGNATURE_LENGTH );
    header->eof_marker = TZX_EOF_MARKER;
    header->ver_major = TMZ_VERSION_MAJOR;
    header->ver_minor = TMZ_VERSION_MINOR;
}


/**
 * @brief Zjisti, zda hlavicka obsahuje TMZ signaturu "TapeMZ!".
 *
 * @param header Ukazatel na hlavicku.
 * @return true pokud je signatura "TapeMZ!", false jinak.
 */
bool tmz_header_is_tmz ( const st_TZX_HEADER *header ) {
    if ( !header ) return false;
    return ( memcmp ( header->signature, TMZ_SIGNATURE, TZX_SIGNATURE_LENGTH ) == 0 );
}


/* ========================================================================= */
/*  Nazvy bloku                                                              */
/* ========================================================================= */

/**
 * @brief Vrati lidsky citelny nazev bloku podle jeho ID.
 *
 * Pro MZ-specificke bloky (0x40-0x4F) vraci lokalni nazvy.
 * Pro standardni TZX bloky deleguje na tzx_block_id_name().
 *
 * @param id ID bloku.
 * @return Retezec s nazvem bloku, nebo "Unknown" pro nezname ID.
 */
const char* tmz_block_id_name ( uint8_t id ) {
    switch ( id ) {
        /* TMZ MZ-specificke bloky */
        case TMZ_BLOCK_ID_MZ_STANDARD_DATA:  return "MZ Standard Data";
        case TMZ_BLOCK_ID_MZ_TURBO_DATA:     return "MZ Turbo Data";
        case TMZ_BLOCK_ID_MZ_EXTRA_BODY:     return "MZ Extra Body";
        case TMZ_BLOCK_ID_MZ_MACHINE_INFO:   return "MZ Machine Info";
        case TMZ_BLOCK_ID_MZ_LOADER:         return "MZ Loader";
        case TMZ_BLOCK_ID_MZ_BASIC_DATA:     return "MZ BASIC Data";
        case TMZ_BLOCK_ID_MZ_FILE:           return "MZ File";

        /* vsechny ostatni delegovat na TZX */
        default:
            return tzx_block_id_name ( id );
    }
}


/**
 * @brief Zjisti, zda je blok s danym ID MZ-specifickou extenzi (0x40-0x4F).
 *
 * @param id ID bloku.
 * @return true pokud je blok MZ extenze, false jinak.
 */
bool tmz_block_is_mz_extension ( uint8_t id ) {
    return ( id >= 0x40 && id <= 0x4F );
}


const char* tmz_version ( void ) {
    return TMZ_VERSION;
}


const char* tmz_format_version ( void ) {
    return TMZ_FORMAT_VERSION;
}
