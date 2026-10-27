/**
 * @file   devbus_dummy.h
 * @brief  Testovací zařízení devbus (jen v testech, nikdy v emulátoru).
 *
 * Zařízení "dummy" využívá všechny druhy háčků devbus: porty 74h-75h
 * (registr a čítač čtení), MMIO 0E013h (stav MAPPED) a 0E014h (stav DRAM),
 * region 256 B, volbu každého typu (BOOL LIVE, KEYWORD LIVE, UNSIGNED
 * CONNECT), video háčky (volitelně), snapshot verze 2 s loaderem verze 1.
 * Každý callback počítá svá volání, aby testy ověřily životní cyklus.
 *
 * @par Licence: GPLv3
 */

#ifndef DEVBUS_DUMMY_H
#define DEVBUS_DUMMY_H

#include <stdint.h>
#include <stdbool.h>

#include "hw-generic/devbus/devbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Index volby BOOL "flag" (LIVE). */
#define DUMMY_OPT_FLAG  0
/** @brief Index volby KEYWORD "mode" (LIVE; "off", "rising", "level"). */
#define DUMMY_OPT_MODE  1
/** @brief Index volby UNSIGNED "fill" (CONNECT, 0-255, výchozí 0AAh = obsah RAM po zapnutí). */
#define DUMMY_OPT_FILL  2

/** @brief Velikost RAM (regionu) dummy zařízení. */
#define DUMMY_RAM_SIZE 256

/**
 * @brief Stav a čítače dummy zařízení (kontext callbacků).
 *
 * @invariant Čítače jen rostou; vynuluje je devbus_dummy_reset_counters().
 */
typedef struct st_DEVBUS_DUMMY {
    uint8_t reg;                 /**< Registr na portu 74h (zápis / čtení). */
    uint8_t reads;               /**< Čítač čtení portu 75h (vedlejší efekt čtení). */
    uint32_t extra;              /**< Pole přidané ve verzi 2 stavu (verze 1 = výchozí 0x5A). */
    uint8_t ram[DUMMY_RAM_SIZE]; /**< RAM zařízení (region). */
    bool fail_connect;           /**< true = connect() vrátí chybu. */
    bool fail_init;              /**< true = init() vrátí chybu. */
    unsigned wait_on_write;      /**< T-stavy WAIT při zápisu na 74h (devbus_cpu_wait). */
    int slot;                    /**< Slot z devbus_register() (nastaví test); connect() podle něj čte volbu "fill". */

    unsigned n_init, n_connect, n_reset, n_disconnect, n_destroy; /**< Čítače životního cyklu. */
    unsigned n_option_changed, n_frame_done, n_after_load, n_rom_changed; /**< Čítače událostí. */
    unsigned last_opt, last_opt_value;       /**< Poslední option_changed(). */
    unsigned n_mmio;                         /**< Počet pozorovaných MMIO zápisů. */
    uint16_t mmio_addr;                      /**< Poslední MMIO adresa. */
    uint8_t mmio_value;                      /**< Poslední MMIO hodnota. */
    unsigned mmio_flags;                     /**< Poslední MMIO příznaky. */
    unsigned n_region_write;                 /**< Počet on_write() regionu. */
    unsigned n_row;                          /**< Počet render_row(). */
} st_DEVBUS_DUMMY;

/**
 * @brief Vyplní deskriptor dummy zařízení se všemi háčky.
 *
 * @param out     Cílový deskriptor (musí žít do konce procesu - static).
 * @param id      Id zařízení (statický řetězec).
 * @param section Sekce INI (statický řetězec).
 * @param video   0 = bez video háčků, 1 = glyph_source, 2 = render_row.
 * @post Testy smějí pole deskriptoru po vyplnění upravit (např. ports = NULL).
 */
void devbus_dummy_make_desc(st_DEVBUS_DEVICE_DESC *out, const char *id, const char *section, int video);

/** @brief Vynuluje čítače a stav (ne příznaky fail_* a slot). */
void devbus_dummy_reset_counters(st_DEVBUS_DUMMY *d);

#ifdef __cplusplus
}
#endif

#endif /* DEVBUS_DUMMY_H */
