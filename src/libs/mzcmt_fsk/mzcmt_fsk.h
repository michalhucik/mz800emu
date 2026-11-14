/**
 * @file   mzcmt_fsk.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  FSK (Frequency Shift Keying) koder pro kazetovy zaznam Sharp MZ.
 *
 * Knihovna implementuje FSK modulaci pouzivnou programem mzf2snd (mzftools).
 * FSK koduje bity zmenou frekvence signalu: bit "1" = delsi pulz (nizsi
 * frekvence), bit "0" = kratsi pulz (vyssi frekvence). Dekoder na Z80
 * rozlisuje pulzy pevnym prahem z tabulky podle urovne rychlosti.
 *
 * @par Struktura FSK signalu:
 * @verbatim
 *   [GAP]       200 cyklu (8 LOW + 8 HIGH vzorku na cyklus)
 *   [POLARITY]  4 LOW + 2 HIGH vzorku (detekce polarity)
 *   [SYNC]      2 LOW + 2 HIGH vzorku (synchronizacni kod)
 *   [DATA]      N bajtu (8 FSK pulzu na bajt, MSB first, bez stop bitu)
 *   [FADEOUT]   8 iteraci s rostouci sirkou pulzu (32-46 vzorku)
 * @endverbatim
 *
 * @par Pouziti v kontextu TMZ:
 * FSK koduje pouze datove telo (body). MZF hlavicka se typicky odesila
 * standardnim Sharp formatem (NORMAL FM) a prechod na FSK zajistuje
 * loader (TMZ blok 0x44). Checksum neni soucasti FSK signalu - je
 * predpocitan a ulozen primo v kodu loaderu (self-modifying Z80 kod).
 *
 * @par Referencni implementace:
 * - mzftools: src/coder.c (fsk_pulse, fsk_encode_data_byte)
 * - mzftools: src/formats.c (fsk_body)
 * - mzftools: src/fsk.asm (Z80 dekoder)
 *
 * @par Licence:
 * Odvozeno z projektu MZFTools v0.2.2 (mz-fuzzy@users.sf.net),
 * sireneho pod GNU General Public License v3 (GPLv3).
 *
 * Copyright (C) MZFTools authors (mz-fuzzy@users.sf.net)
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


#ifndef MZCMT_FSK_H
#define MZCMT_FSK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/mzf/mzf.h"


    /**
     * @brief Rychlostni urovne FSK koderu.
     *
     * Rychlost urcuje pomer delek long a short pulzu. Nizsi rychlost =
     * delsi pulzy = pomalejsi prenos = vyssi spolehlivost pri cteni.
     * Vyssi rychlost = kratsi pulzy = rychlejsi prenos.
     *
     * Hodnoty jsou pocty referencnich vzorku (LOW+HIGH) pri 44100 Hz:
     * - Speed 0: long=2+6=8, short=2+2=4 (pomer 2:1)
     * - Speed 6: long=1+2=3, short=1+1=2 (pomer 3:2)
     *
     * Invariant: hodnota musi byt < MZCMT_FSK_SPEED_COUNT.
     */
    typedef enum en_MZCMT_FSK_SPEED {
        MZCMT_FSK_SPEED_0 = 0, /**< Nejpomalejsi: long 2+6, short 2+2 vzorku. */
        MZCMT_FSK_SPEED_1,     /**< long 2+5, short 2+2 vzorku. */
        MZCMT_FSK_SPEED_2,     /**< long 2+4, short 2+2 vzorku. */
        MZCMT_FSK_SPEED_3,     /**< long 2+4, short 2+1 vzorku. */
        MZCMT_FSK_SPEED_4,     /**< long 2+3, short 1+1 vzorku. */
        MZCMT_FSK_SPEED_5,     /**< long 2+2, short 1+1 vzorku. */
        MZCMT_FSK_SPEED_6,     /**< Nejrychlejsi: long 1+2, short 1+1 vzorku. */
        MZCMT_FSK_SPEED_COUNT  /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_FSK_SPEED;


    /**
     * @brief Referencni vzorkovaci frekvence pro FSK pulzni tabulky (Hz).
     *
     * Pulzni tabulky definuji pocty vzorku pri teto frekvenci.
     * Pro jine frekvence se pocty proporcionalne skaluji,
     * cimz se zachova absolutni casovani signalu.
     */
#define MZCMT_FSK_REF_RATE 44100

    /** @brief Pocet cyklu v GAP (leader) sekvenci. */
#define MZCMT_FSK_GAP_CYCLES 200
    /** @brief Pocet LOW vzorku v jednom GAP cyklu (pri ref. frekvenci). */
#define MZCMT_FSK_GAP_LOW_REF 8
    /** @brief Pocet HIGH vzorku v jednom GAP cyklu (pri ref. frekvenci). */
#define MZCMT_FSK_GAP_HIGH_REF 8

    /** @brief Pocet LOW vzorku v polaritnim pulzu (pri ref. frekvenci). */
#define MZCMT_FSK_POLARITY_LOW_REF 4
    /** @brief Pocet HIGH vzorku v polaritnim pulzu (pri ref. frekvenci). */
#define MZCMT_FSK_POLARITY_HIGH_REF 2

    /** @brief Pocet LOW vzorku v sync kodu (pri ref. frekvenci). */
#define MZCMT_FSK_SYNC_LOW_REF 2
    /** @brief Pocet HIGH vzorku v sync kodu (pri ref. frekvenci). */
#define MZCMT_FSK_SYNC_HIGH_REF 2

    /** @brief Pocatecni sirka fade-out pulzu (pri ref. frekvenci). */
#define MZCMT_FSK_FADEOUT_START_REF 32
    /** @brief Koncova sirka fade-out pulzu (exkluzivni, pri ref. frekvenci). */
#define MZCMT_FSK_FADEOUT_END_REF 48
    /** @brief Krok zvetsovani fade-out pulzu (pri ref. frekvenci). */
#define MZCMT_FSK_FADEOUT_STEP_REF 2


    /**
     * @brief Chybove kody FSK koderu.
     */
    typedef enum en_MZCMT_FSK_ERROR {
        MZCMT_FSK_OK = 0,              /**< Bez chyby. */
        MZCMT_FSK_ERROR_NULL_INPUT,     /**< Vstupni data jsou NULL. */
        MZCMT_FSK_ERROR_ZERO_SIZE,      /**< Velikost dat je 0. */
        MZCMT_FSK_ERROR_INVALID_SPEED,  /**< Neplatna rychlostni uroven. */
        MZCMT_FSK_ERROR_INVALID_RATE,   /**< Neplatna vzorkovaci frekvence (0). */
        MZCMT_FSK_ERROR_VSTREAM,        /**< Chyba pri vytvareni nebo zapisu do vstreamu. */
    } en_MZCMT_FSK_ERROR;


    /**
     * @brief Vytvori CMT vstream z datoveho bloku FSK kodovanim.
     *
     * Generuje kompletni FSK signal: GAP (leader) -> polaritni pulz ->
     * sync kod -> data (8 FSK pulzu na bajt, MSB first) -> fade-out.
     *
     * Signal zacina v LOW stavu. Pulzni sirky se skaluji proporcionalne
     * z referencni frekvence 44100 Hz na cilovou frekvenci @p rate.
     *
     * @param data     Ukazatel na datovy blok k zakodovani.
     * @param data_size Velikost datoveho bloku v bajtech (musi byt > 0).
     * @param speed    Rychlostni uroven (0-6).
     * @param rate     Vzorkovaci frekvence vystupniho vstreamu (Hz, musi byt > 0).
     * @return Ukazatel na novy vstream, nebo NULL pri chybe.
     *
     * @pre @p data != NULL
     * @pre @p data_size > 0
     * @pre @p speed < MZCMT_FSK_SPEED_COUNT
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream a musi
     *       jej uvolnit volanim cmt_vstream_destroy().
     * @post Navratova hodnota == NULL => zadna pamet nebyla alokovana.
     *
     * @note FSK kodovani nepridava stop bity za datove bajty (na rozdil
     *       od NORMAL FM). Checksum neni soucasti signalu.
     * @note Pri nizsich vzorkovacich frekvencich (v emulatoru uz 22050 Hz)
     *       a vyssich rychlostech nemusi byt dostatecne rozliseni pro
     *       spravne casovani pulzu (player zmerene necitelne kombinace odmitne).
     */
    extern st_CMT_VSTREAM* mzcmt_fsk_create_vstream (
        const uint8_t *data,
        uint32_t data_size,
        en_MZCMT_FSK_SPEED speed,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream z datoveho bloku FSK kodovanim.
     *
     * Obaluje mzcmt_fsk_create_vstream() do polymorfniho st_CMT_STREAM.
     * Pro typ CMT_STREAM_TYPE_BITSTREAM interno vytvori vstream a konvertuje.
     *
     * @param data      Ukazatel na datovy blok k zakodovani.
     * @param data_size Velikost datoveho bloku v bajtech (musi byt > 0).
     * @param speed     Rychlostni uroven (0-6).
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz, musi byt > 0).
     * @return Ukazatel na novy stream, nebo NULL pri chybe.
     *
     * @pre @p data != NULL
     * @pre @p data_size > 0
     * @pre @p speed < MZCMT_FSK_SPEED_COUNT
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni stream a musi
     *       jej uvolnit volanim cmt_stream_destroy().
     */
    extern st_CMT_STREAM* mzcmt_fsk_create_stream (
        const uint8_t *data,
        uint32_t data_size,
        en_MZCMT_FSK_SPEED speed,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /**
     * @brief Spocita XOR checksum datoveho bloku (pro FSK loader).
     *
     * FSK dekoder na Z80 pocita prubeznz XOR vsech prijatych bajtu.
     * Vysledek porovnava s hodnotou ulozenzou v kodu loaderu.
     * Tato funkce spocita stejny checksum pro ucely pripravy loaderu.
     *
     * @param data Ukazatel na datovy blok.
     * @param size Velikost datoveho bloku v bajtech.
     * @return XOR vsech bajtu v bloku. Vraci 0 pro NULL nebo size==0.
     */
    extern uint8_t mzcmt_fsk_compute_checksum ( const uint8_t *data, uint32_t size );


    /**
     * @brief Pulzni sady pro NORMAL FM kodovani v preloader casti.
     *
     * Urcuje vychozi casovani pulzu pro NORMAL FM kodovani preloaderu
     * a loaderu. Mapuje se na stejne hodnoty jako MZCMT_TURBO_PULSESET.
     */
    typedef enum en_MZCMT_FSK_PULSESET {
        MZCMT_FSK_PULSESET_700 = 0, /**< MZ-700, MZ-80K, MZ-80A. */
        MZCMT_FSK_PULSESET_800,     /**< MZ-800, MZ-1500. */
        MZCMT_FSK_PULSESET_80B,     /**< MZ-80B. */
        MZCMT_FSK_PULSESET_COUNT    /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_FSK_PULSESET;


    /**
     * @brief Konfigurace pro generovani kompletniho paskoveho signalu s loaderem.
     *
     * Urcuje vsechny parametry potrebne pro vygenerovani funkcni pasky:
     * preloader v NORMAL FM + FSK loader body v NORMAL FM + uzivatelska data v FSK.
     *
     * @par Invarianty:
     * - pulseset musi byt < MZCMT_FSK_PULSESET_COUNT
     * - speed musi byt < MZCMT_FSK_SPEED_COUNT
     * - loader_speed je rychlost ROM pro nacteni FSK loaderu (0-6, 0 = standard)
     */
    typedef struct st_MZCMT_FSK_TAPE_CONFIG {
        en_MZCMT_FSK_PULSESET pulseset; /**< pulzni sada pro NORMAL FM casti */
        en_MZCMT_FSK_SPEED speed;       /**< rychlost FSK dat (0-6) */
        uint8_t loader_speed;            /**< rychlost ROM pro nacteni FSK loaderu (0-6) */
    } st_MZCMT_FSK_TAPE_CONFIG;


    /**
     * @brief Vytvori CMT vstream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Generuje trojdilny signal na pasce:
     * 1. PRELOADER v NORMAL FM pri 1:1 rychlosti (ROM ho nacte standardne)
     * 2. FSK LOADER body v NORMAL FM pri loader_speed (preloader ho nacte)
     * 3. Uzivatelska data v FSK formatu (FSK loader je nacte)
     *
     * Preloader je loader_turbo (128B, fsize=0, Z80 kod v komentari hlavicky).
     * Po nacteni ROMem modifikuje readpoint a vola ROM cteci rutinu pro
     * nacteni FSK loaderu. FSK loader pak cte data vlastni FSK modulaci.
     *
     * Oba loadery jsou automaticky patchovany s parametry uzivatelskeho MZF
     * (velikost, startovni adresa, exec adresa, delay, XOR checksum).
     *
     * @param original  Originalni MZF hlavicka (zdrojovy program).
     * @param body      Ukazatel na datove telo k zakodovani.
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace (pulseset, speed, loader_speed).
     * @param rate      Vzorkovaci frekvence (Hz).
     * @return Novy vstream s kompletnim signalem, nebo NULL pri chybe.
     *
     * @pre @p original != NULL
     * @pre @p body != NULL pokud @p body_size > 0
     * @pre @p config != NULL
     * @pre @p config->pulseset < MZCMT_FSK_PULSESET_COUNT
     * @pre @p config->speed < MZCMT_FSK_SPEED_COUNT
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream.
     */
    extern st_CMT_VSTREAM* mzcmt_fsk_create_tape_vstream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_FSK_TAPE_CONFIG *config,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Obaluje mzcmt_fsk_create_tape_vstream() do polymorfniho st_CMT_STREAM.
     *
     * @param original  Originalni MZF hlavicka (zdrojovy program).
     * @param body      Ukazatel na datove telo k zakodovani.
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace (pulseset, speed, loader_speed).
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz).
     * @return Novy stream, nebo NULL pri chybe.
     *
     * @note Čitelnost zavaděčem závisí na vzorkovací frekvenci: při 44100 Hz
     *       prošly v emulátoru všechny úrovně, jinde jen některé
     *       (měření v emulátoru). Player změřené nečitelné
     *       kombinace odmítne; tato funkce frekvenci nekontroluje.
     */
    extern st_CMT_STREAM* mzcmt_fsk_create_tape_stream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_FSK_TAPE_CONFIG *config,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /** @brief Uzivatelsky alokator - umoznuje nahradit vychozi malloc/calloc/free. */
    typedef struct st_MZCMT_FSK_ALLOCATOR {
        void* (*alloc)(size_t size);   /**< alokace pameti (jako malloc) */
        void* (*alloc0)(size_t size);  /**< alokace s nulovanim (jako calloc) */
        void  (*free)(void *ptr);      /**< uvolneni pameti */
    } st_MZCMT_FSK_ALLOCATOR;

    /**
     * @brief Nastavi vlastni alokator pro knihovnu mzcmt_fsk.
     * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset na vychozi.
     */
    extern void mzcmt_fsk_set_allocator ( const st_MZCMT_FSK_ALLOCATOR *allocator );


    /** @brief Typ callback funkce pro hlaseni chyb. */
    typedef void (*mzcmt_fsk_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastavi vlastni callback pro chybova hlaseni.
     * @param cb Callback funkce, nebo NULL pro reset na vychozi (fprintf stderr).
     */
    extern void mzcmt_fsk_set_error_callback ( mzcmt_fsk_error_cb cb );


    /** @brief Verze knihovny mzcmt_fsk. */
#define MZCMT_FSK_VERSION "1.0.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzcmt_fsk.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzcmt_fsk_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZCMT_FSK_H */
