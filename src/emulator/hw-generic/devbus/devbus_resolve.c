/**
 * @file   devbus_resolve.c
 * @brief  Čistá rezoluce čtení sdílené datové sběrnice (kontrakt v devbus_resolve.h).
 *
 * Bez závislostí na jádru devbus ani na platformě; testuje se STANDALONE
 * (tests/peripherals/test_devbus_resolve.c).
 *
 * @par Licence: GPLv3
 */

#include "devbus_resolve.h"

#include <stddef.h>


uint8_t devbus_drive_mask(en_DEVBUS_DRIVE drive, uint8_t drive_mask)
{
    switch (drive) {
    case DEVBUS_DRIVE_TRISTATE:
    case DEVBUS_DRIVE_OPEN_COLLECTOR:
        return 0xFFu;
    case DEVBUS_DRIVE_PARTIAL:
        return drive_mask;
    case DEVBUS_DRIVE_NONE:
    case DEVBUS_DRIVE_UNKNOWN:
    default:
        return 0u;
    }
}


void devbus_resolve(const st_DEVBUS_CONTRIB *c, unsigned n, uint8_t idle,
                    en_DEVBUS_CONFLICT policy, st_DEVBUS_RESOLVED *out)
{
    uint8_t ts_mask = 0;   /* bity buzené třístavovými / částečnými budiči */
    uint8_t ts_value = 0;  /* jejich hodnota (platná v ts_mask) */
    uint8_t oc_mask = 0;   /* bity, na kterých visí aspoň jeden otevřený kolektor */
    uint8_t oc_value = 0xFFu; /* wired-AND: klid 1, kdokoli stáhne na 0 */
    uint8_t conflict = 0;

    for (unsigned i = 0; i < n; i++) {
        const st_DEVBUS_CONTRIB *p = &c[i];
        uint8_t driven = p->driven;
        if (p->drive == DEVBUS_DRIVE_NONE || p->drive == DEVBUS_DRIVE_UNKNOWN || driven == 0) continue;
        if (p->drive == DEVBUS_DRIVE_OPEN_COLLECTOR) {
            oc_mask |= driven;
            oc_value &= (uint8_t) (p->value | (uint8_t) ~driven);
            continue;
        }
        uint8_t overlap = (uint8_t) (driven & ts_mask);
        if (overlap) {
            conflict |= overlap;
            if (policy == DEVBUS_CONFLICT_AND) {
                ts_value = (uint8_t) ((ts_value & (uint8_t) ~overlap) | ((ts_value & p->value) & overlap));
            }
            /* FIRST / REFUSE: sporné bity zůstávají od dřívějšího budiče. */
        }
        uint8_t fresh = (uint8_t) (driven & (uint8_t) ~ts_mask);
        ts_value = (uint8_t) ((ts_value & (uint8_t) ~fresh) | (p->value & fresh));
        ts_mask |= driven;
    }

    /* Otevřený kolektor proti třístavovému budiči: také kolize. */
    uint8_t mixed = (uint8_t) (oc_mask & ts_mask);
    uint8_t value = ts_value;
    if (mixed) {
        conflict |= mixed;
        if (policy == DEVBUS_CONFLICT_AND) {
            value = (uint8_t) ((value & (uint8_t) ~mixed) | ((value & oc_value) & mixed));
        }
    }
    uint8_t oc_only = (uint8_t) (oc_mask & (uint8_t) ~ts_mask);
    value = (uint8_t) ((value & (uint8_t) ~oc_only) | (oc_value & oc_only));

    uint8_t driven_total = (uint8_t) (ts_mask | oc_mask);
    out->value = (uint8_t) ((value & driven_total) | (idle & (uint8_t) ~driven_total));
    out->driven = driven_total;
    out->conflict = conflict;
}


int devbus_drive_overlaps(en_DEVBUS_DRIVE da, uint8_t ma, en_DEVBUS_DRIVE db, uint8_t mb)
{
    uint8_t ea = devbus_drive_mask(da, ma);
    uint8_t eb = devbus_drive_mask(db, mb);
    if (!(ea & eb)) return 0;
    if (da == DEVBUS_DRIVE_OPEN_COLLECTOR && db == DEVBUS_DRIVE_OPEN_COLLECTOR) return 0;
    return 1;
}


const char *devbus_drive_name(en_DEVBUS_DRIVE drive)
{
    switch (drive) {
    case DEVBUS_DRIVE_NONE: return "none";
    case DEVBUS_DRIVE_TRISTATE: return "tristate";
    case DEVBUS_DRIVE_PARTIAL: return "partial";
    case DEVBUS_DRIVE_OPEN_COLLECTOR: return "open_collector";
    case DEVBUS_DRIVE_UNKNOWN:
    default: return "unknown";
    }
}


const char *devbus_conflict_name(en_DEVBUS_CONFLICT policy)
{
    switch (policy) {
    case DEVBUS_CONFLICT_AND: return "and";
    case DEVBUS_CONFLICT_FIRST: return "first";
    case DEVBUS_CONFLICT_REFUSE:
    default: return "refuse";
    }
}


/** @brief Porovnání bez ohledu na velikost písmen (jen ASCII, bez <strings.h>). */
static int devbus_resolve_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char) (ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char) (cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}


int devbus_conflict_parse(const char *text, en_DEVBUS_CONFLICT *out)
{
    if (!text || !out) return 0;
    if (devbus_resolve_ieq(text, "refuse")) { *out = DEVBUS_CONFLICT_REFUSE; return 1; }
    if (devbus_resolve_ieq(text, "and"))    { *out = DEVBUS_CONFLICT_AND;    return 1; }
    if (devbus_resolve_ieq(text, "first"))  { *out = DEVBUS_CONFLICT_FIRST;  return 1; }
    return 0;
}


const char *devbus_idle_name(en_DEVBUS_IDLE mode)
{
    return mode == DEVBUS_IDLE_CONST ? "const" : "latch";
}


int devbus_idle_parse(const char *text, en_DEVBUS_IDLE *out)
{
    if (!text || !out) return 0;
    if (devbus_resolve_ieq(text, "latch")) { *out = DEVBUS_IDLE_LATCH; return 1; }
    if (devbus_resolve_ieq(text, "const")) { *out = DEVBUS_IDLE_CONST; return 1; }
    return 0;
}
