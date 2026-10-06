/**
 * @file snap_unicard.c
 * @brief Snapshot handler: Unicard — uložení a načtení stavu Unicardu
 *
 * Připojení (connected) a emulovaná verze firmware (fw_emulated) jsou stav
 * stroje: snapshot je obnoví pro běh, INI ([UNICARD] connected,
 * fw_version) si drží volbu uživatele (snapshot_config_pin_ini_value).
 */

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "snapshot/snapshot_config.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "hw-generic/unicard/unicard.h"
#include "emulator/mzarch/mzarch_platform.h"


static en_SNAPSHOT_RESULT snap_unicard_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "unicard_state");

    /* Stav připojení */
    snapshot_xml_write_uint(w, "connected", (unsigned)g_unicard.connected);

    /* Simulovaná verze firmware (UC1=0 / UC3=1). Dispatch UNIMGR
     * příkazů (cmdREV, cmdREVD, cmdMOUNT, cmdGETCWD) závisí na této
     * volbě; pokud bychom ji při loadu nepoznali, Z80 by viděl
     * mismatch mezi původní simulací a aktuální. */
    snapshot_xml_write_uint(w, "fw_emulated", (unsigned)g_unicard.fw_emulated);

    /* Pracovní adresář (může být NULL) */
    snapshot_xml_write_string(w, "work_dir",
                              g_unicard.work_dir ? g_unicard.work_dir : "");

    snapshot_xml_close_element(w); /* unicard_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/unicard.xml", xml);
    g_free(xml);

    return res;
}


static en_SNAPSHOT_RESULT snap_unicard_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Unicard entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, "devices/unicard.xml")) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/unicard.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("unicard", "Cannot load devices/unicard.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("unicard", "Parse error in devices/unicard.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "unicard_state")) {
        SNAP_ERR("unicard", "Missing element unicard_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Stav připojení */
    unsigned uval;
    unsigned ini_connected = (unsigned)g_unicard.connected;
    unsigned ini_fw = (unsigned)g_unicard.fw_emulated;
    if (snapshot_xml_read_uint(r, "connected", &uval))
        g_unicard.connected = (en_UNICARD_CONNECTION)uval;
    snapshot_config_pin_ini_value("UNICARD", "connected", ini_connected);

    /* Simulovaná verze firmware. Nastavujeme přímo (ne přes
     * unicard_set_fw, který má side effects disconnect+reconnect -
     * při loadu snapshotu chceme čistý state set bez UI dialogu). */
    if (snapshot_xml_read_uint(r, "fw_emulated", &uval)) {
        en_UNICARD_FW fw = (en_UNICARD_FW)uval;
        /* Sanity: uc1 je validní jen pro MZ-800 - na ostatních platformách
         * force UC3 (= ochrana proti cross-mzarch loadu). */
        if (fw == UNICARD_FW_UC1 && g_mzarch_platform_numeric != 800) {
            fw = UNICARD_FW_UC3;
        }
        g_unicard.fw_emulated = fw;
    }
    /* Verze FW jen pro běh - INI hodnota se připne. */
    snapshot_config_pin_ini_value("UNICARD", "fw_version", ini_fw);

    /* Pracovní adresář (CWD uvnitř SD karty - FatFS vlastnost, stav stroje).
     * Obnovujeme přes unicard_chdir(), který nastaví g_unicard.work_dir
     * s jail kontrolou a ověřením existence adresáře.
     *
     * POZOR: dříve se zde chybně volalo unicard_set_sd_root_dirpath(work_dir),
     * což uloženou hodnotu CWD nacpalo do KONFIGURACE SD root. SD root je ale
     * host konfigurace (CFGELM, INI) - kam uživatel namapoval SD kartu - NE
     * stav stroje. Snapshot ji nesmí přepisovat (jinak se po loadu rozbije
     * nastavení SD root a navíc se neobnoví CWD). */
    char *work_dir = NULL;
    if (snapshot_xml_read_string(r, "work_dir", &work_dir)) {
        if (work_dir && work_dir[0] != '\0') {
            unicard_chdir(work_dir);
        }
        g_free(work_dir);
    }

    snapshot_xml_leave_element(r); /* unicard_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


void snap_unicard_register(void)
{
    snapshot_register_component("unicard",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_unicard_save,
                                snap_unicard_load,
                                true);
}
