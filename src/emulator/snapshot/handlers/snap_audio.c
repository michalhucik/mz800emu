/**
 * @file snap_audio.c
 * @brief Snapshot handler: Audio log — fáze PSG a reset po načtení snapshotu
 *
 * Audio log je tranzientní stav (buffer pro 20ms audio frame), jeho data se
 * neukládají. Ukládá se jen fáze krokování PSG (`hw/audio.xml`,
 * `psg_phase_ticks`): PSG se krokuje po PSG_DIVIDER ticích od
 * last_psg_timestamp logu a část periody, která v okamžiku uložení ještě
 * nebyla odkrokovaná, patří ke stavu stroje. Bez ní by po načtení PSG
 * krokoval v mřížce posunuté na okamžik načtení a čítače kanálů by se
 * rozešly s během bez načtení (přehrání záznamu vstupů po seeku přes
 * checkpoint pak hlásilo rozdíl v části `periph`).
 *
 * Při loadu je nutné resetovat timestampy logu, jinak dojde k podtečení
 * unsigned odčítání v audio_log_fill_psg() a nekonečné smyčce. Log se
 * založí v čase (aktuální tiky - fáze), takže další krok PSG přijde ve
 * stejném čase jako bez načtení. Snapshot bez `hw/audio.xml` (starší) má
 * fázi 0 (dřívější chování).
 *
 * Stav kanálu CTC0 (úroveň po hradlování PC0 a příznak "režim 3 s děličem 2")
 * se neukládá; před resetem logu se odvodí ze stavu 8253 a 8255
 * (ctc8253_ctc0_audio_restore()), které se načítají dřív.
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "snapshot/snapshot_io.h"
#include "audio.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/ctc8253/ctc8253.h"


/** @brief Položka archivu s fází PSG. */
#define SNAP_AUDIO_XML "hw/audio.xml"


static en_SNAPSHOT_RESULT snap_audio_save(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Audio log se neukládá — je to odvozený tranzientní stav; jen fáze
     * krokování PSG (viz hlavička souboru). */
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);
    snapshot_xml_open_element(w, "audio_state");
    snapshot_xml_write_uint64(w, "psg_phase_ticks",
                              audio_psg_phase_ticks(gdg_get_total_ticks()));
    snapshot_xml_close_element(w);

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, SNAP_AUDIO_XML, xml);
    g_free(xml);
    return res;
}


/**
 * @brief Přečte fázi PSG ze snapshotu.
 * @param ctx Kontext načítání.
 * @return Fáze v ticích GDG; 0 bez položky nebo při chybě (starší snapshot).
 */
static uint64_t snap_audio_read_phase(st_SNAPSHOT_CONTEXT *ctx)
{
    uint64_t phase = 0;
    if (!snapshot_io_entry_exists(ctx->io, SNAP_AUDIO_XML)) return 0;

    char *xml = NULL;
    if (snapshot_io_read_xml(ctx->io, SNAP_AUDIO_XML, &xml) != SNAPSHOT_OK) return 0;
    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);
    if (!r) return 0;
    if (snapshot_xml_enter_element(r, "audio_state")) {
        snapshot_xml_read_uint64(r, "psg_phase_ticks", &phase);
        snapshot_xml_leave_element(r);
    }
    snapshot_xml_reader_free(r);
    return phase;
}


static en_SNAPSHOT_RESULT snap_audio_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Resetovat audio log na aktuální GDG timestamp (už načtený ze snapshotu)
     * posunutý o neodkrokovanou fázi PSG. */
    uint64_t current_ticks = gdg_get_total_ticks();
    uint64_t phase = snap_audio_read_phase(ctx);
    if (phase > current_ticks) phase = current_ticks;
    /* stav kanálu CTC0 podle načteného 8253 a PC0; reset logu podle něj ustálí vazbu */
    ctc8253_ctc0_audio_restore();
    audio_reset_log(current_ticks - phase);

    fprintf(stderr, "Snapshot: audio log reset (ticks=%llu, psg phase=%llu)\n",
            (unsigned long long)current_ticks, (unsigned long long)phase);

    return SNAPSHOT_OK;
}


void snap_audio_register(void)
{
    /* Registrujeme s prioritou DEVICE — po všech I/O handlerech (GDG, PSG, CTC)
     * aby g_gdg.total_elapsed byl už načtený ze snapshotu */
    snapshot_register_component("audio",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_audio_save,
                                snap_audio_load,
                                false);
}
