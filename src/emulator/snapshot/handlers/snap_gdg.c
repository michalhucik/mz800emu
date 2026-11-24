/**
 * @file snap_gdg.c
 * @brief Snapshot handler: GDG — zobrazovací řadič
 *
 * Čekající GDG událost (g_gdg.event) se do snapshotu neukládá - při loadu
 * se dopočítá z obnovené pozice paprsku, viz snap_gdg_event_resync().
 * Na MZ-800 ukládá i registry HW scrollu (g_hwscroll); snapshot bez nich
 * načte HW scroll ve stavu po resetu.
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/video.h"
#include "customspeed.h"
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#include "mzarch/mz800/gdg/mz800_framebuffer.h"
#endif

/**
 * @brief Obnoví termín další 20ms události po načtení snapshotu.
 *
 * 20ms událost (customspeed) uzavírá zvukový log a dokrokovává PSG, na její
 * fázi tedy závisí stav PSG po dalším běhu. Snapshot ji nese v klíči
 * speed_sync_ticks (tiky od začátku snímku); klíč není součástí otisku
 * stavu, aby otisky zůstaly porovnatelné se záznamy starších verzí.
 *
 * Snapshot bez klíče pochází ze starší verze, která termín neukládala ani
 * při načtení neměnila; fáze se tehdy řídila jen dosavadním během procesu
 * (například po návratu ze 150 % na 100 % zůstává termín uprostřed snímku).
 * Stejně se zachová i zde: termín se nemění, takže starší záznam se přehraje
 * ze stejně připraveného procesu jako na původní verzi. Výjimka je proces,
 * ve kterém termín před načtením posunuly snímky zkrácené bitem CKSW: takový
 * posun starší verze neznala. Každý zkrácený snímek odečte od termínu méně
 * než VIDEO_SCREEN_TICKS, termín je tedy větší právě o součet zkrácení
 * dokončených snímků (@p cksw_drift_ticks; zkrácení běžícího snímku se
 * projeví až na jeho konci). Tento součet se od termínu odečte a výsledek
 * se o celé periody srovná do okna jedné periody za časem snímku před
 * načtením; vyjde termín, jaký by měl stejně připravený proces bez
 * zkrácených snímků, včetně fáze posunuté dřívější změnou rychlosti.
 * Výpočet byl ověřen pro periodu ustálenou před prvním zkrácením a dále
 * neměnnou. Po změně periody mezi krátkými snímky nebo po nich součet
 * zkrácení sám neurčuje fázi, kterou by měl proces bez CKSW.
 * Je-li po této korekci termín v obnoveném rastru již splatný, přesune se
 * o celé periody za obnovenou pozici, aby jej čtečka mohla znovu obnovit
 * z nově uloženého snapshotu.
 * Z uložených dat samotných fázi starší verze určit nelze.
 *
 * @param has_saved true = z XML se podařilo přečíst uložený termín.
 * @param saved Uložený termín (platí při @p has_saved).
 * @param cksw_drift_ticks Součet zkrácení dokončených snímků bitem CKSW ve
 *        stavu PŘED načtením (g_gdg.short_ticks_total), 0 = žádné.
 * @param prev_ticks Čas snímku (g_gdg.total_elapsed.ticks) PŘED načtením.
 * @pre g_gdg.total_elapsed.ticks je už obnovený.
 * @post Přečtený termín se obnoví přesně, i když již nastal. Takový stav může
 *       vzniknout přeuložením starého snapshotu před dalším krokem emulace.
 *       Výpočet podle historie přehrávače se použije jen při chybějícím termínu.
 */
static void snap_gdg_restore_speed_sync(bool has_saved, unsigned saved, uint64_t cksw_drift_ticks, unsigned prev_ticks)
{
    if (has_saved)
    {
        g_customspeed.speed_sync_event.ticks = saved;
    }
    else if (cksw_drift_ticks)
    {
        int64_t width = (int64_t)g_customspeed.speed_frame_width_requested;
        if (width <= 0)
        {
            g_customspeed.speed_sync_event.ticks = VIDEO_SCREEN_TICKS;
            return;
        }
        int64_t term = (int64_t)g_customspeed.speed_sync_event.ticks - (int64_t)(cksw_drift_ticks % (uint64_t)width);
        while (term <= (int64_t)prev_ticks)
        {
            term += width;
        }
        while (term > (int64_t)prev_ticks + width)
        {
            term -= width;
        }
        while (term <= (int64_t)g_gdg.total_elapsed.ticks)
        {
            term += width;
        }
        g_customspeed.speed_sync_event.ticks = (unsigned)term;
    }
}

static en_SNAPSHOT_RESULT snap_gdg_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "gdg_state");

    /* Časové razítko */
    snapshot_xml_open_element(w, "total_elapsed");
    snapshot_xml_write_uint(w, "screens", g_gdg.total_elapsed.screens);
    snapshot_xml_write_uint(w, "ticks", g_gdg.total_elapsed.ticks);
    snapshot_xml_close_element(w);

    /* Pozice paprsku */
    snapshot_xml_write_uint(w, "beam_row", g_gdg.beam_row);

    /* Termín další 20ms události (tiky od začátku snímku), viz
     * snap_gdg_restore_speed_sync(). Otisk stavu ho vynechává: starší verze
     * klíč neměly a jejich záznamy vstupů by se jinak při kontrole otisků
     * rozešly; odlišná fáze se v otisku projeví stavem PSG po další události. */
    if (!ctx->for_hash)
    {
        snapshot_xml_write_uint(w, "speed_sync_ticks", g_customspeed.speed_sync_event.ticks);
    }

#if MZARCH == 800
    /* Zkrácení řádků bitem CKSW: délka aktuálního řádku, součet za snímek
     * a za celý běh (souvislý absolutní čas). Bez CKSW je vše nulové
     * a element se nezapisuje, takže snapshot i jeho otisk jsou stejné
     * jako před zavedením těchto polí. */
    if (g_gdg.row_shrink || g_gdg.frame_short_ticks || g_gdg.short_ticks_total)
    {
        snapshot_xml_open_element(w, "cksw_timing");
        snapshot_xml_write_uint(w, "row_shrink", g_gdg.row_shrink);
        snapshot_xml_write_uint(w, "frame_short_ticks", g_gdg.frame_short_ticks);
        snapshot_xml_write_uint64(w, "short_ticks_total", g_gdg.short_ticks_total);
        snapshot_xml_close_element(w);
    }
#endif

    /* Registry */
    snapshot_xml_open_element(w, "registers");
    snapshot_xml_write_uint(w, "regDMD", g_gdg.regDMD);
    snapshot_xml_write_uint(w, "regBOR", g_gdg.regBOR);
#if MZARCH == 800
    snapshot_xml_write_uint(w, "regPALGRP", g_gdg.regPALGRP);
    snapshot_xml_write_uint(w, "regPAL0", g_gdg.regPAL0);
    snapshot_xml_write_uint(w, "regPAL1", g_gdg.regPAL1);
    snapshot_xml_write_uint(w, "regPAL2", g_gdg.regPAL2);
    snapshot_xml_write_uint(w, "regPAL3", g_gdg.regPAL3);
    snapshot_xml_write_uint(w, "cksw", g_gdg.cksw);
#endif
    snapshot_xml_write_uint(w, "regct53g7", g_gdg.regct53g7);
#if MZARCH == 800
    /* Model WAIT u VRAM - uvolnění WRITE FF posledního zápisu. Starší
     * snapshoty bez klíče se při načtení inicializují na 0 (žádný zápis);
     * dřívější klíče vram800_hot_phase_* se už nečtou. */
    snapshot_xml_write_uint64(w, "vram_wrff_release", g_gdg.vram_wrff_release);
#endif
    snapshot_xml_close_element(w); /* registers */

    /* Tempo */
    snapshot_xml_write_uint(w, "tempo", g_gdg.tempo);
    snapshot_xml_write_uint(w, "tempo_divider", g_gdg.tempo_divider);
#if MZARCH == 800
    /* Perioda TEMPO v řádcích (g_gdg.tempo_rows). Klíč není součástí otisku
     * stavu: otisky v záznamech vstupů starších verzí ho neznají. */
    if (!ctx->for_hash)
        snapshot_xml_write_uint(w, "tempo_rows", g_gdg.tempo_rows);

    /* Čítač adresy řádku video fetche (st_GDG.row_addr). Není součástí
     * otisku stavu: otisky v záznamech vstupů starších verzí ho neznají
     * a bez přepnutí DMD uprostřed snímku plyne z DMD a polohy paprsku. */
    if (!ctx->for_hash)
    {
        snapshot_xml_open_element(w, "row_counter");
        snapshot_xml_write_uint(w, "row_addr", g_gdg.row_addr);
        snapshot_xml_write_uint(w, "char_row", g_gdg.char_row);
        snapshot_xml_write_uint(w, "row_addr_next", g_gdg.row_addr_next);
        snapshot_xml_write_uint(w, "char_row_next", g_gdg.char_row_next);
        snapshot_xml_close_element(w);
    }
#endif

    /* Synchronizační signály */
    snapshot_xml_open_element(w, "sync_signals");
    snapshot_xml_write_uint(w, "sts_vsync", g_gdg.sts_vsync);
    snapshot_xml_write_uint(w, "sts_hsync", g_gdg.sts_hsync);
    snapshot_xml_write_uint(w, "hbln", g_gdg.hbln);
    snapshot_xml_write_uint(w, "vbln", g_gdg.vbln);
    snapshot_xml_close_element(w); /* sync_signals */

    /* Stav renderování - průběh vykreslení obrazu pro hostitele, ne stav
     * stroje (posune ho např. překreslení v pauze); hash ho vynechá. */
    if (!ctx->for_hash) {
        snapshot_xml_write_uint(w, "screen_is_already_rendered_at_beam_pos",
                                g_gdg.screen_is_already_rendered_at_beam_pos);
        snapshot_xml_write_uint(w, "screen_need_update_from",
                                g_gdg.screen_need_update_from);
        snapshot_xml_write_uint(w, "last_updated_border_pixel",
                                g_gdg.last_updated_border_pixel);
    }

#if MZARCH == 1500
    /* MZ-1500 specifické: barevná paleta */
    snapshot_xml_open_element(w, "mode1500_colors");
    snapshot_xml_write_int(w, "mode1500_color_0", g_gdg.mode1500_color[0]);
    snapshot_xml_write_int(w, "mode1500_color_1", g_gdg.mode1500_color[1]);
    snapshot_xml_write_int(w, "mode1500_color_2", g_gdg.mode1500_color[2]);
    snapshot_xml_write_int(w, "mode1500_color_3", g_gdg.mode1500_color[3]);
    snapshot_xml_write_int(w, "mode1500_color_4", g_gdg.mode1500_color[4]);
    snapshot_xml_write_int(w, "mode1500_color_5", g_gdg.mode1500_color[5]);
    snapshot_xml_write_int(w, "mode1500_color_6", g_gdg.mode1500_color[6]);
    snapshot_xml_write_int(w, "mode1500_color_7", g_gdg.mode1500_color[7]);
    snapshot_xml_close_element(w); /* mode1500_colors */
#elif MZARCH == 700
    /* MZ-700 specifické: barevná paleta */
    snapshot_xml_open_element(w, "mode700_colors");
    snapshot_xml_write_int(w, "mode700_color_0", g_gdg.mode700_color[0]);
    snapshot_xml_write_int(w, "mode700_color_1", g_gdg.mode700_color[1]);
    snapshot_xml_write_int(w, "mode700_color_2", g_gdg.mode700_color[2]);
    snapshot_xml_write_int(w, "mode700_color_3", g_gdg.mode700_color[3]);
    snapshot_xml_write_int(w, "mode700_color_4", g_gdg.mode700_color[4]);
    snapshot_xml_write_int(w, "mode700_color_5", g_gdg.mode700_color[5]);
    snapshot_xml_write_int(w, "mode700_color_6", g_gdg.mode700_color[6]);
    snapshot_xml_write_int(w, "mode700_color_7", g_gdg.mode700_color[7]);
    snapshot_xml_close_element(w); /* mode700_colors */
#endif

#if MZARCH == 800
    /* HW scroll: registry překládají adresy CPU přístupů do VRAM. */
    snapshot_xml_open_element(w, "hwscroll");
    snapshot_xml_write_uint(w, "regSSA", g_hwscroll.regSSA);
    snapshot_xml_write_uint(w, "regSEA", g_hwscroll.regSEA);
    snapshot_xml_write_uint(w, "regSW", g_hwscroll.regSW);
    snapshot_xml_write_uint(w, "regSOF", g_hwscroll.regSOF);
    snapshot_xml_write_uint(w, "enabled", g_hwscroll.enabled);
    snapshot_xml_close_element(w); /* hwscroll */
#endif

    snapshot_xml_close_element(w); /* gdg_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/gdg.xml", xml);
    g_free(xml);

    return res;
}

/**
 * @brief Dopočítá čekající GDG událost (g_gdg.event) z obnovené pozice paprsku.
 *
 * g_gdg.event je další GDG událost, kterou plánovač (mzarch_main_queue_next_event,
 * gdg_event_set_next) čeká. Snapshot ji neukládá, a bez dopočtu by po loadu
 * zůstala z předchozího stroje: např. AFTER_LAST_SCREEN_PIXEL řádku 103
 * zpracovaná při obnoveném beam_row = 0 čte VRAM mimo rozsah (index
 * (beam_row - 46) * 40 přeteče) a emulátor spadne; jindy se přeskočí nebo
 * posunou události řádku (HBLN, VSYNC, hrana CTC1 z REAL_HSYNC_START).
 *
 * Mezi instrukcemi (jen tam se snapshot ukládá a načítá) mzarch_main_process_events
 * zpracoval všechny události s ticks <= total_elapsed.ticks, takže čekající
 * událost je první aktivní událost aktuálního řádku (v pořadí g_gdgevent[],
 * seřazeném vzestupně podle event_column) s ticks > total_elapsed.ticks -
 * tedy totéž, co by vybral gdg_event_set_next. V konzistentním stavu takovou
 * událost najdeme vždy: poslední událost řádku SCREEN_ROW_END leží na konci
 * řádku. Čas události dává gdg_get_event_ticks(), stejná funkce jako
 * v gdg_event_set_next (na MZ-800 počítá se zkrácením řádků bitem CKSW).
 *
 * Index do g_gdgevent[] je hodnota en_MZEVENT (MZEVENT_GDG_HBLN_END = 0 ..
 * MZEVENT_GDG_SCREEN_ROW_END), stejně jako v gdg_event_set_next.
 *
 * @pre g_gdg.beam_row a g_gdg.total_elapsed.ticks jsou už obnovené ze snapshotu
 *      a g_gdg.beam_row < VIDEO_SCREEN_HEIGHT; na MZ-800 jsou obnovené
 *      i g_gdg.row_shrink a g_gdg.frame_short_ticks.
 * @post g_gdg.event odpovídá obnovenému rastru. Pokud je total_elapsed.ticks
 *       za koncem řádku (nekonzistentní snapshot), nastaví se SCREEN_ROW_END
 *       aktuálního řádku - zpracuje se hned při dalším mzarch_main_process_events
 *       a plánovač se tím sám srovná.
 * @note Volá se na EMU vlákně v pauze (snapshot load), bez dalšího zamykání.
 *       Hlavní událost g_mzarch_main.event obnovuje snap_mzarch; z g_gdg.event
 *       ji plánovač přepočítá při nejbližším mzarch_main_queue_next_event.
 */
static void snap_gdg_event_resync(void)
{
    unsigned row = g_gdg.beam_row;

    for (int name = MZEVENT_GDG_HBLN_END; name <= MZEVENT_GDG_SCREEN_ROW_END; name++)
    {
        const st_GDGEVENT *ev = &g_gdgevent[name];
        unsigned ticks = gdg_get_event_ticks(ev->event_column);
        if (row >= ev->start_row && row < ev->start_row + ev->num_rows &&
            ticks > g_gdg.total_elapsed.ticks)
        {
            g_gdg.event.event_name = (en_MZEVENT)name;
            g_gdg.event.ticks = ticks;
            return;
        }
    }

    /* total_elapsed za koncem řádku - nekonzistentní stav, viz @post */
    g_gdg.event.event_name = MZEVENT_GDG_SCREEN_ROW_END;
    g_gdg.event.ticks = gdg_get_event_ticks(g_gdgevent[MZEVENT_GDG_SCREEN_ROW_END].event_column);
}

static en_SNAPSHOT_RESULT snap_gdg_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Zkrácení řádků bitem CKSW ve stavu před načtením: rozhoduje o termínu
     * 20ms události u snapshotu bez klíče (snap_gdg_restore_speed_sync()). */
#if MZARCH == 800
    uint64_t cksw_drift_ticks = g_gdg.short_ticks_total;
#else
    uint64_t cksw_drift_ticks = 0;
#endif
    unsigned prev_ticks = g_gdg.total_elapsed.ticks;

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/gdg.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("gdg", "Cannot load hw/gdg.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("gdg", "Parse error in hw/gdg.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "gdg_state")) {
        SNAP_ERR("gdg", "Missing element gdg_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Časové razítko */
    if (snapshot_xml_enter_element(r, "total_elapsed")) {
        snapshot_xml_read_uint(r, "screens", &g_gdg.total_elapsed.screens);
        snapshot_xml_read_uint(r, "ticks", &g_gdg.total_elapsed.ticks);
        snapshot_xml_leave_element(r);
    }

    /* Pozice paprsku */
    snapshot_xml_read_uint(r, "beam_row", &g_gdg.beam_row);
    if (g_gdg.beam_row >= VIDEO_SCREEN_HEIGHT) {
        SNAP_ERR("gdg", "beam_row %u out of range", g_gdg.beam_row);
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_CORRUPTED;
    }

    /* Termín další 20ms události (snap_gdg_restore_speed_sync()). */
    unsigned speed_sync = 0;
    bool has_speed_sync = snapshot_xml_read_uint(r, "speed_sync_ticks", &speed_sync);

#if MZARCH == 800
    /* Zkrácení řádků bitem CKSW. Element chybí ve snapshotech bez CKSW
     * i ve starších verzích (ty řádky nezkracovaly): vše nulové. Čtečka XML
     * při chybějícím klíči hodnotu nemění, proto se výchozí hodnoty nastavují
     * předem - jinak by snapshot zdědil stav stroje, přes který se načítá
     * (platí i pro bit cksw v registrech níže). */
    g_gdg.cksw = 0;
    g_gdg.row_shrink = 0;
    g_gdg.frame_short_ticks = 0;
    g_gdg.short_ticks_total = 0;
    if (snapshot_xml_enter_element(r, "cksw_timing")) {
        snapshot_xml_read_uint(r, "row_shrink", &g_gdg.row_shrink);
        snapshot_xml_read_uint(r, "frame_short_ticks", &g_gdg.frame_short_ticks);
        snapshot_xml_read_uint64(r, "short_ticks_total", &g_gdg.short_ticks_total);
        /* Pracovní verze mutanta superimpose (206f2fd2) ukládala termín
         * 20ms události sem. */
        if (!has_speed_sync) {
            has_speed_sync = snapshot_xml_read_uint(r, "speed_sync_ticks", &speed_sync);
        }
        snapshot_xml_leave_element(r);
    }
    if (((g_gdg.row_shrink != 0) && (g_gdg.row_shrink != GDG_CKSW_ROW_SHRINK)) ||
        (g_gdg.frame_short_ticks > g_gdg.beam_row * GDG_CKSW_ROW_SHRINK) ||
        (g_gdg.frame_short_ticks % GDG_CKSW_ROW_SHRINK)) {
        SNAP_ERR("gdg", "cksw_timing out of range (row_shrink %u, frame_short_ticks %u)",
                 g_gdg.row_shrink, g_gdg.frame_short_ticks);
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_CORRUPTED;
    }
#endif

    snap_gdg_restore_speed_sync(has_speed_sync, speed_sync, cksw_drift_ticks, prev_ticks);

    /* Čekající GDG událost patří k obnovenému rastru, ne k předchozímu stroji. */
    snap_gdg_event_resync();

    /* Registry */
    if (snapshot_xml_enter_element(r, "registers")) {
        snapshot_xml_read_uint(r, "regDMD", &g_gdg.regDMD);
        snapshot_xml_read_uint(r, "regBOR", &g_gdg.regBOR);
#if MZARCH == 800
        snapshot_xml_read_uint(r, "regPALGRP", &g_gdg.regPALGRP);
        snapshot_xml_read_uint(r, "regPAL0", &g_gdg.regPAL0);
        snapshot_xml_read_uint(r, "regPAL1", &g_gdg.regPAL1);
        snapshot_xml_read_uint(r, "regPAL2", &g_gdg.regPAL2);
        snapshot_xml_read_uint(r, "regPAL3", &g_gdg.regPAL3);
        /* cksw - novy klic, starsi snapshoty ho nemaji -> default 0
         * (snapshot_xml_read_uint nesnaha pri chybejicim klici). */
        snapshot_xml_read_uint(r, "cksw", &g_gdg.cksw);
        g_gdg.cksw &= 1;
#endif
        snapshot_xml_read_uint(r, "regct53g7", &g_gdg.regct53g7);
#if MZARCH == 800
        /* Model WAIT u VRAM - klíč chybí ve starších snapshotech, pak 0
         * (první přístup do VRAM po načtení se chová jako bez předchozího
         * zápisu). */
        g_gdg.vram_wrff_release = 0;
        snapshot_xml_read_uint64(r, "vram_wrff_release", &g_gdg.vram_wrff_release);
#endif
        snapshot_xml_leave_element(r);
    }

    /* Tempo */
    snapshot_xml_read_uint(r, "tempo", &g_gdg.tempo);
    snapshot_xml_read_uint(r, "tempo_divider", &g_gdg.tempo_divider);
#if MZARCH == 800
    /* Perioda TEMPO: snapshot starší verze klíč nemá a běží dál s tehdejší
     * periodou 229 řádků, aby jeho záznam vstupů dával stejné otisky. */
    g_gdg.tempo_rows = GDG_TEMPO_DIVIDER_ROWS_LEGACY;
    snapshot_xml_read_uint(r, "tempo_rows", &g_gdg.tempo_rows);
    if (g_gdg.tempo_rows != GDG_TEMPO_DIVIDER_ROWS_LEGACY)
        g_gdg.tempo_rows = GDG_TEMPO_DIVIDER_ROWS;
#endif

    /* Synchronizační signály */
    if (snapshot_xml_enter_element(r, "sync_signals")) {
        snapshot_xml_read_uint(r, "sts_vsync", &g_gdg.sts_vsync);
        snapshot_xml_read_uint(r, "sts_hsync", &g_gdg.sts_hsync);
        snapshot_xml_read_uint(r, "hbln", &g_gdg.hbln);
        snapshot_xml_read_uint(r, "vbln", &g_gdg.vbln);
        snapshot_xml_leave_element(r);
    }

    /* Stav renderování */
    snapshot_xml_read_uint(r, "screen_is_already_rendered_at_beam_pos",
                            &g_gdg.screen_is_already_rendered_at_beam_pos);
    snapshot_xml_read_uint(r, "screen_need_update_from",
                            &g_gdg.screen_need_update_from);
    snapshot_xml_read_uint(r, "last_updated_border_pixel",
                            &g_gdg.last_updated_border_pixel);

#if MZARCH == 1500
    /* MZ-1500 specifické: barevná paleta */
    if (snapshot_xml_enter_element(r, "mode1500_colors")) {
        snapshot_xml_read_int(r, "mode1500_color_0", &g_gdg.mode1500_color[0]);
        snapshot_xml_read_int(r, "mode1500_color_1", &g_gdg.mode1500_color[1]);
        snapshot_xml_read_int(r, "mode1500_color_2", &g_gdg.mode1500_color[2]);
        snapshot_xml_read_int(r, "mode1500_color_3", &g_gdg.mode1500_color[3]);
        snapshot_xml_read_int(r, "mode1500_color_4", &g_gdg.mode1500_color[4]);
        snapshot_xml_read_int(r, "mode1500_color_5", &g_gdg.mode1500_color[5]);
        snapshot_xml_read_int(r, "mode1500_color_6", &g_gdg.mode1500_color[6]);
        snapshot_xml_read_int(r, "mode1500_color_7", &g_gdg.mode1500_color[7]);
        snapshot_xml_leave_element(r);
    }
#elif MZARCH == 700
    /* MZ-700 specifické: barevná paleta */
    if (snapshot_xml_enter_element(r, "mode700_colors")) {
        snapshot_xml_read_int(r, "mode700_color_0", &g_gdg.mode700_color[0]);
        snapshot_xml_read_int(r, "mode700_color_1", &g_gdg.mode700_color[1]);
        snapshot_xml_read_int(r, "mode700_color_2", &g_gdg.mode700_color[2]);
        snapshot_xml_read_int(r, "mode700_color_3", &g_gdg.mode700_color[3]);
        snapshot_xml_read_int(r, "mode700_color_4", &g_gdg.mode700_color[4]);
        snapshot_xml_read_int(r, "mode700_color_5", &g_gdg.mode700_color[5]);
        snapshot_xml_read_int(r, "mode700_color_6", &g_gdg.mode700_color[6]);
        snapshot_xml_read_int(r, "mode700_color_7", &g_gdg.mode700_color[7]);
        snapshot_xml_leave_element(r);
    }
#endif

#if MZARCH == 800
    /* HW scroll; snapshot bez něj (starší verze) = stav po resetu. */
    hwscroll_reset();
    if (snapshot_xml_enter_element(r, "hwscroll")) {
        snapshot_xml_read_uint(r, "regSSA", &g_hwscroll.regSSA);
        snapshot_xml_read_uint(r, "regSEA", &g_hwscroll.regSEA);
        snapshot_xml_read_uint(r, "regSW", &g_hwscroll.regSW);
        snapshot_xml_read_uint(r, "regSOF", &g_hwscroll.regSOF);
        snapshot_xml_read_uint(r, "enabled", &g_hwscroll.enabled);
        snapshot_xml_leave_element(r);
    }
#endif

#if MZARCH == 800
    /* Čítač adresy řádku: historii po řádcích odvodí z DMD, stav aktuálního
     * řádku vezme ze snapshotu; starší snapshot bez elementu = ustálený
     * stav podle DMD (gdg_row_counter_rebuild()). */
    gdg_row_counter_rebuild();
    /* Geometrie řádku (režim 640 v okamžiku hran) se neukládá: podle DMD.
     * Stav kreslení vázaný na řádek (část řádku v režimu MZ-700, přepnutí
     * 320 -> 640) patří předchozímu stroji: zapomenout. */
    gdg_geometry_row_start();
    g_gdg.canvas_split_x = 0;
    g_gdg.mz700_skip_row = 0;
    framebuffer_row_state_reset();
    if (snapshot_xml_enter_element(r, "row_counter")) {
        snapshot_xml_read_uint(r, "row_addr", &g_gdg.row_addr);
        snapshot_xml_read_uint(r, "char_row", &g_gdg.char_row);
        snapshot_xml_read_uint(r, "row_addr_next", &g_gdg.row_addr_next);
        snapshot_xml_read_uint(r, "char_row_next", &g_gdg.char_row_next);
        snapshot_xml_leave_element(r);
        g_gdg.char_row &= 0x07;
        g_gdg.char_row_next &= 0x07;
        if ((g_gdg.row_addr >= VIDEO_SCREEN_HEIGHT) || (g_gdg.row_addr_next >= VIDEO_SCREEN_HEIGHT))
        {
            /* Čítač za běhu nepřesáhne počet řádků snímku (nuluje se na
             * prvním řádku canvasu); jiná hodnota je poškozený snapshot
             * a grafika by četla mimo rovinu VRAM: stav podle DMD. */
            gdg_row_counter_rebuild();
        }
        else
        {
            g_gdg.row_addr_of_row[g_gdg.beam_row] = (uint16_t)g_gdg.row_addr;
        };
    }
#endif

    snapshot_xml_leave_element(r); /* gdg_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}

void snap_gdg_register(void)
{
    snapshot_register_component("gdg",
                                SNAPSHOT_PRIORITY_HW_CORE,
                                snap_gdg_save,
                                snap_gdg_load,
                                false);
}
