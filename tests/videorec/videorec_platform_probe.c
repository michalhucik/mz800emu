/**
 * @file   videorec_platform_probe.c
 * @brief  Testovací sonda: parametry video záznamu z per-arch maker jedné platformy.
 *
 * Soubor se překládá vícekrát (tests/videorec/CMakeLists.txt), pokaždé
 * s jinými MZARCH / MZTVSYS (z mz_arch_compile_definitions() v
 * cmake/AddMzEmu.cmake - stejný zdroj jako u exe) a s jiným jménem funkce
 * `VR_PROBE_FN`. Test
 * test_videorec_platform.c tak porovná skutečné hodnoty per-arch maker
 * všech čtyř exe (mz700emu-pal, mz700emu-ntsc, mz800emu, mz1500emu)
 * s očekáváním, přestože jinak se testy překládají jen pro MZ-800.
 *
 * @par Licence: GPLv3
 */

#include "emulator/videorec/videorec_platform_arch.h"

#ifndef VR_PROBE_FN
#error "VR_PROBE_FN must name the probe function"
#endif

/**
 * @brief Vrátí parametry platformy, pro kterou je sonda přeložená.
 * @param out Výstup (nesmí být NULL).
 * @post `*out` = VIDEOREC_PLATFORM_CURRENT_INIT této platformy.
 */
void VR_PROBE_FN(st_VIDEOREC_PLATFORM *out);

void VR_PROBE_FN(st_VIDEOREC_PLATFORM *out)
{
    static const st_VIDEOREC_PLATFORM p = VIDEOREC_PLATFORM_CURRENT_INIT;
    *out = p;
}
