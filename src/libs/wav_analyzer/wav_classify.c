/**
 * @file   wav_classify.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  Implementace vrstvy 3 - klasifikace formátu kazetového záznamu.
 *
 * Dvoustupňová detekce:
 * A) rychlostní třída z leader tónu (tabulkový přístup z wavdec.c)
 * B) zpřesnění formátu ze signatur v hlavičce (FASTIPL, TURBO, BSD)
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

#include <stdio.h>
#include <string.h>

#include "libs/mzf/mzf.h"
#include "libs/endianity/endianity.h"

#include "wav_classify.h"


/** @brief Hranice rychlostních tříd (us). */
#define SPEED_CLASS_DIRECT_MAX      70.0
#define SPEED_CLASS_FAST_MAX        155.0
#define SPEED_CLASS_MEDIUM_MAX      220.0
#define SPEED_CLASS_NORMAL_MAX      500.0
/* ZX: pilot 2168 T (619 us při 3,5 MHz, Intercopy 1400 Bd asi 612 us), Intercopy SINCLAIR 900 Bd 930 us */
#define SPEED_CLASS_ZX_MAX          1100.0

/**
 * @brief Signatura předzavaděče TurboCopy V1.2x v cmnt[0..6] hlavičky ("[turbo]" v Sharp ASCII).
 *
 * Stejné bajty porovnává Intercopy 10.2 na $18CA (hlavička $12..$1E =
 * 5A 00 00 D4 00 D4 5B 96 A5 9D 9A B7 5D).
 * Ověřeno na všech předzavaděčích TurboCopy a Intercopy v tstdata.
 * Dřívější heuristika hledala v komentáři ASCII "NIPSOFT", ten ale
 * předzavaděč nenese a nese ho řada běžných programů.
 */
static const uint8_t g_turbocopy_signature[7] = { 0x5B, 0x96, 0xA5, 0x9D, 0x9A, 0xB7, 0x5D };

/** @brief Velikost těla předzavaděče TurboCopy V1.2x pro TURBO (B). */
#define TURBOCOPY_PRELOADER_SIZE   90


/**
 * @brief Cílová adresa FSK/SLOW/DIRECT loaderu v paměti Z80.
 *
 * Všechny tři formáty (FSK, SLOW, DIRECT) používají stejný
 * preloader (loader_turbo), který nahraje loader body na adresu
 * 0xD400. Tato adresa je uložena v cmnt[3..4] (LE).
 * TURBO formát naopak nahrává přímo uživatelská data
 * na originální adresu programu.
 */
#define NONSTANDARD_LOADER_FROM     0xD400

/** @brief Velikost loader body pro FSK formát (bajty). */
#define FSK_LOADER_BODY_SIZE        191
/** @brief Velikost loader body pro SLOW formát (bajty). */
#define SLOW_LOADER_BODY_SIZE       249
/** @brief Velikost loader body pro DIRECT formát (bajty). */
#define DIRECT_LOADER_BODY_SIZE     360


/** @brief Textové názvy rychlostních tříd. */
static const char *g_speed_class_names[] = {
    [WAV_SPEED_CLASS_UNKNOWN] = "UNKNOWN",
    [WAV_SPEED_CLASS_DIRECT]  = "DIRECT (<70 us)",
    [WAV_SPEED_CLASS_FAST]    = "FAST (70-155 us)",
    [WAV_SPEED_CLASS_MEDIUM]  = "MEDIUM (155-220 us)",
    [WAV_SPEED_CLASS_NORMAL]  = "NORMAL (220-500 us)",
    [WAV_SPEED_CLASS_ZX]      = "ZX (500-1100 us)",
    [WAV_SPEED_CLASS_ERROR]   = "ERROR (>1100 us)",
};


en_WAV_SPEED_CLASS wav_classify_speed_class (
    double avg_period_us
) {
    if ( avg_period_us < SPEED_CLASS_DIRECT_MAX ) {
        return WAV_SPEED_CLASS_DIRECT;
    } else if ( avg_period_us < SPEED_CLASS_FAST_MAX ) {
        return WAV_SPEED_CLASS_FAST;
    } else if ( avg_period_us < SPEED_CLASS_MEDIUM_MAX ) {
        return WAV_SPEED_CLASS_MEDIUM;
    } else if ( avg_period_us < SPEED_CLASS_NORMAL_MAX ) {
        return WAV_SPEED_CLASS_NORMAL;
    } else if ( avg_period_us < SPEED_CLASS_ZX_MAX ) {
        return WAV_SPEED_CLASS_ZX;
    } else {
        return WAV_SPEED_CLASS_ERROR;
    }
}


en_WAV_TAPE_FORMAT wav_classify_from_leader (
    const st_WAV_LEADER_INFO *leader,
    const st_WAV_HISTOGRAM *hist
) {
    (void) hist; /* zatim nepouzito, pripraveno pro budouci histogramovou klasifikaci */
    if ( !leader ) return WAV_TAPE_FORMAT_UNKNOWN;

    en_WAV_SPEED_CLASS speed = wav_classify_speed_class ( leader->avg_period_us );

    switch ( speed ) {
        case WAV_SPEED_CLASS_DIRECT:
            return WAV_TAPE_FORMAT_DIRECT;

        case WAV_SPEED_CLASS_FAST:
        case WAV_SPEED_CLASS_MEDIUM:
        case WAV_SPEED_CLASS_NORMAL:
            /*
             * FM-first pristup: NORMAL FM muze byt pri jakekoli rychlosti
             * (1:1 az 3:1+). Intercopy napriklad nahraba stejny program
             * pri ruznych rychlostech, cimz leader prumer prochazi
             * pres tridy NORMAL (220+ us), MEDIUM (155-220 us)
             * i FAST (70-155 us).
             *
             * Konkretni format (TURBO, FASTIPL, CPM-CMT, BSD,
             * FSK, SLOW, DIRECT) se urci az ze stupne B - z obsahu
             * hlavicky po uspesnem FM dekodovani.
             *
             * FM dekoder (wav_decode_fm) adaptivne kalibruje threshold
             * z leaderu (leader_avg * 1.6, WAV_ANALYZER_FM_THRESHOLD_FACTOR), takze funguje pri jakekoli
             * rychlosti.
             */
            return WAV_TAPE_FORMAT_NORMAL;

        case WAV_SPEED_CLASS_ZX:
            return WAV_TAPE_FORMAT_ZX_SPECTRUM;

        default:
            return WAV_TAPE_FORMAT_UNKNOWN;
    }
}


/**
 * @brief Vstupní bod TURBO/FASTIPL loaderu v paměti MZ-800.
 *
 * Obě varianty loaderu (TURBO i FASTIPL) používají fexec=$1110
 * jako vstupní bod, protože loader se nachází v komentářové oblasti
 * hlavičky načtené na adresu $1200.
 */
#define TURBO_LOADER_FEXEC  0x1110

/**
 * @brief Adresa načtení hlavičky v paměti MZ-800 (IPL oblast).
 */
#define TURBO_LOADER_FSTRT  0x1200

/**
 * @brief Z80 opcode vzor TURBO loaderu v komentáři hlavičky.
 *
 * TURBO loader (z mzftools) ma na cmnt[8..11] sekvenci:
 * LD A,$08 ; OUT ($CE),A - vypnuti boot ROM MZ-800.
 * Tato sekvence je specificka pro TURBO loader a neobjevuje se
 * v beznych programech.
 */
static const uint8_t g_turbo_loader_pattern[] = { 0x3E, 0x08, 0xD3, 0xCE };
#define TURBO_LOADER_PATTERN_OFFSET 8
#define TURBO_LOADER_PATTERN_LEN    ( sizeof ( g_turbo_loader_pattern ) )

/**
 * @brief Zkontroluje, zda hlavička obsahuje TURBO loader.
 *
 * Detekuje TURBO loader z mzftools podle kombinace:
 * - fsize = 0 (loader nema telo)
 * - fexec = $1110 (vstupni bod loaderu)
 * - fstrt = $1200 (adresa hlavicky v pameti)
 * - cmnt[8..11] = Z80 opcode vzor (LD A,8 ; OUT ($CE),A)
 *
 * @param header MZF hlavička (po endianity korekci).
 * @return 1 pokud detekovan TURBO loader, 0 jinak.
 */
static int classify_check_turbo_loader ( const st_MZF_HEADER *header ) {
    if ( header->fsize != 0 ) return 0;
    if ( header->fexec != TURBO_LOADER_FEXEC ) return 0;
    if ( header->fstrt != TURBO_LOADER_FSTRT ) return 0;

    if ( TURBO_LOADER_PATTERN_OFFSET + TURBO_LOADER_PATTERN_LEN > MZF_CMNT_LENGTH ) {
        return 0;
    }

    return memcmp ( &header->cmnt[TURBO_LOADER_PATTERN_OFFSET],
                    g_turbo_loader_pattern,
                    TURBO_LOADER_PATTERN_LEN ) == 0;
}


/**
 * @brief Zkontroluje, zda preloader obsahuje FSK/SLOW/DIRECT loader.
 *
 * FSK, SLOW a DIRECT formáty používají stejný preloader (loader_turbo)
 * jako TURBO, ale liší se hodnotami v komentářové oblasti:
 * - cmnt[3..4] (LE) = 0xD400 (cílová adresa loaderu)
 * - cmnt[1..2] (LE) = velikost loader body (191=FSK, 249=SLOW, 360=DIRECT)
 *
 * TURBO formát naopak nahrává uživatelská data na originální adresu
 * programu, takže cmnt[3..4] != 0xD400.
 *
 * @param header MZF hlavička (po endianity korekci).
 * @return Detekovaný formát (FSK/SLOW/DIRECT), nebo UNKNOWN pokud neodpovídá.
 */
static en_WAV_TAPE_FORMAT classify_check_nonstandard_loader ( const st_MZF_HEADER *header ) {
    /* cmnt[3..4] = loader_from (LE, nekorekovaný) */
    uint16_t loader_from;
    memcpy ( &loader_from, &header->cmnt[3], 2 );
    loader_from = endianity_bswap16_LE ( loader_from );

    if ( loader_from != NONSTANDARD_LOADER_FROM ) {
        return WAV_TAPE_FORMAT_UNKNOWN;
    }

    /* cmnt[1..2] = loader_body_size (LE, nekorekovaný) */
    uint16_t loader_size;
    memcpy ( &loader_size, &header->cmnt[1], 2 );
    loader_size = endianity_bswap16_LE ( loader_size );

    if ( loader_size == FSK_LOADER_BODY_SIZE ) {
        return WAV_TAPE_FORMAT_FSK;
    }
    if ( loader_size == SLOW_LOADER_BODY_SIZE ) {
        return WAV_TAPE_FORMAT_SLOW;
    }
    if ( loader_size == DIRECT_LOADER_BODY_SIZE ) {
        return WAV_TAPE_FORMAT_DIRECT;
    }

    return WAV_TAPE_FORMAT_UNKNOWN;
}


en_WAV_TAPE_FORMAT wav_classify_from_header (
    const st_MZF_HEADER *header,
    en_WAV_TAPE_FORMAT preliminary_format
) {
    if ( !header ) return preliminary_format;

    /* test FASTIPL: header[0] == 0xBB */
    if ( header->ftype == 0xBB ) {
        return WAV_TAPE_FORMAT_FASTIPL;
    }

    /*
     * Test turbo-family preloader (sdílený loader_turbo).
     * FSK, SLOW a DIRECT používají stejný preloader jako TURBO,
     * liší se hodnotami v cmnt oblasti.
     */
    if ( classify_check_turbo_loader ( header ) ) {
        en_WAV_TAPE_FORMAT nonstandard = classify_check_nonstandard_loader ( header );
        if ( nonstandard != WAV_TAPE_FORMAT_UNKNOWN ) {
            return nonstandard;
        }
        return WAV_TAPE_FORMAT_TURBO;
    }

    /*
     * Test TurboCopy preloader: fstrt=fexec=$D400, fsize <= 500.
     * Preloader nahravany na adresu $D400 s loaderem v body (fsize 90..360).
     * TurboCopy TURBO ma 90 B se signaturou; velikosti ~191/249/360 se
     * vykladaji jako loader FSK/SLOW/DIRECT (puvod techto preloaderu
     * [neovereno]). Typ loaderu urcime z velikosti body:
     *   ~90 = TURBO, ~191 = FSK, ~249 = SLOW, ~360 = DIRECT
     */
    if ( header->fstrt == NONSTANDARD_LOADER_FROM &&
         header->fexec == NONSTANDARD_LOADER_FROM &&
         header->fsize > 0 && header->fsize <= 500 ) {
        if ( header->fsize >= DIRECT_LOADER_BODY_SIZE - 10 &&
             header->fsize <= DIRECT_LOADER_BODY_SIZE + 10 ) {
            return WAV_TAPE_FORMAT_DIRECT;
        }
        if ( header->fsize >= SLOW_LOADER_BODY_SIZE - 10 &&
             header->fsize <= SLOW_LOADER_BODY_SIZE + 10 ) {
            return WAV_TAPE_FORMAT_SLOW;
        }
        if ( header->fsize >= FSK_LOADER_BODY_SIZE - 10 &&
             header->fsize <= FSK_LOADER_BODY_SIZE + 10 ) {
            return WAV_TAPE_FORMAT_FSK;
        }
        /*
         * TURBO jen se signaturou předzavaděče TurboCopy V1.2x; jiný program
         * na $D400 je obyčejný záznam (dřív se zahodil)
         */
        if ( header->fsize == TURBOCOPY_PRELOADER_SIZE
             && memcmp ( header->cmnt, g_turbocopy_signature, sizeof ( g_turbocopy_signature ) ) == 0 ) {
            return WAV_TAPE_FORMAT_TURBO;
        }
        return preliminary_format;
    }

    /*
     * Test BSD: typ 0x04 (BRD, zapisuje WOPEN# BASICu 1Z-016) nebo 0x03
     * (BSD, dřívější výchozí typ dat2bsd), fsize=0, fstrt=0,
     * fexec=0 (soubor z WOPEN# BASICu 1Z-016 má typ 04h).
     * Pozor: fsize, fstrt, fexec jsou v host byte-order
     * (po mzf_header_swap_endianity).
     */
    if ( ( header->ftype == MZF_FTYPE_BRD || header->ftype == MZF_FTYPE_BSD ) &&
         header->fsize == 0 &&
         header->fstrt == 0 &&
         header->fexec == 0 ) {
        return WAV_TAPE_FORMAT_BSD;
    }

    /*
     * Test CPM-CMT: ftype=$22 (CP/M CMT.COM, SOKODI a RadSoft).
     * Identifikace z CMT.COM v.7 (SOKODI 1988): write_header na $0773
     * zapisuje LD (HL),$22 jako ftype.
     * Intercopy CPM do této kategorie nepatří: zapisuje Manchester
     * (rodina Pezik TAPE) s původním ftype souboru.
     */
    if ( header->ftype == 0x22 ) {
        return WAV_TAPE_FORMAT_CPM_CMT;
    }

    /* žádná signatura - ponecháme předběžný formát */
    return preliminary_format;
}


const char* wav_speed_class_name (
    en_WAV_SPEED_CLASS speed_class
) {
    if ( speed_class < 0 || ( int ) speed_class > ( int ) WAV_SPEED_CLASS_ERROR ) {
        return "UNKNOWN";
    }
    return g_speed_class_names[speed_class];
}
