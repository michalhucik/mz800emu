/*
 * File:   bptmap.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 30. září 2015, 10:17
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef BPTMAP_H
#define BPTMAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>


    typedef enum en_BREAKPOINT_TYPE {
        BREAKPOINT_TYPE_NONE = -1, // na adrese neni zadny breakpoint
        BREAKPOINT_TYPE_TEMPORARY = -2, // na adrese se nachazi docasny breakpoint (run to cursor)
    } en_BREAKPOINT_TYPE;


    /**
     * @brief Per-typ index BP enforcement tabulek (D.2).
     *
     * Hodnoty mapují en_BPT_TYPE pro non-PC typy do interních lookup polí
     * v st_BPTMAP. PC_EXEC má vlastní O(1) lookup přes bpmap[addr] a do
     * této enumerace nepatří.
     *
     * Pořadí stabilní - používá se jako index do polí (per_type_lists/active).
     */
    typedef enum en_BPTMAP_TYPE_IDX {
        BPTMAP_IDX_MEM_R = 0,
        BPTMAP_IDX_MEM_W,
        BPTMAP_IDX_IORQ_R,
        BPTMAP_IDX_IORQ_W,
        BPTMAP_IDX_IRQ,
        BPTMAP_IDX_GLOBAL,
        BPTMAP_IDX_HW_EVENT,        /**< D.3: BP typu BPT_TYPE_HW_EVENT */
        BPTMAP_IDX_SP_THRESHOLD,    /**< D.4: BP typu BPT_TYPE_SP_THRESHOLD */
        BPTMAP_IDX_IRQ_SIG,         /**< V1.5.A8.5: BP typu BPT_TYPE_IRQ_SIG (pre-dispatch) */
        BPTMAP_IDX_PC_EXEC_NONSINGLE, /**< V1.6+ TODO 4.3: PC_EXEC RANGE/MASK list (per-instruction iter) */
        BPTMAP_IDX_COUNT
    } en_BPTMAP_TYPE_IDX;


    /**
     * @brief Hlavní tabulka pro BP fast-dispatch v hot path.
     *
     * - bpmap[]: O(1) lookup PC EXEC BP (legacy, A' fáze).
     * - per_type_lists[]: GArray(int bp_id) per non-PC typ. Linear scan
     *   v hot path, akceptovatelný do ~10 BP per typ (V1).
     * - per_type_active[]: bool flag per typ, fast-skip pokud žádný BP
     *   daného typu není registrovaný (= branch predictor friendly).
     *
     * Invarianty:
     *   per_type_active[i] == (per_type_lists[i] != NULL && len > 0),
     *     u BPTMAP_IDX_GLOBAL navíc || global_hook
     *   bp_id v per_type_lists[i] musí odpovídat existujícímu BP s
     *     správným typem v g_breakpoints.breakpoints (= caller povinen
     *     synchronizovat při add/remove/set_type).
     *
     * Threading: čte EMU vlákno (hot path), zapisuje breakpoints.c při
     * mutacích. V1 nemá explicit lock - mutace jsou redundantní (UI
     * volá breakpoints_* z UI vlákna, EMU už paused, žádný race).
     */
    typedef struct st_BPTMAP {
        int temporary_bpt_addr; // hodnota 0x0000 - 0xffff odkazuje na adresu docasneho breakpointu (smi byt jen jeden)
        int bpmap [ 0x10000 ]; // hodnota >= 0 odkazuje na ID nastaveneho breakpointu
        GArray *per_type_lists [ BPTMAP_IDX_COUNT ];   /**< GArray(int bp_id) per typ */
        bool   per_type_active [ BPTMAP_IDX_COUNT ];   /**< fast-skip flag per typ */
        bool   any_active;                              /**< OR všech per_type_active (= jen jeden test pro fast-skip) */
        bool   has_enabled_bp;                          /**< true pokud existuje aspoň jeden efektivně povolený BP (jakýkoliv typ, vč. PC_EXEC v bpmap[]). Nastavuje breakpoints_sync_bptmap. Používá TEST_DEBUGGER_CPUHIST_ACTIVE - historie CPU je k dispozici i bez okna, pokud je BP enabled. */
        bool   global_hook;                             /**< true = bránu GLOBAL drží zapnutou i zastavení na čase (run_control_stop_at_arm()); per_type_active[GLOBAL] == (seznam GLOBAL neprázdný || global_hook). */
    } st_BPTMAP;

    extern st_BPTMAP g_bptmap;

    extern void bptmap_init ( void );
    extern void bptmap_clear_all ( void );

    /**
     * @brief Zapne nebo vypne háček brány GLOBAL (zastavení na čase).
     *
     * Brána GLOBAL v hlavní smyčce volá breakpoints_enforce_global() před
     * každou instrukcí, když per_type_active[BPTMAP_IDX_GLOBAL]. Háček ji
     * drží zapnutou i bez uživatelských GLOBAL breakpointů; odebrání
     * posledního GLOBAL breakpointu ani bptmap_clear_all() ji pak nevypne.
     *
     * @param on true = háček aktivní.
     * @pre Emu vlákno (run_control_stop_at_arm() / _clear()).
     * @post per_type_active[GLOBAL] == (seznam GLOBAL neprázdný || on),
     *       any_active přepočítaný.
     */
    extern void bptmap_set_global_hook ( bool on );
    extern int bptmap_event_add ( uint16_t addr, int id );
    extern int bptmap_event_clear ( uint16_t addr, int id );
    extern void bptmap_event_clear_addr ( uint16_t addr );
    extern void bptmap_event_clear_id ( int id );
    extern int bptmap_event_get_addr_by_id ( int id );
    extern int bptmap_event_get_id_by_addr ( uint16_t addr );
    extern void bptmap_reset_temporary_event ( void );
    extern void bptmap_set_temporary_event ( uint16_t addr );


    /* ===== D.2 per-typ API ================================================= */

    /**
     * @brief Zaregistruje BP do per-typ lookup tabulky.
     *
     * @param bp_id Kladné ID breakpointu (musí odpovídat g_breakpoints).
     * @param idx   Cílová tabulka (BPTMAP_IDX_*).
     *
     * Pokud bp_id už v tabulce existuje, druhé volání je idempotentní.
     * Po návratu per_type_active[idx] = true a any_active = true.
     */
    extern void bptmap_register ( int bp_id, en_BPTMAP_TYPE_IDX idx );

    /**
     * @brief Odregistruje BP z per-typ lookup tabulky.
     *
     * @param bp_id BP ID k odstranění.
     * @param idx   Tabulka.
     *
     * Po odebrání posledního prvku per_type_active[idx] = false.
     * any_active přepočítá podle ostatních tabulek.
     */
    extern void bptmap_unregister ( int bp_id, en_BPTMAP_TYPE_IDX idx );

    /**
     * @brief Odregistruje BP ze všech per-typ tabulek (= remove BP).
     *
     * @param bp_id BP ID.
     *
     * Užitečné pro breakpoints_remove() bez znalosti aktuálního typu.
     */
    extern void bptmap_unregister_all ( int bp_id );

    /**
     * @brief Vrátí list BP ID pro daný typ (read-only).
     *
     * @param idx Tabulka.
     * @return Pointer na GArray(int) nebo NULL pokud žádný BP daného
     *         typu neexistuje. Caller musí list považovat za read-only
     *         a nedrží ho přes mutaci breakpoints.
     */
    extern GArray* bptmap_get_list ( en_BPTMAP_TYPE_IDX idx );


#ifdef __cplusplus
}
#endif

#endif /* BPTMAP_H */
