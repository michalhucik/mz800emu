/**
 * @file   recent_media.c
 * @brief  Historie naposledy vložených médií: stav pod zámkem, porovnání cest a uložení v konfiguraci.
 *
 * Implementace API z recent_media.h. Stav drží statická struktura chráněná
 * jedním GMutexem. Do INI se zapisuje přes save callbacky elementů sekce
 * [RECENT_MEDIA] (vzor baseui_fchooser_lastdir.c), načítá se jednou v
 * recent_media_config_init().
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

#include <string.h>

#include <glib.h>

#include "recent_media.h"

#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

/**
 * @brief Položka historie.
 *
 * Invariant: path == NULL <=> položka je volná (pak je time 0).
 */
typedef struct st_RECENT_MEDIA_ITEM {
    char *path;  /**< cesta (UTF-8), alokovaná GLibem; vlastní ji historie */
    gint64 time; /**< čas posledního vložení v sekundách od 1. 1. 1970 UTC; 0 = neznámý */
} st_RECENT_MEDIA_ITEM;

/**
 * @brief Stav historie.
 *
 * Přístup jen pod @c mutex (kromě @c serial, který se čte atomicky).
 *
 * Invarianty (mimo načítání konfigurace): položky druhu jsou obsazené
 * souvisle od indexu 0 do count - 1, bez dvou stejných cest (porovnání
 * recent_media_path_equal()); count <= RECENT_MEDIA_MAX; suspended >= 0.
 */
static struct {
    GMutex mutex;                                                          /**< zámek celého stavu */
    st_RECENT_MEDIA_ITEM items[RECENT_MEDIA_KIND_COUNT][RECENT_MEDIA_MAX]; /**< položky od naposledy vložené */
    unsigned count[RECENT_MEDIA_KIND_COUNT];                               /**< počet obsazených položek druhu */
    int suspended;                                                         /**< počet platných pozastavení zápisu */
    gint serial;                                                           /**< čítač změn (recent_media_serial()) */
} g_recent_media;

/** @brief Základ jmen klíčů INI pro druhy média; pořadí odpovídá en_RECENT_MEDIA_KIND. */
static const char *const s_kind_names[RECENT_MEDIA_KIND_COUNT] = {"dsk", "cmt", "qd"};

/**
 * @brief Adresa slotu pro callbacky konfigurace: který údaj které položky element nese.
 */
typedef struct st_RECENT_MEDIA_CFG_SLOT {
    en_RECENT_MEDIA_KIND kind; /**< druh média */
    unsigned index;            /**< pořadí položky */
    bool is_time;              /**< true = element nese čas, false = cestu */
} st_RECENT_MEDIA_CFG_SLOT;

/** @brief Sloty elementů konfigurace (cbdata callbacků); žijí do konce procesu. */
static st_RECENT_MEDIA_CFG_SLOT s_cfg_slots[RECENT_MEDIA_KIND_COUNT][RECENT_MEDIA_MAX][2];

/**
 * @brief Je druh média v rozsahu?
 * @param kind Druh.
 */
static bool recent_media_kind_valid(en_RECENT_MEDIA_KIND kind)
{
    return (int)kind >= 0 && (int)kind < (int)RECENT_MEDIA_KIND_COUNT;
}

/**
 * @brief Klíč cesty pro porovnání.
 *
 * Na Windows: oddělovače sjednocené na '/' a text převedený pro porovnání
 * bez ohledu na velikost písmen (g_utf8_casefold(); u neplatného UTF-8
 * jen malá písmena ASCII). Jinde kopie cesty.
 *
 * @param path Cesta.
 * @return Nově alokovaný klíč (g_free()).
 */
static char *recent_media_path_key(const char *path)
{
#ifdef G_OS_WIN32
    char *tmp = g_strdup(path);
    for (char *c = tmp; *c; c++)
        if (*c == '\\') *c = '/';
    char *key = g_utf8_validate(tmp, -1, NULL) ? g_utf8_casefold(tmp, -1) : g_ascii_strdown(tmp, -1);
    g_free(tmp);
    return key;
#else
    return g_strdup(path);
#endif
}

/**
 * @brief Označují dvě cesty totéž médium (porovnání textů, viz recent_media.h)?
 * @param a První cesta.
 * @param b Druhá cesta.
 */
static bool recent_media_path_equal(const char *a, const char *b)
{
    char *ka = recent_media_path_key(a);
    char *kb = recent_media_path_key(b);
    const bool equal = (strcmp(ka, kb) == 0);
    g_free(ka);
    g_free(kb);
    return equal;
}

/**
 * @brief Index položky s danou cestou.
 * @param kind Platný druh.
 * @param path Cesta.
 * @return Index, nebo -1.
 * @pre Volající drží g_recent_media.mutex.
 */
static int recent_media_find(en_RECENT_MEDIA_KIND kind, const char *path)
{
    for (unsigned i = 0; i < g_recent_media.count[kind]; i++)
        if (recent_media_path_equal(g_recent_media.items[kind][i].path, path)) return (int)i;
    return -1;
}

/**
 * @brief Odstraní položku a posune následující.
 * @param kind  Platný druh.
 * @param index Index obsazené položky.
 * @pre Volající drží g_recent_media.mutex.
 */
static void recent_media_remove_locked(en_RECENT_MEDIA_KIND kind, unsigned index)
{
    st_RECENT_MEDIA_ITEM *items = g_recent_media.items[kind];
    g_free(items[index].path);
    for (unsigned i = index; i + 1 < g_recent_media.count[kind]; i++) items[i] = items[i + 1];
    g_recent_media.count[kind]--;
    items[g_recent_media.count[kind]].path = NULL;
    items[g_recent_media.count[kind]].time = 0;
    g_atomic_int_inc(&g_recent_media.serial);
}

void recent_media_add(en_RECENT_MEDIA_KIND kind, const char *path)
{
    if (!recent_media_kind_valid(kind) || !path || !path[0]) return;
    /* absolutní cesta: historie platí i po startu z jiného pracovního adresáře */
    char *abs = g_path_is_absolute(path) ? g_strdup(path) : g_canonicalize_filename(path, NULL);
    const gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_recent_media.mutex);
    if (g_recent_media.suspended > 0) {
        g_mutex_unlock(&g_recent_media.mutex);
        g_free(abs);
        return;
    }
    st_RECENT_MEDIA_ITEM *items = g_recent_media.items[kind];
    int found = recent_media_find(kind, abs);
    char *keep;
    if (found >= 0) {
        /* existující položka nahoru, v původním zápisu cesty */
        keep = items[found].path;
        g_free(abs);
    } else {
        keep = abs;
        if (g_recent_media.count[kind] < RECENT_MEDIA_MAX) {
            found = (int)g_recent_media.count[kind]++;
        } else {
            found = RECENT_MEDIA_MAX - 1;
            g_free(items[found].path);
        }
    }
    for (int i = found; i > 0; i--) items[i] = items[i - 1];
    items[0].path = keep;
    items[0].time = now;
    g_atomic_int_inc(&g_recent_media.serial);
    g_mutex_unlock(&g_recent_media.mutex);
}

unsigned recent_media_count(en_RECENT_MEDIA_KIND kind)
{
    if (!recent_media_kind_valid(kind)) return 0;
    g_mutex_lock(&g_recent_media.mutex);
    const unsigned n = g_recent_media.count[kind];
    g_mutex_unlock(&g_recent_media.mutex);
    return n;
}

const char *recent_media_get(en_RECENT_MEDIA_KIND kind, unsigned index)
{
    if (!recent_media_kind_valid(kind)) return NULL;
    g_mutex_lock(&g_recent_media.mutex);
    const char *path = (index < g_recent_media.count[kind]) ? g_recent_media.items[kind][index].path : NULL;
    g_mutex_unlock(&g_recent_media.mutex);
    return path;
}

gint64 recent_media_get_time(en_RECENT_MEDIA_KIND kind, unsigned index)
{
    if (!recent_media_kind_valid(kind)) return 0;
    g_mutex_lock(&g_recent_media.mutex);
    const gint64 t = (index < g_recent_media.count[kind]) ? g_recent_media.items[kind][index].time : 0;
    g_mutex_unlock(&g_recent_media.mutex);
    return t;
}

void recent_media_remove(en_RECENT_MEDIA_KIND kind, unsigned index)
{
    if (!recent_media_kind_valid(kind)) return;
    g_mutex_lock(&g_recent_media.mutex);
    if (index < g_recent_media.count[kind]) recent_media_remove_locked(kind, index);
    g_mutex_unlock(&g_recent_media.mutex);
}

bool recent_media_remove_path(en_RECENT_MEDIA_KIND kind, const char *path)
{
    if (!recent_media_kind_valid(kind) || !path || !path[0]) return false;
    g_mutex_lock(&g_recent_media.mutex);
    const int found = recent_media_find(kind, path);
    if (found >= 0) recent_media_remove_locked(kind, (unsigned)found);
    g_mutex_unlock(&g_recent_media.mutex);
    return found >= 0;
}

char *recent_media_dup(en_RECENT_MEDIA_KIND kind, unsigned index, gint64 *time)
{
    if (time) *time = 0;
    if (!recent_media_kind_valid(kind)) return NULL;
    char *path = NULL;
    g_mutex_lock(&g_recent_media.mutex);
    if (index < g_recent_media.count[kind]) {
        path = g_strdup(g_recent_media.items[kind][index].path);
        if (time) *time = g_recent_media.items[kind][index].time;
    }
    g_mutex_unlock(&g_recent_media.mutex);
    return path;
}

unsigned recent_media_serial(void)
{
    return (unsigned)g_atomic_int_get(&g_recent_media.serial);
}

void recent_media_suspend(void)
{
    g_mutex_lock(&g_recent_media.mutex);
    g_recent_media.suspended++;
    g_mutex_unlock(&g_recent_media.mutex);
}

void recent_media_resume(void)
{
    g_mutex_lock(&g_recent_media.mutex);
    if (g_recent_media.suspended > 0) g_recent_media.suspended--;
    g_mutex_unlock(&g_recent_media.mutex);
}

/**
 * @brief Vyprázdní všechny položky.
 * @pre Volající drží g_recent_media.mutex.
 */
static void recent_media_clear_locked(void)
{
    for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT; k++) {
        for (unsigned i = 0; i < RECENT_MEDIA_MAX; i++) {
            g_free(g_recent_media.items[k][i].path);
            g_recent_media.items[k][i].path = NULL;
            g_recent_media.items[k][i].time = 0;
        }
        g_recent_media.count[k] = 0;
    }
    g_atomic_int_inc(&g_recent_media.serial);
}

void recent_media_clear(void)
{
    g_mutex_lock(&g_recent_media.mutex);
    recent_media_clear_locked();
    g_mutex_unlock(&g_recent_media.mutex);
}

void recent_media_reset(void)
{
    g_mutex_lock(&g_recent_media.mutex);
    recent_media_clear_locked();
    g_recent_media.suspended = 0;
    g_mutex_unlock(&g_recent_media.mutex);
}

/* ========================================================================= */
/*                              INI                                          */
/* ========================================================================= */

/**
 * @brief Propagate callback: hodnota z INI -> položka (cbdata = st_RECENT_MEDIA_CFG_SLOT *).
 *
 * Zapisuje přímo na pozici položky; díry a opakované cesty odstraní
 * recent_media_config_init() po načtení celé sekce.
 *
 * @param e    Element konfigurace.
 * @param data Slot elementu.
 */
static void recent_media_propagatecfg(void *e, void *data)
{
    const st_RECENT_MEDIA_CFG_SLOT *slot = (const st_RECENT_MEDIA_CFG_SLOT *)data;
    const char *value = cfgelement_get_text_value((st_CFGELEMENT *)e);
    g_mutex_lock(&g_recent_media.mutex);
    st_RECENT_MEDIA_ITEM *item = &g_recent_media.items[slot->kind][slot->index];
    if (slot->is_time) {
        gint64 t = (value && value[0]) ? g_ascii_strtoll(value, NULL, 10) : 0;
        item->time = (t > 0) ? t : 0;
    } else {
        g_free(item->path);
        item->path = (value && value[0]) ? g_strdup(value) : NULL;
    }
    g_mutex_unlock(&g_recent_media.mutex);
}

/**
 * @brief Save callback: položka -> hodnota elementu před zápisem INI (cbdata = st_RECENT_MEDIA_CFG_SLOT *).
 * @param e    Element konfigurace.
 * @param data Slot elementu.
 */
static void recent_media_savecfg(void *e, void *data)
{
    const st_RECENT_MEDIA_CFG_SLOT *slot = (const st_RECENT_MEDIA_CFG_SLOT *)data;
    char *value = NULL;
    g_mutex_lock(&g_recent_media.mutex);
    if (slot->index < g_recent_media.count[slot->kind]) {
        const st_RECENT_MEDIA_ITEM *item = &g_recent_media.items[slot->kind][slot->index];
        value = slot->is_time ? g_strdup_printf("%" G_GINT64_FORMAT, item->time) : g_strdup(item->path);
    }
    g_mutex_unlock(&g_recent_media.mutex);
    cfgelement_set_text_value((st_CFGELEMENT *)e, value ? value : "");
    g_free(value);
}

void recent_media_config_init(void)
{
    g_mutex_lock(&g_recent_media.mutex);
    recent_media_clear_locked();
    g_mutex_unlock(&g_recent_media.mutex);

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "RECENT_MEDIA");
    for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT; k++) {
        for (unsigned i = 0; i < RECENT_MEDIA_MAX; i++) {
            for (int t = 0; t < 2; t++) {
                st_RECENT_MEDIA_CFG_SLOT *slot = &s_cfg_slots[k][i][t];
                slot->kind = (en_RECENT_MEDIA_KIND)k;
                slot->index = i;
                slot->is_time = (t == 1);
                char *key = g_strdup_printf(t ? "%s_%u_time" : "%s_%u", s_kind_names[k], i + 1);
                /* cfgmodule si jméno kopíruje */
                CFGELM *elm = cfgmodule_register_new_element(cmod, key, CFGENTYPE_TEXT, "");
                g_free(key);
                cfgelement_set_propagate_cb(elm, recent_media_propagatecfg, slot);
                cfgelement_set_save_cb(elm, recent_media_savecfg, slot);
            }
        }
    }

    /* vzor baseui_fchooser_lastdir_config_init(): globální cfgroot_propagate se nevolá */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    /* ruční zásah do INI mohl nechat díry, opakované cesty nebo čas bez cesty: zhutnit (invarianty) */
    g_mutex_lock(&g_recent_media.mutex);
    for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT; k++) {
        st_RECENT_MEDIA_ITEM *items = g_recent_media.items[k];
        unsigned dst = 0;
        for (unsigned i = 0; i < RECENT_MEDIA_MAX; i++) {
            st_RECENT_MEDIA_ITEM item = items[i];
            items[i].path = NULL;
            items[i].time = 0;
            if (!item.path) continue;
            bool dup = false;
            for (unsigned j = 0; j < dst && !dup; j++) dup = recent_media_path_equal(items[j].path, item.path);
            if (dup) g_free(item.path);
            else items[dst++] = item;
        }
        g_recent_media.count[k] = dst;
    }
    g_atomic_int_inc(&g_recent_media.serial);
    g_mutex_unlock(&g_recent_media.mutex);
}
