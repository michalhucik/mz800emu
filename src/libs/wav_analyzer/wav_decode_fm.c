/**
 * @file   wav_decode_fm.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.5.0
 * @brief  Implementace vrstvy 4a - FM dekodér.
 *
 * Dekóduje FM modulovaný signál (NORMAL/SINCLAIR/CPM/MZ-80B).
 * Algoritmus inspirován wavdec.c a Intercopy V10.2:
 * - práh SHORT/LONG = leader_avg * 1.6 (WAV_ANALYZER_FM_THRESHOLD_FACTOR)
 * - tapemark: N LONG + N SHORT pulzů
 * - bajty: MSB first, 1 stop bit (LONG)
 * - CRC: 16-bit popcount (počet jedničkových bitů), big-endian
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
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "libs/mzf/mzf.h"
#include "libs/endianity/endianity.h"

#include "wav_decode_fm.h"
#include "wav_leader.h"


/**
 * @brief Minimální počty LONG a SHORT pulzů pro detekci tapemarku.
 *
 * Skutečné hodnoty jsou 40/40 (hlavička) a 20/20 (tělo).
 * Používáme nižší prahy kvůli možným chybám v signálu.
 */
#define TAPEMARK_LONG_MIN_LTM   36  /**< min LONG pro hlavičku */
#define TAPEMARK_SHORT_MIN_LTM  30  /**< min SHORT pro hlavičku */
#define TAPEMARK_LONG_MIN_STM   18  /**< min LONG pro tělo */
#define TAPEMARK_SHORT_MIN_STM  18  /**< min SHORT pro tělo */
/** @brief Nejvíc dlouhých period tapemarku těla při hledání po periodách (STM má 20, LTM 40). */
#define BODY_STM_MAX_LONG       30

/**
 * @brief Minimální počet pulzů leader tónu pro hledání body leaderu.
 *
 * Body leader je kratší než header leader (11000 vs. 22000),
 * takže používáme nižší práh.
 */
#define BODY_LEADER_MIN_PULSES  500


void wav_decode_fm_init (
    st_WAV_FM_DECODER *dec,
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t start_pos,
    double leader_avg_us
) {
    if ( !dec || !seq ) return;

    dec->seq = seq;
    dec->pos = start_pos;
    dec->threshold_us = leader_avg_us * WAV_ANALYZER_FM_THRESHOLD_FACTOR;
    dec->crc_accumulator = 0;
}


/**
 * @brief Zjistí, zda je pulz na dané pozici LONG (bit 1).
 *
 * @param dec Kontext dekodéru.
 * @param pos Pozice pulzu v sekvenci.
 * @return 1 pokud je LONG, 0 pokud je SHORT.
 */
static inline int is_long_pulse ( const st_WAV_FM_DECODER *dec, uint32_t pos ) {
    return dec->seq->pulses[pos].duration_us >= dec->threshold_us;
}


/**
 * @brief Najde tapemark s volitelnou horní mezí délky běhu LONG.
 *
 * @param dec Dekodér; při nalezení je dec->pos za tapemarkem, jinak beze změny.
 * @param type Druh tapemarku (dolní meze).
 * @param max_long_hp Nejvíc půlvln LONG (0 = bez meze). Delší tapemark znamená LTM
 *        dalšího záznamu a hledání končí chybou (tělo aktuálního záznamu chybí).
 * @return WAV_ANALYZER_OK, WAV_ANALYZER_ERROR_DECODE_TAPEMARK, WAV_ANALYZER_ERROR_INVALID_PARAM.
 */
static en_WAV_ANALYZER_ERROR find_tapemark_bounded (
    st_WAV_FM_DECODER *dec,
    en_WAV_TAPEMARK_TYPE type,
    uint32_t max_long_hp
);


en_WAV_ANALYZER_ERROR wav_decode_fm_find_tapemark (
    st_WAV_FM_DECODER *dec,
    en_WAV_TAPEMARK_TYPE type
) {
    return find_tapemark_bounded ( dec, type, 0 );
}


static en_WAV_ANALYZER_ERROR find_tapemark_bounded (
    st_WAV_FM_DECODER *dec,
    en_WAV_TAPEMARK_TYPE type,
    uint32_t max_long_hp
) {
    if ( !dec || !dec->seq ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    uint32_t min_long = ( type == WAV_TAPEMARK_LONG ) ? TAPEMARK_LONG_MIN_LTM : TAPEMARK_LONG_MIN_STM;
    uint32_t min_short = ( type == WAV_TAPEMARK_LONG ) ? TAPEMARK_SHORT_MIN_LTM : TAPEMARK_SHORT_MIN_STM;

    const st_WAV_PULSE_SEQUENCE *seq = dec->seq;
    uint32_t pos = dec->pos;

    /*
     * Hledáme sekvenci: N LONG pulzů + N SHORT pulzů.
     * Každý pulz = 2 půl-periody (HIGH + LOW).
     * Počítáme půl-periody, ale minimum odpovídá celým pulzům.
     *
     * Tapemark: 40 LONG (= 80 LONG půl-period) + 40 SHORT (= 80 SHORT půl-period)
     * Minimum: 36 LONG pulzů = 72 LONG půl-period, 30 SHORT = 60 SHORT půl-period
     *
     * Ale protože obě fáze LONG pulzu (HIGH i LOW) jsou nad prahem
     * a obě fáze SHORT pulzu jsou pod prahem, stačí počítat
     * po sobě jdoucí půl-periody nad/pod prahem.
     */
    uint32_t min_long_hp = min_long * 2;    /* minimum v půl-periodách */
    uint32_t min_short_hp = min_short * 2;

    while ( pos < seq->count ) {
        /* přeskočíme SHORT půl-periody (zbytek leaderu) */
        while ( pos < seq->count && !is_long_pulse ( dec, pos ) ) {
            pos++;
        }

        /* počítáme LONG půl-periody */
        uint32_t long_hp_count = 0;
        while ( pos < seq->count && is_long_pulse ( dec, pos ) ) {
            long_hp_count++;
            pos++;
        }

        if ( long_hp_count < min_long_hp ) {
            /* nedostatek LONG pul-period - pokracujeme */
            continue;
        }

        /* počítáme SHORT půl-periody */
        uint32_t short_hp_count = 0;
        while ( pos < seq->count && !is_long_pulse ( dec, pos ) ) {
            short_hp_count++;
            pos++;
        }

        if ( short_hp_count >= min_short_hp ) {
            /* příliš dlouhý tapemark patří dalšímu záznamu: hledání končí */
            if ( max_long_hp && long_hp_count > max_long_hp ) {
                return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
            }
            /* tapemark nalezen - pozice je za ním */
            dec->pos = pos;
            return WAV_ANALYZER_OK;
        }

        /* nedostatek SHORT půl-period - pokračujeme od aktuální pozice */
    }

    return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
}


/**
 * @brief Průměr půlvlny v dvojici začínající na idx (us).
 * @param seq Sekvence. @param idx Index první půlvlny dvojice (idx + 1 < count).
 * @return Průměrná délka půlvlny dvojice.
 */
static inline double pair_avg_us ( const st_WAV_PULSE_SEQUENCE *seq, uint32_t idx ) {
    return ( seq->pulses[idx].duration_us + seq->pulses[idx + 1].duration_us ) / 2.0;
}


en_WAV_ANALYZER_ERROR wav_decode_fm_find_tapemark_period (
    st_WAV_FM_DECODER *dec,
    uint32_t min_long,
    uint32_t min_short,
    uint32_t max_long,
    uint32_t max_skip
) {
    if ( !dec || !dec->seq ) return WAV_ANALYZER_ERROR_INVALID_PARAM;
    const st_WAV_PULSE_SEQUENCE *seq = dec->seq;
    uint32_t q = dec->pos;
    while ( q + 1 < seq->count ) {
        while ( q + 1 < seq->count && pair_avg_us ( seq, q ) < dec->threshold_us ) q += 2;
        /* tapemark musí začít nejvýš max_skip period od začátku hledání */
        if ( max_skip && ( q - dec->pos ) / 2 > max_skip ) return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
        uint32_t n_long = 0;
        while ( q + 1 < seq->count && pair_avg_us ( seq, q ) >= dec->threshold_us ) {
            n_long++;
            q += 2;
        }
        if ( n_long < min_long ) continue;
        if ( max_long && n_long > max_long ) return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
        uint32_t n_short = 0;
        while ( q + 1 < seq->count && pair_avg_us ( seq, q ) < dec->threshold_us ) {
            n_short++;
            q += 2;
        }
        if ( n_short < min_short ) continue;
        /* synchronizace: nejvýš 2 dlouhé periody (2L) */
        for ( int sk = 0; sk < 2 && q + 1 < seq->count && pair_avg_us ( seq, q ) >= dec->threshold_us; sk++ ) q += 2;
        dec->pos = q;
        return WAV_ANALYZER_OK;
    }
    return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
}



en_WAV_ANALYZER_ERROR wav_decode_fm_read_bit (
    st_WAV_FM_DECODER *dec,
    int *bit
) {
    if ( !dec || !bit ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /*
     * Každý bit na pásce je celý pulz = 2 půl-periody (HIGH + LOW).
     * Přečteme 2 po sobě jdoucí půl-periody a zprůměrujeme.
     * Průměr porovnáme s prahem.
     *
     * Inspirováno wavdec.c a Intercopy: "Měří 2 půl-periody -> průměr
     * -> porovnání s thresholdem"
     */
    if ( dec->pos + 1 >= dec->seq->count ) {
        return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
    }

    double avg = ( dec->seq->pulses[dec->pos].duration_us +
                   dec->seq->pulses[dec->pos + 1].duration_us ) / 2.0;

    *bit = ( avg >= dec->threshold_us ) ? 1 : 0;

    /* aktualizace popcount */
    if ( *bit ) {
        dec->crc_accumulator++;
    }

    dec->pos += 2;
    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_decode_fm_read_byte (
    st_WAV_FM_DECODER *dec,
    uint8_t *byte
) {
    if ( !dec || !byte ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    uint8_t value = 0;

    /* 8 datových bitů, MSB first */
    for ( int i = 7; i >= 0; i-- ) {
        int bit;
        en_WAV_ANALYZER_ERROR err = wav_decode_fm_read_bit ( dec, &bit );
        if ( err != WAV_ANALYZER_OK ) return err;
        if ( bit ) {
            value |= ( 1 << i );
        }
    }

    /*
     * 1 stop bit (LONG) = 2 půl-periody.
     * Ověříme, ale neselháváme při chybě - některé záznamy
     * mají poškozený stop bit, ale data jsou v pořádku.
     */
    if ( dec->pos + 1 < dec->seq->count ) {
        /* přečteme stop bit (2 půl-periody), neakumulujeme do CRC */
        dec->pos += 2;
    }

    *byte = value;
    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_decode_fm_read_block_n (
    st_WAV_FM_DECODER *dec,
    uint8_t *data,
    uint32_t size,
    uint32_t *out_read
) {
    if ( out_read ) *out_read = 0;
    if ( !dec || !data ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    for ( uint32_t i = 0; i < size; i++ ) {
        en_WAV_ANALYZER_ERROR err = wav_decode_fm_read_byte ( dec, &data[i] );
        if ( err != WAV_ANALYZER_OK ) return err;
        if ( out_read ) *out_read = i + 1;
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_decode_fm_read_block (
    st_WAV_FM_DECODER *dec,
    uint8_t *data,
    uint32_t size
) {
    return wav_decode_fm_read_block_n ( dec, data, size, NULL );
}


en_WAV_ANALYZER_ERROR wav_decode_fm_verify_checksum (
    st_WAV_FM_DECODER *dec,
    en_WAV_CRC_STATUS *crc_status,
    uint16_t *crc_stored,
    uint16_t *crc_computed
) {
    if ( !dec || !crc_status ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* uložíme vypočtený checksum */
    uint16_t computed = dec->crc_accumulator;
    if ( crc_computed ) *crc_computed = computed;

    /*
     * Přečteme 2 bajty checksumu z pásky (big-endian).
     * CRC bajty přečteme BEZ akumulace do popcountem,
     * proto resetujeme akumulátor a přečteme přímo.
     */
    dec->crc_accumulator = 0;

    uint8_t crc_hi, crc_lo;
    en_WAV_ANALYZER_ERROR err;

    err = wav_decode_fm_read_byte ( dec, &crc_hi );
    if ( err != WAV_ANALYZER_OK ) {
        dec->crc_accumulator = 0;
        *crc_status = WAV_CRC_NOT_AVAILABLE;
        return WAV_ANALYZER_OK;
    }

    err = wav_decode_fm_read_byte ( dec, &crc_lo );
    if ( err != WAV_ANALYZER_OK ) {
        dec->crc_accumulator = 0;
        *crc_status = WAV_CRC_NOT_AVAILABLE;
        return WAV_ANALYZER_OK;
    }

    uint16_t stored = ( ( uint16_t ) crc_hi << 8 ) | crc_lo;
    if ( crc_stored ) *crc_stored = stored;

    /* porovnáme */
    *crc_status = ( computed == stored ) ? WAV_CRC_OK : WAV_CRC_ERROR;

    /* resetujeme akumulátor pro další blok */
    dec->crc_accumulator = 0;

    return WAV_ANALYZER_OK;
}


/**
 * @brief Nejkratší oddělovač druhé kopie (půlvlny SHORT).
 *
 * ROM MZ-800 zapisuje mezi kopie 256 SHORT pulzů (512 půlvln) a při čtení
 * kopie čeká na 100 souvislých nul (@RINTR v ROM), tedy 200 půlvln. V datech FM je nejdelší
 * běh SHORT 16 půlvln (8 nul a start bit LONG), takže kratší běh je data.
 */
#define COPY2_MIN_SEP_HALVES    200

/**
 * @brief Nejdelší oddělovač druhé kopie (půlvlny SHORT).
 *
 * Delší běh SHORT je tón další části záznamu (GAP těla nebo další nahrávka),
 * hledání kopie tím končí.
 */
#define COPY2_MAX_SEP_HALVES    3000

/** @brief Rezerva okna hledání kopie za dvojnásobkem nominální délky sekce (půlvlny). */
#define COPY2_SEARCH_MARGIN     4000


/**
 * @brief Pokusí se dekódovat druhou kopii bloku (Copy2) podle zápisu ROM.
 *
 * ROM zapíše za checksum 1. kopie 1 LONG, 256 SHORT a hned druhou kopii
 * (každý bajt start bit LONG a 8 bitů, pak checksum a 1 LONG), bez tónu
 * a tapemarku (kód ROM; záznam ROM SAVE z emulátoru; generátor mzcmt_fm). Funkce hledá od začátku
 * 1. kopie běh SHORT dlouhý COPY2_MIN_SEP_HALVES až COPY2_MAX_SEP_HALVES
 * půlvln (kratší jsou data, delší tón další části), za ním přeskočí start
 * bit (2 půlvlny) a přečte blok a checksum. Kandidát s chybou CRC se
 * přeskočí a zkusí se další běh v okně (šum v 1. kopii umí vytvořit
 * falešný dlouhý běh SHORT).
 *
 * Hledá se od začátku sekce, ne od konce 1. kopie: výpadek v 1. kopii
 * posune čtení přes oddělovač do 2. kopie. Za oddělovačem se nehledá
 * tapemark, ROM ho před kopii nezapisuje.
 *
 * @param seq Sekvence pulzů.
 * @param section_start První datová půlvlna 1. kopie sekce (za start bitem).
 * @param threshold_us Práh SHORT/LONG v mikrosekundách.
 * @param expected_size Velikost dat sekce v bajtech (bez checksumu).
 * @param[out] out_data Data 2. kopie s CRC OK (alokována, uvolní volající),
 *             jinak NULL.
 * @param[out] out_crc_status WAV_CRC_OK (kopie s platným checksumem),
 *             WAV_CRC_ERROR (kopie strukturálně nalezena, ale checksum
 *             nesedí; data se nevracejí), WAV_CRC_NOT_AVAILABLE (nenalezena).
 * @param[out] out_crc_stored Uložený checksum. Může být NULL.
 * @param[out] out_crc_computed Vypočtený checksum. Může být NULL.
 * @param[out] out_end_pos Pozice za checksumem 2. kopie (i při WAV_CRC_ERROR,
 *             aby se oddělovač nehlásil jako leader). Může být NULL.
 * @return WAV_ANALYZER_OK, když byla kopie nalezena (CRC viz out_crc_status);
 *         WAV_ANALYZER_ERROR_NO_LEADER, když v okně není žádný oddělovač
 *         s čitelnou kopií; WAV_ANALYZER_ERROR_ALLOC; WAV_ANALYZER_ERROR_INVALID_PARAM.
 */
en_WAV_ANALYZER_ERROR wav_decode_fm_try_copy2 (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t section_start,
    double threshold_us,
    uint32_t expected_size,
    uint8_t **out_data,
    en_WAV_CRC_STATUS *out_crc_status,
    uint16_t *out_crc_stored,
    uint16_t *out_crc_computed,
    uint32_t *out_end_pos
) {
    if ( !seq || !out_data || !out_crc_status ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    *out_data = NULL;
    *out_crc_status = WAV_CRC_NOT_AVAILABLE;
    if ( out_end_pos ) *out_end_pos = section_start;

    /* okno: 2x nominální délka sekce (data + checksum, 18 půlvln na bajt) + rezerva */
    uint64_t sec_halves = ( ( uint64_t ) expected_size + 2 ) * 18 + 2;
    uint64_t lim64 = ( uint64_t ) section_start + 2 * sec_halves + COPY2_SEARCH_MARGIN;
    uint32_t limit = ( lim64 > seq->count ) ? seq->count : ( uint32_t ) lim64;

    int structural_found = 0;       /* kopie s chybou CRC (pro konec spotřebovaného úseku) */
    uint32_t structural_end = section_start;
    uint16_t structural_stored = 0, structural_computed = 0;

    uint32_t pos = section_start;
    while ( pos < limit ) {
        if ( seq->pulses[pos].duration_us >= threshold_us ) {
            pos++;
            continue;
        }
        uint32_t run_start = pos;
        while ( pos < seq->count && seq->pulses[pos].duration_us < threshold_us ) pos++;
        uint32_t run = pos - run_start;
        if ( run < COPY2_MIN_SEP_HALVES ) continue;          /* krátký běh SHORT = data */
        if ( run > COPY2_MAX_SEP_HALVES ) break;             /* tón další části */

        /* start bit prvního bajtu kopie = 1 LONG (2 půlvlny) */
        if ( pos + 1 >= seq->count ) break;
        st_WAV_FM_DECODER dec;
        dec.seq = seq;
        dec.pos = pos + 2;
        dec.threshold_us = threshold_us;
        dec.crc_accumulator = 0;

        uint8_t *data = ( uint8_t* ) malloc ( expected_size ? expected_size : 1 );
        if ( !data ) return WAV_ANALYZER_ERROR_ALLOC;
        if ( wav_decode_fm_read_block ( &dec, data, expected_size ) != WAV_ANALYZER_OK ) {
            free ( data );
            continue;
        }
        en_WAV_CRC_STATUS st;
        uint16_t cs = 0, cc = 0;
        wav_decode_fm_verify_checksum ( &dec, &st, &cs, &cc );
        if ( st == WAV_CRC_OK ) {
            *out_data = data;
            *out_crc_status = WAV_CRC_OK;
            if ( out_crc_stored ) *out_crc_stored = cs;
            if ( out_crc_computed ) *out_crc_computed = cc;
            if ( out_end_pos ) *out_end_pos = dec.pos;
            return WAV_ANALYZER_OK;
        }
        free ( data );
        if ( !structural_found ) {
            structural_found = 1;
            structural_end = dec.pos;
            structural_stored = cs;
            structural_computed = cc;
        }
        /* chyba CRC: další kandidát (např. šum v 1. kopii) */
    }

    if ( structural_found ) {
        *out_crc_status = WAV_CRC_ERROR;
        if ( out_crc_stored ) *out_crc_stored = structural_stored;
        if ( out_crc_computed ) *out_crc_computed = structural_computed;
        if ( out_end_pos ) *out_end_pos = structural_end;
        return WAV_ANALYZER_OK;
    }
    return WAV_ANALYZER_ERROR_NO_LEADER;
}


/**
 * @brief Tapemark po celých periodách v obou fázích dvojic půlvln.
 *
 * Konec leaderu může ležet na sudé i liché půlvlně; při nesouměrných
 * půlvlnách jen jedna fáze dvojic dá správné bity. Zkusí fázi od from
 * a od from + 1; za tapemarkem přečte data_size bajtů a checksum a vybere
 * fázi s platným checksumem. Bez platného checksumu vrátí první fázi,
 * ve které se tapemark našel (data a CRC pak vyhodnotí volající).
 *
 * @param dec Kontext dekodéru (seq, threshold_us); nemění se.
 * @param from Začátek hledání (pulz). @param min_long, min_short, max_long Jako
 *        wav_decode_fm_find_tapemark_period(). @param data_size Bajtů dat za
 *        tapemarkem (0 = jen najít tapemark ve fázi from).
 * @param[out] pos Pozice první půlvlny dat (za synchronizací).
 * @return WAV_ANALYZER_OK, nebo WAV_ANALYZER_ERROR_DECODE_TAPEMARK, když se
 *         tapemark nenašel v žádné fázi.
 */
static en_WAV_ANALYZER_ERROR find_tapemark_period_phase ( const st_WAV_FM_DECODER *dec, uint32_t from,
                                                         uint32_t min_long, uint32_t min_short, uint32_t max_long,
                                                         uint32_t data_size, uint32_t *pos ) {
    int found = 0;
    for ( uint32_t phase = 0; phase < 2; phase++ ) {
        st_WAV_FM_DECODER t = *dec;
        t.pos = from + phase;
        if ( wav_decode_fm_find_tapemark_period ( &t, min_long, min_short, max_long, 0 ) != WAV_ANALYZER_OK ) continue;
        if ( !found ) *pos = t.pos;
        found = 1;
        if ( data_size == 0 ) break;
        uint8_t *probe = ( uint8_t* ) malloc ( data_size );
        if ( !probe ) break;
        st_WAV_FM_DECODER p = t;
        p.crc_accumulator = 0;
        en_WAV_CRC_STATUS st = WAV_CRC_ERROR;
        uint16_t stored = 0, computed = 0;
        if ( wav_decode_fm_read_block ( &p, probe, data_size ) == WAV_ANALYZER_OK ) {
            wav_decode_fm_verify_checksum ( &p, &st, &stored, &computed );
        }
        free ( probe );
        if ( st == WAV_CRC_OK ) {
            *pos = t.pos;
            break;
        }
    }
    return found ? WAV_ANALYZER_OK : WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
}


int wav_decode_fm_find_copy_start (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t section_start,
    double threshold_us,
    uint32_t expected_size,
    uint32_t *out_start
) {
    if ( !seq || !out_start ) return 0;

    uint64_t sec_halves = ( ( uint64_t ) expected_size + 2 ) * 18 + 2;
    uint64_t lim64 = ( uint64_t ) section_start + 2 * sec_halves + COPY2_SEARCH_MARGIN;
    uint32_t limit = ( lim64 > seq->count ) ? seq->count : ( uint32_t ) lim64;

    uint32_t pos = section_start;
    while ( pos < limit ) {
        if ( seq->pulses[pos].duration_us >= threshold_us ) {
            pos++;
            continue;
        }
        uint32_t run_start = pos;
        while ( pos < seq->count && seq->pulses[pos].duration_us < threshold_us ) pos++;
        uint32_t run = pos - run_start;
        if ( run < COPY2_MIN_SEP_HALVES ) continue;
        if ( run > COPY2_MAX_SEP_HALVES || pos + 2 >= seq->count ) return 0;
        *out_start = pos + 2;   /* za start bitem prvního bajtu kopie */
        return 1;
    }
    return 0;
}


/**
 * @brief Spočítá bajty, ve kterých se data 1. kopie liší od 2. kopie.
 *
 * @param dec Dekodér (práh, sekvence; nemění se).
 * @param section_start První datová půlvlna 1. kopie.
 * @param data Data 1. kopie.
 * @param size Velikost dat.
 * @return Počet rozdílných bajtů; 0, když kopie není, nedá se přečíst, nemá vlastní
 *         platný součet (poškozená kopie, nebo vůbec o kopii nejde), nebo se liší ve více
 *         než polovině bajtů.
 */
static uint32_t count_copy_diff ( const st_WAV_FM_DECODER *dec, uint32_t section_start, const uint8_t *data, uint32_t size ) {
    uint32_t start = 0;
    if ( size == 0 || !wav_decode_fm_find_copy_start ( dec->seq, section_start, dec->threshold_us, size, &start ) ) return 0;
    uint8_t *copy = ( uint8_t* ) malloc ( size );
    if ( !copy ) return 0;
    st_WAV_FM_DECODER c = *dec;
    c.pos = start;
    c.crc_accumulator = 0;
    uint32_t diff = 0;
    if ( wav_decode_fm_read_block ( &c, copy, size ) == WAV_ANALYZER_OK ) {
        en_WAV_CRC_STATUS st = WAV_CRC_ERROR;
        wav_decode_fm_verify_checksum ( &c, &st, NULL, NULL );
        if ( st == WAV_CRC_OK ) {
            for ( uint32_t i = 0; i < size; i++ ) diff += ( copy[i] != data[i] );
        }
    }
    free ( copy );
    return ( diff > size / 2 ) ? 0 : diff;
}


/**
 * @brief Nejkratší běh SHORT půlvln, který uvnitř dat nemůže vzniknout.
 *
 * Bajt má vždy start bit LONG (kód ROM @WBYTE), nejdelší běh SHORT v datech je
 * proto 16 půlvln (bajt 00h). Oddělovač kopie (256 S), tón ani konec dat kratší
 * běh neprodukují; hodnota 32 nechává rezervu na poškozený start bit.
 */
#define FM_DATA_END_SHORT_RUN 32

/** @brief Půlvln na bajt: start bit a 8 datových bitů po dvou půlvlnách. */
#define FM_BYTE_HALVES 18U


/**
 * @brief Najde v úseku první běh SHORT půlvln delší, než jaký může být v datech.
 *
 * @param dec Dekodér (práh, sekvence).
 * @param from První prohledávaná půlvlna.
 * @param to Konec úseku (výlučně); běh může za něj pokračovat.
 * @param[out] out_pos Index první půlvlny běhu.
 * @return 1 = běh nalezen, 0 = nenalezen.
 */
static int find_short_run ( const st_WAV_FM_DECODER *dec, uint32_t from, uint32_t to, uint32_t *out_pos ) {
    uint32_t run = 0;
    for ( uint32_t i = from; i < dec->seq->count && ( i < to || run > 0 ); i++ ) {
        if ( is_long_pulse ( dec, i ) ) {
            run = 0;
        } else if ( ++run >= FM_DATA_END_SHORT_RUN ) {
            *out_pos = i + 1 - run;
            return 1;
        }
    }
    return 0;
}


/**
 * @brief Násobek prahu, od kterého je půlvlna mezera v signálu (ne LONG).
 *
 * LONG je zhruba 1,2násobek prahu; poslední půlvlna záznamu před tichem splyne s pauzou.
 */
#define FM_DATA_END_GAP_FACTOR 4.0


/**
 * @brief Najde konec datové sekce od dané půlvlny.
 *
 * @param dec Dekodér.
 * @param from První datová půlvlna.
 * @param stop_at_gap 1 = mezera v signálu také končí sekci (nepoškozený záznam), 0 = ne
 *        (výpadek uvnitř poškozené kopie sekci nekončí).
 * @return Index za koncem sekce: začátek běhu SHORT, který v datech být nemůže, půlvlna
 *         za mezerou (koncová půlvlna splynulá s tichem patří k sekci), nebo konec sekvence.
 *         Se stop_at_gap = 0 končí sekci mezera jen tehdy, když za ní do běhu SHORT
 *         nebo konce sekvence není ani jeden bajt (ticho za záznamem, ne výpadek v datech).
 */
static uint32_t data_section_end ( const st_WAV_FM_DECODER *dec, uint32_t from, int stop_at_gap ) {
    uint32_t run = 0;
    uint32_t end = dec->seq->count;
    uint32_t gap_end = 0;
    for ( uint32_t i = from; i < dec->seq->count; i++ ) {
        if ( dec->seq->pulses[i].duration_us >= dec->threshold_us * FM_DATA_END_GAP_FACTOR ) {
            if ( stop_at_gap ) return i + 1;
            gap_end = i + 1;
        }
        if ( is_long_pulse ( dec, i ) ) {
            run = 0;
        } else if ( ++run >= FM_DATA_END_SHORT_RUN ) {
            end = i + 1 - run;
            break;
        }
    }
    return ( gap_end && end - gap_end < FM_BYTE_HALVES ) ? gap_end : end;
}


en_WAV_ANALYZER_ERROR wav_decode_fm_estimate_body_bytes (
    const st_WAV_FM_DECODER *dec,
    uint32_t from,
    uint32_t *out_bytes,
    int *out_frame_ok
) {
    if ( !dec || !out_bytes || !out_frame_ok ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    uint32_t end = data_section_end ( dec, from, 1 );

    /* od první datové půlvlny: (n + 2) bajtů po 18 půlvlnách včetně koncového L */
    uint32_t halves = ( end > from ) ? end - from : 0;
    if ( halves + 1 < FM_BYTE_HALVES * 3 ) {
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }
    *out_bytes = ( halves + 1 ) / FM_BYTE_HALVES - 2;
    *out_frame_ok = ( ( halves + 1 ) % FM_BYTE_HALVES ) <= 2;
    return WAV_ANALYZER_OK;
}


/** @brief Kolik bajtů před místem ztráty rámce se považuje za nespolehlivých. */
#define FM_MERGE_BACKOFF 8U

/** @brief Nejvíc rozdílných bajtů, jejichž kombinace se zkoušejí. */
#define FM_MERGE_MAX_DIFF 16U

/** @brief Rezerva bitu pod tímto podílem prahu je pochybná (změřeno na simulaci poškození). */
#define FM_MERGE_DOUBT 0.05

/** @brief Kolik z následujících start bitů musí sedět, aby šlo o chybu jednoho bitu, ne ztrátu rámce. */
#define FM_MERGE_LOOKAHEAD_HITS 7

/** @brief Počet následujících start bitů kontrolovaných při pochybnosti o rámci. */
#define FM_MERGE_LOOKAHEAD 8U


/** @brief Počet jedničkových bitů v bajtu. @param v Bajt. @return Počet. */
static int bits_set ( uint8_t v ) {
    int n = 0;
    for ( ; v; v &= ( uint8_t ) ( v - 1 ) ) n++;
    return n;
}


/** @brief Průměr dvojice půlvln. @param dec Dekodér. @param p Index první půlvlny (p + 1 < count). @return Průměr v us. */
static inline double pair_avg ( const st_WAV_FM_DECODER *dec, uint32_t p ) {
    return ( dec->seq->pulses[p].duration_us + dec->seq->pulses[p + 1].duration_us ) / 2.0;
}


/**
 * @brief Přečte bajt na pevné pozici.
 * @param dec Dekodér. @param p První půlvlna bajtu (p + 16 <= count).
 * @param[out] v Bajt. @param[out] margin Nejmenší vzdálenost bitu od prahu v us.
 */
static void framed_byte ( const st_WAV_FM_DECODER *dec, uint32_t p, uint8_t *v, double *margin ) {
    uint8_t x = 0;
    double m = 1e18;
    for ( uint32_t b = 0; b < 8; b++ ) {
        double a = pair_avg ( dec, p + 2 * b );
        x = ( uint8_t ) ( ( x << 1 ) | ( a >= dec->threshold_us ) );
        double d = fabs ( a - dec->threshold_us );
        if ( d < m ) m = d;
    }
    *v = x;
    *margin = m;
}


/**
 * @brief Zjistí, zda je v úseku půlvln mezera (výpadek signálu).
 * @param dec Dekodér. @param from Začátek úseku. @param len Délka úseku.
 * @return 1 = mezera v úseku je, 0 = není.
 */
static int section_has_gap ( const st_WAV_FM_DECODER *dec, uint32_t from, uint32_t len ) {
    for ( uint32_t i = from; i < from + len && i < dec->seq->count; i++ ) {
        if ( dec->seq->pulses[i].duration_us >= dec->threshold_us * FM_DATA_END_GAP_FACTOR ) return 1;
    }
    return 0;
}


/**
 * @brief Přečte kopii z obou konců podle rámce start bitů.
 *
 * Zepředu se čte, dokud sedí start bity; ojedinělý špatný start bit je chyba bitu, víc
 * jich znamená ztrátu rámce. Zezadu totéž od konce sekce. Bajty těsně před místem ztráty
 * se nepoužijí (FM_MERGE_BACKOFF). Mezera v bajtu znamená ztrátu rámce, i když start bity
 * dál sedí (výpadek o celé bajty). Sekce s jinou délkou, než odpovídá n, se čte jen zezadu:
 * posun o celé bajty bez mezery by jinak vydával špatné bajty za platné (sloučení je
 * pak odmítne přes počet rozdílů). Čtení zezadu proběhne, jen když se rámec zepředu ztratil.
 *
 * @param dec Dekodér.
 * @param from První datová půlvlna kopie.
 * @param n Bajtů v sekci (data + součet).
 * @param[out] v Bajty (n). @param[out] ok 1 = bajt je platný (n). @param[out] mg Rezerva bajtu v us (n).
 */
static void read_framed ( const st_WAV_FM_DECODER *dec, uint32_t from, uint32_t n, uint8_t *v, uint8_t *ok, double *mg ) {
    const uint32_t count = dec->seq->count;
    memset ( ok, 0, n );
    if ( from >= count ) return;

    /* zepředu: start bit bajtu k leží na from + 18k - 2 */
    uint32_t lost = n;
    for ( uint32_t k = 1; k < n; k++ ) {
        uint32_t p = from + FM_BYTE_HALVES * k - 2;
        if ( p + 1 >= count || section_has_gap ( dec, p > FM_BYTE_HALVES ? p - FM_BYTE_HALVES : from, FM_BYTE_HALVES ) ) {
            lost = k;
            break;
        }
        if ( pair_avg ( dec, p ) < dec->threshold_us ) {
            int hits = 0;
            for ( uint32_t j = 1; j <= FM_MERGE_LOOKAHEAD && p + FM_BYTE_HALVES * j + 1 < count; j++ ) {
                hits += ( pair_avg ( dec, p + FM_BYTE_HALVES * j ) >= dec->threshold_us );
            }
            if ( hits < FM_MERGE_LOOKAHEAD_HITS ) {
                lost = k;
                break;
            }
        }
    }
    /* rámec zepředu drží, ale sekce má jinou délku: posun o celé bajty beze stopy */
    uint32_t end_run = data_section_end ( dec, from, 0 );
    if ( lost == n && end_run != from + FM_BYTE_HALVES * n ) lost = 0;
    uint32_t good = ( lost == n ) ? n : ( lost > FM_MERGE_BACKOFF ? lost - FM_MERGE_BACKOFF : 0 );
    for ( uint32_t k = 0; k < good; k++ ) {
        uint32_t p = from + FM_BYTE_HALVES * k;
        if ( p + 16 > count ) break;
        framed_byte ( dec, p, &v[k], &mg[k] );
        ok[k] = 1;
    }
    if ( lost == n ) return;

    /* zezadu: sekce včetně koncového L má (n) x 18 půlvln od první datové */
    uint32_t e = data_section_end ( dec, from, 0 );
    /*
     * Končí-li sekce mezerou, koncová půlvlna buď splynula s tichem (mezera patří k sekci),
     * nebo je ticho samostatná půlvlna navíc. Rozhodnou start bity posledních bajtů.
     */
    if ( e > from + FM_BYTE_HALVES * ( FM_MERGE_LOOKAHEAD + 2 ) && e <= count
         && dec->seq->pulses[e - 1].duration_us >= dec->threshold_us * FM_DATA_END_GAP_FACTOR ) {
        int with_gap = 0, without_gap = 0;
        for ( uint32_t j = 1; j <= FM_MERGE_LOOKAHEAD; j++ ) {
            with_gap += ( pair_avg ( dec, e - FM_BYTE_HALVES * j - 2 ) >= dec->threshold_us );
            without_gap += ( pair_avg ( dec, e - 1 - FM_BYTE_HALVES * j - 2 ) >= dec->threshold_us );
        }
        if ( without_gap > with_gap ) e--;
    }
    if ( e < from + FM_BYTE_HALVES || e < FM_BYTE_HALVES * n || pair_avg ( dec, e - 2 ) < dec->threshold_us ) return;
    uint32_t first = e - FM_BYTE_HALVES * n;   /* poloha bajtu 0 podle konce */
    uint32_t back = 0;                         /* index, kde se rámec zezadu ztratil; 0 = neztratil */
    for ( uint32_t k = n - 1; k >= 1; k-- ) {
        uint32_t p = first + FM_BYTE_HALVES * k - 2;
        if ( p < from || section_has_gap ( dec, p, FM_BYTE_HALVES ) ) {
            back = k;
            break;
        }
        if ( pair_avg ( dec, p ) < dec->threshold_us ) {
            int hits = 0;
            for ( uint32_t j = 1; j <= FM_MERGE_LOOKAHEAD && p >= from + FM_BYTE_HALVES * j; j++ ) {
                hits += ( pair_avg ( dec, p - FM_BYTE_HALVES * j ) >= dec->threshold_us );
            }
            if ( hits < FM_MERGE_LOOKAHEAD_HITS ) {
                back = k;
                break;
            }
        }
    }
    uint32_t start = back ? back + FM_MERGE_BACKOFF : 0;
    for ( uint32_t k = start; k < n; k++ ) {
        uint32_t p = first + FM_BYTE_HALVES * k;
        if ( ok[k] || p < from || p + 16 > count ) continue;
        framed_byte ( dec, p, &v[k], &mg[k] );
        ok[k] = 1;
    }
}


int wav_decode_fm_merge_copies (
    const st_WAV_FM_DECODER *dec,
    uint32_t from1,
    uint32_t from2,
    uint32_t size,
    uint8_t *out,
    uint32_t *out_from_copy
) {
    if ( !dec || !out || size == 0 ) return 0;
    uint32_t n = size + 2;
    uint8_t *mem = ( uint8_t* ) malloc ( 4 * ( size_t ) n + 2 * ( size_t ) n * sizeof ( double ) + 2 * ( size_t ) n );
    if ( !mem ) return 0;
    uint8_t *a = mem, *b = a + n, *oa = b + n, *ob = oa + n, *base = ob + n, *alt = base + n;
    double *ma = ( double* ) calloc ( 2 * ( size_t ) n, sizeof ( double ) );
    if ( !ma ) {
        free ( mem );
        return 0;
    }
    double *mb = ma + n;
    int accepted = 0;
    uint32_t diff[FM_MERGE_MAX_DIFF];
    uint32_t k = 0;
    int doubt_us_ok = 1;
    double doubt = dec->threshold_us * FM_MERGE_DOUBT;

    read_framed ( dec, from1, n, a, oa, ma );
    read_framed ( dec, from2, n, b, ob, mb );

    int cks[2], ncks = 0;
    if ( oa[size] && oa[size + 1] ) cks[ncks++] = ( a[size] << 8 ) | a[size + 1];
    if ( ob[size] && ob[size + 1] ) {
        int c = ( b[size] << 8 ) | b[size + 1];
        if ( !ncks || c != cks[0] ) cks[ncks++] = c;
    }
    if ( !ncks ) doubt_us_ok = 0;

    for ( uint32_t i = 0; i < size && doubt_us_ok; i++ ) {
        if ( !oa[i] && !ob[i] ) {
            doubt_us_ok = 0;
            break;
        }
        base[i] = oa[i] ? a[i] : b[i];
        alt[i] = ob[i] ? b[i] : a[i];
        if ( oa[i] && ob[i] && a[i] != b[i] ) {
            int da = ma[i] < doubt, db = mb[i] < doubt;
            if ( da != db ) {
                base[i] = alt[i] = da ? b[i] : a[i];   /* pochybná kopie ustoupí čisté */
                continue;
            }
            if ( k == FM_MERGE_MAX_DIFF ) {
                doubt_us_ok = 0;
                break;
            }
            diff[k++] = i;
        }
    }

    if ( doubt_us_ok ) {
        int fixed = 0;
        for ( uint32_t i = 0; i < size; i++ ) fixed += bits_set ( base[i] );
        int found = 0;
        uint32_t best = 0;
        for ( uint32_t m = 0; m < ( 1U << k ); m++ ) {
            int s = fixed;
            for ( uint32_t i = 0; i < k; i++ ) {
                if ( m & ( 1U << i ) ) s += bits_set ( alt[diff[i]] ) - bits_set ( base[diff[i]] );
            }
            for ( int c = 0; c < ncks; c++ ) {
                if ( s == cks[c] ) {
                    found++;
                    best = m;
                    break;
                }
            }
        }
        if ( found == 1 ) {
            for ( uint32_t i = 0; i < k; i++ ) {
                if ( best & ( 1U << i ) ) base[diff[i]] = alt[diff[i]];
            }
            uint32_t from_copy = 0;
            for ( uint32_t i = 0; i < size; i++ ) from_copy += ( !oa[i] || base[i] != a[i] );
            memcpy ( out, base, size );
            if ( out_from_copy ) *out_from_copy = from_copy;
            accepted = 1;
        }
    }

    free ( ma );
    free ( mem );
    return accepted;
}


en_WAV_ANALYZER_ERROR wav_decode_fm_read_body (
    st_WAV_FM_DECODER *dec,
    uint8_t *data,
    uint32_t size,
    st_WAV_BODY_RECOVERY *rec,
    en_WAV_CRC_STATUS *crc_status,
    uint16_t *crc_stored,
    uint16_t *crc_computed,
    uint32_t *out_end
) {
    if ( !dec || !data || !crc_status || !crc_stored || !crc_computed || !out_end ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    int recover = rec && rec->enable;
    uint32_t start = dec->pos;
    uint32_t got = 0;
    int partial = 0;
    *crc_stored = 0;
    *crc_computed = 0;
    *crc_status = WAV_CRC_ERROR;

    dec->crc_accumulator = 0;
    en_WAV_ANALYZER_ERROR err = wav_decode_fm_read_block_n ( dec, data, size, &got );
    if ( err != WAV_ANALYZER_OK ) {
        if ( !recover || got == 0 ) return err;
        memset ( &data[got], 0, size - got );
        partial = 1;
    } else {
        wav_decode_fm_verify_checksum ( dec, crc_status, crc_stored, crc_computed );
        if ( *crc_status == WAV_CRC_NOT_AVAILABLE && recover && size > 0 ) {
            *crc_status = WAV_CRC_ERROR;
            partial = 1;
        }
    }
    *out_end = dec->pos;

    /* po dvojicích půlvln: nesouměrné pulzy rychlých záznamů */
    if ( recover && size > 0 && *crc_status == WAV_CRC_ERROR ) {
        uint32_t limit = start + FM_BYTE_HALVES * size;
        uint32_t run = 0;
        for ( uint32_t i = start; i + 1 < dec->seq->count && i < limit + FM_DATA_END_SHORT_RUN; i += 2 ) {
            if ( pair_avg ( dec, i ) >= dec->threshold_us ) {
                run = 0;
            } else if ( ( run += 2 ) >= FM_DATA_END_SHORT_RUN ) {
                uint32_t pos = i + 2 - run;
                if ( pos >= limit ) break;
                got = ( pos - start ) / FM_BYTE_HALVES;
                if ( got == 0 ) return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
                memset ( &data[got], 0, size - got );
                *out_end = pos;
                partial = 1;
                break;
            }
        }
    }

    if ( partial ) {
        *crc_stored = 0;
        *crc_computed = 0;
        rec->status |= WAV_RECOVERY_PARTIAL_BODY;
        rec->read_bytes = got;
        rec->expected_bytes = size;
    }
    return WAV_ANALYZER_OK;
}


/**
 * @brief Zjistí, zda n bajtů od dané pozice má platný checksum.
 *
 * @param dec Dekodér (nemění se).
 * @param from První datová půlvlna.
 * @param size Počet bajtů.
 * @return 1 = checksum souhlasí, 0 = nesouhlasí nebo data dojdou.
 */
static int probe_block_crc ( const st_WAV_FM_DECODER *dec, uint32_t from, uint32_t size ) {
    uint8_t *probe = ( uint8_t* ) malloc ( size );
    if ( !probe ) return 0;
    st_WAV_FM_DECODER p = *dec;
    en_WAV_CRC_STATUS st = WAV_CRC_ERROR;
    p.pos = from;
    p.crc_accumulator = 0;
    if ( wav_decode_fm_read_block ( &p, probe, size ) == WAV_ANALYZER_OK ) {
        wav_decode_fm_verify_checksum ( &p, &st, NULL, NULL );
    }
    free ( probe );
    return st == WAV_CRC_OK;
}


en_WAV_ANALYZER_ERROR wav_decode_fm_decode_mzf (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_LEADER_INFO *leader,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_header_result,
    st_WAV_DECODE_RESULT *out_body_result
) {
    return wav_decode_fm_decode_mzf_ex ( seq, leader, NULL, out_mzf, out_header_result, out_body_result, NULL );
}


en_WAV_ANALYZER_ERROR wav_decode_fm_decode_mzf_ex (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_LEADER_INFO *leader,
    const st_WAV_FM_DECODE_OPTIONS *opts,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_header_result,
    st_WAV_DECODE_RESULT *out_body_result,
    st_WAV_FM_DECODE_INFO *out_info
) {
    if ( !seq || !leader || !out_mzf ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    *out_mzf = NULL;

    /* informace o průběhu se plní vždy, volající ji může vynechat */
    st_WAV_FM_DECODE_INFO local_info;
    st_WAV_FM_DECODE_INFO *info = out_info ? out_info : &local_info;
    memset ( info, 0, sizeof ( *info ) );
    int recover_body = opts && opts->recover_body;
    int recover_header = opts && opts->recover_header;

    /* inicializujeme dekodér za koncem leader tónu */
    st_WAV_FM_DECODER dec;
    uint32_t data_start = leader->start_index + leader->pulse_count;
    wav_decode_fm_init ( &dec, seq, data_start, leader->avg_period_us );

    /* === 1. Detekce tapemarku hlavicky (LTM) === */
    en_WAV_ANALYZER_ERROR err = wav_decode_fm_find_tapemark ( &dec, WAV_TAPEMARK_LONG );
    int sync_done = 0;
    if ( err != WAV_ANALYZER_OK ) {
        /*
         * Nesouměrné půlvlny (generátor Intercopy NORMAL 2800/3200 podle kódu programu:
         * dlouhá půlvlna 7 a 10 vzorků při 44,1 kHz) se po kvantizaci dostanou pod práh
         * po půlvlnách; podle celých period se tapemark najde. Synchronizaci přeskočí.
         */
        uint32_t found = 0;
        if ( find_tapemark_period_phase ( &dec, data_start, TAPEMARK_LONG_MIN_LTM, TAPEMARK_SHORT_MIN_LTM, 0,
                                          MZF_HEADER_SIZE, &found ) != WAV_ANALYZER_OK ) {
            return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
        }
        dec.pos = found;
        sync_done = 1;
    }

    /*
     * === 2. Sync blok (2L = max 4 LONG pul-periody) ===
     * Po tapemarku preskocime LONG pul-periody (sync/tapemark koncovka).
     */
    if ( !sync_done ) {
        int sync_skip = 0;
        while ( dec.pos < seq->count && is_long_pulse ( &dec, dec.pos ) && sync_skip < 4 ) {
            dec.pos++;
            sync_skip++;
        }
    }

    /* === 3. Dekódování 128B hlavičky === */
    uint32_t header_pulse_start = dec.pos;
    uint8_t header_raw[MZF_HEADER_SIZE];
    dec.crc_accumulator = 0;

    err = wav_decode_fm_read_block ( &dec, header_raw, MZF_HEADER_SIZE );
    if ( err != WAV_ANALYZER_OK ) {
        info->body_error = err;
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    /* === 4. Verifikace CRC hlavičky === */
    en_WAV_CRC_STATUS hdr_crc_status;
    uint16_t hdr_crc_stored = 0, hdr_crc_computed = 0;
    err = wav_decode_fm_verify_checksum ( &dec, &hdr_crc_status, &hdr_crc_stored, &hdr_crc_computed );

    if ( hdr_crc_status == WAV_CRC_ERROR ) {
        fprintf ( stderr, "WARNING: header CRC mismatch (stored=0x%04X, computed=0x%04X)\n",
                  hdr_crc_stored, hdr_crc_computed );
    }

    /*
     * === 4b. Copy2 - druhá kopie hlavičky při CRC chybě ===
     * Hledá se od začátku 1. kopie (wav_decode_fm_try_copy2).
     */
    int hdr_copy2_used = 0;

    if ( hdr_crc_status == WAV_CRC_ERROR ) {
        uint8_t *hdr_copy2_data = NULL;
        en_WAV_CRC_STATUS hdr_c2_crc;
        uint16_t hdr_c2_stored = 0, hdr_c2_computed = 0;
        uint32_t hdr_c2_end = 0;

        en_WAV_ANALYZER_ERROR c2err = wav_decode_fm_try_copy2 (
            seq, header_pulse_start, dec.threshold_us, MZF_HEADER_SIZE,
            &hdr_copy2_data, &hdr_c2_crc, &hdr_c2_stored, &hdr_c2_computed, &hdr_c2_end
        );

        if ( c2err == WAV_ANALYZER_OK && hdr_c2_crc == WAV_CRC_OK ) {
            memcpy ( header_raw, hdr_copy2_data, MZF_HEADER_SIZE );
            hdr_crc_status = WAV_CRC_OK;
            hdr_crc_stored = hdr_c2_stored;
            hdr_crc_computed = hdr_c2_computed;
            hdr_copy2_used = 1;
            dec.pos = hdr_c2_end;
        }

        free ( hdr_copy2_data );
    }

    if ( hdr_crc_status == WAV_CRC_OK && !hdr_copy2_used ) {
        info->header_copy_diff = count_copy_diff ( &dec, header_pulse_start, header_raw, MZF_HEADER_SIZE );
        if ( info->header_copy_diff ) {
            fprintf ( stderr, "WARNING: header copies differ although the checksum is valid (%u bytes)\n",
                      info->header_copy_diff );
        }
    }

    if ( out_header_result ) {
        out_header_result->format = WAV_TAPE_FORMAT_NORMAL;
        out_header_result->data = ( uint8_t* ) malloc ( MZF_HEADER_SIZE );
        if ( out_header_result->data ) {
            memcpy ( out_header_result->data, header_raw, MZF_HEADER_SIZE );
        }
        out_header_result->data_size = MZF_HEADER_SIZE;
        out_header_result->crc_status = hdr_crc_status;
        out_header_result->crc_stored = hdr_crc_stored;
        out_header_result->crc_computed = hdr_crc_computed;
        out_header_result->pulse_start = header_pulse_start;
        out_header_result->pulse_end = dec.pos;
        out_header_result->is_header = 1;
        out_header_result->copy2_used = hdr_copy2_used;
    }

    /* parsujeme hlavičku */
    st_MZF_HEADER mzf_header;
    memcpy ( &mzf_header, header_raw, MZF_HEADER_SIZE );
    mzf_header_swap_endianity ( &mzf_header );   /* 11h (fname.terminator) zůstává podle pásky */

    uint16_t body_size = mzf_header.fsize;

    info->header_decoded = 1;
    info->header_crc_ok = ( hdr_crc_status == WAV_CRC_OK );
    info->expected_bytes = body_size;
    memcpy ( info->header_raw, header_raw, MZF_HEADER_SIZE );

    /*
     * Deklarace před prvním skokem na create_mzf_no_body - goto nesmí
     * přeskočit inicializaci (dříve zde vznikal free() neurčeného ukazatele).
     */
    uint8_t *body_data = NULL;
    uint32_t body_pulse_start = 0;

    /* soubor bez těla: CRC těla se neověřuje (volající výsledek nuluje, 0 = WAV_CRC_OK) */
    if ( body_size == 0 && out_body_result ) {
        out_body_result->crc_status = WAV_CRC_NOT_AVAILABLE;
    }

    /*
     * === 5. Hledání body leaderu ===
     * Mezi hlavičkou a tělem je krátký GAP (256 SHORT + kopie hlavičky
     * v plném formátu, nebo přímo krátký leader).
     *
     * Hledáme další leader nebo tapemark.
     * Pokud je plný formát (s kopiemi), přeskočíme kopii hlavičky.
     */

    /*
     * Přeskočíme případné LONG pulzy (2L) a krátký blok (256S).
     * Pak hledáme kratký tapemark (STM).
     * Pokud je tam kopie hlavičky, přeskočíme ji hledáním dalšího leaderu.
     */

    /* hledáme krátký tapemark (STM) */
    uint32_t stm_from = dec.pos;
    err = find_tapemark_bounded ( &dec, WAV_TAPEMARK_SHORT, 2 * BODY_STM_MAX_LONG );
    sync_done = 0;
    if ( err != WAV_ANALYZER_OK ) {
        /*
         * Podle celých period (nesouměrné půlvlny, viz LTM). Delší běh než
         * BODY_STM_MAX_LONG period je LTM další hlavičky, ne tapemark těla.
         */
        uint32_t found = 0;
        err = find_tapemark_period_phase ( &dec, stm_from, TAPEMARK_LONG_MIN_STM, TAPEMARK_SHORT_MIN_STM,
                                           BODY_STM_MAX_LONG, body_size, &found );
        if ( err == WAV_ANALYZER_OK ) {
            dec.pos = found;
            sync_done = 1;
        } else {
            dec.pos = stm_from;
        }
    }
    if ( err != WAV_ANALYZER_OK ) {
        /*
         * Pokud nenajdeme krátký tapemark, zkusíme najít leader.
         * To se stane u formátu s kopií, kde je mezi tapemarks
         * kopie hlavičky.
         */
        st_WAV_LEADER_INFO body_leader;
        err = wav_leader_detect ( seq, dec.pos, BODY_LEADER_MIN_PULSES,
                                  WAV_ANALYZER_DEFAULT_TOLERANCE, &body_leader );
        if ( err == WAV_ANALYZER_OK ) {
            dec.pos = body_leader.start_index + body_leader.pulse_count;
            /* rekalibrujeme práh z body leaderu */
            dec.threshold_us = body_leader.avg_period_us * WAV_ANALYZER_FM_THRESHOLD_FACTOR;
            err = find_tapemark_bounded ( &dec, WAV_TAPEMARK_SHORT, 2 * BODY_STM_MAX_LONG );
        }

        if ( err != WAV_ANALYZER_OK ) {
            /* stále nenalezen - nemáme tělo */
            if ( body_size == 0 ) {
                /* MZF bez těla - to je OK */
                goto create_mzf_no_body;
            }
            info->body_error = WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
            if ( recover_header && hdr_crc_status == WAV_CRC_OK ) {
                goto keep_header_only;
            }
            return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
        }
    }

    /* === 6. Sync blok za tapemarkem těla (2L = max 4 půl-periody) === */
    if ( !sync_done ) {
        int sync_skip = 0;
        while ( dec.pos < seq->count && is_long_pulse ( &dec, dec.pos ) && sync_skip < 4 ) {
            dec.pos++;
            sync_skip++;
        }
    }

    /* === 7. Dekódování těla === */
    body_pulse_start = dec.pos;

    /*
     * Hlavička s chybou CRC (bez platné kopie) může mít zničené fsize. Délku dat
     * dá signál; přijme se jen ta, se kterou souhlasí checksum těla a kdy
     * fsize z hlavičky checksum nesplní.
     */
    if ( hdr_crc_status == WAV_CRC_ERROR ) {
        uint32_t est = 0;
        int frame_ok = 0;
        if ( wav_decode_fm_estimate_body_bytes ( &dec, body_pulse_start, &est, &frame_ok ) == WAV_ANALYZER_OK
             && frame_ok && est > 0 && est <= 0xFFFF && est != body_size
             && !( body_size > 0 && probe_block_crc ( &dec, body_pulse_start, body_size ) )
             && probe_block_crc ( &dec, body_pulse_start, est ) ) {
            fprintf ( stderr, "WARNING: header fsize %u does not match body length on tape (%u bytes), using tape length\n",
                      body_size, est );
            body_size = ( uint16_t ) est;
            mzf_header.fsize = body_size;
            info->expected_bytes = body_size;
            info->recovery_status |= WAV_RECOVERY_FSIZE_REPAIRED;
        }
    }

    if ( body_size > 0 ) {
        body_data = ( uint8_t* ) malloc ( body_size );
        if ( !body_data ) return WAV_ANALYZER_ERROR_ALLOC;

        dec.crc_accumulator = 0;
        uint32_t body_read = 0;
        err = wav_decode_fm_read_block_n ( &dec, body_data, body_size, &body_read );
        info->recovered_bytes = body_read;
        int body_partial = 0;
        if ( err != WAV_ANALYZER_OK ) {
            info->body_error = err;
            if ( body_read == 0 && recover_header && hdr_crc_status == WAV_CRC_OK ) {
                goto keep_header_only;
            }
            if ( !recover_body || body_read == 0 ) {
                free ( body_data );
                return WAV_ANALYZER_ERROR_DECODE_DATA;
            }
            /* konec signálu uvnitř těla: přečtené bajty zůstanou, zbytek 00h */
            memset ( &body_data[body_read], 0, body_size - body_read );
            body_partial = 1;
        }

        /* === 8. Verifikace CRC těla === */
        en_WAV_CRC_STATUS body_crc_status = WAV_CRC_ERROR;
        uint16_t body_crc_stored = 0, body_crc_computed = 0;
        if ( !body_partial ) {
            wav_decode_fm_verify_checksum ( &dec, &body_crc_status, &body_crc_stored, &body_crc_computed );
            if ( body_crc_status == WAV_CRC_NOT_AVAILABLE && recover_body ) {
                /* data celá, signál skončil v checksumu: bez ověření nejde o úspěch */
                body_crc_status = WAV_CRC_ERROR;
                body_partial = 1;
            }
        }

        if ( body_crc_status == WAV_CRC_ERROR && !body_partial ) {
            fprintf ( stderr, "WARNING: body CRC mismatch (stored=0x%04X, computed=0x%04X)\n",
                      body_crc_stored, body_crc_computed );
        }

        /*
         * === 8b. Copy2 - druhá kopie těla ===
         * ROM zapisuje kopii bez tónu a tapemarku (wav_decode_fm_try_copy2). Při CRC
         * chybě 1. kopie se použije 2. kopie s platným checksumem. Konec 2. kopie
         * se hledá vždy (i s chybou CRC), aby se oddělovač nehlásil jako leader.
         */
        int body_copy2_used = 0;
        uint32_t body_consumed_end = dec.pos;

        if ( !body_partial ) {
            uint8_t *copy2_data = NULL;
            en_WAV_CRC_STATUS copy2_crc;
            uint16_t copy2_stored = 0, copy2_computed = 0;
            uint32_t copy2_end = 0;

            en_WAV_ANALYZER_ERROR c2err = wav_decode_fm_try_copy2 (
                seq, body_pulse_start, dec.threshold_us, body_size,
                &copy2_data, &copy2_crc, &copy2_stored, &copy2_computed, &copy2_end
            );

            if ( c2err == WAV_ANALYZER_OK ) {
                /* aktualizujeme consumed pozici za druhou kopii */
                body_consumed_end = copy2_end;

                if ( body_crc_status == WAV_CRC_ERROR && copy2_crc == WAV_CRC_OK ) {
                    /* nahradíme data z první kopie daty z druhé */
                    free ( body_data );
                    body_data = copy2_data;
                    copy2_data = NULL;
                    body_crc_status = WAV_CRC_OK;
                    body_crc_stored = copy2_stored;
                    body_crc_computed = copy2_computed;
                    body_copy2_used = 1;
                }
            }
            free ( copy2_data );
        }

        /*
         * Obě kopie poškozené: složení z platných úseků obou kopií. Přijme se
         * jen výsledek, který jednoznačně souhlasí se součtem z pásky.
         */
        int body_merged = 0;
        if ( recover_body && !body_partial && body_crc_status == WAV_CRC_ERROR ) {
            uint32_t copy_start = 0, from_copy = 0;
            if ( wav_decode_fm_find_copy_start ( seq, body_pulse_start, dec.threshold_us, body_size, &copy_start )
                 && wav_decode_fm_merge_copies ( &dec, body_pulse_start, copy_start, body_size, body_data, &from_copy ) ) {
                uint16_t sum = 0;
                for ( uint32_t i = 0; i < body_size; i++ ) sum = ( uint16_t ) ( sum + bits_set ( body_data[i] ) );
                body_crc_status = WAV_CRC_OK;
                body_crc_stored = sum;
                body_crc_computed = sum;
                body_merged = 1;
                body_consumed_end = data_section_end ( &dec, copy_start, 0 );
                info->merged_bytes = from_copy;
                info->recovery_status |= WAV_RECOVERY_COPY_MERGED;
                info->body_error = WAV_ANALYZER_OK;
            }
        }

        /*
         * Tělo přemazané dalším záznamem: čtení běželo přes jeho tón (do fsize, nebo do
         * konce signálu). Dlouhý běh SHORT uvnitř dat je konec skutečných dat
         * a tón zůstane dalšímu záznamu.
         */
        if ( recover_body && !body_merged && ( body_crc_status == WAV_CRC_ERROR || !body_copy2_used ) ) {
            uint32_t run_pos = 0;
            uint32_t data_end = body_pulse_start + ( uint32_t ) body_size * FM_BYTE_HALVES;
            /*
             * Samé SHORT hned od začátku dají nulová data se shodným nulovým součtem,
             * proto se začátek kontroluje i při CRC OK; jinde je CRC OK důkazem dat.
             */
            uint32_t scan_to = ( body_crc_status == WAV_CRC_ERROR ) ? data_end : body_pulse_start + 1;
            uint32_t sep_copy = 0;
            /* běh před 2. kopií je její oddělovač, ne tón dalšího záznamu (kopie se jen zkrátila výpadkem) */
            if ( find_short_run ( &dec, body_pulse_start, scan_to, &run_pos ) && run_pos < data_end
                 && !( wav_decode_fm_find_copy_start ( seq, body_pulse_start, dec.threshold_us, body_size, &sep_copy )
                       && run_pos < sep_copy ) ) {
                body_read = ( run_pos - body_pulse_start ) / FM_BYTE_HALVES;
                if ( body_read == 0 ) {
                    /* za tapemarkem žádná data: není to částečné tělo */
                    info->body_error = WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
                    info->recovered_bytes = 0;
                    if ( recover_header && hdr_crc_status == WAV_CRC_OK ) {
                        goto keep_header_only;
                    }
                    free ( body_data );
                    return WAV_ANALYZER_ERROR_DECODE_DATA;
                }
                memset ( &body_data[body_read], 0, body_size - body_read );
                info->recovered_bytes = body_read;
                body_consumed_end = run_pos;
                body_partial = 1;
            }
        }

        if ( body_partial ) {
            info->recovery_status |= WAV_RECOVERY_PARTIAL_BODY;
        }

        if ( body_crc_status == WAV_CRC_OK && !body_copy2_used && !body_partial && !body_merged ) {
            info->body_copy_diff = count_copy_diff ( &dec, body_pulse_start, body_data, body_size );
            if ( info->body_copy_diff ) {
                fprintf ( stderr, "WARNING: body copies differ although the checksum is valid (%u bytes)\n",
                          info->body_copy_diff );
            }
        }

        if ( out_body_result ) {
            out_body_result->format = WAV_TAPE_FORMAT_NORMAL;
            out_body_result->data = ( uint8_t* ) malloc ( body_size );
            if ( out_body_result->data ) {
                memcpy ( out_body_result->data, body_data, body_size );
            }
            out_body_result->data_size = body_size;
            out_body_result->crc_status = body_crc_status;
            out_body_result->crc_stored = body_crc_stored;
            out_body_result->crc_computed = body_crc_computed;
            out_body_result->pulse_start = body_pulse_start;
            out_body_result->pulse_end = body_consumed_end;
            out_body_result->is_header = 0;
            out_body_result->copy2_used = body_copy2_used;
        }
    }

    goto create_mzf_no_body;

    /*
     * Obnova hlavičky bez těla: hlavička má platný CRC, tělo na pásce není.
     * Tělo má fsize nul, aby soubor šel uložit a načíst.
     */
keep_header_only:
    free ( body_data );
    body_data = ( uint8_t* ) calloc ( body_size, 1 );
    if ( !body_data ) return WAV_ANALYZER_ERROR_ALLOC;
    info->recovery_status |= WAV_RECOVERY_HEADER_ONLY;
    info->recovered_bytes = 0;
    if ( out_body_result ) {
        out_body_result->format = WAV_TAPE_FORMAT_NORMAL;
        out_body_result->crc_status = WAV_CRC_NOT_AVAILABLE;
        out_body_result->pulse_end = stm_from;
        out_body_result->is_header = 0;
    }

    /* === 9. Sestavíme MZF strukturu === */
create_mzf_no_body:
    ;

    st_MZF *mzf = ( st_MZF* ) calloc ( 1, sizeof ( st_MZF ) );
    if ( !mzf ) {
        free ( body_data );
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    memcpy ( &mzf->header, &mzf_header, sizeof ( st_MZF_HEADER ) );
    mzf->body = body_data;
    mzf->body_size = body_size;

    *out_mzf = mzf;
    return WAV_ANALYZER_OK;
}
