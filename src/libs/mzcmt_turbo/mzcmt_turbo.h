/**
 * @file   mzcmt_turbo.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.3.0
 * @brief  TURBO koder pro kazetovy zaznam Sharp MZ (NORMAL FM s konfigurovatelnym casovanim).
 *
 * Knihovna implementuje TURBO kodovani pro Sharp MZ. TURBO je standardni
 * FM modulace (1 bit na pulz, MSB first, stop bit za kazdym bajtem) se
 * zmenenymi pulznimi casovanimi oproti ROM vychozim hodnotam. Pouzivaji
 * jej programy TurboCopy v1.0 a v1.2x, ktere kopirouji ROM do RAM a meni
 * readpoint prodlevu pro rychlejsi cteni z pasky.
 *
 * Na rozdil od FSK/SLOW/DIRECT koderu, ktere generuji signal pouze pro
 * datove telo, TURBO koder generuje kompletni paskovy ramec vcetne GAP,
 * tapemarku, hlavicky, tela, checksumu a volitelnych kopii. Duvod: TURBO
 * pouziva stejnou ramcovou strukturu jako standardni NORMAL format.
 *
 * @par Struktura TURBO signalu (SANE - bez kopii):
 * @verbatim
 *   [LGAP]      lgap_length kratkych pulzu (vychozi 22000)
 *   [LTM]       40 long + 40 short pulzu (dlouhy tapemark)
 *   [2L]        2 long pulzy
 *   [HDR]       128 bajtu hlavicky (8 bitu + 1 stop bit na bajt, MSB first)
 *   [CHKH]      2 bajty checksum hlavicky (big-endian, pocet 1-bitu)
 *   [2L]        2 long pulzy
 *   [SGAP]      sgap_length kratkych pulzu (vychozi 11000)
 *   [STM]       20 long + 20 short pulzu (kratky tapemark)
 *   [2L]        2 long pulzy
 *   [BODY]      N bajtu tela (8 bitu + 1 stop bit na bajt, MSB first)
 *   [CHKB]      2 bajty checksum tela (big-endian, pocet 1-bitu)
 *   [2L]        2 long pulzy
 * @endverbatim
 *
 * @par Volitelne kopie (dle flagu):
 * - TMZ_TURBO_FLAG_HEADER_COPY: za CHKH+2L nasleduje 256S + HDR + CHKH + 2L
 * - TMZ_TURBO_FLAG_BODY_COPY: za CHKB+2L nasleduje 256S + BODY + CHKB + 2L
 *
 * @par Pulzní časování (od verze 2.1.0, tape API mzcmt_turbo_create_tape_*):
 * Datová část a čtecí bod N se počítají z profilu zapisovače (výchozí
 * TurboCopy V1.2x, mzcmt_turbo_get_timing). Konfigurace umožňuje zadat
 * vlastní délky pulzů v jednotkách 0,1 us (pole *_us100 - název je
 * historický); N se pak spočítá z pulzů. Generátor odmítne časování,
 * které ROM se zapsaným N nepřečte s rezervou MZCMT_TURBO_MIN_MARGIN_US
 * (mzcmt_turbo_check_readpoint). Předzavaděč se zapisuje 1:1 pulzy sady.
 * Dřívější verze dělily pulzy sady poměrem rychlosti nezávisle na N;
 * podle vzorkovací frekvence byly 2:1, 7:3, 8:3 a 3:1 nečitelné.
 * Model čtecího bodu i S-model jsou pro MZ-800 (CPU 3 546 895 Hz, CTC0
 * 1 108 405 Hz) bez ohledu na pulzní sadu; pro MZ-700 [neověřeno].
 *
 * @par Experimentální API bez předzavaděče:
 * mzcmt_turbo_create_vstream/_stream (bez volajících v projektu) dál
 * dělí pulzy sady poměrem rychlosti bez vazby na N a bez kontroly
 * čitelnosti. Stejná konfigurace tam dává jiné pulzy než tape API.
 *
 * @par Checksum:
 * NORMAL FM checksum = 16bitovy pocet jednickovych bitu v datech,
 * odesilany big-endian (horni bajt prvni). Shodny s ROM algoritmem.
 *
 * @par Referencni implementace:
 * - cmttool: cmttool_turbo.c (detekce loaderu)
 * - mzftools: src/loader.c (readpoint delay tabulky)
 * - mzftools: src/turbo.asm (Z80 loader)
 * - src/libs/mztape/mztape.c (referencni NORMAL FM implementace)
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


#ifndef MZCMT_TURBO_H
#define MZCMT_TURBO_H

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
     * aby mzcmt_turbo nezaviselo na mztape.
     *
     * Invariant: hodnota musi byt < MZCMT_TURBO_PULSESET_COUNT.
     */
    typedef enum en_MZCMT_TURBO_PULSESET {
        MZCMT_TURBO_PULSESET_700 = 0, /**< MZ-700, MZ-80K, MZ-80A. */
        MZCMT_TURBO_PULSESET_800,     /**< MZ-800, MZ-1500. */
        MZCMT_TURBO_PULSESET_80B,     /**< MZ-80B. */
        MZCMT_TURBO_PULSESET_COUNT    /**< Pocet platnych hodnot (sentinel). */
    } en_MZCMT_TURBO_PULSESET;


    /**
     * @brief Profil zapisovače, podle kterého se počítá časování datové části TURBO.
     *
     * Profil určuje šířky pulzů dat i čtecí bod N zapsaný do předzavaděče.
     * Obojí se počítá z jednoho místa, aby k sobě pulzy a N vždy patřily.
     *
     * Invariant: hodnota musí být < MZCMT_TURBO_PROFILE_COUNT.
     */
    typedef enum en_MZCMT_TURBO_PROFILE {
        /**
         * TurboCopy V1.2x: poměr a:b s číslicemi 1-9, pulzy z čítače 8253
         * (CTC0 1 108 405 Hz) S = 71 + floor(480*b/a) tiků, půlvlna SHORT S/2,
         * půlvlna LONG S (S-model je hypotéza nafitovaná na nahrávky
         * z emulátoru),
         * čtecí bod N = floor(82*b/a) (ověřeno z nahrávky).
         */
        MZCMT_TURBO_PROFILE_TURBOCOPY_V12X = 0,
        MZCMT_TURBO_PROFILE_COUNT            /**< Počet profilů (sentinel). */
    } en_MZCMT_TURBO_PROFILE;


    /**
     * @brief Chybove kody TURBO koderu.
     */
    typedef enum en_MZCMT_TURBO_ERROR {
        MZCMT_TURBO_OK = 0,               /**< Bez chyby. */
        MZCMT_TURBO_ERROR_NULL_HEADER,     /**< Ukazatel na hlavicku je NULL. */
        MZCMT_TURBO_ERROR_NULL_BODY,       /**< Ukazatel na body je NULL pri body_size > 0. */
        MZCMT_TURBO_ERROR_NULL_CONFIG,     /**< Konfiguracni struktura je NULL. */
        MZCMT_TURBO_ERROR_INVALID_PULSESET, /**< Neplatna pulzni sada. */
        MZCMT_TURBO_ERROR_INVALID_SPEED,   /**< Neplatna rychlost. */
        MZCMT_TURBO_ERROR_INVALID_RATE,    /**< Neplatna vzorkovaci frekvence (0). */
        MZCMT_TURBO_ERROR_VSTREAM,         /**< Chyba pri vytvareni nebo zapisu do vstreamu. */
    } en_MZCMT_TURBO_ERROR;


/** @brief Vychozi delka dlouheho GAPu - 22 000 kratkych pulzu. */
#define MZCMT_TURBO_LGAP_DEFAULT   22000
/** @brief Vychozi delka kratkeho GAPu - 11 000 kratkych pulzu. */
#define MZCMT_TURBO_SGAP_DEFAULT   11000

/** @brief Pocet long pulzu v dlouhem tapemarku. */
#define MZCMT_TURBO_LTM_LONG       40
/** @brief Pocet short pulzu v dlouhem tapemarku. */
#define MZCMT_TURBO_LTM_SHORT      40
/** @brief Pocet long pulzu v kratkem tapemarku. */
#define MZCMT_TURBO_STM_LONG       20
/** @brief Pocet short pulzu v kratkem tapemarku. */
#define MZCMT_TURBO_STM_SHORT      20

/** @brief Pocet short pulzu v separacni sekci pred kopii. */
#define MZCMT_TURBO_COPY_SEP       256

/** @brief Priznak: blok obsahuje kopii hlavicky (header nahran 2x). */
#define MZCMT_TURBO_FLAG_HEADER_COPY  0x01
/** @brief Priznak: blok obsahuje kopii tela (body nahrano 2x). */
#define MZCMT_TURBO_FLAG_BODY_COPY    0x02


    /**
     * @brief Konfigurace TURBO koderu.
     *
     * Určuje všechny parametry generovaného signálu. Nulové hodnoty
     * u volitelných polí znamenají výchozí hodnoty: lgap/sgap konstanty
     * MZCMT_TURBO_*_DEFAULT, pulzy datové části z profilu (tape API) nebo
     * z pulzní sady dělené poměrem (experimentální API bez předzavaděče).
     *
     * @par Invarianty:
     * - pulseset musí být < MZCMT_TURBO_PULSESET_COUNT; v tape API určuje
     *   jen pulzy předzavaděče a 80B je chyba
     * - bez vlastních pulzů musí profil rychlost podporovat
     *   (mzcmt_turbo_speed_is_supported); s vlastními pulzy se speed ignoruje
     *
     * @par Jednotky pulzních délek:
     * Pole *_us100 jsou navzdory názvu v jednotkách 0,1 us (kód je tak vždy
     * počítal). Hodnota 4700 odpovídá 470,0 us. Nulová
     * hodnota = časování z profilu. Jsou-li zadány vlastní pulzy, čtecí bod N
     * se spočítá z nich (mzcmt_turbo_get_timing).
     *
     * @par Profil:
     * Pole profile má hodnotu 0 = TurboCopy V1.2x, takže konfigurace
     * vynulovaná přes memset dostane výchozí profil.
     */
    typedef struct st_MZCMT_TURBO_CONFIG {
        en_MZCMT_TURBO_PULSESET pulseset; /**< pulzní sada předzavaděče (tape API), resp. základ škálování (API bez předzavaděče) */
        en_CMTSPEED speed;                /**< rychlostní poměr; s vlastními pulzy se v tape API ignoruje */
        uint32_t lgap_length;             /**< delka LGAP v kratkych pulzech (0 = 22000) */
        uint32_t sgap_length;             /**< delka SGAP v kratkych pulzech (0 = 11000) */
        uint16_t long_high_us100;         /**< HIGH část LONG pulzu (0,1 us, 0 = z profilu) */
        uint16_t long_low_us100;          /**< LOW část LONG pulzu (0,1 us, 0 = z profilu) */
        uint16_t short_high_us100;        /**< HIGH část SHORT pulzu (0,1 us, 0 = z profilu) */
        uint16_t short_low_us100;         /**< LOW část SHORT pulzu (0,1 us, 0 = z profilu) */
        uint8_t flags;                    /**< priznaky (MZCMT_TURBO_FLAG_*) */
        en_MZCMT_TURBO_PROFILE profile;   /**< profil zapisovače (0 = TurboCopy V1.2x) */
    } st_MZCMT_TURBO_CONFIG;


    /**
     * @brief Časování datové části TURBO po kvantizaci na vzorky.
     *
     * Invariant (po úspěšném mzcmt_turbo_get_timing): všechny šířky >= 1,
     * readpoint v 1..255.
     */
    typedef struct st_MZCMT_TURBO_TIMING {
        uint8_t readpoint;      /**< čtecí bod N zapsaný předzavaděčem na 0A4Bh (ROM po patchi) */
        uint32_t short_high;    /**< HIGH část SHORT pulzu ve vzorcích */
        uint32_t short_low;     /**< LOW část SHORT pulzu ve vzorcích */
        uint32_t long_high;     /**< HIGH část LONG pulzu ve vzorcích */
        uint32_t long_low;      /**< LOW část LONG pulzu ve vzorcích */
    } st_MZCMT_TURBO_TIMING;


    /**
     * @brief Rezervy tří podmínek čtecího bodu ROM v mikrosekundách.
     *
     * Záporná hodnota znamená, že ROM rutina s daným N signál nepřečte.
     * Podmínky (T-stavy CPU 3 546 895 Hz po hraně, podle disassembly
     * zavaděče TurboCopy): SHORT HIGH < 86+14N,
     * LONG HIGH > 138+14N, perioda SHORT > 256+14N.
     */
    typedef struct st_MZCMT_TURBO_MARGINS {
        double short_high_us;   /**< (86+14N) - SHORT_H: SHORT HIGH skončí před vzorkem */
        double long_high_us;    /**< LONG_H - (138+14N): LONG HIGH trvá přes vzorek */
        double short_period_us; /**< SHORT_H+SHORT_L - (256+14N): perioda delší než návrat do hlídání hrany */
    } st_MZCMT_TURBO_MARGINS;


/**
 * @brief Minimální přijatelná rezerva čtecího bodu (us).
 *
 * Minimální rezerva čtecího bodu je 10 µs. Generátor pásky
 * odmítne časování s menší rezervou kterékoliv ze tří podmínek.
 */
#define MZCMT_TURBO_MIN_MARGIN_US   10.0


    /**
     * @brief Vytvori CMT vstream z MZF dat TURBO kodovanim.
     *
     * Generuje kompletni paskovy ramec ve formatu NORMAL FM s pulznim
     * casovanim dle konfigurace:
     *
     * 1. LGAP (leader ton - kratke pulzy)
     * 2. Dlouhy tapemark (40 long + 40 short)
     * 3. 2 long + hlavicka (128 B, 8 bitu + stop bit) + checksum + 2 long
     * 4. [pokud HEADER_COPY: 256 short + hlavicka + checksum + 2 long]
     * 5. SGAP (kratke pulzy)
     * 6. Kratky tapemark (20 long + 20 short)
     * 7. 2 long + telo (N B, 8 bitu + stop bit) + checksum + 2 long
     * 8. [pokud BODY_COPY: 256 short + telo + checksum + 2 long]
     *
     * Signal zacina v HIGH stavu (shodne s mztape). Checksum je 16bitovy
     * pocet jednickovych bitu, odesilany big-endian.
     *
     * @param header    Ukazatel na 128B MZF hlavicku v originalni endianite.
     * @param body      Ukazatel na datove telo (muze byt NULL pokud body_size == 0).
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace koderu (nesmi byt NULL).
     * @param rate      Vzorkovaci frekvence vystupniho vstreamu (Hz, musi byt > 0).
     * @return Ukazatel na novy vstream, nebo NULL pri chybe.
     *
     * @pre @p header != NULL
     * @pre @p body != NULL pokud @p body_size > 0
     * @pre @p config != NULL
     * @pre @p config->pulseset < MZCMT_TURBO_PULSESET_COUNT
     * @pre cmtspeed_is_valid(@p config->speed)
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream a musi
     *       jej uvolnit volanim cmt_vstream_destroy().
     * @post Navratova hodnota == NULL => zadna pamet nebyla alokovana.
     */
    extern st_CMT_VSTREAM* mzcmt_turbo_create_vstream (
        const uint8_t *header,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_TURBO_CONFIG *config,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream z MZF dat TURBO kodovanim.
     *
     * Obaluje mzcmt_turbo_create_vstream() do polymorfniho st_CMT_STREAM.
     * Pro typ CMT_STREAM_TYPE_BITSTREAM interno vytvori vstream a konvertuje.
     *
     * @param header    Ukazatel na 128B MZF hlavicku v originalni endianite.
     * @param body      Ukazatel na datove telo (muze byt NULL pokud body_size == 0).
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace koderu (nesmi byt NULL).
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz, musi byt > 0).
     * @return Ukazatel na novy stream, nebo NULL pri chybe.
     *
     * @pre @p header != NULL
     * @pre @p body != NULL pokud @p body_size > 0
     * @pre @p config != NULL
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni stream a musi
     *       jej uvolnit volanim cmt_stream_destroy().
     */
    extern st_CMT_STREAM* mzcmt_turbo_create_stream (
        const uint8_t *header,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_TURBO_CONFIG *config,
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
    extern uint16_t mzcmt_turbo_compute_checksum ( const uint8_t *data, uint16_t size );


    /**
     * @brief Spočítá časování datové části TURBO (pulzy ve vzorcích a čtecí bod N).
     *
     * Bez vlastních pulzů se časování bere z profilu (config->profile) a
     * rychlosti (config->speed). S vlastními pulzy (kterékoliv pole *_us100
     * nenulové, jednotka 0,1 us) se pulzy převezmou a N se zvolí jako střed
     * intervalu, ve kterém platí všechny tři podmínky čtecího bodu.
     * Chybějící půlvlna pulzu se doplní z druhé půlvlny téhož pulzu.
     *
     * Funkce nekontroluje rezervy; to dělá mzcmt_turbo_check_readpoint().
     *
     * @param config Konfigurace kodéru (nesmí být NULL).
     * @param rate   Vzorkovací frekvence (Hz, > 0).
     * @param[out] timing Výsledné časování (nesmí být NULL; při chybě vynulované).
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE při neplatném vstupu, neznámém
     *         profilu, rychlosti, kterou profil neumí (TurboCopy: 25:14,
     *         2:1 CP/M), neúplných vlastních pulzech (obě půlvlny pulzu nulové)
     *         nebo vlastních pulzech bez přípustného N. Chyba se hlásí přes
     *         error callback.
     */
    extern int mzcmt_turbo_get_timing ( const st_MZCMT_TURBO_CONFIG *config, uint32_t rate, st_MZCMT_TURBO_TIMING *timing );


    /** @brief Velikost těla předzavaděče TurboCopy v bajtech (loader na D400h). */
#define MZCMT_TURBO_PRELOADER_SIZE 90

    /**
     * @brief Převede rychlost na poměr a:b s číslicemi 1-9 (zápis TurboCopy).
     *
     * @param speed Rychlost.
     * @param[out] a Čitatel (1-9); nesmí být NULL.
     * @param[out] b Jmenovatel (1-9); nesmí být NULL.
     * @return EXIT_SUCCESS (1:1, 2:1, 3:1, 3:2, 7:3, 8:3, 9:7), jinak
     *         EXIT_FAILURE (25:14, 2:1 CP/M a neplatné hodnoty TurboCopy zadat neumí).
     */
    extern int mzcmt_turbo_speed_to_ratio ( en_CMTSPEED speed, unsigned *a, unsigned *b );

    /**
     * @brief Sestaví hlavičku a tělo předzavaděče TurboCopy (Loader Y).
     *
     * Hlavička: kopie originálu s fsize = 90, fstrt = fexec = D400h a
     * signaturou TurboCopy v cmnt[0..6]. Tělo: 75 B generického loaderu,
     * čtecí bod N na 4Bh, read_param 01h na 4Ch, fsize/fstrt/fexec programu
     * (LE) na 4Dh-52h a původní cmnt[0..6] na 53h. fname.terminator (11h)
     * hlavičky se zkopíruje z originálu beze změny, jako TurboCopy V1.21
     * (záznamy z emulátoru: vstup s 0Dh -> 0Dh, vstup s 00h -> 00h).
     *
     * @param original Hlavička programu v pořadí hostitele (jak ji vrací knihovna mzf).
     * @param body_size Velikost těla programu (nejvýš 65535).
     * @param readpoint Čtecí bod N, který předzavaděč zapíše na 0A4Bh.
     * @param[out] header_out 128 B hlavičky předzavaděče v pořadí MZF souboru (LE).
     * @param[out] body_out Tělo předzavaděče (MZCMT_TURBO_PRELOADER_SIZE B).
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE při NULL parametru a body_size > 65535.
     *
     * @note Výstupní buffery vlastní volající; funkce nealokuje.
     */
    extern int mzcmt_turbo_build_preloader ( const st_MZF_HEADER *original, uint32_t body_size, uint8_t readpoint,
                                             uint8_t header_out[128], uint8_t body_out[MZCMT_TURBO_PRELOADER_SIZE] );

    /**
     * @brief Zjistí, zda profil zapisovače umí danou rychlost (bez vlastních pulzů).
     *
     * Slouží nástrojům k odmítnutí rychlosti už při vytváření bloku 0x41,
     * ne až při přehrávání. TurboCopy V1.2x umí poměry s číslicemi 1-9:
     * 1:1, 2:1, 3:2, 7:3, 8:3, 9:7, 3:1; neumí 25:14 a 2:1 CP/M.
     *
     * @param profile Profil zapisovače.
     * @param speed Rychlost.
     * @return 1 pokud je rychlost podporovaná, jinak 0 (také pro neznámý profil).
     */
    extern int mzcmt_turbo_speed_is_supported ( en_MZCMT_TURBO_PROFILE profile, en_CMTSPEED speed );


    /**
     * @brief Ověří, že ROM rutina se čtecím bodem N přečte dané pulzy.
     *
     * Omezení: periodu LONG (LONG_H + LONG_L) nekontroluje. U profilu
     * (symetrické pulzy) plyne z podmínky LONG HIGH; u vlastních silně
     * asymetrických pulzů s velmi krátkou LOW částí LONG může kontrola
     * projít, i když čtení na hranici bajtu selže [neověřeno].
     *
     * Vyhodnotí tři podmínky (viz st_MZCMT_TURBO_MARGINS) na časování po
     * kvantizaci na vzorky při taktu CPU MZ-800 3 546 895 Hz. Okno vzorku
     * 0..52 T a návrat 204..256 T jsou výpočet z ROM (0A4Ah) podle
     * disassembly zavaděče TurboCopy; do rezervy se počítá vždy
     * horší okraj okna.
     *
     * @param timing Časování (nesmí být NULL).
     * @param rate   Vzorkovací frekvence, na kterou je časování kvantováno (Hz, > 0).
     * @param[out] margins Rezervy v us (může být NULL); vyplní se vždy, při
     *             neplatném vstupu nulami.
     * @return EXIT_SUCCESS, pokud jsou všechny rezervy >= MZCMT_TURBO_MIN_MARGIN_US,
     *         jinak EXIT_FAILURE (také při neplatném vstupu).
     */
    extern int mzcmt_turbo_check_readpoint ( const st_MZCMT_TURBO_TIMING *timing, uint32_t rate, st_MZCMT_TURBO_MARGINS *margins );


    /**
     * @brief Vytvori CMT vstream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Generuje dvoudilny signal:
     * 1. TURBO loader header v NORMAL FM 1:1 (ROM ho nacte, fsize=0, Z80 kod
     *    v komentari modifikuje readpoint a vola ROM cteci rutinu)
     * 2. Uzivatelska data v TURBO formatu (loader je nacte modifikovanym ROMem)
     *
     * Loader je automaticky patchovan s parametry ciloveho MZF programu
     * (velikost, startovni adresa, exec adresa, čtecí bod N).
     *
     * Pulzy datové části a N pocházejí z mzcmt_turbo_get_timing() (profil
     * zapisovače, nebo vlastní pulzy). Předzavaděč se vždy zapisuje 1:1
     * pulzy zvolené sady.
     *
     * @param original  Originalni MZF hlavicka (zdrojovy program).
     * @param body      Ukazatel na datove telo k zakodovani.
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace TURBO kodéru (pulseset, speed, profile, GAP délky, vlastní pulzy, flags).
     * @param rate      Vzorkovaci frekvence (Hz).
     * @return Novy vstream s kompletnim signalem, nebo NULL pri chybe.
     *         Chybou je i: pulzní sada 80B, rychlost nepodporovaná profilem
     *         (TurboCopy: 25:14, 2:1 CP/M), a časování, které ROM se
     *         zapsaným N nepřečte s rezervou MZCMT_TURBO_MIN_MARGIN_US
     *         při dané vzorkovací frekvenci (např. vysoké rychlosti při
     *         22 050 Hz). Důvod se hlásí přes error callback.
     *
     * @pre @p original != NULL
     * @pre @p body != NULL pokud @p body_size > 0
     * @pre @p config != NULL
     * @pre @p rate > 0
     *
     * @post Navratova hodnota != NULL => volajici vlastni vstream.
     */
    extern st_CMT_VSTREAM* mzcmt_turbo_create_tape_vstream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_TURBO_CONFIG *config,
        uint32_t rate
    );


    /**
     * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
     *
     * Obaluje mzcmt_turbo_create_tape_vstream() do polymorfniho st_CMT_STREAM.
     *
     * @param original  Originalni MZF hlavicka (zdrojovy program).
     * @param body      Ukazatel na datove telo k zakodovani.
     * @param body_size Velikost datoveho tela v bajtech.
     * @param config    Konfigurace TURBO koderu.
     * @param type      Typ vystupniho streamu (bitstream/vstream).
     * @param rate      Vzorkovaci frekvence (Hz).
     * @return Novy stream, nebo NULL pri chybe.
     */
    extern st_CMT_STREAM* mzcmt_turbo_create_tape_stream (
        const st_MZF_HEADER *original,
        const uint8_t *body,
        uint32_t body_size,
        const st_MZCMT_TURBO_CONFIG *config,
        en_CMT_STREAM_TYPE type,
        uint32_t rate
    );


    /** @brief Uzivatelsky alokator - umoznuje nahradit vychozi malloc/calloc/free. */
    typedef struct st_MZCMT_TURBO_ALLOCATOR {
        void* (*alloc)(size_t size);   /**< alokace pameti (jako malloc) */
        void* (*alloc0)(size_t size);  /**< alokace s nulovanim (jako calloc) */
        void  (*free)(void *ptr);      /**< uvolneni pameti */
    } st_MZCMT_TURBO_ALLOCATOR;

    /**
     * @brief Nastavi vlastni alokator pro knihovnu mzcmt_turbo.
     * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset na vychozi.
     */
    extern void mzcmt_turbo_set_allocator ( const st_MZCMT_TURBO_ALLOCATOR *allocator );


    /** @brief Typ callback funkce pro hlaseni chyb. */
    typedef void (*mzcmt_turbo_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastavi vlastni callback pro chybova hlaseni.
     * @param cb Callback funkce, nebo NULL pro reset na vychozi (fprintf stderr).
     */
    extern void mzcmt_turbo_set_error_callback ( mzcmt_turbo_error_cb cb );


    /** @brief Verze knihovny mzcmt_turbo. */
#define MZCMT_TURBO_VERSION "2.3.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzcmt_turbo.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzcmt_turbo_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZCMT_TURBO_H */
