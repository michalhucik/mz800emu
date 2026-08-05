/*
 * File:   freeze.h
 *
 * Freeze Bytes - simple "cheat engine" style freeze loop for Memory Browser
 * (V1). Drží set (kind, sub_id, offset, value) záznamů; per-frame hook v
 * mzarch loop přepíše příslušné byty na zafrozenou hodnotu (analogie
 * Mesen2 cheat engine).
 *
 * Storage:
 *   - statická alokace v BSS, max FREEZE_MAX_ENTRIES (256)
 *   - lineární scan při add / remove / is_frozen / apply (V1 simplicity;
 *     pro V2+ lze přidat hash mapu pro O(1) lookup)
 *   - aktivační flag g_freeze_active = (count > 0) - hot path test je
 *     1 atomic byte load + branch (branch predictor naučí default OFF)
 *
 * Threading:
 *   - storage mutace (add/remove/clear) jen z UI vlákna (typicky z Memory
 *     Browser context menu)
 *   - freeze_apply_all() z EMU vlákna per-frame (mzarch screen_done callback)
 *   - žádný mutex - "shear" je v praxi tolerovatelný (V1: max 1 frame
 *     vynechání pokud UI právě modifikuje storage při tom co EMU vlákno
 *     iteruje). Pro V2+ lze přidat lock-free queue.
 *
 * Hot path discipline:
 *   - freeze_apply_all() MUSÍ skončit v O(1) když g_freeze_active = 0
 *     (= žádné frozen byty). Volání: test atomic flag + return.
 *   - Při g_freeze_active = 1: lineární scan max 256 entries × 1 dbgapi
 *     write. Per-frame to je zanedbatelné (60 Hz × 256 = 15 360 writes/s).
 *
 * Závislosti:
 *   - dbgapi_regions_* pro write per (region_id, offset). Region ID se
 *     dynamicky resolvuje per scan (banking aware). Pokud region disconnected,
 *     freeze entry se skipne (= "soft" tolerance).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef FREEZE_H
#define FREEZE_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


/**
 * @brief Maximální počet současně zafrozených bajtů.
 *
 * Pevný horní limit pro statickou alokaci v BSS. V1 zámerná konstanta -
 * dostatečná pro běžné použití (typicky 1-20 frozen bytů per session).
 */
#define FREEZE_MAX_ENTRIES 256


/**
 * @brief Záznam jedné zafrozené adresy.
 *
 * Drží (region_kind, sub_id, offset) klíč + hodnotu která se má při každém
 * tickovém apply zapsat zpět. Region ID se neukládá - resolvuje se dynamicky
 * při freeze_apply_all() (banking-aware).
 *
 * @invariant in_use = true znamená že (kind, sub_id, offset) je platná
 *            adresa v aktuální architektuře.
 */
typedef struct st_FREEZE_ENTRY {
    int       region_kind;   /**< en_REGION_KIND (z dbgapi_regions.h). */
    int       sub_id;        /**< Disambiguator (bank/plane index). */
    uint32_t  offset;        /**< Offset v rámci regionu. */
    uint8_t   value;         /**< Hodnota která se zapíše při apply. */
    bool      in_use;        /**< Slot je obsazen. */
} st_FREEZE_ENTRY;


/**
 * @brief Globální flag - zkratka pro hot path test.
 *
 * 0 = žádné frozen byty; freeze_apply_all() vrátí okamžitě bez scan.
 * 1 = alespoň 1 entry. Sync s freeze_count() při každém add/remove/clear.
 *
 * Volume: extern unsigned (ne bool, aby šel test v C i C++ shodně bez
 * stdbool include).
 */
extern unsigned g_freeze_active;


/**
 * @brief Inicializace modulu - vyčistí storage.
 *
 * Volat jednou při startu emulátoru (před prvním freeze_add).
 */
void freeze_init ( void );


/**
 * @brief Smaže všechny záznamy.
 *
 * @post freeze_count() = 0, g_freeze_active = 0.
 */
void freeze_clear ( void );


/**
 * @brief Přidá nový freeze záznam.
 *
 * Pokud již existuje entry pro stejnou (kind, sub_id, offset), přepíše
 * její value (= update). Jinak hledá první volný slot.
 *
 * @param region_kind  en_REGION_KIND hodnota.
 * @param sub_id       Disambiguator (0 pro single-bank regiony).
 * @param offset       Offset v rámci regionu.
 * @param value        Hodnota která se má zapisovat při apply.
 * @return true při úspěchu, false pokud je storage plný.
 */
bool freeze_add ( int region_kind, int sub_id, uint32_t offset, uint8_t value );


/**
 * @brief Odstraní freeze záznam.
 *
 * @return true pokud byl entry nalezen a smazán, false pokud neexistoval.
 */
bool freeze_remove ( int region_kind, int sub_id, uint32_t offset );


/**
 * @brief Test zda je byte zafrozený.
 *
 * @param[out] out_value Pokud non-NULL a entry existuje, naplní se hodnotou.
 * @return true pokud existuje frozen entry, false jinak.
 */
bool freeze_is_frozen ( int region_kind, int sub_id, uint32_t offset,
                        uint8_t *out_value );


/**
 * @brief Vrátí počet aktivních frozen entries.
 */
size_t freeze_count ( void );


/**
 * @brief Iterace - vrátí entry na slot indexu (0..FREEZE_MAX_ENTRIES-1).
 *
 * @return Pointer na entry nebo NULL pokud idx mimo rozsah nebo slot prázdný.
 */
const st_FREEZE_ENTRY *freeze_get_slot ( size_t idx );


/**
 * @brief Aplikuj všechny frozen byty - per-frame hook z EMU vlákna.
 *
 * Hot path discipline: pokud g_freeze_active = 0, vrací okamžitě bez scan.
 * Volat z mz800_main_event_callback_screen_done() (per-frame point).
 *
 * Region ID se resolvuje dynamicky přes dbgapi_regions_enumerate (banking
 * aware). Pokud region disconnected, entry se skipne (= soft tolerance pro
 * Memext/Ramdisk disconnect uprostřed session).
 */
void freeze_apply_all ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* FREEZE_H */
