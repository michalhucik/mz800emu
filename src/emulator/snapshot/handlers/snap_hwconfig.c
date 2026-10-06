/**
 * @file snap_hwconfig.c
 * @brief Snapshot handler: HW konfigurace — uložení a načtení HW nastavení relevantních pro emulaci
 *
 * Volba HWCOMPAT allow_psg1 (druhý PSG) je stav stroje: snapshot ji obnoví
 * pro běh, INI ([HWCOMPAT] allow_psg1) si drží volbu uživatele
 * (snapshot_config_pin_ini_value). Druhý PSG se zakládá jen při startu
 * emulátoru (mz800_main.c), obnovená volba ho tedy nezaloží [neověřeno,
 * zda to vadí].
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "snapshot/snapshot_config.h"
#include "mzarch/mzarch.h"
#include "hw-generic/psg/psg.h"


static en_SNAPSHOT_RESULT snap_hwconfig_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "hwconfig");

    /* Zadní přepínač SW1 (MZ-700 / MZ-800 mód, en_MZ800_MODE_SW) - jen
     * MZ-800/MZ-1500; MZ-700 nativní ho nemá (je vždy v MZ-700 módu).
     * Název elementu switch700 zůstává kvůli kompatibilitě snapshotů. */
#if MZARCH != 700
    snapshot_xml_write_int(w, "switch700", (int)g_mzarch_main.mode_sw);
#endif

    /* Audio režim (mono/stereo) - jen pro platformy s PSG */
#if HAVE_PSG >= 1
    snapshot_xml_write_bool(w, "psg_stereo", g_psg_module.stereo);
#endif

    /* Nastavení HW kompatibility - pouze MZ-800 ma konfigurovatelne HW experimenty */
#if MZARCH == 800
    snapshot_xml_write_int(w, "hwcompat_allow_psg1",
                           (int)g_mzarch_main.mz800_hwcompat_allow_psg1);
#endif /* MZARCH == 800 */

    snapshot_xml_close_element(w); /* hwconfig */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "config.xml", xml);
    g_free(xml);

    return res;
}


static en_SNAPSHOT_RESULT snap_hwconfig_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "config.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("hwconfig", "Cannot load config.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("hwconfig", "Parse error in config.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "hwconfig")) {
        SNAP_ERR("hwconfig", "Missing element hwconfig");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    int ival;
    bool bval;

    /* Přepínač kompatibility MZ-700 - viz save block. */
#if MZARCH != 700
    {
        /* Element switch700 nese en_MZ800_MODE_SW (0 = MZ-700, 1 = MZ-800).
         * Poloha přepínače jen pro běh - INI hodnota se připne (MZ-800). */
        unsigned ini_mode_sw = (unsigned)g_mzarch_main.mode_sw;
        if (snapshot_xml_read_int(r, "switch700", &ival))
            g_mzarch_main.mode_sw = (ival == MZ800_MODE_SW_MZ800) ? MZ800_MODE_SW_MZ800 : MZ800_MODE_SW_MZ700;
#if MZARCH == 800
        snapshot_config_pin_ini_value("MZ800", "mode_switch", ini_mode_sw);
#else
        (void)ini_mode_sw;
#endif
    }
#endif

    /* Audio režim - jen pro platformy s PSG */
#if HAVE_PSG >= 1
    if (snapshot_xml_read_bool(r, "psg_stereo", &bval))
        g_psg_module.stereo = bval;
#endif

    /* Nastavení HW kompatibility - pouze MZ-800 ma konfigurovatelne HW experimenty.
     * Klice hwcompat_mz700_pal_timing a hwcompat_mz700_fixed_e008 jsou v starsich
     * snapshotech pripadne ignorovany (snapshot_xml_read_int vraci false). */
#if MZARCH == 800
    {
        /* allow_psg1 jen pro běh - INI hodnota se připne. */
        unsigned ini_allow_psg1 = (unsigned)g_mzarch_main.mz800_hwcompat_allow_psg1;
        if (snapshot_xml_read_int(r, "hwcompat_allow_psg1", &ival))
            g_mzarch_main.mz800_hwcompat_allow_psg1 = (en_MZ800_HWCOMPAT_ALLOW_PSG1)ival;
        snapshot_config_pin_ini_value("HWCOMPAT", "allow_psg1", ini_allow_psg1);
    }
#endif /* MZARCH == 800 */

    snapshot_xml_leave_element(r); /* hwconfig */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


void snap_hwconfig_register(void)
{
    snapshot_register_component("hwconfig",
                                SNAPSHOT_PRIORITY_CONFIG,
                                snap_hwconfig_save,
                                snap_hwconfig_load,
                                false);
}
