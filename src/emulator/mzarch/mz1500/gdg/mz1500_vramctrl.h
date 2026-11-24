/*
 * File:   mz1500_vramctrl.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 19:12
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

#ifndef MZ1500_VRAMCTRL_H
#define MZ1500_VRAMCTRL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "debugger/debugger.h"

    /**
     * @brief Stav řadiče VRAM MZ-1500.
     *
     * mz700_wr_latch_is_used: pozůstatek jednorázové závory z modelu MZ-800; na MZ-1500
     *   se k čekání nepoužívá (stroj čeká u každého přístupu v okně), zůstává kvůli
     *   společnému snapshotu a je vždy 0.
     * wait_ticks: počet taktů GDGCLK do konce okna čekání pro právě zpracovávaný přístup;
     *   platí jen mezi vramctrl_mz1500_memop_wait() a zpracováním
     *   INSIDEOP_MREQ_MZ700_VRAMCTRL, jinak bez významu (do snapshotu nepatří).
     */
    typedef struct st_VRAMCTRL
    {
        unsigned mz700_wr_latch_is_used;
        unsigned wait_ticks;
    } st_VRAMCTRL;

    extern st_VRAMCTRL g_vramctrl;

    extern void vramctrl_reset(void);

#include "mz1500_gdg.h"
#include "mz1500_framebuffer.h"
#include "memory/memory.h"
    /*******************************************************************************
     *
     * VRAM kontroler pro rezimy MZ-1500
     *
     ******************************************************************************/

    /**
     * Cteni z MZ-700 VRAM bez ohledu na synchronizaci.
     *
     * @param addr
     * @return
     */
#define vramctrl_mz700_memop_read_byte(addr) g_memory.VRAM[addr]

    /**
     * Interni subrutina fyzickeho zapisu do MZ-700 VRAM - generuje vsak zmenu ve screenu.
     *
     * @param addr
     * @param value
     */
#define vramctrl_mz700_memop_write_byte_internal(addr, value)                                                     \
    {                                                                                                             \
        /* TODO: meli by jsme nastavovat jen pokud se zapisuje do CG-RAM, nebo do VRAM, ktera je opravdu videt */ \
        /* PCG makra MZ-1500 pouzivaji offset presahujici VRAM do sousedniho PCG pole ve strukture */             \
        _Pragma("GCC diagnostic push")                                                                            \
            _Pragma("GCC diagnostic ignored \"-Wstringop-overflow\"")                                             \
                g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;                                            \
        g_memory.VRAM[addr] = value;                                                                              \
        VRAMCTRL_DBG_NOTIFY_VRAM_TOUCH();                                                                         \
        _Pragma("GCC diagnostic pop")                                                                             \
    }

    /**
     * Zapis do MZ-700 VRAM bez synchronizace - generuje vsak zmenu ve screenu.
     *
     * @param addr
     * @param value
     */
#define vramctrl_mz700_memop_write_byte(addr, value) vramctrl_mz700_memop_write_byte_internal(addr, value)

    /**
     * @brief Čekání procesoru při přístupu do VRAM nebo do roviny PCG na MZ-1500.
     *
     * Podle měření na skutečném MZ-1500 (emu-experiments mz1500-meas, VYSLEDKY3.md,
     * 2026-11-24): každý přístup, který padne do okna řádku, čeká do konce okna.
     * Čeká i první přístup v řádku (jednorázová závora z modelu GDG MZ-800 v režimu
     * MZ-700 tu neplatí) a čeká se i v řádcích mimo obraz - okno je dané jen polohou
     * v řádku. Čtení a zápis, znaky a atributy se neliší.
     *
     * @param window_ticks Délka okna v taktech GDGCLK od VIDEO_BEAM_VRAMWAIT_FIRST_COLUMN
     *                     (VIDEO_VRAMWAIT_VRAM_TICKS nebo VIDEO_VRAMWAIT_PCG_TICKS).
     * @pre Před voláním proběhlo mzarch_main_insideop_mreq(), takže
     *      g_gdg.total_elapsed.ticks odpovídá okamžiku přístupu.
     * @post Mimo okno beze změny. V okně je do instrukce vloženo čekání do konce okna
     *       zaokrouhlené nahoru na celé T (mzarch_main_insideop,
     *       INSIDEOP_MREQ_MZ700_VRAMCTRL) a čas GDG je posunutý za ně.
     * @note Vedlejší efekt: zapisuje g_vramctrl.wait_ticks.
     */
    static inline void vramctrl_mz1500_memop_wait(unsigned window_ticks)
    {
        unsigned col = VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks);
        unsigned from_first = (col + VIDEO_SCREEN_WIDTH - VIDEO_BEAM_VRAMWAIT_FIRST_COLUMN) % VIDEO_SCREEN_WIDTH;
        if (from_first >= window_ticks) {
            return;     /* mimo okno - bez čekání */
        }
        g_vramctrl.wait_ticks = window_ticks - from_first;
        mzarch_main_insideop_mreq_mz700_vramctrl();
    }

    /**
     * @brief Čtení z VRAM nebo z roviny PCG se synchronizací a čekáním.
     *
     * @param addr Offset v g_memory.VRAM (roviny PCG leží za VRAM).
     * @param window_ticks Délka okna čekání, viz vramctrl_mz1500_memop_wait().
     * @return Přečtený bajt.
     */
    static inline uint8_t vramctrl_mz1500_memop_read_byte_sync(unsigned addr, unsigned window_ticks)
    {
        mzarch_main_insideop_mreq();
        vramctrl_mz1500_memop_wait(window_ticks);
        return vramctrl_mz700_memop_read_byte(addr);
    }

    /**
     * @brief Zápis do VRAM nebo do roviny PCG se synchronizací a čekáním.
     *
     * @param addr Offset v g_memory.VRAM (roviny PCG leží za VRAM).
     * @param value Zapisovaný bajt.
     * @param window_ticks Délka okna čekání, viz vramctrl_mz1500_memop_wait().
     */
#define vramctrl_mz1500_memop_write_byte_sync(addr, value, window_ticks) \
    {                                                                   \
        mzarch_main_insideop_mreq();                                    \
        vramctrl_mz1500_memop_wait(window_ticks);                       \
        vramctrl_mz700_memop_write_byte_internal(addr, value);          \
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ1500_VRAMCTRL_H */
