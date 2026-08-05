/**
 * @file psg_audio_scope_window.cpp
 * @brief Debugger: PSG Audio Scope F5 (ring buffer, scope, envelope, note events, piano roll, MIDI+CSV export).
 *
 * Sourozenec `psg_window.cpp` (= statický chip state). Audio Scope se
 * zaměřuje na **slyšitelný výstup** PSG SN76489: per-(chip, channel) ring
 * buffer minulých vzorků a responsive oscilloscope renderer.
 *
 * F1+F2 scope:
 *   - Datová struktura `st_PSG_SCOPE_RING` per (chip, channel) - 4 nebo 8
 *     ringů dle mono/stereo runtime detekce (`g_psg_module.stereo`).
 *   - `psg_audio_scope_tick()` - polling snapshot z mirror getterů
 *     (side-effect free), 1x per ImGui frame z UI threadu.
 *   - `imgui_psg_audio_scope_window()` - per-kanál responsive
 *     oscilloscope (šířka = `ImGui::GetContentRegionAvail().x - padding`,
 *     výška = `PSG_SCOPE_ROW_HEIGHT`), waveform render reuse logiky z
 *     `psg_window.cpp::render_channel_waveform` (TONE square 4 periody,
 *     NOISE pseudo-random).
 *   - F2 envelope timeline: druhý filled-rect graf pod scope (attn
 *     historie přes 10 s ring, inverted attn → bar height, aggregate
 *     min-attn pixel down-sampling).
 *   - F3 note event detector: state machine per (chip, channel) detekuje
 *     přechody silent ↔ playing v `psg_audio_scope_tick()`, zaznamenává
 *     note on/off do per-kanál events ring bufferu (1024 not kapacita).
 *     Pitch (TONE) přes `g_mzhal.gdgclk_base/(32*divider*g_mzhal.gdgclk2cpu_divider)` → MIDI 0..127 + cents
 *     -50..+50; NOISE má pitch=-1. Velocity z attn jako MIDI standard
 *     `round(127*(1-attn/15))`. Channel type swap během aktivní noty
 *     ukončí + případně začne novou.
 *
 * F4 piano roll: timeline view per (chip, channel) - vodorovné pruhy
 *   `(t_on..t_off, pitch)` v dolní collapsible sekci. Time range selector
 *   (10s / 30s / All), auto-fit pitch range podle obsahu events bufferu +
 *   active note, octave labels (C0..C8), per-channel + per-chip color tint,
 *   NOISE samostatný lane pod hlavním pitch rozsahem, hover tooltip s
 *   detailem (CH, chip, pitch name + cents, duration, velocity), aktivní
 *   nota se prodlužuje do aktuálního `g_psg_scope_frame_counter`.
 *
 * F5 export: toolbar tlačítka "Export MIDI..." + "Export CSV..." + Tempo
 *   BPM input, IGFD save dialog per typ. CSV header
 *   `time_s,channel,chip,pitch_midi,note_name,duration_s,velocity,cents`,
 *   `%.3f` via `g_ascii_formatd` (locale-safe `.` separator). MIDI =
 *   Standard MIDI File type 1, 480 PPQN, conductor track + per (chip,
 *   channel) tracky, tempo z UI, VLQ delta-time, NOISE → MIDI drum
 *   channel (= 0-based 9, pitch 38 Acoustic Snare). Při I/O chybě modal
 *   error popup. Per-channel TONE tracky dostávají hned za track name
 *   Program Change na GM patch 80 (Lead 1 - Square wave) - bez toho
 *   Windows MediaPlayer použije Acoustic Grand Piano (range A0..C8) a
 *   PSG vysoké pitche (E7..C#9) jsou neslyšitelné. Drum tracky (NOISE)
 *   PC nedostávají - drum channel má implicit drum kit.
 *
 * Pozdější F-fáze (mimo F5 scope):
 *   F6 = docs + i18n + tests.
 *
 * Post-polish (offline analýza): opt-in debug log do TSV souborů. Dva
 * nezávislé toggle v toolbaru:
 *   - "Log samples" = per-frame, per (chip, channel) raw mirror snapshot
 *     (~240-480 řádků/s při typickém UI render rate, ~100 MB/h),
 *   - "Log events" = state machine přechody (START / NOTE_ON / NOTE_OFF /
 *     TYPE_CHANGE / PITCH_CHANGE / PAUSE / RESUME), sparse.
 * Soubory s timestamp v názvu (`psg_scope_samples_YYYYMMDD_HHMMSS.tsv` a
 * obdobné pro events), TSV s headerem, `.` decimal separator. Cílí na
 * offline cross-correlation s IORQ logem + PSG MIDI exportem.
 *
 * Reference plánu: `emu-experiments/psg-audio-scope/README.md`.
 *
 * MZ-700 PSG nemá - TU je sdílené (mz_emucore), volající gatují okno
 * i tick runtime přes `g_mzhal.psg_count >= 1`.
 *
 * License: GPLv3.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"

#include "i18n.h"

#include "mzarch/mzcommon_config.h"

#include "ui-imgui/debugger/psg_audio_scope_window.h"


extern "C"
{
#include "hw-generic/psg/psg.h"
#include "emulator/mzarch/mzhal.h"
#include "hw-generic/gdg/gdg_state.h"
#include "emulator/emulator.h"   /* EMULATOR_TEST_PAUSED - gate samplingu v pauze */

    void imgui_psg_audio_scope_window(bool *p_open);
    void psg_audio_scope_tick(void);
    unsigned psg_audio_scope_total_notes(void);
}

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <algorithm>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

/* ===================================================================== */
/* Ring buffer per (chip, channel)                                        */
/* ===================================================================== */

/**
 * @brief Počet vzorků v ring bufferu per (chip, channel).
 *
 * 600 vzorků = ~10 s historie při typické UI polling frekvenci (~60 Hz). Volba je
 * kompromis mezi pamětí (8 ringů × 600 × 8 B = ~38 kB total stereo) a
 * délkou viditelné historie pro pozdější envelope / piano roll.
 */
#define PSG_SCOPE_RING_CAPACITY 600

/**
 * @brief Jeden vzorek PSG stavu pro pozdější analýzu (envelope, note events).
 *
 * Časový údaj `t_frame` je hodnota `gdg_get_total_ticks()` v okamžiku
 * snapshotu - tj. monotónní pxCLK time base (= 17.7345 MHz pro MZ-800).
 * Tato volba zaručuje, že derivace času v sekundách (`t_frame / g_mzhal.gdgclk_base`)
 * odpovídá reálnému emu času bez ohledu na frekvenci UI render loopu
 * (60 Hz / 75 Hz / 144 Hz monitor, vsync mode, atd.). Driftu sampling
 * rastr nepřímo ovlivňuje jen rozlišení snapshotů, ne časové známky.
 *
 * Invarianty:
 *  - `attn` v rozsahu 0..15.
 *  - `divider` v rozsahu 0..1023 (10-bit, irelevantní pro NOISE).
 *  - `channel_type` jeden z `en_PSG_CHTYPE` (TONE=0, NOISE=1).
 */
typedef struct st_PSG_SCOPE_SAMPLE
{
    uint64_t t_frame;     /**< pxCLK total ticks v okamžiku snapshotu (gdg timebase). */
    uint16_t divider;     /**< 10-bit tone divider (TONE), nepoužité pro NOISE. */
    uint8_t  attn;        /**< 4-bit attenuation 0..15 (0 = max, 15 = silent). */
    uint8_t  channel_type; /**< Hodnota `en_PSG_CHTYPE` (TONE / NOISE). */
} st_PSG_SCOPE_SAMPLE;

/**
 * @brief Ring buffer per (chip, channel).
 *
 * `head` ukazuje na **další** zápisovou pozici (= modulo arithmetic).
 * Při `count < capacity` se buffer plní, jakmile dosáhne kapacity,
 * nejstarší vzorky se přepisují (oldest dropped). `count` po dosažení
 * kapacity zůstává `PSG_SCOPE_RING_CAPACITY`.
 *
 * Lifetime: globální statické instance v tomto překladu, nealokuje se
 * dynamicky.
 */
typedef struct st_PSG_SCOPE_RING
{
    st_PSG_SCOPE_SAMPLE samples[PSG_SCOPE_RING_CAPACITY];
    unsigned head;   /**< Index dalšího zápisu (0..PSG_SCOPE_RING_CAPACITY-1). */
    unsigned count;  /**< Aktuální naplnění (0..PSG_SCOPE_RING_CAPACITY). */
} st_PSG_SCOPE_RING;

/**
 * @brief Globální ring buffery pro všechny (chip, channel) kombinace.
 *
 * `g_psg_scope[chip][channel]`:
 *  - chip 0 = mono PSG (MZ-800) nebo levý kanál (MZ-1500 stereo),
 *  - chip 1 = pravý kanál (MZ-1500 stereo); pro MZ-800 mono se nepoužívá.
 *
 * V mono režimu se zapisuje jen do `g_psg_scope[0][*]`. Stereo detekce
 * běhěm tick callbacku přes `g_psg_module.stereo`.
 */
static st_PSG_SCOPE_RING g_psg_scope[PSG_MAX_COUNT][PSG_CHANNELS_COUNT];

/**
 * @brief Aktuální pxCLK timestamp (= gdg_get_total_ticks() snapshot).
 *
 * Aktualizuje se v `psg_audio_scope_tick()` z `gdg_get_total_ticks()`. Slouží
 * jako monotonní pxCLK časová známka pro envelope / piano roll renderery a
 * pro derivaci doby v sekundách (`t / g_mzhal.gdgclk_base`). Hodnota neroste lineárně
 * (delta mezi po sobě jdoucími tickny ~ g_mzhal.gdgclk_base / UI_render_hz).
 * Pauza emulátoru gate-uje update, takže paused interval nepřetváří t_frame.
 *
 * Historický název `frame_counter` zachován kvůli inkrementální refaktorizaci -
 * sémantika se ale změnila z "UI render frame counter" na "pxCLK timestamp".
 */
static uint64_t g_psg_scope_frame_counter = 0;

/**
 * @brief Push jednoho vzorku do ring bufferu (ring overwrite semantics).
 *
 * Side effect: posune `head` a zvýší `count` (max-clamped na kapacitu).
 *
 * @param r        Ring buffer (NESMÍ být NULL).
 * @param sample   Vzorek k uložení.
 */
static inline void psg_scope_ring_push(st_PSG_SCOPE_RING *r,
                                       const st_PSG_SCOPE_SAMPLE *sample)
{
    r->samples[r->head] = *sample;
    r->head = (r->head + 1u) % PSG_SCOPE_RING_CAPACITY;
    if (r->count < PSG_SCOPE_RING_CAPACITY)
        r->count++;
}

/**
 * @brief Vrátí ukazatel na nejnovější (head-1) vzorek ringu, nebo NULL.
 *
 * @param r  Ring buffer (NESMÍ být NULL).
 * @return Ukazatel na poslední vzorek (vlastní r), nebo NULL pokud
 *         ring prázdný (count == 0).
 */
static inline const st_PSG_SCOPE_SAMPLE *psg_scope_ring_latest(const st_PSG_SCOPE_RING *r)
{
    if (r->count == 0u)
        return NULL;
    unsigned idx = (r->head + PSG_SCOPE_RING_CAPACITY - 1u) % PSG_SCOPE_RING_CAPACITY;
    return &r->samples[idx];
}

/* ===================================================================== */
/* Note event detector (F3) - per-(chip, channel) state machine            */
/* ===================================================================== */

/**
 * @brief Kapacita events ring bufferu per (chip, channel).
 *
 * 1024 not vystačí na cca 10-20 min hudby při tempu 120 BPM @ 4 noty/s.
 * Při překročení = oldest dropped (ring overwrite). Plánovaný F5 export
 * pošle aktuální obsah ringu (= ne všechno co kdy hrálo).
 */
#define PSG_SCOPE_EVENTS_CAPACITY 1024

/**
 * @brief Events ring buffer + state machine per (chip, channel).
 *
 * Drží uzavřené note eventy (`events[]`) i právě probíhající aktivní notu
 * (`active_note`, platná pokud `note_active == true`). State machine v
 * `psg_audio_scope_tick()` určuje přechody:
 *   - `!was_playing && is_playing` → note_on (active_note = new, note_active=true)
 *   - `was_playing && !is_playing` → note_off (push active_note, note_active=false)
 *   - channel type swap během aktivní noty → ukončit + start nová (pokud splňuje is_playing)
 *   - pitch change během aktivní TONE noty (V1.5): pokud nový divider
 *     odpovídá jinému MIDI integer pitch než starý → split (note_off
 *     staré + note_on s novým pitch). Drobný cents drift ve stejném
 *     semitonu se ignoruje. Volume modulation (attn během noty)
 *     se ignoruje - follow-up scope.
 *
 * `is_playing` def: `(attn < 15) AND (divider >= 2)` pro TONE,
 *                   `(attn < 15)` pro NOISE.
 *
 * Lifetime: globální statická instance per (chip, channel), nealokuje se
 * dynamicky.
 *
 * Invarianty:
 *  - `count <= PSG_SCOPE_EVENTS_CAPACITY`
 *  - `head` < kapacita
 *  - `active_note.t_off == 0` dokud není note_off (= ne validní)
 */
typedef struct st_PSG_SCOPE_EVENTS
{
    st_PSG_SCOPE_NOTE_EVENT events[PSG_SCOPE_EVENTS_CAPACITY];
    unsigned                head;          /**< Index dalšího zápisu. */
    unsigned                count;         /**< Aktuální naplnění 0..capacity. */
    bool                    note_active;   /**< true = běží note (čeká note_off). */
    st_PSG_SCOPE_NOTE_EVENT active_note;   /**< Probíhající nota (t_off vyplníme při note_off). */
} st_PSG_SCOPE_EVENTS;

/**
 * @brief Globální events ringy per (chip, channel).
 *
 * V mono režimu se používá jen `g_psg_events[0][*]`. Stereo detekce přes
 * `g_psg_module.stereo` ve stejném sampling tickem jako sample ring.
 */
static st_PSG_SCOPE_EVENTS g_psg_events[PSG_MAX_COUNT][PSG_CHANNELS_COUNT];

/**
 * @brief Push uzavřené noty do events ring bufferu (overwrite oldest).
 *
 * @param ev      Events buffer (NESMÍ být NULL).
 * @param note    Notu k zapsání (kompletně vyplněnou včetně `t_off`).
 */
static inline void psg_events_push(st_PSG_SCOPE_EVENTS *ev,
                                   const st_PSG_SCOPE_NOTE_EVENT *note)
{
    ev->events[ev->head] = *note;
    ev->head = (ev->head + 1u) % PSG_SCOPE_EVENTS_CAPACITY;
    if (ev->count < PSG_SCOPE_EVENTS_CAPACITY)
        ev->count++;
}

/**
 * @brief Frekvence v Hz pro TONE divider.
 *
 * Vzorec: `f = g_mzhal.gdgclk_base / (32 * divider * g_mzhal.gdgclk2cpu_divider)`,
 * ekvivalentní `CPU_CLOCK / (32 * divider)` (standardní SN76489 formule
 * s input clock = CPU clock + interní /16 prescaler + 2x toggle).
 *
 * Pro `divider < 2` vrací 0 (DC případ, PSG drží output_signal=1, není
 * to oscilace). `g_mzhal.gdgclk_base` a `g_mzhal.gdgclk2cpu_divider` jsou per-arch
 * makra: MZ-800 (17 721 600 Hz, /5 → 3.5469 MHz CPU),
 * MZ-700/MZ-1500 (14 336 640 Hz, /4 → 3.5796 MHz CPU).
 *
 * Odvození: `PSG_DIVIDER = 16 * g_mzhal.gdgclk2cpu_divider` GDG ticků mezi
 * `psg_step()` voláními (psg.h), per step se interní timer dekrementuje
 * o 1, toggle output při dosažení 0. Full period = 2 * divider * PSG_DIVIDER
 * GDG ticků → f_out = g_mzhal.gdgclk_base / (2 * divider * PSG_DIVIDER)
 * = g_mzhal.gdgclk_base / (32 * divider * g_mzhal.gdgclk2cpu_divider).
 *
 * Reference: shoda s `psg_window.cpp::psg_tone_frequency_hz`.
 *
 * @param divider  10-bit hodnota 0..1023.
 * @return Frekvence v Hz, 0 pro DC případ.
 */
static double psg_scope_tone_frequency_hz(unsigned divider)
{
    if (divider < 2u)
        return 0.0;
    /* Runtime z g_mzhal (mzhal 10b, cold - UI render). */
    return (double)g_mzhal.gdgclk_base / (32.0 * (double)divider * (double)g_mzhal.gdgclk2cpu_divider);
}

/**
 * @brief Detekuje, zda kanál právě "hraje" slyšitelný tón.
 *
 * Definice pro state machine: kanál hraje pokud
 *  - `attn < 15` (= attenuator nepuštěný do silent),
 *  - a pro TONE platí `divider >= 2` (DC případ = silent),
 *  - pro NOISE druhá podmínka neplatí (= NOISE generátor hraje vždy
 *    pokud není attn=15).
 *
 * @param s  Vzorek k vyhodnocení.
 * @return true = hraje, false = ticho.
 */
static inline bool psg_sample_is_playing(const st_PSG_SCOPE_SAMPLE *s)
{
    if (s->attn >= 15u)
        return false;
    if ((en_PSG_CHTYPE)s->channel_type == PSG_CHTYPE_TONE && s->divider < 2u)
        return false;
    return true;
}

/**
 * @brief Spočte MIDI pitch (0..127) + cents detune pro daný TONE divider.
 *
 * Postup:
 *   1) frekvence Hz = g_mzhal.gdgclk_base / (32 * divider * g_mzhal.gdgclk2cpu_divider)
 *   2) midi_float = 12 * log2(f/440) + 69
 *   3) nearest MIDI = round(midi_float), cents = round((midi_float - nearest) * 100)
 *   4) clamp MIDI do 0..127, cents do -50..+50
 *
 * Pro `divider < 2` (= DC) by se sem nemělo dostat (volá se jen pokud
 * `psg_sample_is_playing == true`), defenzivně vrací pitch=0, cents=0.
 *
 * @param divider     10-bit divider 0..1023.
 * @param out_pitch   Out: MIDI pitch 0..127 (NESMÍ být NULL).
 * @param out_cents   Out: cents -50..+50 (NESMÍ být NULL).
 */
static void psg_tone_to_midi(unsigned divider, int *out_pitch, int *out_cents)
{
    double f = psg_scope_tone_frequency_hz(divider);
    if (f <= 0.0)
    {
        *out_pitch = 0;
        *out_cents = 0;
        return;
    }
    double midi_float = 12.0 * log2(f / 440.0) + 69.0;
    int nearest = (int)lround(midi_float);
    int cents   = (int)lround((midi_float - (double)nearest) * 100.0);
    if (nearest < 0)   nearest = 0;
    if (nearest > 127) nearest = 127;
    if (cents < -50)   cents = -50;
    if (cents > 50)    cents = 50;
    *out_pitch = nearest;
    *out_cents = cents;
}

/**
 * @brief Spustí novou aktivní notu (note_on) podle aktuálního vzorku.
 *
 * Vyplní `ev->active_note` z `cur` a nastaví `ev->note_active = true`.
 * Velocity z attn jako MIDI standard: `v = round(127 * (1 - attn/15))`.
 * Pro NOISE pitch = -1, cents = 0.
 *
 * @param ev    Events struktura kanálu (NESMÍ být NULL).
 * @param cur   Aktuální vzorek (předpoklad: `psg_sample_is_playing(cur) == true`).
 * @param chip  Index chipu.
 * @param ch    Index kanálu.
 */
static void psg_events_note_on(st_PSG_SCOPE_EVENTS *ev,
                               const st_PSG_SCOPE_SAMPLE *cur,
                               unsigned chip, unsigned ch)
{
    st_PSG_SCOPE_NOTE_EVENT *n = &ev->active_note;
    n->t_on = cur->t_frame;
    n->t_off = 0u; /* vyplní note_off */
    if ((en_PSG_CHTYPE)cur->channel_type == PSG_CHTYPE_NOISE)
    {
        n->midi_pitch = -1;
        n->cents_detune = 0;
    }
    else
    {
        psg_tone_to_midi(cur->divider, &n->midi_pitch, &n->cents_detune);
    }
    double v = 127.0 * (1.0 - (double)cur->attn / 15.0);
    int vi = (int)lround(v);
    if (vi < 0)   vi = 0;
    if (vi > 127) vi = 127;
    n->velocity = (uint8_t)vi;
    n->channel  = (uint8_t)ch;
    n->chip     = (uint8_t)chip;
    n->reserved = 0u;

    /* Volume envelope tracking - inicializace na začátku noty.
     * Počáteční attn je implicitně přítomno přes `velocity`, takže
     * `attn_history` zde zůstává prázdná. `attn_last_seen` se použije
     * v `psg_events_append_attn_point` k detekci skutečné změny. */
    n->attn_history_count      = 0u;
    n->attn_history_head       = 0u;
    n->attn_history_overflowed = false;
    n->attn_last_seen          = cur->attn;

    ev->note_active = true;
}

/**
 * @brief Připojí attn change point do aktivní noty (volume envelope tracking).
 *
 * Volá se ze sampling ticku pokud:
 *  - `ev->note_active == true` (= běží nota),
 *  - aktuální `attn` se liší od `active_note.attn_last_seen`,
 *  - a `attn < 15` (= attn=15 je rezervováno pro note_off, ne přidávat
 *    jako envelope point).
 *
 * Při překročení kapacity `PSG_SCOPE_MAX_ATTN_POINTS` se nejstarší point
 * přepíše (ring overwrite) a `attn_history_overflowed` nastavuje true.
 *
 * Side effect: aktualizuje `active_note.attn_last_seen` na nově přidanou
 * hodnotu (= další stejné `attn` v dalším ticku se ignoruje).
 *
 * @param ev    Events struktura kanálu (NESMÍ být NULL, předpoklad: note_active).
 * @param cur   Aktuální vzorek (`t_frame` + `attn` použité).
 */
static void psg_events_append_attn_point(st_PSG_SCOPE_EVENTS *ev,
                                         const st_PSG_SCOPE_SAMPLE *cur)
{
    st_PSG_SCOPE_NOTE_EVENT *n = &ev->active_note;
    st_PSG_SCOPE_ATTN_POINT *p = &n->attn_history[n->attn_history_head];
    p->t_ticks = cur->t_frame;
    p->attn = cur->attn;
    p->reserved[0] = p->reserved[1] = p->reserved[2] = 0u;

    n->attn_history_head = (uint8_t)((n->attn_history_head + 1u) % PSG_SCOPE_MAX_ATTN_POINTS);
    if (n->attn_history_count < (uint8_t)PSG_SCOPE_MAX_ATTN_POINTS)
        n->attn_history_count++;
    else
        n->attn_history_overflowed = true;

    n->attn_last_seen = cur->attn;
}

/**
 * @brief Uzavře právě probíhající notu (note_off), zapíše do ringu.
 *
 * Použije `cur->t_frame` jako `t_off`. Pokud `ev->note_active == false`,
 * je no-op (= defensivní).
 *
 * @param ev   Events struktura kanálu (NESMÍ být NULL).
 * @param cur  Aktuální vzorek (use jen pro `t_frame`).
 */
static void psg_events_note_off(st_PSG_SCOPE_EVENTS *ev,
                                const st_PSG_SCOPE_SAMPLE *cur)
{
    if (!ev->note_active)
        return;
    ev->active_note.t_off = cur->t_frame;
    psg_events_push(ev, &ev->active_note);
    ev->note_active = false;
}

/* TODO: hystereze pokud potřebná - viz README "Note_off threshold".
 * V1 detekuje raw přechod is_playing == false; pokud melodie cyklí
 * attn 14↔15 pro vibrato/tremolo, mohlo by generovat falešné note_off.
 * Případný helper: per-(chip, ch) counter silent_streak, note_off až po
 * N=3 po sobě jdoucích silent vzorcích. */

unsigned psg_audio_scope_total_notes(void)
{
    unsigned sum = 0u;
    for (unsigned chip = 0; chip < PSG_MAX_COUNT; ++chip)
        for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
            sum += g_psg_events[chip][ch].count;
    return sum;
}

/**
 * @brief Vynuluje všechny buffery scope - sample ringy + events + counter.
 *
 * Použito Clear tlačítkem v toolbaru. Reset je úplný:
 *   - sample ring per (chip, channel) -> count=0, head=0
 *   - events ring per (chip, channel) -> count=0, head=0
 *   - aktivní nota se zruší (note_active=false)
 *   - frame counter resetnut na 0 (timeline začíná znovu)
 *
 * Po resetu se UI tick znovu spustí, nová data se začnou plnit od ticku 0.
 * Data jsou pouze v RAM, žádný persistent state se nevyhazuje.
 *
 * Side effects: zápis do globálních `g_psg_scope[][]`, `g_psg_events[][]`,
 * `g_psg_scope_frame_counter`. Voláno z UI vlákna.
 */
static void psg_audio_scope_clear_all(void)
{
    for (unsigned chip = 0; chip < PSG_MAX_COUNT; ++chip)
    {
        for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
        {
            g_psg_scope[chip][ch].head  = 0u;
            g_psg_scope[chip][ch].count = 0u;
            g_psg_events[chip][ch].head        = 0u;
            g_psg_events[chip][ch].count       = 0u;
            g_psg_events[chip][ch].note_active = false;
        }
    }
    g_psg_scope_frame_counter = 0u;
}

/* ===================================================================== */
/* Debug log - sample-level + event-level (post-polish offline analysis)  */
/* ===================================================================== */

/**
 * @brief TSV hlavička sample logu.
 *
 * Tab-separated, jeden řádek per (chip, channel, frame). Sloupce:
 *  - frame: pxCLK total ticks (= `g_psg_scope_frame_counter`, gdg timebase)
 *  - t_sec: čas v sekundách (frame / g_mzhal.gdgclk_base), `.` decimal separator (locale-safe)
 *  - chip: 0 nebo 1
 *  - channel: 0..3
 *  - attn: 0..15 raw z mirror
 *  - divider: 0..1023 raw z mirror (TONE), 0 pro NOISE
 *  - type: 0=TONE, 1=NOISE (en_PSG_CHTYPE)
 *  - is_playing: 0/1 dle state machine kritéria
 *  - freq_hz: vypočítaná frekvence v Hz (0 pokud silent nebo NOISE)
 *  - midi_pitch: -1 (NOISE/silent) nebo 0..127
 */
#define PSG_LOG_SAMPLES_HEADER \
    "frame\tt_sec\tchip\tchannel\tattn\tdivider\ttype\tis_playing\tfreq_hz\tmidi_pitch\n"

/**
 * @brief TSV hlavička event logu.
 *
 * Tab-separated, jeden řádek per state transition. Sloupce:
 *  - frame, t_sec: stejné jako sample log
 *  - chip, channel: 0..1, 0..3
 *  - event: START / NOTE_ON / NOTE_OFF / TYPE_CHANGE / PITCH_CHANGE / PAUSE / RESUME
 *  - pitch_or_attn: MIDI pitch (NOTE_ON / PITCH_CHANGE TONE), nebo "-" jinak
 *  - divider: divider hodnota (NOTE_ON / PITCH_CHANGE TONE), "-" jinak
 *  - type: 0/1 channel type pro NOTE_ON / PITCH_CHANGE / TYPE_CHANGE, "-" jinak
 *  - notes: volná diagnostická poznámka (cents, velocity, duration,
 *           prev_pitch / new_pitch u PITCH_CHANGE, ...)
 */
#define PSG_LOG_EVENTS_HEADER \
    "frame\tt_sec\tchip\tchannel\tevent\tpitch_or_attn\tdivider\ttype\tnotes\n"

/**
 * @brief Stav debug log subsystému (= UI toggle, file handles, counters).
 *
 * Lazy init: soubor se otevře při prvním enable checkboxu, nový timestamp
 * v názvu se generuje při každé re-enable transition. Při disable se file
 * handle uzavře, ale path se zachová pro tooltip / status display.
 *
 * Lifetime: globální statická instance v překladu, nealokuje se dynamicky.
 *
 * Invarianty:
 *  - `samples_fp != NULL` ⇒ soubor je otevřen pro append text mode
 *  - `samples_enabled == false` ⇒ `samples_fp == NULL` (po disable cleanup)
 *  - stejně pro events
 *  - `*_row_count` se inkrementuje při každém úspěšném zápisu řádku
 *
 * Thread: voláno z UI vlákna (= stejné jako tick()).
 */
typedef struct st_PSG_SCOPE_LOG
{
    bool      samples_enabled;       /**< UI toggle stav. */
    bool      events_enabled;        /**< UI toggle stav. */
    FILE     *samples_fp;            /**< NULL = uzavřeno / ještě neotevřeno. */
    FILE     *events_fp;             /**< NULL = uzavřeno / ještě neotevřeno. */
    char      samples_path[512];     /**< Cesta k aktuálnímu sample logu (UTF-8). */
    char      events_path[512];      /**< Cesta k aktuálnímu event logu (UTF-8). */
    unsigned  samples_row_count;     /**< Počet řádků zapsaných od posledního open. */
    unsigned  events_row_count;      /**< Počet řádků zapsaných od posledního open. */
    bool      was_paused_last_tick;  /**< Detekce pause/resume přechodu pro event log. */
} st_PSG_SCOPE_LOG;

/**
 * @brief Globální stav debug log subsystému.
 *
 * Inicializovaný na nuly (= oboje vypnuto, fp=NULL, prázdné path).
 */
static st_PSG_SCOPE_LOG g_psg_log = { false, false, NULL, NULL, "", "", 0u, 0u, false };

/**
 * @brief Naformátuje double s `.` decimal separatorem nezávisle na locale.
 *
 * Zabaluje `g_ascii_formatd` z GLib (= ekvivalent F5 CSV exportu). Buffer
 * musí mít kapacitu pro výsledek (typicky 32 B stačí pro %.3f).
 *
 * @param val   Hodnota k formátování.
 * @param buf   Cílový buffer (NESMÍ být NULL).
 * @param sz    Velikost bufferu.
 * @return     `buf` (= chainable do fprintf).
 */
static inline const char *psg_log_format_double(double val, char *buf, size_t sz)
{
    return g_ascii_formatd(buf, (int)sz, "%.3f", val);
}

/**
 * @brief Vygeneruje cestu logu s timestamp suffixem `psg_scope_<kind>_YYYYMMDD_HHMMSS.tsv`.
 *
 * Pracovní adresář = current working directory emulátoru (= stejně jako
 * F5 CSV/MIDI export). Pokud `g_get_current_time` neuspěje nebo formát
 * selže, fallback `<kind>.tsv` bez timestampu.
 *
 * @param kind     Identifikátor podtřídy logu (= "samples" / "events").
 * @param out      Cílový buffer (NESMÍ být NULL).
 * @param out_sz   Velikost bufferu (doporučeno >= 64).
 */
static void psg_log_make_path(const char *kind, char *out, size_t out_sz)
{
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char ts[32];
    if (strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf) == 0)
    {
        snprintf(out, out_sz, "psg_scope_%s.tsv", kind);
        return;
    }
    snprintf(out, out_sz, "psg_scope_%s_%s.tsv", kind, ts);
}

/**
 * @brief Lazy open log souboru s headerem.
 *
 * Volá se před prvním write. Pokud je `*fp != NULL`, no-op. Jinak vytvoří
 * cestu přes `psg_log_make_path`, otevře pro write (`"w"`), zapíše TSV
 * header a nastaví `*fp`. Při selhání ponechá `*fp == NULL` a path
 * vyplní pro diagnostiku.
 *
 * @param fp        Cíl - adresa file pointeru (NESMÍ být NULL).
 * @param path      Cíl - cesta (NESMÍ být NULL, kapacita >= 64).
 * @param path_sz   Velikost path bufferu.
 * @param kind      "samples" / "events" - vstup do `psg_log_make_path`.
 * @param header    TSV header string k zapsání (NESMÍ být NULL).
 * @return  true = soubor otevřen a header zapsán; false = open selhal.
 */
static bool psg_log_ensure_open(FILE **fp, char *path, size_t path_sz,
                                const char *kind, const char *header)
{
    if (*fp != NULL)
        return true;
    psg_log_make_path(kind, path, path_sz);
    /* g_fopen je MSYS2/Windows aware (= UTF-8 cesty fungují). */
    *fp = g_fopen(path, "w");
    if (*fp == NULL)
        return false;
    fputs(header, *fp);
    fflush(*fp);
    return true;
}

/**
 * @brief Uzavře log soubor (= disable transition).
 *
 * Pokud `*fp != NULL`, zavolá fclose + nastaví na NULL. Path se nemaže
 * (= zůstává viditelná v UI pro referenci posledního logu). Row count
 * se resetuje, aby další enable začal od nuly.
 *
 * @param fp         Cíl - adresa file pointeru (NESMÍ být NULL).
 * @param row_count  Cíl - row counter (NESMÍ být NULL).
 */
static void psg_log_close(FILE **fp, unsigned *row_count)
{
    if (*fp != NULL)
    {
        fclose(*fp);
        *fp = NULL;
    }
    *row_count = 0u;
}

/**
 * @brief Zapíše jeden řádek do sample logu pro daný (chip, channel) vzorek.
 *
 * No-op pokud `g_psg_log.samples_enabled == false`. Po prvním zápisu
 * v session otevře soubor (lazy). Per řádek volá fflush pro odolnost
 * proti crash emulátoru.
 *
 * Výpočet `is_playing` / `freq_hz` / `midi_pitch` se dělá tady (= ne v
 * `tick()` aby nezatěžovalo no-log hot path). Volá se po naplnění
 * sample struktury, ale NEZÁVISÍ na ring push (= bere data z `s`).
 *
 * @param chip  Index chipu 0..1.
 * @param ch    Index kanálu 0..3.
 * @param s     Vzorek (NESMÍ být NULL).
 */
static void psg_log_write_sample(unsigned chip, unsigned ch,
                                 const st_PSG_SCOPE_SAMPLE *s)
{
    if (!g_psg_log.samples_enabled)
        return;
    if (!psg_log_ensure_open(&g_psg_log.samples_fp,
                             g_psg_log.samples_path,
                             sizeof(g_psg_log.samples_path),
                             "samples",
                             PSG_LOG_SAMPLES_HEADER))
    {
        /* Open selhal - vypni aby se neopakoval try-open per frame. */
        g_psg_log.samples_enabled = false;
        return;
    }

    bool is_playing = psg_sample_is_playing(s);
    double freq_hz  = 0.0;
    int midi_pitch  = -1;
    if (is_playing && (en_PSG_CHTYPE)s->channel_type == PSG_CHTYPE_TONE)
    {
        freq_hz = psg_scope_tone_frequency_hz(s->divider);
        int cents_unused = 0;
        psg_tone_to_midi(s->divider, &midi_pitch, &cents_unused);
    }

    char t_buf[32], f_buf[32];
    /* t_frame je pxCLK total (gdg ticks); převod na sekundy = / g_mzhal.gdgclk_base. */
    psg_log_format_double((double)s->t_frame / (double)g_mzhal.gdgclk_base, t_buf, sizeof(t_buf));
    psg_log_format_double(freq_hz, f_buf, sizeof(f_buf));

    fprintf(g_psg_log.samples_fp,
            "%llu\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%d\n",
            (unsigned long long)s->t_frame,
            t_buf,
            chip, ch,
            (unsigned)s->attn,
            (unsigned)s->divider,
            (unsigned)s->channel_type,
            (unsigned)(is_playing ? 1u : 0u),
            f_buf,
            midi_pitch);
    fflush(g_psg_log.samples_fp);
    g_psg_log.samples_row_count++;
}

/**
 * @brief Zapíše jeden řádek do event logu.
 *
 * No-op pokud `g_psg_log.events_enabled == false`. Lazy open. Per řádek
 * fflush. Pole `event` je krátký identifikátor (START/NOTE_ON/...), `notes`
 * volná diagnostika.
 *
 * @param frame          Frame counter v okamžiku eventu.
 * @param chip           Index chipu (= 0 pokud globální event jako PAUSE).
 * @param ch             Index kanálu (= 0 pokud globální event jako PAUSE).
 * @param event_name     Krátký identifikátor (= "NOTE_ON", "PAUSE", ...).
 * @param pitch_or_attn  String "-" nebo decimal hodnota (= MIDI pitch).
 * @param divider        String "-" nebo decimal hodnota.
 * @param type           String "-" nebo "0"/"1" (TONE/NOISE).
 * @param notes          Volná diagnostická poznámka (může být "").
 */
static void psg_log_write_event(uint64_t frame, unsigned chip, unsigned ch,
                                const char *event_name,
                                const char *pitch_or_attn,
                                const char *divider,
                                const char *type,
                                const char *notes)
{
    if (!g_psg_log.events_enabled)
        return;
    if (!psg_log_ensure_open(&g_psg_log.events_fp,
                             g_psg_log.events_path,
                             sizeof(g_psg_log.events_path),
                             "events",
                             PSG_LOG_EVENTS_HEADER))
    {
        g_psg_log.events_enabled = false;
        return;
    }

    char t_buf[32];
    /* `frame` je pxCLK total (gdg ticks); převod na sekundy = / g_mzhal.gdgclk_base. */
    psg_log_format_double((double)frame / (double)g_mzhal.gdgclk_base, t_buf, sizeof(t_buf));

    fprintf(g_psg_log.events_fp,
            "%llu\t%s\t%u\t%u\t%s\t%s\t%s\t%s\t%s\n",
            (unsigned long long)frame,
            t_buf,
            chip, ch,
            event_name,
            pitch_or_attn,
            divider,
            type,
            notes);
    fflush(g_psg_log.events_fp);
    g_psg_log.events_row_count++;
}

/**
 * @brief Emit NOTE_ON event pro právě nastavenou aktivní notu.
 *
 * Volá se hned po `psg_events_note_on`. `ev->active_note` je platná
 * a obsahuje pitch + velocity + cents. Pro NOISE pitch="-" a `notes`
 * obsahuje "NOISE". Pro TONE pitch=MIDI + notes="cents=±N vel=M".
 */
static void psg_log_emit_note_on(const st_PSG_SCOPE_EVENTS *ev,
                                 const st_PSG_SCOPE_SAMPLE *s,
                                 unsigned chip, unsigned ch)
{
    if (!g_psg_log.events_enabled)
        return;
    const st_PSG_SCOPE_NOTE_EVENT *n = &ev->active_note;
    char pitch_buf[16], div_buf[16], type_buf[8], notes_buf[64];
    if (n->midi_pitch < 0)
    {
        snprintf(pitch_buf, sizeof(pitch_buf), "-");
        snprintf(div_buf,   sizeof(div_buf),   "-");
        snprintf(type_buf,  sizeof(type_buf),  "1"); /* NOISE */
        snprintf(notes_buf, sizeof(notes_buf), "NOISE vel=%u", (unsigned)n->velocity);
    }
    else
    {
        snprintf(pitch_buf, sizeof(pitch_buf), "%d", n->midi_pitch);
        snprintf(div_buf,   sizeof(div_buf),   "%u", (unsigned)s->divider);
        snprintf(type_buf,  sizeof(type_buf),  "0"); /* TONE */
        snprintf(notes_buf, sizeof(notes_buf), "cents=%+d vel=%u",
                 n->cents_detune, (unsigned)n->velocity);
    }
    psg_log_write_event(s->t_frame, chip, ch, "NOTE_ON",
                        pitch_buf, div_buf, type_buf, notes_buf);
}

/**
 * @brief Emit NOTE_OFF event pro právě uzavřenou notu.
 *
 * Volá se hned PŘED `psg_events_note_off` (= aby active_note byla ještě
 * platná pro získání t_on a duration). Duration ve frame jednotkách
 * převedeno na sekundy přes / g_mzhal.gdgclk_base (pxCLK timebase).
 */
static void psg_log_emit_note_off(const st_PSG_SCOPE_EVENTS *ev,
                                  const st_PSG_SCOPE_SAMPLE *s,
                                  unsigned chip, unsigned ch)
{
    if (!g_psg_log.events_enabled)
        return;
    if (!ev->note_active)
        return;
    char dur_buf[32], notes_buf[64];
    /* t_frame / t_on jsou pxCLK total ticks; sekundy = delta / g_mzhal.gdgclk_base. */
    double dur_s = (double)(s->t_frame - ev->active_note.t_on) / (double)g_mzhal.gdgclk_base;
    psg_log_format_double(dur_s, dur_buf, sizeof(dur_buf));
    snprintf(notes_buf, sizeof(notes_buf), "dur=%ss", dur_buf);
    psg_log_write_event(s->t_frame, chip, ch, "NOTE_OFF",
                        "-", "-", "-", notes_buf);
}

/**
 * @brief Emit TYPE_CHANGE event při swapu TONE↔NOISE během aktivní noty.
 *
 * Volá se mezi note_off a note_on dvojicí v `tick()` při type_changed
 * větvi. `s->channel_type` = nový typ; předchozí typ z prev sample
 * není dostupný v parametrech, takže notes obsahuje jen nový typ.
 */
static void psg_log_emit_type_change(const st_PSG_SCOPE_SAMPLE *prev,
                                     const st_PSG_SCOPE_SAMPLE *s,
                                     unsigned chip, unsigned ch)
{
    if (!g_psg_log.events_enabled)
        return;
    char type_buf[8], notes_buf[32];
    snprintf(type_buf, sizeof(type_buf), "%u", (unsigned)s->channel_type);
    const char *from = (prev->channel_type == (uint8_t)PSG_CHTYPE_TONE) ? "TONE" : "NOISE";
    const char *to   = (s->channel_type    == (uint8_t)PSG_CHTYPE_TONE) ? "TONE" : "NOISE";
    snprintf(notes_buf, sizeof(notes_buf), "%s->%s", from, to);
    psg_log_write_event(s->t_frame, chip, ch, "TYPE_CHANGE",
                        "-", "-", type_buf, notes_buf);
}

/**
 * @brief Emit PITCH_CHANGE event při změně MIDI integer pitch během noty.
 *
 * Volá se z `tick()` mezi note_off (uzavírá starou notu se starým pitch)
 * a note_on (otevírá novou notu s novým pitch). `notes` obsahuje
 * `prev_pitch=X new_pitch=Y` pro snadnou offline analýzu split bodu.
 *
 * Sloupec `pitch_or_attn` drží **nový** MIDI pitch (= konzistentní s
 * následujícím NOTE_ON), `divider` drží **nový** divider.
 *
 * @param s            Aktuální vzorek s novým dividerem (NESMÍ být NULL).
 * @param chip         Index chipu.
 * @param ch           Index kanálu.
 * @param prev_pitch   Pitch staré (uzavřené) noty 0..127.
 * @param new_pitch    Pitch nové (otevírané) noty 0..127.
 */
static void psg_log_emit_pitch_change(const st_PSG_SCOPE_SAMPLE *s,
                                      unsigned chip, unsigned ch,
                                      int prev_pitch, int new_pitch)
{
    if (!g_psg_log.events_enabled)
        return;
    char pitch_buf[16], div_buf[16], notes_buf[64];
    snprintf(pitch_buf, sizeof(pitch_buf), "%d", new_pitch);
    snprintf(div_buf,   sizeof(div_buf),   "%u", (unsigned)s->divider);
    snprintf(notes_buf, sizeof(notes_buf), "prev_pitch=%d new_pitch=%d",
             prev_pitch, new_pitch);
    psg_log_write_event(s->t_frame, chip, ch, "PITCH_CHANGE",
                        pitch_buf, div_buf, "0", notes_buf);
}

/* ===================================================================== */
/* Sampling tick - UI thread, side-effect free vůči emu                  */
/* ===================================================================== */

void psg_audio_scope_tick(void)
{
    /* Pause/resume detekce pro event log - PŘED early return na pause.
     * Frame counter v okamžiku eventu = aktuální g_psg_scope_frame_counter
     * (= poslední validní non-paused tick). */
    bool is_paused_now = EMULATOR_TEST_PAUSED;
    if (g_psg_log.events_enabled)
    {
        if (!g_psg_log.was_paused_last_tick && is_paused_now)
        {
            psg_log_write_event(g_psg_scope_frame_counter, 0u, 0u,
                                "PAUSE", "-", "-", "-",
                                "emulator paused");
        }
        else if (g_psg_log.was_paused_last_tick && !is_paused_now)
        {
            psg_log_write_event(g_psg_scope_frame_counter, 0u, 0u,
                                "RESUME", "-", "-", "-",
                                "emulator resumed");
        }
    }
    g_psg_log.was_paused_last_tick = is_paused_now;

    /* Pauza emulátoru: neukládáme phantom samples ani neinkrementujeme
     * frame counter. Po unpause sampling pokračuje od kde skončil (= žádný
     * velký delta gap pro existující noty). Pokud byla v okamžiku pauzy
     * aktivní nota, po unpause buď PSG mirror vrátí stejný attn/divider
     * (state machine nic nezmění, nota pokračuje), nebo se změní a nastane
     * note_off + případně new note_on - správné chování pause/resume. */
    if (is_paused_now)
        return;

    /* Stereo detekce - mono = jen psg[0], stereo = psg[0]+psg[1]. */
    unsigned chip_count = g_psg_module.stereo ? 2u : 1u;

    /* Časová známka = aktuální pxCLK total (gdg_get_total_ticks). Hodnota je
     * monotonní a roste ~17.7M tiků/s reálného času emulátoru bez ohledu na
     * UI render frekvenci. Pauza emu je gate-ovaná výše (early return). */
    g_psg_scope_frame_counter = gdg_get_total_ticks ( );

    for (unsigned chip = 0; chip < chip_count; ++chip)
    {
        const st_PSG *psg = &g_psg_module.psg[chip];
        for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
        {
            st_PSG_SCOPE_SAMPLE s;
            s.t_frame      = g_psg_scope_frame_counter;
            s.attn         = (uint8_t)psg_mirror_channel_attn(psg, ch);
            s.channel_type = (uint8_t)psg_mirror_channel_type(psg, ch);
            /* Divider má smysl jen pro TONE; pro NOISE čteme 0 (irelevantní). */
            if ((en_PSG_CHTYPE)s.channel_type == PSG_CHTYPE_TONE)
                s.divider = psg_mirror_channel_tone_divider(psg, ch);
            else
                s.divider = 0u;

            /* State machine F3: porovnání předchozího (latest před pushem)
             * s aktuálním vzorkem. Detekuje přechody silent ↔ playing a
             * channel type swap během aktivní noty. */
            st_PSG_SCOPE_EVENTS *ev = &g_psg_events[chip][ch];
            const st_PSG_SCOPE_SAMPLE *prev = psg_scope_ring_latest(&g_psg_scope[chip][ch]);

            bool was_playing = (prev != NULL) && psg_sample_is_playing(prev);
            bool is_playing  = psg_sample_is_playing(&s);

            /* Channel type swap během aktivní noty (CH3 TONE↔NOISE).
             * Strategie (a) z F3 briefu: ukončit + start nová, pokud nový
             * vzorek splňuje is_playing. */
            bool type_changed = ev->note_active &&
                                prev != NULL &&
                                prev->channel_type != s.channel_type;

            if (type_changed)
            {
                psg_log_emit_type_change(prev, &s, chip, ch);
                psg_log_emit_note_off(ev, &s, chip, ch);
                psg_events_note_off(ev, &s);
                if (is_playing)
                {
                    psg_events_note_on(ev, &s, chip, ch);
                    psg_log_emit_note_on(ev, &s, chip, ch);
                }
            }
            else if (!was_playing && is_playing)
            {
                psg_events_note_on(ev, &s, chip, ch);
                psg_log_emit_note_on(ev, &s, chip, ch);
            }
            else if (was_playing && !is_playing)
            {
                psg_log_emit_note_off(ev, &s, chip, ch);
                psg_events_note_off(ev, &s);
            }
            else if (was_playing && is_playing && ev->note_active)
            {
                /* Pitch change detection (V1.5):
                 * Pokud se divider během TONE noty změní natolik, že
                 * nový MIDI integer pitch je jiný než původní, rozdělíme
                 * notu: note_off staré + note_on s novým pitch. Drobný
                 * cents drift (= stejný integer pitch) ignorujeme - to je
                 * jen vibrato / mikroladění.
                 *
                 * Důvod: chiptune programy (Flappy a podobné) mění
                 * divider mid-note pro melodii bez attn resetu - V1
                 * exportovala 52 s sustained noty namísto reálné melodie.
                 *
                 * Po pitch_change je nota nová, attn_history začíná
                 * znovu. Pokud k pitch_change nedošlo, kontrolujeme
                 * volume envelope change (= attn modulace během noty) a
                 * případně zapisujeme do `attn_history`. */
                bool pitch_changed = false;
                if ((en_PSG_CHTYPE)s.channel_type == PSG_CHTYPE_TONE &&
                    ev->active_note.midi_pitch >= 0)
                {
                    int new_pitch = 0, new_cents = 0;
                    psg_tone_to_midi(s.divider, &new_pitch, &new_cents);
                    if (new_pitch != ev->active_note.midi_pitch)
                    {
                        int prev_pitch = ev->active_note.midi_pitch;
                        psg_log_emit_note_off(ev, &s, chip, ch);
                        psg_events_note_off(ev, &s);
                        psg_log_emit_pitch_change(&s, chip, ch, prev_pitch, new_pitch);
                        psg_events_note_on(ev, &s, chip, ch);
                        psg_log_emit_note_on(ev, &s, chip, ch);
                        pitch_changed = true;
                    }
                }

                /* Volume envelope tracking - zachytí attn modulaci během
                 * běžící noty (vibrato, fade, 1-bit PCM). Filtruje:
                 *  - attn == 15: rezervováno pro note_off, neukládat,
                 *  - stejný attn jako poslední: žádná změna, neukládat. */
                if (!pitch_changed && ev->note_active &&
                    s.attn < 15u &&
                    s.attn != ev->active_note.attn_last_seen)
                {
                    psg_events_append_attn_point(ev, &s);
                }
            }

            psg_scope_ring_push(&g_psg_scope[chip][ch], &s);

            /* Sample-level log - po push (= struktura `s` validní). */
            psg_log_write_sample(chip, ch, &s);
        }
    }
}

/* ===================================================================== */
/* Responsive oscilloscope renderer                                       */
/* ===================================================================== */

/**
 * @brief Výška jednoho oscilloscope řádku v px.
 *
 * Šířka řádku je responsive (= závisí na `ImGui::GetContentRegionAvail().x`),
 * výška zůstává konstantní pro vizuální konzistenci mezi kanály.
 */
static const float PSG_SCOPE_ROW_HEIGHT = 40.0f;

/**
 * @brief Počet TONE square wave period vykreslených napříč regionem.
 *
 * Fixní hodnota - region vždy ukazuje 4 cykly bez ohledu na skutečnou
 * frekvenci. Konzistentní vzhled napříč low/high f, nezávislé na časovém
 * měřítku ringu (= oscilloscope ukazuje "co PSG právě hraje", ne sample
 * historii ze zvukové pipeline).
 */
static const int PSG_SCOPE_TONE_PERIODS = 4;

/**
 * @brief Vykreslí responsive oscilloscope řádek jednoho PSG kanálu.
 *
 * Region: full content-region šířka × `PSG_SCOPE_ROW_HEIGHT` výška.
 * Zdrojem stavu je nejnovější vzorek z ring bufferu (= `psg_scope_ring_latest`)
 * - pokud ring prázdný (úvodní 1 frame před prvním tickem), nakreslí
 * pouze pozadí + středovou čáru.
 *
 * Render dle typu kanálu:
 *  - TONE + divider >= 2: square wave 50% duty, `PSG_SCOPE_TONE_PERIODS`
 *    cyklů, amplituda lineárně `(15 - attn) / 15`.
 *  - TONE + divider < 2: DC (vodorovná čára na horní úrovni amplitudy
 *    + "DC" label).
 *  - NOISE: deterministický pseudo-random pattern (`seed ^ x` xor-shift),
 *    konzistentní mezi frame-y aby pattern neblikal.
 *  - Silent (attn == 15): vodorovná čára uprostřed + "silent" label.
 *
 * Amplitude scaling i základní waveform logika je kopií z `psg_window.cpp`
 * (responsive width adaptace). Refactor do sdíleného helperu je plánovaný
 * pro pozdější F-fáze, v F1 schválně inline (= minimalizace rušení
 * referenčního psg_window.cpp).
 *
 * @param chip       Index PSG chipu (0 = mono/levý, 1 = pravý).
 * @param ch         Index kanálu (0..3).
 * @param row_label  ImGui label nad řádkem (typicky "Channel N (TYPE)").
 * @param id_suffix  Unikátní suffix pro InvisibleButton ID (musí být anglický).
 */
static void psg_audio_scope_render_channel(unsigned chip, unsigned ch,
                                           const char *row_label,
                                           const char *id_suffix,
                                           float row_height_override)
{
    const st_PSG_SCOPE_RING *r = &g_psg_scope[chip][ch];
    const st_PSG_SCOPE_SAMPLE *latest = psg_scope_ring_latest(r);

    /* Hlavička řádku - label + textový stav. */
    ImGui::TextUnformatted(row_label);

    /* Responsive width = full content region; padding 2 px na obou stranách. */
    float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w < 80.0f)
        avail_w = 80.0f; /* sanity floor - extrémně úzké okno */
    /* Vertikální škálování - pokud caller předal nenulovou hodnotu, použij
     * ji (= proporční výška dle window content). Jinak default 40 px. */
    float row_h = ( row_height_override > 0.0f )
                  ? row_height_override
                  : PSG_SCOPE_ROW_HEIGHT;
    ImVec2 region_size(avail_w, row_h);

    /* 96 = id_suffix max 63 + "##scope_" 8 + \0 + rezerva. */
    char btnid[96];
    snprintf(btnid, sizeof(btnid), "##scope_%s", id_suffix);
    ImGui::InvisibleButton(btnid, region_size);
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const ImU32 bg_col     = IM_COL32(18, 22, 32, 255);
    const ImU32 border_col = IM_COL32(70, 80, 100, 255);
    const ImU32 mid_col    = IM_COL32(60, 70, 90, 255);
    const ImU32 wave_col   = IM_COL32(180, 220, 255, 255);
    const ImU32 silent_col = IM_COL32(120, 120, 130, 255);

    dl->AddRectFilled(p0, p1, bg_col);
    dl->AddRect(p0, p1, border_col);

    float w = p1.x - p0.x;
    float h = p1.y - p0.y;
    float mid_y = p0.y + h * 0.5f;
    dl->AddLine(ImVec2(p0.x, mid_y), ImVec2(p1.x, mid_y), mid_col, 1.0f);

    /* Pokud ring zatím prázdný (1. frame), žádný vzorek = jen pozadí. */
    if (!latest)
    {
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), silent_col, _("no data"));
        return;
    }

    unsigned attn = latest->attn;
    if (attn >= 15u)
    {
        dl->AddLine(ImVec2(p0.x + 2.0f, mid_y), ImVec2(p1.x - 2.0f, mid_y),
                    silent_col, 1.5f);
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), silent_col, _("silent"));
        return;
    }

    float amp_norm = (15.0f - (float)attn) / 15.0f;
    float amp_px = (h * 0.45f) * amp_norm;

    en_PSG_CHTYPE type = (en_PSG_CHTYPE)latest->channel_type;

    if (type == PSG_CHTYPE_TONE)
    {
        uint16_t divider = latest->divider;

        if (divider < 2u)
        {
            float dc_y = mid_y - amp_px;
            dl->AddLine(ImVec2(p0.x + 2.0f, dc_y), ImVec2(p1.x - 2.0f, dc_y),
                        wave_col, 1.5f);
            dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), wave_col, _("DC"));
            return;
        }

        /* Square wave - 4 periody na regionové šířce, responsive. Allocujeme
         * dynamicky max 4*period+1 bodů (= 17 pro periods=4, fits do
         * statického bufferu). */
        const int periods = PSG_SCOPE_TONE_PERIODS;
        const int n_pts = periods * 4 + 1;
        ImVec2 pts[64];
        if (n_pts > (int)(sizeof(pts) / sizeof(pts[0])))
            return; /* defensivní - n_pts=17 pro periods=4, fits. */

        float period_w = (w - 4.0f) / (float)periods;
        float x = p0.x + 2.0f;
        float y_hi = mid_y - amp_px;
        float y_lo = mid_y + amp_px;
        int idx = 0;
        pts[idx++] = ImVec2(x, y_hi);
        for (int p = 0; p < periods; ++p)
        {
            float x_half = x + period_w * 0.5f;
            float x_end  = x + period_w;
            pts[idx++] = ImVec2(x_half, y_hi);
            pts[idx++] = ImVec2(x_half, y_lo);
            pts[idx++] = ImVec2(x_end,  y_lo);
            pts[idx++] = ImVec2(x_end,  y_hi);
            x = x_end;
        }
        dl->AddPolyline(pts, idx, wave_col, 0, 1.5f);
    }
    else /* PSG_CHTYPE_NOISE */
    {
        /* Deterministický xor-shift pattern - seed z (chip, ch) aby
         * pattern neblikal mezi frame-y. */
        unsigned seed = 0x9E3779B1u ^ (chip * 0x85EBCA6Bu) ^ (ch * 0xC2B2AE35u);
        const float step = 2.0f;
        for (float dx = 0.0f; dx < w - 4.0f; dx += step)
        {
            unsigned s = seed ^ (unsigned)(dx * 7.0f);
            s ^= s >> 13;
            s ^= s << 17;
            s ^= s >> 5;
            float sign = (s & 1u) ? 1.0f : -1.0f;
            float xs = p0.x + 2.0f + dx;
            float ys = mid_y + sign * amp_px;
            dl->AddLine(ImVec2(xs, mid_y), ImVec2(xs, ys), wave_col, 1.0f);
        }
    }
}

/* ===================================================================== */
/* Envelope renderer - attn historie z ring bufferu                       */
/* ===================================================================== */

/**
 * @brief Výška envelope řádku v px.
 *
 * Záměrně menší než `PSG_SCOPE_ROW_HEIGHT` - envelope je sekundární
 * vizualizace pod scope. Šířka je responsive, stejná jako u scope.
 */
static const float PSG_SCOPE_ENVELOPE_HEIGHT = 26.0f;

/**
 * @brief Vykreslí envelope timeline jednoho kanálu (attn historie přes ring).
 *
 * Region: full content-region šířka × `PSG_SCOPE_ENVELOPE_HEIGHT` výška.
 * Iteruje vzorky v ringu chronologicky (oldest = vlevo, newest = vpravo),
 * pro každý vzorek nakreslí svislý sloupec vysoký podle invertovaného
 * attenuation:
 *  - `attn == 0`  (max volume = 0 dB) → plná výška (top regionu),
 *  - `attn == 15` (silent)            → nulová výška (baseline),
 *  - linear interpolace `(15 - attn) / 15`.
 *
 * Aggregate strategie: pokud `region_w / count < 1.0`, vykreslí jeden
 * sloupec šířky 1 px za každou X-pozici a vybere **maximum** amplitudy
 * vzorků mapujících se do tohoto sloupce (= envelope špičky nezmizí
 * při downsamplingu). Pokud `region_w / count >= 1.0`, použije sloupec
 * šířky `region_w / count` per vzorek (= žádná ztráta detailu).
 *
 * Pokud ring není plný (`count < capacity`), levá nepoužitá část se
 * nechá černá (= "no data yet").
 *
 * Color scheme:
 *  - background: stejné `IM_COL32(18, 22, 32, 255)` jako scope,
 *  - fill:       solid `IM_COL32(150, 200, 100, 200)` (jemně zelená,
 *                semi-transparent), shoda s ADSR-style envelope intuicí,
 *  - "now" indicator: tenká vertikální čára na pravé hraně (= newest
 *                sample), pomáhá rozumět směru času.
 *
 * @param chip       Index chipu (0 = mono/levý, 1 = pravý).
 * @param ch         Index kanálu (0..3).
 * @param id_suffix  Unikátní suffix pro InvisibleButton ID (musí být anglický).
 */
static void psg_audio_scope_render_envelope(unsigned chip, unsigned ch,
                                            const char *id_suffix,
                                            float env_height_override)
{
    const st_PSG_SCOPE_RING *r = &g_psg_scope[chip][ch];

    float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w < 80.0f)
        avail_w = 80.0f;
    /* Vertikální škálování - viz psg_audio_scope_render_channel. */
    float env_h = ( env_height_override > 0.0f )
                  ? env_height_override
                  : PSG_SCOPE_ENVELOPE_HEIGHT;
    ImVec2 region_size(avail_w, env_h);

    /* 96 = id_suffix max 63 + "##envelope_" 11 + \0 + rezerva. */
    char btnid[96];
    snprintf(btnid, sizeof(btnid), "##envelope_%s", id_suffix);
    ImGui::InvisibleButton(btnid, region_size);
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const ImU32 bg_col     = IM_COL32(18, 22, 32, 255);
    const ImU32 border_col = IM_COL32(70, 80, 100, 255);
    const ImU32 fill_col   = IM_COL32(150, 200, 100, 200);
    const ImU32 now_col    = IM_COL32(220, 220, 100, 200);
    const ImU32 silent_col = IM_COL32(120, 120, 130, 255);

    dl->AddRectFilled(p0, p1, bg_col);
    dl->AddRect(p0, p1, border_col);

    if (r->count == 0u)
    {
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), silent_col, _("no data"));
        return;
    }

    float w = p1.x - p0.x;
    float h = p1.y - p0.y;
    float inner_w = w - 2.0f; /* 1 px padding na každé straně */
    if (inner_w < 1.0f)
        return;

    /* Pro neúplný ring kreslíme od x odpovídajícího `(capacity - count)`,
     * tj. nejstarší platný vzorek vlevo, levá nepoužitá část zůstává
     * tmavá (= "no data yet"). */
    const unsigned cap = PSG_SCOPE_RING_CAPACITY;
    unsigned start_idx_in_ring = (r->head + cap - r->count) % cap;

    /* X-pozice plné kapacity - `cap` slotů přes `inner_w`. */
    float slot_w = inner_w / (float)cap;

    if (slot_w >= 1.0f)
    {
        /* Dostatek místa - sloupec per vzorek, žádná aggregation. */
        float col_w = slot_w;
        if (col_w < 1.0f) col_w = 1.0f;
        for (unsigned i = 0; i < r->count; ++i)
        {
            unsigned ring_idx = (start_idx_in_ring + i) % cap;
            unsigned attn = r->samples[ring_idx].attn;
            if (attn >= 15u)
                continue; /* silent = nekreslíme nic */
            float amp_norm = (15.0f - (float)attn) / 15.0f;
            float bar_h = h * amp_norm;

            /* Vzorek leží na slotu `(cap - count + i)` - tak aby
             * nejnovější (i = count-1) padl na poslední slot vpravo. */
            unsigned slot = cap - r->count + i;
            float x = p0.x + 1.0f + (float)slot * slot_w;
            float y_top = p1.y - bar_h;
            float y_bot = p1.y;
            float x_end = x + col_w;
            if (x_end > p1.x - 1.0f) x_end = p1.x - 1.0f;
            dl->AddRectFilled(ImVec2(x, y_top), ImVec2(x_end, y_bot), fill_col);
        }
    }
    else
    {
        /* Aggregate: víc vzorků padá do jednoho pixelu. Pro každý pixel
         * (1 px wide) vybereme MAXIMUM amplitudy ze vzorků mapovaných
         * do tohoto pixelu (= envelope špičky se neztratí). Iterujeme
         * po pixelech inner_w a pro každý dohledáme rozsah ringových
         * indexů. */
        int pixels = (int)inner_w;
        if (pixels < 1) pixels = 1;
        for (int px = 0; px < pixels; ++px)
        {
            /* Pixel x rozsahuje slot indexy `[px*cap/pixels, (px+1)*cap/pixels)`.
             * Validní vzorky leží jen v posledních `count` slotech, tj. od
             * `cap - count`. */
            unsigned slot_a = (unsigned)((uint64_t)px * cap / (unsigned)pixels);
            unsigned slot_b = (unsigned)((uint64_t)(px + 1) * cap / (unsigned)pixels);
            if (slot_b <= slot_a) slot_b = slot_a + 1u;

            unsigned valid_from = cap - r->count;
            if (slot_b <= valid_from)
                continue; /* mimo platnou historii */

            unsigned a = slot_a < valid_from ? valid_from : slot_a;
            unsigned b = slot_b > cap ? cap : slot_b;

            /* Hledáme min(attn) = max amplitude. */
            unsigned best_attn = 15u;
            for (unsigned slot = a; slot < b; ++slot)
            {
                unsigned i = slot - valid_from;
                unsigned ring_idx = (start_idx_in_ring + i) % cap;
                unsigned attn = r->samples[ring_idx].attn;
                if (attn < best_attn) best_attn = attn;
            }
            if (best_attn >= 15u)
                continue;

            float amp_norm = (15.0f - (float)best_attn) / 15.0f;
            float bar_h = h * amp_norm;
            float x = p0.x + 1.0f + (float)px;
            float y_top = p1.y - bar_h;
            dl->AddRectFilled(ImVec2(x, y_top), ImVec2(x + 1.0f, p1.y), fill_col);
        }
    }

    /* "Now" indicator - tenká svislá čára na pravé hraně. Pouze pokud
     * ring obsahuje aspoň jeden vzorek. */
    float now_x = p1.x - 1.0f;
    dl->AddLine(ImVec2(now_x, p0.y + 1.0f), ImVec2(now_x, p1.y - 1.0f),
                now_col, 1.0f);
}

/* ===================================================================== */
/* Render jednoho chipu (4 kanály)                                        */
/* ===================================================================== */

/**
 * @brief Vykreslí 4 oscilloscope řádky jednoho PSG chipu.
 *
 * Label kanálu zahrnuje typ (TONE / NOISE) čtený z nejnovějšího vzorku
 * v ringu (= odraz mirror getteru při posledním tick callbacku).
 *
 * @param chip       Index chipu (0 = mono/levý, 1 = pravý).
 * @param id_prefix  Unikátní ImGui ID prefix (např. "psg0" / "psg1").
 *                   Musí být anglický (= součást ImGui internal ID).
 */
static void psg_audio_scope_render_chip(unsigned chip, const char *id_prefix,
                                        float row_height, float env_height)
{
    for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
    {
        const st_PSG_SCOPE_RING *r = &g_psg_scope[chip][ch];
        const st_PSG_SCOPE_SAMPLE *latest = psg_scope_ring_latest(r);

        /* Typ label - lokalizovaný TONE / NOISE; pokud bez dat, default TONE
         * pro vizuálně klidný display (nezáleží, oscilloscope vykreslí
         * "no data"). */
        const char *type_label = _("TONE");
        if (latest && (en_PSG_CHTYPE)latest->channel_type == PSG_CHTYPE_NOISE)
            type_label = _("NOISE");

        char row_label[96];
        snprintf(row_label, sizeof(row_label), "%s %u (%s)",
                 _("Channel"), ch, type_label);

        char id_suffix[64];
        snprintf(id_suffix, sizeof(id_suffix), "%s_ch%u", id_prefix, ch);

        psg_audio_scope_render_channel(chip, ch, row_label, id_suffix, row_height);
        psg_audio_scope_render_envelope(chip, ch, id_suffix, env_height);
        ImGui::Spacing();
    }
}

/* ===================================================================== */
/* Piano roll renderer (F4) - timeline pruhů per (chip, channel)         */
/* ===================================================================== */

/**
 * @brief Časový dělitel pro převod `t_frame` (pxCLK ticks) na sekundy.
 *
 * Od fáze fixu monitor-rate-dependent timing (2026-05-23) je `t_frame`
 * hodnotou `gdg_get_total_ticks()` (= pxCLK monotonní counter, 17.7345 MHz
 * pro MZ-800). Dělitel je tedy `g_mzhal.gdgclk_base` ticks/s, ne 60 Hz.
 *
 * Reálný frame rate UI render loopu (vsync, 60/75/144 Hz monitor) NEMĚNÍ
 * tempo metadata - ovlivňuje jen rozlišení vzorkování (1/UI_hz s mezera
 * mezi po sobě jdoucími snapshoty). Časová známka samotná je v pxCLK
 * time base, takže derivace sekund je nezávislá na UI rendu.
 *
 * Název `FRAMES_PER_SECOND` historicky zachován, ale sémantika = pxCLK ticks/s.
 */
static const double PSG_SCOPE_FRAMES_PER_SECOND = (double) g_mzhal.gdgclk_base;

/**
 * @brief Time range selector hodnoty pro piano roll (frames).
 *
 * - `PSG_SCOPE_RANGE_10S` = posledních 10 s historie (= matche scope/envelope
 *   ring buffer, defaultní volba).
 * - `PSG_SCOPE_RANGE_30S` = zoom out, 30 s historie.
 * - `PSG_SCOPE_RANGE_ALL` = vše co je v events bufferu (může pokrýt 10+ min).
 */
enum
{
    PSG_SCOPE_RANGE_10S = 0,
    PSG_SCOPE_RANGE_30S = 1,
    PSG_SCOPE_RANGE_ALL = 2,
};

/**
 * @brief Per-channel + per-chip barevná paleta pro piano roll bars.
 *
 * Layout `[chip][channel]`:
 *  - chip 0 (mono / Left) - sytější varianta.
 *  - chip 1 (Right)       - světlejší tint stejné palety.
 *
 * Kanál 3 = NOISE má svou unikátní (růžová / lososová) - vykresluje se na
 * samostatný "noise lane" pod hlavním pitch range.
 */
static const ImU32 PSG_SCOPE_CHANNEL_COLORS[2][4] = {
    /* chip 0 = mono / Left */
    { IM_COL32(100, 180, 220, 220),  /* CH0 - modrá */
      IM_COL32(220, 180, 100, 220),  /* CH1 - oranžová */
      IM_COL32(180, 220, 100, 220),  /* CH2 - zelenožlutá */
      IM_COL32(220, 100, 180, 220),  /* CH3 NOISE - růžová */
    },
    /* chip 1 = Right - světlejší tint */
    { IM_COL32(150, 210, 240, 220),
      IM_COL32(240, 210, 150, 220),
      IM_COL32(210, 240, 150, 220),
      IM_COL32(240, 150, 210, 220),
    },
};

/**
 * @brief Výška jednoho řádku v piano roll mřížce.
 *
 * Per-pitch (MIDI semitone) řádek. Hustota volena s ohledem na čitelnost
 * (4 px = ~85 řádků na ~340 px výšky pro celý A0..C8 range, ale typicky
 * auto-fit zúží na 20-30 řádků).
 */
static const float PSG_SCOPE_ROLL_ROW_HEIGHT = 4.0f;

/**
 * @brief Minimální celková výška piano roll oblasti.
 *
 * Fallback pokud auto-fit pitch range je prázdný (= žádné události).
 */
static const float PSG_SCOPE_ROLL_MIN_HEIGHT = 120.0f;

/**
 * @brief Šířka levého sloupce s pitch labels (octave bands "C2", "C3"...).
 */
static const float PSG_SCOPE_ROLL_LABEL_W = 40.0f;

/**
 * @brief Výška noise lane (= samostatný horizontální pruh pro NOISE noty).
 */
static const float PSG_SCOPE_ROLL_NOISE_LANE_H = 14.0f;

/**
 * @brief Names not C..B pro hover tooltip MIDI pitch popisek.
 *
 * Standardní 12-tónový equal temperament. Indexed `pitch % 12`. Octave
 * se počítá jako `pitch / 12 - 1` (MIDI konvence: C-1 = 0, C0 = 12, C4 = 60,
 * A4 = 69).
 *
 * Strings záměrně bez `_()` - jde o standardní MIDI note names (universální
 * notace), překladu by nedávalo smysl.
 */
static const char *const PSG_SCOPE_NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

/**
 * @brief Naformátuje MIDI pitch na lidsky čitelné jméno noty.
 *
 * Příklady: pitch 60 → "C4", pitch 69 → "A4", pitch 70 → "A#4".
 * Pro pitch mimo MIDI range (< 0 nebo > 127) vrací "?".
 *
 * @param pitch  MIDI pitch (0..127). -1 = NOISE - vrací "noise".
 * @param buf    Output buffer, ASCII zero-terminated.
 * @param buflen Velikost bufferu (doporučeno >= 8).
 */
static void psg_scope_midi_pitch_to_name(int pitch, char *buf, size_t buflen)
{
    if (pitch < 0)
    {
        snprintf(buf, buflen, "noise");
        return;
    }
    if (pitch < 0 || pitch > 127)
    {
        snprintf(buf, buflen, "?");
        return;
    }
    int octave = pitch / 12 - 1;
    int idx    = pitch % 12;
    snprintf(buf, buflen, "%s%d", PSG_SCOPE_NOTE_NAMES[idx], octave);
}

/**
 * @brief Vrátí počet frame jednotek, které pokrývá vybraný time range.
 *
 * Pro `PSG_SCOPE_RANGE_ALL` vrací 0 (= unlimited, kompletní obsah events
 * bufferu se zobrazí). Pro 10s / 30s spočte podle `PSG_SCOPE_FRAMES_PER_SECOND`.
 *
 * @param range_idx  Index z `PSG_SCOPE_RANGE_*` enum.
 * @return Počet frames pokrývajících rozsah, nebo 0 pro neomezený.
 */
static uint64_t psg_scope_range_frames(int range_idx)
{
    switch (range_idx)
    {
    case PSG_SCOPE_RANGE_10S: return (uint64_t)(PSG_SCOPE_FRAMES_PER_SECOND * 10.0);
    case PSG_SCOPE_RANGE_30S: return (uint64_t)(PSG_SCOPE_FRAMES_PER_SECOND * 30.0);
    case PSG_SCOPE_RANGE_ALL: /* fall-through */
    default:                  return 0u;
    }
}

/**
 * @brief Iterátor přes všechny uzavřené eventy z events ring bufferu.
 *
 * Použití (typický pattern):
 * @code
 *   for (unsigned i = 0; i < ev->count; ++i) {
 *       unsigned idx = (ev->head + PSG_SCOPE_EVENTS_CAPACITY - ev->count + i)
 *                      % PSG_SCOPE_EVENTS_CAPACITY;
 *       const st_PSG_SCOPE_NOTE_EVENT *n = &ev->events[idx];
 *       ...
 *   }
 * @endcode
 *
 * Funkce inline helper zjednodušuje výpočet ringového indexu pro i-tý
 * vzorek (i=0 = nejstarší, i=count-1 = nejnovější).
 *
 * @param ev  Events buffer (NESMÍ být NULL).
 * @param i   Logický index 0..ev->count-1 (i=0 = nejstarší).
 * @return Ukazatel na i-tý event v chronologickém pořadí.
 */
static inline const st_PSG_SCOPE_NOTE_EVENT *
psg_events_at(const st_PSG_SCOPE_EVENTS *ev, unsigned i)
{
    unsigned idx = (ev->head + PSG_SCOPE_EVENTS_CAPACITY - ev->count + i)
                   % PSG_SCOPE_EVENTS_CAPACITY;
    return &ev->events[idx];
}

/**
 * @brief Spočte auto-fit pitch range pro piano roll.
 *
 * Iteruje uzavřené eventy + případnou aktivní notu přes všechny (chip, ch)
 * v rámci `range_frames` (nebo všechny pokud range_frames == 0) a vrací
 * min/max MIDI pitch. NOISE eventy (pitch=-1) ignoruje pro určení pitch
 * range (NOISE má vlastní lane).
 *
 * Padding +/- 2 semitones aplikuje a clampuje na 0..127.
 *
 * Pokud nejsou žádné TONE eventy v rozsahu, vrací default range C3..C5
 * (= 48..72) - prázdné piano roll s rozumnou výchozí škálou.
 *
 * @param range_frames  Počet frames historie (0 = vše).
 * @param now           Aktuální `g_psg_scope_frame_counter`.
 * @param out_min       Out: nejnižší pitch v rozsahu (NESMÍ být NULL).
 * @param out_max       Out: nejvyšší pitch v rozsahu (NESMÍ být NULL).
 * @return true = nalezeny TONE eventy (validní min/max), false = fallback.
 */
static bool psg_scope_auto_fit_pitch(uint64_t range_frames,
                                     uint64_t now,
                                     int *out_min, int *out_max)
{
    int found_min = 127;
    int found_max = 0;
    bool found = false;

    uint64_t cutoff = (range_frames > 0u && now > range_frames)
                      ? (now - range_frames) : 0u;

    unsigned chip_count = g_psg_module.stereo ? 2u : 1u;

    for (unsigned chip = 0; chip < chip_count; ++chip)
    {
        for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
        {
            const st_PSG_SCOPE_EVENTS *ev = &g_psg_events[chip][ch];

            for (unsigned i = 0; i < ev->count; ++i)
            {
                const st_PSG_SCOPE_NOTE_EVENT *n = psg_events_at(ev, i);
                if (n->midi_pitch < 0) continue; /* NOISE - vlastní lane */
                if (n->t_off < cutoff) continue; /* mimo rozsah */
                if (n->midi_pitch < found_min) found_min = n->midi_pitch;
                if (n->midi_pitch > found_max) found_max = n->midi_pitch;
                found = true;
            }
            /* Aktivní nota - také zahrnout pokud TONE. */
            if (ev->note_active && ev->active_note.midi_pitch >= 0)
            {
                int p = ev->active_note.midi_pitch;
                if (p < found_min) found_min = p;
                if (p > found_max) found_max = p;
                found = true;
            }
        }
    }

    if (!found)
    {
        *out_min = 48; /* C3 */
        *out_max = 72; /* C5 */
        return false;
    }

    /* Padding +/- 2 semitones, clamp 0..127. */
    found_min -= 2;
    found_max += 2;
    if (found_min < 0)   found_min = 0;
    if (found_max > 127) found_max = 127;
    if (found_max - found_min < 6) /* minimální okno 6 semitónů */
        found_max = found_min + 6;
    *out_min = found_min;
    *out_max = found_max;
    return true;
}

/**
 * @brief Vykreslí jeden note bar a obsluhuje hover tooltip.
 *
 * Předpoklady: timeline x-mapping (t0_frame .. t1_frame → x_left..x_right)
 * je předaný přes parametry. Y-pozice pro TONE = pitch row, pro NOISE =
 * noise_lane_y0..noise_lane_y0+noise_lane_h.
 *
 * Tooltip se aktivuje při hoveru přes ImGui::IsMouseHoveringRect a vypisuje:
 *   - kanál + chip
 *   - pitch name + MIDI + cents (TONE), nebo "Noise" (NOISE)
 *   - duration v sekundách
 *   - velocity
 *
 * @param dl              ImGui draw list okna.
 * @param bar_x0,bar_y0,bar_x1,bar_y1  Souřadnice bar rectangle (px).
 * @param color           Barva výplně (per-channel z palette).
 * @param n               Notu k zobrazení (NESMÍ být NULL).
 * @param is_active       true = aktivní (= live), nakreslí výraznější border.
 */
static void psg_scope_draw_note_bar(ImDrawList *dl,
                                    float bar_x0, float bar_y0,
                                    float bar_x1, float bar_y1,
                                    ImU32 color,
                                    const st_PSG_SCOPE_NOTE_EVENT *n,
                                    bool is_active)
{
    if (bar_x1 - bar_x0 < 1.0f) bar_x1 = bar_x0 + 1.0f;
    if (bar_y1 - bar_y0 < 1.0f) bar_y1 = bar_y0 + 1.0f;

    dl->AddRectFilled(ImVec2(bar_x0, bar_y0), ImVec2(bar_x1, bar_y1), color);

    if (is_active)
    {
        /* Aktivní (= live) nota - tenký bílý outline aby bylo vidět, že stále hraje. */
        dl->AddRect(ImVec2(bar_x0, bar_y0), ImVec2(bar_x1, bar_y1),
                    IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.0f);
    }

    /* Volume envelope tick marks - vertikální čárky pro každý attn change
     * point. Pozice X: lineární mapování `t_ticks` z `[t_on, t_off]` na
     * `[bar_x0, bar_x1]`. Délka čárky = výška pruhu. Barva: bíla pokud
     * `attn_history_count > 0`, jinak žádné kreslení.
     *
     * Pro aktivní notu mapujeme proti `t_off = t_on + (bar_x1-bar_x0)
     * scaled` ekvivalent přes výpočet timeline (= caller už ten poměr
     * dodržuje, takže t_off se bere z `n->t_off` které je 0 pro aktivní
     * notu - musíme spočítat fallback). */
    if (n->attn_history_count > 0u && (bar_x1 - bar_x0) >= 2.0f)
    {
        uint64_t t_on  = n->t_on;
        uint64_t t_off = n->t_off;
        /* Pro aktivní notu (t_off==0) použijeme nejnovější attn_point jako
         * dolní hranici - takhle se tick marks aspoň zobrazí na správných
         * relativních pozicích uvnitř bar. Caller pruhu ji ovšem prodlouží
         * až do "now", takže tick marks neuvidíme ve správné absolutní
         * pozici, jen v rámci [t_on..nejnovější_attn_point]. Acceptable
         * trade-off pro V1. */
        if (t_off <= t_on)
        {
            /* Fallback: t_off = poslední attn_point t_ticks. */
            uint8_t last_idx = (uint8_t)((n->attn_history_head
                                          + PSG_SCOPE_MAX_ATTN_POINTS - 1u)
                                         % PSG_SCOPE_MAX_ATTN_POINTS);
            t_off = n->attn_history[last_idx].t_ticks;
            if (t_off <= t_on) t_off = t_on + 1u;
        }
        double span = (double)(t_off - t_on);
        if (span < 1.0) span = 1.0;
        ImU32 tick_col = IM_COL32(255, 255, 255, 180);
        for (unsigned i = 0; i < n->attn_history_count; ++i)
        {
            const st_PSG_SCOPE_ATTN_POINT *p = &n->attn_history[i];
            if (p->t_ticks < t_on || p->t_ticks > t_off) continue;
            double rel = (double)(p->t_ticks - t_on) / span;
            float tx = bar_x0 + (float)rel * (bar_x1 - bar_x0);
            if (tx < bar_x0) tx = bar_x0;
            if (tx > bar_x1) tx = bar_x1;
            dl->AddLine(ImVec2(tx, bar_y0), ImVec2(tx, bar_y1), tick_col, 1.0f);
        }
    }

    /* Hover tooltip - jenom pokud kurzor přesně nad bar rectem. */
    ImVec2 mp = ImGui::GetMousePos();
    if (mp.x >= bar_x0 && mp.x <= bar_x1 && mp.y >= bar_y0 && mp.y <= bar_y1)
    {
        char name_buf[8];
        psg_scope_midi_pitch_to_name(n->midi_pitch, name_buf, sizeof(name_buf));

        double dur_s = (double)(n->t_off > n->t_on ? n->t_off - n->t_on : 0u)
                       / PSG_SCOPE_FRAMES_PER_SECOND;

        ImGui::BeginTooltip();
        ImGui::Text("%s %u (%s %u)",
                    _("Channel"), (unsigned)n->channel,
                    _("chip"),   (unsigned)n->chip);
        if (n->midi_pitch < 0)
        {
            ImGui::Text("%s", _("Pitch: Noise"));
        }
        else
        {
            ImGui::Text("%s: %s (MIDI %d) %+d %s",
                        _("Pitch"), name_buf,
                        n->midi_pitch, n->cents_detune, _("cents"));
        }
        ImGui::Text("%s: %.3f s", _("Duration"), dur_s);
        ImGui::Text("%s: %u", _("Velocity"), (unsigned)n->velocity);

        /* Volume envelope info - počet attn change pointů + min/max attn. */
        if (n->attn_history_count > 0u)
        {
            uint8_t a_min = 15u, a_max = 0u;
            for (unsigned i = 0; i < n->attn_history_count; ++i)
            {
                uint8_t a = n->attn_history[i].attn;
                if (a < a_min) a_min = a;
                if (a > a_max) a_max = a;
            }
            ImGui::Text("%s: %u (attn %u..%u)",
                        _("Volume changes"),
                        (unsigned)n->attn_history_count,
                        (unsigned)a_min, (unsigned)a_max);
            if (n->attn_history_overflowed)
                ImGui::TextDisabled("(%s)", _("overflowed"));
        }

        if (is_active)
            ImGui::TextDisabled("(%s)", _("active"));
        ImGui::EndTooltip();
    }
}

/**
 * @brief Vykreslí piano roll sekci - timeline pruhů per (chip, channel).
 *
 * UI layout:
 *  1) Toolbar - time range selector (RadioButton 10s / 30s / All) +
 *               legend (color swatches per channel).
 *  2) Plot region - levý label sloupec (octave bands) + main timeline:
 *     - mřížka: horizontální čáry per oktávu, vertikální per ~5s
 *     - bary not: ImDrawList::AddRectFilled, color z PSG_SCOPE_CHANNEL_COLORS
 *     - noise lane: samostatný pruh dole pro pitch=-1 NOISE noty
 *     - aktivní nota: prodloužená do `now` s outline
 *  3) Empty state: pokud žádné eventy (uzavřené + active), centered text.
 *
 * Časové měřítko: `now` = `g_psg_scope_frame_counter`. Pro range != ALL
 * je rozsah `[now-range_frames .. now]`. Pro RANGE_ALL spočítáme `t_min`
 * jako nejstarší `t_on` napříč všemi (chip, ch) ringy.
 */
static void psg_audio_scope_render_piano_roll(void)
{
    /* Toolbar - time range selector + legend. */
    static int s_range_idx = PSG_SCOPE_RANGE_10S;

    ImGui::TextUnformatted(_("Range:"));
    ImGui::SameLine();
    ImGui::RadioButton(_L("Last 10s###PSGScopeRoll10s"),
                       &s_range_idx, PSG_SCOPE_RANGE_10S);
    ImGui::SameLine();
    ImGui::RadioButton(_L("Last 30s###PSGScopeRoll30s"),
                       &s_range_idx, PSG_SCOPE_RANGE_30S);
    ImGui::SameLine();
    ImGui::RadioButton(_L("All###PSGScopeRollAll"),
                       &s_range_idx, PSG_SCOPE_RANGE_ALL);

    /* Legend - color swatches per channel index. */
    ImGui::TextUnformatted(_("Channels:"));
    ImDrawList *legend_dl = ImGui::GetWindowDrawList();
    for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
    {
        ImGui::SameLine();
        ImVec2 cur = ImGui::GetCursorScreenPos();
        float sw = ImGui::GetTextLineHeight();
        legend_dl->AddRectFilled(cur, ImVec2(cur.x + sw, cur.y + sw),
                                 PSG_SCOPE_CHANNEL_COLORS[0][ch]);
        ImGui::Dummy(ImVec2(sw + 4.0f, sw));
        ImGui::SameLine();
        if (ch == 3u)
            ImGui::TextUnformatted(_("CH3 (Noise)"));
        else
            ImGui::Text("CH%u", ch);
    }

    /* Časový rozsah. */
    uint64_t now = g_psg_scope_frame_counter;
    uint64_t range_frames = psg_scope_range_frames(s_range_idx);

    /* Pro RANGE_ALL hledáme nejstarší t_on přes všechny eventy. */
    uint64_t t_min, t_max;
    t_max = now;
    if (range_frames == 0u)
    {
        uint64_t oldest = now;
        bool has_any = false;
        unsigned chip_count = g_psg_module.stereo ? 2u : 1u;
        for (unsigned chip = 0; chip < chip_count; ++chip)
        {
            for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
            {
                const st_PSG_SCOPE_EVENTS *ev = &g_psg_events[chip][ch];
                if (ev->count > 0u)
                {
                    const st_PSG_SCOPE_NOTE_EVENT *n0 = psg_events_at(ev, 0);
                    if (n0->t_on < oldest) oldest = n0->t_on;
                    has_any = true;
                }
                if (ev->note_active && ev->active_note.t_on < oldest)
                {
                    oldest = ev->active_note.t_on;
                    has_any = true;
                }
            }
        }
        if (!has_any)
            oldest = (now > (uint64_t)(PSG_SCOPE_FRAMES_PER_SECOND * 10.0))
                     ? (now - (uint64_t)(PSG_SCOPE_FRAMES_PER_SECOND * 10.0))
                     : 0u;
        t_min = oldest;
    }
    else
    {
        t_min = (now > range_frames) ? (now - range_frames) : 0u;
    }

    if (t_max <= t_min) t_max = t_min + 1u;
    double total_frames = (double)(t_max - t_min);

    /* Auto-fit pitch range (jen TONE eventy v aktuálním time range). */
    int pitch_min, pitch_max;
    bool has_tone = psg_scope_auto_fit_pitch(range_frames, now,
                                             &pitch_min, &pitch_max);

    /* Empty state - žádné události + žádné aktivní noty. */
    if (psg_audio_scope_total_notes() == 0u)
    {
        bool any_active = false;
        for (unsigned chip = 0; chip < PSG_MAX_COUNT && !any_active; ++chip)
            for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT && !any_active; ++ch)
                if (g_psg_events[chip][ch].note_active)
                    any_active = true;
        if (!any_active)
        {
            ImGui::Dummy(ImVec2(0, 20.0f));
            float avail_w = ImGui::GetContentRegionAvail().x;
            const char *msg = _("No notes recorded yet. Play some audio to populate the piano roll.");
            float tw = ImGui::CalcTextSize(msg).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - tw) * 0.5f);
            ImGui::TextDisabled("%s", msg);
            ImGui::Dummy(ImVec2(0, 20.0f));
            return;
        }
    }

    /* Plot region geometry. */
    int pitch_rows = pitch_max - pitch_min + 1;
    if (pitch_rows < 1) pitch_rows = 1;
    float pitch_area_h = (float)pitch_rows * PSG_SCOPE_ROLL_ROW_HEIGHT;
    float total_h = pitch_area_h + PSG_SCOPE_ROLL_NOISE_LANE_H + 6.0f;
    if (total_h < PSG_SCOPE_ROLL_MIN_HEIGHT) total_h = PSG_SCOPE_ROLL_MIN_HEIGHT;

    float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w < 120.0f) avail_w = 120.0f;

    ImVec2 region_size(avail_w, total_h);
    ImGui::InvisibleButton("##psg_scope_roll", region_size);
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const ImU32 bg_col       = IM_COL32(18, 22, 32, 255);
    const ImU32 border_col   = IM_COL32(70, 80, 100, 255);
    const ImU32 grid_col     = IM_COL32(40, 50, 65, 255);
    const ImU32 grid_oct_col = IM_COL32(60, 75, 95, 255);
    const ImU32 label_col    = IM_COL32(180, 190, 210, 255);
    const ImU32 noise_div_col = IM_COL32(90, 100, 120, 255);

    dl->AddRectFilled(p0, p1, bg_col);
    dl->AddRect(p0, p1, border_col);

    /* Levý label sloupec + plot oblast. */
    float plot_x0 = p0.x + PSG_SCOPE_ROLL_LABEL_W;
    float plot_x1 = p1.x - 2.0f;
    float plot_y0 = p0.y + 2.0f;
    float plot_y1 = p1.y - 2.0f;
    /* Pitch area = horní část, noise lane = dolní pruh. */
    float pitch_plot_y1 = plot_y1 - PSG_SCOPE_ROLL_NOISE_LANE_H - 2.0f;
    if (pitch_plot_y1 < plot_y0 + 4.0f) pitch_plot_y1 = plot_y0 + 4.0f;
    float pitch_plot_h = pitch_plot_y1 - plot_y0;
    float row_h = pitch_plot_h / (float)pitch_rows;
    if (row_h < 1.0f) row_h = 1.0f;

    /* Octave labels v levém sloupci (C0, C1, C2, ...) */
    for (int p = pitch_min; p <= pitch_max; ++p)
    {
        if (p % 12 == 0) /* C-tóny */
        {
            float y = plot_y0 + (float)(pitch_max - p) * row_h;
            /* Octave grid line */
            dl->AddLine(ImVec2(plot_x0, y), ImVec2(plot_x1, y),
                        grid_oct_col, 1.0f);
            /* 16 = "C" + %d max 11 (signed int) + \0 + rezerva. */
            char label[16];
            int octave = p / 12 - 1;
            snprintf(label, sizeof(label), "C%d", octave);
            dl->AddText(ImVec2(p0.x + 4.0f, y - 6.0f), label_col, label);
        }
        else if (p % 12 == 5) /* F-tóny - sekundární mřížka */
        {
            float y = plot_y0 + (float)(pitch_max - p) * row_h;
            dl->AddLine(ImVec2(plot_x0, y), ImVec2(plot_x1, y),
                        grid_col, 1.0f);
        }
    }

    /* Dělící čára noise lane. */
    dl->AddLine(ImVec2(plot_x0, pitch_plot_y1),
                ImVec2(plot_x1, pitch_plot_y1),
                noise_div_col, 1.5f);

    /* Noise lane label vlevo. */
    dl->AddText(ImVec2(p0.x + 4.0f, pitch_plot_y1 + 1.0f),
                label_col, _("NS"));

    /* Vertikální časové dělení každých ~5s (5 * g_mzhal.gdgclk_base pxCLK ticks). */
    {
        double sec_per_px = (total_frames / PSG_SCOPE_FRAMES_PER_SECOND) /
                            (double)(plot_x1 - plot_x0 > 1.0f ? plot_x1 - plot_x0 : 1.0f);
        if (sec_per_px > 0.0)
        {
            /* Adaptive grid step: 1s, 5s, 10s podle hustoty. */
            double grid_step_s = 5.0;
            double total_s = total_frames / PSG_SCOPE_FRAMES_PER_SECOND;
            if (total_s > 60.0)       grid_step_s = 10.0;
            else if (total_s < 12.0)  grid_step_s = 1.0;
            /* Postavíme grid relativně od now zpět. */
            double range_s = total_s;
            int n_lines = (int)(range_s / grid_step_s);
            if (n_lines > 30) n_lines = 30; /* sanity */
            for (int i = 1; i <= n_lines; ++i)
            {
                double frac = 1.0 - (double)i * grid_step_s / range_s;
                if (frac <= 0.0 || frac >= 1.0) continue;
                float x = plot_x0 + (float)(frac * (plot_x1 - plot_x0));
                dl->AddLine(ImVec2(x, plot_y0), ImVec2(x, plot_y1),
                            grid_col, 1.0f);
            }
        }
    }

    /* Vykreslení note bars per (chip, ch). */
    unsigned chip_count_render = g_psg_module.stereo ? 2u : 1u;
    for (unsigned chip = 0; chip < chip_count_render; ++chip)
    {
        for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
        {
            const st_PSG_SCOPE_EVENTS *ev = &g_psg_events[chip][ch];
            ImU32 col = PSG_SCOPE_CHANNEL_COLORS[chip][ch];

            /* Uzavřené eventy. */
            for (unsigned i = 0; i < ev->count; ++i)
            {
                const st_PSG_SCOPE_NOTE_EVENT *n = psg_events_at(ev, i);
                /* Filter mimo time range. */
                if (n->t_off < t_min) continue;
                if (n->t_on > t_max)  continue;

                uint64_t t_on_clip  = n->t_on  < t_min ? t_min : n->t_on;
                uint64_t t_off_clip = n->t_off > t_max ? t_max : n->t_off;
                float fx0 = plot_x0 + (float)(((double)(t_on_clip  - t_min) / total_frames) *
                                              (plot_x1 - plot_x0));
                float fx1 = plot_x0 + (float)(((double)(t_off_clip - t_min) / total_frames) *
                                              (plot_x1 - plot_x0));
                if (fx1 < fx0 + 1.0f) fx1 = fx0 + 1.0f;

                float fy0, fy1;
                if (n->midi_pitch < 0)
                {
                    /* NOISE - noise lane (rozdělíme lane na 4 sub-pruhy
                     * podle channel index aby bylo vidět overlapy). */
                    float sub_h = PSG_SCOPE_ROLL_NOISE_LANE_H / 4.0f;
                    fy0 = pitch_plot_y1 + 2.0f + sub_h * (float)ch;
                    fy1 = fy0 + sub_h - 1.0f;
                }
                else
                {
                    if (n->midi_pitch < pitch_min || n->midi_pitch > pitch_max)
                        continue; /* mimo auto-fit range */
                    fy0 = plot_y0 + (float)(pitch_max - n->midi_pitch) * row_h;
                    fy1 = fy0 + row_h - 0.5f;
                }
                psg_scope_draw_note_bar(dl, fx0, fy0, fx1, fy1, col, n, false);
            }

            /* Aktivní (live) nota - prodloužená do `now`. */
            if (ev->note_active)
            {
                const st_PSG_SCOPE_NOTE_EVENT *n = &ev->active_note;
                if (n->t_on <= t_max)
                {
                    uint64_t t_on_clip  = n->t_on < t_min ? t_min : n->t_on;
                    uint64_t t_off_clip = t_max; /* live = do teď */
                    float fx0 = plot_x0 + (float)(((double)(t_on_clip - t_min) / total_frames) *
                                                  (plot_x1 - plot_x0));
                    float fx1 = plot_x0 + (float)(((double)(t_off_clip - t_min) / total_frames) *
                                                  (plot_x1 - plot_x0));
                    if (fx1 < fx0 + 1.0f) fx1 = fx0 + 1.0f;

                    float fy0, fy1;
                    if (n->midi_pitch < 0)
                    {
                        float sub_h = PSG_SCOPE_ROLL_NOISE_LANE_H / 4.0f;
                        fy0 = pitch_plot_y1 + 2.0f + sub_h * (float)ch;
                        fy1 = fy0 + sub_h - 1.0f;
                    }
                    else if (n->midi_pitch >= pitch_min && n->midi_pitch <= pitch_max)
                    {
                        fy0 = plot_y0 + (float)(pitch_max - n->midi_pitch) * row_h;
                        fy1 = fy0 + row_h - 0.5f;
                    }
                    else
                    {
                        continue; /* mimo range */
                    }
                    psg_scope_draw_note_bar(dl, fx0, fy0, fx1, fy1, col, n, true);
                }
            }
        }
    }

    /* "now" indicator - pravá hrana. */
    dl->AddLine(ImVec2(plot_x1 - 1.0f, plot_y0),
                ImVec2(plot_x1 - 1.0f, plot_y1),
                IM_COL32(220, 220, 100, 200), 1.0f);

    /* Voláme `has_tone` jen pro vizuálně nezavádějící hint pokud nic
     * netuneme (= NOISE-only sessions). Žádná akce, jen UI consistency. */
    (void)has_tone;
}

/* ===================================================================== */
/* F5 - MIDI + CSV export                                                 */
/* ===================================================================== */

/**
 * @brief Persistentní stav exportního subsystému (F5).
 *
 * UI thread only - dialog open flagy, chybové popupy, konfigurace tempa
 * pro MIDI export. Tempo persistuje mezi otevřeními dialogu (= uživatel
 * nemusí přenastavovat při každém exportu).
 *
 * @invariant `tempo_bpm` v rozsahu 40..300 (UI input clamp).
 * @invariant `err_msg` zero-terminated.
 */
struct st_PSG_SCOPE_EXPORT_STATE
{
    bool  csv_dialog_open;       /**< IGFD CSV save dialog je otevřený. */
    bool  midi_dialog_open;      /**< IGFD MIDI save dialog je otevřený. */
    int   tempo_bpm;             /**< MIDI tempo (BPM), 40..300, default 120. */
    bool  error_popup_queue;     /**< true = otevřít error popup v aktuálním framu. */
    char  err_msg[ 512 ];        /**< Lidsky čitelný popis poslední chyby exportu. */
};

/**
 * @brief Globální instance export state.
 *
 * Default: dialogy zavřené, tempo 120 BPM, žádná chyba.
 * UI thread only.
 */
static st_PSG_SCOPE_EXPORT_STATE g_psg_export = {
    false, false, 120, false, { 0 }
};

/**
 * @brief Reprezentace jedné note události pro export (closed + active).
 *
 * Sjednocuje uzavřené noty z `g_psg_events[*][*].events[]` a případně
 * aktivní notu (`active_note` pokud `note_active == true`). Aktivní nota
 * dostane `t_off = g_psg_scope_frame_counter` (= virtuálně uzavřená v
 * okamžiku exportu).
 *
 * Lifetime: dočasná struktura naplněná `psg_scope_collect_notes`, žádný
 * pointer dovnitř `g_psg_events`.
 */
struct st_PSG_SCOPE_EXPORT_NOTE
{
    uint64_t t_on;        /**< Frame counter note_on. */
    uint64_t t_off;       /**< Frame counter note_off (synthetic pro aktivní). */
    int      midi_pitch;  /**< 0..127 nebo -1 pro NOISE. */
    int      cents_detune;/**< -50..+50 pro TONE, 0 pro NOISE. */
    uint8_t  velocity;    /**< 0..127. */
    uint8_t  channel;     /**< 0..3. */
    uint8_t  chip;        /**< 0..1. */

    /* Volume envelope - kopie attn_history seřazená chronologicky od oldest
     * k newest (= ne raw ring s wrap). attn_overflowed signalizuje že došlo
     * k drop nejstarších pointů (= UI tooltip + případný export marker). */
    st_PSG_SCOPE_ATTN_POINT attn_history[PSG_SCOPE_MAX_ATTN_POINTS];
    uint8_t                 attn_history_count;
    bool                    attn_overflowed;
};

/**
 * @brief Linearizuje ring `attn_history` ze zdrojové noty do export struktury.
 *
 * Ring `n->attn_history` má `head` jako index next-write; oldest valid
 * point je při `count < CAP` na indexu 0 a `head == count`, jinak na
 * indexu `head` (= těsně po wrap). Funkce zkopíruje pointy v
 * chronologickém pořadí (oldest first) do `dst`.
 *
 * @param n             Zdrojová nota (NESMÍ být NULL).
 * @param dst           Cíl - export note (NESMÍ být NULL).
 */
static void psg_scope_linearize_attn_history(const st_PSG_SCOPE_NOTE_EVENT *n,
                                             st_PSG_SCOPE_EXPORT_NOTE *dst)
{
    dst->attn_history_count = n->attn_history_count;
    dst->attn_overflowed    = n->attn_history_overflowed;
    if (n->attn_history_count == 0u)
        return;

    unsigned start;
    if (n->attn_history_count < PSG_SCOPE_MAX_ATTN_POINTS)
        start = 0u;
    else
        start = n->attn_history_head; /* po overflow je oldest na head */

    for (unsigned i = 0; i < n->attn_history_count; ++i)
    {
        unsigned idx = (start + i) % PSG_SCOPE_MAX_ATTN_POINTS;
        dst->attn_history[i] = n->attn_history[idx];
    }
}

/**
 * @brief Posbírá všechny noty (closed + active) napříč (chip, channel).
 *
 * Iteruje per (chip, ch) ring buffer events. Aktivní noty získají synthetic
 * `t_off = g_psg_scope_frame_counter`. Výsledek je seřazen vzestupně podle
 * `t_on` (= stable pořadí pro CSV i MIDI tracky).
 *
 * @param out  Cílový vektor (vyprázdněn na začátku).
 */
static void psg_scope_collect_notes ( std::vector< st_PSG_SCOPE_EXPORT_NOTE > &out )
{
    out.clear ( );
    for ( unsigned chip = 0; chip < PSG_MAX_COUNT; ++chip )
    {
        for ( unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch )
        {
            const st_PSG_SCOPE_EVENTS *ev = &g_psg_events[ chip ][ ch ];
            /* Uzavřené noty - iterujeme od oldest k newest přes
             * (head - count) mod capacity. */
            unsigned start = ( ev->head + PSG_SCOPE_EVENTS_CAPACITY - ev->count )
                             % PSG_SCOPE_EVENTS_CAPACITY;
            for ( unsigned i = 0; i < ev->count; ++i )
            {
                unsigned idx = ( start + i ) % PSG_SCOPE_EVENTS_CAPACITY;
                const st_PSG_SCOPE_NOTE_EVENT &n = ev->events[ idx ];
                st_PSG_SCOPE_EXPORT_NOTE e;
                e.t_on         = n.t_on;
                e.t_off        = n.t_off;
                e.midi_pitch   = n.midi_pitch;
                e.cents_detune = n.cents_detune;
                e.velocity     = n.velocity;
                e.channel      = n.channel;
                e.chip         = n.chip;
                psg_scope_linearize_attn_history ( &n, &e );
                out.push_back ( e );
            }
            /* Aktivní nota - synthetic t_off = now. */
            if ( ev->note_active )
            {
                const st_PSG_SCOPE_NOTE_EVENT &n = ev->active_note;
                st_PSG_SCOPE_EXPORT_NOTE e;
                e.t_on         = n.t_on;
                e.t_off        = g_psg_scope_frame_counter;
                if ( e.t_off < e.t_on ) e.t_off = e.t_on;
                e.midi_pitch   = n.midi_pitch;
                e.cents_detune = n.cents_detune;
                e.velocity     = n.velocity;
                e.channel      = n.channel;
                e.chip         = n.chip;
                psg_scope_linearize_attn_history ( &n, &e );
                out.push_back ( e );
            }
        }
    }
    std::sort ( out.begin ( ), out.end ( ),
                []( const st_PSG_SCOPE_EXPORT_NOTE &a,
                    const st_PSG_SCOPE_EXPORT_NOTE &b )
                {
                    if ( a.t_on != b.t_on ) return a.t_on < b.t_on;
                    if ( a.chip != b.chip ) return a.chip < b.chip;
                    return a.channel < b.channel;
                } );
}

/* --------------------------------------------------------------------- */
/* CSV export                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Zapíše posbírané noty do CSV souboru.
 *
 * Formát:
 *   header `time_s,channel,chip,pitch_midi,note_name,duration_s,velocity,cents,attn_changes`
 *   per nota: `<t_on/g_mzhal.gdgclk_base>,<ch>,<chip>,<pitch | -1>,<name | NOISE>,<dur>,<vel>,<cents>,<attn_history_count>`
 *
 * Sloupec `attn_changes` (0..PSG_SCOPE_MAX_ATTN_POINTS) = počet volume
 * envelope změn během noty (= MIDI CC 7 eventů v ekvivalentním MIDI
 * exportu). 0 = nota se nemodulovala (constant volume), > 0 = vibrato /
 * fade / 1-bit PCM atd. Pokud `attn_overflowed`, count je clipped na
 * kapacitu - důsledek pro CSV uživatele: ta hodnota je dolní hranice.
 *
 * Floats jsou formátované přes `g_ascii_formatd("%.3f", ...)` (= zaručené
 * `.` jako desetinný oddělovač, locale-safe).
 *
 * UTF-8 bez BOM, LF line endings (`\n`). `fopen(..., "wb")` zabrání
 * Windows automatickému CRLF rewrite.
 *
 * @param path        Cesta k cílovému souboru.
 * @param notes       Sebrané noty (sorted by t_on).
 * @param err_msg     Out buffer pro chybový popis (pri false).
 * @param err_msg_sz  Velikost @p err_msg.
 * @return true při úspěchu, false při I/O chybě.
 */
static bool psg_scope_export_csv ( const char *path,
                                   const std::vector< st_PSG_SCOPE_EXPORT_NOTE > &notes,
                                   char *err_msg, size_t err_msg_sz )
{
    if ( err_msg && err_msg_sz ) err_msg[ 0 ] = 0;

    FILE *f = g_fopen ( path, "wb" );
    if ( !f )
    {
        if ( err_msg && err_msg_sz )
        {
            snprintf ( err_msg, err_msg_sz,
                       "Cannot open '%s' for writing (errno=%d).",
                       path, errno );
        }
        return false;
    }

    if ( fputs ( "time_s,channel,chip,pitch_midi,note_name,duration_s,velocity,cents,attn_changes\n",
                 f ) < 0 ) goto write_err;

    for ( const st_PSG_SCOPE_EXPORT_NOTE &n : notes )
    {
        double t_s   = (double) n.t_on / PSG_SCOPE_FRAMES_PER_SECOND;
        uint64_t dur_frames = ( n.t_off >= n.t_on ) ? ( n.t_off - n.t_on ) : 0ULL;
        double dur_s = (double) dur_frames / PSG_SCOPE_FRAMES_PER_SECOND;

        char t_buf[ G_ASCII_DTOSTR_BUF_SIZE ];
        char dur_buf[ G_ASCII_DTOSTR_BUF_SIZE ];
        /* %.3f = 1 ms rozlišení, dostatečné pro typický UI polling rate. */
        g_ascii_formatd ( t_buf,   sizeof ( t_buf ),   "%.3f", t_s );
        g_ascii_formatd ( dur_buf, sizeof ( dur_buf ), "%.3f", dur_s );

        char name_buf[ 16 ];
        if ( n.midi_pitch < 0 )
        {
            snprintf ( name_buf, sizeof ( name_buf ), "NOISE" );
        }
        else
        {
            psg_scope_midi_pitch_to_name ( n.midi_pitch, name_buf, sizeof ( name_buf ) );
        }

        if ( fprintf ( f, "%s,%u,%u,%d,%s,%s,%u,%d,%u\n",
                       t_buf,
                       (unsigned) n.channel,
                       (unsigned) n.chip,
                       n.midi_pitch,
                       name_buf,
                       dur_buf,
                       (unsigned) n.velocity,
                       n.cents_detune,
                       (unsigned) n.attn_history_count ) < 0 )
        {
            goto write_err;
        }
    }

    if ( fclose ( f ) != 0 )
    {
        f = NULL;
        if ( err_msg && err_msg_sz )
        {
            snprintf ( err_msg, err_msg_sz,
                       "Failed to close '%s' (errno=%d).", path, errno );
        }
        return false;
    }
    return true;

write_err:
    if ( err_msg && err_msg_sz )
    {
        snprintf ( err_msg, err_msg_sz,
                   "Write error on '%s' (errno=%d).", path, errno );
    }
    if ( f ) fclose ( f );
    return false;
}

/* --------------------------------------------------------------------- */
/* MIDI export                                                           */
/* --------------------------------------------------------------------- */

/**
 * @brief Pulses Per Quarter Note pro SMF division pole.
 *
 * Standardní hodnota 480 PPQN dává dostatečné rozlišení pro běžnou
 * hudební notaci (= 1/128 nota přesnost při čtyřtaktovém metru).
 */
#define PSG_MIDI_PPQN 480

/**
 * @brief MIDI drum kanál (= "channel 10" v 1-based notaci, 9 v 0-based).
 *
 * NOISE eventy se mapují sem podle MIDI standardu (perkuse). Pitch
 * `PSG_MIDI_NOISE_DRUM_NOTE` = 38 (Acoustic Snare).
 */
#define PSG_MIDI_DRUM_CHANNEL_0BASED 9

/**
 * @brief MIDI pitch pro NOISE eventy v drum kanálu.
 *
 * 38 = Acoustic Snare (General MIDI drum kit). Volba placeholder - PSG
 * NOISE generuje pseudo-random pattern, který nemá pevný pitch ekvivalent.
 */
#define PSG_MIDI_NOISE_DRUM_NOTE 38

/**
 * @brief General MIDI program pro TONE tracky - 80 = Lead 1 (Square wave).
 *
 * PSG SN76489 generuje square wave, takže Square Lead je nejvěrnější
 * GM patch. Důvod existence: Windows MediaPlayer default GM synth =
 * Acoustic Grand Piano (program 0) má piano range A0..C8 (pitch
 * 21..108). PSG produkuje typicky vysoké tóny (min freq při div=1023
 * je 541 Hz = C5, ale běžně E7..C#9 = pitch 100..121). Pitch > 108
 * piano synth NEUMÍ → ticho. Explicitní Program Change na 80 zajistí
 * že player použije patch s plným rozsahem.
 *
 * Drum tracky (NOISE = MIDI ch 9) Program Change NEdostávají - drum
 * channel má implicit drum kit nezávisle na PC.
 */
#define PSG_MIDI_TONE_PROGRAM 80

/**
 * @brief Zapíše variable-length quantity do bufferu (MIDI VLQ encoding).
 *
 * Standardní MIDI delta-time formát: 7 bitů per byte, MSB = continuation
 * (=1 v non-poslední byte, =0 v posledním). Rozsah 0..0x0FFFFFFF (= 28
 * bitů payload, max 4 bytes).
 *
 * [neověřeno - VLQ algoritmus opsán z běžných MIDI 1.0 spec
 * implementací; cross-check s `mido` / fluidsynth ne, manual smoke
 * test až mimo F5 scope.]
 *
 * @param buf    Output buffer (alespoň 4 bytes).
 * @param value  Hodnota k zapsání (0..0x0FFFFFFF).
 * @return Počet zapsaných bytů (1..4).
 */
static int psg_midi_write_vlq ( uint8_t *buf, uint32_t value )
{
    uint8_t tmp[ 4 ];
    int n = 0;
    tmp[ n++ ] = (uint8_t) ( value & 0x7Fu );
    value >>= 7;
    while ( value > 0u && n < 4 )
    {
        tmp[ n++ ] = (uint8_t) ( ( value & 0x7Fu ) | 0x80u );
        value >>= 7;
    }
    /* Reverz: nejvyšší skupina musí jít první (= big-endian semantika). */
    for ( int i = 0; i < n; ++i )
    {
        buf[ i ] = tmp[ n - 1 - i ];
    }
    return n;
}

/**
 * @brief Zapíše 32-bit big-endian hodnotu na pozici v souboru.
 *
 * Slouží pro zpětné vyplnění MTrk length pole po napsání obsahu tracku.
 *
 * @param f      Soubor (otevřený "wb", musí podporovat fseek).
 * @param pos    Pozice (byte offset) k seek.
 * @param value  32-bit hodnota.
 * @return true při úspěšném zápisu, false při I/O chybě.
 */
static bool psg_midi_patch_be32 ( FILE *f, long pos, uint32_t value )
{
    long cur = ftell ( f );
    if ( cur < 0 ) return false;
    if ( fseek ( f, pos, SEEK_SET ) != 0 ) return false;
    uint8_t b[ 4 ] = {
        (uint8_t) ( ( value >> 24 ) & 0xFFu ),
        (uint8_t) ( ( value >> 16 ) & 0xFFu ),
        (uint8_t) ( ( value >>  8 ) & 0xFFu ),
        (uint8_t) (   value         & 0xFFu )
    };
    if ( fwrite ( b, 1, 4, f ) != 4 ) return false;
    if ( fseek ( f, cur, SEEK_SET ) != 0 ) return false;
    return true;
}

/**
 * @brief Zkonvertuje pxCLK total ticks na MIDI ticks @ PPQN.
 *
 * Postup:
 *   seconds   = frames / g_mzhal.gdgclk_base      (= 17.7345 MHz pro MZ-800)
 *   quarters  = seconds * (tempo_bpm / 60)
 *   ticks     = quarters * PPQN
 *
 * Vstup `frames` má sémantiku `gdg_get_total_ticks()` - pxCLK monotonní
 * timebase. Konverze nezávisí na UI render frekvenci.
 *
 * @param frames     pxCLK total ticks (sémantika `g_psg_scope_frame_counter`).
 * @param tempo_bpm  Tempo v BPM (40..300).
 * @return Tick počet (zaokrouhlený).
 */
static uint32_t psg_midi_frames_to_ticks ( uint64_t frames, int tempo_bpm )
{
    if ( tempo_bpm <= 0 ) tempo_bpm = 120;
    double sec      = (double) frames / PSG_SCOPE_FRAMES_PER_SECOND;
    double quarters = sec * ( (double) tempo_bpm / 60.0 );
    double ticks    = quarters * (double) PSG_MIDI_PPQN;
    if ( ticks < 0.0 ) ticks = 0.0;
    return (uint32_t) llround ( ticks );
}

/**
 * @brief Jednoduchá MIDI track-event reprezentace pro mezivýpočet.
 *
 * Drží absolute tick (= ne delta) + raw bytes events bez delta prefixu.
 * Po seřazení podle `abs_tick` se delta dopočítají při zápisu do souboru.
 */
struct st_PSG_MIDI_EVT
{
    uint32_t  abs_tick;  /**< Absolutní tick events. */
    uint8_t   data[ 4 ]; /**< Event bytes (status + 1-2 data; bez delta). */
    uint8_t   len;       /**< Počet validních bytů v `data`. */
    uint8_t   order;     /**< Tie-break: 0=note_off před 1=note_on při stejném ticku. */
};

/**
 * @brief Zapíše posbírané noty do MIDI Standard File (SMF) type 1.
 *
 * Track layout:
 *   Track 0 = conductor (tempo meta + time signature 4/4 + EoT)
 *   Track 1..N = per (chip, channel) - 4 nebo 8 tracků dle stereo runtime
 *
 * Note events:
 *   delta=ticks: Note On  (90+ch_0base) <pitch> <velocity>
 *   delta=dur:   Note Off (80+ch_0base) <pitch> 0
 *
 * NOISE → MIDI drum channel (9 v 0-based = "kanál 10" v 1-based), pitch
 * 38 (Acoustic Snare).
 *
 * Header (MThd):
 *   "MThd" 0x00000006 (length) format=1 ntracks PPQN(BE)
 *
 * Track (MTrk):
 *   "MTrk" <length placeholder, patchnuto po dopsání obsahu> <events>
 *
 * Encoding:
 *   - Delta-time = VLQ
 *   - Tempo meta = FF 51 03 <us_per_quarter:24-bit BE>
 *   - Time signature = FF 58 04 04 02 18 08 (= 4/4, 24 MIDI clocks/click,
 *     8 32nd-notes/quarter)
 *   - Track name = FF 03 <vlq-len> <ascii>
 *   - End of Track = FF 2F 00
 *
 * [neověřeno - SMF byte encoding opsán z MIDI 1.0 spec a běžných
 * implementací; manual smoke test playerem mimo F5 scope.]
 *
 * @param path        Cesta k cílovému souboru.
 * @param notes       Sebrané noty.
 * @param tempo_bpm   Tempo v BPM (UI input, 40..300).
 * @param err_msg     Out buffer pro chybu.
 * @param err_msg_sz  Velikost @p err_msg.
 * @return true při úspěchu, false při I/O / encoding chybě.
 */
static bool psg_scope_export_midi ( const char *path,
                                    const std::vector< st_PSG_SCOPE_EXPORT_NOTE > &notes,
                                    int tempo_bpm,
                                    char *err_msg, size_t err_msg_sz )
{
    if ( err_msg && err_msg_sz ) err_msg[ 0 ] = 0;
    if ( tempo_bpm < 40 )  tempo_bpm = 40;
    if ( tempo_bpm > 300 ) tempo_bpm = 300;

    FILE *f = g_fopen ( path, "wb" );
    if ( !f )
    {
        if ( err_msg && err_msg_sz )
        {
            snprintf ( err_msg, err_msg_sz,
                       "Cannot open '%s' for writing (errno=%d).",
                       path, errno );
        }
        return false;
    }

    /* Rozhodnutí o počtu trackových (mono = 4 channels, stereo = 8). */
    bool is_stereo = g_psg_module.stereo;
    unsigned chip_count = is_stereo ? 2u : 1u;
    unsigned per_ch_track_count = chip_count * PSG_CHANNELS_COUNT;
    unsigned total_tracks = 1u + per_ch_track_count; /* + conductor */

    /* Časová normalizace - odečteme nejmenší t_on napříč všemi notami,
     * aby první nota začínala na ticku 0 (jinak by VLQ delta první noty
     * obsahovala celý frame counter od startu emu = stovky tisíc ticků,
     * a MediaPlayer by nikdy nezahrál).
     *
     * Také spočteme max t_off (= délka songu v frames), abychom mohli
     * conductor track EoT umístit aspoň na konec songu - jinak by player
     * zastavil playback na ticku 0 (= ticho). */
    uint64_t t_origin = UINT64_MAX;
    uint64_t t_end    = 0u;
    for ( const st_PSG_SCOPE_EXPORT_NOTE &n : notes )
    {
        if ( n.t_off <= n.t_on ) continue;
        if ( n.t_on  < t_origin ) t_origin = n.t_on;
        if ( n.t_off > t_end )    t_end    = n.t_off;
    }
    if ( t_origin == UINT64_MAX ) t_origin = 0u;
    uint64_t song_frames = ( t_end > t_origin ) ? ( t_end - t_origin ) : 0u;
    uint32_t song_end_tick = psg_midi_frames_to_ticks ( song_frames, tempo_bpm );

    /* --- MThd --- */
    uint8_t mthd[ 14 ] = {
        'M','T','h','d',
        0x00, 0x00, 0x00, 0x06,           /* length = 6 */
        0x00, 0x01,                        /* format = 1 */
        (uint8_t) ( ( total_tracks >> 8 ) & 0xFFu ),
        (uint8_t) (   total_tracks        & 0xFFu ),
        (uint8_t) ( ( PSG_MIDI_PPQN >> 8 ) & 0xFFu ),
        (uint8_t) (   PSG_MIDI_PPQN        & 0xFFu )
    };
    if ( fwrite ( mthd, 1, sizeof ( mthd ), f ) != sizeof ( mthd ) ) goto write_err;

    /* --- Track 0: conductor --- */
    {
        if ( fwrite ( "MTrk", 1, 4, f ) != 4 ) goto write_err;
        long len_pos = ftell ( f );
        if ( len_pos < 0 ) goto write_err;
        uint8_t zeros[ 4 ] = { 0, 0, 0, 0 };
        if ( fwrite ( zeros, 1, 4, f ) != 4 ) goto write_err;
        long content_begin = ftell ( f );
        if ( content_begin < 0 ) goto write_err;

        /* Tempo: FF 51 03 <us_per_quarter:24bit BE>. */
        uint32_t us_per_quarter = (uint32_t) ( 60000000.0 / (double) tempo_bpm );
        uint8_t buf[ 16 ];
        int idx = 0;
        idx += psg_midi_write_vlq ( buf + idx, 0u ); /* delta=0 */
        buf[ idx++ ] = 0xFF;
        buf[ idx++ ] = 0x51;
        buf[ idx++ ] = 0x03;
        buf[ idx++ ] = (uint8_t) ( ( us_per_quarter >> 16 ) & 0xFFu );
        buf[ idx++ ] = (uint8_t) ( ( us_per_quarter >>  8 ) & 0xFFu );
        buf[ idx++ ] = (uint8_t) (   us_per_quarter         & 0xFFu );
        if ( fwrite ( buf, 1, (size_t) idx, f ) != (size_t) idx ) goto write_err;

        /* Time signature: 4/4, 24 MIDI clocks/click, 8 32nd-notes per quarter. */
        uint8_t ts[] = {
            0x00,                           /* delta=0 (VLQ 1B) */
            0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08
        };
        if ( fwrite ( ts, 1, sizeof ( ts ), f ) != sizeof ( ts ) ) goto write_err;

        /* End of Track na ticku song_end_tick. SMF type 1 vyžaduje aby
         * conductor track byl aspoň tak dlouhý jako per-track tracks -
         * jinak některé playery (Windows MediaPlayer) zastaví playback
         * na conductor EoT (= ticho po prvním ticku). */
        {
            uint8_t vlq[ 4 ];
            int vlen = psg_midi_write_vlq ( vlq, song_end_tick );
            if ( fwrite ( vlq, 1, (size_t) vlen, f ) != (size_t) vlen ) goto write_err;
            uint8_t eot[] = { 0xFF, 0x2F, 0x00 };
            if ( fwrite ( eot, 1, sizeof ( eot ), f ) != sizeof ( eot ) ) goto write_err;
        }

        long content_end = ftell ( f );
        if ( content_end < 0 ) goto write_err;
        uint32_t track_len = (uint32_t) ( content_end - content_begin );
        if ( !psg_midi_patch_be32 ( f, len_pos, track_len ) ) goto write_err;
    }

    /* --- Track 1..N: per (chip, channel) --- */
    for ( unsigned chip = 0; chip < chip_count; ++chip )
    {
        for ( unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch )
        {
            /* Posbírej eventy pro tento (chip, ch). */
            std::vector< st_PSG_MIDI_EVT > evs;
            for ( const st_PSG_SCOPE_EXPORT_NOTE &n : notes )
            {
                if ( n.chip != chip || n.channel != ch ) continue;
                if ( n.t_off <= n.t_on ) continue; /* zero-length skip */

                bool is_noise = ( n.midi_pitch < 0 );
                uint8_t midi_ch_0base = is_noise
                                        ? (uint8_t) PSG_MIDI_DRUM_CHANNEL_0BASED
                                        : (uint8_t) ( ch & 0x0Fu );
                uint8_t pitch = is_noise
                                ? (uint8_t) PSG_MIDI_NOISE_DRUM_NOTE
                                : (uint8_t) ( n.midi_pitch & 0x7F );
                uint8_t vel = (uint8_t) ( n.velocity & 0x7F );

                /* Normalizace - odečteme t_origin aby první nota celého
                 * exportu začínala na ticku 0 (= player nezahrabe čekáním
                 * na ticku 753000+ frame counter). */
                uint64_t t_on_rel  = ( n.t_on  > t_origin ) ? ( n.t_on  - t_origin ) : 0u;
                uint64_t t_off_rel = ( n.t_off > t_origin ) ? ( n.t_off - t_origin ) : 0u;
                uint32_t t_on_ticks  = psg_midi_frames_to_ticks ( t_on_rel,  tempo_bpm );
                uint32_t t_off_ticks = psg_midi_frames_to_ticks ( t_off_rel, tempo_bpm );
                if ( t_off_ticks <= t_on_ticks )
                {
                    /* Po zaokrouhlení může mít nota duration < 1 tick;
                     * vynutit alespoň 1 aby Note Off nepřepadl pres Note On. */
                    t_off_ticks = t_on_ticks + 1u;
                }

                st_PSG_MIDI_EVT non;
                non.abs_tick = t_on_ticks;
                non.data[ 0 ] = (uint8_t) ( 0x90u | midi_ch_0base );
                non.data[ 1 ] = pitch;
                non.data[ 2 ] = vel;
                non.len   = 3;
                non.order = 1; /* Note On po Note Off při stejném ticku. */
                evs.push_back ( non );

                st_PSG_MIDI_EVT noff;
                noff.abs_tick = t_off_ticks;
                noff.data[ 0 ] = (uint8_t) ( 0x80u | midi_ch_0base );
                noff.data[ 1 ] = pitch;
                noff.data[ 2 ] = 0;
                noff.len   = 3;
                noff.order = 0; /* Note Off před Note On při stejném ticku. */
                evs.push_back ( noff );

                /* Volume envelope - emit CC 7 (Channel Volume) na každý
                 * attn change point mezi note_on a note_off. Mapování:
                 *   value = round(127 * (15 - attn) / 15)
                 *   (= stejný vztah jako velocity z attn).
                 *
                 * `order = 2` (po Note On) zajistí, že CC při shodném
                 * ticku přijde až po note_on. Pro NOISE drum kanál
                 * (channel 9) by CC 7 ovlivnil drum volume - export
                 * je tu emituje shodně (= konzistentní envelope; pokud
                 * uživatel chce drum bez expression, CC ignoruje
                 * většina drum patchů). */
                for ( unsigned i = 0; i < n.attn_history_count; ++i )
                {
                    const st_PSG_SCOPE_ATTN_POINT &ap = n.attn_history[ i ];
                    if ( ap.t_ticks < t_origin ) continue;
                    uint64_t ap_rel = ap.t_ticks - t_origin;
                    uint32_t ap_ticks = psg_midi_frames_to_ticks ( ap_rel, tempo_bpm );
                    if ( ap_ticks <= t_on_ticks ) ap_ticks = t_on_ticks + 1u;
                    if ( ap_ticks >= t_off_ticks ) continue; /* po note_off skip */

                    int v = (int) lround ( 127.0 * ( 15.0 - (double) ap.attn ) / 15.0 );
                    if ( v < 0 )   v = 0;
                    if ( v > 127 ) v = 127;

                    st_PSG_MIDI_EVT cc;
                    cc.abs_tick = ap_ticks;
                    cc.data[ 0 ] = (uint8_t) ( 0xB0u | midi_ch_0base );
                    cc.data[ 1 ] = 7u; /* CC 7 = Channel Volume MSB */
                    cc.data[ 2 ] = (uint8_t) v;
                    cc.len   = 3;
                    cc.order = 2; /* CC po Note On / před Note Off při shodném ticku */
                    evs.push_back ( cc );
                }
            }
            std::sort ( evs.begin ( ), evs.end ( ),
                        []( const st_PSG_MIDI_EVT &a, const st_PSG_MIDI_EVT &b )
                        {
                            if ( a.abs_tick != b.abs_tick ) return a.abs_tick < b.abs_tick;
                            return a.order < b.order;
                        } );

            /* MTrk hlavička + placeholder length. */
            if ( fwrite ( "MTrk", 1, 4, f ) != 4 ) goto write_err;
            long len_pos = ftell ( f );
            if ( len_pos < 0 ) goto write_err;
            uint8_t zeros[ 4 ] = { 0, 0, 0, 0 };
            if ( fwrite ( zeros, 1, 4, f ) != 4 ) goto write_err;
            long content_begin = ftell ( f );
            if ( content_begin < 0 ) goto write_err;

            /* Track name meta: FF 03 <vlq-len> <ascii>. */
            char tn_buf[ 48 ];
            if ( is_stereo )
            {
                snprintf ( tn_buf, sizeof ( tn_buf ),
                           "CH%u %s chip %u", ch,
                           ( ch == 3u ) ? "NOISE" : "TONE",
                           chip );
            }
            else
            {
                snprintf ( tn_buf, sizeof ( tn_buf ),
                           "CH%u %s", ch,
                           ( ch == 3u ) ? "NOISE" : "TONE" );
            }
            size_t tn_len = strlen ( tn_buf );
            if ( tn_len > 127u ) tn_len = 127u; /* VLQ 1B bez problémů */

            uint8_t hdr[ 8 ];
            int hi = 0;
            hi += psg_midi_write_vlq ( hdr + hi, 0u ); /* delta=0 */
            hdr[ hi++ ] = 0xFF;
            hdr[ hi++ ] = 0x03;
            hi += psg_midi_write_vlq ( hdr + hi, (uint32_t) tn_len );
            if ( fwrite ( hdr, 1, (size_t) hi, f ) != (size_t) hi ) goto write_err;
            if ( tn_len > 0
                 && fwrite ( tn_buf, 1, tn_len, f ) != tn_len ) goto write_err;

            /* Program Change na Lead 1 (Square wave) pro TONE tracky.
             * Drum tracky (= všechny eventy v tracku míří na drum channel)
             * Program Change nedostávají - drum channel ignoruje PC a
             * používá implicit drum kit. CH3 v MIX TONE+NOISE režimu má
             * v evs jak tone tak drum eventy; v tom případě PC na MIDI
             * channel 3 nastaví patch pro tone podčást, drum eventy na
             * ch 9 zůstanou perkusivní.
             *
             * Detekce drum-only: žádná z evs nemá Note On opcode `0x90`
             * pro tone channel (= bez bitu drum kanálu). Stačí mrknout na
             * status nibble = `evs[i].data[0] >> 4 == 0x9` && low nibble
             * != drum channel. */
            bool has_tone_event = false;
            for ( const st_PSG_MIDI_EVT &e : evs )
            {
                uint8_t st = e.data[ 0 ];
                if ( ( st & 0xF0u ) == 0x90u
                     && ( st & 0x0Fu ) != (uint8_t) PSG_MIDI_DRUM_CHANNEL_0BASED )
                {
                    has_tone_event = true;
                    break;
                }
            }
            if ( has_tone_event )
            {
                /* PC = delta(0) + status(0xC0 | ch) + program byte. */
                uint8_t pc[ 3 ];
                pc[ 0 ] = 0x00;                                  /* delta=0 VLQ */
                pc[ 1 ] = (uint8_t) ( 0xC0u | ( ch & 0x0Fu ) );  /* Program Change */
                pc[ 2 ] = (uint8_t) PSG_MIDI_TONE_PROGRAM;       /* 80 = Square Lead */
                if ( fwrite ( pc, 1, sizeof ( pc ), f ) != sizeof ( pc ) ) goto write_err;
            }

            /* Note events. */
            uint32_t prev_tick = 0u;
            for ( const st_PSG_MIDI_EVT &e : evs )
            {
                uint32_t delta = e.abs_tick - prev_tick;
                prev_tick = e.abs_tick;
                uint8_t vlq[ 4 ];
                int vlen = psg_midi_write_vlq ( vlq, delta );
                if ( fwrite ( vlq,    1, (size_t) vlen, f ) != (size_t) vlen ) goto write_err;
                if ( fwrite ( e.data, 1, e.len,         f ) != e.len )         goto write_err;
            }

            /* End of Track. */
            uint8_t eot[] = { 0x00, 0xFF, 0x2F, 0x00 };
            if ( fwrite ( eot, 1, sizeof ( eot ), f ) != sizeof ( eot ) ) goto write_err;

            long content_end = ftell ( f );
            if ( content_end < 0 ) goto write_err;
            uint32_t track_len = (uint32_t) ( content_end - content_begin );
            if ( !psg_midi_patch_be32 ( f, len_pos, track_len ) ) goto write_err;
        }
    }

    if ( fclose ( f ) != 0 )
    {
        f = NULL;
        if ( err_msg && err_msg_sz )
        {
            snprintf ( err_msg, err_msg_sz,
                       "Failed to close '%s' (errno=%d).", path, errno );
        }
        return false;
    }
    return true;

write_err:
    if ( err_msg && err_msg_sz )
    {
        snprintf ( err_msg, err_msg_sz,
                   "Write error on '%s' (errno=%d).", path, errno );
    }
    if ( f ) fclose ( f );
    return false;
}

/* --------------------------------------------------------------------- */
/* Otevření dialogů + render handlerů                                    */
/* --------------------------------------------------------------------- */

/**
 * @brief Vytvoří default filename s timestampem pro export.
 *
 * Formát: `psg_audio_scope_YYYYMMDD_hhmmss.<ext>`.
 *
 * @param buf     Output buffer.
 * @param buflen  Velikost bufferu.
 * @param ext     Přípona bez tečky ("csv" / "mid").
 */
static void psg_scope_make_default_name ( char *buf, size_t buflen, const char *ext )
{
    time_t now = time ( NULL );
    struct tm *tmv = localtime ( &now );
    if ( tmv )
    {
        snprintf ( buf, buflen,
                   "psg_audio_scope_%04d%02d%02d_%02d%02d%02d.%s",
                   tmv->tm_year + 1900,
                   tmv->tm_mon + 1,
                   tmv->tm_mday,
                   tmv->tm_hour,
                   tmv->tm_min,
                   tmv->tm_sec,
                   ext );
    }
    else
    {
        snprintf ( buf, buflen, "psg_audio_scope.%s", ext );
    }
}

/**
 * @brief Otevře IGFD dialog pro export CSV.
 *
 * Side effect: nastaví `g_psg_export.csv_dialog_open = true`. Default
 * filename obsahuje timestamp aby uživatel nepřepisoval předchozí export.
 */
static void psg_scope_open_csv_dialog ( void )
{
    static char default_name[ 96 ];
    psg_scope_make_default_name ( default_name, sizeof ( default_name ), "csv" );

    IGFD::FileDialogConfig config;
    config.path              = ".";
    config.fileName          = default_name;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                 | ImGuiFileDialogFlags_DontShowHiddenFiles
                 | ImGuiFileDialogFlags_ShowDevicesButton
                 | ImGuiFileDialogFlags_ConfirmOverwrite;

    ImGuiFileDialog::Instance ( )->OpenDialog (
        "PSGScopeExportCSV",
        _( "Export PSG notes as CSV" ),
        ".csv,.*",
        config );
    g_psg_export.csv_dialog_open = true;
}

/**
 * @brief Otevře IGFD dialog pro export MIDI.
 *
 * Side effect: nastaví `g_psg_export.midi_dialog_open = true`.
 */
static void psg_scope_open_midi_dialog ( void )
{
    static char default_name[ 96 ];
    psg_scope_make_default_name ( default_name, sizeof ( default_name ), "mid" );

    IGFD::FileDialogConfig config;
    config.path              = ".";
    config.fileName          = default_name;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                 | ImGuiFileDialogFlags_DontShowHiddenFiles
                 | ImGuiFileDialogFlags_ShowDevicesButton
                 | ImGuiFileDialogFlags_ConfirmOverwrite;

    ImGuiFileDialog::Instance ( )->OpenDialog (
        "PSGScopeExportMIDI",
        _( "Export PSG notes as MIDI" ),
        ".mid,.midi,.*",
        config );
    g_psg_export.midi_dialog_open = true;
}

/**
 * @brief Render handlery IGFD dialogů (CSV + MIDI).
 *
 * Voláno z `imgui_psg_audio_scope_window` každý frame. Při potvrzení
 * dialogu sestaví seznam not přes `psg_scope_collect_notes` a zavolá
 * příslušnou export funkci. Chyba zařadí error popup do queue.
 *
 * Side effects: může resetovat `csv_dialog_open` / `midi_dialog_open`.
 */
static void psg_scope_render_export_dialogs ( void )
{
    if ( g_psg_export.csv_dialog_open )
    {
        ImGui::SetNextWindowSize ( ImVec2 ( 800.0f, 500.0f ),
                                   ImGuiCond_FirstUseEver );
        if ( ImGuiFileDialog::Instance ( )->Display ( "PSGScopeExportCSV" ) )
        {
            if ( ImGuiFileDialog::Instance ( )->IsOk ( ) )
            {
                std::string file_path =
                    ImGuiFileDialog::Instance ( )->GetFilePathName ( );
                std::vector< st_PSG_SCOPE_EXPORT_NOTE > notes;
                psg_scope_collect_notes ( notes );

                char err_buf[ 256 ];
                err_buf[ 0 ] = 0;
                bool ok = psg_scope_export_csv ( file_path.c_str ( ), notes,
                                                 err_buf, sizeof ( err_buf ) );
                if ( !ok )
                {
                    g_strlcpy ( g_psg_export.err_msg, err_buf,
                                sizeof ( g_psg_export.err_msg ) );
                    g_psg_export.error_popup_queue = true;
                }
            }
            ImGuiFileDialog::Instance ( )->Close ( );
            g_psg_export.csv_dialog_open = false;
        }
    }

    if ( g_psg_export.midi_dialog_open )
    {
        ImGui::SetNextWindowSize ( ImVec2 ( 800.0f, 500.0f ),
                                   ImGuiCond_FirstUseEver );
        if ( ImGuiFileDialog::Instance ( )->Display ( "PSGScopeExportMIDI" ) )
        {
            if ( ImGuiFileDialog::Instance ( )->IsOk ( ) )
            {
                std::string file_path =
                    ImGuiFileDialog::Instance ( )->GetFilePathName ( );
                std::vector< st_PSG_SCOPE_EXPORT_NOTE > notes;
                psg_scope_collect_notes ( notes );

                char err_buf[ 256 ];
                err_buf[ 0 ] = 0;
                bool ok = psg_scope_export_midi ( file_path.c_str ( ), notes,
                                                  g_psg_export.tempo_bpm,
                                                  err_buf, sizeof ( err_buf ) );
                if ( !ok )
                {
                    g_strlcpy ( g_psg_export.err_msg, err_buf,
                                sizeof ( g_psg_export.err_msg ) );
                    g_psg_export.error_popup_queue = true;
                }
            }
            ImGuiFileDialog::Instance ( )->Close ( );
            g_psg_export.midi_dialog_open = false;
        }
    }

    /* Error popup (otevírá se v aktuálním framu pokud queued). */
    if ( g_psg_export.error_popup_queue )
    {
        ImGui::OpenPopup ( "###PSGScopeExportError" );
        g_psg_export.error_popup_queue = false;
    }
    if ( ImGui::BeginPopupModal ( _L ( "Export error###PSGScopeExportError" ),
                                  NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::TextUnformatted ( _( "Failed to open file for writing" ) );
        ImGui::Separator ( );
        ImGui::TextUnformatted ( g_psg_export.err_msg );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "OK###PSGScopeExportErrorOK" ),
                              ImVec2 ( 120.0f, 0.0f ) ) )
        {
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }
}

/**
 * @brief Vykreslí horní akční toolbar (Export MIDI / CSV / Tempo BPM / Clear).
 *
 * Informační widgety (počty, log toggle, status text) jsou v dolní status
 * line - viz psg_scope_render_statusline().
 *
 * Disabled stavy: pokud nejsou žádné události (active ani closed) v ringách,
 * Export tlačítka jsou neaktivní + tooltip vysvětlí důvod.
 */
static void psg_scope_render_export_toolbar ( void )
{
    /* Total notes including active (= aspoň 1 zvuk aktivní hraje). */
    unsigned total = psg_audio_scope_total_notes ( );
    bool any_active = false;
    for ( unsigned chip = 0; chip < PSG_MAX_COUNT && !any_active; ++chip )
        for ( unsigned ch = 0; ch < PSG_CHANNELS_COUNT && !any_active; ++ch )
            if ( g_psg_events[ chip ][ ch ].note_active )
                any_active = true;
    bool has_any = ( total > 0u ) || any_active;

    ImGui::BeginDisabled ( !has_any );
    if ( ImGui::Button ( _L ( "Export MIDI...###PSGScopeExportMidiBtn" ) ) )
    {
        psg_scope_open_midi_dialog ( );
    }
    ImGui::EndDisabled ( );
    if ( !has_any && ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) )
    {
        ImGui::SetTooltip ( "%s", _( "No notes recorded yet" ) );
    }

    ImGui::SameLine ( );
    ImGui::BeginDisabled ( !has_any );
    if ( ImGui::Button ( _L ( "Export CSV...###PSGScopeExportCsvBtn" ) ) )
    {
        psg_scope_open_csv_dialog ( );
    }
    ImGui::EndDisabled ( );
    if ( !has_any && ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) )
    {
        ImGui::SetTooltip ( "%s", _( "No notes recorded yet" ) );
    }

    ImGui::SameLine ( );
    /* Šířka InputInt pole = ~140px, aby se vešlo "300" + step buttony.
     * Default ImGui width oříznul 3-ciferné hodnoty na "12".
     *
     * Tempo BPM je čistě MIDI metadata pro export - pro vlastní playback
     * irelevantní (časování se počítá ze sekund). Default 120 BPM odpovídá
     * MIDI standardu. */
    ImGui::SetNextItemWidth ( 140.0f );
    if ( ImGui::InputInt ( _L ( "Tempo BPM###PSGScopeTempoBpm" ),
                            &g_psg_export.tempo_bpm, 1, 10 ) )
    {
        if ( g_psg_export.tempo_bpm < 40 )  g_psg_export.tempo_bpm = 40;
        if ( g_psg_export.tempo_bpm > 300 ) g_psg_export.tempo_bpm = 300;
    }

    /* Clear - reset sample + events ringů a frame counteru. Bez
     * confirmation popupu (= data jen v RAM, reverzibilní z pohledu
     * uživatele = po stisku znovu nahrávat). */
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear###PSGScopeClearBtn" ) ) )
    {
        psg_audio_scope_clear_all ( );
    }
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s",
            _( "Reset scope sample buffer and recorded notes" ) );
    }
}

/**
 * @brief Vykreslí dolní status line okna PSG Audio Scope.
 *
 * Statusline je rozdělená do dvou řádek pod Separatorem:
 *   Řádek 1 (informační): Frame counter | Notes count | Status text
 *   Řádek 2 (kontrolní):  Log samples | Log events | Log writes | (s:N e:N w:N)
 *
 * Důvod splitu: jediná řádka s 6+ widgety se v užším okně zalamovala
 * nečitelně. Dva řádky drží informační a kontrolní obsah vizuálně
 * oddělené a stabilizují výšku footer rezervy (= 2x
 * GetFrameHeightWithSpacing).
 *
 * Šířky jednotlivých sekcí nejsou pevné; widgety se přidávají přes
 * SameLine. Pokud okno extrémně úzké, ImGui default zalamuje.
 */
static void psg_scope_render_statusline ( void )
{
    /* --- Řádek 1: informační ---------------------------------------- */

    /* Frame counter (= signál že emulátor běží + jak daleko jsme v záznamu). */
    ImGui::Text ( "%s: %llu",
                  _( "Frame" ),
                  (unsigned long long) g_psg_scope_frame_counter );

    /* Note count (= aktuální obsah events ringů, suma napříč chip/channel). */
    ImGui::SameLine ( );
    ImGui::Text ( "| %s: %u",
                  _( "Notes" ),
                  psg_audio_scope_total_notes ( ) );

    /* Status text - krátký indikátor že nějaký debug log je aktivní.
     * Detailní per-kanál countery (s:N e:N w:N) jsou v řádku 2 vedle
     * příslušných checkboxů. */
    bool any_log_active_r1 = ( g_psg_log.samples_fp != NULL )
                              || ( g_psg_log.events_fp != NULL )
                              || psg_write_log_is_enabled ( );
    if ( any_log_active_r1 )
    {
        ImGui::SameLine ( );
        ImGui::Text ( "| %s", _( "active" ) );
    }

    /* --- Řádek 2: kontrolní (Log toggles + counter) ------------------ */

    /* Debug log toggles - sample-level + event-level TSV výstup pro
     * offline cross-correlation analýzu. Default oboje vypnuto - opt-in.
     * Při disable se soubor uzavře a další enable otevře nový s novým
     * timestamp v názvu. */
    bool samples_prev = g_psg_log.samples_enabled;
    if ( ImGui::Checkbox ( _L ( "Log samples###PSGScopeLogSamples" ),
                            &g_psg_log.samples_enabled ) )
    {
        if ( !g_psg_log.samples_enabled && samples_prev )
            psg_log_close ( &g_psg_log.samples_fp, &g_psg_log.samples_row_count );
    }
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s",
            _( "Write per-frame sample log to TSV file" ) );
    }

    ImGui::SameLine ( );
    bool events_prev = g_psg_log.events_enabled;
    if ( ImGui::Checkbox ( _L ( "Log events###PSGScopeLogEvents" ),
                            &g_psg_log.events_enabled ) )
    {
        if ( !g_psg_log.events_enabled && events_prev )
            psg_log_close ( &g_psg_log.events_fp, &g_psg_log.events_row_count );
    }
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s",
            _( "Write state machine event log to TSV file" ) );
    }

    /* "Log writes" - autoritativní 1:1 záznam každého register zápisu
     * přímo z `psg_write_byte()` hooku. Na rozdíl od Log samples/events
     * (= polling z UI threadu, sub-tick eventy propadnou) tento log
     * zachytí každý byte přesně jak ho CPU zapsal na PSG data port.
     * Slouží pro offline rekonstrukci PSG audio bit-by-bit. */
    ImGui::SameLine ( );
    bool writes_prev = psg_write_log_is_enabled ( );
    bool writes_now  = writes_prev;
    if ( ImGui::Checkbox ( _L ( "Log writes###PSGScopeLogWrites" ),
                            &writes_now ) )
    {
        if ( writes_now && !writes_prev )
        {
            /* Enable: vygeneruj timestamp filename podle stávajícího
             * pattern (psg_writes_YYYYMMDD_HHMMSS.tsv). */
            char path[ 512 ];
            time_t now = time ( NULL );
            struct tm tm_buf;
#ifdef _WIN32
            localtime_s ( &tm_buf, &now );
#else
            localtime_r ( &now, &tm_buf );
#endif
            char ts[ 32 ];
            if ( strftime ( ts, sizeof ( ts ), "%Y%m%d_%H%M%S", &tm_buf ) == 0 )
                snprintf ( path, sizeof ( path ), "psg_writes.tsv" );
            else
                snprintf ( path, sizeof ( path ), "psg_writes_%s.tsv", ts );

            if ( !psg_write_log_enable ( path ) )
            {
                /* Open selhal - reset UI flag tak, aby checkbox zůstal off. */
                writes_now = false;
            }
        }
        else if ( !writes_now && writes_prev )
        {
            psg_write_log_disable ( );
        }
    }
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s",
            _( "Log every PSG register write to file (1:1 reconstruction)" ) );
    }

    /* Detailní counter (s:N e:N w:N) - per-log řádky zapsané od enable.
     * Zobrazí se pokud alespoň jeden z (samples/events/writes) aktivní. */
    bool any_log_active = ( g_psg_log.samples_fp != NULL )
                          || ( g_psg_log.events_fp != NULL )
                          || psg_write_log_is_enabled ( );
    if ( any_log_active )
    {
        ImGui::SameLine ( );
        ImGui::Text ( "(s:%u e:%u w:%u)",
                      g_psg_log.samples_row_count,
                      g_psg_log.events_row_count,
                      psg_write_log_row_count ( ) );
    }
}

/* ===================================================================== */
/* Hlavní entry point okna                                                */
/* ===================================================================== */

void imgui_psg_audio_scope_window(bool *p_open)
{
    /* Default initial size 720x720 (kompletní PSG chip + Piano roll
     * viditelné). FirstUseEver = jen při prvním zobrazení; pak ImGui
     * persistuje user resize do imgui.ini. */
    ImGui::SetNextWindowSize(ImVec2(720, 720), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("PSG Audio Scope###PSGAudioScope"), p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* Horní akční toolbar - Export MIDI / CSV / Tempo BPM / Clear.
     * Informační prvky (Frame, Notes, Log toggle, status text) jsou
     * v dolní statusline. */
    psg_scope_render_export_toolbar ( );

    /* IGFD dialog handlers (CSV + MIDI + error popup) - voláme každý
     * frame, samy se aktivují podle dialog_open flagů. */
    psg_scope_render_export_dialogs ( );

    ImGui::Separator ( );

    /* Reserva pro spodní statusline - DVĚ řádky widgetů + Separator.
     * Řádek 1 = info (Frame / Notes / status text), řádek 2 = log
     * toggle checkboxy + (s:N e:N w:N) counter. GetFrameHeightWithSpacing()
     * = výška jedné řádky widgetů s default spacingem; pro Separator
     * přičteme jeho odhadovanou výšku. */
    const float footer_h = 2.0f * ImGui::GetFrameHeightWithSpacing ( )
                           + ImGui::GetStyle ( ).ItemSpacing.y;

    /* Centrální obsah v child okně - hlavní views se renderují tady,
     * statusline níž. Child = ImGuiWindowFlags_None, height = -footer_h
     * (= zbývající prostor minus rezerva). */
    ImGui::BeginChild ( "##PSGScopeContent",
                        ImVec2 ( 0.0f, -footer_h ),
                        false,
                        ImGuiWindowFlags_None );

    bool is_stereo = g_psg_module.stereo;

    /* Vertikální škálování per-channel scope + envelope.
     *
     * Strategie: spočítej dostupný vertical space pro chip(s), rozděl mezi
     * channels (4 per chip), per kanál rozděl 60/30/10 (scope/envelope/gap).
     * Aplikuj min/max clamp aby UI zůstalo čitelné i v extrémních velikostech.
     *
     * Piano roll je collapsible - pokud je rozbalený, vyhradíme mu fixní
     * 200 px; pokud sbalený, scopes získají víc prostoru.
     *
     * Defaults (= clamp floor) odpovídají původním 40 + 26 px - okno
     * v default velikosti vypadá stejně jako před fixem; jen větší okno
     * scope/envelope proporčně roste. */
    unsigned chip_count_ui = is_stereo ? 2u : 1u;
    /* Heuristic: piano roll otevřený zhruba ~220 px (header + content).
     * Sbalený ~24 px (jen header). Nelze přímo zjistit collapsed stav před
     * voláním CollapsingHeader - použijeme persisted ImGui storage. */
    ImGuiID piano_id = ImGui::GetID("PSGScopePianoRollHdr");
    bool piano_open  = ImGui::GetStateStorage()->GetInt(piano_id, 1) != 0;
    float piano_reserve = piano_open ? 220.0f : 28.0f;
    /* Conservative subtract: chip headers (24 each), spacing, footer. */
    float chip_hdr_h    = 24.0f * (float)chip_count_ui;
    float reserved      = piano_reserve + chip_hdr_h + 40.0f /* margin */;
    float total_avail   = ImGui::GetContentRegionAvail().y - reserved;
    float per_channel   = total_avail / (float)( chip_count_ui * PSG_CHANNELS_COUNT );
    /* 60/30/10 split - scope:envelope:gap. */
    float dyn_row_h = per_channel * 0.60f;
    float dyn_env_h = per_channel * 0.30f;
    /* Clamp aby UI zůstalo čitelné. */
    if ( dyn_row_h < PSG_SCOPE_ROW_HEIGHT )      dyn_row_h = PSG_SCOPE_ROW_HEIGHT;
    if ( dyn_row_h > 200.0f )                    dyn_row_h = 200.0f;
    if ( dyn_env_h < PSG_SCOPE_ENVELOPE_HEIGHT ) dyn_env_h = PSG_SCOPE_ENVELOPE_HEIGHT;
    if ( dyn_env_h > 120.0f )                    dyn_env_h = 120.0f;

    if (is_stereo)
    {
        if (ImGui::CollapsingHeader(_L("PSG Left (chip 0)###PSGScopeLeftHdr"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            psg_audio_scope_render_chip(0u, "psg0", dyn_row_h, dyn_env_h);
            ImGui::Spacing();
        }
        if (ImGui::CollapsingHeader(_L("PSG Right (chip 1)###PSGScopeRightHdr"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            psg_audio_scope_render_chip(1u, "psg1", dyn_row_h, dyn_env_h);
            ImGui::Spacing();
        }
    }
    else
    {
        if (ImGui::CollapsingHeader(_L("PSG (chip 0)###PSGScopeMonoHdr"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            psg_audio_scope_render_chip(0u, "psg0", dyn_row_h, dyn_env_h);
        }
    }

    /* F4 piano roll - dolní timeline sekce. */
    if (ImGui::CollapsingHeader(_L("Piano roll###PSGScopePianoRollHdr"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        psg_audio_scope_render_piano_roll();
    }

    ImGui::EndChild ( );

    /* Spodní statusline - oddělená Separatorem. */
    ImGui::Separator ( );
    psg_scope_render_statusline ( );

    ImGui::End();
}

