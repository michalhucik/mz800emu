/**
 * @file bootstrap_device_cfg.c
 * @brief Kód zařízení v BC pro platformu buildu a klíč INI `[CMT] run_mzf_device`.
 *
 * Část modulu bootstrap_device.h závislá na buildu: hodnota BC pro MZARCH
 * (bootdev_setting_bc(), bootdev_choice_bc_here()) a registrace klíče INI
 * s callbacky propagate / save. Čistá část (zápis, tabulka, nastavení) je
 * v bootstrap_device.c. Výpisy pro uživatele jsou anglicky (jádro bez
 * lokalizace).
 */

#include "main.h"

#include <stdio.h>
#include <glib.h>

#include "mzarch/mzarch_config.h"
#include "mzarch/bootstrap_device.h"
#include "cfgmain.h"


uint16_t bootdev_choice_bc_here(const st_BOOTDEV_CHOICE *choice)
{
    return bootdev_choice_bc(choice, (unsigned)MZARCH);
}


uint16_t bootdev_setting_bc(void)
{
    char buf[BOOTDEV_TEXT_MAX];
    bootdev_setting_get_text(buf, sizeof(buf));
    st_BOOTDEV_CHOICE c;
    if (!bootdev_parse(buf, &c))
    {
        /* invariant nastavení to vylučuje; obrana: výchozí CMT */
        return bootdev_choice_bc_here(NULL);
    };
    return bootdev_choice_bc_here(&c);
}


/**
 * @brief Propagate callback klíče `[CMT] run_mzf_device`: INI -> nastavení.
 *
 * Neplatnou hodnotu ohlásí na stderr (anglicky, s důvodem z
 * bootdev_parse_message()) a použije výchozí `cmt`;
 * při příštím uložení INI ji save callback přepíše.
 *
 * @param e Element (st_CFGELEMENT*). @param data Nepoužito.
 * @pre Volá cfgmodule_propagate() při startu (cmt_init()).
 * @post Nastavení (bootdev_setting_get_text()) je platné.
 */
static void bootdev_cfg_propagate(void *e, void *data)
{
    (void)data;
    const char *txt = cfgelement_get_text_value((CFGELM *)e);
    en_BOOTDEV_PARSE r = bootdev_parse_ex(txt, NULL);
    if (r != BOOTDEV_PARSE_OK || !bootdev_setting_set_text(txt))
    {
        fprintf(stderr, "Warning: invalid [%s] %s value '%s' (%s), using '%s'\n", BOOTDEV_CFG_MODULE,
                BOOTDEV_CFG_ELEMENT, txt ? txt : "", bootdev_parse_message(r), BOOTDEV_DEFAULT_TEXT);
        (void)bootdev_setting_set_text(BOOTDEV_DEFAULT_TEXT);
    };
}


/**
 * @brief Save callback klíče `[CMT] run_mzf_device`: nastavení -> INI.
 * @param e Element (st_CFGELEMENT*). @param data Nepoužito.
 * @post Text elementu je aktuální text nastavení.
 */
static void bootdev_cfg_save(void *e, void *data)
{
    (void)data;
    char buf[BOOTDEV_TEXT_MAX];
    bootdev_setting_get_text(buf, sizeof(buf));
    cfgelement_set_text_value((CFGELM *)e, buf);
}


void bootdev_cfg_register(void *cmod_void)
{
    CFGMOD *cmod = (CFGMOD *)cmod_void;
    if (!cmod) return;
    CFGELM *elm = cfgmodule_register_new_element(cmod, BOOTDEV_CFG_ELEMENT, CFGENTYPE_TEXT,
                                                 BOOTDEV_DEFAULT_TEXT);
    cfgelement_set_propagate_cb(elm, bootdev_cfg_propagate, NULL);
    cfgelement_set_save_cb(elm, bootdev_cfg_save, NULL);
}
