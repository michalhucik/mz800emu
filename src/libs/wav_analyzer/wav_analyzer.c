/**
 * @file   wav_analyzer.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.15.0
 * @brief  Implementace hlavního API knihovny wav_analyzer.
 *
 * Orchestruje všechny vrstvy analyzéru:
 * preprocessing -> pulse extraction -> leader detection ->
 * histogram analysis -> format classification -> decoding.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/wav/wav.h"
#include "libs/mzf/mzf.h"
#include "libs/endianity/endianity.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "wav_analyzer.h"


/** @brief Počáteční kapacita pole výsledků souborů. */
#define FILE_RESULT_INITIAL_CAPACITY    8

/**
 * @brief Minimální počet pulzů pro histogramovou analýzu datového úseku.
 *
 * Pokud je datový úsek za leaderem kratší, přeskočíme histogram.
 */
#define HISTOGRAM_MIN_PULSES    100

/** @brief Referenční SHORT půl-perioda NORMAL FM při 1200 Bd (us).
 *
 * Průměrná půlvlna SHORT ROM MZ-800 ((844 + 920) T / 2, asi 249 us)
 * při standardní rychlosti 1:1.
 * Používá se pro výpočet reálné rychlosti v Bd z naměřené
 * průměrné půl-periody leaderu.
 */
#define NORMAL_1200BD_SHORT_US  249.0


/**
 * @brief Vypočítá absolutní čas odpovídající danému indexu pulzu v sekvenci.
 *
 * Sečte délky všech pulzů od začátku sekvence po daný index
 * a vrátí výsledek v sekundách. Pro index 0 vrací 0.0.
 * Pro index >= seq->count vrací celkový čas všech pulzů.
 *
 * @param seq Sekvence pulzů. Nesmí být NULL.
 * @param index Index pulzu (0-based, exclusive - čas "před" tímto pulzem).
 * @return Absolutní čas v sekundách od začátku sekvence.
 *
 * @note Složitost O(index) - pro opakované volání s rostoucími indexy
 *       je efektivnější kumulativní přístup.
 */
static double pulse_index_to_sec ( const st_WAV_PULSE_SEQUENCE *seq, uint32_t index ) {
    double time = 0.0;
    uint32_t limit = ( index < seq->count ) ? index : seq->count;
    for ( uint32_t p = 0; p < limit; p++ ) {
        time += seq->pulses[p].duration_us;
    }
    return time / 1000000.0;
}


/**
 * @brief Přidá výsledek souboru do pole výsledků.
 *
 * @param result Celkový výsledek analýzy.
 * @param file Výsledek jednoho souboru k přidání.
 * @return WAV_ANALYZER_OK při úspěchu, WAV_ANALYZER_ERROR_ALLOC při selhání.
 */
static en_WAV_ANALYZER_ERROR result_add_file (
    st_WAV_ANALYZER_RESULT *result,
    const st_WAV_ANALYZER_FILE_RESULT *file
) {
    if ( result->file_count >= result->file_capacity ) {
        uint32_t new_cap = ( result->file_capacity == 0 )
                           ? FILE_RESULT_INITIAL_CAPACITY
                           : result->file_capacity * 2;
        st_WAV_ANALYZER_FILE_RESULT *new_files = ( st_WAV_ANALYZER_FILE_RESULT* ) realloc (
                    result->files, new_cap * sizeof ( st_WAV_ANALYZER_FILE_RESULT )
                );
        if ( !new_files ) return WAV_ANALYZER_ERROR_ALLOC;
        result->files = new_files;
        result->file_capacity = new_cap;
    }

    result->files[result->file_count] = *file;
    result->file_count++;
    return WAV_ANALYZER_OK;
}


/**
 * @brief Minimální počet pulzů pro detekci leaderu Cast 2 (loader body).
 *
 * Cast 2 začíná SGAP (4000-10000 krátkých pulzů). Používáme nižší práh
 * kvůli možné degradaci signálu.
 */
#define NONSTANDARD_LOADER_LEADER_MIN_PULSES    500

/**
 * @brief Offset fsize v loader body (společný pro FSK/SLOW/DIRECT).
 *
 * Uživatelská velikost dat (2B, LE) je na stejném offsetu
 * ve všech třech formátech.
 */
#define LOADER_BODY_OFF_SIZE    5


/**
 * @brief Offsety parametrů v loader body pro jednotlivé formáty.
 *
 * Uživatelská data (fstrt, fexec) jsou na různých offsetech
 * v závislosti na formátu loaderu (FSK/SLOW/DIRECT).
 */
/** @name FSK loader body offsety (191B) */
/** @{ */
#define FSK_LDR_OFF_EXEC       11   /**< offset fexec (2B LE) */
#define FSK_LDR_OFF_FROM       33   /**< offset fstrt (2B LE) */
#define FSK_LDR_OFF_CHK       176   /**< offset XOR těla (1 B), mzcmt_fsk.c FSK_LOADER_OFF_CHECKSUM */
/** @} */

/** @name SLOW loader body offsety (249B) */
/** @{ */
#define SLOW_LDR_OFF_EXEC      12   /**< offset fexec (2B LE) */
#define SLOW_LDR_OFF_FROM      76   /**< offset fstrt (2B LE) */
#define SLOW_LDR_OFF_CHK      231   /**< offset XOR těla (1 B), mzcmt_slow.c SLOW_LOADER_OFF_CHECKSUM */
/** @} */

/** @brief Pilot ZX Spectrum standardní rychlosti: 2168 T při 3,5 MHz (us). */
#define ZX_STD_PILOT_US       ( 2168.0 / 3.5 )
/** @brief Relativní odchylka pilotu, do které jde o standardní rychlost (blok 0x10). */
#define ZX_STD_PILOT_TOL      0.05

/** @name Bajt zpoždění v těle zavaděče FSK a SLOW (určuje úroveň rychlosti) */
/** @{ */
#define FSK_LDR_OFF_DELAY     145   /**< mzcmt_fsk.c FSK_LOADER_OFF_DELAY */
#define SLOW_LDR_OFF_DELAY    238   /**< mzcmt_slow.c SLOW_LOADER_OFF_DELAY */
/** @} */

/**
 * @brief Bajty zpoždění zavaděče FSK podle úrovně 0-6 (44100 / 48000 Hz).
 *
 * Shodné s tabulkami kodéru (mzcmt_fsk.c g_fsk_delay_44k/48k, odvozené
 * z mzftools). Pro danou frekvenci jsou hodnoty jedinečné, mezi frekvencemi
 * se překrývají (D0h = 44100 úroveň 0 i 48000 úroveň 1).
 */
static const uint8_t g_fsk_delay[2][7] = {
    { 0xD0, 0xD1, 0xD3, 0xD5, 0xD8, 0xDA, 0xDB },
    { 0xCF, 0xD0, 0xD2, 0xD4, 0xD7, 0xD9, 0xDA },
};

/** @brief Bajty zpoždění zavaděče SLOW podle úrovně 0-4 (44100 / 48000 Hz), mzcmt_slow.c g_slow_delay_*. */
static const uint8_t g_slow_delay[2][5] = {
    { 22, 17, 11, 9, 6 },
    { 20, 15, 10, 8, 5 },
};


/**
 * @brief Určí úroveň rychlosti FSK/SLOW z bajtu zpoždění v těle zavaděče.
 *
 * Kodér TapeMZ volí tabulku podle vzorkovací frekvence: od 46050 Hz tabulku
 * 48000 Hz, jinak 44100 Hz (mzftools zná jen pevné 44100 a 48000 Hz). Stejně
 * se tu vybere podle frekvence WAV. Převzorkovaná nahrávka (zavaděč pro jinou
 * frekvenci, než má WAV) může dát -1, nebo i jinou úroveň: tabulky 44k a 48k
 * se překrývají (např. D0h = 44k úroveň 0 i 48k úroveň 1).
 * Posloupnost pulzů, která nepochází z WAV (wav_analyzer_analyze_pulses(),
 * např. 17 721 600 Hz), dostane tabulku 48 000 Hz; frekvenci, pro kterou
 * zavaděč vznikl, z ní určit nejde. Úroveň je jen informace, dekódovaná data
 * na ní nezávisí.
 *
 * @param loader_body Dekódované tělo zavaděče.
 * @param loader_size Velikost těla v bajtech.
 * @param format WAV_TAPE_FORMAT_FSK nebo WAV_TAPE_FORMAT_SLOW.
 * @param sample_rate Vzorkovací frekvence WAV (Hz).
 * @return Úroveň (FSK 0-6, SLOW 0-4), -1 když bajt chybí nebo v tabulce není
 *         (jiný zavaděč), nebo jiný formát.
 */
static int fsl_speed_from_loader ( const uint8_t *loader_body, uint32_t loader_size,
                                   en_WAV_TAPE_FORMAT format, uint32_t sample_rate ) {
    int r48 = sample_rate >= 46050;
    if ( format == WAV_TAPE_FORMAT_FSK && loader_size > FSK_LDR_OFF_DELAY ) {
        for ( int i = 0; i < 7; i++ ) {
            if ( g_fsk_delay[r48][i] == loader_body[FSK_LDR_OFF_DELAY] ) return i;
        }
    } else if ( format == WAV_TAPE_FORMAT_SLOW && loader_size > SLOW_LDR_OFF_DELAY ) {
        for ( int i = 0; i < 5; i++ ) {
            if ( g_slow_delay[r48][i] == loader_body[SLOW_LDR_OFF_DELAY] ) return i;
        }
    }
    return -1;
}

/** @brief TurboCopy: součin počet pulzů tónu x S (tiky CTC0), změřeno v emulátoru 3 495 030 až 3 497 152. */
#define TC_TONE_PRODUCT       3495500.0
/** @brief TurboCopy: délka tiku CTC0 v us (1 108 405 Hz). */
#define TC_TICK_US            ( 1e6 / 1108405.0 )
/** @brief TurboCopy: nejvyšší odchylka S odhadnutého z počtu pulzů od S(a/b) v tikách. */
#define TC_S_TOL              2.0
/** @brief Intercopy V10.2: počet pulzů SHORT tónu před hlavičkou NORMAL. */
#define IC_TONE_HDR           11000.0
/** @brief Intercopy V10.2: počet pulzů SHORT tónu před tělem (TURBO za předzavaděčem). */
#define IC_TONE_BODY          5500.0
/** @brief Intercopy V10.2: relativní tolerance počtu pulzů tónu (IC_TONE_HDR, IC_TONE_BODY). */
#define IC_TONE_TOL           0.01
/** @brief Tolerance průměrné periody tónu ve vzorcích (WAV kvantuje hrany na vzorky). */
#define WRITER_PERIOD_TOL_SAMPLES 1.5

/**
 * @brief Spodní mez tolerance průměrné periody tónu zapisovače (us).
 *
 * Při vysoké frekvenci (přesné pulzy z emulátoru) by 1,5 vzorku byly
 * desítky ns, méně než odchylka časové základny emulátoru od taktů, ze
 * kterých jsou konstanty (CTC0 1 108 405 Hz, CPU 3 546 895 Hz): při
 * 17 721 600 Hz je takt o 0,073 % pomalejší (17 734 475 / 17 721 600),
 * u nejdelší periody (Intercopy 600 Bd 964 us, TurboCopy S = 1031 tiků
 * 930 us) asi 0,7 us. Mez 2 us to pokryje s rezervou a pod 750 kHz se
 * neuplatní (1,5 vzorku je víc), WAV se proto posuzuje jako dřív.
 * Nejmenší rozdíl period, které se rozlišují, je 18 us (Intercopy 2800
 * a 3200 Bd).
 */
#define WRITER_PERIOD_TOL_MIN_US 2.0


/**
 * @brief Tolerance průměrné periody tónu zapisovače v us pro danou frekvenci.
 *
 * WRITER_PERIOD_TOL_SAMPLES vzorků, nejméně WRITER_PERIOD_TOL_MIN_US.
 *
 * @param sample_rate Vzorkovací frekvence (Hz, > 0).
 * @return Tolerance v us.
 */
static double writer_period_tol_us ( uint32_t sample_rate ) {
    double tol_us = WRITER_PERIOD_TOL_SAMPLES * 1e6 / ( double ) sample_rate;
    return ( tol_us < WRITER_PERIOD_TOL_MIN_US ) ? WRITER_PERIOD_TOL_MIN_US : tol_us;
}

/**
 * @brief Intercopy V10.2: čtecí bod rp a perioda SHORT v T-stavech podle rychlosti.
 *
 * Zdroj: disassembly Intercopy 10.2 a záznamy hran v emulátoru (rp 167/77/32/22/17,
 * SHORT H+L 1664+1755, 832+936, 403+494, 312+442, 273+416 T).
 */
static const struct { uint16_t bd; uint8_t rp; uint16_t short_t; } g_ic_speed[] = {
    { 600, 167, 3419 }, { 1200, 77, 1768 }, { 2400, 32, 897 }, { 2800, 22, 754 }, { 3200, 17, 689 },
};


/**
 * @brief Najde poměr TurboCopy a/b podle délky tónu.
 *
 * TurboCopy V1.2x zapisuje tón tak, že počet pulzů x S je přibližně stálý
 * (TC_TONE_PRODUCT), S = 71 + floor(480·b/a). Z počtu pulzů se odhadne S
 * a vybere poměr (a, b z 1..9, 1/2 <= a/b <= 3, základní tvar) s nejbližším S.
 * Přijme se, když S sedí do TC_S_TOL a průměrná perioda tónu na S tiků
 * v rámci writer_period_tol_us() (1,5 vzorku, nejméně 2 us).
 *
 * @param pulses Počet pulzů tónu (pulz = 2 půlvlny).
 * @param half_us Průměrná půlvlna tónu (us).
 * @param sample_rate Vzorkovací frekvence WAV.
 * @param[out] out_a Čitatel. @param[out] out_b Jmenovatel.
 * @return 1 = tón odpovídá TurboCopy, 0 = ne.
 */
static int tc_ratio_from_tone ( uint32_t pulses, double half_us, uint32_t sample_rate,
                                uint8_t *out_a, uint8_t *out_b ) {
    if ( pulses < 1000 || half_us <= 0.0 || sample_rate == 0 ) return 0;
    double s_est = TC_TONE_PRODUCT / ( double ) pulses;
    double best = 1e9;
    unsigned ba = 0, bb = 0, bs = 0;
    for ( unsigned a = 1; a <= 9; a++ ) {
        for ( unsigned b = 1; b <= 9; b++ ) {
            if ( 2 * a < b || a > 3 * b ) continue;
            unsigned g = a, h = b;
            while ( h ) { unsigned t = g % h; g = h; h = t; }
            if ( g != 1 ) continue;                 /* jen základní tvar */
            unsigned s = 71 + ( 480 * b ) / a;
            double d = fabs ( ( double ) s - s_est );
            if ( d < best ) { best = d; ba = a; bb = b; bs = s; }
        }
    }
    if ( best > TC_S_TOL ) return 0;
    if ( fabs ( 2.0 * half_us - bs * TC_TICK_US ) > writer_period_tol_us ( sample_rate ) ) return 0;
    *out_a = ( uint8_t ) ba;
    *out_b = ( uint8_t ) bb;
    return 1;
}


/**
 * @brief Délka tónu v pulzech od začátku leaderu po první LONG (tapemark).
 *
 * Detektor leaderu může tón s asymetrickými půlvlnami rozdělit na úseky
 * (Intercopy 2400 a 3200 Bd při 44,1 kHz), počet pulzů leaderu pak není délka
 * tónu. Tady se počítají půlvlny od leader->start_index, dokud nepřijde půlvlna
 * delší než 1,5x průměrná půlvlna leaderu.
 *
 * @param seq Sekvence pulzů.
 * @param leader Leader na začátku tónu.
 * @param[out] out_half_us Průměrná půlvlna přes celý tón (us).
 * @return Počet pulzů tónu (půlvlny / 2).
 */
static uint32_t tone_pulses ( const st_WAV_PULSE_SEQUENCE *seq, const st_WAV_LEADER_INFO *leader, double *out_half_us ) {
    double limit = 1.5 * leader->avg_period_us;
    uint32_t i = leader->start_index;
    double sum = 0.0;
    while ( i < seq->count && seq->pulses[i].duration_us <= limit ) {
        sum += seq->pulses[i].duration_us;
        i++;
    }
    uint32_t halves = i - leader->start_index;
    *out_half_us = halves ? sum / halves : 0.0;
    return halves / 2;
}


/**
 * @brief Rychlost Intercopy NORMAL podle tónu hlavičky (11000 pulzů) a periody SHORT.
 *
 * @param pulses Počet pulzů tónu hlavičky.
 * @param half_us Průměrná půlvlna tónu (us).
 * @param sample_rate Vzorkovací frekvence WAV.
 * @return bd (600, 2400, 2800, 3200), 0 = neodpovídá Intercopy. 1200 se nevrací:
 *         perioda Intercopy 1200 (498,5 us) se v rámci tolerance shoduje se záznamem
 *         ROM s krátkými tóny 11000/5500 (518 us), blok 0x40 zůstává.
 */
static uint16_t ic_normal_bd_from_tone ( uint32_t pulses, double half_us, uint32_t sample_rate ) {
    if ( sample_rate == 0 || fabs ( pulses / IC_TONE_HDR - 1.0 ) > IC_TONE_TOL ) return 0;
    /* nejbližší perioda SHORT (2800 a 3200 Bd se liší jen o 18 us), pak tolerance */
    double tol_us = writer_period_tol_us ( sample_rate );
    double best = 1e9;
    uint16_t bd = 0;
    for ( size_t i = 0; i < sizeof ( g_ic_speed ) / sizeof ( g_ic_speed[0] ); i++ ) {
        double d = fabs ( 2.0 * half_us - g_ic_speed[i].short_t / 3.546895 );
        if ( d < best ) { best = d; bd = g_ic_speed[i].bd; }
    }
    if ( best > tol_us || bd == 1200 ) return 0;
    return bd;
}


/**
 * @brief Rychlost Intercopy podle čtecího bodu rp (tabulka g_ic_speed).
 * @param rp Čtecí bod (u TURBO bajt 4Bh předzavaděče - 5, u FAST IPL bajt 19h hlavičky $BB).
 * @return bd, nebo 0 pro hodnotu mimo tabulku.
 */
static uint16_t ic_bd_from_rp ( int rp ) {
    for ( size_t i = 0; i < sizeof ( g_ic_speed ) / sizeof ( g_ic_speed[0] ); i++ ) {
        if ( g_ic_speed[i].rp == rp ) return g_ic_speed[i].bd;
    }
    return 0;
}

/** @name DIRECT loader body offsety (360B) */
/** @{ */
#define DIRECT_LDR_OFF_EXEC    12   /**< offset fexec (2B LE) */
#define DIRECT_LDR_OFF_FROM    34   /**< offset fstrt (2B LE) */
#define DIRECT_LDR_OFF_CHK    342   /**< offset XOR těla (1 B), mzcmt_direct.c DIRECT_LOADER_OFF_CHECKSUM */
/** @} */


/** @brief Délka generické části těla předzavaděče TurboCopy (B). */
#define TURBOCOPY_LOADER_GENERIC_SIZE  75

/**
 * @brief Generická část těla předzavaděče TurboCopy V1.2x (prvních 75 B).
 *
 * Zdroj: cmttool: cmttool_turbo.c:107-118.
 * Intercopy při zápisu patchuje jen offsety 0x4B a 0x4C (za touto částí).
 * Ověřeno na 8 předzavaděčích v tstdata (tc-loader-all, ic-turbo) a na
 * výstupu mzf2tmz.
 */
static const uint8_t g_turbocopy_loader_generic[TURBOCOPY_LOADER_GENERIC_SIZE] = {
    0x3e, 0x08, 0xd3, 0xce, 0xe5, 0x21, 0x00, 0x00, 0xd3, 0xe4, 0x7e, 0xd3, 0xe0, 0x77, 0x23, 0x7c,
    0xfe, 0x10, 0x20, 0xf4, 0x3a, 0x4b, 0xd4, 0x32, 0x4b, 0x0a, 0x3a, 0x4c, 0xd4, 0x32, 0x12, 0x05,
    0x21, 0x4d, 0xd4, 0x11, 0x02, 0x11, 0x01, 0x0d, 0x00, 0xed, 0xb0, 0xe1, 0x7c, 0xfe, 0xd4, 0x28,
    0x12, 0x2a, 0x04, 0x11, 0xd9, 0x21, 0x00, 0x12, 0x22, 0x04, 0x11, 0xcd, 0x2a, 0x00, 0xd3, 0xe4,
    0xc3, 0x9a, 0xe9, 0xcd, 0x2a, 0x00, 0xd3, 0xe4, 0xc3, 0x24, 0x01
};


/**
 * @brief Extrahuje uživatelské parametry z dekódovaného loader body.
 *
 * Loader body obsahuje patchované hodnoty: fsize, fstrt, fexec a XOR
 * všech bajtů těla (zavaděč ho po načtení porovná). Offset fsize je
 * společný (5), offsety fstrt, fexec a XOR se liší podle formátu.
 *
 * @param loader_body Dekódovaný loader body.
 * @param loader_size Velikost loader body v bajtech.
 * @param format Detekovaný formát (FSK/SLOW/DIRECT).
 * @param[out] out_fsize Uživatelská velikost dat.
 * @param[out] out_fstrt Uživatelská startovní adresa.
 * @param[out] out_fexec Uživatelská exec adresa.
 * @param[out] out_checksum XOR všech bajtů těla uložený v zavaděči.
 * @return WAV_ANALYZER_OK při úspěchu, WAV_ANALYZER_ERROR_INVALID_PARAM při chybě
 *         (jiný formát, nebo loader body kratší než některý offset).
 */
static en_WAV_ANALYZER_ERROR extract_user_params (
    const uint8_t *loader_body,
    uint32_t loader_size,
    en_WAV_TAPE_FORMAT format,
    uint16_t *out_fsize,
    uint16_t *out_fstrt,
    uint16_t *out_fexec,
    uint8_t *out_checksum
) {
    uint32_t off_from, off_exec, off_chk;

    switch ( format ) {
        case WAV_TAPE_FORMAT_FSK:
            off_from = FSK_LDR_OFF_FROM;
            off_exec = FSK_LDR_OFF_EXEC;
            off_chk = FSK_LDR_OFF_CHK;
            break;
        case WAV_TAPE_FORMAT_SLOW:
            off_from = SLOW_LDR_OFF_FROM;
            off_exec = SLOW_LDR_OFF_EXEC;
            off_chk = SLOW_LDR_OFF_CHK;
            break;
        case WAV_TAPE_FORMAT_DIRECT:
            off_from = DIRECT_LDR_OFF_FROM;
            off_exec = DIRECT_LDR_OFF_EXEC;
            off_chk = DIRECT_LDR_OFF_CHK;
            break;
        default:
            return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* kontrola rozsahu */
    if ( LOADER_BODY_OFF_SIZE + 2 > loader_size ||
         off_from + 2 > loader_size ||
         off_exec + 2 > loader_size ||
         off_chk + 1 > loader_size ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* čtení LE hodnot */
    uint16_t fsize, fstrt, fexec;
    memcpy ( &fsize, &loader_body[LOADER_BODY_OFF_SIZE], 2 );
    memcpy ( &fstrt, &loader_body[off_from], 2 );
    memcpy ( &fexec, &loader_body[off_exec], 2 );

    *out_fsize = endianity_bswap16_LE ( fsize );
    *out_fstrt = endianity_bswap16_LE ( fstrt );
    *out_fexec = endianity_bswap16_LE ( fexec );
    *out_checksum = loader_body[off_chk];

    return WAV_ANALYZER_OK;
}


/**
 * @brief Dekóduje dvourychlostní FSK/SLOW/DIRECT tape signál.
 *
 * Struktura signálu:
 * 1. Cast 1: Preloader hlavička (NORMAL FM 1:1) - již dekódovaná
 * 2. Cast 2: Loader body (NORMAL FM, konfigurovatelná rychlost)
 *    - SGAP + STM(20+20) + 2L + body + CRC + 2L
 * 3. Cast 3: Uživatelská data (FSK/SLOW/DIRECT)
 *    - GAP + polarity + sync + data + fadeout
 *
 * Funkce najde Cast 2, dekóduje loader body pomocí FM dekodéru,
 * extrahuje uživatelské parametry, pak najde Cast 3 a dekóduje
 * data příslušným dekodérem.
 *
 * Tělo ověří XOR všech bajtů proti hodnotě v zavaděči:
 * shoda = WAV_CRC_OK; neshoda, nebo méně dekódovaných bajtů než fsize
 * (tělo se ořízne) = WAV_CRC_ERROR. Soubor se vrátí v obou případech.
 * XOR nepozná sudý počet stejně poškozených bajtů (stejně jako zavaděč na Z80).
 *
 * @param seq Sekvence pulzů.
 * @param preloader_raw Surová data preloader hlavičky (128B).
 * @param search_from_pulse Pozice za preloader hlavičkou.
 * @param format Detekovaný formát (FSK/SLOW/DIRECT).
 * @param[out] out_mzf Výstupní MZF soubor (volající uvolní přes mzf_free).
 * @param[out] out_body_result Výsledek dekódování uživatelských dat.
 * @param[out] out_consumed_until Pozice za posledním zpracovaným pulzem.
 * @param[out] out_fsl_speed Úroveň rychlosti FSK/SLOW podle zavaděče
 *             (fsl_speed_from_loader, -1 = nezjištěná; DIRECT -1). Může být NULL.
 * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
 */
static en_WAV_ANALYZER_ERROR decode_nonstandard_tape_mzf (
    const st_WAV_PULSE_SEQUENCE *seq,
    const uint8_t *preloader_raw,
    uint32_t search_from_pulse,
    en_WAV_TAPE_FORMAT format,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_body_result,
    uint32_t *out_consumed_until,
    int *out_fsl_speed
) {
    if ( !seq || !preloader_raw || !out_mzf ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    *out_mzf = NULL;
    if ( out_consumed_until ) *out_consumed_until = 0;
    if ( out_fsl_speed ) *out_fsl_speed = -1;

    /* === 1. Extrakce loader_body_size z preloader cmnt[1..2] (LE) === */

    uint16_t loader_body_size;
    memcpy ( &loader_body_size, &preloader_raw[24 + 1], 2 );
    loader_body_size = endianity_bswap16_LE ( loader_body_size );

    if ( loader_body_size == 0 || loader_body_size > 1024 ) {
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    /* === 2. Hledání leaderu Cast 2 (loader body) === */

    st_WAV_LEADER_INFO loader_leader;
    en_WAV_ANALYZER_ERROR err = wav_leader_detect (
                                    seq, search_from_pulse,
                                    NONSTANDARD_LOADER_LEADER_MIN_PULSES,
                                    WAV_ANALYZER_DEFAULT_TOLERANCE,
                                    &loader_leader
                                );

    if ( err != WAV_ANALYZER_OK ) {
        return WAV_ANALYZER_ERROR_NO_LEADER;
    }

    /* === 3. Dekódování Cast 2 (loader body) pomocí FM dekodéru === */

    st_WAV_FM_DECODER dec;
    uint32_t data_start = loader_leader.start_index + loader_leader.pulse_count;
    wav_decode_fm_init ( &dec, seq, data_start, loader_leader.avg_period_us );

    /* krátký tapemark (STM: 20 LONG + 20 SHORT, minimum 18+18) */
    err = wav_decode_fm_find_tapemark ( &dec, WAV_TAPEMARK_SHORT );
    if ( err != WAV_ANALYZER_OK ) {
        return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
    }

    /* sync blok (2L = max 4 LONG půl-periody) */
    {
        int sync_skip = 0;
        while ( dec.pos < seq->count &&
                seq->pulses[dec.pos].duration_us >= dec.threshold_us &&
                sync_skip < 4 ) {
            dec.pos++;
            sync_skip++;
        }
    }

    /* dekódování loader body */
    uint8_t *loader_body = ( uint8_t* ) malloc ( loader_body_size );
    if ( !loader_body ) return WAV_ANALYZER_ERROR_ALLOC;

    dec.crc_accumulator = 0;
    err = wav_decode_fm_read_block ( &dec, loader_body, loader_body_size );
    if ( err != WAV_ANALYZER_OK ) {
        free ( loader_body );
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    /* verifikace CRC loader body */
    en_WAV_CRC_STATUS loader_crc;
    wav_decode_fm_verify_checksum ( &dec, &loader_crc, NULL, NULL );

    uint32_t after_loader = dec.pos;

    /* === 4. Extrakce uživatelských parametrů z loader body === */

    uint16_t user_fsize, user_fstrt, user_fexec;
    uint8_t user_checksum;
    err = extract_user_params ( loader_body, loader_body_size, format,
                                &user_fsize, &user_fstrt, &user_fexec, &user_checksum );
    if ( out_fsl_speed ) {
        *out_fsl_speed = fsl_speed_from_loader ( loader_body, loader_body_size, format, seq->sample_rate );
    }
    free ( loader_body );

    if ( err != WAV_ANALYZER_OK ) {
        return err;
    }

    if ( user_fsize == 0 ) {
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    /* === 5. Dekódování Cast 3 (uživatelská data) === */

    uint8_t *user_data = NULL;
    uint32_t decoded_size = 0;
    uint32_t data_consumed = 0;

    switch ( format ) {
        case WAV_TAPE_FORMAT_FSK:
            err = wav_decode_fsk_decode_bytes (
                      seq, after_loader,
                      &user_data, &decoded_size, &data_consumed
                  );
            break;
        case WAV_TAPE_FORMAT_SLOW:
            err = wav_decode_slow_decode_bytes (
                      seq, after_loader,
                      &user_data, &decoded_size, &data_consumed
                  );
            break;
        case WAV_TAPE_FORMAT_DIRECT:
            err = wav_decode_direct_decode_bytes (
                      seq, after_loader,
                      &user_data, &decoded_size, &data_consumed
                  );
            break;
        default:
            return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    if ( err != WAV_ANALYZER_OK || !user_data ) {
        free ( user_data );
        return ( err != WAV_ANALYZER_OK ) ? err : WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    /*
     * Dekodéry dekódují až do fadeoutu, takže mohou vrátit více dat
     * než user_fsize. Ořežeme na očekávanou velikost.
     */
    uint32_t actual_size = ( decoded_size >= user_fsize ) ? user_fsize : decoded_size;

    /* === 6. Sestavení MZF struktury === */

    st_MZF *mzf = ( st_MZF* ) calloc ( 1, sizeof ( st_MZF ) );
    if ( !mzf ) {
        free ( user_data );
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    /*
     * Rekonstruujeme hlavičku z preloaderu (fname) a loader body (fsize/fstrt/fexec).
     * Preloader ftype je 0x01 (OBJ) - to je typ loaderu, ne původního programu.
     * Použijeme 0x01 jako výchozí (shodně s FASTIPL).
     */
    mzf->header.ftype = 0x01;
    memcpy ( &mzf->header.fname, &preloader_raw[1], sizeof ( mzf->header.fname ) );
    mzf->header.fsize = actual_size;
    mzf->header.fstrt = user_fstrt;
    mzf->header.fexec = user_fexec;

    /* zkopírujeme data s ořezem */
    uint8_t *body_copy = ( uint8_t* ) malloc ( actual_size );
    if ( !body_copy ) {
        free ( mzf );
        free ( user_data );
        return WAV_ANALYZER_ERROR_ALLOC;
    }
    memcpy ( body_copy, user_data, actual_size );
    free ( user_data );

    mzf->body = body_copy;
    mzf->body_size = actual_size;

    *out_mzf = mzf;

    /* výsledek dekódování */
    if ( out_body_result ) {
        out_body_result->format = format;
        out_body_result->data = ( uint8_t* ) malloc ( actual_size );
        if ( out_body_result->data ) {
            memcpy ( out_body_result->data, body_copy, actual_size );
        }
        out_body_result->data_size = actual_size;
        /*
         * XOR těla nese zavaděč (offset *_LDR_OFF_CHK): shoda = OK,
         * neshoda nebo méně dat než fsize = ERROR (neúplné tělo se dřív tiše ořízlo)
         */
        uint8_t x = 0;
        for ( uint32_t i = 0; i < actual_size; i++ ) x ^= body_copy[i];
        out_body_result->crc_status = ( actual_size == user_fsize && x == user_checksum ) ? WAV_CRC_OK : WAV_CRC_ERROR;
        out_body_result->pulse_start = after_loader;
        out_body_result->pulse_end = data_consumed;
        out_body_result->is_header = 0;
    }

    if ( out_consumed_until ) {
        *out_consumed_until = data_consumed;
    }

    return WAV_ANALYZER_OK;
}


/**
 * @brief Oznámí uživateli záznam FM, jehož hlavičku nebo tělo se nepodařilo přečíst.
 *
 * Hlásí se vždy (i bez --verbose): soubor se zahodil, ale hlavička nebo její část
 * na pásce je. Bez nalezeného tapemarku hlavičky se nehlásí nic, leader bez
 * tapemarku nemusí být záznam FM.
 *
 * @param seq Sekvence pulzů.
 * @param leader Leader tón záznamu.
 * @param config Konfigurace (podle zapnutých voleb se vynechá nápověda).
 * @param info Průběh dekódování z wav_decode_fm_decode_mzf_ex().
 *
 * @note Vedlejší efekt: zápis na stderr.
 */
static void report_fm_failure (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_LEADER_INFO *leader,
    const st_WAV_ANALYZER_CONFIG *config,
    const st_WAV_FM_DECODE_INFO *info
) {
    double at = pulse_index_to_sec ( seq, leader->start_index );

    if ( !info->header_decoded ) {
        if ( info->body_error != WAV_ANALYZER_OK ) {
            fprintf ( stderr, "  Header incomplete at %.3f s (%s), nothing saved\n", at,
                      wav_analyzer_error_string ( info->body_error ) );
        }
        return;
    }

    char fname[MZF_FILE_NAME_LENGTH + 1];
    int n = 0;
    /* jméno končí 0Dh; znaky mimo ASCII se ve výpisu nahradí tečkou */
    while ( n < MZF_FILE_NAME_LENGTH && info->header_raw[1 + n] != 0x0D && info->header_raw[1 + n] != 0x00 ) {
        uint8_t c = info->header_raw[1 + n];
        fname[n++] = ( c < 0x20 || c > 0x7E ) ? '.' : ( char ) c;
    }
    fname[n] = '\0';

    fprintf ( stderr, "  Header \"%s\" found at %.3f s (fsize %u) but body not decoded: %s\n",
              fname, at, info->expected_bytes, wav_analyzer_error_string ( info->body_error ) );

    if ( info->recovered_bytes > 0 && !config->recover_body ) {
        fprintf ( stderr, "  Hint: use --recover-body to keep the partial body (%u bytes read)\n",
                  info->recovered_bytes );
    } else if ( info->recovered_bytes == 0 && !info->header_crc_ok ) {
        fprintf ( stderr, "  The header has a checksum error and no valid copy, it cannot be kept\n" );
    } else if ( info->recovered_bytes == 0 && !config->recover_header ) {
        fprintf ( stderr, "  Hint: use --recover-header to keep the header\n" );
    }
}


/**
 * @brief Zpracuje jeden leader tón - klasifikuje formát a dekóduje data.
 *
 * @param seq Sekvence pulzů.
 * @param leader Informace o leader tónu.
 * @param config Konfigurace analyzéru.
 * @param result Celkový výsledek (přidá se nový soubor).
 * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
 */
static en_WAV_ANALYZER_ERROR process_leader (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_LEADER_INFO *leader,
    const st_WAV_ANALYZER_CONFIG *config,
    st_WAV_ANALYZER_RESULT *result,
    uint32_t *out_consumed_until
) {
    if ( out_consumed_until ) *out_consumed_until = 0;

    /* volitelná histogramová analýza datového úseku za leaderem */
    st_WAV_HISTOGRAM hist;
    memset ( &hist, 0, sizeof ( hist ) );
    int have_hist = 0;

    uint32_t data_start = leader->start_index + leader->pulse_count;
    uint32_t remaining = ( data_start < seq->count ) ? seq->count - data_start : 0;

    if ( remaining >= HISTOGRAM_MIN_PULSES ) {
        /* analyzujeme prvních N pulzů za leaderem pro klasifikaci */
        uint32_t hist_count = ( remaining > 2000 ) ? 2000 : remaining;
        en_WAV_ANALYZER_ERROR herr = wav_histogram_analyze (
                                         seq, data_start, hist_count,
                                         config->histogram_bin_width, &hist
                                     );
        if ( herr == WAV_ANALYZER_OK ) {
            have_hist = 1;
        }
    }

    /* stupeň A: klasifikace z leader tónu */
    en_WAV_TAPE_FORMAT format = wav_classify_from_leader (
                                    leader, have_hist ? &hist : NULL
                                );

    en_WAV_SPEED_CLASS speed = wav_classify_speed_class ( leader->avg_period_us );

    if ( config->verbose ) {
        /* čas leaderu v sekundách: průměrná délka pulzu * počet pulzů / 1e6 */
        double leader_time_sec = ( leader->avg_period_us * leader->pulse_count ) / 1000000.0;
        fprintf ( stderr, "Leader at pulse #%u: %u pulses, avg %.1f us, %.3f sec, class %s -> %s\n",
                  leader->start_index, leader->pulse_count,
                  leader->avg_period_us, leader_time_sec,
                  wav_speed_class_name ( speed ),
                  wav_tape_format_name ( format ) );

        if ( have_hist ) {
            wav_histogram_print ( &hist, stderr );
        }
    }

    /* dekódování podle formátu */
    en_WAV_ANALYZER_ERROR err = WAV_ANALYZER_OK;

    switch ( format ) {
        case WAV_TAPE_FORMAT_NORMAL:
        case WAV_TAPE_FORMAT_SINCLAIR:
        case WAV_TAPE_FORMAT_MZ80B:
        case WAV_TAPE_FORMAT_CPM_CMT:
        {
            /* všechny FM formáty sdílí stejný dekodér */
            st_MZF *mzf = NULL;
            st_WAV_DECODE_RESULT hdr_res, body_res;
            memset ( &hdr_res, 0, sizeof ( hdr_res ) );
            memset ( &body_res, 0, sizeof ( body_res ) );

            st_WAV_FM_DECODE_OPTIONS fm_opts = { config->recover_body, config->recover_header };
            st_WAV_FM_DECODE_INFO fm_info;
            uint32_t fm_recovery = WAV_RECOVERY_NONE;   /* platí, jen když zůstane záznam z dekodéru FM */
            int fm_kept = 0;                            /* výsledkem je záznam z dekodéru FM (bez přesměrování) */
            st_WAV_BODY_RECOVERY body_rec = { config->recover_body, 0, 0, 0 };   /* TURBO, FASTIPL */

            err = wav_decode_fm_decode_mzf_ex ( seq, leader, &fm_opts, &mzf, &hdr_res, &body_res, &fm_info );

            if ( err == WAV_ANALYZER_OK && mzf ) {
                /* stupeň B: zpřesnění formátu z hlavičky */
                en_WAV_TAPE_FORMAT refined = wav_classify_from_header (
                                                 &mzf->header, format
                                             );

                /*
                 * Pokud zpřesněný formát je TURBO/FASTIPL/BSD,
                 * přepneme na specializovaný dekodér.
                 */
                uint32_t consumed_until = 0;
                uint32_t bsd_recovery = WAV_RECOVERY_NONE;
                int fsl_speed = -1;                          /* FSK/SLOW: úroveň ze zavaděče */
                en_WAV_WRITER writer = WAV_WRITER_UNKNOWN;   /* rozpoznaný zapisovač */
                uint8_t tc_a = 0, tc_b = 0;                  /* TurboCopy: poměr a/b */
                uint16_t ic_bd = 0;                          /* Intercopy: rychlost v Bd */
                st_WAV_BSD_INFO bsd_info = { 0, 0, 0, 0 };   /* kusy, chyby CRC, kopie */
                st_WAV_LEADER_INFO turbo_data_leader;
                memset ( &turbo_data_leader, 0, sizeof ( turbo_data_leader ) );

                /*
                 * Předzavaděč TurboCopy na $D400 musí mít i generickou část těla
                 * (prvních TURBOCOPY_LOADER_GENERIC_SIZE B); jinak jde o obyčejný
                 * program a zůstane předběžný formát
                 */
                if ( refined == WAV_TAPE_FORMAT_TURBO && mzf->header.fstrt == 0xD400
                     && ( !body_res.data || body_res.data_size < TURBOCOPY_LOADER_GENERIC_SIZE
                          || memcmp ( body_res.data, g_turbocopy_loader_generic, TURBOCOPY_LOADER_GENERIC_SIZE ) != 0 ) ) {
                    refined = format;
                }

                if ( refined == WAV_TAPE_FORMAT_TURBO ) {
                    if ( mzf->header.fsize > 0 && mzf->header.fstrt == 0xD400 ) {
                        /*
                         * TurboCopy TURBO preloader (kategorie 4):
                         * fstrt=$D400, fsize=90 (realne loader body).
                         *
                         * TurboCopy loader patchne ROM a vola standardni CMT read
                         * rutinu $002A, ktera cte POUZE telo (bez hlavicky).
                         * Metadata (fsize/fstrt/fexec) jsou v preloader body
                         * na offsetu $4D.
                         */
                        uint32_t turbo_search = body_res.pulse_end > 0
                                                ? body_res.pulse_end
                                                : hdr_res.pulse_end;
                        if ( config->verbose ) {
                            fprintf ( stderr, "  TurboCopy preloader (fsize=%u) -> TURBO body-only decode from pulse %u\n",
                                      mzf->header.fsize, turbo_search );
                        }

                        /* ulozime preloader metadata pred uvolnenim */
                        st_MZF_HEADER saved_header;
                        memcpy ( &saved_header, &mzf->header, sizeof ( st_MZF_HEADER ) );
                        uint8_t *saved_body = body_res.data;
                        uint32_t saved_body_size = body_res.data_size;
                        body_res.data = NULL; /* vlastnictvi predano */

                        mzf_free ( mzf );
                        mzf = NULL;
                        free ( hdr_res.data ); hdr_res.data = NULL;
                        memset ( &hdr_res, 0, sizeof ( hdr_res ) );
                        memset ( &body_res, 0, sizeof ( body_res ) );

                        err = wav_decode_turbo_turbocopy_mzf (
                                  seq, turbo_search,
                                  &saved_header, saved_body, saved_body_size,
                                  &mzf, &body_res, &consumed_until,
                                  &turbo_data_leader, &body_rec
                              );

                        /* aktualizujeme speed na TURBO hodnoty */
                        if ( err == WAV_ANALYZER_OK && turbo_data_leader.pulse_count > 0 ) {
                            speed = wav_classify_speed_class ( turbo_data_leader.avg_period_us );

                            /*
                             * Zapisovač podle datového tónu (před přepisem průměru níže):
                             * Intercopy V10.2 píše tón těla 5500 SHORT a na 4Bh čtecí bod + 5,
                             * TurboCopy tón s počtem x S = TC_TONE_PRODUCT.
                             */
                            uint32_t dp = turbo_data_leader.pulse_count / 2;
                            uint16_t bd = ( saved_body_size > 0x4B ) ? ic_bd_from_rp ( ( int ) saved_body[0x4B] - 5 ) : 0;
                            if ( bd && fabs ( dp / IC_TONE_BODY - 1.0 ) <= IC_TONE_TOL ) {
                                writer = WAV_WRITER_INTERCOPY;
                                ic_bd = bd;
                            } else if ( tc_ratio_from_tone ( dp, turbo_data_leader.avg_period_us, seq->sample_rate,
                                                             &tc_a, &tc_b ) ) {
                                writer = WAV_WRITER_TURBOCOPY;
                            }

                            /*
                             * Spocteme rychlostni pomer z speed_val v preloader body.
                             * speed_val je ROM delay na $0A4B. Referencni delay pro 1:1
                             * je ~82 ($52), odvozeno z mereni: 82/41=2.0 (2:1),
                             * 82/35=2.34 (7:3), 82/30=2.73 (8:3), 82/27=3.04 (3:1).
                             * Dosazene pomery presne odpovidaji TurboCopy rychlostem.
                             * Pouzijeme speed_val pro presnejsi odhad nez leader avg
                             * (ktery trpi kvantizaci pri 44100 Hz).
                             */
                            if ( saved_body_size > 0x4B ) {
                                uint8_t speed_val = saved_body[0x4B];
                                if ( speed_val > 0 ) {
                                    double ratio = 82.0 / ( double ) speed_val;
                                    turbo_data_leader.avg_period_us = 249.0 / ratio;
                                }
                            }
                        }

                        free ( saved_body );
                    } else {
                        /* mzftools TURBO (kategorie 3): fsize=0, loader v comment.
                         * Metadata (fsize/fstrt/fexec) v comment oblasti hlavicky.
                         * TURBO data = body-only (loader vola RDATA). */
                        uint32_t turbo_search = hdr_res.pulse_end > 0
                                                ? hdr_res.pulse_end
                                                : leader->start_index + leader->pulse_count;
                        if ( config->verbose ) {
                            fprintf ( stderr, "  mzftools TURBO preloader -> TURBO body-only decode from pulse %u\n",
                                      turbo_search );
                        }

                        st_MZF_HEADER saved_mzf_header;
                        memcpy ( &saved_mzf_header, &mzf->header, sizeof ( st_MZF_HEADER ) );

                        mzf_free ( mzf );
                        mzf = NULL;
                        free ( hdr_res.data ); hdr_res.data = NULL;
                        free ( body_res.data ); body_res.data = NULL;
                        memset ( &hdr_res, 0, sizeof ( hdr_res ) );
                        memset ( &body_res, 0, sizeof ( body_res ) );

                        err = wav_decode_turbo_mzftools_mzf (
                                  seq, turbo_search, &saved_mzf_header,
                                  &mzf, &body_res, &consumed_until, &turbo_data_leader, &body_rec
                              );
                    }
                } else if ( refined == WAV_TAPE_FORMAT_FASTIPL ) {
                    /* FASTIPL: body blok s vlastnim LGAP za header CRC */
                    uint8_t *bb_raw = hdr_res.data;
                    uint32_t pulse_end = hdr_res.pulse_end;
                    st_WAV_LEADER_INFO fastipl_data_leader;
                    memset ( &fastipl_data_leader, 0, sizeof ( fastipl_data_leader ) );

                    mzf_free ( mzf );
                    mzf = NULL;
                    free ( body_res.data ); body_res.data = NULL;
                    memset ( &body_res, 0, sizeof ( body_res ) );

                    err = wav_decode_fastipl_decode_mzf (
                              seq, bb_raw, pulse_end, &mzf,
                              &body_res, &consumed_until,
                              &fastipl_data_leader, &body_rec
                          );

                    /* aktualizace speed z body LGAP leaderu */
                    if ( err == WAV_ANALYZER_OK && fastipl_data_leader.pulse_count > 0 ) {
                        speed = wav_classify_speed_class ( fastipl_data_leader.avg_period_us );
                        turbo_data_leader = fastipl_data_leader;
                    }
                    /*
                     * Intercopy V10.2 (zavaděč V07): na 27h-28h hlavičky $BB je LD (HL),$01
                     * (36h 01h, u V02 97h), na 19h čtecí bod rp z tabulky rychlostí.
                     */
                    if ( err == WAV_ANALYZER_OK && bb_raw && bb_raw[0x27] == 0x36 && bb_raw[0x28] == 0x01 ) {
                        ic_bd = ic_bd_from_rp ( bb_raw[0x19] );
                        if ( ic_bd ) writer = WAV_WRITER_INTERCOPY;
                    }

                    free ( bb_raw );
                    hdr_res.data = NULL;
                } else if ( refined == WAV_TAPE_FORMAT_BSD ) {
                    /* BSD: chunkovane telo za hlavicku */
                    st_MZF_HEADER saved_header;
                    memcpy ( &saved_header, &mzf->header, sizeof ( st_MZF_HEADER ) );
                    uint32_t pulse_end = hdr_res.pulse_end;

                    mzf_free ( mzf );
                    mzf = NULL;
                    free ( body_res.data ); body_res.data = NULL;
                    memset ( &body_res, 0, sizeof ( body_res ) );

                    err = wav_decode_bsd_decode_mzf (
                              seq, leader, &saved_header, pulse_end,
                              config->recover_bsd,
                              &mzf, &body_res, &consumed_until,
                              &bsd_recovery, &bsd_info
                          );

                    free ( hdr_res.data );
                    hdr_res.data = NULL;

                    /* diagnosticky vypis - VZDY pri selhani BSD decode */
                    if ( err != WAV_ANALYZER_OK ) {
                        char fname[MZF_FILE_NAME_LENGTH + 1];
                        memcpy ( fname, saved_header.fname.name, MZF_FILE_NAME_LENGTH );
                        fname[MZF_FILE_NAME_LENGTH] = '\0';
                        for ( int j = 0; j < MZF_FILE_NAME_LENGTH; j++ ) {
                            if ( fname[j] < 0x20 || fname[j] > 0x7E ) fname[j] = '.';
                        }
                        fprintf ( stderr, "  BSD header found: \"%s\", ftype=0x%02X, decode failed: %s\n"
                                          "  Hint: use --recover-bsd to salvage partial data\n",
                                  fname, saved_header.ftype,
                                  wav_analyzer_error_string ( err ) );
                    }

                    /* informace o recovery (vzdy, i pri uspechu) */
                    if ( err == WAV_ANALYZER_OK && bsd_recovery != WAV_RECOVERY_NONE ) {
                        fprintf ( stderr, "  WARNING: BSD recovered - %s (%u bytes salvaged)\n",
                                  wav_recovery_status_string ( bsd_recovery ),
                                  mzf ? mzf->body_size : 0 );
                    }
                } else if ( refined == WAV_TAPE_FORMAT_FSK ||
                            refined == WAV_TAPE_FORMAT_SLOW ||
                            refined == WAV_TAPE_FORMAT_DIRECT ) {
                    /*
                     * FSK/SLOW/DIRECT: dvourychlostní signál.
                     * Cast 1 = preloader hlavička (právě dekódovaná, fsize=0)
                     * Cast 2 = loader body v NORMAL FM
                     * Cast 3 = uživatelská data v FSK/SLOW/DIRECT
                     *
                     * Předzavaděč se signaturou v cmnt má fsize 0. Při fsize > 0
                     * stojí klasifikace jen na velikosti těla a adrese $D400:
                     * když část 3 dekódovat nejde, zůstane dekódovaný záznam
                     * NORMAL (obyčejný program na $D400). Se signaturou je selhání chyba dekódování
                     * (useknutý záznam se nesmí tvářit jako platný soubor).
                     */
                    int size_only = mzf && mzf->header.fsize != 0;
                    st_MZF *ns_mzf = NULL;
                    st_WAV_DECODE_RESULT ns_body;
                    uint32_t ns_consumed = 0;
                    memset ( &ns_body, 0, sizeof ( ns_body ) );

                    en_WAV_ANALYZER_ERROR ns_err = decode_nonstandard_tape_mzf (
                              seq, hdr_res.data, hdr_res.pulse_end, refined,
                              &ns_mzf, &ns_body, &ns_consumed, &fsl_speed
                          );

                    if ( ns_err == WAV_ANALYZER_OK && ns_mzf ) {
                        mzf_free ( mzf );
                        mzf = ns_mzf;
                        free ( hdr_res.data );
                        free ( body_res.data );
                        memset ( &hdr_res, 0, sizeof ( hdr_res ) );
                        body_res = ns_body;
                        consumed_until = ns_consumed;
                    } else if ( !size_only ) {
                        mzf_free ( ns_mzf );
                        free ( ns_body.data );
                        mzf_free ( mzf );
                        mzf = NULL;
                        err = ns_err;
                    } else {
                        mzf_free ( ns_mzf );
                        free ( ns_body.data );
                        if ( config->verbose ) {
                            fprintf ( stderr, "  %s data not decoded (%s), keeping the NORMAL record\n",
                                      wav_tape_format_name ( refined ), wav_analyzer_error_string ( ns_err ) );
                        }
                        refined = format;
                        fm_kept = 1;
                        fm_recovery = fm_info.recovery_status;
                        consumed_until = body_res.pulse_end > 0
                                         ? body_res.pulse_end
                                         : hdr_res.pulse_end;
                    }
                } else {
                    /*
                     * Zadny redispatch - format zustava NORMAL/CPM-CMT/atd.
                     * consumed_until nastavime z FM dekoderu (pulse_end
                     * hlavicky nebo tela, podle toho co je dale).
                     */
                    fm_kept = 1;
                    fm_recovery = fm_info.recovery_status;
                    consumed_until = body_res.pulse_end > 0
                                     ? body_res.pulse_end
                                     : hdr_res.pulse_end;
                }

                if ( err == WAV_ANALYZER_OK && mzf ) {
                    st_WAV_ANALYZER_FILE_RESULT file_result;
                    memset ( &file_result, 0, sizeof ( file_result ) );
                    file_result.mzf = mzf;
                    file_result.format = refined;
                    file_result.header_crc = hdr_res.crc_status;
                    file_result.body_crc = body_res.crc_status;
                    /* pro TURBO pouzijeme datovy leader (rychlost), ne preloader */
                    file_result.leader = ( turbo_data_leader.pulse_count > 0 )
                                         ? turbo_data_leader : *leader;
                    file_result.speed_class = speed;

                    /* naměřené délky pulzů z histogramu (FM: nejmenší peak = SHORT, největší = LONG) */
                    if ( have_hist &&
                         hist.modulation == WAV_MODULATION_FM &&
                         hist.peak_count >= 2 ) {
                        file_result.short_pulse_us = hist.peaks[0].center_us;
                        file_result.long_pulse_us = hist.peaks[hist.peak_count - 1].center_us;
                    }

                    file_result.copy2_used = hdr_res.copy2_used || body_res.copy2_used;
                    file_result.header_leader_pulse = leader->start_index;
                    file_result.consumed_until_pulse = consumed_until;
                    file_result.recovery_status = bsd_recovery | fm_recovery | body_rec.status;
                    file_result.bsd_chunks = bsd_info.chunks;
                    file_result.bsd_crc_errors = bsd_info.crc_errors;
                    file_result.bsd_from_copy = bsd_info.from_copy;
                    file_result.bsd_gap_chunks = bsd_info.gap_chunks;
                    if ( refined == WAV_TAPE_FORMAT_FSK || refined == WAV_TAPE_FORMAT_SLOW ) {
                        file_result.fsl_speed = fsl_speed;
                    }
                    if ( refined == WAV_TAPE_FORMAT_NORMAL && writer == WAV_WRITER_UNKNOWN ) {
                        double th_us = 0.0;
                        uint32_t hp = tone_pulses ( seq, leader, &th_us );
                        if ( tc_ratio_from_tone ( hp, th_us, seq->sample_rate, &tc_a, &tc_b ) ) {
                            writer = WAV_WRITER_TURBOCOPY;
                        } else if ( ( ic_bd = ic_normal_bd_from_tone ( hp, th_us, seq->sample_rate ) ) != 0 ) {
                            writer = WAV_WRITER_INTERCOPY;
                        }
                    }
                    file_result.writer = writer;
                    file_result.tc_a = tc_a;
                    file_result.tc_b = tc_b;
                    file_result.ic_bd = ic_bd;
                    if ( bsd_recovery != WAV_RECOVERY_NONE && mzf ) {
                        file_result.recovered_bytes = mzf->body_size;
                        file_result.expected_bytes = 0; /* u BSD neznáme */
                    }
                    if ( fm_recovery != WAV_RECOVERY_NONE ) {
                        file_result.recovered_bytes = fm_info.recovered_bytes;
                        file_result.expected_bytes = fm_info.expected_bytes;
                    }
                    if ( body_rec.status != WAV_RECOVERY_NONE ) {
                        file_result.recovered_bytes = body_rec.read_bytes;
                        file_result.expected_bytes = body_rec.expected_bytes;
                    }
                    if ( fm_kept ) {
                        /* u neúplného těla se checksum nepřečetl, z pásky není */
                        file_result.header_crc_stored = hdr_res.crc_stored;
                        file_result.header_crc_stored_valid = hdr_res.crc_status == WAV_CRC_ERROR;
                        file_result.body_crc_stored = body_res.crc_stored;
                        file_result.copy_diff_bytes = fm_info.header_copy_diff + fm_info.body_copy_diff;
                        file_result.merged_bytes = fm_info.merged_bytes;
                        file_result.body_crc_stored_valid = body_res.crc_status == WAV_CRC_ERROR
                                                            && !( fm_recovery & ( WAV_RECOVERY_PARTIAL_BODY | WAV_RECOVERY_HEADER_ONLY ) );
                    }

                    err = result_add_file ( result, &file_result );
                    if ( err != WAV_ANALYZER_OK ) {
                        mzf_free ( mzf );
                    }

                    if ( config->verbose ) {
                        fprintf ( stderr, "  Decoded: format=%s, header CRC=%s, body CRC=%s, body_size=%u%s\n",
                                  wav_tape_format_name ( refined ),
                                  ( hdr_res.crc_status == WAV_CRC_OK ) ? "OK" : "ERROR",
                                  ( body_res.crc_status == WAV_CRC_OK ) ? "OK"
                                  : ( body_res.crc_status == WAV_CRC_NOT_AVAILABLE ? "N/A" : "ERROR" ),
                                  mzf->body_size,
                                  file_result.copy2_used ? " (Copy2)" : "" );
                    }
                } else {
                    if ( config->verbose ) {
                        fprintf ( stderr, "  Decode failed: %s\n", wav_analyzer_error_string ( err ) );
                    }
                    /* pokračujeme - nepovažujeme za fatální chybu */
                    err = WAV_ANALYZER_OK;
                }

                /* uvolníme kopie dat ve výsledcích dekódování */
                free ( hdr_res.data );
                free ( body_res.data );
            } else {
                if ( config->verbose ) {
                    fprintf ( stderr, "  Decode failed: %s\n", wav_analyzer_error_string ( err ) );
                }
                report_fm_failure ( seq, leader, config, &fm_info );
                err = WAV_ANALYZER_OK;

                /*
                 * ZX fallback: kdyz FM tapemark detekce selze, zkusime
                 * ZX Spectrum dekoder. SINCLAIR format pouziva ZX protokol
                 * (sync + ZX datove kodovani) pri ruznych rychlostech,
                 * takze leader muze spadat do tridy ZX (612 us) i NORMAL
                 * (476, 408, 340 us pro vyssi rychlosti).
                 *
                 * Bezpecnost: pro skutecny NORMAL FM signal ZX sync
                 * detekce selze (zadne kratke pulzy relativne k leaderu),
                 * takze fallback je bezpecny pro vsechny FM formaty.
                 */
                {
                    uint8_t *tap_data = NULL;
                    uint32_t tap_size = 0;
                    en_WAV_CRC_STATUS zx_crc = WAV_CRC_NOT_AVAILABLE;
                    uint32_t zx_consumed = 0;

                    err = wav_decode_zx_decode_block (
                              seq, leader, &tap_data, &tap_size, &zx_crc, &zx_consumed );

                    if ( err == WAV_ANALYZER_OK && tap_data ) {
                        st_WAV_ANALYZER_FILE_RESULT file_result;
                        memset ( &file_result, 0, sizeof ( file_result ) );
                        file_result.tap_data = tap_data;
                        file_result.tap_data_size = tap_size;
                        file_result.format = WAV_TAPE_FORMAT_SINCLAIR;
                        file_result.header_crc = zx_crc;
                        file_result.body_crc = WAV_CRC_NOT_AVAILABLE;
                        file_result.leader = *leader;
                        file_result.speed_class = speed;
                        file_result.consumed_until_pulse = zx_consumed;

                        err = result_add_file ( result, &file_result );
                        if ( err != WAV_ANALYZER_OK ) {
                            free ( tap_data );
                        }

                        if ( config->verbose ) {
                            fprintf ( stderr, "  ZX fallback: format=SINCLAIR, flag=0x%02X, size=%u, CRC=%s\n",
                                      tap_data[0], tap_size,
                                      ( zx_crc == WAV_CRC_OK ) ? "OK" : "ERROR" );
                        }
                    } else {
                        if ( config->verbose ) {
                            fprintf ( stderr, "  ZX fallback also failed\n" );
                        }
                        err = WAV_ANALYZER_OK;
                    }
                }
            }
            break;
        }

        case WAV_TAPE_FORMAT_ZX_SPECTRUM:
        {
            uint8_t *tap_data = NULL;
            uint32_t tap_size = 0;
            en_WAV_CRC_STATUS crc = WAV_CRC_NOT_AVAILABLE;
            uint32_t consumed = 0;

            err = wav_decode_zx_decode_block (
                      seq, leader, &tap_data, &tap_size, &crc, &consumed );

            if ( err == WAV_ANALYZER_OK && tap_data ) {
                st_WAV_ANALYZER_FILE_RESULT file_result;
                memset ( &file_result, 0, sizeof ( file_result ) );
                file_result.tap_data = tap_data;
                file_result.tap_data_size = tap_size;
                /* pilot mimo standardní rychlost (2168 T = 619,4 us, +-5 %) = SINCLAIR s vlastním
                 * časováním (blok 0x11), např. Intercopy SINCLAIR 900 Bd (pilot 930 us) */
                file_result.format = ( fabs ( leader->avg_period_us / ZX_STD_PILOT_US - 1.0 ) > ZX_STD_PILOT_TOL )
                                     ? WAV_TAPE_FORMAT_SINCLAIR : WAV_TAPE_FORMAT_ZX_SPECTRUM;
                file_result.header_crc = crc;
                file_result.body_crc = WAV_CRC_NOT_AVAILABLE;
                file_result.leader = *leader;
                file_result.speed_class = speed;
                file_result.consumed_until_pulse = consumed;

                err = result_add_file ( result, &file_result );
                if ( err != WAV_ANALYZER_OK ) {
                    free ( tap_data );
                }

                if ( config->verbose ) {
                    fprintf ( stderr, "  Decoded: format=%s, flag=0x%02X, size=%u, CRC=%s\n",
                              wav_tape_format_name ( file_result.format ), tap_data[0], tap_size,
                              ( crc == WAV_CRC_OK ) ? "OK" : "ERROR" );
                }
            } else {
                if ( config->verbose ) {
                    fprintf ( stderr, "  ZX decode failed: %s\n",
                              wav_analyzer_error_string ( err ) );
                }
                err = WAV_ANALYZER_OK;
            }
            break;
        }

        default:
            if ( config->verbose ) {
                fprintf ( stderr, "  Format %s: no decoder available\n",
                          wav_tape_format_name ( format ) );
            }
            break;
    }

    wav_histogram_destroy ( &hist );
    return err;
}


/** @brief Minimální počet pulzů pro uznání mezery jako raw bloku. */
#define RAW_BLOCK_MIN_PULSES    50

/** @brief Počáteční kapacita pole raw bloků. */
#define RAW_BLOCK_INITIAL_CAPACITY  4


/**
 * @brief Konvertuje úsek pulzů na bitová data pro TZX Direct Recording (0x15).
 *
 * Expanduje pulzy na jednotlivé vzorky podle duration_samples.
 * Každý vzorek odpovídá jednomu bitu: level 1 = HIGH = bit 1,
 * level 0 = LOW = bit 0. Bity se balí do bajtů MSb first
 * (dle specifikace TZX bloku 0x15).
 *
 * @param seq Sekvence pulzů.
 * @param start_pulse Index prvního pulzu.
 * @param end_pulse Index za posledním pulzem.
 * @param sample_rate Vzorkovací frekvence (Hz).
 * @param[out] out_data Výstupní bitová data (alokována na heapu).
 * @param[out] out_data_size Velikost výstupních dat v bajtech.
 * @param[out] out_used_bits_last Použité bity v posledním bajtu (1-8).
 * @return WAV_ANALYZER_OK při úspěchu, WAV_ANALYZER_ERROR_ALLOC při chybě
 *         alokace, WAV_ANALYZER_ERROR_BUFFER_OVERFLOW pro úsek nad 2^35
 *         vzorků (bajty bloku přes uint32; *out_data pak NULL).
 *
 * @note Paměť je úměrná počtu vzorků (1 bit na vzorek), při 17 721 600 Hz
 *       asi 2,2 MB na sekundu úseku.
 *
 * @post Při úspěchu *out_data ukazuje na alokovaný buffer.
 *       Volající musí uvolnit přes free().
 */
static en_WAV_ANALYZER_ERROR pulses_to_direct_recording (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t start_pulse,
    uint32_t end_pulse,
    uint32_t sample_rate,
    uint8_t **out_data,
    uint32_t *out_data_size,
    uint8_t *out_used_bits_last
) {
    (void) sample_rate;

    /* spočítáme celkový počet vzorků */
    uint64_t total_samples = 0;
    for ( uint32_t i = start_pulse; i < end_pulse; i++ ) {
        total_samples += seq->pulses[i].duration_samples;
    }

    if ( total_samples == 0 ) {
        *out_data = NULL;
        *out_data_size = 0;
        *out_used_bits_last = 8;
        return WAV_ANALYZER_OK;
    }

    /*
     * 1 bit na vzorek: při vysoké frekvenci (pulzy z emulátoru, 17 721 600 Hz)
     * úsek nad 2^35 vzorků (asi 32 min) nevejde do uint32 bajtů bloku.
     * Dřív se počet bajtů tiše zkrátil a zápis bitů vyšel mimo buffer.
     */
    if ( ( total_samples + 7 ) / 8 > UINT32_MAX ) {
        *out_data = NULL;
        *out_data_size = 0;
        *out_used_bits_last = 8;
        return WAV_ANALYZER_ERROR_BUFFER_OVERFLOW;
    }

    /* alokace bufferu */
    uint32_t byte_count = ( uint32_t ) ( ( total_samples + 7 ) / 8 );
    uint8_t *data = ( uint8_t* ) calloc ( byte_count, 1 );
    if ( !data ) return WAV_ANALYZER_ERROR_ALLOC;

    /* expandujeme pulzy na bity MSb first */
    uint64_t bit_pos = 0;
    for ( uint32_t i = start_pulse; i < end_pulse; i++ ) {
        uint32_t samples = seq->pulses[i].duration_samples;
        int level = seq->pulses[i].level;

        for ( uint32_t s = 0; s < samples && bit_pos < total_samples; s++ ) {
            if ( level ) {
                uint32_t byte_idx = ( uint32_t ) ( bit_pos / 8 );
                uint32_t bit_idx = 7 - ( uint32_t ) ( bit_pos % 8 );
                data[byte_idx] |= ( 1 << bit_idx );
            }
            bit_pos++;
        }
    }

    /* použité bity v posledním bajtu */
    uint8_t used = ( uint8_t ) ( total_samples % 8 );
    if ( used == 0 ) used = 8;

    *out_data = data;
    *out_data_size = byte_count;
    *out_used_bits_last = used;

    return WAV_ANALYZER_OK;
}


/**
 * @brief Zjistí, zda pulz leží uvnitř dekódovaného záznamu CPM-TAPE.
 * @param r Výsledek analýzy. @param pulse Index pulzu.
 * @return 1 = uvnitř [header_leader_pulse, consumed_until_pulse) některého souboru CPM-TAPE.
 */
static int in_cpmtape_record ( const st_WAV_ANALYZER_RESULT *r, uint32_t pulse ) {
    for ( uint32_t i = 0; i < r->file_count; i++ ) {
        const st_WAV_ANALYZER_FILE_RESULT *f = &r->files[i];
        if ( f->format == WAV_TAPE_FORMAT_CPM_TAPE && pulse >= f->header_leader_pulse && pulse < f->consumed_until_pulse ) {
            return 1;
        }
    }
    return 0;
}


/**
 * @brief Pozice souboru na pásce pro řazení.
 * @param f Soubor. @return header_leader_pulse, u souborů bez něj (ZX, SINCLAIR) leader.start_index.
 */
static uint32_t file_position ( const st_WAV_ANALYZER_FILE_RESULT *f ) {
    return f->header_leader_pulse ? f->header_leader_pulse : f->leader.start_index;
}


/**
 * @brief Seřadí soubory podle pozice na pásce (stabilně), jen když jsou mezi nimi záznamy CPM-TAPE.
 *
 * Záznamy CPM-TAPE se hledají před leadery FM (krok 4b), v poli jsou proto
 * první. Bez nich zůstává pořadí, v jakém je dekodéry našly.
 *
 * @param r Výsledek analýzy (pořadí souborů se mění).
 */
static void sort_files_by_position ( st_WAV_ANALYZER_RESULT *r ) {
    int any = 0;
    for ( uint32_t i = 0; i < r->file_count; i++ ) {
        if ( r->files[i].format == WAV_TAPE_FORMAT_CPM_TAPE ) any = 1;
    }
    if ( !any ) return;
    for ( uint32_t i = 1; i < r->file_count; i++ ) {
        st_WAV_ANALYZER_FILE_RESULT t = r->files[i];
        uint32_t k = i;
        while ( k > 0 && file_position ( &r->files[k - 1] ) > file_position ( &t ) ) {
            r->files[k] = r->files[k - 1];
            k--;
        }
        r->files[k] = t;
    }
}


/**
 * @brief Sesbírá mezery mezi dekódovanými soubory a konvertuje je na raw bloky.
 *
 * Sestaví seznam "pokrytých" oblastí z dekódovaných souborů
 * (header_leader_pulse .. consumed_until_pulse), identifikuje mezery
 * a pro každou dostatečně velkou mezeru vytvoří Direct Recording blok.
 *
 * @param result Výsledek analýzy (přidá raw_blocks).
 * @param seq Sekvence pulzů.
 * @param config Konfigurace analyzéru.
 * @return WAV_ANALYZER_OK při úspěchu, WAV_ANALYZER_ERROR_ALLOC při chybě.
 */
static en_WAV_ANALYZER_ERROR collect_gaps (
    st_WAV_ANALYZER_RESULT *result,
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_ANALYZER_CONFIG *config
) {
    /*
     * === 1. Sestavení pokrytých oblastí ===
     * Prosté pole (start, end) z dekódovaných souborů.
     */
    typedef struct { uint32_t start; uint32_t end; } coverage_t;

    uint32_t cov_count = result->file_count;
    coverage_t *cov = NULL;

    if ( cov_count > 0 ) {
        cov = ( coverage_t* ) malloc ( cov_count * sizeof ( coverage_t ) );
        if ( !cov ) return WAV_ANALYZER_ERROR_ALLOC;

        for ( uint32_t i = 0; i < cov_count; i++ ) {
            /* pouzijeme header_leader_pulse (pozice header leaderu),
             * ne leader.start_index (ktery pro TURBO/FASTIPL obsahuje
             * datovy leader, ne header leader) */
            cov[i].start = result->files[i].header_leader_pulse;
            cov[i].end = result->files[i].consumed_until_pulse;
            /* pokud consumed_until_pulse == 0, nemáme info o konci */
            if ( cov[i].end == 0 ) {
                cov[i].end = cov[i].start;
            }
        }

        /* seřadíme dle start (jednoduchý insertion sort, souborů je málo) */
        for ( uint32_t i = 1; i < cov_count; i++ ) {
            coverage_t tmp = cov[i];
            uint32_t j = i;
            while ( j > 0 && cov[j - 1].start > tmp.start ) {
                cov[j] = cov[j - 1];
                j--;
            }
            cov[j] = tmp;
        }
    }

    /*
     * === 2. Identifikace mezer ===
     * Procházíme od pulzu 0 do seq->count. Oblasti mimo
     * pokrytí jsou kandidáti na raw bloky.
     */
    uint32_t current = 0;

    for ( uint32_t i = 0; i <= cov_count; i++ ) {
        uint32_t gap_start = current;
        uint32_t gap_end;

        if ( i < cov_count ) {
            gap_end = cov[i].start;
        } else {
            gap_end = seq->count;
        }

        /* přeskočíme překrývající se nebo příliš krátké mezery */
        if ( gap_end > gap_start && ( gap_end - gap_start ) >= RAW_BLOCK_MIN_PULSES ) {
            /* konvertujeme na Direct Recording */
            uint8_t *raw_data = NULL;
            uint32_t raw_size = 0;
            uint8_t used_bits = 8;

            en_WAV_ANALYZER_ERROR err = pulses_to_direct_recording (
                seq, gap_start, gap_end, seq->sample_rate,
                &raw_data, &raw_size, &used_bits
            );

            if ( err == WAV_ANALYZER_OK && raw_data && raw_size > 0 ) {
                /* přidáme do pole raw_blocks */
                if ( result->raw_block_count >= result->raw_block_capacity ) {
                    uint32_t new_cap = ( result->raw_block_capacity == 0 )
                                       ? RAW_BLOCK_INITIAL_CAPACITY
                                       : result->raw_block_capacity * 2;
                    st_WAV_ANALYZER_RAW_BLOCK *new_blocks = ( st_WAV_ANALYZER_RAW_BLOCK* ) realloc (
                        result->raw_blocks, new_cap * sizeof ( st_WAV_ANALYZER_RAW_BLOCK )
                    );
                    if ( !new_blocks ) {
                        free ( raw_data );
                        free ( cov );
                        return WAV_ANALYZER_ERROR_ALLOC;
                    }
                    result->raw_blocks = new_blocks;
                    result->raw_block_capacity = new_cap;
                }

                /* spočítáme čas a délku */
                double start_time = 0.0;
                for ( uint32_t p = 0; p < gap_start && p < seq->count; p++ ) {
                    start_time += seq->pulses[p].duration_us;
                }
                start_time /= 1000000.0;

                double duration = 0.0;
                for ( uint32_t p = gap_start; p < gap_end && p < seq->count; p++ ) {
                    duration += seq->pulses[p].duration_us;
                }
                duration /= 1000000.0;

                st_WAV_ANALYZER_RAW_BLOCK *rb = &result->raw_blocks[result->raw_block_count];
                rb->pulse_start = gap_start;
                rb->pulse_end = gap_end;
                rb->start_time_sec = start_time;
                rb->duration_sec = duration;
                rb->sample_rate = seq->sample_rate;
                rb->data = raw_data;
                rb->data_size = raw_size;
                rb->used_bits_last = used_bits;

                result->raw_block_count++;

                if ( config->verbose ) {
                    fprintf ( stderr, "Raw block: pulses %u-%u, %.3f sec at %.3f sec, %u bytes\n",
                              gap_start, gap_end, duration, start_time, raw_size );
                }
            } else {
                free ( raw_data );
            }
        }

        /* posuneme current za konec aktuální pokryté oblasti */
        if ( i < cov_count && cov[i].end > current ) {
            current = cov[i].end;
        }
    }

    free ( cov );
    return WAV_ANALYZER_OK;
}


/**
 * @brief Analyzuje posloupnost pulzů: detekce leaderů, klasifikace, dekódování, surové bloky.
 *
 * Společná část wav_analyzer_analyze() a wav_analyzer_analyze_pulses() (kroky
 * od statistik pulzů dál). Doplní do výsledku total_pulses, pulse_stats,
 * leaders, files a raw_blocks; sample_rate a wav_duration_sec nastavuje volající.
 *
 * @param seq Posloupnost pulzů (sample_rate > 0, pulzy s kladnou délkou). Nemění se.
 * @param config Konfigurace analyzéru.
 * @param out_result Výsledek, který volající vynuloval (memset 0) a případně
 *        doplnil sample_rate a wav_duration_sec.
 * @return WAV_ANALYZER_OK, nebo chyba detekce leaderů (WAV_ANALYZER_ERROR_ALLOC).
 *         Chyby dekódování jednotlivých záznamů nejsou chybou analýzy.
 *
 * @post Při chybě může výsledek obsahovat jen total_pulses a pulse_stats,
 *       nic alokovaného; wav_analyzer_result_destroy() je i tak bezpečné.
 * @note Indexy pulzů ve výsledku (leader.start_index, header_leader_pulse,
 *       consumed_until_pulse, raw_blocks) se vztahují k @p seq.
 */
static en_WAV_ANALYZER_ERROR analyze_pulse_sequence (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_ANALYZER_CONFIG *config,
    st_WAV_ANALYZER_RESULT *out_result
) {
    en_WAV_ANALYZER_ERROR err;

    out_result->total_pulses = seq->count;

    /* výpočet statistik pulzů */
    wav_pulse_stats_compute ( seq, &out_result->pulse_stats );

    if ( config->verbose ) {
        if ( seq->count > 0 ) {
            fprintf ( stderr, "Pulse stats: avg=%.1f us, stddev=%.1f us, min=%.1f us, max=%.1f us\n",
                      out_result->pulse_stats.avg_us,
                      out_result->pulse_stats.stddev_us,
                      out_result->pulse_stats.min_us,
                      out_result->pulse_stats.max_us );
        }
    }

    /* === 4. Detekce leader tónů === */
    err = wav_leader_find_all ( seq, config->min_leader_pulses,
                                config->tolerance, &out_result->leaders );
    if ( err != WAV_ANALYZER_OK ) {
        return err;
    }

    if ( config->verbose ) {
        fprintf ( stderr, "Leader detection done: %u leaders found\n",
                  out_result->leaders.count );
    }

    /*
     * === 4b. CPM-TAPE (Manchester) před průchody FM ===
     *
     * Pilot "011" (pulzy 2:1:1:2 půlek bitu) standardní detektor leaderu
     * nepozná; dekodér hledá pilot sám a rozpozná variantu Pezik/Intercopy.
     * Hledá se před leadery FM: úsek bajtů 00h v záznamu CPM vypadá jako
     * leader NORMAL a dekodér FM by si přivlastnil úsek až do dalšího
     * souboru. Leadery uvnitř záznamů CPM krok 5 přeskočí.
     * Po chybě dekódování (poškozený záznam) se hledá dál od místa chyby.
     */
    {
        uint32_t cpmtape_search = 0;

        while ( cpmtape_search < seq->count ) {
            st_MZF *mzf = NULL;
            st_WAV_DECODE_RESULT hdr_res, body_res;
            memset ( &hdr_res, 0, sizeof ( hdr_res ) );
            memset ( &body_res, 0, sizeof ( body_res ) );
            uint32_t consumed_until = 0;
            st_WAV_CPMTAPE_INFO cinfo;

            st_WAV_BODY_RECOVERY cpm_rec = { config->recover_body, 0, 0, 0 };
            err = wav_decode_cpmtape_decode_mzf (
                      seq, cpmtape_search,
                      &mzf, &hdr_res, &body_res, &consumed_until, &cinfo, &cpm_rec
                  );

            if ( err == WAV_ANALYZER_ERROR_NO_LEADER ) {
                /* žádný další pilot CPM-TAPE */
                free ( hdr_res.data );
                free ( body_res.data );
                break;
            }
            if ( err != WAV_ANALYZER_OK || !mzf ) {
                /* poškozený záznam: hledat dál za místem chyby */
                if ( config->verbose ) {
                    fprintf ( stderr, "CPM-TAPE record at pulse #%u not decoded: %s\n", cpmtape_search,
                              wav_analyzer_error_string ( err ) );
                }
                free ( hdr_res.data );
                free ( body_res.data );
                cpmtape_search = ( consumed_until > cpmtape_search ) ? consumed_until : cpmtape_search + 1;
                continue;
            }

            if ( config->verbose ) {
                fprintf ( stderr, "CPM-TAPE decoded: %s %u Bd, half-bit %.1f us, pilot %u/%u, header CRC=%s, "
                                  "body CRC=%s, body_size=%u\n",
                          cinfo.variant == WAV_CPMTAPE_INTERCOPY ? "Intercopy" : "Pezik", cinfo.bd, cinfo.half_us,
                          cinfo.pilot_header, cinfo.pilot_body,
                          ( hdr_res.crc_status == WAV_CRC_OK ) ? "OK" : "ERROR",
                          ( body_res.crc_status == WAV_CRC_OK ) ? "OK"
                          : ( body_res.crc_status == WAV_CRC_NOT_AVAILABLE ? "N/A" : "ERROR" ),
                          mzf->body_size );
            }

            st_WAV_ANALYZER_FILE_RESULT file_result;
            memset ( &file_result, 0, sizeof ( file_result ) );
            file_result.mzf = mzf;
            file_result.format = WAV_TAPE_FORMAT_CPM_TAPE;
            file_result.header_crc = hdr_res.crc_status;
            file_result.body_crc = body_res.crc_status;
            file_result.speed_class = WAV_SPEED_CLASS_FAST;
            file_result.consumed_until_pulse = consumed_until;
            file_result.recovery_status = cpm_rec.status;
            file_result.recovered_bytes = cpm_rec.read_bytes;
            file_result.expected_bytes = cpm_rec.expected_bytes;
            file_result.header_leader_pulse = cinfo.header_start_pulse;   /* začátek záznamu */
            file_result.cpmtape_variant = ( uint8_t ) cinfo.variant;
            file_result.cpmtape_bd = cinfo.bd;

            if ( result_add_file ( out_result, &file_result ) != WAV_ANALYZER_OK ) {
                mzf_free ( mzf );
            }

            free ( hdr_res.data );
            free ( body_res.data );

            cpmtape_search = ( consumed_until > cpmtape_search ) ? consumed_until : cpmtape_search + 1;
        }
        err = WAV_ANALYZER_OK;
    }

    /* === 5. Pro každý leader: klasifikace + dekódování === */

    /*
     * Dvoudílné formáty (TURBO, FASTIPL) konzumují leadery
     * svých datových částí. Sledujeme consumed_until_pulse
     * pro přeskočení těchto leaderů.
     */
    uint32_t skip_until_pulse = 0;

    for ( uint32_t i = 0; i < out_result->leaders.count; i++ ) {
        /* přeskočíme leadery spotřebované předchozím dekodérem.
         * Leader přeskočíme jen pokud jeho KONEC je v consumed oblasti.
         * Leader, který začíná těsně před consumed ale většinou leží za ní,
         * je platný (nastává na hranici TURBO body → další preloader LGAP). */
        uint32_t leader_end = out_result->leaders.leaders[i].start_index
                              + out_result->leaders.leaders[i].pulse_count;
        if ( leader_end < skip_until_pulse ) {
            if ( config->verbose ) {
                fprintf ( stderr, "Leader at pulse #%u: skipped (consumed by previous decode)\n",
                          out_result->leaders.leaders[i].start_index );
            }
            continue;
        }
        if ( in_cpmtape_record ( out_result, out_result->leaders.leaders[i].start_index ) ) {
            if ( config->verbose ) {
                fprintf ( stderr, "Leader at pulse #%u: skipped (inside a CPM-TAPE record)\n",
                          out_result->leaders.leaders[i].start_index );
            }
            continue;
        }

        uint32_t files_before = out_result->file_count;
        uint32_t leader_consumed = 0;
        err = process_leader ( seq, &out_result->leaders.leaders[i],
                               config, out_result, &leader_consumed );
        if ( err != WAV_ANALYZER_OK ) {
            /* chyba při zpracování jednoho leaderu - pokračujeme s dalšími */
            if ( config->verbose ) {
                fprintf ( stderr, "  Leader #%u processing failed: %s\n",
                          i, wav_analyzer_error_string ( err ) );
            }
        }

        /* aktualizujeme skip_until z consumed_until (i bez pridaneho souboru) */
        if ( leader_consumed > skip_until_pulse ) {
            skip_until_pulse = leader_consumed;
        }
        if ( out_result->file_count > files_before ) {
            uint32_t cu = out_result->files[out_result->file_count - 1].consumed_until_pulse;
            if ( cu > skip_until_pulse ) {
                skip_until_pulse = cu;
            }
        }
    }

    /*
     * === 5b. Sekundarni FM sken nepokrytych oblasti ===
     *
     * Rychle signaly (3200+ Bd pri 44100 Hz) maji tak malo vzorku
     * na pul-periodu (~4), ze kvantizacni jitter (~25%) znemoznuje
     * detekci leaderu. Pro tyto signaly skenujeme nepokryte oblasti
     * pomoci histogramove analyzy - najdeme SHORT/LONG peaky a
     * vytvorime synteticky leader pro FM dekoder.
     */
    {
        /* sestavime pokryte oblasti z dekodovanych souboru */
        uint32_t covered_end = 0;
        for ( uint32_t i = 0; i < out_result->file_count; i++ ) {
            if ( out_result->files[i].format == WAV_TAPE_FORMAT_CPM_TAPE ) continue;   /* CPM se hledá zvlášť (krok 4b) */
            uint32_t ce = out_result->files[i].consumed_until_pulse;
            if ( ce > covered_end ) covered_end = ce;
        }

        /* skenujeme oblast za poslednim dekodovanym souborem */
        if ( covered_end > 0 && covered_end < seq->count ) {
            uint32_t uncov_pulses = seq->count - covered_end;

            /* minimalne 50000 pulzu (cca 5 sec pri 200 us) = neprazdna oblast */
            if ( uncov_pulses >= 50000 ) {
                /*
                 * Histogramova analyza nepokryte oblasti -
                 * hledame FM signal (2 klastry, pomer 1.4-2.5).
                 */
                uint32_t hist_start = covered_end;
                uint32_t hist_count = uncov_pulses > 4000 ? 4000 : uncov_pulses;

                /* preskocime prvnich ~10000 pulzu (audio prefix, ticho) */
                uint32_t skip = uncov_pulses > 20000 ? 10000 : 0;
                hist_start += skip;
                hist_count = ( hist_start + hist_count <= seq->count )
                             ? hist_count
                             : seq->count - hist_start;

                st_WAV_HISTOGRAM uncov_hist;
                memset ( &uncov_hist, 0, sizeof ( uncov_hist ) );
                en_WAV_ANALYZER_ERROR herr = wav_histogram_analyze (
                    seq, hist_start, hist_count,
                    config->histogram_bin_width, &uncov_hist
                );

                if ( herr == WAV_ANALYZER_OK &&
                     uncov_hist.modulation == WAV_MODULATION_FM &&
                     uncov_hist.peak_count >= 2 &&
                     !in_cpmtape_record ( out_result, hist_start ) ) {
                    /*
                     * Nasli jsme FM signal. Vytvorime synteticky leader
                     * s prumerem z prvniho peaku (SHORT) a zavolame
                     * process_leader na zacatek FM oblasti.
                     */
                    double short_peak = uncov_hist.peaks[0].center_us;

                    /* najdeme zacatek FM signalu - prvni konzistentni pulz */
                    uint32_t fm_start = covered_end + skip;
                    for ( uint32_t p = fm_start; p < seq->count; p++ ) {
                        double dur = seq->pulses[p].duration_us;
                        if ( dur >= short_peak * 0.5 && dur <= short_peak * 3.0 ) {
                            fm_start = p;
                            break;
                        }
                    }

                    st_WAV_LEADER_INFO synth_leader;
                    memset ( &synth_leader, 0, sizeof ( synth_leader ) );
                    synth_leader.start_index = fm_start;
                    synth_leader.pulse_count = 200; /* minimalni */
                    synth_leader.avg_period_us = short_peak;

                    if ( config->verbose ) {
                        fprintf ( stderr, "FM scan: uncovered region at pulse #%u, "
                                  "histogram SHORT=%.1f us, trying FM decode\n",
                                  fm_start, short_peak );
                    }

                    uint32_t synth_consumed = 0;
                    err = process_leader ( seq, &synth_leader,
                                           config, out_result, &synth_consumed );

                    if ( err == WAV_ANALYZER_OK &&
                         out_result->file_count > 0 &&
                         out_result->files[out_result->file_count - 1].leader.start_index == fm_start ) {
                        if ( config->verbose ) {
                            st_WAV_ANALYZER_FILE_RESULT *last = &out_result->files[out_result->file_count - 1];
                            fprintf ( stderr, "  FM scan decoded: %s, body_size=%u, CRC=%s/%s\n",
                                      wav_tape_format_name ( last->format ),
                                      last->mzf ? last->mzf->body_size : 0,
                                      last->header_crc == WAV_CRC_OK ? "OK" : "ERR",
                                      last->body_crc == WAV_CRC_OK ? "OK"
                                      : ( last->body_crc == WAV_CRC_NOT_AVAILABLE ? "N/A" : "ERR" ) );
                        }
                    }
                }

                wav_histogram_destroy ( &uncov_hist );
            }
        }
    }

    /* seřadit soubory podle pozice na pásce (záznamy CPM-TAPE se hledají před FM, krok 4b) */
    sort_files_by_position ( out_result );

    /* === 7. Sběr neidentifikovaných bloků (raw blocks) === */
    if ( config->keep_unknown ) {
        err = collect_gaps ( out_result, seq, config );
        if ( err != WAV_ANALYZER_OK && config->verbose ) {
            fprintf ( stderr, "Warning: raw block collection failed: %s\n",
                      wav_analyzer_error_string ( err ) );
        }
        /* chyba sběru raw bloků není fatální */
        err = WAV_ANALYZER_OK;
    }

    /* === 8. Výpočet absolutních časů pro dekódované soubory === */
    for ( uint32_t i = 0; i < out_result->file_count; i++ ) {
        st_WAV_ANALYZER_FILE_RESULT *f = &out_result->files[i];
        uint32_t start_idx = f->header_leader_pulse;
        uint32_t end_idx = f->consumed_until_pulse;
        f->start_time_sec = pulse_index_to_sec ( seq, start_idx );
        if ( end_idx > start_idx ) {
            double end_time = pulse_index_to_sec ( seq, end_idx );
            f->duration_sec = end_time - f->start_time_sec;
        } else {
            f->duration_sec = 0.0;
        }
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_analyzer_analyze (
    st_HANDLER *h,
    const st_WAV_ANALYZER_CONFIG *config,
    st_WAV_ANALYZER_RESULT *out_result
) {
    if ( !h || !config || !out_result ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    memset ( out_result, 0, sizeof ( *out_result ) );

    /* === 1. Parsování WAV hlavičky === */
    en_WAV_ERROR wav_err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &wav_err );
    if ( !sh ) {
        /* konkrétní důvod z knihovny wav (dřív se zahodil) */
        fprintf ( stderr, "WAV header: %s\n", wav_error_string ( wav_err ) );
        return WAV_ANALYZER_ERROR_WAV_FORMAT;
    }

    out_result->sample_rate = sh->sample_rate;
    out_result->wav_duration_sec = sh->count_sec;

    if ( config->verbose ) {
        fprintf ( stderr, "WAV: %u Hz, %u-bit, %u ch, %.2f sec, %u samples\n",
                  sh->sample_rate, sh->bits_per_sample,
                  sh->channels, sh->count_sec, sh->blocks );
    }

    /* === 2. Preprocessing === */
    double *samples = NULL;
    uint32_t sample_count = 0;

    en_WAV_ANALYZER_ERROR err = wav_preprocess_run ( h, sh, config, &samples, &sample_count );
    wav_simple_header_destroy ( sh );

    if ( err != WAV_ANALYZER_OK ) {
        return err;
    }

    if ( config->verbose ) {
        fprintf ( stderr, "Preprocessing done: %u samples\n", sample_count );
    }

    /* === 3. Extrakce pulzů === */
    st_WAV_PULSE_SEQUENCE pulse_seq;
    memset ( &pulse_seq, 0, sizeof ( pulse_seq ) );

    err = wav_pulse_extract ( samples, sample_count, out_result->sample_rate,
                              config, &pulse_seq );
    free ( samples );

    if ( err != WAV_ANALYZER_OK ) {
        return err;
    }

    if ( config->verbose ) {
        fprintf ( stderr, "Pulse extraction done: %u pulses\n", pulse_seq.count );
    }

    err = analyze_pulse_sequence ( &pulse_seq, config, out_result );
    wav_pulse_sequence_destroy ( &pulse_seq );
    return err;
}


/**
 * @brief Zjistí, zda má pulz použitelnou délku (konečné kladné duration_us).
 * @param p Pulz.
 * @return 1 = délka je kladná a konečná, 0 = nulová, záporná, NaN nebo nekonečno.
 */
static int pulse_has_duration ( const st_WAV_PULSE *p ) {
    return p->duration_us > 0.0 && isfinite ( p->duration_us );
}


/**
 * @brief Nejvyšší počet vzorků pulzu omezeného na WAV_ANALYZER_PULSE_MAX_US.
 * @param sample_rate Vzorkovací frekvence (Hz, > 0).
 * @return round ( WAV_ANALYZER_PULSE_MAX_US * sample_rate / 1e6 ), nejvýš UINT32_MAX.
 */
static uint32_t pulse_max_samples ( uint32_t sample_rate ) {
    double s = round ( WAV_ANALYZER_PULSE_MAX_US * ( double ) sample_rate / 1000000.0 );
    return ( s >= ( double ) UINT32_MAX ) ? UINT32_MAX : ( uint32_t ) s;
}


/**
 * @brief Omezí délku pulzu na WAV_ANALYZER_PULSE_MAX_US (us i vzorky).
 * @param p Pulz s použitelnou délkou (pulse_has_duration()); mění se na místě.
 * @param max_samples Výsledek pulse_max_samples() pro frekvenci posloupnosti.
 */
static void pulse_clamp ( st_WAV_PULSE *p, uint32_t max_samples ) {
    if ( p->duration_us > WAV_ANALYZER_PULSE_MAX_US ) {
        p->duration_us = WAV_ANALYZER_PULSE_MAX_US;
        if ( p->duration_samples > max_samples ) p->duration_samples = max_samples;
    }
}


/**
 * @brief Připraví posloupnost pro analýzu: vynechá pulzy bez použitelné délky, omezí příliš dlouhé.
 *
 * Pulz s nulovou (zápornou, NaN, nekonečnou) délkou nenese hranu, ve
 * skutečnosti se úroveň nezměnila. Vynechá se a pulz za ním, který má
 * stejnou úroveň jako poslední ponechaný pulz, se k němu přičte (délka v us
 * i ve vzorcích, vzorky se saturují na UINT32_MAX). Pulz delší než
 * WAV_ANALYZER_PULSE_MAX_US (i vzniklý sloučením) se zkrátí na tuto mez
 * (duration_us, duration_samples nejvýš pulse_max_samples()); další vrstvy
 * proto nikdy nedostanou nekonečno ani délku, která by přetekla převody na
 * celá čísla. Bez takových pulzů se nic nekopíruje: @p out pak sdílí pole
 * pulzů se vstupem.
 *
 * @param in Vstupní posloupnost (count > 0 => pulses != NULL). Nemění se.
 * @param[out] out Posloupnost pro analýzu (sample_rate jako vstup).
 * @param[out] out_copied 1 = out->pulses je nová alokace, kterou volající
 *             uvolní free(); 0 = out->pulses ukazuje do vstupu (neuvolňovat).
 * @return WAV_ANALYZER_OK, nebo WAV_ANALYZER_ERROR_ALLOC (out je pak
 *         prázdný a *out_copied 0).
 *
 * @post Každý pulz v @p out má 0 < duration_us <= WAV_ANALYZER_PULSE_MAX_US.
 * @note Indexy pulzů v @p out se po vynechání posunou proti vstupu. Součet
 *       délek v us je součet kladných délek vstupu, zmenšený o zkrácení nad
 *       mez (vynechané záporné délky se nepočítají).
 */
static en_WAV_ANALYZER_ERROR pulse_sequence_prepare (
    const st_WAV_PULSE_SEQUENCE *in,
    st_WAV_PULSE_SEQUENCE *out,
    int *out_copied
) {
    *out = *in;
    *out_copied = 0;

    /* kopie je potřeba, jen když je co vynechat nebo zkrátit */
    uint32_t first_bad = 0;
    while ( first_bad < in->count && pulse_has_duration ( &in->pulses[first_bad] )
            && in->pulses[first_bad].duration_us <= WAV_ANALYZER_PULSE_MAX_US ) {
        first_bad++;
    }
    if ( first_bad == in->count ) return WAV_ANALYZER_OK;

    /* velikost pole bez přetečení size_t (32bitová platforma) */
    size_t bytes = ( size_t ) in->count * sizeof ( st_WAV_PULSE );
    st_WAV_PULSE *buf = ( bytes / sizeof ( st_WAV_PULSE ) == in->count ) ? ( st_WAV_PULSE* ) malloc ( bytes ) : NULL;
    if ( !buf ) {
        out->pulses = NULL;
        out->count = out->capacity = 0;
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    uint32_t max_samples = pulse_max_samples ( in->sample_rate );
    uint32_t n = first_bad;
    memcpy ( buf, in->pulses, ( size_t ) first_bad * sizeof ( st_WAV_PULSE ) );
    int skipped = 0;   /* od posledního ponechaného pulzu se něco vynechalo */
    for ( uint32_t i = first_bad; i < in->count; i++ ) {
        const st_WAV_PULSE *p = &in->pulses[i];
        if ( !pulse_has_duration ( p ) ) {
            skipped = 1;
            continue;
        }
        if ( skipped && n > 0 && buf[n - 1].level == p->level ) {
            /*
             * vynechaný pulz nebyl hrana: sloučit se stejnou úrovní před ním;
             * obě délky jsou nejvýš mez, součet je konečný a pak se omezí
             */
            buf[n - 1].duration_us += ( p->duration_us > WAV_ANALYZER_PULSE_MAX_US ) ? WAV_ANALYZER_PULSE_MAX_US
                                      : p->duration_us;
            uint64_t s = ( uint64_t ) buf[n - 1].duration_samples + p->duration_samples;
            buf[n - 1].duration_samples = ( s > UINT32_MAX ) ? UINT32_MAX : ( uint32_t ) s;
            pulse_clamp ( &buf[n - 1], max_samples );
        } else {
            buf[n] = *p;
            pulse_clamp ( &buf[n], max_samples );
            n++;
        }
        skipped = 0;
    }

    out->pulses = buf;
    out->count = n;
    out->capacity = in->count;
    *out_copied = 1;
    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_analyzer_analyze_pulses (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_ANALYZER_CONFIG *config,
    st_WAV_ANALYZER_RESULT *result
) {
    if ( !result ) return WAV_ANALYZER_ERROR_INVALID_PARAM;
    memset ( result, 0, sizeof ( *result ) );
    if ( !seq || !config || seq->sample_rate == 0 || ( seq->count > 0 && !seq->pulses ) ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    st_WAV_PULSE_SEQUENCE work;
    int copied = 0;
    en_WAV_ANALYZER_ERROR err = pulse_sequence_prepare ( seq, &work, &copied );
    if ( err != WAV_ANALYZER_OK ) return err;

    result->sample_rate = work.sample_rate;
    double total_us = 0.0;
    for ( uint32_t i = 0; i < work.count; i++ ) total_us += work.pulses[i].duration_us;
    result->wav_duration_sec = total_us / 1000000.0;

    if ( config->verbose ) {
        fprintf ( stderr, "Pulse sequence: %u Hz, %.2f sec, %u pulses (input %u, zero-length pulses removed)\n",
                  work.sample_rate, result->wav_duration_sec, work.count, seq->count );
    }

    err = analyze_pulse_sequence ( &work, config, result );
    if ( copied ) free ( work.pulses );
    if ( err != WAV_ANALYZER_OK ) {
        /* kontrakt: při chybě je výsledek celý vynulovaný (i sample_rate, statistiky) */
        wav_analyzer_result_destroy ( result );
        memset ( result, 0, sizeof ( *result ) );
    }
    return err;
}


void wav_analyzer_result_destroy ( st_WAV_ANALYZER_RESULT *result ) {
    if ( !result ) return;

    for ( uint32_t i = 0; i < result->file_count; i++ ) {
        mzf_free ( result->files[i].mzf );
        result->files[i].mzf = NULL;
        free ( result->files[i].tap_data );
        result->files[i].tap_data = NULL;
    }
    free ( result->files );
    result->files = NULL;
    result->file_count = 0;
    result->file_capacity = 0;

    for ( uint32_t i = 0; i < result->raw_block_count; i++ ) {
        wav_analyzer_raw_block_destroy ( &result->raw_blocks[i] );
    }
    free ( result->raw_blocks );
    result->raw_blocks = NULL;
    result->raw_block_count = 0;
    result->raw_block_capacity = 0;

    wav_leader_list_destroy ( &result->leaders );
}


void wav_analyzer_print_summary (
    const st_WAV_ANALYZER_RESULT *result,
    FILE *stream,
    int verbose
) {
    if ( !result || !stream ) return;

    fprintf ( stream, "=== WAV Analysis Summary ===\n" );
    fprintf ( stream, "Sample rate: %u Hz\n", result->sample_rate );
    fprintf ( stream, "Duration: %.2f sec\n", result->wav_duration_sec );
    fprintf ( stream, "Total pulses: %u\n", result->total_pulses );
    fprintf ( stream, "Leaders found: %u\n", result->leaders.count );
    fprintf ( stream, "Files decoded: %u\n", result->file_count );

    /* statistiky pulzů */
    if ( result->pulse_stats.count > 0 ) {
        fprintf ( stream, "\nPulse statistics:\n" );
        fprintf ( stream, "  Count:  %u\n", result->pulse_stats.count );
        fprintf ( stream, "  Avg:    %.1f us\n", result->pulse_stats.avg_us );
        fprintf ( stream, "  Stddev: %.1f us\n", result->pulse_stats.stddev_us );
        fprintf ( stream, "  Min:    %.1f us\n", result->pulse_stats.min_us );
        fprintf ( stream, "  Max:    %.1f us\n", result->pulse_stats.max_us );
    }

    for ( uint32_t i = 0; i < result->file_count; i++ ) {
        const st_WAV_ANALYZER_FILE_RESULT *f = &result->files[i];

        fprintf ( stream, "\n--- File #%u ---\n", i + 1 );
        fprintf ( stream, "Format: %s\n", wav_tape_format_name ( f->format ) );
        if ( f->format == WAV_TAPE_FORMAT_CPM_TAPE && f->cpmtape_variant != WAV_CPMTAPE_NONE ) {
            fprintf ( stream, "CPM-TAPE: %s, %u Bd\n", f->cpmtape_variant == WAV_CPMTAPE_INTERCOPY ? "Intercopy" : "Pezik TAPE.COM",
                      f->cpmtape_bd );
        }
        fprintf ( stream, "Start: %.3f sec, Duration: %.3f sec\n",
                  f->start_time_sec, f->duration_sec );
        fprintf ( stream, "Speed class: %s\n", wav_speed_class_name ( f->speed_class ) );
        fprintf ( stream, "Leader: %u pulses, avg %.1f us, stddev %.1f us\n",
                  f->leader.pulse_count, f->leader.avg_period_us, f->leader.stddev_us );

        /* verbose: reálná rychlost, přibližná rychlost, pulzní sada */
        if ( verbose ) {
            /* pulzní sada dle formátu */
            const char *pulseset_name;
            uint16_t base_bd;
            switch ( f->format ) {
                case WAV_TAPE_FORMAT_MZ80B:
                    pulseset_name = "MZ-80B";
                    base_bd = 1800;
                    break;
                default:
                    pulseset_name = "MZ-800";
                    base_bd = 1200;
                    break;
            }
            fprintf ( stream, "Pulse set: %s\n", pulseset_name );

            /* reálná a přibližná rychlost pro FM formáty */
            if ( f->leader.avg_period_us > 0.0 &&
                 f->format != WAV_TAPE_FORMAT_ZX_SPECTRUM ) {
                double real_bd = ( double ) base_bd * NORMAL_1200BD_SHORT_US
                                 / f->leader.avg_period_us;
                fprintf ( stream, "Real speed: %.0f Bd (leader avg %.1f us)\n",
                          real_bd, f->leader.avg_period_us );

                /* nejbližší standardní rychlost */
                en_CMTSPEED approx = cmtspeed_from_bdspeed (
                    ( uint16_t ) round ( real_bd ), base_bd );
                if ( cmtspeed_is_valid ( approx ) ) {
                    char speed_txt[64];
                    cmtspeed_get_ratiospeedtxt ( speed_txt, sizeof ( speed_txt ),
                                                 approx, base_bd );
                    fprintf ( stream, "Approx speed: %s\n", speed_txt );
                }
            }

            /* naměřené délky pulzů (pokud jsou k dispozici) */
            if ( f->short_pulse_us > 0.0 || f->long_pulse_us > 0.0 ) {
                fprintf ( stream, "Measured pulses: short=%.1f us, long=%.1f us\n",
                          f->short_pulse_us, f->long_pulse_us );
            }
        }

        if ( f->tap_data && f->tap_data_size > 0 ) {
            /* ZX Spectrum blok */
            fprintf ( stream, "Checksum: %s\n",
                      ( f->header_crc == WAV_CRC_OK ) ? "OK" :
                      ( f->header_crc == WAV_CRC_ERROR ) ? "ERROR" : "N/A" );
            fprintf ( stream, "TAP flag: 0x%02X (%s)\n",
                      f->tap_data[0],
                      f->tap_data[0] == 0x00 ? "header" :
                      f->tap_data[0] == 0xFF ? "data" : "custom" );
            fprintf ( stream, "TAP size: %u bytes\n", f->tap_data_size );
        } else if ( f->mzf ) {
            fprintf ( stream, "Header CRC: %s\n",
                      ( f->header_crc == WAV_CRC_OK ) ? "OK" :
                      ( f->header_crc == WAV_CRC_ERROR ) ? "ERROR" : "N/A" );
            fprintf ( stream, "Body CRC: %s\n",
                      ( f->body_crc == WAV_CRC_OK ) ? "OK" :
                      ( f->body_crc == WAV_CRC_ERROR ) ? "ERROR" : "N/A" );
            if ( f->format == WAV_TAPE_FORMAT_BSD ) {
                /* CRC každého kusu, kusy z kopie */
                fprintf ( stream, "BSD chunks: %u, CRC errors: %u, from copy: %u\n", f->bsd_chunks,
                          f->bsd_crc_errors, f->bsd_from_copy );
            }

            fprintf ( stream, "MZF type: 0x%02X\n", f->mzf->header.ftype );
            fprintf ( stream, "MZF size: %u bytes\n", f->mzf->header.fsize );
            fprintf ( stream, "MZF start: 0x%04X\n", f->mzf->header.fstrt );
            fprintf ( stream, "MZF exec: 0x%04X\n", f->mzf->header.fexec );

            /* název souboru - Sharp MZ ASCII -> standardní ASCII */
            char fname[MZF_FILE_NAME_LENGTH + 1];
            memcpy ( fname, f->mzf->header.fname.name, MZF_FILE_NAME_LENGTH );
            fname[MZF_FILE_NAME_LENGTH] = '\0';
            /* nahradíme non-printable znaky tečkami */
            for ( int j = 0; j < MZF_FILE_NAME_LENGTH; j++ ) {
                if ( fname[j] < 0x20 || fname[j] > 0x7E ) fname[j] = '.';
            }
            fprintf ( stream, "MZF name: \"%s\"\n", fname );
        }

        if ( f->copy_diff_bytes ) {
            fprintf ( stream, "WARNING: copies differ in %u bytes although the checksum is valid\n", f->copy_diff_bytes );
        }
        if ( f->recovery_status != WAV_RECOVERY_NONE ) {
            fprintf ( stream, "*** RECOVERED: %s", wav_recovery_status_string ( f->recovery_status ) );
            if ( f->recovered_bytes > 0 ) {
                fprintf ( stream, " (%u bytes salvaged)", f->recovered_bytes );
            }
            fprintf ( stream, " ***\n" );
        }

        if ( f->copy2_used ) {
            fprintf ( stream, "Copy2: yes (data from second copy)\n" );
        }

        fflush ( stream );
    }

    /* raw bloky */
    if ( result->raw_block_count > 0 ) {
        double total_raw_duration = 0.0;
        for ( uint32_t i = 0; i < result->raw_block_count; i++ ) {
            total_raw_duration += result->raw_blocks[i].duration_sec;
        }

        fprintf ( stream, "\nRaw blocks: %u (%.2f sec total)\n", result->raw_block_count, total_raw_duration );
        for ( uint32_t i = 0; i < result->raw_block_count; i++ ) {
            const st_WAV_ANALYZER_RAW_BLOCK *rb = &result->raw_blocks[i];
            fprintf ( stream, "  [%u] pulses %u-%u, %.3f sec at %.3f sec, %u bytes\n",
                      i + 1, rb->pulse_start, rb->pulse_end,
                      rb->duration_sec, rb->start_time_sec, rb->data_size );
        }
    }

    fprintf ( stream, "\n" );
}


const char* wav_analyzer_version ( void ) {
    return WAV_ANALYZER_VERSION;
}
