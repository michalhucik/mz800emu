/**
 * @file   replay.c
 * @brief  Záznam a přehrání vstupů - stavový automat, háčky emu vlákna
 *         a fronta požadavků (viz replay.h).
 *
 * Pořadí v bodě B_f (stejné při záznamu i přehrávání, jinak by se
 * přehrání rozjelo):
 *
 * | Krok | Záznam | Přehrávání |
 * |------|--------|------------|
 * | 1 | čekající reset -> reset, událost `b` | události `d` snímku, které drain nestihl |
 * | 2 | převzetí vstupů z UI (matice, VKBD, numerická klávesnice) | události `b` v pořadí souboru |
 * | 3 | čekající autotype -> start, událost `b` | zápis vstupů (matice, VKBD = 0FFh, joystick) |
 * | 4 | vzorek vstupů, změny jako události `b` | - |
 * | 5 | hash každých N snímků | porovnání hashe (mimo poslední snímek se závěrečným hashem) |
 * | 6 | (v pauze: zásahy `p`) | události `p`, pak konec, je-li to poslední snímek |
 *
 * Za krokem 5 (záznam) nebo 6 (přehrávání) se každých N snímků uloží
 * checkpoint (replay_ckpt.h); rewind a seek ho načtou v paused smyčce.
 *
 * Stav záznamu a přehrávání (s_rec, s_play, kurzory, checkpointy) patří emu
 * vláknu. Pod zámkem s_lock jsou jen fronta požadavků, kopie stavu pro
 * čtenáře a vstupy předávané z UI vlákna.
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "replay.h"
#include "replay_format.h"
#include "replay_media.h"
#include "replay_ckpt.h"
#include "media_writes.h"
#include "state_rev.h"
#include "state_hash.h"
#include "emulator.h"
#include "cfgmain.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/joy/joy.h"
#include "snapshot/snapshot.h"
#include "hw-generic/unicard/unicard.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "automation.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/replay_dbg.h"
#include "debugger/freeze/freeze.h"
#include "debugger/debugger.h"
#include "debugger/run_control/run_control.h"
#endif


int g_replay_mode = REPLAY_MODE_OFF;
int g_replay_frame_hook = 0;

/** @brief Bit g_replay_frame_hook: záznam nebo přehrávání běží. */
#define REPLAY_HOOK_ACTIVE 1
/** @brief Bit g_replay_frame_hook: čeká požadavek. */
#define REPLAY_HOOK_REQUEST 2
/** @brief Bit g_replay_frame_hook: čeká operace kazety z UI (i mimo záznam). */
#define REPLAY_HOOK_UI 4


/** @brief Druh požadavku ve frontě. */
typedef enum en_REPLAY_REQ_KIND
{
    REPLAY_REQ_RECORD = 0, /**< Start záznamu. */
    REPLAY_REQ_PLAY,       /**< Start přehrávání. */
    REPLAY_REQ_STOP,       /**< Konec záznamu nebo přehrávání. */
    REPLAY_REQ_REWIND,     /**< Návrat na checkpoint (záznam i přehrávání). */
    REPLAY_REQ_SEEK,       /**< Skok přehrávání na snímek. */
    REPLAY_REQ_GOTO        /**< Skok přehrávání na čas (goto). */
} en_REPLAY_REQ_KIND;

/**
 * @brief Požadavek ve frontě (vlastní fronta, uvolní replay_req_free()).
 */
typedef struct st_REPLAY_REQ
{
    en_REPLAY_REQ_KIND kind;  /**< Druh. */
    uint32_t seq;             /**< Pořadové číslo (> 0). */
    char *path;               /**< Soubor (záznam, přehrání), jinak NULL. */
    char *description;        /**< Popis záznamu nebo NULL. */
    uint32_t hash_interval;   /**< Interval hashů (0 = výchozí). */
    bool pause_after_load;    /**< Přehrání: zůstat po načtení v pauze. */
    uint32_t frames;          /**< Rewind: o kolik snímků (0 = jeden checkpoint); seek: cílový snímek. */
    st_REPLAY_CKPT_CFG ckpt;  /**< Záznam, přehrání: nastavení checkpointů (< 0 = z INI). */
    uint64_t ticks;           /**< Goto: cílový čas v taktech GDG. */
    bool keep_bps;            /**< Goto: uživatelské breakpointy platí dál. */
    bool ckpt_at_target;      /**< Goto: checkpoint na hranici snímku seeku. */
} st_REPLAY_REQ;


/** @brief Zámek fronty, kopie stavu a vstupů z UI (statický GMutex, init netřeba). */
static GMutex s_lock;

/* ---- pod s_lock ---- */
static GQueue s_requests = G_QUEUE_INIT; /**< Fronta st_REPLAY_REQ *. */
static uint32_t s_request_seq = 0;       /**< Poslední přidělené číslo požadavku. */
static st_REPLAY_STATUS s_status;        /**< Kopie stavu pro čtenáře. */
static char s_refusal[REPLAY_STATUS_TEXT_MAX]; /**< Poslední odmítnutí příkazu. */
static uint8_t s_host_kbd[REPLAY_KBD_COLS];    /**< Poslední sken klávesnice z UI. */
static int s_host_joy_dev = -1;                /**< Joystick z numerické klávesnice (-1 = žádný). */
static uint8_t s_host_joy_state = 0xFF;        /**< Jeho stav. */
static uint8_t s_ui_vkbd[REPLAY_KBD_COLS];     /**< Matice okna VKBD během záznamu. */
static bool s_pending_reset = false;           /**< Reset čeká na hranici snímku. */
static bool s_pending_autotype = false;        /**< Autotype čeká na hranici snímku. */
static char *s_pending_autotype_text = NULL;   /**< Jeho text ("" = zastavit). */
static uint32_t s_pending_kd_ms = 0;           /**< Jeho doba stisku. */
static uint32_t s_pending_ku_ms = 0;           /**< Jeho doba uvolnění. */
static GQueue s_ui_cmt = G_QUEUE_INIT;          /**< Operace kazety z UI (st_REPLAY_UI_CMT *). */
static bool s_pending_delays = false;          /**< Mimo záznam: nové prodlevy autotype čekají na emu vlákno. */
static uint32_t s_pending_delay_kd_ms = 0;     /**< Jejich doba stisku. */
static uint32_t s_pending_delay_ku_ms = 0;     /**< Jejich doba uvolnění. */
static uint8_t s_off_vkbd_press[REPLAY_KBD_COLS];   /**< Mimo záznam: bity okna VKBD ke stisku (1 = stisknout). */
static uint8_t s_off_vkbd_release[REPLAY_KBD_COLS]; /**< Mimo záznam: bity okna VKBD k uvolnění (1 = uvolnit). */
static bool s_off_host_pending = false;             /**< Mimo záznam: sken klávesnice hostitele čeká na emu vlákno. */
static uint8_t s_off_host_kbd[REPLAY_KBD_COLS];     /**< Jeho matice (poslední sken). */
static int s_off_host_joy_dev = -1;                 /**< Jeho joystick z numerické klávesnice (-1 = žádný). */
static uint8_t s_off_host_joy_state = 0xFF;         /**< Stav toho joysticku. */

/** @brief Operace kazety z UI čekající na emu vlákno. */
typedef struct st_REPLAY_UI_CMT
{
    en_REPLAY_CMT_OP op; /**< Operace. */
    int32_t arg;         /**< Hodnota. */
    char *path;          /**< Cesta (REPLAY_CMT_RECORD) nebo NULL. */
} st_REPLAY_UI_CMT;

/* ---- emu vlákno ---- */
static st_REPLAY_DATA *s_rec = NULL;     /**< Probíhající záznam. */
static st_REPLAY_DATA *s_play = NULL;    /**< Přehrávaný záznam. */
static guint s_ev_cursor = 0;            /**< Další událost k přehrání. */
static guint s_hash_cursor = 0;          /**< Další řádek hashů k ověření. */
static uint64_t s_start_screens = 0;     /**< g_gdg.total_elapsed.screens při startu. */
static uint64_t s_boundary_ticks = 0;    /**< Čas GDG posledního bodu B_f (nebo startu). */
static uint32_t s_frame = 0;             /**< Snímek posledního bodu B_f. */
static uint8_t s_last_kbd[REPLAY_KBD_COLS]; /**< Záznam: poslední zapsaná matice. */
static uint8_t s_last_joy[REPLAY_JOY_PORTS]; /**< Záznam: poslední zapsaný stav joysticku. */
static uint8_t s_play_kbd[REPLAY_KBD_COLS];  /**< Přehrávání: aktuální matice. */
static uint8_t s_play_joy[REPLAY_JOY_PORTS]; /**< Přehrávání: aktuální stav joysticku. */
static bool s_deviated = false;          /**< Přehrávání odchýleno. */
static bool s_applying = false;          /**< Přehrávač právě provádí uloženou událost `dbg`. */
static bool s_resume_after_request = false; /**< Pauzu vyvolal požadavek - po vyřízení rozběhnout. */
static bool s_exit_at_end = false;       /**< --exit-at-replay-end. */
static char *s_autotype_buf = NULL;      /**< Text autotype spuštěného záznamem / přehrávačem (žije, dokud ho nenahradí další). */
static uint64_t s_start_ticks = 0;       /**< gdg_get_total_ticks() při startu záznamu / přehrávání. */
static GDateTime *s_rtc_base = NULL;     /**< Základ RTC (složky místního času jako UTC, bez pásma), NULL = hostitelský čas. */
static guint s_cmthack_cursor = 0;       /**< Přehrávání: další událost `cmthack` (podle pořadí, ne snímku). */
static int s_cmthack_count = 0;          /**< Záznam: počet výběrů CMT hacku (index slotu `cmthack.K`). */
static int s_record_count = 0;           /**< Přehrávání: počet nahrávání na kazetu (jméno dočasného souboru). */
static st_REPLAY_CKPT_RING s_ckpt;       /**< Checkpointy běžícího záznamu / přehrávání (capacity 0 = žádné). */
static uint32_t s_ckpt_interval = 0;     /**< Interval checkpointů ve snímcích, 0 = vypnuto. */
static bool s_seeking = false;           /**< Přehrávání dohrává na cíl seeku. */
static uint32_t s_seek_target = 0;       /**< Cílový snímek seeku. */
static bool s_seek_saved_max = false;    /**< MAX SPEED před seekem (po seeku vrátit). */
static bool s_goto_active = false;       /**< Goto běží (seek nebo dojetí na čas). */
static uint64_t s_goto_ticks = 0;        /**< Cílový čas goto (takty GDG). */
static bool s_goto_ckpt = false;         /**< Goto: checkpoint na hranici snímku seeku. */
static bool s_goto_saved_max = false;    /**< MAX SPEED před goto (po goto vrátit). */
static int32_t s_goto_seek_from = REPLAY_GOTO_FROM_NONE; /**< Odkud goto vyšlo (st_REPLAY_STATUS). */
static bool s_goto_ckpt_taken = false;   /**< Goto uložilo checkpoint v cílovém snímku. */

static void replay_goto_finish(void);

/* ---- konfigurace (INI [REPLAY]) ---- */
static unsigned s_cfg_ckpt_interval = REPLAY_CKPT_DEFAULT_INTERVAL; /**< `checkpoint_interval`. */
static unsigned s_cfg_ckpt_count = REPLAY_CKPT_DEFAULT_COUNT;       /**< `checkpoint_count`. */
static bool s_cfg_registered = false;    /**< Sekce [REPLAY] je zaregistrovaná. */


const char *replay_outcome_name(en_REPLAY_OUTCOME outcome)
{
    switch (outcome)
    {
        case REPLAY_OUTCOME_RECORDED: return "recorded";
        case REPLAY_OUTCOME_ENDED: return "ended";
        case REPLAY_OUTCOME_DESYNC: return "desync";
        case REPLAY_OUTCOME_STOPPED: return "stopped";
        case REPLAY_OUTCOME_ERROR: return "error";
        case REPLAY_OUTCOME_NONE:
        default: break;
    };
    return "none";
}


const char *replay_cmt_op_name(en_REPLAY_CMT_OP op)
{
    switch (op)
    {
        case REPLAY_CMT_PLAY: return "play";
        case REPLAY_CMT_PLAY_PAUSED: return "play_paused";
        case REPLAY_CMT_STOP: return "stop";
        case REPLAY_CMT_STOP_BUTTON: return "stop_button";
        case REPLAY_CMT_PAUSE: return "pause";
        case REPLAY_CMT_EJECT: return "eject";
        case REPLAY_CMT_SEEK: return "seek";
        case REPLAY_CMT_SELECT: return "select";
        case REPLAY_CMT_SELECT_FORCED: return "select_forced";
        case REPLAY_CMT_NEXT: return "next";
        case REPLAY_CMT_PREV: return "prev";
        case REPLAY_CMT_RECORD: return "record";
        default: break;
    };
    return "unknown";
}


/** @brief Operace kazety podle jména, -1 = neznámá. */
static int replay_cmt_op_from_name(const char *name)
{
    for (int i = 0; name && i < REPLAY_CMT_OP_COUNT; i++)
    {
        if (strcmp(name, replay_cmt_op_name((en_REPLAY_CMT_OP)i)) == 0) return i;
    };
    return -1;
}


const char *replay_mode_name(en_REPLAY_MODE mode)
{
    switch (mode)
    {
        case REPLAY_MODE_RECORDING: return "recording";
        case REPLAY_MODE_PLAYING: return "playing";
        case REPLAY_MODE_OFF:
        default: break;
    };
    return "off";
}


bool replay_is_active(void)
{
    return g_atomic_int_get(&g_replay_mode) != REPLAY_MODE_OFF;
}


/**
 * @brief Zaregistruje sekci INI `[REPLAY]` (checkpointy), jen jednou.
 *
 * `checkpoint_interval` (snímky, 0 = vypnuto) a `checkpoint_count`
 * (1 .. REPLAY_CKPT_MAX_COUNT); čísla v INI jsou hex jako všude v cfgfile.
 */
static void replay_cfg_init(void)
{
    if (s_cfg_registered || !g_cfgmain) return;
    s_cfg_registered = true;
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "REPLAY");
    CFGELM *elm = cfgmodule_register_new_element(cmod, "checkpoint_interval", CFGENTYPE_UNSIGNED,
                                                 REPLAY_CKPT_DEFAULT_INTERVAL, 0, 0xFFFFFF);
    cfgelement_set_handlers(elm, (void *)&s_cfg_ckpt_interval, (void *)&s_cfg_ckpt_interval);
    elm = cfgmodule_register_new_element(cmod, "checkpoint_count", CFGENTYPE_UNSIGNED,
                                         REPLAY_CKPT_DEFAULT_COUNT, 1, REPLAY_CKPT_MAX_COUNT);
    cfgelement_set_handlers(elm, (void *)&s_cfg_ckpt_count, (void *)&s_cfg_ckpt_count);
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}


void replay_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    memset(s_ui_vkbd, 0xFF, sizeof(s_ui_vkbd));
    memset(s_host_kbd, 0xFF, sizeof(s_host_kbd));
    replay_cfg_init();
}


void replay_set_exit_at_end(bool enable)
{
    s_exit_at_end = enable;
}


void replay_take_refusal(char *out, size_t size)
{
    if (!out || !size) return;
    g_mutex_lock(&s_lock);
    g_strlcpy(out, s_refusal, size);
    s_refusal[0] = '\0';
    g_mutex_unlock(&s_lock);
}


uint32_t replay_current_frame(void)
{
    return (uint32_t)((uint64_t)g_gdg.total_elapsed.screens - s_start_screens);
}


bool replay_paused_at_boundary(void)
{
    return EMULATOR_TEST_PAUSED && gdg_get_total_ticks() == s_boundary_ticks;
}


/**
 * @brief Nastaví základ RTC z textu `YYYY-MM-DDTHH:MM:SS` (NULL / neplatný = žádný).
 *
 * Složky se uloží jako UTC: přičtení emulovaného času pak nezávisí na
 * pásmu ani letním čase hostitele.
 */
static void replay_rtc_set_base(const char *text)
{
    if (s_rtc_base) g_date_time_unref(s_rtc_base);
    s_rtc_base = NULL;
    int y, mo, d, h, mi, se;
    if (text && sscanf(text, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6)
    {
        s_rtc_base = g_date_time_new_utc(y, mo, d, h, mi, (gdouble)se);
    };
}


bool replay_rtc_now(int *year, int *month, int *day, int *hour, int *min, int *sec)
{
    if (!s_rtc_base || (!s_rec && !s_play)) return false;
    uint64_t elapsed = (gdg_get_total_ticks() - s_start_ticks) / (uint64_t)GDGCLK_BASE;
    GDateTime *t = g_date_time_add_seconds(s_rtc_base, (gdouble)elapsed);
    if (!t) return false;
    *year = g_date_time_get_year(t);
    *month = g_date_time_get_month(t);
    *day = g_date_time_get_day_of_month(t);
    *hour = g_date_time_get_hour(t);
    *min = g_date_time_get_minute(t);
    *sec = g_date_time_get_second(t);
    g_date_time_unref(t);
    return true;
}


/* ================================================================
 * Stav pro čtenáře
 * ================================================================ */

/** @brief Nastaví režim a bit 0 příznaku háčku (emu vlákno). */
static void replay_set_mode(en_REPLAY_MODE mode)
{
    g_atomic_int_set(&g_replay_mode, (int)mode);
    if (mode != REPLAY_MODE_OFF) g_atomic_int_or((guint *)&g_replay_frame_hook, REPLAY_HOOK_ACTIVE);
    else g_atomic_int_and((guint *)&g_replay_frame_hook, ~(guint)REPLAY_HOOK_ACTIVE);
}


/** @brief Zapíše průběžný stav (režim, snímek, počty) do kopie pro čtenáře. */
static void replay_publish(void)
{
    const st_REPLAY_DATA *d = s_rec ? s_rec : s_play;
    g_mutex_lock(&s_lock);
    s_status.mode = (en_REPLAY_MODE)g_replay_mode;
    s_status.frame = s_frame;
    /* Délka zůstane po konci přehrávání (výsledek, result.json); nový
     * záznam ji vynuluje. */
    if (s_play) s_status.frames = s_play->frames;
    else if (s_rec) s_status.frames = 0;
    s_status.events = d ? (s_play ? s_ev_cursor : d->events->len) : 0;
    s_status.hashes = d ? (s_play ? s_hash_cursor : d->hashes->len) : 0;
    s_status.hash_interval = d ? d->hash_interval : 0;
    s_status.warnings = d ? d->warnings->len : 0;
    s_status.deviated = s_play ? s_deviated : false;
    s_status.checkpoint_interval = (s_rec || s_play) ? s_ckpt_interval : 0;
    s_status.checkpoint_capacity = s_ckpt.capacity;
    s_status.checkpoints = s_ckpt.count;
    const st_REPLAY_CKPT *first = replay_ckpt_ring_get(&s_ckpt, 0);
    const st_REPLAY_CKPT *last = s_ckpt.count ? replay_ckpt_ring_get(&s_ckpt, s_ckpt.count - 1) : NULL;
    s_status.checkpoint_first = first ? first->frame : 0;
    s_status.checkpoint_last = last ? last->frame : 0;
    s_status.checkpoint_bytes = s_ckpt.bytes;
    if (s_rec) s_status.rerecord_count = s_rec->rerecord_count;
    else if (s_play) s_status.rerecord_count = s_play->rerecord_count;
    s_status.seeking = s_seeking;
    s_status.seek_target = s_seeking ? s_seek_target : 0;
    s_status.goto_active = s_goto_active;
    s_status.goto_target = s_goto_ticks;
    s_status.goto_seek_from = s_goto_seek_from;
    s_status.goto_checkpoint = s_goto_ckpt_taken;
    g_mutex_unlock(&s_lock);
}


/** @brief Zapíše výsledek skončeného záznamu / přehrávání. */
static void replay_publish_outcome(en_REPLAY_OUTCOME outcome, uint32_t frame, const char *message,
                                   const char *desync_parts)
{
    g_mutex_lock(&s_lock);
    s_status.outcome = outcome;
    s_status.outcome_frame = frame;
    g_strlcpy(s_status.outcome_message, message ? message : "", sizeof(s_status.outcome_message));
    g_strlcpy(s_status.desync_parts, desync_parts ? desync_parts : "", sizeof(s_status.desync_parts));
    g_mutex_unlock(&s_lock);
    fprintf(stderr, "[REPLAY] %s at frame %u%s%s\n", replay_outcome_name(outcome), frame,
            (message && message[0]) ? ": " : "", message ? message : "");
}


void replay_get_status(st_REPLAY_STATUS *out)
{
    if (!out) return;
    g_mutex_lock(&s_lock);
    *out = s_status;
    g_mutex_unlock(&s_lock);
}


/* ================================================================
 * Požadavky
 * ================================================================ */

/** @brief Uvolní požadavek. */
static void replay_req_free(st_REPLAY_REQ *r)
{
    if (!r) return;
    g_free(r->path);
    g_free(r->description);
    g_free(r);
}


/** @brief Zařadí požadavek a vrátí jeho číslo. */
static uint32_t replay_enqueue(st_REPLAY_REQ *r)
{
    g_mutex_lock(&s_lock);
    r->seq = ++s_request_seq;
    if (r->seq == 0) r->seq = ++s_request_seq;
    g_queue_push_tail(&s_requests, r);
    g_atomic_int_or((guint *)&g_replay_frame_hook, REPLAY_HOOK_REQUEST);
    uint32_t seq = r->seq;
    g_mutex_unlock(&s_lock);
    return seq;
}


/** @brief Nastavení checkpointů do požadavku (NULL = vše z INI). */
static void replay_req_set_ckpt(st_REPLAY_REQ *r, const st_REPLAY_CKPT_CFG *ckpt)
{
    r->ckpt.interval = ckpt ? ckpt->interval : -1;
    r->ckpt.count = ckpt ? ckpt->count : -1;
}


uint32_t replay_request_record_ex(const char *path, uint32_t hash_interval, const char *description,
                                  const st_REPLAY_CKPT_CFG *ckpt)
{
    st_REPLAY_REQ *r = g_new0(st_REPLAY_REQ, 1);
    r->kind = REPLAY_REQ_RECORD;
    r->path = g_strdup(path);
    r->description = g_strdup(description);
    r->hash_interval = hash_interval;
    replay_req_set_ckpt(r, ckpt);
    return replay_enqueue(r);
}


uint32_t replay_request_record(const char *path, uint32_t hash_interval, const char *description)
{
    return replay_request_record_ex(path, hash_interval, description, NULL);
}


uint32_t replay_request_play_ex(const char *path, bool pause_after_load, const st_REPLAY_CKPT_CFG *ckpt)
{
    st_REPLAY_REQ *r = g_new0(st_REPLAY_REQ, 1);
    r->kind = REPLAY_REQ_PLAY;
    r->path = g_strdup(path);
    r->pause_after_load = pause_after_load;
    replay_req_set_ckpt(r, ckpt);
    return replay_enqueue(r);
}


uint32_t replay_request_play(const char *path, bool pause_after_load)
{
    return replay_request_play_ex(path, pause_after_load, NULL);
}


uint32_t replay_request_rewind(uint32_t frames)
{
    st_REPLAY_REQ *r = g_new0(st_REPLAY_REQ, 1);
    r->kind = REPLAY_REQ_REWIND;
    r->frames = frames;
    return replay_enqueue(r);
}


uint32_t replay_request_seek(uint32_t frame)
{
    st_REPLAY_REQ *r = g_new0(st_REPLAY_REQ, 1);
    r->kind = REPLAY_REQ_SEEK;
    r->frames = frame;
    return replay_enqueue(r);
}


uint32_t replay_request_goto(uint64_t ticks, bool keep_breakpoints, bool checkpoint_at_target)
{
    st_REPLAY_REQ *r = g_new0(st_REPLAY_REQ, 1);
    r->kind = REPLAY_REQ_GOTO;
    r->ticks = ticks;
    r->keep_bps = keep_breakpoints;
    r->ckpt_at_target = checkpoint_at_target;
    return replay_enqueue(r);
}


uint32_t replay_request_stop(void)
{
    st_REPLAY_REQ *r = g_new0(st_REPLAY_REQ, 1);
    r->kind = REPLAY_REQ_STOP;
    return replay_enqueue(r);
}


bool replay_wait_request(uint32_t seq, int timeout_ms)
{
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    while (1)
    {
        g_mutex_lock(&s_lock);
        bool done = (s_status.request_done >= seq);
        g_mutex_unlock(&s_lock);
        if (done) return true;
        if (g_get_monotonic_time() >= deadline) return false;
        g_usleep(2000);
    };
}


/** @brief Zapíše výsledek vyřízeného požadavku. */
static void replay_request_done(uint32_t seq, bool ok, const char *message)
{
    g_mutex_lock(&s_lock);
    s_status.request_done = seq;
    s_status.request_ok = ok;
    g_strlcpy(s_status.request_message, message ? message : "", sizeof(s_status.request_message));
    g_mutex_unlock(&s_lock);
    if (!ok) fprintf(stderr, "[REPLAY] request failed: %s\n", message ? message : "?");
}


/** @brief Vyzvedne další požadavek (nebo NULL) a při prázdné frontě shodí bit 1. */
static st_REPLAY_REQ *replay_dequeue(void)
{
    g_mutex_lock(&s_lock);
    st_REPLAY_REQ *r = (st_REPLAY_REQ *)g_queue_pop_head(&s_requests);
    if (g_queue_is_empty(&s_requests))
    {
        g_atomic_int_and((guint *)&g_replay_frame_hook, ~(guint)REPLAY_HOOK_REQUEST);
    };
    g_mutex_unlock(&s_lock);
    return r;
}


/** @brief true = ve frontě je požadavek (bez vyzvednutí). */
static bool replay_have_request(void)
{
    return (g_atomic_int_get(&g_replay_frame_hook) & REPLAY_HOOK_REQUEST) != 0;
}


/** @brief true = první čekající požadavek je STOP. */
static bool replay_head_is_stop(void)
{
    g_mutex_lock(&s_lock);
    st_REPLAY_REQ *r = (st_REPLAY_REQ *)g_queue_peek_head(&s_requests);
    bool stop = r && r->kind == REPLAY_REQ_STOP;
    g_mutex_unlock(&s_lock);
    return stop;
}


/* ================================================================
 * Společné operace se stavem stroje (emu vlákno)
 * ================================================================ */

/**
 * @brief Zámek životnosti s_autotype_buf vůči čtení z UI (replay_autotype_peek_rest()).
 *
 * Emu vlákno ho drží jen při výměně textu, UI při kopii zbytku.
 */
static GMutex s_autotype_lock;


/** @brief Spustí (text neprázdný) nebo zastaví ("" / NULL) autotype s kopií textu. */
static void replay_apply_autotype(const char *text, uint32_t kd_ms, uint32_t ku_ms)
{
    if (!text || !text[0])
    {
        pio8255_set_autotype(NULL);
        return;
    };
    /* Předchozí text může autotype ještě číst - nahradit až po přepnutí. */
    char *copy = g_strdup(text);
    g_mutex_lock(&s_autotype_lock);
    char *old = s_autotype_buf;
    s_autotype_buf = copy;
    pio8255_autotype_set_key_down_ms((double)kd_ms);
    pio8255_autotype_set_key_up_ms((double)ku_ms);
    pio8255_set_autotype(s_autotype_buf);
    g_mutex_unlock(&s_autotype_lock);
    g_free(old);
}


bool replay_autotype_peek_rest(char *buf, size_t size)
{
    if (!buf || size == 0) return false;
    buf[0] = '\0';
    int gen, done_gen, typed;
    g_mutex_lock(&s_autotype_lock);
    pio8255_autotype_progress(&gen, &done_gen, &typed);
    bool running = (gen != done_gen) && (s_autotype_buf != NULL);
    if (running)
    {
        size_t len = strlen(s_autotype_buf);
        size_t off = (typed < 0) ? 0 : (size_t)typed;
        if (off > len) off = len;
        g_strlcpy(buf, s_autotype_buf + off, size);
    };
    g_mutex_unlock(&s_autotype_lock);
    return running;
}


/**
 * @brief Komponenty snapshotu, ze kterých se počítá část `periph`.
 *
 * Jen stav čipů a řadičů, který načtení snapshotu obnoví celý; pole
 * řízení emulátoru a průběhu vykreslení (plánovaná událost `mzarch`, stav
 * renderování `gdg`) handlery při hashi vynechají (`for_hash`, viz
 * snapshot_hash_components()): bez toho se záznam s pauzou po run(frames)
 * rozjel na snímku pauzy a na závěrečném hashi (změřeno). Vynechané
 * komponenty: `ide8` (cesta k obrazu, při přehrání dočasná kopie, načtení ji
 * neobnoví), `unicard` (pracovní adresář hostitele), `cmt` (jméno souboru
 * pásky), paměti a CPU a zvuk, video a obraz (stav výstupu hostitele, ne
 * stroje). `fdc` a `qdisk` cestu k obrazu zapisují, načtení snapshotu ji
 * ale obnoví, takže se při přehrání shoduje.
 *
 * Známá mezera (N68): části `ram` a `memext` (state_hash.h) hashují jen
 * obsah RAM, ne mapování. Komponenty `memory` (mapa `g_memory.map`),
 * `memext` (mapa a konfigurace MemExt), `ramdisk` a `hwconfig` nepokrývá
 * žádná část, takže rozjetí jen v bankování se pozná až podle dalšího
 * průběhu. Seznam se kvůli ověření starších záznamů `.mzr` nemění; otisk
 * stroje (machine_hash.h) ho sdílí a mapování pokrývá vlastní částí
 * `memmap`.
 */
static const char *const s_periph_components[] = {
    "mzarch", "ctc8253", "pio8255", "pioz80", "psg", "gdg", "vramctrl", "fdc", "qdisk",
};


const char *const *replay_periph_components(size_t *out_count)
{
    *out_count = G_N_ELEMENTS(s_periph_components);
    return s_periph_components;
}


/** @brief Hash všech částí do řádku (chybějící část = ""). */
static void replay_compute_hash(st_REPLAY_HASH_ROW *row, uint32_t frame)
{
    /* Části ze state_hash (pořadí en_REPLAY_HASH_PART od CPU po PCG). */
    static const en_STATE_HASH_PART map[REPLAY_HASH_PCG + 1] = {
        [REPLAY_HASH_CPU] = STATE_HASH_CPU,
        [REPLAY_HASH_RAM] = STATE_HASH_RAM,
        [REPLAY_HASH_MEMEXT] = STATE_HASH_MEMEXT,
        [REPLAY_HASH_VRAM] = STATE_HASH_VRAM,
        [REPLAY_HASH_PCG] = STATE_HASH_PCG,
    };
    memset(row, 0, sizeof(*row));
    row->frame = frame;
    row->ticks = gdg_get_total_ticks();
    for (int p = 0; p <= REPLAY_HASH_PCG; p++)
    {
        if (!state_hash_part(map[p], row->part[p])) row->part[p][0] = '\0';
    };
    if (snapshot_hash_components(s_periph_components, G_N_ELEMENTS(s_periph_components),
                                 row->part[REPLAY_HASH_PERIPH], sizeof(row->part[REPLAY_HASH_PERIPH])) != SNAPSHOT_OK)
    {
        row->part[REPLAY_HASH_PERIPH][0] = '\0';
    };
}


/**
 * @brief Začátek / konec záznamu nebo přehrání pro joystick.
 *
 * Při aktivním replay čte joy_read_byte() port typu JOYSTICK ze
 * `g_joy.dev[].state` (joy_host_latch()), který záznam jednou za snímek
 * naplní ze SDL a přehrání z událostí.
 */
static void replay_joy_latch(bool on)
{
#ifdef HAVE_JOY
    joy_host_latch(on);
#else
    (void)on;
#endif
}


/** @brief Záznam: stav portů typu JOYSTICK ze SDL (jednou za snímek, krok 2 B_f). */
static void replay_rec_sample_host_joystick(void)
{
#ifdef HAVE_JOY
    for (int d = 0; d < JOY_DEVID_COUNT; d++)
    {
        if (g_joy.dev[d].type == JOY_TYPE_JOYSTICK) g_joy.dev[d].state = joy_host_scan((en_JOY_DEVID)d);
    };
#endif
}


/** @brief Aktuální výsledná matice klávesnice (`keyboard_matrix & vkbd_matrix`). */
static void replay_effective_kbd(uint8_t *out)
{
    for (int i = 0; i < REPLAY_KBD_COLS; i++)
    {
        out[i] = g_pio8255.keyboard_matrix[i] & g_pio8255.vkbd_matrix[i];
    };
}


/* ================================================================
 * Checkpointy (emu vlákno)
 * ================================================================ */

/**
 * @brief Založí buffer checkpointů podle požadavku (záporné hodnoty z INI).
 * @post `s_ckpt` je prázdný a inicializovaný, `s_ckpt_interval` nastavený.
 */
static void replay_ckpt_begin(const st_REPLAY_CKPT_CFG *cfg)
{
    replay_ckpt_ring_free(&s_ckpt);
    int interval = (cfg && cfg->interval >= 0) ? cfg->interval : (int)s_cfg_ckpt_interval;
    int count = (cfg && cfg->count >= 0) ? cfg->count : (int)s_cfg_ckpt_count;
    s_ckpt_interval = (uint32_t)(interval > 0 ? interval : 0);
    replay_ckpt_ring_init(&s_ckpt, (guint)(count > 0 ? count : 1));
}


/** @brief Uvolní checkpointy (konec záznamu / přehrávání). */
static void replay_ckpt_end(void)
{
    replay_ckpt_ring_free(&s_ckpt);
    s_ckpt_interval = 0;
}


/** @brief Číslo aktuálního bloku vložené pásky, -1 = žádná páska nebo blok. */
static int replay_tape_block(void)
{
    if (!CMT_TEST_FILLED || !g_cmt.ext->block) return -1;
    return cmtext_block_get_block_id(g_cmt.ext->block);
}


/**
 * @brief Uloží checkpoint aktuálního stavu (bod B_f nebo start v pauze).
 *
 * Vynechá ho, když jsou checkpointy vypnuté, běží autotype (jeho průběh
 * snapshot nenese) nebo se nahrává na kazetu (rozpracované nahrávání
 * vrátit nejde). Vynechá ho i při přehrávání bez odchýlení, když checkpoint
 * téhož snímku už existuje (po seeku zpět): přehrávání je deterministické,
 * nový by byl shodný a jeho uložení by smazalo novější checkpointy (N82).
 * Snapshot je i s RAMdiskem jako start záznamu; za běhu emulace se ukládá
 * v snapshot safe-pointu (emu vlákno mezi instrukcemi).
 */
static void replay_ckpt_take_ex(bool keep_newer);

static void replay_ckpt_take(void)
{
    replay_ckpt_take_ex(false);
}


/**
 * @brief Uloží checkpoint; @p keep_newer = vložit podle snímku a novější
 *        ponechat (replay_ckpt_ring_insert(), goto při přehrávání).
 *
 * Podmínky vynechání jako replay_ckpt_take().
 */
static void replay_ckpt_take_ex(bool keep_newer)
{
    if (!s_ckpt.capacity || !s_ckpt_interval) return;
    if (g_pio8255.vkbd_autotype || CMT_TEST_RECORD) return;
    if (s_play && !s_deviated && replay_ckpt_ring_has_frame(&s_ckpt, replay_current_frame())) return;

    bool saved_ramdisk = g_snapshot_settings.include_ramdisk;
    bool saved_sp = g_emulator.snapshot_safepoint;
    g_snapshot_settings.include_ramdisk = true;
    g_emulator.snapshot_safepoint = true;
    uint8_t *snap = NULL;
    size_t size = 0;
    en_SNAPSHOT_RESULT sr = snapshot_save_to_buffer("replay checkpoint", &snap, &size);
    g_emulator.snapshot_safepoint = saved_sp;
    g_snapshot_settings.include_ramdisk = saved_ramdisk;
    if (sr != SNAPSHOT_OK)
    {
        fprintf(stderr, "[REPLAY] checkpoint at frame %u failed: %s\n", replay_current_frame(),
                snapshot_result_to_string(sr));
        return;
    };

    st_REPLAY_CKPT c;
    memset(&c, 0, sizeof(c));
    c.frame = replay_current_frame();
    c.ticks = gdg_get_total_ticks();
    c.snap = snap;
    c.snap_size = size;
    c.media_writes = g_media_writes;
    c.tape_block = replay_tape_block();
    if (s_rec)
    {
        c.events_len = s_rec->events->len;
        c.hashes_len = s_rec->hashes->len;
        c.media_len = s_rec->media->len;
        c.cmthack_count = s_cmthack_count;
        memcpy(c.last_kbd, s_last_kbd, sizeof(c.last_kbd));
        memcpy(c.last_joy, s_last_joy, sizeof(c.last_joy));
    }
    else if (s_play)
    {
        c.ev_cursor = s_ev_cursor;
        c.hash_cursor = s_hash_cursor;
        c.cmthack_cursor = s_cmthack_cursor;
        c.record_count = s_record_count;
        memcpy(c.play_kbd, s_play_kbd, sizeof(c.play_kbd));
        memcpy(c.play_joy, s_play_joy, sizeof(c.play_joy));
        c.deviated = s_deviated;
    };
    if (keep_newer) replay_ckpt_ring_insert(&s_ckpt, &c);
    else replay_ckpt_ring_push(&s_ckpt, &c);
}


/** @brief Bod B_f: checkpoint, je-li tento snímek na řadě. */
static void replay_ckpt_on_boundary(void)
{
    uint32_t f = replay_current_frame();
    if (s_ckpt_interval && f > 0 && (f % s_ckpt_interval) == 0) replay_ckpt_take();
}


/**
 * @brief Načte stav stroje z checkpointu (paused smyčka).
 *
 * Blok pásky se otevře před snapshotem (jako replay_media_prepare());
 * snapshot nenese průběh autotype ani matici VKBD, ty se vynulují (checkpoint
 * vzniká jen bez běžícího autotype).
 *
 * @return NULL = načteno, jinak chyba anglicky (stav stroje pak nemusí
 *         odpovídat žádnému snímku).
 * @post `s_frame` = snímek checkpointu, emulace stojí na hranici snímku.
 */
static const char *replay_ckpt_load(const st_REPLAY_CKPT *c, char *errbuf, size_t errsize)
{
    if (c->tape_block >= 0 && CMT_TEST_FILLED && replay_tape_block() != c->tape_block)
    {
        st_CMTEXT_CONTAINER *ct = cmtext_get_container(g_cmt.ext);
        if (!ct || !ct->cb_open_block || ct->cb_open_block(c->tape_block) != EXIT_SUCCESS)
        {
            return "cannot reopen the tape block of the checkpoint";
        };
    };
    en_SNAPSHOT_RESULT sr = snapshot_load_from_buffer(c->snap, c->snap_size);
    if (sr != SNAPSHOT_OK)
    {
        g_snprintf(errbuf, errsize, "checkpoint load failed: %s", snapshot_result_to_string(sr));
        return errbuf;
    };
    pio8255_set_autotype(NULL);
    memset(g_pio8255.vkbd_matrix, 0xFF, sizeof(g_pio8255.vkbd_matrix));
    s_frame = c->frame;
    s_boundary_ticks = gdg_get_total_ticks();
    return NULL;
}


/** @brief Text odmítnutí podle výsledku hledání checkpointu. */
static const char *replay_ckpt_refusal(en_REPLAY_CKPT_FIND r, guint idx, uint32_t max_frame, char *errbuf,
                                       size_t errsize)
{
    if (r == REPLAY_CKPT_MEDIA_WRITTEN)
    {
        g_snprintf(errbuf, errsize, "a disk or the tape was written or ejected after frame %u; "
                   "rewinding past a media change is not possible",
                   replay_ckpt_ring_get(&s_ckpt, idx)->frame);
        return errbuf;
    };
    if (!s_ckpt_interval) return "checkpoints are off";
    if (!s_ckpt.count)
    {
        g_snprintf(errbuf, errsize, "no checkpoint at or before frame %u", max_frame);
        return errbuf;
    };
    g_snprintf(errbuf, errsize, "no checkpoint at or before frame %u (the oldest kept is frame %u)", max_frame,
               replay_ckpt_ring_get(&s_ckpt, 0)->frame);
    return errbuf;
}


/* ================================================================
 * Záznam (emu vlákno)
 * ================================================================ */

/** @brief Připojí událost do záznamu s aktuálním snímkem. */
static void replay_rec_log(st_REPLAY_EVENT *ev, en_REPLAY_EV_AT at)
{
    ev->frame = replay_current_frame();
    ev->at = at;
    replay_data_add_event(s_rec, ev);
}


/**
 * @brief Vzorek vstupů: změněnou matici a stavy joysticků zapíše jako události.
 * @param at    Poloha událostí.
 * @param force true = zapsat i beze změny (start záznamu).
 */
static void replay_rec_sample(en_REPLAY_EV_AT at, bool force)
{
    st_REPLAY_EVENT ev;
    uint8_t m[REPLAY_KBD_COLS];
    replay_effective_kbd(m);
    if (force || memcmp(m, s_last_kbd, sizeof(m)) != 0)
    {
        memset(&ev, 0, sizeof(ev));
        ev.kind = REPLAY_EV_KBD;
        memcpy(ev.kbd, m, sizeof(m));
        replay_rec_log(&ev, at);
        memcpy(s_last_kbd, m, sizeof(m));
    };
#ifdef HAVE_JOY
    for (int d = 0; d < REPLAY_JOY_PORTS && d < JOY_DEVID_COUNT; d++)
    {
        uint8_t s = g_joy.dev[d].state;
        if (force || s != s_last_joy[d])
        {
            memset(&ev, 0, sizeof(ev));
            ev.kind = REPLAY_EV_JOY;
            ev.joy_port = (uint8_t)d;
            ev.joy_state = s;
            replay_rec_log(&ev, at);
            s_last_joy[d] = s;
        };
    };
#endif
}


/** @brief Hash do záznamu; řádek téhož snímku nahradí. */
static void replay_rec_hash(void)
{
    st_REPLAY_HASH_ROW row;
    replay_compute_hash(&row, replay_current_frame());
    GArray *h = s_rec->hashes;
    if (h->len && g_array_index(h, st_REPLAY_HASH_ROW, h->len - 1).frame == row.frame)
    {
        g_array_index(h, st_REPLAY_HASH_ROW, h->len - 1) = row;
    }
    else
    {
        replay_data_add_hash(s_rec, &row);
    };
}


/** @brief Provede reset a zapíše ho do záznamu. */
static void replay_rec_reset(en_REPLAY_EV_AT at)
{
    mzarch_main_reset_now();
    st_REPLAY_EVENT ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = REPLAY_EV_RESET;
    replay_rec_log(&ev, at);
}


/** @brief Spustí autotype a zapíše ho do záznamu (text převezme). */
static void replay_rec_autotype(char *text, uint32_t kd_ms, uint32_t ku_ms, en_REPLAY_EV_AT at)
{
    replay_apply_autotype(text, kd_ms, ku_ms);
    st_REPLAY_EVENT ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = REPLAY_EV_AUTOTYPE;
    ev.text = text ? text : (char *)"";
    ev.kd_ms = kd_ms;
    ev.ku_ms = ku_ms;
    replay_rec_log(&ev, at);
    g_free(text);
}


/** @brief Provede operaci kazety a při záznamu ji zapíše jako událost `cmt`. */
static void replay_cmt_run(en_REPLAY_CMT_OP op, int32_t arg, const char *path, en_REPLAY_EV_AT at)
{
    (void)replay_media_cmt_execute(op, arg, path);
    /* Revize stavu (state_rev.h, Z9): operace kazety z UI nebo záznamu. */
    STATE_REV_BUMP(STATE_REV_SRC_TAPE);
    if (!s_rec) return;
    st_REPLAY_EVENT ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = REPLAY_EV_CMT;
    ev.cmd = (char *)replay_cmt_op_name(op);
    ev.arg = arg;
    ev.text = (char *)path;
    replay_rec_log(&ev, at);
}


/**
 * @brief Nastaví nebo shodí bit REPLAY_HOOK_UI podle čekajících vstupů z UI.
 *
 * Bit drží, dokud čeká operace kazety, autotype, nové prodlevy autotype,
 * změna okna VKBD nebo sken klávesnice hostitele mimo záznam; hlavní
 * smyčka pak volá replay_on_frame_boundary() i bez záznamu.
 *
 * @pre Volající drží s_lock.
 */
static void replay_ui_hook_refresh_locked(void)
{
    bool any = !g_queue_is_empty(&s_ui_cmt) || s_pending_autotype || s_pending_delays || s_off_host_pending;
    for (int i = 0; i < REPLAY_KBD_COLS && !any; i++)
    {
        any = (s_off_vkbd_press[i] | s_off_vkbd_release[i]) != 0;
    };
    if (any) g_atomic_int_or((guint *)&g_replay_frame_hook, REPLAY_HOOK_UI);
    else g_atomic_int_and((guint *)&g_replay_frame_hook, ~(guint)REPLAY_HOOK_UI);
}


/** @brief Zahodí čekající prodlevy autotype, změny VKBD a sken klávesnice hostitele mimo záznam. @pre Drží s_lock. */
static void replay_ui_off_clear_locked(void)
{
    s_pending_delays = false;
    s_off_host_pending = false;
    memset(s_off_vkbd_press, 0, sizeof(s_off_vkbd_press));
    memset(s_off_vkbd_release, 0, sizeof(s_off_vkbd_release));
}


/**
 * @brief Provede (nebo zahodí) vstupy z UI čekající mimo záznam.
 *
 * Autotype, nové prodlevy autotype, změny okna VKBD a sken klávesnice
 * hostitele, které UI zařadilo mimo záznam (replay_host_autotype(),
 * replay_host_autotype_delays(), replay_host_vkbd_set(),
 * replay_host_keyboard_submit()). Emu vlákno je tak mění samo: UI vlákno
 * dřív zapisovalo ukazatel autotype, matici VKBD a matici klávesnice
 * souběžně s jejich čtením. Revizi stavu zvedne jen skutečná změna (sken
 * klávesnice se stejnou maticí a stejným joystickem nic nemění).
 *
 * @param apply true = provést; false = zahodit (přehrávání).
 * @pre Emu vlákno, neběží záznam.
 */
static void replay_ui_off_drain(bool apply)
{
    uint8_t press[REPLAY_KBD_COLS], release[REPLAY_KBD_COLS];
    uint8_t host[REPLAY_KBD_COLS];
    g_mutex_lock(&s_lock);
    bool do_host = s_off_host_pending;
    memcpy(host, s_off_host_kbd, sizeof(host));
    int host_joy_dev = s_off_host_joy_dev;
    uint8_t host_joy_state = s_off_host_joy_state;
    bool do_at = s_pending_autotype;
    char *at_text = s_pending_autotype_text;
    uint32_t kd = s_pending_kd_ms, ku = s_pending_ku_ms;
    s_pending_autotype = false;
    s_pending_autotype_text = NULL;
    bool do_delays = s_pending_delays;
    uint32_t dkd = s_pending_delay_kd_ms, dku = s_pending_delay_ku_ms;
    memcpy(press, s_off_vkbd_press, sizeof(press));
    memcpy(release, s_off_vkbd_release, sizeof(release));
    replay_ui_off_clear_locked();
    replay_ui_hook_refresh_locked();
    g_mutex_unlock(&s_lock);

    if (apply)
    {
        /* Revize stavu (state_rev.h, Z9): vstupy z UI mimo záznam, jen při
         * skutečné změně (autotype, prodlevy, VKBD, klávesnice hostitele). */
        bool changed = do_at || do_delays;
        for (int i = 0; i < REPLAY_KBD_COLS && !changed; i++) changed = (press[i] | release[i]) != 0;
        if (do_host)
        {
            if (memcmp(g_pio8255.keyboard_matrix, host, sizeof(host)) != 0)
            {
                memcpy(g_pio8255.keyboard_matrix, host, sizeof(host));
                changed = true;
            };
#ifdef HAVE_JOY
            if (host_joy_dev >= 0 && host_joy_dev < JOY_DEVID_COUNT && g_joy.dev[host_joy_dev].state != host_joy_state)
            {
                g_joy.dev[host_joy_dev].state = host_joy_state;
                changed = true;
            };
#else
            (void)host_joy_dev;
            (void)host_joy_state;
#endif
        };
        if (changed) STATE_REV_BUMP(STATE_REV_SRC_INPUT);
        if (do_at) replay_apply_autotype(at_text, kd, ku);
        if (do_delays)
        {
            pio8255_autotype_set_key_down_ms((double)dkd);
            pio8255_autotype_set_key_up_ms((double)dku);
        };
        for (int i = 0; i < REPLAY_KBD_COLS; i++)
        {
            g_pio8255.vkbd_matrix[i] = (uint8_t)((g_pio8255.vkbd_matrix[i] & ~press[i]) | release[i]);
        };
    };
    g_free(at_text);
}


/**
 * @brief Vyřídí operace kazety z UI.
 *
 * Mimo záznam je hned provede (tak jako dřív UI vlákno), při záznamu je
 * zapíše s polohou @p at, při přehrávání je zahodí.
 *
 * @param at Poloha (B_f = `b`, pauza na hranici = `p`).
 */
static void replay_ui_cmt_drain(en_REPLAY_EV_AT at)
{
    while (1)
    {
        g_mutex_lock(&s_lock);
        st_REPLAY_UI_CMT *u = (st_REPLAY_UI_CMT *)g_queue_pop_head(&s_ui_cmt);
        replay_ui_hook_refresh_locked();
        g_mutex_unlock(&s_lock);
        if (!u) break;
        if (!s_play) replay_cmt_run(u->op, u->arg, u->path, at);
        g_free(u->path);
        g_free(u);
    };
}


/** @brief true = z UI čeká operace kazety, nebo vstup mimo záznam (bit REPLAY_HOOK_UI). */
static bool replay_ui_cmt_pending(void)
{
    return (g_atomic_int_get(&g_replay_frame_hook) & REPLAY_HOOK_UI) != 0;
}


/** @brief Bod B_f při záznamu (kroky 1-5 z tabulky v hlavičce). */
static void replay_rec_boundary(void)
{
    g_mutex_lock(&s_lock);
    /* Sken klávesnice hostitele zařazený mimo záznam těsně kolem startu
     * záznamu (UI četlo režim před startem): je novější než s_host_kbd,
     * převezme se tady, jinak by čekal do konce záznamu a pak se zapsal
     * zastaralý. */
    if (s_off_host_pending)
    {
        memcpy(s_host_kbd, s_off_host_kbd, sizeof(s_host_kbd));
        s_host_joy_dev = s_off_host_joy_dev;
        s_host_joy_state = s_off_host_joy_state;
        s_off_host_pending = false;
        replay_ui_hook_refresh_locked();
    };
    bool do_reset = s_pending_reset;
    s_pending_reset = false;
    uint8_t host[REPLAY_KBD_COLS];
    for (int i = 0; i < REPLAY_KBD_COLS; i++) host[i] = s_host_kbd[i] & s_ui_vkbd[i];
    int joy_dev = s_host_joy_dev;
    uint8_t joy_state = s_host_joy_state;
    bool do_at = s_pending_autotype;
    char *at_text = s_pending_autotype_text;
    uint32_t kd = s_pending_kd_ms, ku = s_pending_ku_ms;
    s_pending_autotype = false;
    s_pending_autotype_text = NULL;
    g_mutex_unlock(&s_lock);

    if (do_reset) replay_rec_reset(REPLAY_AT_BOUNDARY);

    memcpy(g_pio8255.keyboard_matrix, host, sizeof(host));
#ifdef HAVE_JOY
    if (joy_dev >= 0 && joy_dev < JOY_DEVID_COUNT) g_joy.dev[joy_dev].state = joy_state;
#else
    (void)joy_dev;
    (void)joy_state;
#endif
    replay_rec_sample_host_joystick();

    if (do_at) replay_rec_autotype(at_text, kd, ku, REPLAY_AT_BOUNDARY);
    if (replay_ui_cmt_pending()) replay_ui_cmt_drain(REPLAY_AT_BOUNDARY);

    replay_rec_sample(REPLAY_AT_BOUNDARY, false);

    if ((replay_current_frame() % s_rec->hash_interval) == 0) replay_rec_hash();
}


/** @brief V pauze na hranici: čekající reset a autotype jako události `p`. */
static void replay_rec_paused(void)
{
    if (!replay_paused_at_boundary()) return;
    g_mutex_lock(&s_lock);
    bool do_reset = s_pending_reset;
    s_pending_reset = false;
    bool do_at = s_pending_autotype;
    char *at_text = s_pending_autotype_text;
    uint32_t kd = s_pending_kd_ms, ku = s_pending_ku_ms;
    s_pending_autotype = false;
    s_pending_autotype_text = NULL;
    g_mutex_unlock(&s_lock);
    if (do_reset)
    {
        replay_rec_reset(REPLAY_AT_PAUSE);
        replay_rec_sample(REPLAY_AT_PAUSE, false);
    };
    if (do_at)
    {
        /* Revize stavu (state_rev.h, Z8): autotype při záznamu v pauze. */
        STATE_REV_BUMP(STATE_REV_SRC_INPUT);
        replay_rec_autotype(at_text, kd, ku, REPLAY_AT_PAUSE);
    };
    if (replay_ui_cmt_pending()) replay_ui_cmt_drain(REPLAY_AT_PAUSE);
}


/** @brief Start záznamu (paused smyčka). @return NULL = v pořádku, jinak chyba anglicky. */
static const char *replay_rec_start(const st_REPLAY_REQ *r, char *errbuf, size_t errsize)
{
    if (!r->path || !r->path[0]) return "no file name";
    if (g_pio8255.vkbd_autotype) return "autotype is running; wait until it ends";
    if (CMT_TEST_RECORD) return "the tape is recording; stop the tape recording first";
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (freeze_count() > 0) return "Freeze Bytes are active; remove them first";
#endif

    /* Kompletní snapshot vždy s RAMdiskem (rozhodnutí 7), pak okamžité
     * načtení: záznam i přehrání vychází ze stejného načteného stavu. */
    bool saved_ramdisk = g_snapshot_settings.include_ramdisk;
    g_snapshot_settings.include_ramdisk = true;
    uint8_t *snap = NULL;
    size_t snap_size = 0;
    en_SNAPSHOT_RESULT sr = snapshot_save_to_buffer("replay start", &snap, &snap_size);
    g_snapshot_settings.include_ramdisk = saved_ramdisk;
    if (sr != SNAPSHOT_OK)
    {
        g_snprintf(errbuf, errsize, "start snapshot failed: %s", snapshot_result_to_string(sr));
        return errbuf;
    };
    sr = snapshot_load_from_buffer(snap, snap_size);
    if (sr != SNAPSHOT_OK)
    {
        g_free(snap);
        g_snprintf(errbuf, errsize, "start snapshot reload failed: %s", snapshot_result_to_string(sr));
        return errbuf;
    };

    s_rec = replay_data_new();
    s_rec->emulator_version = g_strdup(EMULATOR_VERSION);
    s_rec->platform = g_strdup(MZARCH_NAME);
    s_rec->screen_ticks = VIDEO_SCREEN_TICKS;
    GDateTime *now = g_date_time_new_now_local();
    s_rec->created = g_date_time_format_iso8601(now);
    s_rec->rtc_base = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");
    g_date_time_unref(now);
    replay_rtc_set_base(s_rec->rtc_base);
    if (UNICARD_TEST_IS_CONNECTED)
    {
        replay_data_add_warning(s_rec, "the Unicard SD card is a host directory and is not part of the replay; "
                                       "play it back with the same SD card contents");
    };
    s_rec->description = g_strdup(r->description);
    s_rec->hash_interval = r->hash_interval ? r->hash_interval : REPLAY_DEFAULT_HASH_INTERVAL;
    s_rec->start_snapshot = snap;
    s_rec->start_snapshot_size = snap_size;
    replay_media_capture(s_rec);
    s_cmthack_count = 0;

#ifdef HAVE_JOY
    for (int d = 0; d < JOY_DEVID_COUNT && d < REPLAY_JOY_PORTS; d++) s_rec->joy_type[d] = (int)g_joy.dev[d].type;
#endif

    s_start_screens = (uint64_t)g_gdg.total_elapsed.screens;
    s_frame = 0;
    s_boundary_ticks = gdg_get_total_ticks();
    s_start_ticks = s_boundary_ticks;

    g_mutex_lock(&s_lock);
    memcpy(s_host_kbd, g_pio8255.keyboard_matrix, sizeof(s_host_kbd));
    /* Sken klávesnice hostitele, který ještě čeká mimo záznam, je
     * novější než matice: záznam ho převezme v prvním bodě B_f. */
    if (s_off_host_pending)
    {
        memcpy(s_host_kbd, s_off_host_kbd, sizeof(s_host_kbd));
        s_host_joy_dev = s_off_host_joy_dev;
        s_host_joy_state = s_off_host_joy_state;
    };
    memset(s_ui_vkbd, 0xFF, sizeof(s_ui_vkbd));
    s_pending_reset = false;
    s_pending_autotype = false;
    g_free(s_pending_autotype_text);
    s_pending_autotype_text = NULL;
    /* Vstupy z UI čekající mimo záznam do záznamu nepatří. */
    replay_ui_off_clear_locked();
    replay_ui_hook_refresh_locked();
    s_status.outcome = REPLAY_OUTCOME_NONE;
    s_status.outcome_message[0] = '\0';
    s_status.desync_parts[0] = '\0';
    s_status.deviation[0] = '\0';
    g_strlcpy(s_status.path, r->path, sizeof(s_status.path));
    g_mutex_unlock(&s_lock);

    replay_set_mode(REPLAY_MODE_RECORDING);
    replay_joy_latch(true);
    replay_rec_sample_host_joystick();
    replay_rec_sample(REPLAY_AT_BOUNDARY, true);
    replay_rec_hash();
    replay_ckpt_begin(&r->ckpt);
    replay_ckpt_take();
    replay_publish();
    fprintf(stderr, "[REPLAY] recording to '%s' (hash interval %u frames)\n", r->path, s_rec->hash_interval);
    return NULL;
}


/**
 * @brief Konec záznamu: závěrečný hash (jen na hranici), zápis souboru.
 * @param at_boundary true = stav je na hranici snímku (B_f nebo pauza na hranici).
 * @return NULL = uloženo, jinak chyba anglicky.
 */
static const char *replay_rec_stop(bool at_boundary, char *errbuf, size_t errsize)
{
    s_rec->frames = replay_current_frame();
    if (at_boundary)
    {
        replay_rec_hash();
        s_rec->final_hash = true;
    };
    char path[REPLAY_STATUS_PATH_MAX];
    g_mutex_lock(&s_lock);
    g_strlcpy(path, s_status.path, sizeof(path));
    g_mutex_unlock(&s_lock);

    char *err = NULL;
    bool ok = replay_data_write(s_rec, path, &err);
    uint32_t frames = s_rec->frames;
    guint events = s_rec->events->len;
    replay_data_free(s_rec);
    s_rec = NULL;
    replay_ckpt_end();
    replay_joy_latch(false);
    replay_set_mode(REPLAY_MODE_OFF);
    replay_publish();
    if (!ok)
    {
        g_snprintf(errbuf, errsize, "cannot save the replay: %s", err ? err : "?");
        g_free(err);
        replay_publish_outcome(REPLAY_OUTCOME_ERROR, frames, errbuf, NULL);
        return errbuf;
    };
    char msg[REPLAY_STATUS_TEXT_MAX];
    g_snprintf(msg, sizeof(msg), "%u frames, %u events, saved to '%s'", frames, events, path);
    replay_publish_outcome(REPLAY_OUTCOME_RECORDED, frames, msg, NULL);
    return NULL;
}


/**
 * @brief Rewind při záznamu (paused smyčka): checkpoint, zkrácení záznamu.
 *
 * @param frames 0 = nejnovější checkpoint se snímkem menším než aktuální;
 *               jinak nejnovější aspoň o @p frames snímků zpět.
 * @return NULL = hotovo (emulace stojí na hranici snímku checkpointu),
 *         jinak důvod odmítnutí anglicky (záznam beze změny).
 */
static const char *replay_rec_rewind(uint32_t frames, char *errbuf, size_t errsize)
{
    uint32_t cur = replay_current_frame();
    if (frames == 0 && cur == 0) return "already at the start of the recording";
    uint32_t max_frame = (frames == 0) ? cur - 1 : (cur > frames ? cur - frames : 0);
    guint idx = 0;
    en_REPLAY_CKPT_FIND fr = replay_ckpt_ring_find(&s_ckpt, max_frame, g_media_writes, &idx);
    if (fr != REPLAY_CKPT_FOUND) return replay_ckpt_refusal(fr, idx, max_frame, errbuf, errsize);

    const st_REPLAY_CKPT *c = replay_ckpt_ring_get(&s_ckpt, idx);
    const char *e = replay_ckpt_load(c, errbuf, errsize);
    if (e) return e;
    replay_data_truncate(s_rec, c->events_len, c->hashes_len, c->media_len);
    s_cmthack_count = c->cmthack_count;
    memcpy(s_last_kbd, c->last_kbd, sizeof(s_last_kbd));
    memcpy(s_last_joy, c->last_joy, sizeof(s_last_joy));
    s_rec->rerecord_count++;
    uint32_t frame = c->frame;
    replay_ckpt_ring_truncate_after(&s_ckpt, idx);

    /* Čekající vstupy z UI patřily zahozené části. */
    g_mutex_lock(&s_lock);
    s_pending_reset = false;
    s_pending_autotype = false;
    g_free(s_pending_autotype_text);
    s_pending_autotype_text = NULL;
    g_mutex_unlock(&s_lock);

    replay_publish();
    fprintf(stderr, "[REPLAY] rewound the recording to frame %u (rerecord %u)\n", frame, s_rec->rerecord_count);
    return NULL;
}


/* ================================================================
 * Přehrávání (emu vlákno)
 * ================================================================ */

/** @brief Přejde do stavu odchýleno (jen poprvé). */
static void replay_deviate(const char *what)
{
    if (!s_play || s_deviated) return;
    s_deviated = true;
    g_mutex_lock(&s_lock);
    g_strlcpy(s_status.deviation, what ? what : "", sizeof(s_status.deviation));
    s_status.deviated = true;
    g_mutex_unlock(&s_lock);
    fprintf(stderr, "[REPLAY] playback deviated at frame %u: %s (hashes are no longer checked)\n",
            replay_current_frame(), what ? what : "?");
}


void replay_note_external_write(const char *what)
{
    if (s_rec)
    {
        char *w = g_strdup_printf("unrecorded write: %s", what ? what : "?");
        replay_data_add_warning(s_rec, w);
        g_free(w);
    }
    else if (s_play)
    {
        replay_deviate(what);
    };
}


/** @brief Zapíše vstupy přehrávače do stroje (živý vstup tím přepíše). */
static void replay_play_apply_inputs(void)
{
    memcpy(g_pio8255.keyboard_matrix, s_play_kbd, sizeof(s_play_kbd));
    memset(g_pio8255.vkbd_matrix, 0xFF, sizeof(g_pio8255.vkbd_matrix));
#ifdef HAVE_JOY
    for (int d = 0; d < REPLAY_JOY_PORTS && d < JOY_DEVID_COUNT; d++)
    {
        g_joy.dev[d].state = s_play_joy[d];
    };
#endif
}


/** @brief Provede jednu uloženou událost. */
static void replay_play_event(const st_REPLAY_EVENT *ev)
{
    switch (ev->kind)
    {
        case REPLAY_EV_KBD:
            memcpy(s_play_kbd, ev->kbd, sizeof(s_play_kbd));
            break;
        case REPLAY_EV_JOY:
            if (ev->joy_port < REPLAY_JOY_PORTS) s_play_joy[ev->joy_port] = ev->joy_state;
            break;
        case REPLAY_EV_AUTOTYPE:
            replay_apply_autotype(ev->text, ev->kd_ms, ev->ku_ms);
            break;
        case REPLAY_EV_RESET:
            mzarch_main_reset_now();
            break;
        case REPLAY_EV_CMT:
        {
            int op = replay_cmt_op_from_name(ev->cmd);
            if (op < 0)
            {
                replay_deviate("unknown tape operation in the replay");
                break;
            };
            /* Nahrávání na kazetu při přehrání do dočasného souboru, ne
             * do souboru uživatele. */
            char *tmp = NULL;
            if (op == REPLAY_CMT_RECORD)
            {
                char *name = g_strdup_printf("record%d.wav", ++s_record_count);
                tmp = replay_media_tmp_file(name);
                g_free(name);
            };
            (void)replay_media_cmt_execute((en_REPLAY_CMT_OP)op, ev->arg, tmp);
            g_free(tmp);
            break;
        };
        case REPLAY_EV_CMTHACK:
            /* Spotřebuje replay_cmthack_playback() při instrukci CMT hacku. */
            break;
        case REPLAY_EV_DBG:
        {
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            char *err = NULL;
            s_applying = true;
            bool ok = replay_dbg_execute(ev->cmd, ev->args_json, &err);
            s_applying = false;
            if (!ok)
            {
                char *w = g_strdup_printf("recorded %s failed: %s", ev->cmd ? ev->cmd : "?", err ? err : "?");
                replay_deviate(w);
                g_free(w);
            };
            g_free(err);
#endif
            break;
        };
        default:
            break;
    };
}


/** @brief Pořadí polohy v rámci snímku: d = 0, b = 1, p = 2. */
static int replay_at_order(en_REPLAY_EV_AT at)
{
    switch (at)
    {
        case REPLAY_AT_DRAIN: return 0;
        case REPLAY_AT_BOUNDARY: return 1;
        case REPLAY_AT_PAUSE:
        default: break;
    };
    return 2;
}


/**
 * @brief Provede události aktuálního snímku až do dané polohy včetně.
 *
 * Zároveň dožene starší neprovedené události (snímek < aktuální, nebo
 * dřívější poloha téhož snímku), aby se žádná neztratila.
 */
static void replay_play_events_at(en_REPLAY_EV_AT at)
{
    uint32_t f = replay_current_frame();
    while (s_play && s_ev_cursor < s_play->events->len)
    {
        const st_REPLAY_EVENT *ev = &g_array_index(s_play->events, st_REPLAY_EVENT, s_ev_cursor);
        if (ev->frame > f) break;
        /* Pořadí v rámci snímku: d, b, p. Událost pozdější polohy počká. */
        if (ev->frame == f && replay_at_order(ev->at) > replay_at_order(at)) break;
        s_ev_cursor++;
        replay_play_event(ev);
        if (!s_play) break;
    };
}


/** @brief Konec přehrávání: výsledek, pauza nebo žádost o konec procesu. */
/**
 * @brief Konec dohrávání seeku: vrátí rychlost před seekem.
 * @param pause true = zastavit na cíli (pauza s důvodem replay_seek).
 */
static void replay_seek_done(bool pause)
{
    if (!s_seeking) return;
    s_seeking = false;
    if (!s_seek_saved_max) emulator_max_speed(false);
    /* Hrající páska s cpu_boost si MAX SPEED drží sama. */
    cmt_cpu_boost_apply();
    if (pause)
    {
        g_emulator.pause_reason = EMU_PAUSE_REASON_REPLAY_SEEK;
        emulator_pause(true);
        fprintf(stderr, "[REPLAY] seek reached frame %u\n", replay_current_frame());
    };
}


/** @brief Začátek dohrávání na cíl seeku v MAX SPEED. */
static void replay_seek_begin(uint32_t target)
{
    if (!s_seeking) s_seek_saved_max = g_emulator.max_speed;
    s_seeking = true;
    s_seek_target = target;
    if (!g_emulator.max_speed) emulator_max_speed(true);
}


static void replay_play_finish(en_REPLAY_OUTCOME outcome, const char *message, const char *parts)
{
    uint32_t f = replay_current_frame();
    replay_seek_done(false);
    replay_goto_finish();
    replay_data_free(s_play);
    s_play = NULL;
    replay_ckpt_end();
    replay_media_restore();
    replay_joy_latch(false);
    replay_set_mode(REPLAY_MODE_OFF);
    replay_publish();
    replay_publish_outcome(outcome, f, message, parts);

    if (outcome == REPLAY_OUTCOME_STOPPED) return;
    if (s_exit_at_end)
    {
        /* --exit-at-replay-end: konec běhu vyřídí automatizace (pauza
         * s důvodem exit, result.json, konec procesu). */
        bool desync = (outcome == REPLAY_OUTCOME_DESYNC);
        automation_request_exit(desync ? AUTOMATION_EXIT_REPLAY_DESYNC : AUTOMATION_EXIT_SUCCESS,
                                desync ? AUTOMATION_REASON_REPLAY_DESYNC : AUTOMATION_REASON_REPLAY_END,
                                message, NULL);
        return;
    };
    g_emulator.pause_reason = (outcome == REPLAY_OUTCOME_DESYNC) ? EMU_PAUSE_REASON_REPLAY_DESYNC
                                                                  : EMU_PAUSE_REASON_REPLAY_END;
    emulator_pause(true);
}


/**
 * @brief Porovná hash s řádkem záznamu.
 * @param[out] parts Rozjeté části oddělené čárkou ("" = shoda).
 */
static void replay_compare_hash(const st_REPLAY_HASH_ROW *want, char *parts, size_t size)
{
    st_REPLAY_HASH_ROW now;
    replay_compute_hash(&now, want->frame);
    parts[0] = '\0';
    for (int p = 0; p < REPLAY_HASH_PART_COUNT; p++)
    {
        /* Starší záznam část periph nemá - nic k porovnání. */
        if (p == REPLAY_HASH_PERIPH && want->part[p][0] == '\0') continue;
        if (strcmp(now.part[p], want->part[p]) != 0)
        {
            if (parts[0]) g_strlcat(parts, ",", size);
            g_strlcat(parts, replay_hash_part_name((en_REPLAY_HASH_PART)p), size);
        };
    };
    if (now.ticks != want->ticks)
    {
        if (parts[0]) g_strlcat(parts, ",", size);
        g_strlcat(parts, "time", size);
    };
}


/** @brief Ohlásí rozjetí a ukončí přehrávání. */
static void replay_play_desync(uint32_t f, const char *parts)
{
    char msg[REPLAY_STATUS_TEXT_MAX];
    g_snprintf(msg, sizeof(msg), "state differs at frame %u (%s)", f, parts);
    replay_play_finish(REPLAY_OUTCOME_DESYNC, msg, parts);
}


/**
 * @brief Jeden snímek přehrávání (bod B_f nebo start, kroky 1-6).
 */
static void replay_play_step(void)
{
    uint32_t f = replay_current_frame();
    bool last = (f >= s_play->frames);

    replay_play_events_at(REPLAY_AT_DRAIN);
    replay_play_events_at(REPLAY_AT_BOUNDARY);
    replay_play_apply_inputs();

    /* Periodický hash (na posledním snímku se závěrečným hashem se
     * porovná až stav po událostech p). */
    while (s_hash_cursor < s_play->hashes->len
           && g_array_index(s_play->hashes, st_REPLAY_HASH_ROW, s_hash_cursor).frame < f)
    {
        s_hash_cursor++;
    };
    if (s_hash_cursor < s_play->hashes->len && !(last && s_play->final_hash))
    {
        const st_REPLAY_HASH_ROW *want = &g_array_index(s_play->hashes, st_REPLAY_HASH_ROW, s_hash_cursor);
        if (want->frame == f)
        {
            s_hash_cursor++;
            if (!s_deviated)
            {
                char parts[REPLAY_STATUS_TEXT_MAX];
                replay_compare_hash(want, parts, sizeof(parts));
                if (parts[0])
                {
                    replay_play_desync(f, parts);
                    return;
                };
            };
        };
    };

    replay_play_events_at(REPLAY_AT_PAUSE);
    replay_play_apply_inputs();

    if (!last)
    {
        replay_publish();
        return;
    };

    if (s_play->final_hash && !s_deviated && s_hash_cursor < s_play->hashes->len)
    {
        const st_REPLAY_HASH_ROW *want = &g_array_index(s_play->hashes, st_REPLAY_HASH_ROW, s_play->hashes->len - 1);
        char parts[REPLAY_STATUS_TEXT_MAX];
        replay_compare_hash(want, parts, sizeof(parts));
        s_hash_cursor = s_play->hashes->len;
        if (parts[0])
        {
            replay_play_desync(f, parts);
            return;
        };
    };
    char msg[REPLAY_STATUS_TEXT_MAX];
    g_snprintf(msg, sizeof(msg), "end of the replay (%u frames)%s", s_play->frames,
               s_deviated ? ", deviated: hashes not checked after the deviation" : "");
    replay_play_finish(REPLAY_OUTCOME_ENDED, msg, NULL);
}


/**
 * @brief Nasadí média záznamu, načte startovní snapshot a vynuluje kurzory.
 *
 * Společné pro start přehrávání a restart při seeku zpět.
 *
 * @return NULL = načteno; jinak chyba (média jsou pak vrácená podle INI).
 * @post Snímek 0, emulace stojí na hranici snímku.
 */
static const char *replay_play_load_start(const st_REPLAY_DATA *d, char *errbuf, size_t errsize)
{
    /* Obrazy médií před snapshotem: snapshot obnoví stav řadičů nad nimi. */
    const char *merr = replay_media_prepare(d, errbuf, errsize);
    if (merr) return merr;
    en_SNAPSHOT_RESULT sr = snapshot_load_from_buffer(d->start_snapshot, d->start_snapshot_size);
    if (sr != SNAPSHOT_OK)
    {
        replay_media_restore();
        g_snprintf(errbuf, errsize, "start snapshot load failed: %s", snapshot_result_to_string(sr));
        return errbuf;
    };
    pio8255_set_autotype(NULL);
    s_ev_cursor = 0;
    s_hash_cursor = 0;
    s_cmthack_cursor = 0;
    s_record_count = 0;
    s_deviated = false;
    memset(s_play_kbd, 0xFF, sizeof(s_play_kbd));
    memset(s_play_joy, 0xFF, sizeof(s_play_joy));
    s_start_screens = (uint64_t)g_gdg.total_elapsed.screens;
    s_frame = 0;
    s_boundary_ticks = gdg_get_total_ticks();
    s_start_ticks = s_boundary_ticks;
    return NULL;
}


/** @brief Start přehrávání (paused smyčka). @return NULL = v pořádku, jinak chyba. */
static const char *replay_play_start(const st_REPLAY_REQ *r, char *errbuf, size_t errsize)
{
    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(r->path, &err);
    if (!d)
    {
        g_snprintf(errbuf, errsize, "%s", err ? err : "cannot read the replay");
        g_free(err);
        return errbuf;
    };
    if (!d->platform || strcmp(d->platform, MZARCH_NAME) != 0)
    {
        g_snprintf(errbuf, errsize, "the replay is for '%s', this emulator is '%s'",
                   d->platform ? d->platform : "?", MZARCH_NAME);
        replay_data_free(d);
        return errbuf;
    };
    if (d->screen_ticks != (uint32_t)VIDEO_SCREEN_TICKS)
    {
        g_snprintf(errbuf, errsize, "the replay has %u ticks per frame, this emulator %u (other TV system?)",
                   d->screen_ticks, (unsigned)VIDEO_SCREEN_TICKS);
        replay_data_free(d);
        return errbuf;
    };
#ifndef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (d->has_debugger_events)
    {
        replay_data_free(d);
        return "the replay contains debugger events; it needs a build with the debugger";
    };
#endif
#ifdef HAVE_JOY
    /* Port bez joysticku (NONE) čte 0FFh a rozhraní MZ-1X03 ho hlásí jako
     * odpojený - jiný typ by přehrání rozjel. Typ je volba INI, přehrání ho
     * nemění. */
    for (int p = 0; p < JOY_DEVID_COUNT && p < REPLAY_JOY_PORTS; p++)
    {
        if (d->joy_type[p] == REPLAY_JOY_TYPE_UNKNOWN) continue;
        bool rec_none = (d->joy_type[p] == JOY_TYPE_NONE);
        bool now_none = (g_joy.dev[p].type == JOY_TYPE_NONE);
        if (rec_none != now_none)
        {
            g_snprintf(errbuf, errsize, "joystick port %d was %s when recording, here it is %s; "
                       "change the joystick settings", p + 1, rec_none ? "not connected" : "connected",
                       now_none ? "not connected" : "connected");
            replay_data_free(d);
            return errbuf;
        };
    };
#endif
    const char *lerr = replay_play_load_start(d, errbuf, errsize);
    if (lerr)
    {
        replay_data_free(d);
        return lerr;
    };
    s_play = d;
    replay_rtc_set_base(d->rtc_base);

    g_mutex_lock(&s_lock);
    s_status.outcome = REPLAY_OUTCOME_NONE;
    s_status.outcome_message[0] = '\0';
    s_status.desync_parts[0] = '\0';
    s_status.deviation[0] = '\0';
    s_status.deviated = false;
    g_strlcpy(s_status.path, r->path, sizeof(s_status.path));
    g_mutex_unlock(&s_lock);

    replay_set_mode(REPLAY_MODE_PLAYING);
    replay_joy_latch(true);
    fprintf(stderr, "[REPLAY] playing '%s' (%u frames)\n", r->path, d->frames);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (freeze_count() > 0) replay_deviate("Freeze Bytes are active");
#endif
    replay_ckpt_begin(&r->ckpt);
    replay_play_step();
    if (s_play) replay_ckpt_take();
    replay_publish();
    return NULL;
}


/** @brief Obnoví stav přehrávače z checkpointu (kurzory, vstupy, odchýlení). */
static void replay_play_restore_cursors(const st_REPLAY_CKPT *c)
{
    s_ev_cursor = c->ev_cursor;
    s_hash_cursor = c->hash_cursor;
    s_cmthack_cursor = c->cmthack_cursor;
    s_record_count = c->record_count;
    memcpy(s_play_kbd, c->play_kbd, sizeof(s_play_kbd));
    memcpy(s_play_joy, c->play_joy, sizeof(s_play_joy));
    s_deviated = c->deviated;
    g_mutex_lock(&s_lock);
    s_status.deviated = s_deviated;
    if (!s_deviated) s_status.deviation[0] = '\0';
    g_mutex_unlock(&s_lock);
    replay_play_apply_inputs();
}


/**
 * @brief Přehrávání znovu od začátku (seek zpět bez použitelného checkpointu).
 *
 * Vrátí média podle INI a znovu nasadí čisté kopie ze záznamu, načte
 * startovní snapshot a provede snímek 0. Checkpointy se zahodí (patřily
 * obsahu médií, který už neplatí) a vznikne nový checkpoint snímku 0.
 *
 * @return NULL = hotovo; jinak chyba (přehrávání pak skončí s chybou).
 */
static const char *replay_play_restart(char *errbuf, size_t errsize)
{
    replay_media_restore();
    const char *e = replay_play_load_start(s_play, errbuf, errsize);
    if (e) return e;
    replay_ckpt_ring_clear(&s_ckpt);
    g_mutex_lock(&s_lock);
    s_status.deviated = false;
    s_status.deviation[0] = '\0';
    g_mutex_unlock(&s_lock);
    replay_play_step();
    if (s_play) replay_ckpt_take();
    fprintf(stderr, "[REPLAY] playback restarted from the beginning\n");
    return NULL;
}


/**
 * @brief Seek přehrávání (paused smyčka).
 *
 * @param target  Cílový snímek (za koncem se omezí na konec záznamu).
 * @param[out] out_run true = dohrávání běží (emulaci rozběhnout), false = už
 *                 stojí na cíli.
 * @return NULL = v pořádku, jinak chyba anglicky.
 */
static const char *replay_play_seek_from(uint32_t target, bool *out_run, int32_t *out_from,
                                         char *errbuf, size_t errsize);

static const char *replay_play_seek(uint32_t target, bool *out_run, char *errbuf, size_t errsize)
{
    return replay_play_seek_from(target, out_run, NULL, errbuf, errsize);
}


/**
 * @brief Seek přehrávání s údajem, odkud vyšel (goto).
 *
 * @param[out] out_from Volitelně: snímek načteného checkpointu, nebo
 *                 REPLAY_GOTO_FROM_RESTART (od začátku), nebo
 *                 REPLAY_GOTO_FROM_NONE (bez načtení, jen dohrání).
 *
 * Ostatní parametry a návrat jako replay_play_seek().
 */
static const char *replay_play_seek_from(uint32_t target, bool *out_run, int32_t *out_from,
                                         char *errbuf, size_t errsize)
{
    *out_run = false;
    if (out_from) *out_from = REPLAY_GOTO_FROM_NONE;
    if (target > s_play->frames) target = s_play->frames;
    uint32_t cur = replay_current_frame();
    bool at_boundary = replay_paused_at_boundary();
    if (target < cur || (target == cur && !at_boundary))
    {
        guint idx = 0;
        if (replay_ckpt_ring_find(&s_ckpt, target, g_media_writes, &idx) == REPLAY_CKPT_FOUND)
        {
            const st_REPLAY_CKPT *c = replay_ckpt_ring_get(&s_ckpt, idx);
            const char *e = replay_ckpt_load(c, errbuf, errsize);
            if (e)
            {
                /* Stav stroje už neodpovídá záznamu - přehrávání ukončit. */
                replay_play_finish(REPLAY_OUTCOME_ERROR, e, NULL);
                return e;
            };
            replay_play_restore_cursors(c);
            if (out_from) *out_from = (int32_t)c->frame;
            fprintf(stderr, "[REPLAY] playback back at checkpoint frame %u\n", c->frame);
        }
        else
        {
            if (out_from) *out_from = REPLAY_GOTO_FROM_RESTART;
            const char *e = replay_play_restart(errbuf, errsize);
            if (e)
            {
                replay_play_finish(REPLAY_OUTCOME_ERROR, e, NULL);
                return e;
            };
            if (!s_play) return NULL; /* skončilo hned na snímku 0 */
        };
    };
    if (replay_current_frame() < target)
    {
        replay_seek_begin(target);
        *out_run = true;
    };
    replay_publish();
    return NULL;
}


/**
 * @brief Rewind při přehrávání = seek na checkpoint před aktuálním snímkem.
 * @param frames 0 = o jeden checkpoint zpět, jinak aspoň o tolik snímků.
 */
static const char *replay_play_rewind(uint32_t frames, bool *out_run, char *errbuf, size_t errsize)
{
    uint32_t cur = replay_current_frame();
    if (frames != 0) return replay_play_seek(cur > frames ? cur - frames : 0, out_run, errbuf, errsize);
    if (cur == 0)
    {
        *out_run = false;
        return "already at the start of the replay";
    };
    guint idx = 0;
    en_REPLAY_CKPT_FIND fr = replay_ckpt_ring_find(&s_ckpt, cur - 1, g_media_writes, &idx);
    uint32_t target = (fr == REPLAY_CKPT_NONE) ? 0 : replay_ckpt_ring_get(&s_ckpt, idx)->frame;
    return replay_play_seek(target, out_run, errbuf, errsize);
}


/* ================================================================
 * Goto přehrávání (čas v taktech GDG)
 * ================================================================ */

/**
 * @brief Konec goto: potlačení breakpointů pryč, rychlost jako před goto.
 *
 * Volá ho první průchod paused smyčkou po zastavení (cíl, breakpoint
 * s keep, konec nebo rozjetí přehrávání, uživatel) a konec přehrávání.
 * @post s_goto_active == false.
 */
static void replay_goto_finish(void)
{
    if (!s_goto_active) return;
    s_goto_active = false;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    run_control_stop_at_clear();
    g_debugger.bp_suppress = 0;
#endif
    if (!s_goto_saved_max) emulator_max_speed(false);
    /* Hrající páska s cpu_boost si MAX SPEED drží sama. */
    cmt_cpu_boost_apply();
    replay_publish();
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * @brief Na hranici snímku seeku (nebo hned, bez seeku): zapnout zastavení
 *        na čase, případně checkpoint.
 *
 * @param paused true = emulace stojí v pauze (paused smyčka), false = běží
 *               (bod B_f dohrávání seeku).
 * @return true = je potřeba běžet (emulaci rozběhnout), false = cíl už
 *         dosažen (v pauze goto skončilo, za běhu se emulace zastaví).
 */
static bool replay_goto_arm(bool paused)
{
    bool at_boundary = paused ? replay_paused_at_boundary() : true;
    if (s_goto_ckpt && at_boundary && !s_goto_ckpt_taken)
    {
        size_t before = s_ckpt.count;
        bool had = replay_ckpt_ring_has_frame(&s_ckpt, replay_current_frame());
        replay_ckpt_take_ex(true);
        s_goto_ckpt_taken = had || s_ckpt.count != before
                            || replay_ckpt_ring_has_frame(&s_ckpt, replay_current_frame());
    };
    if (gdg_get_total_ticks() >= s_goto_ticks)
    {
        if (paused)
        {
            replay_goto_finish();
        }
        else
        {
            g_emulator.pause_reason = EMU_PAUSE_REASON_REPLAY_GOTO;
            emulator_pause(true);
        };
        return false;
    };
    run_control_stop_at_arm(s_goto_ticks, EMU_PAUSE_REASON_REPLAY_GOTO);
    if (!g_emulator.max_speed) emulator_max_speed(true);
    return true;
}
#endif


/**
 * @brief Goto při přehrávání (paused smyčka): seek na snímek cíle a dojetí.
 *
 * @param r       Požadavek (ticks, keep_bps, ckpt_at_target).
 * @param[out] out_run true = emulaci rozběhnout (seek nebo dojetí běží).
 * @return NULL = v pořádku, jinak chyba anglicky (stav beze změny, pokud
 *         chyba nevznikla až při načtení checkpointu).
 */
static const char *replay_play_goto(const st_REPLAY_REQ *r, bool *out_run, char *errbuf, size_t errsize)
{
    *out_run = false;
#ifndef MZ800EMU_CFG_DEBUGGER_ENABLED
    (void)r;
    (void)errbuf;
    (void)errsize;
    return "goto needs a build with the debugger";
#else
    const uint64_t vst = (uint64_t)VIDEO_SCREEN_TICKS;
    if (r->ticks < s_start_ticks) return "target time is before the start of the replay";
    uint64_t f_abs = r->ticks / vst;
    if (f_abs < s_start_screens) return "target time is before the start of the replay";
    uint64_t f = f_abs - s_start_screens;
    if (f >= s_play->frames) return "target time is after the end of the replay";
    uint32_t seek_f = (uint32_t)f;
    if ((r->ticks % vst) < REPLAY_GOTO_BOUNDARY_MARGIN && seek_f > 0) seek_f--;

    s_goto_saved_max = g_emulator.max_speed;
    s_goto_active = true;
    s_goto_ticks = r->ticks;
    s_goto_ckpt = r->ckpt_at_target;
    s_goto_ckpt_taken = false;
    s_goto_seek_from = REPLAY_GOTO_FROM_NONE;
    g_debugger.bp_suppress = r->keep_bps ? 0 : 1;

    uint64_t now = gdg_get_total_ticks();
    if (r->ticks >= now && seek_f <= replay_current_frame())
    {
        /* Cíl dopředu v běžícím snímku: stačí dojet. */
        *out_run = replay_goto_arm(true);
        replay_publish();
        return NULL;
    };
    bool run = false;
    const char *e = replay_play_seek_from(seek_f, &run, &s_goto_seek_from, errbuf, errsize);
    if (e || !s_play)
    {
        replay_goto_finish();
        return e;
    };
    /* run: dohrávání na hranici seek_f běží, zastavení na čase zapne
     * replay_on_frame_boundary(); jinak stojí na hranici seek_f. */
    *out_run = run ? true : replay_goto_arm(true);
    replay_publish();
    return NULL;
#endif
}


/* ================================================================
 * Háčky emu vlákna
 * ================================================================ */

/** @brief Přehrávání: živý reset (replay_divert_reset_request()) = odchýlení. */
static void replay_play_check_live_reset(void)
{
    g_mutex_lock(&s_lock);
    bool live_reset = s_pending_reset;
    s_pending_reset = false;
    g_mutex_unlock(&s_lock);
    if (live_reset) replay_deviate("reset during playback");
}


/** @brief Pozastaví emulaci kvůli požadavku (vyřídí ho paused smyčka). */
static void replay_pause_for_request(void)
{
    if (EMULATOR_TEST_PAUSED) return;
    s_resume_after_request = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_REPLAY;
    emulator_pause(true);
}


void replay_on_frame_boundary(void)
{
    /* Vstupy a operace kazety z UI mimo záznam: provést hned na hranici
     * snímku, při přehrávání zahodit. */
    if (!s_rec && replay_ui_cmt_pending())
    {
        replay_ui_off_drain(!s_play);
        replay_ui_cmt_drain(REPLAY_AT_BOUNDARY);
    };
    if (s_rec)
    {
        s_frame = replay_current_frame();
        replay_rec_boundary();
        replay_ckpt_on_boundary();
        s_boundary_ticks = gdg_get_total_ticks();
        replay_publish();
    }
    else if (s_play)
    {
        replay_play_check_live_reset();
        s_frame = replay_current_frame();
        replay_play_step();
        if (s_play) replay_ckpt_on_boundary();
        s_boundary_ticks = gdg_get_total_ticks();
        if (s_play && s_seeking && s_frame >= s_seek_target)
        {
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            if (s_goto_active)
            {
                /* Goto: místo pauzy seeku dojet na čas (MAX SPEED zůstane). */
                replay_seek_done(false);
                (void)replay_goto_arm(false);
            }
            else
#endif
            {
                replay_seek_done(true);
            };
            replay_publish();
        };
    };

    if (!replay_have_request()) return;

    /* Konec záznamu / přehrávání jde vyřídit hned na hranici; start
     * potřebuje pauzu (snapshot). */
    if ((s_rec || s_play) && replay_head_is_stop())
    {
        st_REPLAY_REQ *r = replay_dequeue();
        char errbuf[REPLAY_STATUS_TEXT_MAX];
        const char *e = NULL;
        if (s_rec) e = replay_rec_stop(true, errbuf, sizeof(errbuf));
        else replay_play_finish(REPLAY_OUTCOME_STOPPED, "stopped by request", NULL);
        replay_request_done(r->seq, e == NULL, e);
        replay_req_free(r);
        if (!replay_have_request()) return;
    };
    replay_pause_for_request();
}


void replay_on_drain_point(void)
{
    if (!s_play) return;
    replay_play_events_at(REPLAY_AT_DRAIN);
}


void replay_on_paused(void)
{
    /* Goto končí první pauzou (cíl, breakpoint s keep, konec přehrávání,
     * uživatel, další požadavek). */
    if (s_goto_active) replay_goto_finish();
    if (s_rec) replay_rec_paused();
    else if (s_play) replay_play_check_live_reset();
    /* Mimo záznam vstupy a operace kazety z UI hned, při přehrávání je zahodit. */
    if (!s_rec && replay_ui_cmt_pending())
    {
        replay_ui_off_drain(!s_play);
        replay_ui_cmt_drain(REPLAY_AT_PAUSE);
    };

    if (!replay_have_request()) return;

    bool stay_paused = false;
    bool start_running = false;
    st_REPLAY_REQ *r;
    while ((r = replay_dequeue()) != NULL)
    {
        char errbuf[REPLAY_STATUS_TEXT_MAX];
        const char *e = NULL;
        /* Revize stavu (state_rev.h, Z8): každý vyřízený požadavek (start,
         * stop, rewind, seek, goto) může stav změnit; i odmítnutý (bezpečný
         * směr). */
        STATE_REV_BUMP(STATE_REV_SRC_REPLAY);
        switch (r->kind)
        {
            case REPLAY_REQ_RECORD:
                if (s_rec || s_play) e = "recording or playback is already running";
                else e = replay_rec_start(r, errbuf, sizeof(errbuf));
                if (e)
                {
                    g_mutex_lock(&s_lock);
                    g_strlcpy(s_status.path, r->path ? r->path : "", sizeof(s_status.path));
                    g_mutex_unlock(&s_lock);
                    replay_publish_outcome(REPLAY_OUTCOME_ERROR, 0, e, NULL);
                };
                break;
            case REPLAY_REQ_PLAY:
                if (s_rec || s_play) e = "recording or playback is already running";
                else e = replay_play_start(r, errbuf, sizeof(errbuf));
                if (e)
                {
                    replay_publish_outcome(REPLAY_OUTCOME_ERROR, 0, e, NULL);
                    if (s_exit_at_end)
                    {
                        automation_request_exit(AUTOMATION_EXIT_ERROR, AUTOMATION_REASON_ERROR, e, NULL);
                    };
                }
                else if (r->pause_after_load)
                {
                    stay_paused = true;
                }
                else
                {
                    /* Přehrávání se po načtení rozběhne i z pauzy,
                     * kterou nevyvolal požadavek. */
                    start_running = true;
                };
                break;
            case REPLAY_REQ_STOP:
                if (s_rec) e = replay_rec_stop(replay_paused_at_boundary(), errbuf, sizeof(errbuf));
                else if (s_play) replay_play_finish(REPLAY_OUTCOME_STOPPED, "stopped by request", NULL);
                else e = "nothing to stop";
                break;
            case REPLAY_REQ_REWIND:
            case REPLAY_REQ_SEEK:
            {
                bool run = false;
                if (r->kind == REPLAY_REQ_REWIND && s_rec) e = replay_rec_rewind(r->frames, errbuf, sizeof(errbuf));
                else if (!s_play) e = (r->kind == REPLAY_REQ_SEEK) ? "seek works only during playback"
                                                                    : "nothing is recording or playing";
                else if (r->kind == REPLAY_REQ_REWIND) e = replay_play_rewind(r->frames, &run, errbuf, sizeof(errbuf));
                else e = replay_play_seek(r->frames, &run, errbuf, sizeof(errbuf));
                /* Úspěch: dohrávání seeku běží, jinak emulace zůstane stát
                 * na hranici snímku checkpointu. Odmítnutí stav nemění. */
                if (!e && run) start_running = true;
                else if (!e) stay_paused = true;
                break;
            };
            case REPLAY_REQ_GOTO:
            {
                bool run = false;
                if (!s_play) e = "goto works only during playback";
                else e = replay_play_goto(r, &run, errbuf, sizeof(errbuf));
                if (!e && run) start_running = true;
                else if (!e) stay_paused = true;
                break;
            };
        };
        replay_request_done(r->seq, e == NULL, e);
        replay_req_free(r);
    };

    /* Přehrávání mohlo skončit hned při startu (rozjetí na snímku 0 nebo
     * záznam s nulovou délkou) - pak zůstat v pauze. */
    bool resume = start_running && s_play != NULL;
    if (s_resume_after_request)
    {
        s_resume_after_request = false;
        if (!stay_paused && g_emulator.pause_reason == EMU_PAUSE_REASON_REPLAY) resume = true;
    };
    if (resume && !stay_paused)
    {
        emulator_pause(false);
    };

    /* Žádost o konec běhu z tohoto průchodu (přehrávání skončilo nebo
     * selhalo hned při startu s --exit-at-replay-end): automatizace ji
     * jinak vyřizuje jen na vstupu do pauzy, a ta už proběhla. Bez
     * žádosti nedělá nic. */
    automation_on_paused();
}


void replay_shutdown(void)
{
    if (s_rec)
    {
        char errbuf[REPLAY_STATUS_TEXT_MAX];
        /* Konec procesu z paused smyčky (automatizace, zavření okna) stojí
         * typicky na hranici snímku - pak i se závěrečným hashem. */
        (void)replay_rec_stop(replay_paused_at_boundary(), errbuf, sizeof(errbuf));
    };
    if (s_play)
    {
        replay_seek_done(false);
        replay_goto_finish();
        replay_data_free(s_play);
        s_play = NULL;
        replay_media_restore();
    };
    replay_ckpt_end();
    replay_joy_latch(false);
    replay_set_mode(REPLAY_MODE_OFF);
    g_mutex_lock(&s_lock);
    st_REPLAY_UI_CMT *u;
    while ((u = (st_REPLAY_UI_CMT *)g_queue_pop_head(&s_ui_cmt)) != NULL)
    {
        g_free(u->path);
        g_free(u);
    };
    replay_ui_off_clear_locked();
    g_atomic_int_and((guint *)&g_replay_frame_hook, ~(guint)REPLAY_HOOK_UI);
    g_mutex_unlock(&s_lock);
    st_REPLAY_REQ *r;
    while ((r = replay_dequeue()) != NULL)
    {
        replay_request_done(r->seq, false, "emulator is shutting down");
        replay_req_free(r);
    };
}


/* ================================================================
 * Vstupy z UI vlákna
 * ================================================================ */

void replay_host_keyboard_submit(const uint8_t *matrix, int joy_dev, uint8_t joy_state)
{
    int mode = g_atomic_int_get(&g_replay_mode);
    if (mode == REPLAY_MODE_PLAYING) return;
    if (mode == REPLAY_MODE_RECORDING)
    {
        g_mutex_lock(&s_lock);
        memcpy(s_host_kbd, matrix, sizeof(s_host_kbd));
        s_host_joy_dev = joy_dev;
        s_host_joy_state = joy_state;
        g_mutex_unlock(&s_lock);
        return;
    };
    /* Mimo záznam matici zapíše emu vlákno (replay_ui_off_drain()) na
     * hranici snímku nebo v paused smyčce; platí poslední sken. */
    g_mutex_lock(&s_lock);
    memcpy(s_off_host_kbd, matrix, sizeof(s_off_host_kbd));
    s_off_host_joy_dev = joy_dev;
    s_off_host_joy_state = joy_state;
    s_off_host_pending = true;
    replay_ui_hook_refresh_locked();
    g_mutex_unlock(&s_lock);
}


void replay_host_vkbd_set(int col, int bit, bool pressed)
{
    if (col < 0 || col >= REPLAY_KBD_COLS || bit < 0 || bit > 7) return;
    int mode = g_atomic_int_get(&g_replay_mode);
    if (mode == REPLAY_MODE_PLAYING) return;
    if (mode == REPLAY_MODE_RECORDING)
    {
        g_mutex_lock(&s_lock);
        if (pressed) s_ui_vkbd[col] &= (uint8_t)~(1u << bit);
        else s_ui_vkbd[col] |= (uint8_t)(1u << bit);
        g_mutex_unlock(&s_lock);
        return;
    };
    /* Mimo záznam matici změní emu vlákno (replay_ui_off_drain()); platí
     * poslední změna bitu. */
    const uint8_t m = (uint8_t)(1u << bit);
    g_mutex_lock(&s_lock);
    if (pressed)
    {
        s_off_vkbd_press[col] |= m;
        s_off_vkbd_release[col] &= (uint8_t)~m;
    }
    else
    {
        s_off_vkbd_release[col] |= m;
        s_off_vkbd_press[col] &= (uint8_t)~m;
    };
    replay_ui_hook_refresh_locked();
    g_mutex_unlock(&s_lock);
}


void replay_host_autotype(const char *text, double kd_ms, double ku_ms)
{
    int mode = g_atomic_int_get(&g_replay_mode);
    if (mode == REPLAY_MODE_PLAYING) return;
    /* Při záznamu ho převezme bod B_f (zapíše ho), mimo záznam
     * replay_ui_off_drain(); emu vlákno vždy dostane vlastní kopii textu. */
    g_mutex_lock(&s_lock);
    g_free(s_pending_autotype_text);
    s_pending_autotype_text = g_strdup(text ? text : "");
    s_pending_kd_ms = (uint32_t)kd_ms;
    s_pending_ku_ms = (uint32_t)ku_ms;
    s_pending_autotype = true;
    replay_ui_hook_refresh_locked();
    g_mutex_unlock(&s_lock);
}


void replay_host_autotype_delays(double kd_ms, double ku_ms)
{
    if (g_atomic_int_get(&g_replay_mode) != REPLAY_MODE_OFF) return;
    g_mutex_lock(&s_lock);
    s_pending_delay_kd_ms = (uint32_t)kd_ms;
    s_pending_delay_ku_ms = (uint32_t)ku_ms;
    s_pending_delays = true;
    replay_ui_hook_refresh_locked();
    g_mutex_unlock(&s_lock);
}


void replay_host_cmt(en_REPLAY_CMT_OP op, int32_t arg, const char *path)
{
    if (op < 0 || op >= REPLAY_CMT_OP_COUNT) return;
    if (g_atomic_int_get(&g_replay_mode) == REPLAY_MODE_PLAYING) return;
    st_REPLAY_UI_CMT *u = g_new0(st_REPLAY_UI_CMT, 1);
    u->op = op;
    u->arg = arg;
    u->path = g_strdup(path);
    g_mutex_lock(&s_lock);
    g_queue_push_tail(&s_ui_cmt, u);
    g_atomic_int_or((guint *)&g_replay_frame_hook, REPLAY_HOOK_UI);
    g_mutex_unlock(&s_lock);
}


void replay_cmthack_note(const char *chosen)
{
    if (!s_rec) return;
    st_REPLAY_EVENT ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = REPLAY_EV_CMTHACK;
    ev.arg = -1;
    char *base = chosen ? g_path_get_basename(chosen) : g_strdup("");
    ev.text = base;
    if (chosen)
    {
        gchar *data = NULL;
        gsize len = 0;
        if (g_file_get_contents(chosen, &data, &len, NULL))
        {
            char slot[32];
            g_snprintf(slot, sizeof(slot), "cmthack.%d", s_cmthack_count);
            if (replay_data_add_media(s_rec, slot, chosen, (const uint8_t *)data, len, NULL, true) >= 0)
            {
                ev.arg = s_cmthack_count++;
            };
            g_free(data);
        };
        if (ev.arg < 0) replay_data_add_warning(s_rec, "cannot read a file chosen for the CMT hack");
    };
    replay_rec_log(&ev, REPLAY_AT_BOUNDARY);
    g_free(base);
}


bool replay_cmthack_playback(char **out_path)
{
    *out_path = NULL;
    if (!s_play) return false;
    while (s_cmthack_cursor < s_play->events->len)
    {
        const st_REPLAY_EVENT *ev = &g_array_index(s_play->events, st_REPLAY_EVENT, s_cmthack_cursor++);
        if (ev->kind != REPLAY_EV_CMTHACK) continue;
        if (ev->arg < 0) return true; /* v záznamu zrušeno */
        char slot[32];
        g_snprintf(slot, sizeof(slot), "cmthack.%d", ev->arg);
        const st_REPLAY_MEDIA *m = replay_data_find_media(s_play, slot);
        char *tmp = (m && m->data) ? replay_media_tmp_file(ev->text && ev->text[0] ? ev->text : "cmthack.mzf") : NULL;
        if (tmp && g_file_set_contents(tmp, (const gchar *)m->data, (gssize)m->size, NULL))
        {
            *out_path = tmp;
            return true;
        };
        g_free(tmp);
        replay_deviate("a CMT hack file of the replay is missing");
        return true;
    };
    replay_deviate("the CMT hack asked for more files than the replay has");
    return true;
}


bool replay_divert_reset_request(void)
{
    int mode = g_atomic_int_get(&g_replay_mode);
    if (mode == REPLAY_MODE_RECORDING)
    {
        g_mutex_lock(&s_lock);
        s_pending_reset = true;
        g_mutex_unlock(&s_lock);
        return true;
    };
    if (mode == REPLAY_MODE_PLAYING)
    {
        /* Odchýlení zapíše emu vlákno (stav přehrávání patří jemu):
         * reset se provede jako dřív a přehrávač ho uvidí jako zásah. */
        g_mutex_lock(&s_lock);
        s_pending_reset = true;
        g_mutex_unlock(&s_lock);
    };
    return false;
}


/* ================================================================
 * Brána příkazů debuggeru
 * ================================================================ */

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/** @brief Zapíše text odmítnutí (pod zámkem) a na stderr. */
static void replay_refuse(const char *fmt, const char *name)
{
    g_mutex_lock(&s_lock);
    g_snprintf(s_refusal, sizeof(s_refusal), fmt, name);
    fprintf(stderr, "[REPLAY] refused: %s\n", s_refusal);
    g_mutex_unlock(&s_lock);
}


bool replay_dbg_before(struct st_DBGAPI_CMDRQ *rq)
{
    if (s_applying || !s_rec) return true;
    en_DBGAPI_CMD cmd = (en_DBGAPI_CMD)(rq->cmd & DBGAPI_CMD_MASK);
    en_REPLAY_DBG_CLASS cls = replay_dbg_classify(cmd);
    if (cls == REPLAY_DBG_FORBIDDEN)
    {
        replay_refuse("command %s is not allowed while recording a replay", dbgapi_cmd_to_str(cmd));
        return false;
    };
    if ((cls == REPLAY_DBG_RECORD || cls == REPLAY_DBG_INPUT) && EMULATOR_TEST_PAUSED && !replay_paused_at_boundary())
    {
        replay_refuse("command %s changes the machine in a pause between frame boundaries; "
                      "while recording, pause at a frame boundary (run with a frame count or deadline)",
                      dbgapi_cmd_to_str(cmd));
        return false;
    };
    return true;
}


void replay_dbg_after(struct st_DBGAPI_CMDRQ *rq, bool was_paused)
{
    if (s_applying) return;
    en_DBGAPI_CMD cmd = (en_DBGAPI_CMD)(rq->cmd & DBGAPI_CMD_MASK);
    en_REPLAY_DBG_CLASS cls = replay_dbg_classify(cmd);

    if (s_rec)
    {
        int cmt_op = -1;
        int32_t cmt_arg = 0;
        const char *cmt_path = NULL;
        if (cls == REPLAY_DBG_RECORD && rq->success
            && replay_dbg_cmt_op(cmd, rq->data_ptr, &cmt_op, &cmt_arg, &cmt_path))
        {
            /* Kazeta z MCP: událost `cmt` (přehraje ji i build bez
             * debuggeru), ne `dbg`. Operace už proběhla v dispatch. */
            st_REPLAY_EVENT ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = REPLAY_EV_CMT;
            ev.cmd = (char *)replay_cmt_op_name((en_REPLAY_CMT_OP)cmt_op);
            ev.arg = cmt_arg;
            ev.text = (char *)cmt_path;
            replay_rec_log(&ev, was_paused ? REPLAY_AT_PAUSE : REPLAY_AT_DRAIN);
            replay_publish();
            return;
        };
        if (cls == REPLAY_DBG_RECORD && rq->success)
        {
            char *args = replay_dbg_args_to_json(cmd, rq->data_ptr);
            if (!args)
            {
                replay_note_external_write(dbgapi_cmd_to_str(cmd));
                return;
            };
            st_REPLAY_EVENT ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = REPLAY_EV_DBG;
            ev.cmd = (char *)replay_dbg_cmd_name(cmd);
            ev.args_json = args;
            replay_rec_log(&ev, was_paused ? REPLAY_AT_PAUSE : REPLAY_AT_DRAIN);
            g_free(args);
            replay_publish();
        }
        else if (cls == REPLAY_DBG_INPUT && was_paused)
        {
            replay_rec_sample(REPLAY_AT_PAUSE, false);
        };
        return;
    };

    if (s_play && rq->success && (cls == REPLAY_DBG_RECORD || cls == REPLAY_DBG_FORBIDDEN))
    {
        char *w = g_strdup_printf("%s during playback", dbgapi_cmd_to_str(cmd));
        replay_deviate(w);
        g_free(w);
    };
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
