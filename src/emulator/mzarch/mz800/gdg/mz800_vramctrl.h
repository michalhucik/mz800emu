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
     * @brief Čtení z VRAM/CG-RAM v režimu MZ-700 podle registru RF (pomalá cesta).
     *
     * GDG vede čtení CPU i v režimu MZ-700 (DMD 08h až 0Fh) přes registr RF:
     * buňka základní VRAM (sběrnice VA, rovina I) se čte s bitem 0, buňka téže
     * adresy v rozšířené VRAM (sběrnice VC, rovina III) s bitem 2.
     *
     * | RF | Výsledek |
     * |----|----------|
     * | READ (bit 7 = 0) | AND vybraných buněk (bit 0 = VA, bit 2 = VC); nic vybráno = FFh |
     * | SEARCH, bit 4 = 0 | bit 0 = 1: VA, jinak NOT VA |
     * | SEARCH, bit 4 = 1 | bit 2 = 1: VC, jinak NOT VC |
     *
     * Zdroje: READ s bity 1 a 3 = 0 změřeno na skutečném MZ-800 s rozšířenou
     * VRAM programem wf700_meas1 (2026-11-25; všechny porovnávané hodnoty shodné
     * s C modelem netlistu gdg-mz800). SEARCH je jen z C modelu pro DMD 0Ch
     * [na stroji neověřeno].
     *
     * Nemodeluje se: bity 1 a 3 RF (roviny II a IV) a SEARCH v DMD 08h až 0Bh
     * čtou podle modelu navíc jinou buňku VRAM, která nemá pevný vztah k adrese
     * CPU; emulátor ji bere jako nevybranou.
     *
     * Bez rozšířené VRAM čte VC 0FFh (g_memoryVRAM_III).
     *
     * @param addr Offset ve VRAM/CG-RAM (0x0000 až 0x1FFF, MEMORY_MZ700_VRAM_OFS()).
     * @return Přečtený bajt.
     * @pre Režim MZ-700 (DMD bit 3 = 1). Bez vedlejších efektů.
     */
    extern uint8_t vramctrl_mz700_read_rf(uint16_t addr);

    /**
     * @brief Zápis do VRAM/CG-RAM v režimu MZ-700 podle registru WF (pomalá cesta).
     *
     * GDG vede zápis CPU i v režimu MZ-700 (DMD 08h až 0Fh) přes registr WF,
     * stejně pro znaky, atributy i CG-RAM. D = bajt od CPU.
     *
     * | WF | Základní VRAM (VA) | Rozšířená VRAM (VC) |
     * |----|--------------------|---------------------|
     * | SINGLE, XOR, OR, RESET | zápis při bitu 0 = 1 | zápis při bitu 2 = 1 |
     * | REPLACE, bit 4 = 0 | D při bitu 0 = 1, jinak 0 | beze změny |
     * | REPLACE, bit 4 = 1 | beze změny | D při bitu 2 = 1, jinak 0 |
     * | PSET, bit 4 = 0 | OR D při bitu 0 = 1, jinak AND NOT D | beze změny |
     * | PSET, bit 4 = 1 | beze změny | OR D při bitu 2 = 1, jinak AND NOT D |
     *
     * Bity 1 a 3 WF (roviny II a IV) vliv nemají. Zdroj: C model netlistu
     * gdg-mz800 a měření na skutečném MZ-800 s rozšířenou VRAM programem
     * wf700_meas1 (2026-11-25): všech 256 hodnot WF pro znak, atribut a CG-RAM
     * v DMD 08h a pro znak v DMD 0Ch, shoda s modelem ve všech.
     *
     * Bez rozšířené VRAM se zápis do VC ztratí (g_memoryVRAM_III_WR).
     *
     * @param addr  Offset ve VRAM/CG-RAM (0x0000 až 0x1FFF, MEMORY_MZ700_VRAM_OFS()).
     * @param value Bajt od CPU.
     * @pre Režim MZ-700 (DMD bit 3 = 1).
     * @post g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED.
     */
    extern void vramctrl_mz700_write_wf(uint16_t addr, uint8_t value);

    /**
     * @brief Kam by zápis do VRAM v režimu MZ-700 zapsal (bez zápisu a bez vedlejších efektů).
     *
     * Stejné rozhodnutí jako vramctrl_mz700_write_wf(); pro záznam zápisů debuggeru.
     *
     * @param out_we Výstup: bit 0 = buňka základní VRAM (rovina I), bit 2 = buňka
     *               rozšířené VRAM (rovina III). 0 = zápis nic nezmění.
     */
    extern void vramctrl_mz700_write_target(unsigned *out_we);

    /**
     * @brief Čtení z VRAM/CG-RAM v režimu MZ-700 bez synchronizace.
     *
     * S RF = 01h (READ, jen rovina I; hodnota po resetu a z ROM) čte přímo
     * buňku základní VRAM, jinak přes vramctrl_mz700_read_rf().
     *
     * @param addr Offset ve VRAM/CG-RAM (0x0000 až 0x1FFF).
     * @return Přečtený bajt.
     */
    static inline uint8_t vramctrl_mz700_memop_read_byte(uint16_t addr)
    {
        if (!g_vramctrl.regRF_SEARCH && (g_vramctrl.regRF_PLANE == 0x01))
        {
            return g_memory.VRAM[addr];
        };
        return vramctrl_mz700_read_rf(addr);
    }

    /**
     * @brief Zápis do VRAM/CG-RAM v režimu MZ-700 bez synchronizace; hlásí změnu obrazu.
     *
     * S WF = 01h nebo 11h (SINGLE, jen rovina I; hodnota po resetu a z ROM)
     * zapíše přímo do buňky základní VRAM, jinak přes vramctrl_mz700_write_wf().
     *
     * @param addr  Offset ve VRAM/CG-RAM (0x0000 až 0x1FFF).
     * @param value Bajt od CPU.
     */
#define vramctrl_mz700_memop_write_byte_internal(addr, value)                                                     \
    {                                                                                                             \
        if ((g_vramctrl.regWF_MODE == GDG_WF_MODE_SINGLE) && (g_vramctrl.regWF_PLANE == 0x01))                    \
        {                                                                                                         \
            /* TODO: meli by jsme nastavovat jen pokud se zapisuje do CG-RAM, nebo do VRAM, ktera je opravdu videt */ \
            g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;                                                \
            g_memory.VRAM[addr] = value;                                                                          \
        }                                                                                                         \
        else                                                                                                      \
        {                                                                                                         \
            vramctrl_mz700_write_wf(addr, value);                                                                 \
        };                                                                                                        \
        VRAMCTRL_DBG_NOTIFY_VRAM_TOUCH();                                                                         \
    }

    /**
     * @brief Zápis do VRAM/CG-RAM v režimu MZ-700 bez synchronizace (viz vramctrl_mz700_memop_write_byte_internal).
     *
     * @param addr  Offset ve VRAM/CG-RAM (0x0000 až 0x1FFF).
     * @param value Bajt od CPU.
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
