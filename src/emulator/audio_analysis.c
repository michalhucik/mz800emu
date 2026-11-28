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


/**
 * @brief k-tý nejmenší prvek pole (výběr Hoareovým dělením, průměrně O(n)).
 *
 * Dává stejnou hodnotu jako prvek k seřazeného pole (dřív qsort() celého
 * pole skóre; výběr je u krátké cesty se 7000 posuny podstatně levnější).
 *
 * @param x Pole (přeuspořádá se), `n` prvků.
 * @param n Počet prvků (> k).
 * @param k Index v pořadí od nejmenšího.
 * @return Hodnota k-tého nejmenšího prvku.
 */
static double select_kth(double *x, size_t n, size_t k)
{
    size_t lo = 0, hi = n - 1;
    while (lo < hi) {
        const double piv = x[lo + (hi - lo) / 2];
        size_t i = lo, j = hi;
        while (i <= j) {
            while (x[i] < piv) i++;
            while (x[j] > piv) j--;
            if (i <= j) {
                const double t = x[i]; x[i] = x[j]; x[j] = t;
                i++;
                if (j == 0) break;
                j--;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    return x[k];
}


/**
 * @brief Tolerance "stejně dobrého" minima jako podíl mediánu skóre (heuristika).
 *
 * Jiné lokální minimum, které je od nejlepšího skóre nejvýš o tento podíl
 * mediánu skóre všech posunů, data od nejlepšího nerozliší (periodický tón
 * v krátkém úseku, ustálený tón, opakovaný úsek, rovina). Hodnota je volba
 * emulátoru podle měření na MZ-800 (sondy rev34 v exact-clock/mereni):
 * ustálený tón CTC0 (2644 vzorků) má nejbližší vzdálené minimum obálky
 * 0,03 % mediánu nad nejlepším, tón PSG s nástupem (test mcp_audio_read)
 * 8,8 %; ne odvozená mez.
 */
#define AUDIO_COMPARE_TIE_TOL 0.01


/**
 * @brief Rozhodne, zda skóre posunů má jednoznačné nejlepší minimum.
 *
 * Nejednoznačné je, když (1) medián skóre není kladný (všechny posuny stejně
 * dobré, typicky konstantní nebo nulový vstup), (2) nejlepší skóre není
 * výrazné (aspoň polovina mediánu) nebo (3) jiné lokální minimum (skóre
 * nejvýš rovné oběma sousedům; na krajích jednomu) dál než `guard` indexů
 * od nejlepšího je od nejlepšího skóre nejvýš o AUDIO_COMPARE_TIE_TOL
 * mediánu - stejně dobrá minima i rovina (konstantní nenulový rozdíl).
 *
 * `guard` vynechá minima v okolí nejlepšího, která jsou jen rozmazáním
 * téhož rysu: obálka (průměr přes 256 vzorků) má u tónu s nástupem vedlejší
 * minima o periodu tónu vedle (u tónu PSG 254 o 96 vzorků, 1,5 % mediánu);
 * minima v pásmu posoudí audio_compare() na původních vzorcích
 * (near_candidate_ties()). Nad surovým signálem se nevynechává nic (minima
 * po periodách tónu jsou rovnocenné posuny).
 *
 * @param sc    Skóre posunů v pořadí posunů (průměrný čtverec rozdílu), `n` položek.
 * @param n     Počet skóre (> 0).
 * @param best  Index nejlepšího (nejmenšího) skóre.
 * @param guard Kolik indexů na každou stranu od `best` se jako jiné minimum nebere.
 * @return true = posun z těchto skóre určit nejde; true i při nedostatku
 *         paměti na výpočet mediánu (nelze rozhodnout).
 */
static bool lag_scores_ambiguous(const double *sc, size_t n, size_t best, size_t guard)
{
    if (n == 0) return true;
    double *srt = (double *) malloc(n * sizeof(double));
    if (!srt) return true;
    memcpy(srt, sc, n * sizeof(double));
    const double med = select_kth(srt, n, n / 2);
    free(srt);
    const double bv = sc[best];
    if (!(med > 0.0) || bv >= 0.5 * med) return true;
    const double tol = AUDIO_COMPARE_TIE_TOL * med;
    for (size_t l = 0; l < n; l++) {
        const size_t dist = (l > best) ? l - best : best - l;
        if (dist == 0 || dist <= guard) continue;
        const bool left = (l == 0) || sc[l] <= sc[l - 1];
        const bool right = (l + 1 == n) || sc[l] <= sc[l + 1];
        if (left && right && sc[l] - bv <= tol) return true;
    }
    return false;
}


/**
 * @brief Skóre (průměrný čtverec rozdílu) všech posunů [lo, hi] až do prvního bez překryvu.
 *
 * @param a      První signál (mono), `na` vzorků.
 * @param na     Počet vzorků a.
 * @param b      Druhý signál (mono), `nb` vzorků.
 * @param nb     Počet vzorků b.
 * @param lo     První posun.
 * @param hi     Poslední posun (>= lo).
 * @param window Nejvýš porovnaných vzorků (0 = vše).
 * @param out_n  Výstup: počet skóre (posuny lo .. lo + out_n - 1).
 * @return Nové pole (free volající), NULL = nedostatek paměti (out_n 0).
 */
static double *lag_scores(const double *a, size_t na, const double *b, size_t nb,
                          size_t lo, size_t hi, size_t window, size_t *out_n)
{
    *out_n = 0;
    double *sc = (double *) malloc((hi - lo + 1) * sizeof(double));
    if (!sc) return NULL;
    size_t k = 0;
    for (size_t l = lo; l <= hi; l++) {
        size_t n = 0;
        double v = mean_sq(a, na, b, nb, l, window, &n);
        if (n == 0) break;
        sc[k++] = v;
    }
    *out_n = k;
    return sc;
}


/**
 * @brief Relativní tolerance shody dvou posunů na původních vzorcích (heuristika).
 *
 * Jiné samostatné minimum surového rozdílu uvnitř ochranného pásma obálky je
 * "stejně dobré", když jeho průměrný čtverec rozdílu nepřesáhne
 * (1 + AUDIO_COMPARE_RAW_TIE) násobek nejlepšího (plus AUDIO_COMPARE_RAW_EPS
 * kvůli zaokrouhlení u přesných kopií, kde jsou obě skóre 0). Hodnota je
 * volba emulátoru podle měření (sonda rev35_blizke_kandidaty.py
 * v exact-clock/mereni): u tónu PSG s nástupem (scénář testu
 * mcp_audio_read, zpoždění 881) mají minima o periodu tónu vedle (781, 982)
 * 3,6krát a další (681, 1082) 8,5krát větší skóre než nejlepší; ne odvozená
 * mez.
 */
#define AUDIO_COMPARE_RAW_TIE 0.05

/**
 * @brief Přesné hledání (krátká porovnání): nejlepší skóre nad tímto podílem součtu energií obou úseků = nejednoznačné (heuristika).
 *
 * Dva nezávislé signály mají průměrný čtverec rozdílu přibližně součet
 * svých energií při každém posunu; mezi několika desítkami posunů pak
 * některý náhodně vyjde pod polovinou mediánu (sonda posudku 0374ca6a:
 * 32 vzorků, posun 13 z 19 dvojic). Skutečné zarovnání musí rozdíl
 * podstatně zmenšit; hodnota 0,25 (aspoň tři čtvrtiny energie vysvětleny)
 * je volba emulátoru, ne odvozená mez. U obálky se nepoužívá: tam se SDL
 * cesta s referencí v ustálené části ve fázi shodovat nemusí.
 */
#define AUDIO_COMPARE_SHORT_FIT 0.25

/**
 * @brief Nejvýš práce (páry krát zkoušené posuny), pro kterou audio_compare() hledá posun přesně nad původními vzorky.
 *
 * Přesné hledání spočítá průměrný čtverec rozdílu pro každý posun 0..max_lag
 * (cena úměrná párům krát posunům) a najde i rovnocenná zarovnání, která
 * obálka s hrubým krokem 16 vzorků a zpřesněním ±8 mine (posudek 4c69d5cc:
 * sinus s periodou 28 vzorků a kopiemi při posunu 7 a 35 vrátil posun 13
 * s RMS 0,19 jako jistý). Mez 2048 párů při 7051 posunech (výchozích
 * 8 snímků PAL) je volba podle ceny změřené na tomto vývojovém počítači
 * (sonda rev36_cena_delky.py v exact-clock/mereni: 882 párů 3,7 ms, 2048
 * párů 8,3 ms, 4096 párů 16,6 ms); s větším max_lag se přesně hledá kratší
 * okno, cena tak zůstává v jednotkách milisekund. Delší porovnání jdou přes
 * obálku jako dřív.
 */
#define AUDIO_COMPARE_EXACT_WORK (2048u * 7051u)

/** @brief Absolutní rezerva shody na původních vzorcích (jednotky -1..1 na druhou). */
#define AUDIO_COMPARE_RAW_EPS 1e-12

/**
 * @brief Předvýběr kandidátů v ochranném pásmu: skóre obálky nejvýš o tento podíl mediánu nad nejlepším.
 *
 * Jen omezuje počet výpočtů na původních vzorcích; o shodě rozhoduje
 * AUDIO_COMPARE_RAW_TIE. U tónu s nástupem jsou vedlejší minima obálky
 * 1,5 % mediánu nad nejlepším, u přesné kopie 0 %.
 */
#define AUDIO_COMPARE_NEAR_PRESELECT 0.25


/**
 * @brief Průměrný čtverec rozdílu a[i] - b[i + lag] přes právě `n` párů.
 * @param a   První signál (mono).
 * @param b   Druhý signál (mono).
 * @param lag Posun b za a.
 * @param n   Počet párů.
 * @return Průměrný čtverec rozdílu.
 * @pre lag + n <= délka b, n <= délka a, n > 0.
 */
static double mean_sq_n(const double *a, const double *b, size_t lag, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double d = a[i] - b[i + lag];
        s += d * d;
    }
    return s / (double) n;
}


/**
 * @brief Má kandidát obálky blízko zvoleného posunu stejně dobré samostatné minimum na původních vzorcích?
 *
 * Hrubý posun kandidáta `cand` se zpřesní na původních vzorcích v okolí
 * ±`refine`. Za samostatné minimum (jinou "dolinu" než zvolený posun) se
 * uzná, jen když je nalezený posun od `best` dál než `refine` vzorků a jeho
 * skóre není větší než skóre sousedních posunů (i těch za okrajem okolí).
 * Oba posuny se porovnají při stejné délce překryvu `n`.
 *
 * @param ma      První signál (mono).
 * @param mb      Druhý signál (mono), `nb` vzorků.
 * @param nb      Počet vzorků mb.
 * @param best    Zvolený (zpřesněný) posun.
 * @param cand    Hrubý posun kandidáta.
 * @param max_lag Největší dovolený posun.
 * @param refine  Poloměr zpřesnění [vzorky].
 * @param n       Počet párů pro všechny posuny.
 * @return true = jiný posun je na původních vzorcích stejně dobrý.
 * @pre max_lag + 1 + n <= nb, n > 0, best <= max_lag.
 */
static bool near_candidate_ties(const double *ma, const double *mb, size_t nb,
                                size_t best, size_t cand, size_t max_lag, size_t refine, size_t n)
{
    const size_t lo = (cand > refine) ? cand - refine : 0;
    size_t hi = cand + refine;
    if (hi > max_lag) hi = max_lag;
    if (lo > hi) return false;
    size_t l2 = lo;
    double v2 = mean_sq_n(ma, mb, lo, n);
    for (size_t l = lo + 1; l <= hi; l++) {
        const double v = mean_sq_n(ma, mb, l, n);
        if (v < v2) {
            v2 = v;
            l2 = l;
        }
    }
    const size_t dist = (l2 > best) ? l2 - best : best - l2;
    if (dist <= refine) return false;                     /* táž dolina jako zvolený posun */
    if (l2 > 0 && mean_sq_n(ma, mb, l2 - 1, n) < v2) return false;
    if (l2 + 1 + n <= nb && mean_sq_n(ma, mb, l2 + 1, n) < v2) return false;
    const double v1 = mean_sq_n(ma, mb, best, n);
    return v2 <= v1 * (1.0 + AUDIO_COMPARE_RAW_TIE) + AUDIO_COMPARE_RAW_EPS;
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
    /* Kandidát musí mít aspoň polovinu největšího možného překryvu (při
     * posunu 0): posun, při kterém zbývá jen pár dvojic, by vyhrál náhodou
     * (jedna shodná dvojice = skóre 0). S polovinou stojí každý posun aspoň
     * na polovině týchž dat jako posun 0; při volání ze souhrnu (b o max_lag
     * delší než a) mez nic neubere. */
    size_t maxov = (na < nb) ? na : nb;
    if (window && maxov > window) maxov = window;
    const size_t min_pairs = (maxov + 1u) / 2u;
    if ((size_t) max_lag > nb - min_pairs) max_lag = (int) (nb - min_pairs);
    double *ma = (double *) malloc(na * sizeof(double));
    double *mb = (double *) malloc(nb * sizeof(double));
    if (!ma || !mb) {
        free(ma); free(mb);
        return false;
    }
    for (size_t i = 0; i < na; i++) ma[i] = mono_at(a, i);
    for (size_t i = 0; i < nb; i++) mb[i] = mono_at(b, i);

    /* Krátká okna (práce do AUDIO_COMPARE_EXACT_WORK) se hledají přesně nad původními
     * vzorky přes všechny posuny (i rovnocenná zarovnání mimo mřížku obálky);
     * delší přes obálku s hrubým krokem. */
    const size_t cmp_n = (window && window < na) ? window : na;
    const bool exact = (cmp_n < ENV)
                       || ((uint64_t) cmp_n * ((uint64_t) max_lag + 1u) <= (uint64_t) AUDIO_COMPARE_EXACT_WORK);
    size_t nea = 0, neb = 0, nda = 0, ndb = 0;
    double *ea = exact ? NULL : envelope(ma, na, ENV, &nea);
    double *eb = exact ? NULL : envelope(mb, nb, ENV, &neb);
    double *da = ea ? decimate(ea, nea, DEC, &nda) : NULL;
    double *db = eb ? decimate(eb, neb, DEC, &ndb) : NULL;
    int lag = -1;
    bool ambiguous = false;
    double *sc = NULL;                     /* hrubá skóre obálky (i pro kandidáty v ochranném pásmu) */
    size_t nsc = 0, best_i = 0;
    if (da && db && nda > 0 && ndb > 0) {
        /* Hrubě nad obálkou po DEC vzorcích; jednoznačné = výrazné minimum
         * (nejlepší skóre pod polovinou mediánu skóre všech posunů) bez
         * jiného stejně dobrého minima dál než ENV vzorků
         * (lag_scores_ambiguous()); bližší minima posoudí původní vzorky. */
        size_t ncoarse = (size_t) max_lag / DEC + 1;
        sc = (double *) malloc(ncoarse * sizeof(double));
        size_t best_c = 0;
        double bv = -1.0;
        size_t wdec = window ? (window / DEC + 1) : 0;
        for (size_t l = 0; sc && l < ncoarse; l++) {
            size_t n = 0;
            double v = mean_sq(da, nda, db, ndb, l, wdec, &n);
            if (n == 0) break;
            sc[nsc++] = v;
            if (bv < 0.0 || v < bv) {
                bv = v;
                best_i = l;
                best_c = l * DEC;
            }
        }
        if (nsc > 0) {
            ambiguous = lag_scores_ambiguous(sc, nsc, best_i, ENV / DEC);
            size_t lo = (best_c > FINE) ? best_c - FINE : 0;
            size_t hi = best_c + FINE;
            if (hi > (size_t) max_lag) hi = (size_t) max_lag;
            lag = best_lag(ea, nea, eb, neb, lo, hi, window);
        }
    }
    if (lag >= 0) {
        size_t lo = ((size_t) lag > REFINE) ? (size_t) lag - REFINE : 0;
        size_t hi = (size_t) lag + REFINE;
        if (hi > (size_t) max_lag) hi = (size_t) max_lag;
        int l2 = best_lag(ma, na, mb, nb, lo, hi, window);
        if (l2 >= 0) lag = l2;
        /* Kandidáti v ochranném pásmu obálky (do ENV vzorků od nejlepšího):
         * obálka tam jen rozmazává, rozhodnou původní vzorky při stejné délce
         * překryvu (dvě přesné kopie blízko sebe = nejednoznačné, nástup tónu
         * o periodu vedle = horší, zůstane jednoznačné). */
        if (!ambiguous && nsc > 0) {
            size_t n = na;
            if (window && n > window) n = window;
            const size_t lim = (size_t) max_lag + 1u;      /* i soused za posledním posunem */
            n = (nb > lim) ? ((nb - lim < n) ? nb - lim : n) : 0;
            double *tmp = (n > 0) ? (double *) malloc(nsc * sizeof(double)) : NULL;
            if (tmp) {
                memcpy(tmp, sc, nsc * sizeof(double));
                const double med = select_kth(tmp, nsc, nsc / 2);
                free(tmp);
                const double bv = sc[best_i];
                for (size_t j = 0; j < nsc && !ambiguous; j++) {
                    const size_t dist = (j > best_i) ? j - best_i : best_i - j;
                    if (dist == 0 || dist > ENV / DEC) continue;
                    const bool left = (j == 0) || sc[j] <= sc[j - 1];
                    const bool right = (j + 1 == nsc) || sc[j] <= sc[j + 1];
                    if (!left || !right || sc[j] - bv > AUDIO_COMPARE_NEAR_PRESELECT * med) continue;
                    ambiguous = near_candidate_ties(ma, mb, nb, (size_t) lag, j * DEC, (size_t) max_lag,
                                                    REFINE, n);
                }
            } else if (n > 0) {
                ambiguous = true;                             /* bez paměti nelze rozhodnout */
            }
        }
    } else {
        /* Přesné hledání (práce do AUDIO_COMPARE_EXACT_WORK nebo okno kratší než okno
         * obálky): skóre surového signálu pro všechny dovolené posuny; posun = první
         * nejmenší skóre (jako best_lag()), jednoznačnost stejným pravidlem jako
         * u obálky, ale bez ochranného pásma (každé jiné minimum, i o vzorek vedle,
         * je samostatný kandidát). Bez paměti na skóre posun přes best_lag()
         * a jednoznačnost se neposoudí (nejednoznačné). */
        size_t nrs = 0;
        double *rs = lag_scores(ma, na, mb, nb, 0, (size_t) max_lag, window, &nrs);
        if (rs) {
            size_t bi = 0;
            for (size_t l = 1; l < nrs; l++) {
                if (rs[l] < rs[bi]) bi = l;
            }
            if (nrs > 0) {
                lag = (int) bi;
                ambiguous = lag_scores_ambiguous(rs, nrs, bi, 0);
                /* Krátká cesta rozhoduje na původních vzorcích: skutečné zarovnání
                 * musí rozdíl zmenšit pod AUDIO_COMPARE_SHORT_FIT součtu energií
                 * obou úseků (nezávislé signály dají skóre asi rovné součtu). */
                size_t n = na;
                if (window && n > window) n = window;
                if (nb - bi < n) n = nb - bi;
                double ea2 = 0.0, eb2 = 0.0;
                for (size_t i = 0; i < n; i++) {
                    ea2 += ma[i] * ma[i];
                    eb2 += mb[i + bi] * mb[i + bi];
                }
                if (n && rs[bi] > AUDIO_COMPARE_SHORT_FIT * (ea2 + eb2) / (double) n) ambiguous = true;
            }
            free(rs);
        } else {
            lag = best_lag(ma, na, mb, nb, 0, (size_t) max_lag, window);
            ambiguous = true;
        }
    }
    free(sc);
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
