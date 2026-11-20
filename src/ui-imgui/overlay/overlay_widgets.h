/**
 * @file   overlay_widgets.h
 * @brief  Widgety overlay menu: karta, řádek nastavení, tlačítko, přepínač, segmentová volba, posuvník, štítky, text s výpustkou.
 *
 * Widgety kreslí podle tokenů z overlay_style.h a všechny rozměry berou z
 * jednotky overlay_unit(); volají se uvnitř okna, pro které platí
 * overlay_style_push().
 *
 * Společná pravidla:
 * - Každý ovládací prvek je položka ImGui s vlastním ID: reaguje na myš,
 *   navigace ImGui na něj umí přejít (šipky, Tab) a Enter nebo mezerník ho
 *   volí. Prvek s fokusem navigace má obrys OVL_FOCUS_W v barvě
 *   OVL_COL_ACCENT; modrý obrys nic jiného neznamená.
 * - Popisek ve tvaru "Text##id" (makro _L()) zobrazí jen část před "##";
 *   ID se počítá z celého řetězce. Texty předává volající už přeložené.
 * - Zakázaný stav se řídí ImGui::BeginDisabled() / EndDisabled(): prvek
 *   nereaguje a kreslí se tokeny OVL_COL_TEXT_DISABLED a
 *   OVL_COL_BORDER_STRONG (ne ztlumením neprůhledností; styl overlaye má
 *   DisabledAlpha 1). Platí to i pro pasivní prvky: ovl_text_ellipsis(),
 *   ovl_status_chip(), ovl_status_chip_icon() a ovl_icon() berou uvnitř
 *   zakázaného stavu barvu OVL_COL_TEXT_DISABLED bez ohledu na barvu, kterou
 *   dostaly (štítek navíc okraj OVL_COL_BORDER_STRONG a žádný podklad),
 *   ovl_led() se kreslí jako zhasnutá. Text, který má zůstat barevný (důvod
 *   zákazu karty), se proto kreslí před BeginDisabled(). Standardní widgety
 *   ImGui a text kreslený přímo přes ImDrawList se samy neztlumí: kdo je
 *   uvnitř zakázaného stavu kreslí, volí barvu zakázaného prvku sám.
 * - Text, který se do dané šířky nevejde, končí výpustkou a pod ukazatelem
 *   myši ukáže tooltip s plným zněním.
 * - Po návratu každého widgetu je poslední položkou ImGui celý widget
 *   (ImGui::GetItemRectMin() / GetItemRectMax() dají jeho obdélník).
 *
 * @par Vlákna
 * Jen UI vlákno, mezi ImGui::NewFrame() a ImGui::Render().
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

#ifndef OVERLAY_WIDGETS_H
#define OVERLAY_WIDGETS_H

#include <string>

#include "libs/imgui/imgui.h"

/**
 * @brief Ikona vysunutí média kreslená přes ImDrawList.
 *
 * Hlavička IconsLucide.h ji nezná (končí na U+E6FD); písmo lucide.ttf
 * ikonu vysunutí obsahuje na U+E733.
 *
 * Lze předat všude, kde widget bere parametr `icon`.
 */
#define OVL_ICON_EJECT "\x01" "eject"

/** @brief Druh tlačítka ovl_button(). */
typedef enum en_OVL_BUTTON_KIND {
    OVL_BUTTON_NORMAL = 0, /**< běžné tlačítko: hlavní barva textu */
    OVL_BUTTON_PRIMARY,    /**< hlavní akce: ikona a text v barvě akcentu, okraj běžný */
    OVL_BUTTON_DANGER,     /**< nebezpečná akce: červená ikona a text, okraj běžný */
    OVL_BUTTON_DASHED,     /**< nabídka pro prázdné místo ("Insert disk"): čárkovaný okraj, bez výplně */
    OVL_BUTTON_ACTIVE,     /**< právě činná funkce (běžící páska): zelená ikona, text i okraj */
    OVL_BUTTON_TEXT,       /**< vedlejší textová akce ("Clear history"): bez okraje a bez výplně (výplň jen pod myší a při stisku), text barvou vedlejšího textu */
} en_OVL_BUTTON_KIND;

/** @brief Stav, který stavový štítek ovl_status_chip() sděluje barvou. */
typedef enum en_OVL_STATUS {
    OVL_STATUS_NEUTRAL = 0, /**< informace bez stavu (platforma, rychlost) */
    OVL_STATUS_ACTIVE,      /**< činné: zelená */
    OVL_STATUS_WARNING,     /**< pozor (pauza, neuložené změny): jantarová */
    OVL_STATUS_DANGER,      /**< nebezpečí nebo záznam: červená */
} en_OVL_STATUS;

/**
 * @brief Položka rozbalovacího výběru ovl_dropdown().
 *
 * Invariant: položka s section == true nejde zvolit ani zaměřit; její
 * label smí být NULL nebo prázdný (pak se kreslí jen oddělovací linka).
 */
typedef struct st_OVL_DROPDOWN_ITEM {
    const char *label;  /**< přeložený text položky nebo nadpis oddílu */
    bool section;       /**< true = nadpis oddílu nebo oddělovač, ne volba */
    const char *reason; /**< přeložený důvod, proč položku nejde zvolit (tooltip); NULL = jde zvolit */
} st_OVL_DROPDOWN_ITEM;

/* ================================================================
 * Karta a řádek nastavení
 * ================================================================ */

/**
 * @brief Začne kartu: zaoblená plocha s okrajem a volitelným nadpisem.
 *
 * Nadpis se kreslí verzálkami, prostrkaně, vedlejší barvou, pod ním je
 * oddělovací linka. Karta je podřízené okno ImGui se sdílenou navigací
 * (šipky přecházejí mezi kartami); kolečko myši roluje obsah tabu, ne kartu.
 *
 * @param title Nadpis ve tvaru "Text##id"; z celého řetězce se počítá ID karty. Část před "##" smí být prázdná (karta bez nadpisu).
 * @param size  Velikost v pixelech; x <= 0 = zbývající šířka, y <= 0 = výška podle obsahu.
 * @return true = karta je vidět a obsah se má kreslit.
 * @post Vždy (i při false) musí následovat ovl_card_end().
 */
bool ovl_card_begin(const char *title, ImVec2 size);

/**
 * @brief Ukončí kartu začatou ovl_card_begin().
 * @post Poslední položkou ImGui je celá karta.
 */
void ovl_card_end(void);

/**
 * @brief Výška karty pro obsah dané výšky (pro karty s pevnou velikostí ve dvousloupcovém rozvržení).
 * @param content_h  Výška obsahu pod nadpisem v pixelech.
 * @param with_title true = karta má nadpis s oddělovací linkou.
 * @return Výška karty v pixelech při současné jednotce u: vnitřní okraje, nadpis a obsah.
 */
float ovl_card_height(float content_h, bool with_title);

/**
 * @brief Výška jednoho řádku nastavení (ovl_row_begin()) v pixelech při současné jednotce u.
 * @return Celé pixely.
 */
float ovl_row_height(void);

/**
 * @brief Začne řádek nastavení: popisek vlevo, ovládací prvky od pevného sloupce vpravo.
 *
 * Řádek má výšku OVL_ROW_H, mezi řádky téže karty je oddělovací linka.
 * Popisek se do svého sloupce zkracuje výpustkou. Po návratu stojí kurzor
 * na začátku sloupce prvků; další prvky se řadí ImGui::SameLine(). Widgety
 * z tohoto souboru se v řádku samy svisle vystředí.
 *
 * @param label Přeložený popisek; NULL nebo prázdný = řádek bez popisku (prvky začínají vlevo).
 * @post Musí následovat ovl_row_end(). Řádky se nevnořují.
 */
void ovl_row_begin(const char *label);

/**
 * @brief Začne řádek nastavení s daným podílem šířky pro sloupec popisku.
 *
 * Jako ovl_row_begin(); širší sloupec popisku se hodí pro řádky, jejichž
 * jediný prvek je přepínač u pravého okraje.
 *
 * @param label          Přeložený popisek; NULL nebo prázdný = řádek bez popisku.
 * @param label_fraction Podíl šířky řádku pro sloupec popisku (0 až 1); mimo rozsah = výchozí podíl ovl_row_begin().
 * @post Musí následovat ovl_row_end(). Řádky se nevnořují.
 */
void ovl_row_begin_ex(const char *label, float label_fraction);

/**
 * @brief Začne vyšší řádek nastavení: popisek nahoře, ovládací prvky pod ním přes celou šířku.
 *
 * Pro prvek, který se vedle popisku nevejde (segmentová volba se šesti
 * možnostmi). Řádek má výšku ovl_row_stacked_height(); po návratu stojí
 * kurzor vlevo pod popiskem a widgety se svisle středí ve spodním pásu.
 *
 * @param label Přeložený popisek; NULL nebo prázdný = jen pás prvků.
 * @post Musí následovat ovl_row_end(). Řádky se nevnořují.
 */
void ovl_row_begin_stacked(const char *label);

/**
 * @brief Výška řádku ovl_row_begin_stacked() v pixelech při současné jednotce u.
 * @return Celé pixely.
 */
float ovl_row_stacked_height(void);

/**
 * @brief Posune kurzor v řádku tak, aby prvek dané šířky končil u pravého okraje.
 * @param width Šířka prvku v pixelech (např. ovl_toggle_size().x nebo ovl_keycap_size().x).
 * @pre Mezi ovl_row_begin() a ovl_row_end().
 */
void ovl_row_right(float width);

/**
 * @brief Ukončí řádek nastavení.
 * @post Kurzor je na začátku dalšího řádku; poslední položkou ImGui je celý řádek.
 */
void ovl_row_end(void);

/* ================================================================
 * Ovládací prvky
 * ================================================================ */

/**
 * @brief Tlačítko akce v přirozené šířce.
 * @param label  Popisek "Text##id"; část před "##" smí být prázdná (tlačítko jen s ikonou).
 * @param icon   Ikona (ICON_LC_* nebo OVL_ICON_EJECT); NULL = bez ikony.
 * @param keycap Text klávesového štítku se zkratkou, která právě funguje; NULL = bez štítku.
 * @param kind   Druh tlačítka.
 * @return true = tlačítko bylo zvoleno (klik, Enter nebo mezerník při fokusu).
 */
bool ovl_button(const char *label, const char *icon, const char *keycap, en_OVL_BUTTON_KIND kind);

/**
 * @brief Tlačítko akce s danou velikostí.
 *
 * Nevejde-li se klávesový štítek vedle popisku, nekreslí se a zkratka se
 * přesune do tooltipu; nevejde-li se ani popisek, zkrátí se výpustkou.
 * Obsah tlačítka se řadí zleva se stejným odsazením (ikona, popisek) a
 * štítek je vpravo; jen samotná ikona se středí. Zakázané tlačítko
 * (ImGui::BeginDisabled()) klávesový štítek neukazuje: štítek znamená, že
 * zkratka právě funguje; totéž platí pro segmentovou volbu a ovl_keycap().
 *
 * @param label  Viz ovl_button().
 * @param icon   Viz ovl_button().
 * @param keycap Viz ovl_button().
 * @param kind   Druh tlačítka.
 * @param size   Velikost v pixelech; x == 0 = přirozená šířka, x < 0 = zbývající šířka, y <= 0 = OVL_CONTROL_H.
 * @return true = tlačítko bylo zvoleno.
 */
bool ovl_button_ex(const char *label, const char *icon, const char *keycap, en_OVL_BUTTON_KIND kind, ImVec2 size);

/**
 * @brief Přirozená šířka tlačítka akce: šířka, při které ovl_button_ex() nic nezkracuje ani neskrývá.
 * @param label  Popisek "Text##id" (viz ovl_button()).
 * @param icon   Ikona nebo NULL.
 * @param keycap Text klávesového štítku nebo NULL.
 * @return Šířka v pixelech při současné jednotce u (zaokrouhlená nahoru).
 */
float ovl_button_natural_width(const char *label, const char *icon, const char *keycap);

/**
 * @brief Přepínač zapnuto / vypnuto.
 * @param id    Řetězec pro ID prvku (nezobrazuje se).
 * @param value Hodnota; při zvolení se obrátí.
 * @return true = hodnota se změnila (klik, Enter nebo mezerník při fokusu).
 */
bool ovl_toggle(const char *id, bool *value);

/**
 * @brief Velikost přepínače v pixelech (pro ovl_row_right()).
 * @return Šířka a výška přepínače při současné jednotce u.
 */
ImVec2 ovl_toggle_size(void);

/**
 * @brief Segmentová volba jedné z několika možností, v přirozené šířce (nejvýš zbývající).
 * @param id       Řetězec pro ID prvku (nezobrazuje se).
 * @param labels   Přeložené popisky segmentů.
 * @param count    Počet segmentů, 1 až 12 (jako ovl_segmented_ex()).
 * @param selected Index zvoleného segmentu; při zvolení jiného se přepíše.
 * @return true = hodnota se změnila.
 */
bool ovl_segmented(const char *id, const char *const *labels, int count, int *selected);

/**
 * @brief Segmentová volba s klávesovými štítky a danou šířkou.
 *
 * Každý segment je položka navigace: šipky vlevo a vpravo mezi segmenty
 * přecházejí, Enter nebo mezerník segment zvolí. Zvolený segment má plochu
 * OVL_COL_SELECTED a akcentní linku dole. Nevejdou-li se segmenty do
 * šířky, štítky se nekreslí a popisky se zkrátí výpustkou.
 *
 * @param id       Řetězec pro ID prvku.
 * @param labels   Přeložené popisky segmentů.
 * @param keycaps  Texty klávesových štítků segmentů; NULL = bez štítků, NULL prvek = segment bez štítku.
 * @param count    Počet segmentů, 1 až 12 (OVL_SEGMENT_MAX v overlay_widgets.cpp). Větší počet je chyba
 *                 volajícího: v ladicím buildu IM_ASSERT, jinak se segmenty nad mez nekreslí a nejdou zvolit.
 * @param selected Index zvoleného segmentu.
 * @param width    Šířka v pixelech; 0 = přirozená (nejvýš zbývající), < 0 = zbývající.
 * @return true = hodnota se změnila.
 * @post Prvek není širší než daná šířka (width > 0), jinak než zbývající šířka: v úzkém místě se segmenty
 *       zúží pod čtverec a popisky se zkrátí (plné znění v tooltipu). Výjimka: nejméně jeden pixel na segment.
 */
bool ovl_segmented_ex(const char *id, const char *const *labels, const char *const *keycaps, int count, int *selected, float width);

/**
 * @brief Segmentová volba, ve které jsou některé segmenty zakázané s důvodem.
 *
 * Jako ovl_segmented_ex(); segment s důvodem na myš ani klávesnici
 * nereaguje, kreslí se barvami zakázaného prvku a pod ukazatelem myši
 * ukáže důvod v tooltipu. Zvolený segment se nezakazuje (jeho zvolení nic
 * nemění).
 *
 * @param id       Řetězec pro ID prvku.
 * @param labels   Přeložené popisky segmentů.
 * @param keycaps  Texty klávesových štítků segmentů; NULL = bez štítků.
 * @param reasons  Přeložené důvody zákazu segmentů; NULL = žádný segment není zakázaný, NULL prvek = segment je povolený.
 * @param count    Počet segmentů, 1 až 12 (jako ovl_segmented_ex()).
 * @param selected Index zvoleného segmentu.
 * @param width    Šířka v pixelech; 0 = přirozená (nejvýš zbývající), < 0 = zbývající.
 * @return true = hodnota se změnila.
 * @post Šířka prvku jako u ovl_segmented_ex().
 */
bool ovl_segmented_reasons(const char *id, const char *const *labels, const char *const *keycaps, const char *const *reasons, int count, int *selected, float width);

/**
 * @brief Posuvník hodnoty v procentech s číselným údajem vpravo, přes zbývající šířku, krok 1.
 * @param id    Řetězec pro ID prvku.
 * @param value Hodnota; drží se v rozsahu min až max.
 * @param min   Nejmenší hodnota.
 * @param max   Největší hodnota (větší než min).
 * @return true = hodnota se změnila.
 */
bool ovl_slider_percent(const char *id, float *value, float min, float max);

/**
 * @brief Posuvník hodnoty v procentech s danou šířkou a krokem.
 *
 * Myš: tažení nebo klik do drážky (klik na číselný údaj vpravo hodnotu
 * nemění). Klávesnice: při fokusu šipky vlevo a
 * vpravo mění hodnotu o krok (vodorovnou navigaci si prvek po dobu fokusu
 * bere pro sebe; nahoru, dolů a Tab fokus posunou dál), Home a End nastaví
 * krajní hodnoty. Zakázaný posuvník (ImGui::BeginDisabled()) hodnotu
 * volajícího nikdy nezapisuje a vrací false; hodnotu mimo rozsah jen
 * vykreslí omezenou.
 *
 * @param id    Řetězec pro ID prvku.
 * @param value Hodnota; u povoleného prvku se drží v rozsahu min až max a zaokrouhluje se na násobek kroku od min.
 * @param min   Nejmenší hodnota.
 * @param max   Největší hodnota (větší než min).
 * @param step  Krok klávesnice a zaokrouhlení; <= 0 = 1.
 * @param width Šířka celého prvku včetně číselného údaje v pixelech; <= 0 = zbývající šířka.
 * @return true = hodnota se změnila.
 * @post Prvek není širší než daná (nebo zbývající) šířka: v úzkém místě se zkrátí drážka. Výjimka: šířka
 *       pod součtem jezdce, číselného údaje a drážky osmi návrhových bodů; tu prvek přesáhne, volající
 *       proto posuvníku dává aspoň tolik místa.
 */
bool ovl_slider_percent_ex(const char *id, float *value, float min, float max, float step, float width);

/**
 * @brief Posuvník číselné hodnoty s danou jednotkou za číselným údajem.
 *
 * Chování, ovládání a parametry jako ovl_slider_percent_ex(); liší se jen
 * textem jednotky.
 *
 * @param id    Řetězec pro ID prvku.
 * @param value Hodnota.
 * @param min   Nejmenší hodnota.
 * @param max   Největší hodnota (větší než min).
 * @param step  Krok klávesnice a zaokrouhlení; <= 0 = 1.
 * @param width Šířka celého prvku včetně číselného údaje v pixelech; <= 0 = zbývající šířka.
 * @param unit  Text jednotky za číslem ("%", "Hz"; jednotky se nepřekládají); NULL nebo prázdný = jen číslo.
 * @return true = hodnota se změnila.
 */
bool ovl_slider_unit_ex(const char *id, float *value, float min, float max, float step, float width, const char *unit);

/**
 * @brief Posuvník hodnoty v procentech se značkou význačné hodnoty na drážce.
 *
 * Chování, ovládání a parametry jako ovl_slider_percent_ex(). Navíc kreslí
 * na drážce svislou značku v místě hodnoty `mark`, tažení a klik myší se
 * v pásmu `detent` kolem značky přichytí přesně na ni a dvojklik do drážky
 * hodnotu na značku vrátí. Klávesnice krokuje volně (značku lze přejít po
 * krocích).
 *
 * @param id     Řetězec pro ID prvku.
 * @param value  Hodnota.
 * @param min    Nejmenší hodnota.
 * @param max    Největší hodnota (větší než min).
 * @param step   Krok klávesnice a zaokrouhlení; <= 0 = 1.
 * @param width  Šířka celého prvku včetně číselného údaje v pixelech; <= 0 = zbývající šířka.
 * @param mark   Hodnota se značkou; mimo otevřený interval (min, max) se značka nekreslí a prvek se chová jako ovl_slider_percent_ex().
 * @param detent Polovina šířky pásma přichycení v jednotkách hodnoty; <= 0 = bez přichycení.
 * @return true = hodnota se změnila.
 */
bool ovl_slider_percent_mark(const char *id, float *value, float min, float max, float step, float width, float mark, float detent);

/**
 * @brief Rozbalovací výběr jedné z N položek.
 *
 * Zavřený prvek ukazuje text zvolené položky a šipku dolů. Zvolením (klik,
 * Enter nebo mezerník při fokusu) se pod ním (nebo nad ním, není-li dole
 * místo) rozbalí seznam: popup ImGui se stylem overlaye, nejméně tak
 * široký jako prvek, nejvýš tak vysoký, aby zůstal v hlavním viewportu
 * a nad mezí ovl_dropdown_set_bottom_limit() (delší seznam se roluje). V seznamu se pohybuje šipkami nahoru a dolů,
 * Enter nebo mezerník položku zvolí, Esc a klik mimo seznam ho zavřou beze
 * změny; po rozbalení klávesnicí má fokus zvolená položka. Položky s
 * section == true jsou nadpisy oddílů, položka s důvodem nejde zvolit a
 * důvod ukáže v tooltipu. Po dobu rozbalení má fokus popup, ne okno,
 * které prvek obsahuje (viz ovl_dropdown_is_open()).
 *
 * @param id       Řetězec pro ID prvku (nezobrazuje se).
 * @param items    Položky v pořadí zobrazení (platné po dobu volání).
 * @param count    Počet položek (nejméně 1).
 * @param selected Index zvolené položky; hodnota mimo rozsah nebo index oddílu = prvek ukazuje prázdný text.
 *                 Při zvolení jiné položky se přepíše.
 * @param width    Šířka zavřeného prvku v pixelech; <= 0 = zbývající šířka.
 * @return true = uživatel zvolil jinou položku, než byla zvolená.
 */
bool ovl_dropdown(const char *id, const st_OVL_DROPDOWN_ITEM *items, int count, int *selected, float width);

/**
 * @brief Nastaví pro tento snímek dolní mez rozbalených seznamů ovl_dropdown().
 *
 * Seznam rozbalený dolů končí nejvýš na této souřadnici; nevejde-li se, a
 * nad prvkem je víc místa, rozbalí se nahoru, jinak se zkrátí a roluje.
 * Overlay tím drží seznam nad svou patičkou. Mez platí jen pro snímek
 * ImGui, ve kterém byla nastavena; bez ní je mezí dolní hrana hlavního
 * viewportu zmenšená o odstup.
 *
 * @param y Dolní mez v souřadnicích obrazovky; hodnota pod dolní hranou viewportu se nepoužije.
 * @pre Volá se před ovl_dropdown() v témže snímku. Bez kontextu ImGui nedělá nic.
 */
void ovl_dropdown_set_bottom_limit(float y);

/**
 * @brief Byl v tomto nebo v minulém snímku rozbalený seznam některého ovl_dropdown()?
 *
 * Okno, které výběr obsahuje, podle toho nesmí obsloužit Esc: klávesa,
 * která seznam zavřela, by jinak ve stejném snímku zavřela i to okno
 * (fokus se mu vrací hned).
 *
 * @return true = seznam byl rozbalený v tomto nebo v předchozím snímku ImGui.
 */
bool ovl_dropdown_is_open(void);

/**
 * @brief Jen pro testy: obdélník položky naposledy vykresleného rozbaleného seznamu.
 * @param index Index položky (pořadí v poli items).
 * @param min   Výstup: levý horní roh (smí být NULL).
 * @param max   Výstup: pravý dolní roh (smí být NULL).
 * @return true = seznam byl v posledním snímku rozbalený a položka (ne oddíl) vykreslená.
 */
bool ovl_test_dropdown_item_rect(int index, ImVec2 *min, ImVec2 *max);

/* ================================================================
 * Pasivní prvky
 * ================================================================ */

/**
 * @brief Klávesový štítek: název klávesy nebo zkratky v rámečku.
 * @param text Text štítku (názvy kláves se nepřekládají); NULL nebo prázdný = nic.
 */
void ovl_keycap(const char *text);

/**
 * @brief Velikost klávesového štítku v pixelech.
 * @param text Text štítku.
 * @return Šířka (nejméně výška, aby jednoznakový štítek byl čtverec) a výška, které by ovl_keycap() zabral; nuly pro NULL nebo prázdný text.
 */
ImVec2 ovl_keycap_size(const char *text);

/**
 * @brief Vykreslí klávesový štítek bez vlivu na rozvržení (pro prvky složené z více částí).
 * @param dl       Seznam kreslení; NULL = nic.
 * @param pos      Levý horní roh.
 * @param text     Text štítku; NULL nebo prázdný = nic.
 * @param disabled true = štítek patří zakázanému prvku a nekreslí se (štítek znamená "zkratka funguje právě teď").
 */
void ovl_draw_keycap(ImDrawList *dl, ImVec2 pos, const char *text, bool disabled);

/**
 * @brief Text na jednom řádku, při přetečení zkrácený výpustkou, s tooltipem s plným zněním.
 *
 * Kreslí současným písmem a barvou textu ImGui; uvnitř zakázaného stavu
 * (ImGui::BeginDisabled()) barvou OVL_COL_TEXT_DISABLED.
 *
 * @param text      Text; NULL = prázdný.
 * @param max_width Největší šířka v pixelech; <= 0 = zbývající šířka.
 * @post Položka ImGui není širší než max_width.
 */
void ovl_text_ellipsis(const char *text, float max_width);

/**
 * @brief Stavový štítek: krátký text v rámečku, barva podle stavu.
 * @param text   Přeložený text.
 * @param status Stav; uvnitř zakázaného stavu se nepoužije (text OVL_COL_TEXT_DISABLED, okraj OVL_COL_BORDER_STRONG, bez podkladu).
 */
void ovl_status_chip(const char *text, en_OVL_STATUS status);

/**
 * @brief Stavový štítek s ikonou před textem.
 * @param icon   Ikona (ICON_LC_* nebo OVL_ICON_EJECT); NULL = bez ikony.
 * @param text   Přeložený text; prázdný řetězec = štítek se samotnou ikonou (bez mezery za ikonou).
 * @param status Stav; uvnitř zakázaného stavu se nepoužije (jako u ovl_status_chip()).
 */
void ovl_status_chip_icon(const char *icon, const char *text, en_OVL_STATUS status);

/**
 * @brief Velikost stavového štítku v pixelech.
 * @param icon Ikona nebo NULL.
 * @param text Přeložený text; prázdný řetězec = štítek se samotnou ikonou.
 * @return Šířka a výška, které by ovl_status_chip_icon() zabral.
 */
ImVec2 ovl_status_chip_size(const char *icon, const char *text);

/**
 * @brief Kontrolka: plný kroužek, zelený při činnosti, šedý v klidu.
 * @param on true = svítí; uvnitř zakázaného stavu se kreslí vždy jako zhasnutá.
 */
void ovl_led(bool on);

/**
 * @brief Ikona jako samostatná položka velikosti OVL_ICON.
 * @param icon  Ikona (ICON_LC_* nebo OVL_ICON_EJECT); NULL = jen prázdné místo.
 * @param color Barva ikony; uvnitř zakázaného stavu se nepoužije a ikona má barvu OVL_COL_TEXT_DISABLED.
 * @note Bez písma ikon (overlay_style_has_icons() == false) zůstane po ikoně z písma prázdné místo.
 */
void ovl_icon(const char *icon, ImU32 color);

/**
 * @brief Vykreslí ikonu vystředěnou ve čtverci, bez vlivu na rozvržení.
 * @param dl    Seznam kreslení.
 * @param pos   Levý horní roh čtverce.
 * @param size  Strana čtverce v pixelech.
 * @param icon  Ikona (ICON_LC_* nebo OVL_ICON_EJECT); NULL = nic.
 * @param color Barva ikony.
 * @return true = ikona se nakreslila; false = NULL nebo chybí písmo ikon.
 */
bool ovl_draw_icon(ImDrawList *dl, ImVec2 pos, float size, const char *icon, ImU32 color);

/**
 * @brief Tooltip s textem pro poslední položku ImGui, je-li pod ukazatelem myši.
 * @param text Přeložený text; NULL nebo prázdný = nic.
 */
void ovl_tooltip(const char *text);

/**
 * @brief Tooltip pro poslední položku ImGui, i když je zakázaná (důvod zákazu).
 * @param text Přeložený text; NULL nebo prázdný = nic.
 */
void ovl_tooltip_disabled(const char *text);

/* ================================================================
 * Stavebnice vlastních prvků (horní lišta, řádky médií)
 * ================================================================ */

/**
 * @brief Má poslední položka ImGui kreslit obrys fokusu?
 *
 * Pro prvky, které si položku ImGui zakládají samy (ItemAdd()): vrací
 * true, když má položka fokus navigace a kurzor navigace je vidět, nebo
 * když obrys vynutila ovl_test_force_focus_next() (požadavek tím zaniká).
 *
 * @return true = nakreslit obrys ovl_draw_focus_ring().
 * @pre Voláno hned po ItemAdd() prvku.
 */
bool ovl_last_item_focused(void);

/**
 * @brief Nakreslí obrys fokusu (OVL_FOCUS_W, OVL_COL_ACCENT) kolem obdélníku do současného okna.
 * @param min      Levý horní roh prvku.
 * @param max      Pravý dolní roh prvku.
 * @param rounding Zaoblení rohů prvku v pixelech.
 */
void ovl_draw_focus_ring(ImVec2 min, ImVec2 max, float rounding);

/* ================================================================
 * Testy a galerie
 * ================================================================ */

/**
 * @brief Jen pro testy a galerii: příští ovládací prvek se vykreslí s obrysem fokusu.
 *
 * Skutečný fokus navigace může mít v jednom snímku jen jeden prvek; galerie
 * stavů tak ukáže obrys u každého druhu widgetu. Chování prvku se nemění.
 */
void ovl_test_force_focus_next(void);

/**
 * @brief Jen pro testy: barva, kterou naposledy kreslil pasivní prvek.
 * @return Barva textu ovl_text_ellipsis() a ovl_status_chip*(), kroužku ovl_led() nebo ikony ovl_icon(),
 *         podle toho, který z nich se kreslil poslední; 0, když se ještě žádný nekreslil.
 */
ImU32 ovl_test_last_passive_color(void);

/**
 * @brief Text tooltipu tlačítka: viditelná část popisku a případně zkratka v závorce.
 *
 * Skládá se bez omezení délky, text v UTF-8 se tedy nikde neřeže uprostřed
 * znaku. Veřejná kvůli testu.
 *
 * @param label     Začátek popisku; NULL = prázdný.
 * @param label_end Konec viditelné části popisku (před "##").
 * @param keycap    Text klávesového štítku, který se do tlačítka nevešel; NULL nebo prázdný = bez závorky.
 * @return "Popisek" nebo "Popisek (Zkratka)".
 */
std::string ovl_button_tooltip_text(const char *label, const char *label_end, const char *keycap);

#endif /* OVERLAY_WIDGETS_H */
