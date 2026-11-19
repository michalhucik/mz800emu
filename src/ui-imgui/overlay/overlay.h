/**
 * @file   overlay.h
 * @brief  Overlay menu: celoobrazovkové menu nad obrazem emulátoru (veřejné API, stav otevření, pauza, rámec).
 *
 * Overlay je obyčejné okno ImGui `###OverlayMenu` (ne popup) přes celý
 * hlavní viewport: horní lišta s taby a stavovými štítky, obsah zvoleného
 * tabu, patička s nápovědou kláves. Otevírá se klávesou F10 bez
 * modifikátorů při fokusu hlavního okna (main_window_overlay_open_key();
 * ne z okna debuggeru) nebo levým Alt + levým tlačítkem myši nad obrazem
 * (main_window_overlay_open_click()), obojí v main_window.cpp; zavírá se
 * F10, Esc nebo tlačítkem Close.
 *
 * Klávesnice: po dobu otevření a po zavření do uvolnění všech kláves
 * hlavní okno emulované klávesnici nic nepředává (main_window.cpp se ptá
 * imgui_overlay_blocks_keyboard()) a při otevření se pošle klidová matice
 * (main_window_keyboard_release()).
 *
 * Joystick emulovaného stroje: po dobu otevření a po zavření do klidu
 * hostitelského joysticku čte stroj joystick v klidu, i když emulace běží
 * (tab Sound). Port typu JOYSTICK zadržuje joy_host_hold(), joystick
 * z numerické klávesnice klidový sken při otevření a zadržení klávesnice
 * (imgui_overlay_blocks_joystick()).
 *
 * Pauza: běžící emulaci overlay při otevření zastaví a při zavření ji
 * rozběhne, pokud ji mezitím nerozběhl a znovu nezastavil někdo jiný
 * (MCP, debugger); emulaci, která stála už před otevřením, nechá stát.
 * Cizí zásah pozná overlay dvojím způsobem: jednou za snímek UI vidí
 * emulaci běžet, nebo se liší číslo běhu jádra (run_log_current(), zvedá
 * ho každý rozběh i krok) od čísla zapamatovaného při otevření - tím se
 * zachytí i krok nebo krátký běh mezi dvěma snímky. Zbývající omezení:
 * požadavek na pauzu, který přijde ve chvíli, kdy emulace už stojí kvůli
 * overlayi (MCP `pause`, pauza z debuggeru), v jádře nezanechá žádnou
 * stopu (DBGAPI_CMD_PAUSE nad stojící emulací nemění číslo běhu ani důvod
 * pauzy), takže zavření overlaye emulaci rozběhne. Odstranit by to šlo jen
 * vlastníkem pauzy v jádře. V buildu bez debuggeru se číslo běhu nevede a
 * platí jen vzorkování jednou za snímek.
 * Nepovede-li se zastavení, emulace běží pod menu dál a zavření nic
 * nerozbíhá; nepovedené rozběhnutí se po zavření několikrát zopakuje.
 * Akce Resume zapamatovaný stav přepíše funkcí
 * imgui_overlay_note_pause_changed(): emulace se rozběhne, i když před
 * otevřením stála. Akce, které si pauzu řídí samy (Quick Load), overlay
 * nejdřív zavřou a pak teprve proběhnou (overlay_actions.h).
 *
 * Tab Sound: emulace na něm stojí jako na ostatních tabech. Aby byla změna
 * hlasitosti, ztlumení, propusti a kanálů slyšet hned, má tab přepínač
 * Listen (imgui_overlay_set_listen()): zapnutý ruší pauzu overlaye a
 * emulace běží, vypnutý ji vrací. Při otevření overlaye i při příchodu na
 * tab je vždy vypnutý. Sám se vypne (a overlay emulaci znovu zastaví a
 * pauzu si převezme) při přepnutí na jiný tab a při otevření výsuvného
 * listu nebo rozbaleného výběru; při zavření overlaye emulace běží dál
 * (běžela i před otevřením). Týká se to jen pauzy overlaye: emulaci, která
 * stála už před otevřením nebo kterou v overlayi zastavil uživatel (Alt+P,
 * přepínač Emulation), poslech nerozbíhá a přepínač je zakázaný; emulaci,
 * kterou uživatel v overlayi rozběhl, přepínač neřídí (ukazuje zapnuto a je
 * zakázaný). Zastaví-li emulaci během poslechu někdo jiný (uživatel, MCP,
 * debugger), přepínač se vypne a pauza overlaye není; po uložení snapshotu,
 * které si emulaci zastavilo a zase rozběhlo samo, se přepínač vypne a
 * overlay emulaci zastaví svou pauzou. Stav přepínače pro kreslení je
 * v kopii stavu (st_OVERLAY_VIEW_SOUND::listen). Klávesnice emulovaného
 * stroje zůstává po celou dobu otevření odpojená (viz výše), i když
 * emulace běží. Štítek Paused v horní liště ukazuje skutečný stav.
 *
 * Zásahy do emulátoru jdou jen přes tabulku akcí (overlay_actions.h):
 * z ní se kreslí tlačítka, registrují zkratky pro okno overlaye a volají
 * stejné funkce jako z klasického menu. Médium z historie naposledy
 * vložených vkládá výsuvný list výměny média (overlay_tabs.h,
 * overlay_insert_sheet.cpp) funkcemi, které volá souborový dialog
 * klasického menu po výběru souboru. Po dobu otevřeného listu rámec
 * obsluhuje jen F10 (zavře overlay i s listem); Esc zavírá list, zkratky
 * tabů a akcí jsou vypnuté a patička ukazuje nápovědu listu.
 *
 * Kreslení: imgui_overlay_draw() kreslí jen z kopie stavu st_OVERLAY_VIEW
 * (overlay_view.h); jádro nečte. Vzhled určuje vizuální systém
 * (overlay_style.h): všechny rozměry jsou násobky jednotky
 * `u = min(šířka / 1280, výška / 720)`, nejméně 0,85; plocha overlaye má
 * nad obrazem krytí 85 %, na tabu Display 55 % (změna barev a scanlines má
 * být vidět hned). V menším okně se obsah tabu svisle roluje a
 * horní lišta ustupuje (štítek platformy, štítek rychlosti, popisky
 * neaktivních tabů, popisek Close); patička vypouští text vpravo a potom
 * nápovědy od konce.
 *
 * @par Vlákna
 * Celé API je jen pro UI vlákno. Zavřený overlay stojí UI jeden test
 * příznaku za snímek; horké cesty emulace se netýká.
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

#ifndef OVERLAY_H
#define OVERLAY_H

#include "libs/imgui/imgui.h"

#include "ui-imgui/overlay/overlay_view.h"

/** @brief Stabilní ID okna overlay menu (ImGui::FindWindowByName()). */
#define OVERLAY_WINDOW_ID "###OverlayMenu"

/** @brief Taby overlay menu v pořadí horní lišty (číslice 1 až 6). */
typedef enum {
    OVERLAY_TAB_MEDIA,   /**< 1 Media: mechaniky, kazeta, Quick Disk (úvodní tab) */
    OVERLAY_TAB_MACHINE, /**< 2 Machine: reset, rychlost, ROM, snapshoty */
    OVERLAY_TAB_DISPLAY, /**< 3 Display: fullscreen, velikost okna, barvy */
    OVERLAY_TAB_SOUND,   /**< 4 Sound: hlasitost, kanály */
    OVERLAY_TAB_INPUT,   /**< 5 Input: virtuální klávesnice, joystick */
    OVERLAY_TAB_CAPTURE, /**< 6 Capture: screenshot, video, replay */
    OVERLAY_TAB_COUNT    /**< počet tabů (není platný tab) */
} en_OVERLAY_TAB;

/**
 * @brief Je overlay menu otevřené?
 * @return true = otevřené (kreslí se a emulovaná klávesnice nic nedostává).
 */
bool imgui_overlay_is_open(void);

/**
 * @brief Má hlavní okno emulované klávesnici zadržet klávesy kvůli overlayi?
 *
 * Ano po dobu otevření a po zavření ještě do chvíle, kdy není stisknutá
 * žádná klávesa klávesnice (klávesa, která overlay zavřela - F10 nebo Esc -
 * je i klávesou stroje nebo zkratkou a držená by po zavření prosákla do
 * matice stroje s další událostí klávesnice).
 *
 * @return true = klávesy emulované klávesnici nepředávat a globální zkratky neobsluhovat.
 * @pre UI vlákno; po zavření overlaye navíc mezi ImGui::NewFrame() a ImGui::Render()
 *      (čte stav kláves ImGui).
 * @note Při zavřeném overlayi bez čekání na uvolnění kláves jde o jeden test příznaku.
 */
bool imgui_overlay_blocks_keyboard(void);

/**
 * @brief Zadržuje overlay joystick emulovaného stroje?
 *
 * Ano po dobu otevření (i při poslechu na tabu Sound, kdy emulace běží) a po zavření
 * ještě do chvíle, kdy je hostitelský joystick každého portu typu JOYSTICK
 * v klidu (žádný směr, žádné tlačítko): tlačítko držené při zavření by
 * jinak prosáklo do stroje. Po tu dobu čte stroj joystick v klidu
 * (joy_host_hold()); joystick z numerické klávesnice uvádí do klidu
 * klidový sken při otevření a drží ho zadržení klávesnice
 * (imgui_overlay_blocks_keyboard()). Uvolnění zadržení provádí tato
 * funkce, hlavní okno ji proto volá každý snímek.
 *
 * @return true = joystick je zadržený.
 * @pre UI vlákno.
 * @note Při zavřeném overlayi bez čekání na klid joysticku jde o dva testy
 *       příznaku; při čekání čte živý stav joysticku (joy_host_scan_raw()).
 */
bool imgui_overlay_blocks_joystick(void);

/**
 * @brief Vyzvedne požadavek vrátit fokus hlavnímu oknu po zavření overlaye.
 *
 * Overlay se otevírá z hlavního okna (F10, Alt + klik), po zavření má tedy
 * fokus dostat zpět obraz, ne okno nástroje, které je v pořadí fokusu
 * ImGui výš. Hlavní okno po vrácení true zavolá ImGui::SetNextWindowFocus()
 * před svým Begin().
 *
 * @return true = overlay se od posledního volání zavřel; požadavek se voláním ruší.
 * @pre UI vlákno.
 */
bool imgui_overlay_take_main_focus_request(void);

/**
 * @brief Otevře overlay menu.
 *
 * Běžící emulaci zastaví (dbg_ui_pause()) a podle skutečného stavu po
 * volání (EMULATOR_TEST_PAUSED) si zapamatuje, zda pauzu způsobil overlay;
 * emulaci, která už stála, nechá být. Platí to na všech tabech včetně
 * tabu Sound; přepínač Listen je po otevření vypnutý. Pošle emulované klávesnici klidovou matici
 * (main_window_keyboard_release()); vyžádá fokus okna overlaye pro příští
 * kreslení; vypne přepínání oken ImGui klávesami Ctrl+Tab (ty v overlayi
 * přepínají taby; main_window_nav_windowing_set()), po dobu otevření ho
 * vypnuté drží hlavní okno. Zvolený tab se nemění.
 *
 * @pre UI vlákno. Kontext ImGui existovat nemusí (bez něj se přepínání
 *      oken ImGui neřeší).
 * @post imgui_overlay_is_open() == true. Otevřený overlay volání ignoruje.
 * @note S debuggerem může dbg_ui_pause() čekat na emu vlákno nejvýš
 *       DBG_UI_DEFAULT_TIMEOUT_MS. Nepovede-li se zastavení, overlay se
 *       otevře i tak, emulace běží pod ním (štítek Paused se neukáže)
 *       a zavření ji nerozbíhá. Otevření v době, kdy ještě čeká opakované
 *       rozběhnutí po minulém zavření, bere stojící emulaci jako
 *       zastavenou overlayem.
 */
void imgui_overlay_open(void);

/**
 * @brief Zavře overlay menu a obnoví zapamatovaný stav pauzy.
 *
 * Emulaci rozběhne (dbg_ui_run()) jen tehdy, když právě stojí a pauzu
 * způsobil overlay a nikdo ji mezitím nezměnil (overlay ji neviděl běžet
 * a číslo běhu jádra je stejné jako při otevření), nebo když to určila akce
 * (imgui_overlay_note_pause_changed(false)). Emulace, která běží kvůli
 * poslechu na tabu Sound (přepínač Listen), běží dál; přepínač se vypne.
 * Nepovede-li se rozběhnutí
 * (emu vlákno neodpoví včas), zopakuje ho imgui_overlay_frame() v dalších
 * snímcích, nejvýš pětkrát po dvaceti snímcích; potom zůstane emulace stát
 * a na standardní chybový výstup jde varování. Čeká-li v okamžiku zavření
 * uložení snapshotu vyžádané z overlaye (Quick Save, Save Snapshot...;
 * vyřizuje ho emu vlákno jen v pauze), overlay emulaci nerozbíhá a
 * rozběhnutí předá dokončení uložení (snapshot_ui_run_after_request()).
 * Přepínání oken ImGui klávesami Ctrl+Tab nevrací: rozhoduje o něm hlavní
 * okno podle fokusu (main_window_nav_windowing_allowed()). Vyžádá
 * vrácení fokusu hlavnímu oknu (imgui_overlay_take_main_focus_request())
 * a zadrží emulovanou klávesnici do uvolnění všech kláves
 * (imgui_overlay_blocks_keyboard()) a joystick stroje do jeho klidu
 * (imgui_overlay_blocks_joystick()).
 *
 * @pre UI vlákno.
 * @post imgui_overlay_is_open() == false. Zavřený overlay volání ignoruje.
 */
void imgui_overlay_close(void);

/**
 * @brief Otevře zavřený overlay, zavře otevřený.
 *
 * Klávesa F10 ho nepoužívá: otevření obsluhuje hlavní okno přes
 * imgui_overlay_open(), zavření overlay sám.
 * @pre UI vlákno.
 */
void imgui_overlay_toggle(void);

/**
 * @brief Přepíše zapamatovaný stav pauzy, který se uplatní při zavření.
 *
 * Volá akce overlaye, která o běhu emulace po zavření rozhoduje sama
 * (Resume přes overlay_actions_run_pending(), přepnutí pauzy). Volání
 * vypíná přepínač Listen tabu Sound a ten pak běh emulace neřídí (je
 * zakázaný): emulaci zastavenou uživatelem nerozbíhá a emulaci, kterou
 * uživatel rozběhl, přepnutí tabu nezastaví.
 *
 * @param paused true = zavření emulaci nerozbíhá (pauza už není overlaye),
 *               false = zavření stojící emulaci rozběhne, i když před otevřením stála.
 * @pre UI vlákno. Při zavřeném overlayi nemá volání účinek (otevření stav přepíše).
 */
void imgui_overlay_note_pause_changed(bool paused);

/**
 * @brief Zapne nebo vypne poslech na tabu Sound (přepínač Listen).
 *
 * Zapnutí jen zaznamená žádost; emulaci rozběhne příští
 * imgui_overlay_frame() (dbg_ui_run(), po případném čekání na uložení
 * snapshotu; nepovedené rozběhnutí se opakuje nejvýš pětkrát po dvaceti
 * snímcích, potom se přepínač vypne). Vypnutí emulaci, která kvůli
 * poslechu běží, v příštím imgui_overlay_frame() znovu zastaví
 * (dbg_ui_pause()) a pauza je zase overlaye.
 *
 * @param on true = zapnout: platí jen na tabu Sound bez výsuvného listu a
 *           rozbaleného výběru a jen když emulace stojí pauzou overlaye;
 *           jinak se volání ignoruje (cizí pauzu overlay nerozbíhá).
 *           false = vypnout.
 * @pre UI vlákno. Při zavřeném overlayi nemá volání účinek.
 */
void imgui_overlay_set_listen(bool on);

/**
 * @brief Stav přepínače Listen tabu Sound.
 *
 * Stejná hodnota, jakou imgui_overlay_frame() zapisuje do kopie stavu
 * (st_OVERLAY_VIEW_SOUND::listen), spočítaná z okamžitého stavu pauzy.
 *
 * @return Hodnota en_OVERLAY_LISTEN (overlay_view.h); při zavřeném overlayi
 *         OVERLAY_LISTEN_OFF.
 * @pre UI vlákno.
 */
int imgui_overlay_listen_state(void);

/**
 * @brief Požádá hlavní okno, aby otevřelo klasické menu (popup pravého tlačítka myši).
 *
 * Volá akce "Open classic menu" tabu Machine těsně před zavřením overlaye.
 * Popup patří hlavnímu oknu, proto ho overlay neotevírá sám: hlavní okno si
 * požadavek vyzvedne imgui_overlay_take_classic_menu_request() ve svém
 * příštím snímku, kdy už má zpět fokus.
 *
 * @pre UI vlákno.
 */
void imgui_overlay_request_classic_menu(void);

/**
 * @brief Vyzvedne požadavek na otevření klasického menu.
 *
 * Je-li v okamžiku vyzvednutí otevřený jiný popup (modál otevřený mezitím z
 * jiného zdroje; imgui_modal_can_open() == false), požadavek se zahodí a
 * funkce vrátí false: ImGui::OpenPopup() klasického menu by cizí popup
 * zavřel. Menu se v tom případě neotevře ani později.
 *
 * @return true = od posledního volání byl požadavek podán (imgui_overlay_request_classic_menu())
 *         a žádný popup není otevřený; hlavní okno má zavolat ImGui::OpenPopup() svého klasického
 *         menu. Voláním se požadavek ruší vždy.
 * @pre UI vlákno, kontext ImGui existuje.
 */
bool imgui_overlay_take_classic_menu_request(void);

/**
 * @brief Požádá hlavní okno, aby otevřelo overlay menu (položka "Overlay Menu..." klasického menu).
 *
 * Opačný směr k imgui_overlay_request_classic_menu(): položka se volí
 * uvnitř popupu klasického menu, který se jejím zvolením teprve zavírá.
 * Overlay se proto neotevírá hned, ale až v příštím snímku hlavního okna
 * (imgui_overlay_take_open_request()), kdy už popup otevřený není a
 * overlay dostane fokus.
 *
 * @pre UI vlákno. Při otevřeném overlayi nemá volání účinek.
 */
void imgui_overlay_request_open(void);

/**
 * @brief Vyzvedne požadavek na otevření overlay menu.
 *
 * Je-li v okamžiku vyzvednutí otevřený nějaký popup (klasické menu se
 * nezavřelo, modál otevřený mezitím z jiného zdroje;
 * imgui_modal_can_open() == false) nebo je overlay už otevřený, požadavek
 * se zahodí a funkce vrátí false.
 *
 * @return true = od posledního volání byl požadavek podán (imgui_overlay_request_open()),
 *         overlay je zavřený a žádný popup není otevřený; hlavní okno má zavolat
 *         imgui_overlay_open(). Voláním se požadavek ruší vždy.
 * @pre UI vlákno, kontext ImGui existuje.
 */
bool imgui_overlay_take_open_request(void);

/**
 * @brief Vyžádá fokus okna overlaye pro příští kreslení.
 *
 * Volá hlavní okno, když při otevřeném overlayi dostane fokus zpět (např.
 * po zavření dialogu), aby klávesy overlaye fungovaly bez klepnutí myší.
 *
 * @pre UI vlákno. Při zavřeném overlayi nemá volání účinek.
 */
void imgui_overlay_request_focus(void);

/**
 * @brief Snímek overlaye: při otevřeném overlayi vyplní kopii stavu a vykreslí ho přes hlavní viewport.
 *
 * Volá se každý snímek z imgui_main_window() po hlavním okně. Při zavřeném
 * overlayi se hned vrací (jeden test příznaku); jen když po zavření čeká
 * opakované rozběhnutí emulace, udělá jeho další krok. Kopii plní
 * overlay_view_fill() pod device_state_lock() (zámek si bere a uvolňuje
 * sama); nedávná média doplní overlay_view_fill_recent() až po uvolnění
 * zámku a existenci jejich souborů nechá zjišťovat jen na tabu Media.
 * Všimne-li si, že emulaci zastavenou overlayem mezitím někdo
 * rozběhl nebo krokoval (běží, nebo má jiné číslo běhu), přestane pauzu
 * považovat za svou. Před plněním kopie srovná běh emulace s přepínačem
 * Listen tabu Sound: po zapnutí emulaci zastavenou overlayem rozběhne, po
 * vypnutí (i samočinném: jiný tab, výsuvný list, rozbalený výběr) ji znovu
 * zastaví (nejvýš jedno volání dbg_ui_run() nebo dbg_ui_pause() za snímek,
 * jen při změně). Stav přepínače zapíše do kopie
 * (st_OVERLAY_VIEW_SOUND::listen).
 *
 * @pre UI vlákno, mezi ImGui::NewFrame() a ImGui::Render(), mimo Begin() / End()
 *      jiného okna; volající nedrží device_state_lock().
 */
void imgui_overlay_frame(void);

/**
 * @brief Zvolí tab.
 * @param tab Tab; hodnota mimo rozsah se ignoruje.
 * @pre UI vlákno.
 */
void imgui_overlay_set_tab(en_OVERLAY_TAB tab);

/**
 * @brief Zvolený tab.
 * @return Tab v rozsahu OVERLAY_TAB_MEDIA až OVERLAY_TAB_CAPTURE.
 */
en_OVERLAY_TAB imgui_overlay_get_tab(void);

/**
 * @brief Vykreslí overlay z kopie stavu (čisté kreslení, používají ho testy).
 *
 * Vytvoří okno OVERLAY_WINDOW_ID na dané ploše, vykreslí horní lištu,
 * obsah zvoleného tabu a patičku a obslouží vstup overlaye: klik na tab
 * a na Close, při fokusu okna klávesy F10 a Esc (zavřít), číslice 1 až 6
 * (tab), Ctrl+Tab a Ctrl+Shift+Tab (další a předchozí tab) a zkratky akcí
 * (overlay_actions_handle_shortcuts()). Je-li rozbalený seznam
 * rozbalovacího výběru (ovl_dropdown()), má fokus jeho popup a klávesy
 * rámce se neobsluhují (Esc zavře jen seznam). Pohyb fokusu mezi prvky dělá
 * navigace ImGui (šipky, Tab; Enter nebo mezerník volí). Akci vyžádanou
 * prvkem nebo zkratkou provede po vykreslení okna
 * (overlay_actions_run_pending()); ta může overlay zavřít. Stejně odloženě
 * se provede vložení média vyžádané výsuvným listem
 * (overlay_sheet_run_pending()). Při otevřeném výsuvném listu se z kláves
 * rámce obsluhuje jen F10.
 * Nastaví plochu vizuálního systému (overlay_style_set_viewport()). Klávesy se
 * neobsluhují ve snímku, ve kterém byl overlay otevřen (stisk F10, který
 * ho otevřel, ho nesmí hned zavřít), ani při psaní do textového pole.
 * Stav jádra nečte; zavření jde přes imgui_overlay_close().
 *
 * Kreslí bez ohledu na imgui_overlay_is_open() (o tom rozhoduje
 * imgui_overlay_frame()).
 *
 * @param v    Kopie stavu; NULL = nic se nekreslí.
 * @param pos  Levý horní roh plochy overlaye (souřadnice ImGui).
 * @param size Velikost plochy; nekladná výška = nic se nekreslí.
 * @pre UI vlákno, mezi ImGui::NewFrame() a ImGui::Render(), mimo Begin() / End() jiného okna.
 * @post Zásobníky stylů, písem a oken ImGui jsou ve stavu jako před voláním.
 */
void imgui_overlay_draw(const st_OVERLAY_VIEW *v, ImVec2 pos, ImVec2 size);

/**
 * @brief Jen pro testy: obdélník prvku z posledního imgui_overlay_draw().
 * @param name Jméno prvku: "close", "tab0" až "tab5", "chip_platform", "chip_speed", "chip_paused", "chip_muted",
 *             "footer_hints" (vykreslená nápověda kláves jako celek), "footer_right", "content" (plocha obsahu tabu),
 *             nebo jméno prvku tabu zaznamenané overlay_test_note_rect() (jen v testovacím buildu).
 * @param min  Výstup: levý horní roh (smí být NULL).
 * @param max  Výstup: pravý dolní roh (smí být NULL).
 * @return true = prvek byl v posledním kreslení vykreslen (prvek, který v úzkém okně ustoupil, vrací false).
 */
bool imgui_overlay_test_item_rect(const char *name, ImVec2 *min, ImVec2 *max);

#endif /* OVERLAY_H */
