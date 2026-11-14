/**
 * @file   dsk.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Implementace nízkoúrovňového API pro Extended CPC DSK formát.
 *
 * Čtení/zápis sektorů, výpočty offsetů, parsování hlaviček,
 * získání geometrie disku.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
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
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsk.h"
#include "libs/generic_driver/generic_driver.h"


/**
 * @brief Vrátí textový popis poslední chyby.
 *
 * Pokud je chyba na úrovni generic_driver, deleguje na generic_driver_error_message().
 *
 * @param h Handler DSK operace.
 * @param d Driver pro I/O operace.
 * @return Ukazatel na statický řetězec s popisem chyby.
 */
const char* dsk_error_message ( st_HANDLER *h, st_DRIVER *d ) {

    if ( d->err != GENERIC_DRIVER_ERROR_NONE ) return generic_driver_error_message ( h, d );

    static const char *dsk_err_msg[] = {
        "no error",
        "image not ready",
        "image is write protected",
        "track not found",
        "sector not found",
        "not available on double-sided disk",
        "no tracks",
        "invalid parameter"
    };

    if ( (en_DSK_ERROR) h->err >= DSK_ERROR_UNKNOWN ) return "unknown error";

    return dsk_err_msg[h->err];
}


/**
 * Výpočet offsetu pro absolutní stopu.
 *
 * @param abstrack Absolutní číslo stopy (0 .. total_tracks-1)
 * @param tsizes Pole velikostí stop z hlavičky DSK (min DSK_MAX_TOTAL_TRACKS prvků)
 * @return Offset v bajtech od začátku souboru
 */
uint32_t dsk_compute_track_offset ( uint8_t abstrack, const uint8_t *tsizes ) {
    if ( tsizes == NULL ) return sizeof ( st_DSK_HEADER );

    uint32_t offset = sizeof ( st_DSK_HEADER );
    int i;
    for ( i = 0; i < abstrack; i++ ) {
        offset += dsk_decode_track_size ( tsizes[i] );
    }
    return offset;
}


/**
 * Výpočet offsetu sektoru v rámci stopy.
 *
 * @param sector ID sektoru
 * @param tinfo Informace o stopě
 * @return Offset v rámci stopy, nebo -1 pokud sektor nenalezen
 */
int32_t dsk_compute_sector_offset ( uint8_t sector, const st_DSK_SHORT_TRACK_INFO *tinfo ) {
    if ( tinfo == NULL ) return -1;

    uint32_t offset = sizeof ( st_DSK_TRACK_INFO );
    uint32_t ssize_bytes = dsk_decode_sector_size ( tinfo->ssize );
    int i;
    for ( i = 0; i < tinfo->sectors; i++ ) {
        if ( tinfo->sinfo[i] == sector ) {
            return offset;
        }
        /* Pokud stopa podporuje per-sector sizes, použijeme je */
        uint32_t this_sector_size = ( tinfo->sector_sizes[i] != 0 )
            ? dsk_decode_sector_size ( tinfo->sector_sizes[i] )
            : ssize_bytes;
        offset += this_sector_size;
    }
    return -1;
}


/**
 * Přečte image info a uloží jej do short_image_info struktury.
 *
 * @param h Handler
 * @param short_image_info Výstupní struktura
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_read_short_image_info ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;

    if ( short_image_info == NULL ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_INVALID_PARAM;
        return EXIT_FAILURE;
    }

    uint8_t tmpbuffer [ DSK_MAX_TOTAL_TRACKS ];
    uint8_t *buffer = NULL;

    uint32_t offset = DSK_FILEINFO_FIELD_LENGTH + DSK_CREATOR_FIELD_LENGTH;

    if ( EXIT_SUCCESS != generic_driver_prepare ( h, offset, (void**) &buffer, &tmpbuffer, 2 ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buffer, 2 ) ) return EXIT_FAILURE;

    short_image_info->sides = ( buffer[1] <= 1 ) ? 1 : 2;
    short_image_info->tracks = buffer[0];
    short_image_info->header_tracks = buffer[0]; /* raw hodnota před ořezem (audit H-11) */

    /* Tichá redukce na DSK_MAX_TOTAL_TRACKS je záměrná - umožňuje
     * vyšším vrstvám (dsk_tools_diagnose/repair) detekovat a opravit
     * TRACKCOUNT_EXCEEDED v malformované hlavičce. Rozdíl
     * `header_tracks > tracks` signalizuje, že vrstvy nad driverem
     * mají situaci reportovat a nabídnout repair.
     *
     * Výpočet dělám v uint16_t, aby nedošlo k uint8_t overflow
     * (např. tracks=128, sides=2 → 256 → ořez na 0 = nulová
     * geometrie). Audit MSYS2 M-31. */
    uint16_t total_tracks_u16 = (uint16_t) short_image_info->tracks * short_image_info->sides;

    if ( total_tracks_u16 > DSK_MAX_TOTAL_TRACKS ) {
        total_tracks_u16 = DSK_MAX_TOTAL_TRACKS;
        short_image_info->tracks = (uint8_t) ( DSK_MAX_TOTAL_TRACKS / short_image_info->sides );
    }

    uint8_t total_tracks = (uint8_t) total_tracks_u16;

    offset += 4;

    if ( EXIT_SUCCESS != generic_driver_prepare ( h, offset, (void**) &buffer, &tmpbuffer, total_tracks ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buffer, total_tracks ) ) return EXIT_FAILURE;

    memcpy ( short_image_info->tsize, buffer, total_tracks );

    return EXIT_SUCCESS;
}


/**
 * Přečte informaci o stopě na daném offsetu.
 *
 * @param h Handler
 * @param track_offset Offset stopy v souboru
 * @param short_track_info Výstupní struktura
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_read_short_track_info_on_offset ( st_HANDLER *h, uint32_t track_offset, st_DSK_SHORT_TRACK_INFO *short_track_info ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;

    if ( short_track_info == NULL ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_INVALID_PARAM;
        return EXIT_FAILURE;
    }

    uint8_t tmpbuffer [ 6 ];
    uint8_t *buffer = NULL;

    uint32_t offset = track_offset + DSK_TRACKINFO_FIELD_LENGTH + 4;

    if ( EXIT_SUCCESS != generic_driver_prepare ( h, offset, (void**) &buffer, &tmpbuffer, 6 ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buffer, 6 ) ) return EXIT_FAILURE;

    short_track_info->track = buffer[0];
    short_track_info->side = buffer[1];
    short_track_info->ssize = buffer[4];
    short_track_info->sectors = buffer[5];

    /* Ochrana proti malformovanému DSK: `sectors` je uint8_t (0..255), ale
     * `short_track_info->sinfo[]` a `sector_sizes[]` mají jen DSK_MAX_SECTORS
     * prvků. Bez této kontroly by cyklus níže zapsal OOB do struktury. */
    if ( short_track_info->sectors > DSK_MAX_SECTORS ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_INVALID_PARAM;
        return EXIT_FAILURE;
    }

    /* Inicializovat per-sector sizes na 0 (= použij společnou velikost) */
    memset ( short_track_info->sector_sizes, 0, sizeof ( short_track_info->sector_sizes ) );

    offset += 8;

    int i;
    for ( i = 0; i < short_track_info->sectors; i++ ) {
        st_DSK_SECTOR_INFO tmpbuffer2;
        st_DSK_SECTOR_INFO *sinfo_buf = NULL;
        if ( EXIT_SUCCESS != generic_driver_prepare ( h, offset, (void**) &sinfo_buf, &tmpbuffer2, sizeof ( st_DSK_SECTOR_INFO ) ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != generic_driver_read ( h, offset, sinfo_buf, sizeof ( st_DSK_SECTOR_INFO ) ) ) return EXIT_FAILURE;
        offset += sizeof ( st_DSK_SECTOR_INFO );
        short_track_info->sinfo[i] = sinfo_buf->sector;
        short_track_info->sector_sizes[i] = sinfo_buf->ssize;
    }

    return EXIT_SUCCESS;
}


/**
 * Přečte informaci o stopě podle absolutního čísla.
 *
 * @param h Handler
 * @param short_image_info Informace o obrazu (NULL = načte se automaticky)
 * @param abstrack Absolutní číslo stopy
 * @param short_track_info Výstupní struktura
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_read_short_track_info ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, uint8_t abstrack, st_DSK_SHORT_TRACK_INFO *short_track_info ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;

    uint32_t track_offset = sizeof ( st_DSK_HEADER );

    if ( abstrack != 0 ) {
        st_DSK_SHORT_IMAGE_INFO local_short_image_info;
        st_DSK_SHORT_IMAGE_INFO *iinfo = short_image_info;

        if ( iinfo == NULL ) {
            if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &local_short_image_info ) ) return EXIT_FAILURE;
            iinfo = &local_short_image_info;
        }

        if ( abstrack >= ( iinfo->tracks * iinfo->sides ) ) {
            h->err = (en_HANDLER_ERROR) DSK_ERROR_TRACK_NOT_FOUND;
            return EXIT_FAILURE;
        }

        track_offset = dsk_compute_track_offset ( abstrack, iinfo->tsize );
    }

    if ( EXIT_SUCCESS != dsk_read_short_track_info_on_offset ( h, track_offset, short_track_info ) ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


/**
 * Pro požadovaný sektor na konkrétní stopě získá offset a velikost v bajtech.
 *
 * @param h Handler
 * @param short_image_info Může být NULL — načte se automaticky
 * @param short_track_info Může být NULL — načte se automaticky
 * @param abstrack Absolutní stopa
 * @param sector ID sektoru
 * @param sector_offset Výstup: absolutní pozice sektoru v souboru
 * @param ssize_bytes Výstup: velikost sektoru v bajtech
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_read_short_sector_info ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, st_DSK_SHORT_TRACK_INFO *short_track_info, uint8_t abstrack, uint8_t sector, uint32_t *sector_offset, uint16_t *ssize_bytes ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;

    if ( sector_offset == NULL || ssize_bytes == NULL ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_INVALID_PARAM;
        return EXIT_FAILURE;
    }

    *sector_offset = 0;
    *ssize_bytes = 0;

    st_DSK_SHORT_IMAGE_INFO local_short_image_info;
    st_DSK_SHORT_IMAGE_INFO *iinfo = short_image_info;

    if ( iinfo == NULL ) {
        if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &local_short_image_info ) ) return EXIT_FAILURE;
        iinfo = &local_short_image_info;
    }

    if ( ( abstrack >= ( iinfo->tracks * iinfo->sides ) ) || ( iinfo->tsize[abstrack] == 0 ) ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_TRACK_NOT_FOUND;
        return EXIT_FAILURE;
    }

    uint32_t track_offset = dsk_compute_track_offset ( abstrack, iinfo->tsize );

    st_DSK_SHORT_TRACK_INFO local_short_track_info;
    st_DSK_SHORT_TRACK_INFO *tinfo = short_track_info;

    if ( tinfo == NULL ) {
        if ( EXIT_SUCCESS != dsk_read_short_track_info_on_offset ( h, track_offset, &local_short_track_info ) ) return EXIT_FAILURE;
        tinfo = &local_short_track_info;
    }

    int32_t sector_on_track_offset = dsk_compute_sector_offset ( sector, tinfo );

    if ( sector_on_track_offset == -1 ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_SECTOR_NOT_FOUND;
        return EXIT_FAILURE;
    }

    *sector_offset = track_offset + sector_on_track_offset;
    *ssize_bytes = dsk_decode_sector_size ( tinfo->ssize );

    return EXIT_SUCCESS;
}


/**
 * Provede operaci čtení nebo zápisu na konkrétním absolutním sektoru.
 *
 * @param h Handler
 * @param rwop Typ operace (READ / WRITE)
 * @param short_image_info Může být NULL
 * @param short_track_info Může být NULL
 * @param abstrack Absolutní stopa
 * @param sector ID sektoru
 * @param buffer Datový buffer
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_rw_sector ( st_HANDLER *h, en_DSK_RWOP rwop, st_DSK_SHORT_IMAGE_INFO *short_image_info, st_DSK_SHORT_TRACK_INFO *short_track_info, uint8_t abstrack, uint8_t sector, void *buffer ) {

    if ( buffer == NULL ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_INVALID_PARAM;
        return EXIT_FAILURE;
    }

    uint32_t sector_offset = 0;
    uint16_t ssize_bytes = 0;

    if ( EXIT_SUCCESS != dsk_read_short_sector_info ( h, short_image_info, short_track_info, abstrack, sector, &sector_offset, &ssize_bytes ) ) return EXIT_FAILURE;

    if ( DSK_RWOP_READ == rwop ) return generic_driver_read ( h, sector_offset, buffer, ssize_bytes );
    return generic_driver_write ( h, sector_offset, buffer, ssize_bytes );
}


/**
 * @brief Přečte data z libovolného offsetu v DSK souboru.
 * @param h Handler.
 * @param offset Offset v souboru.
 * @param buffer Cílový buffer.
 * @param buffer_size Počet bajtů ke čtení.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int dsk_read_on_offset ( st_HANDLER *h, uint32_t offset, void *buffer, uint16_t buffer_size ) {
    return generic_driver_read ( h, offset, buffer, buffer_size );
}


/**
 * @brief Zapíše data na libovolný offset v DSK souboru.
 * @param h Handler.
 * @param offset Offset v souboru.
 * @param buffer Zdrojový buffer.
 * @param buffer_size Počet bajtů k zápisu.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int dsk_write_on_offset ( st_HANDLER *h, uint32_t offset, const void *buffer, uint16_t buffer_size ) {
    return generic_driver_write ( h, offset, (void*) buffer, buffer_size );
}


/**
 * @brief Přečte celý sektor z DSK obrazu.
 * @param h Handler.
 * @param abstrack Absolutní stopa.
 * @param sector ID sektoru.
 * @param buffer Cílový buffer (musí být dostatečně velký pro velikost sektoru).
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int dsk_read_sector ( st_HANDLER *h, uint8_t abstrack, uint8_t sector, void *buffer ) {
    return dsk_rw_sector ( h, DSK_RWOP_READ, NULL, NULL, abstrack, sector, buffer );
}


/**
 * @brief Zapíše celý sektor do DSK obrazu.
 * @param h Handler.
 * @param abstrack Absolutní stopa.
 * @param sector ID sektoru.
 * @param buffer Zdrojový buffer s daty sektoru.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int dsk_write_sector ( st_HANDLER *h, uint8_t abstrack, uint8_t sector, void *buffer ) {
    return dsk_rw_sector ( h, DSK_RWOP_WRITE, NULL, NULL, abstrack, sector, buffer );
}


/**
 * Získá souhrnnou geometrii DSK obrazu.
 *
 * @param h Handler
 * @param geom Výstupní struktura
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_get_geometry ( st_HANDLER *h, st_DSK_GEOMETRY *geom ) {
    if ( geom == NULL ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_INVALID_PARAM;
        return EXIT_FAILURE;
    }

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    geom->tracks = iinfo.tracks;
    geom->sides = iinfo.sides;
    geom->total_tracks = iinfo.tracks * iinfo.sides;

    /* Spočítat celkovou velikost dat a image */
    geom->total_data_bytes = 0;
    geom->image_size = sizeof ( st_DSK_HEADER );

    uint8_t i;
    for ( i = 0; i < geom->total_tracks; i++ ) {
        uint16_t track_size = dsk_decode_track_size ( iinfo.tsize[i] );
        geom->image_size += track_size;
        if ( track_size > sizeof ( st_DSK_TRACK_INFO ) ) {
            geom->total_data_bytes += track_size - sizeof ( st_DSK_TRACK_INFO );
        }
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Vrátí řetězec s verzí knihovny dsk.
 * @return Statický řetězec s verzí (např. "2.0.0").
 */
const char* dsk_version ( void ) {
    return DSK_VERSION;
}
