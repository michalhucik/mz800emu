/**
 * @file   mzcmt_bsd.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.1
 * @brief  BSD/BRD kodér TMZ v1.0 (chunkovaný datový soubor BASICu), jen pro TAPEMZ-V1-LEGACY.
 *
 * @warning Výstup tohoto kodéru je struktura TMZ v1.0: tón (SGAP) jen
 * před prvním kusem, kusy bez kopie a bez pauz. BASIC 1Z-016 takový záznam
 * za prvním kusem nepřečte (ověřeno v emulátoru). Player ho nepoužívá:
 * blok 0x45 přehrává profilem BASIC-1Z016. Kodér zůstává jen jako
 * generátor profilu TAPEMZ-V1-LEGACY formátu bsd (zpětná kompatibilita
 * bloků 0x46 vzniklých převodem v1.0).
 *
 * Datový soubor BASICu: BASIC 1Z-016 ho zapisuje příkazy WOPEN#/PRINT#
 * s ftype 0x04 (ROM monitoru tento typ jmenuje BRD); typ 0x03 ROM jmenuje
 * BSD (Owner's Manual MZ-800, tabulka typů v ROM). Oba typy mají
 * stejný chunkovaný záznam na pásce.
 *
 * Na rozdil od standardniho MZF zaznamu, kde hlavicka obsahuje fsize
 * urcujici delku tela, u BSD/BRD je fsize=0, fstrt=0, fexec=0 a data
 * jsou rozdelena do 258B chunku. Kazdy chunk obsahuje 2B ID (LE) a 256B
 * datoveho payloadu. Chunky jsou na pasce ulozeny jako samostatne body
 * bloky s krátkym tapemarkem a vlastnim checksumem.
 *
 * Kódování používá NORMAL FM modulaci (1 bit na pulz, MSB first, start
 * bit LONG před každým bajtem). Pulzy jsou symetrické hodnoty v1.0
 * (MZ-800 249/498 µs), ne časování ROM ani BASICu.
 *
 * @par Struktura BSD/BRD signalu:
 * @verbatim
 *   [LGAP]      lgap_length kratkych pulzu (vychozi 22000)
 *   [LTM]       40 long + 40 short pulzu (dlouhy tapemark)
 *   [2L]        2 long pulzy
 *   [HDR]       128 bajtu hlavicky (8 bitu + 1 stop bit na bajt, MSB first)
 *   [CHKH]      2 bajty checksum hlavicky (big-endian, pocet 1-bitu)
 *   [2L]        2 long pulzy
 *   [kopie hlavicky: 256 short + HDR + CHKH + 2L   (pokud HEADER_COPY)]
 *   [SGAP]      sgap_length kratkych pulzu (vychozi 11000)
 *
 *   Pro kazdy chunk (0 .. chunk_count-1):
 *     [STM]     20 long + 20 short pulzu (kratky tapemark)
 *     [2L]      2 long pulzy
 *     [CHUNK]   258 bajtu chunku (8 bitu + 1 stop bit na bajt, MSB first)
 *     [CHK]     2 bajty checksum chunku (big-endian, pocet 1-bitu)
 *     [2L]      2 long pulzy
 * @endverbatim
 *
 * @par Chunk ID:
 * - Prvni chunk: ID = 0x0000
 * - Dalsi chunky: ID se inkrementuje (0x0001, 0x0002, ...)
 * - Posledni chunk: ID = 0xFFFF (ukoncujici marker)
 *
 * @par Checksum:
 * NORMAL FM checksum = 16bitovy pocet jednickovych bitu v datech,
 * odesilany big-endian (horni bajt prvni). Shodny s ROM algoritmem.
 *
 * @par Referencni implementace:
 * - cmttool: cmttool.c (BSD detekce)
 * - src/libs/mztape/mztape.c (referencni NORMAL FM implementace)
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


#ifndef MZCMT_BSD_H
#define MZCMT_BSD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mzf/mzf.h"


    /**
     * @brief Pulzni sady pro vychozi casovani (indexovano pulseset hodnotou).
     *
     * Mapuje se na en_MZTAPE_PULSESET z mztape.h. Definujeme vlastni enum,
     * aby mzcmt_bsd nezaviselo na mztape.
     *
     * Invariant: hodnota musi byt < MZCMT_BSD_PULSESET_COUNT.
     */
    typedef enum en_MZCMT_BSD_PULSESET {
        MZCMT_BSD_PULSESET_700 = 0, /**< MZ-700, MZ-80K, MZ-80A. */
        MZCMT_BSD_PULSESET_800,     /**< MZ-800, MZ-1500. */
        MZCMT_BSD_PULSESET_80B,     /**< MZ-80B. */
        MZCMT_BSD_PULSESET_COUNT    /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_BSD_PULSESET;


    /**
     * @brief Chybove kody BSD koderu.
     */
    typedef enum en_MZCMT_BSD_ERROR {
        MZCMT_BSD_OK = 0,                  /**< Bez chyby. */
        MZCMT_BSD_ERROR_NULL_HEADER,       /**< Ukazatel na hlavicku je NULL. */
        MZCMT_BSD_ERROR_NULL_CHUNKS,       /**< Ukazatel na chunkova data je NULL pri chunk_count > 0. */
        MZCMT_BSD_ERROR_NULL_CONFIG,       /**< Konfiguracni struktura je NULL. */
        MZCMT_BSD_ERROR_INVALID_PULSESET,  /**< Neplatna pulzni sada. */
        MZCMT_BSD_ERROR_INVALID_SPEED,     /**< Neplatna rychlost. */
        MZCMT_BSD_ERROR_INVALID_RATE,      /**< Neplatna vzorkovaci frekvence (0). */
        MZCMT_BSD_ERROR_ZERO_CHUNKS,       /**< chunk_count je 0 (minimalne 1 chunk je vyzadovan). */
        MZCMT_BSD_ERROR_VSTREAM,           /**< Chyba pri vytvareni nebo zapisu do vstreamu. */
    } en_MZCMT_BSD_ERROR;


/** @brief Vychozi delka dlouheho GAPu - 22 000 kratkych pulzu. */
#define MZCMT_BSD_LGAP_DEFAULT   22000
/** @brief Vychozi delka kratkeho GAPu - 11 000 kratkych pulzu. */
#define MZCMT_BSD_SGAP_DEFAULT   11000

/** @brief Pocet long pulzu v dlouhem tapemarku. */
#define MZCMT_BSD_LTM_LONG       40
/** @brief Pocet short pulzu v dlouhem tapemarku. */
#define MZCMT_BSD_LTM_SHORT      40
/** @brief Pocet long pulzu v kratkem tapemarku. */
#define MZCMT_BSD_STM_LONG       20
/** @brief Pocet short pulzu v kratkem tapemarku. */
#define MZCMT_BSD_STM_SHORT      20

/** @brief Pocet short pulzu v separacni sekci pred kopii hlavicky. */
#define MZCMT_BSD_COPY_SEP       256

/** @brief Priznak: blok obsahuje kopii hlavicky (header nahran 2x). */
#define MZCMT_BSD_FLAG_HEADER_COPY  0x01

/** @brief Velikost jednoho BSD/BRD chunku v bajtech (2B ID + 256B data). */
#define MZCMT_BSD_CHUNK_SIZE         258

/** @brief Velikost datove casti jednoho BSD/BRD chunku v bajtech. */
#define MZCMT_BSD_CHUNK_DATA_SIZE    256

/** @brief Chunk ID posledniho chunku v BSD/BRD zaznamu. */
#define MZCMT_BSD_LAST_CHUNK_ID      0xFFFF


    /**
     * @brief Konfigurace BSD koderu.
     *
     * Urcuje parametry generovaneho signalu. Nulove hodnoty u volitelnych
     * poli (lgap_length, sgap_length) znamenaji pouziti vychozich hodnot.
     *
     * BSD pouziva standardni NORMAL FM modulaci - pulzni casovani je
     * odvozeno z pulsesetu a rychlostniho divisoru (typicky 1:1).
     *
     * @par Invarianty:
     * - pulseset musi byt < MZCMT_BSD_PULSESET_COUNT
     * - speed musi byt platna en_CMTSPEED (overeni pres cmtspeed_is_valid)
     */
    typedef struct st_MZCMT_BSD_CONFIG {
        en_MZCMT_BSD_PULSESET pulseset; /**< pulzni sada pro vychozi casovani */
        en_CMTSPEED speed;              /**< rychlostni pomer (divisor), typicky CMTSPEED_1_1 */
        uint32_t lgap_length;           /**< delka LGAP v kratkych pulzech (0 = 22000) */
        uint32_t sgap_length;           /**< delka SGAP v kratkych pulzech (0 = 11000) */
        uint8_t flags;                  /**< priznaky (MZCMT_BSD_FLAG_*) */
    } st_MZCMT_BSD_CONFIG;


    /**
     * @brief Vytvori CMT vstream z BSD/BRD dat chunkovym kodovanim.
     *
     * Generuje kompletni paskovy ramec pro BSD/BRD zaznam:
     *
     * 1. LGAP (leader ton - kratke pulzy)
     * 2. Dlouhy tapemark (40 long + 40 short)
     * 3. 2 long + hlavicka (128 B, 8 bitu + stop bit) + checksum + 2 long
     * 4. [pokud HEADER_COPY: 256 short + hlavicka + checksum + 2 long]
     * 5. SGAP (kratke pulzy)
     * 6. Pro kazdy chunk:
     *    a. Kratky tapemark (20 long + 20 short)
     *    b. 2 long + chunk (258 B, 8 bitu + stop bit) + checksum + 2 long
     *
     * Signal zacina v HIGH stavu (shodne s mztape). Checksum je 16bitovy
     * pocet jednickovych bitu, odesilany big-endian.
     *
     * @param header      Ukazatel na 128B MZF hlavicku v originalni endianite.
     *                    Hlavicka by mela mit ftype=0x03 (BSD) nebo 0x04 (BRD),
     *                    fsize=0, fstrt=0, fexec=0.
     * @param chunks_data Ukazatel na chunkova data: chunk_count * 258 bajtu.
     *                    Kazdy chunk: 2B ID (LE) + 256B data. Nesmi byt NULL
     *                    pokud chunk_count > 0.
     * @param chunk_count Pocet chunku (musi byt >= 1).
     * @param config      Konfigurace koderu (nesmi byt NULL).
     * @param rate        Vzorkovaci frekvence vystupniho vstreamu (Hz, musi byt > 0).
     * @return Ukazatel na novy vstream, nebo NULL pri chybe.
     *
     * @pre @p header != NULL
     * @pre @p chunks_data != NULL
     * @pre @p chunk_count >= 1
     * @pre @p config != NULL
     * @pre @p config->pulseset < MZCMT_BSD_PULSESET_COUNT
     * @pre cmtspeed_is_valid(@p config->speed)
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream a musi
     *       jej uvolnit volanim cmt_vstream_destroy().
     * @post Navratova hodnota == NULL => zadna pamet nebyla alokovana.
     */
    extern st_CMT_VSTREAM* mzcmt_bsd_create_vstream (
        const uint8_t *header,
        const uint8_t *chunks_data,
        uint16_t chunk_count,
        const st_MZCMT_BSD_CONFIG *config,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream z BSD/BRD dat chunkovym kodovanim.
     *
     * Obaluje mzcmt_bsd_create_vstream() do polymorfniho st_CMT_STREAM.
     * Pro typ CMT_STREAM_TYPE_BITSTREAM interno vytvori vstream a konvertuje.
     *
     * @param header      Ukazatel na 128B MZF hlavicku v originalni endianite.
     * @param chunks_data Ukazatel na chunkova data: chunk_count * 258 bajtu.
     * @param chunk_count Pocet chunku (musi byt >= 1).
     * @param config      Konfigurace koderu (nesmi byt NULL).
     * @param type        Typ vystupniho streamu (bitstream/vstream).
     * @param rate        Vzorkovaci frekvence (Hz, musi byt > 0).
     * @return Ukazatel na novy stream, nebo NULL pri chybe.
     *
     * @pre @p header != NULL
     * @pre @p chunks_data != NULL
     * @pre @p chunk_count >= 1
     * @pre @p config != NULL
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni stream a musi
     *       jej uvolnit volanim cmt_stream_destroy().
     */
    extern st_CMT_STREAM* mzcmt_bsd_create_stream (
        const uint8_t *header,
        const uint8_t *chunks_data,
        uint16_t chunk_count,
        const st_MZCMT_BSD_CONFIG *config,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /**
     * @brief Spocita NORMAL FM checksum datoveho bloku.
     *
     * ROM algoritmus: scita pocet jednickovych bitu ve vsech bajtech bloku.
     * Vysledek je 16bitova hodnota (uint16_t). Checksum se na pasku
     * odesilala big-endian (horni bajt prvni).
     *
     * @param data Ukazatel na datovy blok.
     * @param size Velikost datoveho bloku v bajtech.
     * @return Pocet jednickovych bitu v bloku. Vraci 0 pro NULL nebo size==0.
     */
    extern uint16_t mzcmt_bsd_compute_checksum ( const uint8_t *data, uint16_t size );

    /**
     * @brief Délky půlvln pulzní sady při rychlosti 1:1.
     *
     * Hodnoty vnitřní tabulky pulzů (výstup v1.0) pro převod bloků v1.0
     * na TMZ v1.1 (tmz_profile, profil TAPEMZ-V1-LEGACY).
     *
     * @param pulseset Pulzní sada.
     * @param[out] out LONG H, LONG L, SHORT H, SHORT L v sekundách.
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE (out je NULL, neplatná sada).
     */
    extern int mzcmt_bsd_get_pulse_lengths ( en_MZCMT_BSD_PULSESET pulseset, double out[4] );


    /** @brief Uzivatelsky alokator - umoznuje nahradit vychozi malloc/calloc/free. */
    typedef struct st_MZCMT_BSD_ALLOCATOR {
        void* (*alloc)(size_t size);   /**< alokace pameti (jako malloc) */
        void* (*alloc0)(size_t size);  /**< alokace s nulovanim (jako calloc) */
        void  (*free)(void *ptr);      /**< uvolneni pameti */
    } st_MZCMT_BSD_ALLOCATOR;

    /**
     * @brief Nastavi vlastni alokator pro knihovnu mzcmt_bsd.
     * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset na vychozi.
     */
    extern void mzcmt_bsd_set_allocator ( const st_MZCMT_BSD_ALLOCATOR *allocator );


    /** @brief Typ callback funkce pro hlaseni chyb. */
    typedef void (*mzcmt_bsd_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastavi vlastni callback pro chybova hlaseni.
     * @param cb Callback funkce, nebo NULL pro reset na vychozi (fprintf stderr).
     */
    extern void mzcmt_bsd_set_error_callback ( mzcmt_bsd_error_cb cb );


    /** @brief Verze knihovny mzcmt_bsd. */
#define MZCMT_BSD_VERSION "1.1.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzcmt_bsd.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzcmt_bsd_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZCMT_BSD_H */
