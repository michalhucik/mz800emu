/*
 * File:   rom.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 22. února 2016, 18:30
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

#ifndef ROM_H
#define ROM_H

#include <stdint.h>
#include "rom_config.h"

#define ROM_SIZE_0000 0x1000
#define ROM_SIZE_CGROM 0x1000
#define ROM_SIZE_E000 0x2000

#define ROM_SIZE_TOTAL ROM_SIZE_0000 + ROM_SIZE_CGROM + ROM_SIZE_E000

typedef enum en_ROM_BOOL
{
    ROM_BOOL_NO = 0,
    ROM_BOOL_YES = 1
} en_ROM_BOOL;

typedef enum en_ROM_CMTHACK
{
    ROM_CMTHACK_DISABLED = 0,
    ROM_CMTHACK_DEFAULT,
    ROM_CMTHACK_CUSTOM
} en_ROM_CMTHACK;

typedef struct st_ROM_AREA
{
    uint8_t mz700rom[ROM_SIZE_0000];
    uint8_t cgrom[ROM_SIZE_CGROM];
    uint8_t mz800rom[ROM_SIZE_E000];
} st_ROM_AREA;

typedef struct st_ROM
{
    st_ROM_CONFIG_ROW *rom_cfg;
    uint8_t *mz700rom;
    uint8_t *cgrom;
    uint8_t *mz800rom;
    en_ROM_BOOL user_defined_allinone;
    char *user_defined_allinone_fp;
    char *user_defined_mz700_fp;
    char *user_defined_cgrom_fp;
    char *user_defined_mz800_fp;
    en_ROM_CMTHACK user_defined_cmthack_type;
    en_ROM_BOOL user_defined_cmthack_allinone;
    char *user_defined_cmthack_allinone_fp;
    char *user_defined_cmthack_mz700_fp;
    char *user_defined_cmthack_cgrom_fp;
    char *user_defined_cmthack_mz800_fp;
    st_ROM_AREA rom_user_defined;
    st_ROM_AREA rom_user_defined_cmthack;
    en_ROM_BOOL user_defined_rom_loaded;
} st_ROM;

extern st_ROM g_rom;

#define TEST_ROM_CMTHACK_INCOMP ROM_CFG_TEST_FLAG(g_rom.rom_cfg, ROM_CONFIG_FLAG_CMTHACK_INCOMP)
#define TEST_ROM_USER_DEFINED ROM_CFG_TEST_FLAG(g_rom.rom_cfg, ROM_CONFIG_FLAG_USER_DEFINED)

#define ROM_TEST_CURRENT(row_ptr) (g_rom.rom_cfg == (row_ptr))

#ifdef __cplusplus
extern "C"
{
#endif

    void rom_init(void);
    /**
     * @brief Nainstaluje ROM podle řádku konfigurace a znovu použije patch CMT hacku.
     *
     * Přepíše g_memory.ROM (včetně CG-ROM) a g_rom.rom_cfg; uživatelskou ROM
     * načte ze souborů, pokud ještě není načtená. Při chybě uživatelské ROM
     * nainstaluje výchozí ROM.
     *
     * @param row Řádek g_rom_config->roms; NULL = výchozí ROM.
     * @pre Emu vlákno (zásah z GUI přes rom_request.h) nebo inicializace.
     */
    void rom_reinstall(st_ROM_CONFIG_ROW *row);
    int rom_user_defined_check_filepath(char **filepath, en_ROM_BOOL clear);
    int rom_user_defined_check_size(char **filepath, uint32_t size, en_ROM_BOOL clear);
    void rom_set_user_defined_filepath(char **dst, char *src);
    int rom_user_defined_rom_area_load(st_ROM_AREA *dst, en_ROM_BOOL allinone, char *allinone_fp, char *mz700_fp, char *cgrom_fp, char *mz800_fp);

#ifdef __cplusplus
}
#endif

#endif /* ROM_H */
