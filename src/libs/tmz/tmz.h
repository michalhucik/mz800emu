/**
 * @file   tmz.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.14.0
 * @brief  Verejne API knihovny pro format TMZ - MZ-specificka nadstavba nad TZX.
 *
 * TMZ (TapeMZ) je rozsireni TZX formatu o bloky specificke
 * pro pocitace Sharp MZ (MZ-700, MZ-800, MZ-1500, MZ-80B).
 * Bloky 0x40-0x4F jsou TMZ Extension bloky.
 *
 * Zakladni souborove I/O (hlavicka, bloky, load/save/free)
 * poskytuje knihovna tzx (tzx.h). Tato knihovna doplnuje:
 * - TMZ konstanty (signatura "TapeMZ!", verze)
 * - TMZ Extension blokova ID (0x40-0x4F)
 * - MZ-specificke vycty (machine, pulseset, format, loader)
 * - tmz_header_init(), tmz_header_is_tmz()
 * - tmz_block_id_name() - nazvy bloku vcetne MZ-specifickych
 * - tmz_block_is_mz_extension()
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


#ifndef TMZ_H
#define TMZ_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../tzx/tzx.h"
#include "../mzf/mzf.h"
#include "../cmtspeed/cmtspeed.h"


/** @defgroup tmz_constants Konstanty TMZ formatu
 *  @{ */

/** @brief Signatura TMZ souboru ("TapeMZ!") */
#define TMZ_SIGNATURE           "TapeMZ!"

/** @brief Hlavni cislo verze formatu TMZ */
#define TMZ_VERSION_MAJOR       1

/** @brief Vedlejší číslo verze formátu TMZ (1.1 = blok 0x46, tmz-v1.1-specification.md kap. 2) */
#define TMZ_VERSION_MINOR       1

/** @} */


/** @defgroup tmz_block_ids TMZ Extension blokova ID (0x40-0x4F)
 *  Bloky specificke pro pocitace Sharp MZ. Nejsou soucasti puvodniho TZX formatu.
 *  @{ */

/** @brief Sharp MZ standardni data (hlavicka + telo, 1200 Bd zaklad) */
#define TMZ_BLOCK_ID_MZ_STANDARD_DATA  0x40
/** @brief Sharp MZ turbo data (zrychleny zaznam) */
#define TMZ_BLOCK_ID_MZ_TURBO_DATA     0x41
/** @brief Sharp MZ extra body (dodatecna datova cast) */
#define TMZ_BLOCK_ID_MZ_EXTRA_BODY     0x42
/** @brief Sharp MZ informace o stroji (model, konfigurace) */
#define TMZ_BLOCK_ID_MZ_MACHINE_INFO   0x43
/** @brief Sharp MZ loader (zavadec pro turbo/fast-IPL) */
#define TMZ_BLOCK_ID_MZ_LOADER         0x44
/** @brief Sharp MZ BASIC datovy zaznam (BSD/BRD chunkovany format) */
#define TMZ_BLOCK_ID_MZ_BASIC_DATA     0x45
/** @brief Blok 0x46 - MZ File: MZF soubor + seznam částí (TMZ v1.1, tmz-v1.1-specification.md kap. 4) */
#define TMZ_BLOCK_ID_MZ_FILE           0x46

/** @brief První rezervované ID bloku MZ; rezervované bloky čtečka přeskočí s varováním (spec v1.1 kap. 3). */
#define TMZ_BLOCK_ID_RESERVED_FIRST    0x47

/** @brief Poslední rezervované ID bloku MZ (viz TMZ_BLOCK_ID_RESERVED_FIRST). */
#define TMZ_BLOCK_ID_RESERVED_LAST     0x4F

/** @} */


/** @defgroup tmz_enums Vycty specifické pro Sharp MZ
 *  @{ */

    /**
     * @brief Cilovy stroj (model pocitace Sharp MZ).
     *
     * Pouziva se v bloku MZ_MACHINE_INFO pro identifikaci
     * modelu pocitace, pro ktery je zaznam urcen.
     */
    typedef enum en_TMZ_MACHINE {
        TMZ_MACHINE_GENERIC = 0, /**< genericke - neni specifikovano */
        TMZ_MACHINE_MZ700,       /**< Sharp MZ-700 (vcetne MZ-80K, MZ-80A) */
        TMZ_MACHINE_MZ800,       /**< Sharp MZ-800 */
        TMZ_MACHINE_MZ1500,      /**< Sharp MZ-1500 */
        TMZ_MACHINE_MZ80B,       /**< Sharp MZ-80B */
        TMZ_MACHINE_COUNT,       /**< pocet platnych hodnot (sentinel) */
    } en_TMZ_MACHINE;


    /**
     * @brief Pulzni sada - urcuje casovani pulzu pro dany model.
     *
     * Kazdy model Sharp MZ pouziva mirne odlisne delky
     * pulzu pro logicke "0" a "1". Pulzni sada urcuje,
     * ktere casovani se pouzije pri generovani CMT streamu.
     */
    typedef enum en_TMZ_PULSESET {
        TMZ_PULSESET_700 = 0, /**< pulzni sada MZ-700 / MZ-80K / MZ-80A */
        TMZ_PULSESET_800,     /**< pulzni sada MZ-800 / MZ-1500 */
        TMZ_PULSESET_80B,     /**< pulzni sada MZ-80B */
        TMZ_PULSESET_COUNT,   /**< pocet platnych hodnot (sentinel) */
    } en_TMZ_PULSESET;


    /**
     * @brief Format zaznamu na pasce.
     *
     * Urcuje zpusob kodovani dat na pasce. Kazdy format pouziva jinou
     * modulaci a/nebo strukturu zaznamu:
     *
     * - NORMAL: standardni FM modulace (1 bit = 1 pulz, long/short)
     * - TURBO: proprietarni rychle kodovani (TurboCopy)
     * - FASTIPL: rychle IPL kodovani s $BB prefixem (Intercopy)
     * - SINCLAIR: ZX Spectrum kompatibilni kodovani
     * - FSK: Frequency Shift Keying - zmena frekvence nosneho signalu
     * - SLOW: 2 bity na pulz, kompaktnejsi nez FM
     * - DIRECT: primy bitovy zapis bez modulace, nejrychlejsi prenos
     */
    typedef enum en_TMZ_FORMAT {
        TMZ_FORMAT_NORMAL = 0, /**< standardni FM format (1200 Bd zaklad) */
        TMZ_FORMAT_TURBO,      /**< turbo format (proprietarni rychle kodovani) */
        TMZ_FORMAT_FASTIPL,    /**< fast-IPL format (MZ-800, $BB prefix kodovani) */
        TMZ_FORMAT_SINCLAIR,   /**< Sinclair/ZX Spectrum format (1400 Bd zaklad) */
        TMZ_FORMAT_FSK,        /**< FSK - Frequency Shift Keying (zmena frekvence) */
        TMZ_FORMAT_SLOW,       /**< SLOW - 2 bity na pulz, kompaktnejsi kodovani */
        TMZ_FORMAT_DIRECT,     /**< DIRECT - primy bitovy zapis, nejrychlejsi */
        TMZ_FORMAT_CPM_TAPE,   /**< CPM-TAPE - Pezik/MarVan format (LSB first, bez stop bitu) */
        TMZ_FORMAT_COUNT,      /**< pocet platnych hodnot (sentinel) */
    } en_TMZ_FORMAT;


    /**
     * @brief Typ loaderu (zavadece) pro turbo a specialni formaty.
     *
     * Urcuje konkretni loader, ktery se pouziva pro nacteni dat
     * z pasky. Ruzne loadery podporuji ruzne rychlosti a formaty.
     */
    typedef enum en_TMZ_LOADER_TYPE {
        TMZ_LOADER_TURBO_1_0 = 0, /**< turbo loader verze 1.0 */
        TMZ_LOADER_TURBO_1_2X,    /**< turbo loader verze 1.2x */
        TMZ_LOADER_FASTIPL_V2,    /**< fast-IPL loader verze 2 */
        TMZ_LOADER_FASTIPL_V7,    /**< fast-IPL loader verze 7 */
        TMZ_LOADER_FSK,           /**< FSK (Frequency Shift Keying) loader */
        TMZ_LOADER_SLOW,          /**< SLOW loader (2 bity na pulz, mzftools) */
        TMZ_LOADER_DIRECT,        /**< DIRECT loader (prima bitova data, vlastni zavadec) */
        TMZ_LOADER_COUNT,         /**< pocet platnych hodnot (sentinel) */
    } en_TMZ_LOADER_TYPE;

/** @} */


/** @defgroup tmz_header_ops Operace s TMZ hlavickou
 *  @{ */

    /**
     * @brief Overi, zda hlavicka obsahuje TMZ signaturu ("TapeMZ!").
     *
     * @param header Ukazatel na hlavicku k overeni.
     * @return true pokud signatura odpovida "TapeMZ!", false jinak.
     *
     * @pre header != NULL
     */
    extern bool tmz_header_is_tmz ( const st_TZX_HEADER *header );

    /**
     * @brief Inicializuje hlavicku vychozimi hodnotami TMZ formatu.
     *
     * Nastavi signaturu na "TapeMZ!", EOF marker na 0x1A
     * a verzi na TMZ_VERSION_MAJOR.TMZ_VERSION_MINOR.
     *
     * @param header Ukazatel na hlavicku k inicializaci.
     *
     * @pre header != NULL
     * @post header obsahuje platnou TMZ hlavicku
     */
    extern void tmz_header_init ( st_TZX_HEADER *header );

/** @} */


/** @defgroup tmz_block_ops Operace s bloky
 *  Pomocne funkce pro praci s identifikatory bloku.
 *  @{ */

    /**
     * @brief Vrati textovy nazev bloku pro dany identifikator.
     *
     * Pokryva jak standardni TZX bloky (deleguje na tzx_block_id_name()),
     * tak TMZ MZ-specificke bloky (0x40-0x4F).
     * Pro nezname ID vraci "Unknown". Vzdy vraci platny retezec (nikdy NULL).
     *
     * @param id Identifikator bloku.
     * @return Ukazatel na staticky retezec s nazvem bloku.
     */
    extern const char* tmz_block_id_name ( uint8_t id );

    /**
     * @brief Overi, zda je dany identifikator TMZ Extension blokem (0x40-0x4F).
     *
     * TMZ Extension bloky jsou specificke pro Sharp MZ a nejsou
     * soucasti puvodniho TZX formatu.
     *
     * @param id Identifikator bloku k overeni.
     * @return true pokud je blok TMZ Extension (0x40-0x4F), false jinak.
     */
    extern bool tmz_block_is_mz_extension ( uint8_t id );

/** @} */


    /** @brief Verze knihovny tmz. */
#define TMZ_VERSION "1.14.0"

    /** @brief Verze formátu TMZ jako řetězec ("1.1"). */
#define TMZ_FORMAT_VERSION "1.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny tmz.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* tmz_version ( void );

    /**
     * @brief Vrátí řetězec s verzí formátu TMZ.
     * @return Statický řetězec s verzí formátu (např. "1.0").
     */
    extern const char* tmz_format_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_H */
