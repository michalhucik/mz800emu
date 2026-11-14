/**
 * @file   mzf.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.1
 * @brief  Implementace nízkoúrovňového I/O, validace a lifecycle API pro MZF formát.
 *
 * Implementace nízkoúrovňového I/O, validace a lifecycle API
 * pro MZF souborový formát. Viz mzf.h pro popis formátu.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-11-09: popisy typů souborů 02h-05h podle dokumentace BASICu 1Z-016 (2.1.1)
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
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "mzf.h"
#include "../generic_driver/generic_driver.h"
#include "../endianity/endianity.h"


/* ═══════════════════════════════════════════════════════════════════════
 * Interní pomocné funkce
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Zapíše 16-bit hodnotu v little-endian pořadí na daný offset.
 *
 * Bajty se zapisují explicitně (low byte první, high byte druhý),
 * čímž se zajistí správný LE formát nezávisle na hostitelském byte-order.
 *
 * @param h      Otevřený handler pro zápis
 * @param offset Byte offset v datovém zdroji
 * @param w      16-bit hodnota k zápisu
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
 */
static int mzf_write_word_le ( st_HANDLER *h, uint32_t offset, uint16_t w ) {
    uint8_t lo = w & 0xff;
    if ( EXIT_SUCCESS != generic_driver_write ( h, offset, &lo, 1 ) ) return EXIT_FAILURE;
    uint8_t hi = ( w >> 8 ) & 0xff;
    return generic_driver_write ( h, offset + 1, &hi, 1 );
}


/* ═══════════════════════════════════════════════════════════════════════
 * Konverze endianity
 * ═══════════════════════════════════════════════════════════════════════ */

/** @copydoc mzf_header_swap_endianity */
void mzf_header_swap_endianity ( st_MZF_HEADER *mzfhdr ) {
    if ( !mzfhdr ) return;
    mzfhdr->fsize = endianity_bswap16_LE ( mzfhdr->fsize );
    mzfhdr->fstrt = endianity_bswap16_LE ( mzfhdr->fstrt );
    mzfhdr->fexec = endianity_bswap16_LE ( mzfhdr->fexec );
}


/** @copydoc mzf_header_items_correction */
void mzf_header_items_correction ( st_MZF_HEADER *mzfhdr ) {
    mzfhdr->fsize = endianity_bswap16_LE ( mzfhdr->fsize );
    mzfhdr->fstrt = endianity_bswap16_LE ( mzfhdr->fstrt );
    mzfhdr->fexec = endianity_bswap16_LE ( mzfhdr->fexec );
    mzfhdr->fname.terminator = MZF_FNAME_TERMINATOR;
}


/* ═══════════════════════════════════════════════════════════════════════
 * Nízkoúrovňové čtení/zápis hlavičky
 * ═══════════════════════════════════════════════════════════════════════ */

/** @copydoc mzf_read_header_on_offset */
int mzf_read_header_on_offset ( st_HANDLER *h, uint32_t offset, st_MZF_HEADER *mzfhdr ) {
    if ( EXIT_SUCCESS != generic_driver_read ( h, offset, mzfhdr, sizeof ( st_MZF_HEADER ) ) ) return EXIT_FAILURE;
    mzf_header_items_correction ( mzfhdr );
    return EXIT_SUCCESS;
}


/** @copydoc mzf_read_header */
int mzf_read_header ( st_HANDLER *h, st_MZF_HEADER *mzfhdr ) {
    return mzf_read_header_on_offset ( h, 0, mzfhdr );
}


/** @copydoc mzf_write_header_on_offset */
int mzf_write_header_on_offset ( st_HANDLER *h, uint32_t offset, const st_MZF_HEADER *mzfhdr ) {
    /* Zápis ftype + fname (18 bajtů) — tyto pole nevyžadují konverzi */
    uint32_t pos = offset;
    uint32_t fname_block_size = sizeof ( mzfhdr->ftype ) + MZF_FNAME_FULL_LENGTH;

    if ( EXIT_SUCCESS != generic_driver_write ( h, pos, (void*) mzfhdr, fname_block_size ) ) return EXIT_FAILURE;
    pos += fname_block_size;

    /* Zápis uint16 polí explicitně v LE pořadí */
    if ( EXIT_SUCCESS != mzf_write_word_le ( h, pos, mzfhdr->fsize ) ) return EXIT_FAILURE;
    pos += sizeof ( mzfhdr->fsize );

    if ( EXIT_SUCCESS != mzf_write_word_le ( h, pos, mzfhdr->fstrt ) ) return EXIT_FAILURE;
    pos += sizeof ( mzfhdr->fstrt );

    if ( EXIT_SUCCESS != mzf_write_word_le ( h, pos, mzfhdr->fexec ) ) return EXIT_FAILURE;
    pos += sizeof ( mzfhdr->fexec );

    /* Zápis komentáře */
    if ( EXIT_SUCCESS != generic_driver_write ( h, pos, (void*) mzfhdr->cmnt, MZF_CMNT_LENGTH ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/** @copydoc mzf_write_header */
int mzf_write_header ( st_HANDLER *h, const st_MZF_HEADER *mzfhdr ) {
    return mzf_write_header_on_offset ( h, 0, mzfhdr );
}


/* ═══════════════════════════════════════════════════════════════════════
 * Nízkoúrovňové čtení/zápis těla
 * ═══════════════════════════════════════════════════════════════════════ */

/** @copydoc mzf_read_body_on_offset */
int mzf_read_body_on_offset ( st_HANDLER *h, uint32_t offset, uint8_t *buffer, uint32_t buffer_size ) {
    return generic_driver_read ( h, offset, buffer, buffer_size );
}


/** @copydoc mzf_read_body */
int mzf_read_body ( st_HANDLER *h, uint8_t *buffer, uint32_t buffer_size ) {
    return mzf_read_body_on_offset ( h, sizeof ( st_MZF_HEADER ), buffer, buffer_size );
}


/** @copydoc mzf_write_body_on_offset */
int mzf_write_body_on_offset ( st_HANDLER *h, uint32_t offset, const uint8_t *buffer, uint32_t buffer_size ) {
    return generic_driver_write ( h, offset, (void*) buffer, buffer_size );
}


/** @copydoc mzf_write_body */
int mzf_write_body ( st_HANDLER *h, const uint8_t *buffer, uint32_t buffer_size ) {
    return mzf_write_body_on_offset ( h, sizeof ( st_MZF_HEADER ), buffer, buffer_size );
}


/* ═══════════════════════════════════════════════════════════════════════
 * Chybové zprávy
 * ═══════════════════════════════════════════════════════════════════════ */

/** @copydoc mzf_error_message */
const char* mzf_error_message ( st_HANDLER *h, st_DRIVER *d ) {
    return generic_driver_error_message ( h, d );
}


/** @copydoc mzf_error_string */
const char* mzf_error_string ( en_MZF_ERROR err ) {
    switch ( err ) {
        case MZF_OK:                        return "OK";
        case MZF_ERROR_IO:                  return "I/O error";
        case MZF_ERROR_INVALID_HEADER:      return "invalid MZF header";
        case MZF_ERROR_NO_FNAME_TERMINATOR: return "missing filename terminator (0x0d)";
        case MZF_ERROR_SIZE_MISMATCH:       return "file size does not match fsize field";
        case MZF_ERROR_ALLOC:               return "memory allocation failed";
    }
    return "unknown error";
}


/* ═══════════════════════════════════════════════════════════════════════
 * Test terminátoru jména
 * ═══════════════════════════════════════════════════════════════════════ */

/** @copydoc mzf_header_test_fname_terminator_on_offset */
int mzf_header_test_fname_terminator_on_offset ( st_HANDLER *h, uint32_t offset ) {
    st_MZF_FILENAME mzffname;
    if ( EXIT_SUCCESS != generic_driver_read ( h, offset, &mzffname, sizeof ( mzffname ) ) ) return EXIT_FAILURE;

    for ( uint32_t i = 0; i < sizeof ( mzffname ); i++ ) {
        if ( ( (uint8_t*) &mzffname )[i] == MZF_FNAME_TERMINATOR ) {
            return EXIT_SUCCESS;
        }
    }
    return EXIT_FAILURE;
}


/** @copydoc mzf_header_test_fname_terminator */
int mzf_header_test_fname_terminator ( st_HANDLER *h ) {
    return mzf_header_test_fname_terminator_on_offset ( h, 1 );
}


/* ═══════════════════════════════════════════════════════════════════════
 * Validační API
 * ═══════════════════════════════════════════════════════════════════════ */

/** @copydoc mzf_header_validate */
en_MZF_ERROR mzf_header_validate ( const st_MZF_HEADER *mzfhdr ) {
    if ( mzfhdr == NULL ) return MZF_ERROR_INVALID_HEADER;

    /* Kontrola přítomnosti terminátoru v poli jména */
    int has_terminator = 0;
    for ( unsigned i = 0; i < MZF_FNAME_FULL_LENGTH; i++ ) {
        if ( ( (const uint8_t*) &mzfhdr->fname )[i] == MZF_FNAME_TERMINATOR ) {
            has_terminator = 1;
            break;
        }
    }
    if ( !has_terminator ) return MZF_ERROR_NO_FNAME_TERMINATOR;

    return MZF_OK;
}


/** @copydoc mzf_file_validate */
en_MZF_ERROR mzf_file_validate ( st_HANDLER *h ) {
    /* Načtení hlavičky */
    st_MZF_HEADER hdr;
    if ( EXIT_SUCCESS != mzf_read_header ( h, &hdr ) ) return MZF_ERROR_IO;

    /* Validace hlavičky */
    en_MZF_ERROR err = mzf_header_validate ( &hdr );
    if ( err != MZF_OK ) return err;

    /* Kontrola velikosti — soubor musí obsahovat minimálně header + fsize bajtů.
     * U paměťového handleru kontrolujeme reálnou velikost bufferu,
     * u souborového handleru to nelze snadno zjistit — přeskočíme. */
    if ( h->type == HANDLER_TYPE_MEMORY ) {
        st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
        uint32_t expected = MZF_HEADER_SIZE + hdr.fsize;
        if ( memspec->size < expected ) return MZF_ERROR_SIZE_MISMATCH;
    }

    return MZF_OK;
}


/* ═══════════════════════════════════════════════════════════════════════
 * Lifecycle API
 * ═══════════════════════════════════════════════════════════════════════ */

/** @copydoc mzf_load */
st_MZF* mzf_load ( st_HANDLER *h, en_MZF_ERROR *err ) {
    /* Alokace struktury */
    st_MZF *mzf = (st_MZF*) malloc ( sizeof ( st_MZF ) );
    if ( !mzf ) {
        if ( err ) *err = MZF_ERROR_ALLOC;
        return NULL;
    }
    memset ( mzf, 0, sizeof ( st_MZF ) );

    /* Načtení hlavičky */
    if ( EXIT_SUCCESS != mzf_read_header ( h, &mzf->header ) ) {
        free ( mzf );
        if ( err ) *err = MZF_ERROR_IO;
        return NULL;
    }

    /* Načtení těla (pokud má nenulovou velikost) */
    if ( mzf->header.fsize > 0 ) {
        mzf->body = (uint8_t*) malloc ( mzf->header.fsize );
        if ( !mzf->body ) {
            free ( mzf );
            if ( err ) *err = MZF_ERROR_ALLOC;
            return NULL;
        }
        mzf->body_size = mzf->header.fsize;

        if ( EXIT_SUCCESS != mzf_read_body ( h, mzf->body, mzf->header.fsize ) ) {
            free ( mzf->body );
            free ( mzf );
            if ( err ) *err = MZF_ERROR_IO;
            return NULL;
        }
    }

    if ( err ) *err = MZF_OK;
    return mzf;
}


/** @copydoc mzf_save */
en_MZF_ERROR mzf_save ( st_HANDLER *h, const st_MZF *mzf ) {
    if ( !mzf ) return MZF_ERROR_INVALID_HEADER;

    /* Zápis hlavičky */
    if ( EXIT_SUCCESS != mzf_write_header ( h, &mzf->header ) ) return MZF_ERROR_IO;

    /* Zápis těla (pokud existuje) */
    if ( mzf->body != NULL && mzf->header.fsize > 0 ) {
        if ( EXIT_SUCCESS != mzf_write_body ( h, mzf->body, mzf->header.fsize ) ) return MZF_ERROR_IO;
    }

    return MZF_OK;
}


/** @copydoc mzf_free */
void mzf_free ( st_MZF *mzf ) {
    if ( !mzf ) return;
    if ( mzf->body ) {
        free ( mzf->body );
    }
    free ( mzf );
}


const char* mzf_version ( void ) {
    return MZF_VERSION;
}
