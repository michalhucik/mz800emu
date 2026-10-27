/**
 * @file   devbus_internal.h
 * @brief  Vnitřní stav jádra devbus sdílený jeho překladovými jednotkami.
 *
 * Jen pro devbus.c, devbus_cfg.c, devbus_snap.c, devbus_vout.c a
 * devbus_request.c. Zařízení a adaptéry používají jen devbus.h.
 *
 * @par Licence: GPLv3
 */

#ifndef DEVBUS_INTERNAL_H
#define DEVBUS_INTERNAL_H

#include "devbus.h"

#ifdef __cplusplus
extern "C" {
#endif

struct st_CFGMODULE;
struct st_DEVBUS_VOUT;

/**
 * @brief Stav jednoho registrovaného zařízení.
 *
 * @invariant desc != NULL pro sloty 0 .. g_devbus.count - 1;
 *            connected je 0 nebo 1; opt[i] je platná hodnota volby i;
 *            mmio_when[i] je podmnožina DEVBUS_MMIO_WHEN_MASK.
 * @note connected a opt[] jsou zároveň zdrojem pro uložení INI (save
 *       handler cfgfile čte unsigned bez zámku, jako u ostatních zařízení).
 */
typedef struct st_DEVBUS_SLOT {
    const st_DEVBUS_DEVICE_DESC *desc;          /**< Deskriptor (statický). */
    void *ctx;                                  /**< Kontext zařízení. */
    unsigned connected;                         /**< 1 = připojené; atomicky čtené z UI; save handler INI "connected". */
    unsigned ini_connected;                     /**< Hodnota "connected" načtená z INI (cíl propagate). */
    unsigned opt[DEVBUS_MAX_OPTIONS];           /**< Aktuální hodnoty voleb (propagate i save INI). */
    unsigned mmio_when[DEVBUS_MAX_MMIO_RANGES]; /**< Aktuální maska stavů MMIO rozsahů. */
    struct st_CFGMODULE *cfg_module;            /**< Sekce INI zařízení (vlastní g_cfgmain). */
    struct st_DEVBUS_VOUT *vout[DEVBUS_MAX_VOUTS]; /**< Sekundární výstupy zařízení (vlastní devbus_vout.c). */
    unsigned vout_count;                        /**< Počet výstupů. */
} st_DEVBUS_SLOT;

/**
 * @brief Aktivní MMIO rozsah (připojené zařízení, maska stavů != 0).
 */
typedef struct st_DEVBUS_MMIO_ACTIVE {
    uint16_t first;  /**< První adresa. */
    uint16_t last;   /**< Poslední adresa. */
    unsigned when;   /**< Maska stavů mapy. */
    void (*write)(void *ctx, uint16_t addr, uint8_t value, unsigned flags); /**< Handler. */
    void *ctx;       /**< Kontext zařízení. */
} st_DEVBUS_MMIO_ACTIVE;

/**
 * @brief Celý stav jádra devbus.
 * @invariant count <= DEVBUS_MAX_DEVICES; mmio_active_count <=
 *            DEVBUS_MAX_DEVICES * DEVBUS_MAX_MMIO_RANGES.
 */
typedef struct st_DEVBUS {
    bool initialized;                       /**< devbus_init() proběhl. */
    bool started;                           /**< devbus_start() proběhl, registr je neměnný. */
    const st_DEVBUS_ARCH_OPS *arch;         /**< Adaptér platformy. */
    st_DEVBUS_SLOT slot[DEVBUS_MAX_DEVICES];/**< Registr. */
    unsigned count;                         /**< Počet registrovaných zařízení. */
    unsigned connected_count;               /**< Počet připojených zařízení. */
    unsigned frame_done_count;              /**< Počet připojených zařízení s frame_done(). */
    st_DEVBUS_MMIO_ACTIVE mmio_active[DEVBUS_MAX_DEVICES * DEVBUS_MAX_MMIO_RANGES]; /**< Aktivní MMIO rozsahy. */
    unsigned mmio_active_count;             /**< Počet aktivních MMIO rozsahů. */
} st_DEVBUS;

/** @brief Stav jádra (emu vlákno; registr čitelný odkudkoli po devbus_start()). */
extern st_DEVBUS g_devbus;

/**
 * @brief Platný slot?
 * @return Ukazatel na slot, nebo NULL.
 */
st_DEVBUS_SLOT *devbus_slot_get(int slot);

/**
 * @brief Ověří hodnotu volby podle typu.
 * @return true = hodnota je platná.
 */
bool devbus_option_value_valid(const st_DEVBUS_OPTION *o, unsigned value);

/**
 * @brief Počet klíčových slov volby KEYWORD (0 u jiných typů).
 */
unsigned devbus_option_keyword_count(const st_DEVBUS_OPTION *o);

/**
 * @brief Založí (nebo znovu použije) sekci INI zařízení a načte ji.
 *
 * Elementy: "connected" (BOOL, výchozí 0; propagate do ini_connected,
 * save z connected) a volby (propagate i save do opt[]).
 *
 * @return DEVBUS_OK; DEVBUS_ERR_INVALID, když sekce už existuje a nezaložil
 *         ji devbus (kolize s jiným modulem), nebo bez g_cfgmain.
 * @post s->opt[] a s->ini_connected mají hodnoty z INI, jinak výchozí.
 */
int devbus_cfg_register(st_DEVBUS_SLOT *s);

/**
 * @brief Uvolní sekundární výstupy slotu (devbus_exit()).
 */
void devbus_vout_release_slot(st_DEVBUS_SLOT *s);

#ifdef __cplusplus
}
#endif

#endif /* DEVBUS_INTERNAL_H */
