/**
 * @file   devbus_cfg.c
 * @brief  Sekce INI zařízení devbus (volby a "connected").
 *
 * Vzor ide8_init(): modul v g_cfgmain, elementy navázané na úložiště
 * devbus, cfgmodule_parse() + cfgmodule_propagate(). Uloží se standardně
 * přes cfgroot_save() při ukončení. Sekce zakládá jen devbus_register(),
 * tedy jen inicializovaný devbus (v D1 jen MZ-700).
 *
 * Opakovaná registrace téhož zařízení (testy po devbus_exit()) sekci znovu
 * použije: cfgfile modul ani element zrušit neumí. Modul založený devbusem
 * nese značku v save callbacku modulu (data = s_devbus_cfg_mark), takže se
 * pozná i po výměně kořene konfigurace; jeho elementy se jen znovu navážou.
 *
 * @par Licence: GPLv3
 */

#include "devbus_internal.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"


/** @brief Značka modulů INI založených devbusem (adresa = identita). */
static const char s_devbus_cfg_mark[] = "devbus";


/**
 * @brief Save callback modulu devbus: nic nedělá, nese jen značku.
 * @param m    Modul (nepoužito).
 * @param data Značka s_devbus_cfg_mark.
 */
static void devbus_cfg_save_mark(void *m, void *data)
{
    (void) m;
    (void) data;
}


/** @brief Založil modul @p m devbus? */
static bool devbus_cfg_is_known(CFGMOD *m)
{
    return m->save_cb.exec == devbus_cfg_save_mark && m->save_cb.data == (void *) s_devbus_cfg_mark;
}


/**
 * @brief Vrátí element @p name modulu, založí ho, když neexistuje.
 *
 * @param m    Modul.
 * @param name Klíč.
 * @param o    Volba (NULL = "connected", BOOL výchozí 0).
 * @return Element (nikdy NULL).
 */
static CFGELM *devbus_cfg_element(CFGMOD *m, const char *name, const st_DEVBUS_OPTION *o)
{
    CFGELM *e = cfgmodule_get_element_by_name(m, (char *) name);
    if (e) return e;

    if (!o) return cfgmodule_register_new_element(m, (char *) name, CFGENTYPE_BOOL, 0);

    switch (o->type) {
    case DEVBUS_OPT_BOOL:
        return cfgmodule_register_new_element(m, (char *) name, CFGENTYPE_BOOL, (int) o->def);
    case DEVBUS_OPT_UNSIGNED:
        return cfgmodule_register_new_element(m, (char *) name, CFGENTYPE_UNSIGNED,
                                              (int) o->def, (int) o->min, (int) o->max);
    case DEVBUS_OPT_KEYWORD:
    default: {
        /* Počet slov se zná až za běhu: varargs dostanou jen výchozí slovo
         * (cfgfile ho při založení vyžaduje v tabulce), zbytek se doplní
         * veřejnou funkcí. */
        e = cfgmodule_register_new_element(m, (char *) name, CFGENTYPE_KEYWORD, (int) o->def,
                                           (int) o->def, (char *) o->keywords[o->def], -1);
        unsigned n = devbus_option_keyword_count(o);
        for (unsigned k = 0; k < n; k++) {
            if (k == o->def) continue;
            cfgelement_property_add_keyword(e->pkw, (int) k, (char *) o->keywords[k]);
        }
        return e;
    }
    }
}


int devbus_cfg_register(st_DEVBUS_SLOT *s)
{
    if (!g_cfgmain) {
        fprintf(stderr, "devbus: no configuration root, device '%s' not registered\n", s->desc->id);
        return DEVBUS_ERR_INVALID;
    }
    const st_DEVBUS_DEVICE_DESC *d = s->desc;
    CFGMOD *m = cfgroot_get_module_by_name(g_cfgmain, (char *) d->ini_section);
    if (m && !devbus_cfg_is_known(m)) {
        fprintf(stderr, "devbus: INI section [%s] of device '%s' already exists\n", d->ini_section, d->id);
        return DEVBUS_ERR_INVALID;
    }
    if (!m) {
        m = cfgroot_register_new_module(g_cfgmain, (char *) d->ini_section);
        if (!m) return DEVBUS_ERR_INVALID;
        cfgmodule_set_save_cb(m, devbus_cfg_save_mark, (void *) s_devbus_cfg_mark);
    }
    s->cfg_module = m;

    CFGELM *e = devbus_cfg_element(m, "connected", NULL);
    cfgelement_bind_separate(e, &s->ini_connected, &s->connected);
    for (unsigned i = 0; i < d->option_count; i++) {
        e = devbus_cfg_element(m, d->options[i].key, &d->options[i]);
        cfgelement_bind(e, &s->opt[i]);
    }

    cfgmodule_parse(m);
    cfgmodule_propagate(m);

    /* propagate zapíše hodnotu z INI bez kontroly rozsahu klíčového slova;
     * neplatnou nahradit výchozí. */
    for (unsigned i = 0; i < d->option_count; i++) {
        if (!devbus_option_value_valid(&d->options[i], s->opt[i])) s->opt[i] = d->options[i].def;
    }
    if (s->ini_connected > 1) s->ini_connected = 1;
    return DEVBUS_OK;
}
