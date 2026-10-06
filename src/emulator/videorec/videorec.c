/**
 * @file   videorec.c
 * @brief  Lepidlo video záznamu: stav session, požadavky z UI, hooky jádra, writer vlákno.
 *
 * Viz videorec.h. Přehled toku dat:
 *
 * @verbatim
 *  emu vlákno                                     writer vlákno
 *  ----------                                     -------------
 *  videorec_on_screen_done() --pipe_frame-->  st_VIDEOREC_PIPE
 *  videorec_audio_tap()      --audio_event->       |
 *  videorec_audio_horizon()  --horizon------>      | sink: VR_MSG_FRAME
 *                                                  v
 *                                     GAsyncQueue  --->  zdvojení řádků -> ZMBV enkodér -> AVI zapisovač
 * @endverbatim
 *
 * Emu vlákno kopíruje jen nativní framebuffer (VR_WIDTH x VR_HEIGHT, např.
 * 928x288 u MZ-800, 704x232 u MZ-700 a MZ-1500 - viz g_vr_platform).
 * Writer každý řádek zapíše dvakrát (VR_WIDTH x VR_AVI_HEIGHT, 928x576 / 704x464), takže
 * AVI má stejný poměr stran jako okno emulátoru (to zobrazuje framebuffer
 * s dvojnásobnou výškou). Zdvojení neovlivňuje číslování snímků, retake ani
 * rollover partů - vše pracuje s indexy snímků.
 *
 * Writer vlákno vlastní enkodér, AVI zapisovač a seznam partů aktuální
 * session. Sidecar model vzniká v emu vlákně; při ukončení nahrávání se jeho
 * vlastnictví předá zprávou VR_MSG_SIDECAR writeru, který do něj doplní party
 * (včetně těch, které vznikly až při zpracování posledních snímků ve frontě)
 * a uloží ho. Díky tomu sidecar vždy zná všechny party, bez ohledu na to, kolik
 * snímků ve frontě v okamžiku stopu ještě čekalo.
 *
 * @par Licence: GPLv3
 */

#include "videorec.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/sdlapp/sdlapp.h"

/** Aplikační kontext (main.c); z něj se bere home_dir pro výchozí adresář nahrávek. */
extern SdlApp *g_sdlapp;

/**
 * Výchozí nastavení. `output_dir` prázdné = `<home_dir>/videos`
 * (viz videorec_resolve_output_dir()).
 */
st_VIDEOREC_SETTINGS g_videorec_settings = {
    "",                              /* output_dir */
    48000,                           /* audio_rate */
    VIDEOREC_RETAKE_DISCARD,         /* retake_mode */
    VIDEOREC_TRANS_FADE,             /* default_transition */
    500,                             /* transition_ms */
    250,                             /* keyframe_interval */
    VIDEOREC_TIMEBASE_EMULATED,      /* timebase */
    VIDEOREC_RT_PAUSE_SKIP,          /* realtime_pause */
    VIDEOREC_RT_PAUSE_CAP_DEFAULT_S, /* realtime_pause_cap_s */
    VIDEOREC_RT_SPEED_AS_SEEN,       /* realtime_speed */
    VIDEOREC_TURBO_AUDIO_AS_HEARD,   /* realtime_turbo_audio */
    VIDEOREC_STATE_MARKS_SIDECAR,    /* state_marks */
    true,                            /* auto_markers */
    false                            /* record_debugger_steps */
};

/**
 * @brief Najde název v tabulce (pomocník pro videorec_*_from_name()).
 * @param names Tabulka názvů indexovaná hodnotou enumu.
 * @param n     Počet položek.
 * @param name  Hledaný název (nesmí být NULL).
 * @return Index (hodnota enumu), nebo -1 pro neznámý název.
 */
static int vr_name_index(const char *const *names, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) return i;
    }
    return -1;
}

/** Názvy en_VIDEOREC_TIMEBASE (INI, UI). */
static const char *const VR_TIMEBASE_NAMES[] = { "emulated", "realtime" };
/** Názvy en_VIDEOREC_RT_PAUSE. */
static const char *const VR_RT_PAUSE_NAMES[] = { "skip", "freeze", "freeze_capped" };
/** Názvy en_VIDEOREC_RT_SPEED. */
static const char *const VR_RT_SPEED_NAMES[] = { "as_seen", "emulated_when_fast" };
/** Názvy en_VIDEOREC_TURBO_AUDIO. */
static const char *const VR_TURBO_AUDIO_NAMES[] = { "as_heard", "silence", "attenuate" };
/** Názvy en_VIDEOREC_STATE_MARKS. */
static const char *const VR_STATE_MARKS_NAMES[] = { "none", "sidecar" };

/** @brief Počet prvků statického pole. */
#define VR_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const char *videorec_timebase_name(en_VIDEOREC_TIMEBASE v)
{
    return ((int)v >= 0 && (int)v < VR_COUNT(VR_TIMEBASE_NAMES)) ? VR_TIMEBASE_NAMES[v] : VR_TIMEBASE_NAMES[0];
}

bool videorec_timebase_from_name(const char *name, en_VIDEOREC_TIMEBASE *out)
{
    int i = vr_name_index(VR_TIMEBASE_NAMES, VR_COUNT(VR_TIMEBASE_NAMES), name);
    if (i < 0) return false;
    *out = (en_VIDEOREC_TIMEBASE)i;
    return true;
}

const char *videorec_rt_pause_name(en_VIDEOREC_RT_PAUSE v)
{
    return ((int)v >= 0 && (int)v < VR_COUNT(VR_RT_PAUSE_NAMES)) ? VR_RT_PAUSE_NAMES[v] : VR_RT_PAUSE_NAMES[0];
}

bool videorec_rt_pause_from_name(const char *name, en_VIDEOREC_RT_PAUSE *out)
{
    int i = vr_name_index(VR_RT_PAUSE_NAMES, VR_COUNT(VR_RT_PAUSE_NAMES), name);
    if (i < 0) return false;
    *out = (en_VIDEOREC_RT_PAUSE)i;
    return true;
}

const char *videorec_rt_speed_name(en_VIDEOREC_RT_SPEED v)
{
    return ((int)v >= 0 && (int)v < VR_COUNT(VR_RT_SPEED_NAMES)) ? VR_RT_SPEED_NAMES[v] : VR_RT_SPEED_NAMES[0];
}

bool videorec_rt_speed_from_name(const char *name, en_VIDEOREC_RT_SPEED *out)
{
    int i = vr_name_index(VR_RT_SPEED_NAMES, VR_COUNT(VR_RT_SPEED_NAMES), name);
    if (i < 0) return false;
    *out = (en_VIDEOREC_RT_SPEED)i;
    return true;
}

const char *videorec_turbo_audio_name(en_VIDEOREC_TURBO_AUDIO v)
{
    return ((int)v >= 0 && (int)v < VR_COUNT(VR_TURBO_AUDIO_NAMES)) ? VR_TURBO_AUDIO_NAMES[v]
                                                                       : VR_TURBO_AUDIO_NAMES[0];
}

bool videorec_turbo_audio_from_name(const char *name, en_VIDEOREC_TURBO_AUDIO *out)
{
    int i = vr_name_index(VR_TURBO_AUDIO_NAMES, VR_COUNT(VR_TURBO_AUDIO_NAMES), name);
    if (i < 0) return false;
    *out = (en_VIDEOREC_TURBO_AUDIO)i;
    return true;
}

const char *videorec_state_marks_name(en_VIDEOREC_STATE_MARKS v)
{
    return ((int)v >= 0 && (int)v < VR_COUNT(VR_STATE_MARKS_NAMES)) ? VR_STATE_MARKS_NAMES[v]
                                                                       : VR_STATE_MARKS_NAMES[1];
}

bool videorec_state_marks_from_name(const char *name, en_VIDEOREC_STATE_MARKS *out)
{
    int i = vr_name_index(VR_STATE_MARKS_NAMES, VR_COUNT(VR_STATE_MARKS_NAMES), name);
    if (i < 0) return false;
    *out = (en_VIDEOREC_STATE_MARKS)i;
    return true;
}

volatile gint g_videorec_active = 0;

/**
 * Zámek chybových textů (g_videorec_last_error, g_vr_writer_error). Je to
 * "listový" zámek: pod ním se nebere žádný jiný zámek, takže ho smí brát
 * emu, UI i writer vlákno kdykoli (i pod g_vr.mutex). Statický GMutex
 * nepotřebuje inicializaci.
 */
static GMutex g_vr_err_mutex;
/** Text poslední chyby (anglicky). Pod g_vr_err_mutex. */
static char g_videorec_last_error[256];

bool videorec_is_supported(void)
{
    return true; /* všechny platformy (MZ-700 PAL/NTSC, MZ-800, MZ-1500), parametry viz g_vr_platform */
}

/** @brief Nastaví g_videorec_last_error (pod g_vr_err_mutex). @param msg Anglický text. */
static void vr_set_error(const char *msg)
{
    g_mutex_lock(&g_vr_err_mutex);
    g_strlcpy(g_videorec_last_error, msg, sizeof(g_videorec_last_error));
    g_mutex_unlock(&g_vr_err_mutex);
}

const char *videorec_get_last_error(void)
{
    /* Kopie per vlákno: volající dostane konzistentní text i při souběžném zápisu. */
    static _Thread_local char copy[sizeof(g_videorec_last_error)];
    g_mutex_lock(&g_vr_err_mutex);
    g_strlcpy(copy, g_videorec_last_error, sizeof(copy));
    g_mutex_unlock(&g_vr_err_mutex);
    return copy;
}

/** Text poslední události nahrávání (anglicky, viz videorec_get_last_event()). Pod g_vr_err_mutex. */
static char g_vr_last_event[sizeof(((st_VIDEOREC_EVENT *)0)->text)];
/**
 * Kruhový buffer posledních událostí; událost `seq` je na indexu
 * `seq % VIDEOREC_EVENT_RING`. Pod g_vr_err_mutex.
 */
static st_VIDEOREC_EVENT g_vr_events[VIDEOREC_EVENT_RING];
/**
 * Pořadové číslo poslední události. Zapisuje se pod g_vr_err_mutex až po
 * zápisu dat události (bez zámku se jen atomicky čte).
 */
static volatile gint g_vr_event_seq = 0;

/**
 * @brief Zapíše událost do kruhového bufferu, nastaví text poslední události a zvýší pořadové číslo.
 * @param kind  Druh události.
 * @param frame Index / počet snímků (význam podle druhu).
 * @param path  Cesta k nahrávce, nebo NULL.
 * @param text  Anglický text (zkopíruje se, zkrátí na velikost bufferu); u
 *              VIDEOREC_EVENT_FAILED text chyby - text poslední události pak
 *              bude "Recording failed: <text>".
 * @note Bere jen listový zámek g_vr_err_mutex - smí se volat i pod g_vr.mutex
 *       a z libovolného vlákna (emu, writer).
 */
static void vr_emit_event(en_VIDEOREC_EVENT kind, uint64_t frame, const char *path, const char *text)
{
    g_mutex_lock(&g_vr_err_mutex);
    uint32_t seq = (uint32_t)g_atomic_int_get(&g_vr_event_seq) + 1;
    if (seq == 0) seq = 1; /* 0 je vyhrazená pro "žádná událost" */
    st_VIDEOREC_EVENT *e = &g_vr_events[seq % VIDEOREC_EVENT_RING];
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    e->kind = kind;
    e->frame = frame;
    if (path) g_strlcpy(e->path, path, sizeof(e->path));
    g_strlcpy(e->text, text, sizeof(e->text));
    if (kind == VIDEOREC_EVENT_FAILED) {
        g_snprintf(g_vr_last_event, sizeof(g_vr_last_event), "Recording failed: %s", text);
    } else {
        g_strlcpy(g_vr_last_event, text, sizeof(g_vr_last_event));
    }
    g_atomic_int_set(&g_vr_event_seq, (gint)seq);
    g_mutex_unlock(&g_vr_err_mutex);
}

bool videorec_get_event(uint32_t seq, st_VIDEOREC_EVENT *out)
{
    bool ok = false;
    g_mutex_lock(&g_vr_err_mutex);
    const st_VIDEOREC_EVENT *e = &g_vr_events[seq % VIDEOREC_EVENT_RING];
    if (seq != 0 && e->seq == seq) {
        *out = *e;
        ok = true;
    }
    g_mutex_unlock(&g_vr_err_mutex);
    return ok;
}

/**
 * @brief Výchozí adresář nahrávek, když output_dir není nastaven.
 * @param out  Výstupní buffer.
 * @param size Velikost bufferu.
 * @post `out` obsahuje existující adresář (`<home_dir>/videos`, Videa uživatele, nebo ".").
 */
static void vr_default_output_dir(char *out, size_t size)
{
    if (g_sdlapp && g_sdlapp->paths && g_sdlapp->paths->home_dir) {
        char *dir = g_build_filename(g_sdlapp->paths->home_dir, "videos", NULL);
        bool ok = (g_mkdir_with_parents(dir, 0755) == 0) && g_file_test(dir, G_FILE_TEST_IS_DIR);
        if (ok) g_strlcpy(out, dir, size);
        g_free(dir);
        if (ok) return;
    }
    const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    g_strlcpy(out, (videos && g_file_test(videos, G_FILE_TEST_IS_DIR)) ? videos : ".", size);
}

/**
 * @brief Výstupní adresář podle nastavení (viz videorec_resolve_output_dir()).
 * @param configured Nastavený adresář (prázdný = výchozí).
 * @param out        Výstupní buffer.
 * @param size       Velikost bufferu.
 * @return true pokud výsledný adresář existuje.
 */
static bool vr_resolve_dir(const char *configured, char *out, size_t size)
{
    if (!configured[0]) {
        vr_default_output_dir(out, size);
        return true;
    }
    g_strlcpy(out, configured, size);
    (void)g_mkdir_with_parents(out, 0755);
    return g_file_test(out, G_FILE_TEST_IS_DIR);
}

bool videorec_resolve_output_dir(char *out, size_t size)
{
    return vr_resolve_dir(g_videorec_settings.output_dir, out, size);
}

const char *videorec_get_last_event(void)
{
    static _Thread_local char copy[sizeof(g_vr_last_event)];
    g_mutex_lock(&g_vr_err_mutex);
    g_strlcpy(copy, g_vr_last_event, sizeof(copy));
    g_mutex_unlock(&g_vr_err_mutex);
    return copy;
}

uint32_t videorec_get_event_seq(void)
{
    return (uint32_t)g_atomic_int_get(&g_vr_event_seq);
}

/**
 * Zámek zveřejněného souhrnného stavu (g_vr_status, g_vr_status_gen,
 * g_vr_wstat_*). Listový zámek jako g_vr_err_mutex: pod ním se jen kopírují
 * data a nebere se žádný jiný zámek, takže ho smí brát emu vlákno (pod
 * g_vr.mutex), writer vlákno i UI vlákno - a UI tak nikdy nečeká na emulaci.
 */
static GMutex g_vr_status_mutex;
/** Zveřejněný stav lepidla (bez `bytes` a `parts`). Pod g_vr_status_mutex. */
static st_VIDEOREC_STATUS g_vr_status;
/** Generace session, ke které patří g_vr_status (0 = zatím žádná). Pod g_vr_status_mutex. */
static uint32_t g_vr_status_gen = 0;
/** Generace session, ke které patří údaje writeru (g_vr_wstat_bytes, g_vr_wstat_parts). Pod g_vr_status_mutex. */
static uint32_t g_vr_wstat_gen = 0;
/** Bajty všech partů session g_vr_wstat_gen (viz st_VIDEOREC_STATUS::bytes). Pod g_vr_status_mutex. */
static uint64_t g_vr_wstat_bytes = 0;
/** Počet partů session g_vr_wstat_gen. Pod g_vr_status_mutex. */
static unsigned g_vr_wstat_parts = 0;

void videorec_get_status(st_VIDEOREC_STATUS *out)
{
    g_mutex_lock(&g_vr_status_mutex);
    *out = g_vr_status;
    bool same = (g_vr_status_gen != 0) && (g_vr_wstat_gen == g_vr_status_gen);
    out->bytes = same ? g_vr_wstat_bytes : 0;
    out->parts = same ? g_vr_wstat_parts : 0;
    g_mutex_unlock(&g_vr_status_mutex);
}

#include "audio.h"
#include "display.h"
#include "emulator.h"
#include "hw-generic/gdg/framebuffer.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/psg/psg.h"
#include "libs/avirec/avi_writer.h"
#include "libs/avirec/zmbv_enc.h"
#include "videorec_pipe.h"
#include "videorec_platform_arch.h"

/**
 * Parametry platformy, pro kterou je lepidlo přeložené (MZ-700 PAL/NTSC,
 * MZ-800, MZ-1500): rozměry framebufferu, canvas, takty na snímek, GDG
 * takty, fps a počet zvukových kanálů z per-arch maker. Kontrolu
 * invariantů a frekvence zvuku dělá videorec_request_start().
 */
static const st_VIDEOREC_PLATFORM g_vr_platform = VIDEOREC_PLATFORM_CURRENT_INIT;

/* Počet kanálů v parametrech platformy musí odpovídat audio logu a vejít se do rendereru. */
G_STATIC_ASSERT(VIDEOREC_PLATFORM_AUDIO_CHANNELS == AUDIO_SRC_CHANNELS_COUNT);
G_STATIC_ASSERT(AUDIO_SRC_CHANNELS_COUNT <= VIDEOREC_AUDIO_MAX_CHANNELS);
/* Snímek trvá celý počet GDG taktů (GDGCLK_BASE je definovaný jako VIDEO_SCREEN_TICKS * fps). */
G_STATIC_ASSERT(GDGCLK_BASE == VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC);

/**
 * @brief Velikost bloku SDL cesty [stereo vzorky] na této platformě.
 *
 * Výstup jedné synchronizační události emulace (jeden snímek): 882 při 50,
 * 735 při 60 snímcích/s (iface_audio_wait_for_data()). Režim podle reality
 * podle ní pozná, zda jitter buffer potřebuje další blok. Snímková frekvence
 * jen z g_vr_platform (jediný zdroj fps v lepidle); že blok vyjde celý,
 * hlídá G_STATIC_ASSERT v iface_audio.c.
 *
 * @return videorec_rt_sdl_chunk() pro fps platformy.
 */
static inline unsigned vr_sdl_chunk(void)
{
    return videorec_rt_sdl_chunk(g_vr_platform.fps_num, g_vr_platform.fps_den);
}

/**
 * @brief Zpoždění obrazu v režimu podle reality [ticky] na této platformě.
 * @return videorec_rt_video_delay_ticks() pro fps platformy (2 při 50 i 60 snímcích/s).
 */
static inline unsigned vr_rt_delay_ticks(void)
{
    return videorec_rt_video_delay_ticks(g_vr_platform.fps_num, g_vr_platform.fps_den);
}

unsigned videorec_get_fps(void)
{
    unsigned n = g_vr_platform.fps_num / (g_vr_platform.fps_den ? g_vr_platform.fps_den : 1u);
    return n ? n : 1u;
}

/** Šířka zaznamenávaného snímku (celý framebuffer včetně borderu). */
#define VR_WIDTH VIDEO_DISPLAY_WIDTH
/** Výška zaznamenávaného snímku (celý framebuffer včetně borderu); tolik řádků kopíruje emu vlákno. */
#define VR_HEIGHT VIDEO_DISPLAY_HEIGHT
/**
 * Výška snímku v AVI: každý řádek framebufferu dvakrát (poměr stran jako okno
 * emulátoru). Zdvojení dělá writer vlákno (vr_double_lines()).
 */
#define VR_AVI_HEIGHT (2 * VR_HEIGHT)
/** Backpressure: nad touto délkou fronty writeru začne emu vlákno čekat. */
#define VR_QUEUE_HIGH 250
/** Backpressure: čekání skončí, když fronta klesne pod tuto délku. */
#define VR_QUEUE_LOW 200

/** @brief Typ zprávy pro writer vlákno. */
typedef enum {
    VR_MSG_OPEN = 0,  /**< Začátek session: převzít otevřený AVI zapisovač, vytvořit enkodér. */
    VR_MSG_FRAME,     /**< Zakódovat a zapsat jeden snímek (pixely + zvuk). */
    VR_MSG_TRUNCATE,  /**< Zahodit snímky od globálního indexu `frame_count` (retake). */
    VR_MSG_SIDECAR,   /**< Doplnit party do sidecar modelu, uložit ho a uvolnit. */
    VR_MSG_CLOSE,     /**< Finalizovat a zavřít AVI, uvolnit enkodér (konec session). */
    VR_MSG_SYNC,      /**< Bariéra: ohlásit stav writeru po zpracování všech dřívějších zpráv. */
    VR_MSG_QUIT       /**< Zavřít vše otevřené a ukončit vlákno. */
} en_VR_MSG;

/**
 * @brief Bariéra lepidla s writerem (viz vr_writer_sync()).
 *
 * Žije na zásobníku odesílatele (emu vlákno), který čeká, dokud writer
 * nenastaví `done`. Writer ji vyplní pod `mutex`, signalizuje a po odemčení
 * na ni už nesahá (odesílatel ji pak zruší).
 *
 * Invariant: `done == false` dokud writer nevyplnil `part_first`.
 */
typedef struct st_VR_SYNC {
    GMutex mutex;        /**< Zámek bariéry. */
    GCond cond;          /**< Signál dokončení. */
    bool done;           /**< Writer zprávu zpracoval. */
    uint64_t part_first; /**< Globální index prvního snímku aktuálního partu writeru. */
} st_VR_SYNC;

/**
 * @brief Zpráva pro writer vlákno.
 *
 * Ownership: všechny ukazatele (`avi`, `path`, `pixels`, `audio`, `sidecar`)
 * přecházejí odesláním na writer, který je uvolní. Výjimka: `sync` patří
 * odesílateli (writer ho jen vyplní). Struktura se alokuje přes g_new0
 * a uvolní writer přes g_free.
 */
typedef struct st_VR_MSG {
    en_VR_MSG type;                 /**< Typ zprávy. */
    uint32_t gen;                   /**< OPEN: generace session (nenulová, roste s každým startem). */
    st_AVI_WRITER *avi;             /**< OPEN: otevřený zapisovač prvního partu. */
    char *path;                     /**< OPEN: cesta prvního partu; SIDECAR: cílová cesta sidecaru. */
    st_AVI_WRITER_PARAMS params;    /**< OPEN: parametry pro otevírání dalších partů (rozměry AVI, tj. VR_WIDTH x VR_AVI_HEIGHT). */
    unsigned keyframe_interval;     /**< OPEN: interval klíčových snímků enkodéru. */
    uint8_t *pixels;                /**< FRAME: VR_WIDTH*VR_HEIGHT indexů barev (0..15), nativní framebuffer bez zdvojení. */
    int16_t *audio;                 /**< FRAME: stereo vzorky snímku. */
    size_t audio_frames;            /**< FRAME: počet stereo vzorků. */
    uint64_t index;                 /**< FRAME: globální index snímku. */
    uint64_t frame_count;           /**< TRUNCATE: počet ponechaných snímků (globálně). */
    st_VIDEOREC_SIDECAR *sidecar;   /**< SIDECAR: model (vlastnictví přechází na writer). */
    st_VR_SYNC *sync;               /**< SYNC: bariéra (vlastní odesílatel). */
} st_VR_MSG;

/**
 * @brief Větev (take) časové osy nahrávky - viz st_VIDEOREC_SNAPINFO.
 *
 * Linie (`g_vr.takes`) je posloupnost větví s neklesajícím `start`; větev
 * i pokrývá v nahrávce snímky `start(i) .. start(i+1)` (poslední až do
 * aktuálního počtu snímků).
 */
typedef struct st_VR_TAKE {
    uint64_t take_id; /**< Náhodné nenulové ID větve. */
    uint64_t start;   /**< Globální index prvního snímku větve. */
    bool realtime;    /**< Větev nahraná v režimu podle reality (snapshot z ní nikdy nevede na retake). */
} st_VR_TAKE;

/** @brief Počet pamatovaných stavů zvuku pro body snapshotů (nejstarší se přepisují). */
#define VR_AUDIO_STATES 64

/** @brief Maximální počet událostí zvuku držených během ochrany švu (cca 6 snímků plného PSG). */
#define VR_HOLD_MAX_EVENTS 262144u

/**
 * @brief Stav audio rendereru zachycený pro bod snapshotu (plynulý retake).
 *
 * Klíčem je bod snapshotu (`take_id`, `frame`) ze st_VIDEOREC_SNAPINFO; stav
 * je stav rendereru na konci snímku `frame - 1` (posledního snímku před
 * bodem). Zachytává se při uložení snapshotu (videorec_get_snapinfo());
 * pokud snímek ještě čeká na zvuk, až při jeho vyrenderování - do té doby
 * je `valid == false`.
 *
 * Invariant: záznam ve slotu `seq % VR_AUDIO_STATES` patří požadavku `seq`
 * (0 = prázdný slot); starší požadavek, jehož slot se mezitím přepsal, se
 * při zachycení pozná podle nesouhlasu `seq` a zahodí.
 */
typedef struct st_VR_AUDIO_SNAP {
    uint64_t seq;                  /**< Pořadové číslo požadavku (nenulové; 0 = volný slot). */
    uint64_t take_id;              /**< Větev snapshotu. */
    uint64_t frame;                /**< Snímek snapshotu (globální index dalšího snímku). */
    bool valid;                    /**< Stav už zachycený (`state` platný). */
    st_VIDEOREC_AUDIO_STATE state; /**< Stav rendereru na konci snímku `frame - 1`. */
} st_VR_AUDIO_SNAP;

/** @brief Událost zvuku podržená během ochrany švu (`audio_hold`). */
typedef struct st_VR_HOLD_EVENT {
    uint64_t ticks; /**< Absolutní čas změny. */
    uint8_t ch;     /**< Kanál. */
    uint8_t value;  /**< Nová hodnota. */
} st_VR_HOLD_EVENT;

/**
 * @brief Stav lepidla (vše pod `mutex`, kromě front a atomických příznaků).
 *
 * Invarianty:
 * - `state != IDLE` <=> bit VIDEOREC_FLAG_ACTIVE v `g_videorec_active` <=> `pipe` a `sidecar` jsou platné;
 * - `audio_hold` => tap se nepouští do pipe (jen do `hold_events`) a horizont se ignoruje,
 *   na nejbližším konci snímku vznikne šev nebo retake;
 * - `state != IDLE` <=> `hold_events != NULL`; mimo `audio_hold` je prázdný;
 * - `req_start` => `start_avi != NULL` (soubor otevřený, čeká na předání writeru);
 * - `state != IDLE` <=> `pause_frames != NULL`; všechny jeho prvky jsou <= počet snímků nahrávky;
 * - `state != IDLE` <=> `takes != NULL`; linie má aspoň 1 větev, první začíná
 *   na 0, `start` neklesá a žádný není větší než počet snímků nahrávky;
 * - `queue` a `writer` jsou nenulové právě když `initialized`;
 * - `rt_on` <=> bit VIDEOREC_FLAG_RT; `rt_on` => `state != IDLE` a `rt_fifo != NULL`;
 *   `rt_leaving` => `rt_on`; `state != IDLE` <=> `rt_fifo != NULL`;
 * - v realtime pipe nemá čekající snímky a její `next_index` je index dalšího
 *   zapsaného snímku (vzorkovač ho posouvá).
 *
 * Ownership: `sidecar` vlastní lepidlo do ukončení session (pak přejde na
 * writer), `start_avi` vlastní lepidlo do zpracování startu (pak přejde na
 * writer), `req_markers` vlastní řetězce popisků, `pause_frames`, `takes`
 * a `hold_events` vlastní lepidlo po dobu session; `audio_states` je pevné pole
 * (vynuluje se při startu session).
 */
typedef struct st_VR_GLUE {
    GMutex mutex;                    /**< Zámek celé struktury (staticky alokovaný, nemusí se inicializovat). */
    bool initialized;                /**< Proběhl videorec_init() (a ne videorec_exit()). */
    en_VIDEOREC_STATE state;         /**< Stav nahrávání. */
    uint64_t session_id;             /**< ID session (0 = nenahrává se). */
    uint32_t gen;                    /**< Generace aktuální session (pro párování chyb writeru). */
    st_VIDEOREC_PIPE pipe;           /**< Párování obrazu a zvuku (platné jen při nahrávání). */
    st_VIDEOREC_SIDECAR *sidecar;    /**< Sidecar model (platný jen při nahrávání). */
    char sidecar_path[1100];         /**< Cesta k `<jméno>.cuts.json`. */
    uint64_t frames;                 /**< Počet snímků nahrávky po posledním konci snímku. */
    /* parametry session (kopie nastavení z okamžiku požadavku na start) */
    en_VIDEOREC_TRANSITION transition; /**< Výchozí přechod na švech. */
    unsigned transition_ms;            /**< Délka přechodu [ms]. */
    unsigned audio_rate;               /**< Vzorkovací frekvence [Hz]. */
    en_VIDEOREC_RETAKE retake_mode;    /**< Režim retake. */
    unsigned keyframe_interval;        /**< Interval klíčových snímků. */
    uint64_t stop_after_frames;        /**< Automatický stop po N snímcích (0 = ne). */
    bool quit_after_stop;              /**< Po automatickém stopu ukončit emulátor. */
    /* požadavky */
    bool req_start;                  /**< Čeká start. */
    st_VIDEOREC_START start;         /**< Parametry čekajícího startu (path = skutečná cesta). */
    st_AVI_WRITER *start_avi;        /**< Otevřený soubor čekajícího startu. */
    bool req_stop;                   /**< Čeká stop. */
    bool req_pause_toggle;           /**< Čeká přepnutí record-pause. */
    GPtrArray *req_markers;          /**< Čekající popisky markerů (char*, g_free). */
    bool req_snapshot;               /**< Čeká zpracování nahraného snapshotu. */
    bool snap_has_info;              /**< Snapshot nesl videorec informace. */
    st_VIDEOREC_SNAPINFO snap_info;  /**< Informace ze snapshotu (platné při snap_has_info). */
    /* ochrana zvuku na švu */
    bool audio_hold;                 /**< Časová osa se přerušila: tap jen do hold_events, horizont ignorovat do švu na konci snímku. */
    uint64_t last_tap[AUDIO_SRC_CHANNELS_COUNT]; /**< Čas poslední přijaté události po kanálech (detekce skoku zpět). */
    GArray *hold_events;             /**< Události nové časové osy přijaté během audio_hold (st_VR_HOLD_EVENT); při retake se přehrají do pipe. */
    bool hold_overflow;              /**< Během audio_hold se překročil VR_HOLD_MAX_EVENTS (přehrání se neprovede). */
    st_VR_AUDIO_SNAP audio_states[VR_AUDIO_STATES]; /**< Zachycené stavy zvuku pro body snapshotů (kruhově podle seq). */
    uint64_t audio_state_seq;        /**< Poslední přidělené pořadové číslo požadavku na zachycení. */
    GArray *pause_frames;            /**< Indexy snímků (uint64_t), na kterých v session začala record-pause, včetně přesunu začátku pauzy retakem (neklesající). */
    GArray *takes;                   /**< Linie větví aktuální časové osy (st_VR_TAKE), viz st_VIDEOREC_SNAPINFO. */
    uint32_t part_limit;             /**< Limit velikosti AVI partu pro další start (0 = výchozí; jen testy, viz videorec_test_set_part_limit()). */
    uint32_t start_part_limit;       /**< Limit partu zkopírovaný při požadavku na start (předá se writeru ve VR_MSG_OPEN). */
    /* režim podle reality - nastavení session (kopie z okamžiku požadavku na start) */
    en_VIDEOREC_TIMEBASE timebase;     /**< Požadovaná časová základna (mění i videorec_request_timebase()). */
    en_VIDEOREC_RT_PAUSE rt_pause;     /**< Chování při pauze emulace. */
    uint64_t rt_pause_cap_ticks;       /**< Limit zamrzlé pauzy v ticích (realtime_pause_cap_s * fps platformy). */
    en_VIDEOREC_RT_SPEED rt_speed;     /**< Chování při rychlosti != 100 %. */
    en_VIDEOREC_TURBO_AUDIO turbo_audio; /**< Zvuk při rychlosti > 100 %. */
    bool state_marks;                  /**< Zapisovat události stavu do sidecaru. */
    bool auto_markers;                 /**< Automatické markery. */
    bool record_debugger_steps;        /**< Zapisovat snímky při krokování debuggeru (realtime). */
    /* režim podle reality - stav */
    bool rt_on;                        /**< Efektivní základna realtime (bit VIDEOREC_FLAG_RT); snímky zapisuje vzorkovač. */
    bool rt_leaving;                   /**< Čeká návrat do emulačního času na nejbližším konci emulovaného snímku. */
    GQueue *rt_fifo;                   /**< Zpožďovací fronta obrazu (uint8_t* snímky VR_WIDTH*VR_HEIGHT, g_free); nejvýš vr_rt_delay_ticks(). */
    st_VIDEOREC_RT_CLOCK rt_clock;     /**< Rozvrh ticků vzorkovače (běží po celou session). */
    bool obs_paused;                   /**< Pozorovaný stav pauzy emulace (pro události). */
    char obs_pause_value[16];          /**< Hodnota pause_start aktuální pozorované pauzy ("user", ...); platí při `obs_paused`. */
    unsigned obs_speed;                /**< Pozorovaná rychlost [%], 0 = MAX SPEED (pro události). */
    gint obs_dbg_steps;                /**< Čítač kroků debuggeru (g_vr_dbg_steps) na začátku aktuální pauzy. */
    bool dbg_pause;                    /**< Aktuální pauza je zastavení na dočasném breakpointu (step over / run to cursor);
                                            nastaví ji paused smyčka z g_vr_dbg_run_inflight, zruší ji pozorovaný
                                            běh emulace (konec pauzy, nebo běh po nepozorované pauze), viz vr_observe_pause(). */
    uint64_t rt_pause_ticks;           /**< Ticky od začátku aktuální pauzy emulace (limit freeze_capped). */
    bool req_reset;                    /**< Čeká událost resetu (videorec_on_reset()). */
    bool quit_pending;                 /**< Vzorkovač dosáhl stop_after_frames s quit_after_stop: emu vlákno má ukončit emulátor. */
    uint32_t rt_last_seq;              /**< Pořadí posledního navzorkovaného zobrazeného snímku (statistika opakování). */
    bool rt_have_seq;                  /**< rt_last_seq je platné. */
    en_VIDEOREC_RT_ACTIVITY rt_activity; /**< Činnost posledního ticku (pro stav UI). */
    st_VIDEOREC_RT_STATS rt_stats;     /**< Statistiky režimu podle reality (běžící / poslední session). */
    /* writer */
    GAsyncQueue *queue;              /**< Fronta zpráv pro writer (st_VR_MSG*). */
    GThread *writer;                 /**< Writer vlákno. */
} st_VR_GLUE;

static st_VR_GLUE g_vr;

/**
 * Jitter buffer zvuku SDL výstupu pro režim podle reality. Inicializuje se
 * jednou (první videorec_init()) a nikdy se neuvolňuje: producent (SDL audio
 * callback) může běžet kdykoli, i po videorec_exit(). Mimo realtime je
 * zápis zakázaný (`enabled == false`).
 */
static st_VIDEOREC_RT_AUDIO g_vr_rta;
/** g_vr_rta je inicializovaný (atomicky; producent před inicializací nic nedělá). */
static volatile gint g_vr_rta_ready = 0;

/**
 * Zámek slotu posledního zobrazeného snímku (g_vr_rtv_pixels, g_vr_rtv_seq).
 * Listový zámek: pod ním se jen kopíruje snímek.
 */
static GMutex g_vr_rtv_mutex;
/** Poslední zobrazený snímek (indexy barev bez masky), viz videorec_rt_video_tap(). Pod g_vr_rtv_mutex. */
static uint8_t g_vr_rtv_pixels[VR_WIDTH * VR_HEIGHT];
/** Pořadí posledního zobrazeného snímku (roste s každým tapem). Pod g_vr_rtv_mutex. */
static uint32_t g_vr_rtv_seq = 0;

/** Čítač dokončených kroků debuggeru (videorec_on_debugger_step()); atomický. */
static volatile gint g_vr_dbg_steps = 0;
/**
 * Probíhá běh k dočasnému breakpointu - step over, run to cursor (0/1, atomicky).
 * Nastaví ho videorec_on_debugger_run() před zrušením pauzy, zruší ho emu
 * vlákno při vstupu do paused smyčky, jakmile emulace znovu stojí (zastavení
 * na dočasném breakpointu, viz videorec_on_emulation_stopped()) - nezávisle
 * na tom, zda vzorkovač běh vůbec zaznamenal, a dřív, než paused smyčka
 * zpracuje frontu dbgapi (pokračování zadané hned po kroku tak příznak
 * nepřežije). Zrušení i převod na `g_vr.dbg_pause` probíhá pod g_vr.mutex,
 * takže tick vzorkovače nikdy nevidí mezistav "příznak zrušen, dbg_pause
 * ještě nenastaveno".
 */
static volatile gint g_vr_dbg_run_inflight = 0;

/**
 * @brief Řízení vlákna vzorkovače "videorec-rt".
 *
 * Invarianty: `thread != NULL` mezi videorec_init() a videorec_exit();
 * vlákno drží `mutex` jen během čekání (nikdy při ticku), takže pořadí zámků
 * je vždy g_vr.mutex -> mutex.
 */
typedef struct st_VR_RTCTL {
    GMutex mutex;     /**< Zámek řízení (listový vůči g_vr.mutex). */
    GCond cond;       /**< Probuzení vlákna (změna run/manual/quit). */
    GThread *thread;  /**< Vlákno vzorkovače. */
    bool quit;        /**< Ukončit vlákno. */
    bool run;         /**< Probíhá session - tickovat. */
    bool manual;      /**< Jen testy: netickovat samo (videorec_test_rt_tick()). */
    int64_t deadline; /**< Termín dalšího ticku [us monotónních hodin]; 0 = hned. */
} st_VR_RTCTL;

/** Řízení vlákna vzorkovače. */
static st_VR_RTCTL g_vr_rtc;

/** Generace session, ve které writer narazil na chybu (0 = žádná). */
static volatile gint g_vr_failed_gen = 0;
/** Text chyby writeru; pod g_vr_err_mutex, zapíše se před nastavením g_vr_failed_gen. */
static char g_vr_writer_error[256];

/** @brief Atomicky nastaví bity v g_videorec_active. @param f Bity VIDEOREC_FLAG_*. */
static inline void vr_flag_set(guint f)
{
    g_atomic_int_or((volatile guint *)&g_videorec_active, f);
}

/** @brief Atomicky smaže bity v g_videorec_active. @param f Bity VIDEOREC_FLAG_*. */
static inline void vr_flag_clear(guint f)
{
    g_atomic_int_and((volatile guint *)&g_videorec_active, ~f);
}

/*******************************************************************************
 *
 *                  Writer vlákno
 *
 ******************************************************************************/

/** @brief Part AVI souboru zapsaný writerem v aktuální session. */
typedef struct st_VR_PART {
    char *filename;       /**< Jméno souboru bez cesty (g_free). */
    uint64_t first_frame; /**< Globální index prvního snímku partu. */
} st_VR_PART;

/** @brief Uvolní st_VR_PART (GDestroyNotify pro GPtrArray). @param p Part. */
static void vr_part_free(gpointer p)
{
    st_VR_PART *part = p;
    g_free(part->filename);
    g_free(part);
}

/**
 * @brief Lokální stav writer vlákna (žije jen na zásobníku vlákna).
 *
 * Invarianty: `avi != NULL` => `enc != NULL` nebo `failed`;
 * `first_path != NULL` <=> session otevřená (mezi VR_MSG_OPEN a VR_MSG_CLOSE / VR_MSG_QUIT);
 * `dbl` je platný po celou dobu běhu vlákna.
 */
typedef struct st_VR_WRITER {
    st_AVI_WRITER *avi;          /**< Aktuální part (NULL = nic otevřeno). */
    st_ZMBV_ENC *enc;            /**< Enkodér session. */
    st_AVI_WRITER_PARAMS params; /**< Parametry pro další party. */
    char *base;                  /**< Cesta prvního partu bez přípony ".avi". */
    char *first_path;            /**< Cesta prvního partu (pro událost SAVED / FAILED); g_free. */
    uint64_t frames;             /**< Počet snímků nahrávky po posledním zápisu / truncate. */
    unsigned part;               /**< Číslo aktuálního partu (1 = první). */
    uint64_t part_first;         /**< Globální index prvního snímku aktuálního partu. */
    GPtrArray *parts;            /**< Seznam partů session (st_VR_PART*). */
    bool force_key;              /**< Příští snímek musí být klíčový (po truncate). */
    bool failed;                 /**< Session skončila chybou zápisu - další snímky se zahazují. */
    uint32_t gen;                /**< Generace aktuální session. */
    uint64_t closed_bytes;       /**< Součet velikostí (na disku) už uzavřených partů session. */
    uint8_t *dbl;              /**< Buffer snímku se zdvojenými řádky (VR_WIDTH*VR_AVI_HEIGHT), znovu použitý pro každý snímek; g_free na konci vlákna. */
} st_VR_WRITER;

/**
 * @brief Zdvojí řádky snímku: řádek k zdroje se zapíše na řádky 2k a 2k+1 cíle.
 * @param dst Cíl, VR_WIDTH*VR_AVI_HEIGHT bajtů.
 * @param src Zdroj, VR_WIDTH*VR_HEIGHT bajtů (nativní framebuffer).
 * @pre `dst` a `src` se nepřekrývají.
 */
static void vr_double_lines(uint8_t *dst, const uint8_t *src)
{
    for (unsigned y = 0; y < VR_HEIGHT; y++) {
        const uint8_t *row = src + (size_t)y * VR_WIDTH;
        memcpy(dst + (size_t)(2 * y) * VR_WIDTH, row, VR_WIDTH);
        memcpy(dst + (size_t)(2 * y + 1) * VR_WIDTH, row, VR_WIDTH);
    }
}

/**
 * @brief Označí session jako neúspěšnou: zapíše text chyby a nastaví g_vr_failed_gen.
 * @param w   Stav writeru.
 * @param msg Anglický text chyby.
 */
static void vr_writer_fail(st_VR_WRITER *w, const char *msg)
{
    if (w->failed) return;
    w->failed = true;
    g_mutex_lock(&g_vr_err_mutex);
    g_strlcpy(g_vr_writer_error, msg, sizeof(g_vr_writer_error));
    g_mutex_unlock(&g_vr_err_mutex);
    fprintf(stderr, "[videorec] ERROR: %s\n", msg);
    g_atomic_int_set(&g_vr_failed_gen, (gint)w->gen);
}

/**
 * @brief Zveřejní velikost a počet partů session pro videorec_get_status().
 *
 * Velikost = uzavřené party (velikost na disku) + otevřený part
 * (avi_writer_bytes(), bez indexu).
 *
 * @param w Stav writeru (session otevřená, `first_path != NULL`).
 * @note Bere jen listový zámek g_vr_status_mutex.
 */
static void vr_writer_publish(const st_VR_WRITER *w)
{
    uint64_t bytes = w->closed_bytes + avi_writer_bytes(w->avi);
    g_mutex_lock(&g_vr_status_mutex);
    g_vr_wstat_gen = w->gen;
    g_vr_wstat_bytes = bytes;
    g_vr_wstat_parts = w->part;
    g_mutex_unlock(&g_vr_status_mutex);
}

/**
 * @brief Cesta partu číslo `n` aktuální session.
 * @param w Stav writeru (`first_path` a `base` platné).
 * @param n Číslo partu (1 = první).
 * @return Nový řetězec (g_free).
 */
static char *vr_writer_part_path(const st_VR_WRITER *w, unsigned n)
{
    return (n == 1) ? g_strdup(w->first_path) : g_strdup_printf("%s_%03u.avi", w->base, n);
}

/**
 * @brief Přičte velikost uzavřeného partu `n` (na disku) k `closed_bytes`.
 * @param w Stav writeru (`first_path` a `base` platné).
 * @param n Číslo právě uzavřeného partu.
 */
static void vr_writer_add_closed_part(st_VR_WRITER *w, unsigned n)
{
    char *path = vr_writer_part_path(w, n);
    GStatBuf st;
    if (g_stat(path, &st) == 0) w->closed_bytes += (uint64_t)st.st_size;
    g_free(path);
}

/**
 * @brief Odstraní aktuální part, pokud je druhý nebo další a nemá žádný snímek.
 *
 * Prázdný part vznikne, když retake zkrátí nahrávku přesně na první snímek
 * aktuálního partu (truncate na 0 snímků partu) a session skončí dřív, než se
 * do partu zapíše další snímek (stop ve stejném snímku). Takový soubor nemá
 * v nahrávce žádný obsah: zavře se, smaže z disku a vyřadí ze seznamu partů
 * (do sidecaru se nedostane), počet partů se sníží. První part se nemaže
 * ani prázdný - je to soubor, který uživatel zadal a dostane v události SAVED.
 *
 * Volá se před zápisem partů do sidecaru (VR_MSG_SIDECAR) a před zavřením
 * session (vr_writer_close()); je idempotentní. Po odstranění už do session
 * nesmí přijít snímek (lepidlo posílá SIDECAR a CLOSE až po posledním snímku).
 *
 * @param w Stav writeru.
 * @post Je-li `w->part > 1`, aktuální part má aspoň 1 snímek, nebo už žádný
 *       part otevřený není (`w->avi == NULL`, poslední part z `parts` i z disku
 *       odstraněn, `w->part` snížen).
 * @note Selhání smazání souboru se jen vypíše na stderr (part už je vyřazený
 *       ze sidecaru, export ho proto nepoužije); session se kvůli tomu
 *       neoznačí jako neúspěšná.
 */
static void vr_writer_drop_empty_part(st_VR_WRITER *w)
{
    if (!w->avi || w->part < 2 || !w->first_path || !w->base) return;
    if (avi_writer_frame_count(w->avi) != 0) return;
    char *path = vr_writer_part_path(w, w->part);
    if (avi_writer_close(w->avi) != AVI_WRITER_OK) {
        fprintf(stderr, "[videorec] WARNING: failed to finalize the empty AVI part %u\n", w->part);
    }
    w->avi = NULL;
    if (g_remove(path) != 0) {
        fprintf(stderr, "[videorec] WARNING: cannot delete the empty video file part: %s\n", path);
    } else {
        fprintf(stderr, "[videorec] Deleted the empty video file part: %s\n", path);
    }
    g_free(path);
    if (w->parts->len > 0) g_ptr_array_remove_index(w->parts, w->parts->len - 1);
    w->part--;
}

/**
 * @brief Zavře aktuální part, uvolní enkodér a ohlásí výsledek session.
 *
 * Prázdný druhý nebo další part se předtím odstraní (vr_writer_drop_empty_part()).
 * Selhání finalizace AVI označí session jako neúspěšnou. Byla-li otevřená
 * session (`first_path`), vydá událost VIDEOREC_EVENT_SAVED, nebo
 * VIDEOREC_EVENT_FAILED s textem chyby writeru - tak se do UI dostanou
 * i chyby, které nastanou až při dobíhání writeru po stopu.
 *
 * @param w Stav writeru.
 * @post Nic otevřeno, `first_path == NULL`.
 */
static void vr_writer_close(st_VR_WRITER *w)
{
    vr_writer_drop_empty_part(w);
    if (w->avi) {
        if (avi_writer_close(w->avi) != AVI_WRITER_OK) {
            vr_writer_fail(w, "Cannot finalize the video file");
        }
        w->avi = NULL;
        if (w->first_path && w->base) vr_writer_add_closed_part(w, w->part);
    }
    zmbv_enc_free(w->enc);
    w->enc = NULL;
    g_free(w->base);
    w->base = NULL;

    if (w->first_path) {
        /* Konečná velikost dřív než událost: UI ji při SAVED / FAILED už vidí. */
        vr_writer_publish(w);
        char *ev;
        if (w->failed) {
            char err[sizeof(g_vr_writer_error)];
            g_mutex_lock(&g_vr_err_mutex);
            g_strlcpy(err, g_vr_writer_error[0] ? g_vr_writer_error : "Video file write error", sizeof(err));
            g_mutex_unlock(&g_vr_err_mutex);
            vr_emit_event(VIDEOREC_EVENT_FAILED, w->frames, w->first_path, err);
        } else {
            ev = g_strdup_printf("Recording saved: %s", w->first_path);
            vr_emit_event(VIDEOREC_EVENT_SAVED, w->frames, w->first_path, ev);
            g_free(ev);
        }
        g_free(w->first_path);
        w->first_path = NULL;
    }
}

/**
 * @brief Zdvojí řádky snímku, zakóduje ho a zapíše; při plném partu otevře další part.
 *
 * Nativní snímek (VR_WIDTH x VR_HEIGHT) se nejdřív rozepíše do `w->dbl`
 * (vr_double_lines()); enkodér i AVI pracují s rozměrem VR_WIDTH x VR_AVI_HEIGHT.
 *
 * Po AVI_WRITER_ERR_FULL se part zavře, otevře se `<base>_NNN.avi` a snímek
 * se zakóduje znovu jako klíčový (nový soubor musí začínat klíčovým snímkem).
 * Při AVI_WRITER_ERR_IO se session označí jako neúspěšná (další snímky se
 * zahazují; rozepsaný chunk zůstane v souboru, soubor je přesto čitelný).
 *
 * @param w Stav writeru.
 * @param m Zpráva VR_MSG_FRAME (pixely a zvuk zůstávají ve vlastnictví volajícího).
 * @post `w->dbl` obsahuje zdvojený snímek `m` (přepíše se dalším snímkem).
 */
static void vr_writer_frame(st_VR_WRITER *w, const st_VR_MSG *m)
{
    const uint8_t *out = NULL;
    size_t out_size = 0;
    bool key = false;

    vr_double_lines(w->dbl, m->pixels);
    if (zmbv_enc_frame(w->enc, w->dbl, w->force_key, &out, &out_size, &key) != 0) {
        vr_writer_fail(w, "Video encoder error");
        return;
    }
    en_AVI_WRITER_RESULT r = avi_writer_write_frame(w->avi, out, out_size, key, m->audio, m->audio_frames);

    if (r == AVI_WRITER_ERR_FULL) {
        if (avi_writer_close(w->avi) != AVI_WRITER_OK) {
            fprintf(stderr, "[videorec] ERROR: failed to finalize the full AVI part %u\n", w->part);
        }
        w->avi = NULL;
        vr_writer_add_closed_part(w, w->part);
        w->part++;
        char *path = g_strdup_printf("%s_%03u.avi", w->base, w->part);
        w->avi = avi_writer_open(path, &w->params);
        if (!w->avi) {
            char *e = g_strdup_printf("Cannot create video file part: %s", path);
            vr_writer_fail(w, e);
            g_free(e);
            g_free(path);
            w->part--; /* part nevznikl - počet partů pro videorec_get_status() */
            return;
        }
        st_VR_PART *part = g_new0(st_VR_PART, 1);
        part->filename = g_path_get_basename(path);
        part->first_frame = m->index;
        g_ptr_array_add(w->parts, part);
        w->part_first = m->index;
        fprintf(stderr, "[videorec] Continuing in a new file part: %s\n", path);
        g_free(path);

        if (zmbv_enc_frame(w->enc, w->dbl, true, &out, &out_size, &key) != 0) {
            vr_writer_fail(w, "Video encoder error");
            return;
        }
        r = avi_writer_write_frame(w->avi, out, out_size, key, m->audio, m->audio_frames);
    }

    if (r != AVI_WRITER_OK) {
        vr_writer_fail(w, "Video file write error (disk full?)");
        return;
    }
    w->force_key = false;
    w->frames = m->index + 1;
}

/**
 * @brief Tělo writer vlákna: zpracovává zprávy z fronty až do VR_MSG_QUIT.
 * @param data Fronta zpráv (GAsyncQueue*).
 * @return NULL.
 */
static gpointer vr_writer_thread(gpointer data)
{
    GAsyncQueue *q = data;
    st_VR_WRITER w;
    memset(&w, 0, sizeof(w));
    w.parts = g_ptr_array_new_with_free_func(vr_part_free);
    w.dbl = g_malloc((size_t)VR_WIDTH * VR_AVI_HEIGHT);

    for (;;) {
        st_VR_MSG *m = g_async_queue_pop(q);
        bool quit = false;

        switch (m->type) {
        case VR_MSG_OPEN: {
            vr_writer_close(&w);
            g_ptr_array_set_size(w.parts, 0);
            w.avi = m->avi;
            w.params = m->params;
            w.gen = m->gen;
            w.part = 1;
            w.part_first = 0;
            w.force_key = false;
            w.failed = false;
            w.first_path = g_strdup(m->path);
            w.frames = 0;
            w.base = g_strdup(m->path);
            size_t len = strlen(w.base);
            if (len > 4 && g_ascii_strcasecmp(w.base + len - 4, ".avi") == 0) w.base[len - 4] = '\0';
            st_VR_PART *part = g_new0(st_VR_PART, 1);
            part->filename = g_path_get_basename(m->path);
            part->first_frame = 0;
            g_ptr_array_add(w.parts, part);

            uint32_t pal[DISPLAY_MZCOLORS];
            for (unsigned i = 0; i < DISPLAY_MZCOLORS; i++) pal[i] = g_display_predef_colors[i] & 0x00FFFFFFu;
            w.enc = zmbv_enc_new(m->params.width, m->params.height, pal, DISPLAY_MZCOLORS, m->keyframe_interval);
            if (!w.enc) vr_writer_fail(&w, "Cannot create video encoder");
            g_free(m->path);
            w.closed_bytes = 0;
            vr_writer_publish(&w);
            break;
        }
        case VR_MSG_FRAME:
            if (!w.failed && w.avi && w.enc) {
                vr_writer_frame(&w, m);
                vr_writer_publish(&w);
            }
            g_free(m->pixels);
            g_free(m->audio);
            break;
        case VR_MSG_TRUNCATE:
            if (!w.failed && w.avi) {
                if (m->frame_count < w.part_first) {
                    /* Lepidlo posílá truncate jen v rámci aktuálního partu (rozhoduje po
                     * bariéře vr_writer_sync()). Pojistka: zkrácení přes hranici partu
                     * neumíme - nekrátit naslepo (zahodilo by celý nový part, zatímco
                     * starý by si zahozené snímky ponechal), ale ukončit session;
                     * soubory zůstanou konzistentní. */
                    fprintf(stderr, "[videorec] ERROR: truncate to frame %llu precedes the current part (first frame %llu)\n",
                            (unsigned long long)m->frame_count, (unsigned long long)w.part_first);
                    vr_writer_fail(&w, "Video file truncate error");
                } else if (avi_writer_truncate_to_frame(w.avi, m->frame_count - w.part_first) != AVI_WRITER_OK) {
                    vr_writer_fail(&w, "Video file truncate error");
                }
                w.force_key = true;
            }
            if (!w.failed) w.frames = m->frame_count;
            if (w.first_path) vr_writer_publish(&w);
            break;
        case VR_MSG_SIDECAR: {
            /* Prázdný poslední part (retake na jeho začátek + stop) do sidecaru nepatří. */
            vr_writer_drop_empty_part(&w);
            if (w.first_path) vr_writer_publish(&w);
            for (guint i = 0; i < w.parts->len; i++) {
                const st_VR_PART *part = g_ptr_array_index(w.parts, i);
                videorec_sidecar_add_part(m->sidecar, part->filename, part->first_frame);
            }
            GError *err = NULL;
            if (!videorec_sidecar_save(m->sidecar, m->path, &err)) {
                fprintf(stderr, "[videorec] ERROR: cannot write sidecar %s: %s\n", m->path,
                        err ? err->message : "unknown error");
                g_clear_error(&err);
                vr_writer_fail(&w, "Cannot write the cuts file (.cuts.json)");
            }
            videorec_sidecar_free(m->sidecar);
            g_free(m->path);
            break;
        }
        case VR_MSG_CLOSE:
            vr_writer_close(&w);
            g_ptr_array_set_size(w.parts, 0);
            break;
        case VR_MSG_SYNC:
            g_mutex_lock(&m->sync->mutex);
            m->sync->part_first = w.part_first;
            m->sync->done = true;
            g_cond_signal(&m->sync->cond);
            g_mutex_unlock(&m->sync->mutex);
            /* Od teď bariéra patří zase jen odesílateli - nesahat na ni. */
            break;
        case VR_MSG_QUIT:
            vr_writer_close(&w);
            quit = true;
            break;
        }
        g_free(m);
        if (quit) break;
    }

    g_ptr_array_free(w.parts, TRUE);
    g_free(w.dbl);
    return NULL;
}

/*******************************************************************************
 *
 *                  Pomocné funkce lepidla (emu vlákno, pod zámkem)
 *
 ******************************************************************************/

/** @brief Pošle zprávu writeru. @param m Zpráva (vlastnictví přechází na writer). */
static void vr_push(st_VR_MSG *m)
{
    g_async_queue_push(g_vr.queue, m);
}

/**
 * @brief Bariéra: počká, až writer zpracuje všechny dříve odeslané zprávy.
 *
 * Writer může být za lepidlem pozadu (fronta až ~VR_QUEUE_HIGH snímků);
 * rozhodnutí závislá na stavu writeru (např. zda bod retake leží v aktuálním
 * AVI partu - rollover nastává až při zápisu) se proto smí dělat až po bariéře.
 *
 * @return Globální index prvního snímku aktuálního partu writeru po zpracování
 *         všech dřívějších zpráv.
 * @pre Emu vlákno, pod g_vr.mutex, `initialized`. Čekání pod zámkem nemůže
 *      uváznout: writer g_vr.mutex nikdy nebere a frontu vždy vyprazdňuje.
 * @note Blokuje emu vlákno (a vlákna čekající na g_vr.mutex) na dobu
 *       zpracování fronty writerem; volá se jen při retake.
 */
static uint64_t vr_writer_sync(void)
{
    st_VR_SYNC sync;
    memset(&sync, 0, sizeof(sync));
    g_mutex_init(&sync.mutex);
    g_cond_init(&sync.cond);
    st_VR_MSG *m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_SYNC;
    m->sync = &sync;
    vr_push(m);
    g_mutex_lock(&sync.mutex);
    while (!sync.done) g_cond_wait(&sync.cond, &sync.mutex);
    g_mutex_unlock(&sync.mutex);
    g_cond_clear(&sync.cond);
    g_mutex_clear(&sync.mutex);
    return sync.part_first;
}

/** @brief Náhodné nenulové 64bitové ID (session, větev). @return ID. */
static uint64_t vr_random_id(void)
{
    uint64_t id = ((uint64_t)g_random_int() << 32) | (uint64_t)g_random_int();
    return id ? id : 1;
}

/**
 * @brief Začne novou větev linie na snímku `start` (s novým náhodným ID).
 *
 * Větev se označí jako realtime, je-li efektivní základna realtime (`rt_on`).
 *
 * @param start Globální index prvního snímku větve.
 * @pre Pod zámkem, `takes != NULL`, `start` >= start poslední větve.
 */
static void vr_take_begin(uint64_t start)
{
    st_VR_TAKE t = { vr_random_id(), start, g_vr.rt_on };
    g_array_append_val(g_vr.takes, t);
}

/**
 * @brief Najde větev snapshotu v linii a ověří, že jeho bod leží v jejím zachovaném úseku.
 *
 * @param take_id ID větve ze snapshotu.
 * @param frame   Bod snapshotu (globální index snímku).
 * @param count   Aktuální počet snímků nahrávky (konec poslední větve).
 * @param idx     Výstup: index větve v linii (platný při true).
 * @return true pokud je větev v linii a `start <= frame <= konec úseku`
 *         (konec = start následující větve, u poslední `count`).
 * @pre Pod zámkem, `takes != NULL`.
 */
static bool vr_take_find(uint64_t take_id, uint64_t frame, uint64_t count, guint *idx)
{
    for (guint i = 0; i < g_vr.takes->len; i++) {
        const st_VR_TAKE *t = &g_array_index(g_vr.takes, st_VR_TAKE, i);
        if (t->take_id != take_id) continue;
        if (t->realtime) return false; /* realtime větev: retake je pojem emulačního času */
        uint64_t end = (i + 1 < g_vr.takes->len) ? g_array_index(g_vr.takes, st_VR_TAKE, i + 1).start : count;
        if (t->start <= frame && frame <= end) {
            *idx = i;
            return true;
        }
        return false;
    }
    return false;
}

/**
 * @brief Sink párování: předá hotový snímek writeru (emu vlákno, pod zámkem).
 * @param fr   Snímek; vlastnictví pixelů a zvuku přechází na writer.
 * @param user Nepoužito.
 */
static void vr_sink(st_VIDEOREC_PIPE_FRAME *fr, void *user)
{
    (void)user;
    st_VR_MSG *m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_FRAME;
    m->pixels = fr->pixels;
    m->audio = fr->audio;
    m->audio_frames = fr->audio_frames;
    m->index = fr->index;
    vr_push(m);
}

/**
 * @brief Callback pipe se zachyceným stavem zvuku pro bod snapshotu (emu vlákno nebo ukládající vlákno, pod zámkem).
 * @param tag   Pořadové číslo požadavku (st_VR_AUDIO_SNAP::seq).
 * @param state Stav rendereru na konci snímku před bodem snapshotu.
 * @param user  Nepoužito.
 */
static void vr_audio_capture(uint64_t tag, const st_VIDEOREC_AUDIO_STATE *state, void *user)
{
    (void)user;
    st_VR_AUDIO_SNAP *as = &g_vr.audio_states[tag % VR_AUDIO_STATES];
    if (as->seq != tag) return; /* slot mezitím převzal novější požadavek */
    as->state = *state;
    as->valid = true;
}

/**
 * @brief Najde zachycený stav zvuku pro bod snapshotu.
 * @param take_id Větev snapshotu.
 * @param frame   Snímek snapshotu.
 * @return Nejnovější platný záznam se shodným klíčem, nebo NULL.
 * @pre Pod zámkem.
 */
static const st_VR_AUDIO_SNAP *vr_audio_state_find(uint64_t take_id, uint64_t frame)
{
    const st_VR_AUDIO_SNAP *best = NULL;
    for (unsigned i = 0; i < VR_AUDIO_STATES; i++) {
        const st_VR_AUDIO_SNAP *as = &g_vr.audio_states[i];
        if (as->seq && as->valid && as->take_id == take_id && as->frame == frame && (!best || as->seq > best->seq))
            best = as;
    }
    return best;
}

/**
 * @brief Zahájí ochranu švu: od teď se události zvuku drží v hold_events.
 * @pre Pod zámkem, stav != IDLE. Opakované volání během ochrany buffer nemaže.
 */
static void vr_hold_begin(void)
{
    if (g_vr.audio_hold) return;
    g_vr.audio_hold = true;
    g_array_set_size(g_vr.hold_events, 0);
    g_vr.hold_overflow = false;
}

/**
 * @brief Ukončí ochranu švu a zahodí podržené události.
 * @pre Pod zámkem, stav != IDLE.
 */
static void vr_hold_end(void)
{
    g_vr.audio_hold = false;
    g_array_set_size(g_vr.hold_events, 0);
    g_vr.hold_overflow = false;
    memset(g_vr.last_tap, 0, sizeof(g_vr.last_tap));
}

/**
 * @brief Backpressure: když writer nestíhá, emu vlákno počká (bez držení zámku).
 *
 * Nad VR_QUEUE_HIGH zprávami se čeká po 1 ms, dokud fronta neklesne pod
 * VR_QUEUE_LOW. Zpomalí to MAX SPEED, ale žádný snímek se neztratí. Writer
 * frontu vždy vyprazdňuje (i po chybě zápisu zprávy jen zahazuje), takže
 * čekání skončí.
 *
 * @pre Volá emu vlákno bez držení g_vr.mutex.
 */
static void vr_backpressure(void)
{
    GAsyncQueue *q = g_vr.queue; /* mění se jen v emu vlákně (init/exit) */
    if (!q || g_async_queue_length(q) <= VR_QUEUE_HIGH) return;
    while (g_async_queue_length(q) >= VR_QUEUE_LOW) g_usleep(1000);
}

/**
 * @brief Hraje emulátor právě stereo (druhý PSG aktivní)?
 *
 * MZ-1500 má druhý PSG vždy, MZ-800 jen s `allow_psg1` (lze přepnout za
 * běhu), MZ-700 PSG nemá. Stejný příznak řídí mix SDL cesty
 * (st_AUDIO_LOG::stereo, iface_audio_wait_for_data()).
 *
 * @return true = stereo rozložení L/R (videorec_audio_set_stereo()).
 * @pre Emu vlákno (stav PSG patří emu vláknu).
 */
static bool vr_psg_stereo(void)
{
#if HAVE_PSG == 2
    return g_psg_module.stereo;
#else
    return false;
#endif
}

/**
 * @brief Aktuální hodnoty zvukových kanálů podle živého audio logu.
 * @param values Výstup, AUDIO_SRC_CHANNELS_COUNT prvků.
 * @pre Emu vlákno (g_audio.log patří emu vláknu).
 */
static void vr_current_values(uint8_t values[AUDIO_SRC_CHANNELS_COUNT])
{
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        values[i] = (uint8_t)(g_audio.log ? (g_audio.log->src[i]->last_value & 0x0F) : 0);
    }
}

/**
 * @brief Počet snímků nahrávky: zapsané (předané writeru) + čekající v pipe s pixely.
 * @return Globální index, který dostane další přijatý snímek.
 */
static uint64_t vr_frame_count(void)
{
    uint64_t n = videorec_pipe_next_index(&g_vr.pipe);
    for (unsigned i = 0; i < g_vr.pipe.count; i++) {
        if (g_vr.pipe.pending[(g_vr.pipe.head + i) % VIDEOREC_PIPE_MAX_PENDING].pixels) n++;
    }
    return n;
}

/**
 * @brief Zveřejní stav lepidla pro videorec_get_status().
 *
 * Volá se před uvolněním g_vr.mutex všude, kde se mohl změnit stav, čekající
 * start, počet snímků nebo segment (požadavek na start / stop, konec snímku,
 * stop v paused smyčce, init / exit). Cesta se přepíše jen při nahrávání -
 * bez nahrávání zůstává cesta poslední session (čekající start ji nemění).
 *
 * @pre Pod g_vr.mutex. Bere listový zámek g_vr_status_mutex.
 */
static void vr_publish_status(void)
{
    bool active = (g_vr.state != VIDEOREC_STATE_IDLE);
    g_mutex_lock(&g_vr_status_mutex);
    g_vr_status.state = g_vr.state;
    g_vr_status.start_pending = g_vr.req_start;
    g_vr_status.frames = active ? g_vr.frames : 0;
    g_vr_status.segment = (active && g_vr.sidecar) ? videorec_sidecar_segment_count(g_vr.sidecar) : 0;
    g_vr_status.segment_open = (active && g_vr.sidecar) ? videorec_sidecar_segment_open(g_vr.sidecar) : false;
    g_vr_status.retake_mode = g_vr.retake_mode;
    /* Cesta i u čekajícího startu (vygenerované jméno je známé hned po přijetí). */
    if (active || g_vr.req_start) g_strlcpy(g_vr_status.path, g_vr.start.path, sizeof(g_vr_status.path));
    g_vr_status.timebase = (active || g_vr.req_start) ? g_vr.timebase : g_videorec_settings.timebase;
    g_vr_status.timebase_effective =
        (active && g_vr.rt_on && !g_vr.rt_leaving) ? VIDEOREC_TIMEBASE_REALTIME : VIDEOREC_TIMEBASE_EMULATED;
    g_vr_status.rt_activity = (active && g_vr.rt_on) ? g_vr.rt_activity : VIDEOREC_RT_ACTIVITY_OFF;
    g_vr_status_gen = g_vr.gen;
    g_mutex_unlock(&g_vr_status_mutex);
}

/**
 * @brief Zruší čekající (nezpracovaný) start: zavře a smaže založený soubor.
 * @pre Pod zámkem.
 */
static void vr_cancel_pending_start(void)
{
    if (!g_vr.req_start) return;
    g_vr.req_start = false;
    if (g_vr.start_avi) {
        (void)avi_writer_close(g_vr.start_avi);
        g_vr.start_avi = NULL;
        (void)g_remove(g_vr.start.path);
    }
}

/*******************************************************************************
 *
 *                  Režim podle reality (pod zámkem g_vr.mutex)
 *
 ******************************************************************************/

/**
 * @brief Max. ticků zpracovaných najednou (dohánění pozdního probuzení): ticky za 1 s.
 * @return Snímková frekvence platformy zaokrouhlená dolů, nejméně 1.
 */
static inline unsigned vr_rt_max_catchup(void)
{
    return videorec_get_fps();
}

/** @brief Probudí / uspí vlákno vzorkovače. @param run true = probíhá session. */
static void vr_rtc_set_run(bool run)
{
    g_mutex_lock(&g_vr_rtc.mutex);
    g_vr_rtc.run = run;
    g_vr_rtc.deadline = 0;
    g_cond_signal(&g_vr_rtc.cond);
    g_mutex_unlock(&g_vr_rtc.mutex);
}

/** @brief Zveřejní termín dalšího ticku pro vlákno vzorkovače. @param deadline Termín [us]. */
static void vr_rtc_set_deadline(int64_t deadline)
{
    g_mutex_lock(&g_vr_rtc.mutex);
    g_vr_rtc.deadline = deadline;
    g_mutex_unlock(&g_vr_rtc.mutex);
}

/** @brief Aktuální rychlost emulace. @return Procenta, 0 = MAX SPEED. */
static unsigned vr_cur_speed(void)
{
    /* Čtení z vlákna vzorkovače bez zámku: hodnoty mění emu / UI vlákno po celých
     * slovech, starší hodnota znamená jen zpoždění o jeden tick. */
    return g_emulator.max_speed ? 0u : customspeed_get_current_speed();
}

/**
 * @brief Má být efektivní časová základna realtime?
 * @return true pro požadovanou základnu realtime, pokud ji `emulated_when_fast`
 *         nepotlačí rychlostí != 100 %.
 */
static bool vr_rt_want(void)
{
    if (g_vr.timebase != VIDEOREC_TIMEBASE_REALTIME) return false;
    if (g_vr.rt_speed == VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST && vr_cur_speed() != 100) return false;
    return true;
}

/**
 * @brief Pozice v nahrávce, ke které se vztahují značky (markery, události, hranice).
 *
 * V emulačním čase počet snímků nahrávky. V realtime při nahrávání navíc
 * délka zpožďovací fronty: aktuálně zobrazený obraz se do nahrávky dostane
 * až za vr_rt_delay_ticks() ticků.
 *
 * @return Globální index snímku.
 */
static uint64_t vr_pos(void)
{
    uint64_t n = vr_frame_count();
    if (g_vr.rt_on && g_vr.state == VIDEOREC_STATE_RECORDING && g_vr.rt_fifo) n += g_queue_get_length(g_vr.rt_fifo);
    return n;
}

/**
 * @brief Zapíše událost stavu do sidecaru a případně auto marker.
 * @param at     Pozice (globální index snímku).
 * @param kind   Druh (VIDEOREC_SC_EVENT_*).
 * @param value  Hodnota.
 * @param marker Popisek auto markeru (anglicky), nebo NULL = bez markeru.
 * @pre Pod zámkem, stav != IDLE.
 */
static void vr_state_event(uint64_t at, const char *kind, const char *value, const char *marker)
{
    if (g_vr.state_marks) videorec_sidecar_add_event(g_vr.sidecar, at, kind, value);
    if (marker && g_vr.auto_markers) videorec_sidecar_add_marker(g_vr.sidecar, at, marker);
}

/**
 * @brief Událost rychlosti (hodnota a auto marker podle `speed`).
 * @param at    Pozice.
 * @param speed Rychlost [%], 0 = MAX SPEED.
 * @param mark  true = vložit i auto marker (ne na začátku nahrávky).
 * @pre Pod zámkem, stav != IDLE.
 */
static void vr_speed_event(uint64_t at, unsigned speed, bool mark)
{
    char value[16], label[32];
    if (speed == 0) {
        g_strlcpy(value, "max", sizeof(value));
        g_strlcpy(label, "Speed MAX", sizeof(label));
    } else {
        g_snprintf(value, sizeof(value), "%u", speed);
        g_snprintf(label, sizeof(label), "Speed %u%%", speed);
    }
    vr_state_event(at, VIDEOREC_SC_EVENT_SPEED, value, mark ? label : NULL);
}

/**
 * @brief Je efektivní časová základna právě realtime (ne emulated ani návrat z ní)?
 * @return true = `rt_on && !rt_leaving`.
 * @pre Pod zámkem.
 */
static bool vr_rt_effective(void)
{
    return g_vr.rt_on && !g_vr.rt_leaving;
}

/**
 * @brief Sleduje rychlost emulace a čekající reset (události + auto markery).
 *
 * Událost rychlosti se zapisuje vždy, auto marker jen v efektivní realtime:
 * v emulačním čase video hraje normální rychlostí a změna rychlosti v něm
 * není vidět, marker by byl šum (MAX SPEED přes nudnou pasáž, MCP agent;
 * final review I2). Marker "Reset" se vkládá v obou základnách.
 *
 * @pre Pod zámkem, stav != IDLE. Volá konec emulovaného snímku (emulační čas)
 *      nebo tick vzorkovače (realtime).
 */
static void vr_observe_speed_reset(void)
{
    unsigned sp = vr_cur_speed();
    if (sp != g_vr.obs_speed) {
        g_vr.obs_speed = sp;
        vr_speed_event(vr_pos(), sp, vr_rt_effective());
    }
    if (g_vr.req_reset) {
        g_vr.req_reset = false;
        vr_state_event(vr_pos(), VIDEOREC_SC_EVENT_RESET, "", "Reset");
    }
}

/**
 * @brief Patří aktuální pauza (nebo běh) krokování v debuggeru?
 *
 * Ano, když během pauzy proběhl krok debuggeru (čítač g_vr_dbg_steps), pauza
 * je od breakpointu, právě probíhá běh k dočasnému breakpointu
 * (g_vr_dbg_run_inflight: step over a run to cursor krátce zruší
 * g_emulator.paused), nebo pauza je zastavení na dočasném breakpointu
 * (`dbg_pause`, jeho `pause_reason` se nenastavuje). Ruční pokračování po
 * dokončeném kroku je normální běh uživatele.
 *
 * @return true = krokování (řídí se `record_debugger_steps`).
 * @pre Pod zámkem, stav != IDLE.
 */
static bool vr_pause_is_debugger(void)
{
    return g_vr.dbg_pause || g_atomic_int_get(&g_vr_dbg_run_inflight) ||
           (g_atomic_int_get(&g_vr_dbg_steps) != g_vr.obs_dbg_steps) ||
           (g_emulator.pause_reason == EMU_PAUSE_REASON_BREAKPOINT);
}

/**
 * @brief Sleduje pauzu emulace (události + auto marker).
 *
 * Volá se tam, kde je pozice přesná: v emulačním čase z emu vlákna (paused
 * smyčka videorec_on_emulation_paused() = začátek pauzy, konec emulovaného
 * snímku = konec pauzy; v pauze nevznikají snímky, takže pozice je počet
 * snímků v okamžiku pauzy a zachytí se i pauza kratší než jeden snímek), v realtime
 * z ticku vzorkovače (pozice je čas obrazu; pauza kratší než tick se ve videu
 * neprojeví a nezaznamená se).
 *
 * Hodnota pause_start: "breakpoint" (pauza od breakpointu), "debugger"
 * (pauza po step over / run to cursor), "frames" (doběhnutí N snímků, MCP
 * emu_run), jinak "user". Auto marker "Pause" jen pro "user" v efektivní
 * realtime (v emulačním čase pauza ve videu není; krokování a řízení
 * automatem marker nedostávají).
 *
 * Pauza na dočasném breakpointu je "debugger" i tehdy, když ji vzorkovač
 * pozoruje dřív než paused smyčka (g_vr_dbg_run_inflight ještě platí).
 * `dbg_pause` ruší pozorovaný běh emulace: buď pozorovaný konec pauzy, nebo
 * běh, před kterým pauzu na dočasném breakpointu nikdo nepozoroval
 * (`!g_emulator.paused && !obs_paused` - např. MCP `step_over` a hned `run`
 * zpracované v první iteraci paused smyčky, kdy vzorkovač ani hook
 * videorec_on_emulation_paused() pauzu neviděly). Jinak by `dbg_pause`
 * přežilo volný běh a další pauza uživatele by se označila "debugger"
 * (bez markeru "Pause"). Běh k dočasnému breakpointu poznává
 * g_vr_dbg_run_inflight, takže ruční pokračování (F5, MCP run) po
 * dokončeném kroku je normální běh.
 *
 * @pre Pod zámkem, stav != IDLE.
 * @post Běží-li emulace, `dbg_pause == false`.
 */
static void vr_observe_pause(void)
{
    bool paused = g_emulator.paused;
    /* Nepozorovaná pauza na dočasném BP skončila dřív, než ji kdo viděl. */
    if (!paused && !g_vr.obs_paused) g_vr.dbg_pause = false;
    if (paused == g_vr.obs_paused) return;
    g_vr.obs_paused = paused;
    if (paused) {
        en_EMU_PAUSE_REASON why = g_emulator.pause_reason;
        const char *value = (why == EMU_PAUSE_REASON_BREAKPOINT) ? "breakpoint"
                            : (g_vr.dbg_pause || g_atomic_int_get(&g_vr_dbg_run_inflight)) ? "debugger"
                            : (why == EMU_PAUSE_REASON_FRAMES)   ? "frames"
                                                                 : "user";
        g_vr.obs_dbg_steps = g_atomic_int_get(&g_vr_dbg_steps);
        g_vr.rt_pause_ticks = 0;
        g_strlcpy(g_vr.obs_pause_value, value, sizeof(g_vr.obs_pause_value));
        bool mark = g_vr.rt_on && !g_vr.rt_leaving && (strcmp(value, "user") == 0);
        vr_state_event(vr_pos(), VIDEOREC_SC_EVENT_PAUSE_START, value, mark ? "Pause" : NULL);
    } else {
        g_vr.dbg_pause = false;
        vr_state_event(vr_pos(), VIDEOREC_SC_EVENT_PAUSE_END, "", NULL);
    }
}

/**
 * @brief Po retake znovu zapíše stav platný od bodu snapshotu (bez auto markerů).
 *
 * videorec_sidecar_truncate() zahodí události od bodu snapshotu dál, ale
 * pozorovaný stav (`obs_speed`, `obs_paused`, časová základna) zůstává
 * ze zahozené budoucnosti - další událost by vznikla až při jeho změně. Bez
 * opětovného zápisu by např. rychlost změněná v zahozeném úseku v sidecaru
 * chyběla a export by po pozdějším přepnutí do realtime ukázal špatný stav.
 * Zapíše se časová základna (efektivní), rychlost a probíhající pauza.
 *
 * @param at Bod snapshotu (globální index snímku).
 * @pre Pod zámkem, stav != IDLE, po videorec_sidecar_truncate().
 */
static void vr_reemit_state(uint64_t at)
{
    vr_state_event(at, VIDEOREC_SC_EVENT_TIMEBASE, (g_vr.rt_on && !g_vr.rt_leaving) ? "realtime" : "emulated", NULL);
    vr_speed_event(at, g_vr.obs_speed, false);
    if (g_vr.obs_paused) vr_state_event(at, VIDEOREC_SC_EVENT_PAUSE_START, g_vr.obs_pause_value, NULL);
}

/**
 * @brief Zkopíruje poslední zobrazený snímek (s maskou `& 0x0F`) do nového bufferu.
 * @param seq_out Výstup: pořadí snímku (viz g_vr_rtv_seq), nebo NULL.
 * @return Nový buffer VR_WIDTH*VR_HEIGHT (g_free).
 */
static uint8_t *vr_rt_grab(uint32_t *seq_out)
{
    uint8_t *px = g_malloc((size_t)VR_WIDTH * VR_HEIGHT);
    g_mutex_lock(&g_vr_rtv_mutex);
    memcpy(px, g_vr_rtv_pixels, (size_t)VR_WIDTH * VR_HEIGHT);
    if (seq_out) *seq_out = g_vr_rtv_seq;
    g_mutex_unlock(&g_vr_rtv_mutex);
    for (size_t i = 0; i < (size_t)VR_WIDTH * VR_HEIGHT; i++) px[i] &= 0x0F;
    return px;
}

/**
 * @brief Zesílení zvuku v realtime podle rychlosti a `realtime_turbo_audio`.
 * @return 1 při rychlosti <= 100 %, jinak podle nastavení (1, 0, VIDEOREC_TURBO_ATTENUATE_GAIN).
 */
static float vr_rt_gain(void)
{
    unsigned sp = vr_cur_speed();
    if (sp != 0 && sp <= 100) return 1.0f;
    switch (g_vr.turbo_audio) {
    case VIDEOREC_TURBO_AUDIO_SILENCE: return 0.0f;
    case VIDEOREC_TURBO_AUDIO_ATTENUATE: return VIDEOREC_TURBO_ATTENUATE_GAIN;
    default: return 1.0f;
    }
}

/**
 * @brief Zapíše jeden realtime snímek: obraz + zvuk jednoho snímku (20 ms / 16,7 ms) z jitter bufferu.
 * @param px Pixely (vlastnictví přechází na writer).
 * @pre Pod zámkem, `rt_on`, stav RECORDING.
 * @post Index dalšího snímku (pipe next_index) o 1 vyšší.
 */
static void vr_rt_write_frame(uint8_t *px)
{
    size_t spf = videorec_platform_samples_per_frame(&g_vr_platform, g_vr.audio_rate);
    int16_t *audio = g_malloc(spf * 2u * sizeof(int16_t));
    videorec_rt_audio_pull(&g_vr_rta, audio, spf, vr_rt_gain());
    st_VR_MSG *m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_FRAME;
    m->pixels = px;
    m->audio = audio;
    m->audio_frames = spf;
    m->index = videorec_pipe_next_index(&g_vr.pipe);
    videorec_pipe_set_next_index(&g_vr.pipe, m->index + 1);
    vr_push(m);
    g_vr.rt_stats.frames_written++;
}

/**
 * @brief Vyprázdní zpožďovací frontu obrazu.
 * @param write      true = snímky zapsat (se zvukem z jitter bufferu), false = zahodit.
 * @param max_write  Nejvýš tolik snímků zapsat, zbytek zahodit (UINT64_MAX = vše).
 * @pre Pod zámkem, `rt_on`; při `write` stav RECORDING.
 * @post Fronta prázdná.
 */
static void vr_rt_drain_fifo(bool write, uint64_t max_write)
{
    uint8_t *px;
    while ((px = g_queue_pop_head(g_vr.rt_fifo)) != NULL) {
        if (write && max_write > 0) {
            vr_rt_write_frame(px);
            max_write--;
        } else {
            g_free(px);
        }
    }
}

/**
 * @brief Doplní zpožďovací frontu obrazu aktuálním zobrazeným snímkem na vr_rt_delay_ticks().
 *
 * Zpoždění obrazu proti zvuku dává délka fronty (každý zapisující tick
 * vloží 1 snímek a 1 vyjme) - fronta kratší než vr_rt_delay_ticks()
 * by zvuk trvale posunula za obraz. Chybějící snímky se doplní na konec
 * fronty (nejstarší obsah zůstává na začátku).
 *
 * @pre Pod zámkem, `rt_on`.
 * @post Fronta obsahuje alespoň vr_rt_delay_ticks() snímků.
 */
static void vr_rt_fifo_prime(void)
{
    while (g_queue_get_length(g_vr.rt_fifo) < vr_rt_delay_ticks())
        g_queue_push_tail(g_vr.rt_fifo, vr_rt_grab(NULL));
}

/**
 * @brief Uloží statistiky jitter bufferu do rt_stats (před jeho resetem).
 * @pre Pod zámkem.
 */
static void vr_rt_keep_audio_stats(void)
{
    videorec_rt_audio_get_stats(&g_vr_rta, &g_vr.rt_stats.audio);
    g_vr.rt_stats.clock_rebased = g_vr.rt_clock.rebased;
    g_vr.rt_stats.lost_ticks = g_vr.rt_clock.lost_ticks;
}

/**
 * @brief Přepne efektivní časovou základnu na realtime (emulated -> realtime).
 *
 * Dopíše čekající emulační snímky, nastaví bit VIDEOREC_FLAG_RT, vynuluje
 * jitter buffer a povolí do něj zápis, naplní zpožďovací frontu aktuálním
 * zobrazeným snímkem, začne realtime větev a (při nahrávání, mimo snímek 0)
 * nový segment s výchozím přechodem. Událost `timebase = realtime`.
 *
 * @pre Pod zámkem, stav != IDLE, `!rt_on`. Libovolné vlákno (tick vzorkovače,
 *      start session v emu vlákně).
 * @post `rt_on`, fronta obsahuje vr_rt_delay_ticks() snímků.
 */
static void vr_rt_enter(void)
{
    videorec_pipe_flush(&g_vr.pipe);
    vr_hold_end();
    uint64_t at = vr_frame_count();
    g_vr.rt_on = true;
    g_vr.rt_leaving = false;
    vr_flag_set(VIDEOREC_FLAG_RT);
    videorec_rt_audio_reset(&g_vr_rta, true, g_vr.audio_rate);
    vr_rt_fifo_prime(); /* fronta je po předchozím realtime úseku prázdná (vr_rt_leave) */
    g_vr.rt_have_seq = false;
    g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_LIVE;
    vr_take_begin(at);
    if (g_vr.state == VIDEOREC_STATE_RECORDING && at > 0) {
        videorec_sidecar_segment_begin(g_vr.sidecar, at, g_vr.transition);
    }
    vr_state_event(at, VIDEOREC_SC_EVENT_TIMEBASE, "realtime", NULL);
    fprintf(stderr, "[videorec] Timebase: real time (frame %llu)\n", (unsigned long long)at);
}

/**
 * @brief Vrátí se do emulačního času na konci emulovaného snímku (realtime -> emulated).
 *
 * Dopíše zpožďovací frontu (při nahrávání), zakáže zápis do jitter bufferu,
 * založí emulační osu (rebase pipe na začátek aktuálního snímku s aktuálními
 * hodnotami kanálů - aktuální snímek na ni přesně padne), začne emulační
 * větev a (při nahrávání) nový segment. Událost `timebase = emulated`.
 *
 * @param frame_end Takt konce aktuálního snímku.
 * @pre Pod zámkem, emu vlákno (konec snímku), `rt_on`.
 * @post `!rt_on`, bit VIDEOREC_FLAG_RT smazán.
 */
static void vr_rt_leave(uint64_t frame_end)
{
    vr_rt_drain_fifo(g_vr.state == VIDEOREC_STATE_RECORDING, UINT64_MAX);
    vr_rt_keep_audio_stats();
    g_vr.rt_on = false;
    g_vr.rt_leaving = false;
    vr_flag_clear(VIDEOREC_FLAG_RT);
    videorec_rt_audio_reset(&g_vr_rta, false, 0);
    uint8_t values[AUDIO_SRC_CHANNELS_COUNT];
    vr_current_values(values);
    uint64_t origin = (frame_end >= VIDEO_SCREEN_TICKS) ? frame_end - VIDEO_SCREEN_TICKS : 0;
    videorec_pipe_rebase(&g_vr.pipe, origin, values);
    vr_hold_end();
    g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_OFF;
    uint64_t at = vr_frame_count();
    vr_take_begin(at);
    if (g_vr.state == VIDEOREC_STATE_RECORDING) videorec_sidecar_segment_begin(g_vr.sidecar, at, g_vr.transition);
    vr_state_event(at, VIDEOREC_SC_EVENT_TIMEBASE, "emulated", NULL);
    fprintf(stderr, "[videorec] Timebase: emulated time (frame %llu)\n", (unsigned long long)at);
}

/**
 * @brief Nahraný snapshot v realtime: vždy šev (retake je pojem emulačního času).
 * @pre Pod zámkem, `rt_on`, `req_snapshot`.
 */
static void vr_rt_snapshot(void)
{
    g_vr.req_snapshot = false;
    vr_hold_end(); /* v realtime se události kanálů do pipe nepouštějí - nic k přehrání */
    uint64_t at = vr_pos();
    vr_take_begin(vr_frame_count());
    if (g_vr.state == VIDEOREC_STATE_RECORDING) videorec_sidecar_segment_begin(g_vr.sidecar, at, g_vr.transition);
    vr_state_event(at, VIDEOREC_SC_EVENT_SNAPSHOT, "seam", "Snapshot loaded");
    vr_emit_event(VIDEOREC_EVENT_SEAM, at, NULL, "Recording seam (snapshot loaded)");
    fprintf(stderr, "[videorec] Seam: snapshot loaded at frame %llu (real time)\n", (unsigned long long)at);
}

static void vr_finalize(void);

/**
 * @brief Zpracuje čekající požadavky v realtime (markery, snapshot, record-pause, stop).
 *
 * V realtime nevznikají požadavky vázané na konec emulovaného snímku -
 * zpracovává je tick vzorkovače (i v pauze emulace). Record-pause dopíše
 * zpožďovací frontu obrazu; obnovení ji doplní aktuálním obrazem na
 * vr_rt_delay_ticks() (vr_rt_fifo_prime()), aby zůstal zachovaný
 * posun obrazu proti zvuku.
 *
 * @pre Pod zámkem, `rt_on && !rt_leaving`, stav != IDLE.
 * @post Při stopu stav IDLE.
 */
static void vr_rt_requests(void)
{
    for (guint i = 0; i < g_vr.req_markers->len; i++) {
        videorec_sidecar_add_marker(g_vr.sidecar, vr_pos(), g_ptr_array_index(g_vr.req_markers, i));
    }
    g_ptr_array_set_size(g_vr.req_markers, 0);

    if (g_vr.req_snapshot) vr_rt_snapshot();

    if (g_vr.req_pause_toggle) {
        g_vr.req_pause_toggle = false;
        if (g_vr.state == VIDEOREC_STATE_RECORDING) {
            /* Obraz ve frontě je minulost před pauzou - dopsat, pak uzavřít segment. */
            vr_rt_drain_fifo(true, UINT64_MAX);
            uint64_t at = vr_frame_count();
            videorec_sidecar_segment_end(g_vr.sidecar, at);
            g_array_append_val(g_vr.pause_frames, at);
            g_vr.state = VIDEOREC_STATE_PAUSED;
            fprintf(stderr, "[videorec] Recording paused\n");
        } else {
            /* Během pauzy nahrávání se fronta doplňuje jen při běžící emulaci - při
             * obnovení v pauze emulace nebo do vr_rt_delay_ticks() ticků by
             * byla kratší a zvuk by trvale zaostával za obrazem (final review M1). */
            vr_rt_fifo_prime();
            videorec_sidecar_segment_begin(g_vr.sidecar, vr_frame_count(), g_vr.transition);
            g_vr.state = VIDEOREC_STATE_RECORDING;
            fprintf(stderr, "[videorec] Recording resumed\n");
        }
    }

    if (g_vr.req_stop) vr_finalize();
}

/**
 * @brief Jeden tick vzorkovače v realtime: rozhodne o zápisu a zapíše snímek.
 *
 * - record-pause: fronta obrazu se posouvá (nejstarší snímek se zahodí)
 *   a zvuk se spotřebuje naprázdno - po obnovení obraz i zvuk navazují;
 * - pauza emulace: krokování debuggeru (vr_pause_is_debugger()) podle
 *   `record_debugger_steps`, jinak podle `realtime_pause`; běh k dočasnému
 *   breakpointu (step over, run to cursor) bez `record_debugger_steps` se
 *   také nezapisuje;
 *   když se nezapisuje, nic se nespotřebuje (hodiny nahrávky stojí);
 * - jinak se navzorkuje poslední zobrazený snímek do fronty a zapíše se
 *   nejstarší snímek fronty se zvukem jednoho snímku.
 *
 * @pre Pod zámkem, `rt_on && !rt_leaving`, stav != IDLE.
 */
static void vr_rt_emit_tick(void)
{
    size_t spf = videorec_platform_samples_per_frame(&g_vr_platform, g_vr.audio_rate);
    bool paused = g_emulator.paused;
    g_vr.rt_stats.ticks++;

    if (paused) g_vr.rt_pause_ticks++;

    if (g_vr.state == VIDEOREC_STATE_PAUSED) {
        g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_SKIPPING;
        if (paused) return;
        g_queue_push_tail(g_vr.rt_fifo, vr_rt_grab(NULL));
        while (g_queue_get_length(g_vr.rt_fifo) > vr_rt_delay_ticks()) g_free(g_queue_pop_head(g_vr.rt_fifo));
        videorec_rt_audio_pull(&g_vr_rta, NULL, spf, 1.0f);
        return;
    }

    if (!paused && g_atomic_int_get(&g_vr_dbg_run_inflight) && !g_vr.record_debugger_steps) {
        /* běh k dočasnému breakpointu (step over, run to cursor) je součást krokování */
        g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_SKIPPING;
        return;
    }

    if (paused) {
        bool dbg = vr_pause_is_debugger();
        bool write;
        if (dbg) {
            write = g_vr.record_debugger_steps;
        } else if (g_vr.rt_pause == VIDEOREC_RT_PAUSE_FREEZE) {
            write = true;
        } else if (g_vr.rt_pause == VIDEOREC_RT_PAUSE_FREEZE_CAPPED) {
            write = (g_vr.rt_pause_ticks <= g_vr.rt_pause_cap_ticks);
        } else {
            write = false;
        }
        if (!write) {
            g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_SKIPPING;
            return;
        }
        g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_FROZEN;
    } else {
        g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_LIVE;
    }

    uint32_t seq = 0;
    uint8_t *px = vr_rt_grab(&seq);
    if (g_vr.rt_have_seq && seq == g_vr.rt_last_seq) g_vr.rt_stats.frames_repeated++;
    g_vr.rt_last_seq = seq;
    g_vr.rt_have_seq = true;
    g_queue_push_tail(g_vr.rt_fifo, px);
    vr_rt_write_frame(g_queue_pop_head(g_vr.rt_fifo));
}

/**
 * @brief Tick vzorkovače (vlákno "videorec-rt", v testech videorec_test_rt_tick()).
 *
 * Bez nahrávání nic. Jinak: rozvrh ticků (videorec_rt_clock_due()),
 * chyba writeru, sledování pauzy (jen realtime), rozhodnutí
 * o efektivní časové základně (vstup do realtime hned, návrat do emulačního
 * času na nejbližším konci snímku), v realtime požadavky, rychlost/reset,
 * zápis snímků za všechny splatné ticky a automatický stop.
 *
 * @param now Čas monotónních hodin [us].
 * @pre Volající nedrží g_vr.mutex ani g_vr_rtc.mutex.
 */
static void vr_rt_tick(int64_t now)
{
    bool quit = false;
    g_mutex_lock(&g_vr.mutex);
    if (g_vr.state == VIDEOREC_STATE_IDLE) {
        vr_rtc_set_run(false); /* pod g_vr.mutex - nemůže přepsat run nové session */
        g_mutex_unlock(&g_vr.mutex);
        return;
    }
    unsigned due = videorec_rt_clock_due(&g_vr.rt_clock, now);
    vr_rtc_set_deadline(videorec_rt_clock_next_deadline(&g_vr.rt_clock));
    if (due == 0) {
        g_mutex_unlock(&g_vr.mutex);
        return;
    }
    if (due > 1) g_vr.rt_stats.multi_ticks++;

    if (g_atomic_int_get(&g_vr_failed_gen) == (gint)g_vr.gen) {
        /* chyba writeru (stejné chování jako na konci emulovaného snímku) */
        g_mutex_lock(&g_vr_err_mutex);
        g_strlcpy(g_videorec_last_error, g_vr_writer_error[0] ? g_vr_writer_error : "Video file write error",
                  sizeof(g_videorec_last_error));
        g_mutex_unlock(&g_vr_err_mutex);
        vr_finalize();
        vr_publish_status();
        g_mutex_unlock(&g_vr.mutex);
        return;
    }

    /* Pauzu v emulačním čase sleduje emu vlákno (přesná pozice), tady jen realtime. */
    if (g_vr.rt_on && !g_vr.rt_leaving) vr_observe_pause();

    bool want = vr_rt_want();
    if (want && !g_vr.rt_on) {
        vr_rt_enter();
    } else if (!want && g_vr.rt_on && !g_vr.rt_leaving) {
        g_vr.rt_leaving = true; /* dokončí konec emulovaného snímku (vr_rt_leave()) */
        vr_flag_set(VIDEOREC_FLAG_REQ);
    } else if (want && g_vr.rt_leaving) {
        g_vr.rt_leaving = false; /* rozmyšleno dřív, než emulace dosáhla konce snímku */
    }

    if (g_vr.rt_on && !g_vr.rt_leaving) {
        vr_observe_speed_reset();
        vr_rt_requests();
        for (unsigned i = 0; i < due && g_vr.state != VIDEOREC_STATE_IDLE; i++) {
            if (g_vr.stop_after_frames && vr_pos() >= g_vr.stop_after_frames) {
                /* Fronta obsahuje snímky do limitu - dopsat jen je, pak ukončit. */
                uint64_t n = vr_frame_count();
                vr_rt_drain_fifo(g_vr.state == VIDEOREC_STATE_RECORDING,
                                 g_vr.stop_after_frames > n ? g_vr.stop_after_frames - n : 0);
                quit = g_vr.quit_after_stop;
                vr_finalize();
                break;
            }
            vr_rt_emit_tick();
        }
        if (g_vr.state != VIDEOREC_STATE_IDLE) g_vr.frames = vr_frame_count();
    }
    if (quit) {
        /* emulator_quit() smí volat jen emu vlákno - předat konci emulovaného snímku */
        g_vr.quit_pending = true;
        vr_flag_set(VIDEOREC_FLAG_REQ);
    }
    vr_publish_status();
    g_mutex_unlock(&g_vr.mutex);
}

/**
 * @brief Tělo vlákna vzorkovače "videorec-rt": tick v termínech monotónních hodin.
 *
 * Bez session (nebo v ručním testovacím režimu) spí na podmínce. Při session
 * čeká do termínu dalšího ticku (g_cond_wait_until s monotónním časem) a pak
 * volá vr_rt_tick() bez držení svého zámku.
 *
 * @param data Nepoužito.
 * @return NULL.
 */
static gpointer vr_rt_thread(gpointer data)
{
    (void)data;
    g_mutex_lock(&g_vr_rtc.mutex);
    for (;;) {
        if (g_vr_rtc.quit) break;
        if (!g_vr_rtc.run || g_vr_rtc.manual) {
            g_cond_wait(&g_vr_rtc.cond, &g_vr_rtc.mutex);
            continue;
        }
        int64_t now = g_get_monotonic_time();
        if (g_vr_rtc.deadline > now) {
            (void)g_cond_wait_until(&g_vr_rtc.cond, &g_vr_rtc.mutex, g_vr_rtc.deadline);
            continue;
        }
        g_mutex_unlock(&g_vr_rtc.mutex);
        vr_rt_tick(now);
        g_mutex_lock(&g_vr_rtc.mutex);
    }
    g_mutex_unlock(&g_vr_rtc.mutex);
    return NULL;
}

/**
 * @brief Zahájí session na konci snímku `frame_end` (ten se už nezapisuje).
 * @param frame_end Takt konce aktuálního snímku = origin nahrávky.
 * @pre Pod zámkem, emu vlákno, `req_start`, stav IDLE.
 * @post Stav RECORDING, bit VIDEOREC_FLAG_ACTIVE nastaven, writer dostal VR_MSG_OPEN.
 */
static void vr_do_start(uint64_t frame_end)
{
    uint8_t values[AUDIO_SRC_CHANNELS_COUNT];
    vr_current_values(values);

    const st_VIDEOREC_PLATFORM *pf = &g_vr_platform;
    videorec_pipe_init(&g_vr.pipe, pf->fb_width, pf->fb_height, pf->ticks_per_frame, pf->clk_hz, g_vr.audio_rate,
                       pf->audio_channels, (const float(*)[VIDEOREC_AUDIO_LEVELS])g_vr.start.level, values,
                       frame_end, vr_sink, NULL);
    videorec_pipe_set_capture_cb(&g_vr.pipe, vr_audio_capture, NULL);
    videorec_pipe_set_stereo(&g_vr.pipe, vr_psg_stereo());
    memset(g_vr.audio_states, 0, sizeof(g_vr.audio_states));
    g_vr.audio_state_seq = 0;
    g_vr.sidecar = videorec_sidecar_new(VR_WIDTH, VR_AVI_HEIGHT, pf->fps_num, pf->fps_den, g_vr.audio_rate,
                                        g_vr.transition, g_vr.transition_ms);
    videorec_sidecar_set_line_doubled(g_vr.sidecar, true);
    videorec_sidecar_set_platform(g_vr.sidecar, pf->name, pf->tv_system, pf->fb_width, pf->fb_height, pf->canvas_x,
                                  pf->canvas_y, pf->canvas_w, pf->canvas_h);
    videorec_sidecar_segment_begin(g_vr.sidecar, 0, VIDEOREC_TRANS_NONE);

    const char *path = g_vr.start.path;
    size_t len = strlen(path);
    if (len > 4 && g_ascii_strcasecmp(path + len - 4, ".avi") == 0) {
        g_snprintf(g_vr.sidecar_path, sizeof(g_vr.sidecar_path), "%.*s.cuts.json", (int)(len - 4), path);
    } else {
        g_snprintf(g_vr.sidecar_path, sizeof(g_vr.sidecar_path), "%s.cuts.json", path);
    }

    g_vr.gen++;
    if (g_vr.gen == 0) g_vr.gen = 1;
    st_VR_MSG *m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_OPEN;
    m->gen = g_vr.gen;
    m->avi = g_vr.start_avi;
    m->path = g_strdup(path);
    m->params.width = VR_WIDTH;
    m->params.height = VR_AVI_HEIGHT;
    m->params.fps_num = pf->fps_num;
    m->params.fps_den = pf->fps_den;
    m->params.audio_rate = g_vr.audio_rate;
    m->params.audio_channels = 2;
    m->params.max_bytes = g_vr.start_part_limit;
    m->keyframe_interval = g_vr.keyframe_interval;
    vr_push(m);

    g_vr.start_avi = NULL;
    g_vr.req_start = false;
    g_vr.stop_after_frames = g_vr.start.stop_after_frames;
    g_vr.quit_after_stop = g_vr.start.quit_after_stop;
    g_vr.session_id = vr_random_id();
    g_vr.frames = 0;
    g_vr.state = VIDEOREC_STATE_RECORDING;
    g_vr.audio_hold = false;
    memset(g_vr.last_tap, 0, sizeof(g_vr.last_tap));
    g_vr.pause_frames = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    g_vr.takes = g_array_new(FALSE, FALSE, sizeof(st_VR_TAKE));
    g_vr.hold_events = g_array_new(FALSE, FALSE, sizeof(st_VR_HOLD_EVENT));
    g_vr.hold_overflow = false;
    /* režim podle reality: stav session */
    g_vr.rt_on = false;
    g_vr.rt_leaving = false;
    g_vr.rt_fifo = g_queue_new();
    videorec_rt_clock_init_fps(&g_vr.rt_clock, g_vr_platform.fps_num, g_vr_platform.fps_den, vr_rt_max_catchup());
    g_vr.obs_paused = g_emulator.paused;
    g_strlcpy(g_vr.obs_pause_value, "user", sizeof(g_vr.obs_pause_value));
    g_vr.obs_speed = vr_cur_speed();
    g_vr.obs_dbg_steps = g_atomic_int_get(&g_vr_dbg_steps);
    g_vr.dbg_pause = false;
    g_vr.rt_pause_ticks = 0;
    g_vr.req_reset = false;
    g_vr.quit_pending = false;
    g_vr.rt_have_seq = false;
    g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_OFF;
    memset(&g_vr.rt_stats, 0, sizeof(g_vr.rt_stats));
    /* Slot zobrazeného snímku: aktuální obraz (emu vlákno, konec snímku - framebuffer je hotový). */
    g_mutex_lock(&g_vr_rtv_mutex);
    memcpy(g_vr_rtv_pixels, g_framebuffer.pixels, sizeof(g_vr_rtv_pixels));
    g_vr_rtv_seq++;
    g_mutex_unlock(&g_vr_rtv_mutex);

    vr_take_begin(0);
    vr_flag_set(VIDEOREC_FLAG_ACTIVE);
    if (vr_rt_want()) {
        vr_rt_enter(); /* událost timebase = realtime na snímku 0 */
    } else {
        vr_state_event(0, VIDEOREC_SC_EVENT_TIMEBASE, "emulated", NULL);
    }
    vr_speed_event(0, g_vr.obs_speed, false);
    vr_rtc_set_run(true);
    char *ev = g_strdup_printf("Recording started: %s", path);
    vr_emit_event(VIDEOREC_EVENT_STARTED, 0, path, ev);
    g_free(ev);
    fprintf(stderr, "[videorec] Recording started: %s\n", path);
}

/**
 * @brief Ukončí session: dopíše čekající snímky, předá sidecar writeru a zavře AVI.
 *
 * V realtime nejdřív dopíše zpožďovací frontu obrazu (při nahrávání) a zakáže
 * zápis do jitter bufferu; vlákno vzorkovače uspí.
 *
 * @pre Pod zámkem, emu vlákno nebo vlákno vzorkovače, stav != IDLE.
 * @post Stav IDLE, bity VIDEOREC_FLAG_ACTIVE a VIDEOREC_FLAG_RT smazány; pipe
 *       uvolněna, sidecar předán writeru.
 */
static void vr_finalize(void)
{
    if (g_vr.rt_on) {
        /* realtime: obraz ve zpožďovací frontě je minulost - dopsat (při nahrávání) */
        vr_rt_drain_fifo(g_vr.state == VIDEOREC_STATE_RECORDING, UINT64_MAX);
        vr_rt_keep_audio_stats();
        g_vr.rt_on = false;
        g_vr.rt_leaving = false;
        vr_flag_clear(VIDEOREC_FLAG_RT);
        videorec_rt_audio_reset(&g_vr_rta, false, 0);
    }
    if (g_vr.rt_fifo) {
        g_queue_free_full(g_vr.rt_fifo, g_free);
        g_vr.rt_fifo = NULL;
    }
    g_vr.rt_activity = VIDEOREC_RT_ACTIVITY_OFF;
    vr_rtc_set_run(false);
    if (g_vr.rt_stats.ticks) {
        fprintf(stderr,
                "[videorec] Real time: %llu ticks, %llu frames written (%llu repeated), %llu late wakeups, "
                "%llu clock rebases; audio %llu underruns, %llu overruns, mean buffer fill %.0f samples, "
                "max drift correction %.4f %%\n",
                (unsigned long long)g_vr.rt_stats.ticks, (unsigned long long)g_vr.rt_stats.frames_written,
                (unsigned long long)g_vr.rt_stats.frames_repeated, (unsigned long long)g_vr.rt_stats.multi_ticks,
                (unsigned long long)g_vr.rt_stats.clock_rebased, (unsigned long long)g_vr.rt_stats.audio.underruns,
                (unsigned long long)g_vr.rt_stats.audio.overruns, g_vr.rt_stats.audio.fill_ema,
                g_vr.rt_stats.audio.max_abs_corr * 100.0);
    }
    videorec_pipe_flush(&g_vr.pipe);
    uint64_t n = videorec_pipe_next_index(&g_vr.pipe);
    videorec_sidecar_segment_end(g_vr.sidecar, n);

    st_VR_MSG *m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_SIDECAR;
    m->sidecar = g_vr.sidecar;
    m->path = g_strdup(g_vr.sidecar_path);
    vr_push(m);
    g_vr.sidecar = NULL;

    m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_CLOSE;
    vr_push(m);

    videorec_pipe_free(&g_vr.pipe);
    g_vr.state = VIDEOREC_STATE_IDLE;
    g_vr.session_id = 0;
    g_vr.frames = 0;
    g_vr.req_stop = false;
    g_vr.req_pause_toggle = false;
    g_vr.req_snapshot = false;
    g_ptr_array_set_size(g_vr.req_markers, 0);
    g_vr.audio_hold = false;
    g_array_free(g_vr.pause_frames, TRUE);
    g_vr.pause_frames = NULL;
    g_array_free(g_vr.takes, TRUE);
    g_vr.takes = NULL;
    g_array_free(g_vr.hold_events, TRUE);
    g_vr.hold_events = NULL;
    g_vr.hold_overflow = false;
    vr_flag_clear(VIDEOREC_FLAG_ACTIVE);
    fprintf(stderr, "[videorec] Recording stopped: %llu frames\n", (unsigned long long)n);
}

/**
 * @brief Šev: dopíše čekající snímky a založí novou časovou osu (+ nový segment, novou větev).
 *
 * Volá se při nesouvislosti času (reset, nahraný snapshot). Nová osa začíná
 * na `frame_end - VIDEO_SCREEN_TICKS`, takže aktuální snímek na ni přesně padne.
 * Snímky za švem patří nové větvi: snapshot uložený za švem a snapshot
 * uložený před ním se tak dají rozlišit (viz vr_take_find()).
 *
 * @param frame_end Takt konce aktuálního snímku.
 * @pre Pod zámkem, emu vlákno, stav != IDLE.
 */
static void vr_seam(uint64_t frame_end)
{
    uint8_t values[AUDIO_SRC_CHANNELS_COUNT];
    videorec_pipe_flush(&g_vr.pipe);
    vr_current_values(values);
    uint64_t origin = (frame_end >= VIDEO_SCREEN_TICKS) ? frame_end - VIDEO_SCREEN_TICKS : 0;
    videorec_pipe_rebase(&g_vr.pipe, origin, values);
    vr_hold_end(); /* šev: podržené události nové osy se nepřehrávají (dnešní chování) */
    vr_take_begin(vr_frame_count());
    if (g_vr.state == VIDEOREC_STATE_RECORDING) {
        videorec_sidecar_segment_begin(g_vr.sidecar, vr_frame_count(), g_vr.transition);
    }
}

/**
 * @brief Navázání zvuku při retake: obnoví stav rendereru z bodu snapshotu a přehraje podržené události.
 *
 * Volá se po videorec_pipe_rebase() na začátek snímku, ve kterém byl snapshot
 * nahrán (= konec snímku před bodem snapshotu na nové ose). Obnoví stav filtrů
 * a hodnoty kanálů zachycené pro (`take_id`, `frame`) a do pipe pošle události
 * nové osy, které přišly během ochrany švu - první snímek po retake tak dostane
 * stejný zvuk jako nepřerušený běh.
 *
 * Hodnoty kanálů: kanál, který má podržené události, začne zachycenou hodnotou
 * (log zvuku po loadu začíná nulami - audio_reset_log() - a první krok PSG
 * hodnotu hned obnoví); kanál bez událostí převezme hodnotu z živého logu
 * (`log_values`), protože log ji bude dál považovat za platnou a změnu na ni
 * už neohlásí.
 *
 * Bez zachyceného stavu (cizí nebo příliš starý snapshot) nebo po přetečení
 * bufferu událostí se nic nemění (dřívější chování: filtry pokračují ze
 * zahozené budoucnosti, první snímek drží hodnoty z logu).
 *
 * Snapshot uložený během record-pause má klíč (větev, snímek začátku pauzy)
 * shodný se snapshotem uloženým těsně před pauzou; vr_audio_state_find()
 * vrátí nejnovější zachycený stav s tímto klíčem, takže se stav obnoví
 * i tady (není to fallback). V bodě začátku pauzy ale šev zůstává
 * (`pause_at_point` ve vr_handle_snapshot()).
 *
 * @param take_id    Větev snapshotu.
 * @param frame      Snímek snapshotu.
 * @param log_values Hodnoty kanálů z živého logu na konci aktuálního snímku.
 * @return true = stav obnoven a události přehrány.
 * @pre Pod zámkem, emu vlákno, stav != IDLE, pipe právě po rebase.
 */
static bool vr_audio_restore(uint64_t take_id, uint64_t frame, const uint8_t log_values[AUDIO_SRC_CHANNELS_COUNT])
{
    const st_VR_AUDIO_SNAP *as = vr_audio_state_find(take_id, frame);
    if (!as || g_vr.hold_overflow) return false;
    st_VIDEOREC_AUDIO_STATE st = as->state;
    bool has_ev[AUDIO_SRC_CHANNELS_COUNT] = { false };
    for (guint i = 0; i < g_vr.hold_events->len; i++) {
        const st_VR_HOLD_EVENT *e = &g_array_index(g_vr.hold_events, st_VR_HOLD_EVENT, i);
        if (e->ch < AUDIO_SRC_CHANNELS_COUNT) has_ev[e->ch] = true;
    }
    for (int c = 0; c < AUDIO_SRC_CHANNELS_COUNT; c++) {
        if (!has_ev[c]) st.value[c] = log_values[c];
    }
    videorec_audio_set_state(&g_vr.pipe.audio, &st);
    for (guint i = 0; i < g_vr.hold_events->len; i++) {
        const st_VR_HOLD_EVENT *e = &g_array_index(g_vr.hold_events, st_VR_HOLD_EVENT, i);
        videorec_pipe_audio_event(&g_vr.pipe, e->ch, e->value, e->ticks);
    }
    return true;
}

/**
 * @brief Zpracuje nahraný snapshot (retake DISCARD, jinak šev).
 *
 * Podmínky retake viz videorec_on_snapshot_loaded(): stejná session, větev
 * snapshotu v aktuální linii a bod v jejím zachovaném úseku (vr_take_find()),
 * a bod neleží před prvním snímkem aktuálního AVI partu. Poslední podmínka
 * se ověřuje až po bariéře s writerem (vr_writer_sync()), protože rollover
 * partu nastává až při zápisu snímků, které mohou ještě čekat ve frontě.
 *
 * @param frame_end Takt konce aktuálního snímku.
 * @pre Pod zámkem, emu vlákno, stav != IDLE, `req_snapshot`.
 * @post Při retake: linie zkrácena za větev snapshotu + nová větev v bodě
 *       snapshotu, zvuk navázaný na stav z bodu snapshotu (vr_audio_restore()).
 *       Při švu: nová větev na konci nahrávky (vr_seam()).
 */
static void vr_handle_snapshot(uint64_t frame_end)
{
    g_vr.req_snapshot = false;
    uint64_t count = vr_frame_count();
    uint64_t written = videorec_pipe_next_index(&g_vr.pipe); /* předáno writeru před rewind (jen do logu) */
    guint take_idx = 0;
    bool discard = (g_vr.retake_mode == VIDEOREC_RETAKE_DISCARD) && g_vr.snap_has_info &&
                   (g_vr.snap_info.session_id == g_vr.session_id) && (g_vr.snap_info.frame <= count) &&
                   vr_take_find(g_vr.snap_info.take_id, g_vr.snap_info.frame, count, &take_idx);

    if (discard) {
        /* Snímky před bodem snapshotu, které ještě čekají na zvuk, jsou platná minulost:
         * vydat je dřív, než rebase zahodí zbytek fronty (jinak by index přeskočil
         * a TRUNCATE by chtěl víc snímků, než AVI obsahuje). snap_info.frame <= count,
         * takže čekajících snímků je dost. Při švu níže se zbytek vydá také (pořadí
         * snímků zůstane zachované). */
        videorec_pipe_flush_until_index(&g_vr.pipe, g_vr.snap_info.frame);
        /* Rollover partu mohl nastat ve snímcích, které writer ještě nezpracoval. */
        uint64_t part_first = vr_writer_sync();
        if (g_vr.snap_info.frame < part_first) {
            fprintf(stderr, "[videorec] Retake point %llu precedes the current file part (first frame %llu): seam\n",
                    (unsigned long long)g_vr.snap_info.frame, (unsigned long long)part_first);
            discard = false;
        }
    }

    if (!discard) {
        vr_seam(frame_end);
        /* emulační čas: bez auto markeru (šev je hranice segmentu s přechodem; final review I2) */
        vr_state_event(count, VIDEOREC_SC_EVENT_SNAPSHOT, "seam", NULL);
        vr_emit_event(VIDEOREC_EVENT_SEAM, count, NULL, "Recording seam (snapshot loaded)");
        fprintf(stderr, "[videorec] Seam: snapshot loaded at frame %llu\n", (unsigned long long)count);
        return;
    }

    uint8_t values[AUDIO_SRC_CHANNELS_COUNT];
    vr_current_values(values);
    uint64_t origin = (frame_end >= VIDEO_SCREEN_TICKS) ? frame_end - VIDEO_SCREEN_TICKS : 0;
    videorec_pipe_rebase(&g_vr.pipe, origin, values); /* zbylé čekající snímky jsou po bodu snapshotu */
    videorec_pipe_set_next_index(&g_vr.pipe, g_vr.snap_info.frame);
    bool smooth = vr_audio_restore(g_vr.snap_info.take_id, g_vr.snap_info.frame, values);
    vr_hold_end();

    st_VR_MSG *m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_TRUNCATE;
    m->frame_count = g_vr.snap_info.frame;
    vr_push(m);

    videorec_sidecar_truncate(g_vr.sidecar, g_vr.snap_info.frame);

    /* Větve za větví snapshotu jsou zahozená budoucnost; od bodu snapshotu nová větev. */
    g_array_set_size(g_vr.takes, take_idx + 1);
    vr_take_begin(g_vr.snap_info.frame);

    /* Record-pause začatá přesně v bodě snapshotu: snapshot mohl vzniknout během
     * pauzy, kdy emulace běžela dál bez zápisu - jeho stav pak nenavazuje na
     * poslední snímek před pauzou a šev tam musí zůstat. Pauzy za bodem
     * snapshotu jsou zahozenou budoucností. */
    bool pause_at_point = false;
    guint keep = 0;
    while (keep < g_vr.pause_frames->len) {
        uint64_t pf = g_array_index(g_vr.pause_frames, uint64_t, keep);
        if (pf > g_vr.snap_info.frame) break;
        if (pf == g_vr.snap_info.frame) pause_at_point = true;
        keep++;
    }
    g_array_set_size(g_vr.pause_frames, keep);

    if (g_vr.state == VIDEOREC_STATE_PAUSED) {
        /* Pauza trvá: nově začíná v bodě snapshotu (pozdější snímky jsou zahozené). */
        videorec_sidecar_segment_end(g_vr.sidecar, g_vr.snap_info.frame);
        if (!pause_at_point) g_array_append_val(g_vr.pause_frames, g_vr.snap_info.frame);
    } else if (pause_at_point) {
        videorec_sidecar_segment_begin(g_vr.sidecar, g_vr.snap_info.frame, g_vr.transition);
    }

    /* bez auto markeru: retake slibuje plynulé navázání bez viditelného švu (final review I2) */
    vr_state_event(g_vr.snap_info.frame, VIDEOREC_SC_EVENT_SNAPSHOT, "retake", NULL);
    vr_reemit_state(g_vr.snap_info.frame);

    uint64_t sec = g_vr.snap_info.frame * g_vr_platform.fps_den / g_vr_platform.fps_num;
    char ev[64];
    g_snprintf(ev, sizeof(ev), "Retake: rewound to %02u:%02u:%02u", (unsigned)(sec / 3600),
               (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    vr_emit_event(VIDEOREC_EVENT_RETAKE, g_vr.snap_info.frame, NULL, ev);
    fprintf(stderr, "[videorec] Retake: recording rewound to frame %llu (from %llu, %llu written before rewind)%s\n",
            (unsigned long long)g_vr.snap_info.frame, (unsigned long long)count, (unsigned long long)written,
            smooth ? "" : ", audio state not available");
}

/**
 * @brief Předá aktuální snímek párování; řeší plnou frontu a nesouvislost času.
 * @param frame_end Takt konce aktuálního snímku.
 * @pre Pod zámkem, emu vlákno, stav != IDLE.
 */
static void vr_feed_frame(uint64_t frame_end)
{
    const uint8_t *px = (g_vr.state == VIDEOREC_STATE_RECORDING) ? g_framebuffer.pixels : NULL;
    /* Tap zaznamenal skok času zpět: šev i tehdy, když konec snímku náhodou navazuje. */
    if (g_vr.audio_hold) vr_seam(frame_end);
    en_VIDEOREC_PIPE_RESULT r = videorec_pipe_frame(&g_vr.pipe, px, frame_end);
    if (r == VIDEOREC_PIPE_ERR_FULL) {
        /* Horizont nepřišel 64 snímků - nemělo by nastat (synchronizační událost emulace, mzarch_main_event_callback_20ms(), chodí nejpozději po 40 snímcích). */
        videorec_pipe_flush(&g_vr.pipe);
        r = videorec_pipe_frame(&g_vr.pipe, px, frame_end);
    }
    if (r == VIDEOREC_PIPE_ERR_DISCONTINUITY) {
        vr_seam(frame_end);
        r = videorec_pipe_frame(&g_vr.pipe, px, frame_end);
    }
    if (r != VIDEOREC_PIPE_OK) {
        fprintf(stderr, "[videorec] WARNING: frame not accepted (%d)\n", (int)r);
    }
}

/*******************************************************************************
 *
 *                  Veřejné API
 *
 ******************************************************************************/

void videorec_init(void)
{
    g_mutex_lock(&g_vr.mutex);
    if (!g_vr.initialized) {
        g_vr.state = VIDEOREC_STATE_IDLE;
        g_vr.req_markers = g_ptr_array_new_with_free_func(g_free);
        g_vr.queue = g_async_queue_new();
        g_vr.writer = g_thread_new("videorec-writer", vr_writer_thread, g_vr.queue);
        if (!g_atomic_int_get(&g_vr_rta_ready)) {
            videorec_rt_audio_init(&g_vr_rta, VIDEOREC_RT_SDL_RATE, 48000, VIDEOREC_RT_AUDIO_TARGET_MS,
                                   VIDEOREC_RT_AUDIO_CAPACITY_MS);
            g_atomic_int_set(&g_vr_rta_ready, 1);
        }
        g_mutex_lock(&g_vr_rtc.mutex);
        g_vr_rtc.quit = false;
        g_vr_rtc.run = false;
        g_vr_rtc.deadline = 0;
        g_mutex_unlock(&g_vr_rtc.mutex);
        g_vr_rtc.thread = g_thread_new("videorec-rt", vr_rt_thread, NULL);
        g_vr.initialized = true;
    }
    vr_publish_status();
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_exit(void)
{
    g_mutex_lock(&g_vr.mutex);
    if (!g_vr.initialized) {
        g_mutex_unlock(&g_vr.mutex);
        return;
    }
    if (g_vr.state != VIDEOREC_STATE_IDLE) vr_finalize();
    vr_cancel_pending_start();
    g_vr.initialized = false;
    vr_publish_status();
    GAsyncQueue *q = g_vr.queue;
    GThread *t = g_vr.writer;
    g_mutex_unlock(&g_vr.mutex);

    /* Vzorkovač až po uvolnění zámku modulu (tick ho bere); session už je ukončená. */
    g_mutex_lock(&g_vr_rtc.mutex);
    g_vr_rtc.quit = true;
    g_cond_signal(&g_vr_rtc.cond);
    GThread *rt = g_vr_rtc.thread;
    g_vr_rtc.thread = NULL;
    g_mutex_unlock(&g_vr_rtc.mutex);
    if (rt) g_thread_join(rt);

    st_VR_MSG *m = g_new0(st_VR_MSG, 1);
    m->type = VR_MSG_QUIT;
    g_async_queue_push(q, m);
    g_thread_join(t);

    g_mutex_lock(&g_vr.mutex);
    g_async_queue_unref(q);
    g_vr.queue = NULL;
    g_vr.writer = NULL;
    g_ptr_array_free(g_vr.req_markers, TRUE);
    g_vr.req_markers = NULL;
    g_mutex_unlock(&g_vr.mutex);
}

bool videorec_request_start(const st_VIDEOREC_START *s)
{
    if (!s) return false;

    g_mutex_lock(&g_vr.mutex);
    if (!g_vr.initialized) {
        vr_set_error("Video recording is not initialized");
        g_mutex_unlock(&g_vr.mutex);
        return false;
    }
    if (g_vr.state != VIDEOREC_STATE_IDLE || g_vr.req_start) {
        vr_set_error("Video recording is already running");
        g_mutex_unlock(&g_vr.mutex);
        return false;
    }
    unsigned rate = g_videorec_settings.audio_rate;
    if (rate != 44100 && rate != 48000) rate = 48000;
    /* Každý snímek AVI musí nést celý počet vzorků (48000/60 = 800, 44100/60 = 735, 48000/50 = 960). */
    char perr[160];
    if (!videorec_platform_check(&g_vr_platform, rate, perr, sizeof(perr))) {
        char *e = g_strdup_printf("Video recording is not possible on this platform: %s", perr);
        vr_set_error(e);
        fprintf(stderr, "[videorec] ERROR: %s\n", e);
        g_free(e);
        g_mutex_unlock(&g_vr.mutex);
        return false;
    }
    char dir[sizeof(g_videorec_settings.output_dir)];
    g_strlcpy(dir, g_videorec_settings.output_dir, sizeof(dir));
    uint32_t part_limit = g_vr.part_limit;
    g_mutex_unlock(&g_vr.mutex);

    /* Cesta: zadaná, nebo <výstupní adresář>/<platforma>_%Y%m%d_%H%M%S.avi (mz800_..., mz700_..., mz1500_...) */
    char path[sizeof(s->path)];
    if (s->path[0]) {
        g_strlcpy(path, s->path, sizeof(path));
    } else {
        char base_dir[sizeof(g_videorec_settings.output_dir)];
        (void)vr_resolve_dir(dir, base_dir, sizeof(base_dir)); /* neexistující adresář -> chyba při vytvoření souboru */
        GDateTime *now = g_date_time_new_now_local();
        char *fmt = g_strdup_printf("%s_%%Y%%m%%d_%%H%%M%%S", g_vr_platform.name);
        char *stem = g_date_time_format(now, fmt);
        g_free(fmt);
        g_date_time_unref(now);
        /* Vygenerované jméno nesmí přepsat existující nahrávku (stop + start v téže
         * sekundě - writer může starý soubor ještě dopisovat): _2, _3, ... */
        for (unsigned n = 1;; n++) {
            char *name = (n == 1) ? g_strdup_printf("%s.avi", stem) : g_strdup_printf("%s_%u.avi", stem, n);
            char *full = g_build_filename(base_dir, name, NULL);
            char *cuts = g_strdup_printf("%.*s.cuts.json", (int)(strlen(full) - 4), full);
            bool taken = g_file_test(full, G_FILE_TEST_EXISTS) || g_file_test(cuts, G_FILE_TEST_EXISTS);
            g_strlcpy(path, full, sizeof(path));
            g_free(cuts);
            g_free(full);
            g_free(name);
            if (!taken) break;
        }
        g_free(stem);
    }

    st_AVI_WRITER_PARAMS params = { VR_WIDTH, VR_AVI_HEIGHT, g_vr_platform.fps_num, g_vr_platform.fps_den, rate, 2, part_limit };
    st_AVI_WRITER *avi = avi_writer_open(path, &params);

    g_mutex_lock(&g_vr.mutex);
    if (!avi) {
        char *e = g_strdup_printf("Cannot create video file: %s", path);
        vr_set_error(e);
        fprintf(stderr, "[videorec] ERROR: %s\n", e);
        g_free(e);
        g_mutex_unlock(&g_vr.mutex);
        return false;
    }
    if (!g_vr.initialized || g_vr.state != VIDEOREC_STATE_IDLE || g_vr.req_start) {
        /* Souběžný start z jiného vlákna mezitím vyhrál. */
        vr_set_error("Video recording is already running");
        g_mutex_unlock(&g_vr.mutex);
        (void)avi_writer_close(avi);
        (void)g_remove(path);
        return false;
    }
    g_vr.start = *s;
    g_strlcpy(g_vr.start.path, path, sizeof(g_vr.start.path));
    g_vr.start_avi = avi;
    g_vr.audio_rate = rate;
    g_vr.transition = g_videorec_settings.default_transition;
    g_vr.transition_ms = g_videorec_settings.transition_ms;
    g_vr.retake_mode = g_videorec_settings.retake_mode;
    g_vr.keyframe_interval = g_videorec_settings.keyframe_interval;
    g_vr.start_part_limit = part_limit;
    g_vr.timebase = g_videorec_settings.timebase;
    g_vr.rt_pause = g_videorec_settings.realtime_pause;
    unsigned cap = g_videorec_settings.realtime_pause_cap_s;
    if (cap < VIDEOREC_RT_PAUSE_CAP_MIN_S) cap = VIDEOREC_RT_PAUSE_CAP_MIN_S;
    if (cap > VIDEOREC_RT_PAUSE_CAP_MAX_S) cap = VIDEOREC_RT_PAUSE_CAP_MAX_S;
    g_vr.rt_pause_cap_ticks = (uint64_t)cap * g_vr_platform.fps_num / g_vr_platform.fps_den;
    g_vr.rt_speed = g_videorec_settings.realtime_speed;
    g_vr.turbo_audio = g_videorec_settings.realtime_turbo_audio;
    g_vr.state_marks = (g_videorec_settings.state_marks == VIDEOREC_STATE_MARKS_SIDECAR);
    g_vr.auto_markers = g_videorec_settings.auto_markers;
    g_vr.record_debugger_steps = g_videorec_settings.record_debugger_steps;
    g_vr.req_start = true;
    g_vr.req_stop = false;
    g_mutex_lock(&g_vr_err_mutex);
    g_vr_writer_error[0] = '\0';
    g_videorec_last_error[0] = '\0';
    g_mutex_unlock(&g_vr_err_mutex);
    vr_flag_set(VIDEOREC_FLAG_REQ);
    vr_publish_status();
    g_mutex_unlock(&g_vr.mutex);
    return true;
}

void videorec_request_stop(void)
{
    g_mutex_lock(&g_vr.mutex);
    if (g_vr.req_start) {
        vr_cancel_pending_start();
    } else if (g_vr.state != VIDEOREC_STATE_IDLE) {
        g_vr.req_stop = true;
        vr_flag_set(VIDEOREC_FLAG_REQ);
    }
    vr_publish_status();
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_request_pause_toggle(void)
{
    g_mutex_lock(&g_vr.mutex);
    if (g_vr.state != VIDEOREC_STATE_IDLE) {
        g_vr.req_pause_toggle = !g_vr.req_pause_toggle; /* dvojí stisk před koncem snímku se vyruší */
        vr_flag_set(VIDEOREC_FLAG_REQ);
    }
    g_mutex_unlock(&g_vr.mutex);
}

bool videorec_request_pause_set(bool paused)
{
    g_mutex_lock(&g_vr.mutex);
    bool running = (g_vr.state != VIDEOREC_STATE_IDLE);
    if (running) {
        /* Čekající přepnutí = cíl se liší od aktuálního stavu (přepíše i dřívější toggle). */
        g_vr.req_pause_toggle = (paused != (g_vr.state == VIDEOREC_STATE_PAUSED));
        if (g_vr.req_pause_toggle) vr_flag_set(VIDEOREC_FLAG_REQ);
    }
    g_mutex_unlock(&g_vr.mutex);
    return running;
}

void videorec_request_marker(const char *label)
{
    g_mutex_lock(&g_vr.mutex);
    if (g_vr.state != VIDEOREC_STATE_IDLE) {
        g_ptr_array_add(g_vr.req_markers, g_strdup(label ? label : ""));
        vr_flag_set(VIDEOREC_FLAG_REQ);
    }
    g_mutex_unlock(&g_vr.mutex);
}

bool videorec_request_timebase(en_VIDEOREC_TIMEBASE tb)
{
    if (tb != VIDEOREC_TIMEBASE_REALTIME) tb = VIDEOREC_TIMEBASE_EMULATED;
    g_videorec_settings.timebase = tb;
    g_mutex_lock(&g_vr.mutex);
    bool session = (g_vr.state != VIDEOREC_STATE_IDLE) || g_vr.req_start;
    if (session) g_vr.timebase = tb; /* přepnutí provede nejbližší tick vzorkovače */
    vr_publish_status();
    g_mutex_unlock(&g_vr.mutex);
    return session;
}

void videorec_rt_video_tap(const uint8_t *pixels)
{
    g_mutex_lock(&g_vr_rtv_mutex);
    memcpy(g_vr_rtv_pixels, pixels, sizeof(g_vr_rtv_pixels));
    g_vr_rtv_seq++;
    g_mutex_unlock(&g_vr_rtv_mutex);
}

void videorec_rt_audio_output(const float *stereo, size_t frames)
{
    if (!g_atomic_int_get(&g_vr_rta_ready) || !videorec_rt_audio_is_enabled(&g_vr_rta)) return;
    videorec_rt_audio_push(&g_vr_rta, stereo, frames);
}

bool videorec_rt_audio_wants_output(void)
{
    if (!g_atomic_int_get(&g_vr_rta_ready) || !videorec_rt_audio_is_enabled(&g_vr_rta)) return false;
    return videorec_rt_audio_wants_data(&g_vr_rta, vr_sdl_chunk());
}

void videorec_on_debugger_step(void)
{
    g_atomic_int_inc(&g_vr_dbg_steps);
}

void videorec_on_debugger_run(void)
{
    g_atomic_int_set(&g_vr_dbg_run_inflight, 1);
}

void videorec_on_reset(void)
{
    g_mutex_lock(&g_vr.mutex);
    if (g_vr.state != VIDEOREC_STATE_IDLE) {
        g_vr.req_reset = true;
        vr_flag_set(VIDEOREC_FLAG_REQ);
    }
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_get_rt_stats(st_VIDEOREC_RT_STATS *out)
{
    g_mutex_lock(&g_vr.mutex);
    *out = g_vr.rt_stats;
    if (g_vr.rt_on) {
        videorec_rt_audio_get_stats(&g_vr_rta, &out->audio);
        out->clock_rebased = g_vr.rt_clock.rebased;
        out->lost_ticks = g_vr.rt_clock.lost_ticks;
    }
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_test_rt_manual(bool manual)
{
    g_mutex_lock(&g_vr_rtc.mutex);
    g_vr_rtc.manual = manual;
    g_cond_signal(&g_vr_rtc.cond);
    g_mutex_unlock(&g_vr_rtc.mutex);
}

void videorec_test_rt_tick(int64_t now_us)
{
    vr_rt_tick(now_us);
}

en_VIDEOREC_STATE videorec_get_state(void)
{
    g_mutex_lock(&g_vr.mutex);
    en_VIDEOREC_STATE s = g_vr.state;
    g_mutex_unlock(&g_vr.mutex);
    return s;
}

uint64_t videorec_get_frames(void)
{
    g_mutex_lock(&g_vr.mutex);
    uint64_t n = g_vr.frames;
    g_mutex_unlock(&g_vr.mutex);
    return n;
}

uint64_t videorec_get_session_id(void)
{
    g_mutex_lock(&g_vr.mutex);
    uint64_t id = g_vr.session_id;
    g_mutex_unlock(&g_vr.mutex);
    return id;
}

bool videorec_is_start_pending(void)
{
    g_mutex_lock(&g_vr.mutex);
    bool p = g_vr.req_start;
    g_mutex_unlock(&g_vr.mutex);
    return p;
}

void videorec_test_set_part_limit(uint32_t max_bytes)
{
    g_mutex_lock(&g_vr.mutex);
    g_vr.part_limit = max_bytes;
    g_mutex_unlock(&g_vr.mutex);
}

bool videorec_get_snapinfo(st_VIDEOREC_SNAPINFO *out)
{
    bool ok = false;
    g_mutex_lock(&g_vr.mutex);
    memset(out, 0, sizeof(*out));
    if (g_vr.state != VIDEOREC_STATE_IDLE) {
        out->session_id = g_vr.session_id;
        out->frame = vr_frame_count();
        ok = true;
        if (g_vr.req_snapshot || g_vr.audio_hold) {
            /* Časová osa se přerušila (nahraný snapshot nebo skok času čeká na zpracování
             * na konci snímku): ukládaný stav už nepatří poslední větvi linie. Bod mimo
             * linii (VIDEOREC_TAKE_ID_NONE žádná větev nemá) -> pozdější load vždy šev.
             * Stav zvuku se pro takový bod nezachytává (retake na něj nikdy nebude). */
            out->take_id = VIDEOREC_TAKE_ID_NONE;
        } else {
            out->take_id = g_array_index(g_vr.takes, st_VR_TAKE, g_vr.takes->len - 1).take_id;
            /* Zapamatovat stav zvuku pro tento bod (plynulý retake); čeká-li poslední
             * snímek na zvuk, zachytí se až při jeho vyrenderování. */
            uint64_t seq = ++g_vr.audio_state_seq;
            st_VR_AUDIO_SNAP *as = &g_vr.audio_states[seq % VR_AUDIO_STATES];
            memset(as, 0, sizeof(*as));
            as->seq = seq;
            as->take_id = out->take_id;
            as->frame = out->frame;
            videorec_pipe_request_capture(&g_vr.pipe, seq);
        }
    }
    g_mutex_unlock(&g_vr.mutex);
    return ok;
}

void videorec_on_snapshot_loaded(const st_VIDEOREC_SNAPINFO *info_or_null)
{
    g_mutex_lock(&g_vr.mutex);
    if (g_vr.state != VIDEOREC_STATE_IDLE) {
        g_vr.req_snapshot = true;
        /* Od teď přicházejí události nové časové osy: do švu je nepouštět do starých snímků
         * (podrží se a při retake se přehrají, viz vr_audio_restore()). */
        vr_hold_begin();
        g_vr.snap_has_info = (info_or_null != NULL);
        if (info_or_null) g_vr.snap_info = *info_or_null;
        vr_flag_set(VIDEOREC_FLAG_REQ);
    }
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_on_screen_done(void)
{
    /* Nečinný stav: jediné atomické čtení (aktivita i požadavky jsou bity jednoho slova). */
    gint flags = g_atomic_int_get(&g_videorec_active);
    if (flags == 0) return;
    /* Realtime bez požadavku: snímky zapisuje vzorkovač, konec snímku nic nedělá (stejné jedno čtení). */
    if ((flags & (VIDEOREC_FLAG_RT | VIDEOREC_FLAG_REQ)) == VIDEOREC_FLAG_RT) return;

    bool quit = false;
    g_mutex_lock(&g_vr.mutex);
    vr_flag_clear(VIDEOREC_FLAG_REQ);

    /* Konec snímku zaokrouhlený na násobek VIDEO_SCREEN_TICKS (hook běží na konci posledního řádku). */
    uint64_t frame_end = ((gdg_get_total_ticks() + VIDEO_SCREEN_TICKS / 2) / VIDEO_SCREEN_TICKS) * VIDEO_SCREEN_TICKS;

    if (g_vr.state == VIDEOREC_STATE_IDLE) {
        if (g_vr.req_start && g_vr.initialized) vr_do_start(frame_end);
        g_vr.req_stop = false;
        g_vr.req_pause_toggle = false;
        g_vr.req_snapshot = false;
        if (g_vr.req_markers) g_ptr_array_set_size(g_vr.req_markers, 0);
        if (g_vr.quit_pending) {
            /* vzorkovač dosáhl stop_after_frames s quit_after_stop (realtime) */
            g_vr.quit_pending = false;
            quit = true;
        }
        vr_publish_status();
        g_mutex_unlock(&g_vr.mutex);
        if (quit) {
            fprintf(stderr, "[videorec] Requested frame count reached, quitting the emulator\n");
            emulator_quit(EXIT_SUCCESS); /* stejný kontext jako níže; nevrátí se */
        }
        return;
    }

    if (g_vr.rt_on) {
        if (!g_vr.rt_leaving) {
            /* Požadavky v realtime zpracovává vzorkovač. */
            g_mutex_unlock(&g_vr.mutex);
            return;
        }
        vr_rt_leave(frame_end); /* tento snímek už patří emulačnímu času */
    }

    /* Chyba zápisu ve writeru -> ukončit nahrávání s chybou. */
    if (g_atomic_int_get(&g_vr_failed_gen) == (gint)g_vr.gen) {
        g_mutex_lock(&g_vr_err_mutex);
        g_strlcpy(g_videorec_last_error, g_vr_writer_error[0] ? g_vr_writer_error : "Video file write error",
                  sizeof(g_videorec_last_error));
        g_mutex_unlock(&g_vr_err_mutex);
        vr_finalize();
        vr_publish_status();
        g_mutex_unlock(&g_vr.mutex);
        return;
    }

    vr_observe_pause(); /* konec pauzy v emulačním čase: první konec snímku po pokračování */
    vr_observe_speed_reset();

    for (guint i = 0; i < g_vr.req_markers->len; i++) {
        videorec_sidecar_add_marker(g_vr.sidecar, vr_frame_count(), g_ptr_array_index(g_vr.req_markers, i));
    }
    g_ptr_array_set_size(g_vr.req_markers, 0);

    if (g_vr.req_snapshot) vr_handle_snapshot(frame_end);

    if (g_vr.req_pause_toggle) {
        g_vr.req_pause_toggle = false;
        if (g_vr.state == VIDEOREC_STATE_RECORDING) {
            uint64_t at = vr_frame_count();
            videorec_sidecar_segment_end(g_vr.sidecar, at);
            g_array_append_val(g_vr.pause_frames, at);
            g_vr.state = VIDEOREC_STATE_PAUSED;
            fprintf(stderr, "[videorec] Recording paused\n");
        } else {
            videorec_sidecar_segment_begin(g_vr.sidecar, vr_frame_count(), g_vr.transition);
            g_vr.state = VIDEOREC_STATE_RECORDING;
            fprintf(stderr, "[videorec] Recording resumed\n");
        }
    }

    if (g_vr.req_stop) {
        vr_finalize();
    } else {
        vr_feed_frame(frame_end);
        g_vr.frames = vr_frame_count();
        if (g_vr.stop_after_frames && g_vr.frames >= g_vr.stop_after_frames) {
            quit = g_vr.quit_after_stop;
            vr_finalize();
        }
    }
    vr_publish_status();
    g_mutex_unlock(&g_vr.mutex);

    vr_backpressure();

    if (quit) {
        fprintf(stderr, "[videorec] Requested frame count reached, quitting the emulator\n");
        /* Bezpečné: stejný kontext jako emulator_quit() v mzarch_main_event_callback_20ms()
         * (emu vlákno, uvnitř zpracování události). videorec_exit() v emulator_quit()
         * počká na dokončení všech zápisů writeru (join). Funkce se nevrátí (longjmp). */
        emulator_quit(EXIT_SUCCESS);
    }
}

void videorec_on_emulation_stopped(void)
{
    /* Emulace stojí: případný běh k dočasnému breakpointu skončil (step over / run to
     * cursor). Ruší se i bez nahrávání, aby příznak nepřežil do další session. */
    if (g_atomic_int_get(&g_videorec_active) == 0) {
        (void)g_atomic_int_compare_and_exchange(&g_vr_dbg_run_inflight, 1, 0);
        return;
    }
    g_mutex_lock(&g_vr.mutex);
    bool temp_bp = g_atomic_int_compare_and_exchange(&g_vr_dbg_run_inflight, 1, 0);
    if (g_vr.state != VIDEOREC_STATE_IDLE && temp_bp) g_vr.dbg_pause = true;
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_on_emulation_paused(void)
{
    if (g_atomic_int_get(&g_videorec_active) == 0) return;
    g_mutex_lock(&g_vr.mutex);
    /* Začátek pauzy v emulačním čase (i během čekání na návrat z realtime): přesná
     * pozice. V efektivní realtime sleduje pauzu vzorkovač. */
    if (g_vr.state != VIDEOREC_STATE_IDLE && (!g_vr.rt_on || g_vr.rt_leaving)) vr_observe_pause();
    /* Jen stop: ostatní požadavky jsou vázané na pozici v nahrávce a počkají
     * na nejbližší konec snímku (bit VIDEOREC_FLAG_REQ zůstává nastaven). */
    if (g_vr.state != VIDEOREC_STATE_IDLE && g_vr.req_stop) vr_finalize();
    vr_publish_status();
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_audio_tap(unsigned src_id, uint8_t value, uint64_t ticks)
{
    g_mutex_lock(&g_vr.mutex);
    if (g_vr.state != VIDEOREC_STATE_IDLE && !g_vr.rt_on && src_id < AUDIO_SRC_CHANNELS_COUNT) {
        /* Události jednoho kanálu chodí s neklesajícím časem; skok zpět = nová
         * časová osa (nahraný snapshot), kterou frame hook ještě nezachytil.
         * Do švu se události nepouštějí do pipe, aby nepronikly do starých
         * čekajících snímků; podrží se pro případný retake (vr_audio_restore()). */
        if (ticks < g_vr.last_tap[src_id]) vr_hold_begin();
        if (!g_vr.audio_hold) {
            g_vr.last_tap[src_id] = ticks;
            videorec_pipe_audio_event(&g_vr.pipe, src_id, value, ticks);
        } else if (g_vr.hold_events->len < VR_HOLD_MAX_EVENTS) {
            st_VR_HOLD_EVENT e = { ticks, (uint8_t)src_id, value };
            g_array_append_val(g_vr.hold_events, e);
        } else {
            g_vr.hold_overflow = true;
        }
    }
    g_mutex_unlock(&g_vr.mutex);
}

void videorec_audio_horizon(uint64_t ticks)
{
    g_mutex_lock(&g_vr.mutex);
    bool emu = (g_vr.state != VIDEOREC_STATE_IDLE && !g_vr.rt_on);
    if (emu && !g_vr.audio_hold) {
        /* Rozložení L/R podle aktuálního stavu PSG (MZ-800 může druhý PSG zapnout za běhu),
         * jako SDL cesta, která o mixu rozhoduje po blocích podle stereo příznaku logu. */
        videorec_pipe_set_stereo(&g_vr.pipe, vr_psg_stereo());
        videorec_pipe_horizon(&g_vr.pipe, ticks);
    }
    g_mutex_unlock(&g_vr.mutex);
    if (emu) vr_backpressure();
}

