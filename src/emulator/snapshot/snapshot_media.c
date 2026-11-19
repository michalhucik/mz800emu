/**
 * @file snapshot_media.c
 * @brief Vložená média při načtení snapshotu: režim záznamu vstupů a zpráva o rozdílu.
 *
 * Kontrakt v snapshot_media.h. Stav modulu patří vláknu, které načítá
 * snapshot (vlákno emulátoru).
 *
 * License: GPLv3.
 */

#include "snapshot/snapshot_media.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "baseui/baseui.h"
#include "i18n.h"


/** @brief true = načítá se snapshot záznamu vstupů (snapshot_media_set_replay_load()). */
static bool s_replay_load = false;

/**
 * @brief Řádky rozdílu právě načítaného snapshotu, anglicky (NULL = žádný rozdíl).
 *
 * Plní snapshot_media_notice_add() během load handlerů; slouží i jako klíč,
 * podle kterého se pozná opakování stejného rozdílu.
 */
static GString *s_notice_en = NULL;
/** @brief Tytéž řádky přeložené pro okno se zprávou. */
static GString *s_notice_tr = NULL;
/**
 * @brief Rozdíl zjištěný při minulém načtení (anglické řádky), NULL = minulé načtení bylo bez rozdílu.
 *
 * Vlastní řetězec (g_free()). Opakované načtení se stejným rozdílem (Quick
 * Load ve smyčce) zprávu znovu neukazuje.
 */
static char *s_notice_last = NULL;
/** @brief Počet zpráv o rozdílu ukázaných od startu (snapshot_media_get_notice()). */
static unsigned s_notice_serial = 0;


void snapshot_media_set_replay_load(bool replay_load)
{
    s_replay_load = replay_load;
}


bool snapshot_media_is_replay_load(void)
{
    return s_replay_load;
}


const char *snapshot_media_get_notice(unsigned *serial)
{
    if (serial) *serial = s_notice_serial;
    return s_notice_last;
}


void snapshot_media_notice_begin(void)
{
    if (s_notice_en) g_string_free(s_notice_en, TRUE);
    if (s_notice_tr) g_string_free(s_notice_tr, TRUE);
    s_notice_en = NULL;
    s_notice_tr = NULL;
}


void snapshot_media_notice_names(const char *snap_path, const char *now_path, char **was, char **now)
{
    char *snap_name = snap_path ? g_path_get_basename(snap_path) : NULL;
    char *now_name = now_path ? g_path_get_basename(now_path) : NULL;
    if (snap_name && now_name && strcmp(snap_name, now_name) == 0) {
        g_free(snap_name);
        g_free(now_name);
        snap_name = g_strdup(snap_path);
        now_name = g_strdup(now_path);
    }
    *was = snap_name;
    *now = now_name;
}


void snapshot_media_notice_add(const char *line_en, const char *line_tr)
{
    if (!s_notice_en) {
        s_notice_en = g_string_new(NULL);
        s_notice_tr = g_string_new(NULL);
    }
    if (s_notice_en->len) {
        g_string_append_c(s_notice_en, '\n');
        g_string_append_c(s_notice_tr, '\n');
    }
    g_string_append(s_notice_en, line_en);
    g_string_append(s_notice_tr, line_tr);
}


void snapshot_media_notice_show(void)
{
    if (!s_notice_en) {
        g_free(s_notice_last);
        s_notice_last = NULL;
        return;
    }
    if (!s_notice_last || strcmp(s_notice_last, s_notice_en->str) != 0) {
        fprintf(stderr, "Snapshot [media]: the snapshot was saved with different disks, the inserted disks were kept:\n%s\n",
                s_notice_en->str);
        baseui_show_message_tr(0, N_("The snapshot was saved with different disks:\n%s\nThe inserted disks were kept."),
                               s_notice_tr->str);
        s_notice_serial++;
    }
    g_free(s_notice_last);
    s_notice_last = g_string_free(s_notice_en, FALSE);
    g_string_free(s_notice_tr, TRUE);
    s_notice_en = NULL;
    s_notice_tr = NULL;
}
