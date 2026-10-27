/**
 * @file   devbus_snap.c
 * @brief  Komponenta snapshotu "devbus" (stav připojených zařízení).
 *
 * Formát (devices/devbus.xml):
 * @code
 * <devbus version="1">
 *   <devices type="string">pcg700 suc80</devices>   (id připojených zařízení)
 *   <pcg700>
 *     <state_version type="uint">2</state_version>
 *     <options>
 *       <strobe type="string">rising</strobe>       (KEYWORD jménem, BOOL a UNSIGNED číslem)
 *     </options>
 *   </pcg700>
 * </devbus>
 * @endcode
 * Soubory zařízení leží pod devices/devbus/<id>/. Čtečka XML snapshotu umí
 * jen vstup do potomka podle jména (ne průchod sourozenci), proto má každé
 * zařízení element pojmenovaný svým id (id začíná písmenem, viz devbus.h)
 * a seznam připojených je v elementu `devices`.
 *
 * Pravidla:
 *  - Save: jen připojená zařízení; bez nich se nezapíše žádný soubor, takže
 *    snapshot i hash stavu jsou shodné se snapshotem bez devbus.
 *  - Load: nejdřív se odpojí zařízení, která snapshot neuvádí (i starší
 *    snapshot bez devbus.xml), pak se připojí uvedená a načte se jejich stav.
 *    Neznámé id nebo vyšší state_version = varování, zařízení odpojené.
 *    Kolize nebo chyba připojení = varování, zařízení odpojené.
 *    Volba uživatele v INI (connected i volby) se připne
 *    (snapshot_config_pin_ini_value()), snapshot je stav stroje.
 *  - Volby ze snapshotu se nastaví bez option_changed(); odvozený stav si
 *    zařízení přepočítá v load() nebo after_load().
 *
 * @par Licence: GPLv3
 */

#include "devbus_internal.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "snapshot/snapshot_config.h"


/** @brief Soubor seznamu zařízení v archivu. */
#define DEVBUS_SNAP_XML "devices/devbus.xml"

/** @brief Verze formátu devbus.xml. */
#define DEVBUS_SNAP_FORMAT 1

/**
 * @brief Kontext snapshotu pro zařízení.
 * @invariant ctx platí jen během save/load komponenty.
 */
struct st_DEVBUS_SNAP {
    st_SNAPSHOT_CONTEXT *ctx;  /**< Kontext snapshot_mgr. */
    const char *id;            /**< Id zařízení (prefix souborů). */
};


/** @brief Složí cestu "devices/devbus/<id>/<name>" (volající uvolní g_free()). */
static char *devbus_snap_path(const st_DEVBUS_SNAP *s, const char *name)
{
    return g_strdup_printf("devices/devbus/%s/%s", s->id, name);
}


en_SNAPSHOT_RESULT devbus_snap_write_xml(st_DEVBUS_SNAP *s, const char *name, const char *xml)
{
    if (!s || !name || !xml) return SNAPSHOT_ERR_IO;
    char *p = devbus_snap_path(s, name);
    en_SNAPSHOT_RESULT r = snapshot_io_write_xml(s->ctx->io, p, xml);
    g_free(p);
    return r;
}


en_SNAPSHOT_RESULT devbus_snap_write_bin(st_DEVBUS_SNAP *s, const char *name, const void *d, size_t n)
{
    if (!s || !name || (!d && n)) return SNAPSHOT_ERR_IO;
    char *p = devbus_snap_path(s, name);
    en_SNAPSHOT_RESULT r = snapshot_io_write_bin(s->ctx->io, p, d, n);
    g_free(p);
    return r;
}


en_SNAPSHOT_RESULT devbus_snap_read_xml(st_DEVBUS_SNAP *s, const char *name, char **out_xml)
{
    if (!s || !name || !out_xml) return SNAPSHOT_ERR_IO;
    char *p = devbus_snap_path(s, name);
    en_SNAPSHOT_RESULT r = snapshot_io_read_xml(s->ctx->io, p, out_xml);
    g_free(p);
    return r;
}


en_SNAPSHOT_RESULT devbus_snap_read_bin_into(st_DEVBUS_SNAP *s, const char *name, void *d, size_t n)
{
    if (!s || !name || (!d && n)) return SNAPSHOT_ERR_IO;
    char *p = devbus_snap_path(s, name);
    en_SNAPSHOT_RESULT r = snapshot_io_read_bin_into(s->ctx->io, p, d, n);
    g_free(p);
    return r;
}


bool devbus_snap_exists(st_DEVBUS_SNAP *s, const char *name)
{
    if (!s || !name) return false;
    char *p = devbus_snap_path(s, name);
    bool r = snapshot_io_entry_exists(s->ctx->io, p);
    g_free(p);
    return r;
}


bool devbus_snap_for_hash(const st_DEVBUS_SNAP *s)
{
    return s && s->ctx && s->ctx->for_hash;
}


/** @brief Save komponenty: seznam připojených, jejich volby a stav. */
static en_SNAPSHOT_RESULT devbus_snap_save(st_SNAPSHOT_CONTEXT *ctx)
{
    if (!g_devbus.connected_count) return SNAPSHOT_OK;

    GString *list = g_string_new(NULL);
    for (unsigned i = 0; i < g_devbus.count; i++) {
        if (!g_devbus.slot[i].connected) continue;
        if (list->len) g_string_append_c(list, ' ');
        g_string_append(list, g_devbus.slot[i].desc->id);
    }

    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);
    char ver[16];
    g_snprintf(ver, sizeof(ver), "%d", DEVBUS_SNAP_FORMAT);
    snapshot_xml_open_element_attr(w, "devbus", "version", ver);
    snapshot_xml_write_string(w, "devices", list->str);
    g_string_free(list, TRUE);
    for (unsigned i = 0; i < g_devbus.count; i++) {
        const st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (!s->connected) continue;
        snapshot_xml_open_element(w, s->desc->id);
        snapshot_xml_write_uint(w, "state_version", s->desc->snap.version);
        snapshot_xml_open_element(w, "options");
        for (unsigned o = 0; o < s->desc->option_count; o++) {
            char buf[40];
            devbus_option_format((int) i, o, s->opt[o], buf, sizeof(buf));
            snapshot_xml_write_string(w, s->desc->options[o].key, buf);
        }
        snapshot_xml_close_element(w); /* options */
        snapshot_xml_close_element(w); /* id */
    }
    snapshot_xml_close_element(w); /* devbus */
    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT r = snapshot_io_write_xml(ctx->io, DEVBUS_SNAP_XML, xml);
    g_free(xml);
    if (r != SNAPSHOT_OK) return r;

    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (!s->connected) continue;
        st_DEVBUS_SNAP ds = { ctx, s->desc->id };
        r = s->desc->snap.save(s->ctx, &ds);
        if (r != SNAPSHOT_OK) {
            SNAP_ERR("devbus", "device '%s': save failed: %s", s->desc->id, snapshot_result_to_string(r));
            return r;
        }
    }
    return SNAPSHOT_OK;
}


/** @brief Obsahuje seznam @p list (id oddělená mezerou) id @p id? */
static bool devbus_snap_list_has(const char *list, const char *id)
{
    if (!list) return false;
    gchar **v = g_strsplit(list, " ", -1);
    bool found = false;
    for (gchar **p = v; *p && !found; p++) found = (strcmp(*p, id) == 0);
    g_strfreev(v);
    return found;
}


/** @brief Načte volby zařízení ze snapshotu (reader stojí v elementu zařízení). */
static void devbus_snap_load_options(snapshot_xml_reader_t *r, int slot)
{
    st_DEVBUS_SLOT *s = &g_devbus.slot[slot];
    bool has = snapshot_xml_enter_element(r, "options");
    for (unsigned o = 0; o < s->desc->option_count; o++) {
        const st_DEVBUS_OPTION *opt = &s->desc->options[o];
        unsigned ini_value = s->opt[o];
        unsigned v = opt->def; /* chybějící volba (starší stav) = výchozí */
        char *text = NULL;
        if (has && snapshot_xml_read_string(r, opt->key, &text) && text) {
            unsigned parsed;
            if (devbus_option_parse(slot, o, text, &parsed)) v = parsed;
            else SNAP_WARN("devbus", "device '%s': invalid value '%s' of option '%s', default used",
                           s->desc->id, text, opt->key);
        }
        g_free(text);
        s->opt[o] = v;
        snapshot_config_pin_ini_value(s->desc->ini_section, opt->key, ini_value);
    }
    if (has) snapshot_xml_leave_element(r);
}


/** @brief Load komponenty (pravidla v hlavičce souboru). */
static en_SNAPSHOT_RESULT devbus_snap_load(st_SNAPSHOT_CONTEXT *ctx)
{
    unsigned ini_connected[DEVBUS_MAX_DEVICES];
    for (unsigned i = 0; i < g_devbus.count; i++) ini_connected[i] = g_devbus.slot[i].connected;

    char *list = NULL;
    snapshot_xml_reader_t *r = NULL;
    if (snapshot_io_entry_exists(ctx->io, DEVBUS_SNAP_XML)) {
        char *xml = NULL;
        if (snapshot_io_read_xml(ctx->io, DEVBUS_SNAP_XML, &xml) == SNAPSHOT_OK) {
            r = snapshot_xml_reader_new(xml);
        }
        g_free(xml);
        if (!r || !snapshot_xml_enter_element(r, "devbus")) {
            SNAP_WARN("devbus", "cannot parse %s, all devices disconnected", DEVBUS_SNAP_XML);
            if (r) snapshot_xml_reader_free(r);
            r = NULL;
        } else if (!snapshot_xml_read_string(r, "devices", &list)) {
            list = NULL;
        }
    }

    /* Neznámá id ve snapshotu (snapshot z jiné sestavy) */
    if (list) {
        gchar **v = g_strsplit(list, " ", -1);
        for (gchar **p = v; *p; p++) {
            if (**p && devbus_find(*p) < 0) {
                SNAP_WARN("devbus", "unknown device '%s' in snapshot, ignored", *p);
            }
        }
        g_strfreev(v);
    }

    /* 1. Odpojit neuvedená */
    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (s->connected && !devbus_snap_list_has(list, s->desc->id)) devbus_disconnect((int) i);
    }

    /* 2. Připojit uvedená a načíst stav */
    for (unsigned i = 0; i < g_devbus.count && list; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (!devbus_snap_list_has(list, s->desc->id)) continue;
        if (!snapshot_xml_enter_element(r, s->desc->id)) {
            SNAP_WARN("devbus", "device '%s' listed without its element, disconnected", s->desc->id);
            devbus_disconnect((int) i);
            continue;
        }
        unsigned version = 0;
        snapshot_xml_read_uint(r, "state_version", &version);
        if (version < 1 || version > s->desc->snap.version) {
            SNAP_WARN("devbus", "device '%s': unsupported state version %u (max %u), disconnected",
                      s->desc->id, version, (unsigned) s->desc->snap.version);
            snapshot_xml_leave_element(r);
            devbus_disconnect((int) i);
            continue;
        }
        devbus_snap_load_options(r, (int) i);
        snapshot_xml_leave_element(r);

        if (!s->connected) {
            int rc = devbus_connect((int) i);
            if (rc != DEVBUS_OK) {
                SNAP_WARN("devbus", "device '%s': cannot connect (error %d), disconnected", s->desc->id, rc);
                continue;
            }
        }
        st_DEVBUS_SNAP ds = { ctx, s->desc->id };
        en_SNAPSHOT_RESULT lr = s->desc->snap.load(s->ctx, &ds, version);
        if (lr != SNAPSHOT_OK) {
            SNAP_WARN("devbus", "device '%s': load failed (%s), disconnected",
                      s->desc->id, snapshot_result_to_string(lr));
            devbus_disconnect((int) i);
        }
    }

    g_free(list);
    if (r) snapshot_xml_reader_free(r);

    /* 3. Volba uživatele v INI zůstane */
    for (unsigned i = 0; i < g_devbus.count; i++) {
        snapshot_config_pin_ini_value(g_devbus.slot[i].desc->ini_section, "connected", ini_connected[i]);
    }
    return SNAPSHOT_OK;
}


void devbus_snap_register(void)
{
    if (!g_devbus.initialized) return;
    snapshot_register_component("devbus", SNAPSHOT_PRIORITY_DEVICE, devbus_snap_save, devbus_snap_load, true);
}
