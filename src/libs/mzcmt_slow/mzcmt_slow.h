/**
 * @file   mzcmt_slow.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  SLOW koder pro kazetovy zaznam Sharp MZ (2 bity na pulz).
 *
 * Knihovna implementuje SLOW modulaci pouzivnou programem mzf2snd (mzftools).
 * SLOW koduje vzdy 2 bity jednim pulzem - kazdy ze 4 symbolu (00, 01, 10, 11)
 * ma vlastni delku LOW a HIGH casti. Dekoder na Z80 meri sirku pulzu a pomoci
 * lookup tabulky na adrese $0300 urcuje symbol. Diky 2 bitum na pulz je
 * SLOW kompaktnejsi nez standardni FM kodovani (1 bit na pulz).
 *
 * @par Struktura SLOW signalu:
 * @verbatim
 *   [GAP]       200 cyklu (8 LOW + 8 HIGH vzorku na cyklus)
 *   [POLARITY]  4 LOW + 2 HIGH vzorku (detekce polarity)
 *   [SYNC]      2 LOW + 2 HIGH vzorku (synchronizacni kod)
 *   [DATA]      N bajtu (4 SLOW pulzy na bajt, MSB first, po 2 bitech)
 *   [FADEOUT]   8 iteraci s rostouci sirkou pulzu (32-46 vzorku)
 * @endverbatim
 *
 * @par Kodovani bajtu:
 * Kazdy bajt se rozlozi na 4 dvoubitove symboly (MSB first):
 * @verbatim
 *   bajt 0xA7 = 10_10_01_11
 *   symbol[0] = (bajt >> 6) & 0x03 = 2  (bity 7-6)
 *   symbol[1] = (bajt >> 4) & 0x03 = 2  (bity 5-4)
 *   symbol[2] = (bajt >> 2) & 0x03 = 1  (bity 3-2)
 *   symbol[3] = (bajt >> 0) & 0x03 = 3  (bity 1-0)
 * @endverbatim
 * Pro kazdy symbol se z tabulky vybere pocet LOW a HIGH vzorku.
 *
 * @par Pouziti v kontextu TMZ:
 * SLOW koduje pouze datove telo (body). MZF hlavicka se typicky odesila
 * standardnim Sharp formatem (NORMAL FM) a prechod na SLOW zajistuje
 * loader (TMZ blok 0x44). Checksum je XOR vsech bajtu, predpocitan
 * a ulozen primo v kodu loaderu.
 *
 * @par Referencni implementace:
 * - mzftools: src/coder.c (slow_encode_data_byte)
 * - mzftools: src/formats.c (fsk_body - spolecny frame)
 * - mzftools: src/slow.asm (Z80 dekoder)
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


#ifndef MZCMT_SLOW_H
#define MZCMT_SLOW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/mzf/mzf.h"


    /**
     * @brief Rychlostni urovne SLOW koderu.
     *
     * Rychlost urcuje delky pulzu pro kazdy ze 4 symbolu.
     * Nizsi rychlost = delsi pulzy = pomalejsi prenos = vyssi spolehlivost.
     * Vyssi rychlost = kratsi pulzy = rychlejsi prenos.
     *
     * Mapovani na referencni tabulky:
     * Speed 0 je nejpomalejsi (tabulkovy index 4), speed 4 nejrychlejsi
     * (tabulkovy index 0). Referencni implementace pouziva inverzi:
     * table_index = 4 - speed.
     *
     * Invariant: hodnota musi byt < MZCMT_SLOW_SPEED_COUNT.
     */
    typedef enum en_MZCMT_SLOW_SPEED {
        MZCMT_SLOW_SPEED_0 = 0, /**< Nejpomalejsi: nejdelsi pulzy. */
        MZCMT_SLOW_SPEED_1,     /**< Pomaly. */
        MZCMT_SLOW_SPEED_2,     /**< Stredni. */
        MZCMT_SLOW_SPEED_3,     /**< Rychly. */
        MZCMT_SLOW_SPEED_4,     /**< Nejrychlejsi: nejkratsi pulzy. */
        MZCMT_SLOW_SPEED_COUNT  /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_SLOW_SPEED;


    /**
     * @brief Referencni vzorkovaci frekvence pro SLOW pulzni tabulky (Hz).
     *
     * Pulzni tabulky definuji pocty vzorku pri teto frekvenci.
     * Pro jine frekvence se pocty proporcionalne skaluji,
     * cimz se zachova absolutni casovani signalu.
     */
#define MZCMT_SLOW_REF_RATE 44100

    /** @brief Pocet cyklu v GAP (leader) sekvenci. */
#define MZCMT_SLOW_GAP_CYCLES 200
    /** @brief Pocet LOW vzorku v jednom GAP cyklu (pri ref. frekvenci). */
#define MZCMT_SLOW_GAP_LOW_REF 8
    /** @brief Pocet HIGH vzorku v jednom GAP cyklu (pri ref. frekvenci). */
#define MZCMT_SLOW_GAP_HIGH_REF 8

    /** @brief Pocet LOW vzorku v polaritnim pulzu (pri ref. frekvenci). */
#define MZCMT_SLOW_POLARITY_LOW_REF 4
    /** @brief Pocet HIGH vzorku v polaritnim pulzu (pri ref. frekvenci). */
#define MZCMT_SLOW_POLARITY_HIGH_REF 2

    /** @brief Pocet LOW vzorku v sync kodu (pri ref. frekvenci). */
#define MZCMT_SLOW_SYNC_LOW_REF 2
    /** @brief Pocet HIGH vzorku v sync kodu (pri ref. frekvenci). */
#define MZCMT_SLOW_SYNC_HIGH_REF 2

    /** @brief Pocatecni sirka fade-out pulzu (pri ref. frekvenci). */
#define MZCMT_SLOW_FADEOUT_START_REF 32
    /** @brief Koncova sirka fade-out pulzu (exkluzivni, pri ref. frekvenci). */
#define MZCMT_SLOW_FADEOUT_END_REF 48
    /** @brief Krok zvetsovani fade-out pulzu (pri ref. frekvenci). */
#define MZCMT_SLOW_FADEOUT_STEP_REF 2

    /** @brief Pocet moznych symbolu (2 bity = 4 kombinace: 00, 01, 10, 11). */
#define MZCMT_SLOW_SYMBOL_COUNT 4


    /**
     * @brief Chybove kody SLOW koderu.
     */
    typedef enum en_MZCMT_SLOW_ERROR {
        MZCMT_SLOW_OK = 0,              /**< Bez chyby. */
        MZCMT_SLOW_ERROR_NULL_INPUT,     /**< Vstupni data jsou NULL. */
        MZCMT_SLOW_ERROR_ZERO_SIZE,      /**< Velikost dat je 0. */
        MZCMT_SLOW_ERROR_INVALID_SPEED,  /**< Neplatna rychlostni uroven. */
        MZCMT_SLOW_ERROR_INVALID_RATE,   /**< Neplatna vzorkovaci frekvence (0). */
        MZCMT_SLOW_ERROR_VSTREAM,        /**< Chyba pri vytvareni nebo zapisu do vstreamu. */
    } en_MZCMT_SLOW_ERROR;


    /**
     * @brief Vytvori CMT vstream z datoveho bloku SLOW kodovanim.
     *
     * Generuje kompletni SLOW signal: GAP (leader) -> polaritni pulz ->
     * sync kod -> data (4 SLOW pulzy na bajt, po 2 bitech, MSB first) ->
     * fade-out.
     *
     * Kazdy datovy bajt se rozlozi na 4 dvoubitove symboly. Pro kazdy
     * symbol se z tabulky vybere pocet LOW a HIGH vzorku. Tabulky jsou
     * indexovane rychlostni urovni (s inverzi: table_index = 4 - speed)
     * a hodnotou symbolu (0-3).
     *
     * Signal zacina v LOW stavu. Pulzni sirky se skaluji proporcionalne
     * z referencni frekvence 44100 Hz na cilovou frekvenci @p rate.
     *
     * @param data     Ukazatel na datovy blok k zakodovani.
     * @param data_size Velikost datoveho bloku v bajtech (musi byt > 0).
     * @param speed    Rychlostni uroven (0-4).
     * @param rate     Vzorkovaci frekvence vystupniho vstreamu (Hz, musi byt > 0).
     * @return Ukazatel na novy vstream, nebo NULL pri chybe.
     *
     * @pre @p data != NULL
     * @pre @p data_size > 0
     * @pre @p speed < MZCMT_SLOW_SPEED_COUNT
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream a musi
     *       jej uvolnit volanim cmt_vstream_destroy().
     * @post Navratova hodnota == NULL => zadna pamet nebyla alokovana.
     *
     * @note SLOW kodovani nepridava stop bity za datove bajty.
     *       Checksum neni soucasti signalu.
     */
    extern st_CMT_VSTREAM* mzcmt_slow_create_vstream (
        const uint8_t *data,
        uint32_t data_size,
        en_MZCMT_SLOW_SPEED speed,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream z datoveho bloku SLOW kodovanim.
     *
     * Obaluje mzcmt_slow_create_vstream() do polymorfniho st_CMT_STREAM.
     * Pro typ CMT_STREAM_TYPE_BITSTREAM interno vytvori vstream a konvertuje.
     *
     * @param data      Ukazatel na datovy blok k zakodovani.
     * @param data_size Velikost datoveho bloku v bajtech (musi byt > 0).
     * @param speed     Rychlostni uroven (0-4).
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz, musi byt > 0).
     * @return Ukazatel na novy stream, nebo NULL pri chybe.
     *
     * @pre @p data != NULL
     * @pre @p data_size > 0
     * @pre @p speed < MZCMT_SLOW_SPEED_COUNT
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni stream a musi
     *       jej uvolnit volanim cmt_stream_destroy().
     */
    extern st_CMT_STREAM* mzcmt_slow_create_stream (
        const uint8_t *data,
        uint32_t data_size,
        en_MZCMT_SLOW_SPEED speed,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /**
     * @brief Spocita XOR checksum datoveho bloku (pro SLOW loader).
     *
     * SLOW dekoder na Z80 pouziva registr IXL jako akumulator pro XOR
     * checksum. Po prijeti vsech bajtu porovnava vysledek s hodnotou
     * ulozenou v kodu loaderu (adresa checksumx v slow.asm).
     *
     * @param data Ukazatel na datovy blok.
     * @param size Velikost datoveho bloku v bajtech.
     * @return XOR vsech bajtu v bloku. Vraci 0 pro NULL nebo size==0.
     */
    extern uint8_t mzcmt_slow_compute_checksum ( const uint8_t *data, uint32_t size );


    /**
     * @brief Pulzni sady pro NORMAL FM kodovani v preloader casti.
     *
     * Urcuje vychozi casovani pulzu pro NORMAL FM kodovani preloaderu
     * a loaderu. Mapuje se na stejne hodnoty jako MZCMT_TURBO_PULSESET.
     */
    typedef enum en_MZCMT_SLOW_PULSESET {
        MZCMT_SLOW_PULSESET_700 = 0, /**< MZ-700, MZ-80K, MZ-80A. */
        MZCMT_SLOW_PULSESET_800,     /**< MZ-800, MZ-1500. */
        MZCMT_SLOW_PULSESET_80B,     /**< MZ-80B. */
        MZCMT_SLOW_PULSESET_COUNT    /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_SLOW_PULSESET;


    /**
     * @brief Konfigurace pro generovani kompletniho paskoveho signalu s loaderem.
     *
     * Urcuje vsechny parametry potrebne pro vygenerovani funkcni pasky:
     * preloader v NORMAL FM + SLOW loader body v NORMAL FM + uzivatelska data v SLOW.
     *
     * @par Invarianty:
     * - pulseset musi byt < MZCMT_SLOW_PULSESET_COUNT
     * - speed musi byt < MZCMT_SLOW_SPEED_COUNT
     * - loader_speed je rychlost ROM pro nacteni SLOW loaderu (0-6, 0 = standard)
     */
    typedef struct st_MZCMT_SLOW_TAPE_CONFIG {
        en_MZCMT_SLOW_PULSESET pulseset; /**< pulzni sada pro NORMAL FM casti */
        en_MZCMT_SLOW_SPEED speed;       /**< rychlost SLOW dat (0-4) */
        uint8_t loader_speed;            /**< rychlost ROM pro nacteni SLOW loaderu (0-6) */
    } st_MZCMT_SLOW_TAPE_CONFIG;


    /**
     * @brief Vytvori CMT vstream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Generuje trojdilny signal na pasce:
     * 1. PRELOADER v NORMAL FM pri 1:1 rychlosti (ROM ho nacte standardne)
     * 2. SLOW LOADER body v NORMAL FM pri loader_speed (preloader ho nacte)
     * 3. Uzivatelska data v SLOW formatu (SLOW loader je nacte)
     *
     * Preloader je loader_turbo (128B, fsize=0, Z80 kod v komentari hlavicky).
     * Po nacteni ROMem modifikuje readpoint a vola ROM cteci rutinu pro
     * nacteni SLOW loaderu. SLOW loader pak cte data vlastni SLOW modulaci.
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
     * @pre @p config->pulseset < MZCMT_SLOW_PULSESET_COUNT
     * @pre @p config->speed < MZCMT_SLOW_SPEED_COUNT
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream.
     */
    extern st_CMT_VSTREAM* mzcmt_slow_create_tape_vstream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_SLOW_TAPE_CONFIG *config,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Obaluje mzcmt_slow_create_tape_vstream() do polymorfniho st_CMT_STREAM.
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
    extern st_CMT_STREAM* mzcmt_slow_create_tape_stream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_SLOW_TAPE_CONFIG *config,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /** @brief Uzivatelsky alokator - umoznuje nahradit vychozi malloc/calloc/free. */
    typedef struct st_MZCMT_SLOW_ALLOCATOR {
        void* (*alloc)(size_t size);   /**< alokace pameti (jako malloc) */
        void* (*alloc0)(size_t size);  /**< alokace s nulovanim (jako calloc) */
        void  (*free)(void *ptr);      /**< uvolneni pameti */
    } st_MZCMT_SLOW_ALLOCATOR;

    /**
     * @brief Nastavi vlastni alokator pro knihovnu mzcmt_slow.
     * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset na vychozi.
     */
    extern void mzcmt_slow_set_allocator ( const st_MZCMT_SLOW_ALLOCATOR *allocator );


    /** @brief Typ callback funkce pro hlaseni chyb. */
    typedef void (*mzcmt_slow_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastavi vlastni callback pro chybova hlaseni.
     * @param cb Callback funkce, nebo NULL pro reset na vychozi (fprintf stderr).
     */
    extern void mzcmt_slow_set_error_callback ( mzcmt_slow_error_cb cb );


    /** @brief Verze knihovny mzcmt_slow. */
#define MZCMT_SLOW_VERSION "1.0.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzcmt_slow.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzcmt_slow_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZCMT_SLOW_H */
