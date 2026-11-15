/**
 * @file   cmt_window.cpp
 * @brief  Okno Virtual CMT: tokeny, velké počítadlo, deck, rozvržení Compact a Full, režimy načítání, zkratky.
 *
 * Deck (SPEC v2, "Deck = displej nad ovládáním") má čtyři řádky:
 * 1. páska: název souboru (elipsa uprostřed, tooltip s cestou), "Modified",
 *    vpravo Open... / Eject / Blocks (bez pásky "No tape", Open tape... /
 *    New tape / Blocks);
 * 2. displej: vlevo velké počítadlo (skutečný řez monospace 2,2x
 *    základní velikosti, PushFont(mono, velikost)) s popiskem, vpravo
 *    "Block n / m", název bloku, průběh bloku a stavový řádek se stavem
 *    a motorem;
 * 3. transport: Rewind, Previous block, Play / Pause, Stop, Next block,
 *    vpravo Record (ikony kreslené DrawListem);
 * 4. vlevo "Signal speed" a výběr rychlosti, vpravo "Loading" a přepínač
 *    Tape | Boost | Direct.
 * Pod deckem je v rozvržení Full tabulka bloků (ImGuiListClipper, výběr
 * řádku je stav okna, kontextové menu řádku) a sbalitelná sekce
 * vlastností vybraného bloku (bez výběru vlastnosti pásky; výchozí
 * sbalená); dole vždy řádek zpráv. Tabulka i vlastnosti ukazují jen celé
 * řádky a posouvají se po celých řádcích.
 *
 * Rozměry: jednotkou je výška rámečku (GetFrameHeight()) a výška řádku
 * textu, mezery jsou násobky 4 px škálované písmem (GetFontSize() / 7, při
 * 28 px = 4 px), šířky prvků z CalcTextSize(). Minimální velikost okna se
 * spočítá z nejdelších textů všech stavů, aby nic nebylo oříznuté a okno
 * při změně stavu neposkakovalo.
 *
 * Úpravy pásky: tabulka má vícenásobný výběr (BeginMultiSelect,
 * Ctrl+klik, Shift+klik, Ctrl+A při fokusu tabulky; vlastnosti při více
 * vybraných jen "N blocks selected"), kontextové menu řádku má za Set
 * speed položky Move up (Alt+Up), Move down (Alt+Down), Import file
 * here... a Delete / Delete N blocks (Del), menu Tape Import file...
 * (Ctrl+I) a Export tape as WAV.... Mazání neuložených bloků potvrzuje
 * modál Delete blocks (výchozí Cancel). Řádek jde přetáhnout myší
 * (přesun jednoho bloku, čára místa vložení v barvě accent); soubor
 * přetažený z OS (imgui_cmt_window_os_drop_event()) nad tabulku se
 * importuje na místo pod kurzorem, nad jinou část okna otevře. Po vlastní
 * úpravě si okno obnoví výběr podle výsledku zásahu
 * (cmt_view_take_select_after_edit()). Všechny úpravy jen ve STOP a mimo
 * záznam a přehrávání vstupů; jdou jako zásahy fronty s čítačem výměn
 * pásky (imgui_cmt_tape_remove_blocks(), imgui_cmt_tape_move_block(),
 * imgui_cmt_tape_import_paths()).
 *
 * Tažení řádku nese čítač výměn pásky ze začátku
 * tažení (změna pásky ho zruší s varováním) a během tažení se klávesy úprav
 * ignorují; tabulka se u okraje při tažení posouvá; druhé Alt+šipka před
 * výsledkem předchozího přesunu se zapamatuje; zakázaná klávesa úpravy
 * ukáže důvod v řádku zpráv; vybrané řádky mají pruh accent u levé hrany,
 * popisek taženého bloku je neprůhledný s číslem bloku; modály okna se
 * otevírají uprostřed okna kazety (imgui_cmt_window_modal_next_pos());
 * přetažení souboru, které se nepoužije, ohlásí varování.
 *
 * Analýza záznamu: kontextové menu řádku má za Set speed položku
 * Analyze..., menu Tape Analyze recording... (jediný vybraný řádek); obě
 * jen nad surovým blokem 0x18, ve STOP a mimo záznam a přehrávání vstupů
 * (cw_reason_analyze()). Dialog (cmt_analyze_dialog.cpp) okno kreslí jako
 * svůj modál a zavřené okno ho opustí.
 *
 * Nástroje nad páskou: menu Tape Tape information... (modál
 * cmt_tapeinfo_dialog.cpp, jen ve STOP a mimo záznam a přehrávání vstupů)
 * a Check tape (nemodální okno cmt_check_window.cpp, kreslí ho
 * main_window.cpp; jen čte, jde v každém stavu); kontextové menu řádku
 * Convert to profile... hned za Analyze... (modál cmt_convert_dialog.cpp).
 * Klik na nález okna kontroly vybere blok a odroluje ho do pohledu, okno
 * kazety dopředu nevynáší (imgui_cmt_window_select_block()), okno kontroly
 * tak zůstane vidět.
 *
 * Okno čte stav jen z kopie st_CMT_VIEW (cmt_view.h) a jádro mění jen
 * frontami požadavků: transport replay_host_cmt(), režim načítání
 * cmt_request_set_loading_mode() (CMT hack a CPU Boost jedním zásahem),
 * rychlost cmt_request_set_speed(), akce s páskou automatem
 * imgui_cmt_tape_action() (dotazy na nahrávání a neuložené změny).
 * Tlačítka, zkratky (jen s Ctrl, při fokusu okna) a menu Tape sdílejí
 * jedna pravidla zákazu (cw_actions()). Výsledky zásahů a záznamu jsou
 * v řádku zpráv (st_CMT_VIEW::message, cmt_view_post_message()).
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

#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <glib.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <SDL3/SDL.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"

#include "emulator/cfgmain.h"
#include "emulator/i18n_lang.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "emulator/replay.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mztape/mztape.h"

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "device_request.h"
#include "imgui_cmt.h"
#include "ImGuiCmt.hpp"
#include "cmt_view.h"
#include "cmt_tokens.h"
#include "cmt_window.h"
#include "cmt_analyze_dialog.h"
#include "cmt_tapeinfo_dialog.h"
#include "cmt_convert_dialog.h"
#include "cmt_check_window.h"


/* ------------------------------------------------------------------------- */
/*  Pseudo-překlad pro testy (texty o 40 % delší)                            */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
/**
 * @brief Testy: _L() nad cmt_view_test_tr() ("text##klíč").
 * @param msgid Klíč katalogu. @return "přeložený text##msgid" (ukazatel platí do konce běhu).
 */
static const char *cw_test_trl ( const char *msgid )
{
    static std::map<std::string, std::string> cache;
    std::string key = std::string ( cmt_view_test_stretch ( ) ? "1" : "0" ) + msgid;
    std::map<std::string, std::string>::iterator it = cache.find ( key );
    if ( it != cache.end ( ) ) return it->second.c_str ( );
    std::string s = std::string ( cmt_view_test_tr ( msgid ) ) + "##" + msgid;
    return cache.emplace ( key, s ).first->second.c_str ( );
}

/* _(), C_() a _L() s testovacím prodloužením (cmt_view.cpp); C_() i s kontextem */
#undef _
#define _(STRING) cmt_view_test_tr ( STRING )
#define _L(STRING) cw_test_trl ( STRING )
#undef C_
#define C_(CTX, STR) cmt_view_test_trc ( CTX "\004" STR, STR )
#endif


/* ------------------------------------------------------------------------- */
/*  Stav okna                                                                */
/* ------------------------------------------------------------------------- */

/** @brief Stabilní ID okna (SPEC: ###virtual_cmt). */
#define CW_WINDOW_ID "###virtual_cmt"

/** @brief Násobek základní velikosti písma pro velké počítadlo (SPEC: asi 2,2x, 62 px při 28 px). */
#define CW_COUNTER_FONT_FACTOR 2.2f

/** @brief Výška tlačítek transportu v násobcích výšky rámečku (D-G2-6). */
#define CW_TRANSPORT_H_FACTOR 1.6f

/** @brief Šířka tlačítek transportu v násobcích výšky rámečku (D-G2-6). */
#define CW_TRANSPORT_W_FACTOR 2.5f

/** @brief Výchozí šířka okna Compact v násobcích výšky rámečku (SPEC K01: asi 25 L). */
#define CW_COMPACT_W_FACTOR 25.0f
/** @brief Výchozí výška okna Compact v násobcích výšky rámečku (SPEC K01: asi 12 L). */
#define CW_COMPACT_H_FACTOR 12.0f
/** @brief Výchozí šířka okna Full v násobcích výšky rámečku (SPEC K01: asi 30 L). */
#define CW_FULL_W_FACTOR 30.0f
/**
 * @brief Výchozí výška okna Full v násobcích výšky rámečku.
 *
 * SPEC K01 uvádí asi 23 L (782 px při 28 px); výchozí
 * Full se ale musí vejít na displej 768 px při 100 %, proto 20 L (680 px,
 * platí-li minimum menší) - viz D-G2-24 v devdoc/cmt.md.
 */
#define CW_FULL_H_FACTOR 20.0f

/**
 * @brief Nejméně viditelných řádků tabulky (minimální výška okna Full).
 *
 * Minimum Full = deck + hlavička tabulky + tyto řádky + sbalená hlavička
 * vlastností + řádek zpráv; obsah vlastností dostane, co
 * zbude, v celých řádcích a posouvá se. Je-li okno menší (pracovní plocha),
 * tabulka smí mít i méně řádků (nejméně jeden, pak se skryje i hlavička
 * vlastností, když se nevejde).
 */
#define CW_TABLE_MIN_ROWS 3

/**
 * @brief Celé řádky tabulky ve výchozí velikosti Full se sbalenými vlastnostmi.
 *
 * Koncept cmt-full-v2 ukazuje 5 řádků; výchozí výška Full je proto aspoň
 * minimum + (CW_TABLE_DEFAULT_ROWS - CW_TABLE_MIN_ROWS) řádků
 * (cw_default_window_size()).
 */
#define CW_TABLE_DEFAULT_ROWS 5

/** @brief Sloupce tabulky bloků (index sloupce ImGui; pořadí ukládá ImGui do INI). */
typedef enum en_CW_COL {
    CW_COL_MARK = 0, /**< značka pozice pásky (trojúhelník accent), při záznamu červená tečka */
    CW_COL_NUM,      /**< "#": číslo bloku od 1 */
    CW_COL_NAME,     /**< "Name": jméno MZF nebo druh bloku */
    CW_COL_TYPE,     /**< "Type": typ MZF slovně */
    CW_COL_SIZE,     /**< "Size" (výchozí skrytý) */
    CW_COL_LOAD,     /**< "Load" (výchozí skrytý) */
    CW_COL_EXEC,     /**< "Exec" (výchozí skrytý) */
    CW_COL_SPEED,    /**< "Speed" (výchozí skrytý) */
    CW_COL_LENGTH,   /**< "Length": délka bloku */
    CW_COL_STATUS,   /**< "Status": stav bloku, u řídicího bloku popis */
    CW_COL_COUNT     /**< počet sloupců (sentinel) */
} en_CW_COL;

/** @brief Horní mez pamatované velikosti okna v INI (px). */
#define CW_CFG_SIZE_MAX 20000u

/**
 * @brief Stav okna mimo kopii jádra (rozvržení, pamatované velikosti, režim počítadla).
 *
 * @invariant layout < CMT_WINDOW_LAYOUT_COUNT.
 * @invariant cfg_w / cfg_h jsou velikosti okna v px pro každé rozvržení
 *            (0 = dosud žádná, platí výchozí velikost z výšky rámečku);
 *            cfgfile je čte a zapisuje přímo (UNSIGNED).
 */
typedef struct st_CW_STATE {
    en_CMT_WINDOW_LAYOUT layout; /**< aktuální rozvržení */
    unsigned cfg_layout;          /**< rozvržení pro INI (0 Compact, 1 Full) */
    unsigned cfg_w[CMT_WINDOW_LAYOUT_COUNT]; /**< pamatovaná šířka okna podle rozvržení (px, 0 = výchozí) */
    unsigned cfg_h[CMT_WINDOW_LAYOUT_COUNT]; /**< pamatovaná výška okna podle rozvržení (px, 0 = výchozí) */
    bool apply_size;              /**< v příštím snímku nastavit velikost okna podle rozvržení */
    bool cfg_applied;             /**< hodnoty z INI už převzaté do layout (první vykreslení) */
    bool drawn_once;              /**< okno se od otevření aspoň jednou nakreslilo (zaostření při otevření) */
    bool show_remaining;          /**< počítadlo ukazuje čas do konce pásky (Remaining), jinak od začátku (Elapsed) */
    bool cfg_registered;          /**< modul INI zaregistrovaný */
    int sel_id;                   /**< vybraný řádek tabulky (index bloku), -1 = bez výběru; stav okna, ne pozice pásky */
    unsigned sel_tape_serial;     /**< st_CMT_VIEW::tape_serial, ke kterému výběr patří (jiná páska = výběr se zruší) */
    unsigned props_open;          /**< sekce vlastností rozbalená (1) / sbalená (0, výchozí podle SPEC); INI props_open */
    bool col_enabled[CW_COL_COUNT]; /**< sloupec tabulky zapnutý (z minulého snímku, pro menu View > Columns) */
    int col_toggle;               /**< sloupec, který menu View > Columns přepne v příštím vykreslení tabulky (-1 = žádný) */
    ImGuiID table_id;             /**< ID tabulky bloků (po prvním vykreslení) */
    ImGuiID nav_last;             /**< položka řádku, která měla naposledy fokus navigace (výběr šipkami) */
    unsigned seen_tape_serial;    /**< st_CMT_VIEW::tape_serial minulého snímku (jiná páska = posun tabulky na aktuální blok) */
    bool scroll_current;          /**< v příštím vykreslení tabulky posunout na řádek aktuálního bloku (jen po výměně pásky) */
    bool rec_scrolled;            /**< tabulka už je posunutá na řádek Recording... probíhajícího záznamu */
    int drawn_frame;              /**< ImGui::GetFrameCount() posledního kreslení (s drawn_once) */
    unsigned menu_tape_serial;    /**< st_CMT_VIEW::tape_serial v době otevření kontextového menu řádku (jiná páska = menu se zavře) */
    unsigned pulses_tape_serial;  /**< st_CMT_VIEW::tape_serial v době otevření editoru vlastních pulzů bloku */
    bool reset_widths;            /**< View > Reset column widths: v příštím vykreslení tabulky nastavit šířky sloupců podle obsahu */
    ImGuiID menu_popup_id;        /**< ID otevřeného kontextového menu řádku (0 = žádné); osiřelé menu zavře cw_close_orphan_menu() */
    int menu_block;               /**< blok, ke kterému otevřené kontextové menu patří */
    int menu_frame;               /**< ImGui::GetFrameCount() snímku, ve kterém se menu naposledy kreslilo */
    bool col_show_all;            /**< menu View > Columns > Show all: příští vykreslení tabulky zapne všechny sloupce */
} st_CW_STATE;

/**
 * @brief Volba "Find files while playing": 1 = hrající surový blok se analyzuje na pozadí (cmt_analyze_dialog_auto()).
 *
 * INI [VIRTUAL_CMT_UI] auto_analyze (výchozí 1; prvek konfigurace ukazuje přímo sem), položka menu Tape.
 */
static unsigned s_cw_cfg_auto_analyze = 1u;

static st_CW_STATE s_cw = {
    CMT_WINDOW_LAYOUT_COMPACT, 0, {0, 0}, {0, 0}, true, false, false, false, false,
    -1, 0u, 0u, { true, true, true, true, false, false, false, false, true, true }, -1, 0, 0, 0u, false, false, 0, 0u, 0u, false,
    0, -1, -1, false
};


/**
 * @brief Stav okna pro úpravy pásky: vícenásobný výběr, potvrzení mazání, tažení, přetažení souboru z OS.
 *
 * Patří UI vláknu. Geometrie tabulky (table_*, row_*) platí jen ze snímku
 * table_frame (cw_draw_table()).
 *
 * @invariant sel obsahuje indexy bloků pásky s čítačem s_cw.sel_tape_serial;
 *            s_cw.sel_id >= 0 <=> sel.Size > 0 a sel obsahuje s_cw.sel_id.
 * @invariant del_open || del_pending => del_ids neprázdné, vzestupně.
 * @invariant os_ready => os_files neprázdné.
 */
typedef struct st_CW_EDIT {
    ImGuiSelectionBasicStorage sel; /**< vybrané řádky (index bloku = ImGuiID, výchozí adaptér ImGui) */
    bool range_reset;               /**< výběr nastavil program (obnova po úpravě): kotvu Shift+klik ImGui zahodit */
    int focus_row = -1;             /**< řádek, na který po obnově výběru přejde fokus navigace a posun (-1 = žádný) */
    bool table_focus;               /**< fokus navigace je v tabulce (BeginMultiSelect IsFocused) v posledním snímku */
    ImGuiID keys_owner;             /**< vlastník kláves Up / Down při držení Alt (Alt+Up / Alt+Down místo navigace) */
    bool del_pending;               /**< modál Delete blocks se má otevřít (OpenPopup v kontextu okna) */
    bool del_open;                  /**< modál Delete blocks je otevřený */
    std::vector<int> del_ids;       /**< bloky k potvrzení smazání (vzestupně) */
    unsigned del_tape_serial;       /**< čítač výměn pásky v době žádosti o smazání */
    int del_unsaved;                /**< počet neuložených bloků mezi del_ids (text modálu) */
    bool drag_cancel;               /**< tažení řádku zrušené Escapem nebo změnou pásky (do puštění tlačítka myši) */
    int drag_from = -1;             /**< tažený blok (-1 = netáhne se); clipper ho vždy kreslí, jinak by tažení zaniklo */
    unsigned drag_serial;           /**< čítač výměn pásky ze začátku tažení (nese ho payload; jiný = tažení se zruší) */
    float scroll_acc;               /**< automatický posun při tažení: nastřádané řádky (posouvá se po celých řádcích) */
    uint32_t move_seq;              /**< zásah přesunu z Alt+Up / Alt+Down, na jehož výsledek se čeká (0 = žádný) */
    int move_queued;                /**< čekající další přesun (-1 nahoru, +1 dolů, 0 = žádný; nejvýš jeden) */
    int move_idle_frame;            /**< snímek, kdy byla fronta zásahů prázdná a move_seq nevyřízený (-1 = ne; ztracený výsledek) */
    int table_frame = -1;           /**< ImGui::GetFrameCount() snímku, ve kterém se tabulka kreslila (-1 = nikdy) */
    ImRect table_area;              /**< celá plocha tabulky (i plocha prázdné pásky) pro přetažení souboru */
    ImRect body;                    /**< tělo tabulky pod hlavičkou (ořez řádků a čáry místa vložení) */
    bool rows_any;                  /**< v tabulce se kreslil aspoň jeden řádek bloku */
    int row_ref_id;                 /**< první kreslený řádek (index bloku) */
    float row_ref_y0;               /**< horní okraj řádku row_ref_id (px, souřadnice obrazovky) */
    int row_min_id;                 /**< nejmenší kreslený řádek */
    int row_max_id;                 /**< největší kreslený řádek */
    float row_h;                    /**< výška řádku tabulky (cw_row_h()) */
    int row_count;                  /**< počet bloků pásky při kreslení */
    bool os_active;                 /**< probíhá přetažení souboru z OS (SDL_EVENT_DROP_BEGIN .. _COMPLETE) */
    bool os_pos_valid;              /**< os_pos je známá poloha během přetažení */
    ImVec2 os_pos;                  /**< poslední poloha přetažení (souřadnice ImGui) */
    double os_pos_time;             /**< ImGui::GetTime() poslední polohy (bez další polohy se čára po 1 s skryje) */
    std::vector<std::string> os_files; /**< soubory přetažení v pořadí od SDL */
    ImVec2 os_file_pos;             /**< poloha puštění (poslední SDL_EVENT_DROP_FILE) */
    bool os_ready;                  /**< přetažení skončilo (SDL_EVENT_DROP_COMPLETE): soubory vyhodnotí kreslení okna */
    int os_ready_frame;             /**< ImGui::GetFrameCount() při os_ready (starší než 2 snímky se zahodí) */
} st_CW_EDIT;

/** @brief Stav úprav pásky okna (UI vlákno). */
static st_CW_EDIT s_cw4;

/** @brief Typ dat tažení řádku tabulky (ImGui payload st_CW_DND_ROW). */
#define CW_DND_BLOCK "CMT_BLOCK"

/**
 * @brief Data tažení řádku (ImGui payload CW_DND_BLOCK).
 * @invariant tape_serial je čítač výměn pásky ze začátku tažení; puštění nad jinou páskou nic nepřesune.
 */
typedef struct st_CW_DND_ROW {
    int from;             /**< tažený blok (index na pásce tape_serial) */
    unsigned tape_serial; /**< čítač výměn pásky v okamžiku začátku tažení */
} st_CW_DND_ROW;

/** @brief Automatický posun tabulky při tažení: nejvyšší rychlost v řádcích za sekundu (kurzor na okraji zóny). */
#define CW_AUTOSCROLL_ROWS_PER_S 12.0f

/** @brief Stabilní ID modálu potvrzení mazání (titulek se překládá, ID ne). */
#define CW_DELETE_POPUP_ID "###cw_delete_blocks"


/* ------------------------------------------------------------------------- */
/*  Testovací záznam prvků                                                   */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cw_items; /**< prvky rozpracovaného snímku */
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cw_items_done; /**< prvky posledního dokončeného snímku */
static ImVec2 s_cw_inner_min, s_cw_inner_max; /**< vnitřní obdélník okna z posledního snímku */
static ImVec2 s_cw_min_size;                  /**< minimální velikost okna z posledního snímku */
static std::string s_cw_title;                /**< titulek okna z posledního snímku */
static bool s_cw_table_scroll_x = false;      /**< tabulka bloků měla v posledním snímku vodorovný posuvník */
static bool s_cw_props_scroll_y = false;      /**< obsah vlastností měl v posledním snímku svislý posuvník */
#endif

/** @brief Kotva prvku pro test posunu decku mezi rozvrženími. */
typedef enum en_CW_ANCHOR {
    CW_ANCHOR_LEFT = 0, /**< prvek je ukotvený k levému okraji decku */
    CW_ANCHOR_RIGHT,    /**< prvek je ukotvený k pravému okraji decku */
} en_CW_ANCHOR;

/** @brief Druh záznamu prvku. */
typedef enum en_CW_KIND {
    CW_KIND_DECK = 0,   /**< prvek decku */
    CW_KIND_CONTAINER,  /**< obal prvků decku (vynechává se z kontroly překryvu) */
    CW_KIND_OTHER,      /**< prvek mimo deck (řádek zpráv, hlavička tabulky, vlastnosti) */
    CW_KIND_OTHER_CONTAINER, /**< obal mimo deck (tabulka, sekce vlastností, hlavička vlastností) */
    CW_KIND_SCROLL,     /**< prvek ve svisle posouvané oblasti (řádek tabulky, obsah vlastností) */
    CW_KIND_SCROLL_CONTAINER, /**< obal ve svisle posouvané oblasti (Selectable řádku přes všechny sloupce) */
    CW_KIND_POPUP,      /**< položka kontextového menu (vlastní okno popupu) */
} en_CW_KIND;

/**
 * @brief Zaznamená obdélník prvku pro testy (v aplikaci nic).
 *
 * @param name Jméno prvku. @param text Zobrazený text (nebo druh ikony).
 * @param min Levý horní roh. @param max Pravý dolní roh.
 * @param kind Druh. @param anchor Kotva. @param disabled Prvek je zakázaný.
 * @pre Uvnitř okna (Begin()), ořez okna = GetCurrentWindow()->ClipRect.
 */
static void cw_record ( const char *name, const char *text, ImVec2 min, ImVec2 max, en_CW_KIND kind, en_CW_ANCHOR anchor,
                        bool disabled )
{
#ifdef MZTEST_HEADLESS
    st_CMT_WINDOW_TEST_ITEM it;
    it.name = name;
    it.text = text ? text : "";
    it.disabled = disabled;
    it.min = min;
    it.max = max;
    ImGuiWindow *w = ImGui::GetCurrentWindow ( );
    it.clip_min = w->ClipRect.Min;
    it.clip_max = w->ClipRect.Max;
    it.container = ( kind == CW_KIND_CONTAINER || kind == CW_KIND_OTHER_CONTAINER || kind == CW_KIND_SCROLL_CONTAINER );
    it.deck = ( kind == CW_KIND_DECK || kind == CW_KIND_CONTAINER );
    it.scroll_y = ( kind == CW_KIND_SCROLL || kind == CW_KIND_SCROLL_CONTAINER );
    it.popup = ( kind == CW_KIND_POPUP );
    it.right = ( anchor == CW_ANCHOR_RIGHT );
    s_cw_items.push_back ( it );
#else
    ( void ) name;
    ( void ) text;
    ( void ) min;
    ( void ) max;
    ( void ) kind;
    ( void ) anchor;
    ( void ) disabled;
#endif
}

/**
 * @brief Doplní k naposledy zaznamenanému prvku důvod zákazu (jen testy, v aplikaci nic).
 * @param reason Důvod (přeložený), NULL = žádný.
 */
static void cw_record_reason ( const char *reason )
{
#ifdef MZTEST_HEADLESS
    if ( !s_cw_items.empty ( ) ) s_cw_items.back ( ).reason = reason ? reason : "";
#else
    ( void ) reason;
#endif
}

/**
 * @brief Zaznamená poslední položku ImGui (GetItemRectMin / Max) pro testy.
 * @param name Jméno. @param text Text. @param anchor Kotva. @param disabled Zakázaná.
 */
static void cw_record_item ( const char *name, const char *text, en_CW_ANCHOR anchor, bool disabled )
{
    cw_record ( name, text, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_DECK, anchor, disabled );
}

/**
 * @brief Zaznamená prvek kreslený mimo aktuální okno (DrawList jiného okna) s daným ořezem (jen testy).
 *
 * Čára místa vložení se kreslí do DrawListu vnitřního okna tabulky až po
 * EndTable(), aktuální okno je tehdy okno kazety; záznam proto nese ořez,
 * pod kterým se prvek skutečně kreslil.
 *
 * @param name Jméno prvku. @param min Levý horní roh. @param max Pravý dolní roh.
 * @param clip Ořez kreslení. @param kind Druh záznamu.
 */
static void cw_record_clip ( const char *name, ImVec2 min, ImVec2 max, const ImRect &clip, en_CW_KIND kind )
{
#ifdef MZTEST_HEADLESS
    cw_record ( name, NULL, min, max, kind, CW_ANCHOR_LEFT, false );
    s_cw_items.back ( ).clip_min = clip.Min;
    s_cw_items.back ( ).clip_max = clip.Max;
#else
    ( void ) name;
    ( void ) min;
    ( void ) max;
    ( void ) clip;
    ( void ) kind;
#endif
}


/**
 * @brief Zaznamená položku menu baru (Tape, View) hned po BeginMenu() (jen testy).
 *
 * Je-li menu otevřené, aktuální okno je už popup menu a jeho ořez by
 * položku menu baru (ležící v okně kazety) hlásil jako vyčnívající; ořez
 * záznamu je proto obdélník okna, kterému položka patří.
 *
 * @param name Jméno prvku. @param open Návrat BeginMenu() (true = aktuální okno je popup menu).
 * @pre Hned po BeginMenu() (poslední položka = položka menu baru).
 */
static void cw_record_menu_bar_item ( const char *name, bool open )
{
#ifdef MZTEST_HEADLESS
    ImGuiWindow *w = ImGui::GetCurrentWindow ( );
    ImGuiWindow *host = ( open && w->ParentWindow ) ? w->ParentWindow : w;
    cw_record_clip ( name, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), host->Rect ( ), CW_KIND_POPUP );
#else
    ( void ) name;
    ( void ) open;
#endif
}


/* ------------------------------------------------------------------------- */
/*  Pomocné funkce: texty, čísla, čas                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Šířka textu bez skrývání "##" (jména pásek a bloků mohou "##" obsahovat).
 * @param s Text (UTF-8). @return Šířka v px aktuálním písmem.
 */
static float cw_text_w ( const std::string &s )
{
    return ImGui::CalcTextSize ( s.c_str ( ), s.c_str ( ) + s.size ( ), false ).x;
}

/**
 * @brief Začátky znaků UTF-8 v řetězci (a konec řetězce jako poslední prvek).
 * @param s Text. @return Indexy bajtů začátků znaků + s.size().
 */
static std::vector<size_t> cw_utf8_starts ( const std::string &s )
{
    std::vector<size_t> b;
    for ( size_t i = 0; i < s.size ( ); i++ )
        if ( ( ( unsigned char ) s[i] & 0xC0 ) != 0x80 ) b.push_back ( i );
    b.push_back ( s.size ( ) );
    return b;
}

/**
 * @brief Zkrátí text na šířku elipsou uprostřed ("games_...backup.tmz"), po celých znacích UTF-8.
 * @param s Text. @param max_w Šířka v px. @param[out] elided true = text se zkrátil.
 * @return Text, který se do max_w vejde (nejvýš "...").
 */
static std::string cw_ellipsize_middle ( const std::string &s, float max_w, bool *elided )
{
    *elided = false;
    if ( cw_text_w ( s ) <= max_w ) return s;
    *elided = true;
    std::vector<size_t> b = cw_utf8_starts ( s );
    int n = ( int ) b.size ( ) - 1;
    std::string best = "...";
    int lo = 0, hi = n - 1;
    while ( lo <= hi ) {
        int k = ( lo + hi ) / 2;
        int head = ( k + 1 ) / 2;
        int tail = k / 2;
        std::string cand = s.substr ( 0, b[( size_t ) head] ) + "..." + s.substr ( b[( size_t ) ( n - tail )] );
        if ( cw_text_w ( cand ) <= max_w ) {
            best = cand;
            lo = k + 1;
        } else {
            hi = k - 1;
        }
    }
    return best;
}

/**
 * @brief Zkrátí text na šířku elipsou na konci, po celých znacích UTF-8.
 * @param s Text. @param max_w Šířka v px. @param[out] elided true = text se zkrátil.
 * @return Text, který se do max_w vejde (nejvýš "...").
 */
static std::string cw_ellipsize_end ( const std::string &s, float max_w, bool *elided )
{
    *elided = false;
    if ( cw_text_w ( s ) <= max_w ) return s;
    *elided = true;
    std::vector<size_t> b = cw_utf8_starts ( s );
    int n = ( int ) b.size ( ) - 1;
    std::string best = "...";
    int lo = 0, hi = n - 1;
    while ( lo <= hi ) {
        int k = ( lo + hi ) / 2;
        std::string cand = s.substr ( 0, b[( size_t ) k] ) + "...";
        if ( cw_text_w ( cand ) <= max_w ) {
            best = cand;
            lo = k + 1;
        } else {
            hi = k - 1;
        }
    }
    return best;
}

/**
 * @brief Zkrátí zprávu na šířku tak, aby zůstalo vidět jméno souboru z cesty.
 *
 * Obsahuje-li zpráva cestu (slovo s '/' nebo '\\'), zkrátí se elipsou
 * uprostřed jen adresářová část cesty: text před cestou, jméno souboru
 * a text za ním zůstanou ("Tape saved: C:/Us...name/games.tmz (...)").
 * Nevejde-li se ani to, zprávu se zkrácenou cestou zkrátí elipsou na
 * konci. Zpráva bez cesty: elipsa na konci (cw_ellipsize_end()).
 *
 * @param s Zpráva (UTF-8). @param max_w Šířka v px. @param[out] elided true = zpráva se zkrátila.
 * @return Text, který se do max_w vejde.
 */
static std::string cw_ellipsize_message ( const std::string &s, float max_w, bool *elided )
{
    *elided = false;
    if ( cw_text_w ( s ) <= max_w ) return s;
    size_t first_sep = s.find_first_of ( "/\\" );
    if ( first_sep == std::string::npos ) return cw_ellipsize_end ( s, max_w, elided );
    size_t last_sep = s.find_last_of ( "/\\" );
    /* začátek cesty = začátek slova s prvním oddělovačem ("C:/...") */
    size_t start = s.rfind ( ' ', first_sep );
    start = ( start == std::string::npos ) ? 0 : start + 1;
    std::string head = s.substr ( 0, start );
    std::string dir = s.substr ( start, last_sep - start );
    std::string rest = s.substr ( last_sep );
    float dir_w = max_w - cw_text_w ( head ) - cw_text_w ( rest );
    *elided = true;
    if ( dir_w >= cw_text_w ( "..." ) ) {
        bool e = false;
        return head + cw_ellipsize_middle ( dir, dir_w, &e ) + rest;
    }
    bool e = false;
    return cw_ellipsize_end ( head + "..." + rest, max_w, &e );
}

/**
 * @brief Číslo s tisíci oddělenými mezerou (SPEC K27: "12 345", nezávisle na jazyce).
 * @param value Číslo. @return Text.
 */
static std::string cw_thousands ( uint32_t value )
{
    char raw[16];
    snprintf ( raw, sizeof ( raw ), "%u", ( unsigned ) value );
    std::string out;
    size_t len = strlen ( raw );
    for ( size_t i = 0; i < len; i++ ) {
        if ( i > 0 && ( len - i ) % 3 == 0 ) out += ' ';
        out += raw[i];
    }
    return out;
}

/**
 * @brief Čas pro počítadlo ve tvaru MM:SS.d (minuty mohou mít víc číslic).
 * @param seconds Čas v s (záporný = 0). @return Text.
 */
static std::string cw_time_text ( double seconds )
{
    if ( !( seconds > 0.0 ) ) seconds = 0.0;
    long long tenths = ( long long ) floor ( seconds * 10.0 + 0.5 );
    char buf[32];
    snprintf ( buf, sizeof ( buf ), "%02lld:%02lld.%lld", tenths / 600, ( tenths / 10 ) % 60, tenths % 10 );
    return std::string ( buf );
}

/** @brief Text počítadla, když čas není k dispozici (bez pásky, Direct, neznámý). */
#define CW_TIME_NONE "--:--.-"


/* ------------------------------------------------------------------------- */
/*  Stav decku                                                               */
/* ------------------------------------------------------------------------- */

/** @brief Stav decku pro stavový řádek (SPEC: tabulka Stavy decku a oddíl K06). */
typedef enum en_CW_STATUS {
    CW_STATUS_NO_TAPE = 0, /**< "No tape" (text-dim) */
    CW_STATUS_STOPPED,     /**< "Stopped" (text) */
    CW_STATUS_PLAYING,     /**< "Playing" (play) */
    CW_STATUS_WAITING,     /**< "Waiting for motor" (warn): PLAY / RECORD, páska stojí, protože stojí motor (bez pauzy uživatele) */
    CW_STATUS_PAUSED,      /**< "Paused" (warn): pauza uživatele (tlačítko, MCP), motor může běžet i stát */
    CW_STATUS_RECORDING,   /**< "Recording" (rec) */
    CW_STATUS_DIRECT,      /**< "Direct load" (accent): režim Direct mimo nahrávání, transport se nepoužívá */
} en_CW_STATUS;

/**
 * @brief Stav decku z kopie.
 *
 * Jádro drží dva nezávislé zdroje zastavení pásky (st_CMT::user_paused
 * a motor, cmt_pause() / cmt_motor_toggle()): páska stojí s pauzou
 * uživatele -> "Paused" (tlačítko ukáže Play, akce pauzu zruší), bez ní
 * stojí kvůli motoru -> "Waiting for motor" (tlačítko ukáže Pause). Viz
 * D-G2-3 v devdoc/cmt.md a tabulku stav x motor návrhu okna.
 *
 * Nahrávání v režimu Direct ukazuje stav decku (Recording, Waiting for
 * motor, Paused) jako mimo Direct: CMT hack patchuje jen čtení a při
 * RECORD řídí deck motor programu (CMTHACK_TEST_FAKES_MOTOR).
 *
 * @param v Kopie. @return Stav.
 */
static en_CW_STATUS cw_status ( const st_CMT_VIEW *v )
{
    if ( v->mode == CMT_VIEW_MODE_DIRECT && v->state != CMT_VIEW_STATE_RECORD ) return CW_STATUS_DIRECT;
    if ( !v->has_tape ) return CW_STATUS_NO_TAPE;
    if ( v->state == CMT_VIEW_STATE_STOP ) return CW_STATUS_STOPPED;
    if ( v->paused ) return v->user_paused ? CW_STATUS_PAUSED : CW_STATUS_WAITING;
    if ( v->state == CMT_VIEW_STATE_RECORD ) return CW_STATUS_RECORDING;
    return CW_STATUS_PLAYING;
}

/**
 * @brief Text a barva stavu decku.
 * @param st Stav. @param[out] color Barva (token). @param[out] dot true = před textem tečka v barvě stavu.
 * @return Přeložený text.
 */
static const char *cw_status_text ( en_CW_STATUS st, ImU32 *color, bool *dot )
{
    *dot = true;
    switch ( st ) {
        case CW_STATUS_NO_TAPE: *color = CMT_TOK_TEXT_DIM; *dot = false; return _ ( "No tape" );
        case CW_STATUS_STOPPED: *color = CMT_TOK_TEXT; *dot = false; return _ ( "Stopped" );
        case CW_STATUS_PLAYING: *color = CMT_TOK_PLAY; return _ ( "Playing" );
        case CW_STATUS_WAITING: *color = CMT_TOK_WARN; return _ ( "Waiting for motor" );
        case CW_STATUS_PAUSED: *color = CMT_TOK_WARN; return _ ( "Paused" );
        case CW_STATUS_RECORDING: *color = CMT_TOK_REC; return _ ( "Recording" );
        case CW_STATUS_DIRECT:
        default: *color = CMT_TOK_ACCENT; return _ ( "Direct load" );
    }
}

/**
 * @brief Aktuální (otevřený) blok kopie.
 * @param v Kopie.
 * @return Blok v v->blocks (platí do dalšího plnění kopie), nebo NULL bez
 *         pásky, bez otevřeného bloku a při RECORD (current_block ==
 *         block_count, blok teprve vzniká).
 */
static const st_CMT_VIEW_BLOCK *cw_current_block ( const st_CMT_VIEW *v )
{
    if ( !v->has_tape || v->current_block < 0 || v->current_block >= ( int ) v->blocks.size ( ) ) return NULL;
    return &v->blocks[( size_t ) v->current_block];
}

/**
 * @brief Nejbližší přehratelný blok od indexu daným směrem (jako cmttmz_block_open_first() v jádře).
 *
 * Přehratelný = st_CMT_VIEW_BLOCK::playable (ne řídicí blok, ne blok, který
 * player neumí). Požadavky Previous / Next jádra (cmt_tape_step_block())
 * přeskakují nepřehratelné bloky stejně, Rewind míří na první z nich.
 *
 * @param v Kopie. @param from První zkoumaný index (může být mimo rozsah).
 * @param dir +1 dopředu, -1 dozadu.
 * @return Index bloku, nebo -1, když tím směrem žádný není.
 */
static int cw_playable_from ( const st_CMT_VIEW *v, int from, int dir )
{
    for ( int i = from; i >= 0 && i < ( int ) v->blocks.size ( ); i += dir )
        if ( v->blocks[( size_t ) i].playable ) return i;
    return -1;
}

/**
 * @brief Jméno bloku k zobrazení (deck, tabulka, vlastnosti).
 *
 * SPEC (deck, bod 2): blok bez jména ukazuje svůj druh. Kopie dává
 * jméno druhu sama u surového záznamu ("Raw recording"), WAV, dat ZX
 * a bloků TZX ("TZX 0x11 ..."); prázdné zůstává jméno MZF a hlavičky
 * ZX TAP a řídicí blok bez jména. "(no name)" jen u MZF s prázdným
 * jménem (D-G2-12), hlavička ZX bez jména "ZX header", jinak typ bloku.
 * Řídicí blok vždy "Control block" (jeho popis je ve sloupci Status, typ
 * v knihovně je anglicky). Typ řídicího bloku z knihovny ukazují
 * vlastnosti (Type).
 *
 * @param b Blok. @param[out] dim true = náhradní text (ukáže se ztlumeně).
 * @return Text (UTF-8).
 */
static std::string cw_block_display_name ( const st_CMT_VIEW_BLOCK *b, bool *dim )
{
    *dim = false;
    /* řídicí blok: jméno typu z knihovny (anglicky) by opakovalo popis ze
     * sloupce Status; jen přeložený druh */
    if ( b->kind == CMT_VIEW_KIND_CONTROL ) {
        *dim = true;
        return std::string ( _ ( "Control block" ) );
    }
    if ( !b->name.empty ( ) ) return b->name;
    *dim = true;
    switch ( b->kind ) {
        case CMT_VIEW_KIND_MZF: return std::string ( _ ( "(no name)" ) );
        case CMT_VIEW_KIND_TAP_HEADER: return std::string ( _ ( "ZX header" ) );
        case CMT_VIEW_KIND_CONTROL: return std::string ( _ ( "Control block" ) );
        default: break;
    }
    if ( !b->type_text.empty ( ) && b->type_text != "-" ) return b->type_text;
    return std::string ( _ ( "(no name)" ) );
}

/**
 * @brief Obsah počítadla: text, popisek a jestli jde přepnout na Remaining.
 *
 * - bez pásky a v Direct "--:--.-" ztlumeně (SPEC v2, režim x prvek);
 * - RECORD: čas záznamu, popisek "Recording time" (žádná vymyšlená délka);
 * - jinak Elapsed (pozice od začátku pásky) nebo Remaining (do konce pásky).
 *   Pozice od začátku pásky potřebuje délky všech předchozích bloků, čas do
 *   konce délky všech bloků. Neznámá-li pozice od začátku pásky, ukáže se
 *   pozice v bloku s popiskem "Block time"; Remaining jde zapnout jen se
 *   známou délkou pásky (jinak je plocha počítadla zakázaná s tooltipem).
 *   Viz D-G2-4 v devdoc/cmt.md.
 */
typedef struct st_CW_COUNTER {
    std::string text;        /**< čas MM:SS.d nebo "--:--.-" */
    const char *caption;     /**< popisek pod číslem (přeložený) */
    bool dim;                /**< číslo ztlumeně (čas není k dispozici) */
    bool can_toggle;         /**< klik přepne Elapsed / Remaining */
    const char *reason;      /**< proč přepnout nejde (přeložený), NULL = jde */
} st_CW_COUNTER;

/**
 * @brief Vyplní obsah počítadla z kopie.
 * @param v Kopie. @param remaining Uživatel chce čas do konce pásky. @param[out] c Obsah.
 */
static void cw_counter ( const st_CMT_VIEW *v, bool remaining, st_CW_COUNTER *c )
{
    c->dim = false;
    c->can_toggle = false;
    c->reason = NULL;
    /* nahrávání ukazuje čas záznamu i v Direct (motor programu deck řídí) */
    if ( ( v->mode == CMT_VIEW_MODE_DIRECT && v->state != CMT_VIEW_STATE_RECORD ) || !v->has_tape ) {
        c->text = CW_TIME_NONE;
        c->caption = _ ( "Elapsed" );
        c->dim = true;
        c->reason = ( v->mode == CMT_VIEW_MODE_DIRECT ) ? _ ( "Not used in Direct mode" ) : _ ( "No tape inserted." );
        return;
    }
    if ( v->state == CMT_VIEW_STATE_RECORD ) {
        c->text = cw_time_text ( v->rec_time_s );
        c->caption = _ ( "Recording time" );
        c->reason = _ ( "Not available while recording." );
        return;
    }
    bool elapsed_known = v->tape_elapsed_s >= 0.0;
    bool remaining_known = elapsed_known && v->tape_total_s >= 0.0;
    c->can_toggle = remaining_known;
    if ( !remaining_known ) c->reason = _ ( "The remaining time is shown once the lengths of all blocks are known. They are computed while the tape is stopped." );
    if ( remaining && remaining_known ) {
        double r = v->tape_total_s - v->tape_elapsed_s;
        c->text = cw_time_text ( r );
        c->caption = _ ( "Remaining" );
    } else if ( elapsed_known ) {
        c->text = cw_time_text ( v->tape_elapsed_s );
        c->caption = _ ( "Elapsed" );
    } else if ( v->block_pos_s >= 0.0 ) {
        c->text = cw_time_text ( v->block_pos_s );
        c->caption = _ ( "Block time" );
    } else {
        c->text = CW_TIME_NONE;
        c->caption = _ ( "Elapsed" );
        c->dim = true;
    }
}


/* ------------------------------------------------------------------------- */
/*  Rozměry decku                                                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Rozměry decku a okna odvozené z písma a textů jednoho snímku.
 *
 * Počítá se před Begin() (minimální velikost okna) a stejné hodnoty
 * používá kreslení, takže poloha prvků nezávisí na předchozím snímku.
 * Texty, které na kopii nezávisí, se měří jen při změně písma, měřítka
 * nebo jazyka (st_CW_MEASURE_CACHE); každý snímek se měří jen řádek
 * "Block n / m" a text výchozí rychlosti.
 *
 * Zalomení (malá obrazovka): když se okno nevejde na pracovní
 * plochu, smí být užší než jednořádkový deck. Pak se stavový řádek
 * displeje (motor pod stav) a řádek 4 (Loading pod Signal speed) zalomí
 * na dva řádky, viz cw_metrics_wrap().
 *
 * @invariant Všechny délky jsou v px a > 0 (kromě volitelných textů).
 * @invariant row2_h, row4_h a deck_h odpovídají wrap2 a wrap4 (cw_metrics_wrap()).
 */
typedef struct st_CW_METRICS {
    float F;            /**< výška rámečku (GetFrameHeight()) */
    float Lh;           /**< výška řádku textu (GetTextLineHeight()) */
    float u;            /**< mřížka 4 px škálovaná písmem (GetFontSize() / 7) */
    float pad;          /**< vnitřní okraj panelu decku a odstup od dělicích čar (2u) */
    float gap;          /**< mezera mezi prvky v řádku (2u) */
    ImFont *mono;       /**< monospace písmo aplikace (NULL = výchozí) */
    float big_size;     /**< velikost písma počítadla bez měřítka (PushFont) */
    float big_h;        /**< výška řádku velkého počítadla (px) */
    float counter_w;    /**< šířka sloupce počítadla */
    float status_w;     /**< šířka nejdelšího stavu decku (tečka, mezera, text) */
    float motor_w;      /**< šířka indikátoru motoru (tečka, mezera, delší z Motor on / off) */
    float right_min_w;  /**< nejmenší šířka pravého sloupce displeje (stav a motor na jednom řádku) */
    float right_wrap_w; /**< nejmenší šířka pravého sloupce displeje se zalomeným motorem */
    float row1_h;       /**< výška řádku pásky */
    float row2_h;       /**< výška displeje (podle wrap2) */
    float btn_w;        /**< šířka tlačítka transportu */
    float btn_h;        /**< výška tlačítka transportu */
    float row4_h;       /**< výška řádku rychlosti a režimu (podle wrap4) */
    float deck_h;       /**< výška celého panelu decku (podle wrap2, wrap4) */
    float speed_label_w;  /**< šířka popisku "Signal speed" */
    float speed_value_w;  /**< šířka místa pro výběr rychlosti nebo text pevné rychlosti */
    float combo_w;        /**< šířka výběru rychlosti */
    float loading_label_w; /**< šířka popisku "Loading" */
    float seg_w;        /**< šířka jednoho segmentu Tape / Boost / Direct */
    float row13_min_w;  /**< nejmenší šířka řádků 1 a 3 (nezalamují se) */
    float row2_min_w;   /**< nejmenší šířka řádku 2 se stavem a motorem na jednom řádku */
    float row2_wrap_w;  /**< nejmenší šířka řádku 2 se zalomeným motorem */
    float row4_min_w;   /**< nejmenší šířka řádku 4 na jednom řádku */
    float row4_wrap_w;  /**< nejmenší šířka řádku 4 zalomeného (Loading pod Signal speed) */
    float deck_min_w;   /**< nejmenší šířka obsahu decku bez zalomení (bez okrajů panelu) */
    float deck_wrap_w;  /**< nejmenší šířka obsahu decku se zalomením obou řádků */
    bool wrap2;         /**< stavový řádek displeje zalomený (motor pod stavem) */
    bool wrap4;         /**< řádek 4 zalomený (Loading a přepínač pod Signal speed) */
    float table_min_w;  /**< nejmenší šířka tabulky bloků (Full; zapnuté sloupce bez oříznutí) */
    float table_min_h;  /**< nejmenší výška tabulky (hlavička + CW_TABLE_MIN_ROWS řádků) */
    float props_min_h;  /**< nejmenší výška sekce vlastností (jen hlavička, obsah se posouvá) */
    float bar_h;        /**< výška ukazatele průběhu bloku */
    float dot_w;        /**< šířka místa pro tečku stavu */
} st_CW_METRICS;

/**
 * @brief Šířka tlačítka s textem: CalcTextSize bez "##", vnitřní okraje rámečku a 2 x 8 px (škálováno písmem).
 *
 * Výchozí okraj rámečku ImGui (4 px) dělá tlačítka decku stísněná proti
 * konceptu v2; každá strana dostane navíc 2 jednotky mřížky.
 *
 * @param label Popisek (může obsahovat "##"). @return Šířka v px.
 */
static float cw_button_w ( const char *label )
{
    return ImGui::CalcTextSize ( label, NULL, true ).x + ImGui::GetStyle ( ).FramePadding.x * 2.0f + 4.0f * ImGui::GetFontSize ( ) / 7.0f;
}

/**
 * @brief Vrátí základní velikost písma bez měřítka (pro PushFont(font, velikost)).
 * @return GImGui->FontSizeBase (PushFont ji násobí FontScaleMain a FontScaleDpi).
 */
static float cw_font_base ( void )
{
    return ImGui::GetCurrentContext ( )->FontSizeBase;
}

/**
 * @brief Seznam pevných rychlostí MZ pro výběr Signal speed (kopie jednou za běh).
 *
 * ImGuiCmt::getMzSpeedList() vrací kopii vektoru; okno ho potřebuje každý
 * snímek (měření, výběr), proto si ho drží. Seznam se v ImGuiCmt sestaví
 * jednou za běh, takže kopie se nezmění.
 *
 * @return Seznam (platí do konce běhu).
 */
static const std::vector<ImGuiCmtSpeedList_t> &cw_speed_list ( void )
{
    static std::vector<ImGuiCmtSpeedList_t> list;
    if ( list.empty ( ) ) list = ImGuiCmt::getMzSpeedList ( );
    return list;
}

/**
 * @brief Mezipaměť měření textů decku, které nezávisí na kopii stavu.
 *
 * @invariant valid => m a pomocné šířky odpovídají klíči (velikost písma,
 *            základní velikost, monospace písmo, okraj rámečku, zvolený
 *            jazyk lang_id a proměnná LANGUAGE lang_env, testovací
 *            prodloužení textů).
 */
typedef struct st_CW_MEASURE_CACHE {
    bool valid;          /**< obsah platí */
    float font_size;     /**< klíč: GetFontSize() */
    float font_base;     /**< klíč: FontSizeBase */
    float frame_pad;     /**< klíč: FramePadding.x (měřítko stylu) */
    ImFont *mono;        /**< klíč: monospace písmo */
    int lang_id;         /**< klíč: zvolený jazyk (g_i18n_lang_settings.language) */
    std::string lang_env; /**< klíč: proměnná LANGUAGE, podle které gettext vybírá katalog */
    bool stretch;        /**< klíč: testovací prodloužení textů o 40 % */
    st_CW_METRICS m;     /**< změřené hodnoty (bez částí závislých na kopii) */
    float row1_min;      /**< nejmenší šířka řádku 1 */
    float row3_min;      /**< nejmenší šířka řádku 3 */
    float right_static_w; /**< nejdelší pevný text pravého sloupce displeje (bez "Block n / m") */
    float combo_static_w; /**< nejdelší popisek výběru rychlosti (bez textu výchozí rychlosti) */
    float fixed_speed_w; /**< nejdelší text pevné rychlosti ("Not used in Direct mode", ...) */
} st_CW_MEASURE_CACHE;

/** @brief Mezipaměť měření (jediné okno, UI vlákno). */
static st_CW_MEASURE_CACHE s_cw_mc;

/**
 * @brief Změří texty decku nezávislé na kopii (jen při změně klíče mezipaměti).
 * @pre UI vlákno, uvnitř snímku ImGui (aktivní písmo).
 * @post s_cw_mc.valid a klíč odpovídá aktuálnímu písmu, stylu a jazyku.
 */
static void cw_measure_static ( void )
{
    const ImGuiStyle &st = ImGui::GetStyle ( );
    ImFont *mono = myimgui_get_monospace_font ( );
    /* jazyk podle kódu, ne podle přeloženého textu: neúplný katalog by
     * vzorek nepřeložil a mezipaměť by po přepnutí jazyka zůstala stará.
     * i18n_lang_set() mění jazyk přes LANGUAGE, testy LANGUAGE přímo. */
    int lang_id = (int) g_i18n_lang_settings.language;
    const char *lang_env = getenv ( "LANGUAGE" );
    if ( !lang_env ) lang_env = "";
#ifdef MZTEST_HEADLESS
    bool stretch = cmt_view_test_stretch ( );
#else
    bool stretch = false;
#endif
    if ( s_cw_mc.valid && s_cw_mc.font_size == ImGui::GetFontSize ( ) && s_cw_mc.font_base == cw_font_base ( )
         && s_cw_mc.frame_pad == st.FramePadding.x && s_cw_mc.mono == mono && s_cw_mc.stretch == stretch && s_cw_mc.lang_id == lang_id && s_cw_mc.lang_env == lang_env )
        return;
    s_cw_mc.valid = true;
    s_cw_mc.font_size = ImGui::GetFontSize ( );
    s_cw_mc.font_base = cw_font_base ( );
    s_cw_mc.frame_pad = st.FramePadding.x;
    s_cw_mc.mono = mono;
    s_cw_mc.stretch = stretch;
    s_cw_mc.lang_id = lang_id;
    s_cw_mc.lang_env = lang_env;

    st_CW_METRICS *m = &s_cw_mc.m;
    memset ( m, 0, sizeof ( *m ) );
    m->F = ImGui::GetFrameHeight ( );
    m->Lh = ImGui::GetTextLineHeight ( );
    m->u = ImGui::GetFontSize ( ) / 7.0f;
    m->pad = 2.0f * m->u;
    m->gap = 2.0f * m->u;
    m->mono = mono;
    m->big_size = cw_font_base ( ) * CW_COUNTER_FONT_FACTOR;
    m->bar_h = floorf ( m->F * 0.4f );
    m->dot_w = floorf ( m->Lh * 0.6f );

    /* počítadlo: velký řez monospace (D-G2-1) */
    ImGui::PushFont ( m->mono, m->big_size );
    m->big_h = ImGui::GetTextLineHeight ( );
    float digits_w = ImGui::CalcTextSize ( "00:00.0" ).x;
    float none_w = ImGui::CalcTextSize ( CW_TIME_NONE ).x;
    ImGui::PopFont ( );
    float captions_w = 0.0f;
    const char *captions[] = { _ ( "Elapsed" ), _ ( "Remaining" ), _ ( "Block time" ), _ ( "Recording time" ) };
    for ( const char *t : captions ) captions_w = ImMax ( captions_w, ImGui::CalcTextSize ( t ).x );
    m->counter_w = ImMax ( ImMax ( digits_w, none_w ), captions_w );

    /* pravý sloupec displeje: nejdelší text všech stavů (okno při změně stavu neposkočí) */
    m->status_w = 0.0f;
    for ( int s = CW_STATUS_NO_TAPE; s <= CW_STATUS_DIRECT; s++ ) {
        ImU32 col;
        bool dot;
        const char *t = cw_status_text ( ( en_CW_STATUS ) s, &col, &dot );
        m->status_w = ImMax ( m->status_w, m->dot_w + m->u + ImGui::CalcTextSize ( t ).x );
    }
    m->motor_w = m->dot_w + m->u + ImMax ( ImGui::CalcTextSize ( _ ( "Motor on" ) ).x, ImGui::CalcTextSize ( _ ( "Motor off" ) ).x );
    const char *right_texts[] = {
        _ ( "No block" ), _ ( "New recording" ), _ ( "Next LOAD takes this block" ), _ ( "This block cannot be loaded directly" ),
    };
    s_cw_mc.right_static_w = 0.0f;
    for ( const char *t : right_texts ) s_cw_mc.right_static_w = ImMax ( s_cw_mc.right_static_w, ImGui::CalcTextSize ( t ).x );
    {
        /* počet pulzů záznamu monospace (do 9 999 999 pulzů bez posunu) */
        std::string pulses = cw_thousands ( 9999999u );
        char buf[96];
        snprintf ( buf, sizeof ( buf ), _ ( "%s pulses" ), pulses.c_str ( ) );
        ImGui::PushFont ( m->mono, 0.0f );
        s_cw_mc.right_static_w = ImMax ( s_cw_mc.right_static_w, ImGui::CalcTextSize ( buf ).x );
        ImGui::PopFont ( );
    }

    m->row1_h = m->F;
    m->btn_h = floorf ( m->F * CW_TRANSPORT_H_FACTOR + 0.5f );
    m->btn_w = floorf ( m->F * CW_TRANSPORT_W_FACTOR + 0.5f );

    /* řádek 1: jméno (aspoň 4 znaky s elipsou), Modified, tři tlačítka (delší ze dvou sad) */
    float name_min = ImGui::CalcTextSize ( "WW...WW" ).x;
    float set_tape = cw_button_w ( _L ( "Open..." ) ) + cw_button_w ( _L ( "Eject" ) ) + cw_button_w ( _L ( "Blocks" ) );
    float set_none = cw_button_w ( _L ( "Open tape..." ) ) + cw_button_w ( _L ( "New tape" ) ) + cw_button_w ( _L ( "Blocks" ) );
    s_cw_mc.row1_min = ImMax ( name_min + m->gap + ImGui::CalcTextSize ( _ ( "Modified" ) ).x, ImGui::CalcTextSize ( _ ( "No tape" ) ).x )
        + 2.0f * m->gap + ImMax ( set_tape, set_none ) + 2.0f * m->u;

    /* řádek 3: pět tlačítek, mezera, Record */
    s_cw_mc.row3_min = 5.0f * m->btn_w + 4.0f * m->u + 2.0f * m->gap + m->btn_w;

    /* řádek 4: popisky, výběr rychlosti, pevné texty, segmenty */
    m->speed_label_w = ImGui::CalcTextSize ( _ ( "Signal speed" ) ).x;
    s_cw_mc.combo_static_w = 0.0f;
    for ( const ImGuiCmtSpeedList_t &s : cw_speed_list ( ) )
        s_cw_mc.combo_static_w = ImMax ( s_cw_mc.combo_static_w, ImGui::CalcTextSize ( s.label_txt.c_str ( ), NULL, true ).x );
    std::string fixed_custom = std::string ( _ ( "Custom pulses" ) ) + " " + _ ( "(set in file)" );
    const char *fixed_texts[] = { _ ( "Not used in Direct mode" ), _ ( "(recorded signal)" ), fixed_custom.c_str ( ) };
    s_cw_mc.fixed_speed_w = 0.0f;
    for ( const char *t : fixed_texts ) s_cw_mc.fixed_speed_w = ImMax ( s_cw_mc.fixed_speed_w, ImGui::CalcTextSize ( t ).x );
    /* pevná rychlost je v rámečku zakázaného prvku: text + vnitřní okraje */
    s_cw_mc.fixed_speed_w += 2.0f * st.FramePadding.x;
    m->loading_label_w = ImGui::CalcTextSize ( _ ( "Loading" ) ).x;
    m->seg_w = ImMax ( ImMax ( cw_button_w ( _L ( "Tape" ) ), cw_button_w ( _L ( "Boost" ) ) ), cw_button_w ( _L ( "Direct" ) ) );
    m->seg_w = ImMax ( m->seg_w, 3.0f * m->F );
}

/**
 * @brief Nastaví zalomení řádků 2 a 4 podle šířky obsahu decku a přepočítá výšky.
 * @param m Rozměry (po cw_measure()). @param deck_w Šířka obsahu decku bez okrajů panelu (px).
 * @post wrap2 / wrap4 = řádek se do deck_w nevejde na jednom řádku; row2_h, row4_h a deck_h podle nich.
 */
static void cw_metrics_wrap ( st_CW_METRICS *m, float deck_w )
{
    m->wrap2 = deck_w + 0.5f < m->row2_min_w;
    m->wrap4 = deck_w + 0.5f < m->row4_min_w;
    float left_h = m->big_h + m->Lh;
    float right_h = m->wrap2 ? ( 5.0f * m->Lh + 4.0f * m->u ) : ( 4.0f * m->Lh + 3.0f * m->u );
    m->row2_h = ImMax ( left_h, right_h );
    m->row4_h = m->wrap4 ? ( 2.0f * m->F + m->u ) : m->F;
    /* pad, řádek, 2 pad (čára), řádek, ... , pad */
    m->deck_h = m->pad + m->row1_h + 2.0f * m->pad + m->row2_h + 2.0f * m->pad + m->btn_h + 2.0f * m->pad + m->row4_h + m->pad;
}

/**
 * @brief Spočítá rozměry decku pro danou kopii (nejdelší texty všech stavů).
 *
 * Pevné texty bere z mezipaměti (cw_measure_static()), každý snímek měří
 * jen "Block n / m" (počet bloků kopie) a text výchozí rychlosti.
 * Zalomení zůstane vypnuté (deck_h pro jednořádkový deck); kreslení ho
 * nastaví podle skutečné šířky (cw_metrics_wrap()).
 *
 * @param v Kopie. @param[out] m Rozměry.
 * @pre UI vlákno, uvnitř snímku ImGui (aktivní písmo).
 */
static void cw_measure ( const st_CMT_VIEW *v, st_CW_METRICS *m )
{
    cw_measure_static ( );
    *m = s_cw_mc.m;
    const ImGuiStyle &st = ImGui::GetStyle ( );

    char line[96];
    int count = ImMax ( v->block_count + 1, 1 );
    snprintf ( line, sizeof ( line ), _ ( "Block %d / %d" ), count, count );
    float right_text_w = ImMax ( s_cw_mc.right_static_w, ImGui::CalcTextSize ( line ).x );
    m->right_min_w = ImMax ( right_text_w, m->status_w + 3.0f * m->u + m->motor_w );
    m->right_wrap_w = ImMax ( right_text_w, ImMax ( m->status_w, m->motor_w ) );

    m->combo_w = ImMax ( s_cw_mc.combo_static_w, ImGui::CalcTextSize ( v->default_speed_text.c_str ( ) ).x );
    m->combo_w += st.FramePadding.x * 2.0f + m->F;
    m->speed_value_w = ImMax ( m->combo_w, s_cw_mc.fixed_speed_w );

    /* řádek 2: počítadlo, oddělovač, pravý sloupec */
    m->row2_min_w = m->counter_w + 4.0f * m->gap + m->right_min_w;
    m->row2_wrap_w = m->counter_w + 4.0f * m->gap + m->right_wrap_w;
    /* řádek 4: Signal speed + výběr / text, Loading + 3 segmenty (zalomený: delší z obou částí) */
    float speed_part = m->speed_label_w + m->gap + m->speed_value_w;
    float loading_part = m->loading_label_w + m->gap + 3.0f * m->seg_w;
    m->row4_min_w = speed_part + 3.0f * m->gap + loading_part;
    m->row4_wrap_w = ImMax ( speed_part, loading_part );
    m->row13_min_w = ImMax ( s_cw_mc.row1_min, s_cw_mc.row3_min );

    m->deck_min_w = ImMax ( ImMax ( m->row13_min_w, m->row2_min_w ), m->row4_min_w );
    m->deck_wrap_w = ImMax ( ImMax ( m->row13_min_w, m->row2_wrap_w ), m->row4_wrap_w );
    cw_metrics_wrap ( m, m->deck_min_w );
    /* tabulka a vlastnosti doplní cw_measure_full() (potřebuje šířky sloupců) */
    m->table_min_w = 0.0f;
    m->table_min_h = 0.0f;
    m->props_min_h = 0.0f;
}

#ifdef MZTEST_HEADLESS
/** @brief Testy: náhradní pracovní plocha (x <= 0 = skutečná, imgui_cmt_window_test_set_work_size()). */
static ImVec2 s_cw_test_work ( 0.0f, 0.0f );
#endif

/**
 * @brief Pracovní plocha hlavního okna aplikace pro velikost okna kazety.
 *
 * Velikost pracovní plochy hlavního viewportu (bez hlavního menu); testy ji
 * mohou nahradit (imgui_cmt_window_test_set_work_size()).
 *
 * @return Velikost v px (aspoň 1 x 1).
 * @pre Uvnitř snímku ImGui.
 */
static ImVec2 cw_work_size ( void )
{
    ImVec2 ws = ImGui::GetMainViewport ( )->WorkSize;
#ifdef MZTEST_HEADLESS
    if ( s_cw_test_work.x > 0.0f ) ws = ImVec2 ( s_cw_test_work.x, ( s_cw_test_work.y > 0.0f ) ? s_cw_test_work.y : ws.y );
#endif
    return ImVec2 ( ImMax ( ws.x, 1.0f ), ImMax ( ws.y, 1.0f ) );
}

/**
 * @brief Nejmenší velikost okna, při které se nic neořízne (s ohledem na pracovní plochu).
 *
 * Šířka: deck bez zalomení (a tabulka ve Full); nevejde-li se na pracovní
 * plochu @p work, deck se zalomeným stavovým řádkem a řádkem 4 (ve Full
 * nejméně tabulka), a nevejde-li se ani to, pracovní plocha. Výška podle
 * zalomení při té šířce, nejvýš výška pracovní plochy (pak se zmenší
 * tabulka a vlastnosti, obsah decku se může oříznout).
 *
 * @param m Rozměry decku (zalomení se nastaví podle výsledné šířky).
 * @param layout Rozvržení. @param work Pracovní plocha (px).
 * @return Velikost okna v px (titulek, menu, okraje okna, deck, oblast seznamu, řádek zpráv).
 */
static ImVec2 cw_min_window_size ( st_CW_METRICS *m, en_CMT_WINDOW_LAYOUT layout, ImVec2 work )
{
    const ImGuiStyle &st = ImGui::GetStyle ( );
    float frame_w = 2.0f * m->pad + 2.0f * st.WindowPadding.x;
    float table_w = ( layout == CMT_WINDOW_LAYOUT_FULL ) ? m->table_min_w + 2.0f * st.WindowPadding.x : 0.0f;
    float w = ImMax ( m->deck_min_w + frame_w, table_w );
    if ( w > work.x ) w = ImMax ( m->deck_wrap_w + frame_w, table_w );
    if ( w > work.x ) w = work.x;
    cw_metrics_wrap ( m, w - frame_w );
    /* titulek a menu bar mají výšku rámečku (TitleBarHeight, MenuBarHeight) */
    float h = 2.0f * m->F + 2.0f * st.WindowPadding.y + m->deck_h;
    if ( layout == CMT_WINDOW_LAYOUT_FULL ) h += m->gap + m->table_min_h + m->gap + m->props_min_h;
    /* řádek zpráv: mezera, čára, mezera, text */
    h += 2.0f * m->gap + 1.0f + m->Lh;
    h = ImMin ( h, work.y );
    return ImVec2 ( ceilf ( w ), ceilf ( h ) );
}

/**
 * @brief Výchozí velikost okna rozvržení (SPEC K01), aspoň minimální a nejvýš pracovní plocha.
 *
 * Full: aspoň CW_FULL_H_FACTOR výšek rámečku a aspoň výška, při které má
 * tabulka se sbalenými vlastnostmi CW_TABLE_DEFAULT_ROWS celých řádků
 * (minimum počítá s CW_TABLE_MIN_ROWS řádky). Při 28 px a 100 %
 * 1020 x 682 px.
 *
 * @param m Rozměry. @param layout Rozvržení. @param min Nejmenší velikost (cw_min_window_size()).
 * @param work Pracovní plocha (px). @return Velikost v px.
 */
static ImVec2 cw_default_window_size ( const st_CW_METRICS *m, en_CMT_WINDOW_LAYOUT layout, ImVec2 min, ImVec2 work )
{
    ImVec2 d = ( layout == CMT_WINDOW_LAYOUT_FULL )
        ? ImVec2 ( CW_FULL_W_FACTOR * m->F, ImMax ( CW_FULL_H_FACTOR * m->F,
                                                    min.y + ( float ) ( CW_TABLE_DEFAULT_ROWS - CW_TABLE_MIN_ROWS ) * ( m->Lh + m->u ) ) )
        : ImVec2 ( CW_COMPACT_W_FACTOR * m->F, CW_COMPACT_H_FACTOR * m->F );
    d = ImVec2 ( ceilf ( ImMax ( d.x, min.x ) ), ceilf ( ImMax ( d.y, min.y ) ) );
    return ImVec2 ( ImMin ( d.x, ImMax ( work.x, min.x ) ), ImMin ( d.y, ImMax ( work.y, min.y ) ) );
}


/* ------------------------------------------------------------------------- */
/*  Kreslení prvků                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief Tooltip prvku: "Popisek (Zkratka)" a na dalším řádku důvod zákazu.
 * @param label Popisek (přeložený), NULL = jen důvod. @param shortcut Zkratka (nepřekládá se), NULL = bez ní.
 * @param reason Důvod zákazu (přeložený), NULL = prvek je povolený.
 * @pre Hned po prvku; funguje i u zakázaného prvku.
 */
static void cw_tooltip ( const char *label, const char *shortcut, const char *reason )
{
    if ( !ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled ) ) return;
    std::string t;
    if ( label ) {
        t = label;
        if ( shortcut ) {
            t += " (";
            t += shortcut;
            t += ")";
        }
    }
    if ( reason ) {
        if ( !t.empty ( ) ) t += "\n";
        t += reason;
    }
    if ( !t.empty ( ) ) ImGui::SetTooltip ( "%s", t.c_str ( ) );
}

/**
 * @brief Text na dané pozici v barvě tokenu, zaznamenaný pro testy.
 * @param name Jméno prvku. @param pos Levý horní roh. @param text Text. @param color Barva.
 * @param anchor Kotva. @return Šířka textu.
 */
static float cw_text_at ( const char *name, ImVec2 pos, const std::string &text, ImU32 color, en_CW_ANCHOR anchor )
{
    ImGui::SetCursorScreenPos ( pos );
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    ImGui::TextUnformatted ( text.c_str ( ), text.c_str ( ) + text.size ( ) );
    ImGui::PopStyleColor ( );
    cw_record_item ( name, text.c_str ( ), anchor, false );
    return ImGui::GetItemRectSize ( ).x;
}

/**
 * @brief Text na dané pozici mimo deck (tabulka, vlastnosti), zaznamenaný pro testy s daným druhem.
 * @param name Jméno prvku. @param pos Levý horní roh. @param text Text. @param color Barva. @param kind Druh záznamu.
 * @return Šířka textu.
 */
static float cw_text_at_kind ( const char *name, ImVec2 pos, const std::string &text, ImU32 color, en_CW_KIND kind )
{
    ImGui::SetCursorScreenPos ( pos );
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    ImGui::TextUnformatted ( text.c_str ( ), text.c_str ( ) + text.size ( ) );
    ImGui::PopStyleColor ( );
    cw_record ( name, text.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), kind, CW_ANCHOR_LEFT, false );
    return ImGui::GetItemRectSize ( ).x;
}

/**
 * @brief Tečka stavu (kruh v barvě) na místě široké m->dot_w, zaznamenaná pro testy.
 * @param name Jméno prvku. @param pos Levý horní roh řádku. @param m Rozměry. @param color Barva tečky.
 */
static void cw_dot_at ( const char *name, ImVec2 pos, const st_CW_METRICS *m, ImU32 color )
{
    ImGui::SetCursorScreenPos ( pos );
    ImGui::Dummy ( ImVec2 ( m->dot_w, m->Lh ) );
    ImVec2 a = ImGui::GetItemRectMin ( );
    ImGui::GetWindowDrawList ( )->AddCircleFilled ( ImVec2 ( a.x + m->dot_w * 0.5f, a.y + m->Lh * 0.5f ), m->Lh * 0.2f, color );
    cw_record_item ( name, NULL, CW_ANCHOR_LEFT, false );
}

/** @brief Druh ikony tlačítka transportu. */
typedef enum en_CW_GLYPH {
    CW_GLYPH_REWIND = 0, /**< svislá čára + dva trojúhelníky doleva (na začátek pásky) */
    CW_GLYPH_PREV,       /**< svislá čára + trojúhelník doleva (předchozí blok) */
    CW_GLYPH_PLAY,       /**< trojúhelník doprava */
    CW_GLYPH_PAUSE,      /**< dvě svislé čáry */
    CW_GLYPH_STOP,       /**< čtverec */
    CW_GLYPH_NEXT,       /**< trojúhelník doprava + svislá čára (další blok) */
    CW_GLYPH_RECORD,     /**< kroužek (nenahrává) */
    CW_GLYPH_RECORD_ON,  /**< plný kruh (nahrává) */
} en_CW_GLYPH;

/**
 * @brief Jméno ikony pro testy (zaznamenaný text tlačítka transportu).
 * @param g Ikona.
 * @return Statický řetězec: "rewind", "prev", "play", "pause", "stop", "next", "record", "record_active".
 */
static const char *cw_glyph_name ( en_CW_GLYPH g )
{
    switch ( g ) {
        case CW_GLYPH_REWIND: return "rewind";
        case CW_GLYPH_PREV: return "prev";
        case CW_GLYPH_PLAY: return "play";
        case CW_GLYPH_PAUSE: return "pause";
        case CW_GLYPH_STOP: return "stop";
        case CW_GLYPH_NEXT: return "next";
        case CW_GLYPH_RECORD: return "record";
        case CW_GLYPH_RECORD_ON:
        default: return "record_active";
    }
}

/**
 * @brief Nakreslí ikonu tlačítka transportu (tvary podle glyfů dřívějšího okna, velikost z výšky tlačítka).
 * @param g Ikona. @param rmin Levý horní roh tlačítka. @param rmax Pravý dolní roh. @param color Barva.
 */
static void cw_draw_glyph ( en_CW_GLYPH g, ImVec2 rmin, ImVec2 rmax, ImU32 color )
{
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    float h = ( rmax.y - rmin.y ) * 0.42f;   /* výška ikony */
    float tw = h * 0.8f;                      /* šířka trojúhelníku */
    float bw = ImMax ( 1.0f, floorf ( h * 0.16f + 0.5f ) ); /* šířka svislé čáry */
    float cx = floorf ( ( rmin.x + rmax.x ) * 0.5f );
    float cy = floorf ( ( rmin.y + rmax.y ) * 0.5f );
    float top = cy - h * 0.5f, bot = cy + h * 0.5f;
    switch ( g ) {
        case CW_GLYPH_REWIND:
        {
            float x = cx - ( bw + 2.0f * tw ) * 0.5f;
            dl->AddRectFilled ( ImVec2 ( x, top ), ImVec2 ( x + bw, bot ), color );
            x += bw;
            dl->AddTriangleFilled ( ImVec2 ( x, cy ), ImVec2 ( x + tw, top ), ImVec2 ( x + tw, bot ), color );
            x += tw;
            dl->AddTriangleFilled ( ImVec2 ( x, cy ), ImVec2 ( x + tw, top ), ImVec2 ( x + tw, bot ), color );
            break;
        }
        case CW_GLYPH_PREV:
        {
            float x = cx - ( bw + tw ) * 0.5f;
            dl->AddRectFilled ( ImVec2 ( x, top ), ImVec2 ( x + bw, bot ), color );
            x += bw;
            dl->AddTriangleFilled ( ImVec2 ( x, cy ), ImVec2 ( x + tw, top ), ImVec2 ( x + tw, bot ), color );
            break;
        }
        case CW_GLYPH_PLAY:
        {
            float x = cx - tw * 0.5f;
            dl->AddTriangleFilled ( ImVec2 ( x, top ), ImVec2 ( x, bot ), ImVec2 ( x + tw, cy ), color );
            break;
        }
        case CW_GLYPH_PAUSE:
        {
            float pw = ImMax ( 2.0f, floorf ( h * 0.28f + 0.5f ) );
            float sp = floorf ( pw * 0.8f + 0.5f );
            float x = cx - ( 2.0f * pw + sp ) * 0.5f;
            dl->AddRectFilled ( ImVec2 ( x, top ), ImVec2 ( x + pw, bot ), color );
            dl->AddRectFilled ( ImVec2 ( x + pw + sp, top ), ImVec2 ( x + 2.0f * pw + sp, bot ), color );
            break;
        }
        case CW_GLYPH_STOP:
        {
            float s = h * 0.8f * 0.5f;
            dl->AddRectFilled ( ImVec2 ( cx - s, cy - s ), ImVec2 ( cx + s, cy + s ), color );
            break;
        }
        case CW_GLYPH_NEXT:
        {
            float x = cx - ( bw + tw ) * 0.5f;
            dl->AddTriangleFilled ( ImVec2 ( x, top ), ImVec2 ( x, bot ), ImVec2 ( x + tw, cy ), color );
            x += tw;
            dl->AddRectFilled ( ImVec2 ( x, top ), ImVec2 ( x + bw, bot ), color );
            break;
        }
        case CW_GLYPH_RECORD:
            dl->AddCircle ( ImVec2 ( cx, cy ), h * 0.5f - bw * 0.5f, color, 0, bw );
            break;
        case CW_GLYPH_RECORD_ON:
        default:
            dl->AddCircleFilled ( ImVec2 ( cx, cy ), h * 0.5f, color );
            break;
    }
}

/**
 * @brief Tlačítko transportu s ikonou, tooltipem "Popisek (Zkratka)" a důvodem zákazu.
 *
 * @param id ID tlačítka ("##..."). @param name Jméno prvku pro testy.
 * @param g Ikona. @param pos Levý horní roh. @param m Rozměry.
 * @param label Popisek tooltipu (přeložený). @param shortcut Zkratka do tooltipu (stejnou akci dělá cw_shortcuts()), NULL = bez zkratky.
 * @param reason Důvod zákazu, NULL = povolené. @param anchor Kotva.
 * @return true = stisknuto.
 */
static bool cw_transport_button ( const char *id, const char *name, en_CW_GLYPH g, ImVec2 pos, const st_CW_METRICS *m,
                                  const char *label, const char *shortcut, const char *reason, en_CW_ANCHOR anchor )
{
    bool disabled = ( reason != NULL );
    bool rec_on = ( g == CW_GLYPH_RECORD_ON );
    if ( rec_on ) {
        ImGui::PushStyleColor ( ImGuiCol_Button, CMT_TOK_REC_ACTIVE_BG );
        ImGui::PushStyleColor ( ImGuiCol_ButtonHovered, CMT_TOK_REC_ACTIVE_BG );
        ImGui::PushStyleColor ( ImGuiCol_ButtonActive, CMT_TOK_REC_ACTIVE_BG );
    }
    ImGui::SetCursorScreenPos ( pos );
    ImGui::BeginDisabled ( disabled );
    /* bez navigace klávesnicí (ani po kliknutí myší): mezerník ani Enter akci
     * decku nespustí, z klávesnice jen zkratky s Ctrl (D-G4-1) */
    ImGui::PushItemFlag ( ImGuiItemFlags_NoNav, true );
    bool pressed = ImGui::ButtonEx ( id, ImVec2 ( m->btn_w, m->btn_h ), ImGuiButtonFlags_NoNavFocus );
    ImGui::PopItemFlag ( );
    ImGui::EndDisabled ( );
    if ( rec_on ) ImGui::PopStyleColor ( 3 );
    ImU32 color = CMT_TOK_TEXT;
    if ( disabled ) color = CMT_TOK_ICON_DISABLED;
    else if ( g == CW_GLYPH_RECORD || g == CW_GLYPH_RECORD_ON ) color = CMT_TOK_REC;
    cw_draw_glyph ( g, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), color );
    cw_tooltip ( label, shortcut, reason );
    cw_record_item ( name, cw_glyph_name ( g ), anchor, disabled );
    cw_record_reason ( reason );
    return pressed && !disabled;
}

/**
 * @brief Tlačítko s textem, volitelně "zamáčknuté" / primární (výplň segment-on).
 * @param label Popisek s ID (_L()). @param name Jméno prvku. @param pos Levý horní roh.
 * @param on Výplň segment-on (primární tlačítko, zamáčknuté Blocks, aktivní segment).
 * @param reason Důvod zákazu, NULL = povolené. @param tip Popisek tooltipu (přeložený; NULL = jen důvod).
 * @param shortcut Zkratka do tooltipu "Popisek (Zkratka)" jako u transportu (nepřekládá se), NULL = bez ní.
 * @param anchor Kotva. @param width Šířka (0 = cw_button_w()).
 * @return true = stisknuto.
 */
static bool cw_text_button ( const char *label, const char *name, ImVec2 pos, bool on, const char *reason, const char *tip,
                             const char *shortcut, en_CW_ANCHOR anchor, float width )
{
    bool disabled = ( reason != NULL );
    if ( on ) {
        ImGui::PushStyleColor ( ImGuiCol_Button, CMT_TOK_SEGMENT_ON );
        ImGui::PushStyleColor ( ImGuiCol_ButtonHovered, CMT_TOK_SEGMENT_ON_HOVER );
        ImGui::PushStyleColor ( ImGuiCol_ButtonActive, CMT_TOK_SEGMENT_ON );
    }
    ImGui::SetCursorScreenPos ( pos );
    ImGui::BeginDisabled ( disabled );
    ImGui::PushItemFlag ( ImGuiItemFlags_NoNav, true ); /* jako tlačítka transportu (D-G4-1) */
    bool pressed = ImGui::ButtonEx ( label, ImVec2 ( ( width > 0.0f ) ? width : cw_button_w ( label ), 0.0f ), ImGuiButtonFlags_NoNavFocus );
    ImGui::PopItemFlag ( );
    ImGui::EndDisabled ( );
    if ( on ) ImGui::PopStyleColor ( 3 );
    cw_tooltip ( tip, shortcut, reason );
    const char *end = ImGui::FindRenderedTextEnd ( label );
    std::string shown ( label, end );
    cw_record_item ( name, shown.c_str ( ), anchor, disabled );
    cw_record_reason ( reason );
    return pressed && !disabled;
}

/**
 * @brief Vodorovná dělicí čára decku (token border, 1 px).
 * @param x0 Levý okraj. @param x1 Pravý okraj. @param y Svislá poloha.
 */
static void cw_hline ( float x0, float x1, float y )
{
    ImGui::GetWindowDrawList ( )->AddLine ( ImVec2 ( x0, y + 0.5f ), ImVec2 ( x1, y + 0.5f ), CMT_TOK_BORDER, 1.0f );
}


/* ------------------------------------------------------------------------- */
/*  Řádky decku                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Důvod zákazu všech ovládacích prvků (přehrávání záznamu vstupů), NULL = žádný.
 * @param v Kopie. @return Přeložený text nebo NULL.
 */
static const char *cw_reason_all ( const st_CMT_VIEW *v )
{
    return v->replay_playing ? _ ( "Not available while an input replay is played." ) : NULL;
}

/**
 * @brief Důvod zákazu Open... / Open tape... (tlačítko, menu Tape, Ctrl+O), NULL = povoleno.
 *
 * Otevření jiné pásky se při záznamu vstupů zamyká (zásah by se odmítl),
 * při nahrávání nejdřív Stop.
 *
 * @param v Kopie. @return Přeložený důvod.
 */
static const char *cw_reason_open ( const st_CMT_VIEW *v )
{
    const char *all = cw_reason_all ( v );
    if ( all ) return all;
    if ( v->replay_recording ) return _ ( "Not available while an input replay is recorded." );
    if ( v->has_tape && v->state == CMT_VIEW_STATE_RECORD ) return _ ( "Stop the recording first." );
    return NULL;
}

/**
 * @brief Důvod zákazu Eject (tlačítko, menu Tape, Ctrl+E), NULL = povoleno.
 *
 * Vysunutí se při záznamu vstupů nezamyká (záznam ho zapíše).
 *
 * @param v Kopie. @return Přeložený důvod.
 */
static const char *cw_reason_eject ( const st_CMT_VIEW *v )
{
    const char *all = cw_reason_all ( v );
    if ( all ) return all;
    if ( !v->has_tape ) return _ ( "No tape inserted." );
    if ( v->state == CMT_VIEW_STATE_RECORD ) return _ ( "Stop the recording first." );
    return NULL;
}

/**
 * @brief Důvod zákazu úprav pásky v okně (Delete, Move up / down, Import file here..., tažení řádku), NULL = povoleno.
 *
 * Pořadí podle návrhu okna (SPEC): přehrávání vstupů a záznam vstupů dnešními texty okna, pak bez
 * pásky, pak mimo STOP (i při nahrávání) "Stop the tape first.". Jádro
 * a fronta zásahů podmínky ověří znovu (cmt_tape_edit_allowed()).
 *
 * @param v Kopie. @return Přeložený důvod.
 */
static const char *cw_reason_edit ( const st_CMT_VIEW *v )
{
    const char *all = cw_reason_all ( v );
    if ( all ) return all;
    if ( v->replay_recording ) return _ ( "Not available while an input replay is recorded." );
    if ( !v->has_tape ) return _ ( "No tape inserted." );
    if ( v->state != CMT_VIEW_STATE_STOP ) return _ ( "Stop the tape first." );
    return NULL;
}

/**
 * @brief Důvod zákazu Import file... (menu Tape, Ctrl+I, soubor z OS nad tabulkou), NULL = povoleno.
 *
 * Jako cw_reason_edit(), ale bez pásky povolené: import vloží novou pásku.
 *
 * @param v Kopie. @return Přeložený důvod.
 */
static const char *cw_reason_import ( const st_CMT_VIEW *v )
{
    const char *all = cw_reason_all ( v );
    if ( all ) return all;
    if ( v->replay_recording ) return _ ( "Not available while an input replay is recorded." );
    if ( v->state != CMT_VIEW_STATE_STOP ) return _ ( "Stop the tape first." );
    return NULL;
}

/**
 * @brief Čítač výměn pásky pro import: s páskou čítač kopie, bez pásky 0 (import do prázdného decku).
 *
 * Kopie před prvním plněním nese CMT_VIEW_SERIAL_NONE; bez pásky se vždy
 * posílá 0, které fronta zásahů v prázdném decku přijme.
 *
 * @param v Kopie. @return Čítač pro cmt_request_import_on_tape().
 */
static unsigned cw_import_serial ( const st_CMT_VIEW *v )
{
    return v->has_tape ? v->tape_serial : 0u;
}

static bool cw_menu_item ( const char *label, const char *name, const char *shortcut, const char *reason );

/** @brief Zruší celý výběr řádků tabulky. */
static void cw_sel_clear ( void )
{
    s_cw4.sel.Clear ( );
    s_cw.sel_id = -1;
}

/**
 * @brief Nastaví výběr na jeden řádek.
 * @param id Index bloku (záporný = bez výběru). @param tape_serial Čítač výměn pásky, ke kterému výběr patří.
 */
static void cw_sel_single ( int id, unsigned tape_serial )
{
    s_cw4.sel.Clear ( );
    if ( id >= 0 ) s_cw4.sel.SetItemSelected ( ( ImGuiID ) id, true );
    s_cw.sel_id = id;
    s_cw.sel_tape_serial = tape_serial;
}

/** @brief Je řádek vybraný? @param id Index bloku. @return true = ano. */
static bool cw_sel_has ( int id )
{
    return id >= 0 && s_cw4.sel.Contains ( ( ImGuiID ) id );
}

/** @brief Vybrané řádky vzestupně. @return Indexy bloků. */
static std::vector<int> cw_sel_list ( void )
{
    std::vector<int> out;
    void *it = NULL;
    ImGuiID id;
    while ( s_cw4.sel.GetNextSelectedItem ( &it, &id ) ) out.push_back ( ( int ) id );
    std::sort ( out.begin ( ), out.end ( ) );
    return out;
}

/**
 * @brief Srovná řádek, který ukazují vlastnosti (s_cw.sel_id), s výběrem: musí být vybraný.
 *
 * Ctrl+klik může ubrat právě ten řádek; pak se vezme nejmenší vybraný.
 *
 * @post s_cw.sel_id >= 0 <=> výběr není prázdný.
 */
static void cw_sel_fix_primary ( void )
{
    if ( s_cw4.sel.Size == 0 ) {
        s_cw.sel_id = -1;
        return;
    }
    if ( cw_sel_has ( s_cw.sel_id ) ) return;
    std::vector<int> l = cw_sel_list ( );
    s_cw.sel_id = l.empty ( ) ? -1 : l.front ( );
}

/**
 * @brief Táhne se právě řádek tabulky (payload CW_DND_BLOCK, i zrušené tažení do puštění myši)?
 *
 * Během tažení okno klávesy úprav (Del, Alt+Up, Alt+Down, Ctrl+I) ignoruje:
 * úprava by posunula čísla bloků pod taženým řádkem.
 *
 * @return true = tažení řádku probíhá.
 * @pre Uvnitř snímku ImGui.
 */
static bool cw_row_dragging ( void )
{
    const ImGuiPayload *pl = ImGui::GetDragDropPayload ( );
    if ( pl && pl->IsDataType ( CW_DND_BLOCK ) ) return true;
    return s_cw4.drag_from >= 0 || ( s_cw4.drag_cancel && ImGui::IsMouseDown ( ImGuiMouseButton_Left ) );
}

/**
 * @brief Bloky, se kterými pracuje Delete z kontextového menu řádku: výběr, je-li v něm řádek, jinak jen řádek.
 * @param b Blok řádku. @return Indexy bloků vzestupně.
 */
static std::vector<int> cw_menu_blocks ( const st_CMT_VIEW_BLOCK *b )
{
    if ( cw_sel_has ( b->id ) ) return cw_sel_list ( );
    return std::vector<int> ( 1, b->id );
}

/**
 * @brief Smazání bloků: uložené hned jedním zásahem, s neuloženým blokem nejdřív modál Delete blocks.
 *
 * @param v Kopie (tape_serial, příznaky unsaved bloků). @param ids Bloky vzestupně (platné indexy).
 * @pre Úpravy povolené (cw_reason_edit() == NULL).
 * @post Zásah zařazený (imgui_cmt_tape_remove_blocks()), nebo modál čeká na potvrzení.
 */
static void cw_request_delete ( const st_CMT_VIEW *v, const std::vector<int> &ids )
{
    if ( ids.empty ( ) ) return;
    /* počet = přesně vybrané bloky, které sloupec Status ukazuje jako Unsaved (stav Unsaved má přednost) */
    int unsaved = 0;
    for ( int id : ids )
        if ( id >= 0 && id < v->block_count && v->blocks[( size_t ) id].status == CMT_VIEW_STATUS_UNSAVED ) unsaved++;
    if ( unsaved == 0 ) {
        ( void ) imgui_cmt_tape_remove_blocks ( v->tape_serial, ids.data ( ), ( int ) ids.size ( ) );
        return;
    }
    s_cw4.del_ids = ids;
    s_cw4.del_tape_serial = v->tape_serial;
    s_cw4.del_unsaved = unsaved;
    s_cw4.del_pending = true;
}

/**
 * @brief Důvod zákazu Move up / Move down bloku, NULL = povoleno.
 * @param v Kopie. @param b Blok. @param up true = Move up, false = Move down. @param several Vybraných je víc.
 * @return Přeložený důvod (společné důvody úprav, víc vybraných, první / poslední blok).
 */
static const char *cw_reason_move ( const st_CMT_VIEW *v, const st_CMT_VIEW_BLOCK *b, bool up, bool several )
{
    const char *r = cw_reason_edit ( v );
    if ( r ) return r;
    if ( several ) return _ ( "Select a single block to move it." );
    if ( up && b->id <= 0 ) return _ ( "This is the first block." );
    if ( !up && b->id >= v->block_count - 1 ) return _ ( "This is the last block." );
    return NULL;
}

/**
 * @brief Důvod zákazu Analyze... (kontextové menu řádku) a Tape > Analyze recording..., NULL = povoleno.
 *
 * Společné důvody úprav pásky (cw_reason_edit(): přehrávání a záznam
 * vstupů - fronta by kopii pulzů odmítla -, bez pásky, mimo
 * STOP), pak blok: bez vybraného bloku nebo víc vybraných, jiný blok než
 * surový 0x18 (analyzovat jde jen surový blok).
 *
 * @param v Kopie. @param b Blok (NULL = žádný jednoznačný blok, menu Tape bez výběru nebo s více vybranými).
 * @return Přeložený důvod.
 */
static const char *cw_reason_analyze ( const st_CMT_VIEW *v, const st_CMT_VIEW_BLOCK *b )
{
    const char *r = cw_reason_edit ( v );
    if ( r ) return r;
    if ( !b ) return _ ( "Select one raw recording in the list of blocks." );
    if ( b->kind != CMT_VIEW_KIND_RAW ) return _ ( "Only a raw recording can be analyzed." );
    return NULL;
}

/**
 * @brief Důvod zákazu Convert to profile... (kontextové menu řádku), NULL = povoleno.
 *
 * Společné důvody úprav pásky (cw_reason_edit(): přehrávání a záznam
 * vstupů, bez pásky, mimo STOP), pak víc vybraných řádků (převádí se jeden
 * blok) a blok, který jádro převést neumí (st_CMT_VIEW_BLOCK::profile_convertible:
 * jen blok 0x46 se souborem MZF).
 *
 * @param v Kopie. @param b Blok řádku. @param several Řádek je mezi víc vybranými.
 * @return Přeložený důvod.
 */
static const char *cw_reason_convert ( const st_CMT_VIEW *v, const st_CMT_VIEW_BLOCK *b, bool several )
{
    const char *r = cw_reason_edit ( v );
    if ( r ) return r;
    if ( several ) return _ ( "Select a single block to convert it." );
    if ( !b->profile_convertible ) return _ ( "This block cannot be converted to another profile." );
    return NULL;
}

/**
 * @brief Důvod zákazu Tape > Check tape, NULL = povoleno.
 *
 * Kontrola jen čte (přímé čtení pod zámkem, ne zásah fronty), proto jde
 * v každém stavu transportu i při záznamu a přehrávání vstupů; potřebuje
 * jen vloženou pásku.
 *
 * @param v Kopie. @return Přeložený důvod.
 */
static const char *cw_reason_check ( const st_CMT_VIEW *v )
{
    return v->has_tape ? NULL : _ ( "No tape inserted." );
}

/**
 * @brief Okno se má v příštím snímku rozbalit (sbalené) a ukázat vybraný řádek (imgui_cmt_window_select_block() z okna Tape check).
 *
 * Fokus (vynesení dopředu) okno kazety nedostane: okno Tape check, které na
 * řádek ukázalo, má zůstat vidět i tam, kde okno kazety překrývá.
 */
static bool s_cw_focus_request = false;

/**
 * @brief Okno se má v příštím snímku rozbalit: je sbalené a má otevřený modál.
 *
 * Modál okna, který se nekreslí (okno kreslí jen titulek), by dál blokoval
 * vstup všech ostatních oken, takže by okno kazety nešlo rozbalit ani
 * kliknutím; okno se proto rozbalí samo a modál pokračuje se svým stavem.
 */
static bool s_cw_expand_for_modal = false;

/**
 * @brief Okno se v tomto snímku otevřelo (minulý snímek se nekreslilo): Alt+C ho v tomto snímku nezavře.
 *
 * Globální zkratka Alt+C hlavního okna (global_shortcuts.cpp) okno otevře
 * a to ve stejném snímku dostane fokus (SetNextWindowFocus); stejný stisk by
 * pak vlastní zkratka okna (cw_shortcuts() a sbalené okno) vzala jako zavření
 * a okno by se hned zase zavřelo.
 */
static bool s_cw_opened_this_frame = false;

/** @brief Levý horní roh tabulky bloků (obrazovka) z posledního snímku, kdy se kreslila (Full). */
static ImVec2 s_cw_table_min = ImVec2 ( 0.0f, 0.0f );
/** @brief Pravý dolní roh tabulky bloků z posledního snímku, kdy se kreslila. */
static ImVec2 s_cw_table_max = ImVec2 ( 0.0f, 0.0f );
/** @brief Snímek, kdy se tabulka bloků naposledy kreslila (-1 = nikdy); starší než minulý snímek = neplatný obdélník. */
static int s_cw_table_frame = -1;

/**
 * @brief Řádek 1: páska (jméno, Modified, Open... / Eject / Blocks; bez pásky Open tape... / New tape / Blocks).
 * @param v Kopie. @param m Rozměry. @param x0 Levý okraj obsahu decku. @param x1 Pravý okraj. @param y Horní okraj řádku.
 */
static void cw_row_tape ( const st_CMT_VIEW *v, const st_CW_METRICS *m, float x0, float x1, float y )
{
    const char *all = cw_reason_all ( v );
    float ty = y + ImGui::GetStyle ( ).FramePadding.y;
    bool full = ( s_cw.layout == CMT_WINDOW_LAYOUT_FULL );

    /* tlačítka zprava doleva: Blocks, pak Eject / New tape, pak Open... / Open tape... */
    const char *l_blocks = _L ( "Blocks" );
    float w_blocks = cw_button_w ( l_blocks );
    float bx = x1 - w_blocks;
    const char *r_blocks = v->has_tape ? NULL : _ ( "Open or create a tape to see its blocks." );
    if ( cw_text_button ( l_blocks, "btn_blocks", ImVec2 ( bx, y ), full && v->has_tape, r_blocks,
                          full ? _ ( "Show only the deck" ) : _ ( "Show the list of blocks" ), NULL, CW_ANCHOR_RIGHT, 0.0f ) ) {
        imgui_cmt_window_set_layout ( full ? CMT_WINDOW_LAYOUT_COMPACT : CMT_WINDOW_LAYOUT_FULL );
    }

    if ( v->has_tape ) {
        const char *l_eject = _L ( "Eject" );
        const char *l_open = _L ( "Open..." );
        float ex = bx - m->gap - cw_button_w ( l_eject );
        float ox = ex - m->gap - cw_button_w ( l_open );
        const char *r_open = cw_reason_open ( v );
        const char *r_eject = cw_reason_eject ( v );
        if ( cw_text_button ( l_open, "btn_open", ImVec2 ( ox, y ), false, r_open, _ ( "Open a tape file" ), "Ctrl+O", CW_ANCHOR_RIGHT, 0.0f ) )
            imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_OPEN );
        if ( cw_text_button ( l_eject, "btn_eject", ImVec2 ( ex, y ), false, r_eject, _ ( "Eject the tape" ), "Ctrl+E", CW_ANCHOR_RIGHT, 0.0f ) )
            imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_EJECT );

        /* jméno: elipsa uprostřed, tooltip s celou cestou */
        std::string name = v->tape_name.empty ( ) ? std::string ( _ ( "(new tape)" ) ) : v->tape_name;
        const char *mod = _ ( "Modified" );
        float mod_w = v->modified ? ( m->gap + ImGui::CalcTextSize ( mod ).x ) : 0.0f;
        float name_max = ox - 2.0f * m->gap - x0 - mod_w;
        bool elided = false;
        std::string shown = cw_ellipsize_middle ( name, name_max, &elided );
        float nw = cw_text_at ( "tape_name", ImVec2 ( x0, ty ), shown, CMT_TOK_TEXT, CW_ANCHOR_LEFT );
        if ( elided || !v->tape_path.empty ( ) ) cw_tooltip ( v->tape_path.empty ( ) ? name.c_str ( ) : v->tape_path.c_str ( ), NULL, NULL );
        if ( v->modified ) {
            cw_text_at ( "modified", ImVec2 ( x0 + nw + m->gap, ty ), mod, CMT_TOK_WARN, CW_ANCHOR_LEFT );
            cw_tooltip ( _ ( "The tape has unsaved changes." ), NULL, NULL );
        }
    } else {
        const char *l_new = _L ( "New tape" );
        const char *l_open = _L ( "Open tape..." );
        float nx = bx - m->gap - cw_button_w ( l_new );
        float ox = nx - m->gap - cw_button_w ( l_open );
        const char *r_open = cw_reason_open ( v );
        if ( cw_text_button ( l_open, "btn_open_tape", ImVec2 ( ox, y ), r_open == NULL, r_open, _ ( "Open a tape file" ),
                              "Ctrl+O", CW_ANCHOR_RIGHT, 0.0f ) )
            imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_OPEN );
        if ( cw_text_button ( l_new, "btn_new_tape", ImVec2 ( nx, y ), false, all, _ ( "Insert a new empty tape" ), NULL, CW_ANCHOR_RIGHT, 0.0f ) )
            imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_NEW_TAPE );
        cw_text_at ( "no_tape", ImVec2 ( x0, ty ), _ ( "No tape" ), CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
    }
}

/**
 * @brief Řádek 2: displej (počítadlo s popiskem, blok, název, průběh, stav a motor).
 * @param v Kopie. @param m Rozměry. @param x0 Levý okraj obsahu decku. @param x1 Pravý okraj. @param y Horní okraj řádku.
 */
static void cw_row_display ( const st_CMT_VIEW *v, const st_CW_METRICS *m, float x0, float x1, float y )
{
    /* počítadlo: plocha je tlačítko (přepnutí Elapsed / Remaining), texty uvnitř */
    st_CW_COUNTER c;
    cw_counter ( v, s_cw.show_remaining, &c );
    ImGui::SetCursorScreenPos ( ImVec2 ( x0, y ) );
    ImGui::BeginDisabled ( !c.can_toggle );
    ImGui::PushItemFlag ( ImGuiItemFlags_NoNav, true );
    if ( ImGui::InvisibleButton ( "##counter", ImVec2 ( m->counter_w, m->big_h + m->Lh ), ImGuiButtonFlags_NoNavFocus ) && c.can_toggle )
        s_cw.show_remaining = !s_cw.show_remaining;
    ImGui::PopItemFlag ( );
    ImGui::EndDisabled ( );
    cw_record ( "counter_area", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_CONTAINER, CW_ANCHOR_LEFT, !c.can_toggle );
    if ( c.can_toggle ) cw_tooltip ( s_cw.show_remaining ? _ ( "Click to show the time from the start of the tape" )
                                                         : _ ( "Click to show the time to the end of the tape" ), NULL, NULL );
    else cw_tooltip ( NULL, NULL, c.reason );
    ImGui::PushFont ( m->mono, m->big_size );
    cw_text_at ( "counter", ImVec2 ( x0, y ), c.text, c.dim ? CMT_TOK_TEXT_DIM : CMT_TOK_TEXT, CW_ANCHOR_LEFT );
    ImGui::PopFont ( );
    cw_text_at ( "counter_caption", ImVec2 ( x0, y + m->big_h ), c.caption, CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );

    /* svislý oddělovač */
    float sx = floorf ( x0 + m->counter_w + 2.0f * m->gap ) + 0.5f;
    ImGui::GetWindowDrawList ( )->AddLine ( ImVec2 ( sx, y ), ImVec2 ( sx, y + m->row2_h ), CMT_TOK_BORDER, 1.0f );

    /* pravý sloupec */
    float rx = sx + 2.0f * m->gap;
    float rw = x1 - rx;
    float step = m->Lh + m->u;
    en_CW_STATUS st = cw_status ( v );
    const st_CMT_VIEW_BLOCK *b = cw_current_block ( v );
    char line[96];
    bool recording = v->has_tape && v->state == CMT_VIEW_STATE_RECORD;

    /* 1: Block n / m */
    if ( recording ) {
        snprintf ( line, sizeof ( line ), _ ( "Block %d / %d" ), v->block_count + 1, v->block_count + 1 );
        cw_text_at ( "block_line", ImVec2 ( rx, y ), line, CMT_TOK_TEXT, CW_ANCHOR_LEFT );
    } else if ( b ) {
        snprintf ( line, sizeof ( line ), _ ( "Block %d / %d" ), v->current_block + 1, v->block_count );
        cw_text_at ( "block_line", ImVec2 ( rx, y ), line, CMT_TOK_TEXT, CW_ANCHOR_LEFT );
    } else if ( v->has_tape && v->block_count == 0 ) {
        snprintf ( line, sizeof ( line ), _ ( "Block %d / %d" ), 0, 0 );
        cw_text_at ( "block_line", ImVec2 ( rx, y ), line, CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
    } else {
        cw_text_at ( "block_line", ImVec2 ( rx, y ), _ ( "No block" ), CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
    }

    /* 2: název bloku (elipsa na konci, tooltip s celým jménem) */
    if ( recording ) {
        cw_text_at ( "block_name", ImVec2 ( rx, y + step ), _ ( "New recording" ), CMT_TOK_TEXT, CW_ANCHOR_LEFT );
    } else if ( b ) {
        bool empty = false;
        std::string name = cw_block_display_name ( b, &empty );
        bool elided = false;
        std::string shown = cw_ellipsize_end ( name, rw, &elided );
        cw_text_at ( "block_name", ImVec2 ( rx, y + step ), shown, empty ? CMT_TOK_TEXT_DIM : CMT_TOK_TEXT, CW_ANCHOR_LEFT );
        if ( elided ) cw_tooltip ( name.c_str ( ), NULL, NULL );
    }

    /* 3: průběh bloku / počet pulzů záznamu / nápověda Direct */
    float y3 = y + 2.0f * step;
    if ( recording ) {
        char buf[96];
        snprintf ( buf, sizeof ( buf ), _ ( "%s pulses" ), cw_thousands ( v->rec_pulses ).c_str ( ) );
        ImGui::PushFont ( m->mono, 0.0f );
        cw_text_at ( "pulses", ImVec2 ( rx, y3 ), buf, CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
        ImGui::PopFont ( );
    } else if ( st == CW_STATUS_DIRECT && b ) {
        if ( v->direct_block_loadable ) cw_text_at ( "direct_hint", ImVec2 ( rx, y3 ), _ ( "Next LOAD takes this block" ), CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
        else cw_text_at ( "direct_hint", ImVec2 ( rx, y3 ), _ ( "This block cannot be loaded directly" ), CMT_TOK_WARN, CW_ANCHOR_LEFT );
    } else {
        float frac = ( b && v->block_progress > 0.0 ) ? ( float ) v->block_progress : 0.0f;
        ImGui::SetCursorScreenPos ( ImVec2 ( rx, floorf ( y3 + ( m->Lh - m->bar_h ) * 0.5f ) ) );
        ImGui::PushStyleColor ( ImGuiCol_FrameBg, CMT_TOK_BG );
        ImGui::PushStyleColor ( ImGuiCol_PlotHistogram, CMT_TOK_ACCENT );
        ImGui::ProgressBar ( frac, ImVec2 ( rw, m->bar_h ), "" );
        ImGui::PopStyleColor ( 2 );
        cw_record_item ( "progress", NULL, CW_ANCHOR_LEFT, false );
    }

    /* 4: stav a motor (motor jen při známém stavu, ne bez pásky ani v Direct; SPEC K06) */
    float y4 = y + 3.0f * step;
    ImU32 col;
    bool dot;
    const char *stext = cw_status_text ( st, &col, &dot );
    float x = rx;
    if ( dot ) {
        cw_dot_at ( "status_dot", ImVec2 ( x, y4 ), m, col );
        x += m->dot_w + m->u;
    }
    x += cw_text_at ( "status_text", ImVec2 ( x, y4 ), stext, col, CW_ANCHOR_LEFT );
    if ( v->has_tape && st != CW_STATUS_DIRECT && v->motor_known ) {
        /* úzké okno (malá obrazovka): motor na dalším řádku pod stavem */
        if ( m->wrap2 ) {
            x = rx;
            y4 += step;
        } else {
            x += 3.0f * m->u;
        }
        cw_dot_at ( "motor_dot", ImVec2 ( x, y4 ), m, CMT_TOK_TEXT_DIM );
        x += m->dot_w + m->u;
        cw_text_at ( "motor_text", ImVec2 ( x, y4 ), v->motor_on ? _ ( "Motor on" ) : _ ( "Motor off" ), CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
    }
}

/**
 * @brief Akce decku a jejich důvody zákazu v jednom snímku (tlačítka, zkratky, menu Tape).
 *
 * Jedno místo pro pravidla "režim x prvek" a "stav x motor" (SPEC v2):
 * tlačítko, klávesová zkratka i položka menu dělají totéž a jsou zakázané
 * ze stejného důvodu.
 *
 * @invariant reason == NULL <=> akce je povolená; first / prev_id / next_id
 *            >= 0, je-li příslušná akce povolená.
 */
typedef struct st_CW_ACTIONS {
    const char *r_rewind;  /**< důvod zákazu Rewind (NULL = povoleno) */
    const char *r_prev;    /**< důvod zákazu Previous block */
    const char *r_next;    /**< důvod zákazu Next block */
    const char *r_play;    /**< důvod zákazu Play / Pause */
    const char *r_stop;    /**< důvod zákazu Stop */
    const char *r_record;  /**< důvod zákazu Record / Stop recording */
    const char *r_open;    /**< důvod zákazu Open... / Open tape... */
    const char *r_eject;   /**< důvod zákazu Eject */
    const char *r_save;    /**< důvod zákazu Save / Save as... */
    const char *r_new;     /**< důvod zákazu New tape */
    const char *r_export_mzf; /**< důvod zákazu Export block as MZF... */
    const char *r_export_wav; /**< důvod zákazu Export block as WAV... */
    const char *r_import;  /**< důvod zákazu Import file... (menu Tape, Ctrl+I) */
    const char *r_export_tape; /**< důvod zákazu Export tape as WAV... */
    int first;             /**< první přehratelný blok (cíl Rewind), -1 = žádný */
    bool show_pause;       /**< tlačítko Play / Pause ukazuje Pause (akce = pauza uživatele) */
    bool rec;              /**< probíhá nahrávání (Record = Stop recording) */
} st_CW_ACTIONS;

/**
 * @brief Spočítá akce decku a důvody zákazu z kopie stavu.
 * @param v Kopie. @param[out] a Akce.
 */
static void cw_actions ( const st_CMT_VIEW *v, st_CW_ACTIONS *a )
{
    const char *all = cw_reason_all ( v );
    bool direct = ( v->mode == CMT_VIEW_MODE_DIRECT );
    bool rec = v->has_tape && v->state == CMT_VIEW_STATE_RECORD;
    bool stop = ( v->state == CMT_VIEW_STATE_STOP );
    const char *r_direct = _ ( "Not used in Direct mode" );
    const char *r_notape = _ ( "No tape inserted." );
    const char *r_empty = _ ( "The tape is empty." );
    const char *r_rec = _ ( "Not available while recording." );
    const char *r_replay_rec = v->replay_recording ? _ ( "Not available while an input replay is recorded." ) : NULL;
    a->rec = rec;

    /* posun po pásce: i v Direct (vybírá blok, který vezme příští LOAD); jen
     * k přehratelným blokům, jako jádro (cmt_tape_step_block() přeskakuje
     * řídicí a nepřehratelné bloky, otevřít jde jen přehratelný blok) */
    a->first = cw_playable_from ( v, 0, +1 );
    int prev_id = ( v->current_block > 0 ) ? cw_playable_from ( v, v->current_block - 1, -1 ) : -1;
    int next_id = cw_playable_from ( v, v->current_block + 1, +1 );
    const char *r_seek = all ? all : ( !v->has_tape ? r_notape : ( v->block_count == 0 ? r_empty : ( rec ? r_rec : NULL ) ) );
    a->r_rewind = r_seek ? r_seek : ( a->first < 0 ? _ ( "The tape has no playable block." ) : NULL );
    a->r_prev = a->r_rewind ? a->r_rewind : ( prev_id < 0 ? _ ( "There is no playable block before this one." ) : NULL );
    a->r_next = a->r_rewind ? a->r_rewind : ( next_id < 0 ? _ ( "There is no playable block after this one." ) : NULL );

    /* Play / Pause ukazuje akci (K07): Pause při Playing a Waiting for motor */
    en_CW_STATUS st = cw_status ( v );
    a->show_pause = ( st == CW_STATUS_PLAYING || st == CW_STATUS_WAITING ) && v->state == CMT_VIEW_STATE_PLAY;
    /* při nahrávání jen rozjetí stojící pásky: zrušení pauzy uživatele (MCP, záznam
     * vstupů ji mohly zapnout) a rozjetí nahrávání čekajícího na motor (program,
     * který motor sám nezapne); běžící nahrávání nic */
    const char *r_play_rec = ( rec && !v->paused ) ? r_rec : NULL;
    /* v Direct jen při nahrávání (rozjetí stojícího nahrávání jako mimo Direct) */
    a->r_play = all ? all : ( ( direct && !rec ) ? r_direct : ( !v->has_tape ? r_notape : ( rec ? r_play_rec : ( v->block_count == 0 ? r_empty : NULL ) ) ) );
    /* Stop i v Direct, kdykoli transport nestojí (Fast Open & Play, MCP play / record
     * s hackem): jinak by z okna nešlo pásku zastavit ani Direct opustit */
    a->r_stop = all ? all : ( !v->has_tape ? r_notape : ( stop ? ( direct ? r_direct : _ ( "The tape is already stopped." ) ) : NULL ) );
    /* nahrávání jde i bez pásky (do nové prázdné pásky) a i v Direct: CMT hack
     * patchuje jen čtení, při RECORD motor programu deck řídí (pio8255.c,
     * CMTHACK_TEST_FAKES_MOTOR); při nahrávání = Stop recording */
    a->r_record = all ? all : ( rec ? NULL : ( v->state == CMT_VIEW_STATE_PLAY ? _ ( "Stop the tape first." ) : NULL ) );

    /* páska: otevření jiné se při záznamu vstupů zamyká, vysunutí ne (záznam ho zapíše) */
    a->r_open = cw_reason_open ( v );
    a->r_eject = cw_reason_eject ( v );
    /* uložení a export jsou zásahy emu vlákna: při záznamu vstupů se odmítají */
    const char *r_stopped = !stop ? ( rec ? _ ( "Stop the recording first." ) : _ ( "Stop the tape first." ) ) : NULL;
    a->r_save = all ? all : ( !v->has_tape ? r_notape : ( r_replay_rec ? r_replay_rec : r_stopped ) );
    a->r_new = all ? all : r_stopped;
    const st_CMT_VIEW_BLOCK *b = cw_current_block ( v );
    const char *r_block = !v->has_tape ? r_notape : ( !b ? _ ( "No block is selected on the tape." ) : NULL );
    a->r_export_wav = all ? all : ( r_block ? r_block : ( r_replay_rec ? r_replay_rec : r_stopped ) );
    a->r_export_mzf = a->r_export_wav ? a->r_export_wav : ( !b->has_mzf ? _ ( "The current block does not contain an MZF file." ) : NULL );
    /* import i do prázdného decku (nová páska); export celé pásky jako export
     * bloku jen ve STOP a mimo záznam vstupů (zásah fronty by se odmítl), prázdná páska nic */
    a->r_import = cw_reason_import ( v );
    a->r_export_tape = all ? all : ( !v->has_tape ? r_notape : ( r_replay_rec ? r_replay_rec
        : ( !stop ? _ ( "Stop the tape first." ) : ( v->block_count == 0 ? r_empty : NULL ) ) ) );
}

/**
 * @brief Zařadí operaci kazety s číslem bloku jen pro pásku, kterou okno ukazuje.
 *
 * Číslo bloku patří pásce z kopie stavu; emu vlákno operaci provede jen
 * se stejným čítačem výměn pásky (REPLAY_CMT_GUARD_TAPE), jinak ji zahodí
 * (replay_host_cmt_guarded()).
 *
 * @param v Kopie (tape_serial). @param op Operace (SEEK, SELECT, SELECT_FORCED). @param id Blok.
 * @param flags Další podmínky REPLAY_CMT_GUARD_* (0 = jen páska).
 */
static void cw_block_op ( const st_CMT_VIEW *v, en_REPLAY_CMT_OP op, int id, unsigned flags )
{
    st_REPLAY_CMT_GUARD g;
    g.flags = REPLAY_CMT_GUARD_TAPE | flags;
    g.tape_serial = v->tape_serial;
    replay_host_cmt_guarded ( op, id, &g );
}

/**
 * @brief Akce Rewind (REPLAY_CMT_SEEK na první přehratelný blok, jen nad páskou kopie).
 * @param v Kopie. @param a Akce snímku (povolená).
 */
static void cw_do_rewind ( const st_CMT_VIEW *v, const st_CW_ACTIONS *a )
{
    cw_block_op ( v, REPLAY_CMT_SEEK, a->first, 0u );
}

/**
 * @brief Akce Play / Pause: PLAY ze STOP; v pauze uživatele ji zrušit, jinak pauza uživatele.
 *
 * Zrušení pauzy uživatele nechá motor, jak ho nastavil počítač (stojí-li,
 * stav přejde na Waiting for motor, cmt_pause()). Při nahrávání je akce
 * povolená jen se stojící páskou (cw_actions()) a vždy pošle
 * REPLAY_CMT_PAUSE 0: v pauze uživatele ji zruší, bez ní (Waiting for
 * motor) nahrávání rozjede jako dosavadní "rozjeď čekající pásku" jádra
 * (cmt_pause(0) rozběhne motor; chování verze 2.0.6).
 * Pauza (REPLAY_CMT_PAUSE 1) má podmínku REPLAY_CMT_GUARD_NOT_STOP: skončí-li
 * páska mezi snímkem a provedením, cmt_pause(1) by ve STOP spustil PLAY
 * čekající na motor - emu vlákno ji pak zahodí.
 * REPLAY_CMT_PAUSE 0 při nahrávání má podmínku REPLAY_CMT_GUARD_RECORD:
 * přejde-li deck mezi snímkem a provedením z RECORD přes STOP do PLAY (MCP
 * stop + play), cmt_pause(0) by rozjel přehrávání čekající na program -
 * emu vlákno ji pak zahodí.
 *
 * @param v Kopie.
 */
static void cw_do_play_pause ( const st_CMT_VIEW *v )
{
    st_REPLAY_CMT_GUARD g;
    g.tape_serial = 0u;
    if ( v->state == CMT_VIEW_STATE_STOP ) replay_host_cmt ( REPLAY_CMT_PLAY, 0, NULL );
    else if ( v->state == CMT_VIEW_STATE_RECORD ) {
        g.flags = REPLAY_CMT_GUARD_RECORD;
        replay_host_cmt_guarded ( REPLAY_CMT_PAUSE, 0, &g );
    } else if ( v->user_paused ) replay_host_cmt ( REPLAY_CMT_PAUSE, 0, NULL );
    else {
        g.flags = REPLAY_CMT_GUARD_NOT_STOP;
        replay_host_cmt_guarded ( REPLAY_CMT_PAUSE, 1, &g );
    }
}

/**
 * @brief Akce Record: ve STOP záznam bez cesty (bez pásky do nové prázdné), při nahrávání Stop.
 * @param a Akce snímku (povolená).
 */
static void cw_do_record ( const st_CW_ACTIONS *a )
{
    /* cmt_record() mimo STOP selže; zákaz v PLAY je v cw_actions() */
    if ( a->rec ) replay_host_cmt ( REPLAY_CMT_STOP_BUTTON, 0, NULL );
    else replay_host_cmt ( REPLAY_CMT_RECORD, 0, NULL );
}

/**
 * @brief Řádek 3: transport (Rewind, Previous block, Play / Pause, Stop, Next block; vpravo Record).
 *
 * Požadavky jdou frontou replay_host_cmt() (záznam vstupů je zachytí),
 * Record = REPLAY_CMT_RECORD bez cesty. Rewind = REPLAY_CMT_SEEK na první
 * přehratelný blok (začátek pásky, transport beze změny); Previous / Next
 * jsou povolené, jen když tím směrem přehratelný blok je (jádro řídicí
 * a nepřehratelné bloky přeskakuje). Play / Pause: PLAY ze STOP, v pauze
 * uživatele a při nahrávání se stojící páskou REPLAY_CMT_PAUSE 0, jinak
 * REPLAY_CMT_PAUSE 1 (cw_do_play_pause()). Totéž dělají
 * zkratky (cw_shortcuts()). Tooltip "Rewind" má kontext překladu "cmt":
 * stejný anglický text jinde znamená návrat záznamu vstupů na checkpoint.
 *
 * @param v Kopie. @param a Akce snímku (cw_actions()). @param m Rozměry.
 * @param x0 Levý okraj obsahu decku. @param x1 Pravý okraj. @param y Horní okraj řádku.
 */
static void cw_row_transport ( const st_CMT_VIEW *v, const st_CW_ACTIONS *a, const st_CW_METRICS *m, float x0, float x1, float y )
{
    float step = m->btn_w + m->u;
    float x = x0;
    if ( cw_transport_button ( "##cw_rewind", "btn_rewind", CW_GLYPH_REWIND, ImVec2 ( x, y ), m, C_ ( "cmt", "Rewind" ), "Ctrl+Home", a->r_rewind, CW_ANCHOR_LEFT ) )
        cw_do_rewind ( v, a );
    x += step;
    if ( cw_transport_button ( "##cw_prev", "btn_prev", CW_GLYPH_PREV, ImVec2 ( x, y ), m, _ ( "Previous block" ), "Ctrl+PageUp", a->r_prev, CW_ANCHOR_LEFT ) )
        replay_host_cmt ( REPLAY_CMT_PREV, 0, NULL );
    x += step;
    if ( cw_transport_button ( "##cw_play", "btn_play", a->show_pause ? CW_GLYPH_PAUSE : CW_GLYPH_PLAY, ImVec2 ( x, y ), m,
                               a->show_pause ? _ ( "Pause" ) : _ ( "Play" ), "Ctrl+Space", a->r_play, CW_ANCHOR_LEFT ) )
        cw_do_play_pause ( v );
    x += step;
    if ( cw_transport_button ( "##cw_stop", "btn_stop", CW_GLYPH_STOP, ImVec2 ( x, y ), m, _ ( "Stop" ), "Ctrl+.", a->r_stop, CW_ANCHOR_LEFT ) )
        replay_host_cmt ( REPLAY_CMT_STOP_BUTTON, 0, NULL );
    x += step;
    if ( cw_transport_button ( "##cw_next", "btn_next", CW_GLYPH_NEXT, ImVec2 ( x, y ), m, _ ( "Next block" ), "Ctrl+PageDown", a->r_next, CW_ANCHOR_LEFT ) )
        replay_host_cmt ( REPLAY_CMT_NEXT, 0, NULL );

    /* Record vpravo: přepínač (při nahrávání plná výplň, klik = Stop) */
    if ( cw_transport_button ( "##cw_record", "btn_record", a->rec ? CW_GLYPH_RECORD_ON : CW_GLYPH_RECORD, ImVec2 ( x1 - m->btn_w, y ), m,
                               a->rec ? _ ( "Stop recording" ) : _ ( "Record" ), "Ctrl+R", a->r_record, CW_ANCHOR_RIGHT ) )
        cw_do_record ( a );
}

/**
 * @brief Klávesová zkratka okna: Ctrl + klávesa, bez dalších modifikátorů, při fokusu okna (i potomků).
 * @param key Klávesa. @return true = stisknuto v tomto snímku.
 * @pre Uvnitř okna (mezi Begin() a End()).
 */
static bool cw_chord ( ImGuiKey key )
{
    return ImGui::IsKeyChordPressed ( ImGuiMod_Ctrl | key );
}

/**
 * @brief Zkratky okna (SPEC v2, K11): jen s Ctrl a jen při fokusu okna nebo jeho potomků.
 *
 * Ctrl+Space Play / Pause, Ctrl+. Stop, Ctrl+R Record, Ctrl+Home Rewind,
 * Ctrl+PageUp / Ctrl+PageDown předchozí / další blok, Ctrl+O Open...,
 * Ctrl+S Save, Ctrl+E Eject, Ctrl+I Import file... (na konec pásky);
 * Del a Alt+Up / Alt+Down platí jen při fokusu tabulky (cw_table_keys());
 * zakázaná akce (stejný důvod jako tlačítko) nic neudělá; zakázané Ctrl+I
 * (úprava pásky) ukáže důvod jako varování v řádku zpráv
 * a během tažení řádku se ignoruje.
 * Alt+C (globální zkratka hlavního okna, global_shortcuts.cpp)
 * okno zavře i při jeho fokusu - globální zkratky běží jen při fokusu
 * hlavního okna; ve snímku, kdy se okno otevřelo, ho Alt+C nezavře
 * (s_cw_opened_this_frame). Escape okno nezavírá. Klávesy bez Ctrl nic nespustí:
 * emulovaná klávesnice při fokusu okna kazety stejně nic nedostává
 * (main_window.cpp, jen při fokusu hlavního okna, D-G4-1 v devdoc/cmt.md).
 *
 * @param v Kopie. @param a Akce snímku. @param p_open Viditelnost okna (Alt+C ji vypne).
 * @pre Uvnitř okna, UI vlákno.
 */
static void cw_shortcuts ( const st_CMT_VIEW *v, const st_CW_ACTIONS *a, bool *p_open )
{
    if ( !ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) ) return;
    /* otevřený popup (menu, kontextové menu, výběr rychlosti) má klávesy pro sebe */
    if ( ImGui::IsPopupOpen ( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) ) return;
    /* ne ve snímku otevření: tentýž stisk okno právě otevřel (s_cw_opened_this_frame) */
    if ( !s_cw_opened_this_frame && ImGui::IsKeyChordPressed ( ImGuiMod_Alt | ImGuiKey_C ) ) {
        *p_open = false;
        return;
    }
    if ( cw_chord ( ImGuiKey_Space ) && !a->r_play ) cw_do_play_pause ( v );
    if ( cw_chord ( ImGuiKey_Period ) && !a->r_stop ) replay_host_cmt ( REPLAY_CMT_STOP_BUTTON, 0, NULL );
    if ( cw_chord ( ImGuiKey_R ) && !a->r_record ) cw_do_record ( a );
    if ( cw_chord ( ImGuiKey_Home ) && !a->r_rewind ) cw_do_rewind ( v, a );
    if ( cw_chord ( ImGuiKey_PageUp ) && !a->r_prev ) replay_host_cmt ( REPLAY_CMT_PREV, 0, NULL );
    if ( cw_chord ( ImGuiKey_PageDown ) && !a->r_next ) replay_host_cmt ( REPLAY_CMT_NEXT, 0, NULL );
    if ( cw_chord ( ImGuiKey_O ) && !a->r_open ) imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_OPEN );
    if ( cw_chord ( ImGuiKey_S ) && !a->r_save ) imgui_cmt_tape_save ( false );
    if ( cw_chord ( ImGuiKey_E ) && !a->r_eject ) imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_EJECT );
    /* úprava pásky: během tažení řádku nic; zakázaná ukáže důvod v řádku zpráv */
    if ( cw_chord ( ImGuiKey_I ) && !cw_row_dragging ( ) ) {
        if ( !a->r_import ) imgui_cmt_tape_import ( -1, cw_import_serial ( v ) );
        else cmt_view_post ( a->r_import, CMT_VIEW_MSG_WARNING );
    }
}

/** @brief Scope editoru vlastních pulzů nového okna (ImGuiCmt::openPulsesEditor()). */
#define CW_PULSES_SCOPE "cmt_window_speed"

/**
 * @brief Řádek 4: Signal speed + výběr (nebo text pevné rychlosti), Loading + přepínač Tape | Boost | Direct.
 *
 * Pevná rychlost (Direct, rychlost daná souborem, surový záznam) je ztlumený
 * text v rámečku zakázaného prvku na místě výběru (aspoň šířka výběru), aby
 * se popisek a hodnota nečetly jako jedna věta.
 *
 * Důvody zákazu segmentu Direct (mimo režim Direct) v pořadí přednosti:
 * přehrávání záznamu vstupů (zámek celého okna), vybraná ROM přímé
 * načtení nepodporuje (!st_CMT_VIEW::direct_supported,
 * cmt_view_direct_unsupported_reason()), záznam vstupů, nahrávání, hrající
 * páska, aktuální blok bez MZF. ROM je před přechodnými stavy, protože
 * jejich odstranění by Direct nezpřístupnilo. Tape a Boost na ROM nezávisí.
 *
 * @param v Kopie. @param m Rozměry. @param x0 Levý okraj obsahu decku. @param x1 Pravý okraj. @param y Horní okraj řádku.
 */
static void cw_row_speed_mode ( const st_CMT_VIEW *v, const st_CW_METRICS *m, float x0, float x1, float y )
{
    const char *all = cw_reason_all ( v );
    float ty = y + ImGui::GetStyle ( ).FramePadding.y;
    bool direct = ( v->mode == CMT_VIEW_MODE_DIRECT );
    const st_CMT_VIEW_BLOCK *b = cw_current_block ( v );

    /* --- Signal speed --- */
    cw_text_at ( "speed_label", ImVec2 ( x0, ty ), _ ( "Signal speed" ), CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
    float vx = x0 + m->speed_label_w + m->gap;
    std::string fixed;
    if ( direct ) {
        fixed = _ ( "Not used in Direct mode" );
    } else if ( b && b->speed_kind == CMT_VIEW_SPEED_SET ) {
        fixed = b->speed_text + " " + _ ( "(set in file)" );
    } else if ( b && b->speed_kind == CMT_VIEW_SPEED_NONE ) {
        fixed = ( b->kind == CMT_VIEW_KIND_RAW ) ? std::string ( _ ( "(recorded signal)" ) ) : std::string ( _ ( "(set in file)" ) );
    }
    if ( !fixed.empty ( ) ) {
        const ImGuiStyle &st = ImGui::GetStyle ( );
        ImVec2 fmin ( vx, y );
        ImVec2 fmax ( vx + ImMax ( m->combo_w, cw_text_w ( fixed ) + 2.0f * st.FramePadding.x ), y + m->F );
        /* barvy a okraj zakázaného prvku (DisabledAlpha jako zakázaný výběr rychlosti) */
        ImGui::BeginDisabled ( true );
        ImGui::RenderFrame ( fmin, fmax, ImGui::GetColorU32 ( ImGuiCol_FrameBg ), true, st.FrameRounding );
        ImGui::EndDisabled ( );
        cw_record ( "speed_frame", NULL, fmin, fmax, CW_KIND_CONTAINER, CW_ANCHOR_LEFT, true );
        cw_text_at ( "speed_text", ImVec2 ( vx + st.FramePadding.x, ty ), fixed, CMT_TOK_TEXT_DIM, CW_ANCHOR_LEFT );
    } else {
        const char *reason = all ? all
            : ( v->replay_recording ? _ ( "Not available while an input replay is recorded." )
                : ( v->state != CMT_VIEW_STATE_STOP ? _ ( "Stop the tape to change the speed." ) : NULL ) );
        const std::vector<ImGuiCmtSpeedList_t> &speeds = cw_speed_list ( );
        ImGui::SetCursorScreenPos ( ImVec2 ( vx, y ) );
        ImGui::SetNextItemWidth ( m->combo_w );
        ImGui::BeginDisabled ( reason != NULL );
        ImGui::PushItemFlag ( ImGuiItemFlags_NoNav, true ); /* jako tlačítka decku (D-G4-1) */
        bool combo_open = ImGui::BeginCombo ( "##cw_speed", v->default_speed_text.c_str ( ) );
        ImGui::PopItemFlag ( );
        if ( combo_open ) {
            for ( size_t n = 0; n < speeds.size ( ); n++ ) {
                bool selected = ( ( int ) speeds[n].cmtspeed == v->default_speed );
                if ( ImGui::Selectable ( speeds[n].label_txt.c_str ( ), selected ) ) {
                    if ( cmtspeed_is_custom ( speeds[n].cmtspeed ) ) {
                        /* délky se zadají v editoru, rychlost se změní až po OK */
                        st_MZTAPE_PULSES_LENGTH p;
                        if ( EXIT_SUCCESS != mztape_pulses_set_us ( &p, v->default_pulses_us[0], v->default_pulses_us[1],
                                                                    v->default_pulses_us[2], v->default_pulses_us[3] ) )
                            mztape_pulses_set_us ( &p, 470.0, 494.0, 240.0, 278.0 );
                        ImGuiCmt::openPulsesEditor ( CW_PULSES_SCOPE, 0, &p );
                    } else {
                        cmt_request_set_speed ( speeds[n].cmtspeed );
                    }
                }
                if ( selected ) ImGui::SetItemDefaultFocus ( );
            }
            ImGui::EndCombo ( );
        }
        ImGui::EndDisabled ( );
        cw_tooltip ( _ ( "Speed of the tape signal for blocks without their own speed" ), NULL, reason );
        cw_record_item ( "speed_combo", v->default_speed_text.c_str ( ), CW_ANCHOR_LEFT, reason != NULL );
    }

    /* --- Loading: Tape | Boost | Direct (úzké okno: na druhém řádku, vpravo) --- */
    if ( m->wrap4 ) {
        y += m->F + m->u;
        ty += m->F + m->u;
    }
    float sx = x1 - 3.0f * m->seg_w;
    cw_text_at ( "loading_label", ImVec2 ( sx - m->gap - m->loading_label_w, ty ), _ ( "Loading" ), CMT_TOK_TEXT_DIM, CW_ANCHOR_RIGHT );
    /* Přepnutí jde zásahem emu vlákna (cmt_request_set_loading_mode()), při
     * záznamu vstupů se odmítá. Změna CMT hacku (do / z Direct) jen ve STOP:
     * přímé načtení by hrající pásku zastavilo a při nahrávání by hack pásku
     * nepoužil (záznam by se rozešel s tím, co program uložil). */
    const char *r_mode = all ? all : ( v->replay_recording ? _ ( "Not available while an input replay is recorded." ) : NULL );
    const char *r_hack = r_mode ? r_mode
        : ( v->state == CMT_VIEW_STATE_RECORD ? _ ( "Stop the recording first." )
            : ( v->state != CMT_VIEW_STATE_STOP ? _ ( "Stop the tape first." ) : NULL ) );
    const char *r_tape = direct ? r_hack : r_mode;
    const char *r_boost = direct ? r_hack : r_mode;
    /* ROM bez přímého načtení: za zámkem celého okna (přehrávání záznamu
     * vstupů), před přechodnými stavy (záznam vstupů, hrající páska, blok
     * bez MZF), jejichž odstranění by Direct nezpřístupnilo */
    const char *r_rom = v->direct_supported ? NULL : cmt_view_direct_unsupported_reason ( );
    const char *r_direct = direct ? r_mode : ( all ? all : ( r_rom ? r_rom : ( r_hack ? r_hack
        : ( ( b && !b->has_mzf ) ? _ ( "The current block does not contain an MZF file." ) : NULL ) ) ) );
    struct {
        const char *label;
        const char *name;
        en_CMT_VIEW_MODE mode;
        const char *tip;
        const char *reason;
    } segs[3] = {
        { _L ( "Tape" ), "seg_tape", CMT_VIEW_MODE_TAPE, _ ( "Play the signal in real time" ), r_tape },
        { _L ( "Boost" ), "seg_boost", CMT_VIEW_MODE_BOOST, _ ( "Run at maximum speed while the motor is on" ), r_boost },
        { _L ( "Direct" ), "seg_direct", CMT_VIEW_MODE_DIRECT, _ ( "Copy the program into memory without playing the signal" ), r_direct },
    };
    for ( int i = 0; i < 3; i++ ) {
        bool on = ( v->mode == segs[i].mode );
        const char *reason = on ? NULL : segs[i].reason;
        if ( cw_text_button ( segs[i].label, segs[i].name, ImVec2 ( sx + ( float ) i * m->seg_w, y ), on, reason, segs[i].tip,
                              NULL, CW_ANCHOR_RIGHT, m->seg_w ) && !on ) {
            /* Tape = hack i Boost vypnuté, Boost = jen Boost, Direct = jen hack (D-G4-2) */
            cmt_request_set_loading_mode ( ( segs[i].mode == CMT_VIEW_MODE_DIRECT ) ? CMT_LOADING_MODE_DIRECT
                                           : ( ( segs[i].mode == CMT_VIEW_MODE_BOOST ) ? CMT_LOADING_MODE_BOOST : CMT_LOADING_MODE_TAPE ) );
        }
    }
}


/* ------------------------------------------------------------------------- */
/*  Seznam bloků (rozvržení Full)                                            */
/* ------------------------------------------------------------------------- */

/** @brief Stabilní ID tabulky bloků (nastavení sloupců ukládá ImGui do svého INI). */
#define CW_TABLE_ID "###cmt_blocks"

/** @brief Scope editoru vlastních pulzů bloku (kontextové menu Set speed > Custom...). */
#define CW_BLOCK_PULSES_SCOPE "cmt_window_block"

/**
 * @brief Šířky sloupců tabulky z obsahu (bez vnitřních okrajů buňky), mezipaměť.
 *
 * Počítá se jen při změně pásky, údajů bloků, počtu bloků, písma nebo
 * záznamu, ne každý snímek (300 bloků = 300 x CalcTextSize ve sloupcích
 * Type, Speed, Length, Status). Tabulce se šířky dávají jen jako počáteční
 * šířka sloupce bez uložené šířky (TableSetupColumn()) a na příkaz View >
 * Reset column widths; šířku z INI nebo od uživatele nikdy nepřepíšou
 * (D-G3-19). Minimální šířka okna se z nich počítá vždy.
 *
 * @invariant valid => w[] odpovídá klíči (serial, count, font, stretch, recording).
 */
typedef struct st_CW_TABLE_CACHE {
    bool valid;            /**< obsah platí */
    unsigned tape_serial;  /**< klíč: st_CMT_VIEW::tape_serial */
    unsigned change_serial; /**< klíč: st_CMT_VIEW::change_serial */
    unsigned open_serial;  /**< klíč: st_CMT_VIEW::open_serial */
    int count;             /**< klíč: počet bloků */
    float font;            /**< klíč: velikost písma */
    bool stretch;          /**< klíč: testovací prodloužení textů */
    float w[CW_COL_COUNT]; /**< šířka obsahu sloupce v px; u Name nejmenší šířka */
} st_CW_TABLE_CACHE;

static st_CW_TABLE_CACHE s_cw_tc = { false, 0, 0, 0, 0, 0.0f, false, { 0 } };

/**
 * @brief Délka bloku pro tabulku a vlastnosti ("1:52", neznámá "-").
 * @param s Délka v s (< 0 = neznámá). @return Text.
 */
static std::string cw_len_text ( double s )
{
    if ( s < 0.0 ) return std::string ( "-" );
    long long t = ( long long ) floor ( s + 0.5 );
    char buf[32];
    snprintf ( buf, sizeof ( buf ), "%lld:%02lld", t / 60, t % 60 );
    return std::string ( buf );
}

/**
 * @brief Popisek sloupce tabulky (přeložený; prázdný sloupec značky s ID).
 *
 * "Load" a "Exec" mají kontext překladu "cmt column" (adresa načtení
 * a spuštění bloku); "Load" bez kontextu je jinde sloveso "načíst".
 * @param c Sloupec. @return Popisek pro TableSetupColumn() / TableHeader().
 */
static const char *cw_col_label ( int c )
{
    switch ( c ) {
        case CW_COL_MARK: return "##mark";
        case CW_COL_NUM: return "#";
        case CW_COL_NAME: return _ ( "Name" );
        case CW_COL_TYPE: return _ ( "Type" );
        case CW_COL_SIZE: return _ ( "Size" );
        case CW_COL_LOAD: return C_ ( "cmt column", "Load" );
        case CW_COL_EXEC: return C_ ( "cmt column", "Exec" );
        case CW_COL_SPEED: return _ ( "Speed" );
        case CW_COL_LENGTH: return _ ( "Length" );
        case CW_COL_STATUS:
        default: return _ ( "Status" );
    }
}

/**
 * @brief Text sloupce Speed pro blok.
 * @param b Blok (nesmí být NULL).
 * @return Text rychlosti ("1:1", "3:1", ...); "-" u bloku bez měnitelné rychlosti nebo bez textu.
 */
static std::string cw_speed_cell ( const st_CMT_VIEW_BLOCK *b )
{
    if ( b->speed_kind == CMT_VIEW_SPEED_NONE || b->speed_text.empty ( ) ) return std::string ( "-" );
    return b->speed_text;
}

/**
 * @brief Šířky sloupců tabulky (mezipaměť s_cw_tc), přepočet jen při změně klíče.
 * @param v Kopie. @param m Rozměry decku (písmo monospace).
 */
static void cw_table_widths ( const st_CMT_VIEW *v, const st_CW_METRICS *m )
{
    float font = ImGui::GetFontSize ( );
#ifdef MZTEST_HEADLESS
    bool stretch = cmt_view_test_stretch ( );
#else
    bool stretch = false;
#endif
    if ( s_cw_tc.valid && s_cw_tc.tape_serial == v->tape_serial && s_cw_tc.change_serial == v->change_serial
         && s_cw_tc.open_serial == v->open_serial && s_cw_tc.count == v->block_count && s_cw_tc.font == font
         && s_cw_tc.stretch == stretch ) return;
    float *w = s_cw_tc.w;
    for ( int c = 0; c < CW_COL_COUNT; c++ ) w[c] = ImGui::CalcTextSize ( cw_col_label ( c ), NULL, true ).x;
    w[CW_COL_MARK] = floorf ( m->Lh * 0.6f );
    ImGui::PushFont ( m->mono, 0.0f );
    char buf[32];
    /* aspoň tři číslice: počáteční šířka sloupce platí jen poprvé (D-G3-19) a jiná
     * páska se stovkami bloků by čísla ořízla */
    snprintf ( buf, sizeof ( buf ), "%d", ImMax ( v->block_count + 1, 999 ) );
    w[CW_COL_NUM] = ImMax ( w[CW_COL_NUM], ImGui::CalcTextSize ( buf ).x );
    w[CW_COL_SIZE] = ImMax ( w[CW_COL_SIZE], ImGui::CalcTextSize ( "65 535 B" ).x );
    w[CW_COL_LOAD] = ImMax ( w[CW_COL_LOAD], ImGui::CalcTextSize ( "FFFFh" ).x );
    w[CW_COL_EXEC] = ImMax ( w[CW_COL_EXEC], ImGui::CalcTextSize ( "FFFFh" ).x );
    float len_w = ImGui::CalcTextSize ( "00:00" ).x;
    for ( const st_CMT_VIEW_BLOCK &b : v->blocks ) len_w = ImMax ( len_w, cw_text_w ( cw_len_text ( b.length_s ) ) );
    w[CW_COL_LENGTH] = ImMax ( w[CW_COL_LENGTH], len_w );
    ImGui::PopFont ( );
    /* obvyklé texty: šířka sloupce vytvořeného bez pásky (počáteční šířka platí jen
     * poprvé) nesmí ořezat typ a stav obvyklé pásky */
    w[CW_COL_TYPE] = ImMax ( w[CW_COL_TYPE], ImGui::CalcTextSize ( "01 machine code" ).x );
    {
        /* stavy a popisy řídicích bloků (cmt_view_control_text()) obvyklé délky */
        char loop[64];
        snprintf ( loop, sizeof ( loop ), _ ( "Loop start x%u" ), 99u );
        const char *st_texts[] = { _ ( "Not decoded" ), _ ( "Cannot play" ), _ ( "Stop the tape" ), loop };
        for ( const char *tx : st_texts ) w[CW_COL_STATUS] = ImMax ( w[CW_COL_STATUS], ImGui::CalcTextSize ( tx ).x );
    }
    w[CW_COL_SPEED] = ImMax ( w[CW_COL_SPEED], ImGui::CalcTextSize ( "3:1" ).x );
    /* Name: nejmenší šířka (10 širokých znaků), zbytek šířky dostane roztažením */
    w[CW_COL_NAME] = ImMax ( w[CW_COL_NAME], ImGui::CalcTextSize ( "MMMMMMMMMM" ).x );
    w[CW_COL_NAME] = ImMax ( w[CW_COL_NAME], ImGui::CalcTextSize ( _ ( "Recording..." ) ).x );
    for ( const st_CMT_VIEW_BLOCK &b : v->blocks ) {
        w[CW_COL_TYPE] = ImMax ( w[CW_COL_TYPE], cw_text_w ( b.type_text ) );
        w[CW_COL_SPEED] = ImMax ( w[CW_COL_SPEED], cw_text_w ( cw_speed_cell ( &b ) ) );
        w[CW_COL_STATUS] = ImMax ( w[CW_COL_STATUS], cw_text_w ( b.status_text ) );
    }
    s_cw_tc.valid = true;
    s_cw_tc.tape_serial = v->tape_serial;
    s_cw_tc.change_serial = v->change_serial;
    s_cw_tc.open_serial = v->open_serial;
    s_cw_tc.count = v->block_count;
    s_cw_tc.font = font;
    s_cw_tc.stretch = stretch;
}

/**
 * @brief Nejmenší šířka tabulky bez oříznutí: zapnuté sloupce, vnitřní okraje, čáry, posuvník.
 *
 * Pevný sloupec má šířku uživatele (myš, INI) natrvalo (D-G3-19), proto se
 * u existující tabulky počítá větší z šířky obsahu a požadované šířky
 * sloupce (ImGuiTableColumn::WidthRequest); jinak by rozšířený pevný
 * sloupec při minimální šířce okna ořízl poslední sloupec.
 * Sloupec Name (roztažený) se počítá šířkou obsahu.
 *
 * @return Šířka v px (potřebuje platnou mezipaměť cw_table_widths()).
 */
static float cw_table_min_w ( void )
{
    const ImGuiStyle &st = ImGui::GetStyle ( );
    float cp = 2.0f * ImGui::GetFontSize ( ) / 7.0f; /* vnitřní okraj buňky (2u), viz cw_draw_table() */
    ImGuiTable *t = s_cw.table_id ? ImGui::TableFindByID ( s_cw.table_id ) : NULL;
    float w = st.ScrollbarSize;
    int n = 0;
    for ( int c = 0; c < CW_COL_COUNT; c++ ) {
        if ( !s_cw.col_enabled[c] ) continue;
        float cw = s_cw_tc.w[c];
        if ( t && c != CW_COL_NAME && c < t->ColumnsCount && t->Columns[c].WidthRequest > 0.0f ) cw = ImMax ( cw, t->Columns[c].WidthRequest );
        w += cw + 2.0f * cp;
        n++;
    }
    return w + ( float ) ImMax ( n - 1, 0 ) * 1.0f;
}

/**
 * @brief Výška řádku tabulky (řádek textu + 2 x vnitřní okraj buňky 0,5u; při 28 px 32 px jako koncept).
 *
 * Stejná je i rozteč řádků vlastností, takže tabulka i vlastnosti se
 * posouvají a dělí po celých řádcích stejné výšky.
 */
static float cw_row_h ( const st_CW_METRICS *m )
{
    return m->Lh + m->u;
}

/**
 * @brief Výška tabulky bloků, jejíž tělo ukáže právě @p rows celých řádků.
 *
 * Hlavička (jeden řádek) + @p rows řádků + 1 px: tabulka se svislým
 * posuvem ořezává řádky těla od spodní čáry hlavičky + 1 px (první pixel
 * řádku leží pod čarou hlavičky) a spodní okraj tabulky leží na posledním
 * pixelu rámečku (imgui_tables.cpp, TableEndRow(), větev unfreeze_rows).
 * Viditelné tělo je tak přesně @p rows x cw_row_h().
 *
 * @param m Rozměry. @param rows Počet řádků těla (>= 0).
 * @return Výška v px.
 */
static float cw_table_frame_h ( const st_CW_METRICS *m, int rows )
{
    return ( float ) ( 1 + rows ) * cw_row_h ( m ) + 1.0f;
}

/**
 * @brief Výška sekce vlastností: hlavička, nebo hlavička a obsah s @p rows celými řádky.
 *
 * Rozbalený obsah: čára pod hlavičkou (1 px), horní okraj 2u, @p rows
 * řádků po cw_row_h() (posouvaná oblast), dolní okraj u a spodní okraj
 * rámečku (1 px). Bez řádků (@p rows <= 0) jen hlavička.
 *
 * @param m Rozměry. @param rows Viditelné řádky obsahu (0 = jen hlavička).
 * @return Výška v px.
 */
static float cw_props_frame_h ( const st_CW_METRICS *m, int rows )
{
    if ( rows <= 0 ) return m->F;
    return m->F + 2.0f + 3.0f * m->u + ( float ) rows * cw_row_h ( m );
}

/**
 * @brief Před Begin() posouvané oblasti zarovná cíl posunu na celé řádky.
 *
 * Kolečko myši (UpdateMouseWheel(): 5 x výška písma), navigace klávesnicí
 * a SetScrollHereY() nastavují ImGuiWindow::ScrollTarget, který ImGui
 * uplatní až v Begin() oblasti (CalcNextScrollFromScrollTargetAndClamp()).
 * Funkce spočítá výsledný posun stejným vzorcem (bez přichycení k okraji),
 * zaokrouhlí ho na násobek @p step a uloží jako cíl bez středování; posun
 * menší než půl řádku se zaokrouhlí na jeden celý řádek daným směrem, aby
 * kolečko vždy posunulo.
 *
 * @param w Posouvaná oblast z minulého snímku (NULL = nic).
 * @param step Výška řádku v px (> 0).
 * @post w->ScrollTarget.y je FLT_MAX, nebo násobek @p step se středem 0.
 */
static void cw_scroll_snap_target ( ImGuiWindow *w, float step )
{
    if ( !w || step <= 0.0f || !( w->ScrollTarget.y < FLT_MAX ) ) return;
    float deco = w->DecoOuterSizeY1 + w->DecoInnerSizeY1 + w->DecoOuterSizeY2;
    float target = w->ScrollTarget.y - w->ScrollTargetCenterRatio.y * ( w->SizeFull.y - deco );
    float snapped = roundf ( target / step ) * step;
    if ( snapped == w->Scroll.y && fabsf ( target - w->Scroll.y ) >= 1.0f )
        snapped = w->Scroll.y + ( ( target > w->Scroll.y ) ? step : -step );
    w->ScrollTarget.y = ImMax ( snapped, 0.0f );
    w->ScrollTargetCenterRatio.y = 0.0f;
    w->ScrollTargetEdgeSnapDist.y = 0.0f;
}

/**
 * @brief Po obsahu posouvané oblasti srovná posun, který není na celém řádku (tažení posuvníku).
 *
 * Tažení posuvníku mění ImGuiWindow::Scroll přímo; dokud uživatel táhne,
 * posun se nechá, po puštění se v dalším snímku zarovná na nejbližší
 * řádek. Konec obsahu (ScrollMax) je zarovnaný tím, že výška obsahu je
 * celý počet řádků.
 *
 * @param w Posouvaná oblast (aktuální okno uvnitř ní). @param step Výška řádku v px.
 * @pre Uvnitř snímku ImGui.
 */
static void cw_scroll_snap_after ( ImGuiWindow *w, float step )
{
    if ( !w || step <= 0.0f ) return;
    if ( ImGui::GetActiveID ( ) != 0 && ImGui::GetActiveID ( ) == ImGui::GetWindowScrollbarID ( w, ImGuiAxis_Y ) ) return;
    float s = w->Scroll.y;
    float snapped = roundf ( s / step ) * step;
    if ( fabsf ( snapped - s ) > 0.5f && w->ScrollTarget.y == FLT_MAX ) ImGui::SetScrollY ( w, snapped );
}

/**
 * @brief Barva textu stavu bloku (SPEC v2, K14); ve vybraném řádku ztlumené #B0B0BC a chyba bíle.
 * @param b Blok. @param selected Řádek je vybraný. @return Barva.
 */
static ImU32 cw_status_color ( const st_CMT_VIEW_BLOCK *b, bool selected )
{
    switch ( b->status ) {
        case CMT_VIEW_STATUS_UNSAVED: return CMT_TOK_WARN;
        case CMT_VIEW_STATUS_UNPLAYABLE:
        case CMT_VIEW_STATUS_CHECKSUM_ERROR: return selected ? CMT_TOK_TEXT : CMT_TOK_ERROR;
        case CMT_VIEW_STATUS_OK:
        case CMT_VIEW_STATUS_NOT_DECODED:
        case CMT_VIEW_STATUS_CONTROL:
        default: return selected ? CMT_TOK_TEXT_DIM_SELECTED : CMT_TOK_TEXT_DIM;
    }
}

/**
 * @brief Důvod, proč blok nejde přehrát od začátku (dvojklik, Play from here), NULL = jde.
 * @param v Kopie. @param b Blok. @return Přeložený důvod.
 */
static const char *cw_reason_play_block ( const st_CMT_VIEW *v, const st_CMT_VIEW_BLOCK *b )
{
    if ( v->replay_playing ) return _ ( "Not available while an input replay is played." );
    if ( !b->selectable ) return _ ( "This block cannot be played." );
    if ( v->mode == CMT_VIEW_MODE_DIRECT ) return _ ( "Not used in Direct mode" );
    if ( v->state == CMT_VIEW_STATE_RECORD ) return _ ( "Not available while recording." );
    return NULL;
}

/**
 * @brief Důvod, proč na blok nejde skočit (Ctrl+Enter, kontextové menu Go to this block), NULL = jde.
 *
 * Skok je i v Direct (vybírá blok, který vezme příští LOAD), jako Previous / Next.
 *
 * @param v Kopie. @param b Blok. @return Přeložený důvod.
 */
static const char *cw_reason_jump_block ( const st_CMT_VIEW *v, const st_CMT_VIEW_BLOCK *b )
{
    if ( v->replay_playing ) return _ ( "Not available while an input replay is played." );
    if ( !b->selectable ) return _ ( "This block cannot be played." );
    if ( v->state == CMT_VIEW_STATE_RECORD ) return _ ( "Not available while recording." );
    return NULL;
}

/**
 * @brief Přehraje blok od začátku (dvojklik, Play from here): REPLAY_CMT_SELECT_FORCED, na hrajícím bloku REPLAY_CMT_SEEK.
 *
 * REPLAY_CMT_SELECT_FORCED (cmt_tape_select_block(id, 1)): jiný blok otevře
 * a pustí (v pauze zůstane pauza), stejný blok ve STOP pustí, v pauze
 * uživatele ho jako jiný blok otevře znovu od začátku a pauza zůstane,
 * při čekání jen na motor ho rozjede. Na právě hrajícím
 * bloku by týž požadavek pauzoval, proto tam REPLAY_CMT_SEEK (blok znovu
 * od začátku, hraje dál). D-G3-5. Obě operace
 * platí jen pro pásku kopie (cw_block_op()); SELECT_FORCED navíc s podmínkou
 * REPLAY_CMT_GUARD_NOT_PLAYING_ARG: stane-li se blok mezi snímkem
 * a provedením hrajícím (přechod N -> N+1), emu vlákno operaci zahodí místo
 * pozastavení pásky (blok už hraje).
 *
 * @param v Kopie. @param id Blok.
 */
static void cw_play_from ( const st_CMT_VIEW *v, int id )
{
    if ( id == v->current_block && v->state == CMT_VIEW_STATE_PLAY && !v->paused ) cw_block_op ( v, REPLAY_CMT_SEEK, id, 0u );
    else cw_block_op ( v, REPLAY_CMT_SELECT_FORCED, id, REPLAY_CMT_GUARD_NOT_PLAYING_ARG );
}

/**
 * @brief Kontextové menu řádku: Play from here, Load directly, Export as MZF / WAV, Set speed, Move up / down, Import file here..., Delete, Properties.
 *
 * Vypnuté položky mají tooltip s důvodem (i zakázané). Požadavky: viz
 * cw_play_from(); Load directly = jeden zásah
 * cmt_request_load_directly_on_tape() s čítačem výměn pásky kopie (výběr
 * bloku ve STOP a režim Direct, vše, nebo nic); zakázané s důvodem
 * v pořadí: přehrávání záznamu vstupů, blok bez MZF, vybraná ROM přímé
 * načtení nepodporuje (!st_CMT_VIEW::direct_supported,
 * cmt_view_direct_unsupported_reason(); táž ROM jako segment Direct), záznam
 * vstupů, deck mimo STOP;
 * export přes
 * imgui_cmt_tape_export_block() (dialog souboru menu Tape); rychlost bloku
 * cmt_request_set_block_speed_on_tape(). Vyměnila-li se mezitím páska
 * (čítač výměn pásky jiný než při otevření menu), menu se zavře se zprávou
 * "The tape changed; the action was cancelled." (číslo bloku by patřilo
 * jiné pásce) s úrovní varování (uživatel nic nevybral;
 * menu, jehož řádek se už nekreslí, zavře cw_close_orphan_menu()).
 * Export a editor pulzů nesou čítač dál.
 *
 * Hned za Set speed: Analyze... otevře dialog analýzy surového
 * bloku (cmt_analyze_dialog_open()); zakázané s důvodem mimo STOP, při
 * záznamu a přehrávání vstupů a u jiného než surového bloku
 * (cw_reason_analyze()). Za ní Convert to profile... otevře dialog
 * převodu bloku na jiný profil zapisovače (cmt_convert_dialog_open());
 * zakázané s důvodem mimo STOP, při záznamu a přehrávání vstupů, při více
 * vybraných řádcích a u bloku, který převést nejde (cw_reason_convert()).
 *
 * Za ní: Move up (Alt+Up) a Move down (Alt+Down) přesunou
 * blok řádku o jedno místo (zakázané na prvním / posledním bloku a při více
 * vybraných), Import file here... importuje soubor před řádek, Delete
 * (při více vybraných, je-li řádek mezi nimi, "Delete N blocks") smaže
 * výběr, s neuloženým blokem po potvrzení modálem (cw_request_delete()).
 * Všechny jen ve STOP a mimo záznam a přehrávání vstupů (cw_reason_edit()).
 * Properties vybere jen řádek menu.
 *
 * @param v Kopie. @param b Blok řádku.
 * @pre Uvnitř BeginPopupContextItem(), s_cw.menu_tape_serial nastavený při otevření.
 */
static void cw_row_menu ( const st_CMT_VIEW *v, const st_CMT_VIEW_BLOCK *b )
{
    if ( v->tape_serial != s_cw.menu_tape_serial ) {
        ImGui::CloseCurrentPopup ( );
        /* uživatel nic nevybral: varování, ne chyba */
        cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
        return;
    }
    const char *all = cw_reason_all ( v );
    bool stop = ( v->state == CMT_VIEW_STATE_STOP );
    const char *r_lock = all ? all : ( v->replay_recording ? _ ( "Not available while an input replay is recorded." ) : NULL );
    const char *r_stop = stop ? NULL : _ ( "Stop the tape first." );
    const char *r_mzf = b->has_mzf ? NULL : _ ( "This block does not contain an MZF file." );

    const char *r_play = cw_reason_play_block ( v, b );
    bool clicked = ImGui::MenuItem ( _L ( "Play from here" ), NULL, false, r_play == NULL );
    cw_tooltip ( NULL, NULL, r_play );
    cw_record ( "ctx_play", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, r_play != NULL );
    cw_record_reason ( r_play );
    if ( clicked ) cw_play_from ( v, b->id );

    /* skok na blok (transport beze změny), zkratka Ctrl+Enter na řádku (D-G3-16) */
    const char *r_seek = cw_reason_jump_block ( v, b );
    clicked = ImGui::MenuItem ( _L ( "Go to this block" ), "Ctrl+Enter", false, r_seek == NULL );
    cw_tooltip ( r_seek ? NULL : _ ( "Move the tape to the start of this block; playback state does not change" ), NULL, r_seek );
    cw_record ( "ctx_seek", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, r_seek != NULL );
    cw_record_reason ( r_seek );
    if ( clicked ) cw_block_op ( v, REPLAY_CMT_SEEK, b->id, 0u );

    /* ROM bez přímého načtení za blokem bez MZF (na takovém řádku Load directly
     * nejde nikdy), před zámkem záznamu vstupů a hrající páskou */
    const char *r_rom = v->direct_supported ? NULL : cmt_view_direct_unsupported_reason ( );
    const char *r_load = all ? all : ( r_mzf ? r_mzf : ( r_rom ? r_rom : ( r_lock ? r_lock : r_stop ) ) );
    clicked = ImGui::MenuItem ( _L ( "Load directly" ), NULL, false, r_load == NULL );
    cw_tooltip ( r_load ? NULL : _ ( "Copy the program into memory without playing the signal" ), NULL, r_load );
    cw_record ( "ctx_load", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, r_load != NULL );
    cw_record_reason ( r_load );
    /* výběr bloku a Direct jedním zásahem: vše, nebo nic */
    if ( clicked ) cmt_request_load_directly_on_tape ( v->tape_serial, b->id );

    ImGui::Separator ( );
    const char *r_exp_mzf = r_mzf ? r_mzf : ( r_lock ? r_lock : r_stop );
    clicked = ImGui::MenuItem ( _L ( "Export as MZF..." ), NULL, false, r_exp_mzf == NULL );
    cw_tooltip ( NULL, NULL, r_exp_mzf );
    cw_record ( "ctx_export_mzf", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, r_exp_mzf != NULL );
    cw_record_reason ( r_exp_mzf );
    if ( clicked ) imgui_cmt_tape_export_block ( b->id, false, v->tape_serial );
    const char *r_exp_wav = b->playable ? ( r_lock ? r_lock : r_stop ) : _ ( "This block cannot be played." );
    clicked = ImGui::MenuItem ( _L ( "Export as WAV..." ), NULL, false, r_exp_wav == NULL );
    cw_tooltip ( NULL, NULL, r_exp_wav );
    cw_record ( "ctx_export_wav", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, r_exp_wav != NULL );
    cw_record_reason ( r_exp_wav );
    if ( clicked ) imgui_cmt_tape_export_block ( b->id, true, v->tape_serial );

    ImGui::Separator ( );
    /* rychlost bloku: jen ve STOP (D-G3-14), ne při záznamu vstupů (zásah by se odmítl) */
    const char *r_speed = ( b->speed_kind == CMT_VIEW_SPEED_NONE ) ? _ ( "The speed of this block is given by the file." )
        : ( r_lock ? r_lock : r_stop );
    bool open = ImGui::BeginMenu ( _L ( "Set speed" ), r_speed == NULL );
    cw_tooltip ( NULL, NULL, r_speed );
    cw_record ( "ctx_speed", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, r_speed != NULL );
    cw_record_reason ( r_speed );
    if ( open ) {
        std::vector<ImGuiCmtSpeedList_t> speeds = b->speed_is_mz ? ImGuiCmt::getMzSpeedListDefault ( ) : ImGuiCmt::getZxSpeedList ( );
        for ( size_t n = 0; n < speeds.size ( ); n++ ) {
            bool selected = ( b->speed_kind == CMT_VIEW_SPEED_DEFAULT && n == 0 )
                || ( b->speed_kind == CMT_VIEW_SPEED_SET && ( int ) speeds[n].cmtspeed == b->cmtspeed );
            bool pick = ImGui::MenuItem ( speeds[n].label_txt.c_str ( ), NULL, selected );
#ifdef MZTEST_HEADLESS
            char nm[32];
            snprintf ( nm, sizeof ( nm ), "speed_%u", ( unsigned ) n );
            cw_record ( nm, speeds[n].cmtspeed_txt.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
#endif
            if ( !pick ) continue;
            if ( cmtspeed_is_custom ( speeds[n].cmtspeed ) ) {
                /* délky se zadají v editoru (výchozí jsou současné pulzy bloku, jinak výchozí pulzy) */
                const double *us = ( b->pulses_us[0] > 0.0 ) ? b->pulses_us : v->default_pulses_us;
                st_MZTAPE_PULSES_LENGTH p;
                if ( EXIT_SUCCESS != mztape_pulses_set_us ( &p, us[0], us[1], us[2], us[3] ) )
                    mztape_pulses_set_us ( &p, 470.0, 494.0, 240.0, 278.0 );
                ImGuiCmt::openPulsesEditor ( CW_BLOCK_PULSES_SCOPE, b->id, &p );
                s_cw.pulses_tape_serial = v->tape_serial;
            } else {
                cmt_request_set_block_speed_on_tape ( v->tape_serial, b->id, ( int ) speeds[n].cmtspeed, b->speed_is_mz, NULL );
            }
        }
        ImGui::EndMenu ( );
    }
    std::vector<int> ids = cw_menu_blocks ( b );
    const bool several = ( ids.size ( ) > 1 );
    /* analýza surového bloku hned za Set speed */
    if ( cw_menu_item ( _L ( "Analyze..." ), "ctx_analyze", NULL, cw_reason_analyze ( v, b ) ) ) cmt_analyze_dialog_open ( v, b->id );
    /* převod bloku na jiný profil zapisovače za Analyze..., před skupinou úprav */
    if ( cw_menu_item ( _L ( "Convert to profile..." ), "ctx_convert", NULL, cw_reason_convert ( v, b, several ) ) )
        cmt_convert_dialog_open ( v, b->id );

    /* úpravy pásky za Convert to profile... (SPEC: za Set speed, mezi ně přibyly Analyze... a Convert to profile...): Move up, Move down, Import file here..., čára, Delete */
    ImGui::Separator ( );
    const char *r_edit = cw_reason_edit ( v );
    const char *r_up = cw_reason_move ( v, b, true, several );
    const char *r_down = cw_reason_move ( v, b, false, several );
    if ( cw_menu_item ( _L ( "Move up" ), "ctx_move_up", "Alt+Up", r_up ) ) ( void ) imgui_cmt_tape_move_block ( v->tape_serial, b->id, b->id - 1 );
    if ( cw_menu_item ( _L ( "Move down" ), "ctx_move_down", "Alt+Down", r_down ) )
        ( void ) imgui_cmt_tape_move_block ( v->tape_serial, b->id, b->id + 1 );
    if ( cw_menu_item ( _L ( "Import file here..." ), "ctx_import_here", NULL, r_edit ) ) imgui_cmt_tape_import ( b->id, v->tape_serial );
    ImGui::Separator ( );
    std::string del_label;
    if ( several ) {
        char buf[64];
        snprintf ( buf, sizeof ( buf ), _ ( "Delete %d blocks" ), ( int ) ids.size ( ) );
        del_label = buf;
    } else {
        del_label = _ ( "Delete" );
    }
    /* stabilní ID položky: text se mění s počtem vybraných bloků */
    del_label += "###cw_ctx_delete";
    if ( cw_menu_item ( del_label.c_str ( ), "ctx_delete", "Del", r_edit ) ) cw_request_delete ( v, ids );

    ImGui::Separator ( );
    clicked = ImGui::MenuItem ( _L ( "Properties" ) );
    cw_record ( "ctx_properties", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
    if ( clicked ) {
        /* vlastnosti tohoto řádku: vybere jen jeho (při více vybraných by panel ukázal jen počet) */
        cw_sel_single ( b->id, v->tape_serial );
        s_cw4.range_reset = true;
        s_cw.props_open = 1u;
    }
}

/**
 * @brief Text buňky zarovnaný vpravo (čísla monospace), zaznamenaný pro testy.
 * @param name Jméno prvku (jen testy). @param text Text. @param color Barva.
 */
static void cw_cell_right ( const char *name, const std::string &text, ImU32 color )
{
    float tw = cw_text_w ( text );
    float avail = ImGui::GetContentRegionAvail ( ).x;
    if ( avail > tw ) ImGui::SetCursorPosX ( ImGui::GetCursorPosX ( ) + avail - tw );
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    ImGui::TextUnformatted ( text.c_str ( ), text.c_str ( ) + text.size ( ) );
    ImGui::PopStyleColor ( );
    cw_record ( name, text.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_SCROLL, CW_ANCHOR_LEFT, false );
}

/**
 * @brief Text buňky zarovnaný vlevo, oříznutý elipsou na šířku buňky, zaznamenaný pro testy.
 * @param name Jméno prvku (jen testy). @param text Text. @param color Barva.
 * @return true = text se zkrátil (volající může dát tooltip s celým textem).
 */
static bool cw_cell_left ( const char *name, const std::string &text, ImU32 color )
{
    bool elided = false;
    std::string shown = cw_ellipsize_end ( text, ImGui::GetContentRegionAvail ( ).x, &elided );
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    ImGui::TextUnformatted ( shown.c_str ( ), shown.c_str ( ) + shown.size ( ) );
    ImGui::PopStyleColor ( );
    cw_record ( name, shown.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_SCROLL, CW_ANCHOR_LEFT, false );
    return elided;
}

/** @brief Jméno prvku buňky pro testy ("name_3"); v aplikaci se neformátuje. */
#ifdef MZTEST_HEADLESS
#define CW_CELL_NAME( buf, col, id ) ( snprintf ( buf, sizeof ( buf ), "%s_%d", col, id ), buf )
#else
#define CW_CELL_NAME( buf, col, id ) ( ( void ) ( id ), col )
#endif

/**
 * @brief Jeden řádek bloku v tabulce (jen viditelné řádky z ImGuiListClipper).
 *
 * Výběr řádků je stav okna (vícenásobný výběr s_cw4.sel, řádek vlastností
 * s_cw.sel_id), ne pozice pásky; klik vybere jeden řádek, Ctrl+klik přidá
 * nebo ubere, Shift+klik rozsah (BeginMultiSelect() v cw_draw_table()),
 * dvojklik přehraje řádek pod kurzorem (cw_play_from()), Ctrl+Enter na řádku
 * s fokusem navigace skočí na blok (REPLAY_CMT_SEEK); samotný Enter nic,
 * šipky jen přesouvají fokus navigace ImGui a s ním výběr (D-G3-16). Pravé
 * tlačítko vybere nevybraný řádek a otevře kontextové menu. Řádek je zdroj
 * tažení (payload CW_DND_BLOCK = st_CW_DND_ROW: index bloku a čítač výměn
 * pásky ze začátku tažení), jen když úpravy jdou
 * (cw_reason_edit()); vybraný řádek má pruh accent u levé hrany (stav ne jen barvou);
 * řádek obnoveného výběru (s_cw4.focus_row) dostane
 * fokus navigace a posune se do pohledu. Horní okraj prvního kresleného
 * řádku se zapamatuje pro místo vložení (cw_gap_at()).
 *
 * Poslední řádek tabulky je o 1 px vyšší (pod spodním okrajem rámečku):
 * obsah tabulky je pak hlavička + celé řádky + 1 px jako rámeček
 * (cw_table_frame_h()), takže i konec posunu leží na celém řádku.
 *
 * @param v Kopie. @param m Rozměry. @param b Blok.
 * @param scroll_here true = posunout tabulku tak, aby byl řádek uprostřed (po výměně pásky; zarovná se na celé řádky).
 * @param last true = poslední řádek tabulky.
 */
static void cw_table_row ( const st_CMT_VIEW *v, const st_CW_METRICS *m, const st_CMT_VIEW_BLOCK *b, bool scroll_here, bool last )
{
    char nb[32];
    bool selected = cw_sel_has ( b->id );
    bool control = ( b->kind == CMT_VIEW_KIND_CONTROL );
    ImU32 text_col = control ? ( selected ? CMT_TOK_TEXT_DIM_SELECTED : CMT_TOK_TEXT_DIM ) : CMT_TOK_TEXT;

    ImGui::TableNextRow ( ImGuiTableRowFlags_None, cw_row_h ( m ) + ( last ? 1.0f : 0.0f ) );
    ImGui::PushID ( b->id );
    ImGui::TableSetColumnIndex ( CW_COL_MARK );
    ImVec2 cell = ImGui::GetCursorScreenPos ( );
    /* geometrie řádků pro místo vložení (tažení řádku, soubor z OS): horní okraj = obsah buňky - vnitřní okraj */
    float row_y0 = cell.y - ImGui::GetStyle ( ).CellPadding.y;
    if ( !s_cw4.rows_any ) {
        s_cw4.rows_any = true;
        s_cw4.row_ref_id = b->id;
        s_cw4.row_ref_y0 = row_y0;
        s_cw4.row_min_id = s_cw4.row_max_id = b->id;
    }
    s_cw4.row_min_id = ImMin ( s_cw4.row_min_id, b->id );
    s_cw4.row_max_id = ImMax ( s_cw4.row_max_id, b->id );
    /* vícenásobný výběr (BeginMultiSelect v cw_draw_table()): index bloku jako data výběru */
    ImGui::SetNextItemSelectionUserData ( ( ImGuiSelectionUserData ) b->id );
    bool pressed = ImGui::Selectable ( "##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                                       ImVec2 ( 0.0f, m->Lh ) );
    cw_record ( CW_CELL_NAME ( nb, "row", b->id ), NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_SCROLL_CONTAINER,
                CW_ANCHOR_LEFT, false );
    ImGuiID row_item = ImGui::GetItemID ( );
    /* vybraný řádek: kromě výplně svislý pruh accent u levé hrany (2 px x měřítko), stav ne jen barvou */
    if ( selected ) {
        /* ořez pozadí řádků tabulky (buňka sloupce značky by pruh u levé hrany uřízla) */
        ImRect clip = ImGui::GetCurrentTable ( )->BgClipRect;
        ImVec2 r0 = ImGui::GetItemRectMin ( ), r1 = ImGui::GetItemRectMax ( );
        float bw = ImMax ( 1.0f, floorf ( 0.5f * m->u + 0.5f ) );
        float bx = ImMax ( r0.x, clip.Min.x );
        ImVec2 bmin ( bx, r0.y ), bmax ( bx + bw, r1.y );
        ImDrawList *rdl = ImGui::GetWindowDrawList ( );
        rdl->PushClipRect ( clip.Min, clip.Max, false );
        rdl->AddRectFilled ( bmin, bmax, CMT_TOK_ACCENT );
        rdl->PopClipRect ( );
        cw_record_clip ( CW_CELL_NAME ( nb, "selbar", b->id ), bmin, bmax, clip, CW_KIND_SCROLL_CONTAINER );
    }
#ifdef MZTEST_HEADLESS
    /* testy: ImGui kreslí rámeček fokusu navigace kolem tohoto řádku (Selectable ve vícenásobném výběru
     * ho kreslí vždy, ImGuiNavRenderCursorFlags_AlwaysDraw, i bez viditelného kurzoru navigace) */
    if ( ImGui::GetFocusID ( ) == row_item && !ImGui::GetCurrentWindow ( )->DC.NavHideHighlightOneFrame )
        cw_record_clip ( CW_CELL_NAME ( nb, "navframe", b->id ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), ImGui::GetCurrentWindow ( )->ClipRect,
                         CW_KIND_SCROLL_CONTAINER );
#endif
    if ( scroll_here ) ImGui::SetScrollHereY ( 0.5f );
    if ( pressed ) {
        s_cw.sel_id = b->id;
        s_cw.sel_tape_serial = v->tape_serial;
    }
    /* výběr obnovený po úpravě: fokus navigace na řádek (Alt+Up / Alt+Down, Del pokračují) a posun, aby byl vidět */
    if ( b->id == s_cw4.focus_row ) {
        s_cw4.focus_row = -1;
        if ( ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) ) {
            ImGui::SetFocusID ( row_item, ImGui::GetCurrentWindow ( ) );
            s_cw.nav_last = row_item;
        }
        ImGui::ScrollToItem ( ImGuiScrollFlags_KeepVisibleEdgeY );
    }
    /* tažení řádku = přesun jednoho bloku (jen když úpravy jdou; Escape nebo změna pásky tažení zruší do
     * puštění myši). Payload nese čítač výměn pásky ze začátku tažení; popisek u kurzoru
     * kreslí cw_draw_drag_label() (neprůhledný, vpravo pod kurzorem). */
    if ( !s_cw4.drag_cancel && cw_reason_edit ( v ) == NULL
         && ImGui::BeginDragDropSource ( ImGuiDragDropFlags_SourceNoHoldToOpenOthers | ImGuiDragDropFlags_PayloadAutoExpire
                                         | ImGuiDragDropFlags_SourceNoPreviewTooltip ) ) {
        if ( s_cw4.drag_from != b->id ) s_cw4.drag_serial = v->tape_serial;
        st_CW_DND_ROW d;
        d.from = b->id;
        d.tape_serial = s_cw4.drag_serial;
        ImGui::SetDragDropPayload ( CW_DND_BLOCK, &d, sizeof ( d ) );
        s_cw4.drag_from = b->id;
        ImGui::EndDragDropSource ( );
    }
    /* Ctrl+Enter na řádku s fokusem navigace v zaostřeném okně kazety = skok na blok.
     * Samotný Enter nic: klik myší dá řádku fokus navigace a Enter napsaný pak
     * (např. na výzvu programu) by skočil na blok hrající pásky (D-G3-16, SPEC K11). */
    if ( ImGui::IsItemFocused ( ) && ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows )
         && ( ImGui::IsKeyChordPressed ( ImGuiMod_Ctrl | ImGuiKey_Enter ) || ImGui::IsKeyChordPressed ( ImGuiMod_Ctrl | ImGuiKey_KeypadEnter ) )
         && cw_reason_jump_block ( v, b ) == NULL )
        cw_block_op ( v, REPLAY_CMT_SEEK, b->id, 0u );
    if ( ImGui::IsItemHovered ( ) && ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) && cw_reason_play_block ( v, b ) == NULL )
        cw_play_from ( v, b->id );
    /* šipky: výběr následuje fokus navigace (jen nově zaostřený řádek) */
    if ( ImGui::IsItemFocused ( ) && row_item != s_cw.nav_last ) {
        s_cw.nav_last = row_item;
        s_cw.sel_id = b->id;
        s_cw.sel_tape_serial = v->tape_serial;
    }
    if ( ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
        s_cw.sel_id = b->id;
        s_cw.sel_tape_serial = v->tape_serial;
    }
    ImGuiID ctx_id = ImGui::GetID ( "##ctx" );
    if ( ImGui::BeginPopupContextItem ( "##ctx" ) ) {
        /* páska, ke které menu patří (jiná páska = menu se zavře, cw_row_menu()) */
        if ( ImGui::IsWindowAppearing ( ) ) s_cw.menu_tape_serial = v->tape_serial;
        /* řádek menu: kdyby se přestal kreslit, zavře menu cw_close_orphan_menu() */
        s_cw.menu_popup_id = ctx_id;
        s_cw.menu_block = b->id;
        s_cw.menu_frame = ImGui::GetFrameCount ( );
        cw_row_menu ( v, b );
        ImGui::EndPopup ( );
    }
    /* značka pozice pásky (ne výběr): trojúhelník v barvě accent */
    if ( b->id == v->current_block && v->state != CMT_VIEW_STATE_RECORD ) {
        float s = m->Lh * 0.36f;
        ImVec2 a ( cell.x + 1.0f, floorf ( cell.y + m->Lh * 0.5f - s ) );
        ImGui::GetWindowDrawList ( )->AddTriangleFilled ( a, ImVec2 ( a.x, a.y + 2.0f * s ), ImVec2 ( a.x + 1.4f * s, a.y + s ),
                                                          selected ? CMT_TOK_TEXT : CMT_TOK_ACCENT );
        cw_record ( "mark", NULL, a, ImVec2 ( a.x + 1.4f * s, a.y + 2.0f * s ), CW_KIND_SCROLL, CW_ANCHOR_LEFT, false );
    }

    char buf[48];
    if ( ImGui::TableSetColumnIndex ( CW_COL_NUM ) ) {
        snprintf ( buf, sizeof ( buf ), "%d", b->id + 1 );
        ImGui::PushFont ( m->mono, 0.0f );
        cw_cell_right ( CW_CELL_NAME ( nb, "num", b->id ), buf, text_col );
        ImGui::PopFont ( );
    }
    if ( ImGui::TableSetColumnIndex ( CW_COL_NAME ) ) {
        bool empty = false;
        std::string name = cw_block_display_name ( b, &empty );
        if ( cw_cell_left ( CW_CELL_NAME ( nb, "name", b->id ), name, empty ? CMT_TOK_TEXT_DIM : text_col ) ) cw_tooltip ( name.c_str ( ), NULL, NULL );
    }
    if ( ImGui::TableSetColumnIndex ( CW_COL_TYPE ) ) cw_cell_left ( CW_CELL_NAME ( nb, "type", b->id ), b->type_text, text_col );
    ImGui::PushFont ( m->mono, 0.0f );
    if ( ImGui::TableSetColumnIndex ( CW_COL_SIZE ) ) {
        std::string t = b->has_mzf ? cw_thousands ( ( uint32_t ) b->fsize ) + " B" : std::string ( "-" );
        cw_cell_right ( CW_CELL_NAME ( nb, "size", b->id ), t, text_col );
    }
    if ( ImGui::TableSetColumnIndex ( CW_COL_LOAD ) ) {
        if ( b->has_mzf ) snprintf ( buf, sizeof ( buf ), "%04Xh", ( unsigned ) b->fstrt & 0xFFFFu );
        else snprintf ( buf, sizeof ( buf ), "-" );
        cw_cell_right ( CW_CELL_NAME ( nb, "load", b->id ), buf, text_col );
    }
    if ( ImGui::TableSetColumnIndex ( CW_COL_EXEC ) ) {
        if ( b->has_mzf ) snprintf ( buf, sizeof ( buf ), "%04Xh", ( unsigned ) b->fexec & 0xFFFFu );
        else snprintf ( buf, sizeof ( buf ), "-" );
        cw_cell_right ( CW_CELL_NAME ( nb, "exec", b->id ), buf, text_col );
    }
    ImGui::PopFont ( );
    if ( ImGui::TableSetColumnIndex ( CW_COL_SPEED ) ) cw_cell_left ( CW_CELL_NAME ( nb, "speed", b->id ), cw_speed_cell ( b ), text_col );
    if ( ImGui::TableSetColumnIndex ( CW_COL_LENGTH ) ) {
        ImGui::PushFont ( m->mono, 0.0f );
        /* řídicí blok nemá signál: délka prázdná */
        cw_cell_right ( CW_CELL_NAME ( nb, "len", b->id ), control ? std::string ( ) : cw_len_text ( b->length_s ), text_col );
        ImGui::PopFont ( );
    }
    if ( ImGui::TableSetColumnIndex ( CW_COL_STATUS ) ) {
        if ( cw_cell_left ( CW_CELL_NAME ( nb, "status", b->id ), b->status_text, cw_status_color ( b, selected ) ) )
            cw_tooltip ( b->status_text.c_str ( ), NULL, NULL );
    }
    ImGui::PopID ( );
}

/**
 * @brief Dočasný řádek "Recording..." na konci tabulky při záznamu (SPEC v2, K26).
 *
 * Je vždy poslední, proto o 1 px vyšší (viz cw_table_row()).
 *
 * @param v Kopie. @param m Rozměry.
 */
static void cw_table_rec_row ( const st_CMT_VIEW *v, const st_CW_METRICS *m )
{
    ImGui::TableNextRow ( ImGuiTableRowFlags_None, cw_row_h ( m ) + 1.0f );
    if ( ImGui::TableSetColumnIndex ( CW_COL_MARK ) ) {
        ImVec2 c = ImGui::GetCursorScreenPos ( );
        ImGui::Dummy ( ImVec2 ( s_cw_tc.w[CW_COL_MARK], m->Lh ) );
        ImGui::GetWindowDrawList ( )->AddCircleFilled ( ImVec2 ( c.x + s_cw_tc.w[CW_COL_MARK] * 0.5f, c.y + m->Lh * 0.5f ), m->Lh * 0.2f, CMT_TOK_REC );
        cw_record ( "rec_mark", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_SCROLL, CW_ANCHOR_LEFT, false );
    }
    char buf[32];
    if ( ImGui::TableSetColumnIndex ( CW_COL_NUM ) ) {
        snprintf ( buf, sizeof ( buf ), "%d", v->block_count + 1 );
        ImGui::PushFont ( m->mono, 0.0f );
        cw_cell_right ( "rec_num", buf, CMT_TOK_TEXT );
        ImGui::PopFont ( );
    }
    if ( ImGui::TableSetColumnIndex ( CW_COL_NAME ) ) cw_cell_left ( "rec_name", _ ( "Recording..." ), CMT_TOK_REC );
    if ( ImGui::TableSetColumnIndex ( CW_COL_LENGTH ) ) {
        ImGui::PushFont ( m->mono, 0.0f );
        cw_cell_right ( "rec_len", cw_len_text ( v->rec_time_s ), CMT_TOK_TEXT );
        ImGui::PopFont ( );
    }
}

/**
 * @brief Zavře osiřelé kontextové menu řádku.
 *
 * Menu řádku se kreslí jen s jeho řádkem (BeginPopupContextItem() v
 * cw_table_row()). Zmizí-li řádek (kratší páska) nebo se páska vymění,
 * zatímco řádek není v tomto snímku kreslený, zůstalo by menu v zásobníku
 * popupů ImGui a mohlo by se ukázat nad jiným blokem, až by se řádek se
 * stejným indexem znovu kreslil. Menu otevřené v minulém snímku, které se
 * v tomto snímku nekreslilo a jehož blok už neexistuje nebo patří jiné
 * pásce, se zavře se stejným varováním jako v cw_row_menu().
 *
 * @param v Kopie.
 * @pre Uvnitř tabulky bloků, po kreslení řádků (clipper).
 * @post s_cw.menu_popup_id == 0, pokud se menu zavřelo nebo už otevřené není.
 */
static void cw_close_orphan_menu ( const st_CMT_VIEW *v )
{
    if ( !s_cw.menu_popup_id || s_cw.menu_frame == ImGui::GetFrameCount ( ) ) return;
    if ( !ImGui::IsPopupOpen ( s_cw.menu_popup_id, ImGuiPopupFlags_None ) ) {
        s_cw.menu_popup_id = 0;
        return;
    }
    if ( s_cw.menu_block < v->block_count && v->tape_serial == s_cw.menu_tape_serial ) return;
    ImGuiContext &g = *ImGui::GetCurrentContext ( );
    for ( int n = 0; n < g.OpenPopupStack.Size; n++ ) {
        if ( g.OpenPopupStack[n].PopupId != s_cw.menu_popup_id ) continue;
        ImGui::ClosePopupToLevel ( n, true );
        break;
    }
    s_cw.menu_popup_id = 0;
    cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
}

/**
 * @brief Místo vložení (mezera mezi řádky) pod svislou polohou: 0 = před první blok, počet bloků = na konec.
 *
 * Z geometrie řádků kreslených v tomto snímku (stejná výška všech řádků):
 * nejbližší hranice řádků k poloze omezené na tělo tabulky. Bez kreslených
 * řádků (prázdná páska) konec pásky.
 *
 * @param y Svislá poloha (souřadnice obrazovky).
 * @return Mezera 0 .. s_cw4.row_count.
 * @pre Po kreslení řádků tabulky v tomto snímku (s_cw4.body, row_*).
 */
static int cw_gap_at ( float y )
{
    if ( !s_cw4.rows_any || s_cw4.row_h <= 0.0f ) return s_cw4.row_count;
    y = ImClamp ( y, s_cw4.body.Min.y, s_cw4.body.Max.y );
    int g = s_cw4.row_ref_id + ( int ) floorf ( ( y - s_cw4.row_ref_y0 ) / s_cw4.row_h + 0.5f );
    return ImClamp ( g, 0, s_cw4.row_count );
}

/**
 * @brief Automatický posun tabulky při tažení řádku k hornímu nebo dolnímu okraji těla.
 *
 * Zóna je řádek vysoká u každého okraje uvnitř těla tabulky a stejně
 * vysoká vně (nad horní hranou leží hlavička). Rychlost roste s hloubkou
 * v zóně lineárně do CW_AUTOSCROLL_ROWS_PER_S řádků za sekundu (hloubka
 * = vzdálenost od vnitřní hrany zóny / výška řádku, nejvýš 1). Posouvá se
 * po celých řádcích (nastřádané zlomky v s_cw4.scroll_acc), takže tabulka
 * dál ukazuje jen celé řádky. Mimo zónu nebo bez posunu
 * (začátek / konec) se střádání nuluje.
 *
 * @param mouse Poloha myši (souřadnice obrazovky).
 * @pre Uvnitř tabulky (aktuální okno = vnitřní okno tabulky), probíhá vlastní tažení řádku.
 */
static void cw_table_autoscroll ( ImVec2 mouse )
{
    const float rh = s_cw4.row_h;
    const ImRect &b = s_cw4.body;
    if ( rh <= 0.0f || mouse.x < b.Min.x || mouse.x > b.Max.x ) {
        s_cw4.scroll_acc = 0.0f;
        return;
    }
    float depth = 0.0f;
    if ( mouse.y >= b.Min.y - rh && mouse.y < b.Min.y + rh ) depth = -ImMin ( ( b.Min.y + rh - mouse.y ) / rh, 1.0f );
    else if ( mouse.y > b.Max.y - rh && mouse.y <= b.Max.y + rh ) depth = ImMin ( ( mouse.y - ( b.Max.y - rh ) ) / rh, 1.0f );
    ImGuiWindow *w = ImGui::GetCurrentWindow ( );
    if ( depth == 0.0f || ( depth < 0.0f && w->Scroll.y <= 0.0f ) || ( depth > 0.0f && w->Scroll.y >= w->ScrollMax.y ) ) {
        s_cw4.scroll_acc = 0.0f;
        return;
    }
    s_cw4.scroll_acc += depth * CW_AUTOSCROLL_ROWS_PER_S * ImGui::GetIO ( ).DeltaTime;
    float rows = ( s_cw4.scroll_acc > 0.0f ) ? floorf ( s_cw4.scroll_acc ) : ceilf ( s_cw4.scroll_acc );
    if ( rows == 0.0f ) return;
    s_cw4.scroll_acc -= rows;
    ImGui::SetScrollY ( ImClamp ( w->Scroll.y + rows * rh, 0.0f, w->ScrollMax.y ) );
}

/**
 * @brief Cíl tažení řádku a čára místa vložení: tažení řádku myší nebo soubor z OS nad tabulkou.
 *
 * Tažení řádku (payload CW_DND_BLOCK, st_CW_DND_ROW): nad tělem tabulky
 * mezera pod kurzorem; puštění = přesun z bloku "from" na pozici "to" =
 * mezera, nebo mezera - 1, leží-li pod taženým blokem (move(4, 1) = blok 4
 * nad řádek 1). Puštění na vlastní místo nic nepošle. Escape tažení zruší
 * (do puštění tlačítka se nic nepřesune ani nezačne znovu). Mimo STOP a při
 * záznamu a přehrávání vstupů cíl nic nepřijme (cw_reason_edit()). Puštění
 * nad jinou páskou, než ze které tažení začalo (čítač v payloadu), nic
 * nepřesune a ohlásí varování (tažení zruší už
 * cw_draw_table() při změně čítače). U okraje těla se tabulka posouvá
 * (cw_table_autoscroll()); rámeček fokusu navigace během tažení skryje
 * cw_draw_table() (tažený řádek by vypadal jako čára místa vložení).
 *
 * Soubor z OS: během přetažení (SDL_EVENT_DROP_POSITION, nejvýš 1 s od
 * poslední polohy) nad plochou tabulky okna pod kurzorem a s povoleným
 * importem mezera pod kurzorem; nad hlavičkou tabulky konec pásky (čára
 * pod posledním řádkem, stejně jako import při puštění).
 *
 * @param v Kopie.
 * @return Mezera, nad kterou se má kreslit čára (0 .. počet bloků), -1 = žádná.
 * @pre Uvnitř tabulky po kreslení řádků; s_cw4.body je tělo tabulky tohoto snímku.
 */
static int cw_table_drop_target ( const st_CMT_VIEW *v )
{
    const ImGuiPayload *pl = ImGui::GetDragDropPayload ( );
    bool ours = pl && pl->IsDataType ( CW_DND_BLOCK );
    if ( !ours ) {
        s_cw4.drag_from = -1;
        s_cw4.scroll_acc = 0.0f;
    }
    if ( ours && ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) ) s_cw4.drag_cancel = true;
    int gap = -1;
    if ( ours && !s_cw4.drag_cancel && cw_reason_edit ( v ) == NULL ) {
        cw_table_autoscroll ( ImGui::GetIO ( ).MousePos );
    }
    /* test polohy myši cíle ořezává aktuální ořez (zde buňka posledního sloupce): ořez celého těla */
    ImGui::PushClipRect ( s_cw4.body.Min, s_cw4.body.Max, false );
    if ( ours && !s_cw4.drag_cancel && cw_reason_edit ( v ) == NULL
         && ImGui::BeginDragDropTargetCustom ( s_cw4.body, ImGui::GetID ( "##cw_drop" ) ) ) {
        const ImGuiPayload *p = ImGui::AcceptDragDropPayload ( CW_DND_BLOCK, ImGuiDragDropFlags_AcceptBeforeDelivery
                                                                | ImGuiDragDropFlags_AcceptNoDrawDefaultRect );
        if ( p && p->Data && p->DataSize == ( int ) sizeof ( st_CW_DND_ROW ) ) {
            st_CW_DND_ROW d;
            memcpy ( &d, p->Data, sizeof ( d ) );
            int g = cw_gap_at ( ImGui::GetIO ( ).MousePos.y );
            if ( p->IsDelivery ( ) ) {
                int to = ( g > d.from ) ? g - 1 : g;
                if ( d.tape_serial != v->tape_serial ) {
                    cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
                } else if ( d.from >= 0 && d.from < v->block_count && to >= 0 && to < v->block_count && to != d.from ) {
                    ( void ) imgui_cmt_tape_move_block ( d.tape_serial, d.from, to );
                }
                s_cw4.drag_from = -1;
            } else {
                gap = g;
            }
        }
        ImGui::EndDragDropTarget ( );
    }
    ImGui::PopClipRect ( );
    if ( gap < 0 && s_cw4.os_active && s_cw4.os_pos_valid && ( ImGui::GetTime ( ) - s_cw4.os_pos_time ) < 1.0
         && cw_reason_import ( v ) == NULL && s_cw4.table_area.Contains ( s_cw4.os_pos )
         && ImGui::IsWindowHovered ( ImGuiHoveredFlags_RootAndChildWindows ) )
        gap = ( s_cw4.os_pos.y < s_cw4.body.Min.y ) ? s_cw4.row_count : cw_gap_at ( s_cw4.os_pos.y );
    return gap;
}

/**
 * @brief Zařadí přesun bloku o jedno místo z klávesy Alt+Up / Alt+Down (s_cw4.move_seq pro další stisk).
 * @param v Kopie. @param id Blok. @param dir -1 = nahoru, +1 = dolů.
 * @return true = zásah zařazen.
 * @pre Úpravy povolené, 0 <= id + dir < v->block_count.
 */
static bool cw_move_key_submit ( const st_CMT_VIEW *v, int id, int dir )
{
    uint32_t seq = imgui_cmt_tape_move_block ( v->tape_serial, id, id + dir );
    s_cw4.move_seq = seq;
    s_cw4.move_idle_frame = -1;
    return seq != 0;
}

/**
 * @brief Klávesy výběru při fokusu tabulky: Del smaže výběr, Alt+Up / Alt+Down přesune jediný vybraný blok.
 *
 * Fokus tabulky = fokus navigace ve vícenásobném výběru (stejná podmínka
 * jako Ctrl+A) v zaostřeném okně kazety bez otevřeného popupu. Při držení
 * Alt si okno vezme šipky Up / Down (SetKeyOwner()), aby Alt+šipka
 * nepohnula navigací a s ní výběrem; vlastnictví platí od dalšího snímku,
 * proto se Alt zmáčkne dřív než šipka (obvyklé pořadí kláves).
 *
 * Během tažení řádku se klávesy ignorují. Zakázaná úprava
 * ukáže důvod jako varování v řádku zpráv (stejný text jako tooltip
 * položky menu). Alt+šipka stisknutá dřív, než přišel
 * výsledek předchozího přesunu, se zapamatuje (nejvýš jedna) a provede se
 * nad přesunutým blokem po obnově výběru (cw_move_queue_apply()).
 *
 * @param v Kopie.
 * @pre Uvnitř tabulky po EndMultiSelect() (s_cw4.table_focus tohoto snímku).
 */
static void cw_table_keys ( const st_CMT_VIEW *v )
{
    if ( !s_cw4.table_focus || !ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) ) return;
    if ( ImGui::IsPopupOpen ( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) ) return;
    const ImGuiIO &io = ImGui::GetIO ( );
    const bool alt_only = io.KeyAlt && !io.KeyCtrl && !io.KeyShift && !io.KeySuper;
    if ( alt_only ) {
        ImGui::SetKeyOwner ( ImGuiKey_UpArrow, s_cw4.keys_owner );
        ImGui::SetKeyOwner ( ImGuiKey_DownArrow, s_cw4.keys_owner );
    }
    if ( cw_row_dragging ( ) ) return;
    const bool del = ImGui::IsKeyChordPressed ( ImGuiKey_Delete );
    int dir = 0;
    if ( alt_only && ImGui::IsKeyPressed ( ImGuiKey_UpArrow, ImGuiInputFlags_None, s_cw4.keys_owner ) ) dir = -1;
    else if ( alt_only && ImGui::IsKeyPressed ( ImGuiKey_DownArrow, ImGuiInputFlags_None, s_cw4.keys_owner ) ) dir = +1;
    if ( !del && dir == 0 ) return;
    const char *r = cw_reason_edit ( v );
    if ( r ) {
        cmt_view_post ( r, CMT_VIEW_MSG_WARNING );
        return;
    }
    if ( del ) {
        std::vector<int> ids;
        for ( int id : cw_sel_list ( ) )
            if ( id < v->block_count ) ids.push_back ( id );
        cw_request_delete ( v, ids );
    }
    if ( dir == 0 ) return;
    /* předchozí přesun ještě nemá výsledek (výběr se obnoví až po něm): zapamatovat, nejvýš jeden */
    if ( s_cw4.move_seq != 0 ) {
        s_cw4.move_queued = dir;
        return;
    }
    if ( s_cw4.sel.Size == 0 ) return;
    const st_CMT_VIEW_BLOCK *b = ( s_cw.sel_id >= 0 && s_cw.sel_id < v->block_count ) ? &v->blocks[( size_t ) s_cw.sel_id] : NULL;
    if ( !b ) return;
    const char *rm = cw_reason_move ( v, b, dir < 0, s_cw4.sel.Size > 1 );
    if ( rm ) {
        cmt_view_post ( rm, CMT_VIEW_MSG_WARNING );
        return;
    }
    ( void ) cw_move_key_submit ( v, b->id, dir );
}

/**
 * @brief Čára místa vložení: vodorovná čára v barvě accent přes celé tělo tabulky, tloušťka 2 px násobená měřítkem (0,5u).
 *
 * Leží na hranici řádků mezery @p gap. Ořez je tělo tabulky rozšířené
 * nahoru o polovinu tloušťky a 1 px (dolní čára hlavičky): čára nad prvním
 * řádkem tak leží na hranici jako ostatní a nepřekryje text řádku; u okraje
 * ořezu se posune dovnitř, aby ji ořez neuřízl.
 *
 * @param inner Vnitřní okno tabulky (kreslí se do jeho DrawListu nad řádky).
 * @param m Rozměry. @param gap Mezera 0 .. počet bloků.
 * @pre Po EndTable() téhož snímku (s_cw4.body, row_*).
 */
static void cw_draw_insert_line ( ImGuiWindow *inner, const st_CW_METRICS *m, int gap )
{
    if ( !s_cw4.rows_any ) return;
    float t = 0.5f * m->u;
    ImRect clip = s_cw4.body;
    clip.Min.y -= 0.5f * t + 1.0f;
    if ( clip.GetHeight ( ) < t || clip.GetWidth ( ) <= 0.0f ) return;
    float y = s_cw4.row_ref_y0 + ( float ) ( gap - s_cw4.row_ref_id ) * s_cw4.row_h;
    y = ImClamp ( y, clip.Min.y + 0.5f * t, clip.Max.y - 0.5f * t );
    ImVec2 a ( clip.Min.x, y - 0.5f * t );
    ImVec2 b ( clip.Max.x, y + 0.5f * t );
    inner->DrawList->PushClipRect ( clip.Min, clip.Max, false );
    inner->DrawList->AddRectFilled ( a, b, CMT_TOK_ACCENT );
    inner->DrawList->PopClipRect ( );
    cw_record_clip ( "drop_line", a, b, clip, CW_KIND_OTHER );
}

/**
 * @brief Modál Delete blocks: potvrzení smazání výběru s neuloženým blokem.
 *
 * Text podle výběru: jsou-li neuložené všechny vybrané bloky,
 * "Delete N unsaved blocks? Their data is not stored anywhere else."
 * (jeden: "Delete this unsaved block? Its data is not stored anywhere
 * else."); smíšený výběr "Delete blocks: N. Unsaved: M. Data of the unsaved
 * blocks is not stored anywhere else." (bez tvarů množného čísla).
 * Tlačítka Delete blocks a Cancel; výchozí (Enter a počáteční fokus
 * navigace) je Cancel, Escape = Cancel. Fokus navigace je vidět hned po
 * otevření i myší (SetNavCursorVisible()), takže je
 * vidět, kam jde Enter. Modál se otevírá uprostřed okna kazety a v jeho
 * viewportu (imgui_cmt_window_modal_next_pos()); nad i pod čarou je
 * mezera stylu. Vymění-li se páska nebo se upraví (čítač výměn pásky
 * jiný než při žádosti), modál se zavře s varováním "The tape changed; the
 * action was cancelled." a nic se nesmaže. Delete blocks
 * ověří povolení úprav znovu (deck mezitím mimo STOP = varování "The tape
 * state changed; the action was cancelled.") a zařadí
 * imgui_cmt_tape_remove_blocks().
 *
 * @param v Kopie.
 * @pre Uvnitř okna kazety (ID popupu v kontextu okna).
 */
static void cw_delete_modal ( const st_CMT_VIEW *v )
{
    if ( !s_cw4.del_pending && !s_cw4.del_open ) return;
    if ( !v->has_tape || v->tape_serial != s_cw4.del_tape_serial ) {
        if ( s_cw4.del_open && ImGui::IsPopupOpen ( CW_DELETE_POPUP_ID ) ) {
            ImGuiContext &g = *ImGui::GetCurrentContext ( );
            ImGuiID pid = ImGui::GetID ( CW_DELETE_POPUP_ID );
            for ( int n = 0; n < g.OpenPopupStack.Size; n++ ) {
                if ( g.OpenPopupStack[n].PopupId != pid ) continue;
                ImGui::ClosePopupToLevel ( n, true );
                break;
            }
        }
        s_cw4.del_pending = s_cw4.del_open = false;
        s_cw4.del_ids.clear ( );
        cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
        return;
    }
    if ( s_cw4.del_pending ) {
        ImGui::OpenPopup ( CW_DELETE_POPUP_ID );
        s_cw4.del_pending = false;
        s_cw4.del_open = true;
    }
    std::string title = std::string ( _ ( "Delete blocks" ) ) + CW_DELETE_POPUP_ID;
    imgui_cmt_window_modal_next_pos ( );
    if ( !ImGui::BeginPopupModal ( title.c_str ( ), NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        /* modál zavřel někdo jiný: jako Cancel */
        s_cw4.del_open = false;
        s_cw4.del_ids.clear ( );
        return;
    }
    const bool appearing = ImGui::IsWindowAppearing ( );
    char text[192];
    const int total = ( int ) s_cw4.del_ids.size ( );
    if ( s_cw4.del_unsaved < total )
        snprintf ( text, sizeof ( text ), _ ( "Delete blocks: %d. Unsaved: %d. Data of the unsaved blocks is not stored anywhere else." ), total,
                   s_cw4.del_unsaved );
    else if ( s_cw4.del_unsaved == 1 )
        snprintf ( text, sizeof ( text ), "%s", _ ( "Delete this unsaved block? Its data is not stored anywhere else." ) );
    else
        snprintf ( text, sizeof ( text ), _ ( "Delete %d unsaved blocks? Their data is not stored anywhere else." ), s_cw4.del_unsaved );
    /* zalomení na šířku úměrnou písmu (texty o 40 % delší i 175 % se vejdou na obrazovku) */
    ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + 22.0f * ImGui::GetFontSize ( ) );
    ImGui::TextWrapped ( "%s", text );
    ImGui::PopTextWrapPos ( );
    cw_record ( "del_text", text, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
    /* rytmus mezer: text - čára - tlačítka stejně jako tlačítka - hrana okna */
    ImGui::Spacing ( );
    ImGui::Separator ( );
    ImGui::Spacing ( );
    const char *l_del = _L ( "Delete blocks" );
    const char *l_cancel = _L ( "Cancel" );
    bool do_delete = ImGui::Button ( l_del );
    ImGuiID del_id = ImGui::GetItemID ( );
    {
        std::string shown ( l_del, ImGui::FindRenderedTextEnd ( l_del ) );
        cw_record ( "del_confirm", shown.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
    }
    ImGui::SameLine ( );
    bool do_cancel = ImGui::Button ( l_cancel );
    ImGuiID cancel_id = ImGui::GetItemID ( );
    /* výchozí tlačítko je Cancel (SPEC): počáteční fokus navigace */
    ImGui::SetItemDefaultFocus ( );
    /* fokus navigace je vidět i po otevření myší: uživatel vidí, kam jde Enter */
    ImGui::SetNavCursorVisible ( true );
    {
        std::string shown ( l_cancel, ImGui::FindRenderedTextEnd ( l_cancel ) );
        cw_record ( "del_cancel", shown.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
#ifdef MZTEST_HEADLESS
        ImGuiContext &g = *ImGui::GetCurrentContext ( );
        /* testy: zvýrazněný fokus navigace na Cancel (jméno prvku, text = co má fokus) */
        const char *focus = ( !g.NavCursorVisible ) ? "none" : ( ImGui::GetFocusID ( ) == cancel_id ? "cancel"
                                                                : ( ImGui::GetFocusID ( ) == del_id ? "delete" : "other" ) );
        cw_record ( "del_focus", focus, ImVec2 ( 0.0f, 0.0f ), ImVec2 ( 0.0f, 0.0f ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
#endif
    }
    ( void ) cancel_id;
    /* Escape = Cancel; Enter = Cancel, pokud fokus navigace nestojí na Delete blocks
     * (Enter ze snímku, kdy se modál otevřel, se nepočítá) */
    if ( ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) ) do_cancel = true;
    if ( !appearing && ( ImGui::IsKeyPressed ( ImGuiKey_Enter, false ) || ImGui::IsKeyPressed ( ImGuiKey_KeypadEnter, false ) )
         && ImGui::GetFocusID ( ) != del_id )
        do_cancel = true;
    if ( do_cancel || do_delete ) {
        ImGui::CloseCurrentPopup ( );
        s_cw4.del_open = false;
        if ( do_delete && !do_cancel ) {
            if ( cw_reason_edit ( v ) != NULL ) cmt_view_post ( _ ( "The tape state changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
            else ( void ) imgui_cmt_tape_remove_blocks ( s_cw4.del_tape_serial, s_cw4.del_ids.data ( ), ( int ) s_cw4.del_ids.size ( ) );
        }
        s_cw4.del_ids.clear ( );
    }
    ImGui::EndPopup ( );
}

/**
 * @brief Vyhodnotí soubory přetažené z OS (po SDL_EVENT_DROP_COMPLETE) podle místa puštění.
 *
 * Nad tělem nebo plochou tabulky bloků (rozvržení Full): import všech
 * souborů postupně na místo pod kurzorem (pod posledním řádkem a nad
 * hlavičkou tabulky na konec, stejně jako ukazuje čára místa vložení; bez
 * pásky do nové pásky s čítačem 0). Nad jinou částí okna Virtual CMT (i nad
 * sbaleným oknem): otevření prvního souboru obvyklým postupem (dotazy na
 * nahrávání a neuložené změny). Žádné přetažení se nezahodí mlčky:
 * víc souborů nad deckem = otevře se první a varování "Only one
 * file can be opened as a tape."; běží-li jiný dotaz automatu pásky nebo je
 * nad oknem otevřený popup (modál i obyčejný: kontextové menu řádku, menu
 * Tape, menu sloupců) = varování "Finish the open dialog
 * first.";
 * zakázaný import / otevření (nahrávání, záznam a přehrávání vstupů) =
 * varování s důvodem. Puštění mimo okno kazety (jiné okno nad kurzorem)
 * nic. Přetažení starší než 2 snímky (okno se mezitím nekreslilo) se zahodí.
 *
 * @param v Kopie.
 * @pre Uvnitř okna kazety po kreslení tabulky (geometrie tohoto snímku).
 * @post s_cw4.os_ready == false.
 */
static void cw_os_drop_process ( const st_CMT_VIEW *v )
{
    if ( !s_cw4.os_ready ) return;
    s_cw4.os_ready = false;
    std::vector<std::string> files;
    files.swap ( s_cw4.os_files );
    if ( files.empty ( ) || ( ImGui::GetFrameCount ( ) - s_cw4.os_ready_frame ) > 2 ) return;
    ImVec2 p = s_cw4.os_file_pos;
    /* nad oknem kazety je otevřený popup (modál, kontextové menu řádku, menu Tape, menu
     * sloupců; vlastní nebo jiného okna): přetažení se nepoužije, ale nezahodí se mlčky.
     * Menu z lišty okna nechává okno "hovered", proto se popup
     * zkoumá před dotazem na hover. */
    ImGuiContext &g = *GImGui;
    if ( g.OpenPopupStack.Size > 0 ) {
        ImGuiWindow *popup = ImGui::GetTopMostPopupModal ( );
        if ( !popup ) popup = g.OpenPopupStack.back ( ).Window; /* NULL = popup ještě bez okna */
        if ( ImGui::GetCurrentWindow ( )->Rect ( ).Contains ( p ) || ( popup && popup->Rect ( ).Contains ( p ) ) )
            cmt_view_post ( _ ( "Finish the open dialog first." ), CMT_VIEW_MSG_WARNING );
        return;
    }
    if ( !ImGui::IsWindowHovered ( ImGuiHoveredFlags_RootAndChildWindows ) ) return;
    bool over_table = ( s_cw.layout == CMT_WINDOW_LAYOUT_FULL ) && ( s_cw4.table_frame == ImGui::GetFrameCount ( ) )
        && s_cw4.table_area.Contains ( p );
    if ( over_table ) {
        const char *r = cw_reason_import ( v );
        if ( r ) {
            cmt_view_post ( r, CMT_VIEW_MSG_WARNING );
            return;
        }
        std::vector<const char *> paths;
        for ( const std::string &f : files ) paths.push_back ( f.c_str ( ) );
        int at = v->has_tape ? ( ( p.y < s_cw4.body.Min.y ) ? v->block_count : cw_gap_at ( p.y ) ) : -1;
        imgui_cmt_tape_import_paths ( paths.data ( ), ( int ) paths.size ( ), at, cw_import_serial ( v ) );
        return;
    }
    const char *r = cw_reason_open ( v );
    if ( r ) {
        cmt_view_post ( r, CMT_VIEW_MSG_WARNING );
        return;
    }
    if ( !imgui_cmt_tape_open_path ( files[0].c_str ( ) ) ) {
        cmt_view_post ( _ ( "Finish the open dialog first." ), CMT_VIEW_MSG_WARNING );
        return;
    }
    if ( files.size ( ) > 1 ) cmt_view_post ( _ ( "Only one file can be opened as a tape." ), CMT_VIEW_MSG_WARNING );
}

/**
 * @brief Zalomí text po slovech na řádky nejvýš @p max_w px (aktuálním písmem).
 *
 * Slovo delší než @p max_w se zkrátí elipsou na konci (cw_ellipsize_end()).
 * Mezery se zachovají jen mezi slovy jednoho řádku.
 *
 * @param text Text (UTF-8). @param max_w Šířka řádku v px.
 * @return Řádky (aspoň jeden, i pro prázdný text).
 */
static std::vector<std::string> cw_wrap_words ( const std::string &text, float max_w )
{
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while ( i < text.size ( ) ) {
        while ( i < text.size ( ) && text[i] == ' ' ) i++;
        size_t j = i;
        while ( j < text.size ( ) && text[j] != ' ' ) j++;
        if ( j == i ) break;
        std::string word = text.substr ( i, j - i );
        i = j;
        std::string cand = cur.empty ( ) ? word : cur + " " + word;
        if ( cw_text_w ( cand ) <= max_w ) {
            cur = cand;
            continue;
        }
        if ( !cur.empty ( ) ) lines.push_back ( cur );
        bool elided = false;
        cur = cw_ellipsize_end ( word, max_w, &elided );
    }
    lines.push_back ( cur );
    return lines;
}

/**
 * @brief Prázdný stav tabulky: dvě věty SPEC zalomené na šířku plochy pod hlavičkou, bez čar sloupců.
 *
 * Plocha má barvu panelu a rámeček; texty se kreslí pod vlastním ořezem
 * plochy (záznam pro testy nese ten ořez). Řádky jsou vodorovně
 * i svisle uprostřed; nevejdou-li se všechny na výšku, poslední viditelný
 * řádek dostane elipsu a tooltip s celým textem. Záznam: první řádek věty
 * "empty_1" / "empty_2", další "empty_2_1", "empty_2_2", ...
 *
 * @param m Rozměry. @param pmin Levý horní roh plochy. @param pmax Pravý dolní roh plochy.
 */
static void cw_draw_empty_state ( const st_CW_METRICS *m, ImVec2 pmin, ImVec2 pmax )
{
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    dl->AddRectFilled ( pmin, pmax, CMT_TOK_PANEL );
    dl->AddRect ( pmin, pmax, CMT_TOK_BORDER );
    cw_record ( "empty_area", NULL, pmin, pmax, CW_KIND_OTHER_CONTAINER, CW_ANCHOR_LEFT, false );
    const char *sentences[2] = { _ ( "This tape is empty." ), _ ( "Press Record to save a program from the computer, or open a tape file." ) };
    const char *names[2] = { "empty_1", "empty_2" };
    float inner_w = ( pmax.x - pmin.x ) - 4.0f * m->pad;
    float inner_h = ( pmax.y - pmin.y ) - 2.0f * m->u;
    if ( inner_w < m->Lh || inner_h < m->Lh ) return;
    struct st_LINE {
        std::string text;
        int sentence;
        int index;
    };
    std::vector<st_LINE> lines;
    for ( int s = 0; s < 2; s++ ) {
        std::vector<std::string> w = cw_wrap_words ( sentences[s], inner_w );
        for ( size_t k = 0; k < w.size ( ); k++ ) lines.push_back ( { w[k], s, ( int ) k } );
    }
    float pitch = m->Lh + m->u;
    int fit = ( int ) floorf ( ( inner_h + m->u ) / pitch );
    bool cut = ( int ) lines.size ( ) > fit;
    if ( cut ) {
        /* poslední viditelný řádek: zbytek textu s elipsou na konci */
        std::string rest;
        for ( size_t k = ( size_t ) ImMax ( fit - 1, 0 ); k < lines.size ( ); k++ ) rest += ( rest.empty ( ) ? "" : " " ) + lines[k].text;
        lines.resize ( ( size_t ) ImMax ( fit, 0 ) );
        if ( !lines.empty ( ) ) {
            bool elided = false;
            lines.back ( ).text = cw_ellipsize_end ( rest, inner_w, &elided );
        }
    }
    float total = ( float ) lines.size ( ) * pitch - m->u;
    float y = floorf ( pmin.y + ( ( pmax.y - pmin.y ) - total ) * 0.5f );
    ImGui::PushClipRect ( ImVec2 ( pmin.x + 1.0f, pmin.y + 1.0f ), ImVec2 ( pmax.x - 1.0f, pmax.y - 1.0f ), true );
    for ( const st_LINE &l : lines ) {
        char nm[32];
        if ( l.index == 0 ) snprintf ( nm, sizeof ( nm ), "%s", names[l.sentence] );
        else snprintf ( nm, sizeof ( nm ), "%s_%d", names[l.sentence], l.index );
        cw_text_at_kind ( nm, ImVec2 ( floorf ( pmin.x + ( ( pmax.x - pmin.x ) - cw_text_w ( l.text ) ) * 0.5f ), y ), l.text, CMT_TOK_TEXT_DIM, CW_KIND_OTHER );
        if ( cut ) cw_tooltip ( sentences[1], NULL, NULL );
        y += pitch;
    }
    ImGui::PopClipRect ( );
}

/**
 * @brief Popisek taženého bloku u kurzoru: číslo a jméno bloku, neprůhledný, vpravo pod kurzorem.
 *
 * Kreslí se do popředí viewportu okna (nad tabulkou i popupy) místo
 * tooltipu zdroje ImGui (ten má krytí 60 % a leží přímo na řádcích u čáry
 * místa vložení). Pozadí barvou panelu, hrana CMT_TOK_CONTROL_BORDER; levý
 * horní roh je o 0,75 výšky písma vpravo a o 1,5 řádku níž než kurzor, takže
 * popisek nezakrývá řádky sousedící s čárou (ta leží nejvýš půl řádku od
 * kurzoru). U okraje viewportu se popisek posune dovnitř. Text "N  JMÉNO"
 * s číslem bloku od 1 jako sloupec # (dva bloky stejného jména se rozliší).
 *
 * @param v Kopie. @param m Rozměry.
 * @pre Po EndTable() téhož snímku; probíhá tažení řádku (s_cw4.drag_from platí pro kopii).
 */
static void cw_draw_drag_label ( const st_CMT_VIEW *v, const st_CW_METRICS *m )
{
    const ImGuiPayload *pl = ImGui::GetDragDropPayload ( );
    if ( !pl || !pl->IsDataType ( CW_DND_BLOCK ) || s_cw4.drag_cancel ) return;
    if ( s_cw4.drag_from < 0 || s_cw4.drag_from >= v->block_count ) return;
    const st_CMT_VIEW_BLOCK *b = &v->blocks[( size_t ) s_cw4.drag_from];
    bool dim = false;
    char num[16];
    snprintf ( num, sizeof ( num ), "%d  ", b->id + 1 );
    std::string text = std::string ( num ) + cw_block_display_name ( b, &dim );
    ImGuiViewport *vp = ImGui::GetWindowViewport ( );
    ImVec2 ts = ImGui::CalcTextSize ( text.c_str ( ), text.c_str ( ) + text.size ( ), false );
    ImVec2 mouse = ImGui::GetIO ( ).MousePos;
    float pad = m->pad;
    ImVec2 size ( ts.x + 2.0f * pad, ts.y + 2.0f * m->u );
    ImVec2 pmin ( floorf ( mouse.x + 0.75f * m->Lh ), floorf ( mouse.y + 1.5f * cw_row_h ( m ) ) );
    ImVec2 vmax ( vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y );
    pmin.x = ImMax ( vp->Pos.x, ImMin ( pmin.x, vmax.x - size.x ) );
    pmin.y = ImMax ( vp->Pos.y, ImMin ( pmin.y, vmax.y - size.y ) );
    ImVec2 pmax ( pmin.x + size.x, pmin.y + size.y );
    ImDrawList *dl = ImGui::GetForegroundDrawList ( vp );
    dl->AddRectFilled ( pmin, pmax, CMT_TOK_PANEL );
    dl->AddRect ( pmin, pmax, CMT_TOK_CONTROL_BORDER );
    ImVec2 tp ( pmin.x + pad, pmin.y + m->u );
    dl->AddText ( tp, dim ? CMT_TOK_TEXT_DIM : CMT_TOK_TEXT, text.c_str ( ), text.c_str ( ) + text.size ( ) );
#ifdef MZTEST_HEADLESS
    cw_record_clip ( "drag_label", pmin, pmax, ImRect ( vp->Pos, vmax ), CW_KIND_POPUP );
    s_cw_items.back ( ).text = text;
#endif
}

/**
 * @brief Tabulka bloků: zamrzlá hlavička, sloupce podle SPEC v2, ImGuiListClipper, prázdný stav.
 *
 * Šířky a viditelnost sloupců ukládá ImGui do svého INI (ID tabulky
 * CW_TABLE_ID v okně ###virtual_cmt). Size, Load, Exec a Speed jsou výchozí
 * skryté (DefaultHide); přepínají se hlavičkou (pravé tlačítko) nebo menu
 * View > Columns (s_cw.col_toggle).
 *
 * Tělo tabulky má celý počet řádků (volající dává výšku z cw_table_frame_h())
 * a posun se drží na celých řádcích (cw_scroll_snap_target(),
 * cw_scroll_snap_after()), takže žádný řádek není vidět napůl.
 * Prázdná páska: tabulka jen s hlavičkou a pod ní plocha s textem
 * (cw_draw_empty_state()).
 *
 * Řádky jsou v jednom vícenásobném výběru (BeginMultiSelect()
 * před hlavičkou, výsledky do s_cw4.sel); po řádcích cíl tažení a čára
 * místa vložení (cw_table_drop_target(), cw_draw_insert_line() po
 * EndTable()), klávesy Del a Alt+Up / Alt+Down (cw_table_keys()).
 * Zapamatuje plochu a tělo tabulky a geometrii řádků tohoto snímku pro
 * soubor přetažený z OS (cw_os_drop_process()). Změní-li se během tažení
 * řádku čítač výměn pásky, tažení zruší s varováním; po
 * EndTable() kreslí popisek taženého bloku (cw_draw_drag_label()).
 *
 * @param v Kopie. @param m Rozměry. @param pos Levý horní roh. @param size Velikost tabulky (u prázdné pásky i s plochou textu).
 * @post s_cw4.table_frame = tento snímek (byla-li tabulka otevřená), výběr srovnaný (cw_sel_fix_primary()).
 */
static void cw_draw_table ( const st_CMT_VIEW *v, const st_CW_METRICS *m, ImVec2 pos, ImVec2 size )
{
    bool empty = v->has_tape && v->block_count == 0 && v->state != CMT_VIEW_STATE_RECORD;
    ImVec2 full_size = size;
    if ( empty ) size.y = ImMin ( size.y, cw_table_frame_h ( m, 0 ) );
    float rh = cw_row_h ( m );
    ImGuiWindow *inner = NULL;
    int line_gap = -1;
    /* Escape zrušil tažení řádku: platí do puštění tlačítka myši */
    if ( !ImGui::IsMouseDown ( ImGuiMouseButton_Left ) ) s_cw4.drag_cancel = false;
    /* páska se během tažení změnila (vlastní klávesa, MCP, jiná páska): tažení se zruší, nic se nepřesune
     * (zdroj tažení se už nezačne, payload vyprší) */
    if ( s_cw4.drag_from >= 0 && !s_cw4.drag_cancel && s_cw4.drag_serial != v->tape_serial ) {
        s_cw4.drag_cancel = true;
        s_cw4.drag_from = -1;
        cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
    }
    s_cw4.rows_any = false;
    /* posun z minulého snímku (kolečko, navigace, posun na aktuální blok) na celé řádky */
    if ( s_cw.table_id ) {
        ImGuiTable *pt = ImGui::TableFindByID ( s_cw.table_id );
        if ( pt ) cw_scroll_snap_target ( pt->InnerWindow, rh );
    }
    ImGui::SetCursorScreenPos ( pos );
    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable
        | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter;
    ImGui::PushStyleColor ( ImGuiCol_TableHeaderBg, CMT_TOK_TABLE_HEADER );
    ImGui::PushStyleColor ( ImGuiCol_TableRowBg, CMT_TOK_PANEL );
    ImGui::PushStyleColor ( ImGuiCol_TableRowBgAlt, CMT_TOK_ROW_ALT );
    ImGui::PushStyleColor ( ImGuiCol_TableBorderLight, CMT_TOK_BORDER );
    ImGui::PushStyleColor ( ImGuiCol_TableBorderStrong, CMT_TOK_BORDER );
    ImGui::PushStyleColor ( ImGuiCol_Header, CMT_TOK_ROW_SELECTED );
    ImGui::PushStyleColor ( ImGuiCol_HeaderHovered, CMT_TOK_CONTROL );
    ImGui::PushStyleColor ( ImGuiCol_HeaderActive, CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_ChildBg, CMT_TOK_PANEL );
    ImGui::PushStyleVar ( ImGuiStyleVar_CellPadding, ImVec2 ( 2.0f * m->u, 0.5f * m->u ) );
    bool open = ImGui::BeginTable ( CW_TABLE_ID, CW_COL_COUNT, flags, size );
    if ( open ) {
        s_cw.table_id = ImGui::GetCurrentTable ( )->ID;
        const float *w = s_cw_tc.w;
        ImGuiTableColumnFlags fix = ImGuiTableColumnFlags_WidthFixed;
        ImGuiTableColumnFlags hide = ImGuiTableColumnFlags_DefaultHide;
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_MARK ), fix | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoResize
                                  | ImGuiTableColumnFlags_NoReorder, w[CW_COL_MARK] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_NUM ), fix, w[CW_COL_NUM] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_NAME ), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 1.0f );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_TYPE ), fix, w[CW_COL_TYPE] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_SIZE ), fix | hide, w[CW_COL_SIZE] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_LOAD ), fix | hide, w[CW_COL_LOAD] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_EXEC ), fix | hide, w[CW_COL_EXEC] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_SPEED ), fix | hide, w[CW_COL_SPEED] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_LENGTH ), fix, w[CW_COL_LENGTH] );
        ImGui::TableSetupColumn ( cw_col_label ( CW_COL_STATUS ), fix, w[CW_COL_STATUS] );
        ImGui::TableSetupScrollFreeze ( 0, 1 );
        if ( s_cw.reset_widths ) {
            /* View > Reset column widths: šířky pevných sloupců podle obsahu (jinak jen počáteční šířka výše) */
            ImGuiTable *t = ImGui::GetCurrentTable ( );
            for ( int c = CW_COL_NUM; c < CW_COL_COUNT; c++ ) {
                if ( c == CW_COL_NAME ) continue;
                t->Columns[c].WidthRequest = w[c];
            }
            t->IsSettingsDirty = true;
            s_cw.reset_widths = false;
        }
        /* menu View > Columns (zapíná sloupec v tomto snímku) */
        if ( s_cw.col_toggle >= 0 && s_cw.col_toggle < CW_COL_COUNT ) {
            bool en = ( ImGui::TableGetColumnFlags ( s_cw.col_toggle ) & ImGuiTableColumnFlags_IsEnabled ) != 0;
            ImGui::TableSetColumnEnabled ( s_cw.col_toggle, !en );
            s_cw.col_toggle = -1;
        }
        /* menu View > Columns > Show all */
        if ( s_cw.col_show_all ) {
            for ( int c = CW_COL_NUM; c < CW_COL_COUNT; c++ ) {
                if ( c != CW_COL_NAME ) ImGui::TableSetColumnEnabled ( c, true );
            }
            s_cw.col_show_all = false;
        }

        /* vícenásobný výběr: klik, Ctrl+klik, Shift+klik, Ctrl+A při fokusu tabulky.
         * Začíná před hlavičkou: BeginMultiSelect() uvnitř buňky by ukončil buňku a přepsal
         * měření řádku (CursorMaxPos), tělo tabulky by se posunulo o řádek níž. */
        int rows = v->block_count;
        ImGuiMultiSelectIO *ms = ImGui::BeginMultiSelect ( ImGuiMultiSelectFlags_None, s_cw4.sel.Size, rows );
        /* během tažení řádku bez rámečku fokusu navigace: tažený řádek by vypadal jako čára místa vložení */
        if ( cw_row_dragging ( ) ) {
            ImGui::GetCurrentWindow ( )->DC.NavHideHighlightOneFrame = true;
        }
        s_cw4.sel.ApplyRequests ( ms );
        {
            ImGuiContext &g = *ImGui::GetCurrentContext ( );
            s_cw4.table_focus = g.CurrentMultiSelect && g.CurrentMultiSelect->IsFocused;
        }

        /* hlavička: TableHeader() kvůli záznamu obdélníků (pravé tlačítko = menu sloupců ImGui) */
        ImGui::TableNextRow ( ImGuiTableRowFlags_Headers, cw_row_h ( m ) );
        for ( int c = 0; c < CW_COL_COUNT; c++ ) {
            if ( !ImGui::TableSetColumnIndex ( c ) ) continue;
            ImGui::TableHeader ( cw_col_label ( c ) );
#ifdef MZTEST_HEADLESS
            char nm[32];
            snprintf ( nm, sizeof ( nm ), "hdr_%d", c );
            const char *lbl = cw_col_label ( c );
            std::string shown ( lbl, ImGui::FindRenderedTextEnd ( lbl ) );
            /* obdélník textu hlavičky (svisle uprostřed buňky), ne celé buňky */
            ImVec2 imin = ImGui::GetItemRectMin ( ), imax = ImGui::GetItemRectMax ( );
            ImVec2 tsz = ImGui::CalcTextSize ( lbl, NULL, true );
            ImVec2 tmin ( imin.x, floorf ( ( imin.y + imax.y - tsz.y ) * 0.5f ) );
            cw_record ( nm, shown.c_str ( ), tmin, ImVec2 ( tmin.x + tsz.x, tmin.y + tsz.y ), CW_KIND_OTHER, CW_ANCHOR_LEFT, false );
#endif
        }

        bool rec = v->has_tape && v->state == CMT_VIEW_STATE_RECORD;
        /* po výměně pásky posun na aktuální blok (výběr se nevnucuje); záznam: posun na řádek Recording... */
        int scroll_row = -1;
        if ( s_cw.scroll_current ) {
            if ( !rec && v->current_block >= 0 && v->current_block < rows ) scroll_row = v->current_block;
            s_cw.scroll_current = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin ( rows, cw_row_h ( m ) );
        if ( scroll_row >= 0 ) clipper.IncludeItemByIndex ( scroll_row );
        /* kotva Shift+klik, tažený řádek a řádek obnoveného výběru se kreslí vždy (jinak by výběr, tažení nebo posun zanikly) */
        if ( ms->RangeSrcItem >= 0 && ms->RangeSrcItem < rows ) clipper.IncludeItemByIndex ( ( int ) ms->RangeSrcItem );
        if ( s_cw4.drag_from >= 0 && s_cw4.drag_from < rows ) clipper.IncludeItemByIndex ( s_cw4.drag_from );
        if ( s_cw4.focus_row >= 0 && s_cw4.focus_row < rows ) clipper.IncludeItemByIndex ( s_cw4.focus_row );
        while ( clipper.Step ( ) ) {
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++ )
                cw_table_row ( v, m, &v->blocks[( size_t ) i], i == scroll_row, !rec && i == rows - 1 );
        }
        clipper.End ( );
        if ( s_cw4.range_reset ) {
            /* výběr nastavil program: stará kotva Shift+klik neplatí */
            ms->RangeSrcReset = true;
            s_cw4.range_reset = false;
        }
        ms = ImGui::EndMultiSelect ( );
        s_cw4.sel.ApplyRequests ( ms );
        cw_sel_fix_primary ( );
        s_cw.sel_tape_serial = v->tape_serial;
        cw_close_orphan_menu ( v );
        if ( rec ) {
            cw_table_rec_row ( v, m );
            if ( !s_cw.rec_scrolled ) ImGui::SetScrollHereY ( 1.0f );
            s_cw.rec_scrolled = true;
        } else {
            s_cw.rec_scrolled = false;
        }
        /* tělo tabulky (pod hlavičkou a její čarou, bez posuvníku), místo vložení a klávesy výběru */
        inner = ImGui::GetCurrentTable ( )->InnerWindow;
        s_cw4.body = ImRect ( inner->InnerClipRect.Min.x, ImMax ( inner->InnerClipRect.Min.y, pos.y + rh + 1.0f ), inner->InnerClipRect.Max.x,
                              inner->InnerClipRect.Max.y );
        s_cw4.row_h = rh;
        s_cw4.row_count = rows;
        line_gap = cw_table_drop_target ( v );
        cw_table_keys ( v );
        /* posun tažením posuvníku: po puštění na celý řádek */
        cw_scroll_snap_after ( ImGui::GetCurrentWindow ( ), rh );
        for ( int c = 0; c < CW_COL_COUNT; c++ ) s_cw.col_enabled[c] = ( ImGui::TableGetColumnFlags ( c ) & ImGuiTableColumnFlags_IsEnabled ) != 0;
#ifdef MZTEST_HEADLESS
        ImGuiTable *t = ImGui::GetCurrentTable ( );
        s_cw_table_scroll_x = t->InnerWindow && t->InnerWindow->ScrollbarX;
#endif
        ImGui::EndTable ( );
    }
    ImGui::PopStyleVar ( );
    ImGui::PopStyleColor ( 9 );
    cw_record ( "table", NULL, pos, ImVec2 ( pos.x + size.x, pos.y + size.y ), CW_KIND_OTHER_CONTAINER, CW_ANCHOR_LEFT, false );
    /* obdélník tabulky pro umístění okna Tape check mimo ni (imgui_cmt_window_place_beside()) */
    s_cw_table_min = pos;
    s_cw_table_max = ImVec2 ( pos.x + size.x, pos.y + size.y );
    s_cw_table_frame = ImGui::GetFrameCount ( );
    /* čára místa vložení až po EndTable(): nad řádky i čarami sloupců vnitřního okna tabulky */
    if ( inner && line_gap >= 0 ) cw_draw_insert_line ( inner, m, line_gap );
    /* číslo a jméno taženého bloku vpravo pod kurzorem */
    cw_draw_drag_label ( v, m );
    s_cw4.table_area = ImRect ( pos, ImVec2 ( pos.x + full_size.x, pos.y + full_size.y ) );
    s_cw4.table_frame = open ? ImGui::GetFrameCount ( ) : -1;

    /* prázdná páska: dvě věty pod hlavičkou, mimo tělo tabulky (bez čar sloupců, SPEC) */
    if ( empty && full_size.y > size.y + 1.0f )
        cw_draw_empty_state ( m, ImVec2 ( pos.x, pos.y + size.y - 1.0f ), ImVec2 ( pos.x + full_size.x, pos.y + full_size.y ) );
}


/* ------------------------------------------------------------------------- */
/*  Vlastnosti bloku (rozvržení Full)                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Jedna vlastnost: popisek a hodnota.
 * @invariant label je přeložený statický text; value vlastní svůj text.
 */
typedef struct st_CW_PROP {
    const char *label; /**< popisek (přeložený) */
    std::string value; /**< hodnota */
    ImU32 color;       /**< barva hodnoty */
} st_CW_PROP;

/**
 * @brief Text původu bloku (SPEC: Imported from MZF / Recorded / From tape file; Imported u neuloženého importovaného bloku).
 *
 * "Recorded" má kontext překladu "cmt" (blok nahraný na pásku); stejný
 * anglický text jinde znamená zapsaný záznam zvuku.
 *
 * @param s Původ bloku. @return Přeložený text (statický řetězec gettext).
 */
static const char *cw_source_text ( en_CMT_VIEW_SOURCE s )
{
    switch ( s ) {
        case CMT_VIEW_SOURCE_IMPORTED_MZF: return _ ( "Imported from MZF" );
        case CMT_VIEW_SOURCE_RECORDED: return C_ ( "cmt", "Recorded" );
        case CMT_VIEW_SOURCE_IMPORTED: return C_ ( "cmt", "Imported" );
        case CMT_VIEW_SOURCE_TAPE_FILE:
        default: return _ ( "From tape file" );
    }
}

/**
 * @brief Obsah sekce vlastností podle druhu bloku (SPEC v2, K10), bez výběru vlastnosti pásky.
 *
 * MZF: Type, Size, Load address, Exec address, Speed, Long pulse (µs),
 * Short pulse (µs), Length, Source. Surový záznam: Type, Length, Source,
 * Sample rate, Pulse count. ZX TAP a jiné bloky TZX: Type, Speed, Length,
 * Source. Řídicí blok: Type, Description, Source. Blok, který player
 * neumí: navíc Status. Pásky: File, Blocks, Total length, Format; při
 * záznamu Blocks počítá i nahrávaný blok (jako deck "Block n / n").
 * Checksum se neukazuje (jádro kontrolní součet neurčuje, D-G3-4).
 *
 * @param v Kopie. @param b Vybraný blok, NULL = vlastnosti pásky.
 * @param[out] out Vlastnosti. @param[out] title Nadpis sekce. @param[out] subtitle Ztlumený podtitul.
 */
static void cw_props_build ( const st_CMT_VIEW *v, const st_CMT_VIEW_BLOCK *b, std::vector<st_CW_PROP> *out, std::string *title,
                             std::string *subtitle )
{
    out->clear ( );
    char buf[64];
    if ( !b ) {
        *title = _ ( "Tape properties" );
        std::string name = !v->has_tape ? std::string ( _ ( "No tape" ) )
            : ( v->tape_name.empty ( ) ? std::string ( _ ( "(new tape)" ) ) : v->tape_name );
        *subtitle = name;
        if ( !v->has_tape ) return;
        out->push_back ( { _ ( "File" ), v->tape_path.empty ( ) ? name : v->tape_path, CMT_TOK_TEXT } );
        /* při záznamu se počítá i nahrávaný blok, jako "Block n / n" v decku a řádek
         * Recording... v tabulce (jeden význam počtu) */
        bool rec = ( v->state == CMT_VIEW_STATE_RECORD );
        snprintf ( buf, sizeof ( buf ), "%d", v->block_count + ( rec ? 1 : 0 ) );
        out->push_back ( { _ ( "Blocks" ), buf, CMT_TOK_TEXT } );
        out->push_back ( { _ ( "Total length" ), cw_len_text ( v->tape_total_s ), CMT_TOK_TEXT } );
        /* formát podle přípony souboru (velkými písmeny), nová páska bez souboru "-" */
        std::string ext = "-";
        size_t dot = v->tape_name.rfind ( '.' );
        if ( dot != std::string::npos && dot + 1 < v->tape_name.size ( ) ) {
            ext = v->tape_name.substr ( dot + 1 );
            for ( char &ch : ext ) ch = g_ascii_toupper ( ch );
        }
        out->push_back ( { _ ( "Format" ), ext, CMT_TOK_TEXT } );
        return;
    }
    *title = _ ( "Block properties" );
    {
        bool dim = false;
        *subtitle = cw_block_display_name ( b, &dim );
    }
    switch ( b->kind ) {
        case CMT_VIEW_KIND_MZF:
        {
            out->push_back ( { _ ( "Type" ), b->type_text, CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Size" ), cw_thousands ( ( uint32_t ) b->fsize ) + " B", CMT_TOK_TEXT } );
            snprintf ( buf, sizeof ( buf ), "%04Xh", ( unsigned ) b->fstrt & 0xFFFFu );
            out->push_back ( { _ ( "Load address" ), buf, CMT_TOK_TEXT } );
            snprintf ( buf, sizeof ( buf ), "%04Xh", ( unsigned ) b->fexec & 0xFFFFu );
            out->push_back ( { _ ( "Exec address" ), buf, CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Speed" ), ( b->speed_kind == CMT_VIEW_SPEED_NONE ) ? std::string ( _ ( "(set in file)" ) ) : b->speed_text, CMT_TOK_TEXT } );
            if ( b->pulses_us[0] > 0.0 ) {
                snprintf ( buf, sizeof ( buf ), "%.0f / %.0f", b->pulses_us[0], b->pulses_us[1] );
                out->push_back ( { _ ( "Long pulse (µs)" ), buf, CMT_TOK_TEXT } );
                snprintf ( buf, sizeof ( buf ), "%.0f / %.0f", b->pulses_us[2], b->pulses_us[3] );
                out->push_back ( { _ ( "Short pulse (µs)" ), buf, CMT_TOK_TEXT } );
            }
            out->push_back ( { _ ( "Length" ), cw_len_text ( b->length_s ), CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Source" ), cw_source_text ( b->source ), CMT_TOK_TEXT } );
            break;
        }
        case CMT_VIEW_KIND_RAW:
            out->push_back ( { _ ( "Type" ), b->name, CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Length" ), cw_len_text ( b->length_s ), CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Source" ), cw_source_text ( b->source ), CMT_TOK_TEXT } );
            if ( b->sample_rate ) out->push_back ( { _ ( "Sample rate" ), cw_thousands ( b->sample_rate ) + " Hz", CMT_TOK_TEXT } );
            if ( b->pulse_count ) out->push_back ( { _ ( "Pulse count" ), cw_thousands ( b->pulse_count ), CMT_TOK_TEXT } );
            break;
        case CMT_VIEW_KIND_CONTROL:
            out->push_back ( { _ ( "Type" ), b->name, CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Description" ), b->status_text, CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Source" ), cw_source_text ( b->source ), CMT_TOK_TEXT } );
            break;
        case CMT_VIEW_KIND_TAP_HEADER:
        case CMT_VIEW_KIND_TAP_DATA:
        case CMT_VIEW_KIND_TZX:
        default:
            out->push_back ( { _ ( "Type" ), ( b->type_text.empty ( ) || b->type_text == "-" ) ? b->name : b->type_text, CMT_TOK_TEXT } );
            if ( b->speed_kind != CMT_VIEW_SPEED_NONE ) out->push_back ( { _ ( "Speed" ), b->speed_text, CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Length" ), cw_len_text ( b->length_s ), CMT_TOK_TEXT } );
            out->push_back ( { _ ( "Source" ), cw_source_text ( b->source ), CMT_TOK_TEXT } );
            break;
    }
    if ( b->status == CMT_VIEW_STATUS_UNPLAYABLE ) out->push_back ( { _ ( "Status" ), b->status_text, CMT_TOK_ERROR } );
}

/**
 * @brief Rozložení vlastností: dva sloupce, je-li dost místa, jinak jeden (šířky z textů).
 *
 * U vlastností bloku se hodnota delší než nejdelší obvyklá hodnota
 * ("8 860 800 Hz") do rozhodnutí nepočítá (zkrátí se elipsou). U vlastností
 * pásky se počítá celá (cesta souboru je hlavní údaj: dlouhá cesta =
 * jeden sloupec, D-G3-8).
 */
typedef struct st_CW_PROPS_LAYOUT {
    bool two;        /**< dva sloupce vedle sebe */
    int rows;        /**< počet řádků */
    float label_w;   /**< šířka sloupce popisků */
    float content_h; /**< výška obsahu (bez hlavičky) */
} st_CW_PROPS_LAYOUT;

/**
 * @brief Spočítá rozložení vlastností pro šířku sekce.
 * @param m Rozměry. @param props Vlastnosti. @param width Šířka sekce.
 * @param cap_values true = šířku hodnot omezit (vlastnosti bloku), false = celé hodnoty (vlastnosti pásky).
 * @param[out] l Rozložení.
 */
static void cw_props_layout ( const st_CW_METRICS *m, const std::vector<st_CW_PROP> &props, float width, bool cap_values,
                              st_CW_PROPS_LAYOUT *l )
{
    l->label_w = 0.0f;
    for ( const st_CW_PROP &p : props ) l->label_w = ImMax ( l->label_w, ImGui::CalcTextSize ( p.label ).x );
    ImGui::PushFont ( m->mono, 0.0f );
    float cap = cap_values ? ImGui::CalcTextSize ( "8 860 800 Hz" ).x : FLT_MAX;
    float vw = 0.0f;
    for ( const st_CW_PROP &p : props ) vw = ImMax ( vw, ImMin ( cw_text_w ( p.value ), cap ) );
    ImGui::PopFont ( );
    float need2 = 2.0f * ( l->label_w + 2.0f * m->gap + vw ) + 4.0f * m->gap + 2.0f * m->pad;
    int n = ( int ) props.size ( );
    l->two = ( width >= need2 ) && n > 1;
    l->rows = l->two ? ( n + 1 ) / 2 : n;
    l->content_h = ( l->rows > 0 ) ? ( cw_props_frame_h ( m, l->rows ) - m->F ) : 0.0f;
}

/** @brief ID obsahu vlastností (posouvaná oblast) z minulého snímku, 0 = dosud nekreslen (zarovnání posunu). */
static ImGuiID s_cw_props_body_id = 0;

/**
 * @brief Sekce vlastností: hlavička (klik sbalí / rozbalí, stav se pamatuje) a obsah ve dvou nebo jednom sloupci.
 *
 * Obsah je posouvaná oblast s @p rows_visible celými řádky (výška
 * cw_props_frame_h()); vejdou-li se všechny řádky, je bez posuvníku. Posun
 * se drží na celých řádcích jako u tabulky. Okraje 2u nahoře a u dole
 * leží mimo posouvanou oblast, aby při posunu nebyl vidět kousek sousedního
 * řádku.
 *
 * @param m Rozměry. @param pos Levý horní roh. @param width Šířka.
 * @param rows_visible Viditelné řádky obsahu (0 = jen hlavička; výška sekce = cw_props_frame_h()).
 * @param props Vlastnosti. @param l Rozložení. @param title Nadpis. @param subtitle Podtitul.
 * @param expandable false = hlavička bez šipky a bez přepínání (více vybraných bloků: jen "N blocks selected");
 *        řádek má stejnou výšku a text stejné odsazení jako s šipkou.
 */
static void cw_draw_props ( const st_CW_METRICS *m, ImVec2 pos, float width, int rows_visible, const std::vector<st_CW_PROP> &props,
                            const st_CW_PROPS_LAYOUT *l, const std::string &title, const std::string &subtitle, bool expandable )
{
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    float height = cw_props_frame_h ( m, ( s_cw.props_open && !props.empty ( ) ) ? rows_visible : 0 );
    ImVec2 pmax ( pos.x + width, pos.y + height );
#ifdef MZTEST_HEADLESS
    s_cw_props_scroll_y = false;
#endif
    dl->AddRectFilled ( pos, pmax, CMT_TOK_PANEL );
    dl->AddRect ( pos, pmax, CMT_TOK_BORDER );
    cw_record ( "props", NULL, pos, pmax, CW_KIND_OTHER_CONTAINER, CW_ANCHOR_LEFT, false );

    /* hlavička */
    ImGui::SetCursorScreenPos ( pos );
    bool hovered = false;
    if ( expandable ) {
        if ( ImGui::InvisibleButton ( "##cw_props_hdr", ImVec2 ( width, m->F ) ) ) s_cw.props_open = s_cw.props_open ? 0u : 1u;
        hovered = ImGui::IsItemHovered ( );
        cw_tooltip ( s_cw.props_open ? _ ( "Hide the properties" ) : _ ( "Show the properties" ), NULL, NULL );
    } else {
        /* nic k rozbalení: prostý řádek téže výšky */
        ImGui::Dummy ( ImVec2 ( width, m->F ) );
    }
    cw_record ( "props_header", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_OTHER_CONTAINER, CW_ANCHOR_LEFT, false );
    if ( hovered ) dl->AddRectFilled ( ImVec2 ( pos.x + 1.0f, pos.y + 1.0f ), ImVec2 ( pmax.x - 1.0f, pos.y + m->F ), CMT_TOK_CONTROL_HOVER );
    float ty = pos.y + ImGui::GetStyle ( ).FramePadding.y;
    float ax = pos.x + m->pad;
    if ( expandable ) {
        ImGui::RenderArrow ( dl, ImVec2 ( ax, ty ), CMT_TOK_TEXT, s_cw.props_open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f );
        cw_record ( "props_arrow", NULL, ImVec2 ( ax, ty ), ImVec2 ( ax + m->Lh * 0.7f, ty + m->Lh * 0.7f ), CW_KIND_OTHER, CW_ANCHOR_LEFT, false );
    }
    float tx = ax + m->Lh * 0.7f + m->gap;
    float tw = cw_text_at_kind ( "props_title", ImVec2 ( tx, ty ), title, CMT_TOK_TEXT, CW_KIND_OTHER );
    float sx = tx + tw + 2.0f * m->gap;
    bool elided = false;
    std::string sub = cw_ellipsize_middle ( subtitle, pos.x + width - m->pad - sx, &elided );
    if ( !sub.empty ( ) && sub != "..." ) cw_text_at_kind ( "props_subtitle", ImVec2 ( sx, ty ), sub, CMT_TOK_TEXT_DIM, CW_KIND_OTHER );

    if ( !s_cw.props_open || props.empty ( ) || rows_visible <= 0 ) return;
    float rh = cw_row_h ( m );
    dl->AddLine ( ImVec2 ( pos.x, pos.y + m->F ), ImVec2 ( pmax.x, pos.y + m->F ), CMT_TOK_BORDER );
    /* posun z minulého snímku (kolečko) na celé řádky */
    if ( s_cw_props_body_id ) cw_scroll_snap_target ( ImGui::FindWindowByID ( s_cw_props_body_id ), rh );
    ImGui::SetCursorScreenPos ( ImVec2 ( pos.x + 1.0f, pos.y + m->F + 1.0f + 2.0f * m->u ) );
    ImGui::PushStyleColor ( ImGuiCol_ChildBg, CMT_TOK_PANEL );
    bool scroll = rows_visible < l->rows;
    if ( ImGui::BeginChild ( "##cw_props_body", ImVec2 ( width - 2.0f, ( float ) rows_visible * rh ), ImGuiChildFlags_None ) ) {
        s_cw_props_body_id = ImGui::GetCurrentWindow ( )->ID;
        ImVec2 o = ImGui::GetCursorScreenPos ( );
        float inner_w = width - 2.0f - 2.0f * m->pad;
        /* obsah vyšší než sekce: svislý posuvník ubere šířku (hodnoty se zkrátí elipsou) */
        if ( scroll ) inner_w -= ImGui::GetStyle ( ).ScrollbarSize;
#ifdef MZTEST_HEADLESS
        s_cw_props_scroll_y = ImGui::GetCurrentWindow ( )->ScrollbarY;
#endif
        float col_w = l->two ? ( inner_w - 4.0f * m->gap ) * 0.5f : inner_w;
        int n = ( int ) props.size ( );
        for ( int i = 0; i < n; i++ ) {
            int col = ( l->two && i >= l->rows ) ? 1 : 0;
            int row = col ? ( i - l->rows ) : i;
            float x = o.x + m->pad + ( float ) col * ( col_w + 4.0f * m->gap );
            float y = o.y + ( float ) row * rh;
            char nm[32];
            snprintf ( nm, sizeof ( nm ), "prop_label_%d", i );
            cw_text_at_kind ( nm, ImVec2 ( x, y ), props[( size_t ) i].label, CMT_TOK_TEXT_DIM, CW_KIND_SCROLL );
            float vx = x + l->label_w + 2.0f * m->gap;
            ImGui::PushFont ( m->mono, 0.0f );
            bool el = false;
            std::string val = cw_ellipsize_middle ( props[( size_t ) i].value, x + col_w - vx, &el );
            snprintf ( nm, sizeof ( nm ), "prop_value_%d", i );
            cw_text_at_kind ( nm, ImVec2 ( vx, y ), val, props[( size_t ) i].color, CW_KIND_SCROLL );
            ImGui::PopFont ( );
            if ( el ) cw_tooltip ( props[( size_t ) i].value.c_str ( ), NULL, NULL );
        }
        if ( l->two ) {
            float sxl = floorf ( o.x + m->pad + col_w + 2.0f * m->gap ) + 0.5f;
            ImGui::GetWindowDrawList ( )->AddLine ( ImVec2 ( sxl, o.y ), ImVec2 ( sxl, o.y + ( float ) l->rows * rh - m->u ), CMT_TOK_BORDER );
        }
        /* výška obsahu = celé řádky (i s mezerou za posledním): konec posunu leží na celém řádku */
        ImGui::SetCursorScreenPos ( ImVec2 ( o.x, o.y + ( float ) l->rows * rh ) );
        ImGui::Dummy ( ImVec2 ( 1.0f, 0.0f ) );
        cw_scroll_snap_after ( ImGui::GetCurrentWindow ( ), rh );
    }
    ImGui::EndChild ( );
    ImGui::PopStyleColor ( );
}

/**
 * @brief Menu View > Columns: zapnutí a vypnutí sloupců tabulky (stejné příznaky jako hlavička tabulky).
 *
 * První položka Show all zapne všechny sloupce najednou (s_cw.col_show_all);
 * jsou-li už všechny zapnuté, je zakázaná.
 *
 * @pre Uvnitř menu View.
 */
static void cw_columns_menu ( void )
{
    bool full = ( s_cw.layout == CMT_WINDOW_LAYOUT_FULL );
    bool open = ImGui::BeginMenu ( _L ( "Columns" ), full );
    if ( !full ) cw_tooltip ( NULL, NULL, _ ( "The list of blocks is shown in the Full layout." ) );
    cw_record ( "menu_columns", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, !full );
    if ( !open ) return;
    bool all_on = true;
    for ( int c = CW_COL_NUM; c < CW_COL_COUNT; c++ ) {
        if ( c != CW_COL_NAME && !s_cw.col_enabled[c] ) all_on = false;
    }
    if ( ImGui::MenuItem ( _L ( "Show all" ), NULL, false, !all_on ) ) s_cw.col_show_all = true;
    cw_record ( "menu_col_all", NULL, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, all_on );
    ImGui::Separator ( );
    for ( int c = CW_COL_NUM; c < CW_COL_COUNT; c++ ) {
        if ( c == CW_COL_NAME ) continue;
        if ( ImGui::MenuItem ( cw_col_label ( c ), NULL, s_cw.col_enabled[c] ) ) s_cw.col_toggle = c;
#ifdef MZTEST_HEADLESS
        char nm[32];
        snprintf ( nm, sizeof ( nm ), "menu_col_%d", c );
        cw_record ( nm, s_cw.col_enabled[c] ? "on" : "off", ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
#endif
    }
    ImGui::EndMenu ( );
}


/**
 * @brief Doplní rozměry pro rozvržení Full: šířky sloupců, nejmenší tabulka a sekce vlastností.
 * @param v Kopie. @param m Rozměry (doplní table_min_w, table_min_h, props_min_h).
 */
static void cw_measure_full ( const st_CMT_VIEW *v, st_CW_METRICS *m )
{
    cw_table_widths ( v, m );
    m->table_min_w = cw_table_min_w ( );
    m->table_min_h = cw_table_frame_h ( m, CW_TABLE_MIN_ROWS );
    /* jen hlavička vlastností: obsah dostane zbytek výšky a posouvá se */
    m->props_min_h = m->F;
}


/* ------------------------------------------------------------------------- */
/*  Okno                                                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief Potvrzení editoru vlastních pulzů bloku: zásah jen nad páskou, ze které blok pochází.
 *
 * Editor je modální a otevřený libovolně dlouho; mezitím mohla být páska
 * vyměněna (MCP, záznam vstupů, návrat na checkpoint). Jiný čítač výměn
 * pásky = žádný zásah a zpráva "The tape changed; the action was
 * cancelled."; jinak cmt_request_set_block_speed_on_tape() (kontrolu
 * opakuje emu vlákno).
 *
 * @param v Kopie. @param owner Blok z openPulsesEditor(). @param pulses Potvrzené pulzy.
 */
static void cw_block_pulses_ok ( const st_CMT_VIEW *v, int owner, const st_MZTAPE_PULSES_LENGTH *pulses )
{
    if ( v->tape_serial != s_cw.pulses_tape_serial || owner < 0 || owner >= v->block_count ) {
        cmt_view_post_message ( _ ( "The tape changed; the action was cancelled." ), true );
        return;
    }
    cmt_request_set_block_speed_on_tape ( s_cw.pulses_tape_serial, owner, CMTSPEED_CUSTOM, v->blocks[( size_t ) owner].speed_is_mz, pulses );
}

/**
 * @brief Šířka pro text titulku okna podle velikosti okna z minulého snímku.
 *
 * Stejné rozložení jako ImGui::RenderWindowTitleBarContents(): vlevo
 * trojúhelník sbalení, vpravo křížek, každý FramePadding.x + výška písma
 * + 2 x ItemInnerSpacing.x (text končí nejméně 2 x ItemInnerSpacing.x,
 * při 100 % 8 px, před křížkem).
 *
 * @return Šířka v px; 0 = okno ještě neexistuje (titulek se nezkracuje).
 * @pre Uvnitř snímku ImGui, před Begin() okna.
 */
static float cw_title_avail_w ( void )
{
    ImGuiWindow *w = ImGui::FindWindowByName ( CW_WINDOW_ID );
    if ( !w ) return 0.0f;
    const ImGuiStyle &st = ImGui::GetStyle ( );
    return w->Size.x - 2.0f * ( st.FramePadding.x + ImGui::GetFontSize ( ) + 2.0f * st.ItemInnerSpacing.x );
}

/** @brief Mezera nulové šířky (U+200B v UTF-8); písmo aplikace DroidSans ji má s nulovým posunem. */
#define CW_ZERO_WIDTH_SPACE "\xE2\x80\x8B"

/**
 * @brief Viditelná část titulku okna bez "##": za každý '#' vloží mezeru nulové šířky.
 *
 * ImGui ukazuje z titulku jen text před prvním "##" a "###" bere jako
 * začátek ID; jméno pásky "a##b.tmz" by titulek useklo na "a" (i bez
 * hvězdičky Modified). Za každým '#' (i posledním před ###virtual_cmt)
 * následuje U+200B, takže viditelná část žádné "##" nemá, ID je jen
 * koncové ###virtual_cmt a titulek vypadá stejně.
 * Písmo DroidSans.ttf má pro U+200B glyf s nulovým posunem (cmap uni200B,
 * hmtx 0); jiné písmo bez tohoto glyfu by ukázalo náhradní znak.
 *
 * @param text Viditelná část titulku (UTF-8). @return Text bez dvou '#' za sebou.
 */
static std::string cw_title_escape_hash ( const std::string &text )
{
    std::string out;
    out.reserve ( text.size ( ) );
    for ( char c : text ) {
        out += c;
        if ( c == '#' ) out += CW_ZERO_WIDTH_SPACE;
    }
    return out;
}

/**
 * @brief Titulek okna: "Virtual CMT - <jméno> *###virtual_cmt" (SPEC K22).
 *
 * Nevejde-li se titulek do @p avail_w, zkrátí se jméno pásky elipsou na
 * konci (hvězdička Modified zůstane), takže titulek nikdy nekončí
 * useknutým znakem. Znaky '#' viditelné části jsou
 * oddělené mezerou nulové šířky (cw_title_escape_hash()), jméno s "##"
 * nebo "###" titulek nezkrátí ani nezmění ID.
 *
 * @param v Kopie. @param avail_w Šířka pro text titulku (cw_title_avail_w(); <= 0 = nezkracovat).
 * @return Titulek se stabilním ID (hvězdička jen u Modified, bez pásky jen "Virtual CMT").
 */
static std::string cw_title ( const st_CMT_VIEW *v, float avail_w )
{
    std::string t = _ ( "Virtual CMT" );
    if ( v->has_tape ) {
        std::string head = t + " - ";
        std::string name = v->tape_name.empty ( ) ? std::string ( _ ( "(new tape)" ) ) : v->tape_name;
        std::string tail = v->modified ? " *" : "";
        if ( avail_w > 0.0f && cw_text_w ( head + name + tail ) > avail_w ) {
            bool elided = false;
            name = cw_ellipsize_end ( name, avail_w - cw_text_w ( head ) - cw_text_w ( tail ), &elided );
        }
        t = head + name + tail;
    }
    t = cw_title_escape_hash ( t );
    t += CW_WINDOW_ID;
    return t;
}

/** @brief Převezme hodnoty z INI (rozvržení) při prvním vykreslení. */
static void cw_apply_cfg ( void )
{
    if ( s_cw.cfg_applied ) return;
    s_cw.cfg_applied = true;
    s_cw.layout = ( s_cw.cfg_layout == 1u ) ? CMT_WINDOW_LAYOUT_FULL : CMT_WINDOW_LAYOUT_COMPACT;
    s_cw.apply_size = true;
}

/**
 * @brief Položka menu s důvodem zákazu v tooltipu a záznamem pro testy.
 * @param label Popisek s ID (_L()). @param name Jméno prvku pro testy. @param shortcut Zkratka (nepřekládá se), NULL = žádná.
 * @param reason Důvod zákazu, NULL = povolená. @return true = vybrána.
 * @pre Uvnitř otevřeného menu.
 */
static bool cw_menu_item ( const char *label, const char *name, const char *shortcut, const char *reason )
{
    bool clicked = ImGui::MenuItem ( label, shortcut, false, reason == NULL );
    if ( reason ) cw_tooltip ( NULL, NULL, reason );
    std::string shown ( label, ImGui::FindRenderedTextEnd ( label ) );
    cw_record ( name, shown.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, reason != NULL );
    cw_record_reason ( reason );
    return clicked && !reason;
}

/**
 * @brief Menu Tape (SPEC): New tape, Open..., Import file..., Save, Save as..., Export block as MZF / WAV..., Export tape as WAV...,
 *        Analyze recording..., Tape information..., Check tape, Eject.
 *
 * Akce jdou přes cmt_tape_actions.cpp (dotaz na nahrávání a neuložené
 * změny, dialogy souborů, zásahy emu vlákna); zákazy stejné jako tlačítka
 * decku a zkratky (cw_actions()). Export bloku bere otevřený (aktuální)
 * blok; Import file... vkládá na konec pásky (bez pásky do nové pásky
 * s čítačem 0), Export tape as WAV... celou pásku.
 * Analyze recording... otevře dialog analýzy pro jediný vybraný řádek
 * tabulky, je-li to surový blok (cw_reason_analyze()). Tape
 * information... otevře modál informací o pásce (jen ve STOP, mimo záznam
 * a přehrávání vstupů, cw_reason_edit()), Check tape okno kontroly pásky
 * (jen s vloženou páskou, cw_reason_check()).
 *
 * @param v Kopie. @param a Akce snímku.
 * @pre Uvnitř BeginMenuBar(), drží device_state_lock() (akce čtou jádro pod ním).
 */
static void cw_tape_menu ( const st_CMT_VIEW *v, const st_CW_ACTIONS *a )
{
    bool open = ImGui::BeginMenu ( _L ( "Tape" ) );
    cw_record_menu_bar_item ( "menu_tape", open );
    if ( !open ) return;
    if ( cw_menu_item ( _L ( "New tape" ), "menu_new", NULL, a->r_new ) ) imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_NEW_TAPE );
    if ( cw_menu_item ( _L ( "Open..." ), "menu_open", "Ctrl+O", a->r_open ) ) imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_OPEN );
    if ( cw_menu_item ( _L ( "Import file..." ), "menu_import", "Ctrl+I", a->r_import ) ) imgui_cmt_tape_import ( -1, cw_import_serial ( v ) );
    if ( cw_menu_item ( _L ( "Save" ), "menu_save", "Ctrl+S", a->r_save ) ) imgui_cmt_tape_save ( false );
    if ( cw_menu_item ( _L ( "Save as..." ), "menu_save_as", NULL, a->r_save ) ) imgui_cmt_tape_save ( true );
    ImGui::Separator ( );
    if ( cw_menu_item ( _L ( "Export block as MZF..." ), "menu_export_mzf", NULL, a->r_export_mzf ) )
        imgui_cmt_tape_export_block ( v->current_block, false, v->tape_serial );
    if ( cw_menu_item ( _L ( "Export block as WAV..." ), "menu_export_wav", NULL, a->r_export_wav ) )
        imgui_cmt_tape_export_block ( v->current_block, true, v->tape_serial );
    if ( cw_menu_item ( _L ( "Export tape as WAV..." ), "menu_export_tape", NULL, a->r_export_tape ) )
        imgui_cmt_tape_export_tape ( v->tape_serial );
    ImGui::Separator ( );
    {
        /* analýza jednoho vybraného surového bloku */
        const st_CMT_VIEW_BLOCK *sb = ( s_cw4.sel.Size == 1 && s_cw.sel_id >= 0 && s_cw.sel_id < v->block_count )
            ? &v->blocks[( size_t ) s_cw.sel_id] : NULL;
        if ( cw_menu_item ( _L ( "Analyze recording..." ), "menu_analyze", NULL, cw_reason_analyze ( v, sb ) ) && sb )
            cmt_analyze_dialog_open ( v, sb->id );
    }
    /* analýza na pozadí při hraní surového bloku (volba okna, ne úprava pásky) */
    if ( ImGui::MenuItem ( _L ( "Find files while playing" ), NULL, s_cw_cfg_auto_analyze != 0u ) ) s_cw_cfg_auto_analyze = s_cw_cfg_auto_analyze ? 0u : 1u;
    cw_tooltip ( NULL, NULL, _ ( "While a raw recording plays, the files in it are looked for in the background and reported when it stops." ) );
    cw_record ( "menu_auto_analyze", s_cw_cfg_auto_analyze ? "on" : "off", ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_LEFT, false );
    /* informace o pásce (úprava pásky) a kontrola pásky (jen čtení) */
    if ( cw_menu_item ( _L ( "Tape information..." ), "menu_tapeinfo", NULL, cw_reason_edit ( v ) ) ) cmt_tapeinfo_dialog_open ( v );
    if ( cw_menu_item ( _L ( "Check tape" ), "menu_check", NULL, cw_reason_check ( v ) ) ) cmt_check_window_open ( );
    ImGui::Separator ( );
    if ( cw_menu_item ( _L ( "Eject" ), "menu_eject", "Ctrl+E", a->r_eject ) ) imgui_cmt_tape_action ( IMGUI_CMT_TAPE_ACTION_EJECT );
    ImGui::EndMenu ( );
}

/**
 * @brief Menu View: Compact / Full (zaškrtnutí), Columns (sloupce tabulky bloků) a Reset column widths.
 *
 * Columns a Reset column widths jsou jen v rozvržení Full (jinak zakázané
 * s důvodem); Reset column widths nastaví šířky pevných sloupců podle
 * obsahu v příštím vykreslení tabulky (s_cw.reset_widths).
 *
 * @pre Uvnitř BeginMenuBar().
 */
static void cw_view_menu ( void )
{
    bool open = ImGui::BeginMenu ( _L ( "View" ) );
    cw_record_menu_bar_item ( "menu_view", open );
    if ( !open ) return;
    bool compact = ( s_cw.layout == CMT_WINDOW_LAYOUT_COMPACT );
    if ( ImGui::MenuItem ( _L ( "Compact" ), NULL, compact ) ) imgui_cmt_window_set_layout ( CMT_WINDOW_LAYOUT_COMPACT );
    if ( ImGui::MenuItem ( _L ( "Full" ), NULL, !compact ) ) imgui_cmt_window_set_layout ( CMT_WINDOW_LAYOUT_FULL );
    ImGui::Separator ( );
    cw_columns_menu ( );
    bool full = ( s_cw.layout == CMT_WINDOW_LAYOUT_FULL );
    if ( cw_menu_item ( _L ( "Reset column widths" ), "menu_reset_widths", NULL,
                        full ? NULL : _ ( "The list of blocks is shown in the Full layout." ) ) )
        s_cw.reset_widths = true;
    ImGui::EndMenu ( );
}

/**
 * @brief Přepínač Echo v liště menu okna, zarovnaný k pravému okraji decku.
 *
 * Malé zaškrtávátko echa kazety (st_CMT_VIEW::echo, cmt_echo_set()): totéž
 * co položka CMT Options > Echo hlavního menu. Kreslí se vlastní: čtvereček
 * o něco menší než výška písma s okrajem (token border, pod myší
 * zvýrazněný), zapnutý vyplněný barvou segment-on se zatržítkem, za ním
 * popisek v barvě textu menu; celek je svisle na střed lišty a jeho pravý
 * okraj lícuje s pravým okrajem panelu decku. Standardní Checkbox() by
 * zabral celou výšku lišty a neměl okraj. Mění jen příznak jádra (zvuk),
 * proto je povolený vždy, i bez pásky a při záznamu vstupů. Bez navigace
 * klávesnicí jako tlačítka decku (D-G4-1).
 *
 * @param v Kopie.
 * @pre Uvnitř BeginMenuBar(), po položkách menu.
 */
static void cw_menu_bar_echo ( const st_CMT_VIEW *v )
{
    const ImGuiStyle &st = ImGui::GetStyle ( );
    ImGuiWindow *win = ImGui::GetCurrentWindow ( );
    const char *label = _ ( "Echo" );
    float fs = ImGui::GetFontSize ( );
    float box = floorf ( fs * 0.8f );
    float gap = floorf ( fs * 0.35f );
    float text_w = ImGui::CalcTextSize ( label ).x;
    float w = box + gap + text_w;
    ImRect bar = win->MenuBarRect ( );
    /* pravý okraj jako panel decku (pracovní oblast okna); nikdy přes položky menu */
    float x = win->WorkRect.Max.x - w;
    x = ImMax ( x, ImGui::GetCursorScreenPos ( ).x + st.ItemSpacing.x );
    ImVec2 pos ( floorf ( x ), bar.Min.y );
    ImGui::SetCursorScreenPos ( pos );
    ImGui::PushItemFlag ( ImGuiItemFlags_NoNav, true );
    bool pressed = ImGui::InvisibleButton ( "##cw_echo", ImVec2 ( w, bar.GetHeight ( ) ), ImGuiButtonFlags_NoNavFocus );
    ImGui::PopItemFlag ( );
    bool hovered = ImGui::IsItemHovered ( );
    cw_tooltip ( _ ( "Play the tape signal through the speaker" ), NULL, NULL );
    cw_record ( "btn_echo", label, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_POPUP, CW_ANCHOR_RIGHT, false );

    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    float cy = floorf ( ( bar.Min.y + bar.Max.y ) * 0.5f );
    ImVec2 bmin ( pos.x, cy - floorf ( box * 0.5f ) );
    ImVec2 bmax ( bmin.x + box, bmin.y + box );
    dl->AddRectFilled ( bmin, bmax, v->echo ? CMT_TOK_SEGMENT_ON : CMT_TOK_PANEL );
    dl->AddRect ( bmin, bmax, hovered ? CMT_TOK_TEXT_DIM : CMT_TOK_BORDER );
    if ( v->echo ) {
        float padc = ImMax ( 1.0f, floorf ( box / 5.0f ) );
        ImGui::RenderCheckMark ( dl, ImVec2 ( bmin.x + padc, bmin.y + padc ), CMT_TOK_TEXT, box - 2.0f * padc );
    }
    dl->AddText ( ImVec2 ( bmax.x + gap, cy - floorf ( fs * 0.5f ) ), hovered ? CMT_TOK_TEXT : CMT_TOK_TEXT_DIM, label );
    if ( pressed ) cmt_echo_set ( v->echo ? 0 : 1 );
}

void imgui_cmt_window_draw ( st_CMT_VIEW *v, bool *p_open )
{
#ifdef MZTEST_HEADLESS
    s_cw_items.clear ( );
#endif
    cw_apply_cfg ( );
    if ( !*p_open ) {
        s_cw.drawn_once = false;
        cmt_view_window_mark_hidden ( );
        /* zavřené okno: potvrzení mazání ani přetažený soubor nečekají na příští otevření */
        s_cw4.del_pending = s_cw4.del_open = false;
        s_cw4.del_ids.clear ( );
        s_cw4.os_ready = false;
        s_cw4.os_files.clear ( );
        /* dialog analýzy se zavřeným oknem: opustit (běžící analýza doběhne a uvolní se sama) */
        cmt_analyze_dialog_abandon ( );
        /* modály informací o pásce a převodu profilu: jako Cancel */
        cmt_tapeinfo_dialog_abandon ( );
        cmt_convert_dialog_abandon ( );
        return;
    }
    /* zaostřit při otevření: okno se kreslí jen otevřené, takže "otevření" =
     * minulý snímek se nekreslilo (vynechaný snímek, nebo nikdy) */
    int frame = ImGui::GetFrameCount ( );
    bool focus_on_open = !s_cw.drawn_once || ( frame - s_cw.drawn_frame ) > 1;
    s_cw.drawn_once = true;
    s_cw.drawn_frame = frame;
    s_cw_opened_this_frame = focus_on_open;

    /* výběr řádků patří pásce: jiná páska výběr zruší celý, zmizelé bloky z něj vypadnou
     * (nikdy se nevnucuje hrající řádek) */
    if ( s_cw4.sel.Size > 0 || s_cw.sel_id >= 0 ) {
        if ( !v->has_tape || v->tape_serial != s_cw.sel_tape_serial ) {
            cw_sel_clear ( );
        } else {
            for ( int id : cw_sel_list ( ) )
                if ( id >= v->block_count ) s_cw4.sel.SetItemSelected ( ( ImGuiID ) id, false );
            cw_sel_fix_primary ( );
        }
    }
    /* výběr po vlastní úpravě pásky (výsledek zásahu): jen nad kopií se stejným čítačem výměn pásky */
    {
        int restore = -1;
        bool restored = false;
        if ( cmt_view_take_select_after_edit ( v->tape_serial, &restore ) && v->has_tape && restore < v->block_count ) {
            cw_sel_single ( restore, v->tape_serial );
            s_cw4.range_reset = true;
            s_cw4.focus_row = restore;
            restored = ( restore >= 0 );
        }
        /* Alt+Up / Alt+Down stisknuté před výsledkem předchozího přesunu: po výsledku nad
         * přesunutým blokem, jen když se výběr obnovil (jinak se páska mezitím změnila a přesun se zahodí) */
        if ( s_cw4.move_seq ) {
            if ( imgui_cmt_tape_move_done ( s_cw4.move_seq ) ) {
                const int dir = s_cw4.move_queued;
                s_cw4.move_seq = 0;
                s_cw4.move_queued = 0;
                if ( dir && restored && restore + dir >= 0 && restore + dir < v->block_count && cw_reason_edit ( v ) == NULL )
                    ( void ) cw_move_key_submit ( v, restore, dir );
            } else if ( device_request_pending ( ) == 0 ) {
                /* fronta prázdná a výsledek nepřišel ani v dalším snímku: ztracený (device_request_clear()) */
                if ( s_cw4.move_idle_frame < 0 ) s_cw4.move_idle_frame = ImGui::GetFrameCount ( );
                else if ( s_cw4.move_idle_frame != ImGui::GetFrameCount ( ) ) {
                    s_cw4.move_seq = 0;
                    s_cw4.move_queued = 0;
                }
            } else {
                s_cw4.move_idle_frame = -1;
            }
        }
    }
    if ( v->tape_serial != s_cw.seen_tape_serial ) {
        s_cw.seen_tape_serial = v->tape_serial;
        s_cw.scroll_current = true;
    }

    st_CW_METRICS m;
    cw_measure ( v, &m );
    en_CMT_WINDOW_LAYOUT layout = s_cw.layout;
    if ( layout == CMT_WINDOW_LAYOUT_FULL ) cw_measure_full ( v, &m );
    ImVec2 work = cw_work_size ( );
    ImVec2 min_size = cw_min_window_size ( &m, layout, work );
#ifdef MZTEST_HEADLESS
    s_cw_min_size = min_size;
#endif
    std::string title = cw_title ( v, cw_title_avail_w ( ) );
#ifdef MZTEST_HEADLESS
    s_cw_title = title;
#endif
    ImVec2 def = cw_default_window_size ( &m, layout, min_size, work );

    /* poloha při prvním otevření podle auto_layout (klíč = stabilní ID, ne měnící se titulek) */
    auto_layout_first_use_portrait ( CW_WINDOW_ID, def.x, def.y );
    if ( s_cw.apply_size ) {
        /* výchozí i pamatovaná velikost nejvýš pracovní plocha (malá obrazovka), aspoň minimum */
        ImVec2 sz = ( s_cw.cfg_w[layout] && s_cw.cfg_h[layout] )
            ? ImVec2 ( ( float ) s_cw.cfg_w[layout], ( float ) s_cw.cfg_h[layout] ) : def;
        sz = ImVec2 ( ImMin ( sz.x, ImMax ( work.x, min_size.x ) ), ImMin ( sz.y, ImMax ( work.y, min_size.y ) ) );
        ImGui::SetNextWindowSize ( ImVec2 ( ImMax ( sz.x, min_size.x ), ImMax ( sz.y, min_size.y ) ), ImGuiCond_Always );
        s_cw.apply_size = false;
    }
    ImGui::SetNextWindowSizeConstraints ( min_size, ImVec2 ( FLT_MAX, FLT_MAX ) );
    /* klik na nález okna Tape check okno kazety dopředu nevynáší (okno kontroly zůstane vidět) */
    if ( focus_on_open ) ImGui::SetNextWindowFocus ( );
    /* sbalené okno by vybraný blok neukázalo: rozbalit */
    if ( s_cw_focus_request || s_cw_expand_for_modal ) ImGui::SetNextWindowCollapsed ( false );
    s_cw_focus_request = false;
    s_cw_expand_for_modal = false;

    ImGui::PushStyleColor ( ImGuiCol_WindowBg, CMT_TOK_BG );
    ImGui::PushStyleColor ( ImGuiCol_MenuBarBg, CMT_TOK_BG );
    bool visible = ImGui::Begin ( title.c_str ( ), p_open, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
    ImGui::PopStyleColor ( 2 );
    if ( !visible ) {
        /* sbalené okno: zprávy jdou modálně (cmt_view_post()); Alt+C ho zavře i tak
         * (fokus má okno, globální zkratky hlavního okna neběží) */
        if ( !s_cw_opened_this_frame
             && ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows )
             && !ImGui::IsPopupOpen ( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel )
             && ImGui::IsKeyChordPressed ( ImGuiMod_Alt | ImGuiKey_C ) )
            *p_open = false;
        /* soubor přetažený nad sbalené okno: otevření jako nad deckem, ne tiché zahození */
        cw_os_drop_process ( v );
        /* obsah se v tomto snímku nekreslí: modály okna (analýza, informace, převod) se nezavírají jako Cancel,
         * jejich stav zůstane a kreslení pokračuje v příštím snímku s obsahem; sbalené okno s otevřeným modálem
         * se v příštím snímku rozbalí (skrytý modál by blokoval vstup) */
        bool modal = cmt_analyze_dialog_skip_frame ( );
        modal = cmt_tapeinfo_dialog_skip_frame ( ) || modal;
        modal = cmt_convert_dialog_skip_frame ( ) || modal;
        modal = modal || s_cw4.del_open;
        if ( modal && ImGui::IsWindowCollapsed ( ) ) s_cw_expand_for_modal = true;
        ImGui::End ( );
        return;
    }
    /* křížek v titulku nastaví *p_open = false už v Begin(): okno se zavírá, ne zobrazuje */
    if ( *p_open ) cmt_view_window_mark_shown ( );
    else cmt_view_window_mark_hidden ( );
    /* vlastník šipek pro Alt+Up / Alt+Down (stabilní ID v kontextu okna) */
    s_cw4.keys_owner = ImGui::GetID ( "##cw_move_keys" );

    /* pamatovaná velikost rozvržení (před případným přepnutím v tomto snímku) */
    if ( !ImGui::IsWindowCollapsed ( ) ) {
        ImVec2 ws = ImGui::GetWindowSize ( );
        s_cw.cfg_w[layout] = ( unsigned ) ImMin ( ws.x + 0.5f, ( float ) CW_CFG_SIZE_MAX );
        s_cw.cfg_h[layout] = ( unsigned ) ImMin ( ws.y + 0.5f, ( float ) CW_CFG_SIZE_MAX );
    }

    /* tokeny jen uvnitř okna */
    ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT );
    ImGui::PushStyleColor ( ImGuiCol_TextDisabled, CMT_TOK_TEXT_DIM );
    ImGui::PushStyleColor ( ImGuiCol_Button, CMT_TOK_CONTROL );
    ImGui::PushStyleColor ( ImGuiCol_ButtonHovered, CMT_TOK_CONTROL_HOVER );
    ImGui::PushStyleColor ( ImGuiCol_ButtonActive, CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_FrameBg, CMT_TOK_CONTROL );
    ImGui::PushStyleColor ( ImGuiCol_FrameBgHovered, CMT_TOK_CONTROL_HOVER );
    ImGui::PushStyleColor ( ImGuiCol_FrameBgActive, CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_Border, CMT_TOK_CONTROL_BORDER );
    ImGui::PushStyleColor ( ImGuiCol_PopupBg, CMT_TOK_PANEL );
    ImGui::PushStyleColor ( ImGuiCol_Header, CMT_TOK_SEGMENT_ON );
    ImGui::PushStyleColor ( ImGuiCol_HeaderHovered, CMT_TOK_CONTROL_HOVER );
    ImGui::PushStyleColor ( ImGuiCol_HeaderActive, CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_ChildBg, CMT_TOK_PANEL );
    ImGui::PushStyleColor ( ImGuiCol_Separator, CMT_TOK_BORDER );
    const int n_colors = 15;
    ImGui::PushStyleVar ( ImGuiStyleVar_FrameBorderSize, 1.0f );
    ImGui::PushStyleVar ( ImGuiStyleVar_FrameRounding, 0.0f );
    ImGui::PushStyleVar ( ImGuiStyleVar_ChildRounding, 0.0f );
    const int n_vars = 3;

    st_CW_ACTIONS act;
    cw_actions ( v, &act );
    if ( ImGui::BeginMenuBar ( ) ) {
        cw_tape_menu ( v, &act );
        cw_view_menu ( );
        cw_menu_bar_echo ( v );
        ImGui::EndMenuBar ( );
    }

    ImGuiWindow *win = ImGui::GetCurrentWindow ( );
#ifdef MZTEST_HEADLESS
    s_cw_inner_min = win->InnerRect.Min;
    s_cw_inner_max = win->InnerRect.Max;
#endif
    ImVec2 o = ImGui::GetCursorScreenPos ( );
    float cw = ImGui::GetContentRegionAvail ( ).x;
    float wbottom = win->InnerRect.Max.y - ImGui::GetStyle ( ).WindowPadding.y;
    /* zalomení stavového řádku a řádku 4 podle skutečné šířky (jen užší okno než jednořádkový deck) */
    cw_metrics_wrap ( &m, cw - 2.0f * m.pad );

    /* panel decku */
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    ImVec2 dmin = o;
    ImVec2 dmax ( o.x + cw, o.y + m.deck_h );
    dl->AddRectFilled ( dmin, dmax, CMT_TOK_PANEL );
    dl->AddRect ( dmin, dmax, CMT_TOK_BORDER );
    cw_record ( "deck", NULL, dmin, dmax, CW_KIND_CONTAINER, CW_ANCHOR_LEFT, false );

    float x0 = o.x + m.pad;
    float x1 = o.x + cw - m.pad;
    float y = o.y + m.pad;
    cw_row_tape ( v, &m, x0, x1, y );
    y += m.row1_h + m.pad;
    cw_hline ( o.x, o.x + cw, y );
    y += m.pad;
    cw_row_display ( v, &m, x0, x1, y );
    y += m.row2_h + m.pad;
    cw_hline ( o.x, o.x + cw, y );
    y += m.pad;
    cw_row_transport ( v, &act, &m, x0, x1, y );
    y += m.btn_h + m.pad;
    cw_hline ( o.x, o.x + cw, y );
    y += m.pad;
    cw_row_speed_mode ( v, &m, x0, x1, y );

    /* řádek zpráv dole: poslední zpráva ztlumeně, chyba v barvě error (K18) */
    float my = wbottom - m.Lh;
    float line_y = my - m.gap - 1.0f;
    if ( layout == CMT_WINDOW_LAYOUT_FULL ) {
        /* Tabulka nahoře, vlastnosti u řádku zpráv; obě jen v celých řádcích:
         * vlastnosti dostanou všechny své řádky, pokud tabulce zbude aspoň
         * CW_TABLE_MIN_ROWS řádků, jinak jen tolik celých řádků, kolik se vejde
         * (obsah se posouvá); tabulka zbytek v celých řádcích. Zbylé pixely
         * (méně než řádek) zůstanou jako mezera mezi tabulkou a vlastnostmi. */
        float top = dmax.y + m.gap;
        float bottom = line_y - m.gap;
        const st_CMT_VIEW_BLOCK *sel = ( s_cw.sel_id >= 0 ) ? &v->blocks[( size_t ) s_cw.sel_id] : NULL;
        std::vector<st_CW_PROP> props;
        std::string ptitle, psub;
        st_CW_PROPS_LAYOUT pl;
        if ( s_cw4.sel.Size > 1 ) {
            /* více vybraných (návrh okna, SPEC): jen "N blocks selected" a nic dalšího */
            char buf[64];
            snprintf ( buf, sizeof ( buf ), _ ( "%d blocks selected" ), s_cw4.sel.Size );
            ptitle = buf;
        } else {
            cw_props_build ( v, sel, &props, &ptitle, &psub );
        }
        cw_props_layout ( &m, props, cw, sel != NULL, &pl );
        float rh = cw_row_h ( &m );
        float avail = bottom - top - m.gap;
        int prows = ( s_cw.props_open && !props.empty ( ) ) ? pl.rows : 0;
        int table_rows = ( int ) floorf ( ( avail - cw_props_frame_h ( &m, prows ) - cw_table_frame_h ( &m, 0 ) ) / rh );
        if ( table_rows < CW_TABLE_MIN_ROWS && prows > 0 ) {
            /* vlastnosti ustoupí tabulce: jen celé řádky, které se vejdou (i žádný = jen hlavička) */
            float left = avail - cw_table_frame_h ( &m, CW_TABLE_MIN_ROWS ) - cw_props_frame_h ( &m, 1 ) + rh;
            prows = ImClamp ( ( int ) floorf ( left / rh ), 0, prows );
            table_rows = ( int ) floorf ( ( avail - cw_props_frame_h ( &m, prows ) - cw_table_frame_h ( &m, 0 ) ) / rh );
        }
        /* okno menší než minimum (malá pracovní plocha): tabulka aspoň hlavička
         * a řádek, vlastnosti jen pokud se pod ni vejde jejich hlavička */
        bool show_props = true;
        if ( table_rows < 1 ) {
            table_rows = ImMax ( ( int ) floorf ( ( avail - cw_table_frame_h ( &m, 0 ) ) / rh ), 0 );
            prows = 0;
            show_props = avail >= cw_table_frame_h ( &m, table_rows ) + m.gap + m.F;
            if ( show_props ) table_rows = ImMax ( ( int ) floorf ( ( avail - m.F - cw_table_frame_h ( &m, 0 ) ) / rh ), 0 );
        }
        float table_h = cw_table_frame_h ( &m, table_rows );
        if ( avail >= table_h ) cw_draw_table ( v, &m, ImVec2 ( o.x, top ), ImVec2 ( cw, table_h ) );
        if ( show_props ) {
            float props_h = cw_props_frame_h ( &m, prows );
            cw_draw_props ( &m, ImVec2 ( o.x, top + avail + m.gap - props_h ), cw, prows, props, &pl, ptitle, psub, s_cw4.sel.Size <= 1 );
        }
    }
    cw_hline ( o.x, o.x + cw, line_y );
    {
        bool elided = false;
        /* cesta ve zprávě: elipsa uprostřed adresáře, jméno souboru zůstane vidět */
        /* vpravo dole je úchyt změny velikosti okna (ImGui: asi 1,1 x výška písma) */
        float msg_w = cw - m.pad - ImMax ( m.pad, ImGui::GetFontSize ( ) * 1.1f );
        std::string msg = cw_ellipsize_message ( v->message, msg_w, &elided );
        ImGui::SetCursorScreenPos ( ImVec2 ( o.x + m.pad, my ) );
        ImGui::PushStyleColor ( ImGuiCol_Text, v->message_is_error ? CMT_TOK_ERROR : ( v->message_is_warning ? CMT_TOK_WARN : CMT_TOK_TEXT_DIM ) );
        ImGui::TextUnformatted ( msg.c_str ( ), msg.c_str ( ) + msg.size ( ) );
        ImGui::PopStyleColor ( );
        if ( elided ) cw_tooltip ( v->message.c_str ( ), NULL, NULL );
        cw_record ( "message", msg.c_str ( ), ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), CW_KIND_OTHER, CW_ANCHOR_LEFT, false );
    }

    /* potvrzení mazání neuložených bloků a soubor přetažený z OS (geometrie tabulky z tohoto snímku) */
    cw_delete_modal ( v );
    cw_os_drop_process ( v );
    /* analýza hrajícího surového bloku na pozadí (nález ohlásí řádek zpráv) */
    cmt_analyze_dialog_auto ( v, s_cw_cfg_auto_analyze != 0u );
    /* dialog analýzy surového bloku (modál v kontextu okna, akce s důvodem úprav pásky) */
    cmt_analyze_dialog_draw ( v, cw_reason_edit ( v ) );
    /* informace o pásce a převod profilu (modály v kontextu okna, akce s důvodem úprav pásky) */
    cmt_tapeinfo_dialog_draw ( v, cw_reason_edit ( v ) );
    cmt_convert_dialog_draw ( v, cw_reason_edit ( v ) );

    /* editor vlastních pulzů výchozí rychlosti (otevírá ho výběr Custom...) */
    {
        int owner;
        st_MZTAPE_PULSES_LENGTH pulses;
        if ( ImGuiCmt::drawPulsesEditor ( CW_PULSES_SCOPE, &owner, &pulses ) ) cmt_request_set_custom_pulses ( &pulses );
        /* vlastní pulzy bloku (kontextové menu Set speed > Custom...) */
        if ( ImGuiCmt::drawPulsesEditor ( CW_BLOCK_PULSES_SCOPE, &owner, &pulses ) ) cw_block_pulses_ok ( v, owner, &pulses );
    }

    /* zkratky s Ctrl při fokusu okna (stejné akce a zákazy jako tlačítka) */
    cw_shortcuts ( v, &act, p_open );
    /* Alt+C v tomto snímku: výsledky zpracované po kreslení oken jdou podle pravidel skrytého okna */
    if ( !*p_open ) cmt_view_window_mark_hidden ( );

    ImGui::PopStyleVar ( n_vars );
    ImGui::PopStyleColor ( n_colors );
    ImGui::End ( );
#ifdef MZTEST_HEADLESS
    s_cw_items_done = s_cw_items;
#endif
}

void imgui_cmt_window ( bool *p_open )
{
    static st_CMT_VIEW s_view;
    static bool s_view_init = false;
    if ( !s_view_init ) {
        cmt_view_reset ( &s_view );
        s_view_init = true;
    }
    if ( *p_open ) {
        cmt_view_fill ( &s_view );
        cmt_view_request_lengths ( &s_view );
    }
    imgui_cmt_window_draw ( &s_view, p_open );
}

void imgui_cmt_window_modal_next_pos ( void )
{
    ImGuiWindow *w = ImGui::FindWindowByName ( CW_WINDOW_ID );
    /* okno se v tomto ani minulém snímku nekreslilo (zavřené): výchozí poloha ImGui (střed hlavního viewportu) */
    if ( !w || !( w->Active || w->WasActive ) ) return;
    ImVec2 c ( w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f );
    /* jen poloha: viewport si ImGui zvolí sám (modál větší než vytažené okno kazety dostane vlastní
     * okno OS a jde přetáhnout kamkoli; připnutí k viewportu okna kazety ho v něm věznilo) */
    ImGui::SetNextWindowPos ( c, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
}

/**
 * @brief Plocha, do které se má vejít modál okna kazety: pracovní plocha monitoru okna, jinak jeho viewportu.
 *
 * S okny mimo hlavní okno (ImGuiConfigFlags_ViewportsEnable) je to pracovní
 * plocha monitoru, na kterém je viewport okna kazety (okno vytažené jako
 * samostatné okno OS má viewport jen velký jako ono samo); bez nich
 * pracovní plocha viewportu.
 *
 * @param w Okno Virtual CMT. @param[out] a Levý horní roh. @param[out] b Pravý dolní roh.
 * @return true = plocha známá.
 */
static bool cw_modal_area ( const ImGuiWindow *w, ImVec2 *a, ImVec2 *b )
{
    ImGuiViewportP *vp = ( ImGuiViewportP * ) ImGui::FindViewportByID ( w->ViewportId );
    if ( !vp ) return false;
    *a = vp->WorkPos;
    *b = ImVec2 ( vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y );
    const ImGuiPlatformIO &pio = ImGui::GetPlatformIO ( );
    if ( ( ImGui::GetIO ( ).ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) && vp->PlatformMonitor >= 0
         && vp->PlatformMonitor < pio.Monitors.Size ) {
        const ImGuiPlatformMonitor &mon = pio.Monitors[vp->PlatformMonitor];
        *a = mon.WorkPos;
        *b = ImVec2 ( mon.WorkPos.x + mon.WorkSize.x, mon.WorkPos.y + mon.WorkSize.y );
    }
    return true;
}

void imgui_cmt_window_select_block ( int block_id, unsigned tape_serial )
{
    if ( block_id < 0 ) return;
    cw_sel_single ( block_id, tape_serial );
    s_cw4.range_reset = true;
    /* řádek do viditelné části a fokus navigace na něj (cw_table_row()) */
    s_cw4.focus_row = block_id;
    /* seznam bloků je jen v rozvržení Full */
    if ( s_cw.layout != CMT_WINDOW_LAYOUT_FULL ) imgui_cmt_window_set_layout ( CMT_WINDOW_LAYOUT_FULL );
    if ( g_gui ) g_gui->showVirtualCmtWindow = true;
    s_cw_focus_request = true;
}

ImVec2 imgui_cmt_window_work_size ( void )
{
    return cw_work_size ( );
}

bool imgui_cmt_window_modal_center ( ImVec2 size, ImGuiCond cond )
{
    ImGuiWindow *w = ImGui::FindWindowByName ( CW_WINDOW_ID );
    if ( !w || !( w->Active || w->WasActive ) ) return false;
    ImVec2 c ( w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f );
    /* viewport se nepřipíná (viz imgui_cmt_window_modal_next_pos()): modál jde přetáhnout mimo okno */
    if ( size.x <= 0.0f || size.y <= 0.0f ) {
        ImGui::SetNextWindowPos ( c, cond, ImVec2 ( 0.5f, 0.5f ) );
        return true;
    }
    ImVec2 p ( floorf ( c.x - size.x * 0.5f ), floorf ( c.y - size.y * 0.5f ) );
    /* celý modál v pracovní ploše monitoru (nebo viewportu) okna: velký dialog u okraje obrazovky */
    ImVec2 a, b;
    if ( cw_modal_area ( w, &a, &b ) ) {
        p.x = ImMax ( a.x, ImMin ( p.x, b.x - size.x ) );
        p.y = ImMax ( a.y, ImMin ( p.y, b.y - size.y ) );
    }
    ImGui::SetNextWindowPos ( p, cond );
    return true;
}

float imgui_cmt_window_place_beside ( ImVec2 size, ImGuiCond cond, float min_h )
{
    ImGuiWindow *w = ImGui::FindWindowByName ( CW_WINDOW_ID );
    if ( !w || !( w->Active || w->WasActive ) ) {
        /* okno kazety se nekreslí: doprostřed pracovní plochy hlavního viewportu */
        const ImGuiViewport *mv = ImGui::GetMainViewport ( );
        ImGui::SetNextWindowPos ( ImVec2 ( mv->WorkPos.x + mv->WorkSize.x * 0.5f, mv->WorkPos.y + mv->WorkSize.y * 0.5f ), cond,
                                  ImVec2 ( 0.5f, 0.5f ) );
        return FLT_MAX;
    }
    if ( size.x <= 0.0f || size.y <= 0.0f ) {
        ( void ) imgui_cmt_window_modal_center ( size, cond );
        return FLT_MAX;
    }
    /* plocha umístění: pracovní plocha monitoru okna kazety (okno vytažené jako samostatné okno OS má
     * viewport jen velký jako ono samo), jinak jeho viewportu */
    ImVec2 a ( -FLT_MAX, -FLT_MAX ), b ( FLT_MAX, FLT_MAX );
    ( void ) cw_modal_area ( w, &a, &b );
    const float gap = ImGui::GetStyle ( ).ItemSpacing.x;
    const ImVec2 w1 ( w->Pos.x + w->Size.x, w->Pos.y + w->Size.y );
    /* vodorovně u levého okraje okna kazety (pod a nad ním), svisle u horního okraje (vpravo a vlevo); omezeno do plochy */
    const float x_under = ImMax ( a.x, ImMin ( w->Pos.x, b.x - size.x ) );
    const float y_side = ImMax ( a.y, ImMin ( w->Pos.y, b.y - size.y ) );
    ImVec2 p;
    float max_h = FLT_MAX;
    if ( w1.x + gap + size.x <= b.x ) {
        p = ImVec2 ( w1.x + gap, y_side );
    } else if ( w1.y + gap + size.y <= b.y && size.x <= b.x - a.x ) {
        p = ImVec2 ( x_under, w1.y + gap );
    } else if ( w->Pos.y - gap - size.y >= a.y && size.x <= b.x - a.x ) {
        p = ImVec2 ( x_under, w->Pos.y - gap - size.y );
    } else if ( w->Pos.x - gap - size.x >= a.x ) {
        p = ImVec2 ( w->Pos.x - gap - size.x, y_side );
    } else {
        /* vedle okna místo není: pás okna kazety nad tabulkou bloků nebo pod ní (tabulka z minulého snímku) */
        const bool table_ok = ( s_cw_table_frame >= ImGui::GetFrameCount ( ) - 1 ) && s_cw_table_max.y > s_cw_table_min.y;
        const float top0 = ImMax ( a.y, w->Pos.y ), top1 = s_cw_table_min.y - gap;
        const float bot0 = s_cw_table_max.y + gap, bot1 = ImMin ( b.y, w1.y );
        const float top_h = table_ok ? top1 - top0 : 0.0f, bot_h = table_ok ? bot1 - bot0 : 0.0f;
        const float x_right = ImMax ( a.x, ImMin ( w1.x - size.x, b.x - size.x ) );
        if ( table_ok && ( top_h >= size.y || bot_h >= size.y ) ) {
            /* celé se vejde: dolní pás má přednost (vlastnosti a řádek zpráv, deck zůstane vidět) */
            p = ( bot_h >= size.y ) ? ImVec2 ( x_right, bot0 ) : ImVec2 ( x_right, top0 );
        } else if ( table_ok && ImMax ( top_h, bot_h ) >= min_h && min_h > 0.0f ) {
            /* vejde se jen zmenšené: vyšší pás a výška omezená na něj */
            const bool bottom = ( bot_h >= top_h );
            p = ImVec2 ( x_right, bottom ? bot0 : top0 );
            max_h = floorf ( bottom ? bot_h : top_h );
        } else {
            ( void ) imgui_cmt_window_modal_center ( size, cond );
            return FLT_MAX;
        }
    }
    /* viewport se nepřipíná: okno vedle vytaženého okna kazety dostane vlastní okno OS */
    ImGui::SetNextWindowPos ( ImVec2 ( floorf ( p.x ), floorf ( p.y ) ), cond );
    return max_h;
}

float imgui_cmt_button_w ( const char *label )
{
    return cw_button_w ( label );
}

bool imgui_cmt_button ( const char *label, float width, const char *reason, bool primary )
{
    const bool on = primary && !reason;
    ImGui::PushStyleColor ( ImGuiCol_Button, on ? CMT_TOK_SEGMENT_ON : CMT_TOK_CONTROL );
    ImGui::PushStyleColor ( ImGuiCol_ButtonHovered, on ? CMT_TOK_SEGMENT_ON_HOVER : CMT_TOK_CONTROL_HOVER );
    ImGui::PushStyleColor ( ImGuiCol_ButtonActive, on ? CMT_TOK_SEGMENT_ON : CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_Border, CMT_TOK_CONTROL_BORDER );
    ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT );
    ImGui::PushStyleVar ( ImGuiStyleVar_FrameBorderSize, 1.0f );
    ImGui::PushStyleVar ( ImGuiStyleVar_FrameRounding, 0.0f );
    ImGui::BeginDisabled ( reason != NULL );
    const bool pressed = ImGui::Button ( label, ImVec2 ( ( width > 0.0f ) ? width : imgui_cmt_button_w ( label ), 0.0f ) );
    ImGui::EndDisabled ( );
    ImGui::PopStyleVar ( 2 );
    ImGui::PopStyleColor ( 5 );
    if ( reason && ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled ) ) ImGui::SetTooltip ( "%s", reason );
    return pressed && !reason;
}

bool imgui_cmt_section_header ( const char *id, const char *title, bool open, float width )
{
    const float F = ImGui::GetFrameHeight ( );
    const float Lh = ImGui::GetTextLineHeight ( );
    const float u = ImGui::GetFontSize ( ) / 7.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos ( );
    const ImVec2 pmax ( pos.x + width, pos.y + F );
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    /* jako cw_draw_props(): panel, rámeček border, pod myší control-hover uvnitř rámečku, šipka 0,7 řádku, nadpis */
    dl->AddRectFilled ( pos, pmax, CMT_TOK_PANEL );
    dl->AddRect ( pos, pmax, CMT_TOK_BORDER );
    if ( ImGui::InvisibleButton ( id, ImVec2 ( width, F ) ) ) open = !open;
    if ( ImGui::IsItemHovered ( ) ) dl->AddRectFilled ( ImVec2 ( pos.x + 1.0f, pos.y + 1.0f ), ImVec2 ( pmax.x - 1.0f, pmax.y - 1.0f ), CMT_TOK_CONTROL_HOVER );
    const float ty = pos.y + ImGui::GetStyle ( ).FramePadding.y;
    const float ax = pos.x + 2.0f * u;
    ImGui::RenderArrow ( dl, ImVec2 ( ax, ty ), CMT_TOK_TEXT, open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f );
    dl->AddText ( ImVec2 ( ax + Lh * 0.7f + 2.0f * u, ty ), CMT_TOK_TEXT, title );
    return open;
}

bool imgui_cmt_window_current_outside_work ( void )
{
    ImGuiWindow *win = ImGui::GetCurrentWindow ( );
    if ( !win || !win->Viewport || ImGui::IsWindowAppearing ( ) ) return false;
    const ImGuiViewport *vp = win->Viewport;
    return win->Pos.y + win->Size.y > vp->WorkPos.y + vp->WorkSize.y + 0.5f || win->Pos.x + win->Size.x > vp->WorkPos.x + vp->WorkSize.x + 0.5f
           || win->Pos.y < vp->WorkPos.y - 0.5f || win->Pos.x < vp->WorkPos.x - 0.5f;
}

void imgui_cmt_close_popup ( ImGuiID *popup_id )
{
    ImGuiContext *ctx = ImGui::GetCurrentContext ( );
    if ( *popup_id && ctx && ctx->WithinFrameScope ) {
        for ( int n = 0; n < ctx->OpenPopupStack.Size; n++ ) {
            if ( ctx->OpenPopupStack[n].PopupId != *popup_id ) continue;
            ImGui::ClosePopupToLevel ( n, true );
            break;
        }
    }
    *popup_id = 0;
}

int imgui_cmt_window_selected_block ( unsigned *tape_serial )
{
    if ( tape_serial ) *tape_serial = s_cw.sel_tape_serial;
    return ( s_cw4.sel.Size == 1 ) ? s_cw.sel_id : -1;
}

bool imgui_cmt_replay_recording ( void )
{
    return g_atomic_int_get ( &g_replay_mode ) == REPLAY_MODE_RECORDING;
}

bool imgui_cmt_replay_playing ( void )
{
    return g_atomic_int_get ( &g_replay_mode ) == REPLAY_MODE_PLAYING;
}

en_CMT_WINDOW_LAYOUT imgui_cmt_window_get_layout ( void )
{
    return s_cw.layout;
}

void imgui_cmt_window_set_layout ( en_CMT_WINDOW_LAYOUT layout )
{
    if ( layout != CMT_WINDOW_LAYOUT_FULL ) layout = CMT_WINDOW_LAYOUT_COMPACT;
    s_cw.cfg_applied = true;
    if ( layout == s_cw.layout ) return;
    s_cw.layout = layout;
    s_cw.cfg_layout = ( unsigned ) layout;
    s_cw.apply_size = true;
}

void imgui_cmt_window_cfg_init ( void )
{
    if ( s_cw.cfg_registered ) return;
    s_cw.cfg_registered = true;
    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, ( char * ) "VIRTUAL_CMT_UI" );
    if ( !cmod ) return; /* duplicitní modul: okno pojede bez persistence */
    CFGELM *elm = cfgmodule_register_new_element ( cmod, ( char * ) "layout", CFGENTYPE_UNSIGNED, 0, 0, 1 );
    cfgelement_set_handlers ( elm, ( void * ) &s_cw.cfg_layout, ( void * ) &s_cw.cfg_layout );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "compact_w", CFGENTYPE_UNSIGNED, 0, 0, CW_CFG_SIZE_MAX );
    cfgelement_set_handlers ( elm, ( void * ) &s_cw.cfg_w[CMT_WINDOW_LAYOUT_COMPACT], ( void * ) &s_cw.cfg_w[CMT_WINDOW_LAYOUT_COMPACT] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "compact_h", CFGENTYPE_UNSIGNED, 0, 0, CW_CFG_SIZE_MAX );
    cfgelement_set_handlers ( elm, ( void * ) &s_cw.cfg_h[CMT_WINDOW_LAYOUT_COMPACT], ( void * ) &s_cw.cfg_h[CMT_WINDOW_LAYOUT_COMPACT] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "full_w", CFGENTYPE_UNSIGNED, 0, 0, CW_CFG_SIZE_MAX );
    cfgelement_set_handlers ( elm, ( void * ) &s_cw.cfg_w[CMT_WINDOW_LAYOUT_FULL], ( void * ) &s_cw.cfg_w[CMT_WINDOW_LAYOUT_FULL] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "full_h", CFGENTYPE_UNSIGNED, 0, 0, CW_CFG_SIZE_MAX );
    cfgelement_set_handlers ( elm, ( void * ) &s_cw.cfg_h[CMT_WINDOW_LAYOUT_FULL], ( void * ) &s_cw.cfg_h[CMT_WINDOW_LAYOUT_FULL] );
    /* vlastnosti výchozí sbalené (D-G3-9) */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "props_open", CFGENTYPE_UNSIGNED, 0, 0, 1 );
    cfgelement_set_handlers ( elm, ( void * ) &s_cw.props_open, ( void * ) &s_cw.props_open );
    /* analýza hrajícího surového bloku na pozadí, výchozí zapnutá */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "auto_analyze", CFGENTYPE_UNSIGNED, 1, 0, 1 );
    cfgelement_set_handlers ( elm, ( void * ) &s_cw_cfg_auto_analyze, ( void * ) &s_cw_cfg_auto_analyze );
    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


/**
 * @brief Poloha události přetažení SDL v souřadnicích ImGui (jako pohyb myši v imgui_impl_sdl3.cpp).
 *
 * Okno SDL musí být viewport ImGui (PlatformHandle = SDL_WindowID, backend
 * SDL3); s ViewportsEnable jsou souřadnice ImGui souřadnice plochy, proto
 * se přičte poloha okna SDL.
 *
 * @param e Událost přetažení. @param[out] pos Poloha. @param[out] vp Viewport okna.
 * @return true = okno je viewport ImGui; false = cizí okno (událost se ignoruje).
 */
static bool cw_os_drop_pos ( const SDL_DropEvent *e, ImVec2 *pos, ImGuiViewport **vp )
{
    *vp = ImGui::FindViewportByPlatformHandle ( ( void * ) ( intptr_t ) e->windowID );
    if ( !*vp ) return false;
    ImVec2 p ( e->x, e->y );
    if ( ImGui::GetIO ( ).ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
        int wx = 0, wy = 0;
        SDL_Window *w = SDL_GetWindowFromID ( e->windowID );
        if ( w ) SDL_GetWindowPosition ( w, &wx, &wy );
        p.x += ( float ) wx;
        p.y += ( float ) wy;
    }
    *pos = p;
    return true;
}


void imgui_cmt_window_os_drop_event ( const union SDL_Event *event )
{
    if ( !event || !ImGui::GetCurrentContext ( ) ) return;
    const SDL_DropEvent *e = &event->drop;
    ImVec2 pos;
    ImGuiViewport *vp = NULL;
    switch ( event->type ) {
        case SDL_EVENT_DROP_BEGIN:
            s_cw4.os_active = true;
            s_cw4.os_pos_valid = false;
            s_cw4.os_files.clear ( );
            s_cw4.os_ready = false;
            break;
        case SDL_EVENT_DROP_POSITION:
            if ( !cw_os_drop_pos ( e, &pos, &vp ) ) break;
            s_cw4.os_active = true;
            s_cw4.os_pos_valid = true;
            s_cw4.os_pos = pos;
            s_cw4.os_pos_time = ImGui::GetTime ( );
            /* o okně pod kurzorem rozhodne ImGui (během tažení OS jinak polohu myši nedostává) */
            ImGui::GetIO ( ).AddMouseViewportEvent ( vp->ID );
            ImGui::GetIO ( ).AddMousePosEvent ( pos.x, pos.y );
            break;
        case SDL_EVENT_DROP_FILE:
            if ( !e->data || !e->data[0] || !cw_os_drop_pos ( e, &pos, &vp ) ) break;
            s_cw4.os_files.push_back ( e->data );
            s_cw4.os_file_pos = pos;
            ImGui::GetIO ( ).AddMouseViewportEvent ( vp->ID );
            ImGui::GetIO ( ).AddMousePosEvent ( pos.x, pos.y );
            break;
        case SDL_EVENT_DROP_COMPLETE:
            s_cw4.os_active = false;
            s_cw4.os_pos_valid = false;
            if ( !s_cw4.os_files.empty ( ) ) {
                s_cw4.os_ready = true;
                s_cw4.os_ready_frame = ImGui::GetFrameCount ( );
            }
            break;
        default:
            break;
    }
}


/* ------------------------------------------------------------------------- */
/*  Testovací rozhraní                                                       */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
const std::vector<st_CMT_WINDOW_TEST_ITEM> &imgui_cmt_window_test_items ( void )
{
    return s_cw_items_done;
}

void imgui_cmt_window_test_inner_rect ( ImVec2 *min, ImVec2 *max )
{
    *min = s_cw_inner_min;
    *max = s_cw_inner_max;
}

void imgui_cmt_window_test_set_selection ( int block_id, unsigned tape_serial )
{
    cw_sel_single ( block_id, tape_serial );
    s_cw4.range_reset = true;
}

std::vector<int> imgui_cmt_window_test_get_selected ( void )
{
    return cw_sel_list ( );
}

void imgui_cmt_window_test_set_selected ( const std::vector<int> &blocks, unsigned tape_serial )
{
    cw_sel_clear ( );
    for ( int id : blocks )
        if ( id >= 0 ) s_cw4.sel.SetItemSelected ( ( ImGuiID ) id, true );
    s_cw.sel_id = blocks.empty ( ) ? -1 : blocks.back ( );
    s_cw.sel_tape_serial = tape_serial;
    s_cw4.range_reset = true;
}

bool imgui_cmt_window_test_delete_modal_open ( void )
{
    return s_cw4.del_open || s_cw4.del_pending;
}

int imgui_cmt_window_test_get_selection ( void )
{
    return s_cw.sel_id;
}

ImGuiID imgui_cmt_window_test_table_id ( void )
{
    return s_cw.table_id;
}

bool imgui_cmt_window_test_table_scroll_x ( void )
{
    return s_cw_table_scroll_x;
}

unsigned imgui_cmt_window_test_props_open ( void )
{
    return s_cw.props_open;
}

void imgui_cmt_window_test_set_props_open ( unsigned open )
{
    s_cw.props_open = open ? 1u : 0u;
}

bool imgui_cmt_window_test_props_scrollbar ( void )
{
    return s_cw_props_scroll_y;
}

const char *imgui_cmt_window_test_title ( void )
{
    return s_cw_title.c_str ( );
}

ImVec2 imgui_cmt_window_test_min_size ( void )
{
    return s_cw_min_size;
}

void imgui_cmt_window_test_set_stretch ( bool on )
{
    cmt_view_test_set_stretch ( on );
}

void imgui_cmt_window_test_reset ( void )
{
    s_cw.layout = CMT_WINDOW_LAYOUT_COMPACT;
    s_cw.cfg_layout = 0;
    for ( int i = 0; i < CMT_WINDOW_LAYOUT_COUNT; i++ ) {
        s_cw.cfg_w[i] = 0;
        s_cw.cfg_h[i] = 0;
    }
    s_cw.apply_size = true;
    s_cw.cfg_applied = true;
    s_cw.show_remaining = false;
    s_cw_focus_request = false;
    s_cw_expand_for_modal = false;
    cw_sel_clear ( );
    s_cw4.range_reset = true;
    s_cw4.focus_row = -1;
    s_cw4.del_pending = s_cw4.del_open = false;
    s_cw4.del_ids.clear ( );
    s_cw4.drag_cancel = false;
    s_cw4.drag_from = -1;
    s_cw4.drag_serial = 0u;
    s_cw4.scroll_acc = 0.0f;
    s_cw4.move_seq = 0;
    s_cw4.move_queued = 0;
    s_cw4.move_idle_frame = -1;
    s_cw4.os_active = s_cw4.os_pos_valid = s_cw4.os_ready = false;
    s_cw4.os_files.clear ( );
    {
        int dummy;
        ( void ) cmt_view_take_select_after_edit ( 0u, &dummy );
    }
    if ( ImGui::GetCurrentContext ( ) && ImGui::IsPopupOpen ( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) )
        ImGui::ClosePopupToLevel ( 0, false );
    s_cw.props_open = 0u;
    s_cw.col_toggle = -1;
    s_cw.col_show_all = false;
    s_cw_cfg_auto_analyze = 1u;
    s_cw.nav_last = 0;
    s_cw.seen_tape_serial = 0u;
    s_cw.scroll_current = true;
    s_cw.rec_scrolled = false;
    s_cw.reset_widths = false;
    s_cw_tc.valid = false;
    /* nastavení tabulky (šířky, viditelnost) z dřívějšího měřítka nebo testu zahodit */
    if ( s_cw.table_id ) {
        ImGuiTable *t = ImGui::TableFindByID ( s_cw.table_id );
        if ( t ) ImGui::TableResetSettings ( t );
    }
    const bool def_enabled[CW_COL_COUNT] = { true, true, true, true, false, false, false, false, true, true };
    for ( int c = 0; c < CW_COL_COUNT; c++ ) s_cw.col_enabled[c] = def_enabled[c];
    s_cw_items.clear ( );
    s_cw_items_done.clear ( );
}

void imgui_cmt_window_test_set_work_size ( ImVec2 size )
{
    s_cw_test_work = size;
}

void imgui_cmt_window_test_confirm_block_pulses ( const st_CMT_VIEW *v, int owner, const st_MZTAPE_PULSES_LENGTH *pulses )
{
    cw_block_pulses_ok ( v, owner, pulses );
}

void imgui_cmt_window_test_cfg_forget ( void )
{
    s_cw.cfg_registered = false;
    s_cw.cfg_applied = false;
}
#endif
