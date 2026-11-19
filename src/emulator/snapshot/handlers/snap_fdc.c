/**
 * @file snap_fdc.c
 * @brief Snapshot handler: FDC floppy - uložení a načtení stavu řadiče
 *        floppy disků.
 *
 * Ukládá stav chipu WD279x (registry, state machine včetně přechodného
 * RNF na konci stopy `pending_rnf_end` a seznamu ID sektorů READ TRACK
 * `rt_sinfo`) a per-drive informace (`mounted`, `readonly`, `filename`).
 *
 * ## Vložené médium je vlastnost hostitele, ne stavu stroje
 *
 * Načtení obnoví stav řadiče a čipu (registry, poloha hlavy, motor, buffer).
 * Co je vložené v mechanikách, nemění: obrazy znovu nevkládá a pole
 * `mounted`, `filename` ani `readonly` mechaniky nepřepisuje. Mechanika
 * s obrazem ze souboru si nechá obraz, jméno, ochranu proti zápisu, režim
 * ukládání, identitu souboru i neuložené změny; prázdná mechanika zůstane
 * prázdná. Pole `mounted`, `readonly` a `filename` se do snapshotu dál
 * zapisují (formát se nemění, starší verze emulátoru je čtou) a při načtení
 * se přečtou jen kvůli porovnání: liší-li se vložené diskety od těch ze
 * snapshotu, dostane uživatel jednu souhrnnou zprávu společnou s Quick
 * Diskem (snapshot_media.h; ukazuje ji správce snapshotů po načtení všech
 * komponent).
 *
 * Výjimka pro záznam vstupů (replay), kde se dosavadní chování nemění.
 * Hash `periph` záznamu (replay.c, replay_compute_hash()) zahrnuje výstup
 * této komponenty včetně polí `mounted`, `readonly` a `filename` a spoléhá
 * na to, že je načtení snapshotu vrátí; jinak by přehrání hlásilo rozjetí.
 *  - Při načtení snapshotů záznamu vstupů (snapshot_media_set_replay_load():
 *    startovní snapshot, checkpointy) se `mounted` a neprázdné `filename`
 *    berou ze snapshotu u každé mechaniky, `readonly` u mechaniky bez obrazu
 *    ze souboru; zpráva o rozdílu se neukazuje.
 *  - Mechanika s obrazem nasazeným z paměti (`handler_valid` bez
 *    `file_backed`, fdc_mount_dsk_buffer(): přehrávání) si tato pole bere
 *    z každého snapshotu.
 * Důsledek: při přehrání staršího záznamu bez seznamu médií a po návratu
 * na checkpoint během nahrávání může `filename` mechaniky s obrazem ze
 * souboru dál nést jméno ze snapshotu. Zápis takové kopie do cizího souboru
 * hlídá pojistka řadiče (fdc.c, fdc_drive_save_target_ok(): disk se neuloží
 * do jiného souboru, než ze kterého byl načten).
 *
 * Ochrana proti zápisu mechaniky s obrazem ze souboru (`file_backed`) se
 * řídí vždy vloženým médiem: volba uživatele NEBO soubor chráněný na disku
 * NEBO sdílení obrazu mezi mechanikami (fdc.h).
 * fdc_enforce_single_writer() po načtení zůstává jako pojistka invariantu
 * jediné zapisující mechaniky.
 *
 * Připojení řadiče (connected) je stav stroje: snapshot ho obnoví pro
 * běh, INI ([FDC] / [FDC1] connected) si drží volbu uživatele
 * (snapshot_config_pin_ini_value).
 *
 * ## Zpětná kompatibilita
 *
 * Historicky existovaly dvě implementační varianty FDC (_old_/_new_)
 * a snapshot na nejvyšší úrovni nesl atribut `impl="old|new"` v elementu
 * `<fdc_state>`. OLD byla odstraněna, NEW je jediná. Nové snapshoty
 * atribut `impl` nezapisují (čistší formát). Při loadu:
 *  - chybějící `impl` nebo `impl="new"` -> standardní NEW load
 *  - `impl="old"` -> log warning a skip (staré OLD snapshoty nelze
 *    obnovit, ale nepadne)
 */

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "snapshot/snapshot_config.h"
#include "snapshot/snapshot_media.h"

#if CFG_HWEXT_HAVE_FDC

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>

#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_fileid.h"
#include "baseui/baseui.h"
#include "i18n.h"


/* ============================================================
 * Rozdíl mezi disketami ze snapshotu a vloženými disketami
 * ============================================================ */

/**
 * @brief Připojí k rozdílu řádek jedné mechaniky.
 *
 * Jména vybírá snapshot_media_notice_names() (bez cest; celé cesty, když
 * se soubory liší jen adresářem).
 *
 * @param drive_no  Číslo mechaniky přes oba řadiče (0-7).
 * @param snap_path Cesta ze snapshotu, NULL = mechanika byla prázdná.
 * @param now_path  Cesta vloženého obrazu, NULL = mechanika je prázdná.
 * @pre Aspoň jedna z cest není NULL. Vlákno emulátoru.
 */
static void snap_fdc_media_notice_add(int drive_no, const char *snap_path, const char *now_path)
{
    char *was = NULL;
    char *now = NULL;
    snapshot_media_notice_names(snap_path, now_path, &was, &now);
    const char *empty_tr = baseui_translate(N_("empty"));
    char *line_en = g_strdup_printf("FDD %d: %s (now: %s)", drive_no, was ? was : "empty", now ? now : "empty");
    char *line_tr = g_strdup_printf(baseui_translate(N_("FDD %d: %s (now: %s)")), drive_no,
                                    was ? was : empty_tr, now ? now : empty_tr);
    snapshot_media_notice_add(line_en, line_tr);
    g_free(line_en);
    g_free(line_tr);
    g_free(was);
    g_free(now);
}


/**
 * @brief Porovná disketu ze snapshotu s disketou vloženou v mechanice a rozdíl zapíše do zprávy.
 *
 * Shoda = obě prázdné, nebo stejná cesta, nebo tentýž soubor podle identity
 * z vložení (fdc_fileid.h: jiný zápis téže cesty, pevný odkaz). Identita
 * cesty ze snapshotu je jedno otevření souboru a zjišťuje se jen při
 * různých cestách.
 *
 * @param fdc          Řadič.
 * @param drive        Mechanika (0..FDC_NUM_DRIVES-1); drží obraz ze souboru, nebo je prázdná.
 * @param snap_mounted Pole `mounted` ze snapshotu.
 * @param snap_path    Pole `filename` ze snapshotu (NULL nebo "" = žádné).
 * @pre Vlákno emulátoru.
 */
static void snap_fdc_media_compare(const st_FDC *fdc, unsigned drive, unsigned snap_mounted, const char *snap_path)
{
    const st_FDDrive *drv = &fdc->drive[drive];
    const bool snap_has = snap_mounted && snap_path && snap_path[0] != '\0';
    const bool now_has = drv->mounted && drv->handler_valid;
    if (!snap_has && !now_has) return;
    if (snap_has && now_has) {
        if (strcmp(snap_path, drv->filename) == 0) return;
        st_FDC_FILEID id;
        fdc_fileid_init(&id, snap_path);
        const bool same = fdc_fileid_same(&drv->fileid, &id);
        fdc_fileid_clear(&id);
        if (same) return;
    }
    snap_fdc_media_notice_add(FDC_GLOBAL_DRIVE_NO(fdc->index, drive), snap_has ? snap_path : NULL,
                              now_has ? drv->filename : NULL);
}


/* ============================================================
 * Save (always NEW)
 * ============================================================ */

static en_SNAPSHOT_RESULT snap_fdc_save_instance(st_SNAPSHOT_CONTEXT *ctx, st_FDC *fdc,
                                                 const char *xml_entry, const char *buf_entry)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    /* Nové snapshoty nezapisují atribut `impl` - existuje jen jedna impl. */
    snapshot_xml_open_element(w, "fdc_state");

    snapshot_xml_write_uint(w, "connected", fdc->connected);

    /* Config switche (hd_patch, bus_xlate) - ukládáme pro informativní
     * účely; jsou ovládané CLI/cfgfile, nemusí se při restore aplikovat. */
    snapshot_xml_write_int(w, "hd_patch", fdc->hd_patch);
    snapshot_xml_write_int(w, "bus_xlate", (int)fdc->bus_xlate);

    /* Registry chipu (true-bus hodnoty). */
    snapshot_xml_open_element(w, "wd279x");

    snapshot_xml_write_hex8(w, "regSTATUS", fdc->wd279x.regSTATUS);
    snapshot_xml_write_hex8(w, "regCOMMAND", fdc->wd279x.regCOMMAND);
    snapshot_xml_write_hex8(w, "regTRACK", fdc->wd279x.regTRACK);
    snapshot_xml_write_hex8(w, "regSECTOR", fdc->wd279x.regSECTOR);
    snapshot_xml_write_hex8(w, "regDATA", fdc->wd279x.regDATA);

    snapshot_xml_write_hex8(w, "MOTOR", fdc->wd279x.MOTOR);
    snapshot_xml_write_hex8(w, "SIDE", fdc->wd279x.SIDE);
    snapshot_xml_write_hex8(w, "DENSITY", fdc->wd279x.DENSITY);
    snapshot_xml_write_hex8(w, "EINT", fdc->wd279x.EINT);

    snapshot_xml_write_hex16(w, "buffer_pos", fdc->wd279x.buffer_pos);
    snapshot_xml_write_hex16(w, "data_counter", fdc->wd279x.data_counter);
    snapshot_xml_write_hex16(w, "current_sector_size", fdc->wd279x.current_sector_size);

    snapshot_xml_write_int(w, "status_mode", (int)fdc->wd279x.status_mode);

    snapshot_xml_write_hex8(w, "multiblock_rw", fdc->wd279x.multiblock_rw);
    snapshot_xml_write_int(w, "direction_latch", (int)fdc->wd279x.direction_latch);
    snapshot_xml_write_hex8(w, "intrq_active", fdc->wd279x.intrq_active);
    snapshot_xml_write_hex8(w, "reading_status_counter", fdc->wd279x.reading_status_counter);
    snapshot_xml_write_hex8(w, "pending_busy_status", fdc->wd279x.pending_busy_status);
    snapshot_xml_write_hex8(w, "pending_drq", fdc->wd279x.pending_drq);
    snapshot_xml_write_hex8(w, "positioned_sector", fdc->wd279x.positioned_sector);
    snapshot_xml_write_hex8(w, "positioned_track", fdc->wd279x.positioned_track);
    snapshot_xml_write_hex8(w, "positioned_side", fdc->wd279x.positioned_side);
    snapshot_xml_write_hex8(w, "waitForInt", fdc->wd279x.waitForInt);
    snapshot_xml_write_hex8(w, "write_track_stage", fdc->wd279x.write_track_stage);
    snapshot_xml_write_hex16(w, "write_track_counter", fdc->wd279x.write_track_counter);
    snapshot_xml_write_hex8(w, "rt_sectors", fdc->wd279x.rt_sectors);
    snapshot_xml_write_hex16(w, "rt_sector_bytes", fdc->wd279x.rt_sector_bytes);
    snapshot_xml_write_hex8(w, "rt_ssize_code", fdc->wd279x.rt_ssize_code);
    snapshot_xml_write_int(w, "rt_cached_sec_idx", (int)fdc->wd279x.rt_cached_sec_idx);
    snapshot_xml_write_hex8(w, "pending_rnf_end", fdc->wd279x.pending_rnf_end);
    {
        /* Seznam ID sektorů READ TRACK jako hex řetězec (2 znaky na bajt). */
        char hex[sizeof(fdc->wd279x.rt_sinfo) * 2 + 1];
        for (size_t k = 0; k < sizeof(fdc->wd279x.rt_sinfo); k++) {
            snprintf(&hex[k * 2], 3, "%02x", fdc->wd279x.rt_sinfo[k]);
        }
        snapshot_xml_write_string(w, "rt_sinfo", hex);
    }

    /* Mechaniky */
    for (int i = 0; i < FDC_NUM_DRIVES; i++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "drive_%d", i);
        snapshot_xml_open_element(w, elem_name);

        snapshot_xml_write_uint(w, "mounted", fdc->drive[i].mounted);
        snapshot_xml_write_int(w, "readonly", fdc->drive[i].readonly);
        snapshot_xml_write_string(w, "filename", fdc->drive[i].filename);

        snapshot_xml_close_element(w); /* drive_N */
    }

    snapshot_xml_close_element(w); /* wd279x */
    snapshot_xml_close_element(w); /* fdc_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, xml_entry, xml);
    g_free(xml);
    if (res != SNAPSHOT_OK) return res;

    /* Buffer chipu jako binární soubor. */
    res = snapshot_io_write_bin(ctx->io, buf_entry,
                                fdc->wd279x.buffer, WD279X_BUFFER_SIZE);

    return res;
}


/* ============================================================
 * Load (NEW)
 * ============================================================ */

static en_SNAPSHOT_RESULT snap_fdc_load_instance(st_SNAPSHOT_CONTEXT *ctx,
                                                 snapshot_xml_reader_t *r,
                                                 st_FDC *fdc, const char *buf_entry)
{
    /* Předpoklad: caller už vstoupil do <fdc_state>. */

    /* connected jen pro běh - INI hodnota se připne. */
    unsigned ini_connected = fdc->connected;
    snapshot_xml_read_uint(r, "connected", &fdc->connected);
    snapshot_config_pin_ini_value(fdc_cfg_section_name(fdc), "connected", ini_connected);

    /* hd_patch / bus_xlate jsou informativní - aktivní hodnoty jsou
     * z cfgfile/CLI, ne ze snapshotu. */

    if (snapshot_xml_enter_element(r, "wd279x")) {
        int ival;
        uint8_t hval8;
        uint16_t hval16;

        if (snapshot_xml_read_hex8(r, "regSTATUS", &hval8)) fdc->wd279x.regSTATUS = hval8;
        if (snapshot_xml_read_hex8(r, "regCOMMAND", &hval8)) fdc->wd279x.regCOMMAND = hval8;
        if (snapshot_xml_read_hex8(r, "regTRACK", &hval8)) fdc->wd279x.regTRACK = hval8;
        if (snapshot_xml_read_hex8(r, "regSECTOR", &hval8)) fdc->wd279x.regSECTOR = hval8;
        if (snapshot_xml_read_hex8(r, "regDATA", &hval8)) fdc->wd279x.regDATA = hval8;

        if (snapshot_xml_read_hex8(r, "MOTOR", &hval8)) fdc->wd279x.MOTOR = hval8;
        if (snapshot_xml_read_hex8(r, "SIDE", &hval8)) fdc->wd279x.SIDE = hval8;
        if (snapshot_xml_read_hex8(r, "DENSITY", &hval8)) fdc->wd279x.DENSITY = hval8;
        if (snapshot_xml_read_hex8(r, "EINT", &hval8)) fdc->wd279x.EINT = hval8;

        if (snapshot_xml_read_hex16(r, "buffer_pos", &hval16)) fdc->wd279x.buffer_pos = hval16;
        if (snapshot_xml_read_hex16(r, "data_counter", &hval16)) fdc->wd279x.data_counter = hval16;
        if (snapshot_xml_read_hex16(r, "current_sector_size", &hval16)) fdc->wd279x.current_sector_size = hval16;

        if (snapshot_xml_read_int(r, "status_mode", &ival)) {
            fdc->wd279x.status_mode = (en_WD279X_STATUS_MODE)ival;
        }

        if (snapshot_xml_read_hex8(r, "multiblock_rw", &hval8)) fdc->wd279x.multiblock_rw = hval8;
        if (snapshot_xml_read_int(r, "direction_latch", &ival)) fdc->wd279x.direction_latch = (int8_t)ival;
        if (snapshot_xml_read_hex8(r, "intrq_active", &hval8)) fdc->wd279x.intrq_active = hval8;
        if (snapshot_xml_read_hex8(r, "reading_status_counter", &hval8)) fdc->wd279x.reading_status_counter = hval8;
        if (snapshot_xml_read_hex8(r, "pending_busy_status", &hval8)) fdc->wd279x.pending_busy_status = hval8;
        if (snapshot_xml_read_hex8(r, "pending_drq", &hval8)) fdc->wd279x.pending_drq = hval8;
        if (snapshot_xml_read_hex8(r, "positioned_sector", &hval8)) fdc->wd279x.positioned_sector = hval8;
        if (snapshot_xml_read_hex8(r, "positioned_track", &hval8)) fdc->wd279x.positioned_track = hval8;
        if (snapshot_xml_read_hex8(r, "positioned_side", &hval8)) fdc->wd279x.positioned_side = hval8;
        if (snapshot_xml_read_hex8(r, "waitForInt", &hval8)) fdc->wd279x.waitForInt = hval8;
        if (snapshot_xml_read_hex8(r, "write_track_stage", &hval8)) fdc->wd279x.write_track_stage = hval8;
        if (snapshot_xml_read_hex16(r, "write_track_counter", &hval16)) fdc->wd279x.write_track_counter = hval16;
        if (snapshot_xml_read_hex8(r, "rt_sectors", &hval8)) fdc->wd279x.rt_sectors = hval8;
        if (snapshot_xml_read_hex16(r, "rt_sector_bytes", &hval16)) fdc->wd279x.rt_sector_bytes = hval16;
        if (snapshot_xml_read_hex8(r, "rt_ssize_code", &hval8)) fdc->wd279x.rt_ssize_code = hval8;
        if (snapshot_xml_read_int(r, "rt_cached_sec_idx", &ival)) fdc->wd279x.rt_cached_sec_idx = (int8_t)ival;
        /* Novější prvky; starší snapshot je nemá -> 0 jako po resetu chipu. */
        fdc->wd279x.pending_rnf_end = 0;
        if (snapshot_xml_read_hex8(r, "pending_rnf_end", &hval8)) fdc->wd279x.pending_rnf_end = hval8;
        memset(fdc->wd279x.rt_sinfo, 0, sizeof(fdc->wd279x.rt_sinfo));
        {
            char *hex = NULL;
            if (snapshot_xml_read_string(r, "rt_sinfo", &hex) && hex) {
                size_t n = strlen(hex) / 2;
                if (n > sizeof(fdc->wd279x.rt_sinfo)) n = sizeof(fdc->wd279x.rt_sinfo);
                for (size_t k = 0; k < n; k++) {
                    char byte[3] = { hex[k * 2], hex[k * 2 + 1], 0 };
                    fdc->wd279x.rt_sinfo[k] = (uint8_t)strtoul(byte, NULL, 16);
                }
            }
            g_free(hex);
        }

        for (int i = 0; i < FDC_NUM_DRIVES; i++) {
            char elem_name[16];
            snprintf(elem_name, sizeof(elem_name), "drive_%d", i);

            if (snapshot_xml_enter_element(r, elem_name)) {
                /* Pole média se čtou vždy (formát se nemění); co s nimi,
                 * rozhoduje to, co mechanika drží (viz popis souboru). */
                st_FDDrive *drv = &fdc->drive[i];
                unsigned snap_mounted = 0;
                int snap_rdo = 0;
                char *fname = NULL;
                const bool have_mounted = snapshot_xml_read_uint(r, "mounted", &snap_mounted);
                const bool have_rdo = snapshot_xml_read_int(r, "readonly", &snap_rdo);
                const bool have_fname = snapshot_xml_read_string(r, "filename", &fname);

                const bool buffer_image = drv->handler_valid && !drv->file_backed;
                if (snapshot_media_is_replay_load() || buffer_image) {
                    /* Záznam vstupů: dosavadní chování beze změny. Ochranu
                     * proti zápisu si mechanika s obrazem ze souboru nechává
                     * podle vloženého média i tady. */
                    if (have_mounted) drv->mounted = snap_mounted;
                    if (have_rdo && !drv->file_backed) drv->readonly = snap_rdo;
                    if (have_fname && fname && fname[0] != '\0') {
                        strncpy(drv->filename, fname, sizeof(drv->filename) - 1);
                        drv->filename[sizeof(drv->filename) - 1] = '\0';
                    }
                } else {
                    /* Vložené médium zůstává; jen zjistit rozdíl pro zprávu. */
                    snap_fdc_media_compare(fdc, (unsigned)i, have_mounted ? snap_mounted : 0,
                                           have_fname ? fname : NULL);
                }
                g_free(fname);

                snapshot_xml_leave_element(r); /* drive_N */
            }
        }

        snapshot_xml_leave_element(r); /* wd279x */
    }

    /* Buffer chipu - load do fdc->wd279x.buffer. */
    if (snapshot_io_entry_exists(ctx->io, buf_entry)) {
        en_SNAPSHOT_RESULT res = snapshot_io_read_bin_into(
            ctx->io, buf_entry,
            fdc->wd279x.buffer, WD279X_BUFFER_SIZE);
        if (res != SNAPSHOT_OK) {
            SNAP_WARN("fdc", "Cannot load FDC buffer (size mismatch or I/O error)");
        }
    }

    return SNAPSHOT_OK;
}


/* ============================================================
 * Top-level dispatch
 * ============================================================ */

static en_SNAPSHOT_RESULT snap_fdc_save(st_SNAPSHOT_CONTEXT *ctx)
{
    /* FDC0 - historické entry devices/fdc.xml (zpětná kompatibilita 2A). */
    en_SNAPSHOT_RESULT res = snap_fdc_save_instance(
        ctx, &g_fdc[FDC0], "devices/fdc.xml", "devices/fdc_buffer.bin");
    if (res != SNAPSHOT_OK) return res;

    /* FDC1 - nové entry devices/fdc1.xml + devices/fdc1_buffer.bin. */
    res = snap_fdc_save_instance(
        ctx, &g_fdc[FDC1], "devices/fdc1.xml", "devices/fdc1_buffer.bin");
    return res;
}


/**
 * @brief Načte stav jedné instance FDC z daného XML entry.
 *
 * Entry je volitelný - když chybí (např. starý snapshot bez FDC1), vrátí
 * OK a instance zůstane v default stavu po init.
 *
 * @param ctx       snapshot kontext.
 * @param fdc       cílová instance FDC.
 * @param xml_entry název XML entry (devices/fdc.xml / devices/fdc1.xml).
 * @param buf_entry název binárního buffer entry.
 * @return SNAPSHOT_OK nebo chyba parse/IO.
 */
static en_SNAPSHOT_RESULT snap_fdc_load_one(st_SNAPSHOT_CONTEXT *ctx, st_FDC *fdc,
                                            const char *xml_entry, const char *buf_entry)
{
    /* FDC entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, xml_entry)) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, xml_entry, &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("fdc", "Cannot load FDC xml entry");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("fdc", "Parse error in FDC xml entry");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "fdc_state")) {
        SNAP_ERR("fdc", "Missing element fdc_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Zpětná kompatibilita: starší snapshoty (před odstraněním OLD impl)
     * měly v <fdc_state> atribut `impl="old|new"`. Nové snapshoty atribut
     * nezapisují (čistší formát). Při loadu:
     *  - chybějící atribut nebo `impl="new"` -> standardní load
     *  - `impl="old"` -> log warning a skip (= staré OLD snapshoty nelze
     *    obnovit, ale neshazujeme load celého snapshotu) */
    char *impl_attr = NULL;
    if (snapshot_xml_read_attr(r, "impl", &impl_attr) && impl_attr) {
        if (strcmp(impl_attr, "old") == 0) {
            SNAP_WARN("fdc",
                "Snapshot FDC impl='old' is no longer supported - skipping FDC restore");
            g_free(impl_attr);
            snapshot_xml_leave_element(r); /* fdc_state */
            snapshot_xml_reader_free(r);
            return SNAPSHOT_OK;
        }
        g_free(impl_attr);
    }

    res = snap_fdc_load_instance(ctx, r, fdc, buf_entry);

    snapshot_xml_leave_element(r); /* fdc_state */
    snapshot_xml_reader_free(r);

    return res;
}


/**
 * @brief Načte stav obou řadičů; vložené diskety zůstávají beze změny.
 *
 * Mechaniky s obrazem ze souboru a prázdné mechaniky si nechávají vložené
 * médium (viz popis souboru). Liší-li se od disket ze snapshotu, připojí
 * řádky do souhrnné zprávy (snapshot_media.h); při načtení snapshotu
 * záznamu vstupů (snapshot_media_set_replay_load()) ne.
 *
 * @param ctx snapshot kontext.
 * @return SNAPSHOT_OK, nebo chyba prvního řadiče, který se nepodařilo
 *         načíst (FDC1 se pak nenačítá).
 * @post I po chybě platí, že do jednoho souboru obrazu smí zapisovat
 *       nejvýš jedna mechanika (fdc_enforce_single_writer()).
 */
static en_SNAPSHOT_RESULT snap_fdc_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* FDC0 - historické entry. */
    en_SNAPSHOT_RESULT res = snap_fdc_load_one(
        ctx, &g_fdc[FDC0], "devices/fdc.xml", "devices/fdc_buffer.bin");

    /* FDC1 - nové entry; starý snapshot ho nemá -> FDC1 zůstane default. */
    if (res == SNAPSHOT_OK) {
        res = snap_fdc_load_one(
            ctx, &g_fdc[FDC1], "devices/fdc1.xml", "devices/fdc1_buffer.bin");
    }

    /* Mechaniky s obrazem ze souboru si `readonly` nechaly podle vloženého
     * média, takže invariant jediné mechaniky se zápisem na soubor (fdc.h)
     * platí dál; volání je pojistka, kdyby ho něco porušilo. */
    fdc_enforce_single_writer();
    return res;
}


void snap_fdc_register(void)
{
    snapshot_register_component("fdc",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_fdc_save,
                                snap_fdc_load,
                                true);
}

#endif /* CFG_HWEXT_HAVE_FDC */
