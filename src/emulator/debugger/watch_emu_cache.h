/*
 * File:   watch_emu_cache.h
 *
 * EMU-side thread-safe zrcadlo Watch panel stavů pro MCP dispatch.
 *
 * Účel:
 *   Modul `watch_cache.{c,h}` je single-threaded (= jen UI vlákno). MCP
 *   dispatch vlákno potřebuje číst stejné statistiky (snap baseline,
 *   current, delta, min, max, change_count) pro Resource
 *   `emulator://watch/snapshot/{name}`. Tento modul drží thread-safe
 *   mirror, do kterého UI vlákno jednou per frame publikuje aktuální stav
 *   řádku, a dispatch vlákno z něj čte (= 1-frame stale, ale safe).
 *
 *   Mirror je klíčován **jménem** řádku (= string), aby resource URI
 *   `emulator://watch/snapshot/{name}` měl deterministický lookup nezávislý
 *   na pořadí řádků v `watch.c` storage.
 *
 * Threading:
 *   - `watch_emu_cache_publish(...)` volá UI vlákno (= watch render loop)
 *     po každém `watch_cache_update` per řádek.
 *   - `watch_emu_cache_get_by_name(...)` volá dispatch vlákno z MCP
 *     handleru.
 *   - Interní GMutex chrání hash table před race condition.
 *   - Akceptujeme 1-frame stale (= dispatch vidí stav z minulého render).
 *
 * Lifetime:
 *   - `watch_emu_cache_init()` se volá při debugger_init (idempotentní).
 *   - `watch_emu_cache_shutdown()` při debugger_exit.
 *   - Mezi init a shutdown jsou publish/get/remove thread-safe.
 *
 * Scope omezení (V1.D.2.C):
 *   - Nezahrnuje historii (= jen aktuální stav).
 *   - Nezahrnuje string/bytes typy v plné šíři (= jen int snapshot
 *     statistiky podle st_WATCH_VALUE_SNAP.value_int + len).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later
 *
 * ---------------------------------------------------------------------------
 */

#ifndef WATCH_EMU_CACHE_H
#define WATCH_EMU_CACHE_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "debugger/watch_cache.h"   /* st_WATCH_VALUE_SNAP, st_WATCH_CACHE_VIEW */


/**
 * @brief Maximální délka jména řádku v zrcadle (= storage budget).
 *
 * Delší jména se ořežou (= prefix byte-safe, ne UTF-8 hranice). Watch
 * GUI typicky používá krátká jména (< 32 znaků); 64 dává rezervu.
 */
#define WATCH_EMU_SNAPSHOT_NAME_MAX 64


/**
 * @brief Snímek statistik jednoho watch řádku pro dispatch vlákno.
 *
 * Naplňuje `watch_emu_cache_get_by_name`. Pole jsou kopie ze zrcadla
 * (= caller vlastní; lifetime nezávislá na cache mutaci).
 *
 * @invariant Pokud `snapshot_active == false`, `snap_int` má hodnotu 0
 *            a klient ji nesmí používat (= UI nedrží baseline).
 * @invariant `name` je NULL-terminated.
 */
typedef struct st_WATCH_EMU_SNAPSHOT {
    int      row_id;                              /**< Stable id řádku (>= 1). */
    char     name[ WATCH_EMU_SNAPSHOT_NAME_MAX ]; /**< Jméno řádku (kopie). */
    bool     snapshot_active;                     /**< Snapshot baseline aktivní (E.2). */
    int64_t  snap_int;                            /**< Sign-extended snapshot value. */
    int64_t  cur_int;                             /**< Sign-extended current value. */
    int64_t  delta_int;                           /**< cur - snap (jen pokud snapshot_active). */
    int64_t  min_int;                             /**< Min od poslední init/reset. */
    int64_t  max_int;                             /**< Max od poslední init/reset. */
    uint64_t change_count;                        /**< Počet změn od poslední init/reset. */
    bool     min_max_valid;                       /**< false = string typ nebo neinicializováno. */
    int      type_snap;                           /**< Typ řádku v okamžiku publish. */
} st_WATCH_EMU_SNAPSHOT;


/* ===================================================================== */
/*  Lifecycle                                                            */
/* ===================================================================== */


/**
 * @brief Inicializuje zrcadlo (= alokuje mutex + hash table). Idempotentní.
 *
 * Volat z debugger_init nebo mcp_init po watch_cache_init. Vícenásobné
 * volání jsou no-op (= ochrana přes g_once_init_enter).
 *
 * @post Mutex a hashtable jsou alokovány a modul je připraven přijímat
 *       publish/get volání z libovolného vlákna.
 */
extern void watch_emu_cache_init ( void );


/**
 * @brief Uvolní hash table a mutex. Volat z debugger_exit / mcp_exit.
 *
 * @post Storage je smazaný; další publish/get vrátí no-op / false.
 *       Init lze znovu volat.
 */
extern void watch_emu_cache_shutdown ( void );


/* ===================================================================== */
/*  Publish (UI vlákno) + Get (dispatch vlákno)                          */
/* ===================================================================== */


/**
 * @brief Publikuje aktuální stav jednoho watch řádku do zrcadla.
 *
 * Volá UI vlákno z watch render loop jednou per frame per řádek po
 * `watch_cache_update`. Pokud `view` nebo `name` jsou NULL, řádek se
 * z mirroru odstraní (= prune odebraných řádků).
 *
 * Pokud `name` je prázdný řetězec (anonymní řádek), publikace se
 * přeskočí (= anonymní řádky nejsou pojmenovatelně lookupovatelné a
 * v Resource URI nedávají smysl).
 *
 * @param row_id  Stable id řádku (>= 1).
 * @param view    Pointer na čerstvou cache view (nebo NULL = remove).
 * @param name    Jméno řádku (nebo NULL = remove). Kopíruje se interně.
 * @param snap    Snapshot baseline view (nebo NULL = baseline neaktivní).
 * @param type    en_WATCH_TYPE řádku (= pro sign-extend snap/cur_int).
 *
 * Thread-safe (= drží interní mutex po dobu publish).
 */
extern void watch_emu_cache_publish ( int row_id,
                                       const st_WATCH_CACHE_VIEW *view,
                                       const char *name,
                                       const st_WATCH_VALUE_SNAP *snap,
                                       int type );


/**
 * @brief Smaže řádek ze zrcadla podle stable id (= odebraný řádek).
 *
 * Volá UI vlákno z watch_remove hook nebo z prune_stale path. No-op
 * pokud řádek v zrcadle není.
 *
 * @param row_id  Stable id (= ten, který byl publish-nut).
 */
extern void watch_emu_cache_remove ( int row_id );


/**
 * @brief Smaže ze zrcadla všechny řádky, jejichž id není v `live_ids`.
 *
 * Volá UI vlákno jednou per frame z prune cesty (= analogie k
 * `watch_cache_prune_stale`). Pokud `n == 0`, zrcadlo se vyprázdní.
 *
 * @param live_ids  Pole platných stable ID (= aktuální watch storage).
 * @param n         Počet položek.
 *
 * Thread-safe (= drží mutex po dobu prune).
 */
extern void watch_emu_cache_prune_stale ( const int *live_ids, size_t n );


/**
 * @brief Hledá řádek podle jména a vyplní `out` snapshotem.
 *
 * Volá dispatch vlákno z MCP handleru. Lookup je linear scan přes
 * hash table values (= jméno není primární klíč, malý počet řádků
 * v praxi - desítky).
 *
 * Case-sensitive porovnání jména (= shoda s tím, co UI publish-nul).
 *
 * @param name  Hledané jméno (NULL nebo prázdné = vrací false).
 * @param out   Výstupní snapshot (musí být ne-NULL).
 * @return true pokud nalezen + `*out` vyplněn, false jinak.
 *
 * Thread-safe (= drží mutex po dobu lookupu + kopie).
 */
extern bool watch_emu_cache_get_by_name ( const char *name,
                                          st_WATCH_EMU_SNAPSHOT *out );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* WATCH_EMU_CACHE_H */
