/**
 * @file   menu_recent.cpp
 * @brief  Podnabídka "Recent" klasického menu: naposledy vložená média jednoho druhu.
 *
 * Popis a kontrakty jsou v menu_recent.h.
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

#include <stdio.h>
#include <string.h>
#include <glib.h>
#ifdef MZTEST_HEADLESS
#include <string>
#include <vector>
#endif

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/menu_recent.h"

/** @brief Nejvyšší počet znaků jména souboru v položce; delší jméno se zkrátí a doplní "...". */
#define MENU_RECENT_NAME_CHARS 40

/**
 * @brief Kopie historie jednoho druhu média pro kreslení podnabídky.
 *
 * Patří jen UI vláknu. Obnovuje se v menu_recent_refresh() při změně čísla
 * stavu historie jádra.
 *
 * @invariant count <= RECENT_MEDIA_MAX; path[i] a label[i] jsou pro
 *            i < count platné řetězce vlastněné kopií (g_free()), ostatní NULL.
 */
struct st_MENU_RECENT_LIST
{
    bool valid;                      /**< kopie byla aspoň jednou naplněna */
    unsigned serial;                 /**< recent_media_serial() z doby naplnění */
    unsigned count;                  /**< počet položek */
    char *path[RECENT_MEDIA_MAX];    /**< celá cesta (UTF-8) */
    char *label[RECENT_MEDIA_MAX];   /**< jméno souboru pro položku (zkrácené) */
};

/** @brief Kopie historie pro každý druh média. */
static st_MENU_RECENT_LIST s_list[RECENT_MEDIA_KIND_COUNT];

#ifdef MZTEST_HEADLESS
/** @brief Obdélník prvku podnabídky zaznamenaný pro testy (jen testovací build). */
struct st_MENU_RECENT_TEST_ITEM
{
    std::string name; /**< jméno prvku */
    ImVec2 min;       /**< levý horní roh */
    ImVec2 max;       /**< pravý dolní roh */
};
/** @brief Prvky podnabídky z posledního kreslení. */
static std::vector<st_MENU_RECENT_TEST_ITEM> s_test_items;

bool imgui_menu_recent_test_item_rect(const char *name, float *x0, float *y0, float *x1, float *y1)
{
    for (const st_MENU_RECENT_TEST_ITEM &it : s_test_items)
    {
        if (it.name == name)
        {
            *x0 = it.min.x;
            *y0 = it.min.y;
            *x1 = it.max.x;
            *y1 = it.max.y;
            return true;
        };
    };
    return false;
}
#endif

/**
 * @brief Zaznamená obdélník naposledy nakresleného prvku pro testy (v aplikaci nic).
 * @param name Jméno prvku.
 * @pre Uvnitř okna ImGui, hned za prvkem.
 */
static void menu_recent_test_record(const char *name)
{
#ifdef MZTEST_HEADLESS
    for (st_MENU_RECENT_TEST_ITEM &it : s_test_items)
    {
        if (it.name == name)
        {
            it.min = ImGui::GetItemRectMin();
            it.max = ImGui::GetItemRectMax();
            return;
        };
    };
    s_test_items.push_back({name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
#else
    (void)name;
#endif
}

/**
 * @brief Obnoví kopii historie druhu, pokud se historie jádra změnila.
 *
 * Číslo stavu se čte před kopírováním: změna během kopírování se pozná při
 * příštím volání. Jméno položky je poslední část cesty, nad
 * MENU_RECENT_NAME_CHARS znaků zkrácená.
 *
 * @param kind Druh média (platný).
 * @pre UI vlákno.
 */
static void menu_recent_refresh(en_RECENT_MEDIA_KIND kind)
{
    st_MENU_RECENT_LIST *l = &s_list[kind];
    const unsigned serial = recent_media_serial();
    if (l->valid && l->serial == serial) return;
    for (unsigned i = 0; i < l->count; i++)
    {
        g_free(l->path[i]);
        g_free(l->label[i]);
        l->path[i] = NULL;
        l->label[i] = NULL;
    };
    l->count = 0;
    for (unsigned i = 0; i < RECENT_MEDIA_MAX; i++)
    {
        char *path = recent_media_dup(kind, i, NULL);
        if (!path) break;
        char *base = g_path_get_basename(path);
        char *label = NULL;
        if (g_utf8_validate(base, -1, NULL) && g_utf8_strlen(base, -1) > MENU_RECENT_NAME_CHARS)
        {
            char *head = g_utf8_substring(base, 0, MENU_RECENT_NAME_CHARS);
            label = g_strconcat(head, "...", NULL);
            g_free(head);
            g_free(base);
        }
        else
        {
            label = base;
        };
        l->path[l->count] = path;
        l->label[l->count] = label;
        l->count++;
    };
    l->serial = serial;
    l->valid = true;
}

bool imgui_menu_recent(en_RECENT_MEDIA_KIND kind, bool enabled, menu_recent_filter_fn filter, void *user, char *out_path, size_t out_size)
{
    if ((int)kind < 0 || kind >= RECENT_MEDIA_KIND_COUNT) return false;
    bool picked = false;
    const bool open = ImGui::BeginMenu(_L("Recent"), enabled);
    menu_recent_test_record("recent_menu");
    if (!open) return false;

    menu_recent_refresh(kind);
    const st_MENU_RECENT_LIST *l = &s_list[kind];
    unsigned shown = 0;
    for (unsigned i = 0; i < l->count; i++)
    {
        if (filter && !filter(l->path[i], user)) continue;
        /* text položky je jméno souboru, ID je pořadí v nabídce (jméno se může opakovat v různých adresářích) */
        char label[512];
        g_snprintf(label, sizeof(label), "%s###recent_%u", l->label[i], shown);
        const bool clicked = ImGui::MenuItem(label);
        char name[32];
        g_snprintf(name, sizeof(name), "recent_%u", shown);
        menu_recent_test_record(name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l->path[i]);
        if (clicked && out_path && strlen(l->path[i]) < out_size)
        {
            g_strlcpy(out_path, l->path[i], out_size);
            picked = true;
        };
        shown++;
    };
    if (shown == 0)
    {
        ImGui::MenuItem(_("(empty)"), NULL, false, false);
        menu_recent_test_record("recent_empty");
    };

    ImGui::Separator();
    bool any = false;
    for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT && !any; k++) any = recent_media_count((en_RECENT_MEDIA_KIND)k) > 0;
    if (ImGui::MenuItem(_L("Clear recent media"), NULL, false, any))
    {
        recent_media_clear();
    };
    menu_recent_test_record("recent_clear");
    ImGui::EndMenu();
    return picked;
}
