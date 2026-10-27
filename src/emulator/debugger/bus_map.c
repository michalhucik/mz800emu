/**
 * @file   bus_map.c
 * @brief  Bus Map - skládání snímku vlastníků adres (kontrakt v bus_map.h).
 *
 * Statická tabulka platformy: MZ-700 (mz700_bus_owners.c, přítomnost přes
 * mz700_bus_owner_present()), MZ-800 (mz800_bus_owners.c,
 * mz800_bus_owner_present()), MZ-1500 (mz1500_bus_owners.c,
 * mz1500_bus_owner_present()). Zařízení devbus z registru (deskriptory)
 * a tabulky portů (jen MZ-700); aktivita z io_activity.
 *
 * @par Licence: GPLv3
 */

#include "bus_map.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>

#include "io_activity.h"
#include "hw-generic/gdg/gdg.h"

#include "hw-generic/memory/memory.h"
#if MZARCH == 700
#include "mzarch/mz700/mz700_bus_owners.h"
#include "mzarch/mz700/devbus/mz700_devbus.h"
#elif MZARCH == 800
#include "mzarch/mz800/mz800_bus_owners.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/mz1500_bus_owners.h"
#endif


/** @brief Statická tabulka aktuální platformy (vyplní bus_map_static_table()). */
static st_BUS_STATIC_TABLE s_table;
/** @brief s_table je vyplněná. */
static bool s_table_ready = false;


const st_BUS_STATIC_TABLE *bus_map_static_table(void)
{
    if (!s_table_ready) {
        memset(&s_table, 0, sizeof(s_table));
#if MZARCH == 700
        s_table.rows = mz700_bus_owners(&s_table.count);
        s_table.present = mz700_bus_owner_present;
#elif MZARCH == 800
        s_table.rows = mz800_bus_owners(&s_table.count);
        s_table.present = mz800_bus_owner_present;
#elif MZARCH == 1500
        s_table.rows = mz1500_bus_owners(&s_table.count);
        s_table.present = mz1500_bus_owner_present;
#endif
        s_table_ready = true;
    }
    return &s_table;
}


bool bus_map_supported(void)
{
    return bus_map_static_table()->count > 0;
}


bool bus_map_static_port_driven(uint8_t port_lsb)
{
    const st_BUS_STATIC_TABLE *t = bus_map_static_table();
    for (unsigned i = 0; i < t->count; i++) {
        const st_BUS_STATIC_OWNER *r = &t->rows[i];
        if (r->mmio || !(r->dirs & BUS_DIR_R) || r->drive == DEVBUS_DRIVE_NONE) continue;
        if (port_lsb < r->first || port_lsb > r->last) continue;
        if (r->present_kind != BUS_PRESENT_ALWAYS && t->present && !t->present(r->present_kind)) continue;
        return true;
    }
    return false;
}


void bus_map_abbrev_from_id(const char *id, char *out)
{
    out[0] = out[1] = out[2] = '\0';
    if (!id || !id[0]) {
        out[0] = '?';
        return;
    }
    char c0 = id[0];
    out[0] = (char) ((c0 >= 'a' && c0 <= 'z') ? c0 - 'a' + 'A' : c0);
    const char *digit = NULL;
    for (const char *p = id + 1; *p; p++) {
        if (*p >= '0' && *p <= '9') { digit = p; break; }
    }
    if (digit) out[1] = *digit;
    else if (id[1]) out[1] = id[1];
}


/** @brief Je vlastník budič (pro potenciální kolizi počítá i UNKNOWN jako budič všech bitů)? */
static uint8_t bus_map_owner_drive_mask(const st_BUS_OWNER *o)
{
    if (o->drive == DEVBUS_DRIVE_UNKNOWN) return 0xFFu;
    return o->drive_mask;
}


/**
 * @brief Přidá vlastníka do pole (nejvýš BUS_MAP_MAX_OWNERS; další se zahodí).
 * @return true = přidán.
 */
static bool bus_map_add_owner(st_BUS_OWNER *owners, uint8_t *count, const st_BUS_OWNER *o)
{
    if (*count >= BUS_MAP_MAX_OWNERS) return false;
    owners[*count] = *o;
    (*count)++;
    return true;
}


/**
 * @brief Příznaky odvozené jen z vlastníků (UNMAPPED, DIR_SPLIT, POTENTIAL_CONFLICT).
 */
static uint8_t bus_map_owner_flags(const st_BUS_OWNER *owners, unsigned count)
{
    uint8_t flags = 0;
    bool any_present = false;
    const char *r_id = NULL, *w_id = NULL;
    bool r_multi = false, w_multi = false;
    for (unsigned i = 0; i < count; i++) {
        const st_BUS_OWNER *o = &owners[i];
        if (!o->connected) continue;
        any_present = true;
        if (o->dirs & BUS_DIR_R) {
            if (!r_id) r_id = o->id;
            else if (strcmp(r_id, o->id) != 0) r_multi = true;
        }
        if (o->dirs & BUS_DIR_W) {
            if (!w_id) w_id = o->id;
            else if (strcmp(w_id, o->id) != 0) w_multi = true;
        }
    }
    if (!any_present) flags |= BUS_PORT_UNMAPPED;
    if (r_id && w_id && (strcmp(r_id, w_id) != 0 || r_multi != w_multi)) flags |= BUS_PORT_DIR_SPLIT;
    /* Potenciální kolize: dva budiče (i odpojené) s překryvem masek. */
    for (unsigned i = 0; i < count && !(flags & BUS_PORT_POTENTIAL_CONFLICT); i++) {
        const st_BUS_OWNER *a = &owners[i];
        if (!(a->dirs & BUS_DIR_R) || a->drive == DEVBUS_DRIVE_NONE) continue;
        uint8_t ma = bus_map_owner_drive_mask(a);
        for (unsigned j = i + 1; j < count; j++) {
            const st_BUS_OWNER *b = &owners[j];
            if (!(b->dirs & BUS_DIR_R) || b->drive == DEVBUS_DRIVE_NONE) continue;
            if (strcmp(a->id, b->id) == 0) continue; /* dva rozsahy téhož čipu */
            uint8_t mb = bus_map_owner_drive_mask(b);
            if (!(ma & mb)) continue;
            if (a->drive == DEVBUS_DRIVE_OPEN_COLLECTOR && b->drive == DEVBUS_DRIVE_OPEN_COLLECTOR) continue;
            flags |= BUS_PORT_POTENTIAL_CONFLICT;
            break;
        }
    }
    return flags;
}


/** @brief Statický řádek -> vlastník snímku. */
static void bus_map_owner_from_row(const st_BUS_STATIC_TABLE *t, const st_BUS_STATIC_OWNER *r, st_BUS_OWNER *o)
{
    memset(o, 0, sizeof(*o));
    o->id = r->id;
    o->name = r->name;
    strncpy(o->abbrev, r->abbrev ? r->abbrev : "?", 2);
    o->kind = BUS_OWNER_STATIC;
    o->devbus_slot = -1;
    o->dirs = r->dirs;
    o->connected = (r->present_kind == BUS_PRESENT_ALWAYS || !t->present || t->present(r->present_kind)) ? 1 : 0;
    o->drive = r->drive;
    o->drive_mask = devbus_drive_mask(r->drive, r->drive_mask);
    o->range_first = r->first;
    o->range_last = r->last;
    o->when_mask = r->mmio ? DEVBUS_MMIO_MAPPED : 0;
    o->source_note = r->source_note;
    o->mirror_note = r->mirror_note;
}


/** @brief Rozsah portů devbus -> vlastník snímku. */
static void bus_map_owner_from_port_range(int slot, const st_DEVBUS_DEVICE_DESC *d, const st_DEVBUS_PORT_RANGE *r,
                                          bool connected, st_BUS_OWNER *o)
{
    memset(o, 0, sizeof(*o));
    o->id = d->id;
    o->name = d->name;
    bus_map_abbrev_from_id(d->id, o->abbrev);
    o->kind = BUS_OWNER_DEVBUS;
    o->devbus_slot = (int8_t) slot;
    o->dirs = (uint8_t) ((r->read ? BUS_DIR_R : 0) | (r->write ? BUS_DIR_W : 0));
    o->connected = connected ? 1 : 0;
    o->drive = r->drive;
    o->drive_mask = devbus_drive_mask(r->drive, r->drive_mask);
    o->range_first = r->first;
    o->range_last = r->last;
}


/** @brief MMIO rozsah devbus -> vlastník snímku. */
static void bus_map_owner_from_mmio_range(int slot, unsigned idx, const st_DEVBUS_DEVICE_DESC *d,
                                          bool connected, st_BUS_OWNER *o)
{
    const st_DEVBUS_MMIO_RANGE *r = &d->mmio[idx];
    memset(o, 0, sizeof(*o));
    o->id = d->id;
    o->name = d->name;
    bus_map_abbrev_from_id(d->id, o->abbrev);
    o->kind = BUS_OWNER_DEVBUS;
    o->devbus_slot = (int8_t) slot;
    o->dirs = (uint8_t) ((r->read ? BUS_DIR_R : 0) | (r->write ? BUS_DIR_W : 0));
    o->connected = connected ? 1 : 0;
    o->drive = r->drive;
    o->drive_mask = devbus_drive_mask(r->drive, r->drive_mask);
    o->range_first = r->first;
    o->range_last = r->last;
    o->when_mask = (uint8_t) devbus_mmio_get_when(slot, idx);
}


/** @brief Pořadí "síly" budiče pro souhrn zařízení. */
static int bus_map_drive_rank(en_DEVBUS_DRIVE d)
{
    switch (d) {
    case DEVBUS_DRIVE_NONE: return 0;
    case DEVBUS_DRIVE_OPEN_COLLECTOR: return 1;
    case DEVBUS_DRIVE_PARTIAL: return 2;
    case DEVBUS_DRIVE_TRISTATE: return 3;
    case DEVBUS_DRIVE_UNKNOWN:
    default: return 4;
    }
}


/** @brief Najde nebo založí řádek zařízení podle id. */
static st_BUS_DEVICE *bus_map_device(st_BUS_MAP_SNAPSHOT *out, const char *id)
{
    for (unsigned i = 0; i < out->device_count; i++) {
        if (strcmp(out->devices[i].id, id) == 0) return &out->devices[i];
    }
    if (out->device_count >= BUS_MAP_MAX_DEVICES) return NULL;
    st_BUS_DEVICE *dv = &out->devices[out->device_count++];
    memset(dv, 0, sizeof(*dv));
    dv->id = id;
    dv->devbus_slot = -1;
    return dv;
}


/** @brief Doplní souhrn zařízení o vlastníka. */
static void bus_map_device_add(st_BUS_MAP_SNAPSHOT *out, const st_BUS_OWNER *o, bool mmio)
{
    st_BUS_DEVICE *dv = bus_map_device(out, o->id);
    if (!dv) return;
    if (!dv->name) {
        dv->name = o->name;
        dv->kind = o->kind;
        dv->devbus_slot = o->devbus_slot;
        dv->connected = o->connected;
    }
    if (mmio) { if (dv->mmio_count != 255) dv->mmio_count++; }
    else      { if (dv->port_count != 255) dv->port_count++; }
    if (bus_map_drive_rank(o->drive) > bus_map_drive_rank(dv->drive)) dv->drive = o->drive;
}


void bus_map_snapshot(st_BUS_MAP_SNAPSHOT *out, unsigned want)
{
    memset(out, 0, sizeof(*out));
    const st_BUS_STATIC_TABLE *t = bus_map_static_table();
    out->platform = MZARCH;
    out->supported = (t->count > 0);
    out->devbus_initialized = devbus_is_initialized();
    if (out->devbus_initialized) {
        out->conflict_policy = devbus_conflict_policy();
        out->bus_idle = devbus_bus_idle();
        out->bus_idle_value = devbus_bus_idle_value();
        out->idle_byte = devbus_idle_byte();
        out->devbus_registered = devbus_count();
        out->devbus_connected = devbus_connected_count();
    }
    out->tracking_active = (g_io_window_tracking_active != 0);
    out->frame = (uint32_t) g_gdg.total_elapsed.screens;
    /* Okno MMIO je dostupné: MZ-700 a MZ-800 s horní ROM mapovanou (na
     * MZ-800 navíc dekódují čipy jen v režimu MZ-700, to říká present_kind
     * řádků), MZ-1500 s horní ROM a SPEC = 0. */
#if MZARCH == 700
    out->mmio_mapped_now = MEMORY_MZ700_MAP_TEST_ROM_E000 ? true : false;
#elif MZARCH == 800
    out->mmio_mapped_now = MEMORY_MZ800_MAP_TEST_ROM_E000 ? true : false;
#elif MZARCH == 1500
    out->mmio_mapped_now = MEMORY_MZ1500_MAP_TEST_E000_PORTS ? true : false;
#endif
    for (unsigned i = 0; i < 256; i++) out->mmio[i].addr = (uint16_t) (BUS_MAP_MMIO_FIRST + i);

    bool want_ports = (want & BUS_WANT_PORTS) != 0;
    bool want_mmio = (want & BUS_WANT_MMIO) != 0;
    bool want_dev = (want & BUS_WANT_DEVICES) != 0;

    /* Statická tabulka. */
    for (unsigned i = 0; i < t->count; i++) {
        const st_BUS_STATIC_OWNER *r = &t->rows[i];
        st_BUS_OWNER o;
        bus_map_owner_from_row(t, r, &o);
        if (r->mmio) {
            if (want_mmio) {
                for (unsigned a = r->first; a <= r->last; a++) {
                    if (a < BUS_MAP_MMIO_FIRST || a > BUS_MAP_MMIO_FIRST + 255u) continue;
                    st_BUS_MMIO_CELL *c = &out->mmio[a - BUS_MAP_MMIO_FIRST];
                    bus_map_add_owner(c->owners, &c->owner_count, &o);
                    c->when_mask |= o.when_mask;
                }
            }
        } else if (want_ports) {
            for (unsigned p = r->first; p <= r->last && p <= 0xFFu; p++) {
                bus_map_add_owner(out->ports[p].owners, &out->ports[p].owner_count, &o);
            }
        }
        if (want_dev) {
            for (unsigned a = r->first; a <= r->last; a++) bus_map_device_add(out, &o, r->mmio != 0);
        }
    }

    /* Zařízení devbus (registrovaná; připojení určuje connected). */
    if (out->devbus_initialized) {
        for (unsigned s = 0; s < out->devbus_registered; s++) {
            const st_DEVBUS_DEVICE_DESC *d = devbus_desc((int) s);
            if (!d) continue;
            bool conn = devbus_is_connected((int) s);
            st_BUS_DEVICE *dv = want_dev ? bus_map_device(out, d->id) : NULL;
            if (dv && !dv->name) {
                dv->name = d->name;
                dv->kind = BUS_OWNER_DEVBUS;
                dv->devbus_slot = (int8_t) s;
                dv->connected = conn ? 1 : 0;
                dv->has_glyph = (d->video && d->video->glyph_source) ? 1 : 0;
                dv->has_row = (d->video && d->video->render_row) ? 1 : 0;
                dv->region_count = (uint8_t) d->region_count;
                for (unsigned v = 0; v < devbus_vout_count(); v++) {
                    if (devbus_vout_slot(devbus_vout_at(v)) == (int) s) dv->vout_count++;
                }
            }
            for (unsigned k = 0; k < d->port_count; k++) {
                st_BUS_OWNER o;
                bus_map_owner_from_port_range((int) s, d, &d->ports[k], conn, &o);
                if (want_ports) {
                    for (unsigned p = d->ports[k].first; p <= d->ports[k].last; p++) {
                        bus_map_add_owner(out->ports[p].owners, &out->ports[p].owner_count, &o);
                    }
                }
                if (want_dev) {
                    for (unsigned p = d->ports[k].first; p <= d->ports[k].last; p++) bus_map_device_add(out, &o, false);
                }
            }
            for (unsigned k = 0; k < d->mmio_count; k++) {
                st_BUS_OWNER o;
                bus_map_owner_from_mmio_range((int) s, k, d, conn, &o);
                if (want_mmio) {
                    for (unsigned a = d->mmio[k].first; a <= d->mmio[k].last; a++) {
                        if (a < BUS_MAP_MMIO_FIRST || a > BUS_MAP_MMIO_FIRST + 255u) continue;
                        st_BUS_MMIO_CELL *c = &out->mmio[a - BUS_MAP_MMIO_FIRST];
                        bus_map_add_owner(c->owners, &c->owner_count, &o);
                        c->when_mask |= o.when_mask;
                    }
                }
                if (want_dev) {
                    for (unsigned a = d->mmio[k].first; a <= d->mmio[k].last; a++) {
                        if (a >= BUS_MAP_MMIO_FIRST && a <= BUS_MAP_MMIO_FIRST + 255u) bus_map_device_add(out, &o, true);
                    }
                }
            }
        }
    }

    /* Příznaky a aktivita. */
    bool want_act = (want & BUS_WANT_ACTIVITY) != 0;
    uint32_t frame = out->frame;
    if (want_ports) {
        for (unsigned p = 0; p < 256; p++) {
            st_BUS_PORT *bp = &out->ports[p];
            bp->flags = bus_map_owner_flags(bp->owners, bp->owner_count);
            uint64_t dev_conf = out->devbus_initialized ? devbus_port_conflict_hits((uint8_t) p) : 0;
            if (want_act) {
                uint64_t ghost = 0, conf = 0;
                io_activity_get_totals_8bit((uint8_t) p, &ghost, &conf, &bp->total_in, &bp->total_out);
                bp->ghost_hits = ghost;
                bp->conflict_hits = conf;
                bp->hits_in_per_sec = io_activity_get_hits_per_sec_8bit((uint8_t) p, true);
                bp->hits_out_per_sec = io_activity_get_hits_per_sec_8bit((uint8_t) p, false);
                uint8_t v = 0;
                uint32_t age = 0;
                if (io_activity_get_last_value_8bit((uint8_t) p, true, frame, &v, &age)) {
                    bp->last_read_valid = 1;
                    bp->last_read_value = v;
                    bp->last_read_age = age;
                }
                if (io_activity_get_last_value_8bit((uint8_t) p, false, frame, &v, &age)) {
                    bp->last_write_valid = 1;
                    bp->last_write_value = v;
                    bp->last_write_age = age;
                }
                if (bp->ghost_hits) bp->flags |= BUS_PORT_GHOST_SEEN;
            }
            /* Čítač devbus je nezávislý na sledování aktivity: kolize vidí i bez okna I/O. */
            if (dev_conf > bp->conflict_hits) bp->conflict_hits = dev_conf;
            if (bp->conflict_hits) bp->flags |= BUS_PORT_CONFLICT_SEEN;
        }
    }
    if (want_mmio) {
        for (unsigned i = 0; i < 256; i++) {
            st_BUS_MMIO_CELL *c = &out->mmio[i];
            c->flags = (uint8_t) (bus_map_owner_flags(c->owners, c->owner_count) & (uint8_t) ~BUS_PORT_GHOST_SEEN);
            if (want_act) {
                uint8_t v = 0;
                uint32_t age = 0;
                if (io_activity_get_last_value(c->addr, false, frame, &v, &age)) {
                    c->last_write_valid = 1;
                    c->last_write_value = v;
                    c->last_write_age = age;
                }
                c->hits_out_per_sec = io_activity_get_hits_per_sec(c->addr, false);
            }
            if (out->devbus_initialized && devbus_mmio_conflict_hits(c->addr)) c->flags |= BUS_PORT_CONFLICT_SEEN;
        }
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
