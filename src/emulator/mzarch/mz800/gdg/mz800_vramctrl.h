/*
 * File:   mz800_vramctrl.h
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

#ifndef MZ800_VRAMCTRL_H
#define MZ800_VRAMCTRL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "debugger/debugger.h"

    typedef enum
    {
        GDG_WF_MODE_SINGLE = 0,
        GDG_WF_MODE_EXOR,
        GDG_WF_MODE_OR,
        GDG_WF_MODE_RESET,
        GDG_WF_MODE_REPLACE,
        GDG_WF_MODE_PSET = 6
    } WFR_MODE;

    typedef struct st_VRAMCTRL
    {
        unsigned regWF_PLANE;
        WFR_MODE regWF_MODE;

        unsigned regRF_PLANE;
        unsigned regRF_SEARCH;
        unsigned regWFRF_VBANK;
    } st_VRAMCTRL;

    extern st_VRAMCTRL g_vramctrl;

    extern void vramctrl_reset(void);

    extern void vramctrl_mz800_set_wf_rf_reg(int addr, uint8_t value);

    extern uint8_t vramctrl_mz800_memop_read_byte_sync(uint16_t addr);
    extern uint8_t vramctrl_mz800_memop_read_byte(uint16_t addr);
    extern void vramctrl_mz800_memop_write_byte(uint16_t addr, uint8_t value);
    extern void vramctrl_mz800_memop_write_byte_sync(uint16_t addr, uint8_t value);

    /**
     * @brief Kam by zápis do VRAM v režimu MZ-800 zapsal (bez zápisu a bez vedlejších efektů).
     *
     * Stejný výpočet jako vramctrl_mz800_memop_write_byte() (640 režim,
     * hwscroll, WF_MODE, WF_PLANE, VBANK); pro záznam zápisů debuggeru.
     *
     * @param addr   Mz800 VRAM offset (0x0000 - 0x3FFF, = CPU adresa & 0x3FFF).
     * @param out_phy Výstup: fyzická adresa v rovině (0x0000 - 0x1FFF).
     * @param out_we  Výstup: maska rovin bit 0 = I, 1 = II, 2 = III, 3 = IV.
     */
    extern void vramctrl_mz800_write_target(uint16_t addr, unsigned *out_phy, unsigned *out_we);

#include "mz800_gdg.h"
#include "mz800_framebuffer.h"
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
    /**
     * Hodnota zapisu do MZ-700 VRAM podle registru WF: GDG ji zpracuje jako rovinu I.
     *
     * SM800 (tabulka WF): v rezimu MZ-700 ma byt WF = 01h (SINGLE, rovina I) - "Writes WD into the DATA, ATB,
     * and CG area". Skutecne MZ-800 s WF ponechanym v rezimu XOR (posledni kresleni programu v rezimu MZ-800)
     * nove znaky do stareho obsahu prixoruje a smazani nulou nic nezmeni (foto autora UC-UniMgr, 2026-11-12).
     * SINGLE, XOR, OR, RESET zapisuji jen s vybranou rovinou I; REPLACE a PSET s nevybranou rovinou I zapisuji
     * 0 (REPLACE), resp. VRAM AND NOT data (PSET), jako nevybrane roviny v rezimu MZ-800.
     *
     * @param old   Puvodni bajt VRAM.
     * @param value Bajt od CPU.
     * @param out   Vystup: bajt k zapisu.
     * @return 1 = zapsat, 0 = beze zmeny.
     */
    static inline int vramctrl_mz700_wf_value(uint8_t old, uint8_t value, uint8_t *out)
    {
        int p1 = (g_vramctrl.regWF_PLANE & 0x01) ? 1 : 0;
        switch (g_vramctrl.regWF_MODE) {
            case GDG_WF_MODE_SINGLE:
                *out = value;
                return p1;
            case GDG_WF_MODE_EXOR:
                *out = (uint8_t) (old ^ value);
                return p1;
            case GDG_WF_MODE_OR:
                *out = (uint8_t) (old | value);
                return p1;
            case GDG_WF_MODE_RESET:
                *out = (uint8_t) (old & ~value);
                return p1;
            case GDG_WF_MODE_REPLACE:
                *out = p1 ? value : 0x00;
                return 1;
            default: /* PSET */
                *out = p1 ? (uint8_t) (old | value) : (uint8_t) (old & ~value);
                return 1;
        }
    }

#define vramctrl_mz700_memop_write_byte_internal(addr, value)                                                     \
    {                                                                                                             \
        /* TODO: meli by jsme nastavovat jen pokud se zapisuje do CG-RAM, nebo do VRAM, ktera je opravdu videt */ \
        uint8_t wf_out_;                                                                                          \
        if (vramctrl_mz700_wf_value(g_memory.VRAM[addr], (value), &wf_out_)) {                                    \
            g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;                                                \
            g_memory.VRAM[addr] = wf_out_;                                                                        \
        }                                                                                                         \
        VRAMCTRL_DBG_NOTIFY_VRAM_TOUCH();                                                                         \
    }

    /**
     * Zapis do MZ-700 VRAM bez synchronizace - generuje vsak zmenu ve screenu.
     *
     * @param addr
     * @param value
     */
#define vramctrl_mz700_memop_write_byte(addr, value) vramctrl_mz700_memop_write_byte_internal(addr, value)

    /**
     * @brief Čtení z VRAM/CG-RAM v režimu MZ-700 se synchronizací a WAIT.
     *
     * Synchronizuje čas GDG na konec M-cyklu a přidá WAIT podle modelu
     * mz800_vramwait.h s rozvrhem slotů MZ-700: ve viditelné části řádku
     * čeká čtení do jejího konce, v zatemnění 2-4 TW.
     *
     * @param addr Offset ve VRAM/CG-RAM (MEMORY_MZ700_VRAM_OFS(): v DMD 08h až 0Bh adresa bez bitů 0E000h, v 0Ch až 0Fh (adresa >> 1) & 1FFFh).
     * @return Přečtený bajt.
     * @pre Volá se z emu vlákna uvnitř instrukce (callback čtení paměti).
     */
    static inline uint8_t vramctrl_mz700_memop_read_byte_sync(uint16_t addr)
    {
        mzarch_main_insideop_mreq();
        mzarch_main_insideop_mreq_mz800_vramctrl_read();
        return vramctrl_mz700_memop_read_byte(addr);
    }

    /**
     * @brief Zápis do VRAM/CG-RAM v režimu MZ-700 se synchronizací a WAIT.
     *
     * Zápis jde přes jednobajtový buffer GDG: čeká jen, dokud se nezapíše
     * předchozí zápis (ve viditelné části řádku tedy druhý zápis čeká
     * do jejího konce). Model mz800_vramwait.h, rozvrh slotů MZ-700.
     *
     * @param addr Offset ve VRAM/CG-RAM (MEMORY_MZ700_VRAM_OFS(): v DMD 08h až 0Bh adresa bez bitů 0E000h, v 0Ch až 0Fh (adresa >> 1) & 1FFFh).
     * @param value Zapisovaný bajt.
     * @pre Volá se z emu vlákna uvnitř instrukce (callback zápisu paměti).
     */
#define vramctrl_mz700_memop_write_byte_sync(addr, value)           \
    {                                                               \
        mzarch_main_insideop_mreq();                                \
        mzarch_main_insideop_mreq_mz800_vramctrl_write();           \
        vramctrl_mz700_memop_write_byte_internal(addr, value);      \
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ800_VRAMCTRL_H */
