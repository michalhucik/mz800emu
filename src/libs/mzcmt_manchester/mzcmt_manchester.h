/**
 * @file   mzcmt_manchester.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.0
 * @brief  Obecný generátor rodiny MANCHESTER z parametrů části bloku 0x46 (TMZ v1.1).
 *
 * Manchester (CPM-TAPE: Pezik/MarVan TAPE.COM, Intercopy "CPM") kóduje bit
 * změnou úrovně uprostřed bitu: při level = 1 je bit 1 první půlka HIGH
 * a druhá LOW, bit 0 obráceně. Bajty jdou LSB první, bez start a stop bitu.
 * Generátor sestaví posloupnost bitů části (pilot, sync, flag, data,
 * checksum, koncové bity) přesně podle tmz-v1.1-specification.md
 * kap. 7.2. První půlka bitu závisí na hodnotě bitu, druhá na kontextu
 * přechodu (co následuje) a na hodnotě bitu; u přechodů mezi datovými
 * bajty navíc na přenosu průběžného součtu (Pezik).
 *
 * Výstupem jsou půlvlny od hrany k hraně: sousední půlky stejné úrovně
 * se sečtou. Délky jsou celá čísla v jednotkách časové základny části
 * (timebase Hz, typicky T-stavy CPU). Převod na vzorky akumuluje čas od
 * začátku části jako mzcmt_fm, zaokrouhlovací chyba se nesčítá.
 *
 * Knihovna nezávisí na formátu TMZ: parametry plní volající (např.
 * tmz_part_manchester z TLV bloku 0x46, profily v tmz_profile).
 *
 * @par Changelog:
 * - 2026-10-18 (1.0.0): první verze.
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

#ifndef MZCMT_MANCHESTER_H
#define MZCMT_MANCHESTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "../cmt_stream/cmt_vstream.h"


/** @brief Výchozí časová základna: takt CPU MZ-800 v Hz (T-stavy). */
#define MZCMT_MANCH_DEFAULT_TIMEBASE 3546895u
/** @brief Položka kontextu platí pro bit 0 i 1. */
#define MZCMT_MANCH_BIT_ANY 0xFF
/** @brief Nejvíc položek kontextu: 15 kontextů x 2 bity. */
#define MZCMT_MANCH_MAX_CONTEXT 30
/** @brief Nejvíc bitů vzoru pilotu, sync a koncových bitů. */
#define MZCMT_MANCH_MAX_PATTERN_BITS 8
/**
 * @brief Nejvíc vzorů pilotu jedné části (profily 1 024 - 2 134).
 *
 * Meze MZCMT_MANCH_MAX_* chrání před souborem, který by počtem bitů nebo
 * délkou signálu zahltil paměť a čas; vycházejí z délky signálu (jedna
 * část i s pauzou kratší než hodina pásky), stejně jako u rodiny FM.
 */
#define MZCMT_MANCH_MAX_PILOT 1000000u
/** @brief Nejdelší půlka bitu (HALVES, CONTEXT) v sekundách časové základny části. */
#define MZCMT_MANCH_MAX_HALF_SECONDS 1u
/** @brief Nejdelší signál jedné části včetně pauzy v sekundách časové základny. */
#define MZCMT_MANCH_MAX_PART_SECONDS 3600u
/** @brief Nejvíc bitů jedné části (pilot, sync, flag, data, checksum, koncové bity). */
#define MZCMT_MANCH_MAX_BITS 16000000u
/** @brief Verze knihovny mzcmt_manchester. */
#define MZCMT_MANCH_VERSION "1.1.0"


    /**
     * @brief Kontext druhé půlky bitu (spec 7.2): určuje ho to, co za bitem následuje.
     */
    typedef enum en_MZCMT_MANCH_CTX {
        MZCMT_MANCH_CTX_BIT = 0,              /**< bity 0-6 každého bajtu (výchozí, HALVES) */
        MZCMT_MANCH_CTX_PILOT_IN,             /**< uvnitř vzoru pilotu */
        MZCMT_MANCH_CTX_PILOT_NEXT,           /**< poslední bit vzoru, za ním další vzor */
        MZCMT_MANCH_CTX_PILOT_END,            /**< poslední bit pilotu */
        MZCMT_MANCH_CTX_SYNC_IN,              /**< bit sync, za ním další bit sync */
        MZCMT_MANCH_CTX_SYNC_END,             /**< poslední bit sync */
        MZCMT_MANCH_CTX_FLAG_END,             /**< bit 7 flagu */
        MZCMT_MANCH_CTX_DATA_FIRST,           /**< bit 7 data[0], za ním data[1] */
        MZCMT_MANCH_CTX_DATA_BYTE,            /**< bit 7 data[k], k >= 1, za ním data[k+1] */
        MZCMT_MANCH_CTX_DATA_END,             /**< bit 7 posledního bajtu dat (délka >= 2) */
        MZCMT_MANCH_CTX_DATA_SINGLE_END,      /**< bit 7 jediného bajtu dat (délka 1) */
        MZCMT_MANCH_CTX_CK_MID,               /**< bit 7 nižšího bajtu checksumu */
        MZCMT_MANCH_CTX_CK_END,               /**< bit 7 vyššího bajtu checksumu */
        MZCMT_MANCH_CTX_END_IN,               /**< koncový bit, za ním další koncový bit */
        MZCMT_MANCH_CTX_END_LAST,             /**< poslední koncový bit */
        MZCMT_MANCH_CTX_COUNT                 /**< počet kontextů (není platný kontext) */
    } en_MZCMT_MANCH_CTX;


    /**
     * @brief Algoritmus checksumu (spec 7.2).
     */
    typedef enum en_MZCMT_MANCH_CHECKSUM {
        MZCMT_MANCH_CHECKSUM_NONE = 0,       /**< bez checksumu */
        MZCMT_MANCH_CHECKSUM_SUM16_LE = 1,   /**< 16bitový součet datových bajtů (bez flagu), nižší bajt první */
    } en_MZCMT_MANCH_CHECKSUM;


    /**
     * @brief Položka kontextové délky druhé půlky bitu.
     *
     * @invariant ctx < MZCMT_MANCH_CTX_COUNT, bit je 0, 1 nebo MZCMT_MANCH_BIT_ANY, half2 > 0
     */
    typedef struct st_MZCMT_MANCH_CONTEXT {
        uint8_t ctx;      /**< en_MZCMT_MANCH_CTX */
        uint8_t bit;      /**< hodnota bitu, ke kterému půlka patří (0, 1, MZCMT_MANCH_BIT_ANY) */
        uint32_t half2;   /**< druhá půlka (jednotky timebase) */
    } st_MZCMT_MANCH_CONTEXT;


    /**
     * @brief Parametry jedné části MANCHESTER (efektivní hodnoty, spec 5.3 a 7.2).
     *
     * Vzory (pilot, sync, koncové bity) mají bit 0 = první vyslaný bit.
     *
     * @invariant Platnost ověřuje mzcmt_manch_params_validate(): timebase > 0,
     *            všechny HALVES > 0, level a pause_level 0/1, pilot_bits 1-8
     *            při pilot_count > 0, sync_bits a end_bits nejvýš 8, známý
     *            checksum, checksum_explicit jen se SUM16_LE, context_count
     *            <= MZCMT_MANCH_MAX_CONTEXT, položky platné a bez opakované
     *            dvojice (ctx, bit), druhé půlky DATA_FIRST a DATA_BYTE
     *            s carry_adjust > 0; pilot_count <= MZCMT_MANCH_MAX_PILOT,
     *            každá půlka nejvýš MZCMT_MANCH_MAX_HALF_SECONDS x timebase,
     *            pause nejvýš MZCMT_MANCH_MAX_PART_SECONDS x timebase.
     */
    typedef struct st_MZCMT_MANCH_PARAMS {
        uint32_t timebase;          /**< časová základna v Hz */
        uint8_t level;              /**< 1 = bit 1 má první půlku HIGH, 0 = LOW */
        uint32_t pause;             /**< ticho za částí (jednotky timebase) */
        uint8_t pause_level;        /**< úroveň během pauzy: 0 = LOW, 1 = HIGH */
        uint32_t half1[2];          /**< první půlka bitu 0 a bitu 1 */
        uint32_t half2[2];          /**< druhá půlka bitu 0 a bitu 1 v kontextu BIT */
        uint32_t pilot_count;       /**< počet vzorů pilotu */
        uint8_t pilot_bits;         /**< délka vzoru v bitech (1-8) */
        uint8_t pilot_pattern;      /**< vzor pilotu (bit 0 první) */
        uint8_t sync_bits;          /**< počet bitů sync (0-8) */
        uint8_t sync_pattern;       /**< bity sync (bit 0 první) */
        uint8_t has_flag;           /**< 1 = vysílá se flag */
        uint8_t flag;               /**< hodnota flagu */
        uint8_t checksum;           /**< en_MZCMT_MANCH_CHECKSUM */
        uint8_t checksum_explicit;  /**< 1 = zapsat checksum_bytes místo vypočtených */
        uint8_t checksum_bytes[2];  /**< explicitní bajty checksumu v pořadí zápisu */
        uint8_t end_bits;           /**< počet koncových bitů (0-8) */
        uint8_t end_pattern;        /**< koncové bity (bit 0 první) */
        int8_t carry_adjust;        /**< úprava druhé půlky DATA_FIRST/DATA_BYTE při přenosu součtu */
        uint32_t context_count;     /**< počet platných položek context */
        st_MZCMT_MANCH_CONTEXT context[MZCMT_MANCH_MAX_CONTEXT];  /**< kontextové druhé půlky */
    } st_MZCMT_MANCH_PARAMS;


    /**
     * @brief Výstup generátoru: jedna půlvlna (od hrany k hraně).
     *
     * @param user Uživatelská data z mzcmt_manch_generate().
     * @param level Úroveň: 1 = HIGH, 0 = LOW.
     * @param duration Délka v jednotkách timebase (> 0).
     * @return EXIT_SUCCESS pokračuje, jiná hodnota generování ukončí s chybou.
     */
    typedef int ( *mzcmt_manch_half_cb ) ( void *user, int level, uint32_t duration );


    /**
     * @brief Nastaví výchozí hodnoty podle spec (TIMEBASE 3 546 895, LEVEL 1, ostatní 0).
     * @param p Parametry (nesmí být NULL).
     * @post HALVES jsou 0, parametry tedy ještě nejsou platné.
     */
    extern void mzcmt_manch_params_init ( st_MZCMT_MANCH_PARAMS *p );

    /**
     * @brief Ověří invarianty st_MZCMT_MANCH_PARAMS.
     * @param p Parametry (NULL = neplatné).
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE.
     */
    extern int mzcmt_manch_params_validate ( const st_MZCMT_MANCH_PARAMS *p );

    /**
     * @brief Ověří meze signálu části s daným počtem datových bajtů.
     *
     * Počet bitů (pilot x pilot_bits, sync, flag, size bajtů, checksum,
     * koncové bity) nesmí přesáhnout MZCMT_MANCH_MAX_BITS a horní odhad
     * délky (bity x nejdelší dvojice půlek + pause)
     * MZCMT_MANCH_MAX_PART_SECONDS x timebase. mzcmt_manch_generate() ji
     * volá sama; player ji volá předem, aby část mimo meze přeskočil.
     *
     * @param p Parametry (musí projít mzcmt_manch_params_validate()).
     * @param size Počet datových bajtů části.
     * @return EXIT_SUCCESS v mezích; EXIT_FAILURE mimo meze nebo s neplatnými parametry.
     */
    extern int mzcmt_manch_check_size ( const st_MZCMT_MANCH_PARAMS *p, uint32_t size );

    /**
     * @brief Meze části jako mzcmt_manch_check_size() a k tomu odhady pro mez celého bloku.
     *
     * @param p Parametry (musí projít mzcmt_manch_params_validate()).
     * @param size Počet datových bajtů části.
     * @param[out] bits Počet bitů části (smí být NULL).
     * @param[out] units Horní odhad délky části v jednotkách timebase
     *        (bity x nejdelší bit + pause; smí být NULL).
     * @return EXIT_SUCCESS v mezích části (výstupy vyplněné); EXIT_FAILURE
     *         mimo meze nebo s neplatnými parametry (výstupy beze změny).
     */
    extern int mzcmt_manch_part_bounds ( const st_MZCMT_MANCH_PARAMS *p, uint32_t size, uint64_t *bits, uint64_t *units );

    /**
     * @brief Spočítá checksum SUM16_LE: součet bajtů modulo 65536.
     * @param data Bajty (smí být NULL jen při size == 0).
     * @param size Počet bajtů.
     * @return Součet; zapisuje se nižší bajt první.
     */
    extern uint16_t mzcmt_manch_checksum ( const uint8_t *data, uint32_t size );

    /**
     * @brief Úroveň první půlvlny, kterou část vygeneruje.
     *
     * Podle prvního vysílaného bitu (pilot, sync, flag, data, checksum, konec)
     * a LEVEL; část bez bitů začíná pauzou (pause_level).
     *
     * @param p Platné parametry.
     * @param data Bajty části.
     * @param size Počet bajtů.
     * @return 1 = HIGH, 0 = LOW.
     */
    extern int mzcmt_manch_first_level ( const st_MZCMT_MANCH_PARAMS *p, const uint8_t *data, uint32_t size );

    /**
     * @brief Vygeneruje půlvlny jedné části MANCHESTER.
     *
     * Posloupnost (spec 7.2): pilot (vzor x počet), sync, flag, data LSB
     * první, checksum (nižší, vyšší bajt), koncové bity. První půlka podle
     * hodnoty bitu, druhá podle kontextu přechodu: položka (ctx, bit), pak
     * (ctx, MZCMT_MANCH_BIT_ANY), jinak half2[bit]. U DATA_FIRST a DATA_BYTE
     * se přičte carry_adjust, když nižší bajt součtu data[0..k] plus data[k+1]
     * přeteče přes FFh. Sousední půlky stejné úrovně se předají jako jedna
     * půlvlna. Je-li pause > 0, následuje samostatná půlvlna délky pause
     * s úrovní pause_level (stejnou úroveň sloučí až vstream).
     *
     * @param p Parametry (musí projít mzcmt_manch_params_validate()).
     * @param data Bajty části (smí být NULL jen při size == 0).
     * @param size Počet bajtů.
     * @param cb Výstup půlvln.
     * @param user Data pro cb.
     * @return EXIT_SUCCESS; EXIT_FAILURE při neplatných parametrech nebo
     *         chybě z cb (generování se zastaví).
     */
    extern int mzcmt_manch_generate ( const st_MZCMT_MANCH_PARAMS *p, const uint8_t *data, uint32_t size,
                                      mzcmt_manch_half_cb cb, void *user );

    /**
     * @brief Připojí část k existujícímu vstreamu.
     *
     * Hrany se umístí podle akumulovaného času od začátku této části:
     * vzorek hrany = floor((cum * rate + timebase / 2) / timebase) (spec 5.4,
     * stejně jako mzcmt_fm_append_vstream()). Pauza, která po zaokrouhlení
     * nedá ani jeden vzorek, se vynechá.
     *
     * @param vs Cílový vstream.
     * @param p Parametry části.
     * @param data Bajty části.
     * @param size Počet bajtů.
     * @return EXIT_SUCCESS; EXIT_FAILURE při neplatných parametrech, chybě
     *         vstreamu nebo půlvlně kratší než jeden vzorek (vypíše se na stderr).
     *
     * @warning Při chybě zůstanou ve vstreamu rozpracovaná data; volající ho má zahodit.
     */
    extern int mzcmt_manch_append_vstream ( st_CMT_VSTREAM *vs, const st_MZCMT_MANCH_PARAMS *p,
                                            const uint8_t *data, uint32_t size );

    /**
     * @brief Vytvoří vstream s jednou částí MANCHESTER.
     *
     * Počáteční hodnota vstreamu je úroveň první půlvlny (mzcmt_manch_first_level()).
     *
     * @param p Parametry části.
     * @param data Bajty části.
     * @param size Počet bajtů.
     * @param rate Vzorkovací frekvence v Hz.
     * @return Nový vstream, nebo NULL při chybě.
     *
     * @post Volající vlastní vstream a uvolní ho cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* mzcmt_manch_create_vstream ( const st_MZCMT_MANCH_PARAMS *p, const uint8_t *data,
                                                        uint32_t size, uint32_t rate );

    /**
     * @brief Vrátí verzi knihovny.
     * @return Statický řetězec MZCMT_MANCH_VERSION.
     */
    extern const char* mzcmt_manch_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZCMT_MANCHESTER_H */
