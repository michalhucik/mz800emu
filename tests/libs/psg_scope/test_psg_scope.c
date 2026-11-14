/**
 * @file   test_psg_scope.c
 * @brief  Standalone unit testy pro pomocné algoritmy PSG Audio Scope okna.
 *
 * Pokrývá:
 *  - ring buffer ne-destruktivní push + wrap-around (= overwrite oldest)
 *  - note event state machine (silent <-> playing transitions, edge cases)
 *  - pitch change split V1.5 (= jiný MIDI int -> split, drift v rámci
 *    stejného semitonu -> no split)
 *  - MIDI pitch detection (divider -> MIDI note + cents detune)
 *  - MIDI frames -> ticks konverze
 *  - CSV velocity z attenuace
 *
 * Algoritmy zde nejsou znovu-vytvořené - jsou to **přesné kopie** privátních
 * helper funkcí z psg_audio_scope_window.cpp pro paritu testu. Pokud se
 * helpery v okenním kódu změní, je nutné zde aktualizovat duplicitu (a test
 * pomůže odhalit nekonzistenci pomocí konkrétních hodnot).
 *
 * Standalone - nepotřebuje mztest_init ani emulator core, jen libm.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Konstanty (paritní k psg_audio_scope_window.cpp + mz800 video)
 * ======================================================================== */

/* MZ-800 PAL GDGCLK_BASE = VIDEO_SCREEN_WIDTH * VIDEO_SCREEN_HEIGHT * 50.
 * Pro testovací výpočty MIDI z divideru musíme volit konkrétní hodnotu.
 * Použijeme 17 721 600 Hz (= MZ-800 simulovaná frekvence). */
#define TEST_GDGCLK_BASE       17721600.0

/* GDG -> CPU clock divider (MZ-800 = 5). Frekvenční vzorec PSG SN76489:
 *   f_out = GDGCLK_BASE / (32 * divider * GDGCLK2CPU_DIVIDER)
 *         = CPU_CLOCK / (32 * divider)
 * Odpovídá psg_audio_scope_window.cpp::psg_scope_tone_frequency_hz. */
#define TEST_GDGCLK2CPU_DIV    5.0

#define TEST_RING_CAPACITY     600u
#define TEST_EVENTS_CAPACITY   1024u
/* GDGCLK_BASE pro MZ-800 simulovaný = VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC.
 * Paritní k mz800_gdgclk.h::GDGCLK_BASE (~17.7345 MHz, mírně upraveno na celé
 * pxCLK/s). Mock použijeme 17734475 (= GDGCLK_REAL_BASE) - pro testy je rozdíl
 * v jednotkách Hz irelevantní. */
#define TEST_FRAMES_PER_SECOND 17734475.0
#define TEST_MIDI_PPQN         480u

/* PSG channel type (paritní k en_PSG_CHTYPE z psg.h). */
typedef enum {
    TEST_PSG_CHTYPE_TONE  = 0,
    TEST_PSG_CHTYPE_NOISE = 1,
} test_psg_chtype_t;


/* ========================================================================
 * Ring buffer (paritní k st_PSG_SCOPE_RING)
 * ======================================================================== */

/** @brief Vzorek v ring bufferu. */
typedef struct {
    uint64_t t_frame;
    uint8_t  attn;
    uint16_t divider;
    uint8_t  channel_type;
} test_psg_sample_t;

/** @brief Ring buffer (paritní invarianty: head je index příštího slotu, count <= capacity). */
typedef struct {
    test_psg_sample_t samples[TEST_RING_CAPACITY];
    unsigned head;
    unsigned count;
} test_psg_ring_t;

/** @brief Push vzorku - overwrite oldest při plnosti. */
static void test_ring_push(test_psg_ring_t *r, const test_psg_sample_t *s) {
    r->samples[r->head] = *s;
    r->head = (r->head + 1u) % TEST_RING_CAPACITY;
    if (r->count < TEST_RING_CAPACITY) r->count++;
}

/** @brief Vrátí ukazatel na nejnovější vzorek (nebo NULL pokud prázdné). */
static const test_psg_sample_t *test_ring_latest(const test_psg_ring_t *r) {
    if (r->count == 0u) return NULL;
    unsigned idx = (r->head + TEST_RING_CAPACITY - 1u) % TEST_RING_CAPACITY;
    return &r->samples[idx];
}


/* ========================================================================
 * Note event state machine (paritní k psg_events_note_on / note_off /
 * psg_sample_is_playing)
 * ======================================================================== */

typedef struct {
    uint64_t t_on, t_off;
    int      midi_pitch;
    int      cents_detune;
    uint8_t  velocity;
    uint8_t  channel;
    uint8_t  chip;
    uint8_t  channel_type;
} test_note_event_t;

typedef struct {
    test_note_event_t events[TEST_EVENTS_CAPACITY];
    unsigned head, count;
    bool     note_active;
    test_note_event_t active_note;
} test_events_t;

static bool test_is_playing(const test_psg_sample_t *s) {
    if (s->attn >= 15u) return false;
    if (s->channel_type == TEST_PSG_CHTYPE_TONE && s->divider == 1u) return false;
    return true;
}

static double test_tone_freq_hz(unsigned divider) {
    if (divider == 1u) return 0.0;
    if (divider == 0u) divider = 0x400u;
    return TEST_GDGCLK_BASE / (32.0 * (double)divider * TEST_GDGCLK2CPU_DIV);
}

static void test_tone_to_midi(unsigned divider, int *pitch, int *cents) {
    double f = test_tone_freq_hz(divider);
    if (f <= 0.0) { *pitch = 0; *cents = 0; return; }
    double midi_float = 12.0 * log2(f / 440.0) + 69.0;
    int nearest = (int)lround(midi_float);
    int c       = (int)lround((midi_float - (double)nearest) * 100.0);
    if (nearest < 0)   nearest = 0;
    if (nearest > 127) nearest = 127;
    if (c < -50) c = -50;
    if (c > 50)  c = 50;
    *pitch = nearest;
    *cents = c;
}

static void test_events_push(test_events_t *ev, const test_note_event_t *n) {
    ev->events[ev->head] = *n;
    ev->head = (ev->head + 1u) % TEST_EVENTS_CAPACITY;
    if (ev->count < TEST_EVENTS_CAPACITY) ev->count++;
}

static void test_note_on(test_events_t *ev, const test_psg_sample_t *cur,
                         unsigned chip, unsigned ch) {
    test_note_event_t *n = &ev->active_note;
    n->t_on = cur->t_frame;
    n->t_off = 0u;
    n->channel_type = cur->channel_type;
    if (cur->channel_type == TEST_PSG_CHTYPE_NOISE) {
        n->midi_pitch = -1;
        n->cents_detune = 0;
    } else {
        test_tone_to_midi(cur->divider, &n->midi_pitch, &n->cents_detune);
    }
    double v = 127.0 * (1.0 - (double)cur->attn / 15.0);
    int vi = (int)lround(v);
    if (vi < 0)   vi = 0;
    if (vi > 127) vi = 127;
    n->velocity = (uint8_t)vi;
    n->channel  = (uint8_t)ch;
    n->chip     = (uint8_t)chip;
    ev->note_active = true;
}

static void test_note_off(test_events_t *ev, const test_psg_sample_t *cur) {
    if (!ev->note_active) return;
    ev->active_note.t_off = cur->t_frame;
    test_events_push(ev, &ev->active_note);
    ev->note_active = false;
}

/** @brief Vyhodnocení jednoho vzorku v state machine (paritní k psg_audio_scope_tick).
 *
 * V1.5: doplněna pitch_change detekce uvnitř TONE noty (= jiný MIDI integer
 * pitch než active_note → split na novou notu).
 */
static void test_advance(test_events_t *ev, const test_psg_sample_t *prev,
                         const test_psg_sample_t *cur, unsigned chip, unsigned ch) {
    bool was_playing = prev ? test_is_playing(prev) : false;
    bool now_playing = test_is_playing(cur);

    /* Type swap během hraní = uzavřít a začít znovu. */
    bool type_changed = prev && was_playing && now_playing &&
                        (prev->channel_type != cur->channel_type);

    if (type_changed) {
        test_note_off(ev, cur);
        if (now_playing) test_note_on(ev, cur, chip, ch);
    } else if (!was_playing && now_playing) {
        test_note_on(ev, cur, chip, ch);
    } else if (was_playing && !now_playing) {
        test_note_off(ev, cur);
    } else if (was_playing && now_playing && ev->note_active &&
               cur->channel_type == TEST_PSG_CHTYPE_TONE &&
               ev->active_note.midi_pitch >= 0) {
        int new_pitch = 0, new_cents = 0;
        test_tone_to_midi(cur->divider, &new_pitch, &new_cents);
        if (new_pitch != ev->active_note.midi_pitch) {
            test_note_off(ev, cur);
            test_note_on(ev, cur, chip, ch);
        }
    }
}


/* ========================================================================
 * Volume envelope tracking (paritní k attn_history v st_PSG_SCOPE_NOTE_EVENT)
 * ======================================================================== */

/** @brief Maximální počet attn change pointů per nota (paritní s
 * `PSG_SCOPE_MAX_ATTN_POINTS` v psg_audio_scope_window.h). */
#define TEST_MAX_ATTN_POINTS 32u

typedef struct {
    uint64_t t_ticks;
    uint8_t  attn;
} test_attn_point_t;

typedef struct {
    test_attn_point_t attn_history[TEST_MAX_ATTN_POINTS];
    uint8_t           attn_history_count;
    uint8_t           attn_history_head;
    bool              attn_history_overflowed;
    uint8_t           attn_last_seen;
    bool              note_active;
} test_attn_state_t;

/** @brief Inicializace stavu na začátku noty (paritní k psg_events_note_on). */
static void test_attn_state_note_on(test_attn_state_t *st, uint8_t start_attn) {
    st->attn_history_count      = 0u;
    st->attn_history_head       = 0u;
    st->attn_history_overflowed = false;
    st->attn_last_seen          = start_attn;
    st->note_active             = true;
}

/** @brief Připojení attn change pointu (paritní k psg_events_append_attn_point).
 *
 * Filtruje:
 *  - `attn == 15` (note_off rezervováno),
 *  - `attn == attn_last_seen` (žádná změna).
 * Tyto guard podmínky jsou v tick() callbacku, helper sám je nedělá -
 * pro test je tu provádíme.
 */
static void test_attn_append(test_attn_state_t *st, uint64_t t_ticks, uint8_t attn) {
    if (!st->note_active) return;
    if (attn >= 15u) return;
    if (attn == st->attn_last_seen) return;

    test_attn_point_t *p = &st->attn_history[st->attn_history_head];
    p->t_ticks = t_ticks;
    p->attn    = attn;
    st->attn_history_head = (uint8_t)((st->attn_history_head + 1u) % TEST_MAX_ATTN_POINTS);
    if (st->attn_history_count < (uint8_t)TEST_MAX_ATTN_POINTS) {
        st->attn_history_count++;
    } else {
        st->attn_history_overflowed = true;
    }
    st->attn_last_seen = attn;
}

/** @brief CC 7 value mapping (paritní k MIDI export větvi v
 *  `psg_scope_export_midi`). */
static int test_attn_to_cc7(uint8_t attn) {
    int v = (int)lround(127.0 * (15.0 - (double)attn) / 15.0);
    if (v < 0)   v = 0;
    if (v > 127) v = 127;
    return v;
}


/* ========================================================================
 * MIDI helpers (paritní k psg_midi_frames_to_ticks)
 * ======================================================================== */

static uint32_t test_frames_to_ticks(uint64_t frames, int tempo_bpm) {
    if (tempo_bpm <= 0) tempo_bpm = 120;
    double sec      = (double)frames / TEST_FRAMES_PER_SECOND;
    double quarters = sec * ((double)tempo_bpm / 60.0);
    double ticks    = quarters * (double)TEST_MIDI_PPQN;
    if (ticks < 0.0) ticks = 0.0;
    return (uint32_t)llround(ticks);
}


/* ========================================================================
 * MIDI VLQ (paritní k psg_midi_write_vlq)
 * ======================================================================== */

/**
 * @brief Zapíše variable-length quantity do bufferu.
 *
 * Paritní kopie helper funkce z psg_audio_scope_window.cpp. Pokud se
 * implementace v okenním kódu změní, je třeba aktualizovat zde.
 *
 * @return Počet zapsaných bytů (1..4).
 */
static int test_midi_write_vlq(uint8_t *buf, uint32_t value) {
    uint8_t tmp[4];
    int n = 0;
    tmp[n++] = (uint8_t)(value & 0x7Fu);
    value >>= 7;
    while (value > 0u && n < 4) {
        tmp[n++] = (uint8_t)((value & 0x7Fu) | 0x80u);
        value >>= 7;
    }
    for (int i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}


/* ========================================================================
 * setUp / tearDown
 * ======================================================================== */

void setUp(void)    {}
void tearDown(void) {}


/* ========================================================================
 * Ring buffer testy
 * ======================================================================== */

/** @brief Prázdný ring má count=0 a vrací NULL pro latest. */
static void test_ring_empty_state(void) {
    test_psg_ring_t r;
    memset(&r, 0, sizeof(r));
    TEST_ASSERT_EQUAL_UINT(0u, r.count);
    TEST_ASSERT_NULL(test_ring_latest(&r));
}

/** @brief Po push se count zvedne a latest vrací poslední vzorek. */
static void test_ring_single_push(void) {
    test_psg_ring_t r;
    memset(&r, 0, sizeof(r));
    test_psg_sample_t s = { .t_frame = 42, .attn = 5, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_ring_push(&r, &s);
    TEST_ASSERT_EQUAL_UINT(1u, r.count);
    const test_psg_sample_t *l = test_ring_latest(&r);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT64(42u, l->t_frame);
    TEST_ASSERT_EQUAL_UINT(5u, l->attn);
    TEST_ASSERT_EQUAL_UINT(100u, l->divider);
}

/** @brief Po naplnění kapacity zůstává count na kapacitě a oldest se přepisuje. */
static void test_ring_wrap_overwrites_oldest(void) {
    test_psg_ring_t r;
    memset(&r, 0, sizeof(r));
    /* Push capacity + 5 vzorků s rostoucím t_frame. */
    for (unsigned i = 0; i < TEST_RING_CAPACITY + 5u; ++i) {
        test_psg_sample_t s = { .t_frame = i, .attn = 0, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
        test_ring_push(&r, &s);
    }
    /* Count nesmí překročit kapacitu. */
    TEST_ASSERT_EQUAL_UINT(TEST_RING_CAPACITY, r.count);
    /* Latest musí být poslední pushnutý (= t_frame = capacity + 4). */
    const test_psg_sample_t *l = test_ring_latest(&r);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT64(TEST_RING_CAPACITY + 4u, l->t_frame);
}


/* ========================================================================
 * Note event detector testy
 * ======================================================================== */

/** @brief Klasický pattern attn=15 -> 5 -> 15 generuje právě 1 event. */
static void test_note_classic_on_off(void) {
    test_events_t ev;
    memset(&ev, 0, sizeof(ev));

    test_psg_sample_t silent_a = { .t_frame = 10, .attn = 15, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t playing  = { .t_frame = 20, .attn = 5,  .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t silent_b = { .t_frame = 50, .attn = 15, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };

    test_advance(&ev, NULL,       &silent_a, 0, 0);  /* silent stays silent */
    test_advance(&ev, &silent_a,  &playing,  0, 0);  /* note_on */
    test_advance(&ev, &playing,   &silent_b, 0, 0);  /* note_off */

    TEST_ASSERT_FALSE(ev.note_active);
    TEST_ASSERT_EQUAL_UINT(1u, ev.count);
    TEST_ASSERT_EQUAL_UINT64(20u, ev.events[0].t_on);
    TEST_ASSERT_EQUAL_UINT64(50u, ev.events[0].t_off);
}

/** @brief Drobný cents drift ve stejném MIDI semitonu - jedna nota, žádný split.
 *
 * V1.5 pitch change detection rozdělí notu jen pokud nový divider odpovídá
 * jinému MIDI integer pitch. Drobné mikroladění (divider posun o 1 hodnotu)
 * typicky zůstane ve stejném semitonu (= cents drift uvnitř ±50 cents).
 *
 * divider=252: f = 17721600/(32*252*5) ~ 439.40 Hz, MIDI ~ 68.98 -> nearest 69 (A4)
 * divider=253: f = 17721600/(32*253*5) ~ 437.66 Hz, MIDI ~ 68.91 -> nearest 69 (A4)
 *
 * Oba odpovídají MIDI A4 -> jedna sustained nota, jen vibrato/drift uvnitř.
 */
static void test_pitch_drift_no_split(void) {
    test_events_t ev;
    memset(&ev, 0, sizeof(ev));

    test_psg_sample_t silent  = { .t_frame =  0, .attn = 15, .divider = 252, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t a       = { .t_frame = 10, .attn =  5, .divider = 252, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t b       = { .t_frame = 20, .attn =  5, .divider = 253, .channel_type = TEST_PSG_CHTYPE_TONE };

    test_advance(&ev, NULL,    &silent, 0, 0);
    test_advance(&ev, &silent, &a,      0, 0);  /* note_on s pitch A4 (divider 252) */
    test_advance(&ev, &a,      &b,      0, 0);  /* drift - stále A4 (divider 253) */

    TEST_ASSERT_TRUE(ev.note_active);
    TEST_ASSERT_EQUAL_UINT(0u, ev.count);  /* žádný uzavřený event */
    TEST_ASSERT_EQUAL_UINT64(10u, ev.active_note.t_on);
    TEST_ASSERT_EQUAL_INT(69, ev.active_note.midi_pitch);
}

/** @brief Změna divideru na jiný MIDI integer pitch během noty - split na 2 noty.
 *
 * Reprodukuje Flappy bass scenario: V1 detekovala note_off jen na attn 15
 * přechodu, takže melodie která mění divider mid-note bez attn resetu se
 * exportovala jako jedna velmi dlouhá sustained nota. V1.5 rozdělí notu.
 *
 * divider=213: f = 17721600/(32*213*5) ~ 519.85 Hz, MIDI = round(71.89) = 72 (C5)
 * divider=169: f = 17721600/(32*169*5) ~ 654.95 Hz, MIDI = round(75.79) = 76 (E5)
 *
 * Jiný MIDI integer -> note_off C5 + note_on E5, dvě uzavřené noty po
 * celkovém sledu (= druhá v active_note dokud nepřijde note_off).
 */
static void test_pitch_change_split(void) {
    test_events_t ev;
    memset(&ev, 0, sizeof(ev));

    test_psg_sample_t silent  = { .t_frame =  0, .attn = 15, .divider = 213, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t c5      = { .t_frame = 10, .attn =  5, .divider = 213, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t e5      = { .t_frame = 20, .attn =  5, .divider = 169, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t end     = { .t_frame = 30, .attn = 15, .divider = 169, .channel_type = TEST_PSG_CHTYPE_TONE };

    test_advance(&ev, NULL,    &silent, 0, 0);
    test_advance(&ev, &silent, &c5,     0, 0);  /* note_on C5 */
    test_advance(&ev, &c5,     &e5,     0, 0);  /* pitch change: split, note_on E5 */
    test_advance(&ev, &e5,     &end,    0, 0);  /* note_off (silence) */

    TEST_ASSERT_FALSE(ev.note_active);
    TEST_ASSERT_EQUAL_UINT(2u, ev.count);  /* dvě uzavřené noty */
    /* První nota: C5 (pitch 72), t_on=10, t_off=20 (split bod). */
    TEST_ASSERT_EQUAL_INT(72, ev.events[0].midi_pitch);
    TEST_ASSERT_EQUAL_UINT64(10u, ev.events[0].t_on);
    TEST_ASSERT_EQUAL_UINT64(20u, ev.events[0].t_off);
    /* Druhá nota: E5 (pitch 76), t_on=20, t_off=30. */
    TEST_ASSERT_EQUAL_INT(76, ev.events[1].midi_pitch);
    TEST_ASSERT_EQUAL_UINT64(20u, ev.events[1].t_on);
    TEST_ASSERT_EQUAL_UINT64(30u, ev.events[1].t_off);
}

/** @brief Změna divideru na DC (< 2) uprostřed hraní uzavře notu. */
static void test_note_dc_silences(void) {
    test_events_t ev;
    memset(&ev, 0, sizeof(ev));

    test_psg_sample_t silent    = { .t_frame =  0, .attn = 15, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t playing   = { .t_frame = 10, .attn =  5, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t dc_silent = { .t_frame = 30, .attn =  5, .divider =   1, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t playing2  = { .t_frame = 40, .attn =  5, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };

    test_advance(&ev, NULL,    &silent,    0, 0);
    test_advance(&ev, &silent, &playing,   0, 0);  /* on */
    test_advance(&ev, &playing, &dc_silent, 0, 0); /* off (DC = silent) */
    test_advance(&ev, &dc_silent, &playing2, 0, 0); /* nová note_on */

    TEST_ASSERT_TRUE(ev.note_active);
    TEST_ASSERT_EQUAL_UINT(1u, ev.count);  /* jeden uzavřený event */
    TEST_ASSERT_EQUAL_UINT64(10u, ev.events[0].t_on);
    TEST_ASSERT_EQUAL_UINT64(30u, ev.events[0].t_off);
    TEST_ASSERT_EQUAL_UINT64(40u, ev.active_note.t_on);
}

/** @brief Type swap TONE -> NOISE uprostřed hraní uzavře a otevře novou notu. */
static void test_note_type_swap_splits(void) {
    test_events_t ev;
    memset(&ev, 0, sizeof(ev));

    test_psg_sample_t silent  = { .t_frame =  0, .attn = 15, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t tone    = { .t_frame = 10, .attn =  5, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t noise   = { .t_frame = 20, .attn =  5, .divider = 100, .channel_type = TEST_PSG_CHTYPE_NOISE };

    test_advance(&ev, NULL,    &silent, 0, 3);
    test_advance(&ev, &silent, &tone,   0, 3);  /* TONE note_on */
    test_advance(&ev, &tone,   &noise,  0, 3);  /* type swap = off + on */

    TEST_ASSERT_TRUE(ev.note_active);
    TEST_ASSERT_EQUAL_UINT(1u, ev.count);
    TEST_ASSERT_EQUAL_INT(TEST_PSG_CHTYPE_TONE, ev.events[0].channel_type);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ev.events[0].midi_pitch);  /* TONE má pitch */
    TEST_ASSERT_EQUAL_INT(TEST_PSG_CHTYPE_NOISE, ev.active_note.channel_type);
    TEST_ASSERT_EQUAL_INT(-1, ev.active_note.midi_pitch);          /* NOISE bez pitch */
}


/* ========================================================================
 * MIDI pitch detection
 * ======================================================================== */

/** @brief Divider 1 = DC, vrací pitch=0, cents=0; divider 0 se chová jako 1024. */
static void test_midi_dc_returns_zero(void) {
    int pitch = -123, cents = -123;
    int pitch_1024 = -1, cents_1024 = -1;
    test_tone_to_midi(0u, &pitch, &cents);
    test_tone_to_midi(1024u, &pitch_1024, &cents_1024);
    TEST_ASSERT_EQUAL_INT(pitch_1024, pitch);
    TEST_ASSERT_EQUAL_INT(cents_1024, cents);
    TEST_ASSERT_EQUAL_INT(45, pitch);   /* 108,2 Hz na MZ-800 = A2 */

    test_tone_to_midi(1u, &pitch, &cents);
    TEST_ASSERT_EQUAL_INT(0, pitch);
    TEST_ASSERT_EQUAL_INT(0, cents);
}

/** @brief Divider odpovídající A4 (440 Hz) má dát MIDI 69 + cents blízko 0.
 *
 * freq = GDGCLK_BASE / (32 * divider * GDGCLK2CPU_DIVIDER) = 440
 * => divider = GDGCLK_BASE / (32 * 440 * 5) = 17721600 / 70400 ~= 251.7
 * Nejbližší integer = 252 → freq = 17721600 / (32 * 252 * 5) = 439.40 Hz
 *   → midi_float = 12 * log2(439.40/440) + 69 = 68.9763
 *   → nearest = 69, cents = round((68.9763 - 69) * 100) = -2.
 *
 * Pro 10-bit divider hodnoty (max 1023) je toto jediný integer divider
 * který dá MIDI 69 v MZ-800 doméně (po fixu PSG frequency formule).
 */
static void test_midi_a4_pitch(void) {
    unsigned divider = (unsigned)lround(
        TEST_GDGCLK_BASE / (32.0 * 440.0 * TEST_GDGCLK2CPU_DIV));
    int pitch = 0, cents = 0;
    test_tone_to_midi(divider, &pitch, &cents);
    TEST_ASSERT_EQUAL_INT(69, pitch);
    /* Cents mohou být nepatrně nenulové kvůli zaokrouhlení divideru. */
    TEST_ASSERT_INT_WITHIN(5, 0, cents);
}

/** @brief Pitch klesá s rostoucím dividerem (= nižší frekvence = nižší MIDI nota). */
static void test_midi_pitch_monotonic(void) {
    int p_low = 0, p_high = 0, c = 0;
    /* Vyšší frekvence (menší divider) -> vyšší pitch. */
    test_tone_to_midi(100u, &p_low, &c);
    test_tone_to_midi(500u, &p_high, &c);
    TEST_ASSERT_GREATER_THAN_INT(p_high, p_low);
}

/** @brief Cents detune je v rozsahu -50..+50. */
static void test_midi_cents_clamp(void) {
    int pitch = 0, cents = 0;
    /* Sweep dividery, ověř že cents nikdy nepřesáhne rozsah. */
    for (unsigned d = 2u; d < 1024u; d += 7u) {
        test_tone_to_midi(d, &pitch, &cents);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(-50, cents);
        TEST_ASSERT_LESS_OR_EQUAL_INT(50, cents);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pitch);
        TEST_ASSERT_LESS_OR_EQUAL_INT(127, pitch);
    }
}


/* ========================================================================
 * Velocity z attenuace
 * ======================================================================== */

/** @brief Velocity mapping attn=0 -> 127, attn=15 -> 0. */
static void test_velocity_mapping(void) {
    test_events_t ev;
    memset(&ev, 0, sizeof(ev));
    test_psg_sample_t silent  = { .t_frame =  0, .attn = 15, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t loud    = { .t_frame = 10, .attn =  0, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_advance(&ev, NULL,    &silent, 0, 0);
    test_advance(&ev, &silent, &loud,   0, 0);
    TEST_ASSERT_EQUAL_UINT(127u, ev.active_note.velocity);
}

/** @brief Velocity uprostřed (attn=8) je v rozumném rozsahu. */
static void test_velocity_mid(void) {
    test_events_t ev;
    memset(&ev, 0, sizeof(ev));
    test_psg_sample_t silent  = { .t_frame =  0, .attn = 15, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_psg_sample_t mid     = { .t_frame = 10, .attn =  8, .divider = 100, .channel_type = TEST_PSG_CHTYPE_TONE };
    test_advance(&ev, NULL,    &silent, 0, 0);
    test_advance(&ev, &silent, &mid,    0, 0);
    /* v = 127 * (1 - 8/15) = 127 * 7/15 ~= 59 */
    TEST_ASSERT_INT_WITHIN(2, 59, (int)ev.active_note.velocity);
}


/* ========================================================================
 * MIDI tick konverze
 * ======================================================================== */

/** @brief 0 frames = 0 ticks bez ohledu na tempo. */
static void test_ticks_zero(void) {
    TEST_ASSERT_EQUAL_UINT32(0u, test_frames_to_ticks(0u, 120));
    TEST_ASSERT_EQUAL_UINT32(0u, test_frames_to_ticks(0u, 60));
}

/** @brief GDGCLK_BASE pxCLK ticks = 1 sekunda; 120 BPM = 2 čtvrťové = 960 MIDI ticks.
 *
 * Po fixu monitor-rate dependency je `frames` v pxCLK timebase (17.7345 MHz pro
 * MZ-800). Test používá `TEST_FRAMES_PER_SECOND = GDGCLK_REAL_BASE` jako pxCLK/s.
 */
static void test_ticks_one_second_at_120bpm(void) {
    /* 1 s = 17734475 pxCLK ticks, 120 BPM = 2 quarters/s, PPQN=480 -> 960 MIDI ticks */
    TEST_ASSERT_EQUAL_UINT32(960u, test_frames_to_ticks(17734475u, 120));
}

/** @brief 1 s @ 60 BPM = 1 quarter = 480 ticks (PPQN). */
static void test_ticks_one_second_at_60bpm(void) {
    TEST_ASSERT_EQUAL_UINT32(480u, test_frames_to_ticks(17734475u, 60));
}

/** @brief Tempo <= 0 fallbackne na 120 BPM (defensivní). */
static void test_ticks_zero_tempo_fallback(void) {
    /* 1 s @ 0 BPM (-> 120 BPM default) = 960 ticks */
    TEST_ASSERT_EQUAL_UINT32(960u, test_frames_to_ticks(17734475u, 0));
    TEST_ASSERT_EQUAL_UINT32(960u, test_frames_to_ticks(17734475u, -1));
}


/* ========================================================================
 * MIDI VLQ encoding (MIDI 1.0 spec, hodnoty z normy)
 * ======================================================================== */

/** @brief VLQ kontrolní vektory dle MIDI 1.0 spec. */
static void test_vlq_known_values(void) {
    uint8_t buf[4];
    int n;

    /* 0 -> 00 (1 byte) */
    n = test_midi_write_vlq(buf, 0u);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[0]);

    /* 127 -> 7F (1 byte, hraniční hodnota single byte) */
    n = test_midi_write_vlq(buf, 127u);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x7F, buf[0]);

    /* 128 -> 81 00 (2 bytes, první multi-byte hodnota) */
    n = test_midi_write_vlq(buf, 128u);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x81, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[1]);

    /* 16383 -> FF 7F (2 bytes, hraniční 2B) */
    n = test_midi_write_vlq(buf, 16383u);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, buf[1]);

    /* 16384 -> 81 80 00 (3 bytes) */
    n = test_midi_write_vlq(buf, 16384u);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x81, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[2]);

    /* 2097151 -> FF FF 7F (3 bytes, hraniční 3B) */
    n = test_midi_write_vlq(buf, 2097151u);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, buf[2]);
}

/** @brief Continuation bity - MSB=1 pro non-poslední, MSB=0 pro poslední. */
static void test_vlq_continuation_bits(void) {
    uint8_t buf[4];
    /* Test 0x200000 (= 2^21, 4 byte VLQ). */
    int n = test_midi_write_vlq(buf, 0x200000u);
    TEST_ASSERT_EQUAL_INT(4, n);
    /* První 3 bajty MSB=1, poslední MSB=0. */
    TEST_ASSERT_TRUE_MESSAGE((buf[0] & 0x80u) != 0u, "byte 0 must have MSB=1");
    TEST_ASSERT_TRUE_MESSAGE((buf[1] & 0x80u) != 0u, "byte 1 must have MSB=1");
    TEST_ASSERT_TRUE_MESSAGE((buf[2] & 0x80u) != 0u, "byte 2 must have MSB=1");
    TEST_ASSERT_TRUE_MESSAGE((buf[3] & 0x80u) == 0u, "byte 3 must have MSB=0");
}


/* ========================================================================
 * MIDI časová normalizace (B1 fix - silent playback root cause)
 * ======================================================================== */

/**
 * @brief Normalizace t_on/t_off odečtem t_origin = nejranější t_on.
 *
 * Před fixem: t_on byl absolutní frame counter (např. 753712 ticks),
 * MediaPlayer čekal na první notu = tichý playback.
 * Po fixu: t_on - t_origin = první nota začíná na ticku 0.
 */
static void test_midi_time_normalization(void) {
    /* Mock 3 noty s velkým absolute t_on (= simulace dlouho běžícího emu).
     * Hodnoty v pxCLK ticks (gdg timebase). 10 sekund pxCLK = 177344750 ticks. */
    const uint64_t one_sec_pxclk = 17734475ULL;
    uint64_t t_on_abs[3]  = { one_sec_pxclk * 10u,
                              one_sec_pxclk * 11u,
                              one_sec_pxclk * 12u };
    uint64_t t_off_abs[3] = { one_sec_pxclk * 10u + one_sec_pxclk / 2u,
                              one_sec_pxclk * 11u + one_sec_pxclk / 2u,
                              one_sec_pxclk * 12u + one_sec_pxclk / 2u };

    /* Najdi t_origin. */
    uint64_t t_origin = t_on_abs[0];
    for (int i = 1; i < 3; ++i)
        if (t_on_abs[i] < t_origin) t_origin = t_on_abs[i];

    /* Po normalizaci první nota = tick 0. */
    uint64_t t_on_rel_0 = t_on_abs[0] - t_origin;
    TEST_ASSERT_EQUAL_UINT64(0u, t_on_rel_0);

    /* Delta mezi notami zachována. */
    uint64_t delta_01 = t_on_abs[1] - t_on_abs[0];
    uint64_t delta_01_rel = (t_on_abs[1] - t_origin) - (t_on_abs[0] - t_origin);
    TEST_ASSERT_EQUAL_UINT64(delta_01, delta_01_rel);

    /* Konverze první noty do MIDI ticků = 0 (= player nezahrabe). */
    uint32_t first_ticks = test_frames_to_ticks(t_on_rel_0, 120);
    TEST_ASSERT_EQUAL_UINT32(0u, first_ticks);

    /* Song end = last t_off - t_origin (= conductor track EoT).
     * 2.5 s pxCLK od t_origin @ 120 BPM = 4 quarters/s, PPQN=480
     * = 2.5 * 4 * 480 = 4800 MIDI ticks. */
    uint64_t t_end = t_off_abs[2];
    uint64_t song_frames = t_end - t_origin;
    uint32_t song_end_tick = test_frames_to_ticks(song_frames, 120);
    TEST_ASSERT_TRUE_MESSAGE(song_end_tick > 0u,
        "conductor EoT must be > 0 (else MediaPlayer stops on tick 0)");
}


/**
 * @brief Regression: pxCLK timebase invariant pro tempo metadata.
 *
 * Před fixem monitor-rate dependency byl `t_frame` UI-render counter
 * dělený konstantou 60 Hz. Na 144 Hz monitoru se duration každé noty
 * inflated 2.4x -> tempo metadata BPM bylo 2.4x menší (= MIDI player
 * hrál skladbu výrazně pomaleji než PSG).
 *
 * Po fixu je `t_frame` v pxCLK timebase (gdg_get_total_ticks(), 17.7345 MHz),
 * derivace sekund je `frames / GDGCLK_BASE`. Konverze je deterministicky
 * vázána na emu clock a NE na UI refresh rate.
 *
 * Test ověřuje: 0.5 s reálného času (= half of GDGCLK_BASE pxCLK ticks)
 * při 120 BPM = 1 quarter = 480 MIDI ticks (PPQN).
 */
static void test_midi_pxclk_timebase_invariant(void) {
    /* 0.5 s = GDGCLK_BASE/2 pxCLK ticks (= cca 8867237 ticks pro MZ-800). */
    uint64_t half_sec_pxclk = (uint64_t)(TEST_FRAMES_PER_SECOND / 2.0);

    /* 0.5 s @ 120 BPM = 2 quarter/s, 0.5 s = 1 quarter = 480 ticks. */
    uint32_t ticks_120 = test_frames_to_ticks(half_sec_pxclk, 120);
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 480, (int)ticks_120,
        "0.5 s pxCLK @ 120 BPM must produce 480 MIDI ticks (PPQN)");

    /* 2.0 s @ 60 BPM = 1 quarter/s = 2 quarters = 960 ticks. */
    uint64_t two_sec_pxclk = (uint64_t)(TEST_FRAMES_PER_SECOND * 2.0);
    uint32_t ticks_60 = test_frames_to_ticks(two_sec_pxclk, 60);
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 960, (int)ticks_60,
        "2 s pxCLK @ 60 BPM must produce 960 MIDI ticks");

    /* Konzistence: 1 s pxCLK = 17734475 ticks (assert konstanty). */
    TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(17734475.0, TEST_FRAMES_PER_SECOND,
        "TEST_FRAMES_PER_SECOND must equal GDGCLK_REAL_BASE (17734475 Hz)");
}


/* ========================================================================
 * MIDI Program Change (Bug #2 fix - Windows MediaPlayer silent playback)
 * ======================================================================== */

/**
 * @brief GM patch 80 = Lead 1 (Square wave) pro TONE tracky.
 *
 * Paritní k makru `PSG_MIDI_TONE_PROGRAM` v psg_audio_scope_window.cpp.
 * Důvod: Acoustic Grand Piano (default GM patch 0) má piano range
 * A0..C8 (MIDI pitch 21..108). PSG produkuje typicky pitch 100..121
 * (E7..C#9), patch 0 vysoké tóny neumí -> ticho. Square Lead má plný
 * rozsah a je nejvěrnější PSG square wave generátoru.
 */
#define TEST_PSG_MIDI_TONE_PROGRAM 80
#define TEST_PSG_MIDI_DRUM_CHANNEL 9

/**
 * @brief Sestaví Program Change bytes pro daný MIDI channel.
 *
 * Paritní k inline kódu v psg_scope_export_midi() - per-channel track
 * vloží mezi track-name meta event a první note on:
 *   - delta = 0 (VLQ 1B)
 *   - status = 0xC0 | (ch & 0x0F)
 *   - program = TEST_PSG_MIDI_TONE_PROGRAM (80)
 *
 * Celkem 3 bajty.
 *
 * @param buf  Output buffer (alespoň 3 bytes).
 * @param ch   MIDI channel 0..15.
 * @return Počet zapsaných bytů (3).
 */
static int test_build_program_change(uint8_t *buf, uint8_t ch) {
    buf[0] = 0x00;                              /* delta=0 (VLQ 1B) */
    buf[1] = (uint8_t)(0xC0u | (ch & 0x0Fu));   /* Program Change status */
    buf[2] = (uint8_t)TEST_PSG_MIDI_TONE_PROGRAM;
    return 3;
}

/** @brief PC bytes pro CH0 = 00 C0 50 (Square Lead). */
static void test_pc_ch0_bytes(void) {
    uint8_t buf[4] = {0};
    int n = test_build_program_change(buf, 0u);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC0, buf[1]);  /* C0 = Program Change, channel 0 */
    TEST_ASSERT_EQUAL_HEX8(0x50, buf[2]);  /* 0x50 = 80 = Lead 1 (Square wave) */
}

/** @brief PC bytes pro CH3 = 00 C3 50. */
static void test_pc_ch3_bytes(void) {
    uint8_t buf[4] = {0};
    int n = test_build_program_change(buf, 3u);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC3, buf[1]);  /* C3 = Program Change, channel 3 */
    TEST_ASSERT_EQUAL_HEX8(0x50, buf[2]);
}

/**
 * @brief PC nikdy nemíří na drum channel 9 - drum events status nibble
 * 0x9 detekce vynechá PC pro daný track.
 *
 * Heuristika v psg_scope_export_midi: PC se posílá pouze pokud track
 * obsahuje aspoň jeden Note On event s low nibble != 9 (drum channel).
 * Track čistě s NOISE eventy (všechny status 0x99) PC nedostane.
 */
static void test_pc_skip_drum_only_track(void) {
    /* Simulace evs s NOISE-only eventy (vše na drum channel 9). */
    uint8_t drum_note_on_status = (uint8_t)(0x90u | TEST_PSG_MIDI_DRUM_CHANNEL);
    TEST_ASSERT_EQUAL_HEX8(0x99, drum_note_on_status);

    /* Status nibble = 0x9 && low nibble == 9 -> drum event, NOT tone. */
    bool has_tone_event = false;
    uint8_t evs_status[3] = { 0x99, 0x99, 0x99 };
    for (int i = 0; i < 3; ++i) {
        uint8_t st = evs_status[i];
        if ((st & 0xF0u) == 0x90u && (st & 0x0Fu) != (uint8_t)TEST_PSG_MIDI_DRUM_CHANNEL) {
            has_tone_event = true;
            break;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(has_tone_event,
        "drum-only track must not receive Program Change");
}

/**
 * @brief PC posílá track s aspoň 1 tone event (CH3 MIX TONE+NOISE).
 *
 * CH3 v MIX režimu má v evs jak tone (status 0x93) tak drum (0x99)
 * eventy. Heuristika musí detekovat tone podčást a PC poslat - PC na
 * MIDI channel 3 ovlivní tone podčást, drum events na ch 9 zůstanou
 * perkusivní.
 */
static void test_pc_emit_mixed_tone_noise_track(void) {
    uint8_t evs_status[3] = { 0x93, 0x99, 0x83 };  /* tone on, drum on, tone off */
    bool has_tone_event = false;
    for (int i = 0; i < 3; ++i) {
        uint8_t st = evs_status[i];
        if ((st & 0xF0u) == 0x90u && (st & 0x0Fu) != (uint8_t)TEST_PSG_MIDI_DRUM_CHANNEL) {
            has_tone_event = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(has_tone_event,
        "mixed tone+noise track must receive Program Change");
}


/* ========================================================================
 * Volume envelope (attn_history) testy
 * ======================================================================== */

/** @brief Nová nota bez attn změn musí mít prázdnou history. */
static void test_attn_history_empty(void) {
    test_attn_state_t st;
    memset(&st, 0, sizeof(st));
    test_attn_state_note_on(&st, /* attn na note_on */ 4u);
    /* Žádné test_attn_append - simulace constant-volume noty. */
    TEST_ASSERT_EQUAL_UINT(0u, st.attn_history_count);
    TEST_ASSERT_FALSE(st.attn_history_overflowed);
}

/** @brief Tři po sobě jdoucí attn změny během noty -> count == 3 v
 *  chronologickém pořadí, žádný overflow. */
static void test_attn_history_capture(void) {
    test_attn_state_t st;
    memset(&st, 0, sizeof(st));
    test_attn_state_note_on(&st, /* attn na note_on */ 4u);

    /* 3 změny + duplicita (stejný attn = filtrováno) + attn=15 (filtrováno). */
    test_attn_append(&st, 100u, 6u);   /* +1 */
    test_attn_append(&st, 110u, 6u);   /* filtr (stejný attn) */
    test_attn_append(&st, 120u, 8u);   /* +1 */
    test_attn_append(&st, 130u, 15u);  /* filtr (attn=15) */
    test_attn_append(&st, 140u, 10u);  /* +1 */

    TEST_ASSERT_EQUAL_UINT(3u, st.attn_history_count);
    TEST_ASSERT_FALSE(st.attn_history_overflowed);
    TEST_ASSERT_EQUAL_UINT64(100u, st.attn_history[0].t_ticks);
    TEST_ASSERT_EQUAL_UINT(6u,    st.attn_history[0].attn);
    TEST_ASSERT_EQUAL_UINT64(120u, st.attn_history[1].t_ticks);
    TEST_ASSERT_EQUAL_UINT(8u,    st.attn_history[1].attn);
    TEST_ASSERT_EQUAL_UINT64(140u, st.attn_history[2].t_ticks);
    TEST_ASSERT_EQUAL_UINT(10u,   st.attn_history[2].attn);
}

/** @brief 40 změn překročí kapacitu - count clip, overflowed flag, head se
 *  točí. */
static void test_attn_history_overflow(void) {
    test_attn_state_t st;
    memset(&st, 0, sizeof(st));
    test_attn_state_note_on(&st, /* attn na note_on */ 0u);

    /* Push 40 attn changes (alternující 1<->2 aby nebyly filtrovány). */
    for (unsigned i = 0; i < 40u; ++i) {
        uint8_t a = (uint8_t)(1u + (i & 1u));
        test_attn_append(&st, 1000u + i, a);
    }

    TEST_ASSERT_EQUAL_UINT(TEST_MAX_ATTN_POINTS, st.attn_history_count);
    TEST_ASSERT_TRUE(st.attn_history_overflowed);
    /* Po overflow je head zase tam, kde byl po 40 modulo 32 = 8. */
    TEST_ASSERT_EQUAL_UINT(8u, st.attn_history_head);
}

/** @brief CC 7 value mapping musí být monotónní (attn 0 -> 127, attn 15 ->
 *  0, monotonně klesající). */
static void test_midi_cc7_export(void) {
    /* attn 0 = max volume = CC 7 hodnota 127. */
    TEST_ASSERT_EQUAL_INT(127, test_attn_to_cc7(0u));
    /* attn 15 = silence = CC 7 hodnota 0. */
    TEST_ASSERT_EQUAL_INT(0,   test_attn_to_cc7(15u));
    /* attn 7 ~= polovina -> ~68 (= round(127 * 8/15) = 68). */
    TEST_ASSERT_EQUAL_INT(68,  test_attn_to_cc7(7u));

    /* Monotonní klesání. */
    int prev = test_attn_to_cc7(0u);
    for (uint8_t a = 1; a <= 15; ++a) {
        int cur = test_attn_to_cc7(a);
        TEST_ASSERT_TRUE_MESSAGE(cur <= prev, "CC 7 musí monotonně klesat s attn");
        prev = cur;
    }
}


/* ========================================================================
 * main
 * ======================================================================== */

int main(void) {
    UNITY_BEGIN();

    /* Ring buffer */
    RUN_TEST(test_ring_empty_state);
    RUN_TEST(test_ring_single_push);
    RUN_TEST(test_ring_wrap_overwrites_oldest);

    /* Note event detector */
    RUN_TEST(test_note_classic_on_off);
    RUN_TEST(test_pitch_drift_no_split);
    RUN_TEST(test_pitch_change_split);
    RUN_TEST(test_note_dc_silences);
    RUN_TEST(test_note_type_swap_splits);

    /* MIDI pitch detection */
    RUN_TEST(test_midi_dc_returns_zero);
    RUN_TEST(test_midi_a4_pitch);
    RUN_TEST(test_midi_pitch_monotonic);
    RUN_TEST(test_midi_cents_clamp);

    /* Velocity */
    RUN_TEST(test_velocity_mapping);
    RUN_TEST(test_velocity_mid);

    /* MIDI tick konverze */
    RUN_TEST(test_ticks_zero);
    RUN_TEST(test_ticks_one_second_at_120bpm);
    RUN_TEST(test_ticks_one_second_at_60bpm);
    RUN_TEST(test_ticks_zero_tempo_fallback);

    /* MIDI VLQ encoding (MIDI 1.0 spec) */
    RUN_TEST(test_vlq_known_values);
    RUN_TEST(test_vlq_continuation_bits);

    /* MIDI časová normalizace (silent playback fix) */
    RUN_TEST(test_midi_time_normalization);
    RUN_TEST(test_midi_pxclk_timebase_invariant);

    /* MIDI Program Change (Bug #2 fix) */
    RUN_TEST(test_pc_ch0_bytes);
    RUN_TEST(test_pc_ch3_bytes);
    RUN_TEST(test_pc_skip_drum_only_track);
    RUN_TEST(test_pc_emit_mixed_tone_noise_track);

    /* Volume envelope tracking (attn_history + CC 7 export) */
    RUN_TEST(test_attn_history_empty);
    RUN_TEST(test_attn_history_capture);
    RUN_TEST(test_attn_history_overflow);
    RUN_TEST(test_midi_cc7_export);

    return UNITY_END();
}
