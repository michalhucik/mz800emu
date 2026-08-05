/**
 * @file snapshot_quicksave.c
 * @brief Quick Save/Load logika — 3 módy (Basic, Incremental, Rotational)
 *
 * Módy:
 * 0 = Basic — přepisuje jeden soubor {basename}_mzXXX.mzs
 * 1 = Incremental — {basename}_mzXXX-0001.mzs, -0002.mzs, ...
 * 2 = Rotational — jako Incremental, ale po dosažení max_slots smaže nejstarší
 */

#include "mzarch/mzhal.h" /* g_mzhal.arch pro jmena quicksave souboru (mzhal 11c-2c) */
#include "snapshot_quicksave.h"
#include "snapshot_xml.h"
#include "mzarch/mzcommon_config.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>


/**
 * Vytvoří cestu pro Basic mód: {dir}/{basename}_mzXXX.mzs
 */
static void quicksave_basic_path(char *out, size_t out_size,
                                   const char *dir, const char *basename)
{
    /* Precision specifier - shoda s formátem v Incremental/Rotational
     * větvi (~ř. 203), kde out je char[1024] a dir/basename mohou být
     * libovolně dlouhé. Brání format-truncation warningu (-O2). */
    snprintf(out, out_size, "%.700s/%.200s_mz%d.mzs", dir, basename, g_mzhal.arch);
}


int snapshot_quicksave_find_next_number(const char *dir, const char *basename)
{
    int max_num = 0;

    /* Sestavit vzor pro hledání: {basename}_mzXXX-*.mzs */
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s_mz%d-", basename, g_mzhal.arch);
    size_t prefix_len = strlen(pattern);

    GDir *gdir = g_dir_open(dir, 0, NULL);
    if (!gdir)
        return 1;

    const gchar *name;
    while ((name = g_dir_read_name(gdir)) != NULL) {
        if (g_str_has_prefix(name, pattern)) {
            /* Extrahovat číslo ze jména */
            const char *num_str = name + prefix_len;
            char *endptr = NULL;
            long num = strtol(num_str, &endptr, 10);
            /* Ověřit, že po čísle je .mzs */
            if (endptr && g_strcmp0(endptr, ".mzs") == 0 && num > 0) {
                if (num > max_num)
                    max_num = (int)num;
            }
        }
    }

    g_dir_close(gdir);
    return max_num + 1;
}


bool snapshot_quicksave_find_latest(char *out_path, size_t out_size,
                                     const char *dir, const char *basename,
                                     int mode)
{
    if (mode == 0) {
        /* Basic mód — pevný soubor */
        quicksave_basic_path(out_path, out_size, dir, basename);
        return g_file_test(out_path, G_FILE_TEST_EXISTS);
    }

    /* Incremental/Rotational — najít soubor s nejvyšším číslem */
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s_mz%d-", basename, g_mzhal.arch);
    size_t prefix_len = strlen(pattern);

    int max_num = 0;
    char best_name[512] = "";

    GDir *gdir = g_dir_open(dir, 0, NULL);
    if (!gdir)
        return false;

    const gchar *name;
    while ((name = g_dir_read_name(gdir)) != NULL) {
        if (g_str_has_prefix(name, pattern)) {
            const char *num_str = name + prefix_len;
            char *endptr = NULL;
            long num = strtol(num_str, &endptr, 10);
            if (endptr && g_strcmp0(endptr, ".mzs") == 0 && num > 0) {
                if (num > max_num) {
                    max_num = (int)num;
                    snprintf(best_name, sizeof(best_name), "%s", name);
                }
            }
        }
    }

    g_dir_close(gdir);

    if (max_num == 0)
        return false;

    snprintf(out_path, out_size, "%s/%s", dir, best_name);
    return true;
}


void snapshot_quicksave_cleanup_oldest(const char *dir, const char *basename,
                                        int max_slots)
{
    if (max_slots <= 0)
        return;

    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s_mz%d-", basename, g_mzhal.arch);
    size_t prefix_len = strlen(pattern);

    /* Sesbírat všechny soubory s čísly */
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    GArray *numbers = g_array_new(FALSE, FALSE, sizeof(int));

    GDir *gdir = g_dir_open(dir, 0, NULL);
    if (!gdir) {
        g_ptr_array_free(files, TRUE);
        g_array_free(numbers, TRUE);
        return;
    }

    const gchar *name;
    while ((name = g_dir_read_name(gdir)) != NULL) {
        if (g_str_has_prefix(name, pattern)) {
            const char *num_str = name + prefix_len;
            char *endptr = NULL;
            long num = strtol(num_str, &endptr, 10);
            if (endptr && g_strcmp0(endptr, ".mzs") == 0 && num > 0) {
                g_ptr_array_add(files, g_strdup(name));
                int n = (int)num;
                g_array_append_val(numbers, n);
            }
        }
    }

    g_dir_close(gdir);

    /* Smazat nejstarší pokud je jich víc než max_slots */
    while ((int)files->len > max_slots) {
        /* Najít index s nejnižším číslem */
        int min_idx = 0;
        int min_val = g_array_index(numbers, int, 0);
        for (guint i = 1; i < numbers->len; i++) {
            int val = g_array_index(numbers, int, i);
            if (val < min_val) {
                min_val = val;
                min_idx = (int)i;
            }
        }

        /* Smazat soubor */
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s",
                 dir, (const char *)g_ptr_array_index(files, min_idx));
        g_remove(filepath);

        /* Odstranit z polí */
        g_ptr_array_remove_index(files, min_idx);
        g_array_remove_index(numbers, min_idx);
    }

    g_ptr_array_free(files, TRUE);
    g_array_free(numbers, TRUE);
}


en_SNAPSHOT_RESULT snapshot_quicksave(void)
{
    const char *dir = g_snapshot_settings.default_directory;
    const char *basename = g_snapshot_settings.quicksave_filename;
    int mode = g_snapshot_settings.quicksave_mode;

    /* Fallback na aktuální adresář pokud default_directory není nastaveno */
    if (dir[0] == '\0') {
        dir = ".";
    } else {
        /* Vytvořit adresář pokud neexistuje */
        g_mkdir_with_parents(dir, 0755);
    }

    char path[1024];
    en_SNAPSHOT_RESULT result;

    switch (mode) {
        case 0: /* Basic */
            quicksave_basic_path(path, sizeof(path), dir, basename);
            result = snapshot_save(path, "Quick Save");
            break;

        case 1: /* Incremental */
        {
            int next = snapshot_quicksave_find_next_number(dir, basename);
            snprintf(path, sizeof(path), "%.700s/%.200s_mz%d-%04d.mzs",
                     dir, basename, g_mzhal.arch, next);
            result = snapshot_save(path, "Quick Save");
            break;
        }

        case 2: /* Rotational */
        {
            int next = snapshot_quicksave_find_next_number(dir, basename);
            snprintf(path, sizeof(path), "%.700s/%.200s_mz%d-%04d.mzs",
                     dir, basename, g_mzhal.arch, next);
            result = snapshot_save(path, "Quick Save");
            if (result == SNAPSHOT_OK) {
                snapshot_quicksave_cleanup_oldest(dir, basename,
                                                   g_snapshot_settings.quicksave_max_slots);
            }
            break;
        }

        default:
            return SNAPSHOT_ERR_IO;
    }

    if (result == SNAPSHOT_OK) {
        fprintf(stderr, "Snapshot: Quick Save written to '%s'\n", path);
    }

    return result;
}


en_SNAPSHOT_RESULT snapshot_quickload(void)
{
    const char *dir = g_snapshot_settings.default_directory;
    if (dir[0] == '\0') dir = ".";
    const char *basename = g_snapshot_settings.quicksave_filename;
    int mode = g_snapshot_settings.quicksave_mode;

    char path[1024];
    if (!snapshot_quicksave_find_latest(path, sizeof(path), dir, basename, mode)) {
        SNAP_ERR("quickload", "no quicksave file found");
        return SNAPSHOT_ERR_MISSING_FILE;
    }

    fprintf(stderr, "Snapshot: Quick Load z '%s'\n", path);
    return snapshot_load(path);
}
