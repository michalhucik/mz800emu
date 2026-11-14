/**
 * @file   tmz_profile.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.9.0
 * @brief  Implementace registru profilů zapisovače (viz tmz_profile.h).
 *
 * Hodnoty profilů a jejich zdroje:
 * - ROM-800-NOMINAL: pulzy 240/278 a 470/494 µs (nominál dokumentace HW MZ-800, UniCMT 1x).
 * - ROM-800-EXACT: pulzy a 22 kontextů v T-stavech z záznamu ROM SAVE v emulátoru
 *   (spec 6.4).
 * - Obě ROM: struktura z kódu ROM (tapemark 40/40/1 a 20/20/1, 1 LONG za
 *   checksumem, kopie s oddělovačem 256 S), pauza mezi hlavičkou a tělem
 *   6 792 077 T, tóny ROM 22000/11000 nebo poloviční 11000/5500 (volba gap).
 * - TC-1.21: S = 71 + floor(480·b/a) tiků CTC0, tóny, koncovka 99 S, tapemarky
 *   a pauzy z záznamů TurboCopy V1.21 v emulátoru, předzavaděč z mzcmt_turbo.
 * - IC-10.2-NORMAL/TURBO/FASTIPL: pulzy v T-stavech z kódu Intercopy V10.2
 *   a jeho tabulek, struktura, pauza HIGH a bajty předzavaděče a hlavičky
 *   $BB ověřené záznamem hran v emulátoru.
 * - BASIC-1Z016: pulzy, 22 kontextů, struktura (tón a kopie u každého kusu)
 *   a pauza mezi úseky ze záznamu hran BASICu 1Z-016 V1.0A v emulátoru.
 * - MZ800EMU-CMT: žádné vlastní hodnoty; tóny, tapemarky a délky pulzů čte
 *   z knihovny mztape (mztape_get_format(), mztape_get_speed_pulses()),
 *   takže blok popisuje přesně signál, který mztape generuje.
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmz.h"
#include "tmz_tlv.h"
#include "tmz_mzfile.h"
#include "tmz_part_fm.h"
#include "tmz_part_manchester.h"
#include "tmz_profile.h"
#include "../mzcmt_turbo/mzcmt_turbo.h"
#include "../mztape/mztape.h"
#include "../mzcmt_fastipl/mzcmt_fastipl.h"
#include "../mzcmt_bsd/mzcmt_bsd.h"
#include "tmz_blocks.h"
#include "../endianity/endianity.h"


/** @brief Registr profilů. */
static const st_TMZ_PROFILE g_profiles[] = {
    { "ROM-800-NOMINAL", "MZ-800 ROM SAVE, nominal pulses 240/278 + 470/494 us", "documented" },
    { "ROM-800-EXACT", "MZ-800 ROM SAVE timing from the ROM code (T-states, 22 contexts)", "emulator-verified" },
    { "TC-1.21", "TurboCopy V1.21 SAVE (Kreidl), CTC-timed pulses, optional preloader", "emulator-verified" },
    { "TAPEMZ-V1-LEGACY", "TMZ v1.0 generators (mztape, FASTIPL V07, BSD) for converted v1.0 blocks", "tmz-v1.0-output" },
    { "UNICMT", "UniCMT player (Lukasek), pulses from the CMTSPEED header, structure as ROM-800-NOMINAL [unverified]",
      "hardware-measured pulses, structure unverified" },
    { "IC-10.2-NORMAL", "Intercopy V10.2 (NIPSOFT) NORMAL, 600-3200 Bd, T-states from the code", "emulator-verified" },
    { "IC-10.2-TURBO", "Intercopy V10.2 TURBO (TurboCopy preloader at 1200 Bd, body 600-3200 Bd)", "emulator-verified" },
    { "IC-10.2-FASTIPL", "Intercopy V10.2 FAST IPL ($BB header V07 at 1200 Bd, body 600-3200 Bd)", "emulator-verified" },
    { "PEZIK-2.0A", "Pezik/MarVan TAPE.COM V2.0a (CP/M, Manchester), 2400/3200 Bd, T-states from the code", "calculated" },
    { "IC-10.2-CPM", "Intercopy V10.2 CPM (Manchester), 1500-3600 Bd", "emulator-verified" },
    { "BASIC-1Z016", "MZ-800 BASIC 1Z-016 data file (WOPEN#/PRINT#), T-states measured in the emulator",
      "emulator-verified" },
    { TMZ_PROFILE_ID_MZ800EMU_CMT, "Signal mz800emu plays from an MZF (mztape MZ-800 SANE format, GDG clock timebase)",
      "emulator-output" },
};

/** @brief Index profilu ROM-800-NOMINAL v registru. */
#define PROF_NOMINAL 0
/** @brief Index profilu ROM-800-EXACT v registru. */
#define PROF_EXACT 1
/** @brief Index profilu TC-1.21 v registru. */
#define PROF_TC 2
/** @brief Index profilu TAPEMZ-V1-LEGACY v registru. */
#define PROF_LEGACY 3
/** @brief Index profilu UNICMT v registru. */
#define PROF_UNICMT 4
/** @brief Index profilu IC-10.2-NORMAL v registru. */
#define PROF_IC_NORMAL 5
/** @brief Index profilu IC-10.2-TURBO v registru. */
#define PROF_IC_TURBO 6
/** @brief Index profilu IC-10.2-FASTIPL v registru. */
#define PROF_IC_FASTIPL 7
/** @brief Index profilu PEZIK-2.0A v registru. */
#define PROF_PEZIK 8
/** @brief Index profilu IC-10.2-CPM v registru. */
#define PROF_IC_CPM 9
/** @brief Index profilu BASIC-1Z016 v registru. */
#define PROF_BASIC 10
/** @brief Index profilu MZ800EMU-CMT v registru. */
#define PROF_EMU_CMT 11

/** @brief Počet profilů. */
#define PROFILE_COUNT ( sizeof ( g_profiles ) / sizeof ( g_profiles[0] ) )

/** @brief Výchozí časová základna čtečky (T-stavy CPU MZ-800). */
#define CPU_TIMEBASE      3546895u
/** @brief Časová základna TurboCopy: tiky CTC0 (pxCLK/16). */
#define TC_TIMEBASE       1108405u
/** @brief Pauza TurboCopy mezi bloky: 100 ms v tikách (v emulátoru naměřeno 93-119 ms). */
#define TC_PAUSE_TICKS    110841u
/** @brief Koncovka TurboCopy: počet SHORT za LONG po checksumu (změřeno v emulátoru). */
#define TC_TAIL           99
/** @brief S předzavaděče TurboCopy (1:1). */
#define TC_S_PRELOADER    551
/** @brief Konstanta čtecího bodu ROM (LD A,82 na 0A4Ah). */
#define ROM_N             82
/** @brief NOMINAL: pauza za hlavičkou v µs (změřeno v emulátoru: 6 792 077 T minus druhá půlvlna END_L 494 µs). */
#define NOM_PAUSE_HDR_US  1914441u
/** @brief EXACT: pauza za hlavičkou v T (změřeno v emulátoru: 6 792 077 T minus druhá půlvlna END_L 1733 T). */
#define EXACT_PAUSE_HDR_T 6790344u
/** @brief Nejvíc částí v jednom bloku registru. */
#define MAX_PARTS         255

/**
 * @brief Tón TurboCopy podle S (tiky CTC0), změřeno v emulátoru.
 *
 * Počet pulzů tónu (GAP) pro všech 34 poměrů a/b, které TurboCopy V1.21 umí
 * zadat (a, b z 1..9, 1/2..3/1). 7 hodnot (551, 444, 391, 311, 276, 251, 231)
 * je ze záznamu hran v emulátoru, ostatní z počtu pulzů v záznamech SAVE
 * z emulátoru (WAV); pro těch 7 se oba zdroje shodují na pulz. Součin
 * počet x S je 3 495 030 až 3 497 152.
 */
static const struct { uint16_t s; uint16_t tone; } g_tc_tone[] = {
    { 1031, 3392 }, { 935, 3740 }, { 911, 3838 }, { 871, 4014 }, { 839, 4167 }, { 791, 4420 },
    { 743, 4705 }, { 711, 4917 }, { 688, 5081 }, { 671, 5210 }, { 647, 5403 }, { 631, 5540 },
    { 619, 5648 }, { 611, 5722 }, { 551, 6344 }, { 497, 7033 }, { 491, 7119 }, { 482, 7252 },
    { 471, 7422 }, { 455, 7683 }, { 444, 7873 }, { 431, 8110 }, { 413, 8464 }, { 391, 8940 },
    { 371, 9421 }, { 359, 9736 }, { 345, 10131 }, { 337, 10372 }, { 311, 11239 }, { 284, 12307 },
    { 276, 12664 }, { 263, 13290 }, { 251, 13925 }, { 231, 15130 },
};

/** @brief Kontexty ROM-800-EXACT (spec 6.4). */
static const st_MZCMT_FM_CONTEXT g_rom_exact_ctx[] = {
    { MZCMT_FM_CTX_TONE, 0, MZCMT_FM_NEXT_ANY, 844, 918 },
    { MZCMT_FM_CTX_TONE_LAST, 0, MZCMT_FM_NEXT_ANY, 844, 913 },
    { MZCMT_FM_CTX_TM_L, 1, MZCMT_FM_NEXT_ANY, 1664, 1728 },
    { MZCMT_FM_CTX_TM_L_LAST, 1, MZCMT_FM_NEXT_ANY, 1664, 1723 },
    { MZCMT_FM_CTX_TM_S, 0, MZCMT_FM_NEXT_ANY, 844, 908 },
    { MZCMT_FM_CTX_TM_S_LAST, 0, MZCMT_FM_NEXT_ANY, 844, 903 },
    { MZCMT_FM_CTX_TM_END_L, 1, MZCMT_FM_NEXT_ANY, 1664, 1861 },
    { MZCMT_FM_CTX_START, 1, 0, 1664, 1726 },
    { MZCMT_FM_CTX_START, 1, 1, 1664, 1716 },
    { MZCMT_FM_CTX_BIT, 0, 1, 844, 910 },
    { MZCMT_FM_CTX_BIT, 1, 1, 1664, 1740 },
    { MZCMT_FM_CTX_BIT_LAST, 0, MZCMT_FM_NEXT_ANY, 844, 1028 },
    { MZCMT_FM_CTX_BIT_LAST, 1, MZCMT_FM_NEXT_ANY, 1664, 1858 },
    { MZCMT_FM_CTX_BIT_LAST_DATA_END, 0, MZCMT_FM_NEXT_ANY, 844, 1041 },
    { MZCMT_FM_CTX_BIT_LAST_DATA_END, 1, MZCMT_FM_NEXT_ANY, 1664, 1871 },
    { MZCMT_FM_CTX_CK_BIT_LAST, 0, MZCMT_FM_NEXT_ANY, 844, 965 },
    { MZCMT_FM_CTX_CK_BIT_LAST, 1, MZCMT_FM_NEXT_ANY, 1664, 1795 },
    { MZCMT_FM_CTX_CK_BIT_END, 0, MZCMT_FM_NEXT_ANY, 844, 926 },
    { MZCMT_FM_CTX_CK_BIT_END, 1, MZCMT_FM_NEXT_ANY, 1664, 1756 },
    { MZCMT_FM_CTX_END_L, 1, MZCMT_FM_NEXT_ANY, 1664, 1733 },
    { MZCMT_FM_CTX_COPY_SEP, 0, MZCMT_FM_NEXT_ANY, 844, 906 },
    { MZCMT_FM_CTX_COPY_SEP_LAST, 0, MZCMT_FM_NEXT_ANY, 844, 1000 },
};

/** @brief BASIC-1Z016: pauza za úsekem v T (ticho 6 612 558 T od konce signálu po další úsek, změřeno v emulátoru, minus druhá půlvlna END_L 1806 T). */
#define BASIC_PAUSE_T 6610752u
/** @brief BASIC-1Z016: nejvíc kusů (blok 0x46 má nejvýš 255 částí). */
#define BASIC_MAX_CHUNKS 254u

/** @brief Kontexty BASIC-1Z016 (záznam hran BASICu 1Z-016 V1.0A v emulátoru). */
static const st_MZCMT_FM_CONTEXT g_basic_ctx[] = {
    { MZCMT_FM_CTX_TONE, 0, MZCMT_FM_NEXT_ANY, 868, 942 },
    { MZCMT_FM_CTX_TONE_LAST, 0, MZCMT_FM_NEXT_ANY, 868, 937 },
    { MZCMT_FM_CTX_TM_L, 1, MZCMT_FM_NEXT_ANY, 1739, 1803 },
    { MZCMT_FM_CTX_TM_L_LAST, 1, MZCMT_FM_NEXT_ANY, 1739, 1798 },
    { MZCMT_FM_CTX_TM_S, 0, MZCMT_FM_NEXT_ANY, 868, 932 },
    { MZCMT_FM_CTX_TM_S_LAST, 0, MZCMT_FM_NEXT_ANY, 868, 927 },
    { MZCMT_FM_CTX_TM_END_L, 1, MZCMT_FM_NEXT_ANY, 1739, 1946 },
    { MZCMT_FM_CTX_START, 1, 0, 1739, 1801 },
    { MZCMT_FM_CTX_START, 1, 1, 1739, 1791 },
    { MZCMT_FM_CTX_BIT, 0, 1, 868, 934 },
    { MZCMT_FM_CTX_BIT, 1, 1, 1739, 1815 },
    { MZCMT_FM_CTX_BIT_LAST, 0, MZCMT_FM_NEXT_ANY, 868, 1053 },
    { MZCMT_FM_CTX_BIT_LAST, 1, MZCMT_FM_NEXT_ANY, 1739, 1934 },
    { MZCMT_FM_CTX_BIT_LAST_DATA_END, 0, MZCMT_FM_NEXT_ANY, 868, 1061 },
    { MZCMT_FM_CTX_BIT_LAST_DATA_END, 1, MZCMT_FM_NEXT_ANY, 1739, 1942 },
    { MZCMT_FM_CTX_CK_BIT_LAST, 0, MZCMT_FM_NEXT_ANY, 868, 989 },
    { MZCMT_FM_CTX_CK_BIT_LAST, 1, MZCMT_FM_NEXT_ANY, 1739, 1870 },
    { MZCMT_FM_CTX_CK_BIT_END, 0, MZCMT_FM_NEXT_ANY, 868, 950 },
    { MZCMT_FM_CTX_CK_BIT_END, 1, MZCMT_FM_NEXT_ANY, 1739, 1831 },
    { MZCMT_FM_CTX_END_L, 1, MZCMT_FM_NEXT_ANY, 1739, 1806 },
    { MZCMT_FM_CTX_COPY_SEP, 0, MZCMT_FM_NEXT_ANY, 868, 929 },
    { MZCMT_FM_CTX_COPY_SEP_LAST, 0, MZCMT_FM_NEXT_ANY, 868, 1020 },
};


/**
 * @brief Rozpracovaný seznam částí (TLV v samostatných bufferech).
 *
 * @invariant n <= cap <= MAX_PARTS; parts[i].tlv ukazuje do tlv[i].data
 * @par Vlastnictví: pole parts a tlv i buffery tlv[] vlastní seznam, uvolní build_free().
 */
typedef struct st_BUILD {
    st_TMZ_PART *parts;    /**< části (cap položek) */
    st_TMZ_TLVBUF *tlv;    /**< TLV částí (cap položek) */
    uint16_t cap;          /**< kapacita */
    uint16_t n;            /**< počet částí */
    const uint8_t *ck_header;  /**< bajty checksumu hlavičky místo vypočtených, NULL = vypočítat */
    const uint8_t *ck_body;    /**< totéž pro tělo */
} st_BUILD;


/**
 * @brief Připraví prázdný seznam s kapacitou cap částí.
 * @param b Seznam.
 * @param cap Kapacita (1..MAX_PARTS).
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE při selhání alokace (b je pak prázdný, build_free() je bezpečné).
 */
static int build_init ( st_BUILD *b, uint16_t cap ) {
    memset ( b, 0, sizeof ( *b ) );
    b->parts = calloc ( cap, sizeof ( st_TMZ_PART ) );
    b->tlv = calloc ( cap, sizeof ( st_TMZ_TLVBUF ) );
    if ( !b->parts || !b->tlv ) {
        free ( b->parts );
        free ( b->tlv );
        b->parts = NULL;
        b->tlv = NULL;
        return EXIT_FAILURE;
    }
    b->cap = cap;
    return EXIT_SUCCESS;
}


/**
 * @brief Uvolní buffery TLV a pole rozpracovaného seznamu.
 * @param b Seznam.
 */
static void build_free ( st_BUILD *b ) {
    for ( uint16_t i = 0; i < b->n; i++ ) tmz_tlvbuf_free ( &b->tlv[i] );
    free ( b->parts );
    free ( b->tlv );
    memset ( b, 0, sizeof ( *b ) );
}


/**
 * @brief Přidá část FM.
 * @param b Seznam.
 * @param role Role (en_TMZ_PART_ROLE).
 * @param source Zdroj dat (en_TMZ_DATA_SOURCE).
 * @param inl Inline bajty (jen INLINE; musí žít do vytvoření bloku).
 * @param inl_len Délka inline bajtů.
 * @param slice_off Začátek výřezu těla (jen BODY_SLICE, jinak 0).
 * @param slice_len Délka výřezu těla (jen BODY_SLICE, jinak 0).
 * @param p Parametry FM.
 * @param c STOP a READER.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE (plný seznam, chyba kódování).
 */
static int add_part ( st_BUILD *b, uint8_t role, uint8_t source, const uint8_t *inl, uint32_t inl_len,
                      uint32_t slice_off, uint32_t slice_len, st_MZCMT_FM_PARAMS *p, const st_TMZ_PART_COMMON *c ) {
    if ( b->n >= b->cap ) return EXIT_FAILURE;
    st_TMZ_TLVBUF *t = &b->tlv[b->n];
    tmz_tlvbuf_init ( t );
    b->n++;   /* buffer patří do seznamu i při chybě, aby ho uvolnil build_free() */
    /*
     * Výchozí základna 0 = TIMEBASE se zapíše vždy: blok pak nezávisí
     * na taktu z bloku 0x43 v souboru, do kterého ho nástroj připojí.
     */
    const uint8_t *ck = ( role == TMZ_PART_ROLE_HEADER ) ? b->ck_header : ( role == TMZ_PART_ROLE_BODY ) ? b->ck_body : NULL;
    int explicit_ck = ck && p->checksum == MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE;
    if ( explicit_ck ) {
        p->checksum_explicit = 1;
        memcpy ( p->checksum_bytes, ck, 2 );
    }
    int enc = tmz_part_fm_encode ( p, c, 0, t );
    if ( explicit_ck ) p->checksum_explicit = 0;   /* parametry sdílí další části */
    if ( enc != EXIT_SUCCESS ) return EXIT_FAILURE;
    st_TMZ_PART *pt = &b->parts[b->n - 1];
    memset ( pt, 0, sizeof ( *pt ) );
    pt->role = role;
    pt->family = TMZ_FAMILY_FM;
    pt->source = source;
    pt->inline_data = inl;
    pt->inline_length = inl_len;
    pt->slice_offset = slice_off;
    pt->slice_length = slice_len;
    pt->tlv = t->data;
    pt->tlv_length = t->length;
    return EXIT_SUCCESS;
}


/**
 * @brief Parametry části ROM.
 * @param p Výstup.
 * @param exact 1 = ROM-800-EXACT (T-stavy, kontexty), 0 = NOMINAL (µs).
 * @param tone Počet SHORT tónu.
 * @param tm Počet L a S tapemarku (40 hlavička, 20 tělo).
 * @param pause Pauza za částí (jednotky časové základny).
 */
static void rom_params ( st_MZCMT_FM_PARAMS *p, int exact, uint32_t tone, uint16_t tm, uint32_t pause ) {
    mzcmt_fm_params_init ( p );
    if ( exact ) {
        p->timebase = CPU_TIMEBASE;
        p->short_first = 844;
        p->short_second = 920;
        p->long_first = 1664;
        p->long_second = 1750;
        p->context_count = sizeof ( g_rom_exact_ctx ) / sizeof ( g_rom_exact_ctx[0] );
        memcpy ( p->context, g_rom_exact_ctx, sizeof ( g_rom_exact_ctx ) );
    } else {
        p->timebase = 1000000;
        p->short_first = 240;
        p->short_second = 278;
        p->long_first = 470;
        p->long_second = 494;
    }
    p->tone = tone;
    p->tm_long = tm;
    p->tm_short = tm;
    p->tm_end_long = 1;
    p->checksum = MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE;
    p->end_long = 1;
    p->copies = 1;
    p->copy_sep = 256;
    p->pause = pause;
}


/**
 * @brief Parametry části TurboCopy.
 *
 * Půlvlny v tikách CTC0: SHORT (S+1)/2 a S/2 (celočíselně, dělení liché
 * předvolby v módu 3 obvodu 8253, změřeno v emulátoru), LONG S a S.
 *
 * @param p Výstup.
 * @param s S v tikách CTC0.
 * @param tone Počet SHORT tónu.
 * @param tm Počet L a S tapemarku.
 * @param pause Pauza za částí (tiky).
 */
static void tc_params ( st_MZCMT_FM_PARAMS *p, unsigned s, uint32_t tone, uint16_t tm, uint32_t pause ) {
    mzcmt_fm_params_init ( p );
    p->timebase = TC_TIMEBASE;
    p->short_first = ( s + 1 ) / 2;
    p->short_second = s / 2;
    p->long_first = s;
    p->long_second = s;
    p->tone = tone;
    p->tm_long = tm;
    p->tm_short = tm;
    p->tm_end_long = 1;
    p->checksum = MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE;
    p->end_long = 1;
    p->tail_short = TC_TAIL;
    p->pause = pause;
}


/**
 * @brief Tón TurboCopy pro dané S.
 * @param s S v tikách CTC0.
 * @return Počet SHORT, nebo 0 pro S bez měření.
 */
static uint16_t tc_tone ( unsigned s ) {
    for ( size_t i = 0; i < sizeof ( g_tc_tone ) / sizeof ( g_tc_tone[0] ); i++ ) {
        if ( g_tc_tone[i].s == s ) return g_tc_tone[i].tone;
    }
    return 0;
}


void tmz_profile_args_init ( st_TMZ_PROFILE_ARGS *args ) {
    args->speed = CMTSPEED_NONE;
    args->loader = 1;
    args->gap_rom = -1;
    args->pause_ms = 1000;
    args->machine = -1;
    args->legacy_format = TMZ_LEGACY_NONE;
    args->pulseset = -1;
    args->lgap = 0;
    args->sgap = 0;
    memset ( args->pulses, 0, sizeof ( args->pulses ) );
    memset ( args->cmtspeed, 0, sizeof ( args->cmtspeed ) );
    args->ck_header = NULL;
    args->ck_body = NULL;
    args->bd = 0;
    args->tc_a = 0;
    args->tc_b = 0;
}


size_t tmz_profile_count ( void ) {
    return PROFILE_COUNT;
}


const st_TMZ_PROFILE* tmz_profile_get ( size_t index ) {
    return ( index < PROFILE_COUNT ) ? &g_profiles[index] : NULL;
}


const st_TMZ_PROFILE* tmz_profile_find ( const char *id ) {
    if ( !id ) return NULL;
    for ( size_t i = 0; i < PROFILE_COUNT; i++ ) {
        if ( strcmp ( g_profiles[i].id, id ) == 0 ) return &g_profiles[i];
    }
    return NULL;
}


/**
 * @brief Porovná úsek textu s řetězcem.
 * @param p Úsek. @param n Délka úseku. @param s Řetězec ukončený nulou.
 * @return Nenulové při shodě.
 */
static int span_eq ( const char *p, size_t n, const char *s ) {
    return strlen ( s ) == n && memcmp ( p, s, n ) == 0;
}


/**
 * @brief Přečte desítkové číslo 0..max z úseku textu.
 * @param v Úsek. @param n Délka. @param max Nejvyšší hodnota. @param[out] out Výsledek.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE (prázdné, nečíselné, větší než max).
 */
static int parse_u32 ( const char *v, size_t n, uint32_t max, uint32_t *out ) {
    if ( n == 0 || n > 10 ) return EXIT_FAILURE;
    uint64_t x = 0;
    for ( size_t i = 0; i < n; i++ ) {
        if ( v[i] < '0' || v[i] > '9' ) return EXIT_FAILURE;
        x = x * 10 + ( uint64_t ) ( v[i] - '0' );
    }
    if ( x > max ) return EXIT_FAILURE;
    *out = ( uint32_t ) x;
    return EXIT_SUCCESS;
}


/**
 * @brief Přečte 4 hodnoty oddělené '/' (pulzy LONG H, LONG L, SHORT H, SHORT L).
 * @param v Úsek. @param n Délka. @param min Nejmenší hodnota (0 nebo 1).
 * @param[out] out 4 hodnoty; při chybě může být částečně přepsáno.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE (jiný počet hodnot, hodnota mimo min..65535).
 */
static int parse_quad ( const char *v, size_t n, uint32_t min, uint16_t out[4] ) {
    size_t p0 = 0;
    for ( int k = 0; k < 4; k++ ) {
        size_t p1 = p0;
        while ( p1 < n && v[p1] != '/' ) p1++;
        if ( ( k < 3 ) != ( p1 < n ) ) return EXIT_FAILURE;   /* přesně 3 lomítka */
        uint32_t x;
        if ( parse_u32 ( v + p0, p1 - p0, 65535u, &x ) != EXIT_SUCCESS || x < min ) return EXIT_FAILURE;
        out[k] = ( uint16_t ) x;
        p0 = p1 + 1;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Rychlost Intercopy V10.2: čtecí bod a půlvlny v T-stavech (disassembly Intercopy 10.2, ověřeno v emulátoru).
 */
typedef struct st_IC_SPEED {
    uint16_t bd;    /**< popisek rychlosti v Bd */
    uint8_t rp;     /**< čtecí bod z tabulky $16F7 (FAST IPL rp, TURBO rp+5) */
    uint16_t sh;    /**< SHORT HIGH */
    uint16_t sl;    /**< SHORT LOW */
    uint16_t lh;    /**< LONG HIGH */
    uint16_t ll;    /**< LONG LOW */
} st_IC_SPEED;

/** @brief Tabulka rychlostí Intercopy V10.2. */
static const st_IC_SPEED g_ic_speed[] = {
    {  600, 167, 1664, 1755, 3315, 3328 },
    { 1200,  77,  832,  936, 1664, 1755 },
    { 2400,  32,  403,  494,  832,  923 },
    { 2800,  22,  312,  442,  624,  793 },
    { 3200,  17,  273,  416,  559,  637 },
};

/** @brief Pauza Intercopy mezi bloky v T (změřeno v emulátoru: 1 222 325 až 1 223 892 T po odečtení první HIGH tónu). */
#define IC_PAUSE_T 1222500u


/**
 * @brief Najde řádek tabulky Intercopy podle rychlosti.
 * @param bd Rychlost v Bd.
 * @return Řádek, nebo NULL.
 */
static const st_IC_SPEED* ic_speed_row ( uint16_t bd ) {
    for ( size_t i = 0; i < sizeof ( g_ic_speed ) / sizeof ( g_ic_speed[0] ); i++ ) {
        if ( g_ic_speed[i].bd == bd ) return &g_ic_speed[i];
    }
    return NULL;
}


/** @brief Počet položek tabulky jmen. */
#define NAMES_COUNT( t ) ( ( int ) ( sizeof ( t ) / sizeof ( ( t )[0] ) ) )

/** @brief Jména strojů pro klíč machine (index = en_TMZ_MACHINE). */
static const char *const g_machine_names[] = { "generic", "mz700", "mz800", "mz1500", "mz80b" };
/** @brief Jména formátů LEGACY pro klíč format (index = en_TMZ_LEGACY_FORMAT). */
static const char *const g_legacy_format_names[] = { "normal", "fastipl", "bsd" };
/** @brief Jména pulzních sad pro klíč pulseset (index = en_TMZ_PULSESET). */
static const char *const g_pulseset_names[] = { "700", "800", "80b" };


/**
 * @brief Najde hodnotu v tabulce jmen.
 * @param v Úsek. @param n Délka. @param names Tabulka. @param count Počet jmen.
 * @return Index, nebo -1.
 */
static int name_index ( const char *v, size_t n, const char *const *names, int count ) {
    for ( int i = 0; i < count; i++ ) {
        if ( span_eq ( v, n, names[i] ) ) return i;
    }
    return -1;
}


/**
 * @brief Připojí ",machine=..." k textu PROFILE_ARGS, je-li stroj nastaven.
 * @param buf Text ukončený nulou. @param size Velikost bufferu. @param a Parametry.
 */
static void append_machine ( char *buf, size_t size, const st_TMZ_PROFILE_ARGS *a ) {
    if ( a->machine < 0 || a->machine >= NAMES_COUNT ( g_machine_names ) ) return;
    size_t len = strlen ( buf );
    snprintf ( buf + len, size - len, ",machine=%s", g_machine_names[a->machine] );
}


int tmz_profile_args_parse ( const char *text, size_t length, st_TMZ_PROFILE_ARGS *args ) {
    if ( !args || ( length && !text ) ) return EXIT_FAILURE;
    tmz_profile_args_init ( args );
    size_t pos = 0;
    while ( pos < length ) {
        size_t end = pos;
        while ( end < length && text[end] != ',' ) end++;
        const char *item = text + pos;
        size_t ilen = end - pos;
        const char *eq = memchr ( item, '=', ilen );
        if ( !eq ) return EXIT_FAILURE;
        size_t klen = ( size_t ) ( eq - item );
        const char *val = eq + 1;
        size_t vlen = ilen - klen - 1;
        if ( span_eq ( item, klen, "speed" ) ) {
            /* zápis a:b (registr) i a/b (příklad ve spec 8.2) */
            char ratio[16];
            if ( vlen == 0 || vlen >= sizeof ( ratio ) ) return EXIT_FAILURE;
            for ( size_t i = 0; i < vlen; i++ ) ratio[i] = ( val[i] == '/' ) ? ':' : val[i];
            en_CMTSPEED found = CMTSPEED_NONE;
            for ( int s = CMTSPEED_1_1; s < CMTSPEED_CUSTOM && found == CMTSPEED_NONE; s++ ) {
                /* text beze změny: "2:1 (cp/m)" obsahuje '/', který převod na ':' rozbije */
                const char *rt = cmtspeed_get_ratiotxt ( ( en_CMTSPEED ) s );
                if ( span_eq ( val, vlen, rt ) || span_eq ( ratio, vlen, rt ) ) found = ( en_CMTSPEED ) s;
            }
            if ( found != CMTSPEED_NONE ) {
                args->speed = found;
            } else {
                /* poměr TurboCopy: jedna číslice 1..9 : jedna číslice 1..9, 1/2 <= a/b <= 3 */
                if ( vlen != 3 || ratio[1] != ':' || ratio[0] < '1' || ratio[0] > '9' || ratio[2] < '1' || ratio[2] > '9' ) {
                    return EXIT_FAILURE;
                }
                unsigned ta = ( unsigned ) ( ratio[0] - '0' ), tb = ( unsigned ) ( ratio[2] - '0' );
                if ( 2 * ta < tb || ta > 3 * tb ) return EXIT_FAILURE;
                args->tc_a = ( uint8_t ) ta;
                args->tc_b = ( uint8_t ) tb;
            }
        } else if ( span_eq ( item, klen, "loader" ) ) {
            if ( vlen != 1 || ( val[0] != 'Y' && val[0] != 'N' && val[0] != 'y' && val[0] != 'n' ) ) return EXIT_FAILURE;
            args->loader = ( val[0] == 'Y' || val[0] == 'y' );
        } else if ( span_eq ( item, klen, "gap" ) ) {
            if ( span_eq ( val, vlen, "rom" ) ) args->gap_rom = 1;
            else if ( span_eq ( val, vlen, "short" ) ) args->gap_rom = 0;
            else return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "pause" ) ) {
            if ( parse_u32 ( val, vlen, 999999999u, &args->pause_ms ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "machine" ) ) {
            args->machine = name_index ( val, vlen, g_machine_names, NAMES_COUNT ( g_machine_names ) );
            if ( args->machine < 0 ) return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "format" ) ) {
            args->legacy_format = name_index ( val, vlen, g_legacy_format_names, NAMES_COUNT ( g_legacy_format_names ) );
            if ( args->legacy_format < 0 ) return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "pulseset" ) ) {
            args->pulseset = name_index ( val, vlen, g_pulseset_names, NAMES_COUNT ( g_pulseset_names ) );
            if ( args->pulseset < 0 ) return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "lgap" ) ) {
            if ( parse_u32 ( val, vlen, 1000000u, &args->lgap ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "sgap" ) ) {
            if ( parse_u32 ( val, vlen, 1000000u, &args->sgap ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "pulses" ) ) {
            /* LONG H/LONG L/SHORT H/SHORT L v 0,1 µs, 0 = pole ze sady */
            if ( parse_quad ( val, vlen, 0, args->pulses ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        } else if ( span_eq ( item, klen, "bd" ) ) {
            uint32_t v;
            /* rychlosti Intercopy NORMAL/TURBO/FASTIPL (g_ic_speed) a navíc CPM 1500, 3600 (g_ic_cpm);
             * zda rychlost profil zná, ověří až jeho sestavení */
            if ( parse_u32 ( val, vlen, 3600u, &v ) != EXIT_SUCCESS
                 || !( ic_speed_row ( ( uint16_t ) v ) || v == 1500 || v == 3600 ) ) return EXIT_FAILURE;
            args->bd = ( uint16_t ) v;
        } else if ( span_eq ( item, klen, "cmtspeed" ) ) {
            /* tytéž půlvlny v µs (hlavička CMTSPEED UniCMT), nula není dovolena */
            if ( parse_quad ( val, vlen, 1, args->cmtspeed ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        } else {
            return EXIT_FAILURE;
        }
        pos = ( end < length ) ? end + 1 : end;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Převede pauzu v ms na jednotky časové základny.
 * @param ms Milisekundy. @param timebase Hz. @param[out] out Výsledek.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE při přetečení uint32.
 */
static int pause_units ( uint32_t ms, uint32_t timebase, uint32_t *out ) {
    uint64_t v = ( ( uint64_t ) ms * timebase + 500 ) / 1000;
    if ( v > UINT32_MAX ) return EXIT_FAILURE;
    *out = ( uint32_t ) v;
    return EXIT_SUCCESS;
}


/**
 * @brief Parametry části Intercopy V10.2 (T-stavy, pauza HIGH).
 * @param p Výstup. @param s Rychlost. @param tone Počet SHORT tónu.
 * @param tm Počet L a S tapemarku. @param pause Pauza za částí (T).
 */
static void ic_params ( st_MZCMT_FM_PARAMS *p, const st_IC_SPEED *s, uint32_t tone, uint16_t tm, uint32_t pause ) {
    mzcmt_fm_params_init ( p );
    p->timebase = CPU_TIMEBASE;
    p->short_first = s->sh;
    p->short_second = s->sl;
    p->long_first = s->lh;
    p->long_second = s->ll;
    p->tone = tone;
    p->tm_long = tm;
    p->tm_short = tm;
    p->tm_end_long = 1;
    p->checksum = MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE;
    p->end_long = 1;
    p->pause = pause;
    p->pause_level = 1;   /* Intercopy drží mezi bloky PC1 = HIGH (změřeno v emulátoru) */
}


/**
 * @brief Sestaví části profilů IC-10.2-NORMAL, -TURBO a -FASTIPL.
 *
 * @param b Seznam částí.
 * @param prof_index PROF_IC_NORMAL, PROF_IC_TURBO nebo PROF_IC_FASTIPL.
 * @param a Parametry (bd, pause).
 * @param header Hlavička v pořadí hostitele.
 * @param body_size Délka těla.
 * @param pre_hdr Buffer 128 B pro inline hlavičku (žije do vytvoření bloku).
 * @param pre_body Buffer pro tělo předzavaděče TURBO (žije do vytvoření bloku).
 * @param p Pracovní parametry FM.
 * @param[out] text PROFILE_ARGS. @param text_size Velikost bufferu.
 * @param[out] err TMZ_PROFILE_ERROR_ARGS při neplatných parametrech.
 * @return 1 = části přidány, 0 = chyba.
 */
static int ic_build ( st_BUILD *b, int prof_index, const st_TMZ_PROFILE_ARGS *a, const st_MZF_HEADER *header,
                      uint32_t body_size, uint8_t pre_hdr[128], uint8_t pre_body[MZCMT_TURBO_PRELOADER_SIZE],
                      st_MZCMT_FM_PARAMS *p, char *text, size_t text_size, en_TMZ_PROFILE_ERROR *err ) {
    const st_IC_SPEED *s = ic_speed_row ( a->bd ? a->bd : 1200 );
    const st_IC_SPEED *s1200 = ic_speed_row ( 1200 );
    uint32_t pause_last = 0;
    if ( !s || a->speed != CMTSPEED_NONE || pause_units ( a->pause_ms, CPU_TIMEBASE, &pause_last ) != EXIT_SUCCESS ) {
        *err = TMZ_PROFILE_ERROR_ARGS;
        return 0;
    }
    st_TMZ_PART_COMMON c_rom = { 0, 1, 0, ROM_N };
    st_TMZ_PART_COMMON none = { 0, 0, 0, 0 };
    if ( prof_index == PROF_IC_NORMAL ) {
        /* ROM přečte jen 1200 (popisek 1200 = časování ROM, disassembly Intercopy 10.2) */
        const st_TMZ_PART_COMMON *c = ( s->bd == 1200 ) ? &c_rom : &none;
        ic_params ( p, s, 11000, 40, IC_PAUSE_T );
        if ( add_part ( b, TMZ_PART_ROLE_HEADER, TMZ_SOURCE_MZF_HEADER, NULL, 0, 0, 0, p, c ) != EXIT_SUCCESS ) return 0;
        ic_params ( p, s, 5500, 20, pause_last );
        if ( add_part ( b, TMZ_PART_ROLE_BODY, TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, c ) != EXIT_SUCCESS ) return 0;
    } else if ( prof_index == PROF_IC_TURBO ) {
        /* předzavaděč TurboCopy: bajty shodné s TurboCopy kromě čtecího bodu rp+5 (ověřeno v emulátoru) */
        if ( mzcmt_turbo_build_preloader ( header, body_size, ( uint8_t ) ( s->rp + 5 ), pre_hdr, pre_body ) != EXIT_SUCCESS ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            return 0;
        }
        st_TMZ_PART_COMMON c_body = { 0, 1, 1, ( uint16_t ) ( s->rp + 5 ) };
        ic_params ( p, s1200, 11000, 40, IC_PAUSE_T );
        if ( add_part ( b, TMZ_PART_ROLE_LOADER_HEADER, TMZ_SOURCE_INLINE, pre_hdr, 128, 0, 0, p, &c_rom ) != EXIT_SUCCESS ) return 0;
        ic_params ( p, s1200, 5500, 20, IC_PAUSE_T );
        if ( add_part ( b, TMZ_PART_ROLE_LOADER, TMZ_SOURCE_INLINE, pre_body, MZCMT_TURBO_PRELOADER_SIZE, 0, 0, p, &c_rom )
             != EXIT_SUCCESS ) return 0;
        ic_params ( p, s, 5500, 20, pause_last );
        if ( add_part ( b, TMZ_PART_ROLE_BODY, TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, &c_body ) != EXIT_SUCCESS ) return 0;
    } else {
        /* hlavička $BB se zavaděčem V07, čtecí bod rp z tabulky Intercopy, block mode 1 */
        st_MZCMT_FASTIPL_CONFIG f;
        memset ( &f, 0, sizeof ( f ) );
        f.version = MZCMT_FASTIPL_VERSION_V07;
        f.pulseset = MZCMT_FASTIPL_PULSESET_800;
        f.speed = CMTSPEED_1_1;
        f.readpoint = s->rp;
        f.blcount = 1;
        mzcmt_fastipl_build_header ( pre_hdr, header, &f );
        st_TMZ_PART_COMMON c_body = { 0, 1, 2, s->rp };
        ic_params ( p, s1200, 11000, 40, IC_PAUSE_T );
        if ( add_part ( b, TMZ_PART_ROLE_LOADER_HEADER, TMZ_SOURCE_INLINE, pre_hdr, 128, 0, 0, p, &c_rom ) != EXIT_SUCCESS ) return 0;
        ic_params ( p, s, 5500, 20, pause_last );
        if ( add_part ( b, TMZ_PART_ROLE_BODY, TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, &c_body ) != EXIT_SUCCESS ) return 0;
    }
    snprintf ( text, text_size, "bd=%u,pause=%u", s->bd, a->pause_ms );
    return 1;
}


/**
 * @brief Přidá část MANCHESTER.
 * @param b Seznam. @param role Role. @param source Zdroj dat (MZF_HEADER nebo MZF_BODY).
 * @param p Parametry MANCHESTER.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE (plný seznam, chyba kódování).
 */
static int add_part_manchester ( st_BUILD *b, uint8_t role, uint8_t source, const st_MZCMT_MANCH_PARAMS *p ) {
    if ( b->n >= b->cap ) return EXIT_FAILURE;
    st_TMZ_TLVBUF *t = &b->tlv[b->n];
    tmz_tlvbuf_init ( t );
    b->n++;   /* buffer patří do seznamu i při chybě, aby ho uvolnil build_free() */
    if ( tmz_part_manchester_encode ( p, NULL, 0, t ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    st_TMZ_PART *pt = &b->parts[b->n - 1];
    memset ( pt, 0, sizeof ( *pt ) );
    pt->role = role;
    pt->family = TMZ_FAMILY_MANCHESTER;
    pt->source = source;
    pt->tlv = t->data;
    pt->tlv_length = t->length;
    return EXIT_SUCCESS;
}


/**
 * @brief Přidá položku kontextu MANCHESTER.
 * @param p Parametry. @param ctx Kontext. @param bit Bit (0, 1, MZCMT_MANCH_BIT_ANY). @param half2 Druhá půlka.
 */
static void manch_ctx ( st_MZCMT_MANCH_PARAMS *p, uint8_t ctx, uint8_t bit, uint32_t half2 ) {
    p->context[p->context_count].ctx = ctx;
    p->context[p->context_count].bit = bit;
    p->context[p->context_count].half2 = half2;
    p->context_count++;
}


/** @brief Pezik: nejdelší tělo (B); TAPE.COM delší soubor dělí na díly po 16 KB s vlastní hlavičkou CP/M. */
#define PEZIK_MAX_BODY 16384u
/**
 * @brief Intercopy V10.2 CPM: rychlost, základní půlka bitu a pauza hlavička -> tělo.
 *
 * Změřeno v emulátoru (záznam hran PC1, T = takt CPU MZ-800), jeden běh na rychlost:
 * - half: h1 všech bitů a h2 mimo kontexty (ic_cpm_params), = 741 + 13 * (K - 39h)
 *   pro konstantu smyčky K z tabulky programu (5Bh, 39h, 31h, 2Bh, 26h);
 * - pause: od 2. půlky koncového bitu hlavičky (ta je součástí části) do prvního
 *   zápisu těla, bez 1. půlky pilotního bitu těla, úroveň LOW.
 */
static const struct {
    uint16_t bd;        /**< rychlost v Bd, jak ji nabízí Intercopy */
    uint16_t half;      /**< základní půlka bitu v T */
    uint32_t pause;     /**< pauza za hlavičkou v T (LOW) */
} g_ic_cpm[] = {
    { 1500, 1183, 1221586u },
    { 2400,  741, 1221688u },
    { 2800,  637, 1225118u },
    { 3200,  559, 1225220u },
    { 3600,  494, 1225223u },
};


/**
 * @brief Parametry části Pezik/MarVan TAPE.COM V2.0a (výpočet z T-stavů kódu programu).
 *
 * Půlky jsou 13N + c (N = 43 pro 2400, 28 pro 3200); druhá půlka bitu 1
 * je o 1 T kratší než bitu 0 (konec rutiny write_bit). Hlavička končí
 * přechodem rovnou do pilotu těla (CK_END), tělo končí bez koncového bitu.
 *
 * @param p Výstup. @param n N. @param header 1 = hlavička (flag 00h), 0 = tělo (flag 01h).
 * @param pause Pauza za částí (T).
 */
static void pezik_params ( st_MZCMT_MANCH_PARAMS *p, uint32_t n, int header, uint32_t pause ) {
    static const struct { uint8_t ctx; uint16_t c; } k[] = {
        { MZCMT_MANCH_CTX_PILOT_IN, 172 }, { MZCMT_MANCH_CTX_PILOT_NEXT, 189 }, { MZCMT_MANCH_CTX_PILOT_END, 180 },
        { MZCMT_MANCH_CTX_SYNC_END, 206 }, { MZCMT_MANCH_CTX_DATA_FIRST, 186 }, { MZCMT_MANCH_CTX_DATA_BYTE, 184 },
        { MZCMT_MANCH_CTX_DATA_END, 201 }, { MZCMT_MANCH_CTX_DATA_SINGLE_END, 213 }, { MZCMT_MANCH_CTX_CK_MID, 202 },
    };
    mzcmt_manch_params_init ( p );
    p->timebase = CPU_TIMEBASE;
    p->half1[0] = 13 * n + 195;
    p->half1[1] = 13 * n + 196;
    p->half2[0] = 13 * n + 192;
    p->half2[1] = 13 * n + 191;
    p->pilot_count = header ? 2134 : 1067;
    p->pilot_bits = 3;
    p->pilot_pattern = 0x06;   /* 0, 1, 1 */
    p->sync_bits = 1;
    p->sync_pattern = 0x01;
    p->has_flag = 1;
    p->flag = header ? 0x00 : 0x01;
    p->checksum = MZCMT_MANCH_CHECKSUM_SUM16_LE;
    p->carry_adjust = -1;
    p->pause = pause;
    for ( size_t i = 0; i < sizeof ( k ) / sizeof ( k[0] ); i++ ) {
        manch_ctx ( p, k[i].ctx, 0, 13 * n + k[i].c );
        manch_ctx ( p, k[i].ctx, 1, 13 * n + k[i].c - 1 );
    }
    uint32_t flag_end = header ? 231 : 228;
    manch_ctx ( p, MZCMT_MANCH_CTX_FLAG_END, 0, 13 * n + flag_end );
    manch_ctx ( p, MZCMT_MANCH_CTX_FLAG_END, 1, 13 * n + flag_end - 1 );
    if ( header ) {
        manch_ctx ( p, MZCMT_MANCH_CTX_CK_END, 0, 13 * n + 314 );
        manch_ctx ( p, MZCMT_MANCH_CTX_CK_END, 1, 13 * n + 313 );
    }
}


/**
 * @brief Parametry části Intercopy V10.2 "CPM" (změřeno v emulátoru).
 *
 * Všechny půlky bitu mají délku h (g_ic_cpm), kromě druhých půlek v kontextech
 * s pevnou režií kódu: konec pilotu h-1, konec sync h-1 (jen tělo), bit 7 flagu h-1,
 * bit 7 posledního bajtu dat h-2, nižší bajt checksumu h+3, vyšší h+4. Odchylky
 * jsou u všech pěti rychlostí stejné (změřeno). Odchylky -1/-2 T uvnitř těla při
 * (BC mod 256) = 0 se negenerují (v toleranci porovnání se záznamem).
 *
 * @param p Výstup. @param header 1 = hlavička (flag 00h), 0 = tělo (flag 01h).
 * @param pause Pauza za částí (T, LOW). @param h Základní půlka bitu (T), > 4.
 */
static void ic_cpm_params ( st_MZCMT_MANCH_PARAMS *p, int header, uint32_t pause, uint32_t h ) {
    mzcmt_manch_params_init ( p );
    p->timebase = CPU_TIMEBASE;
    p->half1[0] = p->half1[1] = p->half2[0] = p->half2[1] = h;
    p->pilot_count = header ? 2048 : 1024;
    p->pilot_bits = 3;
    p->pilot_pattern = 0x06;
    p->sync_bits = 1;
    p->sync_pattern = 0x01;
    p->has_flag = 1;
    p->flag = header ? 0x00 : 0x01;
    p->checksum = MZCMT_MANCH_CHECKSUM_SUM16_LE;
    p->end_bits = 1;
    p->end_pattern = 0x01;
    p->pause = pause;
    manch_ctx ( p, MZCMT_MANCH_CTX_PILOT_END, MZCMT_MANCH_BIT_ANY, h - 1 );
    if ( !header ) manch_ctx ( p, MZCMT_MANCH_CTX_SYNC_END, MZCMT_MANCH_BIT_ANY, h - 1 );
    manch_ctx ( p, MZCMT_MANCH_CTX_FLAG_END, MZCMT_MANCH_BIT_ANY, h - 1 );
    manch_ctx ( p, MZCMT_MANCH_CTX_DATA_END, MZCMT_MANCH_BIT_ANY, h - 2 );
    manch_ctx ( p, MZCMT_MANCH_CTX_DATA_SINGLE_END, MZCMT_MANCH_BIT_ANY, h - 2 );
    manch_ctx ( p, MZCMT_MANCH_CTX_CK_MID, MZCMT_MANCH_BIT_ANY, h + 3 );
    manch_ctx ( p, MZCMT_MANCH_CTX_CK_END, MZCMT_MANCH_BIT_ANY, h + 4 );
}


/**
 * @brief Sestaví části profilů PEZIK-2.0A a IC-10.2-CPM.
 *
 * Hlavička (MZF_HEADER) a tělo (MZF_BODY, jen při body_size > 0), rodina
 * MANCHESTER. Pezik: bd 2400 (N = 43) nebo 3200 (N = 28), tělo nejvýš
 * PEZIK_MAX_BODY. Intercopy: bd 1500, 2400, 2800, 3200 nebo 3600 (g_ic_cpm),
 * pauza za hlavičkou podle rychlosti.
 * Pauza za posledním dílem = a->pause_ms, LOW.
 *
 * @param b Seznam částí. @param prof_index PROF_PEZIK nebo PROF_IC_CPM. @param a Parametry (bd, pause).
 * @param body_size Délka těla. @param[out] text PROFILE_ARGS. @param text_size Velikost bufferu.
 * @param[out] err TMZ_PROFILE_ERROR_ARGS (rychlost), TMZ_PROFILE_ERROR_BUILD (Pezik tělo nad 16 KB).
 * @return 1 = části přidány, 0 = chyba.
 */
static int cpm_build ( st_BUILD *b, int prof_index, const st_TMZ_PROFILE_ARGS *a, uint32_t body_size,
                       char *text, size_t text_size, en_TMZ_PROFILE_ERROR *err ) {
    int pezik = ( prof_index == PROF_PEZIK );
    uint16_t bd = a->bd ? a->bd : 2400;
    uint32_t pause_last = 0;
    int ic = -1;
    for ( size_t k = 0; !pezik && k < sizeof ( g_ic_cpm ) / sizeof ( g_ic_cpm[0] ); k++ ) {
        if ( g_ic_cpm[k].bd == bd ) ic = ( int ) k;
    }
    if ( a->speed != CMTSPEED_NONE || ( pezik ? !( bd == 2400 || bd == 3200 ) : ic < 0 )
         || pause_units ( a->pause_ms, CPU_TIMEBASE, &pause_last ) != EXIT_SUCCESS ) {
        *err = TMZ_PROFILE_ERROR_ARGS;
        return 0;
    }
    if ( pezik && body_size > PEZIK_MAX_BODY ) {
        fprintf ( stderr, "Error: PEZIK-2.0A: body of %u bytes is longer than 16384 (TAPE.COM writes such a file as "
                  "several 16 KB parts with a CP/M header)\n", body_size );
        *err = TMZ_PROFILE_ERROR_BUILD;
        return 0;
    }
    st_MZCMT_MANCH_PARAMS *p = malloc ( sizeof ( *p ) );
    if ( !p ) return 0;
    uint32_t n = ( bd == 3200 ) ? 28 : 43;
    int ok = 1;
    for ( int part = 0; ok && part < ( body_size ? 2 : 1 ); part++ ) {
        int header = ( part == 0 );
        uint32_t pause = ( header && body_size ) ? ( pezik ? 0 : g_ic_cpm[ic].pause ) : pause_last;
        if ( pezik ) pezik_params ( p, n, header, pause );
        else ic_cpm_params ( p, header, pause, g_ic_cpm[ic].half );
        ok = add_part_manchester ( b, header ? TMZ_PART_ROLE_HEADER : TMZ_PART_ROLE_BODY,
                                   header ? TMZ_SOURCE_MZF_HEADER : TMZ_SOURCE_MZF_BODY, p ) == EXIT_SUCCESS;
    }
    free ( p );
    if ( ok ) snprintf ( text, text_size, "bd=%u,pause=%u", bd, a->pause_ms );
    return ok;
}


/**
 * @brief Parametry části BASIC-1Z016.
 * @param p Výstup.
 * @param tone Počet SHORT tónu (22000 hlavička, 11000 kus).
 * @param tm Počet L a S tapemarku (40 hlavička, 20 kus).
 * @param pause Pauza za částí v T (LOW).
 */
static void basic_params ( st_MZCMT_FM_PARAMS *p, uint32_t tone, uint16_t tm, uint32_t pause ) {
    mzcmt_fm_params_init ( p );
    p->timebase = CPU_TIMEBASE;
    p->short_first = 868;
    p->short_second = 944;
    p->long_first = 1739;
    p->long_second = 1825;
    p->context_count = sizeof ( g_basic_ctx ) / sizeof ( g_basic_ctx[0] );
    memcpy ( p->context, g_basic_ctx, sizeof ( g_basic_ctx ) );
    p->tone = tone;
    p->tm_long = tm;
    p->tm_short = tm;
    p->tm_end_long = 1;
    p->checksum = MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE;
    p->end_long = 1;
    p->copies = 1;
    p->copy_sep = 256;
    p->pause = pause;
}


/**
 * @brief Sestaví části profilu BASIC-1Z016 (spec 6.7).
 *
 * HEADER (tón 22000, tapemark 40/40) a pro každý kus BSD_CHUNK (BODY_SLICE
 * i·258, tón 11000, tapemark 20/20), všechny s kopií. Pauza za hlavičkou
 * a za kusy kromě posledního BASIC_PAUSE_T, za posledním a->pause_ms.
 * Bez READER a STOP. ID kusů se nekontrolují.
 *
 * @param b Seznam částí. @param a Parametry (pause). @param body_size Délka těla.
 * @param p Pracovní parametry FM. @param[out] text PROFILE_ARGS. @param text_size Velikost bufferu.
 * @param[out] err TMZ_PROFILE_ERROR_ARGS (pauza mimo rozsah), jinak beze změny (BUILD: tělo
 *             0 B, ne násobek 258 nebo víc než BASIC_MAX_CHUNKS kusů).
 * @return 1 = části přidány, 0 = chyba.
 */
static int basic_build ( st_BUILD *b, const st_TMZ_PROFILE_ARGS *a, uint32_t body_size, st_MZCMT_FM_PARAMS *p,
                         char *text, size_t text_size, en_TMZ_PROFILE_ERROR *err ) {
    uint32_t pause_last = 0;
    if ( pause_units ( a->pause_ms, CPU_TIMEBASE, &pause_last ) != EXIT_SUCCESS ) {
        *err = TMZ_PROFILE_ERROR_ARGS;
        return 0;
    }
    uint32_t n = body_size / TMZ_BASIC_CHUNK_SIZE;
    if ( body_size == 0 || body_size % TMZ_BASIC_CHUNK_SIZE != 0 || n > BASIC_MAX_CHUNKS ) return 0;
    st_TMZ_PART_COMMON c = { 0, 0, 0, 0 };   /* čtecí rutina BASICu není rozebraná: bez READER */
    basic_params ( p, 22000, 40, BASIC_PAUSE_T );
    if ( add_part ( b, TMZ_PART_ROLE_HEADER, TMZ_SOURCE_MZF_HEADER, NULL, 0, 0, 0, p, &c ) != EXIT_SUCCESS ) return 0;
    for ( uint32_t i = 0; i < n; i++ ) {
        basic_params ( p, 11000, 20, i + 1 == n ? pause_last : BASIC_PAUSE_T );
        if ( add_part ( b, TMZ_PART_ROLE_BSD_CHUNK, TMZ_SOURCE_BODY_SLICE, NULL, 0, i * TMZ_BASIC_CHUNK_SIZE,
                        TMZ_BASIC_CHUNK_SIZE, p, &c ) != EXIT_SUCCESS ) return 0;
    }
    snprintf ( text, text_size, "pause=%u", a->pause_ms );
    return 1;
}


/** @brief Časová základna LEGACY: 0,1 µs (vlastní pulzy bloku 0x41 jsou hodnota/1e7 s). */
#define LEGACY_TIMEBASE 10000000u


/**
 * @brief Pulzní sady formátu normal profilu LEGACY při rychlosti 1:1 (sekundy).
 *
 * Historické sady generátorů v1.0, zachované pro shodné přehrání souborů
 * v1.0; živé cesty používají sady mztape. Hodnoty odpovídají tabulce
 * výchozích pulzních sad specifikace v1.0 (kap. 5.2).
 *
 * Řádek = pulzní sada (index en_MZTAPE_PULSESET: 0 = MZ-700/80K/80A,
 * 1 = MZ-800/1500, 2 = MZ-80B), sloupce LONG H, LONG L, SHORT H, SHORT L.
 * Sady MZ-700 a MZ-800 jsou symetrické (504/504 + 252/252 µs,
 * 498/498 + 249/249 µs); MZ-80B 333/334 + 166,75/166 µs. Hodnoty nemají
 * doložený zdroj [neověřeno]; drží se jen kvůli shodě se soubory v1.0.
 */
static const double g_legacy_pulses[3][4] = {
    { 0.000504, 0.000504, 0.000252, 0.000252 },
    { 0.000498, 0.000498, 0.000249, 0.000249 },
    { 0.000333, 0.000334, 0.000166750, 0.000166 },
};


/* Kontrakt viz tmz_profile.h. */
int tmz_profile_legacy_pulses ( int pulseset, double out[4] ) {
    if ( !out || pulseset < 0 || pulseset >= ( int ) ( sizeof ( g_legacy_pulses ) / sizeof ( g_legacy_pulses[0] ) ) ) {
        return EXIT_FAILURE;
    }
    for ( int i = 0; i < 4; i++ ) out[i] = g_legacy_pulses[pulseset][i];
    return EXIT_SUCCESS;
}


/**
 * @brief Délka půlvlny v jednotkách LEGACY (zaokrouhlení a nejméně 1 jako generátory v1.0).
 * @param sec Délka při rychlosti 1:1 v sekundách.
 * @param divisor Dělitel rychlosti (cmtspeed_get_divisor).
 * @return Jednotky 0,1 µs.
 */
static uint32_t legacy_units ( double sec, double divisor ) {
    double v = round ( sec * LEGACY_TIMEBASE / divisor );
    return v < 1.0 ? 1u : ( uint32_t ) v;
}


/**
 * @brief Parametry části LEGACY.
 *
 * Generátory v1.0 kódují bajt jako 8 bitů a stop bit L, za tapemarkem
 * a za checksumem píšou 2L. V modelu FM to je start bit L před bajtem,
 * 1 L za tapemarkem a 3 L za checksumem (stop bit posledního bajtu + 2L).
 *
 * @param p Výstup.
 * @param pul LONG H, LONG L, SHORT H, SHORT L (jednotky LEGACY).
 * @param tone Počet SHORT tónu.
 * @param tm Počet L a S tapemarku.
 * @param pause Pauza za částí (jednotky LEGACY).
 */
static void legacy_params ( st_MZCMT_FM_PARAMS *p, const uint32_t pul[4], uint32_t tone, uint16_t tm, uint32_t pause ) {
    mzcmt_fm_params_init ( p );
    p->timebase = LEGACY_TIMEBASE;
    p->long_first = pul[0];
    p->long_second = pul[1];
    p->short_first = pul[2];
    p->short_second = pul[3];
    p->tone = tone;
    p->tm_long = tm;
    p->tm_short = tm;
    p->tm_end_long = 1;
    p->checksum = MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE;
    p->end_long = 3;
    p->pause = pause;
}


/**
 * @brief Připojí k textu PROFILE_ARGS ",pulses=a/b/c/d", je-li některé pole nenulové.
 * @param buf Text ukončený nulou. @param size Velikost bufferu. @param a Parametry.
 */
static void append_pulses ( char *buf, size_t size, const st_TMZ_PROFILE_ARGS *a ) {
    if ( !a->pulses[0] && !a->pulses[1] && !a->pulses[2] && !a->pulses[3] ) return;
    size_t len = strlen ( buf );
    snprintf ( buf + len, size - len, ",pulses=%u/%u/%u/%u", a->pulses[0], a->pulses[1], a->pulses[2], a->pulses[3] );
}


/**
 * @brief Sestaví části profilu TAPEMZ-V1-LEGACY (spec 9.2).
 *
 * Formát normal (mztape): HEADER (tón lgap, výchozí 6400, u sady 80B
 * 10000; tapemark 40/40) a BODY (tón sgap, výchozí 11000; tapemark 20/20).
 * Pulzy z historické sady v1.0 (g_legacy_pulses / dělitel rychlosti), nebo
 * všechna čtyři vlastní pole beze změny. Rychlost 2:1 CPM (pulzy cmt.com)
 * a částečně vyplněné vlastní pulzy model nevyjádří.
 *
 * Formát fastipl (mzcmt_fastipl V07): LOADER_HEADER s inline hlavičkou $BB
 * (mzcmt_fastipl_build_header, readpoint podle rychlosti) rychlostí 1:1,
 * tón 11000, tapemark 40/40; BODY rychlostí bloku, tón lgap (výchozí
 * 5500), tapemark 20/20. Vlastní pulzy platí po polích jen pro tělo
 * (0 = ze sady).
 *
 * Formát bsd (mzcmt_bsd, jen 1:1, bez vlastních pulzů): HEADER (tón lgap,
 * výchozí 22000, tapemark 40/40), pak kus po 258 B jako BSD_CHUNK
 * (BODY_SLICE), první s tónem sgap (výchozí 11000), další bez tónu,
 * tapemark 20/20. Tělo musí být násobek 258 B, nejvýš 254 kusů
 * (víc = TMZ_PROFILE_ERROR_BUILD).
 *
 * @param b Seznam částí.
 * @param a Parametry.
 * @param header Hlavička (pořadí hostitele).
 * @param body_size Délka těla.
 * @param bb Buffer pro inline hlavičku (fastipl); musí žít do vytvoření bloku.
 * @param p Pracovní parametry FM.
 * @param[out] text PROFILE_ARGS.
 * @param text_size Velikost bufferu text.
 * @param[out] err TMZ_PROFILE_ERROR_ARGS při neplatných parametrech, jinak beze změny.
 * @return 1 = části přidány, 0 = chyba.
 */
static int legacy_build ( st_BUILD *b, const st_TMZ_PROFILE_ARGS *a, const st_MZF_HEADER *header, uint32_t body_size,
                          uint8_t bb[128], st_MZCMT_FM_PARAMS *p, char *text, size_t text_size,
                          en_TMZ_PROFILE_ERROR *err ) {
    en_CMTSPEED speed = ( a->speed == CMTSPEED_NONE ) ? CMTSPEED_1_1 : a->speed;
    int pulseset = ( a->pulseset < 0 ) ? 1 : a->pulseset;
    uint32_t pause = 0;
    if ( speed == CMTSPEED_2_1_CPM || !cmtspeed_is_valid ( speed ) || pulseset > 2
         || pause_units ( a->pause_ms, LEGACY_TIMEBASE, &pause ) != EXIT_SUCCESS ) {
        *err = TMZ_PROFILE_ERROR_ARGS;
        return 0;
    }
    double divisor = cmtspeed_get_divisor ( speed );
    int any = a->pulses[0] || a->pulses[1] || a->pulses[2] || a->pulses[3];
    int all = a->pulses[0] && a->pulses[1] && a->pulses[2] && a->pulses[3];
    st_TMZ_PART_COMMON c = { 0, 0, 0, 0 };   /* bez STOP a READER (čtecí rutina není známá) */

    if ( a->legacy_format == TMZ_LEGACY_NORMAL ) {
        uint32_t pul[4];
        if ( any && !all ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            return 0;
        }
        /* pulseset je zde 0-2 (kontrola výše) */
        for ( int i = 0; i < 4; i++ ) pul[i] = all ? a->pulses[i] : legacy_units ( g_legacy_pulses[pulseset][i], divisor );
        uint32_t lgap = a->lgap ? a->lgap : ( pulseset == 2 ? 10000u : 6400u );
        uint32_t sgap = a->sgap ? a->sgap : 11000u;
        legacy_params ( p, pul, lgap, 40, 0 );
        if ( add_part ( b, TMZ_PART_ROLE_HEADER, TMZ_SOURCE_MZF_HEADER, NULL, 0, 0, 0, p, &c ) != EXIT_SUCCESS ) return 0;
        legacy_params ( p, pul, sgap, 20, pause );
        if ( add_part ( b, TMZ_PART_ROLE_BODY, TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, &c ) != EXIT_SUCCESS ) return 0;
        snprintf ( text, text_size, "format=normal,pulseset=%s,speed=%s,lgap=%u,sgap=%u", g_pulseset_names[pulseset],
                   cmtspeed_get_ratiotxt ( speed ), lgap, sgap );
        append_pulses ( text, text_size, a );
        size_t len = strlen ( text );
        snprintf ( text + len, text_size - len, ",pause=%u", a->pause_ms );
        return 1;
    }

    if ( a->legacy_format == TMZ_LEGACY_FASTIPL ) {
        double s[4];
        uint32_t hdr_pul[4], body_pul[4];
        if ( mzcmt_fastipl_get_pulse_lengths ( ( en_MZCMT_FASTIPL_PULSESET ) pulseset, s ) != EXIT_SUCCESS ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            return 0;
        }
        /* hlavička $BB vždy 1:1 ze sady, tělo rychlostí bloku; vlastní pulzy po polích jen pro tělo */
        for ( int i = 0; i < 4; i++ ) {
            hdr_pul[i] = legacy_units ( s[i], 1.0 );
            body_pul[i] = a->pulses[i] ? a->pulses[i] : legacy_units ( s[i], divisor );
        }
        st_MZCMT_FASTIPL_CONFIG f;
        memset ( &f, 0, sizeof ( f ) );
        f.version = MZCMT_FASTIPL_VERSION_V07;
        f.pulseset = ( en_MZCMT_FASTIPL_PULSESET ) pulseset;
        f.speed = speed;
        mzcmt_fastipl_build_header ( bb, header, &f );
        uint32_t lgap = a->lgap ? a->lgap : MZCMT_FASTIPL_BODY_LGAP_DEFAULT;
        legacy_params ( p, hdr_pul, MZCMT_FASTIPL_HEADER_LGAP_DEFAULT, MZCMT_FASTIPL_LTM_LONG, 0 );
        if ( add_part ( b, TMZ_PART_ROLE_LOADER_HEADER, TMZ_SOURCE_INLINE, bb, 128, 0, 0, p, &c ) != EXIT_SUCCESS ) return 0;
        legacy_params ( p, body_pul, lgap, MZCMT_FASTIPL_STM_LONG, pause );
        if ( add_part ( b, TMZ_PART_ROLE_BODY, TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, &c ) != EXIT_SUCCESS ) return 0;
        snprintf ( text, text_size, "format=fastipl,pulseset=%s,speed=%s,lgap=%u", g_pulseset_names[pulseset],
                   cmtspeed_get_ratiotxt ( speed ), lgap );
        append_pulses ( text, text_size, a );
        size_t len = strlen ( text );
        snprintf ( text + len, text_size - len, ",pause=%u", a->pause_ms );
        return 1;
    }

    if ( a->legacy_format == TMZ_LEGACY_BSD ) {
        double s[4];
        uint32_t pul[4];
        if ( any || speed != CMTSPEED_1_1 || body_size == 0 || body_size % TMZ_BASIC_CHUNK_SIZE != 0
             || mzcmt_bsd_get_pulse_lengths ( ( en_MZCMT_BSD_PULSESET ) pulseset, s ) != EXIT_SUCCESS ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            return 0;
        }
        uint32_t n = body_size / TMZ_BASIC_CHUNK_SIZE;
        if ( n > MAX_PARTS - 1 ) return 0;   /* *err zůstává BUILD: blok 0x46 má nejvýš 255 částí */
        for ( int i = 0; i < 4; i++ ) pul[i] = legacy_units ( s[i], divisor );
        uint32_t lgap = a->lgap ? a->lgap : 22000u;
        uint32_t sgap = a->sgap ? a->sgap : 11000u;
        legacy_params ( p, pul, lgap, 40, 0 );
        if ( add_part ( b, TMZ_PART_ROLE_HEADER, TMZ_SOURCE_MZF_HEADER, NULL, 0, 0, 0, p, &c ) != EXIT_SUCCESS ) return 0;
        /* kusy jdou za sebou bez tónu; SGAP jen před prvním (mzcmt_bsd) */
        for ( uint32_t i = 0; i < n; i++ ) {
            legacy_params ( p, pul, i == 0 ? sgap : 0, 20, i + 1 == n ? pause : 0 );
            if ( add_part ( b, TMZ_PART_ROLE_BSD_CHUNK, TMZ_SOURCE_BODY_SLICE, NULL, 0, i * TMZ_BASIC_CHUNK_SIZE,
                            TMZ_BASIC_CHUNK_SIZE, p, &c ) != EXIT_SUCCESS ) return 0;
        }
        snprintf ( text, text_size, "format=bsd,pulseset=%s,lgap=%u,sgap=%u,pause=%u", g_pulseset_names[pulseset],
                   lgap, sgap, a->pause_ms );
        return 1;
    }
    *err = TMZ_PROFILE_ERROR_ARGS;
    return 0;
}


/** @brief Časová základna MZ800EMU-CMT: takt GDG MZ-800 v Hz (vzorkovací frekvence pásky v emulátoru MZ-800). */
#define EMU_CMT_TIMEBASE 17721600u


/**
 * @brief Délka půlvlny v tikách EMU_CMT_TIMEBASE.
 *
 * Stejný výpočet jako počet vzorků v mztape při frekvenci EMU_CMT_TIMEBASE
 * (round(sekundy * frekvence)), takže blok přehraný při této frekvenci dá
 * tytéž běhy jako mztape.
 *
 * @param sec Délka v sekundách (> 0).
 * @return Tiky; 0, když délka po zaokrouhlení nedá ani jeden tik nebo
 *         přeteče uint32 (volající to hlásí jako chybu parametrů).
 */
static uint32_t emu_cmt_ticks ( double sec ) {
    double v = round ( sec * EMU_CMT_TIMEBASE );
    return ( v >= 1.0 && v <= ( double ) UINT32_MAX ) ? ( uint32_t ) v : 0u;
}


/**
 * @brief Sestaví části profilu MZ800EMU-CMT (spec 8.4).
 *
 * Popíše záznam mztape formátem MZTAPE_FORMATSET_MZ800_SANE:
 * tón lgap, tapemark MZTAPE_LTM_*, 2 L, hlavička, checksum, 2 L, tón sgap,
 * tapemark MZTAPE_STM_*, 2 L, tělo, checksum, 2 L; za každým bajtem stop
 * bit L. V modelu FM je to start bit L před bajtem, 1 L za tapemarkem a 3 L
 * za checksumem (stop bit posledního bajtu + 2 L), stejně jako u
 * TAPEMZ-V1-LEGACY normal. Tóny a lgap/sgap jdou z mztape_get_format(),
 * počty pulzů tapemarku z maker MZTAPE_LTM_* a MZTAPE_STM_*.
 *
 * PULSES: a->speed (NONE = 1:1) přes mztape_get_speed_pulses() (u 2:1 CPM
 * sada cmt.com), nebo všechna čtyři pole a->cmtspeed v µs (jen se speed
 * NONE) přes mztape_pulses_set_us(); obojí na tiky emu_cmt_ticks().
 * Pauza za hlavičkou 0 (mztape pokračuje tónem těla hned), za posledním
 * dílem a->pause_ms, LOW. Část BODY vzniká vždy, i pro tělo 0 B (tón,
 * tapemark, checksum 0000 a závěr jako v mztape). Bez kopií, READER
 * a STOP (signál nepochází z programu s rozebranou čtecí rutinou).
 *
 * @param b Seznam částí.
 * @param a Parametry (speed, cmtspeed, pause_ms).
 * @param p Pracovní parametry FM.
 * @param[out] text PROFILE_ARGS ("speed=a:b,pause=N" nebo "cmtspeed=LH/LL/SH/SL,pause=N").
 * @param text_size Velikost bufferu text.
 * @param[out] err TMZ_PROFILE_ERROR_ARGS při neplatných parametrech, jinak beze změny.
 * @return 1 = části přidány, 0 = chyba.
 */
static int emu_cmt_build ( st_BUILD *b, const st_TMZ_PROFILE_ARGS *a, st_MZCMT_FM_PARAMS *p, char *text,
                           size_t text_size, en_TMZ_PROFILE_ERROR *err ) {
    const st_MZTAPE_FORMAT *fmt = mztape_get_format ( MZTAPE_FORMATSET_MZ800_SANE );
    int any = a->cmtspeed[0] || a->cmtspeed[1] || a->cmtspeed[2] || a->cmtspeed[3];
    int all = a->cmtspeed[0] && a->cmtspeed[1] && a->cmtspeed[2] && a->cmtspeed[3];
    en_CMTSPEED speed = ( a->speed == CMTSPEED_NONE ) ? CMTSPEED_1_1 : a->speed;
    st_MZTAPE_PULSES_LENGTH pl;
    uint32_t pause_last = 0;
    int ok = fmt != NULL;
    if ( ok && any ) {
        ok = all && a->speed == CMTSPEED_NONE
             && mztape_pulses_set_us ( &pl, a->cmtspeed[0], a->cmtspeed[1], a->cmtspeed[2], a->cmtspeed[3] ) == EXIT_SUCCESS;
    } else if ( ok ) {
        ok = mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, speed, &pl ) == EXIT_SUCCESS;
    }
    uint32_t pul[4] = { 0, 0, 0, 0 };
    if ( ok ) {
        pul[0] = emu_cmt_ticks ( pl.long_pulse.high );
        pul[1] = emu_cmt_ticks ( pl.long_pulse.low );
        pul[2] = emu_cmt_ticks ( pl.short_pulse.high );
        pul[3] = emu_cmt_ticks ( pl.short_pulse.low );
        ok = pul[0] && pul[1] && pul[2] && pul[3] && pause_units ( a->pause_ms, EMU_CMT_TIMEBASE, &pause_last ) == EXIT_SUCCESS;
    }
    if ( !ok ) {
        *err = TMZ_PROFILE_ERROR_ARGS;
        return 0;
    }
    st_TMZ_PART_COMMON c = { 0, 0, 0, 0 };   /* bez STOP a READER */
    for ( int part = 0; part < 2; part++ ) {
        int header = ( part == 0 );
        mzcmt_fm_params_init ( p );
        p->timebase = EMU_CMT_TIMEBASE;
        p->long_first = pul[0];
        p->long_second = pul[1];
        p->short_first = pul[2];
        p->short_second = pul[3];
        p->tone = header ? fmt->lgap : fmt->sgap;
        p->tm_long = header ? MZTAPE_LTM_LLENGTH : MZTAPE_STM_LLENGTH;
        p->tm_short = header ? MZTAPE_LTM_SLENGTH : MZTAPE_STM_SLENGTH;
        p->tm_end_long = 1;
        p->checksum = MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE;
        p->end_long = 3;
        p->pause = header ? 0 : pause_last;
        if ( add_part ( b, header ? TMZ_PART_ROLE_HEADER : TMZ_PART_ROLE_BODY,
                        header ? TMZ_SOURCE_MZF_HEADER : TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, &c ) != EXIT_SUCCESS ) return 0;
    }
    if ( any ) {
        snprintf ( text, text_size, "cmtspeed=%u/%u/%u/%u,pause=%u", a->cmtspeed[0], a->cmtspeed[1], a->cmtspeed[2],
                   a->cmtspeed[3], a->pause_ms );
    } else {
        snprintf ( text, text_size, "speed=%s,pause=%u", cmtspeed_get_ratiotxt ( speed ), a->pause_ms );
    }
    return 1;
}


/** @name Parametry profilu (bity pro args_used_only())
 * @{ */
#define ARG_SPEED    0x001u   /**< speed */
#define ARG_LOADER   0x002u   /**< loader */
#define ARG_GAP      0x004u   /**< gap_rom */
#define ARG_LEGACY   0x008u   /**< legacy_format, pulseset, lgap, sgap, pulses */
#define ARG_CMTSPEED 0x010u   /**< cmtspeed */
#define ARG_BD       0x020u   /**< bd */
#define ARG_TCRATIO  0x040u   /**< tc_a, tc_b (poměr TurboCopy, jen TC-1.21) */
/** @} */


/**
 * @brief Ověří, že parametry mimo výchozí hodnotu profil opravdu používá.
 *
 * Společné všem profilům jsou pause_ms a machine. Ostatní pole musí mít
 * hodnotu z tmz_profile_args_init(), pokud je profil nečte; jinak by je
 * tiše ignoroval a volající by dostal jiný blok, než čekal. Hodnoty
 * používaných polí kontroluje až sestavení profilu.
 *
 * Porovnává se s hodnotou z tmz_profile_args_init(), ne s tím, zda pole
 * volající nastavil: explicitně zadaná výchozí hodnota (např. loader = 1,
 * speed = CMTSPEED_NONE, gap_rom = -1) projde i u profilu, který pole nečte.
 * Totéž platí pro klíč s výchozí hodnotou v PROFILE_ARGS (např. loader=Y
 * u ROM-800-*). Přísnější kontrola podle zadaných voleb je v mzf2tmz.
 *
 * @param prof_index Index profilu v g_profiles.
 * @param a Parametry.
 * @return 1 = v pořádku, 0 = profil dostal parametr, který nepoužívá.
 */
static int args_used_only ( int prof_index, const st_TMZ_PROFILE_ARGS *a ) {
    unsigned allowed;
    switch ( prof_index ) {
        case PROF_NOMINAL:
        case PROF_EXACT: allowed = ARG_SPEED | ARG_GAP; break;
        case PROF_UNICMT: allowed = ARG_SPEED | ARG_GAP | ARG_CMTSPEED; break;   /* speed odmítne sestavení */
        case PROF_TC: allowed = ARG_SPEED | ARG_LOADER | ARG_TCRATIO; break;
        case PROF_LEGACY: allowed = ARG_SPEED | ARG_LEGACY; break;
        case PROF_BASIC: allowed = 0; break;   /* jen pause a machine */
        case PROF_EMU_CMT: allowed = ARG_SPEED | ARG_CMTSPEED; break;   /* speed spolu s cmtspeed odmítne emu_cmt_build() */
        default: allowed = ARG_SPEED | ARG_BD; break;   /* IC-10.2-*, PEZIK-2.0A: speed odmítne ic_build()/cpm_build() */
    }
    static const uint16_t zero4[4] = { 0, 0, 0, 0 };
    unsigned used = 0;
    if ( a->speed != CMTSPEED_NONE ) used |= ARG_SPEED;
    if ( a->tc_a || a->tc_b ) used |= ARG_TCRATIO;
    if ( a->loader != 1 ) used |= ARG_LOADER;
    if ( a->gap_rom != -1 ) used |= ARG_GAP;
    if ( a->legacy_format != TMZ_LEGACY_NONE || a->pulseset != -1 || a->lgap || a->sgap
         || memcmp ( a->pulses, zero4, sizeof ( zero4 ) ) != 0 ) used |= ARG_LEGACY;
    if ( memcmp ( a->cmtspeed, zero4, sizeof ( zero4 ) ) != 0 ) used |= ARG_CMTSPEED;
    if ( a->bd ) used |= ARG_BD;
    return ( used & ~allowed ) == 0;
}


st_TZX_BLOCK* tmz_profile_build_block ( const char *id, const st_TMZ_PROFILE_ARGS *args,
                                        const st_MZF_HEADER *header, const uint8_t *body, uint32_t body_size,
                                        en_TMZ_PROFILE_ERROR *err ) {
    en_TMZ_PROFILE_ERROR local;
    if ( !err ) err = &local;
    const st_TMZ_PROFILE *prof = tmz_profile_find ( id );
    if ( !prof ) {
        *err = TMZ_PROFILE_ERROR_UNKNOWN;
        return NULL;
    }
    if ( !header || ( body_size && !body ) ) {
        *err = TMZ_PROFILE_ERROR_ARGS;
        return NULL;
    }
    if ( body_size > 0xFFFF ) {
        *err = TMZ_PROFILE_ERROR_BUILD;
        return NULL;
    }
    st_TMZ_PROFILE_ARGS a;
    if ( args ) a = *args;
    else tmz_profile_args_init ( &a );
    if ( !args_used_only ( ( int ) ( prof - g_profiles ), &a ) ) {
        *err = TMZ_PROFILE_ERROR_ARGS;
        return NULL;
    }

    /*
     * MZF hlavička v pořadí souboru (LE). Jen převod fsize/fstrt/fexec:
     * mzf_header_items_correction() by navíc přepsal fname.terminator (11h)
     * na 0Dh, ale bajty hlavičky mají jít do bloku tak, jak přišly.
     */
    st_MZF_HEADER le = *header;
    le.fsize = endianity_bswap16_LE ( le.fsize );
    le.fstrt = endianity_bswap16_LE ( le.fstrt );
    le.fexec = endianity_bswap16_LE ( le.fexec );

    st_MZCMT_FM_PARAMS *p = malloc ( sizeof ( st_MZCMT_FM_PARAMS ) );
    if ( !p ) {
        *err = TMZ_PROFILE_ERROR_BUILD;
        return NULL;
    }
    st_BUILD b;
    if ( build_init ( &b, MAX_PARTS ) != EXIT_SUCCESS ) {
        free ( p );
        *err = TMZ_PROFILE_ERROR_BUILD;
        return NULL;
    }
    b.ck_header = a.ck_header;
    b.ck_body = a.ck_body;
    char args_text[192];
    uint8_t pre_hdr[128];
    uint8_t legacy_hdr[128];   /* LEGACY fastipl: hlavička $BB (inline data musí žít do vytvoření bloku) */
    uint8_t pre_body[MZCMT_TURBO_PRELOADER_SIZE];
    *err = TMZ_PROFILE_ERROR_BUILD;
    int ok = 1;

    if ( prof == &g_profiles[PROF_NOMINAL] || prof == &g_profiles[PROF_EXACT] || prof == &g_profiles[PROF_UNICMT] ) {
        /* ROM-800-NOMINAL / ROM-800-EXACT; UNICMT = struktura NOMINAL s pulzy z CMTSPEED (spec 10.2) */
        int exact = ( prof == &g_profiles[PROF_EXACT] );
        int unicmt = ( prof == &g_profiles[PROF_UNICMT] );
        static const uint16_t pulses_1x[4] = { 470, 494, 240, 278 };
        uint16_t us[4];
        int custom = a.cmtspeed[0] || a.cmtspeed[1] || a.cmtspeed[2] || a.cmtspeed[3];
        memcpy ( us, custom ? a.cmtspeed : pulses_1x, sizeof ( us ) );
        if ( unicmt && ( a.speed != CMTSPEED_NONE || ( custom && ( !us[0] || !us[1] || !us[2] || !us[3] ) ) ) ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            ok = 0;
        }
        int is_1x = ( memcmp ( us, pulses_1x, sizeof ( us ) ) == 0 );
        if ( a.speed != CMTSPEED_NONE && a.speed != CMTSPEED_1_1 ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            ok = 0;
        }
        int gap_rom = ( a.gap_rom < 0 ) ? exact : a.gap_rom;
        uint32_t tb = exact ? CPU_TIMEBASE : 1000000u;
        uint32_t pause_body = 0;
        if ( ok && pause_units ( a.pause_ms, tb, &pause_body ) != EXIT_SUCCESS ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            ok = 0;
        }
        st_TMZ_PART_COMMON c = { 0, 1, 0, ROM_N };
        /* UNICMT jinou rychlostí než 1x čte zavaděč programu, který neznáme: bez READER */
        if ( unicmt && !is_1x ) c.has_reader = 0;
        for ( int part = 0; ok && part < 2; part++ ) {
            if ( part == 0 ) rom_params ( p, exact, gap_rom ? 22000 : 11000, 40, exact ? EXACT_PAUSE_HDR_T : NOM_PAUSE_HDR_US );
            else rom_params ( p, exact, gap_rom ? 11000 : 5500, 20, pause_body );
            if ( unicmt ) {
                p->long_first = us[0];
                p->long_second = us[1];
                p->short_first = us[2];
                p->short_second = us[3];
            }
            ok = add_part ( &b, part ? TMZ_PART_ROLE_BODY : TMZ_PART_ROLE_HEADER,
                            part ? TMZ_SOURCE_MZF_BODY : TMZ_SOURCE_MZF_HEADER, NULL, 0, 0, 0, p, &c ) == EXIT_SUCCESS;
        }
        if ( unicmt ) {
            snprintf ( args_text, sizeof ( args_text ), "gap=%s,cmtspeed=%u/%u/%u/%u,pause=%u", gap_rom ? "rom" : "short",
                       us[0], us[1], us[2], us[3], a.pause_ms );
        } else {
            snprintf ( args_text, sizeof ( args_text ), "gap=%s,pause=%u", gap_rom ? "rom" : "short", a.pause_ms );
        }
    } else if ( prof >= &g_profiles[PROF_IC_NORMAL] && prof <= &g_profiles[PROF_IC_FASTIPL] ) {
        args_text[0] = 0;
        ok = ic_build ( &b, ( int ) ( prof - g_profiles ), &a, header, body_size, legacy_hdr, pre_body, p, args_text,
                        sizeof ( args_text ), err );
    } else if ( prof == &g_profiles[PROF_PEZIK] || prof == &g_profiles[PROF_IC_CPM] ) {
        args_text[0] = 0;
        ok = cpm_build ( &b, ( int ) ( prof - g_profiles ), &a, body_size, args_text, sizeof ( args_text ), err );
    } else if ( prof == &g_profiles[PROF_BASIC] ) {
        args_text[0] = 0;
        ok = basic_build ( &b, &a, body_size, p, args_text, sizeof ( args_text ), err );
    } else if ( prof == &g_profiles[PROF_LEGACY] ) {
        args_text[0] = 0;
        ok = legacy_build ( &b, &a, header, body_size, legacy_hdr, p, args_text, sizeof ( args_text ), err );
    } else if ( prof == &g_profiles[PROF_EMU_CMT] ) {
        args_text[0] = 0;
        ok = emu_cmt_build ( &b, &a, p, args_text, sizeof ( args_text ), err );
    } else {
        /* TC-1.21 */
        en_CMTSPEED speed = ( a.speed == CMTSPEED_NONE ) ? CMTSPEED_2_1 : a.speed;
        unsigned ra = 0, rb = 0;
        if ( a.tc_a || a.tc_b ) {
            /* poměr TurboCopy a/b (tc_a, tc_b): číslice 1..9, 1/2 <= a/b <= 3 */
            ra = a.tc_a;
            rb = a.tc_b;
            if ( ra < 1 || ra > 9 || rb < 1 || rb > 9 || 2 * ra < rb || ra > 3 * rb || a.speed != CMTSPEED_NONE ) {
                *err = TMZ_PROFILE_ERROR_ARGS;
                ok = 0;
            }
        } else if ( mzcmt_turbo_speed_to_ratio ( speed, &ra, &rb ) != EXIT_SUCCESS ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            ok = 0;
        }
        unsigned s = ok ? 71 + ( 480 * rb ) / ra : 0;
        uint16_t tone = ok ? tc_tone ( s ) : 0;
        uint32_t pause_last = 0;
        if ( ok && ( !tone || pause_units ( a.pause_ms, TC_TIMEBASE, &pause_last ) != EXIT_SUCCESS ) ) {
            *err = TMZ_PROFILE_ERROR_ARGS;
            ok = 0;
        }
        uint8_t n = ok ? ( uint8_t ) ( ( 82 * rb ) / ra ) : 0;
        if ( ok && a.loader ) {
            st_TMZ_PART_COMMON c_rom = { 1, 1, 0, ROM_N };
            st_TMZ_PART_COMMON c_pre = { 1, 1, 1, n };
            ok = mzcmt_turbo_build_preloader ( header, body_size, n, pre_hdr, pre_body ) == EXIT_SUCCESS;
            if ( ok ) {
                tc_params ( p, TC_S_PRELOADER, tc_tone ( TC_S_PRELOADER ), 40, TC_PAUSE_TICKS );
                ok = add_part ( &b, TMZ_PART_ROLE_LOADER_HEADER, TMZ_SOURCE_INLINE, pre_hdr, sizeof ( pre_hdr ), 0, 0, p, &c_rom ) == EXIT_SUCCESS;
            }
            if ( ok ) {
                tc_params ( p, TC_S_PRELOADER, tc_tone ( TC_S_PRELOADER ), 20, TC_PAUSE_TICKS );
                ok = add_part ( &b, TMZ_PART_ROLE_LOADER, TMZ_SOURCE_INLINE, pre_body, sizeof ( pre_body ), 0, 0, p, &c_rom ) == EXIT_SUCCESS;
            }
            if ( ok ) {
                tc_params ( p, s, tone, 20, pause_last );
                ok = add_part ( &b, TMZ_PART_ROLE_BODY, TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, &c_pre ) == EXIT_SUCCESS;
            }
        } else if ( ok ) {
            st_TMZ_PART_COMMON c_tc = { 1, 0, 0, 0 };   /* čte TurboCopy (adaptivní práh), bez READER */
            tc_params ( p, s, tone, 40, TC_PAUSE_TICKS );
            ok = add_part ( &b, TMZ_PART_ROLE_HEADER, TMZ_SOURCE_MZF_HEADER, NULL, 0, 0, 0, p, &c_tc ) == EXIT_SUCCESS;
            if ( ok ) {
                tc_params ( p, s, tone, 20, pause_last );
                ok = add_part ( &b, TMZ_PART_ROLE_BODY, TMZ_SOURCE_MZF_BODY, NULL, 0, 0, 0, p, &c_tc ) == EXIT_SUCCESS;
            }
        }
        if ( a.tc_a ) {
            snprintf ( args_text, sizeof ( args_text ), "speed=%u:%u,loader=%c,pause=%u", ra, rb,
                       a.loader ? 'Y' : 'N', a.pause_ms );
        } else {
            snprintf ( args_text, sizeof ( args_text ), "speed=%s,loader=%c,pause=%u", cmtspeed_get_ratiotxt ( speed ),
                       a.loader ? 'Y' : 'N', a.pause_ms );
        }
    }
    append_machine ( args_text, sizeof ( args_text ), &a );
    free ( p );

    st_TZX_BLOCK *block = NULL;
    st_TMZ_TLVBUF btlv;
    tmz_tlvbuf_init ( &btlv );
    if ( ok && tmz_tlvbuf_add ( &btlv, TMZ_TAG_PROFILE_ARGS, 0, args_text, ( uint16_t ) strlen ( args_text ) ) == EXIT_SUCCESS ) {
        st_TMZ_MZFILE mf;
        memset ( &mf, 0, sizeof ( mf ) );
        snprintf ( mf.profile_id, sizeof ( mf.profile_id ), "%s", prof->id );
        mf.header = ( const uint8_t* ) &le;
        mf.body = body;
        mf.body_size = body_size;
        mf.block_tlv = btlv.data;
        mf.block_tlv_length = ( uint16_t ) btlv.length;
        mf.part_count = b.n;
        mf.parts = b.parts;
        block = tmz_mzfile_create_block ( &mf, NULL );
        if ( block ) *err = TMZ_PROFILE_OK;
    }
    tmz_tlvbuf_free ( &btlv );
    build_free ( &b );
    return block;
}


const char* tmz_profile_error_string ( en_TMZ_PROFILE_ERROR err ) {
    switch ( err ) {
        case TMZ_PROFILE_OK:            return "OK";
        case TMZ_PROFILE_ERROR_UNKNOWN: return "Unknown profile";
        case TMZ_PROFILE_ERROR_ARGS:    return "Invalid profile arguments";
        case TMZ_PROFILE_ERROR_BUILD:   return "Block build failed";
    }
    return "Unknown profile error";
}
