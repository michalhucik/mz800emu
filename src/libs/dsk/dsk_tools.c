/**
 * @file   dsk_tools.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 3.0.0
 * @brief  Implementace vyšších nástrojů pro Extended CPC DSK obrazy.
 *
 * Vytváření, modifikace, diagnostika, inspekce, editace, validace,
 * identifikace formátu, iterace přes stopy a sektory DSK diskových obrazů.
 *
 * @par Changelog:
 * - 2026-04-15: Diagnostické API (diagnose/repair), inspekce a editace hlaviček.
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
#include <stdarg.h>
#include <stdio.h>

#include "dsk.h"
#include "dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"


/**
 * @brief Zjistí velikost média asociovaného s handlerem.
 *
 * Místní náhrada za generic_driver_get_size(), kterou má generic_driver
 * 2.1.x v projektu mzdisk. Kopie generic_driver v emulátoru (větev 2.0.x,
 * přebíraná z tmz-format) tuto funkci nemá a mimo synchronizaci se nemění.
 * Je to jediný rozdíl tohoto souboru proti dsk_tools 3.4.0 z mzdisk.
 *
 * U souborového handleru zjišťuje velikost přes fseek ( SEEK_END ) a
 * ftell() a původní pozici v souboru obnoví. U paměťového handleru
 * vrací memspec.size.
 *
 * @param h Handler. Musí být otevřený (HANDLER_STATUS_READY).
 * @param size Výstup: velikost média v bajtech. Při chybě se nemění.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE (NULL parametr, handler není
 *         připravený, neznámý typ handleru, chyba fseek/ftell).
 *         Chybové kódy handleru ani driveru nenastavuje.
 */
static int dsk_tools_get_media_size ( st_HANDLER *h, uint32_t *size ) {

    if ( h == NULL || size == NULL ) return EXIT_FAILURE;

    if ( !( h->status & HANDLER_STATUS_READY ) ) return EXIT_FAILURE;

    switch ( h->type ) {

#ifdef GENERIC_DRIVER_FILE
        case HANDLER_TYPE_FILE: {
            st_HANDLER_FILESPC *filespec = &h->spec.filespec;
            if ( filespec->fh == NULL ) return EXIT_FAILURE;

            long current_pos = ftell ( filespec->fh );
            if ( current_pos < 0 ) return EXIT_FAILURE;

            if ( fseek ( filespec->fh, 0, SEEK_END ) != 0 ) return EXIT_FAILURE;

            long end_pos = ftell ( filespec->fh );

            /* Obnovení původní pozice */
            fseek ( filespec->fh, current_pos, SEEK_SET );

            if ( end_pos < 0 ) return EXIT_FAILURE;

            *size = (uint32_t) end_pos;
            return EXIT_SUCCESS;
        }
#endif

#ifdef GENERIC_DRIVER_MEMORY
        case HANDLER_TYPE_MEMORY: {
            st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
            if ( memspec->ptr == NULL ) return EXIT_FAILURE;
            *size = (uint32_t) memspec->size;
            return EXIT_SUCCESS;
        }
#endif

        default:
            break;
    }

    return EXIT_FAILURE;
}


/* Minimální velikost bloku pro I/O operace */
#define DSK_TOOLS_MIN_SECTOR_SIZE   128

/* Maximální délka jedné logovací zprávy */
#define DSK_LOG_BUF_SIZE            256


/* ========================================================================
 * Logování
 * ======================================================================== */

/* Globální logovací callback a user data */
static dsk_tools_log_cb_t s_log_cb = NULL;
static void *s_log_user_data = NULL;


void dsk_tools_set_log_cb ( dsk_tools_log_cb_t cb, void *user_data ) {
    s_log_cb = cb;
    s_log_user_data = user_data;
}


/* Interní pomocná funkce pro logování */
static void dsk_log ( int level, const char *fmt, ... ) {
    if ( s_log_cb == NULL ) return;

    char buf[DSK_LOG_BUF_SIZE];
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( buf, sizeof ( buf ), fmt, ap );
    va_end ( ap );

    s_log_cb ( level, buf, s_log_user_data );
}


/* ========================================================================
 * Vytváření obrazů — popis geometrie
 * ======================================================================== */


/**
 * Přiřazení záznamu v existující struktuře st_DSK_DESCRIPTION.
 *
 * Záznamy musí být uloženy vzestupně podle absolute_track.
 *
 * @param dskdesc Odkaz na existující strukturu
 * @param rule Pořadové číslo záznamu
 * @param abs_track Absolutní stopa od které pravidlo platí
 * @param sectors Počet sektorů
 * @param ssize Kódovaná velikost sektoru
 * @param sector_order Typ řazení sektorů
 * @param sector_map Mapa sektorů (jen pro CUSTOM, jinak NULL)
 * @param default_value Filler byte
 */
void dsk_tools_assign_description ( st_DSK_DESCRIPTION *dskdesc, uint8_t rule, uint8_t abs_track, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, en_DSK_SECTOR_ORDER_TYPE sector_order, uint8_t *sector_map, uint8_t default_value ) {
    if ( dskdesc == NULL || rule >= dskdesc->count_rules ) return;

    dskdesc->rules[rule].absolute_track = abs_track;
    dskdesc->rules[rule].sectors = sectors;
    dskdesc->rules[rule].ssize = ssize;
    dskdesc->rules[rule].sector_order = sector_order;
    dskdesc->rules[rule].sector_map = ( sector_order == DSK_SEC_ORDER_CUSTOM ) ? sector_map : NULL;
    dskdesc->rules[rule].filler = default_value;
}


/* ========================================================================
 * Interní pomocné funkce
 * ======================================================================== */


static int dsk_tools_create_tsizes ( uint8_t *tsize, st_DSK_DESCRIPTION *desc, uint8_t first_abs_track ) {

    if ( first_abs_track < desc->rules[0].absolute_track ) {
        return EXIT_FAILURE;
    }

    uint8_t sectors = desc->rules[0].sectors;
    en_DSK_SECTOR_SIZE ssize = desc->rules[0].ssize;
    uint8_t rule = 0;

    uint8_t abs_track = first_abs_track;
    uint8_t track;
    for ( track = ( first_abs_track / desc->sides ); track < desc->tracks; track++ ) {
        uint8_t side;
        for ( side = 0; side < desc->sides; side++ ) {
            if ( rule < desc->count_rules ) {
                if ( desc->rules[rule].absolute_track == abs_track ) {
                    sectors = desc->rules[rule].sectors;
                    ssize = desc->rules[rule].ssize;
                    rule++;
                }
            }
            tsize[abs_track] = dsk_encode_track_size ( sectors, ssize );
            abs_track++;
        }
    }

    return EXIT_SUCCESS;
}


/* ========================================================================
 * Vytváření obrazů
 * ======================================================================== */


/**
 * Vytvoří DSK header podle description.
 *
 * @param h Handler
 * @param desc Popis geometrie disku
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_create_image_header ( st_HANDLER *h, st_DSK_DESCRIPTION *desc ) {

    if ( h == NULL || desc == NULL ) return EXIT_FAILURE;

    if ( ( !desc->count_rules ) || ( !desc->tracks ) ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_NO_TRACKS;
        return EXIT_FAILURE;
    }

    st_DSK_HEADER dskhdr_buffer;
    st_DSK_HEADER *dskhdr = NULL;

    if ( EXIT_SUCCESS != generic_driver_prepare ( h, 0, (void**) &dskhdr, &dskhdr_buffer, sizeof ( st_DSK_HEADER ) ) ) return EXIT_FAILURE;

    memset ( dskhdr, 0x00, sizeof ( st_DSK_HEADER ) );
    memcpy ( dskhdr->file_info, DSK_DEFAULT_FILEINFO, DSK_FILEINFO_FIELD_LENGTH );
    memcpy ( dskhdr->creator, DSK_DEFAULT_CREATOR, DSK_CREATOR_FIELD_LENGTH );

    dskhdr->tracks = desc->tracks;
    dskhdr->sides = desc->sides;

    if ( EXIT_SUCCESS != dsk_tools_create_tsizes ( dskhdr->tsize, desc, 0 ) ) return EXIT_FAILURE;

    return generic_driver_ppwrite ( h, 0, dskhdr, sizeof ( st_DSK_HEADER ) );
}


/**
 * Vytvoření mapy sektorů podle definice sector order.
 *
 * @param sectors Počet sektorů
 * @param sector_order Typ řazení (CUSTOM se automaticky převede na NORMAL)
 * @param sector_map Výstupní pole o velikosti sectors
 */
void dsk_tools_make_sector_map ( uint8_t sectors, en_DSK_SECTOR_ORDER_TYPE sector_order, uint8_t *sector_map ) {

    if ( sectors == 0 || sector_map == NULL ) return;

    uint8_t sectors_to_order = ( sectors & 1 ) ? ( sectors + 1 ) : sectors;
    uint8_t sector_pos = 0;
    uint8_t i;

    if ( ( sector_order == DSK_SEC_ORDER_CUSTOM ) || ( sector_order > DSK_SEC_ORDER_INTERLACED_LEC_HD ) ) {
        sector_order = DSK_SEC_ORDER_NORMAL;
    }

    for ( i = 0; i < sectors; i += sector_order ) {
        uint8_t j = 0;
        while ( ( j < sector_order ) && ( ( i + j ) < sectors ) ) {
            uint8_t sector_id = 1 + ( i / sector_order ) + ( sectors_to_order / sector_order ) * ( j % sector_order );
            j++;
            sector_map[sector_pos] = sector_id;
            sector_pos++;
        }
    }
}


/**
 * Vytvoření hlavičky pro stopu.
 *
 * Každému sektoru zapíše do Sector Info i skutečnou délku dat
 * (dsk_decode_sector_size ( ssize )), jak vyžaduje Extended CPC DSK.
 *
 * @param h Handler
 * @param dsk_offset Offset v souboru
 * @param track Číslo stopy
 * @param side Strana
 * @param sectors Počet sektorů
 * @param ssize Kódovaná velikost sektoru
 * @param sector_map Seznam ID jednotlivých sektorů
 * @param filler Filler byte zapsaný do hlavičky stopy
 * @param gap Délka GAP#3 zapsaná do hlavičky stopy
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_create_track_header ( st_HANDLER *h, uint32_t dsk_offset, uint8_t track, uint8_t side, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t *sector_map, uint8_t filler, uint8_t gap ) {

    st_DSK_TRACK_INFO trkhdr_buffer;
    st_DSK_TRACK_INFO *trkhdr = NULL;

    if ( EXIT_SUCCESS != generic_driver_prepare ( h, dsk_offset, (void**) &trkhdr, &trkhdr_buffer, sizeof ( st_DSK_TRACK_INFO ) ) ) return EXIT_FAILURE;

    memset ( trkhdr, 0x00, sizeof ( st_DSK_TRACK_INFO ) );
    memcpy ( trkhdr->track_info, DSK_DEFAULT_TRACKINFO, DSK_TRACKINFO_FIELD_LENGTH );
    trkhdr->track = track;
    trkhdr->side = side;
    trkhdr->sectors = sectors;
    trkhdr->ssize = ssize;
    trkhdr->filler = filler;
    trkhdr->gap = gap;

    int i;
    for ( i = 0; i < sectors; i++ ) {
        trkhdr->sinfo[i].track = track;
        trkhdr->sinfo[i].side = side;
        trkhdr->sinfo[i].sector = sector_map[i];
        trkhdr->sinfo[i].ssize = ssize;
        dsk_sector_info_set_data_length ( &trkhdr->sinfo[i], dsk_decode_sector_size ( ssize ) );
    }

    return generic_driver_ppwrite ( h, dsk_offset, trkhdr, sizeof ( st_DSK_TRACK_INFO ) );
}


/**
 * Vyplní všechny sektory na stopě výchozí hodnotou.
 *
 * @param h Handler
 * @param dsk_offset Offset za hlavičkou stopy
 * @param sectors Počet sektorů
 * @param ssize Kódovaná velikost sektoru
 * @param default_value Filler byte
 * @param sectors_total_bytes Výstup: celková velikost zapsaných dat
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_create_track_sectors ( st_HANDLER *h, uint32_t dsk_offset, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t default_value, uint16_t *sectors_total_bytes ) {

    *sectors_total_bytes = 0;

    int my_ssize = dsk_decode_sector_size ( ssize ) / DSK_TOOLS_MIN_SECTOR_SIZE;

    uint8_t i;
    for ( i = 0; i < sectors; i++ ) {
        int j;
        for ( j = 0; j < my_ssize; j++ ) {

            uint8_t data_buffer [ DSK_TOOLS_MIN_SECTOR_SIZE ];
            uint8_t *sector_data = NULL;

            if ( EXIT_SUCCESS != generic_driver_prepare ( h, dsk_offset, (void**) &sector_data, &data_buffer, DSK_TOOLS_MIN_SECTOR_SIZE ) ) return EXIT_FAILURE;

            memset ( sector_data, default_value, DSK_TOOLS_MIN_SECTOR_SIZE );

            generic_driver_ppwrite ( h, dsk_offset, sector_data, DSK_TOOLS_MIN_SECTOR_SIZE );

            *sectors_total_bytes += DSK_TOOLS_MIN_SECTOR_SIZE;
            dsk_offset += DSK_TOOLS_MIN_SECTOR_SIZE;
        }
    }

    return EXIT_SUCCESS;
}


/**
 * Vytvoření jedné DSK stopy.
 *
 * @param h Handler
 * @param dsk_offset Offset v souboru
 * @param track Číslo stopy
 * @param side Strana
 * @param sectors Počet sektorů
 * @param ssize Kódovaná velikost sektoru
 * @param sector_map Seznam ID sektorů
 * @param default_value Filler byte
 * @param track_total_bytes Výstup: celková velikost zapsané stopy
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_create_track ( st_HANDLER *h, uint32_t dsk_offset, uint8_t track, uint8_t side, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t *sector_map, uint8_t default_value, uint32_t *track_total_bytes ) {

    *track_total_bytes = 0;

    if ( sectors != 0 ) {
        if ( EXIT_SUCCESS != dsk_tools_create_track_header ( h, dsk_offset, track, side, sectors, ssize, sector_map, default_value, DSK_DEFAULT_GAP ) ) return EXIT_FAILURE;

        *track_total_bytes += sizeof ( st_DSK_TRACK_INFO );
        dsk_offset += sizeof ( st_DSK_TRACK_INFO );

        uint16_t sectors_total_size = 0;
        if ( EXIT_SUCCESS != dsk_tools_create_track_sectors ( h, dsk_offset, sectors, ssize, default_value, &sectors_total_size ) ) return EXIT_FAILURE;

        /* Padding pro 128B sektory s lichým počtem (zarovnání na 256B) */
        if ( ( ssize == DSK_SECTOR_SIZE_128 ) && ( sectors & 1 ) ) {

            uint8_t data_buffer [ DSK_TOOLS_MIN_SECTOR_SIZE ];
            uint8_t *sector_data = NULL;

            uint32_t zero_filling_offset = dsk_offset + sectors_total_size;

            if ( EXIT_SUCCESS != generic_driver_prepare ( h, zero_filling_offset, (void**) &sector_data, &data_buffer, DSK_TOOLS_MIN_SECTOR_SIZE ) ) return EXIT_FAILURE;

            memset ( sector_data, 0x00, DSK_TOOLS_MIN_SECTOR_SIZE );

            generic_driver_ppwrite ( h, zero_filling_offset, sector_data, DSK_TOOLS_MIN_SECTOR_SIZE );

            sectors_total_size += DSK_TOOLS_MIN_SECTOR_SIZE;
        }

        *track_total_bytes += sectors_total_size;
    }

    return EXIT_SUCCESS;
}


/**
 * Vytvoří postupně všechny stopy podle description.
 *
 * @param h Handler
 * @param desc Popis geometrie
 * @param first_abs_track První absolutní stopa
 * @param dsk_offset Offset v souboru (0 = sizeof(st_DSK_HEADER))
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_create_image_tracks ( st_HANDLER *h, st_DSK_DESCRIPTION *desc, uint8_t first_abs_track, uint32_t dsk_offset ) {

    if ( dsk_offset == 0 ) dsk_offset = sizeof ( st_DSK_HEADER );

    if ( first_abs_track < desc->rules[0].absolute_track ) {
        return EXIT_FAILURE;
    }

    uint8_t sectors = desc->rules[0].sectors;
    en_DSK_SECTOR_SIZE ssize = desc->rules[0].ssize;
    en_DSK_SECTOR_ORDER_TYPE sector_order = desc->rules[0].sector_order;
    uint8_t default_value = desc->rules[0].filler;
    uint8_t rule = 0;

    uint8_t abs_track = first_abs_track;
    uint8_t track;

    en_DSK_SECTOR_ORDER_TYPE last_sector_order = sector_order;
    uint8_t local_sector_map [ DSK_MAX_SECTORS ];
    uint8_t *sector_map;

    if ( ( sector_order == DSK_SEC_ORDER_CUSTOM ) && ( desc->rules[0].sector_map != NULL ) ) {
        sector_map = desc->rules[0].sector_map;
    } else {
        dsk_tools_make_sector_map ( sectors, sector_order, local_sector_map );
        sector_map = local_sector_map;
    }

    for ( track = ( first_abs_track / desc->sides ); track < desc->tracks; track++ ) {
        uint8_t side;
        for ( side = 0; side < desc->sides; side++ ) {
            if ( rule < desc->count_rules ) {
                if ( desc->rules[rule].absolute_track == abs_track ) {
                    sectors = desc->rules[rule].sectors;
                    ssize = desc->rules[rule].ssize;
                    sector_order = desc->rules[rule].sector_order;
                    default_value = desc->rules[rule].filler;

                    if ( ( sector_order == DSK_SEC_ORDER_CUSTOM ) && ( desc->rules[rule].sector_map != NULL ) ) {
                        sector_map = desc->rules[rule].sector_map;
                    } else {
                        if ( sector_order != last_sector_order ) {
                            dsk_tools_make_sector_map ( sectors, sector_order, local_sector_map );
                            sector_map = local_sector_map;
                        }
                    }
                    last_sector_order = sector_order;

                    rule++;
                }
            }

            /* vytvoření stopy */
            uint32_t track_total_bytes = 0;

            if ( EXIT_SUCCESS != dsk_tools_create_track ( h, dsk_offset, track, side, sectors, ssize, sector_map, default_value, &track_total_bytes ) ) return EXIT_FAILURE;
            dsk_offset += track_total_bytes;

            abs_track++;
        }
    }

    return EXIT_SUCCESS;
}


/**
 * Vytvoření DSK podle popisu v desc.
 *
 * @param h Handler
 * @param desc Popis geometrie disku
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_create_image ( st_HANDLER *h, st_DSK_DESCRIPTION *desc ) {
    if ( h == NULL || desc == NULL ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != dsk_tools_create_image_header ( h, desc ) ) return EXIT_FAILURE;
    return dsk_tools_create_image_tracks ( h, desc, 0, 0 );
}


/* ========================================================================
 * Modifikace existujícího obrazu
 * ======================================================================== */


/**
 * Změna parametrů a default obsahu konkrétní absolutní stopy.
 *
 * Pokud se změní velikost stopy, přesune data následujících stop.
 *
 * @param h Handler
 * @param short_image_info Informace o obrazu (NULL = načte se)
 * @param abstrack Absolutní stopa
 * @param sectors Nový počet sektorů
 * @param ssize Nová velikost sektoru
 * @param sector_map Nová mapa sektorů
 * @param default_value Filler byte
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_change_track ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, uint8_t abstrack, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t *sector_map, uint8_t default_value ) {

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

    uint32_t track_offset = dsk_compute_track_offset ( abstrack, iinfo->tsize );
    uint16_t track_size = dsk_decode_track_size ( iinfo->tsize[abstrack] );

    uint16_t new_track_size = dsk_decode_track_size ( dsk_encode_track_size ( sectors, ssize ) );

    uint8_t last_track = ( iinfo->tracks * iinfo->sides ) - 1;
    uint32_t last_track_offset = dsk_compute_track_offset ( last_track, iinfo->tsize );
    uint16_t last_track_size = dsk_decode_track_size ( iinfo->tsize[last_track] );

    uint32_t last_image_byte = last_track_offset + last_track_size;

    if ( track_size != new_track_size ) {

        uint8_t buffer [ DSK_TOOLS_MIN_SECTOR_SIZE ];

        uint32_t next_track_offset = track_offset + track_size;
        uint32_t bytes_to_move = last_image_byte - next_track_offset;
        uint32_t chunks = bytes_to_move / sizeof ( buffer );

        uint32_t src_offset;
        uint32_t dst_offset;
        int32_t step;

        if ( track_size < new_track_size ) {
            /* Stopa se zvětšuje — přesouvat odzadu.
             * Audit M-8: pokud `last_image_byte < sizeof(buffer)`,
             * odčítání by bylo unsigned underflow a `src_offset` by se
             * stal obrovským číslem. V praxi malý DSK s jedinou stopou
             * a zvětšováním. */
            if ( last_image_byte < sizeof ( buffer ) ) {
                return EXIT_FAILURE;
            }
            uint16_t size_difference = new_track_size - track_size;
            src_offset = last_image_byte - sizeof ( buffer );
            dst_offset = src_offset + size_difference;
            step = -(int32_t) sizeof ( buffer );
        } else {
            /* Stopa se zmenšuje — přesouvat odpředu */
            uint16_t size_difference = track_size - new_track_size;
            src_offset = next_track_offset;
            dst_offset = src_offset - size_difference;
            step = (int32_t) sizeof ( buffer );
        }

        uint32_t chunk;
        for ( chunk = 0; chunk < chunks; chunk++ ) {
            if ( EXIT_SUCCESS != dsk_read_on_offset ( h, src_offset, &buffer, sizeof ( buffer ) ) ) return EXIT_FAILURE;
            if ( EXIT_SUCCESS != dsk_write_on_offset ( h, dst_offset, &buffer, sizeof ( buffer ) ) ) return EXIT_FAILURE;
            src_offset += step;
            dst_offset += step;
        }

        if ( track_size > new_track_size ) {
            uint32_t new_last_image_byte = last_image_byte - ( track_size - new_track_size );
            if ( EXIT_SUCCESS != generic_driver_truncate ( h, new_last_image_byte ) ) return EXIT_FAILURE;
        }

        iinfo->tsize[abstrack] = dsk_encode_track_size ( sectors, ssize );
        uint32_t offset = DSK_FILEINFO_FIELD_LENGTH + DSK_CREATOR_FIELD_LENGTH + 4 + abstrack;
        if ( EXIT_SUCCESS != dsk_write_on_offset ( h, offset, &iinfo->tsize[abstrack], 1 ) ) return EXIT_FAILURE;
    }

    uint8_t side = ( iinfo->sides == 1 ) ? 0 : ( abstrack & 1 );
    uint8_t track = abstrack / iinfo->sides;

    if ( sectors != 0 ) {
        if ( EXIT_SUCCESS != dsk_tools_create_track_header ( h, track_offset, track, side, sectors, ssize, sector_map, default_value, DSK_DEFAULT_GAP ) ) return EXIT_FAILURE;

        uint16_t sectors_total_bytes;
        if ( EXIT_SUCCESS != dsk_tools_create_track_sectors ( h, track_offset + sizeof ( st_DSK_TRACK_INFO ), sectors, ssize, default_value, &sectors_total_bytes ) ) return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


int dsk_tools_add_tracks ( st_HANDLER *h, st_DSK_DESCRIPTION *desc ) {

    st_DSK_HEADER dskhdr_buffer;
    st_DSK_HEADER *dskhdr = NULL;

    if ( EXIT_SUCCESS != generic_driver_prepare ( h, 0, (void**) &dskhdr, &dskhdr_buffer, sizeof ( st_DSK_HEADER ) ) ) return EXIT_FAILURE;

    if ( EXIT_SUCCESS != generic_driver_ppread ( h, 0, dskhdr, sizeof ( st_DSK_HEADER ) ) ) return EXIT_FAILURE;

    dskhdr->tracks = desc->tracks;

    uint8_t first_abs_track = desc->rules[0].absolute_track;

    if ( EXIT_SUCCESS != dsk_tools_create_tsizes ( dskhdr->tsize, desc, first_abs_track ) ) return EXIT_FAILURE;

    if ( EXIT_SUCCESS != generic_driver_ppwrite ( h, 0, dskhdr, sizeof ( st_DSK_HEADER ) ) ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO local_short_image_info;
    st_DSK_SHORT_IMAGE_INFO *iinfo;

    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &local_short_image_info ) ) return EXIT_FAILURE;
    iinfo = &local_short_image_info;

    uint32_t track_offset = dsk_compute_track_offset ( first_abs_track, iinfo->tsize );

    return dsk_tools_create_image_tracks ( h, desc, first_abs_track, track_offset );
}


/**
 * Zmenší obraz odstraněním stop od konce.
 *
 * @param h Handler
 * @param short_image_info Informace o obrazu (NULL = načte se)
 * @param total_tracks Nový celkový počet absolutních stop
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_shrink_image ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, uint8_t total_tracks ) {

    st_DSK_SHORT_IMAGE_INFO local_short_image_info;
    st_DSK_SHORT_IMAGE_INFO *iinfo = short_image_info;
    st_DRIVER *d = h->driver;

    if ( iinfo == NULL ) {
        if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &local_short_image_info ) ) return EXIT_FAILURE;
        iinfo = &local_short_image_info;
    }

    if ( total_tracks == 0 ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_NO_TRACKS;
        return EXIT_FAILURE;
    }

    if ( total_tracks >= ( iinfo->tracks * iinfo->sides ) ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_TRACK_NOT_FOUND;
        return EXIT_FAILURE;
    }

    if ( ( iinfo->sides == 2 ) && ( total_tracks & 1 ) ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_DOUBLE_SIDED;
        return EXIT_FAILURE;
    }

    uint32_t track_offset = dsk_compute_track_offset ( total_tracks, iinfo->tsize );

    /* Kontrola existence truncate callbacku PŘED voláním */
    if ( d->truncate_cb == NULL ) {
        d->err = GENERIC_DRIVER_ERROR_CB_NOT_EXIST;
        return EXIT_FAILURE;
    }

    if ( EXIT_SUCCESS != d->truncate_cb ( h, track_offset ) ) {
        return EXIT_FAILURE;
    }

    st_DSK_HEADER dskhdr_buffer;
    st_DSK_HEADER *dskhdr = NULL;

    if ( EXIT_SUCCESS != generic_driver_prepare ( h, 0, (void**) &dskhdr, &dskhdr_buffer, sizeof ( st_DSK_HEADER ) ) ) return EXIT_FAILURE;

    if ( EXIT_SUCCESS != generic_driver_ppread ( h, 0, dskhdr, sizeof ( st_DSK_HEADER ) ) ) return EXIT_FAILURE;

    dskhdr->tracks = total_tracks / dskhdr->sides;

    memset ( &dskhdr->tsize[total_tracks], 0x00, DSK_MAX_TOTAL_TRACKS - total_tracks );

    return generic_driver_ppwrite ( h, 0, dskhdr, sizeof ( st_DSK_HEADER ) );
}


/* ========================================================================
 * Validace a inspekce
 * ======================================================================== */


int dsk_tools_get_dsk_fileinfo ( st_HANDLER *h, uint8_t *dsk_fileinfo_buffer ) {
    uint32_t offset = 0;
    return dsk_read_on_offset ( h, offset, dsk_fileinfo_buffer, DSK_FILEINFO_FIELD_LENGTH );
}


int dsk_tools_check_dsk_fileinfo ( st_HANDLER *h ) {
    uint8_t dsk_fileinfo_buffer[DSK_FILEINFO_FIELD_LENGTH + 1];
    if ( EXIT_FAILURE == dsk_tools_get_dsk_fileinfo ( h, dsk_fileinfo_buffer ) ) return EXIT_FAILURE;
    dsk_fileinfo_buffer[DSK_FILEINFO_FIELD_LENGTH] = 0x00;
    if ( 0 != strcmp ( (char*) dsk_fileinfo_buffer, DSK_DEFAULT_FILEINFO ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


int dsk_tools_get_dsk_creator ( st_HANDLER *h, uint8_t *dsk_creator_buffer ) {
    uint32_t offset = DSK_FILEINFO_FIELD_LENGTH;
    return dsk_read_on_offset ( h, offset, dsk_creator_buffer, DSK_CREATOR_FIELD_LENGTH );
}


en_DSK_TOOLS_CHCKTRKINFO dsk_tools_check_dsk_trackinfo_on_offset ( st_HANDLER *h, uint32_t offset ) {
    uint8_t dsk_trackinfo_buffer[DSK_TRACKINFO_FIELD_LENGTH + 1];
    if ( EXIT_FAILURE == dsk_read_on_offset ( h, offset, dsk_trackinfo_buffer, DSK_TRACKINFO_FIELD_LENGTH ) ) return DSK_TOOLS_CHCKTRKINFO_READ_ERROR;
    dsk_trackinfo_buffer[DSK_TRACKINFO_FIELD_LENGTH] = 0x00;
    if ( 0 != strcmp ( (char*) dsk_trackinfo_buffer, DSK_DEFAULT_TRACKINFO ) ) return DSK_TOOLS_CHCKTRKINFO_FAILURE;
    return DSK_TOOLS_CHCKTRKINFO_SUCCESS;
}


/* ========================================================================
 * Diagnostické API
 * ======================================================================== */


/**
 * Interní: ověření file_info pole v DSK hlavičce.
 *
 * @param h Handler
 * @param result Diagnostický výsledek (nastaví BAD_FILEINFO flag)
 * @return 0 pokud je platný, nenulová pokud ne (fatální chyba)
 */
static int dsk_diag_check_fileinfo ( st_HANDLER *h, st_DSK_DIAG_RESULT *result ) {
    if ( EXIT_FAILURE == dsk_tools_check_dsk_fileinfo ( h ) ) {
        result->image_flags |= DSK_DIAG_IMAGE_BAD_FILEINFO;
        return 1;
    }
    return 0;
}


/**
 * Interní: načtení základních informací z DSK hlavičky.
 *
 * @param h Handler
 * @param result Diagnostický výsledek (naplní creator, header_tracks, sides)
 * @param iinfo Výstup: načtená image info
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int dsk_diag_read_image_info ( st_HANDLER *h, st_DSK_DIAG_RESULT *result, st_DSK_SHORT_IMAGE_INFO *iinfo ) {

    /* Načtení creator stringu */
    uint8_t creator_buf[DSK_CREATOR_FIELD_LENGTH];
    if ( EXIT_FAILURE == dsk_tools_get_dsk_creator ( h, creator_buf ) ) return EXIT_FAILURE;
    memcpy ( result->creator, creator_buf, DSK_CREATOR_FIELD_LENGTH );
    result->creator[DSK_CREATOR_FIELD_LENGTH] = 0x00;

    /* Načtení image info */
    if ( EXIT_FAILURE == dsk_read_short_image_info ( h, iinfo ) ) return EXIT_FAILURE;

    result->header_tracks = iinfo->tracks * iinfo->sides;
    result->raw_header_tracks = iinfo->header_tracks; /* audit H-11 */
    result->sides = iinfo->sides;

    /* Audit H-11: detekce `header_tracks * sides > DSK_MAX_TOTAL_TRACKS`.
     * dsk_read_short_image_info tolerantně oříznul `iinfo->tracks`, ale
     * `iinfo->header_tracks` drží raw hodnotu. Rozdíl signalizuje vadnou
     * hlavičku - přidáme flag pro report a repair. */
    uint16_t raw_total = (uint16_t) iinfo->header_tracks * iinfo->sides;
    if ( raw_total > DSK_MAX_TOTAL_TRACKS ) {
        result->image_flags |= DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED;
    }

    return EXIT_SUCCESS;
}


/**
 * Interní: diagnostika jedné stopy.
 *
 * @param h Handler
 * @param dt Per-track záznam (výstup)
 * @param iinfo Image info
 * @param abs_track Absolutní číslo stopy
 * @param offset Offset stopy v souboru
 * @param expected_track Očekávané číslo stopy
 * @param expected_side Očekávaná strana
 * @param computed_tsize Výstup: spočtená tsize
 * @param track_total_size Výstup: celková velikost stopy v bajtech
 * @return 0 = stopa přečtena (i s chybami), 1 = nelze pokračovat (konec skenování)
 */
static int dsk_diag_check_single_track ( st_HANDLER *h, st_DSK_DIAG_TRACK *dt,
    const st_DSK_SHORT_IMAGE_INFO *iinfo, uint8_t abs_track, uint32_t offset,
    uint8_t expected_track, uint8_t expected_side,
    uint8_t *computed_tsize, uint16_t *track_total_size ) {

    dt->abstrack = abs_track;
    dt->offset = offset;
    dt->flags = DSK_DIAG_TRACK_OK;
    dt->expected_track = expected_track;
    dt->expected_side = expected_side;
    dt->hdr_track = 0;
    dt->hdr_side = 0;
    dt->sectors = 0;
    dt->ssize = 0;
    dt->computed_tsize = 0;
    dt->header_tsize = iinfo->tsize[abs_track];

    *computed_tsize = 0;
    *track_total_size = 0;

    /* Kontrola Track-Info identifikátoru */
    en_DSK_TOOLS_CHCKTRKINFO res = dsk_tools_check_dsk_trackinfo_on_offset ( h, offset );

    if ( res == DSK_TOOLS_CHCKTRKINFO_READ_ERROR ) {
        dt->flags |= DSK_DIAG_TRACK_READ_ERROR;
        return 1; /* konec skenování */
    }

    if ( res == DSK_TOOLS_CHCKTRKINFO_FAILURE ) {
        dt->flags |= DSK_DIAG_TRACK_NO_TRACKINFO;
        return 1; /* konec skenování */
    }

    /* Načtení track headeru */
    st_DSK_SHORT_TRACK_INFO sh_trk_info;
    if ( EXIT_FAILURE == dsk_read_short_track_info_on_offset ( h, offset, &sh_trk_info ) ) {
        dt->flags |= DSK_DIAG_TRACK_READ_ERROR;
        return 1;
    }

    dt->hdr_track = sh_trk_info.track;
    dt->hdr_side = sh_trk_info.side;
    dt->sectors = sh_trk_info.sectors;
    dt->ssize = sh_trk_info.ssize;

    /* Kontrola čísla stopy */
    if ( sh_trk_info.track != expected_track ) {
        dt->flags |= DSK_DIAG_TRACK_BAD_TRACK_NUM;
    }

    /* Kontrola strany */
    if ( sh_trk_info.side != expected_side ) {
        dt->flags |= DSK_DIAG_TRACK_BAD_SIDE_NUM;
    }

    /* Kontrola počtu sektorů. Defense-in-depth: low-level reader
     * `dsk_read_short_track_info_on_offset()` nyní odmítá
     * `sectors > DSK_MAX_SECTORS` ještě před zápisem do sinfo[] (fix
     * C-3), takže sem se hodnota > DSK_MAX_SECTORS nikdy nedostane.
     * Tato kontrola zachycuje jen horní mez jako dodatečnou safety
     * a odhalí `sectors == 0`. Audit M-10. */
    if ( ( sh_trk_info.sectors < 1 ) || ( sh_trk_info.sectors >= DSK_MAX_SECTORS ) ) {
        dt->flags |= DSK_DIAG_TRACK_BAD_SECTORS;
        return 0; /* stopa přečtena, ale nelze spočítat tsize */
    }

    /* Kontrola ssize */
    if ( sh_trk_info.ssize > DSK_SECTOR_SIZE_1024 ) {
        dt->flags |= DSK_DIAG_TRACK_BAD_SSIZE;
        return 0;
    }

    /* Kontrola délky dat v Sector Info. Extended CPC DSK ji vyžaduje,
     * dsk_tools < 3.4.0 ji ale nechávaly nulovou (bug 2026-11-10-003).
     * Hlásí se jen nula - nenulová délka odlišná od velikosti dané N se
     * neposuzuje. Chybu čtení hlavičky zde neřešíme, zachytí ji kontrola
     * čitelnosti dat níže. */
    st_DSK_TRACK_INFO trkhdr;
    if ( EXIT_SUCCESS == dsk_read_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) ) ) {
        uint8_t si;
        for ( si = 0; si < sh_trk_info.sectors; si++ ) {
            if ( dsk_sector_info_get_data_length ( &trkhdr.sinfo[si] ) == 0 ) {
                dt->flags |= DSK_DIAG_TRACK_ZERO_DATALEN;
                break;
            }
        }
    }

    /* Výpočet velikosti sektorových dat a kontrola čitelnosti */
    uint16_t sectors_size = dsk_decode_sector_size ( sh_trk_info.ssize ) * sh_trk_info.sectors;

    if ( ( sh_trk_info.ssize == DSK_SECTOR_SIZE_128 ) && ( sh_trk_info.sectors & 1 ) ) {
        sectors_size += 0x80;
    }

    uint8_t sector_buffer[0x80];
    uint16_t read_offset;
    for ( read_offset = 0; read_offset < sectors_size; read_offset += sizeof ( sector_buffer ) ) {
        if ( EXIT_FAILURE == dsk_read_on_offset ( h, offset + sizeof ( st_DSK_TRACK_INFO ) + read_offset, sector_buffer, sizeof ( sector_buffer ) ) ) {
            dt->flags |= DSK_DIAG_TRACK_DATA_UNREADABLE;
            break;
        }
    }

    /* Spočtení tsize */
    uint16_t track_size = sectors_size + sizeof ( st_DSK_TRACK_INFO );
    *computed_tsize = (uint8_t) ( track_size / 0x0100 );
    dt->computed_tsize = *computed_tsize;
    *track_total_size = track_size;

    /* Porovnání tsize s hlavičkou */
    if ( dt->computed_tsize != dt->header_tsize ) {
        dt->flags |= DSK_DIAG_TRACK_BAD_TSIZE;
    }

    return 0;
}


/**
 * Interní: skenování všech stop a naplnění diagnostických záznamů.
 *
 * @param h Handler
 * @param result Diagnostický výsledek
 * @param iinfo Image info
 * @return EXIT_SUCCESS / EXIT_FAILURE (fatální chyba alokace)
 */
static int dsk_diag_scan_tracks ( st_HANDLER *h, st_DSK_DIAG_RESULT *result, const st_DSK_SHORT_IMAGE_INFO *iinfo ) {

    uint8_t expected_total_tracks = iinfo->tracks * iinfo->sides;

    if ( expected_total_tracks == 0 ) {
        result->actual_tracks = 0;
        result->count_tracks = 0;
        result->tracks = NULL;
        return EXIT_SUCCESS;
    }

    /* Alokace pole per-track záznamů */
    result->tracks = (st_DSK_DIAG_TRACK *) malloc ( expected_total_tracks * sizeof ( st_DSK_DIAG_TRACK ) );
    if ( result->tracks == NULL ) return EXIT_FAILURE;

    uint8_t expected_track = 0;
    uint8_t expected_side = 0;
    uint8_t abs_tracks = 0;
    uint32_t offset = sizeof ( st_DSK_HEADER );

    while ( abs_tracks < expected_total_tracks ) {

        uint8_t computed_tsize = 0;
        uint16_t track_total_size = 0;

        int stop = dsk_diag_check_single_track ( h, &result->tracks[abs_tracks],
            iinfo, abs_tracks, offset, expected_track, expected_side,
            &computed_tsize, &track_total_size );

        /* Aktualizace expected track/side pro další stopu */
        if ( iinfo->sides == 1 ) {
            expected_track++;
        } else {
            expected_side = ( ~expected_side ) & 1;
            if ( expected_side == 0 ) {
                expected_track++;
            }
        }

        abs_tracks++;

        if ( stop ) break;

        /* Pokud nelze spočítat tsize (BAD_SECTORS/BAD_SSIZE), použij tsize z hlavičky */
        if ( track_total_size == 0 ) {
            offset += dsk_decode_track_size ( iinfo->tsize[abs_tracks - 1] );
        } else {
            offset += track_total_size;
        }
    }

    result->actual_tracks = abs_tracks;
    result->count_tracks = abs_tracks;
    result->expected_image_size = offset;

    return EXIT_SUCCESS;
}


/**
 * Interní: vyhodnocení sumarizačních flagů po skenování.
 *
 * @param result Diagnostický výsledek (aktualizuje image_flags)
 * @param iinfo Image info
 */
static void dsk_diag_analyze_summary ( st_DSK_DIAG_RESULT *result, const st_DSK_SHORT_IMAGE_INFO *iinfo ) {

    uint8_t expected_total = iinfo->tracks * iinfo->sides;

    /* Kontrola počtu stop */
    if ( result->actual_tracks != expected_total ) {
        result->image_flags |= DSK_DIAG_IMAGE_BAD_TRACKCOUNT;
    }

    /* Kontrola lichého počtu stop u 2-sided */
    if ( ( iinfo->sides == 2 ) && ( result->actual_tracks & 1 ) ) {
        result->image_flags |= DSK_DIAG_IMAGE_ODD_DOUBLE;
    }

    /* Počítání tsize diferencí, nulových délek dat a per-track chyb */
    uint8_t tsize_diffs = 0;
    uint8_t zero_datalen = 0;
    int has_track_errors = 0;

    uint8_t i;
    for ( i = 0; i < result->count_tracks; i++ ) {
        if ( result->tracks[i].flags & DSK_DIAG_TRACK_BAD_TSIZE ) {
            tsize_diffs++;
        }
        if ( result->tracks[i].flags & DSK_DIAG_TRACK_ZERO_DATALEN ) {
            zero_datalen++;
        }
        if ( result->tracks[i].flags != DSK_DIAG_TRACK_OK ) {
            /* Ignoruj BAD_TSIZE a ZERO_DATALEN - ty mají vlastní
             * image-level flagy a jsou opravitelné */
            uint16_t other_flags = result->tracks[i].flags
                                 & ~( DSK_DIAG_TRACK_BAD_TSIZE | DSK_DIAG_TRACK_ZERO_DATALEN );
            if ( other_flags != 0 ) {
                has_track_errors = 1;
            }
        }
    }

    result->tsize_differences = tsize_diffs;
    result->zero_datalen_tracks = zero_datalen;

    if ( tsize_diffs > 0 ) {
        result->image_flags |= DSK_DIAG_IMAGE_BAD_TSIZE;
    }

    if ( zero_datalen > 0 ) {
        result->image_flags |= DSK_DIAG_IMAGE_ZERO_DATALEN;
    }

    if ( has_track_errors ) {
        result->image_flags |= DSK_DIAG_IMAGE_TRACK_ERRORS;
    }

    /* Trailing data detekce */
    if ( result->actual_file_size > result->expected_image_size ) {
        result->image_flags |= DSK_DIAG_IMAGE_TRAILING_DATA;
    }
}


st_DSK_DIAG_RESULT* dsk_tools_diagnose ( st_HANDLER *h ) {

    if ( h == NULL ) return NULL;

    st_DSK_DIAG_RESULT *result = (st_DSK_DIAG_RESULT *) calloc ( 1, sizeof ( st_DSK_DIAG_RESULT ) );
    if ( result == NULL ) return NULL;

    /* Zjistit skutečnou velikost souboru/paměti */
    uint32_t file_size = 0;
    if ( EXIT_SUCCESS == dsk_tools_get_media_size ( h, &file_size ) ) {
        result->actual_file_size = file_size;
    }

    /* Soubor menší než DSK hlavička (256 B) nemůže být platný obraz */
    if ( file_size < sizeof ( st_DSK_HEADER ) ) {
        result->image_flags |= DSK_DIAG_IMAGE_BAD_FILEINFO;
        return result;
    }

    /* Ověření file_info */
    if ( dsk_diag_check_fileinfo ( h, result ) ) {
        /* Fatální chyba - nelze pokračovat, ale vracíme výsledek */
        return result;
    }

    /* Načtení image info */
    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_FAILURE == dsk_diag_read_image_info ( h, result, &iinfo ) ) {
        /* I/O chyba při čtení hlavičky */
        return result;
    }

    /* Skenování stop */
    if ( EXIT_FAILURE == dsk_diag_scan_tracks ( h, result, &iinfo ) ) {
        /* Fatální chyba alokace */
        free ( result );
        return NULL;
    }

    /* Sumarizace */
    dsk_diag_analyze_summary ( result, &iinfo );

    return result;
}


void dsk_tools_destroy_diag_result ( st_DSK_DIAG_RESULT *result ) {
    if ( result == NULL ) return;
    if ( result->tracks != NULL ) {
        free ( result->tracks );
    }
    free ( result );
}


int dsk_tools_diag_has_repairable_errors ( const st_DSK_DIAG_RESULT *result ) {
    if ( result == NULL ) return 0;

    /* Audit H-11: TRACKCOUNT_EXCEEDED je opravitelná (clamp hlavičky),
     * ale zároveň fatální - blokuje auto-repair při open, vyžaduje
     * explicitní `mzdsk-dsk repair`. */
    uint16_t repairable_mask = DSK_DIAG_IMAGE_BAD_TRACKCOUNT
                             | DSK_DIAG_IMAGE_BAD_TSIZE
                             | DSK_DIAG_IMAGE_TRAILING_DATA
                             | DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED
                             | DSK_DIAG_IMAGE_ZERO_DATALEN;

    return ( result->image_flags & repairable_mask ) ? 1 : 0;
}


int dsk_tools_diag_has_fatal_errors ( const st_DSK_DIAG_RESULT *result ) {
    if ( result == NULL ) return 0;

    /* BAD_FILEINFO je fatální na image úrovni */
    if ( result->image_flags & DSK_DIAG_IMAGE_BAD_FILEINFO ) return 1;

    /* Audit H-11: TRACKCOUNT_EXCEEDED je fatální - blokuje auto-repair
     * při mzdsk_disc_open, vynucuje uživateli spustit explicitní repair.
     * Důvod: auto-clamp hlavičky tiše maže trailing stopy (pokud jsou). */
    if ( result->image_flags & DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED ) return 1;

    /* Per-track fatální chyby */
    uint16_t fatal_track_mask = DSK_DIAG_TRACK_BAD_TRACK_NUM
                              | DSK_DIAG_TRACK_BAD_SIDE_NUM
                              | DSK_DIAG_TRACK_BAD_SECTORS
                              | DSK_DIAG_TRACK_BAD_SSIZE
                              | DSK_DIAG_TRACK_DATA_UNREADABLE;

    uint8_t i;
    for ( i = 0; i < result->count_tracks; i++ ) {
        if ( result->tracks[i].flags & fatal_track_mask ) return 1;
    }

    return 0;
}


/**
 * Opraví opravitelné chyby na základě diagnostiky.
 *
 * Opravuje:
 * - počet stop v hlavičce (BAD_TRACKCOUNT)
 * - tsize pole (BAD_TSIZE)
 * - trailing data za poslední stopou (TRAILING_DATA) - soubor se zkrátí
 *   na `expected_image_size` přes `generic_driver_truncate`
 * - nulové délky dat sektorů v Sector Info (ZERO_DATALEN) - doplní se
 *   velikost, se kterou sektor počítá dsk_compute_sector_offset()
 *   (dekódované ssize sektoru, při nule ssize stopy); nenulové délky,
 *   data sektorů a ostatní pole Sector Info zůstávají beze změny
 *
 * @param h Handler (musí být otevřený pro zápis)
 * @param diag Diagnostický výsledek z dsk_tools_diagnose()
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_repair ( st_HANDLER *h, const st_DSK_DIAG_RESULT *diag ) {

    if ( h == NULL || diag == NULL ) return EXIT_FAILURE;

    /* Není co opravovat */
    if ( !dsk_tools_diag_has_repairable_errors ( diag ) ) return EXIT_SUCCESS;

    /* Načtení DSK hlavičky */
    st_DSK_HEADER dhdr;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, 0, &dhdr, sizeof ( dhdr ) ) ) {
        dsk_log ( DSK_LOG_ERROR, "can't get DSK image info for repair" );
        return EXIT_FAILURE;
    }

    int modified = 0;

    /* Audit H-11: oprava hlavičky při TRACKCOUNT_EXCEEDED (raw tracks*sides
     * > DSK_MAX_TOTAL_TRACKS). Provádí se před BAD_TRACKCOUNT, aby dhdr.tracks
     * nejdřív klesl na hodnotu, kterou driver umí zpracovat; případnou další
     * korekci (actual_tracks) pak vyřeší BAD_TRACKCOUNT větev. */
    if ( diag->image_flags & DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED ) {
        uint8_t max_tracks = (uint8_t) ( DSK_MAX_TOTAL_TRACKS / diag->sides );
        if ( dhdr.tracks > max_tracks ) {
            dsk_log ( DSK_LOG_WARNING,
                      "Clamping DSK header tracks from %d to %d (driver max = %d total per %d side(s)). "
                      "Stopy nad tímto limitem budou při opětovném čtení ignorovány.",
                      dhdr.tracks, max_tracks, DSK_MAX_TOTAL_TRACKS, diag->sides );
            dhdr.tracks = max_tracks;
            modified = 1;
        }
    }

    /* Oprava počtu stop */
    if ( diag->image_flags & DSK_DIAG_IMAGE_BAD_TRACKCOUNT ) {
        uint8_t new_tracks = diag->actual_tracks / diag->sides;
        if ( dhdr.tracks != new_tracks ) {
            dsk_log ( DSK_LOG_INFO, "Repairing track count: %d -> %d", dhdr.tracks, new_tracks );
            dhdr.tracks = new_tracks;
            modified = 1;
        }
    }

    /* Oprava tsize pole */
    if ( diag->image_flags & DSK_DIAG_IMAGE_BAD_TSIZE ) {
        uint8_t i;
        for ( i = 0; i < diag->count_tracks; i++ ) {
            if ( diag->tracks[i].flags & DSK_DIAG_TRACK_BAD_TSIZE ) {
                uint8_t abstrack = diag->tracks[i].abstrack;
                dsk_log ( DSK_LOG_INFO, "Repairing tsize for track %d: 0x%02x -> 0x%02x",
                    abstrack, dhdr.tsize[abstrack], diag->tracks[i].computed_tsize );
                dhdr.tsize[abstrack] = diag->tracks[i].computed_tsize;
                modified = 1;
            }
        }

        /* Vynulovat tsize za posledním platným trackem */
        uint8_t last_valid = diag->actual_tracks;
        if ( last_valid < DSK_MAX_TOTAL_TRACKS ) {
            memset ( &dhdr.tsize[last_valid], 0x00, DSK_MAX_TOTAL_TRACKS - last_valid );
        }
    }

    /* Zápis opraveného headeru */
    if ( modified ) {
        if ( EXIT_SUCCESS != dsk_write_on_offset ( h, 0, &dhdr, sizeof ( dhdr ) ) ) {
            dsk_log ( DSK_LOG_ERROR, "can't write DSK image info for repair" );
            return EXIT_FAILURE;
        }
        dsk_log ( DSK_LOG_INFO, "DSK header repaired successfully." );
    }

    /* Oprava nulových délek dat v Sector Info. Offsety stop bereme
       z diagnostiky (skutečné pozice nalezené skenem), takže oprava
       nezávisí na tom, zda byla tsize v hlavičce správně. */
    if ( diag->image_flags & DSK_DIAG_IMAGE_ZERO_DATALEN ) {
        uint32_t fixed_sectors = 0;
        uint32_t fixed_tracks = 0;
        uint8_t i;
        for ( i = 0; i < diag->count_tracks; i++ ) {
            const st_DSK_DIAG_TRACK *dt = &diag->tracks[i];
            if ( !( dt->flags & DSK_DIAG_TRACK_ZERO_DATALEN ) ) continue;

            st_DSK_TRACK_INFO trkhdr;
            if ( EXIT_SUCCESS != dsk_read_on_offset ( h, dt->offset, &trkhdr, sizeof ( trkhdr ) ) ) {
                dsk_log ( DSK_LOG_ERROR, "can't read track header for repair, abstrack: %d", dt->abstrack );
                return EXIT_FAILURE;
            }

            uint8_t fixed = 0;
            uint8_t si;
            for ( si = 0; si < trkhdr.sectors && si < DSK_MAX_SECTORS; si++ ) {
                st_DSK_SECTOR_INFO *sinfo = &trkhdr.sinfo[si];
                if ( dsk_sector_info_get_data_length ( sinfo ) != 0 ) continue;
                /* Stejné pravidlo jako dsk_compute_sector_offset() */
                uint8_t ssize = ( sinfo->ssize != 0 ) ? sinfo->ssize : trkhdr.ssize;
                dsk_sector_info_set_data_length ( sinfo, dsk_decode_sector_size ( ssize ) );
                fixed++;
            }

            if ( fixed ) {
                if ( EXIT_SUCCESS != dsk_write_on_offset ( h, dt->offset, &trkhdr, sizeof ( trkhdr ) ) ) {
                    dsk_log ( DSK_LOG_ERROR, "can't write track header for repair, abstrack: %d", dt->abstrack );
                    return EXIT_FAILURE;
                }
                fixed_sectors += fixed;
                fixed_tracks++;
            }
        }
        /* Jeden souhrnný řádek - starý obraz má chybu na všech stopách */
        dsk_log ( DSK_LOG_INFO, "Repaired sector data length: %u sector(s) on %u track(s)",
                  (unsigned) fixed_sectors, (unsigned) fixed_tracks );
    }

    /* Oprava trailing dat - soubor zkrátíme na expected_image_size.
       expected_image_size se spočetlo z hlavičky PŘED případnou opravou
       BAD_TSIZE/BAD_TRACKCOUNT, takže je to bezpečná cílová velikost.
       Truncate necháme proběhnout i bez modified=1, protože trailing
       data jsou nezávislá na BAD_TRACKCOUNT/BAD_TSIZE. */
    if ( diag->image_flags & DSK_DIAG_IMAGE_TRAILING_DATA ) {
        if ( diag->expected_image_size > 0
             && diag->actual_file_size > diag->expected_image_size ) {
            dsk_log ( DSK_LOG_INFO, "Truncating trailing data: %u -> %u bytes",
                      diag->actual_file_size, diag->expected_image_size );
            if ( EXIT_SUCCESS != generic_driver_truncate ( h, diag->expected_image_size ) ) {
                dsk_log ( DSK_LOG_ERROR, "can't truncate DSK image to remove trailing data" );
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}


/**
 * Zpětně kompatibilní wrapper nad novým diagnostickým API.
 *
 * @param h Handler
 * @param print_info Nenulová = logovat detaily (přes logovací callback)
 * @param dsk_autofix Nenulová = opravit nalezené chyby
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_check_dsk ( st_HANDLER *h, int print_info, int dsk_autofix ) {

    if ( dsk_autofix != 0 ) {
        dsk_log ( DSK_LOG_INFO, "Checking DSK format (in autofix mode) ... " );
    } else {
        dsk_log ( DSK_LOG_INFO, "Checking DSK format ... " );
    }

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    if ( diag == NULL ) {
        dsk_log ( DSK_LOG_ERROR, "diagnostics failed (allocation error)" );
        return EXIT_FAILURE;
    }

    /* Fatální chyba file_info */
    if ( diag->image_flags & DSK_DIAG_IMAGE_BAD_FILEINFO ) {
        dsk_log ( DSK_LOG_ERROR, "DSK file info check failed" );
        dsk_tools_destroy_diag_result ( diag );
        return EXIT_FAILURE;
    }

    if ( print_info ) {
        dsk_log ( DSK_LOG_INFO, "DSK fileinfo: OK" );

        /* Tisknutelná část creator stringu */
        char creator_str[DSK_CREATOR_FIELD_LENGTH + 1];
        int ci = 0;
        while ( ci < DSK_CREATOR_FIELD_LENGTH && diag->creator[ci] >= 0x20 ) {
            creator_str[ci] = diag->creator[ci];
            ci++;
        }
        creator_str[ci] = '\0';
        dsk_log ( DSK_LOG_INFO, "DSK creator: %s", creator_str );
        dsk_log ( DSK_LOG_INFO, "DSK header sides: %d", diag->sides );
        dsk_log ( DSK_LOG_INFO, "DSK header tracks: %d", diag->header_tracks / diag->sides );
        dsk_log ( DSK_LOG_INFO, "Analyzing tracks ... " );
        dsk_log ( DSK_LOG_INFO, "total tracks: %d", diag->actual_tracks );
    }

    /* Logování per-track chyb */
    uint8_t i;
    for ( i = 0; i < diag->count_tracks; i++ ) {
        st_DSK_DIAG_TRACK *dt = &diag->tracks[i];
        if ( dt->flags & DSK_DIAG_TRACK_NO_TRACKINFO ) {
            if ( dsk_autofix ) {
                dsk_log ( DSK_LOG_WARNING, "invalid track info on 0x%08x, abstrack: %d - treating as end of valid tracks", dt->offset, dt->abstrack );
            } else {
                dsk_log ( DSK_LOG_ERROR, "expected track info on 0x%08x, abstrack: %d", dt->offset, dt->abstrack );
            }
        }
        if ( dt->flags & DSK_DIAG_TRACK_BAD_TRACK_NUM ) {
            dsk_log ( DSK_LOG_ERROR, "bad track '%d' (expected %d) on 0x%08x, abstrack: %d",
                dt->hdr_track, dt->expected_track, dt->offset, dt->abstrack );
        }
        if ( dt->flags & DSK_DIAG_TRACK_BAD_SIDE_NUM ) {
            dsk_log ( DSK_LOG_ERROR, "bad side '%d' (expected %d) on 0x%08x, abstrack: %d",
                dt->hdr_side, dt->expected_side, dt->offset, dt->abstrack );
        }
        if ( dt->flags & DSK_DIAG_TRACK_BAD_SECTORS ) {
            dsk_log ( DSK_LOG_ERROR, "bad sectors count '%d' on 0x%08x, abstrack: %d",
                dt->sectors, dt->offset, dt->abstrack );
        }
        if ( dt->flags & DSK_DIAG_TRACK_BAD_SSIZE ) {
            dsk_log ( DSK_LOG_ERROR, "bad ssize '0x%02x' on 0x%08x, abstrack: %d",
                dt->ssize, dt->offset, dt->abstrack );
        }
        if ( dt->flags & DSK_DIAG_TRACK_DATA_UNREADABLE ) {
            dsk_log ( DSK_LOG_ERROR, "error when reading track data on 0x%08x, abstrack: %d",
                dt->offset, dt->abstrack );
        }
    }

    int errors = 0;

    if ( diag->image_flags & DSK_DIAG_IMAGE_BAD_TRACKCOUNT ) {
        dsk_log ( DSK_LOG_WARNING, "DSK BUG: Bad tracks info in DSK header." );
        errors++;
    }

    if ( diag->image_flags & DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED ) {
        /* Audit H-11: hlavička deklaruje víc stop než driver umí zpracovat.
         * Uživatel musí spustit `mzdsk-dsk repair` - auto-repair při open
         * je zakázán (viz has_fatal_errors). */
        dsk_log ( DSK_LOG_WARNING,
                  "DSK BUG: Header declares %d tracks * %d sides = %d total, but driver max is %d. Run 'mzdsk-dsk repair' to clamp the header.",
                  diag->raw_header_tracks, diag->sides,
                  (int) diag->raw_header_tracks * diag->sides,
                  DSK_MAX_TOTAL_TRACKS );
        errors++;
    }

    if ( diag->image_flags & DSK_DIAG_IMAGE_ODD_DOUBLE ) {
        dsk_log ( DSK_LOG_WARNING, "DSK BUG: The disc is double-sided, but the count of tracks is odd." );
        errors++;
    }

    if ( diag->tsize_differences != 0 ) {
        dsk_log ( DSK_LOG_WARNING, "DSK BUG: Some tracks have bad size info in DSK header." );
        errors++;
    }

    if ( diag->image_flags & DSK_DIAG_IMAGE_ZERO_DATALEN ) {
        dsk_log ( DSK_LOG_WARNING, "DSK BUG: %d track(s) have sectors with zero data length in sector info.",
                  diag->zero_datalen_tracks );
        errors++;
    }

    int result;

    if ( dsk_tools_diag_has_fatal_errors ( diag ) && !dsk_autofix ) {
        dsk_tools_destroy_diag_result ( diag );
        return EXIT_FAILURE;
    }

    if ( errors ) {
        if ( dsk_autofix ) {
            result = dsk_tools_repair ( h, diag );
            if ( result == EXIT_SUCCESS ) {
                dsk_log ( DSK_LOG_INFO, "Result: %d error(s) repaired. DSK is OK!", errors );
            }
        } else {
            dsk_log ( DSK_LOG_WARNING, "Result: this DSK has %d repairable error(s).", errors );
            result = EXIT_FAILURE;
        }
    } else {
        dsk_log ( DSK_LOG_INFO, "Result: DSK is OK!" );
        result = EXIT_SUCCESS;
    }

    dsk_tools_destroy_diag_result ( diag );
    return result;
}


/* ========================================================================
 * Inspekce hlaviček
 * ======================================================================== */


/**
 * Přečte raw informace z DSK hlavičky.
 *
 * @param h Handler
 * @param info Výstupní struktura
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_read_header_info ( st_HANDLER *h, st_DSK_HEADER_INFO *info ) {

    if ( h == NULL || info == NULL ) return EXIT_FAILURE;

    st_DSK_HEADER hdr;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, 0, &hdr, sizeof ( hdr ) ) ) return EXIT_FAILURE;

    memcpy ( info->file_info, hdr.file_info, DSK_FILEINFO_FIELD_LENGTH );
    info->file_info[DSK_FILEINFO_FIELD_LENGTH] = 0x00;

    memcpy ( info->creator, hdr.creator, DSK_CREATOR_FIELD_LENGTH );
    info->creator[DSK_CREATOR_FIELD_LENGTH] = 0x00;

    info->tracks = hdr.tracks;
    info->sides = hdr.sides;
    memcpy ( info->tsize, hdr.tsize, DSK_MAX_TOTAL_TRACKS );

    return EXIT_SUCCESS;
}


/**
 * Přečte raw informace z hlavičky jedné stopy.
 *
 * @param h Handler
 * @param abstrack Absolutní číslo stopy
 * @param info Výstupní struktura
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_read_track_header_info ( st_HANDLER *h, uint8_t abstrack, st_DSK_TRACK_HEADER_INFO *info ) {

    if ( h == NULL || info == NULL ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    if ( abstrack >= ( iinfo.tracks * iinfo.sides ) ) return EXIT_FAILURE;

    uint32_t offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    /* Načtení celého track headeru */
    st_DSK_TRACK_INFO trkhdr;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) ) ) return EXIT_FAILURE;

    info->track = trkhdr.track;
    info->side = trkhdr.side;
    info->ssize = trkhdr.ssize;
    info->sectors = trkhdr.sectors;
    info->gap = trkhdr.gap;
    info->filler = trkhdr.filler;
    memcpy ( info->sinfo, trkhdr.sinfo, sizeof ( info->sinfo ) );

    return EXIT_SUCCESS;
}


/**
 * Detekuje trailing data za poslední stopou.
 *
 * @param h Handler
 * @param trailing_offset Výstup: offset kde trailing data začínají
 * @param trailing_size Výstup: velikost trailing dat
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_detect_trailing_data ( st_HANDLER *h, uint32_t *trailing_offset, uint32_t *trailing_size ) {

    if ( h == NULL || trailing_offset == NULL || trailing_size == NULL ) return EXIT_FAILURE;

    *trailing_offset = 0;
    *trailing_size = 0;

    /* Zjistit skutečnou velikost souboru/paměti */
    uint32_t file_size = 0;
    if ( EXIT_SUCCESS != dsk_tools_get_media_size ( h, &file_size ) ) return EXIT_FAILURE;

    /* Zjistit očekávanou velikost z hlavičky */
    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    uint8_t total_tracks = iinfo.tracks * iinfo.sides;
    uint32_t expected_size = sizeof ( st_DSK_HEADER );

    uint8_t i;
    for ( i = 0; i < total_tracks; i++ ) {
        expected_size += dsk_decode_track_size ( iinfo.tsize[i] );
    }

    if ( file_size > expected_size ) {
        *trailing_offset = expected_size;
        *trailing_size = file_size - expected_size;
    }

    return EXIT_SUCCESS;
}


/* ========================================================================
 * Editace hlaviček
 * ======================================================================== */


/**
 * Nastaví pole creator v DSK hlavičce.
 *
 * @param h Handler
 * @param creator Nový creator string
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_set_creator ( st_HANDLER *h, const char *creator ) {

    if ( h == NULL || creator == NULL ) return EXIT_FAILURE;

    uint8_t buf[DSK_CREATOR_FIELD_LENGTH];
    memset ( buf, 0x00, sizeof ( buf ) );

    size_t len = strlen ( creator );
    if ( len > DSK_CREATOR_FIELD_LENGTH ) len = DSK_CREATOR_FIELD_LENGTH;
    memcpy ( buf, creator, len );

    uint32_t offset = DSK_FILEINFO_FIELD_LENGTH;
    return dsk_write_on_offset ( h, offset, buf, DSK_CREATOR_FIELD_LENGTH );
}


/**
 * Upraví vybraná pole v hlavičce stopy.
 *
 * Hodnota -1 znamená "neměnit", 0-255 je nová hodnota daného pole.
 * Díky int16_t rozsahu lze bezpečně nastavit i hodnotu 0xFF (častý
 * filler u 5.25" disket).
 *
 * @param h Handler
 * @param abstrack Absolutní číslo stopy
 * @param track_num Nové číslo stopy (-1 = neměnit)
 * @param side Nová strana (-1 = neměnit)
 * @param gap Nová hodnota GAP#3 (-1 = neměnit)
 * @param filler Nový filler byte (-1 = neměnit)
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_set_track_header ( st_HANDLER *h, uint8_t abstrack,
    int16_t track_num, int16_t side, int16_t gap, int16_t filler ) {

    if ( h == NULL ) return EXIT_FAILURE;

    /* Validace rozsahu: -1 = neměnit, 0-255 = hodnota bajtu */
    if ( track_num < -1 || track_num > 0xFF ) return EXIT_FAILURE;
    if ( side      < -1 || side      > 0xFF ) return EXIT_FAILURE;
    if ( gap       < -1 || gap       > 0xFF ) return EXIT_FAILURE;
    if ( filler    < -1 || filler    > 0xFF ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    if ( abstrack >= ( iinfo.tracks * iinfo.sides ) ) return EXIT_FAILURE;

    uint32_t offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    /* Načtení celého track headeru */
    st_DSK_TRACK_INFO trkhdr;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) ) ) return EXIT_FAILURE;

    /* Modifikace požadovaných polí */
    if ( track_num >= 0 ) trkhdr.track  = (uint8_t) track_num;
    if ( side      >= 0 ) trkhdr.side   = (uint8_t) side;
    if ( gap       >= 0 ) trkhdr.gap    = (uint8_t) gap;
    if ( filler    >= 0 ) trkhdr.filler = (uint8_t) filler;

    /* Zápis zpět */
    return dsk_write_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) );
}


/**
 * Nastaví FDC stavové registry pro konkrétní sektor.
 *
 * @param h Handler
 * @param abstrack Absolutní číslo stopy
 * @param sector_idx Index sektoru (0-based)
 * @param fdc_sts1 FDC stavový registr 1
 * @param fdc_sts2 FDC stavový registr 2
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_set_sector_fdc_status ( st_HANDLER *h, uint8_t abstrack,
    uint8_t sector_idx, uint8_t fdc_sts1, uint8_t fdc_sts2 ) {

    if ( h == NULL ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    if ( abstrack >= ( iinfo.tracks * iinfo.sides ) ) return EXIT_FAILURE;

    uint32_t offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    /* Načtení track headeru pro zjištění počtu sektorů */
    st_DSK_TRACK_INFO trkhdr;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) ) ) return EXIT_FAILURE;

    if ( sector_idx >= trkhdr.sectors ) return EXIT_FAILURE;

    /* Modifikace FDC statusu */
    trkhdr.sinfo[sector_idx].fdc_sts1 = fdc_sts1;
    trkhdr.sinfo[sector_idx].fdc_sts2 = fdc_sts2;

    /* Zápis celého headeru zpět */
    return dsk_write_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) );
}


/**
 * @brief Nastaví ID jednoho sektoru v hlavičce stopy.
 *
 * Načte track header, změní sinfo[sector_idx].sector a zapíše zpět.
 * Data sektoru a ostatní pole sinfo zůstávají beze změny.
 *
 * @param h Handler
 * @param abstrack Absolutní číslo stopy
 * @param sector_idx Index sektoru (0-based)
 * @param new_sector_id Nová hodnota ID sektoru
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_set_sector_id ( st_HANDLER *h, uint8_t abstrack,
    uint8_t sector_idx, uint8_t new_sector_id ) {

    if ( h == NULL ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    if ( abstrack >= ( iinfo.tracks * iinfo.sides ) ) return EXIT_FAILURE;

    uint32_t offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    /* Načtení track headeru pro zjištění počtu sektorů */
    st_DSK_TRACK_INFO trkhdr;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) ) ) return EXIT_FAILURE;

    if ( sector_idx >= trkhdr.sectors ) return EXIT_FAILURE;

    /* Modifikace sector ID */
    trkhdr.sinfo[sector_idx].sector = new_sector_id;

    /* Zápis celého headeru zpět */
    return dsk_write_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) );
}


/**
 * @brief Nastaví ID všech sektorů na stopě najednou.
 *
 * Načte track header, přepíše sinfo[0..count-1].sector a zapíše zpět.
 * Ostatní pole sinfo a data sektorů zůstávají beze změny.
 *
 * @param h Handler
 * @param abstrack Absolutní číslo stopy
 * @param sector_ids Pole nových ID sektorů
 * @param count Počet prvků (musí odpovídat počtu sektorů na stopě)
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int dsk_tools_set_sector_ids ( st_HANDLER *h, uint8_t abstrack,
    const uint8_t *sector_ids, uint8_t count ) {

    if ( h == NULL || sector_ids == NULL ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    if ( abstrack >= ( iinfo.tracks * iinfo.sides ) ) return EXIT_FAILURE;

    uint32_t offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    /* Načtení track headeru pro zjištění počtu sektorů */
    st_DSK_TRACK_INFO trkhdr;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) ) ) return EXIT_FAILURE;

    if ( count != trkhdr.sectors ) return EXIT_FAILURE;

    /* Přepsání sector ID pro všechny sektory */
    for ( uint8_t i = 0; i < count; i++ ) {
        trkhdr.sinfo[i].sector = sector_ids[i];
    }

    /* Zápis celého headeru zpět */
    return dsk_write_on_offset ( h, offset, &trkhdr, sizeof ( trkhdr ) );
}


/* ========================================================================
 * Verze knihovny
 * ======================================================================== */


const char* dsk_tools_version ( void ) {
    return DSK_TOOLS_VERSION;
}


/* ========================================================================
 * Analýza geometrie (pravidla stop)
 * ======================================================================== */


void dsk_tools_destroy_track_rules ( st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules ) {
    if ( tracks_rules == NULL ) return;
    if ( ( tracks_rules->count_rules != 0 ) && ( tracks_rules->rule != NULL ) ) {
        free ( tracks_rules->rule );
    }
    free ( tracks_rules );
}


/**
 * Analyzuje geometrii DSK obrazu a extrahuje pravidla stop.
 *
 * @param h Handler
 * @return Alokovaná struktura (uvolnit přes dsk_tools_destroy_track_rules),
 *         nebo NULL při chybě
 */
st_DSK_TOOLS_TRACKS_RULES_INFO * dsk_tools_get_tracks_rules ( st_HANDLER * h ) {

    st_DSK_SHORT_IMAGE_INFO sh_img_info;
    if ( EXIT_FAILURE == dsk_read_short_image_info ( h, &sh_img_info ) ) {
        dsk_log ( DSK_LOG_ERROR, "can't get DSK image info" );
        return NULL;
    }

    st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules = malloc ( sizeof ( st_DSK_TOOLS_TRACKS_RULES_INFO ) );
    if ( tracks_rules == NULL ) return NULL;

    tracks_rules->total_tracks = sh_img_info.tracks * sh_img_info.sides;
    tracks_rules->sides = sh_img_info.sides;
    tracks_rules->count_rules = 0;
    tracks_rules->rule = NULL;
    tracks_rules->mzboot_track = 0;

    int last_rule = -1;

    int track;
    for ( track = 0; track < tracks_rules->total_tracks; track++ ) {

        st_DSK_SHORT_TRACK_INFO sh_trk_info;

        if ( EXIT_FAILURE == dsk_read_short_track_info ( h, &sh_img_info, track, &sh_trk_info ) ) {
            dsk_log ( DSK_LOG_ERROR, "can't get DSK track info for track %d", track );
            dsk_tools_destroy_track_rules ( tracks_rules );
            return NULL;
        }

        if ( ( tracks_rules->count_rules == 0 ) || ( sh_trk_info.sectors != tracks_rules->rule[last_rule].sectors ) || ( sh_trk_info.ssize != tracks_rules->rule[last_rule].ssize ) ) {
            st_DSK_TOOLS_TRACK_RULE_INFO *new_rule = realloc ( tracks_rules->rule, ( tracks_rules->count_rules + 1 ) * sizeof ( st_DSK_TOOLS_TRACK_RULE_INFO ) );
            if ( new_rule == NULL ) {
                dsk_tools_destroy_track_rules ( tracks_rules );
                return NULL;
            }
            tracks_rules->rule = new_rule;
            tracks_rules->rule[tracks_rules->count_rules].from_track = track;
            tracks_rules->rule[tracks_rules->count_rules].count_tracks = 1;
            tracks_rules->rule[tracks_rules->count_rules].sectors = sh_trk_info.sectors;
            tracks_rules->rule[tracks_rules->count_rules].ssize = sh_trk_info.ssize;
            tracks_rules->count_rules++;
            last_rule++;
        } else {
            tracks_rules->rule[last_rule].count_tracks++;
        }

        if ( ( track == 1 ) && ( sh_trk_info.sectors == 16 ) && ( sh_trk_info.ssize == DSK_SECTOR_SIZE_256 ) ) {
            tracks_rules->mzboot_track = 1;
        }
    }

    return tracks_rules;
}


/* ========================================================================
 * Identifikace formátu
 * ======================================================================== */


en_DSK_TOOLS_IDENTFORMAT dsk_tools_identformat_from_tracks_rules ( const st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules ) {

    if ( ( tracks_rules == NULL ) || ( tracks_rules->mzboot_track != 1 ) ) return DSK_TOOLS_IDENTFORMAT_UNKNOWN;

    if ( ( tracks_rules->count_rules == 1 ) && ( tracks_rules->rule[0].sectors == 16 ) && ( tracks_rules->rule[0].ssize == DSK_SECTOR_SIZE_256 ) ) {
        return DSK_TOOLS_IDENTFORMAT_MZBASIC;
    } else if ( ( tracks_rules->count_rules == 3 ) && ( tracks_rules->rule[0].sectors == tracks_rules->rule[2].sectors ) && ( tracks_rules->rule[0].ssize == tracks_rules->rule[2].ssize ) ) {
        if ( ( tracks_rules->rule[0].sectors == 9 ) && ( tracks_rules->rule[0].ssize == DSK_SECTOR_SIZE_512 ) ) {
            return DSK_TOOLS_IDENTFORMAT_MZCPM;
        } else if ( ( tracks_rules->rule[0].sectors == 18 ) && ( tracks_rules->rule[0].ssize == DSK_SECTOR_SIZE_512 ) ) {
            return DSK_TOOLS_IDENTFORMAT_MZCPMHD;
        }
    }

    return DSK_TOOLS_IDENTFORMAT_MZBOOT;
}


/**
 * Identifikuje formát DSK obrazu.
 *
 * @param h Handler
 * @param result Výstup: identifikovaný formát
 * @return EXIT_FAILURE | EXIT_SUCCESS
 */
int dsk_tools_identformat ( st_HANDLER *h, en_DSK_TOOLS_IDENTFORMAT *result ) {

    if ( result == NULL ) return EXIT_FAILURE;

    st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules = dsk_tools_get_tracks_rules ( h );
    if ( tracks_rules == NULL ) {
        *result = DSK_TOOLS_IDENTFORMAT_UNKNOWN;
        return EXIT_FAILURE;
    }

    *result = dsk_tools_identformat_from_tracks_rules ( tracks_rules );
    dsk_tools_destroy_track_rules ( tracks_rules );
    return EXIT_SUCCESS;
}


st_DSK_TOOLS_TRACK_RULE_INFO* dsk_tools_get_rule_for_track ( const st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules, uint8_t track ) {
    if ( tracks_rules == NULL || tracks_rules->count_rules == 0 ) return NULL;

    int i = tracks_rules->count_rules - 1;
    st_DSK_TOOLS_TRACK_RULE_INFO *rule = NULL;
    while ( i >= 0 ) {
        rule = &tracks_rules->rule[i--];
        if ( track >= rule->from_track ) break;
    }
    return rule;
}


/* ========================================================================
 * Iterace přes stopy a sektory
 * ======================================================================== */


/**
 * Iteruje přes všechny stopy v DSK obrazu a volá callback pro každou.
 *
 * @param h Handler
 * @param cb Callback volaný pro každou stopu
 * @param user_data Uživatelská data předávaná callbacku
 * @return EXIT_SUCCESS, EXIT_FAILURE (chyba I/O), nebo nenulová návratová
 *         hodnota z callbacku (předčasné ukončení)
 */
int dsk_for_each_track ( st_HANDLER *h, dsk_track_callback_t cb, void *user_data ) {

    if ( h == NULL || cb == NULL ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    uint8_t total_tracks = iinfo.tracks * iinfo.sides;
    uint8_t abstrack;

    for ( abstrack = 0; abstrack < total_tracks; abstrack++ ) {
        st_DSK_SHORT_TRACK_INFO tinfo;
        if ( EXIT_SUCCESS != dsk_read_short_track_info ( h, &iinfo, abstrack, &tinfo ) ) return EXIT_FAILURE;

        int ret = cb ( h, abstrack, &tinfo, user_data );
        if ( ret != 0 ) return ret;
    }

    return EXIT_SUCCESS;
}


/**
 * Iteruje přes všechny sektory na zadané stopě a volá callback pro každý.
 *
 * @param h Handler
 * @param abstrack Absolutní číslo stopy
 * @param cb Callback volaný pro každý sektor
 * @param user_data Uživatelská data předávaná callbacku
 * @return EXIT_SUCCESS, EXIT_FAILURE, nebo nenulová návratová hodnota z cb
 */
int dsk_for_each_sector ( st_HANDLER *h, uint8_t abstrack, dsk_sector_callback_t cb, void *user_data ) {

    if ( h == NULL || cb == NULL ) return EXIT_FAILURE;

    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;

    if ( abstrack >= ( iinfo.tracks * iinfo.sides ) ) {
        h->err = (en_HANDLER_ERROR) DSK_ERROR_TRACK_NOT_FOUND;
        return EXIT_FAILURE;
    }

    uint32_t track_offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    st_DSK_SHORT_TRACK_INFO tinfo;
    if ( EXIT_SUCCESS != dsk_read_short_track_info_on_offset ( h, track_offset, &tinfo ) ) return EXIT_FAILURE;

    uint32_t sector_data_offset = track_offset + sizeof ( st_DSK_TRACK_INFO );
    uint16_t default_ssize = dsk_decode_sector_size ( tinfo.ssize );

    uint8_t i;
    for ( i = 0; i < tinfo.sectors; i++ ) {
        uint16_t this_sector_size = ( tinfo.sector_sizes[i] != 0 )
            ? dsk_decode_sector_size ( tinfo.sector_sizes[i] )
            : default_ssize;

        int ret = cb ( h, abstrack, i, tinfo.sinfo[i], sector_data_offset, this_sector_size, user_data );
        if ( ret != 0 ) return ret;

        sector_data_offset += this_sector_size;
    }

    return EXIT_SUCCESS;
}
