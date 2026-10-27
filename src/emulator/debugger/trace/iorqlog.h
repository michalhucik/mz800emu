/**
 * @file   iorqlog.h
 * @brief  IORQ Log - každý IORQ + MREQ-mapped port + IORQ-unconnected (ghost).
 *
 * Subsystém trace-suite, který loguje I/O bus traffic. Rozsah:
 *  - IORQ IN / OUT (každý port přístup)
 *  - MREQ pro mapped porty (0xE000-0xE008 v MZ-700 mode s horní ROM) -
 *    V1 částečně (Fáze 3.1)
 *  - IORQ unconnected = "ghost bus read" (port není mapovaný, čte se
 *    "duch" hodnota z bus latch)
 *
 * Per-event záznam (24 B fixed):
 *
 * | Offset | Velikost | Pole                                          |
 * |--------|----------|------------------------------------------------|
 * | 0      | 8 B      | pxclk_total                                    |
 * | 8      | 4 B      | screens_total                                  |
 * | 12     | 4 B      | pxclk_in_screen                                |
 * | 16     | 1 B      | event_type (IORQ / MREQ / IORQ_UNCONNECTED / IORQ_CONFLICT) |
 * | 17     | 1 B      | direction (IN / OUT)                           |
 * | 18     | 2 B      | source_addr (regBC pro IORQ, regPC pro MREQ)   |
 * | 20     | 2 B      | port_or_addr                                   |
 * | 22     | 1 B      | value                                          |
 * | 23     | 1 B      | pulse_duration_pxclk (s WAIT delší)            |
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#ifndef IORQLOG_H
#define IORQLOG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "tlog_common.h"

/**
 * @brief Typ I/O události.
 */
typedef enum en_IORQLOG_EVENT_TYPE
{
    IORQLOG_EVENT_IORQ = 0,           /**< Standard IORQ na mapovaný port */
    IORQLOG_EVENT_MREQ_MAPPED,        /**< MREQ na 0xE000-0xE008 (MZ-700 mode) */
    IORQLOG_EVENT_IORQ_UNCONNECTED,   /**< IORQ na nemapovaný port (ghost read) */
    IORQLOG_EVENT_IORQ_CONFLICT,      /**< IORQ s kolizí dvou budičů (devbus, politika AND / FIRST) */
} en_IORQLOG_EVENT_TYPE;

/**
 * @brief Směr I/O operace.
 */
typedef enum en_IORQLOG_DIRECTION
{
    IORQLOG_DIR_IN = 0,                /**< CPU IN (read z portu) */
    IORQLOG_DIR_OUT,                   /**< CPU OUT (write na port) */
} en_IORQLOG_DIRECTION;

/**
 * @brief Konfigurace iorqlog subsystému (paralelní s cputrack).
 */
typedef struct st_IORQLOG_CONFIG
{
    en_TLOG_MODE mode;
    char *dir;
    char *name;
    unsigned chunk_mb;
    unsigned max_total_mb;
    unsigned save_on_exit;
} st_IORQLOG_CONFIG;

extern st_IORQLOG_CONFIG g_iorqlog_config;
extern int g_iorqlog_active;

#define TEST_TRACE_IORQLOG_ACTIVE (g_iorqlog_active)

/**
 * @brief Globální flag - aktuální IORQ čtení/zápis cílí na nemapovaný port.
 *
 * Lifecycle:
 *  - @c port_read_with_logging_cb / @c port_write_with_logging_cb nuluje
 *    flag na vstupu (před voláním @c port_read_cb / @c port_write_cb).
 *  - @c port_read_cb v @c default case + ve všech "X.connected = false"
 *    větvích nastavuje flag na 1 (= z portu jsme přečetli "ducha sběrnice"
 *    z @c regDBUS_latch, žádný chip neodpověděl).
 *  - @c port_write_cb pre-set 1 na vstupu, každá "handled" větev (chip
 *    skutečně write přijal) nuluje na 0.
 *  - Při návratu do @c *_with_logging_cb caller čte flag a podle něj emit
 *    @c IORQLOG_EVENT_IORQ_UNCONNECTED místo @c IORQLOG_EVENT_IORQ.
 *
 * Funkční jen v jednovláknové emulační smyčce. Default 0.
 */
extern uint8_t g_tracelog_iorq_unconnected;

/**
 * @brief Označí aktuálně probíhající IORQ jako nemapovaný (ghost bus).
 *
 * Volat v @c port_read_cb / @c port_write_cb na všech místech kde je
 * detekováno čtení/zápis na port, který není v aktuální HW konfiguraci
 * obsloužen žádným chipem (= read vrací @c regDBUS_latch, write je no-op).
 *
 * Lehký no-op assignment (1 store) - bez větvení, branch predictor friendly.
 */
#define TRACELOG_IORQ_MARK_UNCONNECTED()  do { g_tracelog_iorq_unconnected = 1; } while (0)

/**
 * @brief Označí aktuálně probíhající IORQ jako handled (chip přijal write).
 *
 * Inverzní marker k @c TRACELOG_IORQ_MARK_UNCONNECTED. Používá se na
 * write side, kde je strukturálně pohodlnější předem nastavit
 * "unconnected" v @c port_write_with_logging_cb a v každé "chip přijal"
 * větvi v @c port_write_cb explicitně označit handled (= reset flagu).
 * Default no-action / non-existující chip větve flag nemění.
 *
 * Ekvivalent na write side k pre-clear strategii read side.
 */
#define TRACELOG_IORQ_MARK_HANDLED()      do { g_tracelog_iorq_unconnected = 0; } while (0)

/**
 * @brief Globální flag - aktuální IORQ čtení skončilo kolizí dvou budičů.
 *
 * Nastavuje adaptér devbus (st_DEVBUS_ARCH_OPS::bus_conflict) při rezoluci
 * čtení s politikou AND / FIRST; @c port_read_with_logging_cb ho nuluje
 * na vstupu a při návratu podle něj vybere @c IORQLOG_EVENT_IORQ_CONFLICT
 * (má přednost před UNCONNECTED). Jen emu vlákno. Default 0.
 */
extern uint8_t g_tracelog_iorq_conflict;

/** @brief Označí aktuálně probíhající IORQ jako kolizi budičů (viz g_tracelog_iorq_conflict). */
#define TRACELOG_IORQ_MARK_CONFLICT()     do { g_tracelog_iorq_conflict = 1; } while (0)

void iorqlog_init ( void );
void iorqlog_apply_cli_options ( void );
void iorqlog_recompute_active ( int debugger_active );
int  iorqlog_start ( void );
void iorqlog_stop ( void );
void iorqlog_finalize ( void );

/**
 * @brief Uzavřít (uložit) aktuální segment, volitelně přesměrovat na @p path.
 *
 * Runtime finalizace streamovaného iorqlog segmentu (analogie
 * @ref cputrack_save_segment). Při zadaném @p path přesměruje @c dir/name na
 * cestu pro NÁSLEDNÝ segment; již zapsané chunk soubory nepřejmenovává.
 *
 * @param path  Cílová cesta dalšího segmentu, nebo NULL (jen flush+restart).
 * @return 0 OK, -1 chyba.
 */
int iorqlog_save_segment ( const char *path );

/**
 * @brief Test, zda byl recording zastaven kvůli max_total_mb limitu.
 *
 * Public wrapper nad @ref tlog_writer_is_truncated() pro interní
 * @c s_writer instanci tohoto subsystému. Použití typ. v UI status
 * indicators (např. titulek SDL okna).
 *
 * @return 1 = truncated (recording stopnut), 0 = stále aktivní recording
 *         nebo neaktivní subsystém.
 */
int iorqlog_is_truncated ( void );

/**
 * @brief Hot-path hook - emit eventu po I/O operaci.
 *
 * Volat z `port_read_with_logging_cb` a `port_write_with_logging_cb`
 * (pokud je iorqlog aktivní). Hodnota a pulse_duration_pxclk se
 * předají z volajícího (caller ví zda byl WAIT).
 */
void iorqlog_record ( en_IORQLOG_EVENT_TYPE etype,
                      en_IORQLOG_DIRECTION dir,
                      uint16_t source_addr, uint16_t port_or_addr,
                      uint8_t value, uint8_t pulse_duration_pxclk );

/**
 * @brief Writer kanálu iorqlog pro čtení proudu záznamů (trace_read).
 *
 * Vrací writer i po stopu (popis posledního uzavřeného segmentu), viz
 * tlog_writer_read_snapshot() a devdoc/trace/stream-read.md.
 *
 * @return Ukazatel na statický writer modulu (nikdy NULL).
 * @pre Snímek writeru pořizovat jen z emu vlákna; tlog_writer_segment()
 *      z libovolného vlákna.
 */
const st_TLOG_WRITER *iorqlog_get_writer ( void );

#ifdef __cplusplus
}
#endif

#endif /* IORQLOG_H */
