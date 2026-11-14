/**
 * @file   tmz_v1conv.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.1
 * @brief  Převod bloků TMZ v1.0 (0x40, 0x41, 0x45) na blok 0x46 (TMZ v1.1).
 *
 * Mapování podle tmz-v1.1-specification.md kap. 9.2:
 * - 0x40 s pulzní sadou MZ-800 -> ROM-800-NOMINAL (záměrná změna výstupu),
 *   s jinou sadou -> TAPEMZ-V1-LEGACY normal se sadou z bloku;
 * - 0x41 NORMAL 1:1 -> LEGACY normal s formátem záznamu playeru (SANE,
 *   MZ80B), NORMAL jiné rychlosti se nepřevádí,
 *   TURBO -> TC-1.21 s předzavaděčem (záměrná změna výstupu),
 *   FASTIPL 1:1, 2:1, 7:3, 8:3 -> IC-10.2-FASTIPL (záměrná změna výstupu,
 *   jiné rychlosti a vlastní pulzy se nepřevádějí),
 *   CPM_TAPE -> PEZIK-2.0A vždy (úroveň 2 = bd 3200, jinak 2400; záměrná
 *   změna výstupu), tělo nad 16 KB = TMZ_V1CONV_ERROR,
 * - 0x45 -> BASIC-1Z016 vždy (1 až 254 kusů, záměrná změna výstupu;
 *   bez kusů nebo s víc než 254 kusy = TMZ_V1CONV_ERROR);
 * - pauza po bloku -> PAUSE poslední části, stroj -> PROFILE_ARGS.
 *
 * Ostatní případy se nepřevádějí (výsledek TMZ_V1CONV_NOT_CONVERTIBLE)
 * a player je hraje cestou v1.0. MZF hlavička jde do bloku 0x46 tak, jak
 * ji vrátí parser bloku v1.0 (tmz_block_parse_*), včetně bajtu 11h
 * (fname.terminator) beze změny.
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

#ifndef TMZ_V1CONV_H
#define TMZ_V1CONV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "../tzx/tzx.h"
#include "../mztape/mztape.h"
#include "../cmtspeed/cmtspeed.h"


    /**
     * @brief Výsledek převodu jednoho bloku.
     */
    typedef enum en_TMZ_V1CONV_RESULT {
        TMZ_V1CONV_OK = 0,              /**< *out = nový blok 0x46 */
        TMZ_V1CONV_NOT_CONVERTIBLE,     /**< blok se nepřevádí (spec 9.2), hraje se cestou v1.0 */
        TMZ_V1CONV_ERROR,               /**< poškozený blok, NULL vstup nebo selhání sestavení */
    } en_TMZ_V1CONV_RESULT;


    /**
     * @brief Nastavení playeru v1.0, na kterých závisel jeho výstup.
     *
     * @invariant formatset < MZTAPE_FORMATSET_COUNT; MZTAPE_FORMATSET_MZ800
     *            (kopie) znamená, že se 0x41 NORMAL nepřevádí
     * @invariant default_speed je platná en_CMTSPEED (cmtspeed_is_valid);
     *            rychlost, kterou cílový profil nepřijme, vede na
     *            TMZ_V1CONV_NOT_CONVERTIBLE
     */
    typedef struct st_TMZ_V1CONV_CONFIG {
        en_MZTAPE_FORMATSET formatset;  /**< formát záznamu pro 0x41 NORMAL (player default_formatset) */
        en_CMTSPEED default_speed;      /**< rychlost pro neplatné pole speed bloku 0x41 (player default_speed) */
    } st_TMZ_V1CONV_CONFIG;


    /**
     * @brief Souhrn převodu souboru.
     */
    typedef struct st_TMZ_V1CONV_REPORT {
        uint32_t converted;   /**< bloky převedené na 0x46 */
        uint32_t kept;        /**< bloky v1.0 ponechané (nepřevoditelné, 0x42, 0x44) */
        uint32_t errors;      /**< bloky v1.0, které převést nešly kvůli chybě */
    } st_TMZ_V1CONV_REPORT;


    /**
     * @brief Výchozí nastavení: formát MZTAPE_FORMATSET_MZ800_SANE, rychlost 1:1
     *        (jako tmz_player_config_init).
     * @param cfg Nastavení (nesmí být NULL).
     */
    extern void tmz_v1conv_config_init ( st_TMZ_V1CONV_CONFIG *cfg );


    /**
     * @brief Převede blok v1.0 na blok 0x46 (spec 9.2).
     *
     * Vstupní blok se nemění (parsuje se kopie dat). Blok jiného ID než
     * 0x40, 0x41 a 0x45 vrátí TMZ_V1CONV_NOT_CONVERTIBLE.
     *
     * Vedlejší efekt: poškozený blok ohlásí parser přes error callback
     * knihovny tmz_blocks (player pak stejný blok parsuje znovu cestou
     * v1.0, hláška se tedy vypíše dvakrát).
     *
     * @param block Blok v1.0.
     * @param cfg Nastavení (NULL = výchozí).
     * @param[out] out Nový blok při TMZ_V1CONV_OK (uvolnit tmz_block_free()), jinak NULL.
     * @return Výsledek převodu; TMZ_V1CONV_ERROR i při NULL block nebo out.
     */
    extern en_TMZ_V1CONV_RESULT tmz_v1conv_block ( const st_TZX_BLOCK *block, const st_TMZ_V1CONV_CONFIG *cfg,
                                                   st_TZX_BLOCK **out );


    /**
     * @brief Převede v souboru všechny převoditelné bloky na 0x46 (na místě).
     *
     * Data nahrazeného bloku uvolní free() a nový blok má data z malloc()
     * (tmz_mzfile_create_block). Předpokládá výchozí alokátory knihoven
     * tzx a tmz_blocks (malloc/free); s vlastním alokátorem tzx
     * (tzx_set_allocator) se funkce nesmí použít. Hlavičku souboru
     * (signatura, verze) nemění. Bloky 0x42 a 0x44 se započtou jako
     * ponechané.
     *
     * @param file Soubor.
     * @param cfg Nastavení (NULL = výchozí).
     * @param[out] rep Souhrn (smí být NULL).
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE jen při NULL file.
     */
    extern int tmz_v1conv_file ( st_TZX_FILE *file, const st_TMZ_V1CONV_CONFIG *cfg, st_TMZ_V1CONV_REPORT *rep );


    /**
     * @brief Text výsledku převodu (anglicky).
     * @param r Výsledek.
     * @return Statický řetězec.
     */
    extern const char* tmz_v1conv_result_string ( en_TMZ_V1CONV_RESULT r );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_V1CONV_H */
