/**
 * @file   menu_recent.h
 * @brief  Podnabídka "Recent" klasického menu: naposledy vložená média jednoho druhu.
 *
 * Společná část nabídek disketových mechanik, kazety a Quick Disku
 * (menu_fdcontroller.cpp, menu_cmt.cpp, menu_qdisk.cpp). Kreslí položky
 * z historie jádra (recent_media.h) a vrátí cestu, kterou uživatel zvolil;
 * vložení média provede volající funkcí své nabídky (stejnou jako po výběru
 * v souborovém dialogu).
 *
 * Historie se čte přes kopii (recent_media_dup()) jen při otevřené
 * podnabídce a jen při změně čísla stavu historie (recent_media_serial());
 * zavřená podnabídka nestojí nic. Existenci souborů nabídka nezjišťuje
 * (žádný přístup k disku z UI vlákna): chybějící soubor ohlásí vložení.
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

#ifndef MENU_RECENT_H
#define MENU_RECENT_H

#include <stdbool.h>
#include <stddef.h>

#include "recent_media/recent_media.h"

/**
 * @brief Rozhodne, zda se položka historie v podnabídce nabídne.
 * @param path Cesta položky (UTF-8).
 * @param user Ukazatel volajícího z imgui_menu_recent().
 * @return true = nabídnout, false = vynechat.
 * @note Volá se z UI vlákna při kreslení; nesmí sahat na disk.
 */
typedef bool (*menu_recent_filter_fn)(const char *path, void *user);

/**
 * @brief Nakreslí podnabídku "Recent" s naposledy vloženými médii daného druhu.
 *
 * Položky jdou od naposledy vloženého, nejvýš RECENT_MEDIA_MAX; text je
 * jméno souboru (dlouhé zkrácené), celá cesta je v tooltipu. Bez položek
 * ukáže zakázanou položku "(empty)". Na konci je oddělovač a "Clear recent
 * media", která vyprázdní historii všech druhů (recent_media_clear(), jako
 * overlay menu) a je zakázaná, když je historie všech druhů prázdná.
 *
 * @param kind     Druh média.
 * @param enabled  false = položka podnabídky je zakázaná (nejde otevřít).
 * @param filter   Výběr položek (NULL = všechny).
 * @param user     Ukazatel předaný @p filter.
 * @param out_path Buffer pro zvolenou cestu (UTF-8).
 * @param out_size Velikost @p out_path v bajtech.
 * @return true = uživatel klikl na položku a její cesta je v @p out_path;
 *         cesta, která se do bufferu nevejde, se nevrací (false).
 * @pre UI vlákno, uvnitř otevřené nabídky ImGui (mezi BeginMenu a EndMenu).
 *      Smí se volat pod device_state_lock() (bere jen zámek historie).
 * @post Bez kliknutí se @p out_path nemění.
 */
bool imgui_menu_recent(en_RECENT_MEDIA_KIND kind, bool enabled, menu_recent_filter_fn filter, void *user, char *out_path, size_t out_size);

#ifdef MZTEST_HEADLESS
/**
 * @brief Obdélník prvku podnabídky "Recent" z posledního kreslení (jen testy).
 *
 * Zaznamenané prvky: `recent_menu` (položka podnabídky), `recent_0` až
 * `recent_11` (položky historie podle pořadí v nabídce), `recent_empty`
 * a `recent_clear`.
 *
 * @param name Jméno prvku. @param[out] x0 @param[out] y0 Levý horní roh.
 * @param[out] x1 @param[out] y1 Pravý dolní roh.
 * @return true = prvek byl někdy nakreslen (hodnoty z posledního kreslení).
 */
bool imgui_menu_recent_test_item_rect(const char *name, float *x0, float *y0, float *x1, float *y1);
#endif

#endif /* MENU_RECENT_H */
