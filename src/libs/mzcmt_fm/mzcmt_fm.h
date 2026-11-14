/**
 * @file   mzcmt_fm.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  Obecný generátor FM z parametrů části bloku 0x46 (TMZ v1.1).
 *
 * FM kóduje bit délkou pulzu: SHORT = 0, LONG = 1. Pulz má dvě půlvlny;
 * při level = 1 je první HIGH a druhá LOW. Generátor sestaví posloupnost
 * pulzů části (tón, tapemark, bajty se start bity, checksum, koncovka,
 * kopie) přesně podle tmz-v1.1-specification.md kap. 6.2 a délky
 * půlvln vyhodnotí podle kontextů kap. 6.3: položka (kontext, druh, další
 * druh), pak (kontext, druh, libovolný), pak rodič kontextu, nakonec
 * základ PULSES.
 *
 * Délky jsou celá čísla v jednotkách časové základny části (timebase Hz):
 * T-stavy CPU, tiky CTC, µs. Převod na vzorky (mzcmt_fm_create_vstream)
 * akumuluje čas od začátku části, takže se zaokrouhlovací chyba nesčítá.
 *
 * Knihovna nezávisí na formátu TMZ: parametry plní volající (např.
 * tmz_part_fm z TLV bloku 0x46).
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


#ifndef MZCMT_FM_H
#define MZCMT_FM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "../cmt_stream/cmt_vstream.h"


/** @brief Výchozí časová základna: takt CPU MZ-800 v Hz (T-stavy). */
#define MZCMT_FM_DEFAULT_TIMEBASE 3546895u
/** @brief Druh pulzu SHORT (bit 0). */
#define MZCMT_FM_KIND_SHORT 0
/** @brief Druh pulzu LONG (bit 1, start bit, tapemark L). */
#define MZCMT_FM_KIND_LONG  1
/** @brief Položka kontextu platí pro libovolný druh následujícího pulzu. */
#define MZCMT_FM_NEXT_ANY   0xFF
/** @brief Nejvíc položek kontextu: 20 kontextů x 2 druhy x 3 (S, L, libovolný). */
#define MZCMT_FM_MAX_CONTEXT 120
/**
 * @brief Nejvíc SHORT v tónu jedné části (ROM 22 000; 45násobek).
 *
 * Meze MZCMT_FM_MAX_* chrání před souborem, který by počtem pulzů nebo
 * délkou signálu zahltil paměť a čas (pole jsou 32bitová). Vycházejí
 * z délky signálu: jedna část (hlavička nebo tělo MZF) je i s kopiemi
 * a pauzou kratší než hodina pásky.
 */
#define MZCMT_FM_MAX_TONE 1000000u
/** @brief Nejdelší půlvlna (PULSES, CONTEXT) v sekundách časové základny části. */
#define MZCMT_FM_MAX_HALF_SECONDS 1u
/** @brief Nejdelší signál jedné části včetně kopií a pauzy v sekundách časové základny. */
#define MZCMT_FM_MAX_PART_SECONDS 3600u
/** @brief Nejvíc pulzů jedné části (tón, tapemark, data se start bity a checksumem, END, TAIL, kopie, oddělovače). */
#define MZCMT_FM_MAX_PULSES 16000000u
/** @brief Verze knihovny mzcmt_fm. */
#define MZCMT_FM_VERSION "1.2.0"


    /**
     * @brief Kontext pulzu (spec 6.3). Rodiče: *_LAST -> základní kontext,
     *        BIT_LAST_DATA_END -> BIT_LAST, CK_START -> START, CK_BIT -> BIT,
     *        CK_BIT_LAST -> BIT_LAST, CK_BIT_END -> CK_BIT_LAST.
     */
    typedef enum en_MZCMT_FM_CTX {
        MZCMT_FM_CTX_TONE = 0,            /**< pulz tónu (GAP) */
        MZCMT_FM_CTX_TONE_LAST,           /**< poslední pulz tónu */
        MZCMT_FM_CTX_TM_L,                /**< LONG tapemarku */
        MZCMT_FM_CTX_TM_L_LAST,           /**< poslední LONG tapemarku */
        MZCMT_FM_CTX_TM_S,                /**< SHORT tapemarku */
        MZCMT_FM_CTX_TM_S_LAST,           /**< poslední SHORT tapemarku */
        MZCMT_FM_CTX_TM_END_L,            /**< LONG za tapemarkem */
        MZCMT_FM_CTX_START,               /**< start bit datového bajtu */
        MZCMT_FM_CTX_BIT,                 /**< datový bit na pozici 0-6 */
        MZCMT_FM_CTX_BIT_LAST,            /**< datový bit na pozici 7 */
        MZCMT_FM_CTX_BIT_LAST_DATA_END,   /**< pozice 7 posledního datového bajtu */
        MZCMT_FM_CTX_CK_START,            /**< start bit bajtu checksumu */
        MZCMT_FM_CTX_CK_BIT,              /**< bit checksumu na pozici 0-6 */
        MZCMT_FM_CTX_CK_BIT_LAST,         /**< bit checksumu na pozici 7 */
        MZCMT_FM_CTX_CK_BIT_END,          /**< pozice 7 posledního bajtu checksumu */
        MZCMT_FM_CTX_END_L,               /**< LONG za checksumem */
        MZCMT_FM_CTX_TAIL,                /**< SHORT koncovky */
        MZCMT_FM_CTX_TAIL_LAST,           /**< poslední SHORT koncovky */
        MZCMT_FM_CTX_COPY_SEP,            /**< SHORT oddělovače kopie */
        MZCMT_FM_CTX_COPY_SEP_LAST,       /**< poslední SHORT oddělovače kopie */
        MZCMT_FM_CTX_COUNT                /**< počet kontextů (není platný kontext) */
    } en_MZCMT_FM_CTX;


    /**
     * @brief Algoritmus checksumu (spec 6.5).
     */
    typedef enum en_MZCMT_FM_CHECKSUM {
        MZCMT_FM_CHECKSUM_NONE = 0,               /**< bez checksumu */
        MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE = 1,   /**< počet jedniček v bajtech, 16 bit, vyšší bajt první (ROM @CHECK) */
    } en_MZCMT_FM_CHECKSUM;


    /**
     * @brief Položka kontextové délky půlvln.
     *
     * @invariant ctx < MZCMT_FM_CTX_COUNT, kind <= 1, next <= 1 nebo MZCMT_FM_NEXT_ANY
     * @invariant first > 0, second > 0
     */
    typedef struct st_MZCMT_FM_CONTEXT {
        uint8_t ctx;      /**< en_MZCMT_FM_CTX */
        uint8_t kind;     /**< MZCMT_FM_KIND_* */
        uint8_t next;     /**< druh následujícího pulzu, nebo MZCMT_FM_NEXT_ANY */
        uint32_t first;   /**< první půlvlna (jednotky timebase) */
        uint32_t second;  /**< druhá půlvlna (jednotky timebase) */
    } st_MZCMT_FM_CONTEXT;


    /**
     * @brief Parametry jedné části FM (efektivní hodnoty, spec 5.3 a 6.1).
     *
     * @invariant Platnost ověřuje mzcmt_fm_params_validate(): timebase > 0,
     *            všechny PULSES > 0, level a lsb_first 0/1, známý checksum,
     *            checksum_explicit jen s MZ_POPCOUNT16_BE, context_count
     *            <= MZCMT_FM_MAX_CONTEXT, položky kontextu platné a bez
     *            opakované trojice (ctx, kind, next); tone <=
     *            MZCMT_FM_MAX_TONE, každá půlvlna nejvýš
     *            MZCMT_FM_MAX_HALF_SECONDS x timebase, pause nejvýš
     *            MZCMT_FM_MAX_PART_SECONDS x timebase.
     */
    typedef struct st_MZCMT_FM_PARAMS {
        uint32_t timebase;          /**< časová základna v Hz */
        uint8_t level;              /**< 1 = první půlvlna HIGH, 0 = LOW */
        uint32_t pause;             /**< ticho za částí (jednotky timebase) */
        uint8_t pause_level;        /**< úroveň během pauzy: 0 = LOW (výchozí), 1 = HIGH (Intercopy) */
        uint32_t short_first;       /**< SHORT první půlvlna */
        uint32_t short_second;      /**< SHORT druhá půlvlna */
        uint32_t long_first;        /**< LONG první půlvlna */
        uint32_t long_second;       /**< LONG druhá půlvlna */
        uint32_t tone;              /**< počet SHORT v tónu */
        uint16_t tm_long;           /**< počet LONG tapemarku */
        uint16_t tm_short;          /**< počet SHORT tapemarku */
        uint8_t tm_end_long;        /**< počet LONG za tapemarkem */
        uint8_t start_bits;         /**< počet start bitů (LONG) před bajtem */
        uint8_t lsb_first;          /**< 0 = MSB první, 1 = LSB první */
        uint8_t checksum;           /**< en_MZCMT_FM_CHECKSUM */
        uint8_t checksum_explicit;  /**< 1 = zapsat checksum_bytes místo vypočtených */
        uint8_t checksum_bytes[2];  /**< explicitní bajty checksumu v pořadí zápisu */
        uint8_t end_long;           /**< počet LONG za checksumem */
        uint16_t tail_short;        /**< počet SHORT na konci datové sekce */
        uint8_t copies;             /**< počet kopií datové sekce */
        uint16_t copy_sep;          /**< počet SHORT oddělovače před kopií */
        uint32_t context_count;     /**< počet platných položek context */
        st_MZCMT_FM_CONTEXT context[MZCMT_FM_MAX_CONTEXT];  /**< kontextové délky */
    } st_MZCMT_FM_PARAMS;


    /**
     * @brief Výstup generátoru: jedna půlvlna.
     *
     * @param user Uživatelská data z mzcmt_fm_generate().
     * @param level Úroveň: 1 = HIGH, 0 = LOW.
     * @param duration Délka v jednotkách timebase (> 0).
     * @return EXIT_SUCCESS pokračuje, jiná hodnota generování ukončí s chybou.
     */
    typedef int ( *mzcmt_fm_half_cb ) ( void *user, int level, uint32_t duration );


    /**
     * @brief Nastaví výchozí hodnoty podle spec (TIMEBASE 3 546 895, LEVEL 1,
     *        1 start bit, MSB první, ostatní 0).
     * @param p Parametry (nesmí být NULL).
     * @post PULSES jsou 0, parametry tedy ještě nejsou platné.
     */
    extern void mzcmt_fm_params_init ( st_MZCMT_FM_PARAMS *p );

    /**
     * @brief Ověří invarianty st_MZCMT_FM_PARAMS (včetně mezí tónu, půlvln a pauzy).
     * @param p Parametry (NULL = neplatné).
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE.
     */
    extern int mzcmt_fm_params_validate ( const st_MZCMT_FM_PARAMS *p );

    /**
     * @brief Ověří meze signálu části s daným počtem datových bajtů.
     *
     * Počet pulzů (tón, tapemark, size bajtů a checksum se start bity, END,
     * TAIL, to celé 1 + copies krát, oddělovače kopií) nesmí přesáhnout
     * MZCMT_FM_MAX_PULSES a horní odhad délky (počet pulzů x nejdelší
     * dvojice půlvln + pause) MZCMT_FM_MAX_PART_SECONDS x timebase.
     * mzcmt_fm_generate() ji volá sama; player ji volá předem, aby část
     * mimo meze přeskočil jako neplatnou.
     *
     * @param p Parametry (musí projít mzcmt_fm_params_validate()).
     * @param size Počet datových bajtů části.
     * @return EXIT_SUCCESS v mezích; EXIT_FAILURE mimo meze nebo s neplatnými parametry.
     */
    extern int mzcmt_fm_check_size ( const st_MZCMT_FM_PARAMS *p, uint32_t size );

    /**
     * @brief Meze části jako mzcmt_fm_check_size() a k tomu odhady pro mez celého bloku.
     *
     * @param p Parametry (musí projít mzcmt_fm_params_validate()).
     * @param size Počet datových bajtů části.
     * @param[out] pulses Počet pulzů části (smí být NULL).
     * @param[out] units Horní odhad délky části v jednotkách timebase
     *        (pulzy x nejdelší dvojice půlvln + pause; smí být NULL).
     * @return EXIT_SUCCESS v mezích části (výstupy vyplněné); EXIT_FAILURE
     *         mimo meze nebo s neplatnými parametry (výstupy beze změny).
     */
    extern int mzcmt_fm_part_bounds ( const st_MZCMT_FM_PARAMS *p, uint32_t size, uint64_t *pulses, uint64_t *units );

    /**
     * @brief Vygeneruje půlvlny jedné části FM.
     *
     * Posloupnost (spec 6.2): tón, tapemark (L, S, L za tapemarkem), datová
     * sekce (bajty se start bity, checksum, END L, TAIL S), pro každou kopii
     * oddělovač a znovu datová sekce. Délky podle kontextů (spec 6.3). Je-li
     * pause > 0, následuje jedna půlvlna délky pause s úrovní pause_level
     * (LOW splyne s druhou půlvlnou posledního pulzu, HIGH s první půlvlnou
     * HIGH další části).
     *
     * @param p Parametry (musí projít mzcmt_fm_params_validate()).
     * @param data Bajty části (smí být NULL jen při size == 0).
     * @param size Počet bajtů.
     * @param cb Výstup půlvln.
     * @param user Data pro cb.
     * @return EXIT_SUCCESS; EXIT_FAILURE při neplatných parametrech, signálu
     *         mimo meze (mzcmt_fm_check_size()), selhání alokace nebo chybě
     *         z cb (generování se zastaví).
     *
     * @note Pracovní tabulka se alokuje na haldě a uvolní před návratem.
     */
    extern int mzcmt_fm_generate ( const st_MZCMT_FM_PARAMS *p, const uint8_t *data, uint32_t size,
                                   mzcmt_fm_half_cb cb, void *user );

    /**
     * @brief Připojí část k existujícímu vstreamu.
     *
     * Hrany se umístí podle akumulovaného času od začátku této části:
     * vzorek hrany = floor((cum * rate + timebase / 2) / timebase), kde cum je
     * součet délek dosavadních půlvln části (spec 5.4). Zaokrouhlovací chyba
     * se proto nesčítá. Převod je bez přetečení i při časové základně blízko
     * 2^32 (cmt_vstream_scale_time(): dělení před násobením).
     *
     * @param vs Cílový vstream (vzorkovací frekvence z cmt_vstream_get_rate()).
     * @param p Parametry části.
     * @param data Bajty části.
     * @param size Počet bajtů.
     * @return EXIT_SUCCESS; EXIT_FAILURE při neplatných parametrech, chybě
     *         vstreamu nebo půlvlně kratší než jeden vzorek (chyba se vypíše
     *         na stderr). Výjimkou je pauza: když po zaokrouhlení nedá ani
     *         jeden vzorek, vynechá se.
     *
     * @warning Při chybě zůstanou ve vstreamu rozpracovaná data; volající ho
     *          má zahodit.
     */
    extern int mzcmt_fm_append_vstream ( st_CMT_VSTREAM *vs, const st_MZCMT_FM_PARAMS *p,
                                         const uint8_t *data, uint32_t size );

    /**
     * @brief Vytvoří vstream s jednou částí FM.
     *
     * Počáteční hodnota vstreamu je úroveň první půlvlny (p->level).
     *
     * @param p Parametry části.
     * @param data Bajty části.
     * @param size Počet bajtů.
     * @param rate Vzorkovací frekvence v Hz.
     * @return Nový vstream, nebo NULL při chybě (viz mzcmt_fm_append_vstream()).
     *
     * @post Volající vlastní vstream a uvolní ho cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* mzcmt_fm_create_vstream ( const st_MZCMT_FM_PARAMS *p, const uint8_t *data,
                                                     uint32_t size, uint32_t rate );

    /**
     * @brief Vrátí verzi knihovny.
     * @return Statický řetězec MZCMT_FM_VERSION.
     */
    extern const char* mzcmt_fm_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZCMT_FM_H */
