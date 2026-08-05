/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file symdb_bridge.h
 * @brief Most mezi sym_db (UI-úroveň DB se zdroji+prioritou) a
 *        z80_symtab_t (dasm-z80 engine: addr→jméno lookup).
 *
 * Disassembler dasm-z80 nezná mz800new sym_db. Při formátování
 * instrukce přes @c z80_dasm_to_str_sym() potřebuje pouze
 * @c z80_symtab_t s lookup-by-addr API.
 *
 * Bridge cache:
 *   - jeden file-static z80_symtab_t pro všechny render volání
 *   - rebuild jen když sym_db_get_version() narostlo (= byl import
 *     nebo ruční edit), jinak no-op
 *   - bank handling: jen bank_id == 0 (= "CPU view") - V1 limit,
 *     odpovídá fallback chování sym_db_lookup_by_addr()
 *
 * Threading: jen UI vlákno. Žádný přístup z EMU vlákna.
 *
 * Historie: tento modul byl extrahován v F3 (disassembler-window-v1)
 * z @c dbg_disassembled.cpp:543-634, kde byl původně file-static.
 * Důvod extrakce: druhý konzument - Disassembler V1 okno - potřeboval
 * stejnou bridge cestu, ale bez závislosti na sekci.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef SYMDB_BRIDGE_H
#define SYMDB_BRIDGE_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "libs/dasm-z80/z80_dasm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vrátí cached z80_symtab_t naplněný aktuálním obsahem sym_db.
 *
 * Při prvním volání lazy-alokuje interní tabulku. Při dalších voláních
 * porovná @c sym_db_get_version() s cached verzí - pokud se neliší,
 * vrátí cached pointer beze změny. Pokud se liší, rebuild (clear +
 * insertion-order naplnění z @c sym_db_get_by_index).
 *
 * Filtrace: vstupují jen záznamy s @c bank_id == 0 (CPU view). V1.5+
 * bude bank-aware lookup pro 0xE000-0xFFFF; momentálně jen globální
 * symboly.
 *
 * @return Pointer na cached z80_symtab_t, nebo NULL pokud alokace
 *         selhala. Caller pointer NEUVOLŇUJE - lifetime spravuje
 *         modul (žije do procesního exitu, případně do
 *         @ref symdb_bridge_destroy).
 *
 * @pre Volat jen z UI vlákna (sym_db je čistě UI-thread).
 * @post Vrácený pointer odpovídá aktuální sym_db verzi.
 *
 * @note Pokud volající potřebuje "vždy fresh" snapshot, stačí volat
 *       opakovaně - cache invalidace je automatická přes
 *       @c sym_db_get_version() porovnání.
 */
const z80_symtab_t *symdb_bridge_get_symtab(void);

/**
 * @brief Uvolní bridge cache (volat při shutdownu debuggeru).
 *
 * Idempotentní - opakované volání nic neudělá. Po volání se další
 * @ref symdb_bridge_get_symtab() znovu lazy-alokuje.
 *
 * @post Interní cache je NULL, version reset na 0.
 */
void symdb_bridge_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* SYMDB_BRIDGE_H */
