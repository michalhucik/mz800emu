#ifndef EMULATOR_MEASURING_H
#define EMULATOR_MEASURING_H

#include "main.h"
#include "mzarch/mzhal.h"
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define MAX_MEASURING_FRAME_TIMING_IN_SECS 60
/** @brief Strop obrazovek za sekundu napříč TV systémy (PAL 50, NTSC 60) - dimenzování bufferů. */
#define MEASURING_SCREENS_PER_SEC_MAX 60
#define MAX_MEASURING_FRAME_TIMING_SAMPLES (MEASURING_SCREENS_PER_SEC_MAX * MAX_MEASURING_FRAME_TIMING_IN_SECS)
#define PREDEF_MEASURING_FRAME_TIMING (g_mzhal.video_screens_per_sec * 1)

#define MAX_MEASURING_GDG_IN_SECS 10
#define EMULATOR_MEASURING_DEFAULT_GDG_UPDATE_SEC (1)

/** @brief Kapacita ring bufferu vzorků MAX SPEED benchmarku (vrstva B). */
#define MAX_MAXSPEED_BENCH_SAMPLES 1024
/** @brief Výchozí vzorkovací perioda distribuce MAX SPEED benchmarku [s]. */
#define EMULATOR_MEASURING_DEFAULT_MAXSPEED_SAMPLE_SEC (1)

typedef struct st_EMULATOR_MEASURING_FRAME_TIMING_STATS
{
    double mean;
    double min;
    double max;
    double median;
    double stddev;
    uint32_t samples_count;
    double total_width;
    double real_total_width;
} st_EMULATOR_MEASURING_FRAME_TIMING_STATS;

typedef struct st_EMULATOR_MEASURING_FRAME_TIMING
{
    bool enabled;
    bool console_output_enabled;
    struct timespec last_time;
    double samples[MAX_MEASURING_FRAME_TIMING_SAMPLES];
    uint32_t head;
    uint32_t tail;
    uint32_t measuring_width;
    uint32_t stats_predef; // Počet měření, po kterém se provede statistika
    uint32_t stats_cntr;   // Počítadlo měření - az se vynuluje, provede se statistika
    st_EMULATOR_MEASURING_FRAME_TIMING_STATS stats;
} st_EMULATOR_MEASURING_FRAME_TIMING;

typedef struct st_EMULATOR_MEASURING_GDG
{
    bool enabled;
    bool console_output_enabled;
    GRWLock rwlock;
    GMutex mutex;
    GCond cond;
    GTimer *timer;
    uint64_t last_gdg_ticks;
    float elapsed_time;
    float current_gdg_frequency;
    float current_fps;
    float current_gdg_frequency_percent;
    // pokud bezí vlastní vlákno
    GThread *thread;
    gulong thread_usleep_time;
} st_EMULATOR_MEASURING_GDG;

/**
 * @brief Výsledek MAX SPEED benchmarku (vrstva A - kumulativní celek).
 *
 * Snímek efektivity emulace nasbírané od posledního resetu přes všechny
 * segmenty, kdy emulátor běžel v MAX SPEED (a nebyl pozastaven). Naplňuje
 * jej @ref emulator_measuring_maxspeed_report.
 *
 * @note Všechny odvozené metriky platí jen při @ref valid == true (tj. byl
 *       změřen nenulový reálný čas v MAX SPEED). Jinak jsou nulové.
 */
typedef struct st_MAXSPEED_BENCH_RESULT
{
    uint64_t total_ticks;       /**< Emulované pxCLK takty za měřené segmenty. */
    double total_wall_sec;      /**< Reálný (wall-clock) čas měřených segmentů [s]. */
    double pxclk_per_sec;       /**< Throughput = total_ticks / total_wall_sec [Hz]. */
    double efficiency_percent;  /**< Efektivita = pxclk_per_sec / g_mzhal.gdgclk_real_base * 100. */
    double fps;                 /**< Reálné video snímky za sekundu (FB-FPS). */
    bool valid;                 /**< true, pokud byl změřen nenulový čas (total_wall_sec > 0). */
    bool segment_active;        /**< true, pokud v okamžiku reportu právě běží měřený segment. */

    /* Vrstva B - distribuce okamžité efektivity % přes vzorky od resetu. */
    uint32_t dist_samples;      /**< Počet vzorků v distribuci (0 = bez dat). */
    double eff_min;             /**< Minimální okamžitá efektivita % */
    double eff_max;             /**< Maximální okamžitá efektivita % */
    double eff_mean;            /**< Průměrná okamžitá efektivita % */
    double eff_median;          /**< Medián okamžité efektivity % */
    double eff_stddev;          /**< Směrodatná odchylka okamžité efektivity % */
} st_MAXSPEED_BENCH_RESULT;

/**
 * @brief Stav MAX SPEED benchmarku (vrstva A - kumulativní akumulátory).
 *
 * Měří efektivitu emulace pouze v segmentech, kdy emulátor běží v MAX SPEED
 * a není pozastaven (segment je "aktivní" právě když max_speed && !paused).
 * Akumulátory @ref acc_ticks a @ref acc_wall_us drží součet již uzavřených
 * segmentů; právě probíhající segment se dopočítává při reportu z
 * @ref seg_start_ticks / @ref seg_start_wall_us.
 *
 * Segment je "aktivní" (měří se) právě když platí všechny tři podmínky:
 * emulátor je v MAX SPEED, není pozastaven (pause) a neprobíhá externí stall.
 * Externí stall (@ref stalled) je období, kdy emulační vlákno reálně stojí
 * a nepostupují pxCLK, ale wall-clock běží - typicky blokující CMT-hack
 * (file chooser dialog, čtení MZF z disku). Bez vyloučení by takový stall
 * uměle snižoval naměřenou efektivitu.
 *
 * Invariant: pokud @ref seg_active == true, jsou @ref seg_start_ticks a
 * @ref seg_start_wall_us platné počáteční hodnoty aktuálního segmentu.
 *
 * Měření čte výhradně existující časovou základnu emulace
 * (@c gdg_compute_total_ticks) a wall-clock (@c CLOCK_MONOTONIC), nezasahuje
 * do instrukční smyčky a nelinkuje na debugger.
 *
 * @note @ref mutex chrání všechny členy proti souběhu mezi vláknem, které
 *       přepíná segment (UI thread přes hooky rychlosti/pauzy), a vláknem,
 *       které čte report (UI / sampling / MCP). Čtení samotné časové
 *       základny @c gdg_compute_total_ticks je cross-thread bez locku
 *       (torn read řádu jednoho snímku, vůči délce segmentu zanedbatelné).
 */
typedef struct st_EMULATOR_MEASURING_MAXSPEED
{
    GMutex mutex;               /**< Zámek chránící akumulátory a stav segmentu. */
    bool console_output_enabled; /**< Povolení textového výpisu reportu na stdout. */
    uint64_t acc_ticks;         /**< Akumulované pxCLK z uzavřených segmentů. */
    uint64_t acc_wall_us;       /**< Akumulovaný reálný čas uzavřených segmentů [us]. */
    bool seg_active;            /**< true, pokud právě probíhá měřený segment. */
    bool stalled;               /**< true během externího stallu (CMT-hack apod.) - segment pozastaven. */
    uint64_t seg_start_ticks;   /**< pxCLK na začátku aktuálního segmentu. */
    uint64_t seg_start_wall_us; /**< Wall-clock na začátku aktuálního segmentu [us]. */

    /* Vrstva B - vzorkovaný sampling thread + ring buffer okamžité efektivity %.
     * Thread vzorkuje každých sample_interval_us a z přírůstku kumulativních
     * akumulátorů (které rostou jen v aktivních segmentech) počítá okamžitou
     * efektivitu - tím se NORMAL/pauza/stall vyloučí automaticky. Ring buffer
     * a last_sample_* jsou chráněny @ref mutex; thread řídí sample_ctrl_mutex
     * + sample_cond. */
    GThread *sample_thread;       /**< Sampling thread (NULL pokud neběží). */
    GMutex sample_ctrl_mutex;     /**< Mutex pro GCond řízení životnosti threadu. */
    GCond sample_cond;            /**< Signalizace ukončení sampling threadu. */
    bool sample_thread_run;       /**< Řídí běh sampling threadu (false = ukončit). */
    gulong sample_interval_us;    /**< Vzorkovací perioda [us]. */
    double samples[MAX_MAXSPEED_BENCH_SAMPLES]; /**< Ring buffer vzorků efektivity %. */
    uint32_t samples_head;        /**< Ring buffer head (počet zapsaných). */
    uint32_t samples_tail;        /**< Ring buffer tail (nejstarší platný). */
    uint64_t last_sample_ticks;   /**< Kumulativní pxCLK při posledním vzorku. */
    uint64_t last_sample_wall_us; /**< Kumulativní wall [us] při posledním vzorku. */
    bool have_last_sample;        /**< false = cold start (první vzorek se zahodí). */
} st_EMULATOR_MEASURING_MAXSPEED;

typedef struct st_EMULATOR_MEASURING
{
    st_EMULATOR_MEASURING_FRAME_TIMING frame_timing;
    st_EMULATOR_MEASURING_GDG gdg;
    st_EMULATOR_MEASURING_MAXSPEED maxspeed;
} st_EMULATOR_MEASURING;

extern st_EMULATOR_MEASURING g_emulator_measuring;

#define EMULATOR_MEASURING_TEST_FRAME_TIMING_ENABLED (g_emulator_measuring.frame_timing.enabled)
#define EMULATOR_MEASURING_SET_FRAME_TIMING_ENABLED(x) (g_emulator_measuring.frame_timing.enabled = (x))
#define EMULATOR_MEASURING_FRAME_TIMING_ENABLED_PTR (&g_emulator_measuring.frame_timing.enabled)

#define EMULATOR_MEASURING_FRAME_TIMING_GET_PREDEF_TIME_IN_SEC() (g_emulator_measuring.frame_timing.stats_predef / g_mzhal.video_screens_per_sec)
#define EMULATOR_MEASURING_FRAME_TIMING_SET_PREDEF_TIME_IN_SEC(x) (g_emulator_measuring.frame_timing.stats_predef = x * g_mzhal.video_screens_per_sec)

#define EMULATOR_MEASURING_FRAME_TIMING_GET_MEASURING_WIDTH_IN_SEC() (g_emulator_measuring.frame_timing.measuring_width / g_mzhal.video_screens_per_sec)
#define EMULATOR_MEASURING_FRAME_TIMING_SET_MEASURING_WIDTH_IN_SEC(x) (g_emulator_measuring.frame_timing.measuring_width = x * g_mzhal.video_screens_per_sec)

#define EMULATOR_MEASURING_TEST_GDG_ENABLED (g_emulator_measuring.gdg.enabled)
#define EMULATOR_MEASURING_GDG_ENABLED_PTR (&g_emulator_measuring.gdg.enabled)

#define EMULATOR_MEASURING_GET_GDG_UPDATE_TIME_SEC() (g_emulator_measuring.gdg.thread_usleep_time / 1000000)
#define EMULATOR_MEASURING_SET_GDG_ENABLED_AND_UPDATE_TIME_SEC(enabled, sec) (emulator_measuring_gdg_set_enabled(&g_emulator_measuring.gdg, enabled, sec))

#ifdef __cplusplus
extern "C"
{
#endif
    void emulator_measuring_init(void);
    void emulator_measuring_exit(void);
    void emulator_measuring_frame_timing_reset(void);
    void emulator_measuring_frame_timing_event(void);

    void emulator_measuring_gdg_init(st_EMULATOR_MEASURING_GDG *msgdg);
    void emulator_measuring_gdg_exit(st_EMULATOR_MEASURING_GDG *msgdg);
    void emulator_measuring_gdg_event(st_EMULATOR_MEASURING_GDG *msgdg);
    bool emulator_measuring_gdg_set_enabled(st_EMULATOR_MEASURING_GDG *msgdg, bool enabled, int update_time_sec);

    /**
     * @brief Inicializuje stav MAX SPEED benchmarku.
     *
     * Vynuluje akumulátory, uvede segment do neaktivního stavu a inicializuje
     * zámek. Volá se z @ref emulator_measuring_init.
     *
     * @param ms Ukazatel na stav benchmarku (nesmí být NULL).
     * @post Akumulátory jsou nulové, @c seg_active == false, @c mutex inicializován.
     */
    void emulator_measuring_maxspeed_init(st_EMULATOR_MEASURING_MAXSPEED *ms);

    /**
     * @brief Uvolní prostředky stavu MAX SPEED benchmarku.
     *
     * @param ms Ukazatel na stav benchmarku (nesmí být NULL).
     * @post Zámek @c mutex je uvolněn (clear).
     */
    void emulator_measuring_maxspeed_exit(st_EMULATOR_MEASURING_MAXSPEED *ms);

    /**
     * @brief Vynuluje benchmark a začne nové měření od aktuálního okamžiku.
     *
     * Zahodí všechny dosud nasbírané akumulátory. Pokud je emulátor v okamžiku
     * volání v aktivním měřeném stavu (MAX SPEED a nepozastaven), otevře rovnou
     * nový segment od teď; jinak zůstane segment neaktivní až do nejbližšího
     * přechodu do MAX SPEED.
     *
     * Thread-safe (drží @c mutex). Vhodné volat před měřením konkrétního programu.
     *
     * @post acc_ticks == 0, acc_wall_us == 0; segment případně znovuotevřen.
     */
    void emulator_measuring_maxspeed_reset(void);

    /**
     * @brief Přehodnotí stav měřeného segmentu podle aktuální rychlosti/pauzy.
     *
     * Segment má být aktivní právě když @c g_emulator.max_speed == true a
     * @c g_emulator.paused == false. Funkce podle toho otevře nový segment
     * (zaznamená počáteční pxCLK a wall-clock), nebo uzavře probíhající segment
     * (přičte jeho přírůstek do akumulátorů). Pokud se stav nemění, nedělá nic.
     *
     * Volá se z hooků @ref emulator_max_speed a @ref emulator_pause vždy AŽ PO
     * aktualizaci @c g_emulator.max_speed / @c g_emulator.paused.
     *
     * Thread-safe (drží @c mutex).
     */
    void emulator_measuring_maxspeed_update_segment(void);

    /**
     * @brief Začátek externího stallu - pozastaví měřený segment.
     *
     * Označí, že emulace dočasně stojí a nepostupují pxCLK, ačkoliv wall-clock
     * běží (typicky blokující CMT-hack: file chooser dialog, čtení MZF).
     * Uzavře probíhající segment, takže doba stallu se do měření nezapočítá.
     *
     * Musí být párováno s @ref emulator_measuring_maxspeed_stall_end na všech
     * návratových cestách stallující operace. Thread-safe (drží @c mutex).
     */
    void emulator_measuring_maxspeed_stall_begin(void);

    /**
     * @brief Konec externího stallu - obnoví měřený segment.
     *
     * Zruší příznak stallu a (pokud je emulátor stále v MAX SPEED a není
     * pozastaven) otevře nový segment od aktuálního okamžiku. Doba mezi
     * @ref emulator_measuring_maxspeed_stall_begin a tímto voláním do měření
     * nevstoupí. Thread-safe (drží @c mutex).
     */
    void emulator_measuring_maxspeed_stall_end(void);

    /**
     * @brief Sestaví aktuální výsledek benchmarku (vrstva A).
     *
     * Sečte akumulátory uzavřených segmentů a (pokud právě běží) přírůstek
     * aktuálního segmentu, dopočítá odvozené metriky a naplní @p out.
     *
     * Thread-safe (drží @c mutex). Měření nijak neresetuje.
     *
     * @param out Výstupní struktura (nesmí být NULL).
     * @post @p out naplněn; při nulovém změřeném čase je @c out->valid == false
     *       a odvozené metriky jsou nulové.
     */
    void emulator_measuring_maxspeed_report(st_MAXSPEED_BENCH_RESULT *out);

    /**
     * @brief Vypíše aktuální výsledek benchmarku na stdout.
     *
     * Textový report (vrstva A) ve formátu nezávislém na UI - funguje i v
     * headless / @c NO_DEBUGGER buildu. Měření neresetuje.
     */
    void emulator_measuring_maxspeed_print(void);
#ifdef __cplusplus
}
#endif

#endif // EMULATOR_MEASURING_H