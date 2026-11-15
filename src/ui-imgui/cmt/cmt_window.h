/**
 * @file   cmt_window.h
 * @brief  Okno Virtual CMT: deck (displej nad ovládáním), rozvržení Compact a Full.
 *
 * Okno kreslí jen z kopie stavu st_CMT_VIEW (cmt_view.h); jádro kazety mění
 * jen frontami požadavků (replay_host_cmt(), cmt_request_*) a stávajícím
 * automatem akcí s páskou (imgui_cmt_tape_action()). Barvy jsou pojmenované
 * tokeny z cmt_tokens.h, rozměry se odvozují z výšky rámečku a šířek textů.
 *
 * Viditelnost okna je příznak g_gui->showVirtualCmtWindow: otevírá ho volba
 * --cmt, Alt+C (globální zkratka; při fokusu okna ho Alt+C zavře) i menu
 * HW & Devices > CMT > Show Virtual CMT. Režim Direct (CMT hack) je stav decku,
 * samostatné okno "CMT Hack is activated!" ani Tape Filelist už nejsou
 * Zkratky okna jsou jen s Ctrl a jen při jeho fokusu.
 *
 * Rozvržení: Compact = jen deck a řádek zpráv; Full = deck, pod ním tabulka
 * bloků, sbalitelná sekce vlastností a řádek zpráv. Přepíná se jen ručně
 * (menu View, tlačítko Blocks); každé rozvržení si pamatuje vlastní
 * velikost okna, sekce vlastností svůj stav (INI sekce [VIRTUAL_CMT_UI]);
 * šířky, pořadí a viditelnost sloupců tabulky ukládá ImGui do svého INI.
 *
 * Úpravy pásky: vícenásobný výběr řádků, Delete, Move up / down,
 * tažení řádku, Import file... / Import file here..., Export tape as WAV...
 * a soubor přetažený z OS (imgui_cmt_window_os_drop_event()); po vlastní
 * úpravě okno obnoví výběr podle výsledku zásahu (cmt_view.h).
 *
 * Analýza záznamu: kontextové menu řádku Analyze... a menu Tape >
 * Analyze recording... otevřou nad surovým blokem dialog analýzy
 * (cmt_analyze_dialog.h), který okno kreslí jako svůj modál.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef CMT_WINDOW_H
#define CMT_WINDOW_H

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief Rozvržení okna Virtual CMT. */
    typedef enum en_CMT_WINDOW_LAYOUT
    {
        CMT_WINDOW_LAYOUT_COMPACT = 0, /**< jen deck a řádek zpráv */
        CMT_WINDOW_LAYOUT_FULL = 1,    /**< deck, seznam bloků a řádek zpráv */
        CMT_WINDOW_LAYOUT_COUNT        /**< počet rozvržení (sentinel) */
    } en_CMT_WINDOW_LAYOUT;

    /**
     * @brief Vyplní kopii stavu z jádra a vykreslí okno Virtual CMT (jednou za snímek).
     *
     * Zavolá cmt_view_fill() a cmt_view_request_lengths() (délky bloků se
     * doplňují jen ve STOP, viz cmt_view.h) a pak imgui_cmt_window_draw().
     *
     * @param p_open Viditelnost okna (g_gui->showVirtualCmtWindow); zavřením
     *        okna (křížek) se nastaví na false.
     * @pre UI vlákno, drží device_state_lock() (main_window.cpp); akce
     *      automatu pásky se jen odloží, provede je imgui_cmt_tape_run_deferred()
     *      po uvolnění zámku.
     */
    void imgui_cmt_window(bool *p_open);

    /**
     * @brief Registruje persistenci okna: sekce INI [VIRTUAL_CMT_UI].
     *
     * Klíče: layout (0 Compact, 1 Full), compact_w, compact_h, full_w,
     * full_h (velikost okna v px pro každé rozvržení; 0 = výchozí velikost
     * odvozená z výšky rámečku), props_open (sekce vlastností rozbalená).
     * Opakované volání nic nedělá.
     *
     * @pre g_cfgmain existuje (po cfgmain_init()), volá se jednou při startu UI.
     */
    void imgui_cmt_window_cfg_init(void);

    /** @brief Aktuální rozvržení okna. @return Compact nebo Full. */
    en_CMT_WINDOW_LAYOUT imgui_cmt_window_get_layout(void);

    /**
     * @brief Přepne rozvržení (jako menu View nebo tlačítko Blocks).
     *
     * Velikost okna se v příštím snímku nastaví na pamatovanou velikost
     * nového rozvržení (nebo výchozí, dosud žádná nebyla).
     *
     * @param layout Nové rozvržení (mimo rozsah = Compact).
     * @pre UI vlákno.
     */
    void imgui_cmt_window_set_layout(en_CMT_WINDOW_LAYOUT layout);

#ifdef __cplusplus
}

#include "cmt_view.h"
#include "libs/imgui/imgui.h"

/**
 * @brief Pracovní plocha hlavního okna aplikace, podle které se omezuje velikost okna kazety a jeho dialogů.
 *
 * Velikost pracovní plochy hlavního viewportu (bez hlavního menu); testy ji
 * mohou nahradit (imgui_cmt_window_test_set_work_size()).
 *
 * @return Velikost v px (aspoň 1 x 1).
 * @pre UI vlákno uvnitř snímku ImGui.
 */
ImVec2 imgui_cmt_window_work_size(void);

/**
 * @brief Umístí příští modál doprostřed okna Virtual CMT, celý uvnitř pracovní plochy jeho monitoru.
 *
 * Velikost modálu z minulého snímku (@p size) určí levý horní roh tak, aby
 * střed modálu ležel na středu okna kazety a modál nepřesahoval pracovní
 * plochu monitoru, na kterém okno je (bez oken mimo hlavní okno pracovní
 * plochu viewportu; velký dialog analýzy u okraje obrazovky). Viewport se
 * nepřipíná: modál větší než okno kazety vytažené jako samostatné okno OS
 * dostane vlastní okno a jde přetáhnout kamkoli. Bez
 * známé velikosti (první snímek) jako imgui_cmt_window_modal_next_pos()
 * s podmínkou @p cond. Okno kazety se v tomto ani minulém snímku
 * nekreslilo: nic (výchozí poloha ImGui).
 *
 * @param size Velikost modálu z minulého snímku (x <= 0 = neznámá).
 * @param cond Podmínka ImGui (ImGuiCond_Appearing, ImGuiCond_Always pro
 *        nové vystředění po změně velikosti stavem dialogu).
 * @return true = poloha nastavená; false = okno kazety se nekreslí (nic).
 * @pre UI vlákno, před BeginPopupModal().
 */
bool imgui_cmt_window_modal_center(ImVec2 size, ImGuiCond cond);

/**
 * @brief Umístí příští nemodální okno vedle okna Virtual CMT tak, aby nezakrylo tabulku bloků (okno Tape check).
 *
 * Pořadí: vpravo od okna
 * kazety, když se tam celé vejde do pracovní plochy monitoru okna kazety (bez oken mimo hlavní okno jeho viewportu) (svisle
 * od horního okraje okna kazety, omezeno do plochy); jinak pod ním; jinak
 * nad ním; jinak vlevo. Nevejde-li se nikam vedle okna (okno kazety zabírá
 * skoro celou plochu, např. 175 % na 1920 x 1040), leží okno v pásu okna
 * kazety nad tabulkou bloků (titulek, menu, deck) nebo pod ní (vlastnosti,
 * řádek zpráv) - v tom, kde se vejde, jinak ve vyšším - zarovnané k pravému
 * okraji okna kazety; tabulku bloků nikdy nepřekryje, pokud je pás aspoň
 * @p min_h vysoký. Návratová hodnota pak omezí výšku okna na výšku pásu.
 * Teprve když ani to nejde, doprostřed okna kazety (celé v ploše).
 * Okno kazety se nekreslí: doprostřed pracovní plochy hlavního viewportu.
 *
 * @param size Velikost okna z minulého snímku (x <= 0 = neznámá: střed okna kazety s pivotem 0,5).
 * @param cond Podmínka ImGui.
 * @param min_h Nejmenší rozumná výška okna (px) pro umístění do pásu nad nebo pod tabulkou.
 * @return Největší výška okna v px, kterou poloha snese (FLT_MAX = bez omezení).
 * @pre UI vlákno, před Begin(). Obdélník tabulky bloků je z posledního snímku okna kazety.
 */
float imgui_cmt_window_place_beside(ImVec2 size, ImGuiCond cond, float min_h = 0.0f);

/**
 * @brief Šířka tlačítka okna kazety a jeho dialogů s popiskem (jedno pravidlo tvaru tlačítek).
 *
 * Text + 2 x FramePadding.x + 4u (u = písmo / 7): při 28 px vodorovná
 * výplň textu 12 px na každé straně - stejně jako tlačítka decku a řádku
 * pásky okna kazety.
 *
 * @param label Popisek (_L(), část za "##" se neměří).
 * @return Šířka v px.
 * @pre UI vlákno uvnitř snímku ImGui.
 */
float imgui_cmt_button_w(const char *label);

/**
 * @brief Tlačítko patičky a voleb dialogů okna kazety (jeden tvar a výška).
 *
 * Plocha control (#2E2E36), hrana control-border (#6A6A78) 1 px, bez
 * zaoblení, výška rámečku (FrameHeight); primární (výchozí) tlačítko plochou
 * segment-on (#2B6CC4). Zakázané tlačítko (důvod @p reason) se kreslí
 * přes BeginDisabled() (krytí DisabledAlpha) a důvod ukáže tooltip i nad
 * zakázaným tlačítkem. Barvy a hranu nastaví samo, nezávisle na tom, zda se
 * kreslí uvnitř okna kazety (modály) nebo v samostatném okně (Tape check).
 *
 * @param label Popisek (_L()).
 * @param width Šířka (<= 0 = imgui_cmt_button_w()).
 * @param reason Důvod zákazu (NULL = povolené).
 * @param primary Výchozí tlačítko (plocha segment-on, jen povolené).
 * @return true = stisknuté a povolené.
 * @pre UI vlákno uvnitř okna ImGui.
 */
bool imgui_cmt_button(const char *label, float width, const char *reason, bool primary);

/**
 * @brief Sbalitelná hlavička oddílu jako hlavička vlastností okna kazety.
 *
 * Řádek výšky rámečku přes @p width: plocha panel (#1A1A1E) s rámečkem
 * border (#3A3A44), pod myší control-hover, šipka (doprava / dolů, 0,7
 * výšky řádku) u levého okraje s odstupem 2u a za ní nadpis (text #FFFFFF)
 * - stejné kreslení jako hlavička "Block properties" okna kazety. Klik
 * (a Enter / mezerník s fokusem navigace) přepne stav.
 *
 * @param id Stabilní ID prvku ("##..."). @param title Nadpis (přeložený, bez "##").
 * @param open Stav před kliknutím. @param width Šířka řádku (px).
 * @return Nový stav (true = rozbaleno).
 * @pre UI vlákno uvnitř okna ImGui; kurzor na levém horním rohu řádku.
 * @post Kurzor pod řádkem (položka InvisibleButton).
 */
bool imgui_cmt_section_header(const char *id, const char *title, bool open, float width);

/**
 * @brief Leží aktuální okno ImGui (modál, okno nástroje) aspoň zčásti mimo pracovní plochu svého viewportu?
 *
 * Společná kontrola dialogů okna kazety: okno, které vyrostlo (delší text,
 * větší písmo) nebo zůstalo z dřívějšího měřítka mimo plochu, se znovu
 * umístí. Ve snímku, kdy se okno objevilo, false (poloha ještě není platná).
 *
 * @return true = vyčnívá z pracovní plochy.
 * @pre UI vlákno mezi Begin() a End() okna.
 */
bool imgui_cmt_window_current_outside_work(void);

/**
 * @brief Zavře popup s daným ID v zásobníku popupů ImGui, je-li otevřený (opuštění dialogu mimo jeho modál).
 *
 * Cesty mimo modál (zavřené okno kazety, hlídač, test) by jinak nechaly
 * záznam modálu v zásobníku; imgui_modal_can_open() i zkratky okna by dál
 * viděly "otevřený popup".
 *
 * @param[in,out] popup_id ID popupu (0 = nic); po návratu 0.
 * @pre UI vlákno; mimo snímek ImGui nebo bez kontextu jen vynuluje ID.
 */
void imgui_cmt_close_popup(ImGuiID *popup_id);

/**
 * @brief Vybraný řádek tabulky okna kazety (ukazují ho vlastnosti) a čítač pásky, ke kterému patří.
 *
 * Dialog informací o pásce si ho zapamatuje při otevření a po zápisu
 * (informační bloky na začátku pásky posunou čísla bloků) vybere týž blok
 * na novém čísle (cmt_view_select_after_edit()).
 *
 * @param[out] tape_serial Čítač výměn pásky výběru (smí být NULL).
 * @return Index bloku; -1 = bez výběru nebo víc vybraných řádků.
 * @pre UI vlákno.
 */
int imgui_cmt_window_selected_block(unsigned *tape_serial);

/**
 * @brief Vykreslí okno Virtual CMT z dané kopie stavu (bez čtení jádra).
 *
 * Používá ho imgui_cmt_window() a testy s připravenými stavy
 * (cmt_view_make_fixture()). Akce menu Tape a zkratek (uložení, export,
 * automat imgui_cmt_tape_action()) čtou jádro pod zámkem samy, proto i test
 * volá funkci pod device_state_lock(). Zkratky s Ctrl obsluhuje jen při
 * fokusu okna; Alt+C při fokusu okna nastaví *p_open = false.
 *
 * @param v Kopie stavu (nesmí být NULL); zprávu a výběr okno nemění.
 * @param p_open Viditelnost okna (nesmí být NULL; Alt+C a křížek ji vypnou).
 * @pre UI vlákno, uvnitř ImGui snímku, drží device_state_lock().
 * @post Požadavky z kliknutí a zkratek jsou zařazené ve frontách
 *       (replay_host_cmt(), cmt_request_*, automat imgui_cmt_tape_action()).
 */
void imgui_cmt_window_draw(st_CMT_VIEW *v, bool *p_open);

/**
 * @brief Vybere blok v tabulce okna Virtual CMT a odroluje ho do pohledu (okno Tape check).
 *
 * Výběr jednoho řádku s čítačem výměn pásky, ke kterému číslo bloku patří
 * (jiná páska = nejbližší kreslení výběr zruší), řádek se odroluje do
 * viditelné části (fokus navigace dostane, jen má-li okno kazety fokus).
 * Okno se otevře (g_gui->showVirtualCmtWindow), sbalené se rozbalí,
 * v rozvržení Compact se přepne na Full (seznam bloků je jen tam). Dopředu
 * se nevynáší: okno Tape check, které na řádek ukázalo, zůstane vidět.
 *
 * @param block_id Index bloku (záporný = nic).
 * @param tape_serial Čítač výměn pásky, ke kterému @p block_id patří.
 * @pre UI vlákno.
 */
void imgui_cmt_window_select_block(int block_id, unsigned tape_serial);

union SDL_Event;

/**
 * @brief Přetažení souboru z OS (SDL_EVENT_DROP_*): zapamatuje polohu a soubory pro okno Virtual CMT.
 *
 * Volá ho obsluha událostí hlavního okna (myimgui_event_cb()) pro každou
 * událost; jiné než SDL_EVENT_DROP_BEGIN, _POSITION, _FILE a _COMPLETE
 * ignoruje. Polohu převede na souřadnice ImGui stejně jako backend
 * imgui_impl_sdl3 u pohybu myši (okno SDL podle windowID musí být
 * viewport ImGui; s ViewportsEnable + poloha okna SDL) a předá ji ImGui
 * jako polohu myši (io.AddMousePosEvent(), io.AddMouseViewportEvent()),
 * aby o oknu pod kurzorem rozhodlo ImGui. Událost okna SDL, které není
 * viewportem ImGui, se ignoruje. Soubory sebrané do SDL_EVENT_DROP_COMPLETE
 * vyhodnotí nejbližší imgui_cmt_window_draw(): nad tabulkou bloků import
 * na místo pod kurzorem, nad jinou částí okna otevření prvního souboru
 * obvyklým postupem, jinde nic. Během tažení (SDL_EVENT_DROP_POSITION) okno
 * ukazuje nad tabulkou čáru místa vložení.
 *
 * @param event Událost SDL (nesmí být NULL).
 * @pre UI vlákno, ImGui kontext aplikace je aktuální (po ImGui_ImplSDL3_ProcessEvent()).
 * @post Žádný zásah se tu nezařazuje; jen stav přetažení okna.
 */
void imgui_cmt_window_os_drop_event(const union SDL_Event *event);

#ifdef MZTEST_HEADLESS
#include "libs/mztape/mztape.h"
#include <string>
#include <vector>

/**
 * @brief Jen testy: obdélník jednoho prvku okna z posledního snímku.
 *
 * @invariant min <= max po složkách; clip_min / clip_max je ořezový
 *            obdélník okna (ImGuiWindow::ClipRect) v okamžiku kreslení prvku.
 */
typedef struct st_CMT_WINDOW_TEST_ITEM
{
    std::string name; /**< jméno prvku ("btn_play", "counter", ...) */
    std::string text; /**< zobrazený text; u tlačítek transportu druh ikony ("play", "pause", "record", "record_active", ...) */
    bool disabled;    /**< prvek kreslený jako zakázaný (BeginDisabled) */
    ImVec2 min;       /**< levý horní roh (souřadnice obrazovky) */
    ImVec2 max;       /**< pravý dolní roh */
    ImVec2 clip_min;  /**< ořez okna při kreslení, levý horní roh */
    ImVec2 clip_max;  /**< ořez okna při kreslení, pravý dolní roh */
    bool container;   /**< obal jiných prvků (panel decku, plocha počítadla): z kontroly překryvu se vynechává */
    bool deck;        /**< prvek patří decku (ne řádku zpráv ani seznamu bloků) */
    bool right;       /**< prvek je ukotvený k pravému okraji decku (ne k levému) */
    bool scroll_y;    /**< prvek je ve svisle posouvané oblasti (řádek tabulky, obsah vlastností): svisle smí přesahovat ořez */
    bool popup;       /**< položka kontextového menu (vlastní okno popupu, ne vnitřek okna) */
    std::string reason; /**< důvod zákazu z tooltipu ("" = povolený nebo prvek bez důvodu) */
} st_CMT_WINDOW_TEST_ITEM;

/** @brief Jen testy: prvky z posledního vykresleného snímku okna. @return Seznam (platí do dalšího snímku). */
const std::vector<st_CMT_WINDOW_TEST_ITEM> &imgui_cmt_window_test_items(void);

/**
 * @brief Jen testy: vnitřní obdélník okna (bez titulku, menu a posuvníků) z posledního snímku.
 * @param[out] min Levý horní roh. @param[out] max Pravý dolní roh.
 */
void imgui_cmt_window_test_inner_rect(ImVec2 *min, ImVec2 *max);

/**
 * @brief Jen testy: nastaví výběr řádku tabulky (výběr je stav okna).
 * @param block_id Index bloku (-1 = bez výběru). @param tape_serial st_CMT_VIEW::tape_serial, ke kterému výběr patří.
 */
void imgui_cmt_window_test_set_selection(int block_id, unsigned tape_serial);

/** @brief Jen testy: vybraný řádek tabulky (poslední vybraný, ukazují ho vlastnosti). @return Index bloku, -1 = bez výběru. */
int imgui_cmt_window_test_get_selection(void);

/** @brief Jen testy: všechny vybrané řádky tabulky. @return Indexy bloků vzestupně. */
std::vector<int> imgui_cmt_window_test_get_selected(void);

/**
 * @brief Jen testy: nastaví vícenásobný výběr řádků.
 * @param blocks Indexy bloků (prázdný = bez výběru); poslední je řádek, který ukazují vlastnosti.
 * @param tape_serial st_CMT_VIEW::tape_serial, ke kterému výběr patří.
 */
void imgui_cmt_window_test_set_selected(const std::vector<int> &blocks, unsigned tape_serial);

/** @brief Jen testy: je otevřený modál potvrzení mazání (Delete blocks)? @return true = ano. */
bool imgui_cmt_window_test_delete_modal_open(void);

/** @brief Jen testy: ID tabulky bloků (ImGui::TableFindByID()). @return ID, 0 = tabulka ještě nebyla vykreslena. */
ImGuiID imgui_cmt_window_test_table_id(void);

/** @brief Jen testy: měla tabulka bloků v posledním snímku vodorovný posuvník? @return true = ano. */
bool imgui_cmt_window_test_table_scroll_x(void);

/** @brief Jen testy: stav sekce vlastností. @return 1 = rozbalená, 0 = sbalená. */
unsigned imgui_cmt_window_test_props_open(void);

/**
 * @brief Jen testy: rozbalí nebo sbalí sekci vlastností (jako klik na její hlavičku).
 * @param open 1 = rozbalit, 0 = sbalit.
 */
void imgui_cmt_window_test_set_props_open(unsigned open);

/** @brief Jen testy: měl obsah vlastností v posledním snímku svislý posuvník? @return true = ano. */
bool imgui_cmt_window_test_props_scrollbar(void);

/**
 * @brief Jen testy: titulek okna předaný do Begin() v posledním snímku.
 *
 * ImGuiWindow::Name se po vytvoření okna mění jen výjimečně, titulek
 * v titulkovém pruhu ale kreslí ImGui z předaného textu.
 *
 * @return Titulek včetně "###virtual_cmt" (platí do dalšího snímku).
 */
const char *imgui_cmt_window_test_title(void);

/** @brief Jen testy: minimální velikost okna spočítaná v posledním snímku. @return Velikost v px. */
ImVec2 imgui_cmt_window_test_min_size(void);

/**
 * @brief Jen testy: prodlouží každý přeložený text okna o 40 % (pseudo-překlad).
 *
 * Obal kolem _(), C_() a _L() existuje jen v testovacím sestavení
 * (MZTEST_HEADLESS, cmt_view_test_set_stretch()); prodlužuje texty okna
 * i texty kopie stavu (typy, stavy a druhy bloků v cmt_view.cpp, při
 * dalším sestavení kopie). V aplikaci se texty nemění.
 *
 * @param on true = texty prodlužovat.
 */
void imgui_cmt_window_test_set_stretch(bool on);

/** @brief Jen testy: vrátí rozvržení, pamatované velikosti a režim počítadla na výchozí stav. */
void imgui_cmt_window_test_reset(void);

/**
 * @brief Jen testy: nahradí pracovní plochu hlavního okna (malá obrazovka).
 * @param size Velikost v px; x <= 0 = skutečná pracovní plocha viewportu,
 *        y <= 0 = skutečná výška (omezí se jen šířka).
 */
void imgui_cmt_window_test_set_work_size(ImVec2 size);

/**
 * @brief Jen testy: zapomene registraci sekce INI [VIRTUAL_CMT_UI].
 *
 * Další imgui_cmt_window_cfg_init() ji zaregistruje znovu do aktuálního
 * g_cfgmain a načte z něj hodnoty (zkouška zápisu a čtení přes soubor).
 */
void imgui_cmt_window_test_cfg_forget(void);

/**
 * @brief Jen testy: potvrdí editor vlastních pulzů bloku, jako by uživatel stiskl OK (test modál nekreslí).
 *
 * Stejná cesta jako potvrzení editoru v okně: zásah jen nad páskou, ze které
 * blok pochází (čítač výměn pásky z doby otevření editoru).
 *
 * @param v Kopie stavu. @param owner Blok. @param pulses Pulzy.
 */
void imgui_cmt_window_test_confirm_block_pulses(const st_CMT_VIEW *v, int owner, const st_MZTAPE_PULSES_LENGTH *pulses);
#endif /* MZTEST_HEADLESS */

#endif /* __cplusplus */

#endif /* CMT_WINDOW_H */
