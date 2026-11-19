/**
 * @file   audio_capture.c
 * @brief  Zachycení zvuku do složky - implementace (viz audio_capture.h).
 *
 * Události kanálů a zápisy do PSG se zapisují do CSV hned v emu vlákně
 * (stdio s velkým bufferem). reference.wav renderuje videorec_audio
 * (stejný renderer jako video záznam v emulovaném čase) po každém
 * zvukovém snímku do horizontu, do kterého jsou všechny změny doručené.
 * sdl.wav dostává hotové bloky SDL cesty z háčku v iface_audio_wait_for_data().
 * Paměť zachycení (ringy, souhrn snímků) plní tytéž háčky pod stejným
 * zámkem; čtení kopíruje úsek ringu pod zámkem (audio_capture_stream_copy()).
 *
 * @par Licence: GPLv3
 */

#include "audio_capture.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mzarch/mzarch_config.h"
#include "audio.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/psg/psg.h"
#include "iface/iface_audio.h"
#include "libs/sdlapp/sdlapp.h"

/* Globální instance aplikace (main.c) - domovský adresář pro výchozí složku. */
extern SdlApp *g_sdlapp;
#include "videorec/videorec_audio.h"
#include "cfgmain.h"
#include "emulator.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"


gint g_audio_capture_flags = 0;

/** @brief Velikost stdio bufferu CSV souborů [B]. */
#define AC_CSV_BUFFER (256u * 1024u)
/** @brief Vzorků (stereo párů) na jedno volání rendereru. */
#define AC_RENDER_CHUNK 2048u
/**
 * @brief Zahřátí rendereru reference.wav před začátkem [takty GDG] = 100 ms.
 *
 * GDGCLK_BASE je dělitelný 10 na všech platformách (snímek x 50 nebo 60),
 * 100 ms je tedy přesně 4410 vzorků a celý počet taktů.
 */
#define AC_WARMUP_TICKS ((uint64_t) GDGCLK_BASE / 10u)
/**
 * @brief Zvukové snímky rozjezdu SDL cesty před začátkem (jen se sdl.wav).
 *
 * Bez zvukového zařízení (headless) SDL cesta mimo zachycení neběží a její
 * filtry začínají ze studeného stavu. Během rozjezdu se bloky vyrábějí
 * a zahazují; 6 snímků (100 ms při 60 Hz) stačí filtrům kanálů a výstupní
 * propusti. Střídavé vazby (CTC0, celý výstup) mají paměť delší, jejich stav
 * proto SDL cesta přebírá od emulátoru (st_AUDIO_LOG::ctc0_ac_mean,
 * out_ac_mean), když log nenavazuje na předchozí zpracovaný.
 */
#define AC_SDL_WARMUP_FRAMES 6u

/** @brief Otevřený WAV soubor (16 bit PCM stereo, AUDIO_CAPTURE_WAV_RATE). */
typedef struct st_AC_WAV {
    FILE *fp;          /**< Soubor, NULL = nezapisuje se. */
    uint64_t samples;  /**< Zapsané stereo páry (pro hlavičku při zavření). */
} st_AC_WAV;

/**
 * @brief Ring záznamů pevné velikosti (proud paměti zachycení).
 *
 * @invariant data != NULL => cap > 0 && rec > 0; záznam seq leží na indexu
 *            seq % cap a je platný pro seq v [ac_ring_first(), next).
 */
typedef struct st_AC_RING {
    uint8_t *data;     /**< Paměť cap * rec bajtů, NULL = proud se nedrží. */
    size_t rec;        /**< Velikost záznamu [B]. */
    uint64_t cap;      /**< Kapacita [záznamy]. */
    uint64_t next;     /**< Pořadové číslo příštího záznamu. */
} st_AC_RING;

/**
 * @brief Průběžné součty pro souhrn zvukového snímku (st_AUDIO_CAPTURE_FRAME_REC).
 *
 * Součty on/energy po kanálech jsou od začátku zachycení do last_t;
 * snímek bere rozdíl proti stavu na konci předchozího snímku (*_snap).
 */
typedef struct st_AC_FRAME_ACC {
    uint8_t level[AUDIO_CAPTURE_MAX_CHANNELS];      /**< Aktuální hodnota kanálu (hodnota zvukového logu 0..31). */
    uint64_t last_t[AUDIO_CAPTURE_MAX_CHANNELS];    /**< Čas, do kterého jsou součty kanálu spočtené. */
    uint64_t on[AUDIO_CAPTURE_MAX_CHANNELS];        /**< Takty s úrovní > 0 od začátku. */
    uint64_t energy[AUDIO_CAPTURE_MAX_CHANNELS];    /**< Součet úroveň^2 x takty od začátku (ac_level_sq()). */
    uint64_t on_snap[AUDIO_CAPTURE_MAX_CHANNELS];   /**< `on` na konci předchozího snímku. */
    uint64_t energy_snap[AUDIO_CAPTURE_MAX_CHANNELS]; /**< `energy` na konci předchozího snímku. */
    uint32_t edges[AUDIO_CAPTURE_MAX_CHANNELS];     /**< Změny v aktuálním snímku. */
    uint8_t max_level[AUDIO_CAPTURE_MAX_CHANNELS];  /**< Největší stupeň hlasitosti (0..15) v aktuálním snímku. */
    double ref_sq[2];    /**< Součet čtverců vzorků reference L, R ve snímku. */
    uint32_t ref_peak[2]; /**< Špička reference L, R ve snímku. */
    uint32_t ref_n;      /**< Vzorky reference ve snímku. */
    double sdl_sq[2];    /**< Součet čtverců vzorků SDL L, R od předchozího snímku. */
    uint32_t sdl_peak[2]; /**< Špička SDL L, R. */
    uint32_t sdl_n;      /**< Vzorky SDL od předchozího snímku. */
    uint32_t sdl_blocks; /**< Bloky SDL od předchozího snímku. */
} st_AC_FRAME_ACC;

/**
 * @brief Stav modulu; celý ho chrání s_mutex (háčky emu vlákna, sdl a sdl_blocks
 *        zapisuje i vlákno SDL zvuku přes audio_capture_sdl_output()).
 *
 * @invariant active => (origin, frame, last_horizon) platí pro běžící zachycení.
 * @invariant began => origin a last_horizon platí i po konci zachycení
 *            (pro capture.json); has_last_event => last_event_ticks >= origin.
 * @invariant pending && active se nevylučují jen během audio_capture_on_frame().
 * @invariant mem => ring[LEVELS], ring[PSG_WRITES], ring[FRAMES] jsou alokované;
 *            ringy zůstávají po konci zachycení až do dalšího startu.
 */
static struct {
    bool pending;                    /**< Start naplánovaný na konec snímku. */
    bool active;                     /**< Zachytává se. */
    st_AUDIO_CAPTURE_PARAMS params;  /**< Parametry posledního startu. */
    char dir[AUDIO_CAPTURE_PATH_MAX]; /**< Složka. */
    FILE *events;                    /**< events.csv, nebo NULL. */
    FILE *psg;                       /**< psg_writes.csv, nebo NULL. */
    st_AC_WAV ref;                   /**< reference.wav. */
    st_AC_WAV sdl;                   /**< sdl.wav (zapisuje i vlákno SDL zvuku). */
    uint64_t sdl_blocks;             /**< Zapsané bloky SDL cesty. */
    bool sdl_writing;                /**< Bloky SDL cesty se zapisují (false = rozjezd, zahazují se). */
    unsigned warmup_frames;          /**< Zbývající snímky rozjezdu SDL cesty před začátkem. */
    st_VIDEOREC_AUDIO renderer;      /**< Renderer reference.wav (platí jen při active && ref.fp). */
    bool renderer_init;              /**< renderer je inicializovaný (k uvolnění). */
    bool began;                      /**< Zachycení opravdu začalo (ac_begin), ne jen naplánováno. */
    uint64_t origin;                 /**< Čas začátku zachycení [absolutní takty GDG] = čas 0 v CSV. */
    uint64_t last_horizon;           /**< Horizont posledního snímku [absolutní takty GDG] = konec reference.wav. */
    bool has_last_event;             /**< Zapsán aspoň jeden řádek CSV (events.csv nebo psg_writes.csv). */
    uint64_t last_event_ticks;       /**< Absolutní čas posledního řádku CSV [takty GDG]. */
    uint64_t frame;                  /**< Pořadí aktuálního snímku od začátku (0 = první). */
    uint64_t n_events;               /**< Řádky events.csv. */
    uint64_t n_psg;                  /**< Řádky psg_writes.csv. */
    char started[32];                /**< Čas startu (ISO 8601, místní). */
    char stop_reason[AUDIO_CAPTURE_TEXT_MAX]; /**< Důvod konce. */
    char last_error[AUDIO_CAPTURE_TEXT_MAX];  /**< Poslední chyba. */
    bool sdl_on;                     /**< SDL cesta se zachytává (sdl.wav nebo PCM ring SDL). */
    uint64_t ref_total;              /**< Vyrenderované vzorky reference (i bez reference.wav). */
    uint64_t sdl_total;              /**< Přijaté vzorky SDL cesty (i bez sdl.wav). */
    unsigned segment;                /**< Pořadí zachycení (zvýší každý úspěšný start). */
    bool mem;                        /**< Paměť zachycení se drží. */
    st_AC_RING ring[AUDIO_CAPTURE_STREAM_COUNT]; /**< Proudy paměti (en_AUDIO_CAPTURE_STREAM). */
    st_AC_FRAME_ACC acc;             /**< Součty aktuálního snímku (jen s mem). */
    st_AUDIO_CAPTURE_PSG_INITIAL psg_init; /**< Stav PSG na začátku zachycení. */
} s_ac;

/**
 * @brief Zámek nad celým s_ac.
 *
 * Veřejné funkce ho berou na vstupu a volají vnitřní `ac_*` funkce, které
 * předpokládají, že je zamčený. Háčky emu vlákna ho berou jen během
 * zachycení (audio_capture_wanted()), mimo zachycení stojí háček jedno
 * atomické čtení.
 */
static GMutex s_mutex;

/** @brief Po návratu z háčku ukončit emulátor (frames dosaženo s quit_after_stop). */
static bool s_quit_now;

st_AUDIO_CAPTURE_SETTINGS g_audio_capture_settings = { "", true, true, true, true };


/** @brief Název kanálu audio logu pro CSV ("ctc0", "psg0_0".."psg1_3"). */
static const char *ac_channel_name(unsigned src_id)
{
    static const char *const names[9] = {
        "ctc0", "psg0_0", "psg0_1", "psg0_2", "psg0_3", "psg1_0", "psg1_1", "psg1_2", "psg1_3"
    };
    return (src_id < 9) ? names[src_id] : "?";
}


/** @brief Zapíše text chyby do bufferu volajícího a do s_ac.last_error. */
static void ac_error(char *err, size_t err_size, const char *msg)
{
    g_strlcpy(s_ac.last_error, msg, sizeof(s_ac.last_error));
    if (err && err_size) g_strlcpy(err, msg, err_size);
}


/** @brief Nejstarší držený záznam ringu. */
static uint64_t ac_ring_first(const st_AC_RING *r)
{
    return (r->next > r->cap) ? r->next - r->cap : 0;
}


/** @brief Uvolní ring (proud se přestane držet). */
static void ac_ring_free(st_AC_RING *r)
{
    g_free(r->data);
    memset(r, 0, sizeof(*r));
}


/**
 * @brief Alokuje prázdný ring.
 * @return false = nedostatek paměti (ring zůstane prázdný).
 */
static bool ac_ring_alloc(st_AC_RING *r, size_t rec, uint64_t cap)
{
    ac_ring_free(r);
    if (cap == 0 || rec == 0) return false;
    r->data = (uint8_t *) g_try_malloc((gsize) (cap * rec));
    if (!r->data) return false;
    r->rec = rec;
    r->cap = cap;
    r->next = 0;
    return true;
}


/** @brief Přidá `n` záznamů (při naplnění přepisuje nejstarší). */
static void ac_ring_push(st_AC_RING *r, const void *recs, uint64_t n)
{
    if (!r->data) return;
    const uint8_t *src = (const uint8_t *) recs;
    /* Z dávky větší než ring zůstane jen její konec. */
    if (n > r->cap) {
        src += (n - r->cap) * r->rec;
        r->next += n - r->cap;
        n = r->cap;
    }
    while (n > 0) {
        uint64_t idx = r->next % r->cap;
        uint64_t k = r->cap - idx;
        if (k > n) k = n;
        memcpy(r->data + idx * r->rec, src, (size_t) (k * r->rec));
        src += k * r->rec;
        r->next += k;
        n -= k;
    }
}


/** @brief Uvolní všechny ringy paměti zachycení. */
static void ac_mem_free(void)
{
    for (unsigned i = 0; i < AUDIO_CAPTURE_STREAM_COUNT; i++) ac_ring_free(&s_ac.ring[i]);
    s_ac.mem = false;
}


/** @brief Takty od začátku zachycení na µs jako text s desetinnou tečkou. */
static void ac_time_us(uint64_t rel_ticks, char *buf, size_t size)
{
    double us = (double) rel_ticks * 1000000.0 / (double) GDGCLK_BASE;
    g_ascii_formatd(buf, (int) size, "%.3f", us);
}


/** @brief Zapíše 44bajtovou hlavičku WAV (PCM 16 bit stereo) pro `samples` párů. */
static void ac_wav_header(FILE *fp, uint64_t samples)
{
    uint32_t data = (uint32_t) ((samples * 4u > 0xFFFFFFF0u) ? 0xFFFFFFF0u : samples * 4u);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36u + data;
    for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t) (riff >> (8 * i));
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16, rate = AUDIO_CAPTURE_WAV_RATE, byte_rate = AUDIO_CAPTURE_WAV_RATE * 4u;
    for (int i = 0; i < 4; i++) h[16 + i] = (uint8_t) (fmt_len >> (8 * i));
    h[20] = 1; h[21] = 0;           /* PCM */
    h[22] = 2; h[23] = 0;           /* stereo */
    for (int i = 0; i < 4; i++) h[24 + i] = (uint8_t) (rate >> (8 * i));
    for (int i = 0; i < 4; i++) h[28 + i] = (uint8_t) (byte_rate >> (8 * i));
    h[32] = 4; h[33] = 0;           /* block align */
    h[34] = 16; h[35] = 0;          /* bitů na vzorek */
    memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; i++) h[40 + i] = (uint8_t) (data >> (8 * i));
    fwrite(h, 1, sizeof(h), fp);
}


/**
 * @brief Otevře WAV v adresáři zachycení (s dočasnou hlavičkou).
 * @return false = soubor nejde vytvořit.
 */
static bool ac_wav_open(st_AC_WAV *w, const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    w->fp = g_fopen(path, "wb");
    g_free(path);
    w->samples = 0;
    if (!w->fp) return false;
    ac_wav_header(w->fp, 0);
    return true;
}


/** @brief Zapíše stereo páry int16 (little endian) do WAV. */
static void ac_wav_write(st_AC_WAV *w, const int16_t *lr, size_t frames)
{
    if (!w->fp || frames == 0) return;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    fwrite(lr, sizeof(int16_t), frames * 2u, w->fp);
#else
    for (size_t i = 0; i < frames * 2u; i++) {
        uint8_t b[2] = { (uint8_t) lr[i], (uint8_t) ((uint16_t) lr[i] >> 8) };
        fwrite(b, 1, 2, w->fp);
    }
#endif
    w->samples += frames;
}


/** @brief Dopíše hlavičku WAV a zavře ho. @return false = chyba zápisu. */
static bool ac_wav_close(st_AC_WAV *w)
{
    if (!w->fp) return true;
    bool ok = !ferror(w->fp);
    if (fseek(w->fp, 0, SEEK_SET) == 0) ac_wav_header(w->fp, w->samples);
    else ok = false;
    if (fclose(w->fp) != 0) ok = false;
    w->fp = NULL;
    return ok;
}


/**
 * @brief Otevře CSV s hlavičkou a velkým bufferem.
 * @return Soubor, nebo NULL.
 */
static FILE *ac_csv_open(const char *dir, const char *name, const char *header)
{
    char *path = g_build_filename(dir, name, NULL);
    FILE *fp = g_fopen(path, "wb"); /* binárně: LF na všech platformách */
    g_free(path);
    if (!fp) return NULL;
    setvbuf(fp, NULL, _IOFBF, AC_CSV_BUFFER);
    fputs(header, fp);
    return fp;
}


/** @brief Zavře CSV. @return false = chyba zápisu. */
static bool ac_csv_close(FILE **fp)
{
    if (!*fp) return true;
    bool ok = !ferror(*fp);
    if (fclose(*fp) != 0) ok = false;
    *fp = NULL;
    return ok;
}


/** @brief Escapuje řetězec do JSON (uvozovky, zpětné lomítko, řídicí znaky). */
static void ac_json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') { fputc('\\', fp); fputc(c, fp); }
        else if (c < 0x20) fprintf(fp, "\\u%04x", c);
        else fputc(c, fp);
    }
    fputc('"', fp);
}


/** @brief Zapíše capture.json s metadaty a konečnými počty. */
static void ac_write_json(void)
{
    char *path = g_build_filename(s_ac.dir, "capture.json", NULL);
    FILE *fp = g_fopen(path, "wb");
    g_free(path);
    if (!fp) {
        g_strlcpy(s_ac.last_error, "Cannot write capture.json", sizeof(s_ac.last_error));
        return;
    }
    fprintf(fp, "{\n  \"platform\": ");
    ac_json_str(fp, MZARCH_NAME);
    fprintf(fp, ",\n  \"started\": ");
    ac_json_str(fp, s_ac.started);
    fprintf(fp, ",\n  \"gdg_clk_hz\": %llu,\n  \"frames_per_sec\": %u,\n  \"wav_rate\": %u,\n",
            (unsigned long long) GDGCLK_BASE, (unsigned) VIDEO_SCREENS_PER_SEC, AUDIO_CAPTURE_WAV_RATE);
    /* Absolutní časy v taktech GDG (stejná časová osa jako total_screens
     * * screen_total_ticks_per_frame + total_ticks v emulator://periph/gdg);
     * null, pokud zachycení nezačalo (stop během čekání na konec snímku)
     * nebo nebyl zapsán žádný řádek CSV. */
    fprintf(fp, "  \"began\": %s,\n", s_ac.began ? "true" : "false");
    if (s_ac.began) {
        fprintf(fp, "  \"origin_ticks\": %llu,\n  \"end_ticks\": %llu,\n",
                (unsigned long long) s_ac.origin, (unsigned long long) s_ac.last_horizon);
    } else {
        fprintf(fp, "  \"origin_ticks\": null,\n  \"end_ticks\": null,\n");
    }
    if (s_ac.began && s_ac.has_last_event) {
        fprintf(fp, "  \"last_event_ticks\": %llu,\n", (unsigned long long) s_ac.last_event_ticks);
    } else {
        fprintf(fp, "  \"last_event_ticks\": null,\n");
    }
    /* Hodiny platformy, stejné názvy a hodnoty jako emulator://platform/info. */
    fprintf(fp, "  \"clocks\": {\"gdg_base_hz\": %llu, \"gdg_real_base_hz\": %llu, "
                "\"cpu_divider\": %u, \"ctc0_divider\": %u, ",
            (unsigned long long) GDGCLK_BASE, (unsigned long long) GDGCLK_REAL_BASE,
            (unsigned) GDGCLK2CPU_DIVIDER, (unsigned) GDGCLK_CTC0_DIVIDER);
#if HAVE_PSG
    fprintf(fp, "\"psg_divider\": %u, ", (unsigned) PSG_DIVIDER);
#else
    fprintf(fp, "\"psg_divider\": null, ");
#endif
    fprintf(fp, "\"screen_total_ticks_per_frame\": %llu},\n", (unsigned long long) VIDEO_SCREEN_TICKS);
    fprintf(fp, "  \"channels\": [");
    for (unsigned i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        fprintf(fp, "%s", i ? ", " : "");
        ac_json_str(fp, ac_channel_name(i));
    }
    fprintf(fp, "],\n  \"stereo_at_end\": %s,\n",
#if HAVE_PSG == 2
            g_psg_module.stereo ? "true" : "false"
#else
            "false"
#endif
    );
    uint64_t sdl_blocks = s_ac.sdl_blocks, sdl_samples = s_ac.sdl.samples;
    fprintf(fp, "  \"frames\": %llu,\n  \"events\": %llu,\n  \"psg_writes\": %llu,\n"
                "  \"reference_samples\": %llu,\n  \"sdl_blocks\": %llu,\n  \"sdl_samples\": %llu,\n",
            (unsigned long long) s_ac.frame, (unsigned long long) s_ac.n_events,
            (unsigned long long) s_ac.n_psg, (unsigned long long) s_ac.ref.samples,
            (unsigned long long) sdl_blocks, (unsigned long long) sdl_samples);
    fprintf(fp, "  \"stop_reason\": ");
    ac_json_str(fp, s_ac.stop_reason);
    fprintf(fp, ",\n  \"last_error\": ");
    ac_json_str(fp, s_ac.last_error);
    fprintf(fp, "\n}\n");
    fclose(fp);
}


/** @brief Zavře soubory, uvolní renderer, zapíše capture.json a nastaví důvod konce. */
static void ac_finish(const char *reason)
{
    g_atomic_int_set(&g_audio_capture_flags, 0);
    s_ac.pending = false;
    s_ac.active = false;
    g_strlcpy(s_ac.stop_reason, reason, sizeof(s_ac.stop_reason));

    bool ok = ac_csv_close(&s_ac.events);
    ok = ac_csv_close(&s_ac.psg) && ok;
    ok = ac_wav_close(&s_ac.ref) && ok;
    ok = ac_wav_close(&s_ac.sdl) && ok;
    s_ac.sdl_writing = false;
    if (!ok) g_strlcpy(s_ac.last_error, "Write error while closing the capture files", sizeof(s_ac.last_error));
    if (s_ac.renderer_init) {
        videorec_audio_free(&s_ac.renderer);
        s_ac.renderer_init = false;
    }
    if (s_ac.dir[0]) ac_write_json();   /* zachycení jen do paměti nemá složku */
    if (s_ac.params.quit_after_stop && strcmp(reason, "Frame count reached") == 0) {
        s_quit_now = true;
    }
}


/** @brief Sestaví výchozí složku `<výstupní složka zachycení>/<platforma>_audio_RRRRMMDD_HHMMSS`. */
static void ac_default_dir(char *out, size_t size)
{
    char base[AUDIO_CAPTURE_PATH_MAX];
    audio_capture_resolve_output_dir(base, sizeof(base));
    char name[64];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    g_snprintf(name, sizeof(name), "%s_audio_%s", MZARCH_NAME, stamp);
    char *dir = g_build_filename(base, name, NULL);
    g_strlcpy(out, dir, size);
    g_free(dir);
}


/** @brief audio_capture_start() se zamčeným s_mutex. */
static bool ac_start(const st_AUDIO_CAPTURE_PARAMS *p, char *err, size_t err_size)
{
    if (s_ac.pending || s_ac.active) {
        ac_error(err, err_size, "An audio capture is already running");
        return false;
    }
    bool files = p->events || p->psg_writes || p->reference_wav || p->sdl_wav;
    if (!files && !p->memory) {
        ac_error(err, err_size, "No output selected (events, psg_writes, reference_wav, sdl_wav, memory)");
        return false;
    }

    char dir[AUDIO_CAPTURE_PATH_MAX];
    dir[0] = '\0';
    if (files && p->dir[0]) g_strlcpy(dir, p->dir, sizeof(dir));
    else if (files) ac_default_dir(dir, sizeof(dir));
    if (files && (g_mkdir_with_parents(dir, 0755) != 0 || !g_file_test(dir, G_FILE_TEST_IS_DIR))) {
        char msg[AUDIO_CAPTURE_TEXT_MAX];
        g_snprintf(msg, sizeof(msg), "Cannot create directory '%s'", dir);
        ac_error(err, err_size, msg);
        return false;
    }

    FILE *ev = NULL, *ps = NULL;
    st_AC_WAV ref = { NULL, 0 }, sdl = { NULL, 0 };
    bool ok = true;
    if (p->events) ok = ( ev = ac_csv_open(dir, "events.csv", "ticks,time_us,frame,channel,level\n") ) != NULL;
    if (ok && p->psg_writes) ok = ( ps = ac_csv_open(dir, "psg_writes.csv",
                                                     "ticks,time_us,frame,psg,byte,channel,reg,value\n") ) != NULL;
    if (ok && p->reference_wav) ok = ac_wav_open(&ref, dir, "reference.wav");
    if (ok && p->sdl_wav) ok = ac_wav_open(&sdl, dir, "sdl.wav");
    const char *fail = ok ? NULL : "files";

    /* Paměť: ringy předchozího zachycení se uvolní až tady (start projde). */
    if (ok) {
        ac_mem_free();
        if (p->memory) {
            uint64_t frames = p->ring_frames ? p->ring_frames
                : (uint64_t) AUDIO_CAPTURE_MEMORY_MINUTES_DEFAULT * 60u * VIDEO_SCREENS_PER_SEC;
            uint64_t pcm = p->ring_pcm ? p->ring_pcm
                : (uint64_t) AUDIO_CAPTURE_PCM_SECONDS_DEFAULT * AUDIO_CAPTURE_WAV_RATE;
            ok = ac_ring_alloc(&s_ac.ring[AUDIO_CAPTURE_STREAM_LEVELS], sizeof(st_AUDIO_CAPTURE_LEVEL_REC),
                               p->ring_levels ? p->ring_levels : AUDIO_CAPTURE_RING_LEVELS_DEFAULT)
                 && ac_ring_alloc(&s_ac.ring[AUDIO_CAPTURE_STREAM_PSG_WRITES], sizeof(st_AUDIO_CAPTURE_PSG_REC),
                                  p->ring_psg_writes ? p->ring_psg_writes : AUDIO_CAPTURE_RING_PSG_DEFAULT)
                 && ac_ring_alloc(&s_ac.ring[AUDIO_CAPTURE_STREAM_FRAMES], sizeof(st_AUDIO_CAPTURE_FRAME_REC), frames)
                 && (!p->pcm_reference || ac_ring_alloc(&s_ac.ring[AUDIO_CAPTURE_STREAM_PCM_REFERENCE], 4u, pcm))
                 && (!p->pcm_sdl || ac_ring_alloc(&s_ac.ring[AUDIO_CAPTURE_STREAM_PCM_SDL], 4u, pcm));
            if (!ok) {
                ac_mem_free();
                fail = "memory";
            }
        }
    }
    if (!ok) {
        ac_csv_close(&ev);
        ac_csv_close(&ps);
        ac_wav_close(&ref);
        ac_wav_close(&sdl);
        char msg[AUDIO_CAPTURE_TEXT_MAX];
        if (strcmp(fail, "memory") == 0) {
            g_strlcpy(msg, "Not enough memory for the audio capture rings", sizeof(msg));
        } else {
            g_snprintf(msg, sizeof(msg), "Cannot create the capture files in '%s'", dir);
        }
        ac_error(err, err_size, msg);
        return false;
    }
    s_ac.mem = p->memory;
    s_ac.segment++;

    s_ac.params = *p;
    g_strlcpy(s_ac.dir, dir, sizeof(s_ac.dir));
    s_ac.events = ev;
    s_ac.psg = ps;
    s_ac.ref = ref;
    s_ac.sdl = sdl;
    s_ac.sdl_blocks = 0;
    s_ac.sdl_writing = false;
    s_ac.sdl_on = sdl.fp || (p->memory && p->pcm_sdl);
    s_ac.warmup_frames = s_ac.sdl_on ? AC_SDL_WARMUP_FRAMES : 0;
    s_ac.ref_total = 0;
    s_ac.sdl_total = 0;
    memset(&s_ac.acc, 0, sizeof(s_ac.acc));
    memset(&s_ac.psg_init, 0, sizeof(s_ac.psg_init));
    s_ac.began = false;
    s_ac.has_last_event = false;
    s_ac.frame = 0;
    s_ac.n_events = 0;
    s_ac.n_psg = 0;
    s_ac.stop_reason[0] = '\0';
    s_ac.last_error[0] = '\0';
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(s_ac.started, sizeof(s_ac.started), "%Y-%m-%dT%H:%M:%S", &tmv);
    s_ac.pending = true;
    s_ac.active = false;
    /* SDL cesta běží už během rozjezdu (bloky se zahazují do ac_begin()). */
    g_atomic_int_set(&g_audio_capture_flags,
                     AUDIO_CAPTURE_FLAG_ON | (s_ac.sdl_on ? AUDIO_CAPTURE_FLAG_SDL : 0));
    return true;
}


/** @brief audio_capture_stop() se zamčeným s_mutex. */
static bool ac_stop(void)
{
    if (!s_ac.pending && !s_ac.active) return false;
    ac_finish("Stopped");
    return true;
}


/** @brief audio_capture_get_status() se zamčeným s_mutex. */
static void ac_get_status(st_AUDIO_CAPTURE_STATUS *out)
{
    memset(out, 0, sizeof(*out));
    out->pending = s_ac.pending;
    out->active = s_ac.active;
    out->frames = s_ac.frame;
    out->events = s_ac.n_events;
    out->psg_writes = s_ac.n_psg;
    out->reference_samples = s_ac.ref_total;
    out->sdl_blocks = s_ac.sdl_blocks;
    out->sdl_samples = s_ac.sdl_total;
    g_strlcpy(out->dir, s_ac.dir, sizeof(out->dir));
    g_strlcpy(out->stop_reason, s_ac.stop_reason, sizeof(out->stop_reason));
    g_strlcpy(out->last_error, s_ac.last_error, sizeof(out->last_error));
    out->segment = s_ac.segment;
    out->began = s_ac.began;
    out->origin_ticks = s_ac.began ? s_ac.origin : 0;
    out->channels = AUDIO_SRC_CHANNELS_COUNT;
#if HAVE_PSG == 2
    out->stereo = g_psg_module.stereo;
#endif
    out->memory = s_ac.mem;
    for (unsigned i = 0; i < AUDIO_CAPTURE_STREAM_COUNT; i++) {
        const st_AC_RING *r = &s_ac.ring[i];
        st_AUDIO_CAPTURE_STREAM_INFO *si = &out->streams[i];
        si->present = (r->data != NULL);
        si->record_size = r->rec;
        si->capacity = r->cap;
        si->first_seq = ac_ring_first(r);
        si->next_seq = r->next;
        out->memory_bytes += r->cap * r->rec;
    }
}


/** @brief Zaznamená absolutní čas právě zapsaného řádku CSV (pro last_event_ticks). */
static void ac_note_event(uint64_t abs_ticks)
{
    if (!s_ac.has_last_event || abs_ticks > s_ac.last_event_ticks) {
        s_ac.last_event_ticks = abs_ticks;
    }
    s_ac.has_last_event = true;
}


/** @brief Čitatel a jmenovatel čtverce AUDIO_LEVEL_HALF_FACTOR (0,49^2 = 0,2401, zaokrouhleno na 6 / 25). */
#define AC_HALF_SQ_NUM 6u
/** @brief Viz AC_HALF_SQ_NUM. */
#define AC_HALF_SQ_DEN 25u

/** @brief Čitatel a jmenovatel čtverce AUDIO_CTC0_DIV2_FACTOR (0,64^2 = 0,4096, zaokrouhleno na 41 / 100; samostatný MZ-700 0,50^2 = 25 / 100). */
#if MZARCH == 700
#define AC_DIV2_SQ_NUM 25u
#else
#define AC_DIV2_SQ_NUM 41u
#endif
/** @brief Viz AC_DIV2_SQ_NUM. */
#define AC_DIV2_SQ_DEN 100u

/**
 * @brief Čtverec výchylky hodnoty zvukového logu v jednotkách stupeň^2, krát takty.
 *
 * Plná výchylka: stupeň^2 x takty. Poloviční výchylka (hodnoty 16..30, tón PSG
 * s děličem 001h): stupeň^2 x takty x 6 / 25 (celočíselně, dolů). Hodnota 31
 * (CTC0 v režimu 3 s děličem 2): 15^2 x takty x 41 / 100 (celočíselně, dolů;
 * samostatný MZ-700 x 25 / 100).
 *
 * @param value Hodnota zvukového logu 0..31.
 * @param dt    Doba v taktech GDG.
 * @return Příspěvek do součtu energie kanálu.
 */
static uint64_t ac_level_sq(uint8_t value, uint64_t dt)
{
    uint64_t step = audio_level_step(value);
    uint64_t e = step * step * dt;
    if (audio_level_is_ctc0_div2(value))
        return (e * AC_DIV2_SQ_NUM) / AC_DIV2_SQ_DEN;
    return audio_level_is_half(value) ? (e * AC_HALF_SQ_NUM) / AC_HALF_SQ_DEN : e;
}


/**
 * @brief Zapíše řádek events.csv a záznam do ringu levels.
 * @param rel    Čas od začátku zachycení [takty GDG].
 * @param src_id Index kanálu.
 * @param value  Hodnota zvukového logu 0..31 (16..30 = poloviční výchylka stupně value - 15,
 *               31 = výchylka 0,64 stupně 15 u CTC0 v režimu 3 s děličem 2).
 */
static void ac_write_event(uint64_t rel, unsigned src_id, uint8_t value)
{
    if (s_ac.mem) {
        st_AUDIO_CAPTURE_LEVEL_REC r;
        memset(&r, 0, sizeof(r));
        r.t = s_ac.origin + rel;
        r.frame = (uint32_t) s_ac.frame;
        r.src = (uint8_t) src_id;
        r.level = (uint8_t) (value & VIDEOREC_AUDIO_VALUE_MASK);
        ac_ring_push(&s_ac.ring[AUDIO_CAPTURE_STREAM_LEVELS], &r, 1);
    }
    if (!s_ac.events) return;
    char t[32];
    ac_time_us(rel, t, sizeof(t));
    fprintf(s_ac.events, "%llu,%s,%llu,%s,%u\n", (unsigned long long) rel, t,
            (unsigned long long) s_ac.frame, ac_channel_name(src_id), (unsigned) (value & VIDEOREC_AUDIO_VALUE_MASK));
    s_ac.n_events++;
    ac_note_event(s_ac.origin + rel);
}


/** @brief audio_capture_on_change() se zamčeným s_mutex. */
static void ac_on_change(unsigned src_id, uint8_t value, uint64_t ticks)
{
    if (!s_ac.active) return;
    if (ticks < s_ac.origin) {
        ac_finish("Emulation timeline restarted (reset or snapshot load)");
        return;
    }
    ac_write_event(ticks - s_ac.origin, src_id, value);
    if (s_ac.mem && src_id < AUDIO_SRC_CHANNELS_COUNT) {
        st_AC_FRAME_ACC *a = &s_ac.acc;
        if (ticks > a->last_t[src_id]) {
            uint64_t dt = ticks - a->last_t[src_id];
            if (a->level[src_id]) a->on[src_id] += dt;
            a->energy[src_id] += ac_level_sq(a->level[src_id], dt);
            a->last_t[src_id] = ticks;
        }
        a->level[src_id] = (uint8_t) (value & VIDEOREC_AUDIO_VALUE_MASK);
        a->edges[src_id]++;
        uint8_t step = (uint8_t) audio_level_step(a->level[src_id]);
        if (step > a->max_level[src_id]) a->max_level[src_id] = step;
    }
    if ((s_ac.ref.fp || s_ac.mem) && src_id < AUDIO_SRC_CHANNELS_COUNT) {
        if (videorec_audio_event(&s_ac.renderer, src_id, value, ticks) != 0) {
            g_strlcpy(s_ac.last_error, "Reference renderer: out of memory", sizeof(s_ac.last_error));
        }
    }
}


/** @brief audio_capture_on_psg_write() se zamčeným s_mutex. */
static void ac_on_psg_write(uint64_t ticks, unsigned mask, uint8_t value)
{
#if HAVE_PSG == 0
    (void) ticks; (void) mask; (void) value;
#else
    if (!s_ac.active || (!s_ac.psg && !s_ac.mem)) return;
    if (ticks < s_ac.origin) {
        ac_finish("Emulation timeline restarted (reset or snapshot load)");
        return;
    }
    /* Dekódování ze stavu cílového PSG po zápisu (při zápisu do obou PSG
     * ukazuje PSG0): latch_cs/latch_attn určují kanál a registr. */
    const st_PSG *psg = &g_psg_module.psg[(mask & 0x01) ? 0 : 1];
    unsigned cs = psg->latch_cs;
    const st_PSG_CHANNEL *ch = &psg->channel[cs];
    static const char *const reg_names[4] = { "tone_lo", "tone", "volume", "noise" };
    en_AUDIO_CAPTURE_PSG_REG reg;
    unsigned val;
    if (psg->latch_attn) {
        reg = AUDIO_CAPTURE_PSG_REG_VOLUME;
        val = (unsigned) ch->attn;
    } else if (ch->type == PSG_CHTYPE_TONE) {
        if (value & 0x80) {
            /* Latch bajt tónu nastaví jen spodní 4 bity, dělič se změní až datovým bajtem. */
            reg = AUDIO_CAPTURE_PSG_REG_TONE_LO;
            val = ch->tone.latch_divider;
        } else {
            reg = AUDIO_CAPTURE_PSG_REG_TONE;
            val = ch->tone.divider;
        }
    } else {
        reg = AUDIO_CAPTURE_PSG_REG_NOISE;
        val = ((unsigned) ch->noise.type << 2) | (unsigned) ch->noise.div_type;
    }
    uint64_t rel = ticks - s_ac.origin;
    if (s_ac.mem) {
        st_AUDIO_CAPTURE_PSG_REC r;
        memset(&r, 0, sizeof(r));
        r.t = ticks;
        r.frame = (uint32_t) s_ac.frame;
        r.value = (uint16_t) val;
        r.psg = (uint8_t) mask;
        r.byte = value;
        r.channel = (uint8_t) cs;
        r.reg = (uint8_t) reg;
        ac_ring_push(&s_ac.ring[AUDIO_CAPTURE_STREAM_PSG_WRITES], &r, 1);
    }
    if (s_ac.psg) {
        char t[32];
        ac_time_us(rel, t, sizeof(t));
        fprintf(s_ac.psg, "%llu,%s,%llu,%u,%02X,%u,%s,%u\n", (unsigned long long) rel, t,
                (unsigned long long) s_ac.frame, mask, (unsigned) value, cs, reg_names[reg], val);
        s_ac.n_psg++;
    }
    ac_note_event(ticks);
#endif
}


/** @brief Začne naplánované zachycení v horizontu `horizon`. */
static void ac_begin(uint64_t horizon)
{
    s_ac.pending = false;
    s_ac.active = true;
    s_ac.began = true;
    s_ac.origin = horizon;
    s_ac.last_horizon = horizon;
    s_ac.frame = 0;

    uint8_t values[AUDIO_SRC_CHANNELS_COUNT];
    for (unsigned i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        values[i] = (uint8_t) (g_audio.log ? (g_audio.log->src[i]->last_value & VIDEOREC_AUDIO_VALUE_MASK) : 0);
        /* Výchozí úrovně všech kanálů, aby byl events.csv sám o sobě úplný. */
        ac_write_event(0, i, values[i]);
        if (i < AUDIO_CAPTURE_MAX_CHANNELS) {
            s_ac.acc.level[i] = values[i];
            s_ac.acc.max_level[i] = (uint8_t) audio_level_step(values[i]);
            s_ac.acc.last_t[i] = horizon;
        }
    }
#if HAVE_PSG
    /* Stav PSG na začátku (detektor not v souhrnu potřebuje dělič a útlum
     * nastavené před začátkem zachycení). */
    s_ac.psg_init.valid = true;
    for (unsigned c = 0; c < 2 && c < PSG_MAX_COUNT; c++) {
        for (unsigned k = 0; k < 4 && k < PSG_CHANNELS_COUNT; k++) {
            const st_PSG_CHANNEL *pc = &g_psg_module.psg[c].channel[k];
            s_ac.psg_init.divider[c][k] = (uint16_t) pc->tone.divider;
            s_ac.psg_init.attn[c][k] = (uint8_t) pc->attn;
            s_ac.psg_init.noise[c][k] = (pc->type == PSG_CHTYPE_NOISE);
        }
    }
#endif
    if (s_ac.ref.fp || s_ac.mem) {
        float level[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
        memset(level, 0, sizeof(level));
        iface_audio_build_videorec_levels(level, VIDEOREC_AUDIO_MAX_CHANNELS);
        /* Zahřátí: renderer začne AC_WARMUP_TICKS (100 ms) před začátkem se stejnými
         * úrovněmi a tyto vzorky zahodí, aby filtry kanálů začínaly ustálené
         * (bez vazby výstupu i zaparkované trvalé úrovně PSG). Stav střídavých
         * vazeb se po zahřátí nastaví podle emulátoru. Vzorky
         * reference.wav pak začínají přesně v `horizon`, protože
         * AC_WARMUP_TICKS je celý počet vzorků. */
        uint64_t warm = (horizon >= AC_WARMUP_TICKS) ? AC_WARMUP_TICKS : 0;
        videorec_audio_init(&s_ac.renderer, GDGCLK_BASE, AUDIO_CAPTURE_WAV_RATE, AUDIO_SRC_CHANNELS_COUNT,
                            (const float (*)[VIDEOREC_AUDIO_LEVELS]) level, values, horizon - warm, true);
        s_ac.renderer_init = true;
        /* výstupní propust podle volby hned od začátku (bez přechodu) */
        videorec_audio_set_output_filter(&s_ac.renderer, iface_audio_output_filter_enabled(), true);
        /* střídavá vazba celého výstupu místo parkování PSG (MZ-800, MZ-1500) */
        videorec_audio_set_output_coupling(&s_ac.renderer, AUDIO_HAVE_OUTPUT_COUPLING != 0);
        if (warm) {
            int16_t discard[AC_RENDER_CHUNK * 2u];
            while (videorec_audio_render(&s_ac.renderer, horizon, discard, AC_RENDER_CHUNK) == AC_RENDER_CHUNK) {
            }
        }
        /* Střídavá vazba CTC0 naváže na stav vedený emulátorem (stálá úroveň
         * může v okamžiku začátku ještě doznívat). */
        videorec_audio_set_ctc0_mean(&s_ac.renderer, audio_ctc0_ac_mean(horizon));
        /* totéž střídavá vazba výstupu (bez ní nuly) */
        double out_mean[2];
        iface_audio_output_ac_means(horizon, g_psg_module.stereo, out_mean);
        videorec_audio_set_output_mean(&s_ac.renderer, out_mean[0], out_mean[1]);
    }
    s_ac.sdl_writing = s_ac.sdl_on;
}


/** @brief Přičte vyrenderované vzorky reference do součtů snímku. */
static void ac_acc_ref(const int16_t *lr, size_t n)
{
    st_AC_FRAME_ACC *a = &s_ac.acc;
    for (size_t i = 0; i < n; i++) {
        for (unsigned c = 0; c < 2; c++) {
            int v = lr[i * 2u + c];
            a->ref_sq[c] += (double) v * (double) v;
            uint32_t m = (uint32_t) (v < 0 ? -v : v);
            if (m > a->ref_peak[c]) a->ref_peak[c] = m;
        }
    }
    a->ref_n += (uint32_t) n;
}


/** @brief RMS ze součtu čtverců, omezené na 0..32767. */
static uint16_t ac_rms16(double sq, uint32_t n)
{
    if (n == 0) return 0;
    double r = sqrt(sq / (double) n);
    return (uint16_t) ((r > 32767.0) ? 32767.0 : r + 0.5);
}


/** @brief Takty jako uint32 se saturací. */
static uint32_t ac_sat32(uint64_t v)
{
    return (v > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t) v;
}


/**
 * @brief Uzavře souhrn snímku [frame_start, horizon) do ringu frames a vynuluje součty snímku.
 * @pre s_ac.mem, s_ac.frame je číslo uzavíraného snímku.
 */
static void ac_push_frame(uint64_t frame_start, uint64_t horizon)
{
    st_AC_FRAME_ACC *a = &s_ac.acc;
    st_AUDIO_CAPTURE_FRAME_REC r;
    memset(&r, 0, sizeof(r));
    r.t = frame_start;
    r.frame = (uint32_t) s_ac.frame;
    r.len_ticks = ac_sat32(horizon - frame_start);
    for (unsigned i = 0; i < AUDIO_SRC_CHANNELS_COUNT && i < AUDIO_CAPTURE_MAX_CHANNELS; i++) {
        /* Dopočítat součty do horizontu; změna doručená za horizontem už je
         * v nich a patří tomuto snímku (viz st_AUDIO_CAPTURE_FRAME_REC). */
        if (horizon > a->last_t[i]) {
            uint64_t dt = horizon - a->last_t[i];
            if (a->level[i]) a->on[i] += dt;
            a->energy[i] += ac_level_sq(a->level[i], dt);
            a->last_t[i] = horizon;
        }
        r.ch[i].edges = a->edges[i];
        r.ch[i].on_ticks = ac_sat32(a->on[i] - a->on_snap[i]);
        r.ch[i].energy = ac_sat32(a->energy[i] - a->energy_snap[i]);
        r.ch[i].max_level = a->max_level[i];
        r.ch[i].end_level = (uint8_t) audio_level_step(a->level[i]);
        a->on_snap[i] = a->on[i];
        a->energy_snap[i] = a->energy[i];
        a->edges[i] = 0;
        a->max_level[i] = (uint8_t) audio_level_step(a->level[i]);
    }
    if (s_ac.renderer_init) {
        r.flags |= AUDIO_CAPTURE_FRAME_FLAG_REF;
        for (unsigned c = 0; c < 2; c++) {
            r.ref_rms[c] = ac_rms16(a->ref_sq[c], a->ref_n);
            r.ref_peak[c] = (uint16_t) a->ref_peak[c];
        }
        r.ref_samples = a->ref_n;
    }
    if (s_ac.sdl_on) {
        r.flags |= AUDIO_CAPTURE_FRAME_FLAG_SDL;
        for (unsigned c = 0; c < 2; c++) {
            r.sdl_rms[c] = ac_rms16(a->sdl_sq[c], a->sdl_n);
            r.sdl_peak[c] = (uint16_t) a->sdl_peak[c];
        }
        r.sdl_samples = a->sdl_n;
        r.sdl_blocks = (uint16_t) ((a->sdl_blocks > 0xFFFFu) ? 0xFFFFu : a->sdl_blocks);
    }
    memset(a->ref_sq, 0, sizeof(a->ref_sq));
    memset(a->ref_peak, 0, sizeof(a->ref_peak));
    a->ref_n = 0;
    memset(a->sdl_sq, 0, sizeof(a->sdl_sq));
    memset(a->sdl_peak, 0, sizeof(a->sdl_peak));
    a->sdl_n = 0;
    a->sdl_blocks = 0;
    ac_ring_push(&s_ac.ring[AUDIO_CAPTURE_STREAM_FRAMES], &r, 1);
}


/** @brief audio_capture_on_frame() se zamčeným s_mutex. */
static void ac_on_frame(uint64_t horizon_ticks)
{
    if (s_ac.pending) {
        if (s_ac.warmup_frames > 0) {
            s_ac.warmup_frames--;
            return;
        }
        ac_begin(horizon_ticks);
        return;
    }
    if (!s_ac.active) return;
    if (horizon_ticks < s_ac.last_horizon) {
        ac_finish("Emulation timeline restarted (reset or snapshot load)");
        return;
    }
    uint64_t frame_start = s_ac.last_horizon;
    s_ac.last_horizon = horizon_ticks;

    if (s_ac.renderer_init) {
#if HAVE_PSG == 2
        videorec_audio_set_stereo(&s_ac.renderer, g_psg_module.stereo);
#endif
        /* volbu výstupní propusti lze přepnout za běhu (plynulý přechod jako SDL cesta) */
        videorec_audio_set_output_filter(&s_ac.renderer, iface_audio_output_filter_enabled(), false);
        int16_t buf[AC_RENDER_CHUNK * 2u];
        size_t n;
        do {
            n = videorec_audio_render(&s_ac.renderer, horizon_ticks, buf, AC_RENDER_CHUNK);
            ac_wav_write(&s_ac.ref, buf, n);
            s_ac.ref_total += n;
            if (s_ac.mem) {
                ac_ring_push(&s_ac.ring[AUDIO_CAPTURE_STREAM_PCM_REFERENCE], buf, n);
                ac_acc_ref(buf, n);
            }
        } while (n == AC_RENDER_CHUNK);
    }
    if (s_ac.mem) ac_push_frame(frame_start, horizon_ticks);

    s_ac.frame++;
    uint64_t limit = s_ac.params.frames ? s_ac.params.frames : AUDIO_CAPTURE_MAX_FRAMES_SAFETY;
    if ((s_ac.events && ferror(s_ac.events)) || (s_ac.psg && ferror(s_ac.psg))
        || (s_ac.ref.fp && ferror(s_ac.ref.fp))) {
        g_strlcpy(s_ac.last_error, "Write error (disk full?)", sizeof(s_ac.last_error));
        ac_finish("Error");
    } else if (s_ac.frame >= limit) {
        ac_finish(s_ac.params.frames ? "Frame count reached" : "Safety limit reached");
    }
}


void audio_capture_sdl_output(const float *stereo, size_t frames)
{
    if (!audio_capture_wants_sdl() || !stereo || frames == 0) return;
    int16_t buf[AC_RENDER_CHUNK * 2u];
    g_mutex_lock(&s_mutex);
    if (s_ac.sdl_on && s_ac.sdl_writing) {
        st_AC_FRAME_ACC *a = &s_ac.acc;
        size_t done = 0;
        while (done < frames) {
            size_t n = frames - done;
            if (n > AC_RENDER_CHUNK) n = AC_RENDER_CHUNK;
            for (size_t i = 0; i < n * 2u; i++) {
                float v = stereo[done * 2u + i] * 32767.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                buf[i] = (int16_t) v;
                if (s_ac.mem) {
                    int x = buf[i];
                    a->sdl_sq[i & 1u] += (double) x * (double) x;
                    uint32_t m = (uint32_t) (x < 0 ? -x : x);
                    if (m > a->sdl_peak[i & 1u]) a->sdl_peak[i & 1u] = m;
                }
            }
            ac_wav_write(&s_ac.sdl, buf, n);
            if (s_ac.mem) ac_ring_push(&s_ac.ring[AUDIO_CAPTURE_STREAM_PCM_SDL], buf, n);
            done += n;
        }
        s_ac.sdl_total += frames;
        s_ac.sdl_blocks++;
        if (s_ac.mem) {
            a->sdl_n += (uint32_t) frames;
            a->sdl_blocks++;
        }
    }
    g_mutex_unlock(&s_mutex);
}


/** @brief Ukončí emulátor, pokud o to požádal ac_finish() (volat mimo zámek). */
static void ac_quit_if_requested(void)
{
    if (!s_quit_now) return;
    s_quit_now = false;
    fprintf(stderr, "[audio_capture] Requested frame count reached, quitting the emulator\n");
    emulator_quit(EXIT_SUCCESS); /* stejný kontext jako videorec --record-frames; nevrátí se */
}


bool audio_capture_start(const st_AUDIO_CAPTURE_PARAMS *p, char *err, size_t err_size)
{
    g_mutex_lock(&s_mutex);
    bool ok = ac_start(p, err, err_size);
    g_mutex_unlock(&s_mutex);
    return ok;
}


bool audio_capture_stop(void)
{
    g_mutex_lock(&s_mutex);
    bool ok = ac_stop();
    g_mutex_unlock(&s_mutex);
    return ok;
}


void audio_capture_get_status(st_AUDIO_CAPTURE_STATUS *out)
{
    g_mutex_lock(&s_mutex);
    ac_get_status(out);
    g_mutex_unlock(&s_mutex);
}


void audio_capture_on_change(unsigned src_id, uint8_t value, uint64_t ticks)
{
    g_mutex_lock(&s_mutex);
    ac_on_change(src_id, value, ticks);
    g_mutex_unlock(&s_mutex);
}


void audio_capture_on_psg_write(uint64_t ticks, unsigned mask, uint8_t value)
{
    g_mutex_lock(&s_mutex);
    ac_on_psg_write(ticks, mask, value);
    g_mutex_unlock(&s_mutex);
}


void audio_capture_on_frame(uint64_t horizon_ticks)
{
    g_mutex_lock(&s_mutex);
    ac_on_frame(horizon_ticks);
    g_mutex_unlock(&s_mutex);
    ac_quit_if_requested();
}


void audio_capture_exit(void)
{
    g_mutex_lock(&s_mutex);
    if (s_ac.pending || s_ac.active) ac_finish("Emulator exit");
    s_quit_now = false;
    ac_mem_free();
    g_mutex_unlock(&s_mutex);
}


bool audio_capture_stream_copy(en_AUDIO_CAPTURE_STREAM stream, uint64_t from_seq,
                               uint64_t count, size_t max_bytes,
                               st_AUDIO_CAPTURE_STREAM_COPY *out)
{
    memset(out, 0, sizeof(*out));
    if ((unsigned) stream >= AUDIO_CAPTURE_STREAM_COUNT) return true;
    bool ok = true;
    g_mutex_lock(&s_mutex);
    out->segment = s_ac.segment;
    out->live = s_ac.pending || s_ac.active;
    out->origin_ticks = s_ac.began ? s_ac.origin : 0;
    const st_AC_RING *r = &s_ac.ring[stream];
    if (r->data) {
        out->present = true;
        out->record_size = r->rec;
        out->first_seq = ac_ring_first(r);
        out->next_seq = r->next;
        uint64_t lo = (from_seq > out->first_seq) ? from_seq : out->first_seq;
        uint64_t hi = r->next;
        if (lo < hi && count < hi - lo) hi = lo + count;
        uint64_t max_rec = max_bytes / r->rec;
        if (lo < hi && hi - lo > max_rec) hi = lo + max_rec;
        if (lo < hi) {
            out->data = (uint8_t *) g_try_malloc((gsize) ((hi - lo) * r->rec));
            if (!out->data) {
                ok = false;
            } else {
                uint8_t *dst = out->data;
                for (uint64_t seq = lo; seq < hi; ) {
                    uint64_t idx = seq % r->cap;
                    uint64_t k = r->cap - idx;
                    if (k > hi - seq) k = hi - seq;
                    memcpy(dst, r->data + idx * r->rec, (size_t) (k * r->rec));
                    dst += k * r->rec;
                    seq += k;
                }
                out->data_seq = lo;
                out->data_count = hi - lo;
            }
        }
    }
    g_mutex_unlock(&s_mutex);
    return ok;
}


void audio_capture_stream_copy_clear(st_AUDIO_CAPTURE_STREAM_COPY *c)
{
    g_free(c->data);
    memset(c, 0, sizeof(*c));
}


void audio_capture_get_psg_initial(st_AUDIO_CAPTURE_PSG_INITIAL *out)
{
    g_mutex_lock(&s_mutex);
    *out = s_ac.psg_init;
    if (!s_ac.began) out->valid = false;
    g_mutex_unlock(&s_mutex);
}


bool audio_capture_resolve_output_dir(char *out, size_t size)
{
    if (g_audio_capture_settings.output_dir[0]) {
        g_strlcpy(out, g_audio_capture_settings.output_dir, size);
        (void) g_mkdir_with_parents(out, 0755);
        return g_file_test(out, G_FILE_TEST_IS_DIR);
    }
    if (g_sdlapp && g_sdlapp->paths && g_sdlapp->paths->home_dir) {
        char *dir = g_build_filename(g_sdlapp->paths->home_dir, "audio_captures", NULL);
        bool ok = (g_mkdir_with_parents(dir, 0755) == 0) && g_file_test(dir, G_FILE_TEST_IS_DIR);
        if (ok) g_strlcpy(out, dir, size);
        g_free(dir);
        if (ok) return true;
    }
    g_strlcpy(out, ".", size);
    return true;
}


void audio_capture_params_from_settings(st_AUDIO_CAPTURE_PARAMS *p)
{
    audio_capture_params_init(p);
    p->events = g_audio_capture_settings.events;
    p->psg_writes = g_audio_capture_settings.psg_writes;
    p->reference_wav = g_audio_capture_settings.reference_wav;
    p->sdl_wav = g_audio_capture_settings.sdl_wav;
}


/* ========================================================================= */
/*                     INI [AUDIO_CAPTURE] - výběr souborů                   */
/* ========================================================================= */

/** @brief Ukazatel na položku g_audio_capture_settings podle jména prvku INI. */
static bool *ac_setting_ptr(const char *name)
{
    if (!strcmp(name, "events")) return &g_audio_capture_settings.events;
    if (!strcmp(name, "psg_writes")) return &g_audio_capture_settings.psg_writes;
    if (!strcmp(name, "reference_wav")) return &g_audio_capture_settings.reference_wav;
    return &g_audio_capture_settings.sdl_wav;
}

/** @brief INI -> output_dir. @param e Prvek. @param data Nepoužito. */
static void ac_propagatecfg_dir(void *e, void *data)
{
    (void) data;
    char *dir = cfgelement_get_text_value((st_CFGELEMENT *) e);
    g_strlcpy(g_audio_capture_settings.output_dir, dir ? dir : "", sizeof(g_audio_capture_settings.output_dir));
}

/** @brief output_dir -> INI. @param e Prvek. @param data Nepoužito. */
static void ac_savecfg_dir(void *e, void *data)
{
    (void) data;
    cfgelement_set_text_value((st_CFGELEMENT *) e, g_audio_capture_settings.output_dir);
}

/** @brief INI -> nastavení. @param e Prvek. @param data Jméno prvku. */
static void ac_propagatecfg(void *e, void *data)
{
    *ac_setting_ptr((const char *) data) = cfgelement_get_bool_value((st_CFGELEMENT *) e) != 0;
}

/** @brief Nastavení -> INI. @param e Prvek. @param data Jméno prvku. */
static void ac_savecfg(void *e, void *data)
{
    cfgelement_set_bool_value((st_CFGELEMENT *) e, *ac_setting_ptr((const char *) data) ? 1 : 0);
}


void audio_capture_config_init(void)
{
    static const char *const names[4] = { "events", "psg_writes", "reference_wav", "sdl_wav" };
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "AUDIO_CAPTURE");
    CFGELM *dir = cfgmodule_register_new_element(cmod, "output_dir", CFGENTYPE_TEXT, "");
    cfgelement_set_propagate_cb(dir, ac_propagatecfg_dir, NULL);
    cfgelement_set_save_cb(dir, ac_savecfg_dir, NULL);
    for (int i = 0; i < 4; i++) {
        CFGELM *elm = cfgmodule_register_new_element(cmod, (char *) names[i], CFGENTYPE_BOOL, 1);
        cfgelement_set_propagate_cb(elm, ac_propagatecfg, (void *) names[i]);
        cfgelement_set_save_cb(elm, ac_savecfg, (void *) names[i]);
    }
    /* Načíst sekci z INI a propagovat (vzor screenshot_config_init). */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}
