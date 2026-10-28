/**
 * @file   bus_window.h
 * @brief  Okno debuggeru Bus Map - kdo vlastní který port a MMIO adresu.
 *
 * Adresa-centrický pohled na sběrnici Z80: vlevo mřížka 16 x 16 (porty
 * 00h-0FFh, nebo okno MMIO 0E000h-0E0FFh), vpravo seznam vlastníků aktivní
 * mřížky (mini buňka se zkratkou v barvě buňky = legenda, název, rozsahy,
 * budič, ikona stavu rozlišená tvarem, Connect / Disconnect u zařízení
 * devbus) a pod ním sbalitelný detail vybrané adresy (stav slovem,
 * aktivita, poslední hodnoty, peek bez vedlejších efektů, příznaky slovy,
 * skok do I/O Ports, reset aktivity, breakpointy IORQ).
 * Data dodává vrstva bus_map (src/emulator/debugger/bus_map.h): snímek se
 * bere každý snímek obrazu pod device_state_lock(), peek jde přes dbgapi do
 * emu vlákna (nikdy pod zámkem). Živou aktivitu zapíná sdílený čítač
 * sledování s oknem I/O Ports (io_activity_tracking_set_client()).
 *
 * Rozměry se odvozují z GetFontSize() (buňka podle nejširší zkratky,
 * řádek seznamu podle GetFrameHeight()); pod přirozenou šířkou obsahu se
 * okno nepřeskládává, jen dostane vodorovný posuvník. Při nedostatku výšky
 * se buňka zmenší až k mezi čitelnosti (zkratka + 2 px z každé strany),
 * pod ní levá část roluje svisle; pravá část dostane svislý posuvník, když
 * se seznam (záhlaví + 3 řádky) a detail pevné výšky nevejdou.
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
 * tracking_active, detail_open (bool, výchozí 1 = detail vybrané adresy
 * rozbalený). Starší klíče inspector_width a show_legend se už
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

/**
 * @brief Střed ikony stavu řádku seznamu z posledního kreslení (testy simulující hover s tooltipem).
 * @param row Index řádku v pořadí seznamu.
 * @param x   Výstup x (smí být NULL).
 * @param y   Výstup y (smí být NULL).
 * @return false = řádek se v posledním snímku nekreslil (mimo clipper, nebo neexistuje).
 */
bool bus_window_state_icon_center(int row, float *x, float *y);

/**
 * @brief Střed šipky sbalení / rozbalení detailu z posledního kreslení (testy simulující klik).
 * @param x Výstup x (smí být NULL).
 * @param y Výstup y (smí být NULL).
 * @return false = okno se v posledním snímku nekreslilo.
 */
bool bus_window_detail_toggle_center(float *x, float *y);

/**
 * @brief Střed záhlaví sloupce ikony stavu (značka "(?)") z posledního kreslení (testy simulující hover s tooltipem).
 * @param x Výstup x (smí být NULL).
 * @param y Výstup y (smí být NULL).
 * @return false = seznam se v posledním snímku nekreslil.
 */
bool bus_window_state_header_center(float *x, float *y);

/** @brief Přepne záložku (0 Ports, 1 MMIO; jiná hodnota = Ports) při příštím kreslení. */
void bus_window_show_tab(int tab);

/**
 * @brief Stav okna pro testy (geometrie, texty detailu, výběr).
 *
 * Hodnoty z posledního kreslení; texty jsou v jazyce UI. Jen pro testy
 * a měření, žádná jiná vrstva na ně nespoléhá.
 */
typedef struct st_BUS_WINDOW_DEBUG {
    float font_size;       /**< GetFontSize() posledního kreslení. */
    float cell;            /**< Velikost buňky mřížky v px. */
    float cell_min;        /**< Buňka přirozené velikosti okna (nejširší zkratka obou záložek + vzduch 0,1 em). */
    float cell_floor;      /**< Mez čitelnosti při nedostatku výšky (nejširší zkratka obou záložek a popisek řádku + 2 x 2 px); <= cell_min. */
    float cell_fit_h;      /**< Buňka, se kterou by se mřížka celá vešla do výšky těla. */
    bool grid_full;        /**< Mřížka se v posledním snímku vešla celá (cell <= cell_fit_h), levá část neroluje. */
    float min_full_h;      /**< Nejmenší výška okna, při které je mřížka s buňkou cell_floor celá vidět. */
    float natural_win_w;   /**< Šířka okna při prvním otevření bez omezení plochou: natural_w + 2 x WindowPadding.x. */
    float natural_h;       /**< Výška okna při prvním otevření bez omezení plochou (mřížka s buňkou cell_min). */
    float min_win_h;       /**< Nejmenší výška okna (omezení SetNextWindowSizeConstraints, 13 em). */
    float gap;             /**< Mezera mezi buňkami mřížky. */
    float grid_x;          /**< Levý horní roh buňky 00 x (souřadnice obrazovky, měření snímků). */
    float grid_y;          /**< Levý horní roh buňky 00 y. */
    float badge_r;         /**< Poloměr odznaku posledního kreslení (zmenšený s buňkou). */
    float badge_off;       /**< Posun středu odznaku z rohu buňky dovnitř. */
    float natural_w;       /**< Přirozená (minimální) šířka obsahu okna: mřížka + oddělovač + seznam. */
    float list_w;          /**< Skutečná šířka pravé části. */
    float list_min_w;      /**< Minimální šířka pravé části z textů. */
    float row_h;           /**< Výška řádku seznamu. */
    float detail_h;        /**< Výška detailu změřená při posledním kreslení (bez mezery za poslední řadou). */
    float detail_fixed_h;  /**< Pevná výška detailu, se kterou se dělil prostor seznam / detail (musí se rovnat detail_h). */
    float split_x;         /**< Střed oddělovače mřížka / seznam x (souřadnice obrazovky). */
    float split_y;         /**< Střed oddělovače y. */
    float list_h;          /**< Výška tabulky seznamu vlastníků posledního kreslení. */
    float list_need_h;     /**< Nejmenší výška seznamu: záhlaví + 3 řádky; pod list_need_h + mezera + detail_fixed_h pravá část roluje. */
    float list_scroll_y;   /**< Odrolování seznamu vlastníků (ScrollY tabulky) v posledním snímku. */
    float right_bottom;    /**< Spodní hrana viditelné plochy pravé části ##bus_right (souřadnice obrazovky). */
    float right_scroll_y;  /**< Odrolování pravé části ##bus_right. */
    float right_scroll_max_y; /**< Nejvyšší odrolování pravé části (> 0 = obsah přesahuje; s NoScrollbar se ale rolovat nedá, viz right_scrollbar). */
    bool right_scrollbar;  /**< Pravá část má v posledním snímku viditelný svislý posuvník (nízké okno, P01). */
    float btn_bottom;      /**< Spodní hrana tlačítka Break on OUT detailu (souřadnice obrazovky; 0 = sbalený detail). */
    float owner_text_w;    /**< Šířka nejdelšího zobrazeného názvu vlastníka (obě záložky). */
    float owner_col_x;     /**< Začátek obsahu sloupce Owner x (souřadnice obrazovky). */
    float ports_col_x;     /**< Začátek obsahu sloupce Ports / Addresses x; ports_col_x - owner_col_x - owner_text_w = mezera za nejdelším názvem. */
    float hdr_tip_w;       /**< Šířka okna tooltipu záhlaví sloupce ikony stavu (0 = v posledním snímku nezobrazený). */
    char ports_header[32]; /**< Text záhlaví sloupce rozsahů ("Ports" / "Addresses" podle záložky). */
    int color_mode;        /**< Režim obarvení (hodnoty INI). */
    int active_tab;        /**< Aktivní záložka. */
    int row_count;         /**< Počet řádků seznamu. */
    int selected_row;      /**< Index vybraného řádku, -1 = žádný. */
    int hover_cells;       /**< Počet buněk zvýrazněných kvůli hoveru řádku seznamu. */
    int visible_rows;      /**< Počet řádků seznamu vykreslených v posledním snímku. */
    bool detail_open;      /**< Detail vybrané adresy je rozbalený (persist detail_open). */
    char activity_line[96];/**< Řádek aktivity detailu ("IN/s ..."; při sbaleném detailu z posledního rozbaleného kreslení). */
    char peek_line[96];    /**< Řádek peeku detailu (totéž). */
    char flags_line[300];  /**< Řádek příznaků detailu ("Flags: -" = žádný příznak; totéž). */
    char status_line[96];  /**< Stavový řádek. */
    char state_tip[96];    /**< Text tooltipu ikony stavu zobrazeného v posledním snímku ("State: connected"); "" = žádný. */
    char row_tip_state[96];/**< Řádek stavu tooltipu řádku seznamu zobrazeného v posledním snímku; "" = žádný. */
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
