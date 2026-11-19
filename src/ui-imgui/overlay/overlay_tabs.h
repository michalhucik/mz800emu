/**
 * @file   overlay_tabs.h
 * @brief  Vnitřní rozhraní mezi rámcem overlay menu (overlay.cpp), kreslením jednotlivých tabů a výsuvným listem výměny média.
 *
 * Tab je jedna funkce kreslení volaná rámcem uvnitř okna obsahu overlaye
 * se stylem overlay_style_push(). Kreslí jen z kopie stavu
 * (st_OVERLAY_VIEW); zásahy do emulátoru vyžaduje přes tabulku akcí
 * (overlay_actions.h). Výsuvný list (overlay_insert_sheet.cpp) otevírá tab
 * Media, kreslí ho rámec nad obsahem tabu a média z historie vkládá sám
 * (overlay_sheet_request_insert()).
 *
 * @par Vlákna
 * Jen UI vlákno.
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

#ifndef OVERLAY_TABS_H
#define OVERLAY_TABS_H

#include "libs/imgui/imgui.h"

#include "ui-imgui/overlay/overlay_view.h"

/**
 * @brief Vykreslí obsah tabu Media: mechaniky, kazetu, Quick Disk, nedávná média a řadu akcí.
 *
 * V širokém okně dva sloupce (mechaniky vlevo, kazeta a Quick Disk
 * vpravo), pod nimi pruh nedávných médií a řada akcí; v úzkém okně
 * (overlay_is_narrow()) vše pod sebou a řada akcí ve dvou řadách. Karta
 * zařízení, které build nemá, se nekreslí; karta nedostupného zařízení
 * (aktivní replay, odpojený řadič) je zakázaná s důvodem pod nadpisem.
 *
 * @param v Kopie stavu (platná po dobu volání).
 * @param u Jednotka rozměrů overlaye v pixelech.
 * @pre Uvnitř okna obsahu overlaye se stylem overlay_style_push().
 * @post Zásobníky ImGui jsou ve stavu jako před voláním. Zvolené akce jsou
 *       vyžádané (overlay_action_request()), ne provedené.
 */
void overlay_tab_media_draw(const st_OVERLAY_VIEW *v, float u);

/**
 * @brief Vykreslí obsah tabu Machine: karty Machine, Speed, Snapshots a More hardware.
 *
 * Machine: běh emulace (Running / Paused), Reset, výběr ROM. Speed: rychlost
 * Normal / Custom / Max a vlastní rychlost posuvníkem s tlačítky minus a
 * plus. Snapshots: Quick Save, Quick Load, Save Snapshot..., Load
 * Snapshot..., Snapshot Setup.... More hardware: odkaz na klasické menu s
 * tlačítkem, které overlay zavře a klasické menu otevře. V širokém okně dva
 * sloupce (karty vedle sebe mají stejnou výšku), v úzkém okně
 * (overlay_is_narrow()) jeden sloupec. Výchozí fokus má řádek Emulation.
 *
 * @param v Kopie stavu (platná po dobu volání).
 * @param u Jednotka rozměrů overlaye v pixelech.
 * @pre Uvnitř okna obsahu overlaye se stylem overlay_style_push().
 * @post Zásobníky ImGui jsou ve stavu jako před voláním. Zvolené akce jsou
 *       vyžádané (overlay_action_request()), ne provedené.
 */
void overlay_tab_machine_draw(const st_OVERLAY_VIEW *v, float u);

/**
 * @brief Vykreslí obsah tabu Display: karty Colors, CRT scanlines, Window a Frame rate.
 *
 * Karty jsou pod sebou v tomto pořadí (nahoře ty, jejichž účinek je vidět
 * na obraze). Colors: barevné schéma. CRT scanlines: zapnutí a intenzita.
 * Window: Fullscreen (přepínač se skutečným stavem okna), velikost okna při
 * startu, uzamčení poměru stran, srovnání poměru stran podle šířky a
 * výšky. Frame rate: řízení snímkové frekvence (v režimu Custom FPS navíc
 * řádek s hodnotou) a vynucené překreslení. Pod kartami věta "Changes apply
 * immediately.". Karty jsou v jednom sloupci; v širokém okně zabírá sloupec
 * jen část šířky u levého okraje, v úzkém okně celou šířku, a obsah se
 * podle potřeby svisle roluje. Výchozí fokus má zvolený segment řádku
 * Color schema.
 *
 * @param v Kopie stavu (platná po dobu volání).
 * @param u Jednotka rozměrů overlaye v pixelech.
 * @pre Uvnitř okna obsahu overlaye se stylem overlay_style_push().
 * @post Zásobníky ImGui jsou ve stavu jako před voláním. Zvolené akce jsou
 *       vyžádané (overlay_action_request(), overlay_action_request_value()), ne provedené.
 */
void overlay_tab_display_draw(const st_OVERLAY_VIEW *v, float u);

/**
 * @brief Vykreslí obsah tabu Sound: karty Output a Channels.
 *
 * Output: přepínač Listen (poslech při otevřeném overlayi; stav a
 * dostupnost podle st_OVERLAY_VIEW_SOUND::listen), hlavní hlasitost
 * poslechu (posuvník s ikonou a procenty), ztlumení, věrná výstupní
 * propust (jen na stroji, který ji má) a věty nápovědy; poslední věta
 * říká, zda emulace kvůli poslechu běží a změny jsou slyšet hned, nebo že
 * poslech zapíná přepínač Listen. Channels:
 * posuvník hlasitosti každého kanálu se jmény a počtem jako okno Audio
 * Mixer a tlačítko Reset channels. V širokém okně dva sloupce stejné
 * výšky, v úzkém okně (overlay_is_narrow()) jeden sloupec. Výchozí fokus
 * má posuvník hlavní hlasitosti.
 *
 * @param v Kopie stavu (platná po dobu volání).
 * @param u Jednotka rozměrů overlaye v pixelech.
 * @pre Uvnitř okna obsahu overlaye se stylem overlay_style_push().
 * @post Zásobníky ImGui jsou ve stavu jako před voláním. Zvolené akce jsou
 *       vyžádané (overlay_action_request(), overlay_action_request_value()), ne provedené.
 */
void overlay_tab_sound_draw(const st_OVERLAY_VIEW *v, float u);

/**
 * @brief Vykreslí obsah tabu Input: karty Keyboard a Joystick.
 *
 * Keyboard: tlačítka, která zavřou overlay a ukážou virtuální klávesnici
 * a okno Autotype Buffer, a věta o klávesách při otevřeném menu. Joystick:
 * zařízení prvního portu (None / Numeric keypad / Joystick) a tlačítko
 * Joystick Setup.... V širokém okně dva sloupce stejné výšky, v úzkém
 * okně jeden sloupec. Výchozí fokus má zvolený segment řádku Device (při
 * zakázané volbě tlačítko Virtual keyboard).
 *
 * @param v Kopie stavu (platná po dobu volání).
 * @param u Jednotka rozměrů overlaye v pixelech.
 * @pre Uvnitř okna obsahu overlaye se stylem overlay_style_push().
 * @post Zásobníky ImGui jsou ve stavu jako před voláním. Zvolené akce jsou
 *       vyžádané (overlay_action_request(), overlay_action_request_value()), ne provedené.
 */
void overlay_tab_input_draw(const st_OVERLAY_VIEW *v, float u);

/**
 * @brief Vykreslí obsah tabu Capture: karty Screenshot, Video Recording a Replay.
 *
 * V širokém okně tři sloupce stejné výšky, v úzkém okně jeden sloupec.
 * Tlačítka "Open Output Folder" a "Settings..." jsou vedle sebe, a když se
 * nevejdou (delší překlad), pod sebou. Výchozí fokus má tlačítko Save
 * Screenshot.
 *
 * @param v Kopie stavu (platná po dobu volání).
 * @param u Jednotka rozměrů overlaye v pixelech.
 * @pre Uvnitř okna obsahu overlaye se stylem overlay_style_push().
 * @post Zásobníky ImGui jsou ve stavu jako před voláním. Zvolené akce jsou
 *       vyžádané (overlay_action_request()), ne provedené.
 */
void overlay_tab_capture_draw(const st_OVERLAY_VIEW *v, float u);

/* ================================================================
 * Výsuvný list výměny média (overlay_insert_sheet.cpp)
 * ================================================================ */

/**
 * @brief Jedna nápověda patičky: klávesový štítek (nebo dva se spojkou) a význam.
 *
 * Invariant: key1 a action jsou vždy platné; joiner a key2 jsou buď oba
 * platné, nebo oba NULL.
 */
typedef struct st_OVERLAY_HINT {
    const char *key1;   /**< text prvního klávesového štítku */
    const char *joiner; /**< přeložená spojka mezi štítky ("to"); NULL = jeden štítek */
    const char *key2;   /**< text druhého klávesového štítku; NULL = jeden štítek */
    const char *action; /**< přeložený význam */
} st_OVERLAY_HINT;

/** @brief Největší počet nápověd, které list dá patičce (overlay_sheet_footer_hints()). */
#define OVERLAY_SHEET_HINTS_MAX 4

/** @brief Co výsuvný list právě nabízí. */
typedef enum en_OVL_SHEET_TARGET {
    OVL_SHEET_NONE = 0,   /**< list je zavřený */
    OVL_SHEET_FDD,        /**< výměna diskety v mechanice FDC0 ("Insert into FDD n") */
    OVL_SHEET_CMT,        /**< výměna pásky ("Change tape") */
    OVL_SHEET_QD,         /**< vložení Quick Disku ("Insert into Quick Disk") */
    OVL_SHEET_PICK_DRIVE, /**< volba mechaniky pro obraz diskety z pruhu nedávných médií ("Insert into") */
} en_OVL_SHEET_TARGET;

/** @brief Prvek tabu Media, ke kterému se list ukotvuje. */
typedef enum en_OVL_SHEET_ANCHOR {
    OVL_SHEET_ANCHOR_FDD = 0, /**< tlačítko "Change" / "Insert disk" mechaniky (index = mechanika 0 až 3) */
    OVL_SHEET_ANCHOR_CMT,     /**< tlačítko "Change tape" (index 0) */
    OVL_SHEET_ANCHOR_QD,      /**< tlačítko "Insert disk" Quick Disku (index 0) */
    OVL_SHEET_ANCHOR_CHIP,    /**< položka pruhu nedávných médií (index = pořadí v st_OVERLAY_VIEW::recent) */
} en_OVL_SHEET_ANCHOR;

/**
 * @brief Otevře výsuvný list výměny média u mechaniky, kazety nebo Quick Disku.
 *
 * List se kreslí nad obsahem tabu Media (overlay_sheet_draw()), dostane
 * fokus a po dobu otevření jsou zkratky tabů a akcí overlaye vypnuté.
 * Filtr je po otevření prázdný a fokus má první položka seznamu. Otevřený
 * list se nahradí novým.
 *
 * @param target OVL_SHEET_FDD, OVL_SHEET_CMT nebo OVL_SHEET_QD; jiná hodnota se ignoruje.
 * @param drive  Mechanika 0 až 3 pro OVL_SHEET_FDD (mimo rozsah se volání ignoruje); jinak se nepoužívá.
 * @pre UI vlákno, uvnitř snímku ImGui.
 * @note Čeká-li v tomto snímku vyžádaná akce (overlay_action_is_pending()), volání se ignoruje; totéž platí
 *       pro overlay_sheet_open_pick() a overlay_sheet_request_insert().
 */
void overlay_sheet_open(en_OVL_SHEET_TARGET target, int drive);

/**
 * @brief Otevře malou volbu mechaniky "Insert into" pro obraz diskety z pruhu nedávných médií.
 * @param item Položka nedávných médií druhu OVERLAY_MEDIA_DSK (zkopíruje se); NULL nebo jiný druh se ignoruje.
 * @param chip Pořadí položky v pruhu (kotva volby, OVL_SHEET_ANCHOR_CHIP).
 * @pre UI vlákno, uvnitř snímku ImGui.
 */
void overlay_sheet_open_pick(const st_OVERLAY_VIEW_RECENT *item, int chip);

/**
 * @brief Zavře výsuvný list (nebo volbu mechaniky) a vrátí fokus prvku, který ho otevřel.
 * @post overlay_sheet_is_open() == false; zavřený list volání ignoruje.
 */
void overlay_sheet_close(void);

/**
 * @brief Zahodí stav listu bez vracení fokusu: zavře ho a zruší čekající vložení média.
 *
 * Volá rámec při otevření a zavření overlaye.
 */
void overlay_sheet_reset(void);

/**
 * @brief Je výsuvný list nebo volba mechaniky otevřená?
 * @return true = rámec nesmí obsluhovat klávesy tabů a akcí a Esc zavírá jen list.
 */
bool overlay_sheet_is_open(void);

/**
 * @brief Co otevřený list nabízí.
 * @param[out] drive Mechanika listu OVL_SHEET_FDD, jinak -1; smí být NULL.
 * @return Cíl listu; OVL_SHEET_NONE = zavřený.
 */
en_OVL_SHEET_TARGET overlay_sheet_target(int *drive);

/**
 * @brief Zaznamená polohu prvku, ke kterému se list ukotvuje (volá tab Media při kreslení).
 * @param kind  Druh kotvy.
 * @param index Index podle druhu (viz en_OVL_SHEET_ANCHOR); mimo rozsah se ignoruje.
 * @param min   Levý horní roh prvku (souřadnice obrazovky).
 * @param max   Pravý dolní roh prvku.
 */
void overlay_sheet_note_anchor(en_OVL_SHEET_ANCHOR kind, int index, ImVec2 min, ImVec2 max);

/**
 * @brief Důvod, proč médium z historie teď nejde vložit.
 *
 * Obraz diskety jde vložit, je-li dostupná aspoň jedna mechanika; páska
 * podle pravidel výměny pásky; Quick Disk jen do prázdné mechaniky a jen
 * médium odpovídající jejímu typu (obraz nebo adresář).
 *
 * @param v    Kopie stavu; NULL = nejde.
 * @param item Položka nedávných médií; NULL = nejde.
 * @return NULL = jde vložit; jinak přeložený důvod pro tooltip (smí být prázdný; platný do dalšího volání).
 */
const char *overlay_sheet_insert_reason(const st_OVERLAY_VIEW *v, const st_OVERLAY_VIEW_RECENT *item);

/**
 * @brief Vyžádá vložení média z historie; provede ho overlay_sheet_run_pending() po vykreslení.
 *
 * Vkládá se stejnou funkcí, kterou volá souborový dialog klasického menu po
 * výběru souboru: disketa fdc_request_mount(), páska
 * imgui_cmt_tape_open_path() (dotazy na nahrávání a neuložené změny),
 * Quick Disk qdisk_request_mount(). Existence souboru se bere z posledního
 * známého stavu položky (zjišťuje ho pracovní vlákno, UI vlákno se disku
 * neptá): soubor, který už neexistuje, se místo vložení odstraní z historie
 * a ohlásí se zprávou; položka, jejíž stav ještě není známý
 * (st_OVERLAY_VIEW_RECENT::checked == false), se nevloží a ohlásí se, že
 * ověření ještě běží.
 *
 * @param item  Položka nedávných médií (zkopíruje se); NULL se ignoruje.
 * @param drive Mechanika 0 až 3 pro obraz diskety; u pásky a Quick Disku se nepoužívá.
 * @pre UI vlákno. Do provedení se drží jediný požadavek; další se ignoruje.
 */
void overlay_sheet_request_insert(const st_OVERLAY_VIEW_RECENT *item, int drive);

/**
 * @brief Vykreslí otevřený list nad obsahem tabu a obslouží jeho klávesy.
 *
 * Plochu obsahu ztmaví, klik mimo list ho zavře. Klávesy (jen při fokusu
 * overlaye): Esc zavře list; číslice 1 až 6 vloží položku, jen když je
 * filtr prázdný; ostatní tisknutelné znaky jdou do filtru, Backspace maže
 * poslední znak; šipky, Tab a Enter jsou navigace ImGui uvnitř listu. List
 * se sám zavře, když jeho cíl přestane být dostupný (začátek záznamu
 * vstupů, odpojení řadiče) nebo když není zvolen tab Media.
 *
 * @param v              Kopie stavu.
 * @param area_min       Levý horní roh plochy obsahu tabu (mezi horní lištou a patičkou).
 * @param area_max       Pravý dolní roh plochy obsahu tabu.
 * @param overlay_focused Má okno overlaye (nebo jeho potomek) fokus?
 * @param media_tab      Je zvolen tab Media?
 * @pre Uvnitř okna overlaye se stylem overlay_style_push(), po vykreslení obsahu tabu.
 * @post Zásobníky ImGui jsou ve stavu jako před voláním.
 */
void overlay_sheet_draw(const st_OVERLAY_VIEW *v, ImVec2 area_min, ImVec2 area_max, bool overlay_focused, bool media_tab);

/**
 * @brief Nápověda kláves otevřeného listu pro patičku overlaye.
 * @param[out] hints Pole pro nejvýš OVERLAY_SHEET_HINTS_MAX nápověd (texty platí do konce snímku).
 * @return Počet vyplněných nápověd; 0 = list je zavřený (patička ukáže nápovědu overlaye).
 */
int overlay_sheet_footer_hints(st_OVERLAY_HINT *hints);

/**
 * @brief Provede vyžádané vložení média (overlay_sheet_request_insert()) a vrácení fokusu po zavření listu.
 * @param v Kopie stavu, ze které se v tomto snímku kreslilo (dostupnost se ověří znovu).
 * @pre UI vlákno, mimo Begin() / End() okna overlaye, volající nedrží device_state_lock().
 * @post Žádný požadavek nečeká.
 */
void overlay_sheet_run_pending(const st_OVERLAY_VIEW *v);

/**
 * @brief Jen pro testy: text filtru otevřeného listu.
 * @return Text filtru (UTF-8); prázdný při zavřeném listu.
 */
const char *overlay_sheet_test_filter(void);

/**
 * @brief Jen pro testy: počet zpráv "soubor nenalezen, odstraněn z historie" od posledního volání.
 * @return Počet zpráv; voláním se vynuluje.
 */
int overlay_sheet_test_take_missing_reports(void);

/* ================================================================
 * Fokus a testy
 * ================================================================ */

/**
 * @brief Má tab v tomto kreslení dát fokus navigace svému výchozímu prvku?
 *
 * Rámec o to žádá po otevření overlaye a po přepnutí tabu (po dva snímky,
 * aby výchozí prvek nepřebila úvodní volba navigace ImGui pro nově
 * zaměřené okno).
 *
 * @return true = tab má výchozímu prvku dát fokus (overlay_focus_last_item()).
 */
bool overlay_default_focus_wanted(void);

/**
 * @brief Dá fokus navigace naposledy přidané položce ImGui a zviditelní kurzor navigace.
 * @pre Voláno hned po přidání položky (ItemAdd()) uvnitř okna overlaye.
 */
void overlay_focus_last_item(void);

/**
 * @brief Dá fokus navigace položce současného okna s daným ID a zviditelní kurzor navigace.
 *
 * Pro prvky složené z více položek (segmentová volba), u kterých výchozí
 * fokus nepatří naposledy přidané položce.
 *
 * @param id ID položky v současném okně; 0 se ignoruje.
 * @pre Uvnitř okna overlaye, po přidání položky.
 */
void overlay_focus_item(ImGuiID id);

/**
 * @brief Stavový text řádku disketové mechaniky v tabu Media (druhý řádek pod jménem obrazu).
 *
 * Velikost obrazu a za ní ochrana proti zápisu: u obrazu, který má ochranu
 * kvůli sdílení souboru s jinou mechanikou (shared_readonly a shared_with
 * >= 0), poznámka "Shared with FDD n, write-protected", jinak "Read-only".
 * Řádek s neuloženými změnami tento text neukazuje (kreslí "Unsaved changes").
 *
 * @param d    Mechanika v kopii stavu; NULL nebo prázdná mechanika = prázdný text.
 * @param buf  Výstup, vždy ukončený nulou; delší text se zkrátí.
 * @param size Velikost buf v bajtech (0 = nic se nezapíše).
 * @note UI vlákno (používá překlad); čte jen předanou kopii.
 */
void overlay_media_fdd_info_text(const st_OVERLAY_VIEW_FDD *d, char *buf, size_t size);

/**
 * @brief Zaznamená obdélník prvku tabu pod jménem pro testy (imgui_overlay_test_item_rect()).
 *
 * Mimo testovací build (bez MZTEST_HEADLESS) nedělá nic.
 *
 * @param name Jméno prvku (nejvýš 31 znaků; delší se zkrátí).
 * @param min  Levý horní roh.
 * @param max  Pravý dolní roh.
 */
void overlay_test_note_rect(const char *name, ImVec2 min, ImVec2 max);

/**
 * @brief Zaznamená obdélník naposledy přidané položky ImGui pod jménem pro testy.
 * @param name Jméno prvku (viz overlay_test_note_rect()).
 */
void overlay_test_note_item(const char *name);

#ifdef MZTEST_HEADLESS
/**
 * @brief Jen pro testy: zapne nebo vypne prodlužování přeložených textů tabů o 40 %.
 *
 * Ověřuje rezervu rozvržení na delší překlad (vzor cmt_view_test_set_stretch()).
 *
 * @param on true = texty vracené overlay_test_tr() jsou o 40 % delší.
 */
void overlay_test_set_stretch(bool on);

/**
 * @brief Jen pro testy: překlad textu, při zapnutém prodlužování o 40 % delší.
 *
 * Soubory tabů jím v testovacím buildu nahrazují makro _(). Prodloužení
 * přidává jen písmena a číslice z textu samotného, formátovací značky
 * se nemění.
 *
 * @param msgid Anglický text.
 * @return Přeložený (a případně prodloužený) text; platný do konce procesu.
 */
const char *overlay_test_tr(const char *msgid) __attribute__((format_arg(1)));

/**
 * @brief Jen pro testy: překlad textu s kontextem, při zapnutém prodlužování o 40 % delší.
 *
 * Soubory tabů jím v testovacím buildu nahrazují makro C_(). Hledá se
 * klíč s kontextem; když ho katalog nemá, vrací se anglický text bez
 * kontextu (jako pgettext()). Prodloužení jako u overlay_test_tr().
 *
 * @param ctx_msgid Klíč katalogu: kontext, znak 004h a anglický text.
 * @param msgid     Anglický text bez kontextu.
 * @return Přeložený (a případně prodloužený) text; platný do konce procesu.
 */
const char *overlay_test_trc(const char *ctx_msgid, const char *msgid);
#endif

#endif /* OVERLAY_TABS_H */
