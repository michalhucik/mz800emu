/*
 * File:   memmap_window.h
 *
 * Memory Map debug okno - vizualizace banking + memext stavu pro celý
 * Z80 adresní prostor 0x0000-0xFFFF v 4 kB granularitě (16 řádků).
 *
 * Layout (autoritativní per debugger/rozbory/budoucnost/memory-map/):
 *   [Roletka DMD mode]   <- jen MZ-800
 *   |  Addr  | Banking | MemExt |
 *   | $0000  | ROM low |   --   |
 *   | $1000  | CG-ROM  | SRAM 01h |
 *   |   ...
 *   | $F000  | ROM up  |   --   |
 *
 * Sloupec Banking reflektuje aktuální g_memory.map (a g_gdg.regDMD pro
 * MZ-800) přes per-arch memmap_query() helper. Sloupec MemExt zobrazuje
 * vždy aktuální g_memext.map[] nezávisle na Sharp banking.
 *
 * V0 rozsah:
 *   - 3 sloupce x 16 řádků
 *   - barevné kódování Banking sloupce
 *   - 3 stavy MExt sloupce (žádný / Luftner 4K / PEHU 8K) - tlačítka
 *     "$xx" bez akce, plánovaná funkčnost edit přijde V1+
 *   - DMD roletka (jen MZ-800), direct write do g_gdg.regDMD
 *   - okno se v prvním framu auto-sizuje na obsah (SetNextWindowSize +
 *     ImGuiCond_Once), pak je user-resizable (= AlwaysAutoResize byl
 *     odstraněn kvůli hover race v prvním framu po Appearing)
 *
 * V0 zrušeno (oproti původnímu plánu):
 *   - auto-pauza na klik do okna (= jen drobná race s emu vláknem,
 *     fakticky neproblém - rozhodnuto v review V0)
 *
 * V0 out of scope:
 *   - klikatelný Addr sloupec (focus disasm/memory)
 *   - banking history
 *   - per-region BP / watchpoint quick action
 *   - dbgapi rozšíření
 *
 * Visibilita drží g_gui->showMemoryMapWindow.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMMAP_WINDOW_H
#define MEMMAP_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Persistentní stav Memory Map okna.
 *
 * Drží UI volby, které se ukládají do imgui.ini přes cfgmain modul
 * "MEMMAP_WINDOW". Inicializace defaultů probíhá v memmap_window_state_init(),
 * registrace cfgelementů v memmap_window_register_persistence().
 *
 * Pole:
 *   - compact_mode: true = kompaktní zobrazení (prostřední sloupec
 *                   zúžený na šířku labelu "BNK", obsah jen marker
 *                   nebo nic), false = normální (sloupec roztažen,
 *                   obsah text-label regionu).
 *   - initialized:  flag idempotence pro memmap_window_state_init().
 *   - layout_dirty: one-shot - po toggle compact_mode si render
 *                   v dalším framu vyžádá SetNextWindowSize(0,0)
 *                   pro re-fit okna na nový obsah.
 *
 * Default po fresh installu: compact_mode = true.
 */
typedef struct st_MEMMAP_WINDOW {
    /* unsigned místo bool: cfgelement BOOL handler dělá memcpy(sizeof(int))
     * při propagate i save (cfgelement.c:854, 904). bool (1 B) by tudíž
     * přepsal sousední pole struktury 3 garbage byty. */
    unsigned compact_mode;
    bool initialized;
    bool layout_dirty;
} st_MEMMAP_WINDOW;


/** Globální instance perzistentního stavu Memory Map okna. */
extern struct st_MEMMAP_WINDOW g_memmap_window;


/**
 * @brief Inicializace defaultních hodnot st_MEMMAP_WINDOW.
 *
 * Idempotentní - druhé a další volání nic nedělá. Volá se z
 * memmap_window_register_persistence() PŘED registrací cfgelementů,
 * aby defaulty byly v paměti dříve než cfgmodule_propagate() případně
 * přepíše hodnoty z imgui.ini.
 */
extern void memmap_window_state_init ( void );


/**
 * @brief Registrace cfgelementů Memory Map okna do daného CFGMOD.
 *
 * Volá se z debugger_init() po cfgroot_register_new_module("MEMMAP_WINDOW").
 * Vnitřně volá memmap_window_state_init() pro inicializaci defaultů,
 * pak cfgmodule_register_new_element pro každé persistentní pole.
 *
 * @param cmod_void  Pointer na st_CFGMODULE (cast přes void* aby header
 *                   nemusel includovat cfgfile interní typy).
 */
extern void memmap_window_register_persistence ( void *cmod_void );


/**
 * @brief Vyžádá fokus Memory Map okna v příštím framu.
 *
 * Nastaví interní pending flag - příští volání memmap_window_render()
 * provede ImGui::SetNextWindowFocus() PŘED ImGui::Begin (raise window
 * to foreground + grab keyboard focus přes ImGui Platform_SetWindowFocus
 * backend = OS-level raise). Po aplikaci se flag clearuje.
 *
 * Použití: cross-window navigation z jiného debug okna (např. klik na
 * banking port row v I/O Ports Overview), kde caller typicky nejdřív
 * nastaví `g_gui->showMemoryMapWindow = true` a pak zavolá tuto funkci.
 *
 * Idempotentní v rámci framu (= opakovaná volání nepřidají efekt; flag
 * se aplikuje a clearuje jednou v render volání).
 *
 * Thread safety: pouze UI vlákno (= žádný atomic, jen plain bool).
 */
extern void memmap_window_request_focus ( void );


/**
 * @brief Vyžádá fokus + zvýraznění konkrétní 4 KB stránky.
 *
 * Rozšířená varianta @c memmap_window_request_focus(): kromě focus
 * navíc zvýrazní řádek tabulky odpovídající dané Z80 adrese (= addr/0x1000
 * = řádek 0..15). Pulse highlight cca 1.5 sekundy, pak vyprchá.
 *
 * Použití: cross-window navigation z Memory Browser (RMB "Show in Memory
 * Map" na bajtu z Logical Z80 view) - uživatel vidí, do jaké banking
 * stránky aktuální Memory Browser cursor patří.
 *
 * Pro adresy mimo 0..0xFFFF (= mimo Z80 prostor) se highlight nevyvolá,
 * jen focus okna (= no-op highlight).
 *
 * Idempotentní v rámci framu.
 *
 * @param addr Z80 adresa (0..0xFFFF). Page index = addr >> 12 (0..15).
 *
 * @pre Volat z UI vlákna.
 * @post Příští memmap_window_render zvýrazní řádek addr>>12.
 */
extern void memmap_window_request_focus_at ( unsigned addr );


/**
 * @brief Render Memory Map debug okna.
 *
 * Volá se z main_window každý frame, pokud `*p_open` je true.
 * Pokud uživatel okno zavře přes (X), ImGui vynuluje *p_open na false -
 * caller drží p_open ve své persistent struktuře (typicky
 * &g_gui->showMemoryMapWindow).
 *
 * Side effects:
 *   - (Jen MZ-800) Klik na položku DMD roletky provede direct write
 *     do g_gdg.regDMD + memory_reconnect_ram() pro aktualizaci dispatch
 *     tabulek (= bez IORQ funkce).
 *
 * Thread safety: pouze UI vlákno.
 *
 * @param p_open  Pointer na bool řídící viditelnost.
 */
extern void memmap_window_render ( bool *p_open );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* MEMMAP_WINDOW_H */
