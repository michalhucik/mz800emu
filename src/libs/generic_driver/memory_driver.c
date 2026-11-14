/**
 * @file   memory_driver.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.2
 * @brief  Implementace paměťového driveru pro generic_driver.
 *
 * Soubor slouží standalone buildu (bez baseui, s definovaným
 * GENERIC_DRIVER_NO_BASEUI) i emulátoru mz800emu; prostředí se liší jen
 * v tom, na co baseui_compat.h namapuje makra baseui_tools_mem_*.
 * Poskytuje globální instance driveru pro paměťový handler (statická
 * a realloc varianta), viz memory_driver.h.
 *
 * Všechny callbacky dodržují kontrakt generic_driver: vrací EXIT_SUCCESS
 * nebo EXIT_FAILURE, chybu ukládají do h->err (chyba handleru, např.
 * ochrana proti zápisu) nebo d->err (chyba driveru, např. rozsah, alokace)
 * a errno nepoužívají.
 *
 * Synchronizace: callbacky nepoužívají žádný zámek. Současný přístup
 * k jednomu handleru z více vláken musí vyloučit volající.
 *
 * @par Historie:
 * - 2.0.2: alokace přes makra baseui_tools_mem_* (baseui_compat.h), aby se
 *   v mz800emu používal alokátor baseui; standalone chování beze změny.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memory_driver.h"
#include "../baseui_compat.h"

st_DRIVER g_memory_driver_static;
st_DRIVER g_memory_driver_realloc;


/**
 * @brief Připraví požadovanou část obrazu v paměti - bez realokace.
 *
 * Vrátí ukazatel přímo do bufferu handleru (žádná kopie). Buffer se nikdy
 * nezvětšuje.
 *
 * @param h           otevřený paměťový handler
 * @param offset      pozice prvního požadovaného bajtu
 * @param buffer      [out] ukazatel do bufferu handleru na pozici offset;
 *                    při chybě NULL (kromě selhání bootstrapu, kdy se
 *                    nemění)
 * @param count_bytes počet požadovaných bajtů
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE: handler není připravený
 *         (bootstrap), offset > size (GENERIC_DRIVER_ERROR_SEEK),
 *         offset + count_bytes > size (GENERIC_DRIVER_ERROR_SIZE)
 *
 * @pre Handler je typu HANDLER_TYPE_MEMORY a je otevřený.
 * @post Vrácený ukazatel patří handleru a je platný jen do další operace,
 *       která může buffer realokovat nebo uvolnit (truncate, close,
 *       prepare realloc varianty).
 */
static int memory_driver_prepare_static_cb ( st_HANDLER *h, uint32_t offset, void **buffer, uint32_t count_bytes ) {
    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;
    uint32_t need_size = offset + count_bytes;
    *buffer = NULL;
    if ( offset > memspec->size ) { d->err = GENERIC_DRIVER_ERROR_SEEK; return EXIT_FAILURE; }
    if ( need_size > memspec->size ) { d->err = GENERIC_DRIVER_ERROR_SIZE; return EXIT_FAILURE; }
    *buffer = &memspec->ptr[offset];
    return EXIT_SUCCESS;
}


/**
 * @brief Připraví požadovanou část obrazu v paměti - s povolenou realokací.
 *
 * Pokud požadovaný rozsah přesahuje aktuální velikost bufferu, buffer se
 * realokuje na přesně offset + count_bytes bajtů (baseui_tools_mem_realloc).
 * Obsah nově přidané části není inicializovaný.
 *
 * @param h           otevřený paměťový handler
 * @param offset      pozice prvního požadovaného bajtu
 * @param buffer      [out] ukazatel do bufferu handleru na pozici offset;
 *                    při chybě NULL (kromě selhání bootstrapu)
 * @param count_bytes počet požadovaných bajtů
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE: handler není připravený
 *         (bootstrap), zvětšení u handleru jen pro čtení
 *         (h->err = HANDLER_ERROR_WRITE_PROTECTED), selhání realokace
 *         (GENERIC_DRIVER_ERROR_REALLOC; původní buffer zůstává platný)
 *
 * @pre Handler je typu HANDLER_TYPE_MEMORY a je otevřený.
 * @post Při zvětšení se mění memspec->ptr a memspec->size; dříve vrácené
 *       ukazatele do bufferu jsou neplatné. Vrácený ukazatel patří handleru.
 */
static int memory_driver_prepare_realloc_cb ( st_HANDLER *h, uint32_t offset, void **buffer, uint32_t count_bytes ) {
    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;
    uint32_t need_size = offset + count_bytes;
    *buffer = NULL;
    if ( ( offset > memspec->size ) || ( need_size > memspec->size ) ) {
        if ( h->status & HANDLER_STATUS_READ_ONLY ) { h->err = HANDLER_ERROR_WRITE_PROTECTED; return EXIT_FAILURE; }
        uint8_t *new_ptr = baseui_tools_mem_realloc ( memspec->ptr, need_size );
        if ( !new_ptr ) { d->err = GENERIC_DRIVER_ERROR_REALLOC; return EXIT_FAILURE; }
        memspec->ptr = new_ptr;
        memspec->size = need_size;
    }
    *buffer = &memspec->ptr[offset];
    return EXIT_SUCCESS;
}


/**
 * @brief Přečte data z paměťového obrazu do bufferu volajícího.
 *
 * Pokud buffer volajícího ukazuje přímo na zdrojové místo v obrazu
 * (typicky výsledek prepare_cb), kopie se vynechá.
 *
 * @param h           otevřený paměťový handler
 * @param offset      pozice prvního čteného bajtu
 * @param buffer      cílový buffer o velikosti alespoň count_bytes
 * @param count_bytes počet čtených bajtů
 * @param readlen     [out] počet přečtených bajtů; při chybě 0
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE: handler není připravený
 *         (bootstrap), offset > size (GENERIC_DRIVER_ERROR_SEEK),
 *         offset + count_bytes > size (GENERIC_DRIVER_ERROR_SIZE);
 *         částečné čtení se neprovádí
 *
 * @pre Handler je typu HANDLER_TYPE_MEMORY a je otevřený.
 */
static int memory_driver_read_cb ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *readlen ) {
    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
    *readlen = 0;
    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;
    if ( offset > memspec->size ) { d->err = GENERIC_DRIVER_ERROR_SEEK; return EXIT_FAILURE; }
    if ( offset + count_bytes > memspec->size ) { d->err = GENERIC_DRIVER_ERROR_SIZE; return EXIT_FAILURE; }
    *readlen = count_bytes;
    if ( &memspec->ptr[offset] != buffer ) memmove ( buffer, &memspec->ptr[offset], count_bytes );
    return EXIT_SUCCESS;
}


/**
 * @brief Zapíše data z bufferu volajícího do paměťového obrazu.
 *
 * Buffer se nezvětšuje; zápis za konec obrazu je chyba (pro zvětšení
 * slouží prepare_cb realloc varianty). Pokud buffer volajícího ukazuje
 * přímo na cílové místo v obrazu (data už zapsal volající přes
 * prepare_cb), kopie se vynechá a příznak memspec->updated se nenastaví.
 *
 * @param h           otevřený paměťový handler
 * @param offset      pozice prvního zapisovaného bajtu
 * @param buffer      zdrojová data o velikosti alespoň count_bytes
 * @param count_bytes počet zapisovaných bajtů
 * @param writelen    [out] počet zapsaných bajtů; při chybě 0
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE: handler není připravený
 *         (bootstrap), handler jen pro čtení
 *         (h->err = HANDLER_ERROR_WRITE_PROTECTED), offset > size
 *         (GENERIC_DRIVER_ERROR_SEEK), offset + count_bytes > size
 *         (GENERIC_DRIVER_ERROR_SIZE)
 *
 * @pre Handler je typu HANDLER_TYPE_MEMORY a je otevřený.
 * @post Po skutečné kopii je memspec->updated = 1.
 */
static int memory_driver_write_cb ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *writelen ) {
    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
    *writelen = 0;
    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;
    if ( h->status & HANDLER_STATUS_READ_ONLY ) { h->err = HANDLER_ERROR_WRITE_PROTECTED; return EXIT_FAILURE; }
    if ( offset > memspec->size ) { d->err = GENERIC_DRIVER_ERROR_SEEK; return EXIT_FAILURE; }
    if ( offset + count_bytes > memspec->size ) { d->err = GENERIC_DRIVER_ERROR_SIZE; return EXIT_FAILURE; }
    *writelen = count_bytes;
    if ( &memspec->ptr[offset] != buffer ) { memspec->updated = 1; memmove ( &memspec->ptr[offset], buffer, count_bytes ); }
    return EXIT_SUCCESS;
}


/**
 * @brief Změní velikost paměťového obrazu na size bajtů.
 *
 * Přes baseui_tools_mem_realloc obraz zkrátí i prodlouží; prodloužená
 * část není inicializovaná.
 *
 * @param h    otevřený paměťový handler
 * @param size nová velikost obrazu v bajtech (alespoň 1)
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE: handler není připravený
 *         (bootstrap), handler jen pro čtení
 *         (h->err = HANDLER_ERROR_WRITE_PROTECTED), size == 0
 *         (GENERIC_DRIVER_ERROR_SIZE), selhání realokace
 *         (GENERIC_DRIVER_ERROR_REALLOC; původní buffer zůstává platný)
 *
 * @pre Handler je typu HANDLER_TYPE_MEMORY a je otevřený.
 * @post Při úspěchu se mění memspec->ptr a memspec->size; dříve vrácené
 *       ukazatele do bufferu jsou neplatné.
 */
static int memory_driver_truncate_cb ( st_HANDLER *h, uint32_t size ) {
    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;
    if ( h->status & HANDLER_STATUS_READ_ONLY ) { h->err = HANDLER_ERROR_WRITE_PROTECTED; return EXIT_FAILURE; }
    if ( size < 1 ) { d->err = GENERIC_DRIVER_ERROR_SIZE; return EXIT_FAILURE; }
    uint8_t *new_ptr = baseui_tools_mem_realloc ( memspec->ptr, size );
    if ( !new_ptr ) { d->err = GENERIC_DRIVER_ERROR_REALLOC; return EXIT_FAILURE; }
    memspec->ptr = new_ptr;
    memspec->size = size;
    return EXIT_SUCCESS;
}


/**
 * @brief Otevře paměťový handler - alokuje vynulovaný buffer.
 *
 * Alokuje memspec->open_size bajtů přes baseui_tools_mem_alloc0 a nastaví
 * handler do stavu HANDLER_STATUS_READY (případný příznak READ_ONLY se
 * tím smaže).
 *
 * @param h paměťový handler s nastaveným memspec->open_size
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE: handler už je otevřený nebo má
 *         buffer (GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY), handler není typu
 *         HANDLER_TYPE_MEMORY (GENERIC_DRIVER_ERROR_HANDLER_TYPE),
 *         open_size == 0 (GENERIC_DRIVER_ERROR_SIZE), selhání alokace
 *         (GENERIC_DRIVER_ERROR_MALLOC; handler zůstane NOT_READY)
 *
 * @pre memspec->ptr == NULL a handler není otevřený.
 * @post Při úspěchu handler vlastní buffer memspec->ptr o velikosti
 *       memspec->size == open_size; uvolní jej memory_driver_close_cb.
 */
static int memory_driver_open_cb ( st_HANDLER *h ) {
    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;
    if ( h->status & HANDLER_STATUS_READY ) { d->err = GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY; return EXIT_FAILURE; }
    if ( h->type != HANDLER_TYPE_MEMORY ) { d->err = GENERIC_DRIVER_ERROR_HANDLER_TYPE; return EXIT_FAILURE; }
    if ( memspec->ptr != NULL ) { d->err = GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY; return EXIT_FAILURE; }
    if ( memspec->open_size < 1 ) { d->err = GENERIC_DRIVER_ERROR_SIZE; return EXIT_FAILURE; }
    h->status = HANDLER_STATUS_NOT_READY;
    uint8_t *new_ptr = baseui_tools_mem_alloc0 ( memspec->open_size );
    if ( !new_ptr ) { d->err = GENERIC_DRIVER_ERROR_MALLOC; return EXIT_FAILURE; }
    memspec->ptr = new_ptr;
    memspec->size = memspec->open_size;
    h->status = HANDLER_STATUS_READY;
    return EXIT_SUCCESS;
}


/**
 * @brief Zavře paměťový handler - uvolní buffer.
 *
 * @param h otevřený paměťový handler
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE, pokud handler není připravený
 *         (bootstrap; buffer se pak neuvolní)
 *
 * @post Při úspěchu je buffer uvolněn (baseui_tools_mem_free),
 *       memspec->ptr == NULL, memspec->size == 0, chybové kódy vynulované
 *       a stav HANDLER_STATUS_NOT_READY. Všechny dříve vrácené ukazatele do
 *       bufferu jsou neplatné.
 */
static int memory_driver_close_cb ( st_HANDLER *h ) {
    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;
    baseui_tools_mem_free ( memspec->ptr );
    memspec->ptr = NULL;
    memspec->size = 0;
    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;
    h->status = HANDLER_STATUS_NOT_READY;
    return EXIT_SUCCESS;
}


/**
 * @brief Naplní callbacky obou globálních paměťových driverů.
 *
 * Viz deklarace v memory_driver.h.
 */
void memory_driver_init ( void ) {
    generic_driver_setup ( &g_memory_driver_static, memory_driver_open_cb, memory_driver_close_cb, memory_driver_read_cb, memory_driver_write_cb, memory_driver_prepare_static_cb, memory_driver_truncate_cb );
    generic_driver_setup ( &g_memory_driver_realloc, memory_driver_open_cb, memory_driver_close_cb, memory_driver_read_cb, memory_driver_write_cb, memory_driver_prepare_realloc_cb, memory_driver_truncate_cb );
}
