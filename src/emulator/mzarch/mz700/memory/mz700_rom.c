#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Lokalizace
#include "i18n.h"

#include "hw-generic/memory/rom_config.h"

extern uint8_t c_ROM_MZ700_13A_0000[];
extern uint8_t c_ROM_MZ700_EU_CGROM[];
extern uint8_t c_ROM_MZ700_E000[];

extern uint8_t c_ROM_MZ700_9B_0000[];
extern uint8_t c_ROM_MZ700_JP_CGROM[];


static st_ROM_CONFIG_ROW c_roms[] = {
    {ROM_CONFIG_TYPE_ROM, true, "STANDARD-EU", N_("Standard Sharp MZ-700 EU ROM (13A)"), ROM_CONFIG_FLAG_NONE, c_ROM_MZ700_13A_0000, c_ROM_MZ700_EU_CGROM, NULL},
    {ROM_CONFIG_TYPE_ROM, true, "STANDARD-JP", N_("Standard Sharp MZ-700 JP ROM (9B)"), ROM_CONFIG_FLAG_NONE, c_ROM_MZ700_9B_0000, c_ROM_MZ700_JP_CGROM, NULL},
    {ROM_CONFIG_TYPE_SECTION, true, NULL, N_("User"), ROM_CONFIG_FLAG_NONE, NULL, NULL, NULL},
    {ROM_CONFIG_TYPE_ROM, true, "USER_DEFINED", N_("User Defined ROM"), ROM_CONFIG_FLAG_USER_DEFINED, NULL, NULL, NULL},
    {ROM_CONFIG_TYPE_NONE, false, NULL, NULL, ROM_CONFIG_FLAG_NONE, NULL, NULL, NULL}};

/**
 * @brief Konfigurace ROM MZ-700.
 *
 * Výchozí ROM podle televizní normy sestavení: NTSC (japonský MZ-700) má japonskou
 * 1Z-009B se znakovou sadou JP, PAL evropskou 1Z-013A.
 */
static st_ROM_CONFIG c_rom_config = {
    .roms = c_roms,
#if MZTVSYS == MZTVSYS_NTSC
    .default_rom = &c_roms[1]};
#else
    .default_rom = &c_roms[0]};
#endif

st_ROM_CONFIG *g_rom_config = &c_rom_config;
