/*
 * File:   mz700_memory.h
 * Author: chaky
 *
 * Created on 15. června 2015, 17:34
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

#ifndef MZ700_MEMORY_H
#define MZ700_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     *
     *  Flagy mapovani pameti
     *
     */
#define MEMORY_MZ700_MAP_FLAG_ROM_0000   (1 << 0)
#define MEMORY_MZ700_MAP_FLAG_ROM_E000   (1 << 1) /* VRAM D000, ROM E000 (porty) */
#define MEMORY_MZ700_MAP_FLAG_PROHIBITED (1 << 2) /* MZ-700 banking "Prohibited" mode (OUT 0xE5/E6).
                                                   * 0 = normal (ROM E800/code viditelna),
                                                   * 1 = Prohibited (ROM E800-EFFF a F000-FFFF
                                                   *     odpojene, cteni vraci 0xFF).
                                                   * Aktivuje OUT 0xE5, rusi OUT 0xE6 nebo
                                                   * memory_reset. Polarita sjednocena s
                                                   * MZ-800 (= flag set = Prohibited active).
                                                   */

    /* Memory map porty pro IORQ - PWRITE */
    typedef enum en_MMAP_MZ700_PWRITE
    {
        MMAP_MZ700_PWRITE_E0 = 0xe0, /* unmap ROM 0000 */
        MMAP_MZ700_PWRITE_E1 = 0xe1, /* unmap VRAM D000, ROM E000 (porty) */
        MMAP_MZ700_PWRITE_E2 = 0xe2, /* map ROM 0000 */
        MMAP_MZ700_PWRITE_E3 = 0xe3, /* map VRAM D000, ROM E000 (porty) */
        MMAP_MZ700_PWRITE_E4 = 0xe4, /* map ROM 0000, VRAM D000, ROM E000 (porty), ukonci Prohibited */
        MMAP_MZ700_PWRITE_E5 = 0xe5, /* aktivuje Prohibited mode (= unmap ROM E800/code) */
        MMAP_MZ700_PWRITE_E6 = 0xe6, /* ukonci Prohibited mode (= remap ROM E800/code) */
    } en_MMAP_MZ700_PWRITE;

    extern uint8_t *g_memoryVRAM;

    /**
     * @brief Zápis debuggeru do aktuálně mapované paměti MZ-700 (bez synchronizace, bez MEM_W BP).
     *
     * Tělo memory_write_byte_no_bp(); volá se přes g_mz700_cpu_ops.raw_write
     * (mz700_devbus.h), aby obálka devbus mohla zápis ohlásit zařízením.
     * Zápis na 0E000h-0E008h při mapované horní oblasti trefí 8255 / 8253 / GDG.
     *
     * @param addr  Adresa v mapě CPU.
     * @param value Zapisovaný bajt.
     * @pre Emu vlákno.
     */
    extern void memory_write_byte_raw(uint16_t addr, uint8_t value);

    /*
     *
     *  Testy mapovacich stavu
     *
     */
#define MEMORY_MZ700_MAP_TEST_ROM_0000   (g_memory.map & MEMORY_MZ700_MAP_FLAG_ROM_0000)
#define MEMORY_MZ700_MAP_TEST_ROM_E000   (g_memory.map & MEMORY_MZ700_MAP_FLAG_ROM_E000)
#define MEMORY_MZ700_MAP_TEST_PROHIBITED (g_memory.map & MEMORY_MZ700_MAP_FLAG_PROHIBITED)

#define MEMORY_MZ700_MAP_TEST_VRAM_D000 (g_memory.map & MEMORY_MZ700_MAP_FLAG_ROM_E000)
#define MEMORY_MZ700_MAP_TEST_VRAM      (MEMORY_MZ700_MAP_TEST_VRAM_D000)
#define MEMORY_MZ700_MAP_TEST_PORT_E000 (g_memory.map & MEMORY_MZ700_MAP_FLAG_ROM_E000)


#ifdef __cplusplus
}
#endif

#endif /* MZ700_MEMORY_H */
