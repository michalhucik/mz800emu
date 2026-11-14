/**
 * @file   mzf.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.1
 * @brief  Veřejné API knihovny pro MZF souborový formát počítačů Sharp MZ.
 *
 * Knihovna pro práci s MZF souborovým formátem počítačů Sharp MZ
 * (MZ-700, MZ-800, MZ-1500). MZF soubor se skládá ze 128B hlavičky
 * (typ, jméno, velikost, startovací/spouštěcí adresa, komentář)
 * a volitelného binárního těla (programová/datová oblast).
 *
 * Všechna vícebajtová pole v hlavičce jsou uložena v little-endian
 * pořadí (odpovídá Z80 procesoru). Konverze na hostitelský byte-order
 * probíhá automaticky při čtení/zápisu.
 *
 * Jména souborů v hlavičce používají znakovou sadu Sharp MZ ASCII,
 * která se liší od standardního ASCII. Konverze zajišťuje modul
 * mzf_tools (viz mzf_tools.h).
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-11-09: popisy typů souborů 02h-05h podle dokumentace BASICu 1Z-016 (2.1.1)
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


#ifndef MZF_H
#define MZF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "../generic_driver/generic_driver.h"


/** @defgroup mzf_constants Konstanty formátu
 *  @{ */

/** @brief Délka jména souboru (bez terminátoru) */
#define MZF_FILE_NAME_LENGTH    16

/** @brief Délka komentáře v hlavičce */
#define MZF_CMNT_LENGTH         104

/** @brief Terminátorový bajt jména souboru (CR) */
#define MZF_FNAME_TERMINATOR    0x0d

/** @brief Velikost celé MZF hlavičky v bajtech */
#define MZF_HEADER_SIZE         128

/** @brief Maximální velikost těla MZF souboru (omezeno 16-bit polem fsize) */
#define MZF_MAX_BODY_SIZE       0xFFFF

/** @} */


/** @defgroup mzf_ftype Kódy typů souborů (pole ftype v hlavičce)
 *  Hodnoty dle dokumentace Sharp MZ-800/700/1500. Některé typy jsou
 *  specifické pro konkrétní architekturu.
 *  @{ */

/** @brief Strojový kód (object file) */
#define MZF_FTYPE_OBJ           0x01
/** @brief Program BASICu uložený `SAVE` bez volby A (binární forma) */
#define MZF_FTYPE_BTX           0x02
/** @brief BSD - jméno podle tabulky typů ROM; podle dokumentace BASICu 1Z-016
 *  sekvenční data nebo program BASICu v ASCII (`SAVE ...,A`, kód 03h
 *  neověřen); používá ho i boot disku IPLPRO */
#define MZF_FTYPE_BSD           0x03
/** @brief BRD - datový soubor BASICu 1Z-016 z `WOPEN#`/`PRINT#` (dokumentace
 *  BASICu ho nazývá "BSD file") */
#define MZF_FTYPE_BRD           0x04
/** @brief RB - program BASICu ve sbalené formě */
#define MZF_FTYPE_RB            0x05

/** @} */


/** @brief Chybové kódy MZF operací */
typedef enum en_MZF_ERROR {
    MZF_OK = 0,                     /**< operace úspěšná */
    MZF_ERROR_IO,                   /**< chyba čtení/zápisu přes generic_driver */
    MZF_ERROR_INVALID_HEADER,       /**< hlavička nesplňuje validaci */
    MZF_ERROR_NO_FNAME_TERMINATOR,  /**< jméno souboru nemá terminátor 0x0d */
    MZF_ERROR_SIZE_MISMATCH,        /**< velikost souboru neodpovídá fsize v hlavičce */
    MZF_ERROR_ALLOC,                /**< selhání alokace paměti */
} en_MZF_ERROR;


/** @defgroup mzf_structs Datové struktury
 *  Struktury jsou packed pro přímé mapování na binární formát.
 *  Layout odpovídá specifikaci — celkem 128 bajtů hlavičky.
 *  @{ */

    /** @brief Jméno souboru v Sharp MZ ASCII — 16 znaků + terminátor 0x0d */
    typedef struct __attribute__((packed)) st_MZF_FILENAME {
        uint8_t name[MZF_FILE_NAME_LENGTH]; /**< znaky jména souboru v Sharp MZ ASCII */
        uint8_t terminator;                 /**< bajt 11h: obvykle 0x0d, ROM SAVE zapíše bajt z paměti (např. 0x00) */
    } st_MZF_FILENAME;

/** @brief Plná délka struktury jména (16 znaků + 1 terminátor) */
#define MZF_FNAME_FULL_LENGTH sizeof ( st_MZF_FILENAME )


    /** @brief MZF hlavička — 128 bajtů, všechna uint16 pole jsou little-endian */
    typedef struct __attribute__((packed)) st_MZF_HEADER {
        uint8_t ftype;                      /**< typ souboru (viz MZF_FTYPE_*) */
        st_MZF_FILENAME fname;              /**< jméno souboru (Sharp MZ ASCII) */
        uint16_t fsize;                     /**< velikost datové části v bajtech */
        uint16_t fstrt;                     /**< startovací adresa v paměti Z80 */
        uint16_t fexec;                     /**< adresa spuštění v paměti Z80 */
        uint8_t cmnt[MZF_CMNT_LENGTH];      /**< komentář / rezervovaný prostor */
    } st_MZF_HEADER;

    /* Compile-time ověření velikosti hlavičky */
#ifdef __cplusplus
    static_assert ( sizeof ( st_MZF_HEADER ) == MZF_HEADER_SIZE,
                    "st_MZF_HEADER musí mít přesně 128 bajtů" );
#else
    _Static_assert ( sizeof ( st_MZF_HEADER ) == MZF_HEADER_SIZE,
                     "st_MZF_HEADER musí mít přesně 128 bajtů" );
#endif


    /** @brief Kompletní MZF soubor — hlavička + tělo */
    typedef struct st_MZF {
        st_MZF_HEADER header;       /**< MZF hlavička (128 bajtů) */
        uint8_t *body;              /**< ukazatel na datovou část (může být NULL) */
        uint32_t body_size;         /**< skutečná velikost alokovaného body bufferu */
    } st_MZF;

/** @} */


/** @defgroup mzf_lowlevel_io Nízkoúrovňové I/O API (přes generic_driver)
 *  Tyto funkce pracují přímo s handlerem a jsou vhodné pro situace,
 *  kdy volající sám řídí otevření/zavření datového zdroje.
 *  @{ */

    /**
     * @brief Konverze endianity uint16 polí hlavičky (LE <-> host byte order).
     *
     * Funkce je symetrická - volání dvakrát vrátí původní hodnoty.
     * Nastavuje také fname.terminator na 0x0D.
     *
     * @param mzfhdr Ukazatel na hlavičku k úpravě
     * @see mzf_header_swap_endianity() - totéž bez změny fname.terminator
     */
    extern void mzf_header_items_correction ( st_MZF_HEADER *mzfhdr );

    /**
     * @brief Konverze endianity fsize, fstrt a fexec (LE <-> host byte order) bez dalších změn.
     *
     * Na rozdíl od mzf_header_items_correction() nechá fname.terminator (11h)
     * i všechny ostatní bajty hlavičky beze změny. Pro cesty, které mají přenést
     * hlavičku tak, jak je na pásce nebo v souboru (ROM SAVE zapíše 11h tak, jak
     * je v paměti, např. 00h).
     *
     * Funkce je symetrická - volání dvakrát vrátí původní hodnoty.
     *
     * @param mzfhdr Ukazatel na hlavičku k úpravě; NULL se ignoruje.
     */
    extern void mzf_header_swap_endianity ( st_MZF_HEADER *mzfhdr );

    /**
     * @brief Načte MZF hlavičku z handleru na daném byte offsetu.
     *
     * Automaticky provede konverzi endianity přes mzf_header_items_correction().
     * Vedlejší efekt: bajt 11h (fname.terminator) nastaví na 0Dh (platí i pro
     * mzf_read_header() a mzf_load()); kdo potřebuje bajt ze souboru, přečte
     * ho zvlášť.
     *
     * @param h       Otevřený handler (soubor nebo paměť)
     * @param offset  Byte offset v datovém zdroji
     * @param mzfhdr  Výstupní buffer pro hlavičku
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_read_header_on_offset ( st_HANDLER *h, uint32_t offset, st_MZF_HEADER *mzfhdr );

    /**
     * @brief Načte MZF hlavičku od začátku handleru (offset 0).
     *
     * @param h       Otevřený handler
     * @param mzfhdr  Výstupní buffer pro hlavičku
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_read_header ( st_HANDLER *h, st_MZF_HEADER *mzfhdr );

    /**
     * @brief Zapíše MZF hlavičku do handleru na daný byte offset.
     *
     * Pole fsize, fstrt, fexec se automaticky konvertují do little-endian.
     * Vstupní hlavička se nemodifikuje (const).
     *
     * @param h       Otevřený handler pro zápis
     * @param offset  Byte offset v datovém zdroji
     * @param mzfhdr  Hlavička k zápisu
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_write_header_on_offset ( st_HANDLER *h, uint32_t offset, const st_MZF_HEADER *mzfhdr );

    /**
     * @brief Zapíše MZF hlavičku od začátku handleru (offset 0).
     *
     * @param h       Otevřený handler pro zápis
     * @param mzfhdr  Hlavička k zápisu
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_write_header ( st_HANDLER *h, const st_MZF_HEADER *mzfhdr );

    /**
     * @brief Načte tělo MZF souboru z handleru na daném byte offsetu.
     *
     * @param h           Otevřený handler
     * @param offset      Byte offset v datovém zdroji
     * @param buffer      Cílový buffer pro data
     * @param buffer_size Počet bajtů ke čtení
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_read_body_on_offset ( st_HANDLER *h, uint32_t offset, uint8_t *buffer, uint32_t buffer_size );

    /**
     * @brief Načte tělo MZF souboru od konce hlavičky (offset MZF_HEADER_SIZE).
     *
     * @param h           Otevřený handler
     * @param buffer      Cílový buffer pro data
     * @param buffer_size Počet bajtů ke čtení (typicky mzfhdr.fsize)
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_read_body ( st_HANDLER *h, uint8_t *buffer, uint32_t buffer_size );

    /**
     * @brief Zapíše tělo MZF souboru do handleru na daný byte offset.
     *
     * @param h           Otevřený handler pro zápis
     * @param offset      Byte offset v datovém zdroji
     * @param buffer      Zdrojová data k zápisu
     * @param buffer_size Počet bajtů k zápisu
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_write_body_on_offset ( st_HANDLER *h, uint32_t offset, const uint8_t *buffer, uint32_t buffer_size );

    /**
     * @brief Zapíše tělo MZF souboru za hlavičku (offset MZF_HEADER_SIZE).
     *
     * @param h           Otevřený handler pro zápis
     * @param buffer      Zdrojová data k zápisu
     * @param buffer_size Počet bajtů k zápisu
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int mzf_write_body ( st_HANDLER *h, const uint8_t *buffer, uint32_t buffer_size );

    /**
     * @brief Vrátí textový popis chyby z generic_driver.
     *
     * Deleguje na generic_driver_error_message().
     *
     * @param h Handler s chybovým stavem
     * @param d Driver s chybovým stavem
     * @return Textový popis poslední chyby
     */
    extern const char* mzf_error_message ( st_HANDLER *h, st_DRIVER *d );

    /**
     * @brief Ověří přítomnost terminátoru 0x0d v poli jména na daném offsetu.
     *
     * Čte pole fname přímo z handleru a hledá bajt 0x0D.
     *
     * @param h      Otevřený handler
     * @param offset Byte offset pole fname v datovém zdroji
     * @return EXIT_SUCCESS pokud terminátor nalezen, EXIT_FAILURE pokud ne
     */
    extern int mzf_header_test_fname_terminator_on_offset ( st_HANDLER *h, uint32_t offset );

    /**
     * @brief Ověří přítomnost terminátoru 0x0d v poli jména od výchozího offsetu (1).
     *
     * @param h Otevřený handler
     * @return EXIT_SUCCESS pokud terminátor nalezen, EXIT_FAILURE pokud ne
     */
    extern int mzf_header_test_fname_terminator ( st_HANDLER *h );

/** @} */


/** @defgroup mzf_validation Validační API
 *  Ověření platnosti MZF dat — kontrola terminátoru, rozumné fsize atp.
 *  @{ */

    /**
     * @brief Validace hlavičky v paměti.
     *
     * Kontroluje NULL vstup a přítomnost terminátoru 0x0D v poli fname.
     *
     * @param mzfhdr Ukazatel na hlavičku k validaci
     * @return MZF_OK při úspěchu, jinak konkrétní chybový kód
     */
    extern en_MZF_ERROR mzf_header_validate ( const st_MZF_HEADER *mzfhdr );

    /**
     * @brief Validace celého MZF souboru přes handler.
     *
     * Načte hlavičku, provede validaci a u paměťového handleru kontroluje
     * shodu fsize s reálnou velikostí dat.
     *
     * @param h Otevřený handler s MZF daty
     * @return MZF_OK při úspěchu, jinak konkrétní chybový kód
     */
    extern en_MZF_ERROR mzf_file_validate ( st_HANDLER *h );

    /**
     * @brief Vrátí textový popis chybového kódu en_MZF_ERROR.
     *
     * Vždy vrací platný řetězec (nikdy NULL), i pro neznámé kódy.
     *
     * @param err Chybový kód
     * @return Textový popis chyby (česky)
     */
    extern const char* mzf_error_string ( en_MZF_ERROR err );

/** @} */


/** @defgroup mzf_lifecycle Lifecycle API (vyšší úroveň)
 *  Kompletní správa MZF souboru — načtení, uložení, uvolnění.
 *  Pracuje se strukturou st_MZF, která zapouzdřuje hlavičku i tělo.
 *  @{ */

    /**
     * @brief Načte celý MZF soubor (hlavičku + tělo) z handleru.
     *
     * Alokuje a vrací novou st_MZF strukturu na heapu.
     * Volající musí uvolnit přes mzf_free(). Pokud fsize == 0,
     * body bude NULL a body_size bude 0.
     *
     * @param h   Otevřený handler s MZF daty
     * @param err Výstupní chybový kód (může být NULL)
     * @return Ukazatel na st_MZF, nebo NULL při chybě
     */
    extern st_MZF* mzf_load ( st_HANDLER *h, en_MZF_ERROR *err );

    /**
     * @brief Zapíše celý MZF (hlavičku + tělo) do handleru.
     *
     * @param h   Otevřený handler pro zápis
     * @param mzf MZF data k uložení (nesmí být NULL)
     * @return MZF_OK při úspěchu, jinak chybový kód
     */
    extern en_MZF_ERROR mzf_save ( st_HANDLER *h, const st_MZF *mzf );

    /**
     * @brief Uvolní st_MZF strukturu včetně body bufferu.
     *
     * Bezpečné volat s NULL (no-op).
     *
     * @param mzf Ukazatel na strukturu k uvolnění (může být NULL)
     */
    extern void mzf_free ( st_MZF *mzf );

/** @} */


/** @brief Přetypování proměnné na uint8_t* — zpětně kompatibilní makro pro starší kód */
#define MZF_UINT8_FNAME(n) ((uint8_t*)&(n))

    /** @brief Verze knihovny mzf. */
#define MZF_VERSION "2.1.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzf.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* mzf_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZF_H */
