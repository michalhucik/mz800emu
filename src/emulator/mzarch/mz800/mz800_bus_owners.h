/**
 * @file   mz800_bus_owners.h
 * @brief  Statická tabulka vlastníků portů a mapovaných adres MZ-800.
 *
 * Obsahově odpovídá větvím `case` v mz800_iorq.c (port_read_cb,
 * port_write_cb) a mapovaným portům 0E000h-0E008h v režimu MZ-700
 * (mz800_memory.c), se stejnými podmínkami překladu (mz800_config.h).
 * Je zdrojem pro Bus Map (bus_map.c); test
 * tests/debugger/test_bus_map_static_mz800.c hlídá shodu s referenčním
 * seznamem větví switche.
 *
 * Soubor nezávisí na emulátoru (jen mz800_config.h a bus_owners.h), aby
 * šel přeložit STANDALONE; přítomnost periferií a stav režimu DMD
 * vyhodnocuje mz800_bus_owner_present() (definice v mz800_iorq.c, jen
 * build s debuggerem).
 *
 * @par Licence: GPLv3
 */

#ifndef MZ800_BUS_OWNERS_H
#define MZ800_BUS_OWNERS_H

#include "hw-generic/devbus/bus_owners.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Druhy přítomnosti statických vlastníků MZ-800 (present_kind).
 *
 * Vyhodnocuje mz800_bus_owner_present() podle stavu periferií, režimu DMD
 * a voleb HW kompatibility.
 */
typedef enum en_MZ800_BUS_PRESENT {
    MZ800_BUS_PRESENT_ALWAYS = BUS_PRESENT_ALWAYS, /**< Vždy (GDG, mapper, Z80 PIO, joystick, PSG 0F2h). */
    MZ800_BUS_PRESENT_CMTHACK,     /**< Zapnutý patch CMT hack (softwarová past, bez HW). */
    MZ800_BUS_PRESENT_UNICARD,     /**< Připojený Unicard. */
    MZ800_BUS_PRESENT_PEZIK68,     /**< Připojený RAM disk Pezik na 68h. */
    MZ800_BUS_PRESENT_PEZIK_E8,    /**< Připojený RAM disk Pezik na 0E8h. */
    MZ800_BUS_PRESENT_IDE8,        /**< Připojený IDE8 (master nebo slave). */
    MZ800_BUS_PRESENT_FDC0,        /**< Připojený FDC 0. */
    MZ800_BUS_PRESENT_FDC1,        /**< Připojený FDC 1. */
    MZ800_BUS_PRESENT_RAMDISK_STD, /**< Připojený standardní RAM disk. */
    MZ800_BUS_PRESENT_MEMEXT,      /**< Připojená paměťová expanze. */
    MZ800_BUS_PRESENT_QDISK,       /**< Připojená jednotka Quick Disk. */
    MZ800_BUS_PRESENT_MODE800,     /**< DMD v režimu MZ-800: 8255 a 8253 na portech 0D0h-0D7h. */
    MZ800_BUS_PRESENT_MODE700,     /**< DMD v režimu MZ-700: mapované porty 0E000h-0E008h. */
    MZ800_BUS_PRESENT_PSG_STEREO,  /**< Druhý PSG (volba HW kompatibility allow_psg1): porty 0F3h, 0F9h. */
    MZ800_BUS_PRESENT_COUNT        /**< Počet druhů. */
} en_MZ800_BUS_PRESENT;

/**
 * @brief Řádky statické tabulky MZ-800.
 * @param count Výstup počtu řádků (nesmí být NULL).
 * @return Pole řádků (statické).
 */
const st_BUS_STATIC_OWNER *mz800_bus_owners(unsigned *count);

/**
 * @brief Obsluhuje port statický switch MZ-800 (v libovolném směru)?
 * @param port_lsb Dolní bajt adresy portu.
 * @return true = port patří některému řádku tabulky (bez ohledu na přítomnost zařízení a režim DMD).
 */
bool mz800_bus_port_is_static(uint8_t port_lsb);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * @brief Je statický vlastník druhu @p kind (en_MZ800_BUS_PRESENT) právě přítomný?
 *
 * Lepidlo tabulky pro vrstvu bus_map: čte stav periferií (Unicard, FDC,
 * IDE8, RAM disky, Quick Disk, MemExt, CMT hack), režim DMD (8255 a 8253
 * na portech jen v režimu MZ-800, mapované porty jen v režimu MZ-700)
 * a volbu druhého PSG. Definice je v mz800_iorq.c (má všechny hlavičky
 * periferií), jen v buildu s debuggerem.
 *
 * @param kind Druh přítomnosti (en_MZ800_BUS_PRESENT).
 * @return true = na sběrnici je; neznámý druh = false.
 * @pre Emu vlákno, nebo UI pod device_state_lock() (jen čtení příznaků).
 */
bool mz800_bus_owner_present(unsigned kind);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MZ800_BUS_OWNERS_H */
