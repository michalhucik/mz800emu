/**
 * @file   mzf_tools.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.2.1
 * @brief  Implementace pomocných funkcí pro MZF hlavičku.
 *
 * Implementace konverze jmen souborů (Sharp MZ ASCII <-> ASCII),
 * tovární funkce pro vytvoření hlavičky a debug výpisu.
 * Viz mzf_tools.h pro popis API.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-08-15: v2.2.0 - hex dump se znakovou sadou (en_MZF_DUMP_CHARSET,
 *   mzf_tools_dump_char, mzf_tools_hex_dump, mzf_tools_parse_dump_charset).
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mzf.h"
#include "mzf_tools.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"


/** @copydoc mzf_tools_set_fname */
void mzf_tools_set_fname ( st_MZF_HEADER *mzfhdr, const char *ascii_filename ) {
    unsigned ascii_fname_length = strlen ( ascii_filename );
    unsigned num_chars = ( ascii_fname_length >= MZF_FILE_NAME_LENGTH ) ? MZF_FILE_NAME_LENGTH : ascii_fname_length;

    /* Konverze ASCII → Sharp MZ ASCII */
    unsigned i;
    for ( i = 0; i < num_chars; i++ ) {
        mzfhdr->fname.name[i] = sharpmz_cnv_to ( ascii_filename[i] );
    }

    /* Vyplnění zbytku jména terminátory */
    for ( ; i < MZF_FILE_NAME_LENGTH; i++ ) {
        mzfhdr->fname.name[i] = MZF_FNAME_TERMINATOR;
    }

    /* Explicitní nastavení terminátoru struktury */
    mzfhdr->fname.terminator = MZF_FNAME_TERMINATOR;
}


/** @copydoc mzf_tools_get_fname_length */
uint8_t mzf_tools_get_fname_length ( const st_MZF_HEADER *mzfhdr ) {
    for ( unsigned i = 0; i < MZF_FILE_NAME_LENGTH; i++ ) {
        if ( mzfhdr->fname.name[i] == MZF_FNAME_TERMINATOR ) {
            return i;
        }
    }
    return MZF_FILE_NAME_LENGTH;
}


/** @copydoc mzf_tools_get_fname */
void mzf_tools_get_fname ( const st_MZF_HEADER *mzfhdr, char *ascii_filename ) {
    mzf_tools_get_fname_ex ( mzfhdr, ascii_filename,
                              MZF_FILE_NAME_LENGTH + 1, MZF_NAME_ASCII_EU );
}


/** @copydoc mzf_tools_get_fname_ex */
void mzf_tools_get_fname_ex ( const st_MZF_HEADER *mzfhdr, char *filename,
                               size_t buf_size, en_MZF_NAME_ENCODING encoding ) {
    if ( !mzfhdr || !filename || buf_size == 0 ) return;

    if ( encoding == MZF_NAME_ASCII_EU || encoding == MZF_NAME_ASCII_JP ) {
        /* ASCII režim (EU nebo JP) */
        const uint8_t *fname = mzfhdr->fname.name;
        char *dst = filename;
        char *dst_end = filename + buf_size - 1;

        for ( unsigned i = 0; i < MZF_FNAME_FULL_LENGTH && dst < dst_end; i++ ) {
            if ( fname[i] == MZF_FNAME_TERMINATOR ) break;
            uint8_t c = ( encoding == MZF_NAME_ASCII_JP )
                          ? sharpmz_jp_cnv_from ( fname[i] )
                          : sharpmz_cnv_from ( fname[i] );
            if ( c >= 0x20 ) {
                *dst++ = c;
            }
        }
        *dst = '\0';
        return;
    }

    /* UTF-8 režim (EU nebo JP) */
    sharpmz_charset_t charset = ( encoding == MZF_NAME_UTF8_JP || encoding == MZF_NAME_ASCII_JP )
                                  ? SHARPMZ_CHARSET_JP : SHARPMZ_CHARSET_EU;

    /* zjistíme délku jména (po terminátor 0x0D) */
    uint8_t src_len = mzf_tools_get_fname_length ( mzfhdr );

    sharpmz_str_to_utf8 ( mzfhdr->fname.name, src_len,
                           filename, buf_size, charset );
}


/** @copydoc mzf_tools_create_mzfhdr */
st_MZF_HEADER* mzf_tools_create_mzfhdr ( uint8_t ftype, uint16_t fsize, uint16_t fstrt, uint16_t fexec, const uint8_t *fname, unsigned fname_length, const uint8_t *cmnt ) {
    st_MZF_HEADER *mzfhdr = (st_MZF_HEADER*) malloc ( sizeof ( st_MZF_HEADER ) );
    if ( !mzfhdr ) return NULL;

    mzfhdr->ftype = ftype;
    mzfhdr->fsize = fsize;
    mzfhdr->fstrt = fstrt;
    mzfhdr->fexec = fexec;

    /* Komentář — vynulovat pokud není zadán */
    if ( !cmnt ) {
        memset ( mzfhdr->cmnt, 0x00, sizeof ( mzfhdr->cmnt ) );
    } else {
        memcpy ( mzfhdr->cmnt, cmnt, sizeof ( mzfhdr->cmnt ) );
    }

    /* Jméno — vyplnit terminátory, pak překopírovat zadané znaky */
    memset ( &mzfhdr->fname, MZF_FNAME_TERMINATOR, sizeof ( mzfhdr->fname ) );
    unsigned length = ( fname_length < MZF_FILE_NAME_LENGTH ) ? fname_length : MZF_FILE_NAME_LENGTH;
    memcpy ( mzfhdr->fname.name, fname, length );

    return mzfhdr;
}


/** @copydoc mzf_tools_parse_dump_charset */
int mzf_tools_parse_dump_charset ( const char *name, en_MZF_DUMP_CHARSET *charset ) {
    if ( !name || !charset ) return 0;
    if ( strcmp ( name, "raw" ) == 0 )     { *charset = MZF_DUMP_RAW;      return 1; }
    if ( strcmp ( name, "eu" ) == 0 )      { *charset = MZF_DUMP_ASCII_EU; return 1; }
    if ( strcmp ( name, "jp" ) == 0 )      { *charset = MZF_DUMP_ASCII_JP; return 1; }
    if ( strcmp ( name, "utf8-eu" ) == 0 ) { *charset = MZF_DUMP_UTF8_EU;  return 1; }
    if ( strcmp ( name, "utf8-jp" ) == 0 ) { *charset = MZF_DUMP_UTF8_JP;  return 1; }
    return 0;
}


/** @copydoc mzf_tools_dump_char */
const char* mzf_tools_dump_char ( uint8_t c, en_MZF_DUMP_CHARSET charset ) {
    static char buf[2];
    int converted = 0;
    int printable = 0;

    switch ( charset ) {

        case MZF_DUMP_ASCII_EU:
        case MZF_DUMP_ASCII_JP:
        {
            uint8_t a = ( charset == MZF_DUMP_ASCII_JP )
                          ? sharpmz_jp_convert_to_ASCII ( c, &converted, &printable )
                          : sharpmz_convert_to_ASCII ( c, &converted, &printable );
            if ( !converted || !printable ) return MZF_DUMP_PLACEHOLDER;
            buf[0] = ( char ) a;
            buf[1] = '\0';
            return buf;
        }

        case MZF_DUMP_UTF8_EU:
        case MZF_DUMP_UTF8_JP:
        {
            const char *u = ( charset == MZF_DUMP_UTF8_JP )
                              ? sharpmz_jp_convert_to_UTF8 ( c, &converted, &printable )
                              : sharpmz_eu_convert_to_UTF8 ( c, &converted, &printable );
            if ( !converted || !printable || !u ) return MZF_DUMP_PLACEHOLDER;
            return u;
        }

        case MZF_DUMP_RAW:
        default:
            if ( c >= 0x20 && c <= 0x7E ) {
                buf[0] = ( char ) c;
                buf[1] = '\0';
                return buf;
            }
            return MZF_DUMP_PLACEHOLDER;
    }
}


/** @copydoc mzf_tools_hex_dump */
void mzf_tools_hex_dump ( FILE *fp, const uint8_t *data, size_t size,
                          uint32_t base_addr, en_MZF_DUMP_CHARSET charset,
                          const char *indent ) {
    if ( !fp ) return;
    if ( !data && size > 0 ) return;
    if ( !indent ) indent = "";

    for ( size_t off = 0; off < size; off += 16 ) {
        fprintf ( fp, "%s%04X  ", indent, ( unsigned ) ( base_addr + off ) );

        /* hex část */
        for ( size_t j = 0; j < 16; j++ ) {
            if ( off + j < size ) {
                fprintf ( fp, "%02X ", data[off + j] );
            } else {
                fprintf ( fp, "   " );
            }
            if ( j == 7 ) fprintf ( fp, " " );
        }

        /* textová část */
        fprintf ( fp, " |" );
        for ( size_t j = 0; j < 16 && off + j < size; j++ ) {
            fputs ( mzf_tools_dump_char ( data[off + j], charset ), fp );
        }
        fprintf ( fp, "|\n" );
    }
}


/** @copydoc mzf_tools_dump_header */
void mzf_tools_dump_header ( const st_MZF_HEADER *mzfhdr, FILE *fp ) {
    if ( !mzfhdr || !fp ) return;

    /* Jméno souboru */
    char ascii_name[MZF_FILE_NAME_LENGTH + 1];
    mzf_tools_get_fname ( mzfhdr, ascii_name );

    fprintf ( fp, "MZF header:\n" );
    fprintf ( fp, "  ftype : 0x%02X\n", mzfhdr->ftype );
    fprintf ( fp, "  fname : \"%s\" (length: %u)\n", ascii_name, mzf_tools_get_fname_length ( mzfhdr ) );
    fprintf ( fp, "  fsize : 0x%04X (%u)\n", mzfhdr->fsize, mzfhdr->fsize );
    fprintf ( fp, "  fstrt : 0x%04X\n", mzfhdr->fstrt );
    fprintf ( fp, "  fexec : 0x%04X\n", mzfhdr->fexec );

    /* Komentář — prvních 16 bajtů hex dump */
    fprintf ( fp, "  cmnt  : " );
    unsigned dump_len = ( MZF_CMNT_LENGTH < 16 ) ? MZF_CMNT_LENGTH : 16;
    for ( unsigned i = 0; i < dump_len; i++ ) {
        fprintf ( fp, "%02X ", mzfhdr->cmnt[i] );
    }
    if ( MZF_CMNT_LENGTH > 16 ) fprintf ( fp, "..." );
    fprintf ( fp, "\n" );
}
