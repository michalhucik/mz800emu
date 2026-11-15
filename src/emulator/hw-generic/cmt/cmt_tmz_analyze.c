/**
 * @file   cmt_tmz_analyze.c
 * @brief  Analýza surového bloku pásky (0x18 CSW) na soubory a uplatnění výsledku (implementace cmt_tmz_analyze.h).
 *
 * Tři části s různými podmínkami vláken (viz cmt_tmz_analyze.h):
 * kopie pulzů a uplatnění pracují s vloženou páskou (vlákno emulátoru pod
 * device_state_lock()), analýza a histogram jsou čisté funkce nad kopií.
 * Rozhodování o souborech sdílí analýza se záznamem (cmttmz_rec_plan(),
 * cmttmz_rec_file_accepted(), cmttmz_rec_build_mzf() v cmt_tmz_record.c),
 * modul sám pásku mění jen přes úpravy pásky v cmt_tmz.c
 * (cmttmz_insert_blocks(), cmttmz_replace_block()).
 *
 * Paměť: kopie pulzů 4 B na pulz, analýza navíc dočasně 16 B na pulz
 * (st_WAV_PULSE pro analyzátor) a výsledek analyzátoru.
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_player.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/mzcmt_bsd/mzcmt_bsd.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/wav_analyzer/wav_analyzer.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"

#include "cmt.h"
#include "cmt_tmz.h"
#include "cmt_tmz_import.h"
#include "cmt_tmz_record.h"
#include "cmt_tmz_analyze.h"


/**
 * @brief Výsledek analýzy (neprůhledný typ z cmt_tmz_analyze.h).
 *
 * @invariant rows, mzf a src mají row_count položek (při row_count 0 NULL).
 * @invariant mzf[i] != NULL právě u řádků s výsledkem ADDABLE a ADDABLE_REPAIRED
 *            (tělo aspoň fsize bajtů, hlavička v pořadí hostitele).
 * @invariant block_id, tape_serial, pulse_count, sample_rate, block_length
 *            a block_hash jsou údaje kopie pulzů, ze které analýza vznikla.
 *
 * Vlastnictví: vše patří analýze (cmttmz_analysis_free()); MZF převzala
 * analýza z výsledku analyzátoru.
 */
/**
 * @brief Údaje analyzátoru o záznamu souboru, podle kterých se sestaví jeho blok.
 *
 * Kopie polí st_WAV_ANALYZER_FILE_RESULT (výsledek analyzátoru se po
 * sestavení řádků uvolní). U neznámého úseku vynulované (format UNKNOWN).
 */
typedef struct st_AN_SOURCE {
    en_WAV_TAPE_FORMAT format;   /**< formát záznamu */
    en_WAV_WRITER writer;        /**< rozpoznaný zapisovač (TurboCopy, Intercopy), jinak UNKNOWN */
    uint8_t tc_a;                /**< TurboCopy: čitatel poměru rychlosti */
    uint8_t tc_b;                /**< TurboCopy: jmenovatel poměru rychlosti */
    uint16_t ic_bd;              /**< Intercopy: rychlost v Bd */
    uint8_t cpmtape_variant;     /**< CPM-TAPE: en_WAV_CPMTAPE_VARIANT */
    uint16_t cpmtape_bd;         /**< CPM-TAPE: rychlost v Bd (0 = neznámá) */
    double leader_avg_us;        /**< průměrná půlperioda datového zaváděcího tónu v µs (odhad rychlosti bloku 0x41) */
} st_AN_SOURCE;


struct st_CMTTMZ_ANALYSIS {
    uint32_t row_count;                    /**< počet řádků */
    st_CMTTMZ_ANALYZE_ROW *rows;           /**< řádky v pořadí polohy */
    st_MZF **mzf;                          /**< data souboru řádku, který jde přidat (jinak NULL) */
    st_AN_SOURCE *src;                     /**< údaje záznamu řádku pro stavbu bloku */
    int block_id;                          /**< číslo bloku kopie */
    unsigned tape_serial;                  /**< čítač výměn pásky kopie */
    uint32_t pulse_count;                  /**< počet pulzů kopie */
    uint32_t sample_rate;                  /**< frekvence kopie */
    uint32_t block_length;                 /**< délka dat bloku kopie */
    uint32_t block_hash;                   /**< otisk dat bloku kopie */
    char error[CMTTMZ_ANALYZE_ERROR_SIZE]; /**< text chyby ("" = analýza proběhla) */
};


#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: nižší mez pulzů surového zbytku (0 = TMZ_PLAYER_MAX_BLOCK_PULSES). */
static uint32_t s_test_max_raw_pulses = 0;
#endif


void cmttmz_analyze_opts_default ( st_CMTTMZ_ANALYZE_OPTS *opts ) {
    if ( !opts ) return;
    opts->tolerance = WAV_ANALYZER_DEFAULT_TOLERANCE;
    opts->repair_header = 0;
    opts->repair_body = 0;
    opts->repair_bsd = 0;
}


/**
 * @brief Převod vzorků na µs se zaokrouhlením, bez přetečení.
 * @param samples Počet vzorků. @param rate Frekvence (> 0).
 * @return round ( samples * 1e6 / rate ).
 */
static uint64_t an_samples_to_us ( uint64_t samples, uint32_t rate ) {
    return ( samples / rate ) * 1000000u + ( ( samples % rate ) * 1000000u + rate / 2u ) / rate;
}


/**
 * @brief Přečte 32bitové číslo little-endian.
 * @param d Ukazatel na 4 bajty. @return Hodnota.
 */
static uint32_t an_rd32 ( const uint8_t *d ) {
    return ( uint32_t ) d[0] | ( ( uint32_t ) d[1] << 8 ) | ( ( uint32_t ) d[2] << 16 ) | ( ( uint32_t ) d[3] << 24 );
}


/**
 * @brief Nastaví text chyby úpravy pásky s číslem.
 * @param fmt Formát s jedním %d. @param n Číslo.
 */
static void an_error_num ( const char *fmt, int n ) {
    char msg[CMTTMZ_EDIT_ERROR_SIZE];
    snprintf ( msg, sizeof ( msg ), fmt, n );
    cmttmz_set_edit_error ( msg );
}


/**
 * @brief Otisk dat bloku (FNV-1a 32 bitů přes length bajtů data).
 *
 * Pojistka "týž blok" pro cmttmz_analysis_apply() nezávislá na čítači
 * výměn pásky; jeden průchod daty bez alokace.
 *
 * @param b Blok. @return Otisk (0x811C9DC5 u bloku bez dat).
 */
static uint32_t an_block_hash ( const st_TZX_BLOCK *b ) {
    uint32_t h = 0x811C9DC5u;
    for ( uint32_t i = 0; b->data && i < b->length; i++ ) {
        h ^= b->data[i];
        h *= 0x01000193u;
    }
    return h;
}


st_CMTTMZ_PULSES* cmttmz_analyze_copy_pulses ( int block_id ) {
    const st_TZX_FILE *f = cmttmz_get_file ( );
    if ( !f ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return NULL;
    }
    if ( block_id < 0 || ( uint32_t ) block_id >= f->block_count ) {
        an_error_num ( "Block %d does not exist", block_id );
        return NULL;
    }
    const st_TZX_BLOCK *b = &f->blocks[block_id];
    if ( b->id != TZX_BLOCK_ID_CSW_RECORDING ) {
        an_error_num ( "Block %d is not a raw signal block (CSW recording)", block_id );
        return NULL;
    }
    st_TZX_CSW_RECORDING csw;
    if ( TZX_OK != tzx_parse_csw_recording ( b->data, b->length, &csw ) || csw.sample_rate == 0
         || ( csw.data_length && !csw.data ) ) {
        an_error_num ( "Block %d contains invalid signal data", block_id );
        return NULL;
    }
    if ( csw.compression_type == TZX_CSW_COMPRESSION_ZRLE ) {
        an_error_num ( "Block %d uses Z-RLE compression, which is not supported", block_id );
        return NULL;
    }
    /* pulz RLE má aspoň 1 bajt: počet z hlavičky nad délkou dat je chyba ještě před alokací */
    if ( csw.compression_type != TZX_CSW_COMPRESSION_RLE || csw.pulse_count > csw.data_length ) {
        an_error_num ( "Block %d contains invalid signal data", block_id );
        return NULL;
    }

    st_CMTTMZ_PULSES *p = ( st_CMTTMZ_PULSES* ) calloc ( 1, sizeof ( *p ) );
    if ( !p ) {
        cmttmz_set_edit_error ( "Not enough memory" );
        return NULL;
    }
    if ( csw.pulse_count ) {
        p->lengths = ( uint32_t* ) malloc ( ( size_t ) csw.pulse_count * sizeof ( uint32_t ) );
        if ( !p->lengths ) {
            free ( p );
            cmttmz_set_edit_error ( "Not enough memory" );
            return NULL;
        }
    }
    /* RLE: bajt 1-255 = délka, 00h + DWORD = delší délka; pravidla jako tzx_csw_recording_check() */
    const uint8_t *d = csw.data;
    uint32_t pos = 0;
    uint64_t sum = 0;
    int bad = 0;
    for ( uint32_t i = 0; i < csw.pulse_count && !bad; i++ ) {
        if ( pos >= csw.data_length ) {
            bad = 1;
            break;
        }
        uint32_t len = d[pos++];
        if ( len == 0 ) {
            if ( csw.data_length - pos < 4 ) {
                bad = 1;
                break;
            }
            len = an_rd32 ( d + pos );
            pos += 4;
            if ( len == 0 ) {
                bad = 1;
                break;
            }
        }
        p->lengths[i] = len;
        sum += len;
    }
    if ( bad || pos != csw.data_length ) {
        cmttmz_pulses_free ( p );
        an_error_num ( "Block %d contains invalid signal data", block_id );
        return NULL;
    }
    p->sample_rate = csw.sample_rate;
    p->count = csw.pulse_count;
    p->first_level = 0;   /* první pulz CSW je LOW (tzx.h) */
    p->pause_ms = csw.pause_ms;
    p->tape_serial = cmttmz_get_tape_serial ( );
    p->block_id = block_id;
    p->unsaved = cmttmz_block_is_unsaved ( block_id );
    p->wav_type = cmttmz_block_is_wav ( block_id );
    p->total_us = an_samples_to_us ( sum, p->sample_rate );
    p->block_length = b->length;
    p->block_hash = an_block_hash ( b );
    return p;
}


void cmttmz_pulses_free ( st_CMTTMZ_PULSES *p ) {
    if ( !p ) return;
    free ( p->lengths );
    free ( p );
}


const char* cmttmz_analyze_reason_text ( en_CMTTMZ_ANALYZE_REASON reason ) {
    switch ( reason ) {
        case CMTTMZ_ANALYZE_REASON_REPAIRED: return "Repaired by the selected repair option";
        case CMTTMZ_ANALYZE_REASON_NO_FILE: return "No file found in this part of the signal";
        case CMTTMZ_ANALYZE_REASON_FORMAT: return "This recording format cannot be added as a file block";
        case CMTTMZ_ANALYZE_REASON_PROFILE: return "The file cannot be rebuilt as a tape block in this format";
        case CMTTMZ_ANALYZE_REASON_NO_MZF: return "The recording carries no Sharp MZ file";
        case CMTTMZ_ANALYZE_REASON_HEADER_CHECKSUM: return "Checksum error in the header";
        case CMTTMZ_ANALYZE_REASON_BODY_CHECKSUM: return "Checksum error in the body";
        case CMTTMZ_ANALYZE_REASON_REPAIR_DISABLED: return "The file needs a repair option that is not enabled";
        case CMTTMZ_ANALYZE_REASON_INCOMPLETE: return "The file is incomplete";
        case CMTTMZ_ANALYZE_REASON_NONE:
        default: return "";
    }
}


/**
 * @brief Stav součtu analyzátoru jako stav řádku.
 * @param s Stav analyzátoru. @return Stav řádku.
 */
static en_CMTTMZ_ANALYZE_CHECKSUM an_checksum ( en_WAV_CRC_STATUS s ) {
    if ( s == WAV_CRC_OK ) return CMTTMZ_ANALYZE_CHECKSUM_OK;
    if ( s == WAV_CRC_ERROR ) return CMTTMZ_ANALYZE_CHECKSUM_ERROR;
    return CMTTMZ_ANALYZE_CHECKSUM_NOT_AVAILABLE;
}


/** @brief Jméno bloku 0x41 v st_CMTTMZ_ANALYZE_ROW::writer_profile (TURBO a FASTIPL bez rozpoznaného zapisovače). */
#define AN_PROFILE_TURBO_DATA "MZ Turbo Data"

/** @brief Půlperioda zaváděcího tónu NORMAL 1200 Bd v µs (měření nahrávek při 44,1 kHz; jako wav2tmz v TapeMZ). */
#define AN_NORMAL_1200BD_SHORT_US 249.0

/** @brief Okno, ve kterém se rychlost bere jako 1:1 (+-15 %; jako wav2tmz v TapeMZ). */
#define AN_SPEED_1_1_TOLERANCE 0.15


/**
 * @brief Jde formát záznamu sestavit jako blok jinou cestou než standardní soubor NORMAL?
 *
 * CPM-CMT (NORMAL 2400 Bd), TURBO, FASTIPL, BSD a CPM-TAPE: analyzátor dá
 * MZF a registr TapeMZ má profil, kterým se soubor zapíše znovu. MZ-80B,
 * SINCLAIR, FSK, SLOW, DIRECT a ZX Spectrum jako blok sestavit nejdou
 * (emulované stroje je nenačtou nebo je nemá čím hrát).
 *
 * @param format Formát. @return 1 = ano.
 */
static int an_format_buildable ( en_WAV_TAPE_FORMAT format ) {
    return format == WAV_TAPE_FORMAT_CPM_CMT || format == WAV_TAPE_FORMAT_TURBO || format == WAV_TAPE_FORMAT_FASTIPL
           || format == WAV_TAPE_FORMAT_BSD || format == WAV_TAPE_FORMAT_CPM_TAPE;
}


/**
 * @brief Odhad rychlosti z průměrné půlperiody zaváděcího tónu (stejné pravidlo jako wav2tmz v TapeMZ).
 * @param leader_avg_us Průměrná půlperioda v µs (<= 0 = neznámá).
 * @return CMTSPEED_1_1 v okně AN_SPEED_1_1_TOLERANCE a při neznámé půlperiodě, jinak nejbližší poměr en_CMTSPEED.
 */
static en_CMTSPEED an_speed_from_leader ( double leader_avg_us ) {
    if ( !( leader_avg_us > 0.0 ) ) return CMTSPEED_1_1;
    double ratio = AN_NORMAL_1200BD_SHORT_US / leader_avg_us;
    if ( ratio >= ( 1.0 - AN_SPEED_1_1_TOLERANCE ) && ratio <= ( 1.0 + AN_SPEED_1_1_TOLERANCE ) ) return CMTSPEED_1_1;
    en_CMTSPEED best = CMTSPEED_2_1;
    double best_diff = 1e9;
    for ( int s = CMTSPEED_1_1; s < CMTSPEED_COUNT; s++ ) {
        double divisor = cmtspeed_get_divisor ( ( en_CMTSPEED ) s );
        if ( divisor <= 0.0 ) continue;
        double diff = fabs ( ratio - divisor );
        if ( diff < best_diff ) {
            best_diff = diff;
            best = ( en_CMTSPEED ) s;
        }
    }
    return best;
}


/**
 * @brief Sestaví blok souboru v jiném formátu než standardní NORMAL (mapování jako wav2tmz v TapeMZ).
 *
 * - CPM-CMT: blok 0x46 MZ800EMU-CMT rychlostí 2:1 CPM (cmttmz_build_mzf_block()),
 *   záznam rychlosti FIXED.
 * - TURBO: TurboCopy -> profil TC-1.21 s poměrem tc_a / tc_b a předzavaděčem,
 *   Intercopy -> IC-10.2-TURBO s rychlostí ic_bd, jinak blok 0x41 formátu
 *   TURBO s rychlostí odhadnutou ze zaváděcího tónu.
 * - FASTIPL: Intercopy -> IC-10.2-FASTIPL s ic_bd, jinak blok 0x41 formátu FASTIPL.
 * - CPM-TAPE: Intercopy -> IC-10.2-CPM, jinak PEZIK-2.0A; rychlost cpmtape_bd (0 = 2400).
 * - BSD: profil BASIC-1Z016; tělo MZF se rozdělí na kusy po 256 B s ID
 *   0, 1, ... a posledním FFFFh (jak zapisuje BASIC 1Z-016), hlavička
 *   s fsize, fstrt a fexec 0. Nejvýš 254 kusů.
 *
 * Signál bloku vyrobí profil znovu; časování původní nahrávky se nezachová.
 * Čistá funkce nad vstupy (registr profilů je statický), smí běžet
 * v pracovním vlákně analýzy.
 *
 * @param mzf Data souboru (hlavička v pořadí hostitele, tělo aspoň fsize bajtů; u BSD body_size bajtů).
 * @param src Údaje záznamu (formát podle an_format_buildable()).
 * @param pause_ms Pauza za blokem.
 * @param[out] block Blok (data vlastní volající, free()); NULL = jen zkouška, blok se zahodí.
 * @param[out] sp Záznam rychlosti bloku (smí být NULL): FIXED u CPM-CMT, jinak NONE.
 * @param[out] profile Čím je blok sestaven (statický řetězec, smí být NULL).
 * @return EXIT_SUCCESS; EXIT_FAILURE, když formát sestavit nejde nebo profil soubor odmítl.
 */
static int an_build_format_block ( const st_MZF *mzf, const st_AN_SOURCE *src, uint16_t pause_ms, st_TZX_BLOCK *block,
                                   st_CMTTMZ_BLOCK_SPEED *sp, const char **profile ) {
    if ( sp ) memset ( sp, 0, sizeof ( *sp ) );
    if ( profile ) *profile = "";
    if ( !mzf || !src ) return EXIT_FAILURE;
    st_TZX_BLOCK *nb = NULL;
    const char *id = NULL;
    st_TMZ_PROFILE_ARGS a;
    tmz_profile_args_init ( &a );
    a.pause_ms = pause_ms;
    uint32_t fsize = mzf->header.fsize;

    switch ( src->format ) {
        case WAV_TAPE_FORMAT_CPM_CMT:
            id = TMZ_PROFILE_ID_MZ800EMU_CMT;
            nb = cmttmz_build_mzf_block ( &mzf->header, mzf->body, fsize, CMTSPEED_2_1_CPM, NULL, pause_ms );
            if ( nb && sp ) {
                sp->origin = CMTTMZ_SPEED_ORIGIN_FIXED;
                sp->speed = CMTSPEED_2_1_CPM;
                sp->pause_ms = pause_ms;
            }
            break;

        case WAV_TAPE_FORMAT_TURBO:
        case WAV_TAPE_FORMAT_FASTIPL:
            if ( src->writer == WAV_WRITER_TURBOCOPY && src->format == WAV_TAPE_FORMAT_TURBO ) {
                id = "TC-1.21";
                a.tc_a = src->tc_a;
                a.tc_b = src->tc_b;
                a.loader = 1;
            } else if ( src->writer == WAV_WRITER_INTERCOPY ) {
                id = ( src->format == WAV_TAPE_FORMAT_TURBO ) ? "IC-10.2-TURBO" : "IC-10.2-FASTIPL";
                a.bd = src->ic_bd;
            }
            if ( id ) {
                nb = tmz_profile_build_block ( id, &a, &mzf->header, mzf->body, fsize, NULL );
            } else {
                /* zapisovač neznámý: blok 0x41 s rychlostí odhadnutou z datového zaváděcího tónu */
                id = AN_PROFILE_TURBO_DATA;
                if ( fsize > 0xFFFFu ) break;
                st_TMZ_MZ_TURBO_DATA params;
                memset ( &params, 0, sizeof ( params ) );
                params.machine = ( uint8_t ) TMZ_MACHINE_MZ800;
                params.pulseset = ( uint8_t ) TMZ_PULSESET_800;
                params.format = ( uint8_t ) ( ( src->format == WAV_TAPE_FORMAT_TURBO ) ? TMZ_FORMAT_TURBO : TMZ_FORMAT_FASTIPL );
                params.speed = ( uint8_t ) an_speed_from_leader ( src->leader_avg_us );
                params.pause_ms = pause_ms;
                memcpy ( &params.mzf_header, &mzf->header, sizeof ( st_MZF_HEADER ) );
                params.body_size = ( uint16_t ) fsize;
                nb = tmz_block_create_mz_turbo ( &params, mzf->body, ( uint16_t ) fsize );
            }
            break;

        case WAV_TAPE_FORMAT_CPM_TAPE:
            id = ( src->cpmtape_variant == WAV_CPMTAPE_INTERCOPY ) ? "IC-10.2-CPM" : "PEZIK-2.0A";
            a.bd = src->cpmtape_bd ? src->cpmtape_bd : 2400;
            nb = tmz_profile_build_block ( id, &a, &mzf->header, mzf->body, fsize, NULL );
            break;

        case WAV_TAPE_FORMAT_BSD:
        {
            id = "BASIC-1Z016";
            uint32_t size = ( mzf->body && mzf->body_size ) ? mzf->body_size : 0;
            uint32_t total = size ? ( size + MZCMT_BSD_CHUNK_DATA_SIZE - 1u ) / MZCMT_BSD_CHUNK_DATA_SIZE : 1u;
            if ( total > 254u ) break;   /* BASIC-1Z016 unese 254 kusů */
            uint32_t chunks_size = total * MZCMT_BSD_CHUNK_SIZE;
            uint8_t *chunks = ( uint8_t* ) calloc ( 1, chunks_size );
            if ( !chunks ) break;
            for ( uint32_t i = 0; i < total; i++ ) {
                uint8_t *chunk = &chunks[i * MZCMT_BSD_CHUNK_SIZE];
                uint16_t chunk_id = ( i + 1u == total ) ? 0xFFFFu : ( uint16_t ) i;   /* poslední kus ukončuje soubor */
                chunk[0] = ( uint8_t ) ( chunk_id & 0xFFu );
                chunk[1] = ( uint8_t ) ( chunk_id >> 8 );
                uint32_t offset = i * MZCMT_BSD_CHUNK_DATA_SIZE;
                if ( offset < size ) {
                    uint32_t n = size - offset;
                    if ( n > MZCMT_BSD_CHUNK_DATA_SIZE ) n = MZCMT_BSD_CHUNK_DATA_SIZE;
                    memcpy ( &chunk[2], &mzf->body[offset], n );
                }
            }
            st_MZF_HEADER hdr;
            memcpy ( &hdr, &mzf->header, sizeof ( hdr ) );
            hdr.fsize = 0;
            hdr.fstrt = 0;
            hdr.fexec = 0;
            nb = tmz_profile_build_block ( id, &a, &hdr, chunks, chunks_size, NULL );
            free ( chunks );
            break;
        }

        default:
            break;
    }
    if ( !nb ) return EXIT_FAILURE;
    if ( profile ) *profile = id;
    if ( block ) {
        *block = *nb;
        nb->data = NULL;   /* data převzal volající */
    }
    tmz_block_free ( nb );
    return EXIT_SUCCESS;
}


/**
 * @brief Je soubor jiného formátu než standardní NORMAL bezchybný a úplný?
 *
 * Součet hlavičky musí souhlasit, součet těla nesmí hlásit chybu (formát
 * bez součtu těla ho nemá k dispozici), BSD nesmí mít kus bez platné kopie,
 * žádná oprava, tělo aspoň fsize bajtů a platný rozsah pulzů.
 *
 * @param f Soubor analyzátoru s MZF. @param npulses Počet pulzů. @return 1 = ano.
 */
static int an_format_file_clean ( const st_WAV_ANALYZER_FILE_RESULT *f, uint32_t npulses ) {
    if ( f->header_crc != WAV_CRC_OK || f->body_crc == WAV_CRC_ERROR ) return 0;
    if ( f->format == WAV_TAPE_FORMAT_BSD && f->bsd_crc_errors ) return 0;
    if ( f->recovery_status != WAV_RECOVERY_NONE ) return 0;
    return cmttmz_rec_file_body_complete ( f ) && cmttmz_rec_file_range_valid ( f, npulses );
}


/**
 * @brief Určí výsledek a důvod řádku souboru.
 *
 * Pořadí pravidel viz cmttmz_analyze_pulses(); přijetí ADDABLE standardního
 * souboru NORMAL je totéž rozhodnutí jako u záznamu
 * (cmttmz_rec_file_accepted()), u formátů an_format_buildable() rozhoduje
 * an_format_file_clean(). Volba Repair incomplete BSD file dovolí přidat
 * i neúplný soubor BSD (ADDABLE_REPAIRED).
 *
 * @param f Soubor analyzátoru. @param npulses Počet pulzů. @param o Volby.
 * @param[out] row Řádek (result, reason).
 */
static void an_classify ( const st_WAV_ANALYZER_FILE_RESULT *f, uint32_t npulses, const st_CMTTMZ_ANALYZE_OPTS *o,
                          st_CMTTMZ_ANALYZE_ROW *row ) {
    int other = an_format_buildable ( f->format );
    if ( f->format != WAV_TAPE_FORMAT_NORMAL && !other ) {
        row->result = CMTTMZ_ANALYZE_CANNOT_ADD;
        row->reason = CMTTMZ_ANALYZE_REASON_FORMAT;
        return;
    }
    if ( !f->mzf ) {
        row->result = CMTTMZ_ANALYZE_CANNOT_ADD;
        row->reason = CMTTMZ_ANALYZE_REASON_NO_MZF;
        return;
    }
    if ( other ? an_format_file_clean ( f, npulses ) : cmttmz_rec_file_accepted ( f, npulses ) ) {
        row->result = CMTTMZ_ANALYZE_ADDABLE;
        row->reason = CMTTMZ_ANALYZE_REASON_NONE;
        return;
    }
    /* opravy, které dovolují zapnuté volby (FSIZE_REPAIRED žádná volba nezapíná) */
    uint32_t allowed = 0;
    if ( o->repair_body ) allowed |= WAV_RECOVERY_PARTIAL_BODY | WAV_RECOVERY_COPY_MERGED;
    if ( o->repair_header ) allowed |= WAV_RECOVERY_HEADER_ONLY;
    if ( o->repair_bsd && f->format == WAV_TAPE_FORMAT_BSD ) allowed |= WAV_RECOVERY_BSD_INCOMPLETE | WAV_RECOVERY_BSD_GAPS;
    int complete = cmttmz_rec_file_body_complete ( f ) && cmttmz_rec_file_range_valid ( f, npulses );
    int repair_ok = ( f->recovery_status != WAV_RECOVERY_NONE ) && !( f->recovery_status & ~allowed );
    if ( repair_ok && ( f->header_crc == WAV_CRC_OK ) && complete ) {
        row->result = CMTTMZ_ANALYZE_ADDABLE_REPAIRED;
        row->reason = CMTTMZ_ANALYZE_REASON_REPAIRED;
        return;
    }
    row->result = CMTTMZ_ANALYZE_STAYS_RAW;
    if ( f->header_crc != WAV_CRC_OK ) row->reason = CMTTMZ_ANALYZE_REASON_HEADER_CHECKSUM;
    else if ( ( f->recovery_status != WAV_RECOVERY_NONE ) && !repair_ok ) row->reason = CMTTMZ_ANALYZE_REASON_REPAIR_DISABLED;
    else if ( !complete ) row->reason = CMTTMZ_ANALYZE_REASON_INCOMPLETE;
    else row->reason = CMTTMZ_ANALYZE_REASON_BODY_CHECKSUM;
}


/**
 * @brief Vyplní řádek souboru z výsledku analyzátoru (bez rozsahu a časů).
 * @param f Soubor. @param npulses Počet pulzů. @param o Volby. @param[out] row Řádek.
 */
static void an_fill_file_row ( const st_WAV_ANALYZER_FILE_RESULT *f, uint32_t npulses, const st_CMTTMZ_ANALYZE_OPTS *o,
                               st_CMTTMZ_ANALYZE_ROW *row ) {
    row->kind = CMTTMZ_ANALYZE_FILE;
    row->format_name = wav_tape_format_name ( f->format );
    row->header_checksum = an_checksum ( f->header_crc );
    row->body_checksum = an_checksum ( f->body_crc );
    row->short_pulse_us = f->short_pulse_us;
    row->long_pulse_us = f->long_pulse_us;
    row->recovery_status = f->recovery_status;
    row->recovery_text = wav_recovery_status_string ( f->recovery_status );
    if ( f->mzf ) {
        uint8_t len = mzf_tools_get_fname_length ( &f->mzf->header );
        if ( len > sizeof ( row->name_sharp ) ) len = ( uint8_t ) sizeof ( row->name_sharp );
        memcpy ( row->name_sharp, f->mzf->header.fname.name, len );
        row->name_sharp_len = len;
        if ( len ) sharpmz_str_to_utf8 ( row->name_sharp, len, row->name, sizeof ( row->name ), SHARPMZ_CHARSET_EU );
        row->ftype = f->mzf->header.ftype;
        row->size = f->mzf->header.fsize;
    } else {
        row->size = f->tap_data_size;
    }
    row->writer_profile = "";
    an_classify ( f, npulses, o, row );
}


/**
 * @brief Zapíše údaje záznamu souboru potřebné ke stavbě bloku.
 * @param f Soubor analyzátoru. @param[out] src Údaje.
 */
static void an_fill_source ( const st_WAV_ANALYZER_FILE_RESULT *f, st_AN_SOURCE *src ) {
    src->format = f->format;
    src->writer = f->writer;
    src->tc_a = f->tc_a;
    src->tc_b = f->tc_b;
    src->ic_bd = f->ic_bd;
    src->cpmtape_variant = f->cpmtape_variant;
    src->cpmtape_bd = f->cpmtape_bd;
    src->leader_avg_us = f->leader.avg_period_us;
}


/**
 * @brief U řádku, který jde přidat, určí, čím se blok sestaví, a stavbu vyzkouší.
 *
 * Standardní soubor NORMAL: profil MZ800EMU-CMT (cmttmz_rec_build_mzf() při
 * uplatnění; stavbu nezkouší). Ostatní formáty: zkušební
 * an_build_format_block(); když profil soubor odmítne, řádek se změní na
 * CANNOT_ADD s důvodem PROFILE, takže uplatnění na stavbě neselže.
 *
 * @param mzf Data souboru. @param src Údaje záznamu. @param[in,out] row Řádek (ADDABLE nebo ADDABLE_REPAIRED).
 */
static void an_resolve_profile ( const st_MZF *mzf, const st_AN_SOURCE *src, st_CMTTMZ_ANALYZE_ROW *row ) {
    if ( src->format == WAV_TAPE_FORMAT_NORMAL ) {
        row->writer_profile = TMZ_PROFILE_ID_MZ800EMU_CMT;
        return;
    }
    const char *profile = "";
    if ( EXIT_SUCCESS == an_build_format_block ( mzf, src, CMTTMZ_ANALYZE_FILE_GAP_MS, NULL, NULL, &profile ) ) {
        row->writer_profile = profile;
        return;
    }
    row->result = CMTTMZ_ANALYZE_CANNOT_ADD;
    row->reason = CMTTMZ_ANALYZE_REASON_PROFILE;
    row->writer_profile = "";
}


/**
 * @brief Vyplní řádek neznámého úseku (bez rozsahu a časů).
 * @param[out] row Řádek.
 */
static void an_fill_unknown_row ( st_CMTTMZ_ANALYZE_ROW *row ) {
    row->kind = CMTTMZ_ANALYZE_UNKNOWN;
    row->format_name = wav_tape_format_name ( WAV_TAPE_FORMAT_UNKNOWN );
    row->header_checksum = CMTTMZ_ANALYZE_CHECKSUM_NOT_AVAILABLE;
    row->body_checksum = CMTTMZ_ANALYZE_CHECKSUM_NOT_AVAILABLE;
    row->result = CMTTMZ_ANALYZE_STAYS_RAW;
    row->reason = CMTTMZ_ANALYZE_REASON_NO_FILE;
    row->recovery_text = wav_recovery_status_string ( WAV_RECOVERY_NONE );
    row->writer_profile = "";
}


/**
 * @brief Doplní řádkům časy (µs od začátku bloku) z rozsahů pulzů.
 *
 * Časy jsou zaokrouhlené součty pulzů od začátku bloku: u souvislých
 * řádků je začátek dalšího přesně konec předchozího, krátký vynechaný
 * úsek mezi řádky má délku rozdílu.
 *
 * @param p Kopie pulzů. @param a Analýza s rozsahy řádků (vzestupně, bez překryvu).
 */
static void an_fill_times ( const st_CMTTMZ_PULSES *p, st_CMTTMZ_ANALYSIS *a ) {
    uint64_t cum = 0;
    uint32_t pulse = 0;
    for ( uint32_t i = 0; i < a->row_count; i++ ) {
        st_CMTTMZ_ANALYZE_ROW *r = &a->rows[i];
        for ( ; pulse < r->first_pulse; pulse++ ) cum += p->lengths[pulse];
        r->start_us = an_samples_to_us ( cum, p->sample_rate );
        for ( ; pulse < r->end_pulse; pulse++ ) cum += p->lengths[pulse];
        r->length_us = an_samples_to_us ( cum, p->sample_rate ) - r->start_us;
    }
}


/**
 * @brief Sestaví řádky analýzy z výsledku analyzátoru a výsledek uvolní.
 *
 * Plán cmttmz_rec_plan() v režimu všech souborů s mezí
 * CMTTMZ_REC_MIN_EDGES; soubor -> řádek souboru (MZF řádku, který jde
 * přidat, převezme analýza), úsek RAW -> neznámý řádek. Selhání analyzátoru
 * (@p aerr) nebo alokace plánu = jeden neznámý řádek přes celý blok a text
 * chyby; úspěšná analýza bez souboru a bez úseku od CMTTMZ_REC_MIN_EDGES
 * pulzů = žádný řádek.
 *
 * @param p Kopie pulzů (count > 0). @param o Volby (tolerance už omezená).
 * @param a Analýza s vyplněnou vazbou na kopii (bez řádků); při chybě alokace se uvolní.
 * @param res Výsledek analyzátoru (při chybě vynulovaný); vždy se uvolní.
 * @param aerr Výsledek wav_analyzer_analyze_pulses().
 * @return @p a s řádky; NULL při selhání alokace řádků (pak je @p a uvolněná).
 */
static st_CMTTMZ_ANALYSIS* an_build_rows ( const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYZE_OPTS *o,
                                           st_CMTTMZ_ANALYSIS *a, st_WAV_ANALYZER_RESULT *res, en_WAV_ANALYZER_ERROR aerr ) {
    st_CMTTMZ_REC_PART *parts = NULL;
    uint32_t nparts = 0;
    if ( aerr == WAV_ANALYZER_OK ) {
        parts = ( st_CMTTMZ_REC_PART* ) malloc ( ( ( size_t ) res->file_count * 2u + 1u ) * sizeof ( st_CMTTMZ_REC_PART ) );
        if ( !parts ) aerr = WAV_ANALYZER_ERROR_ALLOC;
        else nparts = cmttmz_rec_plan ( res, p->count, 1, CMTTMZ_REC_MIN_EDGES, parts );   /* krátké úseky jako záznam vynechá */
    }
    int failed = ( aerr != WAV_ANALYZER_OK );
    if ( failed ) {
        /* analýza selhala: celý blok jako jeden neznámý úsek (jako záloha záznamu) */
        snprintf ( a->error, sizeof ( a->error ), "Analysis failed: %s", wav_analyzer_error_string ( aerr ) );
        nparts = 0;
    }

    uint32_t nrows = failed ? 1 : nparts;
    a->rows = ( st_CMTTMZ_ANALYZE_ROW* ) calloc ( nrows ? nrows : 1, sizeof ( st_CMTTMZ_ANALYZE_ROW ) );
    a->mzf = ( st_MZF** ) calloc ( nrows ? nrows : 1, sizeof ( st_MZF* ) );
    a->src = ( st_AN_SOURCE* ) calloc ( nrows ? nrows : 1, sizeof ( st_AN_SOURCE ) );
    if ( !a->rows || !a->mzf || !a->src ) {
        free ( parts );
        wav_analyzer_result_destroy ( res );
        cmttmz_analysis_free ( a );
        return NULL;
    }
    a->row_count = nrows;
    if ( failed ) {
        an_fill_unknown_row ( &a->rows[0] );
        a->rows[0].first_pulse = 0;
        a->rows[0].end_pulse = p->count;
    }
    for ( uint32_t i = 0; i < nparts; i++ ) {
        st_CMTTMZ_ANALYZE_ROW *r = &a->rows[i];
        if ( parts[i].kind == CMTTMZ_REC_BLOCK_MZF ) {
            st_WAV_ANALYZER_FILE_RESULT *f = &res->files[parts[i].file];
            an_fill_file_row ( f, p->count, o, r );
            an_fill_source ( f, &a->src[i] );
            if ( r->result == CMTTMZ_ANALYZE_ADDABLE || r->result == CMTTMZ_ANALYZE_ADDABLE_REPAIRED ) {
                an_resolve_profile ( f->mzf, &a->src[i], r );
            }
            if ( r->result == CMTTMZ_ANALYZE_ADDABLE || r->result == CMTTMZ_ANALYZE_ADDABLE_REPAIRED ) {
                a->mzf[i] = f->mzf;   /* data převzala analýza */
                f->mzf = NULL;
            }
        } else {
            an_fill_unknown_row ( r );
        }
        r->first_pulse = parts[i].first_pulse;
        r->end_pulse = parts[i].end_pulse;
    }
    an_fill_times ( p, a );
    free ( parts );
    wav_analyzer_result_destroy ( res );
    return a;
}


/**
 * @brief Nová analýza s vazbou na kopii pulzů a omezenými volbami (bez řádků).
 * @param p Kopie. @param opts Volby (NULL = výchozí). @param[out] o Omezené volby.
 * @return Analýza (calloc); NULL při selhání alokace.
 */
static st_CMTTMZ_ANALYSIS* an_new ( const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYZE_OPTS *opts, st_CMTTMZ_ANALYZE_OPTS *o ) {
    if ( opts ) *o = *opts;
    else cmttmz_analyze_opts_default ( o );
    /* rozsah tolerance analyzátoru (wav_analyzer_types.h); NaN = výchozí */
    if ( isnan ( o->tolerance ) ) o->tolerance = WAV_ANALYZER_DEFAULT_TOLERANCE;
    if ( o->tolerance < WAV_ANALYZER_MIN_TOLERANCE ) o->tolerance = WAV_ANALYZER_MIN_TOLERANCE;
    if ( o->tolerance > WAV_ANALYZER_MAX_TOLERANCE ) o->tolerance = WAV_ANALYZER_MAX_TOLERANCE;
    st_CMTTMZ_ANALYSIS *a = ( st_CMTTMZ_ANALYSIS* ) calloc ( 1, sizeof ( *a ) );
    if ( !a ) return NULL;
    a->block_id = p->block_id;
    a->tape_serial = p->tape_serial;
    a->pulse_count = p->count;
    a->sample_rate = p->sample_rate;
    a->block_length = p->block_length;
    a->block_hash = p->block_hash;
    return a;
}


#ifdef MZTEST_HEADLESS
st_CMTTMZ_ANALYSIS* cmttmz_analysis_test_from_result ( const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYZE_OPTS *opts,
                                                        struct st_WAV_ANALYZER_RESULT *res ) {
    if ( !p || !res || p->count == 0 || !p->lengths ) return NULL;
    st_CMTTMZ_ANALYZE_OPTS o;
    st_CMTTMZ_ANALYSIS *a = an_new ( p, opts, &o );
    if ( !a ) {
        wav_analyzer_result_destroy ( res );
        return NULL;
    }
    return an_build_rows ( p, &o, a, res, WAV_ANALYZER_OK );
}
#endif


st_CMTTMZ_ANALYSIS* cmttmz_analyze_pulses ( const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYZE_OPTS *opts ) {
    if ( !p ) return NULL;
    st_CMTTMZ_ANALYZE_OPTS o;
    st_CMTTMZ_ANALYSIS *a = an_new ( p, opts, &o );
    if ( !a ) return NULL;
    if ( p->count == 0 || !p->lengths || p->sample_rate == 0 ) {
        snprintf ( a->error, sizeof ( a->error ), "The block contains no signal" );
        return a;
    }

    /* pulzy pro analyzátor: indexy 1:1 s kopií (pulz CSW nemá délku 0) */
    en_WAV_ANALYZER_ERROR aerr = WAV_ANALYZER_ERROR_ALLOC;
    st_WAV_ANALYZER_RESULT res;
    memset ( &res, 0, sizeof ( res ) );
    st_WAV_PULSE_SEQUENCE seq;
    memset ( &seq, 0, sizeof ( seq ) );
    seq.pulses = ( st_WAV_PULSE* ) malloc ( ( size_t ) p->count * sizeof ( st_WAV_PULSE ) );
    if ( seq.pulses ) {
        seq.count = seq.capacity = p->count;
        seq.sample_rate = p->sample_rate;
        for ( uint32_t i = 0; i < p->count; i++ ) {
            st_WAV_PULSE *wp = &seq.pulses[i];
            wp->duration_samples = p->lengths[i];
            wp->duration_us = ( double ) p->lengths[i] * 1000000.0 / ( double ) p->sample_rate;
            wp->level = ( p->first_level ^ ( int ) ( i & 1u ) ) & 1;
        }
        st_WAV_ANALYZER_CONFIG cfg;
        wav_analyzer_config_default ( &cfg );   /* keep_unknown 0, verbose 0 */
        cfg.tolerance = o.tolerance;
        cfg.recover_header = o.repair_header ? 1 : 0;
        cfg.recover_body = o.repair_body ? 1 : 0;
        cfg.recover_bsd = o.repair_bsd ? 1 : 0;
        aerr = wav_analyzer_analyze_pulses ( &seq, &cfg, &res );
        free ( seq.pulses );
    }

    return an_build_rows ( p, &o, a, &res, aerr );
}


void cmttmz_analysis_free ( st_CMTTMZ_ANALYSIS *a ) {
    if ( !a ) return;
    if ( a->mzf ) {
        for ( uint32_t i = 0; i < a->row_count; i++ ) mzf_free ( a->mzf[i] );
    }
    free ( a->mzf );
    free ( a->src );
    free ( a->rows );
    free ( a );
}


uint32_t cmttmz_analysis_row_count ( const st_CMTTMZ_ANALYSIS *a ) {
    return a ? a->row_count : 0;
}


const st_CMTTMZ_ANALYZE_ROW* cmttmz_analysis_row ( const st_CMTTMZ_ANALYSIS *a, uint32_t i ) {
    if ( !a || i >= a->row_count ) return NULL;
    return &a->rows[i];
}


const char* cmttmz_analysis_error ( const st_CMTTMZ_ANALYSIS *a ) {
    return a ? a->error : "";
}


int cmttmz_analysis_histogram ( const st_CMTTMZ_PULSES *p, uint32_t first, uint32_t end, double bin_us,
                                uint32_t *bins, uint32_t bin_count, double *max_us ) {
    if ( bins && bin_count ) memset ( bins, 0, ( size_t ) bin_count * sizeof ( uint32_t ) );
    if ( max_us ) *max_us = 0;
    if ( !p || !p->lengths || p->sample_rate == 0 || first >= end || end > p->count ) return EXIT_FAILURE;
    if ( !( bin_us > 0 ) || !isfinite ( bin_us ) ) return EXIT_FAILURE;
    if ( bin_count && !bins ) return EXIT_FAILURE;

    uint64_t n = ( uint64_t ) end - first;
    uint64_t take = ( n > CMTTMZ_ANALYZE_HIST_MAX_PULSES ) ? CMTTMZ_ANALYZE_HIST_MAX_PULSES : n;
    double scale = 1000000.0 / ( double ) p->sample_rate;
    /* nejdelší pulz přesně z celého rozsahu (jeden průchod bez paměti) jen na žádost, biny ze vzorku */
    uint32_t mx = 0;
    for ( uint32_t k = first; max_us && k < end; k++ ) {
        if ( p->lengths[k] > mx ) mx = p->lengths[k];
    }
    for ( uint64_t j = 0; bin_count && j < take; j++ ) {
        uint64_t idx = first + ( ( take == n ) ? j : ( j * n ) / take );
        double d = ( double ) p->lengths[idx] * scale;
        double k = d / bin_us;
        uint32_t bin = ( k < ( double ) bin_count ) ? ( uint32_t ) k : bin_count - 1u;
        bins[bin]++;
    }
    if ( max_us ) *max_us = ( double ) mx * scale;
    return EXIT_SUCCESS;
}


/**
 * @brief Volby stavby bloků souborů jako u záznamu (výchozí rychlost decku).
 * @param[out] opts Volby (cmttmz_import_opts_init() + rychlost a vlastní pulzy decku, jako cmt_record_finish()).
 */
static void an_build_opts ( st_CMTTMZ_IMPORT_OPTS *opts ) {
    cmttmz_import_opts_init ( opts );
    opts->default_speed = g_cmt.mz_cmtspeed;
    opts->custom_pulses = g_cmt.mz_custom_pulses;
}


/**
 * @brief Záznam rychlosti bloku souboru (původ DEFAULT, jako záznam v cmt_record_finish()).
 * @param opts Volby stavby. @param pause_ms Pauza bloku. @param[out] sp Záznam.
 */
static void an_file_speed ( const st_CMTTMZ_IMPORT_OPTS *opts, uint16_t pause_ms, st_CMTTMZ_BLOCK_SPEED *sp ) {
    memset ( sp, 0, sizeof ( *sp ) );
    sp->origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    sp->speed = opts->default_speed;
    if ( cmtspeed_is_custom ( opts->default_speed ) ) sp->pulses = opts->custom_pulses;
    sp->pause_ms = pause_ms;
}


/**
 * @brief Sestaví blok 0x18 z pulzů kopie [first, end) beze změny délek.
 *
 * Začíná-li úsek úrovní HIGH, dostane předřazený pulz LOW délky 1 vzorek
 * (první pulz CSW je LOW; stejné pravidlo jako cmttmz_rec_build_raw()).
 *
 * @param p Kopie. @param first První pulz. @param end Pulz za úsekem (> first).
 * @param pause_ms Pauza bloku. @param[out] block Blok (data vlastní volající, free()).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace nebo stavby.
 */
static int an_build_raw ( const st_CMTTMZ_PULSES *p, uint32_t first, uint32_t end, uint16_t pause_ms, st_TZX_BLOCK *block ) {
    int prepend = ( p->first_level ^ ( int ) ( first & 1u ) ) & 1;
    uint64_t n = ( uint64_t ) end - first + ( uint64_t ) prepend;
    if ( n > UINT32_MAX ) return EXIT_FAILURE;
    uint32_t *arr = ( uint32_t* ) malloc ( ( size_t ) n * sizeof ( uint32_t ) );
    if ( !arr ) return EXIT_FAILURE;
    if ( prepend ) arr[0] = 1;
    memcpy ( arr + prepend, &p->lengths[first], ( size_t ) ( end - first ) * sizeof ( uint32_t ) );
    en_TZX_ERROR terr = tzx_block_create_csw_recording ( p->sample_rate, pause_ms, arr, ( uint32_t ) n, block );
    free ( arr );
    return ( terr == TZX_OK ) ? EXIT_SUCCESS : EXIT_FAILURE;
}


/**
 * @brief Sestaví blok souboru (0x46 jako záznam) a jeho záznam rychlosti DEFAULT.
 * @param mzf Data souboru. @param opts Volby stavby. @param pause_ms Pauza za blokem.
 * @param[out] block Blok (data vlastní volající). @param[out] sp Záznam rychlosti.
 * @return EXIT_SUCCESS; EXIT_FAILURE při chybě stavby.
 */
static int an_build_file ( const st_MZF *mzf, const st_CMTTMZ_IMPORT_OPTS *opts, uint16_t pause_ms, st_TZX_BLOCK *block,
                           st_CMTTMZ_BLOCK_SPEED *sp ) {
    if ( EXIT_SUCCESS != cmttmz_rec_build_mzf ( mzf, opts, pause_ms, block ) ) return EXIT_FAILURE;
    an_file_speed ( opts, pause_ms, sp );
    return EXIT_SUCCESS;
}

/**
 * @brief Ověří, že analýza i kopie pulzů platí pro vloženou pásku.
 *
 * Analýza musí patřit ke kopii (číslo bloku, čítač, počet pulzů, frekvence,
 * délka a otisk dat); páska musí mít týž čítač výměn a na čísle bloku blok
 * 0x18 se stejnou frekvencí, počtem pulzů, délkou dat a otiskem dat
 * (an_block_hash(); pojistka nezávislá na čítači výměn pásky).
 *
 * @param p Kopie. @param a Analýza.
 * @return EXIT_SUCCESS; EXIT_FAILURE s textem chyby úpravy.
 */
static int an_apply_check ( const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a ) {
    if ( !p || !a || a->block_id != p->block_id || a->tape_serial != p->tape_serial || a->pulse_count != p->count
         || a->sample_rate != p->sample_rate || a->block_length != p->block_length || a->block_hash != p->block_hash ) {
        cmttmz_set_edit_error ( "The analysis does not belong to these pulses" );
        return EXIT_FAILURE;
    }
    const st_TZX_FILE *f = cmttmz_get_file ( );
    if ( !f ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    if ( cmttmz_get_tape_serial ( ) != p->tape_serial ) {
        cmttmz_set_edit_error ( "The tape changed since the analysis; analyze the block again" );
        return EXIT_FAILURE;
    }
    int same = 0;
    if ( p->block_id >= 0 && ( uint32_t ) p->block_id < f->block_count ) {
        const st_TZX_BLOCK *b = &f->blocks[p->block_id];
        st_TZX_CSW_RECORDING csw;
        same = ( b->id == TZX_BLOCK_ID_CSW_RECORDING ) && ( TZX_OK == tzx_parse_csw_recording ( b->data, b->length, &csw ) )
               && ( csw.pulse_count == p->count ) && ( csw.sample_rate == p->sample_rate )
               && ( b->length == p->block_length ) && ( an_block_hash ( b ) == p->block_hash );   /* otisk dat */
    }
    if ( !same ) {
        an_error_num ( "Block %d changed since the analysis; analyze the block again", p->block_id );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Uvolní data a pole bloků připravených k uplatnění.
 * @param blocks Pole (smí být NULL). @param count Počet položek. @param speeds Pole rychlostí.
 */
static void an_blocks_free ( st_TZX_BLOCK *blocks, uint32_t count, st_CMTTMZ_BLOCK_SPEED *speeds ) {
    if ( blocks ) {
        for ( uint32_t i = 0; i < count; i++ ) free ( blocks[i].data );
    }
    free ( blocks );
    free ( speeds );
}


/** @brief Nejvýš tolik pulzů smí mít surový zbytek (mez playeru; v testech nižší, cmttmz_analysis_test_set_max_pulses()). */
static uint32_t an_max_raw_pulses ( void ) {
#ifdef MZTEST_HEADLESS
    if ( s_test_max_raw_pulses ) return s_test_max_raw_pulses;
#endif
    return TMZ_PLAYER_MAX_BLOCK_PULSES;
}


/**
 * @brief Úsek posloupnosti Replace: blok souboru (řádek) nebo surový blok (rozsah pulzů).
 *
 * @invariant row >= 0 => blok souboru řádku row; row < 0 => surový blok
 *            [first, end) s first < end.
 */
typedef struct st_AN_SEGMENT {
    int32_t row;    /**< řádek souboru, nebo -1 = surový blok */
    uint32_t first; /**< surový blok: první pulz */
    uint32_t end;   /**< surový blok: pulz za úsekem */
} st_AN_SEGMENT;


/**
 * @brief Rozdělí řádky analýzy na úseky posloupnosti Replace.
 *
 * Vybraný řádek = úsek souboru; každý souvislý běh nevybraných řádků =
 * jeden surový úsek od first_pulse prvního do end_pulse posledního řádku
 * běhu (krátký vynechaný úsek uvnitř běhu v něm zůstane). Běh s prázdným
 * rozsahem (jen soubor celý uvnitř dřívějšího) úsek nedá.
 *
 * @param a Analýza. @param use Výběr. @param[out] seg Pole nejméně row_count úseků.
 * @return Počet úseků.
 */
static uint32_t an_segments ( const st_CMTTMZ_ANALYSIS *a, const uint8_t *use, st_AN_SEGMENT *seg ) {
    uint32_t n = 0;
    uint32_t i = 0;
    while ( i < a->row_count ) {
        if ( use[i] ) {
            seg[n].row = ( int32_t ) i;
            seg[n].first = seg[n].end = 0;
            n++;
            i++;
            continue;
        }
        uint32_t first = a->rows[i].first_pulse;
        while ( i < a->row_count && !use[i] ) i++;
        uint32_t end = a->rows[i - 1].end_pulse;
        if ( end > first ) {
            seg[n].row = -1;
            seg[n].first = first;
            seg[n].end = end;
            n++;
        }
    }
    return n;
}


int cmttmz_analysis_first_file_block ( const st_CMTTMZ_ANALYSIS *a, const uint8_t *use, int replace ) {
    if ( !a || !use ) return -1;
    uint32_t k = 0;
    while ( k < a->row_count && !use[k] ) k++;
    if ( k >= a->row_count ) return -1;
    if ( !replace ) return a->block_id + 1;
    /* stejné dělení jako an_segments(): před první vybraný řádek přijde surový
     * blok jen z běhu nevybraných řádků s neprázdným rozsahem */
    int lead = ( k > 0 && a->rows[k - 1].end_pulse > a->rows[0].first_pulse ) ? 1 : 0;
    return a->block_id + lead;
}


int cmttmz_analysis_apply ( const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a, const uint8_t *use,
                            int replace, uint32_t *added ) {
    if ( added ) *added = 0;
    cmttmz_set_edit_error ( "" );
    if ( EXIT_SUCCESS != an_apply_check ( p, a ) ) return EXIT_FAILURE;
    if ( !use && a->row_count ) {
        cmttmz_set_edit_error ( "The row selection is missing" );
        return EXIT_FAILURE;
    }

    uint32_t nsel = 0;
    for ( uint32_t i = 0; i < a->row_count; i++ ) {
        if ( !use[i] ) continue;
        if ( !a->mzf[i] ) {
            an_error_num ( "Row %d cannot be added", ( int ) i );
            return EXIT_FAILURE;
        }
        nsel++;
    }
    if ( nsel == 0 ) return EXIT_SUCCESS;

    st_CMTTMZ_IMPORT_OPTS opts;
    an_build_opts ( &opts );

    /* úseky: Add jen soubory; Replace soubory a běhy nevybraných řádků */
    st_AN_SEGMENT *seg = ( st_AN_SEGMENT* ) calloc ( a->row_count, sizeof ( st_AN_SEGMENT ) );
    if ( !seg ) {
        cmttmz_set_edit_error ( "Not enough memory" );
        return EXIT_FAILURE;
    }
    uint32_t nblocks = 0;
    if ( replace ) {
        nblocks = an_segments ( a, use, seg );
    } else {
        for ( uint32_t i = 0; i < a->row_count; i++ ) {
            if ( use[i] ) seg[nblocks++].row = ( int32_t ) i;
        }
    }
    st_TZX_BLOCK *blocks = ( st_TZX_BLOCK* ) calloc ( nblocks, sizeof ( st_TZX_BLOCK ) );
    st_CMTTMZ_BLOCK_SPEED *speeds = ( st_CMTTMZ_BLOCK_SPEED* ) calloc ( nblocks, sizeof ( st_CMTTMZ_BLOCK_SPEED ) );
    if ( !blocks || !speeds ) {
        free ( seg );
        an_blocks_free ( blocks, 0, speeds );
        cmttmz_set_edit_error ( "Not enough memory" );
        return EXIT_FAILURE;
    }

    /* za nahrazovaným blokem je na pásce další blok (soubor nikdy bez mezery před dalším blokem) */
    const st_TZX_FILE *f = cmttmz_get_file ( );
    int followed = ( ( uint32_t ) p->block_id + 1u < f->block_count );
    int ok = 1;
    for ( uint32_t k = 0; ok && k < nblocks; k++ ) {
        int last = ( k + 1 == nblocks );
        if ( seg[k].row >= 0 ) {
            uint32_t i = ( uint32_t ) seg[k].row;
            uint16_t pause;
            if ( !replace ) {
                /* Add: pauza jako u záznamu (1000 ms mezi dvěma bloky souborů, za posledním 0;
                 * šev v cmttmz_insert_blocks() mu dá 1000 ms, následuje-li blok) */
                pause = cmttmz_rec_mzf_pause ( &opts, !last );
            } else if ( !last ) {
                /* Replace: jako u záznamu - 1000 ms před dalším blokem souboru, jinak 0 */
                pause = cmttmz_rec_mzf_pause ( &opts, seg[k + 1].row >= 0 );
            } else {
                /* poslední nový blok: pauza nahrazeného bloku, s blokem za sebou aspoň
                 * 1000 ms (soubor nikdy bez mezery před dalším blokem) */
                pause = p->pause_ms;
                if ( followed && pause < CMTTMZ_ANALYZE_FILE_GAP_MS ) pause = CMTTMZ_ANALYZE_FILE_GAP_MS;
            }
            if ( a->src[i].format == WAV_TAPE_FORMAT_NORMAL ) {
                ok = ( EXIT_SUCCESS == an_build_file ( a->mzf[i], &opts, pause, &blocks[k], &speeds[k] ) );
            } else {
                /* jiný formát: blok podle profilu zapisovače, signál se vyrobí znovu */
                ok = ( EXIT_SUCCESS == an_build_format_block ( a->mzf[i], &a->src[i], pause, &blocks[k], &speeds[k], NULL ) );
            }
            if ( !ok ) an_error_num ( "Can't build a tape block from row %d", ( int ) i );
            continue;
        }
        /* surový zbytek: s předřazeným pulzem nesmí přesáhnout mez playeru */
        int prepend = ( p->first_level ^ ( int ) ( seg[k].first & 1u ) ) & 1;
        uint64_t npulses = ( uint64_t ) seg[k].end - seg[k].first + ( uint64_t ) prepend;
        if ( npulses > an_max_raw_pulses ( ) ) {
            an_error_num ( "Block %d cannot be split: a raw part would be longer than a playable block", p->block_id );
            ok = 0;
            break;
        }
        ok = ( EXIT_SUCCESS == an_build_raw ( p, seg[k].first, seg[k].end, last ? p->pause_ms : 0, &blocks[k] ) );
        if ( !ok ) {
            cmttmz_set_edit_error ( "Not enough memory" );
            break;
        }
        /* délka zbytku (pulzy a pauza) proti mezi přehrávání, jako přehrání bloku */
        st_TZX_CSW_RECORDING csw;
        if ( TZX_OK != tzx_parse_csw_recording ( blocks[k].data, blocks[k].length, &csw )
             || TZX_OK != tzx_csw_recording_check ( &csw ) ) {
            an_error_num ( "Block %d cannot be split: a raw part would be longer than a playable block", p->block_id );
            ok = 0;
        }
        /* speeds[k]: ORIGIN_NONE, rychlost dává blok */
    }
    free ( seg );
    if ( !ok ) {
        an_blocks_free ( blocks, nblocks, speeds );
        return EXIT_FAILURE;
    }

    int rc;
    if ( !replace ) rc = cmttmz_insert_blocks ( p->block_id + 1, blocks, speeds, nblocks );
    else rc = cmttmz_replace_block ( p->block_id, blocks, speeds, nblocks, p->wav_type );
    /* po úspěchu převzala data bloků páska (v poli NULL), jinak se uvolní */
    an_blocks_free ( blocks, nblocks, speeds );
    if ( rc != EXIT_SUCCESS ) return EXIT_FAILURE;
    /* nové bloky souborů pro výchozí takt: blok 0x43 s jiným taktem před nimi = varování jako
     * u importu (kanál úprav; Replace mohl větu dát už pro dosavadní bloky) */
    cmttmz_warn_new_blocks_clock ( replace ? ( uint32_t ) p->block_id : ( uint32_t ) p->block_id + 1u, nblocks );
    if ( added ) *added = nsel;
    return EXIT_SUCCESS;
}


#ifdef MZTEST_HEADLESS
void cmttmz_analysis_test_set_max_pulses ( uint32_t max_pulses ) {
    s_test_max_raw_pulses = max_pulses;
}
#endif
