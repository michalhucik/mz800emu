/**
 * @file   overlay.cpp
 * @brief  Overlay menu: stav otevření, pauza, fokus, klávesy a rámec (horní lišta s taby, obsah, patička).
 *
 * Popis chování a kontraktů je v overlay.h. Vzhled se řídí vizuálním
 * systémem (overlay_style.h) a prvky rámce se skládají z widgetů
 * (overlay_widgets.h).
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef MZTEST_HEADLESS
#include <map>
#include <string>
#endif

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"
#include "emulator/emulator.h"
#include "device_request.h"
#include "debugger/run_control/run_log.h"

#include "ui-imgui/main_window_focus.h"
#include "ui-imgui/imgui_modal.h"
#include "snapshot/snapshot_request.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/overlay/overlay.h"
#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

/* ================================================================
 * Rozměry rámce v návrhových bodech (overlay_px())
 * ================================================================ */

/** @brief Svislý vnitřní okraj obsahu tabu. */
#define OVL_CONTENT_PAD_Y 24.0f
/** @brief Vodorovný vnitřní okraj tabu a tlačítka Close v horní liště. */
#define OVL_TAB_PAD_X 10.0f
/** @brief Svislý odstup tabu a tlačítka Close od okrajů horní lišty. */
#define OVL_TAB_INSET_Y 8.0f
/** @brief Mezera mezi číslicí, ikonou a popiskem tabu. */
#define OVL_TAB_GAP 8.0f
/** @brief Mezera mezi taby. */
#define OVL_TAB_SPACING 4.0f
/** @brief Mezera mezi stavovými štítky. */
#define OVL_CHIP_SPACING 8.0f
/** @brief Nejmenší mezera mezi taby a pravou skupinou lišty a mezi štítky a tlačítkem Close. */
#define OVL_BAR_GROUP_GAP 16.0f
/** @brief Mezera mezi klávesovým štítkem a textem nápovědy v patičce. */
#define OVL_HINT_GAP 6.0f
/** @brief Mezera mezi nápovědou patičky a oddělovací tečkou. */
#define OVL_HINT_SPACING 10.0f
/** @brief Krytí vrstvy, kterou se horní lišta a patička přitmaví nad plochu overlaye. */
#define OVL_BAR_EXTRA_OPACITY 0.60f

/* ================================================================
 * Obnova běhu emulace po zavření
 * ================================================================ */

/** @brief Kolikrát se nejvýš zopakuje nepovedené rozběhnutí emulace (po zavření; při otevřeném overlayi pro tab Sound). */
#define OVL_RUN_RETRY_ATTEMPTS 5
/** @brief Počet snímků UI mezi dvěma pokusy o rozběhnutí emulace. */
#define OVL_RUN_RETRY_INTERVAL_FRAMES 20
/** @brief Počet kreslení po otevření nebo přepnutí tabu, ve kterých tab dává fokus svému výchozímu prvku. */
#define OVL_DEFAULT_FOCUS_FRAMES 2

/* ================================================================
 * Tabulka tabů
 * ================================================================ */

/**
 * @brief Funkce kreslení obsahu tabu.
 * @param v Kopie stavu (platná po dobu volání).
 * @param u Jednotka rozměrů overlaye v pixelech.
 * @pre Volá se uvnitř okna obsahu overlaye se stylem overlay_style_push(); smí kreslit jen z kopie.
 */
typedef void (*overlay_tab_draw_fn)(const st_OVERLAY_VIEW *v, float u);

/**
 * @brief Řádek tabulky tabů.
 *
 * Invariant: řádek s indexem i má tab == i (pořadí en_OVERLAY_TAB).
 */
typedef struct st_OVERLAY_TAB_DEF {
    en_OVERLAY_TAB tab;       /**< tab, kterému řádek patří */
    const char *label;        /**< anglický popisek (msgid, překládá se při kreslení) */
    const char *icon;         /**< znak ikony z písma Lucide (kódový bod ověřený proti lucide-static 1.42.0) */
    overlay_tab_draw_fn draw; /**< kreslení obsahu tabu */
} st_OVERLAY_TAB_DEF;

/** @brief Taby v pořadí horní lišty. Kreslení tabů je v overlay_tab_<jméno>.cpp. */
static const st_OVERLAY_TAB_DEF s_tabs[OVERLAY_TAB_COUNT] = {
    {OVERLAY_TAB_MEDIA, N_("Media"), ICON_LC_SAVE, overlay_tab_media_draw},
    {OVERLAY_TAB_MACHINE, N_("Machine"), ICON_LC_CPU, overlay_tab_machine_draw},
    {OVERLAY_TAB_DISPLAY, N_("Display"), ICON_LC_MONITOR, overlay_tab_display_draw},
    {OVERLAY_TAB_SOUND, N_("Sound"), ICON_LC_VOLUME_2, overlay_tab_sound_draw},
    {OVERLAY_TAB_INPUT, N_("Input"), ICON_LC_KEYBOARD, overlay_tab_input_draw},
    {OVERLAY_TAB_CAPTURE, N_("Capture"), ICON_LC_CAMERA, overlay_tab_capture_draw},
};

/* Stav žádostí klasického menu Snapshot (menu_snapshot.cpp; modul nemá hlavičku) */
extern "C" bool snapshot_ui_is_quicksave_requested(void);
extern "C" bool snapshot_ui_is_save_requested(void);
extern "C" void snapshot_ui_run_after_request(void);

/* ================================================================
 * Stav
 * ================================================================ */

/** @brief Overlay je otevřený. */
static bool s_open = false;
/** @brief Zvolený tab (vždy platný). */
static en_OVERLAY_TAB s_tab = OVERLAY_TAB_MEDIA;
/**
 * @brief Pauzu způsobil overlay a od té doby ji nikdo nezměnil.
 *
 * Nastaví se při otevření, když overlay běžící emulaci zastavil; shodí se,
 * jakmile overlay uvidí emulaci běžet (rozběhl ji někdo jiný: MCP,
 * debugger) nebo jiné číslo běhu než s_pause_run_seq (krok nebo krátký běh
 * mezi dvěma snímky UI), když akce overlaye oznámí, že emulace má stát,
 * nebo když overlay emulaci sám rozběhne pro poslech na tabu Sound (pak
 * platí s_listen_run).
 */
static bool s_pause_is_ours = false;
/**
 * @brief Přepínač Listen tabu Sound je zapnutý: uživatel chce, aby emulace při otevřeném overlayi běžela.
 *
 * Nastavuje ho jen imgui_overlay_set_listen(true), a to jen nad pauzou
 * overlaye. Shodí se při vypnutí přepínače, při otevření a zavření
 * overlaye, při změně tabu, při otevřeném výsuvném listu nebo rozbaleném
 * výběru, když emulaci během poslechu zastaví někdo jiný, když pauza
 * přestane být overlaye dřív, než se emulace rozběhne, po vyčerpání pokusů
 * o rozběhnutí a když akce overlaye oznámí změnu pauzy
 * (imgui_overlay_note_pause_changed()). Skutečný běh sleduje s_listen_run;
 * srovnává je overlay_sync_listen().
 */
static bool s_listen_wanted = false;
/**
 * @brief Emulace běží kvůli poslechu: overlay svou pauzu dočasně zrušil, aby byly změny zvuku slyšet.
 *
 * Nastaví se, když overlay na žádost přepínače Listen rozběhne emulaci,
 * kterou sám zastavil; platí jen při otevřeném overlayi a nikdy zároveň
 * s s_pause_is_ours. Shodí se, když poslech skončí (overlay emulaci znovu
 * zastaví a pauzu si převezme), když emulaci mezitím zastavil někdo jiný
 * (pauza pak overlaye není), při zavření (emulace běží dál) a když akce
 * overlaye oznámí změnu pauzy (imgui_overlay_note_pause_changed()).
 * Výjimka: zastaví-li si emulaci uložení snapshotu, příznak vydrží do jeho
 * konce (s_listen_wanted už ne), aby si overlay po něm pauzu vzal zpět.
 */
static bool s_listen_run = false;
/**
 * @brief Číslo běhu jádra (run_log_current()) v okamžiku, kdy overlay pauzu převzal; platí při s_pause_is_ours.
 *
 * Číslo zvedá emu vlákno při každém opuštění pauzy (rozběh i krok jedné
 * instrukce); jiná hodnota tedy znamená, že emulaci mezitím někdo rozběhl
 * nebo krokoval, i když při kreslení overlaye zase stála. V buildu bez
 * debuggeru se číslo nemění a rozhoduje jen stav pauzy viděný jednou za
 * snímek.
 */
static uint64_t s_pause_run_seq = 0;
/** @brief Akce overlaye určila, že po zavření má emulace běžet bez ohledu na stav před otevřením (Resume). */
static bool s_run_on_close = false;
/**
 * @brief Zbývající pokusy o rozběhnutí emulace.
 *
 * Zavřený overlay: pokusy po zavření (0 = nic nečeká; overlay_retry_run()).
 * Otevřený overlay: pokusy o rozběhnutí pro poslech na tabu Sound od
 * posledního zapnutí přepínače Listen (overlay_sync_listen()).
 */
static int s_run_retry_attempts = 0;
/** @brief Snímky UI do dalšího pokusu o rozběhnutí emulace. */
static int s_run_retry_wait = 0;
/** @brief imgui_overlay_frame() má co dělat: overlay je otevřený nebo čeká pokus o rozběhnutí (jediný test zavřeného overlaye). */
static bool s_frame_work = false;
/** @brief Zbývající kreslení, ve kterých má tab dát fokus svému výchozímu prvku (overlay_default_focus_wanted()). */
static int s_default_focus_frames = 0;
/** @brief Příští kreslení má oknu overlaye vynutit fokus. */
static bool s_focus_request = false;
/** @brief Číslo snímku ImGui, ve kterém byl overlay otevřen (-1 = mimo snímek); v něm se klávesy neobsluhují. */
static int s_open_frame = -1;
/** @brief Overlay se zavřel a emulovaná klávesnice čeká na uvolnění všech kláves (imgui_overlay_blocks_keyboard()). */
static bool s_wait_keys_released = false;
/** @brief Overlay se zavřel a joystick stroje čeká na klid hostitelského joysticku (imgui_overlay_blocks_joystick()). */
static bool s_wait_joy_released = false;
/** @brief Overlay se zavřel a hlavní okno si má vzít fokus zpět (imgui_overlay_take_main_focus_request()). */
static bool s_main_focus_request = false;
/** @brief Hlavní okno má otevřít klasické menu (imgui_overlay_take_classic_menu_request()). */
static bool s_classic_menu_request = false;
/** @brief Hlavní okno má otevřít overlay menu (imgui_overlay_take_open_request()). */
static bool s_open_request = false;
/** @brief Kopie stavu plněná v imgui_overlay_frame(). */
static st_OVERLAY_VIEW s_view;

/**
 * @brief Prvky rámce, jejichž obdélník se zaznamenává pro testy (imgui_overlay_test_item_rect()).
 *
 * Taby mají index OVL_TEST_ITEM_TAB0 + tab.
 */
typedef enum en_OVL_TEST_ITEM {
    OVL_TEST_ITEM_CLOSE = 0,     /**< tlačítko Close ("close") */
    OVL_TEST_ITEM_CHIP_PLATFORM, /**< stavový štítek platformy ("chip_platform") */
    OVL_TEST_ITEM_CHIP_SPEED,    /**< stavový štítek rychlosti ("chip_speed") */
    OVL_TEST_ITEM_CHIP_PAUSED,   /**< stavový štítek pauzy ("chip_paused") */
    OVL_TEST_ITEM_CHIP_MUTED,    /**< stavový štítek ztlumení ("chip_muted") */
    OVL_TEST_ITEM_FOOTER_HINTS,  /**< nápověda kláves v patičce jako celek ("footer_hints") */
    OVL_TEST_ITEM_FOOTER_RIGHT,  /**< text u pravého okraje patičky ("footer_right") */
    OVL_TEST_ITEM_CONTENT,       /**< plocha obsahu tabu ("content") */
    OVL_TEST_ITEM_TAB0,          /**< první tab ("tab0"); další následují */
    OVL_TEST_ITEM_COUNT = OVL_TEST_ITEM_TAB0 + OVERLAY_TAB_COUNT /**< počet zaznamenávaných prvků */
} en_OVL_TEST_ITEM;

/** @brief Jména prvků před taby v pořadí en_OVL_TEST_ITEM. */
static const char *const s_test_item_names[OVL_TEST_ITEM_TAB0] = {"close", "chip_platform", "chip_speed", "chip_paused", "chip_muted", "footer_hints", "footer_right", "content"};

/**
 * @brief Obdélník prvku z posledního kreslení (imgui_overlay_test_item_rect()).
 */
typedef struct st_OVERLAY_TEST_RECT {
    bool drawn; /**< prvek byl v posledním kreslení vykreslen */
    ImVec2 min; /**< levý horní roh */
    ImVec2 max; /**< pravý dolní roh */
} st_OVERLAY_TEST_RECT;

/** @brief Obdélníky prvků z posledního imgui_overlay_draw() (index en_OVL_TEST_ITEM). */
static st_OVERLAY_TEST_RECT s_test_rects[OVL_TEST_ITEM_COUNT];

#ifdef MZTEST_HEADLESS
/** @brief Největší počet pojmenovaných obdélníků prvků tabu. */
#define OVL_TEST_NAMED_MAX 128

/**
 * @brief Pojmenovaný obdélník prvku tabu z posledního kreslení (overlay_test_note_rect()).
 */
typedef struct st_OVERLAY_TEST_NAMED {
    char name[32]; /**< jméno prvku */
    ImVec2 min;    /**< levý horní roh */
    ImVec2 max;    /**< pravý dolní roh */
} st_OVERLAY_TEST_NAMED;

/** @brief Pojmenované obdélníky prvků tabu z posledního imgui_overlay_draw(). */
static st_OVERLAY_TEST_NAMED s_test_named[OVL_TEST_NAMED_MAX];
/** @brief Počet platných záznamů v s_test_named. */
static int s_test_named_n = 0;
#endif

/**
 * @brief Zaznamená obdélník prvku rámce.
 * @param index Index v s_test_rects.
 * @param min   Levý horní roh.
 * @param max   Pravý dolní roh.
 */
static void overlay_note_rect(int index, ImVec2 min, ImVec2 max)
{
    s_test_rects[index].drawn = true;
    s_test_rects[index].min = min;
    s_test_rects[index].max = max;
}

/**
 * @brief Zaznamená obdélník naposledy vykresleného prvku ImGui.
 * @param index Index v s_test_rects.
 */
static void overlay_note_item(int index)
{
    overlay_note_rect(index, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void overlay_test_note_rect(const char *name, ImVec2 min, ImVec2 max)
{
#ifdef MZTEST_HEADLESS
    if (!name || s_test_named_n >= OVL_TEST_NAMED_MAX) return;
    st_OVERLAY_TEST_NAMED *r = &s_test_named[s_test_named_n++];
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->min = min;
    r->max = max;
#else
    (void)name;
    (void)min;
    (void)max;
#endif
}

void overlay_test_note_item(const char *name)
{
    overlay_test_note_rect(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

#ifdef MZTEST_HEADLESS
/** @brief Testy: prodlužovat přeložené texty tabů o 40 % (overlay_test_set_stretch()). */
static bool s_test_stretch = false;

void overlay_test_set_stretch(bool on)
{
    s_test_stretch = on;
}

/**
 * @brief Prodlouží přeložený text o 40 % (písmeny a číslicemi z textu samotného) a uloží ho do mezipaměti.
 * @param key        Klíč mezipaměti (klíč katalogu včetně případného kontextu).
 * @param translated Přeložený text.
 * @return Prodloužený text; platný do konce procesu.
 */
static const char *overlay_test_stretch(const char *key, const char *translated)
{
    static std::map<std::string, std::string> cache;
    std::map<std::string, std::string>::iterator it = cache.find(key);
    if (it != cache.end()) return it->second.c_str();
    std::string s(translated);
    std::string pool;
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) chars++;
        /* jen písmena a číslice mimo formátovací značky (znak za '%' se nebere) */
        if (((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && (i == 0 || s[i - 1] != '%')) pool += (char)c;
    }
    if (pool.empty()) pool = "x";
    const size_t add = (chars * 4 + 9) / 10;
    for (size_t i = 0; i < add; i++) s += pool[i % pool.size()];
    return cache.emplace(key, s).first->second.c_str();
}

const char *overlay_test_tr(const char *msgid)
{
    const char *translated = gettext(msgid);
    if (!s_test_stretch) return translated;
    return overlay_test_stretch(msgid, translated);
}

const char *overlay_test_trc(const char *ctx_msgid, const char *msgid)
{
    const char *translated = gettext(ctx_msgid);
    if (translated == ctx_msgid) translated = msgid;
    if (!s_test_stretch) return translated;
    return overlay_test_stretch(ctx_msgid, translated);
}
#endif

bool overlay_default_focus_wanted(void)
{
    return s_default_focus_frames > 0;
}

void overlay_focus_last_item(void)
{
    ImGuiContext &g = *GImGui;
    if (g.LastItemData.ID == 0) return;
    ImGui::SetFocusID(g.LastItemData.ID, g.CurrentWindow);
    ImGui::SetNavCursorVisible(true);
}

void overlay_focus_item(ImGuiID id)
{
    if (id == 0) return;
    ImGui::SetFocusID(id, GImGui->CurrentWindow);
    ImGui::SetNavCursorVisible(true);
}

/**
 * @brief Smí teď poslech trvat?
 *
 * Jen na tabu Sound a jen když nad ním není výsuvný list ani rozbalený
 * výběr; jinde se přepínač Listen sám vypíná.
 *
 * @return true = zapnutý přepínač Listen platí (overlay_sync_listen()).
 */
static bool overlay_listen_place_ok(void)
{
    return s_tab == OVERLAY_TAB_SOUND && !overlay_sheet_is_open() && !ovl_dropdown_is_open();
}

/**
 * @brief Zvolí tab a požádá ho o fokus výchozího prvku.
 *
 * Změna tabu vypíná přepínač Listen; emulaci, která kvůli němu běží,
 * zastaví až příští imgui_overlay_frame() (overlay_sync_listen()).
 *
 * @param tab Platný tab.
 */
static void overlay_select_tab(en_OVERLAY_TAB tab)
{
    if (tab != s_tab) {
        s_default_focus_frames = OVL_DEFAULT_FOCUS_FRAMES;
        /* na tab Sound se vždy přichází s vypnutým poslechem */
        s_listen_wanted = false;
    }
    s_tab = tab;
}

/* ================================================================
 * Otevření, zavření, pauza
 * ================================================================ */

bool imgui_overlay_is_open(void)
{
    return s_open;
}

bool imgui_overlay_blocks_keyboard(void)
{
    if (s_open) return true;
    if (!s_wait_keys_released) return false;
    if (ImGui::GetCurrentContext()) {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; k++)
            if (ImGui::IsKeyDown((ImGuiKey)k)) return true;
    }
    s_wait_keys_released = false;
    return false;
}

bool imgui_overlay_blocks_joystick(void)
{
    if (s_open) return true;
    if (!s_wait_joy_released) return false;
    for (int d = 0; d < JOY_DEVID_COUNT; d++) {
        if (g_joy.dev[d].type == JOY_TYPE_JOYSTICK && joy_host_scan_raw((en_JOY_DEVID)d) != 0xff) return true;
    }
    s_wait_joy_released = false;
    joy_host_hold(false);
    return false;
}

bool imgui_overlay_take_main_focus_request(void)
{
    const bool request = s_main_focus_request;
    s_main_focus_request = false;
    return request;
}

void imgui_overlay_open(void)
{
    if (s_open) return;
    s_open = true;
    overlay_sheet_reset();
    s_focus_request = true;
    s_main_focus_request = false;

    /* emulovaná klávesnice a joystick z numerické klávesnice: žádná držená klávesa (stejná cesta jako ztráta
     * fokusu hlavního okna); hostitelský joystick čte stroj po dobu otevření v klidu, i když emulace běží */
    main_window_keyboard_release();
    joy_host_hold(true);
    s_wait_joy_released = false;

    /* pauza: běžící emulaci zastavit a zapamatovat, že pauza je naše */
    s_run_on_close = false;
    s_listen_wanted = false;
    s_listen_run = false;
    if (s_run_retry_attempts > 0) {
        /* minulé zavření emulaci ještě nerozběhlo: stojí kvůli overlayi, ne kvůli uživateli */
        s_pause_is_ours = EMULATOR_TEST_PAUSED;
    } else if (EMULATOR_TEST_PAUSED) {
        s_pause_is_ours = false;
    } else {
        /* výsledek volání nerozhoduje: po vypršení čekání se příkaz ruší a emulace běží dál pod menu,
         * proto se čte skutečný stav (horní lišta pak štítek Paused neukáže a zavření nic nerozbíhá) */
        (void)dbg_ui_pause();
        s_pause_is_ours = EMULATOR_TEST_PAUSED;
    }
    /* číslo běhu, který pauzou skončil: jiné číslo při zavření = emulaci mezitím někdo rozběhl nebo krokoval */
    s_pause_run_seq = run_log_current(NULL);
    /* čítače teď patří poslechu (imgui_overlay_set_listen()), ne opakování po zavření */
    s_run_retry_attempts = 0;
    s_run_retry_wait = 0;
    s_frame_work = true;
    s_default_focus_frames = OVL_DEFAULT_FOCUS_FRAMES;

    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx) {
        s_open_frame = ImGui::GetFrameCount();
        /* Ctrl+Tab v overlayi přepíná taby; přepínání oken ImGui by fokus odneslo jinam. Vypnutí platí hned;
         * dál ho drží a po zavření o něm rozhoduje hlavní okno (main_window_nav_windowing_allowed()) */
        main_window_nav_windowing_set(false);
    } else {
        s_open_frame = -1;
    }
}

/**
 * @brief Čeká nebo běží uložení snapshotu (Quick Save, Save Snapshot...)?
 *
 * Od vyžádání (příznak klasického menu, který hlavní okno vyzvedne ještě v
 * tomtéž snímku) do vyřízení požadavku emu vláknem. Otevřený dialog Save
 * Snapshot sem nepatří: je modální a overlay pod ním zavřít nejde.
 *
 * @return true = emulace musí zůstat stát, dokud uložení neskončí.
 * @pre UI vlákno.
 */
static bool overlay_snapshot_in_progress(void)
{
    return snapshot_ui_is_quicksave_requested() || snapshot_ui_is_save_requested() || snapshot_request_pending() > 0;
}

void imgui_overlay_close(void)
{
    if (!s_open) return;
    s_open = false;
    overlay_sheet_reset();
    s_focus_request = false;
    s_wait_keys_released = true;
    s_wait_joy_released = true;
    s_main_focus_request = true;

    /* rozběhnout jen emulaci, kterou zastavil overlay a nikdo ji mezitím nezměnil, nebo po akci Resume;
     * emulace, která běží kvůli poslechu na tabu Sound (s_listen_run), běží dál (běžela i před otevřením)
     * a zastavená někým jiným zůstane stát */
    s_listen_wanted = false;
    s_listen_run = false;
    const bool run = s_run_on_close || (s_pause_is_ours && run_log_current(NULL) == s_pause_run_seq);
    s_run_on_close = false;
    s_pause_is_ours = false;
    s_run_retry_attempts = 0;
    if (run && EMULATOR_TEST_PAUSED && overlay_snapshot_in_progress()) {
        /* Uložení snapshotu vyžádané z overlaye ještě neproběhlo (vyřizuje ho emu vlákno jen v pauze a běh po
         * něm obnovuje jen podle stavu před svou pauzou, tedy "stála"): emulaci nerozbíhat, rozběhne ji
         * dokončení nebo zrušení uložení (review O5, bod Quick Save a hned Esc). */
        snapshot_ui_run_after_request();
    } else if (run && EMULATOR_TEST_PAUSED && !dbg_ui_run() && EMULATOR_TEST_PAUSED) {
        /* rozběhnutí se nepovedlo (emu vlákno neodpovědělo včas): zkusit znovu v dalších snímcích */
        s_run_retry_attempts = OVL_RUN_RETRY_ATTEMPTS;
        s_run_retry_wait = OVL_RUN_RETRY_INTERVAL_FRAMES;
    }
    s_frame_work = s_run_retry_attempts > 0;
}

/**
 * @brief Opakuje po zavření nepovedené rozběhnutí emulace (jeden krok za snímek UI).
 *
 * Pokus se dělá jednou za OVL_RUN_RETRY_INTERVAL_FRAMES snímků, nejvýš
 * OVL_RUN_RETRY_ATTEMPTS krát; skončí, jakmile emulace běží. Po vyčerpání
 * pokusů zůstane emulace stát a na standardní chybový výstup jde varování.
 *
 * @pre Overlay je zavřený a s_run_retry_attempts > 0.
 * @post Při skončení je s_frame_work false.
 */
static void overlay_retry_run(void)
{
    if (EMULATOR_TEST_PAUSED) {
        if (--s_run_retry_wait > 0) return;
        s_run_retry_wait = OVL_RUN_RETRY_INTERVAL_FRAMES;
        s_run_retry_attempts--;
        if (!dbg_ui_run() && EMULATOR_TEST_PAUSED) {
            if (s_run_retry_attempts > 0) return;
            fprintf(stderr, "Overlay menu: the emulation could not be resumed after closing the menu.\n");
        }
    }
    s_run_retry_attempts = 0;
    s_frame_work = false;
}

/**
 * @brief Srovná běh emulace s přepínačem Listen tabu Sound (jeden krok za snímek UI).
 *
 * Staví na vlastnictví pauzy overlaye (s_pause_is_ours), jiný stav nezavádí:
 * - přepínač je zapnutý a pauza je overlaye: emulaci rozběhne
 *   (dbg_ui_run()) a pauzu pustí (s_listen_run). Čeká-li uložení snapshotu
 *   vyžádané z overlaye (overlay_snapshot_in_progress(); vyřizuje se jen
 *   v pauze), počká na jeho konec. Nepovedené rozběhnutí opakuje jako po
 *   zavření: nejvýš OVL_RUN_RETRY_ATTEMPTS krát po
 *   OVL_RUN_RETRY_INTERVAL_FRAMES snímcích od zapnutí přepínače; potom
 *   přepínač vypne a emulace zůstane stát jako pauza overlaye (zavření ji
 *   rozběhne obvyklou cestou).
 * - přepínač je vypnutý (uživatelem, změnou tabu, výsuvným listem nebo
 *   rozbaleným výběrem) a emulace běží kvůli poslechu: znovu ji zastaví
 *   (dbg_ui_pause()) a podle skutečného stavu si pauzu převezme i s novým
 *   číslem běhu.
 * - emulace, která běžela kvůli poslechu, stojí (zastavil ji někdo jiný:
 *   MCP, debugger; pauzu uživatele hlásí imgui_overlay_note_pause_changed()):
 *   přepínač se vypne, pauza overlaye není a zavření nic nerozbíhá.
 *   Zastavilo-li si ji uložení snapshotu, přepínač se vypne také, ale
 *   overlay počká na konec uložení (po něm emulace zase běží) a pak ji
 *   zastaví a pauzu si převezme; jinak by emulace běžela pod menu bez
 *   poslechu.
 * - přepínač je zapnutý, ale pauza overlaye není (někdo mezitím emulaci
 *   rozběhl nebo krokoval): přepínač se vypne.
 *
 * @pre UI vlákno, overlay je otevřený; s_pause_is_ours už je v tomto snímku
 *      ověřené proti stavu a číslu běhu jádra; volající nedrží device_state_lock()
 *      (dbg_ui_run() a dbg_ui_pause() čekají na emu vlákno).
 * @post s_listen_run a s_pause_is_ours nejsou obě true; s_listen_wanted platí
 *       jen s pauzou overlaye nebo s s_listen_run.
 */
static void overlay_sync_listen(void)
{
    const bool paused = EMULATOR_TEST_PAUSED;
    if (s_listen_wanted && !overlay_listen_place_ok()) s_listen_wanted = false;
    if (s_listen_run) {
        if (paused) {
            /* emulaci zastavil někdo jiný */
            s_listen_wanted = false;
            if (overlay_snapshot_in_progress()) return;
            s_listen_run = false;
            return;
        }
        if (s_listen_wanted) return;
        s_listen_run = false;
        /* výsledek volání nerozhoduje, čte se skutečný stav (jako při otevření) */
        (void)dbg_ui_pause();
        s_pause_is_ours = EMULATOR_TEST_PAUSED;
        s_pause_run_seq = run_log_current(NULL);
        return;
    }
    if (!s_listen_wanted) return;
    if (!s_pause_is_ours || !paused) {
        s_listen_wanted = false;
        return;
    }
    if (overlay_snapshot_in_progress()) return;
    if (s_run_retry_wait > 0) {
        s_run_retry_wait--;
        return;
    }
    if (!dbg_ui_run() && EMULATOR_TEST_PAUSED) {
        if (--s_run_retry_attempts <= 0) s_listen_wanted = false;
        else s_run_retry_wait = OVL_RUN_RETRY_INTERVAL_FRAMES;
        return;
    }
    s_pause_is_ours = false;
    s_listen_run = true;
}

/**
 * @brief Stav přepínače Listen pro kopii stavu (st_OVERLAY_VIEW_SOUND::listen).
 * @return Stav podle skutečné pauzy, vlastnictví pauzy a přepínače.
 * @pre UI vlákno, overlay je otevřený. Kopie stavu ho dostává po overlay_sync_listen() v tomtéž snímku.
 */
static en_OVERLAY_LISTEN overlay_listen_state(void)
{
    if (!EMULATOR_TEST_PAUSED) return (s_listen_run && s_listen_wanted) ? OVERLAY_LISTEN_ON : OVERLAY_LISTEN_BLOCKED_RUNNING;
    if (!s_pause_is_ours) return OVERLAY_LISTEN_BLOCKED_PAUSED;
    return s_listen_wanted ? OVERLAY_LISTEN_ON : OVERLAY_LISTEN_OFF;
}

void imgui_overlay_set_listen(bool on)
{
    if (!s_open) return;
    if (!on) {
        s_listen_wanted = false;
        return;
    }
    if (s_listen_wanted || s_listen_run) return;
    /* jen nad pauzou overlaye: cizí pauzu overlay nerozbíhá a běžící emulaci neřídí */
    if (!overlay_listen_place_ok() || !s_pause_is_ours || !EMULATOR_TEST_PAUSED) return;
    s_listen_wanted = true;
    s_run_retry_attempts = OVL_RUN_RETRY_ATTEMPTS;
    s_run_retry_wait = 0;
}

int imgui_overlay_listen_state(void)
{
    return s_open ? (int)overlay_listen_state() : (int)OVERLAY_LISTEN_OFF;
}

void imgui_overlay_toggle(void)
{
    if (s_open) imgui_overlay_close();
    else imgui_overlay_open();
}

void imgui_overlay_note_pause_changed(bool paused)
{
    if (!s_open) return;
    s_run_on_close = !paused;
    if (paused) s_pause_is_ours = false;
    /* o běhu rozhodl uživatel: emulace už neběží (ani nestojí) kvůli poslechu a přepínač Listen ji neřídí */
    s_listen_wanted = false;
    s_listen_run = false;
}

void imgui_overlay_request_classic_menu(void)
{
    s_classic_menu_request = true;
}

bool imgui_overlay_take_classic_menu_request(void)
{
    const bool request = s_classic_menu_request;
    s_classic_menu_request = false;
    /* OpenPopup() klasického menu by zavřel modál, který se mezitím otevřel odjinud: žádost se zahodí (review Y3) */
    return request && imgui_modal_can_open();
}

void imgui_overlay_request_open(void)
{
    if (s_open) return;
    s_open_request = true;
}

bool imgui_overlay_take_open_request(void)
{
    const bool request = s_open_request;
    s_open_request = false;
    /* popup, který by zůstal pod overlayem (nezavřené klasické menu, cizí modál): žádost se zahodí */
    return request && !s_open && imgui_modal_can_open();
}

void imgui_overlay_request_focus(void)
{
    if (!s_open) return;
    s_focus_request = true;
}

void imgui_overlay_set_tab(en_OVERLAY_TAB tab)
{
    if ((int)tab < 0 || (int)tab >= (int)OVERLAY_TAB_COUNT) return;
    overlay_select_tab(tab);
}

en_OVERLAY_TAB imgui_overlay_get_tab(void)
{
    return s_tab;
}

bool imgui_overlay_test_item_rect(const char *name, ImVec2 *min, ImVec2 *max)
{
    if (!name) return false;
    int index = -1;
    if (strncmp(name, "tab", 3) == 0 && name[3] >= '0' && name[3] < '0' + (int)OVERLAY_TAB_COUNT && name[4] == '\0') {
        index = OVL_TEST_ITEM_TAB0 + (name[3] - '0');
    } else {
        for (int i = 0; i < OVL_TEST_ITEM_TAB0; i++)
            if (strcmp(name, s_test_item_names[i]) == 0) index = i;
    }
#ifdef MZTEST_HEADLESS
    if (index < 0) {
        for (int i = 0; i < s_test_named_n; i++) {
            if (strcmp(name, s_test_named[i].name) != 0) continue;
            if (min) *min = s_test_named[i].min;
            if (max) *max = s_test_named[i].max;
            return true;
        }
    }
#endif
    if (index < 0 || !s_test_rects[index].drawn) return false;
    if (min) *min = s_test_rects[index].min;
    if (max) *max = s_test_rects[index].max;
    return true;
}

/* ================================================================
 * Kreslení
 * ================================================================ */

/**
 * @brief Text stavového štítku rychlosti.
 * @param v    Kopie stavu.
 * @param buf  Výstupní buffer.
 * @param size Velikost bufferu.
 */
static void overlay_speed_text(const st_OVERLAY_VIEW *v, char *buf, size_t size)
{
    if (v->speed_mode == OVERLAY_SPEED_MAX) snprintf(buf, size, "%s", _("Max speed"));
    else snprintf(buf, size, "%u %%", v->speed_percent);
}

/**
 * @brief Šířka textu daným písmem a návrhovou velikostí.
 * @param font   Písmo.
 * @param design Velikost písma v návrhových bodech.
 * @param text   Text.
 * @return Šířka v pixelech.
 */
static float overlay_text_width(ImFont *font, float design, const char *text)
{
    ImGui::PushFont(font, overlay_font_size(font, design));
    const float w = ImGui::CalcTextSize(text, NULL, false).x;
    ImGui::PopFont();
    return w;
}

/**
 * @brief Vykreslí text daným písmem svisle vystředěný na ose.
 * @param dl     Seznam kreslení.
 * @param font   Písmo.
 * @param design Velikost písma v návrhových bodech.
 * @param x      Levý okraj textu.
 * @param cy     Svislý střed textu.
 * @param col    Barva.
 * @param text   Text.
 */
static void overlay_text_centered_y(ImDrawList *dl, ImFont *font, float design, float x, float cy, ImU32 col, const char *text)
{
    const float fs = overlay_font_size(font, design);
    dl->AddText(font, fs, ImVec2(floorf(x), floorf(cy - fs * 0.5f)), col, text);
}

/**
 * @brief Šířka tabu v horní liště.
 * @param index   Index tabu.
 * @param compact true = bez popisku (jen číslice a ikona).
 * @return Šířka v pixelech.
 */
static float overlay_tab_width(int index, bool compact)
{
    const char digit[2] = {(char)('1' + index), '\0'};
    float w = 2.0f * overlay_px(OVL_TAB_PAD_X) + ovl_keycap_size(digit).x;
    if (overlay_style_has_icons()) w += overlay_px(OVL_TAB_GAP) + overlay_px(OVL_ICON);
    if (!compact) w += overlay_px(OVL_TAB_GAP) + overlay_text_width(overlay_font_ui_bold(), OVL_FONT_TAB, _(s_tabs[index].label));
    return floorf(w + 0.5f);
}

/**
 * @brief Tab horní lišty: číslice v klávesovém štítku, ikona a popisek; aktivní tab má akcentní podtržení u spodního okraje lišty.
 * @param index   Index tabu.
 * @param compact true = bez popisku.
 * @param pos     Levý horní roh položky.
 * @param size    Velikost položky.
 * @param bar_y1  Spodní okraj horní lišty (souřadnice obrazovky).
 * @return true = tab byl zvolen (klik, Enter nebo mezerník při fokusu).
 * @pre Uvnitř okna overlaye.
 */
static bool overlay_tab_item(int index, bool compact, ImVec2 pos, ImVec2 size, float bar_y1)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    ImDrawList *dl = window->DrawList;
    const bool active = (index == (int)s_tab);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::PushID("ovl_tab");
    const ImGuiID id = window->GetID(index);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, id)) return false;
    overlay_note_item(OVL_TEST_ITEM_TAB0 + index);
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const bool focused = ovl_last_item_focused();
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);

    if (hovered || held) dl->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_PRESSED : OVL_COL_RAISED, rounding);

    const char digit[2] = {(char)('1' + index), '\0'};
    const ImU32 col = active ? OVL_COL_TEXT : OVL_COL_TEXT_SECONDARY;
    const float cy = bb.Min.y + size.y * 0.5f;
    const float gap = overlay_px(OVL_TAB_GAP);
    float x = bb.Min.x + overlay_px(OVL_TAB_PAD_X);
    const ImVec2 kc = ovl_keycap_size(digit);
    ovl_draw_keycap(dl, ImVec2(floorf(x), floorf(cy - kc.y * 0.5f)), digit, false);
    x += kc.x;
    if (overlay_style_has_icons()) {
        const float icon = overlay_px(OVL_ICON);
        x += gap;
        ovl_draw_icon(dl, ImVec2(floorf(x), floorf(cy - icon * 0.5f)), icon, s_tabs[index].icon, col);
        x += icon;
    }
    if (!compact) {
        x += gap;
        overlay_text_centered_y(dl, overlay_font_ui_bold(), OVL_FONT_TAB, x, cy, col, _(s_tabs[index].label));
    }
    if (active) dl->AddRectFilled(ImVec2(bb.Min.x, bar_y1 - overlay_px(OVL_FOCUS_W)), ImVec2(bb.Max.x, bar_y1), OVL_COL_ACCENT);
    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, rounding);
    if (compact && hovered) ovl_tooltip(_(s_tabs[index].label));
    return pressed;
}

/**
 * @brief Tlačítko Close horní lišty: klávesový štítek F10 a popisek.
 * @param with_label true = s popiskem "Close"; false = jen štítek (úzké okno).
 * @param pos        Levý horní roh položky.
 * @param size       Velikost položky.
 * @return true = tlačítko bylo zvoleno.
 * @pre Uvnitř okna overlaye.
 */
static bool overlay_close_item(bool with_label, ImVec2 pos, ImVec2 size)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    ImDrawList *dl = window->DrawList;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    const ImGuiID id = window->GetID("###ovl_close");
    ImGui::SetCursorScreenPos(pos);
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, id)) return false;
    overlay_note_item(OVL_TEST_ITEM_CLOSE);
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const bool focused = ovl_last_item_focused();
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);

    if (hovered || held) dl->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_PRESSED : OVL_COL_RAISED, rounding);
    const float cy = bb.Min.y + size.y * 0.5f;
    float x = bb.Min.x + overlay_px(OVL_TAB_PAD_X);
    const ImVec2 kc = ovl_keycap_size("F10");
    ovl_draw_keycap(dl, ImVec2(floorf(x), floorf(cy - kc.y * 0.5f)), "F10", false);
    x += kc.x + overlay_px(OVL_TAB_GAP);
    if (with_label) overlay_text_centered_y(dl, overlay_font_ui(), OVL_FONT_BODY, x, cy, OVL_COL_TEXT, _("Close"));
    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, rounding);
    if (!with_label && hovered) ovl_tooltip(_("Close"));
    return pressed;
}

/**
 * @brief Přitmaví pruh přes celou šířku overlaye (horní lišta, patička) a nakreslí jeho okraj.
 * @param y0     Horní okraj pruhu (souřadnice obrazovky).
 * @param y1     Spodní okraj pruhu.
 * @param line_y Svislá poloha okrajové linky.
 * @param x0     Levý okraj overlaye.
 * @param x1     Pravý okraj overlaye.
 */
static void overlay_draw_bar(float y0, float y1, float line_y, float x0, float x1)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), overlay_color_alpha(OVL_COL_PANEL, OVL_BAR_EXTRA_OPACITY));
    dl->AddLine(ImVec2(x0, floorf(line_y)), ImVec2(x1, floorf(line_y)), OVL_COL_BORDER, overlay_border_px());
}

/**
 * @brief Horní lišta: taby vlevo, stavové štítky a Close vpravo.
 *
 * Nevejde-li se vše vedle sebe, ustupuje postupně: štítek platformy, štítek
 * rychlosti, popisky neaktivních tabů (zůstane číslice a ikona), text štítku
 * ztlumení (zůstane samotná ikona s tooltipem; jen s ikonovým fontem),
 * popisek tlačítka Close, celý štítek ztlumení, text štítku pauzy (zůstane
 * samotná ikona s tooltipem; jen s ikonovým fontem) a nakonec popisek
 * aktivního tabu. Štítek ztlumení tak zmizí, až když se nevejde ani jeho
 * ikona vedle tabů a Close bez popisků (posudek N04). Poslední dva stupně
 * jsou pro úzké okno s dlouhým překladem (640 x 480: popisek aktivního tabu
 * a štítek pauzy se jinak v češtině a dalších jazycích překryly se šestým
 * tabem). Štítek pauzy a tlačítko Close zůstávají
 * vždy. Štítek "Muted" je vidět jen při zapnutém ztlumení poslechu a
 * připomíná ho na všech tabech (mimo overlay ho ukazuje titulek hlavního
 * okna a okno Audio Mixer; ztlumení se neukládá). Štítek "Paused" se řídí
 * skutečným stavem emulace: na tabu Sound, kde ji overlay nechává běžet,
 * vidět není.
 *
 * @param v           Kopie stavu.
 * @param size        Velikost overlaye.
 * @param[out] close  Nastaví na true při zvolení Close (jinak nemění).
 * @pre Uvnitř okna overlaye se stylem overlay_style_push().
 */
static void overlay_draw_topbar(const st_OVERLAY_VIEW *v, ImVec2 size, bool *close)
{
    const ImVec2 origin = ImGui::GetWindowPos();
    const float bar_h = overlay_px(OVL_TOPBAR_H);
    const float edge = overlay_px(OVL_BAR_EDGE);
    const float group_gap = overlay_px(OVL_BAR_GROUP_GAP);
    const float chip_gap = overlay_px(OVL_CHIP_SPACING);
    const float tab_gap = overlay_px(OVL_TAB_SPACING);
    overlay_draw_bar(origin.y, origin.y + bar_h, origin.y + bar_h, origin.x, origin.x + size.x);

    char speed[64];
    overlay_speed_text(v, speed, sizeof(speed));
    const char *paused = v->paused ? _("Paused") : NULL;
    const bool have_platform = v->platform_text[0] != '\0';
    const ImVec2 chip_platform = have_platform ? ovl_status_chip_size(NULL, v->platform_text) : ImVec2(0.0f, 0.0f);
    const ImVec2 chip_speed = ovl_status_chip_size(NULL, speed);
    const ImVec2 chip_paused = paused ? ovl_status_chip_size(ICON_LC_PAUSE, paused) : ImVec2(0.0f, 0.0f);
    const char *muted = v->sound.mute ? _("Muted") : NULL;
    const ImVec2 chip_muted = muted ? ovl_status_chip_size(ICON_LC_VOLUME_X, muted) : ImVec2(0.0f, 0.0f);
    const float close_kc = 2.0f * overlay_px(OVL_TAB_PAD_X) + ovl_keycap_size("F10").x;
    const float close_full = floorf(close_kc + overlay_px(OVL_TAB_GAP) + overlay_text_width(overlay_font_ui(), OVL_FONT_BODY, _("Close")) + 0.5f);

    /* stupně ústupu: 0 = vše, 1 = bez platformy, 2 = bez rychlosti, 3 = neaktivní taby bez popisku, 4 = ztlumení jen ikonou,
     * 5 = Close bez popisku, 6 = bez ztlumení, 7 = pauza jen ikonou, 8 = i aktivní tab bez popisku */
    const ImVec2 chip_paused_icon = (paused && overlay_style_has_icons()) ? ovl_status_chip_size(ICON_LC_PAUSE, "") : ImVec2(0.0f, 0.0f);
    const ImVec2 chip_muted_icon = (muted && overlay_style_has_icons()) ? ovl_status_chip_size(ICON_LC_VOLUME_X, "") : ImVec2(0.0f, 0.0f);
    const float avail = size.x - 2.0f * edge;
    int level = 0;
    bool show_platform = false, show_speed = false, show_muted = false, muted_icon = false, compact = false, close_label = true;
    bool paused_icon = false, compact_active = false;
    float tabs_w = 0.0f, right_w = 0.0f, close_w = 0.0f;
    for (level = 0; level <= 8; level++) {
        show_platform = have_platform && level < 1;
        show_speed = level < 2;
        muted_icon = (level == 4 || level == 5) && chip_muted_icon.x > 0.0f;
        show_muted = muted && (level < 4 || muted_icon);
        compact = level >= 3;
        close_label = level < 5;
        paused_icon = level >= 7 && chip_paused_icon.x > 0.0f;
        compact_active = level >= 8;
        tabs_w = tab_gap * (float)((int)OVERLAY_TAB_COUNT - 1);
        for (int i = 0; i < (int)OVERLAY_TAB_COUNT; i++) tabs_w += overlay_tab_width(i, compact && (compact_active || i != (int)s_tab));
        close_w = close_label ? close_full : floorf(close_kc + 0.5f);
        float chips_w = 0.0f;
        int chips = 0;
        if (show_platform) { chips_w += chip_platform.x; chips++; }
        if (show_speed) { chips_w += chip_speed.x; chips++; }
        if (show_muted) { chips_w += muted_icon ? chip_muted_icon.x : chip_muted.x; chips++; }
        if (paused) { chips_w += paused_icon ? chip_paused_icon.x : chip_paused.x; chips++; }
        if (chips > 1) chips_w += chip_gap * (float)(chips - 1);
        right_w = close_w + (chips > 0 ? chips_w + group_gap : 0.0f);
        if (tabs_w + group_gap + right_w <= avail) break;
    }

    /* taby */
    const float item_y = origin.y + overlay_px(OVL_TAB_INSET_Y);
    const float item_h = bar_h - 2.0f * overlay_px(OVL_TAB_INSET_Y);
    float x = origin.x + edge;
    for (int i = 0; i < (int)OVERLAY_TAB_COUNT; i++) {
        const bool tab_compact = compact && (compact_active || i != (int)s_tab);
        const float w = overlay_tab_width(i, tab_compact);
        if (overlay_tab_item(i, tab_compact, ImVec2(x, item_y), ImVec2(w, item_h), origin.y + bar_h)) overlay_select_tab(s_tabs[i].tab);
        x += w + tab_gap;
    }

    /* stavové štítky a Close, zarovnané vpravo */
    const float cy = origin.y + bar_h * 0.5f;
    x = origin.x + size.x - edge - right_w;
    if (right_w > close_w && x - group_gap * 0.5f > origin.x + edge + tabs_w) {
        const float lx = floorf(x - group_gap * 0.5f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(lx, origin.y + overlay_px(14.0f)), ImVec2(lx, origin.y + bar_h - overlay_px(14.0f)), OVL_COL_BORDER,
                                            overlay_border_px());
    }
    if (show_platform) {
        ImGui::SetCursorScreenPos(ImVec2(floorf(x), floorf(cy - chip_platform.y * 0.5f)));
        ovl_status_chip(v->platform_text, OVL_STATUS_NEUTRAL);
        overlay_note_item(OVL_TEST_ITEM_CHIP_PLATFORM);
        x += chip_platform.x + chip_gap;
    }
    if (show_speed) {
        ImGui::SetCursorScreenPos(ImVec2(floorf(x), floorf(cy - chip_speed.y * 0.5f)));
        ovl_status_chip(speed, OVL_STATUS_NEUTRAL);
        overlay_note_item(OVL_TEST_ITEM_CHIP_SPEED);
        x += chip_speed.x + chip_gap;
    }
    if (show_muted) {
        ImGui::SetCursorScreenPos(ImVec2(floorf(x), floorf(cy - chip_muted.y * 0.5f)));
        ovl_status_chip_icon(ICON_LC_VOLUME_X, muted_icon ? "" : muted, OVL_STATUS_WARNING);
        overlay_note_item(OVL_TEST_ITEM_CHIP_MUTED);
        if (muted_icon) ovl_tooltip(muted);
        x += (muted_icon ? chip_muted_icon.x : chip_muted.x) + chip_gap;
    }
    if (paused) {
        ImGui::SetCursorScreenPos(ImVec2(floorf(x), floorf(cy - chip_paused.y * 0.5f)));
        ovl_status_chip_icon(ICON_LC_PAUSE, paused_icon ? "" : paused, OVL_STATUS_WARNING);
        overlay_note_item(OVL_TEST_ITEM_CHIP_PAUSED);
        if (paused_icon) ovl_tooltip(paused);
    }
    if (overlay_close_item(close_label, ImVec2(floorf(origin.x + size.x - edge - close_w), item_y), ImVec2(close_w, item_h))) *close = true;
}

/**
 * @brief Změří a volitelně vykreslí jednu nápovědu patičky.
 * @param hint Nápověda.
 * @param x    Levý okraj.
 * @param cy   Svislý střed.
 * @param draw true = kreslit; false = jen měřit.
 * @return Šířka nápovědy v pixelech.
 */
static float overlay_footer_hint(const st_OVERLAY_HINT *hint, float x, float cy, bool draw)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = overlay_font_ui();
    const float gap = overlay_px(OVL_HINT_GAP);
    float w = 0.0f;
    ImVec2 kc = ovl_keycap_size(hint->key1);
    if (draw) ovl_draw_keycap(dl, ImVec2(floorf(x + w), floorf(cy - kc.y * 0.5f)), hint->key1, false);
    w += kc.x + gap;
    if (hint->joiner && hint->key2) {
        if (draw) overlay_text_centered_y(dl, font, OVL_FONT_HINT, x + w, cy, OVL_COL_TEXT_SECONDARY, hint->joiner);
        w += overlay_text_width(font, OVL_FONT_HINT, hint->joiner) + gap;
        kc = ovl_keycap_size(hint->key2);
        if (draw) ovl_draw_keycap(dl, ImVec2(floorf(x + w), floorf(cy - kc.y * 0.5f)), hint->key2, false);
        w += kc.x + gap;
    }
    if (draw) overlay_text_centered_y(dl, font, OVL_FONT_HINT, x + w, cy, OVL_COL_TEXT_SECONDARY, hint->action);
    w += overlay_text_width(font, OVL_FONT_HINT, hint->action);
    return w;
}

/**
 * @brief Patička: nápověda kláves vlevo, odkaz na klasické menu vpravo.
 *
 * Při otevřeném výsuvném listu ukazuje nápovědu listu
 * (overlay_sheet_footer_hints()): klávesy tabů a akcí tehdy nefungují.
 * Nevejde-li se vše, ustoupí nejdřív text vpravo, potom nápovědy od konce.
 *
 * @param size Velikost overlaye.
 * @pre Uvnitř okna overlaye se stylem overlay_style_push().
 */
static void overlay_draw_footer(ImVec2 size)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float foot_h = overlay_px(OVL_FOOTER_H);
    const float top = origin.y + size.y - foot_h;
    const float edge = overlay_px(OVL_BAR_EDGE);
    const float spacing = overlay_px(OVL_HINT_SPACING);
    ImFont *font = overlay_font_ui();
    overlay_draw_bar(top, origin.y + size.y, top, origin.x, origin.x + size.x);

    const st_OVERLAY_HINT frame_hints[] = {
        {"1", C_("overlay footer", "to"), "6", _("Tabs")},
        {_("Arrows"), NULL, NULL, _("Move")},
        {"Enter", NULL, NULL, _("Select")},
        {"Esc", NULL, NULL, _("Close")},
    };
    st_OVERLAY_HINT sheet_hints[OVERLAY_SHEET_HINTS_MAX];
    const int sheet_count = overlay_sheet_footer_hints(sheet_hints);
    const st_OVERLAY_HINT *hints = (sheet_count > 0) ? sheet_hints : frame_hints;
    const int hint_count = (sheet_count > 0) ? sheet_count : (int)(sizeof(frame_hints) / sizeof(frame_hints[0]));
    static const char dot[] = "\xC2\xB7"; /* U+00B7 */
    const float dot_w = overlay_text_width(font, OVL_FONT_HINT, dot);
    const float cy = top + foot_h * 0.5f;

    float hints_w = 0.0f;
    for (int i = 0; i < hint_count; i++) hints_w += overlay_footer_hint(&hints[i], 0.0f, cy, false) + (i > 0 ? 2.0f * spacing + dot_w : 0.0f);
    const char *right = _("Right-click: classic menu");
    const float right_w = overlay_text_width(font, OVL_FONT_HINT, right);
    const float x_max = origin.x + size.x - edge;
    const bool show_right = hints_w + overlay_px(OVL_BAR_GROUP_GAP) + right_w <= size.x - 2.0f * edge;

    float x = origin.x + edge;
    const float x0 = x;
    for (int i = 0; i < hint_count; i++) {
        const float w = overlay_footer_hint(&hints[i], 0.0f, cy, false);
        const float lead = (i > 0) ? 2.0f * spacing + dot_w : 0.0f;
        if (x + lead + w > x_max) break;
        if (i > 0) {
            overlay_text_centered_y(dl, font, OVL_FONT_HINT, x + spacing, cy, OVL_COL_TEXT_SECONDARY, dot);
            x += lead;
        }
        overlay_footer_hint(&hints[i], x, cy, true);
        x += w;
    }
    const float fs = overlay_font_px(OVL_FONT_HINT);
    if (x > x0) overlay_note_rect(OVL_TEST_ITEM_FOOTER_HINTS, ImVec2(x0, top), ImVec2(x, origin.y + size.y));
    if (show_right) {
        overlay_text_centered_y(dl, font, OVL_FONT_HINT, x_max - right_w, cy, OVL_COL_TEXT_SECONDARY, right);
        overlay_note_rect(OVL_TEST_ITEM_FOOTER_RIGHT, ImVec2(floorf(x_max - right_w), floorf(cy - fs * 0.5f)), ImVec2(x_max, floorf(cy - fs * 0.5f) + fs));
    }
}

/**
 * @brief Obslouží klávesy overlaye (zavření, přepínání tabů, zkratky akcí).
 *
 * Při otevřeném výsuvném listu obslouží jen F10 (zavře overlay i s listem):
 * Esc, číslice a znaky patří listu (overlay_sheet_draw()) a zkratky tabů a
 * akcí jsou vypnuté. Ve snímku, kdy se zavřel rozbalený seznam
 * rozbalovacího výběru, neobslouží nic: Esc, které seznam zavřelo, nesmí
 * zavřít i overlay.
 *
 * @param v          Kopie stavu (dostupnost akcí).
 * @param[out] close Nastaví na true při F10 nebo Esc (jinak nemění).
 * @pre Uvnitř okna overlaye, které má fokus; ne ve snímku otevření.
 */
static void overlay_handle_keys(const st_OVERLAY_VIEW *v, bool *close)
{
    const ImGuiIO &io = ImGui::GetIO();
    const bool no_mods = !io.KeyCtrl && !io.KeyAlt && !io.KeySuper;
    if (overlay_sheet_is_open()) {
        if (no_mods && ImGui::IsKeyPressed(ImGuiKey_F10, false)) *close = true;
        return;
    }
    if (io.WantTextInput) return;
    if (ovl_dropdown_is_open()) return;

    if (no_mods && (ImGui::IsKeyPressed(ImGuiKey_F10, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        *close = true;
        return;
    }
    if (no_mods && !io.KeyShift) {
        for (int i = 0; i < (int)OVERLAY_TAB_COUNT; i++) {
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false) || ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad1 + i), false))
                overlay_select_tab(s_tabs[i].tab);
        }
    }
    if (io.KeyCtrl && !io.KeyAlt && !io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        const int n = (int)OVERLAY_TAB_COUNT;
        overlay_select_tab((en_OVERLAY_TAB)(((int)s_tab + (io.KeyShift ? n - 1 : 1)) % n));
    }
    /* zkratky akcí: stejné kombinace jako globální zkratky, které jsou při otevřeném overlayi vypnuté */
    overlay_actions_handle_shortcuts(v);
}

void imgui_overlay_draw(const st_OVERLAY_VIEW *v, ImVec2 pos, ImVec2 size)
{
    for (int i = 0; i < OVL_TEST_ITEM_COUNT; i++) s_test_rects[i].drawn = false;
#ifdef MZTEST_HEADLESS
    s_test_named_n = 0;
#endif
    if (!v || size.y <= 0.0f || size.x <= 0.0f) return;
    overlay_style_set_viewport(size);
    const float u = overlay_unit();

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    if (s_focus_request) {
        ImGui::SetNextWindowFocus();
        s_focus_request = false;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                   | ImGuiWindowFlags_NoScrollWithMouse;
    bool close = false;

    overlay_style_push();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    /* tab Display: obraz pod overlayem má být vidět (změna barev a scanlines se ukáže hned) */
    ImGui::PushStyleColor(ImGuiCol_WindowBg, overlay_color_alpha(OVL_COL_PANEL, (s_tab == OVERLAY_TAB_DISPLAY) ? OVL_PANEL_OPACITY_PREVIEW : OVL_PANEL_OPACITY));
    const bool visible = ImGui::Begin(OVERLAY_WINDOW_ID, NULL, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (visible) {
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetFrameCount() != s_open_frame;
        if (focused) overlay_handle_keys(v, &close);
        /* Pravé tlačítko myši kdekoli v overlayi: klasické menu, jak slibuje patička. Overlay kryje hlavní okno,
         * které by klik jinak nedostalo; cesta je stejná jako tlačítko "Open classic menu" (zavřít a požádat
         * hlavní okno). Ne nad výsuvným listem, rozbaleným výběrem ani modálem. */
        if (!overlay_sheet_is_open() && !ovl_dropdown_is_open() && imgui_modal_can_open() && ImGui::GetFrameCount() != s_open_frame
            && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            overlay_action_request(OVL_ACTION_CLASSIC_MENU);

        overlay_draw_topbar(v, size, &close);

        /* obsah zvoleného tabu; v menším okně se svisle roluje */
        const float bar_h = overlay_px(OVL_TOPBAR_H);
        const float content_h = size.y - bar_h - overlay_px(OVL_FOOTER_H);
        ImGui::SetCursorPos(ImVec2(0.0f, bar_h));
        /* rozbalený seznam nesmí přes patičku (posudek N10) */
        ovl_dropdown_set_bottom_limit(floorf(ImGui::GetWindowPos().y + bar_h + content_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(overlay_px(OVL_MARGIN), overlay_px(OVL_CONTENT_PAD_Y)));
        const bool content = ImGui::BeginChild("###ovl_content", ImVec2(size.x, content_h),
                                               ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        if (content) s_tabs[s_tab].draw(v, u);
        ImGui::EndChild();
        overlay_note_item(OVL_TEST_ITEM_CONTENT);

        /* výsuvný list výměny média: nad obsahem tabu, pod horní lištou a patičkou */
        overlay_sheet_draw(v, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), focused, s_tab == OVERLAY_TAB_MEDIA);

        overlay_draw_footer(size);
    }
    ImGui::End();
    overlay_style_pop();
    if (s_default_focus_frames > 0) s_default_focus_frames--;

    /* akce vyžádaná prvkem nebo zkratkou a vložení média z listu: až po vykreslení, mimo okno overlaye */
    overlay_actions_run_pending(v);
    overlay_sheet_run_pending(v);
    if (close) imgui_overlay_close();
}

void imgui_overlay_frame(void)
{
    if (!s_frame_work) return;
    if (!s_open) {
        overlay_retry_run();
        return;
    }

    /* emulaci mezitím rozběhl nebo krokoval někdo jiný (MCP, debugger): pauza už není naše a zavření nic nerozbíhá */
    if (s_pause_is_ours && (!EMULATOR_TEST_PAUSED || run_log_current(NULL) != s_pause_run_seq)) s_pause_is_ours = false;
    /* poslech na tabu Sound: emulace běží jen při zapnutém přepínači Listen; před plněním kopie, aby štítek
     * Paused a tab Sound odpovídaly skutečnosti už v tomto snímku */
    overlay_sync_listen();

    /* jediné místo, kde overlay čte stav jádra pro kreslení */
    device_state_lock();
    overlay_view_fill(&s_view);
    device_state_unlock();
    /* přepínač Listen není stav jádra: doplňuje ho rámec */
    s_view.sound.listen = (int)overlay_listen_state();
    /* série snímků, video záznam a replay: vlastní synchronizace, mimo zámek zařízení jako v klasickém menu */
    overlay_view_fill_capture(&s_view);
    /* nedávná média mimo zámek zařízení; existence souborů jen když jsou vidět (pruh a list jsou na tabu Media) */
    overlay_view_fill_recent(&s_view, s_tab == OVERLAY_TAB_MEDIA || overlay_sheet_is_open());

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    imgui_overlay_draw(&s_view, vp->Pos, vp->Size);
}
