/**
 * @file rom_request.c
 * @brief Zásahy GUI do ROM přes emu vlákno (kontrakt v rom_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): výběr ROM i[0] = index řádku
 * v g_rom_config->roms. Nastavení uživatelské ROM se do argumentů nevejde
 * (až osm cest), proto čeká v kopii modulu (s_pending) pod zámkem a zásah
 * si ji převezme.
 */

#include <glib.h>
#include <string.h>

#include "rom.h"
#include "rom_request.h"
#include "device_request.h"


/**
 * @brief Čekající nastavení uživatelské ROM (vlastní kopie řetězců).
 * @invariant Řetězce jsou NULL nebo alokované g_strdup(); platné jen při
 *            `s_pending_set`; chrání je s_pending_lock.
 */
static st_ROM_USER_DEFINED_SETTINGS s_pending;
/** @brief true = s_pending čeká na převzetí emu vláknem. */
static bool s_pending_set = false;
/** @brief Zámek s_pending a s_pending_set. */
static GMutex s_pending_lock;


/** @brief Uvolní řetězce nastavení vlastněné modulem a vynuluje ho. */
static void rom_request_settings_free(st_ROM_USER_DEFINED_SETTINGS *s)
{
    g_free((char *)s->allinone_fp);
    g_free((char *)s->mz700_fp);
    g_free((char *)s->cgrom_fp);
    g_free((char *)s->mz800_fp);
    g_free((char *)s->cmthack_allinone_fp);
    g_free((char *)s->cmthack_mz700_fp);
    g_free((char *)s->cmthack_cgrom_fp);
    g_free((char *)s->cmthack_mz800_fp);
    memset(s, 0, sizeof(*s));
}


/** @brief Index řádku v g_rom_config->roms, -1 = řádek tam není. */
static int rom_request_row_index(const st_ROM_CONFIG_ROW *row)
{
    int idx = 0;
    for (const st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++, idx++)
    {
        if (r == row) return idx;
    };
    return -1;
}


/** @brief Zásah: výběr ROM (i[0] index řádku). */
static int rom_request_exec_select(const st_DEVICE_REQ_ARGS *a)
{
    rom_reinstall(&g_rom_config->roms[a->i[0]]);
    return 0;
}


uint32_t rom_request_select(const st_ROM_CONFIG_ROW *row)
{
    if (!row || row->type != ROM_CONFIG_TYPE_ROM) return 0;
    int idx = rom_request_row_index(row);
    if (idx < 0) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = idx;
    return device_request_submit("rom_select", rom_request_exec_select, &a);
}


/** @brief Zásah: převezme čekající nastavení uživatelské ROM a použije ho. */
static int rom_request_exec_set_user_defined(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    st_ROM_USER_DEFINED_SETTINGS s;
    g_mutex_lock(&s_pending_lock);
    bool have = s_pending_set;
    s = s_pending;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending_set = false;
    g_mutex_unlock(&s_pending_lock);
    /* Dřívější zásah už převzal novější nastavení. */
    if (!have) return 0;

    g_rom.user_defined_allinone = s.allinone;
    rom_set_user_defined_filepath(&g_rom.user_defined_allinone_fp, (char *)s.allinone_fp);
    rom_set_user_defined_filepath(&g_rom.user_defined_mz700_fp, (char *)s.mz700_fp);
    rom_set_user_defined_filepath(&g_rom.user_defined_cgrom_fp, (char *)s.cgrom_fp);
    rom_set_user_defined_filepath(&g_rom.user_defined_mz800_fp, (char *)s.mz800_fp);

    g_rom.user_defined_cmthack_type = s.cmthack_type;
    g_rom.user_defined_cmthack_allinone = s.cmthack_allinone;
    rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_allinone_fp, (char *)s.cmthack_allinone_fp);
    rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_mz700_fp, (char *)s.cmthack_mz700_fp);
    rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_cgrom_fp, (char *)s.cmthack_cgrom_fp);
    rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_mz800_fp, (char *)s.cmthack_mz800_fp);

    /* Vynutit znovunačtení ROM ze souborů */
    g_rom.user_defined_rom_loaded = ROM_BOOL_NO;

    /* Přeinstalovat ROM, pokud je právě vybraná uživatelská */
    if (g_rom.rom_cfg && (g_rom.rom_cfg->flags & ROM_CONFIG_FLAG_USER_DEFINED))
    {
        rom_reinstall(g_rom.rom_cfg);
    };

    rom_request_settings_free(&s);
    return 0;
}


uint32_t rom_request_set_user_defined(const st_ROM_USER_DEFINED_SETTINGS *s)
{
    if (!s) return 0;
    g_mutex_lock(&s_pending_lock);
    rom_request_settings_free(&s_pending);
    s_pending.allinone = s->allinone;
    s_pending.allinone_fp = g_strdup(s->allinone_fp);
    s_pending.mz700_fp = g_strdup(s->mz700_fp);
    s_pending.cgrom_fp = g_strdup(s->cgrom_fp);
    s_pending.mz800_fp = g_strdup(s->mz800_fp);
    s_pending.cmthack_type = s->cmthack_type;
    s_pending.cmthack_allinone = s->cmthack_allinone;
    s_pending.cmthack_allinone_fp = g_strdup(s->cmthack_allinone_fp);
    s_pending.cmthack_mz700_fp = g_strdup(s->cmthack_mz700_fp);
    s_pending.cmthack_cgrom_fp = g_strdup(s->cmthack_cgrom_fp);
    s_pending.cmthack_mz800_fp = g_strdup(s->cmthack_mz800_fp);
    s_pending_set = true;
    g_mutex_unlock(&s_pending_lock);
    return device_request_submit("rom_set_user_defined", rom_request_exec_set_user_defined, NULL);
}
