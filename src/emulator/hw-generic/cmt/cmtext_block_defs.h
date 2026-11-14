/* 
 * File:   cmtext_block_defs.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 20. května 2018, 7:52
 * 
 * 
 * ----------------------------- License -------------------------------------
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * ---------------------------------------------------------------------------
 */


/**
 * @file cmtext_block_defs.h
 * @brief Společné výčty bloku pásky Virtual CMT (typ bloku, režim rychlosti).
 */

#ifndef CMTEXT_BLOCK_DEFS_H
#define CMTEXT_BLOCK_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif


    /**
     * @brief Typ bloku pásky z pohledu indexu, UI a MCP.
     *
     * Číselné hodnoty jsou veřejné (MCP resource periph/cmt/tape hlásí typ
     * číslem), nové hodnoty se proto přidávají jen na konec.
     */
    typedef enum en_CMTEXT_BLOCK_TYPE {
        CMTEXT_BLOCK_TYPE_WAV = 0,  /**< zvukový záznam: blok 0x18 z importu WAV (blok 0x18 ze záznamu emulátoru je TZX) */
        CMTEXT_BLOCK_TYPE_MZF,      /**< blok nese MZF (TMZ 0x40, 0x41, 0x46) */
        CMTEXT_BLOCK_TYPE_TAPHEADER,/**< hlavička ZX TAP: blok 0x10/0x11 s 19 B dat a flagem 00h */
        CMTEXT_BLOCK_TYPE_TAPDATA,  /**< ostatní bloky 0x10/0x11 (data ZX TAP) */
        CMTEXT_BLOCK_TYPE_TZX,      /**< jiný blok se signálem (0x12-0x15, 0x18 mimo import WAV, 0x20 s nenulovou pauzou, 0x45) */
        CMTEXT_BLOCK_TYPE_CONTROL,  /**< blok bez signálu podle ID (řídicí, informační, Pause 0, 0x2A, 0x42, 0x44, nepodporované ID) - neotevírá se; blok, který player odmítne až při přehrání, typ v indexu nemění (cmt_tmz ho přeskakuje příznakem) */
    } en_CMTEXT_BLOCK_TYPE;


    /**
     * @brief Režim rychlosti bloku pásky.
     */
    typedef enum en_CMTEXT_BLOCK_SPEED {
        CMTEXT_BLOCK_SPEED_NONE = 0, /**< rychlost bloku není možné měnit (dává ji soubor) */
        CMTEXT_BLOCK_SPEED_DEFAULT,  /**< rychlost bloku se řídí výchozí rychlostí Virtual CMT (g_cmt.mz_cmtspeed) */
        CMTEXT_BLOCK_SPEED_SET,      /**< blok má vlastní rychlost uloženou v indexu pásky */
    } en_CMTEXT_BLOCK_SPEED;


#ifdef __cplusplus
}
#endif

#endif /* CMTEXT_BLOCK_DEFS_H */

