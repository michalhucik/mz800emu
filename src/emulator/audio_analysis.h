/**
 * @file   audio_analysis.h
 * @brief  Čisté funkce pro rozbor zvuku: výška tónu PSG, noty, úseky,
 *         histogram intervalů hran, převzorkování, WAV, zpoždění dvou signálů.
 *
 * Modul nemá stav emulátoru ani zámky; všechny vstupy dostává parametry.
 * Používají ho PSG okna debuggeru (výška tónu a noty, aby UI i MCP počítaly
 * stejně) a MCP souhrn a PCM výřez zachyceného zvuku (audio_capture.h).
 *
 * Výška tónu PSG je vzorec z kódu emulátoru (dělič PSG -> frekvence při
 * daném taktu GDG a děliči GDG -> CPU), ne nezávisle ověřené chování
 * hardwaru. Heuristika audio_ctc0_guess() je odhad (viz její popis).
 *
 * @par Licence: GPLv3
 */

#ifndef AUDIO_ANALYSIS_H
#define AUDIO_ANALYSIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Útlum PSG, při kterém je kanál vypnutý (attn 15). */
#define AUDIO_PSG_ATTN_OFF 15u

/** @brief Počet košů histogramu intervalů (koš i = [2^i, 2^(i+1)) µs, koš 0 = pod 2 µs). */
#define AUDIO_HIST_BUCKETS 24u

/**
 * @brief Frekvence tónu PSG z děliče.
 *
 * Vzorec `gdg_hz / (32 * divider * cpu_divider)` (stejný jako dříve
 * v oknech PSG debuggeru): CPU takt = gdg_hz / cpu_divider, PSG dělí 16
 * a výstup se překlápí jednou za periodu děliče.
 *
 * @param divider     10bitový dělič tónu 0..1023.
 * @param gdg_hz      Takt GDG [Hz] (GDGCLK_BASE platformy).
 * @param cpu_divider Takty GDG na takt CPU (GDGCLK2CPU_DIVIDER).
 * @return Frekvence [Hz]; divider 0 se počítá jako 1024 (chování SN76489AN);
 *         0 pro divider 1 (emulátor drží stejnosměrný výstup) nebo nulový
 *         cpu_divider.
 */
double audio_psg_tone_hz(unsigned divider, uint64_t gdg_hz, unsigned cpu_divider);

/**
 * @brief Převede frekvenci na nejbližší notu MIDI a odchylku v centech.
 *
 * `midi = 12 * log2(hz / 440) + 69`, nejbližší celé číslo, centy
 * zaokrouhlené; MIDI omezeno na 0..127, centy na -50..+50.
 *
 * @param hz        Frekvence [Hz].
 * @param out_pitch Výstup: MIDI 0..127 (nesmí být NULL).
 * @param out_cents Výstup: centy -50..+50 (nesmí být NULL).
 * @return false = hz <= 0 (výstupy 0).
 */
bool audio_hz_to_midi(double hz, int *out_pitch, int *out_cents);

/**
 * @brief Jméno noty MIDI ("C4", "A#4"); MIDI konvence C4 = 60, A4 = 69.
 *
 * @param pitch  MIDI 0..127; záporné = "noise" (šumový kanál PSG), nad 127 = "?".
 * @param buf    Výstup (ASCII, ukončený nulou).
 * @param buflen Velikost bufferu (stačí 8).
 */
void audio_midi_note_name(int pitch, char *buf, size_t buflen);

/**
 * @brief Hraje kanál PSG slyšitelně? (definice detektoru not PSG Audio Scope)
 *
 * @param attn     Útlum 0..15 (15 = vypnuto).
 * @param divider  Dělič tónu (u šumu se nebere v úvahu).
 * @param noise    Kanál je šumový.
 * @return true = attn < 15 a (šum nebo divider >= 2).
 */
bool audio_psg_is_playing(unsigned attn, unsigned divider, bool noise);

/** @brief Souvislý úsek snímků [from, to] (oba včetně). */
typedef struct st_AUDIO_SPAN {
    uint64_t from; /**< První snímek úseku. */
    uint64_t to;   /**< Poslední snímek úseku (>= from). */
} st_AUDIO_SPAN;

/**
 * @brief Najde souvislé úseky snímků se zadanou hodnotou příznaku.
 *
 * @param flags   Příznak po snímcích (0 / nenulový), `n` položek.
 * @param n       Počet snímků.
 * @param base    Číslo snímku flags[0].
 * @param value   Hledaná hodnota (true = nenulový příznak).
 * @param out     Výstup úseků (smí být NULL, když max == 0).
 * @param max     Kapacita `out`.
 * @param total   Výstup: počet všech úseků (i těch, které se nevešly), smí být NULL.
 * @return Počet zapsaných úseků (<= max).
 */
size_t audio_spans(const uint8_t *flags, size_t n, uint64_t base, bool value,
                   st_AUDIO_SPAN *out, size_t max, size_t *total);

/**
 * @brief Přidá interval mezi hranami do histogramu (koše po mocninách 2 µs).
 *
 * @param hist Histogram AUDIO_HIST_BUCKETS košů.
 * @param us   Interval [µs] (>= 0); nad poslední koš padne do posledního.
 */
void audio_hist_add(uint32_t *hist, double us);

/**
 * @brief Dolní mez koše histogramu [µs] (0 pro koš 0, jinak 2^i).
 * @param bucket Index koše < AUDIO_HIST_BUCKETS.
 * @return Dolní mez [µs].
 */
uint32_t audio_hist_bucket_us(unsigned bucket);

/**
 * @brief Odhad, co dělá jednobitový výstup (reproduktor CTC0), z histogramu intervalů hran.
 *
 * Heuristika, ne fakt: "silent" = žádná hrana; "tone" = dva sousední koše
 * drží aspoň 80 % intervalů (jedna převládající perioda); "pcm" = jinak,
 * pokud je hran aspoň 2000 za sekundu (široké rozložení krátkých
 * intervalů, typicky 1bitové PCM); jinak "irregular".
 *
 * @param hist     Histogram AUDIO_HIST_BUCKETS košů.
 * @param edges    Počet hran v rozebíraném úseku.
 * @param seconds  Délka úseku [s].
 * @param out_share Výstup: podíl dvou nejsilnějších sousedních košů 0..1, smí být NULL.
 * @return Statický řetězec "silent", "tone", "pcm" nebo "irregular".
 */
const char *audio_ctc0_guess(const uint32_t *hist, uint64_t edges, double seconds, double *out_share);

/**
 * @brief Počet výstupních vzorků převzorkování.
 *
 * @param in_frames Vstupní vzorky (stereo páry).
 * @param in_rate   Vstupní frekvence [Hz] (> 0).
 * @param out_rate  Výstupní frekvence [Hz] (1..in_rate).
 * @return floor(in_frames * out_rate / in_rate).
 */
size_t audio_resample_frames(size_t in_frames, unsigned in_rate, unsigned out_rate);

/**
 * @brief Převzorkuje 16bitové stereo průměrováním (analytický náhled, ne hi-fi).
 *
 * Výstupní vzorek j je průměr vstupních vzorků [j*in/out, (j+1)*in/out)
 * (celočíselné meze). Při out_rate == in_rate a stereo je výstup bajtově
 * shodný se vstupem. Mono = průměr L a R.
 *
 * @param in        Vstup: prokládané L,R (`in_frames` párů).
 * @param in_frames Počet vstupních párů.
 * @param in_rate   Vstupní frekvence [Hz] (> 0).
 * @param out_rate  Výstupní frekvence [Hz] (1..in_rate).
 * @param stereo    true = výstup L,R; false = mono.
 * @param out       Výstup; místo na audio_resample_frames() vzorků (x2 u stereo).
 * @return Počet zapsaných výstupních vzorků (párů u stereo).
 */
size_t audio_resample_s16(const int16_t *in, size_t in_frames, unsigned in_rate,
                          unsigned out_rate, bool stereo, int16_t *out);

/**
 * @brief Sestaví 44bajtovou hlavičku WAV (PCM 16 bit, little endian).
 *
 * @param h        Výstup 44 bajtů.
 * @param rate     Vzorkovací frekvence [Hz].
 * @param channels Počet kanálů (1 nebo 2).
 * @param frames   Počet vzorků na kanál (velikost dat se omezí na 4 GiB - 16 B).
 */
void audio_wav_header(uint8_t h[44], unsigned rate, unsigned channels, uint64_t frames);

/** @brief Výsledek porovnání dvou signálů (audio_compare()). */
typedef struct st_AUDIO_COMPARE {
    int lag;          /**< Posun druhého signálu za prvním [vzorky] (b[i + lag] ~ a[i]). */
    size_t compared;  /**< Počet porovnaných párů vzorků. */
    double diff_rms;  /**< RMS rozdílu (mono průměr L,R) při `lag`, v jednotkách -1..1. */
    double diff_max;  /**< Největší absolutní rozdíl při `lag`, -1..1. */
    double a_rms;     /**< RMS prvního signálu v porovnaném úseku, -1..1. */
    bool lag_ambiguous; /**< audio_compare(): obálka nemá výrazné minimum (ustálený signál bez nástupu), posun z dat určit nejde. */
} st_AUDIO_COMPARE;

/**
 * @brief Najde posun druhého signálu (0..max_lag vzorků), při kterém se nejvíc shoduje s prvním.
 *
 * Hledá podle obálky (průměr |x| mono průměru přes 256 vzorků): nejmenší
 * průměrný čtverec rozdílu obálek nejdřív hrubě po 16 vzorcích, pak přesně
 * v okolí ±32 vzorků; nakonec zpřesní na surovém signálu v okolí ±8 vzorků.
 * Obálka je u ustáleného periodického signálu plochá, takže posun určí
 * nástupy a změny hlasitosti, ne minima rozdílu po periodách tónu. Bez
 * výrazného minima (nejlepší skóre aspoň polovina mediánu) nastaví
 * out->lag_ambiguous. Porovnává nejvýš `window` párů od začátku signálu a;
 * signály kratší než okno obálky porovná přímo.
 *
 * @param a       První signál (prokládané L,R), `na` párů.
 * @param na      Počet párů a.
 * @param b       Druhý signál (prokládané L,R), `nb` párů.
 * @param nb      Počet párů b.
 * @param max_lag Největší zkoušený posun [vzorky] (>= 0).
 * @param window  Nejvýš porovnaných párů (0 = vše).
 * @param out     Výstup.
 * @return false = signály se při žádném posunu nepřekrývají (out vynulovaný).
 */
bool audio_compare(const int16_t *a, size_t na, const int16_t *b, size_t nb,
                   int max_lag, size_t window, st_AUDIO_COMPARE *out);

/**
 * @brief RMS a maximum rozdílu dvou signálů při daném posunu (mono průměr L,R).
 *
 * @param a       První signál, `na` párů.
 * @param na      Počet párů a.
 * @param b       Druhý signál, `nb` párů.
 * @param nb      Počet párů b.
 * @param lag     Posun b za a [vzorky] (>= 0).
 * @param from    První porovnávaný vzorek a.
 * @param count   Nejvýš porovnaných párů.
 * @param out     Výstup (lag, compared, diff_rms, diff_max, a_rms).
 */
void audio_diff(const int16_t *a, size_t na, const int16_t *b, size_t nb,
                int lag, size_t from, size_t count, st_AUDIO_COMPARE *out);

/**
 * @brief Stav detektoru not jednoho kanálu PSG.
 *
 * @invariant playing => from_frame je snímek začátku rozehrané noty.
 */
typedef struct st_AUDIO_NOTE_TRACK {
    unsigned divider;     /**< Platný dělič tónu. */
    unsigned attn;        /**< Útlum 0..15. */
    bool noise;           /**< Šumový kanál. */
    bool playing;         /**< Kanál právě hraje (audio_psg_is_playing()). */
    uint64_t from_frame;  /**< Začátek rozehrané noty. */
    unsigned min_attn;    /**< Nejmenší útlum během noty (= největší hlasitost). */
} st_AUDIO_NOTE_TRACK;

/** @brief Dokončená nota (výstup detektoru). */
typedef struct st_AUDIO_NOTE {
    unsigned divider;     /**< Dělič tónu (u šumu 0). */
    bool noise;           /**< Šumový kanál. */
    unsigned max_level;   /**< Největší hlasitost 15 - min_attn (1..15). */
    uint64_t from_frame;  /**< První snímek noty. */
    uint64_t to_frame;    /**< Poslední snímek noty (>= from_frame). */
} st_AUDIO_NOTE;

/**
 * @brief Inicializuje detektor stavem kanálu na začátku.
 *
 * @param t       Detektor.
 * @param divider Dělič tónu.
 * @param attn    Útlum.
 * @param noise   Šumový kanál.
 * @param frame   Snímek, od kterého stav platí.
 */
void audio_note_track_init(st_AUDIO_NOTE_TRACK *t, unsigned divider, unsigned attn,
                           bool noise, uint64_t frame);

/**
 * @brief Nový stav kanálu ve snímku `frame`; ukončí notu, když se změnil dělič nebo kanál přestal hrát.
 *
 * Změna útlumu během hrající noty notu neukončí (jen zvýší max_level).
 *
 * @param t       Detektor.
 * @param divider Nový dělič tónu.
 * @param attn    Nový útlum.
 * @param noise   Šumový kanál.
 * @param frame   Snímek změny (>= snímek předchozí změny).
 * @param done    Výstup dokončené noty (to_frame = snímek změny; nová nota
 *                začíná ve stejném snímku).
 * @return true = nota skončila, `done` platí.
 */
bool audio_note_track_update(st_AUDIO_NOTE_TRACK *t, unsigned divider, unsigned attn,
                             bool noise, uint64_t frame, st_AUDIO_NOTE *done);

/**
 * @brief Ukončí rozehranou notu na konci rozebíraného úseku.
 *
 * @param t          Detektor.
 * @param last_frame Poslední snímek úseku.
 * @param done       Výstup.
 * @return true = kanál hrál, `done` platí (to_frame = last_frame).
 */
bool audio_note_track_finish(st_AUDIO_NOTE_TRACK *t, uint64_t last_frame, st_AUDIO_NOTE *done);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ANALYSIS_H */
