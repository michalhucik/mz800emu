/* 
 * File:   cfgmain.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 15. září 2015, 13:58
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

#ifndef CFGMAIN_H
#define CFGMAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "libs/cfgfile/cfgroot.h"

    // Povolena konvence a.b.c, nebo a.b.c.d
#define CFGMAIN_EMULATOR_VERSION_NUM_STRING "2.0.7"

    // #define CFGMAIN_EMULATOR_VERSION_TAG "preview"
#define CFGMAIN_EMULATOR_VERSION_TAG "devel"
    //#define CFGMAIN_EMULATOR_VERSION_TAG "snapshot"
    // #define CFGMAIN_EMULATOR_VERSION_TAG "stable"

#define EMULATOR_VERSION CFGMAIN_EMULATOR_VERSION_NUM_STRING " " CFGMAIN_EMULATOR_VERSION_TAG

#ifdef EMULATOR_VERSION
#define CFGMAIN_EMULATOR_VERSION EMULATOR_VERSION
#else
#define CFGMAIN_EMULATOR_VERSION "unknown"
#endif

#define CFGMAIN_EMULATOR_VERSION_TEXT CFGMAIN_EMULATOR_VERSION

#ifdef LINUX
#define CFGMAIN_PLATFORM    "Linux"
#endif

#ifdef WINDOWS
#ifdef WINDOWS_X86
#define CFGMAIN_PLATFORM    "Windows x86"
#else
#ifdef WINDOWS_X64
#define CFGMAIN_PLATFORM    "Windows x64"
#else
#define CFGMAIN_PLATFORM    "Windows"
#endif
#endif
#endif

    extern struct st_CFGROOT *g_cfgmain;
    extern bool g_cfgmain_ini_file_exists;

/**
 * @brief Verze formátu INI, kterou emulátor zapisuje do `[CFGMAIN] config_version`.
 *
 * 1.0: klíčové slovo `[ROM] rom_type` zapsané s posunem o skryté vývojové
 *      ROM (do verze 2.0.6 a vývojové verze po ní; viz
 *      cfgmain_ini_has_legacy_rom_keywords()).
 * 1.1: `rom_type` je jméno vybrané ROM.
 */
#define CFGMAIN_CONFIG_VERSION "1.1"

/**
 * @brief Nejstarší `[CFGMAIN] emulator_build_time`, od kterého INI formátu 1.0 nese posunutá jména ROM.
 *
 * Den zavedení dynamické tabulky ROM. Tvar jako emulator_build_time
 * ("RRRR-MM-DD hh:mm:ss"), porovnává se jako řetězec.
 */
#define CFGMAIN_LEGACY_ROM_KEYWORDS_SINCE "2026-03-08"

    /**
     * @brief Je načtené INI ve formátu 1.0, kde je `[ROM] rom_type` zapsané s posunem?
     *
     * Do formátu 1.0 se index klíčového slova `rom_type` počítal i přes
     * vývojové ROM, které bez vývojového režimu klíčové slovo nemají; na
     * MZ-800 se tak vybraná ROM zapsala pod jménem jiné (JSS108C jako
     * WILLY_EN) a stejně posunutě se četla. rom.c podle této funkce čte
     * staré INI starým způsobem, aby se uživateli po přechodu na novou
     * verzi nezměnila ROM; při uložení se INI zapíše ve formátu
     * CFGMAIN_CONFIG_VERSION se skutečným jménem.
     *
     * Posun zapisovaly jen verze sestavené od CFGMAIN_LEGACY_ROM_KEYWORDS_SINCE
     * (zavedení dynamické tabulky ROM); starší verze psaly formát 1.0 se
     * správnými jmény, jejich INI se proto čte podle jmen.
     *
     * @return true = INI existovalo, má `config_version = 1.0`
     *         a `emulator_build_time` od CFGMAIN_LEGACY_ROM_KEYWORDS_SINCE;
     *         false = INI neexistuje, je novější, starší, nebo verzi či čas
     *         sestavení neuvádí (ručně psané INI se čte podle jmen).
     * @pre Po cfgmain_init(). Hodnota se za běhu nemění.
     */
    extern bool cfgmain_ini_has_legacy_rom_keywords ( void );

    extern void cfgmain_init ( void );
    extern void cfgmain_exit ( void );
    extern const char* cfgmain_get_build_date ( void );
    extern const char* cfgmain_get_build_datetime ( void );
    extern char* cfgmain_create_timestamp ( void );
    extern uint32_t cfgmain_get_version_uint32 ( void );

#ifdef __cplusplus
}
#endif

#endif /* CFGMAIN_H */

