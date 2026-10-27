/**
 * @file   bus_window.h
 * @brief  Okno debuggeru Bus Map - kdo vlastní který port a MMIO adresu.
 *
 * Adresa-centrický pohled na sběrnici Z80: vlevo mřížka 16 x 16 (porty
 * 00h-0FFh, nebo okno MMIO 0E000h-0E0FFh), vpravo seznam vlastníků aktivní
 * mřížky (mini buňka se zkratkou v barvě buňky = legenda, název, rozsahy,
 * budič, stav, Connect / Disconnect u zařízení devbus) a pod ním detail
 * vybrané adresy (aktivita, poslední hodnoty, peek bez vedlejších efektů,
 * příznaky slovy, skok do I/O Ports, reset aktivity, breakpointy IORQ).
 * Data dodává vrstva bus_map (src/emulator/debugger/bus_map.h): snímek se
 * bere každý snímek obrazu pod device_state_lock(), peek jde přes dbgapi do
 * emu vlákna (nikdy pod zámkem). Živou aktivitu zapíná sdílený čítač
 * sledování s oknem I/O Ports (io_activity_tracking_set_client()).
 *
 * Rozměry se odvozují z GetFontSize() (buňka podle nejširší zkratky,
 * řádek seznamu podle GetFrameHeight()); pod přirozenou šířkou obsahu se
 * okno nepřeskládává, jen dostane vodorovný posuvník.
 *
 * Položka okna je v menu HW & Devices na všech třech platformách (každá má
 * statickou tabulku vlastníků; zařízení devbus jen MZ-700).
 *
 * Vlákna: jen UI vlákno. Zásahy (připojení zařízení) jdou přes frontu
 * device_request (devbus_request_connect()), reset aktivity a peek přes
 * dbgapi.
 *
 * @par Licence: GPLv3
 */

#ifndef BUS_WINDOW_H
#define BUS_WINDOW_H

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí okno Bus Map (každý snímek, když je otevřené).
 *
 * @param p_open Ukazatel na viditelnost (typicky &g_gui->showBusMapWindow);
 *               NULL nebo false = okno zavřené (vypne svého klienta sledování).
 * @pre UI vlákno, uvnitř snímku ImGui, bez drženého device_state_lock()
 *      (okno si ho bere samo kolem bus_map_snapshot()).
 * @post Může otevřít panel breakpointu (bpt_edit_panel_open_new_iorq()),
 *       okno I/O Ports (io_window_request_show_port()) nebo zařadit zásah
 *       device_request.
 */
void bus_window_render(bool *p_open);

/**
 * @brief Registrace klíčů persistence okna do sekce INI [BUS_MAP_PANEL].
 *
 * Klíče: color_mode (0 Owners, 1 Activity, 2 Drive - hodnoty zůstávají
 * kvůli starším INI, combo je řadí Owners, Drive, Activity), active_tab
 * (0 Ports, 1 MMIO; hodnota 2 ze staršího INI = zrušená záložka Devices,
 * bere se jako Ports), list_width (px; 0 = automaticky podle minima textů),
 * tracking_active. Starší klíče inspector_width a show_legend se už
 * neregistrují, parser INI je přeskočí. Vybraná adresa se neukládá. Po
 * registraci volající zavolá cfgmodule_parse() + cfgmodule_propagate()
 * (vzor debugger_init()).
 *
 * @param cmod_void st_CFGMODULE * (bez typu kvůli hlavičce C / C++).
 */
void bus_window_register_persistence(void *cmod_void);

/**
 * @brief Vybere adresu do detailu a přepne na její záložku (křížový skok, testy).
 * @param addr Port 00h-0FFh (mmio false) nebo MMIO adresa 0E000h-0E0FFh (mmio true).
 * @param mmio true = záložka MMIO.
 */
void bus_window_select(unsigned addr, bool mmio);

/**
 * @brief Vybraná adresa aktivní záložky (testy).
 * @param mmio Výstup: true = aktivní je záložka MMIO (smí být NULL).
 * @return Adresa.
 */
unsigned bus_window_selected(bool *mmio);

/**
 * @brief Střed buňky mřížky v souřadnicích obrazovky z posledního kreslení (testy simulující klik).
 * @param addr Port 00h-0FFh (záložka Ports) nebo MMIO adresa (záložka MMIO).
 * @param x    Výstup x.
 * @param y    Výstup y.
 * @return false = mřížka s touto adresou se v posledním snímku nekreslila.
 */
bool bus_window_cell_center(unsigned addr, float *x, float *y);

/**
 * @brief Střed řádku seznamu vlastníků z posledního kreslení (testy simulující klik a hover).
 * @param row Index řádku v pořadí seznamu (podle prvního portu).
 * @param x   Výstup x (střed sloupce Owner).
 * @param y   Výstup y.
 * @return false = řádek se v posledním snímku nekreslil (mimo clipper, nebo neexistuje).
 */
bool bus_window_row_center(int row, float *x, float *y);

/** @brief Přepne záložku (0 Ports, 1 MMIO; jiná hodnota = Ports) při příštím kreslení. */
void bus_window_show_tab(int tab);

/**
 * @brief Stav okna pro testy (geometrie, texty detailu, výběr).
 *
 * Hodnoty z posledního kreslení; texty jsou v jazyce UI. Jen pro testy
 * a měření, žádná jiná vrstva na ně nespoléhá.
 */
typedef struct st_BUS_WINDOW_DEBUG {
    float cell;            /**< Velikost buňky mřížky v px. */
    float cell_min;        /**< Minimální buňka (nejširší zkratka + vzduch). */
    float natural_w;       /**< Přirozená (minimální) šířka obsahu okna: mřížka + oddělovač + seznam. */
    float list_w;          /**< Skutečná šířka pravé části. */
    float list_min_w;      /**< Minimální šířka pravé části z textů. */
    float row_h;           /**< Výška řádku seznamu. */
    float detail_h;        /**< Výška detailu změřená při posledním kreslení (bez mezery za poslední řadou). */
    float detail_fixed_h;  /**< Pevná výška detailu, se kterou se dělil prostor seznam / detail (musí se rovnat detail_h). */
    float split_x;         /**< Střed oddělovače mřížka / seznam x (souřadnice obrazovky). */
    float split_y;         /**< Střed oddělovače y. */
    int color_mode;        /**< Režim obarvení (hodnoty INI). */
    int active_tab;        /**< Aktivní záložka. */
    int row_count;         /**< Počet řádků seznamu. */
    int selected_row;      /**< Index vybraného řádku, -1 = žádný. */
    int hover_cells;       /**< Počet buněk zvýrazněných kvůli hoveru řádku seznamu. */
    int visible_rows;      /**< Počet řádků seznamu vykreslených v posledním snímku. */
    char activity_line[96];/**< Řádek aktivity detailu ("IN/s ..."). */
    char peek_line[96];    /**< Řádek peeku detailu. */
    char flags_line[300];  /**< Řádek příznaků detailu ("Flags: -" = žádný příznak). */
    char status_line[96];  /**< Stavový řádek. */
} st_BUS_WINDOW_DEBUG;

/**
 * @brief Vyplní stav okna pro testy.
 * @param out Cíl (nesmí být NULL).
 */
void bus_window_debug_get(st_BUS_WINDOW_DEBUG *out);

/**
 * @brief Nastaví režim obarvení a sledování (testy, smoke screenshoty stavů).
 * @param color_mode 0 Owners, 1 Activity, 2 Drive; -1 = beze změny.
 * @param tracking   0 vypnout, 1 zapnout (jako checkbox Track activity); -1 = beze změny.
 */
void bus_window_debug_set(int color_mode, int tracking);

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* BUS_WINDOW_H */
