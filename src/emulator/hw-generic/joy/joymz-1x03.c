/*
 * File:   joymz-1x03.c
 *
 * MZ-1X03 - Sharp analogovy joystick pro MZ-700 a MZ-1500.
 *
 * Pro popis principu, bit mappingu a casovani viz joymz-1x03.h.
 *
 * Compile-time scope:
 *   - Plne aktivni pro MZARCH == 700 a MZARCH == 1500.
 *   - Pro MZARCH == 800 se modul kompiluje jako stub - vsechny funkce jsou
 *     no-op a joymz_get_status_bits14 vraci 0x1E (= zadny joystick pripojen,
 *     vsechny 4 bity HIGH). Diky tomu hookpointy v hw-generic kodu (kdyby se
 *     nahodou linkly i do MZ-800 buildu) nemenu chovani 0xE008.
 *
 * Lazy compute architektura:
 *   joymz nezasahuje do gdg_event hot-path (= zadny per-VBLK callback).
 *   Veskery vypocet probiha az v joymz_get_status_bits14 pri MEMOP read
 *   na 0xE008. To setri ticky kdyz nikdo joystick necte (= typicky cely
 *   gameplay krome polling smyck).
 *
 *   In-frame pozice paprsku se cte z g_gdg.total_elapsed.ticks (= ticks
 *   od zacatku aktualniho frame, reset na zacatku kazdeho snimku v
 *   gdg_on_screen_done_event). VBLK falling edge pozice je odvozena
 *   z gdg event tabulky (= MZEVENT_GDG_HBLN_START na radku
 *   VIDEO_BEAM_CANVAS_LAST_ROW, sloupec VIDEO_BEAM_HBLN_FIRST_COLUMN).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPLv3 - viz licence projektu.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mzarch/mzarch_config.h"
#include "joymz-1x03.h"

#include "joy.h"
#include "hw-generic/gdg/gdgclk.h"

st_JOYMZ g_joymz;

#if (MZARCH == 700) || (MZARCH == 1500)
#include "hw-generic/gdg/gdg_state.h"
#include "hw-generic/gdg/video.h"

/* Pomocne offsety pro bit mapping na 0xE008. Bity 1-4 (mask 0x1E):
 *   stick 0: bit 1 = JA1, bit 2 = JA2
 *   stick 1: bit 3 = JB1, bit 4 = JB2
 */
#define JOYMZ_BIT_BASE(stick_idx) (1 + 2 * (stick_idx))

/**
 * Sebrat aktualni digitalni state z iface_joy / joy.h a prevest na
 * analogove osy + tlacitka.
 *
 * Iteruje pres sticky kde g_joy.dev[s].type != JOY_TYPE_NONE - timto
 * preskocime cteni z neaktivnich devices. connected flag v g_joymz.stick
 * se nastavi podle joy type (= jediny zdroj truth pro "stick existuje").
 *
 * Mapping (= vystup ze joy_read_byte je byte s active LOW bity UP/DOWN/
 * LEFT/RIGHT/TRIG1/TRIG2):
 *   - LEFT  -> X = 0,    RIGHT -> X = 255,  jinak X = 128
 *   - UP    -> Y = 0,    DOWN  -> Y = 255,  jinak Y = 128
 *   - TRIG1 -> SW1 stisknuto, TRIG2 -> SW2 stisknuto
 *
 * TODO: skutecny analog joystick (= SDL_GetGamepadAxis 0..32767 -> 0..255).
 */
static void joymz_refresh_from_iface_joy(void)
{
    for (int s = 0; s < JOYMZ_STICK_COUNT; s++) {
        if (g_joy.dev[s].type == JOY_TYPE_NONE) {
            g_joymz.stick[s].connected = false;
            continue;
        }
        g_joymz.stick[s].connected = true;

        uint8_t state = joy_read_byte((en_JOY_DEVID) s);

        /* Bit cleared = aktivni. */
        bool up    = !(state & (1u << JOY_STATEBIT_UP));
        bool down  = !(state & (1u << JOY_STATEBIT_DOWN));
        bool left  = !(state & (1u << JOY_STATEBIT_LEFT));
        bool right = !(state & (1u << JOY_STATEBIT_RIGHT));
        bool trig1 = !(state & (1u << JOY_STATEBIT_TRIG1));
        bool trig2 = !(state & (1u << JOY_STATEBIT_TRIG2));

        /* X: left=0, right=255, jinak 128. Pokud oba aktivni (nemelo by se
         * stat u 4-smerne paky), preferuj 128 = klid. */
        uint8_t x = JOYMZ_AXIS_CENTER;
        if (left && !right) x = 0;
        else if (right && !left) x = 255;

        uint8_t y = JOYMZ_AXIS_CENTER;
        if (up && !down) y = 0;
        else if (down && !up) y = 255;

        g_joymz.stick[s].pos[0] = x;
        g_joymz.stick[s].pos[1] = y;
        g_joymz.stick[s].btn[0] = trig1;
        g_joymz.stick[s].btn[1] = trig2;
    }
}

void joymz_init(void)
{
    memset(&g_joymz, 0, sizeof(g_joymz));
    for (int s = 0; s < JOYMZ_STICK_COUNT; s++) {
        g_joymz.stick[s].pos[0] = JOYMZ_AXIS_CENTER;
        g_joymz.stick[s].pos[1] = JOYMZ_AXIS_CENTER;
        g_joymz.stick[s].btn[0] = false;
        g_joymz.stick[s].btn[1] = false;
        g_joymz.stick[s].connected = false;
    }
    /* connected flag se odvodi za behu z g_joy.dev[s].type pri kazdem cteni
     * 0xE008 v joymz_refresh_from_iface_joy. */
}

void joymz_reset(void)
{
    for (int s = 0; s < JOYMZ_STICK_COUNT; s++) {
        g_joymz.stick[s].pos[0] = JOYMZ_AXIS_CENTER;
        g_joymz.stick[s].pos[1] = JOYMZ_AXIS_CENTER;
        g_joymz.stick[s].btn[0] = false;
        g_joymz.stick[s].btn[1] = false;
    }
}

uint8_t joymz_get_status_bits14(uint64_t total_ticks)
{
    (void) total_ticks; /* lazy implementace cte in-frame ticks z g_gdg */

    uint8_t mask = 0x1E; /* default: vsechny 4 bity HIGH (released / pulse done) */
    static const uint8_t MASK[2][2] = { { 0x02, 0x04 }, { 0x08, 0x10 } };

    /* Predpoklad: volajici jiz testoval JOYMZ_TEST_ANY_ACTIVE. Pokud sem
     * tedy dorazime, alespon jeden joystick device je type != JOY_TYPE_NONE. */

    /* Snapshot vstupu z iface_joy. Refreshuje connected flag dle joy.dev[].type
     * a primarne axis/btn fields ze ziveho stavu. */
    joymz_refresh_from_iface_joy();

    /* SIGNAL_GDG_VBLNK = g_gdg.vbln. Konvence:
     *   VBLN_OFF    (= 1) -> fyzicky VBLK = HIGH (display)
     *   VBLN_ACTIVE (= 0) -> fyzicky VBLK = LOW  (vblank period)
     */
    bool in_vblank = (SIGNAL_GDG_VBLNK == VBLN_ACTIVE);

    if (!in_vblank) {
        /* Display mode -> bity reflektuji button states (active LOW pri
         * stisku). */
        for (int s = 0; s < JOYMZ_STICK_COUNT; s++) {
            if (!g_joymz.stick[s].connected) continue;
            for (int btn = 0; btn < 2; btn++) {
                if (g_joymz.stick[s].btn[btn]) {
                    mask &= ~MASK[s][btn];
                }
            }
        }
        return mask;
    }

    /* Vblank mode -> PWM pulze pro osy.
     *
     * VBLK falling edge pozice in-frame: gdg_event nastavuje vbln=VBLN_ACTIVE
     * v MZEVENT_GDG_HBLN_START handleru kdyz beam_row == VIDEO_BEAM_CANVAS_LAST_ROW.
     * Event ticks = beam_row * VIDEO_SCREEN_WIDTH + event_column. Sloupec
     * pro HBLN_START je VIDEO_BEAM_HBLN_FIRST_COLUMN.
     */
    const uint64_t vblk_fall_in_frame =
        (uint64_t) VIDEO_BEAM_CANVAS_LAST_ROW * (uint64_t) VIDEO_SCREEN_WIDTH
        + (uint64_t) VIDEO_BEAM_HBLN_FIRST_COLUMN;

    uint64_t in_frame_ticks = (uint64_t) g_gdg.total_elapsed.ticks;

    /* Defenzivne: pokud bychom z nejakeho duvodu byli v vblank ale paprsek
     * jeste pred VBLK fall (= bug v GDG), vrat default mask (vse HIGH).
     * Pokud tato situace fakticky nastane, je to znamka ze konstanta
     * vblk_fall_in_frame neodpovida realite. */
    if (in_frame_ticks < vblk_fall_in_frame) {
        return mask;
    }
    uint64_t ticks_since_vblk_fall = in_frame_ticks - vblk_fall_in_frame;

    for (int s = 0; s < JOYMZ_STICK_COUNT; s++) {
        if (!g_joymz.stick[s].connected) continue;
        for (int axis = 0; axis < 2; axis++) {
            uint8_t pos = g_joymz.stick[s].pos[axis];
            /* T-states pulse duration: 68 + pos * 28. Prevod na GDG ticks
             * pres GDGCLK2CPU_DIVIDER (= GDG ticks per CPU T-state). */
            uint64_t pulse_dur_ticks =
                (68u + (uint64_t) pos * 28u) * (uint64_t) GDGCLK2CPU_DIVIDER;
            if (ticks_since_vblk_fall < pulse_dur_ticks) {
                mask &= ~MASK[s][axis]; /* pulz LOW jeste trva */
            }
        }
    }

    return mask;
}

void joymz_set_axis(int stick_idx, int axis, uint8_t value)
{
    if (stick_idx < 0 || stick_idx >= JOYMZ_STICK_COUNT) return;
    if (axis < 0 || axis > 1) return;
    g_joymz.stick[stick_idx].pos[axis] = value;
}

void joymz_set_button(int stick_idx, int button, bool pressed)
{
    if (stick_idx < 0 || stick_idx >= JOYMZ_STICK_COUNT) return;
    if (button < 0 || button > 1) return;
    g_joymz.stick[stick_idx].btn[button] = pressed;
}

void joymz_set_connected(int stick_idx, bool connected)
{
    if (stick_idx < 0 || stick_idx >= JOYMZ_STICK_COUNT) return;
    g_joymz.stick[stick_idx].connected = connected;
}

#else  /* MZARCH == 800 */

/* Stub implementace: MZ-1X03 nepatri k MZ-800 (= MZ-800 ma vlastni
 * digital JOY na 0xF0/0xF1 IORQ, ne na 0xE008). 0xE008 status read
 * vraci 0x1E (= bity 1-4 vsechny HIGH). */

void joymz_init(void)
{
    memset(&g_joymz, 0, sizeof(g_joymz));
}

void joymz_reset(void)
{
    /* no-op pro MZ-800. */
}

uint8_t joymz_get_status_bits14(uint64_t total_ticks)
{
    (void) total_ticks;
    /* Bity 1-4 vsechny HIGH (= zadny joystick). */
    return 0x1E;
}

void joymz_set_axis(int stick_idx, int axis, uint8_t value)
{
    (void) stick_idx; (void) axis; (void) value;
}

void joymz_set_button(int stick_idx, int button, bool pressed)
{
    (void) stick_idx; (void) button; (void) pressed;
}

void joymz_set_connected(int stick_idx, bool connected)
{
    (void) stick_idx; (void) connected;
}

#endif /* MZARCH == 700 || MZARCH == 1500 */
