/**
 * @file   bus_map.h
 * @brief  Bus Map - pohled "kdo vlastní adresu" přes statický switch i devbus (arch-neutrální).
 *
 * Skládá jeden snímek pro okno Bus Map a MCP get_bus_map: za každý port
 * 00h-0FFh a každou adresu okna MMIO 0E000h-0E0FFh seznam vlastníků
 * (statické čipy desky z tabulky platformy bus_owners.h, zařízení devbus
 * z deskriptorů), jejich směr, výstupní stupeň, přítomnost, příznaky
 * (nemapováno, různí vlastníci podle směru, potenciální kolize, viděná
 * kolize, viděný ghost přístup) a živou aktivitu z io_activity.
 *
 * Snímek je odvozený read-only pohled: nic nemění, horké cesty se
 * nedotýká (aktivitu plní stávající logovací callbacky za
 * g_io_window_tracking_active). Peek hodnoty v snímku nejsou; debugger
 * je čte zvlášť pro vybranou adresu (DBGAPI_CMD_BUS_PEEK).
 *
 * Statickou tabulku mají všechny tři platformy (mz700_bus_owners.c,
 * mz800_bus_owners.c, mz1500_bus_owners.c); zařízení devbus jen MZ-700.
 *
 * Vlákna: bus_map_snapshot() volá UI pod device_state_lock() nebo emu
 * vlákno (dbgapi). Čte registr devbus (neměnný), připojení (atomicky),
 * tabulku io_activity (bez zámku, nejvýše stale o snímek) a stav
 * periferií (příznaky connected, jen čtení).
 *
 * @par Licence: GPLv3
 */

#ifndef BUS_MAP_H
#define BUS_MAP_H

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>
#include <stdbool.h>

#include "hw-generic/devbus/devbus.h"
#include "hw-generic/devbus/bus_owners.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Nejvyšší počet vlastníků jedné adresy ve snímku. */
#define BUS_MAP_MAX_OWNERS 4
/** @brief Nejvyšší počet zařízení v seznamu snímku (statické čipy + devbus). */
#define BUS_MAP_MAX_DEVICES 24
/** @brief První adresa okna MMIO ve snímku (buňky mmio[0..255] = 0E000h-0E0FFh). */
#define BUS_MAP_MMIO_FIRST 0xE000u

/** @brief Druh vlastníka. */
typedef enum en_BUS_OWNER_KIND {
    BUS_OWNER_STATIC = 0, /**< Čip nebo periferie základní desky (statický switch). */
    BUS_OWNER_DEVBUS = 1, /**< Volitelné zařízení devbus (registrované; connected říká, zda je připojené). */
} en_BUS_OWNER_KIND;

/** @brief Příznak portu / buňky: žádný přítomný vlastník (čtení vrací nebuzenou sběrnici). */
#define BUS_PORT_UNMAPPED           (1u << 0)
/** @brief Příznak: čtení a zápis obsluhují různí vlastníci. */
#define BUS_PORT_DIR_SPLIT          (1u << 1)
/** @brief Příznak: dva budiče (i odpojený registrovaný) na týchž bitech - připojení by s REFUSE selhalo, nebo už koexistují. */
#define BUS_PORT_POTENTIAL_CONFLICT (1u << 2)
/** @brief Příznak: za běhu došlo k rezoluci s kolizí (čítač devbus nebo aktivity). */
#define BUS_PORT_CONFLICT_SEEN      (1u << 3)
/** @brief Příznak: program sáhl na adresu bez vlastníka (ghost přístup v aktivitě). */
#define BUS_PORT_GHOST_SEEN         (1u << 4)

/**
 * @brief Jeden vlastník adresy ve snímku.
 * @invariant id, name nenulové (statické řetězce platformy nebo deskriptoru);
 *            abbrev 1-2 znaky; drive_mask je efektivní maska (devbus_drive_mask()).
 */
typedef struct st_BUS_OWNER {
    const char *id;           /**< Stabilní id ("ppi8255", "kp80"). */
    const char *name;         /**< Anglický název (msgid). */
    char abbrev[4];           /**< Zkratka do mřížky (1-2 znaky). */
    uint8_t kind;             /**< en_BUS_OWNER_KIND. */
    int8_t devbus_slot;       /**< Slot devbus, -1 u statických. */
    uint8_t dirs;             /**< BUS_DIR_R | BUS_DIR_W. */
    uint8_t connected;        /**< 1 = přítomný (statický: periferie připojená; devbus: zařízení připojené). */
    en_DEVBUS_DRIVE drive;    /**< Výstupní stupeň při čtení. */
    uint8_t drive_mask;       /**< Efektivní maska buzených bitů (0 u NONE / UNKNOWN). */
    uint16_t range_first;     /**< Začátek rozsahu, do kterého adresa patří. */
    uint16_t range_last;      /**< Konec rozsahu. */
    uint8_t when_mask;        /**< Jen MMIO: DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM, kdy vlastník dekóduje. */
    const char *source_note;  /**< Zdroj deklarace budiče, nebo NULL (devbus bez poznámky). */
    const char *mirror_note;  /**< Zrcadla podle báze, NULL = neověřeno. */
} st_BUS_OWNER;

/**
 * @brief Jeden port 00h-0FFh.
 *
 * Aktivita je agregát přes všech 256 horních bajtů adresy (OUT (C),r
 * s libovolným B), platná jen při tracking_active a want
 * BUS_WANT_ACTIVITY; conflict_hits sčítá čítač devbus (bez sledování)
 * a čítač aktivity.
 */
typedef struct st_BUS_PORT {
    st_BUS_OWNER owners[BUS_MAP_MAX_OWNERS]; /**< Vlastníci (statické první, pak devbus v pořadí slotů). */
    uint8_t owner_count;      /**< Počet vlastníků. */
    uint8_t flags;            /**< BUS_PORT_*. */
    uint8_t last_read_valid;  /**< 1 = last_read_value platí. */
    uint8_t last_write_valid; /**< 1 = last_write_value platí. */
    uint8_t last_read_value;  /**< Poslední hodnota IN. */
    uint8_t last_write_value; /**< Poslední hodnota OUT. */
    uint32_t last_read_age;   /**< Stáří posledního IN ve snímcích. */
    uint32_t last_write_age;  /**< Stáří posledního OUT ve snímcích. */
    uint32_t hits_in_per_sec; /**< IN za sekundu (klouzavé okno). */
    uint32_t hits_out_per_sec;/**< OUT za sekundu. */
    uint64_t total_in;        /**< IN od resetu aktivity. */
    uint64_t total_out;       /**< OUT od resetu aktivity. */
    uint64_t ghost_hits;      /**< Přístupy bez vlastníka. */
    uint64_t conflict_hits;   /**< Čtení s kolizí budičů. */
} st_BUS_PORT;

/**
 * @brief Jedna adresa okna MMIO 0E000h-0E0FFh.
 */
typedef struct st_BUS_MMIO_CELL {
    uint16_t addr;            /**< Adresa. */
    st_BUS_OWNER owners[BUS_MAP_MAX_OWNERS]; /**< Vlastníci. */
    uint8_t owner_count;      /**< Počet vlastníků. */
    uint8_t flags;            /**< BUS_PORT_* (bez GHOST_SEEN). */
    uint8_t when_mask;        /**< Sjednocení when_mask vlastníků. */
    uint8_t last_write_valid; /**< 1 = last_write_value platí. */
    uint8_t last_write_value; /**< Poslední zapsaná hodnota (sledování MMIO zápisů). */
    uint32_t last_write_age;  /**< Stáří ve snímcích. */
    uint32_t hits_out_per_sec;/**< Zápisy za sekundu. */
} st_BUS_MMIO_CELL;

/**
 * @brief Řádek záložky Devices (jeden statický čip nebo jedno zařízení devbus).
 */
typedef struct st_BUS_DEVICE {
    const char *id;           /**< Id. */
    const char *name;         /**< Anglický název. */
    uint8_t kind;             /**< en_BUS_OWNER_KIND. */
    int8_t devbus_slot;       /**< Slot devbus, -1 u statických. */
    uint8_t connected;        /**< 1 = přítomné / připojené. */
    uint8_t port_count;       /**< Počet portů (dolní bajty), které obsluhuje. */
    uint8_t mmio_count;       /**< Počet MMIO adres v okně 0E000h-0E0FFh. */
    en_DEVBUS_DRIVE drive;    /**< Nejsilnější deklarovaný budič (NONE < OC < PARTIAL < TRISTATE; UNKNOWN zvlášť). */
    uint8_t has_glyph;        /**< Devbus: glyph_source. */
    uint8_t has_row;          /**< Devbus: render_row. */
    uint8_t region_count;     /**< Devbus: regiony paměti. */
    uint8_t vout_count;       /**< Devbus: sekundární výstupy. */
} st_BUS_DEVICE;

/** @brief Co má snímek obsahovat: porty. */
#define BUS_WANT_PORTS    (1u << 0)
/** @brief Co má snímek obsahovat: okno MMIO. */
#define BUS_WANT_MMIO     (1u << 1)
/** @brief Co má snímek obsahovat: seznam zařízení. */
#define BUS_WANT_DEVICES  (1u << 2)
/** @brief Co má snímek obsahovat: živou aktivitu (jinak jen vlastníci a příznaky z čítačů devbus). */
#define BUS_WANT_ACTIVITY (1u << 3)
/** @brief Vše. */
#define BUS_WANT_ALL      (BUS_WANT_PORTS | BUS_WANT_MMIO | BUS_WANT_DEVICES | BUS_WANT_ACTIVITY)

/**
 * @brief Celý snímek Bus Map.
 * @invariant ports[p] platí po BUS_WANT_PORTS, mmio[i] po BUS_WANT_MMIO,
 *            devices po BUS_WANT_DEVICES; ostatní jsou vynulované.
 */
typedef struct st_BUS_MAP_SNAPSHOT {
    unsigned platform;            /**< 700 / 800 / 1500 (MZARCH). */
    bool supported;               /**< Statická tabulka platformy existuje (MZ-700, MZ-800, MZ-1500). */
    bool devbus_initialized;      /**< devbus na platformě běží. */
    en_DEVBUS_CONFLICT conflict_policy; /**< Aktuální politika kolize. */
    en_DEVBUS_IDLE bus_idle;      /**< Model nebuzené sběrnice. */
    uint8_t bus_idle_value;       /**< Hodnota pro CONST. */
    uint8_t idle_byte;            /**< Aktuální hodnota nebuzených bitů (latch nebo konstanta). */
    bool tracking_active;         /**< Sledování aktivity běží (hodnoty aktivity jsou živé). */
    bool mmio_mapped_now;         /**< Horní oblast mapovaná (porty 0E000h+ dostupné; MZ-1500 se SPEC = 0). */
    uint32_t frame;               /**< Číslo snímku obrazu, ze kterého je snímek. */
    unsigned devbus_registered;   /**< Počet registrovaných zařízení devbus. */
    unsigned devbus_connected;    /**< Počet připojených. */
    st_BUS_PORT ports[256];       /**< Porty podle dolního bajtu. */
    st_BUS_MMIO_CELL mmio[256];   /**< Okno MMIO 0E000h-0E0FFh. */
    st_BUS_DEVICE devices[BUS_MAP_MAX_DEVICES]; /**< Zařízení. */
    unsigned device_count;        /**< Počet zařízení. */
} st_BUS_MAP_SNAPSHOT;

/**
 * @brief Má platforma statickou tabulku vlastníků (okno Bus Map se zobrazuje)?
 * @return true na MZ-700, MZ-800 i MZ-1500.
 */
bool bus_map_supported(void);

/**
 * @brief Statická tabulka vlastníků aktuální platformy.
 * @return Tabulka (rows NULL a count 0 bez podpory); statická.
 */
const st_BUS_STATIC_TABLE *bus_map_static_table(void);

/**
 * @brief Složí snímek Bus Map.
 *
 * @param out  Cíl (velký, cca 140 kB: statická instance nebo heap); vynuluje se.
 * @param want Maska BUS_WANT_*.
 * @pre UI vlákno pod device_state_lock(), nebo emu vlákno (dbgapi).
 * @post Vyplněné části podle @p want; hlavička (platform, policy, idle,
 *       frame, tracking) vždy.
 */
void bus_map_snapshot(st_BUS_MAP_SNAPSHOT *out, unsigned want);

/**
 * @brief Jméno příznaku pro MCP ("unmapped", "dir_split", "potential_conflict", "conflict_seen", "ghost_seen").
 *
 * Inline v hlavičce, aby ho mohl použít i STANDALONE test dispatch MCP bez
 * jádra bus_map.
 *
 * @param flag Jeden bit BUS_PORT_*.
 * @return Statický řetězec, nebo NULL pro neznámý bit.
 */
static inline const char *bus_map_flag_name(unsigned flag)
{
    switch (flag) {
    case BUS_PORT_UNMAPPED:           return "unmapped";
    case BUS_PORT_DIR_SPLIT:          return "dir_split";
    case BUS_PORT_POTENTIAL_CONFLICT: return "potential_conflict";
    case BUS_PORT_CONFLICT_SEEN:      return "conflict_seen";
    case BUS_PORT_GHOST_SEEN:         return "ghost_seen";
    default:                          return NULL;
    }
}

/**
 * @brief Budí při čtení portu @p port_lsb některý přítomný čip desky?
 *
 * Řádek statické tabulky se směrem čtení, s budičem jiným než
 * DEVBUS_DRIVE_NONE (UNKNOWN se počítá jako budič) a právě přítomný
 * (bus_present_fn). Používá peek portu (DBGAPI_CMD_BUS_PEEK): zrcadla
 * katalogu I/O se čtou jen pro port, který deska na této platformě
 * a v tomto režimu skutečně dekóduje; jinak platí model nebuzené
 * sběrnice.
 *
 * @param port_lsb Dolní bajt adresy portu.
 * @return true = deska budí.
 * @pre Jako bus_map_snapshot() (čte stav periferií).
 */
bool bus_map_static_port_driven(uint8_t port_lsb);

/**
 * @brief Zkratka vlastníka devbus z id (první písmeno velké + první číslice nebo druhé písmeno).
 * @param id  Id zařízení.
 * @param out Výstup (nejméně 3 B).
 */
void bus_map_abbrev_from_id(const char *id, char *out);

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* BUS_MAP_H */
