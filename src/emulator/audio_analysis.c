/**
 * @file   audio_analysis.c
 * @brief  Čisté funkce pro rozbor zvuku - implementace (viz audio_analysis.h).
 *
 * @par Licence: GPLv3
 */

#include "audio_analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


double audio_psg_tone_hz(unsigned divider, uint64_t gdg_hz, unsigned cpu_divider)
{
    if (divider == 1u || cpu_divider == 0) return 0.0;
    if (divider == 0u) divider = 0x400u;   /* SN76489AN: dělič 000h se chová jako 1024 */
    return (double) gdg_hz / (32.0 * (double) divider * (double) cpu_divider);
}


bool audio_hz_to_midi(double hz, int *out_pitch, int *out_cents)
{
    *out_pitch = 0;
    *out_cents = 0;
    if (!(hz > 0.0)) return false;
    double midi = 12.0 * log2(hz / 440.0) + 69.0;
    int nearest = (int) lround(midi);
    int cents = (int) lround((midi - (double) nearest) * 100.0);
    if (nearest < 0) nearest = 0;
    if (nearest > 127) nearest = 127;
    if (cents < -50) cents = -50;
    if (cents > 50) cents = 50;
    *out_pitch = nearest;
    *out_cents = cents;
    return true;
}


void audio_midi_note_name(int pitch, char *buf, size_t buflen)
{
    /* Standardní jména not (MIDI konvence), záměrně bez překladu. */
    static const char *const names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    if (!buf || buflen == 0) return;
    if (pitch < 0) {
        snprintf(buf, buflen, "noise");
    } else if (pitch > 127) {
        snprintf(buf, buflen, "?");
    } else {
        snprintf(buf, buflen, "%s%d", names[pitch % 12], pitch / 12 - 1);
    }
}


bool audio_psg_is_playing(unsigned attn, unsigned divider, bool noise)
{
    if (attn >= AUDIO_PSG_ATTN_OFF) return false;
    return noise || divider != 1u;
}


size_t audio_spans(const uint8_t *flags, size_t n, uint64_t base, bool value,
                   st_AUDIO_SPAN *out, size_t max, size_t *total)
{
    size_t written = 0, count = 0;
    size_t i = 0;
    while (i < n) {
        if ((flags[i] != 0) != value) {
            i++;
            continue;
        }
        size_t j = i;
        while (j + 1 < n && (flags[j + 1] != 0) == value) j++;
        if (written < max && out) {
            out[written].from = base + i;
            out[written].to = base + j;
            written++;
        }
        count++;
        i = j + 1;
    }
    if (total) *total = count;
    return written;
}


void audio_hist_add(uint32_t *hist, double us)
{
    unsigned b = 0;
    double lim = 2.0;
    while (b + 1 < AUDIO_HIST_BUCKETS && us >= lim) {
        b++;
        lim *= 2.0;
    }
    hist[b]++;
}


uint32_t audio_hist_bucket_us(unsigned bucket)
{
    if (bucket == 0 || bucket >= AUDIO_HIST_BUCKETS) return 0;
    return 1u << bucket;
}


const char *audio_ctc0_guess(const uint32_t *hist, uint64_t edges, double seconds, double *out_share)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < AUDIO_HIST_BUCKETS; i++) total += hist[i];
    double share = 0.0;
    if (total > 0) {
        uint64_t best = 0;
        for (unsigned i = 0; i < AUDIO_HIST_BUCKETS; i++) {
            uint64_t pair = (uint64_t) hist[i] + ((i + 1 < AUDIO_HIST_BUCKETS) ? hist[i + 1] : 0);
            if (pair > best) best = pair;
        }
        share = (double) best / (double) total;
    }
    if (out_share) *out_share = share;
    if (edges == 0) return "silent";
    if (total > 0 && share >= 0.8) return "tone";
    if (seconds > 0.0 && (double) edges / seconds >= 2000.0) return "pcm";
    return "irregular";
}


size_t audio_resample_frames(size_t in_frames, unsigned in_rate, unsigned out_rate)
{
    if (in_rate == 0 || out_rate == 0) return 0;
    return (size_t) (((uint64_t) in_frames * out_rate) / in_rate);
}


size_t audio_resample_s16(const int16_t *in, size_t in_frames, unsigned in_rate,
                          unsigned out_rate, bool stereo, int16_t *out)
{
    size_t n = audio_resample_frames(in_frames, in_rate, out_rate);
    for (size_t j = 0; j < n; j++) {
        uint64_t a = ((uint64_t) j * in_rate) / out_rate;
        uint64_t b = ((uint64_t) (j + 1) * in_rate) / out_rate;
        if (b <= a) b = a + 1;
        if (b > in_frames) b = in_frames;
        int64_t sl = 0, sr = 0;
        for (uint64_t k = a; k < b; k++) {
            sl += in[k * 2u];
            sr += in[k * 2u + 1u];
        }
        int64_t cnt = (int64_t) (b - a);
        if (stereo) {
            out[j * 2u] = (int16_t) (sl / cnt);
            out[j * 2u + 1u] = (int16_t) (sr / cnt);
        } else {
            out[j] = (int16_t) ((sl + sr) / (2 * cnt));
        }
    }
    return n;
}


void audio_wav_header(uint8_t h[44], unsigned rate, unsigned channels, uint64_t frames)
{
    uint64_t bytes = frames * channels * 2u;
    uint32_t data = (uint32_t) ((bytes > 0xFFFFFFF0u) ? 0xFFFFFFF0u : bytes);
    uint32_t byte_rate = rate * channels * 2u;
    uint32_t fields[] = { 36u + data, 16u, rate, byte_rate, data };
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 36, "data", 4);
    const unsigned at[] = { 4, 16, 24, 28, 40 };
    for (unsigned f = 0; f < 5; f++) {
        for (int i = 0; i < 4; i++) h[at[f] + (unsigned) i] = (uint8_t) (fields[f] >> (8 * i));
    }
    h[20] = 1; h[21] = 0;                                   /* PCM */
    h[22] = (uint8_t) channels; h[23] = 0;
    h[32] = (uint8_t) (channels * 2u); h[33] = 0;           /* block align */
    h[34] = 16; h[35] = 0;                                  /* bitů na vzorek */
}


/** @brief Mono průměr páru L,R v jednotkách -1..1. */
static inline double mono_at(const int16_t *s, size_t i)
{
    return ((double) s[i * 2u] + (double) s[i * 2u + 1u]) / 65536.0;
}


/** @brief Průměrný čtverec rozdílu a[i] - b[i + lag] pro i v [0, n). */
static double mean_sq(const double *a, size_t na, const double *b, size_t nb,
                      size_t lag, size_t window, size_t *out_n)
{
    *out_n = 0;
    if (lag >= nb) return -1.0;
    size_t n = na;
    if (nb - lag < n) n = nb - lag;
    if (window && n > window) n = window;
    if (n == 0) return -1.0;
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = a[i] - b[i + lag];
        s += d * d;
    }
    *out_n = n;
    return s / (double) n;
}


/**
 * @brief Obálka signálu: průměr |x| přes okno `w` vzorků od indexu i (délka n - w + 1).
 * @return Nové pole (free volající), NULL = nedostatek paměti nebo n < w.
 */
static double *envelope(const double *x, size_t n, size_t w, size_t *out_n)
{
    *out_n = 0;
    if (n < w || w == 0) return NULL;
    size_t m = n - w + 1;
    double *e = (double *) malloc(m * sizeof(double));
    if (!e) return NULL;
    double s = 0.0;
    for (size_t i = 0; i < w; i++) s += fabs(x[i]);
    for (size_t i = 0; i < m; i++) {
        e[i] = s / (double) w;
        if (i + w < n) s += fabs(x[i + w]) - fabs(x[i]);
    }
    *out_n = m;
    return e;
}


/** @brief Průměry po `dec` vzorcích (délka n / dec). */
static double *decimate(const double *x, size_t n, size_t dec, size_t *out_n)
{
    size_t m = n / dec;
    double *d = (double *) malloc((m + 1) * sizeof(double));
    if (!d) return NULL;
    for (size_t k = 0; k < m; k++) {
        double s = 0.0;
        for (size_t i = 0; i < dec; i++) s += x[k * dec + i];
        d[k] = s / (double) dec;
    }
    *out_n = m;
    return d;
}


/**
 * @brief Posun s nejmenším průměrným čtvercem rozdílu v rozsahu [lo, hi].
 * @return Posun, nebo -1 (signály se nepřekrývají).
 */
static int best_lag(const double *a, size_t na, const double *b, size_t nb,
                    size_t lo, size_t hi, size_t window)
{
    int best = -1;
    double bv = -1.0;
    for (size_t l = lo; l <= hi; l++) {
        size_t n = 0;
        double v = mean_sq(a, na, b, nb, l, window, &n);
        if (n == 0) break;
        if (bv < 0.0 || v < bv) {
            bv = v;
            best = (int) l;
        }
    }
    return best;
}


/** @brief Pořadí double pro qsort. */
static int cmp_double(const void *x, const void *y)
{
    double a = *(const double *) x, b = *(const double *) y;
    return (a > b) - (a < b);
}


bool audio_compare(const int16_t *a, size_t na, const int16_t *b, size_t nb,
                   int max_lag, size_t window, st_AUDIO_COMPARE *out)
{
    memset(out, 0, sizeof(*out));
    if (!a || !b || na == 0 || nb == 0 || max_lag < 0) return false;
    /* Posun se hledá podle obálky (průměr |x| přes ENV vzorků): ustálený
     * tón má obálku plochou, nástupy a změny hlasitosti ji lámou, takže
     * minimum je jednoznačné i u periodického signálu, kde surový rozdíl
     * má minima po periodách (SDL cesta není s referencí v ustálené části
     * přesně ve fázi). Pak zpřesnění na surovém signálu v okolí ±REFINE. */
    enum { ENV = 256, DEC = 16, FINE = 32, REFINE = 8 };
    double *ma = (double *) malloc(na * sizeof(double));
    double *mb = (double *) malloc(nb * sizeof(double));
    if (!ma || !mb) {
        free(ma); free(mb);
        return false;
    }
    for (size_t i = 0; i < na; i++) ma[i] = mono_at(a, i);
    for (size_t i = 0; i < nb; i++) mb[i] = mono_at(b, i);

    size_t nea = 0, neb = 0, nda = 0, ndb = 0;
    double *ea = envelope(ma, na, ENV, &nea);
    double *eb = envelope(mb, nb, ENV, &neb);
    double *da = ea ? decimate(ea, nea, DEC, &nda) : NULL;
    double *db = eb ? decimate(eb, neb, DEC, &ndb) : NULL;
    int lag = -1;
    bool ambiguous = false;
    if (da && db && nda > 0 && ndb > 0) {
        /* Hrubě nad obálkou po DEC vzorcích; výrazné minimum = nejlepší
         * skóre pod polovinou mediánu skóre všech posunů. */
        size_t ncoarse = (size_t) max_lag / DEC + 1;
        double *sc = (double *) malloc(ncoarse * sizeof(double));
        size_t nsc = 0, best_c = 0;
        double bv = -1.0;
        size_t wdec = window ? (window / DEC + 1) : 0;
        for (size_t l = 0; sc && l < ncoarse; l++) {
            size_t n = 0;
            double v = mean_sq(da, nda, db, ndb, l, wdec, &n);
            if (n == 0) break;
            sc[nsc++] = v;
            if (bv < 0.0 || v < bv) {
                bv = v;
                best_c = l * DEC;
            }
        }
        if (nsc > 0) {
            qsort(sc, nsc, sizeof(double), cmp_double);
            double med = sc[nsc / 2];
            ambiguous = !(med > 0.0) || bv >= 0.5 * med;
            size_t lo = (best_c > FINE) ? best_c - FINE : 0;
            size_t hi = best_c + FINE;
            if (hi > (size_t) max_lag) hi = (size_t) max_lag;
            lag = best_lag(ea, nea, eb, neb, lo, hi, window);
        }
        free(sc);
    }
    if (lag >= 0) {
        size_t lo = ((size_t) lag > REFINE) ? (size_t) lag - REFINE : 0;
        size_t hi = (size_t) lag + REFINE;
        if (hi > (size_t) max_lag) hi = (size_t) max_lag;
        int l2 = best_lag(ma, na, mb, nb, lo, hi, window);
        if (l2 >= 0) lag = l2;
    } else {
        /* Krátké signály (pod okno obálky): přímo nad surovým signálem. */
        lag = best_lag(ma, na, mb, nb, 0, (size_t) max_lag, window);
    }
    free(ma); free(mb); free(ea); free(eb); free(da); free(db);
    if (lag < 0) return false;
    audio_diff(a, na, b, nb, lag, 0, window ? window : na, out);
    out->lag_ambiguous = ambiguous;
    return out->compared > 0;
}


void audio_diff(const int16_t *a, size_t na, const int16_t *b, size_t nb,
                int lag, size_t from, size_t count, st_AUDIO_COMPARE *out)
{
    memset(out, 0, sizeof(*out));
    out->lag = lag;
    if (!a || !b || lag < 0 || from >= na || (size_t) lag + from >= nb) return;
    size_t n = na - from;
    if (nb - (size_t) lag - from < n) n = nb - (size_t) lag - from;
    if (count < n) n = count;
    double s = 0.0, sa = 0.0, mx = 0.0;
    for (size_t i = from; i < from + n; i++) {
        double x = mono_at(a, i);
        double d = x - mono_at(b, i + (size_t) lag);
        s += d * d;
        sa += x * x;
        if (fabs(d) > mx) mx = fabs(d);
    }
    out->compared = n;
    if (n) {
        out->diff_rms = sqrt(s / (double) n);
        out->a_rms = sqrt(sa / (double) n);
    }
    out->diff_max = mx;
}


void audio_note_track_init(st_AUDIO_NOTE_TRACK *t, unsigned divider, unsigned attn,
                           bool noise, uint64_t frame)
{
    memset(t, 0, sizeof(*t));
    t->divider = divider;
    t->attn = attn;
    t->noise = noise;
    t->playing = audio_psg_is_playing(attn, divider, noise);
    t->from_frame = frame;
    t->min_attn = attn;
}


/** @brief Vyplní dokončenou notu z rozehraného stavu. */
static void note_done(const st_AUDIO_NOTE_TRACK *t, uint64_t to_frame, st_AUDIO_NOTE *done)
{
    done->divider = t->noise ? 0 : t->divider;
    done->noise = t->noise;
    done->max_level = AUDIO_PSG_ATTN_OFF - t->min_attn;
    done->from_frame = t->from_frame;
    done->to_frame = (to_frame < t->from_frame) ? t->from_frame : to_frame;
}


bool audio_note_track_update(st_AUDIO_NOTE_TRACK *t, unsigned divider, unsigned attn,
                             bool noise, uint64_t frame, st_AUDIO_NOTE *done)
{
    bool playing = audio_psg_is_playing(attn, divider, noise);
    bool pitch_changed = (noise != t->noise) || (!noise && divider != t->divider);
    bool ended = false;
    if (t->playing && (!playing || pitch_changed)) {
        note_done(t, frame, done);
        ended = true;
    }
    if (playing && (!t->playing || pitch_changed)) {
        t->from_frame = frame;
        t->min_attn = attn;
    } else if (playing && attn < t->min_attn) {
        t->min_attn = attn;
    }
    t->divider = divider;
    t->attn = attn;
    t->noise = noise;
    t->playing = playing;
    return ended;
}


bool audio_note_track_finish(st_AUDIO_NOTE_TRACK *t, uint64_t last_frame, st_AUDIO_NOTE *done)
{
    if (!t->playing) return false;
    note_done(t, last_frame, done);
    t->playing = false;
    return true;
}
