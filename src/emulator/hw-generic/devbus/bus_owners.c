/**
 * @file   bus_owners.c
 * @brief  Pomocné funkce nad statickou tabulkou vlastníků (bus_owners.h).
 *
 * @par Licence: GPLv3
 */

#include "bus_owners.h"


bool bus_owners_port_is_static(const st_BUS_STATIC_TABLE *t, uint8_t port_lsb)
{
    if (!t || !t->rows) return false;
    for (unsigned i = 0; i < t->count; i++) {
        const st_BUS_STATIC_OWNER *r = &t->rows[i];
        if (r->mmio) continue;
        if (port_lsb >= r->first && port_lsb <= r->last) return true;
    }
    return false;
}
