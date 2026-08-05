/**
 * @file dasm_window.h
 * @brief Disassembler V1 - public API samostatného range-based okna.
 *
 * Mutant disassembler-window-v1. Aktivuje DASM tlačítko v iconbaru
 * (dbg_iconbar.cpp:304-313), které doposud bylo BeginDisabled(true)
 * stub. Otevírá samostatné dokovatelné okno, ve kterém uživatel zadá
 * rozsah adres From/To, klikne Disassemble a vidí read-only listing.
 *
 * F0+F1 (tento stav): minimální skelet + range UI + ImGui table se
 * 4 sloupci (Addr | Bytes | Label | Mnemonic). Sloupec Label zůstává
 * prázdný - bude naplněn ve F2 (auto-labely S/L/D/W). Save dialog,
 * symbol substituce a per-dialect rendering přijdou ve F3-F6.
 *
 * Vlastní state okna je file-static v dasm_window.cpp; persistence
 * pozice/velikosti probíhá přes standardní ImGui ini mechanizmus
 * (window state ImGui ukládá sám podle stable ID @c ###dasm_window).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef DASM_WINDOW_H
#define DASM_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí Disassembler V1 okno do aktuálního ImGui frame.
 *
 * Volá se per-frame z hlavní debugger render smyčky v
 * @c main_window.cpp. Pokud @c g_gui->showDisassemblerWindow je
 * false, funkce ihned vrátí (nevykresluje nic).
 *
 * Sám si interně řídí vlastní state (range From/To, výsledek
 * posledního disassemble). První render lazy-inicializuje
 * @c s_state s default rozsahem 0x0000-0x00FF.
 *
 * @pre Volat pouze z UI vlákna mezi ImGui::NewFrame a EndFrame.
 * @pre @c g_gui != NULL (existuje po sdlapp_imgui_video init).
 */
void dasm_window_render(void);

/**
 * @brief Registruje cfgmain klíče sekce [DASM_WINDOW] pro persistenci.
 *
 * F7: range, options, dialect, save_path. Volá se z @c debugger_init
 * po @c cfgroot_register_new_module na samostatném modulu.
 *
 * @param cmod_void Pointer na @c st_CFGMODULE (cast přes @c void* aby
 *                  header nemusel includovat cfgfile/cfgmodule.h).
 *                  Pokud NULL, funkce nic neudělá.
 *
 * @post Klíče jsou registrované; uživatel okna ji uvidí v INI souboru
 *       po prvním ukončení emulátoru.
 */
void dasm_window_register_persistence(void *cmod_void);

/**
 * @brief Aplikuje persisted hodnoty na živý UI state.
 *
 * F7: volá se po @c cfgmodule_propagate v @c debugger_init - po
 * načtení .ini z disku se zkopírují @c persist_* pole do živých UI
 * polí (typové konverze + clamping).
 *
 * @post UI state je sync s persisted hodnotami; první render uvidí
 *       obnovené range/options.
 */
void dasm_window_apply_persisted(void);

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DASM_WINDOW_H */
