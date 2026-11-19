/**
 * @file   overlay_style.h
 * @brief  Vizuální systém overlay menu: tokeny barev a rozměrů, jednotka u, písma a ikony.
 *
 * Barvy jsou kroky škál Radix Colors (tmavé škály slate, blue, green, amber
 * a red; hodnoty ověřené proti src/dark.ts repa radix-ui/colors, commit
 * dbdb8547 z 2025-12-17). Každý token nese jméno role, ne jméno barvy;
 * krok škály je v komentáři.
 *
 * Rozměry jsou v návrhových bodech (koncepty jsou kreslené pro 1280 x 720)
 * a na pixely se převádějí jednotkou `u = min(šířka / 1280, výška / 720)`,
 * nejméně OVL_UNIT_MIN (overlay_unit(), overlay_px()). Velikost písma se
 * navíc nikdy nedostane pod OVL_FONT_MIN_PX skutečných pixelů
 * (overlay_font_px()).
 *
 * Písma: Inter Regular a SemiBold (text), JetBrains Mono (jména souborů),
 * ikony Lucide sloučené jen do písem overlaye (rozsah Lucide se překrývá
 * se soukromým rozsahem mzglyphs.ttf, do základního písma UI se proto
 * neslučuje; z písem textu se soukromá oblast Unicode vylučuje, protože
 * Inter má na kódových bodech ikon vlastní glyfy). Evidence licencí je v
 * ui_resources/imgui/fonts/LICENSES.md. Chybí-li soubor písma, vrací overlay_font_*() základní písmo
 * ImGui a ikony se nekreslí (overlay_style_has_icons()).
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

#ifndef OVERLAY_STYLE_H
#define OVERLAY_STYLE_H

#include "libs/imgui/imgui.h"

/* ================================================================
 * Barvy (ImU32, neprůhledné)
 * ================================================================ */

/** @brief Plocha overlaye nad obrazem emulátoru (slate 1); kreslí se s krytím OVL_PANEL_OPACITY. */
#define OVL_COL_PANEL IM_COL32(0x11, 0x11, 0x13, 0xFF)
/** @brief Plocha karty (slate 2). */
#define OVL_COL_CARD IM_COL32(0x18, 0x19, 0x1B, 0xFF)
/** @brief Plocha ovládacího prvku a prvku pod ukazatelem myši (slate 3). */
#define OVL_COL_RAISED IM_COL32(0x21, 0x22, 0x25, 0xFF)
/** @brief Plocha vybraného prvku, např. zvoleného segmentu (slate 4). */
#define OVL_COL_SELECTED IM_COL32(0x27, 0x2A, 0x2D, 0xFF)
/** @brief Plocha stisknutého prvku (slate 5). */
#define OVL_COL_PRESSED IM_COL32(0x2E, 0x31, 0x35, 0xFF)
/** @brief Okraj karty a oddělovací linky (slate 6). */
#define OVL_COL_BORDER IM_COL32(0x36, 0x3A, 0x3F, 0xFF)
/** @brief Okraj klávesového štítku, stavového štítku a zakázaného prvku (slate 7). */
#define OVL_COL_BORDER_STRONG IM_COL32(0x43, 0x48, 0x4E, 0xFF)
/** @brief Vyplněná část drážky zakázaného posuvníku (slate 8); informaci nenese, proti kartě má jen 2,81:1. */
#define OVL_COL_MUTED_DIM IM_COL32(0x5A, 0x61, 0x69, 0xFF)
/** @brief Zhasnutá kontrolka, tečka zakázaného stavu a drážka posuvníku (slate 9; kontrast proti kartě nejméně 3:1). */
#define OVL_COL_MUTED IM_COL32(0x69, 0x6E, 0x77, 0xFF)
/** @brief Okraj ovládacího prvku: tlačítko, pole, přepínač, segmentová volba (slate 9; kontrast proti kartě nejméně 3:1). */
#define OVL_COL_BORDER_CONTROL IM_COL32(0x69, 0x6E, 0x77, 0xFF)
/** @brief Text zakázaného prvku (slate 10; kontrast proti kartě nejméně 3:1). */
#define OVL_COL_TEXT_DISABLED IM_COL32(0x77, 0x7B, 0x84, 0xFF)
/** @brief Vedlejší text: popisky, nápověda, nadpisy karet (slate 11). */
#define OVL_COL_TEXT_SECONDARY IM_COL32(0xB0, 0xB4, 0xBA, 0xFF)
/** @brief Hlavní text (slate 12). */
#define OVL_COL_TEXT IM_COL32(0xED, 0xEE, 0xF0, 0xFF)
/** @brief Akcent: obrys fokusu, podtržení aktivního tabu a zvoleného segmentu, zapnutý přepínač, výplň posuvníku (blue 9). */
#define OVL_COL_ACCENT IM_COL32(0x00, 0x90, 0xFF, 0xFF)
/** @brief Text a ikona v barvě akcentu, např. tlačítko OVL_BUTTON_PRIMARY (blue 11). */
#define OVL_COL_ACCENT_TEXT IM_COL32(0x70, 0xB8, 0xFF, 0xFF)
/** @brief Stav "činné": kontrolka mechaniky, běžící páska (green 9). */
#define OVL_COL_GREEN IM_COL32(0x30, 0xA4, 0x6C, 0xFF)
/** @brief Text stavu "činné" (green 11). */
#define OVL_COL_GREEN_TEXT IM_COL32(0x3D, 0xD6, 0x8C, 0xFF)
/** @brief Stav "pozor": neuložené změny, pauza; i jako text (amber 9). */
#define OVL_COL_AMBER IM_COL32(0xFF, 0xC5, 0x3D, 0xFF)
/** @brief Nebezpečná akce a záznam, jen ikony a okraje (red 9). */
#define OVL_COL_RED IM_COL32(0xE5, 0x48, 0x4D, 0xFF)
/** @brief Text nebezpečné akce (red 11; kontrast proti kartě nejméně 4,5:1). */
#define OVL_COL_RED_TEXT IM_COL32(0xFF, 0x95, 0x92, 0xFF)
/** @brief Jezdec přepínače a posuvníku (bílá). */
#define OVL_COL_KNOB IM_COL32(0xFF, 0xFF, 0xFF, 0xFF)

/** @brief Krytí plochy overlaye nad obrazem emulátoru (0 až 1). */
#define OVL_PANEL_OPACITY 0.85f
/** @brief Krytí plochy overlaye na tabu Display: obraz pod ním má být vidět, aby se změna barev a scanlines ukázala hned. */
#define OVL_PANEL_OPACITY_PREVIEW 0.55f

/* ================================================================
 * Rozměry v návrhových bodech (násobí se jednotkou u)
 * ================================================================ */

/** @brief Šířka návrhu v bodech. */
#define OVL_DESIGN_WIDTH 1280.0f
/** @brief Výška návrhu v bodech. */
#define OVL_DESIGN_HEIGHT 720.0f
/** @brief Nejmenší jednotka u; v menším okně se obsah roluje. */
#define OVL_UNIT_MIN 0.85f
/** @brief Nejmenší velikost písma (velikost em) ve skutečných pixelech. */
#define OVL_FONT_MIN_PX 12.0f
/**
 * @brief Poměr výšky řádku k velikosti em u písma Inter (hhea: ascender 1984, descender -494, 2048 jednotek na em).
 *
 * ImGui udává velikost písma jako výšku řádku (stbtt_ScaleForPixelHeight():
 * ascent - descent), tokeny OVL_FONT_* jsou velikosti em jako v konceptech.
 */
#define OVL_FONT_LINE_PER_EM_UI 1.21f
/** @brief Poměr výšky řádku k velikosti em u písma JetBrains Mono (hhea: ascender 1020, descender -300, 1000 jednotek na em). */
#define OVL_FONT_LINE_PER_EM_MONO 1.32f
/** @brief Návrhová šířka, pod kterou se dvousloupcové rozvržení skládá do jednoho sloupce (overlay_is_narrow()). */
#define OVL_NARROW_WIDTH 960.0f

/** @brief Krok mřížky rozestupů. */
#define OVL_GRID 8.0f
/** @brief Šířka pruhu posuvníku rolované plochy v jednotkách overlaye (včetně okrajů OVL_SCROLLBAR_PAD po obou stranách jezdce). */
#define OVL_SCROLLBAR_W 12.0f
/** @brief Okraj mezi jezdcem posuvníku a hranou jeho pruhu; jezdec má šířku OVL_SCROLLBAR_W - 2 * OVL_SCROLLBAR_PAD = 8 jednotek. */
#define OVL_SCROLLBAR_PAD 2.0f
/** @brief Vnější okraj obsahu tabu. */
#define OVL_MARGIN 32.0f
/** @brief Výška horní lišty (asi 8 % výšky návrhu). */
#define OVL_TOPBAR_H 56.0f
/** @brief Výška patičky (asi 5 % výšky návrhu). */
#define OVL_FOOTER_H 36.0f
/** @brief Vodorovný okraj horní lišty a patičky. */
#define OVL_BAR_EDGE 16.0f
/** @brief Zaoblení rohů karty. */
#define OVL_RADIUS_CARD 8.0f
/** @brief Zaoblení rohů tlačítka, segmentové volby a obrysu fokusu. */
#define OVL_RADIUS_CONTROL 6.0f
/** @brief Zaoblení rohů klávesového a stavového štítku. */
#define OVL_RADIUS_CHIP 4.0f
/** @brief Tloušťka okraje (nejméně jeden skutečný pixel, overlay_border_px()). */
#define OVL_BORDER_W 1.0f
/** @brief Tloušťka obrysu fokusu a akcentních linek. */
#define OVL_FOCUS_W 2.0f
/** @brief Vnitřní okraj karty. */
#define OVL_CARD_PAD 16.0f
/** @brief Výška ovládacího prvku (tlačítko, segmentová volba). */
#define OVL_CONTROL_H 36.0f
/** @brief Výška řádku nastavení (ovl_row_begin()). */
#define OVL_ROW_H 52.0f
/** @brief Velikost ikony. */
#define OVL_ICON 20.0f

/** @brief Velikost písma: popisek tabu (SemiBold). */
#define OVL_FONT_TAB 16.0f
/** @brief Velikost písma: nadpis karty (verzálky, prostrkaně, vedlejší barva). */
#define OVL_FONT_CARD_TITLE 13.0f
/** @brief Velikost písma: hlavní hodnota. */
#define OVL_FONT_VALUE 18.0f
/** @brief Velikost písma: běžný text a popisky prvků. */
#define OVL_FONT_BODY 14.0f
/** @brief Velikost písma: nápověda, klávesové a stavové štítky. */
#define OVL_FONT_HINT 12.0f

/* ================================================================
 * Jednotka a převody
 * ================================================================ */

/**
 * @brief Spočítá jednotku u pro danou plochu overlaye.
 * @param size Velikost plochy v pixelech.
 * @return min(šířka / OVL_DESIGN_WIDTH, výška / OVL_DESIGN_HEIGHT), nejméně OVL_UNIT_MIN.
 */
float overlay_unit_for(ImVec2 size);

/**
 * @brief Nastaví plochu, pro kterou se počítá jednotka u.
 *
 * Volá imgui_overlay_draw() na začátku kreslení; widgety pak čtou
 * overlay_unit(). Hodnota platí do dalšího volání.
 *
 * @param size Velikost plochy overlaye v pixelech.
 */
void overlay_style_set_viewport(ImVec2 size);

/**
 * @brief Jednotka u plochy nastavené overlay_style_set_viewport().
 * @return Počet pixelů na návrhový bod; před prvním nastavením 1.
 */
float overlay_unit(void);

/**
 * @brief Převede rozměr z návrhových bodů na pixely.
 * @param design Rozměr v návrhových bodech.
 * @return design * overlay_unit().
 */
float overlay_px(float design);

/**
 * @brief Tloušťka okraje v pixelech.
 * @return OVL_BORDER_W * overlay_unit() zaokrouhlené na celé pixely, nejméně 1.
 */
float overlay_border_px(void);

/**
 * @brief Velikost písma pro ImGui::PushFont() a ImDrawList::AddText() tak, aby velikost em odpovídala návrhové velikosti.
 *
 * Jediné místo převodu tokenu (velikost em v návrhových bodech) na velikost,
 * kterou čeká ImGui (výška řádku písma): em = design * overlay_unit(),
 * nejméně OVL_FONT_MIN_PX, krát poměr výšky řádku k em daného písma
 * (OVL_FONT_LINE_PER_EM_MONO pro overlay_font_mono(), jinak
 * OVL_FONT_LINE_PER_EM_UI; náhradní základní písmo aplikace se počítá jako
 * Inter). Vrácená hodnota je zároveň výška řádku textu, se kterou se počítá
 * rozvržení.
 *
 * @param font   Písmo, kterým se bude kreslit; NULL = písmo textu overlaye.
 * @param design Velikost písma (em) v návrhových bodech (OVL_FONT_*).
 * @return Velikost písma ImGui (výška řádku) v celých pixelech.
 */
float overlay_font_size(ImFont *font, float design);

/**
 * @brief Velikost písma textu overlaye (overlay_font_ui(), overlay_font_ui_bold()) pro návrhovou velikost.
 * @param design Velikost písma (em) v návrhových bodech (OVL_FONT_*).
 * @return overlay_font_size(NULL, design): výška řádku v celých pixelech.
 */
float overlay_font_px(float design);

/**
 * @brief Je plocha overlaye užší než dvousloupcové rozvržení?
 * @return true = šířka plochy v návrhových bodech je menší než OVL_NARROW_WIDTH; taby mají skládat sloupce pod sebe.
 */
bool overlay_is_narrow(void);

/**
 * @brief Barva tokenu s danou neprůhledností.
 * @param col   Barva (OVL_COL_*).
 * @param alpha Neprůhlednost 0 až 1.
 * @return Barva s nahrazenou složkou alfa.
 */
ImU32 overlay_color_alpha(ImU32 col, float alpha);

/* ================================================================
 * Písma
 * ================================================================ */

/**
 * @brief Přidá písma overlaye do atlasu ImGui.
 *
 * Z adresáře načte Inter-Regular.ttf, Inter-SemiBold.ttf a
 * JetBrainsMono-Regular.ttf jako samostatná písma (ne sloučená do
 * základního). Do obou řezů Inter sloučí ikony lucide.ttf a do všech tří
 * písem, je-li v adresáři, japonské znaky z NotoSansJP-Regular.ttf. Obsah
 * souborů lucide.ttf a NotoSansJP-Regular.ttf se čte jednou a sdílí ho
 * všechny zdroje (overlay_style_add_shared_font(); NotoSansJP má asi 16 MB
 * a stejný buffer používá i základní písmo UI). Z písem textu se
 * neberou glyfy soukromé oblasti Unicode (patří ikonám). Chybějící soubor se tiše
 * přeskočí: příslušné overlay_font_*() pak vrací náhradní písmo.
 *
 * @param atlas    Atlas písem aktuálního kontextu ImGui; NULL = jen zapomene dříve načtená písma.
 * @param font_dir Adresář se soubory písem; NULL nebo neexistující = nic se nenačte.
 * @return true = načetla se všechna tři písma i ikony.
 * @pre Volá se při inicializaci kontextu ImGui po přidání základního písma (to musí zůstat první v atlasu).
 * @post Dříve vrácené ukazatele overlay_font_*() z předchozího volání se už nepoužívají.
 */
bool overlay_style_load_fonts(ImFontAtlas *atlas, const char *font_dir);

/**
 * @brief Obsah souboru písma ve sdílené paměti; soubor se z disku čte jen při prvním volání.
 *
 * Paměť patří vizuálnímu systému a drží se do konce procesu, takže ji smí
 * používat libovolný počet zdrojů písem v libovolném atlasu
 * (`ImFontConfig::FontDataOwnedByAtlas = false`).
 *
 * @param path Cesta souboru písma (klíč sdílení: stejná cesta = stejná paměť).
 * @param[out] size Velikost obsahu v bajtech, 0 při neúspěchu; smí být NULL.
 * @return Obsah souboru; NULL = soubor chybí, nejde přečíst, je prázdný nebo je tabulka sdílených souborů plná.
 *         Neúspěch se neeviduje, další volání čte znovu.
 * @pre UI vlákno.
 */
const void *overlay_style_font_file_data(const char *path, int *size);

/**
 * @brief Přidá do atlasu zdroj písma ze souboru, jehož obsah se drží ve sdílené paměti.
 *
 * Náhrada ImFontAtlas::AddFontFromFileTTF() pro soubor, který používá víc
 * zdrojů (slučování téhož souboru do několika písem): obsah se načte jednou
 * (overlay_style_font_file_data()) a zdroj ho nevlastní. Vše ostatní je
 * stejné jako u AddFontFromFileTTF(): nastavení se bere z @p cfg, jméno
 * zdroje je jméno souboru bez cesty a rozsahy glyfů z @p glyph_ranges.
 *
 * @param atlas        Atlas písem; NULL = nic.
 * @param path         Cesta souboru písma.
 * @param size_pixels  Velikost písma v pixelech.
 * @param cfg          Nastavení zdroje (zkopíruje se; FontDataOwnedByAtlas se přepíše na false); NULL = výchozí.
 * @param glyph_ranges Rozsahy glyfů (musí žít po dobu života atlasu) nebo NULL.
 * @return Přidané písmo (při cfg->MergeMode cílové písmo); NULL = soubor nejde načíst (atlas se nemění).
 * @pre UI vlákno; atlas není zamčený.
 */
ImFont *overlay_style_add_shared_font(ImFontAtlas *atlas, const char *path, float size_pixels, const ImFontConfig *cfg, const ImWchar *glyph_ranges);

/**
 * @brief Jen pro testy: počet čtení souboru písma z disku do sdílené paměti od startu procesu.
 * @return Počet čtení (úspěšných i s prázdným souborem).
 */
int overlay_style_test_font_file_loads(void);

/**
 * @brief Písmo běžného textu overlaye (Inter Regular s ikonami Lucide).
 * @return Písmo; není-li načtené, základní písmo ImGui. NULL jen bez kontextu ImGui.
 */
ImFont *overlay_font_ui(void);

/**
 * @brief Tučnější písmo overlaye (Inter SemiBold s ikonami Lucide).
 * @return Písmo; není-li načtené, overlay_font_ui().
 */
ImFont *overlay_font_ui_bold(void);

/**
 * @brief Neproporcionální písmo pro jména souborů (JetBrains Mono se sloučenými japonskými znaky).
 * @return Písmo; není-li načtené, overlay_font_ui().
 */
ImFont *overlay_font_mono(void);

/**
 * @brief Jsou k dispozici ikony Lucide?
 * @return true = lucide.ttf je sloučený do písem overlaye; false = ikony z písma se nekreslí.
 */
bool overlay_style_has_icons(void);

/* ================================================================
 * Styl ImGui
 * ================================================================ */

/**
 * @brief Nastaví styl ImGui pro kreslení overlaye (barvy textu, tooltipu a posuvníku, rozestupy, písmo).
 *
 * Zakázané prvky se neprůhledností netlumí (DisabledAlpha 1): widgety pro
 * ně používají vlastní tokeny s ověřeným kontrastem.
 *
 * @pre Plocha je nastavená (overlay_style_set_viewport()); mezi ImGui::NewFrame() a ImGui::Render().
 * @post Zásobníky stylů a písem ImGui jsou hlubší; vrací je overlay_style_pop().
 */
void overlay_style_push(void);

/**
 * @brief Vrátí styl ImGui nastavený overlay_style_push().
 * @pre Párové volání po overlay_style_push().
 */
void overlay_style_pop(void);

#endif /* OVERLAY_STYLE_H */
