/**
 * @file   mzcmt_fastipl.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.3.0
 * @brief  FASTIPL koder pro kazetovy zaznam Sharp MZ ($BB prefix, Intercopy).
 *
 * Knihovna implementuje FASTIPL (Fast IPL) kodovani pouzivane programy
 * InterCopy v2 az v10.2. FASTIPL vyuziva IPL mechanismus MZ-800, ktery
 * pri detekci ftype=$BB spusti kod ulozeny primo v komentarove oblasti
 * MZF hlavicky. Tento loader zkopiruje ROM do RAM, modifikuje readpoint
 * prodlevu a nacte datove telo pri vyssi rychlosti.
 *
 * Na rozdil od TURBO (kde je loader v samostatnem body bloku), FASTIPL
 * ma loader vestaveny v 128B hlavicce. To umoznuje rychlejsi nahravani
 * protoze IPL mechanismus spousti loader ihned po nacteni hlavicky.
 *
 * @par Struktura FASTIPL zaznamu na pasce:
 * @verbatim
 *   === Cast 1: $BB hlavicka (standardni rychlost 1:1) ===
 *   [LGAP]      11000 kratkych pulzu
 *   [LTM]       40 long + 40 short (dlouhy tapemark)
 *   [2L]        2 long pulzy
 *   [HDR]       128 bajtu $BB hlavicky (s loaderem v komentari)
 *   [CHKH]      2 bajty checksum hlavicky
 *   [2L]        2 long pulzy
 *
 *   === Cast 2: Datove telo (turbo rychlost), bez pauzy ===
 *   [LGAP]      lgap_length kratkych pulzu (vychozi 5500)
 *   [STM]       20 long + 20 short (kratky tapemark)
 *   [2L]        2 long pulzy
 *   [BODY]      N bajtu dat (start bit LONG + 8 bitu na bajt, MSB first)
 *   [CHKB]      2 bajty checksum tela
 *   [2L]        2 long pulzy
 * @endverbatim
 *
 * @par $BB hlavicka (128 bajtu):
 * @verbatim
 *   offset 0x00:      ftype = $BB (FASTIPL identifikator)
 *   offset 0x01-0x11: fname (17 bajtu, originalni nazev souboru)
 *   offset 0x12-0x13: fsize = $0000 (LE, nulova - body neni soucasti)
 *   offset 0x14-0x15: fstrt = $1200 (LE, adresa hlavicky v pameti)
 *   offset 0x16-0x17: fexec = $1110 (LE, vstupni bod loaderu)
 *   offset 0x18:      blcount (pocet opakovani bloku)
 *   offset 0x19:      readpoint (prodleva pro vzorkovani signalu)
 *   offset 0x1A-0x1B: skutecny fsize (LE)
 *   offset 0x1C-0x1D: skutecny fstrt (LE)
 *   offset 0x1E-0x1F: skutecny fexec (LE)
 *   offset 0x20-0x7F: kod loaderu (pole loaderu ma 110 B od $12; $12-$1F
 *                     prepise hlavicka a parametry, vlastni kod je od $20)
 * @endverbatim
 *
 * @par Referencni implementace:
 * - cmttool: cmttool_fastipl.c
 * - disassembly Intercopy 10.2
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


#ifndef MZCMT_FASTIPL_H
#define MZCMT_FASTIPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mzf/mzf.h"


    /**
     * @brief Verze FASTIPL loaderu.
     *
     * V02 odpovida InterCopy v2, V07 odpovida InterCopy v7 a novejsim
     * (v7, v7.2, v8, v8.2, v10.1, v10.2). Rozdil je v jedne instrukci
     * loaderu (LD (HL),$01 u V07, bez ni u V02).
     */
    typedef enum en_MZCMT_FASTIPL_VERSION {
        MZCMT_FASTIPL_VERSION_V02 = 0, /**< InterCopy v2 (loader 110 B od $12). */
        MZCMT_FASTIPL_VERSION_V07,      /**< InterCopy v7+ (loader 110 B od $12). */
        MZCMT_FASTIPL_VERSION_COUNT     /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_FASTIPL_VERSION;


    /**
     * @brief Pulzni sady pro vychozi casovani.
     *
     * Shodne s en_MZCMT_TURBO_PULSESET, definovano zvlast
     * pro nezavislost knihovny.
     */
    typedef enum en_MZCMT_FASTIPL_PULSESET {
        MZCMT_FASTIPL_PULSESET_700 = 0, /**< MZ-700, MZ-80K, MZ-80A. */
        MZCMT_FASTIPL_PULSESET_800,     /**< MZ-800, MZ-1500. */
        MZCMT_FASTIPL_PULSESET_80B,     /**< MZ-80B. */
        MZCMT_FASTIPL_PULSESET_COUNT    /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_FASTIPL_PULSESET;


    /**
     * @brief Chybove kody FASTIPL koderu.
     */
    typedef enum en_MZCMT_FASTIPL_ERROR {
        MZCMT_FASTIPL_OK = 0,               /**< Bez chyby. */
        MZCMT_FASTIPL_ERROR_NULL_HEADER,     /**< Ukazatel na originalni hlavicku je NULL. */
        MZCMT_FASTIPL_ERROR_NULL_BODY,       /**< Ukazatel na body je NULL pri body_size > 0. */
        MZCMT_FASTIPL_ERROR_NULL_CONFIG,     /**< Konfiguracni struktura je NULL. */
        MZCMT_FASTIPL_ERROR_INVALID_VERSION, /**< Neplatna verze loaderu. */
        MZCMT_FASTIPL_ERROR_INVALID_PULSESET, /**< Neplatna pulzni sada. */
        MZCMT_FASTIPL_ERROR_INVALID_SPEED,   /**< Neplatna rychlost. */
        MZCMT_FASTIPL_ERROR_INVALID_RATE,    /**< Neplatna vzorkovaci frekvence (0). */
        MZCMT_FASTIPL_ERROR_VSTREAM,         /**< Chyba pri vytvareni nebo zapisu do vstreamu. */
    } en_MZCMT_FASTIPL_ERROR;


/** @brief Vychozi delka header LGAP - 11000 pulzu (= 22000 pul-period).
 *  Intercopy sub_19EE: header mode HL=$157C*2 = 11000 pulzu. */
#define MZCMT_FASTIPL_HEADER_LGAP_DEFAULT   11000
/** @brief Vychozi delka body LGAP - 5500 pulzu (= 11000 pul-period).
 *  Intercopy sub_19EE: body mode HL=$157C = 5500 pulzu. */
#define MZCMT_FASTIPL_BODY_LGAP_DEFAULT     5500

/** @brief Pocet long pulzu v dlouhem tapemarku. */
#define MZCMT_FASTIPL_LTM_LONG       40
/** @brief Pocet short pulzu v dlouhem tapemarku. */
#define MZCMT_FASTIPL_LTM_SHORT      40
/** @brief Pocet long pulzu v kratkem tapemarku. */
#define MZCMT_FASTIPL_STM_LONG       20
/** @brief Pocet short pulzu v kratkem tapemarku. */
#define MZCMT_FASTIPL_STM_SHORT      20

/** @brief Ftype identifikator FASTIPL hlavicky. */
#define MZCMT_FASTIPL_FTYPE          0xBB
/** @brief Adresa nacteni hlavicky v pameti MZ-800. */
#define MZCMT_FASTIPL_FSTRT          0x1200
/** @brief Vstupni bod (exec address) FASTIPL loaderu. */
#define MZCMT_FASTIPL_FEXEC          0x1110

/** @brief Offset blcount v $BB hlavicce (od zacatku hlavicky). */
#define MZCMT_FASTIPL_OFF_BLCOUNT    0x18
/** @brief Offset readpoint v $BB hlavicce. */
#define MZCMT_FASTIPL_OFF_READPOINT  0x19
/** @brief Offset skutecneho fsize v $BB hlavicce. */
#define MZCMT_FASTIPL_OFF_FSIZE      0x1A
/** @brief Offset skutecneho fstrt v $BB hlavicce. */
#define MZCMT_FASTIPL_OFF_FSTRT      0x1C
/** @brief Offset skutecneho fexec v $BB hlavicce. */
#define MZCMT_FASTIPL_OFF_FEXEC      0x1E
/** @brief Offset kodu loaderu v $BB hlavicce (= standardni fsize pozice).
 *  Loader zabira offsety $12-$7F (110 bajtu). Prvnich 6 bajtu ($12-$17)
 *  se zaroven interpretuje jako fsize=0, fstrt=$1200, fexec=$1110.
 *  Intercopy patchuje offsety $18-$1F parametry (blcount, readpoint,
 *  skutecne fsize/fstrt/fexec). */
#define MZCMT_FASTIPL_OFF_LOADER     0x12
/** @brief Velikost kodu loaderu v bajtech (110 = $6E). */
#define MZCMT_FASTIPL_LOADER_SIZE    110

/** @brief Vychozi readpoint pro standardni rychlost (ROM = $52 = 82). */
#define MZCMT_FASTIPL_READPOINT_DEFAULT 82


    /**
     * @brief Konfigurace FASTIPL koderu.
     *
     * @par Invarianty:
     * - version < MZCMT_FASTIPL_VERSION_COUNT
     * - pulseset < MZCMT_FASTIPL_PULSESET_COUNT
     * - cmtspeed_is_valid(speed)
     *
     * @par Vychozi hodnoty (pri 0):
     * - lgap_length: 5500 (body LGAP, Intercopy sub_19EE body mode)
     * - blcount: 1 (bez opakovani)
     * - readpoint: automaticky z rychlosti (mzcmt_fastipl_default_readpoint)
     * - *_us100: vychozi z pulsesetu + rychlosti
     */
    typedef struct st_MZCMT_FASTIPL_CONFIG {
        en_MZCMT_FASTIPL_VERSION version;  /**< verze loaderu (V02/V07) */
        en_MZCMT_FASTIPL_PULSESET pulseset; /**< pulzni sada pro casovani */
        en_CMTSPEED speed;                  /**< rychlost datoveho tela (turbo) */
        uint32_t lgap_length;               /**< body LGAP delka v pulzech (0 = 5500) */
        uint16_t long_high_us100;           /**< explicitni long HIGH (0,1 us, 0 = default) */
        uint16_t long_low_us100;            /**< explicitni long LOW (0,1 us, 0 = default) */
        uint16_t short_high_us100;          /**< explicitni short HIGH (0,1 us, 0 = default) */
        uint16_t short_low_us100;           /**< explicitni short LOW (0,1 us, 0 = default) */
        uint8_t blcount;                    /**< pocet opakovani bloku (0 = 1) */
        uint8_t readpoint;                  /**< readpoint prodleva (0 = auto z rychlosti) */
    } st_MZCMT_FASTIPL_CONFIG;


    /**
     * @brief Sestavi $BB FASTIPL hlavicku (128 bajtu).
     *
     * Vyplni vystupni buffer kompletni $BB hlavickou obsahujici:
     * ftype=$BB, originalni fname, fsize=0, fstrt=$1200, fexec=$1110,
     * parametry loaderu (blcount, readpoint, skutecne fsize/fstrt/fexec)
     * a kod loaderu (V02 nebo V07).
     *
     * @param[out] out_header Vystupni buffer pro 128B hlavicku (nesmi byt NULL).
     * @param original        Originalni MZF hlavicka (fname, fsize, fstrt, fexec).
     * @param config          Konfigurace (verze, readpoint, blcount).
     *
     * @pre @p out_header != NULL
     * @pre @p original != NULL
     * @pre @p config != NULL
     * @pre @p config->version < MZCMT_FASTIPL_VERSION_COUNT
     *
     * @post out_header obsahuje platnou $BB hlavicku pripravenu pro zaznam.
     */
    extern void mzcmt_fastipl_build_header (
        uint8_t *out_header,
        const st_MZF_HEADER *original,
        const st_MZCMT_FASTIPL_CONFIG *config
    );


    /**
     * @brief Vytvori CMT vstream z MZF dat FASTIPL kodovanim.
     *
     * Generuje standardni dvou-blokovy MZF zaznam (shodne s Intercopy 10.2):
     *
     * Header blok (NORMAL 1:1 rychlost):
     *   LGAP(11000) + LTM(40L+40S) + 2L + HDR(128B) + CRC + 2L
     *
     * Body blok (turbo rychlost, primo za header blokem bez pauzy):
     *   LGAP(5500) + STM(20L+20S) + 2L + BODY(N B) + CRC + 2L
     *
     * Pulzni casovani header bloku je vzdy standardni (1:1 rychlost).
     * Casovani body bloku je dle konfigurace (speed + pulseset nebo
     * explicitni hodnoty).
     *
     * @param original  Originalni MZF hlavicka (fname, fsize, fstrt, fexec).
     * @param body      Datove telo (muze byt NULL pokud body_size == 0).
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace koderu.
     * @param rate      Vzorkovaci frekvence (Hz, musi byt > 0).
     * @return Novy vstream, nebo NULL pri chybe.
     *
     * @pre @p original != NULL
     * @pre @p body != NULL pokud @p body_size > 0
     * @pre @p config != NULL
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream
     *       a musi jej uvolnit volanim cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* mzcmt_fastipl_create_vstream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_FASTIPL_CONFIG *config,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream z MZF dat FASTIPL kodovanim.
     *
     * Obaluje mzcmt_fastipl_create_vstream() do polymorfniho st_CMT_STREAM.
     *
     * @param original  Originalni MZF hlavicka.
     * @param body      Datove telo (muze byt NULL pokud body_size == 0).
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace koderu.
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz, musi byt > 0).
     * @return Novy stream, nebo NULL pri chybe.
     */
    extern st_CMT_STREAM* mzcmt_fastipl_create_stream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_FASTIPL_CONFIG *config,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /**
     * @brief Spocita NORMAL FM checksum (pocet jednickovych bitu).
     *
     * Shodny algoritmus s ROM a mzcmt_turbo_compute_checksum().
     *
     * @param data Ukazatel na datovy blok.
     * @param size Velikost datoveho bloku v bajtech.
     * @return Pocet jednickovych bitu. Vraci 0 pro NULL nebo size==0.
     */
    extern uint16_t mzcmt_fastipl_compute_checksum ( const uint8_t *data, uint16_t size );


    /**
     * @brief Vrati vychozi readpoint pro danou rychlost.
     *
     * Tabulka podle rychlosti: 1:1, 2:1, 7:3 a 8:3 (77, 32, 22, 17) jsou
     * hodnoty z tabulky programu Intercopy V10.2, ostatni rychlosti jsou
     * odhad. 82 ($52) je cteci bod ROM MZ-800 (vychozi pro neplatnou
     * rychlost).
     *
     * @param speed Rychlostni pomer.
     * @return Vychozi readpoint (1-82). Pro neplatne hodnoty vraci 82.
     */
    extern uint8_t mzcmt_fastipl_default_readpoint ( en_CMTSPEED speed );

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
    extern int mzcmt_fastipl_get_pulse_lengths ( en_MZCMT_FASTIPL_PULSESET pulseset, double out[4] );


    /** @brief Uzivatelsky alokator. */
    typedef struct st_MZCMT_FASTIPL_ALLOCATOR {
        void* (*alloc)(size_t size);
        void* (*alloc0)(size_t size);
        void  (*free)(void *ptr);
    } st_MZCMT_FASTIPL_ALLOCATOR;

    /**
     * @brief Nastavi vlastni alokator pro knihovnu mzcmt_fastipl.
     * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset.
     */
    extern void mzcmt_fastipl_set_allocator ( const st_MZCMT_FASTIPL_ALLOCATOR *allocator );

    /** @brief Typ callback funkce pro hlaseni chyb. */
    typedef void (*mzcmt_fastipl_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastavi vlastni callback pro chybova hlaseni.
     * @param cb Callback funkce, nebo NULL pro reset na vychozi (fprintf stderr).
     */
    extern void mzcmt_fastipl_set_error_callback ( mzcmt_fastipl_error_cb cb );


    /** @brief Verze knihovny mzcmt_fastipl. */
#define MZCMT_FASTIPL_VERSION "1.3.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzcmt_fastipl.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzcmt_fastipl_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZCMT_FASTIPL_H */
