/**
 * @file membrowser_window.h
 * @brief Memory Browser - public API hlavního okna (V0 hex MVP).
 *
 * Nahrazuje "Memory Browser zatím neimplementováno" stub v
 * dbg_iconbar.cpp:288-294. Plná paleta regionů (Logical Z80, RAM,
 * ROM, CG-ROM, VRAM plane, MZ-700 char/attr, CG-RAM, PCG, Memext,
 * Ramdisk) přes dbgapi_regions_*. 8 encodings (Raw, Sharp EU/JP,
 * Display EU/JP, UTF-8 EU/JP, KOI8-CS). Editace + basic search.
 *
 * Multi-instance API (V3+): membrowser_view_create() s window_id.
 * V0 vytváří lazy singleton "main". Sekundární okna (Memory Browser
 * #2..#5) budou přidána V3 bez refaktoringu interní logiky - pattern
 * shodný s dbg_disassembled.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_WINDOW_H
#define MEMBROWSER_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle na instanci Memory Browser okna.
 *
 * Definice je v membrowser_window.cpp - volající nesahá na fieldy přímo.
 * V0: jediný singleton "main" lazy-vytvořený při prvním renderu.
 * V3+: API umožňuje 4 sekundární instance (#2..#5) s vlastním stavem.
 */
typedef struct MembrowserView MembrowserView;

/**
 * @brief Vytvoří novou instanci Memory Browser okna.
 *
 * Alokuje strukturu MembrowserView, vynuluje state, načte persisted
 * hodnoty (pro hlavní instanci "main") a registruje window_id pro
 * ImGui stable ID.
 *
 * @param window_id stabilní ID instance ("main", "2", ..., "5"),
 *                  držený řetězec musí žít po celou dobu existence
 *                  instance (statický string je OK).
 * @return Pointer na novou instanci, NULL při chybě alokace.
 */
MembrowserView *membrowser_view_create ( const char *window_id );

/**
 * @brief Uvolní instanci a před tím uloží state do persistu.
 *
 * @param self Smí být NULL (no-op).
 */
void membrowser_view_destroy ( MembrowserView *self );

/**
 * @brief Vykreslí instanci do aktuálního ImGui frame.
 *
 * Volá se per frame pokud okno má být viditelné. Po zavření krížkem
 * ImGui vynuluje *p_open na false.
 *
 * @param self  Instance (nesmí být NULL).
 * @param p_open Pointer na bool řídící viditelnost.
 *
 * @pre Volat pouze z UI vlákna mezi NewFrame a EndFrame.
 */
void membrowser_view_render ( MembrowserView *self, bool *p_open );

/* ---------------------------------------------------------------------------
 * Backward-compat C-API - tenké obaly nad singleton hlavní instancí + V3
 * multi-view API pro sekundární okna.
 *
 * main_window.cpp volá render hlavního okna přes membrowser_window_render()
 * a 4 sekundární okna přes @ref membrowser_window_render_all_secondary.
 * ------------------------------------------------------------------------- */

/**
 * @brief Render hlavního singleton Memory Browser okna (instance "main").
 *
 * Lazy-inicializuje singleton při prvním volání. Volá se z main_window.cpp
 * pokud g_gui->showMemoryBrowserWindow == true.
 *
 * @param p_open Pointer na bool řídící viditelnost
 *               (typicky &g_gui->showMemoryBrowserWindow).
 */
void membrowser_window_render ( bool *p_open );

/**
 * @brief V3 multi-view: render všech otevřených sekundárních oken (#2..#5).
 *
 * Iteruje @c g_gui->showMemoryBrowserWindowExtra[0..3]. Pro otevřené okno
 * lazy-vytvoří instanci, pro zavřené ji uvolní. Volá se z main_window.cpp
 * vždy (per-frame), funkce sama přeskočí zavřené sloty.
 *
 * @pre Volat pouze z UI vlákna mezi NewFrame a EndFrame.
 */
void membrowser_window_render_all_secondary ( void );

/**
 * @brief Toggle viditelnosti hlavního Memory Browser okna.
 *
 * Přepne g_gui->showMemoryBrowserWindow. Volá se z menu / shortcut Alt+E.
 */
void membrowser_window_show_hide ( void );

/**
 * @brief Registrace persist klíčů pro VŠECHNY instance Memory Browser oken
 *        a legacy fallback sekci.
 *
 * Volá se z debugger.c po vytvoření CFGMOD modulů. Interně registruje:
 *   - [MEMBROWSER_WINDOW_MAIN] -> slot 0 (main),
 *   - [MEMBROWSER_WINDOW_2..5] -> sloty 1..4 (sekundární okna),
 *   - [MEMBROWSER_WINDOW] (legacy) -> scratch slot - pokud sekce MAIN
 *     chybí v INI, hodnoty se zkopírují do MAIN slotu (jednorázová
 *     migrace; po prvním save bude zapsána MAIN a legacy už se neaplikuje).
 *
 * Sekce se zaregistrují pomocí cfgroot_register_new_module - volajícímu
 * pošleme přes parametr ukazatel na funkci @ref membrowser_cfg_register_cb
 * nebo si modul najdeme sami; ve V3 použijeme přístup, kde si modul
 * registrujeme uvnitř (volajícímu stačí předat @c cfgroot pointer).
 *
 * @param cfgroot Ukazatel na CFGROOT (= g_cfgmain).
 */
void membrowser_window_register_persistence_all ( void *cfgroot );

/**
 * @brief DEPRECATED: zachovaná signatura pro zpětnou kompatibilitu.
 *
 * Předpokládá, že @p cmod je CFGMOD pro sekci @c [MEMBROWSER_WINDOW_MAIN]
 * a registruje persist klíče slotu 0 (main). Pro V3 multi-view použij
 * @ref membrowser_window_register_persistence_all místo toho - registruje
 * všechny sloty + legacy fallback. Tato funkce nadále existuje pro
 * případnou zpětnou kompatibilitu, ale nedoporučuje se její nové
 * použití (registruje jen MAIN slot, sekundární okna nemají persist).
 *
 * @param cmod CFGMOD pointer.
 */
void membrowser_window_register_persistence ( void *cmod );

/**
 * @brief Char Inserter API: vrátí cursor adresu + writable + encoding +
 *        region název zdrojové MB instance.
 *
 * Voláno per frame z @c membrowser_char_inserter_render() pro status řádek
 * "Target: MB #N @ 0xNNNN (Region: %s)". Pokud instance neexistuje (slot
 * nenaplněný, okno zavřené) nebo nemá platný region, vrátí false a
 * out-parametry zůstanou nezměněné.
 *
 * @param instance_idx           0..MB_INSTANCE_COUNT-1 (0 = main, 1..4 = #2..#5).
 * @param[out] cursor_addr_out   Aktuální cursor adresa instance.
 * @param[out] writable_out      true pokud region je R/W A edit_enabled.
 * @param[out] encoding_id_out   en_MEMBROWSER_ENCODING aktivní instance.
 * @param[out] region_name_buf   Buffer pro UTF-8 název regionu (nebo NULL).
 * @param      region_name_buf_sz Velikost @p region_name_buf (alespoň 64).
 * @return true pokud instance má platný region (out-parametry vyplněny).
 */
bool membrowser_window_get_cursor_info ( int instance_idx,
                                          uint32_t *cursor_addr_out,
                                          bool *writable_out,
                                          int *encoding_id_out,
                                          char *region_name_buf,
                                          unsigned long region_name_buf_sz );

/**
 * @brief Char Inserter API: zapíše jeden byte na cursor pos zdrojové MB
 *        instance + posune cursor + push undo + edited mark.
 *
 * Stejná write cesta jako hexview ASCII typing - reuse undo/edited stack.
 * Bezpečné volat jen z UI vlákna. Backend si vytvoří interně přes
 * @c membrowser_io_make_emu_backend.
 *
 * @param instance_idx 0..MB_INSTANCE_COUNT-1.
 * @param byte         Byte k zapsání.
 * @return true při úspěchu; false pokud: instance nenaplněná, region_id<0,
 *         RO region, edit_enabled=false, write_bytes selhání, cursor mimo
 *         region size.
 */
bool membrowser_window_write_byte_at_cursor ( int instance_idx, uint8_t byte );

/**
 * @brief Aplikuje persistované / CLI overridy na @c g_gui flagy.
 *
 * Volá se ze @c sdlapp_imgui_video.cpp po vytvoření @c g_gui
 * (= analog @c event_viewer_window_apply_persisted). V0-leftovers F4
 * implementace propaguje CLI flag @c --memory-browser:
 * pokud přítomen, vynutí @c g_gui->showMemoryBrowserWindow = true.
 *
 * Per design persistence neukládá show/hide stav oken (existuje jako
 * standalone ImGui window state v imgui.ini). CLI override je tedy
 * primární cesta jak hlavní okno automaticky otevřít při startu.
 */
void membrowser_window_apply_persisted ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* MEMBROWSER_WINDOW_H */
