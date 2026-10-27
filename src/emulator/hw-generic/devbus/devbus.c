/**
 * @file   devbus.c
 * @brief  Sběrnice volitelných zařízení - registr, životní cyklus, tabulky, kolize, volby.
 *
 * Kontrakt v devbus.h. Jádro je bez lokalizačních maker; hlášení jdou na
 * stderr anglicky.
 *
 * @par Licence: GPLv3
 */

#include "devbus_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include <glib.h>


st_DEVBUS g_devbus;
st_DEVBUS_PORT_SLOT g_devbus_ports[256];
st_DEVBUS_HOOKS g_devbus_hooks;

const uint8_t devbus_mz700_colormap[8] = {0, 9, 10, 11, 12, 13, 14, 15};


/* ------------------------------------------------------------------------- */
/*                           Pomocné kontroly                                */
/* ------------------------------------------------------------------------- */

st_DEVBUS_SLOT *devbus_slot_get(int slot)
{
    if (slot < 0 || (unsigned) slot >= g_devbus.count) return NULL;
    return &g_devbus.slot[slot];
}


/**
 * @brief Odpovídá text tvaru [a-z][a-z0-9_]{0,max-1}?
 * @param s   Text (smí být NULL).
 * @param max Nejvyšší délka.
 */
static bool devbus_valid_lower_ident(const char *s, size_t max)
{
    if (!s || !s[0]) return false;
    size_t n = strlen(s);
    if (n > max) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}


/**
 * @brief Odpovídá klíčové slovo volby tvaru [a-z0-9_]{1,31}?
 */
static bool devbus_valid_keyword(const char *s)
{
    if (!s || !s[0] || strlen(s) > 31) return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}


/**
 * @brief Odpovídá text tvaru [A-Z0-9_]{1,DEVBUS_INI_SECTION_MAX}?
 */
static bool devbus_valid_section(const char *s)
{
    if (!s || !s[0]) return false;
    size_t n = strlen(s);
    if (n > DEVBUS_INI_SECTION_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}


unsigned devbus_option_keyword_count(const st_DEVBUS_OPTION *o)
{
    if (!o || o->type != DEVBUS_OPT_KEYWORD || !o->keywords) return 0;
    unsigned n = 0;
    while (o->keywords[n]) n++;
    return n;
}


bool devbus_option_value_valid(const st_DEVBUS_OPTION *o, unsigned value)
{
    switch (o->type) {
    case DEVBUS_OPT_BOOL:
        return value <= 1;
    case DEVBUS_OPT_KEYWORD:
        return value < devbus_option_keyword_count(o);
    case DEVBUS_OPT_UNSIGNED:
        return value >= o->min && value <= o->max;
    }
    return false;
}


/** @brief Leží rozsah v okně platformy pro každý stav z masky @p when? */
static bool devbus_mmio_range_fits(uint16_t first, uint16_t last, unsigned when)
{
    const st_DEVBUS_ARCH_OPS *a = g_devbus.arch;
    if (when & ~DEVBUS_MMIO_WHEN_MASK) return false;
    if (when & DEVBUS_MMIO_MAPPED) {
        if (a->mmio_mapped_first > a->mmio_mapped_last) return false;
        if (first < a->mmio_mapped_first || last > a->mmio_mapped_last) return false;
    }
    if (when & DEVBUS_MMIO_DRAM) {
        if (a->mmio_dram_first > a->mmio_dram_last) return false;
        if (first < a->mmio_dram_first || last > a->mmio_dram_last) return false;
    }
    return true;
}


/** @brief Má platforma aspoň jedno MMIO okno? */
static bool devbus_arch_has_mmio(void)
{
    const st_DEVBUS_ARCH_OPS *a = g_devbus.arch;
    return (a->mmio_mapped_first <= a->mmio_mapped_last) || (a->mmio_dram_first <= a->mmio_dram_last);
}


/**
 * @brief Ověří deskriptor proti pravidlům devbus.h a adaptéru.
 * @return DEVBUS_OK, DEVBUS_ERR_INVALID, DEVBUS_ERR_UNSUPPORTED.
 */
static int devbus_validate_desc(const st_DEVBUS_DEVICE_DESC *d)
{
    if (!d || !d->ops || !d->name) return DEVBUS_ERR_INVALID;
    if (!devbus_valid_lower_ident(d->id, DEVBUS_ID_MAX)) return DEVBUS_ERR_INVALID;
    if (!devbus_valid_section(d->ini_section)) return DEVBUS_ERR_INVALID;
    if (!(d->platforms & g_devbus.arch->platform)) return DEVBUS_ERR_UNSUPPORTED;
    if (d->snap.version < 1 || !d->snap.save || !d->snap.load) return DEVBUS_ERR_INVALID;

    /* Volby */
    if (d->option_count > DEVBUS_MAX_OPTIONS) return DEVBUS_ERR_INVALID;
    if (d->option_count && !d->options) return DEVBUS_ERR_INVALID;
    for (unsigned i = 0; i < d->option_count; i++) {
        const st_DEVBUS_OPTION *o = &d->options[i];
        if (!devbus_valid_lower_ident(o->key, DEVBUS_ID_MAX)) return DEVBUS_ERR_INVALID;
        if (strcmp(o->key, "connected") == 0) return DEVBUS_ERR_INVALID;
        if (!o->label) return DEVBUS_ERR_INVALID;
        if (o->apply != DEVBUS_APPLY_LIVE && o->apply != DEVBUS_APPLY_CONNECT) return DEVBUS_ERR_INVALID;
        if (o->type == DEVBUS_OPT_KEYWORD) {
            unsigned n = devbus_option_keyword_count(o);
            if (n == 0) return DEVBUS_ERR_INVALID;
            for (unsigned k = 0; k < n; k++) {
                if (!devbus_valid_keyword(o->keywords[k])) return DEVBUS_ERR_INVALID;
                if (o->keyword_labels && !o->keyword_labels[k]) return DEVBUS_ERR_INVALID;
            }
        } else if (o->keyword_labels) {
            return DEVBUS_ERR_INVALID;  /* popisky hodnot jen u KEYWORD */
        } else if (o->type == DEVBUS_OPT_UNSIGNED) {
            if (o->min > o->max) return DEVBUS_ERR_INVALID;
        } else if (o->type != DEVBUS_OPT_BOOL) {
            return DEVBUS_ERR_INVALID;
        }
        if (!devbus_option_value_valid(o, o->def)) return DEVBUS_ERR_INVALID;
        for (unsigned j = 0; j < i; j++) {
            if (strcmp(d->options[j].key, o->key) == 0) return DEVBUS_ERR_INVALID;
        }
    }

    /* Porty */
    if (d->port_count > DEVBUS_MAX_PORT_RANGES) return DEVBUS_ERR_INVALID;
    if (d->port_count && !d->ports) return DEVBUS_ERR_INVALID;
    for (unsigned i = 0; i < d->port_count; i++) {
        const st_DEVBUS_PORT_RANGE *r = &d->ports[i];
        if (r->first > r->last) return DEVBUS_ERR_INVALID;
        for (unsigned p = r->first; p <= r->last; p++) {
            if (g_devbus.arch->port_is_static && g_devbus.arch->port_is_static((uint8_t) p))
                return DEVBUS_ERR_INVALID;
        }
        for (unsigned j = 0; j < i; j++) {
            if (!(r->last < d->ports[j].first || r->first > d->ports[j].last)) return DEVBUS_ERR_INVALID;
        }
    }

    /* MMIO */
    if (d->mmio_count > DEVBUS_MAX_MMIO_RANGES) return DEVBUS_ERR_INVALID;
    if (d->mmio_count && !d->mmio) return DEVBUS_ERR_INVALID;
    if (d->mmio_count && !devbus_arch_has_mmio()) return DEVBUS_ERR_UNSUPPORTED;
    for (unsigned i = 0; i < d->mmio_count; i++) {
        const st_DEVBUS_MMIO_RANGE *r = &d->mmio[i];
        if (r->first > r->last || !r->write) return DEVBUS_ERR_INVALID;
        if (!devbus_mmio_range_fits(r->first, r->last, r->when_default)) return DEVBUS_ERR_INVALID;
        for (unsigned j = 0; j < i; j++) {
            if (!(r->last < d->mmio[j].first || r->first > d->mmio[j].last)) return DEVBUS_ERR_INVALID;
        }
    }

    /* Video */
    if (d->video && !d->video->glyph_source && !d->video->render_row) return DEVBUS_ERR_INVALID;

    /* Regiony */
    if (d->region_count > DEVBUS_MAX_REGIONS) return DEVBUS_ERR_INVALID;
    if (d->region_count && !d->regions) return DEVBUS_ERR_INVALID;
    for (unsigned i = 0; i < d->region_count; i++) {
        const st_DEVBUS_REGION *r = &d->regions[i];
        if (!r->name || !r->size || !r->data) return DEVBUS_ERR_INVALID;
    }

    /* Katalog I/O */
    if (d->io_catalog_count && !d->io_catalog) return DEVBUS_ERR_INVALID;

    return DEVBUS_OK;
}


/* ------------------------------------------------------------------------- */
/*                         Inicializace a registr                            */
/* ------------------------------------------------------------------------- */

void devbus_init(const st_DEVBUS_ARCH_OPS *ops)
{
    memset(&g_devbus, 0, sizeof(g_devbus));
    memset(g_devbus_ports, 0, sizeof(g_devbus_ports));
    memset(&g_devbus_hooks, 0, sizeof(g_devbus_hooks));
    if (!ops || !ops->hooks_changed) {
        fprintf(stderr, "devbus: invalid arch ops, devbus stays uninitialized\n");
        return;
    }
    g_devbus.arch = ops;
    g_devbus.initialized = true;
}


bool devbus_is_initialized(void)
{
    return g_devbus.initialized;
}


int devbus_register(const st_DEVBUS_DEVICE_DESC *desc, void *ctx)
{
    if (!g_devbus.initialized) return DEVBUS_ERR_UNSUPPORTED;
    if (g_devbus.started) return DEVBUS_ERR_INVALID;
    int v = devbus_validate_desc(desc);
    if (v != DEVBUS_OK) return v;
    if (devbus_find(desc->id) >= 0) return DEVBUS_ERR_EXISTS;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        if (strcasecmp(g_devbus.slot[i].desc->ini_section, desc->ini_section) == 0) return DEVBUS_ERR_INVALID;
    }
    if (g_devbus.count >= DEVBUS_MAX_DEVICES) return DEVBUS_ERR_FULL;

    int slot = (int) g_devbus.count;
    st_DEVBUS_SLOT *s = &g_devbus.slot[slot];
    memset(s, 0, sizeof(*s));
    s->desc = desc;
    s->ctx = ctx;
    for (unsigned i = 0; i < desc->option_count; i++) s->opt[i] = desc->options[i].def;
    for (unsigned i = 0; i < desc->mmio_count; i++) s->mmio_when[i] = desc->mmio[i].when_default;
    g_devbus.count++;

    int rc = devbus_cfg_register(s);
    if (rc != DEVBUS_OK) {
        g_devbus.count--;
        memset(s, 0, sizeof(*s));
        return rc;
    }

    if (desc->ops->init && desc->ops->init(ctx) != 0) {
        fprintf(stderr, "devbus: init() of device '%s' failed\n", desc->id);
        devbus_vout_release_slot(s);
        g_devbus.count--;
        memset(s, 0, sizeof(*s));
        return DEVBUS_ERR_DEVICE;
    }
    return slot;
}


void devbus_start(void)
{
    if (!g_devbus.initialized || g_devbus.started) return;
    g_devbus.started = true;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        if (!g_devbus.slot[i].ini_connected) continue;
        int rc = devbus_connect((int) i);
        if (rc != DEVBUS_OK) {
            fprintf(stderr, "devbus: cannot connect device '%s' from INI (error %d)\n",
                    g_devbus.slot[i].desc->id, rc);
        }
    }
}


void devbus_exit(void)
{
    if (!g_devbus.initialized) return;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        devbus_disconnect((int) i);
    }
    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (s->desc->ops->destroy) s->desc->ops->destroy(s->ctx);
        devbus_vout_release_slot(s);
    }
    memset(&g_devbus, 0, sizeof(g_devbus));
    memset(g_devbus_ports, 0, sizeof(g_devbus_ports));
    memset(&g_devbus_hooks, 0, sizeof(g_devbus_hooks));
}


int devbus_find(const char *id)
{
    if (!id) return DEVBUS_ERR_NOT_FOUND;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        if (strcmp(g_devbus.slot[i].desc->id, id) == 0) return (int) i;
    }
    return DEVBUS_ERR_NOT_FOUND;
}


unsigned devbus_count(void)
{
    return g_devbus.count;
}


const st_DEVBUS_DEVICE_DESC *devbus_desc(int slot)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    return s ? s->desc : NULL;
}


void *devbus_ctx(int slot)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    return s ? s->ctx : NULL;
}


bool devbus_is_connected(int slot)
{
    if (slot < 0 || slot >= DEVBUS_MAX_DEVICES) return false;
    return __atomic_load_n(&g_devbus.slot[slot].connected, __ATOMIC_ACQUIRE) != 0;
}


unsigned devbus_connected_count(void)
{
    return __atomic_load_n(&g_devbus.connected_count, __ATOMIC_ACQUIRE);
}


/* ------------------------------------------------------------------------- */
/*                         Tabulky a souhrn háčků                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Přepočítá seznam aktivních MMIO rozsahů a souhrn háčků.
 * @return true, když se změnila potřeba obálek nebo video háčky
 *         (adaptér musí přepočítat callbacky).
 */
static bool devbus_recompute(void)
{
    st_DEVBUS_HOOKS old = g_devbus_hooks;
    st_DEVBUS_HOOKS h;
    memset(&h, 0, sizeof(h));

    g_devbus.mmio_active_count = 0;
    g_devbus.frame_done_count = 0;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (!s->connected) continue;
        const st_DEVBUS_DEVICE_DESC *d = s->desc;
        if (d->port_count) h.ports = true;
        for (unsigned m = 0; m < d->mmio_count; m++) {
            if (!s->mmio_when[m]) continue;
            st_DEVBUS_MMIO_ACTIVE *a = &g_devbus.mmio_active[g_devbus.mmio_active_count++];
            a->first = d->mmio[m].first;
            a->last = d->mmio[m].last;
            a->when = s->mmio_when[m];
            a->write = d->mmio[m].write;
            a->ctx = s->ctx;
            h.mmio = true;
        }
        if (d->video) {
            if (d->video->glyph_source) {
                h.glyph = d->video->glyph_source;
                h.glyph_ctx = s->ctx;
            }
            if (d->video->render_row) {
                h.row = d->video->render_row;
                h.row_ctx = s->ctx;
            }
        }
        if (d->ops->frame_done) g_devbus.frame_done_count++;
    }
    g_devbus_hooks = h;
    return memcmp(&old, &h, sizeof(h)) != 0;
}


/**
 * @brief Koliduje zařízení @p s s některým jiným připojeným zařízením?
 */
static bool devbus_conflicts(const st_DEVBUS_SLOT *s)
{
    const st_DEVBUS_DEVICE_DESC *d = s->desc;
    for (unsigned i = 0; i < d->port_count; i++) {
        for (unsigned p = d->ports[i].first; p <= d->ports[i].last; p++) {
            if (g_devbus_ports[p].range) return true;
        }
    }
    for (unsigned i = 0; i < g_devbus.count; i++) {
        const st_DEVBUS_SLOT *o = &g_devbus.slot[i];
        if (o == s || !o->connected) continue;
        const st_DEVBUS_DEVICE_DESC *od = o->desc;
        for (unsigned a = 0; a < d->mmio_count; a++) {
            for (unsigned b = 0; b < od->mmio_count; b++) {
                if (!(d->mmio[a].last < od->mmio[b].first || d->mmio[a].first > od->mmio[b].last)) return true;
            }
        }
        if (d->video && od->video) {
            if (d->video->glyph_source && od->video->glyph_source) return true;
            if (d->video->render_row && od->video->render_row) return true;
        }
    }
    return false;
}


int devbus_connect(int slot)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s) return DEVBUS_ERR_NOT_FOUND;
    if (s->connected) return DEVBUS_OK;
    if (devbus_conflicts(s)) return DEVBUS_ERR_CONFLICT;
    if (s->desc->ops->connect && s->desc->ops->connect(s->ctx) != 0) return DEVBUS_ERR_DEVICE;

    const st_DEVBUS_DEVICE_DESC *d = s->desc;
    for (unsigned i = 0; i < d->port_count; i++) {
        for (unsigned p = d->ports[i].first; p <= d->ports[i].last; p++) {
            g_devbus_ports[p].ctx = s->ctx;
            g_devbus_ports[p].range = &d->ports[i];
        }
    }
    __atomic_store_n(&s->connected, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_devbus.connected_count, g_devbus.connected_count + 1, __ATOMIC_RELEASE);
    devbus_recompute();
    g_devbus.arch->hooks_changed();
    return DEVBUS_OK;
}


int devbus_disconnect(int slot)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s) return DEVBUS_ERR_NOT_FOUND;
    if (!s->connected) return DEVBUS_OK;

    const st_DEVBUS_DEVICE_DESC *d = s->desc;
    for (unsigned i = 0; i < d->port_count; i++) {
        for (unsigned p = d->ports[i].first; p <= d->ports[i].last; p++) {
            g_devbus_ports[p].range = NULL;
            g_devbus_ports[p].ctx = NULL;
        }
    }
    __atomic_store_n(&s->connected, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_devbus.connected_count, g_devbus.connected_count - 1, __ATOMIC_RELEASE);
    devbus_recompute();
    g_devbus.arch->hooks_changed();
    if (d->ops->disconnect) d->ops->disconnect(s->ctx);
    return DEVBUS_OK;
}


/* ------------------------------------------------------------------------- */
/*                         Události životního cyklu                          */
/* ------------------------------------------------------------------------- */

void devbus_reset_all(void)
{
    if (!g_devbus.connected_count) return;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (s->connected && s->desc->ops->reset) s->desc->ops->reset(s->ctx);
    }
}


void devbus_frame_done(void)
{
    if (!g_devbus.frame_done_count) return;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (s->connected && s->desc->ops->frame_done) s->desc->ops->frame_done(s->ctx);
    }
}


void devbus_rom_changed(void)
{
    if (!g_devbus.initialized || !g_devbus.connected_count) return;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (s->connected && s->desc->ops->rom_changed) s->desc->ops->rom_changed(s->ctx);
    }
}


void devbus_after_load(void)
{
    if (!g_devbus.initialized || !g_devbus.connected_count) return;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (s->connected && s->desc->ops->after_load) s->desc->ops->after_load(s->ctx);
    }
}


/* ------------------------------------------------------------------------- */
/*                                  Volby                                    */
/* ------------------------------------------------------------------------- */

unsigned devbus_option_get(int slot, unsigned opt)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || opt >= s->desc->option_count) return 0;
    return s->opt[opt];
}


int devbus_option_set(int slot, unsigned opt, unsigned value)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || opt >= s->desc->option_count) return DEVBUS_ERR_NOT_FOUND;
    const st_DEVBUS_OPTION *o = &s->desc->options[opt];
    if (!devbus_option_value_valid(o, value)) return DEVBUS_ERR_INVALID;
    if (s->opt[opt] == value) return DEVBUS_OK;
    s->opt[opt] = value;
    if (o->apply == DEVBUS_APPLY_LIVE && s->connected && s->desc->ops->option_changed) {
        s->desc->ops->option_changed(s->ctx, opt, value);
    }
    return DEVBUS_OK;
}


int devbus_option_find(int slot, const char *key)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || !key) return DEVBUS_ERR_NOT_FOUND;
    for (unsigned i = 0; i < s->desc->option_count; i++) {
        if (strcmp(s->desc->options[i].key, key) == 0) return (int) i;
    }
    return DEVBUS_ERR_NOT_FOUND;
}


bool devbus_option_parse(int slot, unsigned opt, const char *text, unsigned *out)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || opt >= s->desc->option_count || !text || !out) return false;
    const st_DEVBUS_OPTION *o = &s->desc->options[opt];
    unsigned v;
    if (o->type == DEVBUS_OPT_KEYWORD) {
        unsigned n = devbus_option_keyword_count(o);
        for (unsigned k = 0; k < n; k++) {
            if (strcasecmp(o->keywords[k], text) == 0) {
                *out = k;
                return true;
            }
        }
        return false;
    }
    if (o->type == DEVBUS_OPT_BOOL) {
        if (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0) v = 1;
        else if (strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0) v = 0;
        else return false;
    } else {
        char *end = NULL;
        unsigned long ul = strtoul(text, &end, 0);
        if (!text[0] || !end || *end != '\0' || ul > 0xFFFFFFFFul) return false;
        v = (unsigned) ul;
    }
    if (!devbus_option_value_valid(o, v)) return false;
    *out = v;
    return true;
}


const char *devbus_option_format(int slot, unsigned opt, unsigned value, char *buf, size_t size)
{
    if (!buf || !size) return "";
    buf[0] = '\0';
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || opt >= s->desc->option_count) return buf;
    const st_DEVBUS_OPTION *o = &s->desc->options[opt];
    if (o->type == DEVBUS_OPT_KEYWORD) {
        if (value < devbus_option_keyword_count(o)) g_strlcpy(buf, o->keywords[value], size);
    } else {
        snprintf(buf, size, "%u", value);
    }
    return buf;
}


int devbus_option_key(const char *module, const char *element, unsigned *opt_out)
{
    if (!module || !element) return DEVBUS_ERR_NOT_FOUND;
    for (unsigned i = 0; i < g_devbus.count; i++) {
        const st_DEVBUS_DEVICE_DESC *d = g_devbus.slot[i].desc;
        if (strcasecmp(d->ini_section, module) != 0) continue;
        if (strcasecmp(element, "connected") == 0) return DEVBUS_ERR_INVALID;
        for (unsigned o = 0; o < d->option_count; o++) {
            if (strcasecmp(d->options[o].key, element) == 0) {
                if (opt_out) *opt_out = o;
                return (int) i;
            }
        }
        return DEVBUS_ERR_NOT_FOUND;
    }
    return DEVBUS_ERR_NOT_FOUND;
}


/* ------------------------------------------------------------------------- */
/*                                  MMIO                                     */
/* ------------------------------------------------------------------------- */

int devbus_mmio_set_when(int slot, unsigned range_idx, unsigned when)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || range_idx >= s->desc->mmio_count) return DEVBUS_ERR_NOT_FOUND;
    const st_DEVBUS_MMIO_RANGE *r = &s->desc->mmio[range_idx];
    if (!devbus_mmio_range_fits(r->first, r->last, when)) return DEVBUS_ERR_INVALID;
    if (s->mmio_when[range_idx] == when) return DEVBUS_OK;
    s->mmio_when[range_idx] = when;
    if (s->connected && devbus_recompute()) g_devbus.arch->hooks_changed();
    return DEVBUS_OK;
}


unsigned devbus_mmio_get_when(int slot, unsigned range_idx)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || range_idx >= s->desc->mmio_count) return 0;
    return s->mmio_when[range_idx];
}


void devbus_mmio_dispatch(uint16_t addr, uint8_t value, unsigned flags)
{
    bool synced = false;
    unsigned state = flags & DEVBUS_MMIO_WHEN_MASK;
    for (unsigned i = 0; i < g_devbus.mmio_active_count; i++) {
        const st_DEVBUS_MMIO_ACTIVE *a = &g_devbus.mmio_active[i];
        if (addr < a->first || addr > a->last || !(a->when & state)) continue;
        if ((flags & DEVBUS_ACC_CPU) && !synced) {
            if (g_devbus.arch->mmio_sync) g_devbus.arch->mmio_sync();
            synced = true;
        }
        a->write(a->ctx, addr, value, flags);
    }
}


bool devbus_mmio_claims(uint16_t addr)
{
    for (unsigned i = 0; i < g_devbus.count; i++) {
        const st_DEVBUS_SLOT *s = &g_devbus.slot[i];
        if (!s->connected) continue;
        for (unsigned m = 0; m < s->desc->mmio_count; m++) {
            if (addr >= s->desc->mmio[m].first && addr <= s->desc->mmio[m].last) return true;
        }
    }
    return false;
}


/* ------------------------------------------------------------------------- */
/*                                  Porty                                    */
/* ------------------------------------------------------------------------- */

bool devbus_port_peek(uint16_t port, uint8_t *out)
{
    const st_DEVBUS_PORT_SLOT *ps = &g_devbus_ports[port & 0xff];
    if (!ps->range || !ps->range->peek || !out) return false;
    *out = ps->range->peek(ps->ctx, port);
    return true;
}


/* ------------------------------------------------------------------------- */
/*                             WAIT, čas, obraz                              */
/* ------------------------------------------------------------------------- */

void devbus_cpu_wait(unsigned tstates)
{
    if (g_devbus.arch && g_devbus.arch->cpu_wait && tstates) g_devbus.arch->cpu_wait(tstates);
}


uint64_t devbus_time_ticks(void)
{
    if (g_devbus.arch && g_devbus.arch->time_ticks) return g_devbus.arch->time_ticks();
    return 0;
}


void devbus_video_invalidate(void)
{
    if (g_devbus.arch && g_devbus.arch->video_invalidate) g_devbus.arch->video_invalidate();
}


/* ------------------------------------------------------------------------- */
/*                         Regiony a katalog I/O                             */
/* ------------------------------------------------------------------------- */

const st_DEVBUS_REGION *devbus_region(int slot, unsigned idx)
{
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || idx >= s->desc->region_count) return NULL;
    return &s->desc->regions[idx];
}


size_t devbus_io_catalog_total(void)
{
    size_t n = 0;
    for (unsigned i = 0; i < g_devbus.count; i++) n += g_devbus.slot[i].desc->io_catalog_count;
    return n;
}


const st_IO_PORT_DESC *devbus_io_catalog_at(size_t i, int *slot_out)
{
    for (unsigned s = 0; s < g_devbus.count; s++) {
        const st_DEVBUS_DEVICE_DESC *d = g_devbus.slot[s].desc;
        if (i < d->io_catalog_count) {
            if (slot_out) *slot_out = (int) s;
            return &d->io_catalog[i];
        }
        i -= d->io_catalog_count;
    }
    return NULL;
}
