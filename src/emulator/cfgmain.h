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
#define CFGMAIN_EMULATOR_VERSION_NUM_STRING "2.1.7"

    // #define CFGMAIN_EMULATOR_VERSION_TAG "preview"
#define CFGMAIN_EMULATOR_VERSION_TAG "devel"
    //#define CFGMAIN_EMULATOR_VERSION_TAG "snapshot"
    //#define CFGMAIN_EMULATOR_VERSION_TAG "stable"

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

