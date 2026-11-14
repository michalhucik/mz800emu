/**
 * @file   mzcmt_direct.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  DIRECT koder pro kazetovy zaznam Sharp MZ (primy bitovy zapis).
 *
 * Knihovna implementuje DIRECT modulaci pouzivnou programem mzf2snd (mzftools).
 * DIRECT je nejrychlejsi metoda prenosu dat na kazetach Sharp MZ - kazdy bit
 * se zapise primo jako jeden audio vzorek (HIGH/LOW) bez frekvencni modulace.
 * Po kazdem druhem datovem bitu nasleduje synchronizacni bit (opacna hodnota),
 * ktery dekoderu umoznuje resynchronizaci timingu.
 *
 * @par Struktura DIRECT signalu:
 * @verbatim
 *   [GAP]       200 cyklu (8 LOW + 8 HIGH vzorku na cyklus)
 *   [POLARITY]  4 LOW + 2 HIGH vzorku (detekce polarity)
 *   [SYNC]      2 LOW + 2 HIGH vzorku (synchronizacni kod)
 *   [DATA]      N bajtu (12 vzorku na bajt: 8 dat + 4 synchro, MSB first)
 *   [FADEOUT]   8 iteraci s rostouci sirkou pulzu (32-46 vzorku)
 * @endverbatim
 *
 * @par Kodovani bajtu:
 * Kazdy bajt se koduje jako sekvence 12 vzorku (MSB first):
 * @verbatim
 *   D7 D6 ~D6 D5 D4 ~D4 D3 D2 ~D2 D1 D0 ~D0
 * @endverbatim
 * kde Dn = n-ty datovy bit (1=HIGH, 0=LOW), ~Dn = opak datoveho bitu
 * (synchronizacni bit). Synchro bity se pridavaji po kazdem sudem bitu
 * (indexy 1, 3, 5, 7 v poradi od MSB).
 *
 * @par Pouziti v kontextu TMZ:
 * DIRECT koduje pouze datove telo (body). MZF hlavicka se typicky odesila
 * standardnim Sharp formatem (NORMAL FM) a prechod na DIRECT zajistuje
 * loader (TMZ blok 0x44). Checksum je XOR vsech bajtu, predpocitan
 * a ulozen primo v kodu loaderu (self-modifying Z80 kod).
 *
 * @par Z80 dekoder:
 * Dekoder cte signál primo z PPI portu ($E002, bit 5). Timing je
 * softwarovy (presne pocty T-states v instrukcni smycce). Po kazdem
 * druhem bitu dekoder ceka na prechod synchro pulzu, cimz se
 * resynchronizuje casovani. Bajt se sklada z 8 bitu pomoci AND, RLCA,
 * RRCA a OR instrukci do registru C.
 *
 * @par Referencni implementace:
 * - mzftools: src/coder.c (direct_encode_data_byte)
 * - mzftools: src/formats.c (fsk_body - spolecny frame)
 * - mzftools: src/direct.asm (Z80 dekoder)
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


#ifndef MZCMT_DIRECT_H
#define MZCMT_DIRECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/mzf/mzf.h"


    /**
     * @brief Referencni vzorkovaci frekvence pro DIRECT pulzni tabulky (Hz).
     *
     * Frame konstanty (GAP, polarity, sync, fadeout) definuji pocty vzorku
     * pri teto frekvenci. Pro jine frekvence se pocty proporcionalne skaluji,
     * cimz se zachova absolutni casovani signalu.
     *
     * Datove vzorky se neskaluji - kazdy datovy/synchro element je vzdy
     * 1 audio vzorek nezavisle na frekvenci. Rychlost prenosu dat je tak
     * primo umerna vzorkovaci frekvenci.
     */
#define MZCMT_DIRECT_REF_RATE 44100

    /** @brief Pocet cyklu v GAP (leader) sekvenci. */
#define MZCMT_DIRECT_GAP_CYCLES 200
    /** @brief Pocet LOW vzorku v jednom GAP cyklu (pri ref. frekvenci). */
#define MZCMT_DIRECT_GAP_LOW_REF 8
    /** @brief Pocet HIGH vzorku v jednom GAP cyklu (pri ref. frekvenci). */
#define MZCMT_DIRECT_GAP_HIGH_REF 8

    /** @brief Pocet LOW vzorku v polaritnim pulzu (pri ref. frekvenci). */
#define MZCMT_DIRECT_POLARITY_LOW_REF 4
    /** @brief Pocet HIGH vzorku v polaritnim pulzu (pri ref. frekvenci). */
#define MZCMT_DIRECT_POLARITY_HIGH_REF 2

    /** @brief Pocet LOW vzorku v sync kodu (pri ref. frekvenci). */
#define MZCMT_DIRECT_SYNC_LOW_REF 2
    /** @brief Pocet HIGH vzorku v sync kodu (pri ref. frekvenci). */
#define MZCMT_DIRECT_SYNC_HIGH_REF 2

    /** @brief Pocatecni sirka fade-out pulzu (pri ref. frekvenci). */
#define MZCMT_DIRECT_FADEOUT_START_REF 32
    /** @brief Koncova sirka fade-out pulzu (exkluzivni, pri ref. frekvenci). */
#define MZCMT_DIRECT_FADEOUT_END_REF 48
    /** @brief Krok zvetsovani fade-out pulzu (pri ref. frekvenci). */
#define MZCMT_DIRECT_FADEOUT_STEP_REF 2

    /** @brief Pocet datovych bitu v jednom bajtu. */
#define MZCMT_DIRECT_BITS_PER_BYTE 8
    /** @brief Pocet synchro bitu v jednom bajtu (po kazdem 2. datovem bitu). */
#define MZCMT_DIRECT_SYNC_BITS_PER_BYTE 4
    /** @brief Celkovy pocet vzorku na bajt (8 dat + 4 synchro). */
#define MZCMT_DIRECT_SAMPLES_PER_BYTE 12


    /**
     * @brief Chybove kody DIRECT koderu.
     */
    typedef enum en_MZCMT_DIRECT_ERROR {
        MZCMT_DIRECT_OK = 0,              /**< Bez chyby. */
        MZCMT_DIRECT_ERROR_NULL_INPUT,     /**< Vstupni data jsou NULL. */
        MZCMT_DIRECT_ERROR_ZERO_SIZE,      /**< Velikost dat je 0. */
        MZCMT_DIRECT_ERROR_INVALID_RATE,   /**< Neplatna vzorkovaci frekvence (0). */
        MZCMT_DIRECT_ERROR_VSTREAM,        /**< Chyba pri vytvareni nebo zapisu do vstreamu. */
    } en_MZCMT_DIRECT_ERROR;


    /**
     * @brief Vytvori CMT vstream z datoveho bloku DIRECT kodovanim.
     *
     * Generuje kompletni DIRECT signal: GAP (leader) -> polaritni pulz ->
     * sync kod -> data (12 vzorku na bajt: 8 dat + 4 synchro, MSB first) ->
     * fade-out.
     *
     * Frame struktura (GAP, polarity, sync, fadeout) je shodna s FSK a SLOW
     * kodery. Pulzni sirky frame se skaluji proporcionalne z referencni
     * frekvence 44100 Hz na cilovou frekvenci @p rate.
     *
     * Datove a synchro bity se neskaluji - kazdy element je 1 audio vzorek.
     * Rychlost prenosu dat je tak primo umerna vzorkovaci frekvenci:
     * pri 44100 Hz je to 44100/12 = 3675 bajtu/s.
     *
     * Signal zacina v LOW stavu.
     *
     * @param data      Ukazatel na datovy blok k zakodovani.
     * @param data_size Velikost datoveho bloku v bajtech (musi byt > 0).
     * @param rate      Vzorkovaci frekvence vystupniho vstreamu (Hz, musi byt > 0).
     * @return Ukazatel na novy vstream, nebo NULL pri chybe.
     *
     * @pre @p data != NULL
     * @pre @p data_size > 0
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream a musi
     *       jej uvolnit volanim cmt_vstream_destroy().
     * @post Navratova hodnota == NULL => zadna pamet nebyla alokovana.
     *
     * @note DIRECT kodovani nepridava stop bity za datove bajty.
     *       Checksum neni soucasti signalu.
     * @note Na rozdil od FSK a SLOW nema DIRECT rychlostni urovne.
     *       Rychlost prenosu je dana vzorkovaci frekvenci.
     */
    extern st_CMT_VSTREAM* mzcmt_direct_create_vstream (
        const uint8_t *data,
        uint32_t data_size,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream z datoveho bloku DIRECT kodovanim.
     *
     * Obaluje mzcmt_direct_create_vstream() do polymorfniho st_CMT_STREAM.
     * Pro typ CMT_STREAM_TYPE_BITSTREAM interno vytvori vstream a konvertuje.
     *
     * @param data      Ukazatel na datovy blok k zakodovani.
     * @param data_size Velikost datoveho bloku v bajtech (musi byt > 0).
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz, musi byt > 0).
     * @return Ukazatel na novy stream, nebo NULL pri chybe.
     *
     * @pre @p data != NULL
     * @pre @p data_size > 0
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni stream a musi
     *       jej uvolnit volanim cmt_stream_destroy().
     */
    extern st_CMT_STREAM* mzcmt_direct_create_stream (
        const uint8_t *data,
        uint32_t data_size,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /**
     * @brief Spocita XOR checksum datoveho bloku (pro DIRECT loader).
     *
     * DIRECT dekoder na Z80 pocita prubezny XOR vsech prijatych bajtu
     * do registru A' (alternativni akumulator). Po prijeti vsech bajtu
     * porovnava vysledek s hodnotou ulozenou v kodu loaderu na adrese
     * checksumx (instrukce CP $xx v direct.asm).
     *
     * @param data Ukazatel na datovy blok.
     * @param size Velikost datoveho bloku v bajtech.
     * @return XOR vsech bajtu v bloku. Vraci 0 pro NULL nebo size==0.
     */
    extern uint8_t mzcmt_direct_compute_checksum ( const uint8_t *data, uint32_t size );


    /**
     * @brief Pulzni sady pro NORMAL FM kodovani v preloader casti.
     *
     * Urcuje vychozi casovani pulzu pro NORMAL FM kodovani preloaderu
     * a loaderu. Mapuje se na stejne hodnoty jako MZCMT_TURBO_PULSESET.
     */
    typedef enum en_MZCMT_DIRECT_PULSESET {
        MZCMT_DIRECT_PULSESET_700 = 0, /**< MZ-700, MZ-80K, MZ-80A. */
        MZCMT_DIRECT_PULSESET_800,     /**< MZ-800, MZ-1500. */
        MZCMT_DIRECT_PULSESET_80B,     /**< MZ-80B. */
        MZCMT_DIRECT_PULSESET_COUNT    /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_DIRECT_PULSESET;


    /**
     * @brief Konfigurace pro generovani kompletniho paskoveho signalu s loaderem.
     *
     * Urcuje vsechny parametry potrebne pro vygenerovani funkcni pasky:
     * preloader v NORMAL FM + DIRECT loader body v NORMAL FM + uzivatelska data v DIRECT.
     *
     * @par Rozdil od FSK:
     * DIRECT nema rychlostni urovne pro datovou cast. Rychlost prenosu dat
     * je primo umerna vzorkovaci frekvenci (kazdy datovy element = 1 vzorek).
     *
     * @par Invarianty:
     * - pulseset musi byt < MZCMT_DIRECT_PULSESET_COUNT
     * - loader_speed je rychlost ROM pro nacteni DIRECT loaderu (0-6, 0 = standard)
     */
    typedef struct st_MZCMT_DIRECT_TAPE_CONFIG {
        en_MZCMT_DIRECT_PULSESET pulseset; /**< pulzni sada pro NORMAL FM casti */
        uint8_t loader_speed;              /**< rychlost ROM pro nacteni DIRECT loaderu (0-6) */
    } st_MZCMT_DIRECT_TAPE_CONFIG;


    /**
     * @brief Vytvori CMT vstream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Generuje trojdilny signal na pasce:
     * 1. PRELOADER v NORMAL FM pri 1:1 rychlosti (ROM ho nacte standardne)
     * 2. DIRECT LOADER body v NORMAL FM pri loader_speed (preloader ho nacte)
     * 3. Uzivatelska data v DIRECT formatu (DIRECT loader je nacte)
     *
     * Preloader je loader_turbo (128B, fsize=0, Z80 kod v komentari hlavicky).
     * Po nacteni ROMem modifikuje readpoint a vola ROM cteci rutinu pro
     * nacteni DIRECT loaderu. DIRECT loader pak cte data primym bitovym zapisem.
     *
     * Oba loadery jsou automaticky patchovany s parametry uzivatelskeho MZF
     * (velikost, startovni adresa, exec adresa, delay, XOR checksum).
     *
     * @param original  Originalni MZF hlavicka (zdrojovy program).
     * @param body      Ukazatel na datove telo k zakodovani.
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace (pulseset, loader_speed).
     * @param rate      Vzorkovaci frekvence (Hz).
     * @return Novy vstream s kompletnim signalem, nebo NULL pri chybe.
     *
     * @pre @p original != NULL
     * @pre @p body != NULL pokud @p body_size > 0
     * @pre @p config != NULL
     * @pre @p config->pulseset < MZCMT_DIRECT_PULSESET_COUNT
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream.
     */
    extern st_CMT_VSTREAM* mzcmt_direct_create_tape_vstream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_DIRECT_TAPE_CONFIG *config,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Obaluje mzcmt_direct_create_tape_vstream() do polymorfniho st_CMT_STREAM.
     *
     * @param original  Originalni MZF hlavicka (zdrojovy program).
     * @param body      Ukazatel na datove telo k zakodovani.
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace (pulseset, loader_speed).
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz).
     * @return Novy stream, nebo NULL pri chybe.
     *
     * @note Čitelnost zavaděčem závisí na vzorkovací frekvenci: při 44100 Hz
     *       prošly v emulátoru všechny úrovně, jinde jen některé
     *       (měření v emulátoru). Player změřené nečitelné
     *       kombinace odmítne; tato funkce frekvenci nekontroluje.
     */
    extern st_CMT_STREAM* mzcmt_direct_create_tape_stream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_DIRECT_TAPE_CONFIG *config,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /** @brief Uzivatelsky alokator - umoznuje nahradit vychozi malloc/calloc/free. */
    typedef struct st_MZCMT_DIRECT_ALLOCATOR {
        void* (*alloc)(size_t size);   /**< alokace pameti (jako malloc) */
        void* (*alloc0)(size_t size);  /**< alokace s nulovanim (jako calloc) */
        void  (*free)(void *ptr);      /**< uvolneni pameti */
    } st_MZCMT_DIRECT_ALLOCATOR;

    /**
     * @brief Nastavi vlastni alokator pro knihovnu mzcmt_direct.
     * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset na vychozi.
     */
    extern void mzcmt_direct_set_allocator ( const st_MZCMT_DIRECT_ALLOCATOR *allocator );


    /** @brief Typ callback funkce pro hlaseni chyb. */
    typedef void (*mzcmt_direct_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastavi vlastni callback pro chybova hlaseni.
     * @param cb Callback funkce, nebo NULL pro reset na vychozi (fprintf stderr).
     */
    extern void mzcmt_direct_set_error_callback ( mzcmt_direct_error_cb cb );


    /** @brief Verze knihovny mzcmt_direct. */
#define MZCMT_DIRECT_VERSION "1.0.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzcmt_direct.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzcmt_direct_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZCMT_DIRECT_H */
