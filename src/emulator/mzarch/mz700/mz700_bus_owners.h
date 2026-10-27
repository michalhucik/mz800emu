/**
 * @file   mz700_bus_owners.h
 * @brief  Statická tabulka vlastníků portů a mapovaných adres MZ-700.
 *
 * Obsahově odpovídá větvím `case` v mz700_iorq.c (port_read_cb,
 * port_write_cb) a mapovaným portům 0E000h-0E008h (mz700_memory.c), se
 * stejnými podmínkami překladu (mz700_config.h). Je zdrojem pro validaci
 * portů devbus (mz700_devbus_port_is_static()) i pro Bus Map; test
 * tests/debugger/test_bus_map_static.c hlídá shodu s referenčním seznamem
 * větví switche.
 *
 * Soubor nezávisí na emulátoru (jen mz700_config.h a bus_owners.h), aby
 * šel přeložit STANDALONE; přítomnost periferií vyhodnocuje
 * mz700_bus_owner_present() v adaptéru devbus.
 *
 * @par Licence: GPLv3
 */

#ifndef MZ700_BUS_OWNERS_H
#define MZ700_BUS_OWNERS_H

#include "hw-generic/devbus/bus_owners.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Druhy přítomnosti statických vlastníků MZ-700 (present_kind).
 *
 * Vyhodnocuje mz700_bus_owner_present() podle stavu periferií.
 */
typedef enum en_MZ700_BUS_PRESENT {
    MZ700_BUS_PRESENT_ALWAYS = BUS_PRESENT_ALWAYS, /**< Vždy (mapper, 8255, 8253). */
    MZ700_BUS_PRESENT_CMTHACK,     /**< Zapnutý patch CMT hack (softwarová past, bez HW). */
    MZ700_BUS_PRESENT_UNICARD,     /**< Připojený Unicard. */
    MZ700_BUS_PRESENT_PEZIK68,     /**< Připojený RAM disk Pezik na 68h. */
    MZ700_BUS_PRESENT_IDE8,        /**< Připojený IDE8 (master nebo slave). */
    MZ700_BUS_PRESENT_FDC0,        /**< Připojený FDC 0. */
    MZ700_BUS_PRESENT_FDC1,        /**< Připojený FDC 1. */
    MZ700_BUS_PRESENT_RAMDISK_STD, /**< Připojený standardní RAM disk. */
    MZ700_BUS_PRESENT_MEMEXT,      /**< Připojená paměťová expanze. */
    MZ700_BUS_PRESENT_QDISK,       /**< Připojená jednotka Quick Disk. */
    MZ700_BUS_PRESENT_PIOZ80,      /**< Z80 PIO (v sestavě MZ-700 není, HAVE_PIOZ80 = 0). */
    MZ700_BUS_PRESENT_COUNT        /**< Počet druhů. */
} en_MZ700_BUS_PRESENT;

/**
 * @brief Řádky statické tabulky MZ-700.
 * @param count Výstup počtu řádků (nesmí být NULL).
 * @return Pole řádků (statické).
 */
const st_BUS_STATIC_OWNER *mz700_bus_owners(unsigned *count);

/**
 * @brief Obsluhuje port statický switch MZ-700 (v libovolném směru)?
 * @param port_lsb Dolní bajt adresy portu.
 * @return true = port patří některému řádku tabulky (bez ohledu na přítomnost zařízení).
 */
bool mz700_bus_port_is_static(uint8_t port_lsb);

#ifdef __cplusplus
}
#endif

#endif /* MZ700_BUS_OWNERS_H */
