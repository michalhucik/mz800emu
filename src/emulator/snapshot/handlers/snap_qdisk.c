/**
 * @file snap_qdisk.c
 * @brief Snapshot handler: Quick Disk - uložení a načtení stavu Quick Disku
 *
 * Snapshot ukládá stav čipu a mechaniky (kanály SIO, `image_position`,
 * `status`, `out_crc16`) a informativní pole média (`readonly` = efektivní
 * ochrana, `filename`). Obsah obrazu se neukládá (neuložené změny v paměti
 * snapshot nenese, stejně jako u disket), stav virtuálního Quick Disku
 * (`virt_status`, rozečtený soubor MZF) také ne.
 *
 * ## Vložené médium je vlastnost hostitele, ne stavu stroje
 *
 * Načtení obnoví stav čipu a mechaniky (registry SIO, polohu hlavy
 * `image_position`, bity stavu "hlava na začátku" a "synchronizace").
 * Co je v mechanice vložené, nemění: obraz znovu nevkládá a pole `filename`
 * ani `readonly` nepřepisuje. Mechanika s obrazem si nechá obraz, jméno
 * souboru, ochranu proti zápisu, režim ukládání, identitu souboru
 * i neuložené změny; prázdná mechanika zůstane prázdná. Pole `status`,
 * `readonly` a `filename` se do snapshotu dál zapisují (formát se nemění,
 * starší verze emulátoru je čtou).
 *
 * Bity stavu, které popisují médium, se po načtení řídí vloženým médiem,
 * ne snapshotem (snap_qdisk_media_status()):
 *  - QDSTS_IMG_READY: mechanika s obrazem (typ IMAGE a UNICARD) je
 *    připravená, právě když drží otevřený obraz; virtuální Quick Disk si
 *    nechává hodnotu z doby před načtením (otevřený adresář);
 *  - QDSTS_IMG_READONLY: efektivní ochrana vloženého média.
 * Emulovaný stroj tak nevidí disk v prázdné mechanice ani nezapisuje na
 * chráněný disk. Poloha hlavy se bere ze snapshotu i při jiném vloženém
 * médiu (je to stav mechaniky, stejně jako stopa u disket): program, který
 * byl při uložení uprostřed čtení, pokračuje od téže polohy na jiném
 * disku a dostane jeho data.
 *
 * Typ mechaniky a připojení (`type`, `connected`) jsou stav stroje a berou
 * se ze snapshotu. Když snapshot mění druh mechaniky mezi mechanikou
 * s obrazem (IMAGE, UNICARD) a virtuální (VIRTUAL), vložené médium k novému
 * druhu nepatří: zavře se jako při vysunutí (neuložené změny se uloží do
 * svého souboru, qdisk_umount_keep_cfg()) a připojená mechanika nového
 * druhu dostane médium, které pro něj drží INI, stejně jako při startu
 * emulátoru (qdisk_open(): obraz z `mz1f11_std_filepath`, adresář
 * z `mz1f11_virtual_filepath`, obraz zavaděče Unicard). Mezi typy IMAGE
 * a UNICARD obraz zůstává.
 *
 * Liší-li se vložený obraz od obrazu ze snapshotu, dostane uživatel řádek
 * v souhrnné zprávě společné s disketami (snapshot_media.h). Adresář
 * virtuálního Quick Disku snapshot nenese, proto se neporovnává; snapshot
 * bez polí média (starší formát, pozná se podle chybějícího `readonly`) se
 * neporovnává také.
 *
 * Výjimka pro záznam vstupů (replay), kde se dosavadní chování nemění.
 * Hash `periph` záznamu (replay.c, replay_compute_hash()) zahrnuje výstup
 * této komponenty včetně polí `status`, `readonly` a `filename` a spoléhá
 * na to, že je načtení snapshotu vrátí; jinak by přehrání hlásilo rozjetí.
 *  - Při načtení snapshotů záznamu vstupů (snapshot_media_set_replay_load():
 *    startovní snapshot, checkpointy) se `status`, `readonly` a neprázdné
 *    `filename` berou ze snapshotu vždy; zpráva o rozdílu se neukazuje.
 *  - Obraz nasazený z paměti pro přehrání (`replay_image`,
 *    qdisk_mount_image_buffer()) si tato pole bere z každého snapshotu.
 * Důsledek: při přehrání staršího záznamu bez seznamu médií a po návratu
 * na checkpoint během nahrávání může `filename` mechaniky s obrazem ze
 * souboru dál nést jméno ze snapshotu. Zápis takové kopie do cizího souboru
 * hlídá pojistka mechaniky (qdisk.c, qdisk_save_target_ok(): obraz se
 * neuloží do jiného souboru, než ze kterého byl načten).
 *
 * Zpětná kompatibilita: starší snapshot bez polí `readonly` a `filename`
 * projde načtením - `snapshot_xml_read_*` vrátí false a hodnoty mechaniky
 * se nezmění.
 *
 * Připojení a typ mechaniky (connected, type) snapshot obnoví pro běh, INI
 * ([QDISK] mz1f11_connected, mz1f11_type) si drží volbu uživatele
 * (snapshot_config_pin_ini_value).
 */

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "snapshot/snapshot_config.h"
#include "snapshot/snapshot_media.h"

#if CFG_HWEXT_HAVE_QDISK

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/fdc/fdc_fileid.h"
#include "baseui/baseui.h"
#include "i18n.h"

/** @brief Bity `status`, které popisují vložené médium (ne mechaniku). */
#define SNAP_QDISK_MEDIA_BITS (QDSTS_IMG_READY | QDSTS_IMG_READONLY)


/**
 * @brief Je typ mechaniky mechanika s obrazem (IMAGE nebo UNICARD)?
 * @param type Hodnota `st_QDISK.type`.
 * @return true = IMAGE nebo UNICARD, false = VIRTUAL (a neznámé hodnoty).
 */
static bool snap_qdisk_type_has_image(unsigned type)
{
    return (type == QDISK_TYPE_IMAGE) || (type == QDISK_TYPE_UNICARD);
}


/**
 * @brief Bity stavu média podle toho, co je v mechanice skutečně vložené.
 *
 * Mechanika s obrazem: připravená, právě když drží otevřený obraz; chráněná
 * podle efektivní ochrany vloženého média. Virtuální Quick Disk: bity,
 * které má mechanika teď (otevřený adresář a volba ochrany).
 *
 * @return Kombinace QDSTS_IMG_READY a QDSTS_IMG_READONLY.
 * @pre `g_qdisk.type` už nese typ, se kterým mechanika poběží.
 */
static unsigned snap_qdisk_media_status(void)
{
    if (!snap_qdisk_type_has_image(g_qdisk.type)) return g_qdisk.status & SNAP_QDISK_MEDIA_BITS;
    if (!g_qdisk.handler_valid) return 0;
    return QDSTS_IMG_READY | (g_qdisk.readonly ? QDSTS_IMG_READONLY : 0);
}


/**
 * @brief Porovná obraz ze snapshotu s obrazem vloženým v mechanice a rozdíl zapíše do souhrnné zprávy.
 *
 * Shoda = obě prázdné, nebo stejná cesta, nebo tentýž soubor podle identity
 * z vložení (fdc_fileid.h; zjišťuje se jen při různých cestách a jen
 * u obrazu ze souboru). Virtuální Quick Disk se počítá jako prázdná
 * mechanika na obou stranách (adresář snapshot nenese).
 *
 * @param snap_has  Snapshot byl uložen s obrazem v mechanice.
 * @param snap_path Pole `filename` ze snapshotu (platí při snap_has).
 * @pre Vlákno emulátoru; `g_qdisk` už je ve stavu po načtení.
 */
static void snap_qdisk_media_compare(bool snap_has, const char *snap_path)
{
    const bool now_has = snap_qdisk_type_has_image(g_qdisk.type) && g_qdisk.handler_valid;
    if (!snap_has && !now_has) return;
    if (snap_has && now_has) {
        if (strcmp(snap_path, g_qdisk.filename) == 0) return;
        if (g_qdisk.file_backed) {
            st_FDC_FILEID id;
            fdc_fileid_init(&id, snap_path);
            const bool same = fdc_fileid_same(&g_qdisk.fileid, &id);
            fdc_fileid_clear(&id);
            if (same) return;
        }
    }
    char *was = NULL;
    char *now = NULL;
    snapshot_media_notice_names(snap_has ? snap_path : NULL, now_has ? g_qdisk.filename : NULL, &was, &now);
    const char *empty_tr = baseui_translate(N_("empty"));
    char *line_en = g_strdup_printf("Quick Disk: %s (now: %s)", was ? was : "empty", now ? now : "empty");
    char *line_tr = g_strdup_printf(baseui_translate(N_("Quick Disk: %s (now: %s)")),
                                    was ? was : empty_tr, now ? now : empty_tr);
    snapshot_media_notice_add(line_en, line_tr);
    g_free(line_en);
    g_free(line_tr);
    g_free(was);
    g_free(now);
}


/**
 * @brief Zapíše stav Quick Disku do `devices/qdisk.xml`.
 * @param ctx Snapshot kontext.
 * @return SNAPSHOT_OK, nebo chyba zápisu.
 */
static en_SNAPSHOT_RESULT snap_qdisk_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "qdisk_state");

    /* Základní stav */
    snapshot_xml_write_uint(w, "connected", g_qdisk.connected);
    snapshot_xml_write_uint(w, "type", g_qdisk.type);
    snapshot_xml_write_uint(w, "status", g_qdisk.status);
    snapshot_xml_write_uint(w, "image_position", g_qdisk.image_position);
    snapshot_xml_write_hex16(w, "out_crc16", g_qdisk.out_crc16);

    /* Informativní pole média (vzor snap_fdc.c); běžné načtení je čte jen
     * kvůli porovnání. Mount user pref (storage_mode, user_readonly) se
     * neukládá - SSOT je INI. fs_readonly se neukládá - runtime detect při
     * mount. */
    snapshot_xml_write_int(w, "readonly", g_qdisk.readonly);
    snapshot_xml_write_string(w, "filename", g_qdisk.filename);

    /* SIO kanály */
    for (int ch = 0; ch < 2; ch++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "channel_%d", ch);
        snapshot_xml_open_element(w, elem_name);

        /* Zápisové registry (8 ks) */
        for (int j = 0; j < 8; j++) {
            char reg_name[16];
            snprintf(reg_name, sizeof(reg_name), "Wreg_%d", j);
            snapshot_xml_write_hex8(w, reg_name, g_qdisk.channel[ch].Wreg[j]);
        }

        /* Čtecí registry (3 ks) */
        for (int j = 0; j < 3; j++) {
            char reg_name[16];
            snprintf(reg_name, sizeof(reg_name), "Rreg_%d", j);
            snapshot_xml_write_hex8(w, reg_name, g_qdisk.channel[ch].Rreg[j]);
        }

        /* Adresa registru */
        snapshot_xml_write_uint(w, "REG_addr", (unsigned)g_qdisk.channel[ch].REG_addr);

        snapshot_xml_close_element(w); /* channel_N */
    }

    snapshot_xml_close_element(w); /* qdisk_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/qdisk.xml", xml);
    g_free(xml);

    return res;
}


/**
 * @brief Načte stav Quick Disku; vložené médium zůstává beze změny.
 *
 * Obnoví čip SIO, polohu hlavy a stav mechaniky; jméno souboru, ochranu
 * proti zápisu a přítomnost disku nechává podle vloženého média (viz popis
 * souboru). Při rozdílu proti snapshotu připojí řádek do souhrnné zprávy
 * (snapshot_media.h). Snapshot záznamu vstupů a obraz nasazený pro přehrání
 * se načítají jako dřív (pole média ze snapshotu).
 *
 * @param ctx Snapshot kontext.
 * @return SNAPSHOT_OK (i když entry chybí), nebo chyba čtení / XML.
 * @pre Vlákno emulátoru, emulace nestojí uprostřed přístupu na Quick Disk.
 * @post Mimo záznam vstupů: QDSTS_IMG_READY a QDSTS_IMG_READONLY odpovídají
 *       vloženému médiu; `filename`, `readonly`, obraz a jeho neuložené
 *       změny se nezměnily, ledaže snapshot změnil druh mechaniky (pak
 *       jsou změny uložené do svého souboru a mechanika drží médium
 *       nového druhu z INI, nebo je prázdná).
 */
static en_SNAPSHOT_RESULT snap_qdisk_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Quick Disk entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, "devices/qdisk.xml")) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/qdisk.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("qdisk", "Cannot load devices/qdisk.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("qdisk", "Parse error in devices/qdisk.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "qdisk_state")) {
        SNAP_ERR("qdisk", "Missing element qdisk_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Základní stav. Pole se čtou vždy (formát se nemění); co s poli média,
     * rozhoduje to, co mechanika drží (viz popis souboru). */
    const unsigned ini_connected = g_qdisk.connected;
    const unsigned ini_type = g_qdisk.type;
    unsigned snap_connected = g_qdisk.connected;
    unsigned snap_type = g_qdisk.type;
    unsigned snap_status = 0;
    unsigned snap_position = 0;
    int snap_rdo = 0;
    char *fname = NULL;
    snapshot_xml_read_uint(r, "connected", &snap_connected);
    snapshot_xml_read_uint(r, "type", &snap_type);
    const bool have_status = snapshot_xml_read_uint(r, "status", &snap_status);
    const bool have_position = snapshot_xml_read_uint(r, "image_position", &snap_position);

    uint16_t hval16;
    if (snapshot_xml_read_hex16(r, "out_crc16", &hval16)) g_qdisk.out_crc16 = hval16;

    const bool have_rdo = snapshot_xml_read_int(r, "readonly", &snap_rdo);
    const bool have_fname = snapshot_xml_read_string(r, "filename", &fname);

    if (snapshot_media_is_replay_load() || g_qdisk.replay_image) {
        /* Záznam vstupů: dosavadní chování beze změny. */
        g_qdisk.connected = snap_connected;
        g_qdisk.type = snap_type;
        if (have_status) g_qdisk.status = snap_status;
        if (have_position) g_qdisk.image_position = snap_position;
        if (have_rdo) g_qdisk.readonly = snap_rdo;
        if (have_fname && fname && fname[0] != '\0') {
            g_strlcpy(g_qdisk.filename, fname, sizeof(g_qdisk.filename));
        }
    } else {
        /* Snapshot mění druh mechaniky (s obrazem / virtuální): vložené
         * médium k novému druhu nepatří. Zavřít ho jako při vysunutí a do
         * mechaniky nového druhu vložit médium, které pro něj drží INI
         * (jako při startu emulátoru). */
        const bool kind_changed = snap_qdisk_type_has_image(g_qdisk.type) != snap_qdisk_type_has_image(snap_type);
        if (kind_changed) qdisk_umount_keep_cfg();
        g_qdisk.connected = snap_connected;
        g_qdisk.type = snap_type;
        if (kind_changed && QDISK_TEST_CONNECTED) qdisk_open();

        /* Stav mechaniky (poloha hlavy, synchronizace) ze snapshotu, bity
         * média podle vloženého média. */
        if (have_position) g_qdisk.image_position = snap_position;
        const unsigned mech = (have_status ? snap_status : g_qdisk.status) & ~SNAP_QDISK_MEDIA_BITS;
        g_qdisk.status = mech | snap_qdisk_media_status();

        /* Pole média nese snapshot od zavedení `readonly` a `filename`;
         * prázdné `filename` se od chybějícího nepozná, proto rozhoduje
         * přítomnost `readonly`. */
        if (have_rdo) {
            const bool snap_has = snap_qdisk_type_has_image(snap_type) && have_status
                                  && (snap_status & QDSTS_IMG_READY) && have_fname && fname && fname[0] != '\0';
            snap_qdisk_media_compare(snap_has, fname);
        }
    }
    g_free(fname);

    /* connected a type jen pro běh - INI hodnoty se připnou. */
    snapshot_config_pin_ini_value("QDISK", "mz1f11_connected", ini_connected);
    snapshot_config_pin_ini_value("QDISK", "mz1f11_type", ini_type);

    /* SIO kanály */
    for (int ch = 0; ch < 2; ch++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "channel_%d", ch);

        if (snapshot_xml_enter_element(r, elem_name)) {
            uint8_t hval8;

            /* Zápisové registry */
            for (int j = 0; j < 8; j++) {
                char reg_name[16];
                snprintf(reg_name, sizeof(reg_name), "Wreg_%d", j);
                if (snapshot_xml_read_hex8(r, reg_name, &hval8))
                    g_qdisk.channel[ch].Wreg[j] = hval8;
            }

            /* Čtecí registry */
            for (int j = 0; j < 3; j++) {
                char reg_name[16];
                snprintf(reg_name, sizeof(reg_name), "Rreg_%d", j);
                if (snapshot_xml_read_hex8(r, reg_name, &hval8))
                    g_qdisk.channel[ch].Rreg[j] = hval8;
            }

            /* Adresa registru */
            unsigned uval;
            if (snapshot_xml_read_uint(r, "REG_addr", &uval))
                g_qdisk.channel[ch].REG_addr = (en_QDSIO_REGADRR)uval;

            snapshot_xml_leave_element(r); /* channel_N */
        }
    }

    snapshot_xml_leave_element(r); /* qdisk_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


/** @brief Zaregistruje komponentu "qdisk" (volitelná, priorita zařízení). */
void snap_qdisk_register(void)
{
    snapshot_register_component("qdisk",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_qdisk_save,
                                snap_qdisk_load,
                                true);
}

#endif /* CFG_HWEXT_HAVE_QDISK */
