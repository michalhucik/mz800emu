/**
 * @file   tmz_blocks.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.0
 * @brief  Definice a parsovani TMZ bloku specifickych pro pocitace Sharp MZ.
 *
 * Hlavickovy soubor definuje packed struktury pro kazdy typ TMZ bloku
 * rozsirujici TZX format o Sharp MZ specificke informace:
 *
 *   - 0x40 MZ Standard Data   - standardni MZF data (hlavicka + telo)
 *   - 0x41 MZ Turbo Data      - turbo rezim s volitelnym nastavenim pulzu
 *   - 0x42 MZ Extra Body      - dodatecny datovy blok (napr. druhy program)
 *   - 0x43 MZ Machine Info    - informace o cilove architekture
 *   - 0x44 MZ Loader          - vlastni loader pro nestandardni formaty
 *   - 0x45 MZ BASIC Data      - datovy soubor BASICu (kusy po 258 B)
 *
 * Blok 0x46 (MZ File, TMZ v1.1) definuje tmz_mzfile.h.
 *
 * Vsechna vicebajova pole jsou ulozena v little-endian poradi
 * (odpovidajici Z80 procesoru). Struktury jsou packed pro prime
 * mapovani na binarni data.
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


#ifndef TMZ_BLOCKS_H
#define TMZ_BLOCKS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "tmz.h"
#include "../mzf/mzf.h"


/* Blokova ID jsou definovana v tmz.h (TMZ_BLOCK_ID_MZ_*) */


/** @defgroup tmz_block_structs Datove struktury TMZ bloku
 *  Packed struktury pro prime mapovani na binarni data v TMZ souboru.
 *  Vsechna uint16/uint32 pole jsou v little-endian poradi.
 *  @{ */

    /**
     * @brief Blok 0x40 - MZ Standard Data.
     *
     * Standardni MZF zaznam na pasce - hlavicka (128 B) nasledovana telem.
     * Pouziva se pro bezny zaznam v originalni rychlosti s vychozimi
     * parametry pulzu. Za touto strukturou nasleduje body_size bajtu
     * datoveho tela.
     *
     * @par Invarianty:
     * - machine musi byt platna hodnota en_TMZ_MACHINE
     * - pulseset musi byt platna hodnota en_TMZ_PULSESET
     * - body_size musi odpovidat velikosti dat nasledujicich za strukturou
     * - bajt 11h (mzf_header.fname.terminator) se ukládá a čte beze změny,
     *   jak je na pásce (ROM SAVE zapíše bajt z paměti, např. 00h)
     *
     * @par Ownership:
     * - datove telo nasledujici za strukturou je soucasti binarniho bloku
     */
    typedef struct __attribute__((packed)) st_TMZ_MZ_STANDARD_DATA {
        uint8_t machine;            /**< cilovy model pocitace (en_TMZ_MACHINE) */
        uint8_t pulseset;           /**< pulzni sada (en_TMZ_PULSESET) */
        uint16_t pause_ms;          /**< pauza po bloku v milisekundach */
        st_MZF_HEADER mzf_header;   /**< MZF hlavicka (128 bajtu) */
        uint16_t body_size;         /**< velikost datoveho tela v bajtech */
        /* nasleduje body_size bajtu dat */
    } st_TMZ_MZ_STANDARD_DATA;


    /**
     * @brief Blok 0x41 - MZ Turbo Data.
     *
     * Turbo rezim zaznamu s moznosti nastaveni vsech parametru:
     * rychlost, delky pulzu, delky GAP sekci a priznaky pro kopii
     * hlavicky/tela. Hodnota 0 u volitelnych poli znamena pouziti
     * vychozich hodnot pro dany model a rychlost.
     *
     * Delky pulzu (long_high, long_low, short_high, short_low) jsou
     * v jednotkach 0,1 µs (sekundy = hodnota / 1e7), cele cislo bez
     * plovouci carky.
     *
     * Za touto strukturou nasleduje body_size bajtu datoveho tela.
     *
     * @par Invarianty:
     * - machine musi byt platna hodnota en_TMZ_MACHINE
     * - pulseset musi byt platna hodnota en_TMZ_PULSESET
     * - format musi byt platna hodnota en_TMZ_FORMAT
     * - speed musi byt platna hodnota en_CMTSPEED
     * - body_size musi odpovidat velikosti dat nasledujicich za strukturou
     *
     * @par Ownership:
     * - datove telo nasledujici za strukturou je soucasti binarniho bloku
     */
    typedef struct __attribute__((packed)) st_TMZ_MZ_TURBO_DATA {
        uint8_t machine;            /**< cilovy model pocitace (en_TMZ_MACHINE) */
        uint8_t pulseset;           /**< pulzni sada (en_TMZ_PULSESET) */
        uint8_t format;             /**< formatova varianta zaznamu (en_TMZ_FORMAT) */
        uint8_t speed;              /**< index rychlosti zaznamu - interpretace zavisi na formatu:
                                     *   FM formaty (NORMAL/TURBO/FASTIPL/SINCLAIR): en_CMTSPEED (1-9),
                                     *   FSK: en_MZCMT_FSK_SPEED (0-6),
                                     *   SLOW: en_MZCMT_SLOW_SPEED (0-4),
                                     *   DIRECT: ignorovano (0),
                                     *   CPM_TAPE: nativni uroven (0-2) */
        uint16_t lgap_length;       /**< delka LGAP v poctu kratkych pulzu (0 = vychozi) */
        uint16_t sgap_length;       /**< delka SGAP v poctu kratkych pulzu (0 = vychozi) */
        uint16_t pause_ms;          /**< pauza po bloku v milisekundach */
        uint16_t long_high;         /**< delka high casti dlouheho pulzu (0,1 µs, 0 = vychozi) */
        uint16_t long_low;          /**< delka low casti dlouheho pulzu (0,1 µs, 0 = vychozi) */
        uint16_t short_high;        /**< delka high casti kratkeho pulzu (0,1 µs, 0 = vychozi) */
        uint16_t short_low;         /**< delka low casti kratkeho pulzu (0,1 µs, 0 = vychozi) */
        uint8_t flags;              /**< priznaky (viz TMZ_TURBO_FLAG_*) */
        st_MZF_HEADER mzf_header;   /**< MZF hlavicka (128 bajtu) */
        uint16_t body_size;         /**< velikost datoveho tela v bajtech */
        /* nasleduje body_size bajtu dat */
    } st_TMZ_MZ_TURBO_DATA;


    /**
     * @brief Blok 0x42 - MZ Extra Body.
     *
     * Dodatecny datovy blok, ktery neni soucast hlavniho MZF souboru.
     * Pouziva se napr. pro druhy program v multi-part zaznamech
     * nebo pro data nahravana v jinem formatu ci rychlosti.
     *
     * Za touto strukturou nasleduje body_size bajtu dat.
     *
     * @par Invarianty:
     * - format musi byt platna hodnota en_TMZ_FORMAT
     * - speed musi byt platna hodnota en_CMTSPEED
     * - body_size musi odpovidat velikosti dat nasledujicich za strukturou
     *
     * @par Ownership:
     * - datove telo nasledujici za strukturou je soucasti binarniho bloku
     */
    typedef struct __attribute__((packed)) st_TMZ_MZ_EXTRA_BODY {
        uint8_t format;             /**< formatova varianta zaznamu (en_TMZ_FORMAT) */
        uint8_t speed;              /**< index rychlosti zaznamu (en_CMTSPEED) */
        uint16_t pause_ms;          /**< pauza po bloku v milisekundach */
        uint16_t body_size;         /**< velikost datove casti v bajtech */
        /* nasleduje body_size bajtu dat */
    } st_TMZ_MZ_EXTRA_BODY;


    /**
     * @brief Blok 0x43 - MZ Machine Info.
     *
     * Informacni blok popisujici cilovou architekturu. Neprenasi zadna
     * programova data, slouzi k upresneni parametru pro spravne
     * dekodovani nasledujicich bloku.
     *
     * @par Invarianty:
     * - machine musi byt platna hodnota en_TMZ_MACHINE
     * - cpu_clock musi byt nenulove (typicke hodnoty: 3546895 Hz pro MZ-800)
     */
    typedef struct __attribute__((packed)) st_TMZ_MZ_MACHINE_INFO {
        uint8_t machine;            /**< cilovy model pocitace (en_TMZ_MACHINE) */
        uint32_t cpu_clock;         /**< takt CPU v Hz (napr. 3546895 pro MZ-800) */
        uint8_t rom_version;        /**< verze ROM (0 = nezname) */
    } st_TMZ_MZ_MACHINE_INFO;


    /**
     * @brief Blok 0x44 - MZ Loader.
     *
     * Vlastni loader pro nestandardni formaty zaznamu. Obsahuje
     * strojovy kod loaderu a MZF hlavicku popisujici jeho umisteni
     * v pameti. Pole speed urcuje rychlost pro nasledujici datove bloky
     * (body), ktere tento loader nacita.
     *
     * Za touto strukturou nasleduje loader_size bajtu strojoveho kodu
     * loaderu.
     *
     * @par Invarianty:
     * - loader_type musi byt platna hodnota en_TMZ_LOADER_TYPE
     * - speed musi byt platna hodnota en_CMTSPEED
     * - loader_size musi odpovidat velikosti dat nasledujicich za strukturou
     *
     * @par Ownership:
     * - data loaderu nasledujici za strukturou jsou soucasti binarniho bloku
     */
    typedef struct __attribute__((packed)) st_TMZ_MZ_LOADER {
        uint8_t loader_type;        /**< typ loaderu (en_TMZ_LOADER_TYPE) */
        uint8_t speed;              /**< rychlost pro nasledujici body bloky (en_CMTSPEED) */
        uint16_t loader_size;       /**< velikost dat loaderu v bajtech */
        st_MZF_HEADER mzf_header;   /**< MZF hlavicka loaderu */
        /* nasleduje loader_size bajtu loaderu */
    } st_TMZ_MZ_LOADER;


    /**
     * @brief Blok 0x45 - MZ BASIC Data.
     *
     * BASIC datovy zaznam (BSD ftype=0x03 nebo BRD ftype=0x04).
     * Na rozdil od standardniho MZF, kde velikost dat urcuje pole fsize,
     * u BSD/BRD zaznamu je fsize=0 a data jsou rozdelena do 258B chunku.
     *
     * Kazdy chunk obsahuje 2B ID (LE) a 256B dat. Chunky jsou na pasce
     * ulozeny jako samostatne body bloky s kratkym tapemarkem.
     * Prvni chunk ma ID=0x0000, dalsi se inkrementuji, posledni ma ID=0xFFFF.
     *
     * Za touto strukturou nasleduje chunk_count * 258 bajtu chunkovych dat.
     *
     * @par Invarianty:
     * - machine musi byt platna hodnota en_TMZ_MACHINE
     * - pulseset musi byt platna hodnota en_TMZ_PULSESET
     * - mzf_header.ftype musi byt 0x03 (BSD) nebo 0x04 (BRD)
     * - mzf_header.fsize, fstrt, fexec musi byt 0
     * - chunk_count musi byt >= 1 (minimalne ukoncujici chunk)
     * - posledni chunk musi mit ID = 0xFFFF
     *
     * @par Ownership:
     * - chunky nasledujici za strukturou jsou soucasti binarniho bloku
     */
    typedef struct __attribute__((packed)) st_TMZ_MZ_BASIC_DATA {
        uint8_t machine;            /**< cilovy model pocitace (en_TMZ_MACHINE) */
        uint8_t pulseset;           /**< pulzni sada (en_TMZ_PULSESET) */
        uint16_t pause_ms;          /**< pauza po bloku v milisekundach */
        st_MZF_HEADER mzf_header;   /**< MZF hlavicka (ftype=0x03/0x04, fsize/fstrt/fexec=0) */
        uint16_t chunk_count;       /**< pocet chunku */
        /* nasleduje chunk_count * 258 bajtu (kazdy chunk: 2B ID LE + 256B data) */
    } st_TMZ_MZ_BASIC_DATA;

/** @brief Velikost jednoho BSD/BRD chunku v bajtech (2B ID + 256B data) */
#define TMZ_BASIC_CHUNK_SIZE         258

/** @brief Velikost datove casti jednoho BSD/BRD chunku v bajtech */
#define TMZ_BASIC_CHUNK_DATA_SIZE    256

/** @brief Chunk ID posledniho chunku v BSD/BRD zaznamu */
#define TMZ_BASIC_LAST_CHUNK_ID      0xFFFF

/** @} */


/** @defgroup tmz_turbo_flags Priznaky turbo bloku
 *  Bitove priznaky pro pole flags ve strukture st_TMZ_MZ_TURBO_DATA.
 *  @{ */

/** @brief Blok obsahuje kopii hlavicky (header je nahran 2x) */
#define TMZ_TURBO_FLAG_HEADER_COPY   0x01

/** @brief Blok obsahuje kopii datoveho tela (body je nahrano 2x) */
#define TMZ_TURBO_FLAG_BODY_COPY     0x02

/** @} */


/** @defgroup tmz_block_parse Parsovani TMZ bloku
 *  Funkce pro parsovani specifickych TMZ bloku z raw binarniho obsahu
 *  struktury st_TZX_BLOCK. Kazda funkce validuje velikost a typ bloku,
 *  vraci ukazatel na novou strukturu a volitelne ukazatel na datovou
 *  oblast za pevnou casti.
 *  @{ */

    /**
     * @brief Rozparsuje blok 0x40 (MZ Standard Data) z raw dat (zero-copy).
     *
     * Vraci ukazatel primo do dat bloku - nealokuje novou pamet.
     * Provede validaci velikosti bloku a konverzi endianity.
     * Pokud blok obsahuje datove telo, nastavi body_data na jeho zacatek.
     *
     * @param block     Vstupni raw TMZ blok (nesmi byt NULL)
     * @param body_data Vystupni ukazatel na datove telo za hlavickou (muze byt NULL)
     * @param err       Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel do block->data, nebo NULL pri chybe.
     *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
     *         velikost dat (body_size) větší než data bloku - validuje se před
     *         konverzí endianity, poškozený blok se tedy nemodifikuje.
     *
     * @note Platnost vraceneho ukazatele je vazana na zivotnost bloku.
     * @warning Funkce modifikuje data bloku (konverze endianity) - lze volat jen jednou.
     */
    extern st_TMZ_MZ_STANDARD_DATA* tmz_block_parse_mz_standard ( const st_TZX_BLOCK *block, uint8_t **body_data, en_TZX_ERROR *err );

    /**
     * @brief Rozparsuje blok 0x41 (MZ Turbo Data) z raw dat (zero-copy).
     *
     * Vraci ukazatel primo do dat bloku - nealokuje novou pamet.
     * Provede validaci velikosti a konverzi endianity.
     *
     * @param block     Vstupni raw TMZ blok (nesmi byt NULL)
     * @param body_data Vystupni ukazatel na datove telo za hlavickou (muze byt NULL)
     * @param err       Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel do block->data, nebo NULL pri chybe.
     *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
     *         velikost dat (body_size) větší než data bloku - validuje se před
     *         konverzí endianity, poškozený blok se tedy nemodifikuje.
     *
     * @note Platnost vraceneho ukazatele je vazana na zivotnost bloku.
     * @warning Funkce modifikuje data bloku (konverze endianity) - lze volat jen jednou.
     */
    extern st_TMZ_MZ_TURBO_DATA* tmz_block_parse_mz_turbo ( const st_TZX_BLOCK *block, uint8_t **body_data, en_TZX_ERROR *err );

    /**
     * @brief Rozparsuje blok 0x42 (MZ Extra Body) z raw dat (zero-copy).
     *
     * Vraci ukazatel primo do dat bloku - nealokuje novou pamet.
     * Provede validaci velikosti a konverzi endianity.
     *
     * @param block     Vstupni raw TMZ blok (nesmi byt NULL)
     * @param body_data Vystupni ukazatel na datovou cast za hlavickou (muze byt NULL)
     * @param err       Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel do block->data, nebo NULL pri chybe.
     *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
     *         velikost dat (body_size) větší než data bloku - validuje se před
     *         konverzí endianity, poškozený blok se tedy nemodifikuje.
     *
     * @note Platnost vraceneho ukazatele je vazana na zivotnost bloku.
     * @warning Funkce modifikuje data bloku (konverze endianity) - lze volat jen jednou.
     */
    extern st_TMZ_MZ_EXTRA_BODY* tmz_block_parse_mz_extra_body ( const st_TZX_BLOCK *block, uint8_t **body_data, en_TZX_ERROR *err );

    /**
     * @brief Rozparsuje blok 0x43 (MZ Machine Info) z raw dat (zero-copy).
     *
     * Vraci ukazatel primo do dat bloku - nealokuje novou pamet.
     * Provede validaci velikosti a konverzi endianity.
     *
     * @param block Vstupni raw TMZ blok (nesmi byt NULL)
     * @param err   Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel do block->data, nebo NULL pri chybe.
     *
     * @note Platnost vraceneho ukazatele je vazana na zivotnost bloku.
     * @warning Funkce modifikuje data bloku (konverze endianity) - lze volat jen jednou.
     */
    extern st_TMZ_MZ_MACHINE_INFO* tmz_block_parse_mz_machine_info ( const st_TZX_BLOCK *block, en_TZX_ERROR *err );

    /**
     * @brief Rozparsuje blok 0x44 (MZ Loader) z raw dat (zero-copy).
     *
     * Vraci ukazatel primo do dat bloku - nealokuje novou pamet.
     * Provede validaci velikosti a konverzi endianity.
     *
     * @param block       Vstupni raw TMZ blok (nesmi byt NULL)
     * @param loader_data Vystupni ukazatel na data loaderu za hlavickou (muze byt NULL)
     * @param err         Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel do block->data, nebo NULL pri chybe.
     *         Chybou (NULL, err = TZX_ERROR_INVALID_BLOCK) je i deklarovaná
     *         velikost dat (loader_size) větší než data bloku - validuje se před
     *         konverzí endianity, poškozený blok se tedy nemodifikuje.
     *
     * @note Platnost vraceneho ukazatele je vazana na zivotnost bloku.
     * @warning Funkce modifikuje data bloku (konverze endianity) - lze volat jen jednou.
     */
    extern st_TMZ_MZ_LOADER* tmz_block_parse_mz_loader ( const st_TZX_BLOCK *block, uint8_t **loader_data, en_TZX_ERROR *err );

    /**
     * @brief Rozparsuje blok 0x45 (MZ BASIC Data) z raw dat (zero-copy).
     *
     * Vraci ukazatel primo do dat bloku - nealokuje novou pamet.
     * Provede validaci velikosti a konverzi endianity.
     *
     * @param block       Vstupni raw TMZ blok (nesmi byt NULL)
     * @param chunks_data Vystupni ukazatel na zacatek chunkovych dat (muze byt NULL)
     * @param err         Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel do block->data, nebo NULL pri chybe.
     *
     * @note Platnost vraceneho ukazatele je vazana na zivotnost bloku.
     * @warning Funkce modifikuje data bloku (konverze endianity) - lze volat jen jednou.
     */
    extern st_TMZ_MZ_BASIC_DATA* tmz_block_parse_mz_basic_data ( const st_TZX_BLOCK *block, uint8_t **chunks_data, en_TZX_ERROR *err );

/** @} */


/** @defgroup tmz_block_create Vytvareni TMZ bloku
 *  Funkce pro vytvareni novych TMZ bloku ze zadanych parametru.
 *  Vraceny blok obsahuje kompletni binarni data vcetne datoveho tela.
 *  @{ */

    /**
     * @brief Vytvori blok 0x40 (MZ Standard Data) z parametru.
     *
     * Alokuje novy st_TZX_BLOCK na heapu. Serializuje hlavicku
     * a prilozenou datovou cast do binarniho formatu.
     *
     * @param machine    Cilovy model pocitace
     * @param pulseset   Pulzni sada
     * @param pause_ms   Pauza po bloku v milisekundach
     * @param mzf_header Ukazatel na MZF hlavicku (nesmi byt NULL)
     * @param body       Ukazatel na datove telo (muze byt NULL pokud body_size == 0)
     * @param body_size  Velikost datoveho tela v bajtech
     * @return Ukazatel na novy blok, nebo NULL pri chybe alokace.
     *         Volajici musi uvolnit pomoci tmz_block_free().
     */
    extern st_TZX_BLOCK* tmz_block_create_mz_standard ( en_TMZ_MACHINE machine, en_TMZ_PULSESET pulseset, uint16_t pause_ms, const st_MZF_HEADER *mzf_header, const uint8_t *body, uint16_t body_size );

    /**
     * @brief Vytvori blok 0x41 (MZ Turbo Data) z parametru.
     *
     * Alokuje novy st_TZX_BLOCK na heapu. Struktura params musi byt
     * vyplnena vcetne mzf_header. Datove telo se pripoji za serializovanou
     * hlavicku.
     *
     * @param params    Ukazatel na parametry turbo bloku (nesmi byt NULL)
     * @param body      Ukazatel na datove telo (muze byt NULL pokud body_size == 0)
     * @param body_size Velikost datoveho tela v bajtech
     * @return Ukazatel na novy blok, nebo NULL pri chybe alokace.
     *         Volajici musi uvolnit pomoci tmz_block_free().
     */
    extern st_TZX_BLOCK* tmz_block_create_mz_turbo ( const st_TMZ_MZ_TURBO_DATA *params, const uint8_t *body, uint16_t body_size );

    /**
     * @brief Vytvori blok 0x42 (MZ Extra Body) z parametru.
     *
     * Alokuje novy st_TZX_BLOCK na heapu. Serializuje hlavicku
     * a prilozenou datovou cast do binarniho formatu.
     *
     * @param format    Formatova varianta zaznamu (en_TMZ_FORMAT)
     * @param speed     Index rychlosti zaznamu (en_CMTSPEED)
     * @param pause_ms  Pauza po bloku v milisekundach
     * @param body      Ukazatel na datove telo (muze byt NULL pokud body_size == 0)
     * @param body_size Velikost datoveho tela v bajtech
     * @return Ukazatel na novy blok, nebo NULL pri chybe alokace.
     *         Volajici musi uvolnit pomoci tmz_block_free().
     */
    extern st_TZX_BLOCK* tmz_block_create_mz_extra_body ( en_TMZ_FORMAT format, en_CMTSPEED speed, uint16_t pause_ms, const uint8_t *body, uint16_t body_size );

    /**
     * @brief Vytvori blok 0x44 (MZ Loader) z parametru.
     *
     * Alokuje novy st_TZX_BLOCK na heapu. Serializuje hlavicku loaderu
     * a prilozenou binarku loaderu do binarniho formatu.
     *
     * @param loader_type Typ loaderu (en_TMZ_LOADER_TYPE)
     * @param speed       Rychlost pro nasledujici body bloky (en_CMTSPEED)
     * @param mzf_header  Ukazatel na MZF hlavicku loaderu (nesmi byt NULL)
     * @param loader_data Ukazatel na data loaderu (muze byt NULL pokud loader_size == 0)
     * @param loader_size Velikost dat loaderu v bajtech
     * @return Ukazatel na novy blok, nebo NULL pri chybe alokace.
     *         Volajici musi uvolnit pomoci tmz_block_free().
     */
    extern st_TZX_BLOCK* tmz_block_create_mz_loader ( en_TMZ_LOADER_TYPE loader_type, en_CMTSPEED speed, const st_MZF_HEADER *mzf_header, const uint8_t *loader_data, uint16_t loader_size );

    /**
     * @brief Vytvori blok 0x43 (MZ Machine Info) z parametru.
     *
     * Alokuje novy st_TZX_BLOCK na heapu. Blok neobsahuje zadna
     * datova tela, pouze pevnou strukturu.
     *
     * @param machine     Cilovy model pocitace
     * @param cpu_clock   Taktovaci frekvence CPU v Hz
     * @param rom_version Verze ROM (0 = nezname)
     * @return Ukazatel na novy blok, nebo NULL pri chybe alokace.
     *         Volajici musi uvolnit pomoci tmz_block_free().
     */
    extern st_TZX_BLOCK* tmz_block_create_mz_machine_info ( en_TMZ_MACHINE machine, uint32_t cpu_clock, uint8_t rom_version );

    /**
     * @brief Vytvori blok 0x45 (MZ BASIC Data) z parametru.
     *
     * Alokuje novy st_TZX_BLOCK na heapu. Serializuje hlavicku
     * a chunky do binarniho formatu. Kazdy chunk ma 258 bajtu
     * (2B ID LE + 256B data).
     *
     * @param machine     Cilovy model pocitace
     * @param pulseset    Pulzni sada
     * @param pause_ms    Pauza po bloku v milisekundach
     * @param mzf_header  Ukazatel na MZF hlavicku (nesmi byt NULL)
     * @param chunks      Ukazatel na pole chunku (chunk_count * 258 B)
     * @param chunk_count Pocet chunku
     * @return Ukazatel na novy blok, nebo NULL pri chybe alokace.
     *         Volajici musi uvolnit pomoci tmz_block_free().
     */
    extern st_TZX_BLOCK* tmz_block_create_mz_basic_data ( en_TMZ_MACHINE machine, en_TMZ_PULSESET pulseset, uint16_t pause_ms, const st_MZF_HEADER *mzf_header, const uint8_t *chunks, uint16_t chunk_count );

/** @} */


/** @defgroup tmz_block_conv Konverze mezi TMZ a MZF
 *  Vyssi uroven - konverze celych MZF souboru na TMZ bloky a zpet.
 *  @{ */

    /**
     * @brief Vytvori TMZ blok 0x40 (MZ Standard Data) z MZF souboru.
     *
     * Prekonvertuje kompletni MZF soubor (hlavicku + telo) na jeden
     * standardni TMZ blok.
     *
     * @param mzf       Ukazatel na MZF soubor (nesmi byt NULL)
     * @param machine   Cilovy model pocitace
     * @param pulseset  Pulzni sada
     * @param pause_ms  Pauza po bloku v milisekundach
     * @return Ukazatel na novy blok, nebo NULL pri chybe.
     *         Volajici musi uvolnit pomoci tmz_block_free().
     */
    extern st_TZX_BLOCK* tmz_block_from_mzf ( const st_MZF *mzf, en_TMZ_MACHINE machine, en_TMZ_PULSESET pulseset, uint16_t pause_ms );

    /**
     * @brief Extrahuje MZF soubor z TMZ bloku.
     *
     * Podporuje bloky 0x40 (Standard Data), 0x41 (Turbo Data) a 0x46
     * (MZ File, TMZ v1.1), ktere obsahuji kompletni MZF hlavicku a datove
     * telo. U 0x46 je chybou (TZX_ERROR_INVALID_BLOCK) telo delsi nez
     * 65535 B a fsize v hlavicce odlisne od delky tela (spec 4.1 to v bloku
     * dovoluje, MZF to popsat neumi). Hlavička z 0x46 má fsize/fstrt/fexec
     * v pořadí hostitele jako u 0x40/0x41; fname.terminator (11h) zůstává
     * u všech bloků podle bloku. Alokuje novou st_MZF strukturu
     * na heapu.
     *
     * @param block Vstupni TMZ blok (nesmi byt NULL)
     * @param err   Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel na novy MZF soubor, nebo NULL pri chybe.
     *         Volajici musi uvolnit pomoci mzf_free().
     */
    extern st_MZF* tmz_block_to_mzf ( const st_TZX_BLOCK *block, en_TZX_ERROR *err );

/** @} */


/** @defgroup tmz_block_lifecycle Sprava zivotniho cyklu TMZ bloku
 *  @{ */

    /**
     * @brief Uvolni TMZ blok vcetne jeho binarniho obsahu.
     *
     * Bezpecne volani s NULL (no-op).
     *
     * @param block Ukazatel na blok k uvolneni (muze byt NULL)
     */
    extern void tmz_block_free ( st_TZX_BLOCK *block );

/** @} */


#ifdef __cplusplus
}
#endif

#endif /* TMZ_BLOCKS_H */
