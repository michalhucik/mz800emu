/*
 * File:   mz700_vramctrl.h
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

#ifndef MZ700_VRAMCTRL_H
#define MZ700_VRAMCTRL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "debugger/debugger.h"

    /**
     * @brief Stav řadiče VRAM MZ-700.
     *
     * mz700_wr_latch_is_used: pozůstatek jednorázové závory z modelu MZ-800; na MZ-700
     *   se k čekání nepoužívá (čeká každý přístup v okně), zůstává kvůli snapshotu
     *   a je vždy 0.
     * wait_ticks: počet taktů GDGCLK do konce okna čekání pro právě zpracovávaný přístup;
     *   platí jen mezi vramctrl_mz700_memop_wait() a zpracováním
     *   INSIDEOP_MREQ_MZ700_VRAMCTRL, jinak bez významu (do snapshotu nepatří).
     */
    typedef struct st_VRAMCTRL
    {
        unsigned mz700_wr_latch_is_used;
        unsigned wait_ticks;
    } st_VRAMCTRL;

    extern st_VRAMCTRL g_vramctrl;

    extern void vramctrl_reset(void);

#include "mz700_gdg.h"
#include "mz700_framebuffer.h"
#include "memory/memory.h"
    /*******************************************************************************
     *
     * VRAM kontroler pro rezimy MZ-700
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
        /* PCG makra MZ-700 pouzivaji offset presahujici VRAM do sousedniho PCG pole ve strukture */              \
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
     * @brief Čekání procesoru při přístupu do VRAM na MZ-700.
     *
     * Na samostatném MZ-700 změřeno není; pravidlo je převzaté z měření na skutečném
     * MZ-1500 (emu-experiments mz1500-meas, VYSLEDKY3.md, 2026-11-24) [interpolace]:
     * každý přístup, který padne do okna řádku, čeká do konce okna. Čeká i první přístup
     * v řádku (jednorázová závora z modelu GDG MZ-800 v režimu MZ-700 se nepoužívá)
     * a čeká se i v řádcích mimo obraz. Čtení a zápis se neliší.
     *
     * @pre Před voláním proběhlo mzarch_main_insideop_mreq(), takže
     *      g_gdg.total_elapsed.ticks odpovídá okamžiku přístupu.
     * @post Mimo okno beze změny. V okně je do instrukce vloženo čekání do konce okna
     *       zaokrouhlené nahoru na celé T (mzarch_main_insideop,
     *       INSIDEOP_MREQ_MZ700_VRAMCTRL) a čas GDG je posunutý za ně.
     * @note Vedlejší efekt: zapisuje g_vramctrl.wait_ticks.
     */
    static inline void vramctrl_mz700_memop_wait(void)
    {
        unsigned col = VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks);
        unsigned from_first = (col + VIDEO_SCREEN_WIDTH - VIDEO_BEAM_VRAMWAIT_FIRST_COLUMN) % VIDEO_SCREEN_WIDTH;
        if (from_first >= VIDEO_VRAMWAIT_VRAM_TICKS) {
            return;     /* mimo okno - bez čekání */
        }
        g_vramctrl.wait_ticks = VIDEO_VRAMWAIT_VRAM_TICKS - from_first;
        mzarch_main_insideop_mreq_mz700_vramctrl();
    }

    /**
     * @brief Čtení z VRAM se synchronizací a čekáním.
     *
     * @param addr Offset v g_memory.VRAM.
     * @return Přečtený bajt.
     */
    static inline uint8_t vramctrl_mz700_memop_read_byte_sync(uint16_t addr)
    {
        mzarch_main_insideop_mreq();
        vramctrl_mz700_memop_wait();
        return vramctrl_mz700_memop_read_byte(addr);
    }

    /**
     * @brief Zápis do VRAM se synchronizací a čekáním.
     *
     * @param addr Offset v g_memory.VRAM.
     * @param value Zapisovaný bajt.
     */
#define vramctrl_mz700_memop_write_byte_sync(addr, value)           \
    {                                                               \
        mzarch_main_insideop_mreq();                                \
        vramctrl_mz700_memop_wait();                                \
        vramctrl_mz700_memop_write_byte_internal(addr, value);      \
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ700_VRAMCTRL_H */
