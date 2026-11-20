/**
 * @file   overlay_view.cpp
 * @brief  Plnění kopie stavu pro overlay menu (st_OVERLAY_VIEW) z jádra a testovací stavy.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <algorithm>

#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#include "i18n.h"
#include "emulator/emulator.h"
#include "emulator/customspeed.h"
#include "emulator/display.h"
#include "iface/iface_video.h"
#include "hw-generic/memory/rom.h"
#include "emulator/replay.h"
#include "emulator/recent_media/recent_media.h"
#include "mzarch/mzarch_config.h"

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#endif
#if CFG_HWEXT_HAVE_QDISK
#include "hw-generic/qdisk/qdisk.h"
#endif

#include "audio.h"
#include "iface/iface_audio.h"
#include "iface/iface_joy.h"
#include "hw-generic/joy/joy.h"
#if HAVE_PSG >= 1
#include "hw-generic/psg/psg.h"
#endif
#include "emulator/screenshot/screenshot_series.h"
#include "emulator/videorec/videorec.h"

#include "ui-imgui/cmt/cmt_view.h"
#include "ui-imgui/videorec/videorec_menu.h"
#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_view.h"

/**
 * @brief Název emulovaného stroje pro stavový štítek (podle MZARCH buildu).
 *
 * Název stroje je vlastní jméno, nepřekládá se.
 */
#if MZARCH == 700
#define OVERLAY_VIEW_PLATFORM "MZ-700"
#elif MZARCH == 1500
#define OVERLAY_VIEW_PLATFORM "MZ-1500"
#else
#define OVERLAY_VIEW_PLATFORM "MZ-800"
#endif

/** @brief Bit "motor běží" registru 0DCh řadiče disket (st_WD279X::MOTOR, hodnota bez inverze sběrnice). */
#define OVERLAY_VIEW_FDC_MOTOR_ON 0x80
/** @brief Maska čísla vybrané mechaniky v registru 0DCh řadiče disket. */
#define OVERLAY_VIEW_FDC_DRIVE_MASK 0x03

/** @brief Nejkratší doba mezi výsledkem jednoho zjišťování existence souborů historie médií a zadáním dalšího, v mikrosekundách. */
#define OVERLAY_VIEW_RECENT_CHECK_US G_USEC_PER_SEC
/** @brief Současný čas testovacích stavů (st_OVERLAY_VIEW::recent_now); pevný, aby stáří položek na snímcích nezáviselo na dni běhu. */
#define OVERLAY_VIEW_FIXTURE_NOW 1800000000ll

/* pořadí voleb zařízení joysticku a stavů replay v kopii je stejné jako v jádře (plnění hodnoty přetypovává) */
static_assert((int)JOY_TYPE_NONE == (int)OVERLAY_JOY_NONE && (int)JOY_TYPE_NUM_KEYPAD == (int)OVERLAY_JOY_NUM_KEYPAD && (int)JOY_TYPE_JOYSTICK == (int)OVERLAY_JOY_JOYSTICK,
              "overlay joystick types must match en_JOY_TYPE");
static_assert(AUDIO_SRC_CHANNELS_COUNT <= OVERLAY_VIEW_AUDIO_CHANNELS_MAX, "overlay view must hold every audio channel");
static_assert(OVERLAY_VIEW_MASTER_GAIN_MAX == AUDIO_MASTER_GAIN_MAX, "overlay view master volume range must match the audio interface");

/* pořadí druhů historie jádra a kopie je stejné (plnění druh přetypovává) */
static_assert((int)RECENT_MEDIA_DSK == (int)OVERLAY_MEDIA_DSK && (int)RECENT_MEDIA_CMT == (int)OVERLAY_MEDIA_CMT && (int)RECENT_MEDIA_QD == (int)OVERLAY_MEDIA_QD
                  && (int)RECENT_MEDIA_KIND_COUNT == (int)OVERLAY_MEDIA_KIND_COUNT && RECENT_MEDIA_MAX <= OVERLAY_VIEW_RECENT_PER_KIND,
              "overlay media kinds must match recent_media kinds");

/**
 * @brief Mezipaměť nedávných médií: kopie historie jádra seřazená pro st_OVERLAY_VIEW::recent.
 *
 * Z jádra se plní jen při změně čísla stavu historie (recent_media_serial()).
 * Stav existence souborů (checked, missing, is_dir) do ní zapisuje UI
 * vlákno z výsledku pracovního vlákna (s_probe). Jen UI vlákno.
 *
 * Invarianty: count <= OVERLAY_VIEW_RECENT_MAX; při !valid se obsah nepoužívá.
 */
static struct {
    st_OVERLAY_VIEW_RECENT items[OVERLAY_VIEW_RECENT_MAX]; /**< položky od naposledy vložené */
    unsigned count;                                        /**< počet platných položek */
    unsigned serial;                                       /**< číslo stavu historie, ze kterého položky jsou */
    bool valid;                                            /**< položky byly aspoň jednou naplněné */
    bool recheck;                                          /**< další kolo zjišťování existence se má zadat bez čekání na interval */
    gint64 checked_us;                                     /**< čas vyzvednutí posledního výsledku zjišťování (g_get_monotonic_time()) */
} s_recent;

/** @brief Stav cesty zjištěný pracovním vláknem (st_RECENT_PROBE_JOB::state). */
enum {
    RECENT_PROBE_MISSING = 0, /**< cesta neexistuje */
    RECENT_PROBE_FILE,        /**< cesta existuje a není adresář */
    RECENT_PROBE_DIR,         /**< cesta je adresář */
};

/** @brief Stav výsledku jedné cesty v kole (st_RECENT_PROBE_JOB::ready). */
enum {
    RECENT_PROBE_WAITING = 0, /**< pracovní vlákno se na cestu ještě nezeptalo nebo odpověď nemá */
    RECENT_PROBE_READY,       /**< výsledek je zapsaný a čeká na vyzvednutí UI vláknem */
    RECENT_PROBE_TAKEN,       /**< UI vlákno výsledek vyzvedlo */
};

/**
 * @brief Jedno kolo zjišťování existence souborů: zadání a výsledky.
 *
 * Vlastnictví: vytvoří ho UI vlákno, zapíše do s_probe.job a předá
 * pracovnímu vláknu. Pracovní vlákno výsledek každé cesty zveřejní hned
 * (state, time_us a ready pod s_probe.mutex), takže cesta, na které se
 * dotaz zablokuje, nezdrží výsledky cest před ní. Uvolní ho UI vlákno, až
 * pracovní vlákno skončí a všechny výsledky jsou vyzvednuté.
 * Invarianty: count <= OVERLAY_VIEW_RECENT_MAX; paths[0 .. count-1] jsou
 * vlastní kopie cest a po vytvoření se nemění (smí je číst obě vlákna bez
 * zámku); state, time_us a ready chrání s_probe.mutex.
 */
typedef struct st_RECENT_PROBE_JOB {
    unsigned count;                               /**< počet cest */
    char *paths[OVERLAY_VIEW_RECENT_MAX];         /**< kopie cest (g_strdup) */
    unsigned char state[OVERLAY_VIEW_RECENT_MAX]; /**< výsledek: RECENT_PROBE_MISSING / _FILE / _DIR; platí při ready != RECENT_PROBE_WAITING */
    unsigned char ready[OVERLAY_VIEW_RECENT_MAX]; /**< RECENT_PROBE_WAITING / _READY / _TAKEN */
    gint64 time_us[OVERLAY_VIEW_RECENT_MAX];      /**< čas výsledku (g_get_monotonic_time()) */
#ifdef MZTEST_HEADLESS
    overlay_view_exists_fn fn;                    /**< náhradní zjišťování pro testy; NULL = dotaz na disk */
#endif
} st_RECENT_PROBE_JOB;

/**
 * @brief Stav sdílený s pracovním vláknem zjišťování existence souborů.
 *
 * Statická data: žijí do konce procesu, pracovní vlákno na ně smí sáhnout
 * kdykoli. Invariant: running == true <=> právě jedno pracovní vlákno drží
 * kolo job; job je NULL, nebo kolo, které běží nebo čeká na vyzvednutí
 * posledních výsledků. Pole running a job a výsledky kola chrání mutex.
 */
static struct {
    GMutex mutex;             /**< zámek polí running a job a výsledků kola (listový, nic dalšího se pod ním nebere) */
    bool running;             /**< pracovní vlákno běží */
    st_RECENT_PROBE_JOB *job; /**< současné kolo; NULL = žádné (ukazatel mění jen UI vlákno) */
} s_probe;

#ifdef MZTEST_HEADLESS
/** @brief Náhradní zjišťování existence pro testy (overlay_view_test_set_exists_fn()); jen UI vlákno, do kola se kopíruje. */
static overlay_view_exists_fn s_probe_test_fn = NULL;
#endif

/**
 * @brief Kopie okna Virtual CMT, do které se plní stav kazety pro overlay.
 *
 * Drží se mezi snímky: cmt_view_fill() sestavuje seznam bloků znovu jen
 * při změně pásky. Jen UI vlákno.
 */
static st_CMT_VIEW s_cmt_view;

void overlay_view_copy_text(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t len = strlen(src);
    if (len > size - 1) len = size - 1;
    /* platná část: neplatné UTF-8 i znak rozdělený zkrácením končí před prvním vadným bajtem */
    const gchar *end = NULL;
    g_utf8_validate(src, (gssize)len, &end);
    len = (size_t)(end - src);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/**
 * @brief Zkopíruje do pole jméno souboru bez cesty.
 * @param dst  Cíl (ukončí se nulou).
 * @param size Velikost cíle.
 * @param path Cesta; NULL nebo prázdná = prázdné jméno.
 */
static void overlay_view_copy_basename(char *dst, size_t size, const char *path)
{
    if (!path || !*path) {
        overlay_view_copy_text(dst, size, "");
        return;
    }
    gchar *base = g_path_get_basename(path);
    overlay_view_copy_text(dst, size, base);
    g_free(base);
}

void overlay_view_reset(st_OVERLAY_VIEW *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->speed_percent = 100;
    v->speed_mode = OVERLAY_SPEED_NORMAL;
    for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
        v->fdd[i].shared_with = -1;
        v->fdd[i].wp_blocker = -1;
    }
    v->cmt.state = OVERLAY_CMT_STOP;
    v->cmt.current_block = -1;
    v->cmt.elapsed_s = -1.0;
    v->cmt.total_s = -1.0;
    v->cmt.mode = OVERLAY_CMT_MODE_TAPE;
    v->cmt.current_has_mzf = true;
    v->cmt.first_playable = -1;
    v->display.startup_size = (int)DISPLAY_DEFAULT_STARTUP_SIZE;
    v->display.color_schema = (int)DISPLAY_DEFAULT_COLOR_SCHEMA;
    v->display.framerate_mode = (int)DISPLAY_DEFAULT_FRAMERATE_MODE;
    v->display.scanlines_intensity = DISPLAY_SCANLINES_INTENSITY_DEFAULT;
    v->display.custom_fps = DISPLAY_CUSTOM_FPS_DEFAULT;
    v->display.slave_hz = 50;
    v->sound.master_gain = 1.0f;
    v->sound.channel_count = 1;
    for (int i = 0; i < OVERLAY_VIEW_AUDIO_CHANNELS_MAX; i++) v->sound.channel_gain[i] = 1.0f;
}

/**
 * @brief Přeložený důvod zákazu karet médií při aktivním záznamu nebo přehrávání vstupů.
 * @param v Kopie (zapisuje se media_disabled_reason).
 */
static void overlay_view_set_replay_reason(st_OVERLAY_VIEW *v)
{
    overlay_view_copy_text(v->media_disabled_reason, sizeof(v->media_disabled_reason), _("Not available while a replay is active."));
}

/**
 * @brief Vyplní stav mechanik řadiče FDC0.
 * @param v Kopie po overlay_view_reset().
 * @pre Volající drží device_state_lock().
 */
static void overlay_view_fill_fdd(st_OVERLAY_VIEW *v)
{
#if CFG_HWEXT_HAVE_FDC
    st_FDC *fdc = &g_fdc[FDC0];
    const bool connected = (fdc->connected == FDC_CONNECTED);
    /* jeden bajt registru 0DCh: vybraná mechanika a motor; čtení bez zásahu do emulace */
    const uint8_t motor = fdc->wd279x.MOTOR;
    for (unsigned i = 0; i < OVERLAY_VIEW_FDD_COUNT && i < FDC_NUM_DRIVES; i++) {
        st_OVERLAY_VIEW_FDD *d = &v->fdd[i];
        d->present = true;
        d->connected = connected;
        d->mounted = FDC_TEST_DRIVE_ID_MOUNTED(fdc, i) != 0;
        d->active = connected && (motor & OVERLAY_VIEW_FDC_MOTOR_ON) && (unsigned)(motor & OVERLAY_VIEW_FDC_DRIVE_MASK) == i;
        /* volba platí pro mechaniku, i když je prázdná (položka "Read-only" klasického menu) */
        d->user_readonly = fdc_cfg_get_readonly(fdc, i) != 0;
        if (!d->mounted) continue;
        const char *path = fdc_get_dsk_filepath(fdc, i);
        overlay_view_copy_text(d->path, sizeof(d->path), path);
        overlay_view_copy_basename(d->name, sizeof(d->name), path);
        d->readonly = fdc->drive[i].readonly != 0;
        d->fs_readonly = fdc_drive_fs_readonly(fdc, i) != 0;
        /* tentýž soubor ve více mechanikách (fdc.h): porovnání uložených identit, bez přístupu na disk */
        d->shared_readonly = fdc_drive_shared_readonly(fdc, i) != 0;
        d->shared_with = fdc_drive_shared_with(fdc, i);
        d->wp_blocker = fdc_drive_write_blocker(fdc, i);
        /* podmínka položky "Sync now" klasického menu (menu_fdcontroller.cpp) */
        const char *mode = fdc_cfg_get_storage_mode(fdc, i);
        d->unsaved = mode && strcmp(mode, "cached") == 0 && fdc_drive_has_unsaved_changes(fdc, i) != 0;
        if (fdc->drive[i].geometry_valid) d->size_kb = (unsigned)((fdc->drive[i].geometry.total_data_bytes + 512u) / 1024u);
    }
#else
    (void)v;
#endif
}

/**
 * @brief Vyplní stav Quick Disku.
 * @param v Kopie po overlay_view_reset().
 * @pre Volající drží device_state_lock().
 */
static void overlay_view_fill_qd(st_OVERLAY_VIEW *v)
{
#if CFG_HWEXT_HAVE_QDISK
    st_OVERLAY_VIEW_QD *q = &v->qd;
    q->present = true;
    q->connected = QDISK_TEST_CONNECTED;
    q->unicard = QDISK_TEST_TYPE_UNICARD;
    q->directory = QDISK_TEST_TYPE_VIRTUAL;
    q->write_protected = qdisc_get_write_protected() != 0;
    /* vložené médium určuje nastavená cesta, jako v klasickém menu (menu_qdisk.cpp) */
    const char *path = NULL;
    if (QDISK_TEST_TYPE_IMAGE && QDISK_TEST_STD_PATH_IS_SET) path = qdisc_get_ui_std_filepath();
    else if (QDISK_TEST_TYPE_VIRTUAL && QDISK_TEST_VIRT_PATH_IS_SET) path = qdisc_get_ui_virt_filepath();
    if (!path) return;
    q->mounted = true;
    overlay_view_copy_text(q->path, sizeof(q->path), path);
    overlay_view_copy_basename(q->name, sizeof(q->name), path);
    q->unsaved = qdisk_drive_has_unsaved_changes() != 0;
#else
    (void)v;
#endif
}

/**
 * @brief Vyplní stav kazety z kopie okna Virtual CMT.
 * @param v Kopie po overlay_view_reset().
 * @pre Volající drží device_state_lock() (podmínka cmt_view_fill()).
 */
static void overlay_view_fill_cmt(st_OVERLAY_VIEW *v)
{
    cmt_view_fill(&s_cmt_view);
    const st_CMT_VIEW *c = &s_cmt_view;
    st_OVERLAY_VIEW_CMT *o = &v->cmt;

    o->has_tape = c->has_tape;
    overlay_view_copy_text(o->name, sizeof(o->name), c->tape_name.c_str());
    overlay_view_copy_text(o->path, sizeof(o->path), c->tape_path.c_str());
    o->modified = c->modified;
    o->state = (c->state == CMT_VIEW_STATE_PLAY) ? OVERLAY_CMT_PLAY : ((c->state == CMT_VIEW_STATE_RECORD) ? OVERLAY_CMT_RECORD : OVERLAY_CMT_STOP);
    o->paused = c->paused;
    o->user_paused = c->user_paused;
    o->block_count = c->block_count;
    o->current_block = c->current_block;
    o->elapsed_s = c->tape_elapsed_s;
    o->total_s = c->tape_total_s;
    double progress = c->block_progress;
    if (c->tape_total_s > 0.0 && c->tape_elapsed_s >= 0.0) progress = c->tape_elapsed_s / c->tape_total_s;
    o->progress = (progress < 0.0) ? 0.0 : ((progress > 1.0) ? 1.0 : progress);
    o->mode = (c->mode == CMT_VIEW_MODE_DIRECT) ? OVERLAY_CMT_MODE_DIRECT : ((c->mode == CMT_VIEW_MODE_BOOST) ? OVERLAY_CMT_MODE_BOOST : OVERLAY_CMT_MODE_TAPE);
    o->direct_supported = c->direct_supported;
    o->tape_serial = c->tape_serial;
    o->replay_recording = c->replay_recording;
    o->replay_playing = c->replay_playing;

    o->first_playable = -1;
    for (size_t i = 0; i < c->blocks.size(); i++) {
        if (c->blocks[i].playable) {
            o->first_playable = (int)i;
            break;
        }
    }
    o->current_has_mzf = true;
    if (c->current_block >= 0 && (size_t)c->current_block < c->blocks.size()) o->current_has_mzf = c->blocks[(size_t)c->current_block].has_mzf;
}

/**
 * @brief Je položka a novější než položka b (řazení nedávných médií od naposledy vložené)?
 * @param a První položka.
 * @param b Druhá položka.
 */
static bool overlay_view_recent_newer(const st_OVERLAY_VIEW_RECENT &a, const st_OVERLAY_VIEW_RECENT &b)
{
    return a.time > b.time;
}

/**
 * @brief Seřadí položky nedávných médií od naposledy vložené; položky se stejným časem zůstanou v daném pořadí.
 * @param items Položky.
 * @param count Počet položek.
 */
static void overlay_view_recent_sort(st_OVERLAY_VIEW_RECENT *items, unsigned count)
{
    std::stable_sort(items, items + count, overlay_view_recent_newer);
}

void overlay_view_recent_recheck(void)
{
    s_recent.recheck = true;
}

/**
 * @brief Uvolní kolo zjišťování existence včetně kopií cest.
 * @param job Kolo; NULL se ignoruje.
 */
static void overlay_view_probe_free(st_RECENT_PROBE_JOB *job)
{
    if (!job) return;
    for (unsigned i = 0; i < job->count; i++) g_free(job->paths[i]);
    g_free(job);
}

/**
 * @brief Zjistí stav jedné cesty jedním dotazem na disk.
 * @param job  Kolo (v testech nese náhradní zjišťování).
 * @param path Cesta.
 * @return RECENT_PROBE_MISSING / _FILE / _DIR.
 * @note Volá pracovní vlákno; dotaz se smí zablokovat.
 */
static unsigned char overlay_view_probe_path(const st_RECENT_PROBE_JOB *job, const char *path)
{
#ifdef MZTEST_HEADLESS
    if (job->fn) {
        const en_OVERLAY_VIEW_PATH_STATE st = job->fn(path);
        return (st == OVERLAY_VIEW_PATH_DIR) ? RECENT_PROBE_DIR : ((st == OVERLAY_VIEW_PATH_FILE) ? RECENT_PROBE_FILE : RECENT_PROBE_MISSING);
    }
#else
    (void)job;
#endif
    GStatBuf st;
    if (g_stat(path, &st) != 0) return RECENT_PROBE_MISSING;
    return S_ISDIR(st.st_mode) ? RECENT_PROBE_DIR : RECENT_PROBE_FILE;
}

/**
 * @brief Pracovní vlákno: zjistí existenci cest jednoho kola a každý výsledek hned zveřejní.
 *
 * Sahá jen na své kolo a na statický s_probe. Dotaz na disk se smí
 * zablokovat na libovolně dlouho (nedostupná síťová cesta); UI vlákno na
 * něj nečeká a výsledky cest před zablokovanou už má (review O6 Z1).
 *
 * @param data Kolo (st_RECENT_PROBE_JOB) zapsané v s_probe.job; uvolní ho UI vlákno po skončení vlákna.
 * @return NULL.
 */
static gpointer overlay_view_probe_thread(gpointer data)
{
    st_RECENT_PROBE_JOB *job = (st_RECENT_PROBE_JOB *)data;
    for (unsigned i = 0; i < job->count; i++) {
        const unsigned char st = overlay_view_probe_path(job, job->paths[i]);
        g_mutex_lock(&s_probe.mutex);
        job->state[i] = st;
        job->time_us[i] = g_get_monotonic_time();
        job->ready[i] = RECENT_PROBE_READY;
        g_mutex_unlock(&s_probe.mutex);
    }
    g_mutex_lock(&s_probe.mutex);
    s_probe.running = false;
    g_mutex_unlock(&s_probe.mutex);
    return NULL;
}

bool overlay_view_recent_busy(void)
{
    g_mutex_lock(&s_probe.mutex);
    const bool busy = s_probe.running || s_probe.job != NULL;
    g_mutex_unlock(&s_probe.mutex);
    return busy;
}

#ifdef MZTEST_HEADLESS
void overlay_view_test_set_exists_fn(overlay_view_exists_fn fn)
{
    s_probe_test_fn = fn;
}
#endif

/**
 * @brief Vyzvedne dosud zveřejněné výsledky pracovního vlákna a zapíše je do mezipaměti (páruje podle cesty).
 *
 * Výsledky bere i z kola, které ještě běží. Skončené kolo uvolní a jeho
 * konec zapíše jako čas posledního zjišťování.
 *
 * @return true = pracovní vlákno ještě běží (další kolo zadat nejde).
 * @pre UI vlákno.
 * @post Při návratu false je s_probe.job NULL.
 */
static bool overlay_view_probe_collect(void)
{
    unsigned char state[OVERLAY_VIEW_RECENT_MAX];
    gint64 time_us[OVERLAY_VIEW_RECENT_MAX];
    bool fresh[OVERLAY_VIEW_RECENT_MAX];
    g_mutex_lock(&s_probe.mutex);
    st_RECENT_PROBE_JOB *job = s_probe.job;
    const bool running = s_probe.running;
    const unsigned count = job ? job->count : 0;
    for (unsigned j = 0; j < count; j++) {
        fresh[j] = job->ready[j] == RECENT_PROBE_READY;
        if (!fresh[j]) continue;
        state[j] = job->state[j];
        time_us[j] = job->time_us[j];
        job->ready[j] = RECENT_PROBE_TAKEN;
    }
    if (!running) s_probe.job = NULL;
    g_mutex_unlock(&s_probe.mutex);
    if (!job) return running;

    /* cesty kola se po vytvoření nemění a kolo uvolňuje jen toto vlákno: číst je jde bez zámku */
    for (unsigned j = 0; j < count; j++) {
        if (!fresh[j]) continue;
        for (unsigned i = 0; i < s_recent.count; i++) {
            st_OVERLAY_VIEW_RECENT *r = &s_recent.items[i];
            if (strcmp(r->path, job->paths[j]) != 0) continue;
            r->checked = true;
            r->is_dir = state[j] == RECENT_PROBE_DIR;
            r->missing = state[j] == RECENT_PROBE_MISSING;
            r->checked_us = (long long)time_us[j];
            break;
        }
    }
    if (!running) {
        overlay_view_probe_free(job);
        s_recent.checked_us = g_get_monotonic_time();
    }
    return running;
}

/**
 * @brief Zadá pracovnímu vláknu kolo zjišťování existence všech položek mezipaměti.
 *
 * Pořadí cest v kole: nejdřív položky, jejichž stav už je známý (obnoví
 * se rychle), potom položky s neznámým stavem. Cesta, na které se dotaz
 * blokuje (stav nikdy nezjištěn), je tak v dalších kolech poslední a
 * ostatní položky nezdržuje.
 *
 * Když vlákno nejde vytvořit, kolo se zahodí a zkusí se znovu při dalším
 * plnění.
 *
 * @pre UI vlákno; žádné pracovní vlákno neběží (overlay_view_probe_collect() vrátilo false, s_probe.job je NULL).
 */
static void overlay_view_probe_start(void)
{
    if (s_recent.count == 0) return;
    st_RECENT_PROBE_JOB *job = g_new0(st_RECENT_PROBE_JOB, 1);
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < s_recent.count; i++)
            if (s_recent.items[i].checked == (pass == 0)) job->paths[job->count++] = g_strdup(s_recent.items[i].path);
    }
#ifdef MZTEST_HEADLESS
    job->fn = s_probe_test_fn;
#endif
    g_mutex_lock(&s_probe.mutex);
    s_probe.running = true;
    s_probe.job = job;
    g_mutex_unlock(&s_probe.mutex);
    GThread *t = g_thread_try_new("ovl-recent-probe", overlay_view_probe_thread, job, NULL);
    if (!t) {
        g_mutex_lock(&s_probe.mutex);
        s_probe.running = false;
        s_probe.job = NULL;
        g_mutex_unlock(&s_probe.mutex);
        overlay_view_probe_free(job);
        return;
    }
    /* odpojené vlákno: nikdo na ně nečeká, ani ukončení aplikace */
    g_thread_unref(t);
}

void overlay_view_fill_recent(st_OVERLAY_VIEW *v, bool visible)
{
    if (!v) return;
    /* číslo stavu před čtením: změna během čtení se pozná při příštím plnění */
    const unsigned serial = recent_media_serial();
    if (!s_recent.valid || serial != s_recent.serial) {
        /* dosavadní položky: známý stav existence se přenese na stejné cesty */
        static st_OVERLAY_VIEW_RECENT old[OVERLAY_VIEW_RECENT_MAX];
        const unsigned old_count = s_recent.valid ? s_recent.count : 0;
        if (old_count > 0) memcpy(old, s_recent.items, sizeof(old[0]) * old_count);
        unsigned n = 0;
        for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT; k++) {
            for (unsigned i = 0; i < RECENT_MEDIA_MAX && n < OVERLAY_VIEW_RECENT_MAX; i++) {
                gint64 t = 0;
                char *path = recent_media_dup((en_RECENT_MEDIA_KIND)k, i, &t);
                if (!path) break;
                /* cesta, která se do kopie nevejde celá, by nešla vložit: vynechat */
                if (strlen(path) < OVERLAY_VIEW_PATH_SIZE && g_utf8_validate(path, -1, NULL)) {
                    st_OVERLAY_VIEW_RECENT *r = &s_recent.items[n++];
                    memset(r, 0, sizeof(*r));
                    r->kind = (en_OVERLAY_MEDIA_KIND)k;
                    overlay_view_copy_text(r->path, sizeof(r->path), path);
                    overlay_view_copy_basename(r->name, sizeof(r->name), path);
                    r->time = (long long)t;
                    for (unsigned o = 0; o < old_count; o++) {
                        if (!old[o].checked || strcmp(old[o].path, r->path) != 0) continue;
                        r->checked = true;
                        r->missing = old[o].missing;
                        r->is_dir = old[o].is_dir;
                        r->checked_us = old[o].checked_us;
                        break;
                    }
                }
                g_free(path);
            }
        }
        overlay_view_recent_sort(s_recent.items, n);
        s_recent.count = n;
        s_recent.serial = serial;
        s_recent.valid = true;
    }
    /* výsledek pracovního vlákna; další kolo jen když jsou položky vidět a žádné neběží */
    const bool running = overlay_view_probe_collect();
    if (visible && !running) {
        bool want = s_recent.recheck || (g_get_monotonic_time() - s_recent.checked_us >= OVERLAY_VIEW_RECENT_CHECK_US);
        for (unsigned i = 0; i < s_recent.count && !want; i++) want = !s_recent.items[i].checked;
        if (want) {
            s_recent.recheck = false;
            overlay_view_probe_start();
        }
    }
    if (s_recent.count > 0) memcpy(v->recent, s_recent.items, sizeof(s_recent.items[0]) * s_recent.count);
    v->recent_count = s_recent.count;
    v->recent_now = (long long)(g_get_real_time() / G_USEC_PER_SEC);
}

/**
 * @brief Přidá řádek do nabídky ROM v kopii.
 * @param m       Stav tabu Machine.
 * @param label   Přeložený popis; NULL = prázdný.
 * @param section true = nadpis oddílu nebo oddělovač.
 * @param current true = právě vybraná ROM.
 * @param row     Index řádku v g_rom_config->roms.
 * @return false = kopie je plná (řádek se nepřidal).
 */
static bool overlay_view_add_rom(st_OVERLAY_VIEW_MACHINE *m, const char *label, bool section, bool current, int row)
{
    if (m->rom_count >= OVERLAY_VIEW_ROM_MAX) return false;
    st_OVERLAY_VIEW_ROM *r = &m->rom[m->rom_count++];
    overlay_view_copy_text(r->label, sizeof(r->label), label);
    r->section = section;
    r->current = current && !section;
    r->row = row;
    return true;
}

/**
 * @brief Vyplní stav tabu Machine: nabídku ROM a dostupnost vlastní rychlosti.
 *
 * Řádky nabídky vybírá jako klasické menu (menu_rom.cpp): jen povolené,
 * vývojové ROM jen v Development Mode. Oddělovač bez nadpisu, který by
 * stál na začátku nabídky nebo hned za nadpisem oddílu (po vynechání
 * vývojových ROM), se do kopie nedává.
 *
 * @param v Kopie po overlay_view_reset().
 * @pre Volající drží device_state_lock().
 */
static void overlay_view_fill_machine(st_OVERLAY_VIEW *v)
{
    st_OVERLAY_VIEW_MACHINE *m = &v->machine;
    if (g_rom_config && g_rom_config->roms) {
        int index = 0;
        for (const st_ROM_CONFIG_ROW *row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++, index++) {
            if (!row->enabled) continue;
            if ((row->flags & ROM_CONFIG_FLAG_DEVELOPMENT) && !EMULATOR_TEST_DEVELOPMENT_MODE) continue;
            if (row->type == ROM_CONFIG_TYPE_SECTION) {
                const bool titled = row->description && *row->description;
                if (!titled && (m->rom_count == 0 || m->rom[m->rom_count - 1].section)) continue;
                if (!overlay_view_add_rom(m, titled ? _(row->description) : "", true, false, index)) break;
            } else if (row->type == ROM_CONFIG_TYPE_ROM) {
                if (!overlay_view_add_rom(m, row->description ? _(row->description) : row->name, false, ROM_TEST_CURRENT(row), index)) break;
            }
        }
    }
    /* "Custom": vlastní rychlost je nastavená, nebo ji akce umí vrátit (z paměti overlaye nebo jádra, i při stojící emulaci) */
    m->custom_speed_available = g_customspeed.speed_in_percentage_requested != 100 || overlay_action_custom_speed_restore_value() != 0;
}

/**
 * @brief Vyplní stav tabu Display z g_display a z video rozhraní.
 * @param v Kopie po overlay_view_reset().
 * @pre UI vlákno (dotaz na fullscreen čte stav okna SDL), volající drží device_state_lock().
 */
static void overlay_view_fill_display(st_OVERLAY_VIEW *v)
{
    st_OVERLAY_VIEW_DISPLAY *d = &v->display;
    d->fullscreen = g_iface_video_callbacks && g_iface_video_callbacks->is_fullscreen && g_iface_video_callbacks->is_fullscreen();
    d->startup_size = (int)g_display.startup_window_size;
    d->locked_aspect = DISPLAY_TEST_LOCKED_ASPECT_RATIO != 0;
    d->color_schema = (int)g_display.color_schema;
    d->scanlines = DISPLAY_TEST_SCANLINES != 0;
    d->scanlines_intensity = g_display.scanlines_intensity;
    d->framerate_mode = (int)g_display.framerate_mode;
    d->custom_fps = g_display.custom_fps;
    d->slave_hz = VIDEO_SCREENS_PER_SEC;
    d->forced_redraw = DISPLAY_TEST_FORCED_FULL_SCREEN_REDRAWING != 0;
}

/**
 * @brief Vyplní stav tabu Sound: hlavní hlasitost, ztlumení, výstupní propust a hlasitosti kanálů.
 *
 * Počet a pojmenování kanálů jako okno Audio Mixer (audio_window.cpp):
 * stroj bez PSG má jen CTC0, se dvěma čipy PSG (MZ-1500, MZ-800 s volbou
 * druhého PSG) devět kanálů, jinak pět.
 *
 * @param v Kopie po overlay_view_reset().
 * @pre Volající drží device_state_lock() (g_psg_module mění emu vlákno).
 */
static void overlay_view_fill_sound(st_OVERLAY_VIEW *v)
{
    st_OVERLAY_VIEW_SOUND *s = &v->sound;
    s->master_gain = iface_audio_get_master_gain();
    s->mute = iface_audio_get_mute();
    s->has_output_filter = (AUDIO_HAVE_OUTPUT_FILTER != 0);
    s->output_filter = iface_audio_output_filter_enabled();
#if HAVE_PSG == 2
    s->stereo_names = g_psg_module.stereo;
    s->channel_count = g_psg_module.stereo ? 9 : 5;
#else
    s->stereo_names = false;
    s->channel_count = AUDIO_SRC_CHANNELS_COUNT;
#endif
    if (s->channel_count > OVERLAY_VIEW_AUDIO_CHANNELS_MAX) s->channel_count = OVERLAY_VIEW_AUDIO_CHANNELS_MAX;
    for (unsigned i = 0; i < s->channel_count && i < AUDIO_SRC_CHANNELS_COUNT; i++) s->channel_gain[i] = g_iface_audio.gain[i];
}

/**
 * @brief Vyplní stav tabu Input: zařízení prvního portu joysticku a dostupnost joysticku hostitele.
 * @param v Kopie po overlay_view_reset().
 * @pre UI vlákno (g_iface_joy mění jen UI vlákno), volající drží device_state_lock() (typ portu mění emu vlákno).
 */
static void overlay_view_fill_input(st_OVERLAY_VIEW *v)
{
    v->input.joy_type = (int)g_joy.dev[JOY_DEVID_0].type;
    v->input.joy_available = g_iface_joy.available_count > 0;
}

void overlay_view_fill_capture(st_OVERLAY_VIEW *v)
{
    if (!v) return;
    st_OVERLAY_VIEW_CAPTURE *c = &v->capture;
    memset(c, 0, sizeof(*c));

    st_SCREENSHOT_SERIES_STATUS series;
    screenshot_series_get_status(&series);
    c->series_active = series.active;

    /* stejný model jako položky "Video Recording" klasického menu (videorec_menu.cpp) */
    const en_VIDEOREC_STATE state = videorec_get_state();
    const bool pending = videorec_ui_is_start_pending();
    const st_VIDEOREC_UI_MENU_MODEL m = videorec_ui_menu_model(state, pending);
    c->video_stop = (state != VIDEOREC_STATE_IDLE) || pending;
    c->video_pause_enabled = m.pause_enabled;
    c->video_paused = m.pause_checked;
    st_VIDEOREC_STATUS vst;
    videorec_get_status(&vst);
    c->video_realtime = vst.timebase == VIDEOREC_TIMEBASE_REALTIME;

    st_REPLAY_STATUS rst;
    replay_get_status(&rst);
    c->replay_frame = rst.frame;
    c->replay_frames = rst.frames;
    c->replay_events = rst.events;
    if (rst.mode == REPLAY_MODE_RECORDING) {
        c->replay_mode = OVERLAY_REPLAY_RECORDING;
    } else if (rst.mode == REPLAY_MODE_PLAYING) {
        c->replay_mode = OVERLAY_REPLAY_PLAYING;
        if (rst.deviated) overlay_view_copy_text(c->replay_text, sizeof(c->replay_text), rst.deviation);
    } else {
        c->replay_mode = OVERLAY_REPLAY_OFF;
        if (rst.outcome != REPLAY_OUTCOME_NONE) overlay_view_copy_text(c->replay_text, sizeof(c->replay_text), rst.outcome_message);
    }
}

void overlay_view_fill(st_OVERLAY_VIEW *v)
{
    if (!v) return;
    overlay_view_reset(v);
    g_strlcpy(v->platform_text, OVERLAY_VIEW_PLATFORM, sizeof(v->platform_text));
    /* nastavená (požadovaná) rychlost: uplatněnou mění emu vlákno až na hranici snímku, při pauze tedy vůbec */
    v->speed_percent = g_customspeed.speed_in_percentage_requested;
    if (EMULATOR_TEST_MAX_SPEED) v->speed_mode = OVERLAY_SPEED_MAX;
    else if (v->speed_percent != 100) v->speed_mode = OVERLAY_SPEED_CUSTOM;
    else v->speed_mode = OVERLAY_SPEED_NORMAL;
    v->paused = EMULATOR_TEST_PAUSED;
    v->replay_active = replay_is_active();
    if (v->replay_active) overlay_view_set_replay_reason(v);
    overlay_view_fill_machine(v);
    overlay_view_fill_display(v);
    overlay_view_fill_sound(v);
    overlay_view_fill_input(v);

    overlay_view_fill_fdd(v);
    overlay_view_fill_cmt(v);
    overlay_view_fill_qd(v);
}

/**
 * @brief Nastaví mechaniku testovacího stavu jako připojenou s vloženým obrazem.
 * @param d       Mechanika.
 * @param path    Cesta obrazu.
 * @param size_kb Velikost dat v KB.
 */
static void overlay_view_fixture_disk(st_OVERLAY_VIEW_FDD *d, const char *path, unsigned size_kb)
{
    d->mounted = true;
    overlay_view_copy_text(d->path, sizeof(d->path), path);
    overlay_view_copy_basename(d->name, sizeof(d->name), path);
    d->size_kb = size_kb;
}

/**
 * @brief Přidá položku nedávných médií do testovacího stavu.
 * @param v     Kopie (položka se přidá na konec; plná kopie se nemění).
 * @param kind  Druh média.
 * @param path  Cesta.
 * @param age_s Stáří položky v sekundách proti st_OVERLAY_VIEW::recent_now.
 * @return Přidaná položka; NULL při plné kopii.
 */
static st_OVERLAY_VIEW_RECENT *overlay_view_fixture_recent(st_OVERLAY_VIEW *v, en_OVERLAY_MEDIA_KIND kind, const char *path, long long age_s)
{
    if (v->recent_count >= OVERLAY_VIEW_RECENT_MAX) return NULL;
    st_OVERLAY_VIEW_RECENT *r = &v->recent[v->recent_count++];
    r->kind = kind;
    overlay_view_copy_text(r->path, sizeof(r->path), path);
    overlay_view_copy_basename(r->name, sizeof(r->name), path);
    r->time = v->recent_now - age_s;
    r->checked = true;
    r->checked_us = (long long)g_get_monotonic_time();
    return r;
}

/**
 * @brief Obsah konceptu media-v2 (společný základ testovacích stavů DEFAULT, REPLAY a NO_FDC).
 * @param v Kopie po overlay_view_reset().
 */
static void overlay_view_fixture_concept(st_OVERLAY_VIEW *v)
{
    g_strlcpy(v->platform_text, "MZ-800", sizeof(v->platform_text));
    v->speed_percent = 100;
    v->speed_mode = OVERLAY_SPEED_NORMAL;
    v->paused = true;

    /* koncept machine-v1: nabídka ROM stroje MZ-800 mimo Development Mode, vybraná Standard */
    st_OVERLAY_VIEW_MACHINE *m = &v->machine;
    overlay_view_add_rom(m, _("Standard Sharp MZ-800 ROM"), false, true, 0);
    overlay_view_add_rom(m, _("JSS ROMs"), true, false, 1);
    overlay_view_add_rom(m, _("JSS v1.08C - for MZ-1R18"), false, false, 5);
    overlay_view_add_rom(m, _("JSS v1.06A - for Pezik"), false, false, 6);
    overlay_view_add_rom(m, _("Willy ROMs"), true, false, 7);
    overlay_view_add_rom(m, _("Willy - English"), false, false, 8);
    overlay_view_add_rom(m, _("Willy - German"), false, false, 9);
    overlay_view_add_rom(m, _("Willy - Japanese"), false, false, 10);
    overlay_view_add_rom(m, _("User"), true, false, 11);
    overlay_view_add_rom(m, _("User Defined ROM"), false, false, 12);
    m->custom_speed_available = true;

    /* koncept display-v1 */
    st_OVERLAY_VIEW_DISPLAY *dsp = &v->display;
    dsp->fullscreen = true;
    dsp->startup_size = (int)DISPLAY_STARTUP_SIZE_FULLSCREEN;
    dsp->locked_aspect = true;
    dsp->color_schema = (int)DISPLAY_NORMAL;
    dsp->scanlines = true;
    dsp->scanlines_intensity = 35;
    dsp->framerate_mode = (int)DISPLAY_FRAMERATE_MODE_VSYNC;
    dsp->custom_fps = DISPLAY_CUSTOM_FPS_DEFAULT;
    dsp->slave_hz = 50;
    dsp->forced_redraw = false;

    /* koncept sound-v1: hlavní hlasitost 80 %, propust zapnutá, pět kanálů, šum 70 % */
    st_OVERLAY_VIEW_SOUND *snd = &v->sound;
    snd->master_gain = 0.8f;
    snd->mute = false;
    snd->has_output_filter = true;
    snd->output_filter = true;
    snd->channel_count = 5;
    snd->stereo_names = false;
    snd->channel_gain[4] = 0.7f;
    /* koncept input-v1: joystick na numerické klávesnici */
    v->input.joy_type = (int)OVERLAY_JOY_NUM_KEYPAD;
    v->input.joy_available = true;
    /* koncept capture-v1: nic neběží, záznam podle reality */
    v->capture.video_realtime = true;
    for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
        v->fdd[i].present = true;
        v->fdd[i].connected = true;
    }
    overlay_view_fixture_disk(&v->fdd[0], "C:/mz800/disks/cpm-system.dsk", 320);
    v->fdd[0].active = true;
    overlay_view_fixture_disk(&v->fdd[1], "C:/mz800/disks/games-vol2.dsk", 320);
    v->fdd[1].unsaved = true;

    st_OVERLAY_VIEW_CMT *c = &v->cmt;
    c->has_tape = true;
    overlay_view_copy_text(c->name, sizeof(c->name), "flappy.mzf");
    overlay_view_copy_text(c->path, sizeof(c->path), "C:/mz800/tapes/flappy.mzf");
    c->state = OVERLAY_CMT_PLAY;
    c->block_count = 5;
    c->current_block = 1;
    c->elapsed_s = 72.0;
    c->total_s = 185.0;
    c->progress = 72.0 / 185.0;
    c->mode = OVERLAY_CMT_MODE_BOOST;
    c->direct_supported = true;
    c->current_has_mzf = true;
    c->first_playable = 0;
    c->tape_serial = 1;

    v->qd.present = true;
    v->qd.connected = true;

    /* nedávná média: šest disket konceptu media-insert-v2 a další druhy; cesty s '/' kvůli přenositelnosti testů */
    const long long minute = 60, hour = 3600, day = 86400;
    v->recent_now = OVERLAY_VIEW_FIXTURE_NOW;
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_DSK, "D:/MZ/disks/games-vol2.dsk", 2 * minute);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_DSK, "D:/MZ/disks/utils.dsk", day + 3 * hour);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_DSK, "D:/MZ/demos/demo-disk.dsk", 3 * day);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_DSK, "D:/MZ/disks/basic-disk.dsk", 8 * day);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_DSK, "D:/MZ/cpm/turbo-pascal.dsk", 9 * day);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_DSK, "D:/MZ/work/blank-720k.dsk", 15 * day);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_DSK, "D:/MZ/disks/cpm-system.dsk", 20 * day);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_CMT, "D:/MZ/tapes/flappy.mzf", 10 * minute);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_CMT, "D:/MZ/tapes/basic-1z016.mzt", 2 * day);
    overlay_view_fixture_recent(v, OVERLAY_MEDIA_QD, "D:/MZ/qd/games.mzq", 5 * day);
    overlay_view_recent_sort(v->recent, v->recent_count);
}

void overlay_view_make_fixture(st_OVERLAY_VIEW *v, en_OVERLAY_FIXTURE f)
{
    if (!v) return;
    overlay_view_reset(v);
    switch (f) {
        case OVERLAY_FIXTURE_DEFAULT:
            overlay_view_fixture_concept(v);
            break;

        case OVERLAY_FIXTURE_EMPTY:
            g_strlcpy(v->platform_text, "MZ-800", sizeof(v->platform_text));
            for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
                v->fdd[i].present = true;
                v->fdd[i].connected = true;
            }
            v->cmt.direct_supported = true;
            v->qd.present = true;
            v->qd.connected = true;
            overlay_view_add_rom(&v->machine, _("Standard Sharp MZ-800 ROM"), false, true, 0);
            /* ztlumeno, stroj bez propusti a bez PSG, žádný joystick, poslední replay s výsledkem; emulace
             * běží, takže přepínač Listen ukazuje zapnuto a je zakázaný */
            v->sound.listen = (int)OVERLAY_LISTEN_BLOCKED_RUNNING;
            v->sound.master_gain = 0.0f;
            v->sound.mute = true;
            overlay_view_copy_text(v->capture.replay_text, sizeof(v->capture.replay_text), "playback finished at frame 1234");
            break;

        case OVERLAY_FIXTURE_LONG_TEXTS: {
            overlay_view_fixture_concept(v);
            v->speed_percent = 1000;
            v->speed_mode = OVERLAY_SPEED_CUSTOM;
            /* vybraná ROM s popisem přes celé pole, režim Custom FPS s největší hodnotou */
            v->machine.rom[0].current = false;
            v->machine.rom[2].current = true;
            memset(v->machine.rom[2].label, 'W', sizeof(v->machine.rom[2].label) - 1);
            v->machine.rom[2].label[sizeof(v->machine.rom[2].label) - 1] = '\0';
            v->display.framerate_mode = (int)DISPLAY_FRAMERATE_MODE_CUSTOM;
            v->display.custom_fps = DISPLAY_CUSTOM_FPS_MAX;
            /* devět kanálů se jmény dvou čipů PSG; běží série, pozastavený video záznam a záznam vstupů */
            v->sound.channel_count = 9;
            v->sound.stereo_names = true;
            for (int i = 0; i < OVERLAY_VIEW_AUDIO_CHANNELS_MAX; i++) v->sound.channel_gain[i] = (float)(i + 1) / 9.0f;
            v->input.joy_type = (int)OVERLAY_JOY_JOYSTICK;
            v->capture.series_active = true;
            v->capture.video_stop = true;
            v->capture.video_pause_enabled = true;
            v->capture.video_paused = true;
            v->capture.video_realtime = false;
            v->capture.replay_mode = (int)OVERLAY_REPLAY_RECORDING;
            v->capture.replay_frame = 4000000000u;
            v->capture.replay_events = 4000000000u;
            /* jméno souboru o 200 znacích bez mezer, v adresáři s diakritikou */
            char name[201];
            for (int i = 0; i < 200; i++) name[i] = (char)((i % 10 == 9) ? '-' : ('a' + i % 26));
            name[200] = '\0';
            static const char dir[] = "C:/U\xC5\xBEivatel\xC3\xA9/P\xC5\x99\xC3\xADli\xC5\xA1 \xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88/disky";
            char path[OVERLAY_VIEW_PATH_SIZE];
            for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
                memcpy(name + 196, ".dsk", 4);
                name[0] = (char)('0' + i);
                g_snprintf(path, sizeof(path), "%s/%s", dir, name);
                overlay_view_fixture_disk(&v->fdd[i], path, 2880);
                v->fdd[i].unsaved = true;
                v->fdd[i].readonly = (i == 3);
                v->fdd[i].active = (i == 0);
            }
            memcpy(name + 196, ".mzt", 4);
            g_snprintf(path, sizeof(path), "%s/%s", dir, name);
            overlay_view_copy_text(v->cmt.path, sizeof(v->cmt.path), path);
            overlay_view_copy_basename(v->cmt.name, sizeof(v->cmt.name), path);
            v->cmt.modified = true;
            v->cmt.block_count = 300;
            v->cmt.current_block = 149;
            v->cmt.elapsed_s = 35999.0;
            v->cmt.total_s = 71999.0;
            v->cmt.progress = 0.5;
            memcpy(name + 196, ".mzq", 4);
            g_snprintf(path, sizeof(path), "%s/%s", dir, name);
            v->qd.mounted = true;
            overlay_view_copy_text(v->qd.path, sizeof(v->qd.path), path);
            overlay_view_copy_basename(v->qd.name, sizeof(v->qd.name), path);
            v->qd.unsaved = true;
            /* plná historie: 12 položek každého druhu se jmény o 200 znacích, každá třetí chybí na disku */
            v->recent_count = 0;
            static const char *const ext[OVERLAY_MEDIA_KIND_COUNT] = {".dsk", ".mzt", ".mzq"};
            for (int k = 0; k < (int)OVERLAY_MEDIA_KIND_COUNT; k++) {
                for (int i = 0; i < OVERLAY_VIEW_RECENT_PER_KIND; i++) {
                    memcpy(name + 196, ext[k], 4);
                    name[0] = (char)('a' + i);
                    g_snprintf(path, sizeof(path), "%s/%s", dir, name);
                    st_OVERLAY_VIEW_RECENT *r = overlay_view_fixture_recent(v, (en_OVERLAY_MEDIA_KIND)k, path, (long long)(i * 3 + k) * 40000);
                    if (r) r->missing = (i % 3 == 2);
                }
            }
            overlay_view_recent_sort(v->recent, v->recent_count);
            break;
        }

        case OVERLAY_FIXTURE_REPLAY:
            overlay_view_fixture_concept(v);
            v->replay_active = true;
            v->cmt.replay_playing = true;
            v->capture.replay_mode = (int)OVERLAY_REPLAY_PLAYING;
            v->capture.replay_frame = 1250;
            v->capture.replay_frames = 9000;
            overlay_view_set_replay_reason(v);
            break;

        case OVERLAY_FIXTURE_NO_FDC:
            overlay_view_fixture_concept(v);
            for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
                v->fdd[i].connected = false;
                v->fdd[i].active = false;
            }
            v->qd.connected = false;
            break;

        case OVERLAY_FIXTURE_COUNT:
        default:
            break;
    }
}
