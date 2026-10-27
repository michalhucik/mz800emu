/**
 * @file   mz1500_bus_owners.h
 * @brief  Statická tabulka vlastníků portů a mapovaných adres MZ-1500.
 *
 * Obsahově odpovídá větvím `case` v mz1500_iorq.c (port_read_cb,
 * port_write_cb) a mapovaným portům 0E000h-0E008h (mz1500_memory.c), se
 * stejnými podmínkami překladu (mz1500_config.h). Je zdrojem pro Bus Map
 * (bus_map.c); test tests/debugger/test_bus_map_static_mz1500.c hlídá
 * shodu s referenčním seznamem větví switche.
 *
 * Soubor nezávisí na emulátoru (jen mz1500_config.h a bus_owners.h), aby
 * šel přeložit STANDALONE; přítomnost periferií vyhodnocuje
 * mz1500_bus_owner_present() (definice v mz1500_iorq.c, jen build
 * s debuggerem).
 *
 * @par Licence: GPLv3
 */

#ifndef MZ1500_BUS_OWNERS_H
#define MZ1500_BUS_OWNERS_H

#include "hw-generic/devbus/bus_owners.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Druhy přítomnosti statických vlastníků MZ-1500 (present_kind).
 *
 * Vyhodnocuje mz1500_bus_owner_present() podle stavu periferií.
 */
typedef enum en_MZ1500_BUS_PRESENT {
    MZ1500_BUS_PRESENT_ALWAYS = BUS_PRESENT_ALWAYS, /**< Vždy (mapper, 8255, 8253, GDG, PSG, Z80 PIO). */
    MZ1500_BUS_PRESENT_CMTHACK,     /**< Zapnutý patch CMT hack (softwarová past, bez HW). */
    MZ1500_BUS_PRESENT_UNICARD,     /**< Připojený Unicard. */
    MZ1500_BUS_PRESENT_PEZIK68,     /**< Připojený RAM disk Pezik na 68h. */
    MZ1500_BUS_PRESENT_IDE8,        /**< Připojený IDE8 (master nebo slave). */
    MZ1500_BUS_PRESENT_FDC0,        /**< Připojený FDC 0. */
    MZ1500_BUS_PRESENT_FDC1,        /**< Připojený FDC 1. */
    MZ1500_BUS_PRESENT_RAMDISK_STD, /**< Připojený standardní RAM disk. */
    MZ1500_BUS_PRESENT_MEMEXT,      /**< Připojená paměťová expanze. */
    MZ1500_BUS_PRESENT_QDISK,       /**< Připojená jednotka Quick Disk. */
    MZ1500_BUS_PRESENT_COUNT        /**< Počet druhů. */
} en_MZ1500_BUS_PRESENT;

/**
 * @brief Řádky statické tabulky MZ-1500.
 * @param count Výstup počtu řádků (nesmí být NULL).
 * @return Pole řádků (statické).
 */
const st_BUS_STATIC_OWNER *mz1500_bus_owners(unsigned *count);

/**
 * @brief Obsluhuje port statický switch MZ-1500 (v libovolném směru)?
 * @param port_lsb Dolní bajt adresy portu.
 * @return true = port patří některému řádku tabulky (bez ohledu na přítomnost zařízení).
 */
bool mz1500_bus_port_is_static(uint8_t port_lsb);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * @brief Je statický vlastník druhu @p kind (en_MZ1500_BUS_PRESENT) právě přítomný?
 *
 * Lepidlo tabulky pro vrstvu bus_map: čte stav periferií (Unicard, FDC,
 * IDE8, RAM disky, Quick Disk, MemExt, CMT hack). Definice je
 * v mz1500_iorq.c (má všechny hlavičky periferií), jen v buildu
 * s debuggerem.
 *
 * @param kind Druh přítomnosti (en_MZ1500_BUS_PRESENT).
 * @return true = na sběrnici je; neznámý druh = false.
 * @pre Emu vlákno, nebo UI pod device_state_lock() (jen čtení příznaků).
 */
bool mz1500_bus_owner_present(unsigned kind);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MZ1500_BUS_OWNERS_H */
