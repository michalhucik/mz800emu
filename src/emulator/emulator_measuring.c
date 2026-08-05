#include "main.h"
#ifdef WINDOWS
#include <windows.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <glib.h>
#include <time.h>
#include <math.h>
#include <inttypes.h>

#include "emulator_measuring.h"
#include "emulator.h"
#include "hw-generic/gdg/gdg_state.h"
#include "mzarch/mzhal.h"

st_EMULATOR_MEASURING g_emulator_measuring;

// Funkce pro porovnání čísel (qsort potřebuje tuto funkci)
static int compare(const void *a, const void *b)
{
    double diff = (*(double *)a - *(double *)b);
    return (diff > 0) - (diff < 0);
}

void emulator_measuring_frame_timing_reset(void)
{
    st_EMULATOR_MEASURING_FRAME_TIMING *ms20 = &g_emulator_measuring.frame_timing;
    ms20->head = 0;
    ms20->tail = 0;
    struct timespec *last_time = &ms20->last_time;
    last_time->tv_sec = 0;
    last_time->tv_nsec = 0;
    ms20->stats_cntr = ms20->stats_predef;
}

static void emulator_measuring_stats(double *samples, uint32_t head, uint32_t tail, uint32_t measuring_width)
{
    uint32_t samples_count = head - tail;
    if (measuring_width < samples_count)
    {
        samples_count = measuring_width;
        tail = head - samples_count;
    };

    // Vyhodnocení měření
    double sum = 0.0, min = samples[tail % MAX_MEASURING_FRAME_TIMING_SAMPLES], max = samples[tail % MAX_MEASURING_FRAME_TIMING_SAMPLES];
    double sum_sq = 0.0; // Pro výpočet směrodatné odchylky

    for (uint32_t i = 0; i < samples_count; i++)
    {
        double val = samples[(tail + i) % MAX_MEASURING_FRAME_TIMING_SAMPLES];
        sum += val;
        sum_sq += val * val;
        if (val < min)
            min = val;
        if (val > max)
            max = val;
    }

    double mean = sum / samples_count;
    double variance = (sum_sq / samples_count) - (mean * mean);
    double stddev = sqrt(variance);

    // Výpočet mediánu
    double sorted[MAX_MEASURING_FRAME_TIMING_SAMPLES];
    for (uint32_t i = 0; i < samples_count; i++)
    {
        sorted[i] = samples[i];
    }
    qsort(sorted, samples_count, sizeof(double), compare);

    double median;
    if (samples_count % 2 == 0)
    {
        median = (sorted[samples_count / 2 - 1] + sorted[samples_count / 2]) / 2.0;
    }
    else
    {
        median = sorted[samples_count / 2];
    };

    st_EMULATOR_MEASURING_FRAME_TIMING_STATS *stats = &g_emulator_measuring.frame_timing.stats;

    stats->mean = mean / 1000;
    stats->min = min / 1000;
    stats->max = max / 1000;
    stats->median = median / 1000;
    stats->stddev = stddev / 1000;
    stats->samples_count = samples_count;
    stats->total_width = (float)samples_count / g_mzhal.video_screens_per_sec;
    stats->real_total_width = sum / 1e6;

    if (g_emulator_measuring.frame_timing.console_output_enabled)
    {
        g_print("\n---- Frame timing statistics ----\n");
        g_print(" Average time:       %7.4f ms\n", stats->mean);
        g_print(" Max time:           %7.4f ms\n", stats->max);
        g_print(" Min time:           %7.4f ms\n", stats->min);
        g_print(" Median:             %7.4f ms\n", stats->median);
        g_print(" Standard deviation: %7.4f ms\n", stats->stddev);
        g_print("--------------------------------\n");
        g_print(" Frames count:       %7d\n", stats->samples_count);
        g_print(" Total width:        %7.2f s\n", stats->total_width);
        g_print(" Real total width:   %7.4f s\n\n", stats->real_total_width);
    };
}

void emulator_measuring_frame_timing_event(void)
{
    st_EMULATOR_MEASURING_FRAME_TIMING *ms20 = &g_emulator_measuring.frame_timing;
    struct timespec *last_time = &ms20->last_time;
    double *samples = ms20->samples;
    uint32_t samples_count = ms20->head - ms20->tail;

    struct timespec curr_time;
    clock_gettime(CLOCK_MONOTONIC, &curr_time);

    if (last_time->tv_sec != 0 && last_time->tv_nsec != 0)
    {
        // Výpočet rozdílu času mezi voláními v mikrosekundách
        double delta = (curr_time.tv_sec - last_time->tv_sec) * 1e6 +
                       (curr_time.tv_nsec - last_time->tv_nsec) / 1e3;

        samples[ms20->head % MAX_MEASURING_FRAME_TIMING_SAMPLES] = delta;

        ms20->head++;
        if (samples_count >= MAX_MEASURING_FRAME_TIMING_SAMPLES)
            ms20->tail++;
        ms20->stats_cntr--;
    };

    ms20->last_time.tv_sec = curr_time.tv_sec;
    ms20->last_time.tv_nsec = curr_time.tv_nsec;

    if (ms20->stats_cntr == 0)
    {
        emulator_measuring_stats(samples, ms20->head, ms20->tail, ms20->measuring_width);
        ms20->stats_cntr = ms20->stats_predef; // Resetujeme čítač
    };
}

static char *format_uint32_with_thousands(uint32_t number)
{
    GString *str0 = g_string_new(NULL);
    GString *str1 = g_string_new(NULL);
    g_string_printf(str0, "%u", number);
    g_string_printf(str1, "%'u", number);

    if (!g_string_equal(str0, str1))
    {
        // Porovnejme znak po znaku a zjistíme oddělovač
        for (size_t i = 0; i < str1->len; i++)
        {
            // Znak, který není číslem, je pravděpodobně oddělovač
            if (!g_ascii_isdigit(str1->str[i]))
            {
                char separator = str1->str[i]; // Oddělovač nalezen

                // Nahrazení oddělovače mezerou
                for (size_t j = 0; j < str1->len; j++)
                {
                    if (str1->str[j] == separator)
                    {
                        str1->str[j] = ' ';
                    }
                }
                break;
            };
        };
    };

    g_string_free(str0, TRUE);
    return g_string_free(str1, FALSE);
}

void emulator_measuring_gdg_event(st_EMULATOR_MEASURING_GDG *msgdg)
{
    if (msgdg->enabled)
    {
        g_rw_lock_writer_lock(&msgdg->rwlock);
    };

    if (!msgdg->timer)
    {
        msgdg->timer = g_timer_new();
        g_timer_start(msgdg->timer);
    };

    g_timer_stop(msgdg->timer);
    // cas v sekundach od posledniho zastaveni
    msgdg->elapsed_time = (float)g_timer_elapsed(msgdg->timer, NULL);
    g_timer_start(msgdg->timer);

    uint64_t now_gdg_ticks = gdg_compute_total_ticks(0);
    uint64_t elapsed_gdg_ticks = now_gdg_ticks - msgdg->last_gdg_ticks;
    msgdg->last_gdg_ticks = now_gdg_ticks;

    msgdg->current_gdg_frequency = elapsed_gdg_ticks / msgdg->elapsed_time;
    /* Runtime z g_mzhal (mzhal 10b, cold - measuring vlákno 1x za periodu). */
    msgdg->current_fps = (elapsed_gdg_ticks / g_mzhal.video_screen_ticks) / msgdg->elapsed_time;
    msgdg->current_gdg_frequency_percent = (msgdg->current_gdg_frequency / g_mzhal.gdgclk_real_base) * 100;

    if (msgdg->enabled)
    {
        g_rw_lock_writer_unlock(&msgdg->rwlock);
    };

    if (msgdg->console_output_enabled)
    {
        float native_fps = (float)g_mzhal.gdgclk_real_base / g_mzhal.video_screen_ticks;
        float simulated_fps = (float)g_mzhal.gdgclk_base / g_mzhal.video_screen_ticks;

        char *formatted_num = NULL;
        printf("\n\nGDG meassuring:\n");
        printf(" - Measured time: %f s\n", msgdg->elapsed_time);

        formatted_num = format_uint32_with_thousands(g_mzhal.gdgclk_real_base);
        printf(" - Native GDG freq.: %s Hz\n", formatted_num);
        g_free(formatted_num);

        printf(" - Native FPS: %.4f\n", native_fps);

        formatted_num = format_uint32_with_thousands(g_mzhal.gdgclk_base);
        printf(" - Simulated GDG freq.: %s Hz\n", formatted_num);
        g_free(formatted_num);

        printf(" - Simulated FPS: %.4f\n", simulated_fps);

        formatted_num = format_uint32_with_thousands(msgdg->current_gdg_frequency);
        printf(" - Current GDG freq.: %s Hz\n", formatted_num);
        g_free(formatted_num);

        printf(" - Current FPS: %.4f\n", msgdg->current_fps);
        printf(" - Current GDG freq. in %%: %.4f %%\n", msgdg->current_gdg_frequency_percent);
    };
}

void emulator_measuring_gdg_init(st_EMULATOR_MEASURING_GDG *msgdg)
{
    msgdg->enabled = false;
    msgdg->console_output_enabled = false;
    msgdg->timer = NULL;
    msgdg->last_gdg_ticks = 0;
    msgdg->elapsed_time = 0.0;
    msgdg->current_gdg_frequency = 0.0;
    msgdg->current_fps = 0.0;
    msgdg->current_gdg_frequency_percent = 0.0;
    msgdg->thread = NULL;
    g_rw_lock_init(&msgdg->rwlock);
    g_mutex_init(&msgdg->mutex);
    g_cond_init(&msgdg->cond);
    msgdg->thread_usleep_time = EMULATOR_MEASURING_DEFAULT_GDG_UPDATE_SEC * 1000000;
}

void emulator_measuring_gdg_exit(st_EMULATOR_MEASURING_GDG *msgdg)
{
    msgdg->enabled = false;

    if (msgdg->thread)
    {
        g_print("Stopping GDG measuring thread...\n");

        g_mutex_lock(&msgdg->mutex);
        msgdg->enabled = false;
        g_cond_signal(&msgdg->cond);
        g_mutex_unlock(&msgdg->mutex);

        g_thread_join(msgdg->thread);
        msgdg->thread = NULL;
    };

    if (msgdg->timer)
    {
        g_timer_stop(msgdg->timer);
        g_timer_destroy(msgdg->timer);
        msgdg->timer = NULL;
    };

    g_rw_lock_clear(&msgdg->rwlock);
    g_mutex_clear(&msgdg->mutex);
    g_cond_clear(&msgdg->cond);
}

void emulator_measuring_init(void)
{
    st_EMULATOR_MEASURING_FRAME_TIMING *ms20 = &g_emulator_measuring.frame_timing;
    ms20->enabled = false;
    ms20->console_output_enabled = false;
    ms20->stats_predef = PREDEF_MEASURING_FRAME_TIMING;
    ms20->measuring_width = MAX_MEASURING_FRAME_TIMING_SAMPLES;
    emulator_measuring_frame_timing_reset();

    st_EMULATOR_MEASURING_GDG *msgdg = &g_emulator_measuring.gdg;
    emulator_measuring_gdg_init(msgdg);

    emulator_measuring_maxspeed_init(&g_emulator_measuring.maxspeed);
    // Synchronizace segmentu s aktuální rychlostí (kdyby emulátor startoval v MAX SPEED)
    emulator_measuring_maxspeed_update_segment();
}

void emulator_measuring_exit(void)
{
    st_EMULATOR_MEASURING_GDG *msgdg = &g_emulator_measuring.gdg;
    emulator_measuring_gdg_exit(msgdg);

    emulator_measuring_maxspeed_exit(&g_emulator_measuring.maxspeed);
}

gpointer emulator_measuring_gdg_thread(st_EMULATOR_MEASURING_GDG *msgdg)
{
    while (msgdg->enabled)
    {
        emulator_measuring_gdg_event(msgdg);
        g_mutex_lock(&msgdg->mutex);
        // cekame na signal s timeoutem thread_usleep_time
        g_cond_wait_until(&msgdg->cond, &msgdg->mutex, g_get_monotonic_time() + msgdg->thread_usleep_time);
        g_mutex_unlock(&msgdg->mutex);
    };
    return NULL;
}

bool emulator_measuring_gdg_set_enabled(st_EMULATOR_MEASURING_GDG *msgdg, bool enabled, int update_time_sec)
{
    if ((enabled == true) && ((update_time_sec < 1) || (update_time_sec > 10)))
    {
        SDLAPP_ERROR("Invalid parameters for GDG measuring!");
        return false;
    };

    if (enabled == false)
    {
        if (!msgdg->thread)
            return true;

        g_print("Stopping GDG measuring thread...\n");

        g_mutex_lock(&msgdg->mutex);
        msgdg->enabled = false;
        g_cond_signal(&msgdg->cond);
        g_mutex_unlock(&msgdg->mutex);

        g_thread_join(msgdg->thread);
        msgdg->thread = NULL;
        return true;
    };

    if ((msgdg->enabled == false) && (msgdg->thread))
    {
        g_print("Stopping previous GDG measuring thread...\n");

        g_mutex_lock(&msgdg->mutex);
        msgdg->enabled = false;
        g_cond_signal(&msgdg->cond);
        g_mutex_unlock(&msgdg->mutex);

        g_thread_join(msgdg->thread);
        msgdg->thread = NULL;
    };

    g_mutex_lock(&msgdg->mutex);
    msgdg->enabled = true;
    msgdg->thread_usleep_time = update_time_sec * 1000000;
    g_cond_signal(&msgdg->cond);
    g_mutex_unlock(&msgdg->mutex);

    if (!msgdg->thread)
    {
        msgdg->thread = g_thread_new("emulator_measuring_gdg_thread", (GThreadFunc)emulator_measuring_gdg_thread, msgdg);
        if (!msgdg->thread)
        {
            g_print("Failed to start GDG measuring thread!\n");
            msgdg->enabled = false;
            return false;
        };
    };
    return true;
}

/* ====================================================================== */
/* MAX SPEED benchmark (vrstva A - kumulativní celek)                     */
/* ====================================================================== */

/**
 * @brief Vrátí aktuální monotónní wall-clock v mikrosekundách.
 *
 * Používá @c CLOCK_MONOTONIC (shodně s frame timing měřením), takže není
 * ovlivněn skoky systémových hodin.
 *
 * @return Počet mikrosekund od neurčeného počátku (smysl mají jen rozdíly).
 */
static uint64_t maxspeed_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_nsec / 1000ULL;
}

static gpointer emulator_measuring_maxspeed_sample_thread(st_EMULATOR_MEASURING_MAXSPEED *ms);

/**
 * @brief Vrátí aktuální kumulativní součet (pxCLK, wall) měřených segmentů.
 *
 * Sečte uzavřené akumulátory a (pokud právě běží) přírůstek aktuálního segmentu.
 *
 * @pre Volající MUSÍ držet @c ms->mutex.
 * @param ms Stav benchmarku.
 * @param[out] ticks Kumulativní emulované pxCLK.
 * @param[out] wall_us Kumulativní reálný čas měřených segmentů [us].
 */
static void maxspeed_current_totals_locked(st_EMULATOR_MEASURING_MAXSPEED *ms,
                                           uint64_t *ticks, uint64_t *wall_us)
{
    uint64_t t = ms->acc_ticks;
    uint64_t w = ms->acc_wall_us;
    if (ms->seg_active)
    {
        t += gdg_compute_total_ticks(0) - ms->seg_start_ticks;
        w += maxspeed_now_us() - ms->seg_start_wall_us;
    };
    *ticks = t;
    *wall_us = w;
}

/**
 * @brief Spočítá distribuci (min/max/mean/median/stddev) vzorků efektivity %.
 *
 * Naplní pole @c dist_* ve výsledku z ring bufferu vzorků (vrstva B). Při
 * prázdném bufferu nastaví @c dist_samples = 0 a ostatní na nulu.
 *
 * @pre Volající MUSÍ držet @c ms->mutex.
 * @param ms Stav benchmarku.
 * @param[out] out Výsledek, jehož @c dist_* pole se naplní.
 */
static void maxspeed_fill_distribution_locked(st_EMULATOR_MEASURING_MAXSPEED *ms,
                                              st_MAXSPEED_BENCH_RESULT *out)
{
    uint32_t n = ms->samples_head - ms->samples_tail;

    if (n == 0)
    {
        out->dist_samples = 0;
        out->eff_min = 0.0;
        out->eff_max = 0.0;
        out->eff_mean = 0.0;
        out->eff_median = 0.0;
        out->eff_stddev = 0.0;
        return;
    };

    double sum = 0.0, sum_sq = 0.0;
    double mn = ms->samples[ms->samples_tail % MAX_MAXSPEED_BENCH_SAMPLES];
    double mx = mn;
    double sorted[MAX_MAXSPEED_BENCH_SAMPLES];

    for (uint32_t i = 0; i < n; i++)
    {
        double v = ms->samples[(ms->samples_tail + i) % MAX_MAXSPEED_BENCH_SAMPLES];
        sum += v;
        sum_sq += v * v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sorted[i] = v;
    }

    double mean = sum / n;
    double variance = (sum_sq / n) - (mean * mean);
    if (variance < 0.0) variance = 0.0; /* numerická ochrana proti -0 */
    double stddev = sqrt(variance);

    qsort(sorted, n, sizeof(double), compare);
    double median = (n % 2 == 0) ? (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0 : sorted[n / 2];

    out->dist_samples = n;
    out->eff_min = mn;
    out->eff_max = mx;
    out->eff_mean = mean;
    out->eff_median = median;
    out->eff_stddev = stddev;
}

void emulator_measuring_maxspeed_init(st_EMULATOR_MEASURING_MAXSPEED *ms)
{
    ms->console_output_enabled = false;
    ms->acc_ticks = 0;
    ms->acc_wall_us = 0;
    ms->seg_active = false;
    ms->stalled = false;
    ms->seg_start_ticks = 0;
    ms->seg_start_wall_us = 0;
    g_mutex_init(&ms->mutex);

    /* Vrstva B - sampling thread + ring buffer */
    ms->sample_thread = NULL;
    g_mutex_init(&ms->sample_ctrl_mutex);
    g_cond_init(&ms->sample_cond);
    ms->sample_thread_run = false;
    ms->sample_interval_us = (gulong)EMULATOR_MEASURING_DEFAULT_MAXSPEED_SAMPLE_SEC * 1000000;
    ms->samples_head = 0;
    ms->samples_tail = 0;
    ms->last_sample_ticks = 0;
    ms->last_sample_wall_us = 0;
    ms->have_last_sample = false;

    ms->sample_thread_run = true;
    ms->sample_thread = g_thread_new("maxspeed_bench_sampler",
                                     (GThreadFunc)emulator_measuring_maxspeed_sample_thread, ms);
}

void emulator_measuring_maxspeed_exit(st_EMULATOR_MEASURING_MAXSPEED *ms)
{
    if (ms->sample_thread)
    {
        g_mutex_lock(&ms->sample_ctrl_mutex);
        ms->sample_thread_run = false;
        g_cond_signal(&ms->sample_cond);
        g_mutex_unlock(&ms->sample_ctrl_mutex);
        g_thread_join(ms->sample_thread);
        ms->sample_thread = NULL;
    };

    g_cond_clear(&ms->sample_cond);
    g_mutex_clear(&ms->sample_ctrl_mutex);
    g_mutex_clear(&ms->mutex);
}

void emulator_measuring_maxspeed_update_segment(void)
{
    st_EMULATOR_MEASURING_MAXSPEED *ms = &g_emulator_measuring.maxspeed;

    g_mutex_lock(&ms->mutex);
    // Segment se měří jen v MAX SPEED, jen když emulace není pozastavena
    // a jen mimo externí stall (CMT-hack apod.)
    bool want_active = EMULATOR_TEST_MAX_SPEED && (!EMULATOR_TEST_PAUSED) && (!ms->stalled);

    if (want_active && (!ms->seg_active))
    {
        // Otevření nového segmentu
        ms->seg_start_ticks = gdg_compute_total_ticks(0);
        ms->seg_start_wall_us = maxspeed_now_us();
        ms->seg_active = true;
    }
    else if ((!want_active) && ms->seg_active)
    {
        // Uzavření segmentu - přírůstek do akumulátorů
        ms->acc_ticks += gdg_compute_total_ticks(0) - ms->seg_start_ticks;
        ms->acc_wall_us += maxspeed_now_us() - ms->seg_start_wall_us;
        ms->seg_active = false;
    };
    g_mutex_unlock(&ms->mutex);
}

void emulator_measuring_maxspeed_stall_begin(void)
{
    st_EMULATOR_MEASURING_MAXSPEED *ms = &g_emulator_measuring.maxspeed;

    g_mutex_lock(&ms->mutex);
    ms->stalled = true;
    g_mutex_unlock(&ms->mutex);

    // Uzavře probíhající segment (stalled -> want_active false)
    emulator_measuring_maxspeed_update_segment();
}

void emulator_measuring_maxspeed_stall_end(void)
{
    st_EMULATOR_MEASURING_MAXSPEED *ms = &g_emulator_measuring.maxspeed;

    g_mutex_lock(&ms->mutex);
    ms->stalled = false;
    g_mutex_unlock(&ms->mutex);

    // Otevře nový segment, pokud je stále MAX SPEED a není pauza
    emulator_measuring_maxspeed_update_segment();
}

void emulator_measuring_maxspeed_reset(void)
{
    st_EMULATOR_MEASURING_MAXSPEED *ms = &g_emulator_measuring.maxspeed;

    g_mutex_lock(&ms->mutex);
    ms->acc_ticks = 0;
    ms->acc_wall_us = 0;
    ms->seg_active = false; // vynutí znovuotevření segmentu níže
    // Vrstva B - vyprázdni ring buffer vzorků a obnov cold start
    ms->samples_head = 0;
    ms->samples_tail = 0;
    ms->last_sample_ticks = 0;
    ms->last_sample_wall_us = 0;
    ms->have_last_sample = false;
    g_mutex_unlock(&ms->mutex);

    // Pokud je emulátor právě v měřeném stavu, otevři nový segment od teď
    emulator_measuring_maxspeed_update_segment();
}

void emulator_measuring_maxspeed_report(st_MAXSPEED_BENCH_RESULT *out)
{
    st_EMULATOR_MEASURING_MAXSPEED *ms = &g_emulator_measuring.maxspeed;

    g_mutex_lock(&ms->mutex);
    uint64_t ticks, wall_us;
    maxspeed_current_totals_locked(ms, &ticks, &wall_us);
    bool active = ms->seg_active;
    maxspeed_fill_distribution_locked(ms, out); // vrstva B - distribuce
    g_mutex_unlock(&ms->mutex);

    out->total_ticks = ticks;
    out->total_wall_sec = (double)wall_us / 1e6;
    out->segment_active = active;
    out->valid = (wall_us > 0);

    if (out->valid)
    {
        out->pxclk_per_sec = (double)ticks / out->total_wall_sec;
        out->efficiency_percent = out->pxclk_per_sec / (double)g_mzhal.gdgclk_real_base * 100.0;
        out->fps = out->pxclk_per_sec / (double)g_mzhal.video_screen_ticks;
    }
    else
    {
        out->pxclk_per_sec = 0.0;
        out->efficiency_percent = 0.0;
        out->fps = 0.0;
    };
}

void emulator_measuring_maxspeed_print(void)
{
    st_MAXSPEED_BENCH_RESULT r;
    emulator_measuring_maxspeed_report(&r);

    printf("\n---- MAX SPEED benchmark ----\n");
    if (!r.valid)
    {
        printf(" No MAX SPEED time measured yet.\n");
        printf("-----------------------------\n\n");
        return;
    };

    printf(" Measured time:      %10.4f s%s\n", r.total_wall_sec, r.segment_active ? " (running)" : "");
    printf(" Emulated pxCLK:     %10" PRIu64 "\n", r.total_ticks);
    printf(" Throughput:         %10.0f pxCLK/s\n", r.pxclk_per_sec);
    printf(" Efficiency:         %10.2f %%\n", r.efficiency_percent);
    printf(" FB-FPS:             %10.2f\n", r.fps);
    if (r.dist_samples > 0)
    {
        printf(" -- Efficiency distribution (%u samples) --\n", r.dist_samples);
        printf("   min/max:          %8.2f / %8.2f %%\n", r.eff_min, r.eff_max);
        printf("   mean/median:      %8.2f / %8.2f %%\n", r.eff_mean, r.eff_median);
        printf("   stddev:           %8.2f %%\n", r.eff_stddev);
    };
    printf("-----------------------------\n\n");
}

/**
 * @brief Vezme jeden vzorek okamžité efektivity do ring bufferu (vrstva B).
 *
 * Z přírůstku kumulativních součtů (pxCLK, wall) od posledního vzorku spočítá
 * okamžitou efektivitu % a uloží ji. Protože kumulativní součty rostou jen
 * v aktivních segmentech, přírůstek automaticky odpovídá jen reálně
 * odpracovanému času (NORMAL/pauza/stall jsou vyloučeny - jejich přírůstek
 * je nulový). První vzorek po resetu (cold start) se jen zapamatuje jako
 * výchozí bod a zahodí. Vzorky s příliš krátkým aktivním oknem (méně než
 * polovina vzorkovací periody) se přeskočí, aby distribuce nebyla zašuměná
 * okrajovými mikrovzorky.
 *
 * @param ms Stav benchmarku. Thread-safe (drží @c ms->mutex).
 */
static void emulator_measuring_maxspeed_take_sample(st_EMULATOR_MEASURING_MAXSPEED *ms)
{
    g_mutex_lock(&ms->mutex);

    uint64_t ticks, wall_us;
    maxspeed_current_totals_locked(ms, &ticks, &wall_us);

    if (!ms->have_last_sample)
    {
        // Cold start - jen zapamatuj výchozí bod, vzorek zahoď
        ms->last_sample_ticks = ticks;
        ms->last_sample_wall_us = wall_us;
        ms->have_last_sample = true;
        g_mutex_unlock(&ms->mutex);
        return;
    };

    uint64_t dticks = ticks - ms->last_sample_ticks;
    uint64_t dwall = wall_us - ms->last_sample_wall_us;
    ms->last_sample_ticks = ticks;
    ms->last_sample_wall_us = wall_us;

    // Vzorkuj jen pokud aktivní okno pokrylo aspoň půlku periody (omezení šumu)
    if (dwall >= (uint64_t)ms->sample_interval_us / 2)
    {
        double eff = ((double)dticks / ((double)dwall / 1e6)) / (double)g_mzhal.gdgclk_real_base * 100.0;
        ms->samples[ms->samples_head % MAX_MAXSPEED_BENCH_SAMPLES] = eff;
        ms->samples_head++;
        if ((ms->samples_head - ms->samples_tail) > MAX_MAXSPEED_BENCH_SAMPLES)
            ms->samples_tail++;
    };

    g_mutex_unlock(&ms->mutex);
}

/**
 * @brief Vlákno periodicky vzorkující okamžitou efektivitu (vrstva B).
 *
 * Probouzí se každých @c sample_interval_us a zaznamená jeden vzorek. Běží
 * od @ref emulator_measuring_maxspeed_init do @ref emulator_measuring_maxspeed_exit.
 * Ukončení signalizuje @c sample_thread_run == false + @c sample_cond.
 *
 * @param ms Stav benchmarku.
 * @return Vždy NULL.
 */
static gpointer emulator_measuring_maxspeed_sample_thread(st_EMULATOR_MEASURING_MAXSPEED *ms)
{
    /* Throttle periodického konzolového reportu (--maxspeed-bench): tiskni
     * jednou za N vzorků, ne každý vzorek. */
    const unsigned print_every_n = 5;
    unsigned print_cntr = 0;

    while (1)
    {
        g_mutex_lock(&ms->sample_ctrl_mutex);
        gint64 deadline = g_get_monotonic_time() + ms->sample_interval_us;
        // Čekej do deadline; g_cond_wait_until vrací TRUE při signálu (-> re-check)
        while (ms->sample_thread_run &&
               g_cond_wait_until(&ms->sample_cond, &ms->sample_ctrl_mutex, deadline))
        {
            /* probuzeno signálem před deadline - smyčka znovu zkontroluje run */
        }
        bool run = ms->sample_thread_run;
        g_mutex_unlock(&ms->sample_ctrl_mutex);

        if (!run)
            break;

        emulator_measuring_maxspeed_take_sample(ms);

        // Periodický konzolový report (zapnut přes --maxspeed-bench)
        if (ms->console_output_enabled)
        {
            print_cntr++;
            if (print_cntr >= print_every_n)
            {
                print_cntr = 0;
                emulator_measuring_maxspeed_print();
            };
        };
    }
    return NULL;
}
