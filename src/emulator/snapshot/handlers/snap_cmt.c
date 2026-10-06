/**
 * @file snap_cmt.c
 * @brief Snapshot handler: CMT kazeta - uložení a načtení stavu kazetového magnetofonu
 *
 * Ukládá stav transportu (state, paused, playsts, časy), nastavení a jméno
 * posledního souboru do devices/cmt.xml. Obraz pásky se do snapshotu
 * neukládá a při načtení se automaticky neotevírá. Pokud obnovený stav
 * transportu neodpovídá aktuálně vložené pásce (typicky snapshot pořízený
 * během přehrávání načtený v novém procesu bez pásky), loader přepne
 * transport do STOP (cmt_sanitize_state) - jinak by emulace četla data
 * z nevložené pásky.
 *
 * Volby cpu_boost (automatická MAX SPEED během přehrávání), polarity
 * (zadní přepínač polarity čtení), mz_cmtspeed (výchozí rychlost pásky)
 * a mzfsize_check jsou uživatelské preference (cfg sekce [CMT]), ne stav
 * stroje: do snapshotu se dál zapisují (kvůli kompatibilitě se staršími
 * verzemi emulátoru, které je čtou), při načtení se ale ignorují a platí
 * aktuální volby uživatele. Páska se ze snapshotu neotevírá, takže by
 * obnovená polarita ani rychlost neměly na co působit.
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/cmt/cmt.h"


/**
 * @brief Uloží stav CMT (g_cmt) do devices/cmt.xml.
 *
 * @param ctx Kontext snapshotu (otevřený pro zápis).
 * @return SNAPSHOT_OK, nebo chyba zápisu z snapshot_io_write_xml.
 *
 * @pre Emulace je pozastavená.
 */
static en_SNAPSHOT_RESULT snap_cmt_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "cmt_state");

    /* Základní stavové proměnné */
    snapshot_xml_write_int(w, "state", (int)g_cmt.state);
    snapshot_xml_write_int(w, "paused", g_cmt.paused);
    snapshot_xml_write_int(w, "polarity", (int)g_cmt.polarity);
    snapshot_xml_write_int(w, "mz_cmtspeed", (int)g_cmt.mz_cmtspeed);
    snapshot_xml_write_int(w, "output", g_cmt.output);
    /* Stav přehrávání bloku (BODY/PAUSE/STOP). Bez něj by po načtení
     * snapshotu do procesu ve stavu STOP cmt_update_output nedetekoval
     * konec bloku (porovnává nový playsts s uloženým). */
    snapshot_xml_write_int(w, "playsts", (int)g_cmt.playsts);

    /* Časové údaje */
    snapshot_xml_write_uint64(w, "start_time", g_cmt.start_time);
    snapshot_xml_write_uint64(w, "paused_time", g_cmt.paused_time);
    snapshot_xml_write_uint64(w, "recording_last_event", g_cmt.recording_last_event);

    /* Nastavení. cpu_boost a mzfsize_check (a výše polarity, mz_cmtspeed)
     * se zapisují jen kvůli kompatibilitě se staršími verzemi emulátoru,
     * které je při načtení obnovují; snap_cmt_load je ignoruje (uživatelské
     * preference). */
    snapshot_xml_write_int(w, "cpu_boost", (int)g_cmt.cpu_boost);
    snapshot_xml_write_int(w, "mzfsize_check", (int)g_cmt.mzfsize_check);
    snapshot_xml_write_int(w, "recording_to_stream", g_cmt.recording_to_stream);

    /* Název posledního souboru (může být NULL) */
    snapshot_xml_write_string(w, "last_filename",
                              g_cmt.last_filename ? g_cmt.last_filename : "");

    snapshot_xml_close_element(w); /* cmt_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/cmt.xml", xml);
    g_free(xml);

    return res;
}


/**
 * @brief Načte stav CMT z devices/cmt.xml do g_cmt.
 *
 * Chybějící entry není chyba (komponenta je volitelná). Chybějící
 * playsts (starší snapshot) se odvodí ze stavu transportu. Po načtení
 * se volá cmt_sanitize_state(): bez vložené pásky (nebo s páskou, kterou
 * nelze v obnoveném stavu použít) skončí transport ve STOP a vypíše se
 * varování na stderr. Nakonec cmt_cpu_boost_apply() srovná MAX SPEED
 * s obnoveným transportem a aktuální volbou cpu_boost.
 *
 * Elementy cpu_boost, polarity, mz_cmtspeed a mzfsize_check v XML se
 * záměrně ignorují: jsou to uživatelské preference a jejich cfg elementy
 * v sekci [CMT] ukazují přímo na proměnné g_cmt, takže by hodnota ze
 * snapshotu při ukončení emulátoru přepsala i INI.
 *
 * @param ctx Kontext snapshotu (otevřený pro čtení).
 * @return SNAPSHOT_OK, nebo chyba čtení / parsování XML.
 *
 * @pre Emulace je pozastavená.
 * @post Platí invarianty st_CMT (bez pásky je transport ve STOP).
 * @post g_cmt.cpu_boost, polarity, mz_cmtspeed a mzfsize_check mají
 *       stejnou hodnotu jako před voláním.
 * @post Hrající páska (PLAY/RECORD bez pauzy) s cpu_boost -> MAX SPEED
 *       zapnutá; jinak neběží MAX SPEED zapnutá automatikou. MAX SPEED
 *       zvolená uživatelem se nemění.
 */
static en_SNAPSHOT_RESULT snap_cmt_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* CMT entry je volitelný — pokud neexistuje, přeskočíme */
    if (!snapshot_io_entry_exists(ctx->io, "devices/cmt.xml")) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/cmt.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("cmt", "Cannot load devices/cmt.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("cmt", "Parse error in devices/cmt.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "cmt_state")) {
        SNAP_ERR("cmt", "Missing element cmt_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    int ival;

    /* Základní stavové proměnné */
    if (snapshot_xml_read_int(r, "state", &ival)) g_cmt.state = (en_CMT_STATE)ival;
    snapshot_xml_read_int(r, "paused", &g_cmt.paused);
    /* polarity a mz_cmtspeed záměrně nečteme - uživatelské preference. */
    snapshot_xml_read_int(r, "output", &g_cmt.output);

    /* playsts: starší snapshoty ho neobsahují - odvodíme ze stavu. */
    if (snapshot_xml_read_int(r, "playsts", &ival)
        && (ival >= (int)CMTEXT_BLOCK_PLAYSTS_BODY)
        && (ival <= (int)CMTEXT_BLOCK_PLAYSTS_STOP)) {
        g_cmt.playsts = (en_CMTEXT_BLOCK_PLAYSTS)ival;
    } else {
        g_cmt.playsts = (g_cmt.state == CMT_STATE_STOP)
                            ? CMTEXT_BLOCK_PLAYSTS_STOP
                            : CMTEXT_BLOCK_PLAYSTS_BODY;
    }

    /* Časové údaje */
    snapshot_xml_read_uint64(r, "start_time", &g_cmt.start_time);
    snapshot_xml_read_uint64(r, "paused_time", &g_cmt.paused_time);
    snapshot_xml_read_uint64(r, "recording_last_event", &g_cmt.recording_last_event);

    /* Nastavení. Elementy cpu_boost a mzfsize_check záměrně nečteme - jsou
     * to uživatelské preference (cfg [CMT]), ne stav stroje; obnovení by je
     * tiše a trvale přepsalo (i v INI). */
    snapshot_xml_read_int(r, "recording_to_stream", &g_cmt.recording_to_stream);

    /* Název posledního souboru */
    char *fname = NULL;
    if (snapshot_xml_read_string(r, "last_filename", &fname)) {
        if (fname && fname[0] != '\0') {
            SNAP_WARN("cmt", "Cassette reference file '%s' recorded, but will not be auto-opened", fname);
        }
        g_free(fname);
    }

    snapshot_xml_leave_element(r); /* cmt_state */
    snapshot_xml_reader_free(r);

    /* Páska se ze snapshotu neotevírá. Stav PLAY/RECORD bez vložené
     * (nebo bez vhodné) pásky by vedl k práci s nevloženou páskou ->
     * transport přepneme do STOP. Pokud páska v procesu vložená je
     * (načtení ve stejném procesu), stav i pozice zůstávají ze snapshotu. */
    if (cmt_sanitize_state()) {
        SNAP_WARN("cmt", "Tape transport was active in the snapshot, but no matching tape is inserted - transport set to STOP");
    }

    /* Snapshot obnovil stav transportu, rychlost emulace ale ne: hrající
     * páska s cpu_boost by běžela normální rychlostí a MAX SPEED zapnutá
     * boostem před načtením by zůstala viset i po STOP. Srovnává se podle
     * aktuální volby cpu_boost uživatele. Uživatelem zvolenou MAX SPEED
     * cmt_cpu_boost_apply nemění. */
    cmt_cpu_boost_apply();

    return SNAPSHOT_OK;
}


void snap_cmt_register(void)
{
    snapshot_register_component("cmt",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_cmt_save,
                                snap_cmt_load,
                                true);
}
