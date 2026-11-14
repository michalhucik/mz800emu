/**
 * @file   mzf_tools.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.2.1
 * @brief  Pomocné funkce pro práci s MZF hlavičkou.
 *
 * Konverze jmen souborů mezi Sharp MZ ASCII a standardním ASCII,
 * tovární funkce pro vytvoření hlavičky, debug výpis.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
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


#ifndef MZF_TOOLS_H
#define MZF_TOOLS_H

#include <stdio.h>
#include <stddef.h>
#include "mzf.h"

#ifdef __cplusplus
extern "C" {
#endif


    /**
     * @brief Režim kódování jména souboru z MZF hlavičky.
     *
     * Určuje, jakým způsobem se konvertuje jméno souboru ze Sharp MZ
     * znakové sady do cílového kódování.
     */
    typedef enum en_MZF_NAME_ENCODING {
        MZF_NAME_ASCII_EU = 0,   /**< Sharp MZ-EU -> ASCII (výchozí, jednobajtová konverze z evropské znakové sady) */
        MZF_NAME_ASCII_JP,    /**< Sharp MZ-JP -> ASCII (jednobajtová konverze z japonské znakové sady) */
        MZF_NAME_UTF8_EU,     /**< Sharp MZ-EU -> UTF-8, evropská varianta znakové sady */
        MZF_NAME_UTF8_JP,     /**< Sharp MZ-JP -> UTF-8, japonská varianta znakové sady */
    } en_MZF_NAME_ENCODING;


    /** @brief Maximální velikost bufferu pro UTF-8 jméno (17 znaků * max 4 bajty + 1). */
    #define MZF_FNAME_UTF8_BUF_SIZE  ( ( MZF_FNAME_FULL_LENGTH * 4 ) + 1 )


    /**
     * @brief Nastaví jméno souboru v hlavičce z ASCII řetězce.
     *
     * Konvertuje ASCII na Sharp MZ ASCII, ořeže na 16 znaků,
     * zbytek vyplní terminátorem 0x0d.
     *
     * @param mzfhdr         Ukazatel na hlavičku k úpravě
     * @param ascii_filename ASCII řetězec se jménem souboru
     */
    extern void mzf_tools_set_fname ( st_MZF_HEADER *mzfhdr, const char *ascii_filename );

    /**
     * @brief Vrátí délku jména souboru (počet znaků před prvním terminátorem 0x0d).
     *
     * Pokud terminátor chybí, vrátí MZF_FILE_NAME_LENGTH (16).
     *
     * @param mzfhdr Ukazatel na hlavičku
     * @return Délka jména v rozsahu 0..16
     */
    extern uint8_t mzf_tools_get_fname_length ( const st_MZF_HEADER *mzfhdr );

    /**
     * @brief Extrahuje jméno souboru z hlavičky do ASCII řetězce.
     *
     * Konvertuje Sharp MZ ASCII-EU na ASCII, přeskakuje netisknutelné znaky (< 0x20).
     * Výstupní buffer musí mít minimálně MZF_FILE_NAME_LENGTH + 1 (17) bajtů.
     *
     * @param mzfhdr         Ukazatel na hlavičku
     * @param ascii_filename Výstupní buffer pro ASCII řetězec (nulou ukončený)
     */
    extern void mzf_tools_get_fname ( const st_MZF_HEADER *mzfhdr, char *ascii_filename );

    /**
     * @brief Extrahuje jméno souboru z hlavičky s volitelným kódováním.
     *
     * Podle zadaného kódování provede konverzi:
     * - MZF_NAME_ASCII_EU: Sharp MZ -> ASCII (shodné s mzf_tools_get_fname)
     * - MZF_NAME_ASCII_JP: Sharp MZ-JP -> ASCII (jednobajtová konverze z japonské znakové sady)
     * - MZF_NAME_UTF8_EU: Sharp MZ -> UTF-8, evropská znaková sada
     * - MZF_NAME_UTF8_JP: Sharp MZ -> UTF-8, japonská znaková sada
     *
     * @param mzfhdr   Ukazatel na hlavičku. Nesmí být NULL.
     * @param filename Výstupní buffer pro nulou ukončený řetězec.
     * @param buf_size Velikost výstupního bufferu v bajtech.
     *                 Pro ASCII stačí MZF_FILE_NAME_LENGTH + 1 (17).
     *                 Pro UTF-8 doporučeno MZF_FNAME_UTF8_BUF_SIZE (69).
     * @param encoding Požadované kódování výstupu.
     */
    extern void mzf_tools_get_fname_ex ( const st_MZF_HEADER *mzfhdr, char *filename,
                                          size_t buf_size, en_MZF_NAME_ENCODING encoding );

    /**
     * @brief Vytvoří novou MZF hlavičku alokovanou na heapu.
     *
     * Volající musí uvolnit přes free(). Pokud cmnt == NULL, komentář
     * se vynuluje. Jméno se vyplní terminátory a pak překopíruje zadané znaky.
     *
     * @param ftype        Typ souboru (viz MZF_FTYPE_*)
     * @param fsize        Velikost datové části
     * @param fstrt        Startovací adresa v paměti Z80
     * @param fexec        Adresa spuštění v paměti Z80
     * @param fname        Surové bajty jména (Sharp MZ ASCII)
     * @param fname_length Délka fname (max. 16, delší se ořízne)
     * @param cmnt         Komentář (104 bajtů), nebo NULL pro vynulování
     * @return Ukazatel na st_MZF_HEADER, nebo NULL při selhání alokace
     */
    extern st_MZF_HEADER* mzf_tools_create_mzfhdr ( uint8_t ftype, uint16_t fsize, uint16_t fstrt, uint16_t fexec, const uint8_t *fname, unsigned fname_length, const uint8_t *cmnt );

    /**
     * @brief Znaková sada pro textový sloupec hex dumpu.
     *
     * Určuje, jak se bajty dat interpretují ve sloupci znaků hex dumpu
     * (viz mzf_tools_hex_dump() a mzf_tools_dump_char()). Režim RAW zobrazuje
     * standardní ASCII 0x20-0x7E, ostatní režimy konvertují ze Sharp MZ
     * znakové sady (EU/JP) do ASCII nebo UTF-8 přes knihovnu sharpmz_ascii.
     * Bajty, které nelze zobrazit (nekonvertovatelné nebo netisknutelné),
     * se nahradí zástupným znakem '.'.
     */
    typedef enum en_MZF_DUMP_CHARSET {
        MZF_DUMP_RAW = 0,     /**< Standardní ASCII 0x20-0x7E, ostatní '.' (výchozí pro CLI) */
        MZF_DUMP_ASCII_EU,    /**< Sharp MZ-EU -> ASCII (odpovídá MZF_NAME_ASCII_EU) */
        MZF_DUMP_ASCII_JP,    /**< Sharp MZ-JP -> ASCII (odpovídá MZF_NAME_ASCII_JP) */
        MZF_DUMP_UTF8_EU,     /**< Sharp MZ-EU -> UTF-8 (odpovídá MZF_NAME_UTF8_EU) */
        MZF_DUMP_UTF8_JP,     /**< Sharp MZ-JP -> UTF-8 (odpovídá MZF_NAME_UTF8_JP) */
    } en_MZF_DUMP_CHARSET;


    /** @brief Zástupný znak pro nezobrazitelný bajt v hex dumpu. */
    #define MZF_DUMP_PLACEHOLDER  "."

    /**
     * @brief Převede název režimu z příkazové řádky na en_MZF_DUMP_CHARSET.
     *
     * Rozpoznává řetězce "raw", "eu", "jp", "utf8-eu", "utf8-jp"
     * (shodné názvy jako volba --charset, navíc "raw").
     *
     * @param name    Název režimu (nesmí být NULL).
     * @param charset Výstup - rozpoznaný režim (nesmí být NULL); při neúspěchu se nemění.
     * @return 1 pokud byl název rozpoznán, 0 pokud ne.
     */
    extern int mzf_tools_parse_dump_charset ( const char *name, en_MZF_DUMP_CHARSET *charset );

    /**
     * @brief Vrátí zobrazitelnou reprezentaci jednoho bajtu pro textový sloupec dumpu.
     *
     * - MZF_DUMP_RAW: bajt 0x20-0x7E jako ASCII znak, jinak '.'.
     * - MZF_DUMP_ASCII_EU/JP: sharpmz_convert_to_ASCII() resp. sharpmz_jp_convert_to_ASCII();
     *   pokud konverze vrátí converted=0 nebo printable=0, výsledek je '.'.
     * - MZF_DUMP_UTF8_EU/JP: sharpmz_eu_convert_to_UTF8() resp. sharpmz_jp_convert_to_UTF8()
     *   se stejným pravidlem pro '.'.
     *
     * @param c       Vstupní bajt.
     * @param charset Režim interpretace.
     * @return Ukazatel na nulou ukončený řetězec (1 ASCII znak nebo 1 UTF-8 znak),
     *         platný do dalšího volání této funkce (statický buffer, není reentrantní).
     */
    extern const char* mzf_tools_dump_char ( uint8_t c, en_MZF_DUMP_CHARSET charset );

    /**
     * @brief Vypíše hex dump dat ve formátu "offset | hex (2x8) | znaky".
     *
     * Každý řádek: 4-místný hex offset (base_addr + pozice), 16 bajtů hex
     * s mezerou po 8. bajtu a textový sloupec v ohraničení '|', kde se
     * každý bajt vykreslí přes mzf_tools_dump_char(). Řádky jsou odsazeny
     * řetězcem @p indent (může být NULL = bez odsazení). NULL-safe: při
     * fp == NULL nebo data == NULL (se size > 0) nedělá nic.
     *
     * @param fp        Výstupní stream.
     * @param data      Data k výpisu.
     * @param size      Počet bajtů.
     * @param base_addr Hodnota offsetu prvního bajtu v levém sloupci.
     * @param charset   Znaková sada textového sloupce.
     * @param indent    Prefix každého řádku (např. "  "), nebo NULL.
     */
    extern void mzf_tools_hex_dump ( FILE *fp, const uint8_t *data, size_t size,
                                     uint32_t base_addr, en_MZF_DUMP_CHARSET charset,
                                     const char *indent );

    /**
     * @brief Vypíše obsah MZF hlavičky na zadaný FILE* (pro debug účely).
     *
     * Formát: typ, jméno (s délkou), velikost (hex+dec), adresy (hex),
     * komentář (prvních 16 bajtů hex dump). NULL-safe — oba parametry
     * mohou být NULL (no-op).
     *
     * @param mzfhdr Ukazatel na hlavičku (může být NULL)
     * @param fp     Výstupní FILE* stream (může být NULL)
     */
    extern void mzf_tools_dump_header ( const st_MZF_HEADER *mzfhdr, FILE *fp );

#ifdef __cplusplus
}
#endif

#endif /* MZF_TOOLS_H */
