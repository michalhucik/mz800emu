/**
 * @file   memory_driver.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.2
 * @brief  Paměťový driver pro generic_driver (statická a realloc varianta).
 *
 * Soubor slouží standalone buildu (bez baseui, s definovaným
 * GENERIC_DRIVER_NO_BASEUI) i emulátoru mz800emu. Rozdíl mezi oběma
 * prostředími řeší výhradně baseui_compat.h: alokace probíhá přes makra
 * baseui_tools_mem_*, která se ve standalone buildu mapují na
 * malloc/realloc/free a v mz800emu na alokátor baseui.
 *
 * Poskytuje dvě globální instance driveru pro handler typu
 * HANDLER_TYPE_MEMORY:
 * - g_memory_driver_static - buffer má pevnou velikost, prepare_cb mimo
 *   rozsah selže,
 * - g_memory_driver_realloc - prepare_cb buffer podle potřeby zvětší.
 *
 * Obě instance je nutné před prvním použitím naplnit voláním
 * memory_driver_init().
 *
 * @par Historie:
 * - 2.0.2: alokace přes makra baseui_tools_mem_* (baseui_compat.h).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MEMORY_DRIVER_H
#define MEMORY_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "generic_driver.h"

    /**
     * @brief Paměťový driver s pevnou velikostí bufferu.
     *
     * prepare_cb vrací ukazatel přímo do bufferu handleru a buffer nikdy
     * nezvětšuje; požadavek mimo aktuální velikost skončí chybou
     * GENERIC_DRIVER_ERROR_SEEK nebo GENERIC_DRIVER_ERROR_SIZE.
     *
     * @pre Před použitím musí proběhnout memory_driver_init().
     */
    extern st_DRIVER g_memory_driver_static;

    /**
     * @brief Paměťový driver s automatickým zvětšováním bufferu.
     *
     * prepare_cb při požadavku za koncem bufferu buffer realokuje na
     * velikost offset + count_bytes (pokud handler není jen pro čtení).
     * Ostatní callbacky jsou shodné s g_memory_driver_static.
     *
     * @pre Před použitím musí proběhnout memory_driver_init().
     */
    extern st_DRIVER g_memory_driver_realloc;

    /**
     * @brief Naplní callbacky obou globálních paměťových driverů.
     *
     * Volá generic_driver_setup() pro g_memory_driver_static
     * a g_memory_driver_realloc a vynuluje jejich chybový kód. Funkce
     * nealokuje paměť a nemůže selhat; opakované volání callbacky přepíše
     * stejnými hodnotami.
     *
     * @post Obě instance driveru mají nastavené callbacky
     *       open/close/read/write/prepare/truncate a lze je přiřadit handleru.
     *
     * @note Není thread-safe; volat jednou při inicializaci programu, dřív než
     *       driver použije kterýkoli handler.
     */
    extern void memory_driver_init ( void );

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DRIVER_H */
