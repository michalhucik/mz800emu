/**
 * @file   cmt_tmz_meta.c
 * @brief  Nástroje nad vloženou páskou: informace o pásce, kontrola pásky, převod profilu.
 *
 * Modul čte pásku jen přes cmttmz_get_file() a mění ji jen přes úpravy pásky
 * (cmttmz_insert_blocks(), cmttmz_remove_blocks(), cmttmz_replace_block()).
 * Vlastní stav má jen tabulku profilů ze zkušební stavby (jednou, g_once).
 * Knihovna TapeMZ parser bloku 0x32 ani kontrolu struktury pásky nenabízí
 * (nástroje tmzinfo a tmzedit je mají jako statický kód), proto jsou tu malé
 * vlastní verze (kandidáti na pozdější přesun do knihovny).
 *
 * Formát bloku 0x32 (specifikace TZX v1.20, data bloku v st_TZX_BLOCK):
 * [WORD délka za WORD][BYTE počet N][N x (BYTE ID textu, BYTE délka L,
 * L bajtů textu ISO 8859-1, zalomení řádku 0Dh)]. Formát bloku 0x43 (TMZ):
 * [DWORD délka 6][BYTE stroj][DWORD takt CPU LE][BYTE verze ROM].
 *
 * V běžném provozu modul nic nevypisuje; knihovna vypíše na stderr jen
 * u poškozeného bloku 0x40 / 0x41 při kontrole (tmz_block_to_mzf()).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
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

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_mzfile.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/tmz/tmz_tlv.h"
#include "libs/mzf/mzf.h"
#include "libs/mztape/mztape.h"
#include "libs/mzcmt_fm/mzcmt_fm.h"
#include "libs/endianity/endianity.h"

#include "cmt.h"
#include "cmtext.h"
#include "cmtext_container.h"
#include "cmt_tmz.h"
#include "cmt_tmz_import.h"
#include "cmt_tmz_meta.h"


/* ------------------------------------------------------------------------- */
/*  Společné                                                                 */
/* ------------------------------------------------------------------------- */


/**
 * @brief Nastaví text chyby úpravy pásky podle formátu (cmttmz_set_edit_error()).
 * @param fmt Formát printf (anglicky).
 */
static void meta_error ( const char *fmt, ... ) G_GNUC_PRINTF ( 1, 2 );
static void meta_error ( const char *fmt, ... ) {
    char msg[CMTTMZ_EDIT_ERROR_SIZE];
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( msg, sizeof ( msg ), fmt, ap );
    va_end ( ap );
    cmttmz_set_edit_error ( msg );
}


/**
 * @brief Přidá větu k varování nástrojů (cmttmz_get_edit_warning(); "; " mezi větami, delší se ořízne).
 * @param text Věta anglicky.
 */
static void meta_warn ( const char *text ) {
    char buf[CMTTMZ_WARNING_SIZE];
    g_strlcpy ( buf, cmttmz_get_edit_warning ( ), sizeof ( buf ) );
    if ( buf[0] ) g_strlcat ( buf, "; ", sizeof ( buf ) );
    g_strlcat ( buf, text, sizeof ( buf ) );
    cmttmz_set_edit_warning ( buf );
}


/**
 * @brief Vložená páska, nebo NULL (bez pásky nebo bez kontejneru rozšíření).
 * @return Soubor pásky (vlastní rozšíření).
 */
static const st_TZX_FILE* meta_file ( void ) {
    if ( !g_cmt_tmz_extension->container ) return NULL;
    return cmttmz_get_file ( );
}


/**
 * @brief Přečte 16bitové číslo LE.
 * @param p Ukazatel na 2 bajty. @return Hodnota.
 */
static uint16_t meta_rd16 ( const uint8_t *p ) {
    return ( uint16_t ) ( p[0] | ( p[1] << 8 ) );
}


/* ------------------------------------------------------------------------- */
/*  Informace o pásce                                                        */
/* ------------------------------------------------------------------------- */


/** @brief Nejvíc položek bloku 0x32 (počet je v jednom bajtu). */
#define META_MAX_ENTRIES 255

/** @brief Délka dat bloku 0x43 se strukturou st_TMZ_MZ_MACHINE_INFO (DWORD délky + 6 B). */
#define META_MACHINE_INFO_LENGTH ( 4u + ( uint32_t ) sizeof ( st_TMZ_MZ_MACHINE_INFO ) )

/** @brief Bajt zalomení řádku v textu TZX (specifikace TZX v1.20, obecná pravidla). */
#define META_TZX_NEWLINE 0x0D


/**
 * @brief Text formuláře: klíč, ID položky TZX a místo ve struktuře.
 */
typedef struct st_META_FIELD {
    const char *key;   /**< klíč (MCP, varování) */
    uint8_t type_id;   /**< ID textu bloku 0x32 */
    size_t offset;     /**< offsetof() bufferu v st_CMTTMZ_TAPE_INFO */
} st_META_FIELD;

/** @brief Texty formuláře v pořadí položek st_CMTTMZ_TAPE_INFO (ID podle specifikace TZX). */
static const st_META_FIELD g_meta_fields[CMTTMZ_TAPE_INFO_FIELDS] = {
    { "title", 0x00, offsetof ( st_CMTTMZ_TAPE_INFO, title ) },
    { "publisher", 0x01, offsetof ( st_CMTTMZ_TAPE_INFO, publisher ) },
    { "author", 0x02, offsetof ( st_CMTTMZ_TAPE_INFO, author ) },
    { "year", 0x03, offsetof ( st_CMTTMZ_TAPE_INFO, year ) },
    { "language", 0x04, offsetof ( st_CMTTMZ_TAPE_INFO, language ) },
    { "type", 0x05, offsetof ( st_CMTTMZ_TAPE_INFO, type ) },
    { "price", 0x06, offsetof ( st_CMTTMZ_TAPE_INFO, price ) },
    { "protection", 0x07, offsetof ( st_CMTTMZ_TAPE_INFO, protection ) },
    { "origin", 0x08, offsetof ( st_CMTTMZ_TAPE_INFO, origin ) },
    { "comment", 0xFF, offsetof ( st_CMTTMZ_TAPE_INFO, comment ) },
};


/**
 * @brief Jedna položka bloku 0x32.
 *
 * @invariant text ukazuje na len bajtů (do dat bloku pásky, nebo do bufferu
 *            převedeného textu formuláře); platí jen po dobu volání.
 */
typedef struct st_META_ENTRY {
    uint8_t type;          /**< ID textu */
    uint8_t len;           /**< délka textu v bajtech */
    const uint8_t *text;   /**< bajty textu (nevlastní) */
    int field;             /**< index textu formuláře, ke kterému položka patří; -1 = cizí položka */
} st_META_ENTRY;


/**
 * @brief Rozebraný blok 0x32.
 * @invariant count <= META_MAX_ENTRIES; položky leží celé uvnitř bloku.
 */
typedef struct st_META_ARCHIVE {
    uint32_t count;                         /**< počet položek */
    st_META_ENTRY e[META_MAX_ENTRIES];      /**< položky v pořadí bloku */
} st_META_ARCHIVE;


const char* cmttmz_tape_info_key ( uint32_t field ) {
    return ( field < CMTTMZ_TAPE_INFO_FIELDS ) ? g_meta_fields[field].key : NULL;
}


char* cmttmz_tape_info_text ( st_CMTTMZ_TAPE_INFO *info, uint32_t field ) {
    if ( !info || field >= CMTTMZ_TAPE_INFO_FIELDS ) return NULL;
    return ( char* ) info + g_meta_fields[field].offset;
}


const char* cmttmz_tape_info_text_const ( const st_CMTTMZ_TAPE_INFO *info, uint32_t field ) {
    if ( !info || field >= CMTTMZ_TAPE_INFO_FIELDS ) return NULL;
    return ( const char* ) info + g_meta_fields[field].offset;
}


/**
 * @brief Text formuláře podle indexu (pro čtení).
 * @param info Informace. @param field Index (v rozsahu). @return Buffer textu.
 */
static const char* meta_text ( const st_CMTTMZ_TAPE_INFO *info, uint32_t field ) {
    return ( const char* ) info + g_meta_fields[field].offset;
}


/**
 * @brief Index textu formuláře k ID položky bloku 0x32.
 * @param type ID textu. @return Index 0 .. CMTTMZ_TAPE_INFO_FIELDS - 1, nebo -1 pro ID, které formulář nezná.
 */
static int meta_field_of_type ( uint8_t type ) {
    for ( int k = 0; k < CMTTMZ_TAPE_INFO_FIELDS; k++ ) {
        if ( g_meta_fields[k].type_id == type ) return k;
    }
    return -1;
}


/**
 * @brief Je blok informační (bez signálu a bez řízení průchodu páskou)?
 *
 * Takové bloky smí stát před bloky 0x32 a 0x43 "na začátku pásky":
 * 0x30 Text Description, 0x31 Message, 0x32 Archive Info, 0x33 Hardware
 * Type, 0x35 Custom Info, 0x43 MZ Machine Info, 0x5A Glue.
 *
 * @param id ID bloku. @return 1 = informační blok.
 */
static int meta_is_info_block ( uint8_t id ) {
    switch ( id ) {
        case TZX_BLOCK_ID_TEXT_DESCRIPTION:
        case TZX_BLOCK_ID_MESSAGE:
        case TZX_BLOCK_ID_ARCHIVE_INFO:
        case TZX_BLOCK_ID_HARDWARE_TYPE:
        case TZX_BLOCK_ID_CUSTOM_INFO:
        case TMZ_BLOCK_ID_MZ_MACHINE_INFO:
        case TZX_BLOCK_ID_GLUE:
            return 1;
        default:
            return 0;
    }
}


/**
 * @brief Konec "začátku pásky": počet bloků od 0, které jsou všechny informační.
 * @param f Páska. @return Index prvního neinformačního bloku (nebo počet bloků).
 */
static uint32_t meta_head_end ( const st_TZX_FILE *f ) {
    uint32_t i = 0;
    while ( i < f->block_count && meta_is_info_block ( f->blocks[i].id ) ) i++;
    return i;
}


/**
 * @brief Rozebere blok 0x32 (bez kopie; položky ukazují do dat bloku).
 *
 * Konec bloku dává WORD délky (nejvýš délka dat bloku); bajty za poslední
 * položkou se ignorují. Položka přes konec bloku nebo počet položek nad
 * data = blok nečitelný.
 *
 * @param b Blok. @param[out] a Položky (field = -1).
 * @return 1 = čitelný blok 0x32, 0 = ne.
 */
static int meta_archive_parse ( const st_TZX_BLOCK *b, st_META_ARCHIVE *a ) {
    a->count = 0;
    if ( b->id != TZX_BLOCK_ID_ARCHIVE_INFO || !b->data || b->length < 3 ) return 0;
    uint32_t end = 2u + meta_rd16 ( b->data );
    if ( end > b->length || end < 3 ) return 0;
    uint32_t n = b->data[2];
    uint32_t pos = 3;
    for ( uint32_t i = 0; i < n; i++ ) {
        if ( pos + 2 > end ) return 0;
        uint8_t len = b->data[pos + 1];
        if ( pos + 2 + len > end ) return 0;
        a->e[i].type = b->data[pos];
        a->e[i].len = len;
        a->e[i].text = b->data + pos + 2;
        a->e[i].field = -1;
        pos += 2u + len;
    }
    a->count = n;
    return 1;
}


/**
 * @brief Najde blok 0x32 informací o pásce: první čitelný blok 0x32 na začátku pásky.
 * @param f Páska. @param head Konec začátku pásky (meta_head_end()). @param[out] a Položky.
 * @return Číslo bloku, nebo -1.
 */
static int meta_find_archive ( const st_TZX_FILE *f, uint32_t head, st_META_ARCHIVE *a ) {
    for ( uint32_t i = 0; i < head; i++ ) {
        if ( meta_archive_parse ( &f->blocks[i], a ) ) return ( int ) i;
    }
    a->count = 0;
    return -1;
}


/**
 * @brief Najde blok 0x43 informací o pásce: první blok 0x43 aspoň META_MACHINE_INFO_LENGTH B na začátku pásky.
 * @param f Páska. @param head Konec začátku pásky. @return Číslo bloku, nebo -1.
 */
static int meta_find_machine ( const st_TZX_FILE *f, uint32_t head ) {
    for ( uint32_t i = 0; i < head; i++ ) {
        const st_TZX_BLOCK *b = &f->blocks[i];
        if ( b->id == TMZ_BLOCK_ID_MZ_MACHINE_INFO && b->data && b->length >= META_MACHINE_INFO_LENGTH ) return ( int ) i;
    }
    return -1;
}


/**
 * @brief Převede bajty položky (ISO 8859-1) na text formuláře v UTF-8.
 *
 * 0Dh = '\n'; cizí zalomení (samotné 0Ah, dvojice 0Dh 0Ah) také jeden
 * '\n' (zápis upraveného pole dá 0Dh podle specifikace
 * TZX); 20h až 7Eh beze změny, A0h až FFh = U+00A0 až U+00FF (2 B UTF-8),
 * ostatní bajty (řídicí znaky, 7Fh až 9Fh) '?' (meta_bytes_lossy()).
 *
 * @param s Bajty. @param len Délka (<= CMTTMZ_TAPE_INFO_MAX_CHARS).
 * @param[out] out Buffer CMTTMZ_TAPE_INFO_TEXT_SIZE B (stačí 2 * 255 + 1).
 */
static void meta_decode ( const uint8_t *s, uint8_t len, char *out ) {
    size_t o = 0;
    for ( uint32_t i = 0; i < len; i++ ) {
        uint8_t c = s[i];
        if ( c == META_TZX_NEWLINE ) {
            out[o++] = '\n';
            if ( i + 1 < len && s[i + 1] == 0x0A ) i++;
        } else if ( c == 0x0A ) {
            out[o++] = '\n';
        } else if ( c >= 0x20 && c <= 0x7E ) {
            out[o++] = ( char ) c;
        } else if ( c >= 0xA0 ) {
            out[o++] = ( char ) ( 0xC0 | ( c >> 6 ) );
            out[o++] = ( char ) ( 0x80 | ( c & 0x3F ) );
        } else {
            out[o++] = '?';
        }
    }
    out[o] = '\0';
}


/**
 * @brief Nahradí meta_decode() některý bajt položky znakem '?' (text formuláře by bajt ztratil)?
 *
 * Řídicí znaky kromě 0Dh a 0Ah a bajty 7Fh až 9Fh (C1, např. znaky CP1252).
 * Nezměněná položka jde zpět původními bajty; upravená by je zapsala jako
 * 3Fh - zápis to ohlásí varováním.
 *
 * @param s Bajty. @param len Délka. @return 1 = ano, 0 = ne.
 */
static int meta_bytes_lossy ( const uint8_t *s, uint8_t len ) {
    for ( uint32_t i = 0; i < len; i++ ) {
        uint8_t c = s[i];
        if ( c == META_TZX_NEWLINE || c == 0x0A ) continue;
        if ( c < 0x20 || ( c >= 0x7F && c <= 0x9F ) ) return 1;
    }
    return 0;
}


/**
 * @brief Převede text formuláře (UTF-8) na bajty položky ISO 8859-1.
 *
 * '\n', "\r\n" i samotné '\r' = 0Dh; U+0020 až U+007E a U+00A0 až U+00FF
 * jedním bajtem; jiný řídicí znak, U+007F až U+009F, znak nad U+00FF a každý
 * bajt neplatné sekvence UTF-8 = '?'.
 *
 * @param s Text ukončený nulou.
 * @param[out] out Buffer CMTTMZ_TAPE_INFO_MAX_CHARS B. @param[out] len Délka výsledku.
 * @param[out] replaced 1 = něco se nahradilo '?' (jinak se nemění).
 * @return EXIT_SUCCESS; EXIT_FAILURE, když výsledek přesáhne CMTTMZ_TAPE_INFO_MAX_CHARS bajtů.
 */
static int meta_encode ( const char *s, uint8_t *out, uint8_t *len, int *replaced ) {
    const uint8_t *p = ( const uint8_t* ) s;
    size_t n = strlen ( s );
    size_t i = 0;
    uint32_t o = 0;
    while ( i < n ) {
        uint8_t c = p[i];
        int byte = -1;   /* výsledný bajt, -1 = '?' */
        size_t seq = 1;
        if ( c == '\r' ) {
            byte = META_TZX_NEWLINE;
            if ( i + 1 < n && p[i + 1] == '\n' ) seq = 2;
        } else if ( c == '\n' ) {
            byte = META_TZX_NEWLINE;
        } else if ( c >= 0x20 && c <= 0x7E ) {
            byte = c;
        } else if ( c >= 0x80 ) {
            /* délka sekvence UTF-8 podle vedoucího bajtu (RFC 3629) */
            if ( c >= 0xC2 && c <= 0xDF ) seq = 2;
            else if ( c >= 0xE0 && c <= 0xEF ) seq = 3;
            else if ( c >= 0xF0 && c <= 0xF4 ) seq = 4;
            for ( size_t k = 1; k < seq; k++ ) {
                if ( i + k >= n || ( p[i + k] & 0xC0 ) != 0x80 ) {
                    seq = 1;
                    break;
                }
            }
            if ( seq == 2 ) {
                uint32_t cp = ( ( uint32_t ) ( c & 0x1F ) << 6 ) | ( p[i + 1] & 0x3F );
                if ( cp >= 0xA0 && cp <= 0xFF ) byte = ( int ) cp;
            }
        }
        if ( o >= CMTTMZ_TAPE_INFO_MAX_CHARS ) return EXIT_FAILURE;
        if ( byte < 0 ) {
            out[o++] = '?';
            *replaced = 1;
        } else {
            out[o++] = ( uint8_t ) byte;
        }
        i += seq;
    }
    *len = ( uint8_t ) o;
    return EXIT_SUCCESS;
}


/**
 * @brief Přečte informace o pásce a k nim rozebraný blok 0x32 (společné jádro get a set).
 * @param f Páska.
 * @param[out] out Informace (bez storable a tape_serial). @param[out] a Položky bloku 0x32 s přiřazením k textům.
 * @param[out] archive_id Číslo bloku 0x32 (-1). @param[out] machine_id Číslo bloku 0x43 (-1).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace kopie bloku 0x43.
 */
static int meta_read ( const st_TZX_FILE *f, st_CMTTMZ_TAPE_INFO *out, st_META_ARCHIVE *a, int *archive_id, int *machine_id ) {
    memset ( out, 0, sizeof ( *out ) );
    out->machine = -1;
    out->other_archive_block = -1;
    uint32_t head = meta_head_end ( f );
    *archive_id = meta_find_archive ( f, head, a );
    int seen[CMTTMZ_TAPE_INFO_FIELDS] = { 0 };
    for ( uint32_t i = 0; i < a->count; i++ ) {
        int k = meta_field_of_type ( a->e[i].type );
        if ( k >= 0 && !seen[k] ) {
            seen[k] = 1;
            a->e[i].field = k;
            meta_decode ( a->e[i].text, a->e[i].len, cmttmz_tape_info_text ( out, ( uint32_t ) k ) );
        } else {
            a->e[i].field = -1;
            out->other_entries++;
        }
    }
    for ( uint32_t i = 0; i < f->block_count; i++ ) {
        if ( f->blocks[i].id != TZX_BLOCK_ID_ARCHIVE_INFO || ( int ) i == *archive_id ) continue;
        if ( !out->other_archive_blocks ) out->other_archive_block = ( int ) i;
        out->other_archive_blocks++;
    }
    *machine_id = meta_find_machine ( f, head );
    if ( *machine_id >= 0 ) {
        /* tmz_block_parse_mz_machine_info() mění data bloku (endianita): jen nad kopií */
        const st_TZX_BLOCK *b = &f->blocks[*machine_id];
        st_TZX_BLOCK copy = *b;
        copy.data = ( uint8_t* ) malloc ( b->length );
        if ( !copy.data ) return EXIT_FAILURE;
        memcpy ( copy.data, b->data, b->length );
        en_TZX_ERROR err = TZX_OK;
        st_TMZ_MZ_MACHINE_INFO *mi = tmz_block_parse_mz_machine_info ( &copy, &err );
        if ( mi ) out->machine = mi->machine;
        free ( copy.data );
        if ( !mi ) *machine_id = -1;
    }
    return EXIT_SUCCESS;
}


int cmttmz_tape_info_get ( st_CMTTMZ_TAPE_INFO *out ) {
    if ( !out ) return EXIT_FAILURE;
    memset ( out, 0, sizeof ( *out ) );
    out->machine = -1;
    out->other_archive_block = -1;
    const st_TZX_FILE *f = meta_file ( );
    if ( !f ) return EXIT_FAILURE;
    st_META_ARCHIVE *a = g_new ( st_META_ARCHIVE, 1 );
    int ai, mi;
    int rc = meta_read ( f, out, a, &ai, &mi );
    g_free ( a );
    if ( rc != EXIT_SUCCESS ) {
        memset ( out, 0, sizeof ( *out ) );
        out->machine = -1;
        out->other_archive_block = -1;
        return EXIT_FAILURE;
    }
    const char *path = cmtext_container_get_filepath ( g_cmt_tmz_extension->container );
    if ( !path || !path[0] ) {
        out->storable = 1; /* páska bez souboru: uložení nabídne formát */
    } else {
        const char *ext = cmtext_get_filename_extension ( path );
        out->storable = ( ext && ( !g_ascii_strcasecmp ( ext, "tmz" ) || !g_ascii_strcasecmp ( ext, "tzx" ) ) ) ? 1 : 0;
    }
    out->tape_serial = cmttmz_get_tape_serial ( );
    return EXIT_SUCCESS;
}


/**
 * @brief Takt CPU platný pro každý blok kromě bloků 0x32 a 0x43, v pořadí pásky.
 *
 * Takt z cmttmz_clock_contexts() (pravidlo playeru, 0 = výchozí takt);
 * bloky 0x32 a 0x43 se vynechají, aby šly posloupnosti před úpravou
 * informací a po ní porovnat.
 *
 * @param f Páska. @param[out] count Počet položek.
 * @return Pole (g_free), NULL při 0 blocích.
 */
static uint32_t* meta_clocks ( const st_TZX_FILE *f, uint32_t *count ) {
    *count = 0;
    if ( !f->block_count ) return NULL;
    uint32_t *all = g_new ( uint32_t, f->block_count );
    cmttmz_clock_contexts ( f, all );
    uint32_t n = 0;
    for ( uint32_t i = 0; i < f->block_count; i++ ) {
        uint8_t id = f->blocks[i].id;
        if ( id == TMZ_BLOCK_ID_MZ_MACHINE_INFO || id == TZX_BLOCK_ID_ARCHIVE_INFO ) continue;
        all[n++] = all[i];
    }
    *count = n;
    return all;
}


/**
 * @brief Sestaví data bloku 0x32 z položek (bajty položek beze změny).
 *
 * Knihovní tzx_block_create_archive_info() bere texty jako řetězce
 * ukončené nulou, cizí položka s bajtem 00h by se jí zkrátila; proto
 * vlastní zápis téhož formátu.
 *
 * @param e Položky. @param n Počet (1 .. META_MAX_ENTRIES).
 * @param[out] out Blok (data malloc, výchozí alokátor knihovny tzx; vlastník volající).
 * @return EXIT_SUCCESS; EXIT_FAILURE při těle nad 65535 B nebo selhání alokace.
 */
static int meta_archive_build ( const st_META_ENTRY *e, uint32_t n, st_TZX_BLOCK *out ) {
    uint32_t body = 1;
    for ( uint32_t i = 0; i < n; i++ ) body += 2u + e[i].len;
    if ( body > 0xFFFFu || n > META_MAX_ENTRIES ) return EXIT_FAILURE;
    uint8_t *d = ( uint8_t* ) malloc ( 2u + body );
    if ( !d ) return EXIT_FAILURE;
    d[0] = ( uint8_t ) ( body & 0xFF );
    d[1] = ( uint8_t ) ( body >> 8 );
    d[2] = ( uint8_t ) n;
    uint32_t pos = 3;
    for ( uint32_t i = 0; i < n; i++ ) {
        d[pos++] = e[i].type;
        d[pos++] = e[i].len;
        if ( e[i].len ) memcpy ( d + pos, e[i].text, e[i].len );
        pos += e[i].len;
    }
    out->id = TZX_BLOCK_ID_ARCHIVE_INFO;
    out->length = 2u + body;
    out->data = d;
    return EXIT_SUCCESS;
}


/** @brief Co se stane s blokem 0x32 nebo 0x43 při zápisu informací. */
typedef enum en_META_OP {
    META_OP_NONE = 0,  /**< beze změny */
    META_OP_INSERT,    /**< vložit nový blok */
    META_OP_REPLACE,   /**< nahradit dosavadní blok */
    META_OP_REMOVE,    /**< odstranit dosavadní blok */
} en_META_OP;


/**
 * @brief Pracovní data zápisu informací (velká pole mimo zásobník).
 */
typedef struct st_META_WORK {
    st_META_ARCHIVE cur;                                         /**< dosavadní blok 0x32 */
    st_META_ENTRY next[META_MAX_ENTRIES + CMTTMZ_TAPE_INFO_FIELDS]; /**< položky nového bloku 0x32 */
    uint8_t enc[CMTTMZ_TAPE_INFO_FIELDS][CMTTMZ_TAPE_INFO_MAX_CHARS]; /**< texty formuláře převedené na ISO 8859-1 */
    uint8_t enc_len[CMTTMZ_TAPE_INFO_FIELDS];                    /**< délky převedených textů */
    int touched[CMTTMZ_TAPE_INFO_FIELDS];                        /**< 1 = text se proti pásce změnil */
    st_CMTTMZ_TAPE_INFO now;                                     /**< dosavadní informace (meta_read()) */
} st_META_WORK;


/**
 * @brief Převede změněné texty formuláře na ISO 8859-1 (w->touched, w->enc).
 * @param w Pracovní data (now vyplněné). @param in Formulář.
 * @param[out] replaced Bitová maska změněných textů s náhradou '?': nový text se znakem mimo
 *             ISO 8859-1, nebo dosavadní položka s bajty, které formulář ukázal jako '?'
 *             (meta_bytes_lossy()).
 * @return EXIT_SUCCESS; EXIT_FAILURE, když text po převodu přesáhne
 *         CMTTMZ_TAPE_INFO_MAX_CHARS (chyba v cmttmz_get_edit_error()).
 */
static int meta_encode_form ( st_META_WORK *w, const st_CMTTMZ_TAPE_INFO *in, unsigned *replaced ) {
    *replaced = 0;
    for ( uint32_t k = 0; k < CMTTMZ_TAPE_INFO_FIELDS; k++ ) {
        w->touched[k] = strcmp ( meta_text ( in, k ), meta_text ( &w->now, k ) ) != 0;
        w->enc_len[k] = 0;
        if ( !w->touched[k] || !meta_text ( in, k )[0] ) continue;
        int rep = 0;
        if ( EXIT_SUCCESS != meta_encode ( meta_text ( in, k ), w->enc[k], &w->enc_len[k], &rep ) ) {
            meta_error ( "The %s text is longer than %d characters", g_meta_fields[k].key, CMTTMZ_TAPE_INFO_MAX_CHARS );
            return EXIT_FAILURE;
        }
        /* dosavadní položka měla bajty, které formulář ukázal jako '?': upravený text je zapíše jako 3Fh */
        for ( uint32_t i = 0; i < w->cur.count && !rep; i++ ) {
            if ( w->cur.e[i].field == ( int ) k && meta_bytes_lossy ( w->cur.e[i].text, w->cur.e[i].len ) ) rep = 1;
        }
        if ( rep ) *replaced |= 1u << k;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Sestaví seznam položek nového bloku 0x32 podle formuláře.
 *
 * Cizí položky a nezměněné texty jdou původními bajty, změněný text převedený
 * na ISO 8859-1 na místo dosavadní položky, vymazaný text odstraní všechny
 * položky svého ID (i cizí duplicity), nový text se vloží před
 * první položku s vyšším ID.
 *
 * @param w Pracovní data (cur, now, touched, enc vyplněné). @param in Formulář.
 * @param[out] n Počet položek ve w->next.
 */
static void meta_archive_plan ( st_META_WORK *w, const st_CMTTMZ_TAPE_INFO *in, uint32_t *n ) {
    int used[CMTTMZ_TAPE_INFO_FIELDS] = { 0 };
    uint32_t cnt = 0;
    for ( uint32_t i = 0; i < w->cur.count; i++ ) {
        const st_META_ENTRY *e = &w->cur.e[i];
        int kt = meta_field_of_type ( e->type );
        /* vymazaný text: pryč jsou všechny položky jeho ID */
        if ( kt >= 0 && w->touched[kt] && !meta_text ( in, ( uint32_t ) kt )[0] ) continue;
        if ( e->field < 0 ) {
            w->next[cnt++] = *e;
            continue;
        }
        int k = e->field;
        used[k] = 1;
        if ( !w->touched[k] ) {
            w->next[cnt++] = *e;
        } else {
            st_META_ENTRY ne = { e->type, w->enc_len[k], w->enc[k], k };
            w->next[cnt++] = ne;
        }
    }
    for ( uint32_t k = 0; k < CMTTMZ_TAPE_INFO_FIELDS; k++ ) {
        if ( used[k] || !meta_text ( in, k )[0] ) continue;
        st_META_ENTRY ne = { g_meta_fields[k].type_id, w->enc_len[k], w->enc[k], ( int ) k };
        uint32_t pos = 0;
        while ( pos < cnt && w->next[pos].type <= ne.type ) pos++;
        memmove ( &w->next[pos + 1], &w->next[pos], ( cnt - pos ) * sizeof ( w->next[0] ) );
        w->next[pos] = ne;
        cnt++;
    }
    *n = cnt;
}


/**
 * @brief Liší se nový seznam položek od dosavadního bloku 0x32 (ID, délky, bajty, pořadí)?
 * @param w Pracovní data. @param n Počet nových položek. @return 1 = liší se.
 */
static int meta_archive_differs ( const st_META_WORK *w, uint32_t n ) {
    if ( n != w->cur.count ) return 1;
    for ( uint32_t i = 0; i < n; i++ ) {
        const st_META_ENTRY *a = &w->cur.e[i];
        const st_META_ENTRY *b = &w->next[i];
        if ( a->type != b->type || a->len != b->len ) return 1;
        if ( a->len && memcmp ( a->text, b->text, a->len ) != 0 ) return 1;
    }
    return 0;
}


/** @brief Prázdný záznam rychlosti (ORIGIN_NONE) pro vkládané informační bloky. */
static const st_CMTTMZ_BLOCK_SPEED g_meta_no_speed[2];


int cmttmz_tape_info_set ( const st_CMTTMZ_TAPE_INFO *in, int *changed ) {
    if ( changed ) *changed = 0;
    cmttmz_set_edit_warning ( "" );
    const st_TZX_FILE *f = meta_file ( );
    if ( !f ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    if ( !in ) {
        cmttmz_set_edit_error ( "No tape information" );
        return EXIT_FAILURE;
    }
    if ( in->tape_serial != cmttmz_get_tape_serial ( ) ) {
        cmttmz_set_edit_error ( "The tape changed since the tape information was read; read it again" );
        return EXIT_FAILURE;
    }
    for ( uint32_t k = 0; k < CMTTMZ_TAPE_INFO_FIELDS; k++ ) {
        if ( !memchr ( meta_text ( in, k ), '\0', CMTTMZ_TAPE_INFO_TEXT_SIZE ) ) {
            meta_error ( "The %s text is longer than %d characters", g_meta_fields[k].key, CMTTMZ_TAPE_INFO_MAX_CHARS );
            return EXIT_FAILURE;
        }
    }

    st_META_WORK *w = g_new ( st_META_WORK, 1 );
    int ai, mi;
    if ( EXIT_SUCCESS != meta_read ( f, &w->now, &w->cur, &ai, &mi ) ) {
        g_free ( w );
        cmttmz_set_edit_error ( "Not enough memory" );
        return EXIT_FAILURE;
    }
    if ( in->machine != w->now.machine && ( in->machine < -1 || in->machine >= TMZ_MACHINE_COUNT ) ) {
        meta_error ( "Invalid machine %d", in->machine );
        g_free ( w );
        return EXIT_FAILURE;
    }
    unsigned replaced = 0;
    if ( EXIT_SUCCESS != meta_encode_form ( w, in, &replaced ) ) {
        g_free ( w );
        return EXIT_FAILURE;
    }

    /* Archive Info */
    uint32_t n = 0;
    meta_archive_plan ( w, in, &n );
    en_META_OP aop = META_OP_NONE;
    if ( ai < 0 ) {
        if ( n ) aop = META_OP_INSERT;
    } else if ( !n ) {
        aop = META_OP_REMOVE;
    } else if ( meta_archive_differs ( w, n ) ) {
        aop = META_OP_REPLACE;
    }

    /* Machine Info */
    en_META_OP mop = META_OP_NONE;
    if ( in->machine != w->now.machine ) {
        if ( in->machine < 0 ) mop = META_OP_REMOVE;
        else mop = ( mi >= 0 ) ? META_OP_REPLACE : META_OP_INSERT;
    }

    if ( aop == META_OP_NONE && mop == META_OP_NONE ) {
        /* beze změny: páska se nedotkne */
        g_free ( w );
        cmttmz_set_edit_error ( "" );
        return EXIT_SUCCESS;
    }

    /* všechny nové bloky předem */
    st_TZX_BLOCK ab = { 0, 0, NULL };
    st_TZX_BLOCK mb = { 0, 0, NULL };
    int ok = 1;
    if ( aop == META_OP_INSERT || aop == META_OP_REPLACE ) {
        if ( n > META_MAX_ENTRIES || EXIT_SUCCESS != meta_archive_build ( w->next, n, &ab ) ) {
            cmttmz_set_edit_error ( ( n > META_MAX_ENTRIES ) ? "Archive info has too many entries" : "Archive info is too large" );
            ok = 0;
        }
    }
    if ( ok && mop == META_OP_REPLACE ) {
        const st_TZX_BLOCK *ob = &f->blocks[mi];
        mb.id = ob->id;
        mb.length = ob->length;
        mb.data = ( uint8_t* ) malloc ( ob->length );
        if ( !mb.data ) {
            cmttmz_set_edit_error ( "Not enough memory" );
            ok = 0;
        } else {
            /* jen bajt stroje; takt, verze ROM a další bajty bloku zůstanou */
            memcpy ( mb.data, ob->data, ob->length );
            mb.data[4] = ( uint8_t ) in->machine;
        }
    } else if ( ok && mop == META_OP_INSERT ) {
        st_TZX_BLOCK *nb = tmz_block_create_mz_machine_info ( ( en_TMZ_MACHINE ) in->machine, MZCMT_FM_DEFAULT_TIMEBASE, 0 );
        if ( !nb ) {
            cmttmz_set_edit_error ( "Not enough memory" );
            ok = 0;
        } else {
            mb = *nb;
            nb->data = NULL; /* data převzal mb */
            tmz_block_free ( nb );
        }
    }
    g_free ( w );
    if ( !ok ) {
        free ( ab.data );
        free ( mb.data );
        return EXIT_FAILURE;
    }

    uint32_t clocks_n0 = 0;
    uint32_t *clocks0 = meta_clocks ( f, &clocks_n0 );

    /*
     * Nejvýš dvě úpravy pásky. Nový blok 0x43 patří hned za blok 0x32, proto se
     * s ním vkládá nebo nahrazuje společně; jinak nejdřív nahrazení (čísla
     * bloků se nemění), pak odstranění, nakonec vložení 0x32 jako bloku 0.
     */
    int done = 0;
    int rc = EXIT_SUCCESS;
    st_TZX_BLOCK pair[2];
    if ( mop == META_OP_INSERT ) {
        if ( aop == META_OP_INSERT || aop == META_OP_REPLACE ) {
            pair[0] = ab;
            pair[1] = mb;
            rc = ( aop == META_OP_INSERT ) ? cmttmz_insert_blocks ( 0, pair, g_meta_no_speed, 2 )
                                           : cmttmz_replace_block ( ai, pair, g_meta_no_speed, 2, 0 );
            ab.data = pair[0].data;
            mb.data = pair[1].data;
            if ( rc == EXIT_SUCCESS ) done = 1;
        } else {
            if ( aop == META_OP_REMOVE ) {
                rc = cmttmz_remove_blocks ( &ai, 1 );
                if ( rc == EXIT_SUCCESS ) done = 1;
            }
            if ( rc == EXIT_SUCCESS ) {
                int at = ( aop == META_OP_NONE && ai >= 0 ) ? ai + 1 : 0;
                rc = cmttmz_insert_blocks ( at, &mb, g_meta_no_speed, 1 );
                if ( rc == EXIT_SUCCESS ) done = 1;
            }
        }
    } else {
        if ( aop == META_OP_REPLACE ) {
            rc = cmttmz_replace_block ( ai, &ab, g_meta_no_speed, 1, 0 );
            if ( rc == EXIT_SUCCESS ) done = 1;
        }
        if ( rc == EXIT_SUCCESS && mop == META_OP_REPLACE ) {
            rc = cmttmz_replace_block ( mi, &mb, g_meta_no_speed, 1, 0 );
            if ( rc == EXIT_SUCCESS ) done = 1;
        }
        if ( rc == EXIT_SUCCESS && ( aop == META_OP_REMOVE || mop == META_OP_REMOVE ) ) {
            int ids[2];
            uint32_t cnt = 0;
            if ( aop == META_OP_REMOVE ) ids[cnt++] = ai;
            if ( mop == META_OP_REMOVE ) ids[cnt++] = mi;
            if ( cnt == 2 && ids[0] > ids[1] ) {
                int t = ids[0];
                ids[0] = ids[1];
                ids[1] = t;
            }
            rc = cmttmz_remove_blocks ( ids, cnt );
            if ( rc == EXIT_SUCCESS ) done = 1;
        }
        if ( rc == EXIT_SUCCESS && aop == META_OP_INSERT ) {
            rc = cmttmz_insert_blocks ( 0, &ab, g_meta_no_speed, 1 );
            if ( rc == EXIT_SUCCESS ) done = 1;
        }
    }
    /* po úspěchu převzala data páska (NULL), jinak zůstala tady */
    free ( ab.data );
    free ( mb.data );
    if ( changed ) *changed = done;
    /* změnu taktu určuje nástroj sám (meta_clocks(), i u vložení); varování dílčích
     * úprav (mazání bloku 0x43 v cmttmz_edit_apply()) by větu zdvojilo */
    cmttmz_set_edit_warning ( "" );

    if ( done ) {
        uint32_t clocks_n1 = 0;
        uint32_t *clocks1 = meta_clocks ( cmttmz_get_file ( ), &clocks_n1 );
        int differs = ( clocks_n0 != clocks_n1 )
                      || ( clocks_n0 && memcmp ( clocks0, clocks1, clocks_n0 * sizeof ( uint32_t ) ) != 0 );
        g_free ( clocks1 );
        if ( replaced ) {
            char text[256];
            g_strlcpy ( text, "Characters outside ISO 8859-1 were replaced with '?' in: ", sizeof ( text ) );
            int first = 1;
            for ( uint32_t k = 0; k < CMTTMZ_TAPE_INFO_FIELDS; k++ ) {
                if ( !( replaced & ( 1u << k ) ) ) continue;
                if ( !first ) g_strlcat ( text, ", ", sizeof ( text ) );
                g_strlcat ( text, g_meta_fields[k].key, sizeof ( text ) );
                first = 0;
            }
            meta_warn ( text );
        }
        /* věta stejná jako u importu, jediná definice v cmt_tmz.h */
        if ( differs ) meta_warn ( CMTTMZ_CLOCK_WARNING );
    }
    g_free ( clocks0 );
    return rc;
}


/* ------------------------------------------------------------------------- */
/*  Kontrola pásky                                                           */
/* ------------------------------------------------------------------------- */


/** @brief Nález během sběru: položka a pořadí vzniku (pro stabilní řazení). */
typedef struct st_META_FIND {
    st_CMTTMZ_CHECK_ITEM item; /**< nález */
    uint32_t seq;              /**< pořadí vzniku */
} st_META_FIND;


/**
 * @brief Seznam nálezů kontroly (připojuje se na konec, seřadí se jednou na konci).
 * @invariant count <= cap <= CMTTMZ_CHECK_MAX_ITEMS; dropped = počet nálezů nad mez.
 */
typedef struct st_META_FINDINGS {
    st_META_FIND *items; /**< nálezy (g_renew) */
    uint32_t count;      /**< počet */
    uint32_t cap;        /**< kapacita */
    uint32_t dropped;    /**< nálezy, které se nad mez CMTTMZ_CHECK_MAX_ITEMS nevešly */
} st_META_FINDINGS;


/**
 * @brief Přidá nález (nad mez CMTTMZ_CHECK_MAX_ITEMS jen započítá).
 * @param fs Seznam. @param block Číslo bloku. @param severity Závažnost. @param fmt Formát textu.
 */
static void meta_finding ( st_META_FINDINGS *fs, int block, int severity, const char *fmt, ... ) G_GNUC_PRINTF ( 4, 5 );
static void meta_finding ( st_META_FINDINGS *fs, int block, int severity, const char *fmt, ... ) {
    if ( fs->count >= CMTTMZ_CHECK_MAX_ITEMS ) {
        fs->dropped++;
        return;
    }
    if ( fs->count == fs->cap ) {
        fs->cap = fs->cap ? fs->cap * 2 : 16;
        if ( fs->cap > CMTTMZ_CHECK_MAX_ITEMS ) fs->cap = CMTTMZ_CHECK_MAX_ITEMS;
        fs->items = g_renew ( st_META_FIND, fs->items, fs->cap );
    }
    st_META_FIND *it = &fs->items[fs->count];
    it->seq = fs->count;
    it->item.block_id = block;
    it->item.severity = severity;
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( it->item.text, sizeof ( it->item.text ), fmt, ap );
    va_end ( ap );
    fs->count++;
}


/**
 * @brief Porovnání nálezů pro qsort(): číslo bloku, pak pořadí vzniku (stabilní řazení).
 * @param a Nález. @param b Nález. @return <0, 0, >0.
 */
static int meta_finding_cmp ( const void *a, const void *b ) {
    const st_META_FIND *x = ( const st_META_FIND* ) a;
    const st_META_FIND *y = ( const st_META_FIND* ) b;
    if ( x->item.block_id != y->item.block_id ) return ( x->item.block_id < y->item.block_id ) ? -1 : 1;
    return ( x->seq < y->seq ) ? -1 : ( x->seq > y->seq );
}


/**
 * @brief Kontrola hlavičky MZF a délky těla bloku MZ.
 * @param fs Nálezy. @param i Číslo bloku. @param hdr Hlavička v pořadí hostitele. @param body_size Délka těla v bloku.
 */
static void meta_check_mzf ( st_META_FINDINGS *fs, int i, const st_MZF_HEADER *hdr, uint32_t body_size ) {
    if ( mzf_header_validate ( hdr ) != MZF_OK ) {
        meta_finding ( fs, i, CMTTMZ_CHECK_WARNING, "File name in the header has no terminator 0Dh" );
    }
    if ( hdr->ftype < MZF_FTYPE_OBJ || hdr->ftype > MZF_FTYPE_RB ) {
        meta_finding ( fs, i, CMTTMZ_CHECK_WARNING, "Unknown file type 0x%02X in the header", hdr->ftype );
    }
    /* BSD a BRD nesou tělo jako kusy po 258 B, fsize délku těla neudává */
    if ( hdr->ftype != MZF_FTYPE_BSD && hdr->ftype != MZF_FTYPE_BRD && hdr->fsize != body_size ) {
        meta_finding ( fs, i, CMTTMZ_CHECK_WARNING, "Body length %u B differs from the header size %u B", body_size,
                       ( unsigned ) hdr->fsize );
    }
}


/**
 * @brief Kontrola bloku MZ s MZF (0x40, 0x41, 0x46).
 * @param fs Nálezy. @param i Číslo bloku. @param b Blok (nemění se).
 */
static void meta_check_mz_block ( st_META_FINDINGS *fs, int i, const st_TZX_BLOCK *b ) {
    if ( b->id == TMZ_BLOCK_ID_MZ_FILE ) {
        st_TMZ_MZFILE mf;
        en_TZX_ERROR err = tmz_mzfile_parse ( b, &mf );
        if ( err != TZX_OK ) {
            meta_finding ( fs, i, CMTTMZ_CHECK_WARNING, "MZ file block cannot be parsed: %s", tzx_error_string ( err ) );
            return;
        }
        st_MZF_HEADER hdr;
        memcpy ( &hdr, mf.header, sizeof ( hdr ) );
        hdr.fsize = endianity_bswap16_LE ( hdr.fsize );
        hdr.fstrt = endianity_bswap16_LE ( hdr.fstrt );
        hdr.fexec = endianity_bswap16_LE ( hdr.fexec );
        meta_check_mzf ( fs, i, &hdr, mf.body_size );
        if ( !tmz_profile_find ( mf.profile_id ) ) {
            meta_finding ( fs, i, CMTTMZ_CHECK_INFO, "Profile %s is not known to this emulator", mf.profile_id );
        }
        tmz_mzfile_free ( &mf );
        return;
    }
    /* 0x40, 0x41: parser bloku převádí endianitu na místě, proto kopie */
    if ( !b->data || !b->length ) {
        meta_finding ( fs, i, CMTTMZ_CHECK_WARNING, "MZ file block cannot be parsed: %s", tzx_error_string ( TZX_ERROR_INVALID_BLOCK ) );
        return;
    }
    st_TZX_BLOCK copy = *b;
    copy.data = ( uint8_t* ) g_memdup2 ( b->data, b->length );
    en_TZX_ERROR err = TZX_OK;
    st_MZF *mzf = tmz_block_to_mzf ( &copy, &err );
    if ( !mzf ) {
        meta_finding ( fs, i, CMTTMZ_CHECK_WARNING, "MZ file block cannot be parsed: %s", tzx_error_string ( err ) );
    } else {
        meta_check_mzf ( fs, i, &mzf->header, mzf->body_size );
        mzf_free ( mzf );
    }
    g_free ( copy.data );
}


int cmttmz_tape_check ( st_CMTTMZ_CHECK_ITEM **items, uint32_t *count, uint32_t *blocks_checked ) {
    if ( items ) *items = NULL;
    if ( count ) *count = 0;
    if ( blocks_checked ) *blocks_checked = 0;
    const st_TZX_FILE *f = meta_file ( );
    if ( !f || !items || !count ) return EXIT_FAILURE;

    uint32_t nb = f->block_count;
    int has_call = 0, has_return = 0;
    for ( uint32_t i = 0; i < nb; i++ ) {
        if ( f->blocks[i].id == TZX_BLOCK_ID_CALL_SEQUENCE ) has_call = 1;
        if ( f->blocks[i].id == TZX_BLOCK_ID_RETURN_FROM_SEQ ) has_return = 1;
    }

    st_META_FINDINGS fs = { NULL, 0, 0, 0 };
    GArray *loops = g_array_new ( FALSE, FALSE, sizeof ( int ) );
    GArray *groups = g_array_new ( FALSE, FALSE, sizeof ( int ) );

    for ( uint32_t u = 0; u < nb; u++ ) {
        const st_TZX_BLOCK *b = &f->blocks[u];
        int i = ( int ) u;
        switch ( b->id ) {
            case TZX_BLOCK_ID_LOOP_START:
                if ( loops->len ) meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Loop inside another loop (not allowed in TZX)" );
                g_array_append_val ( loops, i );
                break;
            case TZX_BLOCK_ID_LOOP_END:
                if ( !loops->len ) meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Loop end without a loop start" );
                else g_array_set_size ( loops, loops->len - 1 );
                break;
            case TZX_BLOCK_ID_GROUP_START:
                if ( groups->len ) meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Group inside another group (not allowed in TZX)" );
                g_array_append_val ( groups, i );
                break;
            case TZX_BLOCK_ID_GROUP_END:
                if ( !groups->len ) meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Group end without a group start" );
                else g_array_set_size ( groups, groups->len - 1 );
                break;
            case TZX_BLOCK_ID_JUMP:
            {
                /* stejné čtení jako handle_jump() playeru: 2 B LE se znaménkem, relativně k bloku */
                if ( !b->data || b->length < 2 ) {
                    meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Jump block is damaged" );
                    break;
                }
                int16_t off = ( int16_t ) meta_rd16 ( b->data );
                long long target = ( long long ) i + off;
                if ( off == 0 ) meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Jump to itself (endless loop)" );
                else if ( target < 0 || target >= ( long long ) nb ) {
                    meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Jump to block %lld outside the tape", target );
                }
                break;
            }
            case TZX_BLOCK_ID_CALL_SEQUENCE:
            {
                /* stejné čtení jako handle_call_sequence(): WORD počet, pak offsety 2 B se znaménkem */
                uint32_t calls = ( b->data && b->length >= 2 ) ? meta_rd16 ( b->data ) : 0;
                if ( !b->data || b->length < 2 || b->length < 2u + calls * 2u ) {
                    meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Call sequence block is damaged" );
                    break;
                }
                for ( uint32_t k = 0; k < calls; k++ ) {
                    int16_t off = ( int16_t ) meta_rd16 ( b->data + 2 + k * 2 );
                    long long target = ( long long ) i + off;
                    if ( target < 0 || target >= ( long long ) nb ) {
                        meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Call to block %lld outside the tape", target );
                    }
                }
                if ( !has_return ) meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Call sequence without a return block on the tape" );
                break;
            }
            case TZX_BLOCK_ID_RETURN_FROM_SEQ:
                if ( !has_call ) meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Return without a call sequence on the tape" );
                break;
            case TMZ_BLOCK_ID_MZ_STANDARD_DATA:
            case TMZ_BLOCK_ID_MZ_TURBO_DATA:
            case TMZ_BLOCK_ID_MZ_FILE:
                meta_check_mz_block ( &fs, i, b );
                break;
            default:
                /* tmz_block_id_name(): "Unknown" pro neznámé ID (smlouva tzx.h a tmz.h) */
                if ( !strcmp ( tmz_block_id_name ( b->id ), "Unknown" ) ) {
                    meta_finding ( &fs, i, CMTTMZ_CHECK_WARNING, "Unknown block ID 0x%02X", b->id );
                }
                break;
        }
    }
    for ( guint k = 0; k < loops->len; k++ ) {
        meta_finding ( &fs, g_array_index ( loops, int, k ), CMTTMZ_CHECK_WARNING, "Loop start without a loop end" );
    }
    for ( guint k = 0; k < groups->len; k++ ) {
        meta_finding ( &fs, g_array_index ( groups, int, k ), CMTTMZ_CHECK_WARNING, "Group start without a group end" );
    }
    g_array_free ( loops, TRUE );
    g_array_free ( groups, TRUE );

    if ( fs.count ) qsort ( fs.items, fs.count, sizeof ( fs.items[0] ), meta_finding_cmp );
    uint32_t total = fs.count + ( fs.dropped ? 1u : 0u );
    st_CMTTMZ_CHECK_ITEM *out = total ? g_new ( st_CMTTMZ_CHECK_ITEM, total ) : NULL;
    for ( uint32_t k = 0; k < fs.count; k++ ) out[k] = fs.items[k].item;
    if ( fs.dropped ) {
        st_CMTTMZ_CHECK_ITEM *last = &out[fs.count];
        last->block_id = -1;
        last->severity = CMTTMZ_CHECK_INFO;
        /* mez bere nálezy v pořadí vzniku, ne prvních N podle bloku: "only N of them" */
        snprintf ( last->text, sizeof ( last->text ), "More problems were found; only %d of them are listed.",
                   CMTTMZ_CHECK_MAX_ITEMS );
    }
    g_free ( fs.items );

    *items = out;
    *count = total;
    if ( blocks_checked ) *blocks_checked = nb;
    return EXIT_SUCCESS;
}


/* ------------------------------------------------------------------------- */
/*  Převod profilu                                                           */
/* ------------------------------------------------------------------------- */


/** @brief Výchozí pauza za blokem při převodu, když ji blok neudává (ms, výchozí tmz_profile_args_init()). */
#define META_DEFAULT_PAUSE_MS 1000u

/** @brief Velikost těla zkušebního MZF pro zjištění schopností profilů. */
#define META_PROBE_BODY 256u


/**
 * @brief Schopnosti profilu registru zjištěné zkušební stavbou (meta_profiles()).
 */
typedef struct st_META_CAPS {
    const st_TMZ_PROFILE *p; /**< profil registru (statická data knihovny) */
    int offered;             /**< 1 = z obyčejného MZF sestaví blok výchozími parametry (je v nabídce) */
    int needs_legacy;        /**< 1 = výchozí parametry potřebují legacy_format normal (TAPEMZ-V1-LEGACY) */
    int accepts_speed;       /**< 1 = přijme rychlost CMTSPEED_2_1 */
} st_META_CAPS;

/** @brief Schopnosti všech profilů registru (pořadí registru), vzniknou jednou. */
static st_META_CAPS *g_meta_caps;
/** @brief Počet položek g_meta_caps (= tmz_profile_count()). */
static uint32_t g_meta_caps_count;
/** @brief Indexy nabízených profilů do g_meta_caps. */
static uint32_t *g_meta_offered;
/** @brief Počet nabízených profilů. */
static uint32_t g_meta_offered_count;
/** @brief Strážce jednorázové zkušební stavby (g_once_init_enter()). */
static gsize g_meta_caps_once = 0;


/**
 * @brief Sestaví zkušební blok profilem (jen zjištění, zda stavba projde).
 * @param id ID profilu. @param args Parametry. @param hdr Hlavička. @param body Tělo. @param size Délka těla.
 * @return 1 = stavba prošla (blok se hned uvolní).
 */
static int meta_try_build ( const char *id, const st_TMZ_PROFILE_ARGS *args, const st_MZF_HEADER *hdr,
                            const uint8_t *body, uint32_t size ) {
    st_TZX_BLOCK *b = tmz_profile_build_block ( id, args, hdr, body, size, NULL );
    if ( !b ) return 0;
    tmz_block_free ( b );
    return 1;
}


/**
 * @brief Jednou zjistí schopnosti profilů registru zkušební stavbou obyčejného MZF.
 *
 * Zkušební MZF: typ 01h, jméno "PROBE", META_PROBE_BODY bajtů těla. Profil je
 * nabízený, když ho sestaví s tmz_profile_args_init(), nebo s ním navíc
 * a legacy_format normal; rychlost přijímá, když projde i speed = 2:1.
 * Žádné jméno profilu se tu neporovnává.
 */
static void meta_profiles ( void ) {
    if ( !g_once_init_enter ( &g_meta_caps_once ) ) return;
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    hdr.ftype = MZF_FTYPE_OBJ;
    memset ( &hdr.fname, MZF_FNAME_TERMINATOR, sizeof ( hdr.fname ) );
    memcpy ( &hdr.fname, "PROBE", 5 );
    hdr.fsize = META_PROBE_BODY;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    uint8_t body[META_PROBE_BODY];
    memset ( body, 0, sizeof ( body ) );
    uint32_t n = ( uint32_t ) tmz_profile_count ( );
    g_meta_caps = g_new0 ( st_META_CAPS, n ? n : 1 );
    g_meta_offered = g_new0 ( uint32_t, n ? n : 1 );
    for ( uint32_t i = 0; i < n; i++ ) {
        st_META_CAPS *c = &g_meta_caps[i];
        c->p = tmz_profile_get ( i );
        st_TMZ_PROFILE_ARGS a;
        tmz_profile_args_init ( &a );
        if ( meta_try_build ( c->p->id, &a, &hdr, body, META_PROBE_BODY ) ) {
            c->offered = 1;
        } else {
            a.legacy_format = TMZ_LEGACY_NORMAL;
            if ( meta_try_build ( c->p->id, &a, &hdr, body, META_PROBE_BODY ) ) c->offered = c->needs_legacy = 1;
        }
        if ( c->offered ) {
            a.speed = CMTSPEED_2_1;
            c->accepts_speed = meta_try_build ( c->p->id, &a, &hdr, body, META_PROBE_BODY );
            g_meta_offered[g_meta_offered_count++] = i;
        }
    }
    g_meta_caps_count = n;
    g_once_init_leave ( &g_meta_caps_once, 1 );
}


/**
 * @brief Schopnosti profilu podle ID.
 * @param id ID profilu. @return Schopnosti, nebo NULL (profil mimo registr).
 */
static const st_META_CAPS* meta_caps ( const char *id ) {
    meta_profiles ( );
    for ( uint32_t i = 0; id && i < g_meta_caps_count; i++ ) {
        if ( !strcmp ( g_meta_caps[i].p->id, id ) ) return &g_meta_caps[i];
    }
    return NULL;
}


/**
 * @brief Výchozí parametry profilu pro převod (tmz_profile_args_init(), u profilu, který to potřebuje, legacy_format normal).
 * @param id ID profilu. @param[out] a Parametry.
 */
static void meta_profile_defaults ( const char *id, st_TMZ_PROFILE_ARGS *a ) {
    tmz_profile_args_init ( a );
    const st_META_CAPS *c = meta_caps ( id );
    if ( c && c->needs_legacy ) a->legacy_format = TMZ_LEGACY_NORMAL;
}


uint32_t cmttmz_profile_count ( void ) {
    meta_profiles ( );
    return g_meta_offered_count;
}


int cmttmz_profile_get ( uint32_t i, const char **id, const char **description, int *accepts_speed ) {
    meta_profiles ( );
    if ( i >= g_meta_offered_count ) return EXIT_FAILURE;
    const st_META_CAPS *c = &g_meta_caps[g_meta_offered[i]];
    if ( id ) *id = c->p->id;
    if ( description ) *description = c->p->description;
    if ( accepts_speed ) *accepts_speed = c->accepts_speed;
    return EXIT_SUCCESS;
}


/**
 * @brief Blok vložené pásky podle čísla.
 * @param block_id Číslo bloku. @return Blok, nebo NULL bez pásky / mimo rozsah.
 */
static const st_TZX_BLOCK* meta_block ( int block_id ) {
    const st_TZX_FILE *f = meta_file ( );
    if ( !f || block_id < 0 || ( uint32_t ) block_id >= f->block_count ) return NULL;
    return &f->blocks[block_id];
}


int cmttmz_block_profile_convertible ( int block_id ) {
    const st_TZX_BLOCK *b = meta_block ( block_id );
    if ( !b || b->id != TMZ_BLOCK_ID_MZ_FILE ) return 0;
    /* podmínky tmz_block_to_mzf() pro 0x46 (tmz_blocks.c), bez výpisu a bez kopie těla */
    st_TMZ_MZFILE mf;
    if ( tmz_mzfile_parse ( b, &mf ) != TZX_OK ) return 0;
    uint16_t fsize = ( uint16_t ) ( mf.header[0x12] | ( mf.header[0x13] << 8 ) );
    int ok = ( mf.body_size <= 0xFFFF ) && ( fsize == mf.body_size );
    tmz_mzfile_free ( &mf );
    return ok;
}


int cmttmz_block_profile ( int block_id, char *id, size_t size ) {
    const st_TZX_BLOCK *b = meta_block ( block_id );
    if ( !id || !size ) return EXIT_FAILURE;
    id[0] = '\0';
    if ( !b || b->id != TMZ_BLOCK_ID_MZ_FILE ) return EXIT_FAILURE;
    st_TMZ_MZFILE mf;
    if ( tmz_mzfile_parse ( b, &mf ) != TZX_OK ) return EXIT_FAILURE;
    g_strlcpy ( id, mf.profile_id, size );
    tmz_mzfile_free ( &mf );
    return EXIT_SUCCESS;
}


/**
 * @brief Přečte z bloku 0x46 ID profilu a PROFILE_ARGS.
 * @param b Blok 0x46. @param[out] profile ID profilu ("" při chybě). @param size Velikost bufferu.
 * @param[out] args Parametry (při neúspěchu výchozí tmz_profile_args_init()).
 * @return 1 = PROFILE_ARGS přečtené, 0 = chybí nebo nejdou přečíst.
 */
static int meta_block_args ( const st_TZX_BLOCK *b, char *profile, size_t size, st_TMZ_PROFILE_ARGS *args ) {
    profile[0] = '\0';
    tmz_profile_args_init ( args );
    st_TMZ_MZFILE mf;
    if ( tmz_mzfile_parse ( b, &mf ) != TZX_OK ) return 0;
    g_strlcpy ( profile, mf.profile_id, size );
    int found = 0;
    st_TMZ_TLV_ITER it;
    st_TMZ_TLV t;
    tmz_tlv_iter_init ( &it, mf.block_tlv, mf.block_tlv_length );
    while ( tmz_tlv_iter_next ( &it, &t ) == 1 ) {
        if ( t.tag != TMZ_TAG_PROFILE_ARGS ) continue;
        found = ( tmz_profile_args_parse ( ( const char* ) t.value, t.length, args ) == EXIT_SUCCESS );
        break;
    }
    tmz_mzfile_free ( &mf );
    if ( !found ) tmz_profile_args_init ( args );
    return found;
}


/**
 * @brief Skupiny parametrů profilu, které převod přenáší nebo hlásí jako nepřenesené.
 */
typedef enum en_META_GROUP {
    META_G_SPEED = 0,  /**< speed */
    META_G_TCRATIO,    /**< tc_a, tc_b */
    META_G_CMTSPEED,   /**< cmtspeed[4] (vlastní pulzy) */
    META_G_LOADER,     /**< loader */
    META_G_GAP,        /**< gap_rom */
    META_G_LEGACY,     /**< legacy_format, pulseset, lgap, sgap, pulses[4] */
    META_G_BD,         /**< bd */
    META_G_COUNT,      /**< počet skupin */
} en_META_GROUP;

/** @brief Názvy skupin pro varování (anglicky). */
static const char *const g_meta_group_names[META_G_COUNT] = {
    "speed", "TurboCopy speed ratio", "custom pulse lengths", "preloader setting", "tone length (gap)",
    "TMZ 1.0 format settings", "baud rate",
};


/**
 * @brief Jsou parametry skupiny stejné?
 * @param a Parametry. @param b Parametry. @param g Skupina. @return 1 = stejné.
 */
static int meta_group_equal ( const st_TMZ_PROFILE_ARGS *a, const st_TMZ_PROFILE_ARGS *b, int g ) {
    switch ( g ) {
        case META_G_SPEED: return a->speed == b->speed;
        case META_G_TCRATIO: return a->tc_a == b->tc_a && a->tc_b == b->tc_b;
        case META_G_CMTSPEED: return !memcmp ( a->cmtspeed, b->cmtspeed, sizeof ( a->cmtspeed ) );
        case META_G_LOADER: return a->loader == b->loader;
        case META_G_GAP: return a->gap_rom == b->gap_rom;
        case META_G_LEGACY:
            return a->legacy_format == b->legacy_format && a->pulseset == b->pulseset && a->lgap == b->lgap
                   && a->sgap == b->sgap && !memcmp ( a->pulses, b->pulses, sizeof ( a->pulses ) );
        case META_G_BD: return a->bd == b->bd;
        default: return 1;
    }
}


/**
 * @brief Zkopíruje parametry skupiny.
 * @param dst Cíl. @param src Zdroj. @param g Skupina.
 */
static void meta_group_copy ( st_TMZ_PROFILE_ARGS *dst, const st_TMZ_PROFILE_ARGS *src, int g ) {
    switch ( g ) {
        case META_G_SPEED: dst->speed = src->speed; break;
        case META_G_TCRATIO: dst->tc_a = src->tc_a; dst->tc_b = src->tc_b; break;
        case META_G_CMTSPEED: memcpy ( dst->cmtspeed, src->cmtspeed, sizeof ( dst->cmtspeed ) ); break;
        case META_G_LOADER: dst->loader = src->loader; break;
        case META_G_GAP: dst->gap_rom = src->gap_rom; break;
        case META_G_LEGACY:
            dst->legacy_format = src->legacy_format;
            dst->pulseset = src->pulseset;
            dst->lgap = src->lgap;
            dst->sgap = src->sgap;
            memcpy ( dst->pulses, src->pulses, sizeof ( dst->pulses ) );
            break;
        case META_G_BD: dst->bd = src->bd; break;
        default: break;
    }
}


/**
 * @brief Výchozí PROFILE_ARGS profilu, jak je profil zapíše při stavbě tohoto MZF výchozími parametry.
 *
 * Srovnávací základ "nastavení, která uživatel zvolil": parametr bloku stejný
 * jako tady se nepřenáší ani nehlásí. Profil mimo registr nebo stavba, která
 * neprojde: tmz_profile_args_init().
 *
 * @param id ID profilu bloku. @param mzf MZF bloku. @param[out] base Parametry.
 */
static void meta_profile_baseline ( const char *id, const st_MZF *mzf, st_TMZ_PROFILE_ARGS *base ) {
    tmz_profile_args_init ( base );
    if ( !meta_caps ( id ) ) return;
    st_TMZ_PROFILE_ARGS d;
    meta_profile_defaults ( id, &d );
    st_TZX_BLOCK *b = tmz_profile_build_block ( id, &d, &mzf->header, mzf->body, mzf->body_size, NULL );
    if ( !b ) return;
    char prof[TMZ_MZFILE_PROFILE_ID_MAX + 1];
    ( void ) meta_block_args ( b, prof, sizeof ( prof ), base );
    tmz_block_free ( b );
}


/**
 * @brief Sestaví blok profilem MZ800EMU-CMT jako záznam (cmttmz_build_mzf_block()) a k němu záznam rychlosti.
 *
 * Vlastní pulzy (args->cmtspeed) = CMTSPEED_CUSTOM s pulzy v µs, FIXED;
 * poměr (args->speed) = FIXED; jinak výchozí rychlost decku, DEFAULT.
 *
 * @param mzf MZF. @param args Parametry (pause_ms <= 65535). @param[out] sp Záznam rychlosti.
 * @return Blok (tmz_block_free()), nebo NULL.
 */
static st_TZX_BLOCK* meta_build_emu ( const st_MZF *mzf, const st_TMZ_PROFILE_ARGS *args, st_CMTTMZ_BLOCK_SPEED *sp ) {
    memset ( sp, 0, sizeof ( *sp ) );
    if ( args->pause_ms > 0xFFFFu ) return NULL;
    st_MZTAPE_PULSES_LENGTH pl;
    memset ( &pl, 0, sizeof ( pl ) );
    en_CMTSPEED bspeed;
    const st_MZTAPE_PULSES_LENGTH *pulses = NULL;
    if ( args->cmtspeed[0] || args->cmtspeed[1] || args->cmtspeed[2] || args->cmtspeed[3] ) {
        if ( mztape_pulses_set_us ( &pl, args->cmtspeed[0], args->cmtspeed[1], args->cmtspeed[2], args->cmtspeed[3] ) != EXIT_SUCCESS ) {
            return NULL;
        }
        bspeed = CMTSPEED_CUSTOM;
        pulses = &pl;
        sp->origin = CMTTMZ_SPEED_ORIGIN_FIXED;
    } else if ( args->speed != CMTSPEED_NONE ) {
        bspeed = args->speed;
        sp->origin = CMTTMZ_SPEED_ORIGIN_FIXED;
    } else {
        bspeed = g_cmt.mz_cmtspeed;
        if ( cmtspeed_is_custom ( bspeed ) ) pulses = &g_cmt.mz_custom_pulses;
        sp->origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    }
    sp->speed = bspeed;
    if ( pulses ) sp->pulses = *pulses;
    sp->pause_ms = ( uint16_t ) args->pause_ms;
    return cmttmz_build_mzf_block ( &mzf->header, mzf->body, mzf->body_size, bspeed, pulses, ( uint16_t ) args->pause_ms );
}


/**
 * @brief Dal by dosavadní profil s parametry bloku jiný blok než ten na pásce?
 *
 * Jedna zkušební stavba dosavadním profilem s PROFILE_ARGS bloku. Blok,
 * který profil sám zapsal, vyjde bajtově shodně; rozdíl znamená části, které
 * PROFILE_ARGS nepopisuje a přestavba je spočítá znovu (vnucené součty
 * ck_header / ck_body z cizího nástroje nebo z WAV, další TLV).
 *
 * Profil MZ800EMU-CMT se nezkouší: jeho blok vzniká stavbou emulátoru
 * podle rychlosti decku v době záznamu (záznam rychlosti, ne PROFILE_ARGS),
 * zkouška by hlásila rozdíl i u nezměněného bloku. Neznámý profil nebo
 * stavba, která neprojde: 0 (nic se nehlásí).
 *
 * @param b Dosavadní blok. @param old_id Jeho profil. @param old Jeho PROFILE_ARGS (čitelné).
 * @param mzf MZF bloku. @return 1 = liší se, 0 = shodný nebo nezjistitelné.
 * @note Staví blok MZF (do 64 KB) pod device_state_lock() volajícího.
 */
static int meta_rebuild_differs ( const st_TZX_BLOCK *b, const char *old_id, const st_TMZ_PROFILE_ARGS *old, const st_MZF *mzf ) {
    if ( !old_id[0] || !strcmp ( old_id, TMZ_PROFILE_ID_MZ800EMU_CMT ) || !meta_caps ( old_id ) ) return 0;
    st_TZX_BLOCK *t = tmz_profile_build_block ( old_id, old, &mzf->header, mzf->body, mzf->body_size, NULL );
    if ( !t ) return 0;
    const int d = ( t->id != b->id || t->length != b->length || ( b->length && memcmp ( t->data, b->data, b->length ) != 0 ) );
    tmz_block_free ( t );
    return d;
}


int cmttmz_block_convert_profile ( int block_id, const char *profile_id, en_CMTSPEED speed ) {
    cmttmz_set_edit_warning ( "" );
    if ( !meta_file ( ) ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    const st_TZX_BLOCK *b = meta_block ( block_id );
    if ( !b ) {
        meta_error ( "Block %d does not exist", block_id );
        return EXIT_FAILURE;
    }
    const st_META_CAPS *caps = meta_caps ( profile_id );
    if ( !profile_id || !caps ) {
        meta_error ( "Unknown profile: %s", profile_id ? profile_id : "(none)" );
        return EXIT_FAILURE;
    }
    if ( !cmttmz_block_profile_convertible ( block_id ) ) {
        meta_error ( "Block %d has no MZ file that can be converted", block_id );
        return EXIT_FAILURE;
    }
    if ( speed != CMTSPEED_NONE && !caps->accepts_speed ) {
        meta_error ( "Profile %s does not accept a speed", profile_id );
        return EXIT_FAILURE;
    }
    if ( speed != CMTSPEED_NONE && !cmtspeed_is_valid ( speed ) ) {
        cmttmz_set_edit_error ( "Invalid speed" );
        return EXIT_FAILURE;
    }

    char old_id[TMZ_MZFILE_PROFILE_ID_MAX + 1];
    st_TMZ_PROFILE_ARGS old;
    int old_ok = meta_block_args ( b, old_id, sizeof ( old_id ), &old );
    if ( !old_ok ) {
        /* pauza ze záznamu rychlosti bloku, jinak výchozí */
        st_CMTTMZ_BLOCK_SPEED rec;
        old.pause_ms = META_DEFAULT_PAUSE_MS;
        if ( cmttmz_get_block_speed_record ( block_id, &rec ) == EXIT_SUCCESS && rec.origin != CMTTMZ_SPEED_ORIGIN_NONE ) {
            old.pause_ms = rec.pause_ms;
        }
    }
    int same = !strcmp ( old_id, profile_id );
    if ( same && speed == CMTSPEED_NONE ) {
        /* tentýž profil bez rychlosti: nic se nemění */
        cmttmz_set_edit_error ( "" );
        return EXIT_SUCCESS;
    }

    st_MZF *mzf = tmz_block_to_mzf ( b, NULL );
    if ( !mzf ) {
        meta_error ( "Block %d has no MZ file that can be converted", block_id );
        return EXIT_FAILURE;
    }

    st_TMZ_PROFILE_ARGS args;
    char lost[CMTTMZ_WARNING_SIZE];
    lost[0] = '\0';
    int unreadable = 0;
    if ( same && old_ok ) {
        /* tentýž profil s rychlostí: všechny parametry bloku, jen jiná rychlost */
        args = old;
        args.speed = speed;
        args.tc_a = args.tc_b = 0;
        memset ( args.cmtspeed, 0, sizeof ( args.cmtspeed ) );
    } else {
        /* PROFILE_ARGS bloku nečitelné: výchozí cílového profilu, ohlásí se varováním */
        unreadable = !old_ok;
        meta_profile_defaults ( profile_id, &args );
        args.pause_ms = old.pause_ms;
        args.machine = old.machine;
        if ( speed != CMTSPEED_NONE ) args.speed = speed;
        st_TMZ_PROFILE_ARGS base;
        meta_profile_baseline ( old_id, mzf, &base );
        /* nečitelné parametry: skupiny se nepřenáší ani nehlásí (old = výchozí knihovny, ne volba uživatele;
         * varování říká, že nastavení bloku nešla přečíst) */
        for ( int g = 0; old_ok && g < META_G_COUNT; g++ ) {
            if ( speed != CMTSPEED_NONE && ( g == META_G_SPEED || g == META_G_TCRATIO || g == META_G_CMTSPEED ) ) continue;
            if ( meta_group_equal ( &old, &base, g ) ) continue;
            st_TMZ_PROFILE_ARGS tmp = args;
            meta_group_copy ( &tmp, &old, g );
            if ( meta_try_build ( profile_id, &tmp, &mzf->header, mzf->body, mzf->body_size ) ) {
                args = tmp;
            } else {
                if ( lost[0] ) g_strlcat ( lost, ", ", sizeof ( lost ) );
                g_strlcat ( lost, g_meta_group_names[g], sizeof ( lost ) );
            }
        }
    }

    st_CMTTMZ_BLOCK_SPEED sp;
    memset ( &sp, 0, sizeof ( sp ) );
    st_TZX_BLOCK *nb = NULL;
    en_TMZ_PROFILE_ERROR perr = TMZ_PROFILE_OK;
    if ( !strcmp ( profile_id, TMZ_PROFILE_ID_MZ800EMU_CMT ) ) {
        /* jako záznam; stroj profil MZ800EMU-CMT z emulátoru nenese */
        if ( !same && args.machine != -1 ) {
            if ( lost[0] ) g_strlcat ( lost, ", ", sizeof ( lost ) );
            g_strlcat ( lost, "machine", sizeof ( lost ) );
        }
        nb = meta_build_emu ( mzf, &args, &sp );
        if ( !nb ) meta_error ( "Can't build the block with profile %s", profile_id );
    } else {
        nb = tmz_profile_build_block ( profile_id, &args, &mzf->header, mzf->body, mzf->body_size, &perr );
        if ( !nb ) meta_error ( "Can't build the block with profile %s: %s", profile_id, tmz_profile_error_string ( perr ) );
        /* sp: ORIGIN_NONE, rychlost dává blok (Set speed zakázané) */
    }
    /* části bloku mimo PROFILE_ARGS (vnucené součty ck_header / ck_body) přestavba spočítá znovu */
    const int rebuilt = ( nb && old_ok && meta_rebuild_differs ( b, old_id, &old, mzf ) );
    mzf_free ( mzf );
    if ( !nb ) return EXIT_FAILURE;

    /* výsledek stejný jako dosavadní blok: páska se nedotkne */
    st_CMTTMZ_BLOCK_SPEED rec;
    if ( cmttmz_get_block_speed_record ( block_id, &rec ) == EXIT_SUCCESS && nb->id == b->id && nb->length == b->length
         && ( !b->length || !memcmp ( nb->data, b->data, b->length ) ) && rec.origin == sp.origin && rec.speed == sp.speed
         && rec.pause_ms == sp.pause_ms && !memcmp ( &rec.pulses, &sp.pulses, sizeof ( sp.pulses ) ) ) {
        tmz_block_free ( nb );
        cmttmz_set_edit_error ( "" );
        return EXIT_SUCCESS;
    }

    st_TZX_BLOCK one = *nb;
    int rc = cmttmz_replace_block ( block_id, &one, &sp, 1, 0 );
    nb->data = one.data; /* po úspěchu NULL (data převzala páska), jinak se uvolní s blokem */
    tmz_block_free ( nb );
    if ( rc == EXIT_SUCCESS && unreadable ) {
        meta_warn ( "The profile settings of the block could not be read; profile defaults were used" );
    }
    if ( rc == EXIT_SUCCESS && rebuilt ) {
        meta_warn ( "Parts of the block that its profile settings do not describe (for example checksums) were rebuilt" );
    }
    if ( rc == EXIT_SUCCESS && lost[0] ) {
        char text[CMTTMZ_WARNING_SIZE];
        snprintf ( text, sizeof ( text ), "Settings of profile %s not carried over: %s", old_id[0] ? old_id : "(unknown)", lost );
        meta_warn ( text );
    }
    return rc;
}
