/**
 * @file   tmz_blocks.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.0
 * @brief  Implementace parsovani a vytvareni TMZ MZ-specifickych bloku (0x40-0x4F).
 *
 * Obsahuje funkce pro parsovani surových dat bloku na typovane struktury,
 * vytvareni novych bloku z parametru a konverze mezi MZF a TMZ bloky.
 *
 * Vsechna vicebajtova pole v blocich jsou v little-endian (LE) poradi.
 * Funkce provadeji automatickou konverzi na hostitelsky byte order.
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


#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "../endianity/endianity.h"
#include "../mzf/mzf.h"

#include "tmz.h"
#include "tmz_blocks.h"
#include "tmz_mzfile.h"


/* ========================================================================= */
/*  Externi alokator a error callback (definovany v tmz.c)                   */
/* ========================================================================= */

/** @brief Vychozi alokace pameti (obaluje malloc). */
static void* tmz_blocks_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Vychozi alokace s nulovanim (obaluje calloc). */
static void* tmz_blocks_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Vychozi uvolneni pameti (obaluje free). */
static void  tmz_blocks_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Interni alokator pro tmz_blocks modul. */
static const st_TZX_ALLOCATOR g_tmzblocks_default_allocator = {
    tmz_blocks_default_alloc,
    tmz_blocks_default_alloc0,
    tmz_blocks_default_free,
};

/** @brief Aktualne aktivni alokator. */
static const st_TZX_ALLOCATOR *g_tmzblocks_allocator = &g_tmzblocks_default_allocator;


/**
 * @brief Vychozi error callback pro tmz_blocks modul.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void tmz_blocks_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback pro tmz_blocks modul. */
static void (*g_tmzblocks_error_cb)( const char*, int, const char*, ... ) = tmz_blocks_default_error_cb;


/* ========================================================================= */
/*  Parsovani MZ-specifickych bloku                                          */
/* ========================================================================= */

/**
 * @brief Naparsuje blok 0x40 (MZ Standard Data) ze surovych dat.
 *
 * Vraci ukazatel na strukturu uvnitr dat bloku (zero-copy) a nastavi
 * body_data na zacatek tela za strukturou.
 *
 * @param block TMZ blok s ID 0x40.
 * @param[out] body_data Ukazatel na data tela (ukazuje do block->data).
 * @param[out] err Vystupni chybovy kod (muze byt NULL).
 * @return Ukazatel na st_TMZ_MZ_STANDARD_DATA uvnitr dat bloku, nebo NULL pri chybe.
 *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
 *         velikost dat (body_size) větší než data bloku - validuje se před
 *         konverzí endianity, poškozený blok se tedy nemodifikuje.
 *
 * @pre block nesmi byt NULL, block->id musi byt TMZ_BLOCK_ID_MZ_STANDARD_DATA.
 * @note Vraceny ukazatel ukazuje primo do block->data - nealokuje novou pamet.
 *       Platnost ukazatele je vazana na zivotnost bloku.
 */
st_TMZ_MZ_STANDARD_DATA* tmz_block_parse_mz_standard ( const st_TZX_BLOCK *block, uint8_t **body_data, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block || !block->data ) {
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    /* blok 0x40 ma 4B delku za ID, takze data zacinaji za DWORD delkou */
    if ( block->length < 4 + sizeof ( st_TMZ_MZ_STANDARD_DATA ) ) {
        g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x40 too short: %u bytes\n", block->length );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    /* deklarovaná velikost těla se musí vejít do bloku (validace před mutací dat) */
    {
        const st_TMZ_MZ_STANDARD_DATA *raw = ( const st_TMZ_MZ_STANDARD_DATA* ) ( block->data + 4 );
        uint32_t declared = endianity_bswap16_LE ( raw->body_size );
        uint32_t available = block->length - ( uint32_t ) ( 4 + sizeof ( st_TMZ_MZ_STANDARD_DATA ) );
        if ( declared > available ) {
            g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x40 body_size %u exceeds block data (%u bytes)\n", declared, available );
            *err = TZX_ERROR_INVALID_BLOCK;
            return NULL;
        }
    }

    /* data bloku zacinaji za 4B DWORD delkou */
    st_TMZ_MZ_STANDARD_DATA *std_data = (st_TMZ_MZ_STANDARD_DATA*) ( block->data + 4 );

    /* konverze endianity */
    std_data->pause_ms = endianity_bswap16_LE ( std_data->pause_ms );
    std_data->body_size = endianity_bswap16_LE ( std_data->body_size );

    /* MZF hlavička: jen pořadí bajtů, 11h (fname.terminator) zůstává podle bloku */
    mzf_header_swap_endianity ( &std_data->mzf_header );

    if ( body_data ) {
        *body_data = block->data + 4 + sizeof ( st_TMZ_MZ_STANDARD_DATA );
    }

    *err = TZX_OK;
    return std_data;
}


/**
 * @brief Naparsuje blok 0x41 (MZ Turbo Data) ze surovych dat.
 *
 * @param block TMZ blok s ID 0x41.
 * @param[out] body_data Ukazatel na data tela (ukazuje do block->data).
 * @param[out] err Vystupni chybovy kod (muze byt NULL).
 * @return Ukazatel na st_TMZ_MZ_TURBO_DATA uvnitr dat bloku, nebo NULL pri chybe.
 *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
 *         velikost dat (body_size) větší než data bloku - validuje se před
 *         konverzí endianity, poškozený blok se tedy nemodifikuje.
 *
 * @pre block nesmi byt NULL, block->id musi byt TMZ_BLOCK_ID_MZ_TURBO_DATA.
 * @note Vraceny ukazatel ukazuje primo do block->data - nealokuje novou pamet.
 */
st_TMZ_MZ_TURBO_DATA* tmz_block_parse_mz_turbo ( const st_TZX_BLOCK *block, uint8_t **body_data, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block || !block->data ) {
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    if ( block->length < 4 + sizeof ( st_TMZ_MZ_TURBO_DATA ) ) {
        g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x41 too short: %u bytes\n", block->length );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    /* deklarovaná velikost těla se musí vejít do bloku (validace před mutací dat) */
    {
        const st_TMZ_MZ_TURBO_DATA *raw = ( const st_TMZ_MZ_TURBO_DATA* ) ( block->data + 4 );
        uint32_t declared = endianity_bswap16_LE ( raw->body_size );
        uint32_t available = block->length - ( uint32_t ) ( 4 + sizeof ( st_TMZ_MZ_TURBO_DATA ) );
        if ( declared > available ) {
            g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x41 body_size %u exceeds block data (%u bytes)\n", declared, available );
            *err = TZX_ERROR_INVALID_BLOCK;
            return NULL;
        }
    }

    st_TMZ_MZ_TURBO_DATA *turbo_data = (st_TMZ_MZ_TURBO_DATA*) ( block->data + 4 );

    /* konverze endianity */
    turbo_data->lgap_length = endianity_bswap16_LE ( turbo_data->lgap_length );
    turbo_data->sgap_length = endianity_bswap16_LE ( turbo_data->sgap_length );
    turbo_data->pause_ms = endianity_bswap16_LE ( turbo_data->pause_ms );
    turbo_data->long_high = endianity_bswap16_LE ( turbo_data->long_high );
    turbo_data->long_low = endianity_bswap16_LE ( turbo_data->long_low );
    turbo_data->short_high = endianity_bswap16_LE ( turbo_data->short_high );
    turbo_data->short_low = endianity_bswap16_LE ( turbo_data->short_low );
    turbo_data->body_size = endianity_bswap16_LE ( turbo_data->body_size );

    mzf_header_swap_endianity ( &turbo_data->mzf_header );

    if ( body_data ) {
        *body_data = block->data + 4 + sizeof ( st_TMZ_MZ_TURBO_DATA );
    }

    *err = TZX_OK;
    return turbo_data;
}


/**
 * @brief Naparsuje blok 0x42 (MZ Extra Body) ze surovych dat.
 *
 * @param block TMZ blok s ID 0x42.
 * @param[out] body_data Ukazatel na data tela (ukazuje do block->data).
 * @param[out] err Vystupni chybovy kod (muze byt NULL).
 * @return Ukazatel na st_TMZ_MZ_EXTRA_BODY uvnitr dat bloku, nebo NULL pri chybe.
 *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
 *         velikost dat (body_size) větší než data bloku - validuje se před
 *         konverzí endianity, poškozený blok se tedy nemodifikuje.
 *
 * @pre block nesmi byt NULL, block->id musi byt TMZ_BLOCK_ID_MZ_EXTRA_BODY.
 * @note Vraceny ukazatel ukazuje primo do block->data - nealokuje novou pamet.
 */
st_TMZ_MZ_EXTRA_BODY* tmz_block_parse_mz_extra_body ( const st_TZX_BLOCK *block, uint8_t **body_data, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block || !block->data ) {
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    if ( block->length < 4 + sizeof ( st_TMZ_MZ_EXTRA_BODY ) ) {
        g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x42 too short: %u bytes\n", block->length );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    /* deklarovaná velikost těla se musí vejít do bloku (validace před mutací dat) */
    {
        const st_TMZ_MZ_EXTRA_BODY *raw = ( const st_TMZ_MZ_EXTRA_BODY* ) ( block->data + 4 );
        uint32_t declared = endianity_bswap16_LE ( raw->body_size );
        uint32_t available = block->length - ( uint32_t ) ( 4 + sizeof ( st_TMZ_MZ_EXTRA_BODY ) );
        if ( declared > available ) {
            g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x42 body_size %u exceeds block data (%u bytes)\n", declared, available );
            *err = TZX_ERROR_INVALID_BLOCK;
            return NULL;
        }
    }

    st_TMZ_MZ_EXTRA_BODY *extra = (st_TMZ_MZ_EXTRA_BODY*) ( block->data + 4 );

    extra->pause_ms = endianity_bswap16_LE ( extra->pause_ms );
    extra->body_size = endianity_bswap16_LE ( extra->body_size );

    if ( body_data ) {
        *body_data = block->data + 4 + sizeof ( st_TMZ_MZ_EXTRA_BODY );
    }

    *err = TZX_OK;
    return extra;
}


/**
 * @brief Naparsuje blok 0x43 (MZ Machine Info) ze surovych dat.
 *
 * @param block TMZ blok s ID 0x43.
 * @param[out] err Vystupni chybovy kod (muze byt NULL).
 * @return Ukazatel na st_TMZ_MZ_MACHINE_INFO uvnitr dat bloku, nebo NULL pri chybe.
 *
 * @pre block nesmi byt NULL, block->id musi byt TMZ_BLOCK_ID_MZ_MACHINE_INFO.
 * @note Vraceny ukazatel ukazuje primo do block->data - nealokuje novou pamet.
 */
st_TMZ_MZ_MACHINE_INFO* tmz_block_parse_mz_machine_info ( const st_TZX_BLOCK *block, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block || !block->data ) {
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    if ( block->length < 4 + sizeof ( st_TMZ_MZ_MACHINE_INFO ) ) {
        g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x43 too short: %u bytes\n", block->length );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    st_TMZ_MZ_MACHINE_INFO *info = (st_TMZ_MZ_MACHINE_INFO*) ( block->data + 4 );

    info->cpu_clock = endianity_bswap32_LE ( info->cpu_clock );

    *err = TZX_OK;
    return info;
}


/**
 * @brief Naparsuje blok 0x44 (MZ Loader) ze surovych dat.
 *
 * @param block TMZ blok s ID 0x44.
 * @param[out] loader_data Ukazatel na loader data (ukazuje do block->data).
 * @param[out] err Vystupni chybovy kod (muze byt NULL).
 * @return Ukazatel na st_TMZ_MZ_LOADER uvnitr dat bloku, nebo NULL pri chybe.
 *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
 *         velikost dat (loader_size) větší než data bloku - validuje se před
 *         konverzí endianity, poškozený blok se tedy nemodifikuje.
 *
 * @pre block nesmi byt NULL, block->id musi byt TMZ_BLOCK_ID_MZ_LOADER.
 * @note Vraceny ukazatel ukazuje primo do block->data - nealokuje novou pamet.
 */
st_TMZ_MZ_LOADER* tmz_block_parse_mz_loader ( const st_TZX_BLOCK *block, uint8_t **loader_data, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block || !block->data ) {
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    if ( block->length < 4 + sizeof ( st_TMZ_MZ_LOADER ) ) {
        g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x44 too short: %u bytes\n", block->length );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    /* deklarovaná velikost loaderu se musí vejít do bloku (validace před mutací dat) */
    {
        const st_TMZ_MZ_LOADER *raw = ( const st_TMZ_MZ_LOADER* ) ( block->data + 4 );
        uint32_t declared = endianity_bswap16_LE ( raw->loader_size );
        uint32_t available = block->length - ( uint32_t ) ( 4 + sizeof ( st_TMZ_MZ_LOADER ) );
        if ( declared > available ) {
            g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x44 loader_size %u exceeds block data (%u bytes)\n", declared, available );
            *err = TZX_ERROR_INVALID_BLOCK;
            return NULL;
        }
    }

    st_TMZ_MZ_LOADER *loader = (st_TMZ_MZ_LOADER*) ( block->data + 4 );

    loader->loader_size = endianity_bswap16_LE ( loader->loader_size );
    mzf_header_swap_endianity ( &loader->mzf_header );

    if ( loader_data ) {
        *loader_data = block->data + 4 + sizeof ( st_TMZ_MZ_LOADER );
    }

    *err = TZX_OK;
    return loader;
}


/**
 * @brief Naparsuje blok 0x45 (MZ BASIC Data) ze surovych dat.
 *
 * Vraci ukazatel na strukturu uvnitr dat bloku (zero-copy) a nastavi
 * chunks_data na zacatek chunkovych dat za strukturou.
 *
 * @param block TMZ blok s ID 0x45.
 * @param[out] chunks_data Ukazatel na chunky (ukazuje do block->data).
 * @param[out] err Vystupni chybovy kod (muze byt NULL).
 * @return Ukazatel na st_TMZ_MZ_BASIC_DATA uvnitr dat bloku, nebo NULL pri chybe.
 *
 * @pre block nesmi byt NULL, block->id musi byt TMZ_BLOCK_ID_MZ_BASIC_DATA.
 * @note Vraceny ukazatel ukazuje primo do block->data - nealokuje novou pamet.
 */
st_TMZ_MZ_BASIC_DATA* tmz_block_parse_mz_basic_data ( const st_TZX_BLOCK *block, uint8_t **chunks_data, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block || !block->data ) {
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    if ( block->length < 4 + sizeof ( st_TMZ_MZ_BASIC_DATA ) ) {
        g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x45 too short: %u bytes\n", block->length );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    st_TMZ_MZ_BASIC_DATA *bsd = (st_TMZ_MZ_BASIC_DATA*) ( block->data + 4 );

    /* konverze endianity */
    bsd->pause_ms = endianity_bswap16_LE ( bsd->pause_ms );
    bsd->chunk_count = endianity_bswap16_LE ( bsd->chunk_count );

    /* MZF hlavička: jen pořadí bajtů, 11h (fname.terminator) zůstává podle bloku */
    mzf_header_swap_endianity ( &bsd->mzf_header );

    /* validace: musi byt alespon 1 chunk a data musi byt dostatecne velka */
    uint32_t expected_size = 4 + sizeof ( st_TMZ_MZ_BASIC_DATA )
                             + (uint32_t) bsd->chunk_count * TMZ_BASIC_CHUNK_SIZE;
    if ( block->length < expected_size ) {
        g_tmzblocks_error_cb ( __func__, __LINE__,
            "Block 0x45 data too short for %u chunks: have %u, need %u bytes\n",
            bsd->chunk_count, block->length, expected_size );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    if ( chunks_data ) {
        *chunks_data = block->data + 4 + sizeof ( st_TMZ_MZ_BASIC_DATA );
    }

    *err = TZX_OK;
    return bsd;
}


/* ========================================================================= */
/*  Vytvareni bloku                                                          */
/* ========================================================================= */

/**
 * @brief Vytvori novy blok 0x40 (MZ Standard Data) z parametru.
 *
 * Alokuje st_TZX_BLOCK a jeho data, naplni strukturou a telovymi daty.
 * Vicebajtova pole se konvertuji do little-endian pri zapisu.
 *
 * @param machine Cilovy stroj.
 * @param pulseset Pulsni sada.
 * @param pause_ms Pauza po bloku (ms).
 * @param mzf_header MZF hlavicka (128B).
 * @param body Data tela.
 * @param body_size Velikost tela v bajtech.
 * @return Novy blok, nebo NULL pri chybe.
 *
 * @pre mzf_header nesmi byt NULL.
 * @post Volajici vlastni vraceny blok a musi ho uvolnit pres tmz_block_free().
 */
st_TZX_BLOCK* tmz_block_create_mz_standard ( en_TMZ_MACHINE machine, en_TMZ_PULSESET pulseset,
                                              uint16_t pause_ms, const st_MZF_HEADER *mzf_header,
                                              const uint8_t *body, uint16_t body_size ) {

    if ( !mzf_header ) return NULL;

    uint32_t data_length = sizeof ( st_TMZ_MZ_STANDARD_DATA ) + body_size;
    uint32_t total = 4 + data_length; /* 4B DWORD delka + data */

    st_TZX_BLOCK *block = g_tmzblocks_allocator->alloc0 ( sizeof ( st_TZX_BLOCK ) );
    if ( !block ) return NULL;

    block->id = TMZ_BLOCK_ID_MZ_STANDARD_DATA;
    block->length = total;
    block->data = g_tmzblocks_allocator->alloc0 ( total );
    if ( !block->data ) {
        g_tmzblocks_allocator->free ( block );
        return NULL;
    }

    /* zapis DWORD delky */
    uint32_t le_length = endianity_bswap32_LE ( data_length );
    memcpy ( block->data, &le_length, 4 );

    /* zapis struktury */
    st_TMZ_MZ_STANDARD_DATA *std_data = (st_TMZ_MZ_STANDARD_DATA*) ( block->data + 4 );
    std_data->machine = (uint8_t) machine;
    std_data->pulseset = (uint8_t) pulseset;
    std_data->pause_ms = endianity_bswap16_LE ( pause_ms );
    memcpy ( &std_data->mzf_header, mzf_header, sizeof ( st_MZF_HEADER ) );
    /* hlavička do LE (jen pořadí bajtů, 11h beze změny) */
    mzf_header_swap_endianity ( &std_data->mzf_header );
    std_data->body_size = endianity_bswap16_LE ( body_size );

    /* zapis tela */
    if ( body && body_size > 0 ) {
        memcpy ( block->data + 4 + sizeof ( st_TMZ_MZ_STANDARD_DATA ), body, body_size );
    }

    return block;
}


/**
 * @brief Vytvori novy blok 0x41 (MZ Turbo Data) z parametru.
 *
 * @param params Parametry turbo bloku (vsechna pole v host byte order).
 * @param body Data tela.
 * @param body_size Velikost tela v bajtech.
 * @return Novy blok, nebo NULL pri chybe.
 *
 * @pre params nesmi byt NULL.
 * @post Volajici vlastni vraceny blok a musi ho uvolnit pres tmz_block_free().
 */
st_TZX_BLOCK* tmz_block_create_mz_turbo ( const st_TMZ_MZ_TURBO_DATA *params, const uint8_t *body, uint16_t body_size ) {

    if ( !params ) return NULL;

    uint32_t data_length = sizeof ( st_TMZ_MZ_TURBO_DATA ) + body_size;
    uint32_t total = 4 + data_length;

    st_TZX_BLOCK *block = g_tmzblocks_allocator->alloc0 ( sizeof ( st_TZX_BLOCK ) );
    if ( !block ) return NULL;

    block->id = TMZ_BLOCK_ID_MZ_TURBO_DATA;
    block->length = total;
    block->data = g_tmzblocks_allocator->alloc0 ( total );
    if ( !block->data ) {
        g_tmzblocks_allocator->free ( block );
        return NULL;
    }

    uint32_t le_length = endianity_bswap32_LE ( data_length );
    memcpy ( block->data, &le_length, 4 );

    st_TMZ_MZ_TURBO_DATA *turbo = (st_TMZ_MZ_TURBO_DATA*) ( block->data + 4 );
    *turbo = *params;

    /* konverze do LE */
    turbo->lgap_length = endianity_bswap16_LE ( turbo->lgap_length );
    turbo->sgap_length = endianity_bswap16_LE ( turbo->sgap_length );
    turbo->pause_ms = endianity_bswap16_LE ( turbo->pause_ms );
    turbo->long_high = endianity_bswap16_LE ( turbo->long_high );
    turbo->long_low = endianity_bswap16_LE ( turbo->long_low );
    turbo->short_high = endianity_bswap16_LE ( turbo->short_high );
    turbo->short_low = endianity_bswap16_LE ( turbo->short_low );
    turbo->body_size = endianity_bswap16_LE ( body_size );
    mzf_header_swap_endianity ( &turbo->mzf_header );

    if ( body && body_size > 0 ) {
        memcpy ( block->data + 4 + sizeof ( st_TMZ_MZ_TURBO_DATA ), body, body_size );
    }

    return block;
}


/**
 * @brief Vytvori novy blok 0x42 (MZ Extra Body) z parametru.
 *
 * Alokuje st_TZX_BLOCK a jeho data, naplni strukturou a telovymi daty.
 * Vicebajtova pole se konvertuji do little-endian pri zapisu.
 *
 * @param format Formatova varianta zaznamu.
 * @param speed Index rychlosti zaznamu.
 * @param pause_ms Pauza po bloku (ms).
 * @param body Data tela.
 * @param body_size Velikost tela v bajtech.
 * @return Novy blok, nebo NULL pri chybe.
 *
 * @post Volajici vlastni vraceny blok a musi ho uvolnit pres tmz_block_free().
 */
st_TZX_BLOCK* tmz_block_create_mz_extra_body ( en_TMZ_FORMAT format, en_CMTSPEED speed,
                                                 uint16_t pause_ms,
                                                 const uint8_t *body, uint16_t body_size ) {

    uint32_t data_length = sizeof ( st_TMZ_MZ_EXTRA_BODY ) + body_size;
    uint32_t total = 4 + data_length; /* 4B DWORD delka + data */

    st_TZX_BLOCK *block = g_tmzblocks_allocator->alloc0 ( sizeof ( st_TZX_BLOCK ) );
    if ( !block ) return NULL;

    block->id = TMZ_BLOCK_ID_MZ_EXTRA_BODY;
    block->length = total;
    block->data = g_tmzblocks_allocator->alloc0 ( total );
    if ( !block->data ) {
        g_tmzblocks_allocator->free ( block );
        return NULL;
    }

    /* zapis DWORD delky */
    uint32_t le_length = endianity_bswap32_LE ( data_length );
    memcpy ( block->data, &le_length, 4 );

    /* zapis struktury */
    st_TMZ_MZ_EXTRA_BODY *extra = (st_TMZ_MZ_EXTRA_BODY*) ( block->data + 4 );
    extra->format = (uint8_t) format;
    extra->speed = (uint8_t) speed;
    extra->pause_ms = endianity_bswap16_LE ( pause_ms );
    extra->body_size = endianity_bswap16_LE ( body_size );

    /* zapis tela */
    if ( body && body_size > 0 ) {
        memcpy ( block->data + 4 + sizeof ( st_TMZ_MZ_EXTRA_BODY ), body, body_size );
    }

    return block;
}


/**
 * @brief Vytvori novy blok 0x44 (MZ Loader) z parametru.
 *
 * Alokuje st_TZX_BLOCK a jeho data, naplni strukturou a binarkou loaderu.
 * Vicebajtova pole se konvertuji do little-endian pri zapisu.
 *
 * @param loader_type Typ loaderu.
 * @param speed Rychlost pro nasledujici body bloky.
 * @param mzf_header MZF hlavicka loaderu (128B).
 * @param loader_data Data loaderu (strojovy kod).
 * @param loader_size Velikost dat loaderu v bajtech.
 * @return Novy blok, nebo NULL pri chybe.
 *
 * @pre mzf_header nesmi byt NULL.
 * @post Volajici vlastni vraceny blok a musi ho uvolnit pres tmz_block_free().
 */
st_TZX_BLOCK* tmz_block_create_mz_loader ( en_TMZ_LOADER_TYPE loader_type, en_CMTSPEED speed,
                                             const st_MZF_HEADER *mzf_header,
                                             const uint8_t *loader_data, uint16_t loader_size ) {

    if ( !mzf_header ) return NULL;

    uint32_t data_length = sizeof ( st_TMZ_MZ_LOADER ) + loader_size;
    uint32_t total = 4 + data_length; /* 4B DWORD delka + data */

    st_TZX_BLOCK *block = g_tmzblocks_allocator->alloc0 ( sizeof ( st_TZX_BLOCK ) );
    if ( !block ) return NULL;

    block->id = TMZ_BLOCK_ID_MZ_LOADER;
    block->length = total;
    block->data = g_tmzblocks_allocator->alloc0 ( total );
    if ( !block->data ) {
        g_tmzblocks_allocator->free ( block );
        return NULL;
    }

    /* zapis DWORD delky */
    uint32_t le_length = endianity_bswap32_LE ( data_length );
    memcpy ( block->data, &le_length, 4 );

    /* zapis struktury */
    st_TMZ_MZ_LOADER *loader = (st_TMZ_MZ_LOADER*) ( block->data + 4 );
    loader->loader_type = (uint8_t) loader_type;
    loader->speed = (uint8_t) speed;
    loader->loader_size = endianity_bswap16_LE ( loader_size );
    memcpy ( &loader->mzf_header, mzf_header, sizeof ( st_MZF_HEADER ) );
    mzf_header_swap_endianity ( &loader->mzf_header );

    /* zapis dat loaderu */
    if ( loader_data && loader_size > 0 ) {
        memcpy ( block->data + 4 + sizeof ( st_TMZ_MZ_LOADER ), loader_data, loader_size );
    }

    return block;
}


/**
 * @brief Vytvori novy blok 0x43 (MZ Machine Info) z parametru.
 *
 * @param machine Typ stroje.
 * @param cpu_clock CPU takt v Hz.
 * @param rom_version ROM verze (0 = neznama).
 * @return Novy blok, nebo NULL pri chybe.
 *
 * @post Volajici vlastni vraceny blok a musi ho uvolnit pres tmz_block_free().
 */
st_TZX_BLOCK* tmz_block_create_mz_machine_info ( en_TMZ_MACHINE machine, uint32_t cpu_clock, uint8_t rom_version ) {

    uint32_t data_length = sizeof ( st_TMZ_MZ_MACHINE_INFO );
    uint32_t total = 4 + data_length;

    st_TZX_BLOCK *block = g_tmzblocks_allocator->alloc0 ( sizeof ( st_TZX_BLOCK ) );
    if ( !block ) return NULL;

    block->id = TMZ_BLOCK_ID_MZ_MACHINE_INFO;
    block->length = total;
    block->data = g_tmzblocks_allocator->alloc0 ( total );
    if ( !block->data ) {
        g_tmzblocks_allocator->free ( block );
        return NULL;
    }

    uint32_t le_length = endianity_bswap32_LE ( data_length );
    memcpy ( block->data, &le_length, 4 );

    st_TMZ_MZ_MACHINE_INFO *info = (st_TMZ_MZ_MACHINE_INFO*) ( block->data + 4 );
    info->machine = (uint8_t) machine;
    info->cpu_clock = endianity_bswap32_LE ( cpu_clock );
    info->rom_version = rom_version;

    return block;
}


/**
 * @brief Vytvori novy blok 0x45 (MZ BASIC Data) z parametru.
 *
 * Alokuje st_TZX_BLOCK a naplni ho hlavickou a chunkovymi daty.
 * Kazdy chunk ma 258 bajtu (2B ID LE + 256B data).
 *
 * @param machine Cilovy stroj.
 * @param pulseset Pulsni sada.
 * @param pause_ms Pauza po bloku (ms).
 * @param mzf_header MZF hlavicka (ftype=0x03/0x04, fsize/fstrt/fexec=0).
 * @param chunks Pole chunku (chunk_count * 258 B).
 * @param chunk_count Pocet chunku.
 * @return Novy blok, nebo NULL pri chybe.
 *
 * @pre mzf_header nesmi byt NULL.
 * @post Volajici vlastni vraceny blok a musi ho uvolnit pres tmz_block_free().
 */
st_TZX_BLOCK* tmz_block_create_mz_basic_data ( en_TMZ_MACHINE machine, en_TMZ_PULSESET pulseset,
                                                 uint16_t pause_ms, const st_MZF_HEADER *mzf_header,
                                                 const uint8_t *chunks, uint16_t chunk_count ) {

    if ( !mzf_header ) return NULL;

    uint32_t chunks_size = (uint32_t) chunk_count * TMZ_BASIC_CHUNK_SIZE;
    uint32_t data_length = sizeof ( st_TMZ_MZ_BASIC_DATA ) + chunks_size;
    uint32_t total = 4 + data_length;

    st_TZX_BLOCK *block = g_tmzblocks_allocator->alloc0 ( sizeof ( st_TZX_BLOCK ) );
    if ( !block ) return NULL;

    block->id = TMZ_BLOCK_ID_MZ_BASIC_DATA;
    block->length = total;
    block->data = g_tmzblocks_allocator->alloc0 ( total );
    if ( !block->data ) {
        g_tmzblocks_allocator->free ( block );
        return NULL;
    }

    /* zapis DWORD delky */
    uint32_t le_length = endianity_bswap32_LE ( data_length );
    memcpy ( block->data, &le_length, 4 );

    /* zapis struktury */
    st_TMZ_MZ_BASIC_DATA *bsd = (st_TMZ_MZ_BASIC_DATA*) ( block->data + 4 );
    bsd->machine = (uint8_t) machine;
    bsd->pulseset = (uint8_t) pulseset;
    bsd->pause_ms = endianity_bswap16_LE ( pause_ms );
    memcpy ( &bsd->mzf_header, mzf_header, sizeof ( st_MZF_HEADER ) );
    mzf_header_swap_endianity ( &bsd->mzf_header );
    bsd->chunk_count = endianity_bswap16_LE ( chunk_count );

    /* zapis chunku */
    if ( chunks && chunks_size > 0 ) {
        memcpy ( block->data + 4 + sizeof ( st_TMZ_MZ_BASIC_DATA ), chunks, chunks_size );
    }

    return block;
}


/* ========================================================================= */
/*  Konverze MZF <-> TMZ                                                     */
/* ========================================================================= */

/**
 * @brief Vytvori TMZ blok 0x40 (MZ Standard Data) z MZF struktury.
 *
 * Prevezme hlavicku a telo z st_MZF a vytvori standardni TMZ blok
 * pro NORMAL 1200 Bd format.
 *
 * @param mzf MZF struktura (hlavicka + telo).
 * @param machine Cilovy stroj.
 * @param pulseset Pulsni sada.
 * @param pause_ms Pauza po bloku (ms).
 * @return Novy blok, nebo NULL pri chybe.
 *
 * @pre mzf nesmi byt NULL.
 * @post Volajici vlastni vraceny blok a musi ho uvolnit pres tmz_block_free().
 */
st_TZX_BLOCK* tmz_block_from_mzf ( const st_MZF *mzf, en_TMZ_MACHINE machine,
                                    en_TMZ_PULSESET pulseset, uint16_t pause_ms ) {

    if ( !mzf ) return NULL;

    return tmz_block_create_mz_standard ( machine, pulseset, pause_ms,
                                          &mzf->header, mzf->body, (uint16_t) mzf->header.fsize );
}


/**
 * @brief Extrahuje MZF strukturu z TMZ bloku 0x40 nebo 0x41.
 *
 * Vytvori novou st_MZF strukturu s kopiemi hlavicky a tela.
 * Podporuje bloky 0x40 (MZ Standard Data) a 0x41 (MZ Turbo Data).
 *
 * @param block TMZ blok s ID 0x40 nebo 0x41.
 * @param[out] err Vystupni chybovy kod (muze byt NULL).
 * @return Nova st_MZF struktura, nebo NULL pri chybe.
 *
 * @pre block nesmi byt NULL, block->id musi byt 0x40 nebo 0x41.
 * @post Volajici vlastni vracenou strukturu a musi ji uvolnit pres mzf_free().
 */
st_MZF* tmz_block_to_mzf ( const st_TZX_BLOCK *block, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block ) {
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    const st_MZF_HEADER *src_header = NULL;
    const uint8_t *src_body = NULL;
    uint16_t body_size = 0;

    if ( block->id == TMZ_BLOCK_ID_MZ_STANDARD_DATA ) {
        uint8_t *body_data = NULL;
        st_TMZ_MZ_STANDARD_DATA *std_data = tmz_block_parse_mz_standard ( block, &body_data, err );
        if ( !std_data ) return NULL;
        src_header = &std_data->mzf_header;
        src_body = body_data;
        body_size = std_data->body_size;
    } else if ( block->id == TMZ_BLOCK_ID_MZ_TURBO_DATA ) {
        uint8_t *body_data = NULL;
        st_TMZ_MZ_TURBO_DATA *turbo_data = tmz_block_parse_mz_turbo ( block, &body_data, err );
        if ( !turbo_data ) return NULL;
        src_header = &turbo_data->mzf_header;
        src_body = body_data;
        body_size = turbo_data->body_size;
    } else if ( block->id == TMZ_BLOCK_ID_MZ_FILE ) {
        st_TMZ_MZFILE mf;
        *err = tmz_mzfile_parse ( block, &mf );
        if ( *err != TZX_OK ) return NULL;
        tmz_mzfile_free ( &mf );   /* header a body ukazují do dat bloku, pole částí není potřeba */
        if ( mf.body_size > 0xFFFF ) {
            g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x46 body too large for MZF: %u bytes\n", mf.body_size );
            *err = TZX_ERROR_INVALID_BLOCK;
            return NULL;
        }
        /* MZF nese délku těla ve fsize; blok 0x46 ji smí mít jinou (spec 4.1, rozhoduje N) */
        uint16_t hdr_fsize = ( uint16_t ) ( mf.header[0x12] | ( mf.header[0x13] << 8 ) );
        if ( hdr_fsize != mf.body_size ) {
            g_tmzblocks_error_cb ( __func__, __LINE__, "Block 0x46 MZF fsize %u differs from body size %u\n",
                                   hdr_fsize, mf.body_size );
            *err = TZX_ERROR_INVALID_BLOCK;
            return NULL;
        }
        src_header = ( const st_MZF_HEADER* ) mf.header;
        src_body = mf.body;
        body_size = ( uint16_t ) mf.body_size;
    } else {
        g_tmzblocks_error_cb ( __func__, __LINE__, "Unsupported block ID 0x%02X for MZF extraction\n", block->id );
        *err = TZX_ERROR_INVALID_BLOCK;
        return NULL;
    }

    /* alokace MZF struktury */
    st_MZF *mzf = g_tmzblocks_allocator->alloc0 ( sizeof ( st_MZF ) );
    if ( !mzf ) {
        *err = TZX_ERROR_ALLOC;
        return NULL;
    }

    memcpy ( &mzf->header, src_header, sizeof ( st_MZF_HEADER ) );
    if ( block->id == TMZ_BLOCK_ID_MZ_FILE ) {
        /* hlavička bloku 0x46 je v pořadí souboru (LE): do pořadí hostitele jako u 0x40/0x41, 11h beze změny */
        mzf->header.fsize = endianity_bswap16_LE ( mzf->header.fsize );
        mzf->header.fstrt = endianity_bswap16_LE ( mzf->header.fstrt );
        mzf->header.fexec = endianity_bswap16_LE ( mzf->header.fexec );
    }

    if ( body_size > 0 && src_body ) {
        mzf->body = g_tmzblocks_allocator->alloc ( body_size );
        if ( !mzf->body ) {
            g_tmzblocks_allocator->free ( mzf );
            *err = TZX_ERROR_ALLOC;
            return NULL;
        }
        memcpy ( mzf->body, src_body, body_size );
        mzf->body_size = body_size;
    } else {
        mzf->body = NULL;
        mzf->body_size = 0;
    }

    *err = TZX_OK;
    return mzf;
}


/**
 * @brief Uvolni TMZ blok vcetne jeho dat.
 *
 * Bezpecne volat s NULL (no-op).
 *
 * @param block Ukazatel na blok k uvolneni (muze byt NULL).
 */
void tmz_block_free ( st_TZX_BLOCK *block ) {
    if ( !block ) return;
    if ( block->data ) g_tmzblocks_allocator->free ( block->data );
    g_tmzblocks_allocator->free ( block );
}
