/**
 * @file snap_audio.c
 * @brief Snapshot handler: Audio log — reset po načtení snapshotu
 *
 * Audio log je tranzientní stav (buffer pro 20ms audio frame), nemá vlastní
 * data k uložení. Při loadu je ale nutné resetovat timestampy, jinak dojde
 * k podtečení unsigned odčítání v audio_log_fill_psg() a nekonečné smyčce.
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "audio.h"
#include "hw-generic/gdg/gdg_state.h"


static en_SNAPSHOT_RESULT snap_audio_save(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Audio log se neukládá — je to odvozený tranzientní stav */
    (void)ctx;
    return SNAPSHOT_OK;
}


static en_SNAPSHOT_RESULT snap_audio_load(st_SNAPSHOT_CONTEXT *ctx)
{
    (void)ctx;

    /* Resetovat audio log na aktuální GDG timestamp (už načtený ze snapshotu) */
    uint64_t current_ticks = gdg_get_total_ticks();
    audio_reset_log(current_ticks);

    fprintf(stderr, "Snapshot: audio log reset (ticks=%llu)\n",
            (unsigned long long)current_ticks);

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
