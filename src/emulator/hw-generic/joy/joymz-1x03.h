/*
 * File:   joymz-1x03.h
 *
 * MZ-1X03 - Sharp analogovy joystick pro MZ-700 a MZ-1500.
 *
 * Stav se čte jako bity 1-4 na mem-mapped 0xE008. Kazdy MZ-700
 * a MZ-1500 ma dva konektory (left + right side), proto 2 sticky soucasne.
 *
 * Princip merit pozice osy:
 *   Kazda osa ma potenciometr -> RC obvod -> monostable multivibrator.
 *   Trigger pulzu = VBLK falling edge (vstup do vblank). Behem pulsu vystup
 *   ide LOW po dobu umernou pozici stick. Po vybiti kondenzatoru pin prejde
 *   na HIGH. CPU pollovaci smycka pocita iterace dokud bit zustane LOW.
 *
 * Bit mapping na 0xE008 status byte:
 *   bit | nazev | display (VBLK=H) | vblank (VBLK=L)
 *   ----+-------+------------------+----------------
 *    1  | JA1   | SW1 stick 1      | X osa stick 1
 *    2  | JA2   | SW2 stick 1      | Y osa stick 1
 *    3  | JB1   | SW1 stick 2      | X osa stick 2
 *    4  | JB2   | SW2 stick 2      | Y osa stick 2
 *
 *   Active LOW: bit cleared = stisknuto (display) nebo pulz LOW jeste trva
 *   (vblank).
 *
 * Casovani:
 *   ROM polling smycka (INC DE; BIT n,(HL); JP Z) ~ 28 T-states/iter,
 *   plus initial delay ~ 17 iter (~ 68 T-states). Pro position 0..255 doba
 *   LOW pulsu = position * 28 + 68 T-states.
 *
 * VBLK signal:
 *   Per MZ-1X03 manualu je VBLK na PC7 obvodu 8255 PPI (mem-mapped 0xE002).
 *   V GDG state se aktualni level cte primo z g_gdg.vbln (= SIGNAL_GDG_VBLNK).
 *   Konvence: g_gdg.vbln == VBLN_ACTIVE (= 0) odpovida fyzicky VBLK = LOW
 *   (= signal logicky 1 = vblank period). g_gdg.vbln == VBLN_OFF (= 1)
 *   odpovida fyzicky VBLK = HIGH (= display).
 *
 * Lazy compute:
 *   Stav joystickovych bitu se nepocita preventivne v gdg_event hot path,
 *   ale az ON DEMAND pri kazdem cteni 0xE008 (= joymz_get_status_bits14).
 *   To setri ticks v gdg_event smycce kdyz nikdo joystick necte. PWM pulse
 *   pozice se odvozuje od (in-frame ticks) - (VBLK fall in-frame ticks).
 *
 * Vstupni mapping (MVP):
 *   Pouzivame digitalni state z iface_joy / joy_read_byte (UP/DOWN/LEFT/
 *   RIGHT/TRIG1/TRIG2 active LOW, 0xff = nic). Mapujeme na analog axis:
 *     LEFT -> X = 0,  RIGHT -> X = 255,  zadny smer -> X = 128
 *     UP   -> Y = 0,  DOWN  -> Y = 255,  zadny smer -> Y = 128
 *     TRIG1 -> SW1, TRIG2 -> SW2
 *
 * ----------------------------- License -------------------------------------
 *
 * GPLv3 - viz licence projektu.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef JOYMZ_1X03_H
#define JOYMZ_1X03_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

#include "joy.h"                    /* g_joy, JOY_TYPE_NONE, en_JOY_DEVID */

#ifdef __cplusplus
extern "C"
{
#endif

/** Stred analogove osy (cca odpovida pozici stick v klidu). */
#define JOYMZ_AXIS_CENTER   128

/** Pocet sticku (= 2 konektory MZ-700/MZ-1500). */
#define JOYMZ_STICK_COUNT   2

/**
 * Fast path test: vyhodnoti se jako nenulova hodnota pokud je alespon jeden
 * joystick device aktivni v iface_joy (= type != JOY_TYPE_NONE).
 *
 * Pouzivat na call site (= gdg_read_dmd_status_memop) PRED volanim
 * joymz_get_status_bits14() pro short-circuit - kdyz uzivatel zadny joystick
 * nemа nakonfigurovany, vyhneme se i pouhému function call overheadu v hot
 * path 0xE008 reading.
 *
 * Pouziti:
 *   if (JOYMZ_TEST_ANY_ACTIVE) {
 *       retval = (retval & ~0x1E)
 *              | (joymz_get_status_bits14(gdg_get_total_ticks()) & 0x1E);
 *   }
 *
 */
#define JOYMZ_TEST_ANY_ACTIVE \
    ((g_joy.dev[ JOY_DEVID_0 ].type != JOY_TYPE_NONE) \
     || (g_joy.dev[ JOY_DEVID_1 ].type != JOY_TYPE_NONE))

/**
 * Stav jednoho sticku MZ-1X03.
 *
 * Invarianty:
 *   - pos[axis] je 0..255 (0 = doleva/nahoru, 255 = doprava/dolu)
 *   - btn[i] true = aktualne stisknuto
 *   - connected true = stick je v configu pripojeny; pri false jeho
 *     bity 0xE008 zustavaji HIGH bez ohledu na pos/btn
 *
 * Lifetime: globalni ve g_joymz, prepisuje se z emulator threadu.
 */
typedef struct st_JOYMZ_STICK
{
    uint8_t  pos[2];               /**< [0] = X, [1] = Y, range 0..255. */
    bool     btn[2];               /**< [0] = SW1, [1] = SW2 (true = stisknuto). */
    bool     connected;            /**< true = stick je v configu pripojeny. */
} st_JOYMZ_STICK;

/**
 * Globalni stav MZ-1X03 subsystemu.
 *
 * Pristup z emulator threadu (= jednovlaknovy). UI thread by se mel branit
 * primemu pristupu, pripadne pres iface_joy / joymz_set_* helpery.
 *
 * Po refactoru na lazy compute neobsahuje zadny cache VBLK levelu ani
 * pulse_end_ticks - vse se pocita on-demand v joymz_get_status_bits14
 * z aktualniho GDG state.
 */
typedef struct st_JOYMZ
{
    st_JOYMZ_STICK stick[JOYMZ_STICK_COUNT];
} st_JOYMZ;

extern st_JOYMZ g_joymz;

/**
 * Inicializace MZ-1X03 subsystemu.
 *
 * Volat jednou pri startu emulatoru (vedle joy_init). Nastavi vsechny sticky
 * do klidoveho stavu (axis = 128, buttons released). Connected stav je per
 * MVP odvozen z iface_joy konfigurace - viz joymz_refresh_connected_state.
 *
 * Side effects: pisuje do g_joymz.
 */
extern void joymz_init(void);

/**
 * Reset state do klidoveho stavu (osy = 128, buttons released).
 * Vola se z mzarch_main_reset behem CPU resetu.
 *
 * Side effects: pisuje do g_joymz (kromě connected flagu).
 */
extern void joymz_reset(void);

/**
 * Vrati bity 1-4 (mask 0x1E) pro 0xE008 status registr.
 *
 * Lazy compute - vola se on-demand pri kazdem cteni 0xE008. Nevyzaduje
 * zadny preceding hook v gdg_event hot path. Vystup zavisi na aktualnim
 * VBLK levelu (= SIGNAL_GDG_VBLNK):
 *   - VBLK = HIGH (display, g_gdg.vbln == VBLN_OFF): bity 1-4 odpovidaji
 *     buttonum (active LOW = bit cleared = stisknuto). Stick 1: bit 1 = SW1,
 *     bit 2 = SW2. Stick 2: bit 3 = SW1, bit 4 = SW2.
 *   - VBLK = LOW (vblank, g_gdg.vbln == VBLN_ACTIVE): bity 1-4 jsou PWM
 *     pulze pro osy. Pulse trva (68 + pos * 28) T-states od VBLK falling
 *     edge. Bit cleared dokud pulz LOW trva, pote bit set. Stick 1: bit 1
 *     = X, bit 2 = Y. Stick 2: bit 3 = X, bit 4 = Y.
 *
 *   Pokud stick neni connected, jeho 2 bity zustanou HIGH (= released /
 *   pulse done) bez ohledu na cas a tlacitka.
 *
 *   Behem vblank funkce dale sebere digital state z iface_joy (joy_read_byte)
 *   a prevede ho na pos/btn pole. Tim se osetri ze pri prvnim cteni v dane
 *   vblank periode mame aktualni snapshot vstupu.
 *
 * Vraci pouze bity 1-4. Bity 0, 5-7 jsou 0 (volajici je doplni).
 *
 * Volajici typicky (s fast path testem PRED volanim):
 *   if (JOYMZ_TEST_ANY_ACTIVE) {
 *       retval = (retval & ~0x1E)
 *              | (joymz_get_status_bits14(gdg_get_total_ticks()) & 0x1E);
 *   }
 *
 * Mimo MZ-700/MZ-1500 (= MZ-800, stub implementace) tato funkce vraci
 * 0x1E (= zadny joystick pripojen, vsechny bity HIGH) bez side effectu.
 *
 * @param total_ticks  Aktualni cumulative GDG total ticks (ponechano kvuli
 *                     ABI kompatibilite / volajicim - lazy implementace
 *                     tento parametr nepouziva, in-frame pozici cte primo
 *                     z g_gdg.total_elapsed.ticks).
 * @return Maska bitu 1-4 (rozsah 0x00-0x1E).
 */
extern uint8_t joymz_get_status_bits14(uint64_t total_ticks);

/**
 * Nastaveni analogove pozice osy.
 *
 * @param stick_idx  0 nebo 1 (0 = stick A, 1 = stick B).
 * @param axis       0 = X, 1 = Y.
 * @param value      0..255 (128 = stred).
 *
 * Side effects: pisuje do g_joymz.stick[stick_idx].pos[axis].
 */
extern void joymz_set_axis(int stick_idx, int axis, uint8_t value);

/**
 * Nastaveni stavu tlacitka.
 *
 * @param stick_idx  0 nebo 1.
 * @param button     0 = SW1, 1 = SW2.
 * @param pressed    true = stisknuto, false = uvolneno.
 *
 * Side effects: pisuje do g_joymz.stick[stick_idx].btn[button].
 */
extern void joymz_set_button(int stick_idx, int button, bool pressed);

/**
 * Nastaveni connected flagu sticku.
 *
 * Pokud se stick odpoji za behu, jeho bity status registru zustanou HIGH.
 *
 * @param stick_idx  0 nebo 1.
 * @param connected  true = pripojeny, false = nepripojeny.
 */
extern void joymz_set_connected(int stick_idx, bool connected);

#ifdef __cplusplus
}
#endif

#endif /* JOYMZ_1X03_H */
