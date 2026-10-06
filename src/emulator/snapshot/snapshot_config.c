/**
 * @file snapshot_config.c
 * @brief Konfigurace snapshotů — registrace do cfgmain INI systému
 */

#include "snapshot.h"
#include "snapshot_config.h"
#include "snapshot_xml.h"

#include <stdio.h>
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

#include <glib.h>
#include <string.h>


/* ========================================================================= */
/*                       Propagate callbacky                                 */
/* ========================================================================= */

static void propagatecfg_include_ramdisk(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_snapshot_settings.include_ramdisk = cfgelement_get_bool_value(elm) ? true : false;
}

static void propagatecfg_include_memext(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_snapshot_settings.include_memext = cfgelement_get_bool_value(elm) ? true : false;
}

static void propagatecfg_compression_level(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_snapshot_settings.compression_level = (int)cfgelement_get_unsigned_value(elm);
}

static void propagatecfg_default_directory(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    char *dir = cfgelement_get_text_value(elm);
    if (dir) {
        snprintf(g_snapshot_settings.default_directory,
                 sizeof(g_snapshot_settings.default_directory),
                 "%s", dir);
    }
}

static void propagatecfg_quicksave_filename(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    char *name = cfgelement_get_text_value(elm);
    if (name) {
        snprintf(g_snapshot_settings.quicksave_filename,
                 sizeof(g_snapshot_settings.quicksave_filename),
                 "%s", name);
    }
}

static void propagatecfg_quicksave_mode(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_snapshot_settings.quicksave_mode = (int)cfgelement_get_unsigned_value(elm);
}

static void propagatecfg_quicksave_max_slots(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_snapshot_settings.quicksave_max_slots = (int)cfgelement_get_unsigned_value(elm);
}

static void propagatecfg_load_resume_mode(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_snapshot_settings.load_resume_mode = (int)cfgelement_get_unsigned_value(elm);
}

static void propagatecfg_quickload_resume_mode(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_snapshot_settings.quickload_resume_mode = (int)cfgelement_get_unsigned_value(elm);
}


/* ========================================================================= */
/*                         Save callbacky                                    */
/* ========================================================================= */

static void savecfg_include_ramdisk(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_bool_value(elm, g_snapshot_settings.include_ramdisk ? 1 : 0);
}

static void savecfg_include_memext(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_bool_value(elm, g_snapshot_settings.include_memext ? 1 : 0);
}

static void savecfg_compression_level(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_unsigned_value(elm, (unsigned)g_snapshot_settings.compression_level);
}

static void savecfg_default_directory(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_text_value(elm, g_snapshot_settings.default_directory);
}

static void savecfg_quicksave_filename(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_text_value(elm, g_snapshot_settings.quicksave_filename);
}

static void savecfg_quicksave_mode(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_unsigned_value(elm, (unsigned)g_snapshot_settings.quicksave_mode);
}

static void savecfg_quicksave_max_slots(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_unsigned_value(elm, (unsigned)g_snapshot_settings.quicksave_max_slots);
}

static void savecfg_load_resume_mode(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_unsigned_value(elm, (unsigned)g_snapshot_settings.load_resume_mode);
}

static void savecfg_quickload_resume_mode(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_unsigned_value(elm, (unsigned)g_snapshot_settings.quickload_resume_mode);
}


/* ========================================================================= */
/*                              Init                                         */
/* ========================================================================= */

void snapshot_config_init(void)
{
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "SNAPSHOT");

    CFGELM *elm;

    elm = cfgmodule_register_new_element(cmod, "include_ramdisk", CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, propagatecfg_include_ramdisk, NULL);
    cfgelement_set_save_cb(elm, savecfg_include_ramdisk, NULL);

    elm = cfgmodule_register_new_element(cmod, "include_memext", CFGENTYPE_BOOL, 1);
    cfgelement_set_propagate_cb(elm, propagatecfg_include_memext, NULL);
    cfgelement_set_save_cb(elm, savecfg_include_memext, NULL);

    elm = cfgmodule_register_new_element(cmod, "compression_level", CFGENTYPE_UNSIGNED, 6, 0, 9);
    cfgelement_set_propagate_cb(elm, propagatecfg_compression_level, NULL);
    cfgelement_set_save_cb(elm, savecfg_compression_level, NULL);

    elm = cfgmodule_register_new_element(cmod, "default_directory", CFGENTYPE_TEXT, "");
    cfgelement_set_propagate_cb(elm, propagatecfg_default_directory, NULL);
    cfgelement_set_save_cb(elm, savecfg_default_directory, NULL);

    elm = cfgmodule_register_new_element(cmod, "quicksave_filename", CFGENTYPE_TEXT, "quicksave");
    cfgelement_set_propagate_cb(elm, propagatecfg_quicksave_filename, NULL);
    cfgelement_set_save_cb(elm, savecfg_quicksave_filename, NULL);

    elm = cfgmodule_register_new_element(cmod, "quicksave_mode", CFGENTYPE_UNSIGNED, 0, 0, 2);
    cfgelement_set_propagate_cb(elm, propagatecfg_quicksave_mode, NULL);
    cfgelement_set_save_cb(elm, savecfg_quicksave_mode, NULL);

    elm = cfgmodule_register_new_element(cmod, "quicksave_max_slots", CFGENTYPE_UNSIGNED, 5, 2, 20);
    cfgelement_set_propagate_cb(elm, propagatecfg_quicksave_max_slots, NULL);
    cfgelement_set_save_cb(elm, savecfg_quicksave_max_slots, NULL);

    elm = cfgmodule_register_new_element(cmod, "load_resume_mode", CFGENTYPE_UNSIGNED, SNAPSHOT_RESUME_ALWAYS_PAUSE, 0, 2);
    cfgelement_set_propagate_cb(elm, propagatecfg_load_resume_mode, NULL);
    cfgelement_set_save_cb(elm, savecfg_load_resume_mode, NULL);

    elm = cfgmodule_register_new_element(cmod, "quickload_resume_mode", CFGENTYPE_UNSIGNED, SNAPSHOT_RESUME_ALWAYS_PAUSE, 0, 2);
    cfgelement_set_propagate_cb(elm, propagatecfg_quickload_resume_mode, NULL);
    cfgelement_set_save_cb(elm, savecfg_quickload_resume_mode, NULL);

    /* Načíst sekci z INI a propagovat (vzor mcp_config_init, audio.c):
     * globální cfgroot_propagate se v emulátoru nevolá, takže bez tohoto by
     * se uložené hodnoty nikdy nenačetly a po každém startu platily výchozí.
     * Chybí-li sekce, propagují se výchozí hodnoty elementů. */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}


void snapshot_config_load(void)
{
    /* Konfigurace se načítá v snapshot_config_init() (cfgmodule_parse +
     * cfgmodule_propagate); zde není co dělat. */
}


void snapshot_config_save(void)
{
    /* Konfigurace se ukládá přes cfgroot_save (save callbacky elementů). */
}


void snapshot_config_pin_ini_value(const char *module_name, const char *element_name, unsigned ini_value)
{
    /* Bez konfigurace (unit testy snapshotu bez cfgmain) není co chránit. */
    if (!g_cfgmain) {
        return;
    }
    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)module_name);
    CFGELM *elm = cmod ? cfgmodule_get_element_by_name(cmod, (char *)element_name) : NULL;
    if (!elm) {
        SNAP_WARN("config", "INI key [%s] %s not found, value from snapshot may be saved to INI",
                  module_name, element_name);
        return;
    }
    cfgelement_pin_save_value(elm, ini_value);
}
